#pragma once
#include "plugin.h"
#include "hunt_settings.h"
#include "hunt_town.h"
#include "hunt_buffs.h"
#include "hunt_loot.h"
#include "revive_utils.h"
#include <memory>
#include <unordered_map>

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
    bool FindZoneExplorePosition(CHero* hero, CGameMap* map,
                                 const AutoHuntSettings& settings, Position& out) const;

    bool StartPathTo(CHero* hero, CGameMap* map, const Position& destination, int stopRange);
    bool StartWalkTo(CHero* hero, CGameMap* map, const Position& destination, int stopRange);
    bool StartPathNearTarget(CHero* hero, CGameMap* map, const Position& targetPos, int desiredRange);

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
};
