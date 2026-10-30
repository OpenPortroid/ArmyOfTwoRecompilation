// versus_bots.h - Bots in the empty places of a versus match (versus_bots.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace ao2::versus
{
  // The level's empty places (bit = sim controller id 0-3), set on every
  // machine when the map loads (versus_lobby.cpp); 0 outside versus.
  void SetEmptyPlaces(uint32_t mask);

  // A player's frame record the lockstep replays (sub_82A0EEC8, r3 =
  // record): before it is replayed, and after (the bots' records are
  // replayed after the first record of a frame).
  void NoteReplayedRecord(const uint8_t *base, uint32_t record);
  void AfterReplayedRecord(PPCContext &ctx, uint8_t *base);
} // namespace ao2::versus
