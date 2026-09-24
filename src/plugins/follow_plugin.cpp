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
        // StepDist, not TileDist: dodgeRadius is a "how many steps away" concept (see
        // follow_plugin.h) and TileDist under-counts a diagonal gap (e.g. reads a (2,2) offset
        // as 3 instead of the 2 steps it actually is on this 8-directional grid), which would
        // make a monster that's genuinely within its attack range look safely far away.
        int d = CGameMap::StepDist(hero->m_posMap.x, hero->m_posMap.y,
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
        int d = CGameMap::StepDist(tx, ty, mobs[i].x, mobs[i].y);  // steps, see NearestMobDistance's comment
        if (d < best) best = d;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Pick the best jumpable tile — scored by mob avoidance + target proximity
// ---------------------------------------------------------------------------

static int AbsDiff(int a, int b) { return a > b ? a - b : b - a; }

// `desiredDist` is where we WANT to land relative to the target (the follow band's midpoint —
// see FollowPlugin::Update), in STEPS (CGameMap::StepDist), not TileDist — see CGameMap.h.
//
// No hard "reject farther than desiredDist" filter: an earlier version rejected any candidate
// tile whose distance-to-target still exceeded the desired value, which is correct once the
// target is within reach but STRANDS the hero when the target is farther away than
// desiredDist + MAX_JUMP_DIST (18) — every reachable tile would still fail that check, so
// NOTHING would ever be eligible and the hero would never approach at all. Minimizing the GAP
// to desiredDist (no hard cutoff) handles both cases with one rule: when the target is within
// reach of landing at the desired distance, that's what gets picked; when it's still far
// outside jump range no matter where the hero lands, minimizing the gap reduces to minimizing
// raw distance, i.e. "close as much of the gap as this one jump can" — a normal chase step.
static Position FindBestJumpTile(CHero* hero, CGameMap* map,
    const MobPos* mobs, int mobCount,
    const Position* targetPos, int desiredDist)
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
            // heroDist (jump cost/validity) intentionally stays TileDist — CanReach/CanJump and
            // MAX_JUMP_DIST are all calibrated against the game's own jump-range formula, which
            // this matches (see CGameMap.h); it is not a user-facing "steps" quantity.
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

            int tDist = targetPos
                ? CGameMap::StepDist(cx, cy, targetPos->x, targetPos->y) : 0;

            int minMob = mobCount > 0 ? MinMobDist(cx, cy, mobs, mobCount) : 999;

            int tGap = targetPos ? AbsDiff(tDist, desiredDist) : 0;

            // Pick: farthest from mobs, then closest to the desired follow distance, then shortest jump
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

    // Determine distance to target, in STEPS (see CGameMap.h / follow_plugin.h) — this is the
    // user-facing "how many paces away" quantity the follow band is defined in.
    int targetDist = 999;
    Position targetPos = {0, 0};
    if (target) {
        targetPos = target->m_posMap;
        targetDist = CGameMap::StepDist(hx, hy, targetPos.x, targetPos.y);
    }

    // Collect nearby mobs
    MobPos mobs[128];
    int mobCount = CollectMobs(mobs, 128);
    int mobDist = mobCount > 0 ? MinMobDist(hx, hy, mobs, mobCount) : 999;

    // Decide what to do: dodge or follow.
    // Band with hysteresis, not a single threshold: moving only when OUTSIDE [followMin,
    // followMax] (too far OR too close — the "optionally back off" case) means the target has
    // to actually cross the band's width before the next trigger, instead of a single boundary
    // that re-fires on the target's very next step in either direction.
    const bool needDodge = mobDist <= s.dodgeRadius;
    const bool needFollow = target && (targetDist > s.followMax || targetDist < s.followMin);

    if (!needDodge && !needFollow) {
        m_state = State::Idle;
        return;
    }

    // Aim for the middle of the band: gives equal slack in both directions before the next
    // trigger, whether closing in from beyond followMax or backing off from inside followMin.
    const int desiredDist = (s.followMin + s.followMax) / 2;

    // Find best tile considering both mob avoidance and target proximity
    Position dest = FindBestJumpTile(hero, map, mobs, mobCount,
        target ? &targetPos : nullptr, desiredDist);

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

    ImGui::SliderInt("Follow Min", &s.followMin, 1, 30);
    ImGui::SliderInt("Follow Max", &s.followMax, 1, 30);
    if (s.followMin > s.followMax) s.followMax = s.followMin;   // keep the band valid as either slider moves
    ImGui::TextDisabled("Stay between Min and Max steps of the target (steps, not straight-line "
                         "tiles -- diagonal movement counts the same as cardinal, same as the "
                         "game's own grid). Only moves when outside this band, landing near the "
                         "middle -- not a single distance that re-triggers on every step.");

    ImGui::SliderInt("Dodge Radius", &s.dodgeRadius, 2, 15);
    ImGui::TextDisabled("Emergency dodge when a mob is within this many steps (same step-based "
                         "unit as Follow Min/Max). Floor is 2: most monsters only hit at 1 step "
                         "(adjacent), some reach 2 -- 1 would mean dodging only once already in "
                         "melee range.");

    ImGui::Separator();

    // Status display
    const char* stateStr = "Idle";
    if (m_state == State::Following) stateStr = "Following";
    else if (m_state == State::Dodging) stateStr = "Dodging";

    CRole* target = FindTarget();
    if (s.enabled && target) {
        CHero* hero = Game::GetHero();
        int dist = hero ? CGameMap::StepDist(hero->m_posMap.x, hero->m_posMap.y,
                                              target->m_posMap.x, target->m_posMap.y) : 0;
        ImGui::Text("Status: %s | Target: %s (%d,%d) | Distance: %d",
            stateStr, target->GetName(), target->m_posMap.x, target->m_posMap.y, dist);
    } else if (s.enabled && s.targetName[0]) {
        ImGui::Text("Status: %s | Target not found: %s", stateStr, s.targetName);
    } else {
        ImGui::Text("Status: %s", stateStr);
    }
}
