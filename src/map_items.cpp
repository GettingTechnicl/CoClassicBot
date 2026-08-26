#include "map_items.h"
#include "CGameMap.h"
#include "itemtype.h"
#include "mapdata.h"
#include "game.h"

#include <windows.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <mutex>

namespace
{
    std::vector<CMapItem*> g_front;
    std::vector<CMapItem*> g_back;
    std::mutex             g_backMutex;
    std::atomic<bool>      g_backReady{ false };
    std::atomic<bool>      g_forceRescan{ true };
    std::atomic<bool>      g_threadStarted{ false };
    std::atomic<uint32_t>  g_refreshIntervalMs{ MapItems::kDefaultRefreshIntervalMs };
    MapItems::Stats        g_stats{};
    std::mutex             g_statsMutex;

    template <class T>
    bool TryRead(uintptr_t a, T* out)
    {
        __try { *out = *reinterpret_cast<volatile T*>(a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // CMapItem signature (no vtable available, unlike CRole):
    //   idType resolves through the item table OR the known money-typeId range,
    //   id is non-zero, position falls inside the current map's dimensions.
    // Money's typeId range is narrow (1090000-1091020, see IsMoneyMapItem in
    // hunt_town.cpp) and doesn't collide with real item typeIds (which run in
    // the hundred-thousands+), so it's safe to accept alongside a table hit.
    bool LooksLikeMapItem(uintptr_t p, int mapW, int mapH)
    {
        uint32_t id = 0, idType = 0;
        int32_t px = 0, py = 0;
        if (!TryRead(p, &id) || id == 0)
            return false;
        if (!TryRead(p + 4, &idType) || idType == 0)
            return false;
        if (!TryRead(p + 8, &px) || !TryRead(p + 12, &py))
            return false;
        if (px <= 0 || py <= 0 || px >= mapW || py >= mapH)
            return false;

        const bool isMoney = idType >= 1090000 && idType <= 1091020;
        if (!isMoney && !GetItemTypeInfo(idType))
            return false;
        return true;
    }

    void Rescan()
    {
        const DWORD t0 = GetTickCount();
        std::vector<CMapItem*> found;

        MapGrid* grid = GetCurrentMapGrid();
        if (!grid || !grid->IsLoaded()) {
            std::lock_guard<std::mutex> lk(g_backMutex);
            g_back.swap(found);
            g_backReady.store(true, std::memory_order_release);
            return;
        }
        const int mapW = grid->GetWidth();
        const int mapH = grid->GetHeight();

        uint32_t seen[1024];
        int nseen = 0;

        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t addr = 0x10000;
        uint64_t scanned = 0;
        const uint64_t kScanCap = 40000000ull;

        while (addr < 0x7FFFFFFF0000ull && nseen < 1024 && scanned < kScanCap) {
            if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi)))
                break;
            const uintptr_t rbase = (uintptr_t)mbi.BaseAddress;
            const size_t    rsize = mbi.RegionSize;
            const bool usable = mbi.State == MEM_COMMIT
                             && mbi.Type == MEM_PRIVATE
                             && (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE);

            if (usable && rsize > 0x20 && rsize < 0x4000000) {
                for (uintptr_t a = rbase; a + 0x20 < rbase + rsize && nseen < 1024; a += 0x10) {
                    ++scanned;
                    if (!LooksLikeMapItem(a, mapW, mapH))
                        continue;

                    uint32_t id = 0;
                    TryRead(a, &id);
                    bool dup = false;
                    for (int k = 0; k < nseen; ++k)
                        if (seen[k] == id) { dup = true; break; }
                    if (dup)
                        continue;

                    seen[nseen++] = id;
                    found.push_back(reinterpret_cast<CMapItem*>(a));
                }
            }
            if (!rsize) break;
            addr = rbase + rsize;
        }

        const uint32_t elapsed = GetTickCount() - t0;
        static uint32_t s_scans = 0;
        ++s_scans;
        {
            std::lock_guard<std::mutex> lk(g_backMutex);
            g_back.swap(found);
        }
        {
            std::lock_guard<std::mutex> lk(g_statsMutex);
            g_stats = { nseen, elapsed, s_scans };
        }
        g_backReady.store(true, std::memory_order_release);

        static bool loggedOnce = false;
        if (!loggedOnce) {
            spdlog::info("[mapitems] first scan: {} ground items in {}ms", nseen, elapsed);
            loggedOnce = true;
        }
    }

    DWORD WINAPI ScanThread(LPVOID)
    {
        for (;;) {
            if (Game::GetHero() != nullptr)
                Rescan();
            else
                g_forceRescan.store(true);

            const int intervalMs = (int)g_refreshIntervalMs.load(std::memory_order_relaxed);
            for (int waited = 0; waited < intervalMs; waited += 25) {
                if (g_forceRescan.exchange(false))
                    break;
                Sleep(25);
            }
        }
    }

    void EnsureThread()
    {
        bool expected = false;
        if (g_threadStarted.compare_exchange_strong(expected, true)) {
            if (HANDLE h = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr))
                CloseHandle(h);
        }
    }
}

namespace MapItems
{
    void SetRefreshIntervalMs(uint32_t ms)
    {
        g_refreshIntervalMs.store(ms, std::memory_order_relaxed);
    }

    uint32_t GetRefreshIntervalMs()
    {
        return g_refreshIntervalMs.load(std::memory_order_relaxed);
    }

    const std::vector<CMapItem*>& Get()
    {
        EnsureThread();
        if (g_backReady.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(g_backMutex);
            g_front.swap(g_back);
            g_backReady.store(false, std::memory_order_relaxed);
        }
        return g_front;
    }

    void Invalidate() { g_forceRescan.store(true); }

    bool IsAlive(const CMapItem* item)
    {
        if (!item)
            return false;
        MapGrid* grid = GetCurrentMapGrid();
        const int mapW = grid ? grid->GetWidth() : 4096;
        const int mapH = grid ? grid->GetHeight() : 4096;
        return LooksLikeMapItem(reinterpret_cast<uintptr_t>(item), mapW, mapH);
    }

    Stats GetStats()
    {
        EnsureThread();
        std::lock_guard<std::mutex> lk(g_statsMutex);
        return g_stats;
    }
}
