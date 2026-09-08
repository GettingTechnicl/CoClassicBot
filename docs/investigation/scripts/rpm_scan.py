"""
Read-only memory scanner for ImConquer.exe. Locates the live hero object by
scanning for the known character name, then validates/re-derives struct offsets
against known ground-truth values. Injects nothing (external RPM only).
"""
import ctypes as C
from ctypes import wintypes as W
import json, struct

k32 = C.WinDLL("kernel32", use_last_error=True)
PROCESS_VM_READ = 0x0010; PROCESS_QUERY_INFORMATION = 0x0400

# ---- ground truth (provided by player) ----
NAME = b"Kinux"
SILVER = 7681
LEVEL  = 78
MAXHP  = 2419
BAGCNT = 1

class PROCESSENTRY32(C.Structure):
    _fields_ = [("dwSize",W.DWORD),("cntUsage",W.DWORD),("th32ProcessID",W.DWORD),
        ("th32DefaultHeapID",C.c_void_p),("th32ModuleID",W.DWORD),("cntThreads",W.DWORD),
        ("th32ParentProcessID",W.DWORD),("pcPriClassBase",C.c_long),("dwFlags",W.DWORD),
        ("szExeFile",C.c_char*260)]
class MEMORY_BASIC_INFORMATION(C.Structure):
    _fields_ = [("BaseAddress",C.c_void_p),("AllocationBase",C.c_void_p),
        ("AllocationProtect",W.DWORD),("__align",W.DWORD),("RegionSize",C.c_size_t),
        ("State",W.DWORD),("Protect",W.DWORD),("Type",W.DWORD)]

def find_pid(name=b"ImConquer.exe"):
    snap=k32.CreateToolhelp32Snapshot(0x2,0); pe=PROCESSENTRY32(); pe.dwSize=C.sizeof(pe); pid=None
    if k32.Process32First(snap,C.byref(pe)):
        while True:
            if pe.szExeFile.lower()==name.lower(): pid=pe.th32ProcessID; break
            if not k32.Process32Next(snap,C.byref(pe)): break
    k32.CloseHandle(snap); return pid

class R:
    def __init__(s,pid):
        s.h=k32.OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION,False,pid)
        if not s.h: raise OSError(f"OpenProcess {C.get_last_error()}")
    def read(s,a,n):
        if not a: return None
        buf=(C.c_ubyte*n)(); got=C.c_size_t(0)
        if not k32.ReadProcessMemory(s.h,C.c_void_p(a),buf,n,C.byref(got)): return None
        return bytes(buf[:got.value])
    def u32(s,a):
        b=s.read(a,4); return struct.unpack("<I",b)[0] if b and len(b)==4 else None
    def i32(s,a):
        b=s.read(a,4); return struct.unpack("<i",b)[0] if b and len(b)==4 else None
    def u64(s,a):
        b=s.read(a,8); return struct.unpack("<Q",b)[0] if b and len(b)==8 else None

def regions(h):
    mbi=MEMORY_BASIC_INFORMATION(); addr=0; out=[]
    while addr < 0x7FFFFFFFFFFF:
        if not k32.VirtualQueryEx(h,C.c_void_p(addr),C.byref(mbi),C.sizeof(mbi)): break
        # committed + readable, skip guard/noaccess
        if mbi.State==0x1000 and mbi.Protect not in (0x01,0x100) and (mbi.Protect & 0xFF):
            out.append((mbi.BaseAddress or 0, mbi.RegionSize, mbi.Protect, mbi.Type))
        addr=(mbi.BaseAddress or 0)+mbi.RegionSize
        if mbi.RegionSize==0: break
    return out

def scan_bytes(r, needle, cap_hits=40, max_scan=1<<31):
    hits=[]; scanned=0
    for base,size,prot,typ in regions(r.h):
        if scanned>max_scan: break
        # read in chunks
        off=0
        while off<size:
            chunk=min(0x100000, size-off)
            b=r.read(base+off,chunk)
            if b:
                i=b.find(needle)
                while i!=-1:
                    hits.append((base+off+i, prot, typ))
                    if len(hits)>=cap_hits: return hits
                    i=b.find(needle,i+1)
            scanned+=chunk; off+=chunk
    return hits

def main():
    pid=find_pid()
    if not pid: print(json.dumps({"error":"not running"})); return
    r=R(pid)
    out={"pid":pid}

    # 1) confirm stale ROLE_MGR
    base=0x140000000
    out["stale_check"]={"ROLE_MGR_deref@base+0x4DF588": hex(r.u64(base+0x4DF588) or 0)}

    # 2) find the hero object via the unique name
    name_hits=scan_bytes(r, NAME+b"\x00")
    out["name_hits"]=[]
    hero_candidates=[]
    for addr,prot,typ in name_hits:
        cand=addr-0x94   # m_szName @ +0x94 in current layout
        rec={"name_addr":hex(addr),"prot":hex(prot),"type":hex(typ),
             "as_role_base":hex(cand),
             "m_id@+0x68":r.u32(cand+0x68),
             "m_nMaxHp@+0x3D0":r.i32(cand+0x3D0),
             "silver@+0xA30(u64)":r.u64(cand+0xA30),
             "statTable@+0x968":hex(r.u64(cand+0x968) or 0)}
        # heuristic: player id in the millions AND maxhp matches
        rec["looks_like_hero"] = (rec["m_id@+0x68"] or 0) >= 1000000 and rec["m_nMaxHp@+0x3D0"]==MAXHP
        out["name_hits"].append(rec)
        if rec["looks_like_hero"]:
            hero_candidates.append(cand)

    # 3) for the confirmed hero, locate silver/level/exp by scanning the object window
    out["derived"]={}
    if hero_candidates:
        hero=hero_candidates[0]
        out["derived"]["hero_base"]=hex(hero)
        blob=r.read(hero, 0x3800) or b""
        def find_u32(val):
            res=[]; i=0
            while True:
                j=blob.find(struct.pack("<I",val), i)
                if j==-1: break
                res.append(hex(j)); i=j+1
            return res
        out["derived"]["offsets_of_SILVER_7681"]=find_u32(SILVER)
        out["derived"]["offsets_of_MAXHP_2419"]=find_u32(MAXHP)
        out["derived"]["offsets_of_LEVEL_78"]=find_u32(LEVEL)[:12]
        # also follow the stat table pointer and scan it
        st=r.u64(hero+0x968)
        if st:
            stblob=r.read(st,0x400) or b""
            def find_in(bl,val):
                res=[]; i=0
                while True:
                    j=bl.find(struct.pack("<I",val),i)
                    if j==-1: break
                    res.append(hex(j)); i=j+1
                return res
            out["derived"]["statTable_addr"]=hex(st)
            out["derived"]["statTable_LEVEL_78_at"]=find_in(stblob,LEVEL)[:12]
            out["derived"]["statTable_MAXHP_2419_at"]=find_in(stblob,MAXHP)
    else:
        out["derived"]["note"]="no hero candidate validated maxHP=2419 at +0x3D0 — CRole layout may also have shifted"

    print(json.dumps(out, indent=2))

if __name__=="__main__":
    main()
