// coop_task_pool.cpp - The engine's worker pool, one task at a time (online lockstep).
//
// UE3's FQueuedThreadPool runs work items on worker threads (the guest threads whose loop is sub_82461DA8,
// FQueuedThread::Run: it waits for its event, runs QueuedWork->DoThreadedWork, clears QueuedWork). What the
// items are is the engine's own business (skeleton / animation updates, cloth, PhysX tasks); the game thread
// goes on with its tick once it has queued them and reads their results later. On the console every task
// finished long before it was read; on a PC the two machines' timing differs, and a result read a few
// microseconds early or late is a few ulps off: every versus desync of 2026-11-07 began with the pose or the
// motion of a character on the frame after it went down (a transient difference in the head bone's offset
// came and went on random frames, long before). With ao2_coop_sync_pool the game thread waits for each item
// it hands to a worker to be finished (FQueuedThread::DoWork, sub_82461F60: r3 = thread, r4 = work; the thread
// clears +20 when it is done), so the tasks run one after another in the order they were queued, on both
// machines alike.
#include <windows.h>

#include <bit>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "coop.h"
#include "log_files.h"

REXCVAR_DEFINE_BOOL(ao2_coop_sync_pool, false, "Online",
                    "Online lockstep: the game thread waits for each task it hands to the engine's worker pool "
                    "(deterministic order and completion; costs the parallelism of those tasks). Same on both PCs");

namespace
{
  bool IsGameThread()
  {
    thread_local int main_thread = -1;
    if (main_thread < 0)
    {
      main_thread = 0;
      PWSTR name = nullptr;
      if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &name)) && name)
      {
        main_thread = std::wcsstr(name, L"Main XThread") ? 1 : 0;
        LocalFree(name);
      }
    }
    return main_thread == 1;
  }

  uint32_t LoadVolatile(const uint8_t *base, uint32_t addr)
  {
    uint32_t v = *reinterpret_cast<const volatile uint32_t *>(base + addr);
    return std::byteswap(v);
  }
} // namespace

extern "C" REX_FUNC(__imp__sub_82461F60);
extern "C" REX_FUNC(sub_82461F60)
{
  uint32_t thread = ctx.r3.u32;
  __imp__sub_82461F60(ctx, base);
  if (!REXCVAR_GET(ao2_coop_sync_pool) || !thread || !IsGameThread() || !ao2::coop::OnlineLockstep(base))
  {
    return;
  }
  using Clock = std::chrono::steady_clock;
  auto start = Clock::now();
  uint32_t spins = 0;
  // QueuedWork (+20) goes back to 0 when the worker has run the item (and any it was handed straight after).
  while (LoadVolatile(base, thread + 20) != 0)
  {
    if ((++spins & 0xFF) == 0)
    {
      if (Clock::now() - start > std::chrono::seconds(2))
      {
        break; // a task that waits for the game thread: do not hang
      }
      Sleep(0);
    }
    else
    {
      YieldProcessor();
    }
  }
  static uint32_t waits = 0;
  static double total_ms = 0.0, max_ms = 0.0;
  double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  total_ms += ms;
  max_ms = ms > max_ms ? ms : max_ms;
  if (++waits % 20000 == 0)
  {
    if (FILE *f = ao2::OpenLogFile("coop_lod.log", "a"))
    {
      std::fprintf(f, "worker pool: %u tasks waited for, %.2f ms on average, %.2f ms at most\n", waits, total_ms / waits,
                   max_ms);
      std::fclose(f);
    }
  }
}
