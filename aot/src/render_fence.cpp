// render_fence.cpp - Lost wake-up in the game thread's wait for the
// rendering thread.
//
// Every frame the game thread (sub_82547960) begins a render command fence
// (sub_825F4DD0: counter [0x831E5C10] += 1, the command decrements it) and
// waits until at most one frame is in flight: FRenderCommandFence::Wait
// (sub_825FC128, r3 = fence, r4 = count) loops "while counter > count:
// event->Wait(INFINITE)". The rendering thread wakes it with PulseEvent,
// which only releases threads already waiting: when the command runs between
// the game thread's check and its wait, the pulse is lost and the game hangs
// for good (counter 0, rendering thread idle, network still up - seen after
// a co-op cutscene at 120 FPS). The wait inside the fence loop is bounded
// here, so the loop rechecks the counter; nothing else changes.
#include <windows.h>

#include <chrono>

#include <rex/ppc.h>

#include "perf_waits.h"

namespace
{
  constexpr uint32_t kRecheckMs = 1;
  thread_local bool t_in_fence_wait = false;
} // namespace

extern "C" REX_FUNC(__imp__sub_825FC128);
extern "C" REX_FUNC(sub_825FC128)
{
  bool timed = ao2::perf::WaitsEnabled();
  auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  t_in_fence_wait = true;
  __imp__sub_825FC128(ctx, base);
  t_in_fence_wait = false;
  if (timed)
  {
    ao2::perf::AddRenderFenceWait(start);
  }
}

// FEvent::Wait(ms) (event vtable +20, r3 = event, r4 = ms, -1 = infinite).
extern "C" REX_FUNC(__imp__sub_82461D50);
extern "C" REX_FUNC(sub_82461D50)
{
  if (t_in_fence_wait && ctx.r4.s32 == -1)
  {
    ctx.r4.u64 = kRecheckMs;
  }
  __imp__sub_82461D50(ctx, base);
}
