#include "CRole.h"
#include "game.h"

// Session 9 [FIXED]: GameVtableIndex::CRole_SetCommand (59) is confirmed
// wrong for v1074 — a live vtable scan found no match for the real
// SetCommand anywhere in the hero's vtable, and slot 59 itself resolves to
// an unrelated trivial stub. The real function is reached via a plain
// direct call, not virtual dispatch (see game.h GameRva::CROLE_SET_COMMAND_REAL).
void CRole::SetCommand(CCommand* cmd)
{
    if (!cmd)
        return;

    auto fn = GameCall::CRole_SetCommandReal();
    if (!fn)
        return;

    fn(this, cmd);
}

// Session 9: client-side reposition, live-verified (3/10/12-tile runs, target
// tile held for 20s straight, restored cleanly, no crash).
//
// The map<->world transform is a classic isometric 64x32 tile, fitted by
// least squares over 212 real (map,world) pairs sampled while walking:
//     worldX = 32*(mapX - mapY) + Cx
//     worldY = 16*(mapX + mapY) + Cy
// The per-map constants Cx/Cy are deliberately NOT hardcoded -- anchoring the
// delta on the CURRENT position cancels them out, so this stays correct on
// every map with no constant table and no map-change bookkeeping.
void CRole::SyncClientPosition(int mapX, int mapY)
{
    const int wx = m_posWorld.x + 32 * ((mapX - mapY) - (m_posMap.x - m_posMap.y));
    const int wy = m_posWorld.y + 16 * ((mapX + mapY) - (m_posMap.x + m_posMap.y));

    // All three must be written together or the game reverts us within a
    // frame: m_posWorld gets re-snapped from m_posMoveDest, and m_posMap gets
    // recomputed from m_posWorld.
    m_posMoveStart = Position(wx, wy);
    m_posMoveDest  = Position(wx, wy);
    m_posWorld     = Position(wx, wy);

    // Derived from m_posWorld on the next tick anyway; set it now so a caller
    // reading its own position immediately after doesn't see a stale tile.
    m_posMap = Position(mapX, mapY);

    // Session 10 [CRITICAL]: settle the command's target too.
    //
    // IsJumping() is `iType == _COMMAND_JUMP && posTarget != m_posMap`. Moving
    // the hero without updating posTarget therefore leaves a stale command
    // that reads as "still jumping" FOREVER, which quietly breaks two things:
    //   - GetEffectiveHeroPosition() returns posTarget while jumping, so all
    //     targeting and range checks run from a tile the hero is not on;
    //   - the hunt loop gates its attack on !IsJumping(), so the bot stops
    //     attacking entirely.
    //
    // Observed live as the archer standing motionless while surrounded by
    // monsters, and resuming only when the player moved manually — which made
    // the GAME write a fresh command and cleared the flag.
    //
    // Only posTarget is touched. Writing other CCommand fields (or handing a
    // constructed command to SetCommand) is what crashed the client five times
    // in session 9; this is a plain two-int write that makes existing state
    // self-consistent.
    m_cmdAction.posTarget = Position(mapX, mapY);
}
