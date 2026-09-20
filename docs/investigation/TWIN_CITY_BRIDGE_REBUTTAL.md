# Twin City bridges ARE in the file — the overlay fix generalizes. Don't restore the override.

Read-only verification against `map\map\newplain.DMap` + `map\Scene\bridgeB-L.scene`, 2026-09-02.

## The user's tile (167,543) is on the bridge deck

```
bridgeB-L.scene at (196,548): 8 parts
 part# title        pixOffX pixOffY  cellDX cellDY  W x H   footprint x[..]   y[..]
  0   bridge06.tga    -166    -242     -30      0  7x9   x[160..166] y[540..548]
  1   bridge06.tga    -170    -246     -16      0  7x9   x[174..180] y[540..548]
  2   bridge06.tga    -172    -248      -9      0  7x9   x[181..187] y[540..548]
  3   bridge06.tga    -168    -244     -23      0  7x9   x[167..173] y[540..548]   <-- covers (167,543)
  4   bridge06.tga    -164    -240     -37      0  7x9   x[153..159] y[540..548]
  5   bridge07.tga    -237    -280       0      0  9x9   x[188..196] y[540..548]
  6   bridge06.tga    -162    -238     -44      0  7x9   x[146..152] y[540..548]
  7   bridge05.tga    -157    -192     -51      0  5x9   x[141..145] y[540..548]

UNION footprint x[141..196] y[540..548] — contiguous, zero gaps, zero overlaps.
User tile (167,543): base mask=1 -> COVERED by part 3 at part cell (0,3), overlay mask=0 => WALKABLE (deck)
```

Mask-annotated grid (`.` base walkable, `#` base blocked, `o` overlay deck, `x` overlay rail, `U` user, `A` anchor):

```
         7890123456789012345678901234567890123456789012345678901234567890   (x = 137..200)
  y=539  ........########################################################
  y=540  ....xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx##..
  y=541  ....xxxooxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxooxxxxxxx....
  y=542  ....oooooooooooooooooooooooooooooooooooooooooooooooooooooooo....
  y=543  ....ooooooooooooooooooooooooooUooooooooooooooooooooooooooooo....
  y=544  ....oooooooooooooooooooooooooooooooooooooooooooooooooooooooo....
  y=547  ....oooxxooooooooooooooooooooooooooooooooooooooooooooooooooo....
  y=548  #...xxxxxooooooooooooooooooooooooooooooooooooooooooxxxxxoooA....
  y=549  ##..######################################################......
```

The deck spans bank (x=140, walkable) to bank (x=197, walkable) across the river. That is the bridge.

## Where the analysis went wrong — one field

The eight parts were classified as "animation frames (offsets oscillating -166,-242 / -170,-246 …,
same 7×9, same texture)". That looked only at the **pixel** `OffsetX/OffsetY` at part+0x140 and
ignored the **cell** offsets `partDX/partDY` at part+0x158 (documented in `WALKABILITY_BRIEFING.md`
as "partDX, partDY — this part's cell offset from the scene anchor"). The cell offsets are
`-30,-16,-9,-23,-37,0,-44,-51` — eight *distinct* horizontal positions. Combined with the widths
`7,7,7,7,7,9,7,5` they tile x[141..196] perfectly with no gap and no overlap. Animation frames
cannot do that: frames share one position; segments have distinct positions that add up to the
structure's length. The differing end-piece textures (`bridge05` left cap, `bridge07` right cap,
`bridge06` repeated middle) confirm it's a tiled structure, not a frame sequence. The slightly
different pixel offsets per part are per-segment sprite registration, exactly as expected.

"Nearest scene is 29 tiles away" measured distance to the **anchor point** (196,548). The anchor is
the bottom-right cell of a 56-tile-wide footprint; (167,543) is 29 tiles *inside* it.

## What to do (this is the same overlay code path, no second mechanism)

For each `type 1` scene record `(sceneFile, x, y)` and each part `(partDX, partDY, W, H, cells)`:

```
tileX = x + partDX - W + 1 + i        i in [0, W)
tileY = y + partDY - H + 1 + j        j in [0, H)
mask  = cells[j*W + i].mask           0 = walkable
```

Map 1010's `stand*` platforms are just the degenerate case of this (one part, partDX=partDY=0).
There is no "different mechanism" for Twin City. Same parser, same formula.

Test assertions (off-game, no client needed):
- map 1002: `(167,543)` walkable; `(150,543)` walkable; `(167,540)` NOT (rail); `(167,536)` NOT (river);
  bridgeA: `(624,678)` walkable, `(624,672)` NOT.
- map 1010: `(88,85)` walkable; `(43,71)` NOT; `(62,109)` walkable.

## Why not "both mechanisms coexist"

The override is a hand-walked polyline that has to be re-recorded per bridge, per map, forever, and
silently rots when it's wrong. The overlay is the game's own authoritative data, covers every bridge
on every map (including ones nobody has walked yet) with zero maintenance, and makes the `layer[N]`
debug tooltip finally show the truth. Once the overlay is correct there is nothing left for the
override to do. Keeping it "just in case" is keeping a bug-shaped hole open.

Scripts used: `dmap_check_167_543.py` (this check), `dmap_deep5.py`/`dmap_deep6.py` (anchor +
cell-ordering proofs) in the session scratchpad — all read-only.

---

## Addendum 2026-09-18 — rails now block; this completes the plan above

The formula and test assertions above always specified that the overlay is authoritative
(`(167,540)` "NOT (rail)" is a rail cell over river, `mask=1`). The first implementation
(`mapdata.cpp`, 2026-09-02) applied only the overlay's *walkable* cells and deliberately skipped
its rail cells, pending a live check of part-cell **orientation** (can a part be mirrored within
its footprint?). That deferral is now closed, and rail cells block. **Do not restore the
walkable-only merge, and do not re-litigate the orientation caution — the evidence is below.**

### What forced it: the server refuses rail tiles that sit on walkable bank land

Twin City has 48 tiles the base grid calls walkable but an overlay rail cell covers
(`bridgeA`: 31, `bridgeB-L`: 17). The walkable-only merge left them walkable, so A* happily
routed onto them and the server refused the jump:

| when | jump | tile refused | what it is |
|---|---|---|---|
| 2026-09-04 | `(588,695) -> (601,682)` | `(601,682)` | bridgeA SW end-cap rail (part `bridge05` row 8) |
| 2026-09-18 | `(588,665) -> (604,674)` | `(604,674)` | bridgeA NW end-cap rail (part `bridge05` row 0) |

Both repeated the identical failing jump for seconds to minutes (`Predicted move stale` /
`STUCK timeout`), because every repath read the same wrong grid. `MarkTileBlockedThisSession`
(pathfinder stuck-waypoint blacklist) treated the symptom; this fixes the cause. Nothing in any
log shows the hero standing on any of the 48 tiles.

### Why blocking is safe without an in-game test

1. **Orientation** (`scripts/orient_vote.py`, `scripts/signtest.py`). Scored the four
   candidate orientations (identity / flipX / flipY / flipBoth) over every scene part on every
   shipped `.DMap`: the un-mirrored reading `cell(i,j) -> (x+dx-w+1+i, y+dy-h+1+j)` has the
   highest rate of blocking part cells landing on base-blocked terrain. Per-part sign test over
   asymmetric parts: identity beats flipY 77-17 (p ~ 3e-10), flipX 124-2 (p ~ 2e-34), flipBoth
   109-18 (p ~ 5e-17). (Caveat: most of the statistical weight is the `skymaze` maps; the
   bridges themselves can't discriminate, since their base terrain is uniform river. The two
   refused tiles are blocked under *every* orientation, so the fix never rested on this.)
2. **Full pre/post reachability diff** (`scripts/reach_diff.py`, results in
   `scripts/reach_diff_results_2026-09-18.txt`). For every map with a scene overlay, computed
   the walkable set current-vs-proposed and compared connected components under FindPath's own
   movement model (8-neighbour, |dAlt| <= 200): **no component was split or erased on any city or
   event map, every file portal is still walkable, and the area reachable from a fixed anchor
   dropped by exactly the number of newly blocked tiles** (Twin City 340844 -> 340796; task07
   441322 -> 441288; task08 470206 -> 470174; p-arena 14245 -> 14171; faction-black 25246 ->
   25222). The single exception: the four `skymaze*` event maps each have one 133-135-tile
   pocket that splits in two; it holds no portal and isn't connected to any portal's region.
3. **Order-independent**: opens are collected first, rails applied second, skipping opened tiles,
   so overlapping parts (only on sky/skymaze/faction-black) end walkable regardless of order.

Locked in by `tests/map_tests.cpp` (`overlay_*`; parse the real files, skip if the install is
absent; set `COCLASSIC_GAME_ROOT` to point at a different install).

### If it ever looks wrong live

One specific tile: `MarkTileBlockedThisSession` / `MarkTileWalkableThisSession` heal it for the
session. A *pattern* of wrong tiles: re-run `scripts/reach_diff.py` and the orientation scripts
(edit `ROOT` in `rail_scan.py`), and check the game files weren't patched, before touching the
merge.

### Follow-up 2026-09-20 — does the bot actually use the affected maps? (closes the skymaze pocket)

The reachability diff cleared the maps by *portal* connectivity, which is the right test for a
crossing-driven map (Twin City) but not for a traversal-driven one, and the one region that split
(a 133-135-tile pocket on each `skymaze*` map) shares its risk with the orientation question (that
is where a wrong orientation would show up first). So: does any bot route touch those maps?

- **`skymaze` (1041), `skymaze1` (1060), `skymaze2` (1061), `skymaze3` (1062): the bot never
  uses them. The pocket split is moot.** None of the ids is a `MAP_*` constant, a gateway edge, or
  a `ResolveMapFile` alias, and no saved zone/route/profile (`coclassic*.ini`,
  `coclassic_profiles.ini`) names them (the only `1041` in `src/` is a *y-coordinate* in the
  Adventure Islands portal rows). They are event/instance maps the travel graph can't reach.
  (If someone ever adds them, re-check the pocket then: 135 tiles, x227-246/y289-324 on `skymaze`.)
- **Correction to the earlier "special maps" wording: two of the maps in the blast radius ARE
  used** — `task07` (1207) and `task08` (1208) are the Adventure Zone (`MAP_ADV_TASK07/08`
  gateway edges; a saved profile hunts `zoneMapId=1207` map-wide). They are covered by the same
  diff (34 / 32 newly blocked tiles, no split, reach down by exactly that count), and every
  gateway coordinate and the saved zone centre is still walkable and in the same component,
  261+ tiles from any newly blocked tile. The tiles come from the same assets as Twin City:
  `bridgeA` (all 32 on task08, x390-438/y348-356), `bridgeB-L` (18 on task07,
  x340-395/y208-216), plus `wbridge1` (16 on task07, x907-916/y918-967 — a *different* asset that
  Twin City doesn't exercise). Twin City's saved hunt zone (circle at (105,455), r=12) is 121
  tiles from the nearest newly blocked tile.
- Other special maps in the radius (`p-arena`, `faction-black`, `pk`, `bp-flag`,
  `CelestialChest`) aren't in the travel graph either.

### Live-confirmation watch list (what would falsify this)

1. **Twin City, both bridges** (crossing W->E and back): the log shows
   `[mapdata] scene overlay: ... N tiles blocked`; **any repeat refusal onto `(601,682)` /
   `(604,674)` falsifies the change.** A refusal onto one of the ~38 cap-corner tiles that only
   differ under an x-flip (`bridgeA`: (600,675) (600,681) (601,681) (603,681) (604,675) (604,681)
   (640,675) (640,682) (641,675) (641,682) (642,682) (643,682) (645,682) (646,682) (647,675)
   (647,682) (648,675) (648,682); `bridgeB-L`: (141,541) (141,547) (142,541) (142,547) (144,541)
   (144,547) (145,541) (145,547) (188,541) (188,548) (189,541) (189,548) (190,548) (191,548)
   (193,548) (194,548) (195,541) (195,548) (196,541) (196,548)) means the part orientation is
   mirrored in x.
2. **Adventure Zone traversal**: one Twin City `bridgeA` crossing also validates `task08`'s 32
   tiles (same asset, same orientation question). Separately watch a `task07` run for the
   `wbridge1` bridge near (907-916, 918-967) — the one asset Twin City can't confirm — and the
   `bridgeB-L` near (340-395, 208-216). Signal: bot stuck/repeating a jump beside those spans.
3. **Recovery if any of the above trips**: the merge is centralized in `MapGrid::ParseFile`, so
   scoping rail-blocking down (e.g. to the maps/scenes where it's been confirmed) or flipping the
   orientation is a one-place change; the `overlay_*` tests in `tests/map_tests.cpp` pin the
   current behaviour and would need the matching update.
