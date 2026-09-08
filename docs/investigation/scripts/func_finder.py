"""
Offline function re-deriver for the decrypted ImConquer.exe dump (v1074).
- enumerates function starts (prologue after int3/ret padding)
- linear-disassembles .text, indexing RIP-relative data xrefs and call edges
- anchors on the VERIFIED role-manager pointer (RVA 0x69C730) to locate the
  hero/role accessor, then reports the call graph around it.
Runs on code_dump.bin (file offset == RVA). No game/elevation needed.
"""
from capstone import *
from capstone.x86 import *
import os, collections, json

HERE=os.path.dirname(os.path.abspath(__file__))
data=open(os.path.join(HERE,"code_dump.bin"),"rb").read()
TEXT_START=0x1000; TEXT_END=0x5511DF
ROLE_MGR_PTR_RVA=0x69C730   # [VERIFIED] *(base+this) -> CRoleMgr

md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True

# ---- 1) function-start detection -------------------------------------------
PROLOGUES=[b"\x48\x89\x5c\x24",b"\x48\x89\x4c\x24",b"\x48\x89\x54\x24",
           b"\x48\x89\x44\x24",b"\x4c\x8b\xdc",b"\x48\x8b\xc4",
           b"\x40\x53",b"\x40\x55",b"\x40\x56",b"\x40\x57",
           b"\x48\x83\xec",b"\x48\x81\xec",b"\x55",b"\x53",b"\x56",b"\x57"]
def is_prologue(o):
    return any(data.startswith(p, o) for p in PROLOGUES)
func_starts=set()
for o in range(TEXT_START+1, TEXT_END):
    # a function start is typically preceded by int3 padding (CC) or is 16-aligned after a ret(C3)
    prev=data[o-1]
    if (prev==0xCC or (prev==0xC3 and o%16==0)) and is_prologue(o):
        func_starts.add(o)
sorted_starts=sorted(func_starts)

def func_of(addr):
    # nearest preceding function start
    import bisect
    i=bisect.bisect_right(sorted_starts,addr)-1
    return sorted_starts[i] if i>=0 else None

# ---- 2) linear disasm with resync: xref + call-graph -----------------------
data_xref=collections.defaultdict(list)   # target_rva -> [ref_addr]
call_edges=collections.defaultdict(list)  # callee -> [caller_addr]
callee_count=collections.Counter()
ninsn=0
off=TEXT_START
while off<TEXT_END:
    last=off
    for insn in md.disasm(data[off:TEXT_END], off):
        ninsn+=1
        last=insn.address+insn.size
        if insn.id==X86_INS_CALL and len(insn.operands)==1 and insn.operands[0].type==X86_OP_IMM:
            tgt=insn.operands[0].imm
            call_edges[tgt].append(insn.address); callee_count[tgt]+=1
        for op in insn.operands:
            if op.type==X86_OP_MEM and op.mem.base==X86_REG_RIP:
                tgt=insn.address+insn.size+op.mem.disp
                data_xref[tgt].append(insn.address)
    off = last if last>off else off+1   # resync past a bad byte

out={}
out["stats"]={"func_starts":len(sorted_starts),"insns":ninsn,
              "unique_data_targets":len(data_xref),"unique_callees":len(callee_count)}

# ---- 3) anchor on role-mgr pointer -----------------------------------------
refs=data_xref.get(ROLE_MGR_PTR_RVA,[])
out["role_mgr_ptr_refs"]=[hex(a) for a in refs]
accessor_funcs=sorted(set(func_of(a) for a in refs if func_of(a)))
out["role_mgr_accessor_funcs"]=[hex(f) for f in accessor_funcs]

def disasm_head(start,n=10):
    res=[]
    for insn in md.disasm(data[start:start+80], start):
        res.append(f"0x{insn.address:X}: {insn.mnemonic} {insn.op_str}")
        if len(res)>=n: break
    return res
out["accessor_disasm"]={hex(f):disasm_head(f) for f in accessor_funcs}
# who calls the accessor(s)? (these are GetHero/role users across the bot's needs)
out["accessor_callers"]={}
for f in accessor_funcs:
    callers=sorted(set(func_of(a) for a in call_edges.get(f,[]) if func_of(a)))
    out["accessor_callers"][hex(f)]={"num_call_sites":len(call_edges.get(f,[])),
                                     "distinct_caller_funcs":len(callers)}

# ---- 4) hottest globals + hottest callees (singletons/utilities) -----------
data_hot=sorted(((t,len(v)) for t,v in data_xref.items() if t>=0x552000), key=lambda x:-x[1])[:15]
out["hottest_data_globals"]=[{"rva":hex(t),"refs":c} for t,c in data_hot]
call_hot=callee_count.most_common(15)
out["hottest_callees"]=[{"func":hex(t),"calls":c,"is_known_start":t in func_starts} for t,c in call_hot]

json.dump(out, open(os.path.join(HERE,"func_out.json"),"w"), indent=2)
print(json.dumps(out, indent=2))
