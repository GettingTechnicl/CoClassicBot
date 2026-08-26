#include "CHero.h"
#include "CGameMap.h"
#include "config.h"
#include "game.h"
#include "map_items.h"
#include "gateway.h"
#include "packets.h"
#include "hunt_settings.h"
#include "plugins/plugin_mgr.h"
#include "plugins/travel_plugin.h"
#include "log.h"
#include <cstring>

namespace {
constexpr uint16_t kMsgActionPacketType = 0x03F2;
constexpr uint16_t kMsgMapItemPacketType = 0x044D;
constexpr uint32_t kMsgActionModeRevive = 25;
constexpr uint32_t kMsgMapItemModePickup = 3;
constexpr uint16_t kMsgVipPacketType = 0x1B5C;
constexpr uint16_t kMsgTradePacketType = 0x0420;
constexpr uint32_t kMsgVipModeTeleport = 1;
constexpr uint32_t kVipDestinationTwinCity = 1;
constexpr uint32_t kVipDestinationPhoenixCastle = 2;
constexpr uint32_t kVipDestinationApeMountain = 3;
constexpr uint32_t kVipDestinationDesertCity = 4;
constexpr uint32_t kVipDestinationBirdIsland = 5;
constexpr uint32_t kTradeModeStart = 1;
constexpr uint32_t kTradeModeCancel = 2;
constexpr uint32_t kTradeModeAddItem = 6;
constexpr uint32_t kTradeModeAccept = 10;
constexpr uint64_t kSilverTimestampWindowMs = 5000;
constexpr uint32_t kMaxInitialUntrustedSilver = 100000000;
constexpr uint32_t kMaxUntrustedSilverDelta = 5000000;

using GetTimestampFn = long long*(*)(long long*);

struct SilverCacheState {
    OBJID heroId = 0;
    uint32_t value = 0;
    bool hasValue = false;
    bool isTrusted = false;
};

SilverCacheState g_silverCache;

void ResetSilverCacheForHero(OBJID heroId)
{
    if (g_silverCache.heroId == heroId)
        return;

    g_silverCache = {};
    g_silverCache.heroId = heroId;
}

void StoreSilverCache(OBJID heroId, uint32_t value, bool trusted)
{
    ResetSilverCacheForHero(heroId);
    if (g_silverCache.isTrusted && !trusted)
        return;

    g_silverCache.value = value;
    g_silverCache.hasValue = true;
    g_silverCache.isTrusted = trusted;
}

bool IsPlausibleUntrustedSilverValue(uint32_t candidate)
{
    if (!g_silverCache.hasValue)
        return candidate <= kMaxInitialUntrustedSilver;

    const uint32_t current = g_silverCache.value;
    const uint32_t delta = (candidate > current) ? (candidate - current) : (current - candidate);
    return delta <= kMaxUntrustedSilverDelta;
}

// Session 10 [SAFETY]: 0x0C6A80 is CONFIRMED to resolve to a function
// epilogue on v1074, not a real entry point (see game.h's GameRva comment —
// this is the exact same address whose call crashed CHero::Walk()'s first
// packet-send attempt this session; packets.cpp's fix was to stop calling it
// entirely). This copy is a separate, still-live landmine: RefreshSilverCache
// below only avoids it today because its one call site happens to pass
// trusted=true, but the function's OWN default parameter is false — the next
// new `hero->RefreshSilverCache()` call with no arguments reintroduces the
// identical crash. SEH-guarded rather than removed (unlike packets.cpp) since
// this call's caller doesn't have an equally-simple safe substitute in hand;
// this at least converts "call it now" from a guaranteed crash into a
// graceful 0.
namespace {
// The static-local's thread-safe init guard above conflicts with __try
// living in the same function (MSVC C2712), so the guarded call itself has
// to be a separate function with no such local.
bool TryCallGameTimestamp(GetTimestampFn fn, long long* outNs)
{
    __try {
        fn(outNs);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}

uint64_t GetGameTimestampMs()
{
    static auto fn = Game::Resolve<GetTimestampFn>(0x0C6A80);
    if (!fn)
        return 0;

    long long timestampNs = 0;
    if (!TryCallGameTimestamp(fn, &timestampNs))
        return 0;
    return static_cast<uint64_t>(timestampNs / 1000000);
}

bool LooksLikeMovementTimestamp(uint64_t value, uint64_t nowMs)
{
    return nowMs != 0 && value <= nowMs && nowMs - value <= kSilverTimestampWindowMs;
}

uint32_t GetVipDestinationForMap(OBJID mapId)
{
    switch (mapId) {
        case MAP_TWIN_CITY:      return kVipDestinationTwinCity;
        case MAP_PHOENIX_CASTLE: return kVipDestinationPhoenixCastle;
        case MAP_APE_MOUNTAIN:   return kVipDestinationApeMountain;
        case MAP_DESERT_CITY:    return kVipDestinationDesertCity;
        case MAP_BIRD_ISLAND:    return kVipDestinationBirdIsland;
        default:                 return 0;
    }
}
}

const char* GetEquipSlotName(int slot)
{
    switch (slot) {
        case EquipSlot::HEAD:     return "Head";
        case EquipSlot::NECKLACE: return "Necklace";
        case EquipSlot::ARMOR:    return "Armor";
        case EquipSlot::RWEAPON:  return "R.Hand";
        case EquipSlot::LWEAPON:  return "L.Hand";
        case EquipSlot::RING:     return "Ring";
        case EquipSlot::GARMENT:  return "Garment";
        case EquipSlot::BOOTS:    return "Boots";
        default:                  return "Unknown";
    }
}

CHero* CHero::GetSingletonPtr()
{
    auto* mgr = Game::GetRoleMgr();
    if (!mgr || !mgr->m_pHero) return nullptr;
    return mgr->m_pHero;
}

static int WriteVarint(uint8_t* buf, uint32_t value);

void CHero::Jump(int nX, int nY)
{
    const AutoHuntSettings& ah = GetAutoHuntSettings();
    const TravelSettings& ts = GetTravelSettings();

    // Check if either hunt or travel wants packet jump
    const bool travelActive = [&]() {
        if (auto* tp = PluginManager::Get().GetPlugin<TravelPlugin>())
            return tp->IsTraveling();
        return false;
    }();
    const bool wantPacketJump = ah.usePacketJump || (travelActive && ts.usePacketJump);

    if (!wantPacketJump) {
        if (!GameRva::VERIFIED_V1074) {
            static bool warned = false;
            if (!warned) { spdlog::error("[safety] CHero::Jump: native-jump RVA unverified on v1074, forcing packet jump"); warned = true; }
            // Session 9 [FIXED]: this used SendJumpPacket's DEFAULT
            // applyLocalPrediction=true, which runs ApplyLocalJumpPrediction()
            // — the function that overwrites m_qwRuntimeA30, independently
            // verified on v1074 to be the player's SILVER field. Since
            // VERIFIED_V1074 is false, this fallback is the path actually
            // taken, so that corruption was live, not hypothetical.
            if (SendJumpPacket(GetID(), nX, nY, 0, /*applyLocalPrediction=*/false))
                SyncClientPosition(nX, nY);
            return;
        }
        GameCall::CHero_Jump()(this, nX, nY);
        return;
    }

    // Suppress packet jump when other players are nearby — fall back to
    // the native jump so the game plays a normal animation.
    if (AreOtherPlayersNearby(GetID(), ah.playerWhitelist) && GameRva::VERIFIED_V1074) {
        GameCall::CHero_Jump()(this, nX, nY);
    } else {
        // ── Session 9 [SUPERSEDED — read this before touching the below] ──
        // Everything in the historical commentary that follows describes the
        // CCommand/SetCommand prediction approach. That approach is DEAD: it
        // crashed the game five separate times across three independently
        // real, independently evidenced fixes (wrong vtable slot -> CCommand
        // not zero-initialized -> wrong iStatus), and was ultimately found to
        // be unnecessary in the first place.
        //
        // What was actually wrong: nothing about the jump itself. The server
        // accepts a packet-only move just fine (verified by picking up an
        // item several blocks away after one); it simply never echoes a
        // position update back to the client that originated the move. So the
        // only real problem was a CLIENT-side desync, and the fix is to write
        // the client's own movement bookkeeping directly -- three plain data
        // writes, no native call, no CCommand, no vtable, nothing gated.
        // See CRole::SyncClientPosition().
        //
        // The old commentary is retained below because the reasoning trail is
        // useful, not because the code it describes should come back.
        //
        // ── historical ──
        // SetCommand (the game's own native function, already proven safe
        // elsewhere in this codebase) tells the game a jump is happening so
        // it doesn't reset m_posMap on the next frame — this alone is what
        // provides local prediction.
        //
        // Session 9 [FIXED]: this used to ALSO write `m_qwRuntimeA30 = nowMs
        // > 5000 ? (nowMs - 5000) : 0` directly and call SendJumpPacket()
        // with local prediction still enabled — both are session-8/9
        // confirmed bugs: m_qwRuntimeA30 is independently verified elsewhere
        // in this project to be the player's SILVER field on v1074 (stale
        // pre-v1074 struct semantics, not a real timestamp slot), and
        // ApplyLocalJumpPrediction() (invoked by SendJumpPacket's default
        // applyLocalPrediction=true) redundantly re-pokes CCommand's raw
        // fields directly instead of going through SetCommand — reproduced
        // crashing the game twice. SetCommand() above already provides
        // everything needed for visible local prediction; the raw-struct
        // path is no longer used here at all.
        //
        // Session 9 [FIXED, 2nd bug]: this used to pass iStatus =
        // _CMDSTATUS_ACCOMPLISH directly, on the theory that "already
        // accomplished" would register the jump without starting the
        // animation engine. Live-captured REAL SetCommand() calls (both
        // walk and jump) via Frida show iStatus is ALWAYS 0
        // (_CMDSTATUS_BEGIN) at the moment SetCommand is invoked — the
        // per-tick command dispatcher (0x1B1380, traced this session) itself
        // writes iStatus=6/_CMDSTATUS_ACCOMPLISH internally as PART OF its
        // own state-machine processing (confirmed in its disassembly:
        // `mov dword ptr [rcx+0x18C], 6`, i.e. m_cmdAction.iStatus). Handing
        // it ACCOMPLISH directly skips whatever setup the dispatcher
        // performs during that transition and lands it in a state it only
        // ever expects to produce itself, not receive as input — a much
        // more likely crash cause than anything struct-layout related.
        if (SendJumpPacket(GetID(), nX, nY, 0, /*applyLocalPrediction=*/false))
            SyncClientPosition(nX, nY);
    }
}

void CHero::Walk(int nX, int nY)
{
    if (!GameRva::VERIFIED_V1074) {
        // Session 10 [LIVE-CONFIRMED WORKING]: real walking, including long
        // distances tested via the debug button. Two independent findings
        // got this here:
        //
        // 1. Local state: a real walk goes through CRole::SetCommand() with
        //    CCommand.iType==15 (NOT the enum table's _COMMAND_WALK==3 —
        //    confirmed twice independently, once via Frida in session 9, once
        //    via this session's hardware-breakpoint tracer), which tells the
        //    client's per-tick dispatcher a multi-frame ANIMATED move is in
        //    progress. An earlier attempt used SyncClientPosition() (an
        //    instant position snap, the right tool for jump's one-shot
        //    teleport) instead — that's the wrong model for an animated move
        //    and left m_cmdAction.iType stale/inconsistent with the new
        //    position. idTarget=154 is a constant confirmed across every real
        //    walk sample captured; posTarget is TILE coordinates (verified:
        //    a captured (569,581) matched the hero's actual tile position on
        //    a large map). Every other CCommand field is carried forward from
        //    the hero's EXISTING command state rather than hardcoded — live
        //    captures showed those fields are just whatever the previous
        //    command (of any type) left behind, not walk-specific constants;
        //    hardcoding them would reproduce the exact class of bug that
        //    already crashed this project once before (see CCommand's own
        //    comment in CRole.h).
        //
        // 2. The actual crash (twice, both with a correctly-built CCommand
        //    already in place) turned out to be unrelated to any of the
        //    above — see packets.cpp's SendWalkPacket: GetGameTimestampMs(),
        //    a native game-code call, faulted every time it ran from this
        //    call path. Fixed by switching to GetTickCount().
        SendWalkPacket(GetID(), nX, nY, /*facing=*/0);

        CCommand cmd = m_cmdAction;
        cmd.iType = 15;
        cmd.iStatus = _CMDSTATUS_BEGIN;
        cmd.idTarget = 154;
        cmd.posTarget = Position(nX, nY);
        cmd.nDir = 1;
        spdlog::debug("[walk] SetCommand iType=15 idTarget=154 pos=({},{})", nX, nY);
        SetCommand(&cmd);
        return;
    }
    GameCall::CHero_Walk()(this, nX, nY);
}

void CHero::Attack(OBJID idTarget)
{
    // Session 9: CRoleMgr::m_deqRole is not a deque on v1074 — see entities.h.
    for (CRole* role : Entities::Get()) {
        if (role && role->GetID() == idTarget) {
            AttackTarget(idTarget, role->m_posMap);
            return;
        }
    }
}

void CHero::AttackTarget(OBJID idTarget, const Position& posTarget)
{
    uint8_t buf[48] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, idTarget);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, (uint32_t)posTarget.x);

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, (uint32_t)posTarget.y);

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, 2); // physical attack interaction

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03FE;
    SendPacket(buf, off);
}

void CHero::ShootTarget(OBJID idTarget)
{
    // Get equipped weapon (right hand) ID for the shoot packet
    CItem* weapon = GetEquip(EquipSlot::RWEAPON);
    if (!weapon)
        return;

    uint8_t buf[48] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, idTarget);

    buf[off++] = 0x28; // field 5: equipped weapon item ID
    off += WriteVarint(buf + off, weapon->GetID());

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, 25); // archer/shoot interaction

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03FE;
    SendPacket(buf, off);
}

void CHero::PickupItem(OBJID idItem, const Position& pos)
{
    if (CGameMap* map = Game::GetMap()) {
        for (CMapItem* itemRef : MapItems::Get()) {
            if (!itemRef || itemRef->m_id != idItem)
                continue;
            if (itemRef->m_pos.x != pos.x || itemRef->m_pos.y != pos.y)
                continue;
            PickupItem(*itemRef);
            return;
        }
    }

    CCommand cmd = {};
    cmd.iType = _COMMAND_PICKUP;
    cmd.iStatus = _CMDSTATUS_BEGIN;
    cmd.idTarget = idItem;
    cmd.posTarget = pos;
    SetCommand(&cmd);
}

static int WriteVarint(uint8_t* buf, uint32_t value)
{
    int n = 0;
    while (value > 0x7F) {
        buf[n++] = (uint8_t)(value & 0x7F) | 0x80;
        value >>= 7;
    }
    buf[n++] = (uint8_t)value;
    return n;
}

static int WriteFixed32(uint8_t* buf, uint32_t value)
{
    memcpy(buf, &value, sizeof(value));
    return sizeof(value);
}

// Session 5/6 [LIVE-TRACED, CALL-TESTED AND CRASHED — DO NOT RE-ENABLE without
// re-verifying live]: calls the client's own native "build+serialize+send
// MsgMapItem" function directly, the same way the game's own pickup UI code
// does — see GameRva::CNETCLIENT_SEND_MAPITEM_MSG in game.h for the full
// trace. The 5th (stack) parameter was WRONG in the first attempt — a live
// register diff (session 5c/5d, coclassicbot-live-offsets memory) against two
// REAL native pickups showed it's the message MODE (constant 3 =
// kMsgMapItemModePickup, matching packets.cpp's OWN constant), not the item's
// "plus" bonus as first assumed; passing plus=0 there was almost certainly
// what corrupted the downstream serialize/commit call. Fixed, but the SEH
// guard caught every earlier attempt EXCEPT one that crashed the whole game
// process (SEH did not save it that time) — so treat any live call here as
// capable of a hard crash, not just a caught exception.
//
// Session 6 [FRAGILE HACK]: GameRva::CNETCLIENT_SINGLETON_ACCESSOR (0x96FE0)
// is CONFIRMED WRONG (session 5e) and no real accessor could be found after
// an exhaustive search (session 6a-6e): every one of the ~146 real MSVC
// magic-static singletons in the entire module was checked live and none
// match; a full memory scan for the pointer as a plain global found nothing;
// the call chain leading to the real value turned out to cross a Themida VM
// transition (not a plain call instruction), which blocks tracing it
// statically. The one thing that DID hold up: the real CNetClient "this" has
// been the exact same value across 5+ separate game process launches (no
// ASLR + deterministic early allocation for this client build), so as a
// stopgap we use that literal value directly instead of any accessor. This
// is fragile — it can silently break on a client update, a different
// Windows/loader configuration, or possibly even just bad luck on a future
// relaunch — and there is no way to detect that it's wrong other than the
// call failing again. Revisit if this ever stops working.
// Raw-buffer only (no C++ objects) so this can safely use __try/__except.
static void LogStagingObjectPreCall(void* netClient)
{
    static char hex[0x60 * 3 + 1];
    uint64_t stagingObjPtr = 0;
    __try {
        stagingObjPtr = *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(netClient) + 0x20);
        hex[0] = '\0';
        if (stagingObjPtr) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(stagingObjPtr);
            size_t off = 0;
            for (int i = 0; i < 0x60; i++) {
                off += static_cast<size_t>(snprintf(hex + off, sizeof(hex) - off, "%02X ", p[i]));
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("[native] pre-call staging obj dump itself faulted (SEH caught)");
        return;
    }
    spdlog::info("[native] pre-call staging obj: ptr=0x{:X} bytes=[{}]", stagingObjPtr, hex);
}

// Session 6f [REVERTED — confirmed reproducible (2/2) crash-on-injection-
// alone regression, even though this function is only reachable via the
// gated debug button and was never called. Changing ONLY the __except filter
// expression here (EXCEPTION_EXECUTE_HANDLER -> a real filter function) was
// the sole diff between a confirmed-stable build and this one. Root cause
// NOT understood — some interaction between the compiled SEH/unwind data for
// this function and something at module-load time, not at call time, is the
// leading theory but unconfirmed. Do not reintroduce a filter-expression
// __except here without re-testing injection-only stability first.]
//
// Session 6f/6g: diagnostic-only Vectored Exception Handler (same proven
// pattern as tracer.cpp's VehHandler, deliberately NOT an __except filter
// expression — that form is what caused the injection-crash regression
// above). Logs exactly what/where faults, then lets normal SEH handling
// proceed unchanged (EXCEPTION_CONTINUE_SEARCH). Registered lazily, once.
static LONG CALLBACK NativePickupVehHandler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    uint64_t addr = reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress);
    uint64_t rva = addr - Game::Base();
    spdlog::error("[native] VEH: exception code=0x{:X} at addr=0x{:X} (RVA=0x{:X})", code, addr, rva);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void EnsureNativePickupVehRegistered()
{
    static bool registered = false;
    if (!registered) {
        AddVectoredExceptionHandler(1, NativePickupVehHandler);
        registered = true;
    }
}

// Session 7 [LIVE-TRACED, 2026-08-24]: CallSendMapItemMsgGuarded (calling
// CNETCLIENT_SEND_MAPITEM_MSG / 0x1C8B30 directly) is retired — root-caused,
// not just unverified. Disassembly + a live VEH capture both pinned the fault
// to 0x1C8B30's own prologue: it dereferences netClient+0x20 with no null
// check, and that field is confirmed NULL between sends (only populated by an
// unreached wrapper — see GameRva:: comment). Replaced below by building the
// staging struct ourselves and driving CNETCLIENT_COMPUTE_MAPITEM_SIZE /
// CNETCLIENT_BEGIN_MSG / CNETCLIENT_COMMIT_STAGING by hand — the same steps
// 0x1C8B30 takes internally after the part we're skipping. Pure SEH, no C++
// objects in scope (const CMapItem& is just a reference read for POD fields;
// results come back via out-params so logging can stay outside __try, per the
// session 6f/6g lesson about not mixing SEH with anything non-trivial in this
// function's own scope).
static bool CallOwnStructPickupGuarded(void* netClient, const CMapItem& item, uint32_t* outSize, uint8_t* outCommitResult)
{
    constexpr uint32_t kNativeMapItemModePickup = 3;
    EnsureNativePickupVehRegistered();

    alignas(8) uint8_t staging[0x40] = {};
    const uintptr_t base = Game::Base();

    __try {
        *reinterpret_cast<uint64_t*>(staging + 0x00) = base + GameRva::MSGMAPITEM_VTABLE;
        *reinterpret_cast<uint32_t*>(staging + 0x10) = static_cast<uint32_t>(item.m_id);
        *reinterpret_cast<uint32_t*>(staging + 0x18) = static_cast<uint32_t>(item.m_pos.x);
        *reinterpret_cast<uint32_t*>(staging + 0x1C) = static_cast<uint32_t>(item.m_pos.y);
        *reinterpret_cast<uint32_t*>(staging + 0x24) = kNativeMapItemModePickup;

        auto computeSize = reinterpret_cast<uint64_t(*)(void*)>(base + GameRva::CNETCLIENT_COMPUTE_MAPITEM_SIZE);
        const uint32_t size = static_cast<uint32_t>(computeSize(staging) & 0xFFFF);
        *outSize = size;
        if (size > 0x8000)
            return false;

        auto beginMsg = reinterpret_cast<void*(*)(void*, uint32_t, uint32_t)>(base + GameRva::CNETCLIENT_BEGIN_MSG);
        beginMsg(netClient, size, 0x44D);

        void* cursor = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(netClient) + 0x10);

        auto commit = reinterpret_cast<uint8_t(*)(void*, void*, uint32_t)>(base + GameRva::CNETCLIENT_COMMIT_STAGING);
        const uint8_t ok = commit(staging, cursor, size);
        *outCommitResult = ok;
        return ok != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("[native] own-struct pickup: SEH caught an exception");
        return false;
    }
}

static bool SendPickupItemMsgNative(const CMapItem& item)
{
    // FRAGILE: see the big comment above. Not module-base-relative — this is
    // a raw absolute address (a low, non-ASLR'd early heap allocation), not
    // an RVA into ImConquer.exe's own image.
    constexpr uintptr_t kFragileHardcodedNetClientThis = 0x15E718;
    void* netClient = reinterpret_cast<void*>(kFragileHardcodedNetClientThis);
    spdlog::info("[native] SendPickupItemMsgNative: netClient=0x{:X} (FRAGILE hardcoded, not a real accessor)", (uintptr_t)netClient);
    if (!netClient)
        return false;

    uint32_t size = 0;
    uint8_t commitResult = 0;
    const bool ok = CallOwnStructPickupGuarded(netClient, item, &size, &commitResult);
    spdlog::info("[native] SendPickupItemMsgNative: own-struct path size=0x{:X} commit={} ok={}", size, commitResult, ok);
    return ok;
}

static bool SendPickupItemPacket(const CMapItem& item)
{
    uint8_t buf[48] = {};
    int off = 4;

    buf[off++] = 0x0D;
    off += WriteFixed32(buf + off, item.m_id);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, item.m_idType);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, static_cast<uint32_t>(item.m_pos.x));

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, static_cast<uint32_t>(item.m_pos.y));

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, static_cast<uint32_t>(item.GetPlus()));

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, kMsgMapItemModePickup);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgMapItemPacketType;
    return SendPacket(buf, off);
}

// Session 5 [historical — this native-object path is superseded, see below]:
// this crashed the whole game process once already (see coclassicbot-live-
// offsets memory, session 5c/5d) even with a SEH guard around it. No longer
// wired to the debug button; kept only as a reference for the native-object
// pipeline investigation.
//
// Session 8 [LIVE-CONFIRMED]: the debug button now tests the REAL SendMsg
// path instead — move to the item's own coordinates via a raw jump packet
// (the same SendPacket()/SendMsg() pipeline, already proven working end to
// end), give the server a brief moment to process the move, then send the
// existing SendPickupItemPacket() unmodified. This is a genuine, real-server
// round trip verification, not a local-only success check — confirmed live
// (coclassicbot-live-offsets memory, session 8): item picked up successfully
// once the hero was actually standing on it (the server enforces pickup
// range; sending the packet from elsewhere is correctly rejected).
bool DebugTestNativePickup(const CMapItem& item)
{
    CHero* hero = CHero::GetSingletonPtr();
    if (!hero) {
        spdlog::error("[debug] DebugTestNativePickup: no hero singleton");
        return false;
    }

    spdlog::info("[debug] DebugTestNativePickup: moving to item position ({},{}) before pickup",
                 item.m_pos.x, item.m_pos.y);
    // applyLocalPrediction=false: ApplyLocalJumpPrediction() is a confirmed
    // silver-corrupting no-go (see CHero::Jump()); client-side movement is
    // handled by SyncClientPosition() below instead.
    if (!SendJumpPacket(hero->GetID(), item.m_pos.x, item.m_pos.y, 0, /*applyLocalPrediction=*/false)) {
        spdlog::error("[debug] DebugTestNativePickup: jump packet failed to send");
        return false;
    }

    // Session 9: without this the server moves us but the client stays put —
    // the pickup still succeeds (server-side proximity is satisfied) while the
    // character visibly never moves. Observed live before this was added.
    hero->SyncClientPosition(item.m_pos.x, item.m_pos.y);

    Sleep(300); // give the server a moment to process the move before the pickup request

    const bool ok = SendPickupItemPacket(item);
    spdlog::info("[debug] DebugTestNativePickup: pickup packet sent, ok={}", ok);
    return ok;
}

// Session 9 [UPDATED]: manual jump test, separate from any hunt/travel
// logic — see CHero.h. `applyLocalPrediction` now tests the SAME fixed,
// SetCommand()-based approach CHero::Jump()'s packet-jump branch uses
// (build a CCommand, call the game's own native SetCommand — already
// proven safe elsewhere) instead of the old raw-struct
// ApplyLocalJumpPrediction() path, which is confirmed broken (corrupts the
// silver field, crashed the game twice) and is no longer exercised from
// here at all. SendJumpPacket() is always called with its own
// applyLocalPrediction=false now.
bool DebugTestJump(int destX, int destY, bool applyLocalPrediction)
{
    CHero* hero = CHero::GetSingletonPtr();
    if (!hero) {
        spdlog::error("[debug] DebugTestJump: no hero singleton");
        return false;
    }

    spdlog::info("[debug] DebugTestJump: jumping to ({},{}) applyLocalPrediction={}",
                 destX, destY, applyLocalPrediction);

    // Tell the server first -- it accepts packet-only moves (verified by
    // picking up an item several blocks away after a packet-only move).
    const bool ok = SendJumpPacket(hero->GetID(), destX, destY, 0, /*applyLocalPrediction=*/false);
    spdlog::info("[debug] DebugTestJump: jump packet sent, ok={}", ok);

    // Then bring the CLIENT into agreement. The server moves us but never
    // echoes a position update back to the originating client, so without
    // this the client stays visually and logically at the old tile.
    //
    // Session 9: this REPLACES the old CCommand/SetCommand prediction path,
    // which crashed the game five times running. Repositioning is three plain
    // data writes -- no native call, no CCommand, no vtable, no gated RVA.
    if (ok && applyLocalPrediction) {
        hero->SyncClientPosition(destX, destY);
        spdlog::info("[debug] DebugTestJump: client position synced to ({},{})", destX, destY);
    }

    return ok;
}

// Session 10: on-demand test for CHero::Walk()'s new SetCommand-based
// fallback — added because minimap clicks (the obvious way to "just try
// walking") turned out to route through the game's own native click-to-move
// handler entirely, never through this code at all, so there was no
// controlled way to actually exercise it without a live autohunt session.
bool DebugTestWalk(int destX, int destY)
{
    CHero* hero = CHero::GetSingletonPtr();
    if (!hero) {
        spdlog::error("[debug] DebugTestWalk: no hero singleton");
        return false;
    }

    spdlog::info("[debug] DebugTestWalk: walking to ({},{})", destX, destY);
    hero->Walk(destX, destY);
    return true;
}

void CHero::PickupItem(const CMapItem& item)
{

    if (!SendPickupItemPacket(item)) {
        CCommand cmd = {};
        cmd.iType = _COMMAND_PICKUP;
        cmd.iStatus = _CMDSTATUS_BEGIN;
        cmd.idTarget = item.m_id;
        cmd.posTarget = item.m_pos;
        SetCommand(&cmd);
    }
}

static bool SendMsgItem(OBJID idItem, uint32_t action, int slot = 0)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idItem);

    if (slot > 0) {
        buf[off++] = 0x10;
        off += WriteVarint(buf + off, (uint32_t)slot);
    }

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, action);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03F1;
    return SendPacket(buf, off);
}

static bool SendWarehousePacket(OBJID idNpc, uint32_t action, uint32_t packageType, OBJID idItem = 0)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idNpc);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, action);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, packageType);

    if (idItem != 0) {
        buf[off++] = 0x20;
        off += WriteVarint(buf + off, idItem);
    }

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x044E;
    return SendPacket(buf, off);
}

static bool SendSimplePacket(uint16_t type, uint8_t fieldTag, uint32_t value)
{
    uint8_t buf[16] = {};
    int off = 4;

    buf[off++] = fieldTag;
    off += WriteVarint(buf + off, value);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = type;
    return SendPacket(buf, off);
}

static bool SendBuyItemPacket(OBJID idNpc, uint32_t typeId)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idNpc);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, typeId);

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, 1);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03F1;
    return SendPacket(buf, off);
}

static bool SendSellItemPacket(OBJID idNpc, OBJID idItem)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idNpc);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, idItem);

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, 2);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03F1;
    return SendPacket(buf, off);
}

static bool SendTradePacket(uint32_t mode, uint32_t value)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, mode);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, value);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgTradePacketType;
    return SendPacket(buf, off);
}

static bool SendDropItemPacket(OBJID idItem, const Position& pos)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idItem);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, (uint32_t)pos.x);

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, (uint32_t)pos.y);

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, 3);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03F1;
    return SendPacket(buf, off);
}

void CHero::MagicAttack(OBJID idMagic, OBJID idTarget, const Position& posTarget)
{
    uint8_t buf[48] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, idTarget);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, (uint32_t)posTarget.x);

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, (uint32_t)posTarget.y);

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, 21); // magic attack interaction

    buf[off++] = 0x40;
    off += WriteVarint(buf + off, idMagic);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03FE;
    SendPacket(buf, off);
}

void CHero::MagicAttack(OBJID idMagic, const Position& posTarget)
{
    MagicAttack(idMagic, 0, posTarget);
}

void CHero::StartMining()
{
    if (!GameRva::VERIFIED_V1074) {
        static bool warned = false;
        if (!warned) { spdlog::error("[safety] CHero::StartMining: RVA unverified on v1074, no-op (see game.h GameRva::VERIFIED_V1074)"); warned = true; }
        return;
    }
    GameCall::CHero_StartMining()(this);
}

void CHero::UseItem(OBJID idItem)
{
    SendMsgItem(idItem, 4);
}

void CHero::DropItem(OBJID idItem, const Position& pos)
{
    SendDropItemPacket(idItem, pos);
}

void CHero::EquipItem(OBJID idItem, int slot)
{
    SendMsgItem(idItem, 4, slot + 1);
}

void CHero::UnequipItem(OBJID idItem, int slot)
{
    SendMsgItem(idItem, 6, slot + 1);
}

void CHero::RepairItem(OBJID idItem)
{
    SendMsgItem(idItem, 14);
}

void CHero::OpenWarehouse(OBJID idNpc)
{
    ActivateNpc(idNpc);
    SendMsgItem(idNpc, 9);
    SendWarehousePacket(idNpc, 1, 10);
}

void CHero::DepositWarehouseItem(OBJID idNpc, OBJID idItem)
{
    SendWarehousePacket(idNpc, 2, 10, idItem);
}

void CHero::DepositWarehouseSilver(OBJID idNpc, uint32_t amount)
{
    if (amount == 0) return;

    // Packet type 0x03F1: field1=npcId, field2=amount, field5=10 (deposit silver)
    {
        uint8_t buf[32] = {};
        int off = 4;
        buf[off++] = 0x08;
        off += WriteVarint(buf + off, idNpc);
        buf[off++] = 0x10;
        off += WriteVarint(buf + off, amount);
        buf[off++] = 0x28;
        off += WriteVarint(buf + off, 10);
        *(uint16_t*)buf = (uint16_t)off;
        *(uint16_t*)(buf + 2) = 0x03F1;
        SendPacket(buf, off);
    }

    // Confirm packet: field1=npcId, field5=9
    {
        uint8_t buf[32] = {};
        int off = 4;
        buf[off++] = 0x08;
        off += WriteVarint(buf + off, idNpc);
        buf[off++] = 0x28;
        off += WriteVarint(buf + off, 9);
        *(uint16_t*)buf = (uint16_t)off;
        *(uint16_t*)(buf + 2) = 0x03F1;
        SendPacket(buf, off);
    }

    spdlog::info("[npc] DepositWarehouseSilver npc={} amount={}", idNpc, amount);
}

void CHero::WithdrawWarehouseItem(OBJID idNpc, OBJID idItem)
{
    SendWarehousePacket(idNpc, 3, 10, idItem);
}

void CHero::OpenTreasureBank(OBJID idNpc)
{
    ActivateNpc(idNpc);
}

void CHero::DepositTreasureBankMeteors(OBJID idNpc)
{
    ActivateNpc(idNpc);
    AnswerNpcEx(0, 101);
}

void CHero::DepositTreasureBankDragonBalls(OBJID idNpc)
{
    ActivateNpc(idNpc);
    AnswerNpcEx(1, 101);
}

void CHero::OpenComposeBank(OBJID idNpc)
{
    ActivateNpc(idNpc);
}

void CHero::DepositComposeBankAll()
{
    SendSimplePacket(0x1B67, 0x08, 2);
}

void CHero::CancelFly()
{
    constexpr uint32_t kMsgActionModeCancelFly = 53;
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, kMsgActionModeCancelFly);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, GetTickCount());

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgActionPacketType;
    SendPacket(buf, off);
}

void CHero::Sit()
{
    constexpr uint32_t kMsgActionModeSit = 3;
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, kMsgActionModeSit);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, GetTickCount());

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, static_cast<uint32_t>(m_posMap.x));

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, static_cast<uint32_t>(m_posMap.y));

    buf[off++] = 0x40;
    off += WriteVarint(buf + off, 250);

    *(uint16_t*)buf = static_cast<uint16_t>(off);
    *(uint16_t*)(buf + 2) = kMsgActionPacketType;
    SendPacket(buf, off);
}

void CHero::ReviveInTown()
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, kMsgActionModeRevive);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, GetTickCount());

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgActionPacketType;
    SendPacket(buf, off);
}

bool CHero::VipTeleport(OBJID mapId)
{
    const uint32_t destination = GetVipDestinationForMap(mapId);
    if (destination == 0)
        return false;

    uint8_t buf[16] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, kMsgVipModeTeleport);

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, destination);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgVipPacketType;
    SendPacket(buf, off);
    return true;
}

void CHero::VipTeleportTwinCity()
{
    VipTeleport(MAP_TWIN_CITY);
}

void CHero::BuyItem(OBJID idNpc, uint32_t typeId)
{
    SendBuyItemPacket(idNpc, typeId);
}

void CHero::SellItem(OBJID idNpc, OBJID idItem)
{
    SendSellItemPacket(idNpc, idItem);
}

void CHero::StartTrade(OBJID idPlayer)
{
    SendTradePacket(kTradeModeStart, idPlayer);
}

void CHero::OfferTradeItem(OBJID idItem)
{
    SendTradePacket(kTradeModeAddItem, idItem);
}

void CHero::AcceptTrade(OBJID idPlayer)
{
    SendTradePacket(kTradeModeAccept, idPlayer);
}

void CHero::CancelTrade(OBJID idPlayer)
{
    SendTradePacket(kTradeModeCancel, idPlayer);
}

// =====================================================================
// NPC interaction — packet-based
// for our binary, so we send raw protobuf packets via SendMsg instead)
//
// Activate NPC packet (type 0x07EF):
//   protobuf field 1 (varint) = NPC entity ID
//   protobuf field 5 (varint) = 1
//
// Answer NPC packet (type 0x07F0):
//   protobuf field 5 (varint) = dialog option index
//   protobuf field 6 (varint) = NPC task/dialog ID
// =====================================================================
void CHero::ActivateNpc(OBJID idNpc)
{
    // [u16 size][u16 type=0x07EF][field1=idNpc][field5=1]
    uint8_t buf[32];
    int off = 4; // skip header, fill size+type later

    // field 1 (tag=0x08), varint = NPC entity ID
    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idNpc);

    // field 5 (tag=0x28), varint = 1
    buf[off++] = 0x28;
    buf[off++] = 0x01;

    // Fill header
    *(uint16_t*)buf = (uint16_t)off;       // size
    *(uint16_t*)(buf + 2) = 0x07EF;        // type

    spdlog::debug("[npc] ActivateNpc entity={}, pkt size={}", idNpc, off);
    SendPacket(buf, off);
}

void CHero::AnswerNpc(int answer)
{
    AnswerNpcEx(answer, 101); // default task ID for Conductress
}

void CHero::AnswerNpcEx(int answer, int taskId)
{
    // [u16 size][u16 type=0x07F0][field5=answer][field6=taskId]
    // Proto3: field5=0 is omitted (default value not serialized)
    uint8_t buf[32];
    int off = 4;

    // field 5 (tag=0x28), varint = answer option (skip if 0)
    if (answer != 0) {
        buf[off++] = 0x28;
        off += WriteVarint(buf + off, (uint32_t)answer);
    }

    // field 6 (tag=0x30), varint = task/dialog ID
    buf[off++] = 0x30;
    off += WriteVarint(buf + off, (uint32_t)taskId);

    // Fill header
    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x07F0;

    spdlog::debug("[npc] AnswerNpc option={} taskId={}, pkt size={}", answer, taskId, off);
    SendPacket(buf, off);
}

bool CHero::IsNpcActive() const
{
    return m_bNpcActive != FALSE;
}

OBJID CHero::GetActiveNpc() const
{
    return m_idActiveNpc;
}

int CHero::GetCurrentHp() const
{
    return m_pStatTable ? m_pStatTable->GetValue(1) : 0;
}

int CHero::GetMaxHp() const
{
    if (m_nMaxHp > 0)
        return m_nMaxHp;

    if (!GameRva::VERIFIED_V1074) {
        static bool warned = false;
        if (!warned) { spdlog::error("[safety] CHero::GetMaxHp: fallback RVA unverified on v1074, returning 0 (see game.h GameRva::VERIFIED_V1074)"); warned = true; }
        return 0;
    }
    return GameCall::CHero_GetMaxHp()(this);
}

int CHero::GetCurrentMana() const
{
    if (!GameRva::VERIFIED_V1074) {
        static bool warned = false;
        if (!warned) { spdlog::error("[safety] CHero::GetCurrentMana: RVA unverified on v1074, returning 0 (see game.h GameRva::VERIFIED_V1074)"); warned = true; }
        return 0;
    }
    return GameCall::CHero_GetCurrentMana()(this);
}

int CHero::GetMaxMana() const
{
    if (m_bMaxManaValid)
        return m_nMaxMana;

    if (!GameRva::VERIFIED_V1074) {
        static bool warned = false;
        if (!warned) { spdlog::error("[safety] CHero::GetMaxMana: fallback RVA unverified on v1074, returning 0 (see game.h GameRva::VERIFIED_V1074)"); warned = true; }
        return 0;
    }
    return GameCall::CHero_GetMaxMana()(this);
}

void CHero::RefreshSilverCache(bool trusted) const
{
    const OBJID heroId = GetID();
    ResetSilverCacheForHero(heroId);

    const uint64_t rawValue = m_qwRuntimeA30;
    if (rawValue > UINT32_MAX)
        return;

    if (!trusted) {
        const uint64_t nowMs = GetGameTimestampMs();
        if (LooksLikeMovementTimestamp(rawValue, nowMs))
            return;

        const uint32_t candidate = static_cast<uint32_t>(rawValue);
        if (!IsPlausibleUntrustedSilverValue(candidate))
            return;
    }

    StoreSilverCache(heroId, static_cast<uint32_t>(rawValue), trusted);
}

void CHero::SetTrustedSilver(uint32_t value) const
{
    StoreSilverCache(GetID(), value, true);
}

bool CHero::HasTrustedSilverCache() const
{
    ResetSilverCacheForHero(GetID());
    return g_silverCache.hasValue && g_silverCache.isTrusted;
}

uint32_t CHero::GetSilver() const
{
    // v1074 [LIVE-VERIFIED]: m_qwRuntimeA30 now sits at the correct silver offset
    // (+0xA80), so its low 32 bits are the silver amount directly (confirmed =7681).
    // A packet-trusted cache value still wins when present; otherwise read the field.
    // The old movement-timestamp/plausibility heuristics were a workaround for the
    // previous (wrong) +0xA30 slot and are intentionally no longer applied here.
    ResetSilverCacheForHero(GetID());
    if (g_silverCache.hasValue && g_silverCache.isTrusted)
        return g_silverCache.value;

    return static_cast<uint32_t>(m_qwRuntimeA30 & 0xFFFFFFFFull);
}

CMagic* CHero::FindMagicByName(const char* name) const
{
    if (!name || !name[0])
        return nullptr;

    for (const auto& magicRef : m_vecMagic) {
        if (!magicRef)
            continue;
        if (_stricmp(magicRef->GetName(), name) == 0)
            return magicRef.get();
    }

    return nullptr;
}

CMagic* CHero::FindMagicById(OBJID idMagic) const
{
    for (const auto& magicRef : m_vecMagic) {
        if (magicRef && magicRef->GetMagicType() == idMagic)
            return magicRef.get();
    }
    return nullptr;
}

bool CHero::IsVip() const
{
    return m_bVip != FALSE;
}
