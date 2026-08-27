#include "entities.h"
#include "CRole.h"
#include "game.h"
#include "spawn_memory.h"

#include <windows.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <mutex>

namespace
{
    // Threading model: the scan is expensive (it walks committed private
    // memory), and every consumer runs on the render thread inside HkPresent.
    // Running it there would stutter the client, so a dedicated worker thread
    // scans into a back buffer and Get() swaps it to the front.
    //
    // The swap is safe because there is exactly ONE consumer thread: Get()
    // publishes at most once per call, and the returned reference then stays
    // stable for the remainder of that frame's work.
    std::vector<CRole*> g_front;        // read by consumers
    std::vector<CRole*> g_back;         // written by the scan thread
    std::mutex          g_backMutex;
    std::atomic<bool>   g_backReady{ false };
    std::atomic<bool>   g_forceRescan{ true };
    std::atomic<bool>   g_threadStarted{ false };
    std::atomic<uint32_t> g_refreshIntervalMs{ Entities::kDefaultRefreshIntervalMs };

    // Session 13: the heap scan below (LooksLikeRole) has no per-object
    // "which map" field to check — a CRole left over in memory from the map
    // just vacated can keep passing the exact same signature check on the
    // new map (live-reported: stale entities showing on the minimap right
    // after a map change). Get() detects the map-id change on every call
    // (not gated by scan cadence) and (a) forces an early rescan so the
    // visible list catches up fast, (b) opens a short grace window during
    // which callers that feed this scan into anything PERSISTENT
    // (SpawnMemory, HuntContest) should not trust it yet — see
    // IsMapDataSettled() below. Does not fully eliminate a leftover object
    // surviving into the very next scan (that needs a real per-object map
    // tag, not yet found), just bounds how long it can influence anything
    // that remembers what it saw.
    std::atomic<OBJID> g_lastKnownMapId{ 0 };
    std::atomic<DWORD> g_mapDataSettledTick{ 0 };
    constexpr DWORD kMapChangeGraceMs = 1500;
    Entities::Stats     g_stats{};
    std::mutex          g_statsMutex;

    // Image bounds, resolved once from the PE headers rather than hardcoded —
    // explorer.cpp used a literal 0x28C5000 size, which silently goes wrong
    // the moment the client is patched.
    struct ImageRange { uintptr_t lo, hi; };

    const ImageRange& GetImageRange()
    {
        static ImageRange r = []() -> ImageRange {
            const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
            if (!base)
                return { 0, 0 };
            const auto* dos = (const IMAGE_DOS_HEADER*)base;
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return { base, base + 0x4000000 };
            const auto* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return { base, base + 0x4000000 };
            return { base, base + nt->OptionalHeader.SizeOfImage };
        }();
        return r;
    }

    template <class T>
    bool TryRead(uintptr_t a, T* out)
    {
        __try { *out = *reinterpret_cast<volatile T*>(a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // CRole signature check (same predicate explorer.cpp validated live):
    // vtable points into the image, id is a plausible entity id, and the map
    // position is in range. The name-length test is applied separately by the
    // caller because it needs a bounded string read.
    struct Funnel { uint32_t vtable, id, pos; };

    bool LooksLikeRole(uintptr_t p, uint32_t* idOut, Funnel* fn = nullptr)
    {
        const ImageRange& img = GetImageRange();
        uint64_t vt = 0;
        if (!TryRead(p, &vt) || vt < img.lo || vt >= img.hi)
            return false;
        if (fn) ++fn->vtable;

        uint32_t id = 0;
        int32_t  px = 0, py = 0;
        if (!TryRead(p + 0x68, &id) || id == 0 || id >= 5000000)
            return false;
        if (fn) ++fn->id;

        if (!TryRead(p + 0xD8, &px) || !TryRead(p + 0xDC, &py))
            return false;
        if (px <= 0 || px > 1000 || py <= 0 || py > 1000)
            return false;
        if (fn) ++fn->pos;

        *idOut = id;
        return true;
    }

    // Real entities have real names. This is what fixed explorer's false
    // positives (ids 1/23/46/48 and a DemonArmor typeId collision at 130455).
    bool HasRealName(uintptr_t p)
    {
        char name[16] = {};
        for (int i = 0; i < 15; ++i) {
            char c = 0;
            if (!TryRead(p + 0x94 + i, &c))
                return false;
            name[i] = c;
            if (!c)
                break;
        }
        int len = 0;
        while (len < 15 && name[len])
            ++len;
        if (len < 3)
            return false;
        for (int i = 0; i < len; ++i) {
            const unsigned char c = (unsigned char)name[i];
            if (c < 0x20 || c > 0x7E)
                return false;
        }
        return true;
    }

    void Rescan()
    {
        const DWORD t0 = GetTickCount();

        std::vector<CRole*> found;
        found.reserve(128);
        uint32_t seen[512];
        int nseen = 0;
        int nmon = 0, nplr = 0, nnpc = 0, noth = 0;

        Funnel funnel{};
        uint32_t nregions = 0, npassName = 0;

        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t addr = 0x10000;
        uint64_t scanned = 0;
        const uint64_t kScanCap = 40000000ull;

        while (addr < 0x7FFFFFFF0000ull && nseen < 512 && scanned < kScanCap) {
            if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi)))
                break;
            const uintptr_t rbase = (uintptr_t)mbi.BaseAddress;
            const size_t    rsize = mbi.RegionSize;

            const bool usable = mbi.State == MEM_COMMIT
                             && mbi.Type == MEM_PRIVATE
                             && (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE);

            if (usable && rsize > 0x100 && rsize < 0x4000000) {
                ++nregions;
                for (uintptr_t a = rbase; a + 0xE0 < rbase + rsize && nseen < 512; a += 0x10) {
                    ++scanned;
                    uint32_t id = 0;
                    if (!LooksLikeRole(a, &id, &funnel))
                        continue;
                    if (!HasRealName(a))
                        continue;
                    ++npassName;

                    bool dup = false;
                    for (int k = 0; k < nseen; ++k)
                        if (seen[k] == id) { dup = true; break; }
                    if (dup)
                        continue;

                    seen[nseen++] = id;
                    found.push_back(reinterpret_cast<CRole*>(a));

                    if (id >= 1000000)                    ++nplr;
                    else if (id >= 400000 && id < 500000) ++nmon;
                    else if (id < 400000)                 ++nnpc;
                    else                                  ++noth;
                }
            }

            if (rsize == 0)
                break;
            addr = rbase + rsize;
        }

        const uint32_t elapsed = GetTickCount() - t0;

        {
            std::lock_guard<std::mutex> lk(g_backMutex);
            g_back.swap(found);
        }
        static uint32_t s_scans = 0;
        ++s_scans;
        {
            std::lock_guard<std::mutex> lk(g_statsMutex);
            g_stats = { nmon, nplr, nnpc, noth, nseen, elapsed,
                        nregions, scanned, funnel.vtable, funnel.id, funnel.pos,
                        npassName, s_scans };
        }
        g_backReady.store(true, std::memory_order_release);

        // Log the first few scans and then only on change, so a "found 0"
        // shows its full funnel in coclassic.log without spamming it.
        static int  s_logged = 0;
        static int  s_lastTotal = -1;
        if (s_logged < 3 || nseen != s_lastTotal) {
            spdlog::info("[entities] scan #{}: {} entities ({} mon, {} plr, {} npc) in {}ms | "
                         "regions={} addrs={} vtable={} id={} pos={} name={}",
                         s_scans, nseen, nmon, nplr, nnpc, elapsed,
                         nregions, scanned, funnel.vtable, funnel.id, funnel.pos, npassName);
            ++s_logged;
            s_lastTotal = nseen;
        }
    }

    DWORD WINAPI ScanThread(LPVOID)
    {
        for (;;) {
            // Skip scanning entirely until the game is actually in-world;
            // otherwise we burn CPU walking the heap at the login screen.
            if (Game::GetRoleMgr() != nullptr)
                Rescan();
            else
                g_forceRescan.store(true);

            // Rescan on the normal cadence, but wake early if someone called
            // Invalidate() (map change, teleport). Interval is re-read each
            // outer loop so a slider change takes effect within one cycle.
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

namespace Entities
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
    // copy instead closes this off structurally — at most a few hundred
    // pointers, trivially cheap — without having to prove which call site
    // actually re-enters. Individual CRole* staleness is a separate,
    // already-handled concern (see IsAlive() below); this only protects the
    // container itself.
    std::vector<CRole*> Get()
    {
        EnsureThread();

        // Detect a map change on every call, independent of scan cadence —
        // see g_lastKnownMapId's comment above. Only a genuine map-to-map
        // transition (both ids non-zero and different) opens the grace
        // window; the very first call after login/injection (prevMapId==0)
        // shouldn't delay anything.
        const OBJID curMapId = Game::GetCurrentMapId();
        const OBJID prevMapId = g_lastKnownMapId.exchange(curMapId, std::memory_order_relaxed);
        if (curMapId && prevMapId && curMapId != prevMapId) {
            g_forceRescan.store(true);
            g_mapDataSettledTick.store(GetTickCount() + kMapChangeGraceMs, std::memory_order_relaxed);
        }

        std::lock_guard<std::mutex> lk(g_backMutex);

        // Publish a completed scan if one is waiting. Never blocks on the
        // scan itself — worst case the caller sees the previous frame's list,
        // which is fine: entity FIELDS are read live through these pointers,
        // only the membership of the set is up to kRefreshIntervalMs stale.
        if (g_backReady.load(std::memory_order_acquire)) {
            g_front.swap(g_back);
            g_backReady.store(false, std::memory_order_relaxed);

            // Feed spawn memory from the freshly published list. Done here
            // because this is the one place a NEW scan becomes visible — doing
            // it per-frame would count the same standing monsters repeatedly
            // and reward "wherever the bot is loitering" rather than where
            // monsters actually appear. Skipped during the post-map-change
            // grace window (see above) so a leftover object from the map
            // just vacated can't record a phantom sighting on the new map.
            if (curMapId && GetTickCount() >= g_mapDataSettledTick.load(std::memory_order_relaxed)) {
                std::vector<Position> monsters;
                monsters.reserve(g_front.size());
                for (CRole* r : g_front) {
                    if (r && r->IsMonster() && !r->IsDead())
                        monsters.push_back(r->m_posMap);
                }
                if (!monsters.empty())
                    SpawnMemory::Observe(curMapId, monsters);
            }
        }
        return g_front;
    }

    void Invalidate() { g_forceRescan.store(true); }

    bool IsMapDataSettled()
    {
        return GetTickCount() >= g_mapDataSettledTick.load(std::memory_order_relaxed);
    }

    bool IsAlive(const CRole* role)
    {
        if (!role)
            return false;
        uint32_t id = 0;
        return LooksLikeRole(reinterpret_cast<uintptr_t>(role), &id);
    }

    void Start() { EnsureThread(); }

    Stats GetStats()
    {
        // Must start the worker here too: the overlay's status line reads
        // stats without ever calling Get(), so relying on Get() alone to
        // lazy-start the thread meant it never started at all.
        EnsureThread();
        std::lock_guard<std::mutex> lk(g_statsMutex);
        return g_stats;
    }

}
