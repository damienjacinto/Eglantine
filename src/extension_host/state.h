// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace ExtensionHost {

/// What the user decided about each extension, and the settings extensions stored.
struct State {
    struct Entry {
        bool enabled{};
        /// SHA-256 of the library when the user enabled it.
        std::string approved_hash;
        std::map<std::string, std::string> settings;
    };

    std::map<std::string, Entry> extensions;

    /// Returns an empty state if the file is missing or unreadable.
    static State Load(const std::filesystem::path& path);

    /// Returns false if the file could not be written.
    bool Save(const std::filesystem::path& path) const;

    /// Serialization used by Load and Save.
    static State FromJson(std::string_view text);
    std::string ToJson() const;
};

} // namespace ExtensionHost
