// =====================================================================
// netfinder.cpp — standalone READ-ONLY-w.r.t.-game-code RVA discovery tool
// for CNetClient::SendMsg / CNetClient::GetInstance (v1074 action RE,
// REVAMP_PLAN.md Phase 2, priority #1: every packet action routes through
// CNetClient::SendMsg, and its old RVA is confirmed stale — see
// coclassicbot-live-offsets memory).
//
// Technique: Detour-hook the WINDOWS Winsock exports `send` and `WSASend`
// (ws2_32.dll), not any game code. This is the same class of technique the
// bot's own packet logger already uses (Detouring a known function
// pointer) — it does NOT touch game code, debug registers, or hardware
// breakpoints, so it avoids the disconnect/detection risk hardware
// breakpoints hit in tracer.cpp. Every outbound game packet must eventually
// reach one of these two APIs, so hooking them gives us a stable vantage
// point below CNetClient::SendMsg without needing to already know its RVA.
//
// On each send, we walk the return-address chain (RtlCaptureStackBackTrace)
// and record every frame that lands inside the game module, plus the first
// bytes of the payload (which carry the bot's already-known [u16 size]
// [u16 type] packet header). CNetClient::SendMsg is the ONE frame that is
// common to every packet type; do several different actions in-game (walk,
// talk to an NPC, attack, open a menu) while this is injected, then diff
// the frame lists offline — the RVA that shows up in every capture, at a
// stable shallow depth right above the Winsock call, is CNetClient::SendMsg.
// Its caller's frame (one level up) is a strong CNetClient::GetInstance /
// call-site candidate to cross-check against.
//
// Output: C:\Users\Public\coclassic_netfinder.json, appended (one JSON
// object per line) so nothing is lost across many captures. Capped at
// 500 captures per run to keep the file bounded.
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>
#include <detours.h>
#include <cstdint>
#include <cstdio>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")

static const char* kPath = "C:\\Users\\Public\\coclassic_netfinder.json";
static uint64_t g_base = 0;
static uint64_t g_imageSize = 0;
static CRITICAL_SECTION g_cs;
static volatile LONG g_captureCount = 0;
static const LONG kMaxCaptures = 500;

typedef int (WSAAPI* SendFn)(SOCKET, const char*, int, int);
typedef int (WSAAPI* WSASendFn)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

static SendFn OrigSend = nullptr;
static WSASendFn OrigWSASend = nullptr;

// ---------------------------------------------------------------------
// Session-4b addendum: live capture (52 sends, walk+npc+attack) showed an
// IDENTICAL 5-frame chain for every single outgoing packet:
//   0x1DD888 (send/WSASend via IAT thunk) <- 0x1909D9 <- 0xBFB48 <- 0xE9B5C <- 0x384729
// Resync-disassembly of the real function bodies (code_dump.bin is static,
// RVA == file offset) found two single-parameter (rcx-only) candidates on
// that path, one call apart:
//   RVA 0xE9960 — thin wrapper: calls the 0x96FE0 singleton accessor twice,
//     validates via 0xBD5D0(singleton, param), and on the non-suppressed
//     path (which every one of our 52 captures took) calls 0xBF920(singleton).
//   RVA 0xBF920 — heavier function, does string/log-style setup with 'this'
//     from the singleton at global 0x698BE0 (found via the 0x96FE0 accessor,
//     itself a known MSVC magic-static singleton getter).
// Neither matches the OLD bot's assumed CNetClient::SendMsg(client,data,size)
// 3-arg shape — v1074 appears to route through a message/param OBJECT
// (passed as the sole rcx arg) rather than a raw buffer+length pair, and the
// payload bytes actually reaching send() are already encrypted (no readable
// [size][type] header), so the plaintext buffer this bot would need to hand
// off lives further up the chain, before whatever does the encryption.
// These two hooks dump the raw bytes AT the single pointer argument (SEH-
// guarded) so we can see, live, whether either one is holding onto a
// still-plaintext [u16 size][u16 type]-shaped buffer we can correlate
// against the known wire-format constants (0x3F2/0x420/0x7EF/etc.) — that's
// what actually pins down which layer to hand a bot-crafted packet to.
// ---------------------------------------------------------------------
typedef int64_t(*OneArgFn)(void*);
static OneArgFn OrigE9960 = nullptr;
static OneArgFn OrigBF920 = nullptr;

template <class T>
static bool TryRead(uintptr_t a, T* out)
{
    __try { *out = *reinterpret_cast<volatile T*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void LogArgDump(const char* tag, void* param)
{
    if (InterlockedIncrement(&g_captureCount) > kMaxCaptures)
        return;

    uint64_t vtableOrFirstQword = 0;
    TryRead((uintptr_t)param, &vtableOrFirstQword);

    EnterCriticalSection(&g_cs);
    FILE* f = nullptr;
    if (fopen_s(&f, kPath, "a") == 0 && f) {
        fprintf(f, "{ \"api\":\"%s\", \"param\":\"0x%llX\", \"first_qword\":\"0x%llX\", \"bytes\":[",
                tag, (unsigned long long)(uintptr_t)param, (unsigned long long)vtableOrFirstQword);
        for (int i = 0; i < 64; ++i) {
            uint8_t b = 0;
            TryRead((uintptr_t)param + i, &b);
            fprintf(f, "%u%s", b, (i + 1 < 64) ? "," : "");
        }
        fprintf(f, "] }\n");
        fclose(f);
    }
    LeaveCriticalSection(&g_cs);
}

static int64_t HkE9960(void* param)
{
    LogArgDump("fn_E9960", param);
    return OrigE9960(param);
}

static int64_t HkBF920(void* param)
{
    LogArgDump("fn_BF920", param);
    return OrigBF920(param);
}

static bool InGameModule(uint64_t addr)
{
    return g_base != 0 && addr >= g_base && addr < g_base + g_imageSize;
}

// Walk the return-address chain and log every frame inside the game module,
// plus a small hex preview of the payload for manual packet-type correlation.
// Also peeks the live first bytes at the two game-code hook targets (0xE9960/
// 0xBF920) on every call — if those hooks silently got reverted (e.g. some
// anti-tamper mechanism restoring the original bytes after DetourAttach
// successfully patched them), this is how we'd catch it: the bytes would
// no longer look like a jmp/trampoline stub.
static void LogSend(const uint8_t* data, size_t len, const char* api)
{
    if (InterlockedIncrement(&g_captureCount) > kMaxCaptures)
        return;

    void* frames[32] = {};
    USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);

    uint8_t liveE9960[8] = {}, liveBF920[8] = {};
    if (g_base) {
        for (int i = 0; i < 8; ++i) {
            TryRead(g_base + 0xE9960 + i, &liveE9960[i]);
            TryRead(g_base + 0xBF920 + i, &liveBF920[i]);
        }
    }

    EnterCriticalSection(&g_cs);
    FILE* f = nullptr;
    if (fopen_s(&f, kPath, "a") == 0 && f) {
        fprintf(f, "{ \"api\":\"%s\", \"len\":%zu, \"live_e9960\":[%u,%u,%u,%u,%u,%u,%u,%u], "
                    "\"live_bf920\":[%u,%u,%u,%u,%u,%u,%u,%u], \"hdr\":[",
                api, len,
                liveE9960[0], liveE9960[1], liveE9960[2], liveE9960[3], liveE9960[4], liveE9960[5], liveE9960[6], liveE9960[7],
                liveBF920[0], liveBF920[1], liveBF920[2], liveBF920[3], liveBF920[4], liveBF920[5], liveBF920[6], liveBF920[7]);
        size_t previewLen = len < 16 ? len : 16;
        for (size_t i = 0; i < previewLen; ++i)
            fprintf(f, "%u%s", data ? data[i] : 0, (i + 1 < previewLen) ? "," : "");
        fprintf(f, "], \"frames\":[");
        bool first = true;
        for (USHORT i = 0; i < n; ++i) {
            uint64_t addr = (uint64_t)frames[i];
            if (!InGameModule(addr))
                continue;
            fprintf(f, "%s\"0x%llX\"", first ? "" : ",", (unsigned long long)(addr - g_base));
            first = false;
        }
        fprintf(f, "] }\n");
        fclose(f);
    }
    LeaveCriticalSection(&g_cs);
}

static int WSAAPI HkSend(SOCKET s, const char* buf, int len, int flags)
{
    LogSend((const uint8_t*)buf, (size_t)(len > 0 ? len : 0), "send");
    return OrigSend(s, buf, len, flags);
}

static int WSAAPI HkWSASend(SOCKET s, LPWSABUF bufs, DWORD bufCount, LPDWORD sent,
                             DWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    if (bufs && bufCount > 0)
        LogSend((const uint8_t*)bufs[0].buf, (size_t)bufs[0].len, "WSASend");
    return OrigWSASend(s, bufs, bufCount, sent, flags, ov, cr);
}

static DWORD WINAPI Run(LPVOID)
{
    InitializeCriticalSection(&g_cs);
    g_base = (uint64_t)GetModuleHandleA(nullptr);

    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), (HMODULE)g_base, &mi, sizeof(mi)))
        g_imageSize = mi.SizeOfImage;
    else
        g_imageSize = 0x2900000; // fallback: known approx image size from prior dumps

    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2) ws2 = LoadLibraryA("ws2_32.dll");
    if (!ws2) return 0;

    OrigSend = (SendFn)GetProcAddress(ws2, "send");
    OrigWSASend = (WSASendFn)GetProcAddress(ws2, "WSASend");
    if (!OrigSend || !OrigWSASend) return 0;

    // These two RVAs come from resync-disassembly of code_dump.bin and are real
    // function starts on the confirmed send() call chain (see comment above) —
    // not the old stale pre-v1074 RVAs, so hooking them here is low-risk.
    OrigE9960 = (OneArgFn)(g_base + 0xE9960);
    OrigBF920 = (OneArgFn)(g_base + 0xBF920);

    void* preE9960 = (void*)OrigE9960;
    void* preBF920 = (void*)OrigBF920;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG errSend = OrigSend ? DetourAttach(&(PVOID&)OrigSend, HkSend) : -1;
    LONG errWSASend = OrigWSASend ? DetourAttach(&(PVOID&)OrigWSASend, HkWSASend) : -1;
    LONG errE9960 = DetourAttach(&(PVOID&)OrigE9960, HkE9960);
    LONG errBF920 = DetourAttach(&(PVOID&)OrigBF920, HkBF920);
    LONG errCommit = DetourTransactionCommit();

    // Truncate the output file at start of each run and record hook-install
    // diagnostics as the first line — DetourAttach can silently no-op a hook
    // if the commit fails, so log every individual + overall error code plus
    // whether each Orig pointer actually moved from its plain target to a
    // trampoline (a real sign the hook took).
    FILE* f = nullptr;
    if (fopen_s(&f, kPath, "w") == 0 && f) {
        fprintf(f,
            "{ \"diag\":\"hook_install\", \"base\":\"0x%llX\", "
            "\"err_send\":%ld, \"err_wsasend\":%ld, \"err_e9960\":%ld, \"err_bf920\":%ld, \"err_commit\":%ld, "
            "\"e9960_target_pre\":\"0x%llX\", \"e9960_trampoline_post\":\"0x%llX\", "
            "\"bf920_target_pre\":\"0x%llX\", \"bf920_trampoline_post\":\"0x%llX\" }\n",
            (unsigned long long)g_base,
            errSend, errWSASend, errE9960, errBF920, errCommit,
            (unsigned long long)(uintptr_t)preE9960, (unsigned long long)(uintptr_t)(void*)OrigE9960,
            (unsigned long long)(uintptr_t)preBF920, (unsigned long long)(uintptr_t)(void*)OrigBF920);
        fclose(f);
    }

    // Nothing else to do — the hooks do the work. Sleep forever; DLL_PROCESS_DETACH cleans up.
    for (;;) Sleep(5000);
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (OrigSend || OrigWSASend || OrigE9960 || OrigBF920) {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            if (OrigSend) DetourDetach(&(PVOID&)OrigSend, HkSend);
            if (OrigWSASend) DetourDetach(&(PVOID&)OrigWSASend, HkWSASend);
            if (OrigE9960) DetourDetach(&(PVOID&)OrigE9960, HkE9960);
            if (OrigBF920) DetourDetach(&(PVOID&)OrigBF920, HkBF920);
            DetourTransactionCommit();
        }
    }
    return TRUE;
}
