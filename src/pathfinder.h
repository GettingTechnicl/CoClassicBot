#pragma once
#include "base.h"
#include <functional>
#include <vector>

// Check if any entity occupies the given tile
bool IsTileOccupied(int tileX, int tileY);

class CHero;
class CGameMap;

class Pathfinder {
public:
    // Session 11 [STALE SPEED FIX]: movementIntervalMs used to be a plain
    // DWORD, snapshotted once when the route started and reused for the
    // route's ENTIRE duration — so e.g. a player who was nearby at the
    // moment a long multi-waypoint route began kept the WHOLE route pinned
    // to legit/slow speeds even after they walked out of range, since
    // nothing re-checked "is a player nearby" again until the next route.
    // A provider callback is re-invoked fresh on every throttle check
    // instead, so pacing tracks the CURRENT scan result live. Each caller
    // supplies its own provider since Pathfinder is shared across multiple
    // plugins with independent settings (hunt vs. mining vs. manual travel)
    // — it has no business hardcoding which settings struct to read.
    // jumpDistanceCapProvider (optional): re-invoked fresh on every jump this
    // route issues, same live-reactivity reasoning as movementIntervalProvider
    // above. Returns the max tiles a single jump toward the current waypoint
    // may cover; a shorter cap makes the hero close in on that waypoint over
    // several jumps instead of one, which is what makes jump length vary
    // instead of always hugging the map's true max. Leave unset (default) for
    // callers with no stealth concerns — behavior is then unchanged from
    // before this parameter existed.
    void StartPath(const std::vector<Position>& waypoints, std::function<DWORD()> movementIntervalProvider,
        std::function<int()> jumpDistanceCapProvider = nullptr);
    void Stop();
    void Update();
    bool IsActive() const { return m_active; }
    const std::vector<Position>& GetWaypoints() const { return m_waypoints; }
    size_t GetCurrentIndex() const { return m_index; }
    uint32_t GetGeneration() const { return m_generation; }
    bool GetAvoidMobs() const { return m_avoidMobs; }
    void SetAvoidMobs(bool avoid) { m_avoidMobs = avoid; }
    int GetAvoidMobRadius() const { return m_avoidMobRadius; }
    void SetAvoidMobRadius(int radius) { m_avoidMobRadius = radius; }
    bool GetForceNativeJump() const { return m_forceNativeJump; }
    void SetForceNativeJump(bool force) { m_forceNativeJump = force; }
    DWORD GetLastJumpTick() const { return m_lastJumpTick; }

    // Session 10: the hunt loop treats "pathfinder active" as "movement is
    // happening" — StartPathTo() reports success without doing anything when a
    // path is already running, and the attack is gated on !IsActive(). So a
    // path that stops making progress silently freezes BOTH movement and
    // combat while the UI reports it is busy. Exposed so that state is visible
    // and so callers can watchdog it.
    DWORD GetLastProgressTick() const { return m_lastProgressTick; }
    Position GetCurrentWaypoint() const {
        return (m_index < m_waypoints.size()) ? m_waypoints[m_index] : Position{};
    }

    static Pathfinder& Get();

private:
    bool IssueMovementToWaypoint(CHero* hero, CGameMap* map, const Position& target);
    bool RepathFrom(CHero* hero, CGameMap* map, const Position& finalDest, bool issueImmediate);
    bool CanIssueMovementCommand(DWORD now) const;
    void LoadTilePath(const std::vector<Position>& tilePath, int hx, int hy);

    Position FindSafeAlternative(CHero* hero, CGameMap* map, const Position& target);

    std::vector<Position> m_waypoints;
    size_t m_index = 0;
    bool m_active = false;
    bool m_avoidMobs = false;
    int m_avoidMobRadius = 5;
    bool m_forceNativeJump = false;
    DWORD m_lastJumpTick = 0;
    Position m_lastProgressPos = {};
    Position m_lastIssuedTarget = {};
    Position m_finalDestination = {};
    bool m_lastIssuedMoveWasImmediate = false;
    std::function<DWORD()> m_movementIntervalProvider;
    std::function<int()> m_jumpDistanceCapProvider;
    DWORD m_lastProgressTick = 0;
    uint32_t m_generation = 0;
};
