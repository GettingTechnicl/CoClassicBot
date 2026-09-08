"""READ-ONLY. Resolve MSVC RTTI class names for vtables in the decrypted image dump."""
import os, struct, sys
SP=os.path.dirname(os.path.abspath(__file__)); d=open(os.path.join(SP,"image_dump.bin"),"rb").read()
BASE=0x140000000
def q(rva): return struct.unpack_from("<Q",d,rva)[0] if 0<=rva+8<=len(d) else None
def i32(rva): return struct.unpack_from("<i",d,rva)[0] if 0<=rva+4<=len(d) else None
def u32(rva): return struct.unpack_from("<I",d,rva)[0] if 0<=rva+4<=len(d) else None
def cstr(rva,n=96):
    if rva is None or rva<0 or rva>=len(d): return None
    e=d.find(b"\0",rva,rva+n); return d[rva:e if e>=0 else rva+n].decode("latin-1","ignore")
def inimg(va): return va is not None and BASE<=va<BASE+len(d)
def describe(vt_rva):
    print(f"\n===== vtable RVA 0x{vt_rva:X} (VA 0x{BASE+vt_rva:X}) =====")
    # entries
    ents=[q(vt_rva+8*i) for i in range(6)]
    print("  first entries:", ", ".join(f"0x{e:X}" if e else "?" for e in ents), "| all into image:", all(inimg(e) for e in ents))
    col_va=q(vt_rva-8)
    if not inimg(col_va): print("  no RTTI locator pointer at vtable-8 (0x%X)"%(col_va or 0)); return
    col=col_va-BASE
    sig,off,cdoff,ptd,pchd,pself=u32(col),u32(col+4),u32(col+8),u32(col+12),u32(col+16),u32(col+20)
    print(f"  COL @0x{col:X}: sig={sig} offset={off} cdOffset={cdoff} pTypeDesc=0x{ptd:X} pClassHier=0x{pchd:X} pSelf=0x{pself:X} (self ok: {pself==col})")
    if sig!=1: print("  (unexpected signature; x64 expects 1)"); 
    name=cstr(ptd+16)
    print(f"  CLASS NAME: {name}")
    # class hierarchy -> base classes
    if pchd and pchd<len(d):
        nb=u32(pchd+8); pbca=u32(pchd+12)
        bases=[]
        for k in range(min(nb or 0,8)):
            bcd=u32(pbca+4*k)
            if bcd and bcd<len(d):
                td=u32(bcd); bases.append(cstr(td+16))
        print(f"  hierarchy ({nb} bases, order most-derived first): {bases}")
for a in sys.argv[1:]: describe(int(a,16))
