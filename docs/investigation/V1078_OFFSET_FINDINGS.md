# v1078 offset findings (offline, from one in-world `full` checkpoint)

Source: `C:\Users\Public\coclassic_capture\session_20260921_073000\cp02_idle_twincity` (image.bin + 360 MB private-heap pack,
1013 regions, 0 skipped; game stayed responsive). Client state noted at capture: character **S411**, level 1, HP 51/51, silver 4800,
Twin City (1002) at (237,191), standing still. Build: PE stamp 0x6AB0822B, SizeOfImage 0x2A26000.
Method: hero object found by its name string at `CRole+0x94`, then every field confirmed BY VALUE against the noted screen values.
Status legend: **VALUE-CONFIRMED** (live value matched what was on screen) / **STRUCTURE-OK** (pointer/shape plausible, value not yet
distinguishing) / **CANDIDATE** (static proposal only).
**Nothing in the bot uses these yet** - the launcher fence still refuses v1078, and no behaviour change is made without a go-ahead.

## Hero object (base found at heap 0x7ABA020 this run)
| field | v1074 | v1078 | status |
|---|---|---|---|
| CRole::m_id | 0x68 | 0x68 (1174578) | VALUE-CONFIRMED (matches roleset id) |
| CRole::m_szName | 0x94 | 0x94 ("S411") | VALUE-CONFIRMED |
| CRole::m_posMap | 0xD8 | 0xD8 = (237,191) | VALUE-CONFIRMED |
| posWorld / posScr / moveStart / moveDest | 0xE0/0xE8/0x108/0x110 | unchanged (world 32576,6864; scr 684,272) | STRUCTURE-OK |
| CRole::m_nMaxHp | 0x3D0 | **0x3E0** (51) | VALUE-CONFIRMED (+0x10) |
| CRole::m_nStamina / MaxStamina | 0x6E0/0x6E4 | **0x6F0/0x6F4** (100/100) | VALUE-CONFIRMED (+0x10) |
| CRole::m_nLevel | 0x6E8 | **0x6F8** (1) | VALUE-CONFIRMED (+0x10) |
| CHero::m_pStatTable (HP path: ptr -> [+0x10] -> [+0] = current HP) | 0x968 | **0x978** -> ... = 51 | VALUE-CONFIRMED (+0x10) |
| silver (registry name `CHero::m_qwRuntimeA30`) | 0xA80 | **0xAA8** (4800) | VALUE-CONFIRMED (+0x28) - it IS silver |
| CRole::m_idSyndicate / SyndicateRank | 0x714/0x718 | 0x724/0x728 (0/0) | STRUCTURE-OK (+0x10; zero either way) |
| CHero::m_deqItem | 0xB70 | 0xB98 (heap ptr) | STRUCTURE-OK (+0x28) |
| CHero::m_equipment, m_vecMagic | 0xBD8, 0x1968 | 0xC00, 0x1990 (empty at level 1) | CANDIDATE (+0x28; empty here) |
| CHero::m_nMaxMana / valid | 0xCF8/0xCFC | 0xD20/0xD24 | CANDIDATE (+0x28) |
| CHero::m_idActiveNpc | 0x3774 | 0x37BC (100122, plausible NPC id) | STRUCTURE-OK (+0x48) |
| CHero::m_bVip | 0x3790 | 0x37D8 | CANDIDATE (+0x48) |

Shape of the change: CRole gained 0x10 bytes somewhere in (0x290, 0x3E0]; CHero gained a further 0x18 between 0x978 and 0xAA8 (total
+0x28) and another 0x20 between ~0x19A8 and 0x37BC (total +0x48). Fields up to 0x188 are unchanged. Everything still keyed off
the same object graph. Bot impact: HP read must use 0x978; silver/level/max HP/stamina/etc. all shift as above.

## Globals (image RVA)
| name | v1074 | v1078 | status |
|---|---|---|---|
| ROLE_MGR_PTR (*ptr = CRoleMgr, `*rolemgr` = hero) | 0x69C730 | **0x6BCEF0** (+0x207C0) | VALUE-CONFIRMED: `*(*(base+0x6BCEF0))` == the hero object found by name |
| CURRENT_MAP_ID (+4 = alt) | 0x699560 | **0x6B9D40** / 0x6B9D44 (both 1002) (+0x207E0) | VALUE-CONFIRMED (map = Twin City). **sigscan's single-anchor proposal 0x6B9C30 was WRONG (value 0)** - see below |
| MAP_ITEM_VEC | 0x6994D8 | 0x6B9CB8 (+0x207E0) | STRUCTURE-OK: begin==end (empty, no items nearby), cap-begin 0x3F0, all heap ptrs; delta matches the map-id/scene group. Confirm with items on the ground |
| MAP_SCENE_PTR | 0x699370 | 0x6B9B50 (+0x207E0) | STRUCTURE-OK (heap pointer) |
| CONN_STATIC (+0x20 = connection object) | 0x69A4E8 | 0x6BACE0 (+0x207F8) | STRUCTURE-OK (qword0 = image vtable, +0x20 heap ptr) |
| CGAMEMAP vtable / MSGMAPITEM vtable | 0x5CCB60 / 0x5D2DF0 | 0x5E8348 / 0x5EE890 | STRUCTURE-OK (code-pointer slots) |

The five data globals in the .data block moved by +0x207C0..+0x207F8 (a few small insertions); the map-id/item/scene trio share
exactly +0x207E0, a good consistency check for the ones not yet value-confirmed.

## Code (image RVA), locators agree unless noted
| function | v1074 | v1078 | how |
|---|---|---|---|
| CNETCLIENT_SEND_MSG_REAL | 0x1DD450 | **0x1C70C0** (moved BACKWARDS 0x16390 by the linker) | shape-exact + two exact-matched callers call it at the same ordinal |
| CNETCLIENT_POLLER (registry 0x1DD860) | 0x1DD860 | 0x1C75E0 (candidate) | two exact-matched callers agree; the function grew (gap to SEND_MSG_REAL 0x410 -> 0x520) so it changed - verify |
| BEGIN_MSG / SEND_MAPITEM_MSG / COMPUTE_MAPITEM_SIZE | 0x190DD0 / 0x1C8B30 / 0x22AC50 | 0x19A0A0 / 0x1D71C0 / 0x23CF30 | exact32 (COMPUTE also callers) |
| COMMIT_STAGING | 0x3C4CD0 | 0x3DC860 | shape-exact |
| CONNECTION_SINGLETON accessor | 0xB7320 | 0xB8F00 | shape-fuzzy (medium: byte-identical accessor template) |
| CROLE_SET_COMMAND_REAL / HERO_ROLEMGR_ACCESSOR | 0x1B0660 / 0x181B30 | 0x1BC4E0 / 0x18A880 | exact32 (+callers for the accessor) |
| COROUTINE_BF920 | 0xBF920 | 0xC1140 | exact32 |
| COROUTINE_E9960 | 0xE9960 | not found | changed or moved beyond the locators |
None of the code addresses is behaviour-tested on v1078 (no game function has been called; read-only).

## Lessons recorded (they changed the tooling)
1. Single-anchor global proposals from sigscan are coin-flips: CURRENT_MAP_ID was wrong by 0x110. Rule confirmed: only >=2 agreeing sites is "high".
2. An ordering check is a heuristic, not a law: the linker moved SEND_MSG_REAL backwards, so the check flagged a correct match. `sigscan`
   now treats ordering as a warning that is cleared by an independent locator, and every code entry carries caller anchors for that.
3. Field-re-finding votes were wrong for m_nMaxHp (voted 0x3D0, real 0x3E0): windows can match a different field with the same shape.
   Value confirmation against known on-screen numbers is the arbiter; static votes only narrow the search.

## Equipment + inventory checkpoint (`cp03_items_equip`, action: drop/repick a Stancher, unequip/re-equip bow+arrow)

Same hero object (base 0x7ABA020, id 1174578); silver had dropped to 4349 by this point (durability repair or a shop
interaction between checkpoints - the silver field tracked it correctly either way).

**Equipment array, `CHero+0xC00` (v1074 `0xBD8`, +0x28) - VALUE-CONFIRMED.** Each of the 8 `EquipSlot`s is a 0x10-byte
{item ptr, control ptr} pair, slot index = (offset-0xC00)/0x10, matching `src/CHero.h`'s `EquipSlot` enum exactly:

| slot | offset | item found |
|---|---|---|
| ARMOR (2) | +0x20 | **Coat** (id 295829487, idType 132804) |
| RWEAPON (3) | +0x30 | **LuckyBow** (id 295829486, idType 500301) |
| LWEAPON (4) | +0x40 | **LuckyArrow** (id 295829494, idType 1050000, amount 200) |

Exactly the loadout you reported (armor + bow + arrow) - this is a real value match, not a structural guess.

**Bag / `m_deqItem`, `CHero+0xB98` (v1074 `0xB70`, +0x28) - the pointer chases to a real container, confirming the offset,
but the *container itself* needs the scan approach already flagged in `src/map_probe.h` (`DebugFindInventory`), not a fixed
sub-offset for size:** `[hero+0xB98]` is an MSVC `deque` control block whose second qword (`0x5ED50570` this run) is the
deque's internal map; a pointer near it (`+0x58`) leads to the item-slot block, entries every 0x20 bytes ({item ptr, control
ptr, small tag, packed header}), found by scanning outward from the string `"Stancher"` and confirming by raw-pointer
cross-reference (no offset was assumed, every hop was a value found in the dump). That block holds, among others:

| item | id | amount | note |
|---|---|---|---|
| LuckyBow | 295829486 | 1099 | mirrors the equipped weapon |
| LuckyArrow | 295829498 | 200 | a second stack, alongside the equipped one (295829494) |
| **Stancher** | 295829490 | 1 | the one you dropped and re-picked-up |
| LuckyArrow | 295829494 | 200 | the equipped stack, also present here |
| **Stancher** | 295829488 | 1 | a second, separate Stancher instance |

Two distinct Stancher instances (different `id`, same `idType`) is consistent with drop-then-pickup minting a new instance
and the old one being a still-resident, about-to-be-freed duplicate - not investigated further, not load-bearing for anything.

**Bottom line for the bot:** `m_deqItem` at `0xB98` and `m_equipment` at `0xC00` are both real on v1078 (name/id confirmed
for every slot checked). The equipment array can be read directly at those fixed offsets. The bag must keep using a
pointer-scan for items (as the v1074 code already does per `map_probe.h`), because its size/count is not at a trivial
fixed suboffset in this MSVC deque layout - true on v1074 too, so no behaviour change is needed there, only the base
offset used for the scan.

## Next captures (each needs the state noted)
Items on the ground near the hero (confirms MAP_ITEM_VEC and the item record layout), an equipped item and a learned skill
(equipment / vecMagic / magic records), and a monster or NPC in view (role set records).
