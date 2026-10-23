// frame_spikes.h - What the threads did before a long frame (frame_spikes.cpp).
#pragma once

namespace ao2::spikes
{
  enum Event : unsigned char
  {
    kGameFrame,     // game thread: frame begins (arg 1 = simulation step, 0 = draw only)
    kEngineTick,    // game thread: GEngine->Tick ended (arg = ms)
    kLimiterSleep,  // game thread: frame limiter sleep ended (arg = ms)
    kRenderFence,   // game thread: wait for the rendering thread ended (arg = ms)
    kGpuIdle,       // GPU thread: waited for the guest's commands (arg = ms)
    kGpuFirstDraw,  // GPU thread: the first draw after the last swap
    kGpuFence,      // GPU thread: wait for a frame slot's GPU fence ended (arg = ms)
    kGpuSwap,       // GPU thread: the guest's swap reached the renderer
    kGpuSubmitted,  // GPU thread: the frame's commands submitted, present handed over (arg = ms waited for the last present)
    kSamplePdh,     // overlay sampler: GPU load and memory counters collected (arg = ms)
    kSampleTemperature, // overlay sampler: GPU temperature queried (arg = ms)
    kSampleDxgi,    // overlay sampler: the game's video memory queried (arg = ms)
    kPaceSlot,      // present thread: a frame reached the pacer (arg = ms before its slot, < 0 late)
    kPaceMargin,    // present thread: the margin the pacing grid wants (arg = ms)
    kPaced,         // present thread: pacing wait ended (arg = ms)
    kPresented,     // present thread: Present returned (arg = ms in Present)
    kEventCount
  };

  // --ao2_spike_log: Mark is only worth calling when this is true.
  bool Enabled();
  void Mark(Event event, double arg = 0.0);
  // Present thread, after kPresented: logs the recent events when the
  // interval since the previous present was long.
  void AfterPresent(double interval_ms, double period_ms);
} // namespace ao2::spikes
