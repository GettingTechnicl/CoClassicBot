#pragma once
#include "base.h"
#include "hunt_settings.h"
#include "jitter.h"
#include <algorithm>

// =====================================================================
// hunt_intervals.h — shared action-pacing interval getters.
//
// Session 10 cont.: these were independently duplicated across
// base_hunt_plugin.cpp, hunt_loot.cpp, archer_hunt_plugin.cpp,
// melee_hunt_plugin.cpp, hunt_buffs.cpp, and hunt_town.cpp — up to 4 copies
// of the same getter (GetItemActionIntervalMs) with duplicated ClampMs
// helpers and duplicated min/max constant pairs alongside them. Consolidated
// here rather than left to drift out of sync across copies.
//
// All of these except GetLootSpawnGraceMs are jittered per this project's
// action-jitter policy (see jitter.h) — a random 50-250ms is always ADDED on
// top, never subtracted. GetLootSpawnGraceMs is a one-shot wait period after
// an item spawns, not a repeated action cadence, so it's deliberately not
// jittered (same reasoning as GetReviveDelayMs, which stays in
// base_hunt_plugin.cpp since nothing else duplicates it).
// =====================================================================

inline DWORD ClampMs(int value, int minValue, int maxValue)
{
    return static_cast<DWORD>(std::clamp(value, minValue, maxValue));
}

constexpr int kMinMovementIntervalMs = 100;
constexpr int kMaxMovementIntervalMs = 5000;
inline DWORD GetMovementIntervalMs(const AutoHuntSettings& settings)
{
    return WithActionJitter(ClampMs(settings.movementIntervalMs, kMinMovementIntervalMs, kMaxMovementIntervalMs));
}

constexpr int kMinAttackIntervalMs = 25;
constexpr int kMaxAttackIntervalMs = 5000;
inline DWORD GetAttackIntervalMs(const AutoHuntSettings& settings)
{
    return WithActionJitter(ClampMs(settings.attackIntervalMs, kMinAttackIntervalMs, kMaxAttackIntervalMs));
}
inline DWORD GetCycloneAttackIntervalMs(const AutoHuntSettings& settings)
{
    return WithActionJitter(ClampMs(settings.cycloneAttackIntervalMs, kMinAttackIntervalMs, kMaxAttackIntervalMs));
}

constexpr int kMinTargetSwitchAttackIntervalMs = 0;
constexpr int kMaxTargetSwitchAttackIntervalMs = 5000;
inline DWORD GetTargetSwitchAttackIntervalMs(const AutoHuntSettings& settings)
{
    return WithActionJitter(ClampMs(settings.targetSwitchAttackIntervalMs,
        kMinTargetSwitchAttackIntervalMs, kMaxTargetSwitchAttackIntervalMs));
}

constexpr int kMinItemActionIntervalMs = 100;
constexpr int kMaxItemActionIntervalMs = 5000;
inline DWORD GetItemActionIntervalMs(const AutoHuntSettings& settings)
{
    return WithActionJitter(ClampMs(settings.itemActionIntervalMs, kMinItemActionIntervalMs, kMaxItemActionIntervalMs));
}

constexpr int kMinLootSpawnGraceMs = 0;
constexpr int kMaxLootSpawnGraceMs = 5000;
inline DWORD GetLootSpawnGraceMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.lootSpawnGraceMs, kMinLootSpawnGraceMs, kMaxLootSpawnGraceMs);
}

constexpr int kMinSelfCastIntervalMs = 100;
constexpr int kMaxSelfCastIntervalMs = 5000;
inline DWORD GetSelfCastIntervalMs(const AutoHuntSettings& settings)
{
    return WithActionJitter(ClampMs(settings.selfCastIntervalMs, kMinSelfCastIntervalMs, kMaxSelfCastIntervalMs));
}

constexpr int kMinNpcActionIntervalMs = 100;
constexpr int kMaxNpcActionIntervalMs = 2000;
inline DWORD GetNpcActionIntervalMs(const AutoHuntSettings& settings)
{
    return WithActionJitter(ClampMs(settings.npcActionIntervalMs, kMinNpcActionIntervalMs, kMaxNpcActionIntervalMs));
}

constexpr int kMinReviveRetryIntervalMs = 100;
constexpr int kMaxReviveRetryIntervalMs = 10000;
inline DWORD GetReviveRetryIntervalMs(const AutoHuntSettings& settings)
{
    return WithActionJitter(ClampMs(settings.reviveRetryIntervalMs, kMinReviveRetryIntervalMs, kMaxReviveRetryIntervalMs));
}
