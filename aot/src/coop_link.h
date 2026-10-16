// coop_link.h - A side channel to the online co-op partner (coop_link.cpp).
#pragma once

#include <cstdint>

namespace ao2::coop::link
{
  // Receives what the partner sent and says hello once a second (any game
  // thread; cheap when there is nothing to do).
  void Pump();

  // Tells the partner to skip the movie playing now.
  void SendSkipMovie();

  // True once for a movie skip the partner asked for in the last moment.
  bool TakeRemoteSkipMovie();

  // Simulation settings both machines must share (decouple.cpp). Each side
  // sends its own with every hello; whose settings both use is decided the
  // same way on both machines (SettingsAreMine).
  struct SimSettings
  {
    bool decouple = false;
    int32_t sim_hz = 0;
    bool server = false; // runs the EA lobby server (ao2_coop_server)
  };
  void SetLocalSettings(const SimSettings &settings);
  // The partner's last settings; false before any arrived.
  bool PartnerSettings(SimSettings *out);
  // Whose settings win: the side running the lobby server, else the lower
  // link id (random per run). True = this machine's.
  bool SettingsAreMine(const SimSettings &mine, const SimSettings &partner);
} // namespace ao2::coop::link
