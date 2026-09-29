#!/usr/bin/env python3
"""
find_dispatcher.py - offline, zero-live-risk search for the inbound message-DISPATCH
function (docs/investigation/CONNECTION_DECIPHER_PREP.md, "read the client's own plaintext"
thread, point 2). Does not touch the cipher at all: a dispatcher must switch on the
DECRYPTED message-type value to route to a handler, so its input is necessarily plaintext
regardless of whether the encrypt/decrypt function itself is ever located. That switch is a
distinctive static signature -- disassemble the already-decrypted image and look for a
function containing `cmp`/immediate comparisons against many of msg_types.h's known IDs
clustered together (a real dispatcher references MANY of them in one place; a stray match
against a single ID is far more likely to be an unrelated constant).

Ladder position: this is the offline/static rung, run against an existing memory-image dump
(the same one already used for the stock-Blowfish-table search) -- no injection, no live
game needed. A live Detour hook on whatever candidate this finds is a SEPARATE, later step
that needs its own go-ahead, mirroring how HkSendMsgReal was only added once 0x1DD450/
0x1C70C0 were independently confirmed.

Method (heuristic, not exhaustive): linear-sweep disassembly with capstone starting at every
byte offset in the target section where the previous disassembly attempt failed (standard
practice for a section that may mix code with inline data/padding -- imperfect, but this is
exploratory triage, not a verified decompilation). For every `cmp reg, imm` (and `mov reg,
imm` immediately compared shortly after) whose immediate exactly equals one of msg_types.h's
known IDs, record (address, id). Cluster hits within CLUSTER_WINDOW bytes of each other and
rank clusters by how many DISTINCT ids they reference.

Usage:
    python tools/find_dispatcher.py "C:\\Users\\Public\\coclassic_capture\\session_20260921_073000\\cp02_idle_twincity\\image.bin"
    python tools/find_dispatcher.py <image.bin> --top 15 --cluster-window 0x1000
"""
import argparse, pathlib, re, sys
from collections import defaultdict

try:
    import capstone
except ImportError:
    sys.exit("capstone not installed -- pip install capstone")

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
MSG_TYPES_H = REPO_ROOT / "src" / "msg_types.h"


def load_known_ids(path: pathlib.Path) -> dict:
    """Parses `case NNNN: return "Name";` lines straight out of msg_types.h -- the same
    table already trusted everywhere else in this project (overlay, RelayLogger)."""
    ids = {}
    text = path.read_text(encoding="utf-8", errors="replace")
    for m in re.finditer(r'case\s+(\d+)\s*:\s*return\s*"([^"]+)"', text):
        ids[int(m.group(1))] = m.group(2)
    return ids


def sections_from_csv(csv_path: pathlib.Path):
    """imgdump.cpp's own sections.csv: name,rva,virtual_size,raw_size,characteristics."""
    out = []
    if not csv_path.exists():
        return out
    for line in csv_path.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        parts = line.split(",")
        if len(parts) < 4:
            continue
        name = parts[0].strip() or "(unnamed)"
        try:
            rva = int(parts[1], 0)
            vsize = int(parts[2], 0)
        except ValueError:
            continue
        out.append((name, rva, vsize))
    return out


def scan_section(data: bytes, base_rva: int, size: int, known_ids: dict, md: "capstone.Cs"):
    """Linear-sweep disassembly with resync-on-failure. Returns list of (rva, id) hits."""
    hits = []
    end = min(base_rva + size, len(data))
    offset = base_rva
    known_set = set(known_ids)
    while offset < end:
        chunk = data[offset:min(offset + 16, end)]  # max x86-64 insn length is 15 bytes
        insns = list(md.disasm(chunk, offset, count=1))
        if not insns:
            offset += 1  # resync: not a valid instruction here, try the next byte
            continue
        insn = insns[0]
        if insn.mnemonic in ("cmp", "je", "jne", "jz", "jnz") and insn.operands:
            for op in insn.operands:
                if op.type == capstone.x86.X86_OP_IMM and op.imm in known_set:
                    hits.append((insn.address, op.imm))
        offset += insn.size
    return hits


def cluster_hits(hits, window: int):
    """Groups hits by proximity (greedy: extend a cluster while the next hit is within
    `window` bytes of the cluster's last hit), then ranks by distinct-id count."""
    hits = sorted(hits)
    clusters = []
    cur = []
    for addr, mid in hits:
        if cur and addr - cur[-1][0] > window:
            clusters.append(cur)
            cur = []
        cur.append((addr, mid))
    if cur:
        clusters.append(cur)
    ranked = []
    for c in clusters:
        ids = sorted(set(mid for _, mid in c))
        if len(ids) < 2:
            continue  # a single distinct id in a window is not a dispatcher signature
        ranked.append((len(ids), c[0][0], c[-1][0], ids, c))
    ranked.sort(key=lambda r: -r[0])
    return ranked


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=pathlib.Path, help="imgdump.cpp image.bin (RVA == file offset)")
    ap.add_argument("--sections-csv", type=pathlib.Path, default=None,
                     help="sections.csv from the same checkpoint dir (default: alongside image.bin)")
    ap.add_argument("--only-section", default=None, help="scan only the section whose name/index matches this")
    ap.add_argument("--cluster-window", default="0x2000", help="max byte span for one cluster (hex ok)")
    ap.add_argument("--top", type=int, default=10)
    a = ap.parse_args()

    known_ids = load_known_ids(MSG_TYPES_H)
    if not known_ids:
        sys.exit(f"no `case N: return \"Name\";` entries parsed from {MSG_TYPES_H} -- did its format change?")
    print(f"loaded {len(known_ids)} known message-type ids from {MSG_TYPES_H.relative_to(REPO_ROOT)}")

    sections_csv = a.sections_csv or (a.image.parent / "sections.csv")
    sections = sections_from_csv(sections_csv)
    if not sections:
        sys.exit(f"no sections.csv found at {sections_csv} -- pass --sections-csv explicitly")

    data = a.image.read_bytes()
    print(f"loaded image: {a.image} ({len(data)} bytes)")

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    cluster_window = int(a.cluster_window, 0)
    all_hits = []
    for i, (name, rva, vsize) in enumerate(sections):
        # Executable, non-Themida-VM sections only: .themida is the packer's own VM
        # interpreter, not real game code, and would just add noise/time here.
        if "themida" in name.lower():
            continue
        if a.only_section and a.only_section not in (name, str(i)):
            continue
        print(f"scanning section [{i}] '{name}' rva=0x{rva:X} size=0x{vsize:X} ...")
        hits = scan_section(data, rva, vsize, known_ids, md)
        print(f"  {len(hits)} raw hits ({len(set(h[1] for h in hits))} distinct ids)")
        all_hits.extend(hits)

    print(f"\n{len(all_hits)} total raw hits across all scanned sections. Clustering (window=0x{cluster_window:X})...")
    ranked = cluster_hits(all_hits, cluster_window)

    print(f"\n=== top {min(a.top, len(ranked))} candidate clusters (by distinct id count) ===")
    for distinct, lo, hi, ids, members in ranked[:a.top]:
        names = [f"{i}={known_ids.get(i, '?')}" for i in ids]
        print(f"\n0x{lo:X}-0x{hi:X}  ({distinct} distinct ids, {len(members)} hits)")
        print("  ids: " + ", ".join(names))

    if not ranked:
        print("\nno cluster referenced 2+ distinct known ids -- either the dispatcher isn't a simple "
              "cmp-chain (could be a jump table indexed by type instead, not yet searched for), it's "
              "in a section not scanned here, or the linear-sweep disassembly is desyncing badly enough "
              "to miss it. Worth trying --only-section on '.boot' next if the main section is clean here.")


if __name__ == "__main__":
    main()
