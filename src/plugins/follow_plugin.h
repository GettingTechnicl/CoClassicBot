#pragma once
#include "plugin.h"

struct FollowSettings
{
    bool enabled            = false;
    char targetName[16]     = "";
    int  followDistance     = 3;
    // Floor is 2, not 1: at dodgeRadius==1 the trigger (mobDist <= dodgeRadius) only fires once
    // a monster is ALREADY adjacent, which is already within melee range for most monsters and
    // too late to be a preemptive dodge. 2 gives at least one tile of buffer before melee range
    // and also covers the 2-tile-reach monsters the game has (see follow_plugin.cpp's UI note —
    // there's no per-monster attack-range data available client-side to do better than one
    // global floor right now).
    int  dodgeRadius       = 2;
};

FollowSettings& GetFollowSettings();

class FollowPlugin : public IPlugin {
public:
    const char* GetName() const override { return "Follow"; }
    void Update() override;
    void RenderUI() override;

private:
    enum class State { Idle, Following, Dodging };

    CRole* FindTarget() const;
    int  NearestMobDistance() const;

    State m_state        = State::Idle;
    DWORD m_lastJumpTick = 0;
};
