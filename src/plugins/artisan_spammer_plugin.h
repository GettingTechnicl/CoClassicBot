#pragma once
#include "plugin.h"
#include "base.h"
#include <vector>
#include <utility>

// Session 14 [ARTISAN AUTO-REPAIR + STOP-ON-LEVEL-UP]: drives one item at a
// time through the game's own compose-and-check cycle instead of blind-
// firing a whole 1:1-paired queue. Idle decides what to do next for the
// current item (repair if durability dropped, send another material if
// full), then hands off to one of the two Wait* phases to poll for the
// actual outcome (level-up vs durability-drop vs repaired) via CItem's own
// verified fields (GetPlus()/GetDurabilityRaw() — same fields already
// static_assert-checked and relied on elsewhere in this codebase).
enum class ArtisanPhase {
    Idle,
    WaitUpgradeResult,
    WaitRepairResult,
};

class ArtisanSpammerPlugin : public IPlugin {
public:
    const char* GetName() const override { return "Artisan Spammer"; }
    void Update() override;
    void RenderUI() override;

private:
    static bool SendArtisanPacket(OBJID targetItemId, OBJID materialItemId, uint32_t action);

    int  m_selectedTarget = -1;     // index into unique equipment type list
    int  m_materialType = 0;        // 0=Meteor, 1=MeteorScroll, 2=DragonBall

    // User-facing toggles for the two originally-missing pieces.
    bool m_autoRepair = true;
    bool m_stopOnLevelChange = false;

    bool m_spamming = false;
    ArtisanPhase m_phase = ArtisanPhase::Idle;

    std::vector<OBJID> m_targetQueue;    // remaining target items still to process
    std::vector<OBJID> m_materialQueue;  // shared material pool, consumed as we go

    OBJID m_currentTargetId  = 0;
    // Session 14 [REAL LEVEL FIELD]: "item level" in the Artisan Wind
    // mechanic turned out to be neither GetPlus() nor a hidden byte
    // anywhere in CItem — live-confirmed via before/after dumps that a
    // successful upgrade changes the item's TYPE ID (and name) to a
    // different item outright, e.g. ApeCoat(133443) -> Gambeson(133453).
    // A failed attempt keeps the same type id and only reduces durability.
    OBJID m_prevTypeId       = 0;  // snapshot taken right before sending an attempt
    int   m_prevDurabilityRaw = 0;
    DWORD m_phaseStartTick   = 0;  // when the current Wait* phase started (for timeout)
    int   m_phaseFailCount   = 0;  // consecutive no-result timeouts in the current Wait* phase

    int  m_sentCount     = 0;  // upgrade attempts sent
    int  m_successCount  = 0;  // confirmed level-ups
    int  m_repairCount   = 0;  // confirmed repairs
    int  m_totalTargets  = 0;  // target items queued this run

    DWORD m_lastActionTick = 0;
    int  m_delayMs = 100;           // ms between packets (also gates repair sends)
};
