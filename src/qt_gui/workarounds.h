// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

// Apply workaround presets by game serial
//
// Layers, from lowest to highest priority:
//   1. <launcher dir>/custom_configs/global.json   launcher preset, all games
//   2. user/custom_configs/global.json             user, all games
//   3. <launcher dir>/custom_configs/<serial>.json launcher preset, one game
//   4. user/custom_configs/<serial>.json           user, one game (the emulator's game config)
//
// Presets use a flat "Workarounds" object, though regular sections like "General" and "GPU" also work
// The launcher embeds its presets, each with a "version". At startup it writes the missing ones
// next to itself and adds new keys to copies with an older or no version, keeping their values
// A new game config starts with the settings the presets set in the launcher's own tabs, so those
// tabs show and save the preset values. Workarounds changed by the user are saved there too
// Before launch, merge the layers into user/custom_configs/<serial>.workarounds.json for the emulator to apply over the game config
namespace Workarounds {

enum class ValueType { Bool, Int, UInt64 };

// Settings shown in the Workarounds tab, unique to this emulator build. The launcher's own tabs edit
// the regular settings
struct KeyInfo {
    const char* key;
    const char* section; // Config section used when saving the key
    ValueType type;
    std::int64_t default_value;
    std::int64_t min;
    std::int64_t max;
    const char* label;
    const char* description;
};

const std::vector<KeyInfo>& GetKnownKeys();
const KeyInfo* FindKnownKey(std::string_view key);

enum class LayerKind { LauncherGlobal, UserGlobal, LauncherGame, UserGame };

struct Entry {
    std::string section;
    nlohmann::json value;
};
using Entries = std::map<std::string, Entry>;

struct Layer {
    LayerKind kind;
    std::filesystem::path path;
    bool exists = false;
    Entries entries;
};

std::filesystem::path GetLauncherPresetDir();
std::filesystem::path GetUserPresetDir();
std::filesystem::path GetOverlayPath(const std::string& serial);

/// Writes missing launcher presets and upgrades copies older than the embedded ones
void UpdateLauncherPresets();

/// Moves user layers from the old user/custom_configs/workarounds folder to user/custom_configs
void MigrateUserPresets();

/// Creates the game config from the regular settings the presets set, when it does not exist yet
/// Returns true when it was created
bool CreateGameConfig(const std::string& serial);

/// Loads layers by priority, using only global layers when serial is empty
std::vector<Layer> LoadLayers(const std::string& serial);

/// Merges the first count layers, with later values taking priority
Entries Merge(const std::vector<Layer>& layers, std::size_t count);

/// Reads known keys from global config.json, falling back to their defaults
std::map<std::string, nlohmann::json> GetGlobalValues();

/// Checks whether WriteOverlay created this file
bool IsGeneratedOverlay(const std::filesystem::path& path);

/// Writes the overlay, or removes it when all layers are empty
/// Leaves a manually created overlay alone and returns false
bool WriteOverlay(const std::string& serial);

/// Replaces keys in a user layer with entries, leaving its other keys alone
/// Deletes the file when no values remain
bool SaveUserLayer(const std::filesystem::path& path, const std::vector<std::string>& keys,
                   const Entries& entries);

} // namespace Workarounds
