#pragma once

// =====================================================================
// net_recv_hook.h — [DISCONNECT INVESTIGATION 2026-09-07] inbound half of
// the wire-observation pair described in action_recorder.h. Detour-hooks
// ws2_32.dll's `recv`/`WSARecv` exports so every inbound socket read in
// this process feeds action_recorder's inbound ring (RecordInboundEvent) —
// the piece that can tell "server sent an explicit close/kick" (recv
// returns 0 = peer FIN) apart from "connection just silently died"
// (WSAECONNRESET or nothing further at all).
//
// Same class of technique netfinder.cpp already uses for the OUTBOUND side
// (Detour a known Winsock export rather than touch game code, which avoids
// the disconnect/detection risk hardware breakpoints or game-code patches
// carry) — this is simpler, since it only needs recv's own inputs/result,
// not a stack backtrace or raw arg dump. Observation-only: never modifies
// the buffer, return value, or error code handed back to the game's own
// network code.
// =====================================================================

void InitNetRecvHook();
void CleanupNetRecvHook();
