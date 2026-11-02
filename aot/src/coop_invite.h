// coop_invite.h - Invites to a co-op game without Xbox Live (see coop_invite.cpp).
#pragma once

#include <cstdint>
#include <string>

#include <imgui.h>

namespace ao2::invite
{
  // Starts the presence service (UDP ao2_invite_port): finds the other
  // players and carries invites. Call once at startup, after ao2::ea::Start.
  void Start();

  // Any thread: opens the players window (find players, invite, answer
  // invites) - what the game's "Invite friend" / friends list buttons open.
  void ShowPlayers();

  // Any thread: there is an invite to answer / accept the newest one.
  bool HasPendingInvite();
  void AcceptPendingInvite();

  // Whether the presence service runs (ao2_invites).
  bool Running();

  // A build hash (another player's, a Steam lobby's) is not this game's.
  bool IsOtherBuild(const std::string &build);

  // Any thread: invites the player at `ip` (network order; its presence
  // port) to the game this machine hosts, as the players window's INVITE.
  void InviteAddress(uint32_t ip, const std::string &name);

  // UI thread, inside a scrolling ImGui child window (the F1 debug menu's
  // tab): the players window's content from `origin`, `width` wide;
  // `appearing` starts it over. False when an invite was accepted (the menu
  // should close).
  bool DrawTab(ImVec2 origin, float width, bool appearing);
} // namespace ao2::invite
