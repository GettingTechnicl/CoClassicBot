"""READ-ONLY. Find constructors that store each vtable (lea rax,[rip+vt]; mov [rcx],rax) and the preceding operator new size."""
import os, re, struct
SP=os.path.dirname(os.path.abspath(__file__)); d=open(os.path.join(SP,"image_dump.bin"),"rb").read()
BASE=0x140000000
try:
    import capstone; md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); md.detail=True
except ImportError: md=None
def find_lea_refs(target_rva):
    """scan code for 'lea r64,[rip+disp32]' (48/4C 8D xx disp32) whose target == target_rva."""
    hits=[]
    for m in re.finditer(rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", d[:0x400000]):
        p=m.start(); disp=struct.unpack_from("<i",d,p+3)[0]; tgt=p+7+disp
        if tgt==target_rva: hits.append(p)
    return hits
def ctx(p, back=0x60, fwd=0x30):
    if not md: return "(capstone missing)"
    s=max(0,p-back); lines=[]
    for i in md.disasm(d[s:p+fwd], BASE+s):
        mark=" <== lea vtable" if i.address==BASE+p else ""
        lines.append(f"      0x{i.address-BASE:06X}: {i.mnemonic} {i.op_str}{mark}")
    return "\n".join(lines)
for name,rva in [("ROLE_B",0x5CDB10),("MONSTER",0x5CFA70),("HERO",0x5CEF60)]:
    hits=find_lea_refs(rva)
    print(f"\n===== {name} vtable 0x{rva:X}: {len(hits)} lea-references in code: {[hex(h) for h in hits]} =====")
    for h in hits[:3]:
        print(f"  --- ref at RVA 0x{h:X} ---"); print(ctx(h))
        # look backwards up to 0x200 for 'mov ecx, imm32 ; call' pattern (operator new size)
        win=d[max(0,h-0x200):h]
        sizes=[struct.unpack_from("<I",win,m.start()+1)[0] for m in re.finditer(rb"\xb9(....)\xe8", win)]
        if sizes: print(f"  candidate 'mov ecx,imm32; call' (operator new size) within 0x200 before: {[hex(s) for s in sizes]}")
