// perf_waits.h - Wait timing of the game threads (perf_waits.cpp).
#pragma once

#include <chrono>

namespace ao2::perf
{
  // --ao2_perf_waits: the timers below are only called when this is true.
  bool WaitsEnabled();
  // A wait that began at `start` and just ended, on the calling thread.
  void AddOcclusionWait(std::chrono::steady_clock::time_point start);
  void AddRenderFenceWait(std::chrono::steady_clock::time_point start);
} // namespace ao2::perf
