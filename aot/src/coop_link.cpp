// coop_link.cpp - A side channel to the online co-op partner.
//
// Everything that changes the game goes through the lockstep input (coop.h),
// but while the lockstep stands still - a level's intro movie, during which
// neither game simulates a frame - nothing is exchanged that a player's key
// could ride on. Skipping that movie on one machine only left the other one
// in it and the partner dropped out, so a skip is sent to the partner here
// and both end the movie.
//
// It also carries the simulation settings both machines must share
// (ao2_decouple, ao2_sim_hz - decouple.cpp): each side sends its own with
// every hello.
//
// UDP port kPort (the same program as the game, so the firewall rule of
// "Allow through firewall" covers it). The partner's address is the one the
// game sends its own traffic to (sendto, VDP port 1000); replies go back to
// where hellos came from, so two games on one PC (one of them on another
// port) find each other too.
#include "coop_link.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <vector>

#include <rex/ppc.h>

#include "coop.h"
#include "log_files.h"

namespace
{
  using clock = std::chrono::steady_clock;

  constexpr uint16_t kPort = 1002;
  constexpr uint16_t kGamePort = 1000;
  constexpr uint32_t kMagic = 0x414F324C; // "AO2L"
  constexpr auto kRemoteSkipLifetime = std::chrono::milliseconds(1500);

  enum : uint32_t
  {
    kHello = 0,
    kSkipMovie = 1,
    kSettings = 2, // SettingsMessage
  };

  struct Message
  {
    uint32_t magic;
    uint32_t sender; // random per run: own messages (to ourselves) are ignored
    uint32_t type;
    uint32_t seq;
  };

  // A Message followed by the sender's simulation settings (older builds
  // ignore it: not the size of a Message).
  struct SettingsMessage
  {
    Message header;
    uint32_t flags; // kSettingsDecouple | kSettingsServer
    uint32_t sim_hz;
  };
  constexpr uint32_t kSettingsDecouple = 1;
  constexpr uint32_t kSettingsServer = 2;

  std::mutex g_mutex;
  SOCKET g_socket = INVALID_SOCKET;
  bool g_failed = false;
  uint32_t g_self = 0;
  uint32_t g_seq = 0;
  std::vector<sockaddr_in> g_peers; // where to send: the game's peer + who said hello
  std::atomic<uint32_t> g_game_peer{0}; // IPv4 (network order) the game sends to
  clock::time_point g_last_hello;
  uint32_t g_last_remote_seq = 0;
  std::atomic<int64_t> g_remote_skip{0}; // clock ticks of the partner's last skip
  // Simulation settings: this machine's (sent with hellos) and the
  // partner's (bit 63 = received; then flags << 48, sim_hz << 32, sender id).
  std::atomic<uint64_t> g_local_settings{0}; // flags << 32 | sim_hz
  std::atomic<uint64_t> g_partner_settings{0};
  std::atomic<uint32_t> g_self_id{0};

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("coop_link.log");
    return f;
  }

  template <typename... Args>
  void Logf(const char *format, Args... args)
  {
    if (FILE *f = Log())
    {
      std::fprintf(f, format, args...);
      std::fputc('\n', f);
      std::fflush(f);
    }
  }

  const char *Text(const sockaddr_in &a)
  {
    static thread_local char text[32];
    inet_ntop(AF_INET, &a.sin_addr, text, sizeof(text));
    std::snprintf(text + std::strlen(text), 8, ":%u", ntohs(a.sin_port));
    return text;
  }

  bool Open()
  {
    if (g_socket != INVALID_SOCKET)
    {
      return true;
    }
    if (g_failed)
    {
      return false;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET)
    {
      g_failed = true;
      Logf("socket failed: %d", WSAGetLastError());
      return false;
    }
    u_long nonblocking = 1;
    ioctlsocket(g_socket, FIONBIO, &nonblocking);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    // kPort normally; the next ones when a second game on this PC has it.
    for (uint16_t port = kPort; port < kPort + 8; ++port)
    {
      local.sin_port = htons(port);
      if (bind(g_socket, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0)
      {
        g_self = std::random_device{}() | 1;
        g_self_id.store(g_self, std::memory_order_relaxed);
        Logf("listening on UDP %u", port);
        return true;
      }
    }
    Logf("bind failed: %d", WSAGetLastError());
    closesocket(g_socket);
    g_socket = INVALID_SOCKET;
    g_failed = true;
    return false;
  }

  void AddPeer(const sockaddr_in &a)
  {
    for (const sockaddr_in &p : g_peers)
    {
      if (p.sin_addr.s_addr == a.sin_addr.s_addr && p.sin_port == a.sin_port)
      {
        return;
      }
    }
    g_peers.push_back(a);
    Logf("peer %s", Text(a));
  }

  void SendAll(uint32_t type, uint32_t seq)
  {
    Message m{htonl(kMagic), htonl(g_self), htonl(type), htonl(seq)};
    for (const sockaddr_in &p : g_peers)
    {
      sendto(g_socket, reinterpret_cast<const char *>(&m), sizeof(m), 0, reinterpret_cast<const sockaddr *>(&p),
             sizeof(p));
    }
  }

  void SendSettings()
  {
    uint64_t local = g_local_settings.load(std::memory_order_relaxed);
    SettingsMessage m{{htonl(kMagic), htonl(g_self), htonl(kSettings), 0}, htonl(uint32_t(local >> 32)),
                      htonl(uint32_t(local))};
    for (const sockaddr_in &p : g_peers)
    {
      sendto(g_socket, reinterpret_cast<const char *>(&m), sizeof(m), 0, reinterpret_cast<const sockaddr *>(&p),
             sizeof(p));
    }
  }

  // Under g_mutex.
  void PumpLocked()
  {
    if (!Open())
    {
      return;
    }
    if (uint32_t ip = g_game_peer.load(std::memory_order_relaxed))
    {
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_addr.s_addr = ip;
      a.sin_port = htons(kPort);
      AddPeer(a);
    }
    for (;;)
    {
      SettingsMessage packet;
      Message &m = packet.header;
      sockaddr_in from{};
      int from_len = sizeof(from);
      int n = recvfrom(g_socket, reinterpret_cast<char *>(&packet), sizeof(packet), 0,
                       reinterpret_cast<sockaddr *>(&from), &from_len);
      if (n < 0)
      {
        break; // WSAEWOULDBLOCK: nothing more (or the peer is not there yet)
      }
      if (n < int(sizeof(m)) || ntohl(m.magic) != kMagic || ntohl(m.sender) == g_self)
      {
        continue;
      }
      AddPeer(from);
      uint32_t seq = ntohl(m.seq);
      if (ntohl(m.type) == kSettings && n == int(sizeof(packet)))
      {
        uint64_t settings = 1ull << 63 | uint64_t(ntohl(packet.flags) & 0x7FFF) << 48 |
                            uint64_t(ntohl(packet.sim_hz) & 0xFFFF) << 32 | ntohl(m.sender);
        if (g_partner_settings.exchange(settings, std::memory_order_relaxed) != settings)
        {
          Logf("partner settings: decouple %u, sim %u Hz, server %u", ntohl(packet.flags) & kSettingsDecouple ? 1 : 0,
               ntohl(packet.sim_hz), ntohl(packet.flags) & kSettingsServer ? 1 : 0);
        }
      }
      if (n != int(sizeof(m)))
      {
        continue;
      }
      if (ntohl(m.type) == kSkipMovie && seq != g_last_remote_seq)
      {
        g_last_remote_seq = seq;
        g_remote_skip.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
        Logf("partner skips the movie (%u, from %s)", seq, Text(from));
      }
    }
    auto now = clock::now();
    if (!g_peers.empty() && now - g_last_hello > std::chrono::seconds(1))
    {
      g_last_hello = now;
      SendAll(kHello, 0);
      SendSettings();
    }
  }
} // namespace

namespace ao2::coop::link
{
  void Pump()
  {
    if (!network_session.load(std::memory_order_relaxed) && !g_game_peer.load(std::memory_order_relaxed))
    {
      return;
    }
    std::lock_guard lock(g_mutex);
    PumpLocked();
  }

  void SendSkipMovie()
  {
    std::lock_guard lock(g_mutex);
    PumpLocked();
    if (g_socket == INVALID_SOCKET)
    {
      return;
    }
    // A sequence number per skip; sent a few times (UDP), acted on once.
    uint32_t seq = (g_self & 0xFFFF0000u) | (++g_seq & 0xFFFF);
    for (int i = 0; i < 3; ++i)
    {
      SendAll(kSkipMovie, seq);
    }
    Logf("skip sent (%u) to %zu address(es)", seq, g_peers.size());
  }

  bool TakeRemoteSkipMovie()
  {
    int64_t t = g_remote_skip.load(std::memory_order_relaxed);
    if (!t || clock::now() - clock::time_point(clock::duration(t)) > kRemoteSkipLifetime)
    {
      return false;
    }
    return g_remote_skip.compare_exchange_strong(t, 0, std::memory_order_relaxed);
  }

  void SetLocalSettings(const SimSettings &settings)
  {
    uint64_t flags = (settings.decouple ? kSettingsDecouple : 0) | (settings.server ? kSettingsServer : 0);
    g_local_settings.store(flags << 32 | uint32_t(settings.sim_hz), std::memory_order_relaxed);
  }

  bool PartnerSettings(SimSettings *out)
  {
    uint64_t v = g_partner_settings.load(std::memory_order_relaxed);
    if (!(v >> 63))
    {
      return false;
    }
    uint32_t flags = uint32_t(v >> 48) & 0x7FFF;
    out->decouple = flags & kSettingsDecouple;
    out->server = flags & kSettingsServer;
    out->sim_hz = int32_t((v >> 32) & 0xFFFF);
    return true;
  }

  bool SettingsAreMine(const SimSettings &mine, const SimSettings &partner)
  {
    if (mine.server != partner.server)
    {
      return mine.server;
    }
    uint32_t partner_id = uint32_t(g_partner_settings.load(std::memory_order_relaxed));
    return g_self_id.load(std::memory_order_relaxed) < partner_id;
  }
} // namespace ao2::coop::link

// The game's sendto (r3 = socket, r4 = data, r5 = size, r6 = flags, r7 =
// sockaddr_in, big-endian family, then port and address in network order).
// Game traffic to port 1000 names the partner.
void CoopNetNoteGameSend(PPCContext &ctx, uint8_t *base);

extern "C" REX_FUNC(__imp__sub_82C19DB0);
extern "C" REX_FUNC(sub_82C19DB0)
{
  uint32_t to = ctx.r7.u32;
  if (to && ctx.r8.u32 >= 8)
  {
    const uint8_t *a = base + to;
    uint16_t port = uint16_t(a[2] << 8 | a[3]);
    uint32_t ip;
    std::memcpy(&ip, a + 4, 4); // network order, as in sockaddr_in
    if (port == kGamePort && ip != 0 && ip != 0xFFFFFFFFu)
    {
      g_game_peer.store(ip, std::memory_order_relaxed);
    }
    if (port == kGamePort)
    {
      CoopNetNoteGameSend(ctx, base);
    }
  }
  __imp__sub_82C19DB0(ctx, base);
}
