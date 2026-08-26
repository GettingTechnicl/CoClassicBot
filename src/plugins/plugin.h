#pragma once
#include "base.h"

class CRole;

class IPlugin {
public:
    virtual ~IPlugin() = default;
    virtual const char* GetName() const = 0;
    virtual void Update() = 0;
    virtual void RenderUI() = 0;
    virtual bool OnMapClick(const Position& tile) { return false; }

    // Per-entity render callbacks (optional overrides)
    virtual bool OnPreRenderEntity(CRole* entity) { return true; }
    virtual void OnPostRenderEntity(CRole* entity) {}

    // Called once, only after a crash-recovery relaunch (see dllmain.cpp),
    // never on a normal fresh login — re-applies this plugin's own
    // persisted "enabled" state to whatever live flag it actually checks at
    // runtime. Default no-op: most plugins (Mining/Mule/Follow/AimHelper)
    // check their own settings.enabled directly every frame, so the normal
    // LoadConfig() path already resumes them correctly with no extra step.
    // Only BaseHuntPlugin needs a real override — its live m_enabled is a
    // separate flag from AutoHuntSettings.enabled, kept in sync by
    // ApplyHuntModeSelection() rather than read directly, because autohunt
    // has multiple mutually-exclusive mode plugins sharing one settings
    // enabled/combatMode pair.
    virtual void ResumeEnabledStateFromSettings() {}

    bool m_enabled = true;
};
