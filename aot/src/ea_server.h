// ea_server.h - Built-in EA online server for Army of Two co-op.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ao2::ea
{
  // Applies the co-op cvars (where the game finds the EA server) and, with
  // --ao2_coop_server=true, starts the built-in FESL / Theater / Messenger
  // server in this process. Call once at startup, before the game goes online.
  void Start();

  // The server alone (also used by the standalone ao2_lobby_server.exe):
  // listens on TCP 18480, 18485 and 13505 on all interfaces. False when a
  // port is taken (another server running).
  bool StartServer(bool log_to_console);
  void LogMessage(const std::string &text);

  // IPv4 addresses of this computer's active adapters (to tell the partner).
  struct LocalAddress
  {
    std::string ip;
    std::string adapter;
    bool radmin = false; // Radmin VPN adapter / 26.x.x.x
  };
  std::vector<LocalAddress> LocalAddresses();

  // The lobby game hosted by the player `host_name` on this process's server
  // (invites, coop_invite.cpp): its Xbox Live session id (CGAM XB360SESS),
  // the host's XNADDR (its own EGAM R-XNADDR, 36 bytes) and XUID. Empty when
  // this process runs no server or that player hosts nothing.
  struct HostedGame
  {
    uint32_t gid = 0;
    uint64_t session_id = 0;
    std::vector<uint8_t> xnaddr;
    uint64_t host_xuid = 0;
    bool is_private = false;
    int players = 0;
    bool playgroup = false; // TYPE=P: a versus party (Custom Match), joined as a playgroup
  };
  bool FindHostedGame(const std::string &host_name, HostedGame *out);
} // namespace ao2::ea
