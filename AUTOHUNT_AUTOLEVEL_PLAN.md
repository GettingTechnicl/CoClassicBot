# AutoHunt / AutoLevel — Map-Wide Hunting Design

Two behavioral goals for the existing hunt system, not two new plugins — both
run through the same `MeleeHuntPlugin`/`ArcherHuntPlugin` → `BaseHuntPlugin`
machinery that already exists. What changes is *what the hunt is optimizing
for*, controlled by a new `AutoHuntGoal` (`AutoHuntSettings::huntGoal` in
`src/hunt_settings.h`):

- **Farm** (`AutoHuntGoal::Farm`, the default, today's existing behavior) —
  maximize kill/loot efficiency. No monster-danger gating at all; the
  character is presumed strong enough for wherever it's set to hunt.
- **Level** (`AutoHuntGoal::Level`) — maximize XP-per-risk. Gates engagement
  by the monster's name-color tier relative to the hero's *current* level,
  since a fixed absolute level threshold means nothing as the hero grows —
  the tier boundary is what stays meaningful.

## Foundation: the monster-level field

Session 13 live-correlated `CRole+0x6E8` against user-confirmed green/white/
red/black name colors (see `coclassicbot-project-status` memory for the full
capture data) and mapped it as `CRole::m_nLevel` / `GetLevel()`. Notably, the
ORIGINAL fork's own early static-RE notes (`REVAMP_PLAN.md`, "Where we are
(delivered)") already listed `level +0x6E8` as cross-verified against a
different character entirely, from a completely different investigative
method (static disassembly against a pre-revival build, not live memory
correlation). Two independent methods landing on the same offset is strong
corroboration this mapping is correct.

`CRole::GetDangerTier(int heroLevel)` (`src/CRole.h`) buckets
`monsterLevel - heroLevel` into `MonsterDangerTier::{Green,White,Red,Black}`
using boundaries interpolated from 7 confirmed live data points at hero level
110 — see the class comment for the exact anchor deltas. Boundaries are a
first-pass estimate (midpoint of each observed gap), not exactly pinned;
expect to refine them as more edge-case monsters get spot-checked.

Because `CHero : public CRole` and `CHero`'s own fields don't start until
`+0x71C` (well past `+0x6E8`), `GetLevel()`/`GetDangerTier()` work identically
on the hero object — `hero->GetLevel()` is the hero's own live level, no
separate field needed.

## Shared infrastructure (already exists, reused as-is)

`SpawnMemory` (`src/spawn_memory.h/.cpp`) is a sparse, per-map, decaying
heatmap of observed monster density — the "persistent buckets." It's already
keyed by `OBJID mapId`, not by hunt zone, so it's already map-wide in the
sense that matters here: nothing about it needs to change for either mode.
Today it feeds exactly one hook, `BaseHuntPlugin::FindZoneExplorePosition()`
— when a zone has "useful data" (`SpawnMemory::HasUsefulData`), exploration
prefers the highest-scoring sampled candidate over a uniformly random one.

## Part 1 — Map-Wide zone mode (shipped, commit after this doc)

`AutoHuntZoneMode::MapWide` (4th zone shape, alongside Circle/Polygon/Route).
Every point on the target map (`zoneMapId`) counts as "in zone" — no drawn
shape. This is what lets AutoHunt/AutoLevel roam using `SpawnMemory`'s
already-map-wide data, instead of being artificially confined to a hand-drawn
circle/polygon within it. `IsPointInHuntZone`/`IsPointNearHuntZone`/
`HasValidHuntZone`/`GetHuntZoneAnchor` (`src/hunt_settings.cpp`) all handle it
as a special case; `FindZoneExplorePosition`'s candidate sampling
(`src/plugins/base_hunt_plugin.cpp`) samples uniformly over the map's full
tile extent (`CGameMap::m_sizeMap`) instead of a shape.

Scope note: this is *not* a multi-map roaming system. `zoneMapId` is still a
single map — "map-wide" means "the whole current map," not "every map in the
game." No cross-map travel-graph work needed; `SpawnMemory` was already keyed
per-map correctly.

## Part 2 — AutoLevel danger-tier slider (shipped, commit after this doc)

`AutoHuntSettings::maxDangerTier` (`MonsterDangerTier`, default `White`).
Only consulted when `huntGoal == Level`; Farm mode ignores it entirely.
Deliberately a **per-character slider**, not a hardcoded "always avoid
Red/Black" rule — a tanky, well-geared class (Warrior) can reasonably level
through Black-name areas that would be reckless for a squishier class. Wired
into `CollectHuntTargets` (`src/hunt_targeting.cpp`) as an additional filter
alongside the existing name-based include/ignore/prefer lists: a monster
whose `GetDangerTier(hero->GetLevel())` exceeds `maxDangerTier` is dropped
from the candidate list entirely, same as an ignored name.

Not yet implemented: soft *preference* for higher-XP tiers within the
allowed range (currently Green through `maxDangerTier` are all equally
eligible — no bias toward White/Red over trivial Green when better options
exist nearby). Worth adding once there's a real leveling character to tune
against; premature to hand-tune weights against zero live data.

## Part 3 — AutoHunt exploration incentive (shipped, commit `d87d90a`)

**Problem**: `FindZoneExplorePosition` already avoids being a pure argmax
(its own comment: "Sampling keeps some exploration alive") by sampling 40
uniformly-random candidates and picking the best-SCORED one among them each
call — but nothing ever favors a candidate specifically *because* it's
unscanned. A hot spot found early can dominate every exploration decision
indefinitely, since it'll almost always be among the 40 samples and almost
always score highest. Nothing pulls the bot toward genuinely uncharted
territory to complete the map's density picture. This gets worse under
Map-Wide mode specifically, since the search space is much larger per call.

**Shipped**: `AutoHuntSettings::explorationChancePercent` (0-100, default 0 =
disabled/today's pure-exploit behavior). Rolled once per `FindZoneExplorePosition`
call, only meaningful once `SpawnMemory::HasUsefulData` is true. On an
"explore" roll, the loop keeps the LOWEST-scoring candidate among the 40
samples instead of the highest — reuses the exact same candidate generation
and `SpawnMemory::GetScore` call already in the loop, so the only new code is
which candidate gets kept at the end. Paranoia evasion still takes priority
over this when both apply (safety before exploration). UI: "Exploration
Chance %" slider, shown for every zone mode via a shared
`RenderExplorationBiasUI` helper.

## Part 4 — Paranoia camping-detection with escalating backoff (shipped, commit `17a46d1`)

**Problem**: Paranoia Mode today (`GetParanoiaThreat`, `src/hunt_intervals.h`)
is a pure per-tick nearest-player distance check with no memory of duration
— it biases target choice/explore destination/zone leash away from whoever
is nearest right now, every tick, with no concept of "this player has been
sitting on my best bucket for 10 minutes" vs. "this player just walked
through." The existing Safety Rest feature has the adjacent piece (duration
tracking via `m_nearbyPlayerTicks`, `unordered_map<OBJID, DWORD>` in
`base_hunt_plugin.h`) but for a different purpose (full retreat, not
bucket-avoidance) and at hero-centered range, not bucket-centered.

**Shipped as designed**: tracks contest state per `SpawnMemory` bucket (reuse
its existing 8x8-tile bucket key convention, so density and contest data
line up) in a new small module, `src/hunt_contest.h/.cpp` mirroring
`spawn_memory.h/.cpp`'s shape (in-memory `unordered_map`, no persistence
needed — a camper's presence doesn't need to survive a DLL reload):

```cpp
struct BucketContest {
    OBJID camperPlayerId = 0;
    DWORD continuousPresenceSinceTick = 0; // when THIS camper was first seen there, resets if they leave
    DWORD nextRecheckTick = 0;             // 0 = not currently marked contested
};
// unordered_map<uint32_t /*bucket key*/, BucketContest> per map
```

Logic, evaluated only when Paranoia is enabled and only against the bucket(s)
`FindZoneExplorePosition`/AutoHunt's bucket-selection is about to pick:
1. Scan players near the candidate bucket (same pattern as
   `GetParanoiaThreat`'s scan, radius = bucket size + a small margin).
2. No player present → clear any existing contest entry for that bucket,
   bucket is fully available.
3. Player present, no entry yet → create one (`continuousPresenceSinceTick =
   now`), but DON'T yet treat it as contested — a single sighting is "just
   passing through," which ordinary Paranoia distance-avoidance already
   handles fine without any bucket-level logic.
4. Player present continuously past a confirm threshold (e.g. 3 minutes,
   needs live tuning) → mark contested, exclude the bucket from selection,
   set `nextRecheckTick = now + ~20-30 min`.
5. Once `nextRecheckTick` passes, re-evaluate: still occupied continuously →
   extend `nextRecheckTick` by another ~20-30 min (repeat, not exponential
   — matches "another 20-30 mins" as stated, not a doubling backoff); empty
   or a different transient visitor → clear the entry, bucket available
   again.

Hooked into `FindZoneExplorePosition`'s existing candidate loop directly: a
candidate whose bucket `HuntContest::IsBucketContested` reports true is
skipped (`continue`) alongside the existing validity checks (walkable,
in-zone, minimum travel distance), before scoring even runs — so a contested
bucket simply never becomes a selection candidate that call, same 40-sample
loop as everything else. `IsBucketContested`'s own player-presence scan uses
an 8-tile radius around the candidate point (matching `SpawnMemory`'s bucket
edge length), not a strict same-bucket check — resolves the open question
below in favor of the slightly-larger radius.

**Still open, needs live tuning, not a design decision**: `kCampingConfirmMs`
(3 minutes) and `kRecheckBackoffMs` (25 minutes, the midpoint of the
"20-30 min" the user described) are both first-pass constants in
`hunt_contest.cpp` with zero live data behind them. Adjust once there's a
real character and an actual contested spot to observe.

## Status

All four parts are implemented (commits `00178f3`, `d87d90a`, `17a46d1`) and
build/test clean (`map_tests.exe` 29/29 after each). None have been
live-tested yet — no live character has run either goal mode, the danger
tier boundaries are still first-pass interpolated estimates, and Part 4's
timing constants are unverified guesses. Structurally correct, behaviorally
unverified. Per the user's own framing, "we'll tighten things up more once I
start a character out from 1" — that live tuning pass is the real next step,
not more design work.

## Explicitly out of scope for this doc

Per-class (Warrior/Trojan/Archer/etc.) level-1-to-X action paths — which
skills to prioritize, when to switch zones, what gear checkpoints gate a
danger-tier increase. This is a content-mapping problem (where do
appropriate-tier monsters actually exist at each level bracket, for each
class's playstyle/survivability), not a code problem, and needs the same
live-capture-first approach the color-tier mapping used — guessing this from
outside the game would just be re-creating the same mistake the `L###`
name-suffix hypothesis made earlier this session (a plausible-looking guess
that turned out wrong until checked against real data).
