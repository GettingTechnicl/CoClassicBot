# Classic Conquer 2.0 — Game Mechanics Reference

Living reference for how the *game itself* works, so bot development is
grounded in real player understanding rather than reverse-engineered
guesses. Everything the bot decides — what to loot, when to flee, how to
fight, what's valuable — is downstream of these mechanics.

**Legend:** ✅ = confirmed (code/itemtype.json/live-verified). ❓ = OPEN,
needs the user's answer. Fill the ❓ sections in as answers come; keep it
honest about what's verified vs assumed.

---

## Game identity ✅
- **Classic Conquer 2.0**, client build **v1074**, `ImConquer.exe`,
  Themida-packed, runs admin-elevated on Windows.
- Isometric 2.5D MMORPG. Server-authoritative movement/combat.
- Item database on disk: `F:\Games\Classic Conquer 2.0\ini\itemtype.json`
  (11,142 item types), name/stat source of truth.

---

## Character classes / professions ✅
Five classes:
- **Trojan** — 2-hand melee. Can equip **2× one-hand weapons** (dual-wield)
  OR a 2-hand weapon. Uses Cyclone + Superman XP skills.
- **Warrior** — melee. Can equip **only a 2-hand weapon**, OR **1 one-hand
  weapon + a shield**.
- **Archer** — ranged, shoots with a bow, **Scatter** hits multiple
  monsters. **Best class for hunting/farming.**
- **Water Tao** — healer (has revives).
- **Fire Tao** — like Water Tao but **weaker heals, no revives, stronger
  attacks**.
- **Rebirth**: at **level 110** a character can **reborn into another
  class**. **Max level is 130.**
- (Ties to the code: `requiredProfession` on items gates equipment by
  class; a Trojan/Warrior can't use a bow, etc.)

---

## Skills ❓
Confirmed skills the bot already reacts to:
- **Cyclone** (Trojan) — attack-speed buff; character attacks much faster
  while active. Bot detects `IsCycloneActive()` and uses a faster attack
  interval. **Cyclone is maintained by continuously killing** (drops if
  kills stop). ✅
- **Superman** (Trojan) — very strong; with the **"Snow"** skill it hits
  **multiple nearby targets at once** (genuine AoE). Bot detects
  `IsSupermanActive()` and randomizes target selection to spread damage. ✅
- **Scatter** (Archer) — AoE cone attack, hits a clump. ✅
- **Fly** (Archer) — makes the archer immune to melee (bot ignores archer
  safety distance while active). ✅

### Cyclone / Superman — XP-skill mechanics ✅
- Both are **XP skills**: granted when an **XP bar fills to 100**.
- The bar fills **1 point per second (time)** OR **1 point per monster
  kill** — whichever, they stack.
- **The active duration extends with each monster killed.** So while
  speed-hacking (very fast kills), **the skill never runs out** as long as
  the bot keeps killing good mobs. This is exactly why the trojan's
  cyclone-speed persists indefinitely while farming — and why a stall (no
  kills) is what actually risks losing it. (Directly relevant: the melee
  freeze-near-players and deadlock bugs mattered because a long enough stall
  could drop Cyclone.)

### Mana ✅ (partial)
- **Tao classes (Water/Fire) have magic skills that consume mana**; mana
  depletes as magic skills are cast. (Melee/archer basic attacks are not
  magic — mana matters mainly for the Tao casters.)
❓ Still open:
- Full skill list per class (heal/buff/summon skills the bot should cast or
  watch)? Mana-management thresholds for a Tao bot?
- What breaks an active XP skill besides running out — death, zoning,
  disconnect?

---

## Items

### Quality tiers ✅
Encoded as the **last digit of the type ID**:
- `3` = Normal, `6` = Refined, `7` = Unique, `8` = Elite, `9` = Super.
- (`0`/other digits also appear — see BroadSword type 410050 ending in 0.)
❓ What do the non-{3,6,7,8,9} last digits mean? Is quality strictly these 5
tiers, and does a higher tier mean strictly better stats?

### Plus / enchant level ✅
- `+0` to `+12` enchant level. Bag: `CItem::m_nAddition` (+0x6B). Ground:
  `MapItemInfo+0x48`.
- **Only +1 drops from monsters**; higher plus comes only from player
  crafting/enhancement (Artisan). ✅
❓ What does each plus level actually give (stat scaling)? Is +12 the real
cap? How much better is a +1 vs +0 in practice — is it worth much?

### Sockets / gems ✅ (partial)
- Items have up to 2 sockets (`m_nGem1` +0x67, `m_nGem2` +0x68). Encoding:
  tens digit = gem class, units = level. 0 = no socket, 255 = empty socket.
- Gem classes seen in code: Phoenix, Dragon, Fury, Rainbow, Kylin, Violet,
  Moon, Tortoise, Thunder, Glory. Levels: Normal/Refined/Super.
❓ What do gems do, which are valuable, and should the bot care about them
when deciding what to loot/keep?

### Blessing / luck ✅ (field known, meaning not)
- `m_nBless` (+0x6A), value 0-7.
❓ What is "bless"? What does it do and does it matter for loot decisions?

### Durability ✅
- Raw value at `m_nAmount`/`m_nAmountLimit`; display = raw/100.
- Repairing restores to max. A **failed Artisan upgrade halves durability**.
❓ Does durability drop from normal combat/use, and does an item break at 0?
How does repair cost scale?

---

## Artisan Wind upgrade ✅
- NPC **"Artisan Wind"** in **Twin City**. Give it 1 full-durability
  equipment piece + 1 Meteor → a chance to upgrade.
- **Success: the item's TYPE ID changes to a different, higher item**
  (e.g. AmethystBlade→MoonBlade→ColdBlade→Ataghan), durability stays full,
  can immediately try again. **Failure: durability is halved**, must repair
  before retrying. ✅
- A **repair NPC sits directly next to Artisan Wind** (no travel needed). ✅
- Materials: **Meteor**, **MeteorScroll**, **DragonBall** (DragonBall uses a
  different action code — "upgrade quality" vs "improve"). ✅
❓ What determines upgrade success chance? Does it depend on the item's
current level, quality, or the material used? Is there a max level an item
can reach this way? What's the difference between upgrading with a Meteor
vs MeteorScroll vs DragonBall (quality vs plus vs something else)?

---

## Economy / valuables ✅
Money tiers (ascending), from code: **Silver, Sycee, Gold, GoldBullion,
GoldBar, GoldBars.**
- **Meteor** (type 1088001), **MeteorTear** (1088002), **DragonBall**
  (1088000) — the rare valuables the bot prioritizes.
- **MeteorScroll** (720027), **DBScroll** (720028), **MegaMeteorScroll**
  (720029) — meteors/DBs pack into scrolls (compact storage).

**Approximate values (user, 2026-08-30):**
| Item | Value |
|---|---|
| **DragonBall** | **~3,000,000 gold** — the single most valuable drop |
| **MeteorScroll** (= 10 Meteors) | ~700,000 gold |
| **Meteor** (1/10 of a scroll) | ~70,000 gold |
| **+1 item** | ~500,000 gold each |

**Bot-relevant takeaways:**
- Value ranking: **DragonBall (3M) ≫ +1 item (500k) > Meteor (70k)**.
- A **+1 item is worth ~7 Meteors** — which economically justifies giving
  wanted +1/quality gear the same stop-and-grab urgency as meteors (this is
  what item 49 in project-status implemented). A +1 is NOT a minor pickup.
- **Packing**: 10 Meteors → 1 MeteorScroll (compact storage for the
  warehouse/bank; the bot's "Packing Meteors into MeteorScrolls" step).

---

## Combat & PK ✅ (PK) / ❓ (damage formula)

### PK / PVP system ✅ — **critical for Safety & Paranoia logic**
- **Where:** PK can happen in **any city, by any player — EXCEPT Market**,
  which is a **safe (no-PK) zone**. (This is why the bot's Safety Rest
  retreats to Market — it's the one place a player can't be attacked.)
- **Guards:** major cities have **strong guard NPCs**. If someone PKs, the
  guard attacks and kills them — unless the PKer is very, very strong.
- **How to PK:** a player must (1) toggle a PK-mode setting ("I want to kill
  others"), then (2) use an attack skill on the target. So a random nearby
  player is not an immediate threat unless they've flagged and attack.
- **On death:** the victim **drops everything in their bag** (gold + items).
  **Equipped gear is only at risk if the victim is red/black name** — see
  below.
- **PK points (PKP) and name color** (the Entities-panel "PK?" column):
  - Kill a (non-red/black) player → **+5 PKP**, and your **name flashes blue
    for a couple minutes**. While blue-flashing you can be attacked by anyone
    **without them needing PK mode on**.
  - **30 PKP → red name.** A red-name player who dies **has a chance to drop
    equipped gear**.
  - **100 PKP → black name.** A black-name player has a **much higher chance
    to drop equipped gear** on death.
  - **Killing a red or black name player grants NO PK points** (they're fair
    game / "justified" kills).
- **Bot implications:**
  - The bot NEVER PKs — so it never gains PKP, never turns blue/red/black,
    and its equipped gear is safe on death. It only risks its **bag
    contents** (gold + un-stored loot) if killed.
  - Storing loot (warehouse/bank) between farm runs limits what's lost if
    the bot is ever PK'd.
  - **IMPORTANT — Paranoia is NOT about avoiding PK death.** See the
    Paranoia intent below: the goal is not being *seen* by ANY player
    (recording/report risk), so evasion must react to **every** player, not
    just red/black PKers. An earlier note here suggested weighting evasion
    toward red/black names — that is WRONG for this bot's actual goal and was
    removed.

## Paranoia mode — intent (bot design, per user 2026-08-30) ⚠️ behavior gap
The point of Paranoia is **stealth / anti-detection**, NOT survival: other
players could **record or report** a bot, so the bot should stay **invisible
to all of them**. Intended behavior when a player is detected:
1. **Detected in range** → flee/evade away from the player.
2. **Once out of the player's sight** → speed-hack away to a **different
   area** of the map entirely.
3. **If the player was just passing by** (they leave) → come back to the
   original hunting spot.
4. **MapWide** is the preferred zone mode precisely because it lets the bot
   relocate to a fresh hunting location anywhere on the map when evading.
**Current behavior does NOT fully match this** (user: "not quite where I
want it yet"). Today evasion picks the whole-map-farthest-from-threat tile
and paths there; there's no explicit "out of sight → speed-hack relocate"
phase, no "return if they were just passing" logic, and the relocate target
isn't necessarily a good (heatmap-hot) hunting spot. Likely lives in the
explore-mode / heatmap-bucket path (FindZoneExplorePosition,
base_hunt_plugin.cpp). **This is an open design item to revisit.**

### Damage / accuracy ❓
- Attack has an **accuracy element — melee misses sometimes** (not 100%).
  Item stats: attackMin/Max, defense, dexterity, dodge.
- ❓ What governs hit vs miss (dexterity vs dodge)? How do attack/defense/
  dodge resolve damage? How often does melee land on a same-level monster?

---

## Monsters ✅ (name colors) / ❓ (abilities, drop tables)
Monster **name color = danger relative to the character's level** (the bot's
`GetDangerTier`/`maxDangerTier` Level-mode gate uses this). Monster entity
IDs fall in 400,000–499,999.
- **Green = easy** (below the character's level)
- **White = my level** (even)
- **Red = hard** (above)
- **Black = very hard** (well above)

❓ Still open:
- Do monsters have special abilities to avoid (ranged, AoE, debuffs)?
- Do certain monsters drop meteors/DBs/+1 gear more than others — is there a
  "best farm target" for valuables specifically?

---

## Leveling / progression ✅ (partial)
- **Max level: 130.** At **level 110**, a character can **reborn into
  another class** (see Classes).
- The bot has a Farm mode (ignore level) and a Level mode (respect danger
  tier).
- **Death does NOT cost XP** — the only death penalty is dropping bag
  contents (and equipped gear if red/black name). See PK section.
❓ Still open:
- How does XP scale — is there a best level range to farm?
- Does reborn reset level/stats, and what does it grant?

---

## Maps & locations ✅ (partial)
- **Twin City** — has Artisan Wind + adjacent repair NPC. ✅
- **Market** map — the **no-PK safe zone** (the only city a player can't be
  attacked in). Has a **Pharmacist** (the bot's general gear-repair NPC),
  Warehouse, banks (Treasure Bank, Compose Bank). This is why Safety Rest
  retreats here. ✅
- Hunt happens on open maps (e.g. map 1002 "newplain", 972×972). ✅
- Travel via **city gate items** (TwinCity/Desert/Ape/Castle/BirdIsland/
  StoneCity gates — type 1060020+). ✅
❓ Questions:
- Which **maps/areas are best for farming** at various levels?
- How does **teleport/travel** work — gate items, portals, teleporter NPCs?
- Are there **safe zones** (no PK) vs open-PK areas?

---

## Death & recovery ✅ (partial)
- **On death, the character drops everything in its bag** (gold + un-stored
  items). **No XP loss.** Equipped gear is safe unless the character is
  red/black name (the bot never PKs, so its gear is always safe). See PK.
- **Revive:** the character **revives in the city/town it's currently in**.
❓ Still open:
- Is revive automatic or does it need a click/confirm (what the "Revive
  Helper" automates)? Any revive cost/cooldown?
- If death drops the bag, does the bot need to rush back to recover drops,
  or are they lost? (Argues for frequent warehouse/bank storage runs so
  little is in the bag when killed.)

---

## Disconnect / reconnect ❓ (open bug relevance)
The launcher auto-reconnects after a disconnect, but in-place reconnect
reliably FAILS and falls back to a full relaunch (open issue). To fix it,
the exact reconnect-screen flow matters.
❓ Questions:
- When you **disconnect** (network drop / idle timeout), what exactly
  appears on screen? Is there a **"Connection interrupted, please re-login"
  banner/popup**, and does it need clicking or a keypress to dismiss?
- Is the reconnect login screen **identical to a fresh launch**, or
  different (fields pre-filled, extra dialog, different focus)?
- When you reconnect **manually**, what's the exact click/type sequence you
  do?
