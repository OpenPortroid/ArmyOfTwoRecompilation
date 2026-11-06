// invite_checks.h - what the presence service (coop_invite.cpp) believes of a packet.
//
// Presence packets are plain UDP with no way to prove who sent them: players share no secret, and on a LAN or a public
// VPN network anybody can send one. So nothing in them may do harm by itself: a text field is never looked up or
// trusted for its length, a flood cannot grow the lists without limit, and the address of the server an accepted invite
// connects to must make sense for where the invite came from. The player sees the sender and that server before
// accepting. Free of the game, for tools/invite_checks_test.cpp. IPv4 addresses are in network order.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>

namespace ao2::invite_checks
{
  constexpr size_t kMaxNameBytes = 64;
  constexpr size_t kMaxFieldBytes = 64;
  constexpr size_t kMaxPeers = 256;
  constexpr size_t kMaxReceivedInvites = 8;
  constexpr size_t kMaxInvitesPerIp = 2;
  // New invites the whole service takes in that time; resends of a known one do not count.
  constexpr size_t kInviteBurst = 6;
  constexpr auto kInviteBurstWindow = std::chrono::seconds(10);

  inline void IpBytes(uint32_t ip, uint8_t out[4])
  {
    std::memcpy(out, &ip, 4);
  }

  // "1.2.3.4", or "1.2.3.4:port" (the port is ignored). Never a host name: a packet must not cause a DNS lookup.
  inline bool ParseNumericIp(std::string_view text, uint32_t *ip)
  {
    text = text.substr(0, text.find(':'));
    uint8_t bytes[4] = {};
    size_t at = 0;
    for (int i = 0; i < 4; ++i)
    {
      size_t begin = at;
      int value = 0;
      while (at < text.size() && text[at] >= '0' && text[at] <= '9' && at - begin < 3)
      {
        value = value * 10 + (text[at++] - '0');
      }
      if (at == begin || value > 255)
      {
        return false;
      }
      bytes[i] = uint8_t(value);
      if (i < 3)
      {
        if (at >= text.size() || text[at] != '.')
        {
          return false;
        }
        ++at;
      }
    }
    if (at != text.size())
    {
      return false;
    }
    std::memcpy(ip, bytes, 4);
    return true;
  }

  // Not a unicast address anybody could host on: 0.x.x.x, multicast and above.
  inline bool IsUsableIp(uint32_t ip)
  {
    uint8_t b[4];
    IpBytes(ip, b);
    return b[0] != 0 && b[0] < 224;
  }

  // Only reachable inside a local network.
  inline bool IsPrivateIp(uint32_t ip)
  {
    uint8_t b[4];
    IpBytes(ip, b);
    return b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] < 32) || (b[0] == 192 && b[1] == 168) ||
           (b[0] == 169 && b[1] == 254);
  }

  // This PC. A Steam player's virtual address (127.64.0.0/10) is another PC.
  inline bool IsLoopbackIp(uint32_t ip, bool virtual_ip)
  {
    uint8_t b[4];
    IpBytes(ip, b);
    return b[0] == 127 && !virtual_ip;
  }

  // The EA server to connect to when an invite from `from` names `text`: the sender itself, unless it names a server
  // that makes sense for it (an inviter may be on a third machine's server). is_virtual says whether an address is a
  // Steam player's.
  inline uint32_t ChooseInviteServer(std::string_view text, uint32_t from, bool (*is_virtual)(uint32_t))
  {
    uint32_t server = 0;
    if (!ParseNumericIp(text, &server) || !IsUsableIp(server))
    {
      return from;
    }
    bool from_virtual = is_virtual(from), server_virtual = is_virtual(server);
    bool from_here = IsLoopbackIp(from, from_virtual);
    // Loopback in a packet from another PC means the sender's own PC.
    if (IsLoopbackIp(server, server_virtual) && !from_here)
    {
      return from;
    }
    // Over Steam only Steam addresses can be reached.
    if (from_virtual && !server_virtual)
    {
      return from;
    }
    // From outside this network: its local addresses are not the sender's to name.
    if (!from_here && !IsPrivateIp(from) && IsPrivateIp(server))
    {
      return from;
    }
    return server;
  }

  // A text field as it may be kept and drawn: no control characters, at most max_bytes, not cut inside a UTF-8 character.
  inline std::string CleanText(std::string_view text, size_t max_bytes)
  {
    std::string out;
    out.reserve(std::min(text.size(), max_bytes));
    for (char c : text)
    {
      unsigned char u = static_cast<unsigned char>(c);
      if (u >= 0x20 && u != 0x7F)
      {
        out += c;
      }
    }
    if (out.size() > max_bytes)
    {
      size_t end = max_bytes;
      while (end > 0 && (static_cast<unsigned char>(out[end]) & 0xC0) == 0x80)
      {
        --end;
      }
      out.resize(end);
    }
    return out;
  }

  // At most `limit` events in any `window`.
  class RateLimit
  {
  public:
    using Clock = std::chrono::steady_clock;

    RateLimit(size_t limit, Clock::duration window) : limit_(limit), window_(window) {}

    bool Allow(Clock::time_point now)
    {
      while (!times_.empty() && now - times_.front() >= window_)
      {
        times_.pop_front();
      }
      if (times_.size() >= limit_)
      {
        return false;
      }
      times_.push_back(now);
      return true;
    }

  private:
    size_t limit_;
    Clock::duration window_;
    std::deque<Clock::time_point> times_;
  };
} // namespace ao2::invite_checks
