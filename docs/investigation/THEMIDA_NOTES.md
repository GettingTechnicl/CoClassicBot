# Working with the Themida-packed client (ImConquer.exe) — notes for the other LLM

Terminology: the packed binary is **ImConquer.exe (the game client)**. `launcher.exe` (the
account manager/injector in this repo) is your own unpacked code — not packed.

## The one strategic rule that matters most

**Minimize live interaction with the packed process; maximize offline/file-based and
black-box work.** Every time you reach for a debugger, hardware breakpoint, or code patch on
the *running* client you are (a) fighting Themida's SecureEngine anti-debug, and (b) on a live
official server, risking detection/disconnect. The project already learned this the hard way:
the hardware-breakpoint tracer (`src/tracer.cpp`) tripped a disconnect. The walkability work is
the model to copy instead — the data was in the client's own files in a documented format, so
the packed binary never had to be touched at all.

Before reverse-engineering anything out of the packed client, ask in this order:
1. **Is it in the game's own data files?** (`ini\*.json`, `*.DMap`, `map\Scene\*`, `itemtype.json`,
   `GameMap.json`…). Conquer ships enormous amounts on disk. Walkability, items, maps, portals
   were all here — zero client RE needed.
2. **Is it already documented by the Conquer emulator community?** This is a *known* game. Open
   CO private-server sources (Comet, Chimera, TransCONQUER, and many others) document the packet
   protocol, DMap/scene/puzzle formats, item/magic/monster tables. Much of what looks like
   "reverse the packed client" is really "read someone's existing spec." Prefer that.
3. **Can you get it black-box from the already-decrypted dump (static) or from observing
   inputs/outputs (differential memory / packet capture)?** Only then,
4. **…does live instrumentation of the packed process become necessary** — and if it does,
   scope it to the absolute minimum and treat detection as a real cost.

## What Themida actually does here, and what that means

Themida (Oreans) does three things relevant to you:

- **Packs/encrypts sections on disk, decrypts at runtime.** This is why static analysis of the
  *on-disk* exe is useless, and why the project analyzes a **Scylla dump of the decrypted image
  from a running process** in Ghidra (the ~7 MB `image_dump.bin`, base..~0x71C000). That dump is
  the correct artifact. Normal (non-virtualized) compiled functions are fully recoverable in it —
  which is exactly why the RVAs the project already found (map-item send chain, `CRole::SetCommand`,
  `CNetClient::SendMsg`) are real x86 you can read and reason about.
- **Anti-debug (SecureEngine):** `IsDebuggerPresent`/`NtQueryInformationProcess`/debug-register
  checks/timing (rdtsc). This is what makes *live* debugging of the client hostile. For **offline
  static analysis of the dump you don't attach a debugger at all, so none of this applies** — that's
  the safe lane. ScyllaHide/TitanHide/HyperHide exist to suppress these checks, but using them to
  keep a debugger on the *live official client* is both fragile and squarely in evade-the-protection
  territory — not needed for understanding, and not the track to push.
- **Selective virtualization (Code Virtualizer):** Themida can move *chosen* functions into a
  custom bytecode VM. A virtualized function is NOT real x86 in the dump — it's a dispatch loop
  interpreting per-build-randomized opcodes. **Do not try to statically read a VM'd function; it's
  a time sink.** Tell-tale: a "function" that's a tight fetch-decode-execute loop over a handler
  table, or a call that vanishes into the Themida VM section instead of normal code.

### The practical decision rule for any target function
- **Not virtualized** (the common case, incl. everything found so far): read it in Ghidra from the
  dump like normal code. Fine.
- **Virtualized:** stop reading its internals. Go **black-box** — characterize it by its
  observable behavior: what memory it changes (the differential snapshot technique already in use),
  or what packet it emits (hook your *own* Winsock in the injected DLL, as `src/netfinder.cpp`
  already does — that watches WSASend from outside the game's code, which is far less hostile than
  breakpointing inside it). You almost never need the VM'd bytecode; you need its contract.

## Dump hygiene (if they ever re-dump)

- **IAT reconstruction:** Themida redirects imports; a raw Scylla dump needs its import table
  rebuilt (Scylla's IAT autosearch, or manual). If the existing `image_dump.bin` already resolves
  calls to named APIs, this was handled — don't redo it.
- **Record provenance:** binary hash + client build (v1074) + dump base with every offset. Offsets
  are build-specific; the project's `[LIVE-VERIFIED]`/`[STALE]` tagging convention in `game.h` is
  exactly right — keep it.
- The dump is a *snapshot*: sections Themida decrypts lazily (on first call) may not have been
  decrypted at dump time. If a region looks like garbage/zeros where code should be, it may simply
  not have executed yet — re-dump after exercising that feature, don't assume it's VM'd.

## What NOT to spend effort on (from the RE skill, deliberately de-scoped)

The reverse-engineering skill catalogs heavy dynamic techniques — exception-driven trap-and-emulate
DBI, `KiUserExceptionDispatcher` hooking, WHP hypervisor-assisted tracing, ScyllaHide-style
anti-anti-debug. These are powerful for *offline malware/protection research on a binary you fully
control*, but against a **live, official, anti-cheat-guarded, Themida-protected client** they are
the highest-detection-risk path and the reason the earlier HWBP tracer caused a disconnect. They are
not necessary for the understanding-level work this project is doing, and I'd steer away from them
here. If a VM'd function ever genuinely blocks progress, the answer is black-box observation
(differential memory + own-socket capture), not out-instrumenting Themida on the official server.

## Concrete resources (from the game-security skill's curated index)

The `reverse-engineering-tools` / `awesome-game-security` skill indexes tools and per-repo archives:
- README index: `https://raw.githubusercontent.com/gmh5225/awesome-game-security/refs/heads/main/README.md`
  — sections `Cheat > Fix Themida`, `Anti Cheat > Dump Fix`, `Anti Cheat > Sample Unpacker`,
  `Cheat > Decompiler`, `Cheat > Ghidra Plugins`.
- Per-repo full-code archive: `.../archive/{owner}/{repo}.txt`; short description:
  `.../description/{owner}/{repo}/description_en.txt`.
- Tools worth knowing by name: **Scylla** (dump + IAT rebuild — already the project's approach),
  **Ghidra** (+ MCP if you want an agent driving it), **x64dbg** (offline analysis of the dump,
  not the live game), **ImHex/PE-bear** (inspect the dump/PE), **BinDiff/Diaphora/ghidriff**
  (diff v1074 vs a future client patch to re-locate shifted offsets fast — this is the durable
  way to survive the next update instead of re-hunting by hand).

The last one is the highest-leverage: when the client next patches and offsets drift again, a
**binary diff of the two decrypted dumps** re-locates moved functions far faster than re-deriving
them, and it's fully offline.
