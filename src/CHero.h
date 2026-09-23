#pragma once
#include "CRole.h"
#include "CItem.h"
#include "CMagic.h"
#include "CStatTable.h"
#include "field_offsets.h"
#include <algorithm>

struct CMapItem;

// =====================================================================
// Equipment slot indices (0-based, 8 slots total)
// From FUN_140189950 — equipment update handler
// =====================================================================
namespace EquipSlot {
    constexpr int HEAD     = 0;
    constexpr int NECKLACE = 1;
    constexpr int ARMOR    = 2;
    constexpr int RWEAPON  = 3;  // right hand (main weapon)
    constexpr int LWEAPON  = 4;  // left hand (shield / dual wield)
    constexpr int RING     = 5;
    constexpr int GARMENT  = 6;
    constexpr int BOOTS    = 7;
    constexpr int COUNT    = 8;
}

const char* GetEquipSlotName(int slot);

// Session 5 debug-only entry point — see the definition in CHero.cpp for why
// this is deliberately NOT part of CHero::PickupItem(). Only call this from
// an explicit, manual test action (e.g. the overlay debug button), never from
// automated bot logic.
//
// [CONNECTION-DECIPHER 2026-09-18] `skipJump` (default false, preserves prior
// behavior): the normal jump-then-pickup sequence produces two enqueued
// messages close together, which a netfinder capture showed merging into one
// wire write (docs/investigation/CONNECTION_DECIPHER_PREP.md) -- useless for
// getting a byte-aligned known-plaintext<->ciphertext pair. Skipping the jump
// yields an isolated pickup send, cleanly aligned for black-box cipher
// characterization against the wire capture.
bool DebugTestNativePickup(const CMapItem& item, bool skipJump = false);

// Session 9: manual test hook for SendJumpPacket(), separate from any hunt/
// travel logic. `applyLocalPrediction` lets the debug button test both modes
// — see packets.cpp session-8 comment: ApplyLocalJumpPrediction's struct
// offsets are not yet re-verified for v1074 and crashed the game once
// already, so this is deliberately exposed rather than silently defaulted.
// Only call this from an explicit, manual test action, never from automated
// bot logic.
bool DebugTestJump(int destX, int destY, bool applyLocalPrediction);

// Session 10: on-demand test for CHero::Walk()'s SetCommand-based fallback.
// Only call this from an explicit, manual test action, never from automated
// bot logic — same rule as DebugTestJump above.
bool DebugTestWalk(int destX, int destY);

// =====================================================================
// CHero — the player's controllable character
//
// Hierarchy: CMapObj -> CRole -> CHero
//
// The weak_ptr at CRole+0x08 (from enable_shared_from_this) was
// previously mistaken for a separate shared_ptr<CRole>; it actually
// points back to the object itself.
//
// Offsets verified via Ghidra + Cheat Engine.
// =====================================================================
#pragma pack(push, 1)
class CHero : public CRole
{
public:
    static CHero* GetSingletonPtr();

    static constexpr int MAX_BAG_ITEMS = 40;

    // Shared by every bag-capacity threshold setting (bagStoreThreshold,
    // townBagThreshold, dropItemThreshold) — was independently re-clamped
    // the same way in 4 different places.
    static int ClampBagThreshold(int value) { return std::clamp(value, 1, MAX_BAG_ITEMS); }

private:
    // Every pad here is computed from field_offsets.h's per-build constants (+ a
    // sizeof() for the preceding field) — see CRole.h's matching comment. On v1078
    // most of these gaps come out numerically unchanged even though the ABSOLUTE
    // offsets shift, because both of a gap's neighbouring fields moved by the same
    // amount; a couple genuinely grew (silver-vs-stat-table, active-npc-vs-vecMagic) —
    // see docs/investigation/V1078_OFFSET_FINDINGS.md for the measured deltas.
    BYTE _pad71C[FieldOffsets::CHero_StatTable - kCRoleDataEnd]; // hero runtime fields

public:
    CStatTable* m_pStatTable;      // FieldOffsets::CHero_StatTable — current HP/stat table

private:
    // v1074: a ~0x50-byte field was inserted between +0x968 and the old +0xA30,
    // shifting every field below by +0x50. Live-verified against character "Kinux"
    // (silver, equipped GoldCoronet/Lathee, 18 skills). See coclassicbot-live-offsets.
    BYTE _pad970[FieldOffsets::CHero_Silver - FieldOffsets::CHero_StatTable - sizeof(CStatTable*)];

public:
    uint64_t m_qwRuntimeA30;       // FieldOffsets::CHero_Silver — silver in low 32 bits [v1074 LIVE-VERIFIED =7681; v1078 VALUE-CONFIRMED =4800/4349, character S411]

private:
    BYTE _padA88[FieldOffsets::CHero_DeqItem - FieldOffsets::CHero_Silver - sizeof(uint64_t)];

public:
    std::deque<PItem> m_deqItem;   // FieldOffsets::CHero_DeqItem — inventory items [v1074 inferred from equip offset; v1078 base VALUE-CONFIRMED by pointer-scan, see map_probe.h's DebugFindInventory for why the deque's OWN size/count is not trusted on either build]

private:
    BYTE _padDeq[FieldOffsets::CHero_Equipment - FieldOffsets::CHero_DeqItem - sizeof(std::deque<PItem>)]; // gap between deque end and equipment

public:
    PItem m_equipment[EquipSlot::COUNT]; // FieldOffsets::CHero_Equipment — equipped items [v1074 LIVE-VERIFIED: GoldCoronet/Lathee; v1078 VALUE-CONFIRMED: Coat/LuckyBow/LuckyArrow in the right slots, character S411]

private:
    BYTE _padC58[FieldOffsets::CHero_MaxMana - FieldOffsets::CHero_Equipment - sizeof(PItem) * EquipSlot::COUNT];

public:
    int32_t m_nMaxMana;            // FieldOffsets::CHero_MaxMana — cached max MP [v1074 inferred; v1078 VALUE-CONFIRMED =0, correct for an archer with no mana pool]
    uint8_t m_bMaxManaValid;       // FieldOffsets::CHero_MaxManaValid — max MP cache-valid byte [v1078 VALUE-CONFIRMED =1]

private:
    BYTE _padCFD[FieldOffsets::CHero_NpcActive - FieldOffsets::CHero_MaxManaValid - sizeof(uint8_t)];

public:
    // FieldOffsets::CHero_NpcActive [v1074 LIVE-VERIFIED 2026-09-02; v1078 MODEL, not yet
    // read at all]: 0 -> 1 when a dialog opens (the only 0/1 flip in a closed-vs-open hero
    // dump) — but STICKY: it stays 1 after the dialog is closed, and a bot-sent ActivateNpc
    // packet on a session that never had a manual click leaves it 0 even though the dialog
    // renders. So it answers "has a dialog ever been opened by a click", not
    // "is one open now". Do not gate on it alone. See m_pNpcDialog.
    BOOL m_bNpcActive;

private:
    BYTE _pad1054[FieldOffsets::CHero_NpcDialog - FieldOffsets::CHero_NpcActive - sizeof(BOOL)];

public:
    // FieldOffsets::CHero_NpcDialog [v1074 LIVE-VERIFIED 2026-09-02, npctest v3; v1078
    // MODEL]: pointer to the current NPC dialog object. Sticky across a close too — but it
    // is REPLACED with a fresh object every time a dialog is created, including dialogs
    // opened by the bot's own ActivateNpc packet (observed 0AAC01A0 -> 08F45D50 within
    // 300 ms of the send). That change is the only client-side signal found
    // that says "a dialog appeared just now" for a packet-opened dialog, which
    // is what the confirm-open gates in hunt_town.cpp need. Compare against
    // the value captured immediately before sending the activate.
    uintptr_t m_pNpcDialog;

private:
    BYTE _pad1068[FieldOffsets::CHero_VecMagic - FieldOffsets::CHero_NpcDialog - sizeof(uintptr_t)];

public:
    std::vector<PMagic> m_vecMagic; // FieldOffsets::CHero_VecMagic [v1074 LIVE-VERIFIED: count 18; v1078 MODEL]

private:
    BYTE _pad1980[FieldOffsets::CHero_ActiveNpc - FieldOffsets::CHero_VecMagic - sizeof(std::vector<PMagic>)];

public:
    // FieldOffsets::CHero_ActiveNpc [v1074 LIVE-VERIFIED 2026-09-02; v1078 MODEL — a
    // plausible NPC id (100122) was seen structurally nearby on v1078 but this exact field
    // was never individually value-confirmed]: id of the NPC the client's own click path
    // last opened (101545 -> 101395 when MillionaireLee was clicked). Same caveat as
    // m_bNpcActive: set by the click path, sticky, not updated by a bot-sent activate on a
    // cold session.
    OBJID m_idActiveNpc;

private:
    BYTE _pad3778[FieldOffsets::CHero_Vip - FieldOffsets::CHero_ActiveNpc - sizeof(OBJID)];

public:
    BOOL m_bVip;                    // FieldOffsets::CHero_Vip — VIP status flag [v1074 inferred; v1078 MODEL]

    // ── Helpers ──
    bool IsBagFull() const { return m_deqItem.size() >= MAX_BAG_ITEMS; }
    CItem* GetEquip(int slot) const {
        if (slot < 0 || slot >= EquipSlot::COUNT) return nullptr;
        return m_equipment[slot].get();
    }

    void Jump(int nX, int nY);
    void JumpPacket(int nX, int nY);
    void Walk(int nX, int nY);
    void Attack(OBJID idTarget);
    void AttackTarget(OBJID idTarget, const Position& posTarget);
    void ShootTarget(OBJID idTarget);
    void MagicAttack(OBJID idMagic, const Position& posTarget);
    void PickupItem(OBJID idItem, const Position& pos);
    void PickupItem(const CMapItem& item);
    void MagicAttack(OBJID idMagic, OBJID idTarget, const Position& posTarget);
    void StartMining();
    void UseItem(OBJID idItem);
    void DropItem(OBJID idItem, const Position& pos);
    void EquipItem(OBJID idItem, int slot);
    void UnequipItem(OBJID idItem, int slot);
    void RepairItem(OBJID idItem);
    void OpenWarehouse(OBJID idNpc);
    void DepositWarehouseItem(OBJID idNpc, OBJID idItem);
    void DepositWarehouseSilver(OBJID idNpc, uint32_t amount);
    void WithdrawWarehouseItem(OBJID idNpc, OBJID idItem);
    void OpenTreasureBank(OBJID idNpc);
    void DepositTreasureBankMeteors(OBJID idNpc);
    void DepositTreasureBankDragonBalls(OBJID idNpc);
    void OpenComposeBank(OBJID idNpc);
    void DepositComposeBankAll();
    void CancelFly();
    void Sit();
    void ReviveInTown();
    bool VipTeleport(OBJID mapId);
    void VipTeleportTwinCity();
    void BuyItem(OBJID idNpc, uint32_t typeId);
    void SellItem(OBJID idNpc, OBJID idItem);
    void StartTrade(OBJID idPlayer);
    void OfferTradeItem(OBJID idItem);
    void AcceptTrade(OBJID idPlayer);
    void CancelTrade(OBJID idPlayer);

    // ── NPC interaction (packet-based) ──
    void ActivateNpc(OBJID idNpc);
    void AnswerNpc(int answer);                  // uses default taskId=101
    void AnswerNpcEx(int answer, int taskId);    // explicit task ID
    bool IsNpcActive() const;
    OBJID GetActiveNpc() const;
    // Opaque token for "which dialog object is current". Capture it right
    // before ActivateNpc(); NpcDialogOpenedSince(token) is true once the
    // client has created a NEW dialog object (i.e. the server answered and the
    // dialog rendered). Works on a cold session where m_bNpcActive never flips.
    uintptr_t GetNpcDialogToken() const { return m_pNpcDialog; }
    bool NpcDialogOpenedSince(uintptr_t token) const {
        return m_pNpcDialog != 0 && m_pNpcDialog != token;
    }

    // ── State queries ──
    // Returns -1 ("unknown") on a failed/implausible read -- NEVER 0. Callers
    // must treat a negative return as "skip this tick's HP-based decision,"
    // not as empty health. See CHero.cpp's definition and
    // docs/investigation/CURRENT_HP_READ_INVESTIGATION.md.
    int GetCurrentHp() const;
    int GetMaxHp() const;
    int GetCurrentMana() const;
    int GetMaxMana() const;
    // Session 15 [KILL-SIGNAL RE]: the game's OWN client-side kill counter — a
    // direct int field at +0xA30 (found via the stat-byte dump: reads 0 at
    // login, ticks up 1 per kill, exactly matched the in-game count across 13
    // samples). Reliable, unlike the entity-disappear heuristic in hunt_stats.
    // SEH-guarded; 0 on a bad read.
    int GetGameKillCount() const;
    void RefreshSilverCache(bool trusted = false) const;
    void SetTrustedSilver(uint32_t value) const;
    bool HasTrustedSilverCache() const;
    uint64_t GetSilverRuntimeValue() const { return m_qwRuntimeA30; }
    uint32_t GetSilver() const;
    CMagic* FindMagicByName(const char* name) const;
    CMagic* FindMagicById(OBJID idMagic) const;
    bool IsVip() const;
};
#pragma pack(pop)

// Checked against field_offsets.h (per-build) — see CRole.h's matching comment.
static_assert(offsetof(CHero, m_pStatTable) == FieldOffsets::CHero_StatTable, "CHero::m_pStatTable");
static_assert(offsetof(CHero, m_qwRuntimeA30) == FieldOffsets::CHero_Silver, "CHero::m_qwRuntimeA30");
static_assert(offsetof(CHero, m_nMaxMana) == FieldOffsets::CHero_MaxMana, "CHero::m_nMaxMana");
static_assert(offsetof(CHero, m_bMaxManaValid) == FieldOffsets::CHero_MaxManaValid, "CHero::m_bMaxManaValid");
static_assert(offsetof(CHero, m_bNpcActive) == FieldOffsets::CHero_NpcActive, "CHero::m_bNpcActive");
static_assert(offsetof(CHero, m_pNpcDialog) == FieldOffsets::CHero_NpcDialog, "CHero::m_pNpcDialog");
static_assert(offsetof(CHero, m_deqItem) == FieldOffsets::CHero_DeqItem, "CHero::m_deqItem");
static_assert(offsetof(CHero, m_equipment) == FieldOffsets::CHero_Equipment, "CHero::m_equipment");
static_assert(offsetof(CHero, m_vecMagic) == FieldOffsets::CHero_VecMagic, "CHero::m_vecMagic");
static_assert(offsetof(CHero, m_idActiveNpc) == FieldOffsets::CHero_ActiveNpc, "CHero::m_idActiveNpc");
static_assert(offsetof(CHero, m_bVip) == FieldOffsets::CHero_Vip, "CHero::m_bVip");

#define g_objHero (*CHero::GetSingletonPtr())
