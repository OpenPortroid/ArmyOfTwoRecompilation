// script_trace.cpp - Debug: which UnrealScript functions run in the menus.
//
// --ao2_script_trace=true (or "set ao2_script_trace true" in console.txt)
// logs every script function UObject::ProcessInternal (sub_82445C30, r3 =
// the object, r4 = the FFrame: its Node is the function) runs while the
// world is the main menu's (Shell) to script_trace.log: the object, then
// the function's class and name. A function is logged its first 100 times;
// a repeat of the line before is counted instead. For telling two menu runs
// apart (the campaign's split-screen after a versus match and after the
// start).
#include <bit>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "log_files.h"
#include "ue3_reflect.h"

namespace ao2::split_join
{
  // split_join.cpp
  extern uint32_t g_cause_event;
  extern uint32_t g_login_status;
  bool OnCauseEvent(PPCContext &ctx, uint8_t *base); // true: the function does not run
  void OnLoginStatus(PPCContext &ctx, uint8_t *base);
  void AfterCauseEvent(PPCContext &ctx, uint8_t *base);
}
namespace ao2::lobby_visibility
{
  bool OnCauseEvent(PPCContext &ctx, uint8_t *base); // lobby_visibility.cpp; true: held back
}

REXCVAR_DEFINE_BOOL(ao2_script_trace, false, "Debug",
                    "Debug: log the UnrealScript functions run in the main menu to logs/debug/script_trace.log");

namespace
{
  constexpr uint32_t kWorld = 0x831F2CC8; // GWorld; +40 its package
  constexpr uint32_t kClass = 52;         // UObject.Class
  constexpr uint32_t kOuter = 40;         // UObject.Outer
  constexpr int kMaxPerFunction = 100;

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("script_trace.log");
    return f;
  }

  bool Plausible(uint32_t object)
  {
    return object >= 0x40000000 && object < 0x80000000 && !(object & 3);
  }

  // The FFrame's Node: the first of its words +4..+24 that is a UFunction
  // (found once).
  uint32_t FrameFunction(const uint8_t *base, uint32_t frame)
  {
    static uint32_t function_class = 0;
    static int32_t node = -1;
    if (!function_class)
    {
      function_class = ao2::ue3::FindClass(base, "Function");
      if (!function_class)
      {
        return 0;
      }
    }
    if (node < 0)
    {
      for (uint32_t off = 4; off <= 24; off += 4)
      {
        uint32_t w = LoadU32(base, frame + off);
        if (Plausible(w) && LoadU32(base, w + kClass) == function_class)
        {
          node = int32_t(off);
          if (Log())
          {
            std::fprintf(Log(), "FFrame Node at +%u\n", off);
          }
          break;
        }
      }
      if (node < 0)
      {
        return 0;
      }
    }
    uint32_t function = LoadU32(base, frame + uint32_t(node));
    return Plausible(function) && LoadU32(base, function + kClass) == function_class ? function : 0;
  }

  bool InShell(const uint8_t *base)
  {
    uint32_t world = LoadU32(base, kWorld);
    uint32_t package = world ? LoadU32(base, world + 40) : 0;
    return package && ao2::ue3::ObjectName(base, package) == "Shell";
  }

  void Trace(const uint8_t *base, uint32_t object, uint32_t frame)
  {
    if (!Log() || !Plausible(frame) || !InShell(base))
    {
      return;
    }
    uint32_t function = FrameFunction(base, frame);
    if (!function)
    {
      return;
    }
    static std::unordered_map<uint32_t, int> counts;
    if (++counts[function] > kMaxPerFunction)
    {
      return;
    }
    uint32_t owner = LoadU32(base, function + kOuter);
    std::string line = (Plausible(object) ? ao2::ue3::ObjectName(base, object) : std::string("?")) + " " +
                       (Plausible(owner) ? ao2::ue3::ObjectName(base, owner) : std::string("?")) + "." +
                       ao2::ue3::ObjectName(base, function);
    static std::string last;
    static int repeats = 0;
    if (line == last)
    {
      ++repeats;
      return;
    }
    if (repeats)
    {
      std::fprintf(Log(), "  (x%d)\n", repeats + 1);
      repeats = 0;
    }
    std::fprintf(Log(), "%s\n", line.c_str());
    std::fflush(Log());
    last = std::move(line);
  }
  // The last script calls (object, function), always kept: who caused an event is written out when it comes
  // (split_join.cpp, QuitCoopClientLobby). Raw pointers only here; names are read when written out.
  constexpr uint32_t kRecent = 64;
  uint32_t g_recent_object[kRecent], g_recent_function[kRecent];
  uint32_t g_recent_next = 0;
} // namespace

namespace ao2::script_trace
{
  // The script functions run last, oldest first ("Object Class.Function", repeats folded).
  std::string Recent(const uint8_t *base, uint32_t count)
  {
    count = count > kRecent ? kRecent : count;
    std::string out, last;
    int repeats = 0;
    auto flush = [&]() {
      if (!last.empty())
      {
        out += "  " + last + (repeats > 1 ? " (x" + std::to_string(repeats) + ")" : std::string()) + "\n";
      }
    };
    for (uint32_t i = 0; i < count; ++i)
    {
      uint32_t slot = (g_recent_next + kRecent - count + i) % kRecent;
      uint32_t object = g_recent_object[slot], function = g_recent_function[slot];
      if (!Plausible(function))
      {
        continue;
      }
      uint32_t owner = LoadU32(base, function + kOuter);
      std::string line = (Plausible(object) ? ao2::ue3::ObjectName(base, object) : std::string("?")) + " " +
                         (Plausible(owner) ? ao2::ue3::ObjectName(base, owner) : std::string("?")) + "." +
                         ao2::ue3::ObjectName(base, function);
      if (line == last)
      {
        ++repeats;
        continue;
      }
      flush();
      last = std::move(line);
      repeats = 1;
    }
    flush();
    return out;
  }
} // namespace ao2::script_trace

// UObject::ProcessInternal (r3 = object, r4 = FFrame, r5 = result).
extern "C" REX_FUNC(__imp__sub_82445C30);
extern "C" REX_FUNC(sub_82445C30)
{
  if (REXCVAR_GET(ao2_script_trace))
  {
    Trace(base, ctx.r3.u32, ctx.r4.u32);
  }
  // FFrame +4: its Node, the function.
  const uint32_t function = LoadU32(base, ctx.r4.u32 + 4);
  g_recent_object[g_recent_next] = ctx.r3.u32;
  g_recent_function[g_recent_next] = function;
  g_recent_next = (g_recent_next + 1) % kRecent;
  if (function == ao2::split_join::g_login_status)
  {
    ao2::split_join::OnLoginStatus(ctx, base);
  }
  else if (function == ao2::split_join::g_cause_event &&
           (ao2::split_join::OnCauseEvent(ctx, base) || ao2::lobby_visibility::OnCauseEvent(ctx, base)))
  {
    return;
  }
  __imp__sub_82445C30(ctx, base);
  if (function == ao2::split_join::g_cause_event)
  {
    ao2::split_join::AfterCauseEvent(ctx, base);
  }
}
