// =====================================================================
// heapfind.dll v3 — find the game's OWN registries of live objects (roles,
// map items) and HOW THEY ARE REACHED from a static anchor.
//
// Route 1 of HEAP_SCANNER_REPLACEMENT. Standalone, READ-ONLY, single-shot:
// inject into the game, runs once, writes its report, unloads. No hooks, no
// code patches, every read SEH-guarded.
//
// What v1/v2 established (2026-09-03):
//   * The only contiguous pointer arrays holding roles/items are per-frame
//     WORKING LISTS (a cap-128 std::vector<CRole*> whose size swings 1..24 =
//     "roles on screen"), plus coclassic.dll's own scan snapshots. The real
//     registries are NODE containers (std::map / unordered_map), which a
//     contiguous-run detector cannot see by construction.
//   * v2's owner search was drowned by the tool's own memory: every vector of
//     hit ADDRESSES / run starts / holder records is itself a "holder".
//
// v3 therefore:
//   1. Puts EVERY tool container in one VirtualAlloc arena and excludes that
//      arena (and this DLL's image) from every scan. Values are still XOR-
//      masked, belt and braces.
//   2. Classifies each individual hit by the STRUCTURE around it, with link
//      validation so false positives are impossible:
//        - MSVC std::map<u32,T*> node: {left,parent,right,color,isnil,pad,key@+0x20,val@+0x28}
//          confirmed only if parent->left/right == node (or head->parent == node)
//        - MSVC unordered_map<u32,T*> / list node: {next,prev,key@+0x10,val@+0x18}
//          confirmed only if next->prev == node && prev->next == node
//        - array element (stride-8 / stride-16 shared_ptr runs), shared_ptr pair
//   3. For a confirmed map node: climbs parent links to the HEAD sentinel
//      (isnil==1), traverses the whole tree (count, how many are live P
//      members, samples of the rest), then finds who holds the head pointer
//      (= the map object's _Myhead field; the qword after it is _Mysize) and
//      who points into the object containing it — scanning the IMAGE's data
//      section too, so a global anchor comes out as image+RVA.
//   4. For a confirmed unordered_map node: walks the ring, then owner-searches
//      every ring node address; hits outside the ring that are NOT a bucket
//      array run are the _Myhead field.
//
// RUN IT WITHOUT coclassic.dll LOADED: the bot's double-buffered snapshots are
// indistinguishable from game arrays.
// =====================================================================
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <new>

static const uint64_t  kMask            = 0xA5C3F00D5EED1234ull;
static const uintptr_t RVA_ROLE_MGR_PTR = 0x69C730;      // [LIVE-VERIFIED] *(base+..) = CRoleMgr*
static const uintptr_t RVA_SCENE_PTR    = 0x699370;      // per docs: current map/scene pointer

// ── arena: all tool storage lives here and is excluded from every scan ───
struct Arena {
    uint8_t* base = nullptr; size_t reserved = 0, committed = 0, used = 0;
    bool Init(size_t reserve) {
        base = (uint8_t*)VirtualAlloc(nullptr, reserve, MEM_RESERVE, PAGE_READWRITE);
        reserved = base ? reserve : 0; return base != nullptr;
    }
    void* Alloc(size_t n) {
        n = (n + 15) & ~(size_t)15;
        if (used + n > committed) {
            const size_t chunk = 1u << 24;
            size_t need = ((used + n - committed) + chunk - 1) & ~(chunk - 1);
            if (committed + need > reserved || !VirtualAlloc(base + committed, need, MEM_COMMIT, PAGE_READWRITE)) throw std::bad_alloc();
            committed += need;
        }
        void* p = base + used; used += n; return p;
    }
    bool Contains(uintptr_t a) const { return a >= (uintptr_t)base && a < (uintptr_t)base + reserved; }
};
static Arena g_arena;
template <class T> struct AAlloc {
    using value_type = T;
    AAlloc() = default; template <class U> AAlloc(const AAlloc<U>&) {}
    T* allocate(size_t n) { return (T*)g_arena.Alloc(n * sizeof(T)); }
    void deallocate(T*, size_t) {}
    template <class U> bool operator==(const AAlloc<U>&) const { return true; }
    template <class U> bool operator!=(const AAlloc<U>&) const { return false; }
};
template <class T> using AVec = std::vector<T, AAlloc<T>>;
template <class K> using ASet = std::unordered_set<K, std::hash<K>, std::equal_to<K>, AAlloc<K>>;
template <class K, class V> using AMap = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, AAlloc<std::pair<const K, V>>>;

// ── guarded reads ────────────────────────────────────────────────────────
template <class T> static bool TryRead(uintptr_t a, T* o)
{ __try { *o = *reinterpret_cast<volatile T*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static uint64_t U64(uintptr_t a){ uint64_t v=0; TryRead(a,&v); return v; }
static uint32_t U32(uintptr_t a){ uint32_t v=0; TryRead(a,&v); return v; }
static uint8_t  U8 (uintptr_t a){ uint8_t  v=0; TryRead(a,&v); return v; }
static int32_t  I32(uintptr_t a){ int32_t  v=0; TryRead(a,&v); return v; }
static bool IsHeapPtr(uint64_t p){ return p > 0x10000 && p < 0x7FFFFFFFFFFFull && (p & 7) == 0; }

struct ImageRange { uint64_t lo, hi; };
static ImageRange ModuleRange(HMODULE m)
{
    const uint64_t base = (uint64_t)m;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS* nt  = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    return { base, base + nt->OptionalHeader.SizeOfImage };
}

struct Ctx { ImageRange img; uintptr_t base, roleMgr, scene, hero; };
static const char* Rel(uintptr_t v, const Ctx& c, char* buf, size_t n)
{
    if (v >= c.img.lo && v < c.img.hi) { snprintf(buf, n, "IMAGE+%#llx", (unsigned long long)(v - c.img.lo)); return buf; }
    auto nearAnchor = [&](uintptr_t anchor, const char* nm) -> bool {
        if (!anchor) return false;
        const long long d = (long long)v - (long long)anchor;
        if (d >= 0 && d < 0x10000) { snprintf(buf, n, "%s+%#llx", nm, (unsigned long long)d); return true; }
        return false;
    };
    if (nearAnchor(c.roleMgr, "CRoleMgr") || nearAnchor(c.scene, "scene") || nearAnchor(c.hero, "hero")) return buf;
    snprintf(buf, n, "-"); return buf;
}

// ── shape checks (mirror entities.cpp / map_items.cpp) ───────────────────
static bool LooksLikeRole(uintptr_t p, const ImageRange& img, uint32_t* idOut)
{
    uint64_t vt = 0;
    if (!TryRead(p, &vt) || vt < img.lo || vt >= img.hi) return false;
    uint32_t id = 0; int32_t px = 0, py = 0;
    if (!TryRead(p + 0x68, &id) || id == 0 || id >= 5000000) return false;
    if (!TryRead(p + 0xD8, &px) || !TryRead(p + 0xDC, &py)) return false;
    if (px <= 0 || px > 1000 || py <= 0 || py > 1000) return false;
    *idOut = id; return true;
}
static bool HasRealName(uintptr_t p, char* out16)
{
    char name[16] = {};
    for (int i = 0; i < 15; ++i) { char c = 0; if (!TryRead(p + 0x94 + i, &c)) return false; name[i] = c; if (!c) break; }
    int len = 0; while (len < 15 && name[len]) ++len;
    if (len < 3) return false;
    int letters = 0;
    for (int i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7F) return false;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) ++letters;
    }
    if (letters < 1) return false;
    for (int i = 0; i + 10 <= len; ++i) {
        int j = 0; for (; j < 10; ++j) { char c = name[i+j]; if (c>='A'&&c<='Z') c=(char)(c+32); if (c != "appearance"[j]) break; }
        if (j == 10) return false;
    }
    memcpy(out16, name, 16); return true;
}
static bool LooksLikeMapItem(uintptr_t p)
{
    uint32_t id = 0, type = 0; int32_t px = 0, py = 0; uint64_t p0 = 0, p1 = 0;
    if (!TryRead(p, &id) || id == 0) return false;
    if (!TryRead(p + 4, &type) || type == 0) return false;
    if (!TryRead(p + 8, &px) || !TryRead(p + 12, &py)) return false;
    if (px <= 0 || py <= 0 || px >= 1500 || py >= 1500) return false;
    const bool money = type >= 1090000 && type <= 1091020;
    if (!money && (type < 100000 || type > 3000000)) return false;
    if (!TryRead(p + 0x10, &p0) || !TryRead(p + 0x18, &p1)) return false;
    if ((p0 != 0 && !IsHeapPtr(p0)) || (p1 != 0 && !IsHeapPtr(p1))) return false;
    return true;
}

// ── regions: private RW heap + the game image's writable data ────────────
struct Region { uintptr_t base; size_t size; bool image; };
static AVec<Region> CollectRegions(const Ctx& c, const ImageRange& self)
{
    AVec<Region> out; MEMORY_BASIC_INFORMATION mbi{}; uintptr_t addr = 0x10000;
    while (addr < 0x7FFFFFFF0000ull) {
        if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) break;
        const uintptr_t rbase = (uintptr_t)mbi.BaseAddress; const size_t rsize = mbi.RegionSize;
        if (!rsize) break;
        addr = rbase + rsize;
        if (mbi.State != MEM_COMMIT) continue;
        if (g_arena.Contains(rbase) || (rbase >= self.lo && rbase < self.hi)) continue;
        const DWORD pr = mbi.Protect & 0xFF;
        const bool writable = pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_WRITECOPY;
        if (!writable) continue;
        if (mbi.Type == MEM_PRIVATE && rsize > 0x20 && rsize < 0x4000000) out.push_back({rbase, rsize, false});
        else if (mbi.Type == MEM_IMAGE && rbase >= c.img.lo && rbase < c.img.hi) out.push_back({rbase, rsize, true});
    }
    return out;
}

struct Hit { uintptr_t addr; uint64_t maskedVal; };
static AVec<Hit> ScanFor(const AVec<Region>& regions, const ASet<uint64_t>& targets, uint64_t* qwordsOut, size_t capTotal = 200000)
{
    AVec<Hit> hits; uint64_t q = 0;
    for (const Region& r : regions) {
        for (uintptr_t a = r.base; a + 8 <= r.base + r.size; a += 8) {
            uint64_t v = 0; if (!TryRead(a, &v)) { a = (a + 0xFFF) & ~(uintptr_t)0xFFF; continue; }
            ++q;
            if (!IsHeapPtr(v)) continue;
            const uint64_t m = v ^ kMask;
            if (targets.count(m)) { hits.push_back({a, m}); if (hits.size() >= capTotal) { *qwordsOut = q; return hits; } }
        }
    }
    *qwordsOut = q; return hits;
}

struct Found { uint64_t maskedPtr; uint32_t id; uint32_t type; int32_t x, y; char name[16]; };
struct Run { uintptr_t start; int len; };
static AVec<Run> ContiguousRuns(AVec<uintptr_t> addrs, int stride, int minLen)
{
    std::sort(addrs.begin(), addrs.end()); addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
    AVec<Run> out;
    for (size_t i = 0; i < addrs.size();) {
        size_t j = i; while (j + 1 < addrs.size() && addrs[j+1] - addrs[j] == (uintptr_t)stride) ++j;
        if ((int)(j - i + 1) >= minLen) out.push_back({addrs[i], (int)(j - i + 1)});
        i = j + 1;
    }
    std::sort(out.begin(), out.end(), [](const Run& a, const Run& b){ return a.len > b.len; });
    return out;
}

// ── node-structure checks with LINK VALIDATION ───────────────────────────
// MSVC std::map<K,V> node: +0 left, +8 parent, +0x10 right, +0x18 color, +0x19 isnil,
// +0x20 pair{key, value}. With K=u32 and V=pointer the value sits at +0x28.
static bool CheckMapNode(uintptr_t A, uint32_t id, uintptr_t* nodeOut, bool* keyOk)
{
    const uintptr_t node = A - 0x28;
    const uint64_t l = U64(node), pa = U64(node + 8), r = U64(node + 0x10);
    if (!IsHeapPtr(l) || !IsHeapPtr(pa) || !IsHeapPtr(r)) return false;
    if (U8(node + 0x18) > 1 || U8(node + 0x19) > 1) return false;
    const bool linkOk = U64(pa) == node || U64(pa + 0x10) == node || U64(pa + 8) == node;
    if (!linkOk) return false;
    *nodeOut = node; *keyOk = U32(node + 0x20) == id; return true;
}
// MSVC std::list / unordered_map node: +0 next, +8 prev, +0x10 value. For
// unordered_map<u32,T*> the value is pair{key@+0x10, ptr@+0x18}; for list<T*> ptr@+0x10.
static bool CheckRingNode(uintptr_t node, const char** kind, uint32_t id, uintptr_t A, bool* keyOk)
{
    const uint64_t next = U64(node), prev = U64(node + 8);
    if (!IsHeapPtr(next) || !IsHeapPtr(prev)) return false;
    if (U64(next + 8) != node || U64(prev) != node) return false;
    if (A == node + 0x18) { *kind = "umap-node"; *keyOk = U32(node + 0x10) == id; }
    else { *kind = "list-node"; *keyOk = false; }
    return true;
}
static bool CheckSharedPtr(uintptr_t A, const ImageRange& img)
{
    const uint64_t ctrl = U64(A + 8);
    if (!IsHeapPtr(ctrl)) return false;
    const uint64_t vt = U64((uintptr_t)ctrl);
    if (vt < img.lo || vt >= img.hi) return false;
    const uint32_t uses = U32((uintptr_t)ctrl + 8);
    return uses >= 1 && uses < 100000;
}

// MSVC x64 RTTI: vtable[-1] -> RTTICompleteObjectLocator {sig=1, offset, cdOffset, pTypeDescriptor
// (image-relative), pClassDescriptor, pSelf}; TypeDescriptor name at +0x10 (".?AVCRole@@").
static const char* RttiName(uint64_t vt, const ImageRange& img, char* buf, size_t n)
{
    buf[0] = 0;
    if (vt < img.lo + 8 || vt >= img.hi) return buf;
    const uint64_t col = U64((uintptr_t)vt - 8);
    if (col < img.lo || col >= img.hi) return buf;
    const uint32_t sig = U32((uintptr_t)col), tdOff = U32((uintptr_t)col + 12);
    if (sig != 1 || !tdOff) return buf;
    const uintptr_t td = (uintptr_t)img.lo + tdOff;
    if (td + 0x10 >= img.hi) return buf;
    for (size_t i = 0; i + 1 < n && i < 100; ++i) {
        char ch = 0; if (!TryRead(td + 0x10 + i, &ch) || !ch) { buf[i] = 0; break; }
        buf[i] = (ch >= 0x20 && ch < 0x7F) ? ch : '?'; buf[i + 1] = 0;
    }
    return buf;
}
static bool LooksLikeVtable(uint64_t vt, const ImageRange& img)
{
    if (vt < img.lo + 8 || vt >= img.hi || (vt & 7)) return false;
    const uint64_t f0 = U64((uintptr_t)vt);                 // first virtual: code pointer inside the image
    return f0 >= img.lo && f0 < img.hi;
}

// a live std::shared_ptr {obj, ctrl}: MSVC control block = {vtable, _Uses u32, _Weaks u32}
static bool IsSpPair(uintptr_t A, const ImageRange& img)
{
    const uint64_t obj = U64(A), ctrl = U64(A + 8);
    if (!IsHeapPtr(obj) || !IsHeapPtr(ctrl)) return false;
    const uint64_t vt = U64((uintptr_t)ctrl);
    if (vt < img.lo || vt >= img.hi) return false;
    const uint32_t uses = U32((uintptr_t)ctrl + 8), weaks = U32((uintptr_t)ctrl + 12);
    return uses >= 1 && uses < 100000 && weaks >= 1 && weaks < 100000;
}

// climb a confirmed map node to the head sentinel (isnil == 1)
static uintptr_t ClimbToHead(uintptr_t node)
{
    uintptr_t n = node;
    for (int hops = 0; hops < 64; ++hops) {
        const uint64_t pa = U64(n + 8);
        if (!IsHeapPtr(pa)) return 0;
        if (U8((uintptr_t)pa + 0x19) == 1) {
            // head->parent must be the root, and the root's parent must be head
            const uint64_t root = U64((uintptr_t)pa + 8);
            return (IsHeapPtr(root) && U64((uintptr_t)root + 8) == pa) ? (uintptr_t)pa : 0;
        }
        n = (uintptr_t)pa;
    }
    return 0;
}

struct Report {
    FILE* f; const Ctx* c; const AVec<Region>* regions; ASet<uint64_t>* setP;   // setP masked
    const ImageRange* img;
};

// who holds `target` exactly, and who points into the 32 KB before each such field
static void OwnerSearch(Report& R, uintptr_t target, const char* what, long long expectSize)
{
    char rb[64];
    ASet<uint64_t> t; t.insert((uint64_t)target ^ kMask);
    uint64_t q = 0; const AVec<Hit> hits = ScanFor(*R.regions, t, &q, 256);
    fprintf(R.f, "  owners of %s %#llx: %d\n", what, (unsigned long long)target, (int)hits.size());
    ASet<uint64_t> back;
    for (const Hit& h : hits) {
        const uint64_t sz = U64(h.addr + 8);
        fprintf(R.f, "    field @%#llx [%s]  next qword=%llu%s\n", (unsigned long long)h.addr, Rel(h.addr, *R.c, rb, sizeof rb),
                (unsigned long long)sz, (expectSize >= 0 && (long long)sz == expectSize) ? "  <== equals node count: this is {_Myhead,_Mysize}" : "");
        for (int k = 0; k <= 4096; ++k) back.insert((uint64_t)(h.addr - 8*k) ^ kMask);
    }
    if (hits.empty() || hits.size() > 8) return;
    const AVec<Hit> hb = ScanFor(*R.regions, back, &q, 512);
    // group: only print pointers that land within 32KB before a field; nearest first
    struct Own { uintptr_t addr; uintptr_t to; long long delta; };
    AVec<Own> owns;
    for (const Hit& h : hb) {
        const uintptr_t to = (uintptr_t)(h.maskedVal ^ kMask);
        for (const Hit& fh : hits) if (to <= fh.addr && fh.addr - to <= 0x8000) owns.push_back({h.addr, to, (long long)(fh.addr - to)});
    }
    std::sort(owns.begin(), owns.end(), [](const Own& a, const Own& b){ return a.delta < b.delta; });
    fprintf(R.f, "    pointers INTO the object containing that field (delta = field - pointee), %d total:\n", (int)owns.size());
    for (size_t i = 0; i < owns.size() && i < 40; ++i)
        fprintf(R.f, "      @%#llx [%s] = %#llx  (field = pointee + %#llx)\n", (unsigned long long)owns[i].addr,
                Rel(owns[i].addr, *R.c, rb, sizeof rb), (unsigned long long)owns[i].to, (unsigned long long)owns[i].delta);
}

static void DumpObject(Report& R, const char* name, uintptr_t obj, size_t len);

// Who points INTO the object that contains `field`? Recurse from the most-referenced
// candidate bases until an IMAGE / CRoleMgr / scene / hero anchor shows up.
static void ChainToAnchor(Report& R, uintptr_t field, int depth)
{
    char rb[64]; const char* ind = depth == 1 ? "    " : depth == 2 ? "        " : "            ";
    ASet<uint64_t> t; for (int k = 0; k <= 4096; ++k) t.insert((uint64_t)(field - 8*k) ^ kMask);
    uint64_t q = 0; const AVec<Hit> hits = ScanFor(*R.regions, t, &q, 4000);
    struct Own { uintptr_t addr, to; long long delta; };
    AVec<Own> owns;
    for (const Hit& h : hits) { const uintptr_t to = (uintptr_t)(h.maskedVal ^ kMask); owns.push_back({h.addr, to, (long long)(field - to)}); }
    std::sort(owns.begin(), owns.end(), [](const Own& a, const Own& b){ return a.delta != b.delta ? a.delta < b.delta : a.addr < b.addr; });
    fprintf(R.f, "%schain L%d: %d qwords point into the 32KB before field %#llx (delta = field - pointee)\n", ind, depth, (int)owns.size(), (unsigned long long)field);
    bool anchored = false;
    AMap<uintptr_t, int> refCount; AMap<uintptr_t, uintptr_t> firstHolder;
    int printed = 0;
    for (const Own& o : owns) {
        Rel(o.addr, *R.c, rb, sizeof rb);
        const bool anch = rb[0] != '-';
        if (anch) anchored = true;
        if (printed < 24 || anch) { ++printed;
            fprintf(R.f, "%s  @%#llx [%s] = %#llx  (+%#llx)%s\n", ind, (unsigned long long)o.addr, rb, (unsigned long long)o.to,
                    (unsigned long long)o.delta, anch ? "  <== ANCHOR" : ""); }
        if (++refCount[o.to] == 1) firstHolder[o.to] = o.addr;
    }
    if (anchored || depth >= 1) return;   // v5: window-chain recursion proved noisy; ObjectGraph does the walking
    // recurse on the two most-referenced pointees (an object base is usually held in several places)
    AVec<std::pair<int, uintptr_t>> ranked; for (auto& kv : refCount) ranked.push_back({kv.second, kv.first});
    std::sort(ranked.begin(), ranked.end(), [](const std::pair<int,uintptr_t>& a, const std::pair<int,uintptr_t>& b){ return a.first > b.first; });
    for (size_t i = 0; i < ranked.size() && i < 2; ++i) {
        fprintf(R.f, "%s  -> pointee %#llx held %d times; who holds ITS container? (via holder @%#llx)\n", ind,
                (unsigned long long)ranked[i].second, ranked[i].first, (unsigned long long)firstHolder[ranked[i].second]);
        ChainToAnchor(R, firstHolder[ranked[i].second], depth + 1);
    }
}

// Object-graph walker: find vtable-bearing object bases in the 16KB before `field`
// (nearest first, RTTI-named), then who holds each base EXACTLY (raw pointer, or the
// shared_ptr pair {base, base-0x10}); recurse from those holders until an anchor.
static void ObjectGraph(Report& R, uintptr_t field, int depth, ASet<uintptr_t>& visited)
{
    if (depth > 4) return;
    char rb[64], nm[128]; const char* ind = depth == 1 ? "    " : depth == 2 ? "      " : depth == 3 ? "        " : "          ";
    struct Cand { uintptr_t base; uint64_t vt; };
    AVec<Cand> cands;
    for (int k = 0; k <= 2048 && cands.size() < 6; ++k) {
        const uintptr_t a = field - 8*k; const uint64_t vt = U64(a);
        if (LooksLikeVtable(vt, *R.img) && RttiName(vt, *R.img, nm, sizeof nm)[0]) cands.push_back({a, vt});
    }
    if (cands.empty()) {   // no RTTI: accept plain vtable-looking qwords, nearest 3
        for (int k = 0; k <= 2048 && cands.size() < 3; ++k) { const uintptr_t a = field - 8*k; const uint64_t vt = U64(a); if (LooksLikeVtable(vt, *R.img)) cands.push_back({a, vt}); }
    }
    fprintf(R.f, "%sL%d field %#llx: %d object bases before it\n", ind, depth, (unsigned long long)field, (int)cands.size());
    for (size_t i = 0; i < cands.size() && i < 4; ++i) {
        const Cand& cd = cands[i];
        fprintf(R.f, "%s  base %#llx (field = base+%#llx) vt=IMAGE+%#llx %s\n", ind, (unsigned long long)cd.base,
                (unsigned long long)(field - cd.base), (unsigned long long)(cd.vt - R.img->lo), RttiName(cd.vt, *R.img, nm, sizeof nm));
        if (visited.count(cd.base)) { fprintf(R.f, "%s    (already walked)\n", ind); continue; }
        visited.insert(cd.base);
        ASet<uint64_t> t; t.insert((uint64_t)cd.base ^ kMask);
        uint64_t q = 0; const AVec<Hit> hs = ScanFor(*R.regions, t, &q, 48);
        bool anchored = false; AVec<uintptr_t> next;
        for (const Hit& h : hs) {
            Rel(h.addr, *R.c, rb, sizeof rb);
            const bool anch = rb[0] != '-';
            const bool sp = U64(h.addr + 8) == cd.base - 0x10;
            fprintf(R.f, "%s    held @%#llx [%s]%s%s\n", ind, (unsigned long long)h.addr, rb, sp ? " as shared_ptr" : "", anch ? "  <== ANCHOR" : "");
            if (anch) anchored = true; else if (next.size() < 3) next.push_back(h.addr);
        }
        if (hs.empty()) fprintf(R.f, "%s    (no exact holder)\n", ind);
        if (!anchored) for (uintptr_t h : next) ObjectGraph(R, h, depth + 1, visited);
    }
}

// v6: FORWARD pointer-path search. Breadth-first from every heap pointer held in
// static memory (the image's data sections, CRoleMgr, hero), following heap
// pointers object-to-object, until a pointer lands inside [lo, hi] (the object
// that contains the vector holder). Prints the shortest paths with offsets —
// a static anchor chain that does not depend on which transient object
// happens to cache the pointer this session (the scene+0x4E0 trap).
static void FindStaticPath(Report& R, uintptr_t lo, uintptr_t hi, const char* what)
{
    struct Node { uintptr_t val, srcAddr; int parent; uint8_t depth; };
    AVec<Node> nodes; ASet<uintptr_t> seen; AVec<int> queue; size_t qh = 0;
    char rb[64];
    auto add = [&](uintptr_t srcAddr, uint64_t v, int parent, uint8_t depth) {
        if (!IsHeapPtr(v) || seen.count((uintptr_t)v)) return;
        seen.insert((uintptr_t)v); nodes.push_back({(uintptr_t)v, srcAddr, parent, depth}); queue.push_back((int)nodes.size() - 1);
    };
    const DWORD t0 = GetTickCount();
    for (const Region& r : *R.regions) if (r.image)
        for (uintptr_t a = r.base; a + 8 <= r.base + r.size; a += 8) { uint64_t v = 0; if (TryRead(a, &v)) add(a, v, -1, 1); }
    const size_t nImageRoots = nodes.size();
    for (uintptr_t off = 0; off < 0x10000; off += 8) { uint64_t v = 0; if (R.c->roleMgr && TryRead(R.c->roleMgr + off, &v)) add(R.c->roleMgr + off, v, -1, 1); }
    for (uintptr_t off = 0; off < 0x10000; off += 8) { uint64_t v = 0; if (R.c->hero && TryRead(R.c->hero + off, &v)) add(R.c->hero + off, v, -1, 1); }
    fprintf(R.f, "\n=== %s: forward path search to [%#llx..%#llx] from %d image-data roots + %d CRoleMgr/hero roots ===\n",
            what, (unsigned long long)lo, (unsigned long long)hi, (int)nImageRoots, (int)(nodes.size() - nImageRoots));
    const size_t kMaxNodes = 1500000; const uint8_t kMaxDepth = 6; const uintptr_t kSpan = 0x1800;
    int found = 0;
    while (qh < queue.size() && nodes.size() < kMaxNodes && found < 8) {
        const int ni = queue[qh++]; const Node n = nodes[ni];
        if (n.val >= lo && n.val <= hi) {
            ++found;
            AVec<int> chain; for (int i = ni; i >= 0; i = nodes[i].parent) chain.push_back(i);
            fprintf(R.f, "  PATH #%d (depth %d): ", found, (int)n.depth);
            for (size_t k = chain.size(); k-- > 0;) {
                const Node& c = nodes[chain[k]];
                if (c.parent < 0) fprintf(R.f, "[%s] = %#llx", Rel(c.srcAddr, *R.c, rb, sizeof rb), (unsigned long long)c.val);
                else fprintf(R.f, " -> [+%#llx] = %#llx", (unsigned long long)(c.srcAddr - nodes[c.parent].val), (unsigned long long)c.val);
            }
            fprintf(R.f, "   (target = pointee + %#llx)\n", (unsigned long long)(hi - 0x18 - n.val));
            continue;
        }
        if (n.depth >= kMaxDepth) continue;
        for (uintptr_t a = n.val; a < n.val + kSpan; a += 8) { uint64_t v = 0; if (!TryRead(a, &v)) break; add(a, v, ni, (uint8_t)(n.depth + 1)); }
    }
    fprintf(R.f, "  searched %d objects, %d paths, %lu ms%s\n", (int)nodes.size(), found, GetTickCount() - t0,
            nodes.size() >= kMaxNodes ? "  (node cap hit)" : (qh >= queue.size() ? "  (exhausted)" : ""));
}

// Discover every buffer of consecutive shared_ptr pairs that holds a P member, report
// its contents (uses count, object vtable RVA, in-P), find its {begin,end,cap} owner and
// chain that owner to a static anchor.
static void DiscoverSpBuffers(Report& R, const char* label, const AVec<Hit>& hits, const AMap<uint64_t, uint32_t>& idOf)
{
    char rb[64];
    ASet<uintptr_t> seenLo; int nBuf = 0;
    for (const Hit& h : hits) {
        if (!IsSpPair(h.addr, *R.img)) continue;
        uintptr_t lo = h.addr, hi = h.addr + 16;
        for (int k = 0; k < 8192 && IsSpPair(lo - 16, *R.img); ++k) lo -= 16;
        for (int k = 0; k < 8192 && IsSpPair(hi, *R.img); ++k) hi += 16;
        if (seenLo.count(lo)) continue;
        seenLo.insert(lo);
        const int n = (int)((hi - lo) / 16);
        if (n < 4) continue;
        if (++nBuf > 6) break;
        int inP = 0; AMap<uint64_t, int> vtHist; AMap<uint32_t, int> usesHist;
        for (uintptr_t a = lo; a < hi; a += 16) {
            const uint64_t obj = U64(a), ctrl = U64(a + 8);
            if (R.setP->count(obj ^ kMask)) ++inP;
            const uint64_t vt = U64((uintptr_t)obj);
            vtHist[(vt >= R.img->lo && vt < R.img->hi) ? vt - R.img->lo : 0]++;
            usesHist[U32((uintptr_t)ctrl + 8)]++;
        }
        fprintf(R.f, "\n=== %s shared_ptr buffer #%d: [%#llx..%#llx) %d pairs, %d hold a live P member ===\n",
                label, nBuf, (unsigned long long)lo, (unsigned long long)hi, n, inP);
        fprintf(R.f, "  object vtable RVAs:"); for (auto& kv : vtHist) { char nm[128]; fprintf(R.f, "  %#llx x%d %s", (unsigned long long)kv.first, kv.second, RttiName(kv.first ? kv.first + R.img->lo : 0, *R.img, nm, sizeof nm)); } fprintf(R.f, "\n");
        fprintf(R.f, "  ctrl _Uses histogram:"); for (auto& kv : usesHist) fprintf(R.f, "  uses=%u x%d", kv.first, kv.second); fprintf(R.f, "\n");
        int shown = 0;
        for (uintptr_t a = lo; a < hi && shown < 12; a += 16, ++shown) {
            const uint64_t obj = U64(a), ctrl = U64(a + 8), vt = U64((uintptr_t)obj);
            const uint64_t m = obj ^ kMask;
            fprintf(R.f, "    [%3d] obj=%#llx ctrl=%#llx uses=%u vt=%s%#llx id=%u%s\n", (int)((a - lo) / 16), (unsigned long long)obj, (unsigned long long)ctrl,
                    U32((uintptr_t)ctrl + 8), (vt >= R.img->lo && vt < R.img->hi) ? "IMAGE+" : "", (unsigned long long)((vt >= R.img->lo && vt < R.img->hi) ? vt - R.img->lo : vt),
                    idOf.count(m) ? idOf.at(m) : 0, R.setP->count(m) ? "  (P)" : "");
        }
        // owner: a qword equal to lo (or up to 16 pairs before it, in case the buffer starts with non-sp entries)
        ASet<uint64_t> t; for (int k = 0; k <= 16; ++k) t.insert((uint64_t)(lo - 16*k) ^ kMask);
        uint64_t q = 0; const AVec<Hit> own = ScanFor(*R.regions, t, &q, 64);
        fprintf(R.f, "  holders of begin: %d\n", (int)own.size());
        for (const Hit& o : own) {
            const uintptr_t begin = (uintptr_t)(o.maskedVal ^ kMask);
            const uint64_t e = U64(o.addr + 8), c = U64(o.addr + 16);
            const bool vec = e >= begin && c >= e && (e - begin) % 16 == 0 && (c - begin) < 0x400000;
            fprintf(R.f, "    @%#llx [%s] begin=%#llx end=%#llx cap=%#llx%s\n", (unsigned long long)o.addr, Rel(o.addr, *R.c, rb, sizeof rb),
                    (unsigned long long)begin, (unsigned long long)e, (unsigned long long)c,
                    vec ? "" : "  (not {begin,end,cap})");
            if (vec) fprintf(R.f, "      std::vector: size=%llu cap=%llu  <== LIVE COUNT\n", (unsigned long long)((e - begin) / 16), (unsigned long long)((c - begin) / 16));
            if (vec) {
                ChainToAnchor(R, o.addr, 1);
                // v6: the static anchor chain. Target = the object containing this
                // vector holder (anything pointing into the 8KB before it).
                if (label[0] == 'R') FindStaticPath(R, o.addr - 0x2000, o.addr + 0x18, label);
            }
        }
    }
    if (!nBuf) fprintf(R.f, "\n=== %s: no shared_ptr buffers ===\n", label);
}

static void DumpObject(Report& R, const char* name, uintptr_t obj, size_t len)
{
    if (!obj) return;
    char rb[64];
    fprintf(R.f, "\n--- %s %#llx [+0..+%#llx) heap-pointer qwords ---\n", name, (unsigned long long)obj, (unsigned long long)len);
    for (uintptr_t off = 0; off < len; off += 8) {
        const uint64_t v = U64(obj + off);
        if (!IsHeapPtr(v) && !(v >= R.img->lo && v < R.img->hi)) continue;
        const char* note = "";
        const uint64_t m = v ^ kMask;
        if (R.setP->count(m)) note = "= live P member";
        else if (IsSpPair(obj + off, *R.img)) note = "= shared_ptr pair here";
        else if (IsHeapPtr(v) && IsSpPair((uintptr_t)v, *R.img) && IsSpPair((uintptr_t)v + 16, *R.img)) note = "= points at a shared_ptr array (vector begin?)";
        char nm[128] = ""; if (LooksLikeVtable(v, *R.img)) RttiName(v, *R.img, nm, sizeof nm);
        fprintf(R.f, "  +%#llx = %#llx [%s] %s %s\n", (unsigned long long)off, (unsigned long long)v, Rel((uintptr_t)v, *R.c, rb, sizeof rb), note, nm);
    }
}

// full traversal of a map from its head; reports live/non-live membership
static void ReportMap(Report& R, uintptr_t head, const char* label, bool roleP)
{
    const uintptr_t root = (uintptr_t)U64(head + 8);
    AVec<uintptr_t> stack; ASet<uintptr_t> seen; stack.push_back(root);
    int count = 0, inP = 0, notInP = 0; uint32_t minKey = 0xFFFFFFFF, maxKey = 0;
    struct Sample { uint32_t key; uint64_t val; bool shape; };
    AVec<Sample> samples;
    while (!stack.empty() && count < 50000) {
        const uintptr_t n = stack.back(); stack.pop_back();
        if (!IsHeapPtr(n) || n == head || seen.count(n) || U8(n + 0x19) == 1) continue;
        seen.insert(n); ++count;
        const uint32_t key = U32(n + 0x20); const uint64_t val = U64(n + 0x28);
        minKey = (std::min)(minKey, key); maxKey = (std::max)(maxKey, key);
        if (R.setP->count(val ^ kMask)) ++inP;
        else {
            ++notInP;
            if (samples.size() < 12) {
                bool shape = false;
                if (IsHeapPtr(val)) { uint32_t id = 0; shape = roleP ? LooksLikeRole((uintptr_t)val, *R.img, &id) : LooksLikeMapItem((uintptr_t)val); }
                samples.push_back({key, val, shape});
            }
        }
        stack.push_back((uintptr_t)U64(n)); stack.push_back((uintptr_t)U64(n + 0x10));
    }
    fprintf(R.f, "\n=== %s std::map head=%#llx: %d nodes, %d hold a live P member, %d do not; keys %u..%u ===\n",
            label, (unsigned long long)head, count, inP, notInP, minKey, maxKey);
    for (const Sample& s : samples)
        fprintf(R.f, "    non-P node: key=%u val=%#llx%s\n", s.key, (unsigned long long)s.val,
                s.shape ? "  (passes shape check but wasn't in P: id-dedupe or no name)" : (IsHeapPtr(s.val) ? "  (heap ptr, fails shape: different type or ghost)" : "  (not a heap ptr)"));
    OwnerSearch(R, head, "map head", count);
}

static void ReportRing(Report& R, uintptr_t node, const char* label, int valOff)
{
    AVec<uintptr_t> ring; ASet<uintptr_t> seen; uintptr_t n = node; int inP = 0;
    while (IsHeapPtr(n) && !seen.count(n) && ring.size() < 50000) {
        seen.insert(n); ring.push_back(n);
        if (R.setP->count(U64(n + valOff) ^ kMask)) ++inP;
        n = (uintptr_t)U64(n);
    }
    fprintf(R.f, "\n=== %s ring from node %#llx: %d nodes (%d hold a live P member; one is the sentinel) ===\n",
            label, (unsigned long long)node, (int)ring.size(), inP);
    // owner search: who (outside the ring) holds a ring node address?
    ASet<uint64_t> t; for (uintptr_t r : ring) t.insert((uint64_t)r ^ kMask);
    uint64_t q = 0; const AVec<Hit> hits = ScanFor(*R.regions, t, &q, 200000);
    AVec<uintptr_t> outside;
    for (const Hit& h : hits) if (!seen.count(h.addr) && !seen.count(h.addr - 8)) outside.push_back(h.addr);   // skip next/prev fields of ring nodes
    const AVec<Run> runs8 = ContiguousRuns(outside, 8, 4), runs16 = ContiguousRuns(outside, 16, 4);
    fprintf(R.f, "  %d qwords outside the ring hold a ring node; stride-8 runs: %d, stride-16 runs: %d (bucket arrays)\n",
            (int)outside.size(), (int)runs8.size(), (int)runs16.size());
    char rb[64];
    for (size_t i = 0; i < runs8.size() && i < 3; ++i) fprintf(R.f, "    run8 @%#llx len=%d\n", (unsigned long long)runs8[i].start, runs8[i].len);
    for (size_t i = 0; i < runs16.size() && i < 3; ++i) fprintf(R.f, "    run16 @%#llx len=%d\n", (unsigned long long)runs16[i].start, runs16[i].len);
    // singletons = not inside any run -> candidates for _Myhead
    ASet<uintptr_t> inRun;
    for (const Run& r : runs8) for (int k = 0; k < r.len; ++k) inRun.insert(r.start + 8*k);
    for (const Run& r : runs16) for (int k = 0; k < r.len; ++k) inRun.insert(r.start + 16*k);
    int printed = 0;
    for (uintptr_t a : outside) {
        if (inRun.count(a)) continue;
        const uintptr_t to = (uintptr_t)U64(a);
        fprintf(R.f, "    singleton holder @%#llx [%s] = %#llx  next qword=%llu%s\n", (unsigned long long)a, Rel(a, *R.c, rb, sizeof rb),
                (unsigned long long)to, (unsigned long long)U64(a + 8), (long long)U64(a + 8) == (long long)ring.size() - 1 ? "  <== equals element count: {_Myhead,_Mysize}" : "");
        if (++printed >= 20) break;
    }
}

// classify every hit of one object class and drive the container reports
static void Classify(Report& R, const char* label, const AVec<Hit>& hits, const AMap<uint64_t, uint32_t>& idOf, bool roleP, int printLimit)
{
    char rb[64];
    int nMap = 0, nMapKey = 0, nRing = 0, nArr8 = 0, nArr16 = 0, nSp = 0, nOther = 0, printed = 0;
    ASet<uintptr_t> heads, ringsSeen;
    AVec<uintptr_t> firstUmapNodes;
    ASet<uintptr_t> hitAddrs; for (const Hit& h : hits) hitAddrs.insert(h.addr);
    fprintf(R.f, "\n--- %s: %d hits, per-hit structure classification ---\n", label, (int)hits.size());
    for (const Hit& h : hits) {
        const uint32_t id = idOf.count(h.maskedVal) ? idOf.at(h.maskedVal) : 0;
        const uintptr_t A = h.addr;
        char kinds[256] = ""; size_t kl = 0;
        auto add = [&](const char* s) { kl += snprintf(kinds + kl, sizeof(kinds) - kl, "%s%s", kl ? "," : "", s); };
        uintptr_t node = 0; bool keyOk = false;
        if (CheckMapNode(A, id, &node, &keyOk)) {
            ++nMap; if (keyOk) ++nMapKey;
            add(keyOk ? "MAP-NODE(key==id)" : "MAP-NODE(key!=id)");
            if (heads.size() < 4) { const uintptr_t head = ClimbToHead(node); if (head && !heads.count(head)) heads.insert(head); }
            if (nMap <= 6) {   // raw dump so a non-std layout can be read by hand
                fprintf(R.f, "    node %#llx raw:", (unsigned long long)node);
                for (int o = 0; o < 0x40; o += 8) fprintf(R.f, " +%#x:%#llx", o, (unsigned long long)U64(node + o));
                const uintptr_t third = (uintptr_t)U64(node + 0x10);
                fprintf(R.f, "\n    3rd slot %#llx [%s] raw:", (unsigned long long)third, Rel(third, *R.c, rb, sizeof rb));
                for (int o = 0; o < 0x40; o += 8) fprintf(R.f, " +%#x:%#llx", o, (unsigned long long)U64(third + o));
                fprintf(R.f, "\n");
            }
        }
        const char* rk = nullptr;
        if (CheckRingNode(A - 0x18, &rk, id, A, &keyOk) || CheckRingNode(A - 0x10, &rk, id, A, &keyOk)) {
            ++nRing; add(rk); if (keyOk) add("key==id");
            const uintptr_t rn = (rk[0] == 'u') ? A - 0x18 : A - 0x10;
            if (firstUmapNodes.size() < 4) firstUmapNodes.push_back(rn);
        }
        const bool arr8 = hitAddrs.count(A - 8) || hitAddrs.count(A + 8);
        const bool arr16 = !arr8 && (hitAddrs.count(A - 16) || hitAddrs.count(A + 16)) && IsHeapPtr(U64(A + 8));
        if (arr8) { ++nArr8; add("ARRAY8"); }
        if (arr16) { ++nArr16; add("ARRAY16/shared_ptr-vec"); }
        if (CheckSharedPtr(A, *R.img)) { ++nSp; add("shared_ptr{ptr,ctrl}"); }
        if (!kl) { ++nOther; add("?"); }
        if (printed < printLimit || (node && printed < printLimit + 40)) {
            ++printed;
            fprintf(R.f, "  hit @%#llx [%s] id=%u  %s  | -0x28:%#llx -0x20:%#llx -0x18:%#llx -0x10:%#llx -0x8:%#llx +0x8:%#llx\n",
                    (unsigned long long)A, Rel(A, *R.c, rb, sizeof rb), id, kinds,
                    (unsigned long long)U64(A-0x28), (unsigned long long)U64(A-0x20), (unsigned long long)U64(A-0x18),
                    (unsigned long long)U64(A-0x10), (unsigned long long)U64(A-8), (unsigned long long)U64(A+8));
        }
    }
    fprintf(R.f, "  summary: map-node %d (key==id %d), ring-node %d, array8 %d, array16 %d, shared_ptr %d, unclassified %d\n",
            nMap, nMapKey, nRing, nArr8, nArr16, nSp, nOther);
    for (uintptr_t head : heads) ReportMap(R, head, label, roleP);
    // rings: dedupe by membership
    for (uintptr_t rn : firstUmapNodes) {
        if (ringsSeen.count(rn)) continue;
        uintptr_t n = rn; int guard = 0;
        while (IsHeapPtr(n) && !ringsSeen.count(n) && guard++ < 50000) { ringsSeen.insert(n); n = (uintptr_t)U64(n); }
        ReportRing(R, rn, label, 0x18);
    }
}

// v7: does CRoleMgr reach the role-set object (two shared_ptr<CRole> vectors
// 0x30 apart, the +0x1348 one containing hero) at all, and at what depth?
// Rooted ONLY at CRoleMgr — no image/hero roots competing for the search
// budget, unlike v6's forward search, which is why that run's CRoleMgr
// candidates never got explored deeply. Wide at shallow depth (CRoleMgr is a
// modest fixed struct), tapering as depth grows to bound cost.
static bool RolesVecContainsHeroV7(uintptr_t begin, uintptr_t end, uintptr_t hero)
{
    if (end < begin || (end - begin) % 16 != 0) return false;
    const size_t n = (size_t)((end - begin) / 16);
    if (n == 0 || n > 4096) return false;
    for (size_t i = 0; i < n; ++i) { uint64_t obj = 0; if (!TryRead(begin + 16 * i, &obj)) return false; if (obj == hero) return true; }
    return false;
}
static bool LooksLikeRoleSetV7(uintptr_t obj, uintptr_t hero)
{
    uint64_t ab = 0, ae = 0, ac = 0, rb = 0, re = 0, rc = 0;
    if (!TryRead(obj + 0x1318, &ab) || !TryRead(obj + 0x1320, &ae) || !TryRead(obj + 0x1328, &ac)) return false;
    if (!(ab == 0 && ae == 0) && (!IsHeapPtr(ab) || ae < ab || ac < ae || (ae - ab) % 16 != 0)) return false;
    if (!TryRead(obj + 0x1348, &rb) || !TryRead(obj + 0x1350, &re) || !TryRead(obj + 0x1358, &rc)) return false;
    if (rb == 0 || !IsHeapPtr(rb) || re < rb || rc < re || (re - rb) % 16 != 0) return false;
    return RolesVecContainsHeroV7(rb, re, hero);
}
static void SearchFromRoleMgr(Report& R, uintptr_t roleMgr, uintptr_t hero)
{
    fprintf(R.f, "\n=== v7: dedicated CRoleMgr-only search for the role-set signature (roleMgr=%#llx hero=%#llx) ===\n",
            (unsigned long long)roleMgr, (unsigned long long)hero);
    if (!roleMgr || !hero) { fprintf(R.f, "  no CRoleMgr/hero\n"); return; }
    if (LooksLikeRoleSetV7(roleMgr, hero)) { fprintf(R.f, "  MATCH at depth 0: CRoleMgr itself\n"); return; }
    struct N { uintptr_t addr, srcField; int parent; uint8_t depth; };
    AVec<N> nodes; ASet<uintptr_t> seen; size_t qh = 0;
    seen.insert(roleMgr); nodes.push_back({roleMgr, 0, -1, 0});
    int matches = 0;
    // v8 [SAFETY]: was 4,000,000. A run that never finds enough matches keeps
    // growing `seen` (an unordered_set in the tool's bump allocator, which
    // never frees on rehash) without bound, risking std::bad_alloc — UNCAUGHT,
    // which terminates the thread via std::terminate()/abort() and can take
    // the whole injected process down with it. Lowered with margin below what
    // completed in ~1.1s on a working run (166K nodes), and Run_ now wraps
    // this in try/catch so ANY exception here exits the thread cleanly
    // instead of crashing the host process.
    const size_t kMaxNodes = 300000; const uint8_t kMaxDepth = 5; const DWORD t0 = GetTickCount();
    const DWORD kWallClockCapMs = 8000;
    const uintptr_t spanAtDepth[6] = { 0x20000, 0x20000, 0x8000, 0x2000, 0x800, 0x400 };
    while (qh < nodes.size() && nodes.size() < kMaxNodes && matches < 6 && GetTickCount() - t0 < kWallClockCapMs) {
        const int ni = (int)qh++; const N cur = nodes[ni];
        if (cur.depth >= kMaxDepth) continue;
        const uintptr_t span = spanAtDepth[cur.depth];
        for (uintptr_t off = 0; off < span; off += 8) {
            uint64_t v = 0; if (!TryRead(cur.addr + off, &v)) { if (off == 0) break; continue; }
            if (!IsHeapPtr(v) || seen.count((uintptr_t)v)) continue;
            seen.insert((uintptr_t)v);
            const int newIdx = (int)nodes.size();
            nodes.push_back({(uintptr_t)v, cur.addr + off, ni, (uint8_t)(cur.depth + 1)});
            if (LooksLikeRoleSetV7((uintptr_t)v, hero)) {
                ++matches;
                AVec<int> chain; for (int i2 = newIdx; i2 >= 0; i2 = nodes[i2].parent) chain.push_back(i2);
                fprintf(R.f, "  MATCH #%d depth %d: CRoleMgr", matches, cur.depth + 1);
                for (size_t k = chain.size(); k-- > 0;) {
                    const N& c2 = nodes[chain[k]];
                    if (c2.parent < 0) continue;
                    fprintf(R.f, " -> [+%#llx] = %#llx", (unsigned long long)(c2.srcField - nodes[c2.parent].addr), (unsigned long long)c2.addr);
                }
                fprintf(R.f, "\n");
            }
        }
    }
    fprintf(R.f, "  searched %d objects (cap %d), %d matches, %lu ms%s\n", (int)nodes.size(), (int)kMaxNodes, matches,
            GetTickCount() - t0, nodes.size() >= kMaxNodes ? "  (node cap hit)" : (qh >= nodes.size() ? "  (exhausted)" : ""));
}

// v8: settle the CRoleMgr+0x40 "hash-bucket-array of role pointers" question
// live. Two conflicting written analyses exist (HEAP_SCANNER_REPLACEMENT.md:
// disproven, slots hold UTF-16 text / hero-interior aliases; ROLES_CONTAINER_
// NOTE.md: confirmed, sparse bucket table with a monotonic 4th-qword counter).
// Test directly against the game's own state instead of trusting either
// paper analysis: walk the claimed 0x20-byte records, and for each slot-0
// value that looks like a heap pointer, run it through the SAME CRole shape
// check (vtable-in-image + id + position) the rest of this tool already
// trusts. Also flag inline-text slots and check the monotonic-counter claim.
static void DumpRoleMgrHashHypothesis(FILE* f, const Ctx& c)
{
    fprintf(f, "\n=== v8: CRoleMgr+0x40 hash-bucket-array hypothesis (ROLES_CONTAINER_NOTE.md vs HEAP_SCANNER_REPLACEMENT.md) ===\n");
    if (!c.roleMgr) { fprintf(f, "  no CRoleMgr\n"); return; }
    uint64_t h0 = 0, h10 = 0, h18 = 0, h30 = 0;
    TryRead(c.roleMgr, &h0); TryRead(c.roleMgr + 0x10, &h10); TryRead(c.roleMgr + 0x18, &h18); TryRead(c.roleMgr + 0x30, &h30);
    fprintf(f, "  header: +0x00=%#llx +0x10=%#llx +0x18=%#llx +0x30=%#llx\n",
            (unsigned long long)h0, (unsigned long long)h10, (unsigned long long)h18, (unsigned long long)h30);

    const int kMaxRecords = 4096;
    int slotsHeapPtr = 0, slotsPassShape = 0, slotsText = 0, printed = 0;
    AVec<uint64_t> counters;
    for (int i = 0; i < kMaxRecords; ++i) {
        const uintptr_t rec = c.roleMgr + 0x40 + (uintptr_t)i * 0x20;
        uint64_t p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        if (!TryRead(rec, &p0)) break;   // walked off mapped memory
        TryRead(rec + 8, &p1); TryRead(rec + 0x10, &p2); TryRead(rec + 0x18, &p3);
        if (p0 == 0 && p1 == 0 && p2 == 0 && p3 == 0) { counters.push_back(0); continue; }
        counters.push_back(p3);
        if (IsHeapPtr(p0)) {
            ++slotsHeapPtr;
            uint32_t id = 0;
            if (LooksLikeRole(p0, c.img, &id)) {
                ++slotsPassShape;
                char nm[16] = {};
                const bool named = HasRealName(p0, nm);
                if (printed < 25) {
                    ++printed;
                    fprintf(f, "  [%d] rec@%#llx p0=%#llx id=%u name='%s' raw={%#llx,%#llx,%#llx,%#llx}\n",
                            i, (unsigned long long)rec, (unsigned long long)p0, id, named ? nm : "?",
                            (unsigned long long)p0, (unsigned long long)p1, (unsigned long long)p2, (unsigned long long)p3);
                }
            }
        } else {
            const unsigned char* b = (const unsigned char*)&p0; bool text = true;
            for (int k = 0; k < 8; ++k) { const unsigned char ch = b[k]; if (ch != 0 && (ch < 0x20 || ch > 0x7E)) { text = false; break; } }
            if (text) ++slotsText;
        }
    }
    fprintf(f, "  scanned %d records: %d slot-0 values are heap-pointer-shaped, %d PASS the CRole shape check, %d look like inline ASCII text\n",
            (int)counters.size(), slotsHeapPtr, slotsPassShape, slotsText);
    int incBy1 = 0, pairs = 0;
    for (size_t i = 1; i < counters.size(); ++i) {
        if (counters[i] == 0 && counters[i - 1] == 0) continue;
        ++pairs;
        if (counters[i] == counters[i - 1] + 1) ++incBy1;
    }
    fprintf(f, "  4th-qword monotonic-counter check: %d/%d consecutive non-empty record pairs increment by exactly 1\n", incBy1, pairs);
}

static DWORD WINAPI Run_(LPVOID mod)
{
    if (!g_arena.Init((size_t)1 << 30)) FreeLibraryAndExitThread((HMODULE)mod, 3);
    const DWORD tick = GetTickCount();
    FILE* f = nullptr; fopen_s(&f, "C:/Users/Public/heapfind.log", "a");
    if (!f) { char alt[128]; snprintf(alt, sizeof alt, "C:/Users/Public/heapfind_%lu.log", tick); fopen_s(&f, alt, "a"); }
    if (!f) FreeLibraryAndExitThread((HMODULE)mod, 1);
    fprintf(f, "\n=================== heapfind v8 run @%lu ===================\n", tick);

    // v8 [SAFETY]: everything below is READ-ONLY diagnostics inside the game's
    // process. An uncaught C++ exception on this thread (e.g. std::bad_alloc
    // from the tool's own bump-allocated containers growing unbounded) would
    // call std::terminate()/abort() — which can take the whole host process
    // down, not just this thread. Catch everything, log it, exit clean.
    try {
    Ctx c{}; c.img = ModuleRange(GetModuleHandleA(nullptr)); c.base = (uintptr_t)c.img.lo;
    c.roleMgr = (uintptr_t)U64(c.base + RVA_ROLE_MGR_PTR);
    c.scene   = (uintptr_t)U64(c.base + RVA_SCENE_PTR);
    c.hero    = c.roleMgr ? (uintptr_t)U64(c.roleMgr) : 0;
    const ImageRange self = ModuleRange((HMODULE)mod);
    fprintf(f, "image %#llx..%#llx  roleMgr=%#llx  hero=%#llx  scene=%#llx  arena=%#llx\n",
            (unsigned long long)c.img.lo, (unsigned long long)c.img.hi, (unsigned long long)c.roleMgr,
            (unsigned long long)c.hero, (unsigned long long)c.scene, (unsigned long long)(uintptr_t)g_arena.base);
    if (GetModuleHandleA("coclassic.dll")) fprintf(f, "WARNING: coclassic.dll is loaded - its scan snapshots will pollute the array results\n");

    {
        Report R0{ f, &c, nullptr, nullptr, &c.img };
        SearchFromRoleMgr(R0, c.roleMgr, c.hero);
    }
    DumpRoleMgrHashHypothesis(f, c);

    // ── phase 1: shape scan -> P ──
    const AVec<Region> regions = CollectRegions(c, self);
    AVec<Found> roles, items;
    ASet<uint32_t> seenRole, seenItem;
    ASet<uint64_t> setRole, setItem;
    AMap<uint64_t, uint32_t> idOfRole, idOfItem;
    DWORD t0 = GetTickCount();
    int nImageRegions = 0;
    for (const Region& r : regions) {
        if (r.image) { ++nImageRegions; continue; }
        for (uintptr_t a = r.base; a + 0xE0 < r.base + r.size; a += 0x10) {
            uint32_t id = 0;
            if (LooksLikeRole(a, c.img, &id)) {
                char nm[16];
                if (HasRealName(a, nm) && !seenRole.count(id) && roles.size() < 512) {
                    seenRole.insert(id); Found fd{}; fd.maskedPtr = (uint64_t)a ^ kMask; fd.id = id;
                    fd.x = I32(a+0xD8); fd.y = I32(a+0xDC); memcpy(fd.name, nm, 16); roles.push_back(fd);
                    setRole.insert(fd.maskedPtr); idOfRole[fd.maskedPtr] = id;
                }
            }
            if (LooksLikeMapItem(a)) {
                const uint32_t iid = U32(a);
                if (!seenItem.count(iid) && items.size() < 4096) {
                    seenItem.insert(iid); Found fd{}; fd.maskedPtr = (uint64_t)a ^ kMask; fd.id = iid; fd.type = U32(a+4);
                    fd.x = I32(a+8); fd.y = I32(a+12); items.push_back(fd);
                    setItem.insert(fd.maskedPtr); idOfItem[fd.maskedPtr] = iid;
                }
            }
        }
    }
    fprintf(f, "phase 1 (shape scan): %d heap regions + %d image data regions, %d roles, %d items, %lu ms\n",
            (int)regions.size() - nImageRegions, nImageRegions, (int)roles.size(), (int)items.size(), GetTickCount() - t0);
    if (roles.empty() && items.empty()) { fprintf(f, "nothing found - abort\n"); fclose(f); FreeLibraryAndExitThread((HMODULE)mod, 2); }

    // ── phase 2: inverse search (one pass) ──
    t0 = GetTickCount();
    ASet<uint64_t> both; both.insert(setRole.begin(), setRole.end()); both.insert(setItem.begin(), setItem.end());
    uint64_t q2 = 0;
    const AVec<Hit> hits2 = ScanFor(regions, both, &q2, 400000);
    AVec<Hit> roleHits, itemHits; AVec<uintptr_t> roleAddrs, itemAddrs;
    for (const Hit& h : hits2) {
        if (setRole.count(h.maskedVal)) { roleHits.push_back(h); roleAddrs.push_back(h.addr); }
        if (setItem.count(h.maskedVal)) { itemHits.push_back(h); itemAddrs.push_back(h.addr); }
    }
    fprintf(f, "phase 2 (inverse search): %llu qwords, %d hits (%d role, %d item), %lu ms\n",
            (unsigned long long)q2, (int)hits2.size(), (int)roleHits.size(), (int)itemHits.size(), GetTickCount() - t0);
    auto printRuns = [&](const char* label, const AVec<uintptr_t>& addrs, int stride, int minLen) {
        const AVec<Run> runs = ContiguousRuns(addrs, stride, minLen);
        fprintf(f, "--- %s stride-%d runs (%d) ---\n", label, stride, (int)runs.size());
        for (size_t i = 0; i < runs.size() && i < 6; ++i) {
            ASet<uint64_t> d;
            for (int k = 0; k < runs[i].len; ++k) { const uint64_t v = U64(runs[i].start + stride*k) ^ kMask; if (both.count(v)) d.insert(v); }
            fprintf(f, "  run @%#llx len=%d distinct=%d\n", (unsigned long long)runs[i].start, runs[i].len, (int)d.size());
        }
    };
    printRuns("ROLE", roleAddrs, 8, 8);  printRuns("ROLE", roleAddrs, 16, 8);
    printRuns("ITEM", itemAddrs, 8, 16); printRuns("ITEM", itemAddrs, 16, 16);

    // ── phase 3: structure classification + container reports ──
    Report R{ f, &c, &regions, &setRole, &c.img };
    t0 = GetTickCount();
    Classify(R, "ROLES", roleHits, idOfRole, true, 40);
    DiscoverSpBuffers(R, "ROLES", roleHits, idOfRole);
    fprintf(f, "(roles: %lu ms)\n", GetTickCount() - t0);
    R.setP = &setItem; t0 = GetTickCount();
    Classify(R, "ITEMS", itemHits, idOfItem, false, 20);
    DiscoverSpBuffers(R, "ITEMS", itemHits, idOfItem);
    fprintf(f, "(items: %lu ms)\n", GetTickCount() - t0);

    // ── phase 4: anchor objects, annotated ──
    R.setP = &both;
    DumpObject(R, "CRoleMgr", c.roleMgr, 0x1000);
    DumpObject(R, "scene", c.scene, 0x400);
    fprintf(f, "arena used: %llu KB\n=== done ===\n", (unsigned long long)(g_arena.used >> 10)); fclose(f);
    } catch (const std::exception& e) {
        fprintf(f, "\nEXCEPTION - aborting run: %s\n", e.what()); fclose(f);
        FreeLibraryAndExitThread((HMODULE)mod, 4);
    } catch (...) {
        fprintf(f, "\nUNKNOWN EXCEPTION - aborting run\n"); fclose(f);
        FreeLibraryAndExitThread((HMODULE)mod, 5);
    }
    FreeLibraryAndExitThread((HMODULE)mod, 0);
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(nullptr, 0, Run_, h, 0, nullptr); }
    return TRUE;
}
