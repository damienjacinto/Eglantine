// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "hid_core/frontend/emulated_controller.h"

namespace {

using Core::HID::EmulatedController;
using Core::HID::InputOverride;
using Core::HID::NpadIdType;
using Core::HID::NpadStyleIndex;

constexpr std::size_t PlayerTwo = 1;

InputOverride MakeOverride(int button) {
    InputOverride input_override;
    input_override.buttons[Settings::NativeButton::A] =
        Common::ParamPackage{{"engine", "override_test"}, {"button", std::to_string(button)}};
    input_override.style = NpadStyleIndex::Fullkey;
    return input_override;
}

} // namespace

TEST_CASE("Input override maps and connects a player without saving it", "[extensions]") {
    auto& player = Settings::values.players.GetValue()[PlayerTwo];
    const auto saved_player = player;
    player.connected = false;
    player.buttons[Settings::NativeButton::A] = "engine:keyboard,code:67";

    EmulatedController controller{NpadIdType::Player2};
    controller.ReloadFromSettings();
    REQUIRE(!controller.IsConnected());
    REQUIRE(!controller.HasInputOverride());

    controller.SetInputOverride(MakeOverride(5));
    REQUIRE(controller.HasInputOverride());
    REQUIRE(controller.IsConnected());
    REQUIRE(controller.GetButtonParam(Settings::NativeButton::A).Get("button", -1) == 5);

    // Reloading from settings, as booting a game does, keeps the override.
    controller.ReloadFromSettings();
    REQUIRE(controller.IsConnected());
    REQUIRE(controller.GetButtonParam(Settings::NativeButton::A).Get("button", -1) == 5);

    // Saving never writes the override into the settings.
    controller.SaveCurrentConfig();
    REQUIRE(!player.connected);
    REQUIRE(player.buttons[Settings::NativeButton::A] == "engine:keyboard,code:67");

    controller.SetInputOverride(std::nullopt);
    REQUIRE(!controller.HasInputOverride());
    REQUIRE(!controller.IsConnected());
    REQUIRE(controller.GetButtonParam(Settings::NativeButton::A).Get("code", -1) == 67);

    player = saved_player;
}
