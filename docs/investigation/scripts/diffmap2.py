import struct
def isptr(v): return 0x02000000 <= v < 0x100000000 and (v & 0xF)==0
def diff(name, fa, fb, base):
    a=open(fa,"rb").read(); b=open(fb,"rb").read(); n=min(len(a),len(b)); hits=[]
    for off in range(0,n-8,8):
        va=struct.unpack_from("<Q",a,off)[0]; vb=struct.unpack_from("<Q",b,off)[0]
        if va!=vb and isptr(va) and isptr(vb): hits.append((off,va,vb))
    print(f"=== {name}: {len(hits)} changed 16-aligned heap-pointer qwords ===")
    for off,va,vb in hits[:40]:
        print(f"   +0x{off:X} (abs 0x{base+off:X}):  0x{va:X} -> 0x{vb:X}")
    return hits
diff("HERO",   "twin_hero.bin",   "bird_hero.bin",   0x635C560)
diff("ROLEMGR","twin_rolemgr.bin","bird_rolemgr.bin",0x627C780)
diff("GLOBALS","twin_globals.bin","bird_globals.bin",0x140640000)
