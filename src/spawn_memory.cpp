#include "spawn_memory.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <tuple>
#include <spdlog/spdlog.h>

namespace SpawnMemory
{
namespace
{
    // Decay per observation batch. ~0.9993 halves a score in ~1000 scans;
    // at the entity refresh rate that is roughly 8 minutes of active hunting.
    // Slow enough to survive clearing an area, fast enough to follow a spawn
    // layout that genuinely changes.
    constexpr float kDecayPerObserve = 0.9993f;
    constexpr float kPruneBelow      = 0.05f;
    constexpr int   kMinObservations = 40;      // before scores are trusted

    // Session 13 [NOVELTY BOOST] (user-designed): visibility-limited
    // observation makes the heatmap self-reinforcing — the bot camps its known
    // hot quadrant, never learns the rest of the zone, and an exploration roll
    // that glimpses a monster somewhere new still walks away because one
    // sighting scores ~1.0 against a hot bucket's hundreds. So: a monster seen
    // in a bucket with NO history (fresh insertion into the sparse map) arms a
    // temporary boost that makes that bucket score near the map max — "test it
    // out" — for the next kNoveltyObserves observation batches (~2-5 min at
    // normal scan cadence, same scan-count clock the decay uses). Patrol then
    // pulls the bot over, real observations either earn the bucket a genuine
    // score or don't, and the boost expires either way. Session-local by
    // design (not persisted): novelty is a trigger, not knowledge.
    constexpr int   kNoveltyObserves   = 600;
    constexpr float kNoveltyScoreShare = 0.9f;  // boosted score = 90% of map max
    // Session 13 [NOVELTY CAP]: novelty only means something while it's
    // scarce. First live MapWide run armed 1,073 novel buckets in 5.5 minutes
    // — every step of travel uncovered more "uncharted" ground, every sampled
    // explore candidate scored the same near-max boost, and the bot
    // ping-ponged corner to corner doing nothing but discover more novelty.
    // A small cap turns it into what it was meant to be: a short queue of
    // places worth testing next, arming again as earlier entries expire.
    constexpr int   kMaxNovelBuckets   = 8;

    // Session-2026-09-04 [DENSITY]: raw score conflates "spawns a lot" with
    // "the bot walks past here a lot" — a bucket on the beaten path racks up
    // observation opportunities (and therefore appearances) far more than an
    // equally spawny bucket off to the side. GetDensity divides appearances
    // by opportunities so both are judged on the same footing. Both counters
    // decay on the SAME clock (kDecayPerObserve) so density stays a RECENT
    // rate, not an all-session average that would asymptote toward zero over
    // a long-running grind — and so a bucket the bot hasn't been near in a
    // long time reverts to "unknown" rather than staying "confirmed empty"
    // forever from one old visit.
    //
    // "Within view" radius: reuses the game's own AOI range already assumed
    // elsewhere (kViewTiles in base_hunt_plugin.cpp, ~35 tiles — that's what
    // bounds which monsters the entity scan can even see) rather than
    // inventing a second notion of "visible."
    constexpr float kObservationRadiusTiles = 35.0f;
    // Below this, too few looks have happened to trust a ratio — 3 fresh
    // opportunities (little decay yet) is enough to not divide by noise
    // without delaying "unknown" recognition for long.
    constexpr float kMinTimesObservedTrust = 3.0f;
    // How often (in Observe() calls) to dump a density snapshot to the log —
    // frequent enough that one play session shows whether density-based
    // exploration is roaming instead of camping, cheap enough not to spam.
    constexpr int   kDiagnosticLogEveryObserves = 200;

    // Session 12: this is the second documented safety rail (header comment,
    // "dropping maps not seen for a long time on load") — it was written
    // (lastTouched updated every Observe()) but never actually read anywhere,
    // so a map you stopped hunting months ago stayed in the save file
    // forever. lastTouched now stores a WALL-CLOCK epoch-seconds value
    // (time(nullptr)), not GetTickCount() — GetTickCount() is boot-relative
    // and meaningless once compared across a process/system restart, which
    // is exactly the comparison Load() needs to make.
    constexpr int64_t kStaleMapMaxAgeSeconds = 30LL * 24 * 60 * 60;  // 30 days

    struct MapMemory
    {
        std::unordered_map<uint32_t, float> buckets;   // packed bucket -> score
        // packed bucket -> observation-count deadline for its novelty boost
        // (see kNoveltyObserves). Session-local, never saved.
        std::unordered_map<uint32_t, int> novelty;
        // Monster id -> the bucket it was seen in last Observe() batch. Lets
        // Observe tell an APPEARANCE (new id, or an id that moved buckets)
        // from continued dwell (same id, same bucket) so standing still, or
        // being fought, doesn't rack up score every ~0.5s it stays on screen.
        // Session-local, never saved — ids are only meaningful within a
        // client session.
        std::unordered_map<OBJID, uint32_t> presentLastObserve;
        // packed bucket -> decayed count of Observe() calls this bucket was
        // within view, whether or not a monster was in it. Denominator for
        // GetDensity. Session-local, never saved (see kObservationRadiusTiles).
        std::unordered_map<uint32_t, float> timesObserved;
        int     observations = 0;
        float   maxScore = 0.0f;
        int64_t lastTouched = 0;   // time(nullptr) — see kStaleMapMaxAgeSeconds above
        bool    dirty = false;
    };

    std::unordered_map<uint32_t, MapMemory> g_maps;
    std::mutex g_mutex;

    uint32_t PackBucket(int bx, int by) { return ((uint32_t)bx << 16) | (uint32_t)(by & 0xFFFF); }
    int BucketX(uint32_t k) { return (int)(k >> 16); }
    int BucketY(uint32_t k) { return (int)(k & 0xFFFF); }

    std::string StorePath()
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(GetModuleHandleA("coclassic.dll"), path, MAX_PATH);
        std::string p(path);
        const size_t slash = p.find_last_of("\\/");
        if (slash != std::string::npos)
            p = p.substr(0, slash);
        else
            p = ".";
        return p + "\\spawn_memory.txt";
    }
}

void Observe(OBJID mapId, const Position& heroPos, const std::vector<std::pair<OBJID, Position>>& monsters)
{
    if (mapId == 0)
        return;

    std::lock_guard<std::mutex> lk(g_mutex);
    MapMemory& mm = g_maps[(uint32_t)mapId];
    mm.lastTouched = static_cast<int64_t>(time(nullptr));
    ++mm.observations;
    mm.dirty = true;

    // Decay everything first so knowledge fades where monsters stopped
    // appearing, then reinforce what we can currently see.
    float newMax = 0.0f;
    for (auto it = mm.buckets.begin(); it != mm.buckets.end(); ) {
        it->second *= kDecayPerObserve;
        if (it->second < kPruneBelow) {
            it = mm.buckets.erase(it);       // self-pruning: bounds growth
        } else {
            if (it->second > newMax) newMax = it->second;
            ++it;
        }
    }

    // Decay observation-opportunity the same way, same clock — see the
    // [DENSITY] comment above for why this can't be a monotonic counter.
    for (auto it = mm.timesObserved.begin(); it != mm.timesObserved.end(); ) {
        it->second *= kDecayPerObserve;
        if (it->second < kPruneBelow)
            it = mm.timesObserved.erase(it);
        else
            ++it;
    }

    // Mark every bucket within view as looked-at this batch, monster or not —
    // this is what lets GetDensity tell "searched, nothing here" apart from
    // "never been here."
    if (heroPos.x > 0 && heroPos.y > 0) {
        const int hbx = heroPos.x / kBucketTiles;
        const int hby = heroPos.y / kBucketTiles;
        const int rBuckets = (int)(kObservationRadiusTiles / kBucketTiles) + 1;
        for (int dby = -rBuckets; dby <= rBuckets; ++dby) {
            for (int dbx = -rBuckets; dbx <= rBuckets; ++dbx) {
                const int bx = hbx + dbx, by = hby + dby;
                if (bx < 0 || by < 0)
                    continue;
                const Position center{ bx * kBucketTiles + kBucketTiles / 2, by * kBucketTiles + kBucketTiles / 2 };
                if (heroPos.DistanceTo(center) > kObservationRadiusTiles)
                    continue;
                const uint32_t key = PackBucket(bx, by);
                auto [it, inserted] = mm.timesObserved.try_emplace(key, 0.0f);
                if (inserted && (int)mm.timesObserved.size() > kMaxBucketsPerMap) {
                    mm.timesObserved.erase(it);   // safety rail, see kMaxBucketsPerMap
                    continue;
                }
                it->second += 1.0f;
            }
        }
    }

    // Appearance-based counting: an id still sitting in the bucket it was in
    // last batch is dwell, not a new sighting, and scores nothing — otherwise
    // a monster the bot stands and fights racks up hundreds of points just by
    // staying on screen while monsters merely walked past score once each.
    // `stillPresent` becomes the next batch's `presentLastObserve`, so an id
    // that leaves view is forgotten (a later id reuse — e.g. after a respawn
    // — is then correctly treated as a fresh appearance).
    std::unordered_map<OBJID, uint32_t> stillPresent;
    stillPresent.reserve(monsters.size());

    for (const auto& [id, p] : monsters) {
        if (p.x <= 0 || p.y <= 0)
            continue;
        const uint32_t key = PackBucket(p.x / kBucketTiles, p.y / kBucketTiles);
        stillPresent[id] = key;

        const auto prevIt = mm.presentLastObserve.find(id);
        if (prevIt != mm.presentLastObserve.end() && prevIt->second == key)
            continue;   // same monster, same bucket as last batch — dwell, no credit

        auto [it, inserted] = mm.buckets.try_emplace(key, 0.0f);
        if (inserted && (int)mm.buckets.size() > kMaxBucketsPerMap) {
            // Safety rail only — decay should keep us far below this.
            mm.buckets.erase(it);
            continue;
        }
        // Novelty boost (see kNoveltyObserves): first sighting in an
        // uncharted bucket — only arm once the map's data is otherwise
        // trusted, so the initial learning flood on a fresh map doesn't
        // mark everything novel at once.
        if (inserted && mm.observations >= kMinObservations
            && (int)mm.novelty.size() < kMaxNovelBuckets) {
            mm.novelty[key] = mm.observations + kNoveltyObserves;
            spdlog::debug("[spawnmem] novelty boost armed: map {} bucket ({},{}) for {} observes ({}/{} slots)",
                mapId, (int)(key >> 16), (int)(key & 0xFFFF), kNoveltyObserves,
                (int)mm.novelty.size(), kMaxNovelBuckets);
        }
        it->second += 1.0f;
        if (it->second > newMax) newMax = it->second;
    }
    mm.maxScore = newMax;
    mm.presentLastObserve.swap(stillPresent);

    // Expire novelty boosts whose window has passed. (Kept even if the bucket
    // itself was pruned — the deadline check below makes them inert, and this
    // sweep removes them shortly after either way.)
    for (auto it = mm.novelty.begin(); it != mm.novelty.end(); ) {
        if (it->second <= mm.observations)
            it = mm.novelty.erase(it);
        else
            ++it;
    }

    // [DENSITY diagnostic] one line every kDiagnosticLogEveryObserves calls:
    // top buckets by density with their raw (spawns, timesObserved) so a
    // single live session can confirm the counting fix AND that
    // density-based exploration is actually roaming instead of camping.
    if (mm.observations % kDiagnosticLogEveryObserves == 0) {
        std::vector<std::tuple<float, float, float, uint32_t>> rows;  // density, spawns, timesObserved, key
        rows.reserve(mm.buckets.size());
        for (const auto& [key, spawns] : mm.buckets) {
            const auto tot = mm.timesObserved.find(key);
            const float to = (tot == mm.timesObserved.end()) ? 0.0f : tot->second;
            const float density = (to >= kMinTimesObservedTrust) ? spawns / to : -1.0f;
            rows.emplace_back(density, spawns, to, key);
        }
        std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
        const int n = (std::min)((int)rows.size(), 8);
        std::string dump;
        for (int i = 0; i < n; ++i) {
            const auto& [density, spawns, to, key] = rows[i];
            char buf[80];
            snprintf(buf, sizeof(buf), "%s(%d,%d) d=%.2f s=%.1f/%.1f",
                i ? ", " : "", BucketX(key), BucketY(key), density, spawns, to);
            dump += buf;
        }
        spdlog::info("[spawnmem] map {} density snapshot @obs {} ({} buckets, top {}): {}",
            mapId, mm.observations, (int)mm.buckets.size(), n, dump);
    }
}

float GetScore(OBJID mapId, const Position& tile)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    auto mit = g_maps.find((uint32_t)mapId);
    if (mit == g_maps.end())
        return 0.0f;
    const MapMemory& mm = mit->second;
    const uint32_t key = PackBucket(tile.x / kBucketTiles, tile.y / kBucketTiles);
    auto bit = mm.buckets.find(key);
    float score = (bit == mm.buckets.end()) ? 0.0f : bit->second;
    // Active novelty boost: score this bucket like a near-best one so
    // exploration goes and tests it (see kNoveltyObserves).
    auto nit = mm.novelty.find(key);
    if (nit != mm.novelty.end() && nit->second > mm.observations)
        score = (std::max)(score, mm.maxScore * kNoveltyScoreShare);
    return score;
}

float GetDensity(OBJID mapId, const Position& tile)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    auto mit = g_maps.find((uint32_t)mapId);
    if (mit == g_maps.end())
        return -1.0f;
    const MapMemory& mm = mit->second;
    const uint32_t key = PackBucket(tile.x / kBucketTiles, tile.y / kBucketTiles);
    const auto tot = mm.timesObserved.find(key);
    if (tot == mm.timesObserved.end() || tot->second < kMinTimesObservedTrust)
        return -1.0f;   // unknown — hasn't been looked at enough to trust a rate
    const auto bit = mm.buckets.find(key);
    const float spawns = (bit == mm.buckets.end()) ? 0.0f : bit->second;
    return spawns / tot->second;
}

float GetMaxScore(OBJID mapId)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_maps.find((uint32_t)mapId);
    return (it == g_maps.end()) ? 0.0f : it->second.maxScore;
}

uint32_t BucketKeyFor(const Position& tile)
{
    return PackBucket(tile.x / kBucketTiles, tile.y / kBucketTiles);
}

Position BucketCenterOf(uint32_t key)
{
    return { BucketX(key) * kBucketTiles + kBucketTiles / 2, BucketY(key) * kBucketTiles + kBucketTiles / 2 };
}

std::vector<Position> GetHotBuckets(OBJID mapId, int maxCount)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<Position> out;
    auto mit = g_maps.find((uint32_t)mapId);
    if (mit == g_maps.end() || maxCount <= 0)
        return out;
    const MapMemory& mm = mit->second;

    // Effective score per bucket, novelty boosts applied the same way
    // GetScore applies them, so a boosted uncharted bucket makes the list.
    std::vector<std::pair<float, uint32_t>> scored;
    scored.reserve(mm.buckets.size() + mm.novelty.size());
    for (const auto& [key, sc] : mm.buckets) {
        float s = sc;
        auto nit = mm.novelty.find(key);
        if (nit != mm.novelty.end() && nit->second > mm.observations)
            s = (std::max)(s, mm.maxScore * kNoveltyScoreShare);
        scored.emplace_back(s, key);
    }
    for (const auto& [key, deadline] : mm.novelty) {
        if (deadline > mm.observations && mm.buckets.find(key) == mm.buckets.end())
            scored.emplace_back(mm.maxScore * kNoveltyScoreShare, key);
    }

    const int n = (std::min)((int)scored.size(), maxCount);
    std::partial_sort(scored.begin(), scored.begin() + n, scored.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        out.push_back({ BucketX(scored[i].second) * kBucketTiles + kBucketTiles / 2,
                        BucketY(scored[i].second) * kBucketTiles + kBucketTiles / 2 });
    }
    return out;
}

bool HasUsefulData(OBJID mapId)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_maps.find((uint32_t)mapId);
    if (it == g_maps.end())
        return false;
    return it->second.observations >= kMinObservations && !it->second.buckets.empty();
}

Stats GetStats(OBJID mapId)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    Stats s{};
    s.maps = (int)g_maps.size();
    auto it = g_maps.find((uint32_t)mapId);
    if (it != g_maps.end()) {
        s.buckets = (int)it->second.buckets.size();
        s.observations = it->second.observations;
        s.maxScore = it->second.maxScore;
        s.novelBuckets = (int)it->second.novelty.size();
        for (const auto& [key, to] : it->second.timesObserved)
            if (to >= kMinTimesObservedTrust)
                ++s.knownBuckets;
    }
    return s;
}

void ClearMap(OBJID mapId)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    g_maps.erase((uint32_t)mapId);
}

void Save()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    FILE* f = nullptr;
    if (fopen_s(&f, StorePath().c_str(), "w") != 0 || !f)
        return;

    // Plain text, one line per bucket: map bx by score
    fprintf(f, "# CoClassicBot spawn memory (map bucketX bucketY score), bucket=%d tiles\n", kBucketTiles);
    int written = 0;
    for (const auto& [mapId, mm] : g_maps) {
        if (mm.buckets.empty())
            continue;
        fprintf(f, "map %u %d %lld\n", mapId, mm.observations, (long long)mm.lastTouched);
        for (const auto& [key, score] : mm.buckets) {
            fprintf(f, "%d %d %.2f\n", BucketX(key), BucketY(key), score);
            ++written;
        }
    }
    fclose(f);
    spdlog::info("[spawnmem] saved {} buckets across {} maps", written, (int)g_maps.size());
}

void Load()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    FILE* f = nullptr;
    if (fopen_s(&f, StorePath().c_str(), "r") != 0 || !f)
        return;

    char line[256];
    uint32_t curMap = 0;
    int loaded = 0;
    int skippedStaleMaps = 0;
    const int64_t nowEpoch = static_cast<int64_t>(time(nullptr));
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#')
            continue;
        unsigned m = 0; int obs = 0; long long touchedEpoch = 0;
        const int mapFields = sscanf_s(line, "map %u %d %lld", &m, &obs, &touchedEpoch);
        if (mapFields >= 2) {
            // Save files written before this field existed have no epoch —
            // treat those as freshly touched rather than stale, so an
            // upgrade doesn't silently wipe existing spawn memory.
            const int64_t effectiveTouched = (mapFields >= 3) ? (int64_t)touchedEpoch : nowEpoch;
            if (nowEpoch - effectiveTouched > kStaleMapMaxAgeSeconds) {
                curMap = 0;  // sentinel: bucket lines below get skipped until the next "map" line
                ++skippedStaleMaps;
                continue;
            }
            curMap = m;
            g_maps[curMap].observations = obs;
            g_maps[curMap].lastTouched = effectiveTouched;
            continue;
        }
        int bx = 0, by = 0; float score = 0.0f;
        if (curMap != 0 && sscanf_s(line, "%d %d %f", &bx, &by, &score) == 3 && score >= kPruneBelow) {
            MapMemory& mm = g_maps[curMap];
            if ((int)mm.buckets.size() >= kMaxBucketsPerMap)
                continue;
            mm.buckets[PackBucket(bx, by)] = score;
            if (score > mm.maxScore) mm.maxScore = score;
            ++loaded;
        }
    }
    fclose(f);
    spdlog::info("[spawnmem] loaded {} buckets across {} maps ({} stale map(s) dropped, unseen for 30+ days)",
        loaded, (int)g_maps.size(), skippedStaleMaps);
}

} // namespace SpawnMemory

void MaybeAutoSaveSpawnMemory()
{
    static DWORD s_lastSave = 0;
    constexpr DWORD kSaveIntervalMs = 120000;   // 2 minutes
    const DWORD now = GetTickCount();
    if (s_lastSave != 0 && now - s_lastSave < kSaveIntervalMs)
        return;
    s_lastSave = now;
    SpawnMemory::Save();
}
