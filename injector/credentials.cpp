#include "credentials.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <wincrypt.h>

#include <filesystem>
#include <fstream>
#include <optional>
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

// Session 12: additional entropy for CryptProtectData/CryptUnprotectData.
// This is NOT a secret (it ships in the binary) — DPAPI's real protection
// comes from the user's own protected master key — but without any entropy,
// ANY process running as the same Windows user can call CryptUnprotectData
// on the raw blob with no app-specific knowledge required at all. Given this
// project's whole threat model is "processes get injected into games,"
// requiring the caller to also know/extract this value raises the bar
// somewhat over having none.
constexpr BYTE kEntropyBytes[] = {
    0x8C, 0x1E, 0x4A, 0x2D, 0x9F, 0x6B, 0x3C, 0x71,
    0xE5, 0x0A, 0xD4, 0x5C, 0x2B, 0x8F, 0x11, 0x7E,
};

DATA_BLOB EntropyBlob()
{
    DATA_BLOB entropy{};
    entropy.pbData = const_cast<BYTE*>(kEntropyBytes);
    entropy.cbData = static_cast<DWORD>(sizeof(kEntropyBytes));
    return entropy;
}

// std::nullopt on genuine decrypt failure (corrupt blob, or a store copied
// from a different Windows user/machine — DPAPI ties the key to the user
// profile). An empty-but-present cipherBytes input (nothing was ever saved
// for this field, e.g. no proxy password) is NOT an error and returns "".
std::optional<std::string> DpapiDecrypt(const std::vector<unsigned char>& cipherBytes)
{
    if (cipherBytes.empty())
        return std::string();

    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(cipherBytes.data());
    in.cbData = static_cast<DWORD>(cipherBytes.size());

    DATA_BLOB entropy = EntropyBlob();
    DATA_BLOB out{};
    // Session 12: try WITH entropy first (the current format). Falls back to
    // no entropy for accounts.dat files saved before entropy was added, so
    // this change doesn't lock the user out of already-saved accounts — the
    // next SaveAll() re-encrypts with entropy, migrating it silently.
    BOOL ok = CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok)
        ok = CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok)
        return std::nullopt;

    std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
    // Session 12: scrub the decrypted bytes before freeing rather than
    // leaving the plaintext sitting in a freed (but not yet reused) heap
    // block.
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}

std::string DpapiEncrypt(const std::string& plaintext)
{
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    in.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB entropy = EntropyBlob();
    DATA_BLOB out{};
    // CRYPTPROTECT_LOCAL_MACHINE deliberately NOT set — tying this to the
    // user profile (not the machine) is the whole point.
    if (!CryptProtectData(&in, L"coclassic launcher credentials", &entropy, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return "";

    std::string encoded = Base64Encode(out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return encoded;
}

// Session 14 [USERNAME ENCRYPTION]: username and proxyUser used to be stored
// as plaintext JSON alongside their now-encrypted passwords. They're now
// DPAPI-encrypted the same way. This reader migrates existing stores
// transparently: try to decrypt the stored value as an encrypted+base64
// blob, and if that fails, treat the raw stored string as an old-format
// PLAINTEXT value (which the next SaveAll re-encrypts, silently migrating
// it). Mirrors exactly how the password field's entropy fallback already
// handles an old-format store (DpapiDecrypt above). Unlike the password —
// where a decrypt failure MUST skip the account rather than risk an
// auto-login with a garbage credential — a non-decryptable username is
// almost certainly just an old plaintext entry, and login identifiers
// aren't dangerous the way a wrong password is, so plaintext fallback here
// is the safe, lock-nobody-out choice.
std::string DecryptOrPlaintext(const std::string& stored)
{
    if (stored.empty())
        return std::string();
    auto decrypted = DpapiDecrypt(Base64Decode(stored));
    if (decrypted)
        return std::move(*decrypted);
    return stored;  // old-format plaintext — migrated on next SaveAll
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
        profile.username = DecryptOrPlaintext(entry.value("username", ""));
        profile.server = entry.value("server", "");

        const std::string encoded = entry.value("password", "");
        auto password = DpapiDecrypt(Base64Decode(encoded));
        if (!password)
            continue;  // Session 12: undecryptable blob — skip rather than silently auto-login with a blank password
        profile.password = std::move(*password);

        profile.useProxy = entry.value("useProxy", false);
        profile.proxyHostPort = entry.value("proxyHostPort", "");
        profile.proxyUser = DecryptOrPlaintext(entry.value("proxyUser", ""));

        const std::string encodedProxyPassword = entry.value("proxyPassword", "");
        auto proxyPassword = DpapiDecrypt(Base64Decode(encodedProxyPassword));
        if (!proxyPassword)
            continue;
        profile.proxyPassword = std::move(*proxyPassword);

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
        entry["label"] = profile.label;  // nickname — intentionally NOT encrypted (it's the UI display name, not a credential)
        entry["username"] = DpapiEncrypt(profile.username);
        entry["server"] = profile.server;
        entry["password"] = DpapiEncrypt(profile.password);
        entry["useProxy"] = profile.useProxy;
        entry["proxyHostPort"] = profile.proxyHostPort;
        entry["proxyUser"] = DpapiEncrypt(profile.proxyUser);
        entry["proxyPassword"] = DpapiEncrypt(profile.proxyPassword);
        root.push_back(std::move(entry));
    }

    std::ofstream file(GetStorePath(), std::ios::trunc);
    if (!file)
        return false;
    file << root.dump(2);
    return true;
}

}  // namespace Credentials
