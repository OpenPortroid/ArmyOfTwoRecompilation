// tick_profile.h - Where a long engine tick went (tick_profile.cpp).
#pragma once

namespace ao2::tickprof
{
  // Around GEngine->Tick of a simulation step, on the game thread.
  void Begin();
  void End(double tick_ms);
} // namespace ao2::tickprof
