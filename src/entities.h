#pragma once
// =====================================================================
// entities.h — live entity enumeration for v1074.
//
// WHY THIS EXISTS
// ---------------
// The bot originally enumerated entities by iterating
// `CRoleMgr::m_deqRole`, declared as a std::deque<PRole> at CRoleMgr+0x70.
// On v1074 that is no longer a deque: a live dump of the role manager
// showed CRoleMgr+0x40 onward is an array of ~0x20-byte records
// {rolePtr, 0, flags, hash} — i.e. a hash-map bucket array. Iterating it
// as a deque reads deque bookkeeping (size//block pointers) out of memory
// that holds something else entirely, which yields garbage at best and
// dereferences a garbage pointer at worst.
//
// Rather than reverse the container layout, this uses the approach already
// proven live in explorer.cpp: scan the heap for objects carrying a CRole
// signature. That was verified in Twin City finding 44 monsters (including
// 2 Guards), the Conductress NPC, and the hero — with every name, position
// and maxHP field reading correctly across all of them.
//
// COST / CACHING
// --------------
// A full heap scan is far too expensive to run per frame, so results are
// cached and the scan re-runs at most every kRefreshIntervalMs. Callers get
// raw CRole* pointers and read live fields (position, HP, status) straight
// off them, so cached entries stay current between scans — only the SET of
// entities is refreshed on the interval, not their contents.
// =====================================================================
#include "base.h"
#include <vector>

class CRole;

namespace Entities
{
    // Default cache lifetime. Monsters do not spawn/despawn fast enough for
    // a shorter interval to matter, and the scan is expensive.
    constexpr uint32_t kDefaultRefreshIntervalMs = 500;

    // Session 10: runtime-configurable so the Advanced tab can expose it as a
    // slider (also doubles as the lever for testing whether crash frequency
    // tracks scan load — see coclassic-live-offsets memory notes). Takes
    // effect on the scan thread's next wait-loop iteration, not immediately.
    void SetRefreshIntervalMs(uint32_t ms);
    uint32_t GetRefreshIntervalMs();

    // Start the background scan thread. Safe to call repeatedly; Get() and
    // GetStats() also call it, but calling it explicitly at init means the
    // first list is ready before anything asks for it.
    void Start();

    // Live entity list (hero included). Rescans if the cache has expired.
    // Pointers are validated by signature at scan time and re-validated
    // cheaply by IsAlive() before use.
    std::vector<CRole*> Get();

    // Force a rescan on the next Get(), e.g. after a map change or teleport.
    void Invalidate();

    // Cheap re-validation of a single cached pointer: confirms the object
    // still carries a CRole signature (vtable in image, sane id/position).
    // An entity that despawned between scans will fail this.
    bool IsAlive(const CRole* role);

    // Diagnostics for the overlay. The funnel counters exist so a "found 0"
    // result identifies WHICH predicate rejected everything instead of
    // requiring another guess-and-test cycle.
    struct Stats
    {
        int      monsters, players, npcs, other, total;
        uint32_t lastScanMs;
        uint32_t regions;      // committed private RW regions examined
        uint64_t addrs;        // candidate addresses stepped over
        uint32_t passVtable;   // vtable pointed into the image
        uint32_t passId;       // ...and had a plausible entity id
        uint32_t passPos;      // ...and a map position in range
        uint32_t passName;     // ...and a printable name of length >= 3
        uint32_t scans;        // completed scans since injection
    };
    Stats GetStats();

    // ── Compatibility view ──
    // The old call sites iterated a std::deque<PRole> and used .empty(),
    // .size(), [i], plus `if (!ref)`, `ref.get()` and `ref->` on elements.
    // This exposes that same surface over the scanned list so those ~25 loops
    // become a one-token substitution (`mgr->m_deqRole` -> `Entities::Roles()`)
    // rather than 25 hand-restructured loops, each a chance to introduce a bug.
    struct RoleRef
    {
        CRole* p = nullptr;
        explicit operator bool() const { return p != nullptr; }
        CRole* get() const { return p; }
        CRole* operator->() const { return p; }
        operator CRole*() const { return p; }
    };

    class View
    {
    public:
        bool   empty() const;
        size_t size() const;
        RoleRef operator[](size_t i) const;
    };

    View Roles();
}
