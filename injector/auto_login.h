#pragma once

#include <string>
#include <cstdint>

// =====================================================================
// auto_login.h — drives ImConquer.exe's own native login window from
// OUTSIDE the process, the same category of thing any accessibility tool
// or UI-automation script does.
//
// The login form is custom-rendered (confirmed live: EnumChildWindows
// finds zero child controls), so there are no real Edit/Button HWNDs to
// target with WM_SETTEXT/BM_CLICK. Two input methods are layered:
//
//   1. Synthetic WM_LBUTTONDOWN/UP + WM_CHAR posted directly to the
//      top-level window at measured client-area coordinates. This is a
//      message-queue post, not a hardware-level input event, so it does
//      NOT require attachment to the active input desktop — it should
//      work even while the session is locked, PROVIDED the game's UI
//      framework actually reads window messages for its own hit-testing
//      (unverified — this is the reason for method 2 as a safety net).
//   2. Real SendInput mouse clicks + KEYEVENTF_UNICODE keystrokes at the
//      same coordinates (the original, live-proven method). This DOES
//      require the active input desktop, so it cannot work on a locked
//      session, but stays as the fallback so the normal unlocked case
//      never regresses if method 1 turns out not to be recognized by
//      the game's custom UI.
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
