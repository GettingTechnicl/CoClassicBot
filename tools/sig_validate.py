#!/usr/bin/env python3
"""
sig_validate.py - measure how well each signature style SURVIVES A RECOMPILE, against ground truth.

The registry (tools/build_offset_registry.py) exists to relocate offsets in a DIFFERENT client build. A
self-match on the build it was generated from (sigscan.py --identity) only proves uniqueness. This harness
measures the thing that matters: take a function/global identified in build A, find it in build B, and
compare with the true answer.

Ground truth comes from linker MAP files of two builds of OUR OWN code (same MSVC toolchain and flags as the
game, different source revisions, unpacked so the code is directly readable), e.g. coclassic.dll at an old
commit vs HEAD. Only functions whose translation unit is byte-for-byte unchanged in source between the two
commits are sampled, so what is measured is recompile / relayout / inlining differences, not real edits.

  python tools/sig_validate.py --old-dll A.dll --old-map A.map --new-dll B.dll --new-map B.map [--changed a.cpp,b.h]

Caveat (say it out loud): this validates the METHODOLOGY on a compiler-realistic pair. It says nothing
about Themida-specific effects or about how much the game's code actually changed between v1074 and
v1078; that number only exists once the v1078 image is captured and run through sigscan.py.
"""
import argparse, os, re, sys, time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sigkit as sk

MAP_LINE = re.compile(r"^\s*([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8})\s+(\S+)\s+([0-9A-Fa-f]{16})\s+(?:(f)\s+)?(?:(i)\s+)?(.+?)\s*$")


def parse_map(path):
    base, funcs, data = 0, {}, {}
    txt = open(path, encoding="utf-8", errors="replace").read().split("\n")
    in_pub = False
    for ln in txt:
        m = re.search(r"Preferred load address is ([0-9A-Fa-f]+)", ln)
        if m:
            base = int(m.group(1), 16)
        if "Publics by Value" in ln:
            in_pub = True
            continue
        if not in_pub:
            continue
        m = MAP_LINE.match(ln)
        if not m:
            continue
        sec, off, name, addr, isf, isi, obj = m.groups()
        rva = int(addr, 16) - base
        (funcs if isf else data)[name] = (rva, obj)
    return base, funcs, data


def outcome(hits, truth):
    if not hits:
        return "miss"
    if len(hits) == 1:
        return "correct" if hits[0] == truth else "wrong"
    return "ambiguous" if truth in hits else "wrong"


def pct(n, d):
    return "%5.1f%%" % (100.0 * n / d) if d else "   n/a"


def cascade(f, oi, ni):
    """locate function f (from index oi) in ni: unique exact32 first, else shape with a real margin"""
    sig = sk.exact_masked(f, 32)
    if len(sig) >= 16 and len(oi.find_masked(sig)) == 1:
        h = ni.find_masked(sig)
        if len(h) == 1:
            return h[0], "exact"
    loc = ni.locate_shape(sk.shape(f), sk.imms(f))
    if loc and not (loc[3] == "shape-fuzzy" and loc[2] < 0.05):
        return loc[0], loc[3]
    return None, None


def data_refs(img, f):
    return [(i, r, t) for (i, r, t) in f.refs if not img.is_code(t)]


def eval_globals(oi, ni, odata, ndata, maxg):
    """relocate globals through the code that references them (function-anchored, by ordinal)"""
    idx = {}
    for f in oi.funcs:
        dr = data_refs(oi.img, f)
        for k, (i, r, t) in enumerate(dr):
            tk = f.toks[i]
            same = sum(1 for (i2, _, _) in dr[:k] if f.toks[i2] == tk)
            idx.setdefault(t, []).append((f, k, tk, same))
    cache, tally = {}, {"single-site": Counter(), "voted(<=3 sites)": Counter(), "voted, same-token ordinal": Counter(),
                          "sigscan policy: abstain on any disagreement": Counter(),
                          "  ... only when >=2 sites agree": Counter()}
    n = 0
    for name, (to, _) in odata.items():
        if name not in ndata or to not in idx:
            continue
        tn = ndata[name][0]
        votes_a, votes_b = Counter(), Counter()
        for (f, k, tk, same) in idx[to][:3]:
            if f.start not in cache:
                cache[f.start] = cascade(f, oi, ni)
            nf_rva, how = cache[f.start]
            if nf_rva is None or nf_rva not in ni.by_start:
                continue
            nf = ni.by_start[nf_rva]
            ndr = data_refs(ni.img, nf)
            if k < len(ndr):
                votes_a[ndr[k][2]] += 1
            cand = [t for (i2, _, t) in ndr if nf.toks[i2] == tk]
            if same < len(cand):
                votes_b[cand[same]] += 1
        if not votes_a and not votes_b:
            tally["single-site"]["not-located"] += 1
            continue
        n += 1
        first = None
        for (f, k, tk, same) in idx[to][:1]:
            nf_rva = cache.get(f.start, (None, None))[0]
            if nf_rva in ni.by_start:
                ndr = data_refs(ni.img, ni.by_start[nf_rva])
                first = ndr[k][2] if k < len(ndr) else None
        tally["single-site"]["correct" if first == tn else ("none" if first is None else "wrong")] += 1
        for key, v in (("voted(<=3 sites)", votes_a), ("voted, same-token ordinal", votes_b)):
            if not v:
                tally[key]["none"] += 1
                continue
            best, c = v.most_common(1)[0]
            tally[key]["correct" if best == tn else "wrong"] += 1
        # the policy tools/sigscan.py actually applies: a proposal only if every located site agrees
        v = votes_b or votes_a
        if len(v) == 1:
            best, c = v.most_common(1)[0]
            ok = "correct" if best == tn else "wrong"
            tally["sigscan policy: abstain on any disagreement"][ok] += 1
            tally["  ... only when >=2 sites agree"][ok if c >= 2 else "none"] += 1
        else:
            tally["sigscan policy: abstain on any disagreement"]["none"] += 1
            tally["  ... only when >=2 sites agree"]["none"] += 1
        if n >= maxg:
            break
    return tally, n


def eval_fields(sample, oi, ni, per_func=3, maxsites=3000):
    """re-find a struct-field access in a recompiled function and read its NEW displacement.
    Ground truth = the displacement is unchanged for an unchanged translation unit."""
    W = 3
    tally = {"paired-func, window": Counter(), "paired-func, ordinal-in-token": Counter(),
             "cascade-located func, window": Counter()}
    casc = {}
    n = 0
    for (name, r, tr, stem) in sample:
        fo, fn = oi.by_start[r], ni.by_start[tr]
        if not fo.memacc:
            continue
        ndisp = {i: d for (i, _, d) in fn.memacc}
        for (idx, _rva, disp) in fo.memacc[:per_func]:
            n += 1
            win = tuple(fo.toks[max(0, idx - W):idx + W + 1])
            centre = idx - max(0, idx - W)
            # (1) window search inside the truly-corresponding function
            hits = [p for p in range(len(fn.toks) - len(win) + 1) if tuple(fn.toks[p:p + len(win)]) == win]
            if len(hits) == 1:
                d = ndisp.get(hits[0] + centre)
                tally["paired-func, window"]["correct" if d == disp else ("wrong" if d is not None else "miss")] += 1
            else:
                tally["paired-func, window"]["ambiguous" if hits else "miss"] += 1
            # (2) ordinal among accesses with the same token
            tk = fo.toks[idx]
            same = sum(1 for (i2, _, _) in fo.memacc if fo.toks[i2] == tk and i2 < idx)
            cand = [d for (i2, _, d) in fn.memacc if fn.toks[i2] == tk]
            if same < len(cand):
                tally["paired-func, ordinal-in-token"]["correct" if cand[same] == disp else "wrong"] += 1
            else:
                tally["paired-func, ordinal-in-token"]["miss"] += 1
            # (3) end to end: locate the function first (no ground-truth pairing)
            if r not in casc:
                casc[r] = cascade(fo, oi, ni)
            crva = casc[r][0]
            if crva is None or crva not in ni.by_start:
                tally["cascade-located func, window"]["func-not-located"] += 1
            else:
                fc = ni.by_start[crva]
                hh = [p for p in range(len(fc.toks) - len(win) + 1) if tuple(fc.toks[p:p + len(win)]) == win]
                if len(hh) == 1:
                    dm = {i: d for (i, _, d) in fc.memacc}.get(hh[0] + centre)
                    tally["cascade-located func, window"]["correct" if dm == disp else ("wrong" if dm is not None else "miss")] += 1
                else:
                    tally["cascade-located func, window"]["ambiguous" if hh else "miss"] += 1
            if n >= maxsites:
                return tally, n
    return tally, n


def print_tally(title, tally, n):
    print("\n%s (%d items)" % (title, n))
    print("| method | correct | wrong | ambiguous | miss/none/not-located |")
    print("|---|---|---|---|---|")
    for k, c in tally.items():
        tot = sum(c.values())
        other = c["miss"] + c["none"] + c["not-located"] + c["func-not-located"]
        print("| %s | %s | %s | %s | %s |" % (k, pct(c["correct"], tot), pct(c["wrong"], tot), pct(c["ambiguous"], tot), pct(other, tot)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old-dll", required=True); ap.add_argument("--old-map", required=True)
    ap.add_argument("--new-dll", required=True); ap.add_argument("--new-map", required=True)
    ap.add_argument("--changed", default="", help="comma list of source file STEMS changed between the builds (skip their objects)")
    ap.add_argument("--min-size", type=int, default=64)
    ap.add_argument("--max-funcs", type=int, default=4000)
    ap.add_argument("--out")
    a = ap.parse_args()
    changed = {c.strip().lower() for c in a.changed.split(",") if c.strip()}

    t0 = time.time()
    ob, ofn, odata = parse_map(a.old_map); nb, nfn, ndata = parse_map(a.new_map)
    oimg, nimg = sk.Image.from_pe_file(a.old_dll), sk.Image.from_pe_file(a.new_dll)
    print("old: %d public funcs, new: %d; images %s (%d B) / %s (%d B)" % (len(ofn), len(nfn), os.path.basename(a.old_dll), len(oimg.d), os.path.basename(a.new_dll), len(nimg.d)))
    oi, ni = sk.Index(oimg), sk.Index(nimg)
    print("swept: old %d functions, new %d functions (%.0fs)" % (len(oi.funcs), len(ni.funcs), time.time() - t0))

    # function-boundary detection quality against the MAP
    o_starts = {f.start for f in oi.funcs}
    det = sum(1 for n, (r, _) in ofn.items() if r in o_starts)
    print("function-start detection: %d/%d public functions found by the sweep (%s)" % (det, len(ofn), pct(det, len(ofn))))

    # sample: same decorated name in both, sweep found the start in BOTH, TU source unchanged, big enough
    sample = []
    order = sorted((r, n) for n, (r, _) in ofn.items())
    nxt = {n: (order[i + 1][0] if i + 1 < len(order) else r + 0x100) for i, (r, n) in enumerate(order)}
    n_starts = {f.start for f in ni.funcs}
    for name, (r, obj) in ofn.items():
        if name not in nfn:
            continue
        stem = os.path.splitext(os.path.basename(obj.replace("\\", "/")))[0].lower()
        if stem in changed:
            continue
        tr = nfn[name][0]
        if r not in oi.by_start or tr not in n_starts:
            continue
        if nxt[name] - r < a.min_size or len(oi.by_start[r].ins) < 12:
            continue
        sample.append((name, r, tr, stem))
    sample = sample[:a.max_funcs]
    print("sample: %d functions (unchanged TU, present in both, >=%d bytes, start detected in both)\n" % (len(sample), a.min_size))

    methods = ["exact32", "exact64", "shape", "cascade(e32>shape)"]
    res = {m: Counter() for m in methods}
    usable = {m: 0 for m in methods}
    for name, r, tr, stem in sample:
        f = oi.by_start[r]
        # ---- exact32 / exact64
        for m, L in (("exact32", 32), ("exact64", 64)):
            sig = sk.exact_masked(f, L)
            if len(sig) < 16:
                continue
            uo = oi.find_masked(sig)
            if len(uo) != 1:
                res[m]["not-unique-in-old"] += 1
                continue
            usable[m] += 1
            res[m][outcome(ni.find_masked(sig), tr)] += 1
        # ---- shape (exact prefix, then fuzzy w/ margin); requires margin so a guess is not called a hit
        sh = sk.shape(f)
        im = sk.imms(f)
        loc = ni.locate_shape(sh, im)
        usable["shape"] += 1
        if loc is None:
            res["shape"]["miss"] += 1
        else:
            rv, score, margin, how = loc
            if how == "shape-fuzzy" and margin < 0.05:
                res["shape"]["ambiguous" if rv == tr else "wrong-low-margin"] += 1
            else:
                res["shape"]["correct" if rv == tr else "wrong"] += 1
        # ---- cascade: unique exact32 first, else shape (with margin)
        sig = sk.exact_masked(f, 32)
        casc = None
        if len(sig) >= 16 and len(oi.find_masked(sig)) == 1:
            h = ni.find_masked(sig)
            if len(h) == 1:
                casc = ("correct" if h[0] == tr else "wrong")
        if casc is None and loc is not None and not (loc[3] == "shape-fuzzy" and loc[2] < 0.05):
            casc = "correct" if loc[0] == tr else "wrong"
        usable["cascade(e32>shape)"] += 1
        res["cascade(e32>shape)"][casc or "miss"] += 1

    lines = ["| method | usable* | correct | ambiguous | wrong | miss |", "|---|---|---|---|---|---|"]
    for m in methods:
        u = usable[m]; c = res[m]
        wrong = c["wrong"] + c["wrong-low-margin"]
        lines.append("| %s | %d | %s | %s | %s | %s |" % (m, u, pct(c["correct"], u), pct(c["ambiguous"], u), pct(wrong, u), pct(c["miss"], u)))
    print("\n".join(lines))
    print("\n* usable = the signature is unique in the OLD build (exact methods; the shape method is always attempted).")
    print("  'correct' means the ONE candidate found in the new build is the true function per the MAP.")

    gt, gn = eval_globals(oi, ni, odata, ndata, 1500)
    print_tally("GLOBALS relocated through their referencing code (ground truth: MAP data symbols)", gt, gn)
    ft, fn_ = eval_fields(sample, oi, ni)
    print_tally("STRUCT-FIELD access sites re-found in recompiled code (truth: displacement unchanged in an unchanged TU)", ft, fn_)
    if a.out:
        open(a.out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
