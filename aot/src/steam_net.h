// steam_net.h - Online play over Steam (Spacewar, app 480): lobbies, invites
// and all the game's traffic through Steam's relays (steam_net.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ao2::steam
{
  // A Steam player's address on this PC's virtual LAN: 127.64.0.0/10, the
  // same for that player on every PC (network order, as in sockaddr_in).
  // Inline: the lobby server (ea_server.cpp) is built without Steam.
  inline bool IsVirtual(uint32_t ip)
  {
    return (ip & 0xFF) == 127 && ((ip >> 8) & 0xC0) == 0x40;
  }

  // Starts Steam (ao2_steam) and its thread. Call once at startup, after
  // ao2::invite::Start. Without the Steam client nothing happens.
  void Start();

  // Whether Steam runs (signed in), and as whom.
  bool Running();
  std::string PersonaName();

  // The Steam overlay is open, or closed a moment ago: the game gets no input (its Esc closed the overlay).
  bool OverlayBlocksInput();

  // For the server list (translated): a lobby's mission and mode.
  std::string MissionName(const std::string &map, bool versus);
  const char *ModeName(bool versus, int game_type);
  const char *DifficultyName(int difficulty); // "" when unknown

  // The Steam players this game knows (friends in the game, lobby members,
  // whoever reached it): their virtual addresses, for the presence service's
  // announcements.
  std::vector<uint32_t> PeerAddresses();

  // This PC's own virtual address (0 without Steam).
  uint32_t SelfAddress();

  // The SteamID of the Steam player at virtual address `ip` (0: none known).
  uint64_t SteamIdOf(uint32_t ip);
  // Shows that player's Steam profile in the Steam overlay; false without Steam.
  bool OpenProfile(uint64_t steam_id);

  // From the presence service (every announcement): the lobby game this
  // machine hosts, or none. A hosted game gets a Steam lobby (the server
  // list, friends' "Join game"); a player entering it is invited
  // (ao2::invite::InviteAddress).
  struct Hosting
  {
    bool on = false;
    bool is_private = false;
    bool versus = false;
    bool in_game = false; // the match runs (no joining it)
    bool session = false; // this game is in a network game, hosted or not, also with `on` false
    int players = 1;
    int max = 2;
    std::string name;
    std::string build;
    std::string map;    // ea::HostedGame map / game_type / mode
    int game_type = -1;
    int difficulty = -1; // co-op: 0 Recruit, 1 Contractor, 2 Professional
    std::string mode;
    std::string menu;  // versus: "setup" (Game Setup), "ready" (the ready room), "" (neither)
    std::string rules; // versus: the host's match rules and bots, "key=value;..." (coop_invite.cpp)
  };
  void SetHosting(const Hosting &hosting);

  // An invite from `ip` comes from a lobby this player chose to join (the
  // list's JOIN, a friend's "Join game"): accepted without asking. Once.
  bool TakeAutoAccept(uint32_t ip);
  // How many such invites were accepted so far (a join that went through).
  uint32_t AutoAccepted();

  // The server list (Steam lobbies of this game, worldwide), the lowest
  // ping first.
  struct Lobby
  {
    uint64_t id = 0;
    std::string name;
    std::string build;
    std::string map;
    int game_type = -1;
    int difficulty = -1;
    bool versus = false;
    bool is_private = false;
    bool in_game = false;
    bool joined = false; // this player's lobby (hosted or joined)
    bool own = false;    // ... the one it hosts
    int players = 0;
    int max = 0;
    int ping = -1; // ms, estimated from the host's ping location (-1: unknown)
    std::string menu;  // Hosting::menu
    std::string rules; // Hosting::rules
  };
  // Quick join's choice among the open lobbies (lower is better): the ping, less 60 ms for each player there
  // already, so a fuller game wins over an emptier one unless its ping is clearly worse (an unknown ping counts
  // as 300 ms). A game that waits for one more player starts sooner, and its players are not split up.
  inline int QuickScore(const Lobby &l)
  {
    return (l.ping < 0 ? 300 : l.ping) - 60 * (l.players > 0 ? l.players : 0);
  }
  void RefreshLobbies();
  bool Refreshing();
  std::vector<Lobby> Lobbies();
  void JoinLobby(uint64_t id);

  // Steam's invite dialog for this player's lobby (none: false).
  bool InviteFriends();

  // Playing with someone met elsewhere (Discord, a forum), Steam friends or
  // not: this player's code (their Steam account in 7 letters, "ABC-DEFG";
  // empty without Steam) and a join by the host's code. The host's game, if
  // it hosts a lobby game with a free place, invites whoever knocks with its
  // code, and the invite is accepted by itself.
  std::string JoinCode();
  bool JoinByCode(const std::string &code); // false: not a code
  enum class CodeJoin
  {
    kNone,
    kKnocking, // asking the host (up to ~20 s)
    kInvited,  // the host's game sent its invite
    kNoGame,   // the host hosts no game now
    kFull,
    kStarted,  // its match runs
    kNoAnswer, // no answer (not running the game, or no Steam)
    kOwnCode,
  };
  CodeJoin CodeJoinState();
  std::string CodeJoinHost(); // the host's Steam name, once it answered

  // The game's public quick match found nothing on this PC's server
  // (ea::SetQuickMatchHook): the open Steam lobby of the same mode and build
  // that QuickScore picks (fullest, good ping) is joined; true once its invite was accepted, false
  // (at once, or after ~15 s) when there is none or it did not work out.
  bool QuickMatch(const std::string &mode);
} // namespace ao2::steam
