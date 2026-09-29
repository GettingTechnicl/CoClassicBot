// =====================================================================
// net_connect_hook.cpp — Phase 1 Step 2 of the containment project (docs/investigation/
// CONTAINMENT.md). Detour-hooks the ws2_32 connect-family exports and, when armed,
// rewrites each non-loopback destination to a loopback address handed out by
// injector/main.cpp's ConnectGateway -- so the game leg is carried by the relay too,
// not just the login leg (which already goes through Socks5Relay via the patched
// servers.json, and is already loopback by the time it reaches this hook).
//
// Armed ONLY when the COCLASSIC_GW environment variable is set (host:port of the
// gateway's control port) -- set by launcher.exe exclusively for an account launched
// with proxy mode on. A manually-injected DLL, or proxy mode off, leaves this unset,
// and every hook here is then pure pass-through -- identical in effect to
// connectfinder.cpp's observe-only build, just permanently present instead of a
// separate diagnostic DLL.
//
// SCOPE, grounded in Step 1's live capture (docs/investigation/CONTAINMENT.md): this
// game build uses ONLY plain connect() for both legs -- WSAConnect, WSAConnectByNameA/W,
// WSAConnectByList and ConnectEx were all hooked during that capture and never once
// called. connect(), WSAConnect() and ConnectEx() all take a plain sockaddr destination,
// so all three get REAL redirect logic here (belt-and-suspenders per project standing
// practice, in case a client update switches APIs). WSAConnectByNameA/W and
// WSAConnectByList resolve a hostname INTERNALLY -- there's no sockaddr this hook can
// rewrite without itself calling getaddrinfo, which hasn't been built since nothing has
// ever called them. Consistent with "nothing escapes" over "everything works": if the
// gateway is armed and one of those three ever fires, it is refused (fail-closed) rather
// than let through unrouted, and logged loudly since that would mean a client update
// changed behavior in a way Step 1's evidence didn't anticipate.
//
// FAIL CLOSED throughout: any redirect failure (gateway unreachable, control protocol
// error, timeout) refuses the connect (WSAECONNREFUSED) rather than falling through to
// the real address -- the same principle as Phase 0's firewall backstop, just one layer
// closer to the source.
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include "net_connect_hook.h"
#include <detours.h>
#include <spdlog/spdlog.h>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

typedef int (WSAAPI* ConnectFn)(SOCKET, const sockaddr*, int);
typedef int (WSAAPI* WSAConnectFn)(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
typedef BOOL (WSAAPI* WSAConnectByNameAFn)(SOCKET, LPSTR, LPSTR, LPDWORD, LPSOCKADDR, LPDWORD, LPSOCKADDR, const timeval*, LPWSAOVERLAPPED);
typedef BOOL (WSAAPI* WSAConnectByNameWFn)(SOCKET, LPWSTR, LPWSTR, LPDWORD, LPSOCKADDR, LPDWORD, LPSOCKADDR, const timeval*, LPWSAOVERLAPPED);
typedef BOOL (WSAAPI* WSAConnectByListFn)(SOCKET, PSOCKET_ADDRESS_LIST, LPDWORD, LPSOCKADDR, LPDWORD, LPSOCKADDR, const timeval*, LPWSAOVERLAPPED);
typedef int (WSAAPI* WSAIoctlFn)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef BOOL (PASCAL* ConnectExFn)(SOCKET, const sockaddr*, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);

ConnectFn OrigConnect = nullptr;
WSAConnectFn OrigWSAConnect = nullptr;
WSAConnectByNameAFn OrigWSAConnectByNameA = nullptr;
WSAConnectByNameWFn OrigWSAConnectByNameW = nullptr;
WSAConnectByListFn OrigWSAConnectByList = nullptr;
WSAIoctlFn OrigWSAIoctl = nullptr;
ConnectExFn OrigConnectEx = nullptr;
volatile LONG g_connectExHooked = 0;

bool g_gatewayEnabled = false;
std::string g_gatewayHost;
uint16_t g_gatewayControlPort = 0;

// Only IPv4 is ever redirected or even inspected -- Step 1's live capture saw IPv4-only
// traffic, and mishandling an address family that has never actually been observed is a
// worse risk than simply leaving it untouched (it passes straight through unmodified).
bool IsRedirectableAndNotLoopback(const sockaddr* sa, int len, const sockaddr_in*& out)
{
    if (!sa || len < static_cast<int>(sizeof(sockaddr_in)) || sa->sa_family != AF_INET)
        return false;
    out = reinterpret_cast<const sockaddr_in*>(sa);
    const uint32_t hostOrder = ntohl(out->sin_addr.S_un.S_addr);
    return (hostOrder >> 24) != 127; // whole 127.0.0.0/8, not just 127.0.0.1
}

// One control-channel round trip to the gateway: "CONNECT host:port\n" -> "PORT nnnn\n" /
// "ERR ...\n". Bounded ~2s on connect and on the reply so a stalled/dead gateway can never
// hang the game's own connect() call; a timeout is treated as a normal failure (fail-closed
// by the caller), same as any other gateway error. Uses OrigConnect directly (never HkConnect)
// so this internal control connection is never itself subject to redirection -- moot anyway
// since it always targets loopback, but this keeps the intent explicit rather than relying
// solely on the loopback check to prevent recursion.
bool RequestGatewayPort(const std::string& destHost, uint16_t destPort, uint16_t& outPort)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return false;

    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_gatewayControlPort);
    InetPtonA(AF_INET, g_gatewayHost.c_str(), &addr.sin_addr);

    int rc = OrigConnect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        return false;
    }

    timeval connTimeout{2, 0};
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(s, &writeSet);
    if (select(0, nullptr, &writeSet, nullptr, &connTimeout) <= 0) {
        closesocket(s);
        return false;
    }
    int soErr = 0;
    int soErrLen = sizeof(soErr);
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &soErrLen);
    if (soErr != 0) {
        closesocket(s);
        return false;
    }

    u_long blocking = 0;
    ioctlsocket(s, FIONBIO, &blocking);

    char req[300];
    int reqLen = sprintf_s(req, "CONNECT %s:%u\n", destHost.c_str(), destPort);
    if (reqLen <= 0 || send(s, req, reqLen, 0) != reqLen) {
        closesocket(s);
        return false;
    }

    timeval readTimeout{2, 0};
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(s, &readSet);
    if (select(0, &readSet, nullptr, nullptr, &readTimeout) <= 0) {
        closesocket(s);
        return false;
    }
    char reply[64] = {};
    int n = recv(s, reply, sizeof(reply) - 1, 0);
    closesocket(s);
    if (n <= 0)
        return false;
    reply[n] = 0;

    unsigned port = 0;
    if (sscanf_s(reply, "PORT %u", &port) == 1 && port > 0 && port <= 65535) {
        outPort = static_cast<uint16_t>(port);
        return true;
    }
    return false;
}

enum class RedirectResult { PassThrough, Redirected, FailClosed };

// Rewrites `name`/`namelen` in place (into `storage`) to point at the gateway's loopback
// tunnel for that destination. Only ever called for connect()/WSAConnect()/ConnectEx(),
// which all share this exact (sockaddr*, namelen) shape.
RedirectResult TryRedirect(const sockaddr*& name, int& namelen, sockaddr_in& storage)
{
    if (!g_gatewayEnabled)
        return RedirectResult::PassThrough;

    const sockaddr_in* in4 = nullptr;
    if (!IsRedirectableAndNotLoopback(name, namelen, in4))
        return RedirectResult::PassThrough; // loopback, or a family we don't touch

    char hostBuf[16] = {};
    InetNtopA(AF_INET, const_cast<IN_ADDR*>(&in4->sin_addr), hostBuf, sizeof(hostBuf));
    const uint16_t destPort = ntohs(in4->sin_port);

    uint16_t gwPort = 0;
    if (!RequestGatewayPort(hostBuf, destPort, gwPort)) {
        spdlog::error("[connectgw] FAIL-CLOSED: gateway unreachable/refused for {}:{} -- blocking this connect", hostBuf, destPort);
        return RedirectResult::FailClosed;
    }

    storage = sockaddr_in{};
    storage.sin_family = AF_INET;
    storage.sin_port = htons(gwPort);
    InetPtonA(AF_INET, "127.0.0.1", &storage.sin_addr);
    name = reinterpret_cast<const sockaddr*>(&storage);
    namelen = sizeof(storage);
    spdlog::info("[connectgw] redirecting {}:{} -> 127.0.0.1:{}", hostBuf, destPort, gwPort);
    return RedirectResult::Redirected;
}

int WSAAPI HkConnect(SOCKET s, const sockaddr* name, int namelen)
{
    sockaddr_in storage;
    const sockaddr* useName = name;
    int useLen = namelen;
    if (TryRedirect(useName, useLen, storage) == RedirectResult::FailClosed) {
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }
    return OrigConnect(s, useName, useLen);
}

int WSAAPI HkWSAConnect(SOCKET s, const sockaddr* name, int namelen, LPWSABUF callerData,
                         LPWSABUF calleeData, LPQOS sqos, LPQOS gqos)
{
    sockaddr_in storage;
    const sockaddr* useName = name;
    int useLen = namelen;
    if (TryRedirect(useName, useLen, storage) == RedirectResult::FailClosed) {
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }
    return OrigWSAConnect(s, useName, useLen, callerData, calleeData, sqos, gqos);
}

BOOL PASCAL HkConnectEx(SOCKET s, const sockaddr* name, int namelen, PVOID sendBuf, DWORD sendLen,
                         LPDWORD bytesSent, LPOVERLAPPED ov)
{
    sockaddr_in storage;
    const sockaddr* useName = name;
    int useLen = namelen;
    if (TryRedirect(useName, useLen, storage) == RedirectResult::FailClosed) {
        WSASetLastError(WSAECONNREFUSED);
        return FALSE;
    }
    return OrigConnectEx(s, useName, useLen, sendBuf, sendLen, bytesSent, ov);
}

// These three resolve a hostname INTERNALLY -- no sockaddr to rewrite without this hook
// doing its own getaddrinfo, which hasn't been built since nothing has ever called them
// (Step 1's live capture). Refuse outright while armed rather than let an unrouted
// connection through silently; this is the "observe+warn" backstop for the rest of the
// family the reviewer asked to keep hooked.
BOOL WSAAPI HkWSAConnectByNameA(SOCKET s, LPSTR node, LPSTR service, LPDWORD localLen, LPSOCKADDR local,
                                 LPDWORD remoteLen, LPSOCKADDR remote, const timeval* timeout, LPWSAOVERLAPPED ov)
{
    if (g_gatewayEnabled) {
        spdlog::error("[connectgw] FAIL-CLOSED: WSAConnectByNameA(node={}) has no redirect support and the gateway "
                      "is armed -- refusing rather than letting it escape unrouted", node ? node : "(null)");
        WSASetLastError(WSAECONNREFUSED);
        return FALSE;
    }
    return OrigWSAConnectByNameA(s, node, service, localLen, local, remoteLen, remote, timeout, ov);
}

BOOL WSAAPI HkWSAConnectByNameW(SOCKET s, LPWSTR node, LPWSTR service, LPDWORD localLen, LPSOCKADDR local,
                                 LPDWORD remoteLen, LPSOCKADDR remote, const timeval* timeout, LPWSAOVERLAPPED ov)
{
    if (g_gatewayEnabled) {
        char nodeA[256] = "(null)";
        if (node) WideCharToMultiByte(CP_UTF8, 0, node, -1, nodeA, sizeof(nodeA), nullptr, nullptr);
        spdlog::error("[connectgw] FAIL-CLOSED: WSAConnectByNameW(node={}) has no redirect support and the gateway "
                      "is armed -- refusing rather than letting it escape unrouted", nodeA);
        WSASetLastError(WSAECONNREFUSED);
        return FALSE;
    }
    return OrigWSAConnectByNameW(s, node, service, localLen, local, remoteLen, remote, timeout, ov);
}

BOOL WSAAPI HkWSAConnectByList(SOCKET s, PSOCKET_ADDRESS_LIST list, LPDWORD localLen, LPSOCKADDR local,
                                LPDWORD remoteLen, LPSOCKADDR remote, const timeval* timeout, LPWSAOVERLAPPED ov)
{
    if (g_gatewayEnabled) {
        spdlog::error("[connectgw] FAIL-CLOSED: WSAConnectByList has no redirect support and the gateway is armed "
                      "-- refusing rather than letting it escape unrouted");
        WSASetLastError(WSAECONNREFUSED);
        return FALSE;
    }
    return OrigWSAConnectByList(s, list, localLen, local, remoteLen, remote, timeout, ov);
}

// ConnectEx isn't a plain export -- code obtains it per-socket via
// WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER, &WSAID_CONNECTEX, ...). Hook WSAIoctl, let
// the real call fill in the function pointer as normal, then Detour that pointer too,
// exactly once for the process.
int WSAAPI HkWSAIoctl(SOCKET s, DWORD code, LPVOID inBuf, DWORD inLen, LPVOID outBuf, DWORD outLen,
                       LPDWORD bytesReturned, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    const int result = OrigWSAIoctl(s, code, inBuf, inLen, outBuf, outLen, bytesReturned, ov, cr);

    if (result == 0 && code == SIO_GET_EXTENSION_FUNCTION_POINTER && inBuf && inLen >= sizeof(GUID)
        && outBuf && outLen >= sizeof(PVOID) && IsEqualGUID(*static_cast<const GUID*>(inBuf), WSAID_CONNECTEX)) {
        if (InterlockedCompareExchange(&g_connectExHooked, 1, 0) == 0) {
            OrigConnectEx = *reinterpret_cast<ConnectExFn*>(outBuf);
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            LONG err = DetourAttach(&reinterpret_cast<PVOID&>(OrigConnectEx), HkConnectEx);
            LONG errCommit = DetourTransactionCommit();
            spdlog::info("[connectgw] ConnectEx hook install: addr=0x{:X} err={} commit={}",
                         reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(OrigConnectEx)), err, errCommit);
        }
    }
    return result;
}

} // namespace

void InitNetConnectHook()
{
    char buf[256] = {};
    DWORD n = GetEnvironmentVariableA("COCLASSIC_GW", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        const std::string val(buf, n);
        const size_t colon = val.find(':');
        if (colon != std::string::npos) {
            g_gatewayHost = val.substr(0, colon);
            try {
                g_gatewayControlPort = static_cast<uint16_t>(std::stoi(val.substr(colon + 1)));
                g_gatewayEnabled = true;
            } catch (...) {
                spdlog::error("[connectgw] COCLASSIC_GW='{}' has an unparsable port -- staying disabled (pass-through only)", val);
            }
        } else {
            spdlog::error("[connectgw] COCLASSIC_GW='{}' missing ':port' -- staying disabled (pass-through only)", val);
        }
    }

    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2) ws2 = LoadLibraryA("ws2_32.dll");
    if (!ws2) {
        spdlog::error("[connectgw] ws2_32.dll not found");
        return;
    }

    OrigConnect = reinterpret_cast<ConnectFn>(GetProcAddress(ws2, "connect"));
    OrigWSAConnect = reinterpret_cast<WSAConnectFn>(GetProcAddress(ws2, "WSAConnect"));
    OrigWSAConnectByNameA = reinterpret_cast<WSAConnectByNameAFn>(GetProcAddress(ws2, "WSAConnectByNameA"));
    OrigWSAConnectByNameW = reinterpret_cast<WSAConnectByNameWFn>(GetProcAddress(ws2, "WSAConnectByNameW"));
    OrigWSAConnectByList = reinterpret_cast<WSAConnectByListFn>(GetProcAddress(ws2, "WSAConnectByList"));
    OrigWSAIoctl = reinterpret_cast<WSAIoctlFn>(GetProcAddress(ws2, "WSAIoctl"));

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG e1 = OrigConnect ? DetourAttach(&reinterpret_cast<PVOID&>(OrigConnect), HkConnect) : -1;
    LONG e2 = OrigWSAConnect ? DetourAttach(&reinterpret_cast<PVOID&>(OrigWSAConnect), HkWSAConnect) : -1;
    LONG e3 = OrigWSAConnectByNameA ? DetourAttach(&reinterpret_cast<PVOID&>(OrigWSAConnectByNameA), HkWSAConnectByNameA) : -1;
    LONG e4 = OrigWSAConnectByNameW ? DetourAttach(&reinterpret_cast<PVOID&>(OrigWSAConnectByNameW), HkWSAConnectByNameW) : -1;
    LONG e5 = OrigWSAConnectByList ? DetourAttach(&reinterpret_cast<PVOID&>(OrigWSAConnectByList), HkWSAConnectByList) : -1;
    LONG e6 = OrigWSAIoctl ? DetourAttach(&reinterpret_cast<PVOID&>(OrigWSAIoctl), HkWSAIoctl) : -1;
    LONG ec = DetourTransactionCommit();

    spdlog::info("[connectgw] gateway={} hooks: connect={} wsaconnect={} bynamea={} bynamew={} bylist={} ioctl={} commit={}",
                 g_gatewayEnabled ? (g_gatewayHost + ":" + std::to_string(g_gatewayControlPort)) : "(disabled -- observe/pass-through only)",
                 e1 == NO_ERROR ? "OK" : "FAILED", e2 == NO_ERROR ? "OK" : "FAILED", e3 == NO_ERROR ? "OK" : "FAILED",
                 e4 == NO_ERROR ? "OK" : "FAILED", e5 == NO_ERROR ? "OK" : "FAILED", e6 == NO_ERROR ? "OK" : "FAILED",
                 ec == NO_ERROR ? "OK" : "FAILED");
}

void CleanupNetConnectHook()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (OrigConnect) DetourDetach(&reinterpret_cast<PVOID&>(OrigConnect), HkConnect);
    if (OrigWSAConnect) DetourDetach(&reinterpret_cast<PVOID&>(OrigWSAConnect), HkWSAConnect);
    if (OrigWSAConnectByNameA) DetourDetach(&reinterpret_cast<PVOID&>(OrigWSAConnectByNameA), HkWSAConnectByNameA);
    if (OrigWSAConnectByNameW) DetourDetach(&reinterpret_cast<PVOID&>(OrigWSAConnectByNameW), HkWSAConnectByNameW);
    if (OrigWSAConnectByList) DetourDetach(&reinterpret_cast<PVOID&>(OrigWSAConnectByList), HkWSAConnectByList);
    if (OrigWSAIoctl) DetourDetach(&reinterpret_cast<PVOID&>(OrigWSAIoctl), HkWSAIoctl);
    if (OrigConnectEx) DetourDetach(&reinterpret_cast<PVOID&>(OrigConnectEx), HkConnectEx);
    DetourTransactionCommit();
}
