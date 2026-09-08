"""READ-ONLY. Derive DMap layer record sizes, decode .scene/.Part formats, test overlay coverage."""
import os, re, struct
GD = r"F:\Games\Classic Conquer 2.0"
def rd(p): return open(p,"rb").read()
def cstr(b): return b.split(b"\0",1)[0].decode("latin-1","ignore")
def hexdump(b, base=0, n=0x100):
    for off in range(0, min(len(b), n), 16):
        c = b[off:off+16]
        print(f"    +0x{base+off:04X}: " + " ".join(f"{x:02X}" for x in c).ljust(48) + "  " + "".join(chr(x) if 32<=x<127 else "." for x in c))

# ---------- 1) map 1010 base grid at screenshot coords + tail record sizes ----------
d = rd(os.path.join(GD, r"map\map\newbie.DMap"))
w,h = struct.unpack_from("<II", d, 0x10C); row=w*6+4
def cell(x,y):
    o = 0x114 + y*row + x*6
    return struct.unpack_from("<HHh", d, o)  # mask, terrain, alt
print("== map 1010 (newbie.DMap) BASE GRID at screenshot coords (mask,terrain,alt) ==")
for xy in [(43,71),(62,109),(88,85)]:
    print(f"   {xy}: mask={cell(*xy)[0]} terrain={cell(*xy)[1]} alt={cell(*xy)[2]}")
tail = d[0x114+h*row:]
print(f"\n== 1010 tail: {len(tail)} bytes. String offsets (to derive record sizes) ==")
offs=[(m.start(), m.group().decode()) for m in re.finditer(rb"(ani\[A-Za-z0-9_\-]+\.ani|[A-Za-z0-9_\-]+\.tga|map\Scene\[A-Za-z0-9_\-]+\.scene|map\puzzle\[A-Za-z0-9_\-]+\.pul)", tail)]
for o,s in offs[:12]: print(f"   tail+0x{o:05X}  {s}")
print("   ...")
for o,s in offs[-6:]: print(f"   tail+0x{o:05X}  {s}")
ani=[o for o,s in offs if s.endswith(".ani")]; sc=[o for o,s in offs if s.endswith(".scene")]
if len(ani)>1: print(f"   COVER(type4) record stride = 0x{ani[1]-ani[0]:X} ({ani[1]-ani[0]})")
if len(sc)>1:  print(f"   SCENE(type1) record stride = 0x{sc[1]-sc[0]:X} ({sc[1]-sc[0]})")
print("\n   raw bytes around first COVER record (tail+0x08 .. +0x170):"); hexdump(tail[0x08:0x170], 0x08, 0x168)
if sc:
    print(f"\n   raw bytes at first SCENE record (tail+0x{sc[0]-4:X}):"); hexdump(tail[sc[0]-4:sc[0]-4+0x120], sc[0]-4, 0x120)

# ---------- 2) .scene format ----------
for name in ["stand06.scene","stand01.scene","bridgeA.scene"]:
    p=os.path.join(GD,"map","Scene",name); b=rd(p)
    print(f"\n== {name}: {len(b)} bytes ==")
    hexdump(b,0,0x80)
    print("   strings:", [m.group().decode() for m in re.finditer(rb"[\x20-\x7e]{4,}", b)][:12])
# ---------- 3) .Part format ----------
for name in ["bridge01.Part"]:
    p=os.path.join(GD,"map","ScenePart",name); b=rd(p)
    print(f"\n== ScenePart/{name}: {len(b)} bytes =="); hexdump(b,0,0xC0)
    print("   strings:", [m.group().decode() for m in re.finditer(rb"[\x20-\x7e]{4,}", b)][:12])
# ---------- 4) puzzle header ----------
p=os.path.join(GD,"map","puzzle","newbie.pul"); b=rd(p)
print(f"\n== puzzle/newbie.pul: {len(b)} bytes =="); hexdump(b,0,0x60)
print("   strings:", [m.group().decode() for m in re.finditer(rb"[\x20-\x7e]{4,}", b)][:8])
