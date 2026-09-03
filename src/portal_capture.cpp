#include "portal_capture.h"
#include "game.h"
#include "CHero.h"
#include "CGameMap.h"
#include "mapdata.h"
#include "log.h"

namespace {

bool s_hasLast = false;
OBJID s_lastMapId = 0;
Position s_lastPos = {};
PortalUsageRecord s_lastPortalUsage;

// The file-declared portal the hero was nearest to on the PREVIOUS tick, on
// the previous tick's map. Sampled every frame so that when a transition is
// detected — at which point Game::GetCurrentMapId() already reports the NEW
// map and the current grid has been swapped — the source portal is still
// known without any lookup into the map just left.
int      s_nearPortalId  = -1;
Position s_nearPortalPos = {};

// Comfortably above CGameMap::MAX_JUMP_DIST (18) — the farthest distance any
// single legitimate movement command (walk, jump, speedhack chain) can ever
// cover in one tick — so this only fires for real teleports (portals,
// return scrolls, VIP teleport), never ordinary movement.
constexpr int kSameMapPortalThreshold = 30;

// A portal triggers when the hero steps onto/next to its declared tile, so
// the last tick before a transition has the hero within a couple of tiles of
// it. Small on purpose: two Bird Island portals can be ~10 tiles apart, and
// naming the wrong one would be worse than naming none.
constexpr int kNearPortalTiles = 3;

void SampleNearestPortal(OBJID mapId, const Position& pos)
{
    s_nearPortalId = -1;
    const MapGrid* grid = GetCurrentMapGrid();
    if (!grid || !grid->IsLoaded() || (OBJID)grid->GetMapId() != mapId)
        return;
    int best = kNearPortalTiles + 1;
    for (const MapPortal& p : grid->GetPortals()) {
        const int d = CGameMap::TileDist(pos.x, pos.y, p.x, p.y);
        if (d < best) {
            best = d;
            s_nearPortalId  = p.id;
            s_nearPortalPos = {p.x, p.y};
        }
    }
}

} // namespace

void TrackPortalUsage()
{
    CHero* hero = Game::GetHero();
    if (!hero)
        return;

    const OBJID mapId = Game::GetCurrentMapId();
    const Position pos = hero->m_posMap;

    if (!s_hasLast) {
        s_lastMapId = mapId;
        s_lastPos = pos;
        s_hasLast = true;
        SampleNearestPortal(mapId, pos);
        return;
    }

    const bool mapChanged = (mapId != s_lastMapId);
    const bool bigSameMapJump = !mapChanged
        && CGameMap::TileDist(s_lastPos.x, s_lastPos.y, pos.x, pos.y) > kSameMapPortalThreshold;

    if (mapChanged || bigSameMapJump) {
        if (s_nearPortalId >= 0) {
            spdlog::info("[portal] {} ({},{}) -> {} ({},{}) via portal #{} @({},{}){}",
                s_lastMapId, s_lastPos.x, s_lastPos.y,
                mapId, pos.x, pos.y,
                s_nearPortalId, s_nearPortalPos.x, s_nearPortalPos.y,
                mapChanged ? "" : " (same-map jump)");
        } else {
            spdlog::info("[portal] {} ({},{}) -> {} ({},{}){} (no file portal within {} tiles)",
                s_lastMapId, s_lastPos.x, s_lastPos.y,
                mapId, pos.x, pos.y,
                mapChanged ? "" : " (same-map jump)", kNearPortalTiles);
        }

        s_lastPortalUsage.valid = true;
        s_lastPortalUsage.fromMapId = s_lastMapId;
        s_lastPortalUsage.fromPos = s_lastPos;
        s_lastPortalUsage.toMapId = mapId;
        s_lastPortalUsage.toPos = pos;
        s_lastPortalUsage.tick = GetTickCount();
        s_lastPortalUsage.viaPortalId  = s_nearPortalId;
        s_lastPortalUsage.viaPortalPos = s_nearPortalPos;
    }

    s_lastMapId = mapId;
    s_lastPos = pos;
    SampleNearestPortal(mapId, pos);
}

const PortalUsageRecord& GetLastPortalUsage()
{
    return s_lastPortalUsage;
}
