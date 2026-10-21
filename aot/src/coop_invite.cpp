// coop_invite.cpp - Invites to a co-op game without Xbox Live.
//
// On the console a Private Co-op Game is joined by invitation only: the host
// presses "Invite", the Xbox guide lists friends (XShowGameInviteUI), Live
// delivers the invite, and when the friend accepts it the guide sends the
// title XN_LIVE_INVITE_ACCEPTED; the title then asks XInviteGetAcceptedInfo
// for the host's session (XINVITE_INFO: invitee and inviter XUIDs, title,
// XSESSION_INFO = session id, host XNADDR, key exchange key) and joins it.
// There is no Live here, so this file is the guide and the delivery:
//
// - Presence (UDP ao2_invite_port, 18491): every game announces itself
//   (HELLO) to the local networks' broadcast addresses, to the players in
//   ao2_friends, to ao2_coop_host and to the players it already knows, every
//   few seconds; whoever hears it answers (HERE). A player behind NAT is
//   reached through the address its own announcement came from, so one of
//   the two only needs to be reachable (VPN / LAN / forwarded port).
// - The game's invite / friends UI (XamShowGameInviteUI sub_82B07EB0,
//   XamShowFriendsUI sub_82B07E60) opens the players window (debug_menu.h
//   OpenWindow) instead: the players found, an Invite button each, an address
//   field for anyone not found.
// - An invite carries the lobby game this machine hosts on its built-in EA
//   server (ea_server.cpp FindHostedGame: XB360SESS = the Live session id,
//   the host's XNADDR and XUID). The invited player sees a notice (debug_menu.h
//   SetNotice) and accepts with ao2_invite_accept_key (F6) or in the players
//   window; then its game is pointed at the inviter's EA server
//   (net_title_server_ip) and gets XN_LIVE_INVITE_ACCEPTED (0x02000002).
// - Its notification pump (sub_8222D530) then calls XInviteGetAcceptedInfo
//   (sub_82C41388, XMsgInProcessCall 0xFC / 0x58023; the SDK has no answer),
//   answered here with the invite; EA's layer (sub_82236E30) checks the
//   invitee XUID against the signed-in one (sub_82B07E10(user, 5, &xuid)) and
//   passes the inviter and session to its listeners (vfunc +4), which raise
//   the script's OnXbInviteAccepted.
//
// Packets: "AO2I1 <TYPE>\n" then "key=value\n" lines. HELLO / HERE (name,
// hosting, game state), INVITE (id, name, xuid, session, xnaddr, server),
// ACK (id), ANSWER (id, accepted).
#include "coop_invite.h"

#include "coop.h"
#include "debug_menu.h"
#include "ea_server.h"
#include "log_files.h"
#include "ui_text.h"
#include "ui_theme.h"

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
// clang-format on

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(ao2_invites, true, "Online",
                    "Invites to a co-op game (Private Co-op): the game's invite / friends buttons open the "
                    "players window; other players on the LAN / VPN and in ao2_friends are found automatically.");
REXCVAR_DEFINE_UINT32(ao2_invite_port, 18491, "Online",
                      "UDP port of the invite / presence service (the same for all players).");
REXCVAR_DEFINE_STRING(ao2_friends, "", "Online",
                      "Players to look for besides the local networks: IP addresses (or ip:port), comma "
                      "separated - e.g. a friend's Radmin VPN / public address.");
REXCVAR_DEFINE_STRING(ao2_invite_accept_key, "F6", "Online", "Key that accepts the invite on the screen.");
REXCVAR_DEFINE_STRING(ao2_players_key, "F8", "Online",
                      "Key that opens the players window (invites); empty = none.");

REXCVAR_DECLARE(bool, ao2_coop_server);
REXCVAR_DECLARE(std::string, ao2_coop_host);

extern "C" REX_FUNC(sub_82B07E10);

namespace
{
  using ao2::text::Tr;
  using Clock = std::chrono::steady_clock;
  using Fields = std::vector<std::pair<std::string, std::string>>;

  constexpr char kMagic[] = "AO2I1";
  constexpr uint32_t kInviteAccepted = 0x02000002; // XN_LIVE_INVITE_ACCEPTED
  constexpr auto kPeerTimeout = std::chrono::seconds(30);
  constexpr auto kInviteLifetime = std::chrono::minutes(3);

  // -------------------------------------------------------------------------
  // Log (invite.log)
  // -------------------------------------------------------------------------

  std::mutex g_log_mutex;

  void Log(const char *fmt, ...)
  {
    std::lock_guard lock(g_log_mutex);
    static FILE *f = ao2::OpenLogFile("invite.log");
    if (!f)
    {
      return;
    }
    std::time_t now = std::time(nullptr);
    std::tm local = {};
    localtime_s(&local, &now);
    std::fprintf(f, "[%02d:%02d:%02d] ", local.tm_hour, local.tm_min, local.tm_sec);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fputc('\n', f);
    std::fflush(f);
  }

  // -------------------------------------------------------------------------
  // Addresses
  // -------------------------------------------------------------------------

  struct Address
  {
    uint32_t ip = 0;   // network order
    uint16_t port = 0; // host order
    bool operator<(const Address &o) const { return ip != o.ip ? ip < o.ip : port < o.port; }
    bool operator==(const Address &o) const { return ip == o.ip && port == o.port; }
  };

  std::string IpString(uint32_t ip)
  {
    char buf[INET_ADDRSTRLEN] = {};
    in_addr a = {};
    a.s_addr = ip;
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
  }

  bool IsLoopback(uint32_t ip)
  {
    return (ntohl(ip) >> 24) == 127;
  }

  uint16_t Port()
  {
    return uint16_t(REXCVAR_GET(ao2_invite_port));
  }

  // "1.2.3.4" or "1.2.3.4:port" (a host name is looked up).
  bool ParseAddress(std::string text, Address *out)
  {
    text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == ' ' || c == '\t'; }), text.end());
    if (text.empty())
    {
      return false;
    }
    out->port = Port();
    size_t colon = text.find(':');
    if (colon != std::string::npos)
    {
      int port = std::atoi(text.c_str() + colon + 1);
      if (port > 0 && port < 65536)
      {
        out->port = uint16_t(port);
      }
      text.resize(colon);
    }
    in_addr a = {};
    if (inet_pton(AF_INET, text.c_str(), &a) == 1)
    {
      out->ip = a.s_addr;
      return true;
    }
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(text.c_str(), nullptr, &hints, &res) != 0 || !res)
    {
      return false;
    }
    out->ip = reinterpret_cast<sockaddr_in *>(res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return true;
  }

  std::vector<Address> ParseList(const std::string &list)
  {
    std::vector<Address> out;
    size_t start = 0;
    while (start <= list.size())
    {
      size_t end = list.find_first_of(",;", start);
      if (end == std::string::npos)
      {
        end = list.size();
      }
      Address a;
      if (ParseAddress(list.substr(start, end - start), &a))
      {
        out.push_back(a);
      }
      start = end + 1;
    }
    return out;
  }

  // The directed broadcast address of every active adapter (Radmin VPN
  // passes those on), plus the limited broadcast.
  std::vector<uint32_t> BroadcastAddresses()
  {
    std::vector<uint32_t> out = {htonl(INADDR_BROADCAST)};
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buffer(size);
    auto *list = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) == ERROR_BUFFER_OVERFLOW)
    {
      buffer.resize(size);
      list = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    }
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) != NO_ERROR)
    {
      return out;
    }
    for (auto *a = list; a; a = a->Next)
    {
      if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
      {
        continue;
      }
      for (auto *u = a->FirstUnicastAddress; u; u = u->Next)
      {
        uint32_t ip = ntohl(reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr)->sin_addr.s_addr);
        uint32_t prefix = u->OnLinkPrefixLength;
        if (prefix == 0 || prefix >= 31)
        {
          continue;
        }
        uint32_t mask = prefix ? ~0u << (32 - prefix) : 0;
        uint32_t bcast = htonl(ip | ~mask);
        if (std::find(out.begin(), out.end(), bcast) == out.end())
        {
          out.push_back(bcast);
        }
      }
    }
    return out;
  }

  std::string LocalName()
  {
    std::string name = rex::cvar::GetFlagByName("net_gamertag");
    if (name.empty())
    {
      char host[256] = {};
      gethostname(host, sizeof(host) - 1);
      name = host[0] ? host : "Player";
    }
    if (name.size() > 15)
    {
      name.resize(15); // as the SDK cuts the gamertag
    }
    return name;
  }

  // -------------------------------------------------------------------------
  // Packets
  // -------------------------------------------------------------------------

  std::string Get(const Fields &fields, std::string_view key, std::string_view fallback = "")
  {
    for (const auto &[k, v] : fields)
    {
      if (k == key)
      {
        return v;
      }
    }
    return std::string(fallback);
  }

  std::string Build(const std::string &type, const Fields &fields)
  {
    std::string out = std::string(kMagic) + " " + type + "\n";
    for (const auto &[k, v] : fields)
    {
      out += k + "=" + v + "\n";
    }
    return out;
  }

  bool Parse(const char *data, size_t n, std::string *type, Fields *fields)
  {
    std::string text(data, n);
    size_t eol = text.find('\n');
    std::string head = text.substr(0, eol);
    if (head.rfind(std::string(kMagic) + " ", 0) != 0)
    {
      return false;
    }
    *type = head.substr(sizeof(kMagic));
    size_t at = eol == std::string::npos ? text.size() : eol + 1;
    while (at < text.size())
    {
      size_t end = text.find('\n', at);
      if (end == std::string::npos)
      {
        end = text.size();
      }
      std::string line = text.substr(at, end - at);
      size_t eq = line.find('=');
      if (eq != std::string::npos)
      {
        fields->emplace_back(line.substr(0, eq), line.substr(eq + 1));
      }
      at = end + 1;
    }
    return true;
  }

  std::string Hex(const std::vector<uint8_t> &bytes)
  {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (uint8_t b : bytes)
    {
      out += kDigits[b >> 4];
      out += kDigits[b & 15];
    }
    return out;
  }

  std::vector<uint8_t> Unhex(const std::string &text)
  {
    std::vector<uint8_t> out;
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return -1;
    };
    for (size_t i = 0; i + 1 < text.size(); i += 2)
    {
      int hi = nibble(text[i]), lo = nibble(text[i + 1]);
      if (hi < 0 || lo < 0)
      {
        return {};
      }
      out.push_back(uint8_t(hi << 4 | lo));
    }
    return out;
  }

  // -------------------------------------------------------------------------
  // State
  // -------------------------------------------------------------------------

  struct Peer
  {
    Address address;
    std::string name;
    std::string state; // menu / lobby / game
    bool hosting = false;
    Clock::time_point seen;
  };

  enum class SentState
  {
    kSending,
    kDelivered,
    kAccepted,
    kDeclined,
    kFailed,
  };

  struct SentInvite
  {
    uint32_t id = 0;
    Address to;
    std::string name;
    std::string packet;
    SentState state = SentState::kSending;
    int tries = 0;
    Clock::time_point next_send;
    Clock::time_point since = Clock::now(); // sent, or answered
  };

  // An invite nobody joined through this long may be sent again.
  constexpr auto kReinviteAfter = std::chrono::seconds(15);

  struct ReceivedInvite
  {
    uint32_t id = 0;
    Address from;
    std::string name;
    uint64_t inviter_xuid = 0;
    uint64_t session_id = 0;
    std::vector<uint8_t> xnaddr; // 36 bytes, addresses already the inviter's
    std::string server_ip;       // the inviter's EA server
    bool is_private = false;
    bool playgroup = false;      // a versus party: joined with the playgroup manager
    Clock::time_point received;
  };

  struct State
  {
    std::mutex mutex;
    SOCKET socket = INVALID_SOCKET;
    std::map<Address, Peer> peers;
    std::vector<Address> manual; // added in the players window
    std::vector<SentInvite> sent;
    std::vector<ReceivedInvite> received; // newest last
    bool have_accepted = false;
    ReceivedInvite accepted; // what XInviteGetAcceptedInfo answers
    Clock::time_point next_beacon;
    bool window_open = false;
    // The game this player hosts on a lobby server in another process
    // (GAMEQ / GAME), when its own process runs none.
    ao2::ea::HostedGame remote_game;
    Clock::time_point remote_game_seen;
  };

  State &state()
  {
    static State s;
    return s;
  }

  std::atomic<bool> g_started{false};

  // This game's id in its packets: its own broadcasts come back to it.
  const std::string &InstanceId()
  {
    static const std::string id = std::to_string(std::random_device{}()) + std::to_string(std::random_device{}());
    return id;
  }

  // The lobby server's address (the game's net_title_server_ip).
  bool ServerAddress(Address *out)
  {
    return ParseAddress(rex::cvar::GetFlagByName("net_title_server_ip"), out);
  }

  // The lobby game this player hosts: on this process's server, or as the
  // server's presence service last told (GAME).
  bool MyHostedGame(ao2::ea::HostedGame *out)
  {
    if (ao2::ea::FindHostedGame(LocalName(), out))
    {
      return true;
    }
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (st.remote_game.session_id && Clock::now() - st.remote_game_seen < std::chrono::seconds(12))
    {
      *out = st.remote_game;
      return true;
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // Sending
  // -------------------------------------------------------------------------

  void SendTo(const Address &to, const std::string &packet)
  {
    SOCKET s = state().socket;
    if (s == INVALID_SOCKET)
    {
      return;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = to.ip;
    addr.sin_port = htons(to.port);
    sendto(s, packet.data(), int(packet.size()), 0, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  }

  // What this game is doing, for the other players' lists.
  Fields Presence()
  {
    ao2::ea::HostedGame game;
    bool hosting = MyHostedGame(&game);
    const uint8_t *base = rex::Runtime::instance() ? rex::Runtime::instance()->virtual_membase() : nullptr;
    std::string st = base && ao2::coop::OnlineLockstep(base) ? "game" : ao2::coop::network_session ? "lobby" : "menu";
    return {{"name", LocalName()}, {"iid", InstanceId()}, {"hosting", hosting ? "1" : "0"}, {"state", st}};
  }

  // HELLO to the broadcast addresses, the configured and the known players.
  void Beacon()
  {
    auto &st = state();
    std::string packet = Build("HELLO", Presence());
    std::vector<Address> targets;
    for (uint32_t ip : BroadcastAddresses())
    {
      targets.push_back({ip, Port()});
    }
    for (const Address &a : ParseList(REXCVAR_GET(ao2_friends)))
    {
      targets.push_back(a);
    }
    for (const Address &a : ParseList(REXCVAR_GET(ao2_coop_host)))
    {
      targets.push_back(a);
    }
    {
      std::lock_guard lock(st.mutex);
      for (const Address &a : st.manual)
      {
        targets.push_back(a);
      }
      for (const auto &[a, p] : st.peers)
      {
        targets.push_back(a);
      }
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    for (const Address &a : targets)
    {
      SendTo(a, packet);
    }
    // Our game on a lobby server in another process (its presence answers).
    ao2::ea::HostedGame local;
    Address server;
    if (!ao2::ea::FindHostedGame(LocalName(), &local) && ServerAddress(&server))
    {
      SendTo(server, Build("GAMEQ", {{"name", LocalName()}, {"iid", InstanceId()}}));
    }
  }

  // -------------------------------------------------------------------------
  // Receiving
  // -------------------------------------------------------------------------

  void ShowInviteNotice();
  void ShowAnswerNotice(const std::string &who, bool accepted);

  void OnPacket(const Address &from, const std::string &type, const Fields &f)
  {
    auto &st = state();
    auto now = Clock::now();
    if (type == "HELLO" || type == "HERE")
    {
      std::string name = Get(f, "name");
      if (name.empty())
      {
        return;
      }
      {
        std::lock_guard lock(st.mutex);
        Peer &p = st.peers[from];
        if (p.name.empty())
        {
          Log("found %s at %s:%u", name.c_str(), IpString(from.ip).c_str(), from.port);
        }
        p.address = from;
        p.name = name;
        p.state = Get(f, "state", "menu");
        p.hosting = Get(f, "hosting") == "1";
        p.seen = now;
      }
      if (type == "HELLO")
      {
        SendTo(from, Build("HERE", Presence()));
      }
    }
    else if (type == "GAMEQ")
    {
      // A player of this process's lobby server asks for the game it hosts.
      ao2::ea::HostedGame game;
      if (ao2::ea::FindHostedGame(Get(f, "name"), &game))
      {
        char session[32], xuid[32];
        std::snprintf(session, sizeof(session), "%016llx", (unsigned long long)game.session_id);
        std::snprintf(xuid, sizeof(xuid), "%016llx", (unsigned long long)game.host_xuid);
        SendTo(from, Build("GAME", {{"gid", std::to_string(game.gid)},
                                    {"session", session},
                                    {"xnaddr", Hex(game.xnaddr)},
                                    {"xuid", xuid},
                                    {"private", game.is_private ? "1" : "0"},
                                    {"playgroup", game.playgroup ? "1" : "0"},
                                    {"players", std::to_string(game.players)}}));
      }
    }
    else if (type == "GAME")
    {
      ao2::ea::HostedGame game;
      game.gid = uint32_t(std::strtoul(Get(f, "gid").c_str(), nullptr, 10));
      game.session_id = std::strtoull(Get(f, "session").c_str(), nullptr, 16);
      game.xnaddr = Unhex(Get(f, "xnaddr"));
      game.host_xuid = std::strtoull(Get(f, "xuid").c_str(), nullptr, 16);
      game.is_private = Get(f, "private") == "1";
      game.playgroup = Get(f, "playgroup") == "1";
      game.players = std::atoi(Get(f, "players", "1").c_str());
      if (game.session_id && game.xnaddr.size() == 36)
      {
        std::lock_guard lock(st.mutex);
        st.remote_game = game;
        st.remote_game_seen = now;
      }
    }
    else if (type == "INVITE")
    {
      uint32_t id = uint32_t(std::strtoul(Get(f, "id").c_str(), nullptr, 10));
      SendTo(from, Build("ACK", {{"id", std::to_string(id)}}));
      ReceivedInvite inv;
      inv.id = id;
      inv.from = from;
      inv.name = Get(f, "name", "?");
      inv.inviter_xuid = std::strtoull(Get(f, "xuid").c_str(), nullptr, 16);
      inv.session_id = std::strtoull(Get(f, "session").c_str(), nullptr, 16);
      inv.xnaddr = Unhex(Get(f, "xnaddr"));
      inv.is_private = Get(f, "private") == "1";
      inv.playgroup = Get(f, "playgroup") == "1";
      // The inviter's lobby server; a loopback address means the inviter's
      // own PC (unless both run on this one).
      Address server;
      if (!ParseAddress(Get(f, "server"), &server) || (IsLoopback(server.ip) && !IsLoopback(from.ip)))
      {
        server.ip = from.ip;
      }
      inv.server_ip = IpString(server.ip);
      inv.received = now;
      if (inv.xnaddr.size() != 36 || !inv.session_id)
      {
        Log("invite from %s without a session, ignored", inv.name.c_str());
        return;
      }
      // The inviter's XNADDR as this machine reaches it (ina, inaOnline and
      // abOnline[0..3], see ea_server.cpp RewriteXnAddr).
      for (int offset : {0, 4, 16})
      {
        std::memcpy(inv.xnaddr.data() + offset, &from.ip, 4);
      }
      {
        std::lock_guard lock(st.mutex);
        auto dup = std::find_if(st.received.begin(), st.received.end(),
                                [&](const ReceivedInvite &r) { return r.from == from && r.id == id; });
        if (dup != st.received.end())
        {
          return; // a resend
        }
        std::erase_if(st.received, [&](const ReceivedInvite &r) { return r.from == from; });
        st.received.push_back(inv);
      }
      Log("invite %u from %s (%s), session %016llx, server %s", id, inv.name.c_str(), IpString(from.ip).c_str(),
          (unsigned long long)inv.session_id, inv.server_ip.c_str());
      ShowInviteNotice();
    }
    else if (type == "ACK" || type == "ANSWER")
    {
      uint32_t id = uint32_t(std::strtoul(Get(f, "id").c_str(), nullptr, 10));
      std::string who;
      SentState result = SentState::kDelivered;
      {
        std::lock_guard lock(st.mutex);
        for (SentInvite &s : st.sent)
        {
          if (s.id != id || !(s.to.ip == from.ip))
          {
            continue;
          }
          if (type == "ACK")
          {
            if (s.state == SentState::kSending)
            {
              s.state = SentState::kDelivered;
            }
          }
          else
          {
            s.state = Get(f, "accepted") == "1" ? SentState::kAccepted : SentState::kDeclined;
            s.since = now;
            result = s.state;
            who = s.name;
          }
        }
      }
      if (type == "ANSWER" && !who.empty())
      {
        Log("%s %s invite %u", who.c_str(), result == SentState::kAccepted ? "accepted" : "declined", id);
        ShowAnswerNotice(who, result == SentState::kAccepted);
      }
    }
  }

  void Loop()
  {
    auto &st = state();
    char buf[2048];
    while (true)
    {
      auto now = Clock::now();
      bool beacon = false;
      std::vector<std::pair<Address, std::string>> resend;
      {
        std::lock_guard lock(st.mutex);
        if (now >= st.next_beacon)
        {
          beacon = true;
          st.next_beacon = now + (st.window_open ? std::chrono::seconds(2) : std::chrono::seconds(8));
        }
        std::erase_if(st.peers, [&](const auto &kv) { return now - kv.second.seen > kPeerTimeout; });
        std::erase_if(st.received, [&](const ReceivedInvite &r) { return now - r.received > kInviteLifetime; });
        for (SentInvite &s : st.sent)
        {
          if (s.state != SentState::kSending || now < s.next_send)
          {
            continue;
          }
          if (++s.tries > 8)
          {
            s.state = SentState::kFailed;
            Log("invite %u to %s: no answer", s.id, s.name.c_str());
            continue;
          }
          s.next_send = now + std::chrono::milliseconds(700);
          resend.emplace_back(s.to, s.packet);
        }
      }
      if (beacon)
      {
        Beacon();
      }
      for (const auto &[to, packet] : resend)
      {
        SendTo(to, packet);
      }

      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(st.socket, &fds);
      timeval tv = {0, 250 * 1000};
      if (select(0, &fds, nullptr, nullptr, &tv) <= 0)
      {
        continue;
      }
      sockaddr_in from = {};
      int from_len = sizeof(from);
      int n = recvfrom(st.socket, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
      if (n <= 0)
      {
        continue; // WSAECONNRESET: ICMP unreachable from an earlier send
      }
      std::string type;
      Fields fields;
      if (!Parse(buf, size_t(n), &type, &fields))
      {
        continue;
      }
      Address a{from.sin_addr.s_addr, ntohs(from.sin_port)};
      if (Get(fields, "iid") == InstanceId())
      {
        continue; // our own broadcast
      }
      OnPacket(a, type, fields);
    }
  }

  // -------------------------------------------------------------------------
  // Actions
  // -------------------------------------------------------------------------

  // Invites the player at `to`; false (and why) when this game hosts nothing.
  bool Invite(const Address &to, const std::string &name, std::string *error)
  {
    ao2::ea::HostedGame game;
    if (!MyHostedGame(&game))
    {
      *error = Tr("Create a game first: Campaign Mode - Private (or Public) Co-op Game.");
      return false;
    }
    static std::mt19937 rng{std::random_device{}()};
    auto &st = state();
    SentInvite s;
    s.id = rng() % 1000000000u;
    s.to = to;
    s.name = name;
    char session[32], xuid[32];
    std::snprintf(session, sizeof(session), "%016llx", (unsigned long long)game.session_id);
    std::snprintf(xuid, sizeof(xuid), "%016llx", (unsigned long long)game.host_xuid);
    s.packet = Build("INVITE", {{"id", std::to_string(s.id)},
                                {"name", LocalName()},
                                {"xuid", xuid},
                                {"session", session},
                                {"xnaddr", Hex(game.xnaddr)},
                                {"private", game.is_private ? "1" : "0"},
                                {"playgroup", game.playgroup ? "1" : "0"},
                                {"server", rex::cvar::GetFlagByName("net_title_server_ip")}});
    s.next_send = Clock::now();
    Log("inviting %s (%s:%u) to game %u, session %s", name.c_str(), IpString(to.ip).c_str(), to.port, game.gid,
        session);
    std::lock_guard lock(st.mutex);
    std::erase_if(st.sent, [&](const SentInvite &o) { return o.to == to; });
    st.sent.push_back(std::move(s));
    return true;
  }

  void Answer(const ReceivedInvite &inv, bool accept)
  {
    SendTo(inv.from, Build("ANSWER", {{"id", std::to_string(inv.id)}, {"accepted", accept ? "1" : "0"}}));
    auto &st = state();
    {
      std::lock_guard lock(st.mutex);
      std::erase_if(st.received, [&](const ReceivedInvite &r) { return r.from == inv.from && r.id == inv.id; });
      if (accept)
      {
        st.accepted = inv;
        st.have_accepted = true;
      }
    }
    ao2::menu::SetNotice(nullptr);
    if (!accept)
    {
      Log("declined invite %u from %s", inv.id, inv.name.c_str());
      return;
    }
    // The game must reach the inviter's EA server (Theater) to enter its game.
    std::string current = rex::cvar::GetFlagByName("net_title_server_ip");
    if (!inv.server_ip.empty() && current != inv.server_ip)
    {
      rex::cvar::SetFlagByName("net_title_server_ip", inv.server_ip);
      rex::cvar::SetFlagByName("ao2_coop_host", inv.server_ip);
      Log("EA server: %s -> %s", current.c_str(), inv.server_ip.c_str());
    }
    Log("accepted invite %u from %s: XN_LIVE_INVITE_ACCEPTED", inv.id, inv.name.c_str());
    if (auto *ks = rex::system::kernel_state())
    {
      ks->BroadcastNotification(kInviteAccepted, 0);
    }
  }

  // -------------------------------------------------------------------------
  // Notices (no input, drawn over the picture in the game's look, ui_theme.h)
  // -------------------------------------------------------------------------

  // A notice's animation clock: ImGui time of its first frame. Shared, as
  // the notice's std::function is copied for each frame.
  struct NoticeClock
  {
    double start = -1.0;

    float Since()
    {
      double now = ImGui::GetTime();
      if (start < 0.0)
      {
        start = now;
      }
      return float(now - start);
    }
  };

  // The newest invite: slides down from the top until accepted / declined /
  // expired; the key accepts it.
  void ShowInviteNotice()
  {
    auto clock = std::make_shared<NoticeClock>();
    ao2::menu::SetNotice([clock]() {
      namespace ui = ao2::ui;
      using ui::FontKind;
      auto &st = state();
      std::string name;
      bool is_private = false;
      Clock::time_point received;
      {
        std::lock_guard lock(st.mutex);
        if (st.received.empty())
        {
          return false;
        }
        name = st.received.back().name;
        is_private = st.received.back().is_private;
        received = st.received.back().received;
      }
      float left = std::chrono::duration<float>(kInviteLifetime - (Clock::now() - received)).count();
      float lifetime = std::chrono::duration<float>(kInviteLifetime).count();

      ImDrawList *dl = ImGui::GetForegroundDrawList();
      const ImGuiViewport *vp = ImGui::GetMainViewport();
      float s = ui::Scale();
      float since = clock->Since();
      float now = float(ImGui::GetTime());
      float a = ui::Clamp01(since / 0.2f);
      float drop = ui::EaseOutBack(since / 0.5f);
      ImVec2 size(560.0f * s, 124.0f * s);
      float top = vp->Pos.y + vp->Size.y * 0.05f;
      ImVec2 mn(vp->Pos.x + (vp->Size.x - size.x) * 0.5f, top - (1.0f - drop) * (top - vp->Pos.y + size.y));
      ImVec2 mx(mn.x + size.x, mn.y + size.y);
      float pulse = 0.5f + 0.5f * std::sin(now * 4.0f);

      // A flat navy card with a hairline frame that breathes, and a paper bar on the left.
      dl->AddRectFilled(mn, mx, ui::Fade(ui::kPanel, 0.97f * a));
      dl->AddRect(mn, mx, ui::Fade(ui::kAccent, (0.18f + 0.30f * pulse) * a), 0.0f, ImDrawFlags_None, 1.0f * s);
      dl->AddRectFilled(mn, ImVec2(mn.x + 2.0f * s, mx.y), ui::Fade(ui::kAccent, a));

      // Sight: rings, ticks, a turning arc.
      ImVec2 c(mn.x + 54.0f * s, mn.y + size.y * 0.5f - 4.0f * s);
      float r = 24.0f * s;
      dl->AddCircle(c, r, ui::Fade(ui::kAccent, 0.5f * a), 32, 1.5f * s);
      float turn = now * 1.8f;
      dl->PathArcTo(c, r + 6.0f * s, turn, turn + 1.4f, 16);
      dl->PathStroke(ui::Fade(ui::kAccent, a), ImDrawFlags_None, 2.0f * s);
      dl->PathArcTo(c, r + 6.0f * s, turn + 3.14159f, turn + 4.54f, 16);
      dl->PathStroke(ui::Fade(ui::kAccent, a), ImDrawFlags_None, 2.0f * s);
      for (int i = 0; i < 4; ++i)
      {
        float ang = 1.5708f * float(i);
        ImVec2 dir(std::cos(ang), std::sin(ang));
        dl->AddLine(ImVec2(c.x + dir.x * r * 0.45f, c.y + dir.y * r * 0.45f),
                    ImVec2(c.x + dir.x * r * 0.85f, c.y + dir.y * r * 0.85f), ui::Fade(ui::kAccent, a), 2.0f * s);
      }
      dl->AddCircleFilled(c, (2.5f + 1.5f * pulse) * s, ui::Fade(ui::kAccentLight, a));

      float tx = mn.x + 100.0f * s;
      ui::Caps(dl, FontKind::kLabel, ImVec2(tx, mn.y + 13.0f * s), ui::Fade(ui::kMuted, a), Tr("INCOMING INVITE"));
      char timer[16];
      int secs = std::max(int(left), 0);
      std::snprintf(timer, sizeof(timer), "%d:%02d", secs / 60, secs % 60);
      ImVec2 ts = ui::TextSize(FontKind::kSmall, timer);
      ui::Text(dl, FontKind::kSmall, ImVec2(mx.x - 14.0f * s - ts.x, mn.y + 14.0f * s), ui::Fade(ui::kMuted, a),
               timer);
      ui::Text(dl, FontKind::kHeading, ImVec2(tx, mn.y + 30.0f * s), ui::Fade(ui::kText, a), name.c_str());
      ui::Text(dl, FontKind::kSmall, ImVec2(tx, mn.y + 62.0f * s), ui::Fade(ui::kMuted, a),
               is_private ? Tr("invites you to a private co-op game") : Tr("invites you to a co-op game"));

      float kx = tx, ky = mn.y + 86.0f * s, kh = 22.0f * s;
      auto hint = [&](const std::string &key, const char *text) {
        if (key.empty())
        {
          return;
        }
        kx += ui::KeyCap(dl, ImVec2(kx, ky), kh, key.c_str(), a) + 8.0f * s;
        ui::Caps(dl, FontKind::kLabel, ImVec2(kx, ky + (kh - ui::FontSize(FontKind::kLabel)) * 0.5f - 1.0f * s),
                 ui::Fade(ui::kText, a), text);
        kx += ui::CapsSize(FontKind::kLabel, text).x + 22.0f * s;
      };
      hint(REXCVAR_GET(ao2_invite_accept_key), Tr("ACCEPT"));
      hint(REXCVAR_GET(ao2_players_key), Tr("LIST / DECLINE"));

      float frac = ui::Clamp01(left / lifetime);
      ImVec2 bar(mn.x + 2.0f * s, mx.y - 2.0f * s);
      dl->AddRectFilled(bar, mx, ui::Fade(ui::kWhite, 0.08f * a));
      dl->AddRectFilled(bar, ImVec2(bar.x + (mx.x - bar.x) * frac, mx.y), ui::Fade(ui::kAccent, 0.9f * a));
      return true;
    });
  }

  // The answer to an invite this game sent: a short notice.
  void ShowAnswerNotice(const std::string &who, bool accepted)
  {
    constexpr float kLife = 6.0f, kOut = 0.35f;
    auto clock = std::make_shared<NoticeClock>();
    ao2::menu::SetNotice([clock, who, accepted]() {
      namespace ui = ao2::ui;
      using ui::FontKind;
      float since = clock->Since();
      if (since > kLife)
      {
        return false;
      }
      ImDrawList *dl = ImGui::GetForegroundDrawList();
      const ImGuiViewport *vp = ImGui::GetMainViewport();
      float s = ui::Scale();
      float in = ui::EaseOutBack(since / 0.45f);
      float out = ui::EaseOutCubic((since - (kLife - kOut)) / kOut);
      float a = ui::Clamp01(since / 0.2f) * (1.0f - out);
      ImU32 accent = accepted ? ui::kGreen : ui::kRed;
      ImVec2 size(480.0f * s, 74.0f * s);
      float top = vp->Pos.y + vp->Size.y * 0.05f;
      float hidden = top - vp->Pos.y + size.y;
      ImVec2 mn(vp->Pos.x + (vp->Size.x - size.x) * 0.5f, top - (1.0f - in) * hidden - out * 30.0f * s);
      ImVec2 mx(mn.x + size.x, mn.y + size.y);

      dl->AddRectFilled(mn, mx, ui::Fade(ui::kPanel, 0.97f * a));
      dl->AddRect(mn, mx, ui::Fade(accent, 0.45f * a), 0.0f, ImDrawFlags_None, 1.0f * s);
      dl->AddRectFilled(mn, ImVec2(mn.x + 2.0f * s, mx.y), ui::Fade(accent, a));

      ImVec2 c(mn.x + 44.0f * s, mn.y + size.y * 0.5f);
      // The icon pops in, then a ring spreads out once.
      float pop = ui::EaseOutBack((since - 0.15f) / 0.4f);
      dl->AddCircleFilled(c, 20.0f * s * pop, ui::Fade(accent, 0.18f * a));
      float ring = ui::Clamp01((since - 0.2f) / 0.8f);
      dl->AddCircle(c, (20.0f + 18.0f * ring) * s, ui::Fade(accent, (1.0f - ring) * 0.7f * a), 32, 2.0f * s);
      if (accepted)
      {
        ui::Check(dl, c, 20.0f * s * pop, 3.0f * s, ui::Fade(accent, a));
      }
      else
      {
        ui::Cross(dl, c, 20.0f * s * pop, 3.0f * s, ui::Fade(accent, a));
      }

      float tx = mn.x + 80.0f * s;
      ui::Text(dl, FontKind::kHeading, ImVec2(tx, mn.y + 10.0f * s), ui::Fade(ui::kText, a), who.c_str());
      ui::Text(dl, FontKind::kSmall, ImVec2(tx, mn.y + 44.0f * s), ui::Fade(accepted ? ui::kText : ui::kMuted, a),
               accepted ? Tr("ACCEPTED THE INVITE  ·  JOINING") : Tr("DECLINED THE INVITE"));
      return true;
    });
  }

  const char *StateText(const std::string &s)
  {
    return s == "game" ? Tr("in game") : s == "lobby" ? Tr("in lobby") : Tr("in menu");
  }

  // The players window (UI thread, inside an ImGui frame): a full screen
  // overlay in the game's look (ui_theme.h). It fades and slides in, its
  // rows come in one after another, and Esc / Close play it out.
  struct PlayersWindow
  {
    static constexpr float kOpenTime = 0.35f;
    static constexpr float kCloseTime = 0.2f;

    char address[64] = {};
    std::string error;
    double opened = -1.0;
    double closing = -1.0;
    float panel_h = 0.0f;
    std::map<std::string, double> row_shown; // when each player's row came in
    std::map<std::string, double> row_flash; // when that player was invited
    std::atomic<bool> close_requested = false; // the players key, from any thread

    void BeginClose()
    {
      if (closing < 0.0)
      {
        closing = ImGui::GetTime();
      }
    }

    bool Draw()
    {
      namespace ui = ao2::ui;
      using ui::FontKind;
      auto &st = state();
      double now = ImGui::GetTime();
      if (opened < 0.0)
      {
        opened = now;
      }
      float since = float(now - opened);
      float a = ui::EaseOutCubic(since / kOpenTime);
      if (closing >= 0.0)
      {
        float c = float(now - closing) / kCloseTime;
        if (c >= 1.0f)
        {
          std::lock_guard lock(st.mutex);
          st.window_open = false;
          return false;
        }
        a *= 1.0f - ui::EaseOutCubic(c);
      }
      bool live = closing < 0.0;
      float s = ui::Scale();
      float pulse = 0.5f + 0.5f * std::sin(float(now) * 4.0f);

      ao2::ea::HostedGame game;
      bool hosting = MyHostedGame(&game);
      std::vector<ReceivedInvite> received;
      std::vector<Peer> peers;
      std::vector<SentInvite> sent;
      {
        std::lock_guard lock(st.mutex);
        received = st.received;
        for (const auto &[addr, p] : st.peers)
        {
          peers.push_back(p);
        }
        sent = st.sent;
      }
      std::sort(peers.begin(), peers.end(), [](const Peer &x, const Peer &y) { return x.name < y.name; });

      const ImGuiViewport *vp = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(vp->Pos);
      ImGui::SetNextWindowSize(vp->Size);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::Begin("##players", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::PopStyleVar(2);
      if (live && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || close_requested))
      {
        BeginClose();
      }

      // The content goes on top (channel 1); the panel behind it (channel 0)
      // is drawn last, once the content's height is known.
      ImDrawList *dl = ImGui::GetWindowDrawList();
      dl->ChannelsSplit(2);
      dl->ChannelsSetCurrent(1);

      const float width = std::min(800.0f * s, vp->Size.x - 40.0f * s);
      const float pad = 32.0f * s;
      ImVec2 center = vp->GetCenter();
      float x0 = center.x - width * 0.5f - (1.0f - a) * 40.0f * s;
      float y0 = center.y - panel_h * 0.5f;
      float x1 = x0 + width;
      float cx0 = x0 + pad, cx1 = x1 - pad;
      float y = y0 + pad;

      auto section = [&](const char *label, int count) {
        ui::Caps(dl, FontKind::kLabel, ImVec2(cx0, y), ui::Fade(ui::kMuted, a), label);
        float lx = cx0 + ui::CapsSize(FontKind::kLabel, label).x + 10.0f * s;
        if (count >= 0)
        {
          std::string n = std::to_string(count);
          ui::Caps(dl, FontKind::kLabel, ImVec2(lx, y), ui::Fade(ui::kAccent, a), n.c_str());
          lx += ui::CapsSize(FontKind::kLabel, n.c_str()).x + 10.0f * s;
        }
        float ly = y + ui::FontSize(FontKind::kLabel) * 0.65f;
        dl->AddLine(ImVec2(lx, ly), ImVec2(cx1, ly), ui::Fade(ui::kWhite, 0.10f * a), 1.0f * s);
        y += ui::FontSize(FontKind::kLabel) + 14.0f * s;
      };

      // --- Header: kicker, title (wiped in), status, divider ---------------
      const char *kicker = !hosting          ? Tr("CO-OP  //  INVITES")
                           : game.is_private ? Tr("CO-OP  //  PRIVATE GAME")
                                             : Tr("CO-OP  //  PUBLIC GAME");
      ui::Caps(dl, FontKind::kLabel, ImVec2(cx0, y), ui::Fade(ui::kMuted, a), kicker);
      y += ui::FontSize(FontKind::kLabel) + 6.0f * s;
      const char *title = hosting ? Tr("INVITE A PARTNER") : !received.empty() ? Tr("CALLED TO ARMS") : Tr("PLAYERS ONLINE");
      ImVec2 title_size = ui::CapsSize(FontKind::kTitle, title);
      float wipe = ui::EaseOutCubic((since - 0.08f) / 0.45f);
      float edge = cx0 + title_size.x * wipe;
      dl->PushClipRect(ImVec2(cx0, y), ImVec2(edge + 1.0f, y + title_size.y), true);
      ui::Caps(dl, FontKind::kTitle, ImVec2(cx0, y), ui::Fade(ui::kAccentLight, a), title);
      dl->PopClipRect();
      {
        char status[64];
        if (hosting)
        {
          std::snprintf(status, sizeof(status), Tr("GAME CREATED  ·  %d/2"), game.players);
        }
        else
        {
          std::snprintf(status, sizeof(status), "%s", Tr("NO GAME OF YOURS"));
        }
        ImVec2 ss = ui::CapsSize(FontKind::kLabel, status);
        float chip_h = ss.y + 12.0f * s;
        ImVec2 cmx(cx1, y + (title_size.y + chip_h) * 0.5f);
        ImVec2 cmn(cmx.x - ss.x - 36.0f * s, cmx.y - chip_h);
        ImU32 dot = hosting ? ui::kGreen : ui::kMuted;
        dl->AddRect(cmn, cmx, ui::Fade(ui::kWhite, 0.16f * a), 0.0f, ImDrawFlags_None, 1.0f * s);
        ImVec2 dc(cmn.x + 13.0f * s, (cmn.y + cmx.y) * 0.5f);
        dl->AddCircleFilled(dc, 4.0f * s, ui::Fade(dot, a));
        if (hosting)
        {
          dl->AddCircle(dc, (4.0f + 4.0f * pulse) * s, ui::Fade(dot, (1.0f - pulse) * a), 16, 1.0f * s);
        }
        ui::Caps(dl, FontKind::kLabel, ImVec2(cmn.x + 26.0f * s, cmn.y + 6.0f * s), ui::Fade(ui::kText, a), status);
      }
      y += title_size.y + 12.0f * s;
      // A hairline with a short paper underline at its start, like the launcher's current tab.
      float line = ui::EaseOutCubic((since - 0.15f) / 0.5f);
      dl->AddLine(ImVec2(cx0, y), ImVec2(cx0 + (cx1 - cx0) * line, y), ui::Fade(ui::kWhite, 0.13f * a), 1.0f * s);
      dl->AddRectFilled(ImVec2(cx0, y - 1.0f * s), ImVec2(cx0 + 56.0f * s * line, y + 1.0f * s), ui::Fade(ui::kAccent, a));
      y += 24.0f * s;

      // --- Squad: the two seats of the hosted game -----------------------
      if (hosting)
      {
        enum class Seat
        {
          kReady,
          kJoining,
          kWaiting,
        };
        float gap = 14.0f * s, cw = (cx1 - cx0 - gap) * 0.5f, ch = 80.0f * s;
        auto seat = [&](float x, const char *label, std::string name, Seat mode) {
          ImVec2 mn(x, y), mx(x + cw, y + ch);
          ImU32 accent = mode == Seat::kReady ? ui::kGreen : ui::kAccent;
          dl->AddRectFilled(mn, mx, ui::Fade(ui::kWhite, 0.035f * a));
          if (mode == Seat::kWaiting)
          {
            // The empty seat's frame breathes.
            dl->AddRect(mn, mx, ui::Fade(ui::kAccent, (0.12f + 0.22f * pulse) * a), 0.0f, ImDrawFlags_None, 1.0f * s);
            name = Tr("WAITING") + std::string(size_t(int(now * 2.5) % 4), '.');
          }
          else
          {
            dl->AddRect(mn, mx, ui::Fade(ui::kWhite, 0.10f * a), 0.0f, ImDrawFlags_None, 1.0f * s);
          }
          dl->AddRectFilled(mn, ImVec2(mn.x + 2.0f * s, mx.y), ui::Fade(accent, a));
          ui::Caps(dl, FontKind::kLabel, ImVec2(mn.x + 18.0f * s, mn.y + 13.0f * s), ui::Fade(ui::kMuted, a), label);
          dl->PushClipRect(mn, ImVec2(mx.x - 40.0f * s, mx.y), true);
          ui::Text(dl, FontKind::kHeading, ImVec2(mn.x + 18.0f * s, mn.y + 34.0f * s),
                   ui::Fade(mode == Seat::kWaiting ? ui::kMuted : ui::kText, a), name.c_str());
          dl->PopClipRect();
          ImVec2 ic(mx.x - 24.0f * s, (mn.y + mx.y) * 0.5f);
          if (mode == Seat::kReady)
          {
            dl->AddCircleFilled(ic, 5.0f * s, ui::Fade(ui::kGreen, a));
            dl->AddCircle(ic, (5.0f + 5.0f * pulse) * s, ui::Fade(ui::kGreen, (1.0f - pulse) * a), 16, 1.0f * s);
          }
          else if (mode == Seat::kJoining)
          {
            ui::Spinner(dl, ic, 9.0f * s, 2.5f * s, ui::Fade(ui::kAccent, a));
          }
        };
        seat(cx0, Tr("PLAYER 1  ·  HOST"), LocalName(), Seat::kReady);
        std::string partner;
        for (const SentInvite &x : sent)
        {
          if (x.state == SentState::kAccepted)
          {
            partner = x.name;
          }
        }
        if (game.players >= 2)
        {
          seat(cx0 + cw + gap, Tr("PLAYER 2  ·  PARTNER"), partner.empty() ? Tr("PARTNER") : partner, Seat::kReady);
        }
        else if (!partner.empty())
        {
          seat(cx0 + cw + gap, Tr("PLAYER 2  ·  JOINING"), partner, Seat::kJoining);
        }
        else
        {
          seat(cx0 + cw + gap, Tr("PLAYER 2"), "", Seat::kWaiting);
        }
        y += ch + 26.0f * s;
      }

      // --- Invites to this player ----------------------------------------
      if (!received.empty())
      {
        section(Tr("INCOMING INVITES"), int(received.size()));
        for (const ReceivedInvite &inv : received)
        {
          float ch = 92.0f * s;
          ImVec2 mn(cx0, y), mx(cx1, y + ch);
          dl->AddRectFilled(mn, mx, ui::Fade(ui::kWhite, 0.05f * a));
          dl->AddRect(mn, mx, ui::Fade(ui::kAccent, (0.20f + 0.30f * pulse) * a), 0.0f, ImDrawFlags_None, 1.0f * s);
          dl->AddRectFilled(mn, ImVec2(mn.x + 2.0f * s, mx.y), ui::Fade(ui::kAccent, a));
          ui::Text(dl, FontKind::kHeading, ImVec2(mn.x + 20.0f * s, mn.y + 14.0f * s), ui::Fade(ui::kText, a),
                   inv.name.c_str());
          std::string sub = std::string(inv.is_private ? Tr("invites you to a private co-op game")
                                                       : Tr("invites you to a co-op game")) +
                            "  ·  " + IpString(inv.from.ip);
          ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + 20.0f * s, mn.y + 48.0f * s), ui::Fade(ui::kMuted, a),
                   sub.c_str());
          float frac = ui::Clamp01(1.0f - std::chrono::duration<float>(Clock::now() - inv.received).count() /
                                              std::chrono::duration<float>(kInviteLifetime).count());
          ImVec2 bar(mn.x + 2.0f * s, mx.y - 2.0f * s);
          dl->AddRectFilled(bar, ImVec2(bar.x + (mx.x - bar.x) * frac, mx.y), ui::Fade(ui::kAccent, 0.8f * a));
          float bw = 140.0f * s, bh = 38.0f * s, by = mn.y + (ch - bh) * 0.5f - 2.0f * s;
          std::string id = std::to_string(inv.id);
          if (ui::Button(("accept" + id).c_str(), ImVec2(mx.x - 20.0f * s - 2.0f * bw - 10.0f * s, by),
                         ImVec2(bw, bh), Tr("ACCEPT"), ui::ButtonStyle::kPrimary, live, a))
          {
            Answer(inv, true);
            BeginClose();
          }
          if (ui::Button(("decline" + id).c_str(), ImVec2(mx.x - 20.0f * s - bw, by), ImVec2(bw, bh), Tr("DECLINE"),
                         ui::ButtonStyle::kDanger, live, a))
          {
            Answer(inv, false);
          }
          y += ch + 10.0f * s;
        }
        y += 14.0f * s;
      }

      // --- Players found -------------------------------------------------
      section(Tr("PLAYERS ONLINE"), int(peers.size()));
      if (peers.empty())
      {
        // Radar: rings, a turning sweep with a fading trail.
        float rh = 112.0f * s;
        ImVec2 rc(cx0 + 56.0f * s, y + rh * 0.5f);
        float r = 46.0f * s;
        for (float k : {1.0f, 0.66f, 0.33f})
        {
          dl->AddCircle(rc, r * k, ui::Fade(ui::kWhite, 0.09f * a), 40, 1.0f * s);
        }
        dl->AddLine(ImVec2(rc.x - r, rc.y), ImVec2(rc.x + r, rc.y), ui::Fade(ui::kWhite, 0.06f * a), 1.0f * s);
        dl->AddLine(ImVec2(rc.x, rc.y - r), ImVec2(rc.x, rc.y + r), ui::Fade(ui::kWhite, 0.06f * a), 1.0f * s);
        float ang = float(now) * 2.2f;
        for (int i = 0; i < 8; ++i)
        {
          dl->PathLineTo(rc);
          dl->PathArcTo(rc, r, ang - 0.1f * float(i + 1), ang - 0.1f * float(i), 4);
          dl->PathFillConvex(ui::Fade(ui::kAccent, 0.22f * (1.0f - float(i) / 8.0f) * a));
        }
        dl->AddLine(rc, ImVec2(rc.x + std::cos(ang) * r, rc.y + std::sin(ang) * r), ui::Fade(ui::kAccent, a),
                    2.0f * s);
        dl->AddCircleFilled(rc, 3.0f * s, ui::Fade(ui::kAccent, a));
        float tx = cx0 + 130.0f * s;
        std::string searching = Tr("SEARCHING FOR PLAYERS") + std::string(size_t(int(now * 2.5) % 4), '.');
        ui::Text(dl, FontKind::kHeading, ImVec2(tx, y + 16.0f * s), ui::Fade(ui::kText, a), searching.c_str());
        ui::Text(dl, FontKind::kSmall, ImVec2(tx, y + 52.0f * s), ui::Fade(ui::kMuted, a),
                 Tr("Players on the LAN and Radmin VPN are found automatically."));
        ui::Text(dl, FontKind::kSmall, ImVec2(tx, y + 72.0f * s), ui::Fade(ui::kMuted, a),
                 Tr("Over the internet: the address below or ao2_friends in settings/online.toml."));
        y += rh + 6.0f * s;
      }
      for (size_t i = 0; i < peers.size(); ++i)
      {
        const Peer &p = peers[i];
        std::string key = p.name + "@" + IpString(p.address.ip);
        // Rows come in one after another when the window opens, and when
        // a player is found later.
        auto shown = row_shown.try_emplace(key, std::max(now, opened + 0.18 + 0.06 * double(i))).first->second;
        float rt = ui::EaseOutCubic(float(now - shown) / 0.35f);
        float ra = a * rt;
        float rh = 56.0f * s;
        float rx = cx0 + (1.0f - rt) * 36.0f * s;
        ImVec2 mn(rx, y), mx(rx + (cx1 - cx0), y + rh);

        bool hovered = live && ImGui::IsMouseHoveringRect(mn, mx);
        float &h = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID(("row" + key).c_str()), 0.0f);
        h = ui::Approach(h, hovered ? 1.0f : 0.0f, 14.0f);
        // As the launcher's rows: a faint lift and a paper bar on the left when hovered.
        dl->AddRectFilled(mn, mx, ui::Fade(ui::kWhite, (0.03f + 0.05f * h) * ra));
        dl->AddRectFilled(mn, ImVec2(mn.x + 2.0f * s, mx.y), ui::Fade(ui::kAccent, (0.25f + 0.75f * h) * ra));
        if (auto f = row_flash.find(key); f != row_flash.end())
        {
          float ft = 1.0f - float(now - f->second) / 0.7f;
          if (ft > 0.0f)
          {
            dl->AddRectFilled(mn, mx, ui::Fade(ui::kAccent, 0.25f * ft * ra));
          }
        }

        ImU32 dot = p.state == "game" ? ui::kGreen : p.state == "lobby" ? ui::kAccent : ui::kMuted;
        ImVec2 dc(mn.x + 24.0f * s, mn.y + rh * 0.5f);
        dl->AddCircleFilled(dc, 4.5f * s, ui::Fade(dot, ra));
        if (p.hosting)
        {
          dl->AddCircle(dc, (4.5f + 4.0f * pulse) * s, ui::Fade(dot, (1.0f - pulse) * ra), 16, 1.0f * s);
        }
        ui::Text(dl, FontKind::kBody, ImVec2(mn.x + 42.0f * s, mn.y + 8.0f * s), ui::Fade(ui::kText, ra),
                 p.name.c_str());
        std::string sub = IpString(p.address.ip) + "  ·  " + StateText(p.state) + (p.hosting ? Tr("  ·  hosting a game") : "");
        ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + 42.0f * s, mn.y + 32.0f * s), ui::Fade(ui::kMuted, ra),
                 sub.c_str());

        auto si = std::find_if(sent.begin(), sent.end(), [&](const SentInvite &x) { return x.to == p.address; });
        SentState state = si != sent.end() ? si->state : SentState::kFailed;
        bool have = si != sent.end();
        // Not joined 15 s after the invite (no reply, or accepted without
        // the game coming in): it can be sent again.
        bool stale = have && Clock::now() - si->since > kReinviteAfter &&
                     (state == SentState::kSending || state == SentState::kDelivered ||
                      (state == SentState::kAccepted && game.players < 2));
        float bw = 150.0f * s, bh = 34.0f * s;
        ImVec2 bp(mx.x - 12.0f * s - bw, mn.y + (rh - bh) * 0.5f);
        float mid = mn.y + rh * 0.5f;
        auto right_text = [&](const char *text, ImU32 col, float right) {
          ImVec2 ts = ui::CapsSize(FontKind::kLabel, text);
          ui::Caps(dl, FontKind::kLabel, ImVec2(right - ts.x, mid - ts.y * 0.5f), ui::Fade(col, ra), text);
        };
        if (have && !stale && (state == SentState::kSending || state == SentState::kDelivered))
        {
          ui::Spinner(dl, ImVec2(mx.x - 28.0f * s, mid), 8.0f * s, 2.5f * s, ui::Fade(ui::kAccent, ra));
          right_text(state == SentState::kSending ? Tr("SENDING") : Tr("AWAITING REPLY"), ui::kText, mx.x - 46.0f * s);
        }
        else if (have && !stale && state == SentState::kAccepted)
        {
          ui::Check(dl, ImVec2(mx.x - 28.0f * s, mid), 16.0f * s, 2.5f * s, ui::Fade(ui::kGreen, ra));
          right_text(Tr("ACCEPTED"), ui::kGreen, mx.x - 46.0f * s);
        }
        else
        {
          if (have)
          {
            right_text(state == SentState::kDeclined ? Tr("DECLINED")
                       : state == SentState::kAccepted ? Tr("NOT JOINED")
                                                       : Tr("NOT DELIVERED"),
                       state == SentState::kDeclined ? ui::kRed : ui::kMuted, bp.x - 14.0f * s);
          }
          if (ui::Button(("invite" + key).c_str(), bp, ImVec2(bw, bh), have ? Tr("AGAIN") : Tr("INVITE"),
                         ui::ButtonStyle::kPrimary, hosting && live && rt >= 1.0f, ra))
          {
            error.clear();
            if (Invite(p.address, p.name, &error))
            {
              row_flash[key] = now;
            }
          }
        }
        y += rh + 6.0f * s;
      }
      if (!hosting && !peers.empty())
      {
        ui::Text(dl, FontKind::kSmall, ImVec2(cx0, y + 2.0f * s), ui::Fade(ui::kMuted, a),
                 Tr("To invite, create a game: Campaign Mode → Private Co-op Game."));
        y += ui::FontSize(FontKind::kSmall) + 8.0f * s;
      }

      // --- By address ----------------------------------------------------
      y += 14.0f * s;
      section(Tr("BY ADDRESS"), -1);
      ImGui::PushFont(ui::Font(FontKind::kBody), ui::FontSize(FontKind::kBody));
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f * s, 8.0f * s));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * s);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::Fade(ui::kWhite, 0.05f));
      ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ui::Fade(ui::kWhite, 0.08f));
      ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ui::Fade(ui::kWhite, 0.10f));
      ImGui::PushStyleColor(ImGuiCol_Border, ui::Fade(ui::kAccent, 0.30f));
      ImGui::PushStyleColor(ImGuiCol_Text, ui::kText);
      ImGui::PushStyleColor(ImGuiCol_TextDisabled, ui::kMuted);
      ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ui::Fade(ui::kAccent, 0.35f));
      ImGui::SetCursorScreenPos(ImVec2(cx0, y));
      float field_w = 320.0f * s;
      ImGui::SetNextItemWidth(field_w);
      bool enter = ImGui::InputTextWithHint("##address", Tr("IP or IP:port"), address, sizeof(address),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
      float field_h = ImGui::GetItemRectSize().y;
      ImGui::PopStyleColor(7);
      ImGui::PopStyleVar(4);
      ImGui::PopFont();
      if ((ui::Button("find", ImVec2(cx0 + field_w + 10.0f * s, y), ImVec2(120.0f * s, field_h), Tr("FIND"),
                      ui::ButtonStyle::kOutline, live, a) ||
           enter) &&
          live)
      {
        Address addr;
        if (ParseAddress(address, &addr))
        {
          std::lock_guard lock(st.mutex);
          if (std::find(st.manual.begin(), st.manual.end(), addr) == st.manual.end())
          {
            st.manual.push_back(addr);
          }
          st.next_beacon = Clock::now();
          error.clear();
        }
        else
        {
          error = Tr("Couldn't read the address.");
        }
      }
      y += field_h + 10.0f * s;
      if (!error.empty())
      {
        ui::Text(dl, FontKind::kSmall, ImVec2(cx0, y), ui::Fade(ui::kRed, a), error.c_str());
        y += ui::FontSize(FontKind::kSmall) + 8.0f * s;
      }

      // --- Footer --------------------------------------------------------
      y += 10.0f * s;
      dl->AddLine(ImVec2(cx0, y), ImVec2(cx1, y), ui::Fade(ui::kWhite, 0.08f * a), 1.0f * s);
      y += 14.0f * s;
      char port[96];
      std::snprintf(port, sizeof(port), Tr("UDP %u must be open on both PCs (firewall)"), unsigned(Port()));
      float kh = 24.0f * s;
      ui::Text(dl, FontKind::kSmall, ImVec2(cx0, y + 4.0f * s), ui::Fade(ui::kMuted, a), port);
      {
        const char *label = Tr("CLOSE");
        float label_w = ui::CapsSize(FontKind::kLabel, label).x;
        float key_w = std::max(kh, ui::CapsSize(FontKind::kLabel, "ESC").x + kh * 0.8f); // as ui::KeyCap
        float total = key_w + 8.0f * s + label_w;
        ImVec2 pos(cx1 - total, y);
        ImGui::SetCursorScreenPos(pos);
        if (ImGui::InvisibleButton("##close", ImVec2(total, kh)) && live)
        {
          BeginClose();
        }
        bool hovered = ImGui::IsItemHovered();
        if (hovered)
        {
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        ui::KeyCap(dl, pos, kh, "ESC", a);
        ui::Caps(dl, FontKind::kLabel, ImVec2(pos.x + key_w + 8.0f * s, y + (kh - ui::FontSize(FontKind::kLabel)) * 0.5f - 1.0f * s),
                 ui::Fade(hovered ? ui::kAccentLight : ui::kText, a), label);
      }
      y += kh + pad;

      // The panel grows / shrinks smoothly to its content.
      float target = y - y0;
      panel_h = panel_h <= 0.0f ? target : ui::Approach(panel_h, target, 18.0f);

      // --- Behind: backdrop and panel ------------------------------------
      dl->ChannelsSetCurrent(0);
      // A deep navy wash over the game, then the panel: navy with a hairline frame (as the launcher's).
      ImVec2 vmn = vp->Pos, vmx(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
      dl->AddRectFilled(vmn, vmx, ui::Fade(ui::kInk, 0.78f * a));

      float y1 = y0 + panel_h;
      dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ui::Fade(ui::kPanel, 0.96f * a));
      dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), ui::Fade(ui::kWhite, 0.13f * a), 0.0f, ImDrawFlags_None, 1.0f * s);

      dl->ChannelsMerge();
      ImGui::End();
      return true;
    }
  };

  // The last players window opened (TogglePlayers).
  std::mutex g_players_window_mutex;
  std::weak_ptr<PlayersWindow> g_players_window;

  // -------------------------------------------------------------------------
  // Guest memory
  // -------------------------------------------------------------------------

  void Store32(uint8_t *base, uint32_t addr, uint32_t v)
  {
    v = std::byteswap(v);
    std::memcpy(base + addr, &v, 4);
  }

  void Store64(uint8_t *base, uint32_t addr, uint64_t v)
  {
    v = std::byteswap(v);
    std::memcpy(base + addr, &v, 8);
  }

  uint64_t Load64(const uint8_t *base, uint32_t addr)
  {
    uint64_t v;
    std::memcpy(&v, base + addr, 8);
    return std::byteswap(v);
  }
} // namespace

namespace ao2::invite
{
  void Start()
  {
    if (!REXCVAR_GET(ao2_invites) || g_started.exchange(true))
    {
      return;
    }
    WSADATA wsa = {};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
    {
      Log("cannot create the socket (error %d)", WSAGetLastError());
      return;
    }
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&on), sizeof(on));
    // Port unreachable answers to earlier sends must not end recvfrom.
    DWORD no = 0, ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &no, sizeof(no), nullptr, 0, &ret, nullptr, nullptr);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    std::string bind_ip = rex::cvar::GetFlagByName("net_bind_ip");
    addr.sin_addr.s_addr = INADDR_ANY;
    if (!bind_ip.empty())
    {
      inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr);
    }
    addr.sin_port = htons(Port());
    if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
      // Another game on this PC has the port: any port, found through our
      // own announcements (answers come back to it).
      Log("UDP %u taken (error %d), using another port", unsigned(Port()), WSAGetLastError());
      addr.sin_port = 0;
      if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
      {
        Log("cannot bind (error %d)", WSAGetLastError());
        closesocket(s);
        return;
      }
    }
    int len = sizeof(addr);
    getsockname(s, reinterpret_cast<sockaddr *>(&addr), &len);
    state().socket = s;
    Log("presence on UDP %s:%u as '%s'", IpString(addr.sin_addr.s_addr).c_str(), ntohs(addr.sin_port),
        LocalName().c_str());
    std::thread(Loop).detach();
  }

  void ShowPlayers()
  {
    if (!g_started)
    {
      return;
    }
    {
      std::lock_guard lock(state().mutex);
      state().window_open = true;
      state().next_beacon = Clock::now();
    }
    auto window = std::make_shared<PlayersWindow>();
    {
      std::lock_guard lock(g_players_window_mutex);
      g_players_window = window;
    }
    ao2::menu::OpenWindow("players", [window]() { return window->Draw(); }, /*own_escape=*/true);
  }

  void TogglePlayers()
  {
    std::shared_ptr<PlayersWindow> window;
    {
      std::lock_guard lock(g_players_window_mutex);
      window = g_players_window.lock();
    }
    bool open;
    {
      std::lock_guard lock(state().mutex);
      open = state().window_open;
    }
    // A window already closing (Esc, Close, the key) opens again. The key
    // comes on the UI thread, where the window draws (closing).
    if (open && window && window->closing < 0.0 && !window->close_requested.exchange(true))
    {
      return;
    }
    ShowPlayers();
  }

  bool HasPendingInvite()
  {
    std::lock_guard lock(state().mutex);
    return !state().received.empty();
  }

  void AcceptPendingInvite()
  {
    ReceivedInvite inv;
    {
      std::lock_guard lock(state().mutex);
      if (state().received.empty())
      {
        return;
      }
      inv = state().received.back();
    }
    Answer(inv, true);
  }
} // namespace ao2::invite

// ---------------------------------------------------------------------------
// Guest hooks
// ---------------------------------------------------------------------------

// XamShowGameInviteUI(user, recipients, count, text) - the guide's "invite
// friends" list (UE3 ShowInviteUI, from the lobby's Invite button).
extern "C" REX_FUNC(__imp__sub_82B07EB0);
extern "C" REX_FUNC(sub_82B07EB0)
{
  if (!g_started)
  {
    __imp__sub_82B07EB0(ctx, base);
    return;
  }
  Log("game asks for the invite UI (user %u)", ctx.r3.u32);
  ao2::invite::ShowPlayers();
  ctx.r3.u64 = 0; // ERROR_SUCCESS
}

// XamShowFriendsUI(user) - the guide's friends list.
extern "C" REX_FUNC(__imp__sub_82B07E60);
extern "C" REX_FUNC(sub_82B07E60)
{
  if (!g_started)
  {
    __imp__sub_82B07E60(ctx, base);
    return;
  }
  Log("game asks for the friends UI (user %u)", ctx.r3.u32);
  ao2::invite::ShowPlayers();
  ctx.r3.u64 = 0;
}

// XInviteGetAcceptedInfo(user, XINVITE_INFO *info): the last accepted invite.
// XINVITE_INFO: +0 xuidInvitee, +8 xuidInviter, +16 dwTitleID, +20
// XSESSION_INFO (+20 XNKID, +28 XNADDR, +64 XNKEY), +80 fFromGameInvite.
extern "C" REX_FUNC(__imp__sub_82C41388);
extern "C" REX_FUNC(sub_82C41388)
{
  ReceivedInvite inv;
  {
    std::lock_guard lock(state().mutex);
    if (!state().have_accepted)
    {
      __imp__sub_82C41388(ctx, base);
      return;
    }
    inv = state().accepted;
  }
  uint32_t user = ctx.r3.u32;
  uint32_t info = ctx.r4.u32;
  // The invitee must be the signed-in player (EA's layer compares the XUID
  // it gets the same way, sub_82236E30): XUSER_SIGNIN_INFO into the buffer.
  ctx.r3.u64 = user;
  ctx.r4.u64 = 5;
  ctx.r5.u64 = info;
  sub_82B07E10(ctx, base);
  uint64_t invitee = ctx.r3.u32 == 0 ? Load64(base, info) : 0;
  std::memset(base + info, 0, 84);
  Store64(base, info + 0, invitee);
  Store64(base, info + 8, inv.inviter_xuid);
  auto *ks = rex::system::kernel_state();
  Store32(base, info + 16, ks ? ks->title_id() : 0);
  Store64(base, info + 20, inv.session_id);
  std::memcpy(base + info + 28, inv.xnaddr.data(), 36);
  Store32(base, info + 80, 1);
  Log("XInviteGetAcceptedInfo(user %u): invitee %016llx, inviter %s %016llx, session %016llx", user,
      (unsigned long long)invitee, inv.name.c_str(), (unsigned long long)inv.inviter_xuid,
      (unsigned long long)inv.session_id);
  ctx.r3.u64 = 0;
}

// Joining the inviter after an accepted invite: the online state machine
// (sub_82A2B340) calls sub_82A27E18(this, &inviter_xuid), which asks the game
// manager (EA root +212) to join the inviter's game (vfunc +32, sub_8223B940).
// A versus party (Custom Match) is a playgroup on the inviter's side, peer to
// peer; joined as a game, the two sides connect differently and the host's
// playgroup packets find no handler here. Join it with the playgroup manager
// (EA root +220, vfunc +36, sub_8227C638: the XUID by value) instead.
extern "C" REX_FUNC(__imp__sub_82A27E18);
extern "C" REX_FUNC(sub_82A279A8);
extern "C" REX_FUNC(sub_8227C638);
extern "C" REX_FUNC(sub_82A27E18)
{
  ReceivedInvite inv;
  bool playgroup = false;
  {
    std::lock_guard lock(state().mutex);
    if (state().have_accepted && state().accepted.playgroup)
    {
      inv = state().accepted;
      playgroup = true;
    }
  }
  uint64_t xuid = Load64(base, ctx.r4.u32);
  if (!playgroup || xuid != inv.inviter_xuid)
  {
    __imp__sub_82A27E18(ctx, base);
    return;
  }
  sub_82A279A8(ctx, base); // join parameters, as the original passes them
  uint32_t params = ctx.r3.u32;
  uint32_t root = uint32_t(Load64(base, 0x8321D0D4) >> 32);
  uint32_t manager = uint32_t(Load64(base, root + 220) >> 32);
  Log("joining %s's versus party %016llx as a playgroup (manager %08X)", inv.name.c_str(),
      (unsigned long long)xuid, manager);
  ctx.r3.u64 = manager;
  ctx.r4.u64 = xuid;
  ctx.r5.u64 = params;
  ctx.r6.u64 = 0;
  sub_8227C638(ctx, base);
}
