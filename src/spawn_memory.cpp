#include "spawn_memory.h"

#include <windows.h>
#include <cstdio>
#include <ctime>
#include <mutex>
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

void Observe(OBJID mapId, const std::vector<Position>& monsterTiles)
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

    for (const Position& p : monsterTiles) {
        if (p.x <= 0 || p.y <= 0)
            continue;
        const uint32_t key = PackBucket(p.x / kBucketTiles, p.y / kBucketTiles);
        auto [it, inserted] = mm.buckets.try_emplace(key, 0.0f);
        if (inserted && (int)mm.buckets.size() > kMaxBucketsPerMap) {
            // Safety rail only — decay should keep us far below this.
            mm.buckets.erase(it);
            continue;
        }
        it->second += 1.0f;
        if (it->second > newMax) newMax = it->second;
    }
    mm.maxScore = newMax;
}

float GetScore(OBJID mapId, const Position& tile)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    auto mit = g_maps.find((uint32_t)mapId);
    if (mit == g_maps.end())
        return 0.0f;
    auto bit = mit->second.buckets.find(PackBucket(tile.x / kBucketTiles, tile.y / kBucketTiles));
    return (bit == mit->second.buckets.end()) ? 0.0f : bit->second;
}

float GetMaxScore(OBJID mapId)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_maps.find((uint32_t)mapId);
    return (it == g_maps.end()) ? 0.0f : it->second.maxScore;
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
