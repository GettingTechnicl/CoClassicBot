#pragma once
#include "base.h"
#include <cstdint>

// =====================================================================
// action_recorder.h — flight recorder for the trojan-disconnect
// investigation (2026-09-06). Read-only measurement: records the last
// several committed actions (jump-at-monster, attack, loot-jump,
// loot-pickup, npc-interact) so the moments right before a disconnect can
// be inspected after the fact, without needing trace-level logging running
// continuously (which itself throttles the real action cadence via
// log.cpp's flush_on(debug) — see the investigation report this responds
// to).
//
// Two properties this exists specifically to guarantee, per direct user
// requirement — get these wrong and the recorder always reports "looked
// fine":
//
//   1. FRESH RE-QUERY AT THE COMMIT INSTANT. RecordAction() does its own
//      lookup against Entities::Get() / MapItems::Get() — the live,
//      possibly-just-published snapshot — at the moment it's called. It
//      deliberately does NOT accept a caller-supplied "is this still
//      alive" bool, because that value is typically computed once near the
//      top of a decision tick and then carried through several more lines
//      (sometimes several more TICKS, across an in-flight jump) before the
//      actual packet goes out — reusing it would just echo back "alive"
//      every time regardless of what happened in between.
//
//   2. POSITION PLAUSIBILITY, independent of presence. A heap-scan ghost
//      (freed-and-reused memory that still passes the CRole/CMapItem shape
//      check) can have a real-looking id and still be sitting on garbage
//      coordinates — off the map entirely, exactly (0,0), or absurdly far
//      from the hero. A bare "is the id present" check cannot see this, so
//      it's recorded as an independent flag.
//
// Zero behavior change: nothing here is ever read by a decision — only
// written to and dumped to the log.
// =====================================================================

enum class RecordedActionType : uint8_t
{
    JumpAtMonster,
    AttackMonster,
    LootJump,
    LootPickup,
    NpcInteract,
};

// Call this IMMEDIATELY adjacent to the actual commit (the line that sends
// the jump/attack/pickup/interact packet) — not earlier in the same
// function, and never with a bool/pointer validity result computed
// earlier in the tick. Looks the id up fresh right now.
void RecordAction(RecordedActionType type, OBJID targetId);

// Dump the whole ring buffer to the log at WARN, oldest first, each line
// annotated with how many ms before "now" that action was recorded.
//
// Self-guarding: only the FIRST call in the process's lifetime actually
// dumps anything; every call after that is an inert no-op. This exists
// because there is no single universal disconnect signal — classifying 6
// real disconnects (2026-09-06) split them 3-and-3: three showed
// SendPacket() detecting a null connection object and the process staying
// alive for several more seconds (entity scans kept running); three others
// never logged that line at all, or logged it with zero further activity —
// a harder, faster process death. What EVERY one of the 6 shares is
// CHero::GetMaxHp() falling back and logging its own one-time "RVA
// unverified" warning (hero's cached stats reading 0 for the first time all
// session) — so that call site fires this too, and this one fires as a
// backup in case a future disconnect reaches SendPacket() first instead.
// Whichever happens first wins; the guard above makes the second one a
// no-op rather than a duplicate dump.
void DumpFlightRecorder();

// Count of RecordAction() calls in the current one-second window judged a
// phantom (target missing from the fresh scan, OR present but at an
// implausible position). Read-and-reset — call once per second from the
// [actionrate] summary line in packets.cpp.
int ConsumePhantomActionCountThisSecond();

// =====================================================================
// [DISCONNECT INVESTIGATION 2026-09-07] Outbound/inbound wire rings.
//
// The overnight 2026-09-06 run (first with a working DumpFlightRecorder())
// ruled out phantom-target actions (0 of ~150 recorded actions across 8
// captured disconnects were implausible/absent) and weakened the rate
// theory further (the character with the higher average packet rate had
// the GENTLER disconnect signature, backwards from what a rate-kick theory
// predicts). Crashpad produced zero .dmp files that night either, so it's
// not a process fault. What's left standing is a server-side connection
// loss — but the one open question the action ring above can't answer is
// whether the SERVER sent an explicit close/kick, or the connection just
// silently died: the action ring only sees what WE decided to commit, not
// what actually crossed the wire in either direction. These two small
// rings exist to answer exactly that, dumped from the same GetMaxHp
// sentinel as DumpFlightRecorder() above — no new logging cadence, no new
// I/O-throttle risk (see DumpFlightRecorder()'s own comment on why a
// once-per-disconnect dump was chosen over continuous line-by-line
// logging).
// =====================================================================

// Called from packets.cpp's TrackOutgoingPacket() — already hooked into
// every real outbound send via the CNetClient::SendMsg Detour (HkSendMsgReal),
// so this sees the true wire traffic, not just what this bot's own
// jump/walk helpers issued.
void RecordOutboundPacket(uint16_t msgType, uint16_t rawSize);

enum class InboundEventKind : uint8_t
{
    Data,           // recv/WSARecv returned > 0 bytes
    GracefulClose,  // recv/WSARecv returned 0 — the PEER (server) sent a FIN.
                     // This is the single strongest "the server actually
                     // decided to close this connection" signal available
                     // without a full protocol decode.
    Error,          // recv/WSARecv failed — see the WSAGetLastError() value
                     // recorded in resultOrError (e.g. WSAECONNRESET = an
                     // abrupt reset rather than an orderly close).
};

// Called from the new recv/WSARecv Detour hook (net_recv_hook.cpp). Records
// on ANY socket in this process — there's no cheap way from here to filter
// to just the game's own connection socket, and recording a little more is
// harmless since this is observation-only (never touches the buffer, return
// value, or error code the real recv/WSARecv call produces). `peekBytes`/
// `peekLen` are a best-effort look at the first few received bytes for Data
// events — NOT guaranteed to align to one real packet's header, since TCP
// is a byte stream: a single recv() can return a partial packet, several
// concatenated packets, or the tail end of one split across two calls. Any
// msgType read back out of this in the dump is a hint, not a certainty —
// unlike the outbound ring, which sees whole packets by construction.
void RecordInboundEvent(InboundEventKind kind, uintptr_t socketHandle, int resultOrError,
    const uint8_t* peekBytes, size_t peekLen);
