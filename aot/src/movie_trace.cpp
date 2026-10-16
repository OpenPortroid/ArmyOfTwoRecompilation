// movie_trace.cpp - Debug: who starts and stops the full-screen movie.
//
// --ao2_movie_debug=true logs the UnrealScript natives
// AAO2PlayerControllerNative::execClientPlayMovie (sub_8230C9E8),
// execClientStopMovie (sub_8230CAE0) and execShowLoadingMovie (sub_822EEB18)
// to movie_trace.log with the script frame chain (raw words, UObjects named),
// to find what keeps the "LOADING MAP" screen up.
#include <windows.h>

#include <bit>
#include <cstdio>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "log_files.h"
#include "ue3_reflect.h"

REXCVAR_DEFINE_BOOL(ao2_movie_debug, false, "Debug", "Debug: log full-screen movie natives to movie_trace.log");

namespace
{

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("movie_trace.log");
    return f;
  }

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }

  // False where the host page is not readable.
  bool Readable(const uint8_t *base, uint32_t addr)
  {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(base + addr, &info, sizeof(info)) && info.State == MEM_COMMIT &&
           !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (info.Protect & 0xEE);
  }

  bool Plausible(uint32_t p)
  {
    return p >= 0x40000000 && p < 0x80000000 && !(p & 3);
  }


  // "Name(Class)" of a UObject, or "" when p does not look like one.
  std::string ObjName(const uint8_t *base, uint32_t p)
  {
    auto name = [&](uint32_t idx) -> std::string {
      if (idx >= 0x100000)
      {
        return {};
      }
      uint32_t e = LoadU32(base, ao2::ue3::GNames(base) + idx * 4);
      if (!Plausible(e) || !Readable(base, e + 16))
      {
        return {};
      }
      const char *s = reinterpret_cast<const char *>(base + e + 16);
      size_t n = strnlen(s, 64);
      for (size_t i = 0; i < n; ++i)
      {
        if (s[i] < 32 || s[i] > 126)
        {
          return {};
        }
      }
      return std::string(s, n);
    };
    if (!Plausible(p) || !Readable(base, p) || !Readable(base, p + 52))
    {
      return {};
    }
    uint32_t cls = LoadU32(base, p + 52);
    if (!Plausible(cls) || !Readable(base, cls + 44))
    {
      return {};
    }
    std::string n = name(LoadU32(base, p + 44));
    std::string c = name(LoadU32(base, cls + 44));
    if (n.empty() || c.empty())
    {
      return {};
    }
    return n + "(" + c + ")";
  }

  void DumpFrame(const uint8_t *base, uint32_t frame, int depth)
  {
    FILE *f = Log();
    if (!f || depth > 10 || frame < 0x40000000 || frame >= 0x80000000 || !Readable(base, frame) ||
        !Readable(base, frame + 60))
    {
      return;
    }
    std::fprintf(f, "  %*sframe %08X:", depth * 2, "", frame);
    uint32_t next[8] = {};
    int nnext = 0;
    for (uint32_t off = 0; off < 64; off += 4)
    {
      uint32_t w = LoadU32(base, frame + off);
      std::string n = ObjName(base, w);
      std::fprintf(f, " +%u=%08X%s%s", off, w, n.empty() ? "" : " ", n.c_str());
      // A candidate previous frame: a pointer whose +8 is a UObject.
      if (off >= 12 && n.empty() && w >= 0x40000000 && w < 0x80000000 && !(w & 3) && w != frame &&
          Readable(base, w + 8) &&
          !ObjName(base, LoadU32(base, w + 8)).empty() &&
          nnext < 8)
      {
        next[nnext++] = w;
      }
    }
    std::fprintf(f, "\n");
    for (int i = 0; i < nnext && depth < 2; ++i)
    {
      DumpFrame(base, next[i], depth + 1);
    }
    if (nnext && depth >= 2)
    {
      DumpFrame(base, next[0], depth + 1);
    }
  }

  void Trace(const char *what, PPCContext &ctx, uint8_t *base)
  {
    FILE *f = Log();
    if (!f)
    {
      return;
    }
    uint32_t self = ctx.r3.u32, frame = ctx.r4.u32;
    std::fprintf(f, "%s self=%08X %s\n", what, self, ObjName(base, self).c_str());
    DumpFrame(base, frame, 0);
    std::fflush(f);
  }
} // namespace

extern "C" REX_FUNC(__imp__sub_8230C9E8);
extern "C" REX_FUNC(sub_8230C9E8)
{
  if (REXCVAR_GET(ao2_movie_debug))
  {
    Trace("ClientPlayMovie", ctx, base);
  }
  __imp__sub_8230C9E8(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_8230CAE0);
extern "C" REX_FUNC(sub_8230CAE0)
{
  if (REXCVAR_GET(ao2_movie_debug))
  {
    Trace("ClientStopMovie", ctx, base);
  }
  __imp__sub_8230CAE0(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_822EEB18);
extern "C" REX_FUNC(sub_822EEB18)
{
  if (REXCVAR_GET(ao2_movie_debug))
  {
    Trace("ShowLoadingMovie", ctx, base);
  }
  __imp__sub_822EEB18(ctx, base);
}

// The implementations: (controller, const FString *movie, ...).
namespace
{
  void TraceImpl(const char *what, PPCContext &ctx, uint8_t *base)
  {
    FILE *f = Log();
    if (!f)
    {
      return;
    }
    uint32_t s = ctx.r4.u32;
    bool ok = s >= 0x40000000 && s < 0x80000000 && Readable(base, s); // heap or guest stack
    uint32_t data = ok ? LoadU32(base, s) : 0, count = ok ? LoadU32(base, s + 4) : 0;
    std::string name;
    if (Plausible(data) && count && count < 256 && Readable(base, data))
    {
      name.assign(reinterpret_cast<const char *>(base + data), strnlen(reinterpret_cast<const char *>(base + data), count));
    }
    std::fprintf(f, "%s movie=\"%s\" r5=%08X r6=%08X\n", what, name.c_str(), ctx.r5.u32, ctx.r6.u32);
    std::fflush(f);
  }
} // namespace

extern "C" REX_FUNC(__imp__sub_82382D08);
extern "C" REX_FUNC(sub_82382D08)
{
  if (REXCVAR_GET(ao2_movie_debug))
  {
    TraceImpl("  play impl", ctx, base);
  }
  __imp__sub_82382D08(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_82382EA8);
extern "C" REX_FUNC(sub_82382EA8)
{
  if (REXCVAR_GET(ao2_movie_debug))
  {
    TraceImpl("  stop impl", ctx, base);
  }
  __imp__sub_82382EA8(ctx, base);
}

// Movie player vtable entries 6 and 7 (0x82077CD8): play / stop, with the
// guest call stack (back chain: [sp] = caller's sp, saved LR at [caller sp - 8]).
namespace
{
  void TraceGuest(const char *what, PPCContext &ctx, uint8_t *base)
  {
    FILE *f = Log();
    if (!f)
    {
      return;
    }
    std::fprintf(f, "%s r3=%08X r4=%08X r5=%08X r6=%08X lr=%08X stack:", what, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                 ctx.r6.u32, uint32_t(ctx.lr));
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < 16 && sp; ++i)
    {
      if (!Readable(base, sp))
      {
        break;
      }
      uint32_t prev = LoadU32(base, sp);
      if (prev <= sp || prev - sp > 0x100000 || !Readable(base, prev - 8))
      {
        break;
      }
      std::fprintf(f, " %08X", LoadU32(base, prev - 8));
      sp = prev;
    }
    std::fprintf(f, "\n");
    std::fflush(f);
  }
} // namespace

extern "C" REX_FUNC(__imp__sub_82587190);
extern "C" REX_FUNC(sub_82587190)
{
  if (REXCVAR_GET(ao2_movie_debug))
  {
    TraceGuest("player vt6 (play?)", ctx, base);
  }
  __imp__sub_82587190(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_825749F8);
extern "C" REX_FUNC(sub_825749F8)
{
  if (REXCVAR_GET(ao2_movie_debug))
  {
    TraceGuest("player vt7 (stop?)", ctx, base);
  }
  __imp__sub_825749F8(ctx, base);
}
