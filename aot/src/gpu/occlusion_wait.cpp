// occlusion_wait.cpp - Occlusion query waits that sleep instead of spin.
//
// The engine (Unreal Engine 3, EA's wrapper sub_82AA14D0: r3 = query,
// r4 = result, r5 = wait) polls Direct3D's Query::GetData (sub_82B10140)
// in a loop until the GPU has counted the query. At 60 FPS the GPU thread
// waits for the vertical blank before it gets to the frame's queries, so the
// rendering thread spun there ~7 ms of every frame (a whole core busy). The
// loop is the same here, but between polls the thread sleeps until the
// renderer publishes results (OcclusionResultGeneration), at most 2 ms.
#include <windows.h>

#include <rex/ppc.h>

#include "native/renderer.h"
#include "../perf_waits.h"

namespace
{
  constexpr uint32_t kQueryGetData = 0x82B10140; // (query, data, size, flags) -> HRESULT
  constexpr DWORD kMaxSleepMs = 2;
} // namespace

extern "C" REX_FUNC(__imp__sub_82AA14D0);
extern "C" REX_FUNC(sub_82AA14D0)
{
  uint32_t query = ctx.r3.u32, data = ctx.r4.u32, wait = ctx.r5.u32;
  auto get_data = rex::runtime::ResolveIndirectFunction(kQueryGetData);
  volatile uint32_t &generation = ao2::gpu::OcclusionResultGeneration();
  // Start of the wait for the GPU (first "not counted yet"), for --ao2_perf_waits.
  std::chrono::steady_clock::time_point wait_start{};
  for (;;)
  {
    uint32_t seen = generation;
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
      if (!wait)
      {
        ctx.r3.u64 = 0;
        return;
      }
      if (wait_start == std::chrono::steady_clock::time_point{} && ao2::perf::WaitsEnabled())
      {
        wait_start = std::chrono::steady_clock::now();
      }
      WaitOnAddress(const_cast<uint32_t *>(&generation), &seen, sizeof(seen), kMaxSleepMs);
      continue;
    }
    if (hr < 0)
    {
      continue;
    }
    if (wait_start != std::chrono::steady_clock::time_point{})
    {
      ao2::perf::AddOcclusionWait(wait_start);
    }
    ctx.r3.u64 = 1;
    return;
  }
}
