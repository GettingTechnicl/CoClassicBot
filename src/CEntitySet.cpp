#include "CEntitySet.h"
#include "game.h"

namespace {
// Session 10 [SAFETY]: CEntitySet::GetInstance() resolves an admittedly
// UNVERIFIED RVA (game.h Offsets::ENTITY_SET — see its comment). Even though
// GetEntitySet() itself only casts an address (no read), the CALLER
// iterating m_vec is where a bad `this` actually bites: std::vector's own
// begin/end/capacity live on the object, so if `this` isn't really a
// CEntitySet, this reads them as arbitrary bytes and can walk/dereference
// far out of bounds. SEH-guarded per this project's established idiom
// (entities.cpp/map_items.cpp TryRead<T>) — extracted into its own function
// since a __try block can't share a function with local C++ objects
// requiring unwinding, and the caller (FindSyndicate) has none here so this
// guard can live directly at the call site instead.
bool TryFindSyndicate(const CEntitySet* set, OBJID syndicateId, const CSyndicateEntry** out)
{
    __try {
        for (auto& sp : set->m_vec) {
            if (sp && sp->m_id == static_cast<int>(syndicateId)) {
                *out = sp.get();
                return true;
            }
        }
        return true; // walked cleanly, just no match
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}

CEntitySet* CEntitySet::GetInstance()
{
    return Game::GetEntitySet();
}

const CSyndicateEntry* CEntitySet::FindSyndicate(OBJID syndicateId) const
{
    const CSyndicateEntry* result = nullptr;
    if (!TryFindSyndicate(this, syndicateId, &result))
        return nullptr;
    return result;
}

const char* CEntitySet::GetSyndicateName(OBJID syndicateId) const
{
    auto* entry = FindSyndicate(syndicateId);
    return entry ? entry->m_szName.c_str() : nullptr;
}

const char* GetSyndicateRankName(int rank)
{
    switch (rank) {
        case SyndicateRank::LEADER:     return "Leader";
        case SyndicateRank::DEPUTY:     return "Deputy";
        case SyndicateRank::MANAGER:    return "Manager";
        case SyndicateRank::AIDE:       return "Aide";
        case SyndicateRank::SUPERVISOR: return "Supervisor";
        case SyndicateRank::MEMBER:     return "Member";
        default:                        return "";
    }
}
