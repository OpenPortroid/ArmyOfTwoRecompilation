// coop_input_delay.cpp - How far the online co-op lockstep may run ahead.
//
// The lockstep (coop.h) lets this machine simulate ahead of the last frame
// it has from the partner by a few frames; this player's input takes effect
// that much later (measured: a mouse turn sent on frame N turns the camera
// on frame N + 3 on one PC). sub_82A13B48 (r3 = frame sync object, r4 = out
// frame the partner must have reached) decides it every frame and returns 1
// when the game has to wait: the lead is ([+260] latency + 50 ms, clamped to
// [+68] .. [+72] = 10 .. 500) / [+212] ms, the smoothed frame time.
//
// Made for 30 FPS. Above that it goes wrong both ways: the 50 ms margin kept
// every input 3+ frames late even on a LAN, and without it the lead came out
// as 1 frame - at 120 FPS 8 ms, less than it takes to get a frame's input to
// the partner - so both games waited for each other (~8 ms of every frame)
// and stuck at ~65 FPS, which in turn kept [+212] at ~15 ms and the lead at
// one frame. The lead is counted here in frames of the target frame rate:
// enough for the latency, one frame for sending and a little jitter
// (ao2_coop_input_margin_ms on top). Each machine decides its own lead as
// the game does (the latency differs per machine anyway).
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "decouple.h"

REXCVAR_DECLARE(bool, ao2_fps_unlock);
REXCVAR_DECLARE(int32_t, ao2_fps_unlock_mode);
REXCVAR_DECLARE(int32_t, ao2_fps_target);

REXCVAR_DEFINE_INT32(ao2_coop_input_margin_ms, 0, "Online",
                     "Online co-op: extra input delay in ms on top of the network latency (more = fewer waits on "
                     "a jittery network, less = less input lag); 50 = the game's own rule")
    .range(0, 200);

namespace
{
  constexpr uint32_t kSyncIsNetwork = 0x82A0EC20; // (sync) -> network game
  constexpr int32_t kGameMarginMs = 50;
  constexpr double kJitterMs = 4.0;

  int32_t LoadS32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return int32_t(std::byteswap(v));
  }
  void StoreS32(uint8_t *base, uint32_t addr, int32_t value)
  {
    uint32_t v = std::byteswap(uint32_t(value));
    std::memcpy(base + addr, &v, 4);
  }

  // Frame time the game aims for (midasm_hooks.cpp FpsTarget); with
  // ao2_decouple the lockstep advances once per simulation step.
  double TargetFrameMs()
  {
    if (double sim_ms = ao2::decouple::SimPeriodMs(); sim_ms > 0.0)
    {
      return sim_ms;
    }
    int32_t fps = 60;
    if (REXCVAR_GET(ao2_fps_unlock))
    {
      int32_t target = REXCVAR_GET(ao2_fps_target);
      if (target > 0)
      {
        fps = std::clamp(target, 20, 1000);
      }
      else
      {
        static constexpr int32_t kModes[] = {240, 60, 30};
        fps = kModes[std::clamp(REXCVAR_GET(ao2_fps_unlock_mode), 0, 2)];
      }
    }
    return 1000.0 / fps;
  }
} // namespace

// Lead in frames for a measured latency (also logged by coop_debug.cpp).
int32_t CoopLeadFrames(int32_t latency_ms)
{
  double frame_ms = TargetFrameMs();
  double ms = std::max(latency_ms, 0) + REXCVAR_GET(ao2_coop_input_margin_ms) + kJitterMs;
  return 1 + int32_t(std::ceil(ms / frame_ms));
}

extern "C" REX_FUNC(__imp__sub_82A13B48);
extern "C" REX_FUNC(sub_82A13B48)
{
  uint32_t sync = ctx.r3.u32, out = ctx.r4.u32;
  if (!sync || !out || REXCVAR_GET(ao2_coop_input_margin_ms) == kGameMarginMs)
  {
    __imp__sub_82A13B48(ctx, base);
    return;
  }
  PPCContext saved = ctx;
  rex::runtime::ResolveIndirectFunction(kSyncIsNetwork)(ctx, base);
  bool network = ctx.r3.u32 != 0;
  ctx = saved;
  if (!network)
  {
    __imp__sub_82A13B48(ctx, base);
    return;
  }
  // As the game: the partner must have reached frame - lead; wait (1) if
  // it has not.
  int32_t needed = LoadS32(base, sync + 92) - CoopLeadFrames(LoadS32(base, sync + 260));
  StoreS32(base, out, needed);
  ctx.r3.u64 = needed > 0 && LoadS32(base, sync + 96) < needed ? 1 : 0;
}
