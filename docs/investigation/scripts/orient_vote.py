"""
Decide the part-cell orientation (identity / flipX / flipY / flipBoth) offline.

Idea: scene objects were authored on top of a base terrain that already knew
about the ground. For a correctly-oriented part, cells the object blocks tend to
coincide with base-blocked tiles (a house, tree, wall, cliff-bridge edge sits on
what the base already calls blocked) far more often than under a wrong
orientation. We only score PARTS THAT ARE ASYMMETRIC (a symmetric part gives the
same answer under every orientation and would just dilute the vote).
"""
import struct, glob, os, sys
sys.path.insert(0, os.path.dirname(__file__))
import rail_scan as rs

ORIENTS = {"identity": (0, 0), "flipX": (1, 0), "flipY": (0, 1), "flipBoth": (1, 1)}

def parts_asymmetric(pw, ph, cells):
    def cell(i, j): return cells[j*pw+i]
    outs = set()
    for fx, fy in ORIENTS.values():
        outs.add(tuple(cell(pw-1-i if fx else i, ph-1-j if fy else j) for j in range(ph) for i in range(pw)))
    return len(outs) > 1

def run(paths, label):
    tally = {k: dict(block_on_block=0, block_on_walk=0, walk_on_block=0, walk_on_walk=0) for k in ORIENTS}
    nparts = 0
    for p in paths:
        b = open(p, "rb").read()
        if len(b) < 0x114: continue
        W, H = struct.unpack_from("<II", b, 0x10C)
        if not (0 < W <= 4096 and 0 < H <= 4096): continue
        rb = W*6 + 4
        tail = 0x114 + H*rb
        if len(b) < tail + 8: continue
        pc = struct.unpack_from("<I", b, tail)[0]
        off = tail + 4 + pc*12
        if off + 4 > len(b): continue
        lc = struct.unpack_from("<I", b, off)[0]; off += 4
        scenes = []
        for _ in range(lc):
            if off + 4 > len(b): break
            t = struct.unpack_from("<I", b, off)[0]
            if t not in rs.sizes: break
            if t == 1:
                f = b[off+4:off+264].split(b"\0")[0].decode("latin1")
                x, y = struct.unpack_from("<ii", b, off+264)
                scenes.append((f, x, y))
            off += rs.sizes[t]
        for f, sx, sy in scenes:
            parts = rs.load_scene(f)
            if not parts: continue
            for pw, ph, pdx, pdy, cells in parts:
                if not parts_asymmetric(pw, ph, cells): continue
                nparts += 1
                bx = sx + pdx - pw + 1; by = sy + pdy - ph + 1
                for name, (fx, fy) in ORIENTS.items():
                    for j in range(ph):
                        for i in range(pw):
                            tx, ty = bx+i, by+j
                            if not (0 <= tx < W and 0 <= ty < H): continue
                            m = cells[(ph-1-j if fy else j)*pw + (pw-1-i if fx else i)]
                            bm = struct.unpack_from("<H", b, 0x114 + ty*rb + tx*6)[0]
                            key = ("block" if m else "walk") + "_on_" + ("block" if bm else "walk")
                            tally[name][key] += 1
    print(f"== {label}: {nparts} asymmetric parts")
    for name, t in tally.items():
        blocked = t["block_on_block"] + t["block_on_walk"]
        walked = t["walk_on_block"] + t["walk_on_walk"]
        agree = t["block_on_block"] + t["walk_on_walk"]
        tot = blocked + walked
        print(f"  {name:9s} agree={agree:7d}/{tot:7d} ({100.0*agree/max(tot,1):5.2f}%)  "
              f"block_on_block={t['block_on_block']:6d} block_on_walk={t['block_on_walk']:6d} "
              f"walk_on_block(opened)={t['walk_on_block']:6d} walk_on_walk={t['walk_on_walk']:6d}")

if __name__ == "__main__":
    allmaps = sorted(glob.glob(rs.ROOT + r"\map\map\*.DMap"))
    run(allmaps, "ALL maps")
    for n in ("newplain", "newbie", "sky", "star", "task07", "2009-7x", "pk", "bp-flag", "faction-black", "p-arena", "skymaze"):
        run([rs.ROOT + rf"\map\map\{n}.DMap"], n)
