// render_idle.cpp - The rendering thread sleeps while it has no commands.
//
// The engine's rendering thread (Unreal Engine 3, loop sub_825F4C20) reads
// the render command ring buffer GRenderCommandBuffer (0x831E8BA8: +8 write
// pointer, +20 read pointer) and, while it is empty, repeats "appSleep(0),
// tick the rendering thread tickables (sub_825F4AF0), check again". On the
// console the yield hands the core to a waiting thread; here it returns at
// once, so the thread kept a whole core busy whatever the frame rate (98% at
// 120 FPS, about half of it in this loop).
//
// The tickables are still ticked, but while the buffer is empty the thread
// then sleeps until the game thread starts a command: every enqueue goes
// through the allocation context's constructor (sub_82302DB0, r4 = ring
// buffer), which wakes it. The command is committed a few instructions after
// the constructor returns, so the woken thread polls for it briefly. The wait
// is bounded: the tickables (timers of delayed work) keep running, and a
// command enqueued any other way is found a millisecond late at worst.
#include <windows.h>

#include <atomic>

#include <immintrin.h>

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(ao2_render_idle_sleep, true, "Gameplay",
                    "The rendering thread sleeps while it has no commands instead of spinning on a core");

namespace
{
  constexpr uint32_t kRenderCommandBuffer = 0x831E8BA8;
  constexpr DWORD kMaxSleepMs = 1;
  constexpr uint32_t kCommitPolls = 4000; // ~0.1 ms

  // Enqueues begun (the address the rendering thread waits on).
  std::atomic<uint32_t> g_enqueues{0};
  std::atomic<uint32_t> g_sleeping{0};

  uint32_t Load(uint8_t *base, uint32_t address)
  {
    return __builtin_bswap32(*reinterpret_cast<volatile uint32_t *>(base + address));
  }

  bool Empty(uint8_t *base)
  {
    return Load(base, kRenderCommandBuffer + 20) == Load(base, kRenderCommandBuffer + 8);
  }
} // namespace

// FRingBuffer::AllocationContext constructor (r3 = context, r4 = ring buffer, r5 = size).
extern "C" REX_FUNC(__imp__sub_82302DB0);
extern "C" REX_FUNC(sub_82302DB0)
{
  bool render_commands = ctx.r4.u32 == kRenderCommandBuffer;
  __imp__sub_82302DB0(ctx, base);
  if (render_commands)
  {
    g_enqueues.fetch_add(1);
    if (g_sleeping.load())
    {
      WakeByAddressSingle(&g_enqueues);
    }
  }
}

// TickRenderingTickables: called by the rendering thread's loop only, each
// time it found the command buffer empty.
extern "C" REX_FUNC(__imp__sub_825F4AF0);
extern "C" REX_FUNC(sub_825F4AF0)
{
  __imp__sub_825F4AF0(ctx, base);
  if (!REXCVAR_GET(ao2_render_idle_sleep))
  {
    return;
  }
  uint32_t seen = g_enqueues.load();
  if (!Empty(base))
  {
    return;
  }
  g_sleeping.store(1);
  if (g_enqueues.load() == seen && Empty(base))
  {
    WaitOnAddress(&g_enqueues, &seen, sizeof(seen), kMaxSleepMs);
  }
  g_sleeping.store(0);
  if (g_enqueues.load() != seen)
  {
    // An enqueue has begun: its commit follows.
    for (uint32_t i = 0; i < kCommitPolls && Empty(base); ++i)
    {
      _mm_pause();
    }
  }
}
