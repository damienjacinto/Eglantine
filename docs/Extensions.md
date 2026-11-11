# Extensions

Extensions are shared libraries that the Qt frontend loads at startup. They can add menu items
and dialogs, follow emulation events, create input devices and hand players to them, without
rebuilding Eglantine. The API is plain C, so an extension keeps working across Eglantine
updates and can be built with any compiler.

- Public header: [`src/extension_api/include/eglantine/extension.h`](../src/extension_api/include/eglantine/extension.h)
- Examples are kept outside the repository. Put them in an `extensions/` folder at the root of the
  source tree to build them with Eglantine.

## Installing and enabling

Each extension lives in its own folder inside the `extensions` folder of the data directory
(`%APPDATA%\eglantine\extensions`, `~/.local/share/eglantine/extensions`, or `user/extensions`
for portable builds):

```
extensions/
  dev.example.sample/
    extension.json
    sample.dll        (or sample.so / sample.dylib)
    any libraries it ships with
```

**Extensions > Manage Extensions...** lists what was found. A new extension is off until it is
ticked there. Enabling it records the SHA-256 of its library; if the file changes later, the
extension stays off until it is enabled again.

Ticking an extension starts it at once and unticking stops it: `egl_extension_shutdown` runs and
everything it added (menu items, timers, devices, player assignments) goes away. An extension with
the `early-start` permission only starts with the next launch, since part of it runs before the
main window. A library, once loaded, stays in memory until Eglantine exits, so an extension must
cope with `egl_extension_init` being called again after `egl_extension_shutdown`.

An extension runs inside Eglantine with the same rights as Eglantine. Nothing sandboxes it. Only
enable extensions you trust.

## The manifest

```json
{
    "id": "dev.example.sample",
    "name": "Sample",
    "version": "1.0.0",
    "author": "Someone",
    "description": "What it does, shown in the Extensions dialog.",
    "api_version": 1,
    "permissions": ["ui", "input"],
    "library": {"windows": "sample.dll", "linux": "sample.so", "macos": "sample.dylib"}
}
```

| Field | |
|---|---|
| `id` | Required. 1 to 64 characters among `a-z 0-9 . - _`. Settings and data are stored under it. |
| `api_version` | Required. The `EGL_API_VERSION` the extension was built against. Newer than the build: not loaded. |
| `library` | Required. A file name in the extension folder, or one per platform. |
| `permissions` | The interfaces the extension may fetch, see below. |
| `name`, `version`, `author`, `description` | Shown in the Extensions dialog. |

| Permission | Grants |
|---|---|
| `early-start` | `egl_extension_preinit` is called before the windowing, graphics and input subsystems start. |
| `ui` | The `egl.ui` interface: menus, status bar, dialogs, clipboard, opening web links. |
| `input` | The `egl.input` interface: extension input devices, the device list, player assignments. |

`egl.core` (logging, folders, settings, timers, events) is always available.

## Lifecycle

```c
uint32_t egl_extension_api_version(void);        /* required: return EGL_API_VERSION */
int      egl_extension_preinit(const EglHost*);  /* optional, needs early-start */
int      egl_extension_init(const EglHost*);     /* required, once the main window exists */
void     egl_extension_shutdown(void);           /* optional, when the application closes */
```

`preinit` and `init` return 0 on success. Any other value disables the extension and removes
everything it registered. Unticking and ticking it again in the Extensions dialog calls `init`
again (and `shutdown` before), without unloading the library. In `preinit` only `egl.core` logging, folders and settings
work; use it for what must happen first, such as setting environment variables read by SDL or
starting a library that hooks the graphics API.

Every call into the extension, and every host function except `post_to_ui`, runs on the UI
thread. An extension with its own threads hands work back with `post_to_ui`. Strings are UTF-8
and only valid during the call.

Functionality comes in interfaces:

```c
const EglUiApi* ui = host->get_interface(host->context, EGL_UI_INTERFACE, 1);
```

`get_interface` returns NULL for an unknown interface or version, one the manifest did not ask
for, or one not available in the current phase.

## Input and players

`egl.input` exposes Eglantine's concepts, not its internals:

- **Extension devices.** `create_device` makes a gamepad or keyboard that the extension drives with
  `set_button`, `set_stick`, `set_motion` and `set_key` (USB HID usages). They show up in the
  controller settings next to physical devices, and the device name identifies them across
  sessions, so a mapping the user saves for one keeps working.
- **The device list.** `list_devices` reports every device with an id, its backend and, for SDL
  devices, the USB vendor and product ids. `EGL_EVENT_INPUT_DEVICES_CHANGED` fires on hotplug.
- **Temporary player assignments.** `assign_device` maps and connects a player to a device using
  that device's default mapping. The assignment is layered over the saved controller settings and
  never written to them: it survives games booting and stopping, is taken down while a controller
  configuration dialog is open and put back after, and ends with `release_player`, when the device
  disconnects (the extension gets `EGL_EVENT_PLAYER_RELEASED`), or at shutdown. The player then
  returns to the user's own settings. Player `-1` picks the first player from Player 2 that is
  neither connected nor assigned.

## Versioning rules

- An interface struct only gains members at its end, and `struct_size` tells how much of it the
  host fills. Check it before calling a function added after version 1.
- A change that is not an addition gets a new interface version (`egl.ui` version 2) or a new
  interface. The host keeps serving the old one.
- Structs the extension passes in (`EglEmulationInfo`, `EglPlayerInfo`) carry `struct_size` too.
  Arrays of structs (`list_devices`) cannot grow; a later version would add a new function.

## Developing an extension

An extension needs only the header. Examples in an `extensions/` folder at the root of the
source tree are built with Eglantine (`EGLANTINE_BUILD_EXTENSIONS`, on by default) and copied
next to the executable, in
`<exe dir>/extensions/<id>/`, which is also searched. Two environment variables help:

| Variable | Effect |
|---|---|
| `EGLANTINE_EXTENSIONS_DIR` | More folders to search, separated by `;` on Windows and `:` elsewhere. |
| `EGLANTINE_EXTENSIONS_DEV=1` | Skip the hash check, so a rebuilt library loads without enabling it again. |

An extension still has to be enabled once. The state, including each extension's settings, is in
`extensions.json` in the config folder.

On Windows the loader looks for an extension's dependencies in its own folder first. On Linux and
macOS, link the extension with an `$ORIGIN` (or `@loader_path`) RPATH to get the same behaviour.
