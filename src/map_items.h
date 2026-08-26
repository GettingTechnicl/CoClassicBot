#pragma once
// =====================================================================
// map_items.h — live ground-item (loot) enumeration for v1074.
//
// WHY THIS EXISTS
// ---------------
// Every consumer of ground items (hunt_loot, hunt_buffs potion pickup,
// mining_plugin, two overlay displays) reads CGameMap::m_vecItems. That
// field has NEVER been populated correctly on this client build: before
// session 10 Game::GetMap() returned a confirmed-garbage pointer, and after
// session 10's terrain fix it returns a synthetic CGameMap built from the
// .DMap file on disk — which has cell/dimension/id data but no equivalent
// source for ground items, so m_vecItems stays default-empty forever.
// Net effect: loot pickup (money, plussed gear, everything) has silently
// found nothing this entire project.
//
// Same fix pattern as entities.h: heap-scan for the struct's signature.
// CMapItem (see CGameMap.h) has NO vtable, so the signature instead anchors
// on idType resolving through the item table (or the known money-typeId
// range) plus the position falling inside the current map's bounds.
// =====================================================================
#include "base.h"
#include <vector>

struct CMapItem;

namespace MapItems
{
    constexpr uint32_t kDefaultRefreshIntervalMs = 500;

    // Session 10: runtime-configurable so the Advanced tab can expose it as a
    // slider (shares the same underlying knob entities.h exposes). Takes
    // effect on the scan thread's next wait-loop iteration, not immediately.
    void SetRefreshIntervalMs(uint32_t ms);
    uint32_t GetRefreshIntervalMs();

    // Live ground-item list. Rescans on the interval; pointers are raw CMapItem*
    // into the game's heap (SEH-guard any read at the call site — items can
    // despawn between scans).
    const std::vector<CMapItem*>& Get();

    void Invalidate();

    // Re-validate a pointer previously returned by Get() before using it.
    //
    // CRITICAL: the background scan only refreshes every kRefreshIntervalMs.
    // The moment an item is picked up (by us or anyone else) or despawns, the
    // game frees its memory, but callers can keep receiving the SAME cached
    // pointer for up to that whole interval — and dereferencing a freed
    // CMapItem* is a hard crash, not a bad read. This is far more dangerous
    // for items than it is for entities.h's CRole* pointers, because pickup
    // is a routine, frequent, direct consequence of the very code path that
    // holds the pointer (unlike a monster dying, which is comparatively rare
    // and not something the bot triggers every few seconds by design).
    // SEH-guarded: safe to call on a genuinely dangling pointer.
    bool IsAlive(const CMapItem* item);

    struct Stats { int total; uint32_t lastScanMs; uint32_t scans; };
    Stats GetStats();
}
