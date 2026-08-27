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

    // Session 12 round 2: a first pass dumping just the two biggest gaps
    // (_pad290/_pad3D4, bracketing the already-mapped m_nMaxHp/m_nStamina)
    // found nothing correlating with the user's confirmed green/white/red/
    // black tiers -- not even loosely, at any stability threshold. Widening
    // to dump the ENTIRE mapped CRole range (+0x20 through the end of
    // m_nSyndicateRank at +0x71C) in one pass, covering every remaining
    // unmapped gap (_pad20/_pad38/_pad6C/_padA4/_padF0/_pad118/_pad6E8) so
    // a single re-run covers everything instead of needing another full
    // play-session round trip if the answer isn't in the first two gaps.
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
        DumpU32Range(f, "full", base, 0x20, 0x71C);
        fprintf(f, "}\n");
        ++count;
    }

    fclose(f);
    spdlog::info("[monsterscan] wrote {} monster(s) to {}", count, StorePath());
    return count;
}
