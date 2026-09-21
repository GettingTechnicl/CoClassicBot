#!/usr/bin/env python3
"""
build_offset_registry.py - generate the v1074 offset registry (the intact "old side" of a
client-version diff) from src/game.h + a decrypted image dump of the client.

  python tools/build_offset_registry.py [--image <image_dump.bin>] [--out-dir docs/investigation]

Outputs (checked into the repo so the diff never depends on chat/memory):
  v1074_offset_registry.json   machine-readable (consumed by tools/sigscan.py)
  V1074_OFFSET_REGISTRY.md     human-readable, grouped by verification status

What it records, per entry:
  * CODE RVAs (functions):   a MASKED byte signature of the function start (rel32 branch targets,
                             rip-relative displacements and absolute pointers wildcarded), grown until it
                             is unique in the code section. Locates the same function in a new build.
  * DATA RVAs (globals):     signatures of up to 5 code sites that reference the global via a rip-relative
                             operand, with the displacement position, so a matcher can recover the global's
                             NEW address from wherever the same code now lives (the robust way to relocate a
                             global; the address itself is expected to move).
  * STRUCT-FIELD offsets:    extracted from `// +0xNNN` comments / static_assert(offsetof) in the struct
                             headers, with the verification tag found on the line. These are data-model
                             facts; they are re-verified at runtime (self-test), not by byte signature.
  * status:                  taken from the source's own tags - verified / stale / wrong / garbage /
                             unverified / stale_default (a GameRva constant with no tag of its own inherits
                             the block-level 'CONFIRMED STALE - pre-v1074 dump' verdict in game.h).

The image must be a dump of the SAME build the constants were verified against; the tool records
the image's PE TimeDateStamp / SizeOfImage so provenance travels with the registry.
"""
import argparse, json, os, re, struct, sys, hashlib
import capstone
from capstone import x86

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Verified items that live in comments/notes rather than as constexpr constants in game.h.
# Every entry cites where the claim comes from; nothing here is invented.
SUPPLEMENT = [
    dict(name="CNETCLIENT_POLLER_1DD860", rva=0x1DD860, kind="code", status="verified",
         note="per-tick outbound poller that drains the pending-send queue to real send(); plain code, NOT hook-resistant "
              "(game.h SEND_MSG_REAL comment; flushfinder retirement note in CONNECTION_DECIPHER_PREP.md)"),
    dict(name="CONN_STATIC_69A4E8", rva=0x69A4E8, kind="data", status="verified",
         note="static storage returned by CNETCLIENT_CONNECTION_SINGLETON (0xB7320: `lea rax,[rip+0x5e3144]`); "
              "connection object = *(u64*)(base+0x69A4E8+0x20). Derived 2026-09-20 by decoding the accessor in image_dump.bin"),
    dict(name="MAP_SCENE_PTR_699370", rva=0x699370, kind="data", status="verified",
         note="heap object that tracks the active map (vtable not in image; a real CGameMap vtable is 0x5CCB60) - REVAMP_PLAN session 9"),
    dict(name="MAP_GLOBAL_6993B8", rva=0x6993B8, kind="data", status="unverified",
         note="'repeated 0x10000, not a map' - REVAMP_PLAN session 9; listed so a diff can see whether it moved"),
    dict(name="CGAMEMAP_VTABLE_5CCB60", rva=0x5CCB60, kind="data", status="verified",
         note="genuine CGameMap vtable RVA; map descriptor id at +0x10 (memory live-offsets, map-id verification technique)"),
    dict(name="HERO_ROLEMGR_ACCESSOR_181B30", rva=0x181B30, kind="code", status="verified",
         note="hero/rolemgr singleton accessor foothold found in session 3 (REVAMP_PLAN)"),
    dict(name="GETTIMESTAMP_C6A80", rva=0xC6A80, kind="code", status="stale",
         note="used inline in CHero.cpp/packets.cpp; game.h: resolves to an epilogue on v1074"),
    dict(name="COROUTINE_E9960", rva=0xE9960, kind="code", status="verified",
         note="HOOK-RESISTANT region (anti-tamper); DO NOT hook/call - calling 0xE9960(realNetClient) spawned a duplicate game instance (memory workflow-feedback)"),
    dict(name="COROUTINE_BF920", rva=0xBF920, kind="code", status="verified",
         note="HOOK-RESISTANT region (anti-tamper); DO NOT hook/call (same finding as 0xE9960)"),
]


# Explicit, reviewed verdicts. Tag-scraping game.h is only a first pass: a single word ("WRONG", "garbage")
# in a neighbouring comment can poison an entry, and several constants sit directly under another
# constant's comment block. Each line below was checked against the comment text in game.h /
# the session notes (memory live-offsets, REVAMP_PLAN) and cites why. `status_source` in the output
# says whether a status came from a tag or from this table.
OVERRIDES = {
    "ROLE_MGR_PTR":        ("verified",   "game.h:20 [LIVE-VERIFIED]; roleMgr = *(base+0x69C730), hero = *roleMgr"),
    "ROLE_MGR":            ("stale",      "game.h:21 code now lives where the old static object was"),
    "ENTITY_INFO":         ("unverified", "game.h:23 [UNVERIFIED v1074]"),
    "ENTITY_SET":          ("unverified", "game.h:24 [UNVERIFIED v1074]"),
    "GAME_MAP":            ("garbage",    "game.h:25 [CONFIRMED GARBAGE] live probe dereferenced it to unreadable memory"),
    "CURRENT_MAP_ID":      ("verified",   "game.h:27-42 four-map differential (Twin City/Desert/Market/Phoenix) + session-6 independent match; Game::GetCurrentMapId() works live"),
    "CURRENT_MAP_ID_ALT":  ("verified",   "game.h:42 the adjacent surviving global of the same differential; used as the fallback read"),
    "ROLE_SET_ROLES":      ("unverified", "struct offset inside the role-set object; registries.cpp logs 'role-set object not found' on the live client and falls back to heap scan (tonight's logs) - do NOT treat as verified"),
    "ROLE_SET_ALL":        ("unverified", "same as ROLE_SET_ROLES"),
    "ROLE_MGR_TO_ROLESET_HOP1": ("unverified", "heapfind v7 path; live logs show the chain does not resolve reliably; heap-scan fallback is what actually runs"),
    "ROLE_MGR_TO_ROLESET_HOP2": ("unverified", "same as HOP1"),
    "MAP_ITEM_VEC":        ("verified",   "game.h:62 GLOBAL vector<shared_ptr<CMapItem>>; memory 'registries': confirmed working live (replaced the item heap-scan)"),
    "CENTITY_RENDER_VISUAL": ("stale",    "game.h:66-78 block verdict: inherited pre-v1074 RVA, lands on non-prologue code; Detouring it corrupts the client"),
    "CNETCLIENT_GET_INSTANCE": ("stale",  "game.h:66-78 block verdict; 0x0B9490 does not disassemble as the real accessor"),
    "CNETCLIENT_SEND_MSG":   ("garbage",  "game.h:66-78; superseded by CNETCLIENT_SEND_MSG_REAL (0x1DD450)"),
    "CHERO_GET_MAX_HP":      ("garbage",  "game.h:71 'CSTATTABLE_GET_VALUE/CHERO_GET_MAX_HP to misaligned garbage'; max HP is read as the direct field CRole+0x3D0"),
    "CHERO_GET_MAX_MANA":    ("stale",    "game.h:66-78 block verdict"),
    "CSTATTABLE_GET_VALUE":  ("garbage",  "game.h:71 misaligned garbage; current HP is read via m_pStatTable+0x10 -> +0x0 instead (docs/investigation/CURRENT_HP_READ_INVESTIGATION.md)"),
    "CHERO_GET_CURRENT_MANA": ("stale",   "game.h:66-78 block verdict; mana has the same dead-accessor bug as HP (not yet fixed)"),
    "CHERO_FATAL_STRIKE":    ("garbage",  "game.h:70 'CHERO_FATAL_STRIKE to padding'"),
    "CHERO_WALK":            ("stale",    "game.h:69 'CHERO_WALK resolves mid-function'; movement is done by writing SyncClientPosition fields instead"),
    "CHERO_JUMP":            ("stale",    "game.h:66-78 block verdict; jumps are sent as packets"),
    "CHERO_START_MINING":    ("stale",    "game.h:66-78 block verdict"),
    "MSGUPDATE_PROCESS":     ("stale",    "game.h:66-78 block verdict (was Detoured at init; now gated off by VERIFIED_V1074)"),
    "TRADEWINDOW_HANDLE_MESSAGE": ("stale", "game.h:66-78 block verdict"),
    "CGAMEUI_SHOW_MSG":      ("stale",    "game.h:66-78 block verdict"),
    "CNETCLIENT_SINGLETON_ACCESSOR": ("wrong", "game.h:119-134 CONFIRMED WRONG: resolves to an unrelated (diagnostics-like) object; never use as the net-client 'this'"),
    "CNETCLIENT_SEND_MAPITEM_MSG": ("verified", "game.h:106-118 [LIVE-TRACED via Frida]: a real function, but DO NOT CALL cold - it derefs netClient+0x20 with no null check (memory live-offsets session 7); the bot builds packets itself and uses SEND_MSG_REAL"),
    "CNETCLIENT_COMPUTE_MAPITEM_SIZE": ("verified", "game.h:137-173 [LIVE-TRACED session 7]; byte-exact payload size confirmed"),
    "CNETCLIENT_BEGIN_MSG":  ("verified", "game.h:137-173 [LIVE-TRACED session 7] allocates the output buffer"),
    "CNETCLIENT_COMMIT_STAGING": ("verified", "game.h:137-173 [LIVE-TRACED session 7]; plaintext hand-off point"),
    "MSGMAPITEM_VTABLE":     ("verified", "game.h:179 staging-struct vtable, live-captured 8/8 identical across real pickups"),
    "CNETCLIENT_CONNECTION_SINGLETON": ("verified", "game.h:196-214 [LIVE-CONFIRMED session 8] genuine MSVC magic-static returning &(base+0x69A4E8); connection object = *(that+0x20)"),
    "CNETCLIENT_SEND_MSG_REAL": ("verified", "game.h:196-215 [LIVE-CONFIRMED END TO END session 8]: real CNetClient::SendMsg(conn, buf, len); every packet action goes through it"),
    "CROLE_SET_COMMAND_REAL": ("verified", "game.h:216-232 [LIVE-CONFIRMED session 9]: the real CRole::SetCommand (a direct call, not vtable slot 59)"),
    "CRole_SetCommand":      ("wrong",    "game.h:236-238 vtable slot 59 CONFIRMED WRONG (resolves to a stub `mov eax,0xCD764F08; ret`)"),
}

# Globals that have no rip-relative code reference of their own because code reaches them as sibling+delta.
DERIVED = {"CURRENT_MAP_ID_ALT": ("CURRENT_MAP_ID", 4)}

STRUCT_HEADERS = ["CRole.h", "CHero.h", "CItem.h", "CGameMap.h", "CMapObj.h", "CMagic.h", "CEntityInfo.h", "CEntitySet.h"]


# ----------------------------------------------------------------------------- image
class Image:
    def __init__(self, path):
        self.path = path
        self.d = open(path, "rb").read()
        e = struct.unpack_from("<I", self.d, 0x3C)[0]
        self.stamp = struct.unpack_from("<I", self.d, e + 8)[0]
        nsec = struct.unpack_from("<H", self.d, e + 6)[0]
        opt = struct.unpack_from("<H", self.d, e + 20)[0]
        self.size_of_image = struct.unpack_from("<I", self.d, e + 24 + 56)[0]
        self.sections = []
        off = e + 24 + opt
        for i in range(nsec):
            nm = self.d[off + i * 40:off + i * 40 + 8].rstrip(b"\0").decode("latin1") or "?"
            vs, va = struct.unpack_from("<II", self.d, off + i * 40 + 8)
            ch = struct.unpack_from("<I", self.d, off + i * 40 + 36)[0]
            self.sections.append(dict(name=nm, rva=va, vsize=vs, chars=ch))
        # the executable section(s) actually present in the dump
        self.code = [s for s in self.sections if (s["chars"] & 0x20) and s["rva"] < len(self.d)]

    def in_dump(self, rva):
        return 0 <= rva < len(self.d)

    def section_of(self, rva):
        for s in self.sections:
            if s["rva"] <= rva < s["rva"] + max(s["vsize"], 1):
                return s
        return None

    def is_code(self, rva):
        return any(s["rva"] <= rva < s["rva"] + s["vsize"] for s in self.code)


# ----------------------------------------------------------------------------- masking
def new_md():
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    md.skipdata = True
    return md


def mask_ranges(insn):
    """byte ranges (relative to insn start) that are position-dependent and must be wildcarded"""
    r = []
    if insn.id == 0:                       # skipdata pseudo-instruction carries no operand detail
        return r
    if insn.disp_size and insn.disp_offset is not None:
        # only rip-relative displacements move with the image layout
        for op in insn.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                r.append((insn.disp_offset, insn.disp_offset + insn.disp_size)); break
    is_branch = any(g in (capstone.CS_GRP_JUMP, capstone.CS_GRP_CALL) for g in insn.groups)
    if insn.imm_size and insn.imm_offset is not None and (is_branch or True):
        for op in insn.operands:
            if op.type == x86.X86_OP_IMM:
                v = op.imm & 0xFFFFFFFFFFFFFFFF
                if is_branch or 0x140000000 <= v < 0x150000000:   # rel branch target or absolute image pointer
                    r.append((insn.imm_offset, insn.imm_offset + insn.imm_size)); break
    return r


def masked_bytes(md, data, base_va, max_len, min_len=24):
    """disassemble data (function start) and return (list of int|None, insn_count, decode_ok)"""
    out, count = [], 0
    pos = 0
    for insn in md.disasm(data[:max_len + 16], base_va):
        if insn.mnemonic == ".byte" or insn.id == 0:      # skipdata pseudo-instruction: undecodable
            break
        if pos + insn.size > max_len and pos >= min_len:
            break
        b = list(insn.bytes)
        for lo, hi in mask_ranges(insn):
            for k in range(lo, min(hi, len(b))):
                b[k] = None
        out.extend(b); pos += insn.size; count += 1
        if pos >= max_len:
            break
    return out, count


def to_hex(sig):
    return " ".join("??" if b is None else "%02X" % b for b in sig)


def sig_regex(sig):
    return re.compile(b"".join(b"." if b is None else re.escape(bytes([b])) for b in sig), re.DOTALL)


# ----------------------------------------------------------------------------- game.h
TAG_RE = [
    ("garbage", re.compile(r"GARBAGE", re.I)),
    ("wrong", re.compile(r"\bWRONG\b", re.I)),
    ("stale", re.compile(r"\bSTALE\b", re.I)),
    ("verified", re.compile(r"LIVE-?(VERIFIED|CONFIRMED|TRACED)", re.I)),
    ("unverified", re.compile(r"UNVERIFIED", re.I)),
]


def classify(text, default):
    for status, rx in TAG_RE:
        if rx.search(text):
            return status
    return default


def parse_game_h(path):
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    ns = None
    out = []
    rx = re.compile(r"^\s*constexpr\s+(uintptr_t|size_t)\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;\s*(//.*)?$")
    for i, ln in enumerate(lines):
        m = re.match(r"^\s*(?:namespace|class)\s+(\w+)", ln)
        if m:
            ns = m.group(1)
        m = rx.match(ln)
        if not m:
            continue
        typ, name, val, trail = m.groups()
        # contiguous comment block directly above
        block, j = [], i - 1
        while j >= 0 and lines[j].strip().startswith("//"):
            block.append(lines[j].strip()[2:].strip()); j -= 1
        block.reverse()
        own = " ".join(block) + " " + (trail or "")
        # A tag belongs to the entry only if it is on its own line or in the comment block that
        # ends directly above it AND no other constexpr sits between (guaranteed by 'contiguous').
        default = {"GameRva": "stale_default", "Offsets": "unverified", "GameVtableIndex": "unverified"}.get(ns, "unverified")
        out.append(dict(name=name, ns=ns, type=typ, value=int(val, 0), line=i + 1,
                        evidence=re.sub(r"\s+", " ", own).strip(), status=classify(own, default)))
    return out


# ----------------------------------------------------------------------------- struct fields
FIELD_RE = re.compile(r"^\s*(?P<decl>[^/;{}]*?(\b\w+)(?:\[[^\]]*\])?)\s*;\s*//\s*(?P<c>.*\+0x(?P<off>[0-9A-Fa-f]+).*)$")
ASSERT_RE = re.compile(r"static_assert\s*\(\s*offsetof\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)\s*==\s*(0x[0-9A-Fa-f]+)")


def parse_struct_fields(src_dir):
    out = []
    for h in STRUCT_HEADERS:
        p = os.path.join(src_dir, h)
        if not os.path.exists(p):
            continue
        for i, ln in enumerate(open(p, encoding="utf-8", errors="replace").read().split("\n")):
            m = FIELD_RE.match(ln)
            if m:
                name = re.findall(r"(\w+)(?:\[[^\]]*\])?\s*$", m.group("decl").strip())
                out.append(dict(file=h, line=i + 1, field=name[-1] if name else "?", offset=int(m.group("off"), 16),
                                comment=m.group("c").strip(), status=classify(m.group("c"), "unverified"), asserted=False))
            a = ASSERT_RE.search(ln)
            if a:
                out.append(dict(file=h, line=i + 1, field="%s::%s" % (a.group(1), a.group(2)), offset=int(a.group(3), 16),
                                comment="static_assert(offsetof)", status="verified", asserted=True))
    return out


# ----------------------------------------------------------------------------- build
def build(img, entries, code_bytes, code_base):
    md = new_md()
    # ---- code signatures
    for e in entries:
        if e["kind"] != "code":
            continue
        rva = e["rva"]
        if not img.in_dump(rva) or not img.is_code(rva):
            e["sig"] = None; e["sig_note"] = "not in the dumped code section"; continue
        prev = img.d[rva - 1] if rva else 0
        e["at_function_start"] = prev in (0xCC, 0xC3, 0x90, 0x00) or rva % 16 == 0
        sig = None
        for L in (32, 48, 64, 96, 128):
            s, n = masked_bytes(md, img.d[rva:rva + L + 16], img.stamp * 0 + 0x140000000 + rva, L)
            if n == 0:
                break
            hits = list(sig_regex(s).finditer(code_bytes))
            sig = (s, len(hits), [code_base + h.start() for h in hits[:4]])
            if len(hits) == 1:
                break
        if sig:
            e["sig"] = to_hex(sig[0]); e["sig_len"] = len(sig[0]); e["sig_matches"] = sig[1]; e["sig_hit_rvas"] = [hex(x) for x in sig[2]]
            e["sig_self_hit"] = rva in sig[2]
        else:
            e["sig"] = None; e["sig_note"] = "could not decode from this address (misaligned / garbage)"
    # ---- data xref signatures (single linear sweep of the code section collecting rip-relative refs to wanted targets)
    wanted = {e["rva"]: e for e in entries if e["kind"] == "data" and img.in_dump(e["rva"])}
    refs = {t: [] for t in wanted}
    insns = []
    for s in img.code:
        lo, hi = s["rva"], min(s["rva"] + s["vsize"], len(img.d))
        data = img.d[lo:hi]
        base = 0x140000000 + lo
        for insn in md.disasm(data, base):
            insns.append(insn)
            for op in (insn.operands if (insn.id != 0 and insn.disp_size) else ()):
                if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                    tgt = insn.address + insn.size + op.mem.disp - 0x140000000
                    if tgt in refs and len(refs[tgt]) < 40:
                        refs[tgt].append(len(insns) - 1)
                    break
    code0 = img.d[img.code[0]["rva"]:img.code[0]["rva"] + img.code[0]["vsize"]]
    for tgt, idxs in refs.items():
        e = wanted[tgt]
        e["xref_count_seen"] = len(idxs)
        sigs = []
        for k in idxs:
            best = None
            # Sibling globals are often initialised by near-identical code, so a short window can match at
            # several places (and point at the WRONG sibling). Grow the window until it is unique.
            for radius in (3, 5, 8, 12, 16, 24):
                lo_i, hi_i = max(0, k - radius), min(len(insns), k + radius + 1)
                seq = insns[lo_i:hi_i]
                bytes_, disp_off, next_ip_off = [], None, None
                for j, ins in enumerate(seq):
                    b = list(ins.bytes)
                    if lo_i + j == k:
                        disp_off = len(bytes_) + ins.disp_offset
                        next_ip_off = len(bytes_) + ins.size
                    for a_, z_ in mask_ranges(ins):
                        for q in range(a_, min(z_, len(b))):
                            b[q] = None
                    bytes_.extend(b)
                n = len(list(sig_regex(bytes_).finditer(code0)))
                cand = dict(site_rva=hex(insns[k].address - 0x140000000), sig=to_hex(bytes_), matches_in_code=n,
                            disp_off=disp_off, next_ip_off=next_ip_off, radius=radius,
                            mnemonic="%s %s" % (insns[k].mnemonic, insns[k].op_str))
                if best is None or n < best["matches_in_code"]:
                    best = cand
                if n == 1:
                    break
            # Function-relative form: MSVC magic-static accessors and similar templates are byte-identical
            # apart from their displacements, so NO local window separates the siblings. What does separate
            # them is the enclosing function's own start signature; store that plus the site's offset in it.
            site_rva = insns[k].address - 0x140000000
            p = site_rva
            fs = None
            while p > max(0, site_rva - 0x1000):
                if img.d[p - 1] == 0xCC and img.d[p] != 0xCC:
                    fs = p; break
                p -= 1
            if fs is not None:
                for L in (32, 48, 64, 96, 128):
                    fsig, cnt = masked_bytes(md, img.d[fs:fs + L + 16], 0x140000000 + fs, L)
                    if cnt == 0:
                        break
                    fn = len(list(sig_regex(fsig).finditer(code0)))
                    best["func_rel"] = dict(func_rva=hex(fs), func_sig=to_hex(fsig), func_matches=fn,
                                            site_off=site_rva - fs, ins_size=insns[k].size,
                                            disp_off_in_insn=insns[k].disp_offset)
                    if fn == 1:
                        break
            sigs.append(best)
        # unique local signatures first, then unique function-relative ones, then the rest
        sigs.sort(key=lambda s: (s["matches_in_code"] != 1 and s.get("func_rel", {}).get("func_matches", 9) != 1,
                                 s["matches_in_code"]))
        e["xref_sigs"] = sigs[:6]
    return entries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default=r"C:\Users\TerryGluff\Documents\Claude\CO99\scratchpad\image_dump.bin")
    ap.add_argument("--game-h", default=os.path.join(REPO, "src", "game.h"))
    ap.add_argument("--out-dir", default=os.path.join(REPO, "docs", "investigation"))
    a = ap.parse_args()

    img = Image(a.image)
    print("image: %s  stamp 0x%08X  SizeOfImage 0x%X  dumped 0x%X bytes" % (a.image, img.stamp, img.size_of_image, len(img.d)))
    code_sec = img.code[0]
    code_bytes = img.d[code_sec["rva"]:code_sec["rva"] + code_sec["vsize"]]

    entries = []
    for c in parse_game_h(a.game_h):
        if c["ns"] == "GameVtableIndex":
            entries.append(dict(name=c["name"], kind="vtable_index", value=c["value"], status=c["status"], source="game.h:%d" % c["line"], evidence=c["evidence"]))
            continue
        kind = "code" if img.is_code(c["value"]) else ("data" if img.section_of(c["value"]) else "offset")
        # Small numbers (< 0x10000) in Offsets:: are struct-relative offsets, not RVAs
        if c["ns"] == "Offsets" and c["value"] < 0x10000:
            kind = "struct_offset"
        entries.append(dict(name=c["name"], kind=kind, rva=c["value"], status=c["status"], source="game.h:%d" % c["line"],
                            namespace=c["ns"], evidence=c["evidence"]))
    for s in SUPPLEMENT:
        s = dict(s); s["source"] = "notes"; s["evidence"] = s.pop("note")
        entries.append(s)

    for e in entries:
        if e["name"] in OVERRIDES:
            st, why = OVERRIDES[e["name"]]
            if st != e["status"]:
                e["status_tagscrape"] = e["status"]      # what the naive tag scrape said, kept for audit
            e["status"], e["status_source"], e["status_reason"] = st, "override", why
        if e["name"] in DERIVED:
            e["derived_from"], e["derived_delta"] = DERIVED[e["name"]]
        else:
            e.setdefault("status_source", "tag" if e["status"] not in ("stale_default",) else "default")
    build(img, [e for e in entries if e["kind"] in ("code", "data")], code_bytes, code_sec["rva"])
    fields = parse_struct_fields(os.path.join(REPO, "src"))

    reg = dict(
        generated_by="tools/build_offset_registry.py",
        image=dict(path=os.path.basename(a.image), sha256=hashlib.sha256(img.d).hexdigest(), bytes=len(img.d),
                   pe_timedatestamp="0x%08X" % img.stamp, size_of_image="0x%X" % img.size_of_image,
                   sections=[dict(name=s["name"], rva=hex(s["rva"]), vsize=hex(s["vsize"]), chars=hex(s["chars"])) for s in img.sections]),
        client_build="v1074 (exe SHA-256 C2B53437EF68D687A1EF0F70C74BCF2DF6027BF82B558E93330C839EB5E1C396)",
        entries=entries, struct_fields=fields)
    os.makedirs(a.out_dir, exist_ok=True)
    jp = os.path.join(a.out_dir, "v1074_offset_registry.json")
    with open(jp, "w", encoding="utf-8") as f:
        json.dump(reg, f, indent=1)

    # ---- markdown
    md = ["# v1074 offset registry (generated)", "",
          "Generated by `tools/build_offset_registry.py` from `src/game.h` + the preserved decrypted image dump. "
          "Machine-readable twin: `v1074_offset_registry.json`. **Do not hand-edit; regenerate.** Purpose: the intact OLD side of the "
          "client-version diff — locate each item in the new build with `tools/sigscan.py`, then re-verify at runtime.", "",
          "- Image: `%s`, PE TimeDateStamp `0x%08X`, SizeOfImage `0x%X`, dumped `0x%X` bytes, SHA-256 `%s`" %
          (os.path.basename(a.image), img.stamp, img.size_of_image, len(img.d), reg["image"]["sha256"][:16] + "…"),
          "- Status: each `game.h` constant carries a REVIEWED verdict from the `OVERRIDES` table in the tool, each with a cited reason "
          "(`status_reason` in the JSON) — tag-scraping alone mis-read several entries, so it is only the fallback. `verified` = live-verified "
          "on v1074; `stale`/`garbage`/`wrong` = known NOT to be a valid v1074 address (kept so a diff can see what they are); `unverified` = "
          "never confirmed. Code signatures are masked (branch targets / rip-relative displacements / image pointers wildcarded); data globals "
          "are relocated through code cross-references (local window or enclosing-function signature + offset).",
          "- `uniq` = number of places the signature matches in the v1074 code section (1 = unique). Identity check "
          "(`tools/sigscan.py --identity`): every verified entry resolves to its own address. That validates the tooling mechanically; the "
          "real test is a second build (the fresh-install rehearsal).", ""]
    from collections import Counter
    cnt = Counter((e["kind"], e["status"]) for e in entries)
    md += ["## Summary", "", "| kind | status | count |", "|---|---|---|"] + ["| %s | %s | %d |" % (k, s, n) for (k, s), n in sorted(cnt.items())] + [""]
    order = ["verified", "unverified", "stale", "stale_default", "wrong", "garbage"]
    for st in order:
        rows = [e for e in entries if e["status"] == st]
        if not rows:
            continue
        md += ["## %s (%d)" % (st, len(rows)), "", "| name | kind | RVA | signature | uniq | source |", "|---|---|---|---|---|---|"]
        for e in sorted(rows, key=lambda x: (x["kind"], x.get("rva", x.get("value", 0)))):
            rva = "0x%X" % e["rva"] if "rva" in e else "idx %d" % e.get("value", 0)
            if e["kind"] == "code":
                sig = ("`%s`…" % e["sig"][:47]) if e.get("sig") else "—"
                uniq = str(e.get("sig_matches", "—")) + ("" if e.get("sig_self_hit", True) else " (!self)")
            elif e["kind"] == "data":
                xs = e.get("xref_sigs") or []
                sig = "%d xref site(s)" % len(xs) if xs else "no code refs found"
                uniq = "best %s" % xs[0]["matches_in_code"] if xs else "—"
            else:
                sig, uniq = "—", "—"
            md.append("| `%s` | %s | %s | %s | %s | %s |" % (e["name"], e["kind"], rva, sig, uniq, e.get("source", "")))
        md += [""]
    md += ["## Struct-field offsets (from headers; runtime-verified, not byte-signature)", "",
           "| file:line | field | offset | tag | comment |", "|---|---|---|---|---|"]
    for f in sorted(fields, key=lambda x: (x["file"], x["offset"])):
        md.append("| %s:%d | `%s` | +0x%X | %s | %s |" % (f["file"], f["line"], f["field"], f["offset"], f["status"], f["comment"][:90].replace("|", "/")))
    with open(os.path.join(a.out_dir, "V1074_OFFSET_REGISTRY.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(md) + "\n")
    print("wrote", jp, "and V1074_OFFSET_REGISTRY.md;", len(entries), "entries,", len(fields), "struct fields")
    for k, n in sorted(cnt.items()):
        print("  %-13s %-14s %d" % (k[0], k[1], n))


if __name__ == "__main__":
    sys.exit(main())
