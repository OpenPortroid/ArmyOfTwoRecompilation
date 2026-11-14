// ea_server.cpp - Built-in EA online server for Army of Two co-op.
//
// Army of Two finds its partner through EA's servers (gone since 2014):
//   FESL      (TCP 18480) login: XBL360Login -> lkey / userId,
//   Theater   (TCP 18485) lobby: create / list / enter games,
//   Messenger (TCP 13505) presence.
// The co-op game itself then runs peer to peer between the two consoles (VDP
// UDP port 1000, see the SDK's xam_net.cpp), so all a server has to do is
// matchmaking. This implements just what the game uses, for two players: the
// host runs it in its own game (--ao2_coop_server=true), the partner points
// the game at the host (--ao2_coop_host=<host ip>).
//
// Packets: 4-byte type, u32 BE status/flags, u32 BE total length, then
// "key=value\n" lines ended by NUL. Requests from the game carry 0x40000000
// (Theater) / 0xC0000000 | id (FESL) in the flags.
//
// Joining (found by tracing the game): everybody, the host included, enters
// a game the same way - EGAM, the server forwards EGRQ to the host, the host
// answers EGRS (ALLOWED / REASON) and the player receives EGEG with the
// host's address. The host's game manager only starts hosting after its own
// EGEG; it recognizes its own request by the UID field (= FESL userId) and
// registers the partner in its Xbox Live session with the XUID field.
// Refusals are EGEG with status 'jden' (the client crashes on other forms).
// The client drops a connection that stays silent for 240 s, so the server
// pings every 30 s.
//
// Addresses: each player sends its XNADDR (R-XNADDR, base64) with the
// address of its own adapter. The server replaces it with the address it
// actually sees that player at (and the host's with the address the partner
// reached the server at), so LAN and VPN setups work without configuration.
//
// The same file is the OpenPortroid server's lobby (aot.bhop.pw/aot1/: the
// ArmyOfTwoOpenPortroid repository builds a copy of it on Linux with
// AO2_STANDALONE_SERVER and AO2_OPENPORTROID_SERVER, next to its relay and
// player names). Its players reach it through the HTTP tunnel ("Tunnel"
// below); with ao2_net_mode = openportroid this game's end of that tunnel is
// here too. Keep it building without the SDK and on POSIX sockets.
#include "ea_server.h"
#include "log_files.h"
#include "openportroid.h"
#include "steam_net.h"
#ifdef AO2_OPENPORTROID_SERVER
#include "nick_registry.h"
#include "relay_server.h"
#endif
#ifndef AO2_STANDALONE_SERVER
#include "openportroid_net.h"
#endif

// clang-format off
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
// The OpenPortroid server builds this on Linux.
#include <arpa/inet.h>
#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
inline int closesocket(SOCKET s) { return close(s); }
inline int WSAGetLastError() { return errno; }
#endif
// clang-format on

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef AO2_STANDALONE_SERVER
#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(ao2_net_mode, "steam", "Online",
                      "Online play: steam (LAN / VPN with ao2_coop_server / ao2_coop_host, and Steam when it "
                      "runs, ao2_steam), lan (the same without Steam) or openportroid (the OpenPortroid server, "
                      "ao2_openportroid_server: anyone on the internet, no VPN or open ports; no Steam).");
REXCVAR_DEFINE_STRING(ao2_openportroid_server, "aot.bhop.pw", "Online",
                      "The OpenPortroid server (ao2_net_mode = openportroid): the lobby over HTTP (port 80, or "
                      "host:port), the game traffic over UDP ao2_openportroid_port.");
REXCVAR_DEFINE_UINT32(ao2_openportroid_port, 18501, "Online",
                      "The OpenPortroid server's UDP port: NAT punching between the players and the relay.");
REXCVAR_DEFINE_STRING(ao2_openportroid_key, "", "Online",
                      "This player's key on the OpenPortroid server (32 hex digits, kept by the launcher): the "
                      "player name claimed with it is this player's. Empty: a new key each run, no own name.");
REXCVAR_DEFINE_BOOL(ao2_coop_server, false, "Online",
                    "LAN co-op host (ao2_net_mode steam / lan): run the EA lobby server (TCP 18480, 18485, 13505) "
                    "in this game. The partner connects with ao2_coop_host = this computer's IP (VPN/LAN/public).");
REXCVAR_DEFINE_STRING(ao2_coop_host, "", "Online",
                      "LAN co-op partner (ao2_net_mode steam / lan): IP address of the player who runs "
                      "ao2_coop_server.");
REXCVAR_DEFINE_BOOL(ao2_public_game_search, true, "Online",
                    "Search Co-op Game / Quick Match look for an open game first (on this PC's server, then "
                    "over Steam) and host only when there is none. Off: they host at once, a public game "
                    "everyone sees in the server list (F1 > Online > Servers); Create Co-op Game > Public "
                    "does that too.");
#endif

namespace ao2::ea
{
  namespace
  {
    bool PublicGameSearch()
    {
#ifndef AO2_STANDALONE_SERVER
      return REXCVAR_GET(ao2_public_game_search);
#else
      return false;
#endif
    }

    constexpr uint16_t kFeslPort = 18480;
    constexpr uint16_t kTheaterPort = 18485;
    constexpr uint16_t kMessengerPort = 13505;
    constexpr uint16_t kDiscoveryPort = 18490;
    constexpr auto kKeepAlive = std::chrono::seconds(30);
    // How long an entry is asked again while the host refuses with REASON=2.
    constexpr auto kHostNotReadyWait = std::chrono::seconds(8);

    using Fields = std::vector<std::pair<std::string, std::string>>;

#ifdef _WIN32
    constexpr int kSendFlags = 0;
    void LocalTime(const std::time_t *t, std::tm *out) { localtime_s(out, t); }
    void UtcTime(const std::time_t *t, std::tm *out) { gmtime_s(out, t); }
#else
    constexpr int kSendFlags = MSG_NOSIGNAL; // a closed peer must not kill the server
    void LocalTime(const std::time_t *t, std::tm *out) { localtime_r(t, out); }
    void UtcTime(const std::time_t *t, std::tm *out) { gmtime_r(t, out); }
#endif

    // ---------------------------------------------------------------------
    // Log (logs\net\ea_server.log, log_files.h)
    // ---------------------------------------------------------------------

    std::mutex g_log_mutex;
    bool g_log_console = false;

    void Log(const char *fmt, ...)
    {
      std::lock_guard lock(g_log_mutex);
      static FILE *f = ao2::OpenLogFile("ea_server.log");
      std::time_t now = std::time(nullptr);
      std::tm local = {};
      LocalTime(&now, &local);
      char line[2048];
      int n = std::snprintf(line, sizeof(line), "[%02d:%02d:%02d] ", local.tm_hour, local.tm_min, local.tm_sec);
      va_list args;
      va_start(args, fmt);
      std::vsnprintf(line + n, sizeof(line) - n, fmt, args);
      va_end(args);
      if (f)
      {
        std::fprintf(f, "%s\n", line);
        std::fflush(f);
      }
      if (g_log_console)
      {
        std::printf("%s\n", line);
        std::fflush(stdout);
      }
    }

    std::string Describe(const Fields &fields)
    {
      std::string s;
      for (const auto &[k, v] : fields)
      {
        if (!s.empty())
        {
          s += ' ';
        }
        s += k + '=' + (v.size() > 48 ? v.substr(0, 48) + "..." : v);
      }
      return s;
    }

    // ---------------------------------------------------------------------
    // Fields and packets
    // ---------------------------------------------------------------------

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

    void Set(Fields &fields, std::string_view key, std::string value)
    {
      for (auto &[k, v] : fields)
      {
        if (k == key)
        {
          v = std::move(value);
          return;
        }
      }
      fields.emplace_back(std::string(key), std::move(value));
    }

    uint64_t ToU64(const std::string &s, uint64_t fallback = 0)
    {
      if (s.empty())
      {
        return fallback;
      }
      char *end = nullptr;
      unsigned long long v = std::strtoull(s.c_str(), &end, 10);
      return end && *end == '\0' ? v : fallback;
    }

    uint32_t Tag(const char (&s)[5])
    {
      return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 |
             uint8_t(s[3]);
    }

    struct Packet
    {
      std::string type;
      uint32_t flags = 0;
      Fields fields;
    };

    std::string Build(const std::string &type, uint32_t flags, const Fields &fields)
    {
      std::string body;
      for (const auto &[k, v] : fields)
      {
        body += k;
        body += '=';
        body += v;
        body += '\n';
      }
      body += '\0';
      uint32_t length = uint32_t(12 + body.size());
      std::string out = type.substr(0, 4);
      out.resize(4, ' ');
      for (uint32_t v : {flags, length})
      {
        out += char(v >> 24);
        out += char(v >> 16);
        out += char(v >> 8);
        out += char(v);
      }
      return out + body;
    }

    Fields ParseBody(const char *p, size_t n)
    {
      Fields fields;
      std::string line;
      for (size_t i = 0; i <= n; i++)
      {
        char c = i < n ? p[i] : '\n';
        if (c == '\0' || c == '\n')
        {
          size_t eq = line.find('=');
          if (eq != std::string::npos)
          {
            fields.emplace_back(line.substr(0, eq), line.substr(eq + 1));
          }
          line.clear();
          if (c == '\0')
          {
            break;
          }
          continue;
        }
        line += c;
      }
      return fields;
    }

    // ---------------------------------------------------------------------
    // Addresses (XNADDR in base64, IPv4 in network order)
    // ---------------------------------------------------------------------

    constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string Base64Encode(const std::vector<uint8_t> &in)
    {
      std::string out;
      for (size_t i = 0; i < in.size(); i += 3)
      {
        uint32_t v = uint32_t(in[i]) << 16;
        if (i + 1 < in.size())
          v |= uint32_t(in[i + 1]) << 8;
        if (i + 2 < in.size())
          v |= in[i + 2];
        out += kBase64[(v >> 18) & 63];
        out += kBase64[(v >> 12) & 63];
        out += i + 1 < in.size() ? kBase64[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kBase64[v & 63] : '=';
      }
      return out;
    }

    std::vector<uint8_t> Base64Decode(const std::string &in)
    {
      std::vector<uint8_t> out;
      uint32_t v = 0;
      int bits = 0;
      for (char c : in)
      {
        const char *pos = std::strchr(kBase64, c);
        if (c == '=' || !pos || !c)
        {
          continue;
        }
        v = (v << 6) | uint32_t(pos - kBase64);
        bits += 6;
        if (bits >= 8)
        {
          bits -= 8;
          out.push_back(uint8_t(v >> bits));
        }
      }
      return out;
    }

    uint32_t ParseIp(const std::string &text)
    {
      in_addr a = {};
      return inet_pton(AF_INET, text.c_str(), &a) == 1 ? a.s_addr : 0;
    }

    std::string IpString(uint32_t ip)
    {
      char buf[INET_ADDRSTRLEN] = {};
      in_addr a = {};
      a.s_addr = ip;
      inet_ntop(AF_INET, &a, buf, sizeof(buf));
      return buf;
    }

    // This PC (a Steam or OpenPortroid player's virtual address, steam_net.h /
    // openportroid.h, is another PC).
    bool IsLoopback(uint32_t ip)
    {
      return (ntohl(ip) >> 24) == 127 && !ao2::steam::IsVirtual(ip) && !ao2::opnet::IsVirtual(ip);
    }

    // Loopback, LAN and carrier-grade NAT / Tailscale addresses.
    bool IsPrivate(uint32_t ip)
    {
      uint32_t h = ntohl(ip);
      return IsLoopback(ip) || h >> 24 == 10 || h >> 20 == 0xAC1 || h >> 16 == 0xC0A8 || h >> 22 == (100 << 2 | 1);
    }

    // The server's address on the internet (SetPublicAddress), 0 when not set.
    std::atomic<uint32_t> g_public_ip{0};

    // XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20] (the
    // SDK puts the IP into abOnline[0..3] too).
    std::string RewriteXnAddr(const std::string &xnaddr, uint32_t ip)
    {
      std::vector<uint8_t> raw = Base64Decode(xnaddr);
      if (raw.size() != 36 || !ip)
      {
        return xnaddr;
      }
      for (int offset : {0, 4, 16})
      {
        std::memcpy(raw.data() + offset, &ip, 4);
      }
      return Base64Encode(raw);
    }

    std::atomic<ao2::ea::QuickMatchHook> g_quick_match{nullptr};

    // ---------------------------------------------------------------------
    // Connections
    // ---------------------------------------------------------------------

    class Conn : public std::enable_shared_from_this<Conn>
    {
    public:
      Conn(SOCKET s, const char *kind) : socket_(s), kind_(kind)
      {
        sockaddr_in a = {};
        socklen_t len = sizeof(a);
        if (getpeername(s, reinterpret_cast<sockaddr *>(&a), &len) == 0)
        {
          peer_ip_ = a.sin_addr.s_addr;
          peer_port_ = ntohs(a.sin_port);
        }
        len = sizeof(a);
        if (getsockname(s, reinterpret_cast<sockaddr *>(&a), &len) == 0)
        {
          local_ip_ = a.sin_addr.s_addr;
        }
      }
      virtual ~Conn() { closesocket(socket_); }

      void Run()
      {
        Log("%s connection from %s:%u", kind_, IpString(peer_ip_).c_str(), peer_port_);
        std::string buffer;
        auto next_ping = std::chrono::steady_clock::now() + kKeepAlive;
        char chunk[16384];
        while (true)
        {
          fd_set fds;
          FD_ZERO(&fds);
          FD_SET(socket_, &fds);
          timeval tv = {1, 0};
          int ready = select(int(socket_) + 1, &fds, nullptr, nullptr, &tv);
          if (ready < 0)
          {
            break;
          }
          if (std::chrono::steady_clock::now() >= next_ping)
          {
            KeepAlive();
            next_ping = std::chrono::steady_clock::now() + kKeepAlive;
          }
          if (ready == 0)
          {
            continue;
          }
          int n = recv(socket_, chunk, sizeof(chunk), 0);
          if (n <= 0)
          {
            break;
          }
          buffer.append(chunk, size_t(n));
          while (buffer.size() >= 12)
          {
            auto u32 = [&](size_t o) {
              return uint32_t(uint8_t(buffer[o])) << 24 | uint32_t(uint8_t(buffer[o + 1])) << 16 |
                     uint32_t(uint8_t(buffer[o + 2])) << 8 | uint8_t(buffer[o + 3]);
            };
            uint32_t length = u32(8);
            if (length < 12 || length > (1u << 20))
            {
              Log("%s: bad packet length %u, closing", kind_, length);
              buffer.clear();
              goto done;
            }
            if (buffer.size() < length)
            {
              break;
            }
            Packet p;
            p.type = buffer.substr(0, 4);
            p.flags = u32(4);
            p.fields = ParseBody(buffer.data() + 12, length - 12);
            buffer.erase(0, length);
            if (p.type != "PING" && !(p.type == "fsys" && Get(p.fields, "TXN") == "Ping"))
            {
              Log("%s[%s] <- %s %08X %s", kind_, name_.c_str(), p.type.c_str(), p.flags,
                  Describe(p.fields).c_str());
            }
            OnPacket(p);
          }
        }
      done:
        closed_ = true;
        OnClose();
        Log("%s[%s] closed", kind_, name_.c_str());
      }

      bool Send(const std::string &type, uint32_t flags, const Fields &fields, bool quiet = false)
      {
        if (!quiet)
        {
          Log("%s[%s] -> %s%s %s", kind_, name_.c_str(), type.c_str(),
              flags & 0x00FFFFFF && !(flags & 0x80000000) ? " (error)" : "", Describe(fields).c_str());
        }
        std::string data = Build(type, flags, fields);
        std::lock_guard lock(send_mutex_);
        size_t sent = 0;
        while (sent < data.size())
        {
          int n = send(socket_, data.data() + sent, int(data.size() - sent), kSendFlags);
          if (n <= 0)
          {
            return false;
          }
          sent += size_t(n);
        }
        return true;
      }

      uint32_t peer_ip() const { return peer_ip_; }
      uint32_t local_ip() const { return local_ip_; }
      // The server's address this player reaches it at: the public one for a
      // player on the internet when the server is behind NAT (it only sees
      // its LAN address), else the one the connection arrived at.
      uint32_t server_ip() const
      {
        uint32_t pub = g_public_ip;
        return pub && !tunneled_ && !IsPrivate(peer_ip_) ? pub : local_ip_;
      }
      // A player through the tunnel (StartTunnelBridge): the connection comes
      // from nginx; the player is at `ip`, and its game reaches the server at
      // its own tunnel, `local` (127.0.0.1, or its net_bind_ip).
      void SetTunnel(uint32_t ip, uint32_t local, const std::string &key)
      {
        peer_ip_ = ip;
        local_ip_ = local;
        tunneled_ = true;
        key_ = key;
      }
      bool closed() const { return closed_; }
      const std::string &name() const { return name_; }

    protected:
      virtual void OnPacket(const Packet &p) = 0;
      virtual void KeepAlive() {}
      virtual void OnClose() {}

      SOCKET socket_;
      const char *kind_;
      uint32_t peer_ip_ = 0;
      uint32_t local_ip_ = 0;
      uint16_t peer_port_ = 0;
      bool tunneled_ = false;
      std::string key_; // the player's key (X-AO2-Key), through the tunnel
      std::string name_ = "?";
      std::atomic<bool> closed_{false};
      std::mutex send_mutex_;
    };

    class TheaterConn;

    struct Game
    {
      uint32_t gid = 0;
      std::shared_ptr<TheaterConn> host;
      Fields fields; // CGAM, updated by UGAM
      int joined = 1;
      std::string xnaddr; // host's R-XNADDR
      // Player ids count within the game, the host's own entry is 1: the game manager
      // decides which of two peers opens the connection by comparing them (versus mesh).
      uint32_t next_pid = 1;
    };

    // A player entering a game, until the host answers its EGRQ.
    struct Pending
    {
      std::weak_ptr<TheaterConn> joiner;
      Fields request; // the EGRQ, asked again while the host is not hosting yet
      std::chrono::steady_clock::time_point since;
    };

    struct State
    {
      std::mutex mutex;
      std::map<uint32_t, Game> games;
      uint32_t next_gid = 1;
      std::map<std::pair<uint32_t, uint32_t>, Pending> pending; // (gid, pid)
      std::map<uint64_t, std::string> names;                  // userId -> gamertag
      std::map<uint64_t, std::string> xuids;                  // userId -> XUID (decimal)
    };

    State &state()
    {
      static State s;
      return s;
    }

    // ---------------------------------------------------------------------
    // FESL: login
    // ---------------------------------------------------------------------

    class FeslConn : public Conn
    {
    public:
      explicit FeslConn(SOCKET s) : Conn(s, "FESL") {}

    protected:
      void KeepAlive() override { Send("fsys", 0x80000000, {{"TXN", "Ping"}}, true); }

      void OnPacket(const Packet &p) override
      {
        uint32_t reply = 0x80000000 | (p.flags & 0x00FFFFFF);
        std::string txn = Get(p.fields, "TXN");
        if (p.type == "fsys" && txn == "Hello")
        {
          std::string ip = IpString(server_ip());
          char now[64];
          std::time_t t = std::time(nullptr);
          std::tm utc = {};
          UtcTime(&t, &utc);
          std::strftime(now, sizeof(now), "\"%b-%d-%Y %H:%M:%S UTC\"", &utc);
          Send("fsys", reply,
               {{"TXN", "Hello"},
                {"domainPartition.domain", "eagames"},
                {"domainPartition.subDomain", "AO2"},
                {"messengerIp", ip},
                {"messengerPort", std::to_string(kMessengerPort)},
                {"activityTimeoutSecs", "0"},
                {"curTime", now},
                {"theaterIp", ip},
                {"theaterPort", std::to_string(kTheaterPort)}});
          Send("fsys", 0x80000000, {{"TXN", "MemCheck"}, {"memcheck.[]", "0"}, {"type", "0"}, {"salt", "5"}});
        }
        else if (p.type == "fsys" && (txn == "MemCheck" || txn == "Goodbye"))
        {
          // Answers / notifications, nothing to send.
        }
        else if (p.type == "fsys" && txn == "Ping")
        {
          if (p.flags & 0x40000000) // the client's own ping, not an answer to ours
          {
            Send("fsys", reply, {{"TXN", "Ping"}}, true);
          }
        }
        else if (p.type == "fsys" && txn == "GetPingSites")
        {
          Send("fsys", reply,
               {{"TXN", "GetPingSites"},
                {"minPingSitesToPing", "0"},
                {"pingSites.[]", "1"},
                {"pingSites.0.addr", IpString(server_ip())},
                {"pingSites.0.name", "local"},
                {"pingSites.0.type", "0"}});
        }
        else if (p.type == "acct" && txn == "XBL360Login")
        {
          name_ = Get(p.fields, "gamertag", "Player");
#ifdef AO2_OPENPORTROID_SERVER
          if (tunneled_)
          {
            // A name another player owns is shown as a guest's.
            std::string shown = nicks::LobbyName(name_, key_);
            if (shown != name_)
            {
              Log("FESL: %s is another player's name, shown as %s", name_.c_str(), shown.c_str());
              name_ = shown;
            }
          }
#endif
          std::string xuid = Get(p.fields, "xuid", "1");
          uint64_t uid = ToU64(xuid, 1) & 0x7FFFFFFF;
          {
            std::lock_guard lock(state().mutex);
            state().names[uid] = name_;
            state().xuids[uid] = xuid;
          }
          char lkey[32];
          std::snprintf(lkey, sizeof(lkey), "AO2LKEY%010llu", (unsigned long long)uid);
          Send("acct", reply,
               {{"TXN", "XBL360Login"},
                {"lkey", lkey},
                {"profileId", std::to_string(uid)},
                {"userId", std::to_string(uid)},
                {"nuid", name_},
                {"displayName", name_},
                {"personaName", name_},
                {"entitledGameFeatureWrappers.[]", "0"}});
        }
        else
        {
          Log("FESL: unhandled %s %s, echoing", p.type.c_str(), txn.c_str());
          Send(p.type, reply, {{"TXN", txn}});
        }
      }
    };

    // ---------------------------------------------------------------------
    // Theater: lobby
    // ---------------------------------------------------------------------

    class TheaterConn : public Conn
    {
    public:
      explicit TheaterConn(SOCKET s) : Conn(s, "THEATER") {}

      bool SendT(const std::string &type, const Fields &fields, const char (&status)[5])
      {
        return Send(type, Tag(status), fields);
      }

      uint64_t uid() const { return uid_; }

    protected:
      void KeepAlive() override { Send("PING", 0, {{"TID", "0"}}, true); }

      void OnClose() override
      {
        std::lock_guard lock(state().mutex);
        auto &games = state().games;
        for (auto it = games.begin(); it != games.end();)
        {
          if (it->second.host.get() == this)
          {
            Log("removing game %u (host left)", it->first);
            it = games.erase(it);
          }
          else
          {
            ++it;
          }
        }
      }

      std::shared_ptr<TheaterConn> self()
      {
        return std::static_pointer_cast<TheaterConn>(shared_from_this());
      }

      // The address `viewer` can reach `player` at: the address the server
      // sees the player at; for a player on the server's own machine
      // (loopback), wherever the viewer found the server; for two players
      // behind one NAT (the server sees both at the same address), the
      // player's own address from its XNADDR, since a router seldom forwards
      // its public address back inside.
      static uint32_t PlayerIpFor(const TheaterConn &player, const TheaterConn &viewer)
      {
        // A loopback address is only useless to a viewer on another machine: a second
        // instance on this PC (127.0.0.2) must be reached there, not at the server's.
        uint32_t ip = player.peer_ip();
        if (IsLoopback(ip))
        {
          return IsLoopback(viewer.peer_ip()) ? ip : viewer.server_ip();
        }
        if (ip == viewer.peer_ip() && player.own_ip_ && &player != &viewer && !IsLoopback(player.own_ip_))
        {
          return player.own_ip_;
        }
        return ip;
      }

      Fields Summary(const Game &game, const std::string &tid)
      {
        uint32_t host_ip = PlayerIpFor(*game.host, *this);
        Fields d = {{"TID", tid},
                    {"LID", "1"},
                    {"GID", std::to_string(game.gid)},
                    {"HN", game.host->name()},
                    {"HU", std::to_string(game.host->uid())},
                    {"N", Get(game.fields, "NAME", game.host->name())},
                    {"I", IpString(host_ip)},
                    {"P", Get(game.fields, "PORT", "1000")},
                    {"JP", std::to_string(game.joined)},
                    {"QP", "0"},
                    {"AP", std::to_string(game.joined)},
                    {"MP", Get(game.fields, "MAX-PLAYERS", "2")},
                    {"PL", "XBOX360"},
                    {"TYPE", Get(game.fields, "TYPE", "G")},
                    {"J", "O"},
                    {"F", "0"},
                    {"NF", "0"},
                    {"PW", "0"}};
        for (const auto &[k, v] : game.fields)
        {
          if (k.rfind("B-", 0) == 0 || k == "XB360SESS")
          {
            d.emplace_back(k, v);
          }
        }
        return d;
      }

      void OnPacket(const Packet &p) override
      {
        const Fields &f = p.fields;
        std::string tid = Get(f, "TID", "0");
        const std::string &t = p.type;
        auto &st = state();

        if (t == "CONN")
        {
          Send("CONN", 0,
               {{"TID", tid},
                {"TIME", std::to_string(std::time(nullptr))},
                {"activityTimeoutSecs", "0"},
                {"PROT", Get(f, "PROT", "2")}});
        }
        else if (t == "USER")
        {
          // LKEY = "AO2LKEY<userId>" (FESL login).
          std::string lkey = Get(f, "LKEY");
          size_t at = lkey.rfind("LKEY");
          uid_ = at == std::string::npos ? 0 : ToU64(lkey.substr(at + 4));
          {
            std::lock_guard lock(st.mutex);
            auto it = st.names.find(uid_);
            name_ = Get(f, "NAME");
            if (name_.empty())
            {
              name_ = it != st.names.end() ? it->second : "user" + std::to_string(uid_);
            }
          }
          Send("USER", 0, {{"TID", tid}, {"NAME", name_}, {"CID", ""}});
        }
        else if (t == "PING")
        {
        }
        else if (t == "LLST")
        {
          size_t count;
          {
            std::lock_guard lock(st.mutex);
            count = st.games.size();
          }
          Send("LLST", 0, {{"TID", tid}, {"NUM-LOBBIES", "1"}});
          Send("LDAT", 0,
               {{"TID", tid},
                {"FAVORITE-GAMES", "0"},
                {"FAVORITE-PLAYERS", "0"},
                {"LID", "1"},
                {"LOCALE", "en_US"},
                {"MAX-GAMES", "10000"},
                {"NAME", "ao2eu360"},
                {"NUM-GAMES", std::to_string(count)},
                {"PASSING", std::to_string(count)}});
        }
        else if (t == "PCNT")
        {
          int players = 0;
          {
            std::lock_guard lock(st.mutex);
            for (const auto &[gid, g] : st.games)
            {
              players += g.joined;
            }
          }
          Send("PCNT", 0, {{"TID", tid}, {"LID", Get(f, "LID", "1")}, {"COUNT", std::to_string(players)}});
        }
        else if (t == "CGAM")
        {
          Game game;
          game.host = self();
          game.fields = f;
          {
            std::lock_guard lock(st.mutex);
            game.gid = st.next_gid++;
            gid_ = game.gid;
            st.games[game.gid] = game;
          }
          Log("game %u created by %s", game.gid, name_.c_str());
          Send("CGAM", 0,
               {{"TID", tid},
                {"LID", "1"},
                {"GID", std::to_string(game.gid)},
                {"MAX-PLAYERS", Get(f, "MAX-PLAYERS", "2")},
                {"EKEY", "AO2EKEY"},
                {"UGID", Get(f, "UGID")},
                {"SECRET", Get(f, "SECRET")},
                {"JOIN", "O"},
                {"J", "O"},
                {"ISXBL", "1"}});
        }
        else if (t == "UGAM")
        {
          std::lock_guard lock(st.mutex);
          auto it = st.games.find(gid_);
          if (it != st.games.end())
          {
            for (const auto &[k, v] : f)
            {
              Set(it->second.fields, k, v);
            }
          }
          // No reply expected.
        }
        else if (t == "UBRA")
        {
          Send("UBRA", 0, {{"TID", tid}});
        }
        else if (t == "RGAM")
        {
          {
            // The game named (a versus host removes its party after creating the
            // game the party moves to, which is gid_ by then).
            std::lock_guard lock(st.mutex);
            st.games.erase(uint32_t(ToU64(Get(f, "GID", std::to_string(gid_)))));
          }
          Send("RGAM", 0, {{"TID", tid}});
        }
        else if (t == "GLST")
        {
          bool not_private = Get(f, "FILTER-NOT-PRIVATE") == "1";
          QuickMatchHook hook = g_quick_match.load();
          // Versus: the host's party session (TYPE=P, "Custom Match" screen) lives on
          // next to its game (TYPE=G, ready room); a quick match must find the game.
          std::string type = Get(f, "TYPE");
          std::vector<Fields> found;
          bool hosts_party = false; // this player leads a versus party (its own TYPE=P game here)
          {
            std::lock_guard lock(st.mutex);
            for (const auto &[gid, g] : st.games)
            {
              uint64_t max = ToU64(Get(g.fields, "MAX-PLAYERS", "2"), 2);
              if (g.host.get() == this && !g.host->closed() && Get(g.fields, "TYPE", "G") == "P")
              {
                hosts_party = true;
              }
              if (g.host.get() == this || g.host->closed() || uint64_t(g.joined) >= max ||
                  (not_private && Get(g.fields, "B-U-Private") == "1") ||
                  (!type.empty() && Get(g.fields, "TYPE", "G") != type))
              {
                continue;
              }
              found.push_back(Summary(g, tid));
            }
          }
          // Public game (Search Co-op Game, Quick Match) from this PC's own
          // game, ao2_public_game_search: games on this server, then elsewhere
          // (the hook sends it there through an invite; the list stays
          // unanswered). Without it no search at all: the list comes back
          // empty and the game hosts a public game (a Steam lobby everyone
          // sees in the server list).
          // A party's leader looks for a match game the same way when the party starts (then creates one): that
          // is no quick match, the party must not be taken to somebody else's lobby.
          bool own_quick = not_private && IsLoopback(peer_ip()) && Get(f, "TYPE", "G") == "G" && !hosts_party;
          if (hosts_party && not_private && IsLoopback(peer_ip()))
          {
            Log("game list of a party's leader: this server only");
          }
          if (own_quick && !PublicGameSearch())
          {
            Log("public game: hosting (no search)");
            found.clear();
          }
          else if (found.empty() && own_quick && hook)
          {
            std::string mode = Get(f, "FILTER-ATTR-U-Mode");
            Log("quick match (mode %s): nothing here, looking further", mode.c_str());
            if (hook(mode))
            {
              Log("quick match: sent to another game");
              return;
            }
          }
          std::string n = std::to_string(found.size());
          Send("GLST", 0,
               {{"TID", tid},
                {"LID", Get(f, "LID", "1")},
                {"LOBBY-NUM-GAMES", n},
                {"LOBBY-MAX-GAMES", "10000"},
                {"FAVORITE-GAMES", "0"},
                {"FAVORITE-PLAYERS", "0"},
                {"NUM-GAMES", n}});
          for (const auto &d : found)
          {
            Send("GDAT", 0, d);
          }
        }
        else if (t == "GDAT")
        {
          uint32_t gid = uint32_t(ToU64(Get(f, "GID")));
          Fields d, det;
          {
            std::lock_guard lock(st.mutex);
            auto it = st.games.find(gid);
            if (it != st.games.end())
            {
              d = Summary(it->second, tid);
              det = {{"TID", tid}, {"LID", "1"}, {"GID", std::to_string(gid)}, {"UGID", Get(it->second.fields, "UGID")}};
            }
          }
          if (d.empty())
          {
            Send("GDAT", 0, {{"TID", tid}});
          }
          else
          {
            Send("GDAT", 0, d);
            Send("GDET", 0, det);
          }
        }
        else if (t == "EGAM")
        {
          EnterGame(f, tid);
        }
        else if (t == "EGRS")
        {
          AnswerEnter(f, tid);
        }
        else if (t == "ECNL")
        {
          {
            // An entry given up: not asked again.
            std::lock_guard lock(st.mutex);
            uint32_t gid = uint32_t(ToU64(Get(f, "GID", "0")));
            std::erase_if(st.pending, [&](const auto &kv) {
              return kv.first.first == gid && kv.second.joiner.lock().get() == this;
            });
          }
          Send("ECNL", 0, {{"TID", tid}, {"LID", Get(f, "LID", "1")}, {"GID", Get(f, "GID", "0")}});
        }
        else if (t == "PENT")
        {
          Send("PENT", 0, {{"TID", tid}, {"PID", Get(f, "PID", "0")}});
        }
        else if (t == "PLVT")
        {
          {
            std::lock_guard lock(st.mutex);
            auto it = st.games.find(gid_);
            if (it != st.games.end() && it->second.joined > 1)
            {
              it->second.joined--;
            }
          }
          Send("PLVT", 0, {{"TID", tid}});
        }
        else
        {
          Log("THEATER: unhandled %s, echoing TID", t.c_str());
          Send(t, 0, {{"TID", tid}});
        }
      }

      // EGAM: ack, then the host gets EGRQ (also for its own entry).
      void EnterGame(const Fields &f, const std::string &tid)
      {
        auto &st = state();
        uint32_t gid = uint32_t(ToU64(Get(f, "GID")));
        std::shared_ptr<TheaterConn> host;
        uint32_t pid = 0;
        std::string xuid;
        {
          std::lock_guard lock(st.mutex);
          if (!gid && !Get(f, "XUID").empty())
          {
            // Joining from an invite: no GID, but the inviter's XUID (TYPE=G,
            // then TYPE=P) - the game that player hosts.
            std::string want = Get(f, "XUID");
            for (const auto &[id, g] : st.games)
            {
              auto x = g.host ? st.xuids.find(g.host->uid()) : st.xuids.end();
              if (x != st.xuids.end() && x->second == want && !g.host->closed())
              {
                gid = id;
                Log("%s enters %s's game %u by XUID (invite)", name_.c_str(), g.host->name().c_str(), id);
                break;
              }
            }
          }
          if (!gid)
          {
            gid = gid_;
          }
          auto it = st.games.find(gid);
          if (it != st.games.end())
          {
            host = it->second.host;
            if (host.get() == this)
            {
              it->second.xnaddr = Get(f, "R-XNADDR");
            }
            std::vector<uint8_t> own = Base64Decode(Get(f, "R-XNADDR"));
            if (own.size() == 36)
            {
              std::memcpy(&own_ip_, own.data(), 4);
            }
            pid = it->second.next_pid++;
            st.pending[{gid, pid}] = {self(), {}, std::chrono::steady_clock::now()};
          }
          auto x = st.xuids.find(uid_);
          xuid = x != st.xuids.end() ? x->second : Get(f, "R-U-XUID", "0");
        }
        if (!host)
        {
          SendT("EGAM", {{"TID", tid}, {"LID", "1"}, {"GID", std::to_string(gid)}}, "ngam");
          return;
        }
        Send("EGAM", 0, {{"TID", tid}, {"LID", "1"}, {"GID", std::to_string(gid)}});

        // Where the host finds this player: the address the server sees it at.
        uint32_t ip = host.get() == this ? local_ip() : PlayerIpFor(*this, *host);
        std::string ip_text = IpString(ip);
        std::string port = Get(f, "PORT", "1000");
        char ticket[16];
        std::snprintf(ticket, sizeof(ticket), "%010u", pid);
        Fields req = {{"TID", "0"},
                      {"NAME", name_},
                      {"UID", std::to_string(uid_)},
                      {"XUID", xuid},
                      {"PID", std::to_string(pid)},
                      {"TICKET", ticket},
                      {"IP", ip_text},
                      {"PORT", port},
                      {"INT-IP", ip_text},
                      {"INT-PORT", Get(f, "R-INT-PORT", port)},
                      {"PTYPE", Get(f, "PTYPE", "P")},
                      {"R-XNADDR", host.get() == this ? Get(f, "R-XNADDR") : RewriteXnAddr(Get(f, "R-XNADDR"), ip)},
                      {"LID", "1"},
                      {"GID", std::to_string(gid)}};
        for (const auto &[k, v] : f)
        {
          if (k.rfind("R-U-", 0) == 0 || k.rfind("R-USER", 0) == 0)
          {
            req.emplace_back(k, v);
          }
        }
        {
          std::lock_guard lock(st.mutex);
          auto p = st.pending.find({gid, pid});
          if (p != st.pending.end())
          {
            p->second.request = req;
          }
        }
#ifdef AO2_OPENPORTROID_SERVER
        // Two OpenPortroid players: they punch through to each other now,
        // before the host's game answers and the game traffic starts.
        if (host.get() != this && ao2::opnet::IsVirtual(peer_ip()) && ao2::opnet::IsVirtual(host->peer_ip()))
        {
          relay::Introduce(peer_ip(), host->peer_ip());
        }
#endif
        host->Send("EGRQ", 0, req);
      }

      // EGRS from the host: EGEG (or a refusal) to the player.
      void AnswerEnter(const Fields &f, const std::string &tid)
      {
        auto &st = state();
        uint32_t pid = uint32_t(ToU64(Get(f, "PID")));
        std::shared_ptr<TheaterConn> joiner;
        Game game;
        bool have_game = false;
        bool again = false;
        Fields request;
        uint32_t id = uint32_t(ToU64(Get(f, "GID", std::to_string(gid_))));
        {
          std::lock_guard lock(st.mutex);
          auto p = st.pending.find({id, pid});
          if (p != st.pending.end())
          {
            joiner = p->second.joiner.lock();
            // REASON=2: the host's game manager does not host until its own
            // EGEG is in (online_coop.md); a versus party following its leader
            // asks at once and gives up after three refusals. Asked again for
            // a while instead of refused.
            again = joiner && joiner.get() != this && !joiner->closed() && Get(f, "ALLOWED", "1") != "1" &&
                    Get(f, "REASON", "0") == "2" && !p->second.request.empty() &&
                    std::chrono::steady_clock::now() - p->second.since < kHostNotReadyWait;
            if (again)
            {
              request = p->second.request;
            }
            else
            {
              st.pending.erase(p);
            }
          }
          auto it = st.games.find(id);
          if (it != st.games.end())
          {
            if (joiner && joiner.get() != this && Get(f, "ALLOWED", "1") == "1")
            {
              it->second.joined++;
            }
            game = it->second;
            have_game = true;
          }
        }
        Send("EGRS", 0, {{"TID", tid}});
        if (again)
        {
          Log("host %s not hosting yet, asks again for %s", name_.c_str(), joiner->name().c_str());
          std::weak_ptr<TheaterConn> host = self();
          std::weak_ptr<TheaterConn> player = joiner;
          std::thread([host, player, id, pid, request]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto h = host.lock();
            {
              std::lock_guard lock(state().mutex);
              if (!state().pending.count({id, pid}))
              {
                return; // given up (ECNL)
              }
              auto it = state().games.find(id);
              if (!h || h->closed() || it == state().games.end() || it->second.host != h)
              {
                state().pending.erase({id, pid});
                h = nullptr;
              }
            }
            if (!h)
            {
              // The game gone meanwhile: the refusal after all.
              if (auto p = player.lock())
              {
                p->SendT("EGEG", {{"TID", "0"}, {"LID", "1"}, {"GID", std::to_string(id)}, {"REASON", "2"}}, "jden");
              }
              return;
            }
            h->Send("EGRQ", 0, request);
          }).detach();
          return;
        }
        if (!joiner || !have_game)
        {
          return;
        }
        std::string gid = std::to_string(game.gid);
        if (Get(f, "ALLOWED", "1") != "1")
        {
          joiner->SendT("EGEG", {{"TID", "0"}, {"LID", "1"}, {"GID", gid}, {"REASON", Get(f, "REASON", "0")}},
                        "jden");
          Log("host %s refused %s (reason %s)", name_.c_str(), joiner->name().c_str(),
              Get(f, "REASON", "0").c_str());
          return;
        }
        uint32_t host_ip = PlayerIpFor(*game.host, *joiner);
        std::string host_ip_text = IpString(host_ip);
        std::string port = Get(game.fields, "PORT", "1000");
        char ticket[16];
        std::snprintf(ticket, sizeof(ticket), "%010u", pid);
        joiner->Send("EGEG", 0,
                     {{"TID", "0"},
                      {"PL", "xbox360"},
                      {"TICKET", ticket},
                      {"PID", std::to_string(pid)},
                      {"HUID", std::to_string(uid_)},
                      {"I", host_ip_text},
                      {"P", port},
                      {"INT-IP", host_ip_text},
                      {"INT-PORT", Get(game.fields, "INT-PORT", port)},
                      {"EKEY", "AO2EKEY"},
                      {"UGID", Get(game.fields, "UGID")},
                      {"SECRET", Get(game.fields, "SECRET")},
                      {"XNADDR", joiner.get() == this ? game.xnaddr : RewriteXnAddr(game.xnaddr, host_ip)},
                      {"XB360SESS", Get(game.fields, "XB360SESS")},
                      {"LID", "1"},
                      {"GID", gid}});
        if (joiner.get() != this)
        {
          Log("%s joined %s's game %s (host at %s)", joiner->name().c_str(), name_.c_str(), gid.c_str(),
              host_ip_text.c_str());
        }
      }

    private:
      uint64_t uid_ = 0;
      uint32_t gid_ = 0;
      uint32_t own_ip_ = 0; // the address in the player's own XNADDR (EGAM)
    };

    // ---------------------------------------------------------------------
    // Messenger: presence (accepted, otherwise ignored)
    // ---------------------------------------------------------------------

    class MessengerConn : public Conn
    {
    public:
      explicit MessengerConn(SOCKET s) : Conn(s, "MESSENGER") {}

    protected:
      void OnPacket(const Packet &p) override
      {
        if (p.type == "AUTH")
        {
          Send("AUTH", 0, {{"TITL", "Army of Two"}, {"ID", Get(p.fields, "ID", "1")}});
        }
        else if (p.type != "PING")
        {
          Send(p.type, 0, {{"ID", Get(p.fields, "ID", "0")}});
        }
      }
    };

    // ---------------------------------------------------------------------
    // Listeners
    // ---------------------------------------------------------------------

    template <typename T>
    bool Listen(uint16_t port)
    {
      SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
      addr.sin_addr.s_addr = INADDR_ANY;
#ifndef _WIN32
      int reuse = 1; // restart while old connections are in TIME_WAIT
      if (s != INVALID_SOCKET)
      {
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
      }
#endif
      if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
          listen(s, 16) != 0)
      {
        Log("ERROR: cannot listen on TCP %u (error %d) - is another server running?", port, WSAGetLastError());
        if (s != INVALID_SOCKET)
        {
          closesocket(s);
        }
        return false;
      }
      Log("listening on TCP %u", port);
      std::thread([s]() {
        while (true)
        {
          SOCKET c = accept(s, nullptr, nullptr);
          if (c == INVALID_SOCKET)
          {
            Log("accept failed (error %d)", WSAGetLastError());
            break;
          }
          int nodelay = 1;
          setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
          auto conn = std::make_shared<T>(c);
          std::thread([conn]() { conn->Run(); }).detach();
        }
      }).detach();
      return true;
    }

    // LAN discovery: the launcher broadcasts a 16-byte query (magic "AO2D", version, type, nonce, all
    // big-endian uint32) to UDP kDiscoveryPort; a running host answers with type 2 and the same nonce.
    void StartDiscoveryResponder()
    {
      SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
      addr.sin_port = htons(kDiscoveryPort);
      int reuse = 1;
      if (s != INVALID_SOCKET)
      {
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
      }
      if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
      {
        Log("ERROR: cannot bind UDP %u for LAN discovery (error %d)", kDiscoveryPort, WSAGetLastError());
        if (s != INVALID_SOCKET)
        {
          closesocket(s);
        }
        return;
      }
      Log("LAN discovery responder on UDP %u", kDiscoveryPort);
      std::thread([s]() {
        constexpr uint32_t kMagic = 0x414F3244; // "AO2D"
        while (true)
        {
          uint32_t packet[4] = {};
          sockaddr_in from = {};
          socklen_t from_len = sizeof(from);
          int n = recvfrom(s, reinterpret_cast<char *>(packet), sizeof(packet), 0, reinterpret_cast<sockaddr *>(&from),
                           &from_len);
          if (n < 0)
          {
            int err = WSAGetLastError();
#ifdef _WIN32
            if (err == WSAECONNRESET || err == WSAEMSGSIZE)
#else
            if (err == ECONNREFUSED || err == EINTR)
#endif
            {
              continue; // ICMP port unreachable from an earlier reply / oversized datagram
            }
            Log("LAN discovery stopped (error %d)", err);
            break;
          }
          if (n != sizeof(packet) || ntohl(packet[0]) != kMagic || ntohl(packet[1]) != 1 || ntohl(packet[2]) != 1)
          {
            continue;
          }
          packet[2] = htonl(2);
          sendto(s, reinterpret_cast<const char *>(packet), sizeof(packet), 0, reinterpret_cast<sockaddr *>(&from),
                 from_len);
          Log("LAN discovery query from %s", IpString(from.sin_addr.s_addr).c_str());
        }
      }).detach();
    }

    // The presence service's GAME packet (coop_invite.cpp) for the game the
    // player `name` hosts here; empty when it hosts none.
    std::string GameReply(const std::string &name)
    {
      HostedGame game;
      if (name.empty() || !FindHostedGame(name, &game))
      {
        return "";
      }
      char session[32], xuid[32];
      std::snprintf(session, sizeof(session), "%016llx", (unsigned long long)game.session_id);
      std::snprintf(xuid, sizeof(xuid), "%016llx", (unsigned long long)game.host_xuid);
      std::string hex;
      for (uint8_t b : game.xnaddr)
      {
        char h[3];
        std::snprintf(h, sizeof(h), "%02x", b);
        hex += h;
      }
      return "AO2I1 GAME\ngid=" + std::to_string(game.gid) + "\nsession=" + session + "\nxnaddr=" + hex +
             "\nxuid=" + xuid + "\nprivate=" + (game.is_private ? "1" : "0") +
             "\nplaygroup=" + (game.playgroup ? "1" : "0") + "\nplayers=" + std::to_string(game.players) + "\n";
    }

#ifdef AO2_STANDALONE_SERVER
    // The games' presence service (coop_invite.cpp, UDP 18491, "AO2I1 <TYPE>"
    // then "key=value" lines). A game whose server runs in another process
    // asks it for the game it hosts (GAMEQ name) to put it into its invites;
    // in a game with ao2_coop_server its own presence answers, here this does.
    void StartPresenceResponder()
    {
      constexpr uint16_t kPresencePort = 18491;
      SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
      addr.sin_port = htons(kPresencePort);
      if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
      {
        Log("ERROR: cannot bind UDP %u for invites (error %d)", kPresencePort, WSAGetLastError());
        if (s != INVALID_SOCKET)
        {
          closesocket(s);
        }
        return;
      }
      Log("invite queries on UDP %u", kPresencePort);
      std::thread([s]() {
        char packet[2048];
        while (true)
        {
          sockaddr_in from = {};
          socklen_t from_len = sizeof(from);
          int n = recvfrom(s, packet, sizeof(packet), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
          if (n < 0)
          {
            continue; // ICMP port unreachable from an earlier reply
          }
          std::string text(packet, size_t(n));
          if (text.rfind("AO2I1 GAMEQ\n", 0) != 0)
          {
            continue; // HELLO / HERE: the games' own presence, nothing for the server
          }
          std::string name;
          size_t at = text.find("\nname=");
          if (at != std::string::npos)
          {
            at += 6;
            name = text.substr(at, text.find('\n', at) - at);
          }
          std::string reply = GameReply(name);
          if (!reply.empty())
          {
            sendto(s, reply.data(), int(reply.size()), 0, reinterpret_cast<sockaddr *>(&from), from_len);
          }
        }
      }).detach();
    }
#endif

    // ---------------------------------------------------------------------
    // Tunnel: players on the internet reach the public server through nginx
    // (its router only forwards HTTP). The game listens on 127.0.0.1 at the
    // three ports (StartOpenPortroid) and opens, for each connection, an HTTP
    // request to /aot1/<fesl|theater|messenger> with "Upgrade: aot1-tunnel"
    // (openportroid.h);
    // nginx passes it with the player's address (X-Real-IP) to the server's
    // bridge (StartTunnelBridge), which answers 101 and from then on both
    // sides carry the plain FESL / Theater / Messenger bytes. GAMEQ (the game
    // a player hosts, for invites) is a plain GET /aot1/gameq?name=<name>.
    // ---------------------------------------------------------------------

    using ao2::opnet::kTunnelUpgrade;

    // A path under the tunnel's ("/aot1/" + what).
    std::string TunnelPath(const std::string &what)
    {
      return std::string(ao2::opnet::kTunnelPath) + what;
    }

    bool StartsWith(const std::string &text, const std::string &head)
    {
      return text.rfind(head, 0) == 0;
    }

    std::string UrlEncode(const std::string &text)
    {
      std::string out;
      for (unsigned char c : text)
      {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
        {
          out += char(c);
        }
        else
        {
          char b[4];
          std::snprintf(b, sizeof(b), "%%%02X", c);
          out += b;
        }
      }
      return out;
    }

    std::string UrlDecode(const std::string &text)
    {
      std::string out;
      for (size_t i = 0; i < text.size(); i++)
      {
        if (text[i] == '%' && i + 2 < text.size())
        {
          out += char(std::strtoul(text.substr(i + 1, 2).c_str(), nullptr, 16));
          i += 2;
        }
        else
        {
          out += text[i] == '+' ? ' ' : text[i];
        }
      }
      return out;
    }

    void SetTimeout(SOCKET s, int seconds)
    {
#ifdef _WIN32
      DWORD ms = DWORD(seconds) * 1000;
      setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms));
#else
      timeval tv = {seconds, 0};
      setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
#endif
    }

    bool SendAll(SOCKET s, const std::string &data)
    {
      size_t sent = 0;
      while (sent < data.size())
      {
        int n = send(s, data.data() + sent, int(data.size() - sent), kSendFlags);
        if (n <= 0)
        {
          return false;
        }
        sent += size_t(n);
      }
      return true;
    }

    // Reads an HTTP head (up to the blank line); `rest` gets what came after it.
    bool ReadHead(SOCKET s, std::string *head, std::string *rest)
    {
      std::string buffer;
      char chunk[2048];
      while (buffer.size() < 16384)
      {
        size_t end = buffer.find("\r\n\r\n");
        if (end != std::string::npos)
        {
          *head = buffer.substr(0, end + 2);
          *rest = buffer.substr(end + 4);
          return true;
        }
        int n = recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0)
        {
          return false;
        }
        buffer.append(chunk, size_t(n));
      }
      return false;
    }

    // A header's value from an HTTP head ("Name: value"), case-insensitive name.
    std::string Header(const std::string &head, const std::string &name)
    {
      size_t at = 0;
      while ((at = head.find("\r\n", at)) != std::string::npos)
      {
        at += 2;
        if (head.size() - at > name.size() && head[at + name.size()] == ':' &&
            std::equal(name.begin(), name.end(), head.begin() + at,
                       [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); }))
        {
          size_t v = head.find_first_not_of(' ', at + name.size() + 1);
          return v == std::string::npos ? "" : head.substr(v, head.find("\r\n", v) - v);
        }
      }
      return "";
    }

    template <typename T>
    void RunTunneled(SOCKET c, uint32_t ip, uint32_t local, const std::string &key)
    {
      auto conn = std::make_shared<T>(c);
      conn->SetTunnel(ip, local, key);
      conn->Run();
    }

    void SendText(SOCKET c, const std::string &body)
    {
      SendAll(c, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) +
                     "\r\nConnection: close\r\n\r\n" + body);
    }

    // "name=<value>" from a path's query, decoded.
    std::string QueryName(const std::string &path)
    {
      size_t q = path.find("name=");
      return q == std::string::npos ? "" : UrlDecode(path.substr(q + 5, path.find('&', q) - q - 5));
    }

    void ServeTunnel(SOCKET c)
    {
      SetTimeout(c, 10);
      std::string head, rest;
      if (!ReadHead(c, &head, &rest))
      {
        closesocket(c);
        return;
      }
      // "GET /aot1/theater HTTP/1.1"
      size_t sp = head.find(' ');
      std::string path = sp == std::string::npos ? "" : head.substr(sp + 1, head.find(' ', sp + 1) - sp - 1);
      // nginx sets X-Real-IP; without it (a test without nginx) the player
      // is where the connection comes from.
      uint32_t ip = ParseIp(Header(head, "X-Real-IP"));
      if (!ip)
      {
        sockaddr_in a = {};
        socklen_t len = sizeof(a);
        if (getpeername(c, reinterpret_cast<sockaddr *>(&a), &len) == 0)
        {
          ip = a.sin_addr.s_addr;
        }
      }
      // Where that game's own tunnel listens (its net_bind_ip; 127.0.0.1).
      uint32_t local = ParseIp(Header(head, "X-AO2-Local"));
      if (!IsLoopback(local))
      {
        local = htonl(INADDR_LOOPBACK);
      }
      std::string key = Header(head, "X-AO2-Key");
#ifdef AO2_OPENPORTROID_SERVER
      // A player signed in to the relay is seen (and shown to the others) at
      // its virtual address: its game traffic goes through the relay.
      if (uint32_t vip = relay::VirtualIpForKey(key); vip && ip)
      {
        ip = vip;
      }
      // Player names (nick_registry.h): the launcher checks and claims a name
      // with its key; a game finds a friend's virtual address by name.
      //   /aot1/nick?name=N  (X-AO2-Key) -> free | yours | taken | invalid
      //   /aot1/claim?name=N (X-AO2-Key) -> yours | taken | invalid
      //   /aot1/find?name=N              -> online <virtual ip> | offline | unknown
      if (StartsWith(path, TunnelPath("nick?")) || StartsWith(path, TunnelPath("claim?")))
      {
        std::string name = QueryName(path);
        bool claim = StartsWith(path, TunnelPath("claim?"));
        nicks::Status s = claim ? nicks::Claim(name, key) : nicks::Check(name, key);
        if (claim)
        {
          Log("name %s claimed: %s", name.c_str(), nicks::StatusName(s));
        }
        SendText(c, nicks::StatusName(s));
        closesocket(c);
        return;
      }
      // The statistics page's numbers now (the server repository's stats.py).
      if (path == TunnelPath("stats"))
      {
        int games = 0, in_games = 0;
        {
          auto &st = state();
          std::lock_guard lock(st.mutex);
          for (const auto &[gid, g] : st.games)
          {
            if (g.host && !g.host->closed())
            {
              games++;
              in_games += g.joined;
            }
          }
        }
        char json[160];
        std::snprintf(json, sizeof(json), "{\"online\":%d,\"games\":%d,\"in_games\":%d,\"accounts\":%d}",
                      relay::OnlineCount(), games, in_games, nicks::Count());
        SendText(c, json);
        closesocket(c);
        return;
      }
      if (StartsWith(path, TunnelPath("find?")))
      {
        std::string owner = nicks::KeyFor(QueryName(path));
        uint32_t vip = owner.empty() ? 0 : relay::VirtualIpForKey(owner);
        SendText(c, owner.empty() ? "unknown" : vip ? "online " + IpString(vip) : "offline");
        closesocket(c);
        return;
      }
#endif
      if (StartsWith(path, TunnelPath("gameq?")))
      {
        SendText(c, GameReply(QueryName(path)));
        closesocket(c);
        return;
      }
      std::string kind = StartsWith(path, TunnelPath("")) ? path.substr(TunnelPath("").size()) : "";
      bool upgrade = Header(head, "Upgrade") == kTunnelUpgrade;
      if (!ip || !upgrade || !rest.empty() || (kind != "fesl" && kind != "theater" && kind != "messenger"))
      {
        Log("tunnel: bad request %s from %s", path.c_str(), Header(head, "X-Real-IP").c_str());
        SendAll(c, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        closesocket(c);
        return;
      }
      SetTimeout(c, 0);
      if (!SendAll(c, std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: ") + kTunnelUpgrade +
                          "\r\nConnection: Upgrade\r\n\r\n"))
      {
        closesocket(c);
        return;
      }
      int nodelay = 1;
      setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
      if (kind == "fesl")
      {
        RunTunneled<FeslConn>(c, ip, local, key);
      }
      else if (kind == "theater")
      {
        RunTunneled<TheaterConn>(c, ip, local, key);
      }
      else
      {
        RunTunneled<MessengerConn>(c, ip, local, key);
      }
    }

#ifndef AO2_STANDALONE_SERVER
    std::atomic<bool> g_tunnel{false};
    std::string g_tunnel_host;     // the server's name (the Host header)
    std::atomic<uint32_t> g_tunnel_ip{0};
    uint16_t g_tunnel_port = 80;   // the server's HTTP port (nginx)
    uint32_t g_tunnel_local = 0;   // where this game's tunnel listens

    // An HTTP connection to the public server: the request sent, the answer's
    // head read. INVALID_SOCKET when it cannot be reached.
    SOCKET TunnelOpen(const std::string &request, std::string *head, std::string *rest)
    {
      SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (s == INVALID_SOCKET)
      {
        return s;
      }
      sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(g_tunnel_port);
      addr.sin_addr.s_addr = g_tunnel_ip;
      SetTimeout(s, 10);
      if (connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || !SendAll(s, request) ||
          !ReadHead(s, head, rest))
      {
        closesocket(s);
        return INVALID_SOCKET;
      }
      return s;
    }

    // Copies one direction until it closes, then ends both.
    void Pump(SOCKET from, SOCKET to)
    {
      char chunk[16384];
      while (true)
      {
        int n = recv(from, chunk, sizeof(chunk), 0);
        if (n <= 0 || !SendAll(to, std::string(chunk, size_t(n))))
        {
          break;
        }
      }
#ifdef _WIN32
      shutdown(from, SD_BOTH);
      shutdown(to, SD_BOTH);
#else
      shutdown(from, SHUT_RDWR);
      shutdown(to, SHUT_RDWR);
#endif
    }

    void ForwardToTunnel(SOCKET local, const std::string &kind)
    {
      std::string head, rest;
      std::string key = opnet::KeyHex();
      SOCKET remote = TunnelOpen("GET " + TunnelPath(kind) + " HTTP/1.1\r\nHost: " + g_tunnel_host + "\r\nUpgrade: " +
                                     kTunnelUpgrade + "\r\nConnection: Upgrade\r\nX-AO2-Local: " +
                                     IpString(g_tunnel_local) + "\r\n" +
                                     (key.empty() ? "" : "X-AO2-Key: " + key + "\r\n") + "\r\n",
                                 &head, &rest);
      if (remote == INVALID_SOCKET || head.find(" 101 ") == std::string::npos)
      {
        Log("tunnel %s: %s answered %s", kind.c_str(), g_tunnel_host.c_str(),
            remote == INVALID_SOCKET ? "nothing" : head.substr(0, head.find("\r\n")).c_str());
        if (remote != INVALID_SOCKET)
        {
          closesocket(remote);
        }
        closesocket(local);
        return;
      }
      SetTimeout(remote, 0);
      int nodelay = 1;
      setsockopt(remote, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
      Log("tunnel %s open", kind.c_str());
      if (!rest.empty())
      {
        SendAll(local, rest);
      }
      std::thread up([local, remote]() { Pump(local, remote); });
      Pump(remote, local);
      up.join();
      closesocket(remote);
      closesocket(local);
      Log("tunnel %s closed", kind.c_str());
    }

    // 127.0.0.1:port -> the tunnel. False when the port is taken (another
    // game on this PC already forwards it, or runs a server).
    bool ListenTunnel(uint16_t port, const std::string &kind)
    {
      SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
      addr.sin_addr.s_addr = g_tunnel_local;
      if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
          listen(s, 16) != 0)
      {
        if (s != INVALID_SOCKET)
        {
          closesocket(s);
        }
        return false;
      }
      std::thread([s, kind]() {
        while (true)
        {
          SOCKET c = accept(s, nullptr, nullptr);
          if (c == INVALID_SOCKET)
          {
            break;
          }
          int nodelay = 1;
          setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
          std::thread([c, kind]() { ForwardToTunnel(c, kind); }).detach();
        }
      }).detach();
      return true;
    }
#endif
  } // namespace

  uint32_t ResolveIp(const std::string &address)
  {
    if (uint32_t ip = ParseIp(address))
    {
      return ip;
    }
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (address.empty() || getaddrinfo(address.c_str(), nullptr, &hints, &res) != 0 || !res)
    {
      return 0;
    }
    uint32_t ip = reinterpret_cast<sockaddr_in *>(res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return ip;
  }

  bool SetPublicAddress(const std::string &address)
  {
    uint32_t ip = ResolveIp(address);
    g_public_ip = ip;
    if (ip)
    {
      Log("public address: %s (%s), given to players outside the LAN", IpString(ip).c_str(), address.c_str());
    }
    return ip != 0;
  }

  std::vector<LocalAddress> LocalAddresses()
  {
    std::vector<LocalAddress> out;
#ifdef _WIN32
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
      char name[256] = {};
      WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
      for (auto *u = a->FirstUnicastAddress; u; u = u->Next)
      {
        auto *in = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
        uint32_t ip = in->sin_addr.s_addr;
        bool radmin = std::strstr(name, "Radmin") != nullptr || (ntohl(ip) >> 24) == 26;
        out.push_back({IpString(ip), name, radmin});
      }
    }
#else
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0)
    {
      return out;
    }
    for (auto *a = list; a; a = a->ifa_next)
    {
      if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) || a->ifa_flags & IFF_LOOPBACK)
      {
        continue;
      }
      uint32_t ip = reinterpret_cast<sockaddr_in *>(a->ifa_addr)->sin_addr.s_addr;
      out.push_back({IpString(ip), a->ifa_name, (ntohl(ip) >> 24) == 26});
    }
    freeifaddrs(list);
#endif
    return out;
  }

  bool StartServer(bool log_to_console, bool listen)
  {
    g_log_console = log_to_console;
#ifdef _WIN32
    WSADATA wsa = {};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    for (const auto &a : LocalAddresses())
    {
      Log("this computer: %s (%s)%s", a.ip.c_str(), a.adapter.c_str(), a.radmin ? "  <- Radmin VPN" : "");
    }
    if (!listen)
    {
      Log("no TCP ports of its own: players come through the tunnel only");
      return true;
    }
    bool ok = Listen<FeslConn>(kFeslPort);
    ok = Listen<TheaterConn>(kTheaterPort) && ok;
    ok = Listen<MessengerConn>(kMessengerPort) && ok;
    if (ok)
    {
      StartDiscoveryResponder();
    }
    return ok;
  }

  void LogMessage(const std::string &text) { Log("%s", text.c_str()); }

  bool FindHostedGame(const std::string &host_name, HostedGame *out)
  {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    for (const auto &[gid, g] : st.games)
    {
      if (!g.host || g.host->closed() || g.host->name() != host_name || g.xnaddr.empty())
      {
        continue;
      }
      out->gid = gid;
      // XB360SESS: the session id as a signed decimal (online sessions have the top bit set).
      out->session_id = uint64_t(std::strtoll(Get(g.fields, "XB360SESS", "0").c_str(), nullptr, 10));
      out->xnaddr = Base64Decode(g.xnaddr);
      auto x = st.xuids.find(g.host->uid());
      out->host_xuid = x != st.xuids.end() ? ToU64(x->second) : 0;
      out->is_private = Get(g.fields, "B-U-Private") == "1";
      out->players = g.joined;
      out->playgroup = Get(g.fields, "TYPE") == "P";
      out->max_players = int(ToU64(Get(g.fields, "MAX-PLAYERS", "2"), 2));
      out->map = Get(g.fields, "B-U-Map");
      std::string type = Get(g.fields, "B-U-GameType");
      out->game_type = type.empty() ? -1 : std::atoi(type.c_str());
      std::string difficulty = Get(g.fields, "B-U-Difficulty");
      out->difficulty = difficulty.empty() ? -1 : std::atoi(difficulty.c_str());
      out->mode = Get(g.fields, "B-U-Mode");
      return out->session_id != 0 && out->xnaddr.size() == 36;
    }
    return false;
  }

  void SetQuickMatchHook(QuickMatchHook hook)
  {
    g_quick_match = hook;
  }

#ifdef AO2_STANDALONE_SERVER
  void StartInviteQueries()
  {
    StartPresenceResponder();
  }
#endif

  bool StartTunnelBridge(uint16_t port)
  {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // nginx only
#ifndef _WIN32
    int reuse = 1;
    if (s != INVALID_SOCKET)
    {
      setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
    }
#endif
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        listen(s, 64) != 0)
    {
      Log("ERROR: cannot listen on 127.0.0.1:%u for the tunnel (error %d)", port, WSAGetLastError());
      if (s != INVALID_SOCKET)
      {
        closesocket(s);
      }
      return false;
    }
    Log("tunnel bridge on 127.0.0.1:%u (nginx: %s)", port, ao2::opnet::kTunnelPath);
    std::thread([s]() {
      while (true)
      {
        SOCKET c = accept(s, nullptr, nullptr);
        if (c == INVALID_SOCKET)
        {
          break;
        }
        std::thread([c]() { ServeTunnel(c); }).detach();
      }
    }).detach();
    return true;
  }

#ifndef AO2_STANDALONE_SERVER
  bool TunnelActive()
  {
    return g_tunnel;
  }

  bool TunnelGameQuery(const std::string &name, std::string *packet)
  {
    if (!g_tunnel)
    {
      return false;
    }
    std::string head, rest;
    SOCKET s = TunnelOpen("GET " + TunnelPath("gameq?name=") + UrlEncode(name) + " HTTP/1.1\r\nHost: " +
                              g_tunnel_host + "\r\nConnection: close\r\n\r\n",
                          &head, &rest);
    if (s == INVALID_SOCKET)
    {
      return false;
    }
    char chunk[2048];
    int n;
    while ((n = recv(s, chunk, sizeof(chunk), 0)) > 0)
    {
      rest.append(chunk, size_t(n));
    }
    closesocket(s);
    *packet = rest;
    return head.find(" 200 ") != std::string::npos && !rest.empty();
  }

  std::string TunnelFindPlayer(const std::string &name, uint32_t *vip)
  {
    *vip = 0;
    if (!g_tunnel)
    {
      return "error";
    }
    std::string head, rest;
    SOCKET s = TunnelOpen("GET " + TunnelPath("find?name=") + UrlEncode(name) + " HTTP/1.1\r\nHost: " +
                              g_tunnel_host + "\r\nConnection: close\r\n\r\n",
                          &head, &rest);
    if (s == INVALID_SOCKET)
    {
      return "error";
    }
    char chunk[512];
    int n;
    while ((n = recv(s, chunk, sizeof(chunk), 0)) > 0)
    {
      rest.append(chunk, size_t(n));
    }
    closesocket(s);
    if (head.find(" 200 ") == std::string::npos)
    {
      return "error";
    }
    if (rest.rfind("online ", 0) == 0)
    {
      *vip = ParseIp(rest.substr(7));
      return *vip ? "online" : "error";
    }
    return rest == "offline" || rest == "unknown" ? rest : "error";
  }

  std::string NetMode()
  {
    std::string mode = REXCVAR_GET(ao2_net_mode);
    return mode == "openportroid" || mode == "lan" ? mode : "steam";
  }

  namespace
  {
    // The OpenPortroid server: the lobby through the tunnel (127.0.0.1 here),
    // the game traffic over its UDP port (openportroid_net.cpp).
    void StartOpenPortroid()
    {
      // "host" or "host:port" (the HTTP port, 80 by default; a test
      // without nginx gives the server's --tunnel-port).
      std::string server = REXCVAR_GET(ao2_openportroid_server);
      if (size_t colon = server.find(':'); colon != std::string::npos)
      {
        g_tunnel_port = uint16_t(std::atoi(server.c_str() + colon + 1));
        server.resize(colon);
      }
      uint32_t ip = ResolveIp(server);
      if (!ip)
      {
        Log("OpenPortroid server %s does not resolve: no online play", server.c_str());
        return;
      }
      opnet::Start(server, uint16_t(REXCVAR_GET(ao2_openportroid_port)), REXCVAR_GET(ao2_openportroid_key));
      // The tunnel tells the server this game's key: wait a moment for the
      // sign-in, so the lobby shows this player at its virtual address.
      uint32_t vip = opnet::SelfIp(1500);
      g_tunnel_host = server;
      g_tunnel_ip = ip;
      // Two games on one PC (tests) each have a loopback address of their own.
      uint32_t bind_ip = ParseIp(rex::cvar::GetFlagByName("net_bind_ip"));
      g_tunnel_local = IsLoopback(bind_ip) ? bind_ip : htonl(INADDR_LOOPBACK);
      int bound = 0;
      bound += ListenTunnel(kFeslPort, "fesl");
      bound += ListenTunnel(kTheaterPort, "theater");
      bound += ListenTunnel(kMessengerPort, "messenger");
      g_tunnel = true;
      rex::cvar::SetFlagByName("net_title_server_ip", IpString(g_tunnel_local));
      Log("OpenPortroid server %s (%s): lobby through the tunnel (%d of 3 ports here%s), this player %s", server.c_str(),
          IpString(ip).c_str(), bound, bound < 3 ? ", the rest another game's on this PC" : "",
          vip ? IpString(vip).c_str() : "not signed in yet (game traffic straight, no NAT punching)");
    }
  } // namespace

  void Start()
  {
#ifdef _WIN32
    WSADATA wsa = {};
    WSAStartup(MAKEWORD(2, 2), &wsa); // host names are looked up before the server starts
#endif
    if (NetMode() == "openportroid")
    {
      StartOpenPortroid();
      return;
    }
    std::string host = REXCVAR_GET(ao2_coop_host);
    bool server = REXCVAR_GET(ao2_coop_server);
    if (!host.empty())
    {
      // Partner: the game asks XOnlineGetServiceInfo for EA's servers.
      rex::cvar::SetFlagByName("net_title_server_ip", host);
    }
    if (!server)
    {
      return;
    }
    if (host.empty() && rex::cvar::GetFlagByName("net_title_server_ip").empty())
    {
      // The host's own game uses its server; a socket bound to a VPN address
      // (net_bind_ip) reaches it there, otherwise over loopback.
      std::string bind = rex::cvar::GetFlagByName("net_bind_ip");
      rex::cvar::SetFlagByName("net_title_server_ip", bind.empty() ? "127.0.0.1" : bind);
    }
    Log("Army of Two co-op server (game connects to %s)", rex::cvar::GetFlagByName("net_title_server_ip").c_str());
    StartServer(false);
  }
#endif
} // namespace ao2::ea
