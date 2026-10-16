// debug_tools.h - Diagnostic and debugging tools
#pragma once

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include "game_constants.h"

namespace debug_tools
{
  // Stub sweep: registers a stub function for all unregistered addresses
  // and logs when they are called
  static void PerformStubSweep()
  {
    auto *rt = rex::Runtime::instance();
    auto *fd = rt ? rt->function_dispatcher() : nullptr;
    uint8_t *base = rt ? rt->virtual_membase() : nullptr;
    if (!fd || !base)
    {
      return;
    }

    static FILE *stub_log = std::fopen("logs/stub_sweep.txt", "w");
    static std::mutex stub_mutex;
    static std::unordered_map<uint32_t, uint32_t> stub_hits;

    static PPCFunc *stub = [](PPCContext &ctx, uint8_t *) noexcept
    {
      uint32_t addr = ctx.ctr.u32;
      uint32_t lr = ctx.lr;
      std::lock_guard<std::mutex> lock(stub_mutex);
      uint32_t &count = stub_hits[addr];
      if (count == 0 && stub_log)
      {
        std::fprintf(stub_log,
                     "[stub] addr=0x%08X LR=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n",
                     addr, lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
        std::fflush(stub_log);
      }
      ++count;
    };

    uint32_t stubbed = 0;
    for (uint32_t addr = GameConstants::kCodeBase; addr < GameConstants::kCodeEnd; addr += 4)
    {
      if (!fd->GetFunction(addr))
      {
        fd->SetFunction(addr, stub);
        ++stubbed;
      }
    }
    if (stub_log)
    {
      std::fprintf(stub_log, "=== stub sweep: scanned %u addresses, stubbed %u ===\n",
                   (GameConstants::kCodeEnd - GameConstants::kCodeBase) / 4, stubbed);
      std::fflush(stub_log);
    }
  }

  // Missing function scan: logs all addresses without registered functions
  static void PerformMissingFunctionScan()
  {
    auto *rt = rex::Runtime::instance();
    auto *fd = rt ? rt->function_dispatcher() : nullptr;

    if (!fd)
    {
      return;
    }

    FILE *log = std::fopen("logs/missed_functions.txt", "w");
    if (!log)
    {
      return;
    }

    uint32_t missing = 0;
    uint32_t registered = 0;

    for (uint32_t addr = GameConstants::kCodeBase; addr < GameConstants::kCodeEnd; addr += 4)
    {
      if (fd->GetFunction(addr))
      {
        ++registered;
      }
      else
      {
        ++missing;
        std::fprintf(log, "[missed] addr=0x%08X\n", addr);
      }
    }

    std::fprintf(log,
                 "=== scan: scanned=%u registered=%u missed=%u ===\n",
                 (GameConstants::kCodeEnd - GameConstants::kCodeBase) / 4,
                 registered,
                 missing);

    std::fclose(log);
  }
} // namespace debug_tools
