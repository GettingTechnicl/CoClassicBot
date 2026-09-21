# Classic Conquer client v1074 — baseline dossier (pre-update snapshot)

Started 2026-09-20 (PC-side), the day the client was found to have a pending update. **DRAFT —
status of each item is marked; update this file as the snapshot proceeds.** Purpose: make the
NEXT client update a cheap, offline, mostly-mechanical comparison instead of the weeks of manual
re-derivation that the pre-v1074 -> v1074 update cost (see "Lesson from last time").

## 1. Baseline identity (measured 2026-09-20, read-only)

| item | value |
|---|---|
| Game install | `F:\Games\Classic Conquer 2.0\` (2.56 GB, 53,309 files; `data` 47,119 files, `c3` 4,263, `map` 1,078, `ini` 166) |
| Client exe | `bin\64\ImConquer.exe`, 18,198,544 bytes, file mtime 2026-07-11 01:13:30 |
| **SHA-256** | `C2B53437EF68D687A1EF0F70C74BCF2DF6027BF82B558E93330C839EB5E1C396` |
| **PE TimeDateStamp** | `0x6A51CFB9` (= 2026-07-11 05:08:09 UTC) |
| PE SizeOfImage / ImageBase | `0x28C5000` (~42.8 MB in memory, Themida-expanded) / `0x140000000` |
| `version.json` | `{"version":1074}` (file last touched 2026-09-08 by the launcher) |
| Other July-11 binaries | `graphic.dll` 2,728,960 · `GraphicData.dll` 759,808 · `Role3D.dll` 513,536 · `TqPackage.dll` 280,576 · `TqPackageWdf.dll` 179,200 (all mtime 2026-07-11 00:13) |
| Old/unchanged since 2022-23 | `ImLauncher.exe`, `D3DCompiler_47.dll`, `d3dx10_43.dll`, `discord_game_sdk.dll`, `xaudio2_9redist.dll`, `crashpad_handler.exe`, `ImBootstrapper.exe`, `data.wdf`, `c3.wdf` |
| Vendor manifest | `integrity.json` (13 KB, 2025-11-23): per-file 64-bit hashes for `ini/*.json` and `map/map/*.DMap` only — NOT a full or cryptographic manifest; our own SHA-256 manifest supersedes it |
| Update endpoints | `update_config.json`: `info_host https://api.conqueronline.net`, `download_host https://download.conqueronline.net`, `updates /launcher.json`, `update_storage LauncherResources/updates` |

**Update state on 2026-09-20:** nothing staged — `LauncherResources\updates\` is empty, `version.json`
still says 1074, no install file is newer than the 2026-07-11 build apart from runtime logs.
The bot's launcher starts `ImConquer.exe` directly (`CreateProcessA`, `injector/main.cpp`), so it
never runs the official launcher/updater; the update will only apply if `ImLauncher.exe` /
`ImBootstrapper.exe` is run. (`dragon_updater.exe` seen running is the Comodo Dragon browser's
updater service — unrelated.)

## 2. What we already hold from v1074 (provenance checked)

| artifact | where | provenance / caveat |
|---|---|---|
| `image_dump.bin` 7,454,720 B (2026-08-22) | `CO99/scratchpad/` (persistent) | Scylla dump; **PE header identical to the baseline exe** (same TimeDateStamp/SizeOfImage) so it is a true v1074 dump. Covers only the image up to ~`0x71C000` of `0x28C5000`; Themida lazily-decrypted regions may be missing. |
| `code_dump.bin` 5,578,752 B (2026-08-22) | `CO99/scratchpad/` | `.text` extract of the above; basis of all static RVA work (function RVAs in `game.h`) |
| pktmon wire captures | `CO99/scratchpad/game.etl`, `game_hex.txt`, `multi.etl`, `multi_hex.txt` (2026-09-18) | game-server handshake, 4 fresh connections; the static-cipher-state finding rests on these (`CONNECTION_DECIPHER_PREP.md`) |
| decrypt harnesses + Comet reference | `CO99/scratchpad/try_*.py`, `scan_for_schedule.py`, `comet5187/` | crypto thread, paused |
| object dumps | `CO99/scratchpad/{bird,e2before,e2after}_*.bin`, `conn_dump/` | hero/stat/rolemgr raw dumps from Aug 22 / Sep 18 |
| Frida traces | `CO99/scratchpad/frida_run*.log` (~110 MB) | send-pipeline trace |
| crash minidumps | `CoClassicBot/crashdumps/*.dmp` | game crashes, several builds |
| in-client map dump | `<install>/mapdump_1075.bin` | our own dump of map 1075 |
| verified offsets/RVAs | `src/game.h` (`Offsets::`, `GameRva::`), `CRole.h`, `CHero.h`, `CItem.h`, `CGameMap.h`; narrative in memory `coclassicbot-live-offsets` (1,100+ lines) and `docs/investigation/*` | tagged `[LIVE-VERIFIED]` / `[STALE]` / `[UNVERIFIED]` |
| bot source at this build | git `master` (HEAD at time of writing `57b5752`), older baseline worktree `CO99/baseline_886ebd0` | not yet tagged |
| project memory + all session transcripts | `~/.claude/projects/C--Users-TerryGluff-Documents-Claude-CO99/` (19 memory files 638 KB; transcripts 367 MB) | machine-local, NOT in git |

**Gaps that make the existing dump insufficient for a real diff:** (a) only ~17% of the image;
(b) taken once, at one moment; (c) no region map (which pages were unreadable/undecrypted);
(d) no live object-layout dumps paired to the same moment; (e) no signature for each verified RVA.

## 3. Lesson from last time (pre-v1074 -> v1074)

The pre-v1074 binary was not kept. Result (see `game.h` GameRva comment, memory `live-offsets`):
code moved **non-uniformly** (every inherited function RVA landed on padding/epilogues/mid-function
— calling any of them crashed the game), data fields moved by a **flat +0x50**, the role container
changed from `std::deque` to a hash-map bucket array, and the whole action layer had to be
re-derived over ~9 sessions. Protocol/data formats mostly survived. Expect the same shape:
**function RVAs stale; struct fields shift by a constant per region; a few containers restructured;
data files and message ids mostly stable.** The bot must therefore be fenced to the exact build
(see plan step "runtime build fence") or it will jump into garbage on the new client.

## 4. What "version comparison" means here (from `THEMIDA_NOTES.md`, `REVAMP_PLAN.md`)

The exe on disk is Themida-packed, so a file diff of `ImConquer.exe` is meaningless; the durable
method is a **diff of two decrypted in-memory images** (ghidriff / Diaphora / BinDiff, or our own
byte-signature matcher — REVAMP_PLAN Phase 3/6 "signature registry + self-test become the update
workflow"). That needs the OLD image captured at full coverage before the old client is gone, plus
a signature (masked prologue bytes) for every verified RVA, plus raw object layouts, so each
verified offset can be located and re-validated in the new build instead of re-hunted.

## 5. Snapshot plan / status (fill in as done)

See the plan agreed in chat on 2026-09-20; checklist below is authoritative for status.

- [ ] P0a  Freeze: official launcher/updater not run; VM-side told to hold (TEAM_NOTES)
- [ ] P0b  Byte-exact copy of the install -> `F:\CO_Snapshots\v1074_2026-09-20\install\`, SHA-256 manifest, read-only
- [ ] P0c  Second copy on another volume (E:), manifest re-verified
- [ ] P0d  Backup of memory dir + transcripts + persistent scratchpad + bot binaries/PDBs; git tag `pre-update-v1074`
- [ ] P1a  Fetch update manifest (needs user OK): new version, changed-file list, sizes, hashes
- [ ] P1b  Full-coverage decrypted-image dumps of the running OLD client at login screen / in-city idle / after exercising features (region map + SHA-256 each)
- [ ] P1c  Live object-layout dumps paired to the image dump (hero, stat table + dereferenced records, role mgr, item registry, net client chain, current map descriptor)
- [ ] P1d  Fresh wire capture with the OLD client: login + handshake + steady state (client-version field, static cipher state)
- [ ] P2a  Verified-offset registry (`V1074_OFFSET_REGISTRY.md`) with masked-prologue signature per RVA, generated from `game.h` + struct headers
- [ ] P2b  Runtime build fence (refuse to arm anything unless PE TimeDateStamp == `0x6A51CFB9`)
- [ ] P3   Only then: apply the update; diff; re-derive; re-verify with the self-test harness
