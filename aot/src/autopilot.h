// autopilot.h - Debug autopilot: the player runs along a route (see autopilot.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace ao2::autopilot
{
  // Game thread, engine pad polling (once per frame): reads autopilot.path, steers.
  void Tick(PPCContext &ctx, uint8_t *base);

  // Pad input of the autopilot, added to the first pad by the XInputGetState
  // hook (input_script.cpp). False while it is off.
  struct Pad
  {
    int16_t lx, ly, rx, ry;
    uint16_t buttons;
    uint8_t lt, rt;  // triggers
    uint32_t packet; // changes whenever the input does
  };
  bool CurrentPad(Pad &pad);

  // AAO2PlayerControllerNative::UpdateLookingAction (single player, before the
  // game's own code): turns the look rotation towards the aimbot's target.
  void Aim(uint8_t *base, uint32_t controller, float dt);
} // namespace ao2::autopilot
