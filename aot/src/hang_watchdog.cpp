// hang_watchdog.cpp - Minidump when the game stops presenting frames.
//
// A hang of the guest threads (a lost wake-up, a deadlock) looks the same
// from outside every time: the window freezes, the GPU thread idles, the
// vblank thread keeps running. The vblank heartbeat (every 10 s) reports the
// swap count here; after kStalledHeartbeats without a new frame the process
// writes hang_<date>_<time>.dmp next to the exe once per session (stacks of
// every thread; `python tools\dumpinfo.py <dmp> --threads` shows them). The
// dump has no guest memory, so the guest call stacks go to the log as well.
//
// A crash (an exception nothing handles, abort, std::terminate, a pure
// virtual call, a bad CRT parameter) writes crash_<date>_<time>.dmp the same
// way, with the exception, and crash_<date>_<time>.txt with the exception
// code, the address (exe + offset, for the PDB) and the guest stacks. In
// online co-op the partner's game once closed by itself ~1 min into a level
// without either file kind to show why.
#include "hang_watchdog.h"

#include "ue3_reflect.h"

#include <windows.h>

#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

namespace
{
  constexpr int kStalledHeartbeats = 3; // 30 s

  uint64_t g_last_swaps = 0;
  int g_stalled = 0;
  bool g_written = false;

  // A big-endian guest word, or false where the host page is not readable (a
  // stale back chain can point into a reserved but uncommitted stack page).
  bool LoadGuest32(uint32_t address, uint32_t &value)
  {
    auto *p = rex::system::kernel_memory()->TranslateVirtual<const uint8_t *>(address);
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(info.Protect & 0xEE)) // any readable protection
    {
      return false;
    }
    uint32_t v;
    std::memcpy(&v, p, 4);
    value = __builtin_bswap32(v);
    return true;
  }
  // "<prefix>_<date>_<time>" (local time).
  void StampedName(char *out, size_t size, const char *prefix)
  {
    std::time_t now = std::time(nullptr);
    std::tm local;
    localtime_s(&local, &now);
    std::snprintf(out, size, "%s_%04d%02d%02d_%02d%02d%02d", prefix, local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
  }

  bool WriteDump(const char *path, EXCEPTION_POINTERS *exception)
  {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
      return false;
    }
    MINIDUMP_EXCEPTION_INFORMATION info = {GetCurrentThreadId(), exception, FALSE};
    auto type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, exception ? &info : nullptr,
                                nullptr, nullptr);
    CloseHandle(file);
    return ok != FALSE;
  }

  std::atomic<bool> g_crashed{false};

  bool CommittedReadable(const uint8_t *p, MEMORY_BASIC_INFORMATION &info)
  {
    return VirtualQuery(p, &info, sizeof(info)) && info.State == MEM_COMMIT &&
           !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (info.Protect & 0xEE);
  }

  // Which live objects hold a pointer into [lo, hi] (their first 16 KB) -
  // the field to look at. Reads only committed memory.
  std::string FindReferences(uint32_t lo, uint32_t hi)
  {
    const uint8_t *base = rex::system::kernel_memory()->TranslateVirtual<const uint8_t *>(0);
    std::string out;
    char line[160];
    std::snprintf(line, sizeof(line), "  objects pointing at %08X..%08X:\n", lo, hi);
    out += line;
    int hits = 0;
    uint32_t count = ao2::ue3::ObjectCount(base);
    for (uint32_t i = 0; i < count && hits < 40; ++i)
    {
      uint32_t object = ao2::ue3::ObjectAt(base, i);
      MEMORY_BASIC_INFORMATION info;
      const uint8_t *p = base + object;
      if (!object || !CommittedReadable(p, info))
      {
        continue;
      }
      size_t avail = static_cast<const uint8_t *>(info.BaseAddress) + info.RegionSize - p;
      uint32_t size = uint32_t(std::min<size_t>(avail, 16384)) & ~3u;
      for (uint32_t at = 0; at + 4 <= size && hits < 40; at += 4)
      {
        uint32_t v;
        std::memcpy(&v, p + at, 4);
        v = __builtin_bswap32(v);
        if (v >= lo && v <= hi)
        {
          ++hits;
          uint32_t cls = 0;
          std::memcpy(&cls, p + 52, 4);
          cls = __builtin_bswap32(cls);
          std::snprintf(line, sizeof(line), "    %08X %s (%s) +%u = %08X\n", object,
                        ao2::ue3::ObjectName(base, object).c_str(), ao2::ue3::ObjectName(base, cls).c_str(), at, v);
          out += line;
        }
      }
    }
    if (!hits)
    {
      out += "    none in object bodies\n";
    }
    return out;
  }

  // Every aligned guest word equal to `value` in committed guest memory
  // 0x40000000..0x7FFFFFFF (the heaps: also TArray buffers, which the
  // garbage collector follows too).
  std::string FindWords(uint32_t value)
  {
    const uint8_t *base = rex::system::kernel_memory()->TranslateVirtual<const uint8_t *>(0);
    std::string out;
    char line[96];
    std::snprintf(line, sizeof(line), "  heap words = %08X:", value);
    out += line;
    uint32_t want = __builtin_bswap32(value);
    int hits = 0;
    for (uint64_t at = 0x40000000; at < 0x80000000 && hits < 40;)
    {
      MEMORY_BASIC_INFORMATION info;
      if (!VirtualQuery(base + at, &info, sizeof(info)))
      {
        break;
      }
      uint64_t end = uint64_t(static_cast<const uint8_t *>(info.BaseAddress) + info.RegionSize - base);
      if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (info.Protect & 0xEE))
      {
        for (uint64_t a = at; a + 4 <= end && hits < 40; a += 4)
        {
          uint32_t v;
          std::memcpy(&v, base + a, 4);
          if (v == want)
          {
            ++hits;
            std::snprintf(line, sizeof(line), " %08X", uint32_t(a));
            out += line;
          }
        }
      }
      at = std::max(end, at + 0x1000);
    }
    out += hits ? "\n" : " none\n";
    return out;
  }

  // 64 bytes at a guest address, "" when not readable.
  std::string HexAt(uint32_t address)
  {
    const uint8_t *base = rex::system::kernel_memory()->TranslateVirtual<const uint8_t *>(0);
    MEMORY_BASIC_INFORMATION info;
    const uint8_t *p = base + address;
    if (!CommittedReadable(p, info) ||
        static_cast<const uint8_t *>(info.BaseAddress) + info.RegionSize < p + 64)
    {
      return "  (not readable)\n";
    }
    std::string out = "  ";
    char word[12];
    for (int i = 0; i < 64; i += 4)
    {
      uint32_t v;
      std::memcpy(&v, p + i, 4);
      std::snprintf(word, sizeof(word), "%08X ", __builtin_bswap32(v));
      out += word;
    }
    return out + "\n";
  }

  // The reference fields of `object` (class `cls`) by name: object
  // references with their value, arrays with data / count and where in them
  // `bad` (the value the collector died on) is. Which field of the audio
  // device held garbage (2026-10-09) was the open question.
  std::string ReferenceFields(const uint8_t *base, uint32_t object, uint32_t cls, uint32_t bad)
  {
    std::string out;
    char line[200];
    std::snprintf(line, sizeof(line), "  reference fields (bad value %08X):\n", bad);
    out += line;
    MEMORY_BASIC_INFORMATION info;
    for (const ao2::ue3::Property &p : ao2::ue3::Properties(base, cls))
    {
      bool object_ref = p.type == "ObjectProperty" || p.type == "ClassProperty" || p.type == "ComponentProperty" ||
                        p.type == "InterfaceProperty";
      bool array = p.type == "ArrayProperty";
      if ((!object_ref && !array) || p.offset < 0 || p.offset > 0x10000 ||
          !CommittedReadable(base + object + p.offset, info))
      {
        continue;
      }
      uint32_t v;
      std::memcpy(&v, base + object + p.offset, 4);
      v = __builtin_bswap32(v);
      if (object_ref)
      {
        std::snprintf(line, sizeof(line), "    +%d %s.%s (%s) = %08X %s%s\n", p.offset, p.owner.c_str(),
                      p.name.c_str(), p.type.c_str(), v, v ? ao2::ue3::ObjectName(base, v).c_str() : "",
                      v == bad ? "  <== BAD" : "");
        out += line;
        continue;
      }
      uint32_t count;
      std::memcpy(&count, base + object + p.offset + 4, 4);
      count = __builtin_bswap32(count);
      std::string where;
      if (v && count && count < 100000 && CommittedReadable(base + v, info))
      {
        size_t avail = static_cast<const uint8_t *>(info.BaseAddress) + info.RegionSize - (base + v);
        uint32_t words = uint32_t(std::min<size_t>(avail / 4, 16384));
        for (uint32_t i = 0; i < words; ++i)
        {
          uint32_t e;
          std::memcpy(&e, base + v + i * 4, 4);
          if (__builtin_bswap32(e) == bad && where.size() < 120)
          {
            where += " [" + std::to_string(i) + "]";
          }
        }
      }
      std::snprintf(line, sizeof(line), "    +%d %s.%s (ArrayProperty) data %08X count %u%s%s\n", p.offset,
                    p.owner.c_str(), p.name.c_str(), v, count, where.empty() ? "" : "  <== BAD at word",
                    where.c_str());
      out += line;
    }
    return out;
  }

  // Who put `bad` into the collector's queue. sub_82531730 keeps its state in
  // the structure its caller passed (r3 -> r21): the objects to process (a
  // TArray at +0 / +4, appended to while it runs) and the current one at
  // +12. Found by its +12 near the stack pointer, then the objects queued
  // before `bad` are searched for it: reflected reference fields and array
  // elements first, then anywhere in their first 4 KB (native references).
  // 0x42860000 (67.0, a float in a dozen HUD and weapon defaults) was the
  // "object" on 2026-10-10: the referencer, not the value, is the question.
  std::string GcQueue(const uint8_t *base, uint32_t sp, uint32_t bad)
  {
    std::string out;
    char line[220];
    MEMORY_BASIC_INFORMATION info;
    auto load = [&](uint32_t a, uint32_t &v) {
      if (!CommittedReadable(base + a, info) ||
          static_cast<const uint8_t *>(info.BaseAddress) + info.RegionSize < base + a + 4)
      {
        return false;
      }
      std::memcpy(&v, base + a, 4);
      v = __builtin_bswap32(v);
      return true;
    };
    uint32_t state = 0, data = 0, count = 0;
    for (uint32_t a = sp & ~3u; a < sp + 0x8000 && !state; a += 4)
    {
      uint32_t current, d, n, p;
      // the structure on the stack itself, or a pointer to it there
      for (uint32_t candidate : {a, load(a, p) ? p : 0u})
      {
        if (candidate && load(candidate + 12, current) && current == bad && load(candidate, d) && load(candidate + 4, n) &&
            d >= 0x40000000 && d < 0x80000000 && n && n < 2000000)
        {
          state = candidate, data = d, count = n;
          break;
        }
      }
    }
    if (!state)
    {
      return "  collector queue: not found near the stack\n";
    }
    uint32_t index = count;
    for (uint32_t i = 0; i < count; ++i)
    {
      uint32_t v;
      if (load(data + i * 4, v) && v == bad)
      {
        index = i;
        break;
      }
    }
    std::snprintf(line, sizeof(line), "  collector queue: state %08X data %08X count %u, bad object at [%u]\n", state,
                  data, count, index);
    out += line;
    if (index == count)
    {
      return out;
    }
    std::unordered_map<uint32_t, std::vector<ao2::ue3::Property>> classes;
    int hits = 0;
    for (uint32_t j = index; j-- > 0 && index - j <= 20000 && hits < 12;)
    {
      uint32_t object, cls;
      if (!load(data + j * 4, object) || !load(object + 52, cls))
      {
        continue;
      }
      auto it = classes.find(cls);
      if (it == classes.end())
      {
        it = classes.emplace(cls, ao2::ue3::Properties(base, cls)).first;
      }
      std::string found;
      for (const ao2::ue3::Property &p : it->second)
      {
        uint32_t v, n;
        if (p.offset < 0 || p.offset > 0x10000 || !load(object + p.offset, v))
        {
          continue;
        }
        if (v == bad)
        {
          found += " +" + std::to_string(p.offset) + " " + p.owner + "." + p.name + " (" + p.type + ")";
        }
        else if (p.type == "ArrayProperty" && v >= 0x40000000 && v < 0x80000000 && load(object + p.offset + 4, n) &&
                 n < 100000)
        {
          for (uint32_t k = 0; k < n; ++k)
          {
            uint32_t e;
            if (load(v + k * 4, e) && e == bad)
            {
              found += " +" + std::to_string(p.offset) + " " + p.owner + "." + p.name + "[word " +
                       std::to_string(k) + "] (array " + std::to_string(n) + ")";
              break;
            }
          }
        }
      }
      if (found.empty())
      {
        for (uint32_t at = 0; at < 4096; at += 4)
        {
          uint32_t v;
          if (load(object + at, v) && v == bad)
          {
            found += " raw +" + std::to_string(at);
          }
        }
      }
      if (!found.empty())
      {
        ++hits;
        std::snprintf(line, sizeof(line), "    [%u] %08X %s (%s):", j, object, ao2::ue3::ObjectName(base, object).c_str(),
                      ao2::ue3::ObjectName(base, cls).c_str());
        out += line + found.substr(0, 400) + "\n";
      }
    }
    if (!hits)
    {
      out += "    no queued object before it holds it\n";
    }
    return out;
  }

  // The garbage collector (sub_82531730) crashed on a guest thread: the
  // object it was at (r3) - what is there and who points at it.
  std::string GcSuspects()
  {
    std::string out;
    char line[192];
    const uint8_t *base = rex::system::kernel_memory()->TranslateVirtual<const uint8_t *>(0);
    auto threads = rex::system::kernel_state()->object_table()->GetObjectsByType<rex::system::XThread>();
    for (auto &thread : threads)
    {
      if (!thread->is_guest_thread() || !thread->thread_state() || !thread->thread_state()->context())
      {
        continue;
      }
      const PPCContext *ctx = thread->thread_state()->context();
      uint32_t lr = uint32_t(ctx->lr), r3 = ctx->r3.u32;
      if (lr < 0x82531730 || lr >= 0x82532400 || r3 < 0x40000000 || r3 >= 0x80000000)
      {
        continue;
      }
      uint32_t cls = 0;
      MEMORY_BASIC_INFORMATION info;
      if (CommittedReadable(base + r3, info))
      {
        std::memcpy(&cls, base + r3 + 52, 4);
        cls = __builtin_bswap32(cls);
      }
      std::snprintf(line, sizeof(line), "garbage collector at object %08X %s (%s), guest thread %08X:\n", r3,
                    ao2::ue3::ObjectName(base, r3).c_str(), ao2::ue3::ObjectName(base, cls).c_str(),
                    thread->thread_id());
      out += line;
      out += HexAt(r3);
      out += ReferenceFields(base, r3, cls, ctx->r11.u32);
      out += GcQueue(base, ctx->r1.u32, r3);
      out += FindReferences(r3, r3);
      out += FindWords(r3);
    }
    return out;
  }

  // Once per process: the dump, then what the dump cannot hold.
  void WriteCrash(const char *what, EXCEPTION_POINTERS *exception)
  {
    if (g_crashed.exchange(true))
    {
      return;
    }
    char name[64], path[80];
    StampedName(name, sizeof(name), "crash");
    std::snprintf(path, sizeof(path), "%s.dmp", name);
    bool dumped = WriteDump(path, exception);
    std::snprintf(path, sizeof(path), "%s.txt", name);
    FILE *f = std::fopen(path, "w");
    if (!f)
    {
      return;
    }
    auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(f, "%s on thread %lu, dump %s\n", what, GetCurrentThreadId(), dumped ? "written" : "failed");
    if (exception && exception->ExceptionRecord)
    {
      const EXCEPTION_RECORD *r = exception->ExceptionRecord;
      auto address = reinterpret_cast<uintptr_t>(r->ExceptionAddress);
      std::fprintf(f, "exception %08lX at %p (exe+%llX)", r->ExceptionCode, r->ExceptionAddress,
                   (unsigned long long)(address - exe));
      for (DWORD i = 0; i < r->NumberParameters && i < 4; ++i)
      {
        std::fprintf(f, " p%lu=%llX", i, (unsigned long long)r->ExceptionInformation[i]);
      }
      std::fprintf(f, "\n");
    }
    std::fflush(f);
    std::fprintf(f, "%s", ao2::hang::GuestStacks(true).c_str());
    std::fflush(f);
    // An access violation in guest memory: who points there.
    const EXCEPTION_RECORD *record = exception ? exception->ExceptionRecord : nullptr;
    if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
      auto guest = rex::system::kernel_memory()->TranslateVirtual<const uint8_t *>(0);
      uintptr_t host = record->ExceptionInformation[1];
      uintptr_t lo = reinterpret_cast<uintptr_t>(guest);
      if (host >= lo && host - lo < 0x100000000ull)
      {
        uint32_t address = uint32_t(host - lo);
        std::fprintf(f, "%s", FindReferences(address > 512 ? address - 512 : 0, address).c_str());
        std::fflush(f);
        std::fprintf(f, "%s", GcSuspects().c_str());
      }
    }
    std::fclose(f);
  }

  LONG WINAPI CrashFilter(EXCEPTION_POINTERS *exception)
  {
    WriteCrash("unhandled exception", exception);
    return EXCEPTION_CONTINUE_SEARCH;
  }

  void CrashNow(const char *what)
  {
    // A real exception context for the dump.
    __try
    {
      RaiseException(0xE0A02CA5, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
    __except (WriteCrash(what, GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }
} // namespace

namespace ao2::hang
{
  void InstallCrashHandler()
  {
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate([] {
      CrashNow("std::terminate");
      std::abort();
    });
    std::signal(SIGABRT, [](int) { CrashNow("abort"); });
    _set_purecall_handler([] {
      CrashNow("pure virtual call");
      std::abort();
    });
    _set_invalid_parameter_handler([](const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t) {
      CrashNow("invalid CRT parameter");
      std::abort();
    });
  }

  bool OnHeartbeat(uint64_t swaps, char *path, size_t path_size)
  {
    if (swaps != g_last_swaps || !swaps)
    {
      g_last_swaps = swaps;
      g_stalled = 0;
      return false;
    }
    if (++g_stalled < kStalledHeartbeats || g_written)
    {
      return false;
    }
    g_written = true;
    char name[64];
    StampedName(name, sizeof(name), "hang");
    std::snprintf(path, path_size, "%s.dmp", name);
    return WriteDump(path, nullptr);
  }

  std::string GuestStacks(bool registers)
  {
    // Guest stacks: 0x70000000-0x7F000000 (XThread kStackAddressRangeBegin/End).
    // Frames link through the back chain at [r1]; a function's return address
    // is saved at -8 from its caller's stack pointer (mflr r12; stw r12,-8(r1)).
    auto on_stack = [](uint32_t a) { return a >= 0x70000000u && a < 0x7F000000u && !(a & 3); };
    auto in_code = [](uint32_t a) { return a >= 0x82000000u && a < 0x84000000u; };
    std::string out;
    char line[96];
    // Render command fences in flight (render_fence.cpp): the game thread waits
    // while it is above 1.
    uint32_t fences = 0;
    if (LoadGuest32(0x831E5C10, fences))
    {
      std::snprintf(line, sizeof(line), "  render command fences pending: %d\n", int32_t(fences));
      out += line;
    }
    auto threads = rex::system::kernel_state()->object_table()->GetObjectsByType<rex::system::XThread>();
    for (auto &thread : threads)
    {
      if (!thread->is_guest_thread() || !thread->thread_state() || !thread->thread_state()->context())
      {
        continue;
      }
      const PPCContext *ctx = thread->thread_state()->context();
      std::snprintf(line, sizeof(line), "  guest thread %08X '%.32s': lr %08X", thread->thread_id(),
                    thread->name().c_str(), uint32_t(ctx->lr));
      out += line;
      uint32_t sp = ctx->r1.u32;
      for (int depth = 0; depth < 32 && on_stack(sp); ++depth)
      {
        uint32_t back = 0, ret = 0;
        if (!LoadGuest32(sp, back) || !on_stack(back) || back <= sp || !LoadGuest32(back - 8, ret))
        {
          break;
        }
        if (in_code(ret))
        {
          std::snprintf(line, sizeof(line), " %08X", ret);
          out += line;
        }
        sp = back;
      }
      out += '\n';
      if (registers)
      {
        // As the context last stored them: r3-r12 are current, r14-r31 live
        // in host locals of the recompiled code and can be stale.
        const PPCRegister *gpr[] = {&ctx->r0,  &ctx->r1,  &ctx->r2,  &ctx->r3,  &ctx->r4,  &ctx->r5,  &ctx->r6,
                                    &ctx->r7,  &ctx->r8,  &ctx->r9,  &ctx->r10, &ctx->r11, &ctx->r12, &ctx->r13,
                                    &ctx->r14, &ctx->r15, &ctx->r16, &ctx->r17, &ctx->r18, &ctx->r19, &ctx->r20,
                                    &ctx->r21, &ctx->r22, &ctx->r23, &ctx->r24, &ctx->r25, &ctx->r26, &ctx->r27,
                                    &ctx->r28, &ctx->r29, &ctx->r30, &ctx->r31};
        out += "   ";
        for (int i = 0; i < 32; ++i)
        {
          std::snprintf(line, sizeof(line), " r%d=%08X", i, gpr[i]->u32);
          out += line;
        }
        out += "\n    stack:";
        for (uint32_t i = 0; i < 48; ++i)
        {
          uint32_t word = 0;
          if (!LoadGuest32(ctx->r1.u32 + i * 4, word))
          {
            break;
          }
          std::snprintf(line, sizeof(line), " %08X", word);
          out += line;
        }
        out += '\n';
      }
    }
    return out;
  }
} // namespace ao2::hang
