// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <QPointer>

#include <eglantine/extension.h>

#include "common/common_types.h"
#include "common/param_package.h"
#include "extension_host/library.h"
#include "extension_host/manifest.h"
#include "extension_host/state.h"
#include "hid_core/frontend/emulated_controller.h"

class QAction;
class QLabel;
class QMenu;
class QObject;
class QStatusBar;
class QTimer;
class QWidget;

namespace Core {
class System;
}

namespace InputCommon {
class InputSubsystem;
}

namespace Extensions {

/// What the main window lends to extensions.
struct HostServices {
    QWidget* parent{};
    /// Indexed by EglMenu.
    std::array<QMenu*, 7> menus{};
    QStatusBar* status_bar{};
    InputCommon::InputSubsystem* input{};
    Core::System* system{};
};

enum class Status {
    Disabled,
    InvalidManifest,
    Incompatible,
    Changed,
    Duplicate,
    LoadFailed,
    InitFailed,
    Running,
    /// Enabled in this session, takes effect after a restart.
    PendingRestart,
};

/// One extension folder as the management dialog shows it.
struct ExtensionInfo {
    std::filesystem::path directory;
    ExtensionHost::Manifest manifest;
    Status status{};
    std::string error;
    bool enabled{};
};

struct Extension;

class ExtensionManager {
public:
    ExtensionManager();
    ~ExtensionManager();

    ExtensionManager(const ExtensionManager&) = delete;
    ExtensionManager& operator=(const ExtensionManager&) = delete;

    /**
     * Before the QApplication exists: discovers, loads enabled extensions and runs preinit.
     * @param exe_dir extensions in its "extensions" folder are found too, for development builds
     */
    void PreInitialize(const std::filesystem::path& exe_dir);

    /// Once the main window exists: runs init.
    void Initialize(const HostServices& services);

    /// When the application closes, before input is unloaded.
    void Shutdown();

    void OnEmulationStarted(u64 program_id, const std::string& title);
    void OnEmulationStopping();

    /// Hands players back to their saved settings while a controller configuration is open, so
    /// it neither shows nor saves extension assignments.
    void SuspendAssignments();
    void ResumeAssignments();

    [[nodiscard]] std::vector<ExtensionInfo> GetExtensions() const;

    /**
     * Enabling approves the library as it is now and starts it, disabling stops it. Extensions
     * with the early-start permission only start with the next launch.
     */
    void SetEnabled(const std::string& id, bool enabled);

    [[nodiscard]] std::filesystem::path GetInstallDirectory() const;

    // Called by the C entry points in host_api.cpp.
    const void* GetInterface(Extension& extension, const char* name, u32 version);
    void Log(Extension& extension, EglLogLevel level, const char* message);
    const char* GetDataDir(Extension& extension);
    s32 GetSetting(Extension& extension, const char* key, char* buffer, std::size_t size);
    void SetSetting(Extension& extension, const char* key, const char* value);
    u64 AddTimer(Extension& extension, u32 interval_ms, EglCallback callback, void* user);
    void RemoveTimer(Extension& extension, u64 id);
    void PostToUi(Extension& extension, EglCallback callback, void* user);
    u64 Subscribe(Extension& extension, EglEventType type, EglEventCallback callback, void* user);
    void Unsubscribe(Extension& extension, u64 id);
    s32 GetEmulationInfo(Extension& extension, EglEmulationInfo* info);

    u64 AddMenuItem(Extension& extension, EglMenu menu, const char* label, EglCallback on_click,
                    void* user);
    u64 AddMenuSeparator(Extension& extension, EglMenu menu);
    void SetMenuItemLabel(Extension& extension, u64 id, const char* label);
    void SetMenuItemEnabled(Extension& extension, u64 id, bool enabled);
    void SetMenuItemChecked(Extension& extension, u64 id, bool checked);
    void RemoveMenuItem(Extension& extension, u64 id);
    u64 AddStatusItem(Extension& extension, const char* text);
    void SetStatusItemText(Extension& extension, u64 id, const char* text);
    void RemoveStatusItem(Extension& extension, u64 id);
    void ShowMessage(Extension& extension, EglMessageKind kind, const char* title,
                     const char* text);
    bool AskQuestion(Extension& extension, const char* title, const char* text);
    bool PromptText(Extension& extension, const char* title, const char* label,
                    const char* initial, EglStringCallback on_accept, void* user);
    bool PickFile(Extension& extension, bool save, const char* title, const char* filter,
                  EglStringCallback on_accept, void* user);
    void SetClipboardText(Extension& extension, const char* text);
    void OpenUrl(Extension& extension, const char* url);

    u64 CreateDevice(Extension& extension, EglDeviceKind kind, const char* name);
    void DestroyDevice(Extension& extension, u64 handle);
    void SetButton(Extension& extension, u64 handle, EglButton button, bool pressed);
    void SetStick(Extension& extension, u64 handle, EglStick stick, float x, float y);
    void SetMotion(Extension& extension, u64 handle, float gyro_x, float gyro_y, float gyro_z,
                   float accel_x, float accel_y, float accel_z, u64 delta_us);
    void SetKey(Extension& extension, u64 handle, u32 usage, bool pressed);
    u32 ListDevices(Extension& extension, EglDeviceInfo* devices, u32 capacity);
    u32 GetPlayerCount(Extension& extension);
    s32 GetPlayerInfo(Extension& extension, u32 player, EglPlayerInfo* info);
    s32 AssignDevice(Extension& extension, const char* device_id, s32 player, const char* label);
    void ReleasePlayer(Extension& extension, s32 player);

private:
    enum class Phase {
        None,
        PreInit,
        Running,
        Closing,
    };

    struct MenuItem {
        Extension* owner;
        QPointer<QAction> action;
    };
    struct TimerItem {
        Extension* owner;
        QPointer<QTimer> timer;
    };
    struct StatusItem {
        Extension* owner;
        QPointer<QLabel> label;
    };
    struct Subscription {
        Extension* owner;
        EglEventType type;
        EglEventCallback callback;
        void* user;
    };
    struct Device {
        Extension* owner;
        std::size_t port;
        std::string id;
    };
    struct Assignment {
        Extension* owner;
        std::string device_id;
        std::string label;
        Core::HID::InputOverride input_override;
    };
    /// A device as list_devices reports it.
    struct DeviceEntry {
        std::string id;
        std::string name;
        std::string engine;
        u16 vendor_id{};
        u16 product_id{};
        Common::ParamPackage params;
    };
    struct PendingPost {
        Extension* owner;
        EglCallback callback;
        void* user;
    };

    [[nodiscard]] bool CanUseUi(const Extension& extension) const;
    [[nodiscard]] QMenu* GetMenu(EglMenu menu) const;
    [[nodiscard]] Core::HID::EmulatedController* GetController(std::size_t player) const;
    [[nodiscard]] std::vector<DeviceEntry> EnumerateDevices() const;
    [[nodiscard]] ExtensionHost::State::Entry& StateEntry(const std::string& id);
    void SaveState();

    void DispatchEvent(const EglEvent& event, Extension* only = nullptr);
    void StartDevicePolling();
    void PollDevices();
    bool ApplyAssignment(std::size_t player, Assignment& assignment);
    void EndAssignment(std::size_t player, bool notify);
    void RemoveRegistrations(Extension& extension);
    std::unique_ptr<Extension> OpenExtension(const ExtensionHost::Manifest& manifest, Status& status,
                                             std::string& error);
    bool StartExtension(Extension& extension);
    void StopExtension(Extension& extension);
    void Write(EglLogLevel level, std::string message);
    void FailExtension(Extension& extension, std::string error);

    Phase phase{Phase::None};
    std::vector<std::pair<EglLogLevel, std::string>> early_log;
    HostServices services;
    QObject* bridge{};
    QTimer* device_poll_timer{};
    std::vector<std::string> last_device_ids;
    bool assignments_suspended{};

    ExtensionHost::State state;
    std::filesystem::path state_path;
    std::vector<ExtensionInfo> infos;
    std::vector<std::unique_ptr<Extension>> extensions;

    u64 next_id{1};
    std::map<u64, MenuItem> menu_items;
    std::map<u64, TimerItem> timers;
    std::map<u64, StatusItem> status_items;
    std::map<u64, Subscription> subscriptions;
    std::map<u64, Device> devices;
    std::map<std::size_t, Assignment> assignments;

    u64 current_program_id{};
    std::string current_title;

    // post_to_ui is the one entry point called from other threads.
    std::mutex post_mutex;
    QObject* post_target{};
    bool posts_closed{};
    std::vector<PendingPost> pending_posts;
};

/// Per-extension state. Its EglHost::context points back to it.
struct Extension {
    ExtensionManager* manager{};
    ExtensionHost::Manifest manifest;
    ExtensionHost::Library library;
    EglHost host{};
    std::string directory_utf8;
    std::string data_dir_utf8;
    std::size_t info_index{};
    bool failed{};
    bool initialized{};
    /// Disabled by the user while the application runs.
    bool stopped{};
};

[[nodiscard]] inline bool IsActive(const Extension& extension) {
    return !extension.failed && !extension.stopped;
}

/// The host functions handed to extensions, defined in host_api.cpp.
const EglCoreApi& GetCoreApi();
const EglUiApi& GetUiApi();
const EglInputApi& GetInputApi();
const void* GetInterfaceThunk(void* context, const char* name, u32 version);

} // namespace Extensions
