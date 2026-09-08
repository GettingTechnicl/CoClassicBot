"""READ-ONLY. Compare vtable slots across VT_ROLE_B / VT_MONSTER / VT_HERO; find each vtable's length."""
import os, struct
SP=os.path.dirname(os.path.abspath(__file__)); d=open(os.path.join(SP,"image_dump.bin"),"rb").read()
BASE=0x140000000; TEXT_HI=BASE+0x400000  # code lives below ~0x400000 RVA in this image
def q(r): return struct.unpack_from("<Q",d,r)[0]
def vt(rva, maxn=120):
    out=[]
    for i in range(maxn):
        v=q(rva+8*i)
        if not (BASE<=v<TEXT_HI): break   # stop at first non-code qword (next vtable's RTTI/other data or end)
        out.append(v)
    return out
V={"ROLE_B":0x5CDB10,"MONSTER":0x5CFA70,"HERO":0x5CEF60}
T={k:vt(r) for k,r in V.items()}
for k,t in T.items(): print(f"{k:8s} vtable 0x{V[k]:X}: {len(t)} code slots")
n=min(len(t) for t in T.values())
same_all=sum(1 for i in range(n) if T["ROLE_B"][i]==T["MONSTER"][i]==T["HERO"][i])
rb_mon=sum(1 for i in range(n) if T["ROLE_B"][i]==T["MONSTER"][i])
rb_hero=sum(1 for i in range(n) if T["ROLE_B"][i]==T["HERO"][i])
mon_hero=sum(1 for i in range(n) if T["MONSTER"][i]==T["HERO"][i])
print(f"\nover first {n} slots: identical in all three={same_all}  ROLE_B==MONSTER={rb_mon}  ROLE_B==HERO={rb_hero}  MONSTER==HERO={mon_hero}")
print("\nslot-by-slot (first 40): idx  ROLE_B        MONSTER       HERO          note")
for i in range(min(n,40)):
    a,b,c=T["ROLE_B"][i],T["MONSTER"][i],T["HERO"][i]
    note="ALL SAME" if a==b==c else ("ROLE_B unique" if a!=b and a!=c else ("=MONSTER only" if a==b else "=HERO only"))
    print(f"  {i:3d}  0x{a-BASE:06X}      0x{b-BASE:06X}      0x{c-BASE:06X}      {note}")
# distinguishing slots where ROLE_B differs from BOTH
diff=[i for i in range(n) if T["ROLE_B"][i]!=T["MONSTER"][i] and T["ROLE_B"][i]!=T["HERO"][i]]
print(f"\nslots where ROLE_B differs from BOTH monster and hero: {diff[:30]}{' ...' if len(diff)>30 else ''} (count {len(diff)})")
