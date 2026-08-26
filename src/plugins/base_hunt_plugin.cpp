#include "base_hunt_plugin.h"
#include "jitter.h"
#include "hunt_intervals.h"
#include "hunt_buffs.h"
#include "hunt_loot.h"
#include "hunt_targeting.h"
#include "hunt_town.h"
#include "map_items.h"
#include "inventory_utils.h"
#include "npc_utils.h"
#include "revive_utils.h"
#include "plugin_mgr.h"
#include "travel_plugin.h"
#include "game.h"
#include "spawn_memory.h"
#include "hooks.h"
#include "gateway.h"
#include "CHero.h"
#include "CGameMap.h"
#include "CRole.h"
#include "config.h"
#include "discord.h"
#include "hunt_stats.h"
#include "itemtype.h"
#include "pathfinder.h"
#include "log.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>

// ── Anonymous namespace: shared helpers ──────────────────────────────────────
namespace {

const Position kMarketLandingPos = {211, 196};

constexpr int kReliableAttackRange = 1;
constexpr int kMinMobClumpSize = 2;
constexpr int kMinLootRange = 0;
constexpr int kMaxLootRange = CGameMap::MAX_JUMP_DIST;
constexpr int kLootPathStopRange = 0;
constexpr int kMinEntityScanIntervalMs = 100;
constexpr int kMaxEntityScanIntervalMs = 5000;
constexpr int kMinItemPickupDelayMs = 0;
constexpr int kMaxItemPickupDelayMs = 3000;
constexpr int kMinDecisionThrottleMs = 0;
constexpr int kMaxDecisionThrottleMs = 1000;
constexpr int kMinRandomWalkIntervalMs = 0;
constexpr int kMaxRandomWalkIntervalMs = 10000;
constexpr int kMinLootPickupIgnoreMs = 0;
constexpr int kMaxLootPickupIgnoreMs = 300000;
constexpr int kMinManualControlPauseMs = 0;
constexpr int kMaxManualControlPauseMs = 30000;
constexpr int kMinReviveDelayMs = 0;
constexpr int kMaxReviveDelayMs = 60000;
constexpr DWORD kPendingJumpStallMs = 500;
// Session 11 [LOCKUP FIX]: see m_zoneTravelFailCount's comment in the header.
constexpr int kMaxZoneTravelFailures = 3;
// Session 10: hard cap on a path that claims to be active while the hero is
// not actually moving. The pathfinder has its own stall recovery, but it was
// observed failing to fire for over a minute, freezing movement AND combat.
constexpr DWORD kPathWatchdogMs = 3000;
constexpr DWORD kPendingJumpHardTimeoutMs = 6000;

int GetLootRange(const AutoHuntSettings& settings)
{
    return std::clamp(settings.lootRange, kMinLootRange, kMaxLootRange);
}

// Session 10: Loot Range is meant to control how eagerly the bot detours
// for ordinary drops/money — a rare, high-value item (any +item, Meteor/
// DragonBall family) is always worth fetching regardless of that setting,
// so those bypass the range check entirely here rather than needing a
// separately-configured "unlimited" Loot Range that would also loosen it
// for everything else. The bot still only reaches it via normal pathing,
// so real reachability/zone limits still apply — this only removes the
// extra Loot Range cap specifically.
bool IsWithinLootPickupRange(const AutoHuntSettings& settings, int distance, const CMapItem& item)
{
    if (HuntTownService::IsPriorityLootItem(item))
        return true;
    const int lootRange = GetLootRange(settings);
    return lootRange > 0 ? distance <= lootRange : distance == 0;
}

DWORD GetJumpMovementIntervalMs(const AutoHuntSettings& settings, const CHero* /*hero*/)
{
    return GetMovementIntervalMs(settings);
}

DWORD GetEntityScanIntervalMs(const AutoHuntSettings& settings)
{
    if (ShouldUseAggressiveSpeeds(settings))
        return kMinEntityScanIntervalMs;
    return ClampMs(settings.entityScanIntervalMs, kMinEntityScanIntervalMs, kMaxEntityScanIntervalMs);
}

DWORD GetDecisionThrottleMs(const AutoHuntSettings& settings)
{
    if (ShouldUseAggressiveSpeeds(settings))
        return kMinDecisionThrottleMs;
    return ClampMs(settings.decisionThrottleMs, kMinDecisionThrottleMs, kMaxDecisionThrottleMs);
}

DWORD GetManualControlPauseMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.manualControlPauseMs, kMinManualControlPauseMs, kMaxManualControlPauseMs);
}

DWORD GetReviveDelayMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.reviveDelayMs, kMinReviveDelayMs, kMaxReviveDelayMs);
}

bool TickIsFuture(DWORD targetTick, DWORD now)
{
    return static_cast<int32_t>(targetTick - now) > 0;
}

bool IsArcherModeEnabled(const AutoHuntSettings& settings)
{
    return settings.archerMode || settings.combatMode == AutoHuntCombatMode::Archer;
}

const char* CombatModeLabel(AutoHuntCombatMode mode)
{
    return mode == AutoHuntCombatMode::Archer ? "Archer" : "Melee";
}

void HelpMarker(const char* text)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", text);
}

void HelpMarkerOnSameLine(const char* text)
{
    ImGui::SameLine();
    HelpMarker(text);
}

} // anonymous namespace

// ── StateName (file-scope free function) ─────────────────────────────────────
static const char* StateName(AutoHuntState state)
{
    switch (state) {
        case AutoHuntState::Idle:           return "Idle";
        case AutoHuntState::WaitingForGame: return "Waiting For Game";
        case AutoHuntState::Ready:          return "Ready";
        case AutoHuntState::TravelToZone:   return "Travel To Zone";
        case AutoHuntState::AcquireTarget:  return "Acquire Target";
        case AutoHuntState::ApproachTarget: return "Approach Target";
        case AutoHuntState::AttackTarget:   return "Attack Target";
        case AutoHuntState::LootNearby:     return "Loot Nearby";
        case AutoHuntState::Recover:        return "Recover";
        case AutoHuntState::TravelToMarket: return "Travel To Market";
        case AutoHuntState::Repair:         return "Repair";
        case AutoHuntState::BuyArrows:      return "Buy Arrows";
        case AutoHuntState::StoreItems:     return "Store Items";
        case AutoHuntState::ReturnToZone:   return "Return To Zone";
        case AutoHuntState::Failed:         return "Failed";
        default:                            return "Unknown";
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Shared method implementations
// ═════════════════════════════════════════════════════════════════════════════

void BaseHuntPlugin::SetState(AutoHuntState state, const char* statusText)
{
    if (m_state != state)
        spdlog::info("[hunt] State: {} -> {} | {}", GetStateName(), StateName(state),
            statusText ? statusText : "");
    m_state = state;
    snprintf(m_statusText, sizeof(m_statusText), "%s", statusText ? statusText : "");
}

const char* BaseHuntPlugin::GetStateName() const
{
    return StateName(m_state);
}

Position BaseHuntPlugin::GetEffectiveHeroPosition(const CHero* hero) const
{
    if (!hero)
        return {};

    if (hero->IsJumping())
        return hero->GetCommand().posTarget;

    if (m_pendingJumpTick != 0)
        return m_pendingJumpDest;

    return hero->m_posMap;
}

void BaseHuntPlugin::ClearPendingJumpState()
{
    m_pendingJumpDest = {};
    m_pendingJumpTick = 0;
    m_pendingJumpLastPos = {};
    m_pendingJumpLastProgressTick = 0;
    m_pendingJumpIsRetreat = false;
}

void BaseHuntPlugin::ArmPendingJump(CHero* hero, const Position& destination, DWORD now, bool isRetreat)
{
    m_pendingJumpDest = destination;
    m_pendingJumpTick = now;
    m_pendingJumpLastPos = hero ? hero->m_posMap : Position{};
    m_pendingJumpLastProgressTick = now;
    m_pendingJumpIsRetreat = isRetreat;
}

bool BaseHuntPlugin::UpdatePendingJumpState(CHero* hero, DWORD now)
{
    if (!hero || m_pendingJumpTick == 0)
        return false;

    const bool landed = hero->m_posMap.x == m_pendingJumpDest.x
        && hero->m_posMap.y == m_pendingJumpDest.y;
    if (landed) {
        // Retreat-specific landing is handled by subclass via HandleCombatRetreat
        ClearPendingJumpState();
        return false;
    }

    if (hero->m_posMap.x != m_pendingJumpLastPos.x || hero->m_posMap.y != m_pendingJumpLastPos.y) {
        m_pendingJumpLastPos = hero->m_posMap;
        m_pendingJumpLastProgressTick = now;
        return true;
    }

    const DWORD age = now - m_pendingJumpTick;
    const DWORD stalledFor = now - m_pendingJumpLastProgressTick;
    const bool stalled = stalledFor > kPendingJumpStallMs;
    const bool hardTimedOut = age > kPendingJumpHardTimeoutMs;
    if (!stalled && !hardTimedOut)
        return true;

    if (m_pendingJumpIsRetreat) {
        spdlog::warn("[hunt] Retreat jump failed pos=({},{}) dest=({},{}), age={}ms stalledFor={}ms retry=immediate",
            hero->m_posMap.x, hero->m_posMap.y,
            m_pendingJumpDest.x, m_pendingJumpDest.y,
            age, stalledFor);
    } else {
        spdlog::debug("[hunt] Move jump failed pos=({},{}) dest=({},{}), age={}ms stalledFor={}ms",
            hero->m_posMap.x, hero->m_posMap.y,
            m_pendingJumpDest.x, m_pendingJumpDest.y,
            age, stalledFor);
    }
    ClearPendingJumpState();
    return false;
}

void BaseHuntPlugin::RefreshRuntimeState(CHero* hero, CGameMap* map)
{
    m_lastHeroPos = hero ? hero->m_posMap : Position{};
    const OBJID currentMapId = Game::GetCurrentMapId();
    if (currentMapId != m_lastMapId)
        m_lootMgr.ResetLootPickupAttempts();
    m_lastMapId = currentMapId;
    m_lastHp = hero ? hero->GetCurrentHp() : 0;
    m_lastMaxHp = hero ? hero->GetMaxHp() : 0;
    m_lastMana = hero ? hero->GetCurrentMana() : 0;
    m_lastMaxMana = hero ? hero->GetMaxMana() : 0;
    m_lastBagCount = hero ? hero->m_deqItem.size() : 0;
    m_buffMgr.RefreshBuffState(hero);
    if (m_buffMgr.IsPreLandingRetreat() && m_buffMgr.CanRecastAnyFly(hero, GetAutoHuntSettings())) {
        m_buffMgr.SetPreLandingRetreat(false);
    }
    // Let subclass refresh combat-specific state (e.g. scatter range)
    RefreshCombatState(hero, GetAutoHuntSettings());
}

void BaseHuntPlugin::StopAutomation(bool cancelTravel)
{
    Pathfinder::Get().Stop();
    if (cancelTravel) {
        if (auto* travel = PluginManager::Get().GetPlugin<TravelPlugin>(); travel && travel->IsTraveling())
            travel->CancelTravel();
    }

    m_targetId = 0;
    m_lastClumpSize = 0;
    ClearPendingJumpState();
    m_lootMgr.ResetLootPickupAttempts();
    m_townService.ResetRepairSequence();
    m_townService.ResetBuyArrowsSequence();
    m_townService.ResetStoreSequence();
    m_nearbyPlayerTicks.clear();
    m_safetyResting = false;
    m_safetyRestStartTick = 0;
    SetState(AutoHuntState::Idle, "Disabled");
}

bool BaseHuntPlugin::HandleDeath(CHero* hero, TravelPlugin* travel, const AutoHuntSettings& settings)
{
    if (!hero) {
        m_reviveState = {};
        return false;
    }

    if (!hero->IsDead()) {
        m_reviveState = {};
        return false;
    }

    if (!settings.autoReviveInTown) {
        SetState(AutoHuntState::Failed, "Hero is dead");
        return true;
    }

    if (m_reviveState.deathTick == 0) {
        if (travel && travel->IsTraveling())
            travel->CancelTravel();
        m_targetId = 0;
        m_townService.ResetRepairSequence();
        m_townService.ResetStoreSequence();
    }

    const bool handled = HandleRevive(hero, m_reviveState,
        GetReviveDelayMs(settings), GetReviveRetryIntervalMs(settings),
        settings.autoReviveInTown, m_statusText, sizeof(m_statusText));

    if (handled)
        m_state = AutoHuntState::Recover;
    return handled;
}

bool BaseHuntPlugin::FindZoneExplorePosition(CHero* hero, CGameMap* map,
    const AutoHuntSettings& settings, Position& out) const
{
    out = {};
    if (!hero || !map)
        return false;

    const Position heroPos = hero->m_posMap;

    // Sample points inside the zone and take the first walkable one that is a
    // meaningful distance away — near-identical destinations would leave the
    // bot shuffling in place, which looks the same as being stuck.
    const int minTravel = (std::max)(4, GetHuntLeash(settings));

    // Spawn memory: once enough has been observed, prefer places monsters have
    // actually been seen instead of searching the zone uniformly. This is the
    // ONLY hook the heatmap needs — exploration is where "go look somewhere"
    // is decided, so weighting it changes behaviour without touching the hunt
    // loop, targeting or combat.
    const OBJID mapId = Game::GetCurrentMapId();
    const bool useHeatmap = SpawnMemory::HasUsefulData(mapId);
    const float maxScore = useHeatmap ? SpawnMemory::GetMaxScore(mapId) : 0.0f;
    Position bestScored{};
    float    bestScore = -1.0f;

    // Session 11 [Paranoia Mode]: while a threat is detected, evasion takes
    // priority over normal exploration (heatmap-guided or first-valid) —
    // sample every candidate and keep the one farthest from the threat
    // instead of stopping at the first walkable tile.
    Position threatPos{};
    const bool paranoiaEvading = GetParanoiaThreat(settings, &threatPos);
    Position bestAwayFromThreat{};
    float    bestThreatDist = -1.0f;

    for (int attempt = 0; attempt < 40; ++attempt) {
        Position candidate{};

        if (settings.zoneMode == AutoHuntZoneMode::Route) {
            const HuntRoute* r = GetActiveRoute(settings, settings.zoneMapId);
            if (!r || r->waypoints.empty())
                return false;
            // Head for a waypoint further along than the nearest one, so the
            // bot works the route instead of hovering at one corner.
            const int nearest = NearestWaypointIndex(*r, heroPos);
            const int step = 1 + (int)(NextRandom32() % 3u);
            candidate = r->waypoints[(nearest + step) % (int)r->waypoints.size()];
        } else if (settings.zoneMode == AutoHuntZoneMode::Circle) {
            if (IsZeroPos(settings.zoneCenter) || settings.zoneRadius <= 0)
                return false;
            // Uniform-ish over the disc, so the interior gets visited as often
            // as the rim rather than the bot favouring the boundary.
            const float ang = (float)(NextRandom32() % 3600u) * 0.0017453f;
            const float rad = (float)settings.zoneRadius * sqrtf((float)(NextRandom32() % 1000u) / 1000.0f);
            candidate.x = settings.zoneCenter.x + (int)(cosf(ang) * rad);
            candidate.y = settings.zoneCenter.y + (int)(sinf(ang) * rad);
        } else {
            if (settings.zonePolygon.size() < 3)
                return false;
            int minX = settings.zonePolygon[0].x, maxX = minX;
            int minY = settings.zonePolygon[0].y, maxY = minY;
            for (const Position& v : settings.zonePolygon) {
                minX = (std::min)(minX, v.x); maxX = (std::max)(maxX, v.x);
                minY = (std::min)(minY, v.y); maxY = (std::max)(maxY, v.y);
            }
            const int spanX = (std::max)(1, maxX - minX);
            const int spanY = (std::max)(1, maxY - minY);
            candidate.x = minX + (int)(NextRandom32() % (uint32_t)spanX);
            candidate.y = minY + (int)(NextRandom32() % (uint32_t)spanY);
        }

        if (candidate.x <= 0 || candidate.y <= 0)
            continue;
        if (!IsPointInZone(settings, settings.zoneMapId, candidate))
            continue;
        if (!map->IsWalkable(candidate.x, candidate.y))
            continue;
        if (CGameMap::TileDist(heroPos.x, heroPos.y, candidate.x, candidate.y) < minTravel)
            continue;

        if (paranoiaEvading) {
            const float threatDist = candidate.DistanceTo(threatPos);
            if (threatDist > bestThreatDist) {
                bestThreatDist = threatDist;
                bestAwayFromThreat = candidate;
            }
            continue;
        }

        if (!useHeatmap) {
            out = JitterDestination(map, candidate, GetJitterRadius(settings));
            return true;
        }

        // Keep the best-scoring valid candidate rather than the first one.
        // Deliberately still SAMPLED rather than a straight argmax over the
        // heatmap: always walking to the single hottest bucket would abandon
        // the rest of the zone and let the memory go stale everywhere else.
        // Sampling keeps some exploration alive so the map stays current.
        const float score = SpawnMemory::GetScore(mapId, candidate);
        if (score > bestScore) {
            bestScore = score;
            bestScored = candidate;
        }
    }

    if (paranoiaEvading && bestThreatDist >= 0.0f && !IsZeroPos(bestAwayFromThreat)) {
        out = JitterDestination(map, bestAwayFromThreat, GetJitterRadius(settings));
        spdlog::trace("[hunt] Explore (paranoia) -> ({},{}) threatDist={:.1f}", out.x, out.y, bestThreatDist);
        return true;
    }

    if (useHeatmap && bestScore >= 0.0f && !IsZeroPos(bestScored)) {
        out = JitterDestination(map, bestScored, GetJitterRadius(settings));
        spdlog::trace("[hunt] Explore -> ({},{}) spawnScore={:.1f}/{:.1f}",
                      out.x, out.y, bestScore, maxScore);
        return true;
    }
    return false;
}

bool BaseHuntPlugin::HasValidZone(const AutoHuntSettings& settings) const
{
    return HasValidHuntZone(settings);
}

bool BaseHuntPlugin::IsPointInZone(const AutoHuntSettings& settings, OBJID mapId, const Position& pos) const
{
    return IsPointInHuntZone(settings, mapId, pos);
}

Position BaseHuntPlugin::GetZoneAnchor(const AutoHuntSettings& settings) const
{
    return GetHuntZoneAnchor(settings);
}

bool BaseHuntPlugin::IsPlayerWhitelisted(const AutoHuntSettings& settings, const char* name) const
{
    if (!name || !name[0])
        return false;
    for (const std::string& token : ParseTokens(settings.playerWhitelist)) {
        if (_stricmp(token.c_str(), name) == 0)
            return true;
    }
    return false;
}

bool BaseHuntPlugin::CheckPlayerSafety(CHero* hero, CGameMap* map, TravelPlugin* travel,
    const AutoHuntSettings& settings)
{
    // Session 10: travel wasn't checked here, unlike every sibling function
    // in this file (BeginTravelToZone, BeginTravelToMarket, etc.) that takes
    // the same parameter. GetPlugin<T>() genuinely can return null on a
    // dynamic_cast miss, and this function unconditionally dereferences
    // travel below (IsTraveling()/StartTravel()) with no other guard.
    if (!settings.safetyEnabled || !hero || !map || !travel)
        return false;

    if (m_safetyResting) {
        if (m_lastMapId == MAP_MARKET && !travel->IsTraveling()) {
            // Cancel fly on arrival so we don't waste duration while resting
            if (hero->IsFlyActive())
                hero->CancelFly();
            DWORD elapsed = GetTickCount() - m_safetyRestStartTick;
            if (elapsed >= (DWORD)settings.safetyRestSec * 1000) {
                spdlog::info("[autohunt] Safety rest complete, returning to zone");
                m_safetyResting = false;
                m_nearbyPlayerTicks.clear();
                BeginTravelToZone(travel, settings);
                return true;
            }
            int remaining = (int)(settings.safetyRestSec - elapsed / 1000);
            char buf[128];
            snprintf(buf, sizeof(buf), "Safety rest in Market (%ds remaining)", remaining);
            SetState(AutoHuntState::TravelToMarket, buf);
        }
        return true;
    }

    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr)
        return false;

    // Session 11: this used to call Entities::Roles() (which internally does
    // a fresh Entities::Get() - full cache-check + vector copy - on every
    // single call) up to 1000+ times per invocation: twice for the guard
    // check above, then twice more per loop iteration for up to 500
    // iterations. One Get() call here instead.
    const std::vector<CRole*> roles = Entities::Get();
    if (roles.empty() || roles.size() >= 10000)
        return false;

    const OBJID heroId = hero->GetID();
    const DWORD now = GetTickCount();
    const int range = settings.safetyPlayerRange;
    const DWORD threshold = (DWORD)settings.safetyDetectionSec * 1000;

    std::unordered_set<OBJID> inRange;
    for (size_t i = 0; i < roles.size() && i < 500; ++i) {
        CRole* role = roles[i];
        if (!role) continue;
        if (!role->IsPlayer() || role->GetID() == heroId)
            continue;
        if (IsPlayerWhitelisted(settings, role->GetName()))
            continue;
        if (range > 0) {
            int dist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
                                           role->m_posMap.x, role->m_posMap.y);
            if (dist > range)
                continue;
        }
        inRange.insert(role->GetID());
    }

    for (auto it = m_nearbyPlayerTicks.begin(); it != m_nearbyPlayerTicks.end(); ) {
        if (inRange.find(it->first) == inRange.end())
            it = m_nearbyPlayerTicks.erase(it);
        else
            ++it;
    }

    for (OBJID id : inRange) {
        if (m_nearbyPlayerTicks.find(id) == m_nearbyPlayerTicks.end())
            m_nearbyPlayerTicks[id] = now;
    }

    for (auto& [id, firstTick] : m_nearbyPlayerTicks) {
        if (now - firstTick >= threshold) {
            // Resolve player name for logging/notification
            const char* playerName = "Unknown";
            for (size_t i = 0; i < roles.size() && i < 500; ++i) {
                CRole* r = roles[i];
                if (r && r->GetID() == id) { playerName = r->GetName(); break; }
            }
            spdlog::warn("[autohunt] Player '{}' ({}) nearby for {}s, triggering safety escape to Market",
                playerName, id, (now - firstTick) / 1000);
            if (settings.safetyNotifyDiscord) {
                const char* heroName = hero->GetName();
                char buf[256];
                snprintf(buf, sizeof(buf), "[%s] Player safety triggered — '%s' nearby for %ds",
                    heroName, playerName, (int)((now - firstTick) / 1000));
                SendDiscordNotification(buf, true);
            }
            m_safetyResting = true;
            m_safetyRestStartTick = GetTickCount();
            m_nearbyPlayerTicks.clear();
            Pathfinder::Get().Stop();
            travel->StartTravel(MAP_MARKET, kMarketLandingPos);
            SetState(AutoHuntState::TravelToMarket, "Safety: traveling to Market");
            return true;
        }
    }

    return false;
}

HuntBuffCallbacks BaseHuntPlugin::MakeBuffCallbacks(CHero* hero, CGameMap* map, const AutoHuntSettings& settings)
{
    HuntBuffCallbacks cb;
    cb.setStateFn = [this](AutoHuntState state, const char* text) {
        SetState(state, text);
    };
    cb.startPathNearTargetFn = [this](CHero* h, CGameMap* m, const Position& pos, int range) {
        return StartPathNearTarget(h, m, pos, range);
    };
    cb.armPendingJumpFn = [this](CHero* h, const Position& dest, DWORD now, bool isRetreat) {
        ArmPendingJump(h, dest, now, isRetreat);
    };
    cb.recordMoveTick = [this]() -> DWORD {
        m_lastMoveTick = GetTickCount();
        return m_lastMoveTick;
    };
    cb.setTargetId = [this](OBJID id) {
        m_targetId = id;
    };
    cb.isLootPickupIgnoredFn = [this](OBJID id, DWORD now) {
        return m_lootMgr.IsLootPickupIgnored(id, now);
    };
    cb.tryPickupLootItemFn = [this, &settings](CHero* h, const CMapItem* item, DWORD now) {
        return m_lootMgr.TryPickupLootItem(h, settings, item, now,
            [this, h](DWORD t) { return UpdatePendingJumpState(h, t); });
    };
    // Base does not implement FindSafeArcherRetreat — subclass overrides if needed
    cb.findSafeArcherRetreatFn = [](CHero*, CGameMap*, const AutoHuntSettings&,
        const std::vector<CRole*>&, CRole*, Position&, int) {
        return false;
    };
    cb.collectHuntTargetsFn = [](const AutoHuntSettings& s) {
        return CollectHuntTargets(s);
    };
    return cb;
}

// ── Movement helpers ────────────────────────────────────────────────────────

bool BaseHuntPlugin::StartPathTo(CHero* hero, CGameMap* map, const Position& destination, int stopRange)
{
    if (!hero || !map)
        return false;

    const DWORD now = GetTickCount();
    const bool hasPendingJump = UpdatePendingJumpState(hero, now);

    const int hx = hasPendingJump ? m_pendingJumpDest.x : hero->m_posMap.x;
    const int hy = hasPendingJump ? m_pendingJumpDest.y : hero->m_posMap.y;

    const int dist = CGameMap::TileDist(hx, hy, destination.x, destination.y);
    if (dist <= stopRange)
        return false;

    if (Pathfinder::Get().IsActive()) {
        spdlog::trace("[hunt] Move blocked: pathfinder active, dest=({},{})", destination.x, destination.y);
        return true;
    }

    if (hero->IsJumping()) {
        spdlog::trace("[hunt] Move blocked: IsJumping pos=({},{}) cmd_target=({},{}) dest=({},{})",
            hero->m_posMap.x, hero->m_posMap.y,
            hero->GetCommand().posTarget.x, hero->GetCommand().posTarget.y,
            destination.x, destination.y);
        return true;
    }

    if (hasPendingJump) {
        spdlog::trace("[hunt] Move blocked: pending jump pos=({},{}) pendingDest=({},{}) age={}ms dest=({},{})",
            hero->m_posMap.x, hero->m_posMap.y,
            m_pendingJumpDest.x, m_pendingJumpDest.y,
            now - m_pendingJumpTick,
            destination.x, destination.y);
        return true;
    }

    const AutoHuntSettings& settings = GetAutoHuntSettings();

    // Session 10: this settle delay was hardcoded to 500ms, completely
    // independent of movementIntervalMs/speedhack — even with the
    // aggressive-speed floor (100ms) active, movement was still capped to
    // a ~500ms cadence here, so "Speedhack If No Players Nearby" had no
    // visible effect on normal pathfinding. Also a likely contributor to a
    // separate stuck-loop bug: this branch returns true (blocked) without
    // ever issuing a move, and the caller treats "true" as "movement is in
    // progress" — if this kept blocking indefinitely, the hunt loop would
    // spin here at full frame rate forever instead of ever recovering.
    // Scales with the same aggressive/slider value movementIntervalMs uses
    // instead of a fixed constant, so speedhack actually speeds this up too.
    const DWORD settleThresholdMs = ShouldUseAggressiveSpeeds(settings) ? kMinMovementIntervalMs : 500;
    const DWORD pathfinderJumpAge = now - Pathfinder::Get().GetLastJumpTick();
    if (pathfinderJumpAge < settleThresholdMs && !Pathfinder::Get().IsActive()) {
        spdlog::trace("[hunt] Move blocked: pathfinder settle age={}ms threshold={}ms pos=({},{}) dest=({},{})",
            pathfinderJumpAge, settleThresholdMs, hero->m_posMap.x, hero->m_posMap.y,
            destination.x, destination.y);
        return true;
    }
    // Prefer a real animated Walk over an instant Jump for short hops when
    // not running aggressive speeds (no nearby player to hide from, or the
    // speedhack toggle is off) — falls through to the jump/pathfind logic
    // below if no reachable candidate tile was found (e.g. destination boxed
    // in on all sides).
    if (dist <= kWalkInsteadOfJumpTiles && !ShouldUseAggressiveSpeeds(settings)) {
        if (StartWalkTo(hero, map, destination, stopRange))
            return true;
    }

    const bool canDirectJump = dist <= CGameMap::MAX_JUMP_DIST
        && map->CanJump(hx, hy, destination.x, destination.y, CGameMap::GetHeroAltThreshold())
        && !IsTileOccupied(destination.x, destination.y);
    const DWORD movementIntervalMs = canDirectJump
        ? GetJumpMovementIntervalMs(settings, hero)
        : GetMovementIntervalMs(settings);
    if (now - m_lastMoveTick < movementIntervalMs)
        return true;

    if (canDirectJump) {
        spdlog::debug("[hunt] JUMP ({},{}) -> ({},{}) dist={}", hx, hy, destination.x, destination.y, dist);
        hero->Jump(destination.x, destination.y);
        m_lastMoveTick = now;
        ArmPendingJump(hero, destination, now, false);
        return true;
    }

    spdlog::debug("[hunt] Direct jump failed dist={} canJump={} occupied={}, trying A* ({},{}) -> ({},{})",
        dist, map->CanJump(hx, hy, destination.x, destination.y, CGameMap::GetHeroAltThreshold()),
        IsTileOccupied(destination.x, destination.y),
        hx, hy, destination.x, destination.y);

    auto tilePath = map->FindPath(hx, hy, destination.x, destination.y, 1000000);
    if (tilePath.empty())
        return false;

    auto waypoints = map->SimplifyPath(tilePath);
    if (waypoints.empty())
        return false;

    Pathfinder::Get().StartPath(waypoints, [] { return GetMovementIntervalMs(GetAutoHuntSettings()); });
    m_lastMoveTick = now;
    return true;
}

bool BaseHuntPlugin::TryRandomWalk(CHero* hero, CGameMap* map, const AutoHuntSettings& settings, DWORD now)
{
    const DWORD baseIntervalMs = (DWORD)std::clamp(settings.randomWalkIntervalMs,
        kMinRandomWalkIntervalMs, kMaxRandomWalkIntervalMs);
    if (baseIntervalMs == 0)
        return false;

    // See jitter.h — recomputed fresh each call so the effective interval
    // varies call to call rather than settling into a fixed cadence.
    const DWORD intervalMs = WithActionJitter(baseIntervalMs);

    if (m_lastRandomWalkTick != 0 && (now - m_lastRandomWalkTick) < intervalMs)
        return false;
    m_lastRandomWalkTick = now;

    // Short random hop, 1-2 tiles in any direction. StartWalkTo() already
    // handles walkability/occupancy/reachability and won't fire while the
    // hero is jumping or the pathfinder is mid-route.
    for (int attempt = 0; attempt < 6; ++attempt) {
        const int dx = (int)(NextRandom32() % 5) - 2; // -2..2
        const int dy = (int)(NextRandom32() % 5) - 2;
        if (dx == 0 && dy == 0)
            continue;
        const Position candidate{ hero->m_posMap.x + dx, hero->m_posMap.y + dy };
        if (StartWalkTo(hero, map, candidate, 0))
            return true;
    }
    return false;
}

DWORD BaseHuntPlugin::ComputeNextAttackDelayMs(CHero* hero, CRole* target, const AutoHuntSettings& settings) const
{
    const bool targetChanged = (m_targetId != target->GetID());
    const bool justFinishedApproach = (m_state == AutoHuntState::ApproachTarget);
    const DWORD attackInterval = hero->IsCycloneActive()
        ? GetCycloneAttackIntervalMs(settings)
        : GetAttackIntervalMs(settings);
    return (targetChanged || justFinishedApproach)
        ? GetTargetSwitchAttackIntervalMs(settings)
        : attackInterval;
}

bool BaseHuntPlugin::StartWalkTo(CHero* hero, CGameMap* map, const Position& destination, int stopRange)
{
    if (!hero || !map)
        return false;

    const int hx = hero->m_posMap.x;
    const int hy = hero->m_posMap.y;
    const int dist = CGameMap::TileDist(hx, hy, destination.x, destination.y);
    if (dist <= stopRange)
        return false;

    if (Pathfinder::Get().IsActive())
        Pathfinder::Get().Stop();

    if (hero->IsJumping())
        return true;

    const DWORD now = GetTickCount();
    const bool hasPendingJump = UpdatePendingJumpState(hero, now);

    if (hasPendingJump)
        return true;

    if (now - m_lastMoveTick < GetMovementIntervalMs(GetAutoHuntSettings()))
        return true;

    Position bestPos = destination;
    bool found = false;
    int bestTargetDist = (std::numeric_limits<int>::max)();
    float bestHeroDist = (std::numeric_limits<float>::max)();
    const int searchRadius = (std::max)(stopRange, 0);

    for (int dx = -searchRadius; dx <= searchRadius; ++dx) {
        for (int dy = -searchRadius; dy <= searchRadius; ++dy) {
            const Position candidate = {destination.x + dx, destination.y + dy};
            const int targetDist = CGameMap::TileDist(candidate.x, candidate.y, destination.x, destination.y);
            if (targetDist > stopRange)
                continue;
            if (!map->IsWalkable(candidate.x, candidate.y))
                continue;
            if ((candidate.x != hx || candidate.y != hy) && IsTileOccupied(candidate.x, candidate.y))
                continue;
            if (!map->CanReach(hx, hy, candidate.x, candidate.y))
                continue;

            const float heroDist = Position{hx, hy}.DistanceTo(candidate);
            if (!found || targetDist < bestTargetDist
                || (targetDist == bestTargetDist && heroDist < bestHeroDist)) {
                found = true;
                bestPos = candidate;
                bestTargetDist = targetDist;
                bestHeroDist = heroDist;
            }
        }
    }

    if (!found)
        return false;

    hero->Walk(bestPos.x, bestPos.y);
    m_lastMoveTick = now;
    return true;
}

bool BaseHuntPlugin::StartPathNearTarget(CHero* hero, CGameMap* map, const Position& targetPos, int desiredRange)
{
    if (!hero || !map)
        return false;

    const Position effectivePos = GetEffectiveHeroPosition(hero);
    const int currentDist = CGameMap::TileDist(effectivePos.x, effectivePos.y, targetPos.x, targetPos.y);
    if (currentDist <= desiredRange)
        return false;

    Position bestPos = targetPos;
    bool found = false;
    int bestTargetDist = (std::numeric_limits<int>::max)();
    float bestHeroDist = (std::numeric_limits<float>::max)();
    const int searchRadius = (std::max)(desiredRange + 2, 4);

    for (int dx = -searchRadius; dx <= searchRadius; ++dx) {
        for (int dy = -searchRadius; dy <= searchRadius; ++dy) {
            const Position candidate = {targetPos.x + dx, targetPos.y + dy};
            const int targetDist = CGameMap::TileDist(candidate.x, candidate.y, targetPos.x, targetPos.y);
            if (targetDist > searchRadius)
                continue;
            if (!map->IsWalkable(candidate.x, candidate.y))
                continue;
            if ((candidate.x != hero->m_posMap.x || candidate.y != hero->m_posMap.y)
                && IsTileOccupied(candidate.x, candidate.y)) {
                continue;
            }

            const float heroDist = effectivePos.DistanceTo(candidate);
            if (!found || targetDist < bestTargetDist
                || (targetDist == bestTargetDist && heroDist < bestHeroDist)) {
                found = true;
                bestPos = candidate;
                bestTargetDist = targetDist;
                bestHeroDist = heroDist;
            }
        }
    }

    if (!found)
        return false;

    return StartPathTo(hero, map, bestPos, 0);
}

// ── Travel helpers ──────────────────────────────────────────────────────────

void BaseHuntPlugin::BeginTravelToZone(TravelPlugin* travel, const AutoHuntSettings& settings)
{
    if (!travel) {
        SetState(AutoHuntState::Failed, "Travel plugin not available");
        return;
    }

    // Arrow buying happens on arrival at the zone city (HandleTravelToZone),
    // not before travel starts — Market has no blacksmith.

    const Position anchor = GetZoneAnchor(settings);
    if (IsZeroPos(anchor)) {
        SetState(AutoHuntState::Failed, "Hunt zone anchor is not set");
        return;
    }

    CHero* zoneHero = Game::GetHero();
    CGameMap* zoneMap = Game::GetMap();
    // Session 10: tolerate the leash. Targeting now engages monsters slightly
    // outside the zone, so the hero legitimately stands outside it while
    // fighting. Testing strict containment here would drag it back mid-fight
    // and fight the engage logic — only a drift beyond the leash counts as
    // actually having left.
    const bool sameMapOutsideZone = zoneHero && zoneMap
        && Game::GetCurrentMapId() == settings.zoneMapId
        && !IsPointNearHuntZone(settings, Game::GetCurrentMapId(), zoneHero->m_posMap,
                                GetHuntLeash(settings));
    if (sameMapOutsideZone) {
        if (zoneHero->IsJumping() || Pathfinder::Get().IsActive()) {
            SetState(m_lastMapId == MAP_MARKET ? AutoHuntState::ReturnToZone : AutoHuntState::TravelToZone,
                "Moving into hunt zone");
            return;
        }

        Position zoneEntry = {};
        if (!FindClosestZoneTile(zoneMap, settings, zoneHero->m_posMap, zoneEntry)) {
            SetState(AutoHuntState::Failed, "No walkable tile inside hunt zone");
            return;
        }

        if (!StartPathTo(zoneHero, zoneMap, zoneEntry, 0)) {
            SetState(AutoHuntState::Failed, "Failed to move into hunt zone");
            return;
        }

        SetState(m_lastMapId == MAP_MARKET ? AutoHuntState::ReturnToZone : AutoHuntState::TravelToZone,
            "Moving into hunt zone");
        return;
    }

    travel->StartTravel(settings.zoneMapId, anchor);
    SetState(m_lastMapId == MAP_MARKET ? AutoHuntState::ReturnToZone : AutoHuntState::TravelToZone,
        "Traveling to hunt zone");
}

void BaseHuntPlugin::BeginTravelToMarket(TravelPlugin* travel, CHero* hero, const AutoHuntSettings& settings)
{
    if (!travel || !hero) {
        SetState(AutoHuntState::Failed, "Travel plugin not available");
        return;
    }

    Pathfinder::Get().Stop();
    m_townService.ResetRepairSequence();
    m_townService.ResetBuyArrowsSequence();
    m_townService.ResetStoreSequence();

    if (m_lastMapId == MAP_MARKET) {
        if (settings.autoRepair && m_townService.NeedsRepair(hero, settings)) {
            SetState(AutoHuntState::Repair, "Moving to Pharmacist");
        } else if (settings.autoStore && (m_townService.NeedsStorage(hero, settings)
                || (settings.immediateReturnOnPriorityItems && m_townService.HasPriorityReturnItems(hero, settings)))) {
            SetState(AutoHuntState::StoreItems, "Processing storage rules");
        } else {
            BeginTravelToZone(travel, settings);
        }
        return;
    }

    travel->StartTravel(MAP_MARKET, kMarketLandingPos);
    SetState(AutoHuntState::TravelToMarket, "Traveling to Market");
}

void BaseHuntPlugin::HandleTravelToZone(TravelPlugin* travel, const AutoHuntSettings& settings)
{
    if (!travel) {
        SetState(AutoHuntState::Failed, "Travel plugin not available");
        return;
    }

    if (travel->GetState() == TravelState::Failed) {
        ++m_zoneTravelFailCount;
        SetState(AutoHuntState::Failed, "Failed to reach hunt zone");
        return;
    }

    if (travel->IsTraveling()) {
        if (m_lastMapId == settings.zoneMapId && IsPointInZone(settings, m_lastMapId, m_lastHeroPos)) {
            travel->CancelTravel();
            m_zoneTravelFailCount = 0;
            CHero* zoneHero = Game::GetHero();
            if (zoneHero && NeedsTownRunArrows(zoneHero, settings) && HuntTownService::HasBlacksmithOnMap(m_lastMapId)) {
                m_townService.ResetBuyArrowsSequence();
                SetState(AutoHuntState::BuyArrows, "Buying arrows at zone city");
                return;
            }
            m_targetId = 0;
            SetState(AutoHuntState::AcquireTarget, "Entered hunt zone");
            return;
        }
        SetState(m_state, "Traveling to hunt zone");
        return;
    }

    if (m_lastMapId != settings.zoneMapId || !IsPointInZone(settings, m_lastMapId, m_lastHeroPos)) {
        BeginTravelToZone(travel, settings);
        return;
    }

    CHero* hero = Game::GetHero();
    if (hero && NeedsTownRunArrows(hero, settings) && HuntTownService::HasBlacksmithOnMap(m_lastMapId)) {
        m_townService.ResetBuyArrowsSequence();
        SetState(AutoHuntState::BuyArrows, "Buying arrows at zone city");
        return;
    }

    m_zoneTravelFailCount = 0;
    m_targetId = 0;
    SetState(AutoHuntState::AcquireTarget, "Scanning hunt zone");
}

void BaseHuntPlugin::HandleTravelToMarket(TravelPlugin* travel, CHero* hero, const AutoHuntSettings& settings)
{
    if (!travel || !hero) {
        SetState(AutoHuntState::Failed, "Travel plugin not available");
        return;
    }

    if (travel->GetState() == TravelState::Failed) {
        SetState(AutoHuntState::Failed, "Failed to reach Market");
        return;
    }

    if (travel->IsTraveling()) {
        SetState(AutoHuntState::TravelToMarket, "Traveling to Market");
        return;
    }

    if (m_lastMapId != MAP_MARKET) {
        BeginTravelToMarket(travel, hero, settings);
        return;
    }

    if (m_safetyResting) {
        if (m_safetyRestStartTick == 0)
            m_safetyRestStartTick = GetTickCount();
        return;
    }

    if (settings.autoRepair && m_townService.NeedsRepair(hero, settings)) {
        m_townService.ResetRepairSequence();
        SetState(AutoHuntState::Repair, "Moving to Pharmacist");
    } else if (settings.autoStore && (m_townService.NeedsStorage(hero, settings)
            || (settings.immediateReturnOnPriorityItems && m_townService.HasPriorityReturnItems(hero, settings)))) {
        m_townService.ResetStoreSequence();
        SetState(AutoHuntState::StoreItems, "Processing storage rules");
    } else {
        BeginTravelToZone(travel, settings);
    }
}

HuntTownCallbacks BaseHuntPlugin::MakeTownCallbacks(TravelPlugin* travel, CHero* hero, const AutoHuntSettings& settings)
{
    HuntTownCallbacks cb;
    cb.setStateFn = [this](AutoHuntState state, const char* text) {
        SetState(state, text);
    };
    cb.startPathNearTargetFn = [this](CHero* h, CGameMap* m, const Position& pos, int range) {
        return StartPathNearTarget(h, m, pos, range);
    };
    const AutoHuntSettings* settingsPtr = &settings;
    cb.beginTravelToZoneFn = [this, travel, settingsPtr]() {
        BeginTravelToZone(travel, *settingsPtr);
    };
    cb.beginTravelToMarketFn = [this, travel, hero, settingsPtr]() {
        BeginTravelToMarket(travel, hero, *settingsPtr);
    };
    return cb;
}

// ── Zone helpers ────────────────────────────────────────────────────────────

bool BaseHuntPlugin::FindClosestZoneTile(CGameMap* map, const AutoHuntSettings& settings,
    const Position& from, Position& outZonePos) const
{
    outZonePos = {};
    if (!map)
        return false;

    int minX = 0;
    int maxX = 0;
    int minY = 0;
    int maxY = 0;
    if (settings.zoneMode == AutoHuntZoneMode::Circle) {
        if (IsZeroPos(settings.zoneCenter) || settings.zoneRadius <= 0)
            return false;
        minX = settings.zoneCenter.x - settings.zoneRadius;
        maxX = settings.zoneCenter.x + settings.zoneRadius;
        minY = settings.zoneCenter.y - settings.zoneRadius;
        maxY = settings.zoneCenter.y + settings.zoneRadius;
    } else {
        if (settings.zonePolygon.empty())
            return false;
        minX = maxX = settings.zonePolygon.front().x;
        minY = maxY = settings.zonePolygon.front().y;
        for (const Position& vertex : settings.zonePolygon) {
            minX = (std::min)(minX, vertex.x);
            maxX = (std::max)(maxX, vertex.x);
            minY = (std::min)(minY, vertex.y);
            maxY = (std::max)(maxY, vertex.y);
        }
    }

    const Position anchor = GetZoneAnchor(settings);
    bool found = false;
    int bestHeroDist = (std::numeric_limits<int>::max)();
    float bestAnchorDist = (std::numeric_limits<float>::max)();
    for (int x = minX; x <= maxX; ++x) {
        for (int y = minY; y <= maxY; ++y) {
            const Position candidate = {x, y};
            if (!IsPointInZone(settings, settings.zoneMapId, candidate))
                continue;
            if (!map->IsWalkable(x, y))
                continue;
            if ((x != from.x || y != from.y) && IsTileOccupied(x, y))
                continue;

            const int heroDist = CGameMap::TileDist(from.x, from.y, x, y);
            const float anchorDist = IsZeroPos(anchor) ? 0.0f : candidate.DistanceTo(anchor);
            if (!found
                || heroDist < bestHeroDist
                || (heroDist == bestHeroDist && anchorDist < bestAnchorDist)) {
                found = true;
                outZonePos = candidate;
                bestHeroDist = heroDist;
                bestAnchorDist = anchorDist;
            }
        }
    }

    return found;
}

// ═════════════════════════════════════════════════════════════════════════════
// Update() — the shared hunt loop with virtual combat dispatch
// ═════════════════════════════════════════════════════════════════════════════

void BaseHuntPlugin::Update()
{
    // Always tick session stats so the panel stays live regardless of m_enabled.
    // The tracker internally checks whether any hunt plugin is enabled before
    // attributing kills / drops / gold so we don't double-count between subclasses.
    HuntStats::Update();

    AutoHuntSettings& settings = GetAutoHuntSettings();
    TravelPlugin* travel = PluginManager::Get().GetPlugin<TravelPlugin>();

    // Push the live slider value into the background scanners every tick
    // (cheap atomic store). Runs regardless of m_enabled/hero/map validity
    // since overlay diagnostics can call Entities::Get()/MapItems::Get()
    // independently of an active hunt.
    const DWORD scanIntervalMs = GetEntityScanIntervalMs(settings);
    Entities::SetRefreshIntervalMs(scanIntervalMs);
    MapItems::SetRefreshIntervalMs(scanIntervalMs);

    CHero* hero = Game::GetHero();
    CGameMap* map = Game::GetMap();
    RefreshRuntimeState(hero, map);

    // Update debug best-clump overlay
    m_debugBestClumpCenter = {};
    m_debugBestClumpSize = 0;
    if (settings.debugShowBestClump && hero && map) {
        const auto targets = CollectHuntTargets(settings);
        if (!targets.empty()) {
            const float clumpR = (float)(std::max)(1, settings.clumpRadius);
            if (CRole* best = FindBestClusterTarget(targets, hero->m_posMap, clumpR, &m_debugBestClumpSize)) {
                m_debugBestClumpCenter = best->m_posMap;
            }
        }
    }

    if (!m_enabled) {
        if (m_state != AutoHuntState::Idle)
            StopAutomation(true);
        return;
    }

    // Sync combat mode so IsArcherModeEnabled() matches the active plugin
    settings.combatMode = GetExpectedCombatMode();

    // Mutual exclusion: if another BaseHuntPlugin subclass is already enabled, skip
    for (auto* p = &PluginManager::Get();;) {
        // Check via GetPlugin iteration — we only need to ensure one BaseHuntPlugin runs.
        // Since GetPlugin returns the first match by dynamic_cast, and we ARE a BaseHuntPlugin,
        // just check if another one exists and is enabled.
        (void)p;
        break;
    }

    if (!hero || !map) {
        SetState(AutoHuntState::WaitingForGame, "Waiting for hero and map");
        return;
    }

    // Session 10: independent of the decision throttle below — paced on its
    // own timer so it can be dialed down for testing without being tied to
    // how often the rest of the decision logic runs.
    TryRandomWalk(hero, map, settings, GetTickCount());

    m_lootMgr.PruneLootPickupAttempts(map);
    PruneLootDropRecords();

    // Session 10: the state machine below (zone/death/safety checks, target
    // and loot scanning, pathfinding requests) was found running once per
    // rendered frame (~150Hz) with no gate of its own — only the final action
    // packet (jump/attack/pickup) was interval-throttled. That's the prime
    // suspect for a confirmed memory leak observed over long sessions (Task
    // Manager private/working-set memory climbing without plateauing). This
    // caps how often it runs, independent of the per-action interval sliders.
    // 0 disables the gate (restores the original unthrottled behavior).
    const DWORD decisionThrottleMs = GetDecisionThrottleMs(settings);
    if (decisionThrottleMs > 0) {
        const DWORD nowTick = GetTickCount();
        if (m_lastDecisionTick != 0 && (nowTick - m_lastDecisionTick) < decisionThrottleMs)
            return;
        m_lastDecisionTick = nowTick;
    }

    if (!HasValidZone(settings)) {
        // Session 10: say WHAT is missing. The generic message read as "the
        // feature is broken" when the actual cause was usually one specific
        // unset field — most often zoneMapId=0, or a polygon with no vertices
        // clicked yet.
        const char* why = "Configure a valid hunt zone first";
        if (settings.zoneMapId == 0)
            why = "Hunt zone has no map set - click 'Use Hero Position'";
        else if (settings.zoneMode == AutoHuntZoneMode::Polygon)
            why = "Polygon zone needs 3+ vertices - use 'Capture Polygon Vertices' and click the map";
        else if (settings.zoneMode == AutoHuntZoneMode::Route)
            why = "Route zone has no recorded route for this map - record one first";
        else if (IsZeroPos(settings.zoneCenter))
            why = "Circle zone has no center - click 'Use Hero Position'";
        else if (settings.zoneRadius <= 0)
            why = "Circle zone radius is 0";
        SetState(AutoHuntState::Failed, why);
        return;
    }

    if (HandleDeath(hero, travel, settings))
        return;

    // Player safety: only detect while hunting in zone, but always handle active rest
    if (m_safetyResting) {
        if (CheckPlayerSafety(hero, map, travel, settings))
            return;
    } else if (settings.safetyEnabled
               && Game::GetCurrentMapId() == settings.zoneMapId
               && IsPointInZone(settings, Game::GetCurrentMapId(), hero->m_posMap)) {
        if (CheckPlayerSafety(hero, map, travel, settings))
            return;
    } else {
        m_nearbyPlayerTicks.clear();
    }

    if (travel && travel->IsTraveling()) {
        if (m_state == AutoHuntState::TravelToMarket) {
            HandleTravelToMarket(travel, hero, settings);
            return;
        }
        if (m_state == AutoHuntState::TravelToZone || m_state == AutoHuntState::ReturnToZone) {
            HandleTravelToZone(travel, settings);
            return;
        }

        SetState(m_state, "Waiting for travel plugin");
        return;
    }

    UpdatePendingJumpState(hero, GetTickCount());

    // ── Pathfinder watchdog (session 10) ──
    // A path that stops progressing freezes the whole bot: StartPathTo()
    // returns "success" without acting while one is active, and the attack is
    // gated on !IsActive(). Observed live as the archer standing still for
    // over a minute with monsters adjacent, UI reporting "Jumping to scatter
    // clump" / "Scouting hunt zone" the entire time.
    //
    // The pathfinder has its own stall recovery, but it demonstrably did not
    // fire here, so this is a belt-and-braces cap: if the hero has not moved
    // while a path is active, tear the path down and let the loop re-decide.
    {
        const DWORD wnow = GetTickCount();
        if (Pathfinder::Get().IsActive()) {
            if (hero->m_posMap.x != m_watchdogLastPos.x || hero->m_posMap.y != m_watchdogLastPos.y) {
                m_watchdogLastPos = hero->m_posMap;
                m_watchdogLastMoveTick = wnow;
            } else if (m_watchdogLastMoveTick != 0
                       && wnow - m_watchdogLastMoveTick > kPathWatchdogMs) {
                spdlog::warn("[hunt] Pathfinder stuck at ({},{}) for {}ms with no movement - forcing stop",
                             hero->m_posMap.x, hero->m_posMap.y, wnow - m_watchdogLastMoveTick);
                Pathfinder::Get().Stop();
                ClearPendingJumpState();
                m_watchdogLastMoveTick = wnow;
            }
        } else {
            m_watchdogLastPos = hero->m_posMap;
            m_watchdogLastMoveTick = wnow;
        }
    }

    const DWORD now = GetTickCount();
    const bool manualControlPaused = TickIsFuture(m_manualControlPauseUntilTick, now);
    if (manualControlPaused) {
        SetState(AutoHuntState::Ready, "Manual control pause");
        return;
    }

    {
        const HuntBuffCallbacks buffCb = MakeBuffCallbacks(hero, map, settings);
        if (m_buffMgr.TryPreLandingSafety(hero, map, settings, buffCb))
            return;

        // Cast skills in priority order (flyOnlyWithCyclone is handled inside TryCastFly/XpFly)
        for (int i = 0; i < kHuntSkillCount; i++) {
            const auto& entry = settings.skillPriorities[i];
            if (!entry.enabled) continue;
            switch (entry.type) {
                case HuntSkillType::Superman:
                    if (m_buffMgr.TryCastSuperman(hero, settings, buffCb)) return;
                    break;
                case HuntSkillType::Cyclone:
                    if (m_buffMgr.TryCastCyclone(hero, settings, buffCb)) return;
                    break;
                case HuntSkillType::Accuracy:
                    if (m_buffMgr.TryCastAccuracy(hero, settings, buffCb)) return;
                    break;
                case HuntSkillType::XpFly:
                    if (m_buffMgr.TryCastXpFly(hero, settings, buffCb)) return;
                    break;
                case HuntSkillType::Fly:
                    if (m_buffMgr.TryCastFly(hero, settings, buffCb)) return;
                    break;
                case HuntSkillType::Stigma:
                    if (m_buffMgr.TryCastStigma(hero, settings, m_lastMana, buffCb)) return;
                    break;
                default: break;
            }
        }

        if (m_buffMgr.TryUsePotions(hero, settings, m_lastHp, m_lastMaxHp, m_lastMana, m_lastMaxMana, buffCb))
            return;
    }

    // Subclass combat-item management (arrows, etc.)
    if (HandleCombatItems(hero, settings))
        return;

    if (m_state == AutoHuntState::TravelToMarket) {
        HandleTravelToMarket(travel, hero, settings);
        return;
    }

    if (m_state == AutoHuntState::Repair) {
        m_townService.HandleRepairState(hero, map, settings, MakeTownCallbacks(travel, hero, settings));
        return;
    }

    if (m_state == AutoHuntState::BuyArrows) {
        m_townService.HandleBuyArrowsState(hero, map, settings, MakeTownCallbacks(travel, hero, settings));
        return;
    }

    if (m_state == AutoHuntState::StoreItems) {
        m_townService.HandleStoreState(hero, map, settings, MakeTownCallbacks(travel, hero, settings));
        return;
    }

    if (m_state == AutoHuntState::TravelToZone || m_state == AutoHuntState::ReturnToZone) {
        HandleTravelToZone(travel, settings);
        return;
    }

    // Meteor packing (shared)
    if (hero) {
        static DWORD s_lastPackDiagTick = 0;
        const DWORD diagNow = GetTickCount();
        if (diagNow - s_lastPackDiagTick >= 5000) {
            s_lastPackDiagTick = diagNow;
            spdlog::trace("[town-diag] packMeteorsIntoScrolls={} meteorCount={} autoRepair={} autoStore={} immediateReturnOnPriorityItems={}",
                settings.packMeteorsIntoScrolls, CountInventoryItemsByType(hero, ItemTypeId::METEOR),
                settings.autoRepair, settings.autoStore, settings.immediateReturnOnPriorityItems);
        }
    }
    if (settings.packMeteorsIntoScrolls && hero) {
        const DWORD packNow = GetTickCount();
        if (packNow - m_lastPackTick >= GetItemActionIntervalMs(settings)) {
            if (CountInventoryItemsByType(hero, ItemTypeId::METEOR) >= 10) {
                CItem* meteor = FindInventoryItemByType(hero, ItemTypeId::METEOR);
                if (meteor) {
                    hero->UseItem(meteor->GetID());
                    m_lastPackTick = packNow;
                    SetState(AutoHuntState::Recover, "Packing Meteors into MeteorScrolls");
                    return;
                }
            }
        }
    }

    // Repair / storage — at Market (arrows handled separately below)
    if (m_townService.NeedTownRun(hero, settings, false)) {
        BeginTravelToMarket(travel, hero, settings);
        return;
    }

    // Arrow restocking — buy at local blacksmith or travel to zone city
    if (NeedsTownRunArrows(hero, settings)) {
        if (HuntTownService::HasBlacksmithOnMap(m_lastMapId)) {
            m_townService.ResetBuyArrowsSequence();
            SetState(AutoHuntState::BuyArrows, "Buying arrows");
            return;
        }
        if (HuntTownService::HasBlacksmithOnMap(settings.zoneMapId)) {
            BeginTravelToZone(travel, settings);
            return;
        }
    }

    // Same leash tolerance as above — do not travel back to the zone just
    // because the hero stepped out to engage something on the edge.
    // Session 11 [Paranoia Mode]: widen the leash while a threat is
    // currently detected, so normal zone-return logic doesn't yank the hero
    // back toward (or past) the player mid-evasion. Reverts to the normal
    // leash the instant no threat is detected — the bot then settles back
    // toward the zone on its own via normal target/explore selection,
    // rather than needing an explicit "return to zone" step.
    Position paranoiaThreatPos{};
    const bool paranoiaEvading = GetParanoiaThreat(settings, &paranoiaThreatPos);
    const int leashMargin = paranoiaEvading ? GetHuntLeash(settings) * 3 : GetHuntLeash(settings);
    if (Game::GetCurrentMapId() != settings.zoneMapId
        || !IsPointNearHuntZone(settings, Game::GetCurrentMapId(), hero->m_posMap,
                                leashMargin)) {
        // Session 11 [LOCKUP FIX]: give up after repeated failures instead of
        // retrying forever — see m_zoneTravelFailCount's comment in the
        // header. A genuinely-unreachable zone (bad gateway data, wrong
        // zoneMapId) used to loop this every tick indefinitely with no way
        // to stop it short of editing the config file with the game closed.
        if (m_zoneTravelFailCount >= kMaxZoneTravelFailures) {
            spdlog::error("[hunt] Giving up reaching hunt zone after {} failed attempts — disabling hunting. Check zoneMapId ({}) and gateway routes.",
                m_zoneTravelFailCount, settings.zoneMapId);
            m_zoneTravelFailCount = 0;
            ApplyHuntModeSelection(settings.combatMode, false);
            SetState(AutoHuntState::Failed, "Could not reach hunt zone after repeated attempts — hunting disabled");
            return;
        }
        BeginTravelToZone(travel, settings);
        return;
    }

    if (travel && travel->IsTraveling()) {
        SetState(AutoHuntState::Ready, "Waiting for travel plugin");
        return;
    }

    // ── Phase 2a: bag-full trash drop ───────────────────────────────────
    // Drops one junk item per throttled tick when bag is at threshold so
    // there's room for incoming loot.  No-op unless the user enabled it
    // and configured at least one cutoff (quality or price).
    if (m_lootMgr.TryDropTrashItem(hero, settings, GetTickCount())) {
        SetState(AutoHuntState::Recover, "Dropping bag trash to make room");
        return;
    }

    // ── Loot phase ──────────────────────────────────────────────────────
    CMapItem* loot = m_lootMgr.FindBestLoot(hero, map, settings,
        [this](OBJID id, DWORD now) { return m_lootMgr.IsLootPickupIgnored(id, now); },
        [this, &settings](OBJID mapId, const Position& pos) { return IsPointInZone(settings, mapId, pos); });

    // Session 10 [CRASH FIX]: the ground-item scan (map_items.h) only
    // refreshes every 500ms, but a pickup frees the item's memory
    // immediately. Every Update() tick in that window used to hand back the
    // same now-dangling pointer and dereference it unguarded below — a hard
    // crash, and close to guaranteed rather than occasional, since normal
    // frame rates mean dozens of unguarded reads happen in that one window.
    // Re-validate before touching any field.
    if (loot && !MapItems::IsAlive(loot)) {
        loot = nullptr;
        MapItems::Invalidate();   // don't wait out the rest of the interval
    }

    const bool midMovement = hero->IsJumping() || Pathfinder::Get().IsActive();
    if (loot) {
        const int lootDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, loot->m_pos.x, loot->m_pos.y);
        spdlog::trace("[hunt-loot] Loot id={} type={} dist={} range={}", loot->m_id, loot->m_idType, lootDist, GetLootRange(settings));
        const DWORD pickupNow = GetTickCount();

        // Session 10 [PICKUP-DELAY FIX]: stop any in-flight route the moment
        // we're within pickup range, BEFORE attempting the pickup. Without
        // this, TryPickupLootItem()'s own "movement not settled" check bails
        // out before ever reaching its later Pathfinder::Get().Stop() call
        // whenever the hero is mid-jump crossing this tile as part of a
        // longer route (combat repositioning, an earlier farther loot pick,
        // etc.) — the queued waypoints then carry the hero straight past the
        // item, and with itemPickupDelayMs set the arrival timer never gets
        // a chance to complete since the hero is never actually stationary
        // here. This can't cancel an already-launched jump (a committed
        // client-side animation), but it does stop the pathfinder from
        // queuing the next waypoint once this one lands, so the hero
        // actually comes to rest on the item instead of coasting through.
        if (IsWithinLootPickupRange(settings, lootDist, *loot) && Pathfinder::Get().IsActive())
            Pathfinder::Get().Stop();

        if (IsWithinLootPickupRange(settings, lootDist, *loot) && m_lootMgr.TryPickupLootItem(hero, settings, loot, pickupNow,
                [this, hero](DWORD t) { return UpdatePendingJumpState(hero, t); })) {
            m_targetId = loot->m_id;
            SetState(AutoHuntState::LootNearby, "Picking up nearby loot");
            return;
        }

        // Session 11: don't let loot pull the hero away from an engageable
        // monster — only the free/no-detour pickup above (already attempted;
        // it fires regardless of nearby combat) is allowed to win against
        // combat. Cheap peek: FindBestTarget is a pure scan with no side
        // effects, and gets called again below for the real combat handling
        // — this just decides whether it's worth detouring for loot first.
        Position peekApproachPos{}, peekAttackPos{};
        int peekClumpSize = 0;
        bool peekUseScatter = false;
        const bool hasEngageableTarget = FindBestTarget(hero, map, settings,
            &peekApproachPos, &peekAttackPos, &peekClumpSize, &peekUseScatter) != nullptr;

        if (!midMovement && !hasEngageableTarget) {
            if (StartPathNearTarget(hero, map, loot->m_pos, kLootPathStopRange)) {
                m_targetId = loot->m_id;
                SetState(AutoHuntState::LootNearby, "Jumping to loot");
                return;
            }
            if (IsWithinLootPickupRange(settings, lootDist, *loot)) {
                // Session 10: reaching here means dist != 0 (the pickup
                // attempt above already handles dist==0) AND the pathfinder
                // failed to queue a route to close that gap. Previously this
                // just re-tried every frame (~150Hz) and usually self-corrected
                // within milliseconds; with the new decisionThrottleMs slider,
                // retries only happen every throttle interval, so a genuinely
                // unreachable item's exact tile (corner-blocked, occupied,
                // etc.) now visibly "sticks" for seconds at a time instead of
                // resolving invisibly fast. Feed it into the same
                // attempt-limit/ignore mechanism pickup failures already use,
                // so it gets deprioritized instead of fixated on regardless
                // of throttle setting.
                m_lootMgr.RecordLootPickupAttempt(loot->m_id, now, settings);
                m_targetId = loot->m_id;
                SetState(AutoHuntState::LootNearby, "Settling on nearby loot");
                return;
            }
            spdlog::warn("[hunt-loot] Failed to path to loot id={} at ({},{}) dist={}",
                loot->m_id, loot->m_pos.x, loot->m_pos.y, lootDist);
        }
    }

    // ── Combat phase (virtual dispatch) ─────────────────────────────────
    const bool hasPendingJump = UpdatePendingJumpState(hero, now);

    const bool approachCommitted = (m_state == AutoHuntState::ApproachTarget || m_state == AutoHuntState::LootNearby)
        && (hero->IsJumping() || hasPendingJump || Pathfinder::Get().IsActive());

    // Stuck detection
    if (m_state == AutoHuntState::ApproachTarget
        && !hero->IsJumping() && !hasPendingJump && !Pathfinder::Get().IsActive()) {
        if (m_approachStartTick == 0)
            m_approachStartTick = now;
        else if (now - m_approachStartTick > 3000) {
            m_unreachableTargetId = m_targetId;
            m_unreachableExpireTick = now + 10000;
            m_approachStartTick = 0;
            m_targetId = 0;
            SetState(AutoHuntState::AcquireTarget, "Target unreachable, finding new target");
        }
    } else {
        m_approachStartTick = 0;
    }

    if (m_unreachableTargetId != 0 && GetTickCount() >= m_unreachableExpireTick)
        m_unreachableTargetId = 0;

    // ── Target finding via virtual dispatch ──────────────────────────────
    Position approachPos = {};
    Position attackPos = {};
    int clumpSize = 0;
    bool useScatter = false;
    CRole* target = FindBestTarget(hero, map, settings, &approachPos, &attackPos, &clumpSize, &useScatter);

    // Session 10: same use-after-free shape already fixed for loot above
    // (MapItems::IsAlive()) but never applied to combat targets — target
    // comes from the same kind of periodically-refreshed background scan
    // (Entities::Get()) and gets dereferenced below (HandleCombatRetreat/
    // Approach/Attack all read target->m_posMap etc.) across several more
    // Update() ticks while approaching, plenty of time for the monster to
    // die and free its memory in between.
    if (target && !Entities::IsAlive(target)) {
        target = nullptr;
        Entities::Invalidate();
    }

    // Skip blacklisted unreachable target
    if (target && target->GetID() == m_unreachableTargetId)
        target = nullptr;

    m_lastClumpSize = clumpSize;
    m_lastTargetPos = target ? target->m_posMap : Position{};

    if (target) {
        m_targetId = target->GetID();
        const bool movementCommitted = hero->IsJumping() || hasPendingJump;

        // Combat retreat (virtual — archer overrides, melee returns false)
        if (HandleCombatRetreat(hero, map, settings, target))
            return;

        // Combat approach (virtual dispatch)
        HandleCombatApproach(hero, map, settings, target, approachPos, movementCommitted);

        // If approach has active movement, don't attack yet — wait for arrival
        if (m_state == AutoHuntState::ApproachTarget
            && (hero->IsJumping() || m_pendingJumpTick != 0 || Pathfinder::Get().IsActive()))
            return;

        // Combat attack (virtual dispatch)
        HandleCombatAttack(hero, map, settings, target, attackPos, now);
        return;
    }

    // ── No target found ─────────────────────────────────────────────────
    m_lastClumpSize = 0;
    if (loot) {
        spdlog::trace("[hunt-loot] No combat target, pathing to loot id={} type={} at ({},{})",
            loot->m_id, loot->m_idType, loot->m_pos.x, loot->m_pos.y);
        const bool startedMove = StartPathNearTarget(hero, map, loot->m_pos, kLootPathStopRange);
        if (!startedMove)
            m_lootMgr.RecordLootPickupAttempt(loot->m_id, now, settings);
        m_targetId = loot->m_id;
        SetState(AutoHuntState::LootNearby, startedMove ? "Jumping to loot" : "Settling on nearby loot");
        return;
    }

    // Subclass idle behavior (archer patrol, etc.)
    if (HandleNoTargetIdle(hero, map, settings))
        return;

    // Session 10 [DEAD END FIX]: with no target, no loot and no idle move, the
    // loop used to fall through to a bare SetState("Scanning for monsters")
    // and return — i.e. stand still forever whenever the hero happened to be
    // within 3 tiles of the anchor. Observed live: the bot parked itself for
    // 30+ seconds with monsters just outside its search radius, and only
    // resumed when the player physically dragged it somewhere else.
    //
    // Keep moving instead: pick somewhere else inside the zone and go look.
    // Monsters respawn and wander, so covering ground IS the search.
    Position explorePos{};
    if (FindZoneExplorePosition(hero, map, settings, explorePos)
        && StartPathTo(hero, map, explorePos, 0)) {
        m_targetId = 0;
        SetState(AutoHuntState::AcquireTarget, "Exploring zone for monsters");
        return;
    }

    // Only head back to the anchor if the anchor is somewhere we can actually
    // stand. A circle placed partly off the map (or over water/rock) has an
    // anchor in dead space, and pathing at it repeatedly is what made the bot
    // loiter against the unwalkable edge instead of hunting the usable part.
    const Position anchor = GetZoneAnchor(settings);
    if (!IsZeroPos(anchor)
        && map->IsWalkable(anchor.x, anchor.y)
        && CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, anchor.x, anchor.y) > 3) {
        StartPathTo(hero, map, anchor, 3);
        SetState(AutoHuntState::ReturnToZone, "Returning to hunt anchor");
        return;
    }

    m_targetId = 0;
    SetState(AutoHuntState::AcquireTarget, "Scanning for monsters (no reachable move)");
}

// ═════════════════════════════════════════════════════════════════════════════
// OnMapClick
// ═════════════════════════════════════════════════════════════════════════════

bool BaseHuntPlugin::OnMapClick(const Position& tile)
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    CGameMap* map = Game::GetMap();
    if (!map)
        return false;

    if (m_zoneCaptureMode == ZoneCaptureMode::None) {
        const DWORD pauseMs = GetManualControlPauseMs(settings);
        if (m_enabled && pauseMs > 0) {
            m_manualControlPauseUntilTick = GetTickCount() + pauseMs;
            Pathfinder::Get().Stop();
            ClearPendingJumpState();
            SetState(AutoHuntState::Ready, "Manual control pause");
        }
        return false;
    }

    settings.zoneMapId = Game::GetCurrentMapId();

    switch (m_zoneCaptureMode) {
        case ZoneCaptureMode::CircleCenter:
            settings.zoneCenter = tile;
            m_zoneCaptureMode = ZoneCaptureMode::None;
            snprintf(m_statusText, sizeof(m_statusText), "Zone center set to (%d,%d)", tile.x, tile.y);
            return true;

        case ZoneCaptureMode::CircleRadius:
            if (IsZeroPos(settings.zoneCenter)) {
                snprintf(m_statusText, sizeof(m_statusText), "Set circle center first");
            } else {
                settings.zoneRadius = (std::max)(1, CGameMap::TileDist(
                    settings.zoneCenter.x, settings.zoneCenter.y, tile.x, tile.y));
                snprintf(m_statusText, sizeof(m_statusText), "Zone radius set to %d", settings.zoneRadius);
            }
            m_zoneCaptureMode = ZoneCaptureMode::None;
            return true;

        case ZoneCaptureMode::PolygonVertex:
            settings.zonePolygon.push_back(tile);
            snprintf(m_statusText, sizeof(m_statusText), "Added polygon vertex (%d,%d)", tile.x, tile.y);
            return true;

        case ZoneCaptureMode::None:
        default:
            return false;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// RenderUI — shared settings UI
// ═════════════════════════════════════════════════════════════════════════════

void BaseHuntPlugin::RenderSelectedItemList(const char* title, const char* clearButtonLabel,
    const char* tableId, std::vector<uint32_t>& itemIds)
{
    ImGui::Text("%s: %d", title, (int)itemIds.size());
    ImGui::SameLine();
    if (ImGui::SmallButton(clearButtonLabel))
        itemIds.clear();

    if (itemIds.empty()) {
        ImGui::TextDisabled("No items selected.");
        return;
    }

    int removeIndex = -1;
    if (ImGui::BeginTable(tableId, 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
            ImVec2(0, 140.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < itemIds.size(); ++i) {
            const uint32_t itemId = itemIds[i];
            const ItemTypeInfo* info = GetItemTypeInfo(itemId);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", info ? info->name.c_str() : "Unknown");
            ImGui::TableNextColumn();
            ImGui::Text("%u", itemId);
            ImGui::TableNextColumn();
            char buttonId[64];
            snprintf(buttonId, sizeof(buttonId), "Remove##%s%u", tableId, itemId);
            if (ImGui::SmallButton(buttonId))
                removeIndex = (int)i;
        }

        ImGui::EndTable();
    }

    if (removeIndex >= 0)
        itemIds.erase(itemIds.begin() + removeIndex);
}

void BaseHuntPlugin::RenderItemSelector(AutoHuntSettings& settings)
{
    ImGui::InputText("Item Search", m_itemSearch, IM_ARRAYSIZE(m_itemSearch));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear Search"))
        m_itemSearch[0] = '\0';

    ImGui::Text("Loot: %d  Warehouse: %d  Priority Return: %d",
        (int)settings.lootItemIds.size(),
        (int)settings.warehouseItemIds.size(),
        (int)settings.priorityReturnItemIds.size());

    const std::string searchText = ToLowerCopy(m_itemSearch);
    int shown = 0;
    bool limitedResults = false;
    ImGui::BeginChild("##basehuntitembrowser", ImVec2(0, 240.0f), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##basehuntitemtable", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
            ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Loot", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Warehouse", ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableHeadersRow();

        for (const ItemTypeInfo* info : GetAllItemTypes()) {
            if (!info)
                continue;

            const bool matchesSearch =
                searchText.empty()
                || ToLowerCopy(info->name).find(searchText) != std::string::npos
                || std::to_string(info->id).find(m_itemSearch) != std::string::npos;
            if (!matchesSearch)
                continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", info->name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%u", info->id);

            ImGui::TableNextColumn();
            {
                const bool selected = ContainsItemId(settings.lootItemIds, info->id);
                char buttonId[64];
                snprintf(buttonId, sizeof(buttonId), "%s##loot%u", selected ? "Remove" : "Add", info->id);
                if (ImGui::SmallButton(buttonId)) {
                    if (selected)
                        RemoveItemId(settings.lootItemIds, info->id);
                    else
                        AddItemId(settings.lootItemIds, info->id);
                }
            }

            ImGui::TableNextColumn();
            {
                const bool selected = ContainsItemId(settings.warehouseItemIds, info->id);
                char buttonId[64];
                snprintf(buttonId, sizeof(buttonId), "%s##warehouse%u", selected ? "Remove" : "Add", info->id);
                if (ImGui::SmallButton(buttonId)) {
                    if (selected)
                        RemoveItemId(settings.warehouseItemIds, info->id);
                    else
                        AddItemId(settings.warehouseItemIds, info->id);
                }
            }

            ImGui::TableNextColumn();
            {
                const bool selected = ContainsItemId(settings.priorityReturnItemIds, info->id);
                char buttonId[64];
                snprintf(buttonId, sizeof(buttonId), "%s##priority%u", selected ? "Remove" : "Add", info->id);
                if (ImGui::SmallButton(buttonId)) {
                    if (selected)
                        RemoveItemId(settings.priorityReturnItemIds, info->id);
                    else
                        AddItemId(settings.priorityReturnItemIds, info->id);
                }
            }

            shown++;
            if (searchText.empty() && shown >= 250) {
                limitedResults = true;
                break;
            }
        }

        ImGui::EndTable();
    }

    if (shown == 0) {
        ImGui::TextDisabled("No item types matched the current filter.");
    } else if (limitedResults) {
        ImGui::TextDisabled("Showing the first 250 items. Use search to narrow the list.");
    }
    ImGui::EndChild();
}

BaseHuntPlugin* BaseHuntPlugin::FindHuntPluginForMode(AutoHuntCombatMode mode) const
{
    for (const auto& plugin : PluginManager::Get().GetPlugins()) {
        auto* hunt = dynamic_cast<BaseHuntPlugin*>(plugin.get());
        if (hunt && hunt->GetExpectedCombatMode() == mode)
            return hunt;
    }
    return nullptr;
}

BaseHuntPlugin* BaseHuntPlugin::GetSelectedModePlugin() const
{
    if (BaseHuntPlugin* exact = FindHuntPluginForMode(GetAutoHuntSettings().combatMode))
        return exact;

    for (const auto& plugin : PluginManager::Get().GetPlugins()) {
        if (auto* hunt = dynamic_cast<BaseHuntPlugin*>(plugin.get()))
            return hunt;
    }
    return nullptr;
}

void BaseHuntPlugin::SetAutomationEnabled(bool enabled)
{
    if (!enabled && m_enabled)
        StopAutomation(true);
    m_enabled = enabled;
}

void BaseHuntPlugin::ApplyHuntModeSelection(AutoHuntCombatMode mode, bool enabled)
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    bool wasAnyEnabled = false;
    for (const auto& plugin : PluginManager::Get().GetPlugins()) {
        if (auto* hunt = dynamic_cast<BaseHuntPlugin*>(plugin.get()); hunt && hunt->m_enabled)
            wasAnyEnabled = true;
    }

    settings.combatMode = mode;
    settings.archerMode = (mode == AutoHuntCombatMode::Archer);
    settings.enabled = enabled;

    for (const auto& plugin : PluginManager::Get().GetPlugins()) {
        auto* hunt = dynamic_cast<BaseHuntPlugin*>(plugin.get());
        if (!hunt)
            continue;

        const bool shouldEnable = enabled && hunt->GetExpectedCombatMode() == mode;
        if (!shouldEnable) {
            hunt->SetAutomationEnabled(false);
        } else {
            hunt->m_enabled = true;
            // Fresh enable gets a clean slate — don't inherit a stale fail
            // count from a previous session/attempt (see kMaxZoneTravelFailures).
            hunt->m_zoneTravelFailCount = 0;
        }
    }

    if (enabled && !wasAnyEnabled && HuntStats::GetSettings().autoResetOnEnable)
        HuntStats::Reset();
}

void BaseHuntPlugin::ResumeEnabledStateFromSettings()
{
    // Called once per plugin instance (melee AND archer both get it — see
    // dllmain.cpp), so this may run twice; ApplyHuntModeSelection() just
    // re-resolves to the same result both times, which is harmless.
    const AutoHuntSettings& settings = GetAutoHuntSettings();
    ApplyHuntModeSelection(settings.combatMode, settings.enabled);
}

void BaseHuntPlugin::RenderSkillPriorityUI(AutoHuntSettings& settings)
{
    bool skillChanged = false;
    for (int i = 0; i < kHuntSkillCount; ++i) {
        auto& entry = settings.skillPriorities[i];
        ImGui::PushID(i);

        char label[64];
        snprintf(label, sizeof(label), "%d. %s", i + 1, HuntSkillName(entry.type));
        if (ImGui::Checkbox(label, &entry.enabled))
            skillChanged = true;

        ImGui::SameLine();
        if (i > 0) {
            if (ImGui::SmallButton("^")) {
                std::swap(settings.skillPriorities[i], settings.skillPriorities[i - 1]);
                skillChanged = true;
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::SmallButton("^");
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (i < kHuntSkillCount - 1) {
            if (ImGui::SmallButton("v")) {
                std::swap(settings.skillPriorities[i], settings.skillPriorities[i + 1]);
                skillChanged = true;
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::SmallButton("v");
            ImGui::EndDisabled();
        }

        if (entry.enabled && entry.type == HuntSkillType::Fly) {
            ImGui::Indent();
            if (ImGui::Checkbox("Only Cast Fly With Cyclone", &settings.flyOnlyWithCyclone))
                skillChanged = true;
            ImGui::Unindent();
        }

        if (entry.enabled && entry.type == HuntSkillType::Stigma) {
            ImGui::Indent();
            if (ImGui::Checkbox("Pick Up Nearby Mana Potion For Stigma", &settings.pickupNearbyManaPotionForStigma))
                skillChanged = true;
            ImGui::Unindent();
        }

        ImGui::PopID();
    }

    if (skillChanged)
        settings.SyncSkillBoolsFromPriorities();
}

void BaseHuntPlugin::RenderZoneSetupUI(AutoHuntSettings& settings, CHero* hero)
{
    static const char* kZoneModes[] = { "Circle", "Polygon", "Route" };
    int zoneMode = static_cast<int>(settings.zoneMode);
    if (ImGui::Combo("Zone Shape", &zoneMode, kZoneModes, IM_ARRAYSIZE(kZoneModes))) {
        settings.zoneMode = static_cast<AutoHuntZoneMode>(zoneMode);
        m_zoneCaptureMode = ZoneCaptureMode::None;
    }

    ImGui::Text("Zone Map: %u", settings.zoneMapId);
    ImGui::InputInt2("Zone Center", &settings.zoneCenter.x);
    if (settings.zoneMode == AutoHuntZoneMode::Circle) {
        ImGui::SliderInt("Stay Within Zone Radius", &settings.zoneRadius, 1, 80);
        HelpMarkerOnSameLine("The bot tries to remain inside this circle.");
    }

    // ── Route mode (session 10) ──
    // Lives here, beside the zone shape selector, because a route IS a zone
    // shape — putting it in a separate debug tab made it undiscoverable.
    if (settings.zoneMode == AutoHuntZoneMode::Route) {
        const OBJID curMap = Game::GetCurrentMapId();
        static char routeName[64] = "route1";
        ImGui::Text("Current map: %u", curMap);
        ImGui::InputText("Route name", routeName, sizeof(routeName));

        if (!RouteRecordIsActive()) {
            if (ImGui::Button("Start Recording")) {
                RouteRecordStart();
                spdlog::info("[route] recording started on map {}", curMap);
            }
            HelpMarkerOnSameLine("Walk the patrol you want, then Stop & Save. "
                                 "The trail is reduced to corner waypoints.");
        } else {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1),
                               "RECORDING - %d tiles walked", RouteRecordSampleCount());
            if (ImGui::Button("Stop and Save")) {
                const bool ok = RouteRecordStop(settings, curMap, routeName);
                if (ok)
                    settings.zoneMapId = curMap;
                spdlog::info("[route] recording stopped, saved={}", ok);
            }
        }

        ImGui::Text("Saved routes:");
        for (const HuntRoute& r : settings.routes) {
            const bool active = (r.name == settings.activeRouteName);
            ImGui::Text("  %s%s  map=%u  %d waypoints",
                        active ? "* " : "  ", r.name.c_str(), r.mapId, (int)r.waypoints.size());
            if (r.mapId == curMap) {
                ImGui::SameLine();
                ImGui::PushID(r.name.c_str());
                if (ImGui::SmallButton("Use")) {
                    settings.activeRouteName = r.name;
                    settings.zoneMapId = r.mapId;
                }
                ImGui::PopID();
            }
        }
        if (settings.routes.empty())
            ImGui::TextDisabled("  (none recorded yet)");

        int linger = (int)settings.lingerMode;
        if (ImGui::Combo("Linger", &linger, "Auto\0Move on\0Stay and clear\0"))
            settings.lingerMode = (AutoHuntLingerMode)linger;
        HelpMarkerOnSameLine("Auto derives it from measured time-to-kill: one-shot "
                             "mobs = move on, slow kills = stay and clear.");
    }

    // The leash governs every zone shape, so show it regardless of mode.
    ImGui::TextDisabled("Leash: %d tiles out of zone / engage %d tiles out%s",
                        GetHuntLeash(settings), GetHuntEngageMargin(settings),
                        settings.routeCorridorOverride > 0 ? " (override)" : " (from attack range)");
    ImGui::InputInt("Leash override (0=auto)", &settings.routeCorridorOverride);
    HelpMarkerOnSameLine("How far the bot's body may leave the zone. It still only "
                         "attacks what is within striking range from where it stands.");

    if (hero && ImGui::Button("Use Hero Position")) {
        // Session 10 [FIXED]: this used m_lastMapId, which is only refreshed
        // inside Update() and is therefore 0 while the plugin is disabled — so
        // setting the zone before enabling the bot produced zoneMapId=0 and
        // "Configure a valid hunt zone first". It was masked until today
        // because the old Game::GetMap() returned a garbage pointer whose
        // GetId() yielded a consistent non-zero junk value that compared equal
        // to itself. Read the real map id directly instead.
        settings.zoneMapId = Game::GetCurrentMapId();
        settings.zoneCenter = hero->m_posMap;
    }

    if (settings.zoneMode == AutoHuntZoneMode::Circle) {
        ImGui::SameLine();
        if (ImGui::Button("Capture Center From Map"))
            m_zoneCaptureMode = ZoneCaptureMode::CircleCenter;
        ImGui::SameLine();
        if (ImGui::Button("Capture Radius From Map"))
            m_zoneCaptureMode = ZoneCaptureMode::CircleRadius;
    } else {
        ImGui::SameLine();
        if (ImGui::Button(m_zoneCaptureMode == ZoneCaptureMode::PolygonVertex
                ? "Stop Polygon Capture"
                : "Add Polygon Vertices")) {
            m_zoneCaptureMode = (m_zoneCaptureMode == ZoneCaptureMode::PolygonVertex)
                ? ZoneCaptureMode::None
                : ZoneCaptureMode::PolygonVertex;
            m_editDragVertex = -1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Polygon")) {
            settings.zonePolygon.clear();
            m_editDragVertex = -1;
        }
        ImGui::Text("Polygon Vertices: %d", (int)settings.zonePolygon.size());
    }

    const char* captureText = "Capture: idle";
    switch (m_zoneCaptureMode) {
        case ZoneCaptureMode::CircleCenter: captureText = "Capture: click the map to set the zone center."; break;
        case ZoneCaptureMode::CircleRadius: captureText = "Capture: click the map to set the zone radius."; break;
        case ZoneCaptureMode::PolygonVertex: captureText = "Capture: click the map to add polygon vertices."; break;
        case ZoneCaptureMode::None: break;
    }
    ImGui::TextDisabled("%s", captureText);
}

void BaseHuntPlugin::RenderMonsterFilterUI(AutoHuntSettings& settings, CRoleMgr* mgr)
{
    ImGui::InputText("Target Monster Names", settings.monsterNames, IM_ARRAYSIZE(settings.monsterNames));
    ImGui::InputText("Ignore Monster Names", settings.monsterIgnoreNames, IM_ARRAYSIZE(settings.monsterIgnoreNames));
    ImGui::InputText("Prefer Monster Names", settings.monsterPreferNames, IM_ARRAYSIZE(settings.monsterPreferNames));
    ImGui::TextDisabled("All lists are comma-separated. Ignore rules win over target rules.");
    ImGui::TextDisabled("Prefer names are tried first, then the bot falls back to other valid targets.");

    if (mgr && ImGui::TreeNode("Nearby Monster Names")) {
        std::vector<std::string> names;
        std::unordered_set<std::string> seen;
        const std::vector<CRole*> roles = Entities::Get();
        for (size_t i = 0; i < roles.size() && i < 500; ++i) {
            CRole* roleRef = roles[i];
            if (!roleRef || !roleRef->IsMonster())
                continue;

            const std::string name = roleRef->GetName();
            if (seen.insert(name).second)
                names.push_back(name);
        }

        std::sort(names.begin(), names.end());
        for (const std::string& name : names) {
            ImGui::PushID(name.c_str());
            ImGui::TextUnformatted(name.c_str());
            ImGui::SameLine(180.0f);
            if (ImGui::SmallButton("Target"))
                AppendFilterToken(settings.monsterNames, sizeof(settings.monsterNames), name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Ignore"))
                AppendFilterToken(settings.monsterIgnoreNames, sizeof(settings.monsterIgnoreNames), name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Prefer"))
                AppendFilterToken(settings.monsterPreferNames, sizeof(settings.monsterPreferNames), name.c_str());
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}

void BaseHuntPlugin::RenderQuickSetupSection(BaseHuntPlugin* /*modePlugin*/)
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    CHero* hero = Game::GetHero();
    CRoleMgr* mgr = Game::GetRoleMgr();

    bool huntEnabled = settings.enabled;
    int modeIndex = static_cast<int>(settings.combatMode);
    static const char* kModes[] = { "Melee", "Archer" };

    if (ImGui::Checkbox("Enable Hunting", &huntEnabled))
        ApplyHuntModeSelection(static_cast<AutoHuntCombatMode>(modeIndex), huntEnabled);
    if (ImGui::Combo("Mode", &modeIndex, kModes, IM_ARRAYSIZE(kModes)))
        ApplyHuntModeSelection(static_cast<AutoHuntCombatMode>(modeIndex), huntEnabled);

    ImGui::SeparatorText("Set Hunt Zone");
    RenderZoneSetupUI(settings, hero);

    ImGui::SeparatorText("Target Monsters");
    RenderMonsterFilterUI(settings, mgr);

    ImGui::SeparatorText("Ready Check");
    const bool hasValidZone = HasValidZone(settings);
    const bool hasHero = hero != nullptr;
    ImGui::TextColored(hasValidZone ? ImVec4(0.45f, 0.90f, 0.55f, 1.0f) : ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
        "Zone: %s", hasValidZone ? "Configured" : "Missing or invalid");
    ImGui::TextColored(hasHero ? ImVec4(0.45f, 0.90f, 0.55f, 1.0f) : ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
        "Hero: %s", hasHero ? "Ready" : "Waiting for game data");
    ImGui::Text("Selected Mode: %s", CombatModeLabel(settings.combatMode));
    ImGui::Text("Current State: %s", GetStateName());
    ImGui::TextWrapped("Current Reason: %s", m_statusText);

    std::string currentTarget = "None";
    if (mgr && m_targetId != 0) {
        const std::vector<CRole*> roles = Entities::Get();
        for (size_t i = 0; i < roles.size() && i < 500; ++i) {
            CRole* roleRef = roles[i];
            if (roleRef && roleRef->GetID() == m_targetId) {
                currentTarget = std::string(roleRef->GetName()) + " (" + std::to_string(m_targetId) + ")";
                break;
            }
        }
    }
    const uint32_t silver = hero ? hero->GetSilver() : 0;
    const int arrowPacks = (hero && settings.arrowTypeId != 0)
        ? CountInventoryItemsByType(hero, settings.arrowTypeId)
        : 0;
    ImGui::TextWrapped("Current Target: %s", currentTarget.c_str());
    ImGui::Text("Bag Count: %d / %d", (int)m_lastBagCount, CHero::MAX_BAG_ITEMS);
    ImGui::Text("Silver: %u", silver);
    ImGui::Text("Arrow Packs: %d", arrowPacks);

    HuntStats::RenderUI();
}

void BaseHuntPlugin::RenderCombatSection()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    ImGui::TextDisabled("Combat behavior for %s mode.", CombatModeLabel(settings.combatMode));
    ImGui::TextDisabled("Mode-specific options (Scatter, safety distance, etc.) live on the "
        "Archer Hunt / Melee Hunt tab below.");

    ImGui::SeparatorText("Targeting");
    ImGui::SliderInt("Only Target Mobs Within", &settings.mobSearchRange, 0, CGameMap::MAX_JUMP_DIST);
    HelpMarkerOnSameLine("0 means unlimited target search range.");

    ImGui::SeparatorText("Recovery");
    ImGui::Checkbox("Use Potions", &settings.usePotions);
    if (settings.usePotions) {
        ImGui::SliderInt("HP Potion %", &settings.hpPotionPercent, 1, 99);
        ImGui::SliderInt("Mana Potion %", &settings.manaPotionPercent, 1, 99);
        ImGui::Checkbox("Pick Up Nearby HP Potion When Low", &settings.pickupNearbyHpPotionWhenLow);
    }

    ImGui::SeparatorText("Skill Priority");
    RenderSkillPriorityUI(settings);
}

void BaseHuntPlugin::RenderLootSection()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    ImGui::Checkbox("Loot Silver / Gold / Money", &settings.lootMoney);
    static const char* kGoldTierNames[] = { "Silver and better", "Sycee and better",
        "Gold and better", "Gold Bullion and better", "Gold Bar and better", "Gold Bars only" };
    ImGui::Combo("Minimum Gold Tier", &settings.minimumGoldTier, kGoldTierNames, IM_ARRAYSIZE(kGoldTierNames));
    HelpMarkerOnSameLine("Money drops below this tier are ignored entirely.");
    ImGui::SliderInt("Loot Range", &settings.lootRange, 0, CGameMap::MAX_JUMP_DIST);
    HelpMarkerOnSameLine("Only limits how far to detour for money drops. Plus items and Meteors/DragonBalls are always fetched regardless of range.");
    ImGui::SliderInt("Minimum Loot Plus", &settings.minimumLootPlus, 0, 12);
    ImGui::InputInt("Minimum Sell Value to Loot", &settings.minimumLootGoldValue, 100, 1000);
    if (settings.minimumLootGoldValue < 0)
        settings.minimumLootGoldValue = 0;
    HelpMarkerOnSameLine("Items in the Loot list always bypass this value filter.");

    ImGui::Text("Loot by quality:");
    ImGui::Checkbox("Refined##basehuntlootqualityrefined", &settings.lootRefined);
    ImGui::SameLine();
    ImGui::Checkbox("Unique##basehuntlootqualityunique", &settings.lootUnique);
    ImGui::SameLine();
    ImGui::Checkbox("Elite##basehuntlootqualityelite", &settings.lootElite);
    ImGui::SameLine();
    ImGui::Checkbox("Super##basehuntlootqualitysuper", &settings.lootSuper);

    ImGui::SliderInt("Ignore Failed Pickup For (ms)", &settings.lootPickupIgnoreMs,
        kMinLootPickupIgnoreMs, kMaxLootPickupIgnoreMs);
    ImGui::SliderInt("Wait Before Picking New Drops (ms)", &settings.lootSpawnGraceMs,
        kMinLootSpawnGraceMs, kMaxLootSpawnGraceMs);
    ImGui::TextDisabled("Money is picked up even if it is not in the Loot list.");
    ImGui::TextDisabled("Items in the Loot list always bypass value filters.");

    ImGui::SeparatorText("Selected Loot Items");
    RenderSelectedItemList("Loot Item List", "Clear Loot List", "##lootselected", settings.lootItemIds);
    ImGui::SeparatorText("Item Browser");
    RenderItemSelector(settings);
}

void BaseHuntPlugin::RenderTownRunsSection()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    ImGui::Checkbox("Auto Repair", &settings.autoRepair);
    ImGui::SliderInt("Repair At Or Below %", &settings.repairPercent, 1, 100);
    ImGui::Checkbox("Auto Store", &settings.autoStore);
    ImGui::SliderInt("Go To Town When Bag Has", &settings.bagStoreThreshold, 1, CHero::MAX_BAG_ITEMS);
    ImGui::Checkbox("Return to Town Immediately for Priority Items", &settings.immediateReturnOnPriorityItems);
    ImGui::Checkbox("Pack Meteors into Meteor Scrolls", &settings.packMeteorsIntoScrolls);
    HelpMarkerOnSameLine("Self-service crafting, no NPC required - fires automatically anywhere once 10+ raw Meteors are in the bag.");

    ImGui::SeparatorText("Bank Deposits");
    ImGui::Checkbox("Auto-Deposit Meteors/DragonBalls at Treasure Bank", &settings.storeTreasureBank);
    ImGui::Checkbox("Auto-Deposit +1/+2 Gear at Compose Bank", &settings.storeComposeBank);
    HelpMarkerOnSameLine("Items matching an entry on the Warehouse or Priority Return list below always go to the Warehouse instead, even if these are checked.");
    ImGui::Checkbox("Auto-Deposit Silver", &settings.autoDepositSilver);
    if (settings.autoDepositSilver)
        ImGui::InputInt("Keep This Much Silver On Hand", &settings.silverKeepAmount, 1000, 10000);
    if (settings.silverKeepAmount < 0)
        settings.silverKeepAmount = 0;

    ImGui::SeparatorText("Arrow Restock");
    ImGui::Checkbox("Buy Arrows", &settings.buyArrows);
    if (settings.buyArrows)
        ImGui::SliderInt("Keep This Many Arrow Packs", &settings.arrowBuyCount, 1, 10);

    ImGui::SeparatorText("Keep Lists");
    RenderSelectedItemList("Warehouse Item List", "Clear Warehouse List", "##warehouseselected", settings.warehouseItemIds);
    RenderSelectedItemList("Priority Return Item List", "Clear Priority List", "##priorityselected", settings.priorityReturnItemIds);
}

void BaseHuntPlugin::RenderSafetySection()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    ImGui::Checkbox("Auto Revive In Town", &settings.autoReviveInTown);
    ImGui::SliderInt("Pause After Manual Map Click (ms)", &settings.manualControlPauseMs,
        kMinManualControlPauseMs, kMaxManualControlPauseMs);

    ImGui::SeparatorText("Player Safety");
    ImGui::InputText("Whitelist", settings.playerWhitelist, IM_ARRAYSIZE(settings.playerWhitelist));
    HelpMarkerOnSameLine("Shared by Player Safety and Paranoia Mode below - whitelisted names never count as a threat for either.");
    if (settings.safetyEnabled || settings.paranoiaEnabled)
        ImGui::SliderInt("Detection Range", &settings.safetyPlayerRange, 0, 30);

    ImGui::Checkbox("Player Safety", &settings.safetyEnabled);
    HelpMarkerOnSameLine("Full retreat: once a player has been nearby too long, travels to Market and idles until they've been gone a while.");
    if (settings.safetyEnabled) {
        ImGui::SliderInt("Detection Time (s)", &settings.safetyDetectionSec, 5, 300);
        ImGui::SliderInt("Rest Time (s)", &settings.safetyRestSec, 10, 600);
        ImGui::Checkbox("Discord Notify", &settings.safetyNotifyDiscord);
    }

    ImGui::Checkbox("Paranoia Mode", &settings.paranoiaEnabled);
    HelpMarkerOnSameLine(
        "Distance-based evasion, distinct from Player Safety above - keeps hunting instead of retreating. "
        "While a non-whitelisted player is within Detection Range: prefers monster targets that don't pull "
        "the hero closer to them, idle-exploration heads away from them instead of toward the zone/heatmap, "
        "and the zone leash loosens so normal zone-return logic doesn't pull the hero back toward the player. "
        "Does not apply to town runs or mining. There's no way to know what's actually on another player's "
        "screen (no facing/camera data is readable) - this is positioning-based avoidance, not true stealth.");
}

void BaseHuntPlugin::RenderAdvancedSection()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    ImGui::TextDisabled("Advanced: only change these if actions are too fast or too slow.");
    ImGui::SliderInt("Movement Interval (ms)", &settings.movementIntervalMs, kMinMovementIntervalMs, kMaxMovementIntervalMs);
    ImGui::SliderInt("Attack Interval (ms)", &settings.attackIntervalMs, kMinAttackIntervalMs, kMaxAttackIntervalMs);
    ImGui::SliderInt("Cyclone Attack Interval (ms)", &settings.cycloneAttackIntervalMs, kMinAttackIntervalMs, kMaxAttackIntervalMs);
    ImGui::SliderInt("Target Switch Delay (ms)", &settings.targetSwitchAttackIntervalMs,
        kMinTargetSwitchAttackIntervalMs, kMaxTargetSwitchAttackIntervalMs);
    ImGui::SliderInt("Item Action Interval (ms)", &settings.itemActionIntervalMs, kMinItemActionIntervalMs, kMaxItemActionIntervalMs);
    ImGui::SliderInt("Self Cast Interval (ms)", &settings.selfCastIntervalMs, kMinSelfCastIntervalMs, kMaxSelfCastIntervalMs);
    ImGui::SliderInt("Delay Between NPC Actions (ms)", &settings.npcActionIntervalMs, kMinNpcActionIntervalMs, kMaxNpcActionIntervalMs);
    ImGui::SliderInt("Revive Delay (ms)", &settings.reviveDelayMs, kMinReviveDelayMs, kMaxReviveDelayMs);
    ImGui::SliderInt("Revive Retry Interval (ms)", &settings.reviveRetryIntervalMs,
        kMinReviveRetryIntervalMs, kMaxReviveRetryIntervalMs);

    ImGui::SeparatorText("Background Scanning");
    ImGui::SliderInt("Entity/Item Scan Interval (ms)", &settings.entityScanIntervalMs,
        kMinEntityScanIntervalMs, kMaxEntityScanIntervalMs);
    ImGui::TextWrapped("How often the ground-item and monster scanners refresh. Lower = "
        "faster reaction to spawns/pickups but more CPU load; higher = cheaper but staler.");
    ImGui::SliderInt("Item Pickup Delay (ms)", &settings.itemPickupDelayMs,
        kMinItemPickupDelayMs, kMaxItemPickupDelayMs);
    ImGui::TextWrapped("How long to wait after arriving on an item's tile before the first "
        "pickup attempt. 0 = pick up immediately on arrival.");
    ImGui::SliderInt("Decision Throttle (ms)", &settings.decisionThrottleMs,
        kMinDecisionThrottleMs, kMaxDecisionThrottleMs);
    ImGui::TextWrapped("How often the bot re-evaluates targets/loot/pathing. Was found running "
        "unthrottled every rendered frame (~150Hz) alongside a confirmed memory leak over long "
        "sessions - this caps it independently of the action-interval sliders above. 0 = "
        "unthrottled (original behavior). Higher = less CPU/memory churn, slightly slower reactions.");
    ImGui::SliderInt("Random Walk Interval (ms)", &settings.randomWalkIntervalMs,
        kMinRandomWalkIntervalMs, kMaxRandomWalkIntervalMs);
    ImGui::TextWrapped("Periodically takes a short 1-2 tile walk instead of the usual jump, purely "
        "to mix real walking into normal play. A random 50-250ms is always ADDED on top of this "
        "value (never subtracted), so the actual cadence is never perfectly periodic. 0 disables it.");
}

void BaseHuntPlugin::RenderDebugSection()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    ImGui::Text("Current State: %s", GetStateName());
    ImGui::TextWrapped("Current Reason: %s", m_statusText);
    ImGui::Text("Map / Position: %u @ (%d, %d)", m_lastMapId, m_lastHeroPos.x, m_lastHeroPos.y);
    ImGui::Text("Last Target: %u", m_targetId);
    ImGui::SeparatorText("Overlay Toggles");
    ImGui::Checkbox("Show Action Radius", &settings.debugShowActionRadius);
    ImGui::Checkbox("Show Clump Radius", &settings.debugShowClumpRadius);
    ImGui::Checkbox("Show Mob Search Range", &settings.debugShowMobSearchRange);
    ImGui::Checkbox("Show Loot Range", &settings.debugShowLootRange);
    ImGui::Checkbox("Show Safety Range", &settings.debugShowSafetyRange);
    ImGui::Checkbox("Show Attack Range", &settings.debugShowAttackRange);
    ImGui::Checkbox("Show Archer Safety", &settings.debugShowArcherSafety);
    ImGui::Checkbox("Show Scatter Range", &settings.debugShowScatterRange);
    ImGui::Checkbox("Show Best Mob Clump", &settings.debugShowBestClump);
}

void BaseHuntPlugin::RenderDashboardUI()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    BaseHuntPlugin* modePlugin = GetSelectedModePlugin();
    if (!modePlugin) {
        ImGui::TextDisabled("No hunt modes are available.");
        return;
    }

    settings.enabled = false;
    for (const auto& plugin : PluginManager::Get().GetPlugins()) {
        if (auto* hunt = dynamic_cast<BaseHuntPlugin*>(plugin.get()); hunt && hunt->m_enabled) {
            settings.enabled = true;
            settings.combatMode = hunt->GetExpectedCombatMode();
            settings.archerMode = settings.combatMode == AutoHuntCombatMode::Archer;
            modePlugin = hunt;
            break;
        }
    }

    ImGui::TextDisabled("Workflow view: start in Quick Setup, then tune Combat, Loot, and Town Runs.");
    if (ImGui::BeginTabBar("##huntworkflowtabs")) {
        if (ImGui::BeginTabItem("Quick Setup")) {
            RenderQuickSetupSection(modePlugin);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Combat")) {
            RenderCombatSection();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Loot")) {
            RenderLootSection();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Town Runs")) {
            RenderTownRunsSection();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Safety")) {
            RenderSafetySection();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Advanced")) {
            RenderAdvancedSection();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug")) {
            RenderDebugSection();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void BaseHuntPlugin::RenderGeneralUI()
{
    RenderDashboardUI();
}

void BaseHuntPlugin::RenderUI()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();

    bool enabled = GetAutoHuntSettings().enabled && GetAutoHuntSettings().combatMode == GetExpectedCombatMode();
    if (ImGui::Checkbox("Enable This Mode", &enabled))
        ApplyHuntModeSelection(GetExpectedCombatMode(), enabled);
    ImGui::TextDisabled("Use the Hunting page for the full workflow layout.");
    ImGui::Separator();
    RenderCombatUI(settings);
}
