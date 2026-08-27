#include "mapdata.h"
#include "game.h"

#include <windows.h>
#include <cstdio>
#include <spdlog/spdlog.h>

namespace
{
    // Minimal scan of ini/GameMap.json for a DocumentId -> FileName pair.
    // Deliberately does not pull in a JSON library: the file is a flat array
    // of {"DocumentId": N, "FileName": "..."} objects and this runs once per
    // map change.
    bool FindMapFileInJson(const std::string& json, int mapId, std::string* out)
    {
        char needle[64];
        snprintf(needle, sizeof(needle), "\"DocumentId\": %d,", mapId);
        size_t pos = json.find(needle);
        if (pos == std::string::npos) {
            // tolerate no-space formatting
            snprintf(needle, sizeof(needle), "\"DocumentId\":%d,", mapId);
            pos = json.find(needle);
            if (pos == std::string::npos)
                return false;
        }
        const size_t fn = json.find("\"FileName\"", pos);
        if (fn == std::string::npos) return false;
        const size_t q1 = json.find('"', json.find(':', fn) + 1);
        if (q1 == std::string::npos) return false;
        const size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos) return false;
        *out = json.substr(q1 + 1, q2 - q1 - 1);
        return true;
    }

    bool ReadWholeFile(const std::string& path, std::string* out)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
            return false;
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n <= 0) { fclose(f); return false; }
        out->resize((size_t)n);
        const size_t got = fread(&(*out)[0], 1, (size_t)n, f);
        fclose(f);
        out->resize(got);
        return got > 0;
    }
}

std::string MapGrid::GetGameRoot()
{
    static std::string cached;
    if (!cached.empty())
        return cached;

    // The client lives at <root>\bin\64\ImConquer.exe, so the install root is
    // two directories above the executable.
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
        return cached;

    std::string p(path);
    for (int i = 0; i < 3; ++i) {           // strip exe, then "64", then "bin"
        const size_t slash = p.find_last_of("\\/");
        if (slash == std::string::npos) return cached;
        p = p.substr(0, slash);
    }
    cached = p;
    return cached;
}

std::string MapGrid::ResolveMapFile(int mapId)
{
    const std::string root = GetGameRoot();
    if (root.empty()) return {};

    static std::string json;
    if (json.empty() && !ReadWholeFile(root + "\\ini\\GameMap.json", &json)) {
        spdlog::error("[mapdata] cannot read ini/GameMap.json under '{}'", root);
        return {};
    }

    std::string rel;
    if (!FindMapFileInJson(json, mapId, &rel))
        return {};

    for (char& c : rel)
        if (c == '/') c = '\\';
    return root + "\\" + rel;
}

bool MapGrid::ParseFile(const std::string& path, int* w, int* h, std::vector<Cell>* out)
{
    std::string data;
    if (!ReadWholeFile(path, &data))
        return false;
    if (data.size() < 0x114)
        return false;

    uint32_t width = 0, height = 0;
    memcpy(&width, data.data() + 0x10C, 4);
    memcpy(&height, data.data() + 0x110, 4);
    if (width == 0 || height == 0 || width > 4096 || height > 4096)
        return false;

    // Row stride includes the per-row checksum that follows each row's cells.
    const size_t rowBytes = (size_t)width * 6 + 4;
    const size_t needed = 0x114 + (size_t)height * rowBytes;
    if (data.size() < needed)
        return false;

    out->resize((size_t)width * (size_t)height);
    for (uint32_t y = 0; y < height; ++y) {
        const char* row = data.data() + 0x114 + (size_t)y * rowBytes;
        Cell* dst = out->data() + (size_t)y * width;
        for (uint32_t x = 0; x < width; ++x) {
            const char* c = row + (size_t)x * 6;
            memcpy(&dst[x].mask, c, 2);
            memcpy(&dst[x].terrain, c + 2, 2);
            memcpy(&dst[x].altitude, c + 4, 2);
        }
    }
    *w = (int)width;
    *h = (int)height;
    return true;
}

bool MapGrid::Load(int mapId)
{
    if (m_loaded && m_mapId == mapId)
        return true;

    const std::string file = ResolveMapFile(mapId);
    if (file.empty()) {
        spdlog::warn("[mapdata] no .DMap entry for map id {}", mapId);
        return false;
    }

    int w = 0, h = 0;
    std::vector<Cell> cells;
    if (!ParseFile(file, &w, &h, &cells)) {
        spdlog::error("[mapdata] failed to parse '{}'", file);
        return false;
    }

    m_cells.swap(cells);
    m_width = w;
    m_height = h;
    m_mapId = mapId;
    m_file = file;
    m_loaded = true;

    size_t walkable = 0;
    for (const Cell& c : m_cells)
        if (c.mask == 0) ++walkable;
    spdlog::info("[mapdata] map {} loaded: {}x{} ({} cells, {:.1f}% walkable) from {}",
                 mapId, w, h, m_cells.size(),
                 m_cells.empty() ? 0.0 : 100.0 * (double)walkable / (double)m_cells.size(),
                 file);
    return true;
}

CGameMap* GetFileBackedGameMap()
{
    static CGameMap         s_map;
    static std::vector<CellInfo> s_cells;
    static int              s_builtFor = -1;

    MapGrid* grid = GetCurrentMapGrid();
    if (!grid || !grid->IsLoaded())
        return nullptr;

    if (s_builtFor != grid->GetMapId()) {
        const int w = grid->GetWidth();
        const int h = grid->GetHeight();
        s_cells.assign((size_t)w * (size_t)h, CellInfo{});
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const MapGrid::Cell* c = grid->GetCell(x, y);
                CellInfo& dst = s_cells[(size_t)y * (size_t)w + (size_t)x];
                dst.layer.terrain  = c ? c->terrain : 0;
                dst.layer.mask     = c ? c->mask : 1;      // missing => blocked
                dst.layer.altitude = c ? c->altitude : 0;
                dst.layer.next     = nullptr;              // single layer
                dst.searched       = false;
            }
        }
        s_map.m_sizeMap.iWidth  = w;
        s_map.m_sizeMap.iHeight = h;
        s_map.m_pCellInfo       = s_cells.data();
        s_map.m_idMap           = (OBJID)grid->GetMapId();
        s_builtFor              = grid->GetMapId();
        spdlog::info("[mapdata] built CGameMap view for map {} ({}x{}, {} cells)",
                     s_builtFor, w, h, (int)s_cells.size());
    }
    return &s_map;
}

MapGrid* GetCurrentMapGrid()
{
    static MapGrid grid;
    // Session 12: without this, a map id whose .DMap can't be resolved or
    // parsed (missing file, bad JSON entry, corrupt data) was re-resolved
    // and re-parsed from disk on EVERY call to this function — which runs
    // every frame from pathfinding consumers — spamming warn/error logs and
    // disk I/O for as long as the hero stayed on that map. Remembers the
    // last-failed id and only retries after a cooldown, rather than caching
    // the failure forever (a transient cause, e.g. a network drive hiccup,
    // isn't impossible even if unlikely for static installed content).
    static int   s_lastFailedId = 0;
    static DWORD s_lastFailTick = 0;
    constexpr DWORD kFailRetryCooldownMs = 30000;

    const OBJID id = Game::GetCurrentMapId();
    if (!id)
        return grid.IsLoaded() ? &grid : nullptr;   // keep last map during a transition

    if (!grid.IsLoaded() || grid.GetMapId() != (int)id) {
        const DWORD now = GetTickCount();
        if ((int)id == s_lastFailedId && now - s_lastFailTick < kFailRetryCooldownMs)
            return grid.IsLoaded() ? &grid : nullptr;

        if (!grid.Load((int)id)) {
            s_lastFailedId = (int)id;
            s_lastFailTick = now;
            return grid.IsLoaded() ? &grid : nullptr;
        }
        s_lastFailedId = 0;
    }
    return &grid;
}
