// frame_spikes.cpp - What the threads did before a long frame
// (--ao2_spike_log=true).
//
// The overlay's frame time graph shows single frames a few milliseconds
// longer than the rest. Whose they are - the game thread's simulation step,
// its wait for the rendering thread, the GPU thread's wait for the GPU, the
// present - is told by the times the threads mark here: when a present comes
// later than 1.4 frame times after the previous one, the last 60 ms of marks
// go to frame_spikes.log, timed back from that present. Off by default (a
// flag test per mark).
#include "frame_spikes.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <mutex>

#include <rex/cvar.h>

#include "log_files.h"

REXCVAR_DEFINE_BOOL(ao2_spike_log, false, "Debug",
                    "Log what the game, GPU and present threads did before each long frame (frame_spikes.log)");

namespace
{
  constexpr size_t kRing = 4096;
  constexpr double kWindowMs = 60.0;

  struct Entry
  {
    double ms;
    float arg;
    ao2::spikes::Event event;
  };

  std::mutex g_mutex;
  Entry g_ring[kRing];
  uint64_t g_count = 0;
  double g_last_dump = 0.0;
  uint32_t g_spikes = 0, g_presents = 0;

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
    static FILE *f = ao2::OpenLogFile("frame_spikes.log");
    return f;
  }

  const char *const kNames[ao2::spikes::kEventCount] = {
      "game  frame",      "game  engine tick", "game  limiter sleep", "game  render fence", "gpu   idle",
      "gpu   first draw",  "gpu   fence wait",
      "gpu   swap",       "gpu   submitted",   "ovl   pdh",          "ovl   gpu temp",     "ovl   dxgi vram",
      "pres  slot",         "pres  margin",       "pres  paced",         "pres  presented",
  };
} // namespace

namespace ao2::spikes
{
  bool Enabled()
  {
    return REXCVAR_GET(ao2_spike_log);
  }

  void Mark(Event event, double arg)
  {
    if (event == kLimiterSleep && arg < 0.05)
    {
      return; // the engine's loop calls it again and again at the end of the wait
    }
    double now = NowMs();
    std::lock_guard lock(g_mutex);
    g_ring[g_count % kRing] = {now, float(arg), event};
    ++g_count;
  }

  void AfterPresent(double interval_ms, double period_ms)
  {
    if (!Enabled() || !Log())
    {
      return;
    }
    double now = NowMs();
    static Entry copy[kRing]; // the present thread's only
    size_t n = 0;
    uint32_t spikes, presents;
    {
      std::lock_guard lock(g_mutex);
      ++g_presents;
      bool spike = period_ms > 0.0 && interval_ms > period_ms * 1.4 && interval_ms < 500.0;
      if (spike)
      {
        ++g_spikes;
      }
      spikes = g_spikes;
      presents = g_presents;
      if (!spike || now - g_last_dump < 100.0)
      {
        return;
      }
      g_last_dump = now;
      uint64_t first = g_count > kRing ? g_count - kRing : 0;
      for (uint64_t i = first; i < g_count; ++i)
      {
        const Entry &m = g_ring[i % kRing];
        if (m.ms >= now - kWindowMs)
        {
          copy[n++] = m;
        }
      }
    }
    FILE *f = Log();
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(f, "[%02u:%02u:%02u.%03u] spike: present interval %.2f ms (frame %.2f ms) | %u spikes in %u presents\n",
                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, interval_ms, period_ms, spikes, presents);
    for (size_t i = 0; i < n; ++i)
    {
      const Entry &m = copy[i];
      std::fprintf(f, "  %7.2f  %-20s", m.ms - now, kNames[m.event]);
      if (m.event == kGameFrame)
      {
        std::fprintf(f, " %s", m.arg != 0.0f ? "SIM" : "draw only");
      }
      else if (m.event != kGpuSwap && m.event != kGpuFirstDraw)
      {
        std::fprintf(f, " %.2f ms", m.arg);
      }
      std::fputc('\n', f);
    }
    std::fflush(f);
  }
} // namespace ao2::spikes
