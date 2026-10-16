// cheats.h - Debug cheats (see cheats.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace ao2::cheats
{
  // Game thread, once per frame (engine pad polling).
  void Tick(PPCContext &ctx, uint8_t *base);

  // Online co-op, both machines on the same lockstep frame (coop_cmd.cpp):
  // god mode of the player with this controller id on or off.
  void SetPlayerGod(PPCContext &ctx, uint8_t *base, uint32_t controller_id, bool on);
  // At each replay of that player's frame record: keeps the flag set (a new
  // controller after a level load gets it on the same frame on both).
  void OnReplayedRecord(PPCContext &ctx, uint8_t *base, uint32_t controller_id);
} // namespace ao2::cheats
