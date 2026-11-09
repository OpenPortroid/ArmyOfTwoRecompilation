// openportroid_net.cpp - Online play over the OpenPortroid server (protocol
// and addresses: openportroid.h).
//
// The game keeps talking IPv4 on a virtual LAN on this PC's loopback, as
// Steam play does (steam_net.cpp has the background):
//
// - Every player signed in to the server has a virtual address in
//   127.128.0.0/10. For every player it knows (a peer) the bridge binds the
//   peer's address on the game port (UDP 1000), the link's (1002) and the
//   presence port; Windows
//   lets a socket bound to one address share a port with the game's socket
//   bound to all of them. What the game sends to a peer's address lands here
//   and goes to that player; on the far side the bridge passes it on from a
//   socket bound to the sender's address to this game (its own virtual
//   address, or net_bind_ip), so everybody sees everybody at its virtual
//   address. The lobby server shows each player at it too (the lobby tunnel
//   sends this game's key, ea_server.cpp).
// - The way to a peer: the server gives both players each other's public
//   and LAN ends (when the lobby pairs them, or one asks); both punch at all
//   of them for kPunchTime, and the first end that answers is the straight
//   way. Until then, and whenever the straight way is silent for kDirectLost,
//   datagrams go through the server.
//
// openportroid.log (with log_to_files) shows the peers and their ways.
#include "openportroid_net.h"

#include "coop_link.h"
#include "ea_server.h"
#include "log_files.h"
#include "openportroid.h"

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
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#include <rex/cvar.h>

REXCVAR_DECLARE(uint32_t, ao2_invite_port);
REXCVAR_DEFINE_BOOL(ao2_openportroid_direct, true, "Online",
                    "OpenPortroid: punch through NAT to the other player (straight, the lowest ping). Off: "
                    "everything through the server (a router that breaks the straight way; tests of the relay).");

namespace
{
  using Clock = std::chrono::steady_clock;

  constexpr uint16_t kGamePort = 1000; // VDP
  constexpr auto kHelloEvery = std::chrono::seconds(15);
  constexpr auto kHelloRetry = std::chrono::seconds(1);
  constexpr auto kPunchTime = std::chrono::seconds(10);
  constexpr auto kPunchEvery = std::chrono::milliseconds(200);
  constexpr auto kDirectKeepAlive = std::chrono::seconds(3);
  constexpr auto kDirectLost = std::chrono::seconds(10);
  constexpr auto kRetryIntroduce = std::chrono::seconds(30);

  void Log(const char *fmt, ...)
  {
    static std::mutex mutex;
    static FILE *f = ao2::OpenLogFile("openportroid.log");
    if (!f)
    {
      return;
    }
    std::lock_guard lock(mutex);
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

  std::string IpString(uint32_t ip)
  {
    char buf[INET_ADDRSTRLEN] = {};
    in_addr a = {};
    a.s_addr = ip;
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
  }

  struct End
  {
    uint32_t ip = 0;   // network order
    uint16_t port = 0; // network order
    bool operator==(const End &o) const { return ip == o.ip && port == o.port; }
  };

  std::string EndString(const End &e)
  {
    return IpString(e.ip) + ":" + std::to_string(ntohs(e.port));
  }

  sockaddr_in Sockaddr(uint32_t ip, uint16_t port_host_order)
  {
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = htons(port_host_order);
    return a;
  }

  void NoConnReset(SOCKET s)
  {
    DWORD no = 0, ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &no, sizeof(no), nullptr, 0, &ret, nullptr, nullptr);
  }

  uint16_t PresencePort()
  {
    return uint16_t(REXCVAR_GET(ao2_invite_port));
  }

  // -------------------------------------------------------------------------
  // Shared with the other threads
  // -------------------------------------------------------------------------

  struct Shared
  {
    std::mutex mutex;
    std::condition_variable signed_in;
    uint32_t self_ip = 0;
    std::map<uint32_t, bool> peers; // virtual address -> straight way up
    std::vector<uint32_t> connect;  // asked by Connect, for the bridge
  };

  Shared &shared()
  {
    static Shared s;
    return s;
  }

  uint8_t g_key[ao2::opnet::kKeySize];
  std::atomic<bool> g_started{false};

  // -------------------------------------------------------------------------
  // The bridge (its thread only)
  // -------------------------------------------------------------------------

  struct Peer
  {
    uint32_t vip = 0;
    std::map<uint16_t, SOCKET> udp; // bound to vip:port (host order)
    std::vector<End> ends;          // where to punch (public, then LAN)
    bool have_ends = false;
    End direct;
    bool direct_up = false;
    Clock::time_point heard;        // the straight way, last
    Clock::time_point punch_until;
    Clock::time_point next_punch;
    Clock::time_point next_introduce;
    Clock::time_point next_keepalive;
  };

  class Bridge
  {
  public:
    bool Init(const std::string &server, uint16_t port)
    {
      server_.ip = ao2::ea::ResolveIp(server);
      server_.port = htons(port);
      if (!server_.ip)
      {
        Log("server %s does not resolve", server.c_str());
        return false;
      }
      socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      sockaddr_in a = Sockaddr(htonl(INADDR_ANY), 0);
      if (socket_ == INVALID_SOCKET || bind(socket_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0)
      {
        Log("no UDP socket (error %d)", WSAGetLastError());
        return false;
      }
      NoConnReset(socket_);
      u_long on = 1;
      ioctlsocket(socket_, FIONBIO, &on); // Poll reads until nothing is left
      int len = sizeof(a);
      getsockname(socket_, reinterpret_cast<sockaddr *>(&a), &len);
      for (const auto &l : ao2::ea::LocalAddresses())
      {
        if (local_.size() < 8)
        {
          local_.push_back({inet_addr(l.ip.c_str()), a.sin_port});
        }
      }
      Log("server %s (%s:%u), this game on UDP %u", server.c_str(), IpString(server_.ip).c_str(), port,
          ntohs(a.sin_port));
      return true;
    }

    void Run()
    {
      for (;;)
      {
        auto now = Clock::now();
        if (now >= next_hello_)
        {
          Hello();
          next_hello_ = now + (self_ip_ ? kHelloEvery : kHelloRetry);
        }
        TakeConnects();
        Timers(now);
        Poll();
      }
    }

  private:
    std::string Head(char type) const
    {
      return std::string(ao2::opnet::kMagic, 3) + type;
    }

    void Put(std::string &out, const void *data, size_t n)
    {
      out.append(static_cast<const char *>(data), n);
    }

    void SendTo(const End &to, const std::string &data)
    {
      sockaddr_in a = {};
      a.sin_family = AF_INET;
      a.sin_addr.s_addr = to.ip;
      a.sin_port = to.port;
      sendto(socket_, data.data(), int(data.size()), 0, reinterpret_cast<sockaddr *>(&a), sizeof(a));
    }

    void Hello()
    {
      std::string h = Head('H');
      Put(h, g_key, sizeof(g_key));
      h += char(local_.size());
      for (const End &e : local_)
      {
        Put(h, &e.ip, 4);
        Put(h, &e.port, 2);
      }
      SendTo(server_, h);
    }

    void Publish()
    {
      std::lock_guard lock(shared().mutex);
      shared().peers.clear();
      for (const auto &[vip, p] : peers_)
      {
        shared().peers[vip] = p.direct_up;
      }
    }

    // The peer's UDP socket on `port` (host order), bound to the peer's address.
    SOCKET Udp(Peer &p, uint16_t port)
    {
      if (auto it = p.udp.find(port); it != p.udp.end())
      {
        return it->second;
      }
      SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      sockaddr_in a = Sockaddr(p.vip, port);
      if (s != INVALID_SOCKET && bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0)
      {
        Log("peer %s: UDP %u not bound (error %d)", IpString(p.vip).c_str(), port, WSAGetLastError());
        closesocket(s);
        s = INVALID_SOCKET;
      }
      if (s != INVALID_SOCKET)
      {
        u_long on = 1;
        ioctlsocket(s, FIONBIO, &on);
        NoConnReset(s);
      }
      p.udp[port] = s;
      return s;
    }

    Peer *AddPeer(uint32_t vip)
    {
      if (!ao2::opnet::IsVirtual(vip) || vip == self_ip_)
      {
        return nullptr;
      }
      if (auto it = peers_.find(vip); it != peers_.end())
      {
        return &it->second;
      }
      Peer &p = peers_[vip];
      p.vip = vip;
      for (uint16_t port : {kGamePort, ao2::coop::link::kPort, PresencePort()})
      {
        Udp(p, port);
      }
      Log("peer %s", IpString(vip).c_str());
      Publish();
      return &p;
    }

    void TakeConnects()
    {
      std::vector<uint32_t> asked;
      {
        std::lock_guard lock(shared().mutex);
        asked.swap(shared().connect);
      }
      for (uint32_t vip : asked)
      {
        if (Peer *p = AddPeer(vip); p && !p->have_ends)
        {
          p->next_introduce = Clock::now();
        }
      }
    }

    void Timers(Clock::time_point now)
    {
      bool changed = false;
      for (auto &[vip, p] : peers_)
      {
        if (p.direct_up && now - p.heard > kDirectLost)
        {
          p.direct_up = false;
          p.punch_until = now + kPunchTime;
          changed = true;
          Log("peer %s: the straight way is silent, through the server", IpString(vip).c_str());
        }
        if (p.direct_up && now >= p.next_keepalive)
        {
          Punch(p, p.direct, 'K');
          p.next_keepalive = now + kDirectKeepAlive;
        }
        if (!p.direct_up && p.have_ends && now < p.punch_until && now >= p.next_punch &&
            REXCVAR_GET(ao2_openportroid_direct))
        {
          for (const End &e : p.ends)
          {
            Punch(p, e, 'K');
          }
          p.next_punch = now + kPunchEvery;
        }
        // Introduced again (a new punch) while the way is not straight;
        // relay only, once is enough.
        if (!p.direct_up && now >= p.next_introduce && self_ip_ &&
            (REXCVAR_GET(ao2_openportroid_direct) || !p.have_ends))
        {
          std::string c = Head('C');
          Put(c, &vip, 4);
          SendTo(server_, c);
          p.next_introduce = now + kRetryIntroduce;
        }
      }
      if (changed)
      {
        Publish();
      }
    }

    void Punch(const Peer &p, const End &to, char type)
    {
      std::string k = Head(type);
      Put(k, &self_ip_, 4);
      Put(k, &p.vip, 4);
      SendTo(to, k);
    }

    // To the peer: straight when that way is up, else through the server.
    void Send(Peer &p, const char *payload, size_t n)
    {
      std::string d = Head(p.direct_up ? 'D' : 'R');
      Put(d, p.direct_up ? &self_ip_ : &p.vip, 4);
      Put(d, payload, n);
      SendTo(p.direct_up ? p.direct : server_, d);
    }

    // From the peer: passed on from the peer's address to this game.
    void Deliver(Peer &p, const uint8_t *payload, size_t n)
    {
      if (n < 4)
      {
        return;
      }
      uint16_t src, dst;
      std::memcpy(&src, payload, 2);
      std::memcpy(&dst, payload + 2, 2);
      SOCKET s = Udp(p, ntohs(src));
      if (s == INVALID_SOCKET)
      {
        return;
      }
      std::string bind_ip = rex::cvar::GetFlagByName("net_bind_ip");
      uint32_t to_ip = bind_ip.empty() ? self_ip_ : inet_addr(bind_ip.c_str());
      sockaddr_in to = Sockaddr(to_ip, ntohs(dst));
      sendto(s, reinterpret_cast<const char *>(payload + 4), int(n - 4), 0, reinterpret_cast<sockaddr *>(&to),
             sizeof(to));
    }

    bool IsEndOf(const Peer &p, const End &e) const
    {
      return std::find(p.ends.begin(), p.ends.end(), e) != p.ends.end();
    }

    void OnTransport(const End &from, const uint8_t *buf, size_t n)
    {
      if (n < 4 || std::memcmp(buf, ao2::opnet::kMagic, 3) != 0)
      {
        return;
      }
      const uint8_t *d = buf + 4;
      size_t size = n - 4;
      char type = char(buf[3]);
      bool from_server = from == server_;
      if (type == 'W' && from_server && size >= 10)
      {
        uint32_t vip;
        std::memcpy(&vip, d, 4);
        End pub;
        std::memcpy(&pub.ip, d + 4, 4);
        std::memcpy(&pub.port, d + 8, 2);
        if (vip != self_ip_)
        {
          Log("signed in: %s, public end %s", IpString(vip).c_str(), EndString(pub).c_str());
          self_ip_ = vip;
          std::lock_guard lock(shared().mutex);
          shared().self_ip = vip;
          shared().signed_in.notify_all();
        }
      }
      else if (type == 'P' && from_server && size >= 11)
      {
        uint32_t vip;
        std::memcpy(&vip, d, 4);
        Peer *p = AddPeer(vip);
        size_t count = d[10];
        if (!p || size < 11 + count * 6)
        {
          return;
        }
        std::vector<End> ends;
        End pub;
        std::memcpy(&pub.ip, d + 4, 4);
        std::memcpy(&pub.port, d + 8, 2);
        ends.push_back(pub);
        for (size_t i = 0; i < count; i++)
        {
          End e;
          std::memcpy(&e.ip, d + 11 + i * 6, 4);
          std::memcpy(&e.port, d + 11 + i * 6 + 4, 2);
          ends.push_back(e);
        }
        bool changed = ends != p->ends;
        p->ends = std::move(ends);
        p->have_ends = true;
        if (!p->direct_up)
        {
          auto now = Clock::now();
          p->punch_until = now + kPunchTime;
          p->next_punch = now;
        }
        if (changed)
        {
          Log("peer %s: %s at %s and %zu LAN ends", IpString(vip).c_str(),
              REXCVAR_GET(ao2_openportroid_direct) ? "punching" : "through the server (no punching)",
              EndString(pub).c_str(), count);
        }
      }
      else if (type == 'r' && from_server && size >= 4)
      {
        uint32_t vip;
        std::memcpy(&vip, d, 4);
        if (Peer *p = AddPeer(vip))
        {
          Deliver(*p, d + 4, size - 4);
        }
      }
      else if ((type == 'K' || type == 'k') && size >= 8 && REXCVAR_GET(ao2_openportroid_direct))
      {
        uint32_t vip, to;
        std::memcpy(&vip, d, 4);
        std::memcpy(&to, d + 4, 4);
        auto it = peers_.find(vip);
        // Only from an end the server gave for that player.
        if (to != self_ip_ || it == peers_.end() || !IsEndOf(it->second, from))
        {
          return;
        }
        Peer &p = it->second;
        if (type == 'K')
        {
          Punch(p, from, 'k');
        }
        p.heard = Clock::now();
        if (!p.direct_up || !(p.direct == from))
        {
          bool was = p.direct_up;
          p.direct = from;
          p.direct_up = true;
          p.next_keepalive = p.heard + kDirectKeepAlive;
          if (!was)
          {
            Log("peer %s: straight at %s", IpString(vip).c_str(), EndString(from).c_str());
            Publish();
          }
        }
      }
      else if (type == 'D' && size >= 4)
      {
        uint32_t vip;
        std::memcpy(&vip, d, 4);
        auto it = peers_.find(vip);
        if (it == peers_.end() || !it->second.direct_up || !(it->second.direct == from))
        {
          return;
        }
        it->second.heard = Clock::now();
        Deliver(it->second, d + 4, size - 4);
      }
    }

    void Poll()
    {
      struct Ref
      {
        Peer *peer;
        uint16_t port;
      };
      std::vector<WSAPOLLFD> fds;
      std::vector<Ref> refs;
      fds.push_back({socket_, POLLRDNORM, 0});
      refs.push_back({nullptr, 0});
      for (auto &[vip, p] : peers_)
      {
        for (auto &[port, s] : p.udp)
        {
          if (s != INVALID_SOCKET)
          {
            fds.push_back({s, POLLRDNORM, 0});
            refs.push_back({&p, port});
          }
        }
      }
      if (WSAPoll(fds.data(), ULONG(fds.size()), 20) <= 0)
      {
        return;
      }
      static uint8_t buf[65536];
      for (size_t i = 0; i < fds.size(); ++i)
      {
        if (!(fds[i].revents & (POLLRDNORM | POLLERR)))
        {
          continue;
        }
        for (int n = 0; n < 64; ++n)
        {
          sockaddr_in from = {};
          int from_len = sizeof(from);
          char *at = reinterpret_cast<char *>(buf) + (refs[i].peer ? 4 : 0);
          int got = recvfrom(fds[i].fd, at, int(sizeof(buf) - 4), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
          if (got < 0)
          {
            break;
          }
          if (!refs[i].peer)
          {
            OnTransport({from.sin_addr.s_addr, from.sin_port}, buf, size_t(got));
            continue;
          }
          // From this game to the peer: the ports first.
          uint16_t src = from.sin_port;
          uint16_t dst = htons(refs[i].port);
          std::memcpy(buf, &src, 2);
          std::memcpy(buf + 2, &dst, 2);
          Send(*refs[i].peer, reinterpret_cast<const char *>(buf), size_t(got) + 4);
        }
      }
    }

    SOCKET socket_ = INVALID_SOCKET;
    End server_;
    std::vector<End> local_;
    uint32_t self_ip_ = 0;
    Clock::time_point next_hello_;
    std::map<uint32_t, Peer> peers_;
  };
} // namespace

namespace ao2::opnet
{
  void Start(const std::string &server, uint16_t port, const std::string &key_hex)
  {
    if (g_started.exchange(true))
    {
      return;
    }
    bool valid = key_hex.size() == sizeof(g_key) * 2 &&
                 std::all_of(key_hex.begin(), key_hex.end(), [](char c) { return std::isxdigit(uint8_t(c)) != 0; });
    if (valid)
    {
      for (size_t i = 0; i < sizeof(g_key); i++)
      {
        g_key[i] = uint8_t(std::strtoul(key_hex.substr(i * 2, 2).c_str(), nullptr, 16));
      }
    }
    else
    {
      std::random_device rd;
      for (auto &b : g_key)
      {
        b = uint8_t(rd());
      }
    }
    WSADATA wsa = {};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    std::thread([server, port]() {
      Bridge bridge;
      if (bridge.Init(server, port))
      {
        bridge.Run();
      }
    }).detach();
  }

  bool Active()
  {
    return g_started;
  }

  std::string KeyHex()
  {
    if (!g_started)
    {
      return "";
    }
    std::string hex;
    for (uint8_t b : g_key)
    {
      char h[3];
      std::snprintf(h, sizeof(h), "%02x", b);
      hex += h;
    }
    return hex;
  }

  uint32_t SelfIp(int wait_ms)
  {
    std::unique_lock lock(shared().mutex);
    if (wait_ms > 0)
    {
      shared().signed_in.wait_for(lock, std::chrono::milliseconds(wait_ms), [] { return shared().self_ip != 0; });
    }
    return shared().self_ip;
  }

  std::vector<uint32_t> PeerAddresses()
  {
    std::lock_guard lock(shared().mutex);
    std::vector<uint32_t> out;
    for (const auto &[vip, up] : shared().peers)
    {
      out.push_back(vip);
    }
    return out;
  }

  void Connect(uint32_t vip)
  {
    if (!IsVirtual(vip))
    {
      return;
    }
    std::lock_guard lock(shared().mutex);
    shared().connect.push_back(vip);
  }

  std::string PathTo(uint32_t vip)
  {
    std::lock_guard lock(shared().mutex);
    auto it = shared().peers.find(vip);
    return it == shared().peers.end() ? "" : it->second ? "direct" : "relay";
  }
} // namespace ao2::opnet
