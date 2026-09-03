#pragma once
#include "base.h"
#include <vector>
#include <memory>

// =====================================================================
// LayerInfo — per-cell terrain layer (linked list for multi-layer cells)
//
// Natural alignment (NOT packed) — these are heap-allocated game structs.
// sizeof(LayerInfo) = 16: [terrain:2][mask:2][altitude:2][pad:2][next:8]
// =====================================================================
struct LayerInfo
{
    uint16_t terrain;           // terrain type ID
    uint16_t mask;              // blocking mask (1 = blocked/non-walkable)
    int16_t  altitude;          // height/elevation
    LayerInfo* next;            // next layer in chain (nullptr if last)
};

static_assert(sizeof(LayerInfo) == 16, "LayerInfo must be 16 bytes (natural alignment)");

// =====================================================================
// CellInfo — single map cell (first layer + pathfinding flag)
//
// sizeof(CellInfo) = 24: [LayerInfo:16][searched:1][pad:7]
// =====================================================================
struct CellInfo
{
    LayerInfo layer;            // first (bottom) layer
    bool      searched;         // pathfinding visited flag
};

static_assert(sizeof(CellInfo) == 24, "CellInfo must be 24 bytes (natural alignment)");

// =====================================================================
// CGameMap — map struct for camera/viewport/cell access
//
// Located at base + 0x4E02E0.
// Offsets verified via Cheat Engine + Rust codebase.
// =====================================================================

// =====================================================================
// CMapItem — ground item on the map (dropped loot)
//
// Stored in CGameMap at +0x178 as std::vector<std::shared_ptr<CMapItem>>.
// Offsets from Rust conquer-bot: crates/api/src/items.rs
// =====================================================================
struct CMapItem
{
    OBJID    m_id;              // +0x00  item instance UID
    OBJID    m_idType;          // +0x04  item type ID
    Position m_pos;             // +0x08  tile position (x, y)
    void*    m_pInfo;           // +0x10  MapItemInfo* (rendering data)
    void*    m_pInfoCtrl;       // +0x18  shared_ptr control block

    int  GetQuality() const { return m_idType % 10; }
    // Plus level. MapItemInfo layout (Session 14, live-verified via the
    // "Dump nearest (plus RE)" tool on a stable +0 BreastPlate and +1
    // BroadSword): +0x44 = the item's own type id, +0x48 = the plus (0-N).
    //
    // Session 10 [CRASH HARDENING]: this dereferenced m_pInfo unguarded.
    // m_pInfo (+0x10) is NOT part of the heap-scanner's signature check
    // (map_items.cpp validates id/idType/position, not this field), so a
    // false-positive scan hit — or a genuine item whose m_pInfo hasn't been
    // populated yet, or an item mid-teardown — could read an arbitrary
    // pointer here with no safety net. SEH-guarded; a bad read returns 0.
    //
    // Session 14 [SELF-VALIDATING READ]: the raw +0x48 read was live-proven
    // CORRECT for stable items (0 for a +0, 1 for a +1) but earlier read
    // implausible garbage (80, 64) during fast live hunting — the ground-item
    // heap entries churn/reuse constantly, so GetPlus() was occasionally
    // reading +0x48 of a stale-or-reused MapItemInfo rather than this item's.
    // Fixed by validating the pointer before trusting the plus: read the type
    // id the MapItemInfo stores at +0x44 and require it to equal THIS item's
    // m_idType. A stale/reused/half-populated m_pInfo won't match (its +0x44
    // holds some other item's type, or garbage), so the bad read is rejected
    // precisely — including garbage that would happen to land in a plausible
    // 1-N range, which a value-range clamp alone would miss. Belt-and-braces:
    // a plausibility clamp too, since real plus never exceeds kMaxRealPlus.
    // On any mismatch/unreadable/implausible read, returns 0 (= "not plussed",
    // the safe direction: miss a real +1 rather than loot garbage).
    static constexpr int kMaxRealPlus = 12;
    uint8_t GetPlus() const {
        if (!m_pInfo) return 0;
        __try {
            const uintptr_t info = (uintptr_t)m_pInfo;
            const uint32_t infoType = *(volatile uint32_t*)(info + 0x44);
            if (infoType != m_idType) return 0;  // stale/reused m_pInfo — not our item
            const uint8_t plus = *(volatile uint8_t*)(info + 0x48);
            if (plus > kMaxRealPlus) return 0;   // implausible — a bad read
            return plus;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }
};

static_assert(offsetof(CMapItem, m_id)     == 0x00, "CMapItem::m_id");
static_assert(offsetof(CMapItem, m_idType) == 0x04, "CMapItem::m_idType");
static_assert(offsetof(CMapItem, m_pos)    == 0x08, "CMapItem::m_pos");

#pragma pack(push, 1)
class CGameMap
{
private:
    BYTE _pad00[0x30];
public:
    Size m_sizeMap;          // +0x30  map dimensions in tiles
    Size m_sizeWorld;        // +0x38  world dimensions in pixels
private:
    BYTE _pad40[0x04];
public:
    Position m_posCameraPos;      // +0x44  isometric origin offset
    Position m_posViewport;       // +0x4C  camera scroll (viewport)
private:
    BYTE _pad54[0x04];
public:
    CellInfo* m_pCellInfo;      // +0x58  pointer to cell array [width * height]
private:
    BYTE _pad60[0x178 - 0x60];  // +0x60  gap to ground items vector
public:
    // +0x178 ground items on map. Offset-verified against the real game
    // struct, but the game itself NEVER POPULATES this field on v1074 — see
    // map_items.h's header comment. Ground items are tracked separately via
    // a heap-scanner (MapItems::Get()/IsAlive()), which is what every
    // consumer in this codebase actually uses. Do not read this field
    // expecting live data; it exists here only to keep the struct's offset
    // layout below (m_idMap etc.) correct.
    std::vector<std::shared_ptr<CMapItem>> m_vecItems;
private:
    BYTE _pad198[0x200 - 0x178 - sizeof(std::vector<std::shared_ptr<CMapItem>>)]; // gap to map ID
public:
    OBJID m_idMap;               // +0x200 current map ID (e.g. 1002 = Twin City)
    OBJID m_idDoc;               // +0x204 map document ID

    OBJID GetId() const { return m_idMap; }

    // Get cell at tile coordinates (bounds-checked)
    CellInfo* GetCell(int x, int y) const {
        if (x < 0 || x >= m_sizeMap.iWidth || y < 0 || y >= m_sizeMap.iHeight)
            return nullptr;
        if (!m_pCellInfo) return nullptr;
        return &m_pCellInfo[x + y * m_sizeMap.iWidth];
    }

    // Get the topmost layer for a cell (follows linked list)
    static const LayerInfo* GetLastLayer(const CellInfo* cell) {
        if (!cell) return nullptr;
        const LayerInfo* layer = &cell->layer;
        while (layer->next)
            layer = layer->next;
        return layer;
    }

    // Get mask for a cell (1 = blocked)
    static uint16_t GetMask(const CellInfo* cell) {
        const LayerInfo* layer = GetLastLayer(cell);
        return layer ? layer->mask : 0;
    }

    // Check if a cell is walkable.
    //
    // Bridges and other standable structures are NOT in the base terrain
    // grid — they arrive via the .DMap's scene-placement section, which
    // MapGrid::ParseFile now parses and folds into the cell masks before
    // this ever runs (see mapdata.cpp's scene-overlay block). So a plain
    // mask test is correct again; there is no override table.
    bool IsWalkable(int x, int y) const {
        return GetMask(GetCell(x, y)) != 1;
    }

    // Get altitude of a cell (topmost layer)
    static int16_t GetAltitude(const CellInfo* cell) {
        const LayerInfo* layer = GetLastLayer(cell);
        return layer ? layer->altitude : 0;
    }

    // Get terrain type of a cell (topmost layer)
    static uint16_t GetTerrain(const CellInfo* cell) {
        const LayerInfo* layer = GetLastLayer(cell);
        return layer ? layer->terrain : 0;
    }

    // Rounded integer distance (matches game formula, size=1)
    static int TileDist(int x0, int y0, int x1, int y1);

    // Bresenham line-of-sight: check altitude steps along the path
    bool CanReach(int ox, int oy, int tx, int ty, int altThreshold = 200) const;

    // Full jump validation: distance + walkability + altitude path
    static constexpr int MAX_JUMP_DIST = 18;
    static constexpr int ALT_THRESHOLD_NORMAL = 200;
    static constexpr int ALT_THRESHOLD_FLYING = 100;

    // Returns the correct altitude threshold based on the hero's fly state.
    static int GetHeroAltThreshold();

    bool CanJump(int ox, int oy, int tx, int ty, int altThreshold = ALT_THRESHOLD_NORMAL) const;

    // A* tile-level pathfinding (8-directional, respects walkability + altitude)
    // Returns empty vector if no path found. Includes start and goal.
    std::vector<Position> FindPath(int ox, int oy, int tx, int ty, int maxIter = 50000) const;

    // Every tile reachable from (ox,oy) under EXACTLY FindPath's rules
    // (8-directional, walkable, altitude step <= 200), as a w*h byte map
    // (1 = reachable). One BFS; use it to test many destinations at once —
    // e.g. "which of these 100 tiles around an NPC can I actually get to" —
    // instead of running A* per destination, which exhausts a whole connected
    // region on every sealed-off candidate. maxNodes bounds the flood.
    std::vector<uint8_t> FloodReachable(int ox, int oy, int maxNodes = 200000) const;

    // Simplify a tile path into jump waypoints (greedy: largest valid jumps)
    // Returns waypoints excluding the start position.
    std::vector<Position> SimplifyPath(const std::vector<Position>& tilePath) const;

    // Dump map cell data (mask + altitude per cell) to a binary file.
    // Format: [uint32 width][uint32 height][uint32 mapId][per cell: uint16 mask, int16 altitude]
    bool DumpToFile(const char* path) const;
};
#pragma pack(pop)

static_assert(offsetof(CGameMap, m_sizeMap)      == 0x30, "CGameMap::m_sizeMap");
static_assert(offsetof(CGameMap, m_posCameraPos) == 0x44, "CGameMap::m_posCameraPos");
static_assert(offsetof(CGameMap, m_posViewport)  == 0x4C, "CGameMap::m_posViewport");
static_assert(offsetof(CGameMap, m_pCellInfo)    == 0x58, "CGameMap::m_pCellInfo");
static_assert(offsetof(CGameMap, m_vecItems)     == 0x178, "CGameMap::m_vecItems");
static_assert(offsetof(CGameMap, m_idMap)        == 0x200, "CGameMap::m_idMap");
static_assert(offsetof(CGameMap, m_idDoc)        == 0x204, "CGameMap::m_idDoc");
