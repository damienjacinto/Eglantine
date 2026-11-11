// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>
#include <random>

#include <catch2/catch_test_macros.hpp>

#include "extension_host/manifest.h"
#include "extension_host/state.h"

namespace {

using ExtensionHost::Manifest;
using ExtensionHost::ParseManifest;

const std::filesystem::path Folder{"/extensions/sample"};

std::string Parse(std::string_view json, Manifest& out) {
    return ParseManifest(json, Folder, out);
}

std::filesystem::path MakeTempDir() {
    std::mt19937_64 random{std::random_device{}()};
    auto path = std::filesystem::temp_directory_path() /
                ("eglantine-extension-test-" + std::to_string(random()));
    std::filesystem::create_directories(path);
    return path;
}

void WriteFile(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}

} // namespace

TEST_CASE("Extension manifest accepts a complete manifest", "[extensions]") {
    Manifest manifest;
    const auto error = Parse(R"({
        "id": "dev.example.sample",
        "name": "Sample",
        "version": "1.2.3",
        "author": "Someone",
        "description": "Does things",
        "api_version": 1,
        "permissions": ["ui", "input", "early-start"],
        "library": {"windows": "sample.dll", "linux": "sample.so", "macos": "sample.dylib"}
    })",
                             manifest);
    REQUIRE(error.empty());
    REQUIRE(manifest.id == "dev.example.sample");
    REQUIRE(manifest.name == "Sample");
    REQUIRE(manifest.version == "1.2.3");
    REQUIRE(manifest.api_version == 1);
    REQUIRE(manifest.permissions == (ExtensionHost::PermissionUi | ExtensionHost::PermissionInput |
                                     ExtensionHost::PermissionEarlyStart));
    REQUIRE(manifest.directory == Folder);
    REQUIRE(manifest.library.parent_path() == Folder);
#if defined(_WIN32)
    REQUIRE(manifest.library.filename() == "sample.dll");
#elif defined(__APPLE__)
    REQUIRE(manifest.library.filename() == "sample.dylib");
#else
    REQUIRE(manifest.library.filename() == "sample.so");
#endif
}

TEST_CASE("Extension manifest defaults the name and takes a plain library", "[extensions]") {
    Manifest manifest;
    REQUIRE(Parse(R"({"id": "a", "api_version": 1, "library": "a.so"})", manifest).empty());
    REQUIRE(manifest.name == "a");
    REQUIRE(manifest.permissions == 0);
    REQUIRE(manifest.library == Folder / "a.so");
}

TEST_CASE("Extension manifest rejects invalid fields", "[extensions]") {
    Manifest manifest;
    REQUIRE(!Parse("not json", manifest).empty());
    REQUIRE(!Parse("[]", manifest).empty());
    REQUIRE(!Parse(R"({"api_version": 1, "library": "a.so"})", manifest).empty());
    REQUIRE(!Parse(R"({"id": "Upper", "api_version": 1, "library": "a.so"})", manifest).empty());
    REQUIRE(!Parse(R"({"id": "-a", "api_version": 1, "library": "a.so"})", manifest).empty());
    REQUIRE(!Parse(R"({"id": "a/b", "api_version": 1, "library": "a.so"})", manifest).empty());
    REQUIRE(!Parse(R"({"id": "a", "library": "a.so"})", manifest).empty());
    REQUIRE(!Parse(R"({"id": "a", "api_version": 0, "library": "a.so"})", manifest).empty());
    REQUIRE(!Parse(R"({"id": "a", "api_version": -1, "library": "a.so"})", manifest).empty());
    REQUIRE(!Parse(R"({"id": "a", "api_version": 1})", manifest).empty());
    REQUIRE(!Parse(R"({"id": "a", "api_version": 1, "library": {"nowhere": "a.so"}})", manifest)
                 .empty());
    REQUIRE(!Parse(R"({"id": "a", "api_version": 1, "library": "a.so", "permissions": ["root"]})",
                   manifest)
                 .empty());
    REQUIRE(!Parse(R"({"id": "a", "api_version": 1, "library": "a.so", "permissions": "ui"})",
                   manifest)
                 .empty());
}

TEST_CASE("Extension manifest keeps the library inside the extension folder", "[extensions]") {
    Manifest manifest;
    REQUIRE(!Parse(R"({"id": "a", "api_version": 1, "library": "../a.so"})", manifest).empty());
    REQUIRE(!Parse(R"({"id": "a", "api_version": 1, "library": "sub/a.so"})", manifest).empty());
    REQUIRE(!Parse(R"({"id": "a", "api_version": 1, "library": "/usr/lib/a.so"})", manifest)
                 .empty());
    REQUIRE(!Parse(R"({"id": "a", "api_version": 1, "library": ".."})", manifest).empty());
}

TEST_CASE("Extension discovery lists folders with a manifest in order", "[extensions]") {
    const auto root = MakeTempDir();
    std::filesystem::create_directories(root / "b");
    std::filesystem::create_directories(root / "a");
    std::filesystem::create_directories(root / "no-manifest");
    WriteFile(root / "b" / "extension.json", R"({"id": "b", "api_version": 1, "library": "b.so"})");
    WriteFile(root / "a" / "extension.json", "{");

    const auto found = ExtensionHost::Discover({root, root / "missing"});
    std::filesystem::remove_all(root);

    REQUIRE(found.size() == 2);
    REQUIRE(found[0].directory.filename() == "a");
    REQUIRE(!found[0].error.empty());
    REQUIRE(found[1].directory.filename() == "b");
    REQUIRE(found[1].error.empty());
    REQUIRE(found[1].manifest.id == "b");
}

TEST_CASE("Extension state survives a save and load", "[extensions]") {
    ExtensionHost::State state;
    auto& entry = state.extensions["dev.example.sample"];
    entry.enabled = true;
    entry.approved_hash = "abcdef";
    entry.settings["server"] = "example.org";
    entry.settings["quote"] = "a \"quoted\" value";
    state.extensions["other"].enabled = false;

    const auto root = MakeTempDir();
    const auto path = root / "config" / "extensions.json";
    REQUIRE(state.Save(path));
    const auto loaded = ExtensionHost::State::Load(path);
    std::filesystem::remove_all(root);

    REQUIRE(loaded.extensions.size() == 2);
    const auto& sample = loaded.extensions.at("dev.example.sample");
    REQUIRE(sample.enabled);
    REQUIRE(sample.approved_hash == "abcdef");
    REQUIRE(sample.settings.at("server") == "example.org");
    REQUIRE(sample.settings.at("quote") == "a \"quoted\" value");
    REQUIRE(!loaded.extensions.at("other").enabled);
}

TEST_CASE("Extension state ignores unreadable files", "[extensions]") {
    REQUIRE(ExtensionHost::State::FromJson("{").extensions.empty());
    REQUIRE(ExtensionHost::State::FromJson(R"({"extensions": []})").extensions.empty());
    REQUIRE(ExtensionHost::State::Load("/nonexistent/extensions.json").extensions.empty());
}
