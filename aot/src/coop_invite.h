// coop_invite.h - Invites to a co-op game without Xbox Live (see coop_invite.cpp).
#pragma once

namespace ao2::invite
{
  // Starts the presence service (UDP ao2_invite_port): finds the other
  // players and carries invites. Call once at startup, after ao2::ea::Start.
  void Start();

  // Any thread: opens the players window (find players, invite, answer
  // invites) - what the game's "Invite friend" / friends list buttons open.
  void ShowPlayers();

  // Any thread: the players key - closes the open players window, else opens it.
  void TogglePlayers();

  // Any thread: there is an invite to answer / accept the newest one.
  bool HasPendingInvite();
  void AcceptPendingInvite();
} // namespace ao2::invite
