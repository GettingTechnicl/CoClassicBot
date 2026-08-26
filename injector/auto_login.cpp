#include "auto_login.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr const char* kLoginWindowTitle = "[ClassicConquer]";

struct ChildControl
{
    HWND hwnd;
    std::string className;
    std::string text;
};

std::vector<ChildControl> EnumerateChildren(HWND parent)
{
    std::vector<ChildControl> out;
    EnumChildWindows(parent, [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* out = reinterpret_cast<std::vector<ChildControl>*>(lParam);
        char cls[128] = {};
        GetClassNameA(hwnd, cls, sizeof(cls));
        char text[256] = {};
        GetWindowTextA(hwnd, text, sizeof(text));
        out->push_back({hwnd, cls, text});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    return out;
}

bool ContainsCaseInsensitive(const std::string& haystack, const char* needle)
{
    std::string lowerHay = haystack;
    std::string lowerNeedle = needle;
    for (char& c : lowerHay) c = static_cast<char>(tolower((unsigned char)c));
    for (char& c : lowerNeedle) c = static_cast<char>(tolower((unsigned char)c));
    return lowerHay.find(lowerNeedle) != std::string::npos;
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

    // The window can take a moment to finish laying out its child controls
    // right after it first appears.
    Sleep(500);

    const std::vector<ChildControl> children = EnumerateChildren(loginWnd);
    printf("[auto-login] Enumerated %zu child control(s).\n", children.size());

    HWND usernameEdit = nullptr;
    HWND passwordEdit = nullptr;
    HWND serverCombo = nullptr;
    HWND loginButton = nullptr;

    for (const auto& child : children) {
        if (child.className == "Edit") {
            // First Edit found = Username, second = Password — matches the
            // form's visual top-to-bottom order, which for a standard Win32
            // dialog matches child enumeration/creation order.
            if (!usernameEdit) usernameEdit = child.hwnd;
            else if (!passwordEdit) passwordEdit = child.hwnd;
        } else if (child.className == "ComboBox") {
            serverCombo = child.hwnd;
        } else if (child.className == "Button" && ContainsCaseInsensitive(child.text, "login")) {
            loginButton = child.hwnd;
        }
    }

    if (!usernameEdit || !passwordEdit || !loginButton) {
        printf("[auto-login] Could not identify all required controls "
            "(username=%p password=%p server=%p login=%p). Control layout may "
            "have changed — see auto_login.cpp's classification heuristic.\n",
            (void*)usernameEdit, (void*)passwordEdit, (void*)serverCombo, (void*)loginButton);
        return false;
    }

    SendMessageA(usernameEdit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(request.username.c_str()));
    SendMessageA(passwordEdit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(request.password.c_str()));
    printf("[auto-login] Username/password fields filled.\n");

    if (serverCombo && !request.server.empty()) {
        const LRESULT result = SendMessageA(serverCombo, CB_SELECTSTRING,
            static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(request.server.c_str()));
        if (result == CB_ERR)
            printf("[auto-login] Server \"%s\" not found in dropdown, leaving default selection.\n", request.server.c_str());
        else
            printf("[auto-login] Server set to \"%s\".\n", request.server.c_str());
    }

    Sleep(200);
    SendMessageA(loginButton, BM_CLICK, 0, 0);
    printf("[auto-login] Login clicked.\n");

    // Wait for the login window to close (submitted) rather than returning
    // immediately, so the caller's next step (waiting on the game process /
    // proceeding to injection) doesn't race the login handshake.
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

}  // namespace AutoLogin
