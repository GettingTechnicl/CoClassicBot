#include "pathfinder.h"
#include "game.h"
#include "gateway.h"
#include "log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr DWORD kPathStallRecoverMs = 900;
constexpr DWORD kPathHardStuckMs = 3000;
constexpr DWORD kPathRetryCommandGapMs = 350;
constexpr DWORD kPathPredictedMoveRecoverMs = 250;
constexpr DWORD kPathStaleMovementCommandMs = 1800;
constexpr DWORD kMinPathMovementIntervalMs = 100;
constexpr DWORD kMaxPathMovementIntervalMs = 5000;

bool IsMovementCommand(const CCommand& cmd)
{
    return cmd.iType == _COMMAND_WALK
        || cmd.iType == _COMMAND_RUN
        || cmd.iType == _COMMAND_WALKFORWARD
        || cmd.iType == _COMMAND_RUNFORWARD
        || cmd.iType == _COMMAND_JUMP;
}

bool IsMovementCommandStillAdvancing(const CHero* hero)
{
    if (!hero)
        return false;

    const CCommand& cmd = hero->GetCommand();
    if (!IsMovementCommand(cmd))
        return false;

    return cmd.posTarget.x != hero->m_posMap.x || cmd.posTarget.y != hero->m_posMap.y;
}

DWORD ClampPathMovementIntervalMs(DWORD value)
{
    if (value == 0)
        return 0;
    return std::clamp(value, kMinPathMovementIntervalMs, kMaxPathMovementIntervalMs);
}
}

bool IsTileOccupied(int tileX, int tileY)
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr)
        return false;
    // Session 11: Entities::Roles() internally does a fresh Entities::Get()
    // (full cache-check + vector copy) on EVERY call — this used to call it
    // up to 1002 times per invocation (twice for the guard, twice per loop
    // iteration for up to 500 iterations). One Get() call instead. This is
    // one of the hottest call sites in the whole plugin system (every
    // pathfinding decision, every minimap click), so this alone likely
    // accounts for a large share of the redundant allocation churn found
    // repeated across ~15 call sites this same way project-wide.
    const std::vector<CRole*> roles = Entities::Get();
    if (roles.empty() || roles.size() >= 10000)
        return false;
    for (size_t i = 0; i < roles.size() && i < 500; i++) {
        CRole* e = roles[i];
        // Session 11 [CRASH FIX]: same hazard class as the loot-scanning
        // functions fixed elsewhere this session — a raw pointer into the
        // game's own heap, freeable at any moment between the scan
        // snapshot and this loop touching it.
        if (!e || !Entities::IsAlive(e))
            continue;
        // Session 13: monsters do NOT block movement in this game (confirmed
        // with the game's own live behavior) — the hero can walk onto,
        // through, and stack with monster tiles, which is exactly how melee
        // combat works at all. Counting them as blockers was the root cause
        // of a live-reported bug where, in a dense pack of 20-40 monsters,
        // every candidate tile adjacent to a chosen target was occupied by
        // some OTHER monster, so no combat-approach position could ever be
        // found — the bot would endlessly re-path around the same pack in
        // "explore" mode without engaging anything, regardless of leash or
        // speedhack. Guards/patrols are excluded from IsMonster() and still
        // count as blockers; so do players and NPCs. IsTileNearMonster()
        // (the separate mob-AVOIDANCE primitive) is unaffected — that one
        // deliberately DOES want to find monsters.
        if (e->IsMonster())
            continue;
        if (e->m_posMap.x == tileX && e->m_posMap.y == tileY)
            return true;
    }
    return false;
}

Pathfinder& Pathfinder::Get()
{
    static Pathfinder instance;
    return instance;
}

void Pathfinder::LoadTilePath(const std::vector<Position>& tilePath, int hx, int hy)
{
    m_waypoints.clear();
    m_waypoints.reserve(tilePath.size());
    for (const Position& pos : tilePath) {
        if (pos.x == hx && pos.y == hy)
            continue;
        m_waypoints.push_back(pos);
    }
    m_index = 0;
}

static bool IsTileNearMonster(int tileX, int tileY, int radius)
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr)
        return false;
    const std::vector<CRole*> roles = Entities::Get();
    if (roles.empty() || roles.size() >= 10000)
        return false;
    for (size_t i = 0; i < roles.size() && i < 500; i++) {
        CRole* e = roles[i];
        if (!e || !Entities::IsAlive(e)) continue;
        if (!e->IsMonster()) continue;
        if (CGameMap::TileDist(tileX, tileY, e->m_posMap.x, e->m_posMap.y) < radius)
            return true;
    }
    return false;
}

Position Pathfinder::FindSafeAlternative(CHero* hero, CGameMap* map, const Position& target)
{
    const int hx = hero->m_posMap.x;
    const int hy = hero->m_posMap.y;

    // Search nearby tiles around the target for one that's far from monsters
    Position best = target;
    int bestMinMobDist = 0;
    float bestTargetDist = 0.0f;
    bool found = false;
    constexpr int kSearchRadius = 4;

    // Session 11: was calling Entities::Roles() (a fresh Entities::Get() -
    // full cache-check + vector copy - every single call) up to ~1002 times
    // PER CANDIDATE TILE below, for up to (2*kSearchRadius+1)^2 = 81
    // candidates - up to ~81,000 redundant calls in one FindSafeAlternative
    // invocation. One Get() call for the whole function instead; monster
    // positions don't change mid-search.
    const CRoleMgr* mgr = Game::GetRoleMgr();
    const std::vector<CRole*> roles = mgr ? Entities::Get() : std::vector<CRole*>{};

    for (int dx = -kSearchRadius; dx <= kSearchRadius; ++dx) {
        for (int dy = -kSearchRadius; dy <= kSearchRadius; ++dy) {
            const Position candidate = {target.x + dx, target.y + dy};
            if (!map->IsWalkable(candidate.x, candidate.y))
                continue;
            if (IsTileOccupied(candidate.x, candidate.y))
                continue;
            const int heroToDist = CGameMap::TileDist(hx, hy, candidate.x, candidate.y);
            if (heroToDist > CGameMap::MAX_JUMP_DIST)
                continue;
            if (heroToDist > 2 && !map->CanJump(hx, hy, candidate.x, candidate.y, CGameMap::GetHeroAltThreshold()))
                continue;
            if (heroToDist <= 2 && !map->CanReach(hx, hy, candidate.x, candidate.y))
                continue;

            // Compute min distance to any monster
            int minMobDist = 999;
            if (!roles.empty() && roles.size() < 10000) {
                for (size_t i = 0; i < roles.size() && i < 500; i++) {
                    CRole* e = roles[i];
                    if (!e || !Entities::IsAlive(e)) continue;
                    if (!e->IsMonster()) continue;
                    int d = CGameMap::TileDist(candidate.x, candidate.y, e->m_posMap.x, e->m_posMap.y);
                    if (d < minMobDist) minMobDist = d;
                }
            }

            const float targetDist = candidate.DistanceTo(target);
            // Prefer: farthest from mobs, then closest to original target
            if (!found || minMobDist > bestMinMobDist
                || (minMobDist == bestMinMobDist && targetDist < bestTargetDist)) {
                found = true;
                best = candidate;
                bestMinMobDist = minMobDist;
                bestTargetDist = targetDist;
            }
        }
    }

    return best;
}

bool Pathfinder::IssueMovementToWaypoint(CHero* hero, CGameMap* map, const Position& target)
{
    if (!hero || !map)
        return false;

    const int hx = hero->m_posMap.x;
    const int hy = hero->m_posMap.y;

    // If avoiding mobs, check if target is near a monster and find a safer tile
    Position effective = target;
    if (m_avoidMobs && m_avoidMobRadius > 0
        && IsTileNearMonster(target.x, target.y, m_avoidMobRadius)) {
        effective = FindSafeAlternative(hero, map, target);
        if (effective.x != target.x || effective.y != target.y)
            spdlog::debug("[path] Avoiding mob: ({},{}) -> ({},{})", target.x, target.y, effective.x, effective.y);
    }

    int dist = CGameMap::TileDist(hx, hy, effective.x, effective.y);
    if (dist <= 0)
        return false;

    // Walk-only locations (the Market now; cities later — see
    // ShouldWalkOnlyOnMap): the character must WALK this leg, never jump. Walk
    // animates over long distances too (CHero::Walk), so a whole leg to a
    // simplified waypoint is issued as one animated walk. Skips the jump-only
    // machinery below (distance cap, the jump branch).
    const bool walkOnly = ShouldWalkOnlyOnMap(Game::GetCurrentMapId());
    if (walkOnly) {
        if (!map->CanReach(hx, hy, effective.x, effective.y))
            return false;
        if (IsTileOccupied(effective.x, effective.y))
            return false;
        const Position beforeWalk = hero->m_posMap;
        spdlog::debug("[path] Walk-only ({},{}) -> ({},{}) dist={}", hx, hy, effective.x, effective.y, dist);
        hero->Walk(effective.x, effective.y);
        m_lastIssuedTarget = effective;
        m_lastIssuedMoveWasImmediate = hero->m_posMap.x != beforeWalk.x || hero->m_posMap.y != beforeWalk.y;
        m_lastJumpTick = GetTickCount();
        return true;
    }

    // Session 13 [JUMP VARIANCE]: shrink an over-long waypoint jump toward a
    // walkable intermediate point when the caller supplied a cap (hunt
    // plugins do, when a player is nearby — see ApplyJumpDistanceCap). This
    // waypoint is re-targeted on the NEXT Update() tick since dist stays > 0
    // after landing short, so a long leg becomes several medium jumps toward
    // the same real waypoint instead of one maximal one. Skipped for the
    // walk-range case just below (dist <= 2 is already short and not part of
    // what reads as suspicious).
    if (dist > 2 && m_jumpDistanceCapProvider) {
        const int cap = m_jumpDistanceCapProvider();
        if (cap > 0 && cap < dist) {
            const float dx = (float)(effective.x - hx);
            const float dy = (float)(effective.y - hy);
            const float len = sqrtf(dx * dx + dy * dy);
            for (int backoff = 0; backoff <= 3 && len >= 1.0f; ++backoff) {
                const float scale = (float)(cap - backoff) / len;
                if (scale <= 0.0f)
                    break;
                const Position candidate = { hx + (int)std::lround(dx * scale), hy + (int)std::lround(dy * scale) };
                if (candidate.x == hx && candidate.y == hy)
                    continue;
                if (!map->IsWalkable(candidate.x, candidate.y))
                    continue;
                if (!map->CanJump(hx, hy, candidate.x, candidate.y, CGameMap::GetHeroAltThreshold()))
                    continue;
                effective = candidate;
                dist = CGameMap::TileDist(hx, hy, effective.x, effective.y);
                break;
            }
        }
    }

    const Position before = hero->m_posMap;

    if (dist <= 2 && map->CanReach(hx, hy, effective.x, effective.y)) {
        // Only check occupied for walk-range moves — entities block adjacent movement.
        if (IsTileOccupied(effective.x, effective.y))
            return false;
        spdlog::debug("[path] Walk ({},{}) -> ({},{}) dist={}", hx, hy, effective.x, effective.y, dist);
        hero->Walk(effective.x, effective.y);
        m_lastIssuedTarget = effective;
        m_lastIssuedMoveWasImmediate = hero->m_posMap.x != before.x || hero->m_posMap.y != before.y;
        m_lastJumpTick = GetTickCount();
        return true;
    }

    if (dist <= CGameMap::MAX_JUMP_DIST && map->CanJump(hx, hy, effective.x, effective.y, CGameMap::GetHeroAltThreshold())) {
        spdlog::debug("[path] Jump ({},{}) -> ({},{}) dist={}", hx, hy, effective.x, effective.y, dist);
        // Session 9 [SAFETY FIX]: this used to call the native CHero_Jump RVA
        // directly, bypassing CHero::Jump()'s own VERIFIED_V1074 gate entirely
        // — found via an audit prompted by SetCommand's own vtable slot
        // turning out to be stale. GameRva::CHERO_JUMP has never been
        // independently verified for v1074, and m_forceNativeJump IS reached
        // in practice (TravelPlugin sets it for Portal-type gateways), so
        // this was a real, live crash risk, not dead code. Route through the
        // safe path instead — CHero::Jump() already respects the gate itself.
        if (m_forceNativeJump && GameRva::VERIFIED_V1074)
            GameCall::CHero_Jump()(hero, effective.x, effective.y);
        else
            hero->Jump(effective.x, effective.y);
        m_lastIssuedTarget = effective;
        m_lastIssuedMoveWasImmediate = hero->m_posMap.x != before.x || hero->m_posMap.y != before.y;
        m_lastJumpTick = GetTickCount();
        return true;
    }

    return false;
}

bool Pathfinder::CanIssueMovementCommand(DWORD now) const
{
    if (m_lastJumpTick == 0)
        return true;
    // Re-queried fresh every check rather than a value frozen at StartPath()
    // time — see the comment on the provider field in pathfinder.h.
    const DWORD intervalMs = m_movementIntervalProvider
        ? ClampPathMovementIntervalMs(m_movementIntervalProvider())
        : 0;
    return intervalMs == 0 || now - m_lastJumpTick >= intervalMs;
}

bool Pathfinder::RepathFrom(CHero* hero, CGameMap* map, const Position& finalDest, bool issueImmediate)
{
    if (!hero || !map)
        return false;

    m_finalDestination = finalDest;

    // Reset progress tracking so the stuck timer starts fresh after a repath,
    // preventing immediate re-trigger of STUCK detection on the next frame.
    const DWORD now = GetTickCount();
    m_lastProgressPos = hero->m_posMap;
    m_lastProgressTick = now;

    const int hx = hero->m_posMap.x;
    const int hy = hero->m_posMap.y;
    auto tilePath = map->FindPath(hx, hy, finalDest.x, finalDest.y, 1000000);
    if (tilePath.empty())
        return false;

    auto waypoints = map->SimplifyPath(tilePath);
    if (!waypoints.empty()) {
        m_waypoints = waypoints;
        m_index = 0;
        if (!issueImmediate || IssueMovementToWaypoint(hero, map, m_waypoints[0]))
            return true;
    }

    LoadTilePath(tilePath, hx, hy);
    if (m_waypoints.empty())
        return false;

    return !issueImmediate || IssueMovementToWaypoint(hero, map, m_waypoints[0]);
}

void Pathfinder::StartPath(const std::vector<Position>& waypoints, std::function<DWORD()> movementIntervalProvider,
    std::function<int()> jumpDistanceCapProvider)
{
    if (waypoints.empty())
        return;

    m_waypoints = waypoints;
    m_index = 0;
    m_active = true;
    m_forceNativeJump = false;
    m_finalDestination = waypoints.back();
    m_movementIntervalProvider = std::move(movementIntervalProvider);
    m_jumpDistanceCapProvider = std::move(jumpDistanceCapProvider);
    m_generation++;
    spdlog::info("[path] StartPath: {} waypoints, gen={}", waypoints.size(), m_generation);

    CHero* hero = Game::GetHero();
    CGameMap* map = Game::GetMap();
    if (hero && map) {
        const DWORD now = GetTickCount();
        m_lastProgressPos = hero->m_posMap;
        m_lastProgressTick = now;
        const Position first = m_waypoints[0];
        if (!IssueMovementToWaypoint(hero, map, first)) {
            if (!RepathFrom(hero, map, m_finalDestination, true))
                m_active = false;
        }
    } else {
        m_lastProgressPos = {};
        m_lastProgressTick = 0;
    }
}

void Pathfinder::Stop()
{
    m_active = false;
    m_lastProgressPos = {};
    m_lastIssuedTarget = {};
    m_finalDestination = {};
    m_lastIssuedMoveWasImmediate = false;
    m_movementIntervalProvider = nullptr;
    m_lastProgressTick = 0;
    m_generation++;
    spdlog::debug("[path] Stop, gen={}", m_generation);
}

void Pathfinder::Update()
{
    if (!m_active || m_index >= m_waypoints.size())
        return;

    CHero* hero = Game::GetHero();
    CGameMap* map = Game::GetMap();
    if (!hero || !map)
        return;

    const Position wp = m_waypoints[m_index];
    const int hx = hero->m_posMap.x;
    const int hy = hero->m_posMap.y;
    const DWORD now = GetTickCount();
    if (m_lastProgressTick == 0) {
        m_lastProgressPos = hero->m_posMap;
        m_lastProgressTick = now;
    } else if (hx != m_lastProgressPos.x || hy != m_lastProgressPos.y) {
        m_lastProgressPos = hero->m_posMap;
        m_lastProgressTick = now;
    }
    const int dist = CGameMap::TileDist(hx, hy, wp.x, wp.y);
    const bool canIssueMovementNow = CanIssueMovementCommand(now);

    if (dist == 0) {
        ++m_index;
        if (m_index < m_waypoints.size()) {
            const Position next = m_waypoints[m_index];
            if (canIssueMovementNow && !IssueMovementToWaypoint(hero, map, next)) {
                spdlog::warn("[path] Move failed ({},{}) -> ({},{}), re-pathfinding to ({},{})",
                    hx, hy, next.x, next.y, m_finalDestination.x, m_finalDestination.y);
                if (!RepathFrom(hero, map, m_finalDestination, true)) {
                    spdlog::error("[path] Re-route failed, stopping");
                    m_active = false;
                }
            }
        } else {
            spdlog::info("[path] Reached final waypoint at ({},{})", hx, hy);
            m_active = false;
        }
        return;
    }

    if (dist == 1) {
        if (m_index + 1 >= m_waypoints.size()) {
            spdlog::debug("[path] Near final waypoint at ({},{}) -> ({},{}), issuing final adjustment",
                hx, hy, wp.x, wp.y);
            if (canIssueMovementNow
                && !IssueMovementToWaypoint(hero, map, wp)
                && !RepathFrom(hero, map, m_finalDestination, true)) {
                spdlog::error("[path] Final adjustment failed, stopping");
                m_active = false;
            }
        } else {
            // Session 10 [MEMORY LEAK INVESTIGATION]: this used to
            // unconditionally call RepathFrom() here — a full A* re-solve of
            // the ENTIRE remaining route, up to 1,000,000 nodes — on every
            // intermediate waypoint landing that was merely 1 tile off,
            // even though the landing itself succeeded. That fires once per
            // real jump (not once per decision-loop poll), which matches
            // the evidence already gathered live for the project's
            // confirmed memory leak far better than the decision-loop
            // theory did (see coclassicbot-live-offsets memory: throttling
            // the decision loop 150x didn't reduce growth; slowing real
            // actions did). Mirror the final-waypoint branch above instead
            // of the branch below: try to just continue toward the current
            // waypoint first, only falling back to a full repath if that
            // genuinely fails.
            spdlog::debug("[path] Landed near waypoint at ({},{}) for ({},{}), adjusting toward it",
                hx, hy, wp.x, wp.y);
            if (canIssueMovementNow
                && !IssueMovementToWaypoint(hero, map, wp)
                && !RepathFrom(hero, map, m_finalDestination, canIssueMovementNow)) {
                spdlog::error("[path] Re-route failed after near landing, stopping");
                m_active = false;
            }
        }
        return;
    }

    const DWORD stalledFor = now - m_lastProgressTick;
    const DWORD commandAge = m_lastJumpTick != 0 ? now - m_lastJumpTick : 0;
    const bool movementStillAdvancing = hero->IsJumping() || IsMovementCommandStillAdvancing(hero);
    if (!movementStillAdvancing && canIssueMovementNow && IssueMovementToWaypoint(hero, map, wp))
        return;

    const bool hasIssuedTarget = m_lastIssuedTarget.x != 0 || m_lastIssuedTarget.y != 0;
    const int issuedTargetDist = hasIssuedTarget
        ? CGameMap::TileDist(hx, hy, m_lastIssuedTarget.x, m_lastIssuedTarget.y)
        : 0;
    const bool predictedMoveWentStale = m_lastIssuedMoveWasImmediate
        && hasIssuedTarget
        && commandAge >= kPathPredictedMoveRecoverMs
        && issuedTargetDist > 1;
    if (predictedMoveWentStale) {
        spdlog::warn("[path] Predicted move stale at ({},{}) expected=({},{}) waypoint=({},{}) final=({},{}) age={}ms, re-pathfinding",
            hx, hy,
            m_lastIssuedTarget.x, m_lastIssuedTarget.y,
            wp.x, wp.y,
            m_finalDestination.x, m_finalDestination.y,
            commandAge);
        if (!RepathFrom(hero, map, m_finalDestination, canIssueMovementNow)) {
            spdlog::error("[path] Re-route failed after stale predicted move, stopping");
            m_active = false;
        }
        return;
    }

    const bool staleMovementCommand = movementStillAdvancing
        && commandAge >= kPathStaleMovementCommandMs
        && stalledFor >= kPathStaleMovementCommandMs;
    const bool allowStallRecovery = !movementStillAdvancing || staleMovementCommand;
    if (allowStallRecovery && stalledFor >= kPathHardStuckMs) {
        CellInfo* hCell = map->GetCell(hx, hy);
        CellInfo* wCell = map->GetCell(wp.x, wp.y);
        spdlog::warn("[path] STUCK timeout at ({},{}) target=({},{}) dist={} | hero(alt={},walk={}) wp(alt={},walk={}) canJump={} canReach={}",
            hx, hy, wp.x, wp.y, dist,
            CGameMap::GetAltitude(hCell), map->IsWalkable(hx, hy),
            CGameMap::GetAltitude(wCell), map->IsWalkable(wp.x, wp.y),
            map->CanJump(hx, hy, wp.x, wp.y, CGameMap::GetHeroAltThreshold()),
            map->CanReach(hx, hy, wp.x, wp.y));

        if (!RepathFrom(hero, map, m_finalDestination, canIssueMovementNow))
            m_active = false;
        return;
    }

    if (allowStallRecovery
        && stalledFor >= kPathStallRecoverMs
        && canIssueMovementNow
        && (m_lastJumpTick == 0 || now - m_lastJumpTick >= kPathRetryCommandGapMs)) {
        spdlog::warn("[path] No progress at ({},{}) target=({},{}) dist={} stalledFor={}ms, retrying",
            hx, hy, wp.x, wp.y, dist, stalledFor);
        if (!IssueMovementToWaypoint(hero, map, wp)) {
            spdlog::warn("[path] Retry failed ({},{}) -> ({},{}), re-pathfinding to ({},{})",
                hx, hy, wp.x, wp.y, m_finalDestination.x, m_finalDestination.y);
            if (!RepathFrom(hero, map, m_finalDestination, true)) {
                spdlog::error("[path] Re-route failed after stall, stopping");
                m_active = false;
            }
        }
    }
}
