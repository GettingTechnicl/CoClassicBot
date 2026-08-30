#pragma once
#include "base.h"
#include "hunt_settings.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class CHero;
class CGameMap;
class CItem;
struct CMapItem;

// ── HuntLootManager ───────────────────────────────────────────────────────────
// Encapsulates loot finding, pickup tracking, and pickup attempt throttling.
// Lives as a member of BaseHuntPlugin and is driven each Update() frame.
class HuntLootManager {
public:
    // Find the best loot item on the current map given the hunt zone and settings.
    //
    // Session 10: returns a raw CMapItem* now, not shared_ptr. Ground items
    // come from a heap scan (map_items.h) rather than CGameMap::m_vecItems (see
    // that header for why) — we don't own these objects, we're just pointing at
    // them in the game's own memory, same as CRole* from the entity scanner.
    CMapItem* FindBestLoot(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings,
        std::function<bool(OBJID, DWORD)> isLootPickupIgnoredFn,
        std::function<bool(OBJID mapId, const Position&)> isPointInZoneFn) const;

    // Attempt to pick up an item the hero is standing on.
    // updatePendingJumpFn should call plugin's UpdatePendingJumpState and return its result.
    bool TryPickupLootItem(CHero* hero, const AutoHuntSettings& settings,
        const CMapItem* item, DWORD now,
        std::function<bool(DWORD)> updatePendingJumpFn);

    // ── Pickup-attempt tracking ───────────────────────────────────────────────
    // Non-const: promotes confirmed ghosts (see LootPickupAttemptState) so the
    // one-time promotion log fires exactly once per item.
    bool IsLootPickupIgnored(OBJID itemId, DWORD now);
    void RecordLootPickupAttempt(OBJID itemId, DWORD now,
        const AutoHuntSettings& settings);
    // Session 14 [PLUS VERIFY]: hero param added so a confirmed pickup (item
    // vanishes from the ground scan without ever ghosting) can look up the
    // resulting bag item and log its verified CItem::m_nAddition next to the
    // ground-side CMapItem::GetPlus() read captured at pickup time — see
    // TryPickupLootItem and this function's body for the full mechanism.
    void PruneLootPickupAttempts(CHero* hero, CGameMap* map);
    void ResetLootPickupAttempts();

    // ── Phase 2a: bag-full trash drop ─────────────────────────────────────────
    // When the bag is at/above bagStoreThreshold and autoDropTrashWhenFull is on,
    // attempt to drop one inventory item that fails the user's keep filters.
    // Returns true if a DropItem packet was sent this tick (caller should yield
    // the rest of its frame to let the packet land before doing other actions).
    bool TryDropTrashItem(CHero* hero, const AutoHuntSettings& settings, DWORD now);

    // Predicate: would this bagged item be dropped under the current settings?
    // Static so it can be used by UI counters too.  Strict safety filters apply.
    static bool IsBagItemTrash(const AutoHuntSettings& settings, const CItem& item);

    // ── State accessors ───────────────────────────────────────────────────────
    DWORD GetLastLootTick()   const { return m_lastLootTick; }
    void  SetLastLootTick(DWORD t) { m_lastLootTick = t; }
    OBJID GetLastLootItemId() const { return m_lastLootItemId; }

    // Expose seen-ticks map so callers (e.g., HuntBuffManager) can pass it via lambda.
    mutable std::unordered_map<OBJID, DWORD> m_lootSeenTicks;

private:
    // Session 14 [SKIP-TRACE THROTTLE]: the per-item skip trace added this
    // session (FindBestLoot) collapsed the ENTIRE 4-file log retention
    // window to ~4 SECONDS on first live use — FindBestLoot runs once per
    // rendered frame (~150Hz, see the Session 10 comment on the decision
    // loop in base_hunt_plugin.cpp), and was logging every rejected
    // candidate on every single call. A dense loot field (200+ items is
    // typical, per this file's own PRIORITY SELECTION WEIGHT comment) turns
    // into thousands of near-identical lines per second for items whose
    // fate hasn't changed at all. Gates each item+reason pair to log once,
    // then stay silent until either the reason changes or kSkipLogRefreshMs
    // has passed — keeps the useful signal (a specific item's current fate,
    // and transitions) while cutting volume by roughly two orders of
    // magnitude. Not pruned independently; piggybacks on
    // PruneLootPickupAttempts' existing activeItemIds pass.
    struct SkipLogState { std::string reason; DWORD tick = 0; };
    mutable std::unordered_map<OBJID, SkipLogState> m_lootSkipLogState;
    // const: called from FindBestLoot(), which is itself const. Mutates only
    // the mutable m_lootSkipLogState map, same pattern as m_lootSeenTicks.
    bool ShouldLogSkip(OBJID itemId, const char* reason, DWORD now) const;

    struct LootPickupAttemptState {
        uint8_t attempts       = 0;
        DWORD   ignoreUntilTick = 0;
        // Session 13 [GHOST LOOT FIX]: tick of the first pickup packet sent
        // while standing EXACTLY on the item's tile with bag space free. A
        // real item vanishes within one server round trip of that; one still
        // alive kLootGhostConfirmMs later is a stale heap-scan entry the
        // server no longer knows about ("ghost") — 60% of FindBestLoot
        // selections in the live 2026-08-29 session were the bot standing on
        // such ghosts re-picking them forever, because the timed ignore
        // expired and reset. Ghosts are ignored for as long as the stale
        // entry stays in our list (pruned with it in PruneLootPickupAttempts).
        DWORD   firstOnTileSendTick = 0;
        bool    ghostLogged = false;

        // Session 14 [PLUS VERIFY]: captured at the moment the pickup packet
        // is sent (TryPickupLootItem), while the ground CMapItem is still a
        // valid pointer — by the time PruneLootPickupAttempts notices the
        // item is gone, it's already freed and can't be re-read. groundPlus
        // stays -1 (never captured) unless a pickup was actually attempted.
        uint32_t typeId     = 0;
        int      groundPlus = -1;
    };

    std::unordered_map<OBJID, LootPickupAttemptState> m_lootPickupAttempts;

    // Session 10: tick each item id was first found with the hero standing on
    // its tile (dist == 0). Backs settings.itemPickupDelayMs — a separate
    // grace period from itemActionIntervalMs, which only throttles retries
    // after the first attempt has already fired.
    std::unordered_map<OBJID, DWORD> m_lootArrivedTicks;
    DWORD m_lastLootTick   = 0;
    OBJID m_lastLootItemId = 0;

    // ── Phase 2a: trash-drop throttle ─────────────────────────────────────────
    DWORD m_lastTrashDropTick   = 0;
    OBJID m_lastTrashDropItemId = 0;
};

// Session 14 [PLUS RE]: debug-triggered dump of the nearest live ground item
// — raw CMapItem bytes AND the MapItemInfo it points to — so a known-+0 and
// known-+1 instance of the identical type ID can be byte-diffed offline to
// find where the real plus level actually lives (GetPlus()'s current
// MapItemInfo+0x48 read is proven wrong, see ShouldLootMapItem's disable
// comment in hunt_town.cpp). Free function, not a HuntLootManager method —
// it's a one-off RE tool, not part of the hunt loop's normal operation.
void DebugDumpNearestGroundItem(CHero* hero);
