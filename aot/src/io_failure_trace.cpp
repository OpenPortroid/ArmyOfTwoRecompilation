// io_failure_trace.cpp - Who reports a disc read error.
//
// The game's I/O failure handlers (sub_829CB0A0, sub_829CC340, ... - each
// sets [0x830D6E68] = 1 and calls the XamShowDirtyDiscErrorUI wrapper
// sub_82A3B140; the rendering thread loop sub_8252CED8 also calls
// sub_829C8A68 while the flag is set) end the process: the SDK's
// XamShowDirtyDiscErrorUI calls exit(1), without a dump or a log line. A
// co-op instance vanished like that in Iraq (2026-10-07). (Single player
// from "checkpoint 05_10" does it every time: that checkpoint file is not on
// the disc - a wrong name, not a bug.) These are the game's file wrappers
// too (open / read / size, the flag only on failure): each call is kept in
// a ring of 64; when the disc error comes, the ring and the error go to
// io_failure.log: thread, r3..r6 (with the text they point to), and the
// guest call chain (back chain at [r1], saved LR at -8 of each frame).
#include <windows.h>

#include <bit>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/ppc.h>

namespace
{
  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }

  bool Readable(const uint8_t *base, uint32_t addr, uint32_t size)
  {
    if (addr < 0x10000)
    {
      return false;
    }
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(base + addr, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
    {
      return false;
    }
    return base + addr + size <= static_cast<const uint8_t *>(info.BaseAddress) + info.RegionSize;
  }

  // The text at a guest address: char or big-endian UTF-16, "" if none.
  std::string Text(const uint8_t *base, uint32_t addr)
  {
    if (!Readable(base, addr, 128))
    {
      return {};
    }
    std::string narrow, wide;
    for (uint32_t i = 0; i < 128; ++i)
    {
      char c = char(base[addr + i]);
      if (!c)
      {
        break;
      }
      narrow += std::isprint(uint8_t(c)) ? c : '.';
    }
    for (uint32_t i = 0; i < 64; ++i)
    {
      uint16_t c = uint16_t(base[addr + i * 2] << 8 | base[addr + i * 2 + 1]);
      if (!c)
      {
        break;
      }
      wide += c < 128 && std::isprint(c) ? char(c) : '.';
    }
    const std::string &best = wide.size() > narrow.size() ? wide : narrow;
    return best.size() >= 3 ? best : std::string();
  }

  std::mutex g_mutex;
  // The last file operations (the handlers are the game's file wrappers
  // too): written out only when the disc error comes.
  std::string g_ring[64];
  uint32_t g_next = 0;

  std::string Describe(PPCContext &ctx, uint8_t *base, const char *handler)
  {
    char line[160];
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] %s thread %lu lr %08X
", t.wHour, t.wMinute, t.wSecond,
                  t.wMilliseconds, handler, GetCurrentThreadId(), uint32_t(ctx.lr));
    std::string out = line;
    const uint32_t regs[] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
    for (int i = 0; i < 4; ++i)
    {
      std::string s = Text(base, regs[i]);
      std::snprintf(line, sizeof(line), "  r%d = %08X", i + 3, regs[i]);
      out += line + (s.empty() ? std::string() : " \"" + s + "\"") + "
";
    }
    out += "  chain:";
    uint32_t sp = ctx.r1.u32;
    for (int depth = 0; depth < 24 && Readable(base, sp, 4); ++depth)
    {
      uint32_t next = LoadU32(base, sp);
      if (next <= sp || !Readable(base, next - 8, 8))
      {
        break;
      }
      std::snprintf(line, sizeof(line), " %08X", LoadU32(base, next - 8));
      out += line;
      sp = next;
    }
    return out + "
";
  }

  // Every call goes to the ring; the disc error handlers write the ring and
  // themselves to io_failure.log.
  void Report(PPCContext &ctx, uint8_t *base, const char *handler, bool fatal)
  {
    std::string entry = Describe(ctx, base, handler);
    std::lock_guard lock(g_mutex);
    if (!fatal)
    {
      g_ring[g_next++ % 64] = std::move(entry);
      return;
    }
    if (FILE *f = std::fopen("io_failure.log", "a"))
    {
      std::fprintf(f, "=== disc error; the last file operations before it:
");
      for (uint32_t i = 0; i < 64; ++i)
      {
        std::fputs(g_ring[(g_next + i) % 64].c_str(), f);
      }
      std::fprintf(f, "=== the error:
%s", entry.c_str());
      std::fclose(f);
    }
  }
} // namespace

#define AO2_IO_FAILURE_HOOK(name)                                                                                     \
  extern "C" REX_FUNC(__imp__##name);                                                                                 \
  extern "C" REX_FUNC(name)                                                                                           \
  {                                                                                                                   \
    Report(ctx, base, #name);                                                                                         \
    __imp__##name(ctx, base);                                                                                         \
  }

AO2_IO_FAILURE_HOOK(sub_829C8A68, true)
AO2_IO_FAILURE_HOOK(sub_829C8B70, false)
AO2_IO_FAILURE_HOOK(sub_829CB0A0, false)
AO2_IO_FAILURE_HOOK(sub_829CC340, false)
AO2_IO_FAILURE_HOOK(sub_829CC440, false)
AO2_IO_FAILURE_HOOK(sub_829CC9A8, false)
AO2_IO_FAILURE_HOOK(sub_829CE728, false)
AO2_IO_FAILURE_HOOK(sub_829D05E0, false)
AO2_IO_FAILURE_HOOK(sub_82A3B140, true)
