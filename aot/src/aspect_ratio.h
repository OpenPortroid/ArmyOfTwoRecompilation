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
  // The height a render target texture of width x height was laid out for:
  // the one asked for when the screen's height cut it lower, else its own.
  uint32_t LayoutHeight(uint32_t width, uint32_t height);
  // Wider than 16:9: puts the widgets of UI scene `scene` laid out in fractions
  // of the viewport into its 16:9 middle (call for every scene on the stack).
  void FitUiScene(uint8_t *base, uint32_t scene);
} // namespace ao2::aspect
