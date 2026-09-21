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

## 5. Status (authoritative) — REVISED 2026-09-20 late: the server has flipped

**The v1074 client can no longer log in and play.** The "logged-in, feature-exercised v1074
decrypted image" window (old P1) is CLOSED and will not happen; do not build or wait on a v1074
play session. What v1074 coverage is still gettable is offline/passive only, and the diff baseline
is what we already hold. The v1074 bot is down on every machine, so the critical path is
**restoring the bot on the NEW version**; there is no old-version continuity to preserve, so
both the PC and the VM move to the new version (the earlier "stagger the update" idea is dropped).

### Done
- [x] P0a  Freeze: official launcher/updater not run on the PC; HOLD posted to TEAM_NOTES
- [x] P0b  Byte-exact copy of the install -> `E:\CO_Snapshots\v1074_2026-09-20\install` (53,309/53,309 files, 0 failed,
      `ImConquer.exe` SHA-256 = baseline; full SHA-256 manifest `manifest_E.csv`, 53,308 rows). Verified independently
      2026-09-20: file counts, exe hash, random re-hash sample, live install untouched (`version.json` 1074).
      One file is on disk but not in the manifest: `ini\cache.dat` (0 bytes; identical hash on live/E:/F:) — harmless.
- [x] P0c  Second copy `F:\CO_Snapshots\v1074_2026-09-20\install` (53,309 files, exe hash matches)
- [x] P0d  Non-git artifacts backed up to E: — `CO99\scratchpad` (image_dump.bin, code_dump.bin, pktmon captures, object dumps)
      and the full `.claude` project dir (all memory + transcripts). (Git tag `pre-update-v1074`: see below.)
- [x] Capture tooling built (kept — now for the NEW version): `src/imgdump.cpp` (CMake target `imgdump`; read-only full-image +
      region map + page hashes + paired object-graph dumper; smoke-tested off-game on a harmless process incl. the
      pointer-graph path), `tools/imgdump_report.py` (coverage / packed-vs-clear per page / union image),
      `tools/inject_dll.ps1`. Its object roots are v1074-specific (`kRva*` constants) and only arm when the host PE stamp is
      `0x6A51CFB9`; on any other build it runs in generic mode (image + sections + modules + vmmap) until the roots are re-derived.

### What v1074 coverage is still gettable (offline, passive, from the preserved copy)
- [ ] V1  Determine the exact failure mode of the preserved client: launch a DISPOSABLE working copy
      (`F:\CO_Work\v1074_run`, made from the F: mirror — never the pristine snapshots, never the live install) and record whether it
      reaches the login screen (startup/menu/login-UI code decrypts without a server → capturable) or blocks / forces an update.
      Record child processes, network endpoints, files it modifies, `ImConquer.log`.
- [ ] V2  If it reaches the login screen: `imgdump` checkpoints there (and after opening every login-screen UI: server list,
      settings, etc.) to add whatever Themida decrypts beyond the existing 17% `image_dump.bin`. Report coverage with
      `imgdump_report.py`.
- [x] V3  (DONE 2026-09-20; identity check 19/19 verified entries resolve to their own address) **The real v1074 diff baseline (already preserved):** generate the offset registry (masked signature per verified RVA
      + xref signatures for every verified global) from `game.h` (`Offsets::`/`GameRva::`) + `image_dump.bin`/`code_dump.bin`
      + the struct headers -> `V1074_OFFSET_REGISTRY.md` / `v1074_offset_registry.json`, plus a signature matcher
      (`tools/sigscan.py`) validated on the identity pair (v1074 vs itself: every signature must hit its own RVA).

### The NEW version (where full capture is actually possible)
- [ ] N1  Get it via **fresh install -> update, in a throwaway location, never the live client** (a fresh install ships an older
      build and updates up to current). Record the fresh install's `version.json` + hashes BEFORE it updates, snapshot the base,
      let it update, snapshot the result.
- [ ] N2  Rehearsal: the fresh-base -> updated pair is a cheap KNOWN pair to validate the whole diff toolchain (registry,
      sigscan, ghidriff/Diaphora) before depending on it for v1074 -> new. If the fresh base is older than v1074 it also recovers
      the pre-v1074 baseline that the last update lost (base -> v1074 validation against work already done).
- [ ] N3  Full in-game capture on the new client (it CAN log in): `imgdump` checkpoints at login screen / world idle / after a
      guided feature exercise (open every panel, each skill class, combat, trade, warehouse in/out, teleport & map changes,
      mine, use items) so one session forces maximum Themida decryption; plus paired object layouts (re-derive the roots first)
      and a wire capture (`pktmon filter add -p 9959` / `-p 5816`, `--pkt-size 0`, `pktmon format ... --hex`).
- [ ] N4  Diff new vs the preserved v1074 baseline; re-derive shifted RVAs/offsets; PE-stamp build fence (`0x6A51CFB9` -> new stamp);
      port the bot; re-verify with the self-test harness.

### Still deprioritised (old-client concerns): Twin City live-check, mana offset, warehouse-deposit loop, warn-log spam
None can be live-tested until the bot runs on the new version anyway; the mana/HP method (dereference the stat-table record,
rank-correlate, sentinel-on-failure) is the template when it is picked up again.

### Registry facts worth knowing (V3)
- `docs/investigation/V1074_OFFSET_REGISTRY.md` / `v1074_offset_registry.json`: 45 RVA entries (19 verified: 11 code + 8 data) + 177
  struct-field offsets scraped from the headers. Verdicts come from a reviewed `OVERRIDES` table (cited reasons); an early
  tag-scrape mis-read several entries (e.g. `CNETCLIENT_CONNECTION_SINGLETON` -> "garbage", `CURRENT_MAP_ID` -> "wrong"), which is why.
- Tooling: `tools/build_offset_registry.py` (generate), `tools/sigscan.py` (locate in another image; `--identity` self-check).
  Globals are relocated through code cross-references; MSVC magic-static accessors are byte-identical templates, so those use the
  enclosing function's own start signature + the site's offset (a local window matched sibling globals and gave wrong answers —
  the identity check caught it: 16/19 -> 17/19 -> 19/19).
- Limits: the source image is the existing `image_dump.bin` (17% of the image; code section fully present). Only ~19 items are
  byte-locatable; struct-field offsets (CHero+0x3D0 etc.) are NOT signature-locatable and must be re-verified at runtime on the new build.
  `code_section` coverage of Themida-lazily-decrypted functions in the old dump is unknown (a function still encrypted at dump time
  would have no usable signature) — all 11 verified code entries did decode, so the ones we depend on are fine.
