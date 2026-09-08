# What are the ~150 VT_ROLE_B (0x5CDB10) objects in the roles vector?

Read-only analysis of `image_dump.bin` (decrypted) + `src/registries.h`, 2026-09-03. Nothing modified.
Answering the open note from the heap-scanner replacement work.

## Bottom line first
The class NAME is not recoverable from the binary (RTTI is stripped on the game's own entity
classes — see below), so the definitive ID is a **one-line live read**: for each roles-vector
entry whose vtable == `VT_ROLE_B`, log `id (+0x68)`, `name (+0x94)`, `pos (+0xD8)`. The name
string + id range names them immediately. Everything below is what the dump already tells us,
which strongly constrains what they can be.

## What the dump establishes (offline, no live game)

1. **RTTI can't name it.** The image has 212 type descriptors / 170 RTTI vtables, but they are ALL
   third-party (crashpad, protobuf, std::, the CBoxFilter/CResizeEngine image code). `VT_ROLE_B`,
   `VT_MONSTER`, `VT_HERO`, `VT_OBJ_A/C`, and even the `CGameMap` vtable all resolve to
   `<no RTTI>`. The game's own entity hierarchy was compiled without RTTI records, so no vtable in
   the CRole family yields a name. Don't spend more time trying to get a name from the dump.

2. **It's a genuine CRole-family class, distinct from monster/hero.** Comparing the three vtables
   slot-by-slot: slots 7–18 (and most of the tail) are byte-identical across ROLE_B / MONSTER /
   HERO — i.e. they share the same `CMapObj`/`CRole` base — but ROLE_B overrides its **first 7
   virtuals** (render/update/etc.) with its own methods. So it's a sibling of monster/hero under
   the common role base, with its own behavior.

3. **It's small and non-combatant.** ROLE_B's own destructor frees **0xF0 (240 bytes)**
   (`0x15DAEE: mov edx,0xF0 ; call operator delete`). A monster is multiples of that. 240 bytes
   means the object only spans the CRole header — `m_id (+0x68)`, `m_szName (+0x94)`,
   `m_posMap/World/Scr (+0xD8/E0/E8)` — and is **truncated well before** the combat/stat fields
   (`m_cmdAction +0x188`, `m_nMaxHp +0x3D0`, the stat table, skills, etc.). So: a positioned,
   possibly-named map role with **no HP, no command, no stats** — a passive/scenery object, not a
   fightable entity.

4. **Single factory.** The ctor `0x15DB10` has exactly one caller — a make_shared factory at
   ~`0x145200` (`lea rsi,[rdi+0x10]; call 0x15DB10`) that wraps it in a `shared_ptr<CRole>` and
   drops it in the roles vector. Climbing `0x145200`'s callers (up to a packet handler / named
   subsystem) is the offline route to the purpose if the live name-read isn't enough — but the
   live read is faster.

## What they most likely are
A 240-byte, positioned, named-but-statless "role" that the game lists among roles but that has no
combat state = **scenery / passive map objects**: booths (player vendor stalls), ground/terrain
effects, traps, transport/portal markers, or decorative NPCs. ~150 on a hunt map fits booths or
ground effects well. This is exactly the kind of thing the monster shape check *should* keep
rejecting — so the note's "nothing regresses by ignoring them" is right.

## The definitive check (live, read-only, ~5 lines)
In `Registries::ReadRoles` (or a throwaway diagnostic pass over the roles vector), when
`*(objptr) == BASE + 0x5CDB10`:
```
spdlog::info("[role_b] id={} name='{}' pos=({},{})",
    *(uint32_t*)(obj+0x68), (const char*)(obj+0x94),
    *(int32_t*)(obj+0xD8), *(int32_t*)(obj+0xDC));   // SEH-guard the reads
```
Read the first ~10 that come back:
- **id range** tells the family: NPC/scenery ids are low (roughly 1–100000); a booth carries its
  owner's **player id (≥ 1000000)**; effects/traps have their own bands.
- **name** is usually dispositive ("… Booth", a player name, an effect tag, or empty).

If they turn out to be booths/effects/scenery (most likely), the bot is right to ignore them and
there's nothing to do. If any turn out to be something useful (event NPCs, gatherables), that's
when to add handling — but decide that from the names, not from a guess.
