// native_graphics_system.h - Replacement for ReXGlue's Xenos GPU emulation.
//
// Plugged into the runtime as its IGraphicsSystem (RuntimeConfig::graphics),
// so rexgpu-xenos is never loaded. The guest D3D keeps writing PM4 command
// buffers; they are consumed synchronously when the guest moves the ring
// write pointer and rendered natively with D3D12 (renderer.cpp).
#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace ao2::gpu
{
  std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeGraphicsSystem();

  // The window to present to (HWND); set once the app window exists.
  void SetPresentWindow(void *native_window_handle);
  void *PresentWindow();
} // namespace ao2::gpu
