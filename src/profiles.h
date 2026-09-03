#pragma once
#include <string>
#include <vector>

// ── Hunt/Mining Profiles ─────────────────────────────────────────────────────
// A Profile is a named, complete snapshot of one settings category
// (AutoHuntSettings or MiningSettings), stored in a shared file
// (coclassic_profiles.ini, next to the DLL — same directory every
// character/account already loads from) so it's usable from any character,
// not just the one that saved it. Loading a profile overwrites the live
// settings singleton; nothing is pushed back to the profile until an
// explicit Save. See the "Hunt Profiles + Mining Profiles" plan for the
// full design.
enum class ProfileKind
{
    Hunt,
    Mining,
};

// Display names, alphabetically sorted (case-insensitive).
std::vector<std::string> ListProfiles(ProfileKind kind);

// Case-insensitive existence check by display name.
bool ProfileExists(ProfileKind kind, const std::string& name);

// Saves the CURRENT live settings under `name` — creates a new profile, or
// overwrites in place if `name` already exists. Sets the active-profile
// tracker to `name` on success.
bool SaveProfile(ProfileKind kind, const std::string& name);

// Loads `name`'s stored settings into the live settings singleton. Sets the
// active-profile tracker to `name` on success; false (no-op) if not found.
bool LoadProfile(ProfileKind kind, const std::string& name);

// Renames a stored profile in place (its saved settings are untouched).
// Fails if `oldName` doesn't exist or `newName` already names a DIFFERENT
// existing profile. Updates the active-profile tracker if it pointed at
// `oldName`.
bool RenameProfile(ProfileKind kind, const std::string& oldName, const std::string& newName);

// Deletes a stored profile. Clears the active-profile tracker if it pointed
// at `name` (the live settings themselves are left as-is).
bool DeleteProfile(ProfileKind kind, const std::string& name);

// The name of the profile last explicitly Loaded/Saved for this kind, empty
// if none. Persisted per-character (see config.cpp's SaveSharedSections/
// LoadSharedSections) so the UI shows "Active: X" again after a relogin —
// this is bookkeeping only, never written into a profile's own saved
// content.
std::string& GetActiveProfileName(ProfileKind kind);

// Renders the shared profile bar: name dropdown + Load/Save/Rename/Delete,
// with an explicit overwrite-vs-save-as-new prompt on Save. Call once per
// panel that should offer this profile kind (Hunt: base_hunt_plugin.cpp's
// RenderDashboardUI; Mining: mining_plugin.cpp's RenderUI).
void RenderProfileBar(ProfileKind kind);
