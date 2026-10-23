// interp_time.cpp - The time of a frame drawn between simulation steps.
//
// With ao2_decouple (decouple.cpp) WorldInfo.TimeSeconds moves only on
// simulation steps, and every frame's view family gets it as its world time
// (FSceneViewFamily ctor sub_826179A0: f1 -> +28 CurrentWorldTime, f2 -> +32
// CurrentRealTime; UGameViewportClient::Draw sub_828573C0 passes WorldInfo
// +824 TimeSeconds and +828 RealTimeSeconds, WorldInfo = [[[GWorld +80] +140]]).
// Materials animated by time (water, fire, panning textures) then moved only
// at ao2_sim_hz - in steps of 33 ms at 30 Hz.
//
// Here a frame's view family gets the step's time moved on by how far the
// frame is past the step (decouple Alpha) at the rate the time went over the
// last step: paused or slowed down time stays so. The time is moved forward,
// not placed between the last two steps like the camera: the rendering thread
// writes it into every drawn primitive's and actor's LastRenderTime
// (sub_8261E8B8, read by the simulation), which then stays between the last
// step's TimeSeconds and the next one's, as without decoupling.
//
// Only the times equal to the step's are changed (scene captures with times
// of their own are left alone).
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "decouple.h"

REXCVAR_DEFINE_BOOL(ao2_interp_time, true, "Gameplay",
                    "With ao2_decouple: animated materials move on every drawn frame, not only on simulation steps");

namespace
{
  constexpr uint32_t kWorld = 0x831F2CC8; // GWorld
  constexpr uint32_t kTimeSeconds = 824;  // WorldInfo
  constexpr uint32_t kRealTimeSeconds = 828;

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }
  float LoadF32(const uint8_t *base, uint32_t addr)
  {
    return std::bit_cast<float>(LoadU32(base, addr));
  }

  struct Clock
  {
    float last = 0;
    float rate = 0; // seconds per step

    void Step(float now, float max_rate)
    {
      float d = now - last;
      rate = d > 0 && d <= max_rate ? d : 0.0f;
      last = now;
    }
    void Move(PPCRegister &f, float alpha) const
    {
      if (rate > 0 && float(f.f64) == last)
      {
        f.f64 = double(last + alpha * rate);
      }
    }
  };

  // Game thread only.
  uint32_t g_step = 0;
  Clock g_world, g_real;

  uint32_t WorldInfo(const uint8_t *base)
  {
    uint32_t world = LoadU32(base, kWorld);
    uint32_t level = world ? LoadU32(base, world + 80) : 0;
    uint32_t list = level ? LoadU32(base, level + 140) : 0;
    return list ? LoadU32(base, list) : 0;
  }
} // namespace

// FSceneViewFamily::FSceneViewFamily(this, render target, scene, show flags,
// ..., f1 world time, f2 real time).
extern "C" REX_FUNC(__imp__sub_826179A0);
extern "C" REX_FUNC(sub_826179A0)
{
  if (REXCVAR_GET(ao2_interp_time) && ao2::decouple::Active())
  {
    if (uint32_t info = WorldInfo(base))
    {
      // A step longer than two simulation periods (loading, a pause): no rate.
      float max_rate = float(2.0 * ao2::decouple::SimPeriodMs() / 1000.0);
      if (ao2::decouple::WorldStep() != g_step)
      {
        g_step = ao2::decouple::WorldStep();
        g_world.Step(LoadF32(base, info + kTimeSeconds), max_rate);
        g_real.Step(LoadF32(base, info + kRealTimeSeconds), max_rate);
      }
      float alpha = float(ao2::decouple::Alpha());
      g_world.Move(ctx.f1, alpha);
      g_real.Move(ctx.f2, alpha);
    }
  }
  __imp__sub_826179A0(ctx, base);
}
