"""READ-ONLY. Full .scene binary layout, DMap trailing record, Twin City bridge placements."""
import os, re, struct
GD = "F:/Games/Classic Conquer 2.0"
def rd(p): return open(p,"rb").read()
def cstr(b): return b.split(b"\0",1)[0].decode("latin-1","ignore")
def hexdump(b, base=0, n=0x100):
    for off in range(0, min(len(b), n), 16):
        c = b[off:off+16]
        print(f"    +0x{base+off:04X}: " + " ".join(f"{x:02X}" for x in c).ljust(48) + "  " + "".join(chr(x) if 32<=x<127 else "." for x in c))

# ---- A) full stand06.scene, with the int block decoded ----
b = rd(os.path.join(GD,"map","Scene","stand06.scene"))
print(f"== Scene/stand06.scene FULL ({len(b)} bytes) ==")
n = struct.unpack_from("<I", b, 0)[0]; print(f"   partCount={n}")
print(f"   +0x004 aniFile[260] = {cstr(b[4:4+260])!r}")
print(f"   +0x108 aniTitle[64] = {cstr(b[0x108:0x108+64])!r}")
ints = struct.unpack_from("<%di" % ((len(b)-0x148)//4), b, 0x148)
print(f"   +0x148 ints ({len(ints)}): {ints}")
print("   hex from +0x148:"); hexdump(b[0x148:], 0x148, 0x60)

# ---- B) DMap 1010 trailing record after the 38 layers ----
d = rd(os.path.join(GD,"map","map","newbie.DMap")); w,h = struct.unpack_from("<II", d, 0x10C); row=w*6+4
tail = d[0x114+h*row:]
print(f"\n== 1010 tail trailing bytes after 38 layer records (tail+0x32D0..) ==")
hexdump(tail[0x32D0:0x3300], 0x32D0, 0x30)
print(f"   ints @+0x32D0: {struct.unpack_from('<7i', tail, 0x32D0)}  then puzzle path {cstr(tail[0x32EC:0x32EC+260])!r}; tail ends at 0x{len(tail):X}")

# ---- C) stand01.Part (mixed cells?) + generic .Part reader ----
def read_part(name):
    t = rd(os.path.join(GD,"map","ScenePart",name)).decode("latin-1")
    kv = dict(l.split("=",1) for l in t.replace("\r","").split("\n") if "=" in l and not l.startswith("Cell"))
    cells = {}
    for m in re.finditer(r"Cell\[(\d+),(\d+)\]=\{(-?\d+),(-?\d+),(-?\d+)\}", t):
        cells[(int(m.group(1)),int(m.group(2)))] = (int(m.group(3)),int(m.group(4)),int(m.group(5)))
    return kv, cells
for name in ["stand01.Part","bridge01.Part","wbridge01.Part"]:
    kv, cells = read_part(name)
    W,H = int(kv["Width"]), int(kv["Height"])
    print(f"\n== ScenePart/{name}: {kv}  cells={len(cells)} ==")
    print("   mask grid (rows=y, cols=x; 0=walkable 1=blocked):")
    for y in range(H):
        print("     y=%2d  " % y + " ".join(str(cells.get((x,y),(9,0,0))[0]) for x in range(W)))

# ---- D) Twin City (1002) scene placements containing 'bridge' ----
d = rd(os.path.join(GD,"map","map","newplain.DMap")); w,h = struct.unpack_from("<II", d, 0x10C); row=w*6+4
tail = d[0x114+h*row:]
p=0
np_ = struct.unpack_from("<I", tail, p)[0]; p+=4
portals=[struct.unpack_from("<3i", tail, p+12*i) for i in range(np_)]; p+=12*np_
nl = struct.unpack_from("<I", tail, p)[0]; p+=4
print(f"\n== 1002 (Twin City) tail: portals={portals}  layerCount={nl} ==")
counts={}; scenes=[]
for i in range(nl):
    t = struct.unpack_from("<I", tail, p)[0]
    if t==4: p += 420
    elif t==1:
        fn = cstr(tail[p+4:p+4+260]); x,y = struct.unpack_from("<ii", tail, p+264); scenes.append((fn,x,y)); p += 272
    elif t==10: p += 4+64+8
    elif t==15: p += 4+64+12
    else:
        print(f"   layer[{i}] unknown type {t} at tail+0x{p:X}; next bytes: " + " ".join(f"{q:02X}" for q in tail[p:p+48])); break
    counts[t]=counts.get(t,0)+1
print(f"   type counts: {counts}; consumed 0x{p:X} of 0x{len(tail):X}; remaining {len(tail)-p}")
print(f"   scene placements: {len(scenes)}; with 'bridge' in name:")
for fn,x,y in scenes:
    if "bridge" in fn.lower(): print(f"     {fn} at ({x},{y})")
print("   distinct scene files on 1002:", sorted(set(os.path.basename(f) for f,_,_ in scenes))[:40])
