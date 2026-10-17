/**
 * @file        input/device_assignment.cpp
 * @brief       Policies assigning input devices to guest user indices.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <string_view>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>

REXCVAR_DEFINE_BOOL(keyboard_own_player, false, "Input",
                    "Keyboard and mouse are player 1 on their own, gamepads start at player 2 "
                    "(split-screen with one pad)");
REXCVAR_DEFINE_STRING(input_player1, "auto", "Input",
                      "Devices of player 1, comma-separated: keyboard, pad1-pad4 (gamepads in the order "
                      "they were found), pads (every gamepad no other player names), none. auto: pad 1 "
                      "and the keyboard are player 1, pad N is player N (see keyboard_own_player)");
REXCVAR_DEFINE_STRING(input_player2, "auto", "Input",
                      "Devices of player 2 (split-screen), as input_player1");

namespace rex::input {

namespace {

// The mouse and keyboard driver's device; the NOP stand-in is the other
// synthetic one.
constexpr std::string_view kKeyboardName = "Keyboard and Mouse";

std::atomic<uint32_t> g_keyboard_user{0};

// What one player's input_playerN value names.
struct PlayerDevices {
  bool keyboard = false;
  bool pads = false;      // every gamepad nobody names
  uint32_t pad_mask = 0;  // pad N = bit N-1
};

PlayerDevices ParseDevices(std::string_view text, bool first_player) {
  PlayerDevices d;
  while (!text.empty()) {
    size_t comma = text.find(',');
    std::string_view token = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    while (!token.empty() && token.front() == ' ') {
      token.remove_prefix(1);
    }
    while (!token.empty() && token.back() == ' ') {
      token.remove_suffix(1);
    }
    if (token == "keyboard") {
      d.keyboard = true;
    } else if (token == "pads") {
      d.pads = true;
    } else if (token.size() == 4 && token.substr(0, 3) == "pad" && token[3] >= '1' &&
               token[3] <= '4') {
      d.pad_mask |= 1u << (token[3] - '1');
    } else if (token == "auto") {
      // Next to an explicit choice of the other player: player 1 keeps
      // everything left over, the others get nothing.
      if (first_player) {
        d.keyboard = true;
        d.pads = true;
      }
    }
  }
  return d;
}

}  // namespace

uint32_t KeyboardUser() {
  return g_keyboard_user.load(std::memory_order_relaxed);
}

void SlotAssignment::OnDevicesChanged(const std::vector<DeviceInfo>& devices) {
  for (auto& user : users_) {
    user.clear();
  }

  std::string p1 = REXCVAR_GET(input_player1);
  std::string p2 = REXCVAR_GET(input_player2);
  if (p1 != "auto" || p2 != "auto") {
    // Chosen devices (the launcher's split-screen page). A pad named by
    // both players is the first one's.
    PlayerDevices players[2] = {ParseDevices(p1, true), ParseDevices(p2, false)};
    uint32_t named = players[0].pad_mask | players[1].pad_mask;
    players[1].pad_mask &= ~players[0].pad_mask;
    uint32_t keyboard_user = kNoUser;
    for (uint32_t i = 0; i < 2 && keyboard_user == kNoUser; ++i) {
      if (players[i].keyboard) {
        keyboard_user = i;
      }
    }
    for (const auto& device : devices) {
      if (device.synthetic) {
        // The NOP stand-in keeps player 1 connected whatever pad they use.
        uint32_t user = device.name == kKeyboardName ? keyboard_user : 0;
        if (user != kNoUser) {
          users_[user].push_back(device.id);
        }
        continue;
      }
      uint32_t bit = device.ordinal < 32 ? 1u << device.ordinal : 0;
      for (uint32_t i = 0; i < 2; ++i) {
        if ((players[i].pad_mask & bit) || (players[i].pads && !(named & bit))) {
          users_[i].push_back(device.id);
          break;
        }
      }
    }
    g_keyboard_user.store(keyboard_user, std::memory_order_relaxed);
    return;
  }

  // Key off the ordinal rather than list position, so unplugging pad one does
  // not promote pad two to player one.
  // With keyboard_own_player the first pad is player 2.
  uint32_t first_pad_user = REXCVAR_GET(keyboard_own_player) ? 1 : 0;
  for (const auto& device : devices) {
    if (device.synthetic) {
      users_[0].push_back(device.id);
    } else if (device.ordinal + first_pad_user < kMaxGuestUsers) {
      users_[device.ordinal + first_pad_user].push_back(device.id);
    }
  }
  g_keyboard_user.store(0, std::memory_order_relaxed);
}

void SlotAssignment::DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const {
  out.clear();
  if (user_index >= kMaxGuestUsers) {
    return;
  }
  out = users_[user_index];
}

void SharedAssignment::OnDevicesChanged(const std::vector<DeviceInfo>& devices) {
  user0_.clear();
  user0_.reserve(devices.size());
  for (const auto& device : devices) {
    user0_.push_back(device.id);
  }
  g_keyboard_user.store(0, std::memory_order_relaxed);
}

void SharedAssignment::DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const {
  out.clear();
  if (user_index != 0) {
    return;
  }
  out = user0_;
}

}  // namespace rex::input

extern "C" uint32_t rex_input_keyboard_user() {
  return rex::input::KeyboardUser();
}
