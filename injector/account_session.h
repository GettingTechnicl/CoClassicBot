#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "credentials.h"

// =====================================================================
// account_session.h — per-account runtime state.
//
// Multi-account manager support: everything one account's launch+inject+
// login+supervise/relaunch run needs to track lives here instead of as
// main()-local variables or process-global statics, so more than one of
// these can exist and run concurrently (each on its own worker thread) —
// see RunAccountSupervisionLoop() in main.cpp, and
// ~/.claude/plans/zazzy-wondering-diffie.md for the full design.
//
// Ownership: the manager UI thread creates/owns every AccountSession and
// is the only thing that ever touches the session LIST itself. A given
// session's worker thread only ever touches that one session's own
// fields (state/gamePid/statusText/etc.) — never another session's, and
// never the list. That split is what makes this safe without a lock
// around the session list.
// =====================================================================

enum class SessionState
{
    Idle,       // never launched, or fully stopped/exited cleanly
    Launching,  // CreateProcessA in flight
    Injecting,  // coclassic.dll injection in flight
    LoggingIn,  // AutoLogin::PerformLogin in flight
    Running,    // logged in, being supervised
    Stopping,   // stop requested, waiting for the worker thread to unwind
    Crashed,    // game exited unexpectedly, about to relaunch
    Failed,     // gave up (fast-crash loop, hard launch/inject failure)
};

struct AccountSession
{
    AccountProfile profile;

    std::thread worker;

    std::atomic<SessionState> state{SessionState::Idle};
    std::atomic<DWORD> gamePid{0};

    // Fix B (race 2): a duplicated, SYNCHRONIZE-only handle to the most
    // recently launched game process for this account. Kept specifically so
    // a later Login click can probe genuine liveness of the PRIOR run's
    // process. gamePid alone isn't enough for this: RunAccountSupervisionLoop
    // closes its own pi.hProcess every iteration, and re-opening by bare PID
    // afterward risks Windows having recycled that PID onto some unrelated
    // process by the time we check -- a duplicated HANDLE refers to the exact
    // kernel object and can't be confused by PID reuse. Only ever
    // written/replaced by this session's own worker thread; the UI thread
    // only ever reads it via a non-mutating WaitForSingleObject(..., 0) probe.
    std::atomic<HANDLE> lastGameHandle{nullptr};

    // Manual-reset. Signaled to ask the worker thread to stop supervising.
    HANDLE stopEvent = nullptr;

    // Distinguishes a per-account "Exit" click (kill the game too) from a
    // manager-wide "Exit Manager" (stop supervising, leave the game running
    // headless — matches this project's existing CTRL_CLOSE_EVENT behavior).
    std::atomic<bool> killGameOnStop{false};

    // Set by the worker thread just before it returns, so the UI thread can
    // poll-and-join instead of blocking (a worker may be mid-Inject() or
    // mid-login-wait, up to tens of seconds, when a stop is requested).
    std::atomic<bool> finished{false};

    // Loop-local state, now per-session — only ever touched by this
    // session's own worker thread, never the UI thread, so plain (not
    // atomic) storage is correct here.
    int  consecutiveFastCrashes = 0;
    // Separate from consecutiveFastCrashes on purpose: a stuck-login exit
    // (src/dllmain.cpp's kHeroWaitTimeoutMs) always has uptime well above
    // kFastCrashThresholdMs, so it would never trip the fast-crash counter --
    // this one is keyed on the exit CODE, not uptime. Uncapped by design:
    // retries forever with backoff after a few fast attempts rather than
    // giving up, since these failures are usually transient/environmental.
    // See its use in main.cpp.
    int  consecutiveStuckLogins = 0;
    bool pendingResumeMarker = false;

    mutable std::mutex statusMutex;
    std::string statusText;

    void SetStatus(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        statusText = text;
    }

    std::string GetStatus() const
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        return statusText;
    }
};
