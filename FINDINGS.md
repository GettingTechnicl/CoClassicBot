# CoClassicBot — Findings & Notes

A plain-language write-up of everything we've discovered reverse-engineering this bot
and the Classic Conquer client (v1074). Read top to bottom; the "Still to read" section
at the end lists what's left.

---

## TL;DR

- The bot's **auto-hunt system was written by the original author** (Burak / 0x42524b), not
  by a Claude agent. The fork (justin654) added *surrounding* features — SOCKS5 proxy, HWID
  spoofing, Discord stats, a UI refactor. All of it is real, competent code, not filler.
- The bot **did not work on your installed client** — the game updated (to v1074) and the
  memory offsets the bot relies on had shifted. We **found and fixed the data-side offsets**
  and **proved the fixes work live, inside the running game**.
- The client is **Themida-protected** and uses a **Protocol-Buffers** network protocol —
  both of which shaped every technical decision.
- We built read-only tools that now read the **entire live game state**: your character,
  every nearby monster/player/NPC, your gear, coordinates.

---

## 1. What the fork actually changed (the audit)

Two GitHub repos were involved:
- Original: `0x42524b/CoClassicBot` (author "Burak")
- Your fork: `GettingTechnicl/CoClassicBot` (the "Claude-modified" one, author "justin654")

By adding the original as a git remote and diffing commit-by-commit, we established:

- The commit **"Add hunt plugin system…"** (`254ace7`) is **identical in both repos** (same git
  hash), which proves it came from the *original* author. So the ~7,000-line auto-hunt engine
  (archer/melee variants, targeting, buffs, loot, town runs) is Burak's work.
- The fork added **7 commits (~2,700 lines)** on top:
  - **SOCKS5 proxy** in the injector (route login through a proxy, with a pre-launch test,
    kill-switch, packet logging)
  - **HWID spoofing** (`hwid_spoof.cpp`) — real Detours-based API hooks on volume serial,
    MAC, MachineGuid, etc. Partial coverage (no disk-serial/WMI/SMBIOS).
  - **Hunt stats + Discord** (`hunt_stats.cpp`) — real WinHTTP webhook posts, live stat
    tracking. (The webhook plumbing `discord.cpp` is actually the original author's.)
  - **Loot tweaks** — `lootMoney`, `manualControlPauseMs`, loot filters, auto-drop
  - **UI refactor** into tabbed sections

**Verdict:** the fork's additions are real and mostly sound — not an agent hallucinating.
The problems were bugs and a stale-offset break, not fake code.

---

## 2. Getting it to build

The project wouldn't compile out of the box:
- The three vendored dependencies (`imgui`, `spdlog`, `Detours`) were empty — we cloned them
  at the exact commits the fork pinned. (Those pins turned out to be valid upstream commits,
  another sign the fork was legit.)
- One real build gap: the pinned `spdlog`'s bundled `fmt` **requires the MSVC `/utf-8` flag**,
  which the CMake build never set. Added it → clean build. All targets compile, tests 29/29.

Two **hardcoded install-path bugs** would have broken it on your machine regardless:
- The injector and `itemtype.cpp` both hardcoded `C:\Program Files\Classic Conquer 2.0`, but
  your install is on `F:\`. Fixed: the injector now reads the path from an env var / config
  file, and `itemtype.cpp` derives it from the running game. (The `itemtype.cpp` one was the
  *original author's* code.)

---

## 3. The big discovery: the bot was reading the wrong memory

The bot reads the game's memory at fixed **offsets** (e.g., "the hero's silver is 0xA30 bytes
into the hero object"). Those were reverse-engineered against an *older* client build. Your
client is newer (v1074), and the offsets had drifted.

We verified this live by reading the running game's memory and comparing to your actual
character ("Kinux": silver 7681, level 78, maxHP 2419, etc.). What we found:

**Broken:**
- The **anchor** the bot uses to *find* your character (`ROLE_MGR = 0x4DF588`) now points at
  code, not data. This single break cascades — the bot couldn't find the hero, so *everything*
  downstream read garbage. In v1074 the role manager moved to the heap and is reached through
  a **pointer at `0x69C730`** instead. Fixed.
- **Silver** was at the wrong offset (`0xA30`), which is why the original author had bolted on
  a pile of guessy heuristics to `GetSilver()`. The real location is **`0xA80`**. Fixed, and
  the heuristics removed.

**The pattern:** a single ~0x50-byte field was inserted into the hero object between two
existing fields, so **everything after it shifted by 0x50**. That made the fix mechanical:

| Field       | Old offset | v1074 offset |
|-------------|-----------|--------------|
| silver      | 0xA30     | **0xA80**    |
| inventory   | 0xB20     | **0xB70**    |
| equipment   | 0xB88     | **0xBD8**    |
| max mana    | 0xCA8     | **0xCF8**    |
| skills list | 0x1918    | **0x1968**   |

Fields *before* the insertion (name `+0x94`, id `+0x68`, maxHP `+0x3D0`, stat-table `+0x968`,
level `+0x6E8`) were unchanged.

We verified equipment by decoding the type IDs of your equipped items straight from memory —
they came out as **GoldCoronet, GoldNecklace, DemonArmor, Lathee**, matching your actual gear.

---

## 4. Proving the fixes work — live, inside the game

We wrote a small **read-only DLL** (`selftest.dll`) that gets injected into the running game,
reads your character through the *fixed* offsets, and writes a report. Result:

```
name: Kinux   id: 1157655   maxHP: 2419   level: 78
silver: 7681   equip: [GoldCoronet, GoldNecklace, DemonArmor, Lathee]   skills: 18
```

Every value correct. This confirmed the fixes work through the real code paths, in-process —
not just in an external reader. The bot's "eyes" are revived on v1074.

---

## 5. Mapping the whole live world

We then built `explorer.dll` — still read-only — that scans the game's heap for objects
matching the **"CRole" signature** (a valid vtable + a valid entity ID + a sensible map
position). In Twin City this pulled out the entire area:

- **44 monsters** (a field of Pheasants, maxHP 33; two Guards, maxHP 50000)
- The **Conductress** NPC
- Your character

…each with its real name, ID, live position, and HP. This is exactly the data a bot needs to
*see and target* the world — and it sidesteps having to decode the game's internal container
format, because we just find the objects directly.

---

## 6. Two facts that shape everything

**The client is Themida-protected.** Themida is a commercial "packer" that encrypts the
program's code on disk (it's decrypted only at runtime) and actively resists debugging and
tampering. Consequences:
- We can't analyze the code from the disk file — we have to read the *decrypted* copy from a
  running process's memory (which we did: a ~7MB dump we analyze offline).
- Anything that looks like a debugger (hardware breakpoints, hooks) risks being detected. When
  we tried live function-tracing with hardware breakpoints, your character disconnected — most
  likely our tool being too aggressive, possibly the anti-cheat noticing.

**The network protocol is Protocol Buffers (protobuf-lite).** The game serializes its packets
with Google's protobuf. The important implication: we **don't** need to reverse the wire
protocol to make the bot act — we can call the client's *own* functions (like `CHero::Walk`)
and let the client build and send the packets itself.

---

## 7. The tools we built (all in the repo)

Source in `src/`, built with CMake into `build/bin/Release/`:

| Tool | What it does | Risk |
|------|--------------|------|
| `selftest.dll` | reads your hero through the fixed offsets, writes a report | read-only, safe |
| `explorer.dll` | reads the full game state (entities, gear, stats, coords) | read-only, safe |
| `tracer.dll` | hardware-breakpoint "find what code touches this data" | active debugging — riskier |
| offline scripts | disassemble & analyze the decrypted memory dump | offline, no risk |

Build any of them with:
```
cmake --build build --config Release --target <name>
```
Inject the DLLs with **System Informer** (right-click `ImConquer.exe` → Inject DLL).

---

## 8. Still to read (as of end of session 1)

> **Note:** most of this list was completed in session 2 — see section 9 below. (In
> particular, the entity filter, maps, EXP, current-map pointer, and `CItem` durability are
> done; the `u32[17]` durability clue in item 4 turned out to be wrong — durability is at
> `+0x60`/`+0x64`.)

All of this is passive reading/analysis — the track we're staying on:

1. **Tighten the entity filter** — drop a few false positives (non-entity objects that slip
   past the signature check).
2. **Find the map-ID location** — it's stored in the `CGameMap` object, which we haven't
   pinned yet (searching for "1002" in the hero/role-manager came up empty).
3. **Decode the stat table** — it's a hash-map keyed by stat ID; level/exp/mana live in there.
4. **Pin the `CItem` layout** — durability/+N/gems/quality. We have a clue: a weapon's
   durability field visibly changed (`u32[17]`: 65024 → 0) as it took wear.
5. **The action functions** — we found live candidates for the movement processor (`0x1A8140`)
   and the attack/command path (`0x1B04B0`) before the disconnect. These are the "hands" of
   the bot.

---

## 9. Session 2 — completing the sensory layer

We picked the read-only track and built out the full "eyes" of the bot. Everything here is
passive memory reading, verified live against the real character ("Kinux").

### The reusable toolkit we built
The big shift this session was tooling. Instead of rebuilding the DLL to hunt each value, we
built two reusable capabilities into `explorer.dll`:
- **`value_search`** — searches all memory for a given number and reports every address holding
  it (driven by a small config file, no rebuild).
- **Raw region dumps** — every refresh, it dumps the hero object, role-manager, stat table, and
  the globals block to `.bin` files.

With those, finding *any* value became a simple recipe: **snapshot two states, then diff.**
That single technique cracked silver, the map pointer, EXP, and durability. This is exactly how
the classic tools worked — "find what changed when X happened."

### Entities — full ESP
A heap scan for objects matching the `CRole` signature (a valid vtable + valid entity ID + a
sane map position) reads **every nearby monster, player, and NPC** — name, ID, live position,
HP. Proven in a Pheasant field (44 monsters + guards), a packed market (99 players + 64 NPCs),
and other maps. This sidesteps decoding the game's internal container entirely.

### Maps & items — identified via the game's own data
- **`data_items.json`** — all **11,142 items**, ID → name (from `itemtype.json`).
- **`data_maps.json`** — all **156 maps**, ID → file → friendly name (1000 Desert City,
  1002 Twin City, 1020 Bird Island, …).
So any ID we read from memory resolves to a real name — just like Farmer/Mimic knew you were
in Twin City fighting Pheasants.

### EXP — found by matching the percentage
Killing Pheasants gave ~0 XP (they're far below level 78), so we went to stronger monsters and
watched the exp % move `77.723% → 78.528%`. The one memory value that increased by that *exact
ratio* was EXP. Confirmed by the level-up threshold computing identically from both readings:
- **Current EXP** = `hero + 0x708`
- **Next-level requirement** = `hero + 0x1254`
- **exp %** = `0x708 / 0x1254` — reproduces the displayed % exactly.

### Current-map pointer
The map turned out to be tricky: the client keeps a *cache* of all loaded maps, and there's no
plain "current map ID" integer. By diffing the globals across Twin City vs Bird Island, we found
a **static pointer at `base + 0x699370`** that changes to point at a different scene object per
map — the active-map pointer. Reading the doc ID from it needs one more pointer-hop (scene →
active map object), which we deferred; the pointer itself is nailed and relog-proof.

### `CItem` layout — durability decoded
Durability is stored fixed-point, ×100 (matching the item type's `amountLimit`):

| Field | Offset | How to read |
|-------|--------|-------------|
| UID | `+0x08` | instance ID |
| type ID | `+0x10` | → `itemtype.json` for all base stats |
| price | `+0x48` | copied from item type |
| defense / stat | `+0x54` | armor = defense; weapon = a bonus |
| **current durability** | `+0x60` | `(value >> 16) / 100` |
| **max durability** | `+0x64` | `value / 100` |

Verified: GoldCoronet `40/41`, Lathee `15/27` — both exact. Durability is the field an
**auto-repair** loop watches. (Item `+N`/gems/quality weren't pinned — none of the test gear was
enhanced or socketed; easy to grab later on an upgraded item.)

### Where the sensory layer stands
Character (stats/exp/level/silver/HP), all entities, equipment + durability, the item and map
databases, and the current-map pointer are all working live. That's a complete Farmer/Mimic-grade
read-only foundation on a client years newer than the original bot. Remaining read-only bits are
minor and deferrable: the map doc-ID final hop, item `+N`/gems, and the (largely redundant)
stat-table decode.

---

