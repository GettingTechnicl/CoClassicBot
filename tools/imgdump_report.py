#!/usr/bin/env python3
"""
imgdump_report.py - coverage / decrypt-progress report + "best decrypted image" union
for the checkpoints written by src/imgdump.cpp (tools/capture_session.ps1).

  python tools/imgdump_report.py <session_dir>                 # report only
  python tools/imgdump_report.py <session_dir> --union out.bin # also build the union image

Why: ImConquer.exe is Themida-packed and decrypts lazily, so any single image dump has
holes (never-executed code still encrypted) and different checkpoints have different
holes. Per 4 KB page this classifies:
  unreadable  page not safely readable (uncommitted / PAGE_NOACCESS / PAGE_GUARD), zero-filled
  zero        readable but all zero
  packed      readable, non-zero, and incompressible (zlib ratio >= 0.97): looks encrypted/packed
  clear       readable, non-zero, compressible: looks like decrypted code/data
The classification is a HEURISTIC (compressibility), good for tracking progress and for
choosing between checkpoints; it is not proof a page is valid code.

Union rule: for every page take the LATEST checkpoint in which it is 'clear'; else the latest
non-zero readable one; else zeros. The source checkpoint per page is recorded in <out>.pages.csv.
"""
import csv, json, os, struct, sys, zlib

PAGE = 4096
PACKED_RATIO = 0.97


def classify(page: bytes, readable: bool) -> str:
    if not readable:
        return "unreadable"
    if not any(page):
        return "zero"
    return "packed" if len(zlib.compress(page, 1)) / PAGE >= PACKED_RATIO else "clear"


def read_regions(cp):
    """rva ranges that were readable, from regions.csv"""
    rd = []
    with open(os.path.join(cp, "regions.csv"), newline="") as f:
        for r in csv.DictReader(f):
            if r["readable"] == "1":
                rd.append((int(r["rva"], 16), int(r["rva"], 16) + int(r["size"], 16)))
    return rd


def load_sections(cp):
    out = []
    p = os.path.join(cp, "sections.csv")
    if os.path.exists(p):
        with open(p, newline="") as f:
            for r in csv.DictReader(f):
                out.append((r["name"], int(r["rva"], 16), int(r["virtual_size"], 16)))
    return out


def analyse(cp):
    meta = json.load(open(os.path.join(cp, "meta.json")))
    img = open(os.path.join(cp, "image.bin"), "rb").read()
    npages = len(img) // PAGE
    rd = read_regions(cp)
    readable = bytearray(npages)
    for lo, hi in rd:
        for p in range(lo // PAGE, min(hi // PAGE, npages)):
            readable[p] = 1
    cls = [classify(img[p * PAGE:(p + 1) * PAGE], bool(readable[p])) for p in range(npages)]
    hashes = open(os.path.join(cp, "pagehash.bin"), "rb").read()
    hv = struct.unpack("<%dQ" % (len(hashes) // 8), hashes)
    return dict(dir=cp, meta=meta, img=img, npages=npages, cls=cls, hash=hv, sections=load_sections(cp))


def main():
    if len(sys.argv) < 2:
        print(__doc__); return 2
    sess = sys.argv[1]
    union_out = sys.argv[sys.argv.index("--union") + 1] if "--union" in sys.argv else None
    cps = sorted(d for d in os.listdir(sess) if d.startswith("cp") and os.path.exists(os.path.join(sess, d, "DONE")))
    if not cps:
        print("no completed checkpoints in", sess); return 1

    data = []
    print(f"{'checkpoint':28} {'v1074':5} {'hero':12} {'map':5} {'clear':>6} {'packed':>6} {'zero':>6} {'unread':>6} {'chg-vs-prev':>11} {'new-clear':>9}")
    prev = None
    for name in cps:
        a = analyse(os.path.join(sess, name))
        if a["npages"] * PAGE != int(a["meta"]["size_of_image"], 16):
            print("  WARNING: image.bin size != SizeOfImage in", name)
        c = {k: a["cls"].count(k) for k in ("clear", "packed", "zero", "unreadable")}
        chg = newclear = 0
        if prev:
            n = min(len(a["hash"]), len(prev["hash"]))
            chg = sum(1 for i in range(n) if a["hash"][i] != prev["hash"][i])
            newclear = sum(1 for i in range(n) if a["cls"][i] == "clear" and prev["cls"][i] != "clear")
        m = a["meta"]
        print(f"{name:28} {str(m['build_is_v1074'])[:5]:5} {m['hero_name'][:12]:12} {m['map_id']:<5} "
              f"{c['clear']:6} {c['packed']:6} {c['zero']:6} {c['unreadable']:6} {chg:11} {newclear:9}")
        data.append(a); prev = a

    last = data[-1]
    print("\nPer-section state at the LAST checkpoint (pages: clear/packed/zero/unreadable):")
    for name, rva, vsz in last["sections"]:
        lo, hi = rva // PAGE, min((rva + vsz + PAGE - 1) // PAGE, last["npages"])
        c = {k: sum(1 for p in range(lo, hi) if last["cls"][p] == k) for k in ("clear", "packed", "zero", "unreadable")}
        print(f"  {name:10} rva 0x{rva:07X} size 0x{vsz:07X}  {c['clear']:5}/{c['packed']:5}/{c['zero']:5}/{c['unreadable']:5}")

    if union_out:
        npages = min(d["npages"] for d in data)
        out = bytearray(npages * PAGE)
        src = []
        for p in range(npages):
            pick = None
            for a in reversed(data):
                if a["cls"][p] == "clear":
                    pick = a; break
            if pick is None:
                for a in reversed(data):
                    if a["cls"][p] in ("packed",):
                        pick = a; break
            if pick is not None:
                out[p * PAGE:(p + 1) * PAGE] = pick["img"][p * PAGE:(p + 1) * PAGE]
            src.append((p, os.path.basename(pick["dir"]) if pick else "", pick["cls"][p] if pick else "none"))
        with open(union_out, "wb") as f:
            f.write(out)
        with open(union_out + ".pages.csv", "w", newline="") as f:
            w = csv.writer(f); w.writerow(["page", "rva", "source_checkpoint", "class"])
            for p, s, c in src:
                w.writerow([p, hex(p * PAGE), s, c])
        cnt = {}
        for _, _, c in src: cnt[c] = cnt.get(c, 0) + 1
        print("\nunion written:", union_out, cnt)
    return 0


if __name__ == "__main__":
    sys.exit(main())
