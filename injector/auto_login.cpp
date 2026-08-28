#include "auto_login.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

// Multi-account manager support: SendInput() and SetForegroundWindow() are
// GLOBAL, desktop-wide operations, not scoped to whichever HWND was
// resolved by PID — live-reported, with 3+ accounts reconnecting around
// the same time, one account's ForceForegroundWindow() call could steal
// focus mid-typing from a DIFFERENT account's in-flight keystroke
// sequence, interleaving credentials between the two windows. Serializes
// just the foreground+keystroke portion of PerformLoginViaSendInput
// (below) across every concurrent login/reconnect attempt in this
// process — not the completion-wait that follows it, which doesn't touch
// shared desktop state and would otherwise pointlessly block OTHER
// accounts' typing behind one account's own up-to-8s completion
// detection. PerformLoginViaMessages (the locked-desktop path) doesn't
// need this: PostMessage/SendMessageTimeout target a specific HWND's own
// message queue directly, with no shared global-focus dependency.
std::mutex g_sendInputMutex;


constexpr const char* kLoginWindowTitle = "[ClassicConquer]";

// Session 10: EnumChildWindows found ZERO child controls on this window —
// confirmed live. The login form is custom-rendered (like the modern-
// styled launcher window), not built from real Win32 Edit/Button/ComboBox
// controls, so WM_SETTEXT/BM_CLICK have nothing to target.
//
// Sessions 12-13 tried driving it with coordinate-based synthetic clicks
// (fixed pixel offsets from the client-area center, measured once at a
// 1536x1112 window). That broke as soon as the window's actual size/DPI
// diverged from that one calibration — first over RDP (window ~1380x890),
// then even sitting locally at the PC (cause unconfirmed, but the click
// landed on the password field instead of username both times), each
// time in a different, hard-to-predict way.
//
// Session 13 fix (live-confirmed by the user): NO mouse click is needed
// at all. The login window's default post-launch focus state responds to
// plain Tab-navigation — one Tab reaches the username field, a second
// Tab reaches the password field — which sidesteps the coordinate/DPI
// problem entirely instead of chasing another pixel recalibration.

// SetForegroundWindow alone is unreliable when called from a background
// process — Windows deliberately restricts which processes may steal
// foreground focus, and a plain SetForegroundWindow call can silently no-op
// (just flash the taskbar icon instead). AttachThreadInput temporarily joins
// this thread's input state with the target window's owning thread, which is
// the standard, reliable way external tools force focus onto another
// process's window. Returns whether the target actually ended up foreground
// — checked explicitly rather than assumed, since a silent failure here
// means every subsequent keystroke goes to the wrong place.
bool ForceForegroundWindow(HWND target)
{
    const DWORD targetThreadId = GetWindowThreadProcessId(target, nullptr);
    const DWORD currentThreadId = GetCurrentThreadId();

    const bool attached = targetThreadId != currentThreadId
        && AttachThreadInput(currentThreadId, targetThreadId, TRUE) != 0;

    BringWindowToTop(target);
    SetForegroundWindow(target);
    SetFocus(target);
    SetActiveWindow(target);

    if (attached)
        AttachThreadInput(currentThreadId, targetThreadId, FALSE);

    return GetForegroundWindow() == target;
}

// Multi-account manager support: FindWindowA(nullptr, title) matches ANY
// top-level window on the desktop with that exact title, with no way to
// tell which process it belongs to. That's fine for a single game window,
// but with two+ ImConquer.exe processes running concurrently (each showing
// the identical "[ClassicConquer]" login title before its own auto-login
// completes), a plain title search would nondeterministically grab
// whichever one happens to match first — driving credentials into the
// wrong account's login window. Filtering by the target process's PID
// (already known to the caller, from the CreateProcessA call that just
// launched it) disambiguates correctly regardless of how many other game
// windows are open at the same time.
struct FindLoginWindowContext
{
    DWORD targetPid = 0;
    HWND  found = nullptr;
};

BOOL CALLBACK EnumLoginWindowProc(HWND hwnd, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<FindLoginWindowContext*>(lParam);
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid != ctx->targetPid)
        return TRUE;  // keep enumerating

    char title[256] = {};
    GetWindowTextA(hwnd, title, sizeof(title));
    if (strcmp(title, kLoginWindowTitle) == 0) {
        ctx->found = hwnd;
        return FALSE;  // stop — found it
    }
    return TRUE;
}

// Single EnumWindows pass, no waiting. Shared by FindLoginWindow's
// bounded-wait loop below and by IsAtLoginScreen's one-shot reconnect
// check (AutoLogin::IsAtLoginScreen).
HWND FindLoginWindowOnce(DWORD targetPid)
{
    FindLoginWindowContext ctx{targetPid, nullptr};
    EnumWindows(EnumLoginWindowProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

HWND FindLoginWindow(DWORD targetPid, uint32_t timeoutMs)
{
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        if (HWND hwnd = FindLoginWindowOnce(targetPid))
            return hwnd;
        Sleep(250);
    }
    return nullptr;
}

// OpenInputDesktop fails outright when the session is on a secure desktop
// (Winlogon lock screen, UAC prompt, Ctrl+Alt+Del screen) rather than the
// normal interactive one — and even when it succeeds, the returned desktop's
// name is "Winlogon" (or similar) instead of "Default" in that state. Either
// outcome means SendInput/SetForegroundWindow — which only operate against
// whatever desktop is currently attached to input — cannot reach the game
// window, since the game's own desktop object still exists but is no longer
// the one receiving real input. This is the standard, well-known technique
// for detecting that condition from outside the locked session.
bool IsInputDesktopLocked()
{
    HDESK hDesk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!hDesk)
        return true;

    char name[256] = {};
    DWORD needed = 0;
    const bool gotName = GetUserObjectInformationA(hDesk, UOI_NAME, name, sizeof(name), &needed) != 0;
    CloseDesktop(hDesk);

    if (!gotName)
        return true;

    return _stricmp(name, "Default") != 0;
}

// Posts synthetic keyboard messages straight to the window's message queue
// instead of generating real hardware-level input via SendInput. This does
// NOT require attachment to the active input desktop — it works purely
// through the target thread's own message pump, which keeps running whether
// or not the session is locked. Whether the game's custom UI framework
// actually consults window messages for its own focus/typing (as opposed to
// polling raw input) is unverified; this is a best-effort path for the
// locked case where SendInput is guaranteed not to work at all.
void PostText(HWND wnd, const std::string& text)
{
    for (char c : text) {
        SendMessageTimeoutA(wnd, WM_CHAR, static_cast<WPARAM>(static_cast<unsigned char>(c)), 1,
            SMTO_NORMAL, 500, nullptr);
        Sleep(20);
    }
}

// Posts a Tab/Enter key press to the window's message queue -- same
// locked-session rationale as PostText above.
void PostKey(HWND wnd, WORD vk)
{
    SendMessageTimeoutA(wnd, WM_KEYDOWN, vk, 0, SMTO_NORMAL, 500, nullptr);
    Sleep(20);
    SendMessageTimeoutA(wnd, WM_KEYUP, vk, 0, SMTO_NORMAL, 500, nullptr);
}

// Types via KEYEVENTF_UNICODE (one synthetic keypress per character) rather
// than mapping to virtual-key codes — robust regardless of keyboard layout
// and handles any character the password/username could contain.
void SendText(const std::string& text)
{
    for (char c : text) {
        INPUT down = {};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = static_cast<WORD>(static_cast<unsigned char>(c));
        down.ki.dwFlags = KEYEVENTF_UNICODE;

        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

        INPUT batch[2] = {down, up};
        SendInput(2, batch, sizeof(INPUT));
        Sleep(20);  // a small per-character delay; some UI frameworks drop
                    // keystrokes sent faster than they poll input
    }
}

// Sends a real (non-text) virtual-key press, e.g. Tab/Enter for form
// navigation — deliberately NOT KEYEVENTF_UNICODE, since that path is for
// typing literal characters, not triggering focus-traversal/submit keys.
void SendKey(WORD vk)
{
    INPUT down = {};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = vk;

    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_KEYUP;

    INPUT batch[2] = {down, up};
    SendInput(2, batch, sizeof(INPUT));
}

// Locked-session path: no real input desktop attachment exists to force
// foreground onto, so this skips ForceForegroundWindow entirely and posts
// straight to the window handle — see PostText above for why that doesn't
// need it. Same Tab-only navigation as PerformLoginViaSendInput below.
bool PerformLoginViaMessages(HWND loginWnd, const AutoLoginRequest& request, uint32_t timeoutMs, bool isReconnect)
{
    printf("[auto-login] Session is locked — driving login via posted window messages "
        "(SendInput cannot reach the input desktop in this state).\n");

    // Multi-account manager support: reconnecting after an in-game
    // disconnect shows an "Error: Connection with the server is
    // interrupted. Please re-login." banner FIRST, live-reported —
    // dismissed with Enter/Space before the form is usable, unlike a
    // fresh post-launch login which has no such banner. Only sent on the
    // reconnect path; an unconditional stray Enter on a fresh login could
    // submit the form prematurely.
    if (isReconnect) {
        PostKey(loginWnd, VK_RETURN);
        Sleep(500);
        printf("[auto-login] Reconnect: dismissed disconnect banner.\n");
    }

    PostKey(loginWnd, VK_TAB);
    Sleep(1000);
    PostText(loginWnd, request.username);
    printf("[auto-login] Username posted.\n");

    PostKey(loginWnd, VK_TAB);
    Sleep(500);
    PostText(loginWnd, request.password);
    printf("[auto-login] Password posted.\n");

    Sleep(200);
    PostKey(loginWnd, VK_RETURN);
    printf("[auto-login] Login submitted via Enter.\n");

    const DWORD waitStart = GetTickCount();
    while (IsWindow(loginWnd) && GetTickCount() - waitStart < timeoutMs)
        Sleep(250);

    if (IsWindow(loginWnd)) {
        printf("[auto-login] Login window still open after %ums — message-based login did not "
            "complete. The custom UI may not consult posted window messages for input; this path "
            "needs a live-locked test to confirm either way.\n", timeoutMs);
        return false;
    }

    printf("[auto-login] Login window closed — proceeding.\n");
    return true;
}

bool PerformLoginViaSendInput(HWND loginWnd, const AutoLoginRequest& request, uint32_t timeoutMs, bool isReconnect)
{
    {
        // Multi-account manager support: holds g_sendInputMutex for exactly
        // the foreground+keystroke span — see its own comment for why this
        // is needed at all (SendInput/SetForegroundWindow are global, not
        // per-window) and why the lock is released before the
        // completion-wait loop below rather than held for this whole
        // function.
        std::lock_guard<std::mutex> lock(g_sendInputMutex);

        if (ForceForegroundWindow(loginWnd))
            printf("[auto-login] Login window confirmed foreground.\n");
        else
            printf("[auto-login] WARNING: login window did NOT become foreground — "
                "keystrokes will likely go to the wrong window.\n");
        Sleep(200);

        // Multi-account manager support: reconnecting after an in-game
        // disconnect shows an "Error: Connection with the server is
        // interrupted. Please re-login." banner FIRST, live-reported —
        // dismissed with Enter/Space before the form is usable, unlike a
        // fresh post-launch login which has no such banner. Only sent on
        // the reconnect path; an unconditional stray Enter on a fresh
        // login could submit the form prematurely.
        if (isReconnect) {
            SendKey(VK_RETURN);
            Sleep(500);
            printf("[auto-login] Reconnect: dismissed disconnect banner.\n");
        }

        // Session 13: live-confirmed — no click needed. The window's default
        // post-launch focus state responds directly to Tab: first Tab reaches
        // the username field, second Tab reaches the password field. This
        // replaces the coordinate-based clicking that kept breaking across
        // different window sizes/DPI (see the file-header comment for history).
        //
        // First live run of THIS approach dropped the first few characters of
        // the username ("Shooter411" came out as "er411" -- exactly the first
        // 5 chars missing, matching SendText's 20ms/char pace) because the
        // field wasn't actually ready to receive input yet this soon after
        // Tab/window-launch, even though focus had already moved. Widened both
        // post-Tab delays well past the ~100ms that was dropped, with margin.
        SendKey(VK_TAB);
        Sleep(1000);
        SendText(request.username);
        printf("[auto-login] Username typed.\n");

        SendKey(VK_TAB);
        Sleep(500);
        SendText(request.password);
        printf("[auto-login] Password typed.\n");

        // Server dropdown intentionally left untouched: this game currently
        // only exposes one real option ("Classic (US)") and it's not a real
        // Win32 combo box to drive programmatically anyway. Revisit if/when
        // multi-server support is actually needed.

        Sleep(200);
        SendKey(VK_RETURN);
        printf("[auto-login] Login submitted via Enter.\n");
    }  // g_sendInputMutex released -- the completion-wait below doesn't touch shared desktop state

    const DWORD waitStart = GetTickCount();
    while (IsWindow(loginWnd) && GetTickCount() - waitStart < timeoutMs)
        Sleep(250);

    if (IsWindow(loginWnd)) {
        printf("[auto-login] Login window still open after %ums — login may have failed "
            "(wrong password, banned account, etc.) or is taking unusually long.\n", timeoutMs);
        return false;
    }

    printf("[auto-login] Login window closed — proceeding.\n");
    return true;
}

}  // namespace

namespace AutoLogin {

bool PerformLogin(const AutoLoginRequest& request, uint32_t targetPid, uint32_t timeoutMs, bool isReconnect)
{
    printf("[auto-login] Waiting for login window (\"%s\") for pid=%u...\n", kLoginWindowTitle, targetPid);
    HWND loginWnd = FindLoginWindow(static_cast<DWORD>(targetPid), timeoutMs);
    if (!loginWnd) {
        printf("[auto-login] Login window did not appear within %ums.\n", timeoutMs);
        return false;
    }
    printf("[auto-login] Login window found.\n");

    Sleep(1500);  // let it finish laying out/rendering after first appearing

    // Session 12: live RDP test showed SendInput-driven keystrokes DO reach
    // the game over RDP (unlike the true-lock case), ruling out an earlier
    // wrong hypothesis about RDP needing the message-posting path for any
    // reason other than a genuinely locked desktop.
    if (IsInputDesktopLocked())
        return PerformLoginViaMessages(loginWnd, request, timeoutMs, isReconnect);
    return PerformLoginViaSendInput(loginWnd, request, timeoutMs, isReconnect);
}

bool IsAtLoginScreen(uint32_t targetPid)
{
    return FindLoginWindowOnce(static_cast<DWORD>(targetPid)) != nullptr;
}

}  // namespace AutoLogin
