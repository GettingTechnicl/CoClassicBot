#pragma once

#include <string>
#include <vector>

// =====================================================================
// credentials.h — locally-stored login profiles for launcher.exe's
// character-select screen.
//
// Passwords are encrypted at rest via Windows DPAPI (CryptProtectData),
// which ties the encryption to the current Windows user account — no
// separate key/passphrase to manage, and the file is useless if copied
// to another machine or user account. This is NOT a hardware-key-grade
// secret vault; it's the same trust model most local single-user tools
// use (anything already running as this Windows user could decrypt it).
// =====================================================================

struct AccountProfile
{
    std::string label;     // user-facing name, e.g. "Main" or "Mule 1"
    std::string username;
    std::string password;  // plaintext in memory only; encrypted at rest
    std::string server;    // matches the login form's Server dropdown text

    // Saved SOCKS5 proxy choice for this account — lets launcher.exe skip
    // its interactive proxy setup dialog entirely once an account is
    // selected, reusing whatever was chosen when the account was added.
    // useProxy=false means "connect directly", not "not decided yet".
    bool useProxy = false;
    std::string proxyHostPort;   // "host:port", e.g. "127.0.0.1:1080"
    std::string proxyUser;       // empty if the proxy needs no auth
    std::string proxyPassword;   // plaintext in memory only; encrypted at rest
};

namespace Credentials {

// Loads all saved profiles, decrypting each password via DPAPI. Returns an
// empty list if the store doesn't exist yet, or if a given entry fails to
// decrypt (e.g. the store was copied from a different Windows user/machine)
// — a bad entry is skipped rather than failing the whole load.
std::vector<AccountProfile> LoadAll();

// Overwrites the entire saved profile list, encrypting each password via
// DPAPI before writing to disk. Returns false on write failure.
bool SaveAll(const std::vector<AccountProfile>& profiles);

}  // namespace Credentials
