"""
Find a stable static->role-manager pointer path (read-only RPM).
1) locate hero by name, re-find the external heap objects that point to it (rolemgr candidates)
2) identify the real role manager (the candidate whose structure holds a role deque)
3) BFS up the referrer graph until a pointer slot lands in the image (=> static anchor)
Also validates equipment (+0xB88) and skill vector (+0x1918) using new ground truth.
"""
import ctypes as C
from ctypes import wintypes as W
import json, struct

k32=C.WinDLL("kernel32",use_last_error=True)
PVR=0x10; PQI=0x400; IMG=0x140000000; IMG_END=0x140000000+0x28C5000
NAME=b"Kinux"

class PE32(C.Structure):
    _fields_=[("dwSize",W.DWORD),("a",W.DWORD),("pid",W.DWORD),("b",C.c_void_p),
        ("c",W.DWORD),("d",W.DWORD),("e",W.DWORD),("f",C.c_long),("g",W.DWORD),("exe",C.c_char*260)]
class MBI(C.Structure):
    _fields_=[("Base",C.c_void_p),("Alloc",C.c_void_p),("AllocProt",W.DWORD),("a",W.DWORD),
        ("Size",C.c_size_t),("State",W.DWORD),("Protect",W.DWORD),("Type",W.DWORD)]

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

def regions(h):
    m=MBI(); a=0; out=[]
    while a<0x7FFFFFFFFFFF:
        if not k32.VirtualQueryEx(h,C.c_void_p(a),C.byref(m),C.sizeof(m)): break
        if m.State==0x1000 and m.Protect not in (0x01,0x100) and (m.Protect&0xFF):
            out.append((m.Base or 0,m.Size))
        a=(m.Base or 0)+m.Size
        if m.Size==0: break
    return out

def find_ptrs_to(r, regs, targets, cap_per=25):
    """Return dict target-> list of aligned slot addrs that hold that target value."""
    res={t:[] for t in targets}
    packed={struct.pack("<Q",t):t for t in targets}
    for base,size in regs:
        off=0
        while off<size:
            chunk=min(0x200000,size-off); b=r.rd(base+off,chunk)
            if b:
                for pk,t in packed.items():
                    if len(res[t])>=cap_per: continue
                    i=b.find(pk)
                    while i!=-1:
                        slot=base+off+i
                        if slot%8==0: res[t].append(slot)
                        if len(res[t])>=cap_per: break
                        i=b.find(pk,i+1)
            off+=chunk
    return res

def main():
    pid=pid_of()
    if not pid: print(json.dumps({"error":"not running"})); return
    r=R(pid); regs=regions(r.h); out={"pid":pid,"regions":len(regs)}

    # 1) hero by name
    hero=None
    for base,size in regs:
        off=0
        while off<size and not hero:
            chunk=min(0x200000,size-off); b=r.rd(base+off,chunk)
            if b:
                i=b.find(NAME+b"\x00")
                while i!=-1:
                    c=base+off+i-0x94
                    if (r.u32(c+0x68) or 0)>=1000000 and r.u32(c+0x3D0)==2419: hero=c; break
                    i=b.find(NAME+b"\x00",i+1)
            off+=chunk
        if hero: break
    out["hero"]=hex(hero) if hero else None
    if not hero: print(json.dumps(out,indent=2)); return

    # bonus validation: equipment slots @ +0xB88 (8 shared_ptr), skills vec @ +0x1918
    equip=[]
    for i in range(8):
        p=r.u64(hero+0xB88+i*8)
        equip.append(hex(p) if p else None)
    out["equip_slots@0xB88"]=equip
    # std::vector<PMagic> @ +0x1918 : {first,last,end}; count=(last-first)/8
    vf=r.u64(hero+0x1918); vl=r.u64(hero+0x1918+8)
    out["skills_vec@0x1918"]={"first":hex(vf or 0),"last":hex(vl or 0),
        "count":((vl-vf)//8) if (vf and vl and vl>=vf and vl-vf<0x1000) else "?"}
    # level candidate & exp scan within hero object
    blob=r.rd(hero,0x3800) or b""
    def offs(val,cap=8):
        o=[];i=0
        while True:
            j=blob.find(struct.pack("<I",val),i)
            if j==-1: break
            o.append(hex(j)); i=j+1
            if len(o)>=cap: break
        return o
    out["level_78_at"]=offs(78); out["silver_7681_at"]=offs(7681)

    # 2) rolemgr candidates = external heap objs pointing to hero (exclude self-refs inside hero)
    hitmap=find_ptrs_to(r,regs,[hero])
    cands=[s for s in hitmap[hero] if not (hero<=s<hero+0x3800)]
    out["rolemgr_candidates"]=[hex(x) for x in cands]

    # 3) BFS up to static image anchor
    path_found=None
    frontier=list(dict.fromkeys(cands))  # container base = slot addr (m_pHero@+0)
    seen=set(frontier); parents={x:None for x in frontier}
    for depth in range(1,4):
        # any frontier already in image?
        for f in frontier:
            if IMG<=f<IMG_END:
                path_found=(depth-1,f); break
        if path_found: break
        nxt=find_ptrs_to(r,regs,frontier,cap_per=6)
        newf=[]
        for tgt,slots in nxt.items():
            for s in slots:
                if s in seen: continue
                seen.add(s); parents[s]=tgt; newf.append(s)
                if IMG<=s<IMG_END and path_found is None:
                    path_found=(depth,s)
        if path_found: break
        frontier=newf[:40]
        if not frontier: break

    if path_found:
        depth,node=path_found
        chain=[]; cur=node
        while cur is not None:
            in_img = IMG<=cur<IMG_END
            chain.append({"addr":hex(cur),"in_image":in_img,"rva":hex(cur-IMG) if in_img else None})
            cur=parents.get(cur)
        out["static_path"]={"depth":depth,"chain":chain}
    else:
        out["static_path"]=None
        out["note"]="no static image pointer within depth 3; role mgr reached via deeper/other chain"
    print(json.dumps(out,indent=2))

if __name__=="__main__":
    main()
