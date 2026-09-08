"""
Enumerate MSVC magic-static singleton accessors (GetInstance pattern) in the
decrypted image and the global pointer each returns, ranked by call count.
CNetClient::GetInstance / CGameWorld / CGameUI etc. fall out of this list.
Offline over image_dump.bin (file offset == RVA).
"""
from capstone import *
from capstone.x86 import *
import os, collections, json, bisect

HERE=os.path.dirname(os.path.abspath(__file__))
data=open(os.path.join(HERE,"image_dump.bin"),"rb").read()
TEXT_START=0x1000; TEXT_END=0x5511DF
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True

# --- pass 1: function starts (prologue after CC/ret padding) ---
PROL=[b"\x48\x89\x5c\x24",b"\x48\x89\x4c\x24",b"\x48\x89\x54\x24",b"\x48\x89\x44\x24",
      b"\x4c\x8b\xdc",b"\x48\x8b\xc4",b"\x40\x53",b"\x40\x55",b"\x40\x56",b"\x40\x57",
      b"\x48\x83\xec",b"\x48\x81\xec",b"\x55",b"\x53",b"\x56",b"\x57"]
starts=set()
for o in range(TEXT_START+1,TEXT_END):
    if (data[o-1]==0xCC or (data[o-1]==0xC3 and o%16==0)) and any(data.startswith(p,o) for p in PROL):
        starts.add(o)
ss=sorted(starts)
def fof(a):
    i=bisect.bisect_right(ss,a)-1
    return ss[i] if i>=0 else None

# --- pass 2: linear disasm w/ resync: record per-function insns, calls, rip-refs ---
callee_count=collections.Counter()
func_insns=collections.defaultdict(list)   # func_start -> [(addr,mnem,op,rip_target)]
off=TEXT_START
while off<TEXT_END:
    last=off
    for insn in md.disasm(data[off:TEXT_END],off):
        last=insn.address+insn.size
        f=fof(insn.address)
        rip=None
        if insn.id==X86_INS_CALL and insn.operands and insn.operands[0].type==X86_OP_IMM:
            callee_count[insn.operands[0].imm]+=1
        for op in insn.operands:
            if op.type==X86_OP_MEM and op.mem.base==X86_REG_RIP:
                rip=insn.address+insn.size+op.mem.disp
        if f is not None and len(func_insns[f])<40:
            func_insns[f].append((insn.address,insn.mnemonic,insn.op_str,rip))
    off=last if last>off else off+1

# --- pass 3: detect magic-static accessors ---
# signature: within first ~24 insns: read gs:[0x58] (TLS) AND a `cmp [rip+g], reg`
# guard; the function references a data global (>0x552000) that it returns.
def analyze(f):
    ins=func_insns[f]
    has_tls=any("gs:[0x58]" in (op or "") or "gs:[0x58]" in mn for _,mn,op,_ in ins)
    guard=None; globals_ref=[]
    for a,mn,op,rip in ins:
        if rip is not None and rip>=0x552000:
            globals_ref.append(rip)
            if mn=="cmp": guard=rip
    return has_tls, guard, globals_ref

accessors=[]
for f in ss:
    has_tls,guard,grefs=analyze(f)
    if has_tls and guard is not None:
        # the returned singleton global = a referenced global that's not the guard
        cands=[g for g in grefs if g!=guard]
        accessors.append({"func":hex(f),"calls":callee_count.get(f,0),
                          "guard":hex(guard),
                          "singleton_globals":[hex(g) for g in dict.fromkeys(cands)][:4]})
accessors.sort(key=lambda x:-x["calls"])
out={"num_func_starts":len(ss),"num_accessors":len(accessors),
     "top_accessors":accessors[:30]}
json.dump(out,open(os.path.join(HERE,"singleton_out.json"),"w"),indent=2)
print(json.dumps(out,indent=2))
