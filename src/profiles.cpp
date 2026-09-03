#include "profiles.h"
#include "config.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include "imgui.h"

namespace {

const char* KindPrefix(ProfileKind kind)
{
    return kind == ProfileKind::Hunt ? "HuntProfile:" : "MiningProfile:";
}

std::string ProfilesFilePath()
{
    return GetConfigDirectory() + "coclassic_profiles.ini";
}

bool IEquals(const std::string& a, const std::string& b)
{
    return _stricmp(a.c_str(), b.c_str()) == 0;
}

// All section keys (including the kind's prefix) currently present in the
// profiles file for this kind.
std::vector<std::string> EnumerateSectionKeys(ProfileKind kind)
{
    std::vector<std::string> keys;
    const std::string file = ProfilesFilePath();
    // Section-name list is double-null-terminated; each entry itself is a
    // plain null-terminated string. 64KB comfortably covers even hundreds
    // of profiles (section names here are short: prefix + sanitized name).
    std::vector<char> buf(65536, '\0');
    GetPrivateProfileSectionNamesA(buf.data(), (DWORD)buf.size(), file.c_str());

    const char* prefix = KindPrefix(kind);
    const size_t prefixLen = strlen(prefix);
    const char* p = buf.data();
    while (*p) {
        std::string section(p);
        if (section.compare(0, prefixLen, prefix) == 0)
            keys.push_back(section);
        p += section.size() + 1;
    }
    return keys;
}

std::string ReadDisplayName(const std::string& file, const std::string& section)
{
    char buf[128] = {};
    GetPrivateProfileStringA(section.c_str(), "DisplayName", "", buf, sizeof(buf), file.c_str());
    return buf;
}

// Empty string if no profile of this kind has this display name.
std::string FindSectionKeyByName(ProfileKind kind, const std::string& name)
{
    const std::string file = ProfilesFilePath();
    for (const std::string& key : EnumerateSectionKeys(kind)) {
        if (IEquals(ReadDisplayName(file, key), name))
            return key;
    }
    return {};
}

// Builds a fresh section key for a brand-new profile name — sanitized name,
// prefixed by kind, uniquified with a numeric suffix if that exact key is
// already in use by an unrelated profile (e.g. two differently-typed names
// that sanitize to the same token).
std::string MakeUniqueSectionKey(ProfileKind kind, const std::string& name)
{
    const std::string prefix = KindPrefix(kind);
    const std::string base = prefix + SanitizeIniToken(name.c_str(), "Profile");
    const std::vector<std::string> existing = EnumerateSectionKeys(kind);

    std::string candidate = base;
    int suffix = 2;
    while (std::find(existing.begin(), existing.end(), candidate) != existing.end()) {
        candidate = base + "_" + std::to_string(suffix++);
    }
    return candidate;
}

std::string& ActiveHuntProfileName()
{
    static std::string name;
    return name;
}

std::string& ActiveMiningProfileName()
{
    static std::string name;
    return name;
}

struct ProfileBarState
{
    std::string pendingSelection;
    char nameBuf[128] = "";
};

ProfileBarState& BarStateFor(ProfileKind kind)
{
    static ProfileBarState hunt;
    static ProfileBarState mining;
    return kind == ProfileKind::Hunt ? hunt : mining;
}

} // namespace

std::vector<std::string> ListProfiles(ProfileKind kind)
{
    const std::string file = ProfilesFilePath();
    const char* prefix = KindPrefix(kind);
    const size_t prefixLen = strlen(prefix);

    std::vector<std::string> names;
    for (const std::string& key : EnumerateSectionKeys(kind)) {
        std::string display = ReadDisplayName(file, key);
        if (display.empty())
            display = key.substr(prefixLen); // defensive: shouldn't happen via our own writers
        names.push_back(display);
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return _stricmp(a.c_str(), b.c_str()) < 0;
    });
    return names;
}

bool ProfileExists(ProfileKind kind, const std::string& name)
{
    return !FindSectionKeyByName(kind, name).empty();
}

bool SaveProfile(ProfileKind kind, const std::string& name)
{
    if (name.empty())
        return false;

    const std::string file = ProfilesFilePath();
    std::string key = FindSectionKeyByName(kind, name);
    if (key.empty())
        key = MakeUniqueSectionKey(kind, name);

    if (kind == ProfileKind::Hunt)
        SaveAutoHuntSection(file.c_str(), key.c_str());
    else
        SaveMiningSection(file.c_str(), key.c_str());
    WritePrivateProfileStringA(key.c_str(), "DisplayName", name.c_str(), file.c_str());

    GetActiveProfileName(kind) = name;
    SaveConfig(); // persist the active-profile bookkeeping to this character's own file
    return true;
}

bool LoadProfile(ProfileKind kind, const std::string& name)
{
    const std::string file = ProfilesFilePath();
    const std::string key = FindSectionKeyByName(kind, name);
    if (key.empty())
        return false;

    if (kind == ProfileKind::Hunt)
        LoadAutoHuntSection(file.c_str(), key.c_str());
    else
        LoadMiningSection(file.c_str(), key.c_str());

    GetActiveProfileName(kind) = name;
    SaveConfig(); // persist both the newly-applied settings and the active-profile bookkeeping
    return true;
}

bool RenameProfile(ProfileKind kind, const std::string& oldName, const std::string& newName)
{
    if (newName.empty())
        return false;

    const std::string file = ProfilesFilePath();
    const std::string oldKey = FindSectionKeyByName(kind, oldName);
    if (oldKey.empty())
        return false;

    // Renaming onto an existing DIFFERENT profile's name is refused — the UI
    // offers overwrite explicitly via Save, renaming should never silently
    // clobber another saved profile.
    const std::string collideKey = FindSectionKeyByName(kind, newName);
    if (!collideKey.empty() && collideKey != oldKey)
        return false;

    // GetPrivateProfileSectionA returns "key=value\0key=value\0...\0\0" for
    // the section — 8KB comfortably covers AutoHunt's ~120 keys plus its
    // longest string fields (item-id lists, monster name filters).
    std::vector<char> sectionBuf(8192, '\0');
    GetPrivateProfileSectionA(oldKey.c_str(), sectionBuf.data(), (DWORD)sectionBuf.size(), file.c_str());

    const std::string newKey = MakeUniqueSectionKey(kind, newName);
    for (const char* p = sectionBuf.data(); *p; p += strlen(p) + 1) {
        const std::string entry(p);
        const size_t eq = entry.find('=');
        if (eq == std::string::npos)
            continue;
        WritePrivateProfileStringA(newKey.c_str(), entry.substr(0, eq).c_str(),
            entry.substr(eq + 1).c_str(), file.c_str());
    }
    WritePrivateProfileStringA(newKey.c_str(), "DisplayName", newName.c_str(), file.c_str());
    WritePrivateProfileStringA(oldKey.c_str(), nullptr, nullptr, file.c_str()); // deletes the whole section

    if (IEquals(GetActiveProfileName(kind), oldName)) {
        GetActiveProfileName(kind) = newName;
        SaveConfig();
    }
    return true;
}

bool DeleteProfile(ProfileKind kind, const std::string& name)
{
    const std::string file = ProfilesFilePath();
    const std::string key = FindSectionKeyByName(kind, name);
    if (key.empty())
        return false;

    WritePrivateProfileStringA(key.c_str(), nullptr, nullptr, file.c_str());
    if (IEquals(GetActiveProfileName(kind), name)) {
        GetActiveProfileName(kind).clear();
        SaveConfig();
    }
    return true;
}

std::string& GetActiveProfileName(ProfileKind kind)
{
    return kind == ProfileKind::Hunt ? ActiveHuntProfileName() : ActiveMiningProfileName();
}

void RenderProfileBar(ProfileKind kind)
{
    ImGui::PushID(kind == ProfileKind::Hunt ? "HuntProfileBar" : "MiningProfileBar");

    ProfileBarState& state = BarStateFor(kind);
    std::string& active = GetActiveProfileName(kind);
    const std::vector<std::string> names = ListProfiles(kind);

    if (state.pendingSelection.empty() && !active.empty())
        state.pendingSelection = active;

    ImGui::TextUnformatted("Profile:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    const char* previewName = state.pendingSelection.empty() ? "(none selected)" : state.pendingSelection.c_str();
    if (ImGui::BeginCombo("##profileList", previewName)) {
        if (names.empty())
            ImGui::TextDisabled("No saved profiles yet");
        for (const std::string& name : names) {
            const bool selected = (name == state.pendingSelection);
            if (ImGui::Selectable(name.c_str(), selected))
                state.pendingSelection = name;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(state.pendingSelection.empty());
    if (ImGui::Button("Load"))
        LoadProfile(kind, state.pendingSelection);
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        strncpy_s(state.nameBuf, active.empty() ? "" : active.c_str(), _TRUNCATE);
        ImGui::OpenPopup("SaveProfilePopup");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(active.empty());
    if (ImGui::Button("Rename")) {
        strncpy_s(state.nameBuf, active.c_str(), _TRUNCATE);
        ImGui::OpenPopup("RenameProfilePopup");
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete"))
        ImGui::OpenPopup("DeleteProfilePopup");
    ImGui::EndDisabled();

    if (active.empty())
        ImGui::TextDisabled("Active: (none — unsaved)");
    else
        ImGui::Text("Active: %s", active.c_str());

    if (ImGui::BeginPopup("SaveProfilePopup")) {
        if (!active.empty()) {
            ImGui::Text("Save changes to the loaded profile, or as a new one?");
            const std::string overwriteLabel = "Overwrite \"" + active + "\"";
            if (ImGui::Button(overwriteLabel.c_str())) {
                SaveProfile(kind, active);
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
        }
        ImGui::TextUnformatted("Save As New:");
        ImGui::InputText("##saveAsNewName", state.nameBuf, sizeof(state.nameBuf));
        ImGui::SameLine();
        ImGui::BeginDisabled(state.nameBuf[0] == '\0');
        if (ImGui::Button("Save As New")) {
            SaveProfile(kind, state.nameBuf);
            state.pendingSelection = state.nameBuf;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("RenameProfilePopup")) {
        ImGui::Text("Rename \"%s\" to:", active.c_str());
        ImGui::InputText("##renameName", state.nameBuf, sizeof(state.nameBuf));
        ImGui::BeginDisabled(state.nameBuf[0] == '\0');
        if (ImGui::Button("Confirm Rename")) {
            if (RenameProfile(kind, active, state.nameBuf))
                state.pendingSelection = state.nameBuf;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("DeleteProfilePopup")) {
        ImGui::Text("Delete profile \"%s\"? This cannot be undone.", active.c_str());
        if (ImGui::Button("Delete")) {
            DeleteProfile(kind, active);
            state.pendingSelection.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::TextDisabled("Shared across every account. Loading/switching profiles never touches "
        "this character's own save — only an explicit Save here changes a profile.");

    ImGui::PopID();
}
