"""READ-ONLY. Find callers of the ROLE_B ctor 0x15DB10 and any string refs in those functions."""
import os, re, struct
SP=os.path.dirname(os.path.abspath(__file__)); d=open(os.path.join(SP,"image_dump.bin"),"rb").read(); BASE=0x140000000
try:
    import capstone; md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
except ImportError: md=None
def callers(target):
    hits=[]
    for m in re.finditer(rb"\xe8", d[:0x400000]):
        p=m.start(); rel=struct.unpack_from("<i",d,p+1)[0]; 
        if p+5+rel==target: hits.append(p)
    return hits
def strings_in(lo,hi):
    """rip-relative lea targets in [lo,hi) that land on ascii."""
    out=[]
    for m in re.finditer(rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", d[lo:hi]):
        p=lo+m.start(); disp=struct.unpack_from("<i",d,p+3)[0]; t=p+7+disp
        if 0<=t<len(d):
            s=d[t:t+48]
            mm=re.match(rb"[\x20-\x7e]{4,}",s)
            if mm: out.append((p,t,mm.group().decode('latin-1')))
    return out
for ctor in (0x15DB10,):
    cs=callers(ctor)
    print(f"ROLE_B ctor 0x{ctor:X}: {len(cs)} direct call sites: {[hex(c) for c in cs]}")
    for c in cs[:6]:
        # crude function start: scan back for int3 padding / 'mov [rsp+..],reg' prologue
        fs=c
        for b in range(c, max(0,c-0x400), -1):
            if d[b-1]==0xCC and d[b] in (0x48,0x4C,0x40,0x53,0x55,0x56,0x57): fs=b; break
        print(f"\n  --- call at 0x{c:X} (func ~0x{fs:X}) ---")
        for p,t,s in strings_in(fs, c+0x40):
            print(f"      str ref @0x{p:X} -> \"{s}\"")
        if md:
            for i in md.disasm(d[max(0,c-0x30):c+0x8], BASE+max(0,c-0x30)):
                if i.mnemonic in ("mov","lea","call","cmp") : print(f"      0x{i.address-BASE:06X}: {i.mnemonic} {i.op_str}")
