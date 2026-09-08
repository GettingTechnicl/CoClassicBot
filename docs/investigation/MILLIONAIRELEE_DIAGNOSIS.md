# Why MillionaireLee packing fails (and Treasure/Compose Bank too) — one root cause

Read-only analysis of `src/hunt_town.cpp` + `src/CHero.h` + the process logs, 2026-09-02.
Nothing modified.

## The packets are NOT the problem. The bot never sends them.

The Frida test works because it sends the answers **blindly** right after ActivateNpc:
`ActivateNpc(id)` → sleep 1.2s → `AnswerNpcEx(0,101)` → sleep 1.2s → `AnswerNpcEx(0,101)`.
No state check. The bytes are correct and the server accepts them. Proven.

The bot gates the answer behind a dialog-open check and, unlike the working NPCs, has
**no fallback** — so when the check is false it sends the ActivateNpc again and again and
never sends a single AnswerNpc. On the wire the server sees: Activate, Activate, Activate,
give up. It never receives the answers at all. Same functions, completely different byte
stream.

`WaitPackMeteors` (hunt_town.cpp:897):
```cpp
const bool dialogOpen = hero->IsNpcActive() && hero->GetActiveNpc() == m_millionaireLeeNpcId;
if (!dialogOpen) {
    if (now - m_lastNpcActionTick > 1200) {
        if (++m_millionaireLeeOpenAttempts >= 3) { ...give up -> MoveToWarehouse; }
        hero->ActivateNpc(m_millionaireLeeNpcId);   // re-activates, never answers
        m_lastNpcActionTick = now;
    }
    return;                                          // <-- answer is unreachable
}
// only past here does hero->AnswerNpc(0) run
```

So **everything hinges on `dialogOpen` (i.e. `IsNpcActive()` / `GetActiveNpc()`) being true.**

## Why it's never true: the two fields are unverified guesses

`src/CHero.h`:
```
BOOL  m_bNpcActive;   // +0x1050 (v1074) active NPC dialog flag [inferred]
OBJID m_idActiveNpc;  // +0x3774 (v1074) NPC entity currently in dialog [inferred]
```
Both are tagged **`[inferred]`** — never live-verified. `IsNpcActive()`/`GetActiveNpc()`
return these raw. If either offset is wrong (or only valid for a different dialog type),
`dialogOpen` is false forever and the answer is never sent.

## The corroboration: which town flows work vs. fail lines up EXACTLY with this

| Flow | Gate on dialog-open? | Fallback if it stays false? | Works? |
|---|---|---|---|
| Warehouse deposit (hunt_town.cpp:1000) | yes | **yes** — `\|\| now-last>1200` proceeds anyway | **WORKS** (logs: `DepositWarehouseSilver … OK`) |
| Mining warehouse (mining_plugin.cpp:1397) | yes | **yes** — same `\|\|` timeout | works |
| **MillionaireLee** (hunt_town.cpp:901) | yes | **no** — only re-activates, never answers | **FAILS** |
| **Treasure Bank** (hunt_town.cpp:1132) | yes | **no** — requires confirm to deposit | **"not confirmed working"** (README) |
| **Compose Bank** (hunt_town.cpp:1239) | yes | **no** — requires confirm | **"not confirmed working"** (README) |

Every flow that **trusts** `IsNpcActive()` is broken. Every flow that **bypasses it on a
timeout** works. That's not three separate bugs — it's one wrong offset, surfacing three times.
The Warehouse only "works" because it never actually needed the flag.

## The fix is to verify/correct the offsets — NOT to add a timeout to Lee

Tempting band-aid: give Lee the same `|| timeout` bypass as the Warehouse so it sends the
answer regardless. **Don't.** The code's own history (hunt_town.cpp:1123-1131, "Session 11
FREEZE FIX") records that depositing against an **unconfirmed** NPC window is exactly what
hard-froze the client (Task-Manager kill). Blindly firing the pack-confirm on a timeout
re-introduces that risk. The correct fix makes the confirmation actually work, which also
un-breaks Treasure Bank and Compose Bank for free.

### Read-only way to pin the real offsets (uses the existing toolkit, no new live risk)
1. Stand at MillionaireLee and **manually** open its dialog (or any NPC — Warehouse works too).
2. With `explorer.dll` (already built, read-only), dump the hero region and search for:
   - the **NPC's entity id** (the value the bot would compare against) → that address minus the
     hero base is the true `m_idActiveNpc`. Current guess +0x3774.
   - a byte/dword that reads **1 while a dialog is open and 0 when closed** (snapshot open vs
     closed and diff) → true `m_bNpcActive`. Current guess +0x1050.
   This is the same differential-snapshot method that pinned silver/exp/map/durability.
3. Update the two offsets + static_asserts, rebuild. Re-test Lee through its *existing*
   confirmed-open logic (no timeout hack). Treasure/Compose Bank should also start confirming.

### Two secondary checks while you're there
- `m_millionaireLeeNpcId` comes from `FindNpcByName("MillionaireLee", {241,240}, 16)`
  (hunt_town.cpp:868). The Frida test uses a **hand-supplied** npcId, so it bypasses this
  lookup entirely. Confirm FindNpcByName returns the same id Frida was given (exact name match,
  within 16 tiles of {241,240}). If it returns 0 or a neighbor, that's an independent failure
  on top of the gate.
- The logs captured so far contain **no** `[hunt] MillionaireLee: … activating` line from the
  current (20:57) DLL — only older sessions with `packMeteorsIntoScrolls=false`. Before more
  code changes, grab one clean capture with the current build, 10+ meteors, `autoStore=1` +
  `packMeteorsIntoScrolls=1` (Kinux's ini already has both), and read the
  `IsNpcActive={} activeNpc={}` values the retry line already logs (hunt_town.cpp:913). Those two
  numbers, at the moment the dialog is visibly open on screen, prove or disprove the offset in
  one line: if `IsNpcActive=false` or `activeNpc` != the real id while the window is open, the
  offset is confirmed wrong.

## One-line summary for the handoff
The bot isn't sending the pack answers at all — it's blocked on `IsNpcActive()`/`GetActiveNpc()`
(`CHero +0x1050`/`+0x3774`, both `[inferred]`/unverified), which never report the dialog open.
Frida works because it skips that check. Verify those two offsets with the differential dump;
that single fix also un-breaks Treasure Bank and Compose Bank. Do not band-aid with a
blind-timeout send — that's the documented client-freeze path.
