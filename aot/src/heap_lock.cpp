// heap_lock.cpp - The game's heap lock as a host lock.
//
// GMalloc is UE3's thread-safe proxy (vtable 0x8206E6A8, built by
// sub_8254A1C8): +4 the real allocator, +12 a critical section. Each of its
// methods (slots +0..+76) enters the section, calls the same slot of the real
// allocator with the same arguments, leaves the section and returns its r3;
// Free (+12) and +20 do nothing for a null pointer. Slot +72 takes no lock,
// +60 / +68 return 0.
//
// The section is made without a spin count, and the runtime's critical
// section hands the lock over: a thread that finds it held counts itself in
// and sleeps on the section's event, and the release wakes it and leaves the
// lock to it. Until the sleeper is running again, every other thread that
// wants the heap counts itself in and sleeps too (a lock convoy). The game
// thread and the rendering thread allocate and free all the time, the
// particle data above all (sub_825FF2F8 builds every system's dynamic data
// each tick, the rendering thread frees the last one): on China's river 27%
// of the game thread went into the heap's methods, nearly all of it asleep in
// RtlEnterCriticalSection (fps-findings.md, section 9).
//
// Here the methods take a host SRW lock instead: the thread that comes when
// the lock is free takes it, a release does not wait for a sleeper to wake
// up. The heap's calls stay mutually exclusive, in whatever order the threads
// come, as before; nothing in the game state changes, only how long a thread
// waits for the heap. The lock is recursive, as the section was.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "heap_lock.h"

REXCVAR_DEFINE_BOOL(ao2_heap_host_lock, true, "Debug",
                    "The game's heap takes a host lock instead of its critical section (read at start)");

namespace
{
  uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    uint32_t v;
    std::memcpy(&v, base + address, 4);
    return __builtin_bswap32(v);
  }

  struct HeapLock
  {
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<DWORD> owner{0};
    uint32_t depth = 0;

    void Enter()
    {
      const DWORD self = GetCurrentThreadId();
      if (owner.load(std::memory_order_relaxed) == self)
      {
        ++depth;
        return;
      }
      AcquireSRWLockExclusive(&lock);
      owner.store(self, std::memory_order_relaxed);
      depth = 1;
    }

    void Leave()
    {
      if (--depth == 0)
      {
        owner.store(0, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&lock);
      }
    }
  };
  HeapLock g_lock;

  // Once: a thread must not leave the host lock after entering the section, or the other way round.
  bool On()
  {
    static const bool on = REXCVAR_GET(ao2_heap_host_lock);
    return on;
  }

} // namespace

void ao2::heap::Method(PPCContext &ctx, uint8_t *base, uint32_t slot, bool skip_null, PPCFunc *original)
{
  if (!On())
  {
    original(ctx, base);
    return;
  }
  if (skip_null && ctx.r4.u32 == 0)
  {
    return;
  }
  const uint32_t inner = Load32(base, ctx.r3.u32 + 4);
  g_lock.Enter();
  ctx.r3.u64 = inner;
  rex::runtime::ResolveIndirectFunction(Load32(base, Load32(base, inner) + slot))(ctx, base);
  g_lock.Leave();
}

// Realloc (+8) and Free (+12) are hooked in interp_objects.cpp, which calls Method too.
#define AO2_HEAP_METHOD(addr, slot, skip_null)                                                                   extern "C" REX_FUNC(__imp__sub_##addr);                                                                         extern "C" REX_FUNC(sub_##addr)                                                                                 {                                                                                                                 ao2::heap::Method(ctx, base, slot, skip_null, __imp__sub_##addr);                                             }

AO2_HEAP_METHOD(8254A820, 0, false)
AO2_HEAP_METHOD(8254A288, 4, false) // Malloc
AO2_HEAP_METHOD(8254A3B8, 16, false)
AO2_HEAP_METHOD(8254A418, 20, true)
AO2_HEAP_METHOD(8254A4E0, 24, false)
AO2_HEAP_METHOD(8254A538, 28, false)
AO2_HEAP_METHOD(8254A598, 32, false)
AO2_HEAP_METHOD(8254A5F8, 36, false)
AO2_HEAP_METHOD(8254A658, 40, false)
AO2_HEAP_METHOD(8254A6A8, 44, false)
AO2_HEAP_METHOD(8254A708, 48, false)
AO2_HEAP_METHOD(8254A760, 52, false)
AO2_HEAP_METHOD(8254A7C0, 56, false)
AO2_HEAP_METHOD(8254A488, 64, false)
AO2_HEAP_METHOD(8254A220, 76, false)
