"""
Full pre/post reachability diff for the rail-blocking change.

PRE  = current code:  base grid + overlay-walkable cells opened.
POST = proposed:      same, then overlay rail cells (mask==1) block base-walkable
                      tiles, EXCEPT on tiles some overlay part opened (walkable
                      wins, order-independent: pass 1 collect opened, pass 2 block).

Movement model = CGameMap::FindPath: 8-neighbour, no corner-cutting rule,
|altitude delta| <= 200 per step.

For every map with any scene overlay this reports:
  flipped      tiles walkable in PRE but blocked in POST
  splits       PRE components whose surviving (non-flipped) tiles no longer stay
               in ONE POST component  -> a region got cut off
  erased       PRE components entirely consumed by flipped tiles
  anchor       reachable-area count from a fixed anchor (largest PRE component's
               first surviving tile) PRE vs POST
  portals      every file portal tile: walkable + same component relation
"""
import struct, sys, os, glob, array, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rail_scan as rs

def load_map(path):
    b = open(path, "rb").read()
    if len(b) < 0x114: return None
    W, H = struct.unpack_from("<II", b, 0x10C)
    if not (0 < W <= 4096 and 0 < H <= 4096): return None
    rb = W*6 + 4
    tail = 0x114 + H*rb
    if len(b) < tail + 8: return None
    mask = bytearray(W*H); alt = array.array("h", [0])*(W*H)
    for y in range(H):
        ro = 0x114 + y*rb
        for x in range(W):
            m, t, a = struct.unpack_from("<HHh", b, ro + x*6)
            mask[y*W+x] = 1 if m == 1 else (m if m < 2 else 1)
            alt[y*W+x] = a
    pc = struct.unpack_from("<I", b, tail)[0]
    off = tail + 4
    portals = []
    for _ in range(pc):
        px, py, pid = struct.unpack_from("<iii", b, off); off += 12
        portals.append((px, py, pid))
    lc = struct.unpack_from("<I", b, off)[0]; off += 4
    scenes = []
    for _ in range(lc):
        if off + 4 > len(b): break
        t = struct.unpack_from("<I", b, off)[0]
        if t not in rs.sizes: break
        if t == 1:
            f = b[off+4:off+264].split(b"\0")[0].decode("latin1")
            x, y = struct.unpack_from("<ii", b, off + 264)
            scenes.append((f, x, y))
        off += rs.sizes[t]
    return W, H, mask, alt, portals, scenes

def merge(W, H, base, scenes):
    pre = bytearray(base); post = bytearray(base)
    opened = bytearray(W*H)
    cells_list = []
    for f, sx, sy in scenes:
        for pw, ph, pdx, pdy, cells in (rs.load_scene(f) or []):
            bx = sx + pdx - pw + 1; by = sy + pdy - ph + 1
            cells_list.append((bx, by, pw, ph, cells))
    # pass 1: walkable-opens (both PRE and POST)
    for bx, by, pw, ph, cells in cells_list:
        for j in range(ph):
            ty = by + j
            if not (0 <= ty < H): continue
            for i in range(pw):
                tx = bx + i
                if not (0 <= tx < W): continue
                if cells[j*pw+i] == 0:
                    pre[ty*W+tx] = 0; post[ty*W+tx] = 0; opened[ty*W+tx] = 1
    # pass 2: rails block base-walkable tiles that no part opened
    blocked = 0
    for bx, by, pw, ph, cells in cells_list:
        for j in range(ph):
            ty = by + j
            if not (0 <= ty < H): continue
            for i in range(pw):
                tx = bx + i
                if not (0 <= tx < W): continue
                if cells[j*pw+i] != 0 and not opened[ty*W+tx] and post[ty*W+tx] == 0:
                    post[ty*W+tx] = 1; blocked += 1
    return pre, post, blocked

def label(W, H, mask, alt):
    lab = array.array("i", [0])*(W*H)
    sizes = [0]
    n = 0
    for start in range(W*H):
        if mask[start] != 0 or lab[start]: continue
        n += 1; lab[start] = n; cnt = 1
        stack = [start]
        while stack:
            c = stack.pop()
            cy, cx = divmod(c, W); ca = alt[c]
            for dy in (-1, 0, 1):
                ny = cy + dy
                if ny < 0 or ny >= H: continue
                for dx in (-1, 0, 1):
                    if not (dx or dy): continue
                    nx = cx + dx
                    if nx < 0 or nx >= W: continue
                    nc = ny*W + nx
                    if mask[nc] != 0 or lab[nc]: continue
                    if abs(alt[nc] - ca) > 200: continue
                    lab[nc] = n; cnt += 1; stack.append(nc)
        sizes.append(cnt)
    return lab, sizes

def diff_map(path, verbose=True):
    name = os.path.basename(path)[:-5]
    m = load_map(path)
    if not m: return None
    W, H, base, alt, portals, scenes = m
    if not scenes: return None
    pre, post, nblocked = merge(W, H, base, scenes)
    flipped = [i for i in range(W*H) if pre[i] == 0 and post[i] == 1]
    if not flipped:
        return dict(name=name, flipped=0)
    labpre, szpre = label(W, H, pre, alt)
    labpost, szpost = label(W, H, post, alt)
    # per PRE component: where do the surviving tiles land in POST?
    land = collections.defaultdict(collections.Counter)
    erased_tiles = collections.Counter()
    for i in range(W*H):
        lp = labpre[i]
        if not lp: continue
        if post[i] == 0: land[lp][labpost[i]] += 1
        else: erased_tiles[lp] += 1
    splits = []
    erased = []
    for lp, c in land.items():
        if len(c) > 1:
            parts = sorted(c.values(), reverse=True)
            splits.append((lp, szpre[lp], parts))
    for lp in erased_tiles:
        if lp not in land: erased.append((lp, szpre[lp]))
    # fixed anchor: first surviving tile of the largest PRE component
    big = max(range(1, len(szpre)), key=lambda k: szpre[k])
    anchor = next(i for i in range(W*H) if labpre[i] == big and post[i] == 0)
    pre_reach = szpre[big]
    post_reach = szpost[labpost[anchor]]
    flipped_in_big = sum(1 for i in flipped if labpre[i] == big)
    # portals
    pnotes = []
    for px, py, pid in portals:
        if 0 <= px < W and 0 <= py < H:
            i = py*W+px
            pnotes.append((pid, (px, py), pre[i] == 0, post[i] == 0))
    res = dict(name=name, W=W, H=H, flipped=len(flipped), nblocked_writes=nblocked,
               splits=splits, erased=erased, pre_reach=pre_reach, post_reach=post_reach,
               flipped_in_big=flipped_in_big, portals=pnotes, flipped_tiles=[(i % W, i // W) for i in flipped])
    return res

if __name__ == "__main__":
    names = sys.argv[1:]
    paths = [rs.ROOT + rf"\map\map\{n}.DMap" for n in names] if names else sorted(glob.glob(rs.ROOT + r"\map\map\*.DMap"))
    for p in paths:
        r = diff_map(p)
        if r is None: continue
        if r["flipped"] == 0:
            print(f"{r['name']:16s} flipped=0  (no rail-over-walkable) -> unchanged"); continue
        print(f"{r['name']:16s} {r['W']}x{r['H']} flipped={r['flipped']:5d}  splits={len(r['splits'])}  erased_components={len(r['erased'])} "
              f"(sizes {[s for _, s in r['erased']][:6]})  anchor-reach PRE={r['pre_reach']} POST={r['post_reach']} "
              f"(flipped inside it: {r['flipped_in_big']}, delta={r['pre_reach']-r['flipped_in_big']-r['post_reach']})")
        for lp, size, parts in r["splits"][:6]:
            print(f"     SPLIT component#{lp} size={size}: surviving pieces {parts[:6]}")
        bad = [p for p in r["portals"] if p[2] and not p[3]]
        if bad: print("     PORTALS newly blocked:", bad)
        if r["portals"]:
            print("     portals walkable pre/post:", [(pid, w0, w1) for pid, _, w0, w1 in r["portals"]])
