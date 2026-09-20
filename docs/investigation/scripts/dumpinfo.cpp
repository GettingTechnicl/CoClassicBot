#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#pragma comment(lib, "dbghelp.lib")

struct Mod { ULONG64 base; ULONG size; std::wstring name; };
struct Range { ULONG64 start; ULONG64 size; const BYTE* data; };
static std::vector<Mod> mods;
static std::vector<Range> ranges;

static const Mod* FindMod(ULONG64 a) {
    for (auto& m : mods) if (a >= m.base && a < m.base + m.size) return &m;
    return nullptr;
}
static std::wstring Leaf(const std::wstring& p) { size_t i = p.find_last_of(L"\\/"); return i == std::wstring::npos ? p : p.substr(i + 1); }
static bool ReadMem(ULONG64 a, void* out, size_t n) {
    for (auto& r : ranges) if (a >= r.start && a + n <= r.start + r.size) { memcpy(out, r.data + (a - r.start), n); return true; }
    return false;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { printf("usage: dumpinfo <dump> [symdir]\n"); return 1; }
    HANDLE f = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) { printf("open fail\n"); return 1; }
    HANDLE mp = CreateFileMappingW(f, 0, PAGE_READONLY, 0, 0, 0);
    const BYTE* base = (const BYTE*)MapViewOfFile(mp, FILE_MAP_READ, 0, 0, 0);
    PMINIDUMP_DIRECTORY dir; PVOID st; ULONG sz;

    if (MiniDumpReadDumpStream((PVOID)base, ModuleListStream, &dir, &st, &sz)) {
        auto* ml = (MINIDUMP_MODULE_LIST*)st;
        for (ULONG i = 0; i < ml->NumberOfModules; i++) {
            auto& m = ml->Modules[i];
            auto* s = (MINIDUMP_STRING*)(base + m.ModuleNameRva);
            mods.push_back({ m.BaseOfImage, m.SizeOfImage, std::wstring(s->Buffer, s->Length / 2) });
        }
    }
    if (MiniDumpReadDumpStream((PVOID)base, MemoryListStream, &dir, &st, &sz)) {
        auto* ml = (MINIDUMP_MEMORY_LIST*)st;
        for (ULONG i = 0; i < ml->NumberOfMemoryRanges; i++) {
            auto& d = ml->MemoryRanges[i];
            ranges.push_back({ d.StartOfMemoryRange, d.Memory.DataSize, base + d.Memory.Rva });
        }
    }
    if (MiniDumpReadDumpStream((PVOID)base, Memory64ListStream, &dir, &st, &sz)) {
        auto* ml = (MINIDUMP_MEMORY64_LIST*)st;
        ULONG64 rva = ml->BaseRva;
        for (ULONG64 i = 0; i < ml->NumberOfMemoryRanges; i++) {
            auto& d = ml->MemoryRanges[i];
            ranges.push_back({ d.StartOfMemoryRange, d.DataSize, base + rva });
            rva += d.DataSize;
        }
    }
    printf("modules=%zu memranges=%zu\n", mods.size(), ranges.size());

    HANDLE hp = (HANDLE)0x1234;
    bool symok = false;
    if (argc >= 3) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_LOAD_ANYTHING);
        SymInitializeW(hp, argv[2], FALSE);
        symok = true;
        for (auto& m : mods) {
            std::wstring l = Leaf(m.name);
            std::transform(l.begin(), l.end(), l.begin(), ::towlower);
            if (l == L"coclassic.dll" || l == L"launcher.exe") {
                std::wstring p = std::wstring(argv[2]) + L"\\" + Leaf(m.name);
                SymLoadModuleExW(hp, 0, p.c_str(), 0, m.base, m.size, 0, 0);
            }
        }
    }
    auto Describe = [&](ULONG64 a) {
        const Mod* m = FindMod(a);
        char buf[600];
        if (!m) { snprintf(buf, sizeof buf, "0x%llx (no module)", a); return std::string(buf); }
        std::wstring l = Leaf(m->name);
        char nm[260]; WideCharToMultiByte(CP_UTF8, 0, l.c_str(), -1, nm, sizeof nm, 0, 0);
        snprintf(buf, sizeof buf, "%s+0x%llx", nm, a - m->base);
        std::string s = buf;
        if (symok) {
            alignas(8) char sb[sizeof(SYMBOL_INFO) + 256]; auto* si = (SYMBOL_INFO*)sb; si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (SymFromAddr(hp, a, &disp, si)) { snprintf(buf, sizeof buf, "  <%s+0x%llx>", si->Name, disp); s += buf; }
            IMAGEHLP_LINE64 ln = { sizeof ln }; DWORD ld = 0;
            if (SymGetLineFromAddr64(hp, a, &ld, &ln)) { snprintf(buf, sizeof buf, "  [%s:%lu]", ln.FileName, ln.LineNumber); s += buf; }
        }
        return s;
    };

    if (MiniDumpReadDumpStream((PVOID)base, ExceptionStream, &dir, &st, &sz)) {
        auto* ex = (MINIDUMP_EXCEPTION_STREAM*)st;
        printf("ExceptionCode=0x%08lX  Address=%s  ThreadId=%lu\n", ex->ExceptionRecord.ExceptionCode, Describe(ex->ExceptionRecord.ExceptionAddress).c_str(), ex->ThreadId);
        for (DWORD i = 0; i < ex->ExceptionRecord.NumberParameters && i < 4; i++) printf("  ExceptionInformation[%lu]=0x%llx\n", i, ex->ExceptionRecord.ExceptionInformation[i]);
        auto* c = (CONTEXT*)(base + ex->ThreadContext.Rva);
        printf("RIP=%s\nRSP=0x%llx RBP=0x%llx\nRAX=0x%llx RBX=0x%llx RCX=0x%llx RDX=0x%llx\nR8=0x%llx R9=0x%llx RSI=0x%llx RDI=0x%llx\n",
            Describe(c->Rip).c_str(), c->Rsp, c->Rbp, c->Rax, c->Rbx, c->Rcx, c->Rdx, c->R8, c->R9, c->Rsi, c->Rdi);
        BYTE code[32];
        if (ReadMem(c->Rip, code, 16)) { printf("bytes@RIP:"); for (int i = 0; i < 16; i++) printf(" %02X", code[i]); printf("\n"); }
        else printf("bytes@RIP: (not in dump)\n");
        int shown = 0;
        printf("--- stack scan from RSP (in-module qwords, first 60) ---\n");
        for (int i = 0; i < 2048 && shown < 60; i++) {
            ULONG64 v = 0;
            if (!ReadMem(c->Rsp + i * 8, &v, 8)) { if (i == 0) printf("(stack not in dump)\n"); break; }
            const Mod* m = FindMod(v);
            if (m) { printf("  [rsp+0x%03x] %s\n", i * 8, Describe(v).c_str()); shown++; }
        }
    } else printf("no exception stream\n");

    if (MiniDumpReadDumpStream((PVOID)base, ExceptionStream, &dir, &st, &sz)) {
        auto* ex = (MINIDUMP_EXCEPTION_STREAM*)st;
        auto* c = (CONTEXT*)(base + ex->ThreadContext.Rva);
        int n = 0, total = 0;
        printf("--- FULL stack scan: coclassic.dll / hook frames only ---\n");
        for (int i = 0; i < 65536; i++) {
            ULONG64 v = 0;
            if (!ReadMem(c->Rsp + (ULONG64)i * 8, &v, 8)) { printf("(stack readable for %d qwords = %d bytes)\n", i, i * 8); break; }
            const Mod* m = FindMod(v);
            if (!m) continue;
            total++;
            std::wstring l = Leaf(m->name); std::transform(l.begin(), l.end(), l.begin(), ::towlower);
            if (l.find(L"coclassic") != std::wstring::npos) { printf("  [rsp+0x%04x] %s\n", i * 8, Describe(v).c_str()); n++; }
        }
        printf("coclassic hits=%d of %d in-module qwords\n", n, total);
    }
    printf("--- module list (non-system) ---\n");
    for (auto& m : mods) {
        std::wstring l = Leaf(m.name);
        std::wstring low = m.name; std::transform(low.begin(), low.end(), low.begin(), ::towlower);
        if (low.find(L"\\windows\\") != std::wstring::npos) continue;
        char nm[400]; WideCharToMultiByte(CP_UTF8, 0, m.name.c_str(), -1, nm, sizeof nm, 0, 0);
        printf("  0x%llx size=0x%x %s\n", m.base, m.size, nm);
    }
    return 0;
}
