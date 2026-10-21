// occlusion_wait.cpp - Occlusion query waits.
//
// The engine (Unreal Engine 3, EA's wrapper sub_82AA14D0: r3 = query,
// r4 = result, r5 = wait) polls Direct3D's Query::GetData (sub_82B10140)
// in a loop until the GPU has counted the query. The rendering thread reads
// each primitive's query of the previous frame while it builds the next one
// (the caller in sub_8262B3C8 sets the primitive's occluded bit from it), so
// on the console the GPU was always done by then. Here the GPU command
// thread and the host GPU run behind the rendering thread, and the waits
// (2-3 a frame, ~50% of the rendering thread's time at 200 FPS) put the
// three threads one after another instead of side by side.
//
// ao2_occlusion_wait:
//   false (default) - a query the GPU has not counted yet reports visible:
//       the primitive is drawn, occlusion culls only what was counted in
//       time. Iraq 05_00 at 1440p: 204 -> 230 FPS on average, the slowest
//       second 133 -> 210 (with PGO). Answering with the query's previous
//       result instead hid primitives that had come into view (up to ~20 a
//       second, models blinking).
//   true - wait like the console, sleeping between polls until the renderer
//       publishes results (OcclusionResultGeneration), at most 2 ms.
// The menus (ao2::menumouse::MenuOpen) always wait: without the wait the
// shop's board and the weapon on it (a scene capture drawn only while the
// board is) were taken for hidden in some frames and the world behind them
// showed through - the shop flickered, and at 30 FPS showed no weapon at all
// (issue #26); the contacts' globe flickered too. The game itself keeps the
// frame rate.
// D3D then issues queries again that were not read: renderer.cpp
// (RendererMemWrite) drops the old issue's counts.
#include <windows.h>

#include <chrono>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "native/renderer.h"
#include "../menu_mouse.h"
#include "../perf_waits.h"

REXCVAR_DEFINE_BOOL(ao2_occlusion_wait, false, "GPU",
                    "Wait for occlusion query results like the console (slower); otherwise a query the GPU has "
                    "not counted yet reports visible");

namespace
{
  constexpr uint32_t kQueryGetData = 0x82B10140; // (query, data, size, flags) -> HRESULT
  constexpr DWORD kMaxSleepMs = 2;

  // GetData without waiting: true = done (result in *data), false = not counted yet.
  bool Poll(PPCContext &ctx, uint8_t *base, uint32_t query, uint32_t data)
  {
    static auto get_data = rex::runtime::ResolveIndirectFunction(kQueryGetData);
    for (;;)
    {
      // GetData keeps the non-volatile registers (PPC ABI) like any callee;
      // only the link register is put back (no copy of the whole context per poll).
      uint64_t lr = ctx.lr;
      ctx.r3.u64 = query;
      ctx.r4.u64 = data;
      ctx.r5.u64 = 4;
      ctx.r6.u64 = 1;
      get_data(ctx, base);
      int32_t hr = ctx.r3.s32;
      ctx.lr = lr;
      if (hr == 1) // S_FALSE: not counted yet
      {
        return false;
      }
      if (hr >= 0)
      {
        return true;
      }
    }
  }
} // namespace

namespace ao2::gpu
{
  bool OcclusionWaitOn()
  {
    return REXCVAR_GET(ao2_occlusion_wait) || ao2::menumouse::MenuOpen();
  }
} // namespace ao2::gpu

extern "C" REX_FUNC(__imp__sub_82AA14D0);
extern "C" REX_FUNC(sub_82AA14D0)
{
  uint32_t query = ctx.r3.u32, data = ctx.r4.u32, wait = ctx.r5.u32;
  volatile uint32_t &generation = ao2::gpu::OcclusionResultGeneration();
  uint32_t seen = generation;
  if (Poll(ctx, base, query, data))
  {
    ctx.r3.u64 = 1;
    return;
  }
  if (!wait)
  {
    ctx.r3.u64 = 0;
    return;
  }
  if (!ao2::gpu::OcclusionWaitOn())
  {
    *reinterpret_cast<uint32_t *>(base + data) = __builtin_bswap32(1u); // one sample: visible
    ctx.r3.u64 = 1;
    return;
  }
  // Wait for the GPU, sleeping between polls.
  std::chrono::steady_clock::time_point wait_start{};
  if (ao2::perf::WaitsEnabled())
  {
    wait_start = std::chrono::steady_clock::now();
  }
  for (;;)
  {
    WaitOnAddress(const_cast<uint32_t *>(&generation), &seen, sizeof(seen), kMaxSleepMs);
    seen = generation;
    if (Poll(ctx, base, query, data))
    {
      break;
    }
  }
  if (wait_start != std::chrono::steady_clock::time_point{})
  {
    ao2::perf::AddOcclusionWait(wait_start);
  }
  ctx.r3.u64 = 1;
}
