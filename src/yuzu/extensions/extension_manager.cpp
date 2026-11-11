// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <system_error>
#include <tuple>

#include <QAction>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileDialog>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings_input.h"
#include "core/core.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/extension_input.h"
#include "input_common/main.h"
#include "yuzu/extensions/extension_manager.h"

namespace Extensions {

namespace {

constexpr std::size_t PlayerCount = 8;
constexpr int DevicePollIntervalMs = 500;

#ifdef _WIN32
constexpr char PathListSeparator = ';';
#else
constexpr char PathListSeparator = ':';
#endif

std::vector<std::filesystem::path> SearchRoots(const std::filesystem::path& exe_dir) {
    std::vector<std::filesystem::path> roots{
        Common::FS::GetEdenPath(Common::FS::EdenPath::ExtensionsDir)};
    if (!exe_dir.empty()) {
        roots.push_back(exe_dir / "extensions");
    }
    if (const char* extra = std::getenv("EGLANTINE_EXTENSIONS_DIR"); extra != nullptr) {
        std::string_view list{extra};
        while (!list.empty()) {
            const auto end = list.find(PathListSeparator);
            const auto item = list.substr(0, end);
            if (!item.empty()) {
                roots.emplace_back(std::string{item});
            }
            if (end == std::string_view::npos) {
                break;
            }
            list.remove_prefix(end + 1);
        }
    }
    return roots;
}

bool IsDevMode() {
    const char* value = std::getenv("EGLANTINE_EXTENSIONS_DEV");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

std::string MakeDeviceId(const Common::ParamPackage& params) {
    auto id = fmt::format("{}:{}:{}", params.Get("engine", ""), params.Get("guid", ""),
                          params.Get("port", 0));
    if (params.Has("guid2")) {
        id += ':' + params.Get("guid2", "");
    }
    return id;
}

void CopyString(char* out, std::size_t size, std::string_view value) {
    if (size == 0) {
        return;
    }
    const auto count = std::min(size - 1, value.size());
    std::memcpy(out, value.data(), count);
    out[count] = '\0';
}

template <std::size_t N>
void CopyString(char (&out)[N], std::string_view value) {
    CopyString(out, N, value);
}

QString FromUtf8(const char* text) {
    return text != nullptr ? QString::fromUtf8(text) : QString{};
}

/// SDL GUIDs carry the USB vendor and product ids little endian at bytes 4 and 8.
std::pair<u16, u16> SdlUsbIds(const std::string& guid) {
    if (guid.size() != 32) {
        return {};
    }
    const auto byte = [&](std::size_t index) {
        return static_cast<u16>(std::strtoul(guid.substr(index * 2, 2).c_str(), nullptr, 16));
    };
    return {static_cast<u16>(byte(4) | (byte(5) << 8)), static_cast<u16>(byte(8) | (byte(9) << 8))};
}

} // namespace

ExtensionManager::ExtensionManager() = default;
ExtensionManager::~ExtensionManager() = default;

std::filesystem::path ExtensionManager::GetInstallDirectory() const {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ExtensionsDir);
}

void ExtensionManager::PreInitialize(const std::filesystem::path& exe_dir) {
    state_path = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "extensions.json";
    state = ExtensionHost::State::Load(state_path);
    const bool dev_mode = IsDevMode();
    if (dev_mode) {
        Write(EGL_LOG_WARNING, "EGLANTINE_EXTENSIONS_DEV is set: extension hashes are not checked");
    }

    std::set<std::string> seen;
    for (auto& discovered : ExtensionHost::Discover(SearchRoots(exe_dir))) {
        ExtensionInfo info{.directory = discovered.directory,
                           .manifest = discovered.manifest,
                           .status = Status::Disabled,
                           .error = {},
                           .enabled = false};
        const auto finish = [&](Status status, std::string error = {}) {
            info.status = status;
            info.error = std::move(error);
            if (!info.error.empty()) {
                Write(EGL_LOG_WARNING,
                      fmt::format("Extension {}: {}", Common::FS::PathToUTF8String(info.directory),
                                  info.error));
            }
            infos.push_back(std::move(info));
        };

        if (!discovered.error.empty()) {
            finish(Status::InvalidManifest, std::move(discovered.error));
            continue;
        }
        const auto& manifest = info.manifest;
        if (!seen.insert(manifest.id).second) {
            finish(Status::Duplicate, "an extension with the same id was found first");
            continue;
        }
        const auto entry = state.extensions.find(manifest.id);
        info.enabled = entry != state.extensions.end() && entry->second.enabled;
        if (!info.enabled) {
            finish(Status::Disabled);
            continue;
        }
        if (manifest.api_version > EGL_API_VERSION) {
            finish(Status::Incompatible,
                   fmt::format("needs extension API {}, this build provides {}",
                               manifest.api_version, EGL_API_VERSION));
            continue;
        }
        const auto hash = ExtensionHost::HashFile(manifest.library);
        if (hash.empty()) {
            finish(Status::LoadFailed, "cannot read " +
                                           Common::FS::PathToUTF8String(manifest.library));
            continue;
        }
        if (hash != entry->second.approved_hash && !dev_mode) {
            finish(Status::Changed, "the library changed since it was enabled");
            continue;
        }

        Status status{};
        std::string error;
        auto extension = OpenExtension(manifest, status, error);
        if (!extension) {
            finish(status, std::move(error));
            continue;
        }
        extension->info_index = infos.size();
        finish(Status::Running);
        extensions.push_back(std::move(extension));
    }

    phase = Phase::PreInit;
    for (auto& extension : extensions) {
        if (extension->library.preinit == nullptr) {
            continue;
        }
        if ((extension->manifest.permissions & ExtensionHost::PermissionEarlyStart) == 0) {
            Write(EGL_LOG_WARNING,
                  fmt::format("Extension {} exports egl_extension_preinit without the "
                              "early-start permission, not calling it",
                              extension->manifest.id));
            continue;
        }
        if (const int result = extension->library.preinit(&extension->host); result != 0) {
            FailExtension(*extension, fmt::format("egl_extension_preinit returned {}", result));
        }
    }
    Write(EGL_LOG_INFO,
          fmt::format("Extensions: {} found, {} loaded", infos.size(), extensions.size()));
}

std::unique_ptr<Extension> ExtensionManager::OpenExtension(const ExtensionHost::Manifest& manifest,
                                                           Status& status, std::string& error) {
    auto extension = std::make_unique<Extension>();
    extension->manager = this;
    extension->manifest = manifest;
    extension->directory_utf8 = Common::FS::PathToUTF8String(manifest.directory);
    if (error = ExtensionHost::OpenLibrary(manifest.library, extension->library); !error.empty()) {
        status = Status::LoadFailed;
        return nullptr;
    }
    const auto library_api = extension->library.api_version;
    if (library_api == 0 || library_api > EGL_API_VERSION) {
        status = Status::Incompatible;
        error = fmt::format("the library uses extension API {}, this build provides {}",
                            library_api, EGL_API_VERSION);
        return nullptr;
    }
    extension->host = EglHost{
        .struct_size = sizeof(EglHost),
        .api_version = EGL_API_VERSION,
        .context = extension.get(),
        .get_interface = &GetInterfaceThunk,
    };
    return extension;
}

bool ExtensionManager::StartExtension(Extension& extension) {
    extension.failed = false;
    extension.stopped = false;
    if (const int result = extension.library.init(&extension.host); result != 0) {
        FailExtension(extension, fmt::format("egl_extension_init returned {}", result));
        return false;
    }
    extension.initialized = true;
    auto& info = infos[extension.info_index];
    info.status = Status::Running;
    info.error.clear();
    Write(EGL_LOG_INFO, fmt::format("Extension {} {} started", extension.manifest.id,
                                    extension.manifest.version));
    return true;
}

void ExtensionManager::StopExtension(Extension& extension) {
    if (extension.initialized && IsActive(extension) && extension.library.shutdown != nullptr) {
        extension.library.shutdown();
    }
    extension.stopped = true;
    extension.initialized = false;
    RemoveRegistrations(extension);
    Write(EGL_LOG_INFO, fmt::format("Extension {} stopped", extension.manifest.id));
}

void ExtensionManager::Initialize(const HostServices& services_) {
    services = services_;
    bridge = new QObject(services.parent);
    phase = Phase::Running;
    for (auto& [level, message] : early_log) {
        Write(level, std::move(message));
    }
    early_log.clear();

    for (auto& extension : extensions) {
        if (!extension->failed) {
            StartExtension(*extension);
        }
    }

    std::vector<PendingPost> posts;
    {
        std::scoped_lock lock{post_mutex};
        post_target = bridge;
        posts.swap(pending_posts);
    }
    for (const auto& post : posts) {
        PostToUi(*post.owner, post.callback, post.user);
    }
    const bool wants_devices =
        std::any_of(subscriptions.begin(), subscriptions.end(), [](const auto& item) {
            return item.second.type == EGL_EVENT_INPUT_DEVICES_CHANGED;
        });
    if (wants_devices) {
        StartDevicePolling();
    }
}

void ExtensionManager::Shutdown() {
    if (phase == Phase::Closing) {
        return;
    }
    {
        std::scoped_lock lock{post_mutex};
        posts_closed = true;
        post_target = nullptr;
        pending_posts.clear();
    }
    if (phase == Phase::Running) {
        DispatchEvent(EglEvent{.struct_size = sizeof(EglEvent),
                               .type = EGL_EVENT_APP_CLOSING,
                               .program_id = current_program_id,
                               .title = current_title.c_str(),
                               .player = -1});
    }
    phase = Phase::Closing;
    for (auto it = extensions.rbegin(); it != extensions.rend(); ++it) {
        auto& extension = **it;
        if (extension.initialized && IsActive(extension) && extension.library.shutdown != nullptr) {
            extension.library.shutdown();
        }
    }
    for (auto& extension : extensions) {
        RemoveRegistrations(*extension);
    }
    if (device_poll_timer != nullptr) {
        device_poll_timer->stop();
    }
    delete bridge;
    bridge = nullptr;
    device_poll_timer = nullptr;
    SaveState();
}

void ExtensionManager::OnEmulationStarted(u64 program_id, const std::string& title) {
    current_program_id = program_id;
    current_title = title;
    DispatchEvent(EglEvent{.struct_size = sizeof(EglEvent),
                           .type = EGL_EVENT_EMULATION_STARTED,
                           .program_id = program_id,
                           .title = current_title.c_str(),
                           .player = -1});
}

void ExtensionManager::OnEmulationStopping() {
    DispatchEvent(EglEvent{.struct_size = sizeof(EglEvent),
                           .type = EGL_EVENT_EMULATION_STOPPING,
                           .program_id = current_program_id,
                           .title = current_title.c_str(),
                           .player = -1});
    current_program_id = 0;
    current_title.clear();
}

void ExtensionManager::SuspendAssignments() {
    if (assignments_suspended) {
        return;
    }
    assignments_suspended = true;
    for (const auto& [player, assignment] : assignments) {
        if (auto* controller = GetController(player)) {
            controller->SetInputOverride(std::nullopt);
        }
    }
}

void ExtensionManager::ResumeAssignments() {
    if (!assignments_suspended) {
        return;
    }
    assignments_suspended = false;
    const auto present = EnumerateDevices();
    std::vector<std::size_t> ended;
    for (auto& [player, assignment] : assignments) {
        const bool connected = std::any_of(present.begin(), present.end(), [&](const auto& d) {
            return d.id == assignment.device_id;
        });
        if (!connected || !ApplyAssignment(player, assignment)) {
            ended.push_back(player);
        }
    }
    for (const auto player : ended) {
        EndAssignment(player, true);
    }
}

std::vector<ExtensionInfo> ExtensionManager::GetExtensions() const {
    return infos;
}

void ExtensionManager::SetEnabled(const std::string& id, bool enabled) {
    const auto info_it = std::find_if(infos.begin(), infos.end(), [&](const ExtensionInfo& info) {
        return info.manifest.id == id && info.status != Status::InvalidManifest &&
               info.status != Status::Duplicate;
    });
    if (info_it == infos.end()) {
        return;
    }
    const auto info_index = static_cast<std::size_t>(info_it - infos.begin());
    auto& entry = StateEntry(id);
    entry.enabled = enabled;
    if (enabled) {
        entry.approved_hash = ExtensionHost::HashFile(info_it->manifest.library);
    }
    info_it->enabled = enabled;
    SaveState();

    const auto loaded = std::find_if(extensions.begin(), extensions.end(), [&](const auto& ext) {
        return ext->info_index == info_index;
    });
    Extension* extension = loaded != extensions.end() ? loaded->get() : nullptr;
    const auto set_status = [&](Status status, std::string error = {}) {
        infos[info_index].status = status;
        infos[info_index].error = std::move(error);
    };

    if (phase != Phase::Running) {
        set_status(Status::PendingRestart);
        return;
    }
    if (!enabled) {
        if (extension != nullptr && IsActive(*extension)) {
            StopExtension(*extension);
        }
        set_status(Status::Disabled);
        return;
    }
    // What must run before the main window can only start with the next launch.
    if ((info_it->manifest.permissions & ExtensionHost::PermissionEarlyStart) != 0 &&
        (extension == nullptr || !IsActive(*extension))) {
        set_status(Status::PendingRestart);
        return;
    }
    if (extension != nullptr && IsActive(*extension)) {
        return;
    }
    if (extension == nullptr) {
        const auto manifest = info_it->manifest;
        if (manifest.api_version > EGL_API_VERSION) {
            set_status(Status::Incompatible,
                       fmt::format("needs extension API {}, this build provides {}",
                                   manifest.api_version, EGL_API_VERSION));
            return;
        }
        Status status{};
        std::string error;
        auto opened = OpenExtension(manifest, status, error);
        if (!opened) {
            set_status(status, std::move(error));
            return;
        }
        opened->info_index = info_index;
        extension = opened.get();
        extensions.push_back(std::move(opened));
    }
    StartExtension(*extension);
}

const void* ExtensionManager::GetInterface(Extension& extension, const char* name, u32 version) {
    if (name == nullptr || version != 1 || !IsActive(extension)) {
        return nullptr;
    }
    const std::string_view interface_name{name};
    if (interface_name == EGL_CORE_INTERFACE) {
        return &GetCoreApi();
    }
    if (phase != Phase::Running) {
        return nullptr;
    }
    const auto permissions = extension.manifest.permissions;
    if (interface_name == EGL_UI_INTERFACE && (permissions & ExtensionHost::PermissionUi) != 0) {
        return &GetUiApi();
    }
    if (interface_name == EGL_INPUT_INTERFACE &&
        (permissions & ExtensionHost::PermissionInput) != 0) {
        return &GetInputApi();
    }
    return nullptr;
}

void ExtensionManager::Log(Extension& extension, EglLogLevel level, const char* message) {
    Write(level, fmt::format("[{}] {}", extension.manifest.id, message != nullptr ? message : ""));
}

void ExtensionManager::Write(EglLogLevel level, std::string message) {
    // Logging starts with the main window; keep what comes before it.
    if (phase == Phase::None || phase == Phase::PreInit) {
        early_log.emplace_back(level, std::move(message));
        return;
    }
    switch (level) {
    case EGL_LOG_DEBUG:
        LOG_DEBUG(Frontend, "{}", message);
        break;
    case EGL_LOG_WARNING:
        LOG_WARNING(Frontend, "{}", message);
        break;
    case EGL_LOG_ERROR:
        LOG_ERROR(Frontend, "{}", message);
        break;
    case EGL_LOG_INFO:
    default:
        LOG_INFO(Frontend, "{}", message);
        break;
    }
}

const char* ExtensionManager::GetDataDir(Extension& extension) {
    if (extension.data_dir_utf8.empty()) {
        const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) /
                          "extension-data" / extension.manifest.id;
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        extension.data_dir_utf8 = Common::FS::PathToUTF8String(path);
    }
    return extension.data_dir_utf8.c_str();
}

s32 ExtensionManager::GetSetting(Extension& extension, const char* key, char* buffer,
                                 std::size_t size) {
    if (key == nullptr) {
        return -1;
    }
    const auto entry = state.extensions.find(extension.manifest.id);
    if (entry == state.extensions.end()) {
        return -1;
    }
    const auto setting = entry->second.settings.find(key);
    if (setting == entry->second.settings.end()) {
        return -1;
    }
    if (buffer != nullptr) {
        CopyString(buffer, size, setting->second);
    }
    return static_cast<s32>(setting->second.size());
}

void ExtensionManager::SetSetting(Extension& extension, const char* key, const char* value) {
    if (key == nullptr) {
        return;
    }
    auto& settings = StateEntry(extension.manifest.id).settings;
    if (value == nullptr) {
        settings.erase(key);
    } else {
        settings.insert_or_assign(key, value);
    }
    SaveState();
}

u64 ExtensionManager::AddTimer(Extension& extension, u32 interval_ms, EglCallback callback,
                               void* user) {
    if (!CanUseUi(extension) || callback == nullptr) {
        return 0;
    }
    const u64 id = next_id++;
    auto* timer = new QTimer(bridge);
    timer->setInterval(static_cast<int>(std::max<u32>(interval_ms, 1)));
    QObject::connect(timer, &QTimer::timeout, bridge, [this, id, callback, user] {
        if (timers.contains(id)) {
            callback(user);
        }
    });
    timer->start();
    timers.emplace(id, TimerItem{.owner = &extension, .timer = timer});
    return id;
}

void ExtensionManager::RemoveTimer(Extension& extension, u64 id) {
    const auto it = timers.find(id);
    if (it == timers.end() || it->second.owner != &extension) {
        return;
    }
    if (it->second.timer) {
        it->second.timer->stop();
        it->second.timer->deleteLater();
    }
    timers.erase(it);
}

void ExtensionManager::PostToUi(Extension& extension, EglCallback callback, void* user) {
    if (callback == nullptr) {
        return;
    }
    std::scoped_lock lock{post_mutex};
    if (posts_closed) {
        return;
    }
    if (post_target == nullptr) {
        pending_posts.push_back(PendingPost{.owner = &extension, .callback = callback, .user = user});
        return;
    }
    Extension* owner = &extension;
    QMetaObject::invokeMethod(
        post_target,
        [this, owner, callback, user] {
            if (phase == Phase::Running && IsActive(*owner)) {
                callback(user);
            }
        },
        Qt::QueuedConnection);
}

u64 ExtensionManager::Subscribe(Extension& extension, EglEventType type, EglEventCallback callback,
                                void* user) {
    if (callback == nullptr || phase == Phase::Closing || !IsActive(extension)) {
        return 0;
    }
    const u64 id = next_id++;
    subscriptions.emplace(
        id, Subscription{.owner = &extension, .type = type, .callback = callback, .user = user});
    if (type == EGL_EVENT_INPUT_DEVICES_CHANGED) {
        StartDevicePolling();
    }
    return id;
}

void ExtensionManager::Unsubscribe(Extension& extension, u64 id) {
    const auto it = subscriptions.find(id);
    if (it != subscriptions.end() && it->second.owner == &extension) {
        subscriptions.erase(it);
    }
}

s32 ExtensionManager::GetEmulationInfo(Extension& extension, EglEmulationInfo* info) {
    if (info == nullptr || info->struct_size < sizeof(EglEmulationInfo)) {
        return -1;
    }
    info->state = EGL_EMULATION_STOPPED;
    info->program_id = 0;
    CopyString(info->title, "");
    if (services.system != nullptr && services.system->IsPoweredOn() && current_program_id != 0) {
        info->state =
            services.system->IsPaused() ? EGL_EMULATION_PAUSED : EGL_EMULATION_RUNNING;
        info->program_id = current_program_id;
        CopyString(info->title, current_title);
    }
    return 0;
}

u64 ExtensionManager::AddMenuItem(Extension& extension, EglMenu menu, const char* label,
                                  EglCallback on_click, void* user) {
    QMenu* const target = CanUseUi(extension) ? GetMenu(menu) : nullptr;
    if (target == nullptr) {
        return 0;
    }
    const u64 id = next_id++;
    QAction* action = target->addAction(FromUtf8(label));
    QObject::connect(action, &QAction::triggered, bridge, [this, id, on_click, user] {
        if (menu_items.contains(id) && on_click != nullptr) {
            on_click(user);
        }
    });
    menu_items.emplace(id, MenuItem{.owner = &extension, .action = action});
    return id;
}

u64 ExtensionManager::AddMenuSeparator(Extension& extension, EglMenu menu) {
    QMenu* const target = CanUseUi(extension) ? GetMenu(menu) : nullptr;
    if (target == nullptr) {
        return 0;
    }
    const u64 id = next_id++;
    menu_items.emplace(id, MenuItem{.owner = &extension, .action = target->addSeparator()});
    return id;
}

void ExtensionManager::SetMenuItemLabel(Extension& extension, u64 id, const char* label) {
    const auto it = menu_items.find(id);
    if (it != menu_items.end() && it->second.owner == &extension && it->second.action) {
        it->second.action->setText(FromUtf8(label));
    }
}

void ExtensionManager::SetMenuItemEnabled(Extension& extension, u64 id, bool enabled) {
    const auto it = menu_items.find(id);
    if (it != menu_items.end() && it->second.owner == &extension && it->second.action) {
        it->second.action->setEnabled(enabled);
    }
}

void ExtensionManager::SetMenuItemChecked(Extension& extension, u64 id, bool checked) {
    const auto it = menu_items.find(id);
    if (it != menu_items.end() && it->second.owner == &extension && it->second.action) {
        it->second.action->setCheckable(true);
        it->second.action->setChecked(checked);
    }
}

void ExtensionManager::RemoveMenuItem(Extension& extension, u64 id) {
    const auto it = menu_items.find(id);
    if (it == menu_items.end() || it->second.owner != &extension) {
        return;
    }
    if (it->second.action) {
        // Deferred: the item may be removing itself from its own click handler.
        it->second.action->setVisible(false);
        it->second.action->deleteLater();
    }
    menu_items.erase(it);
}

u64 ExtensionManager::AddStatusItem(Extension& extension, const char* text) {
    if (!CanUseUi(extension) || services.status_bar == nullptr) {
        return 0;
    }
    const u64 id = next_id++;
    auto* label = new QLabel(FromUtf8(text));
    services.status_bar->addPermanentWidget(label);
    status_items.emplace(id, StatusItem{.owner = &extension, .label = label});
    return id;
}

void ExtensionManager::SetStatusItemText(Extension& extension, u64 id, const char* text) {
    const auto it = status_items.find(id);
    if (it != status_items.end() && it->second.owner == &extension && it->second.label) {
        it->second.label->setText(FromUtf8(text));
    }
}

void ExtensionManager::RemoveStatusItem(Extension& extension, u64 id) {
    const auto it = status_items.find(id);
    if (it == status_items.end() || it->second.owner != &extension) {
        return;
    }
    if (it->second.label) {
        if (services.status_bar != nullptr) {
            services.status_bar->removeWidget(it->second.label);
        }
        it->second.label->deleteLater();
    }
    status_items.erase(it);
}

void ExtensionManager::ShowMessage(Extension& extension, EglMessageKind kind, const char* title,
                                   const char* text) {
    if (!CanUseUi(extension)) {
        return;
    }
    switch (kind) {
    case EGL_MESSAGE_WARNING:
        QMessageBox::warning(services.parent, FromUtf8(title), FromUtf8(text));
        break;
    case EGL_MESSAGE_ERROR:
        QMessageBox::critical(services.parent, FromUtf8(title), FromUtf8(text));
        break;
    case EGL_MESSAGE_INFO:
    default:
        QMessageBox::information(services.parent, FromUtf8(title), FromUtf8(text));
        break;
    }
}

bool ExtensionManager::AskQuestion(Extension& extension, const char* title, const char* text) {
    if (!CanUseUi(extension)) {
        return false;
    }
    return QMessageBox::question(services.parent, FromUtf8(title), FromUtf8(text)) ==
           QMessageBox::Yes;
}

bool ExtensionManager::PromptText(Extension& extension, const char* title, const char* label,
                                  const char* initial, EglStringCallback on_accept, void* user) {
    if (!CanUseUi(extension)) {
        return false;
    }
    bool accepted = false;
    const QString value = QInputDialog::getText(services.parent, FromUtf8(title), FromUtf8(label),
                                                QLineEdit::Normal, FromUtf8(initial), &accepted);
    if (accepted && on_accept != nullptr) {
        on_accept(user, value.toUtf8().constData());
    }
    return accepted;
}

bool ExtensionManager::PickFile(Extension& extension, bool save, const char* title,
                                const char* filter, EglStringCallback on_accept, void* user) {
    if (!CanUseUi(extension)) {
        return false;
    }
    const QString path =
        save ? QFileDialog::getSaveFileName(services.parent, FromUtf8(title), {}, FromUtf8(filter))
             : QFileDialog::getOpenFileName(services.parent, FromUtf8(title), {}, FromUtf8(filter));
    if (path.isEmpty()) {
        return false;
    }
    if (on_accept != nullptr) {
        on_accept(user, path.toUtf8().constData());
    }
    return true;
}

void ExtensionManager::SetClipboardText(Extension& extension, const char* text) {
    if (CanUseUi(extension)) {
        QGuiApplication::clipboard()->setText(FromUtf8(text));
    }
}

void ExtensionManager::OpenUrl(Extension& extension, const char* url) {
    if (!CanUseUi(extension)) {
        return;
    }
    const QUrl parsed{FromUtf8(url), QUrl::StrictMode};
    if (!parsed.isValid() || (parsed.scheme() != QStringLiteral("https") &&
                              parsed.scheme() != QStringLiteral("http"))) {
        LOG_WARNING(Frontend, "[{}] refusing to open a non-web URL", extension.manifest.id);
        return;
    }
    QDesktopServices::openUrl(parsed);
}

u64 ExtensionManager::CreateDevice(Extension& extension, EglDeviceKind kind, const char* name) {
    auto* const engine = services.input != nullptr ? services.input->GetExtensionInput() : nullptr;
    if (!CanUseUi(extension) || engine == nullptr || name == nullptr ||
        (kind != EGL_DEVICE_GAMEPAD && kind != EGL_DEVICE_KEYBOARD)) {
        return 0;
    }
    const std::string key = extension.manifest.id + '/' + name;
    const u128 hash = Common::CityHash128(key.data(), key.size());
    std::array<u8, 16> bytes;
    std::memcpy(bytes.data(), hash.data(), bytes.size());
    const auto port = engine->AddDevice(kind == EGL_DEVICE_KEYBOARD
                                            ? InputCommon::ExtensionInput::DeviceKind::Keyboard
                                            : InputCommon::ExtensionInput::DeviceKind::Gamepad,
                                        name, Common::UUID{bytes});
    if (!port) {
        return 0;
    }
    const u64 id = next_id++;
    devices.emplace(id, Device{.owner = &extension,
                               .port = *port,
                               .id = MakeDeviceId(engine->GetDeviceParams(*port))});
    return id;
}

void ExtensionManager::DestroyDevice(Extension& extension, u64 handle) {
    const auto it = devices.find(handle);
    if (it == devices.end() || it->second.owner != &extension) {
        return;
    }
    std::vector<std::size_t> players;
    for (const auto& [player, assignment] : assignments) {
        if (assignment.device_id == it->second.id) {
            players.push_back(player);
        }
    }
    for (const auto player : players) {
        EndAssignment(player, assignments.at(player).owner != &extension);
    }
    if (auto* engine = services.input != nullptr ? services.input->GetExtensionInput() : nullptr) {
        engine->RemoveDevice(it->second.port);
    }
    devices.erase(it);
}

void ExtensionManager::SetButton(Extension& extension, u64 handle, EglButton button,
                                 bool pressed) {
    const auto it = devices.find(handle);
    if (it == devices.end() || it->second.owner != &extension || button < EGL_BUTTON_A ||
        button > EGL_BUTTON_CAPTURE) {
        return;
    }
    services.input->GetExtensionInput()->SetGamepadButton(
        it->second.port, static_cast<InputCommon::ExtensionInput::Button>(button), pressed);
}

void ExtensionManager::SetStick(Extension& extension, u64 handle, EglStick stick, float x,
                                float y) {
    const auto it = devices.find(handle);
    if (it == devices.end() || it->second.owner != &extension) {
        return;
    }
    services.input->GetExtensionInput()->SetGamepadStick(it->second.port,
                                                         static_cast<std::size_t>(stick), x, y);
}

void ExtensionManager::SetMotion(Extension& extension, u64 handle, float gyro_x, float gyro_y,
                                 float gyro_z, float accel_x, float accel_y, float accel_z,
                                 u64 delta_us) {
    const auto it = devices.find(handle);
    if (it == devices.end() || it->second.owner != &extension) {
        return;
    }
    services.input->GetExtensionInput()->SetGamepadMotion(it->second.port,
                                                          BasicMotion{
                                                              .gyro_x = gyro_x,
                                                              .gyro_y = gyro_y,
                                                              .gyro_z = gyro_z,
                                                              .accel_x = accel_x,
                                                              .accel_y = accel_y,
                                                              .accel_z = accel_z,
                                                              .delta_timestamp = delta_us,
                                                          });
}

void ExtensionManager::SetKey(Extension& extension, u64 handle, u32 usage, bool pressed) {
    const auto it = devices.find(handle);
    if (it == devices.end() || it->second.owner != &extension) {
        return;
    }
    services.input->GetExtensionInput()->SetKey(it->second.port, usage, pressed);
}

u32 ExtensionManager::ListDevices(Extension& extension, EglDeviceInfo* out, u32 capacity) {
    if (!CanUseUi(extension)) {
        return 0;
    }
    const auto entries = EnumerateDevices();
    if (out != nullptr) {
        const auto count = std::min<std::size_t>(capacity, entries.size());
        for (std::size_t i = 0; i < count; ++i) {
            const auto& entry = entries[i];
            EglDeviceInfo info{};
            info.struct_size = sizeof(EglDeviceInfo);
            CopyString(info.id, entry.id);
            CopyString(info.name, entry.name);
            CopyString(info.engine, entry.engine);
            info.vendor_id = entry.vendor_id;
            info.product_id = entry.product_id;
            for (const auto& [handle, device] : devices) {
                if (device.owner == &extension && device.id == entry.id) {
                    info.flags |= EGL_DEVICE_FLAG_OWN;
                    info.handle = handle;
                }
            }
            out[i] = info;
        }
    }
    return static_cast<u32>(entries.size());
}

u32 ExtensionManager::GetPlayerCount(Extension& extension) {
    return CanUseUi(extension) ? static_cast<u32>(PlayerCount) : 0;
}

s32 ExtensionManager::GetPlayerInfo(Extension& extension, u32 player, EglPlayerInfo* info) {
    auto* const controller = CanUseUi(extension) ? GetController(player) : nullptr;
    if (controller == nullptr || info == nullptr ||
        info->struct_size < sizeof(EglPlayerInfo)) {
        return -1;
    }
    info->connected = controller->IsConnected() ? 1 : 0;
    info->assigned = 0;
    info->assigned_by_me = 0;
    CopyString(info->label, "");
    if (const auto it = assignments.find(player); it != assignments.end()) {
        info->assigned = 1;
        info->assigned_by_me = it->second.owner == &extension ? 1 : 0;
        CopyString(info->label, it->second.label);
    }
    return 0;
}

s32 ExtensionManager::AssignDevice(Extension& extension, const char* device_id, s32 player,
                                   const char* label) {
    if (!CanUseUi(extension) || device_id == nullptr || services.input == nullptr) {
        return -1;
    }
    if (assignments_suspended) {
        LOG_WARNING(Frontend, "[{}] players cannot be assigned while the controller settings "
                              "are open", extension.manifest.id);
        return -1;
    }
    const auto entries = EnumerateDevices();
    const auto device = std::find_if(entries.begin(), entries.end(),
                                     [&](const auto& entry) { return entry.id == device_id; });
    if (device == entries.end()) {
        LOG_WARNING(Frontend, "[{}] unknown input device {}", extension.manifest.id, device_id);
        return -1;
    }

    std::size_t target = PlayerCount;
    if (player < 0) {
        for (std::size_t i = 1; i < PlayerCount; ++i) {
            auto* controller = GetController(i);
            if (!assignments.contains(i) && controller != nullptr && !controller->IsConnected()) {
                target = i;
                break;
            }
        }
    } else if (static_cast<std::size_t>(player) < PlayerCount) {
        target = static_cast<std::size_t>(player);
        if (const auto it = assignments.find(target);
            it != assignments.end() && it->second.owner != &extension) {
            return -1;
        }
    }
    if (target == PlayerCount) {
        return -1;
    }

    Assignment assignment{.owner = &extension,
                          .device_id = device->id,
                          .label = label != nullptr ? label : "",
                          .input_override = {}};
    const auto buttons = services.input->GetButtonMappingForDevice(device->params);
    const auto sticks = services.input->GetAnalogMappingForDevice(device->params);
    const auto motions = services.input->GetMotionMappingForDevice(device->params);
    if (buttons.empty() && sticks.empty()) {
        LOG_WARNING(Frontend, "[{}] {} has no default mapping", extension.manifest.id,
                    device->name);
        return -1;
    }
    for (const auto& [index, param] : buttons) {
        assignment.input_override.buttons[index] = param;
    }
    for (const auto& [index, param] : sticks) {
        assignment.input_override.sticks[index] = param;
    }
    for (const auto& [index, param] : motions) {
        assignment.input_override.motions[index] = param;
    }
    if (!ApplyAssignment(target, assignment)) {
        LOG_WARNING(Frontend, "[{}] player {} cannot be connected", extension.manifest.id,
                    target + 1);
        return -1;
    }
    LOG_INFO(Frontend, "[{}] player {} assigned to {} ({})", extension.manifest.id, target + 1,
             device->name, assignment.label);
    assignments.insert_or_assign(target, std::move(assignment));
    StartDevicePolling();
    return static_cast<s32>(target);
}

void ExtensionManager::ReleasePlayer(Extension& extension, s32 player) {
    if (player < 0) {
        return;
    }
    const auto it = assignments.find(static_cast<std::size_t>(player));
    if (it != assignments.end() && it->second.owner == &extension) {
        EndAssignment(it->first, false);
    }
}

bool ExtensionManager::CanUseUi(const Extension& extension) const {
    return phase == Phase::Running && IsActive(extension) && bridge != nullptr;
}

QMenu* ExtensionManager::GetMenu(EglMenu menu) const {
    const auto index = static_cast<std::size_t>(menu);
    return index < services.menus.size() ? services.menus[index] : nullptr;
}

Core::HID::EmulatedController* ExtensionManager::GetController(std::size_t player) const {
    if (services.system == nullptr || player >= PlayerCount) {
        return nullptr;
    }
    return services.system->HIDCore().GetEmulatedControllerByIndex(player);
}

std::vector<ExtensionManager::DeviceEntry> ExtensionManager::EnumerateDevices() const {
    std::vector<DeviceEntry> result;
    if (services.input == nullptr) {
        return result;
    }
    for (auto& params : services.input->GetInputDevices()) {
        const auto engine = params.Get("engine", "");
        if (engine.empty() || engine == "any") {
            continue;
        }
        DeviceEntry entry;
        entry.id = MakeDeviceId(params);
        entry.name = params.Get("display", "");
        entry.engine = engine;
        if (engine == "sdl") {
            std::tie(entry.vendor_id, entry.product_id) = SdlUsbIds(params.Get("guid", ""));
        }
        entry.params = std::move(params);
        result.push_back(std::move(entry));
    }
    return result;
}

ExtensionHost::State::Entry& ExtensionManager::StateEntry(const std::string& id) {
    return state.extensions[id];
}

void ExtensionManager::SaveState() {
    if (!state_path.empty() && !state.Save(state_path)) {
        LOG_ERROR(Frontend, "Cannot write {}", Common::FS::PathToUTF8String(state_path));
    }
}

void ExtensionManager::DispatchEvent(const EglEvent& event, Extension* only) {
    std::vector<u64> ids;
    for (const auto& [id, subscription] : subscriptions) {
        if (subscription.type == event.type && (only == nullptr || subscription.owner == only)) {
            ids.push_back(id);
        }
    }
    // A callback may unsubscribe itself or others, so look every subscription up again.
    for (const auto id : ids) {
        const auto it = subscriptions.find(id);
        if (it == subscriptions.end() || !IsActive(*it->second.owner)) {
            continue;
        }
        const auto subscription = it->second;
        subscription.callback(subscription.user, &event);
    }
}

void ExtensionManager::StartDevicePolling() {
    if (bridge == nullptr || device_poll_timer != nullptr) {
        return;
    }
    for (const auto& entry : EnumerateDevices()) {
        last_device_ids.push_back(entry.id);
    }
    std::sort(last_device_ids.begin(), last_device_ids.end());
    device_poll_timer = new QTimer(bridge);
    device_poll_timer->setInterval(DevicePollIntervalMs);
    QObject::connect(device_poll_timer, &QTimer::timeout, bridge, [this] { PollDevices(); });
    device_poll_timer->start();
}

void ExtensionManager::PollDevices() {
    if (phase != Phase::Running) {
        return;
    }
    std::vector<std::string> ids;
    for (const auto& entry : EnumerateDevices()) {
        ids.push_back(entry.id);
    }
    std::sort(ids.begin(), ids.end());
    if (ids == last_device_ids) {
        return;
    }
    last_device_ids = ids;

    if (!assignments_suspended) {
        std::vector<std::size_t> ended;
        for (const auto& [player, assignment] : assignments) {
            if (!std::binary_search(ids.begin(), ids.end(), assignment.device_id)) {
                ended.push_back(player);
            }
        }
        for (const auto player : ended) {
            EndAssignment(player, true);
        }
    }
    DispatchEvent(EglEvent{.struct_size = sizeof(EglEvent),
                           .type = EGL_EVENT_INPUT_DEVICES_CHANGED,
                           .program_id = current_program_id,
                           .title = current_title.c_str(),
                           .player = -1});
}

bool ExtensionManager::ApplyAssignment(std::size_t player, Assignment& assignment) {
    auto* const controller = GetController(player);
    if (controller == nullptr) {
        return false;
    }
    // Games that refuse a Pro Controller usually take a pair of Joy-Cons.
    for (const auto style : {Core::HID::NpadStyleIndex::Fullkey,
                             Core::HID::NpadStyleIndex::JoyconDual}) {
        assignment.input_override.style = style;
        controller->SetInputOverride(assignment.input_override);
        if (controller->IsConnected()) {
            return true;
        }
    }
    controller->SetInputOverride(std::nullopt);
    return false;
}

void ExtensionManager::EndAssignment(std::size_t player, bool notify) {
    const auto it = assignments.find(player);
    if (it == assignments.end()) {
        return;
    }
    Extension* const owner = it->second.owner;
    assignments.erase(it);
    if (auto* controller = GetController(player); controller != nullptr && !assignments_suspended) {
        controller->SetInputOverride(std::nullopt);
    }
    if (notify) {
        DispatchEvent(EglEvent{.struct_size = sizeof(EglEvent),
                               .type = EGL_EVENT_PLAYER_RELEASED,
                               .program_id = current_program_id,
                               .title = current_title.c_str(),
                               .player = static_cast<s32>(player)},
                      owner);
    }
}

void ExtensionManager::RemoveRegistrations(Extension& extension) {
    std::vector<u64> ids;
    for (const auto& [id, item] : menu_items) {
        if (item.owner == &extension) {
            ids.push_back(id);
        }
    }
    for (const auto id : ids) {
        RemoveMenuItem(extension, id);
    }
    ids.clear();
    for (const auto& [id, item] : timers) {
        if (item.owner == &extension) {
            ids.push_back(id);
        }
    }
    for (const auto id : ids) {
        RemoveTimer(extension, id);
    }
    ids.clear();
    for (const auto& [id, item] : status_items) {
        if (item.owner == &extension) {
            ids.push_back(id);
        }
    }
    for (const auto id : ids) {
        RemoveStatusItem(extension, id);
    }
    std::erase_if(subscriptions, [&](const auto& item) { return item.second.owner == &extension; });

    std::vector<std::size_t> players;
    for (const auto& [player, assignment] : assignments) {
        if (assignment.owner == &extension) {
            players.push_back(player);
        }
    }
    for (const auto player : players) {
        EndAssignment(player, false);
    }
    ids.clear();
    for (const auto& [id, device] : devices) {
        if (device.owner == &extension) {
            ids.push_back(id);
        }
    }
    for (const auto id : ids) {
        DestroyDevice(extension, id);
    }
}

void ExtensionManager::FailExtension(Extension& extension, std::string error) {
    Write(EGL_LOG_ERROR, fmt::format("Extension {} disabled: {}", extension.manifest.id, error));
    extension.failed = true;
    auto& info = infos[extension.info_index];
    info.status = Status::InitFailed;
    info.error = std::move(error);
    RemoveRegistrations(extension);
}

} // namespace Extensions
