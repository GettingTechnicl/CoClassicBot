# Current-HP read investigation (2026-09-18)

## The bug

The bot spams HP potions constantly with `usePotions` enabled, even at full health,
because `CHero::GetCurrentHp()` always returns `0`.

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

## Also added: a throttled live diagnostic log line

`BaseHuntPlugin::RefreshRuntimeState` ([base_hunt_plugin.cpp](../../src/plugins/base_hunt_plugin.cpp),
tagged `[hp-diag]`, 2s-throttled) now logs `GetCurrentHp()`, the raw `m_pStatTable` pointer,
`GetValue(1)`'s raw return, `GetMaxHp()`, and the computed `hpPercent` every ~2 seconds while
a hero is loaded. This is a **temporary confirmation aid, marked `[HP RE 2026-09-18, TEMP]`
in the code** — remove it once the real fix lands and is verified. It exists purely to give a
plain before/after log trail matching on-screen HP without needing a separate debug build.

## Next steps (not yet done)

1. Live: with the on-screen HP visible, click "Dump Stat BYTES" at a few different HP states
   — full HP, after taking damage, after a potion/regen tick — and diff the two CHero windows
   above for the dword that tracks the on-screen value in both directions. This is the same
   correlation technique that found the level field (see CRole.h's +0x6E8 comment) and
   stamina.
2. Once a candidate offset is found, validate it across several more damage/heal/regen
   cycles before trusting it (per the standing "verify test target before trusting a
   negative/positive" lesson — a single lucky-looking match isn't enough, see
   coclassicbot-workflow-feedback memory).
3. Only then: propose swapping `CHero::GetCurrentHp()` to read the new direct field (mirroring
   how `GetMaxHp()` reads `m_nMaxHp`), and get explicit go-ahead before implementing — this
   project's standing rule is propose-then-wait, not fix-on-confirmation
   (coclassicbot-confirm-before-fixing memory).
4. After the real fix ships and is confirmed, remove the temporary `[hp-diag]` log line.

## Status

**Diagnosis confirmed** (current HP always reads 0, root cause identified, no live test
needed to establish this much). **Offset search not yet done** — needs a live session with
someone watching on-screen HP. Do not re-derive the diagnosis above from scratch next
session; pick up at "Next steps" step 1.
