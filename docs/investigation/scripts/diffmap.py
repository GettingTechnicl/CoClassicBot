import struct,json
S=r"C:\Users\TERRYG~1\AppData\Local\Temp\claude\C--Users-TerryGluff-Documents-Claude-CO99\eb0bd2e7-43e0-435d-9312-509bfc9a410f\scratchpad"
tw=json.load(open(S+r"\twin.json")); bd=json.load(open(S+r"\bird.json"))
print("twin hero",tw["hero"],"roleMgr",tw["roleMgr"],"| bird hero",bd["hero"],"roleMgr",bd["roleMgr"])
def isheap(v): return 0x1000000 < v < 0x7FFFFFFFFFFF
def diffptr(name, fa, fb, base):
    a=open(S+"\\"+fa,"rb").read(); b=open(S+"\\"+fb,"rb").read()
    n=min(len(a),len(b)); hits=[]
    for off in range(0,n-8,8):
        va=struct.unpack_from("<Q",a,off)[0]; vb=struct.unpack_from("<Q",b,off)[0]
        if va!=vb and isheap(va) and isheap(vb):
            hits.append((off,va,vb))
    print(f"--- {name}: {len(hits)} changed heap-pointer qwords ---")
    for off,va,vb in hits[:30]:
        print(f"   +0x{off:X} (abs 0x{base+off:X}):  0x{va:X} -> 0x{vb:X}")
    return hits
diffptr("globals", "twin_globals.bin","bird_globals.bin", 0x140640000)
