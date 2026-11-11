// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/extensions/extension_manager.h"

namespace Extensions {

namespace {

Extension& Ext(void* context) {
    return *static_cast<Extension*>(context);
}

ExtensionManager& Manager(void* context) {
    return *Ext(context).manager;
}

const EglCoreApi core_api{
    .struct_size = sizeof(EglCoreApi),
    .log = [](void* c, EglLogLevel level,
              const char* message) { Manager(c).Log(Ext(c), level, message); },
    .get_extension_dir = [](void* c) { return Ext(c).directory_utf8.c_str(); },
    .get_data_dir = [](void* c) { return Manager(c).GetDataDir(Ext(c)); },
    .get_setting = [](void* c, const char* key, char* buffer,
                      size_t size) { return Manager(c).GetSetting(Ext(c), key, buffer, size); },
    .set_setting = [](void* c, const char* key,
                      const char* value) { Manager(c).SetSetting(Ext(c), key, value); },
    .add_timer = [](void* c, uint32_t interval, EglCallback callback,
                    void* user) { return Manager(c).AddTimer(Ext(c), interval, callback, user); },
    .remove_timer = [](void* c, uint64_t id) { Manager(c).RemoveTimer(Ext(c), id); },
    .post_to_ui = [](void* c, EglCallback callback,
                     void* user) { Manager(c).PostToUi(Ext(c), callback, user); },
    .subscribe = [](void* c, EglEventType type, EglEventCallback callback,
                    void* user) { return Manager(c).Subscribe(Ext(c), type, callback, user); },
    .unsubscribe = [](void* c, uint64_t id) { Manager(c).Unsubscribe(Ext(c), id); },
    .get_emulation_info = [](void* c,
                             EglEmulationInfo* info) { return Manager(c).GetEmulationInfo(Ext(c), info); },
};

const EglUiApi ui_api{
    .struct_size = sizeof(EglUiApi),
    .add_menu_item =
        [](void* c, EglMenu menu, const char* label, EglCallback on_click, void* user) {
            return Manager(c).AddMenuItem(Ext(c), menu, label, on_click, user);
        },
    .add_menu_separator = [](void* c,
                             EglMenu menu) { return Manager(c).AddMenuSeparator(Ext(c), menu); },
    .set_menu_item_label = [](void* c, uint64_t id,
                              const char* label) { Manager(c).SetMenuItemLabel(Ext(c), id, label); },
    .set_menu_item_enabled =
        [](void* c, uint64_t id, int32_t enabled) {
            Manager(c).SetMenuItemEnabled(Ext(c), id, enabled != 0);
        },
    .set_menu_item_checked =
        [](void* c, uint64_t id, int32_t checked) {
            Manager(c).SetMenuItemChecked(Ext(c), id, checked != 0);
        },
    .remove_menu_item = [](void* c, uint64_t id) { Manager(c).RemoveMenuItem(Ext(c), id); },
    .add_status_item = [](void* c,
                          const char* text) { return Manager(c).AddStatusItem(Ext(c), text); },
    .set_status_item_text = [](void* c, uint64_t id,
                               const char* text) { Manager(c).SetStatusItemText(Ext(c), id, text); },
    .remove_status_item = [](void* c, uint64_t id) { Manager(c).RemoveStatusItem(Ext(c), id); },
    .show_message =
        [](void* c, EglMessageKind kind, const char* title, const char* text) {
            Manager(c).ShowMessage(Ext(c), kind, title, text);
        },
    .ask_question =
        [](void* c, const char* title, const char* text) -> int32_t {
            return Manager(c).AskQuestion(Ext(c), title, text) ? 1 : 0;
        },
    .prompt_text =
        [](void* c, const char* title, const char* label, const char* initial,
           EglStringCallback on_accept, void* user) -> int32_t {
            return Manager(c).PromptText(Ext(c), title, label, initial, on_accept, user) ? 1 : 0;
        },
    .pick_open_file =
        [](void* c, const char* title, const char* filter, EglStringCallback on_accept,
           void* user) -> int32_t {
            return Manager(c).PickFile(Ext(c), false, title, filter, on_accept, user) ? 1 : 0;
        },
    .pick_save_file =
        [](void* c, const char* title, const char* filter, EglStringCallback on_accept,
           void* user) -> int32_t {
            return Manager(c).PickFile(Ext(c), true, title, filter, on_accept, user) ? 1 : 0;
        },
    .set_clipboard_text = [](void* c,
                             const char* text) { Manager(c).SetClipboardText(Ext(c), text); },
    .open_url = [](void* c, const char* url) { Manager(c).OpenUrl(Ext(c), url); },
};

const EglInputApi input_api{
    .struct_size = sizeof(EglInputApi),
    .create_device =
        [](void* c, EglDeviceKind kind, const char* name) {
            return Manager(c).CreateDevice(Ext(c), kind, name);
        },
    .destroy_device = [](void* c, uint64_t device) { Manager(c).DestroyDevice(Ext(c), device); },
    .set_button =
        [](void* c, uint64_t device, EglButton button, int32_t pressed) {
            Manager(c).SetButton(Ext(c), device, button, pressed != 0);
        },
    .set_stick =
        [](void* c, uint64_t device, EglStick stick, float x, float y) {
            Manager(c).SetStick(Ext(c), device, stick, x, y);
        },
    .set_motion =
        [](void* c, uint64_t device, float gx, float gy, float gz, float ax, float ay, float az,
           uint64_t delta_us) {
            Manager(c).SetMotion(Ext(c), device, gx, gy, gz, ax, ay, az, delta_us);
        },
    .set_key =
        [](void* c, uint64_t device, uint32_t usage, int32_t pressed) {
            Manager(c).SetKey(Ext(c), device, usage, pressed != 0);
        },
    .list_devices =
        [](void* c, EglDeviceInfo* devices, uint32_t capacity) {
            return Manager(c).ListDevices(Ext(c), devices, capacity);
        },
    .get_player_count = [](void* c) { return Manager(c).GetPlayerCount(Ext(c)); },
    .get_player_info =
        [](void* c, uint32_t player, EglPlayerInfo* info) {
            return Manager(c).GetPlayerInfo(Ext(c), player, info);
        },
    .assign_device =
        [](void* c, const char* device_id, int32_t player, const char* label) {
            return Manager(c).AssignDevice(Ext(c), device_id, player, label);
        },
    .release_player = [](void* c, int32_t player) { Manager(c).ReleasePlayer(Ext(c), player); },
};

} // namespace

const EglCoreApi& GetCoreApi() {
    return core_api;
}

const EglUiApi& GetUiApi() {
    return ui_api;
}

const EglInputApi& GetInputApi() {
    return input_api;
}

const void* GetInterfaceThunk(void* context, const char* name, u32 version) {
    return Manager(context).GetInterface(Ext(context), name, version);
}

} // namespace Extensions
