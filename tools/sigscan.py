#!/usr/bin/env python3
"""
sigscan.py - propose where each registry entry moved in another build's decrypted image (v2, tiered).

  python tools/sigscan.py --image <new_image.bin|.exe|.dll> [--registry docs/investigation/v1074_offset_registry.json]
                          [--status verified] [--fields] [--out report.md] [--identity]

Locating a FUNCTION cascades: (1) fixed 32-byte masked prefix, only trusted when it was unique in the old
build; (2) normalised instruction-shape stream (exact prefix match, else fuzzy trigram similarity that must beat
the runner-up by a margin); (3) for functions with near-identical siblings, the function's distinctive CALLERS
(locate the caller, take the call at the same ordinal). Each result carries its METHOD and SCORE.
GLOBALS are read out of the code that references them (function-anchored, by ordinal), sites vote.
STRUCT FIELDS: each code-access site is re-found by its window of normalised instructions inside the
located function and the NEW displacement is read back; sites vote; per-struct delta consistency and
interpolation are reported (interpolated values are hypotheses, flagged as such). Fields whose strategy is
`live-correlation` are listed with their recipe - a bare offset never survives an update.

Nothing here verifies anything on a live client; every proposal must be re-validated (self-test / paired dumps).

--identity  run on the build the registry came from. A SANITY GATE only (it proves the anchors are consistent,
            NOT that they survive a recompile). The real measure is tools/sig_validate.py on a known pair and,
            ultimately, the hit-rate on the real new build.
"""
import argparse, json, os, sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sigkit as sk

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARGIN = 0.05            # shape-fuzzy must beat the runner-up by this much to count as a relocation
SHAPE_SANITY = 0.5       # caller-anchored targets must at least look like the original


def hexlist(s):
    return [None if p == "??" else int(p, 16) for p in s.split()]


class Locator:
    def __init__(self, img):
        self.img = img
        self.idx = sk.Index(img)
        self.cache = {}

    def by_fp(self, fp, callers=None, depth=0):
        key = (fp["func_rva"], depth)
        if key in self.cache:
            return self.cache[key]
        res = self._by_fp(fp, callers, depth)
        self.cache[key] = res
        return res

    def _by_fp(self, fp, callers, depth):
        idx = self.idx
        if fp.get("exact32_unique_old"):
            h = idx.find_masked(hexlist(fp["exact32"]))
            if len(h) == 1:
                return dict(rva=h[0], method="exact32", score=1.0, margin=1.0)
        sh = tuple(fp["shape"])
        imm = {int(x, 16) for x in fp.get("imms", [])}
        loc = idx.locate_shape(sh, imm)
        if loc and (loc[3] == "shape-exact" or loc[2] >= MARGIN):
            return dict(rva=loc[0], method=loc[3], score=round(loc[1], 3), margin=round(loc[2], 3))
        if callers and depth == 0:
            for c in callers:
                cl = self.by_fp(c["fp"], None, 1)
                if not cl:
                    continue
                cf = idx.by_start.get(cl["rva"])
                k = c["call_ordinal"]
                if cf and k < len(cf.calls):
                    tgt = cf.calls[k][2]
                    tf = idx.by_start.get(tgt) or sk.func_at(self.img, tgt)
                    sim = len(sk.grams(tuple(fp["shape"])) & sk.grams(sk.shape(tf))) / max(1, len(sk.grams(tuple(fp["shape"])) | sk.grams(sk.shape(tf))))
                    if sim >= SHAPE_SANITY:
                        return dict(rva=tgt, method="caller-anchor", score=round(sim, 3), margin=0.0)
        if loc:
            return dict(rva=loc[0], method="shape-AMBIGUOUS", score=round(loc[1], 3), margin=round(loc[2], 3), low=True)
        return None

    def func(self, rva):
        return self.idx.by_start.get(rva) or sk.func_at(self.img, rva)


def resolve_global(loc, e):
    votes, detail = Counter(), []
    for a in e.get("anchors", []):
        r = loc.by_fp(a["func"], a.get("callers"))
        if not r or r.get("low"):
            continue
        nf = loc.func(r["rva"])
        dr = [(i, t) for (i, _r, t) in nf.refs if not loc.img.is_code(t)]
        pick = None
        if a["ord_all"] < len(dr) and sk._TOKNAME[nf.toks[dr[a["ord_all"]][0]]] == a["token"]:
            pick = dr[a["ord_all"]][1]
        else:
            same = [t for (i, t) in dr if sk._TOKNAME[nf.toks[i]] == a["token"]]
            if a["ord_token"] < len(same):
                pick = same[a["ord_token"]]
        if pick is not None:
            votes[pick] += 1
            detail.append((r["method"], hex(pick)))
    return votes, detail


def resolve_field(loc, fd):
    votes = Counter()
    for s in fd.get("access_sites", []):
        r = loc.by_fp(s["func"], s.get("callers"))
        if not r or r.get("low"):
            continue
        nf = loc.func(r["rva"])
        ids = [sk._TOKID.get(t) for t in s["window"]]
        if None in ids:
            continue
        w = len(ids)
        hits = [p for p in range(len(nf.toks) - w + 1) if nf.toks[p:p + w] == ids]
        disp = None
        md = {i: d for (i, _, d) in nf.memacc}
        if len(hits) == 1:
            disp = md.get(hits[0] + s["centre"])
        if disp is None:
            tk = sk._TOKID.get(s["window"][s["centre"]])
            cand = [d for (i, _, d) in nf.memacc if nf.toks[i] == tk]
            if s["ord_token"] < len(cand):
                disp = cand[s["ord_token"]]
        if disp is not None:
            votes[disp] += 1
    return votes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--registry", default=os.path.join(REPO, "docs", "investigation", "v1074_offset_registry.json"))
    ap.add_argument("--status", default="verified")
    ap.add_argument("--fields", action="store_true")
    ap.add_argument("--identity", action="store_true")
    ap.add_argument("--out")
    a = ap.parse_args()
    reg = json.load(open(a.registry, encoding="utf-8"))
    img = sk.Image.from_dump(a.image) if a.image.lower().endswith(".bin") else sk.Image.from_pe_file(a.image)
    print("target: %s  PE stamp 0x%08X  (registry built from %s)" % (os.path.basename(a.image), img.stamp, reg["image"]["pe_timedatestamp"]))
    loc = Locator(img)
    print("swept %d functions" % len(loc.idx.funcs))
    statuses = set(a.status.split(",")) if a.status != "all" else None

    out, bad = [], 0
    rows = []
    for e in reg["entries"]:
        if statuses and e["status"] not in statuses:
            continue
        if e["kind"] == "code" and e.get("fp"):
            r = loc.by_fp(e["fp"], e.get("callers"))
            rows.append((e, "code", r))
        elif e["kind"] == "data":
            v, d = resolve_global(loc, e)
            rows.append((e, "data", (v, d)))
    by = {e["name"]: (k, r) for (e, k, r) in rows}
    lines = ["| name | kind | old RVA | method | new RVA | delta | confidence |", "|---|---|---|---|---|---|---|"]
    moved = []
    for (e, k, r) in rows:
        if k == "code":
            if not r:
                res = ("NOT FOUND", "-", "-", "none"); ok = False
            else:
                conf = "LOW (siblings)" if r.get("low") else ("high" if r["method"] in ("exact32", "shape-exact") else "medium")
                res = (r["method"], "0x%X" % r["rva"], "%+#x" % (r["rva"] - e["rva"]), conf)
                ok = (r["rva"] == e["rva"] and not r.get("low"))
                if not r.get("low"):
                    moved.append((e["rva"], r["rva"], e["name"]))
        else:
            votes, detail = r
            if e.get("derived_from") and e["derived_from"] in by and by[e["derived_from"]][0] == "data":
                bv = by[e["derived_from"]][1][0]
                if len(bv) == 1:
                    (b0, c0), = bv.items()
                    votes = Counter({b0 + e["derived_delta"]: c0})
            if not votes:
                res = ("no anchors resolved", "-", "-", "none"); ok = False
            elif len(votes) == 1:
                (t, c), = votes.items()
                res = ("anchors x%d" % c, "0x%X" % t, "%+#x" % (t - e["rva"]), "high" if c >= 2 else "candidate (1 anchor - verify live)")
                ok = (t == e["rva"])
                moved.append((e["rva"], t, e["name"]))
            else:
                res = ("CONFLICT", ", ".join("0x%X(x%d)" % (t, c) for t, c in votes.most_common()), "-", "LOW"); ok = False
        if a.identity and not ok:
            bad += 1
        lines.append("| `%s` | %s | 0x%X | %s | %s | %s | %s |" % (e["name"], k, e["rva"], res[0], res[1], res[2], res[3] + ("  <-- differs from old" if a.identity and not ok else "")))
    print("\n".join(lines))

    # ---- ordering check: a linker keeps the relative order of code it did not reorder, so a proposal that lands OUTSIDE the
    # interval set by its nearest exact-matched neighbours (by old address) is suspect however good its own score looks.
    # (The first v1078 capture caught CNETCLIENT_SEND_MSG_REAL this way: shape-exact at delta -0x16390 while every neighbour moved +0x9000..+0x17000.)
    anchors = sorted((e["rva"], r["rva"], e["name"]) for (e, k, r) in rows
                     if k == "code" and r and r["method"] == "exact32")
    violations = []
    for (e, k, r) in rows:
        if k != "code" or not r or r.get("low") or r["method"] == "exact32":
            continue
        lo = [x for x in anchors if x[0] < e["rva"]]
        hi = [x for x in anchors if x[0] > e["rva"]]
        lo_n = lo[-1][1] if lo else 0
        hi_n = hi[0][1] if hi else 1 << 40
        if not (lo_n < r["rva"] < hi_n):
            violations.append(e["name"])
            print("ORDER VIOLATION: %s proposed 0x%X but exact-matched neighbours bound it to (0x%X, %s) -> treat as WRONG/unproven" %
                  (e["name"], r["rva"], lo_n, ("0x%X" % hi_n) if hi else "end"))
    if violations:
        print("%d order violation(s): those proposals are not to be used" % len(violations))
    moved.sort()
    inv = [(moved[i][2], moved[i + 1][2]) for i in range(len(moved) - 1) if moved[i][1] > moved[i + 1][1]]
    print("\nconsistency: %d relocated entries; order inversions (old order != new order): %d %s" % (len(moved), len(inv), inv[:6]))
    dl = Counter((n - o) >> 12 for (o, n, _) in moved)
    print("delta clusters (4 KB units): %s" % dict(dl.most_common(6)))

    if a.fields:
        print("\n### struct fields")
        per_group = defaultdict(list)
        unresolved, conflicts = [], []
        for fd in reg["struct_fields"]:
            if fd.get("strategy") == "code-access":
                v = resolve_field(loc, fd)
                if v:
                    best, c = v.most_common(1)[0]
                    if len(v) > 1:
                        conflicts.append(fd["field"])
                        unresolved.append(fd)
                        continue
                    per_group[fd["group"]].append((fd["offset"], best, c, len(fd["access_sites"]), fd["field"]))
                else:
                    unresolved.append(fd)
            else:
                unresolved.append(fd)
        for g, lst in per_group.items():
            lst.sort()
            ds = Counter(n - o for (o, n, *_r) in lst)
            hi = sum(1 for x in lst if x[2] >= 2)
            print("group %-11s resolved %3d fields (%d with >=2 agreeing sites, the rest single-site candidates); deltas: %s"
                  % (g, len(lst), hi, dict(ds.most_common(5))))
        # interpolate unresolved fields between two resolved neighbours with the same delta (hypothesis only)
        interp = 0
        for fd in unresolved:
            lst = per_group.get(fd.get("group", ""), [])
            lo = [x for x in lst if x[0] <= fd["offset"]]
            hi = [x for x in lst if x[0] >= fd["offset"]]
            if lo and hi and (lo[-1][1] - lo[-1][0]) == (hi[0][1] - hi[0][0]):
                interp += 1
                fd["_proposed"] = fd["offset"] + (lo[-1][1] - lo[-1][0])
        if conflicts:
            print("fields whose sites DISAGREE (rejected, need live check): %s" % ", ".join(conflicts))
        print("unresolved by code: %d; of which interpolated between two equal-delta neighbours (HYPOTHESIS, needs live check): %d" % (len(unresolved), interp))
    if a.identity:
        n = len(rows)
        print("\nidentity sanity gate: %d/%d entries resolve to their own address%s" % (n - bad, n, "" if not bad else "  -- anchors inconsistent"))
        print("(this does NOT show the anchors survive a recompile - see tools/sig_validate.py)")
    if a.out:
        open(a.out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    return 1 if (a.identity and bad) else 0


if __name__ == "__main__":
    sys.exit(main())
