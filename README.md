# CoClassic

A C++ DLL injection bot and overlay for Classic Conquer 2.0 (ImConquer.exe, 64-bit). Uses [Microsoft Detours](https://github.com/microsoft/Detours) for function hooking and [Dear ImGui](https://github.com/ocornut/imgui) for the in-game overlay UI.

![img.png](ImConquer_CIxoWIfUTQ.png)
![img_1.png](ImConquer_xBNuEcy9cU.png)
## Prerequisites

- **Windows 10/11** (x64)
- **Visual Studio 2022** (v143 toolset) or **JetBrains Rider** with MSVC
- **CMake 3.20+** (if using the CMake build)
- C++20 support

All dependencies are vendored — no package manager needed.

## Building

### Visual Studio / Rider

1. Open `coclassic.sln`
2. Select **Release | x64**
3. Build the solution (builds both the main DLL and `launcher.exe` — `coclassic.sln` has projects for both, see Project Structure)

> **Note:** on at least one dev machine, command-line `MSBuild.exe` against `coclassic.sln` fails outright with `MSB8020` (`PlatformToolset=v143` not found) because the installed Visual Studio Build Tools version doesn't include that exact toolset — a pre-existing environment mismatch, not a project-file bug. If you hit this, either install the v143 toolset component, or retarget the solution (Project menu → Retarget Solution) to whatever toolset is actually installed. CMake (below) sidesteps this entirely by auto-detecting a working toolset.

### CMake

```bash
cmake -B build -A x64
cmake --build build --config Release
```

CMake auto-detects whatever Visual Studio toolset is installed (add `-G "Visual Studio 17 2022"` etc. to pin a specific one). **This is the actively maintained build path** — `coclassic.vcxproj` and `injector/launcher.vcxproj`'s source-file lists must both be kept in sync with `CMakeLists.txt` by hand when adding files (see Build Notes).

### Output

| Artifact | Path |
|----------|------|
| Main DLL | `build/bin/Release/coclassic.dll` |
| Launcher | `build/bin/Release/launcher.exe` |
| Tests | `build/bin/Release/map_tests.exe` |

> **Note:** If the linker fails with LNK1104 ("file in use"), the DLL is still injected in a running game process. Uninject or close the game before rebuilding.

## Usage

1. Build the solution (see above)
2. Run `launcher.exe` **as Administrator** (the game itself requires elevation, and the launcher auto-requests it). This opens the persistent **CoClassic Account Manager** window — a plain native Win32 window, not part of the in-game overlay. No console windows appear anywhere (neither the launcher's own nor one per game process for the bot log); everything logs to `launcher.log`/`coclassic.log` next to their respective executables instead.
   - **Add Account** prompts for a label, username, password, server, and optionally a per-account SOCKS5 proxy — credentials are DPAPI-encrypted, tied to the current Windows user account, stored in `accounts.dat` next to `launcher.exe`.
   - **Login** (per account row) launches the game, injects the DLL, and drives the login screen itself — the login window is custom-rendered with no real Win32 controls, so this is synthetic keyboard input (Tab into the username field, Tab into the password field, Enter), not `WM_SETTEXT`. No manual login needed. The row's status tracks progress live: Launching → Injecting → Logging in... → Running.
   - **Exit** (replaces Login once an account is running) stops supervising that account and closes its game window — a hard stop, the manager doesn't wait around to confirm the OS has actually finished tearing the process down before reflecting the exit in the UI.
   - Several accounts can run concurrently, each its own independent game process, started/stopped/managed independently from the same window.
   - Closing the manager window (the X button) hides it to the system tray rather than exiting — every account keeps running and being supervised in the background. Double-click the tray icon to bring the window back; right-click it for **Exit Manager**, which stops supervising every account (without killing any already-running games) and actually quits the process.
   - If a game process crashes, that account automatically relaunches and re-logs in, and any plugin that was enabled before the crash (autohunt, mining, etc.) resumes automatically — one-shot, only fires after a crash, never on a fresh login.
   - If an account gets disconnected in-game (network hiccup, a manual in-game "Disconnect") without the game process itself exiting, the manager notices the login screen reappearing and reconnects in place, no relaunch needed — including dismissing the "connection interrupted" banner that shows up first. Bounded to 5 consecutive failed reconnect attempts before giving up and forcing a full relaunch instead.
3. Press **Insert** in any game window to toggle its overlay

### Launcher Networking

Each account can launch directly, or route its own game login connection through a SOCKS5 relay — proxy settings (host:port, optional auth) are configured per-account in the Add Account flow and saved with that account's profile. There's no separate global proxy prompt or command-line flags; every account's networking choice is self-contained.

Before launching a proxied account, the launcher tests the tunnel:

```text
local launcher -> SOCKS5 proxy -> game login server
```

If the test fails, that account's game is not launched. For example, PIA SOCKS5 may authenticate successfully but return `host unreachable` for `login.conqueronline.net:9959`; that means PIA accepted the credentials but its SOCKS5 endpoint could not reach the game server/port.

SOCKS5 mode temporarily rewrites the game's `servers.json` login target to `127.0.0.1:<relay-port>` for the duration of that account's session, and restores it once the relay stops. Concurrent proxy-mode launches from different accounts are serialized around this rewrite (a short critical section spanning patch → launch → the new game process having had a chance to read its config) so two accounts starting in proxy mode at the same time can't clobber each other's rewrite of the same shared file.

The kill-switch is on by default: if a SOCKS5 tunnel was established and the proxied connection then closes while that account's game process is still running, the launcher terminates that process instead of allowing continued play after a proxy failure.

> **Note:** proxy-mode accounts are implemented (unchanged mechanism from the previous CLI-driven flow, just triggered per-account from Add Account instead) but not yet live-verified end-to-end through the current manager — the maintainer doesn't use SOCKS5.

#### Using a VPN Instead of SOCKS5

A full VPN such as PIA VPN is managed by Windows and the VPN client, not by the launcher relay. To use a VPN:

1. Connect the VPN first.
2. Verify your public IP changed.
3. Add the account without enabling its SOCKS5 proxy option, and click Login normally.

In VPN mode the game connects normally through the system network stack. The launcher does not currently verify VPN state or force traffic through a specific VPN adapter.

### Overlay Tabs

| Tab | Description |
|-----|-------------|
| **Player** | Hero stats, inventory, equipment |
| **Map** | Map info, minimap (pan/zoom/click-to-jump), entity table |
| **Automation** | Per-plugin settings and controls, sidebar grouped by category: Hunting (own sub-tabs), Gathering (Mining, Mule), Travel & Movement (Travel, Follow), Combat & Skill Automation (Artisan Spammer, Skill Trainer), Visual Aids (Aim Helper, Revive Helper) |
| **Notifications** | Discord webhook config + Whisper/Loot Drop/Item notification toggles |
| **Developer Tools** | RE/debug tools not needed for normal use — Native Pickup/Jump/Walk Test, Map Probe, Monster Stat Scan, Combat Test, Logging & Diagnostics |
| **Packets** | Live packet logger |

## Features

### Plugin System

Plugins are C++ classes implementing the `IPlugin` interface. They are compiled directly into the DLL — no dynamic loading.

| Plugin                       | Description |
|------------------------------|-------------|
| **Melee Hunt** / **Archer Hunt** | Hunting automation with zone selection (Circle/Polygon/Route/Map-Wide), combat, loot, town runs, an AutoHunt (farm) / AutoLevel (gate engagement by monster danger tier relative to hero level, per-character slider) goal toggle, spawn-density-weighted exploration with a tunable explore-vs-exploit bias, and Player Safety/Paranoia Mode player-avoidance |
| **Mining**                   | Mine-travel automation with warehouse storage and mule trading |
| **Mule**                     | Market trade helper that accepts trades from whitelisted players |
| **Travel**                   | Cross-map travel via portals, NPCs, and VIP teleport gateways |
| **Follow**                   | Follow a target player with mob dodging and pathfinding |
| **Aim Helper**               | Draws cross markers at entity jump destinations |
| **Revive Helper**            | Guild dead filter — suppresses rendering of non-dead entities |
| **Artisan Spammer**          | Skill spamming automation |
| **Skill Trainer**            | Automated skill training with casting, sitting, and potions |

### Core Systems

- **Overlay** — Hooks `IDXGISwapChain::Present` for ImGui rendering on the game's D3D10.1 device
- **Entity Hooks** — `RenderEntityVisual` hook dispatches per-entity callbacks to plugins; a background heap scan (`entities.cpp`) also drives live entity/ground-item enumeration (`Entities::Get()`/`MapItems::Get()`)
- **Pathfinder** — Singleton jump/walk-by-waypoint path executor with stuck detection; movement pacing (speedhack-aware) is supplied live per-call via a callback rather than fixed once per route
- **Gateway Graph** — Dijkstra pathfinding through inter-map portals and gateways
- **Config** — Per-character INI persistence with autosave
- **Packet Logger** — Hooks `CNetClient::SendMsg` to log outbound packets
- **Discord Webhooks** — Whisper notification forwarding
- **Auto-Login / Crash-Recovery Supervision** (`injector/`) — DPAPI-encrypted multi-account credential store, Tab/Enter-driven login automation (a single click on the username field, then keyboard-only navigation — no coordinate math past that), and a supervisor loop that auto-relaunches and re-logs-in after a crash, resuming any plugin that was enabled beforehand
- **Player Safety / Paranoia Mode** — detects nearby non-whitelisted players. Player Safety fully retreats to Market and idles once a player has lingered too long. Paranoia Mode instead actively interrupts hunting (skips engaging a target and skips ordinary loot, walking toward a threat-biased destination instead) for as long as a threat is detected, plus a separate bucket-level "camping" detector (distinct from the per-tick threat check) that specifically avoids a spot another player has occupied continuously for a while, with an escalating recheck backoff, rather than reacting to someone just passing through. Both Player Safety and Paranoia are positioning/distance-based only — no facing/camera data is available for other players.
- **Monster/Hero Level & Danger Tier** — `CRole::GetLevel()`/`GetDangerTier()` expose the green/white/red/black name-color tier (relative to the hero's own level) for any entity, monster or player — the basis for AutoLevel's engagement gate above
- **Spawn Memory** — a per-map heatmap of observed monster positions that biases idle-exploration destinations toward historically monster-dense areas, with a tunable chance to deliberately favor unscanned areas instead

## Project Structure

```
coclassic/
├── src/                    # Main DLL source
│   ├── dllmain.cpp         # Entry point and initialization
│   ├── overlay.cpp/h       # ImGui overlay and Present hook
│   ├── hooks.cpp/h         # Entity render hook dispatcher
│   ├── game.h              # Game accessors and offsets
│   ├── pathfinder.cpp/h    # Path execution service
│   ├── gateway.cpp/h       # Inter-map gateway graph
│   ├── config.cpp/h        # INI settings persistence
│   ├── packets.cpp/h       # Packet logger
│   ├── C*.h/cpp            # Game struct overlays (CRole, CHero, CGameMap, etc.)
│   └── plugins/            # Plugin implementations
│       ├── plugin.h         # IPlugin interface
│       ├── plugin_mgr.cpp/h # Plugin manager singleton
│       └── *_plugin.cpp/h   # Individual plugins
├── injector/               # Standalone launcher/account-manager executable
│   ├── main.cpp             # WinMain, persistent account-manager GUI window, proxy/relay, per-account supervision loop
│   ├── account_session.h    # AccountSession — per-account runtime state, one worker thread per running account
│   ├── credentials.cpp/h    # DPAPI-encrypted multi-account credential store (accounts.dat)
│   ├── auto_login.cpp/h     # Tab/Enter-driven login automation (initial login + in-place reconnect after a disconnect)
│   └── launcher.vcxproj     # VS project for this executable (kept in sync with CMakeLists.txt by hand, see Build Notes)
├── tests/                  # Unit tests
│   └── map_tests.cpp
├── tools/                  # Standalone diagnostic scripts (not part of the build)
│   ├── parse_minidump.ps1   # Minidump exception/module-list parser
│   ├── symbolize.ps1        # DbgHelp-based address -> symbol/source-line resolver
│   └── dump_streams.ps1     # Minidump stream dumper
├── vendor/                 # Vendored dependencies
│   ├── Detours/            # Microsoft Detours
│   ├── imgui/              # Dear ImGui (D3D10 + Win32 backends)
│   ├── json/               # nlohmann/json
│   └── spdlog/             # spdlog logging
├── coclassic.sln           # Visual Studio solution (main DLL + launcher + tests, see Building)
├── coclassic.vcxproj       # Main DLL project
└── CMakeLists.txt          # CMake build configuration (main DLL, launcher, tests, and diagnostic targets)
```

## Contributing

### Adding a Plugin

1. Create `src/plugins/my_plugin.h` and `src/plugins/my_plugin.cpp`
2. Implement the `IPlugin` interface (`GetName`, `Update`, `RenderUI`, and optionally `OnPreRenderEntity`/`OnPostRenderEntity`)
3. Add a `std::make_unique<MyPlugin>()` call in `PluginManager::Init()`
4. Add the source files to both `coclassic.vcxproj` and `CMakeLists.txt`

### Adding a Game Struct

1. Create a dedicated header (e.g., `src/CMyStruct.h`) with `#pragma pack(push, 1)`
2. Define named fields at the correct offsets, using padding arrays for gaps
3. Add `static_assert(offsetof(...))` checks for every field
4. Never use raw pointer arithmetic — always define proper struct overlays

### Conventions

- Use official game class names: `CRole`, `CHero`, `CMapObj`, `CGameMap`, `CRoleMgr`
- Members use `m_` prefix (`m_id`, `m_posMap`, `m_deqRole`)
- Use standard library types (`std::string`, `std::vector`, `std::shared_ptr`) directly in struct overlays — the game uses the same MSVC ABI
- `Ref<T>` = `std::shared_ptr<T>` for game objects

### Reverse Engineering

This project works with a Themida-packed binary analyzed through a Scylla-dumped copy in Ghidra. When discovering new offsets or struct layouts:

1. Verify findings through Cheat Engine before committing to code
2. Rename functions and data labels in Ghidra when identified
3. Note any Themida-protected (VM'd) functions — static analysis won't work on those

### Build Notes

- The game uses **D3D10.1**, not D3D11. The overlay hooks the shared `IDXGISwapChain::Present` from DXGI and uses the ImGui D3D10 backend.
- Rider may report `"isSuccess": true` even when the linker fails. Use CLI MSBuild for reliable diagnostics:
  ```
  MSBuild.exe coclassic.sln -t:Rebuild -p:Configuration=Release -p:Platform=x64
  ```
- **`coclassic.vcxproj` and `injector/launcher.vcxproj`'s source-file lists are not auto-synced with `CMakeLists.txt`.** When adding a new `.cpp`/`.h` to the main DLL or the launcher, add it to both the relevant `.vcxproj` and `CMakeLists.txt`, or the CMake build will succeed while the Visual Studio build silently omits the file (this has happened before — several core `coclassic.vcxproj` files were missing for a while before being caught and fixed, and `injector/launcher.vcxproj` itself drifted out of sync the same way: it kept the project's old `injector` name/output after the executable was renamed to `launcher.exe`, and never picked up `credentials.cpp`/`auto_login.cpp` when they were added).
- The Release build emits `coclassic.pdb` alongside `coclassic.dll` (`/Zi` + `/DEBUG` + explicit `/OPT:REF,ICF` in `CMakeLists.txt`, so code layout matches a plain Release build exactly — MSVC otherwise silently disables those optimizations once `/DEBUG` is present). This has no runtime cost and means a crash address from a minidump can be symbolized directly against the shipped binary (`tools/symbolize.ps1`) without reconstructing a separate build.
- If the linker fails with LNK1104 while the DLL is injected into a running game, close the game (or kill `ImConquer.exe`/`launcher.exe`) before rebuilding — the crash-recovery supervisor in `launcher.exe` will otherwise relaunch the game and re-lock the DLL between build attempts.

## Dependencies

| Library | Purpose | Location |
|---------|---------|----------|
| [Microsoft Detours](https://github.com/microsoft/Detours) | Function hooking | `vendor/Detours/` |
| [Dear ImGui](https://github.com/ocornut/imgui) | Overlay UI (D3D10 + Win32) | `vendor/imgui/` |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing | `vendor/json/` |
| [spdlog](https://github.com/gabime/spdlog) | Logging | `vendor/spdlog/` |

System libraries: `d3d11.lib`, `dxgi.lib`, `d3dcompiler.lib`, `winhttp.lib`, `ws2_32.lib`

## Known Issues

- **Some `gateway.cpp` map-ID constants are unverified against current game data** — e.g. `MAP_APE_MOUNTAIN` resolves to a map the game's own data labels "Bird Island," and at least one real hunt-zone map has no gateway routes to/from it at all, so automated travel to it fails outright (this fails safely — the bot disables hunting after repeated failures rather than looping or freezing — but doesn't fix the underlying routing gap).
- **Treasure Bank / Compose Bank NPC deposits are not confirmed working.** The client never confirms these bank windows actually opened server-side (unlike the Warehouse NPC flow, which sends 2 additional confirmation packets these two don't); the bot now gives up cleanly after a few failed attempts instead of the game freezing, but the deposits themselves may not be completing.
- **Player Safety / Paranoia Mode have no visibility into another player's facing or camera** — there is no such field readable anywhere on `CRole`/`CHero`. Both features are positioning/distance-based only.
- **Reconnecting after an in-game disconnect occasionally fails login with correct, unchanged credentials**, resolved only by manually retyping part of the username — not every time, hard to reproduce on demand. Ruled out: garbled/dropped characters (visually and independently verified correct) and a stale pre-filled username field (the field does select-all on focus). Root cause unconfirmed; would need live memory instrumentation of the login form to pin down further.

## Limitations
Nothing is currently omitted from the source — the plugin list above and `PluginManager::Init()` reflect what actually builds and runs.