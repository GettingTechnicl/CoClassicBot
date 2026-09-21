#!/usr/bin/env python3
"""
regime_classify.py - decide WHICH regime a new build is in, before trusting any relocation hit-rate.

  python tools/regime_classify.py --old-image v1074_image_dump.bin --new-image cp01/image.bin [cp02/image.bin ...]
                                  [--registry docs/investigation/v1074_offset_registry.json] [--sample 1500]

The question this answers is not "what fraction of offsets ported" but "how did the compiler output change", because that
picks the port strategy (docs/investigation/SIGNATURE_METHODOLOGY.md):

  tier that resolves a function              means
  ---------------------------------------    ---------------------------------------------------------
  exact32   (masked-byte prefix, unique)      same codegen  -> lean on the registry
  shape / caller-anchor only                  codegen-stable-ish (instruction stream kept, bytes moved)
  nothing                                     codegen changed (or not decrypted yet) -> live re-derivation

Reported for (1) every verified code entry in the registry, by name, and (2) a random sample of distinctive functions from
the whole old image (the verified set is only ~11 functions - far too few to classify a build on its own). No ground truth
is needed: a real relocation preserves ORDER, so the inversion rate among relocated functions is the correctness check
(true matches ~0%; a wrong-match cascade shows up as a high rate).

Several --new-image dumps are MERGED page by page (later non-zero page wins), because Themida decrypts lazily and
decryption is cumulative: login-screen + in-world + after-feature-use dumps together cover more than any one.

Guards printed before the verdict (they explain a low hit-rate that is NOT "codegen changed"):
  * sweep coverage: functions detected in the new image vs the old. If the new count is far below the old, suspect the
    function-start sweep (alignment fallback in sigkit.sweep) or an un-decrypted image BEFORE concluding anything.
  * encrypted-looking code pages (byte entropy > 7.5 bits, i.e. ciphertext, not x86) in both images.
Verdict thresholds were calibrated on the own-DLL proxy pairs (same-toolchain pair: exact32 ~80% of the sample;
/O2->/O1 pair: ~18%); see SIGNATURE_METHODOLOGY.md for the calibration run of THIS script on those pairs.
"""
import argparse, json, math, os, random, sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sigkit as sk
import registry_v2 as rv2
import sigscan

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENC_ENTROPY = 7.5


def load_image(paths):
    """one image from one or more dumps; later non-zero pages override earlier ones"""
    def one(p):
        return sk.Image.from_dump(p) if p.lower().endswith(".bin") else sk.Image.from_pe_file(p)
    img = one(paths[0])
    if len(paths) > 1:
        merged = bytearray(img.d)
        for p in paths[1:]:
            other = open(p, "rb").read()
            n = min(len(merged), len(other))
            for off in range(0, n, 4096):
                pg = other[off:off + 4096]
                if any(pg):
                    merged[off:off + len(pg)] = pg
        img = sk.Image(bytes(merged), img.sections if hasattr(img, "sections") else [], img.stamp, img.size_of_image, img.base, "merged")
        # Image.__init__ re-derives the code list from the section table it is given
    return img


def entropy(b):
    if not b:
        return 0.0
    c = Counter(b)
    n = len(b)
    return -sum(v / n * math.log2(v / n) for v in c.values())


def encrypted_pages(img):
    """(set of code-page RVAs that look like ciphertext, number of non-empty code pages)"""
    bad, tot = set(), 0
    for (rva, blob) in img.code_blobs():
        for off in range(0, len(blob) - 4095, 4096):
            pg = blob[off:off + 4096]
            if not any(pg):
                continue
            tot += 1
            if entropy(pg) > ENC_ENTROPY:
                bad.add(rva + off)
    return bad, tot


TIERS = ["exact32", "shape-exact", "shape-fuzzy", "caller-anchor", "shape-AMBIGUOUS", "not found"]


def tier_of(r):
    return "not found" if r is None else r["method"]


def report_tiers(title, results):
    c = Counter(tier_of(r) for r in results)
    n = max(1, len(results))
    print("\n%s (%d)" % (title, len(results)))
    for t in TIERS:
        if c[t]:
            print("  %-16s %5d  %5.1f%%" % (t, c[t], 100.0 * c[t] / n))
    return c


def inversions(pairs):
    """pairs = [(old_rva, new_rva)]; fraction of adjacent (by old order) pairs whose new order is reversed"""
    pairs = sorted(pairs)
    if len(pairs) < 3:
        return 0.0, len(pairs)
    inv = sum(1 for i in range(len(pairs) - 1) if pairs[i][1] > pairs[i + 1][1])
    return inv / (len(pairs) - 1), len(pairs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old-image", required=True)
    ap.add_argument("--new-image", required=True, nargs="+")
    ap.add_argument("--registry", default=os.path.join(REPO, "docs", "investigation", "v1074_offset_registry.json"))
    ap.add_argument("--no-registry", action="store_true", help="skip the verified-entry section (calibration on unrelated images)")
    ap.add_argument("--sample", type=int, default=1500)
    ap.add_argument("--min-instr", type=int, default=10)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()

    old = load_image([a.old_image])
    new = load_image(a.new_image)
    print("old: %s  stamp 0x%08X   new: %d dump(s) merged, stamp 0x%08X" % (os.path.basename(a.old_image), old.stamp, len(a.new_image), new.stamp))

    bad_old, to = encrypted_pages(old)
    bad_new, tn = encrypted_pages(new)
    eo, en = len(bad_old), len(bad_new)
    print("encrypted-looking code pages (entropy>%.1f): old %d/%d (%.1f%%)   new %d/%d (%.1f%%)"
          % (ENC_ENTROPY, eo, to, 100.0 * eo / max(1, to), en, tn, 100.0 * en / max(1, tn)))

    oi = sk.Index(old)
    loc = sigscan.Locator(new)
    ratio = len(loc.idx.funcs) / max(1, len(oi.funcs))
    print("functions detected: old %d, new %d (%.0f%% of old)" % (len(oi.funcs), len(loc.idx.funcs), 100.0 * ratio))
    guard = []
    if ratio < 0.7:
        guard.append("new image yields far fewer functions than the old one: SUSPECT THE FUNCTION-START SWEEP (alignment fallback in "
                     "sigkit.sweep) or an under-decrypted dump BEFORE reading any hit-rate as 'codegen changed'")
    if tn and en / tn > 0.15:
        guard.append("%.0f%% of the new code pages look encrypted: capture more (in-world, after using features) and pass all dumps" % (100.0 * en / tn))

    # ---- (1) verified registry code entries, by name
    if not a.no_registry:
        reg = json.load(open(a.registry, encoding="utf-8"))
        ver = []
        for e in reg["entries"]:
            if e["kind"] == "code" and e["status"] == "verified" and e.get("fp"):
                ver.append((e, loc.by_fp(e["fp"], e.get("callers"))))
        report_tiers("verified registry code entries", [r for _, r in ver])
        for e, r in ver:
            print("    %-34s %-16s %s" % (e["name"], tier_of(r), "" if not r else "0x%X (%+#x)" % (r["rva"], r["rva"] - e["rva"])))

    # ---- (2) sample of distinctive functions from the whole old image
    rng = random.Random(a.seed)
    pool = [f for f in oi.funcs if f.n >= a.min_instr]
    rng.shuffle(pool)
    res, pairs, skipped_enc = [], [], 0
    for f in pool[:a.sample]:
        # a function on a ciphertext page in the OLD image (Themida-protected / never decrypted) cannot be signatured by
        # construction: count it as a known cost, and read the general hit-rate off everything else
        if (f.start & ~0xFFF) in bad_old:
            skipped_enc += 1
            continue
        fp = rv2.fp_of(old, oi, f)
        if not rv2.distinctive(fp):
            continue
        r = loc.by_fp(fp, None)
        res.append(r)
        if r and not r.get("low"):
            pairs.append((f.start, r["rva"]))
    c = report_tiers("random sample of distinctive functions from the old image", res)
    print("  (excluded from the sample: %d functions on ciphertext-looking old pages - the protected subset, a known cost that "
          "needs live re-derivation regardless)" % skipped_enc)
    inv, npairs = inversions(pairs)
    print("order-inversion rate among %d relocated functions: %.1f%%   (true relocations ~0%%; a wrong-match cascade is high)" % (npairs, 100 * inv))
    dcl = Counter(((n - o) >> 12) for o, n in pairs)
    print("delta clusters (4 KB units), top: %s" % dict(dcl.most_common(5)))

    # ---- verdict
    n = max(1, len(res))
    ex = 100.0 * c["exact32"] / n
    ok_shape = 100.0 * (c["exact32"] + c["shape-exact"] + c["shape-fuzzy"] + c["caller-anchor"]) / n
    print("\n=== REGIME ===")
    for g in guard:
        print("GUARD: " + g)
    if ex >= 60:
        v = "A - same codegen (exact masked-byte prefixes survive). Lean on the registry; verify each proposal live."
    elif ex >= 30 or ok_shape >= 60:
        v = "A/B - mixed: bytes moved but the instruction stream is largely intact (or partial coverage). Use shape+caller anchors; expect a live-derivation tail."
    else:
        v = "B - codegen changed. Signatures are a weak guide (single-site globals ~coin flip): fall back to live re-derivation + semantic/correlation methods."
    print("exact32 %.0f%% of sample, any tier %.0f%%  ->  %s" % (ex, ok_shape, v))
    if guard:
        print("(guards fired: do not act on this verdict until they are resolved)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
