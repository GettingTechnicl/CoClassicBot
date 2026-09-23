#pragma once
// =====================================================================
// field_offsets.h — per-client-build struct field / raw-pointer offsets.
//
// WHY A SEPARATE HEADER: CRole.h/CHero.h lay out their fields as real C++ struct
// members at fixed byte offsets (via explicit BYTE padding arrays), because that is
// how the bot overlays its own types onto the game's live objects. A single struct
// layout is fixed at COMPILE time, so supporting two different real memory layouts
// (v1074 and v1078 shifted several fields, non-uniformly) in ONE binary is not
// possible by branching at runtime — the offsets used here are picked at COMPILE
// time instead, by defining COCLASSIC_TARGET_V1078 on the command line. This is why
// there are two DLL build targets (`coclassic`, `coclassic_v1078` in CMakeLists.txt)
// compiled from the exact same source tree: same code, different constants baked in,
// selected by the launcher/injector from the running client's own PE identity
// (build_fence.h) — never guessed, never mixed.
//
// Leaving COCLASSIC_TARGET_V1078 undefined reproduces the pre-existing v1074 values
// byte-for-byte — the default `coclassic` target's behaviour is unchanged by this
// header's existence.
//
// STATUS TAGS (carried over verbatim from docs/investigation/V1078_OFFSET_FINDINGS.md
// — read that file before trusting or changing any V1078 value below):
//   VALUE-CONFIRMED   the live value at this offset matched a known on-screen number.
//   STRUCTURE-OK      a plausible pointer/small value was read; not confirmed by value.
//   MODEL             only the region shift-model's prediction; not read at all yet.
// A read of a MODEL/STRUCTURE-OK field is still SEH-guarded (same as v1074's own
// long-standing "[inferred]" fields), but must not be trusted as ground truth in a
// decision that matters until it graduates to VALUE-CONFIRMED.
//
// HOW TO ADD/UPDATE A BUILD: only after re-deriving via tools/sigscan.py or
// tools/regime_classify.py AND live-confirming (see the ladder in
// docs/investigation/coclassicbot-data-sourcing-ladder equivalent doc). Record the
// evidence in docs/investigation/, then update the constant + its status comment here.
// =====================================================================
#include <cstddef>

namespace FieldOffsets {

#if defined(COCLASSIC_TARGET_V1078)

// ---- CRole (docs/investigation/V1078_OFFSET_FINDINGS.md) ----
// Unchanged from v1074 — VALUE-CONFIRMED (id, name, posMap all matched character S411).
constexpr size_t CRole_StatusFlag   = 0x30;   // STRUCTURE-OK (never individually re-checked, no reason to suspect it)
constexpr size_t CRole_Id           = 0x68;   // VALUE-CONFIRMED (1174578)
constexpr size_t CRole_Name         = 0x94;   // VALUE-CONFIRMED ("S411")
constexpr size_t CRole_PosMap       = 0xD8;   // VALUE-CONFIRMED (237,191)
constexpr size_t CRole_PosWorld     = 0xE0;   // STRUCTURE-OK
constexpr size_t CRole_PosScr       = 0xE8;   // STRUCTURE-OK
constexpr size_t CRole_PosMoveStart = 0x108;  // STRUCTURE-OK
constexpr size_t CRole_PosMoveDest  = 0x110;  // STRUCTURE-OK
constexpr size_t CRole_CmdAction    = 0x188;  // STRUCTURE-OK
// Shifted +0x10 — VALUE-CONFIRMED (maxHp 51, stamina 100/100, level 1, all matched twice
// across two different HP states: cp02 HP=51 and cp04 HP=33).
constexpr size_t CRole_MaxHp        = 0x3E0;
constexpr size_t CRole_Stamina      = 0x6F0;
constexpr size_t CRole_MaxStamina   = 0x6F4;
constexpr size_t CRole_Level        = 0x6F8;
constexpr size_t CRole_Syndicate     = 0x724; // STRUCTURE-OK (read 0, character has no syndicate — not distinguishing)
constexpr size_t CRole_SyndicateRank = 0x728; // STRUCTURE-OK (same caveat)

// ---- CRoleMgr ----
constexpr size_t CRoleMgr_Hero    = 0x00;     // VALUE-CONFIRMED (*(*ROLE_MGR_PTR) == the hero object found by name)
constexpr size_t CRoleMgr_DeqRole = 0x70;     // STRUCTURE-OK (v1074's own deque-based role list is already known unreliable — heap-scan is used instead, see entities.cpp)

// ---- CHero ----
constexpr size_t CHero_StatTable    = 0x978;  // VALUE-CONFIRMED (HP path: [+0x10]->[+0] read 51, then 33)
constexpr size_t CHero_Silver       = 0xAA8;  // VALUE-CONFIRMED (4800, then 4349)
constexpr size_t CHero_DeqItem      = 0xB98;  // VALUE-CONFIRMED (bag pointer-scan from this base found real items by name/id — see the bag-count cross-check)
constexpr size_t CHero_Equipment    = 0xC00;  // VALUE-CONFIRMED (Coat/LuckyBow/LuckyArrow in the exact equipped slots)
constexpr size_t CHero_MaxMana      = 0xD20;  // VALUE-CONFIRMED (0, correct for an archer with no mana pool)
constexpr size_t CHero_MaxManaValid = 0xD24;  // VALUE-CONFIRMED (1 — cache populated, corroborates the 0 above being real)
// Below this point: MODEL only (shift-model +0x28/+0x48 by region), never read on v1078 at all yet.
constexpr size_t CHero_NpcActive = 0x1078;    // MODEL
constexpr size_t CHero_NpcDialog = 0x1088;    // MODEL
constexpr size_t CHero_VecMagic  = 0x1990;    // MODEL
constexpr size_t CHero_ActiveNpc = 0x37BC;    // MODEL (structurally plausible NPC id 100122 seen nearby, not confirmed as THIS field)
constexpr size_t CHero_Vip       = 0x37D8;    // MODEL

// ---- raw offsets used outside the struct declarations (CHero.cpp) ----
// v1074's is a real "[LIVE-VERIFIED session 15]" field; the v1078 number below is the
// UNTESTED shift-model prediction (+0x10, same region as m_nStamina/m_nLevel) — MODEL.
// GetGameKillCount() must keep failing safe (SEH-guarded, 0 on any bad read) until this
// is live-confirmed the same way the session-15 original was.
constexpr size_t CHero_GameKillCount = 0xA40; // MODEL — UNCONFIRMED, do not trust for decisions

#else  // v1074 — byte-identical to the values hardcoded here before this header existed

constexpr size_t CRole_StatusFlag   = 0x30;
constexpr size_t CRole_Id           = 0x68;
constexpr size_t CRole_Name         = 0x94;
constexpr size_t CRole_PosMap       = 0xD8;
constexpr size_t CRole_PosWorld     = 0xE0;
constexpr size_t CRole_PosScr       = 0xE8;
constexpr size_t CRole_PosMoveStart = 0x108;
constexpr size_t CRole_PosMoveDest  = 0x110;
constexpr size_t CRole_CmdAction    = 0x188;
constexpr size_t CRole_MaxHp        = 0x3D0;
constexpr size_t CRole_Stamina      = 0x6E0;
constexpr size_t CRole_MaxStamina   = 0x6E4;
constexpr size_t CRole_Level        = 0x6E8;
constexpr size_t CRole_Syndicate     = 0x714;
constexpr size_t CRole_SyndicateRank = 0x718;

constexpr size_t CRoleMgr_Hero    = 0x00;
constexpr size_t CRoleMgr_DeqRole = 0x70;

constexpr size_t CHero_StatTable    = 0x968;
constexpr size_t CHero_Silver       = 0xA80;
constexpr size_t CHero_DeqItem      = 0xB70;
constexpr size_t CHero_Equipment    = 0xBD8;
constexpr size_t CHero_MaxMana      = 0xCF8;
constexpr size_t CHero_MaxManaValid = 0xCFC;
constexpr size_t CHero_NpcActive = 0x1050;
constexpr size_t CHero_NpcDialog = 0x1060;
constexpr size_t CHero_VecMagic  = 0x1968;
constexpr size_t CHero_ActiveNpc = 0x3774;
constexpr size_t CHero_Vip       = 0x3790;

constexpr size_t CHero_GameKillCount = 0xA30;

#endif

}  // namespace FieldOffsets
