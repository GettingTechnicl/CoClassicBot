#pragma once
// =====================================================================
// map_probe.h — one-shot structural probe for the active CGameMap (v1074).
//
// Session 9 proved Game::GetMap() is broken: Offsets::GAME_MAP (0x4E02E0)
// is used as a static object but dereferences to unreadable garbage. Before
// it can be fixed, BOTH halves must be pinned down together:
//   1. the correct access path to the active CGameMap, and
//   2. that object's real field offsets (m_idMap / m_sizeMap / m_pCellInfo).
// Fixing only (1) is actively dangerous: consumers guard on
// `m_sizeMap.iWidth > 0`, so stale offsets could make that guard PASS on
// garbage and turn today's safe no-op into a garbage-pointer dereference
// inside pathfinding.
//
// Runs in-process (coclassic.dll) rather than via Frida — Frida's injection
// wedged repeatedly late in session 9 while this DLL loaded reliably every
// time. Writes findings to C:\Users\Public\coclassic_mapprobe.json.
// =====================================================================

// Dumps candidate map pointers, every heap object carrying the CGameMap
// vtable, and the scene object's pointer fields. Safe: reads only.
// Returns true if the report file was written.
bool DebugProbeGameMap(int expectedMapId);

// Second-stage probe. The vtable-0x5CCB60 objects turned out to be map
// DESCRIPTORS (id/width/height + inline name strings, no cell data), so the
// live map has to be found another way: search memory for the current map's
// dimensions appearing as an adjacent (width,height) u32 pair, and report any
// nearby pointer that targets a large allocation — Twin City's 543x772 grid
// is ~419k cells, so its cell array must be a multi-MB block that stands out.
// Writes C:\Users\Public\coclassic_mapdata.json. Read-only.
bool DebugProbeMapData(int width, int height);

// Third stage: verify a candidate cell grid by CONTENT rather than by size.
// Dumps the neighbourhood around the hero's tile under both row-major and
// column-major indexing. The hero is by definition standing on a walkable
// cell, so if the candidate is real, the hero's cell and the surrounding
// walls must differ in a consistent way. Writes
// C:\Users\Public\coclassic_cells.json. Read-only.
bool DebugProbeCells(unsigned long long gridBase, int stride, int width, int height, int radius);

// Auto-detect the cell grid. Region SIZE alone is too weak a signal (the
// sizes reported by VirtualQuery are reservations, not exact allocations), so
// this scores candidates by CONTENT instead: for every large region, every
// plausible stride, both indexings, and every byte column within the stride,
// it samples a window around the hero and asks whether that column looks like
// map data — a small set of distinct values (flag-like) arranged with spatial
// structure (neighbouring tiles usually match), rather than noise or a
// constant. Writes C:\Users\Public\coclassic_gridscan.json. Read-only.
bool DebugAutoFindGrid(int width, int height);

// Record the hero's current tile into the walk trace. Cheap; call once a
// frame. The trace is the key to identifying the grid: every tile the hero
// has physically stood on MUST be walkable, so the real grid has to store one
// consistent "walkable" value across all of them. Scattered random memory
// cannot satisfy that constraint across dozens of unrelated tiles, which is
// what makes this decisive where size- and structure-based scoring was not.
void MapProbe_RecordHeroTile();

// How many distinct tiles have been recorded so far (for the UI).
int MapProbe_TraceCount();
void MapProbe_ClearTrace();

// Search all committed private memory for a byte pattern loaded from
// C:\Users\Public\coclassic_pattern.bin, reporting every match to
// C:\Users\Public\coclassic_patternhits.json.
//
// This is ground-truth matching rather than heuristics: the pattern is cut
// straight out of the map's own .DMap file on disk, so a hit is proof that
// the client stores that map data verbatim at that address. Read-only.
bool DebugSearchPattern();

// Find the global that holds the CURRENT map id.
//
// With terrain loaded from the .DMap files on disk, this single u32 is the
// only thing still needed from game memory. Scans the image's data sections
// plus the hero and role-manager objects for the given id and records every
// location. Run it once per map and intersect the results: the location that
// tracks the id across map changes is the current-map global.
// Appends to C:\Users\Public\coclassic_mapid.json. Read-only.
bool DebugScanForMapId(int currentMapId);

// Raw diagnostics for the two current-map pointer candidates, so a failure
// shows WHICH step broke (null pointer / unreadable target / unexpected
// value) instead of collapsing to a single "unavailable".
struct MapIdDiag
{
    unsigned long long ptr1, ptr2;   // raw pointer values read from the globals
    unsigned int       val1, val2;   // u32 at target+0x10
    bool ptr1Readable, ptr2Readable;
};
MapIdDiag MapProbe_MapIdDiag();

// Dump the walk trace to C:\Users\Public\coclassic_walktrace.json.
//
// This identifies the current map EMPIRICALLY rather than by name. Every tile
// the hero has stood on must be walkable in the real map, so matching the
// trace against the walkability of all ~156 .DMap files on disk narrows the
// candidates to the map we are actually standing in — no reliance on map-name
// tables, which have already proven to disagree with each other.
bool DebugDumpWalkTrace();

// Snapshot the scene object (and one level of its pointer targets) to
// C:\Users\Public\coclassic_scene.json, tagged with a caller-supplied label.
//
// Deliberately does NOT take a map id. Searching for "where does 1002 appear"
// requires knowing the id up front, and our map-name tables disagree with each
// other badly enough that two of four earlier scans were mislabelled — which
// silently poisoned the intersection. Diffing raw snapshots instead finds
// fields that CHANGE when the map changes, with no id needed as input. The
// map id can then be read off the winning field in a map we are sure of.
bool DebugSnapshotScene(int label);

// List every file-backed mapping in the process, via GetMappedFileNameW.
//
// Completely different angle: instead of reverse-engineering where the client
// keeps the map id, ask the OS which files it currently has mapped. If the
// active .DMap is among them, that names the current map directly — no game
// structures involved, and it cannot be broken by a client update the way an
// offset can. Read-only. Writes C:\Users\Public\coclassic_mappedfiles.json.
bool DebugListMappedFiles();

// Locate the real inventory container on v1074.
//
// CHero::m_deqItem is declared at +0xB70 as a std::deque<PItem>, but that
// offset is marked "[inferred from equip offset]" and was never verified — it
// reads _Mysize == 0 with items in the bag. 51 call sites depend on it, and
// the consequences are not cosmetic: the repair sequence only re-equips after
// FINDING the repaired item in the inventory, so a blind inventory means the
// bot unequips gear and never puts it back, after which ShootTarget() silently
// no-ops because the right hand is empty.
//
// Scans the hero object for pointers to CItem-signature objects, and for
// pointers to ARRAYS of such pointers (deque blocks / vector storage), so the
// container is found whether items are stored inline or indirectly.
// Writes C:\Users\Public\coclassic_inventory.json. Read-only.
bool DebugFindInventory();
