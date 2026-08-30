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
static constexpr DWORD kLootGhostConfirmMs       = 800;
// Session 13 [STALE LOOT SKIP]: an item still in our heap-scan list this long
// after we FIRST saw it is stale (or another player's protected drop we can't
// take either way), so FindBestLoot skips it without ever walking there.
// Before this, every ghost cost a trip: the bot had to jump to it, stand on
// it, and fail a pickup before the ghost blacklist could catch it (~190
// ghost-trips per session).
//
// Session 13 [PRECISE DESPAWN TIMES]: originally a conservative 180000ms
// guess ("a minute or two"). User hand-timed the actual server despawn
// clocks: gold despawns at 1:10 (70000ms), other items at 1:20 (80000ms)
// after dropping — the first version of this constant used those measured
// times (minus a small margin), split by type.
//
// Session 13 [EFFICIENCY TUNING]: user's follow-up call, given how fast the
// bot moves: don't wait anywhere near the full despawn window before giving
// up — a flat 50000ms for both gold and ordinary items, tightened from the
// measured-despawn-derived 68000/78000ms split. Trades a little real,
// still-collectible loot in the 50-70/80s range for not continuing to
// consider/re-evaluate something increasingly likely to be gone or
// unreachable. Meteors/DragonBalls are exempt from this check entirely (see
// its use site) rather than getting their own longer number — their real
// despawn timer has never been measured, so "give up early" has no evidence
// behind it for them the way it does for ordinary drops, and they're
// priority items specifically because they're worth fetching regardless of
// age. Note: m_lootSeenTicks starts from when OUR scan first notices the
// item, which can lag the true server drop time if the item existed before
// it entered scan range — that only makes this UNDER-count age (occasionally
// still giving up sooner than 50s of true age), never over-count it.
static constexpr DWORD kLootStaleMaxAgeMs        = 50000;
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
    // Session 13 [PRIORITY SELECTION WEIGHT]: tracked separately from `best`
    // so a priority item (meteor/DragonBall/+gear) always wins the pick,
    // regardless of distance, once any exists among the passing candidates.
    // The earlier [PRIORITY SELECTION GAP] fix got a priority item PAST the
    // confirmed-drop/list/quality gate, but this loop still only tracked a
    // single nearest-wins `best` — in a dense loot field (200+ ground items
    // is typical, live-confirmed) an ordinary coin is nearly always closer
    // than a meteor sitting 20-40 tiles out, so the meteor won for exactly
    // one decision tick right after a jump happened to land it in the lead,
    // then instantly lost to nearer clutter on the very next tick. Live log:
    // a meteor picked at dist=22.8, replaced 108ms later by dist=19, then
    // 5.1 — never actually pursued long enough to be reached. Both tiers use
    // nearest-first as their own tie-break; a priority item only loses to
    // another, closer priority item, never to ordinary loot.
    CMapItem* bestPriority = nullptr;
    float bestPriorityDist = (std::numeric_limits<float>::max)();
    int totalItems = 0, skippedFilter = 0, skippedIgnored = 0, skippedZone = 0, skippedSpawnGrace = 0, skippedOutOfRange = 0, skippedBagFull = 0, skippedStale = 0;

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
        // Computed early (moved up from the confirmed-drop gate below) so the
        // stale-age skip just below can exempt it too — see that comment.
        const bool isPriorityItem = HuntTownService::IsMeteorOrDragonBallItem(*itemRef);
        const auto seenResult = m_lootSeenTicks.try_emplace(itemRef->m_id, now);
        const DWORD seenAge = now - seenResult.first->second;
        if (seenAge < spawnGraceMs) { ++skippedSpawnGrace; continue; }
        // Stale skip: too old to still be worth considering. Session 13
        // [EFFICIENCY TUNING]: tightened from the measured-despawn-minus-
        // margin values (68000/78000ms) to a flat 50000ms per the user's own
        // call — give up on stale gold/items sooner to keep the bot focused
        // on fresher loot and hunting rather than continuing to consider
        // something that's probably going stale soon anyway. Meteors/
        // DragonBalls are exempt entirely: they're priority items precisely
        // because they're rare and worth fetching regardless of age, and
        // unlike ordinary drops their real despawn timer has never been
        // measured — assuming they follow the same clock and giving up early
        // risks abandoning a still-real one for no proven reason.
        if (!isPriorityItem && seenAge > kLootStaleMaxAgeMs) { ++skippedStale; continue; }
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
        //
        // Session 13 [PRIORITY SELECTION GAP, then RELIABILITY SPLIT]:
        // Meteor/DragonBall was previously only consulted AFTER an item
        // already passed this gate — the elaborate "stop everything and go
        // get it" handling downstream (base_hunt_plugin.cpp: bypasses Loot
        // Range, the combat-priority check, steer-to-pack) never got a
        // chance to run for one that wasn't ALSO a confirmed drop of ours or
        // already in the Loot Item List. A meteor/DragonBall now bypasses
        // this gate the same way a confirmed drop does. This check is
        // DELIBERATELY the narrow, type-ID-only IsMeteorOrDragonBallItem, not
        // a broader "+items too" version — that version existed briefly and
        // let an unverified CMapItem::GetPlus() misread bypass the user's
        // ENTIRE quality/list filter, sweeping in ordinary equipment as
        // "priority" (live-reported: bag filling with junk). A +item is
        // still real, selectable loot via ShouldLootMapItem's own
        // (equipment-sort-scoped) plus-check just below — it simply isn't
        // granted this gate-bypass or the downstream override behavior,
        // which the user described as meteor/DragonBall-specific from the
        // start.
        const bool confirmed = IsConfirmedDrop(itemRef->m_pos, now);
        // isPriorityItem computed earlier in this loop (see the stale-age
        // skip above, which also needs it).
        //
        // Session 13 [REDUNDANT GATE BUG]: this used to ALSO require
        // IsSelectedLootItem(...) || isWantedMoney(...) after ShouldLootMapItem
        // already passed — but ShouldLootMapItem is a complete, self-contained
        // OR of every valid inclusion path (money, Loot Item List, a checked
        // quality, or Minimum Loot Plus on real equipment). Re-requiring
        // list-or-money on top of that silently vetoed anything that passed
        // ONLY via quality or plus — i.e., every ordinary item the quality
        // checkboxes and plus slider exist to catch, since the Loot Item List
        // is realistically only ever populated with meteor/DragonBall IDs.
        // Live-confirmed: across a full session, ZERO non-money items ever
        // won selection, including a hand-verified +1 PhoenixHook the user
        // dropped and stood next to — it never appeared in the log even
        // once. This bug predates this session's work; it was previously
        // masked by the broader GetPlus()-inclusive selection-gate bypass
        // (see IsMeteorOrDragonBallItem's header comment) letting SOME items
        // through a different, incorrect path, which muddied the picture
        // until that bypass was narrowed to meteor/DragonBall-only.
        if (!confirmed && !isPriorityItem
            && !HuntTownService::ShouldLootMapItem(settings, *itemRef)) {
            ++skippedFilter;
            continue;
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
            && !isPriorityItem) {
            ++skippedOutOfRange;
            continue;
        }

        if (isPriorityItem) {
            if (dist < bestPriorityDist) {
                bestPriorityDist = dist;
                bestPriority = itemRef;
            }
            continue;  // never lets an ordinary item's `best`/bestDist see this one
        }
        if (dist < bestDist) {
            bestDist = dist;
            best = itemRef;
        }
    }

    // A priority item always wins, any distance — see [PRIORITY SELECTION
    // WEIGHT] above.
    if (bestPriority) {
        best = bestPriority;
        bestDist = bestPriorityDist;
    }

    if (best) {
        spdlog::trace("[hunt-loot] FindBestLoot: picked id={} type={} pos=({},{}) dist={:.1f}{} | ground={} filteredOut={} ignored={} outOfZone={} spawnGrace={} outOfRange={} bagFullSkip={} stale={}",
            best->m_id, best->m_idType, best->m_pos.x, best->m_pos.y, bestDist,
            best == bestPriority ? " [PRIORITY]" : "",
            totalItems, skippedFilter, skippedIgnored, skippedZone, skippedSpawnGrace,
            skippedOutOfRange, skippedBagFull, skippedStale);
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
