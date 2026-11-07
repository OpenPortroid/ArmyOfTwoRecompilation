// coop_link.h - A side channel to the online co-op partner (coop_link.cpp).
#pragma once

#include <cstdint>

namespace ao2::coop::link
{
  // The link's UDP port; a second game on one PC takes one of the next few.
  // The Steam bridge (steam_net.cpp) carries it like the game's own ports.
  inline constexpr uint16_t kPort = 1002;

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

  // --- Connection quality (coop_net_status.cpp) ---
  // The link's own round trip to the partner in ms (its messages go the way
  // the game's do: over the LAN / VPN / Steam relay); -1 before it is known.
  int32_t PingMs();
  // Milliseconds since anything came from the partner; -1 if nothing ever did.
  int64_t PartnerSilentMs();
  // The game's partner address (IPv4, network order; 0 before it sent any).
  uint32_t GamePeer();
  // This machine's input delay margin wish (ms on top of the latency) from
  // lockstep frame `from_frame` on, and the partner's last one (false before
  // any arrived). Both machines use the larger one (coop_input_delay.cpp).
  void ProposeExtraMs(int32_t extra_ms, uint32_t from_frame);
  bool PartnerExtraMs(int32_t *extra_ms, uint32_t *from_frame);
  // Lockstep frames this machine waited for the partner in the last second,
  // sent along for the partner's overlay; the partner's last count (-1: none).
  void SetLocalWaits(int32_t waits);
  int32_t PartnerWaits();
  // A new network session: what was measured for the last partner is gone.
  void ResetQuality();
  // This game's network session ended: tells the partner it was not a drop
  // (a game that crashed or lost its connection sends nothing).
  void SendBye();
  // The partner's game said its session ended (since ResetQuality).
  bool PartnerSaidBye();
} // namespace ao2::coop::link
