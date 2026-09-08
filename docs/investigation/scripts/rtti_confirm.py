"""READ-ONLY. (1) Is RTTI present at all? (2) object size per vtable via its dtor's free(size)."""
import os, re, struct
SP=os.path.dirname(os.path.abspath(__file__)); d=open(os.path.join(SP,"image_dump.bin"),"rb").read()
BASE=0x140000000
# (1) RTTI type-descriptor strings look like ".?AV<name>@@" / ".?AU..."
avs=[m.start() for m in re.finditer(rb"\.\?A[VU][A-Za-z0-9_?@]{2,60}@@", d)]
print(f"RTTI type-descriptor strings ('.?AV..@@') in image: {len(avs)}")
for o in avs[:12]:
    e=d.find(b"\0",o,o+80); print("   ", d[o:e].decode('latin-1'))
print()
# (2) object size: a vtable's deleting-destructor is usually vtable slot 0; it ends with
#     'mov edx, imm32 ; call free' (B8/BA imm; E8 call). Read slot0, disasm tail for the size.
def q(r): return struct.unpack_from("<Q",d,r)[0]
def dtor_free_size(vt_rva):
    slot0=q(vt_rva)-BASE
    if not (0<=slot0<0x400000): return None
    win=d[slot0:slot0+0x120]
    # find 'mov edx, imm32' (BA xx xx xx xx) followed within a few bytes by E8 (call)
    best=None
    for m in re.finditer(rb"\xba(....)\xe8", win): best=struct.unpack_from("<I",win,m.start()+1)[0]
    # also 'mov edx, imm32' then 'mov rcx' then call
    if best is None:
        for m in re.finditer(rb"\xba(....)", win):
            v=struct.unpack_from("<I",win,m.start()+1)[0]
            if 0x20<=v<=0x4000: best=v
    return best
for name,rva in [("ROLE_B",0x5CDB10),("MONSTER",0x5CFA70),("HERO",0x5CEF60),("OBJ_A",0x5CDE68),("OBJ_C",0x5CD928)]:
    sz=dtor_free_size(rva)
    print(f"{name:8s} vtable 0x{rva:X}: slot0=0x{q(rva)-BASE:06X}  object size = {('0x%X (%d bytes)'%(sz,sz)) if sz else '??'}")
