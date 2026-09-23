cmake_minimum_required(VERSION 3.20)
project(coclassic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC: bundled fmt in the pinned spdlog requires /utf-8 (static_assert in base.h).
# The Visual Studio .sln sets this implicitly; the CMake path must set it explicitly.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ── Vendor sources (compiled directly, warnings suppressed) ───────
set(DETOURS_SOURCES
    vendor/Detours/src/detours.cpp
    vendor/Detours/src/modules.cpp
    vendor/Detours/src/disasm.cpp
    vendor/Detours/src/image.cpp
    vendor/Detours/src/creatwth.cpp
    vendor/Detours/src/disolx86.cpp
    vendor/Detours/src/disolx64.cpp
    vendor/Detours/src/disolarm.cpp
    vendor/Detours/src/disolarm64.cpp
    vendor/Detours/src/disolia64.cpp
)
set(IMGUI_SOURCES
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_demo.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_dx10.cpp
    vendor/imgui/backends/imgui_impl_dx11.cpp
    vendor/imgui/backends/imgui_impl_win32.cpp
)
set_source_files_properties(${DETOURS_SOURCES} ${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0")

# ── Main DLL(s) ─────────────────────────────────────────────────────
# Two client builds are supported, each its OWN compiled binary from this exact same
# source tree (see src/field_offsets.h's header comment for why a single binary can't do
# runtime dual-build: struct field offsets are fixed at compile time). `coclassic` is the
# v1074 target (default, unchanged); `coclassic_v1078` defines COCLASSIC_TARGET_V1078 to
# pick the v1078 struct offsets / native-call RVAs instead. build_fence.h in each only
# lists its OWN build, so injecting the wrong one into the wrong process just refuses to
# initialise — see docs/investigation/V1078_OFFSET_FINDINGS.md.
set(COCLASSIC_SOURCES
    src/log.cpp
    src/dllmain.cpp
    src/hwid_spoof.cpp
    src/overlay.cpp
    src/CStatTable.cpp
    src/CRole.cpp
    src/CHero.cpp
    src/CEntityInfo.cpp
    src/CEntitySet.cpp
    src/CGameMap.cpp
    src/entities.cpp
    src/mapdata.cpp
    src/map_items.cpp
    src/action_recorder.cpp
    src/registries.cpp
    src/spawn_memory.cpp
    src/mem_stats.cpp
    src/map_probe.cpp
    src/monster_scan.cpp
    src/config.cpp
    src/profiles.cpp
    src/portal_capture.cpp
    src/hunt_settings.cpp
    src/hunt_stats.cpp
    src/hunt_targeting.cpp
    src/hunt_contest.cpp
    src/hunt_town.cpp
    src/hunt_buffs.cpp
    src/hunt_loot.cpp
    src/inventory_utils.cpp
    src/npc_utils.cpp
    src/revive_utils.cpp
    src/discord.cpp
    src/gateway.cpp
    src/gfx.cpp
    src/hooks.cpp
    src/itemtype.cpp
    src/packets.cpp
    src/net_recv_hook.cpp
    src/pathfinder.cpp
    src/plugins/plugin_mgr.cpp
    src/plugins/base_hunt_plugin.cpp
    src/plugins/melee_hunt_plugin.cpp
    src/plugins/archer_hunt_plugin.cpp

    src/plugins/mining_plugin.cpp
    src/plugins/mule_plugin.cpp
    src/plugins/travel_plugin.cpp
    src/plugins/follow_plugin.cpp
    src/plugins/aim_helper_plugin.cpp
    src/plugins/revive_helper_plugin.cpp
    src/plugins/artisan_spammer_plugin.cpp
    src/plugins/skill_trainer_plugin.cpp
    ${DETOURS_SOURCES}
    ${IMGUI_SOURCES}
)

# Both DLL targets get identical include dirs / link libs / properties / PDB settings —
# defined once here so the two targets can't silently drift apart.
function(coclassic_configure_target target_name output_name)
    target_include_directories(${target_name} PRIVATE
        src
        src/plugins
        vendor/Detours/src
        vendor/imgui
        vendor/imgui/backends
        vendor/json/include
        vendor/spdlog/include
    )
    target_link_libraries(${target_name} PRIVATE
        psapi          # GetMappedFileNameW — identifies the active map by its file
        d3d11.lib
        dxgi.lib
        d3dcompiler.lib
        winhttp.lib
        crypt32.lib    # CryptProtectData/CryptUnprotectData — Discord webhook URL at rest (config.cpp)
        ws2_32.lib     # recv/WSARecv Detour hooks (net_recv_hook.cpp)
    )
    set_target_properties(${target_name} PROPERTIES
        OUTPUT_NAME "${output_name}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    # Session 11: emit a PDB alongside the Release DLL so a crash address can be
    # symbolized directly against the exact binary that's actually running,
    # instead of having to reconstruct a matching build after the fact (which
    # turned out to be unreliable — a from-scratch worktree rebuild with /Zi
    # added via an overridden CMAKE_CXX_FLAGS_RELEASE came out roughly 2x the
    # size with a totally different code layout, because MSVC's linker disables
    # /OPT:REF,ICF by default once /DEBUG is present unless re-enabled — so a
    # naive debug rebuild silently stops folding/stripping code the same way,
    # and its addresses no longer correspond to the shipping binary at all).
    # /Zi + /DEBUG + explicit /OPT:REF,ICF keeps codegen and layout identical to
    # a plain Release build, just with debug records added on top — this has no
    # runtime behavior or performance cost, only extra disk space for the PDB.
    target_compile_options(${target_name} PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(${target_name} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endfunction()

add_library(coclassic SHARED ${COCLASSIC_SOURCES})
coclassic_configure_target(coclassic "coclassic")

# v1078 target — same source, same flags, one extra compile definition (field_offsets.h /
# build_fence.h / game.h all branch on it). Struct-field offsets are VALUE-CONFIRMED
# against character S411; the 3 action-critical native-call RVAs are signature-relocated
# but gated OFF (GameRva::V1078_NATIVE_TESTED = false) until a live pickup/movement/attack
# test passes — see docs/investigation/V1078_OFFSET_FINDINGS.md.
add_library(coclassic_v1078 SHARED ${COCLASSIC_SOURCES})
target_compile_definitions(coclassic_v1078 PRIVATE COCLASSIC_TARGET_V1078)
coclassic_configure_target(coclassic_v1078 "coclassic_v1078")

add_executable(launcher WIN32
    injector/main.cpp
    injector/credentials.cpp
    injector/auto_login.cpp
    injector/launcher.rc
)
target_include_directories(launcher PRIVATE
    vendor/json/include
)
target_link_libraries(launcher PRIVATE
    ws2_32
    crypt32
    shell32
)
set_target_properties(launcher PROPERTIES
    OUTPUT_NAME "launcher"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
if(MSVC)
    # ImConquer.exe's own manifest requires admin (requireAdministrator);
    # CreateProcess can't silently elevate a child, so without this the
    # launcher fails with ERROR_ELEVATION_REQUIRED (0x2E4) unless the user
    # manually runs it as administrator. Requesting elevation here makes
    # Windows prompt UAC automatically and the game process inherits it.
    target_link_options(launcher PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()

# ── Self-test validator (standalone read-only injected DLL) ─────────
add_library(selftest SHARED
    src/selftest.cpp
)
set_target_properties(selftest PROPERTIES
    OUTPUT_NAME "selftest"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── Action tracer (standalone; hardware-breakpoint "find what accesses X") ──
add_library(tracer SHARED
    src/tracer.cpp
)
set_target_properties(tracer PROPERTIES
    OUTPUT_NAME "tracer"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── Game-state explorer (standalone read-only: entities, items, stats, map) ──
add_library(explorer SHARED
    src/explorer.cpp
)
set_target_properties(explorer PROPERTIES
    OUTPUT_NAME "explorer"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── NPC dialog test (standalone; inject mid-session, calls the client's own
#    ActivateNpc and logs m_bNpcActive/m_idActiveNpc — see src/npctest.cpp) ──
add_library(npctest SHARED
    src/npctest.cpp
)
set_target_properties(npctest PROPERTIES
    OUTPUT_NAME "npctest"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── Heap-registry finder (standalone, read-only, single-shot; inject mid-session.
#    Inverse-pointer search: finds the game's own container of live CRole*/CMapItem*
#    so the shape-based heap scanners can be replaced — see src/heapfind.cpp) ──
add_library(heapfind SHARED
    src/heapfind.cpp
)
set_target_properties(heapfind PROPERTIES
    OUTPUT_NAME "heapfind"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── Image/object capture (standalone, READ-ONLY; full decrypted-image + paired
#    live object-layout checkpoints for the v1074 baseline -- see src/imgdump.cpp,
#    docs/investigation/V1074_CAPTURE_SESSION.md). CMake-only like the other
#    standalone diagnostic DLLs (no hand-authored vcxproj). ──
add_library(imgdump SHARED
    src/imgdump.cpp
)
target_link_libraries(imgdump PRIVATE psapi)
set_target_properties(imgdump PROPERTIES
    OUTPUT_NAME "imgdump"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── Net finder (standalone; Detour-hooks Winsock send/WSASend to locate
#    CNetClient::SendMsg — safe alternative to hardware-breakpoint tracing) ──
add_library(netfinder SHARED
    src/netfinder.cpp
    ${DETOURS_SOURCES}
)
target_include_directories(netfinder PRIVATE vendor/Detours/src)
target_link_libraries(netfinder PRIVATE ws2_32 psapi)
set_target_properties(netfinder PROPERTIES
    OUTPUT_NAME "netfinder"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── Recv finder (standalone; Detour-hooks Winsock recv/WSARecv -- inbound
#    counterpart to netfinder, checking whether server->client traffic is
#    plaintext-framed the way outbound turned out to be) ──
add_library(recvfinder SHARED
    src/recvfinder.cpp
    ${DETOURS_SOURCES}
)
target_include_directories(recvfinder PRIVATE vendor/Detours/src)
target_link_libraries(recvfinder PRIVATE ws2_32 psapi)
set_target_properties(recvfinder PROPERTIES
    OUTPUT_NAME "recvfinder"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── Flush finder (standalone; Detour-hooks the confirmed-non-hook-resistant
#    per-tick flush poller at 0x1DD860 to read the pending-send buffer
#    BEFORE it reaches send() -- finds which pipeline stage transforms a
#    message, without touching the anti-tamper-protected 0xE9960/0xBF920
#    region at all) ──
add_library(flushfinder SHARED
    src/flushfinder.cpp
    ${DETOURS_SOURCES}
)
target_include_directories(flushfinder PRIVATE vendor/Detours/src)
set_target_properties(flushfinder PROPERTIES
    OUTPUT_NAME "flushfinder"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Iteration 2 of flushfinder (heartbeat diagnostic added) -- separate
# target/output name so it can inject alongside an already-loaded instance
# without needing a relaunch.
add_library(flushfinder2 SHARED
    src/flushfinder.cpp
    ${DETOURS_SOURCES}
)
target_include_directories(flushfinder2 PRIVATE vendor/Detours/src)
set_target_properties(flushfinder2 PROPERTIES
    OUTPUT_NAME "flushfinder2"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Iteration 3 -- reads the message bytes from connPtr+0x00 (proven location)
# instead of dereferencing the bogus +0x2008 "pointer".
add_library(flushfinder3 SHARED
    src/flushfinder.cpp
    ${DETOURS_SOURCES}
)
target_include_directories(flushfinder3 PRIVATE vendor/Detours/src)
set_target_properties(flushfinder3 PROPERTIES
    OUTPUT_NAME "flushfinder3"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── Map tests ──────────────────────────────────────────────────────
add_executable(map_tests
    tests/map_tests.cpp
    src/CGameMap.cpp
    src/mapdata.cpp        # real .DMap/.scene parsing, off-game (COCLASSIC_GAME_ROOT)
)
target_include_directories(map_tests PRIVATE src vendor/spdlog/include)
set_target_properties(map_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
