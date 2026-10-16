// midasm_hooks.cpp - Game logic patches applied through codegen mid-asm hooks.
#include "midasm_hooks.h"

#include <algorithm>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, ao2_fps_unlock);
REXCVAR_DECLARE(int32_t, ao2_fps_unlock_mode);
REXCVAR_DECLARE(int32_t, ao2_present_interval);
REXCVAR_DECLARE(int32_t, ao2_fps_target);
REXCVAR_DECLARE(int32_t, ao2_native_gpu_vblank_hz);
REXCVAR_DECLARE(bool, ao2_anisotropic_16x);

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
// (K / f1) - elapsed(f30). K = f1 / target makes the frame time at least
// 1 / target (never zero: see midasm_hooks.toml).
void AO2FrameLimiterHook(PPCRegister &f0, PPCRegister &f1, PPCRegister &f30)
{
  (void)f30;
  if (!REXCVAR_GET(ao2_fps_unlock) || !(f1.f64 > 0.0))
  {
    return;
  }
  f0.f64 = f1.f64 / double(FpsTarget());
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
