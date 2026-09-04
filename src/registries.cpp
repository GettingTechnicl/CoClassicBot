#include "registries.h"
#include "game.h"

#include <windows.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <mutex>

namespace
{
    template <class T>
    bool TryRead(uintptr_t a, T* out)
    {
        __try { *out = *reinterpret_cast<volatile T*>(a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool IsHeapPtr(uint64_t p) { return p > 0x10000 && p < 0x7FFFFFFFFFFFull && (p & 7) == 0; }

    struct Vec { uintptr_t begin, end, cap; };

    bool ReadVecHeader(uintptr_t at, Vec& v)
    {
        if (!at) return false;
        if (!TryRead(at, &v.begin) || !TryRead(at + 8, &v.end) || !TryRead(at + 16, &v.cap)) return false;
        if (v.begin == 0 && v.end == 0) { v.cap = 0; return true; }   // empty, no buffer yet
        if (!IsHeapPtr(v.begin) || v.end < v.begin || v.cap < v.end) return false;
        if ((v.end - v.begin) % 16 != 0) return false;
        return true;
    }

    // Copy the object pointers out of the pair buffer. A pair whose halves
    // aren't both heap pointers means we raced a resize (or the source moved):
    // fail the whole read rather than publish a half-torn list.
    template <class T>
    bool CopyPairs(const Vec& v, std::vector<T*>& out, uint64_t maxElems)
    {
        const size_t n = (size_t)((v.end - v.begin) / 16);
        if (n > maxElems) return false;
        std::vector<T*> tmp; tmp.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            uint64_t obj = 0, ctrl = 0;
            if (!TryRead(v.begin + 16 * i, &obj) || !TryRead(v.begin + 16 * i + 8, &ctrl)) return false;
            if (!obj && !ctrl) continue;                                          // empty shared_ptr slot
            if (!IsHeapPtr(obj) || !IsHeapPtr(ctrl)) return false;
            tmp.push_back(reinterpret_cast<T*>(obj));
        }
        out.swap(tmp);
        return true;
    }

    // ── role-set locator ──────────────────────────────────────────────────
    // Signature, not an address: two shared_ptr<CRole> vectors 0x30 apart
    // (ROLE_SET_ALL at +0x1318, ROLE_SET_ROLES at +0x1348), the ROLES one
    // containing the hero. See registries.h for why an address-based anchor
    // (the scene pointer, then a "stable RVA" hunt) failed twice.
    bool RolesVecContainsHero(const Vec& roles, uintptr_t hero)
    {
        const size_t n = (size_t)((roles.end - roles.begin) / 16);
        if (n == 0 || n > 4096) return false;
        for (size_t i = 0; i < n; ++i) {
            uint64_t obj = 0;
            if (!TryRead(roles.begin + 16 * i, &obj)) return false;
            if (obj == hero) return true;
        }
        return false;
    }

    bool LooksLikeRoleSet(uintptr_t obj, uintptr_t hero)
    {
        Vec all{}, roles{};
        if (!ReadVecHeader(obj + Offsets::ROLE_SET_ALL, all)) return false;
        if (!ReadVecHeader(obj + Offsets::ROLE_SET_ROLES, roles)) return false;
        if (roles.begin == 0) return false;                       // must hold at least the hero
        return RolesVecContainsHero(roles, hero);
    }

    // Known-good 2-hop path (heapfind v7, 2026-09-03): CRoleMgr+0x4A98 ->
    // intermediate object +0x9240 -> role-set. Tried first (near-zero cost)
    // but never trusted blind — it's re-validated by the same signature check
    // as everything else, so a layout change just fails it and falls through.
    uintptr_t FastPath(uintptr_t roleMgr, uintptr_t hero)
    {
        uint64_t mid = 0;
        if (!TryRead(roleMgr + Offsets::ROLE_MGR_TO_ROLESET_HOP1, &mid) || !IsHeapPtr(mid)) return 0;
        uint64_t rs = 0;
        if (!TryRead((uintptr_t)mid + Offsets::ROLE_MGR_TO_ROLESET_HOP2, &rs) || !IsHeapPtr(rs)) return 0;
        return LooksLikeRoleSet((uintptr_t)rs, hero) ? (uintptr_t)rs : 0;
    }

    // Bounded search from CRoleMgr: depth 0 (CRoleMgr itself), depth 1 (its
    // own fields), depth 2 (one hop further from each depth-1 heap pointer).
    // Only runs when the fast path and the cache both miss.
    //
    // Live evidence (2026-09-03): the fast path found the role-set once, then
    // failed to relocate minutes later on the SAME CRoleMgr, and missed it
    // entirely on a second account — its "intermediate" hop is not the stable
    // structural offset ROLE_SET_ROLES/ALL turned out to be, so this fallback
    // has to carry real weight, not just exist on paper. It ALSO regressed
    // scan time to 6+ seconds on a third process while repeatedly failing:
    // the wide kDepth2Span (needed to reach heapfind v7's real +0x9240 hop)
    // means many of the ~4096*2048 speculative reads land on unmapped memory,
    // and SEH exception dispatch on a miss costs orders of magnitude more
    // than a hit.
    //
    // A flat per-attempt budget fixed the stall but broke completeness: every
    // throttled retry restarted the sweep at offset 0 and hit the SAME budget
    // wall at the SAME candidate every time — it could never progress past
    // whatever the first attempt covered. Fixed properly: depth-1 candidates
    // are collected once (cheap, ~4096 reads) and cached; the depth-2 sweep
    // processes a few candidates per call and RESUMES from where the last
    // call left off, so repeated 2s-throttled attempts sweep the whole
    // candidate set over time instead of re-treading the same dead end.
    constexpr uintptr_t kDepth1Span            = 0x8000;   // 4096 qwords off CRoleMgr
    constexpr uintptr_t kDepth2Span            = 0x10000;  // 8192 qwords per depth-1 candidate (needs to cover +0x9240)
    constexpr int        kMaxCandidates        = 600;
    // The inner sweep now enforces its own clock check every 256 reads, so
    // this is a soft cap for the fast-candidate case, not the real safety net
    // — kTimeBudgetMs is. Raised from 5: with the inner check in place, more
    // candidates per throttled attempt only helps convergence, never risks a
    // stall.
    constexpr int        kCandidatesPerAttempt = 40;
    constexpr DWORD       kTimeBudgetMs        = 20;   // wall-clock cap per attempt (checked at both loop levels)

    uintptr_t          g_candRoleMgr = 0;             // which CRoleMgr the candidate list below was built for
    uintptr_t          g_candidates[kMaxCandidates];
    int                g_candidateCount = -1;         // -1 = needs (re)building
    int                g_candidateResumeIdx = 0;

    // Called only after GetRoleSet's own FastPath attempt has already missed.
    uintptr_t LocateRoleSet(uintptr_t roleMgr, uintptr_t hero)
    {
        if (LooksLikeRoleSet(roleMgr, hero)) return roleMgr;

        if (g_candidateCount < 0 || g_candRoleMgr != roleMgr) {
            g_candRoleMgr = roleMgr;
            g_candidateCount = 0;
            g_candidateResumeIdx = 0;
            for (uintptr_t off = 0; off < kDepth1Span && g_candidateCount < kMaxCandidates; off += 8) {
                uint64_t v = 0;
                if (!TryRead(roleMgr + off, &v) || !IsHeapPtr(v)) continue;
                if (LooksLikeRoleSet((uintptr_t)v, hero)) return (uintptr_t)v;   // depth-1 direct hit
                g_candidates[g_candidateCount++] = (uintptr_t)v;
            }
        }

        // The candidate cap alone isn't a full guarantee: ONE candidate whose
        // depth-2 span happens to land mostly in unmapped memory could still
        // burn the full 8192-read sweep in SEH exception overhead before the
        // outer loop ever rechecks the clock — that was the original 6s-stall
        // mechanism. Check the clock inside the inner sweep too.
        const DWORD t0 = GetTickCount();
        int processed = 0;
        while (g_candidateResumeIdx < g_candidateCount && processed < kCandidatesPerAttempt && GetTickCount() - t0 < kTimeBudgetMs) {
            const uintptr_t cand = g_candidates[g_candidateResumeIdx++];
            ++processed;
            for (uintptr_t off2 = 0; off2 < kDepth2Span; off2 += 8) {
                // A candidate whose span is timing out is almost certainly not
                // a real object's memory (a genuine allocation reads fast); give
                // up on it and move to the next rather than getting stuck here.
                if ((off2 & 0x7FF) == 0 && GetTickCount() - t0 >= kTimeBudgetMs) break;
                uint64_t v2 = 0;
                if (!TryRead(cand + off2, &v2) || !IsHeapPtr(v2)) continue;
                if (LooksLikeRoleSet((uintptr_t)v2, hero)) return (uintptr_t)v2;
            }
            if (GetTickCount() - t0 >= kTimeBudgetMs) break;   // this call's time is spent; resume with the NEXT candidate next call
        }
        if (g_candidateResumeIdx >= g_candidateCount)
            g_candidateCount = -1;   // exhausted with no match — rebuild fresh (heap state may have moved on) next time
        return 0;
    }

    std::mutex g_roleSetMutex;
    uintptr_t  g_roleSetObj = 0;
    DWORD      g_lastLocateAttempt = 0;
    constexpr DWORD kRelocateThrottleMs = 2000;   // while unlocated, don't re-scan more than this often

    // Returns the current role-set object, (re-)locating it as needed. The
    // cached pointer is revalidated against `hero` on every call — cheap,
    // since it's the same membership check CopyPairs' caller needs anyway —
    // so a relog, map change, or the object simply moving is caught
    // immediately and just costs one throttled re-locate, not a crash or a
    // silently stale read.
    uintptr_t GetRoleSet(uintptr_t roleMgr, uintptr_t hero)
    {
        std::lock_guard<std::mutex> lk(g_roleSetMutex);
        if (g_roleSetObj && LooksLikeRoleSet(g_roleSetObj, hero)) return g_roleSetObj;
        const DWORD now = GetTickCount();
        if (g_roleSetObj == 0 && g_lastLocateAttempt != 0 && now - g_lastLocateAttempt < kRelocateThrottleMs)
            return 0;
        g_lastLocateAttempt = now;
        const bool wasSet = g_roleSetObj != 0;
        const uintptr_t fp = FastPath(roleMgr, hero);
        g_roleSetObj = fp ? fp : LocateRoleSet(roleMgr, hero);
        if (g_roleSetObj && !wasSet)
            spdlog::warn("[registries] role-set located via {}: {:#x}", fp ? "fast path" : "bounded fallback scan", g_roleSetObj);
        return g_roleSetObj;
    }

    std::atomic<bool>      g_rolesOk{ false }, g_itemsOk{ false };
    std::atomic<uint32_t>  g_rolesSize{ 0 }, g_itemsSize{ 0 }, g_rolesFail{ 0 }, g_itemsFail{ 0 };
    std::atomic<uintptr_t> g_roleSetObjPublic{ 0 };

    // Loud by design: the first outcome, every ok<->fail transition, and a
    // throttled repeat while failing all log at WARN so they survive the
    // user's log level. A once-a-minute heartbeat covers the case where the
    // very first outcome happens before the user has logging on at all.
    void Note(const char* what, std::atomic<bool>& okFlag, std::atomic<uint32_t>& fails, bool ok, size_t n, const char* detail)
    {
        static std::mutex s_m; std::lock_guard<std::mutex> lk(s_m);
        static bool  s_first[2] = { true, true };
        static DWORD s_lastFailLog[2] = { 0, 0 };
        const int    slot = (what[0] == 'r') ? 0 : 1;
        const bool   was = okFlag.exchange(ok);
        const DWORD  now = GetTickCount();
        if (ok) {
            if (!was || s_first[slot])
                spdlog::warn("[registries] {} registry ONLINE: {} live entries ({})", what, n, detail);
        } else {
            ++fails;
            if (s_first[slot] || was || now - s_lastFailLog[slot] > 30000) {
                s_lastFailLog[slot] = now;
                spdlog::warn("[registries] {} registry read FAILED (fails={}, falling back to heap scan) | {}", what, fails.load(), detail);
            }
        }
        s_first[slot] = false;
        static DWORD s_lastBeat = 0;
        if (now - s_lastBeat > 60000) {
            s_lastBeat = now;
            spdlog::info("[registries] status: roles {} size={} fails={} roleSet={:#x} | items {} size={} fails={}",
                         g_rolesOk.load() ? "ONLINE" : "offline", g_rolesSize.load(), g_rolesFail.load(), (uint64_t)g_roleSetObjPublic.load(),
                         g_itemsOk.load() ? "ONLINE" : "offline", g_itemsSize.load(), g_itemsFail.load());
        }
    }

    // One-shot identification of the VT_ROLE_B population (ROLE_B_IDENTITY.md):
    // id, name, position of the first dozen seen. Read-only, SEH-guarded.
    void DumpRoleB(const std::vector<CRole*>& roles)
    {
        static std::atomic<bool> s_done{ false };
        if (s_done.load() || !Game::Base()) return;
        int shown = 0, total = 0;
        for (CRole* r : roles) {
            const uintptr_t o = reinterpret_cast<uintptr_t>(r);
            uint64_t vt = 0;
            if (!TryRead(o, &vt) || vt != Game::Base() + Registries::VT_ROLE_B) continue;
            ++total;
            if (shown >= 12) continue;
            uint32_t id = 0; int32_t x = 0, y = 0; char name[16] = {};
            TryRead(o + 0x68, &id); TryRead(o + 0xD8, &x); TryRead(o + 0xDC, &y);
            for (int i = 0; i < 15; ++i) { char c = 0; if (!TryRead(o + 0x94 + i, &c) || !c) break; name[i] = (c >= 0x20 && c < 0x7F) ? c : '?'; }
            spdlog::warn("[role_b] #{} obj={:#x} id={} name='{}' pos=({},{})", shown, (uint64_t)o, id, name, x, y);
            ++shown;
        }
        if (total > 0) { spdlog::warn("[role_b] {} VT_ROLE_B objects in the roles vector (showed {})", total, shown); s_done.store(true); }
    }

    bool GetRoleMgrAndHero(uintptr_t& roleMgr, uintptr_t& hero)
    {
        return Game::Base()
            && TryRead(Game::Base() + Offsets::ROLE_MGR_PTR, &roleMgr) && IsHeapPtr(roleMgr)
            && TryRead(roleMgr, &hero) && IsHeapPtr(hero);
    }
}

namespace Registries
{
    bool ReadRoles(std::vector<CRole*>& out)
    {
        char detail[192] = "";
        bool ok = false;
        uintptr_t roleMgr = 0, hero = 0;
        if (GetRoleMgrAndHero(roleMgr, hero)) {
            const uintptr_t rs = GetRoleSet(roleMgr, hero);
            g_roleSetObjPublic.store(rs);
            if (rs) {
                Vec roles{};
                if (ReadVecHeader(rs + Offsets::ROLE_SET_ROLES, roles) && CopyPairs(roles, out, 8192)) {
                    ok = true;
                    snprintf(detail, sizeof detail, "roleSet=%#llx", (unsigned long long)rs);
                } else {
                    snprintf(detail, sizeof detail, "roleSet=%#llx but its roles vector failed to copy (torn read?)", (unsigned long long)rs);
                }
            } else {
                snprintf(detail, sizeof detail, "role-set object not found under CRoleMgr=%#llx", (unsigned long long)roleMgr);
            }
        } else {
            snprintf(detail, sizeof detail, "CRoleMgr/hero chain unavailable");
        }
        if (ok) { g_rolesSize.store((uint32_t)out.size()); DumpRoleB(out); }
        Note("role", g_rolesOk, g_rolesFail, ok, out.size(), detail);
        return ok;
    }

    bool ReadAllObjects(std::vector<CRole*>& out)
    {
        uintptr_t roleMgr = 0, hero = 0;
        if (!GetRoleMgrAndHero(roleMgr, hero)) return false;
        const uintptr_t rs = GetRoleSet(roleMgr, hero);
        if (!rs) return false;
        Vec all{};
        return ReadVecHeader(rs + Offsets::ROLE_SET_ALL, all) && CopyPairs(all, out, 16384);
    }

    bool ReadItems(std::vector<CMapItem*>& out)
    {
        char detail[128] = "";
        bool ok = false;
        if (Game::Base()) {
            Vec v{};
            if (ReadVecHeader(Game::Base() + Offsets::MAP_ITEM_VEC, v) && CopyPairs(v, out, 16384)) {
                ok = true;
                snprintf(detail, sizeof detail, "vec@%#llx", (unsigned long long)(Game::Base() + Offsets::MAP_ITEM_VEC));
            } else {
                snprintf(detail, sizeof detail, "global item vector failed validation/copy");
            }
        } else {
            snprintf(detail, sizeof detail, "module base not initialised");
        }
        if (ok) g_itemsSize.store((uint32_t)out.size());
        Note("item", g_itemsOk, g_itemsFail, ok, out.size(), detail);
        return ok;
    }

    Status GetStatus()
    {
        return { g_rolesOk.load(), g_itemsOk.load(), g_rolesSize.load(), g_itemsSize.load(),
                 g_rolesFail.load(), g_itemsFail.load(), g_roleSetObjPublic.load() };
    }
}
