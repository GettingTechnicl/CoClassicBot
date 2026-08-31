#pragma once
#include "base.h"
#include <vector>

// =====================================================================
// Map IDs
// =====================================================================
constexpr OBJID MAP_DESERT_CITY         = 1000;
constexpr OBJID MAP_MYSTIC_CASTLE       = 1001;
constexpr OBJID MAP_TWIN_CITY           = 1002;
constexpr OBJID MAP_PHOENIX_CASTLE      = 1011;
constexpr OBJID MAP_BIRD_ISLAND         = 1015;
constexpr OBJID MAP_APE_MOUNTAIN        = 1020;
constexpr OBJID MAP_MINE_CAVE           = 1028;
constexpr OBJID MAP_MARKET              = 1036;
constexpr OBJID MAP_GUILD               = 1038;
constexpr OBJID MAP_GROTTO1             = 1926;
constexpr OBJID MAP_GROTTO2             = 1927;
constexpr OBJID MAP_OCEAN               = 3056;

// =====================================================================
// Movement policy — walk-only locations
// =====================================================================
// Reusable predicate for "the character must WALK here, never jump/teleport."
// Consulted by every mover (the hunt loop's StartPathTo and the shared
// Pathfinder), so returning true anywhere forces walking everywhere with no
// per-caller changes.
//
// NOW: the entire Market map (MAP_MARKET) is walk-only. Market is the ONLY map
// that is walk-only in its entirety — the whole map IS the city.
//
// FUTURE ("Walk only in cities" tickbox): every OTHER city is just a small
// RADIUS around a point inside a larger map (or a sub-map), NOT a whole map. So
// the city case is a proximity test — hero within R tiles of a city center —
// keyed on the map + a per-city center/radius table, gated on the setting. That
// is why this stays a map-only predicate today and the future signature will
// grow to take the hero position and the setting, e.g.
//   ShouldWalkOnly(mapId, heroPos, settings)  // Market: whole map;
//                                              // else: near-a-city radius test
// Every mover already routes through this predicate, so when that lands nothing
// else has to change — only this function's body and signature.
inline bool ShouldWalkOnlyOnMap(OBJID mapId)
{
    return mapId == MAP_MARKET;
}

enum class GatewayType { Portal, Npc, VipTeleport, Item };

struct Gateway {
    GatewayType type;
    OBJID       mapId;                  // source map
    Position    pos;                    // position on source map ({0,0} for instant gateways)
    OBJID       destMapId;              // destination map (same as mapId for intra-map teleport)
    Position    destPos      = {0, 0}; // landing position on dest map ({0,0} = unknown)
    const char* npcName      = nullptr;// NPC entity name (nullptr = match by proximity only)
    int         optionCount  = 0;      // how many dialog answers to send
    int         options[8]   = {};     // NPC dialog option indices
    int         npcTaskId    = 101;    // task/dialog ID for answer packets (field6 in 0x07F0)
    int         instantParam = 0;      // extra action parameter for instant gateways like VIP teleports
    uint32_t    itemTypeId   = 0;      // inventory item type for item gateways
    uint32_t    silverCost   = 0;      // silver required before this gateway can be used

    bool IsIntraMap() const { return destMapId == mapId; }
    bool HasDestPos() const { return destPos.x != 0 || destPos.y != 0; }
    bool IsInstant() const { return type == GatewayType::VipTeleport || type == GatewayType::Item; }
};

// Named destinations: map ID + target coordinates
struct Destination {
    const char* name;
    OBJID       mapId;
    Position    pos;        // target position on the map ({0,0} = map center/default)
};

const char* GetMapName(OBJID mapId);
const std::vector<Gateway>& GetGateways(OBJID mapId);
std::vector<Gateway> FindGatewayPath(
    OBJID from,
    OBJID to,
    Position heroPos = {0, 0},
    uint32_t availableSilver = 0,
    bool allowVipTeleport = false,
    Position finalPos = {0, 0},
    std::vector<uint32_t> availableItemTypes = {});
const Destination* GetDestinations(size_t& count);
