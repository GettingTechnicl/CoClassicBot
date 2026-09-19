// =====================================================================
// recvfinder.cpp — standalone READ-ONLY-w.r.t.-game-code diagnostic,
// inbound counterpart to netfinder.cpp (2026-09-18, connection-decipher
// investigation — see docs/investigation/CONNECTION_DECIPHER_PREP.md).
//
// netfinder.cpp already answered the outbound question (a hand-built
// plaintext packet through the confirmed real pipeline was accepted by the
// server with zero encryption applied — coclassicbot-live-offsets session
// 8), but nothing has ever looked at whether INBOUND (server->client) data
// is plaintext-framed the same way. `net_recv_hook.cpp`'s existing hook
// only feeds a ring buffer dumped on a rare disconnect sentinel — this
// tool instead logs EVERY inbound read with a full byte preview and
// backtrace, the same technique netfinder.cpp already uses for outbound
// (Detour the WINDOWS Winsock exports, not any game code — no debug
// registers, no hardware breakpoints, avoids the disconnect/detection risk
// tracer.cpp's technique carries).
//
// Ships as a SEPARATE dll (not a change to net_recv_hook.cpp/coclassic.dll)
// specifically so it can be injected into an ALREADY-RUNNING game process
// without needing a relaunch (Windows won't reload an updated same-named
// module already mapped into the process).
//
// Output: C:\Users\Public\coclassic_recvfinder.json, appended (one JSON
// object per line). Capped at 500 captures per run to keep the file bounded.
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

static const char* kPath = "C:\\Users\\Public\\coclassic_recvfinder.json";
static uint64_t g_base = 0;
static uint64_t g_imageSize = 0;
static CRITICAL_SECTION g_cs;
static volatile LONG g_captureCount = 0;
static const LONG kMaxCaptures = 500;
static const int kPreviewLen = 128;

typedef int (WSAAPI* RecvFn)(SOCKET, char*, int, int);
typedef int (WSAAPI* WSARecvFn)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD,
                                 LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

static RecvFn OrigRecv = nullptr;
static WSARecvFn OrigWSARecv = nullptr;

static bool InGameModule(uint64_t addr)
{
    return g_base != 0 && addr >= g_base && addr < g_base + g_imageSize;
}

// Same preview/backtrace logging shape as netfinder.cpp's LogSend, mirrored
// for inbound data. `data`/`len` must already reflect the REAL bytes
// received (i.e. called AFTER the real recv/WSARecv, unlike send's
// before-the-call logging — recv's buffer is only valid post-call).
static void LogRecv(const uint8_t* data, size_t len, const char* api)
{
    if (len == 0)
        return;
    if (InterlockedIncrement(&g_captureCount) > kMaxCaptures)
        return;

    void* frames[32] = {};
    USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);

    EnterCriticalSection(&g_cs);
    FILE* f = nullptr;
    if (fopen_s(&f, kPath, "a") == 0 && f) {
        fprintf(f, "{ \"api\":\"%s\", \"len\":%zu, \"hdr\":[", api, len);
        size_t previewLen = len < (size_t)kPreviewLen ? len : (size_t)kPreviewLen;
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

static int WSAAPI HkRecv(SOCKET s, char* buf, int len, int flags)
{
    int result = OrigRecv(s, buf, len, flags);
    if (result > 0)
        LogRecv((const uint8_t*)buf, (size_t)result, "recv");
    return result;
}

static int WSAAPI HkWSARecv(SOCKET s, LPWSABUF bufs, DWORD bufCount, LPDWORD received,
                             LPDWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    int result = OrigWSARecv(s, bufs, bufCount, received, flags, ov, cr);
    // Synchronous completions only (ov == nullptr or *received already valid) --
    // same caveat net_recv_hook.cpp already documents: overlapped/async reads
    // aren't chased here, an empty capture file would itself be a signal this
    // assumption needs rechecking, not a silent failure.
    if (result == 0 && !ov && received && bufs && bufCount > 0 && *received > 0)
        LogRecv((const uint8_t*)bufs[0].buf, (size_t)*received, "WSARecv");
    return result;
}

static DWORD WINAPI Run(LPVOID)
{
    InitializeCriticalSection(&g_cs);
    g_base = (uint64_t)GetModuleHandleA(nullptr);

    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), (HMODULE)g_base, &mi, sizeof(mi)))
        g_imageSize = mi.SizeOfImage;
    else
        g_imageSize = 0x2900000;

    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2) ws2 = LoadLibraryA("ws2_32.dll");
    if (!ws2) return 0;

    OrigRecv = (RecvFn)GetProcAddress(ws2, "recv");
    OrigWSARecv = (WSARecvFn)GetProcAddress(ws2, "WSARecv");
    if (!OrigRecv || !OrigWSARecv) return 0;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG errRecv = DetourAttach(&(PVOID&)OrigRecv, HkRecv);
    LONG errWSARecv = DetourAttach(&(PVOID&)OrigWSARecv, HkWSARecv);
    LONG errCommit = DetourTransactionCommit();

    FILE* f = nullptr;
    if (fopen_s(&f, kPath, "w") == 0 && f) {
        fprintf(f, "{ \"diag\":\"hook_install\", \"base\":\"0x%llX\", "
                    "\"err_recv\":%ld, \"err_wsarecv\":%ld, \"err_commit\":%ld }\n",
                (unsigned long long)g_base, errRecv, errWSARecv, errCommit);
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
        if (OrigRecv || OrigWSARecv) {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            if (OrigRecv) DetourDetach(&(PVOID&)OrigRecv, HkRecv);
            if (OrigWSARecv) DetourDetach(&(PVOID&)OrigWSARecv, HkWSARecv);
            DetourTransactionCommit();
        }
    }
    return TRUE;
}
