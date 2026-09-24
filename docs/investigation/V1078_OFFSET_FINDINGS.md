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

**Correction (2026-09-22): the earlier "COROUTINE_E9960/BF920 are the protected subset" framing was wrong** - both are
ordinary decoded x86 on both builds, on the same non-ciphertext pages as everything else measured by `regime_classify.py`
("protected"/"hook-resistant" in `docs/investigation/CONNECTION_DECIPHER_PREP.md` refers to something else - anti-tamper
resistance to a live hook, not signature-invisibility). `COROUTINE_BF920` relocates cleanly by exact bytes, exactly like any
other function (row above). `COROUTINE_E9960` genuinely does not relocate confidently on THIS pair: no exact32 match, no
shape match, and its one caller-anchor candidate (0x39C0C0, call ordinal 12 -> 0xEB620) is correctly REJECTED by `sigscan`'s
shape-sanity gate (similarity 0.15, threshold 0.5) - the candidate's prologue, branch sense and call sequence are all
different from the old function, so either it or its caller changed substantially between builds. That is a measured
"this one function needs live re-derivation," not a packing effect. Moot for the bot either way: per
`CONNECTION_DECIPHER_PREP.md`, our packets enqueue directly via `SEND_MSG_REAL` and never traverse this coroutine.
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

**Bottom line for the bot - two separate things, one done, one deferred:**
* **DONE, low risk (the bot can read the bag today):** `m_deqItem` at `0xB98` and `m_equipment` at `0xC00` are both real
  on v1078, confirmed by item name/id for every slot checked, using the exact pointer-scan approach `map_probe.h`
  already uses on v1074 - only the base offset changes. This is what the bot actually needs to read bag contents, and
  it self-verifies against on-screen items the same way the HP/level/silver fields do.
* **DEFERRED, feature-level only (not a port blocker):** "which of 40 numbered slots" / a literal capacity-40 field is a
  harder, separate question - the pool looks like a hash/bucket table, not a sequential array (see the cross-check
  below). Nothing in the bot's current action layer needs a specific empty-slot index. Do not spend time on the bucket
  addressing scheme until the core port is armed and a feature (e.g. "stop looting when the bag is full") actually
  needs it - then do the bounded dig at that point.


## Mana: max-mana field is simple, current-mana is NOT "same as HP" - it needs a different key first

Started the value-confirmation the user asked for; got as far as the architecture allows without the on-screen number.

* **`CHero::m_nMaxMana` / `m_bMaxManaValid` (`0xD20`/`0xD24` v1078, was `0xCF8`/`0xCFC`)** - a plain cached `CHero` field,
  same shift-model class as max HP. Read at level 1 (no skills yet): both 0, which could mean "correctly zero" or
  "cache never populated" - **not distinguishable without the real on-screen number**, unlike HP where 51 was
  immediately recognisable as real data.
* **Current mana is architecturally different from current HP, not a variant of the same fix.** HP's memory path is a
  fixed two-hop pointer chase (`m_pStatTable` -> `[+0x10]` -> `[+0]`) that happens to land on the HP record because
  that specific record's address is cached at a fixed struct offset. Current mana instead goes through
  `CStatTable::GetValue(int statType)` (`src/CStatTable.h`, native RVA `CSTATTABLE_GET_VALUE` = `0x1F1490` on v1074) -
  an integer-keyed lookup into what the dump shows is a red-black-tree-shaped node pool at the `CStatTable` object
  (confirmed structurally: repeating ~0x20-byte node rows with plausible left/right/parent pointers, keyed records
  each holding a small tagged value, at `0x5BD72A0` this run). **`statType`'s numeric value for mana is not known** -
  it is not defined anywhere in this repo (`CStatTable.h` only declares the signature) and no CO-emulator source is
  present locally to look it up (data-sourcing-ladder: checked next, none found). Finding it memory-only means
  identifying which tree node holds a value matching the on-screen MP - the same kind of search that found HP, just
  keyed differently, and it needs the real number to search for.
* **What's needed to finish this:** S411's on-screen current AND max MP (two different states again helps, e.g. before/
  after a mana potion or natural regen, the same way two HP states nailed down the HP record) - the dump already
  captured is sufficient to search once that number is known; no new checkpoint is required first.
* **CSTATTABLE_GET_VALUE / GETMAXMANA / GETCURRENTMANA are not added to the offset registry**: `src/game.h`'s own
  comment on this RVA block says `CHERO_GET_MAX_HP` resolves to misaligned garbage on v1074 and "the rest were never
  individually checked but ride the same stale dump and are equally unreliable" - building signatures from an
  unverified v1074 address would just manufacture a confident-looking wrong answer. `CHero::GetCurrentMana()` already
  has the same unverified-RVA safety guard that logs once and returns 0, mirroring the pre-fix HP bug class the user
  flagged - so no mana-potion logic should trust it until this is resolved.

## Second HP state confirms the path two-for-two (`cp04_hp_drop_33`)

User: HP now 33/51, stamina still 100/100, character is an archer with no mana pool. Same hero object, same stat-table
pointer chain (`CHero+0x978` -> `[+0x10]` -> `[+0]`) now reads **33**, exactly matching; max HP (0x3E0) and stamina
(0x6F0/0x6F4) are unchanged as expected. Two different HP values now both confirmed on the same live object -
this is the same two-state rigor the original v1074 HP fix used, not a single lucky match.

**Max mana resolved, not ambiguous:** `m_nMaxMana`/`m_bMaxManaValid` (`0xD20`/`0xD24`) now read **0 / valid=1** -
for an archer with no mana pool this is the correct value, not an unpopulated cache (the cache-valid flag being 1
is what distinguishes "genuinely zero" from "never computed"). **Current-mana via `CStatTable::GetValue(statType)`
remains unconfirmed** - this character has no mana stat to search for, so the stat-id-to-tree-node mapping described
above still needs a caster-class account's on-screen MP to resolve. Not blocking: nothing in the current bot targets
mana on this account, and the existing safety guard already prevents `GetCurrentMana()` from being trusted blind.

## Bag enumeration cross-check (user-reported: 6 of 40 slots in use)

Widened the scan around the known item pool (+-40 slots at the 0x20 stride) instead of following string hits alone: found
8 populated entries, of which 2 are the equipped LuckyBow/LuckyArrow re-appearing in the same pool. The remaining **6 match
the user's reported bag count exactly**: 2x Stancher (qty 1 each, distinct ids - likely the drop/pickup minted a fresh
instance) + 4x LuckyArrow stacks (qty 200 each - consistent with a 200-per-stack cap). Populated-slot relative offsets from
the pool anchor were irregular (-9, -2, 0, 1, 2, 5, 25, 37 in units of 0x20 bytes), which does not look like a plain
sequential array - more consistent with a hash/bucket table keyed by item id or type. The exact indexing scheme (and thus a
literal empty-slot / capacity-40 field) is NOT yet pinned down; only "which items are present" is confirmed, not "which of
40 numbered slots each occupies." Needed before any bag-slot-aware bot feature (e.g. "use an empty slot"): find the bucket
function (try `m_id % 40`, `m_idType`-based hashing, or a separate free-list) against a state where you know the true UI
slot positions.

## Next captures (each needs the state noted)
Items on the ground near the hero (confirms MAP_ITEM_VEC and the item record layout), an equipped item and a learned skill
(equipment / vecMagic / magic records), and a monster or NPC in view (role set records).

## Action-layer milestone: SEND_MSG_REAL live-validated (2026-09-23)

Everything above is the READ side. This is the first evidence on the ACTION side.

`coclassic_152784.log` (live v1078 client, user testing): `GameRva::CNETCLIENT_SEND_MSG_REAL` - the native call
underlying `packets.cpp`'s `SendPacket()`, which every action (pickup, movement, attack) routes through - executed
successfully and repeatedly with the game staying up the whole session:
* `DebugTestNativePickup: pickup packet sent, ok=true` (msgType 0x44D) - item genuinely picked up.
* Dozens of jump packets via the pathfinder, distances up to 18 tiles (msgType 0x3F2), zero refusals, zero crashes.
* Walk's own packet send (the pathfinder's short final-adjustment steps near a destination) - same path, also succeeded.

This is the first native call ever executed on v1078, and it held up under real, repeated use, not just one isolated
test. `GameRva::SEND_MSG_TESTED` is `true` and stays true.

**`GameRva::SET_COMMAND_TESTED` is separately gated and is ALSO now `true` in the repo/built DLL, but
`CRole::SetCommand()` (`CROLE_SET_COMMAND_REAL`) has not actually been executed yet** - the same log shows every walk
step's `SetCommand()` call being correctly refused (this was BEFORE the flag was flipped). Same confidence tier as
`SEND_MSG_REAL` (exact32, HIGH), but unproven until a dedicated supervised walk test passes. Do not treat it as
validated until that happens - see `docs/TEAM_NOTES.md`'s matching entry for the full status and the agreed test order.
