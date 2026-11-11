# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(extension_host STATIC
    library.cpp
    library.h
    manifest.cpp
    manifest.h
    state.cpp
    state.h
)

create_target_directory_groups(extension_host)

target_link_libraries(extension_host
    PUBLIC common eglantine_extension_api
    PRIVATE nlohmann_json::nlohmann_json OpenSSL::Crypto fmt::fmt ${CMAKE_DL_LIBS})
