// physx_worker.cpp - A physics scene kicked right after another one is
// simulated too (it used to be dropped, and the game hung waiting for it).
//
// The world's rigid body scenes (GWorld +0x168, +0x16C, +0x170: FRBPhysScene,
// index at +64 into the NxScene map 0x831848A4) are simulated on one worker
// thread, the runnable at [0x831F1DB4] (vtable 0x820C2AC4): +4 the NxScene,
// +8 the time step, +12 "start" (auto-reset FEvent), +16 "done" (manual-reset
// FEvent, set when idle). FEvent vtable: +8 Trigger, +12 Reset, +20 Wait.
//
// The kick, sub_8286A290 (scene, DeltaTime, MaxSubstep, MaxSubsteps, ...),
// sets the scene's timing, waits for "done", writes the scene and the step to
// the worker and triggers "start". The worker (Run, sub_82885B88) waits for
// "start", resets "done", simulates, triggers "done". Two kicks in a row (the
// world kicks two scenes in one tick): when the second comes before the
// worker woke up for the first, "done" is still set from before, the second
// kick passes its wait, overwrites the scene and triggers "start" again - the
// auto-reset event wakes the worker once, it simulates only the second scene.
// The first scene's fetchResults never succeeds, and the wait for all scenes
// (sub_82854DE0: fetchResults(NX_RIGID_BODY_FINISHED, no block) on scenes 0
// and 1, appSleep between tries) loops forever: the game froze, the main
// thread in that wait, the worker idle on "start", nothing else running. On
// the console the worker was awake long before the next kick; the recompiled
// game thread gets there first now and then.
//
// Now a kick first waits until the worker has taken the previous one (woken
// up, read its scene and step, reset "done"), so the kick's own wait for
// "done" waits for that simulation as it was meant to.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "log_files.h"
#include "tick_profile.h"

REXCVAR_DECLARE(int32_t, ao2_tick_profile_ms);

namespace
{
  // Kicks made (counted after the kick triggered "start") and taken by the
  // worker (counted once it has read the kick's scene and step).
  std::atomic<uint64_t> g_kicks{0}, g_taken{0};
  std::atomic<uint32_t> g_waited{0};

  uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    uint32_t v;
    std::memcpy(&v, base + address, 4);
    return std::byteswap(v);
  }

  // object->vtable[slot / 4](object, r4 / f1 as set by the caller).
  void CallVirtual(PPCContext &ctx, uint8_t *base, uint32_t object, uint32_t slot)
  {
    ctx.r3.u64 = object;
    rex::runtime::ResolveIndirectFunction(Load32(base, Load32(base, object) + slot))(ctx, base);
  }

  constexpr uint32_t kTrigger = 8, kReset = 12, kWait = 20, kSimulate = 380;

  // With ao2_tick_profile_ms: how long the worker simulates, per scene, to physx_worker.log every 10 s.
  struct SimStats
  {
    uint32_t scene = 0, count = 0;
    double sum = 0.0, max = 0.0;
  };
  SimStats g_sim[4];
  std::chrono::steady_clock::time_point g_sim_window;

  void NoteSimulation(uint32_t scene, std::chrono::steady_clock::time_point start)
  {
    if (REXCVAR_GET(ao2_tick_profile_ms) <= 0)
    {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(now - start).count();
    SimStats *slot = nullptr;
    for (SimStats &s : g_sim)
    {
      if (s.scene == scene || s.scene == 0)
      {
        slot = &s;
        break;
      }
    }
    if (slot)
    {
      slot->scene = scene;
      slot->count++;
      slot->sum += ms;
      slot->max = std::max(slot->max, ms);
    }
    if (g_sim_window.time_since_epoch().count() == 0)
    {
      g_sim_window = now;
    }
    double window = std::chrono::duration<double>(now - g_sim_window).count();
    if (window < 10.0)
    {
      return;
    }
    if (FILE *log = ao2::OpenLogFile("physx_worker.log", "a"))
    {
      std::fprintf(log, "simulate, last %.1f s:", window);
      for (const SimStats &s : g_sim)
      {
        if (s.count)
        {
          std::fprintf(log, " | scene %08X: %u/s, avg %.2f ms, max %.2f ms, busy %.1f%%", s.scene,
                       unsigned(s.count / window), s.sum / s.count, s.max, s.sum / (window * 10.0));
        }
      }
      std::fputc('\n', log);
      std::fclose(log);
    }
    for (SimStats &s : g_sim)
    {
      s = SimStats{};
    }
    g_sim_window = now;
  }
} // namespace

// The physics worker's Run: the game's loop, telling the kicks when a kick is taken.
extern "C" REX_FUNC(__imp__sub_82885B88);
extern "C" REX_FUNC(sub_82885B88)
{
  const uint32_t self = ctx.r3.u32;
  for (;;)
  {
    ctx.r4.s64 = -1; // INFINITE
    CallVirtual(ctx, base, Load32(base, self + 12), kWait);
    CallVirtual(ctx, base, Load32(base, self + 16), kReset);
    const uint32_t scene = Load32(base, self + 4);
    uint32_t step_bits = Load32(base, self + 8);
    float step;
    std::memcpy(&step, &step_bits, 4);
    g_taken.fetch_add(1, std::memory_order_release);
    ctx.f1.f64 = step;
    auto sim_start = std::chrono::steady_clock::now();
    CallVirtual(ctx, base, scene, kSimulate);
    NoteSimulation(scene, sim_start);
    CallVirtual(ctx, base, Load32(base, self + 16), kTrigger);
  }
}

// The kick: not before the worker has taken the previous one.
extern "C" REX_FUNC(__imp__sub_8286A290);
extern "C" REX_FUNC(sub_8286A290)
{
  auto kick_start = std::chrono::steady_clock::now();
  if (g_taken.load(std::memory_order_acquire) < g_kicks.load(std::memory_order_relaxed))
  {
    auto start = std::chrono::steady_clock::now();
    while (g_taken.load(std::memory_order_acquire) < g_kicks.load(std::memory_order_relaxed))
    {
      // Never forever: a worker that is gone would hang the kick's own wait anyway.
      if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2))
      {
        break;
      }
      SwitchToThread();
    }
    uint32_t waited = g_waited.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((waited & (waited - 1)) == 0) // 1, 2, 4, 8, ...
    {
      if (FILE *log = ao2::OpenLogFile("physx_worker.log", "a"))
      {
        std::fprintf(log, "kick %llu waited for the worker to take the previous one (%u times so far)\n",
                     (unsigned long long)g_kicks.load(), waited);
        std::fclose(log);
      }
    }
  }
  if (ao2::tickprof::On())
  {
    auto game_start = std::chrono::steady_clock::now();
    __imp__sub_8286A290(ctx, base);
    auto end = std::chrono::steady_clock::now();
    using ms = std::chrono::duration<double, std::milli>;
    ao2::tickprof::Add(ao2::tickprof::kPhysicsKickGame, "kick: game", "W", ms(end - game_start).count());
    ao2::tickprof::Add(ao2::tickprof::kPhysicsKick, "sub_8286A290", "W", ms(end - kick_start).count());
  }
  else
  {
    __imp__sub_8286A290(ctx, base);
  }
  g_kicks.fetch_add(1, std::memory_order_release);
}
