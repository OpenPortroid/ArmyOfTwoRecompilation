// steam_net.cpp - Online play over Steam (Spacewar, app 480).
//
// The game's online play (ea_server.cpp, coop_invite.cpp, the VDP game
// traffic) talks IPv4 to fixed ports: the lobby server TCP 18480 / 18485 /
// 13505, the game UDP 1000, the link to the partner UDP 1002 (coop_link.cpp),
// the presence service UDP ao2_invite_port. Over Steam it keeps doing that on
// a virtual LAN on this PC's loopback:
//
// - Every Steam player has an address in 127.64.0.0/10 made from the
//   SteamID (IsVirtual / VirtualIp), the same on every PC; Windows takes the
//   whole 127/8 as loopback, nothing has to be set up.
// - For every Steam player it knows (a peer) this bridge binds the peer's
//   address on those ports: UDP 1000, the link's and the presence port, TCP
//   listeners on the lobby server's ports. A UDP port without such a socket
//   would land in this game's own socket on 0.0.0.0. Windows lets a socket bound to one address
//   share a port with the game's own socket bound to all of them (0.0.0.0).
//   What the game sends to a peer's address lands here and goes to that
//   player through Steam: UDP as ISteamNetworkingMessages (channel 0, the
//   ports in a 4-byte header), a TCP connection as an ISteamNetworkingSockets
//   P2P connection to the same virtual port.
// - On the far side the bridge passes it on from a socket bound to the
//   sender's address to this PC's own virtual address (the game's sockets on
//   0.0.0.0 get it): the game, the presence service and the lobby server
//   see each player at its own address, and the lobby server tells the
//   others that address (ea_server.cpp PlayerIpFor, RewriteXnAddr: virtual
//   addresses are not "loopback" there).
//
// Players are found three ways: friends playing this game (rich presence
// "ao2" = the build), members of a Steam lobby this game is in, and anyone
// whose packets or connections arrive. The presence service announces itself
// to all of them, so Steam friends show in the players window as on a LAN and
// its invites work unchanged.
//
// Lobbies: a game this player hosts (ea::FindHostedGame, passed in by the
// presence service: SetHosting) gets a Steam lobby (public, friends only for
// a private game) with "ao2" = 1, name, build, mode and player count - the
// server list in the players window. A player that enters it (the list's
// JOIN, or Steam's "Join game" / a lobby invite: GameLobbyJoinRequested) is
// sent the game's invite by the host (ao2::invite::InviteAddress), and its
// game accepts it without asking (TakeAutoAccept). A joined lobby is left once
// its game is over for this player, or was never entered: it would keep a
// place of the host's lobby taken (UpdateJoinedLobby).
//
// The list shows each lobby's mission, mode, free places and ping: the host
// puts its ping location (ISteamNetworkingUtils, "loc") in the lobby, the
// others estimate the ping from it without connecting. A public quick match
// of the game that finds nothing on its own server (ea_server.cpp GLST,
// QuickMatch) joins the open lobby of its mode and build with the lowest
// ping the same way; with none the game hosts as before.
//
// steam.log (with log_to_files) shows peers, lobbies and connections.
#include "steam_net.h"

#include "coop_invite.h"
#include "coop_link.h"
#include "ea_server.h"
#include "log_files.h"
#include "ui_text.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <span>
#include <utility>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(ao2_steam, true, "Online",
                    "Online play over Steam (needs the Steam client running): Steam friends in the game are found "
                    "and invited as on a LAN, the players window lists the games hosted over Steam, and all "
                    "traffic goes through Steam's relays (no VPN, no forwarded ports).");
REXCVAR_DEFINE_BOOL(ao2_steam_name, false, "Online",
                    "The player's name online is their Steam name (net_gamertag is replaced once Steam has started). "
                    "Without Steam, or off: net_gamertag (the launcher's Player name).");
REXCVAR_DECLARE(uint32_t, ao2_invite_port);

namespace ao2::steam
{
  using ao2::text::Tr;

  // The server list's mission: a versus map (UIDataProvider_AO2MPMaps:
  // M01 Iraq, M02 China, m03 Afghanistan, M04 Somalia), a campaign level by
  // its number (as the F1 menu's checkpoints), else its file name. A co-op
  // game's B-U-Map is the travel URL of its checkpoint
  // ("Checkpoint?LoadSaveGame?CheckpointToLoad%3d09_100?Difficulty%3d0"):
  // the checkpoint's number.
  std::string MissionName(const std::string &map, bool versus)
  {
    std::string m = map;
    std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char *key : {"checkpointtoload%3d", "checkpointtoload="})
    {
      if (size_t at = m.find(key); at != std::string::npos)
      {
        m = m.substr(at + std::strlen(key));
        m = m.substr(0, m.find('?'));
        break;
      }
    }
    if (m.empty() || m == "any")
    {
      return Tr("Any");
    }
    static const std::pair<const char *, const char *> kVersus[] = {
        {"m01", "Iraq"}, {"m02", "China"}, {"m03", "Afghanistan"}, {"m04", "Somalia"}};
    static const std::pair<const char *, const char *> kCampaign[] = {
        {"01", "Somalia"}, {"04", "Afghanistan"}, {"05", "Iraq"}, {"06", "Aircraft carrier"},
        {"07", "China"},   {"08", "Miami"},       {"09", "Kiev (DLC)"}, {"10", "Miami, alternate ending (DLC)"}};
    using Names = std::span<const std::pair<const char *, const char *>>;
    for (const auto &[prefix, name] : versus ? Names(kVersus) : Names(kCampaign))
    {
      if (m.starts_with(prefix))
      {
        return Tr(name);
      }
    }
    if (size_t cut = m.find("_map_world"); cut != std::string::npos)
    {
      return m.substr(0, cut);
    }
    return m;
  }

  const char *DifficultyName(int difficulty)
  {
    switch (difficulty)
    {
    case 0:
      return Tr("Recruit");
    case 1:
      return Tr("Contractor");
    case 2:
      return Tr("Professional");
    default:
      return "";
    }
  }

  // The list's mode: co-op, or the versus game type (UIDataProvider_AO2MPGameTypes).
  const char *ModeName(bool versus, int game_type)
  {
    if (!versus)
    {
      return Tr("co-op");
    }
    switch (game_type)
    {
    case 0:
      return Tr("Warzone");
    case 1:
      return Tr("Bounties");
    case 2:
      return Tr("Extraction");
    default:
      return Tr("versus");
    }
  }

  namespace
  {
    // Crockford's base 32: no I, L, O or U; read leniently (O as 0, I and L
    // as 1).
    constexpr char kCodeDigits[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  } // namespace

  std::string EncodeCode(uint32_t account)
  {
    std::string code(7, '0');
    for (int i = 6; i >= 0; --i, account >>= 5)
    {
      code[size_t(i)] = kCodeDigits[account & 31];
    }
    return code.substr(0, 3) + "-" + code.substr(3);
  }

  bool DecodeCode(const std::string &text, uint32_t &account)
  {
    uint64_t v = 0;
    int digits = 0;
    for (char c : text)
    {
      c = char(std::toupper(static_cast<unsigned char>(c)));
      if (c == '-' || c == ' ' || c == '\t')
      {
        continue;
      }
      c = c == 'O' ? '0' : (c == 'I' || c == 'L') ? '1' : c;
      const char *at = std::strchr(kCodeDigits, c);
      if (!c || !at)
      {
        return false;
      }
      v = v * 32 + uint64_t(at - kCodeDigits);
      ++digits;
    }
    if (digits != 7 || v > 0xFFFFFFFFull || v == 0)
    {
      return false;
    }
    account = uint32_t(v);
    return true;
  }
} // namespace ao2::steam

#ifdef AO2_STEAM

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
// clang-format on

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

// The SDK's headers use strncpy.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <steam/steam_api.h>
#pragma clang diagnostic pop

namespace
{
  using Clock = std::chrono::steady_clock;

  constexpr uint16_t kTcpPorts[] = {18480, 18485, 13505}; // the lobby server (FESL, Theater, Messenger)
  constexpr uint16_t kGamePort = 1000;                    // VDP
  constexpr const char *kAppId = "480";

  // -------------------------------------------------------------------------
  // Log (steam.log)
  // -------------------------------------------------------------------------

  void Log(const char *fmt, ...)
  {
    static std::mutex mutex;
    static FILE *f = ao2::OpenLogFile("steam.log");
    if (!f)
    {
      return;
    }
    std::lock_guard lock(mutex);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fputc('\n', f);
    std::fflush(f);
  }

  std::string IpString(uint32_t ip)
  {
    char buf[INET_ADDRSTRLEN] = {};
    in_addr a = {};
    a.s_addr = ip;
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
  }

  // 127.64.0.0/10 + 22 bits of the account id, mixed (network order).
  uint32_t VirtualIp(uint64_t steam_id)
  {
    uint32_t h = uint32_t(steam_id) * 2654435761u;
    h ^= h >> 15;
    uint32_t host = (127u << 24) | (1u << 22) | (h & 0x3FFFFF);
    if ((host & 0xFF) == 0 || (host & 0xFF) == 255)
    {
      host ^= 1;
    }
    return htonl(host);
  }

  uint16_t PresencePort()
  {
    return uint16_t(REXCVAR_GET(ao2_invite_port));
  }

  SteamNetworkingIdentity Identity(uint64_t steam_id)
  {
    SteamNetworkingIdentity id;
    id.SetSteamID64(steam_id);
    return id;
  }

  void NonBlocking(SOCKET s)
  {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
  }

  sockaddr_in Sockaddr(uint32_t ip, uint16_t port)
  {
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = htons(port);
    return a;
  }

  // -------------------------------------------------------------------------
  // Shared with the other threads
  // -------------------------------------------------------------------------

  struct Shared
  {
    std::mutex mutex;
    std::vector<uint32_t> peer_ips;
    std::map<uint32_t, uint64_t> peer_ids; // virtual address -> SteamID
    uint64_t profile = 0;                  // a Steam profile to show in the overlay (OpenProfile)
    uint32_t self_ip = 0;
    ao2::steam::Hosting hosting;
    std::map<uint32_t, Clock::time_point> auto_accept; // host address -> until
    uint32_t auto_accepted = 0;                        // invites taken so (AutoAccepted)
    std::vector<ao2::steam::Lobby> lobbies;
    bool refresh = false;
    bool refreshing = false;
    uint64_t join = 0;
    uint64_t lobby = 0; // the lobby this game is in
    bool own_lobby = false; // ... it hosts
    // A joined lobby whose game this one is going to or is in. False once that game is left: the lobby is kept a
    // little longer (a versus party between its screens), but the lists show it as joinable again.
    bool joined_active = false;
    Clock::time_point viewed; // a list was on the screen (Lobbies): its rows' data is asked for again
    bool invite_dialog = false;
    // A quick match (QuickMatch): asked -> lobby list -> joining -> the
    // invite accepted, or failed.
    enum class Quick
    {
      kNone,
      kWanted,
      kSearching,
      kJoining,
      kAccepted,
      kFailed,
    } quick = Quick::kNone;
    std::string quick_mode;
    uint64_t quick_lobby = 0;
    std::string persona; // this player's Steam name
    uint32_t account = 0; // ... and account (JoinCode)
    // A join by code (JoinByCode): the host's account asked for, the state.
    uint32_t code_join = 0;
    ao2::steam::CodeJoin code_state = ao2::steam::CodeJoin::kNone;
    std::string code_host;
  };

  Shared &shared()
  {
    static Shared s;
    return s;
  }

  std::atomic<bool> g_running{false};

  // -------------------------------------------------------------------------
  // The bridge (Steam thread only)
  // -------------------------------------------------------------------------

  // A TCP connection carried over a Steam P2P connection.
  struct Tunnel
  {
    SOCKET local = INVALID_SOCKET;
    HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
    uint64_t peer = 0;
    uint16_t port = 0;
    bool steam_up = false;    // the Steam connection is connected
    bool local_up = false;    // the local socket is connected
    std::string to_steam;     // read locally before the Steam connection was up
    std::string to_local;     // received over Steam, not written yet
    bool closing = false;     // the far side closed: write out to_local, then close
  };

  struct Peer
  {
    uint64_t id = 0;
    uint32_t ip = 0;
    std::string name;
    std::map<uint16_t, SOCKET> udp; // bound to ip:port
    std::vector<std::pair<SOCKET, uint16_t>> listeners;
  };

  class Bridge
  {
  public:
    bool Init()
    {
      self_ = SteamUser()->GetSteamID().ConvertToUint64();
      self_ip_ = VirtualIp(self_);
      {
        std::lock_guard lock(shared().mutex);
        shared().self_ip = self_ip_;
        shared().peer_ids[self_ip_] = self_;
      }
      SteamNetworkingUtils()->InitRelayNetworkAccess();
      for (uint16_t port : kTcpPorts)
      {
        HSteamListenSocket ls = SteamNetworkingSockets()->CreateListenSocketP2P(port, 0, nullptr);
        listen_[ls] = port;
      }
      Log("Steam: %s (%llu), this PC is %s", SteamFriends()->GetPersonaName(), (unsigned long long)self_,
          IpString(self_ip_).c_str());
      SteamFriends()->SetRichPresence("ao2", "1");
      return true;
    }

    // Known from now on: its sockets bound, its address announced to.
    Peer *AddPeer(uint64_t id)
    {
      if (!id || id == self_)
      {
        return nullptr;
      }
      if (auto it = peers_.find(id); it != peers_.end())
      {
        return &it->second;
      }
      Peer &p = peers_[id];
      p.id = id;
      p.ip = VirtualIp(id);
      p.name = SteamFriends()->GetFriendPersonaName(CSteamID(id));
      for (uint16_t port : {kGamePort, ao2::coop::link::kPort, PresencePort()})
      {
        Udp(p, port);
      }
      for (uint16_t port : kTcpPorts)
      {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a = Sockaddr(p.ip, port);
        if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || listen(s, 8) != 0)
        {
          Log("peer %s: TCP %s:%u not bound (error %d)", p.name.c_str(), IpString(p.ip).c_str(), port,
              WSAGetLastError());
          if (s != INVALID_SOCKET)
          {
            closesocket(s);
          }
          continue;
        }
        NonBlocking(s);
        p.listeners.push_back({s, port});
      }
      Log("peer %s (%llu) at %s", p.name.c_str(), (unsigned long long)id, IpString(p.ip).c_str());
      std::lock_guard lock(shared().mutex);
      shared().peer_ips.push_back(p.ip);
      shared().peer_ids[p.ip] = id;
      return &p;
    }

    // The peer's UDP socket on `port` (bound to the peer's address).
    SOCKET Udp(Peer &p, uint16_t port)
    {
      if (auto it = p.udp.find(port); it != p.udp.end())
      {
        return it->second;
      }
      SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      sockaddr_in a = Sockaddr(p.ip, port);
      if (s != INVALID_SOCKET && bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0)
      {
        Log("peer %s: UDP %s:%u not bound (error %d)", p.name.c_str(), IpString(p.ip).c_str(), port,
            WSAGetLastError());
        closesocket(s);
        s = INVALID_SOCKET;
      }
      if (s != INVALID_SOCKET)
      {
        NonBlocking(s);
        DWORD no = 0, ret = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &no, sizeof(no), nullptr, 0, &ret, nullptr, nullptr);
      }
      p.udp[port] = s;
      return s;
    }

    uint32_t SelfIp() const { return self_ip_; }
    uint64_t Self() const { return self_; }

    // One pass: local sockets, then Steam.
    void Pump()
    {
      PumpLocal();
      PumpMessages();
      PumpTunnels();
    }

    // A P2P connection's state changed (SteamAPI_RunCallbacks).
    void OnStatus(SteamNetConnectionStatusChangedCallback_t *e)
    {
      HSteamNetConnection conn = e->m_hConn;
      uint64_t peer = e->m_info.m_identityRemote.GetSteamID64();
      switch (e->m_info.m_eState)
      {
      case k_ESteamNetworkingConnectionState_Connecting:
        if (auto ls = listen_.find(e->m_info.m_hListenSocket); ls != listen_.end())
        {
          // A peer's game connects to our lobby server: connect to it from
          // the peer's address.
          Peer *p = AddPeer(peer);
          SOCKET s = p ? socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) : INVALID_SOCKET;
          sockaddr_in from = Sockaddr(p ? p->ip : 0, 0);
          if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr *>(&from), sizeof(from)) != 0 ||
              SteamNetworkingSockets()->AcceptConnection(conn) != k_EResultOK)
          {
            Log("TCP %u from %llu refused", ls->second, (unsigned long long)peer);
            if (s != INVALID_SOCKET)
            {
              closesocket(s);
            }
            SteamNetworkingSockets()->CloseConnection(conn, 0, nullptr, false);
            return;
          }
          NonBlocking(s);
          sockaddr_in to = Sockaddr(self_ip_, ls->second);
          connect(s, reinterpret_cast<sockaddr *>(&to), sizeof(to));
          auto t = std::make_unique<Tunnel>();
          t->local = s;
          t->conn = conn;
          t->peer = peer;
          t->port = ls->second;
          tunnels_.push_back(std::move(t));
          Log("TCP %u from %s: accepted", ls->second, p->name.c_str());
        }
        break;
      case k_ESteamNetworkingConnectionState_Connected:
        if (Tunnel *t = Find(conn))
        {
          t->steam_up = true;
        }
        break;
      case k_ESteamNetworkingConnectionState_ClosedByPeer:
      case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        if (Tunnel *t = Find(conn))
        {
          Log("TCP %u with %llu: Steam connection ended (%d, %s)", t->port, (unsigned long long)t->peer,
              e->m_info.m_eEndReason, e->m_info.m_szEndDebug);
          ReceiveTunnel(*t); // what arrived before the end
          t->closing = true;
        }
        SteamNetworkingSockets()->CloseConnection(conn, 0, nullptr, false);
        break;
      default:
        break;
      }
    }

  private:
    Tunnel *Find(HSteamNetConnection conn)
    {
      for (auto &t : tunnels_)
      {
        if (t->conn == conn)
        {
          return t.get();
        }
      }
      return nullptr;
    }

    void PumpLocal()
    {
      std::vector<WSAPOLLFD> fds;
      struct Ref
      {
        Peer *peer;
        uint16_t port;
        bool listener;
      };
      std::vector<Ref> refs;
      for (auto &[id, p] : peers_)
      {
        for (auto &[port, s] : p.udp)
        {
          if (s != INVALID_SOCKET)
          {
            fds.push_back({s, POLLRDNORM, 0});
            refs.push_back({&p, port, false});
          }
        }
        for (auto &[s, port] : p.listeners)
        {
          fds.push_back({s, POLLRDNORM, 0});
          refs.push_back({&p, port, true});
        }
      }
      size_t first_tunnel = fds.size();
      for (auto &t : tunnels_)
      {
        SHORT ev = t->closing ? 0 : POLLRDNORM;
        if (!t->local_up || !t->to_local.empty())
        {
          ev |= POLLWRNORM;
        }
        fds.push_back({t->local, ev, 0});
      }
      if (fds.empty())
      {
        Sleep(2);
        return;
      }
      if (WSAPoll(fds.data(), ULONG(fds.size()), 2) <= 0)
      {
        return;
      }
      static char buf[65536];
      for (size_t i = 0; i < first_tunnel; ++i)
      {
        if (!(fds[i].revents & (POLLRDNORM | POLLERR)))
        {
          continue;
        }
        Peer &p = *refs[i].peer;
        uint16_t port = refs[i].port;
        if (refs[i].listener)
        {
          SOCKET s = accept(fds[i].fd, nullptr, nullptr);
          if (s == INVALID_SOCKET)
          {
            continue;
          }
          NonBlocking(s);
          BOOL on = TRUE;
          setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof(on));
          SteamNetworkingIdentity id = Identity(p.id);
          auto t = std::make_unique<Tunnel>();
          t->local = s;
          t->local_up = true;
          t->conn = SteamNetworkingSockets()->ConnectP2P(id, port, 0, nullptr);
          t->peer = p.id;
          t->port = port;
          Log("TCP %u to %s: connecting", port, p.name.c_str());
          tunnels_.push_back(std::move(t));
          continue;
        }
        // UDP: as many datagrams as are waiting.
        for (int n = 0; n < 64; ++n)
        {
          sockaddr_in from = {};
          int from_len = sizeof(from);
          int got = recvfrom(fds[i].fd, buf + 4, int(sizeof(buf) - 4), 0, reinterpret_cast<sockaddr *>(&from),
                             &from_len);
          if (got < 0)
          {
            break;
          }
          uint16_t src = ntohs(from.sin_port);
          std::memcpy(buf, &src, 2);
          std::memcpy(buf + 2, &port, 2);
          SteamNetworkingIdentity id = Identity(p.id);
          SteamNetworkingMessages()->SendMessageToUser(
              id, buf, uint32_t(got + 4),
              k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_AutoRestartBrokenSession, 0);
        }
      }
      for (size_t i = first_tunnel; i < fds.size(); ++i)
      {
        Tunnel &t = *tunnels_[i - first_tunnel];
        if (!t.local_up && (fds[i].revents & POLLWRNORM))
        {
          t.local_up = true;
          BOOL on = TRUE;
          setsockopt(t.local, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof(on));
        }
        if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
        {
          if (!(fds[i].revents & POLLRDNORM))
          {
            Close(t, "local socket error");
            continue;
          }
        }
        if (fds[i].revents & POLLRDNORM)
        {
          int got = recv(t.local, buf, sizeof(buf), 0);
          if (got <= 0)
          {
            if (got == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
            {
              Close(t, "closed locally");
            }
            continue;
          }
          if (t.steam_up)
          {
            SteamNetworkingSockets()->SendMessageToConnection(t.conn, buf, uint32_t(got),
                                                              k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
          }
          else
          {
            t.to_steam.append(buf, size_t(got));
          }
        }
      }
    }

    // UDP from the peers: passed on from the sender's address.
    void PumpMessages()
    {
      SteamNetworkingMessage_t *msgs[64];
      int n = SteamNetworkingMessages()->ReceiveMessagesOnChannel(0, msgs, 64);
      for (int i = 0; i < n; ++i)
      {
        SteamNetworkingMessage_t *m = msgs[i];
        if (m->m_cbSize >= 4)
        {
          if (Peer *p = AddPeer(m->m_identityPeer.GetSteamID64()))
          {
            uint16_t src, dst;
            std::memcpy(&src, m->m_pData, 2);
            std::memcpy(&dst, static_cast<const char *>(m->m_pData) + 2, 2);
            SOCKET s = Udp(*p, src);
            if (s != INVALID_SOCKET)
            {
              sockaddr_in to = Sockaddr(self_ip_, dst);
              sendto(s, static_cast<const char *>(m->m_pData) + 4, m->m_cbSize - 4, 0,
                     reinterpret_cast<sockaddr *>(&to), sizeof(to));
            }
          }
        }
        m->Release();
      }
    }

    void ReceiveTunnel(Tunnel &t)
    {
      SteamNetworkingMessage_t *msgs[32];
      int n;
      while ((n = SteamNetworkingSockets()->ReceiveMessagesOnConnection(t.conn, msgs, 32)) > 0)
      {
        for (int i = 0; i < n; ++i)
        {
          t.to_local.append(static_cast<const char *>(msgs[i]->m_pData), msgs[i]->m_cbSize);
          msgs[i]->Release();
        }
      }
    }

    void PumpTunnels()
    {
      for (auto &tp : tunnels_)
      {
        Tunnel &t = *tp;
        if (t.local == INVALID_SOCKET)
        {
          continue;
        }
        if (t.steam_up && !t.to_steam.empty())
        {
          SteamNetworkingSockets()->SendMessageToConnection(t.conn, t.to_steam.data(), uint32_t(t.to_steam.size()),
                                                            k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
          t.to_steam.clear();
        }
        if (!t.closing)
        {
          ReceiveTunnel(t);
        }
        if (t.local_up && !t.to_local.empty())
        {
          int sent = send(t.local, t.to_local.data(), int(std::min<size_t>(t.to_local.size(), 1 << 20)), 0);
          if (sent > 0)
          {
            t.to_local.erase(0, size_t(sent));
          }
          else if (WSAGetLastError() != WSAEWOULDBLOCK)
          {
            Close(t, "local write failed");
            continue;
          }
        }
        if (t.closing && t.to_local.empty())
        {
          Close(t, nullptr);
        }
      }
      std::erase_if(tunnels_, [](const std::unique_ptr<Tunnel> &t) { return t->local == INVALID_SOCKET; });
    }

    void Close(Tunnel &t, const char *why)
    {
      if (why)
      {
        Log("TCP %u with %llu: %s", t.port, (unsigned long long)t.peer, why);
      }
      if (t.conn != k_HSteamNetConnection_Invalid)
      {
        SteamNetworkingSockets()->CloseConnection(t.conn, 0, nullptr, true);
        t.conn = k_HSteamNetConnection_Invalid;
      }
      if (t.local != INVALID_SOCKET)
      {
        shutdown(t.local, SD_SEND);
        closesocket(t.local);
        t.local = INVALID_SOCKET;
      }
    }

    uint64_t self_ = 0;
    uint32_t self_ip_ = 0;
    std::map<uint64_t, Peer> peers_;
    std::map<HSteamListenSocket, uint16_t> listen_;
    std::vector<std::unique_ptr<Tunnel>> tunnels_;
  };

  // -------------------------------------------------------------------------
  // Steam callbacks and lobbies (Steam thread only)
  // -------------------------------------------------------------------------

  class Session
  {
  public:
    Bridge bridge;

    void Tick()
    {
      auto now = Clock::now();
      Requests();
      if (now >= next_friends_)
      {
        next_friends_ = now + std::chrono::seconds(10);
        ScanFriends();
      }
      UpdateHosting();
      PumpKnocks();
      PollListed(now);
    }

    // While a list is on the screen, its lobbies' data every 2 s (OnLobbyData): the host's menu, the players,
    // the match rules and a lobby gone were only seen at the next refresh (15-20 s). Friends-only lobbies are
    // not in RequestLobbyList at all, so their data was as old as the last RequestLobbyData.
    void PollListed(Clock::time_point now)
    {
      std::vector<uint64_t> ids;
      {
        std::lock_guard lock(shared().mutex);
        if (now - shared().viewed > std::chrono::seconds(1) || now < next_poll_)
        {
          return;
        }
        for (const ao2::steam::Lobby &l : shared().lobbies)
        {
          ids.push_back(l.id);
        }
      }
      next_poll_ = now + std::chrono::seconds(2);
      for (uint64_t id : ids)
      {
        if (id != lobby_) // this game's own lobby updates by itself
        {
          SteamMatchmaking()->RequestLobbyData(CSteamID(id));
        }
      }
    }

  private:
    // --- Joining by code: knocks on channel 1 ("knock\n<name>" to the
    // host, "invited"/"nogame"/"full"/"started" back) ---

    void SendControl(uint64_t to, const std::string &text)
    {
      SteamNetworkingMessages()->SendMessageToUser(
          Identity(to), text.data(), uint32_t(text.size()),
          k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_AutoRestartBrokenSession, 1);
    }

    void SetCodeState(ao2::steam::CodeJoin state, const std::string &host = {})
    {
      std::lock_guard lock(shared().mutex);
      shared().code_state = state;
      if (!host.empty())
      {
        shared().code_host = host;
      }
    }

    void Knock(uint32_t account)
    {
      CSteamID host(account, k_EUniversePublic, k_EAccountTypeIndividual);
      if (host == SteamUser()->GetSteamID())
      {
        SetCodeState(ao2::steam::CodeJoin::kOwnCode);
        return;
      }
      SteamFriends()->RequestUserInformation(host, true);
      knock_host_ = host.ConvertToUint64();
      knock_since_ = Clock::now();
      knock_next_ = knock_since_;
      if (Peer *p = bridge.AddPeer(knock_host_))
      {
        // Its invite may come before its answer.
        std::lock_guard lock(shared().mutex);
        shared().auto_accept[p->ip] = Clock::now() + std::chrono::seconds(60);
      }
      Log("joining by code: knocking at %llu", (unsigned long long)knock_host_);
    }

    void PumpKnocks()
    {
      auto now = Clock::now();
      if (knock_host_ && now >= knock_next_)
      {
        if (now - knock_since_ > std::chrono::seconds(20))
        {
          Log("joining by code: no answer");
          knock_host_ = 0;
          SetCodeState(ao2::steam::CodeJoin::kNoAnswer);
        }
        else
        {
          SendControl(knock_host_, std::string("knock\n") + SteamFriends()->GetPersonaName());
          knock_next_ = now + std::chrono::seconds(2);
        }
      }
      SteamNetworkingMessage_t *msgs[16];
      int n = SteamNetworkingMessages()->ReceiveMessagesOnChannel(1, msgs, 16);
      for (int i = 0; i < n; ++i)
      {
        uint64_t from = msgs[i]->m_identityPeer.GetSteamID64();
        std::string text(static_cast<const char *>(msgs[i]->m_pData), size_t(msgs[i]->m_cbSize));
        msgs[i]->Release();
        std::string word = text.substr(0, text.find('\n'));
        if (word == "knock")
        {
          OnKnock(from, text.size() > 6 ? text.substr(6) : std::string());
        }
        else if (from == knock_host_)
        {
          knock_host_ = 0;
          std::string name = SteamFriends()->GetFriendPersonaName(CSteamID(from));
          Log("joining by code: %s answered %s", name.c_str(), word.c_str());
          SetCodeState(word == "invited" ? ao2::steam::CodeJoin::kInvited
                       : word == "full"  ? ao2::steam::CodeJoin::kFull
                       : word == "started" ? ao2::steam::CodeJoin::kStarted
                                           : ao2::steam::CodeJoin::kNoGame,
                       name);
        }
      }
    }

    // Someone with this player's code: invited into the game it hosts.
    void OnKnock(uint64_t from, const std::string &name)
    {
      ao2::steam::Hosting h;
      {
        std::lock_guard lock(shared().mutex);
        h = shared().hosting;
      }
      const char *answer = !h.on ? "nogame" : h.in_game ? "started" : h.players >= h.max ? "full" : "invited";
      Peer *p = bridge.AddPeer(from);
      if (p && !name.empty() && (p->name.empty() || p->name == "[unknown]"))
      {
        p->name = name;
      }
      Log("%s knocked with this player's code: %s", p ? p->name.c_str() : "?", answer);
      if (p && std::strcmp(answer, "invited") == 0)
      {
        ao2::invite::InviteAddress(p->ip, p->name);
      }
      SendControl(from, answer);
    }

    uint64_t knock_host_ = 0;
    Clock::time_point knock_since_, knock_next_;

    // Friends running this game (its rich presence).
    void ScanFriends()
    {
      int count = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
      for (int i = 0; i < count; ++i)
      {
        CSteamID f = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
        FriendGameInfo_t game;
        if (SteamFriends()->GetFriendGamePlayed(f, &game) && game.m_gameID.AppID() == 480)
        {
          const char *ao2 = SteamFriends()->GetFriendRichPresence(f, "ao2");
          if (ao2 && *ao2)
          {
            bridge.AddPeer(f.ConvertToUint64());
          }
        }
      }
    }

    void Requests()
    {
      bool refresh, invite_dialog, quick = false;
      uint64_t join, profile;
      uint32_t code;
      {
        auto &sh = shared();
        std::lock_guard lock(sh.mutex);
        if (sh.quick == Shared::Quick::kWanted)
        {
          sh.quick = Shared::Quick::kSearching;
          quick = true;
          quick_mode_ = sh.quick_mode;
        }
        refresh = std::exchange(sh.refresh, false);
        join = std::exchange(sh.join, 0);
        code = std::exchange(sh.code_join, 0);
        invite_dialog = std::exchange(sh.invite_dialog, false);
        profile = std::exchange(sh.profile, 0);
        if (refresh)
        {
          sh.refreshing = true;
        }
      }
      if (quick)
      {
        // Only open ones of this build: the quick match's own list.
        SteamMatchmaking()->AddRequestLobbyListStringFilter("ao2", "1", k_ELobbyComparisonEqual);
        SteamMatchmaking()->AddRequestLobbyListStringFilter("private", "0", k_ELobbyComparisonEqual);
        SteamMatchmaking()->AddRequestLobbyListStringFilter("state", "lobby", k_ELobbyComparisonEqual);
        SteamMatchmaking()->AddRequestLobbyListStringFilter("build", hosting_build_.c_str(), k_ELobbyComparisonEqual);
        SteamMatchmaking()->AddRequestLobbyListFilterSlotsAvailable(1);
        SteamMatchmaking()->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
        quick_call_.Set(SteamMatchmaking()->RequestLobbyList(), this, &Session::OnQuickList);
        Log("quick match (mode %s): looking for a lobby", quick_mode_.c_str());
      }
      else if (refresh)
      {
        // Friends-only lobbies are not in the list: friends' ones asked for
        // here (their data comes while the list is fetched, OnList).
        for (CSteamID lobby : FriendLobbies())
        {
          SteamMatchmaking()->RequestLobbyData(lobby);
        }
        SteamMatchmaking()->AddRequestLobbyListStringFilter("ao2", "1", k_ELobbyComparisonEqual);
        SteamMatchmaking()->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
        list_call_.Set(SteamMatchmaking()->RequestLobbyList(), this, &Session::OnList);
      }
      QuickTimeout();
      if (join)
      {
        Join(join);
      }
      if (code)
      {
        Knock(code);
      }
      if (invite_dialog && lobby_)
      {
        SteamFriends()->ActivateGameOverlayInviteDialog(CSteamID(lobby_));
      }
      if (profile)
      {
        Log("profile of %llu in the overlay", (unsigned long long)profile);
        SteamFriends()->ActivateGameOverlayToUser("steamid", CSteamID(profile));
      }
    }

    // The lobbies friends playing this game are in (a friends-only one too).
    std::vector<CSteamID> FriendLobbies()
    {
      std::vector<CSteamID> lobbies;
      int count = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
      for (int i = 0; i < count; ++i)
      {
        FriendGameInfo_t game;
        CSteamID f = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
        if (SteamFriends()->GetFriendGamePlayed(f, &game) && game.m_gameID.AppID() == 480 &&
            game.m_steamIDLobby.IsValid())
        {
          lobbies.push_back(game.m_steamIDLobby);
        }
      }
      return lobbies;
    }

    // A quick match's lobby joined, its invite not accepted in time.
    void QuickTimeout()
    {
      uint64_t lobby = 0;
      {
        std::lock_guard lock(shared().mutex);
        if (shared().quick != Shared::Quick::kJoining || Clock::now() - quick_since_ < std::chrono::seconds(12))
        {
          return;
        }
        shared().quick = Shared::Quick::kFailed;
        lobby = shared().quick_lobby;
      }
      Log("quick match: no invite from lobby %llx in time, left", (unsigned long long)lobby);
      if (lobby == lobby_)
      {
        Leave();
      }
    }

    void QuickFailed(const char *why)
    {
      Log("quick match: %s", why);
      std::lock_guard lock(shared().mutex);
      shared().quick = Shared::Quick::kFailed;
    }

    // A lobby as listed: its data, the ping estimated from its host's location.
    ao2::steam::Lobby Describe(CSteamID id)
    {
      ao2::steam::Lobby l;
      l.id = id.ConvertToUint64();
      l.name = SteamMatchmaking()->GetLobbyData(id, "name");
      l.build = SteamMatchmaking()->GetLobbyData(id, "build");
      l.map = SteamMatchmaking()->GetLobbyData(id, "map");
      const char *type = SteamMatchmaking()->GetLobbyData(id, "gametype");
      l.game_type = *type ? std::atoi(type) : -1;
      const char *difficulty = SteamMatchmaking()->GetLobbyData(id, "difficulty");
      l.difficulty = *difficulty ? std::atoi(difficulty) : -1;
      l.versus = std::strcmp(SteamMatchmaking()->GetLobbyData(id, "mode"), "versus") == 0;
      l.is_private = std::strcmp(SteamMatchmaking()->GetLobbyData(id, "private"), "1") == 0;
      l.in_game = std::strcmp(SteamMatchmaking()->GetLobbyData(id, "state"), "game") == 0;
      l.players = std::atoi(SteamMatchmaking()->GetLobbyData(id, "players"));
      l.max = SteamMatchmaking()->GetLobbyMemberLimit(id);
      l.menu = SteamMatchmaking()->GetLobbyData(id, "menu");
      l.rules = SteamMatchmaking()->GetLobbyData(id, "rules");
      SteamNetworkPingLocation_t loc;
      const char *where = SteamMatchmaking()->GetLobbyData(id, "loc");
      if (*where && SteamNetworkingUtils()->ParsePingLocationString(where, loc))
      {
        l.ping = SteamNetworkingUtils()->EstimatePingTimeFromLocalHost(loc);
      }
      if (l.name.empty())
      {
        l.name = "?";
      }
      return l;
    }

    void OnQuickList(LobbyMatchList_t *e, bool io_failure)
    {
      uint32_t count = io_failure ? 0 : e->m_nLobbiesMatching;
      ao2::steam::Lobby best;
      for (uint32_t i = 0; i < count; ++i)
      {
        ao2::steam::Lobby l = Describe(SteamMatchmaking()->GetLobbyByIndex(int(i)));
        std::string mode = SteamMatchmaking()->GetLobbyData(CSteamID(l.id), "umode");
        if (l.id == lobby_ || l.players >= l.max || l.in_game || (!quick_mode_.empty() && mode != quick_mode_))
        {
          continue;
        }
        if (!best.id || ao2::steam::QuickScore(l) < ao2::steam::QuickScore(best))
        {
          best = l;
        }
      }
      if (!best.id)
      {
        QuickFailed(count ? "no open lobby of this mode" : "no lobbies");
        return;
      }
      Log("quick match: lobby %llx of %s, ping %d ms, %d/%d", (unsigned long long)best.id, best.name.c_str(),
          best.ping, best.players, best.max);
      {
        std::lock_guard lock(shared().mutex);
        shared().quick = Shared::Quick::kJoining;
        shared().quick_lobby = best.id;
      }
      quick_since_ = Clock::now();
      Join(best.id);
    }

    void Join(uint64_t id)
    {
      if (id == lobby_)
      {
        bool active;
        {
          std::lock_guard lock(shared().mutex);
          active = shared().joined_active;
        }
        if (own_lobby_ || active)
        {
          return;
        }
        // Its game was left, the lobby only kept a moment: entered again (the host invites again).
      }
      {
        // A game hosted now goes on until the invite takes this player away:
        // no new lobby for it meanwhile (UpdateHosting).
        std::lock_guard lock(shared().mutex);
        hold_hosting_ = shared().hosting.on;
      }
      Leave();
      joining_ = true;
      Log("joining lobby %llx", (unsigned long long)id);
      // The host (its "host" key) invites as soon as this player enters its lobby, often before this
      // side hears that the join went through (OnJoined): its invite is taken by itself from now on.
      const uint64_t host = std::strtoull(SteamMatchmaking()->GetLobbyData(CSteamID(id), "host"), nullptr, 10);
      if (host && host != SteamUser()->GetSteamID().ConvertToUint64())
      {
        if (Peer *p = bridge.AddPeer(host))
        {
          std::lock_guard lock(shared().mutex);
          shared().auto_accept[p->ip] = Clock::now() + std::chrono::seconds(60);
        }
      }
      join_call_.Set(SteamMatchmaking()->JoinLobby(CSteamID(id)), this, &Session::OnJoined);
    }

    void Leave()
    {
      if (lobby_)
      {
        Log("leaving lobby %llx", (unsigned long long)lobby_);
        SteamMatchmaking()->LeaveLobby(CSteamID(lobby_));
        // Out of the list now (not at the next refresh); the list asked for
        // again.
        std::lock_guard lock(shared().mutex);
        std::erase_if(shared().lobbies, [&](const ao2::steam::Lobby &l) { return l.id == lobby_; });
        shared().refresh = true;
      }
      SetLobby(0, false);
      lobby_host_ = 0;
    }

    void SetLobby(uint64_t id, bool own)
    {
      lobby_ = id;
      own_lobby_ = own;
      std::lock_guard lock(shared().mutex);
      shared().lobby = id;
      shared().own_lobby = own;
      shared().joined_active = id && !own;
    }

    void SetJoinedActive(bool active)
    {
      std::lock_guard lock(shared().mutex);
      shared().joined_active = active;
    }

    // A lobby joined holds a place of the host's lobby: left once this
    // player's game is over (some grace: a versus party moving to its match),
    // or when its game was never entered. A session under way at the join is
    // another game's, so only one that starts afterwards counts. Leaving does
    // not disturb a join still going on: that runs through the invite.
    void UpdateJoinedLobby(const ao2::steam::Hosting &h, Clock::time_point now)
    {
      if (!lobby_ || own_lobby_)
      {
        return;
      }
      if (h.session && !joined_session_before_)
      {
        joined_session_seen_ = true;
      }
      joined_session_before_ = h.session;
      const char *why = nullptr;
      if (!joined_session_seen_)
      {
        if ((!h.session || hold_hosting_) && now - joined_since_ > std::chrono::seconds(30))
        {
          why = "its game was not entered";
        }
      }
      else if (h.session)
      {
        joined_session_ended_ = {};
        SetJoinedActive(true);
      }
      else
      {
        if (joined_session_ended_ == Clock::time_point{})
        {
          joined_session_ended_ = now;
          SetJoinedActive(false); // the lists offer it again at once (the lobby is still kept a moment)
        }
        if (now - joined_session_ended_ > std::chrono::seconds(10))
        {
          why = "its game is over here";
        }
      }
      if (why)
      {
        Log("lobby %llx: %s", (unsigned long long)lobby_, why);
        Leave();
      }
    }

    // The lobby of the game this player hosts: made, kept up to date, left.
    void UpdateHosting()
    {
      ao2::steam::Hosting h;
      {
        std::lock_guard lock(shared().mutex);
        h = shared().hosting;
      }
      hosting_build_ = h.build;
      auto now = Clock::now();
      UpdateJoinedLobby(h, now);
      if (!h.on || (!joining_ && !lobby_))
      {
        hold_hosting_ = false; // the hosted game is over, or the join did not work out
      }
      if (!h.on)
      {
        // A few seconds' grace (the game between its own screens).
        if (own_lobby_ && now - hosting_seen_ > std::chrono::seconds(3))
        {
          Leave();
        }
        return;
      }
      hosting_seen_ = now;
      if (joining_ || hold_hosting_)
      {
        return;
      }
      if (!own_lobby_ && !creating_)
      {
        Leave();
        creating_ = true;
        create_call_.Set(SteamMatchmaking()->CreateLobby(h.is_private ? k_ELobbyTypeFriendsOnly : k_ELobbyTypePublic,
                                                         std::max(h.max, 2)),
                         this, &Session::OnCreated);
        Log("creating a lobby (%s, %s)", h.versus ? "versus" : "co-op", h.is_private ? "friends only" : "public");
        return;
      }
      if (own_lobby_)
      {
        CSteamID lobby(lobby_);
        auto set = [&](const char *key, const std::string &value) {
          if (value != SteamMatchmaking()->GetLobbyData(lobby, key))
          {
            SteamMatchmaking()->SetLobbyData(lobby, key, value.c_str());
          }
        };
        set("ao2", "1");
        // Who hosts: a player joining takes the invite by itself from the start (Join), not only once
        // its join's answer is in - the invite sent when it enters often comes first.
        set("host", std::to_string(SteamUser()->GetSteamID().ConvertToUint64()));
        set("name", h.name);
        set("build", h.build);
        set("mode", h.versus ? "versus" : "coop");
        set("private", h.is_private ? "1" : "0");
        set("players", std::to_string(h.players));
        set("map", h.map);
        set("gametype", h.game_type >= 0 ? std::to_string(h.game_type) : "");
        set("difficulty", h.difficulty >= 0 ? std::to_string(h.difficulty) : "");
        set("umode", h.mode);
        set("state", h.in_game ? "game" : "lobby");
        set("menu", h.menu);
        set("rules", h.rules);
        // Where this PC is, for the others' ping (ready once the relays answered).
        SteamNetworkPingLocation_t loc;
        if (SteamNetworkingUtils()->GetLocalPingLocation(loc) >= 0.0f)
        {
          char where[k_cchMaxSteamNetworkingPingLocationString];
          SteamNetworkingUtils()->ConvertPingLocationToString(loc, where, sizeof(where));
          set("loc", where);
        }
        if (SteamMatchmaking()->GetLobbyMemberLimit(lobby) != std::max(h.max, 2))
        {
          SteamMatchmaking()->SetLobbyMemberLimit(lobby, std::max(h.max, 2));
        }
      }
    }

    void OnCreated(LobbyCreated_t *e, bool io_failure)
    {
      creating_ = false;
      if (io_failure || e->m_eResult != k_EResultOK)
      {
        Log("lobby not created (%d)", io_failure ? -1 : int(e->m_eResult));
        return;
      }
      if (joining_ || lobby_)
      {
        // Another lobby was chosen while this one was being made.
        Log("lobby %llx created, not needed any more", (unsigned long long)e->m_ulSteamIDLobby);
        SteamMatchmaking()->LeaveLobby(CSteamID(e->m_ulSteamIDLobby));
        return;
      }
      SetLobby(e->m_ulSteamIDLobby, true);
      Log("lobby %llx created", (unsigned long long)lobby_);
      UpdateHosting();
    }

    void OnJoined(LobbyEnter_t *e, bool io_failure)
    {
      joining_ = false;
      if (io_failure || e->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess)
      {
        Log("lobby %llx not joined (%d)", (unsigned long long)e->m_ulSteamIDLobby,
            io_failure ? -1 : int(e->m_EChatRoomEnterResponse));
        bool quick;
        {
          std::lock_guard lock(shared().mutex);
          quick = shared().quick == Shared::Quick::kJoining;
        }
        if (quick)
        {
          QuickFailed("the lobby was not joined");
        }
        return;
      }
      SetLobby(e->m_ulSteamIDLobby, false);
      joined_since_ = Clock::now();
      joined_session_seen_ = false;
      joined_session_ended_ = {};
      {
        std::lock_guard lock(shared().mutex);
        joined_session_before_ = shared().hosting.session;
      }
      CSteamID owner =SteamMatchmaking()->GetLobbyOwner(CSteamID(lobby_));
      lobby_host_ = owner.ConvertToUint64();
      int members = SteamMatchmaking()->GetNumLobbyMembers(CSteamID(lobby_));
      for (int i = 0; i < members; ++i)
      {
        bridge.AddPeer(SteamMatchmaking()->GetLobbyMemberByIndex(CSteamID(lobby_), i).ConvertToUint64());
      }
      if (Peer *p = bridge.AddPeer(owner.ConvertToUint64()))
      {
        // The host invites whoever enters (OnMember): accepted here.
        std::lock_guard lock(shared().mutex);
        shared().auto_accept[p->ip] = Clock::now() + std::chrono::seconds(60);
        Log("joined lobby %llx of %s: its invite is accepted", (unsigned long long)lobby_, p->name.c_str());
      }
    }

    void OnList(LobbyMatchList_t *e, bool io_failure)
    {
      std::vector<ao2::steam::Lobby> list;
      uint32_t count = io_failure ? 0 : e->m_nLobbiesMatching;
      for (uint32_t i = 0; i < count; ++i)
      {
        list.push_back(Describe(SteamMatchmaking()->GetLobbyByIndex(int(i))));
      }
      // Not listed (friends only): this game's own lobby and its friends'.
      std::vector<CSteamID> more = FriendLobbies();
      if (lobby_)
      {
        more.push_back(CSteamID(lobby_));
      }
      for (CSteamID id : more)
      {
        bool listed = std::any_of(list.begin(), list.end(),
                                  [&](const ao2::steam::Lobby &l) { return l.id == id.ConvertToUint64(); });
        if (!listed && std::strcmp(SteamMatchmaking()->GetLobbyData(id, "ao2"), "1") == 0)
        {
          list.push_back(Describe(id));
        }
      }
      std::stable_sort(list.begin(), list.end(), [](const ao2::steam::Lobby &a, const ao2::steam::Lobby &b) {
        return (a.ping < 0 ? 999 : a.ping) < (b.ping < 0 ? 999 : b.ping);
      });
      Log("lobby list: %u (%zu with friends' and its own)", count, list.size());
      std::lock_guard lock(shared().mutex);
      shared().lobbies = std::move(list);
      shared().refreshing = false;
    }

    // Someone entered our lobby: invited to the game (a peer from now on).
    STEAM_CALLBACK(Session, OnMember, LobbyChatUpdate_t);
    // A friend's "Join game" or an accepted lobby invite in Steam.
    STEAM_CALLBACK(Session, OnJoinRequested, GameLobbyJoinRequested_t);
    STEAM_CALLBACK(Session, OnStatus, SteamNetConnectionStatusChangedCallback_t);
    STEAM_CALLBACK(Session, OnSessionRequest, SteamNetworkingMessagesSessionRequest_t);
    // Messages to a player that could not get through (its UDP, knocks).
    STEAM_CALLBACK(Session, OnSessionFailed, SteamNetworkingMessagesSessionFailed_t);
    // The Steam overlay opened or closed (OverlayBlocksInput).
    STEAM_CALLBACK(Session, OnOverlay, GameOverlayActivated_t);
    // A lobby's data came (PollListed, RequestLobbyData; this game's own lobby by itself): its row anew.
    STEAM_CALLBACK(Session, OnLobbyData, LobbyDataUpdate_t);

    Clock::time_point next_poll_;

    uint64_t lobby_ = 0;
    bool own_lobby_ = false;
    uint64_t lobby_host_ = 0; // the joined lobby's host (its owner at the join)
    bool creating_ = false;
    bool joining_ = false;      // JoinLobby asked, its answer not in yet
    bool hold_hosting_ = false; // a game hosted when the join started: no lobby for it
    Clock::time_point joined_since_;
    bool joined_session_before_ = false; // the session state last seen (UpdateJoinedLobby)
    bool joined_session_seen_ = false;   // a session started since the lobby was joined
    Clock::time_point joined_session_ended_;
    Clock::time_point hosting_seen_;
    Clock::time_point next_friends_;
    CCallResult<Session, LobbyCreated_t> create_call_;
    CCallResult<Session, LobbyEnter_t> join_call_;
    CCallResult<Session, LobbyMatchList_t> list_call_;
    CCallResult<Session, LobbyMatchList_t> quick_call_;
    std::string quick_mode_;
    std::string hosting_build_;
    Clock::time_point quick_since_;
  };

  void Session::OnMember(LobbyChatUpdate_t *e)
  {
    if (e->m_ulSteamIDLobby != lobby_)
    {
      return;
    }
    // The host gone: this player leaves too (else Steam hands the lobby to
    // it and the game stays in the server list).
    if (!own_lobby_ && e->m_ulSteamIDUserChanged == lobby_host_ &&
        (e->m_rgfChatMemberStateChange &
         (k_EChatMemberStateChangeLeft | k_EChatMemberStateChangeDisconnected | k_EChatMemberStateChangeKicked |
          k_EChatMemberStateChangeBanned)))
    {
      Log("the host left lobby %llx", (unsigned long long)lobby_);
      Leave();
      return;
    }
    if (!(e->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered))
    {
      return;
    }
    Peer *p = bridge.AddPeer(e->m_ulSteamIDUserChanged);
    if (p && own_lobby_)
    {
      Log("%s entered the lobby: invited", p->name.c_str());
      ao2::invite::InviteAddress(p->ip, p->name);
    }
  }

  // The overlay takes the keyboard while open, but the window keeps the focus: the Esc that closes it (and its
  // key repeats) reached the game as well, which opened the pause menu. The game gets no input while it is open
  // and a moment after (keys held then stay from the game until let go, debug_menu.cpp InputBlocker).
  std::atomic<bool> g_overlay_open{false};
  std::atomic<int64_t> g_overlay_closed_ms{0};

  int64_t NowMs()
  {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
  }

  void Session::OnLobbyData(LobbyDataUpdate_t *e)
  {
    if (e->m_ulSteamIDLobby != e->m_ulSteamIDMember)
    {
      return; // a member's data
    }
    CSteamID id(e->m_ulSteamIDLobby);
    bool gone = !e->m_bSuccess || std::strcmp(SteamMatchmaking()->GetLobbyData(id, "ao2"), "1") != 0;
    ao2::steam::Lobby fresh;
    if (!gone)
    {
      fresh = Describe(id);
    }
    std::lock_guard lock(shared().mutex);
    auto &list = shared().lobbies;
    auto it = std::find_if(list.begin(), list.end(), [&](const ao2::steam::Lobby &l) { return l.id == fresh.id || l.id == id.ConvertToUint64(); });
    if (it == list.end())
    {
      return; // not listed: the next refresh decides
    }
    if (gone)
    {
      if (it->id != shared().lobby)
      {
        list.erase(it);
      }
      return;
    }
    if (fresh.ping < 0)
    {
      fresh.ping = it->ping;
    }
    *it = std::move(fresh);
  }

  void Session::OnOverlay(GameOverlayActivated_t *e)
  {
    bool open = e->m_bActive != 0;
    if (!open)
    {
      g_overlay_closed_ms = NowMs();
    }
    g_overlay_open = open;
    Log("Steam overlay %s", open ? "open" : "closed");
  }

  void Session::OnJoinRequested(GameLobbyJoinRequested_t *e)
  {
    Log("join requested through Steam: lobby %llx (%s)", (unsigned long long)e->m_steamIDLobby.ConvertToUint64(),
        SteamFriends()->GetFriendPersonaName(e->m_steamIDFriend));
    Join(e->m_steamIDLobby.ConvertToUint64());
  }

  void Session::OnStatus(SteamNetConnectionStatusChangedCallback_t *e)
  {
    bridge.OnStatus(e);
  }

  void Session::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t *e)
  {
    SteamNetworkingMessages()->AcceptSessionWithUser(e->m_identityRemote);
    bridge.AddPeer(e->m_identityRemote.GetSteamID64());
  }

  void Session::OnSessionFailed(SteamNetworkingMessagesSessionFailed_t *e)
  {
    uint64_t id = e->m_info.m_identityRemote.GetSteamID64();
    Peer *p = bridge.AddPeer(id);
    Log("messages with %s (%llu) failed (%d, %s)", p ? p->name.c_str() : "?", (unsigned long long)id,
        e->m_info.m_eEndReason, e->m_info.m_szEndDebug);
  }

  void Loop()
  {
    auto session = std::make_unique<Session>();
    session->bridge.Init();
    auto next_callbacks = Clock::now();
    for (;;)
    {
      auto now = Clock::now();
      if (now >= next_callbacks)
      {
        next_callbacks = now + std::chrono::milliseconds(10);
        SteamAPI_RunCallbacks();
        session->Tick();
      }
      session->bridge.Pump();
    }
  }
} // namespace

namespace ao2::steam
{
  void Start()
  {
    if (!REXCVAR_GET(ao2_steam) || g_running)
    {
      return;
    }
    // Linked to load late (CMakeLists.txt): a copy without it plays offline.
    if (!LoadLibraryA("steam_api64.dll"))
    {
      Log("Steam not started: no steam_api64.dll");
      return;
    }
    // Spacewar's app id: without it SteamAPI_Init looks for steam_appid.txt.
    SetEnvironmentVariableA("SteamAppId", kAppId);
    SetEnvironmentVariableA("SteamGameId", kAppId);
    SteamErrMsg error = {};
    if (SteamAPI_InitEx(&error) != k_ESteamAPIInitResult_OK)
    {
      Log("Steam not started: %s", error);
      return;
    }
    {
      std::lock_guard lock(shared().mutex);
      shared().persona = SteamFriends()->GetPersonaName();
      shared().account = SteamUser()->GetSteamID().GetAccountID();
    }
    if (REXCVAR_GET(ao2_steam_name))
    {
      // One name everywhere (invites, lobbies, the game's own gamertag): the runtime reads net_gamertag when
      // the game first goes online, later than this. 15 bytes as the runtime cuts it, on a whole UTF-8 letter.
      std::string name = PersonaName();
      if (name.size() > 15)
      {
        size_t cut = 15;
        while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80)
        {
          --cut;
        }
        name.resize(cut);
      }
      if (!name.empty())
      {
        rex::cvar::SetFlagByName("net_gamertag", name);
        Log("the player's name is the Steam name: %s", name.c_str());
      }
    }
    WSADATA wsa = {};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_running = true;
    ao2::ea::SetQuickMatchHook(&QuickMatch);
    std::thread(Loop).detach();
  }

  bool Running()
  {
    return g_running;
  }

  bool OverlayBlocksInput()
  {
    return g_overlay_open.load(std::memory_order_relaxed) ||
           NowMs() - g_overlay_closed_ms.load(std::memory_order_relaxed) < 400;
  }

  std::string PersonaName()
  {
    std::lock_guard lock(shared().mutex);
    return shared().persona;
  }

  std::vector<uint32_t> PeerAddresses()
  {
    std::lock_guard lock(shared().mutex);
    return shared().peer_ips;
  }

  uint32_t SelfAddress()
  {
    std::lock_guard lock(shared().mutex);
    return shared().self_ip;
  }

  uint64_t SteamIdOf(uint32_t ip)
  {
    std::lock_guard lock(shared().mutex);
    auto it = shared().peer_ids.find(ip);
    return it != shared().peer_ids.end() ? it->second : 0;
  }

  bool OpenProfile(uint64_t steam_id)
  {
    std::lock_guard lock(shared().mutex);
    if (!g_running || !steam_id)
    {
      return false;
    }
    shared().profile = steam_id;
    return true;
  }

  void SetHosting(const Hosting &hosting)
  {
    std::lock_guard lock(shared().mutex);
    shared().hosting = hosting;
  }

  bool TakeAutoAccept(uint32_t ip)
  {
    std::lock_guard lock(shared().mutex);
    auto it = shared().auto_accept.find(ip);
    if (it == shared().auto_accept.end())
    {
      return false;
    }
    bool live = Clock::now() < it->second;
    shared().auto_accept.erase(it);
    shared().auto_accepted += live ? 1 : 0;
    if (live && shared().quick == Shared::Quick::kJoining)
    {
      shared().quick = Shared::Quick::kAccepted;
    }
    return live;
  }

  uint32_t AutoAccepted()
  {
    std::lock_guard lock(shared().mutex);
    return shared().auto_accepted;
  }

  void RefreshLobbies()
  {
    std::lock_guard lock(shared().mutex);
    shared().refresh = true;
  }

  bool Refreshing()
  {
    std::lock_guard lock(shared().mutex);
    return shared().refresh || shared().refreshing;
  }

  std::vector<Lobby> Lobbies()
  {
    std::lock_guard lock(shared().mutex);
    shared().viewed = Clock::now();
    std::vector<Lobby> list = shared().lobbies;
    for (Lobby &l : list)
    {
      l.own = l.id == shared().lobby && shared().own_lobby;
      l.joined = l.own || (l.id == shared().lobby && shared().joined_active);
    }
    return list;
  }

  void JoinLobby(uint64_t id)
  {
    std::lock_guard lock(shared().mutex);
    shared().join = id;
  }

  bool QuickMatch(const std::string &mode)
  {
    if (!g_running)
    {
      return false;
    }
    {
      std::lock_guard lock(shared().mutex);
      shared().quick = Shared::Quick::kWanted;
      shared().quick_mode = mode;
      shared().quick_lobby = 0;
    }
    // The steam thread answers within ~1-2 s (the list), then ~12 s at most
    // (the join, the host's invite).
    auto until = Clock::now() + std::chrono::seconds(20);
    while (Clock::now() < until)
    {
      Sleep(50);
      std::lock_guard lock(shared().mutex);
      if (shared().quick == Shared::Quick::kAccepted || shared().quick == Shared::Quick::kFailed)
      {
        bool accepted = shared().quick == Shared::Quick::kAccepted;
        shared().quick = Shared::Quick::kNone;
        return accepted;
      }
    }
    std::lock_guard lock(shared().mutex);
    shared().quick = Shared::Quick::kNone;
    return false;
  }

  std::string JoinCode()
  {
    std::lock_guard lock(shared().mutex);
    return g_running && shared().account ? EncodeCode(shared().account) : std::string();
  }

  bool JoinByCode(const std::string &code)
  {
    uint32_t account;
    if (!g_running || !DecodeCode(code, account))
    {
      return false;
    }
    std::lock_guard lock(shared().mutex);
    shared().code_join = account;
    shared().code_state = CodeJoin::kKnocking;
    shared().code_host.clear();
    return true;
  }

  CodeJoin CodeJoinState()
  {
    std::lock_guard lock(shared().mutex);
    return shared().code_state;
  }

  std::string CodeJoinHost()
  {
    std::lock_guard lock(shared().mutex);
    return shared().code_host;
  }

  bool InviteFriends()
  {
    std::lock_guard lock(shared().mutex);
    if (!g_running || !shared().lobby)
    {
      return false;
    }
    shared().invite_dialog = true;
    return true;
  }
} // namespace ao2::steam

#else // !AO2_STEAM: built without the Steamworks SDK (steamworks/sdk)

namespace ao2::steam
{
  void Start() {}
  bool Running() { return false; }
  bool OverlayBlocksInput() { return false; }
  std::string PersonaName() { return {}; }
  std::vector<uint32_t> PeerAddresses() { return {}; }
  uint32_t SelfAddress() { return 0; }
  uint64_t SteamIdOf(uint32_t) { return 0; }
  bool OpenProfile(uint64_t) { return false; }
  void SetHosting(const Hosting &) {}
  bool TakeAutoAccept(uint32_t) { return false; }
  uint32_t AutoAccepted() { return 0; }
  void RefreshLobbies() {}
  bool Refreshing() { return false; }
  std::vector<Lobby> Lobbies() { return {}; }
  void JoinLobby(uint64_t) {}
  bool InviteFriends() { return false; }
  std::string JoinCode() { return {}; }
  bool JoinByCode(const std::string &) { return false; }
  CodeJoin CodeJoinState() { return CodeJoin::kNone; }
  std::string CodeJoinHost() { return {}; }
  bool QuickMatch(const std::string &) { return false; }
} // namespace ao2::steam

#endif
