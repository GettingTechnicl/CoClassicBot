// =====================================================================
// tracer.cpp — standalone "find what accesses X" tracer (v1074 action RE).
//
// Injects read-only w.r.t. game code (it does NOT patch code). It uses a
// per-thread HARDWARE breakpoint (debug registers) + a vectored exception
// handler to record which instruction addresses access a target address.
// Two modes, chosen by which config file exists:
//   - Hero-relative (original): C:\Users\Public\tracer_cfg.txt containing a
//     single hex hero offset, e.g. "D8" (position). Retargets live if the
//     file's contents change while the DLL is running.
//   - Absolute address (session 7): C:\Users\Public\tracer_target_abs.txt
//     containing a single hex ABSOLUTE address, e.g. "15E718" — used to
//     watch a fixed, non-hero-relative object (like the real CNetClient
//     "this" this project has been chasing all session). Takes priority
//     over the hero-relative file if both exist.
// Watches for BOTH reads and writes (DR7 RW=11), not just writes, since for
// the absolute-address use case we want to see every access, not just the
// one that constructs/stores the value.
//
// Output: C:\Users\Public\tracer_log.json  (unique RIPs + RVAs + hit counts).
//
// NOTE: hardware breakpoints ARE a debugger technique. Used here with
// explicit user consent. IMPORTANT: only re-arms (which briefly suspends all
// game threads) when the target actually changes, never on a fixed timer —
// a session-1 version that re-armed every second caused a missed server
// heartbeat; this was fixed and (per user confirmation, session 6) an
// earlier suspected disconnect was actually an unrelated network outage,
// not this tool or anti-cheat.
// =====================================================================
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

static uint64_t g_base = 0;
static uintptr_t g_target = 0;         // absolute address being watched (breakpoint TRIGGER)
static uint32_t  g_len = 8;            // 1/2/4/8 — 8 since we're usually watching a pointer-sized field
static bool      g_writeOnly = false;  // session 8: write-only watch (see Dr7ForWatch)
static CRITICAL_SECTION g_cs;

// Session 10 cont. [BUG FIX]: snapshot READ address, independent of g_target
// (the trigger address). Retargeting the trigger to a LATER field (so
// everything before it is guaranteed already written — see the big comment
// on SnapshotTarget) previously also moved the snapshot window, silently
// losing every EARLIER field instead of gaining consistency. Trigger late,
// but always read from a fixed struct-start anchor. Defaults to g_target
// (old single-address behavior) when C:\Users\Public\tracer_snapshot_base.txt
// doesn't exist, so other uses of this tool are unaffected.
static uintptr_t g_snapshotBase = 0;

// Plain per-address hit-frequency counters (unchanged from before) — good
// for "which functions touch this field at all", separate from the
// content-change log below which is what actually shows different command
// TYPES' real field values.
struct Hit { uintptr_t rip; uint32_t count; };
static const int MAX_HITS = 512;
static Hit g_hits[MAX_HITS];
static int g_hitCount = 0;

// Session 10: snapshot size covers CCommand's iType/iStatus (8 bytes) plus
// the alternate named-field view (idTarget/posTarget/nDir/.../dwIndex, ~68
// bytes per CRole.h) — NOT the full struct (the union's other member is a
// 256-byte scratch buffer with no per-field meaning).
static const int SNAPSHOT_SIZE = 96;

// Session 10 cont.: logging one snapshot per unique RIP (the original
// design) silently loses data the moment two DIFFERENT command types (e.g.
// walk vs attack vs pickup) happen to write through the SAME call site —
// which is likely, since they'd all go through the same SetCommand()
// function. Instead, log a new chronological entry whenever the watched
// bytes actually CHANGE from what was last recorded, regardless of which RIP
// did it — this is what actually lets a mixed walk/attack/pickup session
// show each action's real field values separately. This project has already
// been burned once by guessing CCommand field values (see CRole.h's own
// CCommand comment on an earlier garbage-field crash), so real captured
// values beat guessing again.
struct ChangeEntry { uintptr_t rip; DWORD tick; uint8_t snapshot[SNAPSHOT_SIZE]; };
static const int MAX_CHANGES = 256;
static ChangeEntry g_changes[MAX_CHANGES];
static int g_changeCount = 0;
static uint8_t g_lastSnapshot[SNAPSHOT_SIZE];
static bool g_haveLastSnapshot = false;

// Session 10 cont.: this session found CCommand.iType==7 fires CONSTANTLY —
// every 50-1000ms, confirmed present even with the bot's autohunt fully
// disabled, so it's some native idle/background behavior, not player or bot
// activity. At that rate it fills the entire MAX_CHANGES buffer well before
// a rare one-off action (like a single manual walk) ever gets a turn to
// register. Config-driven (not hardcoded) so other noisy values found later
// can be filtered the same way without a rebuild — first 4 bytes of the
// snapshot (typically CCommand.iType) are compared against the hex value in
// C:\Users\Public\tracer_skip_value.txt, if present.
static bool g_haveSkipValue = false;
static uint32_t g_skipValue = 0;

template <class T> static bool TryRead(uintptr_t a, T* out)
{ __try { *out = *reinterpret_cast<volatile T*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static uint64_t RdU64(uintptr_t a){ uint64_t v=0; TryRead(a,&v); return v; }
static uint32_t RdU32(uintptr_t a){ uint32_t v=0; TryRead(a,&v); return v; }

static void SnapshotTarget(uint8_t* out)
{
    __try {
        for (int i = 0; i < SNAPSHOT_SIZE; ++i)
            out[i] = *reinterpret_cast<volatile uint8_t*>(g_snapshotBase + i);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Leave whatever was captured before the fault; partial is still useful.
    }
}

static void RecordRip(uintptr_t rip)
{
    EnterCriticalSection(&g_cs);

    bool found = false;
    for (int i = 0; i < g_hitCount; ++i)
        if (g_hits[i].rip == rip) { g_hits[i].count++; found = true; break; }
    if (!found && g_hitCount < MAX_HITS) {
        g_hits[g_hitCount].rip = rip;
        g_hits[g_hitCount].count = 1;
        g_hitCount++;
    }

    // RIP here is "the instruction after the access" (see VehHandler), so
    // the write has already landed — snapshotting now shows the real
    // post-write value, not a guess.
    uint8_t snap[SNAPSHOT_SIZE];
    SnapshotTarget(snap);
    if (!g_haveLastSnapshot || memcmp(snap, g_lastSnapshot, SNAPSHOT_SIZE) != 0) {
        uint32_t firstU32 = 0;
        memcpy(&firstU32, snap, sizeof(firstU32));
        const bool skip = g_haveSkipValue && firstU32 == g_skipValue;
        if (!skip && g_changeCount < MAX_CHANGES) {
            g_changes[g_changeCount].rip = rip;
            g_changes[g_changeCount].tick = GetTickCount();
            memcpy(g_changes[g_changeCount].snapshot, snap, SNAPSHOT_SIZE);
            g_changeCount++;
        }
        // Still update the baseline even when skipped, so a later GENUINE
        // change (walk showing up after N skipped idle-noise writes) still
        // compares against the true last value, not a stale pre-noise one.
        memcpy(g_lastSnapshot, snap, SNAPSHOT_SIZE);
        g_haveLastSnapshot = true;
    }

    LeaveCriticalSection(&g_cs);
}

static LONG CALLBACK VehHandler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        DWORD64 dr6 = ep->ContextRecord->Dr6;
        if (dr6 & 0xF) {                                   // one of DR0..DR3 fired
            RecordRip((uintptr_t)ep->ContextRecord->Rip);  // instruction after the access
            ep->ContextRecord->Dr6 = 0;                    // clear status
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// DR7 encoding for a single DR0 breakpoint: L0 + RW0 + LEN0.
// RW0 = 11 -> break on data read OR write; RW0 = 01 -> write-only (much
// quieter for a target address that also gets hit by unrelated coincidental
// READS, e.g. the stack-aliasing noise found watching netClient itself in
// session 7 — a write-only watch on a specific field should skip that noise
// since it only fires on actual writes, not reads of overlapping stack slots).
static DWORD64 Dr7ForWatch(uint32_t len, bool writeOnly)
{
    DWORD64 dr7 = 0;
    dr7 |= (1ull << 0);                        // L0 (local enable DR0)
    dr7 |= ((writeOnly ? 0b01ull : 0b11ull) << 16); // RW0
    DWORD64 lenbits = (len==1)?0b00 : (len==2)?0b01 : (len==8)?0b10 : 0b11; // 4 -> 11
    dr7 |= (lenbits << 18);      // LEN0
    return dr7;
}

static void ArmAllThreads()
{
    DWORD myTid = GetCurrentThreadId(), myPid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != myPid || te.th32ThreadID == myTid) continue;
            HANDLE th = OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|THREAD_SUSPEND_RESUME,
                                   FALSE, te.th32ThreadID);
            if (!th) continue;
            SuspendThread(th);
            CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &ctx)) {
                ctx.Dr0 = g_target;
                ctx.Dr7 = Dr7ForWatch(g_len, g_writeOnly);
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                SetThreadContext(th, &ctx);
            }
            ResumeThread(th);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static uint32_t ReadOffsetCfg()
{
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\tracer_cfg.txt", "r") == 0 && f) {
        unsigned v = 0; if (fscanf_s(f, "%x", &v) == 1) { fclose(f); return v; }
        fclose(f);
    }
    return 0xD8; // default: m_posMap
}

// Session 10 cont.: optional noise filter (see g_skipValue above). Returns
// true and fills *outVal if the file exists and parses.
static bool ReadSkipValueCfg(uint32_t* outVal)
{
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\tracer_skip_value.txt", "r") == 0 && f) {
        unsigned v = 0;
        bool ok = fscanf_s(f, "%x", &v) == 1;
        fclose(f);
        if (ok) { *outVal = v; return true; }
    }
    return false;
}

// Session 10: optional separate hero-relative offset for the SNAPSHOT read
// (see g_snapshotBase above). Returns true and fills *outOff if the file
// exists and parses; caller falls back to the trigger offset otherwise.
static bool ReadSnapshotBaseOffsetCfg(uint32_t* outOff)
{
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\tracer_snapshot_base.txt", "r") == 0 && f) {
        unsigned v = 0;
        bool ok = fscanf_s(f, "%x", &v) == 1;
        fclose(f);
        if (ok) { *outOff = v; return true; }
    }
    return false;
}

// Session 8: mere presence of this file enables write-only mode (RW0=01
// instead of 11) — much quieter for a target field also hit by unrelated
// coincidental reads (e.g. stack-slot aliasing, session 7).
static bool ReadWriteOnlyCfg()
{
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\tracer_write_only.txt", "r") == 0 && f) { fclose(f); return true; }
    return false;
}

// Session 7: absolute-address mode. Returns true and fills *outAddr if
// C:\Users\Public\tracer_target_abs.txt exists and parses.
static bool ReadAbsoluteTargetCfg(uintptr_t* outAddr)
{
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\tracer_target_abs.txt", "r") == 0 && f) {
        unsigned long long v = 0;
        bool ok = fscanf_s(f, "%llx", &v) == 1;
        fclose(f);
        if (ok) { *outAddr = (uintptr_t)v; return true; }
    }
    return false;
}

static void WriteLog(bool absoluteMode, uint32_t heroOff, uintptr_t hero)
{
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Public\\tracer_log.json", "w") != 0 || !f) return;
    EnterCriticalSection(&g_cs);
    fprintf(f, "{\n  \"mode\": \"%s\",\n  \"hero_offset\": \"0x%X\",\n  \"target\": \"0x%llX\",\n  \"hero\": \"0x%llX\",\n  \"hits\": [\n",
            absoluteMode ? "absolute" : "hero_relative",
            heroOff, (unsigned long long)g_target, (unsigned long long)hero);
    for (int i = 0; i < g_hitCount; ++i) {
        uintptr_t rva = g_hits[i].rip - (uintptr_t)g_base;
        fprintf(f, "    { \"rip\": \"0x%llX\", \"rva\": \"0x%llX\", \"count\": %u }%s\n",
                (unsigned long long)g_hits[i].rip, (unsigned long long)rva, g_hits[i].count,
                (i + 1 < g_hitCount) ? "," : "");
    }
    fprintf(f, "  ],\n  \"changes\": [\n");
    char hexbuf[SNAPSHOT_SIZE * 2 + 1];
    const DWORD firstTick = g_changeCount > 0 ? g_changes[0].tick : 0;
    for (int i = 0; i < g_changeCount; ++i) {
        uintptr_t rva = g_changes[i].rip - (uintptr_t)g_base;
        int p = 0;
        for (int b = 0; b < SNAPSHOT_SIZE; ++b)
            p += snprintf(hexbuf + p, sizeof(hexbuf) - p, "%02X", g_changes[i].snapshot[b]);
        fprintf(f, "    { \"rva\": \"0x%llX\", \"t_ms\": %u, \"snapshot\": \"%s\" }%s\n",
                (unsigned long long)rva, (unsigned)(g_changes[i].tick - firstTick), hexbuf,
                (i + 1 < g_changeCount) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    LeaveCriticalSection(&g_cs);
    fclose(f);
}

static DWORD WINAPI Run(LPVOID)
{
    InitializeCriticalSection(&g_cs);
    g_base = (uint64_t)GetModuleHandleA(nullptr);

    // wait for login (general "game is ready" gate; harmless even in
    // absolute-address mode where we don't otherwise need the hero pointer)
    uintptr_t hero = 0;
    for (int i = 0; i < 1200; ++i) {
        uint64_t rm = RdU64(g_base + 0x69C730);
        if (rm) { uint64_t h = RdU64(rm); if (h && RdU32((uintptr_t)h + 0x68) >= 1000000) { hero = (uintptr_t)h; break; } }
        Sleep(500);
    }
    if (!hero) return 0;

    uintptr_t absTarget = 0;
    bool absoluteMode = ReadAbsoluteTargetCfg(&absTarget);
    uint32_t heroOff = 0;
    if (absoluteMode) {
        g_target = absTarget;
        g_snapshotBase = g_target;
    } else {
        heroOff = ReadOffsetCfg();
        g_target = hero + heroOff;
        uint32_t snapOff = heroOff;
        ReadSnapshotBaseOffsetCfg(&snapOff);
        g_snapshotBase = hero + snapOff;
    }
    g_writeOnly = ReadWriteOnlyCfg();
    g_haveSkipValue = ReadSkipValueCfg(&g_skipValue);

    AddVectoredExceptionHandler(1, VehHandler);
    ArmAllThreads();
    uintptr_t armedTarget = g_target;

    // LIVE loop: re-read the target config each pass so Claude can retarget on the
    // fly. Flushes the log ~1/sec for real-time monitoring. IMPORTANT: it only
    // re-arms (which briefly suspends game threads) when the TARGET ACTUALLY
    // CHANGES (retarget or relog) — never on a fixed timer — so it doesn't
    // stall the client's server heartbeat.
    for (;;) {
        Sleep(1000);
        uint64_t rm = RdU64(g_base + 0x69C730);
        uint64_t h = rm ? RdU64(rm) : 0;
        if (h) hero = (uintptr_t)h;

        uintptr_t newAbsTarget = 0;
        bool newAbsoluteMode = ReadAbsoluteTargetCfg(&newAbsTarget);
        uint32_t newOff = newAbsoluteMode ? heroOff : ReadOffsetCfg();

        bool targetChanged = false;
        if (newAbsoluteMode) {
            if (!absoluteMode || newAbsTarget != absTarget) targetChanged = true;
            absoluteMode = true;
            absTarget = newAbsTarget;
            g_target = absTarget;
            g_snapshotBase = g_target;
        } else {
            if (absoluteMode || newOff != heroOff) targetChanged = true;
            absoluteMode = false;
            heroOff = newOff;
            g_target = hero + heroOff;
            uint32_t snapOff = heroOff;
            ReadSnapshotBaseOffsetCfg(&snapOff);
            g_snapshotBase = hero + snapOff;
        }
        bool newWriteOnly = ReadWriteOnlyCfg();
        if (newWriteOnly != g_writeOnly) targetChanged = true;
        g_writeOnly = newWriteOnly;

        // Cheap to just re-read every poll — no need to gate behind
        // targetChanged, and doing so independently means adding/changing a
        // skip filter takes effect within ~1s without needing a full retarget.
        g_haveSkipValue = ReadSkipValueCfg(&g_skipValue);

        if (targetChanged) {
            // Session 10 bug: this reset g_hitCount but not g_changeCount /
            // g_haveLastSnapshot, so once the content-change log filled up
            // (MAX_CHANGES) on the FIRST target, a retarget silently stopped
            // recording anything new forever — the "hits" counters looked
            // freshly reset (correct) while "changes" quietly kept serving
            // stale data from the old target. Reset all of it together.
            EnterCriticalSection(&g_cs);
            g_hitCount = 0;
            g_changeCount = 0;
            g_haveLastSnapshot = false;
            LeaveCriticalSection(&g_cs);
        }

        if (g_target != armedTarget || targetChanged) {  // re-arm on target OR mode change
            ArmAllThreads();
            armedTarget = g_target;
        }
        WriteLog(absoluteMode, heroOff, hero);
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
    }
    return TRUE;
}
