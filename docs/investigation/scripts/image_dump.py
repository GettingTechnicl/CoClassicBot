"""
Comprehensive read-only dump of the decrypted ImConquer.exe image
(base .. base+0x71C000 => .text + .rdata + .data; file offset == RVA).
Enables string-based + xref-based function identification offline.
Run elevated. Writes image_dump.bin next to this script.
"""
import ctypes as C
from ctypes import wintypes as W
import struct, os

k32=C.WinDLL("kernel32",use_last_error=True)
PVR=0x10; PQI=0x400; BASE=0x140000000
DUMP_LEN=0x71C000
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

def main():
    pid=pid_of()
    if not pid: print("ImConquer.exe not running"); return
    h=k32.OpenProcess(PVR|PQI,False,pid)
    if not h: print("OpenProcess failed",C.get_last_error()); return
    path=os.path.join(HERE,"image_dump.bin")
    got=0; miss=0; off=0
    with open(path,"wb") as f:
        while off<DUMP_LEN:
            chunk=min(0x100000,DUMP_LEN-off)
            buf=(C.c_ubyte*chunk)(); g=C.c_size_t(0)
            ok=k32.ReadProcessMemory(h,C.c_void_p(BASE+off),buf,chunk,C.byref(g))
            b=bytes(buf[:g.value]) if ok else b""
            if len(b)<chunk: b=b+b"\x00"*(chunk-len(b)); miss+=chunk-len(b)
            f.write(b); got+=len(b); off+=chunk
    print(f"OK pid={pid} wrote {path} len={hex(DUMP_LEN)} unreadable~{miss}")

if __name__=="__main__":
    main()
