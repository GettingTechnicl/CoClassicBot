#include "map_items.h"
#include "CGameMap.h"
#include "itemtype.h"
#include "mapdata.h"
#include "game.h"
#include "registries.h"

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

    // Session 14 [SCAN CAP INVESTIGATION]: was 1024. A user report of a
    // manually-dropped, confirmed-on-screen "+1" item never appearing ANYWHERE
    // in FindBestLoot's per-item trace (not even a skip line — meaning
    // MapItems::Get() itself never returned it, not that it was returned and
    // filtered) raised the question of whether this scan silently runs out of
    // its 1024-slot budget before reaching every real item. The scan walks
    // the WHOLE process's committed PAGE_READWRITE/MEM_PRIVATE address space
    // and counts any byte pattern that merely LOOKS like a CMapItem — that
    // includes stale, already-freed memory that hasn't been overwritten yet,
    // not just live items, so the true "distinct plausible matches in one
    // pass" count could exceed 1024 well before 200+ real live items alone
    // would. Raised 4x defensively (trivial cost — `seen` is a stack array,
    // `found` is already heap-allocated regardless of this constant) and
    // paired with a throttled warning below so a future cap-hit is visible
    // in the log instead of silently dropping unscanned items with no trace
    // at all. TODO: if the warning never fires in a future session, this
    // wasn't the actual explanation for the missed item — look elsewhere.
    constexpr int kMaxSeenItems = 4096;

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

        static uint32_t seen[kMaxSeenItems];
        int nseen = 0;
        uint64_t scanned = 0;

        auto consider = [&](uintptr_t a) {
            ++scanned;
            if (!LooksLikeMapItem(a, mapW, mapH))
                return;
            uint32_t id = 0;
            TryRead(a, &id);
            for (int k = 0; k < nseen; ++k)
                if (seen[k] == id) return;
            seen[nseen++] = id;
            found.push_back(reinterpret_cast<CMapItem*>(a));
        };

        // Preferred source: the game's global item vector (registries.h) — the
        // items' sole owner, so a picked-up or despawned item is gone from it
        // on the very next call. The heap walk is only the fallback.
        std::vector<CMapItem*> registry;
        const bool viaRegistry = Registries::ReadItems(registry);
        if (viaRegistry) {
            for (CMapItem* it : registry) {
                if (nseen >= kMaxSeenItems) break;
                consider(reinterpret_cast<uintptr_t>(it));
            }
        } else {
            MEMORY_BASIC_INFORMATION mbi{};
            uintptr_t addr = 0x10000;
            const uint64_t kScanCap = 40000000ull;

            while (addr < 0x7FFFFFFF0000ull && nseen < kMaxSeenItems && scanned < kScanCap) {
                if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi)))
                    break;
                const uintptr_t rbase = (uintptr_t)mbi.BaseAddress;
                const size_t    rsize = mbi.RegionSize;
                const bool usable = mbi.State == MEM_COMMIT
                                 && mbi.Type == MEM_PRIVATE
                                 && (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE);

                if (usable && rsize > 0x20 && rsize < 0x4000000) {
                    for (uintptr_t a = rbase; a + 0x20 < rbase + rsize && nseen < kMaxSeenItems; a += 0x10)
                        consider(a);
                }
                if (!rsize) break;
                addr = rbase + rsize;
            }
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

        // Session 14 [SCAN CAP INVESTIGATION]: see kMaxSeenItems' comment.
        // Throttled the same way hunt_loot.cpp's skip trace is — a sustained
        // cap-hit would otherwise log every ~500ms (the default refresh
        // interval) forever.
        if (nseen >= kMaxSeenItems) {
            static DWORD s_lastCapWarnTick = 0;
            const DWORD nowTick = GetTickCount();
            if (nowTick - s_lastCapWarnTick > 5000) {
                s_lastCapWarnTick = nowTick;
                spdlog::warn("[mapitems] Rescan HIT THE {}-ITEM CAP — some real ground items may be going unscanned this pass",
                    kMaxSeenItems);
            }
        }

        static bool loggedOnce = false;
        if (!loggedOnce) {
            spdlog::info("[mapitems] first scan ({}): {} ground items in {}ms", viaRegistry ? "registry" : "heap-scan", nseen, elapsed);
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

    // Session 10 [CRASH HARDENING]: used to return `const vector&` aliasing
    // g_front directly. Every caller iterates the result with NO lock held
    // (by design — the lock only needs to protect the swap itself), but that
    // means if a second Get() call anywhere lands while an earlier one is
    // still being iterated, its swap silently exchanges g_front's buffer out
    // from under the in-flight iteration; the scanner thread's next Rescan()
    // can then destroy that buffer via its own swap-and-drop of `found`,
    // leaving the original iterator dangling into freed memory. Returning a
    // copy instead closes this off structurally — at most 1024 pointers
    // (the scan cap), trivially cheap — without having to prove which call
    // site actually re-enters. Individual CMapItem* staleness is a separate,
    // already-handled concern (see IsAlive() below); this only protects the
    // container itself.
    std::vector<CMapItem*> Get()
    {
        EnsureThread();
        std::lock_guard<std::mutex> lk(g_backMutex);
        if (g_backReady.load(std::memory_order_acquire)) {
            g_front.swap(g_back);
            g_backReady.store(false, std::memory_order_relaxed);
        }
        return g_front;
    }

    void Invalidate() { g_forceRescan.store(true); }

    // Liveness from the game's own item vector (its sole owner): an item that
    // was picked up or despawned is gone from it immediately, whereas its bytes
    // keep passing the shape check until the heap reuses them — which was the
    // entire ghost-pickup mechanism. One snapshot is shared for 30ms since
    // this runs per candidate per frame. Without the registry the shape check
    // alone decides, as before.
    bool IsAlive(const CMapItem* item)
    {
        if (!item)
            return false;
        static std::mutex             s_snapMutex;
        static std::vector<CMapItem*> s_snap;
        static DWORD                  s_snapTick = 0;
        static bool                   s_snapValid = false;
        {
            std::lock_guard<std::mutex> lk(s_snapMutex);
            const DWORD now = GetTickCount();
            // Throttle on time only: while the registry is failing this must
            // NOT retry on every call (entities.cpp's version once did 9M/min).
            if (s_snapTick == 0 || now - s_snapTick > 30) {
                s_snapValid = Registries::ReadItems(s_snap);
                s_snapTick = now ? now : 1;
            }
            if (s_snapValid) {
                bool present = false;
                for (CMapItem* it : s_snap) if (it == item) { present = true; break; }
                if (!present)
                    return false;
            }
        }
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
