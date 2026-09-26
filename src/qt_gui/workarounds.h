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
//   1. <launcher dir>/custom_configs/global.json       bundled, all games
//   2. user/custom_configs/workarounds/global.json     user, all games
//   3. <launcher dir>/custom_configs/<serial>.json     bundled, one game
//   4. user/custom_configs/workarounds/<serial>.json   user, one game
//
// Presets use a flat "Workarounds" object, though regular sections like "General" and "GPU" also work
// Before launch, merge the layers into user/custom_configs/<serial>.workarounds.json for the emulator to apply over the game config
namespace Workarounds {

enum class ValueType { Bool, Int, UInt64 };

// Settings unique to this emulator build that do not appear in the regular tabs
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

/// Saves a user layer, deleting the file if entries is empty
bool SaveUserLayer(const std::filesystem::path& path, const Entries& entries);

} // namespace Workarounds
