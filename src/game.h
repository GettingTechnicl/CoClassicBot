#pragma once
#include "CHero.h"
#include "CGameMap.h"
#include "mapdata.h"
#include "entities.h"
#include "log.h"
#include <string>

class CStatTable;
class CEntitySet;
class CEntityInfo;

// =====================================================================
// Offsets — all RVAs relative to ImConquer.exe base (0x140000000)
// Verified in Ghidra on the 64-bit Scylla dump.
// =====================================================================
namespace Offsets {
    // v1074 (live-verified): the role manager is heap-allocated and reached via a
    // STATIC pointer in the image. roleMgr = *(base + ROLE_MGR_PTR); hero = *roleMgr.
    constexpr uintptr_t ROLE_MGR_PTR = 0x69C730;  // [LIVE-VERIFIED] pointer-to-CRoleMgr
    constexpr uintptr_t ROLE_MGR  = 0x4DF588;     // [STALE on v1074 — was static object; now code]
    // WARNING: the three below are NOT re-verified for v1074 and are likely stale too.
    constexpr uintptr_t ENTITY_INFO = 0x4DF590;   // [UNVERIFIED v1074]
    constexpr uintptr_t ENTITY_SET = 0x4DF5F0;    // [UNVERIFIED v1074]
    constexpr uintptr_t GAME_MAP  = 0x4E02E0;     // [CONFIRMED GARBAGE on v1074 — see below]

    // ── CURRENT MAP ID (v1074, session 10) ──
    // A DIRECT u32, not a pointer: mapId = *(uint32*)(base + CURRENT_MAP_ID).
    //
    // Found by a four-map differential over every u32 in the image
    // (Twin City 1002, Desert 1000, Market 1036, Phoenix Castle 1011) —
    // exactly two adjacent globals survived, and session 6 independently
    // recorded these same two RVAs from its own differential.
    //
    // IMPORTANT HISTORY: these were briefly REJECTED because they failed a
    // three-map test including "Bird Island = 1020". That id was wrong —
    // Bird Island is 1015 (island.DMap); 1020 is Ape Mountain (canyon.DMap).
    // The repo's data_maps.json mislabels Market (says 1004, is 1036) and
    // Bird Island (says 1020, is 1015). The globals were correct the whole
    // time; the labels were not. Trust ini/GameMap.json, never data_maps.json.
    constexpr uintptr_t CURRENT_MAP_ID     = 0x699560;
    constexpr uintptr_t CURRENT_MAP_ID_ALT = 0x699564;   // adjacent survivor
}

namespace GameRva {
    // v1074 [CONFIRMED STALE — 2026-08-22, disasm of the decrypted in-memory .text dump]:
    // these RVAs are inherited from the pre-v1074 Ghidra dump. The client update shifted
    // code NON-uniformly (unlike the flat +0x50 data-field shift), so none of them land on
    // a real function prologue anymore — CHERO_WALK resolves mid-function, GetTimestamp
    // (0xC6A80, used inline in CHero.cpp/packets.cpp) resolves to an epilogue,
    // CHERO_FATAL_STRIKE to padding, CSTATTABLE_GET_VALUE/CHERO_GET_MAX_HP to misaligned
    // garbage. The rest were never individually checked but ride the same stale dump and
    // must be assumed equally broken until re-derived. Calling through one jumps execution
    // into garbage (crash); Detour-hooking one (CENTITY_RENDER_VISUAL, CNETCLIENT_SEND_MSG,
    // MSGUPDATE_PROCESS, TRADEWINDOW_HANDLE_MESSAGE, CGAMEUI_SHOW_MSG — all Detoured at
    // init in hooks.cpp/packets.cpp) PATCHES garbage bytes and corrupts the client the
    // moment that address is next reached by the game's own code — which for
    // CEntity::RenderVisual is within the first rendered frame after login.
    //
    // GameRva::VERIFIED_V1074 gates every call/Detour site (hooks.cpp InitHooks,
    // packets.cpp InitPacketHook, CHero.cpp Walk/StartMining/GetCurrentMana/GetMaxHp/
    // GetMaxMana fallback) so today's build is safe to inject: it keeps the fully
    // re-verified READ-ONLY sensory layer (see CHero.h, coclassicbot-live-offsets memory)
    // live, and cleanly no-ops the action/hook layer instead of crashing the game.
    // Flip to true only after re-deriving + live-confirming each RVA below
    // (REVAMP_PLAN.md Phase 2; src/netfinder.cpp is the safe WSASend-hook-based tool for
    // CNETCLIENT_SEND_MSG/GET_INSTANCE, the top-priority target since every packet action
    // routes through it).
    constexpr bool VERIFIED_V1074 = false;

    constexpr uintptr_t CENTITY_RENDER_VISUAL = 0x1AFD20;
    constexpr uintptr_t CNETCLIENT_GET_INSTANCE = 0x0B9490;
    constexpr uintptr_t CNETCLIENT_SEND_MSG   = 0x18EEA0;
    constexpr uintptr_t CHERO_GET_MAX_HP       = 0x179980;
    constexpr uintptr_t CHERO_GET_MAX_MANA     = 0x179B90;
    constexpr uintptr_t CSTATTABLE_GET_VALUE   = 0x1F1490;
    constexpr uintptr_t CHERO_GET_CURRENT_MANA = 0x1A58F0;
    constexpr uintptr_t CHERO_FATAL_STRIKE     = 0x2297B0;
    constexpr uintptr_t CHERO_WALK             = 0x229DF0;
    constexpr uintptr_t CHERO_JUMP             = 0x22A0B0;
    constexpr uintptr_t CHERO_START_MINING     = 0x18BB10;
    constexpr uintptr_t MSGUPDATE_PROCESS      = 0x1E0870;
    constexpr uintptr_t TRADEWINDOW_HANDLE_MESSAGE = 0x10B1F0;
    constexpr uintptr_t CGAMEUI_SHOW_MSG           = 0x191960;

    // v1074 [LIVE-TRACED via Frida, 2026-08-23, session 5 — independent of the stale dump
    // above, do NOT gate behind VERIFIED_V1074]: re-derived by hooking send()/WSASend(),
    // walking the confirmed call chain, and disassembling each hop from code_dump.bin.
    // CNETCLIENT_SEND_MAPITEM_MSG is the native "build + header + serialize + hand off"
    // function the client's OWN pickup UI code calls — traced live end-to-end (0x1C8B30 ->
    // 0x22AC50 serialize -> 0x190DD0 BeginMsg -> 0x3C4CD0 commit -> confirmed plaintext
    // matching our own wire-format assumptions -> encryption -> send()) while the user
    // picked up/dropped a real item. Signature (x64 calling convention, read straight off
    // the disasm, CONFIRMED against real calls session 5d):
    //   void* SendMapItemMsg(CNetClient* this, uint32_t itemId, uint16_t x, uint16_t y, uint32_t mode)
    // (5th param is message MODE, e.g. 3=pickup — NOT the item's "plus" bonus; that was a
    // real bug in the first implementation attempt, since fixed in CHero.cpp.)
    //
    // CNETCLIENT_SINGLETON_ACCESSOR IS CONFIRMED WRONG — session 5e, do NOT use.
    // 0x96FE0 does NOT return the CNetClient-equivalent "this" that 0x1C8B30 actually
    // expects. Live-verified (Frida) in the SAME session: our accessor call resolves to
    // base+0x698BE0, whose contents look like a diagnostics/crash-handling object (holds
    // our own module base verbatim twice, and a system-DLL address at +0x20) — NOT a
    // network client. A REAL native pickup's rcx was 0x15E718 instead, a completely
    // different, unrelated address, not reachable from 0x698BE0 or any static global in
    // base+0x680000..0x6B0000 by a single pointer hop. This RVA was carried over from an
    // unconfirmed session-3/4 singleton-hunting guess that got conflated with a different
    // candidate along the way — never re-verified before being wired into code. Calling
    // CNETCLIENT_SEND_MAPITEM_MSG with the result of this accessor as "this" is why every
    // live test in session 5 faulted (one of them crashed the whole game process outright,
    // SEH did not always save it). Finding the real accessor/path to 0x15E718-equivalent is
    // unsolved — see coclassicbot-live-offsets memory, session 5e, for the investigation and
    // concrete next steps. DO NOT call CNetClient_SendMapItemMsg with this accessor's result.
    constexpr uintptr_t CNETCLIENT_SINGLETON_ACCESSOR = 0x96FE0;  // WRONG — see comment above
    constexpr uintptr_t CNETCLIENT_SEND_MAPITEM_MSG   = 0x1C8B30;

    // v1074 [LIVE-TRACED, session 7, 2026-08-24]: CNETCLIENT_SEND_MAPITEM_MSG
    // (above) is NOT directly callable cold — disassembly + a live crash both
    // confirmed it dereferences netClient+0x20 immediately (writing itemId into
    // *(netClient+0x20)+0x10) with NO null check. That field is the address of a
    // small, per-message "staging" struct that some OTHER function (not yet
    // found — its caller sits behind a Themida VM transition, see
    // coclassicbot-live-offsets memory session 6) allocates fresh, stores into
    // netClient+0x20, and frees again around every real send; between sends it
    // reads NULL (confirmed live: *(0x15E718+0x20) == 0 at idle), so calling
    // CNETCLIENT_SEND_MAPITEM_MSG cold is a guaranteed null-pointer write.
    //
    // Workaround: skip that function's own field-writing prologue entirely and
    // build the staging struct OURSELVES, then drive the rest of its own body
    // by hand (this is exactly what 0x1C8B30 does internally after the part we
    // skip — see CHero.cpp's own-struct pickup path):
    //   1. CNETCLIENT_COMPUTE_MAPITEM_SIZE(stagingStruct) -> payload size.
    //      Reads only plain int32 fields off the struct by fixed offset
    //      (protobuf-lite varint-size codegen) — NO vtable dispatch, confirmed
    //      by disassembling its full body, so any zeroed buffer with the right
    //      fields works, no need for a real vtable pointer for THIS call.
    //   2. CNETCLIENT_BEGIN_MSG(netClient, size, msgType=0x44D) -> allocates the
    //      real output buffer, stores its cursor into netClient+0x10 (also
    //      +0x8/+0x18 — not used by us). Confirmed signature via disasm.
    //   3. CNETCLIENT_COMMIT_STAGING(stagingStruct, cursor, size) -> writes the
    //      actual field bytes + queues for send. THIS call dispatches through
    //      the staging struct's OWN vtable internally (session 5 finding), so
    //      the struct's offset-0 vtable pointer must be real — see
    //      MSGMAPITEM_VTABLE below, live-captured from 8 real native pickups
    //      this session, always identical.
    // Staging struct field offsets (confirmed live, both by CNETCLIENT_SEND_
    // MAPITEM_MSG's own writes and matching what CNETCLIENT_COMPUTE_MAPITEM_SIZE
    // reads): itemId@+0x10 (u32), x@+0x18 (u32), y@+0x1C (u32), mode@+0x24 (u32,
    // 3=pickup). Struct only needs to be large enough to cover the highest
    // offset touched by the size-calc (+0x30 seen read) — 0x40 bytes is safe.
    constexpr uintptr_t CNETCLIENT_COMPUTE_MAPITEM_SIZE = 0x22AC50;
    constexpr uintptr_t CNETCLIENT_BEGIN_MSG            = 0x190DD0;
    constexpr uintptr_t CNETCLIENT_COMMIT_STAGING       = 0x3C4CD0;
    // Live-captured vtable RVA for a MsgMapItem-type staging struct (session 7,
    // frida_get_vtable.py hooking CNETCLIENT_SEND_MAPITEM_MSG's own entry
    // during 8 separate real native pickups — 8/8 identical). Client-build-
    // specific like the other v1074 RVAs above; not module-base-relative
    // storage, just the vtable's own RVA within the module.
    constexpr uintptr_t MSGMAPITEM_VTABLE = 0x5D2DF0;

    // v1074 [LIVE-CONFIRMED, session 8, 2026-08-24]: the REAL CNetClient::SendMsg
    // — completely supersedes the stale, garbage `CNETCLIENT_SEND_MSG` (0x18EEA0,
    // above; doesn't even disassemble as valid code) and `CNETCLIENT_GET_INSTANCE`
    // (0x0B9490, never re-verified — do not use for this path). Found via a
    // send()/WSASend() backtrace sweep on REAL traffic (netfinder-style, done via
    // Frida for iteration speed), NOT the session-7 native-object pickup path —
    // that path's own logging/telemetry code (0xE9960 region) actively resists
    // hooking and produced an unwanted duplicate-game-instance side effect when
    // called directly; it's abandoned in favor of this one, which is confirmed
    // NOT hook-resistant and produced zero anomalies across many live calls.
    //
    // Signature: bool SendMsg(void* connectionObject, const uint8_t* buffer,
    // uint32_t length) — `buffer` must be a fully-built [u16 size][u16 type]
    // + payload packet (exactly what packets.cpp already builds); `length` must
    // equal the buffer's own embedded size field (SendMsg validates this itself
    // and fails otherwise). Internally: validates, flushes the pending queue
    // first if it doesn't have room, copies the bytes in, and lets an
    // already-running per-tick poller (0x1DD860) drain the queue to the real
    // Winsock send(). Fully disassembled, plain non-obfuscated code — see
    // coclassicbot-live-offsets memory, session 8, for the complete trace.
    //
    // `connectionObject` (arg 1) is NOT netClient (0x15E718) — it's a DIFFERENT,
    // per-session object obtained via CNETCLIENT_CONNECTION_SINGLETON below:
    // call it (a genuine, safe MSVC magic-static, unlike the confirmed-fake
    // 0x96FE0), then read its own `+0x20` field live — this is a fresh
    // per-session pointer, not stable across relaunches, so re-resolve it
    // every call rather than caching it long-term like the old g_netClient.
    //
    // LIVE-CONFIRMED END TO END (session 8): built a real pickup packet using
    // packets.cpp's own pre-existing, never-modified raw-packet format, called
    // this SendMsg directly — the item was genuinely picked up in-game. This is
    // not just a "call succeeded" result; it's a full round-trip through the
    // real server.
    constexpr uintptr_t CNETCLIENT_CONNECTION_SINGLETON = 0xB7320;
    constexpr uintptr_t CNETCLIENT_SEND_MSG_REAL         = 0x1DD450;

    // v1074 [LIVE-CONFIRMED, session 9, 2026-08-24]: the REAL CRole::SetCommand.
    // GameVtableIndex::CRole_SetCommand (59, below) is CONFIRMED WRONG — a live
    // vtable scan (slots 0-199) found no match for this address anywhere in the
    // hero's own vtable; slot 59 itself resolves to an unrelated trivial stub
    // (`mov eax, 0xCD764F08; ret`, ignores its args). The real function is
    // reached via a plain DIRECT call, not a virtual dispatch — confirmed by
    // hooking it live during natural walking: consistent (this, cmd) args,
    // sensible CCommand contents tracking the character's real position, and a
    // stable, non-vtable call chain. Root-caused after `CRole::SetCommand()`
    // (via the wrong vtable slot) crashed the game live — that call was
    // actually harmless on its own (the stub ignores its args), so the crash's
    // exact mechanism through the old path is unconfirmed, but moot now that
    // this is fixed. Signature: void SetCommand(void* this, CCommand* cmd) —
    // null-checks cmd, then bulk-copies it into this+0x188 (m_cmdAction,
    // independently confirmed correct all session).
    constexpr uintptr_t CROLE_SET_COMMAND_REAL = 0x1B0660;
}

namespace GameVtableIndex {
    // CONFIRMED WRONG for v1074 — see GameRva::CROLE_SET_COMMAND_REAL above.
    // Kept for reference only; do not use for SetCommand.
    constexpr size_t CRole_SetCommand = 59;
}

// =====================================================================
// Game — static accessor for game data
// =====================================================================
class Game
{
public:
    static void Init() {
        g_qwModuleBase = (ULONG64)GetModuleHandleA(nullptr);
        spdlog::info("[game] Base: 0x{:X}", (uintptr_t)g_qwModuleBase);
    }

    static uintptr_t Base() { return (uintptr_t)g_qwModuleBase; }

    template <typename T>
    static T Resolve(uintptr_t rva) {
        return reinterpret_cast<T>(Base() + rva);
    }

    static CRoleMgr* GetRoleMgr() {
        if (!g_qwModuleBase) return nullptr;
        // v1074 [LIVE-VERIFIED]: dereference the static pointer to reach the heap CRoleMgr.
        // Confirmed end-to-end: *(base+ROLE_MGR_PTR) -> CRoleMgr -> m_pHero == "Kinux".
        return *Resolve<CRoleMgr**>(Offsets::ROLE_MGR_PTR);
    }

    // Session 10 [SAFETY FIX]: returns nullptr, always.
    //
    // Offsets::GAME_MAP (0x4E02E0) is confirmed garbage on v1074 — a live
    // probe dereferenced it to 0xFF10408B416074C0. It was previously returned
    // anyway on the assumption that callers "fail closed" because they guard
    // on m_sizeMap.iWidth > 0. That assumption was WRONG and dangerous:
    // base+RVA is a NON-NULL pointer into mapped image memory, so m_sizeMap
    // reads arbitrary image bytes which may well pass the guard — and
    // base_hunt_plugin then calls map->IsWalkable(), dereferencing an
    // equally arbitrary m_pCellInfo. That is a live crash path.
    //
    // Returning nullptr makes every `if (map)` guard genuinely fail closed.
    // Terrain now comes from the .DMap files instead — see mapdata.h and
    // GetCurrentMapGrid(); the only thing still read from game memory is the
    // current map id, via GetCurrentMapId() below.
    // Now returns a CGameMap built from the client's own .DMap file for the
    // current map (see mapdata.h), NOT the game's in-memory map object.
    // Offsets::GAME_MAP is confirmed garbage on v1074 and returning it was a
    // live crash path: base+RVA is non-null, so m_sizeMap read arbitrary image
    // bytes that could pass callers' `iWidth > 0` guard, after which
    // IsWalkable() would dereference an equally arbitrary m_pCellInfo.
    //
    // The file-backed view has correct dimensions, mask and altitude, so every
    // existing consumer (CanReach/CanJump/FindPath, plugins, overlay) works
    // unchanged — and it returns nullptr rather than garbage when no map is
    // loaded, so the null guards those callers already have fail closed.
    static CGameMap* GetMap() {
        return GetFileBackedGameMap();
    }

    // Current map id, via the pointer chain found by four-map differential.
    // Returns 0 if unavailable. SEH-guarded: the pointer is only valid once
    // the client has a map loaded.
    static OBJID GetCurrentMapId() {
        if (!g_qwModuleBase) return 0;
        OBJID id = TryReadMapId(Offsets::CURRENT_MAP_ID);
        if (!id) id = TryReadMapId(Offsets::CURRENT_MAP_ID_ALT);
        return id;
    }

private:
    static OBJID TryReadMapId(uintptr_t rva) {
        __try {
            const uint32_t v = *reinterpret_cast<uint32_t*>(Base() + rva);
            // Map ids are small. Rejecting anything else means a bad read
            // reports "unavailable" instead of a plausible-looking wrong map,
            // which would silently send pathfinding to the wrong terrain.
            return (v >= 100 && v <= 20000) ? (OBJID)v : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }
public:

    // Session 10 [SAFETY]: Offsets::ENTITY_SET/ENTITY_INFO are marked
    // UNVERIFIED for v1074 above (same family as the old GAME_MAP RVA, which
    // WAS confirmed garbage and fixed — these two siblings never got the
    // same treatment). GetEntitySet()'s Resolve() is a pure address cast (no
    // memory touched here), so the real risk lives in CEntitySet.cpp/
    // CEntityInfo.cpp's field reads on the returned pointer — see the
    // SEH-guarded helpers there. GetEntityInfo() is different: it actually
    // DEREFERENCES base+RVA to read the pointer stored there, so THAT read
    // itself needs guarding here, at the one place both callers go through.
    static CEntitySet* GetEntitySet() {
        if (!g_qwModuleBase) return nullptr;
        return Resolve<CEntitySet*>(Offsets::ENTITY_SET);
    }

    static CEntityInfo* GetEntityInfo() {
        if (!g_qwModuleBase) return nullptr;
        __try {
            return *Resolve<CEntityInfo**>(Offsets::ENTITY_INFO);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    static CHero* GetHero() {
        return CHero::GetSingletonPtr();
    }
};

namespace GameCall {
    using CEntity_RenderVisualFn = void(*)(void*);
    using CNetClient_GetInstanceFn = void*(*)();
    using CNetClient_SendMsgFn = uint32_t(*)(int64_t, const uint8_t*, int64_t);
    using CRole_SetCommandFn = void(*)(CRole*, CCommand*);
    using CStatTable_GetValueFn = int(*)(const CStatTable*, int);
    using CHero_GetCurrentManaFn = int(*)(const CHero*);
    using CHero_GetMaxHpFn = int(*)(const CHero*);
    using CHero_GetMaxManaFn = int(*)(const CHero*);
    using CHero_FatalStrikeFn = void(*)(CHero*, OBJID);
    using CHero_WalkFn = void(*)(CHero*, int, int);
    using CHero_JumpFn = void(*)(CHero*, int, int);
    using CHero_StartMiningFn = void(*)(CHero*);
    // Session 5, live-traced native message senders — see GameRva:: comment above.
    using CNetClient_SingletonAccessorFn = void*(*)();
    using CNetClient_SendMapItemMsgFn = void*(*)(void* netClient, uint32_t itemId, uint16_t x, uint16_t y, uint32_t plus);
    // Session 8, live-confirmed real SendMsg — see GameRva:: comment above.
    using CNetClient_ConnectionSingletonFn = void*(*)();
    using CNetClient_SendMsgRealFn = uint8_t(*)(void* connectionObject, const uint8_t* buffer, uint32_t length);
    // Session 9, live-confirmed real SetCommand — see GameRva:: comment above.
    using CRole_SetCommandRealFn = void(*)(void* self, CCommand* cmd);
    using MsgUpdate_ProcessFn = uint8_t(*)(void*);
    using TradeWindow_HandleMessageFn = uint64_t(*)(int64_t*, int, int64_t);
    // CGameUI::ShowMsg — adds a chat message to the display.
    // sender/receiver/suffix/message are std::string* (MSVC ABI).
    using CGameUI_ShowMsgFn = uint64_t(*)(
        void* gameUI,             // CGameUI* this
        const std::string* sender,
        const std::string* receiver,
        const std::string* suffix,
        const std::string* message,
        uint32_t color,           // ARGB
        uint16_t style,
        uint16_t channel,         // 0x7D1 = whisper
        uint32_t timestamp,
        uint32_t extra
    );

    inline CEntity_RenderVisualFn CEntity_RenderVisual() {
        static auto fn = Game::Resolve<CEntity_RenderVisualFn>(GameRva::CENTITY_RENDER_VISUAL);
        return fn;
    }

    inline CNetClient_GetInstanceFn CNetClient_GetInstance() {
        static auto fn = Game::Resolve<CNetClient_GetInstanceFn>(GameRva::CNETCLIENT_GET_INSTANCE);
        return fn;
    }

    inline CNetClient_SendMsgFn CNetClient_SendMsg() {
        static auto fn = Game::Resolve<CNetClient_SendMsgFn>(GameRva::CNETCLIENT_SEND_MSG);
        return fn;
    }

    inline CStatTable_GetValueFn CStatTable_GetValue() {
        static auto fn = Game::Resolve<CStatTable_GetValueFn>(GameRva::CSTATTABLE_GET_VALUE);
        return fn;
    }

    inline CHero_GetMaxHpFn CHero_GetMaxHp() {
        static auto fn = Game::Resolve<CHero_GetMaxHpFn>(GameRva::CHERO_GET_MAX_HP);
        return fn;
    }

    inline CHero_GetCurrentManaFn CHero_GetCurrentMana() {
        static auto fn = Game::Resolve<CHero_GetCurrentManaFn>(GameRva::CHERO_GET_CURRENT_MANA);
        return fn;
    }

    inline CHero_GetMaxManaFn CHero_GetMaxMana() {
        static auto fn = Game::Resolve<CHero_GetMaxManaFn>(GameRva::CHERO_GET_MAX_MANA);
        return fn;
    }

    inline CHero_FatalStrikeFn CHero_FatalStrike() {
        static auto fn = Game::Resolve<CHero_FatalStrikeFn>(GameRva::CHERO_FATAL_STRIKE);
        return fn;
    }

    inline CNetClient_SingletonAccessorFn CNetClient_SingletonAccessor() {
        static auto fn = Game::Resolve<CNetClient_SingletonAccessorFn>(GameRva::CNETCLIENT_SINGLETON_ACCESSOR);
        return fn;
    }

    inline CNetClient_SendMapItemMsgFn CNetClient_SendMapItemMsg() {
        static auto fn = Game::Resolve<CNetClient_SendMapItemMsgFn>(GameRva::CNETCLIENT_SEND_MAPITEM_MSG);
        return fn;
    }

    inline CHero_WalkFn CHero_Walk() {
        static auto fn = Game::Resolve<CHero_WalkFn>(GameRva::CHERO_WALK);
        return fn;
    }

    inline CNetClient_ConnectionSingletonFn CNetClient_ConnectionSingleton() {
        static auto fn = Game::Resolve<CNetClient_ConnectionSingletonFn>(GameRva::CNETCLIENT_CONNECTION_SINGLETON);
        return fn;
    }

    inline CNetClient_SendMsgRealFn CNetClient_SendMsgReal() {
        static auto fn = Game::Resolve<CNetClient_SendMsgRealFn>(GameRva::CNETCLIENT_SEND_MSG_REAL);
        return fn;
    }

    inline CRole_SetCommandRealFn CRole_SetCommandReal() {
        static auto fn = Game::Resolve<CRole_SetCommandRealFn>(GameRva::CROLE_SET_COMMAND_REAL);
        return fn;
    }

    // Session 8: resolves the live per-session connection object
    // (CNetClient_ConnectionSingleton()->+0x20). NOT stable across relaunches
    // or reconnects — call fresh each time, don't cache long-term.
    inline void* ResolveConnectionObject() {
        auto singleton = CNetClient_ConnectionSingleton();
        if (!singleton) return nullptr;
        void* outer = singleton();
        if (!outer) return nullptr;
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(outer) + 0x20);
    }

    inline CHero_JumpFn CHero_Jump() {
        static auto fn = Game::Resolve<CHero_JumpFn>(GameRva::CHERO_JUMP);
        return fn;
    }

    inline CHero_StartMiningFn CHero_StartMining() {
        static auto fn = Game::Resolve<CHero_StartMiningFn>(GameRva::CHERO_START_MINING);
        return fn;
    }

    inline MsgUpdate_ProcessFn MsgUpdate_Process() {
        static auto fn = Game::Resolve<MsgUpdate_ProcessFn>(GameRva::MSGUPDATE_PROCESS);
        return fn;
    }

    inline TradeWindow_HandleMessageFn TradeWindow_HandleMessage() {
        static auto fn = Game::Resolve<TradeWindow_HandleMessageFn>(GameRva::TRADEWINDOW_HANDLE_MESSAGE);
        return fn;
    }

    inline CGameUI_ShowMsgFn CGameUI_ShowMsg() {
        static auto fn = Game::Resolve<CGameUI_ShowMsgFn>(GameRva::CGAMEUI_SHOW_MSG);
        return fn;
    }
}

// Returns true if a name appears in a comma/semicolon-separated whitelist (case-insensitive).
inline bool IsNameWhitelisted(const char* name, const char* whitelist)
{
    if (!name || !name[0] || !whitelist || !whitelist[0])
        return false;
    const char* p = whitelist;
    while (*p) {
        // skip delimiters/whitespace
        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ',' && *p != ';' && *p != '\n' && *p != '\r' && *p != '\t')
            ++p;
        // trim trailing spaces
        const char* end = p;
        while (end > start && *(end - 1) == ' ')
            --end;
        size_t len = end - start;
        if (len > 0 && _strnicmp(start, name, len) == 0 && name[len] == '\0')
            return true;
    }
    return false;
}

// Returns true if any non-whitelisted player (other than heroId) is on the entity list.
inline bool AreOtherPlayersNearby(OBJID heroId, const char* whitelist = nullptr)
{
    // Session 9: was iterating CRoleMgr::m_deqRole, which is not a deque on
    // v1074 (see entities.h). Uses the heap-scan entity list instead.
    for (const CRole* role : Entities::Get()) {
        if (!role)
            continue;
        if (!role->IsPlayer() || role->GetID() == heroId)
            continue;
        if (whitelist && IsNameWhitelisted(role->GetName(), whitelist))
            continue;
        return true;
    }
    return false;
}

// Convenience wrapper — resolves hero ID automatically.
inline bool ArePlayersNearby(const char* whitelist = nullptr)
{
    const CHero* hero = Game::GetHero();
    return AreOtherPlayersNearby(hero ? hero->GetID() : 0, whitelist);
}
