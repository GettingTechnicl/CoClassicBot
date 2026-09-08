#pragma once
// =====================================================================
// spawn_memory.h — remembers where monsters actually appear.
//
// The entity scan already enumerates every monster with coordinates several
// times a second; this just accumulates that into a coarse per-map heatmap so
// the bot can learn its hunting ground instead of searching it uniformly at
// random every circuit.
//
// It plugs into ONE place: FindZoneExplorePosition() currently picks a
// uniformly random point inside the zone. Weighting that choice by observed
// spawn density is the entire behavioural change — the hunt loop, targeting
// and combat are untouched.
//
// SIZE (this was costed before building):
//   8x8 tile buckets, so the largest map (1024x1024) is 128x128 = 16,384
//   buckets. Stored SPARSE — only buckets that have actually seen a monster —
//   because most of a map never does. A radius-30 hunt zone is ~18 buckets; a
//   heavily roamed map a few thousand. Tens of KB per map on disk, well under
//   100 KB resident for the current map.
//
// Decay is what bounds it long-term: scores fade, buckets that reach zero are
// pruned, so a map you stop hunting shrinks away by itself. Two extra rails
// guard the pathological cases — a hard entry cap per map, and dropping maps
// not seen for a long time on load.
//
// Decay is driven by SCAN COUNT, not wall-clock: an idle client should not
// forget what it learned, and a busy one should adapt at the rate it is
// actually observing things.
// =====================================================================
#include "base.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SpawnMemory
{
    constexpr int kBucketTiles = 8;      // tiles per bucket edge
    constexpr int kMaxBucketsPerMap = 20000;

    // Record monster sightings for one entity refresh. Scores an APPEARANCE —
    // a monster id newly seen, or seen in a different bucket than last call —
    // not raw dwell time: an id still sitting in the same bucket as last
    // batch earns no further credit, so a monster the bot stands and fights
    // stops dominating the map the moment it's been counted once. Decay still
    // runs every batch regardless.
    //
    // heroPos also marks every bucket within the game's view range as an
    // OBSERVATION OPPORTUNITY, whether or not a monster was in it — this is
    // what makes "genuinely searched, nothing here" distinguishable from
    // "never been here" (see GetDensity).
    void Observe(OBJID mapId, const Position& heroPos, const std::vector<std::pair<OBJID, Position>>& monsters);

    // Score for the bucket containing a tile. 0 = never seen a monster there.
    // Higher = seen more often / more recently. Raw appearance count, NOT
    // normalised by how often the bucket has been looked at — a bucket on the
    // bot's usual path accrues a higher raw score than an equally-dense one
    // it rarely passes, purely from being seen more. Prefer GetDensity for
    // any decision that compares buckets against each other; this stays
    // mainly for the overlay/novelty machinery that already keys off it.
    float GetScore(OBJID mapId, const Position& tile);

    // Appearance rate for the bucket containing a tile: spawns / observation-
    // opportunities, both decayed on the same clock as GetScore. Removes the
    // "seen more often := scores higher" bias GetScore has. Returns -1.0f
    // when the bucket hasn't been looked at enough to trust a rate yet —
    // callers should treat that as "unknown, worth exploring", a state
    // distinct from a confirmed density of 0 ("looked, nothing spawns here").
    // Does NOT include the novelty boost GetScore/GetHotBuckets apply — the
    // two are different scales (a rate vs. a decayed count) and novelty is
    // being kept as its own separate, unverified mechanism for now.
    float GetDensity(OBJID mapId, const Position& tile);

    // Best-known score anywhere on the map, for normalising weights.
    float GetMaxScore(OBJID mapId);

    // Packed bucket key for a tile, and the tile at a bucket's center — the
    // same kBucketTiles grid GetHotBuckets/GetScore use internally. Exposed
    // so callers that track their own per-bucket state (Phase 2b's dynamic
    // zone cell set) align to the identical grid instead of re-deriving it.
    uint32_t BucketKeyFor(const Position& tile);
    Position BucketCenterOf(uint32_t key);

    // Centers of the top-scored buckets (novelty boosts included), hottest
    // first. Exploration feeds these into its candidate pool directly:
    // uniform sampling alone almost never rediscovers a small hot area on a
    // large map (live MapWide run: best sampled candidate scored 5-40% of the
    // known max while the map held 400-point buckets the sampler never hit).
    std::vector<Position> GetHotBuckets(OBJID mapId, int maxCount);

    // True once enough observations exist for the scores to mean anything —
    // until then callers should keep searching uniformly rather than trusting
    // a map built from two sightings.
    bool HasUsefulData(OBJID mapId);

    // Persist / restore. Storage is sparse and pruned, see header comment.
    void Save();
    void Load();

    // Diagnostics for the overlay. novelBuckets = buckets currently carrying a
    // temporary "test it out" novelty boost (see spawn_memory.cpp).
    // knownBuckets = buckets with enough observation opportunities for
    // GetDensity to return a trusted rate rather than "unknown" (see
    // kMinTimesObservedTrust in spawn_memory.cpp).
    struct Stats { int maps; int buckets; int observations; float maxScore; int novelBuckets; int knownBuckets; };
    Stats GetStats(OBJID mapId);

    void ClearMap(OBJID mapId);
}

// Save on the same cadence as the config autosave — no second timer, and the
// data is cheap enough that the write is unnoticeable.
void MaybeAutoSaveSpawnMemory();
