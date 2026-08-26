#include "auto_login.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>
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

    Sleep(500);  // let it finish laying out/rendering after first appearing

    SetForegroundWindow(loginWnd);
    Sleep(200);

    RECT windowRect{};
    if (GetWindowRect(loginWnd, &windowRect)) {
        printf("[auto-login] Login window rect: (%ld,%ld)-(%ld,%ld)\n",
            windowRect.left, windowRect.top, windowRect.right, windowRect.bottom);
    }

    const POINT usernamePt = ResolveScreenPoint(loginWnd, kUsernameField);
    printf("[auto-login] Clicking username field at (%ld,%ld)\n", usernamePt.x, usernamePt.y);
    SendClick(usernamePt);
    Sleep(150);
    // Select-all first in case the field has leftover text from a previous
    // session (e.g. the game's own "Remember Login?" username autofill)
    // before typing the real value.
    {
        INPUT ctrlA[4] = {};
        ctrlA[0].type = INPUT_KEYBOARD; ctrlA[0].ki.wVk = VK_CONTROL;
        ctrlA[1].type = INPUT_KEYBOARD; ctrlA[1].ki.wVk = 'A';
        ctrlA[2].type = INPUT_KEYBOARD; ctrlA[2].ki.wVk = 'A'; ctrlA[2].ki.dwFlags = KEYEVENTF_KEYUP;
        ctrlA[3].type = INPUT_KEYBOARD; ctrlA[3].ki.wVk = VK_CONTROL; ctrlA[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, ctrlA, sizeof(INPUT));
    }
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

}  // namespace AutoLogin
