import struct, os, glob, sys
ROOT = r"F:\Games\Classic Conquer 2.0"
sizes = {1: 272, 4: 420, 10: 76, 15: 280}
_scene_cache = {}

def load_scene(rel):
    if rel in _scene_cache: return _scene_cache[rel]
    try:
        d = open(ROOT + "\\" + rel.replace("/", "\\"), "rb").read()
    except OSError:
        _scene_cache[rel] = None
        return None
    n = struct.unpack_from("<I", d, 0)[0]
    o = 4; parts = []
    for p in range(n):
        if o + 0x164 > len(d): break
        pw, ph = struct.unpack_from("<ii", d, o + 0x14C)
        pdx, pdy = struct.unpack_from("<ii", d, o + 0x158)
        if pw <= 0 or ph <= 0 or pw > 64 or ph > 64: break
        need = o + 0x164 + pw*ph*12
        if need > len(d): break
        cells = [struct.unpack_from("<i", d, o + 0x164 + k*12)[0] for k in range(pw*ph)]
        parts.append((pw, ph, pdx, pdy, cells))
        o = need
    _scene_cache[rel] = parts
    return parts

def analyse(path):
    b = open(path, "rb").read()
    if len(b) < 0x114: return None
    W, H = struct.unpack_from("<II", b, 0x10C)
    if W == 0 or H == 0 or W > 4096 or H > 4096: return None
    rb = W*6 + 4
    tail = 0x114 + H*rb
    if len(b) < tail + 8: return None
    pc = struct.unpack_from("<I", b, tail)[0]
    off = tail + 4 + pc*12
    if off + 4 > len(b): return None
    lc = struct.unpack_from("<I", b, off)[0]; off += 4
    scenes = []
    for i in range(lc):
        if off + 4 > len(b): break
        t = struct.unpack_from("<I", b, off)[0]
        if t not in sizes: break
        if t == 1:
            f = b[off+4:off+264].split(b"\0")[0].decode("latin1")
            x, y = struct.unpack_from("<ii", b, off + 264)
            scenes.append((f, x, y))
        off += sizes[t]
    def base_mask(x, y):
        return struct.unpack_from("<H", b, 0x114 + y*rb + x*6)[0]
    opened = railwalk = railblock = 0
    xs = []
    for f, sx, sy in scenes:
        parts = load_scene(f)
        if not parts: continue
        for pw, ph, pdx, pdy, cells in parts:
            bx = sx + pdx - pw + 1; by = sy + pdy - ph + 1
            for j in range(ph):
                for i in range(pw):
                    tx, ty = bx+i, by+j
                    if tx < 0 or ty < 0 or tx >= W or ty >= H: continue
                    m = cells[j*pw+i]; bm = base_mask(tx, ty)
                    if m == 0 and bm == 1: opened += 1
                    elif m == 1 and bm == 0:
                        railwalk += 1; xs.append((tx, ty, f))
                    elif m == 1 and bm == 1: railblock += 1
    return dict(W=W, H=H, scenes=len(scenes), opened=opened, rail_over_walkable=railwalk, rail_over_blocked=railblock, xs=xs)

if __name__ == "__main__":
    for p in sorted(glob.glob(ROOT + r"\map\map\*.DMap")):
        r = analyse(p)
        if r and r["scenes"]:
            print(f"{os.path.basename(p):28s} {r['W']}x{r['H']} scenes={r['scenes']:3d} opened={r['opened']:5d} rail_over_walkable={r['rail_over_walkable']:5d} rail_over_blocked={r['rail_over_blocked']:5d}")
