"""READ-ONLY. Pin .scene per-part layout, anchor convention via base-grid void test, 1002 unknown layer types."""
import os, re, struct
GD = "F:/Games/Classic Conquer 2.0"
def rd(p): return open(p,"rb").read()
def cstr(b): return b.split(b"\0",1)[0].decode("latin-1","ignore")
def hexdump(b, base=0, n=0x100):
    for off in range(0, min(len(b), n), 16):
        c = b[off:off+16]
        print(f"    +0x{base+off:04X}: " + " ".join(f"{x:02X}" for x in c).ljust(48) + "  " + "".join(chr(x) if 32<=x<127 else "." for x in c))

# ---- A) .scene per-part layout: file[256] title[64] then ints; find where cells start ----
def scene_parts(name):
    b = rd(os.path.join(GD,"map","Scene",name)); n = struct.unpack_from("<I", b, 0)[0]; p = 4; parts=[]
    for i in range(n):
        f = cstr(b[p:p+256]); t = cstr(b[p+256:p+320]); q = p+320
        hdr = struct.unpack_from("<9i", b, q)   # OffsetX,OffsetY,AniInterval,Width,Height,Thick, +3 unknown
        W,H = hdr[3],hdr[4]
        parts.append((f,t,hdr,q))
        p = q + 9*4 + W*H*12
    return b, n, parts, p
for name in ["stand01.scene","bridgeA.scene"]:
    b,n,parts,end = scene_parts(name)
    print(f"== Scene/{name}: {len(b)} bytes, partCount={n}, parsed-end=0x{end:X} ({'OK' if end==len(b) else 'MISMATCH'}) ==")
    for f,t,hdr,q in parts:
        print(f"   part file={f!r} title={t!r}  OffX={hdr[0]} OffY={hdr[1]} Interval={hdr[2]} W={hdr[3]} H={hdr[4]} Thick={hdr[5]}  extra3={hdr[6:9]}")
        W,H = hdr[3],hdr[4]
        cells = struct.unpack_from("<%di" % (W*H*3), b, q+36)
        print("      first 12 cell-ints:", cells[:12])
        # print mask grid assuming cells stored [x-major? or y-major?] as {mask,terrain,alt}; show both interpretations for 1st part of stand01
        if name=="stand01.scene":
            m=[cells[3*k] for k in range(W*H)]
            print("      mask if row-major (y rows):"); [print("        ", m[y*W:(y+1)*W]) for y in range(H)]
            print("      mask if col-major (x rows):"); [print("        ", m[x*H:(x+1)*H]) for x in range(W)]
            print("      (.Part text says rows y=0..3: [0 0 0 1],[0 0 0 0],[0 0 0 0],[1 0 0 1])")

# ---- B) base-grid neighborhood of map 1010 around the stand chain + anchor test ----
d = rd(os.path.join(GD,"map","map","newbie.DMap")); w,h = struct.unpack_from("<II", d, 0x10C); row=w*6+4
def base(x,y):
    if not (0<=x<w and 0<=y<h): return 9
    return struct.unpack_from("<H", d, 0x114+y*row+x*6)[0]
tail = d[0x114+h*row:]
p=8; stands=[]
for i in range(38):
    t=struct.unpack_from("<I", tail, p)[0]
    if t==4: p+=420
    elif t==1:
        fn=cstr(tail[p+4:p+264]); x,y=struct.unpack_from("<ii", tail, p+264); stands.append((os.path.basename(fn),x,y)); p+=272
print("\n== map 1010 base mask grid, x=60..95 (cols) by y=76..110 (rows). '.'=walkable(0) '#'=blocked(1) 'S'=scene anchor cell ==")
anch={(x,y):fn for fn,x,y in stands}
print("        " + "".join(str(x%10) for x in range(60,96)))
for y in range(76,111):
    print(f"  y={y:3d}  " + "".join("S" if (x,y) in anch else ("." if base(x,y)==0 else "#") for x in range(60,96)))
# part sizes
def part_wh(scene):
    kv = rd(os.path.join(GD,"map","ScenePart",scene.replace(".scene",".Part"))).decode("latin-1")
    W=int(re.search(r"Width=(\d+)",kv).group(1)); H=int(re.search(r"Height=(\d+)",kv).group(1)); return W,H
print("\n== anchor-convention test: for each stand, count base-BLOCKED tiles under each candidate footprint ==")
conv = {"topleft:(x..x+W-1,y..y+H-1)":lambda x,y,W,H:[(x+i,y+j) for i in range(W) for j in range(H)],
        "bottomright:(x-W+1..x,y-H+1..y)":lambda x,y,W,H:[(x-i,y-j) for i in range(W) for j in range(H)],
        "center-ish:(x-W//2..,y-H//2..)":lambda x,y,W,H:[(x-W//2+i,y-H//2+j) for i in range(W) for j in range(H)]}
tot={k:[0,0] for k in conv}
for fn,x,y in stands:
    W,H=part_wh(fn)
    line=f"   {fn:14s} at ({x:3d},{y:3d}) {W}x{H}: "
    for k,f in conv.items():
        tiles=f(x,y,W,H); blk=sum(1 for t in tiles if base(*t)==1); tot[k][0]+=blk; tot[k][1]+=len(tiles)
        line+=f" {k.split(':')[0]}={blk}/{len(tiles)}"
    print(line)
print("   TOTAL blocked-under-footprint:", {k:f"{v[0]}/{v[1]}" for k,v in tot.items()}, "(the true convention should be ~all blocked = stands sit over void)")

# ---- C) 1002: sizes of type 10 / 15 records + what follows ----
d2 = rd(os.path.join(GD,"map","map","newplain.DMap")); w2,h2 = struct.unpack_from("<II", d2, 0x10C); row2=w2*6+4
t2 = d2[0x114+h2*row2:]
p=4+12*6+4
for i in range(1240):
    t=struct.unpack_from("<I", t2, p)[0]
    if t==4: p+=420
    elif t==1: p+=272
    elif t in (10,15):
        print(f"\n== 1002 layer[{i}] type={t} at tail+0x{p:X} ==  strings nearby:", [m.group().decode() for m in re.finditer(rb"[\x20-\x7e]{4,}", t2[p:p+0x200])][:6])
        hexdump(t2[p:p+0x60], p, 0x60)
        # find next record start = next occurrence of a plausible type dword followed by 'ani\' or 'map\' or 'c3\'
        m = re.search(rb"(?:\x04|\x01|\x0a|\x0f)\x00\x00\x00(?=ani.|map.|c3.|sound.)", t2[p+8:p+0x400])
        nxt = p+8+m.start() if m else None
        print(f"   next record starts at tail+0x{nxt:X} => record size {nxt-p} bytes" if nxt else "   next record start not found by heuristic")
        p = nxt if nxt else p+ (4+64+8 if t==10 else 4+64+12)
    else:
        print(f"\n== 1002 parse ended at layer[{i}] type={t} tail+0x{p:X}; remaining {len(t2)-p} bytes; strings in remainder:",
              [m.group().decode() for m in re.finditer(rb"[\x20-\x7e]{4,}", t2[p:])][:20]); break
