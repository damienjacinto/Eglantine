// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bitset>
#include <mutex>
#include <optional>
#include <string>

#include "input_common/input_engine.h"

namespace InputCommon {

/**
 * Input devices created at runtime by extensions. Each device takes a port and shows up in the
 * device list like any other controller.
 */
class ExtensionInput final : public InputEngine {
public:
    enum class DeviceKind {
        Gamepad,
        Keyboard,
    };

    /// Gamepad button ids, in the order of the extension API's EglButton.
    enum class Button {
        A,
        B,
        X,
        Y,
        LStick,
        RStick,
        L,
        R,
        ZL,
        ZR,
        Plus,
        Minus,
        DLeft,
        DUp,
        DRight,
        DDown,
        SL,
        SR,
        Home,
        Capture,
        Count,
    };

    static constexpr std::size_t MaxDevices = 32;
    static constexpr std::size_t MaxKeys = 256;

    explicit ExtensionInput(std::string input_engine_);

    /**
     * Adds a device.
     * @param guid identifies the device across sessions, so a mapping saved for it keeps working
     * @returns the port of the new device, or std::nullopt when every port is in use
     */
    std::optional<std::size_t> AddDevice(DeviceKind kind, std::string name, Common::UUID guid);

    /// Releases everything the device holds and frees its port.
    void RemoveDevice(std::size_t port);

    void SetGamepadButton(std::size_t port, Button button, bool pressed);

    /// x grows to the right and y grows upwards, both in [-1, 1].
    void SetGamepadStick(std::size_t port, std::size_t stick, float x, float y);

    void SetGamepadMotion(std::size_t port, const BasicMotion& motion);

    /// @param usage USB HID keyboard usage id (page 0x07)
    void SetKey(std::size_t port, std::size_t usage, bool pressed);

    /// Returns the device params as GetInputDevices lists them, or an empty package.
    [[nodiscard]] Common::ParamPackage GetDeviceParams(std::size_t port) const;

    std::vector<Common::ParamPackage> GetInputDevices() const override;
    ButtonMapping GetButtonMappingForDevice(const Common::ParamPackage& params) override;
    AnalogMapping GetAnalogMappingForDevice(const Common::ParamPackage& params) override;
    MotionMapping GetMotionMappingForDevice(const Common::ParamPackage& params) override;
    Common::Input::ButtonNames GetUIName(const Common::ParamPackage& params) const override;

private:
    struct Device {
        DeviceKind kind{};
        std::string name;
        Common::UUID guid;
        std::bitset<MaxKeys> held_keys;
    };

    [[nodiscard]] PadIdentifier GetIdentifier(std::size_t port) const;
    [[nodiscard]] std::optional<Device> FindDevice(const Common::ParamPackage& params) const;
    [[nodiscard]] Common::ParamPackage MakeParams(std::size_t port, const Device& device) const;

    mutable std::mutex mutex;
    std::array<std::optional<Device>, MaxDevices> devices;
};

} // namespace InputCommon
