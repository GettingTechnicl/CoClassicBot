#pragma once
#include "base.h"
#include "hunt_settings.h"
#include "jitter.h"
#include "entities.h"
#include "hunt_targeting.h"
#include "config.h"
#include "game.h"
#include "CHero.h"
#include "CRole.h"
#include "CGameMap.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <limits>

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

// Session 10: the Travel tab's "Speedhack If No Players Nearby" checkbox
// (TravelSettings::usePacketJump — NOT AutoHuntSettings::usePacketJump,
// which is a separate, identically-named checkbox on Melee Hunt's own
// combat tab controlling only its packet-jump movement) originally had no
// effect outside the travel plugin. Repurposed here to also gate every
// action-pacing interval below: run at each one's floor (== its own kMin*
// constant, not a separate value — every floor the user asked for already
// matched an existing clamp minimum) when no non-whitelisted player is
// nearby, and fall back to the user's own slider value the moment one is
// detected. Reuses the Player Safety feature's own detection range/
// whitelist (settings.safetyPlayerRange/playerWhitelist) rather than adding
// a second, redundant "how close counts as nearby" setting — this is
// independent of settings.safetyEnabled itself, so the speed behavior works
// whether or not the safety-rest feature is separately turned on.
inline bool IsAnyPlayerNearby(const AutoHuntSettings& settings)
{
    CHero* hero = Game::GetHero();
    if (!hero)
        return false;

    const OBJID heroId = hero->GetID();
    const int range = settings.safetyPlayerRange;  // 0 = unlimited, matches CheckPlayerSafety's own semantics
    const std::vector<std::string> whitelist = ParseTokens(settings.playerWhitelist);

    for (CRole* role : Entities::Get()) {
        if (!role || !Entities::IsAlive(role) || !role->IsPlayer() || role->GetID() == heroId)
            continue;
        if (!whitelist.empty() && NameMatchesFilters(role->GetName(), whitelist))
            continue;
        if (range > 0
            && CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, role->m_posMap.x, role->m_posMap.y) > range)
            continue;
        return true;
    }
    return false;
}

// Session 11: Paranoia Mode's detection step — same scan/whitelist/range
// logic as IsAnyPlayerNearby above, but returns the actual nearest threat
// position rather than just a bool, since biasing target choice/explore
// destinations/the zone leash away from a player needs somewhere to bias
// AWAY FROM. Kept separate from IsAnyPlayerNearby (rather than having that
// function grow an out-param) since most of that function's callers only
// ever wanted the bool and don't need this extra scan-and-track work.
inline bool GetParanoiaThreat(const AutoHuntSettings& settings, Position* outThreatPos)
{
    if (!settings.paranoiaEnabled)
        return false;

    CHero* hero = Game::GetHero();
    if (!hero)
        return false;

    const OBJID heroId = hero->GetID();
    const int range = settings.safetyPlayerRange;
    const std::vector<std::string> whitelist = ParseTokens(settings.playerWhitelist);

    CRole* nearest = nullptr;
    int nearestDist = (std::numeric_limits<int>::max)();
    for (CRole* role : Entities::Get()) {
        if (!role || !Entities::IsAlive(role) || !role->IsPlayer() || role->GetID() == heroId)
            continue;
        if (!whitelist.empty() && NameMatchesFilters(role->GetName(), whitelist))
            continue;
        const int dist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, role->m_posMap.x, role->m_posMap.y);
        if (range > 0 && dist > range)
            continue;
        if (dist < nearestDist) {
            nearestDist = dist;
            nearest = role;
        }
    }

    if (!nearest)
        return false;
    if (outThreatPos)
        *outThreatPos = nearest->m_posMap;
    return true;
}

// Session 12: this exact scan shape — Entities::Get() with the empty/10000
// size guard, an i<500 cap, an IsAlive() check, then "nearest role matching
// some predicate within an optional radius" — was independently duplicated
// across MiningPlugin::FindPlayerNearSpot, MulePlugin::FindNearbyRequester,
// MulePlugin::FindNearbyWhitelistedTrader, and FollowPlugin::FindTarget (each
// also carried a vestigial Game::GetRoleMgr() null-check left over from
// before this codebase switched to heap-scan-based Entities::Get() —
// hunt_targeting.cpp's CollectHuntTargets, the canonical scanner, checks no
// such thing, so it's dropped here too). Consolidated so a new predicate/use
// case doesn't need its own copy of the boilerplate. maxRange <= 0 means "no
// range limit" — still returns the single nearest match, useful when a
// predicate like an exact name match is expected to match at most one role.
template <typename Predicate>
inline CRole* FindNearestRole(const Position& from, int maxRange, Predicate pred)
{
    const std::vector<CRole*> roles = Entities::Get();
    if (roles.empty() || roles.size() >= 10000)
        return nullptr;

    CRole* best = nullptr;
    float bestDist = (std::numeric_limits<float>::max)();
    for (size_t i = 0; i < roles.size() && i < 500; ++i) {
        CRole* role = roles[i];
        if (!role || !Entities::IsAlive(role))
            continue;
        if (!pred(role))
            continue;
        const float dist = from.DistanceTo(role->m_posMap);
        if (maxRange > 0 && dist > (float)maxRange)
            continue;
        if (dist < bestDist) {
            bestDist = dist;
            best = role;
        }
    }
    return best;
}

inline bool ShouldUseAggressiveSpeeds(const AutoHuntSettings& settings)
{
    const bool toggleOn = GetTravelSettings().usePacketJump;
    const bool playerNearby = toggleOn && IsAnyPlayerNearby(settings);
    const bool result = toggleOn && !playerNearby;

    static DWORD s_lastLogTick = 0;
    const DWORD now = GetTickCount();
    if (now - s_lastLogTick >= 2000) {
        s_lastLogTick = now;
        spdlog::trace("[speedhack] toggleOn={} playerNearby={} -> aggressive={}", toggleOn, playerNearby, result);
    }

    return result;
}

// Session 11: short hops prefer a real animated Walk over an instant Jump
// when not running aggressive/speedhack speeds — jumping in front of other
// players looks unnatural, but walking a few tiles reads as normal movement.
// Shared here (rather than living in base_hunt_plugin.cpp) since both
// autohunt movement (StartPathTo) and manual minimap click-to-navigate
// (overlay.cpp) need the same threshold.
constexpr int kWalkInsteadOfJumpTiles = 4;

constexpr int kMinMovementIntervalMs = 100;
constexpr int kMaxMovementIntervalMs = 5000;
inline DWORD GetMovementIntervalMs(const AutoHuntSettings& settings)
{
    if (ShouldUseAggressiveSpeeds(settings))
        return WithActionJitter(kMinMovementIntervalMs);
    return WithActionJitter(ClampMs(settings.movementIntervalMs, kMinMovementIntervalMs, kMaxMovementIntervalMs));
}

constexpr int kMinAttackIntervalMs = 25;
constexpr int kMaxAttackIntervalMs = 5000;
inline DWORD GetAttackIntervalMs(const AutoHuntSettings& settings)
{
    if (ShouldUseAggressiveSpeeds(settings))
        return WithActionJitter(kMinAttackIntervalMs);
    return WithActionJitter(ClampMs(settings.attackIntervalMs, kMinAttackIntervalMs, kMaxAttackIntervalMs));
}
inline DWORD GetCycloneAttackIntervalMs(const AutoHuntSettings& settings)
{
    if (ShouldUseAggressiveSpeeds(settings))
        return WithActionJitter(kMinAttackIntervalMs);
    return WithActionJitter(ClampMs(settings.cycloneAttackIntervalMs, kMinAttackIntervalMs, kMaxAttackIntervalMs));
}

constexpr int kMinTargetSwitchAttackIntervalMs = 0;
constexpr int kMaxTargetSwitchAttackIntervalMs = 5000;
inline DWORD GetTargetSwitchAttackIntervalMs(const AutoHuntSettings& settings)
{
    if (ShouldUseAggressiveSpeeds(settings))
        return WithActionJitter(kMinTargetSwitchAttackIntervalMs);
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
