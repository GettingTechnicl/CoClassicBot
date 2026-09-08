# Spawn-memory heatmap — improvements

For the other worker. Read-only review of `src/spawn_memory.{h,cpp}` + the call site
`src/entities.cpp:425-429`, 2026-09-03. Nothing edited.

## What's already good (don't touch)
Sparse per-map storage, scan-count (not wall-clock) decay, self-pruning below 0.05, the 30-day
staleness rail on load, and the novelty cap after the ping-pong incident. The structure is sound.
The issues below are about WHAT gets counted, not how it's stored.

## #1 (root cause, cheap fix): it counts DWELL TIME, not spawn events

`Observe(mapId, monsterTiles)` runs every entity refresh (~2/sec) and adds **+1 per currently
VISIBLE monster, with no notion of which monster it is** (spawn_memory.cpp:112-135). So a monster
that stands still — or that the bot is actively fighting — is counted **hundreds of times** while
it sits in view, while monsters the bot merely walks past are counted a few times each. The score
therefore measures "where the bot lingered with something on screen," NOT "where monsters
appear."

This is the actual engine of the "camps its known hot quadrant, never learns the rest" behavior.
The novelty boost (kNoveltyObserves/kMaxNovelBuckets) is a PATCH on this symptom — it only exists
because the raw score self-reinforces wherever the bot already fights. Fix the counting and most
of the reason for the novelty machinery goes away.

The fix is small because the id is already in hand at the call site. Today
`src/entities.cpp:425-429` does:
```
if (r && r->IsMonster() && !r->IsDead())
    monsters.push_back(r->m_posMap);   // r->GetID() is right here, discarded
...
SpawnMemory::Observe(curMapId, monsters);
```
Change `Observe` to take `{OBJID id, Position pos}` pairs and credit a bucket only when a monster
**id is newly seen** (an appearance), not every tick it remains:
- Keep a per-map `unordered_set<OBJID> presentLastObserve` (or `id -> bucketKey`).
- On each Observe: a monster whose id was NOT present last batch (or moved to a different bucket)
  scores +1 in its bucket; ids still sitting in the same bucket score nothing.
- Decay still runs every batch as it does now.
This converts the heatmap from a dwell map into a true appearance-rate map. Highest-value change,
a few lines in `entities.cpp` + a dedupe set in `Observe`. Session-local set, not saved.

## #2 (principled follow-on): density as a RATE, and "empty" vs "unknown" as distinct states

Right now bucket score 0 means EITHER "no monsters spawn here" OR "the bot has never been here" —
exploration can't tell them apart, which is the OTHER reason novelty had to be bolted on. Add a
second per-bucket counter: how many Observe batches this bucket was **within view**
(observation opportunities). Then:
- **density = spawns / timesObserved** — removes the bias where buckets near the bot's usual path
  accrue high raw counts merely from being seen more often.
- **timesObserved == 0 → explicitly "unknown, worth exploring"**, a first-class state rather than
  conflated with "empty."

Exploration then becomes the standard formulation: prefer high density; break ties toward the
least-observed in-zone bucket. With this in place the novelty queue (kMaxNovelBuckets, the
kNoveltyObserves window, the arming/expiry sweep) can largely RETIRE — it's hand-tuned machinery
approximating what density-plus-least-observed does directly and more robustly. Net complexity
goes DOWN, not up.

"Within view" = the set of buckets inside the entity scan radius around the hero at that Observe;
increment timesObserved for those, whether or not a monster was in them. That's what makes a
genuinely-searched-but-empty bucket distinguishable from an unvisited one.

## Minor notes (optional, low priority)
- **Bucket-edge splitting:** a hot spawn on an 8x8 boundary dilutes across up to 4 buckets, none
  looking as hot as the real cluster. Light smoothing (score a candidate as itself + a fraction
  of its neighbors) would prefer robust hot REGIONS over single-bucket spikes. Only worth it if
  hot spots feel like they're being under-ranked.
- **`Save()` holds `g_mutex` across the fwrite** (spawn_memory.cpp:252-269) every 2 min, blocking
  the scan and hunt threads for the write. Fine at a few thousand buckets; if it ever hitches,
  snapshot the maps under the lock then write outside it.
- **The "~8 minutes" decay comment** (spawn_memory.cpp:14-15) is cadence-specific. Decay is
  per-scan by design (correct — an idle client shouldn't forget), but the wall-clock half-life
  shifts with the refresh-interval slider and under aggressive speed. Doc caveat only.
- **`observations` is a monotonic int** used as the novelty clock; at ~2/sec it overflows int32
  after ~340 days of continuous scanning. Not a real risk, but if #2 retires novelty this clock
  mostly goes away anyway.

## Why this matters for leveling specifically
A level grind moves through MANY maps and many fresh areas. The dwell-time bias (#1) means each
new map over-weights whatever spot the bot first happened to fight in, and the explore logic
fights that with the novelty crutch. Fixing the counting makes "learn a new map quickly and
fairly" — exactly what leveling needs — work without the crutch. Do #1 before the grind if
there's time; #2 is the cleanup that lets you delete the novelty code afterward.

## Suggested order
1. #1 (id-based appearance counting) — cheap, high impact, removes most of the self-reinforcement.
2. Observe live for a bit; confirm hot buckets now track real spawn geography, not bot dwell.
3. #2 (timesObserved rate + unknown state), then retire the novelty queue.
4. Minor notes only if a specific symptom shows up.
