// versus_rules.h - The host's rules of a versus match (versus_rules.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace ao2::versus::rules
{
  // Every machine, the map load of a match: the respawn price back to the
  // game's (it is a class default: it outlives the match).
  void Reset(uint8_t *base);

  // Every machine, once per lockstep frame of a versus match (in the
  // replay, after the frame's first record): the rules the match's
  // settings say (versus_bots.h VersusSettings) applied to the game.
  void Frame(PPCContext &ctx, uint8_t *base);
} // namespace ao2::versus::rules
