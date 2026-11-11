// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <fstream>
#include <memory>

#include <fmt/format.h>
#include <openssl/evp.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "extension_host/library.h"

namespace ExtensionHost {

namespace {

#ifdef _WIN32
std::string LastError() {
    return fmt::format("error {}", GetLastError());
}

void* Open(const std::filesystem::path& path) {
    return reinterpret_cast<void*>(LoadLibraryExW(
        path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
}

void* Symbol(void* handle, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
}
#else
std::string LastError() {
    const char* error = dlerror();
    return error != nullptr ? error : "unknown error";
}

void* Open(const std::filesystem::path& path) {
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}

void* Symbol(void* handle, const char* name) {
    return dlsym(handle, name);
}
#endif

template <typename T>
T Resolve(void* handle, const char* name) {
    return reinterpret_cast<T>(Symbol(handle, name));
}

} // namespace

std::string OpenLibrary(const std::filesystem::path& path, Library& out) {
    void* const handle = Open(path);
    if (handle == nullptr) {
        return "cannot load the library: " + LastError();
    }
    Library library{.handle = handle};
    const auto api_version = Resolve<Library::ApiVersionFn>(handle, "egl_extension_api_version");
    library.init = Resolve<Library::InitFn>(handle, "egl_extension_init");
    if (api_version == nullptr || library.init == nullptr) {
        // Deliberately not unloaded: a library's constructors may already have registered state.
        return "the library does not export egl_extension_api_version and egl_extension_init";
    }
    library.api_version = api_version();
    library.preinit = Resolve<Library::InitFn>(handle, "egl_extension_preinit");
    library.shutdown = Resolve<Library::ShutdownFn>(handle, "egl_extension_shutdown");
    out = library;
    return {};
}

std::string HashFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{EVP_MD_CTX_new(),
                                                                          &EVP_MD_CTX_free};
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return {};
    }
    std::array<char, 64 * 1024> buffer;
    while (file) {
        file.read(buffer.data(), buffer.size());
        const auto count = file.gcount();
        if (count > 0 && EVP_DigestUpdate(context.get(), buffer.data(),
                                          static_cast<std::size_t>(count)) != 1) {
            return {};
        }
    }
    if (!file.eof()) {
        return {};
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest;
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1) {
        return {};
    }
    std::string hex;
    hex.reserve(digest_size * 2);
    for (unsigned int i = 0; i < digest_size; ++i) {
        hex += fmt::format("{:02x}", digest[i]);
    }
    return hex;
}

} // namespace ExtensionHost
