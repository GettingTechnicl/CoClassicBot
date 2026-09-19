# Current-HP read investigation (2026-09-18) — RESOLVED, fix shipped

## Status: fixed and live-confirmed

`CHero::GetCurrentHp()` now reads the real value (see "The fix" below). Live-verified against
on-screen HP continuously during play, including through normal combat/regen — not just a
one-shot check. **Caveat for full rigor**: a dedicated relog + map-transition check (does the
pointer chain survive a fresh login / map change, not just staying alive within one session)
was not separately isolated before shipping; the fix's own hardening (SEH guards + a
plausibility check + a `-1`/"unknown" sentinel instead of `0` on any failed read, see below)
is exactly what makes that an acceptable risk to ship ahead of that specific test rather than
something that could quietly reintroduce the original spam bug — a bad read now fails safe.
If a relog/map-change ever produces a visible "HP: unknown (read failed)" in the Debug tab,
that's expected-and-handled, not a regression; a sustained stretch of it would be worth a
fresh look.

## The fix

`CHero::GetCurrentHp()` ([CHero.cpp](../../src/CHero.cpp)) reads a **direct value two pointer
hops from `m_pStatTable`**: `*(int32*)(*(uintptr_t*)((char*)m_pStatTable + 0x10))`. Found via
live rank-correlation across 18 samples spanning two play sessions (see "How it was found"
below). Every hop is SEH-guarded; a null/invalid pointer, a failed read, or an implausible
result (negative, or more than `GetMaxHp() + 50` — see `kHpPlausibilityMargin`) all return
**`-1` ("unknown"), never `0`**. `HuntBuffManager::TryUsePotions()`
([hunt_buffs.cpp](../../src/hunt_buffs.cpp)) treats a negative HP as "skip this tick's potion
decision" — this is the load-bearing part of the fix: the original bug was exactly "bad read
→ 0 → treated as real → spam," so a naive fix that still defaulted to 0 on failure would have
reintroduced the same bug under different conditions (a transient bad read instead of an
always-bad one). A live HP readout (`HP: current / max (pct%)`, or "unknown (read failed)" in
orange) was added to the overlay's **Automation → Hunting → Debug** tab for at-a-glance
verification, superseding the temporary throttled log line this investigation used earlier.

Mana is a known, separate follow-on: `GetCurrentMana()` almost certainly has the identical bug
(same dead native-accessor shape, same potion-spam risk via `mpPercent`), not yet
investigated or fixed — see "Next steps" below.

## The bug (original symptom)

The bot spammed HP potions constantly with `usePotions` enabled, even at full health,
because `CHero::GetCurrentHp()` always returned `0`.

## Confirmed facts (static code reading, no live test needed for this part)

- `CHero::GetCurrentHp()` ([CHero.cpp:1215](../../src/CHero.cpp)):
  `return m_pStatTable ? m_pStatTable->GetValue(1) : 0;`
- `CStatTable::GetValue()` ([CStatTable.cpp:4](../../src/CStatTable.cpp)) is gated on
  `GameRva::VERIFIED_V1074`, which is currently `constexpr bool ... = false`
  ([game.h:89](../../src/game.h)). When that flag is false, `GetValue()` **unconditionally
  returns 0 without ever calling the native accessor** — it's not that the native call was
  tried and returned garbage, it's disabled by design as part of the project's broader
  "only re-verified offsets get live-called" safety posture (see game.h:80-83).
- Two prior-session comments (base_hunt_plugin.cpp:1129, :4444, "Session 15 [KILL-SIGNAL RE]")
  record that the native getter itself was separately tested live and confirmed to return 0 on
  v1074 even when actually invoked — so both the gate AND the underlying native function are
  dead for this purpose, not just the gate.
- `CHero::GetMaxHp()` ([CHero.cpp:1229](../../src/CHero.cpp)) works correctly by contrast: it
  reads `m_nMaxHp`, a **direct CHero field at +0x3D0** ([CRole.h:194](../../src/CRole.h)), only
  falling back to the same dead gated path if that direct field reads `<= 0` (a branch that's
  otherwise dead per its own comment).
- `HuntBuffManager::TryUsePotions()` ([hunt_buffs.cpp:363](../../src/hunt_buffs.cpp)):
  `hpPercent = lastMaxHp>0 ? (lastHp*100)/lastMaxHp : 100`. With `lastHp` always 0 and
  `lastMaxHp` correct, this is always 0, which is `<= hpPotionPercent` (default 50) every
  tick — the actual spam mechanism. `lastHp`/`lastMaxHp` are populated at
  `BaseHuntPlugin::RefreshRuntimeState` ([base_hunt_plugin.cpp:335-336](../../src/plugins/base_hunt_plugin.cpp))
  straight from `GetCurrentHp()`/`GetMaxHp()`.
- **`GetCurrentHp()` has exactly one caller in the whole codebase** (that
  `RefreshRuntimeState` line) — the entire "current HP" concept in this project currently
  flows through this single broken chain. There is no other existing current-HP source to
  fall back to.
- The entity heap-scan (`entities.h`/`entities.cpp`) does **not** already solve this: its own
  header comment says every scanned `CRole` (monsters, players, NPCs, the hero) has its
  "**maxHP** field reading correctly" — that's the same `m_nMaxHp` field CHero already uses
  correctly, not a separate current-HP field. No existing consumer in the codebase reads a
  current-HP value off a `CRole*` anywhere (`monster_scan.cpp` only ever touches `m_nMaxHp`).

## What's needed

A direct CHero field for current HP, the same pattern `m_nMaxHp` already uses — not another
attempt at the native `CStatTable::GetValue` call, which is confirmed dead twice over (gate +
underlying native function).

## Tooling already in place for the live memory-correlation scan

The overlay's Developer Tools panel (Map/Debug tab) has a "**Dump Stat BYTES (find HP/kills
offset)**" button (`base_hunt_plugin.cpp`, Session 15, extended 2026-09-18) that does two
raw, read-only SEH-guarded memory dumps to `coclassic.log` at `info` level, tag
`[statbytes]`:

1. **NEW (2026-09-18): `CHero+0x290` through `+0x720`** (292 ints) — brackets `m_nMaxHp`
   (+0x3D0) and `m_nStamina`/`m_nMaxStamina` (+0x6E0/+0x6E4), on the hypothesis that current
   HP is a direct CHero field adjacent to max HP, the same relationship stamina/max-stamina
   already has. **Printed offsets are relative to +0x290** — add `0x290` to a printed offset
   to get the true CHero offset (e.g. printed `+0x140` = CHero `+0x3D0`, which should read
   back as the known max HP value — useful as a sanity anchor when reading the dump).
2. **`CHero+0x800` through `+0xD00`** (320 ints, pre-existing) — the original Session 15
   window, covers the kill counter at +0xA30 and the stat-table pointer at +0x968, plus
   whatever else lives in that range.
3. Also dumps the **raw bytes of the `CStatTable` object itself** (`*statPtr(0x968)`, 64
   ints) — since the object is otherwise opaque (its native accessor is dead), this is the
   direct read of its actual memory contents.

There's also a "**Dump Stat Table (find kill counter)**" button that calls
`m_pStatTable->GetValue(i)` for `i in 0..63` — **this one is currently useless for this
investigation**: since it goes through the gated `CStatTable::GetValue()`, every value it
prints will be `0` regardless of `i`, per the confirmed-dead native path above. Don't read
anything into its output until/unless `VERIFIED_V1074` changes.

## How it was found

1. Widened the "Dump Stat BYTES" dumps (`CHero+0x290..+0x720`, `CHero+0x800..+0xD00`, and the
   raw `CStatTable` object) and, across 9 live HP states (960 down to 241), ran an exact-match
   search (int32/float32/packed-uint16) — clean negative everywhere. Current HP is not stored
   inline as a plain value in either window or in the first 0x800 bytes of the `CStatTable`
   object.
2. Re-ran as a scale/skew-tolerant rank correlation (Spearman) instead of exact match, to catch
   a percentage/fraction encoding or a few points of timing skew — still no credible signal in
   the direct windows (a handful of `|rho|>=0.85` hits all landed in a region that changes
   completely between samples in unstructured ways, consistent with reading adjacent
   heap noise rather than a real object field; discounted as multiple-comparisons false
   positives, not chased further).
3. Disassembling the native `GetValue()` to read its real logic was considered but ruled out:
   `game.h` (GameRva section) already documents `CSTATTABLE_GET_VALUE`'s RVA as **confirmed
   stale** ("resolves to misaligned garbage" post-v1074) — disassembling at that address
   wouldn't show the real function, it'd show unrelated shifted code.
4. Pivoted to following pointers: the raw `CStatTable` dump showed a repeating
   `[tag1][tag2][ptr]`-shaped record pattern (the two tags constant across every HP state —
   type/flag fields, not the value). Extended the dump tool to dereference every plausible
   pointer found in a widened (0x800-byte) `CStatTable` dump and log ~0x40 bytes at each
   target, bounded to 64 dereferences per click (per the project's standing
   bounded-injected-search rule).
5. Re-ran the same rank-correlation method against the dereferenced target dumps (11 more HP
   states, 19 up to 960). One record stood out: the pointer at `CStatTable+0x10` points to an
   object whose `+0x0` int32 matched on-screen HP exactly in 7 of 9 aligned samples, with the
   other 2 explained by timing skew (reading the screen, then clicking, a moment apart). That
   same target's `+0x1C` field is **exactly 128× its `+0x0` value in every single sample** —
   a same-instant structural relationship (not two independently noisy readings), which is
   what elevated this from "a correlation hit" to "trust it."

## Follow-on: mana likely has the same bug (not yet done)

`CHero::GetCurrentMana()` uses the same dead-native-accessor shape as the old `GetCurrentHp()`
did, and the mana-potion branch in `TryUsePotions()` mirrors the HP branch — same spam risk if
`autoMpPotion`/mana thresholds are enabled. The dereference tooling built for this
investigation (`DumpDereferencedRecords` in `base_hunt_plugin.cpp`) applies directly: dump/
correlate for the mana record the same way, then ship as its own separate change (not bundled
with the HP fix).
