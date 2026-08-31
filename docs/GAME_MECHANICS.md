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

## Character classes / professions ❓
Known so far: the user runs a **Trojan** (melee) and an **Archer**.
- **Trojan** — melee. Uses Cyclone + Superman skills (see Skills). ✅
- **Archer** — ranged. Uses Scatter (AoE cone) + Fly. ✅
- ❓ What is the **full list of classes/professions** in this game, and what
  fundamentally defines each (weapon types, role, playstyle)?
- ❓ Is "profession" a fixed choice at creation, or can it change (rebirth/
  reincarnation)? The code has a `requiredProfession` field on items and an
  `AutoHuntGoal::Level` "danger tier" system — how does class tie into what
  a character can equip/hunt?

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

❓ Questions:
- What is the **complete skill list** per class that matters for botting
  (attack skills, buffs, movement)? What does each do, and how is it
  triggered (auto, hotkey, passive)?
- **Cyclone** specifics: exactly how is it gained and lost — X kills within
  Y seconds? Does it have a level/duration? What breaks it besides stopping
  kills (death, zoning, disconnect)?
- **Superman/Snow**: how are they gained/maintained? Duration?
- Are there **healing / buff / summon** skills the bot should cast or watch?
- **Mana**: do skills cost mana? Does the bot need to watch/manage it?

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

## Economy / valuables ❓
Money tiers (ascending), confirmed from code: **Silver, Sycee, Gold,
GoldBullion, GoldBar, GoldBars.** ✅
- **Meteor** (type 1088001), **MeteorTear** (1088002), **DragonBall**
  (1088000) — the "rare/valuable" items the bot prioritizes. ✅
- **MeteorScroll** (720027), **DBScroll** (720028), **MegaMeteorScroll**
  (720029) — meteors/DBs can be packed into scrolls (compact storage). ✅
❓ Questions:
- Roughly what is the **relative value** of these? (1 DragonBall = how many
  Meteors? What's a Meteor worth in gold?)
- What's the **most valuable** thing that realistically drops while hunting?
- Are +1 items / high-quality gear actually worth much on the market, or is
  the real money in meteors/DBs/gold?
- How does **packing** work (N meteors → 1 scroll)? Why do it — storage
  limits, trade convenience, safety?

---

## Combat ❓
Known: attack has an **accuracy element — melee misses sometimes** (not
100% hit). ✅ Item stats include attackMin/Max, defense, dexterity, dodge.
❓ Questions:
- What governs **hit vs miss** (dexterity vs dodge)? How often does a melee
  hit land against a same-level monster?
- How do **attack/defense/dodge** actually resolve damage?
- **PK/PVP**: can other players attack the bot while hunting? What are the
  PK rules (flagging, penalties, safe zones)? This matters a lot for the
  Paranoia/Safety evasion logic.
- Is there a **"blue name / red name" PK status** system? (The Entities
  panel shows a "PK?" column.)

---

## Monsters ❓
Known: monster **name color reflects danger tier relative to the character's
level** (the bot's `GetDangerTier` / `maxDangerTier` uses this). Monster
entity IDs fall in 400,000–499,999. ✅
❓ Questions:
- What does each **name color** mean exactly (e.g., green = trivial, white =
  even, red = dangerous)? The bot's Level-mode gates on this.
- Do monsters have special abilities the bot should avoid (ranged, AoE,
  debuffs)?
- Do certain monsters drop meteors/DBs/+1 gear more than others — is there a
  "best farm target"?

---

## Leveling / progression ❓
- The bot has a Farm mode (ignore level) and a Level mode (respect danger
  tier). ✅
❓ Questions:
- What's the **level cap**? How does XP scale — is there a best level range
  to farm?
- Is there **rebirth / reincarnation / class change** at a level threshold,
  and does it reset anything?
- Does dying **lose XP or drop items**? (Critical for how cautious the bot
  should be.)

---

## Maps & locations ✅ (partial)
- **Twin City** — has Artisan Wind + adjacent repair NPC. ✅
- **Market** map — has a **Pharmacist** (the bot's general gear-repair NPC),
  Warehouse, banks (Treasure Bank, Compose Bank). ✅
- Hunt happens on open maps (e.g. map 1002 "newplain", 972×972). ✅
- Travel via **city gate items** (TwinCity/Desert/Ape/Castle/BirdIsland/
  StoneCity gates — type 1060020+). ✅
❓ Questions:
- Which **maps/areas are best for farming** at various levels?
- How does **teleport/travel** work — gate items, portals, teleporter NPCs?
- Are there **safe zones** (no PK) vs open-PK areas?

---

## Death & recovery ❓
❓ Questions:
- What happens when the character **dies** — respawn where, lose what
  (XP, items, durability)?
- How does **revival** work (auto-revive, revive at a location, revive
  item/skill)?
- The bot has a "Revive Helper" and auto-revive logic — what's the actual
  in-game revive flow it's automating?

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
