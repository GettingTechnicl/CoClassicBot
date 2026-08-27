#pragma once

// =====================================================================
// monster_scan.h — one-shot live dump of nearby monsters' unmapped CRole
// memory, for finding whatever field drives green/white/red/black name
// color (a level-relative "con color" per the user's explanation, not a
// status-flag bit -- USERSTATUS_RED/USERSTATUS_BLACK were live-checked
// and ruled out for monsters, session 12).
//
// Dumps every currently-visible monster's name/ID/position plus a raw u32
// dump of the largest unmapped CRole padding gaps (the ranges bracketing
// the already-mapped m_nMaxHp/m_nStamina fields, where a level/tier field
// would plausibly sit alongside other character stats) to a JSON-lines
// file, appending one line per press so multiple captures across a play
// session accumulate in one place. Read-only; SEH-guarded per read since
// this walks raw, unverified memory offsets.
// =====================================================================

// Scans Entities::Get() for monsters, dumps each to
// C:\Users\Public\coclassic_monsterscan.json (appended). Returns the
// number of monsters written.
int DumpNearbyMonsterStats();
