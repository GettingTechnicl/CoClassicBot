#include "game.h"
#include "jitter.h"
#include "hunt_intervals.h"
#include "map_items.h"
#include "hunt_loot.h"
#include "hunt_town.h"
#include "hooks.h"
#include "CHero.h"
#include "CGameMap.h"
#include "CItem.h"
#include "itemtype.h"
#include "pathfinder.h"
#include "log.h"
#include <algorithm>
#include <unordered_set>

// ── File-local constants ──────────────────────────────────────────────────────
static constexpr int   kLootPickupAttemptLimit   = 2;
// Session 13 [GHOST LOOT FIX]: how long an item may survive an exact-on-tile
// pickup packet before it's declared a ghost (stale heap-scan entry) and
// ignored for the rest of its client-side lifetime. Real pickups resolve in
// one server round trip — well under a second even on a bad connection.
static constexpr DWORD kLootGhostConfirmMs       = 1500;
static constexpr DWORD kLootTargetSwitchIntervalMs = 100;

static constexpr int kMinLootPickupIgnoreMs = 0;
static constexpr int kMaxLootPickupIgnoreMs = 300000;
static constexpr int kMinItemPickupDelayMs = 0;
static constexpr int kMaxItemPickupDelayMs = 3000;
static constexpr DWORD kDropRecordMatchWindowMs = 30000;  // match items against drop records within 30s

// ── File-local helpers ────────────────────────────────────────────────────────
namespace {

DWORD GetLootPickupIgnoreMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.lootPickupIgnoreMs, kMinLootPickupIgnoreMs, kMaxLootPickupIgnoreMs);
}

DWORD GetItemPickupDelayMs(const AutoHuntSettings& settings)
{
    if (ShouldUseAggressiveSpeeds(settings))
        return kMinItemPickupDelayMs;
    return ClampMs(settings.itemPickupDelayMs, kMinItemPickupDelayMs, kMaxItemPickupDelayMs);
}

bool IsConfirmedDrop(const Position& pos, DWORD now)
{
    for (const auto& drop : GetLootDropRecords()) {
        if ((now - drop.tick) > kDropRecordMatchWindowMs)
            continue;
        if (std::abs(pos.x - drop.pos.x) <= 1 && std::abs(pos.y - drop.pos.y) <= 1)
            return true;
    }
    return false;
}

bool TickIsFuture(DWORD targetTick, DWORD now)
{
    return static_cast<int32_t>(targetTick - now) > 0;
}

bool IsMovementCommand(const CCommand& cmd)
{
    return cmd.iType == _COMMAND_WALK
        || cmd.iType == _COMMAND_RUN
        || cmd.iType == _COMMAND_WALKFORWARD
        || cmd.iType == _COMMAND_RUNFORWARD
        || cmd.iType == _COMMAND_JUMP;
}

bool IsMovementCommandStillAdvancing(const CHero* hero)
{
    if (!hero)
        return false;
    const CCommand& cmd = hero->GetCommand();
    if (!IsMovementCommand(cmd))
        return false;
    return cmd.posTarget.x != hero->m_posMap.x || cmd.posTarget.y != hero->m_posMap.y;
}

} // namespace

// ── HuntLootManager implementation ───────────────────────────────────────────

CMapItem* HuntLootManager::FindBestLoot(
    CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    std::function<bool(OBJID, DWORD)> isLootPickupIgnoredFn,
    std::function<bool(OBJID mapId, const Position&)> isPointInZoneFn) const
{
    if (!hero || !map) return nullptr;
    // Session 13 [BAG-FULL MONEY FIX]: a full bag used to skip ALL loot here —
    // including money, which goes straight to the silver counter and never
    // occupies a bag slot. Live session: bag sat at 40/40 for three straight
    // minutes (autoStore and trash-drop both off) and the bot walked over
    // every gold pile without picking any up. Full bag now degrades to
    // money-only looting instead of none; it only fully skips when money
    // looting is off too (nothing could match).
    const bool bagFull = hero->IsBagFull();
    if (bagFull && !settings.lootMoney) {
        spdlog::trace("[hunt-loot] FindBestLoot: bag full (money loot off), skip");
        return nullptr;
    }

    const DWORD now = GetTickCount();
    const DWORD spawnGraceMs = GetLootSpawnGraceMs(settings);
    CMapItem* best = nullptr;
    float bestDist = (std::numeric_limits<float>::max)();
    int totalItems = 0, skippedFilter = 0, skippedIgnored = 0, skippedZone = 0, skippedSpawnGrace = 0, skippedNotOurDrop = 0, skippedOutOfRange = 0, skippedBagFull = 0;

    // True if this is a money drop we actually want (lootMoney on AND at/above
    // the configured minimum tier) — reused below since both the gold-value
    // floor bypass and the primary inclusion gate need the same check.
    auto isWantedMoney = [&settings](const CMapItem& item) {
        return settings.lootMoney && HuntTownService::GetMoneyTier(item) >= settings.minimumGoldTier;
    };

    for (CMapItem* itemRef : MapItems::Get()) {
        if (!itemRef) continue;
        // Session 11 [CRASH FIX]: the later IsAlive() check (base_hunt_plugin.cpp,
        // after FindBestLoot returns) only protects the ONE item that ends up
        // selected - every item touched during the scan itself, below, was
        // NOT validated, the same gap PruneLootPickupAttempts had (see that
        // fix's comment for the full mechanism: ground items are raw
        // pointers into the game's own heap, freeable at any moment).
        if (!MapItems::IsAlive(itemRef)) continue;
        ++totalItems;
        // Bag-full money-only mode (see the bagFull comment above).
        if (bagFull && !isWantedMoney(*itemRef)) { ++skippedBagFull; continue; }
        const auto seenResult = m_lootSeenTicks.try_emplace(itemRef->m_id, now);
        const DWORD seenAge = now - seenResult.first->second;
        if (seenAge < spawnGraceMs) { ++skippedSpawnGrace; continue; }
        if (isLootPickupIgnoredFn && isLootPickupIgnoredFn(itemRef->m_id, now)) { ++skippedIgnored; continue; }
        if (isPointInZoneFn && !isPointInZoneFn(Game::GetCurrentMapId(), itemRef->m_pos)) { ++skippedZone; continue; }

        // Phase 2a: gold-value floor.  Universal pickup filter — applies even
        // to confirmed drops.  Items explicitly listed in lootItemIds bypass
        // the floor (user-curated whitelist always wins).
        if (settings.minimumLootGoldValue > 0
            && !HuntTownService::IsSelectedLootItem(settings, itemRef->m_idType)
            && !isWantedMoney(*itemRef)) {
            const ItemTypeInfo* info = GetItemTypeInfo(itemRef->m_idType);
            if (info && info->price < (uint32_t)settings.minimumLootGoldValue) {
                ++skippedFilter;
                continue;
            }
        }

        // Confirmed drops (our kill via system message) are always looted.
        // Other items must pass the item filter (loot list, quality, or plus).
        const bool confirmed = IsConfirmedDrop(itemRef->m_pos, now);
        if (!confirmed) {
            if (!HuntTownService::ShouldLootMapItem(settings, *itemRef)) { ++skippedFilter; continue; }
            if (!HuntTownService::IsSelectedLootItem(settings, itemRef->m_idType)
                && !isWantedMoney(*itemRef)) {
                ++skippedNotOurDrop;
                continue;
            }
        }

        const float dist = hero->m_posMap.DistanceTo(itemRef->m_pos);

        // Session 13 [LOOT RANGE FIX]: "Loot Range" was only ever applied to
        // the PICKUP check (IsWithinLootPickupRange, base_hunt_plugin.cpp) —
        // never here, when the item is SELECTED. So with Loot Range 6 this
        // still happily picked an item 41 tiles away, and the hunt loop then
        // committed to a cross-map loot run for it, abandoning combat.
        // Live-traced: the bot chain-jumped to items 41 -> 24 -> 47 -> 30
        // tiles out, item to item, barely attacking at all — which is what
        // made loot-enabled runs collapse to a fraction of the kill rate of
        // loot-disabled ones. Priority items (meteor/dragonball/+gear) keep
        // their deliberate "go get it from anywhere" exemption, matching
        // IsWithinLootPickupRange's own carve-out.
        const int lootRange = std::clamp(settings.lootRange, 0, (int)CGameMap::MAX_JUMP_DIST);
        if (lootRange > 0
            && dist > (float)lootRange
            && !HuntTownService::IsPriorityLootItem(*itemRef)) {
            ++skippedOutOfRange;
            continue;
        }

        if (dist < bestDist) {
            bestDist = dist;
            best = itemRef;
        }
    }

    if (best) {
        spdlog::trace("[hunt-loot] FindBestLoot: picked id={} type={} pos=({},{}) dist={:.1f} | ground={} filteredOut={} ignored={} outOfZone={} spawnGrace={} notOurDrop={} outOfRange={} bagFullSkip={}",
            best->m_id, best->m_idType, best->m_pos.x, best->m_pos.y, bestDist,
            totalItems, skippedFilter, skippedIgnored, skippedZone, skippedSpawnGrace, skippedNotOurDrop,
            skippedOutOfRange, skippedBagFull);
    }

    return best;
}

bool HuntLootManager::IsLootPickupIgnored(OBJID itemId, DWORD now)
{
    const auto it = m_lootPickupAttempts.find(itemId);
    if (it == m_lootPickupAttempts.end())
        return false;

    // Ghost check (see LootPickupAttemptState): an item still alive this long
    // after an on-tile pickup packet is a stale entry — ignore it until it
    // finally drops out of the scan list (PruneLootPickupAttempts erases this
    // record with it). Unlike ignoreUntilTick this never expires, which is
    // the point: the timed ignore is what let ghosts cycle back forever.
    LootPickupAttemptState& state = it->second;
    if (state.firstOnTileSendTick != 0
        && now - state.firstOnTileSendTick > kLootGhostConfirmMs) {
        if (!state.ghostLogged) {
            state.ghostLogged = true;
            spdlog::info("[hunt-loot] Item id={} confirmed GHOST (alive {}ms after on-tile pickup) — ignored until it despawns",
                itemId, now - state.firstOnTileSendTick);
        }
        return true;
    }

    return TickIsFuture(state.ignoreUntilTick, now);
}

void HuntLootManager::RecordLootPickupAttempt(OBJID itemId, DWORD now,
    const AutoHuntSettings& settings)
{
    LootPickupAttemptState& state = m_lootPickupAttempts[itemId];
    if (!TickIsFuture(state.ignoreUntilTick, now)) {
        state.ignoreUntilTick = 0;
        if (state.attempts >= kLootPickupAttemptLimit)
            state.attempts = 0;
    }

    if (state.attempts < UINT8_MAX)
        ++state.attempts;

    if (state.attempts >= kLootPickupAttemptLimit) {
        const DWORD ignoreMs = GetLootPickupIgnoreMs(settings);
        state.attempts = 0;
        state.ignoreUntilTick = now + ignoreMs;
        spdlog::info("[hunt-loot] Item id={} hit pickup attempt limit ({}), ignoring for {}ms",
            itemId, kLootPickupAttemptLimit, ignoreMs);
    } else {
        spdlog::trace("[hunt-loot] Item id={} pickup attempt {}/{}", itemId, state.attempts, kLootPickupAttemptLimit);
    }
}

void HuntLootManager::PruneLootPickupAttempts(CGameMap* map)
{
    if (m_lootPickupAttempts.empty() && m_lootSeenTicks.empty() && m_lootArrivedTicks.empty())
        return;
    if (!map) {
        ResetLootPickupAttempts();
        return;
    }

    // Session 11 [CRASH FIX]: this is called unconditionally on EVERY
    // Update() tick (see base_hunt_plugin.cpp) and, for every ground item,
    // used to dereference itemRef->m_id with no liveness check at all —
    // unlike every other place in this file that touches a CMapItem*
    // (FindBestLoot/TryPickupLootItem both call MapItems::IsAlive() first).
    // Ground items are raw pointers into the GAME's own heap from a
    // heap-scan (map_items.cpp) — the game can free one at any moment
    // between the scan snapshot and this loop reaching it, and running that
    // exposure on every single tick for every item gave a real, confirmed-
    // live, reproducible use-after-free plenty of chances to land (crash
    // symbolized to std::unordered_set<uint32_t>::emplace's hash step
    // reading a freed CMapItem's m_id field — see coclassicbot-project-status
    // memory for the full trace).
    const std::vector<CMapItem*> mapItems = MapItems::Get();
    std::unordered_set<OBJID> activeItemIds;
    activeItemIds.reserve(mapItems.size());
    for (CMapItem* itemRef : mapItems) {
        if (itemRef && MapItems::IsAlive(itemRef))
            activeItemIds.insert(itemRef->m_id);
    }

    for (auto it = m_lootPickupAttempts.begin(); it != m_lootPickupAttempts.end();) {
        if (activeItemIds.find(it->first) == activeItemIds.end()) {
            it = m_lootPickupAttempts.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = m_lootSeenTicks.begin(); it != m_lootSeenTicks.end();) {
        if (activeItemIds.find(it->first) == activeItemIds.end()) {
            it = m_lootSeenTicks.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = m_lootArrivedTicks.begin(); it != m_lootArrivedTicks.end();) {
        if (activeItemIds.find(it->first) == activeItemIds.end()) {
            it = m_lootArrivedTicks.erase(it);
        } else {
            ++it;
        }
    }
}

void HuntLootManager::ResetLootPickupAttempts()
{
    m_lootPickupAttempts.clear();
    m_lootSeenTicks.clear();
    m_lootArrivedTicks.clear();
    m_lastLootItemId = 0;
}

bool HuntLootManager::TryPickupLootItem(CHero* hero, const AutoHuntSettings& settings,
    const CMapItem* item, DWORD now,
    std::function<bool(DWORD)> updatePendingJumpFn)
{
    if (!hero || !item)
        return false;

    // Session 10 [CRASH FIX]: `item` can be called across several Update()
    // ticks while the hero walks/jumps over to it — plenty of time for it to
    // be picked up (freeing the memory) between when the caller captured the
    // pointer and this specific call. Re-validate before any field read.
    if (!MapItems::IsAlive(item)) {
        MapItems::Invalidate();
        return false;
    }

    const DWORD spawnGraceMs = GetLootSpawnGraceMs(settings);
    const auto seenResult = m_lootSeenTicks.try_emplace(item->m_id, now);
    const DWORD seenAge = now - seenResult.first->second;
    if (seenAge < spawnGraceMs) {
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=spawn_grace age={}ms grace={}ms",
            item->m_id, item->m_idType, seenAge, spawnGraceMs);
        return false;
    }

    const int actualDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, item->m_pos.x, item->m_pos.y);
    if (actualDist != 0) {
        // Not on the tile yet, so no arrival has happened — clear any stale
        // arrival timestamp (e.g. the hero walked off and back).
        m_lootArrivedTicks.erase(item->m_id);
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} dist={} reason=not_on_tile",
            item->m_id, item->m_idType, actualDist);
        return false;
    }

    const DWORD pickupDelayMs = GetItemPickupDelayMs(settings);
    if (pickupDelayMs > 0) {
        const auto arrivedResult = m_lootArrivedTicks.try_emplace(item->m_id, now);
        const DWORD arrivedAge = now - arrivedResult.first->second;
        if (arrivedAge < pickupDelayMs) {
            spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=arrival_delay age={}ms delay={}ms",
                item->m_id, item->m_idType, arrivedAge, pickupDelayMs);
            return false;
        }
    }

    if (hero->IsJumping() || IsMovementCommandStillAdvancing(hero)) {
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=movement_not_settled pos=({},{}) target=({},{})",
            item->m_id, item->m_idType, hero->m_posMap.x, hero->m_posMap.y, item->m_pos.x, item->m_pos.y);
        return false;
    }

    if (updatePendingJumpFn && updatePendingJumpFn(now)) {
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=pending_jump age={}ms",
            item->m_id, item->m_idType, now);
        return false;
    }

    if (Pathfinder::Get().IsActive()) {
        Pathfinder::Get().Stop();
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=stopping_pathfinder",
            item->m_id, item->m_idType);
        return false;
    }

    const DWORD pathfinderJumpAge = now - Pathfinder::Get().GetLastJumpTick();
    if (Pathfinder::Get().GetLastJumpTick() != 0 && pathfinderJumpAge < 500) {
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=pathfinder_settle age={}ms",
            item->m_id, item->m_idType, pathfinderJumpAge);
        return false;
    }

    const DWORD interval = m_lastLootItemId == item->m_id
        ? GetItemActionIntervalMs(settings)
        : kLootTargetSwitchIntervalMs;
    const DWORD elapsed = now - m_lastLootTick;
    if (elapsed < interval) {
        spdlog::trace("[hunt-loot] TryPickup throttled id={} type={} elapsed={}ms interval={}ms",
            item->m_id, item->m_idType, elapsed, interval);
        return false;
    }

    hero->PickupItem(*item);
    m_lastLootTick   = now;
    m_lastLootItemId = item->m_id;
    RecordLootPickupAttempt(item->m_id, now, settings);
    // Arm the ghost timer (see LootPickupAttemptState): dist==0 is guaranteed
    // here by the not_on_tile check above. Skipped when the bag is full — a
    // full bag makes a REAL item fail pickup too, and we don't want to brand
    // it a ghost over that. Money is exempt from that guard: it never needs a
    // bag slot, so a money pile surviving an on-tile pickup is a ghost
    // regardless of bag state (the live ghosts were in fact Gold, 1090020).
    if (!hero->IsBagFull() || HuntTownService::GetMoneyTier(*item) >= 0) {
        LootPickupAttemptState& ghostState = m_lootPickupAttempts[item->m_id];
        if (ghostState.firstOnTileSendTick == 0)
            ghostState.firstOnTileSendTick = now;
    }
    spdlog::trace("[hunt-loot] PickupItem id={} type={} pos=({},{}) sent_on_tile",
        item->m_id, item->m_idType, item->m_pos.x, item->m_pos.y);
    return true;
}

// =============================================================================
// Phase 2a: bag-full trash drop
// =============================================================================

bool HuntLootManager::IsBagItemTrash(const AutoHuntSettings& settings, const CItem& item)
{
    const uint32_t typeId = item.GetTypeID();

    // ── Hard safety filters: never drop these ─────────────────────────────────
    // 1) Anything in any user-curated keep list.
    if (HuntTownService::IsSelectedLootItem(settings, typeId))           return false;
    if (HuntTownService::IsSelectedWarehouseItem(settings, typeId))      return false;
    if (HuntTownService::IsSelectedPriorityReturnItem(settings, typeId)) return false;
    // 2) Equipment is never auto-dropped from the bag.
    if (item.IsEquipment())                                              return false;
    // 3) Anything plussed (+1 or higher) — these are valuable upgrades.
    if (item.GetPlus() > 0)                                              return false;
    // 4) Don't drop arrows the archer plugin maintains.
    if (settings.arrowTypeId != 0 && typeId == settings.arrowTypeId)     return false;

    // ── Now apply the user's trash criteria (OR-of-thresholds) ───────────────
    bool flagged = false;
    if (settings.autoDropMinKeepQuality > 0) {
        const int quality = (int)(typeId % 10);
        if (quality < settings.autoDropMinKeepQuality)
            flagged = true;
    }
    if (!flagged && settings.autoDropMinKeepPrice > 0) {
        if (const ItemTypeInfo* info = GetItemTypeInfo(typeId)) {
            if (info->price < (uint32_t)settings.autoDropMinKeepPrice)
                flagged = true;
        }
    }
    return flagged;
}

bool HuntLootManager::TryDropTrashItem(CHero* hero, const AutoHuntSettings& settings, DWORD now)
{
    if (!hero) return false;
    if (!settings.autoDropTrashWhenFull) return false;
    // Both criteria off → nothing to do.
    if (settings.autoDropMinKeepQuality <= 0 && settings.autoDropMinKeepPrice <= 0)
        return false;

    // Only fire when the bag is at/over the user's storage threshold.
    const int bagThreshold = CHero::ClampBagThreshold(settings.bagStoreThreshold);
    if ((int)hero->m_deqItem.size() < bagThreshold)
        return false;

    // Throttle: re-use itemActionIntervalMs so we don't spam drop packets.
    const DWORD interval = GetItemActionIntervalMs(settings);
    if ((now - m_lastTrashDropTick) < interval)
        return false;

    // Pick the lowest-quality trash item (deterministic; avoids sticky loops).
    CItem* victim = nullptr;
    int    victimScore = 0;
    for (const auto& itemRef : hero->m_deqItem) {
        if (!itemRef) continue;
        if (!IsBagItemTrash(settings, *itemRef)) continue;
        // Lower quality first, then lower price (rough proxy for worst-first).
        const uint32_t typeId = itemRef->GetTypeID();
        const int      qualityScore = (int)(typeId % 10) * 1000000;
        const ItemTypeInfo* info = GetItemTypeInfo(typeId);
        const int      priceScore = info ? (int)std::min<uint32_t>(info->price, 999999u) : 999999;
        const int      score = qualityScore + priceScore;
        if (!victim || score < victimScore) {
            victim = itemRef.get();
            victimScore = score;
        }
    }
    if (!victim) return false;

    hero->DropItem(victim->GetID(), hero->m_posMap);
    m_lastTrashDropTick   = now;
    m_lastTrashDropItemId = victim->GetID();
    spdlog::info("[hunt-loot] DropTrash id={} type={} name='{}' quality={} plus={} bag={}/{}",
        victim->GetID(), victim->GetTypeID(), victim->GetName(),
        victim->GetQuality(), victim->GetPlus(),
        (int)hero->m_deqItem.size(), CHero::MAX_BAG_ITEMS);
    return true;
}
