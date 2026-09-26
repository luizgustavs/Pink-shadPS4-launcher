// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <QCoreApplication>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "workarounds.h"

using json = nlohmann::json;

namespace Workarounds {

namespace {

constexpr auto kWorkaroundsSection = "Workarounds";
constexpr auto kGeneratedByKey = "_generated_by";
constexpr auto kGeneratedBy = "shadPS4QtLauncher workarounds";
constexpr std::int64_t kIntMax = std::numeric_limits<std::int32_t>::max();
constexpr std::int64_t kUInt64Max = std::numeric_limits<std::int64_t>::max();

// List only keys this emulator supports
// Add these old SotC fork keys once they are ported:
//   GPU: srt_walker_clean_reads, shader_code_clean_reads, readback_writer_tick, readback_flush_writer, gpu_srt_constants,
//        bpe_guard_skip_offheap, periodic_flush_commands, flush_ahead_min_commands
//   General: redirect_app0_logs
//   Audio: audio_follow_game_speed, audio_min_game_speed, audio_game_target_fps
// Leave out drop_stale_gpu_ranges (obsolete), eop_wait_idle and dma_precache_mapped_memory (both caused regressions)
// clang-format off
const std::vector<KeyInfo> kKnownKeys = {
    {"extra_fmem_in_mbytes", "General", ValueType::Int, 0, 0, kIntMax,
     QT_TRANSLATE_NOOP("Workarounds", "Extra flexible memory (MB)"),
     QT_TRANSLATE_NOOP("Workarounds", "Adds flexible memory for games that run out of it.")},

    {"compute_loop_cap", "GPU", ValueType::Int, 0, 0, kIntMax,
     QT_TRANSLATE_NOOP("Workarounds", "Compute loop cap"),
     QT_TRANSLATE_NOOP("Workarounds", "Compute shader loops exit after this many iterations (0 = off), turning a runaway guest loop (GPU TDR) into one wrong dispatch.")},
    {"preserve_split_protection", "GPU", ValueType::Bool, 0, 0, 1,
     QT_TRANSLATE_NOOP("Workarounds", "Preserve split protection"),
     QT_TRANSLATE_NOOP("Workarounds", "Windows: restores the GPU memory tracking protections that splitting a mapped placeholder drops.")},
    {"cpu_authoritative_stacks", "GPU", ValueType::Bool, 0, 0, 1,
     QT_TRANSLATE_NOOP("Workarounds", "CPU-authoritative stacks"),
     QT_TRANSLATE_NOOP("Workarounds", "Guest thread and fiber stacks are never write-protected by GPU memory tracking. A push onto a write-protected stack kills the emulator without any log.")},
    {"bpe_heap_guard_address", "GPU", ValueType::UInt64, 0, 0, kUInt64Max,
     QT_TRANSLATE_NOOP("Workarounds", "BPE heap guard address"),
     QT_TRANSLATE_NOOP("Workarounds", "Address of Shadow of the Colossus' BPE heap object (0 = off). GPU readbacks never overwrite that heap's allocator metadata. Accepts hexadecimal (0x...).")},

    {"lds_barrier_uniform_readlane", "GPU", ValueType::Bool, 0, 0, 1,
     QT_TRANSLATE_NOOP("Workarounds", "LDS barriers in wave-uniform branches"),
     QT_TRANSLATE_NOOP("Workarounds", "Compute shaders keep their shared memory barriers inside branches on a ReadLane with a constant lane (fixes 4-row stripes in SotC's lighting). Changes shader code.")},
    {"early_fragment_tests_from_z_order", "GPU", ValueType::Bool, 0, 0, 1,
     QT_TRANSLATE_NOOP("Workarounds", "Early fragment tests from Z order"),
     QT_TRANSLATE_NOOP("Workarounds", "Pixel shaders with storage writes run depth/stencil tests first when the game asks for early Z (fixes the decal rectangle in SotC). Changes shader code.")},
    {"lod_stats_from_bindings", "GPU", ValueType::Bool, 0, 0, 1,
     QT_TRANSLATE_NOOP("Workarounds", "LOD statistics from bindings"),
     QT_TRANSLATE_NOOP("Workarounds", "Answers the GPU texture LOD counters (IT_GET_LOD_STATS) from the textures bound, so games that stream mips from them load the high-resolution ones. Uses more memory.")},
    {"dynamic_tsharp_array_size", "GPU", ValueType::Int, 0, 0, 64,
     QT_TRANSLATE_NOOP("Workarounds", "Dynamic texture table size"),
     QT_TRANSLATE_NOOP("Workarounds", "Compute shaders that pick a texture from a table at run time get a descriptor array of this many textures (0 = off; SotC reflection probes use 16). Changes shader code.")},
    {"wave64_uniform_branches", "GPU", ValueType::Bool, 0, 0, 1,
     QT_TRANSLATE_NOOP("Workarounds", "Wave64 uniform branches"),
     QT_TRANSLATE_NOOP("Workarounds", "Lowers wave64 lane reads inside branches that are uniform across the workgroup (GPUs with 32-wide subgroups). Needed with the texture table in SotC. Changes shader code.")},
    {"gpu_checkpoints", "GPU", ValueType::Bool, 0, 0, 1,
     QT_TRANSLATE_NOOP("Workarounds", "GPU crash checkpoints (diagnostic)"),
     QT_TRANSLATE_NOOP("Workarounds", "Records every GPU command and submit, with NVIDIA checkpoints when available, and names the command that was running when the GPU is lost. Costs some performance.")},
};
// clang-format on

json DefaultValue(const KeyInfo& info) {
    switch (info.type) {
    case ValueType::Bool:
        return info.default_value != 0;
    case ValueType::UInt64:
        return static_cast<std::uint64_t>(info.default_value);
    case ValueType::Int:
        break;
    }
    return info.default_value;
}

std::optional<std::int64_t> ToInteger(const json& value) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(kUInt64Max)) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(number);
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    // Accept hex strings for addresses too
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        try {
            std::size_t used = 0;
            const auto number = std::stoll(text, &used, 0);
            if (used == text.size()) {
                return number;
            }
        } catch (...) {
        }
    }
    return std::nullopt;
}

// Convert to the emulator's type, or reject values that do not fit
std::optional<json> Normalize(const KeyInfo& info, const json& value) {
    if (info.type == ValueType::Bool) {
        if (value.is_boolean()) {
            return json(value.get<bool>());
        }
        return std::nullopt;
    }
    const auto number = ToInteger(value);
    if (!number || *number < info.min || *number > info.max) {
        return std::nullopt;
    }
    if (info.type == ValueType::UInt64) {
        return json(static_cast<std::uint64_t>(*number));
    }
    return json(*number);
}

void AddEntry(Layer& layer, const std::string& section, const std::string& key, const json& value) {
    if (const auto* info = FindKnownKey(key)) {
        if (auto normalized = Normalize(*info, value)) {
            layer.entries[key] = {info->section, std::move(*normalized)};
        } else {
            LOG_WARNING(Config, "Workarounds: invalid value {} for '{}' in {}", value.dump(), key,
                        layer.path.string());
        }
        return;
    }

    // Pass unfamiliar keys through when their config section is given
    std::string target = EmulatorSettings.GetOverrideableKeySection(key);
    if (target.empty() && section != kWorkaroundsSection) {
        target = section;
    }
    if (target.empty()) {
        LOG_WARNING(Config, "Workarounds: unknown key '{}' in {}", key, layer.path.string());
        return;
    }
    layer.entries[key] = {target, value};
}

json ReadJsonFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return json(json::value_t::discarded);
    }
    return json::parse(in, nullptr, false, true);
}

Layer LoadLayer(LayerKind kind, std::filesystem::path path) {
    Layer layer{kind, std::move(path)};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(layer.path, ec)) {
        return layer;
    }
    layer.exists = true;

    const json root = ReadJsonFile(layer.path);
    if (!root.is_object()) {
        LOG_WARNING(Config, "Workarounds: {} is not a valid JSON object", layer.path.string());
        return layer;
    }
    // JSON sorts object keys, so "Workarounds" takes priority over other sections
    for (const auto& [section, keys] : root.items()) {
        if (!keys.is_object()) {
            continue; // Skip notes such as "_comment"
        }
        for (const auto& [key, value] : keys.items()) {
            AddEntry(layer, section, key, value);
        }
    }
    return layer;
}

} // namespace

const std::vector<KeyInfo>& GetKnownKeys() {
    return kKnownKeys;
}

const KeyInfo* FindKnownKey(std::string_view key) {
    const auto it = std::find_if(kKnownKeys.begin(), kKnownKeys.end(),
                                 [key](const KeyInfo& info) { return key == info.key; });
    return it == kKnownKeys.end() ? nullptr : &*it;
}

std::filesystem::path GetLauncherPresetDir() {
    return (Common::FS::PathFromQString(QCoreApplication::applicationDirPath()) / "custom_configs")
        .make_preferred();
}

std::filesystem::path GetUserPresetDir() {
    return Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) / "workarounds";
}

std::filesystem::path GetOverlayPath(const std::string& serial) {
    return Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
           (serial + ".workarounds.json");
}

std::vector<Layer> LoadLayers(const std::string& serial) {
    std::vector<Layer> layers;
    layers.push_back(LoadLayer(LayerKind::LauncherGlobal, GetLauncherPresetDir() / "global.json"));
    layers.push_back(LoadLayer(LayerKind::UserGlobal, GetUserPresetDir() / "global.json"));
    if (!serial.empty()) {
        layers.push_back(
            LoadLayer(LayerKind::LauncherGame, GetLauncherPresetDir() / (serial + ".json")));
        layers.push_back(LoadLayer(LayerKind::UserGame, GetUserPresetDir() / (serial + ".json")));
    }
    return layers;
}

Entries Merge(const std::vector<Layer>& layers, std::size_t count) {
    Entries merged;
    for (std::size_t i = 0; i < std::min(count, layers.size()); ++i) {
        for (const auto& [key, entry] : layers[i].entries) {
            merged[key] = entry;
        }
    }
    return merged;
}

std::map<std::string, json> GetGlobalValues() {
    const json config =
        ReadJsonFile(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.json");

    std::map<std::string, json> values;
    for (const auto& info : kKnownKeys) {
        std::optional<json> value;
        if (config.is_object() && config.contains(info.section) &&
            config.at(info.section).is_object() && config.at(info.section).contains(info.key)) {
            value = Normalize(info, config.at(info.section).at(info.key));
        }
        values[info.key] = value.value_or(DefaultValue(info));
    }
    return values;
}

bool IsGeneratedOverlay(const std::filesystem::path& path) {
    const json root = ReadJsonFile(path);
    return root.is_object() && root.contains(kGeneratedByKey) &&
           root.at(kGeneratedByKey) == kGeneratedBy;
}

bool WriteOverlay(const std::string& serial) {
    if (serial.empty()) {
        return true;
    }

    const auto path = GetOverlayPath(serial);
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (exists && !IsGeneratedOverlay(path)) {
        LOG_WARNING(Config,
                    "Workarounds: {} was not written by the launcher, so the presets for {} are "
                    "not applied. Move it to {} to keep using it.",
                    path.string(), serial, (GetUserPresetDir() / (serial + ".json")).string());
        return false;
    }

    const auto layers = LoadLayers(serial);
    const auto merged = Merge(layers, layers.size());
    if (merged.empty()) {
        if (exists) {
            std::filesystem::remove(path, ec);
        }
        return true;
    }

    json root = json::object();
    root[kGeneratedByKey] = kGeneratedBy;
    root["_note"] = "Rewritten before every launch. Edit user/custom_configs/workarounds/" +
                    serial + ".json or the Workarounds settings tab instead.";
    json sources = json::array();
    for (const auto& layer : layers) {
        if (!layer.entries.empty()) {
            sources.push_back(layer.path.string());
        }
    }
    root["_sources"] = sources;
    for (const auto& [key, entry] : merged) {
        root[entry.section][key] = entry.value;
    }

    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        LOG_ERROR(Config, "Workarounds: failed to open {} for writing", path.string());
        return false;
    }
    out << std::setw(2) << root;
    LOG_INFO(Config, "Workarounds: applied {} keys to {} from {}", merged.size(), serial,
             sources.dump());
    return !out.fail();
}

bool SaveUserLayer(const std::filesystem::path& path, const Entries& entries) {
    std::error_code ec;
    if (entries.empty()) {
        std::filesystem::remove(path, ec);
        return !ec;
    }

    json root = json::object();
    // Keep handwritten notes while rebuilding the sections
    if (const json old = ReadJsonFile(path); old.is_object()) {
        for (const auto& [key, value] : old.items()) {
            if (!value.is_object()) {
                root[key] = value;
            }
        }
    }

    json workarounds = json::object();
    for (const auto& [key, entry] : entries) {
        // Keep unknown keys in their original sections so the emulator can load them
        if (FindKnownKey(key) || !EmulatorSettings.GetOverrideableKeySection(key).empty()) {
            workarounds[key] = entry.value;
        } else {
            root[entry.section][key] = entry.value;
        }
    }
    if (!workarounds.empty()) {
        root[kWorkaroundsSection] = workarounds;
    }

    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        LOG_ERROR(Config, "Workarounds: failed to open {} for writing", path.string());
        return false;
    }
    out << std::setw(2) << root;
    return !out.fail();
}

} // namespace Workarounds
