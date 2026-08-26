#pragma once
#include <string>
#include "base.h"
#include <cstdint>
#include <vector>

enum class AutoHuntZoneMode {
    Circle = 0,
    Polygon = 1,
    Route = 2,      // recorded patrol path — see HuntRoute
};

// How long to stay and fight before advancing along a route.
//
// Deliberately not a fixed tile/time knob: the right behaviour depends on
// whether the character one-shots monsters (move on immediately, keep spawns
// cycling) or is levelling on them (stay and finish). Auto decides from the
// measured time-to-kill, so it adapts when gear or mob difficulty changes.
enum class AutoHuntLingerMode {
    Auto = 0,
    MoveOn = 1,
    StayAndClear = 2,
};

// A recorded patrol path for one map.
//
// Routes are a guide, not a cage. The bot follows the waypoints, but may leave
// the line by up to its attack range to engage, and attacks anything within
// striking range from wherever it ends up — so the effective kill band is
// roughly twice the attack range around the path, while the body never strays
// further than one attack range. That keeps it from wandering off while still
// clearing monsters that would otherwise be walked past and suppress spawns.
struct HuntRoute
{
    std::string           name;
    OBJID                 mapId = 0;
    std::vector<Position> waypoints;
};

enum class AutoHuntCombatMode {
    Melee = 0,
    Archer = 1,
};

// ── Skill priority system ────────────────────────────────────────────────────
enum class HuntSkillType : int {
    Superman = 0,
    Cyclone = 1,
    Accuracy = 2,
    XpFly = 3,
    Fly = 4,
    Stigma = 5,
    Count = 6,
};

const char* HuntSkillName(HuntSkillType type);

struct HuntSkillEntry {
    HuntSkillType type;
    bool enabled = false;
};

constexpr int kHuntSkillCount = static_cast<int>(HuntSkillType::Count);

enum class AutoHuntState {
    Idle,
    WaitingForGame,
    Ready,
    TravelToZone,
    AcquireTarget,
    ApproachTarget,
    AttackTarget,
    LootNearby,
    Recover,
    TravelToMarket,
    Repair,
    BuyArrows,
    StoreItems,
    ReturnToZone,
    Failed,
};

struct AutoHuntSettings
{
    bool enabled = false;
    bool usePotions = true;
    bool autoRepair = true;
    bool autoStore = true;
    bool autoDepositSilver = false;
    bool autoReviveInTown = true;
    // Skill priority list — order determines cast priority, enabled flag per skill
    HuntSkillEntry skillPriorities[kHuntSkillCount] = {
        { HuntSkillType::Superman, false },
        { HuntSkillType::Cyclone, true },
        { HuntSkillType::Accuracy, false },
        { HuntSkillType::XpFly, false },
        { HuntSkillType::Fly, false },
        { HuntSkillType::Stigma, false },
    };

    // Legacy bools — kept in sync by SyncSkillBoolsFromPriorities()
    bool castSuperman = false;
    bool castCyclone = true;
    bool supermanBeforeCyclone = false;
    bool useAccuracyIfCycloneActive = false;
    bool useStigma = false;
    bool archerMode = false;
    bool castXpFly = false;
    bool castFly = false;
    bool flyOnlyWithCyclone = false;

    // Call after changing skillPriorities to sync legacy bools
    void SyncSkillBoolsFromPriorities();
    bool useScatterLogic = true;
    bool pickupNearbyHpPotionWhenLow = false;
    bool pickupNearbyManaPotionForStigma = false;
    bool prioritizeMobClumps = true;
    bool prioritizeScatterClumps = true;
    int mobSearchRange = 0;
    bool immediateReturnOnPriorityItems = false;
    bool storeTreasureBank = true;
    bool storeComposeBank = true;
    bool packMeteorsIntoScrolls = false;
    bool lootRefined = false;
    bool lootUnique = false;
    bool lootElite = false;
    bool lootSuper = false;
    bool lootMoney = true;
    bool storeRefined = false;
    bool storeUnique = false;
    bool storeElite = false;
    bool storeSuper = false;
    bool buyArrows = false;
    int hpPotionPercent = 50;
    int manaPotionPercent = 35;
    int repairPercent = 70;
    int bagStoreThreshold = 36;
    int clumpRadius = 8;
    int minimumMobClump = 5;
    // Session 10: was 3, which meant scatter only fired on a 3+ clump and
    // everything else fell through to single-target shots. Backwards for an
    // AoE that costs no more than a normal attack: if anything is in the cone,
    // scatter is at least as good. At 1, the existing approach search still
    // repositions when a short move would catch more — so "scatter unless a
    // reposition would hit that same target plus others" is the behaviour.
    int minimumScatterHits = 1;
    int scatterRangeOverride = 0;
    int actionRadius = 6;
    int rangedAttackRange = 0;
    int archerSafetyDistance = 0;
    int lootRange = 5;
    int movementIntervalMs = 900;
    int attackIntervalMs = 300;
    int cycloneAttackIntervalMs = 100;
    int targetSwitchAttackIntervalMs = 75;
    int itemActionIntervalMs = 700;
    int lootSpawnGraceMs = 1000;
    // Session 10: how long the background heap scanners (entities.h, map_items.h)
    // cache their results before rescanning. Exposed as a slider mainly so the
    // Themida-anti-tamper crash hypothesis can be tested live (slow the scan
    // way down, see if crash frequency changes) without a rebuild.
    int entityScanIntervalMs = 500;
    // How long the hero must stand on an item's tile (dist == 0) before the
    // FIRST pickup attempt fires. 0 preserves the original behaviour (attempt
    // immediately). Distinct from itemActionIntervalMs, which only throttles
    // RETRIES after a first attempt has already been made.
    int itemPickupDelayMs = 0;
    // Session 10: caps how often BaseHuntPlugin::Update()'s heavy decision
    // section (target/loot scanning, pathfinding requests, state machine) runs.
    // Discovered running unthrottled at the render frame rate (~150Hz) with
    // sustained memory growth over long sessions — this is the lever to test
    // whether that hot loop is the leak source. 0 = unthrottled (old behavior).
    int decisionThrottleMs = 50;
    // Session 10: periodically walk a short random hop instead of the usual
    // jump, purely to exercise the walk code path regularly during real
    // autohunt use (rather than only when a target happens to end up 1-2
    // tiles away) — both for its own sake and as a controllable-frequency
    // stress test for the still-open memory leak investigation (down to 50ms
    // = ~20 real actions/sec). 0 disables it entirely.
    int randomWalkIntervalMs = 0;
    int selfCastIntervalMs = 1000;
    int npcActionIntervalMs = 400;
    int lootPickupIgnoreMs = 30000;
    int manualControlPauseMs = 3000;
    int reviveDelayMs = 20000;
    int reviveRetryIntervalMs = 1000;
    int minimumLootPlus = 0;
    int minimumStorePlus = 0;
    // ── Phase 2a: gold-value floor for ground-item pickup ─────────────────
    // 0 disables; otherwise items whose ItemTypeInfo::price is below this
    // value are skipped on pickup unless explicitly listed in lootItemIds.
    int minimumLootGoldValue = 0;
    // ── Phase 2a: bag-full trash drop ────────────────────────────────────
    // When enabled and bag size >= bagStoreThreshold, the loot manager will
    // drop bagged items that meet either the quality cutoff or price cutoff
    // below.  Strict safety filters apply (never drops equipment, plussed
    // items, arrows, or anything appearing in lootItemIds / warehouseItemIds
    // / priorityReturnItemIds).
    bool autoDropTrashWhenFull = false;
    int  autoDropMinKeepQuality = 0;   // 0 disables — drop items with quality strictly less than N
    int  autoDropMinKeepPrice   = 0;   // 0 disables — drop items with ItemTypeInfo::price strictly less than N
    int silverKeepAmount = 0;
    uint32_t arrowTypeId = 1050002; // SpeedArrow
    int arrowBuyCount = 3;
    OBJID zoneMapId = 0;
    AutoHuntCombatMode combatMode = AutoHuntCombatMode::Melee;
    AutoHuntZoneMode zoneMode = AutoHuntZoneMode::Circle;
    Position zoneCenter = {0, 0};
    int zoneRadius = 12;
    std::vector<Position> zonePolygon;

    // ── Recorded routes (session 10) ──
    std::vector<HuntRoute> routes;
    std::string            activeRouteName;
    AutoHuntLingerMode     lingerMode = AutoHuntLingerMode::Auto;
    // How close to a waypoint counts as "arrived" before advancing.
    int                    routeWaypointTolerance = 2;
    // 0 = derive the corridor from the class's attack range (the intended
    // behaviour). A positive value pins it, for cases where you want the bot
    // held tighter or given more room than its range implies.
    int                    routeCorridorOverride = 0;
    std::vector<uint32_t> lootItemIds;
    std::vector<uint32_t> warehouseItemIds;
    std::vector<uint32_t> priorityReturnItemIds;
    char monsterNames[256] = "";
    char monsterIgnoreNames[256] = "";
    char monsterPreferNames[256] = "";
    char playerWhitelist[256] = "";
    bool usePacketJump = false;
    bool safetyEnabled = false;
    bool safetyNotifyDiscord = false;
    int safetyPlayerRange = 15;
    int safetyDetectionSec = 30;
    int safetyRestSec = 120;

    // Debug map overlays
    bool debugShowActionRadius = false;
    bool debugShowClumpRadius = false;
    bool debugShowMobSearchRange = false;
    bool debugShowLootRange = false;
    bool debugShowSafetyRange = false;
    bool debugShowAttackRange = false;
    bool debugShowArcherSafety = false;
    bool debugShowScatterRange = false;
    bool debugShowBestClump = false;

    // Zone editor state
    int zoneCaptureMode = 0;
    int editDragVertex = -1;
};

AutoHuntSettings& GetAutoHuntSettings();

// Zone geometry free functions
bool IsZeroPos(const Position& pos);
Position PolygonCentroid(const std::vector<Position>& points);
bool PointInPolygon(const Position& point, const std::vector<Position>& polygon);

// Zone helper free functions
bool HasValidHuntZone(const AutoHuntSettings& settings);
bool IsPointInHuntZone(const AutoHuntSettings& settings, OBJID mapId, const Position& pos);

// Is a tile inside the hunt zone, or within `margin` tiles of it?
//
// The zone bounds where we HUNT, not where every footstep must land. Treating
// it as a hard cage means a clump straddling the boundary can never be
// engaged — the bot orbits the edge instead of fighting. AoE positioning uses
// this with a margin of the skill's range so it can step just outside to line
// up a shot. Shared deliberately: this applies to any class with an area
// attack, not just the archer's scatter.
bool IsPointNearHuntZone(const AutoHuntSettings& settings, OBJID mapId, const Position& pos, int margin);

// ── Route helpers ────────────────────────────────────────────────────────
// The active route for the current map, or nullptr.
const HuntRoute* GetActiveRoute(const AutoHuntSettings& settings, OBJID mapId);

// Distance from a point to the route polyline (perpendicular to segments, not
// just to the waypoints — a long straight leg must count as "on the path"
// everywhere along it, not only at its endpoints).
float DistanceToRoute(const HuntRoute& route, const Position& pos);

// Index of the waypoint nearest `pos`. Used to resume patrol after a fight has
// dragged the bot off the line, so it rejoins where it left rather than
// restarting the circuit.
int NearestWaypointIndex(const HuntRoute& route, const Position& pos);

// Corridor half-width for route mode: the class's attack range unless
// overridden. This is the leash — the bot's body may leave the line by this
// much and no more.
int GetRouteCorridor(const AutoHuntSettings& settings);

// ── The leash (applies to EVERY zone mode, not just routes) ──────────────
// How far the bot's BODY may leave the zone: one attack range. From wherever
// it ends up it may strike anything in range, so the effective kill band is
// about twice the attack range beyond the zone edge, while the bot itself
// never wanders further than one range out.
//
// This is what stops a circle/polygon behaving as a cage: a monster just
// outside the boundary is still worth killing (and killing it keeps spawns
// cycling), but the bot won't chase it across the map.
int GetHuntLeash(const AutoHuntSettings& settings);

// Margin for deciding whether a MONSTER is engageable: leash x2, since the bot
// may stand one leash outside the zone and strike one range beyond that.
int GetHuntEngageMargin(const AutoHuntSettings& settings);

// Attack range actually in effect: the explicit setting if non-zero,
// otherwise the equipped weapon's reach from the game's item table.
int GetEffectiveAttackRange(const AutoHuntSettings& settings);

// ── Route recording ──────────────────────────────────────────────────────
// Walk the patrol you want, then stop and name it. The raw tile trail is
// collapsed to corner waypoints via CGameMap::SimplifyPath, so a route is a
// handful of turns rather than hundreds of tiles.
void RouteRecordStart();
void RouteRecordSample(const Position& pos);   // call once per frame
bool RouteRecordStop(AutoHuntSettings& settings, OBJID mapId, const char* name);
bool RouteRecordIsActive();
int  RouteRecordSampleCount();
Position GetHuntZoneAnchor(const AutoHuntSettings& settings);
