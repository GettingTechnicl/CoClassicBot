from capstone import *
import os, bisect
HERE=os.path.dirname(os.path.abspath(__file__))
data=open(os.path.join(HERE,"image_dump.bin"),"rb").read()
TEXT_START=0x1000; TEXT_END=0x5511DF
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=False
PROL=[b"\x48\x89\x5c\x24",b"\x48\x89\x4c\x24",b"\x48\x89\x54\x24",b"\x48\x89\x44\x24",
      b"\x4c\x8b\xdc",b"\x48\x8b\xc4",b"\x40\x53",b"\x40\x55",b"\x40\x56",b"\x40\x57",
      b"\x48\x83\xec",b"\x48\x81\xec",b"\x55",b"\x53",b"\x56",b"\x57"]
starts=[]
for o in range(TEXT_START+1,TEXT_END):
    if (data[o-1]==0xCC or (data[o-1]==0xC3 and o%16==0)) and any(data.startswith(p,o) for p in PROL):
        starts.append(o)
def fof(a):
    i=bisect.bisect_right(starts,a)-1
    return starts[i] if i>=0 else None

hits=[0x1399D1,0x1A864D,0x1A870D]
for hv in hits:
    f=fof(hv)
    print(f"\n===== hit RVA 0x{hv:X}  ->  function 0x{f:X} =====")
    # disasm from function start, mark the write (instruction ending at hv, i.e. just before)
    for insn in md.disasm(data[f:f+0x400], f):
        mark = "  <== WRITE (trap RIP here)" if (insn.address <= hv < insn.address+insn.size) or (insn.address+insn.size==hv) else ""
        # show a window around the hit
        if f <= insn.address <= f+0x30 or abs(insn.address-hv)<0x40:
            print(f"  0x{insn.address:X}: {insn.mnemonic} {insn.op_str}{mark}")
        if insn.address> hv+0x30: break
