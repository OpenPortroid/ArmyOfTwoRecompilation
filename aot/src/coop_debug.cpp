// coop_debug.cpp - Tracing for online co-op (EA Theater lobby, join flow).
//
// --ao2_coop_debug=true writes coop_debug.log in the working directory: the
// lobby replies the game builds (EGRS: whether the host lets a player in and
// why not) with the host call stack (exe offsets, symbolize with
// llvm-symbolizer --obj=armyoftworecomp.exe 0x140000000+offset).
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdarg>
#include <cstring>
#include <string>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <thread>

#include <windows.h>

#include "coop.h"
#include "log_files.h"

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DECLARE(int32_t, ao2_coop_input_margin_ms);
REXCVAR_DECLARE(bool, ao2_fps_unlock);
REXCVAR_DECLARE(int32_t, ao2_fps_unlock_mode);
REXCVAR_DECLARE(int32_t, ao2_fps_target);
REXCVAR_DECLARE(bool, ao2_mouse_aim);
REXCVAR_DECLARE(bool, ao2_god_mode);
int32_t CoopLeadFrames(int32_t latency_ms);

REXCVAR_DEFINE_BOOL(ao2_coop_debug, false, "Debug", "Log the online co-op lobby flow to coop_debug.log");

namespace
{
  std::mutex g_log_mutex;

  void Log(const char *fmt, ...)
  {
    if (!REXCVAR_GET(ao2_coop_debug))
    {
      return;
    }
    std::lock_guard lock(g_log_mutex);
    static FILE *f = ao2::OpenLogFile("coop_debug.log");
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

  void LogStack()
  {
    if (!REXCVAR_GET(ao2_coop_debug))
    {
      return;
    }
    void *frames[32];
    USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    char line[32 * 12] = {};
    size_t used = 0;
    for (USHORT i = 0; i < n && used + 12 < sizeof(line); i++)
    {
      auto a = reinterpret_cast<uintptr_t>(frames[i]);
      if (a > exe && a - exe < 0x10000000)
      {
        used += std::snprintf(line + used, sizeof(line) - used, " %llX", (unsigned long long)(a - exe));
      }
    }
    Log("  stack:%s", line);
  }
} // namespace

namespace
{
  uint64_t GuestArg(PPCContext &ctx, uint8_t *base, int index)
  {
    // r3..r10, then the caller's parameter save area (8 bytes per slot).
    switch (index)
    {
    case 0: return ctx.r3.u64;
    case 1: return ctx.r4.u64;
    case 2: return ctx.r5.u64;
    case 3: return ctx.r6.u64;
    case 4: return ctx.r7.u64;
    case 5: return ctx.r8.u64;
    case 6: return ctx.r9.u64;
    case 7: return ctx.r10.u64;
    }
    uint32_t a = ctx.r1.u32 + 16 + 8 * index;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
    {
      v = (v << 8) | base[a + i];
    }
    return v;
  }

  std::string GuestString(uint8_t *base, uint32_t addr, size_t max = 256)
  {
    std::string s;
    if (addr < 0x10000)
    {
      return "(null)";
    }
    for (size_t i = 0; i < max && base[addr + i]; i++)
    {
      char c = char(base[addr + i]);
      s += (c == '\n' || c == '\r') ? ' ' : c;
    }
    return s;
  }

  // Minimal printf for guest varargs (fmt is argument number fmt_index).
  std::string GuestFormat(PPCContext &ctx, uint8_t *base, int fmt_index)
  {
    std::string fmt = GuestString(base, uint32_t(GuestArg(ctx, base, fmt_index)), 512);
    std::string out;
    int arg = fmt_index + 1;
    for (size_t i = 0; i < fmt.size(); i++)
    {
      if (fmt[i] != '%')
      {
        out += fmt[i];
        continue;
      }
      size_t j = i + 1;
      std::string spec = "%";
      while (j < fmt.size() && std::strchr("-+ #0123456789.hlLzjt", fmt[j]))
      {
        spec += fmt[j++];
      }
      if (j >= fmt.size())
      {
        break;
      }
      char conv = fmt[j];
      i = j;
      if (conv == '%')
      {
        out += '%';
        continue;
      }
      uint64_t v = GuestArg(ctx, base, arg++);
      char buf[512];
      std::string clean;
      for (char c : spec)
      {
        if (c != 'h' && c != 'l' && c != 'L' && c != 'z' && c != 'j' && c != 't')
        {
          clean += c;
        }
      }
      switch (conv)
      {
      case 's':
        std::snprintf(buf, sizeof(buf), (clean + "s").c_str(), GuestString(base, uint32_t(v)).c_str());
        break;
      case 'c':
        std::snprintf(buf, sizeof(buf), "%c", char(v));
        break;
      case 'f': case 'g': case 'e':
      {
        double d;
        std::memcpy(&d, &v, 8);
        std::snprintf(buf, sizeof(buf), (clean + conv).c_str(), d);
        break;
      }
      case 'd': case 'i':
        std::snprintf(buf, sizeof(buf), (clean + "lld").c_str(),
                      spec.find("ll") != std::string::npos ? (long long)v : (long long)int32_t(v));
        break;
      default: // u x X p o
        std::snprintf(buf, sizeof(buf), (clean + "ll" + (conv == 'p' ? 'x' : conv)).c_str(),
                      spec.find("ll") != std::string::npos ? (unsigned long long)v
                                                            : (unsigned long long)uint32_t(v));
        break;
      }
      out += buf;
    }
    while (!out.empty() && out.back() == ' ')
    {
      out.pop_back();
    }
    return out;
  }
} // namespace

// EA debug output sinks (no output device is installed in the retail game):
// Print(this, text) and Printf(this, size_hint, fmt, ...).
namespace
{
  // DirtySock's debug output tells when EA's game manager runs: from then
  // on this is a network co-op session (coop.h), also before the lockstep.
  void TrackSession(const std::string &text)
  {
    if (text.find("GameManager listening") != std::string::npos || text.find("StartGame()") != std::string::npos ||
        text.find("Game Start Received") != std::string::npos)
    {
      ao2::coop::network_session = true;
    }
  }
} // namespace

extern "C" REX_FUNC(__imp__sub_822170F0);
extern "C" REX_FUNC(sub_822170F0)
{
  TrackSession(GuestString(base, ctx.r4.u32, 64));
  if (REXCVAR_GET(ao2_coop_debug))
  {
    Log("ea: %s", GuestString(base, ctx.r4.u32, 1024).c_str());
  }
  __imp__sub_822170F0(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_82217140);
extern "C" REX_FUNC(sub_82217140)
{
  TrackSession(GuestString(base, uint32_t(GuestArg(ctx, base, 2)), 64));
  if (REXCVAR_GET(ao2_coop_debug))
  {
    Log("ea: %s", GuestFormat(ctx, base, 2).c_str());
  }
  __imp__sub_82217140(ctx, base);
}

namespace
{
  // Hardware write breakpoint on a guest dword: logs who stores a nonzero
  // value there (code address and stack, exe offsets).
  uint8_t *g_watch_host = nullptr;
  std::atomic<int> g_watch_hits{0};

  LONG CALLBACK WatchHandler(EXCEPTION_POINTERS *e)
  {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1))
    {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    e->ContextRecord->Dr6 = 0;
    uint8_t *p = g_watch_host;
    uint32_t v = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    if (v != 0 && g_watch_hits.fetch_add(1) < 40)
    {
      auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
      Log("watch: stored %08X at rip exe+%llX", v, (unsigned long long)(e->ContextRecord->Rip - exe));
      LogStack();
    }
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  void WatchGuestDword(uint8_t *host, DWORD thread_id)
  {
    if (g_watch_host)
    {
      return;
    }
    g_watch_host = host;
    AddVectoredExceptionHandler(1, WatchHandler);
    // A thread cannot set its own debug registers: do it from a helper.
    std::thread([host, thread_id]() {
      HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, thread_id);
      if (!t)
      {
        return;
      }
      SuspendThread(t);
      CONTEXT c = {};
      c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(t, &c))
      {
        c.Dr0 = reinterpret_cast<DWORD64>(host);
        c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (3ull << 18); // local, write, 4 bytes
        SetThreadContext(t, &c);
      }
      ResumeThread(t);
      CloseHandle(t);
      Log("watch: hardware breakpoint set on %p for thread %lu", host, thread_id);
    }).detach();
  }
} // namespace

// AO2 co-op frame sync tick (r3 = sync object): frame number at +92, partner
// frame at +96, checksum of the shared rand() stream at +44 (+46 partner?),
// "problem this tick" flag at +52, consecutive problem ticks at +308 (the
// game leaves after 600).
// Called by the frame sync hook (frame_time.cpp) after every tick.
// Log line from other co-op files (coop_net_pump.cpp).
void CoopDebugPrintf(const char *fmt, ...)
{
  if (!REXCVAR_GET(ao2_coop_debug))
  {
    return;
  }
  char line[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  Log("%s", line);
}

double CoopDebugNowMs()
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

// Once a second: frame rate and how much of the frame the lockstep tick
// spends (waiting for the partner's input of the frame it needs).
static void LogFrameStats(uint32_t self, uint8_t *base, double tick_ms)
{
  auto s32 = [&](uint32_t offset) {
    uint8_t *p = base + self + offset;
    return int32_t(uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]);
  };
  static double window_start = 0, last_tick = 0, period_max = 0, tick_sum = 0, tick_max = 0;
  static uint32_t frames = 0, waited = 0;
  static int32_t lead_min = INT32_MAX, lead_max = INT32_MIN;
  double now = CoopDebugNowMs();
  if (last_tick > 0)
  {
    period_max = std::max(period_max, now - last_tick);
  }
  last_tick = now;
  if (window_start == 0)
  {
    window_start = now;
  }
  frames++;
  tick_sum += tick_ms;
  tick_max = std::max(tick_max, tick_ms);
  waited += tick_ms > 1.0;
  int32_t lead = s32(92) - s32(96);
  lead_min = std::min(lead_min, lead);
  lead_max = std::max(lead_max, lead);
  double elapsed = now - window_start;
  if (elapsed < 1000.0)
  {
    return;
  }
  Log("frames mode=%d fps=%.1f frame avg=%.2f max=%.2f ms | sync tick avg=%.2f max=%.2f ms, >1ms in %u frames | "
      "frame-peer %d..%d | +212=%d ms latency=%d ms +68=%d +72=%d +100=%d",
      s32(16), frames * 1000.0 / elapsed, elapsed / frames, period_max, tick_sum / frames, tick_max, waited, lead_min,
      lead_max, s32(212), s32(260), s32(68), s32(72), s32(100));
  window_start = now;
  period_max = tick_sum = tick_max = 0;
  frames = waited = 0;
  lead_min = INT32_MAX;
  lead_max = INT32_MIN;
}

void CoopDebugSyncTick(uint32_t self, uint8_t *base, double tick_ms)
{
  if (!REXCVAR_GET(ao2_coop_debug) || !self)
  {
    return;
  }
  LogFrameStats(self, base, tick_ms);
  auto u32 = [&](uint32_t offset) {
    uint8_t *p = base + self + offset;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  auto u16 = [&](uint32_t offset) {
    uint8_t *p = base + self + offset;
    return uint32_t(p[0]) << 8 | p[1];
  };
  static uint32_t last_flag = 0, ticks = 0;
  uint32_t flag = u32(52), bad = u32(308);
  if (u32(16) == 1)
  {
    WatchGuestDword(base + self + 52, GetCurrentThreadId());
  }
  ticks++;
  if (flag != last_flag || (flag && bad % 60 == 1) || ticks % 300 == 0)
  {
    // Lead over the partner = input delay (coop_input_delay.cpp).
    int32_t latency = int32_t(u32(260));
    Log("sync tick obj=%08X frame=%u peer=%u +16=%u +48=%08X cs44=%04X cs46=%04X flag52=%u bad308=%u +300=%u "
        "latency=%d ms frame=%d ms input delay=%d frames",
        self, u32(92), u32(96), u32(16), u32(48), u16(44), u16(46), flag, bad, u32(300), latency, int32_t(u32(212)),
        CoopLeadFrames(latency));
  }
  last_flag = flag;
}

// GameManagerHostedGame::EndGame(this) - the host ends the co-op game.
extern "C" REX_FUNC(__imp__sub_82248088);
extern "C" REX_FUNC(sub_82248088)
{
  Log("GameManagerHostedGame::EndGame this=%08X", ctx.r3.u32);
  LogStack();
  __imp__sub_82248088(ctx, base);
}

// GameManager: destroy a game object (manager, game) - leaving on either side.
extern "C" REX_FUNC(__imp__sub_8223BE38);
extern "C" REX_FUNC(sub_8223BE38)
{
  uint32_t game = ctx.r4.u32;
  auto field = [&](uint32_t offset) {
    uint8_t *p = base + game + offset;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  ao2::coop::network_session = false;
  Log("GameManager destroy game mgr=%08X game=%08X end reason %d code %d sub %d", ctx.r3.u32, game,
      int32_t(field(520)), int32_t(field(524)), int32_t(field(528)));
  LogStack();
  __imp__sub_8223BE38(ctx, base);
}

// GameManager: leave the current game (reason 6, code -606).
extern "C" REX_FUNC(__imp__sub_8223BCA8);
extern "C" REX_FUNC(sub_8223BCA8)
{
  Log("GameManager leave current game mgr=%08X", ctx.r3.u32);
  LogStack();
  __imp__sub_8223BCA8(ctx, base);
}

// GameManagerJoinedGame: leave the game normally (end reason 2).
extern "C" REX_FUNC(__imp__sub_82240E68);
extern "C" REX_FUNC(sub_82240E68)
{
  Log("GameManagerJoinedGame leave this=%08X r4=%08X", ctx.r3.u32, ctx.r4.u32);
  LogStack();
  __imp__sub_82240E68(ctx, base);
}

// Joined game failure (reason 4, code -605).
extern "C" REX_FUNC(__imp__sub_82241998);
extern "C" REX_FUNC(sub_82241998)
{
  Log("GameManager joined game failure this=%08X", ctx.r3.u32);
  LogStack();
  __imp__sub_82241998(ctx, base);
}

// Theater ECNL (leave / cancel entering a game): (packet, lid, gid).
extern "C" REX_FUNC(__imp__sub_82223700);
extern "C" REX_FUNC(sub_82223700)
{
  Log("ECNL lid=%u gid=%u", ctx.r5.u32, ctx.r6.u32);
  LogStack();
  __imp__sub_82223700(ctx, base);
}

// Theater EGRS (host's answer to a join request):
// (packet, lid, gid, allowed, pid, reason).
extern "C" REX_FUNC(__imp__sub_82223768);
extern "C" REX_FUNC(sub_82223768)
{
  Log("EGRS lid=%u gid=%u allowed=%u pid=%u reason=%u", ctx.r5.u32, ctx.r6.u32, ctx.r7.u32 & 0xFF, ctx.r8.u32,
      ctx.r9.u32);
  LogStack();
  __imp__sub_82223768(ctx, base);
}
