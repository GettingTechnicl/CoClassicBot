// =====================================================================
// imgdump.cpp — standalone READ-ONLY full-image + paired object-layout capture.
//
// Purpose: the "one-shot" v1074 capture (docs/investigation/CLIENT_V1074_BASELINE.md,
// docs/investigation/V1074_CAPTURE_SESSION.md). Themida decrypts lazily, so the
// decrypted image of a logged-in, feature-exercised client can only be taken from a
// RUNNING v1074 client, and only while the server still accepts v1074. This DLL takes
// numbered checkpoints on demand; each one holds, all from the SAME moment:
//   image.bin        flat copy of the whole main-module image (SizeOfImage bytes;
//                    zero-filled where a page was not safely readable)
//   regions.csv      per-VirtualQuery-region map of the image (what was/wasn't read)
//   pagehash.bin     FNV-1a-64 of every 4 KB page (cheap decrypt-progress diffing)
//   sections.csv     PE section table as loaded
//   modules.csv      every loaded module (base, size, PE stamp, path)
//   vmmap.csv        VirtualQuery walk of the whole address space (heap layout)
//   objects.pack/.csv  raw bytes of the live objects the bot's verified offsets point
//                    at (hero, stat table, role manager chain, item registry, net
//                    connection, map descriptor) plus a bounded pointer-graph
//                    expansion, with an index of name/address/size/parent
//   meta.json        build identity, hero/map id (so dumps pair up), timings, counts
//
// SAFETY (same posture as selftest.cpp / explorer.cpp): reads only. No hooks, no
// patches, no debug registers, no calls into game code (the connection object is read
// from its static storage, not by calling the accessor). Pages that are uncommitted,
// PAGE_NOACCESS, or PAGE_GUARD are NEVER touched — so Themida's exception-driven
// decrypt/guard machinery is not provoked. Every read is SEH-guarded, every loop is
// capped (nodes / bytes / wall-clock), the whole body is exception-safe, and it yields
// between chunks. If the host is not the v1074 build (PE TimeDateStamp 0x6A51CFB9,
// SizeOfImage 0x28C5000) it runs in generic mode: image + sections + modules + vmmap
// only, no object roots (their RVAs are meaningless on another build). That same
// stamp test is the seed of the runtime build fence planned for coclassic.dll.
//
// TRIGGERS (poll every 100 ms): Ctrl+Shift+F9 = checkpoint, Ctrl+Shift+F10 = unload;
// or write "checkpoint <label>" (image) / "full <label>" (image + private-heap pack, the mode for a
// build whose object roots are unknown) / "quit" to <base>\trigger.txt (tools/capture_session.ps1
// does this). Output root: C:\Users\Public\coclassic_capture\ ; the current session dir
// is the first line of <base>\session.txt (created automatically if absent).
// =====================================================================
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>

namespace {

constexpr uint32_t kV1074Stamp     = 0x6A51CFB9;
constexpr uint32_t kV1074ImageSize = 0x28C5000;
constexpr const char* kBaseDir     = "C:\\Users\\Public\\coclassic_capture";

// v1074 RVAs / offsets (game.h Offsets::/GameRva::, CRole.h, CHero.h — all LIVE-VERIFIED).
constexpr uintptr_t kRvaRoleMgrPtr   = 0x69C730;   // *(base+..) = CRoleMgr*; hero = *roleMgr
constexpr uintptr_t kRvaMapItemVec   = 0x6994D8;   // vector<shared_ptr<CMapItem>> {begin,end,cap}
constexpr uintptr_t kRvaCurMapId     = 0x699560;   // u32 (+4 = alt survivor)
constexpr uintptr_t kRvaMapScenePtr  = 0x699370;   // heap object tracking the active map
constexpr uintptr_t kRvaMapGlobals2  = 0x6993B8;
constexpr uintptr_t kRvaConnStatic   = 0x69A4E8;   // storage returned by accessor 0xB7320; +0x20 = connection object
constexpr uintptr_t kRvaNetClientLit = 0x15E718;   // session-6f "real this" (literal address AND base+RVA both tried)
constexpr uintptr_t kRoleMgrHop1     = 0x4A98;     // CRoleMgr+.. -> intermediate
constexpr uintptr_t kRoleMgrHop2     = 0x9240;     // intermediate+.. -> role-set object
constexpr uintptr_t kHeroStatPtr     = 0x968;      // CHero+.. -> CStatTable*

HMODULE     g_self = nullptr;
std::string g_sessionDir;
FILE*       g_log = nullptr;
int         g_seq = 0;
uintptr_t   g_base = 0;
uint32_t    g_imgSize = 0, g_stamp = 0;
bool        g_isV1074 = false;
HANDLE      g_mutex = nullptr;

// ---------------------------------------------------------------- logging
void Log(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fflush(g_log);
}

// ---------------------------------------------------------------- safe reads
bool SafeCopy(void* dst, const void* src, size_t n)
{
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadableProt(DWORD prot, DWORD state)
{
    if (state != MEM_COMMIT) return false;
    if (prot & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    constexpr DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (prot & ok) != 0;
}

// Copy up to len bytes from addr, stopping at the first unreadable region.
size_t ReadSpan(uintptr_t addr, size_t len, uint8_t* out)
{
    size_t done = 0;
    while (done < len) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(addr + done), &mbi, sizeof(mbi))) break;
        if (!ReadableProt(mbi.Protect, mbi.State)) break;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        const size_t n = std::min<size_t>(len - done, regionEnd - (addr + done));
        if (!SafeCopy(out + done, reinterpret_cast<void*>(addr + done), n)) break;
        done += n;
    }
    return done;
}

bool Rd64(uintptr_t a, uint64_t* v) { return ReadSpan(a, 8, reinterpret_cast<uint8_t*>(v)) == 8; }
bool Rd32(uintptr_t a, uint32_t* v) { return ReadSpan(a, 4, reinterpret_cast<uint8_t*>(v)) == 4; }

// A user-mode pointer to committed, readable, NON-image memory (heap/mapped).
bool IsHeapPtr(uint64_t p)
{
    if (p < 0x10000 || p >= 0x00007FFF00000000ull || (p & 7)) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) return false;
    if (mbi.Type == MEM_IMAGE) return false;
    return ReadableProt(mbi.Protect, mbi.State);
}

uint64_t Fnv1a64(const uint8_t* d, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= d[i]; h *= 1099511628211ull; }
    return h;
}

// ---------------------------------------------------------------- paths
std::string Sanitize(std::string s)
{
    for (char& c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            c = '_';
    if (s.empty()) s = "cp";
    if (s.size() > 40) s.resize(40);
    return s;
}

std::string ResolveSessionDir()
{
    CreateDirectoryA(kBaseDir, nullptr);
    const std::string sf = std::string(kBaseDir) + "\\session.txt";
    if (FILE* f = fopen(sf.c_str(), "rb")) {
        char line[512] = {};
        if (fgets(line, sizeof(line), f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
            fclose(f);
            if (!s.empty()) { CreateDirectoryA(s.c_str(), nullptr); return s; }
        } else fclose(f);
    }
    time_t t = time(nullptr); tm lt; localtime_s(&lt, &t);
    char b[64]; strftime(b, sizeof(b), "session_%Y%m%d_%H%M%S", &lt);
    std::string s = std::string(kBaseDir) + "\\" + b;
    CreateDirectoryA(s.c_str(), nullptr);
    return s;
}

// ---------------------------------------------------------------- PE identity
bool ReadPeIdentity()
{
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    uint8_t hdr[0x400] = {};
    if (ReadSpan(g_base, sizeof(hdr), hdr) < 0x200) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hdr);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x300) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(hdr + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    g_stamp   = nt->FileHeader.TimeDateStamp;
    g_imgSize = nt->OptionalHeader.SizeOfImage;
    g_isV1074 = (g_stamp == kV1074Stamp && g_imgSize == kV1074ImageSize);
    return true;
}

// ---------------------------------------------------------------- image dump
struct ImageStats { uint64_t bytesRead = 0, bytesSkipped = 0; uint32_t regions = 0, pages = 0, nonzeroPages = 0; };

ImageStats DumpImage(const std::string& dir)
{
    ImageStats st;
    FILE* fi = fopen((dir + "\\image.bin").c_str(), "wb");
    FILE* fr = fopen((dir + "\\regions.csv").c_str(), "wb");
    FILE* fh = fopen((dir + "\\pagehash.bin").c_str(), "wb");
    if (!fi || !fr || !fh) { if (fi) fclose(fi); if (fr) fclose(fr); if (fh) fclose(fh); return st; }
    fprintf(fr, "rva,size,state,protect,type,readable,bytes_read\n");

    static uint8_t buf[65536];
    const uintptr_t end = g_base + g_imgSize;
    uintptr_t addr = g_base;
    int chunksSinceYield = 0;
    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
            // Unqueryable: emit zeros for the rest of this page and move on.
            memset(buf, 0, 4096); fwrite(buf, 1, 4096, fi);
            uint64_t z = 0; fwrite(&z, 8, 1, fh); st.pages++; st.bytesSkipped += 4096;
            addr += 4096; continue;
        }
        const uintptr_t regionEnd = std::min<uintptr_t>(end, reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);
        const bool rd = ReadableProt(mbi.Protect, mbi.State);
        uint64_t regionRead = 0;
        for (uintptr_t a = addr; a < regionEnd; ) {
            const size_t n = std::min<size_t>(sizeof(buf), regionEnd - a);   // multiples of 4 KB (region+image are page-aligned)
            bool got = false;
            if (rd) got = SafeCopy(buf, reinterpret_cast<void*>(a), n);
            if (!got) memset(buf, 0, n);
            fwrite(buf, 1, n, fi);
            for (size_t p = 0; p + 4096 <= n; p += 4096) {
                const uint64_t h = got ? Fnv1a64(buf + p, 4096) : 0;
                fwrite(&h, 8, 1, fh);
                st.pages++;
                if (got) {
                    bool nz = false;
                    for (size_t k = 0; k < 4096; k += 8) if (*reinterpret_cast<uint64_t*>(buf + p + k)) { nz = true; break; }
                    if (nz) st.nonzeroPages++;
                }
            }
            if (got) { regionRead += n; st.bytesRead += n; } else st.bytesSkipped += n;
            a += n;
            if (++chunksSinceYield >= 16) { chunksSinceYield = 0; Sleep(1); }
        }
        fprintf(fr, "0x%llX,0x%llX,0x%lX,0x%lX,0x%lX,%d,0x%llX\n",
                (unsigned long long)(addr - g_base), (unsigned long long)(regionEnd - addr),
                (unsigned long)mbi.State, (unsigned long)mbi.Protect, (unsigned long)mbi.Type,
                rd ? 1 : 0, (unsigned long long)regionRead);
        st.regions++;
        addr = regionEnd;
    }
    fclose(fi); fclose(fr); fclose(fh);
    return st;
}

void DumpSections(const std::string& dir)
{
    FILE* f = fopen((dir + "\\sections.csv").c_str(), "wb");
    if (!f) return;
    fprintf(f, "name,rva,virtual_size,raw_size,characteristics\n");
    uint8_t hdr[0x1000] = {};
    if (ReadSpan(g_base, sizeof(hdr), hdr) >= 0x400) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hdr);
        if (dos->e_lfanew > 0 && dos->e_lfanew < 0x800) {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(hdr + dos->e_lfanew);
            const auto* sec = IMAGE_FIRST_SECTION(nt);
            const WORD n = std::min<WORD>(nt->FileHeader.NumberOfSections, 64);
            for (WORD i = 0; i < n; ++i) {
                if (reinterpret_cast<const uint8_t*>(&sec[i + 1]) > hdr + sizeof(hdr)) break;
                char nm[9] = {}; memcpy(nm, sec[i].Name, 8);
                for (char& c : nm) if (c && (c < 0x20 || c > 0x7e)) c = '?';
                fprintf(f, "%s,0x%lX,0x%lX,0x%lX,0x%lX\n", nm, (unsigned long)sec[i].VirtualAddress,
                        (unsigned long)sec[i].Misc.VirtualSize, (unsigned long)sec[i].SizeOfRawData,
                        (unsigned long)sec[i].Characteristics);
            }
        }
    }
    fclose(f);
}

void DumpModules(const std::string& dir)
{
    FILE* f = fopen((dir + "\\modules.csv").c_str(), "wb");
    if (!f) return;
    fprintf(f, "base,size,pe_timedatestamp,path\n");
    HMODULE mods[1024]; DWORD need = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        const DWORD n = std::min<DWORD>(need / sizeof(HMODULE), 1024);
        for (DWORD i = 0; i < n; ++i) {
            MODULEINFO mi = {}; GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));
            char path[MAX_PATH] = {}; GetModuleFileNameA(mods[i], path, MAX_PATH);
            uint32_t stamp = 0; uint8_t h[0x200] = {};
            if (ReadSpan(reinterpret_cast<uintptr_t>(mods[i]), sizeof(h), h) >= 0x100) {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
                if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 && dos->e_lfanew < 0x180)
                    stamp = reinterpret_cast<const IMAGE_NT_HEADERS64*>(h + dos->e_lfanew)->FileHeader.TimeDateStamp;
            }
            fprintf(f, "0x%llX,0x%lX,0x%08X,\"%s\"\n", (unsigned long long)reinterpret_cast<uintptr_t>(mi.lpBaseOfDll),
                    (unsigned long)mi.SizeOfImage, stamp, path);
        }
    }
    fclose(f);
}

void DumpVmmap(const std::string& dir)
{
    FILE* f = fopen((dir + "\\vmmap.csv").c_str(), "wb");
    if (!f) return;
    fprintf(f, "base,alloc_base,size,state,protect,type,mapped_file\n");
    uintptr_t a = 0; uint32_t guard = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (guard++ < 400000 && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) {
        char mapped[MAX_PATH] = {};
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE)
            GetMappedFileNameA(GetCurrentProcess(), mbi.BaseAddress, mapped, MAX_PATH);
        if (mbi.State != MEM_FREE)
            fprintf(f, "0x%llX,0x%llX,0x%llX,0x%lX,0x%lX,0x%lX,\"%s\"\n",
                    (unsigned long long)reinterpret_cast<uintptr_t>(mbi.BaseAddress),
                    (unsigned long long)reinterpret_cast<uintptr_t>(mbi.AllocationBase),
                    (unsigned long long)mbi.RegionSize, (unsigned long)mbi.State,
                    (unsigned long)mbi.Protect, (unsigned long)mbi.Type, mapped);
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= a) break;
        a = next;
        if (a >= 0x00007FFFFFFF0000ull) break;
    }
    fclose(f);
}

// ---------------------------------------------------------------- object dump
struct ObjCtx {
    FILE* pack = nullptr; FILE* csv = nullptr;
    uint64_t packOff = 0;
    std::unordered_set<uintptr_t> seen;
    int nodes = 0, maxNodes = 12000;
    uint64_t bytes = 0, maxBytes = 96ull << 20;
    ULONGLONG deadline = 0;
    bool Budget() const { return nodes < maxNodes && bytes < maxBytes && GetTickCount64() < deadline; }
};

// Read + record one node. Returns bytes actually read (0 = skipped/unreadable).
size_t DumpNode(ObjCtx& c, const std::string& name, uintptr_t addr, size_t len, int depth,
                const std::string& parent, std::vector<uint8_t>* keep)
{
    if (!addr || !c.Budget()) return 0;
    if (!c.seen.insert(addr).second) return 0;
    std::vector<uint8_t> b(len);
    const size_t got = ReadSpan(addr, len, b.data());
    if (got == 0) return 0;
    fwrite(b.data(), 1, got, c.pack);
    const long long rva = (addr >= g_base && addr < g_base + g_imgSize) ? (long long)(addr - g_base) : -1;
    fprintf(c.csv, "\"%s\",0x%llX,%lld,%zu,%zu,%llu,%d,\"%s\"\n", name.c_str(), (unsigned long long)addr,
            rva, len, got, (unsigned long long)c.packOff, depth, parent.c_str());
    c.packOff += got; c.bytes += got; c.nodes++;
    if (keep) { b.resize(got); *keep = std::move(b); }
    return got;
}

// Scan a dumped buffer for heap pointers and dump what they point at (bounded).
void Expand(ObjCtx& c, const std::string& name, uintptr_t base, const std::vector<uint8_t>& bytes,
            int depth, int maxDepth, size_t childBytes, int cap)
{
    int used = 0;
    for (size_t off = 0; off + 8 <= bytes.size() && used < cap && c.Budget(); off += 8) {
        uint64_t p; memcpy(&p, bytes.data() + off, 8);
        if (!IsHeapPtr(p) || c.seen.count((uintptr_t)p)) continue;
        char nm[160]; snprintf(nm, sizeof(nm), "%s/+0x%zX", name.c_str(), off);
        std::vector<uint8_t> child;
        if (DumpNode(c, nm, (uintptr_t)p, childBytes, depth + 1, name, depth + 1 < maxDepth ? &child : nullptr)) {
            ++used;
            if (!child.empty() && depth + 1 < maxDepth)
                Expand(c, nm, (uintptr_t)p, child, depth + 1, maxDepth, childBytes, std::max(4, cap / 16));
        }
    }
    (void)base;
}

struct Pairing { uint32_t heroId = 0, level = 0, maxHp = 0, mapId = 0; char name[20] = {}; uint64_t hero = 0, roleMgr = 0; int items = 0; };

Pairing DumpObjects(const std::string& dir)
{
    Pairing pr;
    ObjCtx c;
    c.pack = fopen((dir + "\\objects.pack").c_str(), "wb");
    c.csv  = fopen((dir + "\\objects.csv").c_str(), "wb");
    if (!c.pack || !c.csv) { if (c.pack) fclose(c.pack); if (c.csv) fclose(c.csv); return pr; }
    fprintf(c.csv, "name,addr,rva,requested,read,pack_offset,depth,parent\n");
    c.deadline = GetTickCount64() + 30000;

    auto U64 = [](uintptr_t a) { uint64_t v = 0; Rd64(a, &v); return v; };

    // --- roots reachable from verified globals -------------------------------
    uint64_t roleMgr = U64(g_base + kRvaRoleMgrPtr);
    pr.roleMgr = roleMgr;
    uint64_t hero = IsHeapPtr(roleMgr) ? U64(roleMgr) : 0;
    pr.hero = hero;
    Rd32(g_base + kRvaCurMapId, &pr.mapId);

    std::vector<uint8_t> heroB, rmB, hop1B, rsB, stB;
    if (IsHeapPtr(hero)) {
        DumpNode(c, "hero", hero, 0x2400, 0, "", &heroB);
        Rd32(hero + 0x68, &pr.heroId); Rd32(hero + 0x6E8, &pr.level); Rd32(hero + 0x3D0, &pr.maxHp);
        uint8_t nm[16] = {}; ReadSpan(hero + 0x94, 15, nm);
        for (int i = 0; i < 15; ++i) { char ch = (char)nm[i]; if (!ch) break; pr.name[i] = (ch >= 0x20 && ch < 0x7f) ? ch : '?'; }
    }
    if (IsHeapPtr(roleMgr)) {
        DumpNode(c, "rolemgr", roleMgr, 0x5000, 0, "", &rmB);
        const uint64_t hop1 = U64(roleMgr + kRoleMgrHop1);
        if (IsHeapPtr(hop1)) {
            DumpNode(c, "rolemgr_hop1", hop1, 0x9400, 1, "rolemgr", &hop1B);
            const uint64_t rs = U64(hop1 + kRoleMgrHop2);
            if (IsHeapPtr(rs)) DumpNode(c, "roleset", rs, 0x1600, 2, "rolemgr_hop1", &rsB);
        }
    }
    if (IsHeapPtr(hero)) {
        const uint64_t st = U64(hero + kHeroStatPtr);
        if (IsHeapPtr(st)) DumpNode(c, "stattable", st, 0x800, 1, "hero", &stB);
    }
    // map / descriptor globals
    {
        const uint64_t scene = U64(g_base + kRvaMapScenePtr);
        if (IsHeapPtr(scene)) DumpNode(c, "mapscene_699370", scene, 0x400, 0, "", nullptr);
        const uint64_t g2 = U64(g_base + kRvaMapGlobals2);
        if (IsHeapPtr(g2)) DumpNode(c, "mapglobal_6993B8", g2, 0x200, 0, "", nullptr);
    }
    // network: connection object read from static storage (accessor 0xB7320 returns g_base+0x69A4E8)
    {
        DumpNode(c, "conn_static_69A4E8", g_base + kRvaConnStatic, 0x100, 0, "", nullptr);
        const uint64_t conn = U64(g_base + kRvaConnStatic + 0x20);
        if (IsHeapPtr(conn)) {
            std::vector<uint8_t> cb;
            DumpNode(c, "conn", conn, 0x2100, 0, "", &cb);
            if (!cb.empty()) Expand(c, "conn", conn, cb, 0, 1, 0x100, 24);
        }
        DumpNode(c, "netclient_literal_15E718", (uintptr_t)kRvaNetClientLit, 0x400, 0, "", nullptr);
        DumpNode(c, "netclient_base_15E718", g_base + kRvaNetClientLit, 0x400, 0, "", nullptr);
    }
    // item registry: vector<shared_ptr<CMapItem>> {begin,end,cap}, 16-byte elements
    {
        DumpNode(c, "itemvec_hdr", g_base + kRvaMapItemVec, 0x20, 0, "", nullptr);
        const uint64_t b = U64(g_base + kRvaMapItemVec), e = U64(g_base + kRvaMapItemVec + 8);
        if (IsHeapPtr(b) && e > b && (e - b) % 16 == 0 && (e - b) / 16 <= 100000) {
            const uint64_t n = (e - b) / 16;
            std::vector<uint8_t> arr;
            DumpNode(c, "itemvec_array", b, (size_t)std::min<uint64_t>(n, 512) * 16, 1, "itemvec_hdr", &arr);
            for (uint64_t i = 0; i < std::min<uint64_t>(n, 256) && (i + 1) * 16 <= arr.size(); ++i) {
                uint64_t ip; memcpy(&ip, arr.data() + i * 16, 8);
                if (!IsHeapPtr(ip)) continue;
                char nm[64]; snprintf(nm, sizeof(nm), "item[%llu]", (unsigned long long)i);
                std::vector<uint8_t> ib;
                if (DumpNode(c, nm, ip, 0x80, 2, "itemvec_array", &ib)) {
                    ++pr.items;
                    uint64_t info; memcpy(&info, ib.data() + std::min<size_t>(0x10, ib.size() - 8), 8);
                    if (ib.size() >= 0x18 && IsHeapPtr(info)) {
                        char n2[64]; snprintf(n2, sizeof(n2), "iteminfo[%llu]", (unsigned long long)i);
                        DumpNode(c, n2, info, 0x100, 3, nm, nullptr);
                    }
                }
            }
        }
    }
    // --- bounded pointer-graph expansion (how the current-HP record was found) --
    if (!stB.empty()) Expand(c, "stattable", 0, stB, 1, 3, 0x100, 64);      // depth-3 fan-out capped per level
    if (!heroB.empty()) Expand(c, "hero", hero, heroB, 0, 2, 0x200, 400);
    if (!rsB.empty())   Expand(c, "roleset", 0, rsB, 2, 3, 0x100, 200);
    if (!rmB.empty())   Expand(c, "rolemgr", roleMgr, rmB, 0, 2, 0x100, 300);

    Log("objects: %d nodes, %llu bytes, %s", c.nodes, (unsigned long long)c.bytes,
        c.Budget() ? "budget ok" : "BUDGET HIT (truncated)");
    fclose(c.pack); fclose(c.csv);
    return pr;
}


// Off-game test hook: if <base>\test_root.txt holds a hex address, dump that object and
// expand its pointer graph exactly like a real root, so the pack/index/graph code can be
// exercised on a harmless process. Never present during the real session.
void DumpTestRoot(const std::string& dir)
{
    const std::string tf = std::string(kBaseDir) + "\\test_root.txt";
    FILE* f = fopen(tf.c_str(), "rb");
    if (!f) return;
    char line[64] = {}; fgets(line, sizeof(line), f); fclose(f);
    const uintptr_t root = (uintptr_t)strtoull(line, nullptr, 16);
    if (!root) return;
    ObjCtx c;
    c.pack = fopen((dir + "\\objects_test.pack").c_str(), "wb");
    c.csv  = fopen((dir + "\\objects_test.csv").c_str(), "wb");
    if (!c.pack || !c.csv) { if (c.pack) fclose(c.pack); if (c.csv) fclose(c.csv); return; }
    fprintf(c.csv, "name,addr,rva,requested,read,pack_offset,depth,parent\n");
    c.deadline = GetTickCount64() + 10000;
    std::vector<uint8_t> b;
    if (DumpNode(c, "testroot", root, 0x1000, 0, "", &b)) Expand(c, "testroot", root, b, 0, 2, 0x100, 64);
    Log("test root 0x%llX: %d nodes, %llu bytes", (unsigned long long)root, c.nodes, (unsigned long long)c.bytes);
    fclose(c.pack); fclose(c.csv);
}

// ---------------------------------------------------------------- private-heap dump (generic / unknown-build mode)
// On a build whose object roots are not known yet (v1078...) the paired object dumps cannot follow verified
// pointers, so instead copy every committed, readable MEM_PRIVATE region (heaps, stacks, Themida's own
// allocations) into one pack. The hero / stat table / role manager / item registry / connection object are then
// found OFFLINE by correlating against the on-screen values recorded in the session notes (the way current HP was
// found), with no game code run. Same posture as the image dump: guard/noaccess/uncommitted pages are never
// touched, every copy is SEH-guarded, capped by bytes and wall-clock, and it yields between chunks.
constexpr uint64_t kHeapByteCap = 900ull * 1024 * 1024;
constexpr ULONGLONG kHeapMsCap  = 120000;

struct HeapStats { uint64_t bytes = 0, skipped = 0; uint32_t regions = 0; bool capped = false; };

HeapStats DumpHeap(const std::string& dir)
{
    HeapStats hs;
    FILE* fp = fopen((dir + "\\heap.bin").c_str(), "wb");
    FILE* fc = fopen((dir + "\\heap_regions.csv").c_str(), "wb");
    if (!fp || !fc) { if (fp) fclose(fp); if (fc) fclose(fc); return hs; }
    fprintf(fc, "addr,size,protect,pack_offset,bytes_read\n");
    static uint8_t buf[1 << 20];
    const ULONGLONG deadline = GetTickCount64() + kHeapMsCap;
    const uintptr_t imgLo = g_base, imgHi = g_base + g_imgSize;
    uint64_t packOff = 0;
    int chunks = 0;
    uintptr_t addr = 0x10000;
    while (addr < 0x00007FFF00000000ull) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t next = base + mbi.RegionSize;
        if (next <= addr) break;
        const bool inImage = base < imgHi && next > imgLo;
        if (mbi.Type == MEM_PRIVATE && !inImage && ReadableProt(mbi.Protect, mbi.State)) {
            if (hs.bytes + mbi.RegionSize > kHeapByteCap || GetTickCount64() > deadline) { hs.capped = true; break; }
            uint64_t got = 0;
            for (uintptr_t a = base; a < next; ) {
                const size_t n = std::min<size_t>(sizeof(buf), next - a);
                if (!SafeCopy(buf, reinterpret_cast<void*>(a), n)) break;   // stop this region at the first fault; keep what we have
                fwrite(buf, 1, n, fp);
                got += n; a += n;
                if (++chunks >= 8) { chunks = 0; Sleep(1); }
            }
            fprintf(fc, "0x%llX,0x%llX,0x%lX,0x%llX,0x%llX\n", (unsigned long long)base, (unsigned long long)mbi.RegionSize,
                    (unsigned long)mbi.Protect, (unsigned long long)packOff, (unsigned long long)got);
            packOff += got; hs.bytes += got; hs.skipped += mbi.RegionSize - got; ++hs.regions;
        }
        addr = next;
    }
    fclose(fp); fclose(fc);
    return hs;
}

// ---------------------------------------------------------------- checkpoint
void Checkpoint(const std::string& labelIn, bool withHeap)
{
    if (g_sessionDir.empty() || !g_seq) g_sessionDir = ResolveSessionDir();
    const std::string label = Sanitize(labelIn);
    ++g_seq;
    char sub[96]; snprintf(sub, sizeof(sub), "cp%02d_%s", g_seq, label.c_str());
    const std::string dir = g_sessionDir + "\\" + sub;
    CreateDirectoryA(dir.c_str(), nullptr);

    const ULONGLONG t0 = GetTickCount64();
    Log("checkpoint %s begin (v1074=%d)", sub, g_isV1074 ? 1 : 0);
    ImageStats is;
    try { is = DumpImage(dir); }
    catch (...) { Log("checkpoint %s: image dump threw", sub); }
    const ULONGLONG t1 = GetTickCount64();
    try { DumpSections(dir); DumpModules(dir); DumpVmmap(dir); }
    catch (...) { Log("checkpoint %s: sections/modules/vmmap threw; continuing", sub); }
    Pairing pr;
    try { if (g_isV1074) pr = DumpObjects(dir); }
    catch (...) { Log("checkpoint %s: object dump threw; image checkpoint is still valid", sub); }
    try { DumpTestRoot(dir); }
    catch (...) { Log("checkpoint %s: test-root dump threw", sub); }
    HeapStats hp;
    if (withHeap) {
        try { hp = DumpHeap(dir); Log("heap: %u regions, %llu bytes, %llu skipped%s", hp.regions, (unsigned long long)hp.bytes, (unsigned long long)hp.skipped, hp.capped ? " (CAPPED)" : ""); }
        catch (...) { Log("checkpoint %s: heap dump threw; image checkpoint is still valid", sub); }
    }
    const ULONGLONG t2 = GetTickCount64();

    if (FILE* m = fopen((dir + "\\meta.json").c_str(), "wb")) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(m, "{\n  \"checkpoint\": \"%s\",\n  \"label\": \"%s\",\n  \"local_time\": \"%04d-%02d-%02d %02d:%02d:%02d\",\n",
                sub, label.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(m, "  \"pid\": %lu,\n  \"module_base\": \"0x%llX\",\n  \"pe_timedatestamp\": \"0x%08X\",\n  \"size_of_image\": \"0x%X\",\n",
                (unsigned long)GetCurrentProcessId(), (unsigned long long)g_base, g_stamp, g_imgSize);
        fprintf(m, "  \"build_is_v1074\": %s,\n  \"image_bytes_read\": %llu,\n  \"image_bytes_skipped\": %llu,\n",
                g_isV1074 ? "true" : "false", (unsigned long long)is.bytesRead, (unsigned long long)is.bytesSkipped);
        fprintf(m, "  \"image_regions\": %u,\n  \"image_pages\": %u,\n  \"image_nonzero_pages\": %u,\n",
                is.regions, is.pages, is.nonzeroPages);
        fprintf(m, "  \"hero_name\": \"%s\",\n  \"hero_id\": %u,\n  \"hero_level\": %u,\n  \"hero_max_hp\": %u,\n  \"map_id\": %u,\n",
                pr.name, pr.heroId, pr.level, pr.maxHp, pr.mapId);
        fprintf(m, "  \"hero_ptr\": \"0x%llX\",\n  \"rolemgr_ptr\": \"0x%llX\",\n  \"ground_items_dumped\": %d,\n",
                (unsigned long long)pr.hero, (unsigned long long)pr.roleMgr, pr.items);
        fprintf(m, "  \"heap_dumped\": %s,\n  \"heap_regions\": %u,\n  \"heap_bytes\": %llu,\n  \"heap_capped\": %s,\n",
                withHeap ? "true" : "false", hp.regions, (unsigned long long)hp.bytes, hp.capped ? "true" : "false");
        fprintf(m, "  \"image_ms\": %llu,\n  \"objects_ms\": %llu\n}\n",
                (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1));
        fclose(m);
    }
    if (FILE* d = fopen((dir + "\\DONE").c_str(), "wb")) { fputs("ok\n", d); fclose(d); }
    if (FILE* d = fopen((std::string(kBaseDir) + "\\last_done.txt").c_str(), "wb")) { fprintf(d, "%s\n", dir.c_str()); fclose(d); }
    Log("checkpoint %s done: image %llu read / %llu skipped bytes, %u nonzero pages, %llu ms",
        sub, (unsigned long long)is.bytesRead, (unsigned long long)is.bytesSkipped, is.nonzeroPages,
        (unsigned long long)(GetTickCount64() - t0));
    MessageBeep(MB_OK);
}

// ---------------------------------------------------------------- worker
bool KeyEdge(int vk, bool& prev)
{
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = down && !prev;
    prev = down;
    return edge;
}

DWORD WINAPI Worker(LPVOID)
{
    try {
        CreateDirectoryA(kBaseDir, nullptr);
        g_log = fopen((std::string(kBaseDir) + "\\imgdump.log").c_str(), "ab");
        if (!ReadPeIdentity()) { Log("cannot read PE identity of the host image; exiting"); return 1; }
        Log("attached: base=0x%llX stamp=0x%08X size=0x%X v1074=%d", (unsigned long long)g_base, g_stamp, g_imgSize, g_isV1074 ? 1 : 0);
        MessageBeep(MB_ICONASTERISK);

        bool f9prev = false, f10prev = false;
        const std::string trig = std::string(kBaseDir) + "\\trigger.txt";
        for (;;) {
            Sleep(100);
            const bool chord = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
            bool f9 = KeyEdge(VK_F9, f9prev), f10 = KeyEdge(VK_F10, f10prev);
            if (chord && f9) { Checkpoint("hotkey", false); continue; }
            if (chord && f10) break;

            if (GetFileAttributesA(trig.c_str()) != INVALID_FILE_ATTRIBUTES) {
                char line[256] = {};
                if (FILE* f = fopen(trig.c_str(), "rb")) { fgets(line, sizeof(line), f); fclose(f); }
                DeleteFileA(trig.c_str());
                std::string s(line);
                while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
                if (s.rfind("quit", 0) == 0) break;
                if (s.rfind("checkpoint", 0) == 0 || s.rfind("full", 0) == 0) {
                    const bool full = s.rfind("full", 0) == 0;
                    const size_t skip = full ? 5 : 11;                 // "full " / "checkpoint "
                    Checkpoint(s.size() > skip ? s.substr(skip) : "cp", full);
                }
            }
        }
    } catch (...) {
        Log("worker: unexpected C++ exception; exiting");
    }
    Log("unloading");
    if (g_log) { fclose(g_log); g_log = nullptr; }
    if (g_mutex) { CloseHandle(g_mutex); g_mutex = nullptr; }
    FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        g_self = hMod;
        g_mutex = CreateMutexA(nullptr, TRUE, "coclassic_imgdump_v1");
        if (GetLastError() == ERROR_ALREADY_EXISTS) return TRUE;      // already running in this process
        if (HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(h);
    }
    return TRUE;
}
