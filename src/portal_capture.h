#pragma once
#include "base.h"

// =====================================================================
// portal_capture.h — passive session-wide portal/teleport-use logger.
//
// Watches the hero's (mapId, position) every frame (TickBackgroundLogic,
// overlay.cpp) and logs an entry/exit pair the instant either changes by
// more than a single ordinary movement command could ever cover: a real
// map change, OR a same-map jump farther than CGameMap::MAX_JUMP_DIST
// (Bird Island's inter-island "portals" are same-map teleports within one
// mapId, not map changes — a map-id-only check would miss them entirely).
//
// Exists because manually eyeballing coordinates around a portal use is
// close to impossible in the moment the screen/map changes — this needs
// zero extra action beyond just walking through portals normally during a
// play session; the log then has an exact entry/exit pair for every
// transition, and the overlay shows the most recent one live with a
// one-click clipboard copy.
// =====================================================================

// Call once per frame whenever a hero exists (see overlay.cpp's
// TickBackgroundLogic). Cheap: one position compare.
void TrackPortalUsage();

struct PortalUsageRecord
{
    bool valid = false;
    OBJID fromMapId = 0;
    Position fromPos = {};
    OBJID toMapId = 0;
    Position toPos = {};
    DWORD tick = 0;
    // Which of the source map's file-declared portals the hero was standing
    // on when the transition fired (mapdata.h MapPortal::id), or -1 if none
    // was within range — e.g. a return scroll or an NPC teleport. Lets a
    // capture walk report "portal #12 -> (x,y)" by number, so the result maps
    // straight onto the .DMap's own portal list instead of being reconciled
    // by eye afterwards.
    int viaPortalId = -1;
    Position viaPortalPos = {};
};

// The most recently detected transition this session, for the debug
// overlay's readout. {valid=false} if none has fired yet.
const PortalUsageRecord& GetLastPortalUsage();
