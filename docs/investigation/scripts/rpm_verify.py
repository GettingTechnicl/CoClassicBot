"""
Read-only offset verifier for ImConquer.exe (Themida-protected).
Uses external ReadProcessMemory ONLY — injects nothing into the game process.
Verifies the bot's memory-offset assumptions against the live decrypted image.
"""
import ctypes as C
from ctypes import wintypes as W
import json, sys

k32 = C.WinDLL("kernel32", use_last_error=True)

TH32CS_SNAPMODULE   = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
PROCESS_VM_READ            = 0x0010
PROCESS_QUERY_INFORMATION  = 0x0400

class MODULEENTRY32(C.Structure):
    _fields_ = [("dwSize", W.DWORD), ("th32ModuleID", W.DWORD),
                ("th32ProcessID", W.DWORD), ("GlblcntUsage", W.DWORD),
                ("ProccntUsage", W.DWORD), ("modBaseAddr", C.POINTER(C.c_byte)),
                ("modBaseSize", W.DWORD), ("hModule", W.HMODULE),
                ("szModule", C.c_char * 256), ("szExePath", C.c_char * 260)]

class PROCESSENTRY32(C.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD),
                ("th32ProcessID", W.DWORD), ("th32DefaultHeapID", C.c_void_p),
                ("th32ModuleID", W.DWORD), ("cntThreads", W.DWORD),
                ("th32ParentProcessID", W.DWORD), ("pcPriClassBase", C.c_long),
                ("dwFlags", W.DWORD), ("szExeFile", C.c_char * 260)]

def find_pid(name=b"ImConquer.exe"):
    TH32CS_SNAPPROCESS = 0x2
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32(); pe.dwSize = C.sizeof(pe)
    pid = None
    if k32.Process32First(snap, C.byref(pe)):
        while True:
            if pe.szExeFile.lower() == name.lower():
                pid = pe.th32ProcessID; break
            if not k32.Process32Next(snap, C.byref(pe)): break
    k32.CloseHandle(snap)
    return pid

def module_base(pid, name=b"ImConquer.exe"):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    me = MODULEENTRY32(); me.dwSize = C.sizeof(me)
    base = None
    if k32.Module32First(snap, C.byref(me)):
        while True:
            if me.szModule.lower() == name.lower():
                base = C.cast(me.modBaseAddr, C.c_void_p).value; break
            if not k32.Module32Next(snap, C.byref(me)): break
    k32.CloseHandle(snap)
    return base

class Reader:
    def __init__(self, pid):
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
        if not self.h: raise OSError(f"OpenProcess failed {C.get_last_error()}")
    def read(self, addr, size):
        if not addr: return None
        buf = (C.c_ubyte * size)(); got = C.c_size_t(0)
        ok = k32.ReadProcessMemory(self.h, C.c_void_p(addr), buf, size, C.byref(got))
        if not ok: return None
        return bytes(buf[:got.value])
    def u64(self, a):
        b = self.read(a, 8); return int.from_bytes(b, "little") if b else None
    def u32(self, a):
        b = self.read(a, 4); return int.from_bytes(b, "little") if b else None
    def i32(self, a):
        b = self.read(a, 4); return int.from_bytes(b, "little", signed=True) if b else None
    def cstr(self, a, n):
        b = self.read(a, n);
        if not b: return None
        return b.split(b"\x00")[0].decode("latin1", "replace")

def main():
    pid = find_pid()
    if not pid: print(json.dumps({"error": "ImConquer.exe not running"})); return
    base = module_base(pid)
    if not base: print(json.dumps({"error": "module base not found", "pid": pid})); return
    r = Reader(pid)
    out = {"pid": pid, "base": hex(base), "aslr_shift": hex(base - 0x140000000)}

    ROLE_MGR = base + 0x4DF588
    hero = r.u64(ROLE_MGR + 0x00)          # CRoleMgr::m_pHero @ +0x00
    out["role_mgr_addr"] = hex(ROLE_MGR)
    out["hero_ptr"] = hex(hero) if hero else None
    if hero:
        out["hero"] = {
            "m_id@0x68":        r.u32(hero + 0x68),
            "m_szName@0x94":    r.cstr(hero + 0x94, 16),
            "m_posMap@0xD8":    [r.i32(hero + 0xD8), r.i32(hero + 0xDC)],
            "m_nMaxHp@0x3D0":   r.i32(hero + 0x3D0),
            "m_nStamina@0x6E0": r.i32(hero + 0x6E0),
            "silver_cand@0xA30(u64)": r.u64(hero + 0xA30),
            "statTable@0x968":  hex(r.u64(hero + 0x968) or 0),
            "deq_item_size@0xB38": r.u64(hero + 0xB20 + 0x18),  # MSVC deque _Mysize
            "m_nMaxMana@0xCA8": r.i32(hero + 0xCA8),
            "m_bVip@0x3740":    r.u32(hero + 0x3740),
        }
    # role manager deque size (nearby entities)
    out["role_deque_size@0x70+0x18"] = r.u64(ROLE_MGR + 0x70 + 0x18)
    print(json.dumps(out, indent=2))

if __name__ == "__main__":
    main()
