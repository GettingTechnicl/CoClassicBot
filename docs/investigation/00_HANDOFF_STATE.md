# CoClassicBot — Handoff / State / Migration doc

Written 2026-09-08 for the migration of all work to the dedicated wired QEMU Win11 VM host.
This is the FIRST thing the new LLM on the VM should read. It captures the durable state, the
open investigation, the pending decisions, the setup gotchas, and — critically — the migration
checklist for information that does NOT live in this repo folder.

---

## 0. MIGRATION CHECKLIST — things NOT in this folder (carry manually or they're lost)

1. **Uncommitted git work.** As of this writing the repo has ~18 modified files + the untracked
   instrumentation (`src/action_recorder.{h,cpp}`, `src/net_recv_hook.{h,cpp}`). A fresh
   `git clone` on the VM would LOSE all of it. Either **commit first**, or **copy the entire
   working directory** (not a clone) so uncommitted + untracked files come along. Recommended:
   commit the instrumentation + in-flight work before/after the move so it's versioned.
2. **Claude memory files.** Not in this folder — they live at
   `C:\Users\TerryGluff\.claude\projects\C--Users-TerryGluff-Documents-Claude-CO99\memory\`
   (`MEMORY.md` + 16 `coclassicbot-*.md` files). The new LLM on the VM starts with EMPTY memory
   and will not have any of this unless you carry these files over and import them (there is an
   `import-memory` capability). These hold: fork audit, known defects, game protection, live
   offsets, registries, project status, leveling system, disconnect investigation, and the
   standing feedback rules. High value — do not skip.
3. **Large analysis artifacts (scratchpad, session-temp, re-derivable).** `image_dump.bin` (~7MB,
   the decrypted image), `code_dump.bin` (~5.5MB), and various `*.bin` memory snapshots were in
   the session scratchpad, NOT this folder. They are the game's decrypted code — useful for
   offline RTTI/xref work but re-derivable via a fresh Scylla-style dump, and arguably should
   NOT go in a shareable repo (they're the game's binary). Copy them separately if you want them;
   the analysis SCRIPTS that use them are preserved in `docs/investigation/scripts/`.

The handoff docs and scripts that WERE in the scratchpad have already been copied into
`docs/investigation/` so they travel with the folder.

---

## 1. Current project state

- The sensory/read layer is revived and live-verified for client build v1074 (offsets, entities,
  items, map, hero stats). Action layer is gated behind `GameRva::VERIFIED_V1074` with some
  live-traced call sites ungated — see `src/game.h` provenance tags.
- **Item ground-loot registry** (`src/registries.{h,cpp}`, `src/heapfind.cpp`) is committed and
  confirmed ghost-free — items read from the game's own global vector, not the heap scan.
- **Roles stay on the heap scan by design** — the registry path for roles was refuted; roles live
  in the CRoleMgr hash map, not a vector. See `ROLES_CONTAINER_NOTE.md`.
- **Uncommitted in-flight work** (see checklist): the disconnect instrumentation
  (`action_recorder`, `net_recv_hook`, the `[actionrate]` line in `packets.cpp`), plus modified
  `spawn_memory`, `mapdata` (scene overlays), pathfinder, hunt plugins, etc.

## 2. The disconnect investigation (the active thread) — distilled

Both bots disconnect periodically. Instrumentation added: a flight recorder (ring buffers of the
last actions + outbound packets + inbound socket events, dumped at Warn on the GetMaxHp sentinel)
and a once-per-second `[actionrate]` line. Clean dataset = 39 sessions (2 excluded for being
silently throttled at Trace — see Fix A).

**Ruled OUT as the cause:**
- Memory / OOM (no growth, no crash dumps).
- Action rate / speedhack (bots died at LOWER action rates than they repeatedly survived).
- Phantom/stale targets (0 of 8 recorded actions hit a ghost).
- The faulty WAP uplink cable (re-terminated 10/100 → clean 1G, 2026-09-07 20:22 CDT; no change).
- Duplicate-spawn launcher kicks as the BULK (real but only ~2 of 31, all Shooter411).

**Categorized (the buckets):**
- **Shared simultaneous events (2 pairs, 0–47ms apart on both accounts):** the nightly
  maintenance kick (GracefulClose FIN) and one ECONNRESET pair (network-layer reset). External.
- **Silent per-client deaths (7):** never simultaneous across accounts → per-client crash,
  best-fit the Themida-heavy-client angle. Distinct problem from server closes.
- **Independent GracefulClose (~26 unexplained):** THE open question. A deliberate per-account
  server FIN at scattered times. The logs CANNOT distinguish "WiFi jitter → server drops the
  laggy connection" from "server-side per-account decision (detection/idle/rule)" — both produce
  an identical independent graceful close.

**The decisive next step:** run the bot on a clean WIRED connection (the whole point of the VM
move) and compare independent-GracefulClose frequency vs the WiFi baseline.
- Persist at similar rate → **server-side** (detection/idle/rule), not fixable locally.
- Largely stop → local WiFi was the cause.
- NOTE: moving to the VM changes machine + VM + network at once, so "persist = server-side" is
  the confident read; "stop" can't fully isolate which variable. Keep instrumentation ON at Warn
  so the comparison is apples-to-apples.

**Interpretation caveats learned the hard way:**
- GracefulClose is NON-diagnostic — a confirmed-benign maintenance kick produced the identical
  fingerprint. "Server closed us" ≠ "detection."
- Exact simultaneity (same ms) = a server/network-wide event; "within 2 minutes" is NOT
  simultaneous and is often coincidence at these rates.
- GetMaxHp firing is not infallible proof-of-disconnect (one case had 4+ s of normal play after).
- The flight-recorder dump CAN silently fail to fire (confirmed once) — a missed dump is a third
  outcome, not a "silent death."

## 3. Separate track: LOGIN / RECONNECT failures (do NOT mix with disconnects)

"Can't complete login" is a different problem from "kicked while playing" and must be analyzed
separately. Known cause: the autologin mistypes into the wrong window. The ~4h overnight
login-failure storm (01:21–05:30, both accounts, near-simultaneous recovery) was most likely a
server-side login-path issue; do not attribute it to the bot without evidence. This track is
parked — needs its own instrumentation (did autologin target the right window / type correct
creds / did the server reject) before theorizing.

## 4. Pending decisions (proposed, NOT yet implemented — confirm before building)

- **Fix A — config default.** `config.cpp:1371` and `config.h:30` default logLevel to 0 (Trace)
  on a read failure — i.e. fails to the loudest, I/O-throttling setting. Change both to 3 (Warn).
  Trivial, low-risk, and it protects clean data (a silent Trace-drop throttles a session). Do
  this FIRST. (Does not cover the in-game-overlay level-drift path — keep verifying each
  session's actual level and excluding any that drifted.)
- **Fix B — launcher single-instance guard.** The launcher can run two processes for one account
  (confirmed twice), which triggers server single-session kicks and torn `.ini` reads. Chosen
  approach: option 1 (real `WaitForSingleObject` on the prior process handle before any relaunch)
  PLUS a loud Warn log when a login proceeds for an account that already looks live. Escalate to a
  cross-process named mutex (`Global\CoClassicBot_<account>`, with WAIT_ABANDONED recovery) only
  if overlaps recur. Ship small, watch the log.

## 5. Setup / build gotchas for standing up on the VM

- Vendored deps (`vendor/imgui`, `vendor/spdlog`, `vendor/Detours`) must be cloned at the exact
  pinned commits — see README. They are not fully in-tree.
- MSVC needs `/utf-8` (already in CMakeLists) — pinned spdlog's fmt static_asserts require it.
- Game path is NOT hardcoded — resolved via `COCLASSIC_GAME_DIR` env / `game_dir.txt` /
  `GetModuleFileNameA`. Set for the VM's install path.
- Keep `coclassic.vcxproj` / `injector/launcher.vcxproj` source lists in sync with
  `CMakeLists.txt` by hand — the CMake build is the maintained path; the VS build silently omits
  files that drift. (New files this session: action_recorder, net_recv_hook, registries, heapfind.)
- LNK1104 on build = the DLL is still injected in a running game; close the game first.

## 6. Working posture / standing rules (inherited from project memory)

- **Confirm before fixing.** Propose a fix and wait for explicit go-ahead before implementing,
  even routine-looking ones. This session's confident diagnoses had a real error rate.
- **Read runtime settings, never assume them** (e.g. read `Misc.logLevel` from config before
  designing diagnostics). A wrong assumption here cost real analysis time.
- **Every retry loop needs a bounded escape hatch** (the launcher's 325-attempt no-backoff login
  hammer during the overnight storm is the cautionary example).
- **Measurement before behavior changes** — this whole investigation stayed read-only and
  instrument-first; keep that discipline.
- **Honest context:** this runs against the OFFICIAL conqueronline.net server. The disconnects may
  ultimately be server-side detection acting on the automation; that possibility is real and the
  wired test is what would confirm or rule it out. Ban risk is inherent to running here.

## 7. Investigation doc index (all in docs/investigation/)

- `TROJAN_DISCONNECT_REPORT.md` — first full disconnect analysis (server-close finding).
- `DISCONNECT_EVIDENCE_ROUND2.md` — rate theory undercut by its own data.
- `WALKABILITY_BRIEFING.md` — the real map-walkability mechanism (scene overlays in .DMap).
- `TWIN_CITY_BRIDGE_REBUTTAL.md` — proof the overlay fix generalizes; byte-level anchor formula.
- `THEMIDA_NOTES.md` — how to work with the packed client (offline static, don't fight it live).
- `HEAP_SCANNER_REPLACEMENT.md` — replace shape-scan with the game's own registries.
- `ROLES_CONTAINER_NOTE.md` — roles are a hash map, not a vector (why the registry path failed).
- `ROLE_B_IDENTITY.md` — the ~150 mystery VT_ROLE_B objects (RTTI stripped; identify via live read).
- `MILLIONAIRELEE_DIAGNOSIS.md` — the NPC dialog gate (IsNpcActive click-path-only; dialog token).
- `RANDOMWALK_INTERRUPT_DIAGNOSIS.md` — idle random-walk stomping the pathfinder mid-travel.
- `SPAWN_HEATMAP_IMPROVEMENTS.md` — count spawn events not dwell time; density-as-a-rate.
- `scripts/` — the read-only analysis scripts (DMap decoders, RTTI walk, diff tools, etc.).
