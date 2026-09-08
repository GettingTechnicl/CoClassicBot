# Trojan disconnect investigation — findings + action plan

For the other worker. Read-only analysis of the crash logs (coclassic_10224 / _34792 / _5776
plus the wider set carrying the disconnect signature), src/packets.cpp, src/hunt_intervals.h,
2026-09-03. Nothing was edited.

## READ THIS FIRST — process discipline
The user is explicitly frustrated with "diagnose, then silently apply a fix." This is a DIAGNOSIS
plus a PLAN. Do not change bot behavior off the back of it. Propose specific values/changes and the
measurement, get the user's go-ahead, THEN implement. Not optional here.

## Verdict: the earlier "loot-freeze -> AFK kick" diagnosis does not fit the evidence
The crash logs do NOT show a multi-minute freeze before the disconnect. In all three sessions I
reconstructed, the bot is ACTIVELY hunting and jumping in the split-second before the connection
dies, and the process keeps running afterward (entity scans continue for seconds). So:
- It is a SERVER-SIDE disconnect (the game's own connection object goes null), NOT a crash in our
  code, NOT memory, NOT the loot loop.
- The (0,0) position, GetMaxHp/GetMaxMana=0, and "A* found NO ROUTE" are DOWNSTREAM fallout of the
  already-dead connection (the hero object zeroes out as the session tears down), not the cause.
  The earlier report stated this correctly in one line, then contradicted it with the AFK theory.

The loot dist==0 escape-hatch already added is a fine standalone bug fix (that loop is real) but is
almost certainly unrelated to these disconnects. Do not report it as the disconnect fix, and don't
treat crash frequency changing as validation of it.

## What the logs actually show (3 of 3)
Terminal sequence, identical each time, under 1s from normal play to dead connection:
  [path] Jump ... dist=18                                     <- mid-action
  [safety] CHero::GetMaxHp/GetMaxMana ... returning 0         <- hero stats zero (fallout)
  [packets] SendPacket: failed to resolve connection object   <- game connection pointer now null
  [portal] ... -> (0,0)  /  [path] No progress at (0,0)       <- position zeroed (fallout)
  [entities] scan #N ... (still running)                      <- PROCESS ALIVE; server dropped us

- 10224 (alone, map 1075, traversing): chaining 18-tile packet-jumps every ~200-400 ms — 7
  consecutive max-distance teleports in the final ~2 s; the disconnect lands on the 7th. Memory
  normal (~525 MB).
- 34792 (map 1020, a PLAYER in view, paranoia fleeing): rapid combat jumps (dist 1/7/8),
  disconnect mid-approach. Memory normal (~475 MB).
- 5776 (map 1020, player in view, paranoia): rapid combat jumps (dist 5/8), disconnect mid-combat.
  Memory elevated (~1.1 GB) — but the other two were normal, so memory is not the common factor.

Common factor across all three: HIGH-FREQUENCY movement/action packets right up to the kick. Not
distance specifically (2 of 3 had short/capped jumps), not player-presence (1 of 3 was alone), not
memory (2 of 3 normal), not a freeze (0 of 3).

## Leading hypothesis: server-side movement/behavior rate validation ("speedhacking too hard")
The bot runs at superhuman action rates and the disconnect correlates with that intensity:
- usePacketJump=1 — movement is teleport-jumps, not walking.
- GetJumpDistanceCapTiles returns MAX_JUMP_DIST (18) UNCAPPED whenever no player is nearby
  (hunt_intervals.h:251, comment: "no cap — full efficiency when alone"). The jump cap exists to
  avoid PLAYER WITNESSES, not to stay under the SERVER's movement validation. The server does not
  care whether a human sees it.
- kMinMovementIntervalMs = 100, and cyclone attack intervals as low as 25-119 ms
  (cycloneAttackIntervalMs) — under aggressive/speedhack the action loop fires ~10x/sec.
- Net: bursts of ~18-tile teleports several times a second plus very fast attack packets = a
  movement/packet rate no legitimate client produces. Conquer servers commonly validate movement
  distance/rate server-side and drop offenders. Fits the mid-jump-burst timing exactly.

### The "worse with logging off" clue supports this
The logger uses flush_on(debug), and the hunt loop logs a debug line on every jump, so at Trace
level each jump triggers a SYNCHRONOUS disk flush — inadvertently throttling the action cadence.
With logging OFF (last night) that throttle is gone -> the bot hits its true aggressive rate ->
disconnects every 5-10 min. With Trace back ON now it is I/O-throttled -> the current session has
held 33+ min / 5,400 kills without a disconnect. That is a real (accidental) natural experiment
pointing at action-rate as the trigger. It also means "it got better when I turned logging on" is
NOT evidence any code fix worked — it is the I/O throttle.

Confidence: HIGH that it is a server-side rate/movement kick; MEDIUM on the exact variable (jump
distance-per-second vs jump rate vs total action-packet rate). Cannot fully exclude the anti-cheat
detecting the client another way, but the action-rate + logging correlation and the
"after-minutes, mid-action, intermittent" pattern favor movement validation over static detection.

## Where the user's phantom-target observations fit (secondary, but real)
The user sees the bot "attacking a monster" with nothing in range (then moving on ~10 s later) and
"jumping back and forth to a meteor 2-3 times." These are almost certainly STALE HEAP-SCAN GHOSTS:
roles are still enumerated by heap scan (by design — the registry path was refuted), so a freed
monster/NPC can linger as a phantom target; the bot commits jumps toward it, attacks a dead id,
times out, moves on. The meteor ping-pong is the same class (ghost item, or a real-but-unreachable
item hitting the dist==0 loot loop).
- Genuine bug worth fixing on its own: validate the current target/loot id is still present in the
  FRESH entity/item snapshot before committing a jump; drop it immediately if not.
- Also a RATE AGGRAVATOR: extra phantom jumps add to the movement-packet rate, feeding the primary
  disconnect cause. Fixing them helps two things at once — but they are not the primary trigger.

## The plan (measure -> propose -> confirm; do not fix blind)
1. Instrument, don't change behavior yet. Add lightweight logging (must survive at Warning level)
   that once per second records: jumps-in-last-second, total tiles teleported-in-last-second,
   attacks-in-last-second, and the connection-object pointer value. Run until a couple of
   disconnects occur. Goal: measure the action-rate profile in the final ~30 s before each kick vs
   steady state, and confirm the connection goes null with no prior stall.
2. Confirm the rate link with a controlled test the user signs off on: propose a concrete throttle
   — e.g. cap jump distance even when alone (~10-12 tiles), raise min movement interval
   (~250-350 ms), and/or slow the cyclone attack interval — and run with logging at Warning (so
   I/O is NOT the throttle). If disconnects stop at Warning + throttle, the rate hypothesis is
   confirmed and the throttle IS the fix. Present the specific numbers to the user BEFORE applying;
   let them pick the aggressiveness/uptime trade-off — their call, not ours.
3. Fix phantom targets in parallel (also propose first): before any committed jump toward a monster
   or loot id, require that id to be in the latest snapshot; abandon on miss instead of re-jumping.
4. Re-evaluate the loot escape-hatch already added: keep it (harmless, fixes a real loop) but don't
   credit it with the disconnect behavior either way.

## What NOT to do
- Don't leave logging at Trace as the "fix" — it only masks the problem via I/O throttle and buries
  diagnostics in ~125 lines/sec.
- Don't conclude a change worked from one session — the natural disconnect interval is 5-10 min, so
  a "good" 33-min run is only ~3-6 intervals; watch a longer window.
- Don't change behavior without the user's explicit go-ahead on the specific values.
