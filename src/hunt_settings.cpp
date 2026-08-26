#include <cmath>
#include <limits>
#include "hunt_settings.h"
#include "game.h"
#include "CGameMap.h"
#include "CItem.h"
#include "itemtype.h"

const char* HuntSkillName(HuntSkillType type)
{
    switch (type) {
        case HuntSkillType::Superman: return "Superman";
        case HuntSkillType::Cyclone:  return "Cyclone";
        case HuntSkillType::Accuracy: return "Accuracy";
        case HuntSkillType::XpFly:    return "XP Fly";
        case HuntSkillType::Fly:      return "Stamina Fly";
        case HuntSkillType::Stigma:   return "Stigma";
        default:                      return "Unknown";
    }
}

void AutoHuntSettings::SyncSkillBoolsFromPriorities()
{
    castSuperman = false;
    castCyclone = false;
    castXpFly = false;
    castFly = false;
    useStigma = false;
    for (int i = 0; i < kHuntSkillCount; i++) {
        const auto& entry = skillPriorities[i];
        if (!entry.enabled) continue;
        switch (entry.type) {
            case HuntSkillType::Superman: castSuperman = true; break;
            case HuntSkillType::Cyclone:  castCyclone = true;  break;
            case HuntSkillType::XpFly:    castXpFly = true;    break;
            case HuntSkillType::Fly:      castFly = true;      break;
            case HuntSkillType::Stigma:   useStigma = true;    break;
            default: break;
        }
    }
}

static AutoHuntSettings g_autoHuntSettings;
AutoHuntSettings& GetAutoHuntSettings() { return g_autoHuntSettings; }

bool IsZeroPos(const Position& pos)
{
    return pos.x == 0 && pos.y == 0;
}

Position PolygonCentroid(const std::vector<Position>& points)
{
    if (points.empty())
        return {};

    long long sumX = 0;
    long long sumY = 0;
    for (const Position& point : points) {
        sumX += point.x;
        sumY += point.y;
    }

    return {
        (int)(sumX / (long long)points.size()),
        (int)(sumY / (long long)points.size())
    };
}

bool PointInPolygon(const Position& point, const std::vector<Position>& polygon)
{
    if (polygon.size() < 3)
        return false;

    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Position& a = polygon[i];
        const Position& b = polygon[j];
        const bool intersects =
            ((a.y > point.y) != (b.y > point.y)) &&
            (point.x < (b.x - a.x) * (point.y - a.y) / (double)(b.y - a.y) + a.x);
        if (intersects)
            inside = !inside;
    }

    return inside;
}

bool HasValidHuntZone(const AutoHuntSettings& settings)
{
    if (settings.zoneMapId == 0)
        return false;

    if (settings.zoneMode == AutoHuntZoneMode::Route)
        return GetActiveRoute(settings, settings.zoneMapId) != nullptr;

    if (settings.zoneMode == AutoHuntZoneMode::Circle)
        return !IsZeroPos(settings.zoneCenter) && settings.zoneRadius > 0;

    return settings.zonePolygon.size() >= 3;
}

bool IsPointInHuntZone(const AutoHuntSettings& settings, OBJID mapId, const Position& pos)
{
    if (mapId != settings.zoneMapId)
        return false;

    if (settings.zoneMode == AutoHuntZoneMode::Route) {
        // "Inside the zone" for a route means inside the corridor — the band
        // the bot is allowed to occupy, not the zero-width line itself.
        const HuntRoute* r = GetActiveRoute(settings, mapId);
        if (!r) return false;
        return DistanceToRoute(*r, pos) <= (float)GetRouteCorridor(settings);
    }

    if (settings.zoneMode == AutoHuntZoneMode::Circle) {
        if (IsZeroPos(settings.zoneCenter) || settings.zoneRadius <= 0)
            return false;
        return settings.zoneCenter.DistanceTo(pos) <= (float)settings.zoneRadius;
    }

    return PointInPolygon(pos, settings.zonePolygon);
}

const HuntRoute* GetActiveRoute(const AutoHuntSettings& settings, OBJID mapId)
{
    for (const HuntRoute& r : settings.routes) {
        if (r.mapId == mapId && !r.waypoints.empty()
            && (settings.activeRouteName.empty() || r.name == settings.activeRouteName))
            return &r;
    }
    return nullptr;
}

float DistanceToRoute(const HuntRoute& route, const Position& pos)
{
    if (route.waypoints.empty())
        return (std::numeric_limits<float>::max)();
    if (route.waypoints.size() == 1)
        return route.waypoints[0].DistanceTo(pos);

    float best = (std::numeric_limits<float>::max)();
    for (size_t i = 0; i + 1 < route.waypoints.size(); ++i) {
        const Position& a = route.waypoints[i];
        const Position& b = route.waypoints[i + 1];
        const float ax = (float)a.x, ay = (float)a.y;
        const float bx = (float)b.x, by = (float)b.y;
        const float px = (float)pos.x, py = (float)pos.y;
        const float vx = bx - ax, vy = by - ay;
        const float len2 = vx * vx + vy * vy;

        // Project onto the segment and clamp — measuring to the segment rather
        // than to the endpoints matters on long legs, where the midpoint is
        // "on the path" but far from either waypoint.
        float t = 0.0f;
        if (len2 > 0.0f)
            t = ((px - ax) * vx + (py - ay) * vy) / len2;
        t = (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);

        const float cx = ax + t * vx, cy = ay + t * vy;
        const float dx = px - cx, dy = py - cy;
        const float d = sqrtf(dx * dx + dy * dy);
        if (d < best)
            best = d;
    }
    return best;
}

int NearestWaypointIndex(const HuntRoute& route, const Position& pos)
{
    int best = 0;
    float bestDist = (std::numeric_limits<float>::max)();
    for (size_t i = 0; i < route.waypoints.size(); ++i) {
        const float d = route.waypoints[i].DistanceTo(pos);
        if (d < bestDist) { bestDist = d; best = (int)i; }
    }
    return best;
}

int GetEffectiveAttackRange(const AutoHuntSettings& settings)
{
    if (settings.rangedAttackRange > 0)
        return settings.rangedAttackRange;

    // Session 10: fall back to the equipped weapon's real reach from the
    // client's own item table rather than a hand-tuned number. Kept here (not
    // just in the archer plugin) because the LEASH derives from it, and the
    // leash governs every zone mode for every class.
    if (CHero* hero = Game::GetHero()) {
        if (CItem* weapon = hero->GetEquip(EquipSlot::RWEAPON)) {
            if (const ItemTypeInfo* info = GetItemTypeInfo(weapon->GetTypeID())) {
                if (info->attackRange > 0)
                    return (std::min)((int)info->attackRange, (int)CGameMap::MAX_JUMP_DIST);
            }
        }
    }
    return 0;
}

int GetHuntLeash(const AutoHuntSettings& settings)
{
    if (settings.routeCorridorOverride > 0)
        return settings.routeCorridorOverride;

    // Leash = the class's attack range. An archer with long reach naturally
    // works a wider band than a melee, with nothing to tune by hand.
    const int melee = 2;
    const int r = (settings.combatMode == AutoHuntCombatMode::Archer || settings.archerMode)
                ? GetEffectiveAttackRange(settings) : melee;
    return (std::max)(2, r);
}

int GetHuntEngageMargin(const AutoHuntSettings& settings)
{
    return GetHuntLeash(settings) * 2;
}

// Route corridor is just the leash applied to a polyline.
int GetRouteCorridor(const AutoHuntSettings& settings)
{
    return GetHuntLeash(settings);
}

bool IsPointNearHuntZone(const AutoHuntSettings& settings, OBJID mapId, const Position& pos, int margin)
{
    if (mapId != settings.zoneMapId)
        return false;
    if (margin <= 0)
        return IsPointInHuntZone(settings, mapId, pos);

    if (settings.zoneMode == AutoHuntZoneMode::Route) {
        const HuntRoute* r = GetActiveRoute(settings, mapId);
        if (!r) return false;
        return DistanceToRoute(*r, pos) <= (float)(GetRouteCorridor(settings) + margin);
    }

    if (settings.zoneMode == AutoHuntZoneMode::Circle) {
        if (IsZeroPos(settings.zoneCenter) || settings.zoneRadius <= 0)
            return false;
        return settings.zoneCenter.DistanceTo(pos) <= (float)(settings.zoneRadius + margin);
    }

    // Polygon: inside, or within `margin` of the centroid-expanded bounds.
    // Cheap and good enough — the margin only needs to admit tiles just
    // outside the border, not model exact polygon offsetting.
    if (PointInPolygon(pos, settings.zonePolygon))
        return true;
    for (const Position& v : settings.zonePolygon) {
        if (v.DistanceTo(pos) <= (float)margin)
            return true;
    }
    return false;
}

Position GetHuntZoneAnchor(const AutoHuntSettings& settings)
{
    if (settings.zoneMode == AutoHuntZoneMode::Route) {
        // Anchor on the waypoint nearest the hero rather than the route's
        // centroid: callers use the anchor as "where the hunt is", and for a
        // long route the centroid can sit nowhere near the actual patrol.
        if (const HuntRoute* r = GetActiveRoute(settings, settings.zoneMapId)) {
            if (CHero* hero = Game::GetHero())
                return r->waypoints[NearestWaypointIndex(*r, hero->m_posMap)];
            return r->waypoints.front();
        }
        return {};
    }
    if (settings.zoneMode == AutoHuntZoneMode::Polygon)
        return PolygonCentroid(settings.zonePolygon);
    return settings.zoneCenter;
}

// ── Route recording ──────────────────────────────────────────────────────
namespace {
    bool                  g_recording = false;
    std::vector<Position> g_recordBuffer;
}

void RouteRecordStart()
{
    g_recording = true;
    g_recordBuffer.clear();
}

void RouteRecordSample(const Position& pos)
{
    if (!g_recording)
        return;
    // Only record real movement; standing still must not inflate the route.
    if (!g_recordBuffer.empty()) {
        const Position& last = g_recordBuffer.back();
        if (last.x == pos.x && last.y == pos.y)
            return;
    }
    if (g_recordBuffer.size() < 8192)
        g_recordBuffer.push_back(pos);
}

bool RouteRecordIsActive()    { return g_recording; }
int  RouteRecordSampleCount() { return (int)g_recordBuffer.size(); }

bool RouteRecordStop(AutoHuntSettings& settings, OBJID mapId, const char* name)
{
    g_recording = false;
    if (g_recordBuffer.size() < 2 || !name || !name[0])
        return false;

    HuntRoute route;
    route.name  = name;
    route.mapId = mapId;

    // Collapse the raw tile trail to corner waypoints. Recording samples every
    // tile walked, which is far more detail than patrol needs and would make
    // the bot re-path constantly; SimplifyPath keeps only direction changes.
    if (CGameMap* map = Game::GetMap())
        route.waypoints = map->SimplifyPath(g_recordBuffer);
    if (route.waypoints.size() < 2)
        route.waypoints = g_recordBuffer;

    // Replace an existing route of the same name on the same map.
    for (HuntRoute& r : settings.routes) {
        if (r.mapId == mapId && r.name == route.name) {
            r = route;
            settings.activeRouteName = route.name;
            g_recordBuffer.clear();
            return true;
        }
    }
    settings.routes.push_back(route);
    settings.activeRouteName = route.name;
    g_recordBuffer.clear();
    return true;
}
