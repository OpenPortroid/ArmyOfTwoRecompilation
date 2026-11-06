#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <memory>
#include <mutex>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/state_merge.h>
#include <rex/system/interfaces/input.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem : public system::IInputSystem {
 public:
  explicit InputSystem(rex::ui::Window* window);
  ~InputSystem() override;

  rex::ui::Window* window() const { return window_; }

  X_STATUS Setup() override;
  void Shutdown() override;

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void AttachWindow(rex::ui::Window* window);
  void SetActiveCallback(std::function<bool()> callback);

  /// Replaces any previous assignment. Call before the guest starts polling.
  void SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment);

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke);
  /// Every physical pad merged, whoever it is assigned to and without the
  /// player's button remap; returns how many there are.
  uint32_t ReadPads(X_INPUT_GAMEPAD* out);

 private:
  /// Re-enumerates every driver and notifies the assignment when the set
  /// changed.
  void RefreshDevices();
  /// The physical devices the assignment gives guest user 0.
  std::vector<DeviceId> Player1Pads() const;
  InputDriver* DriverForDevice(DeviceId id);
  const DeviceInfo* DeviceInfoFor(DeviceId id) const;

  rex::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;

  std::unique_ptr<DeviceAssignment> assignment_;
  ActiveDeviceTracker active_devices_;

  // Ordered by ordinal. Ordinals are never recycled, so unplugging pad one
  // does not renumber pad two.
  std::vector<DeviceInfo> devices_;
  std::vector<InputDriver*> device_owners_;

  // Guest threads poll XInput concurrently, and every call re-enumerates
  // devices_, so all device bookkeeping is serialized.
  std::mutex mutex_;
};

/// What player 1 used last, for button prompts that follow the hands:
/// keyboard / mouse, or a gamepad (also when one is plugged in).
enum class LastInput : uint32_t { kNone = 0, kKeyboardMouse = 1, kGamepad = 2 };
void NoteLastInput(LastInput kind);
LastInput GetLastInput();

/// No guest user (KeyboardUser when no player has the keyboard).
constexpr uint32_t kNoUser = 0xFFFFFFFF;
/// The guest user the keyboard and mouse belong to (input_player1/2), or
/// kNoUser.
uint32_t KeyboardUser();

/// Bumped when the devices must be assigned again (rex_input_pad_guest).
uint32_t AssignmentGeneration();

/// Create a default InputSystem with SDL + NOP drivers.
/// In tool mode, only the NOP driver is added.
std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode);

}  // namespace rex::input

/// GetLastInput for the game, by name (GetProcAddress): 0 none yet,
/// 1 keyboard / mouse, 2 gamepad.
extern "C" uint32_t rex_input_last_input();

/// KeyboardUser for the game, by name (GetProcAddress): 0xFFFFFFFF = nobody.
extern "C" uint32_t rex_input_keyboard_user();

/// Guest sign-in with one gamepad (the automatic assignment, which gives the
/// keyboard and the pad to player 1): `on` hands the pad to player 2, the
/// keyboard and mouse stay player 1's; 0 gives it back. Returns 1 when the
/// assignment is the automatic one with exactly one gamepad.
extern "C" uint32_t rex_input_pad_guest(uint32_t on);

/// InputSystem::ReadPads for the game's own screens, by name (GetProcAddress):
/// `triggers` holds left and right, `thumbs` lx, ly, rx, ry. A driver still asks
/// its active callback, which must let this call through.
extern "C" uint32_t rex_input_read_pads(uint16_t* buttons, uint8_t* triggers, int16_t* thumbs);

/// The gamepads (physical devices) the assignment last saw.
extern "C" uint32_t rex_input_gamepads();

/// The devices the assignment last saw, by name ("(synthetic)" after the
/// keyboard / stand-in), into `out` (cut to `size`); returns the full length.
extern "C" uint32_t rex_input_devices(char* out, uint32_t size);
