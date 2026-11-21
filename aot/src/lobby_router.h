// lobby_router.h - Where the game's EA lobby connections go.
//
// The game finds EA's servers at net_title_server_ip, which is set once at
// startup to this router's own address (127.0.0.1, or net_bind_ip) and never
// changes. The router listens there on TCP 18480 (FESL), 18485 (Theater) and
// 13505 (Messenger) and decides for every connection which lobby server it
// goes to:
//
// - Own: the server of this process (ea_server.cpp), handed the socket
//   itself;
// - Direct: a server at an address (a LAN / VPN host, a standalone lobby
//   server, a Steam player's virtual address), carried over TCP;
// - Tunnel: the OpenPortroid server, through its HTTP tunnel.
//
// "Home" is where the game's logins go by themselves (its own server, the
// host of ao2_coop_host, the OpenPortroid server). An accepted invite makes
// a visit: the next login goes to the inviter's server. The game logs in to
// EA anew every time it goes online (FESL, then Theater and Messenger) and
// out when it leaves, so a login is the unit: the visit holds for the login
// it started, and a later login stays on it only while `follow` says so (the
// inviter still hosts: a versus party following its leader into a match).
// Theater and Messenger go where the latest login of the game went: the FESL
// Hello of a Direct server is rewritten to name the router, so every lobby
// connection of the game passes here (logs\net\lobby_router.log).
//
// Connections from anywhere else (another game on this PC joining this one)
// go to this process's own server.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ao2::router
{
  enum class Kind
  {
    kNone,   // no server: the connection is closed
    kOwn,    // this process's server
    kDirect, // a server at `ip`
    kTunnel, // the OpenPortroid server
  };

  struct Target
  {
    Kind kind = Kind::kNone;
    uint32_t ip = 0; // network order, kDirect only
    bool operator==(const Target &o) const { return kind == o.kind && ip == o.ip; }
  };

  // Listens at `local` (network order) and sends the game's logins `home`.
  // Sets net_title_server_ip. False when a port is taken.
  bool Start(uint32_t local, Target home);
  bool Started();

  // Relays the local player's Theater stream, applying the chosen visibility.
  void ForwardTheater(uintptr_t game, uintptr_t server);

  // The router's address (net_title_server_ip), 0 before Start.
  uint32_t LocalAddress();

  // An accepted invite to a game on the server at `ip` (network order). The
  // next login goes there; a later one while `follow()` is true (asked
  // without the router's lock held). A server that is home ends any visit.
  void Visit(uint32_t ip, const std::string &who, std::function<bool()> follow);
  // Logins go home again.
  void Home(const char *why);

  // The server the game's lobby is on: the visit's, else home's (the
  // router's own address for this process's server and the tunnel). What an
  // invite names, where a GAMEQ goes. 0: none.
  uint32_t CurrentServer();
  // The visited server, 0 without a visit.
  uint32_t VisitIp();
} // namespace ao2::router
