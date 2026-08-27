#pragma once
#include "CMapObj.h"

// =====================================================================
// User status flags
// =====================================================================
const __int64 USERSTATUS_NORMAL       = 0ll;
const __int64 USERSTATUS_CRIME        = 1ll << 0;
const __int64 USERSTATUS_POISON       = 1ll << 1;
const __int64 USERSTATUS_INVISIBLE    = 1ll << 2;
const __int64 USERSTATUS_DIE          = 1ll << 3;
const __int64 USERSTATUS_XPFULL       = 1ll << 4;
const __int64 USERSTATUS_DEAD         = 1ll << 5;
const __int64 USERSTATUS_TEAMLEADER   = 1ll << 6;
const __int64 USERSTATUS_ATKSPEED     = 1ll << 7;
const __int64 USERSTATUS_SHIELD       = 1ll << 8;
const __int64 USERSTATUS_ATKPOWER     = 1ll << 9;
const __int64 USERSTATUS_GHOST        = 1ll << 10;
const __int64 USERSTATUS_DISAPPEARING = 1ll << 11;
const __int64 USERSTATUS_MAGICDEF     = 1ll << 12;
const __int64 USERSTATUS_BOWDEF       = 1ll << 13;
const __int64 USERSTATUS_RED          = 1ll << 14;
const __int64 USERSTATUS_BLACK        = 1ll << 15;
const __int64 USERSTATUS_ATKRANGE     = 1ll << 16;
const __int64 USERSTATUS_REFLECT      = 1ll << 17;
const __int64 USERSTATUS_SUPERMAN     = 1ll << 18;
const __int64 USERSTATUS_BODYSHIELD   = 1ll << 19;
const __int64 USERSTATUS_MAGICDMG     = 1ll << 20;
const __int64 USERSTATUS_ATKSPEEDEX   = 1ll << 21;
const __int64 USERSTATUS_INVISIBILTY  = 1ll << 22;
const __int64 USERSTATUS_CYCLONE      = 1ll << 23;
const __int64 USERSTATUS_SYNCRIME     = 1ll << 24;
const __int64 USERSTATUS_REFLECTMAGIC = 1ll << 25;
const __int64 USERSTATUS_DODGE        = 1ll << 26;
const __int64 USERSTATUS_FLY          = 1ll << 27;
const __int64 USERSTATUS_CHARGEUP     = 1ll << 28;
const __int64 USERSTATUS_FROZEN       = 1ll << 29;
const __int64 USERSTATUS_PRAY         = 1ll << 30;
const __int64 USERSTATUS_FOLLOWPRAY   = 1ll << 31;
const __int64 USERSTATUS_CURSE        = 1ll << 32;
const __int64 USERSTATUS_BLESS        = 1ll << 33;

// =====================================================================
// Command types
// =====================================================================
const int _COMMAND_NULL      = 0;
const int _COMMAND_PICKUP    = 1;
const int _COMMAND_STOP      = 1;
const int _COMMAND_STANDBY   = 2;
const int _COMMAND_WALK      = 3;
const int _COMMAND_RUN       = 4;
const int _COMMAND_EMOTION   = 6;
const int _COMMAND_ACTION    = 7;
const int _COMMAND_FOLLOW    = 9;
const int _COMMAND_SHITHAPPEN = 10;
const int _COMMAND_DIE       = 11;
const int _COMMAND_SPATTACK  = 12;
const int _COMMAND_FAINT     = 13;
const int _COMMAND_WOUND     = 15;
const int _COMMAND_JUMP      = 16;
const int _COMMAND_DEFEND    = 16;
const int _COMMAND_WALKFORWARD = 17;
const int _COMMAND_RUNFORWARD  = 18;
const int _COMMAND_ATTACK    = 20;
const int _COMMAND_SHOOT     = 21;
const int _COMMAND_RUSHATK   = 23;
const int _COMMAND_INTONE    = 23;
const int _COMMAND_LOCKATK   = 24;
const int _COMMAND_MINE      = 25;
const int _COMMAND_DASH      = 25;

// =====================================================================
// Command status
// =====================================================================
const int _CMDSTATUS_BEGIN      = 0;
const int _CMDSTATUS_DEPART     = 1;
const int _CMDSTATUS_PROGRESS   = 2;
const int _CMDSTATUS_CONTINUE   = 3;
const int _CMDSTATUS_WAITING    = 4;
const int _CMDSTATUS_ACCOMPLISH = 6;

// =====================================================================
// CCommand — action command
// =====================================================================
// Session 9 [FIXED]: this used to declare an explicit `CCommand() {}`
// constructor. In C++, ANY user-provided constructor (even an empty one)
// makes a class a non-aggregate — which means `CCommand cmd = {};` (used at
// every call site in this codebase) stops zero-initializing the object and
// instead just calls the (do-nothing) constructor, leaving every field NOT
// explicitly assigned afterward as uninitialized stack garbage. The real
// native SetCommand() bulk-copies the ENTIRE struct (confirmed >=0x80 bytes,
// covering fields like idTarget/nFrameStart/nFrameEnd/dwIndex that call
// sites never set) into the target object, and the game's own per-tick
// command dispatcher reads several of those exact fields — garbage there is
// what was crashing the game on the first real SetCommand call this
// session, not the RVA/vtable-slot bug found earlier. Removing the
// constructor makes CCommand an aggregate again, restoring correct
// zero-initialization for `= {}`/`{}` everywhere.
class CCommand {
public:
    int iType;
    int iStatus;

    union {
        char szCmd[256];

        struct {
            OBJID  idTarget;
            Position posTarget;
            int    nDir;
            int    nArg;
            DWORD  dwData;
            int    nUnknown;
            BOOL   bData;
            int    nData;
            BOOL   bHeroSendMsg;
            int    nFrameStart;
            int    nFrameEnd;
            DWORD  dwTimestamp;
            int    nFrameInterval;
            BOOL   bDiagnal;
            BOOL   bAddUp;
            DWORD  dwIndex;
        };
    };
};

// =====================================================================
// CRole — base entity class
//
// Hierarchy: CMapObj -> CRole -> CHero
//
// All offsets verified via Ghidra + Cheat Engine on the 64-bit client.
// =====================================================================
#pragma pack(push, 1)
// =====================================================================
// Monster name-color tier ("con color") — see CRole::GetDangerTier
// =====================================================================
enum class MonsterDangerTier { Green, White, Red, Black };

class CRole : public CMapObj
{
public:
    virtual ~CRole() {}

private:
    BYTE _pad20[0x10];            // +0x20

public:
    __int64 m_nStatusFlag;         // +0x30

private:
    BYTE _pad38[0x30];            // +0x38  (deque<PAniEffect> + unknowns)

public:
    // ── CRoleInfo fields ──
    OBJID m_id;                    // +0x68  entity ID
private:
    BYTE _pad6C[0x28];            // +0x6C
public:
    char m_szName[16];             // +0x94  null-terminated ASCII
private:
    BYTE _padA4[0x34];            // +0xA4
public:
    Position m_posMap;               // +0xD8  map cell position
    Position m_posWorld;             // +0xE0  world position
    Position m_posScr;               // +0xE8  screen position

private:
    BYTE _padF0[0x108 - 0xF0];     // +0xF0
public:
    // ── Movement interpolation state (v1074, LIVE-VERIFIED session 9) ──
    // The client interpolates m_posWorld from m_posMoveStart toward
    // m_posMoveDest while a move is in progress, then SNAPS m_posWorld to
    // m_posMoveDest on completion. m_posMap is in turn DERIVED from
    // m_posWorld by isometric conversion (func 0x1A8140).
    //
    // Consequence: writing m_posMap or m_posWorld alone does NOT stick --
    // it is recomputed and reverted within a frame. To reposition the
    // client all three must be written together; see SyncClientPosition().
    // Found by write-only hardware breakpoint on m_posWorld, which gave
    // exactly two writers:
    //   0x1B0FC4  mov qword [rdi+0xE0], rax   ; rax = qword [rdi+0x108]
    //   0x1B1228  mov qword [rdi+0xE0], rax   ; rax = qword [rdi+0x110]
    Position m_posMoveStart;        // +0x108  movement start world position
    Position m_posMoveDest;         // +0x110  movement destination world position
private:
    BYTE _pad118[0x188 - 0x118];   // +0x118
public:
    CCommand m_cmdAction;           // +0x188  current action command
private:
    BYTE _pad290[0x3D0 - 0x290];    // +0x290
public:
    int   m_nMaxHp;                // +0x3D0 cached max HP
private:
    BYTE _pad3D4[0x6E0 - 0x3D4];  // +0x3D4
public:
    int   m_nStamina;              // +0x6E0 current stamina (PP)
    int   m_nMaxStamina;           // +0x6E4 max stamina
    // Session 13: found via a full-range (+0x20..+0x71C) live memory scan
    // correlated against user-confirmed green/white/red/black monster-name
    // tiers -- zero variance across hundreds of samples per monster type,
    // and exactly matches every "L###" name-suffix variant's own number
    // (RedDevilL117 -> 117 here, RedDevilL118 -> 118, TombBatL103 -> 103,
    // BloodyBatL108 -> 108). Confirmed tier ranges: Green~102, White~107-108,
    // Red~112-113, Black~117-118 -- monotonically increasing with danger,
    // consistent with the user's own description of name color as a
    // level-relative "con color" computed against the hero's own level.
    // CHero publicly inherits CRole and doesn't touch this offset (its own
    // fields start at +0x71C), so this also reads the HERO's own level for
    // free -- unblocks the discordOnLevelUp TODO in hunt_stats.h.
    int32_t m_nLevel;              // +0x6E8
private:
    BYTE _pad6EC[0x714 - 0x6EC];  // +0x6EC
public:
    OBJID m_idSyndicate;           // +0x714  syndicate/guild ID (0 = none)
    int   m_nSyndicateRank;        // +0x718  syndicate rank (100=Leader, 90=Deputy, 50=Member)

    // ── Helpers ──
    OBJID GetID() const { return m_id; }
    const char* GetName() const { return m_szName; }
    BOOL TestState(__int64 nState) const { return (m_nStatusFlag & nState) != 0; }

    BOOL IsDead() const { return (m_nStatusFlag & USERSTATUS_DEAD) != 0; }
    BOOL IsPlayer() const { return m_id >= 1000000; }
    BOOL IsMonster() const { return m_id >= 400000 && m_id < 500000 && !IsGuard() && !IsPatrol(); }
    BOOL HasXpSkillReady() const { return TestState(USERSTATUS_XPFULL); }
    BOOL IsCycloneActive() const { return TestState(USERSTATUS_CYCLONE); }
    BOOL IsFlyActive() const { return TestState(USERSTATUS_FLY); }
    BOOL IsStigmaActive() const { return TestState(USERSTATUS_ATKPOWER); }
    BOOL IsSupermanActive() const { return TestState(USERSTATUS_SUPERMAN); }
    BOOL IsMagicShieldActive() const { return TestState(USERSTATUS_SHIELD); }
    // Session 12: PK-flag name-color bits — a player at 30+ PK points shows
    // a red name (chance to drop gear on death), 100+ shows black (much
    // higher chance). USERSTATUS_RED/USERSTATUS_BLACK were defined but never
    // read anywhere in this codebase before now, and never verified live —
    // added specifically to cross-check against a known-redname/blackname
    // player's actual in-game name color via the overlay's Entities table.
    BOOL IsRedName() const { return TestState(USERSTATUS_RED); }
    BOOL IsBlackName() const { return TestState(USERSTATUS_BLACK); }
    int GetStamina() const { return m_nStamina; }
    int GetMaxStamina() const { return m_nMaxStamina; }
    int GetLevel() const { return m_nLevel; }

    // Session 13: calibrated live against the user's own observed name
    // colors, hero fixed at level 110 throughout (con color is relative
    // to hero level, so this delta -- not the raw monster level -- is
    // what matters). Confirmed anchor points (delta = monster level -
    // hero level): -8/-7 -> Green, -3/-2 -> White, +2/+3 -> Red, +7 ->
    // Black. No monster has been confirmed exactly at a tier boundary
    // yet, so the cutoffs below are the midpoint of each gap (a first-pass
    // estimate, not a confirmed exact threshold) -- refine if a monster
    // near delta -5, -1, or +5 ever gets checked.
    MonsterDangerTier GetDangerTier(int heroLevel) const {
        const int delta = m_nLevel - heroLevel;
        if (delta <= -5) return MonsterDangerTier::Green;
        if (delta <= 0)  return MonsterDangerTier::White;
        if (delta <= 5)  return MonsterDangerTier::Red;
        return MonsterDangerTier::Black;
    }
    BOOL IsMining() const { return m_cmdAction.iType == _COMMAND_MINE; }
    BOOL IsJumping() const {
        return m_cmdAction.iType == _COMMAND_JUMP
            && (m_cmdAction.posTarget.x != m_posMap.x || m_cmdAction.posTarget.y != m_posMap.y);
    }
    // Session 12: strncmp rather than strstr(...) == m_szName — this only
    // ever needs a PREFIX check, and strncmp is bounded by the literal's own
    // length (5/6 bytes here), so it can never read past m_szName's declared
    // 16-byte buffer even if the game ever left it non-null-terminated.
    // strstr would keep scanning for a null terminator with no such bound.
    BOOL IsGuard() const {
        return m_id >= 400000 && m_id < 500000
            && strncmp(m_szName, "Guard", 5) == 0;
    }
    BOOL IsPatrol() const {
        return m_id >= 400000 && m_id < 500000
            && strncmp(m_szName, "Patrol", 6) == 0;
    }
    BOOL HasSyndicate() const { return m_idSyndicate != 0; }

    void GetPos(Position& pos) const { pos = m_posMap; }
    const CCommand& GetCommand() const { return m_cmdAction; }

    void SetCommand(CCommand* cmd);

    // Reposition the CLIENT's view of this role to a map tile, keeping the
    // game's own movement bookkeeping self-consistent so the write sticks.
    // Pure data writes -- no native calls, no CCommand, nothing gated.
    //
    // This does NOT tell the server anything; send the movement packet
    // separately (the server accepts packet-only moves -- verified by picking
    // up an item several blocks away after a packet-only move).
    void SyncClientPosition(int mapX, int mapY);
};
#pragma pack(pop)

static_assert(offsetof(CRole, m_nStatusFlag) == 0x30, "CRole::m_nStatusFlag");
static_assert(offsetof(CRole, m_id)          == 0x68, "CRole::m_id");
static_assert(offsetof(CRole, m_szName)      == 0x94, "CRole::m_szName");
static_assert(offsetof(CRole, m_posMap)      == 0xD8, "CRole::m_posMap");
static_assert(offsetof(CRole, m_posWorld)    == 0xE0, "CRole::m_posWorld");
static_assert(offsetof(CRole, m_posScr)      == 0xE8, "CRole::m_posScr");
static_assert(offsetof(CRole, m_posMoveStart) == 0x108, "CRole::m_posMoveStart");
static_assert(offsetof(CRole, m_posMoveDest)  == 0x110, "CRole::m_posMoveDest");
static_assert(offsetof(CRole, m_cmdAction)   == 0x188, "CRole::m_cmdAction");
static_assert(offsetof(CRole, m_nMaxHp)      == 0x3D0, "CRole::m_nMaxHp");
static_assert(offsetof(CRole, m_nStamina)     == 0x6E0, "CRole::m_nStamina");
static_assert(offsetof(CRole, m_nMaxStamina)  == 0x6E4, "CRole::m_nMaxStamina");
static_assert(offsetof(CRole, m_nLevel)       == 0x6E8, "CRole::m_nLevel");
static_assert(offsetof(CRole, m_idSyndicate)  == 0x714, "CRole::m_idSyndicate");
static_assert(offsetof(CRole, m_nSyndicateRank) == 0x718, "CRole::m_nSyndicateRank");

using PRole = Ref<CRole>;

class CHero; // forward declaration

// =====================================================================
// CRoleMgr — holds hero reference + entity deque
//
// Located at base + 0x4DF588.
// Game uses same MSVC 2022 17.14 as us, so std::deque ABI matches.
// =====================================================================
#pragma pack(push, 1)
class CRoleMgr
{
public:
    CHero*                m_pHero;       // 0x00
private:
    BYTE                  _pad08[0x68];  // 0x08
public:
    std::deque<PRole>     m_deqRole;     // 0x70
};
#pragma pack(pop)

static_assert(offsetof(CRoleMgr, m_pHero)   == 0x00, "CRoleMgr::m_pHero");
static_assert(offsetof(CRoleMgr, m_deqRole) == 0x70, "CRoleMgr::m_deqRole");
