#include "CEntityInfo.h"
#include "game.h"

namespace {
struct WarehouseItemSlot
{
    CItem* pItem = nullptr;
    void* pSharedControl = nullptr;
};

// Session 10 [SAFETY]: CEntityInfo::GetInstance() resolves an admittedly
// UNVERIFIED RVA (game.h Offsets::ENTITY_INFO — see its comment, same family
// as the old GAME_MAP RVA that WAS confirmed garbage and fixed). Every field
// read below is effectively `*(this + offset)`, which faults just like any
// other bad pointer if `this` isn't really a CEntityInfo. Reachable in
// normal play every frame (overlay while in a guild) and during Mining's
// real warehouse deposit/withdraw flow, so this isn't hypothetical.
//
// Raw-output version with no C++ objects, since a __try block can't share a
// function with a local object requiring unwinding (std::vector<CItem*>
// below has one) — matches this project's established TryRead<T> idiom.
constexpr size_t kMaxWarehouseItems = 512; // a real ring will never be anywhere
                                            // near this; hitting the cap means
                                            // the data is garbage, not real
                                            // content, so treat it the same as
                                            // a hard read failure.

bool TryReadWarehouseRing(const CEntityInfo* info, CItem** outItems, size_t* outCount)
{
    *outCount = 0;
    __try {
        void* ringRaw = info->m_pWarehouseRing;
        const uint64_t mask = info->m_qwWarehouseRingMask;
        const uint64_t start = info->m_qwWarehouseRingStart;
        const uint64_t count = info->m_qwWarehouseRingCount;
        if (!ringRaw || count == 0 || mask == 0 || count > kMaxWarehouseItems)
            return true; // not garbage, just empty (or capped — see above)

        auto** ring = reinterpret_cast<WarehouseItemSlot**>(ringRaw);
        const uint64_t end = start + count;
        size_t written = 0;
        for (uint64_t index = start; index != end; ++index) {
            WarehouseItemSlot* slot = ring[(mask - 1) & index];
            if (slot && slot->pItem)
                outItems[written++] = slot->pItem;
        }
        *outCount = written;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outCount = 0;
        return false;
    }
}
}

CEntityInfo* CEntityInfo::GetInstance()
{
    return Game::GetEntityInfo();
}

bool CEntityInfo::HasPendingTradeRequest() const
{
    __try {
        return m_nTradeRequestState == TRADE_REQUEST_PENDING
            && m_idTradeRequester != 0
            && !m_szTradeRequesterName.empty();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t CEntityInfo::GetTradeRequestState() const
{
    __try {
        return m_nTradeRequestState;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

OBJID CEntityInfo::GetTradeRequesterId() const
{
    __try {
        return m_idTradeRequester;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

const char* CEntityInfo::GetTradeRequesterName() const
{
    __try {
        return m_szTradeRequesterName.c_str();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "";
    }
}

std::vector<CItem*> CEntityInfo::GetWarehouseItems() const
{
    std::vector<CItem*> items;
    CItem* buffer[kMaxWarehouseItems];
    size_t count = 0;
    if (!TryReadWarehouseRing(this, buffer, &count))
        return items;
    items.assign(buffer, buffer + count);
    return items;
}

CItem* CEntityInfo::FindWarehouseItemById(OBJID idItem) const
{
    if (idItem == 0)
        return nullptr;

    for (CItem* item : GetWarehouseItems()) {
        if (item && item->GetID() == idItem)
            return item;
    }

    return nullptr;
}
