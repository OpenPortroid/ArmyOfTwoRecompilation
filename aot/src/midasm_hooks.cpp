// midasm_hooks.cpp - Game logic patches applied through codegen mid-asm hooks.
#include "midasm_hooks.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>
#include <rex/runtime.h>

#include "decouple.h"

REXCVAR_DECLARE(bool, ao2_fps_unlock);
REXCVAR_DECLARE(int32_t, ao2_fps_unlock_mode);
REXCVAR_DECLARE(int32_t, ao2_present_interval);
REXCVAR_DECLARE(int32_t, ao2_fps_target);
REXCVAR_DECLARE(int32_t, ao2_native_gpu_vblank_hz);
REXCVAR_DECLARE(bool, ao2_anisotropic_16x);

REXCVAR_DEFINE_BOOL(ao2_limiter_catch_up, true, "Gameplay",
                    "With ao2_decouple: the frame limiter keeps a schedule of 1 / ao2_fps_target, so the frames "
                    "after a long one (a simulation step) come sooner and the average reaches the cap");

namespace
{
  constexpr uint32_t kLastFrameSeconds = 0x831E5E90; // double: the end of the last frame's wait

  double g_due = 0;           // when the last frame was due on the schedule (guest seconds); 0 = none
  double g_frame_seconds = 0; // this frame's time from the end of the last one

  double LastFrameSeconds()
  {
    static const uint8_t *base = rex::Runtime::instance()->virtual_membase();
    uint64_t bits = __builtin_bswap64(*reinterpret_cast<const volatile uint64_t *>(base + kLastFrameSeconds));
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
} // namespace

// Frame rate cap of the unlocked game (also decouple.cpp).
int32_t FpsTarget()
{
  int32_t target = REXCVAR_GET(ao2_fps_target);
  if (target > 0)
  {
    return std::clamp(target, 20, 1000);
  }
  switch (std::clamp(REXCVAR_GET(ao2_fps_unlock_mode), 0, 2))
  {
  case 0: return 240;
  case 1: return 60;
  default: return 30;
  }
}

// Frame limiter: f0 was just loaded with K; the limiter then waits for
// (K / f1) - elapsed(f30). K = f1 * frame makes the frame time at least
// frame.
void AO2FrameLimiterHook(PPCRegister &f0, PPCRegister &f1, PPCRegister &f30)
{
  (void)f30;
  if (!REXCVAR_GET(ao2_fps_unlock) || !(f1.f64 > 0.0))
  {
    return;
  }
  // The wait ends 1 / target after the last frame's end, or, catching up, on
  // the schedule: a simulation step's frame takes the whole tick (longer than
  // a frame at 240), and without catching up every one of them cut the rate.
  // Only with decoupling: the step length is fixed there, so the frame times
  // do not reach the simulation.
  double frame = 1.0 / double(FpsTarget());
  if (REXCVAR_GET(ao2_limiter_catch_up) && ao2::decouple::Active())
  {
    double last = LastFrameSeconds();
    // A frame or more off the schedule (a hitch, a load, frames that cannot
    // keep up): it starts again at the last frame.
    if (g_due == 0 || std::abs(last - g_due) > frame)
    {
      g_due = last;
    }
    g_due += frame;
    // Never no wait at all (see midasm_hooks.toml).
    frame = std::max(g_due - last, 0.25 * frame);
  }
  else
  {
    g_due = 0;
  }
  g_frame_seconds = frame;
  f0.f64 = f1.f64 * frame;
}

// The frame time the limiter waits for this frame (limiter_wait.cpp).
double FrameLimiterSeconds()
{
  return g_frame_seconds > 0 ? g_frame_seconds : 1.0 / double(FpsTarget());
}

// VdSwap presentation interval: 0 = immediate, 1 = every vblank, 2 = every
// other vblank. Unlocked: every vblank while the cap fits the vblank rate,
// immediate above it.
void AO2PresentIntervalHook(PPCRegister &r10)
{
  if (!REXCVAR_GET(ao2_fps_unlock))
  {
    return;
  }
  int32_t interval = REXCVAR_GET(ao2_present_interval);
  if (interval < 0)
  {
    int32_t target = FpsTarget();
    int32_t vblank = REXCVAR_GET(ao2_native_gpu_vblank_hz);
    interval = target <= vblank / 2 ? 2 : (target <= vblank ? 1 : 0);
  }
  r10.u64 = static_cast<uint64_t>(std::min(interval, 2));
}

void AO2AnisotropyHook(PPCRegister &r5)
{
  if (REXCVAR_GET(ao2_anisotropic_16x))
  {
    r5.u64 = 16;
  }
}
