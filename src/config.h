#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct MapSettings
{
    float cellSize    = 1.0f;  // zoom multiplier (1.0 = auto-fit full map)
    bool showEntities = true;
    bool followHero   = true;
};

struct GuildSettings
{
    bool showDeadOnly = false;
    char guildWhitelist[256] = "";  // comma-separated guild names to also show dead members
};

struct MiscSettings
{
    bool whisperNotifyEnabled = false;
    bool itemNotifyEnabled = false;
    bool lootDropNotifyEnabled = false;
    std::vector<uint32_t> notifyItemIds;
    std::vector<uint32_t> mentionItemIds;  // subset of notifyItemIds that also @mention

    // spdlog::level::level_enum value (trace=0 .. off=6). Defaults to trace to
    // match the previous hardcoded behavior; most users should turn this down
    // — trace generates tens of thousands of lines in a few minutes of play.
    int logLevel = 0;
};

struct TravelSettings
{
    bool usePacketJump = false;
};

struct SkillTrainerSettings
{
    int  castDelayMs = 1000;
    bool autoMpPotion = false;
    uint32_t selectedSkillId = 0;
};

MapSettings& GetMapSettings();
GuildSettings& GetGuildSettings();
MiscSettings& GetMiscSettings();
TravelSettings& GetTravelSettings();
SkillTrainerSettings& GetSkillTrainerSettings();

// Save/load all settings to/from a per-character coclassic_<name_uid>.ini next to the DLL.
// Falls back to coclassic.ini only when loading a character without its own file yet.
void LoadConfig();
void SaveConfig();
void MaybeAutoSaveConfig();
void UpdateCharacterConfigBinding();

// ── Shared INI/path primitives, exposed for src/profiles.cpp ────────────────
// A Profile is just another (file, section) pair pointed at a shared,
// unscoped .ini instead of the per-character one — these are the exact same
// helpers config.cpp uses internally for the character file.
std::string GetConfigDirectory();
void WriteInt(const char* file, const char* section, const char* key, int val);
void WriteFloat(const char* file, const char* section, const char* key, float val);
int ReadInt(const char* file, const char* section, const char* key, int def);
float ReadFloat(const char* file, const char* section, const char* key, float def);
// unsigned long, not uint32_t/DWORD by name — matches Win32's DWORD exactly
// (unsigned long on this platform) without requiring <windows.h> here; a
// same-width-but-distinct-type mismatch (e.g. uint32_t == unsigned int)
// against config.cpp's actual DWORD parameter would be a linker error.
void ReadString(const char* file, const char* section, const char* key,
                 const char* def, char* out, unsigned long outSize);
// emptyFallback is used verbatim when text sanitizes to nothing (e.g. a name
// that's entirely punctuation) — pass a fallback appropriate to what's being
// sanitized (a character name vs. a user-typed profile name).
std::string SanitizeIniToken(const char* text, const char* emptyFallback);

void SaveAutoHuntSection(const char* file, const char* section);
void LoadAutoHuntSection(const char* file, const char* section);
void SaveMiningSection(const char* file, const char* section);
void LoadMiningSection(const char* file, const char* section);
