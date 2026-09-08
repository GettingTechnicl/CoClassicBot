"""READ-ONLY. Derive DMap layer record sizes, decode .scene/.Part formats, test overlay coverage."""
import os, re, struct
GD = r"F:\Games\Classic Conquer 2.0"
def rd(p): return open(p,"rb").read()
def cstr(b): return b.split(b"\0",1)[0].decode("latin-1","ignore")
def hexdump(b, base=0, n=0x100):
    for off in range(0, min(len(b), n), 16):
        c = b[off:off+16]
        print(f"    +0x{base+off:04X}: " + " ".join(f"{x:02X}" for x in c).ljust(48) + "  " + "".join(chr(x) if 32<=x<127 else "." for x in c))
PAT = re.compile(rb"(ani.[A-Za-z0-9_-]+[.]ani|[A-Za-z0-9_-]+[.]tga|map.Scene.[A-Za-z0-9_-]+[.]scene|map.puzzle.[A-Za-z0-9_-]+[.]pul)")
STR = re.compile(rb"[\x20-\x7e]{4,}")

d = rd(os.path.join(GD, "map", "map", "newbie.DMap"))
w,h = struct.unpack_from("<II", d, 0x10C); row=w*6+4
tail = d[0x114+h*row:]
print(f"== 1010 tail: {len(tail)} bytes. String offsets ==")
offs=[(m.start(), m.group().decode()) for m in PAT.finditer(tail)]
for o,s in offs[:10]: print(f"   tail+0x{o:05X}  {s}")
print("   ...")
for o,s in offs[-8:]: print(f"   tail+0x{o:05X}  {s}")
ani=[o for o,s in offs if s.endswith(".ani")]; sc=[o for o,s in offs if s.endswith(".scene")]
if len(ani)>1: print(f"   COVER(type4) record stride = 0x{ani[1]-ani[0]:X} ({ani[1]-ani[0]})  [ani->ani]")
if len(sc)>1:  print(f"   SCENE record stride = 0x{sc[1]-sc[0]:X} ({sc[1]-sc[0]})  [scene->scene]")
print("\n   first COVER record raw (tail+0x08..):"); hexdump(tail[0x08:0x08+0x140], 0x08, 0x140)
if sc:
    print(f"\n   first SCENE record raw (tail+0x{sc[0]-8:X}..):"); hexdump(tail[sc[0]-8:sc[0]-8+0x130], sc[0]-8, 0x130)
    # decode all scene records assuming [u32 type=1][char file[260]][i32 x][i32 y]
    print("\n   SCENE placements on map 1010 (assuming type,file[260],x,y):")
    for o in sc:
        t = struct.unpack_from("<I", tail, o-4)[0]
        fn = cstr(tail[o:o+260]); x,y = struct.unpack_from("<ii", tail, o+260)
        print(f"     type={t} {fn} at ({x},{y})")

for name in ["stand06.scene","stand01.scene","bridgeA.scene"]:
    b=rd(os.path.join(GD,"map","Scene",name))
    print(f"\n== Scene/{name}: {len(b)} bytes =="); hexdump(b,0,0xA0)
    print("   strings:", [m.group().decode() for m in STR.finditer(b)][:12])
for name in ["stand06.Part","bridge01.Part"]:
    b=rd(os.path.join(GD,"map","ScenePart",name))
    print(f"\n== ScenePart/{name}: {len(b)} bytes =="); hexdump(b,0,0x140)
    print("   strings:", [m.group().decode() for m in STR.finditer(b)][:12])
b=rd(os.path.join(GD,"map","puzzle","newbie.pul"))
print(f"\n== puzzle/newbie.pul: {len(b)} bytes =="); hexdump(b,0,0x60)
print("   strings:", [m.group().decode() for m in STR.finditer(b)][:8])
