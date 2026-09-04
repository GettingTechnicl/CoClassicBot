#pragma once
#include "base_hunt_plugin.h"

class CMagic;

class ArcherHuntPlugin : public BaseHuntPlugin {
public:
    const char* GetName() const override { return "Archer Hunt"; }
    int GetLastScatterRange() const override { return m_lastScatterRange; }

protected:
    // Required virtual overrides
    CRole* FindBestTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        Position* outApproachPos, Position* outAttackPos,
        int* outClumpSize, bool* outUseScatter) override;
    void HandleCombatApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const Position& approachPos, bool movementCommitted) override;
    void HandleCombatAttack(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const Position& attackPos, DWORD now) override;
    bool HandleCombatRetreat(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target) override;
    bool HandleCombatItems(CHero* hero, const AutoHuntSettings& settings) override;
    bool NeedsTownRunArrows(const CHero* hero, const AutoHuntSettings& settings) const override;
    bool NeedsTownRunArrowsEmergency(const CHero* hero, const AutoHuntSettings& settings) const override;
    void RenderCombatUI(AutoHuntSettings& settings) override;
    bool HandleNoTargetIdle(CHero* hero, CGameMap* map, const AutoHuntSettings& settings) override;
    void RefreshCombatState(CHero* hero, const AutoHuntSettings& settings) override;
    AutoHuntCombatMode GetExpectedCombatMode() const override { return AutoHuntCombatMode::Archer; }

private:
    // Scatter geometry
    CMagic* FindScatterMagic(const CHero* hero) const;
    int GetScatterRange(const CHero* hero) const;
    bool IsTargetInScatterSector(const Position& origin, const Position& castPos,
        const Position& targetPos, int range) const;
    bool FindBestScatterShot(const std::vector<CRole*>& targets, const Position& origin,
        int range, int minimumHits, Position& outCastPos,
        CRole*& outPrimaryTarget, int& outHitCount) const;
    bool FindBestScatterApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        const std::vector<CRole*>& targets, int range, int minimumHits,
        Position& outApproachPos, Position& outCastPos,
        CRole*& outPrimaryTarget, int& outHitCount,
        bool preferFarTiles = false) const;

    // Retreat
    bool TryArcherDangerRetreat(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings, CRole* target);
    bool FindSafeArcherRetreat(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        const std::vector<CRole*>& threats, CRole* target, Position& outRetreatPos,
        int safetyDistOverride = 0) const;

    // Target finding
    CRole* FindBestArcherTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        Position* outApproachPos, Position* outAttackPos,
        int* outClumpSize, bool* outUseScatter) const;
    bool FindArcherPatrolPosition(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings, Position& outPatrolPos) const;

    // Arrow management
    bool TryManageArrows(CHero* hero, const AutoHuntSettings& settings);
    bool NeedsArrows(const CHero* hero, const AutoHuntSettings& settings) const;
    bool NeedsArrowsEmergency(const CHero* hero, const AutoHuntSettings& settings) const;
    int CountUsableArrowPacks(const CHero* hero) const;

    // Archer-specific state
    int m_lastScatterRange = 0;
    int m_lastScatterHitCount = 0;
    mutable DWORD m_lastScatterApproachTick = 0;
    // Session 13 [FIRE-RATE FIX]: the clump this archer committed to
    // approaching, and when it committed. Without this, FindBestArcherTarget
    // re-scored every clump on each ~7ms decision tick and chased whichever
    // momentarily scored highest — live trace showed it ping-ponging between
    // clumps ~38 tiles apart with 15-18 tile jumps, "Move blocked: pathfinder
    // active" hundreds of times per second, and only ~1 scatter every 2-4s
    // because it never stayed put long enough to fire. Sticking with one
    // chosen approach position until arrival (or expiry) is what lets it land
    // and actually shoot.
    mutable Position m_committedApproachPos = {};
    mutable DWORD m_committedApproachTick = 0;
    // Session 13 [DON'T CAMP]: position and start-tick of the current
    // stationary-fire hold — see the [DON'T CAMP] comment in
    // HandleCombatApproach. Caps how long "a local shot exists" is allowed to
    // keep the archer standing dead still repeatedly re-casting at the same
    // spot, instead of advancing through the pack between shots.
    Position m_stationaryFirePos = {};
    DWORD    m_stationaryFireSinceTick = 0;
    // Cycles the patrol band so idle movement drifts inward and outward
    // instead of tracing the zone rim. See FindArcherPatrolPosition.
    mutable int   m_patrolDriftPhase = 0;
    // Session 13 [PATROL COMMIT]: current patrol destination, held until
    // reached or expired. Without this, every no-target tick picked a FRESH
    // patrol point (the compass direction and radius band both rotate per
    // call), so consecutive short paths swung wildly — live trace showed an
    // 18-tile jump west immediately followed by 17 tiles back east, zero net
    // progress, during a 15s dead window in a sparse zone.
    Position m_patrolCommitPos = {};
    DWORD    m_patrolCommitTick = 0;
    // Hold-and-look window after arriving at a patrol destination — see the
    // [ARRIVAL DWELL] comment in HandleNoTargetIdle.
    DWORD    m_patrolDwellUntilTick = 0;
    Position m_lastFailedRetreatDest = {};
    DWORD m_lastFailedRetreatTick = 0;
    DWORD m_retreatCooldownTick = 0;
    DWORD m_retreatHoldUntilTick = 0;
    DWORD m_lastArrowTick = 0;
};
