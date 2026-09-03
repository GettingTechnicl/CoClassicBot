#include "mapdata.h"
#include "game.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <utility>
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
    // ── Scene overlay: bridges and other standable structures ───────────────
    //
    // The base cell grid does NOT describe bridges. A real, walkable bridge
    // tile and genuinely-empty void are byte-identical in it (both mask=1,
    // terrain=0, altitude=0 — live-verified on map 1010, and again on Twin
    // City where the hero stood mid-bridge at (167,543) on a tile the grid
    // calls blocked). What makes those tiles walkable lives in the section
    // that follows the cell grid, which this parser used to stop before.
    //
    // TAIL LAYOUT (verified byte-for-byte on newbie.DMap and newplain.DMap):
    //     u32 portalCount
    //     portalCount * { i32 x, i32 y, i32 portalId }
    //     u32 layerCount
    //     layerCount records, each beginning with a u32 type:
    //         type  1  SCENE  : { u32, char sceneFile[260], i32 x, i32 y }  272 B  <-- walkability
    //         type  4  COVER  : 420 B  visual sprite, skipped
    //         type 10  EFFECT : { u32, char name[64], i32 x, i32 y }         76 B  skipped
    //         type 15  EFFECT2: { u32, char file[260], i32[4] }             280 B  skipped
    //
    // .SCENE LAYOUT — the overlay itself carries a per-cell walkability grid:
    //     u32 partCount, then per part:
    //       +0x000 char aniFile[256]
    //       +0x100 char aniTitle[64]
    //       +0x140 i32 offsetX, offsetY     sprite PIXEL offsets — NOT walkability
    //       +0x148 i32 aniInterval
    //       +0x14C i32 width, height        footprint in TILES
    //       +0x154 i32 thick
    //       +0x158 i32 partDX, partDY       this part's CELL offset from the anchor
    //       +0x164 cells: width*height * { i32 mask, i32 terrain, i32 altitude }
    //                                      row-major, idx = j*width + i
    //     part stride = 0x164 + width*height*12
    //
    // A scene at (x,y) with a part (dx,dy,w,h) covers tiles
    //     tileX in [ x + dx - w + 1 , x + dx ]
    //     tileY in [ y + dy - h + 1 , y + dy ]
    // i.e. the anchor is the part's BOTTOM-RIGHT cell, and part cell (i,j) maps
    // to ( x+dx-w+1+i , y+dy-h+1+j ).
    //
    // A LONG BRIDGE IS ONE SCENE WITH MANY PARTS. This is the thing an earlier
    // version of this parser got wrong and it is worth stating plainly, because
    // the wrong reading is superficially convincing: it read only part 0's
    // footprint, saw the eight parts' PIXEL offsets oscillating by a few px
    // around the same point (-166,-242 / -170,-246 / -172,-248 …), concluded
    // "animation frames", and threw the rest away. The parts are segments, and
    // it is partDX at +0x158 — never the pixel offsets — that positions them:
    // bridgeB-L's are -30,-16,-9,-23,-37,0,-44,-51, which with widths
    // 7,7,7,7,7,9,7,5 tile x[141..196] with zero gaps and zero overlaps, bank
    // to bank across the river. Animation frames cannot tile a span; they share
    // one position. The differing end textures (bridge05 left cap, bridge07
    // right cap, bridge06 repeated middle) say the same thing.
    //
    // Because each part ships its own per-cell mask, there is NO dilation and
    // NO gap-filling heuristic here any more. Both were compensating for the
    // footprint being read wrong, and both are deleted: they made map 1010's
    // bridge visibly too wide on one side ("pieces that are bad") while still
    // not covering the real deck. Map 1010's stand* platforms are simply the
    // degenerate case of this same format — one part, partDX = partDY = 0 —
    // not a separate mechanism.
    bool ReadWholeFile(const std::string& path, std::string* out);   // below

    struct ScenePart
    {
        int              dx = 0, dy = 0;
        int              w = 0, h = 0;
        std::vector<uint8_t> walkable;   // w*h, row-major, 1 = mask 0 = walkable
    };

    // Parse every part of a .scene. Cached: a map references only a handful of
    // distinct scenes, and this would otherwise re-read on every map change.
    const std::vector<ScenePart>& LoadSceneParts(const std::string& gameRoot,
                                                 const std::string& relPath)
    {
        static std::unordered_map<std::string, std::vector<ScenePart>> s_cache;
        auto it = s_cache.find(relPath);
        if (it != s_cache.end())
            return it->second;

        std::vector<ScenePart> parts;
        std::string norm = relPath;
        for (char& c : norm)
            if (c == '/') c = '\\';

        std::string blob;
        if (ReadWholeFile(gameRoot + "\\" + norm, &blob) && blob.size() >= 4) {
            uint32_t partCount = 0;
            memcpy(&partCount, blob.data(), 4);
            size_t off = 4;
            for (uint32_t p = 0; p < partCount; ++p) {
                if (off + 0x164 > blob.size())
                    break;
                int32_t pw = 0, ph = 0, pdx = 0, pdy = 0;
                memcpy(&pw,  blob.data() + off + 0x14C, 4);
                memcpy(&ph,  blob.data() + off + 0x150, 4);
                memcpy(&pdx, blob.data() + off + 0x158, 4);
                memcpy(&pdy, blob.data() + off + 0x15C, 4);
                // A part bigger than this isn't a structure, it's a misparse —
                // stop rather than walk a bogus stride through the rest.
                if (pw <= 0 || ph <= 0 || pw > 64 || ph > 64)
                    break;
                const size_t cells = (size_t)pw * (size_t)ph;
                const size_t need  = off + 0x164 + cells * 12;
                if (need > blob.size())
                    break;

                ScenePart sp;
                sp.dx = pdx; sp.dy = pdy; sp.w = pw; sp.h = ph;
                sp.walkable.resize(cells);
                for (size_t k = 0; k < cells; ++k) {
                    int32_t m = 0;
                    memcpy(&m, blob.data() + off + 0x164 + k * 12, 4);
                    sp.walkable[k] = (m == 0) ? 1 : 0;   // mask 0 = walkable
                }
                parts.push_back(std::move(sp));
                off = need;
            }
        }
        return s_cache.emplace(relPath, std::move(parts)).first->second;
    }

    struct SceneRef { int x, y; std::string path; };

    // Walk the tail's typed layer records and collect the type-1 SCENE ones.
    std::vector<SceneRef> ParseTailScenes(const std::string& data, size_t off,
                                          int width, int height, int* outLayers,
                                          std::vector<MapPortal>* outPortals)
    {
        std::vector<SceneRef> out;
        *outLayers = 0;
        if (outPortals) outPortals->clear();
        auto u32at = [&](size_t o) -> uint32_t {
            uint32_t v = 0; memcpy(&v, data.data() + o, 4); return v;
        };
        auto i32at = [&](size_t o) -> int32_t {
            int32_t v = 0; memcpy(&v, data.data() + o, 4); return v;
        };
        // Strings are fixed buffers padded with 0xCD (uninitialised editor
        // memory), so always read them as C strings.
        auto cstr = [&](size_t o, size_t cap) {
            size_t n = 0;
            while (n < cap && o + n < data.size() && data[o + n] != '\0') ++n;
            return data.substr(o, n);
        };

        if (off + 4 > data.size()) return out;
        const uint32_t portalCount = u32at(off);
        off += 4;
        if (portalCount > 4096) return out;            // implausible — bail
        if (off + (size_t)portalCount * 12 > data.size()) return out;
        for (uint32_t p = 0; p < portalCount; ++p) {   // { x, y, portalId }
            if (outPortals) {
                const int px = i32at(off), py = i32at(off + 4), pid = i32at(off + 8);
                if (px >= 0 && py >= 0 && px < width && py < height)
                    outPortals->push_back({px, py, pid});
            }
            off += 12;
        }
        if (off + 4 > data.size()) return out;

        const uint32_t layerCount = u32at(off);
        off += 4;
        if (layerCount > 100000) return out;
        *outLayers = (int)layerCount;

        for (uint32_t i = 0; i < layerCount; ++i) {
            if (off + 4 > data.size()) break;
            const uint32_t type = u32at(off);
            size_t recSize = 0;
            switch (type) {
                case 1:  recSize = 272; break;
                case 4:  recSize = 420; break;
                case 10: recSize = 76;  break;
                case 15: recSize = 280; break;
                default:
                    spdlog::warn("[mapdata] unknown tail record type {} at +{} "
                                 "({} of {} layers read) — stopping scan",
                                 type, (int)off, i, layerCount);
                    return out;
            }
            if (off + recSize > data.size()) break;
            if (type == 1) {
                const std::string file = cstr(off + 4, 260);
                const int sx = i32at(off + 4 + 260);
                const int sy = i32at(off + 4 + 264);
                if (!file.empty() && sx >= 0 && sy >= 0 && sx < width && sy < height)
                    out.push_back({sx, sy, file});
            }
            off += recSize;
        }
        return out;
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
    if (!FindMapFileInJson(json, mapId, &rel)) {
        // MAP ID != DOCUMENT ID. The client keeps two ids per map
        // (CGameMap::m_idMap at +0x200, m_idDoc at +0x204): the server-side
        // map id, and the document id that names the .DMap in GameMap.json.
        // They coincide for every main city, which is why resolving by map id
        // worked for so long — but instanced/duplicate maps reuse one file
        // under many map ids, and GameMap.json only lists the document id.
        // Without this the bot loads NO terrain there ("no .DMap entry"),
        // which is exactly what happened on Frozen Grotto 1 and on five of
        // the Ape Mountain mine's lower floors (live logs, 2026-09-02).
        //
        // The proper source of truth is the document id the server sends in
        // its enter-map message (or a live read of m_idDoc once the real
        // CGameMap pointer is known); until then this table is filled by
        // evidence. FG1 is direct (icecrypt-lev1.DMap IS DocumentId 1762).
        // The mine floors are INFERRED from the walk log: every floor that
        // lands the hero at (15,77) is entered the same way 2021 (Dgate) is,
        // and every floor landing at (72,15) matches 2024 (Dsigil) — the
        // dungeon is a tree of two repeated templates. Each use logs the
        // alias so a wrong guess is visible on the Map tab the first time
        // that floor is visited. Verify each on first visit; correct here.
        struct Alias { int mapId; int docId; const char* why; };
        static const Alias kAliases[] = {
            {1926, 1762, "Frozen Grotto 1 = icecrypt-lev1 (GameMap.json DocumentId 1762)"},
            {2025, 2021, "Ape mine floor, lands (15,77) like 2021 Dgate  [inferred]"},
            {2031, 2021, "Ape mine floor, lands (15,77) like 2021 Dgate  [inferred]"},
            {2033, 2021, "Ape mine floor, lands (15,77) like 2021 Dgate  [inferred]"},
            {2030, 2024, "Ape mine floor, lands (72,15) like 2024 Dsigil [inferred]"},
            {2032, 2024, "Ape mine floor, lands (72,15) like 2024 Dsigil [inferred]"},
            // Adventure Zone. 1217 is off canyon-fairy's south portal; the hero
            // landed at (363,13) and left from (360,6) — the exit position every
            // new* city duplicate shares (newcanyon #0 is (360,5)). In the canyon
            // branch, so newcanyon over newdesert. 1218 is the Meteor Zone mine
            // cave (user-confirmed); it landed the hero at (29,70), which is the
            // 184x184 mine template's entry (Phoenix's mine-one landed at (32,70)).
            // The client ships four unfiled mine variants mine-a..d (1500-1503,
            // all 184x184, no tile portals); which one is a guess — mine-a.
            {1217, 1075, "Adventure Zone 7.2, exits at (360,6) like newcanyon      [inferred]"},
            {1218, 1500, "Meteor Zone mine cave, lands (29,70) = 184x184 mine entry [inferred: mine-a]"},
            // 1219: reached from Adventure Islands I0 portal #11; the hero was
            // at (1010,1237) on it. Exactly ONE client file has that tile
            // walkable — island-snail, with all 49 neighbours open — so this is
            // a second instance of Adventure Islands. Strongest alias in the
            // table after FG1.
            {1219, 1212, "Adventure Islands 2nd instance: (1010,1237) walkable ONLY in island-snail"},
        };
        bool aliased = false;
        for (const Alias& a : kAliases) {
            if (a.mapId == mapId && FindMapFileInJson(json, a.docId, &rel)) {
                spdlog::warn("[mapdata] map {} has no GameMap.json entry; using map {}'s file "
                             "({}) — {}", mapId, a.docId, rel, a.why);
                aliased = true;
                break;
            }
        }
        if (!aliased)
            return {};
    }

    for (char& c : rel)
        if (c == '/') c = '\\';
    return root + "\\" + rel;
}

bool MapGrid::ParseFile(const std::string& path, int* w, int* h, std::vector<Cell>* out,
                        std::vector<MapPortal>* portals)
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

    // Merge the scene overlay onto the base grid. Each part carries its own
    // per-cell mask, so this is a direct copy of the game's own data — no
    // dilation, no gap filling, no heuristics.
    //
    // Only mask==0 (walkable) cells are applied. A part's mask==1 cells are
    // rails/edges; they are deliberately NOT used to block base-walkable land.
    // The game does treat the overlay as authoritative, but the one piece of
    // this format still unverified live is whether a part's cell pattern can be
    // MIRRORED within its footprint (which corner a rail sits on). Until that
    // is confirmed on an asymmetric part, wrongly opening a tile costs a step
    // the server refuses, while wrongly blocking one would break routing over
    // land that works today. Revisit once mirroring is checked in-game.
    //
    // Terrain and altitude stay as the base cell's: a deck reads flat, and
    // CanReach's altitude stepping should keep working off the ground beneath.
    int layerCount = 0;
    const std::vector<SceneRef> scenes =
        ParseTailScenes(data, needed, (int)width, (int)height, &layerCount, portals);
    const std::string gameRoot = MapGrid::GetGameRoot();

    int partsApplied = 0, tilesOpened = 0;
    for (const SceneRef& sr : scenes) {
        for (const ScenePart& sp : LoadSceneParts(gameRoot, sr.path)) {
            ++partsApplied;
            const int baseX = sr.x + sp.dx - sp.w + 1;
            const int baseY = sr.y + sp.dy - sp.h + 1;
            for (int j = 0; j < sp.h; ++j) {
                for (int i = 0; i < sp.w; ++i) {
                    if (!sp.walkable[(size_t)j * sp.w + i])
                        continue;
                    const int tx = baseX + i;
                    const int ty = baseY + j;
                    if (tx < 0 || ty < 0 || tx >= (int)width || ty >= (int)height)
                        continue;
                    Cell& cell = (*out)[(size_t)ty * width + (size_t)tx];
                    if (cell.mask == 1) {
                        cell.mask = 0;
                        ++tilesOpened;
                    }
                }
            }
        }
    }
    if (!scenes.empty()) {
        spdlog::info("[mapdata] scene overlay: {} layer records, {} scenes, "
                     "{} parts, {} tiles opened",
                     layerCount, (int)scenes.size(), partsApplied, tilesOpened);
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
    std::vector<MapPortal> portals;
    if (!ParseFile(file, &w, &h, &cells, &portals)) {
        spdlog::error("[mapdata] failed to parse '{}'", file);
        return false;
    }

    m_cells.swap(cells);
    m_portals.swap(portals);
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
