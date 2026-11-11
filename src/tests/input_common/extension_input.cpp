// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/settings_input.h"
#include "input_common/drivers/extension_input.h"

namespace {

using InputCommon::ExtensionInput;

Common::UUID MakeGuid(u8 seed) {
    std::array<u8, 16> bytes{};
    bytes.fill(seed);
    return Common::UUID{bytes};
}

} // namespace

TEST_CASE("Extension input devices take free ports", "[extensions]") {
    ExtensionInput engine{"extension"};
    const auto first = engine.AddDevice(ExtensionInput::DeviceKind::Gamepad, "Pad", MakeGuid(1));
    const auto second =
        engine.AddDevice(ExtensionInput::DeviceKind::Keyboard, "Keys", MakeGuid(2));
    REQUIRE(first == 0);
    REQUIRE(second == 1);

    auto devices = engine.GetInputDevices();
    REQUIRE(devices.size() == 2);
    REQUIRE(devices[0].Get("engine", "") == "extension");
    REQUIRE(devices[0].Get("display", "") == "Pad");
    REQUIRE(devices[0].Get("guid", "") == MakeGuid(1).RawString());
    REQUIRE(devices[0].Get("port", -1) == 0);
    REQUIRE(devices[1].Get("kind", "") == "keyboard");

    engine.RemoveDevice(*first);
    devices = engine.GetInputDevices();
    REQUIRE(devices.size() == 1);
    REQUIRE(devices[0].Get("display", "") == "Keys");
    REQUIRE(engine.GetDeviceParams(*first).Get("engine", "").empty());
    REQUIRE(engine.AddDevice(ExtensionInput::DeviceKind::Gamepad, "Again", MakeGuid(3)) == 0);
}

TEST_CASE("Extension input runs out of ports", "[extensions]") {
    ExtensionInput engine{"extension"};
    for (std::size_t i = 0; i < ExtensionInput::MaxDevices; ++i) {
        REQUIRE(engine.AddDevice(ExtensionInput::DeviceKind::Gamepad, "Pad", MakeGuid(1)));
    }
    REQUIRE(!engine.AddDevice(ExtensionInput::DeviceKind::Gamepad, "Pad", MakeGuid(1)));
}

TEST_CASE("Extension gamepads map every Switch button and both sticks", "[extensions]") {
    ExtensionInput engine{"extension"};
    const auto port = engine.AddDevice(ExtensionInput::DeviceKind::Gamepad, "Pad", MakeGuid(4));
    const auto params = engine.GetDeviceParams(*port);

    const auto buttons = engine.GetButtonMappingForDevice(params);
    REQUIRE(buttons.size() == Settings::NativeButton::NumButtons);
    REQUIRE(buttons.at(Settings::NativeButton::A).Get("button", -1) ==
            static_cast<int>(ExtensionInput::Button::A));
    REQUIRE(buttons.at(Settings::NativeButton::Screenshot).Get("button", -1) ==
            static_cast<int>(ExtensionInput::Button::Capture));
    REQUIRE(buttons.at(Settings::NativeButton::SLRight).Get("button", -1) ==
            static_cast<int>(ExtensionInput::Button::SL));
    REQUIRE(buttons.at(Settings::NativeButton::A).Get("guid", "") == MakeGuid(4).RawString());

    const auto sticks = engine.GetAnalogMappingForDevice(params);
    REQUIRE(sticks.size() == Settings::NativeAnalog::NumAnalogs);
    REQUIRE(sticks.at(Settings::NativeAnalog::RStick).Get("axis_x", -1) == 2);
    REQUIRE(sticks.at(Settings::NativeAnalog::RStick).Get("axis_y", -1) == 3);

    REQUIRE(engine.GetMotionMappingForDevice(params).size() == Settings::NativeMotion::NumMotions);
}

TEST_CASE("Extension keyboards map the default keyboard layout", "[extensions]") {
    ExtensionInput engine{"extension"};
    const auto port = engine.AddDevice(ExtensionInput::DeviceKind::Keyboard, "Keys", MakeGuid(5));
    const auto params = engine.GetDeviceParams(*port);

    const auto buttons = engine.GetButtonMappingForDevice(params);
    // C is A, as in the default Qt layout. Home and Capture have no key.
    REQUIRE(buttons.at(Settings::NativeButton::A).Get("button", -1) == 0x06);
    REQUIRE(!buttons.contains(Settings::NativeButton::Home));

    const auto sticks = engine.GetAnalogMappingForDevice(params);
    const auto& left = sticks.at(Settings::NativeAnalog::LStick);
    REQUIRE(left.Get("engine", "") == "analog_from_button");
    REQUIRE(Common::ParamPackage{left.Get("up", "")}.Get("button", -1) == 0x1A);

    REQUIRE(engine.GetMotionMappingForDevice(params).empty());
}

TEST_CASE("Extension input ignores params of another device", "[extensions]") {
    ExtensionInput engine{"extension"};
    const auto port = engine.AddDevice(ExtensionInput::DeviceKind::Gamepad, "Pad", MakeGuid(6));
    auto params = engine.GetDeviceParams(*port);
    params.Set("guid", MakeGuid(7).RawString());
    REQUIRE(engine.GetButtonMappingForDevice(params).empty());
    params.Set("port", 31);
    REQUIRE(engine.GetAnalogMappingForDevice(params).empty());
}
