// lobby_server_main.cpp - ao2_lobby_server.exe: the Army of Two co-op lobby
// server on its own (the same server the game runs with ao2_coop_server).
//
// Run it on one PC (usually the host's), then both games connect to it with
// ao2_coop_host = this PC's address (127.0.0.1 on the PC itself, the Radmin VPN
// / LAN address on the other one). Neither game sets ao2_coop_server then.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "ea_server.h"

int main()
{
  std::printf("Army of Two co-op lobby server (EA FESL / Theater / Messenger)\n\n");
  if (!ao2::ea::StartServer(true))
  {
    std::printf("\nCould not open TCP 18480 / 18485 / 13505: is another server (or a game with\n"
                "ao2_coop_server = true) already running on this PC?\n\nPress Enter to exit.");
    std::getchar();
    return 1;
  }
  std::string radmin;
  for (const auto &a : ao2::ea::LocalAddresses())
  {
    if (a.radmin && radmin.empty())
    {
      radmin = a.ip;
    }
  }
  std::printf("\nOn this PC:        ao2_coop_host = \"127.0.0.1\"\n");
  if (!radmin.empty())
  {
    std::printf("On the partner PC: ao2_coop_host = \"%s\"   (Radmin VPN)\n", radmin.c_str());
  }
  else
  {
    std::printf("On the partner PC: ao2_coop_host = this PC's address from the list above\n"
                "                   (no Radmin VPN adapter found - is Radmin VPN running?)\n");
  }
  std::printf("\nLeave this window open while you play. Log: ea_server.log\n\n");
  for (;;)
  {
    std::this_thread::sleep_for(std::chrono::hours(1));
  }
}
