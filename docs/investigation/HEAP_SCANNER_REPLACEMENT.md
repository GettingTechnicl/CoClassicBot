# Replacing the heap scanner — read the game's own registries instead

Read-only analysis of `src/entities.cpp`, `src/map_items.cpp`, `src/hooks.cpp`, the role-manager
dump (`C:\Users\Public\co_rolemgr.bin`) and `C:\Users\Public\coclassic_mapprobe.json`, 2026-09-03.
Nothing modified.

## 1. Why the scanner is inherently janky (not a tuning problem)

Both scanners (`Entities`, `MapItems`) decide "this object exists" by **shape**: a vtable into
the image (entities), a plausible id, an item type that resolves, a position inside the map
(`LooksLikeMapItem`, map_items.cpp:38). `MapItems::IsAlive()` (map_items.cpp:229) is that
**same shape check re-run on the cached pointer** — nothing more.

That is the ghost mechanism in one sentence: **when the game frees an item, its bytes do not
vanish.** A picked-up item's block still *looks* like a valid item until the heap reuses it, so
`IsAlive` returns true and the bot walks to grab nothing. If the block is reused by a *different*
item, it still looks valid — wrong position / wrong plus (this is exactly why `CMapItem::GetPlus`
grew the +0x44 type-id self-validation). Every loot workaround — `stale age`, `spawn_grace`, the
ghost timer, `IsAlive`, the `PruneLootPickupAttempts` ghost logic — compensates for **inferring
liveness from memory contents, which fundamentally cannot detect a free.**

Cost on top: ~94 ms every ~200–500 ms walking ~19M addresses, and the cost scales with process
heap size. It is scaffolding. Replace it, don't polish it.

Only the game knows an object is gone. Read the game's registry: an entry disappears the instant
the game removes it. Ghosts become impossible by construction and the scan goes away.

## 2. Two prior "authoritative source" leads are FALSE — don't re-chase them

- **`entities.h` header: "CRoleMgr+0x40 onward is an array of 0x20-byte records
  {rolePtr, 0, flags, hash} — a hash-map bucket array."** Not supported by the dump. In
  `co_rolemgr.bin` those slots are a mix: several "rolePtr" values are UTF-16/ASCII text
  (`0x7500670065005200`, `0x720075006F004300`, `0x2072656972756F43` = "Courier " — a font name),
  and the pointer-looking ones (`0x56F76160`, `…6340`, `…6778`, `…6B90`) point *inside the hero
  object itself* (hero at `0x56F76020`). The real role registry was never located.
- **`coclassic_mapprobe.json`: "27 CGameMap instances."** Nearly all false positives from the
  vtable heap scan: `idMap` = 0 / 15 / 3210095554, `size` = 0, and the +0x178 "vector" on two of
  them is literally the string `"windows\...winsxs\a"`. None is a real map with a live item list.
  So "read `m_vecItems` off the real in-memory CGameMap" is NOT yet available — the real map object
  hasn't been found either.

## 3. Route 1 (recommended): inverse-pointer search — use the scanner to find its own replacement

The scanner already yields the exact set of **live** `CRole*` (and `CMapItem*`) right now. The
game's container is, by definition, a structure holding pointers to all of them. So: search memory
for the region that contains those pointers.

Procedure (all read-only, existing toolkit — `explorer.dll` already reads/dumps qwords):
1. Take a scan snapshot: the N live `CRole*` (say 40 monsters). Record the set P.
2. Walk all committed private RW regions (VirtualQuery, same loop the scanner uses). For each
   region, count how many 8-byte-aligned qwords equal a member of P. Report regions ranked by
   hit count. (An `unordered_set<uintptr_t>` of P makes this one pass.)
3. The region holding ~all of P is the role container. Note: it may store the pointer directly
   (`std::vector<CRole*>`), inside a record (`{CRole*, ...}` stride — measure the gap between
   consecutive hits), or as a `shared_ptr` (pointer at +0 of a 16-byte `{ptr, ctrlblock}` pair —
   note `Ref<T> = std::shared_ptr<T>` is the project's own convention, so expect this). The hit
   stride tells you which.
4. Confirm it is authoritative, not a coincidental cache: kill or walk away from one monster and
   re-run — that region must lose exactly that one pointer. Do the same for items: pick one up,
   the item container must lose exactly one `CMapItem*`. If a candidate keeps a pointer after the
   game removed the object, it is a cache, not the registry — discard it.
5. Find how the region is reached from a known anchor so it survives a relog: it will hang off
   `CRoleMgr` (`Offsets::ROLE_MGR_PTR = 0x69C730`, LIVE-VERIFIED) or the current-map/scene
   pointer (`base+0x699370`). Read `Game::GetRoleMgr()`'s object as qwords and look for a
   pointer into (or to the start of) the found region — that offset is the field. Same for items
   off the scene pointer.
6. Decode the container's begin/end (or bucket array + count) so iteration is exact. Then
   `Entities::Get()` / `MapItems::Get()` become "iterate the game's container" — O(n), fresh
   every call, zero ghosts. Delete the scan thread. Delete `IsAlive`'s shape check (replace with
   "is it in the container right now").

Expect items to be the bigger win (that's where the ghosts hurt), so do items first.

## 4. Route 2: the render hook is already a live entity source (code exists, RVA stale)

`HkRenderEntityVisual` (hooks.cpp:82) is a pure passthrough that receives **every `CRole*` the
game draws, every frame**. "Rendered this frame" == "exists" — per-frame fresh, no ghosts, no scan.
It is disabled only because `GameRva::VERIFIED_V1074 == false` (game.h:69) — the
`CENTITY_RENDER_VISUAL` RVA is stale on v1074. Re-finding that one RVA turns it on. To use it as
the entity set: in the hook, insert the `CRole*` into a per-frame set; at frame end swap it into
the published set (double-buffer like `g_front/g_back` today). Items have their own render path —
the same trick applies once that function is found.

Finding the RVA is standard offline work on the decrypted `image_dump.bin`: it is the per-entity
render virtual; cross-reference from the CRole vtable (the scanner already anchors on "vtable in
image", so the vtable address is known) — the render slot is one of its entries. Don't live-debug
the packed client for this; the dump has it.

## 5. Route 3: reverse the real CRoleMgr container by hand

Correct but the most work, and Route 1 effectively locates it for you. Only fall back to this if
Route 1's ranked regions are ambiguous.

## 6. What NOT to do

- Don't shorten the scan interval or add more ghost heuristics. Every one so far has been a
  band-aid on the unfixable "liveness from contents" premise.
- Don't trust the `entities.h` +0x40 bucket theory or the 27 mapprobe "instances" — both are
  disproven above.
- Don't live-debug the packed client to find these; Route 1 is pure memory reading and Route 2's
  RVA comes from the offline dump.

## 7. Acceptance criteria

- Zero `[hunt-loot] Skip … reason=stale` and zero ghost-pickup attempts over a long hunt.
- No scan thread; entity/item enumeration cost negligible and independent of heap size.
- Picking up an item removes it from `MapItems::Get()` on the very next call, every time.
- A relog still enumerates correctly (i.e. the container was reached from a stable anchor, not a
  hardcoded heap address).
