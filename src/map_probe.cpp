#include "map_probe.h"
#include "game.h"

#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <spdlog/spdlog.h>

namespace
{
    // Session-6: a genuine CGameMap carries this vtable RVA (verified 8/8
    // slots pointing to code). Used here as the object signature.
    constexpr uintptr_t kGameMapVtableRva = 0x5CCB60;

    // Static-pointer candidates, in the order they were ranked in session 6.
    struct Candidate { const char* label; uintptr_t rva; };
    const Candidate kCandidates[] = {
        { "session6_primary",  0x699370 },
        { "session6_runnerup", 0x6993B8 },
        { "bot_current_GAME_MAP", 0x4E02E0 },
    };

    template <class T>
    bool TryRead(uintptr_t a, T* out)
    {
        __try { *out = *reinterpret_cast<volatile T*>(a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool Readable(uintptr_t a)
    {
        uint8_t b = 0;
        return TryRead(a, &b);
    }

    struct ImageRange { uintptr_t lo, hi; };

    ImageRange GetImageRange()
    {
        const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
        if (!base) return { 0, 0 };
        const auto* dos = (const IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return { base, base + 0x4000000 };
        const auto* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return { base, base + 0x4000000 };
        return { base, base + nt->OptionalHeader.SizeOfImage };
    }

    bool IsGameMap(uintptr_t p, uintptr_t wantVtable)
    {
        if (!p || (p & 7) || !Readable(p)) return false;
        uint64_t vt = 0;
        return TryRead(p, &vt) && vt == wantVtable;
    }

    // Emit the head of an object as u32s, plus flagged candidates for the
    // fields we actually need. Keeping the raw dump means a wrong guess can be
    // re-analysed offline without another live run.
    void DumpObject(FILE* f, uintptr_t p, int expectedMapId, const ImageRange& img)
    {
        fprintf(f, "      \"u32\": [");
        for (int off = 0; off < 0x240; off += 4) {
            uint32_t v = 0;
            TryRead(p + off, &v);
            fprintf(f, "%s%u", off ? "," : "", v);
        }
        fprintf(f, "],\n");

        fprintf(f, "      \"id_like\": [");
        bool first = true;
        for (int off = 0; off < 0x240; off += 4) {
            uint32_t v = 0;
            if (!TryRead(p + off, &v)) continue;
            if (v >= 100 && v <= 2000) {
                fprintf(f, "%s{\"off\":\"0x%03X\",\"val\":%u,\"is_expected\":%s}",
                        first ? "" : ",", off, v,
                        ((int)v == expectedMapId) ? "true" : "false");
                first = false;
            }
        }
        fprintf(f, "],\n");

        fprintf(f, "      \"dim_like\": [");
        first = true;
        for (int off = 0; off + 4 < 0x240; off += 4) {
            uint32_t a = 0, b = 0;
            if (!TryRead(p + off, &a) || !TryRead(p + off + 4, &b)) continue;
            if (a >= 32 && a <= 8000 && b >= 32 && b <= 8000) {
                fprintf(f, "%s{\"off\":\"0x%03X\",\"w\":%u,\"h\":%u}", first ? "" : ",", off, a, b);
                first = false;
            }
        }
        fprintf(f, "],\n");

        fprintf(f, "      \"heap_ptrs\": [");
        first = true;
        for (int off = 0; off < 0x240; off += 8) {
            uint64_t q = 0;
            if (!TryRead(p + off, &q)) continue;
            if (!q || (q >= img.lo && q < img.hi) || !Readable((uintptr_t)q)) continue;
            fprintf(f, "%s{\"off\":\"0x%03X\",\"ptr\":\"0x%llX\"}",
                    first ? "" : ",", off, (unsigned long long)q);
            first = false;
        }
        fprintf(f, "]\n");
    }
}

bool DebugProbeGameMap(int expectedMapId)
{
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    if (!base) return false;
    const ImageRange img = GetImageRange();
    const uintptr_t wantVtable = base + kGameMapVtableRva;

    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\coclassic_mapprobe.json", "w") != 0 || !f) {
        spdlog::error("[mapprobe] could not open output file");
        return false;
    }

    fprintf(f, "{\n  \"base\": \"0x%llX\", \"image_hi\": \"0x%llX\",\n",
            (unsigned long long)base, (unsigned long long)img.hi);
    fprintf(f, "  \"gamemap_vtable\": \"0x%llX\", \"expected_map_id\": %d,\n",
            (unsigned long long)wantVtable, expectedMapId);

    // Hero position gives an independent cross-check: whichever CGameMap is
    // active should have dimensions large enough to contain the hero's tile.
    if (CHero* hero = Game::GetHero())
        fprintf(f, "  \"hero_pos\": [%d,%d],\n", hero->m_posMap.x, hero->m_posMap.y);

    // ── 1. static pointer candidates ──
    fprintf(f, "  \"candidates\": [\n");
    for (size_t i = 0; i < _countof(kCandidates); ++i) {
        const auto& c = kCandidates[i];
        uint64_t p = 0;
        TryRead(base + c.rva, &p);
        const bool readable = p && Readable((uintptr_t)p);
        uint64_t vt = 0;
        if (readable) TryRead((uintptr_t)p, &vt);

        fprintf(f, "    { \"label\":\"%s\", \"rva\":\"0x%llX\", \"ptr\":\"0x%llX\", "
                   "\"readable\":%s, \"vtable\":\"0x%llX\", \"is_gamemap\":%s",
                c.label, (unsigned long long)c.rva, (unsigned long long)p,
                readable ? "true" : "false", (unsigned long long)vt,
                (readable && vt == wantVtable) ? "true" : "false");
        if (readable) {
            fprintf(f, ",\n");
            DumpObject(f, (uintptr_t)p, expectedMapId, img);
            fprintf(f, "    }%s\n", (i + 1 < _countof(kCandidates)) ? "," : "");
        } else {
            fprintf(f, " }%s\n", (i + 1 < _countof(kCandidates)) ? "," : "");
        }
    }
    fprintf(f, "  ],\n");

    // ── 2. walk the scene object's pointer fields (2 levels) for a CGameMap ──
    fprintf(f, "  \"from_scene\": [\n");
    {
        uint64_t scene = 0;
        TryRead(base + 0x699370, &scene);
        bool first = true;
        if (scene && Readable((uintptr_t)scene)) {
            for (int off = 0; off < 0x400; off += 8) {
                uint64_t q = 0;
                if (!TryRead((uintptr_t)scene + off, &q)) continue;
                if (!q || (q >= img.lo && q < img.hi) || !Readable((uintptr_t)q)) continue;

                if (IsGameMap((uintptr_t)q, wantVtable)) {
                    fprintf(f, "%s    { \"path\":\"scene+0x%X\", \"obj\":\"0x%llX\" }",
                            first ? "" : ",\n", off, (unsigned long long)q);
                    first = false;
                    continue;
                }
                for (int off2 = 0; off2 < 0x200; off2 += 8) {
                    uint64_t r = 0;
                    if (!TryRead((uintptr_t)q + off2, &r)) continue;
                    if (!r || (r >= img.lo && r < img.hi) || !Readable((uintptr_t)r)) continue;
                    if (IsGameMap((uintptr_t)r, wantVtable)) {
                        fprintf(f, "%s    { \"path\":\"scene+0x%X->+0x%X\", \"obj\":\"0x%llX\" }",
                                first ? "" : ",\n", off, off2, (unsigned long long)r);
                        first = false;
                    }
                }
            }
        }
        fprintf(f, "\n  ],\n");
    }

    // ── 3. heap-scan for every CGameMap instance ──
    fprintf(f, "  \"instances\": [\n");
    {
        int found = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t addr = 0x10000;
        while (addr < 0x7FFFFFFF0000ull && found < 32) {
            if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) break;
            const uintptr_t rbase = (uintptr_t)mbi.BaseAddress;
            const size_t rsize = mbi.RegionSize;
            const bool usable = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
                             && (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE);
            if (usable && rsize > 0x240 && rsize < 0x4000000) {
                for (uintptr_t a = rbase; a + 0x240 < rbase + rsize && found < 32; a += 0x10) {
                    uint64_t vt = 0;
                    if (!TryRead(a, &vt) || vt != wantVtable) continue;
                    fprintf(f, "%s    { \"obj\":\"0x%llX\",\n", found ? ",\n" : "", (unsigned long long)a);
                    DumpObject(f, a, expectedMapId, img);
                    fprintf(f, "    }");
                    ++found;
                }
            }
            if (!rsize) break;
            addr = rbase + rsize;
        }
        fprintf(f, "\n  ],\n  \"instance_count\": %d\n", found);
        spdlog::info("[mapprobe] found {} CGameMap instance(s)", found);
    }

    fprintf(f, "}\n");
    fclose(f);
    spdlog::info("[mapprobe] wrote C:\\Users\\Public\\coclassic_mapprobe.json");
    return true;
}

namespace
{
    struct Region { uintptr_t base; size_t size; };

    // Snapshot of committed private RW regions, so a candidate pointer can be
    // reported together with the SIZE of the block it targets. That size is
    // the discriminator here: the cell array should be multi-MB while ordinary
    // objects sit in small blocks.
    std::vector<Region> SnapshotRegions()
    {
        std::vector<Region> out;
        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t addr = 0x10000;
        while (addr < 0x7FFFFFFF0000ull && out.size() < 40000) {
            if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) break;
            const uintptr_t rbase = (uintptr_t)mbi.BaseAddress;
            const size_t rsize = mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
                && (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE))
                out.push_back({ rbase, rsize });
            if (!rsize) break;
            addr = rbase + rsize;
        }
        return out;
    }

    size_t RegionSizeOf(const std::vector<Region>& regions, uintptr_t p)
    {
        for (const auto& r : regions)
            if (p >= r.base && p < r.base + r.size)
                return r.size;
        return 0;
    }
}

bool DebugProbeMapData(int width, int height)
{
    if (width <= 0 || height <= 0) return false;

    const std::vector<Region> regions = SnapshotRegions();

    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\coclassic_mapdata.json", "w") != 0 || !f) {
        spdlog::error("[mapdata] could not open output file");
        return false;
    }

    const uint64_t cells = (uint64_t)width * (uint64_t)height;
    fprintf(f, "{\n  \"width\": %d, \"height\": %d, \"cells\": %llu,\n",
            width, height, (unsigned long long)cells);
    if (CHero* hero = Game::GetHero())
        fprintf(f, "  \"hero_pos\": [%d,%d],\n", hero->m_posMap.x, hero->m_posMap.y);

    // Large allocations, with the per-cell byte size each would imply. A block
    // whose size divided by the cell count lands on a small round number is a
    // strong cell-array candidate.
    fprintf(f, "  \"large_regions\": [\n");
    {
        bool first = true;
        for (const auto& r : regions) {
            if (r.size < 512 * 1024) continue;
            const double perCell = (double)r.size / (double)cells;
            fprintf(f, "%s    { \"base\":\"0x%llX\", \"size\":%llu, \"bytes_per_cell\":%.3f }",
                    first ? "" : ",\n", (unsigned long long)r.base,
                    (unsigned long long)r.size, perCell);
            first = false;
        }
        fprintf(f, "\n  ],\n");
    }

    // Any place holding (width,height) as adjacent u32s — the live map object
    // should be among these, and unlike the descriptors it should sit near a
    // pointer into one of the large regions above.
    fprintf(f, "  \"dim_sites\": [\n");
    {
        int found = 0;
        bool first = true;
        for (const auto& r : regions) {
            if (found >= 64) break;
            for (uintptr_t a = r.base; a + 8 < r.base + r.size && found < 64; a += 4) {
                uint32_t w = 0, h = 0;
                if (!TryRead(a, &w) || w != (uint32_t)width) continue;
                if (!TryRead(a + 4, &h) || h != (uint32_t)height) continue;

                fprintf(f, "%s    { \"at\":\"0x%llX\", \"region_size\":%llu,\n",
                        first ? "" : ",\n", (unsigned long long)a,
                        (unsigned long long)r.size);
                first = false;

                // Pointers in the 0x120 bytes around this site, annotated with
                // the size of the block each one targets.
                fprintf(f, "      \"near_ptrs\": [");
                bool pfirst = true;
                const uintptr_t lo = (a >= 0x80) ? a - 0x80 : 0;
                for (uintptr_t q = lo; q <= a + 0xA0; q += 8) {
                    uint64_t v = 0;
                    if (!TryRead(q, &v) || !v || (v & 7)) continue;
                    if (!Readable((uintptr_t)v)) continue;
                    const size_t tsz = RegionSizeOf(regions, (uintptr_t)v);
                    if (tsz < 64 * 1024) continue;   // only blocks big enough to be a grid
                    fprintf(f, "%s{\"slot_off\":%d,\"ptr\":\"0x%llX\",\"target_region\":%llu,\"bpc\":%.3f}",
                            pfirst ? "" : ",", (int)((int64_t)q - (int64_t)a),
                            (unsigned long long)v, (unsigned long long)tsz,
                            (double)tsz / (double)cells);
                    pfirst = false;
                }
                fprintf(f, "],\n");

                fprintf(f, "      \"u32_around\": [");
                for (int off = -0x40; off < 0x60; off += 4) {
                    uint32_t v = 0;
                    TryRead((uintptr_t)((int64_t)a + off), &v);
                    fprintf(f, "%s%u", (off == -0x40) ? "" : ",", v);
                }
                fprintf(f, "]\n    }");
                ++found;
            }
        }
        fprintf(f, "\n  ],\n  \"dim_site_count\": %d\n", found);
        spdlog::info("[mapdata] {} dim sites, {} regions", found, (int)regions.size());
    }

    fprintf(f, "}\n");
    fclose(f);
    spdlog::info("[mapdata] wrote C:\\Users\\Public\\coclassic_mapdata.json");
    return true;
}

namespace
{
    // ── Walk trace ──
    // Every tile the hero has actually stood on is, by definition, walkable.
    // Collecting a few dozen scattered such tiles gives a constraint that
    // random memory essentially cannot satisfy: the real grid must hold one
    // and the same "walkable" value at ALL of them.
    struct Tile { int x, y; };
    std::vector<Tile> g_walkTrace;

    bool TraceHas(int x, int y)
    {
        for (const auto& t : g_walkTrace)
            if (t.x == x && t.y == y) return true;
        return false;
    }
}

void MapProbe_RecordHeroTile()
{
    CHero* hero = Game::GetHero();
    if (!hero) return;
    const int x = hero->m_posMap.x, y = hero->m_posMap.y;
    if (x <= 0 || y <= 0) return;
    if (g_walkTrace.size() >= 4096) return;
    if (TraceHas(x, y)) return;
    g_walkTrace.push_back({ x, y });
}

int  MapProbe_TraceCount() { return (int)g_walkTrace.size(); }
void MapProbe_ClearTrace() { g_walkTrace.clear(); }

namespace
{
    struct GridScore
    {
        uintptr_t base;
        int       stride;
        int       byteOff;
        bool      rowMajor;
        int       distinct;
        double    structure;   // fraction of horizontal neighbour pairs that match
        double    dominant;    // share held by the most common value in the window
        double    traceAgree;  // share of walked tiles matching the hero's cell value
        int       traceN;
        double    score;
        uint8_t   heroVal;
    };

    // Score one (region, stride, byteOff, indexing) combination over a window
    // centred on the hero.
    bool ScoreCandidate(uintptr_t base, int stride, int byteOff, bool rowMajor,
                        int width, int height, int hx, int hy, int radius,
                        GridScore* out)
    {
        uint8_t vals[41 * 41];
        int n = 0;
        const int side = radius * 2 + 1;
        if (side > 41) return false;

        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int x = hx + dx, y = hy + dy;
                if (x < 0 || y < 0 || x >= width || y >= height) return false;
                const uint64_t idx = rowMajor
                    ? (uint64_t)y * (uint64_t)width + (uint64_t)x
                    : (uint64_t)x * (uint64_t)height + (uint64_t)y;
                uint8_t v = 0;
                if (!TryRead(base + (uintptr_t)(idx * (uint64_t)stride) + byteOff, &v))
                    return false;
                vals[n++] = v;
            }
        }
        if (n != side * side) return false;

        int counts[256] = {};
        for (int i = 0; i < n; ++i) ++counts[vals[i]];
        int distinct = 0, topCount = 0;
        for (int v = 0; v < 256; ++v) {
            if (!counts[v]) continue;
            ++distinct;
            if (counts[v] > topCount) topCount = counts[v];
        }
        if (distinct < 2 || distinct > 12) return false;

        // Reject near-uniform windows. This is what let a mostly-zero memory
        // region masquerade as a perfectly "structured" grid on the first
        // attempt: if ~all cells share one value, neighbour-match is ~1.0 for
        // reasons that have nothing to do with terrain.
        const double dominant = (double)topCount / (double)n;
        if (dominant > 0.90) return false;

        int pairs = 0, matches = 0;
        for (int r = 0; r < side; ++r) {
            for (int c = 0; c + 1 < side; ++c) {
                ++pairs;
                if (vals[r * side + c] == vals[r * side + c + 1]) ++matches;
            }
        }
        const double structure = pairs ? (double)matches / (double)pairs : 0.0;
        if (structure < 0.55) return false;

        const uint8_t heroVal = vals[(side / 2) * side + (side / 2)];

        // ── The decisive test ──
        // Read the candidate at every tile the hero has stood on. All of them
        // must agree, and must agree with the hero's current cell. Random
        // memory will not hold one consistent value across dozens of
        // scattered, unrelated coordinates.
        double traceAgree = 1.0;
        int traceN = 0;
        if (!g_walkTrace.empty()) {
            int agree = 0;
            for (const auto& t : g_walkTrace) {
                if (t.x >= width || t.y >= height) continue;
                const uint64_t idx = rowMajor
                    ? (uint64_t)t.y * (uint64_t)width + (uint64_t)t.x
                    : (uint64_t)t.x * (uint64_t)height + (uint64_t)t.y;
                uint8_t v = 0;
                if (!TryRead(base + (uintptr_t)(idx * (uint64_t)stride) + byteOff, &v))
                    return false;
                ++traceN;
                if (v == heroVal) ++agree;
            }
            if (traceN >= 8) {
                traceAgree = (double)agree / (double)traceN;
                // A real walkability grid agrees on essentially all of them.
                if (traceAgree < 0.90) return false;
            }
        }

        out->base = base;
        out->stride = stride;
        out->byteOff = byteOff;
        out->rowMajor = rowMajor;
        out->distinct = distinct;
        out->structure = structure;
        out->heroVal = heroVal;
        out->dominant = dominant;
        out->traceAgree = traceAgree;
        out->traceN = traceN;
        // Rank primarily on the trace constraint, then on having a genuinely
        // mixed window (walls present) rather than a bland one.
        out->score = traceAgree * (1.0 - dominant) * structure;
        return true;
    }
}

bool DebugAutoFindGrid(int width, int height)
{
    if (width <= 0 || height <= 0) return false;
    CHero* hero = Game::GetHero();
    if (!hero) return false;

    const int hx = hero->m_posMap.x;
    const int hy = hero->m_posMap.y;
    const int radius = 10;
    const uint64_t cells = (uint64_t)width * (uint64_t)height;

    const std::vector<Region> regions = SnapshotRegions();
    const int kStrides[] = { 1, 2, 3, 4, 6, 8, 12, 16, 20, 24 };

    std::vector<GridScore> hits;
    for (const auto& r : regions) {
        if (r.size < 256 * 1024) continue;
        for (int si = 0; si < _countof(kStrides); ++si) {
            const int stride = kStrides[si];
            // The region must be able to hold the whole grid.
            if ((uint64_t)r.size < cells * (uint64_t)stride) continue;
            for (int bo = 0; bo < stride && bo < 8; ++bo) {
                for (int m = 0; m < 2; ++m) {
                    GridScore gs{};
                    if (ScoreCandidate(r.base, stride, bo, m == 0,
                                       width, height, hx, hy, radius, &gs))
                        hits.push_back(gs);
                }
            }
        }
    }

    std::sort(hits.begin(), hits.end(),
              [](const GridScore& a, const GridScore& b) { return a.score > b.score; });

    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\coclassic_gridscan.json", "w") != 0 || !f) {
        spdlog::error("[gridscan] could not open output file");
        return false;
    }
    fprintf(f, "{\n  \"width\": %d, \"height\": %d, \"hero\": [%d,%d],\n", width, height, hx, hy);
    fprintf(f, "  \"trace_tiles\": %d,\n", (int)g_walkTrace.size());
    fprintf(f, "  \"regions_examined\": %d, \"candidates\": %d,\n",
            (int)regions.size(), (int)hits.size());
    fprintf(f, "  \"top\": [\n");
    const int lim = (int)hits.size() < 40 ? (int)hits.size() : 40;
    for (int i = 0; i < lim; ++i) {
        const auto& h = hits[i];
        fprintf(f, "%s    { \"base\":\"0x%llX\", \"stride\":%d, \"byte_off\":%d, \"row_major\":%s, "
                   "\"distinct\":%d, \"structure\":%.4f, \"dominant\":%.4f, "
                   "\"trace_agree\":%.4f, \"trace_n\":%d, \"score\":%.4f, \"hero_val\":%u }",
                i ? ",\n" : "", (unsigned long long)h.base, h.stride, h.byteOff,
                h.rowMajor ? "true" : "false", h.distinct, h.structure, h.dominant,
                h.traceAgree, h.traceN, h.score, h.heroVal);
    }
    fprintf(f, "\n  ]\n}\n");
    fclose(f);

    // Render each top candidate as a downsampled ASCII map of the WHOLE map.
    // This is the decisive check: a real walkability grid looks like a city —
    // contiguous buildings, streets, a defined boundary. Random memory that
    // happens to satisfy the trace constraint will not.
    FILE* g = nullptr;
    if (fopen_s(&g, "C:\\Users\\Public\\coclassic_gridmaps.txt", "w") == 0 && g) {
        const int cols = 108, rows = 76;
        const int stepX = (width + cols - 1) / cols;
        const int stepY = (height + rows - 1) / rows;
        const int shown = (int)hits.size() < 5 ? (int)hits.size() : 5;

        fprintf(g, "map %dx%d  hero (%d,%d)  trace %d tiles\n",
                width, height, hx, hy, (int)g_walkTrace.size());
        fprintf(g, "'.' = hero's cell value (walkable)   '#','+','o','*' = other values\n");
        fprintf(g, "'H' = hero position\n\n");

        for (int i = 0; i < shown; ++i) {
            const auto& h = hits[i];
            fprintf(g, "===== #%d base=0x%llX stride=%d byteOff=%d %s "
                       "distinct=%d dominant=%.3f traceAgree=%.3f heroVal=%u =====\n",
                    i + 1, (unsigned long long)h.base, h.stride, h.byteOff,
                    h.rowMajor ? "row-major" : "col-major",
                    h.distinct, h.dominant, h.traceAgree, h.heroVal);

            for (int y = 0; y < height; y += stepY) {
                for (int x = 0; x < width; x += stepX) {
                    if (x <= hx && hx < x + stepX && y <= hy && hy < y + stepY) {
                        fputc('H', g);
                        continue;
                    }
                    const uint64_t idx = h.rowMajor
                        ? (uint64_t)y * (uint64_t)width + (uint64_t)x
                        : (uint64_t)x * (uint64_t)height + (uint64_t)y;
                    uint8_t v = 0;
                    if (!TryRead(h.base + (uintptr_t)(idx * (uint64_t)h.stride) + h.byteOff, &v)) {
                        fputc('?', g);
                        continue;
                    }
                    if (v == h.heroVal)      fputc('.', g);
                    else if (v == 0)         fputc(' ', g);
                    else if (v < 4)          fputc('#', g);
                    else if (v < 16)         fputc('+', g);
                    else if (v < 64)         fputc('o', g);
                    else                     fputc('*', g);
                }
                fputc('\n', g);
            }
            fprintf(g, "\n\n");
        }
        fclose(g);
        spdlog::info("[gridscan] wrote ASCII maps for top {} candidates", shown);
    }

    spdlog::info("[gridscan] {} candidates from {} regions", (int)hits.size(), (int)regions.size());
    return true;
}

namespace
{
    // Kept in its own function with no C++ objects: MSVC rejects __try in any
    // function that requires object unwinding (C2712).
    bool GuardedCopy(void* dst, uintptr_t src, size_t n)
    {
        __try { memcpy(dst, (const void*)src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

bool DebugSearchPattern()
{
    // Load the needle written by the host-side tooling.
    FILE* pf = nullptr;
    if (fopen_s(&pf, "C:\\Users\\Public\\coclassic_pattern.bin", "rb") != 0 || !pf) {
        spdlog::error("[pattern] no C:\\Users\\Public\\coclassic_pattern.bin");
        return false;
    }
    std::vector<uint8_t> pat;
    {
        uint8_t buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), pf)) > 0)
            pat.insert(pat.end(), buf, buf + n);
        fclose(pf);
    }
    if (pat.size() < 16) {
        spdlog::error("[pattern] pattern too short ({} bytes)", pat.size());
        return false;
    }

    const std::vector<Region> regions = SnapshotRegions();

    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\coclassic_patternhits.json", "w") != 0 || !f)
        return false;
    fprintf(f, "{\n  \"pattern_bytes\": %d,\n  \"regions\": %d,\n  \"hits\": [\n",
            (int)pat.size(), (int)regions.size());

    int hits = 0;
    const uint8_t first = pat[0];
    const size_t plen = pat.size();

    for (const auto& r : regions) {
        if (hits >= 64) break;
        if (r.size < plen) continue;

        // Scan the region in chunks through a guarded copy: reading committed
        // pages can still fault if the region is freed mid-scan.
        const size_t kChunk = 1 << 16;
        std::vector<uint8_t> buf(kChunk + plen);
        for (size_t off = 0; off + plen <= r.size && hits < 64; off += kChunk) {
            const size_t want = (r.size - off < kChunk + plen) ? (r.size - off) : (kChunk + plen);
            if (!GuardedCopy(buf.data(), r.base + off, want))
                continue;

            for (size_t i = 0; i + plen <= want; ++i) {
                if (buf[i] != first) continue;
                if (memcmp(buf.data() + i, pat.data(), plen) != 0) continue;
                fprintf(f, "%s    { \"addr\":\"0x%llX\", \"region_base\":\"0x%llX\", \"region_size\":%llu }",
                        hits ? ",\n" : "",
                        (unsigned long long)(r.base + off + i),
                        (unsigned long long)r.base,
                        (unsigned long long)r.size);
                ++hits;
                if (hits >= 64) break;
            }
        }
    }

    fprintf(f, "\n  ],\n  \"hit_count\": %d\n}\n", hits);
    fclose(f);
    spdlog::info("[pattern] {} match(es) for {}-byte pattern", hits, (int)pat.size());
    return true;
}

namespace
{
    // CItem signature. The equipment array at hero+0xBD8 is live-verified, so
    // the shape of a real CItem is known: vtable into the image, a non-zero
    // UID at +0x08, and a typeId at +0x10 that exists in itemtype.
    bool LooksLikeItem(uintptr_t p, uint32_t* uid, uint32_t* typeId)
    {
        if (!p || (p & 7)) return false;
        const ImageRange img = GetImageRange();
        uint64_t vt = 0;
        if (!TryRead(p, &vt) || vt < img.lo || vt >= img.hi) return false;
        uint32_t u = 0, t = 0;
        if (!TryRead(p + 0x08, &u) || u == 0) return false;
        if (!TryRead(p + 0x10, &t) || t < 100 || t > 3000000) return false;
        *uid = u; *typeId = t;
        return true;
    }
}

bool DebugFindInventory()
{
    CHero* hero = Game::GetHero();
    if (!hero) return false;
    const uintptr_t h = (uintptr_t)hero;

    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\coclassic_inventory.json", "w") != 0 || !f)
        return false;

    fprintf(f, "{\n  \"hero\": \"0x%llX\",\n", (unsigned long long)h);

    // Known-good reference: the equipment array, so the output can be sanity
    // checked against slots we already read correctly.
    fprintf(f, "  \"equipment_0xBD8\": [");
    {
        bool first = true;
        for (int slot = 0; slot < 8; ++slot) {
            uint64_t p = 0;
            TryRead(h + 0xBD8 + (uintptr_t)slot * 0x10, &p);
            uint32_t uid = 0, tid = 0;
            const bool ok = LooksLikeItem((uintptr_t)p, &uid, &tid);
            fprintf(f, "%s{\"slot\":%d,\"ptr\":\"0x%llX\",\"uid\":%u,\"typeId\":%u,\"valid\":%s}",
                    first ? "" : ",", slot, (unsigned long long)p, uid, tid, ok ? "true" : "false");
            first = false;
        }
        fprintf(f, "],\n");
    }

    // Direct CItem pointers anywhere in the hero object.
    fprintf(f, "  \"direct_items\": [");
    {
        bool first = true;
        int n = 0;
        for (int off = 0; off + 8 <= 0x4000 && n < 128; off += 8) {
            uint64_t p = 0;
            if (!TryRead(h + off, &p)) continue;
            uint32_t uid = 0, tid = 0;
            if (!LooksLikeItem((uintptr_t)p, &uid, &tid)) continue;
            fprintf(f, "%s{\"off\":\"0x%X\",\"ptr\":\"0x%llX\",\"uid\":%u,\"typeId\":%u}",
                    first ? "" : ",", off, (unsigned long long)p, uid, tid);
            first = false; ++n;
        }
        fprintf(f, "],\n");
    }

    // Indirect: a hero field pointing at an ARRAY of CItem pointers — this is
    // what a vector's storage or a deque's block map looks like.
    fprintf(f, "  \"item_arrays\": [");
    {
        bool first = true;
        int n = 0;
        for (int off = 0; off + 8 <= 0x4000 && n < 64; off += 8) {
            uint64_t arr = 0;
            if (!TryRead(h + off, &arr)) continue;
            if (!arr || (arr & 7) || !Readable((uintptr_t)arr)) continue;

            // How many consecutive CItem pointers live at this target?
            int run = 0;
            uint32_t firstUid = 0, firstType = 0;
            for (int k = 0; k < 64; ++k) {
                uint64_t q = 0;
                if (!TryRead((uintptr_t)arr + (uintptr_t)k * 8, &q)) break;
                uint32_t uid = 0, tid = 0;
                if (!LooksLikeItem((uintptr_t)q, &uid, &tid)) {
                    if (k == 0) break;      // not an item array at all
                    continue;               // tolerate gaps (deque slack)
                }
                if (run == 0) { firstUid = uid; firstType = tid; }
                ++run;
            }
            if (run < 1) continue;
            fprintf(f, "%s{\"off\":\"0x%X\",\"array\":\"0x%llX\",\"items\":%d,"
                       "\"firstUid\":%u,\"firstType\":%u}",
                    first ? "" : ",", off, (unsigned long long)arr, run, firstUid, firstType);
            first = false; ++n;
        }
        fprintf(f, "]\n}\n");
    }

    fclose(f);
    spdlog::info("[inventory] probe written");
    return true;
}

bool DebugListMappedFiles()
{
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\coclassic_mappedfiles.json", "w") != 0 || !f)
        return false;

    fprintf(f, "{\n  \"mapped\": [\n");
    int n = 0;
    bool first = true;

    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t addr = 0x10000;
    wchar_t path[MAX_PATH];
    wchar_t lastPath[MAX_PATH] = L"";

    while (addr < 0x7FFFFFFF0000ull && n < 400) {
        if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) break;
        const uintptr_t rbase = (uintptr_t)mbi.BaseAddress;
        const size_t rsize = mbi.RegionSize;

        if (mbi.State == MEM_COMMIT && (mbi.Type == MEM_MAPPED || mbi.Type == MEM_IMAGE)) {
            path[0] = 0;
            if (GetMappedFileNameW(GetCurrentProcess(), (void*)rbase, path, MAX_PATH) > 0) {
                // Regions of one file come back consecutively; collapse them.
                if (wcscmp(path, lastPath) != 0) {
                    wcscpy_s(lastPath, path);
                    char nar[MAX_PATH * 2];
                    const int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nar, sizeof(nar), nullptr, nullptr);
                    if (len > 0) {
                        // Escape backslashes for JSON.
                        char esc[MAX_PATH * 4];
                        int k = 0;
                        for (int i = 0; nar[i] && k < (int)sizeof(esc) - 2; ++i) {
                            if (nar[i] == '\\') esc[k++] = '\\';
                            esc[k++] = nar[i];
                        }
                        esc[k] = 0;
                        fprintf(f, "%s    { \"base\":\"0x%llX\", \"size\":%llu, \"type\":\"%s\", \"path\":\"%s\" }",
                                first ? "" : ",\n", (unsigned long long)rbase,
                                (unsigned long long)rsize,
                                mbi.Type == MEM_IMAGE ? "image" : "mapped", esc);
                        first = false;
                        ++n;
                    }
                }
            }
        }
        if (!rsize) break;
        addr = rbase + rsize;
    }

    fprintf(f, "\n  ],\n  \"count\": %d\n}\n", n);
    fclose(f);
    spdlog::info("[mappedfiles] {} file-backed mappings", n);
    return true;
}

bool DebugSnapshotScene(int label)
{
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    if (!base) return false;

    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\coclassic_scene.json", "a") != 0 || !f)
        return false;

    uint64_t scene = 0;
    TryRead(base + 0x699370, &scene);

    fprintf(f, "{ \"label\": %d, \"scene\": \"0x%llX\"", label, (unsigned long long)scene);
    if (CHero* hero = Game::GetHero())
        fprintf(f, ", \"hero\": [%d,%d]", hero->m_posMap.x, hero->m_posMap.y);

    // Raw field dump: the diff happens offline.
    fprintf(f, ",\n  \"fields\": [");
    if (scene && Readable((uintptr_t)scene)) {
        for (int off = 0; off < 0x1000; off += 4) {
            uint32_t v = 0;
            TryRead((uintptr_t)scene + off, &v);
            fprintf(f, "%s%u", off ? "," : "", v);
        }
    }
    fprintf(f, "],\n");

    // One level of indirection, so a map id held in a sub-object is visible.
    fprintf(f, "  \"sub\": [");
    if (scene && Readable((uintptr_t)scene)) {
        bool first = true;
        int emitted = 0;
        for (int off = 0; off < 0x400 && emitted < 24; off += 8) {
            uint64_t q = 0;
            if (!TryRead((uintptr_t)scene + off, &q)) continue;
            if (!q || q < 0x100000ull || (q & 7) || !Readable((uintptr_t)q)) continue;
            fprintf(f, "%s{\"off\":\"0x%X\",\"ptr\":\"0x%llX\",\"v\":[",
                    first ? "" : ",", off, (unsigned long long)q);
            for (int o2 = 0; o2 < 0x80; o2 += 4) {
                uint32_t v = 0;
                TryRead((uintptr_t)q + o2, &v);
                fprintf(f, "%s%u", o2 ? "," : "", v);
            }
            fprintf(f, "]}");
            first = false;
            ++emitted;
        }
    }
    fprintf(f, "],\n");

    // Image-wide pointer slots whose target holds a PLAUSIBLE map id at +0x10.
    // Note this filters by plausibility rather than by a supplied id: asking
    // the user which map they are in is what let mislabelled scans poison an
    // earlier intersection. The diff happens offline, anchored on Twin City
    // (1002), the one id all sources agree on.
    fprintf(f, "  \"idslots\": [");
    {
        const ImageRange img = GetImageRange();
        bool first = true;
        int n = 0;
        for (uintptr_t a = img.lo; a + 8 <= img.hi && n < 4000; a += 8) {
            uint64_t p = 0;
            if (!TryRead(a, &p)) continue;
            if (p < 0x100000ull || p >= 0x7FF000000000ull || (p & 7)) continue;
            if (!Readable((uintptr_t)p)) continue;
            uint32_t v = 0;
            if (!TryRead((uintptr_t)p + 0x10, &v)) continue;
            if (v < 100 || v > 20000) continue;
            fprintf(f, "%s[%llu,%u]", first ? "" : ",",
                    (unsigned long long)(a - base), v);
            first = false;
            ++n;
        }
    }
    fprintf(f, "],\n");

    // Descriptor-pointer slots inside the stable container objects.
    //
    // The client keeps ~25 map descriptors (vtable 0x5CCB60, id at +0x10) and
    // session 6 recorded that the role manager holds pointers to cached maps.
    // So instead of hunting for the id itself — which is nowhere in the image,
    // the scene object, the hero or the role manager — hunt for the SLOT that
    // points at the CURRENT map's descriptor. Record (container, offset, id)
    // and diff across maps: the slot whose id tracks the map is the answer,
    // and it is reached through an object we can already resolve reliably.
    {
        const uintptr_t vt = base + 0x5CCB60;
        struct Container { const char* name; uintptr_t addr; int size; };
        const Container cs[] = {
            { "rolemgr", (uintptr_t)Game::GetRoleMgr(), 0x4000 },
            { "hero",    (uintptr_t)Game::GetHero(),    0x4000 },
            { "scene",   (uintptr_t)scene,              0x1000 },
        };

        fprintf(f, "  \"mapslots\": [");
        bool first = true;
        for (const auto& c : cs) {
            if (!c.addr) continue;
            for (int off = 0; off + 8 <= c.size; off += 8) {
                uint64_t q = 0;
                if (!TryRead(c.addr + off, &q)) continue;
                if (!q || (q & 7) || !Readable((uintptr_t)q)) continue;
                uint64_t tvt = 0;
                if (!TryRead((uintptr_t)q, &tvt) || tvt != vt) continue;
                uint32_t id = 0;
                TryRead((uintptr_t)q + 0x10, &id);
                fprintf(f, "%s{\"c\":\"%s\",\"off\":%d,\"id\":%u,\"ptr\":\"0x%llX\"}",
                        first ? "" : ",", c.name, off, id, (unsigned long long)q);
                first = false;
            }
        }
        fprintf(f, "],\n");
    }

    // Process-wide: every address holding a pointer to ANY map descriptor,
    // tagged with that descriptor's id.
    //
    // This drops the need to know the map id as input, and drops the
    // assumption that the holder is inline in a container we can name — the
    // descriptors are evidently held in heap nodes (a std::map or similar),
    // so a flat scan of rolemgr/hero/scene found nothing. Diffing these
    // across maps finds the ADDRESS whose descriptor changes with the map:
    // that slot is the client's "current map" reference. Holder addresses are
    // stable within a session, which is what makes the intersection valid.
    {
        const uintptr_t vt = base + 0x5CCB60;
        const std::vector<Region> regions = SnapshotRegions();

        // 1. collect descriptor addresses
        std::vector<uintptr_t> descs;
        for (const auto& r : regions) {
            if (descs.size() >= 256) break;
            for (uintptr_t a = r.base; a + 0x20 < r.base + r.size && descs.size() < 256; a += 0x10) {
                uint64_t v = 0;
                if (TryRead(a, &v) && v == vt) descs.push_back(a);
            }
        }
        std::sort(descs.begin(), descs.end());

        // 2. find every qword anywhere that points at one of them
        fprintf(f, "  \"holders\": [");
        bool first = true;
        int n = 0;
        for (const auto& r : regions) {
            if (n >= 3000) break;
            for (uintptr_t a = r.base; a + 8 <= r.base + r.size && n < 3000; a += 8) {
                uint64_t q = 0;
                if (!TryRead(a, &q) || !q || (q & 7)) continue;
                if (!std::binary_search(descs.begin(), descs.end(), (uintptr_t)q)) continue;
                uint32_t id = 0;
                TryRead((uintptr_t)q + 0x10, &id);
                fprintf(f, "%s[%llu,%u]", first ? "" : ",", (unsigned long long)a, id);
                first = false;
                ++n;
            }
        }
        fprintf(f, "],\n  \"descriptor_count\": %d\n}\n", (int)descs.size());
        spdlog::info("[scene] {} descriptors, {} holder slots", (int)descs.size(), n);
    }

    fclose(f);
    spdlog::info("[scene] snapshot label={} scene=0x{:X}", label, scene);
    return true;
}

bool DebugDumpWalkTrace()
{
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\coclassic_walktrace.json", "w") != 0 || !f)
        return false;

    fprintf(f, "{\n  \"count\": %d,\n", (int)g_walkTrace.size());
    if (CHero* hero = Game::GetHero())
        fprintf(f, "  \"hero_now\": [%d,%d],\n", hero->m_posMap.x, hero->m_posMap.y);
    fprintf(f, "  \"tiles\": [");
    for (size_t i = 0; i < g_walkTrace.size(); ++i)
        fprintf(f, "%s[%d,%d]", i ? "," : "", g_walkTrace[i].x, g_walkTrace[i].y);
    fprintf(f, "]\n}\n");
    fclose(f);
    spdlog::info("[walktrace] dumped {} tiles", (int)g_walkTrace.size());
    return true;
}

MapIdDiag MapProbe_MapIdDiag()
{
    MapIdDiag d{};
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    if (!base) return d;

    // Session 10: the map id is a DIRECT u32 in the image, not behind a
    // pointer — so this just reports both surviving globals verbatim.
    uint32_t v = 0;
    if (TryRead(base + Offsets::CURRENT_MAP_ID, &v)) {
        d.val1 = v;
        d.ptr1Readable = true;
    }
    if (TryRead(base + Offsets::CURRENT_MAP_ID_ALT, &v)) {
        d.val2 = v;
        d.ptr2Readable = true;
    }
    return d;
}

bool DebugScanForMapId(int currentMapId)
{
    if (currentMapId <= 0) return false;
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    if (!base) return false;
    const ImageRange img = GetImageRange();

    // Append rather than overwrite: the answer comes from intersecting the
    // hit sets of two different maps, so both runs must survive.
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\coclassic_mapid.json", "a") != 0 || !f)
        return false;

    CHero* hero = Game::GetHero();
    CRoleMgr* mgr = Game::GetRoleMgr();

    fprintf(f, "{ \"map_id\": %d, \"hero\": \"0x%llX\", \"role_mgr\": \"0x%llX\",\n",
            currentMapId, (unsigned long long)hero, (unsigned long long)mgr);

    // 1. Image globals. A current-map id kept in a static lives here.
    fprintf(f, "  \"image_globals\": [");
    {
        bool first = true;
        int n = 0;
        for (uintptr_t a = img.lo; a + 4 <= img.hi && n < 512; a += 4) {
            uint32_t v = 0;
            if (!TryRead(a, &v) || v != (uint32_t)currentMapId) continue;
            fprintf(f, "%s\"0x%llX\"", first ? "" : ",", (unsigned long long)(a - base));
            first = false;
            ++n;
        }
        fprintf(f, "],\n");
    }

    // 2. The hero object — a per-role "which map am I on" field is plausible.
    fprintf(f, "  \"hero_offsets\": [");
    if (hero) {
        bool first = true;
        for (int off = 0; off < 0x4000; off += 4) {
            uint32_t v = 0;
            if (!TryRead((uintptr_t)hero + off, &v) || v != (uint32_t)currentMapId) continue;
            fprintf(f, "%s\"0x%X\"", first ? "" : ",", off);
            first = false;
        }
    }
    fprintf(f, "],\n");

    // 3. The role manager.
    fprintf(f, "  \"rolemgr_offsets\": [");
    if (mgr) {
        bool first = true;
        for (int off = 0; off < 0x2000; off += 4) {
            uint32_t v = 0;
            if (!TryRead((uintptr_t)mgr + off, &v) || v != (uint32_t)currentMapId) continue;
            fprintf(f, "%s\"0x%X\"", first ? "" : ",", off);
            first = false;
        }
    }
    fprintf(f, "],\n");

    // 5. The scene object at 0x699370 — session 6 verified this pointer
    // genuinely changes when the map changes, so the id should be reachable
    // from it. Searching ONE object graph instead of the whole image shrinks
    // the candidate space by ~6 orders of magnitude, which matters: the
    // image-wide pointer scan below produced false positives that survived
    // four maps purely by chance and then evaporated in a fresh process.
    fprintf(f, "  \"scene_paths\": [");
    {
        bool first = true;
        int n = 0;
        uint64_t scene = 0;
        TryRead(base + 0x699370, &scene);
        if (scene && Readable((uintptr_t)scene)) {
            // direct fields
            for (int off = 0; off < 0x1000 && n < 128; off += 4) {
                uint32_t v = 0;
                if (!TryRead((uintptr_t)scene + off, &v) || v != (uint32_t)currentMapId) continue;
                fprintf(f, "%s\"+0x%X\"", first ? "" : ",", off);
                first = false; ++n;
            }
            // one level of indirection
            for (int off = 0; off < 0x800 && n < 128; off += 8) {
                uint64_t q = 0;
                if (!TryRead((uintptr_t)scene + off, &q)) continue;
                if (!q || q < 0x100000ull || (q & 7) || !Readable((uintptr_t)q)) continue;
                for (int off2 = 0; off2 < 0x400 && n < 128; off2 += 4) {
                    uint32_t v = 0;
                    if (!TryRead((uintptr_t)q + off2, &v) || v != (uint32_t)currentMapId) continue;
                    fprintf(f, "%s\"+0x%X->+0x%X\"", first ? "" : ",", off, off2);
                    first = false; ++n;
                }
            }
        }
        fprintf(f, "],\n");
    }

    // 4. Indirect: a global holding a POINTER to an object whose id field
    // matches the current map. NOTE: this produced FALSE POSITIVES that
    // survived a four-map intersection and then failed in a fresh process —
    // the image is large enough for coincidences to persist. Kept for
    // completeness, but trust the scene-object paths above instead.
    fprintf(f, "  \"pointer_globals\": [");
    {
        bool first = true;
        int n = 0;
        for (uintptr_t a = img.lo; a + 8 <= img.hi && n < 256; a += 8) {
            uint64_t p = 0;
            if (!TryRead(a, &p)) continue;
            if (p < 0x100000ull || p >= 0x7FF000000000ull || (p & 7)) continue;
            if (!Readable((uintptr_t)p)) continue;
            for (int off = 0; off <= 0x40; off += 4) {
                uint32_t v = 0;
                if (!TryRead((uintptr_t)p + off, &v) || v != (uint32_t)currentMapId) continue;
                fprintf(f, "%s{\"rva\":\"0x%llX\",\"off\":\"0x%X\"}",
                        first ? "" : ",", (unsigned long long)(a - base), off);
                first = false;
                ++n;
                break;
            }
        }
        fprintf(f, "]}\n");
    }

    fclose(f);
    spdlog::info("[mapid] scanned for map id {}", currentMapId);
    return true;
}

bool DebugProbeCells(unsigned long long gridBase, int stride, int width, int height, int radius)
{
    if (!gridBase || stride <= 0 || stride > 64 || width <= 0 || height <= 0)
        return false;
    if (radius < 1 || radius > 16) radius = 5;

    CHero* hero = Game::GetHero();
    if (!hero) return false;
    const int hx = hero->m_posMap.x;
    const int hy = hero->m_posMap.y;

    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\coclassic_cells.json", "w") != 0 || !f) {
        spdlog::error("[cells] could not open output file");
        return false;
    }

    fprintf(f, "{\n  \"grid_base\": \"0x%llX\", \"stride\": %d,\n", gridBase, stride);
    fprintf(f, "  \"width\": %d, \"height\": %d, \"hero\": [%d,%d], \"radius\": %d,\n",
            width, height, hx, hy, radius);

    // Emit the same window under both indexings; only one can be right, and
    // the wrong one should look like noise next to the hero's own cell.
    for (int mode = 0; mode < 2; ++mode) {
        const bool rowMajor = (mode == 0);
        fprintf(f, "  \"%s\": [\n", rowMajor ? "row_major" : "col_major");

        for (int dy = -radius; dy <= radius; ++dy) {
            fprintf(f, "    [");
            for (int dx = -radius; dx <= radius; ++dx) {
                const int x = hx + dx, y = hy + dy;
                if (x < 0 || y < 0 || x >= width || y >= height) {
                    fprintf(f, "%snull", dx == -radius ? "" : ",");
                    continue;
                }
                const uint64_t idx = rowMajor
                    ? (uint64_t)y * (uint64_t)width + (uint64_t)x
                    : (uint64_t)x * (uint64_t)height + (uint64_t)y;
                const uintptr_t p = (uintptr_t)gridBase + (uintptr_t)(idx * (uint64_t)stride);

                fprintf(f, "%s[", dx == -radius ? "" : ",");
                for (int b = 0; b < stride; ++b) {
                    uint8_t v = 0;
                    TryRead(p + b, &v);
                    fprintf(f, "%s%u", b ? "," : "", v);
                }
                fprintf(f, "]");
            }
            fprintf(f, "]%s\n", dy == radius ? "" : ",");
        }
        fprintf(f, "  ]%s\n", rowMajor ? "," : "");
    }

    fprintf(f, "}\n");
    fclose(f);
    spdlog::info("[cells] wrote C:\\Users\\Public\\coclassic_cells.json (base=0x{:X} stride={})",
                 gridBase, stride);
    return true;
}
