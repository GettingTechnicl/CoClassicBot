#!/usr/bin/env python3
"""
sigscan.py - locate the v1074 registry entries in another build's decrypted image dump.

  python tools/sigscan.py --image <new_image_dump.bin> [--registry docs/investigation/v1074_offset_registry.json]
                          [--status verified,unverified] [--out report.md] [--identity]

For each registry entry:
  code  : search the masked function-start signature in the new image's code section(s); report
          every hit and the delta from the old RVA. Exactly one hit = confident relocation.
  data  : for each stored xref signature, find the same code site in the new image, read the
          rip-relative displacement there and recover the global's NEW address
          (site + next_ip_off + disp32). Sites vote; agreement across sites = confident.
  derived: a global with no code reference of its own (reached as sibling+delta) is resolved from
          its sibling's result.
Nothing here "verifies" anything on a live client - it only proposes where each item moved. Every
proposed address must still be re-validated at runtime (self-test / imgdump paired dumps).

--identity  run against the SAME v1074 dump the registry was built from: every selected entry must
            resolve to its own RVA. This validates the whole toolchain on a known pair before it is
            trusted on v1074 -> new; exit code 1 if any entry fails.
"""
import argparse, json, os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_offset_registry as bor

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def rx_from_hex(sig):
    return re.compile(b"".join(b"." if p == "??" else re.escape(bytes([int(p, 16)])) for p in sig.split()), re.DOTALL)


def scan_one(e, blobs, limit=20):
    if e["kind"] == "code":
        hits = []
        if e.get("sig"):
            rx = rx_from_hex(e["sig"])
            for base, blob in blobs:
                for m in rx.finditer(blob):
                    hits.append(base + m.start())
                    if len(hits) >= limit:
                        break
        return dict(e=e, kind="code", hits=hits, ok=(len(hits) == 1 and hits[0] == e["rva"]), unique=len(hits) == 1)
    # Two kinds of evidence. STRONG = a signature that is unique in the code section (a local window
    # around the reference site, or the enclosing function's start signature + the site's offset in it).
    # WEAK = a local window that matched several places; only used when there is no strong evidence,
    # and then the result is reported as AMBIGUOUS, never as a confident relocation.
    strong, weak, detail = {}, {}, []

    def add(d, new):
        d[new] = d.get(new, 0) + 1

    for xs in e.get("xref_sigs", []):
        rx = rx_from_hex(xs["sig"])
        for base, blob in blobs:
            ms = list(rx.finditer(blob))
            if not ms or len(ms) > 4:
                continue
            for m in ms:
                new = base + m.start() + xs["next_ip_off"] + struct.unpack_from("<i", blob, m.start() + xs["disp_off"])[0]
                add(strong if len(ms) == 1 else weak, new)
                detail.append(("local", xs["site_rva"], hex(new), len(ms)))
        fr = xs.get("func_rel")
        if fr and fr["func_matches"] == 1:
            frx = rx_from_hex(fr["func_sig"])
            for base, blob in blobs:
                fms = list(frx.finditer(blob))
                if len(fms) == 1:
                    site = fms[0].start() + fr["site_off"]
                    if site + fr["disp_off_in_insn"] + 4 <= len(blob):
                        new = base + site + fr["ins_size"] + struct.unpack_from("<i", blob, site + fr["disp_off_in_insn"])[0]
                        add(strong, new)
                        detail.append(("func", xs["site_rva"], hex(new), 1))
    votes = strong or weak
    best = max(votes.items(), key=lambda kv: kv[1]) if votes else None
    unique = len(votes) == 1 and bool(strong)
    return dict(e=e, kind="data", votes=votes, detail=detail, best=best, strong=bool(strong),
                ok=bool(best and best[0] == e["rva"] and unique),
                contains_own=e["rva"] in votes, unique=unique)


def scan(img, reg, statuses, limit=20):
    blobs = [(s["rva"], img.d[s["rva"]:min(s["rva"] + s["vsize"], len(img.d))]) for s in img.code]
    allreg = {e["name"]: e for e in reg["entries"]}
    rows = [scan_one(e, blobs, limit) for e in reg["entries"] if e["status"] in statuses and e["kind"] in ("code", "data")]
    by_name = {r["e"]["name"]: r for r in rows}
    for r in rows:
        e = r["e"]
        if e.get("derived_from"):
            base = by_name.get(e["derived_from"]) or scan_one(allreg[e["derived_from"]], blobs, limit)
            if base["kind"] == "data" and base["best"] and base["unique"]:
                new = base["best"][0] + e["derived_delta"]
                r.update(votes={new: base["best"][1]}, best=(new, base["best"][1]), unique=True, ok=(new == e["rva"]))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default=r"C:\Users\TerryGluff\Documents\Claude\CO99\scratchpad\image_dump.bin")
    ap.add_argument("--registry", default=os.path.join(REPO, "docs", "investigation", "v1074_offset_registry.json"))
    ap.add_argument("--status", default="verified")
    ap.add_argument("--out")
    ap.add_argument("--identity", action="store_true")
    a = ap.parse_args()
    reg = json.load(open(a.registry, encoding="utf-8"))
    img = bor.Image(a.image)
    print("target image: %s stamp 0x%08X  (registry built from stamp %s)" % (os.path.basename(a.image), img.stamp, reg["image"]["pe_timedatestamp"]))
    rows = scan(img, reg, set(a.status.split(",")))

    lines = ["| name | kind | old RVA | result | new RVA(s) | delta |", "|---|---|---|---|---|---|"]
    bad = 0
    for r in rows:
        e = r["e"]
        if r["kind"] == "code":
            hs = r["hits"]
            res = "UNIQUE" if len(hs) == 1 else ("none" if not hs else "%d hits" % len(hs))
            newr = ", ".join("0x%X" % h for h in hs[:4]) or "-"
            delta = "%+#x" % (hs[0] - e["rva"]) if len(hs) == 1 else "-"
        else:
            b = r["best"]
            if b and r["unique"]:
                res = "agree x%d" % b[1]
            elif b and r["strong"]:
                res = "CONFLICT"
            elif b:
                res = "AMBIGUOUS (%d cand%s)" % (len(r["votes"]), ", incl. own" if r["contains_own"] else "")
            else:
                res = "none"
            newr = ", ".join("0x%X" % k for k in sorted(r["votes"])) or "-"
            delta = "%+#x" % (b[0] - e["rva"]) if b and r["unique"] else "-"
        if a.identity and not r["ok"]:
            bad += 1
            res += "  <-- FAILS identity"
        lines.append("| `%s` | %s | 0x%X | %s | %s | %s |" % (e["name"], r["kind"], e["rva"], res, newr, delta))
    out = "\n".join(lines)
    print(out)
    if a.out:
        open(a.out, "w", encoding="utf-8").write(out + "\n")
    if a.identity:
        n = len(rows)
        print("\nidentity check: %d/%d %s entries resolve to their own address%s" % (n - bad, n, a.status, "" if not bad else "  -- TOOLCHAIN NOT VALIDATED"))
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
