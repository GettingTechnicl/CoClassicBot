#pragma once
#include "base.h"

// =====================================================================
// jitter.h — shared RNG + action-timing jitter.
//
// Session 10 cont.: this xorshift32 body was independently copy-pasted 4
// times across the codebase (here, hunt_targeting.cpp's JitterDestination,
// base_hunt_plugin.cpp's FindZoneExplorePosition and TryRandomWalk) — two of
// the four copies had already drifted from the other two (dropped the
// `^0xA5A5A5A5u` seed mask). Consolidated into one shared generator; call
// sites needing their own value range/shape build on NextRandom32() instead
// of reimplementing the generator itself.
// =====================================================================

inline uint32_t NextRandom32()
{
    static uint32_t s_rng = 0;
    if (s_rng == 0)
        s_rng = (GetTickCount() ^ 0xA5A5A5A5u) | 1u;
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

// Session 10: every action-pacing interval (movement, pickup, attack, etc.)
// should have a random 50-250ms ADDED on top of its configured base value,
// NEVER subtracted — a perfectly periodic action timer is itself a
// detectable pattern, and adding jitter (rather than randomizing around the
// base) guarantees an action never fires faster than what the user
// configured, only occasionally a bit slower.
//
// Returns baseMs plus a random 50-250ms. Always >= baseMs + 50.
inline DWORD WithActionJitter(DWORD baseMs)
{
    const DWORD jitterMs = 50 + (NextRandom32() % 201); // 50..250
    return baseMs + jitterMs;
}
