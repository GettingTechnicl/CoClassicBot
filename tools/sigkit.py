#!/usr/bin/env python3
"""
sigkit.py - cross-build-robust function / global / field fingerprints for x86-64 (MSVC) images.

Design rule (learned the hard way, see docs/investigation/SIGNATURE_METHODOLOGY.md): a signature that is
merely UNIQUE in one build is not a good signature; a good one SURVIVES A RECOMPILE. Growing a raw
byte run until it is unique makes it longer, more specific, and MORE brittle. So this library never
grows byte runs for uniqueness. It offers, instead, features that are stable when a function is
recompiled (different register allocation / instruction selection / operand encodings / addresses):

  exact_masked(L)   fixed-length masked bytes (call/jmp targets, rip-relative displacements and absolute
                    image pointers wildcarded). High precision when it hits; brittle across recompiles.
  shape             a normalised instruction stream: mnemonic + operand CLASSES (register SIZE not identity,
                    memory base class, conditional jumps collapsed, small immediates -> 'i'). Survives
                    register allocation and operand-encoding changes.
  imms              distinctive immediate constants (protocol ids, magic values, sizes) - survive recompiles.
  strings           strings referenced through rip-relative operands - survive recompiles.
  callees           call targets (later resolved to registry names / anchors) - survive when neighbours do.

Locating a function in another build cascades exact -> shape (exact, then fuzzy trigram similarity with a
margin over the runner-up) and always reports the METHOD and a SCORE, so a caller can tell a confident
relocation from a guess.

Images: load an unpacked PE file (DLL/EXE: sections mapped to their RVAs) or a flat decrypted dump.
"""
import re, struct, zlib
from collections import Counter, defaultdict
import capstone
from capstone import x86

IMAGE_BASE_LO, IMAGE_BASE_HI = 0x140000000, 0x150000000       # game image pointers (masked when absolute)
JCC = {"ja", "jae", "jb", "jbe", "jc", "je", "jz", "jg", "jge", "jl", "jle", "jna", "jnae", "jnb", "jnbe", "jnc",
       "jne", "jng", "jnge", "jnl", "jnle", "jno", "jnp", "jns", "jnz", "jo", "jp", "jpe", "jpo", "js", "jrcxz", "jecxz"}


# --------------------------------------------------------------------------------------- images
class Image:
    def __init__(self, data, sections, stamp=0, size_of_image=None, base=0x140000000, name=""):
        self.d = data
        self.sections = sections
        self.stamp = stamp
        self.size_of_image = size_of_image or len(data)
        self.base = base
        self.name = name
        self.code = [s for s in sections if (s["chars"] & 0x20) and s["rva"] < len(data)]

    @staticmethod
    def _parse_headers(b):
        e = struct.unpack_from("<I", b, 0x3C)[0]
        nsec = struct.unpack_from("<H", b, e + 6)[0]
        opt = struct.unpack_from("<H", b, e + 20)[0]
        stamp = struct.unpack_from("<I", b, e + 8)[0]
        soi = struct.unpack_from("<I", b, e + 24 + 56)[0]
        magic = struct.unpack_from("<H", b, e + 24)[0]
        base = struct.unpack_from("<Q", b, e + 24 + 24)[0] if magic == 0x20B else 0x140000000
        secs, off = [], e + 24 + opt
        for i in range(nsec):
            q = off + i * 40
            nm = b[q:q + 8].rstrip(b"\0").decode("latin1") or "?"
            vs, va, rs, rp = struct.unpack_from("<IIII", b, q + 8)
            ch = struct.unpack_from("<I", b, q + 36)[0]
            secs.append(dict(name=nm, rva=va, vsize=vs, rawsize=rs, rawptr=rp, chars=ch))
        return stamp, soi, base, secs

    @classmethod
    def from_pe_file(cls, path):
        b = open(path, "rb").read()
        stamp, soi, base, secs = cls._parse_headers(b)
        flat = bytearray(soi)
        flat[0:min(0x1000, len(b))] = b[:0x1000]
        for s in secs:
            n = min(s["rawsize"], s["vsize"] if s["vsize"] else s["rawsize"])
            flat[s["rva"]:s["rva"] + n] = b[s["rawptr"]:s["rawptr"] + n]
        return cls(bytes(flat), secs, stamp, soi, base, path)

    @classmethod
    def from_dump(cls, path):
        b = open(path, "rb").read()
        stamp, soi, base, secs = cls._parse_headers(b)
        return cls(b, secs, stamp, soi, base, path)

    def code_blobs(self):
        return [(s["rva"], self.d[s["rva"]:min(s["rva"] + s["vsize"], len(self.d))]) for s in self.code]

    def is_code(self, rva):
        return any(s["rva"] <= rva < s["rva"] + s["vsize"] for s in self.code)


# --------------------------------------------------------------------------------------- tokens
def _distinct(v):
    v &= 0xFFFFFFFFFFFFFFFF
    if v >= 1 << 63:
        v = (1 << 64) - v
    return v >= 0x1000 and not (IMAGE_BASE_LO <= v < IMAGE_BASE_HI)


def new_md():
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    md.skipdata = True
    return md


def mask_ranges(insn):
    """position-dependent byte ranges of an instruction (relative to its start)"""
    r = []
    if insn.id == 0:
        return r
    if insn.disp_size and insn.disp_offset is not None:
        for op in insn.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                r.append((insn.disp_offset, insn.disp_offset + insn.disp_size)); break
    if insn.imm_size and insn.imm_offset is not None:
        is_br = any(g in (capstone.CS_GRP_JUMP, capstone.CS_GRP_CALL) for g in insn.groups)
        for op in insn.operands:
            if op.type == x86.X86_OP_IMM:
                v = op.imm & 0xFFFFFFFFFFFFFFFF
                if is_br or IMAGE_BASE_LO <= v < IMAGE_BASE_HI:
                    r.append((insn.imm_offset, insn.imm_offset + insn.imm_size)); break
    return r


def token(insn):
    """normalised shape token: survives register allocation, operand encoding and address changes"""
    if insn.id == 0:
        return "?"
    m = insn.mnemonic
    if m in JCC:
        return "jcc"
    if m.startswith("rep") or m in ("nop", "int3", "ud2"):
        return m if m != "nop" else "nop"
    parts = []
    regs = []
    for op in insn.operands:
        if op.type == x86.X86_OP_REG:
            regs.append(op.reg); parts.append("R%d" % op.size)
        elif op.type == x86.X86_OP_IMM:
            if m in ("call", "jmp"):
                parts.append("rel")
            else:
                parts.append("I%x" % (op.imm & 0xFFFFFFFF) if _distinct(op.imm) else "i")
        elif op.type == x86.X86_OP_MEM:
            b = op.mem.base
            cls = "rip" if b == x86.X86_REG_RIP else ("sp" if b in (x86.X86_REG_RSP, x86.X86_REG_RBP) else ("x" if (b or op.mem.index) else "abs"))
            parts.append("M" + cls)
    if m in ("xor", "sub") and len(regs) == 2 and regs[0] == regs[1]:
        return "zero"
    if m in ("call", "jmp") and not parts:
        parts.append("ind")
    return m + " " + ",".join(parts)


# --------------------------------------------------------------------------------------- sweep -> functions
_TOKID = {}
_TOKNAME = []


def tokid(t):
    i = _TOKID.get(t)
    if i is None:
        i = len(_TOKNAME)
        _TOKID[t] = i
        _TOKNAME.append(t)
    return i


class Func:
    __slots__ = ("start", "ins", "end", "toks", "memacc", "refs", "calls", "n")

    def __init__(self, start):
        self.start = start
        self.ins = []      # PREFIX detail: (rva, size, token, masked_bytes(list), rip_target|None, call_target|None, distinct_imm|None)
        self.end = None
        # WHOLE-function data (cheap): normalised token ids, struct-field style accesses, data references, calls
        self.toks = []     # token id per instruction
        self.memacc = []   # (ins_index, ins_rva, disp) for non-rip memory operands with a non-trivial displacement
        self.refs = []     # (ins_index, ins_rva, target_rva) rip-relative references (target may be code or data)
        self.calls = []    # (ins_index, ins_rva, target_rva) direct calls
        self.n = 0


def _add(cur, insn, img, max_ins_per_func):
    """record one instruction into Func `cur` (whole-function lists + detailed prefix)"""
    rva = insn.address - img.base
    tk = token(insn)
    idx = cur.n
    cur.n += 1
    cur.toks.append(tokid(tk))
    if insn.id != 0:
        for op in insn.operands:
            if op.type == x86.X86_OP_MEM:
                if op.mem.base == x86.X86_REG_RIP:
                    cur.refs.append((idx, rva, insn.address + insn.size + op.mem.disp - img.base))
                elif op.mem.disp not in (0,) and abs(op.mem.disp) >= 0x20 and op.mem.base not in (x86.X86_REG_RSP, x86.X86_REG_RBP):
                    cur.memacc.append((idx, rva, op.mem.disp))
            elif op.type == x86.X86_OP_IMM and insn.mnemonic == "call":
                cur.calls.append((idx, rva, (op.imm & 0xFFFFFFFFFFFFFFFF) - img.base))
    if len(cur.ins) >= max_ins_per_func:
        return
    b = list(insn.bytes)
    for a_, z_ in mask_ranges(insn):
        for k in range(a_, min(z_, len(b))):
            b[k] = None
    rip = call = imm = None
    if insn.id != 0:
        for op in insn.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                rip = insn.address + insn.size + op.mem.disp - img.base
            elif op.type == x86.X86_OP_IMM:
                if insn.mnemonic == "call":
                    call = (op.imm & 0xFFFFFFFFFFFFFFFF) - img.base
                elif _distinct(op.imm) and insn.mnemonic not in ("jmp",) and insn.mnemonic not in JCC:
                    imm = op.imm & 0xFFFFFFFFFFFFFFFF
    cur.ins.append((rva, insn.size, tk, b, rip, call, imm))


def func_at(img, rva, max_bytes=0x1000, max_ins_per_func=64):
    """build a Func starting at an arbitrary RVA (registry entries need not sit on a detected function start)"""
    md = new_md()
    f = Func(rva)
    data = img.d[rva:rva + max_bytes]
    for insn in md.disasm(data, img.base + rva):
        if insn.id != 0 and insn.mnemonic == "int3":
            break
        _add(f, insn, img, max_ins_per_func)
        f.end = insn.address - img.base + insn.size
        if f.n >= 600:
            break
    if f.end is None:
        f.end = rva
    return f


def sweep(img, max_ins_per_func=64, align=None):
    """linear sweep of the code section(s); returns Funcs (prefix of up to max_ins_per_func instructions each).

    Function starts are taken after int3 padding at `align`-byte boundaries. align=None auto-selects: 16 (the MSVC
    /O2 convention), falling back to 1 when the density of detected functions is implausibly low (a build that does
    not 16-align functions, e.g. /O1 - found by the /O2-vs-/O1 stress test, docs/investigation/SIGNATURE_METHODOLOGY.md)."""
    if align is None:
        fs = _sweep(img, max_ins_per_func, 16)
        kb = max(1, sum(min(sec["vsize"], len(img.d) - sec["rva"]) for sec in img.code) // 1000)
        if len(fs) / kb < 1.2:
            f1 = _sweep(img, max_ins_per_func, 1)
            if len(f1) >= 1.5 * len(fs):
                return f1
        return fs
    return _sweep(img, max_ins_per_func, align)


def _sweep(img, max_ins_per_func, align):
    md = new_md()
    funcs = []
    cur = None
    prev_pad = True                    # previous instruction was padding (int3) => next real insn starts a function
    for sec in img.code:
        lo, hi = sec["rva"], min(sec["rva"] + sec["vsize"], len(img.d))
        data = img.d[lo:hi]
        for insn in md.disasm(data, img.base + lo):
            rva = insn.address - img.base
            if insn.id != 0 and insn.mnemonic == "int3":
                if cur is not None and cur.end is None:
                    cur.end = rva
                prev_pad = True
                continue
            if insn.id == 0 and insn.bytes[0] == 0xCC:
                prev_pad = True
                continue
            if prev_pad and (rva % align == 0 or cur is None):
                cur = Func(rva)
                funcs.append(cur)
            prev_pad = False
            if cur is None:
                continue
            _add(cur, insn, img, max_ins_per_func)
    for i, f in enumerate(funcs):
        if f.end is None:
            f.end = funcs[i + 1].start if i + 1 < len(funcs) else f.start + 0x100
    return funcs


# --------------------------------------------------------------------------------------- features
def exact_masked(func, L):
    out = []
    for (_, size, _t, b, *_r) in func.ins:
        if len(out) >= L:
            break
        out.extend(b)
    return out


def shape(func, n=40):
    return tuple(t for (_, _, t, *_r) in func.ins[:n])


def grams(sh, k=3):
    return set(zip(*[sh[i:] for i in range(k)])) if len(sh) >= k else set()


def imms(func):
    return {imm for (*_h, imm) in func.ins if imm is not None}


def strings_of(img, func, maxlen=48):
    """ASCII / UTF-16LE strings referenced by rip-relative operands of the function prefix"""
    out = set()
    for (_, _, _t, _b, rip, _c, _i) in func.ins:
        if rip is None or not (0 <= rip < len(img.d)) or img.is_code(rip):
            continue
        chunk = img.d[rip:rip + maxlen]
        m = re.match(rb"[\x20-\x7e]{5,}", chunk)
        if m:
            out.add(m.group(0).decode("ascii"))
        else:
            m = re.match(rb"(?:[\x20-\x7e]\x00){5,}", chunk)
            if m:
                out.add(m.group(0).decode("utf-16le"))
    return out


def callees(func):
    return [c for (*_h, c, _i) in func.ins if c is not None]


def to_hex(sig):
    return " ".join("??" if b is None else "%02X" % b for b in sig)


def sig_regex(sig):
    return re.compile(b"".join(b"." if b is None else re.escape(bytes([b])) for b in sig), re.DOTALL)


# --------------------------------------------------------------------------------------- matching
class Index:
    """everything needed to locate functions in ONE image"""

    def __init__(self, img, funcs=None):
        self.img = img
        self.funcs = funcs if funcs is not None else sweep(img)
        self.by_start = {f.start: f for f in self.funcs}
        self.idx_of = {f.start: i for i, f in enumerate(self.funcs)}
        self.blobs = img.code_blobs()
        self.shape = [shape(f) for f in self.funcs]
        self.grams = [grams(s) for s in self.shape]
        self.post = defaultdict(list)
        for i, g in enumerate(self.grams):
            for x in g:
                self.post[x].append(i)
        self.shape_exact = defaultdict(list)          # first-12-token prefix -> func indexes
        for i, s in enumerate(self.shape):
            if len(s) >= 12:
                self.shape_exact[s[:12]].append(i)

    def find_masked(self, sig, limit=8):
        rx = sig_regex(sig)
        hits = []
        for base, blob in self.blobs:
            for m in rx.finditer(blob):
                hits.append(base + m.start())
                if len(hits) >= limit:
                    return hits
        return hits

    def locate_exact(self, sig):
        hits = self.find_masked(sig)
        return hits

    def locate_shape(self, sh, imm_set=frozenset(), str_set=frozenset(), min_score=0.55, topn=8):
        """best structural match; returns (rva, score, margin, method) or None"""
        if len(sh) >= 12:
            ex = self.shape_exact.get(sh[:12], [])
            if len(ex) == 1:
                return self.funcs[ex[0]].start, 1.0, 1.0, "shape-exact"
        g = grams(sh)
        if not g:
            return None
        cnt = Counter()
        for x in g:
            p = self.post.get(x)
            if p and len(p) <= 60:
                for i in p:
                    cnt[i] += 1
        scored = []
        for i, _n in cnt.most_common(topn):
            gi = self.grams[i]
            jac = len(g & gi) / max(1, len(g | gi))
            sc = jac
            if imm_set:
                ii = imms(self.funcs[i])
                sc = 0.75 * jac + 0.25 * (len(imm_set & ii) / max(1, len(imm_set | ii)))
            scored.append((sc, i))
        if not scored:
            return None
        scored.sort(reverse=True)
        best = scored[0]
        second = scored[1][0] if len(scored) > 1 else 0.0
        if best[0] < min_score:
            return None
        return self.funcs[best[1]].start, best[0], best[0] - second, "shape-fuzzy"

    def best_other(self, f):
        """how distinctive is f? similarity (0..1) of the MOST similar OTHER function in this same image.
        High (>0.85) = there are near-identical siblings (template instantiations, magic-static accessors...)"""
        i = self.idx_of.get(f.start)
        if i is None:
            return (0.0, None)
        g = self.grams[i]
        cnt = Counter()
        for x in g:
            p = self.post.get(x)
            if p and len(p) <= 60:
                for j in p:
                    if j != i:
                        cnt[j] += 1
        best = (0.0, None)
        for j, _n in cnt.most_common(8):
            gj = self.grams[j]
            jac = len(g & gj) / max(1, len(g | gj))
            if jac > best[0]:
                best = (jac, self.funcs[j].start)
        return best

    def callers_of(self):
        """callee rva -> list of (caller Func, ordinal of that call among the caller's calls)"""
        out = defaultdict(list)
        for f in self.funcs:
            for k, (_i, _r, t) in enumerate(f.calls):
                out[t].append((f, k))
        return out
