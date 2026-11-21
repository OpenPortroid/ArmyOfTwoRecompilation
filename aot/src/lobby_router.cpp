// lobby_router.cpp - Where the game's EA lobby connections go (lobby_router.h).

#include "lobby_router.h"

#include "ea_server.h"
#include "log_files.h"

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// clang-format on

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/cvar.h>

namespace ao2::lobby_visibility
{
  int PrivateOverride();
}

namespace ao2::router
{
  namespace
  {
    constexpr uint16_t kFeslPort = 18480;
    constexpr uint16_t kTheaterPort = 18485;
    constexpr uint16_t kMessengerPort = 13505;

    std::mutex g_log_mutex;

    void Log(const char *fmt, ...)
    {
      std::lock_guard lock(g_log_mutex);
      static FILE *f = ao2::OpenLogFile("lobby_router.log");
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

    std::string IpString(uint32_t ip)
    {
      char buf[INET_ADDRSTRLEN] = {};
      in_addr a = {};
      a.s_addr = ip;
      inet_ntop(AF_INET, &a, buf, sizeof(buf));
      return buf;
    }

    const char *PortName(uint16_t port)
    {
      return port == kFeslPort ? "FESL" : port == kTheaterPort ? "THEATER" : "MESSENGER";
    }

    std::string Describe(const Target &t)
    {
      switch (t.kind)
      {
      case Kind::kOwn:
        return "own server";
      case Kind::kDirect:
        return "server " + IpString(t.ip);
      case Kind::kTunnel:
        return "OpenPortroid server";
      default:
        return "no server";
      }
    }

    void WriteLength(std::string &packet)
    {
      const uint32_t length = uint32_t(packet.size());
      for (int i = 0; i < 4; ++i)
      {
        packet[8 + i] = char(length >> (24 - 8 * i));
      }
    }

    // One login of the game: its FESL connection, and where its Theater and
    // Messenger go (a Direct server's Hello names them; the router is
    // named to the game instead).
    struct Login
    {
      uint32_t id = 0;
      Target target;
      std::atomic<uint32_t> theater_ip{0};
      std::atomic<uint32_t> messenger_ip{0};
      std::atomic<bool> open{true};
    };

    struct VisitState
    {
      uint64_t generation = 0;
      bool on = false;
      uint32_t ip = 0;
      std::string who;
      std::function<bool()> follow;
      bool used = false;           // a login went there
      std::chrono::steady_clock::time_point since;
      std::weak_ptr<Login> login;  // the latest login that did
    };

    struct State
    {
      std::mutex mutex;
      bool started = false;
      uint32_t local = 0; // where the router listens
      uint32_t self = 0;  // where the game's own connections come from (net_bind_ip, else 127.0.0.1)
      Target home;
      VisitState visit;
      std::vector<std::shared_ptr<Login>> logins; // newest last, closed ones pruned
      uint32_t next_login = 1;
      uint64_t next_visit = 1;
    };

    State &state()
    {
      static State s;
      return s;
    }

    // The newest login still open (Theater / Messenger follow it).
    std::shared_ptr<Login> LatestLogin(State &st)
    {
      std::erase_if(st.logins, [](const std::shared_ptr<Login> &l) { return !l->open; });
      return st.logins.empty() ? nullptr : st.logins.back();
    }

    // Where a new login of the game goes.
    std::shared_ptr<Login> StartLogin()
    {
      auto &st = state();
      std::function<bool()> follow;
      std::string who;
      bool ask = false;
      uint64_t generation = 0;
      {
        std::lock_guard lock(st.mutex);
        if (st.visit.on && st.visit.used)
        {
          auto last = st.visit.login.lock();
          ask = !st.visit.follow || !(last && last->open); // the visit's login is over: does this one follow?
        }
        else if (st.visit.on)
        {
          // The game logs in at once on an accepted invite: a first login
          // a minute later is not the invite's.
          ask = std::chrono::steady_clock::now() - st.visit.since > std::chrono::seconds(60);
        }
        generation = st.visit.generation;
        follow = st.visit.follow;
        who = st.visit.who;
      }
      bool keep = true;
      if (ask)
      {
        keep = follow && follow();
      }
      std::lock_guard lock(st.mutex);
      auto login = std::make_shared<Login>();
      login->id = st.next_login++;
      login->target = st.home;
      if (st.visit.on)
      {
        if (keep || st.visit.generation != generation)
        {
          login->target = {Kind::kDirect, st.visit.ip};
          if (ask)
          {
            Log("login %u: still with %s (%s hosts)", login->id, IpString(st.visit.ip).c_str(), st.visit.who.c_str());
          }
          st.visit.used = true;
          st.visit.login = login;
        }
        else
        {
          Log("login %u: the visit to %s is over (%s hosts nothing), home", login->id,
              IpString(st.visit.ip).c_str(), who.c_str());
          st.visit = {};
        }
      }
      LatestLogin(st);
      st.logins.push_back(login);
      Log("login %u -> %s", login->id, Describe(login->target).c_str());
      return login;
    }

    bool SendAll(SOCKET s, const char *data, size_t size)
    {
      size_t sent = 0;
      while (sent < size)
      {
        int n = send(s, data + sent, int(size - sent), 0);
        if (n <= 0)
        {
          return false;
        }
        sent += size_t(n);
      }
      return true;
    }

    void Shut(SOCKET a, SOCKET b)
    {
      shutdown(a, SD_BOTH);
      shutdown(b, SD_BOTH);
    }

    void Pump(SOCKET from, SOCKET to)
    {
      char chunk[16384];
      while (true)
      {
        int n = recv(from, chunk, sizeof(chunk), 0);
        if (n <= 0 || !SendAll(to, chunk, size_t(n)))
        {
          break;
        }
      }
      Shut(from, to);
    }

    uint32_t ReadBe(const unsigned char *p)
    {
      return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    }

    // A FESL packet (type, flags, length with the header, key=value lines,
    // NUL): the Hello's theaterIp / messengerIp taken out (where that login's
    // connections really go) and the router's address put in.
    std::string RewriteHello(const std::string &packet, Login &login, uint32_t local)
    {
      std::string body = packet.substr(12);
      if (packet.compare(0, 4, "fsys") != 0 || body.find("TXN=Hello\n") == std::string::npos)
      {
        return packet;
      }
      const std::string me = IpString(local);
      std::string out_body;
      size_t pos = 0;
      while (pos < body.size() && body[pos] != '\0')
      {
        size_t end = body.find_first_of(std::string("\n\0", 2), pos);
        if (end == std::string::npos)
        {
          end = body.size();
        }
        std::string line = body.substr(pos, end - pos);
        for (auto [key, field] : {std::pair{"theaterIp=", &login.theater_ip},
                                  std::pair{"messengerIp=", &login.messenger_ip}})
        {
          if (line.rfind(key, 0) == 0)
          {
            in_addr a = {};
            if (inet_pton(AF_INET, line.c_str() + std::strlen(key), &a) == 1)
            {
              *field = a.s_addr;
            }
            line = key + me;
          }
        }
        out_body += line;
        out_body += '\n';
        pos = end < body.size() && body[end] == '\n' ? end + 1 : end;
      }
      out_body += '\0';
      std::string out = packet.substr(0, 12) + out_body;
      WriteLength(out);
      Log("login %u: theater %s, messenger %s (named the router)", login.id, IpString(login.theater_ip).c_str(),
          IpString(login.messenger_ip).c_str());
      return out;
    }

    // Server -> game for a Direct FESL: packets until the Hello is through,
    // then bytes as they come.
    void PumpFeslDown(SOCKET server, SOCKET game, Login &login, uint32_t local)
    {
      std::string buffer;
      char chunk[16384];
      bool hello_done = false;
      while (true)
      {
        int n = recv(server, chunk, sizeof(chunk), 0);
        if (n <= 0)
        {
          break;
        }
        if (hello_done)
        {
          if (!SendAll(game, chunk, size_t(n)))
          {
            break;
          }
          continue;
        }
        buffer.append(chunk, size_t(n));
        bool ok = true;
        while (buffer.size() >= 12)
        {
          uint32_t length = ReadBe(reinterpret_cast<const unsigned char *>(buffer.data()) + 8);
          if (length < 12 || length > (1u << 20))
          {
            hello_done = true; // not a packet stream we know: pass it on as it is
            break;
          }
          if (buffer.size() < length)
          {
            break;
          }
          std::string packet = buffer.substr(0, length);
          buffer.erase(0, length);
          bool hello = packet.compare(0, 4, "fsys") == 0 && packet.find("TXN=Hello\n") != std::string::npos;
          std::string out = hello ? RewriteHello(packet, login, local) : packet;
          if (!SendAll(game, out.data(), out.size()))
          {
            ok = false;
            break;
          }
          if (hello)
          {
            hello_done = true;
            break;
          }
        }
        if (!ok)
        {
          break;
        }
        if (hello_done && !buffer.empty())
        {
          if (!SendAll(game, buffer.data(), buffer.size()))
          {
            break;
          }
          buffer.clear();
        }
      }
      Shut(server, game);
    }

    SOCKET ConnectTo(uint32_t ip, uint16_t port)
    {
      SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (s == INVALID_SOCKET)
      {
        return s;
      }
      // From the address the game itself uses (two games on one PC, a VPN).
      in_addr bind_ip = {};
      std::string bind_text = rex::cvar::GetFlagByName("net_bind_ip");
      if (!bind_text.empty() && inet_pton(AF_INET, bind_text.c_str(), &bind_ip) == 1)
      {
        sockaddr_in from = {};
        from.sin_family = AF_INET;
        from.sin_addr = bind_ip;
        if (bind(s, reinterpret_cast<sockaddr *>(&from), sizeof(from)) != 0)
        {
          closesocket(s);
          return INVALID_SOCKET;
        }
      }
      DWORD timeout = 10000;
      setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
      sockaddr_in to = {};
      to.sin_family = AF_INET;
      to.sin_port = htons(port);
      to.sin_addr.s_addr = ip;
      if (connect(s, reinterpret_cast<sockaddr *>(&to), sizeof(to)) != 0)
      {
        closesocket(s);
        return INVALID_SOCKET;
      }
      timeout = 0;
      setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
      int nodelay = 1;
      setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
      return s;
    }

    // Carries `game` to the server at ip:port (a FESL login's Hello rewritten).
    void Carry(SOCKET game, uint32_t ip, uint16_t port, const std::shared_ptr<Login> &fesl)
    {
      SOCKET server = ConnectTo(ip, port);
      if (server == INVALID_SOCKET)
      {
        Log("%s: %s:%u does not answer (error %d)", PortName(port), IpString(ip).c_str(), port, WSAGetLastError());
        closesocket(game);
        return;
      }
      uint32_t local = state().local;
      std::thread up([game, server, port]() {
        if (port == kTheaterPort)
        {
          ForwardTheater(game, server);
        }
        else
        {
          Pump(game, server);
        }
      });
      if (fesl)
      {
        PumpFeslDown(server, game, *fesl, local);
      }
      else
      {
        Pump(server, game);
      }
      up.join();
      closesocket(server);
      closesocket(game);
    }

    void Route(SOCKET c, uint16_t port)
    {
      auto &st = state();
      sockaddr_in peer = {};
      int len = sizeof(peer);
      getpeername(c, reinterpret_cast<sockaddr *>(&peer), &len);
      uint32_t self = 0;
      {
        std::lock_guard lock(st.mutex);
        self = st.self;
      }
      if (peer.sin_addr.s_addr != self)
      {
        // Another game on this PC that comes to this one's server.
        if (ao2::ea::OwnServerRunning())
        {
          ao2::ea::ServeLocal(port, c);
        }
        else
        {
          Log("%s from %s: this game runs no server", PortName(port), IpString(peer.sin_addr.s_addr).c_str());
          closesocket(c);
        }
        return;
      }

      std::shared_ptr<Login> login;
      Target target;
      uint32_t ip = 0;
      if (port == kFeslPort)
      {
        login = StartLogin();
        target = login->target;
        ip = target.ip;
      }
      else
      {
        std::lock_guard lock(st.mutex);
        login = LatestLogin(st);
        target = login ? login->target
                       : (st.visit.on ? Target{Kind::kDirect, st.visit.ip} : st.home);
        ip = target.ip;
        if (login && target.kind == Kind::kDirect)
        {
          uint32_t named = port == kTheaterPort ? login->theater_ip : login->messenger_ip;
          ip = named ? named : target.ip;
        }
        Log("%s -> %s%s", PortName(port), target.kind == Kind::kDirect ? ("server " + IpString(ip)).c_str()
                                                                        : Describe(target).c_str(),
            login ? (" (login " + std::to_string(login->id) + ")").c_str() : " (no login open)");
      }

      switch (target.kind)
      {
      case Kind::kOwn:
        ao2::ea::ServeLocal(port, c, true);
        break;
      case Kind::kTunnel:
        ao2::ea::ForwardTunnel(port, c);
        break;
      case Kind::kDirect:
        Carry(c, ip, port, port == kFeslPort ? login : nullptr);
        break;
      default:
        Log("%s: no lobby server to go to", PortName(port));
        closesocket(c);
        break;
      }
      if (port == kFeslPort && login)
      {
        login->open = false;
        Log("login %u closed", login->id);
      }
    }

    bool Listen(uint32_t local, uint16_t port)
    {
      SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
      addr.sin_addr.s_addr = local;
      if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
          listen(s, 16) != 0)
      {
        Log("ERROR: cannot listen on %s:%u (error %d)", IpString(local).c_str(), port, WSAGetLastError());
        if (s != INVALID_SOCKET)
        {
          closesocket(s);
        }
        return false;
      }
      std::thread([s, port]() {
        while (true)
        {
          SOCKET c = accept(s, nullptr, nullptr);
          if (c == INVALID_SOCKET)
          {
            Log("accept on %u failed (error %d)", port, WSAGetLastError());
            break;
          }
          int nodelay = 1;
          setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
          std::thread([c, port]() { Route(c, port); }).detach();
        }
      }).detach();
      return true;
    }
  } // namespace

  void ForwardTheater(uintptr_t game_socket, uintptr_t server_socket)
  {
    SOCKET game = SOCKET(game_socket), server = SOCKET(server_socket);
    std::string buffer;
    char chunk[16384];
    while (true)
    {
      int n = recv(game, chunk, sizeof(chunk), 0);
      if (n <= 0)
      {
        break;
      }
      buffer.append(chunk, size_t(n));
      size_t offset = 0;
      while (buffer.size() - offset >= 12)
      {
        uint32_t length = ReadBe(reinterpret_cast<const unsigned char *>(buffer.data() + offset + 8));
        if (length < 12 || length > (1u << 20))
        {
          Shut(game, server);
          return;
        }
        if (buffer.size() - offset < length)
        {
          break;
        }
        std::string packet = buffer.substr(offset, length);
        offset += length;
        int visibility = ao2::lobby_visibility::PrivateOverride();
        if (visibility >= 0 && (packet.compare(0, 4, "CGAM") == 0 || packet.compare(0, 4, "UGAM") == 0))
        {
          constexpr std::string_view key = "B-U-Private=";
          const char flag = visibility == 0 ? '0' : '1';
          size_t field = packet.find(key, 12);
          while (field != std::string::npos && field != 12 && packet[field - 1] != '\n')
          {
            field = packet.find(key, field + key.size());
          }
          if (field != std::string::npos)
          {
            size_t value = field + key.size();
            size_t end = packet.find_first_of("\n\0", value, 2);
            packet.replace(value, end - value, 1, flag);
          }
          else if (packet.compare(0, 4, "CGAM") == 0)
          {
            if (packet.back() == '\0')
            {
              packet.pop_back();
            }
            if (packet.back() != '\n')
            {
              packet += '\n';
            }
            packet.append(key);
            packet += flag;
            packet += "\n";
            packet += '\0';
          }
          WriteLength(packet);
        }
        if (!SendAll(server, packet.data(), packet.size()))
        {
          Shut(game, server);
          return;
        }
      }
      buffer.erase(0, offset);
    }
    Shut(game, server);
  }

  bool Start(uint32_t local, Target home)
  {
    auto &st = state();
    {
      std::lock_guard lock(st.mutex);
      if (st.started)
      {
        return true;
      }
      st.local = local;
      st.home = home;
      in_addr bind_ip = {};
      std::string bind_text = rex::cvar::GetFlagByName("net_bind_ip");
      st.self = !bind_text.empty() && inet_pton(AF_INET, bind_text.c_str(), &bind_ip) == 1 ? bind_ip.s_addr
                                                                                           : htonl(INADDR_LOOPBACK);
    }
    bool ok = Listen(local, kFeslPort);
    ok = Listen(local, kTheaterPort) && ok;
    ok = Listen(local, kMessengerPort) && ok;
    {
      std::lock_guard lock(st.mutex);
      st.started = ok;
    }
    rex::cvar::SetFlagByName("net_title_server_ip", IpString(local));
    Log("lobby router on %s (net_title_server_ip), home: %s%s", IpString(local).c_str(), Describe(home).c_str(),
        ok ? "" : " - NOT ALL PORTS: another game on this PC has this address?");
    return ok;
  }

  bool Started()
  {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    return st.started;
  }

  uint32_t LocalAddress()
  {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    return st.local;
  }

  void Visit(uint32_t ip, const std::string &who, std::function<bool()> follow)
  {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (Target{Kind::kDirect, ip} == st.home || ip == st.local || !ip)
    {
      if (st.visit.on)
      {
        Log("invite from %s: its server %s is home", who.c_str(), IpString(ip).c_str());
      }
      st.visit = {};
      return;
    }
    Log("invite from %s accepted: the next login goes to %s", who.c_str(), IpString(ip).c_str());
    st.visit = {};
    st.visit.on = true;
    st.visit.generation = st.next_visit++;
    st.visit.ip = ip;
    st.visit.who = who;
    st.visit.follow = std::move(follow);
    st.visit.since = std::chrono::steady_clock::now();
  }

  void Home(const char *why)
  {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (st.visit.on)
    {
      Log("visit to %s ends (%s): logins go home", IpString(st.visit.ip).c_str(), why);
    }
    st.visit = {};
  }

  uint32_t CurrentServer()
  {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (st.visit.on)
    {
      return st.visit.ip;
    }
    switch (st.home.kind)
    {
    case Kind::kDirect:
      return st.home.ip;
    case Kind::kOwn:
    case Kind::kTunnel:
      return st.local;
    default:
      return 0;
    }
  }

  uint32_t VisitIp()
  {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    return st.visit.on ? st.visit.ip : 0;
  }
} // namespace ao2::router
