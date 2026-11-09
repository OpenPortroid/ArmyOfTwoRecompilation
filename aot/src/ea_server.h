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
  // port is taken (another server running). Without `listen` no ports of its
  // own: the OpenPortroid server, whose players all come through the tunnel
  // (and where the 40th Day's server has 18485 / 13505).
  bool StartServer(bool log_to_console, bool listen = true);
  void LogMessage(const std::string &text);

  // The server's address on the internet ("1.2.3.4" or a host name), for a
  // server behind NAT: players outside the LAN are sent it instead of the
  // address their connection arrived at. False when it does not resolve.
  bool SetPublicAddress(const std::string &address);

  // An IPv4 address, or a host name looked up; 0 when it does not resolve.
  uint32_t ResolveIp(const std::string &address);

  // The OpenPortroid server's end of the lobby tunnel (ea_server.cpp,
  // "Tunnel"): nginx passes /aot1/ here, on 127.0.0.1:port. False when it is
  // taken.
  bool StartTunnelBridge(uint16_t port);

  // The standalone server on a LAN: answers the games' GAMEQ on the presence
  // port (UDP 18491; the OpenPortroid server answers it over the tunnel).
  void StartInviteQueries();

  // ao2_net_mode: "steam" (LAN / VPN, and Steam when it runs), "lan" (no
  // Steam) or "openportroid".
  std::string NetMode();

  // The game's end (ao2_net_mode = openportroid): whether the lobby goes
  // through the tunnel, and the server's GAME answer (coop_invite.cpp's
  // presence packet) for the game the player `name` hosts there.
  bool TunnelActive();
  bool TunnelGameQuery(const std::string &name, std::string *packet);
  // Finds a player by the name claimed on the server: "online" (its virtual
  // address in *vip), "offline", "unknown" (no such name) or "error".
  std::string TunnelFindPlayer(const std::string &name, uint32_t *vip);

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
    int max_players = 2;    // MAX-PLAYERS (4: versus)
    std::string map;        // B-U-Map (versus M01_map_world..., "Any"; co-op its level)
    int game_type = -1;     // B-U-GameType (versus: 0 Warzone, 1 Bounties, 2 Extraction)
    int difficulty = -1;    // B-U-Difficulty (co-op: 0 Recruit, 1 Contractor, 2 Professional)
    std::string mode;       // B-U-Mode (what a quick match filters on: FILTER-ATTR-U-Mode)
  };
  bool FindHostedGame(const std::string &host_name, HostedGame *out);

  // A public quick match (GLST with FILTER-NOT-PRIVATE=1) of this machine's
  // own game that found no game on this server: `hook(mode)` may find one
  // elsewhere (steam_net.cpp: the Steam lobby with the lowest ping) and
  // have the game join it through an invite. It blocks until then: true
  // when the game was sent there (the list is not answered: the game goes
  // on through the invite), false to answer "no games" (the game hosts).
  using QuickMatchHook = bool (*)(const std::string &mode);
  void SetQuickMatchHook(QuickMatchHook hook);
} // namespace ao2::ea
