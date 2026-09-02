#pragma once
#include "plugin.h"
#include "hunt_settings.h"
#include "hunt_town.h"
#include "hunt_buffs.h"
#include "hunt_loot.h"
#include "revive_utils.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>

class CHero;
class CRole;
class CRoleMgr;
class CGameMap;
class TravelPlugin;
class CItem;
class CMagic;
struct CMapItem;

// ── BaseHuntPlugin ───────────────────────────────────────────────────────────
// Abstract base class for the shared hunt loop. Subclasses (melee, archer)
// implement the pure virtual combat interface; everything else (state machine,
// travel, loot, buffs, town runs, safety, zone management) lives here.
//
// Abstract base class — MeleeHuntPlugin and ArcherHuntPlugin inherit from this.
class BaseHuntPlugin : public IPlugin {
public:
    BaseHuntPlugin() { m_enabled = false; }

    // ── IPlugin overrides ────────────────────────────────────────────────
    void Update() override;
    void RenderUI() override;
    bool OnMapClick(const Position& tile) override;
    const char* GetName() const override = 0;  // subclass provides name
    void RenderDashboardUI();
    void SetAutomationEnabled(bool enabled);
    void ResumeEnabledStateFromSettings() override;

    // Renders the tabbed hunting dashboard for the active/selected hunt mode.
    void RenderGeneralUI();

    // ── Debug state accessors (virtual, base provides defaults) ──────────
    virtual int GetLastScatterRange() const { return 0; }
    Position GetLastTargetPos() const { return m_lastTargetPos; }
    Position GetDebugBestClumpCenter() const { return m_debugBestClumpCenter; }
    int GetDebugBestClumpSize() const { return m_debugBestClumpSize; }
    // Phase 2b: dynamic-zone cell-set state, exposed so the map overlay can
    // draw it and so hunt_settings.cpp's zone-membership functions (which
    // only see AutoHuntSettings, not a plugin instance) can bound Map-Wide
    // to it — see IsInDynamicZone/IsNearDynamicZone in hunt_settings.cpp.
    bool     IsDynZoneInit()      const { return m_dynInit; }
    OBJID    GetDynZoneMapId()    const { return m_dynMapId; }
    int      GetDynZoneCellTiles() const { return m_dynCellTiles; }
    std::vector<Position> GetDynZoneCellCenters() const;
    bool InDynamicZone(const Position& p) const;         // p inside any active cell
    int GetEditDragVertex() const { return m_editDragVertex; }
    void SetEditDragVertex(int idx) { m_editDragVertex = idx; }

protected:
    // ── Virtual combat interface (subclasses implement) ──────────────────
    virtual CRole* FindBestTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        Position* outApproachPos, Position* outAttackPos,
        int* outClumpSize, bool* outUseScatter) = 0;

    virtual void HandleCombatApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const Position& approachPos, bool movementCommitted) = 0;

    virtual void HandleCombatAttack(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const Position& attackPos, DWORD now) = 0;

    virtual bool HandleCombatRetreat(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target) { return false; }

    virtual void RenderCombatUI(AutoHuntSettings& settings) = 0;

    // Whether this combat mode needs a town run for arrows.
    virtual bool NeedsTownRunArrows(const CHero* hero, const AutoHuntSettings& settings) const { return false; }

    // Called during Update after buffs to manage combat-specific items (arrows etc.).
    // Return true to short-circuit the rest of Update.
    virtual bool HandleCombatItems(CHero* hero, const AutoHuntSettings& settings) { return false; }

    // Called when no target is found and no loot — subclass can do patrol or idle behavior.
    // Return true to short-circuit the rest of Update.
    virtual bool HandleNoTargetIdle(CHero* hero, CGameMap* map, const AutoHuntSettings& settings) { return false; }

    // Called at the top of Update to let subclass refresh combat-mode state.
    virtual void RefreshCombatState(CHero* hero, const AutoHuntSettings& settings) {}

    // Which combat mode this plugin represents — synced to settings each frame.
    virtual AutoHuntCombatMode GetExpectedCombatMode() const = 0;

    // ── Zone capture mode enum ───────────────────────────────────────────
    enum class ZoneCaptureMode {
        None,
        CircleCenter,
        CircleRadius,
        PolygonVertex,
    };

public:
    // Session 10: the hunt loop makes a decision every frame and records why
    // in m_statusText, but nothing ever displayed it — so "it isn't attacking"
    // was undiagnosable from the UI. These expose the state machine so the
    // overlay can show what it is doing and why.
    AutoHuntState      GetState() const      { return m_state; }
    const char*        GetStatusText() const { return m_statusText; }
    const char*        GetStateNamePublic() const { return GetStateName(); }
    const HuntTownService& GetTownService() const { return m_townService; }

    // Session 10: GetEffectiveHeroPosition() substitutes a PENDING JUMP
    // DESTINATION for the hero's real tile. Every range check and — critically
    // — every scatter DIRECTION vector is computed from it, so if the pending
    // state ever fails to clear, the bot aims from a tile it is not standing
    // on. Symptom: firing in the wrong direction and finding no local shot.
    // Exposed so the overlay can show effective-vs-actual instead of leaving
    // that to inference.
    Position GetEffectivePosDebug(const CHero* h) const { return GetEffectiveHeroPosition(h); }
    DWORD    GetPendingJumpTick() const { return m_pendingJumpTick; }
    Position GetPendingJumpDest() const { return m_pendingJumpDest; }

protected:
    // ── Shared protected methods ─────────────────────────────────────────
    void SetState(AutoHuntState state, const char* statusText);
    const char* GetStateName() const;
    void RefreshRuntimeState(CHero* hero, CGameMap* map);
    void StopAutomation(bool cancelTravel);

    bool HandleDeath(CHero* hero, TravelPlugin* travel, const AutoHuntSettings& settings);

    bool HasValidZone(const AutoHuntSettings& settings) const;
    bool IsPointInZone(const AutoHuntSettings& settings, OBJID mapId, const Position& pos) const;
    Position GetZoneAnchor(const AutoHuntSettings& settings) const;

    Position GetEffectiveHeroPosition(const CHero* hero) const;
    void ClearPendingJumpState();
    void ArmPendingJump(CHero* hero, const Position& destination, DWORD now, bool isRetreat);
    bool UpdatePendingJumpState(CHero* hero, DWORD now);

    // Pick somewhere inside the zone to go when there is nothing to fight.
    //
    // Without this the hunt loop had a genuine dead end: if idle patrol
    // produced no move and the hero was near the zone anchor, it set the
    // status text "Scanning for monsters" and returned — every frame, forever.
    // Scanning is not an action; the bot stood still indefinitely while
    // monsters sat just outside its search radius.
    //
    // Lives on the base plugin so every class gets it, not just the archer.
    // When fleeFromSet is non-null and non-empty, forces the Paranoia-evasion
    // selection: keep the in-zone/walkable tile that MAXIMIZES the distance to
    // the NEAREST point in the set (get away from every player at once, not
    // just one). Used by the evade state machine's Fleeing state, which passes
    // all tracked players' current + predicted positions.
    bool FindZoneExplorePosition(CHero* hero, CGameMap* map,
                                 const AutoHuntSettings& settings, Position& out,
                                 const std::vector<Position>* fleeFromSet = nullptr) const;

    // ── Paranoia evasion state machine (Session 14 redesign) ─────────────
    // Phase 1 "gentle flee": on detection, hop paranoiaFleeDistance tiles away
    // (out of the ~30-tile view) at human pace, then RESUME hunting where we
    // landed — no sprint across the map, no compulsive return. Only when the
    // same spot keeps drawing players (paranoiaAbandonAfter re-encounters, or a
    // player lingers in view too long) does it RELOCATE for good to a new
    // heatmap-hot area.
    enum class EvadeState { None, Fleeing, Relocating };
    // Pure state transition (visibility/detection/arrival driven). Called early
    // in the hunt loop so the loot-drop and leash-widen decisions see the
    // current evade state this same tick. Returns true while evading.
    bool UpdateEvadeState(CHero* hero, const AutoHuntSettings& settings);
    // Issues the movement for the current evade state. Returns true if it took
    // over the tick (caller should return from the hunt loop).
    bool DriveEvadeMovement(CHero* hero, CGameMap* map, const AutoHuntSettings& settings);
    // A bounded flee target ~fleeDistance tiles from the nearest threat, in the
    // away direction, walkable and reachable (backs off / spreads angularly if
    // the ideal point is blocked). False if nothing suitable found.
    bool FindGentleFleeTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
                              const std::vector<Position>& threatPositions, int fleeDistance,
                              Position& out) const;
    // Hottest SpawnMemory bucket that is in-zone, walkable and at least
    // minDistFromThreat tiles from EVERY position in threatPositions. False if
    // none qualifies (caller falls back to the farthest-flee tile).
    bool FindParanoiaRelocateDest(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
                                  const std::vector<Position>& threatPositions,
                                  int minDistFromThreat, Position& out) const;
    int  GetEvadeRelocateMinDist(const AutoHuntSettings& settings) const;

    // ── Player tracking for evasion (option A) ───────────────────────────
    // Every non-whitelisted player currently in the entity scan (NO range
    // limit — detection is binary: seen vs not seen), with enough position
    // history to derive a movement vector. UpdatePlayerTracks refreshes this
    // each evade tick; GetPlayerThreatPredictions returns the set of points to
    // flee away from — each player's current position AND a short lookahead in
    // their direction of travel, so the bot routes around where a moving player
    // is HEADING, not just where they are right now.
    struct PlayerTrack {
        Position posNow = {};
        Position posPrev = {};
        DWORD    lastSeenTick = 0;
        bool     hasPrev = false;   // a distinct earlier position was recorded
    };
    void UpdatePlayerTracks(const AutoHuntSettings& settings);
    std::vector<Position> GetPlayerThreatPredictions() const;

    // ── Phase 2b: Dynamic Zone Engine (the core logic for Map-Wide) ──────
    // Map-Wide no longer means "wander the whole map." It maintains a set of
    // active cells (m_dynCells, aligned to SpawnMemory's 8-tile bucket grid)
    // that exploration and clump-steering are focused inside: seeded as one
    // cell at the hero's position, then GROWN toward adjacent cells that keep
    // showing LIVE monsters (not heatmap history) and SHRUNK off cells that
    // sit empty — the shape continuously follows where mobs actually are,
    // instead of a fixed-radius circle that only ever moved on a timer. The
    // heatmap is still used, but only as the target list for a full
    // relocate (FindDynamicRecenterTarget) when the whole zone goes
    // unproductive — see UpdateDynamicZone for the size/kill-rate signal.
    // Targeting stays "engage whatever's near" (mobSearchRange) — the zone
    // only steers where the bot GOES.
    void UpdateDynamicZone(CHero* hero, const AutoHuntSettings& settings);
    bool IsDynamicZoneActive(const AutoHuntSettings& settings) const;
    int  CountMonstersInDynamicZone(const AutoHuntSettings& settings) const;
    bool FindDynamicRecenterTarget(const AutoHuntSettings& settings, OBJID mapId,
                                   const Position& from, Position& out) const;
    // Phase 3: wipes the current cell set and seeds a single fresh cell at
    // `pos` — the same reseed UpdateDynamicZone's own idle-relocate does,
    // exposed so Paranoia's evade relocate can move the zone itself instead
    // of leaving it anchored to the spot being fled. No-op when Map-Wide
    // isn't the active zone mode.
    void ReseedDynamicZoneAt(const Position& pos, const AutoHuntSettings& settings);

    bool StartPathTo(CHero* hero, CGameMap* map, const Position& destination, int stopRange);
    bool StartWalkTo(CHero* hero, CGameMap* map, const Position& destination, int stopRange);
    bool StartPathNearTarget(CHero* hero, CGameMap* map, const Position& targetPos, int desiredRange);

    // Shared by every hunt class and every zone mode: when nothing is
    // engageable within "Only Target Mobs Within" (mobSearchRange), walk
    // toward the nearest/densest monster clump ANYWHERE in the hunt zone
    // instead of random patrol/explore. mobSearchRange gates ATTACK range
    // only (don't scatter/shoot at mobs too far to hit) — it must not blind
    // the bot to distant clumps the minimap clearly shows. Returns true (and
    // starts a path + sets state) when it steered this tick.
    bool TrySteerTowardZoneClump(CHero* hero, CGameMap* map, const AutoHuntSettings& settings);

    // Session 10: periodic short random hop via StartWalkTo — see
    // settings.randomWalkIntervalMs. Paced independently of the shared
    // movement interval so it can be dialed well below it for testing.
    bool TryRandomWalk(CHero* hero, CGameMap* map, const AutoHuntSettings& settings, DWORD now);

    // Shared by ArcherHuntPlugin/MeleeHuntPlugin's HandleCombatAttack: use the
    // target-switch delay right after acquiring/approaching a target, else the
    // normal (cyclone-aware) attack interval.
    DWORD ComputeNextAttackDelayMs(CHero* hero, CRole* target, const AutoHuntSettings& settings) const;

    void BeginTravelToZone(TravelPlugin* travel, const AutoHuntSettings& settings);
    void BeginTravelToMarket(TravelPlugin* travel, CHero* hero, const AutoHuntSettings& settings);
    void HandleTravelToZone(TravelPlugin* travel, const AutoHuntSettings& settings);
    void HandleTravelToMarket(TravelPlugin* travel, CHero* hero, const AutoHuntSettings& settings);

    HuntTownCallbacks MakeTownCallbacks(TravelPlugin* travel, CHero* hero, const AutoHuntSettings& settings);
    HuntBuffCallbacks MakeBuffCallbacks(CHero* hero, CGameMap* map, const AutoHuntSettings& settings);

    bool IsPlayerWhitelisted(const AutoHuntSettings& settings, const char* name) const;
    bool CheckPlayerSafety(CHero* hero, CGameMap* map, TravelPlugin* travel, const AutoHuntSettings& settings);

    bool FindClosestZoneTile(CGameMap* map, const AutoHuntSettings& settings,
        const Position& from, Position& outZonePos) const;

    // ── UI helpers (will be moved to hunt_ui in Task 10) ─────────────────
    void RenderItemSelector(AutoHuntSettings& settings);
    void RenderSelectedItemList(const char* title, const char* clearButtonLabel,
        const char* tableId, std::vector<uint32_t>& itemIds);
    BaseHuntPlugin* FindHuntPluginForMode(AutoHuntCombatMode mode) const;
    BaseHuntPlugin* GetSelectedModePlugin() const;
    void ApplyHuntModeSelection(AutoHuntCombatMode mode, bool enabled);
    void RenderQuickSetupSection(BaseHuntPlugin* modePlugin);
    void RenderCombatSection();
    void RenderLootSection();
    void RenderTownRunsSection();
    void RenderSafetySection();
    void RenderAdvancedSection();
    void RenderDebugSection();
    void RenderZoneSetupUI(AutoHuntSettings& settings, CHero* hero);
    void RenderMonsterFilterUI(AutoHuntSettings& settings, CRoleMgr* mgr);
    void RenderSkillPriorityUI(AutoHuntSettings& settings);

    // ── Shared state members ─────────────────────────────────────────────
    // Pathfinder watchdog — see the Update() comment. Tracks whether the hero
    // is actually moving while a path claims to be active.
    Position m_watchdogLastPos = {};
    DWORD    m_watchdogLastMoveTick = 0;

    // Session 10: Update() was found running once per rendered frame (~150Hz)
    // with no gate on the decision work itself — only the final action packet
    // was interval-throttled. This tracks the last tick the heavy state-machine
    // section actually ran, so it can be capped independently via
    // settings.decisionThrottleMs. See RenderAdvancedSection / Update().
    DWORD m_lastDecisionTick = 0;

    // Session 10: last time TryRandomWalk() fired (or gave up trying), for
    // settings.randomWalkIntervalMs pacing.
    DWORD m_lastRandomWalkTick = 0;

    AutoHuntState m_state = AutoHuntState::Idle;
    char m_statusText[128] = "Disabled";
    Position m_lastHeroPos = {0, 0};
    OBJID m_lastMapId = 0;
    int m_lastHp = 0;
    int m_lastMaxHp = 0;
    int m_lastMana = 0;
    int m_lastMaxMana = 0;
    size_t m_lastBagCount = 0;
    ZoneCaptureMode m_zoneCaptureMode = ZoneCaptureMode::None;
    int m_editDragVertex = -1;
    char m_itemSearch[64] = "";

    DWORD m_lastAttackTick = 0;
    DWORD m_lastPackTick = 0;
    DWORD m_lastMoveTick = 0;
    DWORD m_manualControlPauseUntilTick = 0;

    // Session 13 [EXPLORE TABU]: explore destinations visited recently, so
    // the exploit pick can't oscillate between the map's two hottest buckets
    // (live: arrive at A -> A excluded by minTravel -> hottest remaining is B,
    // 25 tiles away -> arrive at B -> A hottest again -> ping-pong every
    // ~370ms, never pausing long enough to actually engage anything).
    mutable std::vector<std::pair<Position, DWORD>> m_recentExploreDests;

    // Session 13 [LOOT COMMITMENT]: id + expiry of a loot pickup currently in
    // progress — see the comment at its use site in the loot-priority block.
    OBJID m_lootCommitId = 0;
    DWORD m_lootCommitUntilTick = 0;

    Position m_pendingJumpDest = {};
    DWORD m_pendingJumpTick = 0;
    Position m_pendingJumpLastPos = {};
    DWORD m_pendingJumpLastProgressTick = 0;
    bool m_pendingJumpIsRetreat = false;

    DWORD m_approachStartTick = 0;
    OBJID m_unreachableTargetId = 0;
    DWORD m_unreachableExpireTick = 0;

    // Session 11 [LOCKUP FIX]: BeginTravelToZone used to get re-triggered
    // every tick (the zone-leash check outside any state guard) whenever the
    // hero isn't on/near the hunt zone map, with no limit — if the zone map
    // is genuinely unreachable (bad gateway data, wrong zoneMapId, etc.) this
    // retried forever, "Idle -> Travel To Zone -> Failed -> Idle -> ..." on
    // loop, which is exactly what made the bot impossible to turn off live.
    // Bounded retry + auto-disable on repeated failure (see StartPathTo/
    // BeginTravelToZone in base_hunt_plugin.cpp) fixes that regardless of
    // whatever the underlying map-data problem turns out to be.
    int m_zoneTravelFailCount = 0;

    // Session 15 [ZONE-UNREACHABLE FALLBACK]: when we're already ON the zone map
    // but the hunt-zone anchor is genuinely unreachable (e.g. a portal landed us
    // across an impassable center — the Ape City 2 / map-1075 self-disable), we
    // hunt from the current position instead of disabling, and back off retrying
    // the anchor until this tick so it doesn't thrash travel-to-zone every frame.
    DWORD m_zoneUnreachableUntilTick = 0;

    ReviveState m_reviveState;
    HuntBuffManager m_buffMgr;
    HuntLootManager m_lootMgr;
    HuntTownService m_townService;

    OBJID m_targetId = 0;
    int m_lastClumpSize = 0;
    Position m_lastTargetPos = {};
    Position m_debugBestClumpCenter = {};
    int m_debugBestClumpSize = 0;

    std::unordered_map<OBJID, DWORD> m_nearbyPlayerTicks;
    bool m_safetyResting = false;
    DWORD m_safetyRestStartTick = 0;

    // Session 14 [Paranoia evasion — Phase 1] runtime state — see UpdateEvadeState.
    EvadeState m_evadeState = EvadeState::None;
    Position   m_evadeFleeTarget = {};      // current gentle-flee destination (~fleeDistance away)
    std::vector<Position> m_evadeThreatPositions;  // players' last-known positions, snapshotted when a
                                                   // relocate is triggered — relocate away from all of them
    Position   m_evadeRelocateDest = {};    // chosen relocate destination (computed on entering Relocating)
    DWORD      m_evadeFleeStartTick = 0;     // when the current Fleeing began (for the linger-escalation timer)
    int        m_evadeRenudgeCount = 0;      // reappearances mid-relocate → push next relocate farther
    int        m_evadeEncounterCount = 0;    // re-encounters at the current spot; >= paranoiaAbandonAfter → relocate
    DWORD      m_evadeLastEncounterTick = 0; // for decaying the encounter count after undisturbed hunting

    std::unordered_map<OBJID, PlayerTrack> m_playerTracks;  // see PlayerTrack / UpdatePlayerTracks

    // Phase 2b dynamic-zone runtime state (Map-Wide only) — see UpdateDynamicZone.
    bool     m_dynInit = false;
    OBJID    m_dynMapId = 0;
    int      m_dynCellTiles = 8;        // cell size in effect for the CURRENT m_dynCells (see settings.dynZoneCellTiles)
    std::unordered_set<uint32_t> m_dynCells;               // active zone membership (SpawnMemory bucket keys)
    std::unordered_map<uint32_t, int> m_dynCellHotStreak;  // frontier candidates: consecutive live-mob ticks (grow)
    std::unordered_map<uint32_t, int> m_dynCellColdStreak; // member cells: consecutive empty ticks (shrink)
    DWORD    m_dynProductiveTick = 0;   // last time we were productive (a real kill) — drives full relocate
    int      m_dynLastKills = 0;        // CHero::GetGameKillCount() snapshot (accurate, +0xA30)
    DWORD    m_dynLastSizeTick = 0;     // throttle for the grow/shrink + productivity step
    DWORD    m_dynLastRecenterTick = 0; // throttle between full relocates
};
