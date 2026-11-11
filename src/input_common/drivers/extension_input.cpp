// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "common/settings_input.h"
#include "input_common/drivers/extension_input.h"

namespace InputCommon {

namespace {
constexpr int StickCount = 2;

// The default keyboard layout of the Qt frontend, as USB HID usages.
constexpr std::array<int, Settings::NativeButton::NumButtons> KeyboardButtons{
    0x06, 0x1B, 0x19, 0x1D, 0x09, 0x0A, 0x14, 0x08, 0x15, 0x17, 0x10,
    0x11, 0x50, 0x52, 0x4F, 0x51, 0x14, 0x08, 0x00, 0x00, 0x14, 0x08,
};
// up, down, left, right, modifier
constexpr std::array<std::array<int, 5>, Settings::NativeAnalog::NumAnalogs> KeyboardSticks{{
    {0x1A, 0x16, 0x04, 0x07, 0xE1},
    {0x0C, 0x0E, 0x0D, 0x0F, 0x00},
}};

constexpr std::array<ExtensionInput::Button, Settings::NativeButton::NumButtons> GamepadButtons{
    ExtensionInput::Button::A,      ExtensionInput::Button::B,
    ExtensionInput::Button::X,      ExtensionInput::Button::Y,
    ExtensionInput::Button::LStick, ExtensionInput::Button::RStick,
    ExtensionInput::Button::L,      ExtensionInput::Button::R,
    ExtensionInput::Button::ZL,     ExtensionInput::Button::ZR,
    ExtensionInput::Button::Plus,   ExtensionInput::Button::Minus,
    ExtensionInput::Button::DLeft,  ExtensionInput::Button::DUp,
    ExtensionInput::Button::DRight, ExtensionInput::Button::DDown,
    ExtensionInput::Button::SL,     ExtensionInput::Button::SR,
    ExtensionInput::Button::Home,   ExtensionInput::Button::Capture,
    ExtensionInput::Button::SL,     ExtensionInput::Button::SR,
};
} // namespace

ExtensionInput::ExtensionInput(std::string input_engine_) : InputEngine(std::move(input_engine_)) {}

std::optional<std::size_t> ExtensionInput::AddDevice(DeviceKind kind, std::string name,
                                                     Common::UUID guid) {
    std::size_t port = MaxDevices;
    {
        std::scoped_lock lock{mutex};
        for (std::size_t i = 0; i < MaxDevices; ++i) {
            if (!devices[i]) {
                port = i;
                devices[i] = Device{.kind = kind, .name = std::move(name), .guid = guid};
                break;
            }
        }
    }
    if (port == MaxDevices) {
        return std::nullopt;
    }
    PreSetController(PadIdentifier{.guid = guid, .port = port, .pad = 0});
    return port;
}

void ExtensionInput::RemoveDevice(std::size_t port) {
    std::optional<Device> device;
    {
        std::scoped_lock lock{mutex};
        if (port >= MaxDevices || !devices[port]) {
            return;
        }
        device = std::move(devices[port]);
        devices[port].reset();
    }
    const PadIdentifier identifier{.guid = device->guid, .port = port, .pad = 0};
    if (device->kind == DeviceKind::Keyboard) {
        for (std::size_t usage = 0; usage < MaxKeys; ++usage) {
            if (device->held_keys[usage]) {
                SetButton(identifier, static_cast<int>(usage), false);
            }
        }
        return;
    }
    for (int button = 0; button < static_cast<int>(Button::Count); ++button) {
        SetButton(identifier, button, false);
    }
    for (int axis = 0; axis < StickCount * 2; ++axis) {
        SetAxis(identifier, axis, 0.0f);
    }
}

void ExtensionInput::SetGamepadButton(std::size_t port, Button button, bool pressed) {
    if (button >= Button::Count) {
        return;
    }
    const auto identifier = GetIdentifier(port);
    if (identifier.guid.IsInvalid()) {
        return;
    }
    SetButton(identifier, static_cast<int>(button), pressed);
}

void ExtensionInput::SetGamepadStick(std::size_t port, std::size_t stick, float x, float y) {
    if (stick >= StickCount) {
        return;
    }
    const auto identifier = GetIdentifier(port);
    if (identifier.guid.IsInvalid()) {
        return;
    }
    SetAxis(identifier, static_cast<int>(stick * 2), std::clamp(x, -1.0f, 1.0f));
    SetAxis(identifier, static_cast<int>(stick * 2 + 1), std::clamp(y, -1.0f, 1.0f));
}

void ExtensionInput::SetGamepadMotion(std::size_t port, const BasicMotion& motion) {
    const auto identifier = GetIdentifier(port);
    if (identifier.guid.IsInvalid()) {
        return;
    }
    SetMotion(identifier, 0, motion);
}

void ExtensionInput::SetKey(std::size_t port, std::size_t usage, bool pressed) {
    if (usage == 0 || usage >= MaxKeys) {
        return;
    }
    PadIdentifier identifier{};
    {
        std::scoped_lock lock{mutex};
        if (port >= MaxDevices || !devices[port] || devices[port]->kind != DeviceKind::Keyboard) {
            return;
        }
        devices[port]->held_keys[usage] = pressed;
        identifier = PadIdentifier{.guid = devices[port]->guid, .port = port, .pad = 0};
    }
    SetButton(identifier, static_cast<int>(usage), pressed);
}

Common::ParamPackage ExtensionInput::GetDeviceParams(std::size_t port) const {
    std::scoped_lock lock{mutex};
    if (port >= MaxDevices || !devices[port]) {
        return {};
    }
    return MakeParams(port, *devices[port]);
}

std::vector<Common::ParamPackage> ExtensionInput::GetInputDevices() const {
    std::vector<Common::ParamPackage> result;
    std::scoped_lock lock{mutex};
    for (std::size_t port = 0; port < MaxDevices; ++port) {
        if (devices[port]) {
            result.push_back(MakeParams(port, *devices[port]));
        }
    }
    return result;
}

ButtonMapping ExtensionInput::GetButtonMappingForDevice(const Common::ParamPackage& params) {
    const auto device = FindDevice(params);
    if (!device) {
        return {};
    }
    ButtonMapping mapping{};
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        const int button = device->kind == DeviceKind::Keyboard
                               ? KeyboardButtons[i]
                               : static_cast<int>(GamepadButtons[i]);
        if (device->kind == DeviceKind::Keyboard && button == 0) {
            continue;
        }
        Common::ParamPackage param;
        param.Set("engine", GetEngineName());
        param.Set("guid", params.Get("guid", ""));
        param.Set("port", params.Get("port", 0));
        param.Set("button", button);
        mapping.insert_or_assign(static_cast<Settings::NativeButton::Values>(i), std::move(param));
    }
    return mapping;
}

AnalogMapping ExtensionInput::GetAnalogMappingForDevice(const Common::ParamPackage& params) {
    const auto device = FindDevice(params);
    if (!device) {
        return {};
    }
    const auto key_param = [&](int usage) {
        Common::ParamPackage param;
        param.Set("engine", GetEngineName());
        param.Set("guid", params.Get("guid", ""));
        param.Set("port", params.Get("port", 0));
        param.Set("button", usage);
        return param.Serialize();
    };
    AnalogMapping mapping{};
    for (int stick = 0; stick < Settings::NativeAnalog::NumAnalogs; ++stick) {
        Common::ParamPackage param;
        if (device->kind == DeviceKind::Keyboard) {
            const auto& keys = KeyboardSticks[stick];
            param.Set("engine", "analog_from_button");
            param.Set("up", key_param(keys[0]));
            param.Set("down", key_param(keys[1]));
            param.Set("left", key_param(keys[2]));
            param.Set("right", key_param(keys[3]));
            param.Set("modifier", keys[4] != 0 ? key_param(keys[4]) : std::string{});
            param.Set("modifier_scale", 0.5f);
        } else {
            param.Set("engine", GetEngineName());
            param.Set("guid", params.Get("guid", ""));
            param.Set("port", params.Get("port", 0));
            param.Set("axis_x", stick * 2);
            param.Set("axis_y", stick * 2 + 1);
            param.Set("deadzone", 0.1f);
            param.Set("range", 1.0f);
        }
        mapping.insert_or_assign(static_cast<Settings::NativeAnalog::Values>(stick),
                                 std::move(param));
    }
    return mapping;
}

MotionMapping ExtensionInput::GetMotionMappingForDevice(const Common::ParamPackage& params) {
    const auto device = FindDevice(params);
    if (!device || device->kind != DeviceKind::Gamepad) {
        return {};
    }
    MotionMapping mapping{};
    for (int motion = 0; motion < Settings::NativeMotion::NumMotions; ++motion) {
        Common::ParamPackage param;
        param.Set("engine", GetEngineName());
        param.Set("guid", params.Get("guid", ""));
        param.Set("port", params.Get("port", 0));
        param.Set("motion", 0);
        mapping.insert_or_assign(static_cast<Settings::NativeMotion::Values>(motion),
                                 std::move(param));
    }
    return mapping;
}

Common::Input::ButtonNames ExtensionInput::GetUIName(const Common::ParamPackage& params) const {
    if (params.Has("button") || params.Has("axis")) {
        return Common::Input::ButtonNames::Value;
    }
    if (params.Has("motion")) {
        return Common::Input::ButtonNames::Engine;
    }
    return Common::Input::ButtonNames::Invalid;
}

PadIdentifier ExtensionInput::GetIdentifier(std::size_t port) const {
    std::scoped_lock lock{mutex};
    if (port >= MaxDevices || !devices[port]) {
        return {};
    }
    return PadIdentifier{.guid = devices[port]->guid, .port = port, .pad = 0};
}

std::optional<ExtensionInput::Device> ExtensionInput::FindDevice(
    const Common::ParamPackage& params) const {
    const auto port = static_cast<std::size_t>(params.Get("port", -1));
    const Common::UUID guid{params.Get("guid", "")};
    std::scoped_lock lock{mutex};
    if (port >= MaxDevices || !devices[port] || devices[port]->guid != guid) {
        return std::nullopt;
    }
    return devices[port];
}

Common::ParamPackage ExtensionInput::MakeParams(std::size_t port, const Device& device) const {
    return Common::ParamPackage{
        {"engine", GetEngineName()},
        {"display", device.name},
        {"guid", device.guid.RawString()},
        {"port", std::to_string(port)},
        {"kind", device.kind == DeviceKind::Keyboard ? "keyboard" : "gamepad"},
    };
}

} // namespace InputCommon
