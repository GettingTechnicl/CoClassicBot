#include "game.h"
#include "jitter.h"
#include "hunt_intervals.h"
#include "hunt_targeting.h"
#include "hunt_town.h"
#include "inventory_utils.h"
#include "npc_utils.h"
#include "CHero.h"
#include "CGameMap.h"
#include "CItem.h"
#include "itemtype.h"
#include "gateway.h"
#include "log.h"
#include <algorithm>

// ── File-local constants and helpers ─────────────────────────────────────────
namespace {

const Position kTreasureBankPos = {180, 183};
const Position kComposeBankPos  = {179, 187};
const Position kWarehousePos    = {182, 180};
const Position kPharmacistPos   = {198, 181};

// Session 11 [FREEZE FIX]: bank NPCs give up (skip the deposit rather than
// retry forever) after this many failed open confirmations.
constexpr int kMaxBankOpenAttempts = 3;
// Session 12 [LOCKUP FIX]: same idea, applied to the repair sequence and the
// two Store sub-steps (meteor packing, warehouse deposit) that had the same
// unbounded-retry gap -- see the fail-count members' comments in hunt_town.h.
constexpr int kMaxRepairStepFailures = 3;
constexpr int kMaxStoreStepFailures = 3;

struct BlacksmithEntry {
    OBJID       mapId;
    const char* npcName;
    Position    pos;
};

const BlacksmithEntry kBlacksmiths[] = {
    { MAP_TWIN_CITY,      "Blacksmith", {452, 330} },
    { MAP_DESERT_CITY,    "Blacksmith", {486, 621} },
    { MAP_APE_MOUNTAIN,   "Blacksmith", {560, 508} },
    { MAP_BIRD_ISLAND,    "Blacksmith", {751, 544} },
    { MAP_PHOENIX_CASTLE, "Blacksmith", {197, 226} },
};

constexpr uint32_t kArrowCost = 34000;

const BlacksmithEntry* FindBlacksmithForMap(OBJID mapId)
{
    for (const auto& entry : kBlacksmiths) {
        if (entry.mapId == mapId && entry.pos.x != 0 && entry.pos.y != 0)
            return &entry;
    }
    return nullptr;
}

} // namespace

// ── HuntTownService — Map query helpers ──────────────────────────────────────

bool HuntTownService::HasBlacksmithOnMap(OBJID mapId)
{
    return FindBlacksmithForMap(mapId) != nullptr;
}

// ── HuntTownService — Reset helpers ──────────────────────────────────────────

void HuntTownService::ResetRepairSequence()
{
    m_repairPhase  = RepairPhase::MoveToNpc;
    m_repairNpcId  = 0;
    m_repairItemId = 0;
    m_repairSlot   = 0;
    m_repairStepFailCount = 0;
}

void HuntTownService::ResetBuyArrowsSequence()
{
    m_buyArrowsPhase   = BuyArrowsPhase::MoveToBlacksmith;
    m_blacksmithNpcId  = 0;
    m_arrowsBoughtCount = 0;
    m_buyArrowFailCount = 0;
}

void HuntTownService::ResetStoreSequence()
{
    m_storePhase           = StorePhase::PackMeteors;
    m_treasureBankNpcId    = 0;
    m_composeBankNpcId     = 0;
    m_warehouseNpcId       = 0;
    m_storeItemId          = 0;
    m_storeMeteorCountBefore = 0;
    m_treasureBankOpenAttempts = 0;
    m_composeBankOpenAttempts  = 0;
    m_packMeteorsFailCount = 0;
    m_warehouseDepositFailCount = 0;
    m_storeDepositSkipItemId = 0;
}

// ── HuntTownService — Arrow helpers ──────────────────────────────────────────

int HuntTownService::CountUsableArrowPacks(const CHero* hero) const
{
    if (!hero) return 0;
    int count = 0;
    CItem* equipped = hero->GetEquip(EquipSlot::LWEAPON);
    if (equipped && equipped->IsArrow() && equipped->GetDurability() > 3)
        ++count;
    for (const auto& itemRef : hero->m_deqItem) {
        if (itemRef && itemRef->IsArrow() && itemRef->GetDurability() > 3)
            ++count;
    }
    return count;
}

bool HuntTownService::NeedsArrows(const CHero* hero, const AutoHuntSettings& settings) const
{
    if (!settings.buyArrows || !IsArcherModeEnabled(settings) || settings.arrowTypeId == 0)
        return false;
    if (!CanAffordArrowPurchase(hero))
        return false;
    return CountUsableArrowPacks(hero) < settings.arrowBuyCount;
}

// ── HuntTownService — Decision helpers ───────────────────────────────────────

bool HuntTownService::NeedsRepair(const CHero* hero, const AutoHuntSettings& settings) const
{
    if (!settings.autoRepair || !hero)
        return false;

    for (int slot = 0; slot < EquipSlot::COUNT; ++slot) {
        const CItem* item = hero->GetEquip(slot);
        if (!item || item->GetMaxDurabilityRaw() <= 0)
            continue;
        if (item->IsArrow())
            continue;

        if (item->GetDurabilityRaw() < item->GetMaxDurabilityRaw()
            && GetDurabilityPercent(*item) <= settings.repairPercent) {
            return true;
        }
    }

    return false;
}

bool HuntTownService::NeedsStorage(const CHero* hero, const AutoHuntSettings& settings) const
{
    if (!settings.autoStore || !hero)
        return false;

    if (settings.storeTreasureBank && HasTreasureBankItems(hero))
        return true;
    if (settings.storeComposeBank && HasComposeBankItems(hero))
        return true;
    if (HasWarehouseItems(hero, settings))
        return true;
    if (settings.autoDepositSilver) {
        const uint32_t keepAmount = settings.silverKeepAmount > 0 ? (uint32_t)settings.silverKeepAmount : 0;
        if (hero->GetSilver() > keepAmount)
            return true;
    }
    return false;
}

bool HuntTownService::NeedTownRun(CHero* hero, const AutoHuntSettings& settings, bool needsArrows) const
{
    if (!hero)
        return false;

    const bool needsRepair = NeedsRepair(hero, settings);
    const bool priorityReturn = settings.autoStore && settings.immediateReturnOnPriorityItems
        && HasPriorityReturnItems(hero, settings);
    const bool needsStorage = NeedsStorage(hero, settings);
    const int bagThreshold = CHero::ClampBagThreshold(settings.bagStoreThreshold);
    const bool bagOverThreshold = (int)hero->m_deqItem.size() >= bagThreshold || hero->IsBagFull();

    static DWORD s_lastDiagTick = 0;
    const DWORD diagNow = GetTickCount();
    if (diagNow - s_lastDiagTick >= 5000) {
        s_lastDiagTick = diagNow;
        spdlog::trace("[town-diag] NeedTownRun: needsRepair={} needsArrows={} priorityReturn={} needsStorage={} bagOverThreshold={} bagSize={}/{}",
            needsRepair, needsArrows, priorityReturn, needsStorage, bagOverThreshold,
            (int)hero->m_deqItem.size(), bagThreshold);
    }

    if (needsRepair)
        return true;

    if (needsArrows)
        return true;

    if (priorityReturn)
        return true;

    if (needsStorage && bagOverThreshold)
        return true;

    return false;
}

// ── HuntTownService — Item predicates ────────────────────────────────────────

bool HuntTownService::IsSelectedLootItem(const AutoHuntSettings& settings, uint32_t typeId)
{
    return std::find(settings.lootItemIds.begin(), settings.lootItemIds.end(), typeId)
        != settings.lootItemIds.end();
}

bool HuntTownService::IsSelectedWarehouseItem(const AutoHuntSettings& settings, uint32_t typeId)
{
    return std::find(settings.warehouseItemIds.begin(), settings.warehouseItemIds.end(), typeId)
        != settings.warehouseItemIds.end();
}

bool HuntTownService::IsSelectedPriorityReturnItem(const AutoHuntSettings& settings, uint32_t typeId)
{
    return std::find(settings.priorityReturnItemIds.begin(), settings.priorityReturnItemIds.end(), typeId)
        != settings.priorityReturnItemIds.end();
}

bool HuntTownService::IsMoneyMapItem(const CMapItem& item)
{
    const ItemTypeInfo* info = GetItemTypeInfo(item.m_idType);
    if (info) {
        const std::string& name = info->name;
        if (name == "Silver" || name == "Gold" || name == "Money")
            return true;
    }

    return item.m_idType >= 1090000 && item.m_idType <= 1091020;
}

int HuntTownService::GetMoneyTier(const CMapItem& item)
{
    switch (item.m_idType) {
        case 1090000: return 0;  // Silver
        case 1090010: return 1;  // Sycee
        case 1090020: return 2;  // Gold
        case 1091000: return 3;  // GoldBullion
        case 1091010: return 4;  // GoldBar
        case 1091020: return 5;  // GoldBars
        default:      return -1;
    }
}

bool HuntTownService::IsPriorityLootItem(const CMapItem& item)
{
    // Money is never a detour-priority item. CMapItem::GetPlus() reads a raw
    // info-struct byte (+0x48) that only means "plus level" on equipment — on
    // money piles it holds unrelated data and reads nonzero, which made every
    // gold pile "priority", bypassing the Loot Range gate: live session showed
    // the bot chain-jumping to gold 33-59 tiles out (Loot Range 6) while its
    // kill counter sat frozen for 13 straight seconds.
    if (GetMoneyTier(item) >= 0)
        return false;
    if (item.GetPlus() > 0)
        return true;
    // Base DragonBall/Meteor/MeteorTear trio (contiguous IDs) plus the
    // separately-numbered star-tiered/Epic DragonBalls — CItem::IsTreasureItem()
    // only covers the base trio, which is correct for its own (treasure-bank
    // storage) purpose but not broad enough for "is this worth detouring for".
    if (item.m_idType >= ItemTypeId::DRAGONBALL && item.m_idType <= ItemTypeId::METEOR_TEAR)
        return true;
    if (item.m_idType >= 2000031 && item.m_idType <= 2000038)  // 1-7 Star + Epic DragonBall
        return true;
    return false;
}

bool HuntTownService::IsDragonBallMapItem(const CMapItem& item)
{
    if (item.m_idType == ItemTypeId::DRAGONBALL)
        return true;
    if (item.m_idType >= 2000031 && item.m_idType <= 2000038)  // 1-7 Star + Epic DragonBall
        return true;
    return false;
}

bool HuntTownService::ShouldLootMapItem(const AutoHuntSettings& settings, const CMapItem& item)
{
    if (settings.lootMoney && IsMoneyMapItem(item))
        return true;
    if (IsSelectedLootItem(settings, item.m_idType))
        return true;
    if (MatchesSelectedLootQuality(settings, item))
        return true;
    return settings.minimumLootPlus > 0 && item.GetPlus() >= settings.minimumLootPlus;
}

bool HuntTownService::CanAffordArrowPurchase(const CHero* hero)
{
    return hero && hero->GetSilver() >= kArrowCost;
}

bool HuntTownService::IsTreasureBankDragonBallFamily(const CItem& item) const
{
    return item.IsDragonBall() || item.IsDBScroll();
}

bool HuntTownService::IsTreasureBankMeteorFamily(const CItem& item) const
{
    return item.IsMeteor() || item.IsMeteorScroll();
}

bool HuntTownService::IsTreasureBankItem(const CItem& item) const
{
    return IsTreasureBankDragonBallFamily(item) || IsTreasureBankMeteorFamily(item);
}

bool HuntTownService::IsComposeBankItem(const CItem& item) const
{
    return item.IsWearableEquipment() && (item.GetPlus() == 1 || item.GetPlus() == 2);
}

bool HuntTownService::HasTreasureBankItems(const CHero* hero) const
{
    return InventoryHasMatchingItem(hero, [this](const CItem& item) {
        return IsTreasureBankItem(item);
    });
}

bool HuntTownService::HasTreasureBankDragonBallItems(const CHero* hero) const
{
    return InventoryHasMatchingItem(hero, [this](const CItem& item) {
        return IsTreasureBankDragonBallFamily(item);
    });
}

bool HuntTownService::HasTreasureBankMeteorItems(const CHero* hero) const
{
    return InventoryHasMatchingItem(hero, [this](const CItem& item) {
        return IsTreasureBankMeteorFamily(item);
    });
}

bool HuntTownService::HasComposeBankItems(const CHero* hero) const
{
    return InventoryHasMatchingItem(hero, [this](const CItem& item) {
        return IsComposeBankItem(item);
    });
}

bool HuntTownService::HasPriorityReturnItems(const CHero* hero, const AutoHuntSettings& settings) const
{
    return InventoryHasMatchingItem(hero, [&settings, this](const CItem& item) {
        return IsSelectedPriorityReturnItem(settings, item.GetTypeID());
    });
}

bool HuntTownService::HasWarehouseItems(const CHero* hero, const AutoHuntSettings& settings) const
{
    return InventoryHasMatchingItem(hero, [this, &settings](const CItem& item) {
        return ShouldStoreWarehouseItem(settings, item);
    });
}

bool HuntTownService::ShouldStoreWarehouseItem(const AutoHuntSettings& settings, const CItem& item) const
{
    if (const ItemTypeInfo* info = GetItemTypeInfo(item.GetTypeID())) {
        if (IsConsumablePotionType(*info, false) || IsConsumablePotionType(*info, true))
            return false;
    }

    // User-explicit selections always win over the default Treasure/Compose
    // Bank auto-routing below — e.g. a Meteor added to the Priority Return
    // list should go to the Warehouse even though storeTreasureBank would
    // otherwise silently claim it first (that default has no UI toggle, so
    // without this the user's own list picks would look completely inert
    // for any item type the Treasure/Compose Bank auto-routing also covers).
    if (IsSelectedPriorityReturnItem(settings, item.GetTypeID()))
        return true;

    if (IsSelectedWarehouseItem(settings, item.GetTypeID()))
        return true;

    if (settings.storeTreasureBank && IsTreasureBankItem(item))
        return false;

    if (settings.storeComposeBank && IsComposeBankItem(item))
        return false;

    if (IsEquipmentQualitySort(item.GetSort())) {
        const int quality = item.GetQuality();
        if ((quality == ItemQuality::REFINED && settings.storeRefined)
            || (quality == ItemQuality::UNIQUE && settings.storeUnique)
            || (quality == ItemQuality::ELITE && settings.storeElite)
            || (quality == ItemQuality::SUPER && settings.storeSuper))
            return true;
    }

    return settings.minimumStorePlus > 0 && item.GetPlus() >= settings.minimumStorePlus;
}

// ── HuntTownService — HandleRepairState ──────────────────────────────────────

void HuntTownService::HandleRepairState(CHero* hero, CGameMap* map,
    const AutoHuntSettings& settings, const HuntTownCallbacks& cb)
{
    if (!hero || !map) {
        cb.setStateFn(AutoHuntState::WaitingForGame, "Waiting for hero and map");
        return;
    }

    if (Game::GetCurrentMapId() != MAP_MARKET) {
        cb.beginTravelToMarketFn();
        return;
    }

    CRole* pharmacist = FindNpcByName("Pharmacist", kPharmacistPos, 16);
    const Position pharmacistPos = pharmacist ? pharmacist->m_posMap : kPharmacistPos;
    const int npcDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, pharmacistPos.x, pharmacistPos.y);

    if (npcDist > 5) {
        cb.startPathNearTargetFn(hero, map, pharmacistPos, 4);
        cb.setStateFn(AutoHuntState::Repair, "Moving to Pharmacist");
        return;
    }

    if (pharmacist)
        m_repairNpcId = pharmacist->GetID();

    const DWORD now = GetTickCount();
    const DWORD npcActionInterval = GetNpcActionIntervalMs(settings);

    switch (m_repairPhase) {
        case RepairPhase::MoveToNpc:
            m_repairPhase = RepairPhase::Unequip;
            return;

        case RepairPhase::Unequip: {
            for (int slot = 0; slot < EquipSlot::COUNT; ++slot) {
                CItem* item = hero->GetEquip(slot);
                if (!item || item->GetMaxDurabilityRaw() <= 0)
                    continue;
                if (item->IsArrow())
                    continue;
                if (item->GetDurabilityRaw() >= item->GetMaxDurabilityRaw())
                    continue;

                if (now - m_lastNpcActionTick < npcActionInterval)
                    return;

                m_repairSlot = slot;
                m_repairItemId = item->GetID();
                hero->UnequipItem(m_repairItemId, m_repairSlot);
                m_repairPhase = RepairPhase::WaitUnequip;
                m_lastNpcActionTick = now;
                cb.setStateFn(AutoHuntState::Repair, "Unequipping item for repair");
                return;
            }

            ResetRepairSequence();
            if (settings.autoStore && NeedsStorage(hero, settings)) {
                ResetStoreSequence();
                cb.setStateFn(AutoHuntState::StoreItems, "Processing storage rules");
            } else {
                cb.beginTravelToZoneFn();
            }
            return;
        }

        case RepairPhase::WaitUnequip:
            if (!hero->GetEquip(m_repairSlot) && FindInventoryItemById(hero, m_repairItemId)) {
                m_repairStepFailCount = 0;
                m_repairPhase = RepairPhase::Repair;
                return;
            }
            if (now - m_lastNpcActionTick > 2500) {
                if (++m_repairStepFailCount >= kMaxRepairStepFailures) {
                    spdlog::warn("[hunt] Repair: unequip of item {} (slot {}) never confirmed after {} attempts, abandoning this repair cycle",
                        m_repairItemId, m_repairSlot, m_repairStepFailCount);
                    ResetRepairSequence();
                    cb.beginTravelToZoneFn();
                    return;
                }
                m_repairPhase = RepairPhase::Unequip;
            }
            return;

        case RepairPhase::Repair:
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->RepairItem(m_repairItemId);
            m_repairPhase = RepairPhase::WaitRepair;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::Repair, "Repairing item");
            return;

        case RepairPhase::WaitRepair: {
            CItem* bagItem = FindInventoryItemById(hero, m_repairItemId);
            if (bagItem && bagItem->GetDurabilityRaw() >= bagItem->GetMaxDurabilityRaw()) {
                m_repairStepFailCount = 0;
                m_repairPhase = RepairPhase::Reequip;
                return;
            }
            if (now - m_lastNpcActionTick > 2500) {
                // Session 12 [LOCKUP FIX]: give up on this repair attempt
                // entirely rather than retry forever -- see
                // kMaxRepairStepFailures. NOT abandoning just this item and
                // moving to the next: the item is currently unequipped and
                // sitting in the bag, so bailing all the way out (rather
                // than risking silently leaving it there) means the next
                // NeedsRepair() check re-drives the whole sequence, which
                // still sees this item unequipped-but-not-repaired and
                // starts over on it.
                if (++m_repairStepFailCount >= kMaxRepairStepFailures) {
                    spdlog::warn("[hunt] Repair: repairing item {} never confirmed after {} attempts, abandoning this repair cycle",
                        m_repairItemId, m_repairStepFailCount);
                    ResetRepairSequence();
                    cb.beginTravelToZoneFn();
                    return;
                }
                m_repairPhase = RepairPhase::Repair;
            }
            return;
        }

        case RepairPhase::Reequip:
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->EquipItem(m_repairItemId, m_repairSlot);
            m_repairPhase = RepairPhase::WaitReequip;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::Repair, "Re-equipping repaired item");
            return;

        case RepairPhase::WaitReequip: {
            CItem* equipped = hero->GetEquip(m_repairSlot);
            if (equipped && equipped->GetID() == m_repairItemId) {
                m_repairItemId = 0;
                m_repairStepFailCount = 0;
                m_repairPhase = RepairPhase::Unequip;
                return;
            }
            if (now - m_lastNpcActionTick > 2500) {
                if (++m_repairStepFailCount >= kMaxRepairStepFailures) {
                    spdlog::warn("[hunt] Repair: re-equip of item {} (slot {}) never confirmed after {} attempts, abandoning this repair cycle",
                        m_repairItemId, m_repairSlot, m_repairStepFailCount);
                    ResetRepairSequence();
                    cb.beginTravelToZoneFn();
                    return;
                }
                m_repairPhase = RepairPhase::Reequip;
            }
            return;
        }
    }
}

// ── HuntTownService — HandleBuyArrowsState ───────────────────────────────────

void HuntTownService::HandleBuyArrowsState(CHero* hero, CGameMap* map,
    const AutoHuntSettings& settings, const HuntTownCallbacks& cb)
{
    if (!hero || !map) {
        cb.setStateFn(AutoHuntState::WaitingForGame, "Waiting for hero and map");
        return;
    }

    // Helper lambda: finish BuyArrows — go straight to zone.
    auto finishBuyArrows = [&]() {
        ResetBuyArrowsSequence();
        cb.beginTravelToZoneFn();
    };

    // Wait for any in-progress travel before acting on the blacksmith.
    // (Caller ensures travel->IsTraveling() is checked at the top of Update,
    //  but this guard handles the race between state dispatch and travel start.)
    // NOTE: We do NOT have travel here — the caller already gates on travel.IsTraveling()
    // before dispatching to us, so we just check the map.

    const BlacksmithEntry* bs = FindBlacksmithForMap(Game::GetCurrentMapId());
    if (!bs) {
        // No blacksmith on this map — go to the zone city (which has one).
        // HandleTravelToZone will re-enter BuyArrows on arrival.
        spdlog::info("[hunt] No blacksmith on current map, traveling to zone city for arrows");
        finishBuyArrows();
        return;
    }

    CRole* blacksmith = FindNpcByName(bs->npcName, bs->pos, 16);
    const Position bsPos = blacksmith ? blacksmith->m_posMap : bs->pos;
    const int npcDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, bsPos.x, bsPos.y);

    if (npcDist > 5) {
        cb.startPathNearTargetFn(hero, map, bsPos, 4);
        cb.setStateFn(AutoHuntState::BuyArrows, "Moving to Blacksmith");
        return;
    }

    if (blacksmith)
        m_blacksmithNpcId = blacksmith->GetID();

    const DWORD now = GetTickCount();
    const DWORD npcActionInterval = GetNpcActionIntervalMs(settings);

    switch (m_buyArrowsPhase) {
        case BuyArrowsPhase::MoveToBlacksmith:
            m_arrowsBoughtCount = 0;
            m_buyArrowsPhase = BuyArrowsPhase::BuyArrow;
            return;

        case BuyArrowsPhase::BuyArrow: {
            if (m_blacksmithNpcId == 0) {
                spdlog::warn("[hunt] Blacksmith NPC not found near ({},{})", bsPos.x, bsPos.y);
                m_buyArrowsPhase = BuyArrowsPhase::EquipArrows;
                return;
            }
            const int currentPacks = CountUsableArrowPacks(hero);
            if (currentPacks >= settings.arrowBuyCount || hero->IsBagFull()) {
                spdlog::info("[hunt] Arrow purchase complete: have {} packs (target {}), bought {}",
                    currentPacks, settings.arrowBuyCount, m_arrowsBoughtCount);
                m_buyArrowsPhase = BuyArrowsPhase::EquipArrows;
                return;
            }
            if (hero->GetSilver() < kArrowCost) {
                spdlog::info("[hunt] Not enough silver for arrows (have {}, need {}), bought {} so far",
                    hero->GetSilver(), kArrowCost, m_arrowsBoughtCount);
                m_buyArrowsPhase = BuyArrowsPhase::EquipArrows;
                return;
            }
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;

            hero->BuyItem(m_blacksmithNpcId, settings.arrowTypeId);
            m_lastNpcActionTick = now;
            ++m_arrowsBoughtCount;
            m_buyArrowsPhase = BuyArrowsPhase::WaitBuy;
            spdlog::debug("[hunt] Buying arrow type {} from NPC {} (purchase #{}, silver={})",
                settings.arrowTypeId, m_blacksmithNpcId, m_arrowsBoughtCount, hero->GetSilver());
            cb.setStateFn(AutoHuntState::BuyArrows, "Buying arrows");
            return;
        }

        case BuyArrowsPhase::WaitBuy: {
            const int currentPacks = CountUsableArrowPacks(hero);
            if (currentPacks > m_arrowsBoughtCount - 1) {
                m_buyArrowFailCount = 0;
                m_buyArrowsPhase = BuyArrowsPhase::BuyArrow;
                return;
            }
            if (now - m_lastNpcActionTick > 2500) {
                // Session 12 [LOCKUP FIX]: give up buying arrows after
                // repeated failures instead of retrying forever -- see
                // m_buyArrowFailCount's comment in hunt_town.h.
                if (++m_buyArrowFailCount >= kMaxStoreStepFailures) {
                    spdlog::warn("[hunt] Arrow buy never confirmed after {} attempts, giving up on this purchase cycle",
                        m_buyArrowFailCount);
                    m_buyArrowFailCount = 0;
                    m_buyArrowsPhase = BuyArrowsPhase::EquipArrows;
                    return;
                }
                spdlog::warn("[hunt] Arrow buy timeout, retrying");
                m_buyArrowsPhase = BuyArrowsPhase::BuyArrow;
            }
            return;
        }

        case BuyArrowsPhase::EquipArrows: {
            CItem* equipped = hero->GetEquip(EquipSlot::LWEAPON);
            if (equipped && equipped->IsArrow() && equipped->GetDurability() > 3) {
                m_buyArrowsPhase = BuyArrowsPhase::WaitEquip;
                return;
            }
            for (const auto& itemRef : hero->m_deqItem) {
                if (!itemRef || !itemRef->IsArrow() || itemRef->GetDurability() <= 3)
                    continue;
                if (now - m_lastNpcActionTick < npcActionInterval)
                    return;
                hero->EquipItem(itemRef->GetID(), EquipSlot::LWEAPON);
                m_lastNpcActionTick = now;
                cb.setStateFn(AutoHuntState::BuyArrows, "Equipping arrows");
                m_buyArrowsPhase = BuyArrowsPhase::WaitEquip;
                return;
            }
            m_buyArrowsPhase = BuyArrowsPhase::WaitEquip;
            return;
        }

        case BuyArrowsPhase::WaitEquip: {
            CItem* equipped = hero->GetEquip(EquipSlot::LWEAPON);
            if ((equipped && equipped->IsArrow() && equipped->GetDurability() > 3)
                || now - m_lastNpcActionTick > 2500) {
                finishBuyArrows();
            }
            return;
        }
    }
}

// ── HuntTownService — HandleStoreState ───────────────────────────────────────

void HuntTownService::HandleStoreState(CHero* hero, CGameMap* map,
    const AutoHuntSettings& settings, const HuntTownCallbacks& cb)
{
    if (!hero || !map) {
        cb.setStateFn(AutoHuntState::WaitingForGame, "Waiting for hero and map");
        return;
    }

    if (Game::GetCurrentMapId() != MAP_MARKET) {
        cb.beginTravelToMarketFn();
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD npcActionInterval = GetNpcActionIntervalMs(settings);

    switch (m_storePhase) {
        case StorePhase::PackMeteors: {
            if (!settings.packMeteorsIntoScrolls) {
                m_storePhase = StorePhase::MoveToWarehouse;
                return;
            }

            const int meteorCount = CountInventoryItemsByType(hero, ItemTypeId::METEOR);
            if (meteorCount < 10) {
                m_storePhase = StorePhase::MoveToWarehouse;
                return;
            }

            CItem* meteor = FindInventoryItemByType(hero, ItemTypeId::METEOR);
            if (!meteor) {
                m_storePhase = StorePhase::MoveToWarehouse;
                return;
            }

            if (now - m_lastNpcActionTick < npcActionInterval)
                return;

            m_storeMeteorCountBefore = meteorCount;
            m_storeItemId = meteor->GetID();
            hero->UseItem(m_storeItemId);
            m_storePhase = StorePhase::WaitPackMeteors;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::StoreItems, "Packing Meteors into MeteorScrolls");
            return;
        }

        case StorePhase::WaitPackMeteors: {
            const int meteorCount = CountInventoryItemsByType(hero, ItemTypeId::METEOR);
            if (meteorCount < m_storeMeteorCountBefore || meteorCount < 10) {
                m_storeItemId = 0;
                m_storeMeteorCountBefore = 0;
                m_packMeteorsFailCount = 0;
                m_storePhase = StorePhase::PackMeteors;
                return;
            }
            if (now - m_lastNpcActionTick > 2500) {
                m_storeItemId = 0;
                m_storeMeteorCountBefore = 0;
                // Session 12 [LOCKUP FIX]: give up packing meteors after
                // repeated failures instead of retrying forever -- see
                // kMaxStoreStepFailures.
                if (++m_packMeteorsFailCount >= kMaxStoreStepFailures) {
                    spdlog::warn("[hunt] Packing meteors into scrolls never confirmed after {} attempts, giving up for this store cycle",
                        m_packMeteorsFailCount);
                    m_packMeteorsFailCount = 0;
                    m_storePhase = StorePhase::MoveToWarehouse;
                    return;
                }
                m_storePhase = StorePhase::PackMeteors;
            }
            return;
        }

        case StorePhase::MoveToWarehouse: {
            const bool needsWarehouseItems = HasWarehouseItems(hero, settings);
            const bool needsSilverDeposit = settings.autoDepositSilver
                && hero->GetSilver() > (settings.silverKeepAmount > 0 ? (uint32_t)settings.silverKeepAmount : 0u);
            if (!needsWarehouseItems && !needsSilverDeposit) {
                m_storePhase = StorePhase::MoveToTreasureBank;
                return;
            }

            CRole* warehouseman = FindNpcByName("Warehouseman", kWarehousePos, 16);
            if (warehouseman)
                m_warehouseNpcId = warehouseman->GetID();

            const Position warehousePos = warehouseman ? warehouseman->m_posMap : kWarehousePos;
            const int npcDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, warehousePos.x, warehousePos.y);
            if (npcDist > 5) {
                cb.startPathNearTargetFn(hero, map, warehousePos, 4);
                cb.setStateFn(AutoHuntState::StoreItems, "Moving to Warehouseman");
                return;
            }

            if (m_warehouseNpcId == 0) {
                cb.setStateFn(AutoHuntState::StoreItems, "Waiting for Warehouseman");
                return;
            }

            m_storePhase = StorePhase::OpenWarehouse;
            return;
        }

        case StorePhase::OpenWarehouse:
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->OpenWarehouse(m_warehouseNpcId);
            m_storePhase = StorePhase::WaitWarehouseOpen;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::StoreItems, "Opening warehouse");
            return;

        case StorePhase::WaitWarehouseOpen:
            if ((hero->IsNpcActive() && hero->GetActiveNpc() == m_warehouseNpcId)
                || now - m_lastNpcActionTick > 1200) {
                m_storePhase = StorePhase::DepositWarehouse;
            }
            return;

        case StorePhase::DepositWarehouse: {
            for (const auto& itemRef : hero->m_deqItem) {
                if (!itemRef || !ShouldStoreWarehouseItem(settings, *itemRef))
                    continue;
                // Session 12 [LOCKUP FIX]: skip an item that already failed
                // to deposit kMaxStoreStepFailures times this cycle, so it
                // doesn't block every other item behind it in the scan.
                if (itemRef->GetID() == m_storeDepositSkipItemId)
                    continue;
                if (m_warehouseNpcId == 0)
                    return;
                if (now - m_lastNpcActionTick < npcActionInterval)
                    return;

                m_storeItemId = itemRef->GetID();
                hero->DepositWarehouseItem(m_warehouseNpcId, m_storeItemId);
                m_storePhase = StorePhase::WaitWarehouseDeposit;
                m_lastNpcActionTick = now;
                cb.setStateFn(AutoHuntState::StoreItems, "Depositing warehouse item");
                return;
            }

            m_storeDepositSkipItemId = 0;  // no more candidates this cycle -- clear for the next one
            m_storePhase = StorePhase::DepositSilver;
            return;
        }

        case StorePhase::WaitWarehouseDeposit:
            if (!FindInventoryItemById(hero, m_storeItemId)) {
                m_storeItemId = 0;
                m_warehouseDepositFailCount = 0;
                m_storePhase = StorePhase::DepositWarehouse;
                return;
            }
            if (now - m_lastNpcActionTick > 2500) {
                // Session 12 [LOCKUP FIX]: give up on depositing THIS item
                // after repeated failures instead of retrying it forever --
                // see kMaxStoreStepFailures. Skips just this item id for the
                // rest of the store cycle; DepositWarehouse's loop above
                // moves on to the next qualifying item (or DepositSilver if
                // none remain) instead of re-selecting the same stuck one.
                if (++m_warehouseDepositFailCount >= kMaxStoreStepFailures) {
                    spdlog::warn("[hunt] Warehouse deposit of item {} never confirmed after {} attempts, skipping it for the rest of this store cycle",
                        m_storeItemId, m_warehouseDepositFailCount);
                    m_storeDepositSkipItemId = m_storeItemId;
                    m_warehouseDepositFailCount = 0;
                    m_storeItemId = 0;
                }
                m_storePhase = StorePhase::DepositWarehouse;
            }
            return;

        case StorePhase::DepositSilver: {
            if (!settings.autoDepositSilver || m_warehouseNpcId == 0) {
                m_storePhase = StorePhase::MoveToTreasureBank;
                return;
            }
            const uint32_t currentSilver = hero->GetSilver();
            const uint32_t keepAmount = settings.silverKeepAmount > 0 ? (uint32_t)settings.silverKeepAmount : 0;
            if (currentSilver <= keepAmount) {
                m_storePhase = StorePhase::MoveToTreasureBank;
                return;
            }
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            const uint32_t depositAmount = currentSilver - keepAmount;
            hero->DepositWarehouseSilver(m_warehouseNpcId, depositAmount);
            m_lastNpcActionTick = now;
            m_storePhase = StorePhase::WaitSilverDeposit;
            cb.setStateFn(AutoHuntState::StoreItems, "Depositing silver");
            return;
        }

        case StorePhase::WaitSilverDeposit:
            if (now - m_lastNpcActionTick > 2500
                || hero->GetSilver() <= (settings.silverKeepAmount > 0 ? (uint32_t)settings.silverKeepAmount : 0u)) {
                m_storePhase = StorePhase::MoveToTreasureBank;
            }
            return;

        case StorePhase::MoveToTreasureBank: {
            if (!settings.storeTreasureBank || !HasTreasureBankItems(hero)) {
                m_storePhase = StorePhase::MoveToComposeBank;
                return;
            }

            CRole* treasureBank = FindNpcByName("TreasureBank", kTreasureBankPos, 16);
            if (treasureBank)
                m_treasureBankNpcId = treasureBank->GetID();

            const Position treasurePos = treasureBank ? treasureBank->m_posMap : kTreasureBankPos;
            const int npcDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, treasurePos.x, treasurePos.y);
            if (npcDist > 5) {
                cb.startPathNearTargetFn(hero, map, treasurePos, 4);
                cb.setStateFn(AutoHuntState::StoreItems, "Moving to TreasureBank");
                return;
            }

            if (m_treasureBankNpcId == 0) {
                cb.setStateFn(AutoHuntState::StoreItems, "Waiting for TreasureBank");
                return;
            }

            m_storePhase = StorePhase::OpenTreasureBank;
            return;
        }

        case StorePhase::OpenTreasureBank:
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->OpenTreasureBank(m_treasureBankNpcId);
            m_storePhase = StorePhase::WaitTreasureBank;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::StoreItems, "Opening TreasureBank");
            return;

        case StorePhase::WaitTreasureBank:
            // Session 11 [FREEZE FIX]: this used to advance to the deposit
            // phase purely off a 1200ms timeout even when IsNpcActive() never
            // confirmed the bank window actually opened. Live logging caught
            // the client hard-freezing (required a Task Manager kill) right
            // after DepositTreasureBankMeteors sent its deposit-confirm
            // packet in that exact unconfirmed-window state. Never proceed
            // to deposit without confirmation — retry the open a bounded
            // number of times, then give up on this bank for the cycle.
            if (hero->IsNpcActive() && hero->GetActiveNpc() == m_treasureBankNpcId) {
                m_treasureBankOpenAttempts = 0;
                m_storePhase = HasTreasureBankMeteorItems(hero)
                    ? StorePhase::DepositTreasureMeteors
                    : StorePhase::DepositTreasureDragonBalls;
                return;
            }
            if (now - m_lastNpcActionTick > 1200) {
                ++m_treasureBankOpenAttempts;
                if (m_treasureBankOpenAttempts >= kMaxBankOpenAttempts) {
                    spdlog::warn("[hunt] TreasureBank NPC {} never confirmed open after {} attempts, giving up on this deposit cycle",
                        m_treasureBankNpcId, m_treasureBankOpenAttempts);
                    m_treasureBankOpenAttempts = 0;
                    m_storePhase = StorePhase::MoveToComposeBank;
                    return;
                }
                spdlog::warn("[hunt] TreasureBank NPC {} not confirmed open after 1200ms, retrying (attempt {}/{})",
                    m_treasureBankNpcId, m_treasureBankOpenAttempts, kMaxBankOpenAttempts);
                m_storePhase = StorePhase::OpenTreasureBank;
            }
            return;

        case StorePhase::DepositTreasureMeteors:
            if (!HasTreasureBankMeteorItems(hero)) {
                m_storePhase = StorePhase::DepositTreasureDragonBalls;
                return;
            }
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->DepositTreasureBankMeteors(m_treasureBankNpcId);
            m_storePhase = StorePhase::WaitTreasureMeteors;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::StoreItems, "Depositing Meteors");
            return;

        case StorePhase::WaitTreasureMeteors:
            if (!HasTreasureBankMeteorItems(hero)) {
                m_storePhase = StorePhase::DepositTreasureDragonBalls;
                return;
            }
            if (now - m_lastNpcActionTick > 2500)
                m_storePhase = StorePhase::DepositTreasureDragonBalls;
            return;

        case StorePhase::DepositTreasureDragonBalls:
            if (!HasTreasureBankDragonBallItems(hero)) {
                m_storePhase = StorePhase::MoveToComposeBank;
                return;
            }
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->DepositTreasureBankDragonBalls(m_treasureBankNpcId);
            m_storePhase = StorePhase::WaitTreasureDragonBalls;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::StoreItems, "Depositing DragonBalls");
            return;

        case StorePhase::WaitTreasureDragonBalls:
            if (!HasTreasureBankDragonBallItems(hero)) {
                m_storePhase = StorePhase::MoveToComposeBank;
                return;
            }
            if (now - m_lastNpcActionTick > 2500)
                m_storePhase = StorePhase::MoveToComposeBank;
            return;

        case StorePhase::MoveToComposeBank: {
            if (!settings.storeComposeBank || !HasComposeBankItems(hero)) {
                ResetStoreSequence();
                cb.beginTravelToZoneFn();
                return;
            }

            CRole* composeBank = FindNpcByName("ComposeBank", kComposeBankPos, 16);
            if (composeBank)
                m_composeBankNpcId = composeBank->GetID();

            const Position composePos = composeBank ? composeBank->m_posMap : kComposeBankPos;
            const int npcDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, composePos.x, composePos.y);
            if (npcDist > 5) {
                cb.startPathNearTargetFn(hero, map, composePos, 4);
                cb.setStateFn(AutoHuntState::StoreItems, "Moving to ComposeBank");
                return;
            }

            if (m_composeBankNpcId == 0) {
                cb.setStateFn(AutoHuntState::StoreItems, "Waiting for ComposeBank");
                return;
            }

            m_storePhase = StorePhase::OpenComposeBank;
            return;
        }

        case StorePhase::OpenComposeBank:
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->OpenComposeBank(m_composeBankNpcId);
            m_storePhase = StorePhase::WaitComposeBank;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::StoreItems, "Opening ComposeBank");
            return;

        case StorePhase::WaitComposeBank:
            // Session 11 [FREEZE FIX]: same unconfirmed-window issue as
            // WaitTreasureBank above — never proceed to deposit without
            // IsNpcActive() confirmation.
            if (hero->IsNpcActive() && hero->GetActiveNpc() == m_composeBankNpcId) {
                m_composeBankOpenAttempts = 0;
                m_storePhase = StorePhase::DepositComposeBank;
                return;
            }
            if (now - m_lastNpcActionTick > 1200) {
                ++m_composeBankOpenAttempts;
                if (m_composeBankOpenAttempts >= kMaxBankOpenAttempts) {
                    spdlog::warn("[hunt] ComposeBank NPC {} never confirmed open after {} attempts, giving up on this deposit cycle",
                        m_composeBankNpcId, m_composeBankOpenAttempts);
                    m_composeBankOpenAttempts = 0;
                    ResetStoreSequence();
                    cb.beginTravelToZoneFn();
                    return;
                }
                spdlog::warn("[hunt] ComposeBank NPC {} not confirmed open after 1200ms, retrying (attempt {}/{})",
                    m_composeBankNpcId, m_composeBankOpenAttempts, kMaxBankOpenAttempts);
                m_storePhase = StorePhase::OpenComposeBank;
            }
            return;

        case StorePhase::DepositComposeBank:
            if (!HasComposeBankItems(hero)) {
                ResetStoreSequence();
                cb.beginTravelToZoneFn();
                return;
            }
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->DepositComposeBankAll();
            m_storePhase = StorePhase::WaitComposeBankDeposit;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::StoreItems, "Depositing +1/+2 gear");
            return;

        case StorePhase::WaitComposeBankDeposit:
            if (!HasComposeBankItems(hero)) {
                ResetStoreSequence();
                cb.beginTravelToZoneFn();
                return;
            }
            if (now - m_lastNpcActionTick > 2500) {
                ResetStoreSequence();
                cb.beginTravelToZoneFn();
            }
            return;
    }
}
