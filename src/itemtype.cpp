#include "itemtype.h"
#include "CItem.h"
#include "log.h"
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <fstream>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>

using json = nlohmann::json;

static std::unordered_map<uint32_t, ItemTypeInfo> g_itemTypes;
static std::vector<const ItemTypeInfo*> g_allItemTypes;

// Resolve ini/itemtype.json relative to the game install instead of hardcoding a
// drive. This DLL is injected into <gamedir>\bin\64\ImConquer.exe, so the game
// directory is three levels up from the host executable. Falls back to the legacy
// default install path if the derived location is missing.
static std::string ResolveItemTypePath()
{
    namespace fs = std::filesystem;
    char buf[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, buf, MAX_PATH) > 0) {
        fs::path gameDir = fs::path(buf).parent_path().parent_path().parent_path();
        fs::path candidate = gameDir / "ini" / "itemtype.json";
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate.string();
    }
    return R"(C:\Program Files\Classic Conquer 2.0\ini\itemtype.json)";
}

void LoadItemTypes()
{
    const std::string path = ResolveItemTypePath();
    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::error("[itemtype] Failed to open {}", path);
        return;
    }

    auto arr = json::parse(f, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) {
        spdlog::error("[itemtype] Failed to parse JSON");
        return;
    }

    for (auto& obj : arr) {
        ItemTypeInfo item;
        item.id = obj.value("id", 0u);
        item.name = obj.value("name", "");
        item.requiredLevel = obj.value("requiredLevel", 0u);
        item.price = obj.value("price", 0u);
        item.attackMin = obj.value("attackMin", 0u);
        item.attackMax = obj.value("attackMax", 0u);
        item.defense = obj.value("defense", 0u);
        item.life = obj.value("life", 0u);
        item.mana = obj.value("mana", 0u);
        item.amount = obj.value("amount", 0u);
        item.amountLimit = obj.value("amountLimit", 0u);
        item.attackRange = obj.value("attackRange", 0u);
        if (item.id && !item.name.empty())
            g_itemTypes[item.id] = std::move(item);
    }

    g_allItemTypes.clear();
    g_allItemTypes.reserve(g_itemTypes.size());
    for (auto& [_, item] : g_itemTypes)
        g_allItemTypes.push_back(&item);
    std::sort(g_allItemTypes.begin(), g_allItemTypes.end(),
        [](const ItemTypeInfo* a, const ItemTypeInfo* b) {
            return _stricmp(a->name.c_str(), b->name.c_str()) < 0;
        });

    spdlog::info("[itemtype] Loaded {} item types", g_itemTypes.size());
}

const char* GetItemTypeName(uint32_t typeId)
{
    auto* info = GetItemTypeInfo(typeId);
    if (info)
        return info->name.c_str();
    return "";
}

const ItemTypeInfo* GetItemTypeInfo(uint32_t typeId)
{
    auto it = g_itemTypes.find(typeId);
    return it != g_itemTypes.end() ? &it->second : nullptr;
}

const std::vector<const ItemTypeInfo*>& GetAllItemTypes()
{
    return g_allItemTypes;
}

static bool IsEquipmentQualitySort(int itemSort)
{
    switch (itemSort) {
        case ItemSort::HELMET:
        case ItemSort::NECKLACE:
        case ItemSort::ARMOR:
        case ItemSort::WEAPON_SINGLE_HAND:
        case ItemSort::WEAPON_DOUBLE_HAND:
        case ItemSort::SHIELD:
        case ItemSort::RING:
        case ItemSort::SHOES:
        case ItemSort::SPRITE:
        case ItemSort::KATANA:
            return true;
        default:
            return false;
    }
}

static bool IsGemTypeId(uint32_t typeId)
{
    if (GetItemSort(typeId) != ItemSort::OTHER)
        return false;

    const int suffix = typeId % 10;
    return typeId >= 700001 && typeId < 701000 && suffix >= 1 && suffix <= 3;
}

static const char* GetQualityPrefix(uint32_t typeId)
{
    const int suffix = typeId % 10;
    if (IsEquipmentQualitySort(GetItemSort(typeId))) {
        switch (suffix) {
            case ItemQuality::REFINED: return "Refined";
            case ItemQuality::UNIQUE:  return "Unique";
            case ItemQuality::ELITE:   return "Elite";
            case ItemQuality::SUPER:   return "Super";
            default:                   return "";
        }
    }

    if (IsGemTypeId(typeId)) {
        switch (suffix) {
            case 2: return "Refined";
            case 3: return "Super";
            default: return "";
        }
    }

    return "";
}

std::string FormatItemName(uint32_t typeId, uint8_t plus)
{
    const char* baseName = GetItemTypeName(typeId);
    const char* prefix = GetQualityPrefix(typeId);

    std::string result;
    if (baseName[0] && prefix[0])
        result = std::string(prefix) + baseName;
    else if (baseName[0])
        result = baseName;
    else
        result = "Item:" + std::to_string(typeId);

    if (plus > 0)
        result += "(+" + std::to_string(plus) + ")";

    return result;
}
