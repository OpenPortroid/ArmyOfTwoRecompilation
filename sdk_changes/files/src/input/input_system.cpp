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

#include <algorithm>
#include <atomic>
#include <bit>
#include <string_view>
#include <utility>
#include <cmath>

#include <rex/dbg.h>
#include <rex/input/device_assignment.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/input/state_merge.h>
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(input_backend, "sdl", "Input", "Input backend: sdl, xinput")
    .allowed({"sdl", "xinput"});

REXCVAR_DEFINE_BOOL(guide_button, false, "Input", "Enable guide button pass-through");

// Gamepad remapping: each Xbox 360 control of the guest takes the physical
// controls listed (comma-separated; empty = never pressed). Names: A, B, X,
// Y, LB, RB, LT, RT, L3, R3, Up, Down, Left, Right, Back, Start. A trigger
// as a button is pressed past the XInput threshold, a button as a trigger is
// fully pulled. Keyboard and mouse are not affected (keybind_*).
#define PADBIND(name, def, desc) \
  REXCVAR_DEFINE_STRING(padbind_##name, def, "Input/Padbinds", desc)
PADBIND(a, "A", "A button");
PADBIND(b, "B", "B button");
PADBIND(x, "X", "X button");
PADBIND(y, "Y", "Y button");
PADBIND(left_shoulder, "LB", "Left shoulder");
PADBIND(right_shoulder, "RB", "Right shoulder");
PADBIND(left_trigger, "LT", "Left trigger");
PADBIND(right_trigger, "RT", "Right trigger");
PADBIND(lstick_press, "L3", "Left stick press");
PADBIND(rstick_press, "R3", "Right stick press");
PADBIND(dpad_up, "Up", "D-pad up");
PADBIND(dpad_down, "Down", "D-pad down");
PADBIND(dpad_left, "Left", "D-pad left");
PADBIND(dpad_right, "Right", "D-pad right");
PADBIND(back, "Back", "Back button");
PADBIND(start, "Start", "Start button");
#undef PADBIND
REXCVAR_DEFINE_STRING(padbind_left_stick, "LS", "Input/Padbinds",
                      "Physical stick acting as the left stick: LS or RS");
REXCVAR_DEFINE_STRING(padbind_right_stick, "RS", "Input/Padbinds",
                      "Physical stick acting as the right stick: LS or RS");
REXCVAR_DEFINE_BOOL(pad_invert_left_y, false, "Input/Padbinds", "Invert the left stick's Y axis");
REXCVAR_DEFINE_BOOL(pad_invert_right_y, false, "Input/Padbinds",
                    "Invert the right stick's Y axis");

namespace rex::input {

namespace {

// Synthetic devices are parked past every physical ordinal so they cannot push
// a real pad off guest user 0. SlotAssignment routes them by their synthetic
// flag and never reads this value.
constexpr uint32_t kSyntheticOrdinal = UINT32_MAX;

std::atomic<uint32_t> g_last_input{0};

}  // namespace

void NoteLastInput(LastInput kind) {
  g_last_input.store(static_cast<uint32_t>(kind), std::memory_order_relaxed);
}

LastInput GetLastInput() {
  // Player 1 without the keyboard (split-screen) always has a pad in hand.
  if (KeyboardUser() != 0) {
    return LastInput::kGamepad;
  }
  return static_cast<LastInput>(g_last_input.load(std::memory_order_relaxed));
}

namespace {

// Physical controls a guest control can be bound to: a button bit, or a
// trigger.
constexpr uint32_t kLeftTrigger = 0x10000;
constexpr uint32_t kRightTrigger = 0x20000;
constexpr uint8_t kTriggerThreshold = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD

uint32_t PadSource(std::string_view name) {
  static constexpr std::pair<std::string_view, uint32_t> kNames[] = {
      {"Up", 0x0001},  {"Down", 0x0002}, {"Left", 0x0004}, {"Right", 0x0008},
      {"Start", 0x0010}, {"Back", 0x0020}, {"L3", 0x0040},  {"R3", 0x0080},
      {"LB", 0x0100},  {"RB", 0x0200},   {"A", 0x1000},    {"B", 0x2000},
      {"X", 0x4000},   {"Y", 0x8000},    {"LT", kLeftTrigger}, {"RT", kRightTrigger},
  };
  for (const auto& [n, bits] : kNames) {
    if (n == name) {
      return bits;
    }
  }
  return 0;
}

uint32_t PadSources(const std::string& value) {
  uint32_t mask = 0;
  std::string_view rest = value;
  while (!rest.empty()) {
    size_t comma = rest.find(',');
    std::string_view token = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    while (!token.empty() && token.front() == ' ') {
      token.remove_prefix(1);
    }
    while (!token.empty() && token.back() == ' ') {
      token.remove_suffix(1);
    }
    mask |= PadSource(token);
  }
  return mask;
}

struct PadRemap {
  bool identity = true;
  uint32_t buttons[16] = {};  // sources of guest button bit i (Guide, bit 10, passes through)
  uint32_t left_trigger = kLeftTrigger;
  uint32_t right_trigger = kRightTrigger;
  bool left_from_right = false;  // the left stick reads the physical right one
  bool right_from_left = false;  // the right stick reads the physical left one
  bool invert_left_y = false;
  bool invert_right_y = false;
};

// The cvars come from the command line and the settings files before the
// guest polls, so they are read once.
const PadRemap& Remap() {
  static const PadRemap remap = [] {
    PadRemap r;
    const std::pair<uint32_t, std::string> buttons[] = {
        {0x0001, REXCVAR_GET(padbind_dpad_up)},
        {0x0002, REXCVAR_GET(padbind_dpad_down)},
        {0x0004, REXCVAR_GET(padbind_dpad_left)},
        {0x0008, REXCVAR_GET(padbind_dpad_right)},
        {0x0010, REXCVAR_GET(padbind_start)},
        {0x0020, REXCVAR_GET(padbind_back)},
        {0x0040, REXCVAR_GET(padbind_lstick_press)},
        {0x0080, REXCVAR_GET(padbind_rstick_press)},
        {0x0100, REXCVAR_GET(padbind_left_shoulder)},
        {0x0200, REXCVAR_GET(padbind_right_shoulder)},
        {0x1000, REXCVAR_GET(padbind_a)},
        {0x2000, REXCVAR_GET(padbind_b)},
        {0x4000, REXCVAR_GET(padbind_x)},
        {0x8000, REXCVAR_GET(padbind_y)},
    };
    for (const auto& [bit, value] : buttons) {
      uint32_t sources = PadSources(value);
      r.buttons[std::countr_zero(bit)] = sources;
      r.identity &= sources == bit;
    }
    r.left_trigger = PadSources(REXCVAR_GET(padbind_left_trigger));
    r.right_trigger = PadSources(REXCVAR_GET(padbind_right_trigger));
    r.left_from_right = REXCVAR_GET(padbind_left_stick) == "RS";
    r.right_from_left = REXCVAR_GET(padbind_right_stick) == "LS";
    r.invert_left_y = REXCVAR_GET(pad_invert_left_y);
    r.invert_right_y = REXCVAR_GET(pad_invert_right_y);
    r.identity &= r.left_trigger == kLeftTrigger && r.right_trigger == kRightTrigger &&
                  !r.left_from_right && !r.right_from_left && !r.invert_left_y &&
                  !r.invert_right_y;
    return r;
  }();
  return remap;
}

// A physical pad's state as the guest's controls (padbind_*).
void RemapPad(X_INPUT_GAMEPAD& pad) {
  const PadRemap& r = Remap();
  if (r.identity) {
    return;
  }
  uint16_t in_buttons = pad.buttons;
  uint8_t in_lt = pad.left_trigger, in_rt = pad.right_trigger;
  int16_t lx = pad.thumb_lx, ly = pad.thumb_ly, rx = pad.thumb_rx, ry = pad.thumb_ry;

  auto pressed = [&](uint32_t sources) {
    return (in_buttons & sources & 0xFFFF) != 0 ||
           ((sources & kLeftTrigger) && in_lt > kTriggerThreshold) ||
           ((sources & kRightTrigger) && in_rt > kTriggerThreshold);
  };
  auto pulled = [&](uint32_t sources) {
    uint8_t v = (in_buttons & sources & 0xFFFF) ? 255 : 0;
    if (sources & kLeftTrigger) {
      v = std::max(v, in_lt);
    }
    if (sources & kRightTrigger) {
      v = std::max(v, in_rt);
    }
    return v;
  };
  auto flip = [](int16_t v) { return int16_t(std::min(-int32_t(v), 32767)); };

  uint16_t out = in_buttons & 0x0400;
  for (uint32_t i = 0; i < 16; ++i) {
    if (r.buttons[i] && pressed(r.buttons[i])) {
      out |= uint16_t(1u << i);
    }
  }
  pad.buttons = out;
  pad.left_trigger = pulled(r.left_trigger);
  pad.right_trigger = pulled(r.right_trigger);
  int16_t new_ly = r.left_from_right ? ry : ly;
  int16_t new_ry = r.right_from_left ? ly : ry;
  pad.thumb_lx = r.left_from_right ? rx : lx;
  pad.thumb_ly = r.invert_left_y ? flip(new_ly) : new_ly;
  pad.thumb_rx = r.right_from_left ? lx : rx;
  pad.thumb_ry = r.invert_right_y ? flip(new_ry) : new_ry;
}

}  // namespace

InputSystem::InputSystem(rex::ui::Window* window) : window_(window) {}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  return X_STATUS_SUCCESS;
}

void InputSystem::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  // device_owners_ holds raw driver pointers.
  devices_.clear();
  device_owners_.clear();
  drivers_.clear();
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  drivers_.push_back(std::move(driver));
}

void InputSystem::AttachWindow(rex::ui::Window* window) {
  window_ = window;
  for (auto& driver : drivers_) {
    driver->OnWindowAvailable(window);
  }
}

void InputSystem::SetActiveCallback(std::function<bool()> callback) {
  for (auto& driver : drivers_) {
    driver->set_is_active_callback(callback);
  }
}

void InputSystem::SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment) {
  std::lock_guard<std::mutex> lock(mutex_);
  assignment_ = std::move(assignment);
  if (assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

void InputSystem::RefreshDevices() {
  std::vector<DeviceInfo> seen;
  std::vector<InputDriver*> owners;
  std::vector<DeviceInfo> enumerated;
  for (auto& driver : drivers_) {
    enumerated.clear();
    driver->EnumerateDevices(enumerated);
    for (auto& info : enumerated) {
      seen.push_back(info);
      owners.push_back(driver.get());
    }
  }

  // Carry forward ordinals already handed out, so a device keeps its guest user
  // when another pad is unplugged.
  bool changed = seen.size() != devices_.size();
  std::vector<bool> fresh(seen.size(), false);
  for (size_t i = 0; i < seen.size(); i++) {
    auto existing = std::find_if(devices_.begin(), devices_.end(),
                                 [&](const DeviceInfo& d) { return d.id == seen[i].id; });
    if (existing != devices_.end()) {
      seen[i].ordinal = existing->ordinal;
      continue;
    }
    fresh[i] = true;
    changed = true;
  }

  // Runs after the carry-forward pass so a new device cannot take an ordinal a
  // live one is still holding. Lowest free rather than a growing counter,
  // because a reconnected pad arrives as a new device and would otherwise walk
  // off the end of the guest users.
  for (size_t i = 0; i < seen.size(); i++) {
    if (!fresh[i]) {
      continue;
    }
    // Only physical devices consume an ordinal, so the first pad to connect is
    // guest user 0 however many synthetic devices enumerated ahead of it.
    if (seen[i].synthetic) {
      seen[i].ordinal = kSyntheticOrdinal;
      fresh[i] = false;
      continue;
    }
    auto taken = [&](uint32_t candidate) {
      for (size_t j = 0; j < seen.size(); j++) {
        if (!fresh[j] && !seen[j].synthetic && seen[j].ordinal == candidate) {
          return true;
        }
      }
      return false;
    };
    uint32_t ordinal = 0;
    while (taken(ordinal)) {
      ordinal++;
    }
    seen[i].ordinal = ordinal;
    fresh[i] = false;
  }

  for (const auto& old : devices_) {
    if (std::none_of(seen.begin(), seen.end(),
                     [&](const DeviceInfo& d) { return d.id == old.id; })) {
      active_devices_.Forget(old.id);
      changed = true;
    }
  }

  std::vector<size_t> order(seen.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  // Stable: synthetic devices share one ordinal, and their relative order
  // decides which answers GetCapabilities when no pad is attached.
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return seen[a].ordinal < seen[b].ordinal; });

  std::vector<DeviceId> pads_before = Player1Pads();
  devices_.clear();
  device_owners_.clear();
  for (size_t i : order) {
    devices_.push_back(seen[i]);
    device_owners_.push_back(owners[i]);
  }

  if (changed && assignment_) {
    assignment_->OnDevicesChanged(devices_);
    // A pad plugged in for player 1 takes the button prompts at once, the
    // last one unplugged gives them back to the keys.
    std::vector<DeviceId> pads_after = Player1Pads();
    if (std::any_of(pads_after.begin(), pads_after.end(), [&](DeviceId id) {
          return std::find(pads_before.begin(), pads_before.end(), id) == pads_before.end();
        })) {
      NoteLastInput(LastInput::kGamepad);
    } else if (!pads_before.empty() && pads_after.empty()) {
      NoteLastInput(LastInput::kKeyboardMouse);
    }
  }
}

std::vector<DeviceId> InputSystem::Player1Pads() const {
  std::vector<DeviceId> ids;
  if (assignment_) {
    assignment_->DevicesForUser(0, ids);
  }
  std::erase_if(ids, [&](DeviceId id) {
    const DeviceInfo* info = DeviceInfoFor(id);
    return !info || info->synthetic;
  });
  return ids;
}

InputDriver* InputSystem::DriverForDevice(DeviceId id) {
  for (size_t i = 0; i < devices_.size(); i++) {
    if (devices_[i].id == id) {
      return device_owners_[i];
    }
  }
  return nullptr;
}

const DeviceInfo* InputSystem::DeviceInfoFor(DeviceId id) const {
  for (const auto& device : devices_) {
    if (device.id == id) {
      return &device;
    }
  }
  return nullptr;
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");
  std::lock_guard<std::mutex> lock(mutex_);
  if (!out_caps || !assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);
  if (ids.empty()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Prefer the pad in hand, so button glyphs follow it rather than whichever
  // device enumerated first.
  DeviceId chosen = active_devices_.Active(user_index);
  if (std::find(ids.begin(), ids.end(), chosen) == ids.end()) {
    chosen = ids.front();
  }

  auto* driver = DriverForDevice(chosen);
  if (!driver) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return driver->GetDeviceCapabilities(chosen, flags, out_caps);
}

X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");
  std::lock_guard<std::mutex> lock(mutex_);
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  X_INPUT_STATE merged = {};
  bool any = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_INPUT_STATE state = {};
    if (driver->GetDeviceState(id, &state) != X_ERROR_SUCCESS) {
      continue;
    }
    const DeviceInfo* info = DeviceInfoFor(id);
    bool pad = info && !info->synthetic;
    if (pad) {
      RemapPad(state.gamepad);
    }
    active_devices_.Observe(user_index, id, state.gamepad);
    if (user_index == 0 && pad && !IsNeutral(state.gamepad)) {
      NoteLastInput(LastInput::kGamepad);
    }
    if (!any) {
      merged = state;
      any = true;
    } else {
      MergeInto(merged, state);
    }
  }

  if (!any) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_state) {
    *out_state = merged;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");
  std::lock_guard<std::mutex> lock(mutex_);
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  // Every pad on this user belongs to the same player, so all of them buzz.
  // Only pads decide the result: synthetic devices accept any vibration and
  // would otherwise report success for a pad that never rumbled.
  bool any_pad = false;
  bool any_synthetic = false;
  bool pad_rumbled = false;
  X_RESULT pad_error = X_ERROR_DEVICE_NOT_CONNECTED;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    const DeviceInfo* info = DeviceInfoFor(id);
    if (!driver || !info) {
      continue;
    }
    X_RESULT result = driver->SetDeviceVibration(id, vibration);
    if (info->synthetic) {
      any_synthetic = true;
      continue;
    }
    any_pad = true;
    if (result == X_ERROR_SUCCESS) {
      pad_rumbled = true;
    } else {
      pad_error = result;
    }
  }
  if (any_pad) {
    return pad_rumbled ? X_ERROR_SUCCESS : pad_error;
  }
  return any_synthetic ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");
  std::lock_guard<std::mutex> lock(mutex_);
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  bool any_connected = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_RESULT result = driver->GetDeviceKeystroke(id, flags, out_keystroke);
    if (result == X_ERROR_SUCCESS) {
      out_keystroke->user_index = static_cast<uint8_t>(user_index);
      return result;
    }
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);

  if (!tool_mode) {
#if REX_PLATFORM_WIN32
    if (REXCVAR_GET(input_backend) == "xinput") {
      auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
      if (xinput_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(xinput_driver));
      }
    }
#endif

    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl_driver));
      }
    }

    // MnK driver (keyboard/mouse -> controller emulation)
    auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
  }

  // NOP driver (primary in tool mode, fallback otherwise)
  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, nop_index));
  input->SetDeviceAssignment(std::make_unique<SlotAssignment>());
  return input;
}

}  // namespace rex::input

extern "C" uint32_t rex_input_last_input() {
  return static_cast<uint32_t>(rex::input::GetLastInput());
}
