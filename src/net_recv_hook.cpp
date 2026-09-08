#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include "net_recv_hook.h"
#include "action_recorder.h"
#include <detours.h>
#include <spdlog/spdlog.h>

namespace {

typedef int (WSAAPI* RecvFn)(SOCKET, char*, int, int);
typedef int (WSAAPI* WSARecvFn)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD,
                                 LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

RecvFn OrigRecv = nullptr;
WSARecvFn OrigWSARecv = nullptr;

int WSAAPI HkRecv(SOCKET s, char* buf, int len, int flags)
{
    const int result = OrigRecv(s, buf, len, flags);
    if (result > 0) {
        RecordInboundEvent(InboundEventKind::Data, (uintptr_t)s, result,
            reinterpret_cast<const uint8_t*>(buf), (size_t)result);
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
