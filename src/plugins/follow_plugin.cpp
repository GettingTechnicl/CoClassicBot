#include "follow_plugin.h"
#include "game.h"
#include "hunt_intervals.h"
#include "hunt_settings.h"
#include "log.h"
#include "imgui.h"

static FollowSettings g_followSettings;
FollowSettings& GetFollowSettings() { return g_followSettings; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

CRole* FollowPlugin::FindTarget() const
{
    const FollowSettings& s = g_followSettings;
    if (!s.targetName[0])
        return nullptr;

    CHero* hero = Game::GetHero();
    const Position anchor = hero ? hero->m_posMap : Position{};
    return FindNearestRole(anchor, 0, [&](CRole* role) {
        return role->IsPlayer() && _stricmp(role->GetName(), s.targetName) == 0;
    });
}

int FollowPlugin::NearestMobDistance() const
{
    CHero* hero = Game::GetHero();
    if (!hero) return 999;

    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr)
        return 999;
    const std::vector<CRole*> roles = Entities::Get();
    if (roles.empty() || roles.size() >= 10000)
        return 999;

    int best = 999;
    for (size_t i = 0; i < roles.size() && i < 500; ++i) {
        CRole* e = roles[i];
        if (!e || !Entities::IsAlive(e)) continue;
        if (!e->IsMonster() || e->IsDead()) continue;
        int d = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
                                   e->m_posMap.x, e->m_posMap.y);
        if (d < best) best = d;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Mob positions collector (shared by dodge and follow)
// ---------------------------------------------------------------------------

struct MobPos { int x, y; };

static int CollectMobs(MobPos* out, int maxCount)
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr) return 0;
    int count = 0;
    const std::vector<CRole*> roles = Entities::Get();
    for (size_t i = 0; i < roles.size() && i < 500; ++i) {
        CRole* e = roles[i];
        if (!e || !Entities::IsAlive(e)) continue;
        if (!e->IsMonster() || e->IsDead()) continue;
        if (count < maxCount)
            out[count++] = { e->m_posMap.x, e->m_posMap.y };
    }
    return count;
}

static int MinMobDist(int tx, int ty, const MobPos* mobs, int mobCount)
{
    int best = 999;
    for (int i = 0; i < mobCount; ++i) {
        int d = CGameMap::TileDist(tx, ty, mobs[i].x, mobs[i].y);
        if (d < best) best = d;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Pick the best jumpable tile — scored by mob avoidance + target proximity
// ---------------------------------------------------------------------------

static int AbsDiff(int a, int b) { return a > b ? a - b : b - a; }

static Position FindBestJumpTile(CHero* hero, CGameMap* map,
    const MobPos* mobs, int mobCount,
    const Position* targetPos, int followDistance)
{
    const int hx = hero->m_posMap.x;
    const int hy = hero->m_posMap.y;
    const int altThreshold = CGameMap::GetHeroAltThreshold();

    Position best = { hx, hy };
    int bestMinMob = -1;
    int bestTGap = 999999;
    int bestHDist = 999;

    for (int dx = -CGameMap::MAX_JUMP_DIST; dx <= CGameMap::MAX_JUMP_DIST; ++dx) {
        for (int dy = -CGameMap::MAX_JUMP_DIST; dy <= CGameMap::MAX_JUMP_DIST; ++dy) {
            const int cx = hx + dx;
            const int cy = hy + dy;
            const int heroDist = CGameMap::TileDist(hx, hy, cx, cy);
            if (heroDist == 0 || heroDist > CGameMap::MAX_JUMP_DIST)
                continue;
            if (!map->IsWalkable(cx, cy))
                continue;
            if (heroDist <= 2) {
                if (!map->CanReach(hx, hy, cx, cy))
                    continue;
            } else {
                if (!map->CanJump(hx, hy, cx, cy, altThreshold))
                    continue;
            }

            // Reject tiles outside follow range of target
            int tDist = targetPos
                ? CGameMap::TileDist(cx, cy, targetPos->x, targetPos->y) : 0;
            if (targetPos && tDist > followDistance)
                continue;

            int minMob = mobCount > 0 ? MinMobDist(cx, cy, mobs, mobCount) : 999;

            // Session [FOLLOW-DISTANCE FIX]: this used to break ties by minimizing tDist
            // directly, which — with no mobs nearby (the common case, so every candidate tied
            // on minMob) — always picked the tile closest to the target, i.e. right on top of
            // them, no matter what followDistance was set to. followDistance was only ever
            // enforced as a ceiling (the reject-above check), never as the actual standoff the
            // UI promises ("stop moving when within this tile distance"). Minimizing the GAP to
            // followDistance instead lands the hero right at the edge of follow range.
            int tGap = targetPos ? AbsDiff(tDist, followDistance) : 0;

            // Pick: farthest from mobs, then closest to the follow-distance boundary, then shortest jump
            if (minMob > bestMinMob
                || (minMob == bestMinMob && tGap < bestTGap)
                || (minMob == bestMinMob && tGap == bestTGap && heroDist < bestHDist)) {
                best = Position(cx, cy);
                bestMinMob = minMob;
                bestTGap = tGap;
                bestHDist = heroDist;
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Update — called each frame
// ---------------------------------------------------------------------------

void FollowPlugin::Update()
{
    const FollowSettings& s = g_followSettings;
    if (!s.enabled) {
        m_state = State::Idle;
        return;
    }

    CHero* hero = Game::GetHero();
    CGameMap* map = Game::GetMap();
    if (!hero || !map) return;
    if (hero->IsDead()) return;
    if (hero->IsJumping()) return;

    // Throttle movement commands
    const AutoHuntSettings& ah = GetAutoHuntSettings();
    const DWORD now = GetTickCount();
    const DWORD interval = ah.movementIntervalMs > 0 ? ah.movementIntervalMs : 500;
    if (m_lastJumpTick != 0 && now - m_lastJumpTick < interval)
        return;

    CRole* target = FindTarget();
    const int hx = hero->m_posMap.x;
    const int hy = hero->m_posMap.y;

    // Determine distance to target
    int targetDist = 999;
    Position targetPos = {0, 0};
    if (target) {
        targetPos = target->m_posMap;
        targetDist = CGameMap::TileDist(hx, hy, targetPos.x, targetPos.y);
    }

    // Collect nearby mobs
    MobPos mobs[128];
    int mobCount = CollectMobs(mobs, 128);
    int mobDist = mobCount > 0 ? MinMobDist(hx, hy, mobs, mobCount) : 999;

    // Decide what to do: dodge or follow
    const bool needDodge = mobDist <= s.dodgeRadius;
    const bool needFollow = target && targetDist > s.followDistance;

    if (!needDodge && !needFollow) {
        m_state = State::Idle;
        return;
    }

    // Find best tile considering both mob avoidance and target proximity
    Position dest = FindBestJumpTile(hero, map, mobs, mobCount,
        target ? &targetPos : nullptr, s.followDistance);

    if (dest.x == hx && dest.y == hy)
        return;

    m_state = needDodge ? State::Dodging : State::Following;
    hero->Jump(dest.x, dest.y);
    m_lastJumpTick = now;
    spdlog::debug("[follow] {} to ({},{}) mobDist={} targetDist={}",
        needDodge ? "Dodge" : "Follow", dest.x, dest.y, mobDist, targetDist);
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void FollowPlugin::RenderUI()
{
    FollowSettings& s = g_followSettings;
    ImGui::Checkbox("Enabled##follow", &s.enabled);

    ImGui::InputText("Target Player", s.targetName, IM_ARRAYSIZE(s.targetName));

    ImGui::SliderInt("Follow Distance", &s.followDistance, 1, 30);
    ImGui::TextDisabled("Stop moving when within this tile distance of the target.");

    ImGui::SliderInt("Dodge Radius", &s.dodgeRadius, 2, 15);
    ImGui::TextDisabled("Emergency dodge when a mob is within this tile distance (in map "
                         "coordinates, same units as Follow Distance). Floor is 2: most monsters "
                         "only hit at 1 tile (adjacent), some reach 2 -- 1 would mean dodging "
                         "only once already in melee range.");

    ImGui::Separator();

    // Status display
    const char* stateStr = "Idle";
    if (m_state == State::Following) stateStr = "Following";
    else if (m_state == State::Dodging) stateStr = "Dodging";

    CRole* target = FindTarget();
    if (s.enabled && target) {
        CHero* hero = Game::GetHero();
        int dist = hero ? CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
                                              target->m_posMap.x, target->m_posMap.y) : 0;
        ImGui::Text("Status: %s | Target: %s (%d,%d) | Distance: %d",
            stateStr, target->GetName(), target->m_posMap.x, target->m_posMap.y, dist);
    } else if (s.enabled && s.targetName[0]) {
        ImGui::Text("Status: %s | Target not found: %s", stateStr, s.targetName);
    } else {
        ImGui::Text("Status: %s", stateStr);
    }
}
