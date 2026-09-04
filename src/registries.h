#pragma once
// =====================================================================
// registries.h — read the game's OWN object registries (v1074).
//
// Found 2026-09-03 by inverse-pointer search (src/heapfind.cpp v3-v6; see
// HEAP_SCANNER_REPLACEMENT.md). Two registries:
//
//   items : base+MAP_ITEM_VEC = std::vector<std::shared_ptr<CMapItem>> — a GLOBAL,
//           and every entry's control block has _Uses == 1, i.e. the vector is
//           the item's SOLE owner: an item leaves it the instant the game frees it.
//
//   roles : a "role-set" object holding two std::vector<std::shared_ptr<CRole>>
//           0x30 apart (ROLE_SET_ALL at +0x1318, ROLE_SET_ROLES at +0x1348).
//           v6 first reached it via *(base+SCENE_PTR)+0x4E0 — WRONG: two live
//           runs gave different results (garbage on one account, a different
//           object entirely on another), because the scene pointer is
//           per-map/per-instance and +0x4E0 isn't reliably the role-set field
//           there. A THIRD run chasing a "stable static RVA" instead found two
//           different, non-reproducing RVAs across two sessions — also a dead
//           end, and for the same underlying reason: fishing for a specific
//           byte offset from a moving or ambiguous source is fragile no matter
//           how many hops it takes.
//
//           The fix: don't anchor on an address, anchor on the SIGNATURE. The
//           role-set object is unmistakable — two shared_ptr<CRole> vectors
//           exactly 0x30 apart, the ROLES one containing the hero. Locate it
//           by that signature starting from CRoleMgr (base+ROLE_MGR_PTR,
//           LIVE-VERIFIED, relog-stable, and the natural owner of the current
//           role set — unlike the scene), a small bounded object so the scan
//           costs nothing. Cache the result; every read revalidates it for
//           free while copying the vector out (hero must still be present).
//           A relog, map change, or client patch that moves the object just
//           fails that check once and the next call re-locates it — immune to
//           all three by construction, the same philosophy that made the item
//           fix robust, applied one level up.
//
// Entries are MSVC shared_ptr pairs {object, controlBlock} (16 bytes), objects
// made with make_shared (controlBlock == object - 0x10). Each read validates
// {begin,end,cap} and every pair, so a torn read (the game resizing the vector
// under us) fails closed instead of returning garbage.
// =====================================================================
#include "base.h"
#include <vector>

class CRole;
struct CMapItem;

namespace Registries
{
    // Object vtable RVAs observed live in the role vector (2026-09-03). Only
    // VT_MONSTER/VT_HERO objects pass entities.cpp's CRole shape check; the
    // VT_ROLE_B population is scenery/passive objects (see ROLE_B_IDENTITY.md
    // and the one-shot [role_b] diagnostic in registries.cpp).
    constexpr uintptr_t VT_MONSTER = 0x5CFA70;
    constexpr uintptr_t VT_HERO    = 0x5CEF60;
    constexpr uintptr_t VT_ROLE_B  = 0x5CDB10;
    constexpr uintptr_t VT_OBJ_A   = 0x5CDE68;   // non-role scene objects (all-objects vector only)
    constexpr uintptr_t VT_OBJ_C   = 0x5CD928;

    // Each returns false (and leaves `out` untouched) if the source object
    // can't be found/validated this call — callers fall back to the heap scan.
    bool ReadRoles(std::vector<CRole*>& out);
    bool ReadAllObjects(std::vector<CRole*>& out);
    bool ReadItems(std::vector<CMapItem*>& out);

    struct Status
    {
        bool      rolesOk, itemsOk;       // last read succeeded
        uint32_t  rolesSize, itemsSize;   // live element counts from the last successful read
        uint32_t  rolesFail, itemsFail;   // cumulative failed reads (diagnostic)
        uintptr_t roleSetObj;             // last-located role-set object, 0 if not currently located
    };
    Status GetStatus();
}
