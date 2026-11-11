// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct EglHost;

namespace ExtensionHost {

/// The entry points of a loaded extension library. The library is never unloaded.
struct Library {
    using ApiVersionFn = std::uint32_t (*)();
    using InitFn = int (*)(const EglHost*);
    using ShutdownFn = void (*)();

    void* handle{};
    std::uint32_t api_version{};
    InitFn preinit{};
    InitFn init{};
    ShutdownFn shutdown{};
};

/**
 * Loads the library and resolves its entry points. Libraries next to it are found first, so an
 * extension can ship its own dependencies.
 * @returns an empty string on success, otherwise the error.
 */
std::string OpenLibrary(const std::filesystem::path& path, Library& out);

/// Returns the lowercase hex SHA-256 of the file, or an empty string if it cannot be read.
std::string HashFile(const std::filesystem::path& path);

} // namespace ExtensionHost
