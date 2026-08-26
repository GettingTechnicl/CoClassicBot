#include "auto_login.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* kLoginWindowTitle = "[ClassicConquer]";

// Session 10: EnumChildWindows found ZERO child controls on this window —
// confirmed live. The login form is custom-rendered (like the modern-
// styled launcher window), not built from real Win32 Edit/Button/ComboBox
// controls, so WM_SETTEXT/BM_CLICK have nothing to target. Falls back to
// coordinate-based synthetic input instead: real mouse clicks + keystrokes.
//
// The login panel is a small FIXED-SIZE box that stays centered in the
// window regardless of window size (confirmed live — this window is
// resizable) — so positions are anchored as constant PIXEL offsets from
// the window's CLIENT AREA center (not a fraction of window size, which
// would be wrong for a fixed-size panel, and not the outer window rect,
// since the game's own D3D render target is the client area — the title
// bar is separate OS chrome it never draws into). Measured directly off a
// live screenshot at a 1536x1112 window: field centers at y=672 (Username)
// /710 (Password)/793 (Login button), all x=767; client area top-left in
// that same capture was ~(0,31) with height ~1081, giving a client center
// of (768,571) and these offsets.
struct FieldOffset { long dx; long dy; };
constexpr FieldOffset kUsernameField{-1, 101};
constexpr FieldOffset kPasswordField{-1, 139};
constexpr FieldOffset kLoginButton{-1, 222};

// SetForegroundWindow alone is unreliable when called from a background
// process — Windows deliberately restricts which processes may steal
// foreground focus, and a plain SetForegroundWindow call can silently no-op
// (just flash the taskbar icon instead). AttachThreadInput temporarily joins
// this thread's input state with the target window's owning thread, which is
// the standard, reliable way external tools force focus onto another
// process's window. Returns whether the target actually ended up foreground
// — checked explicitly rather than assumed, since a silent failure here
// means every subsequent click/keystroke goes to the wrong place.
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

HWND FindLoginWindow(uint32_t timeoutMs)
{
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        if (HWND hwnd = FindWindowA(nullptr, kLoginWindowTitle))
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

POINT ResolveClientPoint(HWND wnd, FieldOffset offset)
{
    RECT clientRect{};
    GetClientRect(wnd, &clientRect);  // always (0,0,width,height) — origin is client-relative
    return POINT{
        (clientRect.right - clientRect.left) / 2 + offset.dx,
        (clientRect.bottom - clientRect.top) / 2 + offset.dy
    };
}

// Posts synthetic mouse/keyboard messages straight to the window's message
// queue instead of generating real hardware-level input via SendInput. This
// does NOT require attachment to the active input desktop — it works purely
// through the target thread's own message pump, which keeps running whether
// or not the session is locked. Whether the game's custom UI framework
// actually consults window messages for its own hit-testing/typing (as
// opposed to polling raw input) is unverified; this is a best-effort path
// for the locked case where SendInput is guaranteed not to work at all.
void PostClick(HWND wnd, POINT clientPt)
{
    const LPARAM lParam = MAKELPARAM(clientPt.x, clientPt.y);
    SendMessageTimeoutA(wnd, WM_MOUSEMOVE, 0, lParam, SMTO_NORMAL, 500, nullptr);
    SendMessageTimeoutA(wnd, WM_LBUTTONDOWN, MK_LBUTTON, lParam, SMTO_NORMAL, 500, nullptr);
    Sleep(30);
    SendMessageTimeoutA(wnd, WM_LBUTTONUP, 0, lParam, SMTO_NORMAL, 500, nullptr);
}

void PostText(HWND wnd, const std::string& text)
{
    for (char c : text) {
        SendMessageTimeoutA(wnd, WM_CHAR, static_cast<WPARAM>(static_cast<unsigned char>(c)), 1,
            SMTO_NORMAL, 500, nullptr);
        Sleep(20);
    }
}

POINT ResolveScreenPoint(HWND wnd, FieldOffset offset)
{
    RECT clientRect{};
    GetClientRect(wnd, &clientRect);  // always (0,0,width,height) — origin is client-relative
    POINT center{(clientRect.right - clientRect.left) / 2, (clientRect.bottom - clientRect.top) / 2};
    ClientToScreen(wnd, &center);  // now a real screen coordinate
    POINT pt;
    pt.x = center.x + offset.dx;
    pt.y = center.y + offset.dy;
    return pt;
}

void SendClick(POINT screenPt)
{
    // MOUSEEVENTF_ABSOLUTE coordinates are normalized 0-65535 across
    // whichever area MOUSEEVENTF_VIRTUALDESK selects. This machine has two
    // monitors — normalizing against SM_CXSCREEN/SM_CYSCREEN (primary
    // monitor only) would misplace clicks if the game window is on the
    // second one, so use the full virtual desktop's origin/size instead.
    const int vLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // Move the cursor there first (some UI frameworks need a real mouse-move
    // event before a click registers, not just a click at a location the
    // cursor never visited) then click.
    INPUT inputs[3] = {};

    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dx = static_cast<LONG>((screenPt.x - vLeft) * (65535.0 / vWidth));
    inputs[0].mi.dy = static_cast<LONG>((screenPt.y - vTop) * (65535.0 / vHeight));
    inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;

    inputs[2].type = INPUT_MOUSE;
    inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;

    SendInput(3, inputs, sizeof(INPUT));
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

// Locked-session path: no real input desktop attachment exists to force
// foreground onto, so this skips ForceForegroundWindow entirely and posts
// straight to the window handle — see PostClick/PostText above for why that
// doesn't need it. Uses the same measured field offsets as the SendInput
// path, just resolved to client-relative coordinates instead of screen ones.
bool PerformLoginViaMessages(HWND loginWnd, const AutoLoginRequest& request, uint32_t timeoutMs)
{
    printf("[auto-login] Session is locked — driving login via posted window messages "
        "(SendInput cannot reach the input desktop in this state).\n");

    const POINT usernamePt = ResolveClientPoint(loginWnd, kUsernameField);
    printf("[auto-login] Posting click to username field at client (%ld,%ld)\n", usernamePt.x, usernamePt.y);
    PostClick(loginWnd, usernamePt);
    Sleep(600);
    PostText(loginWnd, request.username);
    printf("[auto-login] Username posted.\n");

    const POINT passwordPt = ResolveClientPoint(loginWnd, kPasswordField);
    printf("[auto-login] Posting click to password field at client (%ld,%ld)\n", passwordPt.x, passwordPt.y);
    PostClick(loginWnd, passwordPt);
    Sleep(150);
    PostText(loginWnd, request.password);
    printf("[auto-login] Password posted.\n");

    Sleep(200);
    const POINT loginBtnPt = ResolveClientPoint(loginWnd, kLoginButton);
    printf("[auto-login] Posting click to Login button at client (%ld,%ld)\n", loginBtnPt.x, loginBtnPt.y);
    PostClick(loginWnd, loginBtnPt);
    printf("[auto-login] Login posted.\n");

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

bool PerformLoginViaSendInput(HWND loginWnd, const AutoLoginRequest& request, uint32_t timeoutMs)
{
    if (ForceForegroundWindow(loginWnd))
        printf("[auto-login] Login window confirmed foreground.\n");
    else
        printf("[auto-login] WARNING: login window did NOT become foreground — "
            "clicks/keystrokes will likely go to the wrong window.\n");
    Sleep(200);

    RECT windowRect{};
    if (GetWindowRect(loginWnd, &windowRect)) {
        printf("[auto-login] Login window rect: (%ld,%ld)-(%ld,%ld)\n",
            windowRect.left, windowRect.top, windowRect.right, windowRect.bottom);
    }

    const POINT usernamePt = ResolveScreenPoint(loginWnd, kUsernameField);
    printf("[auto-login] Clicking username field at (%ld,%ld)\n", usernamePt.x, usernamePt.y);
    SendClick(usernamePt);
    // Session 10: click coordinates confirmed pixel-accurate live (matched
    // the measured field position almost exactly), yet username still came
    // up empty while password (identical click+type pattern, just later in
    // the sequence) worked. Two candidate causes, addressed together since
    // there's no way to isolate them without another live round-trip: (1) a
    // Ctrl+A select-all was sent before typing username but not password —
    // if this custom-rendered UI doesn't handle Ctrl+A like a real text
    // field, that keystroke could itself be what dropped focus/input state
    // right before typing; removed rather than debugged further, since nothing
    // here actually needs it (the account picker always provides the full
    // username, so there's no partial-text-append risk to guard against).
    // (2) the field may simply not be ready to receive input yet this soon
    // after the window first appears/gains focus — the 150ms gap here was
    // much shorter than the ~700ms+ that had already elapsed by the time
    // password's click ran. Longer, equal delay before both now.
    Sleep(600);
    SendText(request.username);
    printf("[auto-login] Username typed.\n");

    const POINT passwordPt = ResolveScreenPoint(loginWnd, kPasswordField);
    printf("[auto-login] Clicking password field at (%ld,%ld)\n", passwordPt.x, passwordPt.y);
    SendClick(passwordPt);
    Sleep(150);
    SendText(request.password);
    printf("[auto-login] Password typed.\n");

    // Server dropdown intentionally left untouched: this game currently
    // only exposes one real option ("Classic (US)") and it's not a real
    // Win32 combo box to drive programmatically anyway. Revisit if/when
    // multi-server support is actually needed.

    Sleep(200);
    const POINT loginBtnPt = ResolveScreenPoint(loginWnd, kLoginButton);
    printf("[auto-login] Clicking Login button at (%ld,%ld)\n", loginBtnPt.x, loginBtnPt.y);
    SendClick(loginBtnPt);
    printf("[auto-login] Login clicked.\n");

    const DWORD waitStart = GetTickCount();
    while (IsWindow(loginWnd) && GetTickCount() - waitStart < timeoutMs)
        Sleep(250);

    if (IsWindow(loginWnd)) {
        printf("[auto-login] Login window still open after %ums — login may have failed "
            "(wrong password, banned account, misaligned click coordinates, etc.) or is "
            "taking unusually long.\n", timeoutMs);
        return false;
    }

    printf("[auto-login] Login window closed — proceeding.\n");
    return true;
}

}  // namespace

namespace AutoLogin {

bool PerformLogin(const AutoLoginRequest& request, uint32_t timeoutMs)
{
    printf("[auto-login] Waiting for login window (\"%s\")...\n", kLoginWindowTitle);
    HWND loginWnd = FindLoginWindow(timeoutMs);
    if (!loginWnd) {
        printf("[auto-login] Login window did not appear within %ums.\n", timeoutMs);
        return false;
    }
    printf("[auto-login] Login window found.\n");

    Sleep(1500);  // let it finish laying out/rendering after first appearing

    // Session 12: live RDP test showed SendInput-driven clicks/keystrokes DO
    // reach the game over RDP (unlike the true-lock case) — they just landed
    // on the wrong coordinates, because the field offsets are fixed pixel
    // constants measured at one specific window size and don't hold at a
    // different size/DPI. That's a coordinate bug, not a delivery bug, so
    // remote-session is not treated as a reason to route away from the
    // proven SendInput path — only a genuinely locked desktop is.
    if (IsInputDesktopLocked())
        return PerformLoginViaMessages(loginWnd, request, timeoutMs);
    return PerformLoginViaSendInput(loginWnd, request, timeoutMs);
}

}  // namespace AutoLogin
