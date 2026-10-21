// renderer.h - Native D3D12 renderer driven by the PM4 command processor.
#pragma once

#include <cstdint>

namespace ao2::gpu
{
  class Pm4Consumer;
  class ShaderCache;
  struct DrawPacket;

  // All functions are called on the guest thread that kicks the ring, with the
  // ring mutex held. Initialization is lazy; on failure the renderer stays disabled.
  void RendererDraw(Pm4Consumer &pm4, const DrawPacket &draw, ShaderCache &shaders);
  void RendererResolve(Pm4Consumer &pm4, const DrawPacket &draw);
  // Ends the frame: presents the texture resolved to `frontbuffer` (physical)
  // to the game window.
  void RendererSwap(uint32_t frontbuffer, uint32_t width, uint32_t height);
  // Guest occlusion query begin/end (EVENT_WRITE_ZPD; `address` = RB_SAMPLE_COUNT_ADDR).
  // The draws in between are counted on the GPU and the end struct is written
  // when they complete. Returns false when the caller must report counts itself
  // (`fallback_samples` is also reported for queries that could not be counted).
  bool RendererOcclusionEvent(uint32_t address, bool end, uint32_t fallback_samples);
  // The command thread has nothing to do (or waits in WAIT_REG_MEM): submits
  // recorded work that ended occlusion queries and reports finished ones.
  // Returns true while results are still pending on the GPU.
  bool RendererIdle();
  // Occlusion query results were written to guest memory: a counter that
  // changes with each batch (wait on it with WaitOnAddress).
  volatile uint32_t &OcclusionResultGeneration();
  // The rendering thread waits for occlusion results (ao2_occlusion_wait, or
  // a menu is open; occlusion_wait.cpp).
  bool OcclusionWaitOn();
  // A MEM_WRITE packet stores to `address` (physical, endian bits included).
  void RendererMemWrite(uint32_t address);
  // Counts written to guest memory outside the renderer (pm4.cpp).
  void RendererNotifyOcclusion();
  // Changes the resolution scale (ao2_native_gpu_resolution_scale, in steps
  // of a quarter) from the next frame on. Any thread; false when the native
  // renderer is not presenting (another GPU backend, not started yet).
  bool RendererSetResolutionScale(double scale);
  // The menu probe (menu_mouse.cpp): the draws that sample a texture of
  // `width` x `height` (a menu's render target put on the Shell level's
  // slabs; 0 = off) also write that texture's coordinates, and the ones at
  // guest picture pixel x, y are read back. Any thread.
  void RendererSetMenuProbe(uint32_t width, uint32_t height, uint32_t x, uint32_t y);
  // Those coordinates (0 to 1 over the texture) from the last finished frame;
  // false when the texture of that size was not drawn at that pixel.
  bool RendererMenuProbe(uint32_t width, uint32_t height, float &u, float &v);
  // Display gamma ramp for the following presents (Pm4Consumer::gamma_table/pwl).
  void RendererSetGammaRamp(const uint32_t table[256], const uint32_t pwl[128][3], uint32_t luta_control);

  // Shrinks a (non-fullscreen) window that does not fit its monitor's work
  // area to the largest client area of the same aspect that does, centered. Asynchronous
  // (safe from any thread).
  void FitWindowToMonitor(void *native_window_handle);

  // native_graphics_system.cpp
  void NativeLog(const char *format, ...);
} // namespace ao2::gpu
