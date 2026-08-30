#include "melee_hunt_plugin.h"
#include "hunt_intervals.h"
#include "hunt_targeting.h"
#include "game.h"
#include "CHero.h"
#include "CGameMap.h"
#include "CRole.h"
#include "config.h"
#include "pathfinder.h"
#include "log.h"
#include "imgui.h"
#include <algorithm>
#include <limits>
#include <vector>

namespace {

constexpr int kReliableAttackRange = 1;
constexpr int kMinMobClumpSize = 2;

} // anonymous namespace


// ── FindBestClumpApproach ─────────────────────────────────────────────────────

bool MeleeHuntPlugin::FindBestClumpApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    const std::vector<CRole*>& targets, Position& outApproachPos,
    CRole*& outPrimaryTarget, int& outClumpSize) const
{
    outApproachPos = {};
    outPrimaryTarget = nullptr;
    outClumpSize = 0;

    if (!hero || !map || !settings.prioritizeMobClumps)
        return false;

    const int minMobClump = kMinMobClumpSize;
    if ((int)targets.size() < minMobClump)
        return false;

    const float clumpRadius = (float)(std::max)(1, settings.clumpRadius);
    const Position effectivePos = GetEffectiveHeroPosition(hero);
    const float localTargetRadius = (float)(CGameMap::MAX_JUMP_DIST + settings.clumpRadius + kReliableAttackRange);
    std::vector<CRole*> localTargets;
    localTargets.reserve(targets.size());
    for (CRole* target : targets) {
        if (target && effectivePos.DistanceTo(target->m_posMap) <= localTargetRadius)
            localTargets.push_back(target);
    }

    if ((int)localTargets.size() < minMobClump)
        return false;

    const Position jumpOrigin = GetEffectiveHeroPosition(hero);
    bool found = false;
    int bestAttackDist = (std::numeric_limits<int>::max)();
    int bestMinThreatDist = -1;
    float bestCenterDist = (std::numeric_limits<float>::max)();
    float bestMoveDist = (std::numeric_limits<float>::max)();

    for (int dx = -CGameMap::MAX_JUMP_DIST; dx <= CGameMap::MAX_JUMP_DIST; ++dx) {
        for (int dy = -CGameMap::MAX_JUMP_DIST; dy <= CGameMap::MAX_JUMP_DIST; ++dy) {
            const Position candidate = {jumpOrigin.x + dx, jumpOrigin.y + dy};
            if (candidate.x == jumpOrigin.x && candidate.y == jumpOrigin.y)
                continue;
            // Session 10: leash, not cage — clump approach — must be allowed to step outside to reach a clump.
            if (!IsPointNearHuntZone(settings, settings.zoneMapId, candidate,
                                    GetHuntLeash(settings)))
                continue;
            if (!map->IsWalkable(candidate.x, candidate.y))
                continue;
            if (IsTileOccupied(candidate.x, candidate.y))
                continue;
            if (!map->CanJump(jumpOrigin.x, jumpOrigin.y, candidate.x, candidate.y, CGameMap::GetHeroAltThreshold()))
                continue;

            int clumpSize = 0;
            long long sumX = 0;
            long long sumY = 0;
            CRole* closestAttackable = nullptr;
            int closestAttackDist = (std::numeric_limits<int>::max)();
            float closestTargetDist = (std::numeric_limits<float>::max)();
            for (CRole* target : localTargets) {
                if (!target || candidate.DistanceTo(target->m_posMap) > clumpRadius)
                    continue;

                ++clumpSize;
                sumX += target->m_posMap.x;
                sumY += target->m_posMap.y;

                const int attackDist = CGameMap::TileDist(candidate.x, candidate.y,
                    target->m_posMap.x, target->m_posMap.y);
                if (attackDist > kReliableAttackRange)
                    continue;

                const float targetDist = candidate.DistanceTo(target->m_posMap);
                if (!closestAttackable || attackDist < closestAttackDist
                    || (attackDist == closestAttackDist && targetDist < closestTargetDist)) {
                    closestAttackable = target;
                    closestAttackDist = attackDist;
                    closestTargetDist = targetDist;
                }
            }

            if (clumpSize < minMobClump || !closestAttackable)
                continue;

            const Position centroid = {
                (int)(sumX / (long long)clumpSize),
                (int)(sumY / (long long)clumpSize)
            };
            const float centerDist = candidate.DistanceTo(centroid);
            const float moveDist = effectivePos.DistanceTo(candidate);
            // Melee has no archer safety distance — minThreatDist is always INT_MAX
            constexpr int minThreatDist = (std::numeric_limits<int>::max)();
            if (!found
                || clumpSize > outClumpSize
                || (clumpSize == outClumpSize && closestAttackDist < bestAttackDist)
                || (clumpSize == outClumpSize && closestAttackDist == bestAttackDist && minThreatDist > bestMinThreatDist)
                || (clumpSize == outClumpSize && closestAttackDist == bestAttackDist && minThreatDist == bestMinThreatDist && centerDist < bestCenterDist)
                || (clumpSize == outClumpSize && closestAttackDist == bestAttackDist
                    && minThreatDist == bestMinThreatDist && centerDist == bestCenterDist && moveDist < bestMoveDist)) {
                found = true;
                outApproachPos = candidate;
                outPrimaryTarget = closestAttackable;
                outClumpSize = clumpSize;
                bestAttackDist = closestAttackDist;
                bestMinThreatDist = minThreatDist;
                bestCenterDist = centerDist;
                bestMoveDist = moveDist;
            }
        }
    }

    return found;
}


// ── FindBestMeleeTarget ───────────────────────────────────────────────────────

CRole* MeleeHuntPlugin::FindBestMeleeTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    Position* outApproachPos, int* outClumpSize) const
{
    if (outApproachPos)
        *outApproachPos = {};
    if (outClumpSize)
        *outClumpSize = 0;
    if (!hero || !map)
        return nullptr;

    // Same mid-jump blindness fix as the archer: gate mobSearchRange from the
    // effective position the clump/approach math below already uses.
    const Position effectivePos = GetEffectiveHeroPosition(hero);
    const bool hasPreferFilter = settings.monsterPreferNames[0] != '\0';
    std::vector<CRole*> targets = hasPreferFilter
        ? CollectHuntTargets(settings, true, false, &effectivePos)
        : std::vector<CRole*>{};
    if (targets.empty())
        targets = CollectHuntTargets(settings, false, false, &effectivePos);
    if (targets.empty())
        return nullptr;

    const int actionRadius = (std::max)(1, settings.actionRadius);
    const int clumpRadius = (std::max)(1, settings.clumpRadius);
    const int currentClumpSize = CountTargetsInRadius(targets, effectivePos, (float)clumpRadius);
    const int configuredClumpSize = (std::max)(kMinMobClumpSize, settings.minimumMobClump);
    Position singleTargetApproachPos;

    // 1. Clear all mobs within action radius first.
    //
    // Session 14 [MELEE TARGET COMMITMENT]: this used to re-randomize on
    // EVERY decision tick, unconditionally, whenever Cyclone OR Superman was
    // active — no floor — live-confirmed via a 3-minute video+log review:
    // "Jumping to mob clump" immediately followed by "Attacking..." on
    // nearly every state-transition pair, because a fresh random target most
    // ticks meant a fresh approach-position computation (often a real
    // reposition jump) before a single attack landed.
    //
    // Per the user (who knows the two skills' actual mechanics): Superman's
    // "Snow" is a genuine multi-target AoE hit — which specific monster is
    // nominally "selected" barely matters, since a cast near the cluster
    // hits several regardless, so per-tick randomizing is CORRECT there and
    // deliberately left untouched. Cyclone is a pure speed buff with no
    // built-in multi-target hit — for it (and the no-buff case, which was
    // already effectively single-target via FindClosestTarget) the user
    // wants the opposite: land settings.meleeMinHitsPerTarget attack
    // ATTEMPTS (not confirmed hits — melee doesn't land every swing, and
    // attempts are what's actually countable) on one target, back-to-back
    // with no artificial pause, before considering anyone else. This also
    // fixes a second-order bug: re-entering ApproachTarget state on every
    // random switch made HandleCombatAttack/ComputeNextAttackDelayMs think a
    // target switch had just happened on nearly every cycle, which falls
    // back to a slower post-switch attack interval instead of the fast
    // Cyclone one — sticking with the same target keeps that fast path live
    // for the whole commitment burst.
    CRole* actionTarget = nullptr;
    if (settings.usePacketJump && hero->IsSupermanActive()) {
        actionTarget = FindRandomTarget(targets, effectivePos, actionRadius);
        m_committedTargetId = actionTarget ? actionTarget->GetID() : 0;
        m_hitsOnCommittedTarget = 0;
    } else {
        CRole* committedTarget = nullptr;
        if (m_committedTargetId != 0
            && m_hitsOnCommittedTarget < (std::max)(1, settings.meleeMinHitsPerTarget)) {
            for (CRole* candidate : targets) {
                if (candidate && candidate->GetID() == m_committedTargetId
                    && CGameMap::TileDist(effectivePos.x, effectivePos.y,
                           candidate->m_posMap.x, candidate->m_posMap.y) <= actionRadius) {
                    committedTarget = candidate;
                    break;
                }
            }
        }
        actionTarget = committedTarget ? committedTarget : FindClosestTarget(targets, effectivePos, actionRadius);
        if (actionTarget && actionTarget->GetID() != m_committedTargetId) {
            m_committedTargetId = actionTarget->GetID();
            m_hitsOnCommittedTarget = 0;
        }
    }
    if (actionTarget) {
        if (FindBestMeleeApproachPos(hero, map, settings, actionTarget, targets, effectivePos, singleTargetApproachPos, kReliableAttackRange)
            && outApproachPos) {
            *outApproachPos = singleTargetApproachPos;
        }
        if (outClumpSize)
            *outClumpSize = currentClumpSize;
        return actionTarget;
    }

    // 2. Already inside a large enough local clump — clear it.
    if (currentClumpSize >= configuredClumpSize) {
        if (CRole* localAoeTarget = FindClosestTarget(targets, effectivePos, clumpRadius)) {
            if (FindBestMeleeApproachPos(hero, map, settings, localAoeTarget, targets, effectivePos, singleTargetApproachPos, kReliableAttackRange)
                && outApproachPos) {
                *outApproachPos = singleTargetApproachPos;
            }
            if (outClumpSize)
                *outClumpSize = currentClumpSize;
            return localAoeTarget;
        }
    }

    // 3. No nearby mobs — jump to the best distant clump (single-jump range).
    Position clumpApproachPos;
    CRole* clumpTarget = nullptr;
    int clumpSize = 0;
    if (settings.prioritizeMobClumps
        && FindBestClumpApproach(hero, map, settings, targets, clumpApproachPos, clumpTarget, clumpSize)
        && clumpSize > currentClumpSize) {
        if (outApproachPos)
            *outApproachPos = clumpApproachPos;
        if (outClumpSize)
            *outClumpSize = clumpSize;
        return clumpTarget;
    }

    // 4. Best clump beyond jump range — pathfind toward it.
    if (settings.prioritizeMobClumps) {
        int bestClusterSize = 0;
        if (CRole* bestCluster = FindBestClusterTarget(targets, effectivePos,
                (float)(std::max)(1, settings.clumpRadius), &bestClusterSize);
            bestCluster && bestClusterSize >= configuredClumpSize) {
            if (outApproachPos)
                *outApproachPos = bestCluster->m_posMap;
            if (outClumpSize)
                *outClumpSize = bestClusterSize;
            return bestCluster;
        }
    }

    // 5. Fallback: closest target anywhere.
    CRole* closest = FindClosestTarget(targets, effectivePos);
    if (!closest)
        return nullptr;

    if (FindBestMeleeApproachPos(hero, map, settings, closest, targets, effectivePos, singleTargetApproachPos, kReliableAttackRange)
        && outApproachPos) {
        *outApproachPos = singleTargetApproachPos;
    }
    if (outClumpSize)
        *outClumpSize = currentClumpSize;

    return closest;
}


// ── FindBestMeleeApproachPos ──────────────────────────────────────────────────

bool MeleeHuntPlugin::FindBestMeleeApproachPos(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    CRole* target, const std::vector<CRole*>& allTargets, const Position& heroPos,
    Position& outApproachPos, int attackRange) const
{
    outApproachPos = {};
    if (!hero || !map || !target || attackRange <= 0)
        return false;

    const Position jumpOrigin = heroPos;
    if (CGameMap::TileDist(jumpOrigin.x, jumpOrigin.y, target->m_posMap.x, target->m_posMap.y) <= attackRange)
        return false;

    bool found = false;
    int bestDensity = -1;
    float bestMoveDist = (std::numeric_limits<float>::max)();
    const float densityRadius = (float)(std::max)(1, settings.clumpRadius);

    for (int dx = -attackRange; dx <= attackRange; ++dx) {
        for (int dy = -attackRange; dy <= attackRange; ++dy) {
            // Session 14 [MELEE APPROACH FIX]: never the target's own tile —
            // see this function's header comment (melee_hunt_plugin.h) for
            // why the shared FindBestSingleTargetApproach can't be reused
            // as-is for melee.
            if (dx == 0 && dy == 0)
                continue;
            const Position candidate = {target->m_posMap.x + dx, target->m_posMap.y + dy};
            const int attackDist = CGameMap::TileDist(candidate.x, candidate.y, target->m_posMap.x, target->m_posMap.y);
            if (attackDist > attackRange)
                continue;
            if (!IsPointInHuntZone(settings, settings.zoneMapId, candidate))
                continue;
            if (!map->IsWalkable(candidate.x, candidate.y))
                continue;
            if ((candidate.x != jumpOrigin.x || candidate.y != jumpOrigin.y) && IsTileOccupied(candidate.x, candidate.y))
                continue;
            if (!map->CanJump(jumpOrigin.x, jumpOrigin.y, candidate.x, candidate.y, CGameMap::GetHeroAltThreshold()))
                continue;

            // Prefer the side of the target with the most OTHER monsters
            // nearby — positions the hero toward the density of the pack
            // instead of an arbitrary adjacent tile.
            const int density = CountTargetsInRadius(allTargets, candidate, densityRadius);
            const float moveDist = jumpOrigin.DistanceTo(candidate);
            if (!found
                || density > bestDensity
                || (density == bestDensity && moveDist < bestMoveDist)) {
                found = true;
                outApproachPos = candidate;
                bestDensity = density;
                bestMoveDist = moveDist;
            }
        }
    }

    return found;
}


// ── NoteMeleeAttackAttempt ─────────────────────────────────────────────────────

void MeleeHuntPlugin::NoteMeleeAttackAttempt(OBJID targetId)
{
    if (targetId != m_committedTargetId) {
        // Out of sync with FindBestMeleeTarget's own commitment tracking
        // (shouldn't normally happen — resync rather than silently
        // under/over-counting against the wrong target).
        m_committedTargetId = targetId;
        m_hitsOnCommittedTarget = 0;
    }
    if (m_hitsOnCommittedTarget < (std::numeric_limits<int>::max)())
        ++m_hitsOnCommittedTarget;
}


// ── ComputeMeleeAttackDelayMs ───────────────────────────────────────────────────

DWORD MeleeHuntPlugin::ComputeMeleeAttackDelayMs(CHero* hero, CRole* target, const AutoHuntSettings& settings) const
{
    const bool targetChanged = (m_targetId != target->GetID());
    const bool justFinishedApproach = (m_state == AutoHuntState::ApproachTarget);
    const DWORD attackInterval = hero->IsCycloneActive()
        ? GetCycloneAttackIntervalMsNoJitter(settings)
        : GetAttackIntervalMsNoJitter(settings);
    return (targetChanged || justFinishedApproach)
        ? GetTargetSwitchAttackIntervalMsNoJitter(settings)
        : attackInterval;
}


// ── FindBestTarget override ───────────────────────────────────────────────────

CRole* MeleeHuntPlugin::FindBestTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    Position* outApproachPos, Position* outAttackPos,
    int* outClumpSize, bool* outUseScatter)
{
    if (outAttackPos)
        *outAttackPos = {};
    if (outUseScatter)
        *outUseScatter = false;
    return FindBestMeleeTarget(hero, map, settings, outApproachPos, outClumpSize);
}


// ── HandleCombatApproach override ────────────────────────────────────────────

void MeleeHuntPlugin::HandleCombatApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    CRole* target, const Position& approachPos, bool movementCommitted)
{
    if (!hero || !map || !target)
        return;

    const Position effectiveAttackPos = GetEffectiveHeroPosition(hero);
    const int moveDist = CGameMap::TileDist(effectiveAttackPos.x, effectiveAttackPos.y,
        target->m_posMap.x, target->m_posMap.y);
    const int approachDist = !IsZeroPos(approachPos)
        ? CGameMap::TileDist(effectiveAttackPos.x, effectiveAttackPos.y, approachPos.x, approachPos.y)
        : moveDist;
    const int clumpSize = m_lastClumpSize;
    const bool isClumpTarget = settings.prioritizeMobClumps && clumpSize >= kMinMobClumpSize;
    const int actionRadius = (std::max)(1, settings.actionRadius);
    const int clumpRadius = (std::max)(1, settings.clumpRadius);
    const int localWalkRadius = (std::max)(actionRadius, clumpRadius);

    // Normal mode (Instant Attack off) always walks to nearby mobs
    const bool allowWalkToMob = !settings.usePacketJump;

    // If already in attack range, no approach needed
    if (moveDist <= kReliableAttackRange && IsZeroPos(approachPos))
        return;
    if (!IsZeroPos(approachPos) && approachDist == 0)
        return;

    // Don't re-evaluate while approach is in flight
    if (movementCommitted)
        return;

    if (!IsZeroPos(approachPos)) {
        // Approach to clump position
        const bool usingClumpApproach = isClumpTarget && !IsZeroPos(approachPos);
        const bool startedWalk = allowWalkToMob
            && approachDist <= localWalkRadius
            && StartWalkTo(hero, map, approachPos, 0);
        const bool startedPath = startedWalk || StartPathTo(hero, map, approachPos, 0);
        // Session 14 [OSCILLATION DIAGNOSTIC]: user live-reported (video +
        // log both confirm) stretches of several seconds where the
        // character repeatedly jumps between two fixed points without
        // landing an attack, before suddenly resuming fast kills. Not
        // throttled — this only fires on a genuine "decided to approach"
        // transition (bounded by the state machine itself), not a per-
        // candidate scan loop, so volume is not a concern the way
        // FindBestLoot's per-item trace was (session 14, commit 915fabd).
        // Once this fires during a live oscillation, comparing consecutive
        // lines' targetId/committedId/approachPos answers the open question:
        // same target with an unstable approach search, vs. two different
        // targets alternating despite the commitment fix.
        spdlog::info("[hunt-melee] Approach target={} targetPos=({},{}) committed={} hits={}/{} approachPos=({},{}) heroPos=({},{}) moveDist={} approachDist={} clumpSize={} {}",
            target->GetID(), target->m_posMap.x, target->m_posMap.y,
            m_committedTargetId, m_hitsOnCommittedTarget, (std::max)(1, settings.meleeMinHitsPerTarget),
            approachPos.x, approachPos.y, hero->m_posMap.x, hero->m_posMap.y,
            moveDist, approachDist, clumpSize, startedWalk ? "walk" : "jump");
        if (startedPath) {
            SetState(AutoHuntState::ApproachTarget,
                startedWalk
                    ? (usingClumpApproach ? "Walking to mob clump" : "Walking to target")
                    : (usingClumpApproach ? "Jumping to mob clump" : "Jumping to target"));
        } else {
            SetState(AutoHuntState::ApproachTarget, "Unable to reach target");
        }
    } else {
        // No approach pos — FindBestMeleeApproachPos found NO valid adjacent
        // tile at all (every candidate blocked/unwalkable/out of zone, or
        // no DIRECT jump reaches one from here — see this function's header
        // comment). Session 14 [DEADLOCK FIX]: this used to fall to
        // StartPathNearTarget, which picks a "best nearby tile" via its own
        // distance-ranked candidate search — live-confirmed still landing
        // the deadlock (hit counter into the hundreds, position frozen 3-8
        // tiles out) even after that function's own on-target-tile fix,
        // because the real problem was upstream: FindBestMeleeApproachPos
        // only checks a single DIRECT jump from the hero's CURRENT position,
        // so anything blocking that one line fails the whole search even
        // when a completely ordinary multi-hop walk would reach the target
        // fine. Switched to StartPathTo aimed at the target's own position
        // directly — it already does real A* pathfinding around obstacles
        // (not just a direct-jump check) when a straight jump isn't
        // possible, and naturally stops once in range: HandleCombatAttack
        // stops any in-progress path the moment combat next decides it's
        // close enough to attack, before the path ever reaches the target's
        // own tile. No "which nearby tile is best" computation at all in
        // this fallback, so there's no candidate ranking left that could
        // ever select the target's own square.
        const bool startedWalk = allowWalkToMob
            && moveDist <= localWalkRadius
            && StartWalkTo(hero, map, target->m_posMap, kReliableAttackRange);
        const bool startedPath = startedWalk || StartPathTo(hero, map, target->m_posMap, kReliableAttackRange);
        spdlog::info("[hunt-melee] Approach(no-pos-found) target={} targetPos=({},{}) committed={} hits={}/{} heroPos=({},{}) moveDist={} clumpSize={} {}",
            target->GetID(), target->m_posMap.x, target->m_posMap.y,
            m_committedTargetId, m_hitsOnCommittedTarget, (std::max)(1, settings.meleeMinHitsPerTarget),
            hero->m_posMap.x, hero->m_posMap.y, moveDist, clumpSize, startedWalk ? "walk" : "path");
        if (startedPath) {
            SetState(AutoHuntState::ApproachTarget,
                startedWalk ? "Walking to target" : "Closing distance to target");
        } else {
            SetState(AutoHuntState::ApproachTarget, "Unable to reach target");
        }
    }
}


// ── HandleCombatAttack override ───────────────────────────────────────────────

void MeleeHuntPlugin::HandleCombatAttack(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    CRole* target, const Position& /*attackPos*/, DWORD now)
{
    if (!hero || !target)
        return;

    const bool movementCommitted = hero->IsJumping() || (m_pendingJumpTick != 0);
    const int moveDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
        target->m_posMap.x, target->m_posMap.y);
    const int clumpSize = m_lastClumpSize;
    const bool isClumpTarget = settings.prioritizeMobClumps && clumpSize >= kMinMobClumpSize;

    // Normal mode: only attack when within 1 tile (adjacent).
    // Instant Attack mode: attack immediately at any distance.
    if (!settings.usePacketJump) {
        const int actualDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
            target->m_posMap.x, target->m_posMap.y);
        if (actualDist > 1) {
            SetState(AutoHuntState::AttackTarget,
                movementCommitted
                    ? "Waiting until adjacent before attacking during jump"
                    : "Waiting until adjacent before attacking");
            return;
        }
    }

    // Determine attack interval
    const DWORD nextAttackDelay = ComputeMeleeAttackDelayMs(hero, target, settings);

    if (Pathfinder::Get().IsActive())
        Pathfinder::Get().Stop();

    if (now - m_lastAttackTick >= nextAttackDelay) {
        hero->AttackTarget(target->GetID(), target->m_posMap);
        m_lastAttackTick = now;
        NoteMeleeAttackAttempt(target->GetID());
    }

    if (movementCommitted && moveDist > kReliableAttackRange) {
        SetState(AutoHuntState::AttackTarget,
            isClumpTarget
                ? "Attacking mob clump during jump"
                : "Attacking target during jump");
    } else if (isClumpTarget) {
        SetState(AutoHuntState::AttackTarget, "Attacking target inside mob clump");
    } else {
        SetState(AutoHuntState::AttackTarget, "Attacking target");
    }
}


// ── RenderCombatUI override ───────────────────────────────────────────────────

void MeleeHuntPlugin::RenderCombatUI(AutoHuntSettings& settings)
{
    ImGui::Checkbox("Instant Attack (Skip Distance Check)", &settings.usePacketJump);
    ImGui::TextDisabled("Attacks fire immediately instead of waiting to be adjacent, and approach uses jump-only movement (no walk animation). Falls back to normal behavior when players are nearby.");
    ImGui::SliderInt("Stay Within Zone Radius", &settings.actionRadius, 1, CGameMap::MAX_JUMP_DIST);
    ImGui::Checkbox("Prioritize Mob Clumps", &settings.prioritizeMobClumps);
    ImGui::SliderInt("Clump Radius", &settings.clumpRadius, 1, 18);
    ImGui::SliderInt("Minimum Mob Clump", &settings.minimumMobClump, 2, 12);
    ImGui::TextDisabled("Prefer nearby targets inside Stay Within Zone Radius before chasing distant mob clumps.");
    ImGui::TextDisabled("If enough mobs are already inside Clump Radius, auto hunt clears that local pack first.");
    ImGui::SliderInt("Min Attacks Per Target", &settings.meleeMinHitsPerTarget, 1, 10);
    ImGui::TextDisabled("With Cyclone/Superman active, target choice normally randomizes every tick to spread damage across the clump. This many attack ATTEMPTS (not confirmed hits — melee doesn't land every swing) land on one target before the next randomize is allowed to pick someone else.");
}
