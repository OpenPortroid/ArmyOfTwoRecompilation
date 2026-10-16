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
#include "ea_server.h"
#include "log_files.h"

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <chrono>
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

REXCVAR_DEFINE_BOOL(ao2_coop_server, false, "Online",
                    "Online co-op host: run the EA lobby server (TCP 18480, 18485, 13505) in this game. "
                    "The partner connects with ao2_coop_host = this computer's IP (VPN/LAN/public).");
REXCVAR_DEFINE_STRING(ao2_coop_host, "", "Online",
                      "Online co-op partner: IP address of the player who runs ao2_coop_server.");
#endif

namespace ao2::ea
{
  namespace
  {
    constexpr uint16_t kFeslPort = 18480;
    constexpr uint16_t kTheaterPort = 18485;
    constexpr uint16_t kMessengerPort = 13505;
    constexpr uint16_t kDiscoveryPort = 18490;
    constexpr auto kKeepAlive = std::chrono::seconds(30);

    using Fields = std::vector<std::pair<std::string, std::string>>;

    // ---------------------------------------------------------------------
    // Log (ea_server.log in the working directory)
    // ---------------------------------------------------------------------

    std::mutex g_log_mutex;
    bool g_log_console = false;

    void Log(const char *fmt, ...)
    {
      std::lock_guard lock(g_log_mutex);
      static FILE *f = ao2::OpenLogFile("ea_server.log");
      std::time_t now = std::time(nullptr);
      std::tm local = {};
      localtime_s(&local, &now);
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

    bool IsLoopback(uint32_t ip)
    {
      return (ntohl(ip) >> 24) == 127;
    }

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

    // ---------------------------------------------------------------------
    // Connections
    // ---------------------------------------------------------------------

    class Conn : public std::enable_shared_from_this<Conn>
    {
    public:
      Conn(SOCKET s, const char *kind) : socket_(s), kind_(kind)
      {
        sockaddr_in a = {};
        int len = sizeof(a);
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
          int ready = select(0, &fds, nullptr, nullptr, &tv);
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
          int n = send(socket_, data.data() + sent, int(data.size() - sent), 0);
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
    };

    struct State
    {
      std::mutex mutex;
      std::map<uint32_t, Game> games;
      uint32_t next_gid = 1;
      uint32_t next_pid = 1;
      std::map<uint32_t, std::weak_ptr<TheaterConn>> pending; // pid -> entering player
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
          std::string ip = IpString(local_ip());
          char now[64];
          std::time_t t = std::time(nullptr);
          std::tm utc = {};
          gmtime_s(&utc, &t);
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
                {"pingSites.0.addr", IpString(local_ip())},
                {"pingSites.0.name", "local"},
                {"pingSites.0.type", "0"}});
        }
        else if (p.type == "acct" && txn == "XBL360Login")
        {
          name_ = Get(p.fields, "gamertag", "Player");
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
      // sees the player at, or - for a player on the server's own machine
      // (loopback) - wherever the viewer found the server.
      static uint32_t PlayerIpFor(const TheaterConn &player, const TheaterConn &viewer)
      {
        uint32_t ip = player.peer_ip();
        return IsLoopback(ip) ? viewer.local_ip() : ip;
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
            std::lock_guard lock(st.mutex);
            st.games.erase(gid_);
          }
          Send("RGAM", 0, {{"TID", tid}});
        }
        else if (t == "GLST")
        {
          bool not_private = Get(f, "FILTER-NOT-PRIVATE") == "1";
          std::vector<Fields> found;
          {
            std::lock_guard lock(st.mutex);
            for (const auto &[gid, g] : st.games)
            {
              uint64_t max = ToU64(Get(g.fields, "MAX-PLAYERS", "2"), 2);
              if (g.host.get() == this || g.host->closed() || uint64_t(g.joined) >= max ||
                  (not_private && Get(g.fields, "B-U-Private") == "1"))
              {
                continue;
              }
              found.push_back(Summary(g, tid));
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
            pid = st.next_pid++;
            st.pending[pid] = self();
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
        {
          std::lock_guard lock(st.mutex);
          auto p = st.pending.find(pid);
          if (p != st.pending.end())
          {
            joiner = p->second.lock();
            st.pending.erase(p);
          }
          auto it = st.games.find(gid_);
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
          BOOL nodelay = TRUE;
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
      BOOL reuse = TRUE;
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
          int from_len = sizeof(from);
          int n = recvfrom(s, reinterpret_cast<char *>(packet), sizeof(packet), 0, reinterpret_cast<sockaddr *>(&from),
                           &from_len);
          if (n < 0)
          {
            int err = WSAGetLastError();
            if (err == WSAECONNRESET || err == WSAEMSGSIZE)
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
  } // namespace

  std::vector<LocalAddress> LocalAddresses()
  {
    std::vector<LocalAddress> out;
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
    return out;
  }

  bool StartServer(bool log_to_console)
  {
    g_log_console = log_to_console;
    WSADATA wsa = {};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    for (const auto &a : LocalAddresses())
    {
      Log("this computer: %s (%s)%s", a.ip.c_str(), a.adapter.c_str(), a.radmin ? "  <- Radmin VPN" : "");
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
      return out->session_id != 0 && out->xnaddr.size() == 36;
    }
    return false;
  }

#ifndef AO2_STANDALONE_SERVER
  void Start()
  {
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
