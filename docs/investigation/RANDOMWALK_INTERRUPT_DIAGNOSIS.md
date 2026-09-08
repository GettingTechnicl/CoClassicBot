# The ~6s "Path interrupted externally, replanning" — root cause found

Read-only trace of `src/plugins/base_hunt_plugin.cpp` + `src/CHero.cpp`, 2026-09-02. Nothing modified.

## What it is
The idle "random walk" wander runs in **every** hunt state, including all the travel/town
states — so every ~6 seconds it stops the travel pathfinder and issues a random 1–2 tile hop,
which is the `Path interrupted externally, replanning` the log shows.

## First, a red herring to clear
The log line `[walk] SetCommand iType=15 idTarget=154` (CHero.cpp:292) has those two numbers
**hardcoded into the format string** — they are not telemetry. (`iType=15` is `_COMMAND_WOUND`
in the enum, nonsense for a walk.) The line just means "`CHero::Walk()` ran." Don't chase 154.

## The chain (all proven in code)
1. `BaseHuntPlugin::Update()` calls `TryRandomWalk(hero, map, settings, now)` at
   **base_hunt_plugin.cpp:2330 — unconditionally, near the top, BEFORE the state switch.**
   All the state handlers (`TravelToBlacksmith` @2494, `TravelToMarket` @2489, `StoreItems`
   @2527, `BuyArrows` @2514, `Recover` @2505, `Repair` @2509) run *after* it. So TryRandomWalk
   fires in **every** state, town/travel included.
2. `TryRandomWalk` (base_hunt_plugin.cpp:1736) has **no state guard and no pathfinder-active
   guard**. It fires whenever `now - m_lastRandomWalkTick >= randomWalkIntervalMs`.
   `randomWalkIntervalMs` default = **5556 ms**, + jitter ≈ the "~6 s" observed. It then calls
   `StartWalkTo(hero, map, <random 1-2 tile hop>, 0)`.
3. `StartWalkTo` (base_hunt_plugin.cpp:1778) does the damage:
   ```cpp
   if (Pathfinder::Get().IsActive())
       Pathfinder::Get().Stop();     // kills the in-progress travel path
   ...
   hero->Walk(bestPos.x, bestPos.y); // random hop
   ```
   It **stops the active pathfinder and walks off-route.** The travel plugin's pathfinder then
   detects the interruption and logs `Path interrupted externally, replanning`.

## The tell: the code contradicts its own comment
`TryRandomWalk`'s comment (base_hunt_plugin.cpp:1753) says StartWalkTo
*"won't fire while the hero is jumping or the pathfinder is mid-route."*
For jumping, true (`if (hero->IsJumping()) return true;`). For the pathfinder it's the
**opposite** — StartWalkTo *stops* an active path instead of bailing on it. So the intended
behavior is already documented; the code just doesn't honor it for the pathfinder case.

## Why this matters beyond the arrow run
This isn't only the blacksmith-travel waste. It very likely also feeds the **MillionaireLee
approach failure** that took ~6 rewrites: the other LLM saw this exact interrupt "near Lee at
00:37," and the earlier Lee saga described the hero "bouncing between tiles for 90+ s without
converging" next to the NPC. A random 1–2 tile hop every ~6 s while trying to settle onto an
NPC-adjacent tile in a crowd produces exactly that non-convergence. So the movement half of the
Lee problem may be this same bug — separate from the `IsNpcActive` gate on the *answer* half
(see MILLIONAIRELEE_DIAGNOSIS.md). Worth re-testing Lee once this is fixed; some of the bespoke
approach logic that got added and reverted may have been fighting this, not a real pathing gap.

## Fix direction (for whoever edits — I changed nothing)
Make the wander honor its own contract: it should only run while **free-roam hunting**, never
during the town/travel FSM states, and never on top of an active pathfinder route.
- Simplest, lowest-risk: guard the **call site** at 2330 so `TryRandomWalk` is only called when
  `m_state` is an idle-hunting state (e.g. `Ready`/`AcquireTarget`), not any of
  `TravelToZone/ReturnToZone/TravelToMarket/TravelToBlacksmith/Repair/BuyArrows/StoreItems/Recover`.
- Belt-and-braces: also early-return from `TryRandomWalk` (or from `StartWalkTo`'s random-hop
  use) when `Pathfinder::Get().IsActive()` — i.e. **bail** instead of `Stop()`, matching the
  comment. Be careful: other legit callers of `StartWalkTo` may *want* to preempt the pathfinder,
  so prefer gating the random-walk path specifically rather than changing StartWalkTo's Stop()
  for everyone.
- Verify with a capture: after the fix, a full blacksmith/market trip should show zero
  `Path interrupted externally, replanning` lines, and travel time should drop.
