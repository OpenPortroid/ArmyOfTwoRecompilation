// decouple.cpp - Rendering decoupled from the simulation.
//
// Every frame the game thread (sub_82547960) runs the frame sync tick
// (sub_82A30DE8, the lockstep in online co-op) and then GEngine->Tick
// (UGameEngine::Tick, sub_82777738): the world tick (UWorld::Tick
// sub_827BEF78: actors, physics, animation, cameras) and, as its last step,
// the viewport draw (sub_82717918 on [GameViewport +68], GameViewport =
// [GEngine +772]: views, audio listener, HUD, the scene to the rendering
// thread).
//
// With ao2_decouple the simulation steps at a fixed ao2_sim_hz, by exactly
// 1 / ao2_sim_hz seconds (frame_time.cpp), and the frames in between (the
// frame limiter still paces all of them at ao2_fps_target) are render-only:
// no frame sync tick, no world tick, only the viewport draw of the last
// simulated state. Nothing is interpolated yet, so a render-only frame shows
// the same picture again.
//
// Online co-op: both machines must step the same way, so they share one
// ao2_decouple / ao2_sim_hz - the lobby server's side's, else the one the
// link picks (coop_link.cpp), fixed while the lockstep runs. When a step has
// to wait for the partner's input, the frame sync tick loops (pump the
// network, appSleep(0) sub_82A95510, update the connections, check) with the
// whole game thread; instead of that sleep the last state is drawn again at
// the frame rate, so the screen keeps going. (Checking for the partner's
// input before the tick cannot be done with the game's functions: the
// received records reach the players' lists only in the connection update
// sub_82A37B48, which also rebuilds this machine's outgoing record with the
// frame's checksum - zero between ticks.)
//
// The draw runs UnrealScript (HUD, the camera's view point), and its Rand()
// / FRand() take numbers from the rand() stream that the lockstep checksums
// (sub_823EAF28). That is the shared state [0x8317BA6C] only while
// [0x831B7558] is set and on the thread in [0x8317BBCC]; otherwise - also
// on the game thread in a co-op game, as seen in sync_trace.log - each
// thread's own state at [[r13] + 4]. Both are put back after every extra
// draw (only the shared one was: the per-thread stream moved by a different
// number of draws on each machine and the game desynced 25-40 s in).
//
// --ao2_decouple_debug=true logs frame counts and times once a second to
// decouple.log.
#include "decouple.h"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "coop.h"
#include "coop_link.h"
#include "frame_spikes.h"
#include "log_files.h"

REXCVAR_DECLARE(bool, ao2_fps_unlock);
REXCVAR_DECLARE(bool, ao2_coop_server);
int32_t FpsTarget(); // midasm_hooks.cpp

REXCVAR_DEFINE_BOOL(ao2_decouple, false, "Gameplay",
                    "The simulation steps at ao2_sim_hz, frames in between only draw (no interpolation yet). "
                    "Online co-op: the lobby server's setting is used by both players");
REXCVAR_DEFINE_INT32(ao2_sim_hz, 60, "Gameplay", "Simulation steps per second with ao2_decouple")
    .range(20, 240);
REXCVAR_DEFINE_BOOL(ao2_decouple_debug, false, "Debug", "Log ao2_decouple frame statistics to decouple.log");
REXCVAR_DEFINE_BOOL(ao2_coop_step_slip, true, "Online",
                    "Online co-op: a simulation step that waited for the partner moves the next steps by that "
                    "wait, so the two machines settle in step instead of one waiting every step");

namespace
{
  constexpr uint32_t kEngine = 0x831EFCBC;       // GEngine
  constexpr uint32_t kWorld = 0x831F2CC8;        // GWorld
  constexpr uint32_t kRandState = 0x8317BA6C;    // shared rand() state
  constexpr uint32_t kViewportDraw = 0x82717918; // (FViewport*) views, listener, draw
  constexpr uint32_t kEngineTick = 0x82777738;   // UGameEngine::Tick, GEngine vtable +264
  constexpr uint32_t kRenderFence = 0x831E5C10;  // the game thread's FRenderCommandFence
  constexpr uint32_t kFenceBegin = 0x825F4DD0;   // (fence)
  constexpr uint32_t kFenceWait = 0x825FC128;    // (fence, frames in flight)
  constexpr uint32_t kGameViewport = 772;        // UGameEngine::GameViewport
  constexpr uint32_t kViewport = 68;             // UGameViewportClient::Viewport

  // A simulation step is due this close to its time (the frame limiter
  // wakes a little late or early).
  constexpr double kDueToleranceMs = 1.5;
  // Further behind than this many steps: restart the clock (load, hitch,
  // a long wait for the co-op partner) instead of catching up.
  constexpr double kMaxBehindSteps = 4.0;

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }
  void StoreU32(uint8_t *base, uint32_t addr, uint32_t value)
  {
    value = std::byteswap(value);
    std::memcpy(base + addr, &value, 4);
  }

  double NowMs()
  {
    static const double ms_per_tick = [] {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return 1000.0 / double(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * ms_per_tick;
  }

  void Log(const char *fmt, ...)
  {
    static FILE *f = ao2::OpenLogFile("decouple.log");
    if (!f)
    {
      return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(f, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fputc('\n', f);
    std::fflush(f);
  }

  // The viewport a render-only frame draws, or 0 when there is none.
  uint32_t DrawViewport(const uint8_t *base)
  {
    uint32_t engine = LoadU32(base, kEngine);
    uint32_t client = engine ? LoadU32(base, engine + kGameViewport) : 0;
    return client ? LoadU32(base, client + kViewport) : 0;
  }

  // Calls a guest function with r3, r4; returns r3.
  uint32_t Call(PPCContext &ctx, uint8_t *base, uint32_t fn, uint32_t r3, uint32_t r4 = 0)
  {
    PPCContext saved = ctx;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
    uint32_t result = ctx.r3.u32;
    ctx = saved;
    return result;
  }

  using Settings = ao2::coop::link::SimSettings;

  // This machine's own settings.
  Settings Own()
  {
    Settings s;
    s.decouple = REXCVAR_GET(ao2_decouple) && REXCVAR_GET(ao2_fps_unlock);
    s.sim_hz = std::clamp(REXCVAR_GET(ao2_sim_hz), 20, 240);
    s.server = REXCVAR_GET(ao2_coop_server);
    return s;
  }

  // Game-thread state (the frame sync tick and GEngine->Tick run there).
  Settings g_settings;        // in effect
  bool g_render_only = false; // this frame: GEngine->Tick only draws
  // appSleep runs on other threads too (the rendering thread's D3D waits).
  thread_local bool t_in_sync_tick = false;
  double g_next_sim_ms = 0;  // when the next simulation step is due; 0 = off
  double g_last_draw_ms = 0; // start of the last frame or extra draw
  uint32_t g_sim_steps = 0;  // since decoupling went on
  uint32_t g_world_steps = 0;   // world ticks run (GEngine->Tick of a simulation step)
  double g_world_step_ms = 0;   // start of the frame of the last world tick
  bool g_in_extra_draw = false; // inside DrawOnly (the rand() state is put back after it)

  // The settings in effect: in a network session the shared ones (fixed
  // while the lockstep runs: changing the step there would desync).
  Settings Update(const uint8_t *base)
  {
    Settings own = Own();
    ao2::coop::link::SetLocalSettings(own);
    Settings next = own;
    if (ao2::coop::InNetworkGame(base))
    {
      if (ao2::coop::OnlineLockstep(base))
      {
        return g_settings;
      }
      Settings partner;
      if (ao2::coop::link::PartnerSettings(&partner) && !ao2::coop::link::SettingsAreMine(own, partner))
      {
        next.decouple = partner.decouple;
        next.sim_hz = std::clamp(partner.sim_hz, 20, 240);
      }
    }
    if (next.decouple != g_settings.decouple || next.sim_hz != g_settings.sim_hz)
    {
      Log("settings: decouple %d, sim %d Hz (own: %d, %d Hz%s)", next.decouple, next.sim_hz, own.decouple,
          own.sim_hz, ao2::coop::InNetworkGame(base) ? ", network game" : "");
      if (next.decouple && !REXCVAR_GET(ao2_fps_unlock))
      {
        Log("  warning: ao2_fps_unlock is off here - at the game's 30 FPS the simulation cannot keep up");
      }
    }
    g_settings = next;
    return next;
  }

  struct Stats
  {
    double window_start = 0, last_frame = 0, last_sim = 0;
    uint32_t frames = 0, sims = 0, gaps = 0, draws = 0, wait_draws = 0, no_viewport = 0, rand_restores = 0, slips = 0;
    double frame_max = 0, sim_gap_sum = 0, sim_gap_max = 0;
    double tick_sum = 0, tick_max = 0, draw_sum = 0, draw_max = 0;

    void Frame(double now, bool sim)
    {
      if (window_start == 0)
      {
        window_start = now;
      }
      if (last_frame > 0)
      {
        frame_max = std::max(frame_max, now - last_frame);
      }
      last_frame = now;
      frames++;
      if (sim)
      {
        if (last_sim > 0)
        {
          sim_gap_sum += now - last_sim;
          sim_gap_max = std::max(sim_gap_max, now - last_sim);
          gaps++;
        }
        last_sim = now;
        sims++;
      }
      double elapsed = now - window_start;
      if (elapsed < 1000.0 || !REXCVAR_GET(ao2_decouple_debug))
      {
        return;
      }
      Log("fps=%.1f (frame max %.1f ms) | sim %.1f/s, gap avg %.2f max %.2f ms, engine tick avg %.2f max %.2f "
          "ms | draw-only %u, avg %.2f max %.2f ms, no viewport %u | drawn while waiting %u | rand restored %u | "
          "steps moved after a wait %u",
          (frames + wait_draws) * 1000.0 / elapsed, frame_max, sims * 1000.0 / elapsed,
          gaps ? sim_gap_sum / gaps : 0.0, sim_gap_max, sims ? tick_sum / sims : 0.0, tick_max, draws,
          draws ? draw_sum / draws : 0.0, draw_max, no_viewport, wait_draws, rand_restores, slips);
      double keep_sim = last_sim;
      *this = Stats{};
      window_start = last_frame = now;
      last_sim = keep_sim;
    }
  } g_stats;

  // The viewport draw of the last simulated state, the shared rand() state
  // kept.
  void DrawOnly(PPCContext &ctx, uint8_t *base)
  {
    double start = NowMs();
    uint32_t rand_state = LoadU32(base, kRandState);
    uint32_t tls = ctx.r13.u32 ? LoadU32(base, ctx.r13.u32) : 0;
    uint32_t thread_rand_state = tls ? LoadU32(base, tls + 4) : 0;
    g_in_extra_draw = true;
    if (uint32_t viewport = DrawViewport(base))
    {
      Call(ctx, base, kViewportDraw, viewport);
    }
    else
    {
      g_stats.no_viewport++;
    }
    g_in_extra_draw = false;
    if (LoadU32(base, kRandState) != rand_state)
    {
      StoreU32(base, kRandState, rand_state);
      g_stats.rand_restores++;
    }
    if (tls && LoadU32(base, tls + 4) != thread_rand_state)
    {
      StoreU32(base, tls + 4, thread_rand_state);
      g_stats.rand_restores++;
    }
    double ms = NowMs() - start;
    g_stats.draws++;
    g_stats.draw_sum += ms;
    g_stats.draw_max = std::max(g_stats.draw_max, ms);
  }
} // namespace

namespace ao2::decouple
{
  double SimPeriodMs()
  {
    return g_settings.decouple ? 1000.0 / g_settings.sim_hz : 0.0;
  }

  void SetInSyncTick(bool inside)
  {
    t_in_sync_tick = inside;
  }

  bool Active()
  {
    return g_settings.decouple && g_sim_steps > 0;
  }

  // The steps are due on a fixed schedule (BeginFrame). Online, a machine
  // whose schedule runs ahead of the partner's by more than the lockstep's
  // lead (coop_input_delay.cpp) waits in the sync tick for the partner's
  // frame - and, the schedule staying where it was, in the next step again by
  // as much: one game waited ~10 ms in 30 of its 60 steps a second, on a LAN.
  // A step that waited puts the next one a period (plus a little slack for
  // jitter) after the wait ended: the schedules settle one lead apart.
  void AfterSyncTick(uint8_t *base, double tick_ms)
  {
    constexpr double kWaitedMs = 1.0;
    constexpr double kSlackMs = 1.0;
    if (!REXCVAR_GET(ao2_coop_step_slip) || !g_settings.decouple || g_next_sim_ms == 0 || tick_ms < kWaitedMs ||
        !ao2::coop::OnlineLockstep(base))
    {
      return;
    }
    double next = NowMs() + 1000.0 / g_settings.sim_hz + kSlackMs;
    if (next > g_next_sim_ms)
    {
      g_next_sim_ms = next;
      g_stats.slips++;
    }
  }

  uint32_t WorldStep()
  {
    return g_world_steps;
  }

  bool InExtraDraw()
  {
    return g_in_extra_draw;
  }

  double Alpha()
  {
    double period = 1000.0 / g_settings.sim_hz;
    return std::clamp((g_last_draw_ms - g_world_step_ms) / period, 0.0, 1.0);
  }

  bool BeginFrame(PPCContext &ctx, uint8_t *base, double *sim_dt)
  {
    (void)ctx;
    *sim_dt = 0;
    g_render_only = false;
    Settings settings = Update(base);
    if (!settings.decouple)
    {
      g_next_sim_ms = 0;
      g_sim_steps = 0;
      return false;
    }
    double now = NowMs();
    g_last_draw_ms = now;
    double period = 1000.0 / settings.sim_hz;
    // Draw-only needs something drawn by a simulation step before it.
    bool can_draw = g_sim_steps > 0 && LoadU32(base, kWorld) && DrawViewport(base);
    if (g_next_sim_ms == 0 || !can_draw || now >= g_next_sim_ms - kDueToleranceMs)
    {
      if (g_next_sim_ms == 0)
      {
        uint32_t engine = LoadU32(base, kEngine);
        uint32_t tick = engine ? LoadU32(base, LoadU32(base, engine) + 264) : 0;
        Log("decoupling on: sim %d Hz, GEngine->Tick = %08X (expected %08X)", settings.sim_hz, tick, kEngineTick);
      }
      if (g_next_sim_ms == 0 || now - g_next_sim_ms > kMaxBehindSteps * period)
      {
        g_next_sim_ms = now;
      }
      g_next_sim_ms += period;
      g_sim_steps++;
      *sim_dt = 1.0 / settings.sim_hz;
      g_stats.Frame(now, true);
      return false;
    }
    g_render_only = true;
    g_stats.Frame(now, false);
    return true;
  }
} // namespace ao2::decouple

// UGameEngine::Tick(this, DeltaSeconds): on a render-only frame only the
// viewport draw (the call at its end).
extern "C" REX_FUNC(__imp__sub_82777738);
extern "C" REX_FUNC(sub_82777738)
{
  double spike_start = ao2::spikes::Enabled() ? NowMs() : 0.0;
  struct SpikeMark
  {
    double start;
    ~SpikeMark()
    {
      if (start > 0.0)
      {
        ao2::spikes::Mark(ao2::spikes::kEngineTick, NowMs() - start);
      }
    }
  } spike_mark{spike_start};
  if (!g_render_only)
  {
    double start = NowMs();
    g_world_steps++;
    g_world_step_ms = g_last_draw_ms;
    __imp__sub_82777738(ctx, base);
    double ms = NowMs() - start;
    g_stats.tick_sum += ms;
    g_stats.tick_max = std::max(g_stats.tick_max, ms);
    return;
  }
  g_render_only = false;
  DrawOnly(ctx, base);
}

// appSleep(seconds f1). Inside the frame sync tick on the game thread it is
// the wait for the co-op partner's input (f1 = 0): draw the last state again once a frame
// time has passed, as the game thread's frame does (the render command
// fence keeps at most one frame in flight), instead of only yielding.
namespace ao2
{
  bool LimiterSleep(PPCContext &ctx, uint8_t *base); // limiter_wait.cpp
}

extern "C" REX_FUNC(__imp__sub_82A95510);
extern "C" REX_FUNC(sub_82A95510)
{
  if (ao2::LimiterSleep(ctx, base))
  {
    return;
  }
  if (t_in_sync_tick && ctx.f1.f64 == 0.0 && g_settings.decouple && g_sim_steps > 0 &&
      ao2::coop::OnlineLockstep(base))
  {
    double now = NowMs();
    if (now - g_last_draw_ms >= 1000.0 / std::max(FpsTarget(), 20) - 0.25)
    {
      g_last_draw_ms = now;
      DrawOnly(ctx, base);
      Call(ctx, base, kFenceBegin, kRenderFence);
      Call(ctx, base, kFenceWait, kRenderFence, 1);
      g_stats.wait_draws++;
      return;
    }
  }
  __imp__sub_82A95510(ctx, base);
}
