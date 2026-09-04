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
#include "hunt_contest.h"
#include "map_probe.h"
#include "hooks.h"
#include "gateway.h"
#include "CHero.h"
#include "CGameMap.h"
#include "CRole.h"
#include "config.h"
#include "discord.h"
#include "hunt_stats.h"
#include "profiles.h"
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

// Roughly the game's real monster/entity visibility range — the distance
// within which the client actually gets told about what's there at all, so
// "the hero was near enough to check" means near enough in ACTUAL tiles, not
// scaled by whatever the dynamic zone's own cell size happens to be (a 37+
// tile cell made "checked" mean "anywhere in a 100+ tile area" before this
// was pulled out as its own constant — see the dyn-zone shrink logic).
// Shared with GetEvadeRelocateMinDist's "beyond view" distance below.
constexpr int kViewTiles = 35;

// Session 18 [EVADE ESCAPE VALVE]: EvadeState::Relocating's renudge loop had
// no cap — live 2026-09-04 an archer sat "pushing farther" 100+ times over
// 20+ minutes against a threat that never left view (a heap-scan false-
// positive entity — see entities.cpp's level-plausibility fix — is one way
// this can happen, but the escalation itself has no upper bound regardless
// of cause, same structural gap as StartPathNearTarget's before its own
// fix). GetEvadeRelocateMinDist grows LINEARLY with renudge count
// (kViewTiles + renudge*paranoiaRenudgeTiles, default +10 tiles/renudge) —
// past a handful of renudges it's asking for a relocate destination farther
// than most maps are wide, which is exactly the "stuck in a map corner"
// symptom: FindParanoiaRelocateDest can only ever return the farthest tile
// actually available, never satisfying the ever-growing minimum. Past this
// many renudges, stop escalating and just resume hunting from wherever the
// hero ended up — matches every other "N failures -> give up and move on"
// pattern already used elsewhere in this codebase (m_zoneTravelFailCount,
// the repair/store step fail counters in hunt_town.cpp).
constexpr int kMaxEvadeRenudgeCount = 8;

// Session 16 [USER-TUNABLE CELL SIZE]: the Phase 2b dynamic zone's own grid,
// independent of SpawnMemory's fixed 8-tile heatmap bucket grid — see
// settings.dynZoneCellTiles. Declared up here (not down with the rest of the
// dynamic-zone engine) because FindZoneExplorePosition, earlier in this file,
// needs it too.
uint32_t DynCellKeyFor(const Position& tile, int cellTiles)
{
    return ((uint32_t)(tile.x / cellTiles) << 16) | (uint32_t)((tile.y / cellTiles) & 0xFFFF);
}

Position DynCellCenterOf(uint32_t key, int cellTiles)
{
    return { (int)(key >> 16) * cellTiles + cellTiles / 2,
             (int)(key & 0xFFFF) * cellTiles + cellTiles / 2 };
}

constexpr int kReliableAttackRange = 1;
constexpr int kMinMobClumpSize = 2;
constexpr int kMinLootRange = 0;
constexpr int kMaxLootRange = CGameMap::MAX_JUMP_DIST;
constexpr int kLootPathStopRange = 0;
// See StartPathTo's m_pathFailBackoffUntilTick use — how long to pause
// retrying a walk-only A* route after Pathfinder::StartPath() fails to
// actually issue movement (occupied/unreachable landing tile).
constexpr DWORD kPathFailBackoffMs = 800;
// See StartPathNearTarget's m_nearTargetStuckTile/m_nearTargetExcludedTile
// use — how long a candidate tile can keep getting picked with zero hero
// movement before it's treated as blocked, and how long it stays excluded
// once it is. Deliberately longer than a few backoff cycles (kPathFailBackoffMs)
// so a normal brief stall never misfires this — this is for the case where
// backoff alone never recovers.
constexpr DWORD kNearTargetStuckTimeoutMs = 6000;
constexpr DWORD kNearTargetExcludeCooldownMs = 30000;
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
    // Session 14: urgent loot (meteor/DragonBall + wanted quality/+1) is
    // fetched regardless of Loot Range, matching its selection-side range
    // bypass in FindBestLoot.
    if (HuntTownService::IsUrgentLootItem(settings, item))
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
        case AutoHuntState::TravelToMarket:     return "Travel To Market";
        case AutoHuntState::TravelToBlacksmith: return "Travel To Blacksmith";
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
    m_dynInit = false;   // Phase 2: re-seed the dynamic zone on next start
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
    const AutoHuntSettings& settings, Position& out, const std::vector<Position>* fleeFromSet) const
{
    out = {};
    if (!hero || !map)
        return false;

    const Position heroPos = hero->m_posMap;
    const DWORD now = GetTickCount();

    // Prune expired tabu entries (see m_recentExploreDests) so the list stays
    // a handful of live entries rather than growing all session.
    m_recentExploreDests.erase(
        std::remove_if(m_recentExploreDests.begin(), m_recentExploreDests.end(),
            [now](const auto& e) { return now - e.second >= 20000; }),
        m_recentExploreDests.end());

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

    // Session 13 [AutoHunt exploration incentive]: without this, exploration
    // is a pure argmax over whatever's sampled — a hot bucket found early
    // dominates every future decision (it's near-guaranteed to be among the
    // 40 samples and near-guaranteed to score highest), so nothing ever
    // pulls the bot toward genuinely uncharted territory to fill out the
    // rest of the map's density picture. Rolled once per call: on an
    // "explore" roll, keep the LOWEST-scoring candidate instead of the
    // highest, biasing toward buckets SpawnMemory hasn't observed (or
    // hasn't observed recently). 0 = disabled (today's pure-exploit
    // behavior); only meaningful once useHeatmap is true — before that,
    // sampling is already uniform/first-valid.
    const bool exploring = useHeatmap && settings.explorationChancePercent > 0
        && (int)(NextRandom32() % 100u) < settings.explorationChancePercent;
    Position worstScored{};
    float    worstScore = (std::numeric_limits<float>::max)();

    // Session 11 [Paranoia Mode]: while a threat is detected, evasion takes
    // priority over normal exploration (heatmap-guided or first-valid) —
    // sample every candidate and keep the one farthest from the threat
    // instead of stopping at the first walkable tile.
    // fleeFromSet (evade state machine) forces the away-from-ALL-players
    // selection: keep the tile whose distance to the NEAREST point in the set
    // is largest. The set is every tracked player's current + predicted
    // position, so this heads away from the whole group and around where a
    // moving player is going. When no set is passed, the live nearest-threat
    // scan drives the ordinary idle-exploration bias (single point).
    Position threatPos{};
    bool paranoiaEvading;
    const bool fleeAll = (fleeFromSet && !fleeFromSet->empty());
    if (fleeAll) {
        paranoiaEvading = true;
    } else {
        paranoiaEvading = (fleeFromSet == nullptr) && GetParanoiaThreat(settings, &threatPos);
    }
    Position bestAwayFromThreat{};
    float    bestThreatDist = -1.0f;
    // Session 14 [SPLIT-MAP FLEE]: collect ALL flee candidates so the farthest
    // tile the hero can ACTUALLY reach is chosen post-loop. On maps with an
    // impassable center (two halves joined by a single bridge) the
    // geometrically-farthest tile can be across the barrier; committing to it
    // with only an IsWalkable() check left the bot with no path and it froze.
    // A bounded FindPath picks a reachable spot (routing around via the bridge),
    // and a guaranteed adjacent-step fallback ensures it never just stands still.
    std::vector<std::pair<Position, float>> fleeCandidates;

    // Distance from a candidate to the nearest threat point — the evade
    // objective is to MAXIMIZE this (get as far as possible from whoever is
    // closest to seeing us).
    auto distToNearestThreat = [&](const Position& c) -> float {
        if (fleeAll) {
            float minD = (std::numeric_limits<float>::max)();
            for (const Position& tp : *fleeFromSet)
                minD = (std::min)(minD, c.DistanceTo(tp));
            return minD;
        }
        return c.DistanceTo(threatPos);
    };

    // Session 13 [KNOWN HOT SPOTS]: uniform sampling alone almost never
    // rediscovers a small hot area on a large map — live MapWide run: the
    // best sampled candidate scored 5-40% of the known max while the heatmap
    // held 400-point buckets the sampler never hit, so every "exploit" pick
    // was effectively random and the bot beelined corner to corner. Feed the
    // top-scored buckets we ALREADY KNOW straight into the candidate pool;
    // they pass through the same zone/walkable/contested validation and the
    // same travel-cost discount as sampled candidates.
    const std::vector<Position> hotSpots = useHeatmap
        ? SpawnMemory::GetHotBuckets(mapId, 12)
        : std::vector<Position>{};
    // Phase 2b: active dynamic-zone cells, snapshotted once so the sampling
    // loop below can index into them without walking the unordered_set
    // per-attempt.
    const std::vector<uint32_t> dynCellKeys(m_dynCells.begin(), m_dynCells.end());
    const int totalAttempts = 40 + (int)hotSpots.size();

    for (int attempt = 0; attempt < totalAttempts; ++attempt) {
        Position candidate{};

        if (attempt >= 40) {
            candidate = hotSpots[attempt - 40];
        } else if (settings.zoneMode == AutoHuntZoneMode::MapWide) {
            // Phase 2b: when the dynamic-zone engine is active (and we're not
            // fleeing), sample inside one of its active CELLS instead of the
            // whole map — this is what focuses Map-Wide on the current hot
            // area instead of beelining corner to corner. Fleeing keeps
            // full-map freedom to escape.
            if (IsDynamicZoneActive(settings) && !fleeAll && !dynCellKeys.empty()) {
                const Position c = DynCellCenterOf(
                    dynCellKeys[NextRandom32() % dynCellKeys.size()], m_dynCellTiles);
                const int bt = m_dynCellTiles;
                candidate.x = c.x - bt / 2 + (int)(NextRandom32() % (uint32_t)bt);
                candidate.y = c.y - bt / 2 + (int)(NextRandom32() % (uint32_t)bt);
            } else {
                if (map->m_sizeMap.iWidth <= 0 || map->m_sizeMap.iHeight <= 0)
                    return false;
                candidate.x = (int)(NextRandom32() % (uint32_t)map->m_sizeMap.iWidth);
                candidate.y = (int)(NextRandom32() % (uint32_t)map->m_sizeMap.iHeight);
            }
        } else if (settings.zoneMode == AutoHuntZoneMode::Route) {
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
        // Phase 2b: keep exploration inside the dynamic zone's active cells
        // (also rejects injected hot-spot buckets that fall outside it).
        // Not while fleeing.
        if (!paranoiaEvading && IsDynamicZoneActive(settings) && !InDynamicZone(candidate))
            continue;
        if (!map->IsWalkable(candidate.x, candidate.y))
            continue;
        if (CGameMap::TileDist(heroPos.x, heroPos.y, candidate.x, candidate.y) < minTravel)
            continue;
        // Explore tabu (see m_recentExploreDests): skip spots visited within
        // the last 20s so consecutive picks sweep NEW ground instead of
        // ping-ponging between the two hottest buckets. Paranoia evasion is
        // exempt — fleeing back through a recent spot is fine.
        if (!paranoiaEvading) {
            constexpr DWORD kExploreTabuMs = 20000;
            constexpr int kExploreTabuRadius = 10;
            bool tabu = false;
            for (const auto& [pos, tick] : m_recentExploreDests) {
                if (now - tick < kExploreTabuMs
                    && CGameMap::TileDist(pos.x, pos.y, candidate.x, candidate.y) <= kExploreTabuRadius) {
                    tabu = true;
                    break;
                }
            }
            if (tabu)
                continue;
        }

        // Session 13 [Paranoia camping-detection]: don't let the bot walk
        // onto a bucket another player has been camping — distinct from
        // ordinary Paranoia evasion above, which reacts to whoever is
        // NEAREST right now. This specifically remembers a player who has
        // been sitting on THIS bucket continuously, so a genuinely camped
        // spot stays avoided even between exploration decisions where no
        // player happens to be the single closest threat. See hunt_contest.h.
        if (settings.paranoiaEnabled && HuntContest::IsBucketContested(mapId, candidate, settings))
            continue;

        if (paranoiaEvading) {
            const float threatDist = distToNearestThreat(candidate);
            fleeCandidates.emplace_back(candidate, threatDist);
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
        //
        // Session 13 [TRAVEL COST]: the exploit pick discounts a candidate's
        // score by how far away it is — raw argmax treated a bucket 500 tiles
        // across a MapWide zone as equal to one 20 tiles away, so near-tied
        // scores (e.g. many novelty-boosted buckets) had the bot ping-ponging
        // corner to corner doing nothing but travel. Halving distance is 96
        // tiles: nearby ties win outright, and a far bucket now needs to be
        // genuinely hotter — not merely tied — to justify the trip. The
        // exploration roll below stays on RAW score: deliberately going
        // somewhere far and unknown is that path's entire purpose.
        const float score = SpawnMemory::GetScore(mapId, candidate);
        const float travelDiscounted = score / (1.0f + heroPos.DistanceTo(candidate) / 96.0f);
        if (travelDiscounted > bestScore) {
            bestScore = travelDiscounted;
            bestScored = candidate;
        }
        if (score < worstScore) {
            worstScore = score;
            worstScored = candidate;
        }
    }

    if (paranoiaEvading && !fleeCandidates.empty()) {
        // Prefer the farthest-from-threat tile the hero can ACTUALLY path to.
        // Sort by distance desc, then take the first REACHABLE one (bounded
        // FindPath, so rejecting an unreachable pick across a barrier is cheap).
        // On a connected map the farthest tile is reachable → one FindPath and
        // done; on a split map this routes around the impassable center instead
        // of committing to a pathless spot and freezing.
        std::sort(fleeCandidates.begin(), fleeCandidates.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
        constexpr int kFleeReachChecks = 12;
        constexpr int kFleeReachBudget = 120000;
        const int checks = (std::min)((int)fleeCandidates.size(), kFleeReachChecks);
        for (int i = 0; i < checks; ++i) {
            const Position cand = fleeCandidates[(size_t)i].first;
            if (!map->FindPath(heroPos.x, heroPos.y, cand.x, cand.y, kFleeReachBudget).empty()) {
                out = cand;  // reachable — no jitter, so StartPathTo can definitely path it
                spdlog::trace("[hunt] Explore (paranoia) reachable -> ({},{}) threatDist={:.1f}",
                    out.x, out.y, fleeCandidates[(size_t)i].second);
                return true;
            }
        }
        // None of the farthest candidates were reachable within budget (a
        // disconnected pocket, or a very long path on a large map). Guarantee a
        // MOVE so the bot never freezes: step to the adjacent walkable tile that
        // best increases distance from the threat, and keep stepping each tick.
        Position stepBest{};
        float stepBestDist = -1.0f;
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                if (dx == 0 && dy == 0)
                    continue;
                const Position n{ heroPos.x + dx, heroPos.y + dy };
                if (n.x <= 0 || n.y <= 0 || !map->IsWalkable(n.x, n.y))
                    continue;
                if (!map->CanReach(heroPos.x, heroPos.y, n.x, n.y))
                    continue;
                const float d = distToNearestThreat(n);
                if (d > stepBestDist) {
                    stepBestDist = d;
                    stepBest = n;
                }
            }
        }
        if (stepBestDist >= 0.0f && !IsZeroPos(stepBest)) {
            out = stepBest;
            spdlog::trace("[hunt] Explore (paranoia) fallback step -> ({},{})", out.x, out.y);
            return true;
        }
        // Truly boxed in on every side — fall through (nothing valid to do).
    }

    if (exploring && worstScore < (std::numeric_limits<float>::max)() && !IsZeroPos(worstScored)) {
        out = JitterDestination(map, worstScored, GetJitterRadius(settings));
        m_recentExploreDests.emplace_back(out, now);
        spdlog::trace("[hunt] Explore (exploration roll) -> ({},{}) spawnScore={:.1f}/{:.1f}",
                      out.x, out.y, worstScore, maxScore);
        return true;
    }

    if (useHeatmap && bestScore >= 0.0f && !IsZeroPos(bestScored)) {
        out = JitterDestination(map, bestScored, GetJitterRadius(settings));
        m_recentExploreDests.emplace_back(out, now);
        spdlog::trace("[hunt] Explore -> ({},{}) spawnScore={:.1f}/{:.1f}",
                      out.x, out.y, bestScore, maxScore);
        return true;
    }
    return false;
}

// ── Paranoia evasion state machine (Session 14 — Phase 1 "gentle flee") ─────
// Detection is BINARY — any non-whitelisted player in the ~30-tile view counts,
// distance is irrelevant (IsAnyPlayerVisibleDebounced, no range). The reaction
// is deliberately GENTLE: hop ~paranoiaFleeDistance tiles away from the nearest
// threat (just out of their view), at human pace, then RESUME hunting where we
// landed — no sprint to the far side of the map, no compulsive return trip.
// The bot only gives up on a spot and RELOCATES (to a fresh heatmap-hot area)
// when the spot is genuinely compromised: the same place drew a player
// paranoiaAbandonAfter times in a decaying window, or a player kept the bot in
// sight for longer than paranoiaPassingThresholdMs during one flee (following/
// camping, not passing). Per-player positions/movement come from m_playerTracks
// (UpdatePlayerTracks). All transitions live here; movement is issued by
// DriveEvadeMovement.
int BaseHuntPlugin::GetEvadeRelocateMinDist(const AutoHuntSettings& settings) const
{
    // "A different area" = clearly beyond the view window from where the
    // players were, plus the re-nudge push each time one re-appeared.
    return kViewTiles + m_evadeRenudgeCount * (std::max)(1, settings.paranoiaRenudgeTiles);
}

void BaseHuntPlugin::UpdatePlayerTracks(const AutoHuntSettings& settings)
{
    CHero* hero = Game::GetHero();
    if (!hero)
        return;

    const DWORD now = GetTickCount();
    const OBJID heroId = hero->GetID();
    const std::vector<std::string> whitelist = ParseTokens(settings.playerWhitelist);

    for (CRole* role : Entities::Get()) {
        if (!role || !Entities::IsAlive(role) || !role->IsPlayer() || role->GetID() == heroId)
            continue;
        if (!whitelist.empty() && NameMatchesFilters(role->GetName(), whitelist))
            continue;

        const Position p = role->m_posMap;
        PlayerTrack& t = m_playerTracks[role->GetID()];
        if (t.lastSeenTick == 0) {
            t.posNow = p;
            t.posPrev = p;
            t.hasPrev = false;
        } else if (p.x != t.posNow.x || p.y != t.posNow.y) {
            // Only shift into posPrev on a DISTINCT position, so the movement
            // vector reflects real travel rather than a same-tile re-scan.
            t.posPrev = t.posNow;
            t.posNow = p;
            t.hasPrev = true;
        }
        t.lastSeenTick = now;
    }

    // Drop players who have been gone from the scan long enough to count as
    // out of view (matches the visibility debounce order of magnitude).
    constexpr DWORD kTrackExpiryMs = 3000;
    for (auto it = m_playerTracks.begin(); it != m_playerTracks.end(); ) {
        if (now - it->second.lastSeenTick > kTrackExpiryMs)
            it = m_playerTracks.erase(it);
        else
            ++it;
    }
}

std::vector<Position> BaseHuntPlugin::GetPlayerThreatPredictions() const
{
    // Flee away from BOTH where each player is now AND a short lookahead in
    // their direction of travel — the candidate destination is then scored on
    // its distance to the nearest of these, so it avoids a moving player's path
    // as well as their current spot.
    constexpr int kLookaheadTiles = 8;
    std::vector<Position> out;
    out.reserve(m_playerTracks.size() * 2);
    for (const auto& [id, t] : m_playerTracks) {
        out.push_back(t.posNow);
        if (t.hasPrev) {
            const float dx = (float)(t.posNow.x - t.posPrev.x);
            const float dy = (float)(t.posNow.y - t.posPrev.y);
            const float len = sqrtf(dx * dx + dy * dy);
            if (len > 0.5f) {
                out.push_back(Position{
                    t.posNow.x + (int)std::lround(dx / len * kLookaheadTiles),
                    t.posNow.y + (int)std::lround(dy / len * kLookaheadTiles) });
            }
        }
    }
    return out;
}

bool BaseHuntPlugin::UpdateEvadeState(CHero* hero, const AutoHuntSettings& settings)
{
    if (!settings.paranoiaEnabled || !hero) {
        m_evadeState = EvadeState::None;
        m_evadeRenudgeCount = 0;
        m_evadeEncounterCount = 0;
        m_evadeFleeTarget = {};
        m_playerTracks.clear();
        return false;
    }

    constexpr int kEvadeArriveTiles = 5;
    constexpr DWORD kEncounterDecayMs = 120000;  // undisturbed this long -> forget past encounters
    const int abandonAfter = (std::max)(1, settings.paranoiaAbandonAfter);
    const DWORD now = GetTickCount();

    // Refresh per-player positions/movement every tick; the binary "anyone in
    // view" test then drives every transition.
    UpdatePlayerTracks(settings);
    const bool anyVisible = IsAnyPlayerVisibleDebounced(settings);

    auto snapshotThreats = [&]() {
        m_evadeThreatPositions.clear();
        for (const auto& [id, t] : m_playerTracks)
            m_evadeThreatPositions.push_back(t.posNow);
    };

    switch (m_evadeState) {
    case EvadeState::None:
        if (anyVisible) {
            // Decay stale encounters so only a RUN of visits to this spot
            // (within kEncounterDecayMs of one another) counts toward giving up.
            if (m_evadeLastEncounterTick != 0 && now - m_evadeLastEncounterTick > kEncounterDecayMs)
                m_evadeEncounterCount = 0;
            ++m_evadeEncounterCount;
            m_evadeLastEncounterTick = now;
            m_evadeFleeStartTick = now;
            m_evadeFleeTarget = {};
            m_evadeRelocateDest = {};
            m_evadeState = EvadeState::Fleeing;
            spdlog::info("[paranoia] Player in view -> FLEEING (encounter {}/{}, tracked={})",
                m_evadeEncounterCount, abandonAfter, (int)m_playerTracks.size());
        }
        break;

    case EvadeState::Fleeing: {
        const bool lingering = anyVisible
            && (int)(now - m_evadeFleeStartTick) >= settings.paranoiaPassingThresholdMs;
        const bool spotContested = m_evadeEncounterCount >= abandonAfter;
        if (spotContested || lingering) {
            // Give up on this spot — relocate to a fresh heatmap-hot area away
            // from wherever the players are right now.
            snapshotThreats();
            m_evadeRenudgeCount = 0;
            m_evadeRelocateDest = {};
            m_evadeFleeTarget = {};
            m_evadeState = EvadeState::Relocating;
            spdlog::info("[paranoia] {} -> RELOCATING (threats={})",
                spotContested ? "spot contested" : "player lingering",
                (int)m_evadeThreatPositions.size());
        } else if (!anyVisible) {
            // Gentle flee worked — out of their view. Resume hunting right here,
            // no sprint and no return trip.
            m_evadeFleeTarget = {};
            m_evadeState = EvadeState::None;
            spdlog::info("[paranoia] Out of view -> resume hunting here");
        }
        break;
    }

    case EvadeState::Relocating:
        if (!IsZeroPos(m_evadeRelocateDest)
            && CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
                                  m_evadeRelocateDest.x, m_evadeRelocateDest.y) <= kEvadeArriveTiles) {
            if (anyVisible && m_evadeRenudgeCount >= kMaxEvadeRenudgeCount) {
                // Escalated this many times without ever losing the "threat"
                // — either a genuinely, unusually persistent player (rare) or
                // something that was never a real player to begin with. Either
                // way, retrying farther has demonstrably not worked; stop
                // digging and resume hunting from right here rather than
                // chasing an ever-growing relocate distance forever.
                spdlog::warn("[paranoia] Still \"visible\" after {} renudges — giving up on evasion and resuming hunting here "
                             "(a persistent phantom detection would look like this; check the Entities overlay if this recurs)",
                             m_evadeRenudgeCount);
                m_evadeState = EvadeState::None;
                m_evadeRenudgeCount = 0;
                m_evadeEncounterCount = 0;
                m_evadeLastEncounterTick = 0;
            } else if (anyVisible) {
                // Players here too — push the relocate farther and keep going
                // (speed stays human via disableSpeedhackOnPlayer while seen).
                ++m_evadeRenudgeCount;
                snapshotThreats();
                m_evadeRelocateDest = {};
                spdlog::info("[paranoia] Player at relocate area -> pushing farther (renudge={})", m_evadeRenudgeCount);
            } else {
                spdlog::info("[paranoia] Relocated to a fresh area -> resume hunting");
                m_evadeState = EvadeState::None;
                m_evadeRenudgeCount = 0;
                m_evadeEncounterCount = 0;    // new area, fresh start
                m_evadeLastEncounterTick = 0;
            }
        }
        break;
    }

    return m_evadeState != EvadeState::None;
}

bool BaseHuntPlugin::FindParanoiaRelocateDest(CHero* hero, CGameMap* map,
    const AutoHuntSettings& settings, const std::vector<Position>& threatPositions,
    int minDistFromThreat, Position& out) const
{
    out = {};
    if (!hero || !map)
        return false;

    const OBJID mapId = Game::GetCurrentMapId();
    if (!SpawnMemory::HasUsefulData(mapId))
        return false;

    // Distance from a bucket to the NEAREST threat point — a qualifying bucket
    // must be at least minDistFromThreat from EVERY player's last-known spot.
    auto minThreatDist = [&](const Position& b) -> int {
        if (threatPositions.empty())
            return (std::numeric_limits<int>::max)();
        int m = (std::numeric_limits<int>::max)();
        for (const Position& tp : threatPositions)
            m = (std::min)(m, CGameMap::TileDist(tp.x, tp.y, b.x, b.y));
        return m;
    };

    // Hottest known bucket that is in-zone, walkable, and clearly away from
    // where all the players were — a spot the bot can actually hunt, not a dead
    // corner (fixes G3). Ask for a generous pool so distance filtering still
    // leaves candidates on smaller maps.
    const std::vector<Position> hot = SpawnMemory::GetHotBuckets(mapId, 32);
    Position best{};
    float    bestScore = -1.0f;
    for (const Position& b : hot) {
        if (b.x <= 0 || b.y <= 0)
            continue;
        // Session 16 [PHASE 3]: MapWide's "zone" IS the dynamic zone this
        // relocate is trying to move — checking membership in the OLD one
        // here rejects every candidate outside the tiny spot being fled,
        // defeating "relocate with the full map" (the whole point of
        // MapWide). Circle/Polygon/Route are deliberately user-drawn
        // boundaries, so those still apply; only MapWide is exempt.
        if (settings.zoneMode != AutoHuntZoneMode::MapWide
            && !IsPointInZone(settings, settings.zoneMapId, b))
            continue;
        if (!map->IsWalkable(b.x, b.y))
            continue;
        if (minThreatDist(b) < minDistFromThreat)
            continue;
        if (settings.paranoiaEnabled && HuntContest::IsBucketContested(mapId, b, settings))
            continue;
        const float s = SpawnMemory::GetScore(mapId, b);
        if (s > bestScore) {
            bestScore = s;
            best = b;
        }
    }

    if (bestScore >= 0.0f && !IsZeroPos(best)) {
        out = JitterDestination(map, best, GetJitterRadius(settings));
        return true;
    }
    return false;
}

bool BaseHuntPlugin::FindGentleFleeTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    const std::vector<Position>& threatPositions, int fleeDistance, Position& out) const
{
    out = {};
    if (!hero || !map || threatPositions.empty() || fleeDistance <= 0)
        return false;

    const Position heroPos = hero->m_posMap;

    // Nearest threat point — flee directly away from it.
    Position nearest = threatPositions[0];
    float nearestDist = heroPos.DistanceTo(nearest);
    for (const Position& tp : threatPositions) {
        const float d = heroPos.DistanceTo(tp);
        if (d < nearestDist) { nearestDist = d; nearest = tp; }
    }

    float ax = (float)(heroPos.x - nearest.x);
    float ay = (float)(heroPos.y - nearest.y);
    float len = sqrtf(ax * ax + ay * ay);
    if (len < 0.5f) { ax = 1.0f; ay = 0.0f; len = 1.0f; }  // threat on top of us — pick a direction
    ax /= len; ay /= len;

    // Try the ideal away-point first, then progressively back off and fan out
    // to the sides, taking the first walkable + reachable tile. Backing off /
    // fanning lets it route around an impassable center instead of committing
    // to a blocked straight line. NOT zone-clamped — escaping view beats the
    // zone leash (which is already widened while evading).
    //
    // Session 18 [CORNER FLEE FIX]: the angle fan used to stop at ±1.35 rad
    // (~±77° off the direct away-heading) — a ~154° cone, not the full
    // circle. Live 2026-09-04: hunting was already anchored near a map
    // corner when a threat appeared from the open side, so "directly away"
    // pointed straight into the corner/wall and every angle in that ~154°
    // cone was some flavor of "into the same dead end" — none of them ever
    // pointed back out toward the open ground the threat had come from. The
    // walkable+reachable check below did its job (it never committed to an
    // actually-unwalkable tile), it just never got offered a genuinely
    // different heading to check. Extended to the full circle, in priority
    // order nearest the ideal away-direction first (so open ground roughly
    // away from the threat is still always preferred when it exists) —
    // ~180° (nearly back toward the threat) is a legitimate last resort
    // when it's the only open direction, e.g. slipping past to open ground
    // beyond, rather than pressing into a wall that clearly isn't opening up.
    const float dists[] = { 1.0f, 0.8f, 0.6f, 0.4f, 0.25f };
    const float angs[]  = { 0.0f, 0.45f, -0.45f, 0.9f, -0.9f, 1.35f, -1.35f,
                             1.8f, -1.8f, 2.25f, -2.25f, 2.7f, -2.7f, 3.14159f };
    constexpr int kReachBudget = 60000;
    for (float df : dists) {
        for (float da : angs) {
            const float ca = cosf(da), sa = sinf(da);
            const float rx = ax * ca - ay * sa;
            const float ry = ax * sa + ay * ca;
            const Position cand{
                heroPos.x + (int)std::lround(rx * (float)fleeDistance * df),
                heroPos.y + (int)std::lround(ry * (float)fleeDistance * df) };
            if (cand.x <= 0 || cand.y <= 0)
                continue;
            if (CGameMap::TileDist(heroPos.x, heroPos.y, cand.x, cand.y) < 2)
                continue;
            if (!map->IsWalkable(cand.x, cand.y))
                continue;
            if (map->FindPath(heroPos.x, heroPos.y, cand.x, cand.y, kReachBudget).empty())
                continue;
            out = cand;
            return true;
        }
    }
    return false;
}

bool BaseHuntPlugin::DriveEvadeMovement(CHero* hero, CGameMap* map, const AutoHuntSettings& settings)
{
    if (!hero || !map)
        return false;

    switch (m_evadeState) {
    case EvadeState::Fleeing: {
        // A short bounded hop away from the nearest player's current/predicted
        // position — recomputed only when we have no target or have basically
        // reached the last one (player following: keep edging away).
        const bool reached = !IsZeroPos(m_evadeFleeTarget)
            && CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
                                  m_evadeFleeTarget.x, m_evadeFleeTarget.y) <= 4;
        if (IsZeroPos(m_evadeFleeTarget) || reached) {
            const std::vector<Position> preds = GetPlayerThreatPredictions();
            if (!FindGentleFleeTarget(hero, map, settings, preds, settings.paranoiaFleeDistance, m_evadeFleeTarget))
                m_evadeFleeTarget = {};
        }
        if (!IsZeroPos(m_evadeFleeTarget) && StartPathTo(hero, map, m_evadeFleeTarget, 0)) {
            m_targetId = 0;
            SetState(AutoHuntState::AcquireTarget, "Evading: stepping out of view");
            return true;
        }
        break;
    }
    case EvadeState::Relocating: {
        if (IsZeroPos(m_evadeRelocateDest)) {
            const int minDist = GetEvadeRelocateMinDist(settings);
            if (!FindParanoiaRelocateDest(hero, map, settings, m_evadeThreatPositions, minDist, m_evadeRelocateDest)) {
                // No hot bucket qualified (no heatmap yet, tiny zone, etc.) —
                // fall back to the farthest-from-all-threats reachable tile so
                // the bot still clears the area instead of stalling.
                FindZoneExplorePosition(hero, map, settings, m_evadeRelocateDest, &m_evadeThreatPositions);
            }
            // Phase 3: move the dynamic zone itself to the chosen destination
            // the instant it's picked, not just on arrival — otherwise the OLD
            // zone is still what leash/targeting are bounded to for the whole
            // walk over there, fighting the very relocate this is supposed to
            // be. Re-seeding here also means the escalating re-nudge case
            // (below, a player found AGAIN at the new spot) naturally moves
            // the zone again each time, exactly like a fresh destination does.
            if (!IsZeroPos(m_evadeRelocateDest))
                ReseedDynamicZoneAt(m_evadeRelocateDest, settings);
        }
        if (!IsZeroPos(m_evadeRelocateDest) && StartPathTo(hero, map, m_evadeRelocateDest, 0)) {
            m_targetId = 0;
            SetState(AutoHuntState::AcquireTarget, "Evading: relocating to a new area");
            return true;
        }
        break;
    }
    default:
        break;
    }
    return false;
}

// ── Phase 2b: Dynamic Zone Engine (the core logic for Map-Wide) ─────────────
namespace {
constexpr int   kDynGrowTicks       = 3;      // consecutive live ticks before a frontier cell is absorbed
constexpr int   kDynShrinkTicks     = 6;      // consecutive empty ticks before a member cell is dropped
constexpr DWORD kDynSizeReviewMs    = 1500;
constexpr DWORD kDynIdleRecenterMs  = 12000;  // no new kill this long -> full relocate
constexpr DWORD kDynRecenterGapMs   = 6000;   // min gap between full relocates
constexpr int   kDynRecenterMinMove = 40;     // new relocate target must be at least this far
constexpr int   kDynReachBudget     = 80000;  // bounded FindPath iteration cap (grow + relocate checks)

// Session 15 [KILL-SIGNAL RE]: SEH-guarded raw reads for the stat-table byte
// dump (the native CStatTable::GetValue returns 0 on v1074 — likely a stale
// pointer offset or RVA — so we dump raw bytes to find the real HP / kill-count
// offsets directly). Keep these free of C++ unwind objects (MSVC __try rule).
bool SafeReadBlock(uintptr_t addr, void* out, size_t n)
{
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Dump `count` consecutive int32s at addr as "label +0xOFF (idx N) = value" so
// the user can eyeball their HP / kill count and read off the offset/index.
void DumpInts(const char* label, uintptr_t addr, int count)
{
    for (int i = 0; i < count; ++i) {
        int32_t v = 0;
        if (SafeReadBlock(addr + (size_t)i * 4, &v, 4))
            spdlog::info("[statbytes] {} +0x{:X} (idx {}) = {}", label, i * 4, i, v);
        else
            spdlog::info("[statbytes] {} +0x{:X} (idx {}) = <unreadable>", label, i * 4, i);
    }
}
}

bool BaseHuntPlugin::IsDynamicZoneActive(const AutoHuntSettings& settings) const
{
    return settings.zoneMode == AutoHuntZoneMode::MapWide && m_dynInit
        && m_dynMapId == Game::GetCurrentMapId() && !m_dynCells.empty();
}

bool BaseHuntPlugin::InDynamicZone(const Position& p) const
{
    return m_dynCells.count(DynCellKeyFor(p, m_dynCellTiles)) > 0;
}

int BaseHuntPlugin::CountMonstersInDynamicZone(const AutoHuntSettings& settings) const
{
    int n = 0;
    const std::vector<CRole*> mobs = CollectHuntTargets(settings, false, /*ignoreSearchRange=*/true);
    for (CRole* m : mobs)
        if (m && InDynamicZone(m->m_posMap))
            ++n;
    return n;
}

std::vector<Position> BaseHuntPlugin::GetDynZoneCellCenters() const
{
    std::vector<Position> out;
    out.reserve(m_dynCells.size());
    for (uint32_t key : m_dynCells)
        out.push_back(DynCellCenterOf(key, m_dynCellTiles));
    return out;
}

bool BaseHuntPlugin::FindDynamicRecenterTarget(const AutoHuntSettings& settings, OBJID mapId,
    const Position& from, Position& out) const
{
    out = {};
    if (!SpawnMemory::HasUsefulData(mapId))
        return false;
    CGameMap* map = Game::GetMap();
    CHero* hero = Game::GetHero();
    if (!map || !hero)
        return false;
    const Position heroPos = hero->m_posMap;

    // Collect valid hot-bucket candidates (a genuinely different area, walkable,
    // not player-camped), then take the hottest one the hero can ACTUALLY reach.
    std::vector<std::pair<float, Position>> cands;
    for (const Position& b : SpawnMemory::GetHotBuckets(mapId, 24)) {
        if (b.x <= 0 || b.y <= 0)
            continue;
        if (CGameMap::TileDist(from.x, from.y, b.x, b.y) < kDynRecenterMinMove)
            continue;
        if (!map->IsWalkable(b.x, b.y))
            continue;
        if (HuntContest::IsBucketContested(mapId, b, settings))
            continue;
        cands.emplace_back(SpawnMemory::GetScore(mapId, b), b);
    }
    std::sort(cands.begin(), cands.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });

    // Phase 2a: reachability gate — a bounded FindPath so an unreachable target
    // (e.g. a hot spot on another Bird Island island behind a one-way portal)
    // is cheaply rejected instead of stranding the zone there. Take the
    // hottest REACHABLE candidate; if none are reachable, don't relocate (the
    // caller keeps hunting where it is).
    int checks = 0;
    for (const auto& [score, b] : cands) {
        if (++checks > 12)
            break;
        if (map->FindPath(heroPos.x, heroPos.y, b.x, b.y, kDynReachBudget).empty())
            continue;
        out = b;
        return true;
    }
    return false;
}

void BaseHuntPlugin::UpdateDynamicZone(CHero* hero, const AutoHuntSettings& settings)
{
    if (settings.zoneMode != AutoHuntZoneMode::MapWide || !hero) {
        m_dynInit = false;
        return;
    }
    const OBJID mapId = Game::GetCurrentMapId();
    const DWORD now = GetTickCount();
    const int cellTiles = (std::max)(1, settings.dynZoneCellTiles);
    const int maxCells = (std::max)(1, settings.dynZoneMaxCells);

    // (Re)seed on first use, map change, or a cell-size slider change: a
    // single cell at the hero's CURRENT position. Phase 2a: do NOT seed at
    // the global hottest heatmap bucket — on a map split into unreachable
    // chunks (Bird Island's one-way-portal islands) that bucket can be
    // somewhere the bot can't path to, stranding the zone far from where
    // it's actually hunting. Anchor to where it IS; growth (below) absorbs
    // adjacent cells that show live mobs, and a full relocate only fires
    // when the whole zone dies. A cell-size change reseeds rather than
    // re-keying live cells in place — mixing two different grids in
    // m_dynCells would corrupt neighbor/membership math.
    if (!m_dynInit || m_dynMapId != mapId || m_dynCellTiles != cellTiles) {
        // A marked MapWide start point (settings.mapWideStartPos) only
        // applies to THIS one-time seed, and only when hunting is actually
        // beginning on the map it was marked for — never referenced again
        // afterward by growth/shrink/relocate below, so the bot is never
        // tied back to it once hunting is under way.
        const Position seedPos = (!IsZeroPos(settings.mapWideStartPos) && settings.zoneMapId == mapId)
            ? settings.mapWideStartPos
            : hero->m_posMap;
        m_dynCells.clear();
        m_dynCellHotStreak.clear();
        m_dynCellColdStreak.clear();
        m_dynCellTiles = cellTiles;
        m_dynCells.insert(DynCellKeyFor(seedPos, cellTiles));
        m_dynMapId = mapId;
        m_dynInit = true;
        m_dynLastSizeTick = now;
        m_dynLastRecenterTick = now;
        m_dynProductiveTick = now;
        m_dynLastKills = hero->GetGameKillCount();
        m_dynHasArrived = false;
        spdlog::info("[dynzone] Seeded 1 cell ({} tiles) at ({},{}) on map {}",
            cellTiles, seedPos.x, seedPos.y, mapId);
        return;
    }

    // Efficiency = KILL RATE (the user's point): a spot is "productive" only
    // while kills keep landing — a dense clump of tough mobs that aren't dying
    // is NOT worth staying for. Uses the game's own accurate kill counter
    // (CHero+0xA30), not the flaky entity-disappear heuristic. This drives
    // the full-relocate fallback below; shape (grow/shrink) is driven purely
    // by where LIVE monsters currently are, not by this productivity signal.
    if (now - m_dynLastSizeTick >= kDynSizeReviewMs) {
        m_dynLastSizeTick = now;
        const int kills = hero->GetGameKillCount();
        if (kills > m_dynLastKills) {
            m_dynLastKills = kills;
            m_dynProductiveTick = now;   // still killing here -> productive
        }

        // Phase 2b: bucket the CURRENTLY VISIBLE monsters into cells (live
        // density, not heatmap history) so the shape follows what's actually
        // there right now instead of only jumping on a timer. ignoreZone=true
        // — now that IsPointNearHuntZone actually bounds MapWide to the
        // CURRENT cells (see hunt_settings.cpp), the normal zone-gated scan
        // would filter out every candidate frontier cell before growth ever
        // got to see it, and the zone could never expand.
        std::unordered_set<uint32_t> liveCells;
        for (CRole* m : CollectHuntTargets(settings, false, /*ignoreSearchRange=*/true,
                                            /*rangeOrigin=*/nullptr, /*ignoreZone=*/true))
            if (m)
                liveCells.insert(DynCellKeyFor(m->m_posMap, cellTiles));

        // Shrink: a member cell CHECKED and found empty kDynShrinkTicks times
        // in a row drops out of the zone. "Checked" means the hero was
        // actually within kViewTiles of it — real tiles, NOT scaled by the
        // zone's own cell size. Entities::Get() only contains what the
        // game's server visibility has actually sent the client, so a cell
        // the hero hasn't been near isn't "confirmed empty," it's just
        // unobserved. Session 16 [SHRINK ON UNOBSERVED CELLS]: this used to
        // decay EVERY member cell every tick regardless of hero position, so
        // a multi-cell zone lost cells it wasn't actively standing in — a
        // live fan-out to 5 cells shed one 9s after the hero simply moved to
        // fight elsewhere, not because that cell's spawns were gone. The
        // first fix used cell-grid adjacency ("own cell or a neighbor"),
        // which quietly broke again at a large cellTiles (a 37-tile cell's
        // "neighbor" spans 100+ actual tiles, well past real visibility) —
        // a fixed tile-distance to the cell's center is correct at any cell
        // size. A cell outside the check radius has its streak left
        // untouched (not reset either — "no data" isn't "still alive") until
        // the hero comes back near it for a real look. Never empty the zone
        // entirely — the last cell always stays so there's somewhere to
        // sample/steer toward.
        auto isCellChecked = [&](uint32_t key) {
            const Position c = DynCellCenterOf(key, cellTiles);
            return CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, c.x, c.y) <= kViewTiles;
        };

        // See m_dynHasArrived's comment: don't let the idle-relocate check
        // below fire on travel time alone. Sticky (never cleared here) —
        // only a fresh seed/relocate resets it.
        if (!m_dynHasArrived) {
            for (uint32_t key : m_dynCells) {
                if (isCellChecked(key)) {
                    m_dynHasArrived = true;
                    m_dynProductiveTick = now;   // real hunting starts now, not back at relocate time
                    break;
                }
            }
        }

        for (auto it = m_dynCells.begin(); it != m_dynCells.end(); ) {
            const uint32_t key = *it;
            if (liveCells.count(key)) {
                m_dynCellColdStreak.erase(key);
                ++it;
                continue;
            }
            if (!isCellChecked(key)) {
                ++it;   // hero wasn't close enough to know either way — leave its streak alone
                continue;
            }
            const int cold = ++m_dynCellColdStreak[key];
            if (cold >= kDynShrinkTicks && m_dynCells.size() > 1) {
                m_dynCellColdStreak.erase(key);
                it = m_dynCells.erase(it);
            } else {
                ++it;
            }
        }

        // Grow: any live cell within a "still worth the walk" GAP of the
        // NEAREST existing member gets absorbed after kDynGrowTicks
        // consecutive sightings — so one wandering stray doesn't yank the
        // shape around — provided it's walkable, uncontested and the hero
        // can actually reach it (same bounded-FindPath gate as a full
        // relocate). Session 16 [NON-CONTIGUOUS GROWTH]: this used to require
        // DIRECT 4-neighbor adjacency to an existing cell — but real spawn
        // clusters leave dead ground between them at any cell size small
        // enough to be useful, so a clump 2-4 cells away with nothing
        // touching it never grew in even though it was plainly visible and
        // worth the short walk. The gap is a multiple of the cell size
        // itself (kDynGrowGapCells), not a fixed tile count, so it scales
        // the same way the zone's own cells do. Candidates still have to
        // pass the same walkable/uncontested/reachable gates as before, so
        // this doesn't reopen the old "grow toward somewhere unreachable"
        // problem — it just stops requiring the candidate to be glued to
        // the current shape. m_dynCellHotStreak is rebuilt from this tick's
        // candidates every pass, so one that falls out of range (a member
        // near it was shrunk away) or stops showing mobs is dropped
        // automatically.
        constexpr int kDynGrowGapCells = 4;   // max gap, in CELLS, from the nearest member
        if ((int)m_dynCells.size() < maxCells) {
            CGameMap* map = Game::GetMap();
            const Position heroPos = hero->m_posMap;
            const int gapTiles = kDynGrowGapCells * cellTiles;
            std::unordered_set<uint32_t> frontier;
            for (uint32_t liveKey : liveCells) {
                if (m_dynCells.count(liveKey))
                    continue;   // already a member
                const Position lc = DynCellCenterOf(liveKey, cellTiles);
                int nearestDist = (std::numeric_limits<int>::max)();
                for (uint32_t memberKey : m_dynCells) {
                    const Position mc = DynCellCenterOf(memberKey, cellTiles);
                    nearestDist = (std::min)(nearestDist, CGameMap::TileDist(lc.x, lc.y, mc.x, mc.y));
                }
                if (nearestDist <= gapTiles)
                    frontier.insert(liveKey);
            }
            int frontierLive = 0;
            std::unordered_map<uint32_t, int> freshHot;
            for (uint32_t key : frontier) {
                ++frontierLive;   // everything in `frontier` is already live this tick (built from liveCells)
                auto pit = m_dynCellHotStreak.find(key);
                const int hot = (pit != m_dynCellHotStreak.end() ? pit->second : 0) + 1;
                if (hot >= kDynGrowTicks && (int)m_dynCells.size() < maxCells && map) {
                    const Position c = DynCellCenterOf(key, cellTiles);
                    const bool walkable = map->IsWalkable(c.x, c.y);
                    const bool contested = walkable && HuntContest::IsBucketContested(mapId, c, settings);
                    const bool reachable = walkable && !contested
                        && !map->FindPath(heroPos.x, heroPos.y, c.x, c.y, kDynReachBudget).empty();
                    if (walkable && !contested && reachable) {
                        m_dynCells.insert(key);
                        spdlog::info("[dynzone] Grew: cell ({},{}) added, now {} cells",
                            c.x, c.y, (int)m_dynCells.size());
                        continue;   // promoted to member — no frontier streak to keep
                    }
                    // Session 16 [GROWTH DIAGNOSIS]: hot enough to grow but blocked —
                    // log exactly which gate failed instead of silently resetting,
                    // so a live "zone never grows despite a dense swarm right next
                    // to it" report can be pinned to a specific cause.
                    spdlog::info("[dynzone] Grow blocked: cell ({},{}) walkable={} contested={} reachable={}",
                        c.x, c.y, walkable, contested, reachable);
                }
                freshHot[key] = hot;
            }
            m_dynCellHotStreak.swap(freshHot);
            spdlog::info("[dynzone] size-review: {} cells, {} frontier ({} live), {} total live mobs seen",
                (int)m_dynCells.size(), (int)frontier.size(), frontierLive, (int)liveCells.size());
        }
    }

    // Full relocate when kills have stalled (low kill rate) for a sustained
    // window — the whole zone's played out or too slow, abandon it and reseed
    // a single cell at the next reachable hot bucket. Gated on m_dynHasArrived
    // (see its comment) — otherwise a relocate target farther than the hero
    // can travel within kDynIdleRecenterMs just relocates again before
    // arrival, forever.
    if (m_dynHasArrived
        && now - m_dynProductiveTick >= kDynIdleRecenterMs
        && now - m_dynLastRecenterTick >= kDynRecenterGapMs) {
        m_dynLastRecenterTick = now;
        Position centroid{};
        for (uint32_t key : m_dynCells) {
            const Position c = DynCellCenterOf(key, cellTiles);
            centroid.x += c.x;
            centroid.y += c.y;
        }
        centroid.x /= (int)m_dynCells.size();
        centroid.y /= (int)m_dynCells.size();

        Position newCenter{};
        if (FindDynamicRecenterTarget(settings, mapId, centroid, newCenter)) {
            spdlog::info("[dynzone] Relocate: {} cells around ({},{}) -> ({},{}) after {}ms unproductive",
                (int)m_dynCells.size(), centroid.x, centroid.y, newCenter.x, newCenter.y, now - m_dynProductiveTick);
            m_dynCells.clear();
            m_dynCellHotStreak.clear();
            m_dynCellColdStreak.clear();
            m_dynCells.insert(DynCellKeyFor(newCenter, cellTiles));
            m_dynProductiveTick = now;   // fresh window at the new area
            m_dynHasArrived = false;     // don't count travel time toward the new area's window either
        }
    }
}

void BaseHuntPlugin::ReseedDynamicZoneAt(const Position& pos, const AutoHuntSettings& settings)
{
    if (settings.zoneMode != AutoHuntZoneMode::MapWide || IsZeroPos(pos))
        return;
    const int cellTiles = (std::max)(1, settings.dynZoneCellTiles);
    m_dynCells.clear();
    m_dynCellHotStreak.clear();
    m_dynCellColdStreak.clear();
    m_dynCellTiles = cellTiles;
    m_dynCells.insert(DynCellKeyFor(pos, cellTiles));
    m_dynMapId = Game::GetCurrentMapId();
    m_dynInit = true;
    const DWORD now = GetTickCount();
    m_dynLastSizeTick = now;
    m_dynLastRecenterTick = now;
    m_dynProductiveTick = now;
    m_dynHasArrived = false;   // see its comment on the seed branch in UpdateDynamicZone
    if (CHero* hero = Game::GetHero())
        m_dynLastKills = hero->GetGameKillCount();
    spdlog::info("[dynzone] Paranoia moved the zone: 1 cell ({} tiles) at ({},{})",
        cellTiles, pos.x, pos.y);
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

    if (now < m_pathFailBackoffUntilTick) {
        spdlog::trace("[hunt] Move blocked: path-fail backoff for {}ms more, dest=({},{})",
            m_pathFailBackoffUntilTick - now, destination.x, destination.y);
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
    // Walk-only locations (Market now; cities later — ShouldWalkOnlyOnMap):
    // never issue a direct jump. Route straight to the A* path below, which the
    // Pathfinder now walks leg-by-leg. (Skips both the short-hop StartWalkTo —
    // it would fight the walk-only pathfinder by Stop()ing it — and the
    // direct-jump branch entirely.)
    const bool walkOnly = ShouldWalkOnlyOnMap(Game::GetCurrentMapId());

    // Prefer a real animated Walk over an instant Jump for short hops when
    // not running aggressive speeds (no nearby player to hide from, or the
    // speedhack toggle is off) — falls through to the jump/pathfind logic
    // below if no reachable candidate tile was found (e.g. destination boxed
    // in on all sides).
    if (!walkOnly && dist <= kWalkInsteadOfJumpTiles && !ShouldUseAggressiveSpeeds(settings)) {
        if (StartWalkTo(hero, map, destination, stopRange))
            return true;
    }

    // Session 13 [JUMP VARIANCE]: shrink an over-long direct jump toward a
    // walkable intermediate point when a player is nearby (see
    // ApplyJumpDistanceCap) — the hero simply continues toward the true
    // destination on the next decision tick, so this only changes HOW MANY
    // jumps a long move takes, never whether it completes. Falls back to the
    // untouched `destination` when uncapped, already close enough, or no
    // valid intermediate point exists.
    Position jumpDest = destination;
    ApplyJumpDistanceCap(map, {hx, hy}, jumpDest, settings, CGameMap::GetHeroAltThreshold());

    const bool canDirectJump = !walkOnly
        && dist <= CGameMap::MAX_JUMP_DIST
        && map->CanJump(hx, hy, jumpDest.x, jumpDest.y, CGameMap::GetHeroAltThreshold())
        && !IsTileOccupied(jumpDest.x, jumpDest.y);
    const DWORD movementIntervalMs = canDirectJump
        ? GetJumpMovementIntervalMs(settings, hero)
        : GetMovementIntervalMs(settings);
    if (now - m_lastMoveTick < movementIntervalMs)
        return true;

    if (canDirectJump) {
        spdlog::debug("[hunt] JUMP ({},{}) -> ({},{}) dist={}", hx, hy, jumpDest.x, jumpDest.y,
            CGameMap::TileDist(hx, hy, jumpDest.x, jumpDest.y));
        hero->Jump(jumpDest.x, jumpDest.y);
        m_lastMoveTick = now;
        ArmPendingJump(hero, jumpDest, now, false);
        return true;
    }

    spdlog::debug("[hunt] Direct jump failed dist={} canJump={} occupied={}, trying A* ({},{}) -> ({},{})",
        dist, map->CanJump(hx, hy, destination.x, destination.y, CGameMap::GetHeroAltThreshold()),
        IsTileOccupied(destination.x, destination.y),
        hx, hy, destination.x, destination.y);

    auto tilePath = map->FindPath(hx, hy, destination.x, destination.y, 1000000);
    if (tilePath.empty()) {
        // Was silent. Live 2026-09-03 the store flow retried this every 430 ms
        // for minutes ("trying A*" with no result line) because the chosen
        // destination was walkable but sealed off — say so, throttled.
        static DWORD s_lastNoPathWarn = 0;
        const DWORD nowTick = GetTickCount();
        if (nowTick - s_lastNoPathWarn > 5000) {
            s_lastNoPathWarn = nowTick;
            spdlog::warn("[hunt] A* found NO ROUTE ({},{}) -> ({},{}) — destination walkable but unreachable?",
                         hx, hy, destination.x, destination.y);
        }
        return false;
    }

    // Session 18 [WALK-ONLY STUTTER FIX]: SimplifyPath is jump-capped
    // (MAX_JUMP_DIST=18) and the walk-only case never jumps — see
    // SimplifyPathWalkOnly's header comment. Keeps walk-only legs long.
    auto waypoints = walkOnly ? map->SimplifyPathWalkOnly(tilePath) : map->SimplifyPath(tilePath);
    if (waypoints.empty()) {
        // Was completely silent — the other empty-result branches above both
        // log, this one didn't. Live 2026-09-03: this is exactly the shape a
        // WALK-ONLY destination takes when FindPath's own graph only connects
        // to it via a jump-length edge (confirmed offline for the Market:
        // MillionaireLee's platform is NOT walk-reachable from the landing
        // tile — nearest walk-connected tile is 4 away — yet FindPath found
        // SOME tile-path, which SimplifyPath then can't turn into any
        // walk-legal jump segment). Without this the caller retried at
        // ~430ms forever with the hero's position never changing and zero
        // diagnostic output.
        static DWORD s_lastEmptyWaypointsWarn = 0;
        const DWORD nowTick = GetTickCount();
        if (nowTick - s_lastEmptyWaypointsWarn > 5000) {
            s_lastEmptyWaypointsWarn = nowTick;
            spdlog::warn("[hunt] A* found a route but SimplifyPath produced no walkable waypoints "
                         "({},{}) -> ({},{}) — likely a jump-only gap on a walk-only map", hx, hy,
                         destination.x, destination.y);
        }
        return false;
    }

    spdlog::debug("[hunt] A* route: {} waypoint(s), first=({},{}) last=({},{})",
        waypoints.size(), waypoints.front().x, waypoints.front().y,
        waypoints.back().x, waypoints.back().y);

    Pathfinder::Get().StartPath(waypoints, [] { return GetMovementIntervalMs(GetAutoHuntSettings()); },
        [] { return GetJumpDistanceCapTiles(GetAutoHuntSettings()); });
    m_lastMoveTick = now;
    if (!Pathfinder::Get().IsActive()) {
        // StartPath() couldn't issue movement toward even the first waypoint
        // (see Pathfinder::IssueMovementToWaypoint — walk-only requires
        // CanReach + an unoccupied landing tile) and gave up immediately.
        // Retrying at the normal movementIntervalMs cadence (as low as
        // ~100ms under aggressive speed) just repeats the same failing
        // FindPath/StartPath call hundreds of times a second next to a
        // crowded NPC without ever making progress — force a real pause.
        m_pathFailBackoffUntilTick = now + kPathFailBackoffMs;
        spdlog::warn("[hunt] StartPath failed to activate (blocked/unreachable landing tile), backing off {}ms, dest=({},{})",
            kPathFailBackoffMs, destination.x, destination.y);
    }
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

    // Short random hop, 1-2 tiles in any direction. StartWalkTo() handles
    // walkability/occupancy/reachability and skips while the hero is
    // jumping — but NOTE it STOPS an active pathfinder route to take the hop
    // (it does not wait for it). The caller is responsible for not invoking
    // this while a route, travel, or NPC interaction is in progress; see the
    // gate at the call site in Update().
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

    // Collect every walkable, unoccupied tile within range of the target,
    // ordered by "closest to the target, then closest to the hero" — the old
    // single best pick — and then take the first one A* can actually REACH.
    //
    // Why reachability matters (2026-09-03): NPCs behind counters. Market's
    // MillionaireLee stands at (242,242) inside a walled stall; the tiles
    // around him are walkable in the map file but sealed off from the
    // street (offline A* from the Market landing: NO PATH, altitude or not).
    // The old picker chose the tile touching him, FindPath came back empty,
    // and the store flow retried every 430 ms for minutes without moving —
    // the user had to walk the character over by hand. (237,236), four
    // tiles out in front of the counter, is reachable and is what a player
    // actually stands on. Same failure shape applies to any bank/warehouse
    // NPC behind furniture.
    struct Cand { Position pos; int targetDist; float heroDist; };
    std::vector<Cand> cands;
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
            cands.push_back({candidate, targetDist, effectivePos.DistanceTo(candidate)});
        }
    }
    if (cands.empty())
        return false;

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.targetDist != b.targetDist ? a.targetDist < b.targetDist : a.heroDist < b.heroDist;
    });

    // One flood from the hero, then scan candidates against it. Per-candidate
    // A* was measured offline against Lee's stall and is bad in BOTH
    // directions: the first 34 candidates are sealed, and each failed A*
    // exhausts a whole connected region — the street (~6k tiles) going
    // forward, the building interior (~17k) going backward, ~570k node
    // expansions in total. The flood is one ~6k pass.
    const int hx = effectivePos.x, hy = effectivePos.y;
    const std::vector<uint8_t> reach = map->FloodReachable(hx, hy);
    const int mapW = map->m_sizeMap.iWidth;
    auto reachable = [&](const Position& c) {
        return !reach.empty() && reach[(size_t)c.y * mapW + c.x] != 0;
    };
    const DWORD nearTargetNow = GetTickCount();
    const bool tileExcluded = nearTargetNow < m_nearTargetExcludeUntilTick;
    for (size_t i = 0; i < cands.size(); ++i) {
        const Position& c = cands[i].pos;
        if (c.x == hx && c.y == hy)
            return false;                          // already standing on a valid tile
        if (!reachable(c))
            continue;                              // walkable but sealed off — next
        if (tileExcluded && c.x == m_nearTargetExcludedTile.x && c.y == m_nearTargetExcludedTile.y)
            continue;                              // just proved stuck — give the next candidate a turn

        // Session 18 [NEAR-TARGET STUCK FIX]: FloodReachable calling this tile
        // fine doesn't mean StartPathTo can actually activate a path to it —
        // see the member declarations' comment. Detect zero progress (same
        // candidate, same hero origin, tick after tick) and exclude the tile
        // once it's gone on too long, rather than retrying it forever.
        if (c.x != m_nearTargetStuckTile.x || c.y != m_nearTargetStuckTile.y
            || hx != m_nearTargetStuckOrigin.x || hy != m_nearTargetStuckOrigin.y) {
            m_nearTargetStuckTile = c;
            m_nearTargetStuckOrigin = {hx, hy};
            m_nearTargetStuckSinceTick = nearTargetNow;
        } else if (nearTargetNow - m_nearTargetStuckSinceTick > kNearTargetStuckTimeoutMs) {
            spdlog::warn("[hunt] near-target: ({},{}) made no progress from ({},{}) for over {}ms — "
                         "likely blocked by another entity on the route; excluding it for {}ms and trying the next candidate",
                         c.x, c.y, hx, hy, kNearTargetStuckTimeoutMs, kNearTargetExcludeCooldownMs);
            m_nearTargetExcludedTile = c;
            m_nearTargetExcludeUntilTick = nearTargetNow + kNearTargetExcludeCooldownMs;
            continue;                              // try the next-best candidate in this same call
        }

        if (i > 0)
            spdlog::info("[hunt] near-target: skipped {} unreachable tile(s) around ({},{}), using ({},{})",
                         (int)i, targetPos.x, targetPos.y, c.x, c.y);
        return StartPathTo(hero, map, c, 0);
    }
    spdlog::warn("[hunt] near-target: no reachable tile within {} of ({},{}) from ({},{}) (checked {})",
                 searchRadius, targetPos.x, targetPos.y, hx, hy, (int)cands.size());
    return false;
}

bool BaseHuntPlugin::TrySteerTowardZoneClump(CHero* hero, CGameMap* map, const AutoHuntSettings& settings)
{
    if (!hero || !map)
        return false;

    // With no attack-range limit, every in-zone monster is already an
    // engageable target, so normal targeting would have returned one and we'd
    // never reach here — there's nothing "out of attack range but in zone" to
    // steer toward. (Also avoids double work when the user runs unlimited.)
    if (settings.mobSearchRange <= 0)
        return false;

    // The full in-zone monster set the minimap shows, INCLUDING mobs past
    // mobSearchRange — exactly the clumps the user reports the bot ignoring.
    // Zone geometry + name/tier filters still apply, so this stays bounded to
    // the hunt zone and never chases across the map.
    std::vector<CRole*> zoneTargets = CollectHuntTargets(settings, false, /*ignoreSearchRange=*/true);
    // Phase 2b: keep clump-steering inside the dynamic zone's active cells so
    // Map-Wide works the current hot area instead of chasing a clump on the
    // far side of the map (the zone grows toward live clumps and relocates
    // wholesale when it dries up).
    if (IsDynamicZoneActive(settings)) {
        zoneTargets.erase(
            std::remove_if(zoneTargets.begin(), zoneTargets.end(),
                [this](CRole* m) { return !m || !InDynamicZone(m->m_posMap); }),
            zoneTargets.end());
    }
    if (zoneTargets.empty())
        return false;

    const Position effectivePos = GetEffectiveHeroPosition(hero);
    const int clumpRadius = (std::max)(1, settings.clumpRadius);
    int clusterSize = 0;
    CRole* clump = FindBestClusterTarget(zoneTargets, effectivePos, (float)clumpRadius, &clusterSize);
    // No real cluster (all scattered singles) — still head to the nearest mob
    // so the bot closes on SOMETHING instead of random patrol.
    if (!clump) {
        clump = FindClosestTarget(zoneTargets, effectivePos);
        clusterSize = clump ? 1 : 0;
    }
    if (!clump)
        return false;

    // If the chosen clump is already within attack range, normal targeting
    // should have engaged it — don't shadow that with a redundant steer.
    const int dist = CGameMap::TileDist(effectivePos.x, effectivePos.y,
        clump->m_posMap.x, clump->m_posMap.y);
    if (dist <= settings.mobSearchRange)
        return false;

    // Stop a little inside mobSearchRange so that, on arrival, the clump is
    // comfortably within attack range and normal scatter/shoot targeting takes
    // over on the next tick (which also Stops this steer-path).
    const int stopRange = (std::max)(1, settings.mobSearchRange - 1);
    if (StartPathTo(hero, map, clump->m_posMap, stopRange)) {
        m_targetId = 0;
        char buf[96];
        snprintf(buf, sizeof(buf), "Approaching mob clump (%d mobs, %d tiles)", clusterSize, dist);
        SetState(AutoHuntState::AcquireTarget, buf);
        return true;
    }
    return false;
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

    if (m_lastMapId == MAP_MARKET) {
        // Already at Market — this is a RE-DISPATCH of an in-progress town
        // visit (commonly an HP-potion Recover tick falling through to the
        // town-run check below), NOT a fresh run. Do NOT reset the
        // repair/store/arrow sequences here: resetting mid-repair used to wipe
        // the phase every time a potion fired, so the bot stripped its gear one
        // piece per potion-free gap but never advanced to actually repairing,
        // then wandered off once NeedsRepair went false. The fresh reset for a
        // new visit happens once on arrival in HandleTravelToMarket instead.
        if (settings.autoRepair && m_townService.NeedsRepair(hero, settings)) {
            SetState(AutoHuntState::Repair, "Moving to Pharmacist");
        } else if (settings.autoStore && (m_townService.NeedsStorage(hero, settings)
                || (settings.immediateReturnOnPriorityItems && m_townService.HasPriorityReturnItems(hero, settings)))) {
            SetState(AutoHuntState::StoreItems, "Processing storage rules");
        } else if (!TryDispatchArrowRestock(travel, hero, settings)) {
            BeginTravelToZone(travel, settings);
        }
        return;
    }

    // Fresh town run starting from elsewhere — safe to reset the sequences.
    m_townService.ResetRepairSequence();
    m_townService.ResetBuyArrowsSequence();
    m_townService.ResetStoreSequence();

    travel->StartTravel(MAP_MARKET, kMarketLandingPos);
    SetState(AutoHuntState::TravelToMarket, "Traveling to Market");
}

bool BaseHuntPlugin::TryDispatchArrowRestock(TravelPlugin* travel, CHero* hero, const AutoHuntSettings& settings)
{
    if (!NeedsTownRunArrows(hero, settings))
        return false;

    if (HuntTownService::HasBlacksmithOnMap(m_lastMapId)) {
        m_townService.ResetBuyArrowsSequence();
        SetState(AutoHuntState::BuyArrows, "Buying arrows");
        return true;
    }
    if (HuntTownService::HasBlacksmithOnMap(settings.zoneMapId)) {
        BeginTravelToZone(travel, settings);
        return true;
    }
    // Session 16b [ARROW-RESTOCK FALLBACK]: neither Market nor the hunt zone
    // has a known blacksmith (e.g. a zone on Ape City 2, which has no vendor
    // NPCs of its own) — fall back to the nearest known blacksmith city.
    // BuyArrows' own finishBuyArrows() already routes back to the real hunt
    // zone afterward via beginTravelToZoneFn.
    OBJID fallbackMapId = 0;
    Position fallbackPos = {};
    if (travel && HuntTownService::GetFallbackBlacksmithCity(
            Game::GetCurrentMapId(), hero ? hero->m_posMap : Position{0, 0},
            fallbackMapId, fallbackPos)) {
        m_blacksmithTravelMapId = fallbackMapId;
        travel->StartTravel(fallbackMapId, fallbackPos);
        SetState(AutoHuntState::TravelToBlacksmith, "Traveling to buy arrows");
        return true;
    }
    return false;
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
    } else if (!TryDispatchArrowRestock(travel, hero, settings)) {
        BeginTravelToZone(travel, settings);
    }
}

void BaseHuntPlugin::HandleTravelToBlacksmith(TravelPlugin* travel, const AutoHuntSettings& settings)
{
    // The user can untick "Buy Arrows" mid-run. Without this the state
    // machine ignored the toggle until the multi-hop trip finished (live
    // 2026-09-03: unticked while en route to Twin City, kept going). Abort
    // the trip and head back to the zone instead.
    if (!settings.buyArrows) {
        if (travel) travel->CancelTravel();
        spdlog::info("[hunt] Buy Arrows disabled mid-trip — cancelling blacksmith run, returning to zone");
        BeginTravelToZone(travel, settings);
        return;
    }
    if (!travel) {
        SetState(AutoHuntState::Failed, "Travel plugin not available");
        return;
    }

    if (travel->GetState() == TravelState::Failed) {
        SetState(AutoHuntState::Failed, "Failed to reach a blacksmith city");
        return;
    }

    if (travel->IsTraveling()) {
        SetState(AutoHuntState::TravelToBlacksmith, "Traveling to buy arrows");
        return;
    }

    if (m_lastMapId != m_blacksmithTravelMapId) {
        SetState(AutoHuntState::Failed, "Lost track of the blacksmith destination");
        return;
    }

    m_townService.ResetBuyArrowsSequence();
    SetState(AutoHuntState::BuyArrows, "Buying arrows");
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
    } else if (settings.zoneMode == AutoHuntZoneMode::MapWide) {
        // Bounding box of the dynamic zone's active cells — now that
        // IsPointNearHuntZone actually bounds MapWide (see hunt_settings.cpp),
        // this path gets exercised for real: a hero who's drifted past the
        // leash needs a real walkable tile inside the zone to path back to.
        const std::vector<Position> cells = GetDynZoneCellCenters();
        if (cells.empty())
            return false;
        const int half = m_dynCellTiles / 2;
        minX = maxX = cells.front().x;
        minY = maxY = cells.front().y;
        for (const Position& c : cells) {
            minX = (std::min)(minX, c.x - half);
            maxX = (std::max)(maxX, c.x + half);
            minY = (std::min)(minY, c.y - half);
            maxY = (std::max)(maxY, c.y + half);
        }
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

    // Mutual exclusion between BaseHuntPlugin subclasses is enforced by
    // ApplyHuntModeSelection(), not here — see its dynamic_cast<BaseHuntPlugin*>
    // scan over all plugins.

    if (!hero || !map) {
        SetState(AutoHuntState::WaitingForGame, "Waiting for hero and map");
        return;
    }

    // Session 10: independent of the decision throttle below — paced on its
    // own timer so it can be dialed down for testing without being tied to
    // how often the rest of the decision logic runs.
    //
    // 2026-09-03: ONLY while idle in the zone, and never over an active
    // route. This used to run unconditionally, before any state gate, and
    // StartWalkTo() stops the pathfinder to take its 1-2 tile hop — so every
    // ~5.6 s it killed whatever route was in progress. Live: the travel
    // plugin logged "Path interrupted externally, replanning" on that exact
    // cadence all the way to Twin City, and at MillionaireLee it hopped the
    // hero BETWEEN dialog activates. (Walking does NOT close an NPC dialog
    // in this game — user-confirmed 2026-09-03 — so the hop is a distance
    // and timing problem there, not a dialog-cancel.) Humanizing an idle
    // hero is the whole point of this; humanizing a hero mid-route or
    // mid-interaction is just sabotage.
    if (m_state == AutoHuntState::AcquireTarget
        && !Pathfinder::Get().IsActive()
        && !(travel && travel->IsTraveling())) {
        TryRandomWalk(hero, map, settings, GetTickCount());
    }

    m_lootMgr.PruneLootPickupAttempts(hero, map);
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

        // Don't drink combat HP/mana potions in the Market — it's a safe zone
        // (nothing damages you there), so healing is pointless AND actively
        // harmful here: this runs BEFORE the town-task dispatch below, so a
        // character that arrives low on HP would flip to Recover every tick and
        // never reach the repair/store/leave logic. Live-repro: an archer
        // arrived in Market with a meteor to deposit, sat drinking potions in a
        // Recover loop, and never stored it until it was disabled. Potions
        // resume normally the moment it's back in a hunting zone.
        if (Game::GetCurrentMapId() != MAP_MARKET
            && m_buffMgr.TryUsePotions(hero, settings, m_lastHp, m_lastMaxHp, m_lastMana, m_lastMaxMana, buffCb))
            return;
    }

    // Subclass combat-item management (arrows, etc.)
    if (HandleCombatItems(hero, settings))
        return;

    if (m_state == AutoHuntState::TravelToMarket) {
        HandleTravelToMarket(travel, hero, settings);
        return;
    }

    if (m_state == AutoHuntState::TravelToBlacksmith) {
        HandleTravelToBlacksmith(travel, settings);
        return;
    }

    // A Recover tick (HP potion, meteor-pack, trash-drop) must never abandon an
    // in-progress repair while a piece of gear is sitting unequipped in the bag
    // — resume repairing so it gets repaired and re-equipped. Without this the
    // bot could wander off with gear off the character if the interrupting
    // action fired while the last item was mid-flight (NeedsRepair looks at
    // EQUIPPED gear, so it reads false while the last piece is unequipped).
    if (m_state == AutoHuntState::Recover && m_townService.IsRepairInProgress()) {
        SetState(AutoHuntState::Repair, "Resuming repair after recovery");
    }

    if (m_state == AutoHuntState::Repair) {
        m_townService.HandleRepairState(hero, map, settings, MakeTownCallbacks(travel, hero, settings));
        return;
    }

    if (m_state == AutoHuntState::BuyArrows) {
        // Same as HandleTravelToBlacksmith: the toggle can be unticked while
        // this state is active — honour it instead of finishing the purchase.
        if (!settings.buyArrows) {
            spdlog::info("[hunt] Buy Arrows disabled mid-purchase — abandoning, returning to zone");
            m_townService.ResetBuyArrowsSequence();
            BeginTravelToZone(travel, settings);
            return;
        }
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

    // Session 16: the standalone "pack anywhere via UseItem" block that used
    // to live here is gone — it never actually worked (there's no NPC-less
    // combine mechanic; packing genuinely requires visiting MillionaireLee).
    // Packing is now StorePhase::PackMeteors inside HandleStoreState, which
    // only runs at Market as part of a town trip already happening for
    // another reason (bag-full/repair/DragonBall), matching how the user
    // actually wants this to ride along rather than trigger its own trip.

    // Repair / storage / emergency-out-of-arrows — at Market. A comfort-level
    // arrow shortage (below settings.arrowBuyCount) is NOT a trigger here —
    // it only rides along once one of these other reasons already got the
    // hero to Market, via TryDispatchArrowRestock at the tail of that visit
    // (BeginTravelToMarket / HandleTravelToMarket). Only truly running OUT of
    // arrows (NeedsTownRunArrowsEmergency) is urgent enough to justify a trip
    // by itself, mirroring how needsRepair already works.
    if (m_townService.NeedTownRun(hero, settings, NeedsTownRunArrowsEmergency(hero, settings))) {
        BeginTravelToMarket(travel, hero, settings);
        return;
    }

    // Same leash tolerance as above — do not travel back to the zone just
    // because the hero stepped out to engage something on the edge.
    // Session 11 [Paranoia Mode]: widen the leash while a threat is
    // currently detected, so normal zone-return logic doesn't yank the hero
    // back toward (or past) the player mid-evasion. Reverts to the normal
    // leash the instant no threat is detected — the bot then settles back
    // toward the zone on its own via normal target/explore selection,
    // rather than needing an explicit "return to zone" step.
    // Session 14 [Paranoia evasion redesign]: advance the flee/relocate state
    // machine now, so this tick's loot-drop and leash-widen decisions already
    // reflect whether we're evading (see UpdateEvadeState). evadeActive covers
    // both Fleeing and Relocating — the leash stays loose through the whole
    // sequence so zone-return never yanks the hero mid-evasion (and a gentle
    // flee is allowed to briefly step out of the zone to break line of sight).
    // Phase 2: advance the Map-Wide dynamic-zone engine (moving/resizing hot
    // circle) before the evade + zone decisions below read from it.
    UpdateDynamicZone(hero, settings);
    const bool evadeActive = UpdateEvadeState(hero, settings);
    const int leashMargin = evadeActive ? GetHuntLeash(settings) * 3 : GetHuntLeash(settings);
    if ((Game::GetCurrentMapId() != settings.zoneMapId
         || !IsPointNearHuntZone(settings, Game::GetCurrentMapId(), hero->m_posMap, leashMargin))
        && !TickIsFuture(m_zoneUnreachableUntilTick, now)) {
        // Session 11 [LOCKUP FIX]: give up after repeated failures instead of
        // retrying forever — see m_zoneTravelFailCount's comment in the
        // header. A genuinely-unreachable zone (bad gateway data, wrong
        // zoneMapId) used to loop this every tick indefinitely with no way
        // to stop it short of editing the config file with the game closed.
        if (m_zoneTravelFailCount >= kMaxZoneTravelFailures) {
            m_zoneTravelFailCount = 0;
            // Session 15 [ZONE-UNREACHABLE FALLBACK]: if we're already ON the
            // zone map but the anchor is unreachable (FindPath keeps returning
            // empty — e.g. the Ape City 2 portal landed us across an impassable
            // center), do NOT disable the whole hunt. Hunt from the current
            // position and back off retrying the anchor for a while; normal
            // targeting/exploration takes over from here. Only the genuinely
            // WRONG-map case (nothing local to fall back to) still disables.
            if (Game::GetCurrentMapId() == settings.zoneMapId) {
                spdlog::warn("[hunt] Hunt-zone anchor unreachable on map {} — hunting from current position instead of disabling (retry in 60s).",
                    settings.zoneMapId);
                m_zoneUnreachableUntilTick = now + 60000;
                m_targetId = 0;
                SetState(AutoHuntState::AcquireTarget, "Anchor unreachable — hunting here");
                return;
            }
            spdlog::error("[hunt] Giving up reaching hunt zone after {} failed attempts — disabling hunting. Check zoneMapId ({}) and gateway routes.",
                kMaxZoneTravelFailures, settings.zoneMapId);
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

    // Session 13/14 [Paranoia active evasion]: while a threat is detected,
    // NO loot gets to detour the hero — evasion wins over everything,
    // including meteors and DragonBalls. Session 14 change, per explicit
    // user direction: "if a player is detected nearby, the bot should evade
    // over picking up the meteor or DragonBall, or any wanted item when
    // paranoid mode is enabled." (Previously a DragonBall was the one
    // exception; that carve-out is removed.) Treating it as if nothing was
    // found here makes the entire loot-phase block below naturally no-op and
    // fall through to the evasion check further down, without duplicating any
    // of its logic.
    if (loot && evadeActive)
        loot = nullptr;

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

        // Session 11: don't let ORDINARY loot pull the hero away from an
        // engageable monster — only the free/no-detour pickup above
        // (already attempted; it fires regardless of nearby combat) was
        // previously allowed to win against combat. Priority items
        // (IsMeteorOrDragonBallItem) are an explicit exception per the
        // user's own framing — "if a meteor or dragonball is seen on the
        // ground, stop everything and go get it. This behavior is specific
        // to meteors and dragonballs only" — so they always override an
        // engageable target, restoring the original "loot always wins"
        // behavior just for these specific items instead of every ground
        // item. Also skips the FindBestTarget peek entirely in that case
        // since its result wouldn't be used anyway.
        //
        // Session 14 [URGENT LOOT]: urgent loot (meteor/DragonBall + wanted
        // quality/+1, see IsUrgentLootItem) does NOT defer to a newly-
        // engageable combat target — it's worth interrupting the hunt for.
        // Session 13 had removed +items from this override because the plus
        // read was unreliable (a misread let ordinary equipment override
        // everything); that reliability problem is fixed (GetPlus() now
        // self-validates), so wanted quality/+1 correctly get the urgency
        // again, alongside the meteor/DragonBall it always covered.
        const bool isPriorityLoot = HuntTownService::IsUrgentLootItem(settings, *loot);

        // Session 13 [LOOT COMMITMENT]: hasEngageableTarget below is a fresh
        // peek EVERY decision tick (~150ms) — once combat got frequent enough
        // (a live 11-minute human-speed session logged 248 scatter casts),
        // almost every loot walk got aborted mid-flight the instant a new
        // target became engageable, before the hero ever physically reached
        // the item: only 21 "Jumping to loot" starts produced 4 total
        // pickups, and silver sat completely flat across the whole session
        // despite 657 kills. Once a walk toward a SPECIFIC item has begun,
        // stick with it for a short bounded window regardless of
        // hasEngageableTarget — long enough for one human-paced walk to
        // land, short enough that this can never become a new way to starve
        // combat: it protects one grab already in flight, then reopens to
        // normal priority for whatever loot is nearest next.
        constexpr DWORD kLootCommitMs = 2000;
        const bool lootCommitted = m_lootCommitId == loot->m_id
            && TickIsFuture(m_lootCommitUntilTick, now);

        bool hasEngageableTarget = false;
        if (!isPriorityLoot && !lootCommitted) {
            // Cheap peek: FindBestTarget is a pure scan with no side
            // effects, and gets called again below for the real combat
            // handling — this just decides whether it's worth detouring
            // for loot first.
            Position peekApproachPos{}, peekAttackPos{};
            int peekClumpSize = 0;
            bool peekUseScatter = false;
            hasEngageableTarget = FindBestTarget(hero, map, settings,
                &peekApproachPos, &peekAttackPos, &peekClumpSize, &peekUseScatter) != nullptr;
        }

        if (!midMovement && (!hasEngageableTarget || lootCommitted)) {
            // Session 13 [LOOT-BINGE FIX]: with no engageable target, ordinary
            // loot used to win over walking to the next visible pack — a
            // drop-rich field kept re-winning until it was picked dry while a
            // dense clump sat 15 tiles away untouched (live-measured: 89
            // kills/min during the binge phase vs 243/min in hunt flow, ~30%
            // of the run at ~40% speed). Drops don't despawn in the next 30s
            // and the hunt sweep passes back through, so: gold basically
            // underfoot is still grabbed (a free detour), priority items
            // (meteors/dragonballs) still stop everything, but anything
            // further defers to a steerable pack when one exists. Dedicated
            // hoovering still happens whenever no pack is visible (steer
            // returns false).
            //
            // Session 13 [HUMAN-SPEED LOOT FIX]: the "3 tiles is basically
            // free" assumption only holds when a jump is nearly instant
            // (aggressive/speedhack mode). At the user's own human-paced
            // slider settings a 3-tile detour costs a full movement-interval
            // slot (~900ms+) — the same order of cost as an actual attack —
            // while one scatter volley can drop 5+ coins at once. Live
            // session: ground-item count climbed from ~70 to ~150 over ~6
            // minutes (collection losing to generation), and a full state-
            // duration breakdown showed 70% of session TIME in "Loot Nearby"
            // vs 5.8% actually attacking. Tightened to 0 (truly underfoot,
            // already-standing-on-it) when not aggressive — the always-on
            // free pickup above (IsWithinLootPickupRange + TryPickupLootItem)
            // already covers that case with zero dedicated travel; anything
            // requiring so much as a single extra step now defers to a
            // steerable pack, same as the >3 case already did.
            const int lootUnderfootTiles = ShouldUseAggressiveSpeeds(settings) ? 3 : 0;
            if (!isPriorityLoot && !lootCommitted && lootDist > lootUnderfootTiles
                && TrySteerTowardZoneClump(hero, map, settings))
                return;
            if (StartPathNearTarget(hero, map, loot->m_pos, kLootPathStopRange)) {
                m_targetId = loot->m_id;
                if (m_lootCommitId != loot->m_id) {
                    m_lootCommitId = loot->m_id;
                    m_lootCommitUntilTick = now + kLootCommitMs;
                }
                SetState(AutoHuntState::LootNearby, "Jumping to loot");
                return;
            }
            // Session 10: this branch means dist != 0 (the pickup attempt
            // above already handles dist==0) AND the pathfinder failed to
            // queue a route to close that gap — genuinely stuck (corner-
            // blocked, occupied, etc.), so feed it into the same
            // attempt-limit/ignore mechanism pickup failures already use.
            //
            // Session 13 [PRIORITY-ITEM FALSE-STUCK FIX]: the lootDist > 0
            // guard is load-bearing, not redundant with the comment above —
            // IsWithinLootPickupRange returns true at ANY distance for a
            // priority item (meteor/DragonBall), so with dist==0 this branch
            // used to fire anyway, misreading "the free-grab attempt above
            // was blocked by a normal ~500ms post-jump settle timer" as
            // "stuck," burning both pickup attempts in the same tick and
            // blacklisting the item for lootPickupIgnoreMs (~19.5s in this
            // user's config) while the hero was standing directly on top of
            // it. Live-reported: an archer's priority jumps chain-land
            // straight onto a meteor from a long approach, which is exactly
            // when the settle timer is still counting down, so this raced
            // almost every single arrival — "ran past a meteor about 6
            // times." A real stuck condition always has dist > 0 (can't path
            // the remaining gap); dist==0 means the hero already arrived and
            // just needs the settle timer to finish, which the free-grab
            // check above will complete on its own within ~500ms.
            if (lootDist > 0 && IsWithinLootPickupRange(settings, lootDist, *loot)) {
                m_lootMgr.RecordLootPickupAttempt(loot->m_id, now, settings);
                m_targetId = loot->m_id;
                SetState(AutoHuntState::LootNearby, "Settling on nearby loot");
                return;
            }
            spdlog::warn("[hunt-loot] Failed to path to loot id={} at ({},{}) dist={}",
                loot->m_id, loot->m_pos.x, loot->m_pos.y, lootDist);
        }
    }

    // Session 13/14 [Paranoia active evasion]: the actual interrupt. Paranoia's
    // target/explore biasing only runs when nothing more urgent is happening,
    // but a good hunting spot always has a target, so evasion never triggered —
    // the bot kept fighting next to a detected player. Here, whenever the evade
    // state machine (UpdateEvadeState, run above) is active, skip engaging a
    // target this tick and hand movement to DriveEvadeMovement: flee away at
    // human pace while a player can see us, relocate (speed-hack) to a hot area
    // once out of sight, then return home if they were only passing. Loot
    // pickup above is already gated off for the whole evade sequence.
    if (evadeActive) {
        if (DriveEvadeMovement(hero, map, settings))
            return;
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
        // Session 13 [LOOT-BINGE FIX]: same pack-over-distant-loot rule as the
        // loot-phase block above — non-priority loot beyond underfoot range
        // defers to a steerable pack when one exists. Session 13
        // [HUMAN-SPEED LOOT FIX]: underfoot range is 0 (not 3) when not
        // aggressive — see the loot-phase block's comment for the full
        // reasoning (a 3-tile "free" detour costs a real movement-interval
        // slot at human speed, not the ~0 it costs at speedhack speed).
        const int noTargetLootDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
            loot->m_pos.x, loot->m_pos.y);
        const int lootUnderfootTiles = ShouldUseAggressiveSpeeds(settings) ? 3 : 0;
        if (noTargetLootDist > lootUnderfootTiles
            && !HuntTownService::IsUrgentLootItem(settings, *loot)
            && TrySteerTowardZoneClump(hero, map, settings))
            return;
        spdlog::trace("[hunt-loot] No combat target, pathing to loot id={} type={} at ({},{})",
            loot->m_id, loot->m_idType, loot->m_pos.x, loot->m_pos.y);
        const bool startedMove = StartPathNearTarget(hero, map, loot->m_pos, kLootPathStopRange);
        if (!startedMove)
            m_lootMgr.RecordLootPickupAttempt(loot->m_id, now, settings);
        m_targetId = loot->m_id;
        SetState(AutoHuntState::LootNearby, startedMove ? "Jumping to loot" : "Settling on nearby loot");
        return;
    }

    // Nothing engageable within attack range — but the minimap may still show
    // big clumps deeper in the zone (mobSearchRange only gates ATTACK range, so
    // those clumps are deliberately not attack-targets yet). Walk toward the
    // nearest/densest one instead of random patrol/explore, so the bot actually
    // goes and hunts what it can see. Shared across melee/archer and every zone
    // mode; takes priority over the subclass patrol and the generic explore
    // fallback below. Once it arrives, normal targeting engages on the next tick.
    if (TrySteerTowardZoneClump(hero, map, settings))
        return;

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

// Session 13: shared by every zone mode (see FindZoneExplorePosition's
// exploration-roll comment) — factored out since it's shown from both the
// Map-Wide branch and the shared leash section below for Circle/Polygon/Route.
void RenderExplorationBiasUI(AutoHuntSettings& settings)
{
    ImGui::SliderInt("Exploration Chance %", &settings.explorationChancePercent, 0, 100);
    HelpMarkerOnSameLine("Chance, each exploration decision, to deliberately walk toward "
                         "an unscanned/low-density spot instead of the best-known one. "
                         "0 = always exploit the best-known spot (can get stuck on the "
                         "first hot spot found and never map the rest of the zone).");
}

void BaseHuntPlugin::RenderZoneSetupUI(AutoHuntSettings& settings, CHero* hero)
{
    // Session 13: what the hunt is FOR, gates everything below it (see
    // AutoHuntGoal's comment in hunt_settings.h). Placed first since Level
    // mode is what makes Map-Wide actually useful — a hand-drawn zone stays
    // fine for Farm, but Level wants the whole map so it can keep finding
    // appropriately-tiered monsters as the hero's level (and therefore the
    // tier boundaries) drifts.
    int huntGoal = static_cast<int>(settings.huntGoal);
    if (ImGui::Combo("Hunt Goal", &huntGoal, "Farm (efficiency, no danger gating)\0Level (gate by monster danger tier)\0"))
        settings.huntGoal = static_cast<AutoHuntGoal>(huntGoal);
    HelpMarkerOnSameLine("Farm: kill whatever is efficient, no color/tier gating at all.\n"
                         "Level: only engage monsters at or below the Max Danger Tier "
                         "below, relative to your CURRENT level — the same green/white/"
                         "red/black name-color system the game itself shows.");

    if (settings.huntGoal == AutoHuntGoal::Level) {
        int tier = static_cast<int>(settings.maxDangerTier);
        if (ImGui::SliderInt("Max Danger Tier", &tier, 0, 3,
                tier == 0 ? "Green only" : tier == 1 ? "White (default)" : tier == 2 ? "Red" : "Black"))
            settings.maxDangerTier = static_cast<MonsterDangerTier>(tier);
        HelpMarkerOnSameLine("Highest name-color tier this character will engage. Raise "
                             "this for a tanky, well-geared class that can safely fight "
                             "above its own level (e.g. a Warrior surviving Black-name "
                             "areas); leave it lower for a squishier class.");
    }

    static const char* kZoneModes[] = { "Circle", "Polygon", "Route", "Map-Wide" };
    int zoneMode = static_cast<int>(settings.zoneMode);
    if (ImGui::Combo("Zone Shape", &zoneMode, kZoneModes, IM_ARRAYSIZE(kZoneModes))) {
        settings.zoneMode = static_cast<AutoHuntZoneMode>(zoneMode);
        m_zoneCaptureMode = ZoneCaptureMode::None;
    }

    if (settings.zoneMode == AutoHuntZoneMode::MapWide) {
        ImGui::Text("Zone Map: %u", settings.zoneMapId);
        HelpMarkerOnSameLine("No shape to draw — the bot builds its own zone on this map "
                             "as it hunts, growing/shrinking toward live monster clusters "
                             "and relocating to a new hot spot when the current one dries "
                             "up (see the cyan cells on the minimap). Hunting is bounded to "
                             "that zone the same way Circle/Polygon bound themselves; "
                             "relocating lets it use the whole map over a session. "
                             "Use \"Use Hero Position\" below to set which map.");
        if (hero && ImGui::Button("Use Hero Position")) {
            settings.zoneMapId = Game::GetCurrentMapId();
        }
        if (hero && ImGui::Button("Mark Current Location as Start")) {
            settings.mapWideStartPos = hero->m_posMap;
        }
        ImGui::SameLine();
        if (!IsZeroPos(settings.mapWideStartPos)) {
            ImGui::Text("Start: (%d,%d)", settings.mapWideStartPos.x, settings.mapWideStartPos.y);
            ImGui::SameLine();
            if (ImGui::Button("Clear Start"))
                settings.mapWideStartPos = {};
        } else {
            ImGui::TextDisabled("Start: hero's position when hunting begins");
        }
        HelpMarkerOnSameLine("Simulates walking here yourself before turning the bot on — the "
                             "dynamic zone's very first cell seeds at this spot instead of "
                             "wherever the hero happens to be standing. One-time only: once "
                             "hunting is under way the zone grows/shrinks/relocates completely "
                             "independent of this point, same as if you'd walked here manually.");
        ImGui::SliderInt("Zone Cell Size", &settings.dynZoneCellTiles, 8, 100, "%d tiles");
        HelpMarkerOnSameLine("How large each cell of the dynamic zone is. Bigger cells cover "
                             "more ground per growth step and immediately claim more area on "
                             "a fresh relocate — useful for a fast build that outpaces the "
                             "default 8-tile grid. Changing this reseeds the zone (map-change "
                             "behavior) rather than resizing cells already grown, so it takes "
                             "a moment to rebuild after you move the slider.");
        ImGui::SliderInt("Max Zone Cells", &settings.dynZoneMaxCells, 4, 60);
        HelpMarkerOnSameLine("How many cells the zone can grow to at once. Smaller cells need "
                             "more of them to cover the same real ground, so raise this if "
                             "you're running a small Zone Cell Size and the zone keeps hitting "
                             "its cap instead of continuing to expand.");
        // The leash/anchor text further down still applies and is shown
        // unconditionally, so nothing else to render for this mode.
        ImGui::TextDisabled("Leash: %d tiles out of zone / engage %d tiles out%s",
                            GetHuntLeash(settings), GetHuntEngageMargin(settings),
                            settings.routeCorridorOverride > 0 ? " (override)" : " (from attack range)");
        ImGui::InputInt("Leash override (0=auto)", &settings.routeCorridorOverride);
        RenderExplorationBiasUI(settings);
        return;
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
            // Session 16b: routes only ever live in memory (never saved to
            // the ini) — this is the only way to get a recorded trail's
            // exact coordinates out for anything other than live patrol use
            // (e.g. handing a walkability-override polyline like a windy
            // bridge to Claude by pasting it directly into chat).
            ImGui::SameLine();
            ImGui::PushID(r.name.c_str());
            if (ImGui::SmallButton("Copy Waypoints")) {
                std::string csv;
                char buf[32];
                for (const Position& wp : r.waypoints) {
                    snprintf(buf, sizeof(buf), "%d,%d", wp.x, wp.y);
                    if (!csv.empty())
                        csv += ";";
                    csv += buf;
                }
                ImGui::SetClipboardText(csv.c_str());
            }
            ImGui::PopID();
        }
        if (settings.routes.empty())
            ImGui::TextDisabled("  (none recorded yet)");

        int linger = (int)settings.lingerMode;
        if (ImGui::Combo("Linger", &linger, "Auto\0Move on\0Stay and clear\0"))
            settings.lingerMode = (AutoHuntLingerMode)linger;
        HelpMarkerOnSameLine("Auto derives it from measured time-to-kill: one-shot "
                             "mobs = move on, slow kills = stay and clear.");
        ImGui::InputInt("Waypoint tolerance", &settings.routeWaypointTolerance);
        HelpMarkerOnSameLine("How close to a waypoint counts as \"arrived\" before advancing to the next one.");
    }

    // The leash governs every zone shape, so show it regardless of mode.
    ImGui::TextDisabled("Leash: %d tiles out of zone / engage %d tiles out%s",
                        GetHuntLeash(settings), GetHuntEngageMargin(settings),
                        settings.routeCorridorOverride > 0 ? " (override)" : " (from attack range)");
    ImGui::InputInt("Leash override (0=auto)", &settings.routeCorridorOverride);
    HelpMarkerOnSameLine("How far the bot's body may leave the zone. It still only "
                         "attacks what is within striking range from where it stands.");
    RenderExplorationBiasUI(settings);

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
            if (!roleRef || !Entities::IsAlive(roleRef) || !roleRef->IsMonster())
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
            if (roleRef && Entities::IsAlive(roleRef) && roleRef->GetID() == m_targetId) {
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
    ImGui::SliderInt("Only Attack Mobs Within", &settings.mobSearchRange, 0, CGameMap::MAX_JUMP_DIST);
    HelpMarkerOnSameLine(
        "How close a monster must be before the bot ATTACKS it (e.g. throws a "
        "scatter) -- keep this modest so it doesn't fire at mobs too far to "
        "actually hit.\n\n"
        "This does NOT blind the bot to distant clumps: when nothing is within "
        "this range, it now walks toward the nearest/densest clump anywhere in "
        "the hunt zone, then attacks once it arrives.\n\n"
        "0 = attack any monster in the zone regardless of distance.");

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
    HelpMarkerOnSameLine("Only limits how far to detour for money drops. Meteors/DragonBalls are always fetched regardless of range and interrupt everything else to go get it. Plussed gear is ordinary selectable loot, same as Loot Range and other priorities (see the Loot +1 Items checkbox below).");
    // Session 13 [UI GROUPING]: these three each carry different priority
    // rules from ordinary quality-checkbox loot (see IsMeteorOrDragonBallItem
    // and ShouldLootMapItem's plus-check), so they're grouped together rather
    // than mixed in with the quality row below. Meteor/DragonBall pickup was
    // previously unconditional — no way to turn it off at all — now
    // independently toggleable, each defaulting to on so existing configs
    // keep today's behavior.
    //
    // Minimum Loot Plus [UI SIMPLIFICATION]: was a 0-12 slider, but per the
    // user (who knows the game's actual drop tables): only +1 ever drops
    // from monsters — anything higher only comes from player crafting/
    // enhancement, never a ground find. A slider implying otherwise was
    // misleading. The backing field stays an int (>= threshold, unchanged
    // config-file compatibility) — only the widget changes to on/off.
    ImGui::Text("Priority items (different rules from ordinary quality loot below):");
    {
        bool lootPlusItems = settings.minimumLootPlus > 0;
        if (ImGui::Checkbox("Loot +1 Items##priorityplus", &lootPlusItems))
            settings.minimumLootPlus = lootPlusItems ? 1 : 0;
        // Session 14 [RE-DISABLED — DATA CAME BACK CONCLUSIVE]: was
        // temporarily re-enabled for live cross-verify testing (commit
        // a66d420); the data settled it rather than clearing it — two
        // bag-confirmed-unenchanted pickups (SyeniticHelmet, SlantWand) read
        // 80 and 64 on the ground, nowhere near a valid plus range. Disabled
        // again (commit e50dbbd... see hunt_town.cpp's ShouldLootMapItem for
        // the exact numbers and reasoning). Setting/checkbox still kept for
        // when the real offset gets verified.
        HelpMarkerOnSameLine("Picks up +1 (and higher) equipment from the ground. The ground-item plus read is now self-validating (it confirms the memory it reads actually belongs to the item before trusting the value), after live byte-dumps proved the offset correct for stable items and traced the earlier junk pickups to stale reads during fast hunting. Only +1 ever drops from monsters in this game; higher plus levels come from player crafting, never a ground find.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Loot Meteors##prioritymeteor", &settings.lootMeteor);
    HelpMarkerOnSameLine("When seen, stops everything and goes to get it regardless of Loot Range or an engageable target.");
    ImGui::SameLine();
    ImGui::Checkbox("Loot DragonBalls##prioritydragonball", &settings.lootDragonBall);
    HelpMarkerOnSameLine("Same override as Meteors, plus survives Paranoia Mode's active-evasion interrupt (Meteors and ordinary loot do not).");
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
    ImGui::Checkbox("Also Store These Qualities at Warehouse", &settings.storeSelectedLootQuality);
    HelpMarkerOnSameLine("Reuses the quality checkboxes above instead of a separate set — whatever you loot by quality "
                         "also gets deposited at the Warehouse on a town run for another reason (bag full, repair, "
                         "DragonBall). Off by default so existing loot-only setups aren't changed.");

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
    HelpMarkerOnSameLine("Visits MillionaireLee in Market and packs 10 Meteors into 1 MeteorScroll at a time, looping while "
                         "10+ remain, then deposits the scrolls at the Warehouse. Rides along on a town trip already "
                         "happening for another reason (bag full, gear repair, a DragonBall to deposit) rather than "
                         "triggering its own trip.");

    ImGui::SeparatorText("Bank Deposits");
    ImGui::Checkbox("Auto-Deposit Meteors/DragonBalls at Treasure Bank", &settings.storeTreasureBank);
    ImGui::Checkbox("Auto-Deposit +1/+2 Gear at Compose Bank", &settings.storeComposeBank);
    HelpMarkerOnSameLine("OFF by default: the Compose Bank's dialog window doesn't reliably open (a missing confirm packet, same class of "
                         "issue the Treasure Bank has), so a checked deposit here usually just silently does nothing after 3 attempts. Left "
                         "in for a future fix; use \"Store + Gear at Warehouse\" below instead.");
    ImGui::Checkbox("Store + Gear at Warehouse", &settings.storePlusGear);
    HelpMarkerOnSameLine("Any enchanted item (+1 or higher) is deposited at the Warehouse — the actual working path while Compose Bank is off above.");
    ImGui::TextDisabled("Items matching an entry on the Warehouse or Priority Return list below always go to the Warehouse regardless of these settings.");
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
        "Stealth evasion, distinct from Player Safety above - keeps hunting instead of fully retreating to "
        "Market. When any non-whitelisted player comes into view the bot GENTLY hops ~Flee Distance tiles away "
        "(just out of their ~30-tile view) at a human pace, then RESUMES hunting right there - no sprint across "
        "the map, no compulsive return. Only when a spot is genuinely compromised (the same place draws a player "
        "'Abandon Spot After' times, or one player keeps it in sight past 'Linger Before Relocate') does it "
        "RELOCATE for good to a new heatmap-hot area. Tune it all on the Advanced tab. MapWide zone works best. "
        "Does not apply to town runs or mining. There's no facing/camera data to read - this is positioning-based "
        "avoidance, not a guarantee of being unseen.");

    if (settings.paranoiaEnabled) {
        const char* evadeName =
            m_evadeState == EvadeState::Fleeing    ? "FLEEING (stepping out of view)" :
            m_evadeState == EvadeState::Relocating ? "RELOCATING (new area)" :
                                                     "idle (no player detected)";
        const bool evading = m_evadeState != EvadeState::None;
        ImGui::TextColored(evading ? ImVec4(1, 0.7f, 0.3f, 1) : ImVec4(0.4f, 1, 0.4f, 1),
            "Evasion: %s  (players tracked: %d, encounters: %d)",
            evadeName, (int)m_playerTracks.size(), m_evadeEncounterCount);
    }

    if (settings.paranoiaEnabled) {
        const HuntContest::Stats stats = HuntContest::GetStats(Game::GetCurrentMapId());
        ImGui::TextDisabled("Bucket camping-avoidance: %d spot(s) currently avoided, %d being watched",
                            stats.contestedBuckets, stats.trackedBuckets);
        HelpMarkerOnSameLine(
            "Separate from the distance-based evasion above: this remembers a specific spot "
            "(SpawnMemory bucket) a player has been sitting on continuously for a while, so "
            "the bot stops trying to walk onto it during exploration even between decisions "
            "where that player isn't the single nearest threat. A spot is only avoided after "
            "continuous presence, not a single sighting - someone just passing through isn't "
            "affected. Rechecked periodically, so a spot comes back once they've actually left.");
    }
}

void BaseHuntPlugin::RenderAdvancedSection()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();

    // Session 13 [RESET TO DEFAULTS]: right-aligned so it reads as a
    // corrective action for the slider block below, not a step in the normal
    // top-to-bottom flow. Copies from a default-constructed AutoHuntSettings{}
    // rather than repeating the 14 numbers here — see the [SHIPPED DEFAULTS]
    // comment on the struct fields (hunt_settings.h) for what they are and why.
    {
        const char* kResetLabel = "Reset to Defaults";
        const float buttonWidth = ImGui::CalcTextSize(kResetLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttonWidth);
        if (ImGui::Button(kResetLabel)) {
            const AutoHuntSettings defaults{};
            settings.movementIntervalMs = defaults.movementIntervalMs;
            settings.attackIntervalMs = defaults.attackIntervalMs;
            settings.cycloneAttackIntervalMs = defaults.cycloneAttackIntervalMs;
            settings.targetSwitchAttackIntervalMs = defaults.targetSwitchAttackIntervalMs;
            settings.itemActionIntervalMs = defaults.itemActionIntervalMs;
            settings.selfCastIntervalMs = defaults.selfCastIntervalMs;
            settings.npcActionIntervalMs = defaults.npcActionIntervalMs;
            settings.reviveDelayMs = defaults.reviveDelayMs;
            settings.reviveRetryIntervalMs = defaults.reviveRetryIntervalMs;
            settings.monsterSpawnGraceMs = defaults.monsterSpawnGraceMs;
            settings.entityScanIntervalMs = defaults.entityScanIntervalMs;
            settings.itemPickupDelayMs = defaults.itemPickupDelayMs;
            settings.decisionThrottleMs = defaults.decisionThrottleMs;
            settings.randomWalkIntervalMs = defaults.randomWalkIntervalMs;
            settings.paranoiaFleeDistance = defaults.paranoiaFleeDistance;
            settings.paranoiaAbandonAfter = defaults.paranoiaAbandonAfter;
            settings.paranoiaPassingThresholdMs = defaults.paranoiaPassingThresholdMs;
            settings.paranoiaRenudgeTiles = defaults.paranoiaRenudgeTiles;
        }
        HelpMarkerOnSameLine("Resets only the sliders on this Advanced tab back to their shipped defaults.");
    }

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
    ImGui::SliderInt("Newly Spawned Monster Attack Delay (ms)", &settings.monsterSpawnGraceMs, 0, 1000);
    HelpMarkerOnSameLine("A monster this freshly spawned isn't picked as a target yet. Helps if the bot gets stuck attacking a monster that's still mid-spawn-animation. 0 = off.");

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

    ImGui::SeparatorText("Paranoia Evasion");
    ImGui::TextDisabled("Tuning for Paranoia Mode's gentle-flee behavior (enable it on the Safety tab).");
    ImGui::SliderInt("Flee Distance (tiles)", &settings.paranoiaFleeDistance, 0, 100);
    HelpMarkerOnSameLine("How far to hop away from a detected player before resuming the hunt — just enough to "
        "leave their ~30-tile view, NOT a sprint across the map. 36 is a good default (a bit past view range). "
        "0 disables the hop (it will only relocate when a spot is contested).");
    ImGui::SliderInt("Abandon Spot After (encounters)", &settings.paranoiaAbandonAfter, 1, 20);
    HelpMarkerOnSameLine("How many times a player may be re-encountered at the current spot (within a couple "
        "minutes of each other) before the bot gives up on it and RELOCATES for good to a new heatmap-hot area, "
        "instead of shuffling out-and-back forever. The count resets after a stretch of undisturbed hunting.");
    ImGui::SliderInt("Linger Before Relocate (ms)", &settings.paranoiaPassingThresholdMs, 1000, 30000);
    HelpMarkerOnSameLine("If a single player keeps the bot in sight this long during one flee (they're "
        "following/camping, not just passing), skip straight to relocating rather than continuing to edge away.");
    ImGui::SliderInt("Re-nudge Distance (tiles)", &settings.paranoiaRenudgeTiles, 0, 40);
    HelpMarkerOnSameLine("If a player is also at the area the bot relocated to, the next relocate destination is "
        "pushed this many extra tiles farther from the threat, per reappearance. 0 = don't push farther.");
}

// Session 13: merged in Misc tab's former "Hunt Diagnostics" section (was a
// near-triplicate of the State/Reason text already shown here and on Ready
// Check, plus a bunch of genuinely useful diagnostics — pathfinder stuck
// detection, weapon-equip status, spawn memory, ground items, zone
// walkability, repair phase — that had no other home). This is now the one
// place for deep hunt-loop diagnostics; Ready Check stays a simpler
// at-a-glance summary.
void BaseHuntPlugin::RenderDebugSection()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    CHero* hero = Game::GetHero();

    ImGui::Text("State:  %s", GetStateName());
    ImGui::TextWrapped("Reason: %s", m_statusText);
    ImGui::Text("Last Target: %u", m_targetId);

    // Session 13: live readout for the speedhack player-detection gate —
    // added after a live report that movement stayed fast even with a
    // player well within Detection Range. Root cause found: the per-region
    // heap scan doesn't reliably re-capture the same player on every pass,
    // so raw playerNearby flickers true/false every ~100-300ms — a debounce
    // hold now smooths that into a stable "aggressive" decision (see
    // ShouldUseAggressiveSpeeds' [FLICKER FIX] comment). Both the raw,
    // un-debounced scan result AND the debounced decision are shown so a
    // flickering "raw" alongside a steady "aggressive=no" is the expected,
    // working signature — not a bug.
    {
        const bool toggleOn = GetTravelSettings().usePacketJump;
        const bool playerVisible = IsAnyPlayerVisibleDebounced(settings);
        const bool aggressive = ShouldUseAggressiveSpeeds(settings);
        const DWORD effInterval = GetMovementIntervalMs(settings);
        ImGui::TextColored(aggressive ? ImVec4(1, 0.7f, 0.3f, 1) : ImVec4(0.4f, 1, 0.4f, 1),
            "Speedhack: toggle=%s disableOnPlayer=%s playerVisible=%s -> aggressive=%s  interval=%ums",
            toggleOn ? "on" : "off", settings.disableSpeedhackOnPlayer ? "yes" : "no",
            playerVisible ? "yes" : "no", aggressive ? "yes" : "no", effInterval);

        // Session 13 [JUMP VARIANCE]: companion readout for the jump-distance
        // cap — re-rolled every call, so watching this line while a player is
        // held nearby should show the tile count moving around inside
        // 60-80% of MAX_JUMP_DIST rather than sitting fixed.
        const int jumpCap = GetJumpDistanceCapTiles(settings);
        const bool jumpCapped = jumpCap < CGameMap::MAX_JUMP_DIST;
        ImGui::TextColored(jumpCapped ? ImVec4(1, 0.7f, 0.3f, 1) : ImVec4(0.4f, 1, 0.4f, 1),
            "Jump distance: cap=%d/%d tiles%s",
            jumpCap, CGameMap::MAX_JUMP_DIST, jumpCapped ? " (varied — player nearby)" : " (uncapped)");
    }

    // Effective vs actual position. A mismatch means every range check AND
    // every scatter direction is being computed from a tile the hero isn't
    // on.
    if (hero) {
        const Position eff = GetEffectivePosDebug(hero);
        const Position act = hero->m_posMap;
        const DWORD pj = GetPendingJumpTick();
        if (eff.x != act.x || eff.y != act.y) {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                "AIMING FROM WRONG TILE: effective (%d,%d) vs actual (%d,%d)",
                eff.x, eff.y, act.x, act.y);
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                "  pendingJumpTick=%u dest=(%d,%d) age=%ums",
                pj, GetPendingJumpDest().x, GetPendingJumpDest().y,
                pj ? (GetTickCount() - pj) : 0);
        } else {
            ImGui::TextDisabled("effective pos == actual (%d,%d)%s",
                act.x, act.y, pj ? "  [pendingJump set]" : "");
        }
        // Pathfinder state. "Active" doubles as "movement is happening"
        // throughout the hunt loop and also gates the attack, so a path
        // that stops progressing freezes everything.
        Pathfinder& pf = Pathfinder::Get();
        if (pf.IsActive()) {
            const Position wp = pf.GetCurrentWaypoint();
            const DWORD sinceProgress = GetTickCount() - pf.GetLastProgressTick();
            const bool stuck = sinceProgress > 2000;
            ImGui::TextColored(stuck ? ImVec4(1, 0.3f, 0.3f, 1)
                                     : ImVec4(0.6f, 0.6f, 0.6f, 1),
                "path ACTIVE wp %d/%d -> (%d,%d)  noProgress=%ums%s",
                (int)pf.GetCurrentIndex(), (int)pf.GetWaypoints().size(),
                wp.x, wp.y, sinceProgress,
                stuck ? "  <-- STUCK, blocks attacks" : "");
        } else {
            ImGui::TextDisabled("path idle");
        }
    }

    ImGui::Separator();
    if (hero) {
        // ShootTarget() returns immediately with no weapon, so an
        // unequipped bow silently disables all ranged attacks — exactly
        // the failure seen after a repair run that unequipped but never
        // re-equipped.
        CItem* rw = hero->GetEquip(EquipSlot::RWEAPON);
        CItem* lw = hero->GetEquip(EquipSlot::LWEAPON);
        if (rw)
            ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1),
                "R.Hand: typeId=%u  (ranged attacks enabled)", rw->GetTypeID());
        else
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                "R.Hand: EMPTY -> ShootTarget() no-ops, archer cannot attack");
        ImGui::Text("L.Hand: %s",
            lw ? std::to_string(lw->GetTypeID()).c_str() : "(empty)");
    }

    const auto targets = CollectHuntTargets(settings);
    ImGui::Text("Targets passing filters: %d", (int)targets.size());
    if (!targets.empty() && hero) {
        CRole* t = targets.front();
        ImGui::TextDisabled("  nearest: %s id=%u (%d,%d) dist=%d",
            t->GetName(), t->GetID(), t->m_posMap.x, t->m_posMap.y,
            CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
                               t->m_posMap.x, t->m_posMap.y));
    }
    ImGui::TextDisabled("zoneMapId=%u  currentMap=%u  searchRange=%d",
                        settings.zoneMapId, Game::GetCurrentMapId(), settings.mobSearchRange);
    // The leash applies to every zone mode, not just routes.
    ImGui::TextDisabled("leash=%d tiles (body may leave zone by this)",
                        GetHuntLeash(settings));
    // IsJumping() sticking on is a silent killer: it gates the attack and
    // poisons the effective hero position.
    if (hero) {
        const bool jumping = hero->IsJumping() != 0;
        if (jumping)
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                "IsJumping=TRUE -> attacks are gated off. cmd target (%d,%d) vs pos (%d,%d)",
                hero->GetCommand().posTarget.x, hero->GetCommand().posTarget.y,
                hero->m_posMap.x, hero->m_posMap.y);
        else
            ImGui::TextDisabled("IsJumping=false (attacks allowed)");
    }
    // Show how much of a circle zone is actually usable.
    if (settings.zoneMode == AutoHuntZoneMode::Circle && settings.zoneRadius > 0) {
        if (MapGrid* g = GetCurrentMapGrid()) {
            int total = 0, walk = 0;
            const int r = settings.zoneRadius;
            for (int dy = -r; dy <= r; dy += 2)
                for (int dx = -r; dx <= r; dx += 2) {
                    if (dx * dx + dy * dy > r * r) continue;
                    ++total;
                    if (g->IsWalkable(settings.zoneCenter.x + dx, settings.zoneCenter.y + dy))
                        ++walk;
                }
            const float pct = total ? 100.0f * (float)walk / (float)total : 0.0f;
            if (pct < 70.0f)
                ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1),
                    "Zone is only %.0f%% walkable - shrink or move it", pct);
            else
                ImGui::TextDisabled("Zone walkable: %.0f%%", pct);
        }
    }
    ImGui::TextDisabled("engage margin=%d tiles (monsters valid this far out)",
                        GetHuntEngageMargin(settings));
    {
        // Spawn memory: shows whether the bot has learned enough about
        // this map to steer by it yet.
        const OBJID mid = Game::GetCurrentMapId();
        const SpawnMemory::Stats sm = SpawnMemory::GetStats(mid);
        const bool useful = SpawnMemory::HasUsefulData(mid);
        ImGui::TextColored(useful ? ImVec4(0.4f, 1, 0.4f, 1)
                                  : ImVec4(0.6f, 0.6f, 0.6f, 1),
            "spawn memory: %d buckets, %d observations, %d novel%s",
            sm.buckets, sm.observations, sm.novelBuckets,
            useful ? " (steering exploration)" : " (still learning)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear map"))
            SpawnMemory::ClearMap(mid);
    }
    // Phase 2b dynamic-zone readout (Map-Wide only).
    if (settings.zoneMode == AutoHuntZoneMode::MapWide) {
        if (IsDynamicZoneActive(settings)) {
            const DWORD unprodMs = GetTickCount() - m_dynProductiveTick;
            ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1),
                "dyn-zone: %d cells  mobsInZone=%d  kills=%d  sinceKill=%.1fs  %s",
                (int)m_dynCells.size(), CountMonstersInDynamicZone(settings),
                hero ? hero->GetGameKillCount() : 0, unprodMs / 1000.0f,
                m_dynHasArrived ? "(arrived)" : "(traveling — relocate clock paused)");
        } else {
            ImGui::TextDisabled("dyn-zone: seeding...");
        }
    }
    // Session 15 [KILL-SIGNAL RE]: dump the game's own indexed stat table so we
    // can find a client-side kill-counter index (GetValue(1)=HP is the only one
    // read so far). Stand in-game, note your kill count, click, then match it to
    // an index in the log ([statdump]); kill a few and re-dump to confirm the
    // index that ticks up. Read-only via the same native accessor used for HP.
    if (ImGui::Button("Dump Stat Table (find kill counter)")) {
        if (hero && hero->m_pStatTable) {
            spdlog::info("[statdump] CStatTable::GetValue(0..63):");
            for (int i = 0; i < 64; ++i)
                spdlog::info("[statdump]   [{}] = {}", i, hero->m_pStatTable->GetValue(i));
        } else {
            spdlog::warn("[statdump] no hero / stat table available");
        }
    }
    // Session 15 [KILL-SIGNAL RE]: raw byte-dump — GetValue() returns 0 on v1074,
    // so read the real memory. Dumps (1) a wide CHero int window around the stat
    // pointer (HP/kills may be a direct CHero field), and (2) the objects the
    // stat pointer at +0x968 and the v1074-shifted +0x9B8 point to, as int32s.
    // Note your HP and kill count, click, then match them in the [statbytes] log.
    if (ImGui::Button("Dump Stat BYTES (find HP/kills offset)")) {
        if (hero) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(hero);
            spdlog::info("[statbytes] === hero=0x{:X} : CHero ints +0x800..+0xD00 (kills are at +0xA30) ===", base);
            DumpInts("CHero", base + 0x800, 320);   // 0x500 bytes as int32s (widened to hunt HP)
            uintptr_t p968 = 0, p9B8 = 0;
            SafeReadBlock(base + 0x968, &p968, sizeof(p968));
            SafeReadBlock(base + 0x9B8, &p9B8, sizeof(p9B8));
            spdlog::info("[statbytes] statPtr@+0x968=0x{:X}  statPtr@+0x9B8=0x{:X}", p968, p9B8);
            if (p968 > 0x10000)
                DumpInts("*statPtr(0x968)", p968, 64);   // 0x100 bytes as int32s
            if (p9B8 > 0x10000 && p9B8 != p968)
                DumpInts("*statPtr(0x9B8)", p9B8, 64);
        } else {
            spdlog::warn("[statbytes] no hero available");
        }
    }
    {
        // Ground-item scan (session 10): map->m_vecItems was NEVER
        // populated on v1074, so loot/potion pickup silently found
        // nothing all project. This is its replacement (map_items.h) —
        // if this reads 0 while items are visibly on the ground, the
        // heap-scan signature itself needs revisiting, same as the
        // entity funnel counters did for monster detection.
        const MapItems::Stats ms2 = MapItems::GetStats();
        ImGui::TextColored(ms2.total > 0 ? ImVec4(0.4f, 1, 0.4f, 1)
                                         : ImVec4(1, 0.6f, 0.2f, 1),
            "ground items: %d (scan #%u, %ums)",
            ms2.total, ms2.scans, ms2.lastScanMs);
        ImGui::SameLine();
        // Session 14 [PLUS RE]: one-off tool for finding the real ground-item
        // plus offset — dumps raw bytes for the item nearest the hero (stand
        // right on/next to a known +0 or +1 item, click, repeat for the
        // other). Output goes to coclassic.log at info level, tagged
        // "[hunt-loot] Dump ...". See hunt_loot.h's DebugDumpNearestGroundItem
        // for what's captured and why.
        if (ImGui::SmallButton("Dump nearest (plus RE)"))
            DebugDumpNearestGroundItem(hero);
    }

    ImGui::Separator();
    // m_deqItem (+0xB70) is unverified and reads empty, which is why the
    // repair sequence never re-equips.
    if (hero) {
        const size_t bag = hero->m_deqItem.size();
        if (bag == 0)
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                "Inventory reads 0 items -> repair can never re-equip");
        else
            ImGui::Text("Inventory: %d items", (int)bag);
    }
    if (ImGui::Button("Find Inventory Container")) {
        const bool ok = DebugFindInventory();
        spdlog::info("[debug] Inventory probe ok={}", ok);
    }

    // Repair sequence state. It stalls somewhere between unequip and
    // re-equip; this shows exactly where, and the durability values it is
    // comparing.
    {
        ImGui::Separator();
        const HuntTownService& ts = GetTownService();
        const OBJID rid = ts.GetRepairItemId();
        ImGui::Text("Repair phase: %s",
                    HuntTownService::RepairPhaseName(ts.GetRepairPhase()));
        ImGui::TextDisabled("  repairItemId=%u slot=%d npcId=%u",
                            rid, ts.GetRepairSlot(), ts.GetRepairNpcId());
        if (rid && hero) {
            // WaitRepair only advances when cur >= max, so these two
            // numbers say whether the NPC repair actually took effect.
            CItem* bag = FindInventoryItemById(hero, rid);
            CItem* eq  = hero->GetEquip(ts.GetRepairSlot());
            if (bag)
                ImGui::TextDisabled("  in bag: dur %d / %d",
                    bag->GetDurabilityRaw(), bag->GetMaxDurabilityRaw());
            else
                ImGui::TextDisabled("  in bag: NOT FOUND");
            ImGui::TextDisabled("  in slot: %s",
                eq ? (eq->GetID() == rid ? "yes (re-equipped)" : "different item")
                   : "empty");
        }
    }

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

    RenderProfileBar(ProfileKind::Hunt);
    ImGui::Separator();

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
