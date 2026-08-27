#pragma once
// =====================================================================
// hunt_contest.h — tracks whether another player is "camping" a
// SpawnMemory bucket, distinct from just passing through.
//
// Part 4 of AUTOHUNT_AUTOLEVEL_PLAN.md. Paranoia Mode's existing
// GetParanoiaThreat() (hunt_intervals.h) is a pure per-tick nearest-player
// distance check with no memory of duration — fine for "bias away from
// whoever is nearest right now", but not enough to distinguish "someone
// has been sitting on my best farming spot for 10 minutes" from "someone
// just walked past it once". This module adds that missing duration
// tracking, scoped to individual SpawnMemory buckets (same 8-tile
// granularity, so density and contest data line up on the same grid) so
// FindZoneExplorePosition can skip a genuinely-camped bucket while still
// reconsidering it later once the camper (or their replacement) has been
// confirmed gone for good, or after an escalating recheck backoff if
// they're still there.
//
// Deliberately in-memory only, no persistence — a camper's presence isn't
// meaningful information once the DLL reloads.
// =====================================================================
#include "base.h"
#include "hunt_settings.h"

namespace HuntContest
{
    // Is `pos`'s bucket currently contested (a non-whitelisted player has
    // been camping it past the confirm threshold)? Also updates internal
    // presence-tracking state as a side effect — call this once per
    // candidate actually being considered during an exploration decision,
    // not continuously for every point on the map.
    bool IsBucketContested(OBJID mapId, const Position& pos, const AutoHuntSettings& settings);

    // Diagnostics for the overlay.
    struct Stats { int trackedBuckets; int contestedBuckets; };
    Stats GetStats(OBJID mapId);
}
