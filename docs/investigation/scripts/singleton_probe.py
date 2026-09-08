"""
[LIVE] Probe the candidate singleton objects to identify CNetClient / game world / UI.
Reads singleton_out.json (top accessors), computes each singleton ptr = guard+8,
dereferences it, and dumps the object's vtable RVA + first qwords for identification.
Read-only external RPM. Run elevated. Writes singleton_probe_out.json.
"""
import ctypes as C
from ctypes import wintypes as W
import json, struct, os

k32=C.WinDLL("kernel32",use_last_error=True)
PVR=0x10; PQI=0x400; BASE=0x140000000; IMG_END=BASE+0x28C5000
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
    def u64(s,a):
        b=(C.c_ubyte*8)(); g=C.c_size_t(0)
        if not k32.ReadProcessMemory(s.h,C.c_void_p(a),b,8,C.byref(g)) or g.value<8: return None
        return struct.unpack("<Q",bytes(b))[0]

def main():
    pid=pid_of()
    if not pid: print(json.dumps({"error":"not running"})); return
    r=R(pid)
    sj=json.load(open(os.path.join(HERE,"singleton_out.json")))
    out={"pid":pid,"singletons":[]}
    for acc in sj["top_accessors"][:16]:
        guard=int(acc["guard"],16)
        ptr_rva=guard+8
        obj=r.u64(BASE+ptr_rva)
        rec={"accessor":acc["func"],"calls":acc["calls"],"singleton_ptr_rva":hex(ptr_rva),
             "obj":hex(obj or 0)}
        if obj and 0x10000<obj<0x7FFFFFFFFFFF:
            vt=r.u64(obj)
            rec["vtable_rva"]=hex(vt-BASE) if (vt and BASE<=vt<IMG_END) else hex(vt or 0)
            rec["qwords"]=[hex(r.u64(obj+i*8) or 0) for i in range(16)]
        out["singletons"].append(rec)
    json.dump(out,open(os.path.join(HERE,"singleton_probe_out.json"),"w"),indent=2)
    print("OK wrote singleton_probe_out.json for pid",pid)

if __name__=="__main__":
    main()
