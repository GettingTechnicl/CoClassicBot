#!/usr/bin/env python3
"""
find_heap_dispatch_table.py - offline search of an imgdump.cpp HEAP snapshot for a dynamically
built message-dispatch table (docs/investigation/CONNECTION_DECIPHER_PREP.md). Follows from
find_jumptable.py's static image scan coming up genuinely empty (both the movzx-then-indexed-
jump instruction pattern AND a static pointer-table-in-the-image search, cross-referenced by
code XREF, found nothing real) -- a modern engine plausibly registers message handlers into a
runtime container (std::unordered_map<uint16_t, Handler>, a std::vector<std::pair<id, fn>>,
etc.) at startup instead of compiling a switch, which would only ever exist in HEAP memory, not
the static image this project's earlier scans (Blowfish tables, jump tables) both targeted.

Two candidate shapes, both scanned for:
  1. A dense array of (uint16, padding, function-pointer) or (uint32-ish id, function-pointer)
     entries -- the natural layout for a std::vector<std::pair<uint16_t, FnPtr>>-style table.
  2. A plain run of code-pointer-shaped qwords (same signature as the image scan, just pointed
     at heap regions instead) -- covers a std::vector<FnPtr> indexed directly by a normalized id.

Uses heap_regions.csv (addr,size,protect,pack_offset,bytes_read) to map each scanned offset back
to its real runtime virtual address, since heap.bin packs many separate allocations together.

Usage:
    python tools/find_heap_dispatch_table.py <heap.bin> [--min-run 20] [--known-ids-min 5]
"""
import argparse, csv, pathlib, re, sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
MSG_TYPES_H = REPO_ROOT / "src" / "msg_types.h"
MODULE_BASE = 0x140000000
MODULE_SIZE = 0x2A26000


def load_known_ids():
    text = MSG_TYPES_H.read_text(encoding="utf-8", errors="replace")
    return {int(m.group(1)) for m in re.finditer(r'case\s+(\d+)\s*:', text)}


def load_regions(csv_path: pathlib.Path):
    regions = []
    with open(csv_path, newline="", encoding="utf-8", errors="replace") as f:
        for row in csv.DictReader(f):
            regions.append((int(row["addr"], 0), int(row["size"], 0), int(row["pack_offset"], 0), int(row["bytes_read"], 0)))
    return regions


def is_code_ptr(qword: int) -> bool:
    return MODULE_BASE <= qword < MODULE_BASE + MODULE_SIZE


def scan_plain_pointer_runs(data: bytes, regions, min_run: int):
    hits = []
    for addr, size, pack_off, bytes_read in regions:
        region = data[pack_off:pack_off + bytes_read]
        run_start_i = None
        run_len = 0
        for i in range(0, len(region) - 7, 8):
            qword = int.from_bytes(region[i:i + 8], "little")
            if is_code_ptr(qword):
                if run_start_i is None:
                    run_start_i = i
                run_len += 1
            else:
                if run_start_i is not None and run_len >= min_run:
                    hits.append((addr + run_start_i, run_len))
                run_start_i, run_len = None, 0
        if run_start_i is not None and run_len >= min_run:
            hits.append((addr + run_start_i, run_len))
    return hits


def scan_id_fnptr_pairs(data: bytes, regions, known_ids: set, min_matches: int):
    """Looks for 16-byte entries [uint16 id][6 bytes padding/other][8-byte code pointer] --
    the natural struct layout for std::pair<uint16_t, FnPtr> with alignment padding -- where at
    least `min_matches` CONSECUTIVE entries have an id in the known set and a code-pointer-shaped
    second half. This is a much more specific signature than a plain pointer run: it requires the
    known msg-type IDs to appear in a position consistent with being array keys, not just anywhere.
    """
    hits = []
    for addr, size, pack_off, bytes_read in regions:
        region = data[pack_off:pack_off + bytes_read]
        i = 0
        run_start_i = None
        run_len = 0
        run_ids = []
        while i + 16 <= len(region):
            candidate_id = int.from_bytes(region[i:i + 2], "little")
            ptr = int.from_bytes(region[i + 8:i + 16], "little")
            if candidate_id in known_ids and is_code_ptr(ptr):
                if run_start_i is None:
                    run_start_i = i
                run_len += 1
                run_ids.append(candidate_id)
                i += 16
                continue
            if run_start_i is not None and run_len >= min_matches:
                hits.append((addr + run_start_i, run_len, list(run_ids)))
            run_start_i, run_len, run_ids = None, 0, []
            i += 8  # resync at half-stride, entries might not be 16-byte aligned to our start guess
        if run_start_i is not None and run_len >= min_matches:
            hits.append((addr + run_start_i, run_len, list(run_ids)))
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("heap", type=pathlib.Path)
    ap.add_argument("--regions-csv", type=pathlib.Path, default=None)
    ap.add_argument("--min-run", type=int, default=20, help="min consecutive code-pointer qwords for the plain-run scan")
    ap.add_argument("--known-ids-min", type=int, default=4, help="min consecutive id/fnptr entries for the pair scan")
    a = ap.parse_args()

    regions_csv = a.regions_csv or (a.heap.parent / "heap_regions.csv")
    regions = load_regions(regions_csv)
    print(f"{len(regions)} heap regions from {regions_csv}")

    known_ids = load_known_ids()
    print(f"{len(known_ids)} known message-type ids loaded")

    data = a.heap.read_bytes()
    print(f"loaded heap: {a.heap} ({len(data)} bytes)\n")

    print(f"=== shape 1: [u16 id][padding][8-byte code pointer] runs, >= {a.known_ids_min} consecutive entries ===")
    pair_hits = scan_id_fnptr_pairs(data, regions, known_ids, a.known_ids_min)
    pair_hits.sort(key=lambda h: -h[1])
    print(f"{len(pair_hits)} candidate run(s)")
    for addr, length, ids in pair_hits[:20]:
        print(f"  VA 0x{addr:X}  {length} entries  ids={ids[:15]}{' ...' if len(ids) > 15 else ''}")

    print(f"\n=== shape 2: plain code-pointer runs (>= {a.min_run} qwords) anywhere in heap ===")
    plain_hits = scan_plain_pointer_runs(data, regions, a.min_run)
    plain_hits.sort(key=lambda h: -h[1])
    print(f"{len(plain_hits)} candidate run(s)")
    for addr, length in plain_hits[:20]:
        print(f"  VA 0x{addr:X}  {length} qwords ({length * 8} bytes)")

    if not pair_hits and not plain_hits:
        print("\nBoth heap shapes empty too. Offline structural/static avenues for the dispatcher "
              "are now genuinely exhausted -- next is the recv/WSARecv call-graph trace "
              "(tools/trace_recv_callgraph.py), still offline, or the live Detour fallback "
              "(needs its own go-ahead) if that also comes up dry.")


if __name__ == "__main__":
    main()
