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

# ── Main DLL ────────────────────────────────────────────────────────
add_library(coclassic SHARED
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
target_include_directories(coclassic PRIVATE
    src
    src/plugins
    vendor/Detours/src
    vendor/imgui
    vendor/imgui/backends
    vendor/json/include
    vendor/spdlog/include
)
target_link_libraries(coclassic PRIVATE
    psapi          # GetMappedFileNameW — identifies the active map by its file
    d3d11.lib
    dxgi.lib
    d3dcompiler.lib
    winhttp.lib
    crypt32.lib    # CryptProtectData/CryptUnprotectData — Discord webhook URL at rest (config.cpp)
)
set_target_properties(coclassic PROPERTIES
    OUTPUT_NAME "coclassic"
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
target_compile_options(coclassic PRIVATE $<$<CONFIG:Release>:/Zi>)
target_link_options(coclassic PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)

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

# ── Map tests ──────────────────────────────────────────────────────
add_executable(map_tests
    tests/map_tests.cpp
    src/CGameMap.cpp
)
target_include_directories(map_tests PRIVATE src)
set_target_properties(map_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
