#include "action_recorder.h"
#include "entities.h"
#include "map_items.h"
#include "game.h"
#include "CHero.h"
#include "CRole.h"
#include "CGameMap.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <spdlog/spdlog.h>

namespace {

constexpr int kRingCapacity = 40;

// Comfortably above the largest legitimate mob-search/loot range anywhere
// in this codebase (see hunt_intervals.h) — this is a "garbage coordinate"
// threshold, not a tuned gameplay distance. A position past this is judged
// implausible regardless of whether the id was found in the fresh scan.
constexpr int kImplausibleDistanceTiles = 300;

struct RecordedAction
{
    DWORD tick = 0;
    RecordedActionType type{};
    OBJID targetId = 0;
    Position targetPos{};
    int distanceFromHero = -1;
    bool presentInFreshScan = false;
    bool positionPlausible = false;
};

std::mutex g_mutex;
std::array<RecordedAction, kRingCapacity> g_ring;
int g_ringHead = 0;
int g_ringCount = 0;
int g_phantomThisSecond = 0;

// [DISCONNECT INVESTIGATION 2026-09-07] Outbound/inbound wire rings — see
// action_recorder.h's comment on why these exist. Small on purpose: the
// question they answer only needs the last handful of events either
// direction, not a trace log.
// [2026-09-07] Bumped from 5/8 to 16/48 — at the observed ~14 inbound
// events/sec, 8 entries was only ~0.5s of history, enough to catch the
// close event itself but not the few seconds of context before it (did
// inbound look normal right up to the close, or stutter/stop first). 48
// entries is ~3.4s at that rate; 16 outbound is a proportional bump for the
// same reason.
constexpr int kOutboundRingCapacity = 16;
constexpr int kInboundRingCapacity = 48;
constexpr size_t kInboundPeekBytes = 8;

struct OutboundEntry
{
    DWORD tick = 0;
    uint16_t msgType = 0;
    uint16_t rawSize = 0;
};

struct InboundEntry
{
    DWORD tick = 0;
    InboundEventKind kind{};
    uintptr_t socketHandle = 0;
    int resultOrError = 0;
    uint8_t peek[kInboundPeekBytes] = {};
    size_t peekLen = 0;
};

std::array<OutboundEntry, kOutboundRingCapacity> g_outRing;
int g_outHead = 0;
int g_outCount = 0;
std::array<InboundEntry, kInboundRingCapacity> g_inRing;
int g_inHead = 0;
int g_inCount = 0;

const char* InboundKindName(InboundEventKind k)
{
    switch (k) {
        case InboundEventKind::Data:          return "Data";
        case InboundEventKind::GracefulClose: return "GracefulClose";
        case InboundEventKind::Error:         return "Error";
    }
    return "?";
}

const char* ActionTypeName(RecordedActionType t)
{
    switch (t) {
        case RecordedActionType::JumpAtMonster: return "JumpAtMonster";
        case RecordedActionType::AttackMonster: return "AttackMonster";
        case RecordedActionType::LootJump:       return "LootJump";
        case RecordedActionType::LootPickup:     return "LootPickup";
        case RecordedActionType::NpcInteract:    return "NpcInteract";
    }
    return "?";
}

bool LooksUpViaEntities(RecordedActionType t)
{
    return t == RecordedActionType::JumpAtMonster
        || t == RecordedActionType::AttackMonster
        || t == RecordedActionType::NpcInteract;
}

} // namespace

void RecordAction(RecordedActionType type, OBJID targetId)
{
    RecordedAction entry;
    entry.tick = GetTickCount();
    entry.type = type;
    entry.targetId = targetId;

    CHero* hero = Game::GetHero();
    CGameMap* map = Game::GetMap();
    const Position heroPos = hero ? hero->m_posMap : Position{};

    // Fresh lookup against whatever is CURRENTLY published — see this
    // file's header comment on why this can't be a passed-in bool.
    if (LooksUpViaEntities(type)) {
        for (CRole* r : Entities::Get()) {
            if (r && r->GetID() == targetId) {
                entry.presentInFreshScan = true;
                entry.targetPos = r->m_posMap;
                break;
            }
        }
    } else {
        for (CMapItem* item : MapItems::Get()) {
            if (item && item->m_id == targetId) {
                entry.presentInFreshScan = true;
                entry.targetPos = item->m_pos;
                break;
            }
        }
    }

    if (entry.presentInFreshScan) {
        entry.distanceFromHero = hero
            ? CGameMap::TileDist(heroPos.x, heroPos.y, entry.targetPos.x, entry.targetPos.y)
            : -1;
        const bool inBounds = map
            && entry.targetPos.x > 0 && entry.targetPos.y > 0
            && entry.targetPos.x < map->m_sizeMap.iWidth && entry.targetPos.y < map->m_sizeMap.iHeight;
        const bool isZero = entry.targetPos.x == 0 && entry.targetPos.y == 0;
        const bool tooFar = entry.distanceFromHero >= 0 && entry.distanceFromHero > kImplausibleDistanceTiles;
        entry.positionPlausible = inBounds && !isZero && !tooFar;
    } else {
        entry.positionPlausible = false;   // absent entirely — nothing to judge plausible
    }

    std::lock_guard<std::mutex> lk(g_mutex);
    g_ring[g_ringHead] = entry;
    g_ringHead = (g_ringHead + 1) % kRingCapacity;
    if (g_ringCount < kRingCapacity)
        ++g_ringCount;
    if (!entry.presentInFreshScan || !entry.positionPlausible)
        ++g_phantomThisSecond;
}

void DumpFlightRecorder()
{
    // Self-guarding: two independent teardown signals can both call this
    // for the same disconnect (see the header comment on why neither one
    // alone is a universal trigger) — dump on whichever fires first, and
    // make the second call an inert no-op rather than a duplicate dump.
    static bool alreadyDumped = false;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (alreadyDumped)
        return;
    alreadyDumped = true;

    const DWORD now = GetTickCount();
    spdlog::warn("[flightrec] dumping last {} action(s) before disconnect", g_ringCount);
    const int start = (g_ringHead - g_ringCount + kRingCapacity) % kRingCapacity;
    for (int i = 0; i < g_ringCount; ++i) {
        const RecordedAction& e = g_ring[(start + i) % kRingCapacity];
        spdlog::warn("[flightrec] -{}ms type={} targetId={} pos=({},{}) distFromHero={} presentInFreshScan={} positionPlausible={}",
            now - e.tick, ActionTypeName(e.type), e.targetId,
            e.targetPos.x, e.targetPos.y, e.distanceFromHero,
            e.presentInFreshScan, e.positionPlausible);
    }

    spdlog::warn("[flightrec][outbound] dumping last {} packet(s) actually sent", g_outCount);
    const int outStart = (g_outHead - g_outCount + kOutboundRingCapacity) % kOutboundRingCapacity;
    for (int i = 0; i < g_outCount; ++i) {
        const OutboundEntry& e = g_outRing[(outStart + i) % kOutboundRingCapacity];
        spdlog::warn("[flightrec][outbound] -{}ms msgType=0x{:X} size={}",
            now - e.tick, e.msgType, e.rawSize);
    }

    spdlog::warn("[flightrec][inbound] dumping last {} socket event(s), any socket in-process", g_inCount);
    const int inStart = (g_inHead - g_inCount + kInboundRingCapacity) % kInboundRingCapacity;
    for (int i = 0; i < g_inCount; ++i) {
        const InboundEntry& e = g_inRing[(inStart + i) % kInboundRingCapacity];
        if (e.kind == InboundEventKind::Data) {
            uint16_t peekMsgType = 0;
            if (e.peekLen >= 4)
                peekMsgType = static_cast<uint16_t>(e.peek[2]) | (static_cast<uint16_t>(e.peek[3]) << 8);
            std::string hex;
            char byteBuf[4];
            for (size_t b = 0; b < e.peekLen; ++b) {
                snprintf(byteBuf, sizeof(byteBuf), "%s%02X", b ? "," : "", e.peek[b]);
                hex += byteBuf;
            }
            spdlog::warn("[flightrec][inbound] -{}ms kind={} sock=0x{:X} bytes={} peekMsgType(best-effort)=0x{:X} peek=[{}]",
                now - e.tick, InboundKindName(e.kind), e.socketHandle, e.resultOrError, peekMsgType, hex);
        } else if (e.kind == InboundEventKind::GracefulClose) {
            spdlog::warn("[flightrec][inbound] -{}ms kind={} sock=0x{:X} -- peer sent FIN",
                now - e.tick, InboundKindName(e.kind), e.socketHandle);
        } else {
            spdlog::warn("[flightrec][inbound] -{}ms kind={} sock=0x{:X} wsaError={}",
                now - e.tick, InboundKindName(e.kind), e.socketHandle, e.resultOrError);
        }
    }
}

void RecordOutboundPacket(uint16_t msgType, uint16_t rawSize)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    OutboundEntry& e = g_outRing[g_outHead];
    e.tick = GetTickCount();
    e.msgType = msgType;
    e.rawSize = rawSize;
    g_outHead = (g_outHead + 1) % kOutboundRingCapacity;
    if (g_outCount < kOutboundRingCapacity)
        ++g_outCount;
}

void RecordInboundEvent(InboundEventKind kind, uintptr_t socketHandle, int resultOrError,
    const uint8_t* peekBytes, size_t peekLen)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    InboundEntry& e = g_inRing[g_inHead];
    e.tick = GetTickCount();
    e.kind = kind;
    e.socketHandle = socketHandle;
    e.resultOrError = resultOrError;
    e.peekLen = peekLen < kInboundPeekBytes ? peekLen : kInboundPeekBytes;
    if (peekBytes && e.peekLen > 0)
        memcpy(e.peek, peekBytes, e.peekLen);
    g_inHead = (g_inHead + 1) % kInboundRingCapacity;
    if (g_inCount < kInboundRingCapacity)
        ++g_inCount;
}

int ConsumePhantomActionCountThisSecond()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    const int v = g_phantomThisSecond;
    g_phantomThisSecond = 0;
    return v;
}
