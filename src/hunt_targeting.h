#pragma once
#include "hunt_settings.h"
#include "base.h"
#include <vector>

class CRole;
class CHero;
class CGameMap;

// Collect all live monster targets that satisfy zone/name filters.
// If preferredOnly is true, only monsters matching monsterPreferNames are returned.
std::vector<CRole*> CollectHuntTargets(const AutoHuntSettings& settings, bool preferredOnly = false);

// ── Destination jitter ───────────────────────────────────────────────────
// Return a walkable tile NEAR `target`, never `target` itself.
//
// Applied at every destination the bot picks — patrol points, route
// waypoints, approach tiles. Two reasons this is a single shared helper
// rather than per-call-site randomness:
//   1. Landing on exact, repeatable coordinates is a machine signature.
//      A recorded route replayed tile-for-tile forever is about as obviously
//      automated as movement gets.
//   2. It breaks deadlocks. Several "stuck" behaviours come from recomputing
//      the same rejected tile every frame; jitter guarantees the search space
//      moves.
// Radius defaults to a third of the attack range, so a long-range class
// varies more than a melee one and the offset stays inside the configured
// zone. Falls back to the original tile if nothing walkable is nearby.
Position JitterDestination(const CGameMap* map, const Position& target, int radius);

// Jitter radius for the current settings: ~1/3 of the leash (attack range).
int GetJitterRadius(const AutoHuntSettings& settings);

// Count targets within a Euclidean radius of center.
int CountTargetsInRadius(const std::vector<CRole*>& targets, const Position& center, float radius);

// Return the closest target within maxTileRange (Chebyshev). Pass -1 for no range limit.
CRole* FindClosestTarget(const std::vector<CRole*>& targets, const Position& from, int maxTileRange = -1);

// Return a random target within maxTileRange (Chebyshev).
CRole* FindRandomTarget(const std::vector<CRole*>& targets, const Position& from, int maxTileRange);

// Return the target that is the center of the densest cluster within radius.
// Writes cluster size into *outClusterSize if non-null.
CRole* FindBestClusterTarget(const std::vector<CRole*>& targets, const Position& from,
    float radius, int* outClusterSize);

// Find the best single-jump approach position to attack target from heroPos.
// heroPos should be GetEffectiveHeroPosition(hero) from the caller.
// Returns true and writes outApproachPos if a valid tile is found.
bool FindBestSingleTargetApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    CRole* target, const Position& heroPos, Position& outApproachPos, int attackRange);
