#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>

#include "net_recv_hook.h"
#include "action_recorder.h"
#include <detours.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

extern HMODULE g_hModule;

namespace {

typedef int (WSAAPI* RecvFn)(SOCKET, char*, int, int);
typedef int (WSAAPI* WSARecvFn)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD,
                                 LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

RecvFn OrigRecv = nullptr;
WSARecvFn OrigWSARecv = nullptr;

// =====================================================================
// [CONNECTION-DECIPHER 2026-09-28] Bounded inbound backtrace sweep -- the inbound half of
// the same technique that already found SendMsg's real function (netfinder.cpp's live
// backtrace-on-send() sweep). Themida hides the real inbound call site from static analysis
// (docs/investigation/CONNECTION_DECIPHER_PREP.md's offline exhaustion: the resolved recv()
// IAT slot was located exactly once, but nothing in any disassemblable code section
// references it -- the actual caller is inside Themida's VM-obfuscated section). Return
// addresses on the live call stack are real at runtime regardless of that obfuscation, so
// capturing them converts "statically opaque" into a set of real, offline-disassemblable
// game-module RVAs: recv -> receive-loop -> (wherever decrypt happens) -> dispatch. The goal
// of this capture is to find the EARLIEST chokepoint after decrypt (its output is complete
// inbound plaintext for every message type, a cleaner target than the dispatcher which sits
// downstream of parsing) -- the dispatcher remains the fallback if decrypt isn't identifiable
// from the captured frames.
//
// Deliberately bounded (kMaxBacktraceCaptures samples OR kBacktraceWindowMs, whichever comes
// first, starting from the FIRST real capture, not hook-install time) -- this is exploratory
// RE instrumentation for one investigation, not a permanent always-on feature; the receive
// loop calls recv() continuously, and a handful of samples is enough to lock a stable caller
// chain (the same chain repeats every read), so there is no reason to keep capturing for the
// life of the process.
constexpr int kMaxBacktraceCaptures = 60;
constexpr DWORD kBacktraceWindowMs = 60000;

std::mutex g_backtraceMutex;
std::ofstream g_backtraceLog;
std::atomic<int> g_backtraceCount{0};
std::atomic<DWORD> g_backtraceStartTick{0};
ULONG64 g_moduleBase = 0;
ULONG64 g_moduleSize = 0;

bool BacktraceCaptureActive()
{
    if (g_backtraceCount.load() >= kMaxBacktraceCaptures)
        return false;
    const DWORD start = g_backtraceStartTick.load();
    if (start != 0 && GetTickCount() - start > kBacktraceWindowMs)
        return false;
    return true;
}

void EnsureModuleRangeResolved()
{
    if (g_moduleBase)
        return;
    g_moduleBase = reinterpret_cast<ULONG64>(GetModuleHandleA(nullptr));
    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), reinterpret_cast<HMODULE>(g_moduleBase), &mi, sizeof(mi)))
        g_moduleSize = mi.SizeOfImage;
    else
        g_moduleSize = 0x2A26000; // fallback: known v1078 image size
}

std::string DescribePeer(SOCKET s)
{
    sockaddr_in peer{};
    int peerLen = sizeof(peer);
    if (getpeername(s, reinterpret_cast<sockaddr*>(&peer), &peerLen) != 0)
        return "(getpeername failed)";
    char host[NI_MAXHOST] = {};
    if (!InetNtopA(AF_INET, &peer.sin_addr, host, sizeof(host)))
        return "(unprintable)";
    return std::string(host) + ":" + std::to_string(ntohs(peer.sin_port));
}

// Bounded, one-shot-per-sample capture. `data`/`size` is exactly what OrigRecv/OrigWSARecv
// handed back -- ciphertext, pre-decrypt, same bytes the relay's own capture would show for
// this connection (correlatable via the peer ip:port logged with each entry).
void CaptureInboundBacktrace(const char* api, SOCKET s, const uint8_t* data, size_t size)
{
    if (!BacktraceCaptureActive())
        return;
    EnsureModuleRangeResolved();
    if (g_backtraceStartTick.load() == 0)
        g_backtraceStartTick.store(GetTickCount()); // window starts at the first real sample, not hook-install time

    const int seq = g_backtraceCount.fetch_add(1) + 1;
    if (seq > kMaxBacktraceCaptures)
        return; // lost the race right at the cap -- fine, just drop this one

    void* frames[32] = {};
    const USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    const std::string peer = DescribePeer(s);

    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::ostringstream out;
    out << "[" << std::setfill('0') << std::setw(4) << t.wYear << "-" << std::setw(2) << t.wMonth << "-"
        << std::setw(2) << t.wDay << " " << std::setw(2) << t.wHour << ":" << std::setw(2) << t.wMinute << ":"
        << std::setw(2) << t.wSecond << "." << std::setw(3) << t.wMilliseconds << "] "
        << "[" << api << " #" << seq << "] peer=" << peer << " size=" << size << "\n";

    for (size_t offset = 0; offset < size; offset += 16) {
        out << "  " << std::hex << std::setw(6) << std::setfill('0') << offset << "  " << std::dec;
        for (size_t i = 0; i < 16; ++i) {
            if (offset + i < size)
                out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(data[offset + i]) << ' ' << std::dec;
            else
                out << "   ";
        }
        out << " ";
        for (size_t i = 0; i < 16 && offset + i < size; ++i) {
            const uint8_t b = data[offset + i];
            out << static_cast<char>(b >= 32 && b <= 126 ? b : '.');
        }
        out << "\n";
    }

    out << "  frames (game-module RVA, shallow to deep):";
    bool any = false;
    for (USHORT i = 0; i < n; ++i) {
        const ULONG64 addr = reinterpret_cast<ULONG64>(frames[i]);
        if (g_moduleBase && addr >= g_moduleBase && addr < g_moduleBase + g_moduleSize) {
            out << (any ? ", " : " ") << "0x" << std::hex << (addr - g_moduleBase) << std::dec;
            any = true;
        }
    }
    if (!any)
        out << " (none in game module -- frames are entirely inside ws2_32/ntdll/Themida-VM)";
    out << "\n\n";

    std::lock_guard<std::mutex> lock(g_backtraceMutex);
    if (!g_backtraceLog.is_open()) {
        char buf[MAX_PATH];
        GetModuleFileNameA(g_hModule, buf, MAX_PATH);
        std::string path = buf;
        const auto pos = path.find_last_of("\\/");
        if (pos != std::string::npos)
            path = path.substr(0, pos + 1);
        path += "inbound_backtrace_" + std::to_string(GetCurrentProcessId()) + ".log";
        g_backtraceLog.open(path, std::ios::out | std::ios::trunc);
        spdlog::info("[netrecv] capturing up to {} inbound backtraces (or {}s, whichever first) to {}",
                     kMaxBacktraceCaptures, kBacktraceWindowMs / 1000, path);
    }
    if (g_backtraceLog.is_open()) {
        g_backtraceLog << out.str();
        g_backtraceLog.flush();
    }
}

int WSAAPI HkRecv(SOCKET s, char* buf, int len, int flags)
{
    const int result = OrigRecv(s, buf, len, flags);
    if (result > 0) {
        RecordInboundEvent(InboundEventKind::Data, (uintptr_t)s, result,
            reinterpret_cast<const uint8_t*>(buf), (size_t)result);
        // [2026-09-29] Re-enabled: the login failure was net_connect_hook.cpp fail-closing the
        // telemetry connect (34.160.81.0:443), unrelated to this -- see that file's fix and
        // docs/investigation/CONNECTION_DECIPHER_PREP.md. This was never the actual cause.
        CaptureInboundBacktrace("recv", s, reinterpret_cast<const uint8_t*>(buf), (size_t)result);
    } else if (result == 0) {
        RecordInboundEvent(InboundEventKind::GracefulClose, (uintptr_t)s, 0, nullptr, 0);
    } else {
        RecordInboundEvent(InboundEventKind::Error, (uintptr_t)s, WSAGetLastError(), nullptr, 0);
    }
    return result;
}

int WSAAPI HkWSARecv(SOCKET s, LPWSABUF bufs, DWORD bufCount, LPDWORD received,
                      LPDWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    const int result = OrigWSARecv(s, bufs, bufCount, received, flags, ov, cr);

    // Overlapped/async reads (ov != nullptr) don't have their data ready
    // synchronously here — *received is meaningless until the completion
    // routine/IOCP event fires, which this hook doesn't chase (that would
    // need its own Detour on the completion path, out of scope for what
    // this is answering). Only the synchronous case is recorded; if this
    // game's inbound traffic turns out to go entirely through the
    // overlapped path, the inbound ring will simply stay empty rather than
    // recording something misleading — an empty inbound ring in a future
    // dump is itself a signal this assumption needs rechecking, not silent
    // failure.
    if (!ov) {
        if (result == 0 && received && bufs && bufCount > 0) {
            RecordInboundEvent(InboundEventKind::Data, (uintptr_t)s, (int)*received,
                reinterpret_cast<const uint8_t*>(bufs[0].buf), (size_t)*received);
            // [2026-09-29] Re-enabled -- see the matching comment in HkRecv above.
            CaptureInboundBacktrace("WSARecv", s, reinterpret_cast<const uint8_t*>(bufs[0].buf), (size_t)*received);
        } else if (result == SOCKET_ERROR) {
            RecordInboundEvent(InboundEventKind::Error, (uintptr_t)s, WSAGetLastError(), nullptr, 0);
        }
    }
    return result;
}

} // namespace

void InitNetRecvHook()
{
    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2)
        ws2 = LoadLibraryA("ws2_32.dll");
    if (!ws2) {
        spdlog::error("[netrecv] ws2_32.dll not found");
        return;
    }

    OrigRecv = (RecvFn)GetProcAddress(ws2, "recv");
    OrigWSARecv = (WSARecvFn)GetProcAddress(ws2, "WSARecv");
    if (!OrigRecv || !OrigWSARecv) {
        spdlog::error("[netrecv] failed to resolve recv/WSARecv exports");
        OrigRecv = nullptr;
        OrigWSARecv = nullptr;
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG errRecv = DetourAttach(&(PVOID&)OrigRecv, HkRecv);
    LONG errWSARecv = DetourAttach(&(PVOID&)OrigWSARecv, HkWSARecv);
    LONG errCommit = DetourTransactionCommit();

    spdlog::info("[netrecv] recv/WSARecv hooks: recv={} wsarecv={} commit={}",
        errRecv == NO_ERROR ? "OK" : "FAILED",
        errWSARecv == NO_ERROR ? "OK" : "FAILED",
        errCommit == NO_ERROR ? "OK" : "FAILED");
}

void CleanupNetRecvHook()
{
    if (!OrigRecv && !OrigWSARecv)
        return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (OrigRecv)
        DetourDetach(&(PVOID&)OrigRecv, HkRecv);
    if (OrigWSARecv)
        DetourDetach(&(PVOID&)OrigWSARecv, HkWSARecv);
    DetourTransactionCommit();

    OrigRecv = nullptr;
    OrigWSARecv = nullptr;
}
