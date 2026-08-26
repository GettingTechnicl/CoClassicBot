#include "packets.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include <detours.h>

static PacketLog g_packetLog;
PacketLog& GetPacketLog() { return g_packetLog; }
static DWORD g_lastVipTeleportTick = 0;
DWORD GetLastVipTeleportTick() { return g_lastVipTeleportTick; }
bool IsVipTeleportOnCooldown(DWORD cooldownMs)
{
    return g_lastVipTeleportTick != 0 && GetTickCount() - g_lastVipTeleportTick < cooldownMs;
}

namespace {
constexpr uint16_t kMsgActionPacketType = 0x03F2;
constexpr uint32_t kMsgActionModeJump = 19;
// Session 10 [LIVE-CONFIRMED]: field #1 of 7 real captured walk packets
// (see coclassicbot-live-offsets memory, session 10 cont. 7/8) — the user
// manually walked 5 times and every packet decoded to mode=84, with fields
// #4/#5 matching the real destination tile in 4/5 samples exactly and the
// other 2 off by one tile (consistent with eyeballing the report, not a
// field misread).
constexpr uint32_t kMsgActionModeWalk = 84;
// Field #8 (tag8) — present and IDENTICAL (193661) across all 7 walk
// captures regardless of position, direction, or time elapsed. Purpose
// unconfirmed (not distance/time/direction-dependent), but its constancy
// across a real session makes hardcoding it a reasonable starting point.
// If walk packets get rejected after a fresh login/reconnect, re-verify this
// value hasn't changed (capture a walk packet the same way and compare)
// before assuming something else is wrong.
constexpr uint32_t kWalkTag8Value = 193661;

struct MsgActionPacket
{
    uint32_t mode = 0;
    uint32_t id = 0;
    uint32_t timestamp = 0;
    uint32_t data1 = 0;
    uint32_t data2 = 0;
    uint32_t facing = 0;
    uint32_t tag7 = 0;
    uint32_t tag8 = 0;
    uint32_t data3 = 0;
    uint32_t data4 = 0;
};

uint32_t g_jumpSpeedTimer = 0;
static DWORD g_lastSpeedHackJumpTick = 0;
constexpr DWORD kSpeedTimerResetGapMs = 1000;

static int WriteVarint(uint8_t* buf, uint32_t value)
{
    int n = 0;
    while (value > 0x7F) {
        buf[n++] = static_cast<uint8_t>(value & 0x7F) | 0x80;
        value >>= 7;
    }
    buf[n++] = static_cast<uint8_t>(value);
    return n;
}

static bool ReadVarint(const uint8_t* data, size_t size, size_t& off, uint32_t& value)
{
    value = 0;
    int shift = 0;
    while (off < size && shift <= 28) {
        const uint8_t byte = data[off++];
        value |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0)
            return true;
        shift += 7;
    }
    return false;
}

} // end anonymous namespace

// Session 10: generic decoder for the tag/varint field encoding every
// MsgAction-style outgoing packet uses (see BuildMsgActionPacket below —
// tag byte = field_number << 3, wire type 0/varint only, matches what's
// actually observed on the wire here). Field numbers, not raw tag bytes, so
// callers don't have to re-derive the shift themselves. Used by the overlay's
// Packets tab so a captured packet's fields are readable without hand-parsing
// hex — this is what made finding walk's real MsgAction mode value practical.
std::vector<DecodedField> DecodeVarintFields(const uint8_t* data, size_t size)
{
    std::vector<DecodedField> out;
    if (!data || size <= 4)
        return out;

    size_t off = 4; // skip the 2-byte size + 2-byte type header
    while (off < size) {
        const uint8_t tag = data[off++];
        const uint32_t fieldNumber = tag >> 3;
        const uint32_t wireType = tag & 0x07;
        if (wireType != 0) // only varint fields are used by this packet family
            break;
        uint32_t value = 0;
        if (!ReadVarint(data, size, off, value))
            break;
        out.push_back({ (int)fieldNumber, value });
    }
    return out;
}

static void ApplyLocalJumpPrediction(const MsgActionPacket& packet)
{
    CHero* hero = Game::GetHero();
    if (!hero) {
        spdlog::warn("[prediction] No hero");
        return;
    }
    if (hero->GetID() != packet.id) {
        spdlog::warn("[prediction] ID mismatch: hero={} packet={}", hero->GetID(), packet.id);
        return;
    }

    const Position origin = hero->m_posMap;
    const Position destination(static_cast<int>(packet.data3), static_cast<int>(packet.data4));

    spdlog::debug("[prediction] ({},{}) -> ({},{}) ts={} timer={}",
        origin.x, origin.y, destination.x, destination.y,
        packet.timestamp, g_jumpSpeedTimer);

    // ── Session 9: rewritten. This function previously did four things that
    // were each independently confirmed broken on v1074:
    //
    //  1. Wrote m_cmdAction directly with iStatus=_CMDSTATUS_ACCOMPLISH. Live
    //     captures of REAL SetCommand calls show iStatus is always 0/BEGIN on
    //     input — ACCOMPLISH is a value the game's per-tick dispatcher only
    //     ever PRODUCES. Feeding it in crashed the client repeatedly.
    //  2. Wrote m_posMap/m_posWorld without m_posMoveStart/m_posMoveDest, so
    //     the game reverted the change within a frame regardless.
    //  3. Derived world coords from CGameMap::m_posCameraPos — CGameMap's
    //     offsets are stale on v1074 (see game.h), so this was reading
    //     garbage even when it did run.
    //  4. Clobbered m_qwRuntimeA30, which on v1074 is the player's SILVER
    //     field (live-verified =7681), with a computed timestamp.
    //
    // Note the ORIGINAL transform ((x-y)*32, (x+y)*16) was correct — it
    // matches the isometric fit independently rederived this session from 212
    // live samples. Only the per-map origin it added was unobtainable. Anchoring
    // on the hero's current position instead cancels that origin out entirely,
    // so no map object is needed. All of the above now collapses to one call:
    hero->SyncClientPosition(destination.x, destination.y);
}

static size_t BuildMsgActionPacket(const MsgActionPacket& packet, uint8_t* buf, size_t capacity)
{
    if (!buf || capacity < 32)
        return 0;

    int off = 4;
    buf[off++] = 0x08;
    off += WriteVarint(buf + off, packet.mode);
    buf[off++] = 0x10;
    off += WriteVarint(buf + off, packet.id);
    buf[off++] = 0x18;
    off += WriteVarint(buf + off, packet.timestamp);
    buf[off++] = 0x20;
    off += WriteVarint(buf + off, packet.data1);
    buf[off++] = 0x28;
    off += WriteVarint(buf + off, packet.data2);
    buf[off++] = 0x30;
    off += WriteVarint(buf + off, packet.facing);
    if (packet.tag7 != 0) {
        buf[off++] = 0x38;
        off += WriteVarint(buf + off, packet.tag7);
    }
    if (packet.tag8 != 0) {
        buf[off++] = 0x40;
        off += WriteVarint(buf + off, packet.tag8);
    }
    // Session 10: made conditional (was unconditional) to match a real,
    // live-captured walk packet exactly — walk's destination lives in
    // data1/data2 (fields 4/5, confirmed against 5 real hero moves) and it
    // never emits fields 10/11 at all. Jump always sets real non-zero
    // destX/destY here, so this is a no-op for jump in practice.
    if (packet.data3 != 0) {
        buf[off++] = 0x50;
        off += WriteVarint(buf + off, packet.data3);
    }
    if (packet.data4 != 0) {
        buf[off++] = 0x58;
        off += WriteVarint(buf + off, packet.data4);
    }

    *(uint16_t*)buf = static_cast<uint16_t>(off);
    *(uint16_t*)(buf + 2) = kMsgActionPacketType;
    return static_cast<size_t>(off);
}

static void TrackOutgoingPacket(const uint8_t* data, size_t size)
{
    if (!data || size < 4)
        return;

    const uint16_t msgType = *(const uint16_t*)(data + 2);
    if (msgType == 0x1B5C) {
        g_lastVipTeleportTick = GetTickCount();
        spdlog::debug("[packets] VIP teleport packet sent");
    }

    if (g_packetLog.enabled) {
        PacketEntry entry;
        entry.tick    = GetTickCount();
        entry.msgSize = *(const uint16_t*)(data);
        entry.msgType = msgType;
        entry.rawSize = (uint16_t)size;
        entry.data.assign(data, data + size);
        g_packetLog.Push(entry);
    }
}

// =====================================================================
// CNetClient::SendMsg (real) hook — session 10
//
// Session 8 confirmed GameRva::CNETCLIENT_SEND_MSG_REAL (0x1DD450) is the
// actual outgoing-packet function (SendPacket() below has been sending real
// packets through it since session 8). The OLD hook in this file targeted
// GameRva::CNETCLIENT_SEND_MSG (0x18EEA0) — a stale, never-verified RVA —
// and was permanently gated off by VERIFIED_V1074, so packet capture has
// never actually seen anything except packets THIS bot sent itself (via the
// explicit TrackOutgoingPacket() call inside SendPacket() below). It has
// never seen a single packet the game sends natively — including a manual,
// player-driven walk — which is exactly the data needed to find walk's real
// MsgAction mode value. Detouring the CONFIRMED address is a fundamentally
// different risk profile than the old gate was protecting against (hooking
// an address independently proven correct, not a guess), so this replaces
// the dead hook rather than adding a second one.
// =====================================================================
static GameCall::CNetClient_SendMsgRealFn OrigSendMsgReal = nullptr;

static uint8_t HkSendMsgReal(void* connectionObject, const uint8_t* data, uint32_t size)
{
    TrackOutgoingPacket(data, (size_t)size);
    return OrigSendMsgReal(connectionObject, data, size);
}

// =====================================================================
// Init / Cleanup
// =====================================================================
void InitPacketHook()
{
    OrigSendMsgReal = GameCall::CNetClient_SendMsgReal();
    uintptr_t addr = reinterpret_cast<uintptr_t>(OrigSendMsgReal);
    if (!addr) {
        spdlog::error("[packets] InitPacketHook: failed to resolve CNetClient::SendMsg (real)");
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)OrigSendMsgReal, HkSendMsgReal);
    LONG err = DetourTransactionCommit();

    spdlog::info("[packets] SendMsg (real) @ 0x{:X}: {}", addr, err == NO_ERROR ? "OK" : "FAILED");
}

void CleanupPacketHook()
{
    if (!OrigSendMsgReal) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)OrigSendMsgReal, HkSendMsgReal);
    DetourTransactionCommit();

    OrigSendMsgReal = nullptr;
    spdlog::info("[packets] Hook removed");
}

// Session 8 [LIVE-CONFIRMED]: routes through the real CNetClient::SendMsg
// (GameRva::CNETCLIENT_SEND_MSG_REAL / 0x1DD450), found via a send()
// backtrace sweep on real traffic — completely supersedes the old
// OrigSendMsg/g_netClient/VERIFIED_V1074-gated Detour path above, which
// targeted a stale, garbage RVA and never actually installed (kept in
// place, still inert, in case its packet-observation hook is useful again
// later, but SendPacket() no longer depends on it). `connectionObject` is
// resolved fresh every call (it's a per-session pointer, not stable across
// relaunches/reconnects — see GameCall::ResolveConnectionObject()).
bool SendPacket(const uint8_t* data, size_t size)
{
    void* conn = GameCall::ResolveConnectionObject();
    if (!conn) {
        static bool warned = false;
        if (!warned) { spdlog::error("[packets] SendPacket: failed to resolve connection object"); warned = true; }
        return false;
    }

    auto sendMsg = GameCall::CNetClient_SendMsgReal();
    if (!sendMsg)
        return false;

    // Session 10: no explicit TrackOutgoingPacket() call needed here anymore —
    // InitPacketHook() now Detours this exact function (see above), so this
    // call already gets captured there. An explicit call here too would
    // double-log every packet this bot sends.
    const uint8_t ok = sendMsg(conn, data, static_cast<uint32_t>(size));
    return ok != 0;
}

// Session 8: `applyLocalPrediction` (default true, matching all pre-existing
// callers' behavior) — ApplyLocalJumpPrediction() writes directly into several
// CHero struct fields (m_cmdAction, m_posWorld, m_posScr, m_qwRuntimeA30).
// This code has existed for a long time but was NEVER actually exercised on
// v1074 until SendPacket() started genuinely working this session (it was
// previously a guaranteed no-op, so SendJumpPacket() always returned false
// before reaching this call). A live debug test crashed the game immediately
// after this ran for the first time — the crash signature ("unknown" module,
// huge fault offset) matches jumping through a corrupted pointer, consistent
// with these struct offsets not actually being correct for this client build.
// NOT YET RE-VERIFIED for v1074 — every existing caller (including
// CHero::Jump()'s packet-jump branch, if `usePacketJump` is enabled in hunt/
// travel settings) is exposed to this same risk now that it can actually run.
// Pass false to skip it entirely (used by the debug-only pickup test to
// isolate SendMsg verification from this separate, still-unverified risk).
bool SendJumpPacket(OBJID heroId, int destX, int destY, int facing, bool applyLocalPrediction)
{
    const uint32_t now = GetTickCount();
    if (now - g_lastSpeedHackJumpTick >= kSpeedTimerResetGapMs)
        g_jumpSpeedTimer = 0;

    MsgActionPacket packet = {};
    packet.mode = kMsgActionModeJump;
    packet.id = heroId;
    packet.timestamp = now + g_jumpSpeedTimer;
    packet.facing = static_cast<uint32_t>(facing);
    packet.data3 = static_cast<uint32_t>(destX);
    packet.data4 = static_cast<uint32_t>(destY);

    g_jumpSpeedTimer += 5000;
    g_lastSpeedHackJumpTick = now;

    uint8_t buf[64] = {};
    const size_t sz = BuildMsgActionPacket(packet, buf, sizeof(buf));
    if (sz == 0)
    {
        spdlog::error("Failed to build packet");
        return false;
    }

    if (!SendPacket(buf, sz))
    {
        spdlog::error("Failed to send packet");
        return false;
    }

    if (applyLocalPrediction)
        ApplyLocalJumpPrediction(packet);
    return true;
}

// Session 10 [ROOT CAUSE CONFIRMED, live-verified working]: the crash was
// GetGameTimestampMs() — a call into the game's OWN native code (RVA
// 0x0C6A80) — not SetCommand() as first suspected. It was wrongly assumed
// safe because it's already called from CHero::RefreshSilverCache(), but
// that call is gated behind `if (!trusted)`, which may rarely execute in
// practice — "called elsewhere in source" isn't the same as "proven to run".
// Switched to GetTickCount(), the same timestamp source jump already sends
// successfully every time, removing the native call entirely. Confirmed
// live: works reliably, including long-distance walks via the debug button.
bool SendWalkPacket(OBJID heroId, int destX, int destY, int facing)
{
    MsgActionPacket packet = {};
    packet.mode = kMsgActionModeWalk;
    packet.id = heroId;
    packet.timestamp = static_cast<uint32_t>(GetTickCount());
    packet.data1 = static_cast<uint32_t>(destX);
    packet.data2 = static_cast<uint32_t>(destY);
    packet.facing = static_cast<uint32_t>(facing);
    packet.tag8 = kWalkTag8Value;

    uint8_t buf[64] = {};
    const size_t sz = BuildMsgActionPacket(packet, buf, sizeof(buf));
    if (sz == 0) {
        spdlog::error("[walk] Failed to build packet");
        return false;
    }
    if (!SendPacket(buf, sz)) {
        spdlog::error("[walk] Failed to send packet");
        return false;
    }
    return true;
}
