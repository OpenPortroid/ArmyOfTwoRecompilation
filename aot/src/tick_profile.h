// tick_profile.h - Where a long engine tick went (tick_profile.cpp).
#pragma once

namespace ao2::tickprof
{
  // Around GEngine->Tick of a simulation step, on the game thread.
  void Begin();
  void End(double tick_ms);

  // Times from hooks defined elsewhere (game thread, inside a tick only).
  bool On();
  void Add(int slot, const char *name, const char *callers, double ms);
  constexpr int kPhysicsKick = 66;     // sub_8286A290 with the port's wait for the worker (physx_worker.cpp)
  constexpr int kPhysicsKickGame = 67; // the game's own kick in it: its wait for the worker's "done"
} // namespace ao2::tickprof
