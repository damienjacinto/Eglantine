# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Header only. Extensions need nothing else from the tree.
add_library(eglantine_extension_api INTERFACE)
target_include_directories(eglantine_extension_api INTERFACE include)
