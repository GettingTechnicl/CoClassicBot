"""
Confirm the v1074 static anchor and test the '+0x50 field-insertion' hypothesis.
Read-only external RPM. Run elevated.
"""
import ctypes as C
from ctypes import wintypes as W
import json, struct

k32=C.WinDLL("kernel32",use_last_error=True)
PVR=0x10; PQI=0x400; BASE=0x140000000
ANCHOR_RVA=0x69C730

class PE32(C.Structure):
    _fields_=[("dwSize",W.DWORD),("a",W.DWORD),("pid",W.DWORD),("b",C.c_void_p),
        ("c",W.DWORD),("d",W.DWORD),("e",W.DWORD),("f",C.c_long),("g",W.DWORD),("exe",C.c_char*260)]

def pid_of(name=b"ImConquer.exe"):
    s=k32.CreateToolhelp32Snapshot(0x2,0); pe=PE32(); pe.dwSize=C.sizeof(pe); r=None
    if k32.Process32First(s,C.byref(pe)):
        while True:
            if pe.exe.lower()==name.lower(): r=pe.pid; break
            if not k32.Process32Next(s,C.byref(pe)): break
    k32.CloseHandle(s); return r

class R:
    def __init__(s,pid):
        s.h=k32.OpenProcess(PVR|PQI,False,pid)
        if not s.h: raise OSError(C.get_last_error())
    def rd(s,a,n):
        if not a: return None
        b=(C.c_ubyte*n)(); g=C.c_size_t(0)
        if not k32.ReadProcessMemory(s.h,C.c_void_p(a),b,n,C.byref(g)): return None
        return bytes(b[:g.value])
    def u64(s,a):
        b=s.rd(a,8); return struct.unpack("<Q",b)[0] if b and len(b)==8 else None
    def u32(s,a):
        b=s.rd(a,4); return struct.unpack("<I",b)[0] if b and len(b)==4 else None
    def i32(s,a):
        b=s.rd(a,4); return struct.unpack("<i",b)[0] if b and len(b)==4 else None
    def cstr(s,a,n=16):
        b=s.rd(a,n); return b.split(b"\x00")[0].decode("latin1","replace") if b else None

def main():
    pid=pid_of()
    if not pid: print(json.dumps({"error":"not running"})); return
    r=R(pid); out={"pid":pid}

    # ---- confirm anchor chain ----
    anchor_slot=BASE+ANCHOR_RVA
    roleMgr=r.u64(anchor_slot)
    hero=r.u64(roleMgr) if roleMgr else None   # m_pHero @ +0x00
    out["anchor"]={"slot":hex(anchor_slot),"roleMgr":hex(roleMgr or 0),"hero":hex(hero or 0)}
    if hero:
        out["anchor"]["hero_name@0x94"]=r.cstr(hero+0x94)
        out["anchor"]["hero_id@0x68"]=r.u32(hero+0x68)
        out["anchor"]["hero_maxhp@0x3D0"]=r.i32(hero+0x3D0)
        out["anchor"]["CONFIRMED"]= r.cstr(hero+0x94)=="Kinux"
    if not hero: print(json.dumps(out,indent=2)); return

    # ---- role manager structure (find the role deque) ----
    out["roleMgr_qwords@0..0x100"]=[hex(r.u64(roleMgr+i*8) or 0) for i in range(32)]

    # ---- test +0x50 shift hypothesis on CHero fields ----
    def deque_size(a): # MSVC deque _Mysize @ +0x18
        return r.u64(a+0x18)
    def vec_count(a):
        f=r.u64(a); l=r.u64(a+8)
        return ((l-f)//8) if (f and l and l>=f and l-f<0x100000) else None
    tests={}
    for label,old,new in [("deqItem",0xB20,0xB70),("equip",0xB88,0xBD8),
                           ("maxMana_i32",0xCA8,0xCF8),("skillsVec",0x1918,0x1968)]:
        rec={}
        if label=="deqItem":
            rec["old@%X_size"%old]=deque_size(hero+old)
            rec["new@%X_size"%new]=deque_size(hero+new)
        elif label=="equip":
            rec["old@%X"%old]=[hex(r.u64(hero+old+i*8) or 0) for i in range(8)]
            rec["new@%X"%new]=[hex(r.u64(hero+new+i*8) or 0) for i in range(8)]
        elif label=="maxMana_i32":
            rec["old@%X"%old]=r.i32(hero+old); rec["new@%X"%new]=r.i32(hero+new)
        elif label=="skillsVec":
            rec["old@%X_count"%old]=vec_count(hero+old)
            rec["new@%X_count"%new]=vec_count(hero+new)
        tests[label]=rec
    out["shift_tests"]=tests

    # ---- follow equipment (new offset) to CItems, look for durability 41/41 & 16/27 ----
    equip_new=[r.u64(hero+0xBD8+i*8) for i in range(8)]
    items=[]
    for i,p in enumerate(equip_new):
        if p and 0x10000<p<0x7FFFFFFFFFFF:
            words=[r.u32(p+j*4) for j in range(24)]  # first 0x60 bytes as u32
            items.append({"slot":i,"ptr":hex(p),"u32[0..24]":[w for w in words]})
    out["equip_items"]=items

    # ---- silver re-confirm & level candidate ----
    out["silver@0xA80"]=r.u32(hero+0xA80)
    out["level_cand@0x6E8"]=r.i32(hero+0x6E8)
    print(json.dumps(out,indent=2))

if __name__=="__main__":
    main()
