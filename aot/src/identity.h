// identity.h - The player's identity: one ID for every way of playing online.
//
// A P-256 key pair (Windows CNG, bcrypt) made on the first start and kept in
// settings\identity.key (ao2_identity_file for a second copy on one PC). From
// its public key come:
//
// - the Player ID: SHA-256 of the public key (X || Y), 16 bytes, as 32 hex
//   digits; the same over the LAN, Steam and the OpenPortroid server;
// - the friend code: its first 40 bits in Crockford base 32, "XXXX-XXXX",
//   what players tell each other;
// - the XUID the game is online with (0x0009... from the Player ID, net_xuid
//   for the runtime): stable, where the runtime's own one changed with the
//   PC's address.
//
// The presence packets (coop_invite.cpp) carry the public key and are signed
// with the private one (ECDSA P-256 over SHA-256), so a name, an invite or a
// kick can no longer be sent in another player's name.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao2::identity
{
  constexpr size_t kPublicKeyBytes = 64; // X || Y
  constexpr size_t kSignatureBytes = 64; // r || s
  using PublicKey = std::array<uint8_t, kPublicKeyBytes>;
  using Signature = std::array<uint8_t, kSignatureBytes>;

  // Loads (or makes and saves) this player's key pair and sets net_xuid.
  // Call once at startup, before the game goes online. False when no key
  // could be had (the game then plays unsigned, as older builds).
  bool Start();
  bool Ready();

  const PublicKey &OwnPublicKey();
  const std::string &OwnPlayerId();   // 32 hex digits
  const std::string &OwnFriendCode(); // "XXXX-XXXX"
  uint64_t OwnXuid();
  // Stable across launches with the same identity key.
  const std::string &DefaultGamertag();

  Signature Sign(std::string_view data);
  // Whether `sig` is `key`'s signature of `data` (keys seen are kept).
  bool Verify(const PublicKey &key, std::string_view data, const Signature &sig);

  std::string PlayerIdOf(const PublicKey &key);
  std::string FriendCodeOf(const std::string &player_id);
  uint64_t XuidOf(const std::string &player_id);
  // A typed friend code made comparable ("k7qm 2xra", "K7QM-2XRA", O for 0,
  // I / L for 1); empty when it is no code.
  std::string NormalizeFriendCode(std::string_view text);
} // namespace ao2::identity
