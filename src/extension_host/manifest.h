// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace ExtensionHost {

enum Permission : u32 {
    PermissionEarlyStart = 1u << 0,
    PermissionUi = 1u << 1,
    PermissionInput = 1u << 2,
};

struct Manifest {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    u32 api_version{};
    u32 permissions{};
    std::filesystem::path directory;
    std::filesystem::path library;
};

/// An extension folder, with either a valid manifest or the reason it has none.
struct Discovered {
    std::filesystem::path directory;
    Manifest manifest;
    std::string error;
};

/// The name of the manifest file in every extension folder.
inline constexpr std::string_view ManifestFileName = "extension.json";

/**
 * Parses an extension.json.
 * @param directory the folder the manifest is in. The library must be a file directly inside it.
 * @returns an empty string on success, otherwise the error.
 */
std::string ParseManifest(std::string_view json, const std::filesystem::path& directory,
                          Manifest& out);

/// Lists every folder holding an extension.json in the given roots, in a stable order.
std::vector<Discovered> Discover(const std::vector<std::filesystem::path>& roots);

/// Returns the manifest spelling of each permission set in the mask.
std::vector<std::string> PermissionNames(u32 permissions);

} // namespace ExtensionHost
