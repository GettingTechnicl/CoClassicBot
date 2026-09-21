# v1078 capture runbook (read-only, one live client, several checkpoints)

Goal: get the decrypted v1078 image plus enough heap to find the object layouts offline, then **classify the regime**
(`tools/regime_classify.py`) before choosing a port strategy. Nothing here writes to the game.

## What the dumper is (audited 2026-09-21, `src/imgdump.cpp`)
* Runs on its own thread created in `DllMain`; the thread only polls `trigger.txt` / a hotkey chord and does
  `VirtualQuery` + SEH-guarded `memcpy` of process memory to files under `C:\Users\Public\coclassic_capture\`.
* No hooks, no patches, no debug registers, no calls into game code. No game address is used except on the exact v1074
  stamp; on any other build (v1078) it is in *generic mode*: image + sections + modules + vmmap, plus (with `full`)
  every committed readable `MEM_PRIVATE` region. `PAGE_GUARD`/`PAGE_NOACCESS`/uncommitted pages are never touched.
* Tested on a harmless `cmd.exe` (generic mode, heap pack size matches its index). Not yet run against the real Themida-packed
  client - this is the first contact. **Residual risk is detection of a foreign module / remote thread, not stale hooks.**

## Steps
1. Client in-world (Themida decrypts lazily; the login screen decrypts little). Note PID: `(Get-Process ImConquer).Id`.
2. **Elevated** PowerShell (the game runs elevated, injection needs it), from the repo root:
   `powershell -ExecutionPolicy Bypass -File tools\inject_dll.ps1 -ProcessId <PID> -DllPath build\bin\Release\imgdump.dll`
   (a beep confirms it attached).
3. Checkpoints, each with a note of what is on screen (HP/MP, level, silver, map+coords, panel open) so heap dumps can be
   correlated offline: `tools\capture_session.ps1 -Label <name> -Full -Note "..."` (no admin needed).
   Suggested order (decryption is cumulative, so each later dump is a superset for code):
   1. `idle_inworld`  - standing still, note exact HP/MP/level/silver/coords
   2. `after_panels`  - open every panel: inventory, equipment, skills, quests, friends, guild, market, warehouse, settings
   3. `after_combat`  - fight a few monsters with each skill class you have, use potions, pick up items
   4. `after_travel`  - teleport / change maps, use a portal, enter a mine
   5. `after_trade`   - NPC shop buy/sell, warehouse deposit/withdraw
   6. `final_idle`    - idle again, note exact values (a second known state for correlation)
4. Unload: write `quit` to `C:\Users\Public\coclassic_capture\trigger.txt` (or Ctrl+Shift+F10).
5. Analysis (offline, any machine): `python tools\regime_classify.py --old-image <v1074 image_dump.bin> --new-image cp01_*\image.bin cp02_*\image.bin ...`
   then `python tools\sigscan.py --image <merged v1078 image> --fields`. Pass **all** image dumps to the classifier: it merges them.

## Still to capture on v1078 (needs a fresh login, so not from this injection)
* wire capture with `pktmon` on ports 9959 (login) / 5816 (game): the login packet's client-version field and the static
  handshake cipher state.
