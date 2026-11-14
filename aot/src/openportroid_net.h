// openportroid_net.h - Online play over the OpenPortroid server: this
// player's virtual address and the bridge that carries the game's UDP to the
// others, straight through NAT or through the server (openportroid_net.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ao2::opnet
{
  // Signs in to the server's UDP port with this key (32 hex digits; empty or
  // invalid: a random one) and starts the bridge's thread. Call once
  // (ea::Start, ao2_net_mode = openportroid).
  void Start(const std::string &server, uint16_t port, const std::string &key_hex);

  // Whether Start ran (this game plays over the OpenPortroid server).
  bool Active();

  // The key this game signed in with (32 hex digits; the lobby tunnel sends
  // it so the lobby server shows this player at its virtual address).
  std::string KeyHex();

  // This player's virtual address (network order), 0 until signed in. Waits
  // up to `wait_ms` for the server's answer.
  uint32_t SelfIp(int wait_ms = 0);

  // The players this game knows (introduced, or whose packets arrived): their
  // virtual addresses, for the presence service's announcements.
  std::vector<uint32_t> PeerAddresses();

  // Starts talking to the player at this virtual address: its sockets bound
  // here, the server asked to introduce us.
  void Connect(uint32_t vip);

  // For the players window: how this player is reached ("direct", "relay";
  // "" for an address that is not an OpenPortroid player's).
  std::string PathTo(uint32_t vip);
} // namespace ao2::opnet
