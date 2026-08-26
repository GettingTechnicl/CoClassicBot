#include "npc_utils.h"
#include "CRole.h"
#include "game.h"
#include <cstring>

CRole* FindNpcByName(const char* name, const Position& expectedPos, int radius)
{
    // Session 9: CRoleMgr::m_deqRole is not a deque on v1074 — see entities.h.
    CRole* best = nullptr;
    float bestDist = (float)(radius + 1);
    for (CRole* role : Entities::Get()) {
        if (!role || !Entities::IsAlive(role))
            continue;

        if (role->IsPlayer() || role->IsMonster())
            continue;
        if (name && name[0] && _stricmp(role->GetName(), name) != 0)
            continue;

        const float dist = expectedPos.DistanceTo(role->m_posMap);
        if (dist < bestDist) {
            bestDist = dist;
            best = role;
        }
    }

    return best;
}
