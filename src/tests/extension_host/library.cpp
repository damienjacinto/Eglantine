// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <eglantine/extension.h>

#include "extension_host/library.h"

namespace {

std::string logged;

const EglCoreApi test_core{
    .struct_size = sizeof(EglCoreApi),
    .log = [](void*, EglLogLevel, const char* message) { logged = message; },
    .get_extension_dir = nullptr,
    .get_data_dir = nullptr,
    .get_setting = nullptr,
    .set_setting = nullptr,
    .add_timer = nullptr,
    .remove_timer = nullptr,
    .post_to_ui = nullptr,
    .subscribe = nullptr,
    .unsubscribe = nullptr,
    .get_emulation_info = nullptr,
};

const void* GetTestInterface(void*, const char* name, uint32_t version) {
    if (version == 1 && std::strcmp(name, EGL_CORE_INTERFACE) == 0) {
        return &test_core;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Extension library loads and runs its entry points", "[extensions]") {
    ExtensionHost::Library library;
    const auto error = ExtensionHost::OpenLibrary(EGLANTINE_TEST_EXTENSION_PATH, library);
    INFO(error);
    REQUIRE(error.empty());
    REQUIRE(library.api_version == EGL_API_VERSION);
    REQUIRE(library.init != nullptr);
    REQUIRE(library.preinit == nullptr);
    REQUIRE(library.shutdown != nullptr);

    EglHost host{.struct_size = sizeof(EglHost),
                 .api_version = EGL_API_VERSION,
                 .context = nullptr,
                 .get_interface = &GetTestInterface};
    REQUIRE(library.init(&host) == 0);
    REQUIRE(logged == "test extension init");

    host.get_interface = [](void*, const char*, uint32_t) -> const void* { return nullptr; };
    REQUIRE(library.init(&host) == 7);
}

TEST_CASE("Extension library reports a missing library", "[extensions]") {
    ExtensionHost::Library library;
    REQUIRE(!ExtensionHost::OpenLibrary("/nonexistent/extension.so", library).empty());
    REQUIRE(library.handle == nullptr);
}

TEST_CASE("Extension library hash is SHA-256", "[extensions]") {
    const auto path = std::filesystem::temp_directory_path() / "eglantine-extension-hash-test";
    {
        std::ofstream file(path, std::ios::binary);
        file << "abc";
    }
    const auto hash = ExtensionHost::HashFile(path);
    std::filesystem::remove(path);
    REQUIRE(hash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(ExtensionHost::HashFile("/nonexistent/file").empty());
}
