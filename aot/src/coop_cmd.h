// coop_cmd.h - Debug commands for both online co-op machines (see coop_cmd.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <rex/ppc.h>

namespace ao2::coop::cmd
{
  // Any thread: a command both machines run on the same lockstep frame -
  // "checkpoint <name> <difficulty>". Dropped when the network game ends
  // before it went out.
  void Send(std::string line);

  // True in a network game (or the single-player test of this path,
  // ao2_coop_test_local, while its lockstep runs): the menu's actions go
  // through Send.
  bool Routed(const uint8_t *base);
  // True while commands can go (the lockstep runs).
  bool Ready(const uint8_t *base);

  // Game thread, engine pad polling: puts what is queued into this player's
  // frame record; also sends the god mode wish (ao2_god_mode).
  void Poll(PPCContext &ctx, uint8_t *base);

  // A replayed frame record (sub_82A0EEC8, both machines, same frame):
  // BeforeReplay takes the command entries out of it (neither the game nor
  // the cutscene skip sees them), AfterReplay puts the record back as it was
  // and runs the commands completed in it.
  struct Replay
  {
    uint32_t record = 0;
    uint32_t controller_id = 0;
    bool stripped = false;
    uint32_t count = 0;
    uint8_t entries[15 * 12] = {};
    std::vector<std::pair<std::string, bool>> commands; // text, sent by this machine
  };
  void BeforeReplay(uint8_t *base, uint32_t record, uint32_t controller_id, Replay &replay);
  void AfterReplay(PPCContext &ctx, uint8_t *base, Replay &replay);
} // namespace ao2::coop::cmd
