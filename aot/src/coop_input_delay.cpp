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
//
// ao2_coop_input_auto: on top of that margin an extra one that follows the
// network. Each machine counts the frames it had to wait for the partner;
// 6 or more in a second (a jittery or slow link: the stutter) wish for 8 ms
// more, up to kMaxExtraMs - if the partner waits too (when only one machine
// waits, the other is just slower and a margin would not help); 15 s with at
// most one wait a second, 8 ms less.
// A wish goes to the partner over the link (coop_link.cpp) with the lockstep
// frame it holds from, kScheduleFrames ahead, and both machines use the
// larger of the two wishes from that frame on - so the margin stays the same
// on both PCs, as SharedSettings asks of ao2_coop_input_margin_ms.
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "coop_link.h"
#include "decouple.h"
#include "log_files.h"

REXCVAR_DECLARE(bool, ao2_fps_unlock);
REXCVAR_DECLARE(int32_t, ao2_fps_unlock_mode);
REXCVAR_DECLARE(int32_t, ao2_fps_target);

REXCVAR_DEFINE_INT32(ao2_coop_input_margin_ms, 0, "Online",
                     "Online co-op: extra input delay in ms on top of the network latency (more = fewer waits on "
                     "a jittery network, less = less input lag); 50 = the game's own rule")
    .range(0, 200);
REXCVAR_DEFINE_BOOL(ao2_coop_input_auto, true, "Online",
                    "Online co-op: raise the input delay a little while the game keeps waiting for the partner's "
                    "input (a jittery network: stutter) and lower it again once the link is steady; both PCs use "
                    "the larger wish");

namespace
{
  using clock = std::chrono::steady_clock;

  constexpr uint32_t kSyncIsNetwork = 0x82A0EC20; // (sync) -> network game
  constexpr int32_t kGameMarginMs = 50;
  constexpr double kJitterMs = 4.0;

  // The adaptive margin (ao2_coop_input_auto).
  constexpr int32_t kStepMs = 8;
  constexpr int32_t kMaxExtraMs = 64;
  constexpr int32_t kRaiseWaits = 6;     // waits in a second that ask for more
  constexpr int32_t kSteadyWaits = 1;    // ... at most this many for kLowerAfter: less
  constexpr auto kLowerAfter = std::chrono::seconds(15);
  constexpr auto kRaiseCooldown = std::chrono::seconds(3);
  constexpr uint32_t kScheduleFrames = 120; // a wish holds from this many frames on

  struct Auto
  {
    uint32_t last_frame = 0;
    uint32_t last_wait_frame = 0;
    int32_t waits = 0; // in this second
    clock::time_point window, steady_since, last_change;
    int32_t wish = 0;  // this machine's (sent)
    // This machine's and the partner's wish as in force now, and pending.
    int32_t mine = 0, mine_next = 0;
    uint32_t mine_from = 0;
    int32_t partner = 0;
  };
  Auto g_auto;
  std::atomic<int32_t> g_extra_ms{0};    // in force (both machines' larger wish)
  std::atomic<int32_t> g_waits_shown{0}; // waits in the last full second

  FILE *AutoLog()
  {
    static FILE *f = ao2::OpenLogFile("coop_delay.log");
    return f;
  }

  // Every call of the lead check (game thread): counts the frames that had
  // to wait, moves this machine's wish, applies both wishes at their frames.
  void UpdateAuto(uint32_t frame, bool waiting)
  {
    Auto &a = g_auto;
    auto now = clock::now();
    if (frame + 600 < a.last_frame || a.window == clock::time_point{})
    {
      // A new game (the frame count starts over).
      a = Auto{};
      a.window = a.steady_since = a.last_change = now;
      ao2::coop::link::ProposeExtraMs(0, 0);
      g_extra_ms.store(0, std::memory_order_relaxed);
    }
    a.last_frame = frame;
    if (waiting && frame != a.last_wait_frame)
    {
      a.last_wait_frame = frame;
      a.waits++;
    }
    if (now - a.window >= std::chrono::seconds(1))
    {
      g_waits_shown.store(a.waits, std::memory_order_relaxed);
      ao2::coop::link::SetLocalWaits(a.waits);
      int32_t wish = a.wish;
      if (!REXCVAR_GET(ao2_coop_input_auto))
      {
        wish = 0;
      }
      else if (a.waits >= kRaiseWaits && ao2::coop::link::PartnerWaits() >= kRaiseWaits / 2)
      {
        a.steady_since = now;
        if (now - a.last_change >= kRaiseCooldown)
        {
          wish = std::min(a.wish + kStepMs, kMaxExtraMs);
        }
      }
      else if (a.waits > kSteadyWaits)
      {
        a.steady_since = now;
      }
      else if (now - a.steady_since >= kLowerAfter)
      {
        a.steady_since = now;
        wish = std::max(a.wish - kStepMs, 0);
      }
      if (wish != a.wish)
      {
        a.wish = wish;
        a.last_change = now;
        a.mine_next = wish;
        a.mine_from = frame + kScheduleFrames;
        ao2::coop::link::ProposeExtraMs(wish, a.mine_from);
        if (FILE *f = AutoLog())
        {
          std::fprintf(f, "frame %u: %d waits in a second, wish +%d ms from frame %u\n", frame, a.waits, wish,
                       a.mine_from);
          std::fflush(f);
        }
      }
      a.waits = 0;
      a.window = now;
    }
    if (a.mine != a.mine_next && frame >= a.mine_from)
    {
      a.mine = a.mine_next;
    }
    int32_t partner_ms;
    uint32_t partner_from;
    if (ao2::coop::link::PartnerExtraMs(&partner_ms, &partner_from) && frame >= partner_from)
    {
      a.partner = std::clamp(partner_ms, 0, kMaxExtraMs);
    }
    int32_t extra = std::max(a.mine, a.partner);
    if (g_extra_ms.exchange(extra, std::memory_order_relaxed) != extra)
    {
      if (FILE *f = AutoLog())
      {
        std::fprintf(f, "frame %u: input margin +%d ms (this PC +%d, partner +%d)\n", frame, extra, a.mine,
                     a.partner);
        std::fflush(f);
      }
    }
  }

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
  double ms = std::max(latency_ms, 0) + REXCVAR_GET(ao2_coop_input_margin_ms) +
              g_extra_ms.load(std::memory_order_relaxed) + kJitterMs;
  return 1 + int32_t(std::ceil(ms / frame_ms));
}

// For the connection overlay (coop_net_status.cpp): the adaptive margin in
// force and the frames waited for the partner in the last second.
int32_t CoopInputExtraMs()
{
  return g_extra_ms.load(std::memory_order_relaxed);
}
int32_t CoopWaitsPerSecond()
{
  return g_waits_shown.load(std::memory_order_relaxed);
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
  int32_t frame = LoadS32(base, sync + 92);
  int32_t needed = frame - CoopLeadFrames(LoadS32(base, sync + 260));
  StoreS32(base, out, needed);
  bool wait = needed > 0 && LoadS32(base, sync + 96) < needed;
  ctx.r3.u64 = wait ? 1 : 0;
  UpdateAuto(uint32_t(frame), wait);
}
