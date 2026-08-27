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
3. Build the solution

> **Note:** `coclassic.sln`/`coclassic.vcxproj` only builds the main DLL — there is currently no Visual Studio project for `launcher.exe`. Use CMake (below) to build the launcher, or to build everything in one step.

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
2. Run `launcher.exe` **as Administrator** (the game itself requires elevation, and the launcher auto-requests it). A character-select dialog appears first:
   - Pick a saved account to log in automatically, add a new one (label, username, password, server, optional per-account proxy — credentials are DPAPI-encrypted, tied to the current Windows user account, stored in `accounts.dat` next to `launcher.exe`), or skip to fall through to a manual/proxy-only launch.
   - If an account is selected, the launcher starts the game, injects the DLL, and drives the login screen itself (the login window is custom-rendered with no real Win32 controls, so this is coordinate-based synthetic input, not `WM_SETTEXT`) — no manual login needed.
   - The launcher then supervises the game process. If it crashes, it automatically relaunches and re-logs in with the same account, and any plugin that was enabled before the crash (autohunt, mining, etc.) resumes automatically — this resume-on-relaunch behavior is one-shot and only fires after a crash, never on a fresh manual login.
   - Skipping the account picker falls back to the previous behavior: inject into a fresh or already-running game process, log in manually, no auto-relaunch.
3. Press **Insert** to toggle the overlay

### Launcher Networking

The launcher can launch directly, route the game login connection through a SOCKS5 relay, or be used while a system VPN is already connected.

Proxy settings can now be saved per-account in the character-select dialog (prompted when adding a new account). If a saved account with its own proxy settings is selected, those are used directly and the standalone SOCKS5 prompt below is skipped entirely. The prompt only appears when skipping the account picker (or when no accounts are saved yet).

#### Interactive SOCKS5 Prompt

Running `launcher.exe` with no proxy arguments (and no account selected) shows a pre-launch SOCKS5 prompt:

1. Choose whether to use SOCKS5.
2. Enter proxy `host:port`.
3. Choose whether username/password authentication is required.
4. Enter the username and masked password if needed.
5. Choose whether to enable packet logging.
6. Choose whether to enable the kill-switch.

If SOCKS5 is selected and setup is cancelled, invalid, or fails the pre-launch connection test, the launcher aborts before launching the game.

Saved SOCKS5 settings are stored next to `launcher.exe` in `socks5_config.txt`. The saved fields are proxy host, port, auth flag, username, packet logging preference, and kill-switch preference. The SOCKS5 password is never saved.

#### SOCKS5 Command Line

```powershell
launcher.exe --proxy <host:port> [--proxy-user <user>] [--proxy-pass <pass>] [--relay-port <port>] [--target <host:port>] [--packet-log] [--no-kill-switch]
launcher.exe --no-prompt
```

| Option | Description |
|--------|-------------|
| `--proxy <host:port>` | SOCKS5 proxy server, for example `127.0.0.1:1080` |
| `--proxy-user <user>` | SOCKS5 username |
| `--proxy-pass <pass>` | SOCKS5 password |
| `--relay-port <port>` | Local relay port; defaults to the target server port |
| `--target <host:port>` | Override the upstream game server endpoint |
| `--packet-log` | Enables relay packet logging to `relay_packets.log` |
| `--no-kill-switch` | Disables process termination when a proxied connection fails |
| `--no-prompt` | Skips the SOCKS5 prompt and launches without proxy unless `--proxy` is provided |

SOCKS5 mode temporarily rewrites the game `servers.json` login target to `127.0.0.1:<relay-port>` while the launched game is running. The launcher restores `servers.json` after launch and keeps the relay alive until the game exits.

Before launching the game, SOCKS5 mode tests:

```text
local launcher -> SOCKS5 proxy -> game login server
```

If the test fails, the game is not launched. For example, PIA SOCKS5 may authenticate successfully but return `host unreachable` for `login.conqueronline.net:9959`; that means PIA accepted the credentials but its SOCKS5 endpoint could not reach the game server/port.

Packet logging is off by default because `relay_packets.log` can contain sensitive traffic. Enable it only while debugging.

The kill-switch is on by default. If a SOCKS5 tunnel was established and then the proxied connection closes while the game process is still running, the launcher terminates the game process instead of allowing continued play after a proxy failure.

#### Using a VPN Instead of SOCKS5

A full VPN such as PIA VPN is managed by Windows and the VPN client, not by the launcher relay. To use a VPN:

1. Connect the VPN first.
2. Verify your public IP changed.
3. Run `launcher.exe`.
4. Choose **No** in the SOCKS5 prompt, or run `launcher.exe --no-prompt`.

In VPN mode the game connects normally through the system network stack. The launcher does not currently verify VPN state or force traffic through a specific VPN adapter.

### Overlay Tabs

| Tab | Description |
|-----|-------------|
| **Player** | Hero stats, inventory, equipment |
| **Map** | Map info, tools, and the Travel section |
| **Plugins** | Per-plugin settings and controls |
| **Packets** | Live packet logger |

## Features

### Plugin System

Plugins are C++ classes implementing the `IPlugin` interface. They are compiled directly into the DLL — no dynamic loading.

| Plugin                       | Description |
|------------------------------|-------------|
| **Melee Hunt** / **Archer Hunt** | Hunting automation with zone selection, combat, loot, town runs, and Player Safety/Paranoia Mode player-avoidance |
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
- **Auto-Login / Crash-Recovery Supervision** (`injector/`) — DPAPI-encrypted multi-account credential store, coordinate-based synthetic-input login automation, and a supervisor loop that auto-relaunches and re-logs-in after a crash, resuming any plugin that was enabled beforehand
- **Player Safety / Paranoia Mode** — detects nearby non-whitelisted players and either fully retreats to Market and idles (Player Safety) or biases normal hunting decisions — target choice, idle-exploration destination, zone-leash tolerance — away from the detected player while continuing to hunt (Paranoia Mode); both are positioning/distance-based only, no facing/camera data is available for other players
- **Spawn Memory** — a per-map heatmap of observed monster positions that biases idle-exploration destinations toward historically monster-dense areas

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
├── injector/               # Standalone launcher executable
│   ├── main.cpp             # Entry point, account picker, proxy/relay, crash-recovery supervision loop
│   ├── credentials.cpp/h    # DPAPI-encrypted multi-account credential store (accounts.dat)
│   └── auto_login.cpp/h     # Coordinate-based synthetic-input login automation
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
├── coclassic.sln           # Visual Studio solution (main DLL only, see Building)
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

## Limitations
Nothing is currently omitted from the source — the plugin list above and `PluginManager::Init()` reflect what actually builds and runs.