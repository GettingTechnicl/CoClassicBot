"""READ-ONLY. Disassemble the game bytes at the crash RVA 0x384215 from the decrypted image dump."""
import os, struct
SP = os.path.dirname(os.path.abspath(__file__))
p = os.path.join(SP, "image_dump.bin")
d = open(p, "rb").read()
rva = 0x384215
lo, hi = rva - 0x30, rva + 0x10
print(f"dump size 0x{len(d):X}; bytes around RVA 0x{rva:X}:")
for off in range(lo, hi, 16):
    c = d[off:off+16]
    print(f"  +0x{off:06X}: " + " ".join(f"{b:02X}" for b in c) + ("   <-- crash addr in this row" if off <= rva < off+16 else ""))
print("\nCD 29 (int 29h = __fastfail) within -0x30..+0x10 of crash addr?", "YES at RVA 0x%X" % (lo + d[lo:hi].find(b"\xCD\x29")) if d[lo:hi].find(b"\xCD\x29") >= 0 else "no")
try:
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    print("\ndisasm (starting a bit before the crash address):")
    for i in md.disasm(d[rva-0x28:rva+0x10], 0x140000000 + rva - 0x28):
        mark = "  <== CRASH" if i.address == 0x140000000 + rva else ""
        print(f"  0x{i.address:X}: {i.mnemonic} {i.op_str}{mark}")
except ImportError:
    print("(capstone not installed; hex only)")
