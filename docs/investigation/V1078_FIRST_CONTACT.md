# v1078 first contact (2026-09-21, read-only image checkpoint)

Capture: `imgdump.dll` loaded into the live client by the user (System Informer), generic mode, image-only checkpoint
`C:\Users\Public\coclassic_capture\session_20260921_073000\cp01_first_contact` (client state at capture unknown). Whole 44 MB
image read, 0 bytes skipped, 0.7 s, client stayed responsive. Identity: PE stamp 0x6AB0822B, SizeOfImage 0x2A26000.

## Verdict: regime A (same-toolchain recompile) - with real, specific changes
`tools/regime_classify.py` (proxy calibration: A 90% / B 19% exact32; v1074-vs-itself ceiling 84%):

| measure | v1078 vs v1074 |
|---|---|
| exact32 (masked-byte prefix) of sampled distinctive functions | **74.4%** (ceiling 84%) |
| any tier | 85% (ceiling 92%) |
| order-inversion rate among relocated | 1.5% (A proxy 0.4%, B proxy 9.9%) |
| functions detected | 16,080 vs 15,742 (102%: the sweep is fine, no alignment problem) |
| ciphertext-looking code pages | 47/1387 = 3.4% (v1074: 46/1360 = 3.4%): the code image is already ~fully decrypted |
| section table | identical layout; `.text` 0x5501DF -> 0x56B5DF (+2%); every section grows slightly |

Not a "codegen changed" build. Bytes moved (functions shifted from about +0x1800 to +0x17B90 in growing steps) but the bulk is intact.

Method note: the first classifier run also swept Themida's own `.themida`/`.boot` sections (21 MB of runtime, absent from the
v1074 dump) which took >10 min and would have fired the ciphertext guard falsely. `sigkit.Image` now excludes packer sections.

## Verified code entries (all "candidates" until confirmed live)
Exact-matched with smoothly increasing deltas (a strong consistency signal): `HERO_ROLEMGR_ACCESSOR_181B30` +0x8D50,
`CNETCLIENT_BEGIN_MSG` +0x92D0, `CROLE_SET_COMMAND_REAL` +0xBE80, `CNETCLIENT_SEND_MAPITEM_MSG` +0xE690,
`CNETCLIENT_COMPUTE_MAPITEM_SIZE` +0x122E0, `COROUTINE_BF920` +0x1820; shape-exact `CNETCLIENT_COMMIT_STAGING` +0x17B90;
`CNETCLIENT_CONNECTION_SINGLETON` shape-fuzzy +0x1BE0 (accessor template, medium).
**Changed / not relocatable:** `CNETCLIENT_SEND_MSG_REAL` (0x1DD450) and its neighbour `CNETCLIENT_POLLER_1DD860` - the send path
was actually modified in this update. sigscan's shape match for SEND_MSG_REAL (0x1C70C0, delta -0x16390) is an **order
violation** against its exact-matched neighbours and is rejected (new `sigscan` ordering check). `COROUTINE_E9960` not found.
Consequence: anything that sends packets (pickup, movement commands via SEND_MSG_REAL) needs live re-derivation on v1078.

## Globals (single anchor each = candidates; deltas agree within 0x128, consistent with one shifted .data)
ROLE_MGR_PTR 0x6BCEF0 (+0x207C0), CURRENT_MAP_ID 0x6B9C30 (+0x206D0), MAP_ITEM_VEC 0x6B9CB8 (+0x207E0), MAP_SCENE_PTR 0x6B9B50,
CONN_STATIC 0x6BACE0, MSGMAPITEM_VTABLE 0x5EE890, CGAMEMAP_VTABLE 0x5E8348 (2 sites agree). All to be checked offline against a
`full` checkpoint (read the pointer from the image, follow it into the heap pack).

## Struct layout CHANGED (the important find)
Field re-finding on v1078 (votes across access sites; asserted = has a static_assert in our headers):
* CRole up to `m_nMaxHp` 0x3D0: **unchanged** (id 0x68, roleset deq 0x70, name 0x94, posMap/World/Scr 0xD8/E0/E8, cmdAction 0x188).
* CRole 0x6E0..0x71C (stamina/level/syndicate): mixed votes, mostly +0x10 (level 0x6E8 -> 0x6F8, syndicate 0x714 -> 0x724): noisy.
* CHero: `m_pStatTable` 0x968 votes 2 for **0x968** vs 1 for 0x978 (ambiguous!) - the HP path lives here, so HP needs live re-verification.
  From ~0xA80 on everything shifts **+0x28**: qwRuntime 0xA80->0xAA8, deqItem 0xB70->0xB98, equipment 0xBD8->0xC00, maxMana 0xCF8->0xD20,
  vecMagic 0x1968->0x1990; and **+0x48** by 0x3774: idActiveNpc -> 0x37BC, bVip 0x3790 -> 0x37D8.
* Item / map / magic / entity-set structs: unchanged.
Two insertions in CHero (about +0x28 between 0x968 and 0xA80, another +0x20 between 0x1980 and 0x3774), and something inside CRole
near 0x6E0. None of this can be trusted for the bot without a live check; name at CRole+0x94 (unchanged, 5/5 votes) is the anchor
for finding the hero object in a heap pack, from which every field can be confirmed by value.

## Next
`full` checkpoints in-world with on-screen values noted (see V1078_CAPTURE_RUNBOOK.md); then locate the hero by name, confirm
m_pStatTable / HP / level / silver, verify the global pointers, and re-derive the send path.
