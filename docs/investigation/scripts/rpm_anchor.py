"""
Locate the v1074 role-manager anchor. Read-only external RPM.
1) find hero object via name "Kinux"
2) scan all committed memory for qwords == hero_addr  (these are pointers TO the hero)
3) for each, treat it as CRoleMgr::m_pHero (+0x00); validate m_deqRole @ +0x70 looks
   like a small std::deque; report whether the container is inside the image (=> new
   static RVA) or on the heap (=> needs a pointer-chain from a static).
"""
import ctypes as C
from ctypes import wintypes as W
import json, struct

k32 = C.WinDLL("kernel32", use_last_error=True)
PROCESS_VM_READ=0x10; PROCESS_QUERY_INFORMATION=0x400
IMG_BASE=0x140000000; IMG_SIZE=0x28C5000
NAME=b"Kinux"

class PE32(C.Structure):
    _fields_=[("dwSize",W.DWORD),("cntUsage",W.DWORD),("th32ProcessID",W.DWORD),
        ("th32DefaultHeapID",C.c_void_p),("th32ModuleID",W.DWORD),("cntThreads",W.DWORD),
        ("th32ParentProcessID",W.DWORD),("pcPriClassBase",C.c_long),("dwFlags",W.DWORD),
        ("szExeFile",C.c_char*260)]
class MBI(C.Structure):
    _fields_=[("BaseAddress",C.c_void_p),("AllocationBase",C.c_void_p),
        ("AllocationProtect",W.DWORD),("__a",W.DWORD),("RegionSize",C.c_size_t),
        ("State",W.DWORD),("Protect",W.DWORD),("Type",W.DWORD)]

def find_pid(name=b"ImConquer.exe"):
    s=k32.CreateToolhelp32Snapshot(0x2,0); pe=PE32(); pe.dwSize=C.sizeof(pe); pid=None
    if k32.Process32First(s,C.byref(pe)):
        while True:
            if pe.szExeFile.lower()==name.lower(): pid=pe.th32ProcessID; break
            if not k32.Process32Next(s,C.byref(pe)): break
    k32.CloseHandle(s); return pid

class R:
    def __init__(s,pid):
        s.h=k32.OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION,False,pid)
        if not s.h: raise OSError(C.get_last_error())
    def read(s,a,n):
        if not a: return None
        b=(C.c_ubyte*n)(); g=C.c_size_t(0)
        if not k32.ReadProcessMemory(s.h,C.c_void_p(a),b,n,C.byref(g)): return None
        return bytes(b[:g.value])
    def u64(s,a):
        b=s.read(a,8); return struct.unpack("<Q",b)[0] if b and len(b)==8 else None
    def u32(s,a):
        b=s.read(a,4); return struct.unpack("<I",b)[0] if b and len(b)==4 else None

def regions(h):
    m=MBI(); a=0; out=[]
    while a<0x7FFFFFFFFFFF:
        if not k32.VirtualQueryEx(h,C.c_void_p(a),C.byref(m),C.sizeof(m)): break
        if m.State==0x1000 and m.Protect not in (0x01,0x100) and (m.Protect&0xFF):
            out.append((m.BaseAddress or 0,m.RegionSize,m.Protect,m.Type))
        a=(m.BaseAddress or 0)+m.RegionSize
        if m.RegionSize==0: break
    return out

def main():
    pid=find_pid()
    if not pid: print(json.dumps({"error":"not running"})); return
    r=R(pid); out={"pid":pid}
    # 1) hero via name
    hero=None
    for base,size,prot,typ in regions(r.h):
        off=0
        while off<size:
            chunk=min(0x100000,size-off); b=r.read(base+off,chunk)
            if b:
                i=b.find(NAME+b"\x00")
                while i!=-1:
                    cand=base+off+i-0x94
                    if (r.u32(cand+0x68) or 0)>=1000000 and r.u32(cand+0x3D0)==2419:
                        hero=cand; break
                    i=b.find(NAME+b"\x00",i+1)
            if hero: break
            off+=chunk
        if hero: break
    out["hero_addr"]=hex(hero) if hero else None
    if not hero: print(json.dumps(out,indent=2)); return

    # 2) scan for pointers == hero
    target=struct.pack("<Q",hero); ptr_hits=[]
    for base,size,prot,typ in regions(r.h):
        off=0
        while off<size:
            chunk=min(0x100000,size-off); b=r.read(base+off,chunk)
            if b:
                i=b.find(target)
                while i!=-1:
                    if (base+off+i)%8==0:  # aligned pointer
                        ptr_hits.append(base+off+i)
                    if len(ptr_hits)>=200: break
                    i=b.find(target,i+1)
            if len(ptr_hits)>=200: break
            off+=chunk
        if len(ptr_hits)>=200: break

    # 3) evaluate each pointer slot as CRoleMgr::m_pHero (+0x00)
    cands=[]
    for p in ptr_hits:
        rec={"ptr_slot":hex(p)}
        in_img = IMG_BASE<=p<IMG_BASE+IMG_SIZE
        rec["in_image"]=in_img
        if in_img: rec["image_rva"]=hex(p-IMG_BASE)
        # role mgr candidate = container starting at p (m_pHero @ +0x00). deque @ +0x70
        deq_size=r.u64(p+0x70+0x18)   # MSVC deque _Mysize
        rec["deque_size@+0x88"]=deq_size if (deq_size is not None and deq_size<100000) else hex(deq_size or 0)
        # plausible role mgr if deque size is a small sane count of nearby roles
        rec["plausible_rolemgr"]= isinstance(deq_size,int) and 0<deq_size<20000
        cands.append(rec)
    out["ptr_to_hero_count"]=len(ptr_hits)
    out["candidates"]=cands
    # highlight the best: prefer in-image with plausible deque, else any plausible
    best=[c for c in cands if c.get("plausible_rolemgr")]
    out["best_rolemgr"]= ([c for c in best if c["in_image"]] or best or [None])[0]
    print(json.dumps(out,indent=2))

if __name__=="__main__":
    main()
