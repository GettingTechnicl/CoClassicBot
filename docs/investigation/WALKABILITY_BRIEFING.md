# Map walkability — where it actually lives (handoff briefing)

Read-only investigation, 2026-09-02. Nothing in the repo was modified. All facts below were
derived directly from the client's own files under `F:\Games\Classic Conquer 2.0\` and verified
against the user's three in-game readouts on map 1010.

## TL;DR

The bot's `.DMap` parser reads **only the base terrain cell grid** and stops. Bridges, stands,
planks — every "standable structure" — are **not in that grid**. They are **scene overlays**
listed in the part of the `.DMap` file the parser never reads, each pointing at a
`map\Scene\*.scene` file that carries its own small per-cell walkability grid. The game merges
those onto the base grid at load; the bot never does. That is the entire reason a real bridge
tile and empty void read identically (`mask=1`) to the bot.

The fix is to parse the layer list + `.scene` files and merge them as a second `LayerInfo`
layer. **Do not extend `walkability_overrides.*` — delete it once this lands.**

Proof on the user's own tile: map 1010's layer list contains
`map\Scene\stand06.scene at (88,85)` — the exact coordinate of the "walkable but map says
blocked" screenshot. `(43,71)` has no overlay → genuinely blocked. `(62,109)` is base-walkable.

## Where it lives in the current code

| What | File:line | Note |
|---|---|---|
| `.DMap` parser | `src/mapdata.cpp:95` `MapGrid::ParseFile` | reads header + base grid, returns at `0x114 + h*(w*6+4)`. Everything after is skipped. |
| Format comment | `src/mapdata.h:16-27` | already says `puzzlePath[260]` and "further sections follow the cell data" — those sections are the answer. |
| Synthetic map builder | `src/mapdata.cpp:166` `GetFileBackedGameMap` | hardcodes `dst.layer.next = nullptr; // single layer` at `:187`. This is where the overlay layer must be attached. |
| Walkability decision | `src/CGameMap.h:170` `IsWalkable` → `GetMask` → `GetLastLayer` (`:148-160`) | already walks the `LayerInfo::next` chain and trusts the **top** layer. The machinery for overlays exists; it is simply never populated. |
| Band-aid | `src/walkability_overrides.{h,cpp}` | hand-maintained polyline table, empty. Becomes dead code once overlays are parsed. |
| Blind diagnostic | `src/overlay.cpp:1860-1869` | the `layer[N]…(last)` tooltip hovers the file-backed map, which is always single-layer, so it can structurally never show the overlay. |

## The on-disk format (verified byte-for-byte)

### `.DMap` (e.g. `map\map\newbie.DMap` = map 1010; `map\map\newplain.DMap` = 1002 Twin City)

```
0x000  u32  version
0x004  u32  pad
0x008  char puzzlePath[260]      e.g. "map\puzzle\newbie.pul"   (visual background tiles — ignore)
0x10C  u32  width
0x110  u32  height
0x114  cell grid: height rows, each = width * {u16 mask, u16 terrain, s16 altitude} + u32 rowChecksum
       (row stride = width*6 + 4)                             <-- bot parses up to here and stops
----- TAIL (what the bot skips) -----
       u32  portalCount
       portalCount * { i32 x, i32 y, i32 portalId }           (Twin City: 6 portals, e.g. (401,387,#2))
       u32  layerCount                                        (1010: 38, Twin City: 1240)
       layerCount records, each starting with u32 type:
         type 1  SCENE      : { u32 type, char sceneFile[260], i32 x, i32 y }      = 272 bytes  <-- WALKABILITY OVERLAYS
         type 4  COVER      : 420 bytes total (u32 type + ani file + tga title + ints) — visual sprite, ignore
         type 10 3D-EFFECT  : { u32 type, char name[64], i32 x, i32 y }             = 76 bytes  — ignore
         type 15 3D-EFFECT2 : { u32 type, char file[260], i32[4] }                  = 280 bytes — ignore (size inferred from one sample; validate)
       trailing background-puzzle record (1010: ints (1,0,4,18,35,1,8) then "map\puzzle\newbiebg.pul") — visual, ignore
```

Note: unused bytes inside string buffers are `0xCD` (uninitialised editor memory), so read strings
as C-strings (stop at first NUL), never trust the whole buffer.

### `.scene` (e.g. `map\Scene\stand06.scene`, `bridgeA.scene`) — the overlay itself

```
0x000  u32 partCount
per part:
  +0x000  char aniFile[256]
  +0x100  char aniTitle[64]
  +0x140  i32 OffsetX, OffsetY        (sprite pixel offsets — ignore for walkability)
  +0x148  i32 AniInterval
  +0x14C  i32 Width, Height           (footprint in TILES)
  +0x154  i32 Thick
  +0x158  i32 partDX, i32 partDY      (this part's cell offset from the scene anchor; the 3rd int is garbage)
  +0x160  i32 (garbage / uninitialised)
  +0x164  cells: Width*Height * { i32 mask, i32 terrain, i32 altitude }   row-major, index = j*Width + i
  (next part follows immediately)
```

`mask 0 = walkable, 1 = blocked` — same meaning as the base grid. A bridge part is a walkable
deck with `mask=1` rails along its edges. `map\ScenePart\*.Part` are the same data as human-readable
INI text (`Cell[i,j]={mask,terrain,alt}`) and are handy for eyeballing, **but parse the `.scene`
binary** — at least one `.scene` carries an edited variant of a part that no longer matches its
`.Part` twin (bridgeA's `bridge05`). The `.scene` is what the client loads.

Cell ordering was verified uniquely as row-major `idx = j*W + i` ↔ `Cell[i,j]` on two non-square parts
(`wbridge01` 10×8, `bridge07` 9×9).

### Anchor convention — VERIFIED by geometry

A scene at `(x, y)` with a part `(dx, dy, W, H)` covers map tiles

```
tileX in [ x + dx - W + 1 ,  x + dx ]
tileY in [ y + dy - H + 1 ,  y + dy ]
part cell (i, j)  ->  ( x + dx - W + 1 + i ,  y + dy - H + 1 + j )
```

i.e. the anchor is the part's **bottom-right (max-x, max-y) cell**. Evidence:
- Twin City `bridgeA.scene at (648,682)` (7 parts, dx = -44,-37,-30,-23,-16,-9,0, widths 5,7,7,7,7,7,9)
  tiles *perfectly contiguously* under this rule (x 600..648) and lands exactly on the river gap
  between the two walkable banks (bank ends x=599, resumes x=649), deck rows y=676..682 bank-to-bank.
  Under a top-left anchor the parts overlap and gap. `bridgeB-L.scene at (196,548)` behaves identically
  (x 141..196, banks at 140 / 197).
- Map 1010's chain of 2×2 `stand06` platforms from (69,106) up to the 3×3 `stand08` at (91,79) touches
  walkable land at both ends only under this rule.
- The user-confirmed walkable tile (88,85) is inside the `stand06 at (88,85)` footprint (87..88, 84..85).

Only residual uncertainty: mirroring of a part's cell pattern within its footprint (which corner
a `mask=1` rail sits on). The map-level footprint is certain. Live-verify once on a part with an
asymmetric pattern, e.g. `stand01 at (84,91)` on map 1010 (4×4, blocked corners at Cell[3,0],
Cell[0,3], Cell[3,3]) — walk onto it and see which corner tiles refuse.

## What to implement (Route A — offline, safe, patch-durable)

1. In `MapGrid::ParseFile`, after the cell grid: read `portalCount`, skip portals, read `layerCount`,
   iterate records by type using the sizes above; collect `(sceneFile, x, y)` for every `type == 1`.
   Skip types 4/10/15 by size. Stop cleanly on an unknown type (log it) rather than asserting.
2. For each scene: load `<gameRoot>\<sceneFile>` (backslashes → OS separators), parse `partCount`
   parts as above.
3. Merge: for every part cell with `mask == 0`, mark the map tile computed by the anchor formula as
   walkable. Cleanest is to honour the existing design — give that `CellInfo` a second `LayerInfo`
   (`layer.next` → overlay layer with the part's mask/terrain/altitude) so `GetLastLayer`/`IsWalkable`/
   `CanJump`/`FindPath` all pick it up with **zero changes** to the decision code. (Overlay `mask==1`
   rail cells over base-blocked tiles change nothing; over base-walkable tiles they should probably
   block — that's what the game does with a rail. Either way, the overlay layer is authoritative.)
4. Keep `MapGrid::Cell` single-value if preferred by also OR-ing into a `walkableOverride` bit —
   but attaching the real second layer is what makes the `layer[N]` tooltip finally show the truth.
5. Delete `walkability_overrides.{h,cpp}` and the fallback branch in `IsWalkable`.
6. Unit test it off-game (no client needed): parse `newbie.DMap`, assert `IsWalkable(88,85)==true`,
   `IsWalkable(43,71)==false`, `IsWalkable(62,109)==true`; parse `newplain.DMap`, assert the bridge
   deck at e.g. `(624,678)` is walkable and the river tile `(624,672)` is not.

Scope sanity: overlays are small. Twin City (972×972, 1240 layer records) has exactly **2** scene
records; map 1010 has 20 (all small platforms). Parsing cost is negligible.

## Route B (not recommended first)

Read the game's already-merged in-memory grid (the real `CGameMap`, multi-layer). Requires finishing
the live scene-pointer chase in `src/map_probe.cpp:159` (`base+0x699370 → scene → …`) against a
stale/garbage offset. More fragile across patches than Route A and needs live memory work.

## Scratch scripts (read-only, in this session's scratchpad)

`dmap_tail.py`, `dmap_deep2.py`…`dmap_deep6.py` — the decoders used to derive everything above.
They only read game files and print; useful as reference for the parser and as fixtures for tests.
