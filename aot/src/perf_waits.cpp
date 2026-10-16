// perf_waits.cpp - Where the game's threads wait (--ao2_perf_waits=true).
//
// Above ~120 FPS no thread is busy all the time: the frame rate is set by
// how the game thread, the rendering thread and the GPU command thread wait
// for each other. This times the guest's kernel wait wrappers and the waits
// the port already hooks, per thread, and writes "waits ms/s" lines to
// native_gpu.log every 2 s. Off by default (a flag test per wait).
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "gpu/native/renderer.h"
#include "perf_waits.h"

REXCVAR_DEFINE_BOOL(ao2_perf_waits, false, "Debug",
                    "Log the time each game thread spends in guest waits (native_gpu.log, every 2 s)");

namespace
{
  enum Kind : uint32_t
  {
    kKeWait,
    kKeWaitMultiple,
    kNtWait,
    kNtWaitMultiple,
    kSleep,
    kD3DWait,
    kOcclusion,
    kRenderFence,
    kKindCount
  };
  const char *const kKindNames[kKindCount] = {"KeWait", "KeWaitMulti", "NtWait", "NtWaitMulti", "Sleep",
                                              "D3D",    "Occlusion",   "RenderFence"};

  struct ThreadWaits
  {
    std::string name;
    std::atomic<uint64_t> ns[kKindCount] = {};
    std::atomic<uint64_t> count[kKindCount] = {};
  };

  std::mutex g_mutex;
  std::vector<ThreadWaits *> g_threads;
  std::chrono::steady_clock::time_point g_last = std::chrono::steady_clock::now();

  ThreadWaits &Current()
  {
    thread_local ThreadWaits *t = [] {
      auto *w = new ThreadWaits;
      PWSTR desc = nullptr;
      if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)) && desc)
      {
        char name[64];
        WideCharToMultiByte(CP_UTF8, 0, desc, -1, name, sizeof(name), nullptr, nullptr);
        w->name = name;
        LocalFree(desc);
      }
      if (w->name.empty())
      {
        w->name = "thread " + std::to_string(GetCurrentThreadId());
      }
      std::lock_guard lock(g_mutex);
      g_threads.push_back(w);
      return w;
    }();
    return *t;
  }

  void Report()
  {
    std::lock_guard lock(g_mutex);
    auto now = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(now - g_last).count();
    if (seconds < 2.0)
    {
      return;
    }
    g_last = now;
    for (ThreadWaits *t : g_threads)
    {
      char line[512];
      int n = std::snprintf(line, sizeof(line), "  waits ms/s %s:", t->name.c_str());
      bool any = false;
      for (uint32_t k = 0; k < kKindCount; ++k)
      {
        uint64_t ns = t->ns[k].exchange(0), count = t->count[k].exchange(0);
        if (count)
        {
          any = true;
          n += std::snprintf(line + n, sizeof(line) - n, " %s %.1f (%.0f/s)", kKindNames[k], ns / 1e6 / seconds,
                             count / seconds);
        }
      }
      if (any)
      {
        ao2::gpu::NativeLog("%s\n", line);
      }
    }
  }

  void Add(uint32_t kind, std::chrono::steady_clock::time_point start)
  {
    ThreadWaits &t = Current();
    t.ns[kind] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    ++t.count[kind];
    Report();
  }

  template <typename F>
  void Timed(Kind kind, F &&call)
  {
    if (!REXCVAR_GET(ao2_perf_waits))
    {
      call();
      return;
    }
    auto start = std::chrono::steady_clock::now();
    call();
    Add(kind, start);
  }
} // namespace

namespace ao2::perf
{
  bool WaitsEnabled() { return REXCVAR_GET(ao2_perf_waits); }
  void AddOcclusionWait(std::chrono::steady_clock::time_point start) { Add(kOcclusion, start); }
  void AddRenderFenceWait(std::chrono::steady_clock::time_point start) { Add(kRenderFence, start); }
} // namespace ao2::perf

#define WAIT_HOOK(addr, kind)                                    \
  extern "C" REX_FUNC(__imp__sub_##addr);                        \
  extern "C" REX_FUNC(sub_##addr)                                \
  {                                                              \
    Timed(kind, [&] { __imp__sub_##addr(ctx, base); });          \
  }

// XDK kernel wait wrappers (each calls one wait import).
WAIT_HOOK(82C1C498, kKeWait)
WAIT_HOOK(82C1C3D0, kKeWait)
WAIT_HOOK(82C1C548, kKeWaitMultiple)
WAIT_HOOK(82B073B0, kNtWait)
WAIT_HOOK(82B07280, kNtWait)
WAIT_HOOK(82B0A8B0, kNtWait)
WAIT_HOOK(82B07748, kNtWaitMultiple)
WAIT_HOOK(82B0ABB8, kSleep)
// D3D: waits on a kernel event around Swap.
WAIT_HOOK(82B2ACF0, kD3DWait)
