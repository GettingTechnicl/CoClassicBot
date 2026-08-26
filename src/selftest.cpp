// =====================================================================
// selftest.cpp — standalone READ-ONLY injected validator for v1074.
//
// Purpose: prove the live-verified offset fixes work in-process, and act as
// the seed of the self-test harness (REVAMP_PLAN Phase 3). This DLL does NOT
// hook game code, patch memory, or touch debug registers — it only reads.
// Every read is SEH-guarded so a wrong offset degrades to 0 instead of crashing
// the game. It waits for login, then writes C:\Users\Public\coclassic_selftest.json
// every few seconds with the values read through our fixed offsets.
//
// Offsets mirror game.h / CHero.h [LIVE-VERIFIED] fixes (v1074):
//   roleMgr = *(base + 0x69C730);  hero = *(roleMgr + 0x00)
//   name +0x94, id +0x68, maxHP +0x3D0, level +0x6E8,
//   silver +0xA80, equipment +0xBD8 (shared_ptr<CItem>[8]; CItem typeId +0x10),
//   skills vector +0x1968.
// =====================================================================
#include <windows.h>
#include <cstdint>
#include <cstdio>

static const char* kReportPath = "C:\\Users\\Public\\coclassic_selftest.json";

template <class T>
static bool TryRead(uintptr_t addr, T* out)
{
    __try { *out = *reinterpret_cast<volatile T*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static uint64_t RdU64(uintptr_t a) { uint64_t v = 0; TryRead(a, &v); return v; }
static uint32_t RdU32(uintptr_t a) { uint32_t v = 0; TryRead(a, &v); return v; }

// Copy up to 15 chars of an in-game ASCII name safely.
static void RdName(uintptr_t a, char* out, size_t n)
{
    out[0] = 0;
    for (size_t i = 0; i + 1 < n; ++i) {
        uint8_t c = 0;
        if (!TryRead(a + i, &c) || c == 0 || c < 0x20 || c > 0x7e) { out[i] = 0; return; }
        out[i] = (char)c; out[i + 1] = 0;
    }
}

static void WriteReport(uint64_t base, uint64_t roleMgr, uint64_t hero)
{
    char name[16]; RdName(hero + 0x94, name, sizeof(name));
    uint32_t id     = RdU32(hero + 0x68);
    uint32_t maxhp  = RdU32(hero + 0x3D0);
    uint32_t level  = RdU32(hero + 0x6E8);
    uint32_t silver = RdU32(hero + 0xA80);

    // equipment: 4 shared_ptr<CItem> in the first 8 qwords; even qword = object ptr.
    uint32_t equipType[4] = {0,0,0,0};
    for (int s = 0; s < 4; ++s) {
        uint64_t item = RdU64(hero + 0xBD8 + (uint64_t)s * 0x10);
        if (item) equipType[s] = RdU32(item + 0x10);
    }
    // skills vector<PMagic>: {first,last,end}; count = (last-first)/8
    uint64_t vfirst = RdU64(hero + 0x1968);
    uint64_t vlast  = RdU64(hero + 0x1968 + 8);
    long long skills = (vfirst && vlast && vlast >= vfirst && (vlast - vfirst) < 0x100000)
                       ? (long long)((vlast - vfirst) / 8) : -1;

    FILE* f = nullptr;
    if (fopen_s(&f, kReportPath, "w") != 0 || !f) return;
    fprintf(f,
        "{\n"
        "  \"base\": \"0x%llX\",\n"
        "  \"roleMgr\": \"0x%llX\",\n"
        "  \"hero\": \"0x%llX\",\n"
        "  \"name\": \"%s\",\n"
        "  \"id\": %u,\n"
        "  \"maxHP\": %u,\n"
        "  \"level\": %u,\n"
        "  \"silver\": %u,\n"
        "  \"equip_typeIds\": [%u, %u, %u, %u],\n"
        "  \"skill_count\": %lld\n"
        "}\n",
        (unsigned long long)base, (unsigned long long)roleMgr, (unsigned long long)hero,
        name, id, maxhp, level, silver,
        equipType[0], equipType[1], equipType[2], equipType[3], skills);
    fclose(f);
}

static DWORD WINAPI Run(LPVOID)
{
    const uint64_t base = (uint64_t)GetModuleHandleA(nullptr);

    // Wait for login: role manager pointer resolves and hero has a player id.
    uint64_t roleMgr = 0, hero = 0;
    for (int i = 0; i < 1200; ++i) {           // up to ~10 min
        roleMgr = RdU64(base + 0x69C730);
        if (roleMgr) {
            hero = RdU64(roleMgr);              // m_pHero @ +0x00
            if (hero && RdU32(hero + 0x68) >= 1000000) break;   // IsPlayer
        }
        Sleep(500);
    }
    if (!hero) return 0;

    // Refresh the report periodically so values stay live.
    for (;;) {
        roleMgr = RdU64(base + 0x69C730);
        hero = roleMgr ? RdU64(roleMgr) : 0;
        if (hero) WriteReport(base, roleMgr, hero);
        Sleep(3000);
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
    }
    return TRUE;
}
