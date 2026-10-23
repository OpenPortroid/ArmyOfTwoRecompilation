// frame_time.cpp - Game speed above 30 FPS.
//
// The frame sync tick (sub_82A30DE8, r3 = frame sync object, r4 = the
// engine's DeltaSeconds as a double) replaces the frame's delta time with a
// smoothed one: [+212] ms, an average of 10 samples (+216..+252) that the
// players' frame times feed. Made for 30 FPS (the console's rate), the ring
// keeps taking in 33 ms samples now and then even when frames are 8-12 ms, so
// above 60 FPS the average came out too long and the whole game ran up to
// 1.5x fast - or not, depending on how those samples happened to fall.
//
// Single player: the engine's own measured delta time (exact: it sums to
// the real time) is put back after the tick. Online co-op keeps the game's
// value: both machines must step the simulation by the same amount.
//
// With ao2_decouple (decouple.cpp) render-only frames skip the tick, and a
// simulation step reports and uses the fixed step length - online too: both
// machines then use the same ao2_sim_hz.
#include <algorithm>
#include <bit>
#include <cstring>

#include <rex/ppc.h>

#include "coop.h"
#include "decouple.h"
#include "frame_spikes.h"

void CoopDebugSyncTick(uint32_t self, uint8_t *base, double tick_ms);
double CoopDebugNowMs();
void CoopNetAfterSyncTick(PPCContext &ctx, uint8_t *base);

namespace
{
  // A frame after a load or a hitch: at most this long (the game's own
  // smoothing would have spread it out).
  constexpr double kMaxDeltaSeconds = 0.1;

  double LoadF64(const uint8_t *base, uint32_t addr)
  {
    uint64_t v;
    std::memcpy(&v, base + addr, 8);
    return std::bit_cast<double>(std::byteswap(v));
  }
  void StoreF64(uint8_t *base, uint32_t addr, double d)
  {
    uint64_t v = std::byteswap(std::bit_cast<uint64_t>(d));
    std::memcpy(base + addr, &v, 8);
  }
} // namespace

extern "C" REX_FUNC(__imp__sub_82A30DE8);
extern "C" REX_FUNC(sub_82A30DE8)
{
  uint32_t self = ctx.r3.u32;
  uint32_t delta_addr = ctx.r4.u32;
  double sim_dt = 0.0;
  bool render_only = ao2::decouple::BeginFrame(ctx, base, &sim_dt);
  if (ao2::spikes::Enabled())
  {
    ao2::spikes::Mark(ao2::spikes::kGameFrame, render_only ? 0.0 : 1.0);
  }
  if (render_only)
  {
    return;
  }
  if (delta_addr && sim_dt > 0.0)
  {
    // The frame time this player feeds into the lockstep's average.
    StoreF64(base, delta_addr, sim_dt);
  }
  double measured = delta_addr ? LoadF64(base, delta_addr) : 0.0;
  double start_ms = CoopDebugNowMs();
  ao2::decouple::SetInSyncTick(true);
  __imp__sub_82A30DE8(ctx, base);
  ao2::decouple::SetInSyncTick(false);
  double tick_ms = CoopDebugNowMs() - start_ms;
  if (sim_dt > 0.0)
  {
    ao2::decouple::AfterSyncTick(base, tick_ms);
  }
  if (delta_addr && sim_dt > 0.0)
  {
    StoreF64(base, delta_addr, sim_dt);
  }
  else if (delta_addr && measured > 0.0 && !ao2::coop::OnlineLockstep(base))
  {
    StoreF64(base, delta_addr, std::min(measured, kMaxDeltaSeconds));
  }
  CoopDebugSyncTick(self, base, tick_ms);
  CoopNetAfterSyncTick(ctx, base);
}
