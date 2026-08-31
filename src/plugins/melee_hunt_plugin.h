#pragma once
#include "base_hunt_plugin.h"

class MeleeHuntPlugin : public BaseHuntPlugin {
public:
    const char* GetName() const override { return "Melee Hunt"; }

protected:
    CRole* FindBestTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        Position* outApproachPos, Position* outAttackPos,
        int* outClumpSize, bool* outUseScatter) override;

    void HandleCombatApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const Position& approachPos, bool movementCommitted) override;

    void HandleCombatAttack(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const Position& attackPos, DWORD now) override;

    void RenderCombatUI(AutoHuntSettings& settings) override;
    AutoHuntCombatMode GetExpectedCombatMode() const override { return AutoHuntCombatMode::Melee; }

private:
    CRole* FindBestMeleeTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        Position* outApproachPos, int* outClumpSize) const;

    bool FindBestClumpApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        const std::vector<CRole*>& targets, Position& outApproachPos,
        CRole*& outPrimaryTarget, int& outClumpSize) const;

    // Session 14 [MELEE APPROACH FIX]: melee-only replacement for the shared
    // FindBestSingleTargetApproach (hunt_targeting.cpp) when positioning on a
    // single target inside/near a pack. Two differences from the shared
    // version, both deliberately NOT made to the shared function so archer's
    // behavior stays untouched: (1) never selects the target's own tile —
    // the shared version's distance-based ranking treats standing ON the
    // target (attackDist=0) as strictly better than standing adjacent
    // (attackDist=1), which is exactly the "jumps directly on top of the
    // monster" behavior reported live; (2) among the remaining (genuinely
    // adjacent) candidates, prefers whichever side has the most OTHER
    // targets within settings.clumpRadius, so melee ends up positioned
    // toward the density of the pack rather than an arbitrary adjacent tile.
    bool FindBestMeleeApproachPos(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const std::vector<CRole*>& allTargets, const Position& heroPos,
        Position& outApproachPos, int attackRange) const;

    // Session 14 [MELEE TARGET COMMITMENT]: see settings.meleeMinHitsPerTarget's
    // comment (hunt_settings.h) for the full bug this fixes. Tracks which
    // target the bot is currently committed to attacking and how many attack
    // ATTEMPTS have landed on it, so FindBestMeleeTarget's clump-clearing
    // step can keep returning the SAME target across ticks instead of
    // re-randomizing every single one.
    mutable OBJID m_committedTargetId = 0;
    mutable int   m_hitsOnCommittedTarget = 0;

    // Called from HandleCombatAttack right after a real attack packet is
    // sent, so the commitment counter reflects actual attempts, not ticks.
    void NoteMeleeAttackAttempt(OBJID targetId);

    // Session 14 [MELEE NO-JITTER ATTACK]: melee-only replacement for the
    // shared BaseHuntPlugin::ComputeNextAttackDelayMs (archer uses that one
    // directly, unaffected by this). Same targetChanged/justFinishedApproach
    // logic, but built on hunt_intervals.h's *NoJitter interval getters —
    // per the user, a melee character already fixed on and attacking a
    // target has no reason to pause between swings the way movement between
    // monsters does; jitter stays for the approach/reposition side only.
    DWORD ComputeMeleeAttackDelayMs(CHero* hero, CRole* target, const AutoHuntSettings& settings) const;

    // Session 14 [STEALTH TICKBOX]: the effective Instant Attack state for
    // combat decisions — settings.usePacketJump, but forced OFF while
    // settings.disableInstantAttackOnPlayer is set AND any non-whitelisted
    // player is visible (no range). Detection/debounce live in the shared
    // IsAnyPlayerVisibleDebounced (hunt_intervals.h), so melee and speedhack
    // react to the exact same 1.5s-held sighting. Used everywhere the combat
    // logic reads usePacketJump (approach walk-vs-jump, adjacent-attack gate,
    // cyclone target-randomize); the UI checkbox itself still reads/writes the
    // raw setting. Returns true = behave as Instant Attack on; false = behave
    // as off (walk up, attack adjacent).
    bool IsInstantAttackActive(const AutoHuntSettings& settings) const;
};
