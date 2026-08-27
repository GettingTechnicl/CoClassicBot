#include "hunt_contest.h"
#include "game.h"
#include "CHero.h"
#include "CRole.h"
#include "CGameMap.h"
#include "entities.h"
#include "hunt_targeting.h"

#include <unordered_map>
#include <cstdint>
#include <windows.h>

namespace {
    // How long a player must occupy the SAME bucket continuously before
    // it's treated as camping rather than passing through. Needs live
    // tuning once there's a real character to test against — see
    // AUTOHUNT_AUTOLEVEL_PLAN.md's "Open question" note.
    constexpr DWORD kCampingConfirmMs = 3 * 60 * 1000;   // 3 minutes

    // How long to leave a confirmed-contested bucket alone before checking
    // again. Re-extended by the same window each time the camper is still
    // there at recheck time — deliberately NOT exponential, matches the
    // user's own framing ("another 20-30 mins", not a doubling backoff).
    constexpr DWORD kRecheckBackoffMs = 25 * 60 * 1000;  // ~20-30 min midpoint

    struct BucketState {
        OBJID camperPlayerId = 0;
        DWORD continuousSinceTick = 0;  // when THIS camper was first seen here
        DWORD nextRecheckTick = 0;      // 0 = not currently marked contested
    };

    // Same 8-tile bucket granularity as SpawnMemory's PackBucket, so
    // contest and density data land on the same grid.
    uint32_t PackBucket(int x, int y) {
        const int bx = x / 8;
        const int by = y / 8;
        return (static_cast<uint32_t>(bx) << 16) ^ (static_cast<uint32_t>(by) & 0xFFFFu);
    }

    std::unordered_map<OBJID, std::unordered_map<uint32_t, BucketState>> g_perMap;
}

namespace HuntContest {

bool IsBucketContested(OBJID mapId, const Position& pos, const AutoHuntSettings& settings)
{
    auto& buckets = g_perMap[mapId];
    const uint32_t key = PackBucket(pos.x, pos.y);
    const DWORD now = GetTickCount();

    // Find a non-whitelisted player within one bucket's radius of the
    // candidate point. Same scan shape as GetParanoiaThreat's, just scoped
    // to this one bucket instead of the whole safetyPlayerRange.
    CRole* camper = nullptr;
    if (CHero* hero = Game::GetHero()) {
        const OBJID heroId = hero->GetID();
        const std::vector<std::string> whitelist = ParseTokens(settings.playerWhitelist);
        for (CRole* role : Entities::Get()) {
            if (!role || !Entities::IsAlive(role) || !role->IsPlayer() || role->GetID() == heroId)
                continue;
            if (!whitelist.empty() && NameMatchesFilters(role->GetName(), whitelist))
                continue;
            if (CGameMap::TileDist(pos.x, pos.y, role->m_posMap.x, role->m_posMap.y) > 8)
                continue;
            camper = role;
            break;
        }
    }

    auto it = buckets.find(key);

    if (!camper) {
        // Nobody there right now — nothing to track.
        if (it != buckets.end())
            buckets.erase(it);
        return false;
    }

    const OBJID camperId = camper->GetID();
    if (it == buckets.end() || it->second.camperPlayerId != camperId) {
        // First sighting of THIS player at this bucket (or a different
        // player replaced whoever was tracked before) — start fresh. One
        // sighting isn't camping yet; ordinary Paranoia distance-avoidance
        // already handles a transient visitor without any of this.
        BucketState& st = buckets[key];
        st.camperPlayerId = camperId;
        st.continuousSinceTick = now;
        st.nextRecheckTick = 0;
        return false;
    }

    BucketState& st = it->second;
    if (st.nextRecheckTick == 0) {
        // Same player, continuously, but not yet confirmed as camping.
        if (now - st.continuousSinceTick >= kCampingConfirmMs) {
            st.nextRecheckTick = now + kRecheckBackoffMs;
            return true;
        }
        return false;
    }

    // Already contested. Recheck window reached and they're STILL here —
    // extend the backoff and keep avoiding it.
    if (now >= st.nextRecheckTick)
        st.nextRecheckTick = now + kRecheckBackoffMs;
    return true;
}

Stats GetStats(OBJID mapId)
{
    Stats s{};
    auto mapIt = g_perMap.find(mapId);
    if (mapIt == g_perMap.end())
        return s;
    s.trackedBuckets = (int)mapIt->second.size();
    for (const auto& entry : mapIt->second) {
        if (entry.second.nextRecheckTick != 0)
            s.contestedBuckets++;
    }
    return s;
}

}  // namespace HuntContest
