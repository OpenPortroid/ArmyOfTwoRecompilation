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

  // What the bots play by (the defaults are the cvars' and the F1 menu's
  // Normal), and the match's rules (versus_rules.cpp; 0: the game's).
  struct VersusSettings
  {
    bool on = true;        // ao2_versus_bots
    double miss = 2.5;     // ao2_versus_bots_miss (degrees)
    int reaction = 20;     // ao2_versus_bots_reaction (lockstep frames)
    double turn = 6.0;     // ao2_versus_bots_turn (degrees per frame)
    double range = 4000.0; // ao2_versus_bots_range
    bool buy = true;       // ao2_versus_bots_buy
    double share = 0.5;    // ao2_versus_bots_buy_share
    bool ammo = true;      // ao2_versus_bots_infinite_ammo
    int32_t minutes = 0;   // ao2_versus_round_minutes
    int32_t goal = 0;      // ao2_versus_cash_goal
    int32_t ff = 0;        // ao2_versus_friendly_fire: 1 off, 2 on
    int32_t cost = 0;      // ao2_versus_respawn_cost
    int32_t step = 0;      // ao2_versus_respawn_step
  };

  // Game thread, the lockstep commands' poll (coop_cmd.cpp) while they can
  // go: in a versus match, this machine's bot settings go out once, as the
  // match began.
  void PollSettings(const uint8_t *base);
  // Game thread, a replayed "vbots <args>" of `controller_id` (every
  // machine, the same frame); `mine`: sent by this machine.
  void ReceiveSettings(uint32_t controller_id, const char *args, bool mine);

  // Any thread: whose settings the bots of this match play by (from: the
  // controller id, -1 = none came yet).
  struct MatchSettingsInfo
  {
    int from = -1;
    bool host = false; // the sender said it hosts
    bool mine = false; // they are this machine's
    VersusSettings settings;
  };
  MatchSettingsInfo MatchSettings();
  // Game thread: what the match plays by now (the defaults until any came).
  const VersusSettings &PlayedSettings();
} // namespace ao2::versus
