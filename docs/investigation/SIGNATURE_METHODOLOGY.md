# Offset-registry signatures: methodology, measured survival, caveats

Status 2026-09-21. Tools: `tools/sigkit.py` (primitives), `tools/registry_v2.py` (fingerprint builder),
`tools/build_offset_registry.py` (generator), `tools/sigscan.py` (locator for a new image),
`tools/sig_validate.py` (cross-build validator). Registry: `v1074_offset_registry.json` / `V1074_OFFSET_REGISTRY.md`.

## 1. What is measured, and what is not

The first version of the registry was checked only by **identity**: run the scanner on the same image the
signatures were built from. That proves the anchors are internally consistent (necessary) but says nothing about
whether they survive a rebuild (sufficient). It is kept only as a sanity gate (`sigscan.py --identity`, 19/19).

The real question is *relocation hit-rate across a build change*, so it was measured on a pair with **known ground
truth**: our own `coclassic.dll` built from two commits (`208b3e3` vs `HEAD`) with linker `/MAP` files. The map gives
every function/global's true address in both builds. Only functions from translation units whose source did **not**
change are sampled (so "the code is the same function" is known). This validates the *method* on a compiler-realistic
pair. **It is not the game**: the game is Themida-packed, and its real v1074->v1078 delta (compiler, flags, source
churn) is unknown until the v1078 decrypted image exists. Two regimes bracket the outcome:

| regime | how built | what it models |
|---|---|---|
| A. same compiler + flags, source churn | commit `208b3e3` -> `HEAD`, both `/O2 /Ob2`, changed TUs excluded | a routine content patch |
| B. codegen change | same source, `/O2 /Ob2` -> `/O1 /Ob1` | worst case: different inlining, alignment, register allocation |

## 2. Signature design (why it looks the way it does)

* **No signature is grown until it is unique.** A longer byte run is more specific to one build and *less* likely to
  survive a recompile. Every function instead carries several recompile-stable features: a fixed 32-byte masked prefix
  (rel32 call/jmp targets, rip-relative displacements and absolute image pointers masked), a normalised
  instruction-**shape** stream (mnemonic + operand classes; register *size* not identity; jcc collapsed; small
  immediates collapsed, distinctive immediates >= 0x1000 kept), distinctive immediates, referenced strings, callee
  count. A **distinctiveness** score (similarity of the most similar *other* function in the same image) is stored.
* **Non-distinctive functions** (MSVC magic-static accessors such as `0xB7320`, template instantiations) are anchored
  through their **callers**: a distinctive caller plus the ordinal of the call to the target. Sibling globals are
  disambiguated the same way (function-anchored + reference ordinal), never by widening a window.
* **Locating a function** is a cascade: exact32 (only if unique in the old build) -> shape match with a required margin
  over the runner-up -> caller anchor. Every result reports its method and score.
* **Globals** are read out of the code that references them (several sites, each with the enclosing function's
  fingerprint + ordinal). `CURRENT_MAP_ID+4` style derived entries stay derived from their base.
* **Struct fields (177)** each carry a re-finding *strategy*, not just an offset:
  * `code-access` (107): up to 5 access sites, each a +-3 window of normalised instructions inside an anchored
    function; the scanner finds the window in the new build and **reads the new displacement back** from the
    instruction. Sites vote; disagreement rejects. This covers the 45 `static_assert(offsetof)` fields too.
  * `live-correlation` (70): the code is too generic to anchor on. The entry carries a recipe (dump the owner object at
    several states and rank-correlate against an on-screen quantity - the method that found current HP, see
    `CURRENT_HP_READ_INVESTIGATION.md`). Neighbours in the same struct usually shift by the same delta; the scanner reports
    per-struct delta consistency and marks interpolated values as **hypotheses**.

## 3. Measured results (tools/sig_validate.py; ground truth from /MAP)

Function relocation, sampled from unchanged TUs:

| method | A: correct / wrong / miss | B: correct / wrong / miss |
|---|---|---|
| exact32 (usable when unique in old build) | 98.6% / 0.1% / 1.2% | 17.7% / 1.4% / 80.6% |
| exact64 | 97.1% / 0.1% / 2.8% | 13.6% / 0.4% / 86.0% |
| shape alone | 84.9% / 10.0% / 0.2% | 23.5% / 5.0% / 70.4% |
| **cascade (exact32 > shape+margin)** | **88.0% / 0.1% / 11.9%** | **24.2% / 3.5% / 72.3%** |

Globals through referencing code:

| policy | A: correct / wrong | B: correct / wrong |
|---|---|---|
| single located site | 88.2% / 3.1% | 8.8% / 11.4% (80% not located) |
| majority vote | 97.5% / 2.5% | 48.6% / 45.9% |
| **only when >= 2 sites agree** | **34.1% / 0.1%** (rest abstain) | **6.2% / 0.5%** (rest abstain) |

Struct-field access sites re-found (window search inside the located function, displacement read back):
A: 84.2% correct / 2.8% wrong / 8.4% not located; B: 11.7% correct / 0.5% wrong / 86.9% not located.

### What this means

1. **Regime A (same toolchain): the method works.** Functions relocate ~88% with ~0.1% wrong; globals and fields
   are largely recoverable. The exact-prefix tier alone does most of the work (it is why exact32 is first).
2. **Regime B (codegen change): recall collapses, and one thing gets dangerous.** Functions still relocate with low
   false-positive rate, but a *single* located site for a global is close to a coin flip (majority vote 46% wrong).
   Therefore confidence is tied to **agreement**, not to method: `sigscan.py` labels a global "high" only when >= 2
   sites agree (0.1% / 0.5% measured wrong in A / B), otherwise `candidate - verify live`; conflicting votes are
   rejected. Nothing this tool emits is ever trusted without the in-DLL self-test and a paired-dump check.
3. Which regime the game is in is **unknown** until the v1078 image is decrypted. Cheap early signal once we have it:
   run `sigscan.py` and look at the exact32 hit-rate on the verified functions. High (~90%) => regime A, proceed
   with signatures. Low => regime B: fall back to live-correlation for fields and anchor globals through strings/vtables.

### Bug found by the stress test

The function-start sweep required 16-byte alignment after int3 padding; `/O1` builds are not 16-aligned, so only 907
of ~3300 functions were detected. `sigkit.sweep` now falls back to `align=1` when detection density is implausibly low.
Worth knowing for v1078: if the sweep reports far fewer functions than v1074's ~15.7k, suspect alignment first.

## 4. Caveats (read before believing any number here)

* Proxy pair, not the game. Different compiler heuristics, no Themida, and our DLL has 3-4k functions vs the game's ~15k.
* Sample bias: "unchanged TU" excludes exactly the code a real patch is most likely to change. Real hit-rate on
  *patched* functions will be lower; there the recourse is caller anchors, strings, and live correlation.
* The "correct" ground truth for fields is "displacement unchanged in an unchanged TU" - true by construction, so it
  measures *re-finding*, not whether a real patch moved a field.
* Thresholds (`DISTINCT_BELOW`, `MARGIN`, `SHAPE_SANITY`) were set before/independently of these runs and only the
  vote policy was changed in response; re-tune against the real pair, never against identity.

## 5. Reproduce

```
# build both commits with linker /MAP (CMAKE_SHARED_LINKER_FLAGS=/MAP); vendor/ is untracked so copy it in
python tools/sig_validate.py --old-dll OLD/coclassic.dll --old-map OLD/coclassic.map \
    --new-dll NEW/coclassic.dll --new-map NEW/coclassic.map --changed <stems of changed .cpp files>
python tools/build_offset_registry.py            # regenerate the v1074 registry (about 1 minute)
python tools/sigscan.py --image <decrypted_v1078.bin> --fields --status verified
```
