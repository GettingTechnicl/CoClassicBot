#pragma once

#include <string>
#include <cstdint>

// =====================================================================
// auto_login.h — drives ImConquer.exe's own native login window from
// OUTSIDE the process, purely via standard Win32 window messages
// (WM_SETTEXT / BM_CLICK), the same mechanism any accessibility tool or
// UI-automation script uses.
//
// Deliberately NOT done from inside coclassic.dll: dllmain.cpp's
// InitThread() stays completely idle until login has already completed,
// specifically because doing active work (hooking, D3D device creation)
// before that point triggers the server's "virtual machine detected"
// integrity check. This module never touches ImConquer.exe's own code
// or memory, so it doesn't carry that risk — but keeping the automation
// external, before our DLL does anything, preserves that discipline.
// =====================================================================

struct AutoLoginRequest
{
    std::string username;
    std::string password;
    std::string server;  // exact text as it appears in the Server dropdown, e.g. "Classic (US)"
};

namespace AutoLogin {

// Waits (up to timeoutMs) for ImConquer.exe's login window to appear, fills
// in the given credentials, and clicks Login. Returns true if the window
// was found and all fields/the Login button were successfully driven —
// this does NOT confirm the login itself succeeded (wrong password, banned
// account, etc. all still show true here), only that the UI was operated.
// Logs each step to stdout so a failed run is diagnosable without a debugger.
bool PerformLogin(const AutoLoginRequest& request, uint32_t timeoutMs = 30000);

}  // namespace AutoLogin
