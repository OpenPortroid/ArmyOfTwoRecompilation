// decouple.h - Rendering decoupled from the simulation (decouple.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace ao2::decouple
{
  // Called by the frame sync tick hook (frame_time.cpp) once per game-thread
  // frame, before the tick. True: a render-only frame - skip the tick, no
  // simulation step. False: a simulation step; *sim_dt is its length in
  // seconds when decoupling is on (fixed online, the same on both machines;
  // outside a network game a late step is up to 4 periods long), else 0.
  bool BeginFrame(PPCContext &ctx, uint8_t *base, double *sim_dt);

  // Length of a simulation step in ms while decoupling is on, else 0.
  double SimPeriodMs();

  // Set around the frame sync tick (frame_time.cpp): its wait for the co-op
  // partner then draws.
  void SetInSyncTick(bool inside);

  // After the frame sync tick of a simulation step (frame_time.cpp), with how
  // long it took: a step that waited for the co-op partner moves the
  // schedule of the next ones (decouple.cpp, ao2_coop_step_slip).
  void AfterSyncTick(uint8_t *base, double tick_ms);

  // Interpolation (interp_camera.cpp): whether decoupling is on, the number
  // of the last world tick (changes when a simulation step has run) and how
  // far the frame being drawn is past it, in steps (0..1).
  bool Active();
  uint32_t WorldStep();

  // Inside a draw-only frame or a draw while waiting for the partner (game
  // thread).
  bool InExtraDraw();
  double Alpha();
} // namespace ao2::decouple
