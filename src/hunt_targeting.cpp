#include "hunt_targeting.h"
#include "hunt_intervals.h"
#include "jitter.h"
#include "game.h"
#include "CHero.h"
#include "CRole.h"
#include "CGameMap.h"
#include "pathfinder.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <random>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// Archer mode helpers
// ---------------------------------------------------------------------------

constexpr int kArcherSafetyBufferTiles = 1;

bool IsArcherModeEnabled(const AutoHuntSettings& settings)
{
    return settings.archerMode || settings.combatMode == AutoHuntCombatMode::Archer;
}

int GetArcherSafetyDistance(const AutoHuntSettings& settings)
{
    if (!IsArcherModeEnabled(settings))
        return 0;
    // Fly XP skill makes the archer immune to melee — ignore safety distance
    CHero* hero = Game::GetHero();
    if (hero && hero->IsFlyActive())
        return 0;
    return (std::max)(0, settings.archerSafetyDistance);
}

int GetRequiredArcherThreatDistance(int safetyDist)
{
    return safetyDist > 0 ? (safetyDist + kArcherSafetyBufferTiles) : 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Name filter helpers — shared across hunt_targeting/base_hunt_plugin/
// mining_plugin/mule_plugin (see hunt_targeting.h)
// ---------------------------------------------------------------------------

std::string ToLowerCopy(const std::string& value)
{
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return lower;
}

std::vector<std::string> ParseTokens(const char* text)
{
    std::vector<std::string> tokens;
    if (!text || !text[0])
        return tokens;

    std::string current;
    while (*text) {
        const char ch = *text++;
        if (ch == ',' || ch == ';' || ch == '\n' || ch == '\r' || ch == '\t') {
            if (!current.empty()) {
                size_t start = 0;
                while (start < current.size() && std::isspace((unsigned char)current[start]))
                    start++;
                size_t end = current.size();
                while (end > start && std::isspace((unsigned char)current[end - 1]))
                    end--;
                if (end > start)
                    tokens.push_back(ToLowerCopy(current.substr(start, end - start)));
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }

    if (!current.empty()) {
        size_t start = 0;
        while (start < current.size() && std::isspace((unsigned char)current[start]))
            start++;
        size_t end = current.size();
        while (end > start && std::isspace((unsigned char)current[end - 1]))
            end--;
        if (end > start)
            tokens.push_back(ToLowerCopy(current.substr(start, end - start)));
    }

    return tokens;
}

bool NameMatchesFilters(const char* name, const std::vector<std::string>& filters)
{
    if (filters.empty())
        return true;
    if (!name || !name[0])
        return false;

    const std::string lowered = ToLowerCopy(name);
    for (const std::string& filter : filters) {
        if (!filter.empty() && lowered.find(filter) != std::string::npos)
            return true;
    }
    return false;
}

void AppendFilterToken(char* buffer, size_t bufferSize, const char* token)
{
    if (!buffer || bufferSize == 0 || !token || !token[0])
        return;

    if (buffer[0] != '\0')
        strncat_s(buffer, bufferSize, ", ", _TRUNCATE);
    strncat_s(buffer, bufferSize, token, _TRUNCATE);
}

// ---------------------------------------------------------------------------
// Public free functions
// ---------------------------------------------------------------------------

int GetJitterRadius(const AutoHuntSettings& settings)
{
    // ~1/3 of the leash, minimum 1 — enough to be non-repeating without
    // meaningfully changing where the bot ends up.
    return (std::max)(1, GetHuntLeash(settings) / 3);
}

Position JitterDestination(const CGameMap* map, const Position& target, int radius)
{
    if (radius <= 0)
        return target;

    // Try a handful of offsets and take the first walkable one. Deliberately
    // excludes (0,0): the point is to never land on the exact tile.
    for (int attempt = 0; attempt < 12; ++attempt) {
        const int span = radius * 2 + 1;
        int dx = (int)(NextRandom32() % (uint32_t)span) - radius;
        int dy = (int)(NextRandom32() % (uint32_t)span) - radius;
        if (dx == 0 && dy == 0)
            dx = (NextRandom32() & 1) ? 1 : -1;

        const Position candidate = { target.x + dx, target.y + dy };
        if (candidate.x <= 0 || candidate.y <= 0)
            continue;
        if (map && !map->IsWalkable(candidate.x, candidate.y))
            continue;
        return candidate;
    }
    return target;
}

std::vector<CRole*> CollectHuntTargets(const AutoHuntSettings& settings, bool preferredOnly)
{
    // Session 9: CRoleMgr::m_deqRole is not a deque on v1074 — see entities.h.
    const std::vector<CRole*>& roles = Entities::Get();
    if (roles.empty())
        return {};

    const CHero* hero = Game::GetHero();
    const std::vector<std::string> includeFilters = ParseTokens(settings.monsterNames);
    const std::vector<std::string> ignoreFilters = ParseTokens(settings.monsterIgnoreNames);
    const std::vector<std::string> preferFilters = preferredOnly ? ParseTokens(settings.monsterPreferNames) : std::vector<std::string>{};
    std::vector<CRole*> targets;
    targets.reserve((std::min)(roles.size(), size_t(128)));

    for (CRole* role : roles) {
        if (!role)
            continue;

        if (!role->IsMonster() || role->IsDead() || role->TestState(USERSTATUS_GHOST))
            continue;
        // Session 10: engage anything within the leash band, not strictly
        // inside the zone. The zone marks where we HUNT; a monster just past
        // the edge is still worth killing, and skipping it both wastes the
        // kill and lets spawns stagnate. The margin is bounded (2x attack
        // range) so this never becomes chasing across the map — the bot's own
        // movement is separately capped at one leash outside the zone.
        if (!IsPointNearHuntZone(settings, settings.zoneMapId, role->m_posMap,
                                 GetHuntEngageMargin(settings)))
            continue;
        if (hero && settings.mobSearchRange > 0
            && CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
                role->m_posMap.x, role->m_posMap.y) > settings.mobSearchRange)
            continue;
        if (!ignoreFilters.empty() && NameMatchesFilters(role->GetName(), ignoreFilters))
            continue;
        if (!NameMatchesFilters(role->GetName(), includeFilters))
            continue;
        if (preferredOnly && !preferFilters.empty() && !NameMatchesFilters(role->GetName(), preferFilters))
            continue;
        targets.push_back(role);
    }

    // Session 11 [Paranoia Mode]: bias target choice away from a detected
    // player rather than stopping hunting entirely (that's what Safety Rest
    // already does). Drop targets that would pull the hero MEANINGFULLY
    // closer to the threat than it already is — normal target-selection
    // (closest/clump-based) then naturally drifts toward whatever's left,
    // producing a gradual "hunt away from the player" without any dedicated
    // avoidance pathing. Never lets the filter empty the list outright
    // (falls back to every target) so the bot keeps fighting even when
    // every visible monster happens to be threat-side.
    Position threatPos{};
    if (hero && GetParanoiaThreat(settings, &threatPos)) {
        const float heroToThreat = hero->m_posMap.DistanceTo(threatPos);
        constexpr float kParanoiaMargin = 3.0f;  // tiles; avoids filtering on noise
        std::vector<CRole*> awayFromThreat;
        awayFromThreat.reserve(targets.size());
        for (CRole* target : targets) {
            if (target->m_posMap.DistanceTo(threatPos) >= heroToThreat - kParanoiaMargin)
                awayFromThreat.push_back(target);
        }
        if (!awayFromThreat.empty())
            targets = std::move(awayFromThreat);
    }

    return targets;
}

int CountTargetsInRadius(const std::vector<CRole*>& targets, const Position& center, float radius)
{
    int count = 0;
    for (CRole* target : targets) {
        if (target && center.DistanceTo(target->m_posMap) <= radius)
            ++count;
    }
    return count;
}

CRole* FindClosestTarget(const std::vector<CRole*>& targets, const Position& from, int maxTileRange)
{
    CRole* best = nullptr;
    float bestDist = (std::numeric_limits<float>::max)();
    for (CRole* target : targets) {
        if (!target)
            continue;

        if (maxTileRange >= 0
            && CGameMap::TileDist(from.x, from.y, target->m_posMap.x, target->m_posMap.y) > maxTileRange) {
            continue;
        }

        const float dist = from.DistanceTo(target->m_posMap);
        if (!best || dist < bestDist) {
            best = target;
            bestDist = dist;
        }
    }

    return best;
}

CRole* FindRandomTarget(const std::vector<CRole*>& targets, const Position& from, int maxTileRange)
{
    std::vector<CRole*> candidates;
    for (CRole* target : targets) {
        if (target && CGameMap::TileDist(from.x, from.y, target->m_posMap.x, target->m_posMap.y) <= maxTileRange)
            candidates.push_back(target);
    }
    if (candidates.empty())
        return nullptr;
    if (candidates.size() == 1)
        return candidates[0];

    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
    return candidates[dist(rng)];
}

CRole* FindBestClusterTarget(const std::vector<CRole*>& targets, const Position& from,
    float radius, int* outClusterSize)
{
    if (outClusterSize)
        *outClusterSize = 0;

    CRole* best = nullptr;
    int bestCount = 0;
    float bestDist = (std::numeric_limits<float>::max)();
    for (CRole* target : targets) {
        if (!target)
            continue;

        const int count = CountTargetsInRadius(targets, target->m_posMap, radius);
        const float dist = from.DistanceTo(target->m_posMap);
        if (!best || count > bestCount || (count == bestCount && dist < bestDist)) {
            best = target;
            bestCount = count;
            bestDist = dist;
        }
    }

    if (outClusterSize)
        *outClusterSize = bestCount;
    return best;
}

bool FindBestSingleTargetApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    CRole* target, const Position& heroPos, Position& outApproachPos, int attackRange)
{
    outApproachPos = {};

    if (!hero || !map || !target || attackRange <= 0)
        return false;

    const Position jumpOrigin = heroPos;
    if (CGameMap::TileDist(jumpOrigin.x, jumpOrigin.y, target->m_posMap.x, target->m_posMap.y) <= attackRange)
        return false;

    bool found = false;
    int bestAttackDist = (std::numeric_limits<int>::max)();
    int bestMinThreatDist = -1;
    float bestTargetDist = (std::numeric_limits<float>::max)();
    float bestMoveDist = (std::numeric_limits<float>::max)();

    const int safetyDist = GetArcherSafetyDistance(settings);
    const int requiredThreatDist = GetRequiredArcherThreatDistance(safetyDist);
    const std::vector<CRole*> nearbyThreats = safetyDist > 0 ? CollectHuntTargets(settings) : std::vector<CRole*>{};
    for (int dx = -attackRange; dx <= attackRange; ++dx) {
        for (int dy = -attackRange; dy <= attackRange; ++dy) {
            const Position candidate = {target->m_posMap.x + dx, target->m_posMap.y + dy};
            const int attackDist = CGameMap::TileDist(candidate.x, candidate.y, target->m_posMap.x, target->m_posMap.y);
            if (attackDist > attackRange)
                continue;
            int minThreatDist = (std::numeric_limits<int>::max)();
            if (requiredThreatDist > 0) {
                for (CRole* t : nearbyThreats) {
                    if (!t)
                        continue;
                    const int threatDist = CGameMap::TileDist(candidate.x, candidate.y, t->m_posMap.x, t->m_posMap.y);
                    if (threatDist < minThreatDist)
                        minThreatDist = threatDist;
                }
                if (minThreatDist < requiredThreatDist)
                    continue;
            }
            if (!IsPointInHuntZone(settings, settings.zoneMapId, candidate))
                continue;
            if (!map->IsWalkable(candidate.x, candidate.y))
                continue;
            if ((candidate.x != jumpOrigin.x || candidate.y != jumpOrigin.y) && IsTileOccupied(candidate.x, candidate.y))
                continue;
            if (!map->CanJump(jumpOrigin.x, jumpOrigin.y, candidate.x, candidate.y, CGameMap::GetHeroAltThreshold()))
                continue;

            const float targetDist = candidate.DistanceTo(target->m_posMap);
            const float moveDist = jumpOrigin.DistanceTo(candidate);
            if (!found
                || attackDist < bestAttackDist
                || (attackDist == bestAttackDist && minThreatDist > bestMinThreatDist)
                || (attackDist == bestAttackDist && minThreatDist == bestMinThreatDist && targetDist < bestTargetDist)
                || (attackDist == bestAttackDist && minThreatDist == bestMinThreatDist && targetDist == bestTargetDist && moveDist < bestMoveDist)) {
                found = true;
                outApproachPos = candidate;
                bestAttackDist = attackDist;
                bestMinThreatDist = minThreatDist;
                bestTargetDist = targetDist;
                bestMoveDist = moveDist;
            }
        }
    }

    return found;
}
