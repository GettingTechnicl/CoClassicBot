#pragma once
#include "base.h"

// =====================================================================
// jitter.h — shared action-timing jitter.
//
// Session 10: every action-pacing interval (movement, pickup, attack, etc.)
// should have a random 50-250ms ADDED on top of its configured base value,
// NEVER subtracted — a perfectly periodic action timer is itself a
// detectable pattern, and adding jitter (rather than randomizing around the
// base) guarantees an action never fires faster than what the user
// configured, only occasionally a bit slower.
// =====================================================================

// Returns baseMs plus a random 50-250ms. Always >= baseMs + 50.
inline DWORD WithActionJitter(DWORD baseMs)
{
    static uint32_t s_rng = 0;
    if (s_rng == 0)
        s_rng = (GetTickCount() ^ 0xA5A5A5A5u) | 1u;
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    const DWORD jitterMs = 50 + (s_rng % 201); // 50..250
    return baseMs + jitterMs;
}
