#include "CStatTable.h"
#include "game.h"

int CStatTable::GetValue(int statType) const
{
    if (!GameRva::VERIFIED_V1074) {
        static bool warned = false;
        if (!warned) { spdlog::error("[safety] CStatTable::GetValue: RVA unverified on v1074, returning 0 (see game.h GameRva::VERIFIED_V1074)"); warned = true; }
        return 0;
    }
    return GameCall::CStatTable_GetValue()(this, statType);
}
