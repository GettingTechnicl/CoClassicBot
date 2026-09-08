import struct
def load(f): return open(f,"rb").read()
hero=load("xpafter_hero.bin"); stat=load("xpafter_stat.bin")
P=77.723/100.0
# 1) float32 near 77.723 or 0.77723
print("=== float32 matches for 77.723 / 0.77723 ===")
for name,buf,base in [("hero",hero,0x635C560),("stat",stat,0x627CD00)]:
    for off in range(0,len(buf)-4,4):
        v=struct.unpack_from("<f",buf,off)[0]
        if abs(v-77.723)<0.05 or abs(v-0.77723)<0.0005:
            print(f"  {name}+0x{off:X}: {v}")
# 2) adjacent-ish u32 pairs with ratio ~0.77723 (current_exp / needed_exp)
print("=== u32 pairs with ratio ~0.77723 (exp/needed) within 0x20 ===")
for name,buf,base in [("hero",hero,0x635C560),("stat",stat,0x627CD00)]:
    n=len(buf)
    for off in range(0,n-4,4):
        a=struct.unpack_from("<I",buf,off)[0]
        if a<1000 or a>2000000000: continue
        for d in range(4,0x24,4):
            if off+d+4>n: break
            b=struct.unpack_from("<I",buf,off+d)[0]
            if b>a and b<2000000000 and abs(a/b - P)<0.0008:
                print(f"  {name}+0x{off:X}={a}  /  +0x{off+d:X}={b}  = {a/b:.5f}")
