// console_cmd.h - UE3 console commands from outside the game (see console_cmd.cpp).
#pragma once

#include <cstdint>
#include <string>

#include <rex/ppc.h>

namespace ao2::console
{
  // Game thread, engine pad polling: runs console.txt and the start checkpoint.
  void Poll(PPCContext &ctx, uint8_t *base);
  // Any thread: runs `line` like a console.txt line on the next poll.
  void Queue(std::string line);
  // Game thread: loads a campaign checkpoint (0-2 = Recruit..Professional).
  // `network`: the online co-op URL, for both machines on the same lockstep
  // frame (coop_cmd.cpp).
  bool LoadCheckpoint(PPCContext &ctx, uint8_t *base, const std::string &name, int difficulty, bool network);
} // namespace ao2::console
