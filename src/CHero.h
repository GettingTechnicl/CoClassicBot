#pragma once
#include "CRole.h"
#include "CItem.h"
#include "CMagic.h"
#include "CStatTable.h"
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
bool DebugTestNativePickup(const CMapItem& item);

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
    BYTE _pad71C[0x968 - 0x71C]; // +0x71C  hero runtime fields

public:
    CStatTable* m_pStatTable;      // +0x968 current HP/stat table

private:
    // v1074: a ~0x50-byte field was inserted between +0x968 and the old +0xA30,
    // shifting every field below by +0x50. Live-verified against character "Kinux"
    // (silver, equipped GoldCoronet/Lathee, 18 skills). See coclassicbot-live-offsets.
    BYTE _pad970[0xA80 - 0x970];   // +0x970 gap to silver slot (v1074 +0x50)

public:
    uint64_t m_qwRuntimeA30;       // +0xA80 (v1074) silver in low 32 bits [LIVE-VERIFIED =7681]

private:
    BYTE _padA88[0xB70 - 0xA88];   // +0xA88 gap to inventory deque

public:
    std::deque<PItem> m_deqItem;   // +0xB70 (v1074) inventory items [inferred from equip offset]

private:
    BYTE _padDeq[0xBD8 - 0xB70 - sizeof(std::deque<PItem>)]; // gap between deque end and equipment

public:
    PItem m_equipment[EquipSlot::COUNT]; // +0xBD8 (v1074) equipped items [LIVE-VERIFIED: GoldCoronet/Lathee]

private:
    BYTE _padC58[0xCF8 - 0xC58];   // +0xC58 gap to max mana cache

public:
    int32_t m_nMaxMana;            // +0xCF8 (v1074) cached max MP [inferred]
    uint8_t m_bMaxManaValid;       // +0xCFC max MP cache-valid byte

private:
    BYTE _padCFD[0x1050 - 0xCFD];  // +0xCFD

public:
    // +0x1050 [LIVE-VERIFIED 2026-09-02]: 0 -> 1 when a dialog opens (the only
    // 0/1 flip in a closed-vs-open hero dump) — but STICKY: it stays 1 after
    // the dialog is closed, and a bot-sent ActivateNpc packet on a session
    // that never had a manual click leaves it 0 even though the dialog
    // renders. So it answers "has a dialog ever been opened by a click", not
    // "is one open now". Do not gate on it alone. See m_pNpcDialog.
    BOOL m_bNpcActive;             // +0x1050 (v1074)

private:
    BYTE _pad1054[0x1060 - 0x1054];

public:
    // +0x1060 [LIVE-VERIFIED 2026-09-02, npctest v3]: pointer to the current
    // NPC dialog object. Sticky across a close too — but it is REPLACED with a
    // fresh object every time a dialog is created, including dialogs opened by
    // the bot's own ActivateNpc packet (observed 0AAC01A0 -> 08F45D50 within
    // 300 ms of the send). That change is the only client-side signal found
    // that says "a dialog appeared just now" for a packet-opened dialog, which
    // is what the confirm-open gates in hunt_town.cpp need. Compare against
    // the value captured immediately before sending the activate.
    uintptr_t m_pNpcDialog;        // +0x1060 (v1074)

private:
    BYTE _pad1068[0x1968 - 0x1068]; // +0x1068 gap to magic vector

public:
    std::vector<PMagic> m_vecMagic; // +0x1968 (v1074) learned skills [LIVE-VERIFIED: count 18]

private:
    BYTE _pad1980[0x3774 - 0x1980]; // +0x1980 gap to active NPC UID

public:
    // +0x3774 [LIVE-VERIFIED 2026-09-02]: id of the NPC the client's own click
    // path last opened (101545 -> 101395 when MillionaireLee was clicked).
    // Same caveat as m_bNpcActive: set by the click path, sticky, not updated
    // by a bot-sent activate on a cold session.
    OBJID m_idActiveNpc;            // +0x3774 (v1074)

private:
    BYTE _pad3778[0x3790 - 0x3778]; // +0x3778 gap to VIP flag

public:
    BOOL m_bVip;                    // +0x3790 (v1074) VIP status flag [inferred]

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

static_assert(offsetof(CHero, m_pStatTable) == 0x968, "CHero::m_pStatTable");
static_assert(offsetof(CHero, m_qwRuntimeA30) == 0xA80, "CHero::m_qwRuntimeA30");   // v1074 +0x50
static_assert(offsetof(CHero, m_nMaxMana) == 0xCF8, "CHero::m_nMaxMana");           // v1074 +0x50
static_assert(offsetof(CHero, m_bMaxManaValid) == 0xCFC, "CHero::m_bMaxManaValid"); // v1074 +0x50
static_assert(offsetof(CHero, m_bNpcActive) == 0x1050, "CHero::m_bNpcActive");      // v1074 +0x50
static_assert(offsetof(CHero, m_pNpcDialog) == 0x1060, "CHero::m_pNpcDialog");      // live-verified 2026-09-02
static_assert(offsetof(CHero, m_deqItem) == 0xB70, "CHero::m_deqItem");             // v1074 +0x50
static_assert(offsetof(CHero, m_equipment) == 0xBD8, "CHero::m_equipment");         // v1074 +0x50
static_assert(offsetof(CHero, m_vecMagic) == 0x1968, "CHero::m_vecMagic");          // v1074 +0x50
static_assert(offsetof(CHero, m_idActiveNpc) == 0x3774, "CHero::m_idActiveNpc");    // v1074 +0x50
static_assert(offsetof(CHero, m_bVip) == 0x3790, "CHero::m_bVip");                  // v1074 +0x50

#define g_objHero (*CHero::GetSingletonPtr())
