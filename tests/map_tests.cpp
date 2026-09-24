#include "CGameMap.h"
#include "mapdata.h"
#include "build_fence.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>

// Stub for base.h global (not used by CGameMap algorithms)
ULONG64 g_qwModuleBase = 0;

// =====================================================================
// TestMap — helper to build a CGameMap with custom cell data
// =====================================================================
class TestMap {
public:
    TestMap(int width, int height)
        : m_width(width), m_height(height)
    {
        m_cells.resize(width * height);
        memset(m_cells.data(), 0, m_cells.size() * sizeof(CellInfo));
        BuildGameMap();
    }

    // Load from a dump file produced by CGameMap::DumpToFile
    static TestMap LoadDump(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "Failed to open dump: %s\n", path);
            exit(1);
        }
        uint32_t w, h, id;
        fread(&w, 4, 1, f);
        fread(&h, 4, 1, f);
        fread(&id, 4, 1, f);

        TestMap tm((int)w, (int)h);
        tm.m_mapId = id;
        for (uint32_t i = 0; i < w * h; ++i) {
            uint16_t mask;
            int16_t alt;
            fread(&mask, 2, 1, f);
            fread(&alt, 2, 1, f);
            tm.m_cells[i].layer.mask = mask;
            tm.m_cells[i].layer.altitude = alt;
            tm.m_cells[i].layer.terrain = 0;
            tm.m_cells[i].layer.next = nullptr;
        }
        fclose(f);

        tm.BuildGameMap();
        return tm;
    }

    void SetCell(int x, int y, uint16_t mask, int16_t altitude) {
        if (x >= 0 && x < m_width && y >= 0 && y < m_height) {
            CellInfo& c = m_cells[x + y * m_width];
            c.layer.mask = mask;
            c.layer.altitude = altitude;
        }
    }

    // Fill all cells as walkable at given altitude
    void FillAll(int16_t altitude, uint16_t mask = 0) {
        for (auto& c : m_cells) {
            c.layer.altitude = altitude;
            c.layer.mask = mask;
        }
        BuildGameMap();
    }

    // Set a rectangular region
    void FillRect(int x0, int y0, int x1, int y1, uint16_t mask, int16_t altitude) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                SetCell(x, y, mask, altitude);
    }

    CGameMap* Get() { return &m_map; }

private:
    void BuildGameMap() {
        memset(&m_map, 0, sizeof(m_map));
        m_map.m_sizeMap.iWidth = m_width;
        m_map.m_sizeMap.iHeight = m_height;
        m_map.m_pCellInfo = m_cells.data();
        m_map.m_idMap = m_mapId;
    }

    int m_width, m_height;
    uint32_t m_mapId = 0;
    std::vector<CellInfo> m_cells;
    CGameMap m_map;
};

// =====================================================================
// Test helpers
// =====================================================================
static int g_testsPassed = 0;
static int g_testsFailed = 0;

static int g_testsSkipped = 0;

// Thrown by tests that need the real game files (see RealMap below) when the
// install isn't present, so a machine without the client skips them instead of
// failing.
struct TestSkip {};

#define TEST(name) static void test_##name()
#define RUN(name) do { \
    printf("  %-50s", #name); \
    try { test_##name(); printf("PASS\n"); g_testsPassed++; } \
    catch (const TestSkip&) { printf("SKIP (game files not found)\n"); g_testsSkipped++; } \
    catch (...) { printf("FAIL\n"); g_testsFailed++; } \
} while(0)

#define EXPECT(expr) do { if (!(expr)) { \
    fprintf(stderr, "    FAILED: %s (line %d)\n", #expr, __LINE__); \
    throw 0; \
}} while(0)

// =====================================================================
// Unit tests — flat map (no altitude variation)
// =====================================================================

TEST(jump_same_tile_rejected) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    auto* m = tm.Get();
    EXPECT(!m->CanJump(10, 10, 10, 10));
}

TEST(jump_within_range) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    auto* m = tm.Get();
    EXPECT(m->CanJump(10, 10, 20, 10));    // dist 10
    EXPECT(m->CanJump(10, 10, 28, 10));    // dist 18 = MAX_JUMP_DIST
}

TEST(jump_out_of_range) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    auto* m = tm.Get();
    EXPECT(!m->CanJump(0, 0, 19, 0));      // dist 19 > 18
    EXPECT(!m->CanJump(0, 0, 0, 40));      // dist 40
}

TEST(jump_blocked_destination) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    tm.SetCell(20, 10, 1, 0);  // blocked
    auto* m = tm.Get();
    EXPECT(!m->CanJump(10, 10, 20, 10));
}

TEST(jump_out_of_bounds) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    auto* m = tm.Get();
    EXPECT(!m->CanJump(0, 0, -1, 0));
    EXPECT(!m->CanJump(0, 0, 0, 50));
}

// =====================================================================
// Altitude tests — platforms and cliffs
// =====================================================================

TEST(altitude_same_level_ok) {
    TestMap tm(50, 50);
    tm.FillAll(100);
    auto* m = tm.Get();
    EXPECT(m->CanJump(5, 5, 10, 5));
    EXPECT(m->CanReach(5, 5, 10, 5));
}

TEST(altitude_small_diff_ok) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    // Gradual slope: each tile +50
    for (int x = 0; x < 50; ++x)
        for (int y = 0; y < 50; ++y)
            tm.SetCell(x, y, 0, (int16_t)(x * 50));
    auto* m = tm.Get();
    // Adjacent tiles differ by 50 <= 200, so short hops work
    EXPECT(m->CanJump(0, 5, 3, 5));        // delta = 150
    EXPECT(m->CanReach(0, 5, 3, 5));
}

TEST(altitude_cliff_blocks_jump) {
    // Ground at alt 0, platform at alt 500
    TestMap tm(50, 50);
    tm.FillAll(0);
    tm.FillRect(20, 0, 49, 49, 0, 500);  // platform on right half
    auto* m = tm.Get();
    // Jump from ground to platform: origin alt=0, dest alt=500, diff=500 > 200
    EXPECT(!m->CanJump(10, 10, 25, 10));
}

TEST(altitude_cliff_blocks_reach) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    tm.FillRect(20, 0, 49, 49, 0, 500);
    auto* m = tm.Get();
    EXPECT(!m->CanReach(10, 10, 25, 10));
}

TEST(altitude_jump_within_threshold) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    tm.FillRect(20, 0, 49, 49, 0, 200);  // exactly 200
    auto* m = tm.Get();
    // Line from (10,10) to (25,10) crosses into alt=200 at x=20
    // Origin alt=0, cell at x=20 alt=200, diff=200 <= 200: OK
    EXPECT(m->CanJump(10, 10, 25, 10));
}

TEST(altitude_jump_over_threshold) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    tm.FillRect(20, 0, 49, 49, 0, 201);  // just over
    auto* m = tm.Get();
    // diff=201 > 200: blocked
    EXPECT(!m->CanJump(10, 10, 25, 10));
}

// =====================================================================
// Corner-cutting tests — the key fix
// =====================================================================

TEST(corner_diagonal_with_wall) {
    // Layout (5x5):
    //   . . . . .
    //   . H . . .     H=hero at (1,1), alt 0
    //   . . W . .     W=wall at (2,2), alt 500
    //   . . . D .     D=destination at (3,3), alt 0
    //   . . . . .
    //
    // Bresenham (1,1)->(3,3) goes through (2,2) which has alt 500.
    // But cardinal neighbors (2,1) and (1,2) are at alt 0 — corner passage allowed.
    TestMap tm(10, 10);
    tm.FillAll(0);
    tm.SetCell(2, 2, 0, 500);  // wall corner with high altitude
    auto* m = tm.Get();
    EXPECT(m->CanReach(1, 1, 3, 3));
    EXPECT(m->CanJump(1, 1, 3, 3));
}

TEST(corner_both_cardinals_blocked) {
    // Same layout but both cardinal cells also high altitude → blocked
    TestMap tm(10, 10);
    tm.FillAll(0);
    tm.SetCell(2, 2, 0, 500);
    tm.SetCell(2, 1, 0, 500);  // block cardinal X
    tm.SetCell(1, 2, 0, 500);  // block cardinal Y
    auto* m = tm.Get();
    EXPECT(!m->CanReach(1, 1, 3, 3));
}

TEST(corner_one_cardinal_blocked_is_wall_edge) {
    // One cardinal also blocked — this is a wall edge, not a passable corner.
    // Both cardinals must be clear for the diagonal to be relaxed.
    TestMap tm(10, 10);
    tm.FillAll(0);
    tm.SetCell(2, 2, 0, 500);
    tm.SetCell(2, 1, 0, 500);  // block one cardinal
    // (1,2) is still at alt 0, but (2,1) is also a wall → blocked
    auto* m = tm.Get();
    EXPECT(!m->CanReach(1, 1, 3, 3));
}

TEST(corner_long_diagonal_with_wall_corner) {
    // Jump from (0,0) to (10,10) with a wall corner at (5,5)
    TestMap tm(20, 20);
    tm.FillAll(0);
    tm.SetCell(5, 5, 0, 500);
    auto* m = tm.Get();
    // Bresenham goes through (5,5), but (5,4) and (4,5) are alt 0
    EXPECT(m->CanReach(0, 0, 10, 10));
    EXPECT(m->CanJump(0, 0, 10, 10));
}

TEST(wall_edge_blocks_diagonal_clip) {
    // Reproduces the Mystic Castle case: wall along x=339 from y=572..575.
    // Jump from (338,574) to (343,564) — Bresenham clips wall at (339,572).
    // Cardinal (339,573) is also part of the wall → NOT a corner → blocked.
    TestMap tm(20, 20);
    tm.FillAll(0, 0);
    // Set all cells to alt 300
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 20; ++x)
            tm.SetCell(x, y, 0, 300);
    // Wall column at x=10 from y=5..12 (alt 5000, mask 1)
    for (int y = 5; y <= 12; ++y)
        tm.SetCell(10, y, 1, 5000);
    auto* m = tm.Get();
    // Jump that clips the wall diagonally: (9,11) to (12,3)
    // Bresenham will hit (10,y) cells that are part of the wall
    EXPECT(!m->CanJump(9, 11, 12, 3));
    // Jump that doesn't cross the wall should still work
    EXPECT(m->CanJump(5, 5, 8, 8));
}

TEST(corner_non_diagonal_step_not_relaxed) {
    // Non-diagonal step (only X or only Y advances) — no corner relaxation
    // Place a high-altitude wall in the direct line
    TestMap tm(20, 20);
    tm.FillAll(0);
    tm.SetCell(5, 5, 0, 500);  // wall in straight horizontal path
    auto* m = tm.Get();
    // Horizontal: (0,5) to (10,5) — Bresenham hits (5,5) as a cardinal step
    EXPECT(!m->CanReach(0, 5, 10, 5));
}

// =====================================================================
// Pathfinding tests
// =====================================================================

TEST(pathfind_straight_line) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    auto* m = tm.Get();
    auto path = m->FindPath(5, 5, 15, 5);
    EXPECT(!path.empty());
    EXPECT(path.front().x == 5 && path.front().y == 5);
    EXPECT(path.back().x == 15 && path.back().y == 5);
}

TEST(pathfind_around_wall) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    // Wall from (10,0) to (10,8) — blocks direct path
    for (int y = 0; y <= 8; ++y)
        tm.SetCell(10, y, 1, 0);
    auto* m = tm.Get();
    auto path = m->FindPath(5, 5, 15, 5);
    EXPECT(!path.empty());
    EXPECT(path.back().x == 15 && path.back().y == 5);
    // Path must go around — should contain a tile with y > 8
    bool wentAround = false;
    for (auto& p : path)
        if (p.y > 8) wentAround = true;
    EXPECT(wentAround);
}

TEST(pathfind_around_altitude_cliff) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    // Altitude wall from (10,0) to (10,8)
    for (int y = 0; y <= 8; ++y)
        tm.SetCell(10, y, 0, 500);
    auto* m = tm.Get();
    auto path = m->FindPath(5, 5, 15, 5);
    EXPECT(!path.empty());
    EXPECT(path.back().x == 15 && path.back().y == 5);
    // None of the path tiles should have alt 500
    for (auto& p : path) {
        int16_t alt = CGameMap::GetAltitude(m->GetCell(p.x, p.y));
        EXPECT(alt != 500);
    }
}

TEST(pathfind_no_path) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    // Enclose destination in a box of blocked tiles (walls on all 4 sides)
    for (int i = 12; i <= 18; ++i) {
        tm.SetCell(i, 2, 1, 0);   // top wall
        tm.SetCell(i, 8, 1, 0);   // bottom wall
    }
    for (int j = 2; j <= 8; ++j) {
        tm.SetCell(12, j, 1, 0);  // left wall
        tm.SetCell(18, j, 1, 0);  // right wall
    }
    auto* m = tm.Get();
    auto path = m->FindPath(5, 5, 15, 5);
    EXPECT(path.empty());
}

// =====================================================================
// SimplifyPath tests
// =====================================================================

TEST(simplify_collapses_straight_line) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    auto* m = tm.Get();
    // Tile path: (5,5) through (15,5) — 11 tiles
    std::vector<Position> tilePath;
    for (int x = 5; x <= 15; ++x)
        tilePath.push_back({x, 5});
    auto waypoints = m->SimplifyPath(tilePath);
    // Should collapse to a single jump since dist=10 < MAX_JUMP_DIST
    EXPECT(waypoints.size() == 1);
    EXPECT(waypoints[0].x == 15 && waypoints[0].y == 5);
}

TEST(simplify_respects_altitude_cliff) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    // Cliff at x=10: altitude jumps to 500
    for (int y = 0; y < 50; ++y) {
        for (int x = 10; x < 50; ++x)
            tm.SetCell(x, y, 0, 500);
    }
    auto* m = tm.Get();
    // A* tile path that goes along the cliff edge (staying at alt 0)
    // then crosses somehow — but actually A* won't cross because per-step
    // alt check would fail. Let's just test that SimplifyPath won't create
    // a jump that crosses the cliff.
    std::vector<Position> tilePath;
    for (int x = 5; x <= 9; ++x)
        tilePath.push_back({x, 5});
    // Manually add a path that wraps around (pretend there's a gradual slope elsewhere)
    // For this test, just verify the jump from alt 0 to alt 500 is rejected
    tilePath.push_back({10, 5});  // alt 500
    auto waypoints = m->SimplifyPath(tilePath);
    // CanJump(5,5 -> 10,5) fails because dest alt=500 vs origin alt=0 (diff=500>200)
    // So it can't collapse to one jump — it must step through (6,5) etc.
    EXPECT(waypoints.size() > 1);
}

TEST(simplify_long_path_needs_multiple_jumps) {
    TestMap tm(100, 50);
    tm.FillAll(0);
    auto* m = tm.Get();
    // Path 40 tiles long
    std::vector<Position> tilePath;
    for (int x = 0; x <= 40; ++x)
        tilePath.push_back({x, 5});
    auto waypoints = m->SimplifyPath(tilePath);
    // MAX_JUMP_DIST=18, so 40 tiles requires at least 3 jumps
    EXPECT(waypoints.size() >= 3);
    EXPECT(waypoints.back().x == 40 && waypoints.back().y == 5);
}

// =====================================================================
// TileDist tests
// =====================================================================

TEST(tiledist_zero) {
    EXPECT(CGameMap::TileDist(5, 5, 5, 5) == 0);
}

TEST(tiledist_cardinal) {
    EXPECT(CGameMap::TileDist(0, 0, 10, 0) == 10);
    EXPECT(CGameMap::TileDist(0, 0, 0, 10) == 10);
}

TEST(tiledist_diagonal) {
    // (0,0) to (10,10): sqrt(200) ≈ 14.14
    int d = CGameMap::TileDist(0, 0, 10, 10);
    EXPECT(d == 14);
}

TEST(tiledist_max_jump) {
    // 18 tiles straight
    EXPECT(CGameMap::TileDist(0, 0, 18, 0) == 18);
    EXPECT(CGameMap::TileDist(0, 0, 19, 0) == 19);
}

// StepDist (Chebyshev / 8-directional step count) deliberately diverges from TileDist (rounded
// Euclidean) on a diagonal -- this is the exact discrepancy a reviewer caught in follow_plugin.cpp:
// a "5 steps away" band or "2 steps away" dodge trigger measured in TileDist instead would
// under-react on a diagonal approach. Pinned here so it can't silently regress back to TileDist.
TEST(stepdist_matches_tiledist_on_axis) {
    EXPECT(CGameMap::StepDist(0, 0, 10, 0) == 10);
    EXPECT(CGameMap::StepDist(0, 0, 0, 10) == 10);
    EXPECT(CGameMap::StepDist(5, 5, 5, 5) == 0);
}

TEST(stepdist_diverges_from_tiledist_on_diagonal) {
    // (7,7): 7 diagonal steps on this 8-directional grid, but TileDist rounds sqrt(98)=9.9 -> 10.
    EXPECT(CGameMap::StepDist(0, 0, 7, 7) == 7);
    EXPECT(CGameMap::TileDist(0, 0, 7, 7) == 10);
    // (2,2): 2 steps, but TileDist rounds sqrt(8)=2.83 -> 3 -- the case that made a 2-tile-range
    // monster on a diagonal read as "3 away, safe" when it was actually within range.
    EXPECT(CGameMap::StepDist(0, 0, 2, 2) == 2);
    EXPECT(CGameMap::TileDist(0, 0, 2, 2) == 3);
}

// =====================================================================
// Dump file roundtrip test
// =====================================================================

TEST(dump_and_reload) {
    TestMap tm(20, 20);
    tm.FillAll(0);
    tm.SetCell(5, 5, 1, 300);
    tm.SetCell(10, 10, 0, -100);
    auto* m = tm.Get();
    m->m_idMap = 9999;

    const char* path = "test_dump.bin";
    EXPECT(m->DumpToFile(path));

    TestMap loaded = TestMap::LoadDump(path);
    auto* ml = loaded.Get();

    EXPECT(ml->m_sizeMap.iWidth == 20);
    EXPECT(ml->m_sizeMap.iHeight == 20);
    EXPECT(ml->m_idMap == 9999);
    EXPECT(CGameMap::GetMask(ml->GetCell(5, 5)) == 1);
    EXPECT(CGameMap::GetAltitude(ml->GetCell(5, 5)) == 300);
    EXPECT(CGameMap::GetAltitude(ml->GetCell(10, 10)) == -100);
    EXPECT(CGameMap::GetMask(ml->GetCell(0, 0)) == 0);
    EXPECT(CGameMap::GetAltitude(ml->GetCell(0, 0)) == 0);

    remove(path);
}

// =====================================================================
// Scene-overlay tests — parse the client's REAL .DMap/.scene files, off-game.
//
// mapdata.cpp folds the .DMap tail's scene overlays into the cell masks in
// both directions: a part's walkable cells OPEN deck tiles, its rail cells
// BLOCK the base-walkable bank land under them (walkable wins where parts
// overlap). See mapdata.cpp's scene-overlay block and
// docs/investigation/TWIN_CITY_BRIDGE_REBUTTAL.md for the evidence.
//
// The expected reachable-area counts below come from the offline pre/post
// reachability diff (docs/investigation/scripts/reach_diff.py): each equals
// the pre-change area MINUS exactly the newly blocked tiles inside that
// component, i.e. rail blocking cut nothing off. If one of these changes,
// either a parser edit changed walkability (investigate!) or the game files
// were patched (re-run reach_diff.py and update).
// =====================================================================
struct RealMap {
    int w = 0, h = 0;
    std::vector<MapGrid::Cell> cells;
    std::vector<MapPortal> portals;
    bool Walkable(int x, int y) const {
        return x >= 0 && y >= 0 && x < w && y < h && cells[(size_t)y * w + x].mask != 1;
    }
};

static std::string GameRootForTests() {
    static std::string root;
    if (!root.empty()) return root;
    char env[MAX_PATH] = {};
    if (GetEnvironmentVariableA("COCLASSIC_GAME_ROOT", env, MAX_PATH) > 0)
        root = env;
    else
        root = "F:\\Games\\Classic Conquer 2.0";
    // MapGrid::GetGameRoot() reads the same variable (cached on first use).
    SetEnvironmentVariableA("COCLASSIC_GAME_ROOT", root.c_str());
    return root;
}

static RealMap LoadRealMap(const char* dmapName) {
    const std::string root = GameRootForTests();
    const std::string path = root + "\\map\\map\\" + dmapName + ".DMap";
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        throw TestSkip{};
    RealMap m;
    if (!MapGrid::ParseFile(path, &m.w, &m.h, &m.cells, &m.portals)) {
        fprintf(stderr, "    ParseFile failed: %s\n", path.c_str());
        throw 0;
    }
    return m;
}

static void FillTestMap(TestMap& tm, const RealMap& rm) {
    for (int y = 0; y < rm.h; ++y)
        for (int x = 0; x < rm.w; ++x) {
            const MapGrid::Cell& c = rm.cells[(size_t)y * rm.w + x];
            tm.SetCell(x, y, c.mask, c.altitude);
        }
}

static int CountReachable(TestMap& tm, int x, int y) {
    auto seen = tm.Get()->FloodReachable(x, y, 100000000);
    int n = 0;
    for (uint8_t v : seen) n += v ? 1 : 0;
    return n;
}

TEST(overlay_twincity_rails_block_walkable_land) {
    RealMap m = LoadRealMap("newplain");
    // Rail cells that sit on base-walkable bank land — the server refuses these.
    EXPECT(!m.Walkable(601, 682));   // 2026-09-04 live refusal (bridgeA SW cap)
    EXPECT(!m.Walkable(604, 674));   // 2026-09-18 live refusal (bridgeA NW cap)
    EXPECT(!m.Walkable(600, 674));
    EXPECT(!m.Walkable(603, 682));
    EXPECT(!m.Walkable(644, 674));   // bridgeA east cap rail
    EXPECT(!m.Walkable(141, 540));   // bridgeB-L west cap rail
    EXPECT(!m.Walkable(147, 541));   // 2026-09-18 log: jump destination on a rail tile
    EXPECT(!m.Walkable(144, 547));
}

TEST(overlay_twincity_deck_and_banks_stay_walkable) {
    RealMap m = LoadRealMap("newplain");
    EXPECT(m.Walkable(167, 543));    // hero stood mid-bridge here
    EXPECT(m.Walkable(150, 543));
    EXPECT(m.Walkable(624, 678));    // bridgeA deck
    EXPECT(m.Walkable(617, 676));    // landings the bot has actually used
    EXPECT(m.Walkable(635, 676));
    EXPECT(m.Walkable(620, 676));
    EXPECT(!m.Walkable(167, 540));   // rail over river
    EXPECT(!m.Walkable(167, 536));   // river
    EXPECT(!m.Walkable(624, 672));   // river
    EXPECT(m.Walkable(599, 679));    // bank, untouched by any rail
    EXPECT(m.Walkable(649, 679));
}

TEST(overlay_twincity_bridges_still_route) {
    RealMap m = LoadRealMap("newplain");
    TestMap tm(m.w, m.h);
    FillTestMap(tm, m);
    auto* g = tm.Get();
    EXPECT(!g->FindPath(590, 679, 655, 679, 3000000).empty());   // bridgeA W -> E
    EXPECT(!g->FindPath(655, 679, 590, 679, 3000000).empty());   // and back
    EXPECT(!g->FindPath(135, 544, 205, 544, 3000000).empty());   // bridgeB-L W -> E
    EXPECT(!g->FindPath(205, 544, 135, 544, 3000000).empty());
    // The route must not step on a rail tile the server refuses.
    for (auto& p : g->FindPath(590, 679, 655, 679, 3000000))
        EXPECT(m.Walkable(p.x, p.y));
}

TEST(overlay_reach_invariant_twincity) {
    RealMap m = LoadRealMap("newplain");
    TestMap tm(m.w, m.h);
    FillTestMap(tm, m);
    // Pre-change area 340844, minus the 48 newly blocked tiles, nothing cut off.
    EXPECT(CountReachable(tm, 401, 387) == 340796);
    // Every file portal is walkable and lives in that one component.
    auto seen = tm.Get()->FloodReachable(401, 387, 100000000);
    EXPECT(m.portals.size() == 6);
    for (const MapPortal& p : m.portals) {
        EXPECT(m.Walkable(p.x, p.y));
        EXPECT(seen[(size_t)p.y * m.w + p.x] != 0);
    }
}

TEST(overlay_reach_invariant_special_maps) {
    struct Case { const char* name; int px, py, expect; };
    static const Case cases[] = {
        {"task07",        260,   4, 441288},   // pre 441322, 34 blocked
        {"task08",          3, 500, 470174},   // pre 470206, 32 blocked
        {"p-arena",       251, 166,  14171},   // pre  14245, 74 blocked
        {"faction-black", 333, 342,  25222},   // pre  25246, 24 blocked in this component
    };
    for (const Case& c : cases) {
        RealMap m = LoadRealMap(c.name);
        TestMap tm(m.w, m.h);
        FillTestMap(tm, m);
        EXPECT(m.Walkable(c.px, c.py));
        const int got = CountReachable(tm, c.px, c.py);
        if (got != c.expect)
            fprintf(stderr, "    %s: reachable from (%d,%d) = %d, expected %d\n",
                    c.name, c.px, c.py, got, c.expect);
        EXPECT(got == c.expect);
    }
}

TEST(overlay_map1010_bridge_intact) {
    RealMap m = LoadRealMap("newbie");
    EXPECT(m.Walkable(88, 85));      // user-confirmed bridge tile
    EXPECT(!m.Walkable(43, 71));     // genuine void
    EXPECT(m.Walkable(62, 109));
    TestMap tm(m.w, m.h);
    FillTestMap(tm, m);
    EXPECT(!tm.Get()->FindPath(67, 106, 91, 74, 3000000).empty());
}


// =====================================================================
// Build fence (src/build_fence.h): the bot must arm ONLY on verified client builds.
// =====================================================================
static std::vector<uint8_t> FakePe(uint32_t stamp, uint32_t sizeOfImage)
{
    std::vector<uint8_t> b(0x400, 0);
    b[0] = 'M'; b[1] = 'Z';
    const int32_t e = 0x80;
    memcpy(&b[0x3C], &e, 4);
    memcpy(&b[e], "PE\0\0", 4);
    memcpy(&b[e + 8], &stamp, 4);
    memcpy(&b[e + 24 + 56], &sizeOfImage, 4);
    return b;
}

TEST(buildfence_supports_only_verified_builds) {
    uint32_t st = 0, sz = 0;
    auto v1074 = FakePe(0x6A51CFB9u, 0x28C5000u);
    EXPECT(BuildFence::ParsePeIdentity(v1074.data(), v1074.size(), &st, &sz));
    EXPECT(st == 0x6A51CFB9u && sz == 0x28C5000u);
    EXPECT(BuildFence::IsSupported(st, sz));
    // v1078: now has its own DLL target too (coclassic_v1078.dll) — IsSupported (the launcher's
    // broad "do we have a DLL for this build at all" check) must accept it.
    EXPECT(BuildFence::IsSupported(0x6AB0822Bu, 0x2A26000u));
    // both fields must match: same stamp with a different size, or vice versa, is a different build.
    EXPECT(!BuildFence::IsSupported(0x6A51CFB9u, 0x2A26000u));
    EXPECT(!BuildFence::IsSupported(0x6AB0822Bu, 0x28C5000u));
    EXPECT(!BuildFence::IsSupported(0, 0));
}

TEST(buildfence_self_build_is_narrower_than_supported) {
    // This test binary is compiled WITHOUT COCLASSIC_TARGET_V1078 (same as coclassic.dll), so its
    // kSelfBuild is v1074 — IsSelfBuild must accept ONLY v1074, even though IsSupported (the
    // launcher-facing broad list) accepts both. This is the actual DLL-side self-check
    // (dllmain.cpp) and is what prevents a v1078-targeted DLL from firing v1074 offsets or vice
    // versa if ever injected into the wrong process — see build_fence.h's header comment.
    EXPECT(BuildFence::IsSelfBuild(0x6A51CFB9u, 0x28C5000u));
    EXPECT(!BuildFence::IsSelfBuild(0x6AB0822Bu, 0x2A26000u));
    EXPECT(!BuildFence::IsSelfBuild(0, 0));
}

TEST(buildfence_rejects_malformed_headers) {
    uint32_t st = 0, sz = 0;
    std::vector<uint8_t> tiny(0x20, 0);
    EXPECT(!BuildFence::ParsePeIdentity(tiny.data(), tiny.size(), &st, &sz));
    auto notMz = FakePe(0x6A51CFB9u, 0x28C5000u); notMz[0] = 'X';
    EXPECT(!BuildFence::ParsePeIdentity(notMz.data(), notMz.size(), &st, &sz));
    auto noPe = FakePe(0x6A51CFB9u, 0x28C5000u); noPe[0x80] = 'X';
    EXPECT(!BuildFence::ParsePeIdentity(noPe.data(), noPe.size(), &st, &sz));
    auto badOff = FakePe(0x6A51CFB9u, 0x28C5000u); const int32_t huge = 0x7FFFFFF0; memcpy(&badOff[0x3C], &huge, 4);
    EXPECT(!BuildFence::ParsePeIdentity(badOff.data(), badOff.size(), &st, &sz));
    EXPECT(!BuildFence::ReadPeIdentityFromFile("C:\\definitely\\not\\here\\ImConquer.exe", &st, &sz));
}

TEST(buildfence_real_v1074_exe_is_supported) {
    // The byte-exact v1074 snapshot; skipped where it isn't present.
    const char* path = "E:\\CO_Snapshots\\v1074_2026-09-20\\install\\bin\\64\\ImConquer.exe";
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        throw TestSkip{};
    uint32_t st = 0, sz = 0;
    EXPECT(BuildFence::ReadPeIdentityFromFile(path, &st, &sz));
    EXPECT(st == 0x6A51CFB9u && sz == 0x28C5000u);
    EXPECT(BuildFence::IsSupported(st, sz));
}

// =====================================================================
// Dump file integration tests — run only when a dump is provided
// =====================================================================

static void RunDumpTests(const char* dumpPath)
{
    printf("\n=== Dump integration tests: %s ===\n", dumpPath);
    TestMap tm = TestMap::LoadDump(dumpPath);
    auto* m = tm.Get();
    printf("  Map %u: %dx%d\n", m->m_idMap, m->m_sizeMap.iWidth, m->m_sizeMap.iHeight);

    // Collect altitude statistics
    int walkable = 0, blocked = 0;
    int16_t minAlt = 32767, maxAlt = -32768;
    for (int y = 0; y < m->m_sizeMap.iHeight; ++y) {
        for (int x = 0; x < m->m_sizeMap.iWidth; ++x) {
            const CellInfo* cell = m->GetCell(x, y);
            if (CGameMap::GetMask(cell) == 1) { blocked++; continue; }
            walkable++;
            int16_t alt = CGameMap::GetAltitude(cell);
            if (alt < minAlt) minAlt = alt;
            if (alt > maxAlt) maxAlt = alt;
        }
    }
    printf("  Walkable: %d  Blocked: %d  Alt range: [%d, %d]\n",
        walkable, blocked, minAlt, maxAlt);

    // Verify CanJump never approves altitude differences > 200
    printf("  Verifying CanJump altitude invariant...\n");
    int jumpChecked = 0, jumpBlocked = 0;
    // Sample: test jumps from a grid of walkable tiles
    for (int oy = 0; oy < m->m_sizeMap.iHeight; oy += 10) {
        for (int ox = 0; ox < m->m_sizeMap.iWidth; ox += 10) {
            if (!m->IsWalkable(ox, oy)) continue;
            int16_t oAlt = CGameMap::GetAltitude(m->GetCell(ox, oy));

            for (int ty = oy - 18; ty <= oy + 18; ty += 5) {
                for (int tx = ox - 18; tx <= ox + 18; tx += 5) {
                    if (tx < 0 || ty < 0 || tx >= m->m_sizeMap.iWidth || ty >= m->m_sizeMap.iHeight)
                        continue;
                    jumpChecked++;
                    if (!m->CanJump(ox, oy, tx, ty)) {
                        jumpBlocked++;
                        continue;
                    }
                    // If CanJump approved, verify dest altitude is within 200 of origin
                    int16_t tAlt = CGameMap::GetAltitude(m->GetCell(tx, ty));
                    if (abs(tAlt - oAlt) > 200) {
                        printf("  FAIL: CanJump(%d,%d -> %d,%d) approved alt %d -> %d (diff %d)\n",
                            ox, oy, tx, ty, oAlt, tAlt, abs(tAlt - oAlt));
                        g_testsFailed++;
                        return;
                    }
                }
            }
        }
    }
    printf("  Checked %d jumps (%d blocked). Altitude invariant OK.\n", jumpChecked, jumpBlocked);

    // Verify CanReach intermediate cells are altitude-compatible
    printf("  Verifying CanReach consistency with FindPath...\n");
    int pathTests = 0, pathOk = 0;
    // Sample a few pathfinding queries
    for (int oy = 10; oy < m->m_sizeMap.iHeight - 10; oy += 30) {
        for (int ox = 10; ox < m->m_sizeMap.iWidth - 10; ox += 30) {
            if (!m->IsWalkable(ox, oy)) continue;
            int tx = ox + 15, ty = oy + 8;
            if (tx >= m->m_sizeMap.iWidth || ty >= m->m_sizeMap.iHeight) continue;
            if (!m->IsWalkable(tx, ty)) continue;

            auto tilePath = m->FindPath(ox, oy, tx, ty, 20000);
            if (tilePath.empty()) continue;
            pathTests++;

            // Verify SimplifyPath produces only valid jumps
            auto waypoints = m->SimplifyPath(tilePath);
            Position prev = tilePath.front();
            bool allValid = true;
            for (auto& wp : waypoints) {
                if (!m->CanJump(prev.x, prev.y, wp.x, wp.y)) {
                    printf("  FAIL: SimplifyPath jump (%d,%d -> %d,%d) not valid\n",
                        prev.x, prev.y, wp.x, wp.y);
                    allValid = false;
                    break;
                }
                prev = wp;
            }
            if (allValid) pathOk++;
        }
    }
    printf("  Pathfind+Simplify: %d/%d OK\n", pathOk, pathTests);
    if (pathOk < pathTests) g_testsFailed++;
    else g_testsPassed++;
}

// =====================================================================
// Main
// =====================================================================

int main(int argc, char** argv)
{
    printf("=== CGameMap unit tests ===\n");

    // TileDist
    RUN(tiledist_zero);
    RUN(tiledist_cardinal);
    RUN(tiledist_diagonal);
    RUN(tiledist_max_jump);
    RUN(stepdist_matches_tiledist_on_axis);
    RUN(stepdist_diverges_from_tiledist_on_diagonal);

    // CanJump basics
    RUN(jump_same_tile_rejected);
    RUN(jump_within_range);
    RUN(jump_out_of_range);
    RUN(jump_blocked_destination);
    RUN(jump_out_of_bounds);

    // Altitude
    RUN(altitude_same_level_ok);
    RUN(altitude_small_diff_ok);
    RUN(altitude_cliff_blocks_jump);
    RUN(altitude_cliff_blocks_reach);
    RUN(altitude_jump_within_threshold);
    RUN(altitude_jump_over_threshold);

    // Corner cutting
    RUN(corner_diagonal_with_wall);
    RUN(corner_both_cardinals_blocked);
    RUN(corner_one_cardinal_blocked_is_wall_edge);
    RUN(wall_edge_blocks_diagonal_clip);
    RUN(corner_long_diagonal_with_wall_corner);
    RUN(corner_non_diagonal_step_not_relaxed);

    // Pathfinding
    RUN(pathfind_straight_line);
    RUN(pathfind_around_wall);
    RUN(pathfind_around_altitude_cliff);
    RUN(pathfind_no_path);

    // SimplifyPath
    RUN(simplify_collapses_straight_line);
    RUN(simplify_respects_altitude_cliff);
    RUN(simplify_long_path_needs_multiple_jumps);

    // Dump roundtrip
    RUN(dump_and_reload);

    // Real .DMap/.scene overlays (skipped when the client install isn't present)
    RUN(overlay_twincity_rails_block_walkable_land);
    RUN(overlay_twincity_deck_and_banks_stay_walkable);
    RUN(overlay_twincity_bridges_still_route);
    RUN(overlay_reach_invariant_twincity);
    RUN(overlay_reach_invariant_special_maps);
    RUN(overlay_map1010_bridge_intact);

    // Build fence
    RUN(buildfence_supports_only_verified_builds);
    RUN(buildfence_self_build_is_narrower_than_supported);
    RUN(buildfence_rejects_malformed_headers);
    RUN(buildfence_real_v1074_exe_is_supported);

    // Dump integration tests (if dump files provided as args)
    for (int i = 1; i < argc; ++i)
        RunDumpTests(argv[i]);

    printf("\n=== Results: %d passed, %d failed, %d skipped ===\n", g_testsPassed, g_testsFailed, g_testsSkipped);
    return g_testsFailed > 0 ? 1 : 0;
}
