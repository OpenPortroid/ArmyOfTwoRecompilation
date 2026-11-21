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
// And the connection quality (coop_net_status.cpp): five times a second a
// Quality message with a time stamp of the sender's and the echo of the last
// stamp it got (and how long it held it), so each side measures the round
// trip itself; how many frames the sender waited for its partner; and its
// input delay margin wish with the lockstep frame it holds from
// (coop_input_delay.cpp takes the larger wish on both machines).
//
// UDP port kPort (the same program as the game, so the firewall rule of
// "Allow through firewall" covers it). The partner's address is the one the
// current game sends its own traffic to (sendto, VDP port 1000); replies go
// back to the source port learned at that address.
#include "coop_link.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "coop.h"
#include "log_files.h"

namespace
{
  using clock = std::chrono::steady_clock;

  using ao2::coop::link::kPort;
  constexpr uint16_t kGamePort = 1000;
  constexpr uint32_t kMagic = 0x414F324C; // "AO2L"
  constexpr auto kRemoteSkipLifetime = std::chrono::milliseconds(1500);
  constexpr auto kByeGrace = std::chrono::seconds(2);
  constexpr int kReceiveBudget = 64;

  enum : uint32_t
  {
    kHello = 0,
    kSkipMovie = 1,
    kSettings = 2, // SettingsMessage
    kQuality = 3,  // QualityMessage
    kBye = 4,      // this game's network session is over (left on purpose)
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

  // A Message followed by the connection quality (older builds ignore it).
  struct QualityMessage
  {
    Message header;
    uint32_t stamp;      // the sender's clock, ms (never 0)
    uint32_t echo;       // the last stamp the sender got from us (0: none)
    uint32_t echo_hold;  // ms it held that stamp before sending this
    uint32_t extra_ms;   // input delay margin wish
    uint32_t from_frame; // ... from this lockstep frame on
    uint32_t waits;      // frames waited for the partner in the last second
  };
  constexpr auto kQualityPeriod = std::chrono::milliseconds(200);

  struct Peer
  {
    sockaddr_in address;
    uint32_t quality_stamp = 0;
    uint32_t skip_seq = 0;
  };

  std::mutex g_mutex;
  SOCKET g_socket = INVALID_SOCKET;
  bool g_failed = false;
  uint32_t g_self = 0;
  uint32_t g_seq = 0;
  std::map<uint32_t, Peer> g_peers; // addresses this session's VDP sends to
  std::atomic<uint32_t> g_session_id{0};
  std::atomic<uint32_t> g_game_peer{0}; // IPv4 (network order) the game sends to
  clock::time_point g_ended_at;
  clock::time_point g_last_hello;
  std::atomic<int64_t> g_remote_skip{0}; // clock ticks of the partner's last skip
  // Simulation settings: this machine's (sent with hellos) and the
  // partner's (bit 63 = received; then flags << 48, sim_hz << 32, sender id).
  std::atomic<uint64_t> g_local_settings{0}; // flags << 32 | sim_hz
  std::atomic<uint64_t> g_partner_settings{0};
  std::atomic<uint32_t> g_self_id{0};

  // Connection quality.
  clock::time_point g_last_quality;
  uint32_t g_echo = 0;          // the partner's last stamp (under g_mutex)
  clock::time_point g_echo_got; // ... when it came
  std::atomic<int32_t> g_ping{-1};           // ms, smoothed
  std::atomic<int64_t> g_heard{0};           // clock ticks of the partner's last message
  std::atomic<uint64_t> g_local_extra{0};    // extra_ms << 32 | from_frame
  std::atomic<uint64_t> g_partner_extra{0};  // bit 63 = received, extra_ms << 32 | from_frame
  std::atomic<int32_t> g_local_waits{0};
  std::atomic<int32_t> g_partner_waits{-1};
  std::atomic<bool> g_partner_bye{false};

  uint32_t NowMs()
  {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();
    return uint32_t(ms);
  }

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
    constexpr DWORD kUdpConnReset = _WSAIOW(IOC_VENDOR, 12);
    BOOL report_icmp = FALSE;
    DWORD bytes = 0;
    WSAIoctl(g_socket, kUdpConnReset, &report_icmp, sizeof(report_icmp), nullptr, 0, &bytes, nullptr, nullptr);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    const std::string bind_ip = rex::cvar::GetFlagByName("net_bind_ip");
    if (!bind_ip.empty())
    {
      inet_pton(AF_INET, bind_ip.c_str(), &local.sin_addr);
    }
    // kPort normally; the next ones when a second game on this PC has it.
    for (uint16_t port = kPort; port < kPort + 8; ++port)
    {
      local.sin_port = htons(port);
      if (bind(g_socket, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0)
      {
        g_self = std::random_device{}() | 1;
        g_self_id.store(g_self, std::memory_order_relaxed);
        Logf("listening on UDP %s", Text(local));
        return true;
      }
    }
    Logf("bind failed: %d", WSAGetLastError());
    closesocket(g_socket);
    g_socket = INVALID_SOCKET;
    g_failed = true;
    return false;
  }

  void NoteGamePeer(uint32_t ip)
  {
    if (!ao2::coop::network_session.load(std::memory_order_acquire) ||
        g_game_peer.load(std::memory_order_relaxed) == ip)
    {
      return;
    }
    std::lock_guard lock(g_mutex);
    if (!ao2::coop::network_session.load(std::memory_order_relaxed))
    {
      return;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ip;
    address.sin_port = htons(kPort);
    if (g_peers.try_emplace(ip, Peer{address}).second)
    {
      g_last_hello = {}; // send settings promptly to the new partner
      Logf("peer %s", Text(address));
    }
    g_game_peer.store(ip, std::memory_order_relaxed);
  }

  template <typename T>
  void SendToPeers(const T &message)
  {
    for (const auto &[ip, peer] : g_peers)
    {
      sendto(g_socket, reinterpret_cast<const char *>(&message), sizeof(message), 0,
             reinterpret_cast<const sockaddr *>(&peer.address), sizeof(peer.address));
    }
  }

  void SendAll(uint32_t type, uint32_t seq)
  {
    SendToPeers(Message{htonl(kMagic), htonl(g_self), htonl(type), htonl(seq)});
  }

  void SendSettings()
  {
    uint64_t local = g_local_settings.load(std::memory_order_relaxed);
    SettingsMessage m{{htonl(kMagic), htonl(g_self), htonl(kSettings), 0}, htonl(uint32_t(local >> 32)),
                      htonl(uint32_t(local))};
    SendToPeers(m);
  }

  // Under g_mutex.
  void SendQuality()
  {
    auto now = clock::now();
    uint32_t hold =
        g_echo ? uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(now - g_echo_got).count()) : 0;
    uint64_t extra = g_local_extra.load(std::memory_order_relaxed);
    QualityMessage m{{htonl(kMagic), htonl(g_self), htonl(kQuality), 0},
                     htonl(NowMs() | 1),
                     htonl(g_echo),
                     htonl(hold),
                     htonl(uint32_t(extra >> 32)),
                     htonl(uint32_t(extra)),
                     htonl(uint32_t(g_local_waits.load(std::memory_order_relaxed)))};
    SendToPeers(m);
  }

  // Under g_mutex.
  void OnQuality(const QualityMessage &q)
  {
    g_echo = ntohl(q.stamp);
    g_echo_got = clock::now();
    if (uint32_t echo = ntohl(q.echo))
    {
      int64_t rtt = int64_t(NowMs() - echo) - int64_t(ntohl(q.echo_hold));
      if (rtt >= 0 && rtt < 10000)
      {
        int32_t old = g_ping.load(std::memory_order_relaxed);
        // Smoothed (a quarter of each sample); a jump up is taken at once.
        int32_t ping = old < 0 || rtt > 2 * old ? int32_t(rtt) : int32_t((3 * int64_t(old) + rtt + 2) / 4);
        g_ping.store(ping, std::memory_order_relaxed);
      }
    }
    uint32_t extra_ms = std::min<uint32_t>(ntohl(q.extra_ms), 1000);
    uint64_t extra = 1ull << 63 | uint64_t(extra_ms) << 32 | ntohl(q.from_frame);
    if (g_partner_extra.exchange(extra, std::memory_order_relaxed) != extra)
    {
      Logf("partner input margin wish: +%u ms from frame %u", extra_ms, ntohl(q.from_frame));
    }
    g_partner_waits.store(int32_t(std::min<uint32_t>(ntohl(q.waits), 10000)), std::memory_order_relaxed);
  }

  // Under g_mutex.
  void PumpLocked()
  {
    const bool active = ao2::coop::network_session.load(std::memory_order_relaxed);
    if ((!active && (g_ended_at == clock::time_point{} || clock::now() - g_ended_at > kByeGrace)) ||
        !Open() || g_peers.empty())
    {
      return;
    }
    for (int i = 0; i < kReceiveBudget; ++i)
    {
      char datagram[sizeof(QualityMessage)];
      sockaddr_in from{};
      int from_len = sizeof(from);
      int n = recvfrom(g_socket, datagram, sizeof(datagram), 0,
                       reinterpret_cast<sockaddr *>(&from), &from_len);
      if (n < 0)
      {
        if (WSAGetLastError() == WSAEMSGSIZE)
        {
          continue; // a datagram larger than any packet in this protocol
        }
        break;
      }
      Message m;
      if (n < int(sizeof(m)))
      {
        continue;
      }
      std::memcpy(&m, datagram, sizeof(m));
      if (ntohl(m.magic) != kMagic || ntohl(m.sender) == g_self)
      {
        continue;
      }
      auto peer = g_peers.find(from.sin_addr.s_addr);
      const uint32_t type = ntohl(m.type);
      if (peer == g_peers.end() || (!active && type != kBye))
      {
        continue;
      }
      Peer &remote = peer->second;
      if (type == kQuality && n == int(sizeof(QualityMessage)))
      {
        QualityMessage quality;
        std::memcpy(&quality, datagram, sizeof(quality));
        const uint32_t stamp = ntohl(quality.stamp);
        const uint32_t advance = stamp - remote.quality_stamp;
        if (!stamp || (remote.quality_stamp && (!advance || advance >= 0x80000000u)))
        {
          continue; // UDP reordering must not roll the partner's input margin back
        }
        remote.quality_stamp = stamp;
        OnQuality(quality);
      }
      else if (type == kSettings && n == int(sizeof(SettingsMessage)))
      {
        SettingsMessage packet;
        std::memcpy(&packet, datagram, sizeof(packet));
        const uint32_t hz = ntohl(packet.sim_hz);
        if (hz < 20 || hz > 240)
        {
          continue; // the sender may not have published its settings yet
        }
        uint64_t settings = 1ull << 63 | uint64_t(ntohl(packet.flags) & 0x7FFF) << 48 |
                            uint64_t(hz) << 32 | ntohl(m.sender);
        if (g_partner_settings.exchange(settings, std::memory_order_relaxed) != settings)
        {
          Logf("partner settings: decouple %u, sim %u Hz, server %u", ntohl(packet.flags) & kSettingsDecouple ? 1 : 0,
               ntohl(packet.sim_hz), ntohl(packet.flags) & kSettingsServer ? 1 : 0);
        }
      }
      else if (n != int(sizeof(Message)))
      {
        continue;
      }
      else if (type == kBye)
      {
        if (!g_partner_bye.exchange(true, std::memory_order_relaxed))
        {
          Logf("partner left its network session (from %s)", Text(from));
        }
        continue;
      }
      else if (type == kSkipMovie)
      {
        const uint32_t seq = ntohl(m.seq);
        const uint16_t advance = uint16_t(seq - remote.skip_seq);
        if (!seq || (remote.skip_seq && (!advance || advance >= 0x8000u)))
        {
          continue;
        }
        remote.skip_seq = seq;
        g_remote_skip.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
        Logf("partner skips the movie (%u, from %s)", seq, Text(from));
      }
      else if (type != kHello)
      {
        continue;
      }
      remote.address.sin_port = from.sin_port;
      g_heard.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    }
    if (!active)
    {
      return;
    }
    auto now = clock::now();
    if (now - g_last_quality >= kQualityPeriod)
    {
      g_last_quality = now;
      SendQuality();
    }
    if (now - g_last_hello > std::chrono::seconds(1))
    {
      g_last_hello = now;
      SendAll(kHello, 0);
      SendSettings();
    }
  }
} // namespace

namespace ao2::coop::link
{
  void BeginSession()
  {
    std::lock_guard lock(g_mutex);
    if (network_session.load(std::memory_order_relaxed))
    {
      return;
    }
    g_peers.clear();
    g_game_peer.store(0, std::memory_order_relaxed);
    g_ended_at = {};
    g_last_hello = {};
    g_last_quality = {};
    g_echo = 0;
    g_partner_bye.store(false, std::memory_order_relaxed);
    g_partner_settings.store(0, std::memory_order_relaxed);
    g_remote_skip.store(0, std::memory_order_relaxed);
    g_ping.store(-1, std::memory_order_relaxed);
    g_heard.store(0, std::memory_order_relaxed);
    g_local_extra.store(0, std::memory_order_relaxed);
    g_partner_extra.store(0, std::memory_order_relaxed);
    g_local_waits.store(0, std::memory_order_relaxed);
    g_partner_waits.store(-1, std::memory_order_relaxed);
    if (g_socket != INVALID_SOCKET)
    {
      // Drop datagrams queued between matches, including a rematch with the same peer.
      char stale[sizeof(QualityMessage)];
      for (int i = 0; i < kReceiveBudget; ++i)
      {
        if (recv(g_socket, stale, sizeof(stale), 0) < 0 && WSAGetLastError() != WSAEMSGSIZE)
        {
          break;
        }
      }
    }
    g_session_id.fetch_add(1, std::memory_order_release);
    network_session.store(true, std::memory_order_release);
  }

  void EndSession(bool voluntary)
  {
    std::lock_guard lock(g_mutex);
    if (!network_session.load(std::memory_order_relaxed))
    {
      return;
    }
    if (voluntary && g_socket != INVALID_SOCKET && !g_peers.empty())
    {
      for (int i = 0; i < 3; ++i)
      {
        SendAll(kBye, 0);
      }
      Logf("bye sent to %zu address(es)", g_peers.size());
    }
    g_ended_at = clock::now();
    network_session.store(false, std::memory_order_release);
  }

  uint32_t SessionId()
  {
    return g_session_id.load(std::memory_order_acquire);
  }

  void Pump()
  {
    std::lock_guard lock(g_mutex);
    PumpLocked();
  }

  void SendSkipMovie()
  {
    std::lock_guard lock(g_mutex);
    PumpLocked();
    if (!network_session.load(std::memory_order_relaxed) || g_socket == INVALID_SOCKET || g_peers.empty())
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
    out->sender_id = uint32_t(v);
    return true;
  }

  int32_t PingMs()
  {
    return g_ping.load(std::memory_order_relaxed);
  }

  int64_t PartnerSilentMs()
  {
    int64_t t = g_heard.load(std::memory_order_relaxed);
    if (!t)
    {
      return -1;
    }
    auto since = clock::now() - clock::time_point(clock::duration(t));
    return std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
  }

  uint32_t GamePeer()
  {
    return g_game_peer.load(std::memory_order_relaxed);
  }

  void ProposeExtraMs(int32_t extra_ms, uint32_t from_frame)
  {
    g_local_extra.store(uint64_t(uint32_t(std::max(extra_ms, 0))) << 32 | from_frame, std::memory_order_relaxed);
  }

  bool PartnerExtraMs(int32_t *extra_ms, uint32_t *from_frame)
  {
    uint64_t v = g_partner_extra.load(std::memory_order_relaxed);
    if (!(v >> 63))
    {
      return false;
    }
    *extra_ms = int32_t((v >> 32) & 0x7FFFFFFF);
    *from_frame = uint32_t(v);
    return true;
  }

  void SetLocalWaits(int32_t waits)
  {
    g_local_waits.store(waits, std::memory_order_relaxed);
  }

  int32_t PartnerWaits()
  {
    return g_partner_waits.load(std::memory_order_relaxed);
  }

  bool PartnerSaidBye()
  {
    return g_partner_bye.load(std::memory_order_relaxed);
  }

  bool SettingsAreMine(const SimSettings &mine, const SimSettings &partner)
  {
    if (mine.server != partner.server)
    {
      return mine.server;
    }
    return g_self_id.load(std::memory_order_relaxed) < partner.sender_id;
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
    if (port == kGamePort && a[0] == 0 && a[1] == AF_INET && ip != 0 && ip != INADDR_BROADCAST)
    {
      NoteGamePeer(ip);
    }
    if (port == kGamePort)
    {
      CoopNetNoteGameSend(ctx, base);
    }
  }
  __imp__sub_82C19DB0(ctx, base);
}
