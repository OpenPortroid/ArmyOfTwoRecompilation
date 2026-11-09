// openportroid.h - The OpenPortroid server's network for Army of Two: what the
// game (openportroid_net.cpp, the lobby tunnel in ea_server.cpp) and the
// server (the ArmyOfTwoOpenPortroid repository: relay_server.cpp, its copy of
// ea_server.cpp) share. The server builds this file as it is, so change it
// here and sync the server (its tools/sync_lobby.sh).
//
// Every player signed in to the server has an address in 127.128.0.0/10 (a
// virtual LAN on each PC's loopback, as Steam play has in 127.64.0.0/10,
// steam_net.h). The game's UDP to such an address (the game on 1000, the link
// on 1002, the presence service on ao2_invite_port) goes to that player
// straight through both routers when NAT punching works, else through the
// server.
//
// The lobby (FESL / Theater / Messenger, TCP) goes over HTTP: each connection
// is a request to http://<server>/aot1/<fesl | theater | messenger> with
// "Upgrade: aot1-tunnel", the player's key (X-AO2-Key) and its own loopback
// address (X-AO2-Local); the server answers 101 and both sides then carry the
// lobby's own bytes. Plain GETs under the same path: gameq, find, nick, claim,
// stats (ea_server.cpp "Tunnel").
//
// Datagrams (UDP, the server's ao2_openportroid_port), a 4-byte head
// "OPN" + type, then (addresses and ports in network order):
//   H  player -> server  key[16], count, count x (ip, port): sign in, every 15 s
//   W  server -> player  virtual ip, public ip, public port: signed in
//   C  player -> server  virtual ip: introduce us
//   P  server -> player  virtual ip, public ip, port, count, count x (ip, port):
//                        a player to punch through to (its public and LAN ends)
//   R  player -> server  to virtual ip, payload: relay this
//   r  server -> player  from virtual ip, payload: relayed
//   K  player -> player  from virtual ip, to virtual ip: punch (answered by k)
//   k  player -> player  from virtual ip, to virtual ip: punch answer
//   D  player -> player  from virtual ip, payload: straight
//   I  anyone -> server  status check (the launcher), answered by i
// A payload is the source port, the destination port, then the datagram.
#pragma once

#include <cstdint>

namespace ao2::opnet
{
  constexpr char kMagic[3] = {'O', 'P', 'N'};
  // The 40th Day's server has 18500 and /ao2/ on the same host.
  constexpr uint16_t kDefaultPort = 18501;
  constexpr char kDefaultServer[] = "aot.bhop.pw";
  constexpr char kTunnelPath[] = "/aot1/";
  constexpr char kTunnelUpgrade[] = "aot1-tunnel";
  constexpr int kKeySize = 16;

  // 127.128.0.0/10 (network order, as in sockaddr_in).
  inline bool IsVirtual(uint32_t ip)
  {
    return (ip & 0xFF) == 127 && ((ip >> 8) & 0xC0) == 0x80;
  }
} // namespace ao2::opnet
