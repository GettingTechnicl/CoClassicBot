"""
Phase 2 capture (read-only RPM, run elevated):
 - dump decrypted code+data region (base..base+0x552000) to code_dump.bin (file offset == RVA)
 - dump roleMgr[0..0x400] (to decode m_deqRole targeting deque)
 - dump 4 equipped CItems[0..0x100] (to pin durability/attack/defense vs known stats)
 - dump hero level/exp region + stat table head
Writes code_dump.bin and phase2_out.json next to this script.
"""
import ctypes as C
from ctypes import wintypes as W
import json, struct, os

k32=C.WinDLL("kernel32",use_last_error=True)
PVR=0x10; PQI=0x400; BASE=0x140000000; ANCHOR=0x69C730
HERE=os.path.dirname(os.path.abspath(__file__))

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

def main():
    pid=pid_of()
    if not pid: print(json.dumps({"error":"not running"})); return
    r=R(pid); out={"pid":pid}

    # ---- dump decrypted code+data region: base .. base+0x552000 (file offset == RVA) ----
    DUMP_LEN=0x552000
    path=os.path.join(HERE,"code_dump.bin")
    with open(path,"wb") as f:
        got=0; off=0; miss=0
        while off<DUMP_LEN:
            chunk=min(0x100000,DUMP_LEN-off)
            b=r.rd(BASE+off,chunk)
            if b is None:
                f.write(b"\x00"*chunk); miss+=chunk   # keep offsets aligned
            else:
                if len(b)<chunk: b=b+b"\x00"*(chunk-len(b))
                f.write(b); got+=chunk
            off+=chunk
    out["code_dump"]={"path":path,"len":hex(DUMP_LEN),"unreadable_bytes":miss}

    # ---- resolve chain ----
    roleMgr=r.u64(BASE+ANCHOR); hero=r.u64(roleMgr) if roleMgr else None
    out["roleMgr"]=hex(roleMgr or 0); out["hero"]=hex(hero or 0)
    if not hero:
        json.dump(out,open(os.path.join(HERE,"phase2_out.json"),"w"),indent=2); print(json.dumps(out)); return

    # ---- roleMgr[0..0x400] as qwords (find m_deqRole) ----
    out["roleMgr_qwords"]=[hex(r.u64(roleMgr+i*8) or 0) for i in range(0x80)]

    # ---- equipped CItems: 4 slots at hero+0xBD8 + slot*0x10 ----
    items=[]
    for slot in range(4):
        p=r.u64(hero+0xBD8+slot*0x10)
        rec={"slot":slot,"ptr":hex(p or 0)}
        if p:
            rec["u32"]=[r.u32(p+j*4) for j in range(64)]     # first 0x100 bytes
            rec["u16"]=list(struct.unpack("<128H", (r.rd(p,0x100) or b"\x00"*0x100)))
        items.append(rec)
    out["equip_items"]=items

    # ---- level/exp region + stat table head ----
    out["hero_6E0_720_u32"]=[r.u32(hero+0x6E0+i*4) for i in range(16)]
    st=r.u64(hero+0x968)
    out["statTable"]=hex(st or 0)
    if st:
        out["statTable_u32_0..64"]=[r.u32(st+i*4) for i in range(64)]

    json.dump(out,open(os.path.join(HERE,"phase2_out.json"),"w"),indent=2)
    print(json.dumps({"ok":True,"code_dump":path,"json":os.path.join(HERE,"phase2_out.json"),
                      "roleMgr":out["roleMgr"],"hero":out["hero"]}))

if __name__=="__main__":
    main()
