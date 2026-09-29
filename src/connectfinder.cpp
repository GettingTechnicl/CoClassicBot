// =====================================================================
// connectfinder.cpp — standalone, OBSERVE-ONLY diagnostic DLL: Phase 1 Step 1 of the
// containment project (docs/investigation/CONTAINMENT.md). Detour-hooks every ws2_32.dll
// export that can originate an outbound connection -- connect, WSAConnect,
// WSAConnectByNameA/W, WSAConnectByList, and ConnectEx (caught via WSAIoctl's
// SIO_GET_EXTENSION_FUNCTION_POINTER, since ConnectEx isn't a plain export) -- and logs
// which one(s) the game actually calls, with what destination, from which module.
//
// Zero behavior change: every hook calls straight through to the real function with the
// caller's unmodified arguments and returns its real, unmodified result. This never
// redirects anything -- that's Phase 1 Step 2, gated on an env var, and not this file.
//
// Same technique and output convention as netfinder.cpp/recvfinder.cpp (Detour a Windows
// Winsock export rather than touch game code -- avoids the disconnect/detection risk
// hardware breakpoints or game-code patches carry), pointed at the OUTBOUND CONNECT
// surface instead of send/recv.
//
// Output: C:\Users\Public\coclassic_connectfinder.json, appended (one JSON object per
// line), truncated + a hook-install diagnostic line written at start of each run. Capped
// at 2000 captures per run.
//
// Answers, from live evidence: which connect function(s) the client uses (settles the
// choice of what Phase 1 Step 2's redirect hook must cover), and whether anything besides
// ImConquer.exe's own game/login legs connects while this is loaded (e.g. the
// 34.160.81.0:443 telemetry seen in Phase 0's firewall-block-log discovery).
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <psapi.h>
#include <detours.h>
#include <cstdint>
#include <cstdio>
#include <string>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")

static const char* kPath = "C:\\Users\\Public\\coclassic_connectfinder.json";
static uint64_t g_base = 0;
static uint64_t g_imageSize = 0;
static CRITICAL_SECTION g_cs;
static volatile LONG g_captureCount = 0;
static const LONG kMaxCaptures = 2000;

typedef int (WSAAPI* ConnectFn)(SOCKET, const sockaddr*, int);
typedef int (WSAAPI* WSAConnectFn)(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
typedef BOOL (WSAAPI* WSAConnectByNameAFn)(SOCKET, LPSTR, LPSTR, LPDWORD, LPSOCKADDR, LPDWORD, LPSOCKADDR, const timeval*, LPWSAOVERLAPPED);
typedef BOOL (WSAAPI* WSAConnectByNameWFn)(SOCKET, LPWSTR, LPWSTR, LPDWORD, LPSOCKADDR, LPDWORD, LPSOCKADDR, const timeval*, LPWSAOVERLAPPED);
typedef BOOL (WSAAPI* WSAConnectByListFn)(SOCKET, PSOCKET_ADDRESS_LIST, LPDWORD, LPSOCKADDR, LPDWORD, LPSOCKADDR, const timeval*, LPWSAOVERLAPPED);
typedef int (WSAAPI* WSAIoctlFn)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef BOOL (PASCAL* ConnectExFn)(SOCKET, const sockaddr*, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);

static ConnectFn OrigConnect = nullptr;
static WSAConnectFn OrigWSAConnect = nullptr;
static WSAConnectByNameAFn OrigWSAConnectByNameA = nullptr;
static WSAConnectByNameWFn OrigWSAConnectByNameW = nullptr;
static WSAConnectByListFn OrigWSAConnectByList = nullptr;
static WSAIoctlFn OrigWSAIoctl = nullptr;
static ConnectExFn OrigConnectEx = nullptr;          // set once, the first time WSAIoctl hands one out
static volatile LONG g_connectExHooked = 0;           // guards the one-time DetourAttach

static bool InGameModule(uint64_t addr)
{
    return g_base != 0 && addr >= g_base && addr < g_base + g_imageSize;
}

// sockaddr -> "1.2.3.4:5678" / "[::1]:5678" / "AF=<n> len=<n>" for anything else.
static std::string DescribeSockAddr(const sockaddr* sa, int len)
{
    if (!sa || len <= 0)
        return "(null)";
    char host[NI_MAXHOST] = {};
    if (sa->sa_family == AF_INET && len >= (int)sizeof(sockaddr_in)) {
        auto* in4 = reinterpret_cast<const sockaddr_in*>(sa);
        if (InetNtopA(AF_INET, (PVOID)&in4->sin_addr, host, sizeof(host)))
            return std::string(host) + ":" + std::to_string(ntohs(in4->sin_port));
    } else if (sa->sa_family == AF_INET6 && len >= (int)sizeof(sockaddr_in6)) {
        auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
        if (InetNtopA(AF_INET6, (PVOID)&in6->sin6_addr, host, sizeof(host)))
            return "[" + std::string(host) + "]:" + std::to_string(ntohs(in6->sin6_port));
    }
    char buf[64];
    sprintf_s(buf, "AF=%d len=%d", sa->sa_family, len);
    return buf;
}

// Escapes the bare minimum for our own known-safe content (hostnames from the game/OS);
// quotes and backslashes are the only characters this log ever needs to worry about.
static std::string JsonEscape(const char* s)
{
    if (!s) return "";
    std::string out;
    for (const char* p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') out += '\\';
        out += *p;
    }
    return out;
}

static void LogConnect(const char* api, SOCKET s, const std::string& dest, const char* extra)
{
    if (InterlockedIncrement(&g_captureCount) > kMaxCaptures)
        return;

    void* frames[32] = {};
    USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);

    EnterCriticalSection(&g_cs);
    FILE* f = nullptr;
    if (fopen_s(&f, kPath, "a") == 0 && f) {
        fprintf(f, "{ \"api\":\"%s\", \"socket\":\"0x%llX\", \"dest\":\"%s\", \"tid\":%lu, \"extra\":\"%s\", \"frames\":[",
                api, (unsigned long long)s, JsonEscape(dest.c_str()).c_str(), GetCurrentThreadId(),
                extra ? JsonEscape(extra).c_str() : "");
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

static int WSAAPI HkConnect(SOCKET s, const sockaddr* name, int namelen)
{
    LogConnect("connect", s, DescribeSockAddr(name, namelen), nullptr);
    return OrigConnect(s, name, namelen);
}

static int WSAAPI HkWSAConnect(SOCKET s, const sockaddr* name, int namelen,
                                LPWSABUF callerData, LPWSABUF calleeData, LPQOS sqos, LPQOS gqos)
{
    LogConnect("WSAConnect", s, DescribeSockAddr(name, namelen), nullptr);
    return OrigWSAConnect(s, name, namelen, callerData, calleeData, sqos, gqos);
}

static BOOL WSAAPI HkWSAConnectByNameA(SOCKET s, LPSTR node, LPSTR service, LPDWORD localLen, LPSOCKADDR local,
                                        LPDWORD remoteLen, LPSOCKADDR remote, const timeval* timeout, LPWSAOVERLAPPED ov)
{
    std::string extra = std::string("node=") + (node ? node : "(null)") + " service=" + (service ? service : "(null)");
    LogConnect("WSAConnectByNameA", s, "(resolved internally)", extra.c_str());
    return OrigWSAConnectByNameA(s, node, service, localLen, local, remoteLen, remote, timeout, ov);
}

static BOOL WSAAPI HkWSAConnectByNameW(SOCKET s, LPWSTR node, LPWSTR service, LPDWORD localLen, LPSOCKADDR local,
                                        LPDWORD remoteLen, LPSOCKADDR remote, const timeval* timeout, LPWSAOVERLAPPED ov)
{
    char nodeA[256] = "(null)", serviceA[64] = "(null)";
    if (node) WideCharToMultiByte(CP_UTF8, 0, node, -1, nodeA, sizeof(nodeA), nullptr, nullptr);
    if (service) WideCharToMultiByte(CP_UTF8, 0, service, -1, serviceA, sizeof(serviceA), nullptr, nullptr);
    std::string extra = std::string("node=") + nodeA + " service=" + serviceA;
    LogConnect("WSAConnectByNameW", s, "(resolved internally)", extra.c_str());
    return OrigWSAConnectByNameW(s, node, service, localLen, local, remoteLen, remote, timeout, ov);
}

static BOOL WSAAPI HkWSAConnectByList(SOCKET s, PSOCKET_ADDRESS_LIST list, LPDWORD localLen, LPSOCKADDR local,
                                       LPDWORD remoteLen, LPSOCKADDR remote, const timeval* timeout, LPWSAOVERLAPPED ov)
{
    char extra[64];
    sprintf_s(extra, "candidates=%ld", list ? list->iAddressCount : -1);
    LogConnect("WSAConnectByList", s, "(resolved internally)", extra);
    return OrigWSAConnectByList(s, list, localLen, local, remoteLen, remote, timeout, ov);
}

static BOOL PASCAL HkConnectEx(SOCKET s, const sockaddr* name, int namelen, PVOID sendBuf, DWORD sendLen,
                                LPDWORD bytesSent, LPOVERLAPPED ov)
{
    LogConnect("ConnectEx", s, DescribeSockAddr(name, namelen), nullptr);
    return OrigConnectEx(s, name, namelen, sendBuf, sendLen, bytesSent, ov);
}

// ConnectEx isn't a plain export -- code obtains it per-socket via
// WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER, &WSAID_CONNECTEX, ...). Hook WSAIoctl,
// let the real call fill in the function pointer as normal (zero behavior change), then
// Detour that pointer too, exactly once for the process (every socket gets the same
// real ConnectEx address).
static int WSAAPI HkWSAIoctl(SOCKET s, DWORD code, LPVOID inBuf, DWORD inLen, LPVOID outBuf, DWORD outLen,
                              LPDWORD bytesReturned, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    const int result = OrigWSAIoctl(s, code, inBuf, inLen, outBuf, outLen, bytesReturned, ov, cr);

    if (result == 0 && code == SIO_GET_EXTENSION_FUNCTION_POINTER && inBuf && inLen >= sizeof(GUID)
        && outBuf && outLen >= sizeof(PVOID) && IsEqualGUID(*(const GUID*)inBuf, WSAID_CONNECTEX)) {
        if (InterlockedCompareExchange(&g_connectExHooked, 1, 0) == 0) {
            OrigConnectEx = *reinterpret_cast<ConnectExFn*>(outBuf);
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            LONG err = DetourAttach(&(PVOID&)OrigConnectEx, HkConnectEx);
            LONG errCommit = DetourTransactionCommit();
            EnterCriticalSection(&g_cs);
            FILE* f = nullptr;
            if (fopen_s(&f, kPath, "a") == 0 && f) {
                fprintf(f, "{ \"diag\":\"connectex_hook_install\", \"addr\":\"0x%llX\", \"err\":%ld, \"err_commit\":%ld }\n",
                        (unsigned long long)(uintptr_t)(void*)OrigConnectEx, err, errCommit);
                fclose(f);
            }
            LeaveCriticalSection(&g_cs);
        }
    }
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
        g_imageSize = 0x2A26000; // fallback: known v1078 image size

    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2) ws2 = LoadLibraryA("ws2_32.dll");
    if (!ws2) return 0;

    OrigConnect = (ConnectFn)GetProcAddress(ws2, "connect");
    OrigWSAConnect = (WSAConnectFn)GetProcAddress(ws2, "WSAConnect");
    OrigWSAConnectByNameA = (WSAConnectByNameAFn)GetProcAddress(ws2, "WSAConnectByNameA");
    OrigWSAConnectByNameW = (WSAConnectByNameWFn)GetProcAddress(ws2, "WSAConnectByNameW");
    OrigWSAConnectByList = (WSAConnectByListFn)GetProcAddress(ws2, "WSAConnectByList");
    OrigWSAIoctl = (WSAIoctlFn)GetProcAddress(ws2, "WSAIoctl");

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG errConnect = OrigConnect ? DetourAttach(&(PVOID&)OrigConnect, HkConnect) : -1;
    LONG errWSAConnect = OrigWSAConnect ? DetourAttach(&(PVOID&)OrigWSAConnect, HkWSAConnect) : -1;
    LONG errByNameA = OrigWSAConnectByNameA ? DetourAttach(&(PVOID&)OrigWSAConnectByNameA, HkWSAConnectByNameA) : -1;
    LONG errByNameW = OrigWSAConnectByNameW ? DetourAttach(&(PVOID&)OrigWSAConnectByNameW, HkWSAConnectByNameW) : -1;
    LONG errByList = OrigWSAConnectByList ? DetourAttach(&(PVOID&)OrigWSAConnectByList, HkWSAConnectByList) : -1;
    LONG errIoctl = OrigWSAIoctl ? DetourAttach(&(PVOID&)OrigWSAIoctl, HkWSAIoctl) : -1;
    LONG errCommit = DetourTransactionCommit();

    // Truncate the output file at start of each run and record hook-install diagnostics
    // as the first line, same convention as netfinder.cpp.
    FILE* f = nullptr;
    if (fopen_s(&f, kPath, "w") == 0 && f) {
        fprintf(f,
            "{ \"diag\":\"hook_install\", \"base\":\"0x%llX\", \"image_size\":\"0x%llX\", "
            "\"err_connect\":%ld, \"err_wsaconnect\":%ld, \"err_bynamea\":%ld, \"err_bynamew\":%ld, "
            "\"err_bylist\":%ld, \"err_ioctl\":%ld, \"err_commit\":%ld, "
            "\"resolved\": { \"connect\":%d, \"wsaconnect\":%d, \"bynamea\":%d, \"bynamew\":%d, \"bylist\":%d, \"ioctl\":%d } }\n",
            (unsigned long long)g_base, (unsigned long long)g_imageSize,
            errConnect, errWSAConnect, errByNameA, errByNameW, errByList, errIoctl, errCommit,
            OrigConnect != nullptr, OrigWSAConnect != nullptr, OrigWSAConnectByNameA != nullptr,
            OrigWSAConnectByNameW != nullptr, OrigWSAConnectByList != nullptr, OrigWSAIoctl != nullptr);
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
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        if (OrigConnect) DetourDetach(&(PVOID&)OrigConnect, HkConnect);
        if (OrigWSAConnect) DetourDetach(&(PVOID&)OrigWSAConnect, HkWSAConnect);
        if (OrigWSAConnectByNameA) DetourDetach(&(PVOID&)OrigWSAConnectByNameA, HkWSAConnectByNameA);
        if (OrigWSAConnectByNameW) DetourDetach(&(PVOID&)OrigWSAConnectByNameW, HkWSAConnectByNameW);
        if (OrigWSAConnectByList) DetourDetach(&(PVOID&)OrigWSAConnectByList, HkWSAConnectByList);
        if (OrigWSAIoctl) DetourDetach(&(PVOID&)OrigWSAIoctl, HkWSAIoctl);
        if (OrigConnectEx) DetourDetach(&(PVOID&)OrigConnectEx, HkConnectEx);
        DetourTransactionCommit();
    }
    return TRUE;
}
