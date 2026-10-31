// coop_invite.h - Invites to a co-op game without Xbox Live (see coop_invite.cpp).
#pragma once

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

  // UI thread, inside a scrolling ImGui child window (the F1 debug menu's
  // tab): the players window's content from `origin`, `width` wide;
  // `appearing` starts it over. False when an invite was accepted (the menu
  // should close).
  bool DrawTab(ImVec2 origin, float width, bool appearing);
} // namespace ao2::invite
