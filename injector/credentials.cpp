#include "credentials.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <wincrypt.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char* kStoreFileName = "accounts.dat";

std::string GetStorePath()
{
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    return (fs::path(exePath).parent_path() / kStoreFileName).string();
}

const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const unsigned char* data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kBase64Chars[(n >> 18) & 0x3F];
        out += kBase64Chars[(n >> 12) & 0x3F];
        out += kBase64Chars[(n >> 6) & 0x3F];
        out += kBase64Chars[n & 0x3F];
    }
    const size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = data[i] << 16;
        out += kBase64Chars[(n >> 18) & 0x3F];
        out += kBase64Chars[(n >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
        out += kBase64Chars[(n >> 18) & 0x3F];
        out += kBase64Chars[(n >> 12) & 0x3F];
        out += kBase64Chars[(n >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

int DecodeChar(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<unsigned char> Base64Decode(const std::string& text)
{
    std::vector<unsigned char> out;
    out.reserve((text.size() / 4) * 3);
    int vals[4];
    size_t n = 0;
    for (char c : text) {
        if (c == '=' || c == '\0')
            break;
        const int v = DecodeChar(c);
        if (v < 0)
            continue;
        vals[n++] = v;
        if (n == 4) {
            out.push_back(static_cast<unsigned char>((vals[0] << 2) | (vals[1] >> 4)));
            out.push_back(static_cast<unsigned char>((vals[1] << 4) | (vals[2] >> 2)));
            out.push_back(static_cast<unsigned char>((vals[2] << 6) | vals[3]));
            n = 0;
        }
    }
    if (n >= 2) {
        out.push_back(static_cast<unsigned char>((vals[0] << 2) | (vals[1] >> 4)));
        if (n == 3)
            out.push_back(static_cast<unsigned char>((vals[1] << 4) | (vals[2] >> 2)));
    }
    return out;
}

// Empty string on failure (bad blob, or decrypting a store from a different
// Windows user/machine — DPAPI ties the key to the user profile).
std::string DpapiDecrypt(const std::vector<unsigned char>& cipherBytes)
{
    if (cipherBytes.empty())
        return "";

    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(cipherBytes.data());
    in.cbData = static_cast<DWORD>(cipherBytes.size());

    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
        return "";

    std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return result;
}

std::string DpapiEncrypt(const std::string& plaintext)
{
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    in.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB out{};
    // CRYPTPROTECT_LOCAL_MACHINE deliberately NOT set — tying this to the
    // user profile (not the machine) is the whole point.
    if (!CryptProtectData(&in, L"coclassic launcher credentials", nullptr, nullptr, nullptr, 0, &out))
        return "";

    std::string encoded = Base64Encode(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return encoded;
}

}  // namespace

namespace Credentials {

std::vector<AccountProfile> LoadAll()
{
    std::vector<AccountProfile> profiles;

    const std::string path = GetStorePath();
    if (!fs::exists(path))
        return profiles;

    std::ifstream file(path);
    if (!file)
        return profiles;

    json root;
    try {
        file >> root;
    } catch (const json::exception&) {
        return profiles;
    }

    if (!root.is_array())
        return profiles;

    for (const auto& entry : root) {
        AccountProfile profile;
        profile.label = entry.value("label", "");
        profile.username = entry.value("username", "");
        profile.server = entry.value("server", "");
        const std::string encoded = entry.value("password", "");
        profile.password = DpapiDecrypt(Base64Decode(encoded));
        if (profile.username.empty())
            continue;  // corrupt entry — skip rather than fail the whole load
        profiles.push_back(std::move(profile));
    }

    return profiles;
}

bool SaveAll(const std::vector<AccountProfile>& profiles)
{
    json root = json::array();
    for (const auto& profile : profiles) {
        json entry;
        entry["label"] = profile.label;
        entry["username"] = profile.username;
        entry["server"] = profile.server;
        entry["password"] = DpapiEncrypt(profile.password);
        root.push_back(std::move(entry));
    }

    std::ofstream file(GetStorePath(), std::ios::trunc);
    if (!file)
        return false;
    file << root.dump(2);
    return true;
}

}  // namespace Credentials
