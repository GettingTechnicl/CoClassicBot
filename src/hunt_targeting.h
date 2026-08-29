#pragma once
#include "hunt_settings.h"
#include "base.h"
#include <string>
#include <vector>

class CRole;
class CHero;
class CGameMap;

// ── Name filter helpers ───────────────────────────────────────────────────
// Session 10: this module was independently copy-pasted across 4 files
// (hunt_targeting.cpp, base_hunt_plugin.cpp, mining_plugin.cpp,
// mule_plugin.cpp) — consolidated here since this header was already widely
// included by all of them.

// Lowercase copy, for case-insensitive name/filter comparisons.
std::string ToLowerCopy(const std::string& value);

// Split a comma/semicolon/newline-separated filter list into trimmed,
// lowercased tokens.
std::vector<std::string> ParseTokens(const char* text);

// True if `filters` is empty (no filter configured — everything matches) or
// `name` (case-insensitively) contains any filter as a substring.
bool NameMatchesFilters(const char* name, const std::vector<std::string>& filters);

// Append `token` to a comma-separated filter list buffer (used by the
// overlay's "add to filter" buttons), truncating rather than overflowing.
void AppendFilterToken(char* buffer, size_t bufferSize, const char* token);

// Collect all live monster targets that satisfy zone/name filters.
// If preferredOnly is true, only monsters matching monsterPreferNames are returned.
// If ignoreSearchRange is true, the per-hero "Only Target Mobs Within"
// (mobSearchRange) distance filter is skipped — used for NAVIGATION/steering,
// which should see every in-zone monster the minimap shows, even though
// ATTACK targeting deliberately still gates by mobSearchRange so the bot
// doesn't fire (e.g. scatter) at things too far to actually hit.
//
// rangeOrigin, when non-null, is the position the mobSearchRange gate measures
// from instead of hero->m_posMap. Combat callers MUST pass their
// GetEffectiveHeroPosition() here: during a jump the hero's m_posMap is still
// the departure tile, while every shot/steer decision is made from the jump
// DESTINATION — measuring the two from different tiles made the bot blind to
// mobs around where it was landing (it chain-jumped through dense packs
// without firing, because "in attack range" and "worth shooting" never agreed
// on where the hero actually was).
std::vector<CRole*> CollectHuntTargets(const AutoHuntSettings& settings, bool preferredOnly = false,
    bool ignoreSearchRange = false, const Position* rangeOrigin = nullptr);

// ── Archer mode helpers ───────────────────────────────────────────────────
// Session 12: independently copy-pasted (byte-for-byte identical) across
// hunt_town.cpp, hunt_buffs.cpp, hunt_targeting.cpp, archer_hunt_plugin.cpp,
// and base_hunt_plugin.cpp — consolidated here for the same reason as the
// name-filter helpers above.

bool IsArcherModeEnabled(const AutoHuntSettings& settings);

// Buffer added on top of the configured archer safety distance when
// deciding how far a threat must be before it's considered "safe" —
// prevents a threat sitting exactly at the safety distance from flapping
// in and out of range every tick due to minor position jitter.
constexpr int kArcherSafetyBufferTiles = 1;

// Effective archer safety distance for the current settings — 0 (no safety
// distance needed) if archer mode is off or Fly XP is active (immune to
// melee), otherwise the configured settings.archerSafetyDistance.
int GetArcherSafetyDistance(const AutoHuntSettings& settings);

// safetyDist + kArcherSafetyBufferTiles, or 0 if safetyDist <= 0.
int GetRequiredArcherThreatDistance(int safetyDist);

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
