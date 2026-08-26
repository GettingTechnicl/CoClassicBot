#pragma once
// =====================================================================
// mapdata.h — terrain loaded from the client's own .DMap files.
//
// WHY FROM DISK
// -------------
// The in-memory map was a dead end: Offsets::GAME_MAP is confirmed garbage
// on v1074, the client stores cell data in blocks rather than one flat array,
// and it does not memory-map the files. But every map's walkability, terrain
// and altitude ship on disk in a format we decoded and validated, so there is
// nothing to reverse-engineer here at all.
//
// This is also the more durable choice: file formats survive client patches,
// RVAs do not. The only thing still read from game memory is the current map
// id (Game::GetCurrentMapId(), a direct u32 global).
//
// FORMAT (validated against newplain/desert/canyon/island/mine01/forum)
//   u32   version
//   u32   (padding)
//   char  puzzlePath[260]
//   u32   width           @ 0x10C
//   u32   height          @ 0x110
//   cells from 0x114: { u16 mask, u16 terrain, s16 altitude }  = 6 bytes
//   ...with a 4-byte checksum after EVERY ROW (row stride = width*6 + 4)
//   further sections follow the cell data
//
// mask 0 = walkable, 1 = blocked. Verified against six confirmed Twin City
// hero positions (all 0) and the map border (1).
// =====================================================================
#include "base.h"
#include <cstdint>
#include <string>
#include <vector>

class MapGrid
{
public:
    struct Cell
    {
        uint16_t mask;      // 0 = walkable, 1 = blocked
        uint16_t terrain;
        int16_t  altitude;
    };

    // Load the .DMap for a map id, resolved through the client's own
    // ini/GameMap.json. Returns false if the id is unknown or the file is
    // missing/malformed. Reloading the same id is a no-op.
    bool Load(int mapId);

    bool  IsLoaded()  const { return m_loaded; }
    int   GetMapId()  const { return m_mapId; }
    int   GetWidth()  const { return m_width; }
    int   GetHeight() const { return m_height; }
    const std::string& GetFile() const { return m_file; }

    bool InBounds(int x, int y) const {
        return x >= 0 && y >= 0 && x < m_width && y < m_height;
    }

    const Cell* GetCell(int x, int y) const {
        if (!InBounds(x, y)) return nullptr;
        return &m_cells[(size_t)y * (size_t)m_width + (size_t)x];
    }

    // Out-of-bounds counts as blocked: callers treat it as impassable, which
    // is the safe answer for pathfinding.
    bool IsWalkable(int x, int y) const {
        const Cell* c = GetCell(x, y);
        return c && c->mask == 0;
    }

    int GetAltitude(int x, int y) const {
        const Cell* c = GetCell(x, y);
        return c ? (int)c->altitude : 0;
    }

    // Parse a .DMap directly (used by the loader and by tests).
    static bool ParseFile(const std::string& path, int* w, int* h, std::vector<Cell>* out);

    // Absolute path of the game install, derived from the running module.
    static std::string GetGameRoot();

    // Map id -> .DMap path, via ini/GameMap.json. Empty if unknown.
    static std::string ResolveMapFile(int mapId);

private:
    bool              m_loaded = false;
    int               m_mapId  = 0;
    int               m_width  = 0;
    int               m_height = 0;
    std::string       m_file;
    std::vector<Cell> m_cells;
};

// Terrain for the map the hero is currently on. Reloads automatically when
// Game::GetCurrentMapId() changes. Returns nullptr until a map is loaded.
MapGrid* GetCurrentMapGrid();

// A CGameMap view over the file-loaded terrain.
//
// Rather than rewrite ~20 call sites and duplicate the pathfinding
// algorithms, this builds a real CGameMap in OUR memory from the .DMap data:
// a CellInfo array whose layers carry the file's mask/altitude, plus the
// dimensions and map id. Every existing consumer — CanReach, CanJump,
// FindPath, the plugins, the overlay — then works unchanged, and the 29
// existing map tests keep exercising the same code paths.
//
// Returns nullptr until a map is loaded. Rebuilt automatically on map change.
class CGameMap;
CGameMap* GetFileBackedGameMap();
