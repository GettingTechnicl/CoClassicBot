#!/usr/bin/env python3
"""
registry_v2.py - recompile-robust fingerprints for the offset registry (used by build_offset_registry.py).

What changed from v1 and why (docs/investigation/SIGNATURE_METHODOLOGY.md):
  * NO signature is grown to become unique. A longer raw byte run is more specific to ONE build and more
    brittle across a recompile. Every function carries several stable features instead (fixed 32-byte masked
    prefix, normalised instruction-shape stream, distinctive immediates, referenced strings, callee count)
    plus a DISTINCTIVENESS score (how similar the most similar OTHER function is in the same image).
  * Functions that are not distinctive (template instantiations, MSVC magic-static accessors...) are anchored
    through their CALLERS: a distinctive caller + the ordinal of the call to the target.
  * Globals are relocated through the code that references them: the enclosing function's fingerprint plus
    the ordinal of the reference (and of the reference among same-shaped instructions). A tiny fixed local
    window is kept only as a weak corroborating vote.
  * Struct fields get a RE-FINDING STRATEGY, not just a number: either code-access sites (a window of
    normalised instructions around an instruction that reads/writes the field, inside an anchored function,
    from which the NEW displacement is read back), or - where the code is too generic - a live-correlation
    recipe (the method that found current HP).
Thresholds below are configuration, to be tuned against tools/sig_validate.py results, never against the
identity check.
"""
import re
import sigkit as sk
from collections import defaultdict

DISTINCT_BELOW = 0.80        # best_other similarity under which a function counts as distinctive
MAX_CALLERS = 3
MAX_GLOBAL_SITES = 4
MAX_FIELD_SITES = 5
WINDOW = 3                   # +-instructions around a field-access site (normalised tokens, no displacement)
DISTINCT_FIELD_MIN = 0x40    # offsets below this are too common to anchor on

FIELD_GROUP = {"CRole.h": "role/hero", "CHero.h": "role/hero", "CItem.h": "item", "CGameMap.h": "map",
               "CMapObj.h": "mapobj", "CMagic.h": "magic", "CEntityInfo.h": "entityinfo", "CEntitySet.h": "entityset"}

QUANTITY_HINTS = [("hp", "on-screen HP"), ("mana", "on-screen MP"), ("mp", "on-screen MP"), ("level", "character level"),
                  ("silver", "silver/gold shown in the bag"), ("gold", "gold shown in the bag"), ("exp", "experience bar"),
                  ("stamina", "stamina/PP bar"), ("kill", "the game's own kill counter"), ("pos", "map coordinates (Map tab)"),
                  ("id", "the character/object id shown in logs"), ("name", "the displayed name"), ("skill", "the skills list"),
                  ("equip", "equipped item types"), ("item", "bag/ground item types")]


def fp_of(img, idx, f):
    ex = sk.exact_masked(f, 32)
    best = idx.best_other(f)
    d = dict(
        func_rva=hex(f.start),
        detected_start=(f.start in idx.by_start),
        exact32=sk.to_hex(ex),
        exact32_unique_old=bool(len(ex) >= 16 and len(idx.find_masked(ex)) == 1),
        shape=list(sk.shape(f)),
        imms=[hex(x) for x in sorted(sk.imms(f))][:8],
        strings=sorted(sk.strings_of(img, f))[:4],
        callee_count=len(f.calls),
        func_len=int(f.end - f.start) if f.end else 0,
        best_other=round(best[0], 3),
    )
    d["distinctive"] = bool(d["exact32_unique_old"] or d["best_other"] < DISTINCT_BELOW)
    return d


def distinctive(fp):
    return fp["exact32_unique_old"] or fp["best_other"] < DISTINCT_BELOW


def build(img, entries, fields):
    """returns (entries, fields, stats). Mutates entries/fields with the v2 anchors."""
    idx = sk.Index(img)                                   # whole-image sweep (whole-function refs/calls/accesses)
    callers = idx.callers_of()
    fp_cache = {}

    def fp_for(func):
        if func.start not in fp_cache:
            fp_cache[func.start] = fp_of(img, idx, func)
        return fp_cache[func.start]

    def caller_anchors(target_rva):
        cands = []
        for (cf, k) in callers.get(target_rva, []):
            cfp = fp_for(cf)
            if distinctive(cfp):
                cands.append((cfp["best_other"], dict(fp=cfp, call_ordinal=k, call_count=len(cf.calls))))
        cands.sort(key=lambda t: t[0])
        return [c for _, c in cands[:MAX_CALLERS]]

    stats = dict(code=0, code_distinct=0, code_caller_anchored=0, code_no_fp=0, globals=0, globals_sites=0, globals_no_sites=0,
                 fields=0, fields_code_access=0, fields_live=0)

    # ------------------------------------------------------------------ code entries
    for e in entries:
        if e["kind"] != "code":
            continue
        stats["code"] += 1
        if not img.is_code(e["rva"]):
            e["fp"] = None; stats["code_no_fp"] += 1; continue
        f = idx.by_start.get(e["rva"]) or sk.func_at(img, e["rva"])
        if f.n < 6:
            e["fp"] = None; e["fp_note"] = "does not decode as a function from this address"; stats["code_no_fp"] += 1; continue
        e["fp"] = fp_for(f)
        e["fp"]["distinctive"] = distinctive(e["fp"])
        if e["fp"]["distinctive"]:
            stats["code_distinct"] += 1
        else:
            e["callers"] = caller_anchors(e["rva"])
            stats["code_caller_anchored"] += bool(e["callers"])

    # ------------------------------------------------------------------ data entries (globals)
    refs_by_target = defaultdict(list)                    # data target -> [(func, k among data refs, token id, ordinal among same token)]
    for f in idx.funcs:
        dr = [(i, r, t) for (i, r, t) in f.refs if not img.is_code(t)]
        seen = defaultdict(int)
        for k, (i, r, t) in enumerate(dr):
            tk = f.toks[i]
            refs_by_target[t].append((f, k, tk, seen[tk], r))
            seen[tk] += 1
    for e in entries:
        if e["kind"] != "data":
            continue
        stats["globals"] += 1
        sites = refs_by_target.get(e["rva"], [])
        scored = []
        for (f, k, tk, ordt, site_rva) in sites:
            fp = fp_for(f)
            scored.append((0 if distinctive(fp) else 1, fp["best_other"], f, k, tk, ordt, site_rva))
        scored.sort(key=lambda t: (t[0], t[1]))
        anchors, used = [], set()
        for (_, _, f, k, tk, ordt, site_rva) in scored:
            if f.start in used:
                continue
            used.add(f.start)
            fp = fp_for(f)
            a = dict(func=fp, ord_all=k, ord_token=ordt, token=sk._TOKNAME[tk], site_rva=hex(site_rva))
            if not distinctive(fp):
                a["callers"] = caller_anchors(f.start)
            anchors.append(a)
            if len(anchors) >= MAX_GLOBAL_SITES:
                break
        e["anchors"] = anchors
        stats["globals_sites"] += len(anchors)
        if not anchors:
            stats["globals_no_sites"] += 1

    # ------------------------------------------------------------------ struct fields
    by_group = defaultdict(set)
    for fd in fields:
        if fd["offset"] >= DISTINCT_FIELD_MIN and not fd.get("asserted"):
            by_group[FIELD_GROUP.get(fd["file"], fd["file"])].add(fd["offset"])
    access = defaultdict(list)                            # displacement -> [(func, memacc idx, ins idx, rva)]
    wanted = set().union(*by_group.values()) if by_group else set()
    for f in idx.funcs:
        for (i, r, d) in f.memacc:
            if d in wanted:
                access[d].append((f, i, r))
    for fd in fields:
        stats["fields"] += 1
        grp = FIELD_GROUP.get(fd["file"], fd["file"])
        offs = by_group.get(grp, set())
        fd["group"] = grp
        sites = []
        if fd["offset"] >= DISTINCT_FIELD_MIN:
            ranked = []
            for (f, i, r) in access.get(fd["offset"], []):
                co = len({d for (_, _, d) in f.memacc if d in offs})          # functions touching several fields of the same object
                if co >= 2:
                    ranked.append((-co, fp_for(f)["best_other"], f, i, r, co))
            ranked.sort(key=lambda t: (t[0], t[1]))
            used = set()
            for (_, _, f, i, r, co) in ranked:
                if f.start in used:
                    continue
                used.add(f.start)
                lo = max(0, i - WINDOW)
                win = [sk._TOKNAME[t] for t in f.toks[lo:i + WINDOW + 1]]
                same = sum(1 for (i2, _, _) in f.memacc if f.toks[i2] == f.toks[i] and i2 < i)
                fp = fp_for(f)
                s = dict(func=fp, window=win, centre=i - lo, disp_old=fd["offset"], ord_token=same, co_access=co, site_rva=hex(r))
                if not distinctive(fp):
                    s["callers"] = caller_anchors(f.start)
                sites.append(s)
                if len(sites) >= MAX_FIELD_SITES:
                    break
        fd["access_sites"] = sites
        if len(sites) >= 2:
            fd["strategy"] = "code-access"; stats["fields_code_access"] += 1
        else:
            fd["strategy"] = "live-correlation"; stats["fields_live"] += 1
            name = fd["field"].lower()
            qty = next((q for k, q in QUANTITY_HINTS if k in name), "a value you can read off the screen")
            fd["recipe"] = ("dump the owner object at several states and rank-correlate every dword/word against %s "
                            "(the method that found current HP; see docs/investigation/CURRENT_HP_READ_INVESTIGATION.md); "
                            "neighbours in the same struct usually move by the same delta" % qty)
    return entries, fields, stats
