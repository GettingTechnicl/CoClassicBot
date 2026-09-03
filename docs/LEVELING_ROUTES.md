# Leveling routes — maps, level bands, and spawns

Durable route data for the automated leveling project (0 → 120; Water Taoist 0 → 110).
Companion to [`GAME_MECHANICS.md`](GAME_MECHANICS.md) — that file is *how the game works*,
this one is *where to go at what level*.

**Source:** community zone maps supplied by the user 2026-09-02. Each map shows monster
names, level bands, and roughly where each spawns relative to the town/landmarks. Levels
are as printed on those maps; anything uncertain is marked, not guessed.

> Getting more of these is the cheapest way to extend this table. Prefer them over having
> the user hand-walk and record coordinates — see the `coclassicbot-data-sourcing-ladder`
> memory: game data files first, community/emulator sources second, hand capture last.

## Level progression at a glance

| Level band | Map | Notes |
|---:|---|---|
| 1 – 25 | **Twin City** | Starting zone; the whole early game |
| 27 – 45 | **Phoenix Castle** | |
| 47 – 81 | **Ape Mountain** | Widest single-map band |
| 67 – 83 | **Desert City** | Overlaps Ape Mountain — choice, not a gap |
| 87 – 98 | **Bird Island** | |
| ? | **Mystic Castle** | Indoor/cave map; level bands NOT on the supplied map |

Coverage is continuous 1 → 98 apart from small gaps at 26 and 46, which are almost
certainly just "keep killing the band below" rather than a missing map.
**Above 98 is not yet covered** — Mystic Castle levels still needed, and nothing yet for
the 100 → 120 stretch the end goal requires.

## Twin City — levels 1 – 25

Map id **1002** (`map/map/newplain.DMap`, 972×972).

| Monster | Level | Where (relative to the city) |
|---|---:|---|
| Pheasant | 1 – 3 | Just outside the city walls, west side |
| TurtleDove | 7 – 10 | South-east, open grass |
| Robin | 12 | South-west, across the water in the wooded/brown area |
| Apparition | 17 | West, past the water |
| Poltergeist | 22 – 25 | North-west, in the dark/burnt terrain |

The city sits centre-right on an island ringed by water; the low-level spawns are on the
same landmass, and the 12+ spawns are across the river — which is why the bridges matter
for an unattended leveling run.

**Portals (from the map file's own tail section, exact):**
`(401,387)→#2`, `(382,387)→#3`, `(44,394)→#1`, `(963,557)→#7`, `(223,196)→#8`, `(555,964)→#9`

## Phoenix Castle — levels 27 – 45

| Monster | Level | Where |
|---|---:|---|
| WingedSnake | 27 – 28 | North-east, near the water |
| *(unnamed on map)* | 30 | North-east corner, above WingedSnake |
| Bandit | 32 – 33 | Centre |
| Cateran | 35 | Centre-west |
| Ratling | 37 – 43 | East |
| *(unlabelled, blue canine)* | 40 – 45 | East, right of Ratling |

Two spawns carry a level band but no readable name on the supplied map — capture the names
in-game when the bot first reaches this zone.

## Ape Mountain — levels 47 – 81

Canyon/plateau map, four arms meeting at a centre crossroads, bridges at the arm ends.

| Monster | Level | Where |
|---|---:|---|
| Macaque | 47 – 48 | South arm, by the walled enclosure |
| GiantApe | 52 – 53 | East arm |
| ThunderApe | 57 – 58 | North arm |
| SnakeMan | 63 – 65 | West arm |
| Titan | 75 – 81 | West arm, alongside SnakeMan |

The 65 → 75 gap on this map is where **Desert City** picks up.

## Desert City — levels 67 – 83

Coastal desert; the town sits on the shore, water to the west/south-west.

| Monster | Level | Where |
|---|---:|---|
| SandMonster | 67 – 70 | South of town, near the shore |
| HillMonster | 72 – 78 | East, by the ruined walled structure |
| BladeGhost | 82 – 83 | North |

## Bird Island — levels 87 – 98

Archipelago — small islands separated by water, so **portals/teleports matter more than
pathing here.** Cross-references the portal work already captured in the
`coclassicbot-leveling-system` memory.

| Monster | Level | Where |
|---|---:|---|
| Birdman | 87 – 88 | South, near the large walled compound |
| HawKing | 92 | West island |
| HawKing | 93 | North-east island |
| BanditL97 | 97 | Centre island |
| Banditti | 97 – 98 | North-west and west islands |

## Mystic Castle — levels unknown

Indoor/cave map, dark, laid out as connected platforms.

Monsters present: **Bull Monster, Vampire Bat, Bloody Bat, Tomb Bat, Jinx Tomb Bat,
Red Devil, Bloody Devil.**

The supplied map shows **no level bands** for this zone. Given it follows Bird Island it is
presumably 100+, but that is an inference — confirm before routing anything to it.

## Still needed

- Mystic Castle level bands.
- Anything covering **99 → 120** (and the Water Taoist 110 cap).
- Names for the two unnamed Phoenix Castle spawns (Lvl 30, Lvl 40 – 45).
- Gear purchase thresholds per class and the NPCs that sell them — not on these maps.
- Map ids for every zone above except Twin City (1002); resolvable from
  `ini/GameMap.json` once each zone's `.DMap` is identified.

---

# World map — how the main cities connect

Source: community world map supplied by the user 2026-09-02. Cross-checked against
the portal positions each `.DMap` file declares (exact) and the bot's `gateway.cpp`
table. **Every link below is confirmed by the map file on both ends.**

```
                 Desert                         Bird Island (1015)
         5 Mystic Castle  7 Fountain                   |
         6 Yumen Village  4 Desert City (1000)         | TC #8 (223,196) <-> BI #28 (1018,710)
                 |                                     |
        DC #1 (977,668) <-> TC (69,473)                |
                 |                                     |
                 +---------  Plain  --------------------+
                   3 Plain Mine   1 Twin City (1002)   2 Peach Altar
                      |                  |                   |
        TC #1 (44,394)->Mine Cave   TC #9 (555,964)     TC #7 (963,557)
                                         |                   |
                                   Canyon (1020)        Forest (1011)
                                   9 Ape Mountain       Phoenix Castle
                                         |              11 Waterfall Cave
                              Ape #0 (926,557) <-> Phoenix #2 (448,822)
                                                        12 Forest Village
```

| From | Portal (file) | To | Landing | Status |
|---|---|---|---|---|
| Twin City | #7 (963,557) | Phoenix Castle | (11,376) | in table, validated |
| Twin City | #9 (555,964) | Ape Mountain | (381,21) | in table, validated |
| Twin City | #8 (223,196) | Bird Island | (1010,710) | in table, validated |
| Twin City | #1 (44,394) | Mine Cave | (160,96) | in table, validated |
| Desert City | #1 (977,668) | Twin City | (69,473) | in table, validated |
| Desert City | #0 (77,319) | Mystic Castle | (312,646) | in table, validated |
| **Ape Mountain** | **#0 (926,557)** | **Phoenix Castle** | **(453,819)** | **live-validated 2026-09-02** (user walk) |
| **Phoenix Castle** | **#2 (448,822)** | **Ape Mountain** | **(919,559)** | **live-validated 2026-09-02** (log) |

Twin City → Desert City is an NPC (GeneralPeace @ TC (60,463)), not a tile portal.

**Map 1010 exit** (the one-NPC dialogue that leaves the newbie map, per the user): lands in
Twin City at **(377,336)** — live log `[portal] 1010 (87,33) -> 1002 (377,336)`. The NPC stands
near (87,33) on map 1010.

**Still unknown, per map file** — these exist but nothing says where they go:
- ~~Twin City #2, #3~~ **Captured 2026-09-02 (user walk, both directions):**
  - TC **#2 (401,387)** → map **1004** `forum.DMap` (96×96) landing (51,70); return 1004 #0 (51,73) → TC (403,394)
  - TC **#3 (382,387)** → map **1006** `horse.DMap` (52×52) landing (37,31); return 1006 #0 (41,31) → TC (386,391)
  Both are small in-city rooms, not routes — recorded for completeness, not added to the travel table.
- ~~Phoenix Castle #1~~ **Captured 2026-09-02 (user walk, both directions) — the Waterfall Cave chain:**
  ```
  Phoenix #1 (379,23)  -> 1013 tiger.DMap  (104x104) land (54,81)      [Waterfall Cave]
  1013   #1 (57,21)    -> 1014 dragon.DMap (172x172) land (80,40)
  1014   #1 (137,90)   -> 1016 qiling.DMap (96x96)   land (50,82)      [NPC PirateZhou here]
  back:  1016 #0 (50,88) -> 1014 (131,86);  1014 #0 (77,32) -> 1013 (59,28);  1013 #0 (55,87) -> Phoenix (379,28)
  ```
  **PirateZhou** (map 1016) offers passage to a map called **"Sea of Death"** for **50,000 gold**. Not taken.
  The name is server-side — it appears nowhere in the client's text/ini. Only sea-themed map file in the
  client is `seabed01.DMap` = id 3056 (the bot calls it "Ocean", reached via Guild Conductor). Sea of Death
  is either the private server's name for 3056 or a custom map; unverified until someone pays the fare.
- **Phoenix Castle mine cave — captured 2026-09-02 (user walk, both directions).** Entry is an **NPC**
  at ~(934,564) on Phoenix's east edge (dialogue, not a tile portal — so it has no file record and the
  log correctly said "no file portal"). Exit is a tile portal inside the mine.
  ```
  Phoenix NPC (934,564)  -> 1025 mine-one.DMap (184x184) land (32,70)
  1025 #0 (27,65)        -> Phoenix (933,565)
  ```
  NPC name not yet captured (needed for a travel-table `Npc` gateway entry). Pattern so far:
  Phoenix = `mine-one` (1025), Twin City = `mine-four` (1028) — one mine per city; Ape / Desert are
  presumably `mine-two`/`mine-three` (1026/1027).
- ~~Phoenix Castle #4~~ **Captured 2026-09-02 (user walk, both directions) → "Phoenix Castle 2":**
  ```
  Phoenix #4 (855,482) -> 1076 newwoods.DMap (1000x1000) land (11,361)
  1076    #0 (2,360)   -> Phoenix (850,485)
  ```
  An exact copy of Phoenix Castle (user's description). **Added to the travel table** as
  `MAP_PHOENIX_CASTLE2`, same shape as Ape City 2.

  **This completes the "new*" pattern** — each main city has a duplicate reached from one of its
  edge portals: Ape Mountain → 1075 `newcanyon` (Ape City 2), Phoenix → 1076 `newwoods` (Phoenix 2),
  and by extension Desert City → 1077 `newdesert` (= "New Desert City", the Adventure Zone entrance)
  and Bird Island → 1078 `newisland` (33 portals). Desert #3 (163,471) is the likely door to 1077.
- ~~Ape Mountain #4~~ **Captured 2026-09-02 (user walk, both directions) → "Dreamland":**
  ```
  Ape #4 (11,377)   -> 1012 sky.DMap (588x588, only 2.6% walkable) land (532,529)
  1012 #0 (531,534) -> Ape (23,383)
  ```
  Added to the travel table as `MAP_DREAMLAND`. **Ape Mountain is now fully mapped** — all four of
  its file portals plus the mine NPC have known destinations.
- **Ape Mountain mine — captured 2026-09-02 (user walk, multiple floors).** Entry is an **NPC** at
  ~(527,888) on Ape's south edge (no tile portal). Exit is `mine-two` portal #0.
  ```
  Ape NPC (527,888) -> 1026 mine-two.DMap (216x216) land (138,103)         [Ape's mine: mine-two]
  1026 (181,157)    -> 2021 Dgate.DMap (550x550)     land (15,77)          [dungeon hub]
      2021 (156,105) -> 2025  land (15,77)   [no file entry -> aliased to Dgate]
          2025 (162,111) -> 2033  land (15,77)   [aliased Dgate]     back 2033 (14,77) -> 2025 (162,111)
          2025 (112,158) -> 2032  land (72,15)   [aliased Dsigil]    back 2032 (72,19) -> 2025 (113,155)
          back 2025 (25,81) -> 2021 (162,111)
      2021 (116,152) -> 2024 Dsigil.DMap (302x302)  land (72,15)
          2024 (162,118) -> 2031  land (15,77)   [aliased Dgate]     back 2031 (13,79) -> 2024 (162,111)
          2024 (112,150) -> 2030  land (72,15)   [aliased Dsigil]    back 2030 (72,15) -> 2024 (113,155)
          back 2024 (76,21) -> 2021 (113,155)
  back 2021 (15,77) -> 1026 (187,159);  1026 #0 (136,98) -> Ape (528,890)
  ```
  Every hop inside is NPC/stairs (none are file portals). The dungeon is a **tree of two repeated
  templates**: floors landing at (15,77) behave like Dgate (2021), floors landing at (72,15) like
  Dsigil (2024). Ids 2025/2030/2031/2032/2033 have **no `GameMap.json` entry** — the map-id ≠ doc-id
  problem again — and are now aliased in `ResolveMapFile` (inferred; verify terrain on next visit).
  Also in the client but not yet visited: 2022 `Dsquare` (424²), 2023 `Dcloister` (676²).
  **Mine pattern confirmed:** Phoenix = mine-one (1025), Ape = mine-two (1026), Twin City = mine-four
  (1028) → Desert is almost certainly mine-three (1027).
- Desert City #3 (163,471) — likely Fountain (7) / Yumen Village (6)
- Bird Island: 30 of 31 inner-island portals (checklist in the map file; walk with the logger)
- ~~Mine Cave (49,71) → Grotto 1 suspect~~ **Confirmed working by user walk** (`1028 (49,71) -> 1926 (339,353)`), even though the map file declares no portal there and the tile is plain walkable ground. **So the file's portal list is NOT exhaustive** — some transitions exist without a `.DMap` portal record. Treat the file list as "definitely exists", never as "this is all".
- **BUG — the bot is blind on Frozen Grotto 1.** Server map id **1926** has no `GameMap.json` entry; the file `icecrypt-lev1.DMap` is DocumentId **1762**. Live log: `[mapdata] no .DMap entry for map id 1926`. The bot loads no terrain there → no pathfinding, no walkability. Map id and document id are separate things in this client (`CGameMap::m_idMap` vs `m_idDoc`); the bot resolves files by map id and only works where the two coincide. Needs a map-id → doc-id alias (1926 → 1762) at minimum; other maps may have the same split.

# Adventure Zone — future project

Source: community map supplied by the user 2026-09-02; explicitly "something I've been
planning to work with you on." Chain of numbered maps, entered from **New Desert City**
(map **1077**, `newdesert.DMap`):

```
1 Entrance (New Desert City) -> 2 -> 3 -> 4 -> 5 Backup City -> 6 -> 7
    7 -> 7.1 -> 7.2
    7 -> 7.3 -> 7.4 -> 7.5
    7 -> 8 (Mine Cave, 8.1) -> 9 (Meteor Zone nearby) -> 10 -> 11 Adventure Islands
```

## Captured so far — 2026-09-02, user walk in progress (all hops both directions unless noted)

**Every map below loaded terrain normally — none of these ids has the map-id/doc-id problem.**
The zone's maps are the `task*` / `*-snail` / `*-fairy` files, ids 1201–1216.

```
1077 New Desert City (360,6)  -NPC->  1205 task05  (1351,1198)     [map 2]     back: 1205 #1 (1355,1198) -> 1077 (362,12)
1205 #0 (3,160)               ->      1202 task02  (691,398)       [map 3]
1202 #0 (3,301)               ->      1201 task01  (592,398)       [map 4]
1201 #0 (3,201)               ->      1213 desert-snail (1072,798) [map 5 "Backup City" — desert map]
1213 #0 (4,281)               ->      1204 task04  (1351,1199)     [map 6]
1204 #0 (3,160)               ->      1216 newplain-fairy (950,478) [map 7 — THE HUB, 4 portals]
        1216 #3 (954,479) east  -> 1204 (9,161)            back the way you came
        1216 #2 (480,3)  north  -> 1215 woods-fairy (9,376)
        1216 #0 (3,480)  west   -> ?      1216 #1 (479,954) south -> ?
```

**Portal inventory of the whole zone, from the map files** (what's left to walk):

| id | file | size | portals |
|---|---|---|---|
| 1201 | task01 | 600² | #0 (3,201) ✓, #1 (594,397) |
| 1202 | task02 | 700² | #0 (3,301) ✓, #1 (694,398) |
| 1204 | task04 | 1360² | #0 (3,160) ✓, #1 (1356,1199) |
| 1205 | task05 | 1360² | #0 (3,160) ✓, #1 (1355,1198) ✓ |
| 1207 | task07 | 1220² | #0 (260,4), #1 (958,1214) |
| 1208 | task08 | 1020² | #0 (3,500) |
| 1210 | task10 | 1048² | #0 (4,329) |
| 1211 | task11 | 960² | #0 (3,400), #1 (955,558) |
| 1212 | island-snail | 1360² | **33 portals** — this is "Adventure Islands" (map 11) |
| 1213 | desert-snail | 1080² | #0 (4,281) ✓, #1 (1075,798) |
| 1214 | canyon-fairy | 1440² | #0 (799,1434), #1 (640,3) — dark canyon, likely 7.1 / 7.2 |
| 1215 | woods-fairy | 944² | #0 (3,376) ✓ (arrival), #1 (938,567) |
| 1216 | newplain-fairy | 960² | #0 (3,480), #1 (479,954), #2 (480,3) ✓, #3 (954,479) ✓ — the hub |

**Branches off the hub — captured 2026-09-02 (continued):**
```
NORTH  1216 #2 (480,3)  -> 1215 woods-fairy (9,376)         back: 1215 #0 (3,376)   -> 1216 (481,10)
         1215 #1 (938,567) -> 1207 task07 (958,1211)        back: 1207 #1 (958,1214) -> 1215 (934,565)
           1207 #0 (260,4)  -> 1208 task08 (9,502)           back: 1208 #0 (3,500)   -> 1207 (261,10)   [dead end: 1208 has 1 portal]
SOUTH  1216 #1 (479,954) -> 1214 canyon-fairy (641,9)        back: 1214 #1 (640,3)   -> 1216 (479,950)  [7.1]
         1214 #0 (799,1434) -> 1217 (363,13)  NO FILE ENTRY  back: 1217 (360,6) NPC -> 1214 (799,1432) [7.2 — aliased to newcanyon, inferred]
WEST   1216 #0 (3,480) -> 1210 task10 (1039,718) = "METEOR ZONE" [map 9]   ← TILE PORTAL, CONFIRMED 21:06
       NPC "OldExplorer" at (12,476) goes to the identical landing (1039,718) — two doors, same room.
       The tile portal is the one the bot should use (no dialogue). One-way: Meteor Zone's own exit
       (#0 at (4,329)) goes on to 1211 task11, not back to the hub.
           1210 (719,1036) NPC "MineSupervisor" -> 1218 (29,70)  NO FILE ENTRY = METEOR ZONE MINE CAVE
                                                  [aliased to mine-a 1500, inferred — verify terrain]
           1218 (28,66) NPC -> 1210 (715,1036)   mine exit (presumably the same MineSupervisor)
```
Still to walk: Meteor Zone's exit back to the hub, the Meteor mine's exit, 1211 task11 (2 portals),
and 1212 island-snail = **Adventure Islands** (33 portals).

### Adventure Islands (1212 `island-snail`, 1360×1360) — the islands, computed from the map file

No bridges on this map (zero scene records): every island is a separate walkable component and
**all inter-island travel is by portal.** Islands are numbered by connected component — the user
does not need to name them. 17 components; 15 are real islands (≥200 tiles), 2 are islets holding
portals #19 (872,847), #20 (861,846), #22 (871,856) just south of I7.

| island | tiles | x-range | y-range | file portals on it |
|---|---:|---|---|---|
| I7  | 42565 | 778–1053 | 540–840 | #17 (846,555), #18 (898,828), #24 (810,677), #30 (1049,838) |
| I14 | 42288 | 749–1025 | 1000–1298 | #27 (906,1041), #32 (1015,1295) |
| I3  | 42100 | 98–373 | 357–658 | #5 (372,651), #6 (352,512) |
| I0  | 40201 | 352–617 | 27–317 | #11 (375,153), #12 (614,314) |
| I13 | 11762 | 1134–1270 | 889–1041 | #31 (1135,892) |
| I12 | 11726 | 894–1029 | 889–1040 | #23 (896,891), #26 (942,1015), #28 (1025,1038) |
| I10 | 11718 | 577–714 | 820–972 | #1 (708,921), #2 (579,824) |
| I6  | 11714 | 557–693 | 521–672 | #25 (693,667) |
| I1  | 10665 | 209–341 | 153–288 | #9 (338,285), #10 (310,171) |
| I2  | 6789 | 705–840 | 311–460 | #15 (710,377), #16 (837,458) |
| I4  | 3311 | 389–462 | 385–458 | #7 (404,454), #8 (392,387) |
| I8  | 3307 | 474–546 | 685–758 | #3 (542,755), #4 (476,688) |
| I5  | 3293 | 618–690 | 397–470 | #13 (620,399), #14 (674,415) |
| I15 | 3056 | 1082–1151 | 1117–1188 | #29 (1083,1120) |
| I9  | 1701 | 743–813 | 783–841 | #0 (781,841), #21 (811,826) |

**Destinations (from the walk log — `via portal #N -> (x,y)`; landing tile → island):**

| from | portal | lands | on island | pair |
|---|---|---|---|---|
| I14 | #32 (1015,1295) | 1211 task11 (10,401) | **exit to task11** | ↔ 1211 #0 |
| I14 | #27 (906,1041) | (942,1012) | I12, beside #26 | #26 unwalked |
| I12 | #28 (1025,1038) | (1087,1124) | I15, beside #29 | ↔ #29 |
| I15 | #29 (1083,1120) | (1022,1035) | I12, beside #28 | ↔ #28 |
| I12 | #23 (896,891) | (871,853) | islet, beside #22 | ↔ #22 |
| islet | #22 (871,856) | (899,894) | I12, beside #23 | ↔ #23 |
| islet | #19 (872,847) | (900,826) | I7, beside #18 | ↔ #18 |
| I7 | #18 (898,828) | (872,850) | islet, beside #19 | ↔ #19 |
| islet | #20 (861,846) | (808,823) | I9, beside #21 | ↔ #21 |
| I9 | #21 (811,826) | (864,846) | islet, beside #20 | ↔ #20 |
| I7 | #24 (810,677) | (690,667) | I6, beside #25 | ↔ #25 |
| I6 | #25 (693,667) | (813,677) | I7, beside #24 | ↔ #24 |
| I7 | #17 (846,555) | (834,455) | I2, beside #16 | #16 unwalked (presumably ↔) |
| I2 | #15 (710,377) | (674,418) | I5, beside #14 | ↔ #14 |
| I5 | #14 (674,415) | (713,377) | I2, beside #15 | ↔ #15 |
| I5 | #13 (620,399) | (611,311) | I0, beside #12 | #12 unwalked (presumably ↔) |
| I0 | **#11 (375,153)** — inferred | **map 1219** | **off-map, NO FILE ENTRY** | client froze on this hop; character died on 1219 |

**Pattern so far:** portals come in adjacent pairs — you land right beside the return portal, and
every walked pair is a two-way link. The islet south of I7 is a 3-way junction (#19/#20/#22).

**Route through the islands, as walked:** arrive I14 (#32) → #27 → I12 → #28 → I15 → #29 → I12 → #23 →
islet → #19 → I7 → #24 → I6 → #25 → I7 → #17 → I2 → #15 → I5 → #13 → I0 → (#11) → **1219**.

**Community-map numbering, user-confirmed 2026-09-02:** the top of the Adventure Areas map runs
Meteor Zone (**1210**) → an unnamed connector (**1211** task11) → **Adventure Islands 1 = 1212** →
**Adventure Islands 2 = 1219**, the two island-shaped maps drawn as "10" and "11". Both instances use
the same file, so the island table and portal numbers below apply to both.

**Map 1219 = a SECOND INSTANCE of Adventure Islands.** (Its full walk is tabulated below.) The hero's position on it was (1010,1237);
exactly one client file has that tile walkable — `island-snail` (1212) — with all 49 neighbours open.
Aliased 1219 → 1212. (1010,1237) is on that instance's **I14**, 58 tiles from #32. No `GameMap.json`
entry. The client froze on the transition in — the bot logged nothing for it, so "#11" is inferred
from I0 having only #11 and #12 and #12 being the arrival spot from #13. **Portal numbering on 1219 is
the same as 1212** (same file), so the island table above applies to both instances.

### Adventure Islands 2 (1219) — walked 2026-09-02 21:00–21:03, 21 portals, all by number

Same file as AZ1, so same islands and numbers. Every walked link is an adjacent pair, same as AZ1.

| portal | from → to |
|---|---|
| #27 | I14 → I12 |
| #28 ↔ #29 | I12 ↔ I15 |
| #23 | I12 → islet |
| #19 ↔ #18 | islet ↔ I7 |
| #20 | islet → I9 |
| **#0** | **I9 → I10** (beside #1) |
| **#2** | **I10 → I8** (beside #3) |
| **#4** | **I8 → I3** (beside #5) |
| **#6 ↔ #7** | **I3 ↔ I4** |
| **#8** | **I4 → I1** (beside #9) |
| **#10 ↔ #11** | **I1 ↔ I0** |
| **#12** | **I0 → I5** (beside #13) |
| #13 | I5 → I0 |
| #14 ↔ #15 | I5 ↔ I2 |
| **#16 ↔ #17** | **I2 ↔ I7** |
| **NPC "Boatman" at (386,40) on I0** | **→ 1213 desert-snail (448,272) = Backup City (map 5)** — the exit |

**The complete island graph** (both instances agree wherever both were walked):
I14 –#27/#26– I12 –#28/#29– I15 · I12 –#23/#22– islet –#19/#18– I7 · islet –#20/#21– I9 –#0/#1– I10 –#2/#3– I8
–#4/#5– I3 –#6/#7– I4 –#8/#9– I1 –#10/#11– I0 –#12/#13– I5 –#14/#15– I2 –#16/#17– I7 –#24/#25– I6.
Not yet walked on either instance: #30 (I7), #31 (I13), #26, and the reverse halves #1 #3 #5 #9 #21 #22 #25.

**CORRECTION — how AZ1 leads to AZ2.** I had inferred AZ1's #11 goes to 1219. AZ2 shows #11 is simply
the I0 ↔ I1 pair (#10/#11), and AZ2's exit is a **Boatman NPC on I0 at (386,40)**. The far more likely
story is that AZ1 has the same Boatman on its I0 and *that* is what took you to AZ2 (the client froze on
the hop, so nothing was logged). Treat "AZ1 #11 → 1219" as **withdrawn**; AZ1's I0 Boatman → 1219 is the
working assumption, unverified.

**Unwalked (17):** #0, #1, #2, #3, #4, #5, #6, #7, #8, #9, #10, #12, #16, #26, #30, #31 — and #11 if
it wasn't the one. By island: I0 #12; I1 #9,#10; I3 #5,#6; I4 #7,#8; I8 #3,#4; I10 #1,#2; I7 #30;
I13 #31; I12 #26; I2 #16; I9 #0. Islands I1, I3, I4, I8, I10, I13 not yet reached at all.

**How you get in:** 1210 Meteor Zone #0 (4,329) → 1211 task11 (951,558) → 1211 #0 (3,400) →
1212 Adventure Islands (1017,1294) — you arrive on **I14**, next to #32 which is the way back out.
So Meteor Zone → task11 → Adventure Islands is the route; Meteor Zone does NOT return to the hub
from its west side. Meteor mine exit: 1218 (28,66) → 1210 (715,1036) by NPC (no tile portal —
consistent with the mine-a..d template family, which has none).

Generated by `scratchpad/islands.py`; re-run after any parser change. Map ids 1203, 1206, 1209 are absent
from the client — the zone skips those numbers.

Not yet in the travel table — will be added once the chain is complete.

# Labyrinth (Lab I – IV)

Source: community map supplied by the user 2026-09-02, with key: entry point, general,
big mob spawns, big special mob spawns, lab boss. No coordinates on the source.

**The client DOES ship these maps** — ids **1351–1354** = `s-task01..04.DMap`, sizes
496 → 688 → 808 → 944, growing exactly as Lab I–IV do on the map. All four declare
`portals=0`, so entry/descent is by NPC or item, not a tile portal. The user's memory that
"the bot couldn't navigate down there" is almost certainly the gateway table having no Lab
entries — not missing terrain. The scene-overlay parser loads them like any other map.
