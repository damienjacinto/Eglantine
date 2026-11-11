/* SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* Loaded by the extension_host tests. init asks the host for egl.core and logs through it. */

#include <eglantine/extension.h>

EGL_EXPORT uint32_t egl_extension_api_version(void) {
    return EGL_API_VERSION;
}

EGL_EXPORT int egl_extension_init(const EglHost* host) {
    const EglCoreApi* core = host->get_interface(host->context, EGL_CORE_INTERFACE, 1);
    if (core == NULL) {
        return 7;
    }
    core->log(host->context, EGL_LOG_INFO, "test extension init");
    return 0;
}

EGL_EXPORT void egl_extension_shutdown(void) {}
