#include "monster_scan.h"
#include "game.h"
#include "CHero.h"
#include "CRole.h"
#include "entities.h"

#include <windows.h>
#include <cstdio>
#include <vector>
#include <spdlog/spdlog.h>

namespace
{
    // Session 12: same SEH-guarded-read pattern as map_probe.cpp's TryRead --
    // this walks raw, unverified CRole offsets, so any single field could
    // fault if the object layout assumption is wrong for a given entity.
    template <class T>
    bool TryRead(uintptr_t a, T* out)
    {
        __try { *out = *reinterpret_cast<volatile T*>(a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Dumps [start, end) relative to the CRole base as u32s, into a JSON
    // array under `key`. Covers the two largest unmapped padding gaps in
    // CRole.h -- _pad290 (0x290-0x3D0, right before the already-mapped
    // m_nMaxHp) and _pad3D4 (0x3D4-0x6E0, right after it, before
    // m_nStamina) -- on the theory that a level/tier field would plausibly
    // sit alongside other already-known character-stat fields rather than
    // somewhere unrelated.
    void DumpU32Range(FILE* f, const char* key, uintptr_t base, int start, int end)
    {
        fprintf(f, "\"%s\":[", key);
        bool first = true;
        for (int off = start; off < end; off += 4) {
            uint32_t v = 0;
            TryRead(base + (uintptr_t)off, &v);
            fprintf(f, "%s%u", first ? "" : ",", v);
            first = false;
        }
        fprintf(f, "]");
    }

    std::string StorePath()
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(GetModuleHandleA("coclassic.dll"), path, MAX_PATH);
        std::string p(path);
        const size_t slash = p.find_last_of("\\/");
        p = (slash != std::string::npos) ? p.substr(0, slash) : ".";
        return p + "\\coclassic_monsterscan.json";
    }
}

int DumpNearbyMonsterStats()
{
    CHero* hero = Game::GetHero();
    FILE* f = nullptr;
    if (fopen_s(&f, StorePath().c_str(), "a") != 0 || !f) {
        spdlog::error("[monsterscan] failed to open output file");
        return 0;
    }

    int count = 0;
    const DWORD nowTick = GetTickCount();
    for (CRole* role : Entities::Get()) {
        if (!role || !Entities::IsAlive(role))
            continue;
        if (!role->IsMonster())
            continue;

        const uintptr_t base = reinterpret_cast<uintptr_t>(role);
        const float dist = hero ? hero->m_posMap.DistanceTo(role->m_posMap) : -1.0f;

        fprintf(f, "{\"tick\":%lu,\"id\":%u,\"name\":\"", nowTick, role->GetID());
        // Minimal JSON-string escaping -- monster names are short ASCII in
        // practice, but guard against a literal quote/backslash anyway.
        for (const char* c = role->GetName(); *c; ++c) {
            if (*c == '"' || *c == '\\') fputc('\\', f);
            fputc(*c, f);
        }
        fprintf(f, "\",\"posX\":%d,\"posY\":%d,\"distToHero\":%.1f,\"maxHp\":%d,\"stamina\":%d,\"maxStamina\":%d,",
            role->m_posMap.x, role->m_posMap.y, dist, role->m_nMaxHp, role->m_nStamina, role->m_nMaxStamina);
        DumpU32Range(f, "pad290", base, 0x290, 0x3D0);
        fprintf(f, ",");
        DumpU32Range(f, "pad3D4", base, 0x3D4, 0x6E0);
        fprintf(f, "}\n");
        ++count;
    }

    fclose(f);
    spdlog::info("[monsterscan] wrote {} monster(s) to {}", count, StorePath());
    return count;
}
