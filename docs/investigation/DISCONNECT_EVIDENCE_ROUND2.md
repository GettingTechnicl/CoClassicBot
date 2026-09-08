# Trojan disconnect — round 2 evidence + next-step instrumentation

For the other worker. Read-only analysis of the two instrumented crash logs (coclassic_32348 =
Kinux, died 19:37:19.957 ~13 min in; coclassic_5312 = Shooter411, died ~20:51 after ~87 min),
2026-09-03. No behavior changed. This refines — and partly overturns — the earlier rate hypothesis.

## First: the per-action detail I went looking for is NOT in these logs
Both logs are at Warning level, so the individual `[path] Jump` / `[hunt] JUMP` / per-target
lines (debug/info) were filtered out. Only the new `[actionrate]` per-second aggregate survived.
So "look at individual jump distances / target ids in these two crashes" is impossible from the
captured data — the detail was never written. That is itself the key process finding (see the
instrumentation fix at the bottom). What follows is what the aggregates CAN still tell us.

## The finding that moves the needle: it died at a LOWER rate than it repeatedly survived
- Kinux: died with jumpTiles/s = 40 (last tick). Its healthy max earlier the same session was
  jumpTiles/s = 90. packets/s died at 10 (max 13), jumps/s died at 3 (max 5).
- Shooter: died around jumpTiles/s ~54. Its healthy max was jumpTiles/s = 108.
So both characters spent time at ~2x the teleport rate they died at, with no disconnect, and then
died during ordinary mid-range activity. This is stronger than "no spike before death" — the crash
happened at rates the bot had already proven the server tolerates. That substantially undercuts the
movement/action-rate hypothesis in every form these aggregates can measure (per-second spike AND
"one huge teleport" — jumpTiles/s never spiked at either death, so there was no giant single jump
in the final second either).

I (the read-only reviewer) led with the rate theory at high confidence in the first report. This
round it is on very thin ice. I'm flagging that plainly rather than defending it.

## Other things the aggregates show
- The connection pointer (conn=0x...) is stable and unchanged right up to the last actionrate tick,
  then instantly unresolvable. The death is abrupt — zero warning ramp.
- Roles are running 100% on the heap scan: `role registry read FAILED` counts hit fails=10217
  (Kinux, 13 min) and fails=65625 (Shooter, 87 min). Every role read falls back to the shape-based
  heap scan, so phantom/ghost targets are possible on every tick. This is the one live mechanism
  the aggregates cannot see (see below).

## Revised hypothesis ranking (2 events, still small)
- DEMOTED, near-dead: action/movement RATE (jump tiles/sec, jumps/sec, packets/sec). Died below
  survived peaks; no spike; no giant teleport. The aggregates were built to test exactly this and
  came back negative twice.
- ELEVATED, co-lead:
  1. A DISCRETE INVALID ACTION the aggregates can't see — most plausibly attacking / interacting
     with a PHANTOM (ghost) entity. Roles are all heap-scan (10k-65k registry fails), so a freed
     monster/NPC can linger as a target; an attack/interact packet to an id the server considers
     invalid or out-of-range is a clean discrete-kick candidate, and it is NOT a movement event so
     it never touches jumpTiles/s. Fits the "died mid-ordinary-activity, abruptly" shape and the
     13-min vs 87-min spread (a rare bad action at random times, not a threshold).
  2. EXTERNAL / server-side network drop unrelated to bot action. Now genuinely plausible precisely
     because the bot was doing nothing unusual at either death. Can't be confirmed or excluded from
     current logs. Do not dismiss it — some fraction of these may just be the connection dropping.
- The 13-min vs 87-min spread argues against any deterministic threshold (rate or cumulative time)
  and toward a stochastic/discrete trigger — consistent with either co-lead above.

## Why the current instrumentation can't decide it, and the fix to propose
The `[actionrate]` aggregate structurally cannot see a single bad action (it's one packet inside
packets/s=10) or a ghost-target attack (not a movement). To test the surviving hypotheses without
125-lines/sec Trace spam, propose a "flight recorder":
- A small in-memory ring buffer (say last 30-50 actions). Each entry: tick, action type
  (jump/attack/pickup/npc), target id, target pos, distance, and a bool "target id present in the
  latest entity/item scan" (the phantom flag).
- Dump the whole buffer to the log at Warning ONLY when SendPacket first hits the null connection
  (the exact moment already logged). That yields the precise final actions before every future
  disconnect, ghost-flagged, with near-zero steady-state log cost.
- Add one running counter to the per-second actionrate line: attacks/interacts this second whose
  target id was NOT in that tick's scan (phantom actions/sec). If that is > 0 in the seconds before
  disconnects but ~0 otherwise, the phantom theory is confirmed; if it's 0 right before a death,
  that death points at external/network.
- Optional, heavier: observe inbound via the existing own-winsock hook approach (netfinder-style)
  to see whether a server close/kick packet arrives — distinguishes server-initiated drop from
  anything bot-side. Only if the flight recorder is inconclusive.

## Posture
- This is an INSTRUMENTATION change, not a behavior change (low risk), but per the user's stance:
  propose it and get the go-ahead before implementing, and keep bot behavior untouched.
- Keep collecting disconnects overnight with what's already running; 2 events only redirected the
  theory, they can't conclude it.
- For the A.M. review: the headline is "rate theory undercut by its own data (died below survived
  peaks); phantom-target and external-network are now the live leads; current logging can't tell
  them apart, so the next step is the flight-recorder instrumentation, not another behavior guess."
