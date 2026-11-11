// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "extension_host/manifest.h"

namespace ExtensionHost {

namespace {

constexpr std::array<std::pair<std::string_view, Permission>, 3> PermissionTable{{
    {"early-start", PermissionEarlyStart},
    {"ui", PermissionUi},
    {"input", PermissionInput},
}};

#if defined(_WIN32)
constexpr const char* PlatformKey = "windows";
#elif defined(__APPLE__)
constexpr const char* PlatformKey = "macos";
#else
constexpr const char* PlatformKey = "linux";
#endif

bool IsValidId(std::string_view id) {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    const auto valid_char = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
               c == '_';
    };
    return std::all_of(id.begin(), id.end(), valid_char) && id.front() != '.' &&
           id.front() != '-';
}

std::string GetString(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

} // namespace

std::string ParseManifest(std::string_view text, const std::filesystem::path& directory,
                          Manifest& out) {
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return "extension.json is not a JSON object";
    }

    Manifest manifest;
    manifest.directory = directory;
    manifest.id = GetString(json, "id");
    if (!IsValidId(manifest.id)) {
        return "\"id\" must be 1 to 64 characters among a-z, 0-9, '.', '-' and '_'";
    }
    manifest.name = GetString(json, "name");
    if (manifest.name.empty()) {
        manifest.name = manifest.id;
    }
    manifest.version = GetString(json, "version");
    manifest.author = GetString(json, "author");
    manifest.description = GetString(json, "description");

    const auto api = json.find("api_version");
    if (api == json.end() || !api->is_number_unsigned() || api->get<u64>() == 0) {
        return "\"api_version\" must be a positive integer";
    }
    manifest.api_version = static_cast<u32>(api->get<u64>());

    if (const auto permissions = json.find("permissions"); permissions != json.end()) {
        if (!permissions->is_array()) {
            return "\"permissions\" must be an array of strings";
        }
        for (const auto& permission : *permissions) {
            const auto name = permission.is_string() ? permission.get<std::string>() : "";
            const auto entry = std::find_if(PermissionTable.begin(), PermissionTable.end(),
                                            [&](const auto& e) { return e.first == name; });
            if (entry == PermissionTable.end()) {
                return "unknown permission \"" + name + "\"";
            }
            manifest.permissions |= entry->second;
        }
    }

    std::string library;
    if (const auto lib = json.find("library"); lib != json.end()) {
        if (lib->is_string()) {
            library = lib->get<std::string>();
        } else if (lib->is_object()) {
            library = GetString(*lib, PlatformKey);
        }
    }
    if (library.empty()) {
        return std::string{"no library for this platform (\"library\" or \"library\"."} +
               PlatformKey + ")";
    }
    const std::filesystem::path library_path{library};
    if (library_path.has_parent_path() || library_path.is_absolute() || library == "." ||
        library == "..") {
        return "\"library\" must be a file name inside the extension folder";
    }
    manifest.library = directory / library_path;

    out = std::move(manifest);
    return {};
}

std::vector<Discovered> Discover(const std::vector<std::filesystem::path>& roots) {
    std::vector<Discovered> result;
    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) {
            continue;
        }
        std::vector<std::filesystem::path> folders;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (entry.is_directory(ec) &&
                std::filesystem::is_regular_file(entry.path() / ManifestFileName, ec)) {
                folders.push_back(entry.path());
            }
        }
        std::sort(folders.begin(), folders.end());
        for (auto& folder : folders) {
            Discovered discovered{.directory = folder, .manifest = {}, .error = {}};
            std::ifstream file(folder / ManifestFileName, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(file),
                                   std::istreambuf_iterator<char>()};
            if (!file && !file.eof()) {
                discovered.error = "cannot read extension.json";
            } else {
                discovered.error = ParseManifest(text, folder, discovered.manifest);
            }
            result.push_back(std::move(discovered));
        }
    }
    return result;
}

std::vector<std::string> PermissionNames(u32 permissions) {
    std::vector<std::string> names;
    for (const auto& [name, permission] : PermissionTable) {
        if ((permissions & permission) != 0) {
            names.emplace_back(name);
        }
    }
    return names;
}

} // namespace ExtensionHost
