import struct
S=r"C:\Users\TERRYG~1\AppData\Local\Temp\claude\C--Users-TerryGluff-Documents-Claude-CO99\eb0bd2e7-43e0-435d-9312-509bfc9a410f\scratchpad"
def load(f): return open(S+"\\"+f,"rb").read()
def diff32(name,fa,fb,base):
    a=load(fa); b=load(fb); n=min(len(a),len(b))
    print(f"=== {name}: u32 values that INCREASED (candidate EXP/counters) ===")
    for off in range(0,n-4,4):
        va=struct.unpack_from("<I",a,off)[0]; vb=struct.unpack_from("<I",b,off)[0]
        if vb>va and (vb-va)<50000000 and va>0 and va<0xF0000000:
            # skip obvious floats/pointers; show increases
            print(f"   +0x{off:X} (abs 0x{base+off:X}): {va} -> {vb}  (+{vb-va})")
def diff64(name,fa,fb,base):
    a=load(fa); b=load(fb); n=min(len(a),len(b))
    print(f"--- {name}: u64 that increased ---")
    for off in range(0,n-8,8):
        va=struct.unpack_from("<Q",a,off)[0]; vb=struct.unpack_from("<Q",b,off)[0]
        if vb>va and (vb-va)<50000000 and 0<va<0xFFFFFFFFFF:
            print(f"   +0x{off:X} (abs 0x{base+off:X}): {va} -> {vb}  (+{vb-va})")
import json
tw=json.load(open(S+r"\xpbefore.json")); hero=int(tw['hero'],16); st=int(tw['statTable'],16)
diff32("HERO",   "xpbefore_hero.bin","xpafter_hero.bin", hero)
diff32("STAT",   "xpbefore_stat.bin","xpafter_stat.bin", st)
diff64("HERO64", "xpbefore_hero.bin","xpafter_hero.bin", hero)
