// limiter_wait.cpp - The frame limiter sleeps instead of spinning.
//
// The engine's frame limiter (sub_825459F0, the game thread, once a frame)
// waits until 1 / rate has passed since the last frame's time (double at
// 0x831E5E90; "now" is kept at 0x831B7528): a long wait begins with
// appSleep(wait - 2 ms) in whole milliseconds, and the rest - or all of a
// short wait - is the loop "now = appSeconds(); appSleep(0)". The yield
// returns at once here, so the game thread kept a core busy for the last
// milliseconds of every frame (half a core at 120 FPS, a whole one at 240).
//
// With ao2_fps_unlock the frame time is known (midasm hook: 1 / ao2_fps_target,
// or on the schedule with ao2_limiter_catch_up), so the end of the wait is
// known inside appSleep: both calls sleep on a high-resolution
// timer until shortly before it and leave the rest to the engine's loop,
// which ends the frame at the exact time as before. How long before follows
// the timer's lateness (precise_sleep.h): with the window in the background
// it wakes milliseconds late, and a fixed 0.6 ms made every frame late.
#include <windows.h>

#include <chrono>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "frame_spikes.h"
#include "precise_sleep.h"

REXCVAR_DECLARE(bool, ao2_fps_unlock);
REXCVAR_DEFINE_BOOL(ao2_limiter_sleep, true, "Gameplay",
                    "The frame limiter of ao2_fps_unlock sleeps until the frame's time instead of spinning on a core");

REXCVAR_DEFINE_INT32(ao2_limiter_spin_us, 600, "Gameplay",
                     "ao2_limiter_sleep: microseconds before the frame's time left to the engine's loop at least "
                     "(more when the timer wakes later than that)")
    .range(0, 5000);

double FrameLimiterSeconds(); // midasm_hooks.cpp

namespace
{
  constexpr uint32_t kLastFrameSeconds = 0x831E5E90; // double
  constexpr uint32_t kNowSeconds = 0x831B7528;       // double
  constexpr uint32_t kAfterLongSleep = 0x82545B28;   // return address of appSleep(wait - 2 ms)
  constexpr uint32_t kAfterYield = 0x82545B60;       // return address of the loop's appSleep(0)

  double LoadDouble(uint8_t *base, uint32_t address)
  {
    uint64_t bits = __builtin_bswap64(*reinterpret_cast<volatile uint64_t *>(base + address));
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  thread_local ao2::PreciseSleeper t_sleeper;
} // namespace

namespace ao2
{
  // appSleep called by the frame limiter: true if the wait was done here.
  bool LimiterSleep(PPCContext &ctx, uint8_t *base)
  {
    if ((ctx.lr != kAfterYield && ctx.lr != kAfterLongSleep) || !REXCVAR_GET(ao2_fps_unlock) ||
        !REXCVAR_GET(ao2_limiter_sleep))
    {
      return false;
    }
    double remaining = LoadDouble(base, kLastFrameSeconds) + FrameLimiterSeconds() - LoadDouble(base, kNowSeconds);
    double spin = double(REXCVAR_GET(ao2_limiter_spin_us)) * 1e-6;
    if (!(remaining > spin) || remaining > 0.1)
    {
      // The loop's turn (a yield), or not the wait this is meant for.
      return ctx.lr == kAfterLongSleep && remaining <= spin;
    }
    auto start = std::chrono::steady_clock::now();
    t_sleeper.Sleep(remaining, spin);
    if (ao2::spikes::Enabled())
    {
      ao2::spikes::Mark(ao2::spikes::kLimiterSleep,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    return true;
  }
} // namespace ao2
