import struct
S=r"C:\Users\TERRYG~1\AppData\Local\Temp\claude\C--Users-TerryGluff-Documents-Claude-CO99\eb0bd2e7-43e0-435d-9312-509bfc9a410f\scratchpad"
P1,P2=0.77723,0.78528
def load(f): return open(S+"\\"+f,"rb").read()
def analyze(name, fb, fa, base):
    b=load(fb); a=load(fa); n=min(len(b),len(a))
    # collect all u32 at each offset (before & after)
    print(f"=== {name}: exp candidates (increased, ratio {P2/P1:.5f}, or has nearby threshold) ===")
    def U(buf,o): return struct.unpack_from("<I",buf,o)[0]
    for off in range(0,n-4,4):
        vb=U(b,off); va=U(a,off)
        if not (va>vb and vb>1000 and va<2000000000): continue
        r=va/vb
        # exp raw should grow by factor P2/P1 = 1.01036
        if abs(r-(P2/P1))<0.0015:
            # look for a nearby unchanged threshold T where vb/T~P1 and va/T~P2
            thr=None
            for d in range(-0x40,0x44,4):
                if d==0 or off+d<0 or off+d+4>n: continue
                tb=U(b,off+d); ta=U(a,off+d)
                if tb==ta and tb>va and abs(vb/tb-P1)<0.002 and abs(va/tb-P2)<0.002:
                    thr=(off+d,tb); break
            tag=f"  <== EXP! threshold at +0x{thr[0]:X}={thr[1]}" if thr else ""
            print(f"   +0x{off:X} (0x{base+off:X}): {vb} -> {va}  ratio={r:.5f}{tag}")
analyze("HERO","e2before_hero.bin","e2after_hero.bin",0x635C560)
analyze("STAT","e2before_stat.bin","e2after_stat.bin",0x627CD00)
