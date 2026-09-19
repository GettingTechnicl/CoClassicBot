// =====================================================================
// flushfinder.cpp — standalone, READ-ONLY-w.r.t.-game-code diagnostic to
// find the exact pipeline stage where a message transforms, without going
// anywhere near the anti-tamper-protected 0xE9960/0xBF920 region.
// (2026-09-18 connection-decipher investigation, see
// docs/investigation/CONNECTION_DECIPHER_PREP.md's "known-plaintext
// wire-capture experiment" section for the reasoning.)
//
// Confirmed tonight: a fully-deterministic 23-byte plaintext (from the
// "Debug: Native Pickup Test" button, entering via SendPacket()/0x1DD450 --
// already disassembled as copying bytes verbatim, no visible transform)
// never reaches the wire unchanged (netfinder capture, twice, verified
// live). The transform therefore happens somewhere between 0x1DD450's
// enqueue and the real send() call netfinder already observes.
//
// 0x1DD860 is the per-tick poller that drains the connection object's
// pending queue (+0x2000 count, +0x2008 buffer pointer -- both RVAs
// confirmed live in coclassicbot-live-offsets memory, session 8) and
// calls the real send(). Unlike 0xE9960/0xBF920, it is EXPLICITLY
// confirmed NOT hook-resistant in this project's own history ("hooked
// 0x1908E0... traced one level deeper to 0x1DD860... hooked IT directly
// alongside send(): also fires constantly, completely decoupled from
// actual traffic" -- no anti-tamper issue reported).
//
// Technique: Detour-hook 0x1DD860 itself, but DON'T rely on knowing its
// real argument list -- on entry, independently re-resolve the pending
// buffer via the SAME external chain already proven correct
// (base+0x69A4E8 transport singleton -> +0x20 connection object ->
// +0x2000 count / +0x2008 buffer pointer), log whatever's pending BEFORE
// calling the real function, then call through unmodified. If the queue
// still holds our known plaintext at this point but netfinder observes
// different bytes hitting send(), the transform is INSIDE this function,
// between here and its own internal send() call. If the queue already
// looks transformed by the time we see it here, the transform happened
// earlier (inside 0x1DD450 or wherever native code writes into the
// buffer) and this hook narrows nothing further.
//
// Output: C:\Users\Public\coclassic_flushfinder.json, appended (one JSON
// object per line). Capped at 500 captures per run, and only logs when
// the pending count is nonzero (this function is an unconditional
// per-tick poller that fires ~144/s regardless of traffic -- logging
// every call would be pure noise).
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <detours.h>
#include <cstdint>
#include <cstdio>

static const char* kPath = "C:\\Users\\Public\\coclassic_flushfinder3.json";
static uint64_t g_base = 0;
static CRITICAL_SECTION g_cs;
static volatile LONG g_captureCount = 0;
static const LONG kMaxCaptures = 500;
static const size_t kMaxReadBytes = 256;

typedef int64_t(*OneArgFn)(void*);
static OneArgFn OrigFlush = nullptr;

template <class T>
static bool TryRead(uintptr_t addr, T* out)
{
    __try { *out = *reinterpret_cast<volatile T*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static volatile LONG g_hookFireCount = 0;

static void LogPendingBuffer()
{
    // Unconditional heartbeat -- proves whether the hook fires at all,
    // independent of whether the pending-buffer read below succeeds.
    LONG fireNum = InterlockedIncrement(&g_hookFireCount);

    if (InterlockedIncrement(&g_captureCount) > kMaxCaptures)
        return;

    __try {
        // The transport singleton lives INLINE at base+0x69A4E8 (confirmed
        // module-static storage, not a heap object) -- its own +0x20 field
        // is the connection object pointer (same chain as
        // game.h's ResolveConnectionObject(), verified live all session).
        uint64_t connPtr = 0;
        bool gotConn = TryRead<uint64_t>(g_base + 0x69A4E8 + 0x20, &connPtr) && connPtr != 0;

        uint64_t count = 0, bufPtrRaw = 0;
        bool gotCount = gotConn && TryRead<uint64_t>(connPtr + 0x2000, &count);
        // +0x2008 read for reference only -- confirmed tonight (both via this
        // hook and an earlier external read on a different session) to be a
        // small, implausible value (~0xCBC-0xCC0), not a real heap pointer.
        // The actual message bytes were already proven to live INLINE at
        // connPtr+0x00 (byte-for-byte match against netfinder's wire capture,
        // verified twice earlier tonight), so read from there instead.
        TryRead<uint64_t>(connPtr + 0x2008, &bufPtrRaw);

        // Log every call for the first 30 (proves firing + shows count state
        // even when 0), then only log when there's real pending data worth
        // dumping -- avoids flooding the 500-cap purely with "count=0"
        // heartbeats once firing is already established.
        bool shouldLog = (fireNum <= 30) || (gotCount && count > 0 && count <= kMaxReadBytes);
        if (!shouldLog)
            return;

        uint8_t buf[kMaxReadBytes] = {};
        size_t readLen = 0;
        if (gotConn && gotCount && count > 0 && count <= kMaxReadBytes) {
            for (size_t i = 0; i < count; ++i) {
                uint8_t b = 0;
                if (!TryRead<uint8_t>(connPtr + i, &b)) break;
                buf[i] = b;
                readLen++;
            }
        }

        EnterCriticalSection(&g_cs);
        FILE* f = nullptr;
        if (fopen_s(&f, kPath, "a") == 0 && f) {
            fprintf(f, "{ \"fireNum\":%ld, \"gotConn\":%s, \"connPtr\":\"0x%llX\", "
                        "\"gotCount\":%s, \"count\":%llu, \"field2008\":\"0x%llX\", \"data\":[",
                    fireNum, gotConn ? "true" : "false", (unsigned long long)connPtr,
                    gotCount ? "true" : "false", (unsigned long long)count,
                    (unsigned long long)bufPtrRaw);
            for (size_t i = 0; i < readLen; ++i)
                fprintf(f, "%u%s", buf[i], (i + 1 < readLen) ? "," : "");
            fprintf(f, "] }\n");
            fclose(f);
        }
        LeaveCriticalSection(&g_cs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Never let our own diagnostic logic fault the game thread.
    }
}

static int64_t HkFlush(void* a1)
{
    // Read-only inspection BEFORE the real call -- never modifies a1, the
    // buffer, or anything else. The call-through below is unconditional
    // and untouched, so this function's real behavior is unchanged.
    LogPendingBuffer();
    return OrigFlush(a1);
}

static DWORD WINAPI Run(LPVOID)
{
    InitializeCriticalSection(&g_cs);
    g_base = (uint64_t)GetModuleHandleA(nullptr);

    OrigFlush = (OneArgFn)(g_base + 0x1DD860);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG err = DetourAttach(&(PVOID&)OrigFlush, HkFlush);
    LONG errCommit = DetourTransactionCommit();

    FILE* f = nullptr;
    if (fopen_s(&f, kPath, "w") == 0 && f) {
        fprintf(f, "{ \"diag\":\"hook_install\", \"base\":\"0x%llX\", \"err_attach\":%ld, \"err_commit\":%ld }\n",
                (unsigned long long)g_base, err, errCommit);
        fclose(f);
    }

    for (;;) Sleep(5000);
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (OrigFlush) {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourDetach(&(PVOID&)OrigFlush, HkFlush);
            DetourTransactionCommit();
        }
    }
    return TRUE;
}
