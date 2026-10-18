// aim_assist.h - Turning the game's aim assist off (see aim_assist.cpp).
#pragma once

#include <cstdint>

namespace ao2::aim_assist
{
  // Online co-op, both machines on the same lockstep frame (coop_cmd.cpp):
  // the aim assist of the player with this controller id on or off.
  void SetPlayer(uint32_t controller_id, bool on);
  // The network game is over: every player back to the default (off).
  void Reset();
} // namespace ao2::aim_assist
