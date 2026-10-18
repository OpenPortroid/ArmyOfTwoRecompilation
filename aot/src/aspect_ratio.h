// aspect_ratio.h - The game renders at the window's aspect ratio (ao2_aspect_ratio).
#pragma once

#include <cstdint>

namespace ao2::aspect
{
  // The window whose client area "auto" follows (call before the game creates its device).
  void SetWindow(void *native_window_handle);
  // That window (HWND), nullptr before SetWindow.
  void *Window();
  // The guest back buffer size the game was given (1280x720 until the device exists or when unchanged).
  uint32_t GuestWidth();
  uint32_t GuestHeight();
} // namespace ao2::aspect
