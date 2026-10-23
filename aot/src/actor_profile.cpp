// actor_profile.cpp - Which actors' ticks the game thread spends its time in (--ao2_actor_profile=true).
//
// The tick of a level with few enemies can still cost 5+ ms (China's river, 07_011: 5-7 ms against Iraq's
// 3-4). A sample profile shows the time under two actor tick functions, sub_8279CEE0 and sub_827B1AA8
// (r3 = the actor: script Tick, physics, movement with collision checks), reached both directly and
// through the actor's vtable, and under the components' tick sub_8283BA58 (r3 = the component) - it
// cannot say which ones they were. This times every call of them on the game thread by the class of r3,
// exclusive of the nested ones (an actor ticking its components or an attached actor), and writes the
// classes that cost the most to actor_profile.log every 5 s: ms per second (= % of the thread) and calls
// per second. Off by default (one test per call).
#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "log_files.h"
#include "ue3_reflect.h"

REXCVAR_DEFINE_BOOL(ao2_actor_profile, false, "Debug",
                    "Log the actor classes whose ticks take the most game-thread time to actor_profile.log (every 5 s)");

extern "C" REX_FUNC(__imp__sub_8279CEE0);
extern "C" REX_FUNC(__imp__sub_827B1AA8);
extern "C" REX_FUNC(__imp__sub_8283BA58);

namespace
{
  struct Cost
  {
    double ms = 0.0;
    uint32_t calls = 0;
  };

  std::unordered_map<uint32_t, Cost> g_by_class; // UClass address -> cost
  std::unordered_map<uint32_t, std::string> g_names;
  double g_child_ms = 0.0; // time of nested actor ticks inside the running one
  int g_depth = 0;
  double g_window_start = 0.0;
  DWORD g_thread = 0;

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

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("actor_profile.log");
    return f;
  }

  void Report(const uint8_t *base, double now)
  {
    double seconds = (now - g_window_start) / 1000.0;
    g_window_start = now;
    if (!Log() || g_by_class.empty())
    {
      g_by_class.clear();
      return;
    }
    std::vector<std::pair<uint32_t, Cost>> rows(g_by_class.begin(), g_by_class.end());
    g_by_class.clear();
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second.ms > b.second.ms; });
    double total = 0.0;
    for (const auto &r : rows)
    {
      total += r.second.ms;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(Log(), "[%02u:%02u:%02u] actor ticks %.1f ms/s over %.1f s\n", t.wHour, t.wMinute, t.wSecond,
                 total / seconds, seconds);
    for (size_t i = 0; i < rows.size() && i < 15; ++i)
    {
      auto it = g_names.find(rows[i].first);
      if (it == g_names.end())
      {
        it = g_names.emplace(rows[i].first, ao2::ue3::ObjectName(base, rows[i].first)).first;
      }
      std::fprintf(Log(), "  %6.2f ms/s %6.0f calls/s  %s\n", rows[i].second.ms / seconds,
                   rows[i].second.calls / seconds, it->second.c_str());
    }
    std::fflush(Log());
  }

  template <void (*Original)(PPCContext &__restrict, uint8_t *)>
  void Timed(PPCContext &ctx, uint8_t *base)
  {
    if (!REXCVAR_GET(ao2_actor_profile))
    {
      Original(ctx, base);
      return;
    }
    DWORD thread = GetCurrentThreadId();
    if (!g_thread)
    {
      g_thread = thread; // the first caller: the game thread (actors tick only there)
    }
    if (thread != g_thread)
    {
      Original(ctx, base);
      return;
    }
    uint32_t cls = LoadU32(base, ctx.r3.u32 + 52);
    double outer_child = g_child_ms;
    g_child_ms = 0.0;
    ++g_depth;
    double start = NowMs();
    Original(ctx, base);
    double now = NowMs();
    --g_depth;
    double took = now - start;
    Cost &c = g_by_class[cls];
    c.ms += took - g_child_ms;
    c.calls++;
    g_child_ms = outer_child + took;
    if (g_depth == 0)
    {
      g_child_ms = 0.0;
      if (g_window_start == 0.0)
      {
        g_window_start = now;
      }
      else if (now - g_window_start >= 5000.0)
      {
        Report(base, now);
      }
    }
  }
} // namespace

extern "C" REX_FUNC(sub_8279CEE0)
{
  Timed<__imp__sub_8279CEE0>(ctx, base);
}

extern "C" REX_FUNC(sub_827B1AA8)
{
  Timed<__imp__sub_827B1AA8>(ctx, base);
}

// A component's tick (r3 = the component), from the tick groups (sub_827A6410).
extern "C" REX_FUNC(sub_8283BA58)
{
  Timed<__imp__sub_8283BA58>(ctx, base);
}
