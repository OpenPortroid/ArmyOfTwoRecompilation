// tick_profile.cpp - Where a long engine tick went (--ao2_tick_profile_ms=N).
//
// A simulation step's GEngine->Tick (UGameEngine::Tick sub_82777738) takes
// 4-7 ms at 120 FPS, now and then 8-14 ms, and that frame comes late
// (frame_spikes.log). This times the functions GEngine->Tick and the world
// tick (UWorld::Tick sub_827BEF78) call directly, on the game thread inside
// a tick only, and logs those that took 0.2 ms or more in every tick of N ms
// or longer to tick_profile.log, with their usual time (average over the
// ticks) next to it. E = called by GEngine->Tick, W = by UWorld::Tick; the
// rest of a tick is in the callers themselves or behind indirect calls
// (actors' Tick, script). Off by default (a thread_local test per call).
#include "tick_profile.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <intrin.h>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "log_files.h"

REXCVAR_DEFINE_INT32(ao2_tick_profile_ms, 0, "Debug",
                     "Log where every engine tick of this many ms or longer went (tick_profile.log, 0 = off)")
    .range(0, 1000);

namespace
{
  struct Slot
  {
    const char *name;
    const char *callers;
    double ms = 0.0;
    uint32_t calls = 0;
    double average = 0.0; // over the ticks
  };

  constexpr int kSlots = 66;
  Slot g_slots[kSlots];
  thread_local bool t_on = false;
  uint64_t g_ticks = 0;
  uint64_t g_cycles_start = 0; // the game thread's CPU cycles at the tick's start

  uint64_t ThreadCycles()
  {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
  }

  // CPU cycles per ms (the thread's cycle count runs at the TSC rate).
  double CyclesPerMs()
  {
    static const double rate = [] {
      LARGE_INTEGER f, a, b;
      QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&a);
      uint64_t c0 = __rdtsc();
      do
      {
        QueryPerformanceCounter(&b);
      } while (b.QuadPart - a.QuadPart < f.QuadPart / 50);
      return double(__rdtsc() - c0) / (double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart));
    }();
    return rate;
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

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("tick_profile.log");
    return f;
  }

  struct Timer
  {
    int slot;
    double start;
    Timer(int s, const char *name, const char *callers) : slot(s), start(NowMs())
    {
      g_slots[s].name = name;
      g_slots[s].callers = callers;
    }
    ~Timer()
    {
      g_slots[slot].ms += NowMs() - start;
      g_slots[slot].calls++;
    }
  };
} // namespace

namespace ao2::tickprof
{
  void Begin()
  {
    t_on = REXCVAR_GET(ao2_tick_profile_ms) > 0;
    if (t_on)
    {
      g_cycles_start = ThreadCycles();
    }
  }

  void End(double tick_ms)
  {
    if (!t_on)
    {
      return;
    }
    t_on = false;
    double cpu_ms = double(ThreadCycles() - g_cycles_start) / CyclesPerMs();
    ++g_ticks;
    double weight = 1.0 / double(std::min<uint64_t>(g_ticks, 600));
    bool log = tick_ms >= double(REXCVAR_GET(ao2_tick_profile_ms)) && g_ticks > 60 && Log();
    if (log)
    {
      SYSTEMTIME t;
      GetLocalTime(&t);
      std::fprintf(Log(), "[%02u:%02u:%02u.%03u] tick %.2f ms, the game thread ran %.2f ms of it\n", t.wHour,
                   t.wMinute, t.wSecond, t.wMilliseconds, tick_ms, cpu_ms);
      int order[kSlots];
      for (int i = 0; i < kSlots; ++i)
      {
        order[i] = i;
      }
      std::sort(order, order + kSlots, [](int a, int b) { return g_slots[a].ms > g_slots[b].ms; });
      for (int i : order)
      {
        const Slot &s = g_slots[i];
        if (s.ms < 0.2)
        {
          break;
        }
        std::fprintf(Log(), "  %-12s %-2s %6.2f ms (usually %5.2f)  %u calls\n", s.name, s.callers, s.ms,
                     s.average, s.calls);
      }
      std::fflush(Log());
    }
    for (Slot &s : g_slots)
    {
      s.average += (s.ms - s.average) * weight;
      s.ms = 0.0;
      s.calls = 0;
    }
  }
} // namespace ao2::tickprof

#define AO2_TIMED(index, addr, callers)                                                                        \
  extern "C" REX_FUNC(__imp__sub_##addr);                                                                       \
  extern "C" REX_FUNC(sub_##addr)                                                                               \
  {                                                                                                             \
    if (!t_on)                                                                                                  \
    {                                                                                                           \
      __imp__sub_##addr(ctx, base);                                                                             \
      return;                                                                                                   \
    }                                                                                                           \
    Timer timer(index, "sub_" #addr, callers);                                                                  \
    __imp__sub_##addr(ctx, base);                                                                               \
  }

AO2_TIMED(0, 822E0818, "E")
AO2_TIMED(1, 823099B0, "EW")
AO2_TIMED(2, 82309A60, "E")
AO2_TIMED(3, 82309BC8, "E")
AO2_TIMED(4, 82388AC8, "E")
AO2_TIMED(5, 823E07E8, "E")
AO2_TIMED(6, 82413250, "E")
AO2_TIMED(7, 82451A50, "E")
AO2_TIMED(8, 825FED60, "E")
AO2_TIMED(9, 826F5B70, "E")
AO2_TIMED(10, 82717918, "E")
AO2_TIMED(11, 82752768, "E")
AO2_TIMED(12, 8276EAF0, "E")
AO2_TIMED(13, 82783558, "E")
AO2_TIMED(14, 82783680, "E")
AO2_TIMED(15, 8279F1E0, "EW")
AO2_TIMED(16, 827BD058, "E")
AO2_TIMED(17, 827BEF78, "E")
AO2_TIMED(18, 827F88D0, "E")
AO2_TIMED(19, 8281B318, "E")
AO2_TIMED(20, 8296DEC0, "E")
AO2_TIMED(21, 8299D5C8, "E")
AO2_TIMED(22, 8299E208, "E")
AO2_TIMED(23, 829B4EE8, "E")
AO2_TIMED(24, 829B5670, "E")
AO2_TIMED(25, 829B5980, "E")
AO2_TIMED(26, 82A186E0, "E")
AO2_TIMED(27, 82A30178, "E")
AO2_TIMED(28, 82A30D08, "E")
AO2_TIMED(29, 822E1BD0, "W")
AO2_TIMED(30, 82302D08, "W")
AO2_TIMED(31, 82312730, "W")
AO2_TIMED(32, 8238A5B0, "W")
AO2_TIMED(33, 82416538, "W")
AO2_TIMED(34, 82416660, "W")
AO2_TIMED(35, 82451800, "W")
AO2_TIMED(36, 824598F8, "W")
AO2_TIMED(37, 82463748, "W")
AO2_TIMED(38, 8246BD50, "W")
AO2_TIMED(39, 82545CC0, "W")
AO2_TIMED(40, 825752C0, "W")
AO2_TIMED(41, 825FF2F8, "W")
AO2_TIMED(42, 826787B8, "W")
AO2_TIMED(43, 826A4A10, "W")
AO2_TIMED(44, 826AEF28, "W")
AO2_TIMED(45, 827938A0, "W")
AO2_TIMED(46, 8279F048, "W")
AO2_TIMED(47, 8279F2B8, "W")
AO2_TIMED(48, 827A6630, "W")
AO2_TIMED(49, 827A6888, "W")
AO2_TIMED(50, 827B2148, "W")
AO2_TIMED(51, 827CD4C0, "W")
AO2_TIMED(52, 827D06A0, "W")
AO2_TIMED(53, 827D12F0, "W")
AO2_TIMED(54, 827D13D0, "W")
AO2_TIMED(55, 827D6F68, "W")
AO2_TIMED(56, 827D71A0, "W")
AO2_TIMED(57, 827D73E0, "W")
AO2_TIMED(58, 827DAEE0, "W")
AO2_TIMED(59, 827DB938, "W")
AO2_TIMED(60, 82854DE0, "W")
AO2_TIMED(61, 8285FD58, "W")
AO2_TIMED(62, 8286A3D0, "W")
AO2_TIMED(63, 82877018, "W")
AO2_TIMED(64, 82877138, "W")
AO2_TIMED(65, 829857B8, "W")
