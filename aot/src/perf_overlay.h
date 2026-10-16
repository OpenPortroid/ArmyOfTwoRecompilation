// perf_overlay.h - The frame rate overlay (see perf_overlay.cpp).
#pragma once

#include <cstdint>

struct ImFontAtlas;

namespace ao2::perf
{
  // ao2_fps_overlay.
  enum Mode : int32_t
  {
    kOff = 0,
    kFps = 1,      // frame rate only
    kDetailed = 2, // frame times, GPU and CPU load, GPU temperature, memory
  };

  // Any thread.
  int32_t GetMode();
  // UI thread.
  void SetMode(int32_t mode);
  void CycleMode();

  // Native renderer: the adapter it renders on, once the device exists.
  void SetAdapter(uint32_t luid_low, int32_t luid_high);
  // Native renderer, GPU thread: a frame went to the swap chain.
  void OnPresent();

  // UI thread, inside an ImGui frame: the overlay window (nothing when off).
  void Draw();

  // The overlay's ImGui font (monospaced, with Cyrillic).
  void ConfigureFonts(ImFontAtlas *atlas);
} // namespace ao2::perf
