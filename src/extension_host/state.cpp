// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

#include "extension_host/state.h"

namespace ExtensionHost {

State State::FromJson(std::string_view text) {
    State state;
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return state;
    }
    const auto extensions = json.find("extensions");
    if (extensions == json.end() || !extensions->is_object()) {
        return state;
    }
    for (const auto& [id, value] : extensions->items()) {
        if (!value.is_object()) {
            continue;
        }
        Entry entry;
        entry.enabled = value.value("enabled", false);
        entry.approved_hash = value.value("approved_hash", std::string{});
        if (const auto settings = value.find("settings");
            settings != value.end() && settings->is_object()) {
            for (const auto& [key, setting] : settings->items()) {
                if (setting.is_string()) {
                    entry.settings.emplace(key, setting.get<std::string>());
                }
            }
        }
        state.extensions.emplace(id, std::move(entry));
    }
    return state;
}

std::string State::ToJson() const {
    nlohmann::json extensions_json = nlohmann::json::object();
    for (const auto& [id, entry] : extensions) {
        nlohmann::json settings_json = nlohmann::json::object();
        for (const auto& [key, value] : entry.settings) {
            settings_json[key] = value;
        }
        extensions_json[id] = {
            {"enabled", entry.enabled},
            {"approved_hash", entry.approved_hash},
            {"settings", std::move(settings_json)},
        };
    }
    const nlohmann::json json = {{"extensions", std::move(extensions_json)}};
    return json.dump(2);
}

State State::Load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    return FromJson(text);
}

bool State::Save(const std::filesystem::path& path) const {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temp_path = std::filesystem::path{path}.concat(".tmp");
    {
        std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file << ToJson();
        if (!file) {
            return false;
        }
    }
    std::filesystem::rename(temp_path, path, ec);
    return !ec;
}

} // namespace ExtensionHost
