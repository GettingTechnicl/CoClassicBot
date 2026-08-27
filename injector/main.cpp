#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WinSock2.h>
#include <Ws2tcpip.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "credentials.h"
#include "auto_login.h"
#include "account_session.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

static constexpr const char* GAME_EXE = "ImConquer.exe";
static constexpr const char* GAME_DIR = R"(C:\Program Files\Classic Conquer 2.0)";  // fallback default
static constexpr const char* DLL_NAME = "coclassic.dll";
static constexpr const char* SERVER_CONFIG_NAME = "servers.json";
static constexpr const char* LOCAL_RELAY_HOST = "127.0.0.1";
static constexpr const char* RELAY_LOG_NAME = "relay_packets.log";

struct Endpoint
{
    std::string host;
    uint16_t port = 0;
};

struct LaunchOptions
{
    std::optional<Endpoint> m_proxy;
    std::optional<Endpoint> m_targetOverride;
    std::string m_proxyUser;
    std::string m_proxyPassword;
    uint16_t m_relayPort = 0;
    bool m_showHelp = false;
    bool m_noPrompt = false;  // Skip SOCKS5 dialog if true
    bool m_packetLog = false;
    bool m_killSwitch = true;
};

// Forward declarations for dialog function
static bool ParseEndpoint(const std::string& text, Endpoint* endpoint);

// Simple input box using Windows API - returns true if user clicked OK
static bool InputBox(HWND parent, const char* title, const char* prompt, char* buffer, size_t bufferSize,
                     const char* defaultValue = "", bool password = false)
{
    WNDCLASSA wc = {};
    static bool classRegistered = false;
    static char inputBuffer[256];
    static bool inputConfirmed = false;

    if (!classRegistered) {
        wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
            switch (msg) {
            case WM_CREATE:
                {
                    CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
                    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
                }
                return 0;
            case WM_COMMAND:
                if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                    HWND edit = GetDlgItem(hwnd, 1001);
                    if (LOWORD(wParam) == IDOK) {
                        GetWindowTextA(edit, inputBuffer, sizeof(inputBuffer));
                        inputConfirmed = true;
                    } else {
                        inputConfirmed = false;
                    }
                    DestroyWindow(hwnd);
                }
                return 0;
            case WM_CLOSE:
                inputConfirmed = false;
                DestroyWindow(hwnd);
                return 0;
            }
            return DefWindowProcA(hwnd, msg, wParam, lParam);
        };
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = "InputBoxDlg";
        RegisterClassA(&wc);
        classRegistered = true;
    }

    strncpy_s(inputBuffer, defaultValue, sizeof(inputBuffer) - 1);
    inputBuffer[sizeof(inputBuffer) - 1] = 0;
    inputConfirmed = false;

    RECT rect;
    if (parent) {
        GetWindowRect(parent, &rect);
    } else {
        rect.left = 0; rect.top = 0;
        rect.right = GetSystemMetrics(SM_CXSCREEN);
        rect.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int x = rect.left + (rect.right - rect.left - 400) / 2;
    int y = rect.top + (rect.bottom - rect.top - 150) / 2;

    HWND dlg = CreateWindowExA(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        "InputBoxDlg",
        title,
        WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
        x, y, 400, 150,
        parent, nullptr, GetModuleHandleA(nullptr), nullptr);

    if (!dlg) return false;

    CreateWindowA("STATIC", prompt,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        10, 10, 370, 30, dlg, nullptr, GetModuleHandleA(nullptr), nullptr);

    DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL;
    if (password)
        editStyle |= ES_PASSWORD;

    HWND edit = CreateWindowA("EDIT", inputBuffer,
        editStyle,
        10, 45, 370, 25, dlg, reinterpret_cast<HMENU>(1001), GetModuleHandleA(nullptr), nullptr);

    CreateWindowA("BUTTON", "OK",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        100, 85, 90, 30, dlg, reinterpret_cast<HMENU>(IDOK), GetModuleHandleA(nullptr), nullptr);

    CreateWindowA("BUTTON", "Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        210, 85, 90, 30, dlg, reinterpret_cast<HMENU>(IDCANCEL), GetModuleHandleA(nullptr), nullptr);

    SetFocus(edit);
    
    MSG msg;
    while (IsWindow(dlg)) {
        if (GetMessageA(&msg, nullptr, 0, 0)) {
            if (!IsDialogMessage(dlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    }

    const bool confirmed = inputConfirmed;
    if (confirmed) {
        strncpy_s(buffer, bufferSize, inputBuffer, bufferSize - 1);
        buffer[bufferSize - 1] = 0;
    }
    // Session 12: inputBuffer is `static` and previously never cleared, so
    // whatever was last typed here — including a password, since `password`
    // only changes the EDIT control's display masking, not what's actually
    // stored — sat in static process memory indefinitely until some later,
    // unrelated InputBox() call happened to overwrite it.
    SecureZeroMemory(inputBuffer, sizeof(inputBuffer));
    return confirmed;
}

struct ManagedSocket
{
    explicit ManagedSocket(SOCKET value) : m_socket(value) {}

    SOCKET m_socket = INVALID_SOCKET;
    std::mutex m_mutex;
};

using ManagedSocketPtr = std::shared_ptr<ManagedSocket>;

static std::string EndpointToString(const Endpoint& endpoint)
{
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

static bool ParseUInt16(const std::string& text, uint16_t* value)
{
    if (!value || text.empty())
        return false;

    char* end = nullptr;
    unsigned long parsed = strtoul(text.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed > 65535)
        return false;

    *value = static_cast<uint16_t>(parsed);
    return true;
}

static bool ParseEndpoint(const std::string& text, Endpoint* endpoint)
{
    if (!endpoint || text.empty())
        return false;

    std::string host;
    std::string portText;

    if (text.front() == '[') {
        size_t close = text.find(']');
        if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':')
            return false;
        host = text.substr(1, close - 1);
        portText = text.substr(close + 2);
    } else {
        size_t colon = text.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size())
            return false;
        host = text.substr(0, colon);
        portText = text.substr(colon + 1);
    }

    uint16_t port = 0;
    if (host.empty() || !ParseUInt16(portText, &port))
        return false;

    endpoint->host = std::move(host);
    endpoint->port = port;
    return true;
}

static bool ReadTextFile(const fs::path& path, std::string* text)
{
    if (!text)
        return false;

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;

    text->assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

static bool WriteTextFile(const fs::path& path, const std::string& text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        return false;

    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return stream.good();
}

class RelayLogger
{
public:
    bool Start(fs::path path)
    {
        std::lock_guard lock(m_mutex);

        m_stream.open(path, std::ios::out | std::ios::trunc);
        if (!m_stream)
            return false;

        m_path = std::move(path);
        return true;
    }

    void Stop()
    {
        std::lock_guard lock(m_mutex);

        if (m_stream.is_open()) {
            m_stream.flush();
            m_stream.close();
        }
    }

    const fs::path& Path() const
    {
        return m_path;
    }

    void LogEvent(uint64_t connectionId, const std::string& text)
    {
        std::ostringstream line;
        line << FormatPrefix(connectionId) << text << "\n";
        WriteLocked(line.str());
    }

    void LogChunk(uint64_t connectionId, const char* direction, const uint8_t* data, size_t size)
    {
        if (!data || size == 0)
            return;

        std::ostringstream entry;
        entry << FormatPrefix(connectionId) << direction << " " << size << " bytes\n";

        for (size_t offset = 0; offset < size; offset += 16) {
            entry << "  " << std::setw(6) << std::setfill('0') << std::hex << offset << "  ";

            for (size_t i = 0; i < 16; ++i) {
                if (offset + i < size) {
                    entry << std::setw(2) << std::setfill('0') << std::hex
                          << static_cast<unsigned>(data[offset + i]) << ' ';
                } else {
                    entry << "   ";
                }
            }

            entry << " ";
            for (size_t i = 0; i < 16 && offset + i < size; ++i) {
                const uint8_t byte = data[offset + i];
                entry << (byte >= 32 && byte <= 126 ? static_cast<char>(byte) : '.');
            }
            entry << "\n";
        }

        entry << "\n";
        WriteLocked(entry.str());
    }

private:
    static std::string FormatPrefix(uint64_t connectionId)
    {
        SYSTEMTIME time{};
        GetLocalTime(&time);

        std::ostringstream prefix;
        prefix << "["
               << std::setw(4) << std::setfill('0') << time.wYear << "-"
               << std::setw(2) << std::setfill('0') << time.wMonth << "-"
               << std::setw(2) << std::setfill('0') << time.wDay << " "
               << std::setw(2) << std::setfill('0') << time.wHour << ":"
               << std::setw(2) << std::setfill('0') << time.wMinute << ":"
               << std::setw(2) << std::setfill('0') << time.wSecond << "."
               << std::setw(3) << std::setfill('0') << time.wMilliseconds << "]";

        if (connectionId != 0)
            prefix << " [conn " << connectionId << "]";

        prefix << " ";
        return prefix.str();
    }

    void WriteLocked(const std::string& text)
    {
        std::lock_guard lock(m_mutex);
        if (!m_stream.is_open())
            return;

        m_stream << text;
        m_stream.flush();
    }

    fs::path m_path;
    std::ofstream m_stream;
    mutable std::mutex m_mutex;
};

class ServerConfigPatch
{
public:
    explicit ServerConfigPatch(fs::path path)
        : m_path(std::move(path))
    {
    }

    bool Load()
    {
        std::lock_guard lock(m_mutex);

        if (!ReadTextFile(m_path, &m_originalText)) {
            printf("[!] Failed to read %s\n", m_path.string().c_str());
            return false;
        }

        json root = json::parse(m_originalText, nullptr, true, true);
        if (!root.is_array()) {
            printf("[!] %s has an unexpected format\n", m_path.string().c_str());
            return false;
        }

        m_targets.clear();
        for (const auto& group : root) {
            if (!group.is_object() || !group.contains("servers") || !group["servers"].is_array())
                continue;

            for (const auto& server : group["servers"]) {
                if (!server.is_object())
                    continue;

                std::string address = server.value("address", "");
                int port = server.value("port", 0);
                if (address.empty() || port <= 0 || port > 65535)
                    continue;

                Endpoint endpoint{address, static_cast<uint16_t>(port)};
                auto it = std::find_if(m_targets.begin(), m_targets.end(), [&](const Endpoint& existing) {
                    return existing.host == endpoint.host && existing.port == endpoint.port;
                });
                if (it == m_targets.end())
                    m_targets.push_back(std::move(endpoint));
            }
        }

        if (m_targets.empty()) {
            printf("[!] No login servers were found in %s\n", m_path.string().c_str());
            return false;
        }

        m_loaded = true;
        return true;
    }

    bool Apply(const std::string& listenHost, uint16_t listenPort)
    {
        std::lock_guard lock(m_mutex);

        if (!m_loaded) {
            printf("[!] Server config patch was not loaded before Apply().\n");
            return false;
        }

        json root = json::parse(m_originalText, nullptr, true, true);
        for (auto& group : root) {
            if (!group.is_object() || !group.contains("servers") || !group["servers"].is_array())
                continue;

            for (auto& server : group["servers"]) {
                if (!server.is_object())
                    continue;
                server["address"] = listenHost;
                server["port"] = listenPort;
            }
        }

        if (!WriteTextFile(m_path, root.dump(2))) {
            printf("[!] Failed to write patched %s\n", m_path.string().c_str());
            return false;
        }

        m_applied = true;
        return true;
    }

    void Restore()
    {
        std::lock_guard lock(m_mutex);

        if (!m_applied)
            return;

        if (!WriteTextFile(m_path, m_originalText)) {
            printf("[!] Failed to restore %s\n", m_path.string().c_str());
            return;
        }

        printf("[+] Restored %s\n", m_path.string().c_str());
        m_applied = false;
    }

    const std::vector<Endpoint>& Targets() const
    {
        return m_targets;
    }

private:
    fs::path m_path;
    std::string m_originalText;
    std::vector<Endpoint> m_targets;
    bool m_loaded = false;
    bool m_applied = false;
    mutable std::mutex m_mutex;
};

class WinsockSession
{
public:
    bool Start()
    {
        WSADATA data{};
        int err = WSAStartup(MAKEWORD(2, 2), &data);
        if (err != 0) {
            printf("[!] WSAStartup failed (%d)\n", err);
            return false;
        }

        m_started = true;
        return true;
    }

    ~WinsockSession()
    {
        if (m_started)
            WSACleanup();
    }

private:
    bool m_started = false;
};

static SOCKET GetSocketValue(const ManagedSocketPtr& socket)
{
    if (!socket)
        return INVALID_SOCKET;

    std::lock_guard lock(socket->m_mutex);
    return socket->m_socket;
}

static void CloseManagedSocket(const ManagedSocketPtr& socket)
{
    if (!socket)
        return;

    SOCKET raw = INVALID_SOCKET;
    {
        std::lock_guard lock(socket->m_mutex);
        raw = std::exchange(socket->m_socket, INVALID_SOCKET);
    }

    if (raw != INVALID_SOCKET) {
        shutdown(raw, SD_BOTH);
        closesocket(raw);
    }
}

static void ShutdownSend(const ManagedSocketPtr& socket)
{
    SOCKET raw = GetSocketValue(socket);
    if (raw != INVALID_SOCKET)
        shutdown(raw, SD_SEND);
}

static bool SendAll(SOCKET socket, const uint8_t* data, size_t size, const char* stage)
{
    size_t sent = 0;
    while (sent < size) {
        int rc = send(socket, reinterpret_cast<const char*>(data + sent), static_cast<int>(size - sent), 0);
        if (rc == SOCKET_ERROR) {
            printf("[proxy] send failed during %s (0x%08X)\n", stage, WSAGetLastError());
            return false;
        }
        if (rc == 0) {
            printf("[proxy] send returned 0 during %s\n", stage);
            return false;
        }
        sent += static_cast<size_t>(rc);
    }
    return true;
}

static bool RecvExact(SOCKET socket, uint8_t* data, size_t size, const char* stage)
{
    size_t received = 0;
    while (received < size) {
        int rc = recv(socket, reinterpret_cast<char*>(data + received), static_cast<int>(size - received), 0);
        if (rc == SOCKET_ERROR) {
            printf("[proxy] recv failed during %s (0x%08X)\n", stage, WSAGetLastError());
            return false;
        }
        if (rc == 0) {
            printf("[proxy] connection closed during %s\n", stage);
            return false;
        }
        received += static_cast<size_t>(rc);
    }
    return true;
}

static const char* SocksReplyText(uint8_t reply)
{
    switch (reply) {
    case 0x00: return "succeeded";
    case 0x01: return "general failure";
    case 0x02: return "connection not allowed";
    case 0x03: return "network unreachable";
    case 0x04: return "host unreachable";
    case 0x05: return "connection refused";
    case 0x06: return "TTL expired";
    case 0x07: return "command not supported";
    case 0x08: return "address type not supported";
    default: return "unknown error";
    }
}

static bool ConnectTcp(const Endpoint& endpoint, SOCKET* outSocket)
{
    if (!outSocket)
        return false;

    *outSocket = INVALID_SOCKET;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    std::string portText = std::to_string(endpoint.port);
    int rc = getaddrinfo(endpoint.host.c_str(), portText.c_str(), &hints, &result);
    if (rc != 0) {
        printf("[proxy] getaddrinfo failed for %s (%d)\n", EndpointToString(endpoint).c_str(), rc);
        return false;
    }

    SOCKET connected = INVALID_SOCKET;
    int lastError = 0;

    for (addrinfo* it = result; it; it = it->ai_next) {
        SOCKET socket = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (socket == INVALID_SOCKET) {
            lastError = WSAGetLastError();
            continue;
        }

        if (connect(socket, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
            connected = socket;
            break;
        }

        lastError = WSAGetLastError();
        closesocket(socket);
    }

    freeaddrinfo(result);

    if (connected == INVALID_SOCKET) {
        printf("[proxy] connect failed for %s (0x%08X)\n", EndpointToString(endpoint).c_str(), lastError);
        return false;
    }

    *outSocket = connected;
    return true;
}

static bool PerformSocks5Handshake(SOCKET proxySocket, const Endpoint& target,
                                   const std::string& username, const std::string& password)
{
    std::vector<uint8_t> greeting;
    greeting.push_back(0x05);
    if (!username.empty() || !password.empty()) {
        greeting.push_back(0x02);
        greeting.push_back(0x00);
        greeting.push_back(0x02);
    } else {
        greeting.push_back(0x01);
        greeting.push_back(0x00);
    }

    if (!SendAll(proxySocket, greeting.data(), greeting.size(), "SOCKS5 greeting"))
        return false;

    uint8_t methodReply[2]{};
    if (!RecvExact(proxySocket, methodReply, sizeof(methodReply), "SOCKS5 method reply"))
        return false;

    if (methodReply[0] != 0x05) {
        printf("[proxy] Unexpected SOCKS version in method reply: 0x%02X\n", methodReply[0]);
        return false;
    }

    if (methodReply[1] == 0xFF) {
        printf("[proxy] SOCKS proxy rejected all auth methods\n");
        return false;
    }

    if (methodReply[1] == 0x02) {
        if (username.size() > 255 || password.size() > 255) {
            printf("[proxy] Proxy username/password must be 255 bytes or shorter\n");
            return false;
        }

        std::vector<uint8_t> auth;
        auth.push_back(0x01);
        auth.push_back(static_cast<uint8_t>(username.size()));
        auth.insert(auth.end(), username.begin(), username.end());
        auth.push_back(static_cast<uint8_t>(password.size()));
        auth.insert(auth.end(), password.begin(), password.end());

        if (!SendAll(proxySocket, auth.data(), auth.size(), "SOCKS5 auth request"))
            return false;

        uint8_t authReply[2]{};
        if (!RecvExact(proxySocket, authReply, sizeof(authReply), "SOCKS5 auth reply"))
            return false;

        if (authReply[1] != 0x00) {
            printf("[proxy] SOCKS proxy rejected the supplied credentials\n");
            return false;
        }
    } else if (methodReply[1] != 0x00) {
        printf("[proxy] SOCKS proxy selected unsupported auth method 0x%02X\n", methodReply[1]);
        return false;
    }

    std::vector<uint8_t> request;
    request.push_back(0x05);
    request.push_back(0x01);
    request.push_back(0x00);

    IN_ADDR ipv4{};
    IN6_ADDR ipv6{};
    if (InetPtonA(AF_INET, target.host.c_str(), &ipv4) == 1) {
        request.push_back(0x01);
        auto* bytes = reinterpret_cast<const uint8_t*>(&ipv4);
        request.insert(request.end(), bytes, bytes + sizeof(ipv4));
    } else if (InetPtonA(AF_INET6, target.host.c_str(), &ipv6) == 1) {
        request.push_back(0x04);
        auto* bytes = reinterpret_cast<const uint8_t*>(&ipv6);
        request.insert(request.end(), bytes, bytes + sizeof(ipv6));
    } else {
        if (target.host.size() > 255) {
            printf("[proxy] Target host is too long for SOCKS5 domain encoding\n");
            return false;
        }

        request.push_back(0x03);
        request.push_back(static_cast<uint8_t>(target.host.size()));
        request.insert(request.end(), target.host.begin(), target.host.end());
    }

    request.push_back(static_cast<uint8_t>((target.port >> 8) & 0xFF));
    request.push_back(static_cast<uint8_t>(target.port & 0xFF));

    if (!SendAll(proxySocket, request.data(), request.size(), "SOCKS5 connect request"))
        return false;

    uint8_t replyHead[4]{};
    if (!RecvExact(proxySocket, replyHead, sizeof(replyHead), "SOCKS5 connect reply"))
        return false;

    if (replyHead[0] != 0x05) {
        printf("[proxy] Unexpected SOCKS version in connect reply: 0x%02X\n", replyHead[0]);
        return false;
    }

    if (replyHead[1] != 0x00) {
        printf("[proxy] SOCKS connect failed: %s (0x%02X)\n", SocksReplyText(replyHead[1]), replyHead[1]);
        return false;
    }

    size_t addressBytes = 0;
    if (replyHead[3] == 0x01) {
        addressBytes = 4;
    } else if (replyHead[3] == 0x04) {
        addressBytes = 16;
    } else if (replyHead[3] == 0x03) {
        uint8_t domainLength = 0;
        if (!RecvExact(proxySocket, &domainLength, sizeof(domainLength), "SOCKS5 bound domain length"))
            return false;
        addressBytes = domainLength;
    } else {
        printf("[proxy] SOCKS connect reply used unsupported address type 0x%02X\n", replyHead[3]);
        return false;
    }

    std::vector<uint8_t> trailing(addressBytes + 2);
    if (!RecvExact(proxySocket, trailing.data(), trailing.size(), "SOCKS5 bound address"))
        return false;

    return true;
}

static bool TestSocks5Proxy(const Endpoint& proxy, const Endpoint& target,
                            const std::string& username, const std::string& password)
{
    printf("[proxy] Testing SOCKS5 tunnel: %s -> %s\n",
           EndpointToString(proxy).c_str(), EndpointToString(target).c_str());

    SOCKET testSocket = INVALID_SOCKET;
    if (!ConnectTcp(proxy, &testSocket))
        return false;

    DWORD ioTimeoutMs = 10000;
    setsockopt(testSocket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&ioTimeoutMs), sizeof(ioTimeoutMs));
    setsockopt(testSocket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&ioTimeoutMs), sizeof(ioTimeoutMs));

    bool ok = PerformSocks5Handshake(testSocket, target, username, password);
    closesocket(testSocket);

    if (ok)
        printf("[proxy] SOCKS5 pre-launch test succeeded.\n");
    else
        printf("[proxy] SOCKS5 pre-launch test failed.\n");

    return ok;
}

class Socks5Relay
{
public:
    bool Start(const Endpoint& listen, const Endpoint& proxy, const Endpoint& target,
               std::string proxyUser, std::string proxyPassword, RelayLogger* logger)
    {
        m_listen = listen;
        m_proxy = proxy;
        m_target = target;
        m_proxyUser = std::move(proxyUser);
        m_proxyPassword = std::move(proxyPassword);
        m_logger = logger;

        if (!m_failClosedEvent) {
            m_failClosedEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
            if (!m_failClosedEvent) {
                printf("[proxy] Failed to create kill-switch event (0x%08lX)\n", GetLastError());
                return false;
            }
        }

        m_listenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listenSocket == INVALID_SOCKET) {
            printf("[proxy] Failed to create listen socket (0x%08X)\n", WSAGetLastError());
            return false;
        }

        BOOL exclusive = TRUE;
        setsockopt(m_listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(m_listen.port);
        if (InetPtonA(AF_INET, m_listen.host.c_str(), &address.sin_addr) != 1) {
            printf("[proxy] Invalid listen host: %s\n", m_listen.host.c_str());
            closesocket(m_listenSocket);
            m_listenSocket = INVALID_SOCKET;
            return false;
        }

        if (bind(m_listenSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            int bindErr = WSAGetLastError();
            if (bindErr != WSAEADDRINUSE && bindErr != WSAEACCES) {
                printf("[proxy] bind failed for %s (0x%08X)\n", EndpointToString(m_listen).c_str(), bindErr);
                closesocket(m_listenSocket);
                m_listenSocket = INVALID_SOCKET;
                return false;
            }

            printf("[proxy] Port %d in use, trying dynamic port...\n", m_listen.port);
            closesocket(m_listenSocket);

            m_listenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (m_listenSocket == INVALID_SOCKET) {
                printf("[proxy] Failed to create retry socket (0x%08X)\n", WSAGetLastError());
                return false;
            }

            setsockopt(m_listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

            address.sin_port = htons(0);
            if (bind(m_listenSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
                printf("[proxy] bind failed on dynamic port (0x%08X)\n", WSAGetLastError());
                closesocket(m_listenSocket);
                m_listenSocket = INVALID_SOCKET;
                return false;
            }
        }

        sockaddr_in bound{};
        int boundLen = sizeof(bound);
        if (getsockname(m_listenSocket, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
            m_listen.port = ntohs(bound.sin_port);
        }

        if (::listen(m_listenSocket, SOMAXCONN) != 0) {
            printf("[proxy] listen failed for %s (0x%08X)\n", EndpointToString(m_listen).c_str(), WSAGetLastError());
            closesocket(m_listenSocket);
            m_listenSocket = INVALID_SOCKET;
            return false;
        }

        m_running = true;
        m_acceptThread = std::thread([this]() { AcceptLoop(); });

        printf("[proxy] Relay listening on %s -> %s via SOCKS5 %s\n",
               EndpointToString(m_listen).c_str(),
               EndpointToString(m_target).c_str(),
               EndpointToString(m_proxy).c_str());
        if (m_logger)
            m_logger->LogEvent(0, "Relay listening on " + EndpointToString(m_listen) +
                                      " -> " + EndpointToString(m_target) +
                                      " via SOCKS5 " + EndpointToString(m_proxy));
        return true;
    }

    void Stop()
    {
        if (!m_running.exchange(false))
            return;

        if (m_listenSocket != INVALID_SOCKET) {
            closesocket(m_listenSocket);
            m_listenSocket = INVALID_SOCKET;
        }

        if (m_acceptThread.joinable())
            m_acceptThread.join();

        std::vector<ManagedSocketPtr> sockets;
        std::vector<std::thread> threads;
        {
            std::lock_guard lock(m_stateMutex);
            sockets = m_activeSockets;
            threads.swap(m_sessionThreads);
        }

        for (const auto& socket : sockets)
            CloseManagedSocket(socket);

        for (auto& thread : threads) {
            if (thread.joinable())
                thread.join();
        }

        {
            std::lock_guard lock(m_stateMutex);
            m_activeSockets.clear();
        }

        printf("[proxy] Relay stopped\n");
        if (m_logger)
            m_logger->LogEvent(0, "Relay stopped");
    }

    ~Socks5Relay()
    {
        Stop();
        if (m_failClosedEvent) {
            CloseHandle(m_failClosedEvent);
            m_failClosedEvent = nullptr;
        }
    }

    uint16_t GetListenPort() const { return m_listen.port; }
    bool IsFailClosedTriggered() const { return m_failClosedTriggered.load(); }
    HANDLE GetFailClosedEvent() const { return m_failClosedEvent; }

private:
    void TriggerFailClosed(uint64_t connectionId, const char* reason)
    {
        if (!m_running)
            return;

        bool wasAlreadyTriggered = m_failClosedTriggered.exchange(true);
        printf("[proxy] KILL-SWITCH: %s\n", reason);
        if (m_logger)
            m_logger->LogEvent(connectionId, std::string("KILL-SWITCH: ") + reason);
        if (!wasAlreadyTriggered && m_failClosedEvent)
            SetEvent(m_failClosedEvent);
    }

    void AcceptLoop()
    {
        while (m_running) {
            SOCKET client = accept(m_listenSocket, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (!m_running)
                    break;

                int err = WSAGetLastError();
                if (err == WSAENOTSOCK || err == WSAEINVAL)
                    break;

                printf("[proxy] accept failed (0x%08X)\n", err);
                continue;
            }

            std::lock_guard lock(m_stateMutex);
            m_sessionThreads.emplace_back([this, client]() { HandleClient(client); });
        }
    }

    void RegisterSocket(const ManagedSocketPtr& socket)
    {
        std::lock_guard lock(m_stateMutex);
        m_activeSockets.push_back(socket);
    }

    void UnregisterSocket(const ManagedSocketPtr& socket)
    {
        std::lock_guard lock(m_stateMutex);
        std::erase(m_activeSockets, socket);
    }

    static void PumpTraffic(const ManagedSocketPtr& source, const ManagedSocketPtr& destination,
                            RelayLogger* logger, uint64_t connectionId, const char* direction)
    {
        char buffer[8192];
        for (;;) {
            SOCKET sourceSocket = GetSocketValue(source);
            SOCKET destinationSocket = GetSocketValue(destination);
            if (sourceSocket == INVALID_SOCKET || destinationSocket == INVALID_SOCKET)
                break;

            int received = recv(sourceSocket, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (received <= 0)
                break;

            if (logger) {
                logger->LogChunk(connectionId, direction,
                                 reinterpret_cast<const uint8_t*>(buffer),
                                 static_cast<size_t>(received));
            }

            int sent = 0;
            while (sent < received) {
                int rc = send(destinationSocket, buffer + sent, received - sent, 0);
                if (rc <= 0)
                    goto done;
                sent += rc;
            }
        }

    done:
        ShutdownSend(destination);
    }

    void HandleClient(SOCKET clientSocket)
    {
        const uint64_t connectionId = m_nextConnectionId.fetch_add(1) + 1;
        ManagedSocketPtr client = std::make_shared<ManagedSocket>(clientSocket);
        RegisterSocket(client);
        if (m_logger)
            m_logger->LogEvent(connectionId, "Accepted client connection");

        SOCKET upstreamSocket = INVALID_SOCKET;
        if (!ConnectTcp(m_proxy, &upstreamSocket)) {
            if (m_logger)
                m_logger->LogEvent(connectionId, "Failed to connect to SOCKS5 proxy");
            TriggerFailClosed(connectionId, "Failed to connect to SOCKS5 proxy");
            CloseManagedSocket(client);
            UnregisterSocket(client);
            return;
        }

        ManagedSocketPtr upstream = std::make_shared<ManagedSocket>(upstreamSocket);
        RegisterSocket(upstream);

        if (!PerformSocks5Handshake(upstreamSocket, m_target, m_proxyUser, m_proxyPassword)) {
            if (m_logger)
                m_logger->LogEvent(connectionId, "SOCKS5 handshake failed");
            TriggerFailClosed(connectionId, "SOCKS5 handshake failed");
            CloseManagedSocket(client);
            CloseManagedSocket(upstream);
            UnregisterSocket(client);
            UnregisterSocket(upstream);
            return;
        }

        if (m_logger)
            m_logger->LogEvent(connectionId, "SOCKS5 tunnel established");
        m_establishedTunnel.store(true);

        std::thread forward(PumpTraffic, client, upstream, m_logger, connectionId, "client->target");
        std::thread backward(PumpTraffic, upstream, client, nullptr, connectionId, "target->client");

        forward.join();
        backward.join();

        if (m_logger)
            m_logger->LogEvent(connectionId, "Connection closed");
        if (m_establishedTunnel.load())
            TriggerFailClosed(connectionId, "Proxied game connection closed");

        CloseManagedSocket(client);
        CloseManagedSocket(upstream);
        UnregisterSocket(client);
        UnregisterSocket(upstream);
    }

    Endpoint m_listen;
    Endpoint m_proxy;
    Endpoint m_target;
    std::string m_proxyUser;
    std::string m_proxyPassword;

    SOCKET m_listenSocket = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_establishedTunnel{false};
    std::atomic<bool> m_failClosedTriggered{false};
    std::atomic<uint64_t> m_nextConnectionId{0};
    std::thread m_acceptThread;
    RelayLogger* m_logger = nullptr;
    HANDLE m_failClosedEvent = nullptr;

    std::mutex m_stateMutex;
    std::vector<ManagedSocketPtr> m_activeSockets;
    std::vector<std::thread> m_sessionThreads;
};

// Guards the span from patching servers.json for a proxy-mode account
// through that account's game process having had a chance to read it
// (see RunAccountSupervisionLoop's use of this) — prevents two accounts'
// proxy launches from clobbering each other's rewrite of the same shared
// file mid-startup. Only ever taken by a session's own worker thread, and
// only for the FIRST launch of a proxy-mode session (relaunches never
// re-Apply() the patch, so there's nothing new to protect there).
static std::mutex g_serverConfigMutex;

static bool Inject(DWORD pid, const char* dllPath)
{
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) {
        printf("[!] OpenProcess failed (0x%08lX). Are you running as admin?\n", GetLastError());
        return false;
    }

    size_t pathLen = strlen(dllPath) + 1;
    void* remoteBuf = VirtualAllocEx(hProc, nullptr, pathLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteBuf) {
        printf("[!] VirtualAllocEx failed (0x%08lX)\n", GetLastError());
        CloseHandle(hProc);
        return false;
    }

    if (!WriteProcessMemory(hProc, remoteBuf, dllPath, pathLen, nullptr)) {
        printf("[!] WriteProcessMemory failed (0x%08lX)\n", GetLastError());
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    auto loadLibAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"));

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, loadLibAddr, remoteBuf, 0, nullptr);
    if (!hThread) {
        printf("[!] CreateRemoteThread failed (0x%08lX)\n", GetLastError());
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    WaitForSingleObject(hThread, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (exitCode == 0) {
        printf("[!] LoadLibraryA returned NULL - injection failed.\n");
        return false;
    }

    return true;
}

static std::optional<Endpoint> SelectTargetEndpoint(const ServerConfigPatch& patch, const LaunchOptions& options)
{
    if (options.m_targetOverride)
        return options.m_targetOverride;

    const auto& targets = patch.Targets();
    if (targets.size() != 1) {
        printf("[!] %s contains multiple unique login targets.\n", SERVER_CONFIG_NAME);
        printf("    Re-run with --target <host:port> to choose which upstream target the relay should use.\n");
        return std::nullopt;
    }

    return targets.front();
}

// Resolve the game install directory instead of assuming a fixed drive.
// Priority: COCLASSIC_GAME_DIR env var -> game_dir.txt next to launcher.exe ->
// legacy default (GAME_DIR). Lets the same launcher.exe work on any install path.
static std::string ResolveGameDir()
{
    char envBuf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("COCLASSIC_GAME_DIR", envBuf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        return std::string(envBuf, n);

    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
        fs::path cfg = fs::path(exePath).parent_path() / "game_dir.txt";
        std::ifstream f(cfg);
        std::string line;
        if (f && std::getline(f, line)) {
            while (!line.empty() &&
                   (line.back() == '\r' || line.back() == '\n' ||
                    line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (!line.empty())
                return line;
        }
    }

    return GAME_DIR;
}

// Everything RunAccountSupervisionLoop() needs beyond the AccountSession
// itself — resolved once by main() before the (single, for now) session is
// launched. Bundled rather than passed as a dozen separate parameters.
struct SupervisionParams
{
    LaunchOptions options;
    bool haveProfile = false;
    std::string gamePathStr;
    std::string gameDir;
    std::string dllStr;
    std::string exePath;
    bool proxyMode = false;
    ServerConfigPatch* serverPatch = nullptr;  // non-null only when proxyMode
    Socks5Relay* relay = nullptr;              // non-null only when proxyMode
    RelayLogger* activeLogger = nullptr;       // non-null only when proxyMode && packet logging is on
};

// Sends auto-login keystrokes for `pid`, then waits (bounded) for
// confirmation that login actually completed, before returning. Two
// different confirmation signals depending on which call site this is:
//
// - Initial post-launch login (isReconnect=false): waits for the DLL's
//   ground-truth "hero pointer valid" marker (dllmain.cpp's
//   WriteLoggedInMarker()) — live-confirmed fast and reliable (3-5s).
// - Reconnect after a detected disconnect (isReconnect=true): waits for
//   AutoLogin::IsAtLoginScreen(pid) to become false instead. Live-tested:
//   the marker approach does NOT work here — a mere in-game "Disconnect"
//   almost certainly never actually invalidates the hero pointer
//   client-side (it just drops the connection and shows the login
//   overlay on top of the still-alive hero object), so
//   ConnectionMonitorThread's invalid->valid edge detection never fires
//   on reconnect, and the wait always ran the full 60s timeout before
//   falling back. The login window closing is a signal this file already
//   trusts elsewhere (IsAtLoginScreen IS the disconnect detector one
//   level up in RunAccountSupervisionLoop) and needs no DLL cooperation.
//
// Returns whether confirmation was actually seen, so callers can bound
// retries instead of treating every attempt as having silently succeeded.
static bool PerformLoginAndWaitForConfirmation(AccountSession* session, const SupervisionParams& params, DWORD pid,
                                                bool isReconnect)
{
    AutoLoginRequest loginReq{session->profile.username, session->profile.password, session->profile.server};
    // PerformLogin's own wait is shortened to 8s since the confirmation
    // wait below is the real, authoritative detector -- this call's
    // return value is only used for a diagnostic printf, not gated on.
    if (!AutoLogin::PerformLogin(loginReq, pid, 8000, isReconnect))
        printf("[!] Auto-login window-close check timed out (informational only, not a failure by itself) — pid=%lu\n", pid);

    const DWORD waitStart = GetTickCount();
    bool confirmed = false;

    if (isReconnect) {
        constexpr DWORD kReconnectConfirmTimeoutMs = 30000;
        printf("[*] Waiting for the login window to close (reconnect confirmation)...\n");
        while (GetTickCount() - waitStart < kReconnectConfirmTimeoutMs) {
            if (!AutoLogin::IsAtLoginScreen(pid)) {
                confirmed = true;
                break;
            }
            if (session->stopEvent && WaitForSingleObject(session->stopEvent, 0) == WAIT_OBJECT_0) {
                printf("[*] Reconnect-confirmation wait interrupted by stop request.\n");
                break;
            }
            Sleep(500);
        }
    } else {
        const fs::path loggedInMarker = fs::path(params.exePath).parent_path() /
            ("logged_in_" + std::to_string(pid) + ".flag");
        printf("[*] Waiting for login-confirmed marker: %s\n", loggedInMarker.string().c_str());
        while (GetTickCount() - waitStart < 60000) {
            if (fs::exists(loggedInMarker)) {
                fs::remove(loggedInMarker);
                confirmed = true;
                break;
            }
            if (session->stopEvent && WaitForSingleObject(session->stopEvent, 0) == WAIT_OBJECT_0) {
                printf("[*] Login-marker wait interrupted by stop request.\n");
                break;
            }
            Sleep(500);
        }
    }

    printf(confirmed ? "[+] Login confirmed after %lums.\n"
                      : "[!] Login NOT confirmed within timeout (%lums) — proceeding anyway.\n",
        GetTickCount() - waitStart);
    return confirmed;
}

// The extracted, per-session version of what used to be main()'s own
// for(;;) loop: launch -> inject -> auto-login -> supervise/relanuch,
// writing all mutable state to `session` instead of main()-local variables
// or process-global statics. Returns the same 0/1 main() used to return
// directly (0 = injection succeeded at least once, 1 = a hard failure).
//
// Multi-account manager support, Phase 2: still called exactly once,
// synchronously, from main() below — this only proves the extraction is
// behavior-preserving before Phase 3 wraps it in a worker thread per
// account. See ~/.claude/plans/zazzy-wondering-diffie.md.
// configLock: held by the CALLER (the same worker thread this function
// runs on — never a different thread, std::mutex requires that) across
// the proxy servers.json patch/apply that already happened before this
// call, for a proxy-mode FIRST launch. Released here, right after the
// first CreateProcessA's settle-wait (WaitForInputIdle+Sleep) — the exact
// point the game process has had a chance to read the file — so it's a
// no-op (owns_lock() false) for every non-proxy session and for every
// relaunch after the first. See g_serverConfigMutex's own comment.
static int RunAccountSupervisionLoop(AccountSession* session, const SupervisionParams& params,
                                      std::unique_lock<std::mutex>& configLock)
{
    bool injectionSucceeded = false;

    // Session 12 [LOCKUP FIX]: bounds how many times in a row the game can
    // exit almost immediately after launch before supervision gives up
    // instead of relaunching forever. A crash after a real play session
    // (long uptime) never counts against this — it's specifically a normal,
    // rare recovery case this loop exists for.
    constexpr int kMaxFastCrashes = 3;
    constexpr DWORD kFastCrashThresholdMs = 30000;

    // Session 10: wraps the original single-shot launch+inject sequence in a
    // supervise-and-relaunch loop. Only actually loops when an account was
    // selected (params.haveProfile) — with no saved account this behaves
    // exactly like the original one-shot flow, so existing proxy-only /
    // no-account usage is unaffected.
    for (;;) {
        printf("[*] Launching fresh %s process...\n", GAME_EXE);
        session->state = SessionState::Launching;
        const DWORD launchTick = GetTickCount();

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        if (!CreateProcessA(params.gamePathStr.c_str(), nullptr, nullptr, nullptr, FALSE, 0,
                            nullptr, params.gameDir.c_str(), &si, &pi)) {
            printf("[!] CreateProcess failed (0x%08lX)\n", GetLastError());
            if (params.proxyMode) {
                params.relay->Stop();
                params.serverPatch->Restore();
                if (params.activeLogger)
                    params.activeLogger->Stop();
            }
            session->SetStatus("CreateProcess failed");
            session->state = SessionState::Failed;
            return 1;
        }

        const DWORD pid = pi.dwProcessId;
        session->gamePid = pid;
        printf("[+] Started %s (PID %lu)\n", GAME_EXE, pid);

        // Write the deferred resume_hunt marker (see AccountSession::
        // pendingResumeMarker) now that this relaunch's real PID exists.
        if (session->pendingResumeMarker) {
            std::ofstream marker(fs::path(params.exePath).parent_path() /
                ("resume_hunt_" + std::to_string(pid) + ".flag"));
            session->pendingResumeMarker = false;
        }

        DWORD waitIdle = WaitForInputIdle(pi.hProcess, 10000);
        if (waitIdle == WAIT_TIMEOUT) {
            printf("[*] WaitForInputIdle timed out, continuing with injection.\n");
        } else if (waitIdle == WAIT_FAILED) {
            printf("[*] WaitForInputIdle failed (0x%08lX), continuing with injection.\n", GetLastError());
        }

        Sleep(1000);

        // The game process has now had its chance to read servers.json —
        // safe to let another account's proxy launch patch it. See
        // g_serverConfigMutex's comment and this function's own header
        // comment for why this is a no-op past the first iteration.
        if (configLock.owns_lock())
            configLock.unlock();

        printf("[*] Injecting...\n");
        session->state = SessionState::Injecting;
        injectionSucceeded = Inject(pid, params.dllStr.c_str());
        if (injectionSucceeded) {
            printf("[+] Injection successful!\n");
        } else {
            printf("[!] Injection failed.\n");
            if (!params.proxyMode && !params.haveProfile) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                session->SetStatus("Injection failed");
                session->state = SessionState::Failed;
                return 1;
            }
            printf("[*] Keeping the game running despite the injection failure.\n");
        }

        // Auto-login drives ImConquer.exe's OWN login window from here, in
        // launcher.exe's own process — NOT from inside coclassic.dll, which
        // deliberately stays idle until login completes (see dllmain.cpp's
        // InitThread comment on the server's "virtual machine detected"
        // integrity check). A failed auto-login doesn't abort the run — the
        // game keeps running and the user can finish logging in by hand.
        if (params.haveProfile) {
            session->state = SessionState::LoggingIn;
            PerformLoginAndWaitForConfirmation(session, params, pid, /*isReconnect=*/false);
        }

        // Original single-shot proxy flow (no saved account): restore
        // servers.json immediately so another instance can be launched right
        // away, exactly as before. While actively supervising with a saved
        // account, the patch stays in place across relaunches instead —
        // we're the ones managing this session, so there's no "launch
        // another instance in the meantime" case to leave room for.
        if (params.proxyMode && !params.haveProfile) {
            params.serverPatch->Restore();
            printf("[+] %s restored. You can launch another instance now.\n", SERVER_CONFIG_NAME);
        }

        printf("[*] Supervising game process (PID %lu)...\n", pid);
        session->state = SessionState::Running;
        HANDLE waitHandles[3] = { pi.hProcess, session->stopEvent, nullptr };
        DWORD handleCount = 2;
        const bool killSwitchArmed = params.proxyMode && params.options.m_killSwitch && params.relay->GetFailClosedEvent();
        if (killSwitchArmed) {
            waitHandles[2] = params.relay->GetFailClosedEvent();
            handleCount = 3;
        }
        DWORD wait;
        if (params.haveProfile) {
            // Multi-account manager support: a network hiccup or manual
            // "Disconnect" in-game drops back to the login screen WITHOUT
            // the game process itself exiting -- nothing above (process
            // handle, stop event, kill-switch) reacts to that on its own,
            // so the account would just sit at the login screen forever
            // with no way back short of the user noticing and clicking
            // Login again from the manager (which would launch a SECOND,
            // wrong game process). Poll on a bounded interval instead of
            // waiting INFINITE, and treat "the login window reappeared
            // for this PID" as an in-place reconnect trigger, reusing the
            // exact same mechanism the initial launch used.
            constexpr DWORD kReconnectCheckIntervalMs = 5000;
            // Bounded, matching this project's existing "don't retry
            // forever" philosophy (see kMaxFastCrashes below) — a
            // reconnect attempt that keeps failing (live-reported: the
            // disconnect banner interfering, or a genuinely bad
            // credential/network state) shouldn't hammer the same
            // possibly-stuck login screen indefinitely.
            constexpr int kMaxConsecutiveFailedReconnects = 5;
            int consecutiveFailedReconnects = 0;
            for (;;) {
                wait = WaitForMultipleObjects(handleCount, waitHandles, FALSE, kReconnectCheckIntervalMs);
                if (wait != WAIT_TIMEOUT)
                    break;  // process exited, stop requested, or kill-switch fired

                if (AutoLogin::IsAtLoginScreen(pid)) {
                    printf("[*] Detected a disconnect (login screen reappeared) — reconnecting (attempt %d/%d)...\n",
                        consecutiveFailedReconnects + 1, kMaxConsecutiveFailedReconnects);
                    session->state = SessionState::LoggingIn;
                    session->SetStatus("Reconnecting...");
                    const bool reconnected = PerformLoginAndWaitForConfirmation(session, params, pid, /*isReconnect=*/true);
                    session->SetStatus("");
                    session->state = SessionState::Running;

                    if (reconnected) {
                        consecutiveFailedReconnects = 0;
                    } else if (++consecutiveFailedReconnects >= kMaxConsecutiveFailedReconnects) {
                        printf("[!] Reconnect failed %d times in a row — giving up and forcing a full relaunch "
                            "instead of retrying keystrokes against a possibly-stuck login screen.\n",
                            consecutiveFailedReconnects);
                        session->SetStatus("Reconnect failed repeatedly — relaunching");
                        TerminateProcess(pi.hProcess, 0);
                        break;  // falls through to the crash-relaunch path below, same as a real crash
                    }
                }
            }
        } else {
            wait = WaitForMultipleObjects(handleCount, waitHandles, FALSE, INFINITE);
        }
        const bool killSwitchFired = killSwitchArmed && wait == WAIT_OBJECT_0 + 2;
        const bool stoppedByUser = wait == WAIT_OBJECT_0 + 1;

        if (killSwitchFired) {
            printf("[proxy] KILL-SWITCH: terminating game process to avoid continuing after proxy failure.\n");
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 10000);
        }

        // Per-account "Exit" (as opposed to "Exit Manager") also closes the
        // game itself, not just supervision — see AccountSession::
        // killGameOnStop's own comment. Deliberately does NOT wait to
        // confirm the process has actually exited: live-reported, a
        // Themida-packed target mid-autohunt (heavy packet I/O, in-flight
        // SendInput) can take 30s-60s+ for TerminateProcess to actually
        // finish tearing the process down at the OS level -- ahead of this
        // fix, the manager's own WaitForSingleObject(..., 10000) after it
        // meant the UI/row stayed stuck showing the account as busy for
        // that whole span too. "Exit" is meant to be a hard stop that
        // takes priority over everything -- the launcher's own
        // bookkeeping (row flips to Login, thread reaped) now happens the
        // instant termination is REQUESTED, not once the OS confirms it,
        // exactly like the kill-switch path a few lines above already
        // implicitly assumes for its own book-keeping (it just doesn't
        // matter there since a kill-switch trip never relaunches).
        if (stoppedByUser && session->killGameOnStop.load()) {
            printf("[*] Exit requested — terminating game process.\n");
            TerminateProcess(pi.hProcess, 0);
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        if (stoppedByUser) {
            printf("[*] Stop requested — ending supervision.\n");
            session->state = SessionState::Idle;
            break;
        }
        if (killSwitchFired) {
            // Never auto-relaunch past a kill-switch trip: it exists
            // specifically to stop playing when the proxy connection is
            // broken, so relaunching through it would defeat the point.
            // (Its underlying event is also manual-reset and never cleared,
            // so looping back to wait on it again would fire immediately.)
            printf("[proxy] Kill-switch fired — ending supervision instead of relaunching.\n");
            session->state = SessionState::Failed;
            break;
        }
        if (!params.haveProfile) {
            if (!params.proxyMode)
                Sleep(1000);
            session->state = SessionState::Idle;
            break;
        }

        session->state = SessionState::Crashed;
        const DWORD uptimeMs = GetTickCount() - launchTick;
        if (uptimeMs < kFastCrashThresholdMs) {
            ++session->consecutiveFastCrashes;
            printf("[!] Game process exited after only %lums (fast crash %d/%d)\n",
                uptimeMs, session->consecutiveFastCrashes, kMaxFastCrashes);
            if (session->consecutiveFastCrashes >= kMaxFastCrashes) {
                printf("[!] Game crashed %d times in a row shortly after launch — giving up auto-relaunch "
                    "instead of retrying forever. Check the game/DLL for what's actually failing, then "
                    "run the launcher again manually.\n", session->consecutiveFastCrashes);
                session->state = SessionState::Failed;
                break;
            }
        } else {
            session->consecutiveFastCrashes = 0;
        }

        printf("[*] Game process exited unexpectedly — relaunching and logging back in automatically...\n");

        // Tells coclassic.dll's next init that this specific relaunch is a
        // crash-recovery resume, not an intentional fresh session — the only
        // case where previously-enabled plugins (autohunt, mining, etc.)
        // should come back on automatically without the user re-checking
        // anything. See dllmain.cpp's ConsumeResumeHuntMarker(). The actual
        // file write is deferred to the top of the next iteration, once the
        // new process's PID is known (see AccountSession::pendingResumeMarker).
        session->pendingResumeMarker = true;

        Sleep(2000);
    }

    if (params.proxyMode) {
        params.relay->Stop();
        if (params.activeLogger)
            params.activeLogger->Stop();
        params.serverPatch->Restore();  // no-op if the !haveProfile path above already restored it
    }

    return injectionSucceeded ? 0 : 1;
}

// =====================================================================
// Multi-account manager window (Phase 3).
//
// Replaces the old one-shot console flow (CLI args, modal account-picker
// dialog, modal SOCKS5 dialog) with a single persistent GUI window that
// can run several accounts at once, each on its own worker thread driving
// RunAccountSupervisionLoop(). See account_session.h and
// ~/.claude/plans/zazzy-wondering-diffie.md for the full design.
// =====================================================================

namespace {

constexpr UINT kWmTrayIcon = WM_APP + 1;
constexpr UINT_PTR kRowRefreshTimerId = 1;
constexpr UINT_PTR kExitPollTimerId = 2;
constexpr UINT kRowRefreshIntervalMs = 500;
constexpr UINT kExitPollIntervalMs = 200;

constexpr int kIdAddAccount = 4001;
constexpr int kIdTrayShow = 4002;
constexpr int kIdTrayExit = 4003;
// Per-row controls: kId*Base + row index. 1000 apart so a reasonable
// number of saved accounts can never make one row's ID range collide
// with the next control-kind's base.
constexpr int kIdRowActionBase = 5000;
constexpr int kIdRowRemoveBase = 6000;
constexpr int kIdRowStatusBase = 7000;

constexpr int kRowHeight = 36;
constexpr int kRowTopMargin = 10;
constexpr int kWindowClientWidth = 560;
constexpr int kBottomBarHeight = 46;

std::vector<std::unique_ptr<AccountSession>> g_sessions;  // UI-thread-only, see account_session.h
std::vector<HWND> g_rowChildWindows;
NOTIFYICONDATAA g_trayIcon{};
bool g_exiting = false;

// "Busy" = there's a worker thread that hasn't been reaped yet — covers
// every state from Launching through Crashed (mid-relaunch). A session
// that was added but never logged in, or one that already finished and
// was reaped, is not busy.
bool IsSessionBusy(const AccountSession& session)
{
    return session.worker.joinable();
}

const char* StateLabel(SessionState state)
{
    switch (state) {
    case SessionState::Idle: return "Idle";
    case SessionState::Launching: return "Launching...";
    case SessionState::Injecting: return "Injecting...";
    case SessionState::LoggingIn: return "Logging in...";
    case SessionState::Running: return "Running";
    case SessionState::Stopping: return "Stopping...";
    case SessionState::Crashed: return "Relaunching...";
    case SessionState::Failed: return "Failed";
    }
    return "?";
}

// Joins a worker thread that has already finished (session->finished),
// so the row's button can flip back to "Login" without the UI thread
// ever blocking on join() itself. Safe to call every timer tick — a
// still-running session is left untouched.
void ReapFinishedSession(AccountSession& session)
{
    if (session.worker.joinable() && session.finished.load()) {
        session.worker.join();
        session.killGameOnStop = false;
    }
}

void SaveSessionsToCredentials()
{
    std::vector<AccountProfile> profiles;
    profiles.reserve(g_sessions.size());
    for (auto& s : g_sessions)
        profiles.push_back(s->profile);
    Credentials::SaveAll(profiles);
}

void DestroyRowControls()
{
    for (HWND h : g_rowChildWindows)
        if (h && IsWindow(h))
            DestroyWindow(h);
    g_rowChildWindows.clear();
}

// Tears down and recreates every per-account row's child controls, then
// resizes the window to fit and repositions the fixed "Add Account"
// button beneath them. Only called when the account LIST changes (add/
// remove) — per-tick status/button-caption updates go through
// RefreshRows() instead, which mutates the existing controls in place.
void RebuildRows(HWND hwnd)
{
    DestroyRowControls();

    HINSTANCE hInst = GetModuleHandleA(nullptr);
    int y = kRowTopMargin;
    for (size_t i = 0; i < g_sessions.size(); ++i) {
        AccountSession& session = *g_sessions[i];
        char labelText[300];
        snprintf(labelText, sizeof(labelText), "%s  (%s)",
            session.profile.label.c_str(), session.profile.username.c_str());

        HWND label = CreateWindowA("STATIC", labelText, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
            10, y + 8, 260, 20, hwnd, nullptr, hInst, nullptr);
        HWND status = CreateWindowA("STATIC", StateLabel(session.state.load()), WS_CHILD | WS_VISIBLE | SS_LEFT,
            280, y + 8, 140, 20, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRowStatusBase + static_cast<int>(i))), hInst, nullptr);
        const bool busy = IsSessionBusy(session);
        HWND actionBtn = CreateWindowA("BUTTON", busy ? "Exit" : "Login", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            430, y, 60, 28, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRowActionBase + static_cast<int>(i))), hInst, nullptr);
        HWND removeBtn = CreateWindowA("BUTTON", "Remove",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | (busy ? WS_DISABLED : 0), 495, y, 60, 28, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRowRemoveBase + static_cast<int>(i))), hInst, nullptr);

        g_rowChildWindows.push_back(label);
        g_rowChildWindows.push_back(status);
        g_rowChildWindows.push_back(actionBtn);
        g_rowChildWindows.push_back(removeBtn);
        y += kRowHeight;
    }

    if (g_sessions.empty()) {
        HWND empty = CreateWindowA("STATIC", "No saved accounts yet - click \"Add Account\" to get started.",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 10, y + 8, 540, 20, hwnd, nullptr, hInst, nullptr);
        g_rowChildWindows.push_back(empty);
        y += kRowHeight;
    }

    const int clientHeight = y + kBottomBarHeight;
    RECT clientRect{0, 0, kWindowClientWidth, clientHeight};
    constexpr DWORD kStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&clientRect, kStyle, FALSE);
    SetWindowPos(hwnd, nullptr, 0, 0,
        clientRect.right - clientRect.left, clientRect.bottom - clientRect.top,
        SWP_NOMOVE | SWP_NOZORDER);

    if (HWND addBtn = GetDlgItem(hwnd, kIdAddAccount))
        SetWindowPos(addBtn, nullptr, 10, y + 6, 140, 30, SWP_NOZORDER);
}

// Per-tick refresh: updates existing controls in place (no
// destroy/recreate — that's RebuildRows()'s job, only needed when the
// account list itself changes) and reaps any worker thread that finished
// since the last tick.
void RefreshRows(HWND hwnd)
{
    for (auto& s : g_sessions)
        ReapFinishedSession(*s);

    for (size_t i = 0; i < g_sessions.size(); ++i) {
        AccountSession& session = *g_sessions[i];
        HWND statusCtrl = GetDlgItem(hwnd, kIdRowStatusBase + static_cast<int>(i));
        HWND actionCtrl = GetDlgItem(hwnd, kIdRowActionBase + static_cast<int>(i));
        HWND removeCtrl = GetDlgItem(hwnd, kIdRowRemoveBase + static_cast<int>(i));
        if (!statusCtrl || !actionCtrl)
            continue;

        std::string statusText = session.GetStatus();
        if (statusText.empty())
            statusText = StateLabel(session.state.load());
        SetWindowTextA(statusCtrl, statusText.c_str());

        const bool busy = IsSessionBusy(session);
        SetWindowTextA(actionCtrl, busy ? "Exit" : "Login");
        if (removeCtrl)
            EnableWindow(removeCtrl, !busy);
    }
}

// Redirects stdio to a log file next to launcher.exe. The manager is a
// GUI-subsystem app with no console at all (that's the whole point —
// see the plan's "zero visible console windows" requirement), so the
// launch/inject/login/proxy diagnostics this file already prints via
// printf() would otherwise vanish into a detached stdout instead of
// being lost outright, this keeps them readable after the fact.
void RedirectStdioToLogFile()
{
    char exePathBuf[MAX_PATH];
    GetModuleFileNameA(nullptr, exePathBuf, MAX_PATH);
    const fs::path logPath = fs::path(exePathBuf).parent_path() / "launcher.log";
    FILE* f = nullptr;
    freopen_s(&f, logPath.string().c_str(), "a", stdout);
    freopen_s(&f, logPath.string().c_str(), "a", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
}

// The "Login" click handler. Resolves the DLL/game paths fresh (matches
// what main() used to do once at startup — now done per-launch, since
// accounts can be started/stopped/restarted repeatedly across the
// manager's lifetime), does synchronous proxy setup INSIDE the new
// worker thread if this account uses one (so the mutex critical section
// below never has to cross an OS thread boundary — see
// RunAccountSupervisionLoop's own comment on configLock), then spawns
// the worker thread that runs RunAccountSupervisionLoop for real.
void HandleLoginClick(AccountSession* session)
{
    if (IsSessionBusy(*session))
        return;  // shouldn't be reachable (button would read "Exit"), guard anyway

    if (!session->stopEvent)
        session->stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(session->stopEvent);
    session->killGameOnStop = false;
    session->finished = false;
    session->consecutiveFastCrashes = 0;
    session->pendingResumeMarker = false;
    session->state = SessionState::Launching;
    // Clears any error message left over from a previous failed run (e.g.
    // "Proxy setup failed") -- the live state (StateLabel(), read in
    // RefreshRows) already conveys "starting" via SessionState::Launching,
    // so there's no separate sticky message to set here. See RefreshRows'
    // own comment for why statusText is error-only, not a running commentary.
    session->SetStatus("");

    char exePathBuf[MAX_PATH];
    GetModuleFileNameA(nullptr, exePathBuf, MAX_PATH);
    const std::string exePath = exePathBuf;
    const fs::path dllPath = fs::path(exePath).parent_path() / DLL_NAME;

    if (!fs::exists(dllPath)) {
        session->SetStatus("coclassic.dll not found next to launcher.exe -- build it first");
        session->state = SessionState::Failed;
        return;
    }

    const std::string gameDir = ResolveGameDir();
    const std::string gamePathStr = (fs::path(gameDir) / "bin" / "64" / GAME_EXE).string();
    if (!fs::exists(gamePathStr)) {
        session->SetStatus("Game not found -- set COCLASSIC_GAME_DIR or game_dir.txt");
        session->state = SessionState::Failed;
        return;
    }

    SupervisionParams params;
    params.haveProfile = true;
    params.exePath = exePath;
    params.gamePathStr = gamePathStr;
    params.gameDir = gameDir;
    params.dllStr = dllPath.string();
    params.options.m_killSwitch = true;

    AccountProfile profileCopy = session->profile;

    session->worker = std::thread([session, params, profileCopy]() mutable {
        // Lives for this thread's whole lifetime — RunAccountSupervisionLoop
        // holds pointers into these for as long as proxy mode is active.
        ServerConfigPatch patch(fs::path(params.gameDir) / SERVER_CONFIG_NAME);
        Socks5Relay relay;
        std::unique_lock<std::mutex> configLock(g_serverConfigMutex, std::defer_lock);

        if (profileCopy.useProxy) {
            Endpoint proxyEndpoint;
            if (!ParseEndpoint(profileCopy.proxyHostPort, &proxyEndpoint)) {
                session->SetStatus("Saved proxy address invalid -- connecting directly");
            } else {
                configLock.lock();
                bool proxyOk = patch.Load();

                std::optional<Endpoint> target;
                if (proxyOk) {
                    LaunchOptions proxyOpts;
                    proxyOpts.m_proxy = proxyEndpoint;
                    target = SelectTargetEndpoint(patch, proxyOpts);
                    proxyOk = target.has_value();
                }
                if (proxyOk) {
                    proxyOk = TestSocks5Proxy(proxyEndpoint, *target,
                        profileCopy.proxyUser, profileCopy.proxyPassword);
                }

                Endpoint listen{LOCAL_RELAY_HOST, target ? target->port : static_cast<uint16_t>(0)};
                if (proxyOk) {
                    proxyOk = relay.Start(listen, proxyEndpoint, *target,
                        profileCopy.proxyUser, profileCopy.proxyPassword, nullptr);
                }
                if (proxyOk && !patch.Apply(listen.host, relay.GetListenPort())) {
                    relay.Stop();
                    proxyOk = false;
                }

                if (!proxyOk) {
                    configLock.unlock();
                    session->SetStatus("Proxy setup failed -- see launcher.log");
                    session->state = SessionState::Failed;
                    session->finished = true;
                    return;
                }

                params.proxyMode = true;
                params.serverPatch = &patch;
                params.relay = &relay;
            }
        }

        RunAccountSupervisionLoop(session, params, configLock);
        session->finished = true;
    });
}

// The "Exit" click handler — stops supervision AND closes the game
// (killGameOnStop=true). Distinct from the manager-wide "Exit Manager"
// path (BeginManagerExit below), which never kills games.
void HandleExitClick(AccountSession* session)
{
    if (!IsSessionBusy(*session))
        return;
    session->killGameOnStop = true;
    if (session->stopEvent)
        SetEvent(session->stopEvent);
    // Written directly rather than via SetStatus() -- StateLabel() already
    // maps this to "Stopping...", and RunAccountSupervisionLoop will move
    // it on to Idle/Failed itself once the worker thread actually notices
    // the stop event and unwinds, same as every other state transition.
    session->state = SessionState::Stopping;
}

void HandleRemoveClick(HWND hwnd, size_t index)
{
    if (index >= g_sessions.size())
        return;
    AccountSession& session = *g_sessions[index];
    if (IsSessionBusy(session)) {
        MessageBoxA(hwnd, "Exit this account first.", "coclassic", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return;
    }
    if (MessageBoxA(hwnd, "Remove this saved account?", "coclassic",
            MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) != IDYES)
        return;

    if (session.stopEvent)
        CloseHandle(session.stopEvent);
    g_sessions.erase(g_sessions.begin() + static_cast<long>(index));
    SaveSessionsToCredentials();
    RebuildRows(hwnd);
}

// Mirrors the field-by-field InputBox() sequence the old modal
// account-picker dialog used for "Add New" — see this file's git history
// for the version this replaced.
void HandleAddAccountClick(HWND hwnd)
{
    struct ScrubOnExit {
        char* buf; size_t len;
        ~ScrubOnExit() { SecureZeroMemory(buf, len); }
    };
    char label[128] = "", username[128] = "", password[128] = "", server[128] = "Classic (US)";
    ScrubOnExit scrubPassword{password, sizeof(password)};
    if (!InputBox(hwnd, "Add Account - Label", "Label (e.g. \"Main\"):", label, sizeof(label)))
        return;
    if (!InputBox(hwnd, "Add Account - Username", "Username:", username, sizeof(username)))
        return;
    if (!InputBox(hwnd, "Add Account - Password", "Password:", password, sizeof(password), "", true))
        return;
    if (!InputBox(hwnd, "Add Account - Server", "Server (as shown in the login screen's dropdown):",
            server, sizeof(server), server))
        return;

    AccountProfile profile;
    profile.label = label;
    profile.username = username;
    profile.password = password;
    profile.server = server;

    const int useProxy = MessageBoxA(hwnd,
        "Use a SOCKS5 proxy for this account?\n\n"
        "Select YES to save a proxy for this account (skips proxy setup "
        "prompts on future logins). Select NO to always connect directly.",
        "Add Account - Proxy", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
    if (useProxy == IDYES) {
        char hostPort[128] = "", proxyUser[128] = "", proxyPassword[128] = "";
        ScrubOnExit scrubProxyPassword{proxyPassword, sizeof(proxyPassword)};
        if (!InputBox(hwnd, "Add Account - Proxy Address", "Proxy host:port (e.g. 127.0.0.1:1080):",
                hostPort, sizeof(hostPort)))
            return;
        const int useAuth = MessageBoxA(hwnd, "Does this proxy require a username/password?",
            "Add Account - Proxy Auth", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
        if (useAuth == IDYES) {
            if (!InputBox(hwnd, "Add Account - Proxy Username", "Proxy username:", proxyUser, sizeof(proxyUser)))
                return;
            if (!InputBox(hwnd, "Add Account - Proxy Password", "Proxy password:",
                    proxyPassword, sizeof(proxyPassword), "", true))
                return;
        }
        profile.useProxy = true;
        profile.proxyHostPort = hostPort;
        profile.proxyUser = proxyUser;
        profile.proxyPassword = proxyPassword;
    }

    auto session = std::make_unique<AccountSession>();
    session->profile = std::move(profile);
    session->stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_sessions.push_back(std::move(session));
    SaveSessionsToCredentials();
    RebuildRows(hwnd);
}

// Real "Exit Manager" quit path — stops supervision for every running
// account (never kills the games themselves, matching this project's
// previous CTRL_CLOSE_EVENT behavior, now an explicit action instead of
// an accidental side effect of closing the wrong window). Non-blocking:
// a worker could be mid-Inject() or mid-login-wait (up to tens of
// seconds) when this is requested, so the UI thread polls for every
// session to actually finish instead of joining synchronously here.
void BeginManagerExit(HWND hwnd)
{
    g_exiting = true;
    for (auto& s : g_sessions) {
        if (s->worker.joinable()) {
            s->killGameOnStop = false;
            if (s->stopEvent)
                SetEvent(s->stopEvent);
        }
    }
    ShowWindow(hwnd, SW_HIDE);
    SetTimer(hwnd, kExitPollTimerId, kExitPollIntervalMs, nullptr);
}

LRESULT CALLBACK ManagerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = GetModuleHandleA(nullptr);
        CreateWindowA("BUTTON", "Add Account", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 10, 140, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAddAccount)), hInst, nullptr);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == kIdAddAccount) {
            HandleAddAccountClick(hwnd);
        } else if (id == kIdTrayShow) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else if (id == kIdTrayExit) {
            BeginManagerExit(hwnd);
        } else if (id >= kIdRowActionBase && id < kIdRowActionBase + static_cast<int>(g_sessions.size())) {
            AccountSession& session = *g_sessions[static_cast<size_t>(id - kIdRowActionBase)];
            if (IsSessionBusy(session))
                HandleExitClick(&session);
            else
                HandleLoginClick(&session);
            RefreshRows(hwnd);
        } else if (id >= kIdRowRemoveBase && id < kIdRowRemoveBase + static_cast<int>(g_sessions.size())) {
            HandleRemoveClick(hwnd, static_cast<size_t>(id - kIdRowRemoveBase));
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == kRowRefreshTimerId) {
            RefreshRows(hwnd);
        } else if (wParam == kExitPollTimerId) {
            const bool allDone = std::all_of(g_sessions.begin(), g_sessions.end(),
                [](const std::unique_ptr<AccountSession>& s) { return !s->worker.joinable() || s->finished.load(); });
            if (allDone) {
                KillTimer(hwnd, kExitPollTimerId);
                for (auto& s : g_sessions)
                    if (s->worker.joinable())
                        s->worker.join();
                Shell_NotifyIconA(NIM_DELETE, &g_trayIcon);
                DestroyWindow(hwnd);
            }
        }
        return 0;
    case kWmTrayIcon: {
        const UINT mouseMsg = static_cast<UINT>(lParam);
        if (mouseMsg == WM_LBUTTONUP || mouseMsg == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else if (mouseMsg == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuA(menu, MF_STRING, kIdTrayShow, "Show Account Manager");
            AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(menu, MF_STRING, kIdTrayExit, "Exit Manager (stop all supervision)");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
        }
        return 0;
    }
    case WM_CLOSE:
        // Never destroy on the X button — this is what makes "closing the
        // manager window doesn't break auto-relogin for running accounts"
        // unconditional. Only BeginManagerExit() (tray menu) really quits.
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    RedirectStdioToLogFile();
    printf("=== coclassic account manager starting ===\n");

    // Winsock init is process-wide (refcounted by the OS), not per-thread —
    // one Start() here covers every account's proxy relay for the whole
    // app lifetime. Harmless to call even if no account ever uses a proxy.
    WinsockSession winsock;
    if (!winsock.Start())
        printf("[!] WSAStartup failed -- proxy-mode accounts will not be able to connect.\n");

    for (const AccountProfile& profile : Credentials::LoadAll()) {
        auto session = std::make_unique<AccountSession>();
        session->profile = profile;
        session->stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        g_sessions.push_back(std::move(session));
    }

    WNDCLASSA wc{};
    wc.lpfnWndProc = ManagerWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconA(hInstance, MAKEINTRESOURCEA(100));  // launcher.rc's icon
    wc.lpszClassName = "CoClassicManagerWnd";
    RegisterClassA(&wc);

    constexpr DWORD kStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT clientRect{0, 0, kWindowClientWidth, 200};
    AdjustWindowRect(&clientRect, kStyle, FALSE);
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int w = clientRect.right - clientRect.left;
    const int h = clientRect.bottom - clientRect.top;

    HWND hwnd = CreateWindowExA(0, "CoClassicManagerWnd", "CoClassic Account Manager", kStyle,
        (screenW - w) / 2, (screenH - h) / 2, w, h, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        MessageBoxA(nullptr, "Failed to create the account manager window.", "coclassic",
            MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }

    RebuildRows(hwnd);
    RefreshRows(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = hwnd;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = kWmTrayIcon;
    g_trayIcon.hIcon = wc.hIcon;
    strncpy_s(g_trayIcon.szTip, "CoClassic Account Manager", _TRUNCATE);
    Shell_NotifyIconA(NIM_ADD, &g_trayIcon);

    SetTimer(hwnd, kRowRefreshTimerId, kRowRefreshIntervalMs, nullptr);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return static_cast<int>(msg.wParam);
}
