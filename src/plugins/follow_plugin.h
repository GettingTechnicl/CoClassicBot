#pragma once
#include "plugin.h"

struct FollowSettings
{
    bool enabled            = false;
    char targetName[16]     = "";
    // Band, not a single trigger distance: a single threshold (the old `followDistance`) meant
    // ANY further step by the target re-triggered a jump, since the tile picker landed exactly
    // on the trigger boundary — constant hopping. Move only when outside [followMin, followMax];
    // land near the middle of the band, so the target has to cross real distance before the next
    // trigger. See follow_plugin.cpp's FindBestJumpTile.
    int  followMin          = 5;
    int  followMax          = 10;
    // Floor is 2, not 1: at dodgeRadius==1 the trigger (mobDist <= dodgeRadius) only fires once
    // a monster is ALREADY adjacent, which is already within melee range for most monsters and
    // too late to be a preemptive dodge. 2 gives at least one tile of buffer before melee range
    // and also covers the 2-tile-reach monsters the game has (see follow_plugin.cpp's UI note —
    // there's no per-monster attack-range data available client-side to do better than one
    // global floor right now). Measured in STEPS (CGameMap::StepDist), not TileDist — see its
    // header comment for why that distinction matters for a diagonal approach.
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
