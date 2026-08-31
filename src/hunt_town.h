#pragma once
#include "base.h"
#include "hunt_settings.h"
#include <functional>
#include <cstddef>

class CHero;
class CGameMap;
class CItem;
struct CMapItem;

// ── Callback bundle passed to HuntTownService handlers ───────────────────────
// The parent plugin fills these lambdas so the service can call back for
// operations that live on BaseHuntPlugin (state transitions, pathing).
struct HuntTownCallbacks {
    // Set the plugin's current state and status text.
    std::function<void(AutoHuntState, const char*)> setStateFn;
    // Walk/path the hero to a tile near targetPos within desiredRange.
    std::function<bool(CHero*, CGameMap*, const Position&, int)> startPathNearTargetFn;
    // Begin traveling to the hunt zone (handles arrows, zone entry, etc.).
    std::function<void()> beginTravelToZoneFn;
    // Begin traveling to Market (resets sequences, decides repair/store).
    std::function<void()> beginTravelToMarketFn;
};

// ── HuntTownService ───────────────────────────────────────────────────────────
// Encapsulates the three town-run state machines: repair, buy arrows, store.
// Lives as a member of BaseHuntPlugin and is driven each Update() frame.
class HuntTownService {
public:
    // ── Phase enums ──────────────────────────────────────────────────────────
    enum class RepairPhase {
        MoveToNpc,
        Unequip,
        WaitUnequip,
        Repair,
        WaitRepair,
        Reequip,
        WaitReequip,
    };

    enum class BuyArrowsPhase {
        MoveToBlacksmith,
        BuyArrow,
        WaitBuy,
        EquipArrows,
        WaitEquip,
    };

    enum class StorePhase {
        PackMeteors,
        WaitPackMeteors,
        MoveToWarehouse,
        OpenWarehouse,
        WaitWarehouseOpen,
        DepositWarehouse,
        WaitWarehouseDeposit,
        DepositSilver,
        WaitSilverDeposit,
        MoveToTreasureBank,
        OpenTreasureBank,
        WaitTreasureBank,
        DepositTreasureMeteors,
        WaitTreasureMeteors,
        DepositTreasureDragonBalls,
        WaitTreasureDragonBalls,
        MoveToComposeBank,
        OpenComposeBank,
        WaitComposeBank,
        DepositComposeBank,
        WaitComposeBankDeposit,
    };

    // ── State machine handlers ────────────────────────────────────────────────
    // Each handler drives one town-service phase per frame.  Calls back into
    // the plugin via the HuntTownCallbacks bundle for state changes and pathing.
    void HandleRepairState(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings, const HuntTownCallbacks& cb);

    void HandleBuyArrowsState(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings, const HuntTownCallbacks& cb);

    void HandleStoreState(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings, const HuntTownCallbacks& cb);

    // ── Reset helpers ─────────────────────────────────────────────────────────
    void ResetRepairSequence();
    void ResetBuyArrowsSequence();
    void ResetStoreSequence();

    // ── Decision helpers ──────────────────────────────────────────────────────
    // NeedTownRun takes a pre-computed needsArrows flag (caller owns archer logic).
    bool NeedTownRun(CHero* hero, const AutoHuntSettings& settings, bool needsArrows) const;
    bool NeedsRepair(const CHero* hero, const AutoHuntSettings& settings) const;
    bool NeedsStorage(const CHero* hero, const AutoHuntSettings& settings) const;

    // ── Arrow helpers ─────────────────────────────────────────────────────────
    bool NeedsArrows(const CHero* hero, const AutoHuntSettings& settings) const;
    int  CountUsableArrowPacks(const CHero* hero) const;

    // ── Item predicates ───────────────────────────────────────────────────────
    static bool IsSelectedLootItem(const AutoHuntSettings& settings, uint32_t typeId);
    static bool IsSelectedWarehouseItem(const AutoHuntSettings& settings, uint32_t typeId);
    static bool IsSelectedPriorityReturnItem(const AutoHuntSettings& settings, uint32_t typeId);
    static bool IsMoneyMapItem(const CMapItem& item);

    // Money tier, ascending value: 0=Silver, 1=Sycee, 2=Gold, 3=GoldBullion,
    // 4=GoldBar, 5=GoldBars. -1 if not a money item at all.
    static int GetMoneyTier(const CMapItem& item);

    // Session 13 [RELIABILITY SPLIT]: Meteor/MeteorTear/DragonBall (incl. all
    // star tiers + Epic) ONLY — identified purely by numeric type ID, never
    // by CMapItem::GetPlus(). This replaced a broader "+items are priority
    // too" version (also matching by GetPlus()) after that unreliable byte
    // read (already proven wrong for money and for EXPEND/consumable items,
    // see ShouldLootMapItem's plus-check comment) started letting ordinary,
    // unenchanted equipment bypass the user's ENTIRE quality/list filter via
    // this function's use as a selection-gate override — live-reported as
    // "the bot picks up any item type, filling my bag with garbage" right
    // after that change shipped. The override behavior (bypass Loot Range,
    // bypass the combat-priority check, stop everything) is reserved for
    // Meteor/DragonBall specifically per the user's own original framing:
    // "this behavior is specific to meteors and dragonballs only." A +item
    // is still real, wanted loot — it's just SELECTED like everything else,
    // via ShouldLootMapItem's own (equipment-sort-scoped) plus-check, not
    // granted an override an unverified byte read can trigger by accident.
    // See IsWithinLootPickupRange in base_hunt_plugin.cpp.
    //
    // Session 13: takes settings now — Meteor pickup was unconditional
    // before (no way to turn it off), per user request each half is now
    // independently gated by settings.lootMeteor / settings.lootDragonBall.
    // When a half is off, that item type gets NO special treatment at all —
    // it falls through to ordinary ShouldLootMapItem rules (money/list/
    // quality/plus), which for a non-equipment, non-money item effectively
    // means "only if explicitly added to the Loot Item List."
    static bool IsMeteorOrDragonBallItem(const AutoHuntSettings& settings, const CMapItem& item);

    // Session 13: DragonBall specifically (base + all star tiers/Epic), NOT
    // the broader Meteor/MeteorTear/+items priority-loot bucket above. Used
    // to let a DragonBall pickup survive Paranoia's active-evasion
    // interrupt (base_hunt_plugin.cpp) while ordinary loot — including
    // Meteors — does not, per explicit user direction: "a detected player
    // should interrupt everything except for picking up a DragonBall."
    static bool IsDragonBallMapItem(const CMapItem& item);

    static bool ShouldLootMapItem(const AutoHuntSettings& settings, const CMapItem& item);

    // Session 14 [URGENT LOOT]: items worth STOPPING the hunt to go grab
    // right now — the "drop everything, ignore Loot Range, don't defer to
    // combat, steer to it" urgency that was meteor/DragonBall-only. Per user:
    // a wanted quality-tier match (a ticked refined/unique/elite/super box)
    // and a wanted +1 (the +1 box, now that GetPlus() is self-validating —
    // this exact combination regressed before ONLY because the plus read was
    // unreliable, see IsMeteorOrDragonBallItem's header) get the same urgency
    // as meteor/DragonBall. Deliberately NARROWER than ShouldLootMapItem:
    // money/gold is NOT urgent (stays ordinary Loot-Range loot), and neither
    // is a bare Loot-Item-List entry. This drives the go-get-it-now behaviors
    // only; it does NOT bypass the selection filter (quality/+1 pass
    // ShouldLootMapItem on their own) and does NOT override Paranoia evasion
    // (a detected player interrupts EVEN this — see base_hunt_plugin.cpp) or
    // the stale-age skip (a quality/+1 item sitting past the despawn cutoff
    // is likely gone/protected; only meteor/DragonBall are age-exempt).
    static bool IsUrgentLootItem(const AutoHuntSettings& settings, const CMapItem& item);

    static bool CanAffordArrowPurchase(const CHero* hero);

    bool IsTreasureBankDragonBallFamily(const CItem& item) const;
    bool IsTreasureBankMeteorFamily(const CItem& item) const;
    bool IsTreasureBankItem(const CItem& item) const;
    bool IsComposeBankItem(const CItem& item) const;

    bool HasTreasureBankItems(const CHero* hero) const;
    bool HasTreasureBankDragonBallItems(const CHero* hero) const;
    bool HasTreasureBankMeteorItems(const CHero* hero) const;
    bool HasComposeBankItems(const CHero* hero) const;
    bool HasPriorityReturnItems(const CHero* hero, const AutoHuntSettings& settings) const;
    bool HasWarehouseItems(const CHero* hero, const AutoHuntSettings& settings) const;
    bool ShouldStoreWarehouseItem(const AutoHuntSettings& settings, const CItem& item) const;

    // ── Map query helpers ─────────────────────────────────────────────────────
    // Returns true if a known blacksmith NPC entry exists for the given map.
    static bool HasBlacksmithOnMap(OBJID mapId);

    // ── Accessors for state the parent plugin may need ───────────────────────
    RepairPhase    GetRepairPhase()    const { return m_repairPhase; }
    BuyArrowsPhase GetBuyArrowsPhase() const { return m_buyArrowsPhase; }
    StorePhase     GetStorePhase()     const { return m_storePhase; }

    // Session 10: the repair sequence stalled in the field (gear unequipped
    // and never put back), and two plausible-looking root causes — a broken
    // equipment array, then a broken inventory — both turned out to be wrong
    // when actually measured. These expose the sequence's own state so the
    // stall point is observed rather than guessed at a third time.
    OBJID       GetRepairItemId() const { return m_repairItemId; }
    int         GetRepairSlot()   const { return m_repairSlot; }
    OBJID       GetRepairNpcId()  const { return m_repairNpcId; }
    // True while a piece of gear is mid-flight (unequipped into the bag and not
    // yet repaired + re-equipped). Used so an HP-potion Recover tick can't
    // abandon a repair with gear sitting off the character. m_repairItemId is
    // set from Unequip through WaitReequip and cleared (0) only between items.
    bool        IsRepairInProgress() const { return m_repairItemId != 0; }
    static const char* RepairPhaseName(RepairPhase p) {
        switch (p) {
            case RepairPhase::MoveToNpc:   return "MoveToNpc";
            case RepairPhase::Unequip:     return "Unequip";
            case RepairPhase::WaitUnequip: return "WaitUnequip";
            case RepairPhase::Repair:      return "Repair";
            case RepairPhase::WaitRepair:  return "WaitRepair";
            case RepairPhase::Reequip:     return "Reequip";
            case RepairPhase::WaitReequip: return "WaitReequip";
        }
        return "?";
    }

    // Shared NPC-action throttle tick: parent plugin reads/writes this so the
    // timer is consistent with other NPC actions the plugin performs.
    DWORD m_lastNpcActionTick = 0;

private:
    RepairPhase    m_repairPhase    = RepairPhase::MoveToNpc;
    BuyArrowsPhase m_buyArrowsPhase = BuyArrowsPhase::MoveToBlacksmith;
    StorePhase     m_storePhase     = StorePhase::PackMeteors;

    OBJID m_repairNpcId        = 0;
    OBJID m_blacksmithNpcId    = 0;
    OBJID m_warehouseNpcId     = 0;
    OBJID m_treasureBankNpcId  = 0;
    OBJID m_composeBankNpcId   = 0;
    OBJID m_repairItemId       = 0;
    OBJID m_storeItemId        = 0;
    int   m_repairSlot         = 0;
    int   m_arrowsBoughtCount  = 0;
    // Session 12 [LOCKUP FIX]: same gap as the repair/store sequences --
    // WaitBuy retried BuyArrow on timeout with no bound. If BuyItem ever
    // silently no-ops (pack count and silver both unchanged), neither of
    // BuyArrow's own exit conditions would ever trip, looping forever.
    int   m_buyArrowFailCount = 0;
    int   m_storeMeteorCountBefore = 0;

    // Session 12 [LOCKUP FIX]: mirrors m_treasureBankOpenAttempts below --
    // WaitUnequip/WaitRepair/WaitReequip each retried their action on
    // timeout with no bound, so a single stuck unequip/repair/equip action
    // stalled the whole town-run (not just repair) forever. Shared across
    // all three sub-phases since they're sequential parts of one repair
    // attempt; on giving up, the whole repair sequence is abandoned for
    // this cycle rather than risking leaving gear half-unequipped by
    // skipping just one step.
    int   m_repairStepFailCount = 0;
    // Same idea for the two Store sub-steps that had the identical gap.
    int   m_packMeteorsFailCount = 0;
    int   m_warehouseDepositFailCount = 0;
    // Item that failed to deposit repeatedly this store cycle -- skipped by
    // DepositWarehouse's scan so a single stuck item doesn't block every
    // other item behind it, instead of aborting the whole store sequence.
    OBJID m_storeDepositSkipItemId = 0;

    // Session 11 [FREEZE FIX]: OpenTreasureBank/OpenComposeBank have no
    // "window confirmed open" packet the way OpenWarehouse does — live
    // logging caught the client freezing hard (requiring a Task Manager
    // kill) right after DepositTreasureBankMeteors sent a deposit-confirm
    // packet to a bank NPC that IsNpcActive() never actually confirmed was
    // open. These bound how many times WaitTreasureBank/WaitComposeBank will
    // retry the open before giving up on that bank entirely for this store
    // cycle, instead of ever blindly proceeding to send a deposit action
    // against an unconfirmed dialog.
    int   m_treasureBankOpenAttempts = 0;
    int   m_composeBankOpenAttempts  = 0;
};
