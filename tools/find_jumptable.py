#!/usr/bin/env python3
"""
find_jumptable.py - offline search for the inbound message-dispatch JUMP TABLE, the stronger
structural signature over find_dispatcher.py's individual-cmp approach (which produced a
checked negative -- see docs/investigation/CONNECTION_DECIPHER_PREP.md). A 241+-way dispatch
is far more likely compiled as a bounds-check + indexed indirect jump than a flat cmp chain.

Two independent techniques, either one locates the dispatcher, both together are conclusive:

  A) INSTRUCTION PATTERN: `movzx reg, word ptr [base+OFFSET]` (load the u16 type field) followed
     within a short window by a bounds check (`cmp`/`sub` against a small immediate) and an
     indirect jump/call through a SIB memory operand with scale 4 or 8 (`jmp/call [table +
     idx*8]`) -- the classic MSVC compiled-switch shape.
  B) TABLE SHAPE: independently, scan every 8-byte-aligned qword in the data-ish sections for a
     long contiguous run where every value, interpreted as an absolute VA, falls inside this
     module's own code range -- a jump table is exactly that: a dense array of code pointers.

--type-offset lets the type-field offset be corrected once a real plaintext capture exists
(point 1 of the decipher thread) instead of assuming +2 -- default is 2, the confirmed
OUTBOUND header offset, used here as a starting hypothesis for inbound.

Usage:
    python tools/find_jumptable.py <image.bin> [--type-offset 2] [--min-run 40]
"""
import argparse, pathlib, sys

try:
    import capstone
    from capstone import x86
except ImportError:
    sys.exit("capstone not installed -- pip install capstone")

MODULE_BASE = 0x140000000  # confirmed v1078 load base (imgdump.cpp meta.json, this dump)


def sections_from_csv(csv_path: pathlib.Path):
    out = []
    for line in csv_path.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        parts = line.split(",")
        if len(parts) < 5:
            continue
        name = parts[0].strip() or "(unnamed)"
        try:
            rva, vsize = int(parts[1], 0), int(parts[2], 0)
            chars = int(parts[4], 0)
        except ValueError:
            continue
        out.append((name, rva, vsize, chars))
    return out


def find_movzx_type_load(data: bytes, sections, type_offset: int, md):
    """Pattern A: movzx reg32, word ptr [reg+type_offset], then within 64 bytes forward, an
    indirect jmp/call through a scale-4 or scale-8 SIB memory operand."""
    hits = []
    for name, rva, vsize, chars in sections:
        if "themida" not in name.lower() and not (chars & 0x20):  # executable sections only
            continue
        if "themida" in name.lower():
            continue
        end = min(rva + vsize, len(data))
        offset = rva
        while offset < end:
            chunk = data[offset:min(offset + 16, end)]
            insns = list(md.disasm(chunk, offset, count=1))
            if not insns:
                offset += 1
                continue
            insn = insns[0]
            is_movzx_word = insn.mnemonic == "movzx" and ",  word ptr" in f", {insn.op_str}".replace(" ", "  ") or (
                insn.mnemonic == "movzx" and "word ptr" in insn.op_str)
            if is_movzx_word and insn.operands and len(insn.operands) == 2:
                mem = insn.operands[1]
                if mem.type == x86.X86_OP_MEM and mem.mem.disp == type_offset:
                    # look forward up to 64 bytes for an indirect jmp/call via scaled SIB
                    fwd_off = offset + insn.size
                    fwd_end = min(fwd_off + 64, end)
                    while fwd_off < fwd_end:
                        fchunk = data[fwd_off:min(fwd_off + 16, fwd_end)]
                        finsns = list(md.disasm(fchunk, fwd_off, count=1))
                        if not finsns:
                            fwd_off += 1
                            continue
                        fi = finsns[0]
                        if fi.mnemonic in ("jmp", "call") and fi.operands:
                            op = fi.operands[0]
                            if op.type == x86.X86_OP_MEM and op.mem.scale in (4, 8):
                                hits.append((insn.address, fi.address, fi.mnemonic, op.mem.scale))
                        fwd_off += fi.size
            offset += insn.size
    return hits


def find_pointer_table(data: bytes, sections, min_run: int, code_lo: int, code_hi: int):
    """Pattern B: a contiguous run of 8-byte-aligned qwords all landing in [code_lo, code_hi)."""
    runs = []
    for name, rva, vsize, chars in sections:
        if chars & 0x20:  # skip executable sections -- a table lives in data, not code
            continue
        end = min(rva + vsize, len(data))
        run_start = None
        run_len = 0
        off = rva - (rva % 8)
        while off + 8 <= end:
            qword = int.from_bytes(data[off:off + 8], "little")
            in_range = code_lo <= qword < code_hi
            if in_range:
                if run_start is None:
                    run_start = off
                run_len += 1
            else:
                if run_start is not None and run_len >= min_run:
                    runs.append((name, run_start, run_len))
                run_start, run_len = None, 0
            off += 8
        if run_start is not None and run_len >= min_run:
            runs.append((name, run_start, run_len))
    return runs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=pathlib.Path)
    ap.add_argument("--sections-csv", type=pathlib.Path, default=None)
    ap.add_argument("--type-offset", type=int, default=2,
                     help="byte offset of the u16 type field within a message (default 2, the "
                          "confirmed OUTBOUND header offset -- correct this once real inbound "
                          "plaintext exists)")
    ap.add_argument("--min-run", type=int, default=40,
                     help="minimum consecutive code-pointer-shaped qwords to call it a candidate table")
    a = ap.parse_args()

    sections_csv = a.sections_csv or (a.image.parent / "sections.csv")
    sections = sections_from_csv(sections_csv)
    if not sections:
        sys.exit(f"no sections.csv at {sections_csv}")

    data = a.image.read_bytes()
    image_size = len(data)
    code_lo, code_hi = MODULE_BASE, MODULE_BASE + image_size
    print(f"module base=0x{MODULE_BASE:X} image size=0x{image_size:X} -> code pointer range "
          f"[0x{code_lo:X}, 0x{code_hi:X})")

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    print(f"\n=== Pattern A: movzx [x+{a.type_offset}] -> indirect jmp/call via scaled SIB ===")
    a_hits = find_movzx_type_load(data, sections, a.type_offset, md)
    print(f"{len(a_hits)} candidate(s)")
    for load_addr, jmp_addr, mnem, scale in a_hits[:30]:
        print(f"  type-load @0x{load_addr:X}  ->  {mnem} [.. * {scale}] @0x{jmp_addr:X}  (span {jmp_addr - load_addr} bytes)")

    print(f"\n=== Pattern B: contiguous code-pointer runs (>= {a.min_run} qwords) in data sections ===")
    b_hits = find_pointer_table(data, sections, a.min_run, code_lo, code_hi)
    print(f"{len(b_hits)} candidate run(s)")
    for name, off, length in sorted(b_hits, key=lambda r: -r[2])[:20]:
        print(f"  section '{name}' @0x{off:X}  length={length} qwords ({length * 8} bytes) "
              f"-- entries would index type values 0..{length - 1} if used directly")

    if not a_hits and not b_hits:
        print("\nBoth patterns came up empty. Next offline avenue: the recv/WSARecv call-graph "
              "trace (tools/trace_recv_callgraph.py).")


if __name__ == "__main__":
    main()
