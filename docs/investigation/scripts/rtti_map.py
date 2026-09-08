"""READ-ONLY. Proper MSVC x64 RTTI walk: map every vtable that carries a Complete Object Locator to its class name."""
import os, re, struct
SP=os.path.dirname(os.path.abspath(__file__)); d=open(os.path.join(SP,"image_dump.bin"),"rb").read()
N=len(d)
def u32(r): return struct.unpack_from("<I",d,r)[0] if 0<=r+4<=N else None
def cstr(r,n=100):
    e=d.find(b"\0",r,r+n); return d[r:e if e>=0 else r+n].decode("latin-1","ignore")
# 1) type descriptors: ".?AV...@@" ; TD struct = [vftable ptr(8)][spare(8)][name...], so name at TD+0x10
tds={}  # td_rva -> name
for m in re.finditer(rb"\.\?A[VU][^\x00]{2,80}@@\x00", d):
    name_rva=m.start(); td_rva=name_rva-0x10; tds[td_rva]=cstr(name_rva)
tdset=set(tds)
# 2) scan for COLs: u32[0]==1(sig), u32[3]=pTypeDesc(RVA in tds), u32[5]=pSelf(==this rva)
cols={}  # col_rva -> td_rva
for r in range(0, N-24, 4):
    if u32(r)!=1: continue
    ptd=u32(r+12); pself=u32(r+20)
    if pself==r and ptd in tdset:
        cols[r]=ptd
# 3) vtable that uses a COL: a qword in data == (col_rva) as RVA-stored? In x64 the vtable's
#    [-1] slot holds an ABSOLUTE VA (BASE+col_rva). Search for that 8-byte value; vtable=loc+8.
BASE=0x140000000
colvals={ (BASE+c):c for c in cols }
vt_name={}
for r in range(0, N-8, 8):
    v=struct.unpack_from("<Q",d,r)[0]
    if v in colvals:
        vt_name[r+8]=tds[cols[colvals[v]]]
print(f"type descriptors: {len(tds)} | COLs: {len(cols)} | vtables-with-RTTI: {len(vt_name)}\n")
targets={0x5CDB10:"VT_ROLE_B",0x5CFA70:"VT_MONSTER",0x5CEF60:"VT_HERO",0x5CDE68:"VT_OBJ_A",0x5CD928:"VT_OBJ_C",0x5CCB60:"CGameMap?"}
print("=== our target vtables ===")
for rva,label in targets.items():
    print(f"  {label:10s} 0x{rva:X}: {vt_name.get(rva,'<no RTTI on this vtable>')}")
print("\n=== all CRole/entity/object-ish class names found (any vtable) ===")
for vt in sorted(vt_name):
    nm=vt_name[vt]
    if re.search(r'\.\?AV(C?(Role|Monster|Hero|Npc|Player|Obj|MapObj|Entity|Screen|Ground|Effect|Item|Trap|Booth|Pet|Call|Guard|Dyna|Terrain|Magic|Region|Passage)|)', nm, re.I) and 'std' not in nm and 'protobuf' not in nm:
        print(f"  vtable 0x{vt-0:06X} -> {nm}")
