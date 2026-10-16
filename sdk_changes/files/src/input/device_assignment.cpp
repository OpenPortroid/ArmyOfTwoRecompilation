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

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>

REXCVAR_DEFINE_BOOL(keyboard_own_player, false, "Input",
                    "Keyboard and mouse are player 1 on their own, gamepads start at player 2 "
                    "(split-screen with one pad)");

namespace rex::input {

void SlotAssignment::OnDevicesChanged(const std::vector<DeviceInfo>& devices) {
  for (auto& user : users_) {
    user.clear();
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
}

void SharedAssignment::DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const {
  out.clear();
  if (user_index != 0) {
    return;
  }
  out = user0_;
}

}  // namespace rex::input
