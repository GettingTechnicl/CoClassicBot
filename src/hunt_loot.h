#pragma once
#include "base.h"
#include "hunt_settings.h"
#include <functional>
#include <memory>
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
    void PruneLootPickupAttempts(CGameMap* map);
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
