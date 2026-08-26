#pragma once
#include <string>
#include <cstdint>
#include <vector>

struct ItemTypeInfo
{
    uint32_t id = 0;
    std::string name;
    uint32_t requiredLevel = 0;
    uint32_t price = 0;
    uint32_t attackMin = 0;
    uint32_t attackMax = 0;
    uint32_t defense = 0;
    uint32_t life = 0;
    uint32_t mana = 0;
    uint32_t amount = 0;
    uint32_t amountLimit = 0;
    // Session 10: the client's own item table carries the weapon's reach
    // (e.g. TightBow = 21). Previously unparsed, which forced the archer's
    // attack range to be a hand-tuned number — and an under-set one silently
    // shrinks the hunt leash and engage margin too, since both derive from it.
    uint32_t attackRange = 0;
};

// Load itemtype.json from the game's ini directory.
// Must be called once (e.g. from InitThread). Thread-safe after init.
void LoadItemTypes();

// Look up an item's base name by type ID. Returns "" if not found.
const char* GetItemTypeName(uint32_t typeId);
const ItemTypeInfo* GetItemTypeInfo(uint32_t typeId);
const std::vector<const ItemTypeInfo*>& GetAllItemTypes();

// Format a display name with quality prefix and plus, e.g. "SuperConquestWand(+7)"
// quality from typeId % 10, plus from CMapItem::GetPlus()
std::string FormatItemName(uint32_t typeId, uint8_t plus);
