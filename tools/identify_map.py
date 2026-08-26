"""Identify which map the hero is standing in, from a walk trace.

Map-name tables disagree with each other (our data_maps.json calls 1020
"Bird Island" while the client's own GameMap.json maps 1020 to canyon.DMap,
and a 2008 reference lists BirdIsland as 1015). So instead of trusting any
name table, this matches ground truth: every tile the hero physically stood on
must be walkable in the real map, so scoring the trace against the walkability
of every .DMap on disk identifies the map empirically.

usage: python identify_map.py [walktrace.json]
"""
import json, struct, os, sys

GAME = r"F:\Games\Classic Conquer 2.0"
TRACE = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\Public\coclassic_walktrace.json"

trace = json.load(open(TRACE))
tiles = [tuple(t) for t in trace["tiles"]]
print(f"walk trace: {len(tiles)} tiles, hero now at {trace.get('hero_now')}")

gm = json.load(open(os.path.join(GAME, "ini", "GameMap.json"), encoding="utf-8", errors="replace"))

# Optional friendly names, for display only — never for matching.
names = {}
for cand in ("data_maps.json", os.path.join("CoClassicBot", "data_maps.json")):
    p = os.path.join(r"C:\Users\TerryGluff\Documents\Claude\CO99", cand)
    if os.path.exists(p):
        d = json.load(open(p))
        names = d if isinstance(d, dict) else {str(x.get("id")): x.get("name") for x in d}
        break


def load_masks(path):
    """Return (width, height, mask_lookup) for a .DMap, or None."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return None
    if len(data) < 0x114:
        return None
    w, h = struct.unpack_from("<II", data, 0x10C)
    if not (0 < w <= 4096 and 0 < h <= 4096):
        return None
    rowbytes = w * 6 + 4
    if 0x114 + h * rowbytes > len(data):
        return None
    return w, h, data, rowbytes


def mask_at(data, rowbytes, x, y):
    off = 0x114 + y * rowbytes + x * 6
    return struct.unpack_from("<H", data, off)[0]


results = []
for e in gm:
    did, fn = e["DocumentId"], e["FileName"]
    full = os.path.join(GAME, fn.replace("/", os.sep))
    got = load_masks(full)
    if not got:
        continue
    w, h, data, rowbytes = got

    inb = walk = 0
    for (x, y) in tiles:
        if 0 <= x < w and 0 <= y < h:
            inb += 1
            if mask_at(data, rowbytes, x, y) == 0:   # mask 0 == walkable
                walk += 1
    if inb != len(tiles):
        continue                                     # some tile out of bounds: not this map
    frac = walk / inb if inb else 0.0
    results.append((frac, did, os.path.basename(fn), w, h))

results.sort(reverse=True)
print(f"\n{len(results)} map(s) contain every traced tile in-bounds\n")
print(f"{'walkable':>9}  {'id':>6}  {'file':<26} {'dims'}")
for frac, did, fn, w, h in results[:15]:
    nm = names.get(str(did), "")
    flag = "  <== MATCH" if frac >= 0.99 else ""
    print(f"{frac*100:8.2f}%  {did:>6}  {fn:<26} {w}x{h}  {nm}{flag}")

perfect = [r for r in results if r[0] >= 0.99]
print()
if len(perfect) == 1:
    print(f"IDENTIFIED: map id {perfect[0][1]} ({perfect[0][2]})")
elif not perfect:
    print("No map has all traced tiles walkable — check the trace spans a single map.")
else:
    print(f"{len(perfect)} maps still tie; walk a longer/more varied route to separate them.")
