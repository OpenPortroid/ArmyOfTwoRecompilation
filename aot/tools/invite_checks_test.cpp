// invite_checks_test.cpp - Checks what the presence service believes of a packet (src/invite_checks.h). Needs no game.
#include <algorithm>
#include <cstdio>
#include <string>

#include "../src/invite_checks.h"

using namespace ao2::invite_checks;

namespace
{
  int g_failures = 0;

  void Check(bool ok, const char *what)
  {
    if (!ok)
    {
      std::printf("FAIL %s\n", what);
      ++g_failures;
    }
  }

  uint32_t Ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
  {
    uint8_t bytes[4] = {a, b, c, d};
    uint32_t ip;
    std::memcpy(&ip, bytes, 4);
    return ip;
  }

  // 127.64.0.0/10, as steam_net.cpp.
  bool IsSteam(uint32_t ip)
  {
    uint8_t b[4];
    IpBytes(ip, b);
    return b[0] == 127 && (b[1] & 0xC0) == 64;
  }

  void TestParse()
  {
    uint32_t ip = 0;
    Check(ParseNumericIp("1.2.3.4", &ip) && ip == Ip(1, 2, 3, 4), "plain address");
    Check(ParseNumericIp("1.2.3.4:13505", &ip) && ip == Ip(1, 2, 3, 4), "address with a port");
    Check(ParseNumericIp("255.255.255.255", &ip) && ip == Ip(255, 255, 255, 255), "largest address");
    for (const char *bad : {"", "example.com", "evil.example.com:80", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.1000",
                            "1..2.3", ".1.2.3", "1.2.3.", "1.2.3.4x", "-1.2.3.4", " 1.2.3.4", "0x7f.0.0.1"})
    {
      Check(!ParseNumericIp(bad, &ip), bad);
    }
  }

  void TestServer()
  {
    const uint32_t lan = Ip(192, 168, 1, 5), lan2 = Ip(192, 168, 1, 9), vpn = Ip(26, 10, 20, 30), net = Ip(8, 8, 4, 4);
    const uint32_t here = Ip(127, 0, 0, 1), here2 = Ip(127, 0, 0, 2), steam = Ip(127, 64, 1, 2), steam2 = Ip(127, 65, 3, 4);
    auto choose = [](const char *text, uint32_t from) { return ChooseInviteServer(text, from, IsSteam); };

    Check(choose("192.168.1.9", lan) == lan2, "a LAN player on a third PC's server");
    Check(choose("192.168.1.5", lan) == lan, "the inviter's own server");
    Check(choose("8.8.4.4", vpn) == net, "a public server, from a VPN");
    Check(choose("26.10.20.31", vpn) == Ip(26, 10, 20, 31), "another VPN address");
    Check(choose("127.65.3.4", steam) == steam2, "a Steam player's server");
    Check(choose("127.0.0.2", here) == here2, "two games on this PC");
    Check(choose("192.168.1.9", here) == lan2, "from this PC, on the LAN's server");

    Check(choose("127.0.0.1", lan) == lan, "loopback from another PC is that PC");
    Check(choose("127.0.0.1", net) == net, "loopback from the internet is the sender");
    Check(choose("192.168.1.9", steam) == steam, "over Steam, a LAN address");
    Check(choose("8.8.4.4", steam) == steam, "over Steam, a public address");
    Check(choose("192.168.1.1", net) == net, "from outside, an address on this LAN");
    Check(choose("10.0.0.1", vpn) == vpn, "from a public-looking VPN, a local address");
    Check(choose("169.254.169.254", net) == net, "from outside, a link-local address");
    Check(choose("evil.example.com", lan) == lan, "a host name is not looked up");
    Check(choose("", lan) == lan, "no server");
    Check(choose("0.0.0.0", lan) == lan, "nobody hosts at 0.0.0.0");
    Check(choose("224.0.0.1", lan) == lan, "multicast");
    Check(choose("255.255.255.255", lan) == lan, "broadcast");
  }

  void TestText()
  {
    Check(CleanText("Player", kMaxNameBytes) == "Player", "a plain name");
    Check(CleanText("a\tb\rc\x7f", kMaxNameBytes) == "abc", "control characters");
    Check(CleanText(std::string(1000, 'x'), kMaxNameBytes).size() == kMaxNameBytes, "a long name");
    // Two bytes each: cut at 5 bytes, the third character must go whole.
    Check(CleanText("\xD0\x90\xD0\x91\xD0\x92", 5) == "\xD0\x90\xD0\x91", "not inside a UTF-8 character");
    Check(CleanText("\xD0\x90\xD0\x91", 4) == "\xD0\x90\xD0\x91", "exactly the limit");
    Check(CleanText("\xF0\x9F\x98\x80", 3).empty(), "one character over the limit");
  }

  void TestSettings()
  {
    Check(SettingsDifference("a:1 b:2", "a:1 b:2").empty(), "the same settings");
    Check(SettingsDifference("a:1 b:2", "").empty(), "a build without them");
    Check(SettingsDifference("a:1 b:2", "b:2 a:1").empty(), "any order");
    Check(SettingsDifference("a:1 b:2", "a:1 b:3") == "b 2/3", "one differs");
    Check(SettingsDifference("a:1 b:2", "a:5 b:3") == "a 1/5, b 2/3", "two differ");
    Check(SettingsDifference("a:1 b:2", "a:1") == "b 2/?", "one missing");
    Check(SettingsDifference("ab:1", "a:1 abc:1") == "ab 1/?", "a whole key only");
  }

  void TestRate()
  {
    using Clock = RateLimit::Clock;
    RateLimit rate(2, std::chrono::seconds(10));
    Clock::time_point t{};
    Check(rate.Allow(t), "first");
    Check(rate.Allow(t + std::chrono::seconds(1)), "second");
    Check(!rate.Allow(t + std::chrono::seconds(2)), "third in the window");
    Check(rate.Allow(t + std::chrono::seconds(10)), "the first left the window");
    Check(!rate.Allow(t + std::chrono::seconds(10)), "still two in the window");
  }
} // namespace

int main()
{
  TestParse();
  TestServer();
  TestText();
  TestSettings();
  TestRate();
  if (g_failures)
  {
    std::printf("%d failed\n", g_failures);
    return 1;
  }
  std::printf("PASS\n");
  return 0;
}
