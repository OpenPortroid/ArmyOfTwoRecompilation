// coop_net_status.cpp - How the online co-op connection is doing, and what
// happens when the partner drops out.
//
// The overlay (ao2_coop_net_hud; drawn in the frame rate overlay's layer by
// debug_menu.cpp) shows, while the lockstep runs:
// - Ping: the link's own round trip (coop_link.cpp Quality messages, the same
//   route as the game's traffic: LAN, VPN or Steam relay).
// - Delay: the input delay in frames the lockstep runs with
//   (coop_input_delay.cpp: latency + margin), and the adaptive margin on top.
// - Waits: lockstep frames that had to wait for the partner's input in the
//   last second, here and on the partner's PC (each wait is a stutter).
// - Warnings: the partner silent for over 1.5 s (its game hangs, or the
//   connection is gone: the game itself gives up only after a while),
//   checksums out of step (frame sync +308 counts the frames; the game
//   leaves at 600), and many waits a second.
// Mode 1 shows the panel only while there is such a warning; 2 always.
//
// Drops. When the network session ends (coop.h network_session) shortly
// after the lockstep ran, the end was a drop if the partner's game had gone
// silent for 3 s without saying bye (coop_link.cpp: a game that leaves on
// purpose says so; one that crashed or lost its connection cannot), or the
// game was out of sync (+308 past 300). Then ao2::invite::OnPartnerLost puts
// up a notice and arranges the rejoin (ao2_coop_rejoin, coop_invite.cpp).
#include "coop_net_status.h"

#include "coop.h"
#include "coop_invite.h"
#include "coop_link.h"
#include "log_files.h"
#include "ui_text.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_INT32(ao2_coop_net_hud, 1, "Online",
                     "Online co-op connection overlay: 0 = off, 1 = only when the connection has trouble (partner "
                     "not responding, out of sync, stutter), 2 = always (ping, input delay, waits)")
    .range(0, 2);

int32_t CoopLeadFrames(int32_t latency_ms);
int32_t CoopInputExtraMs();
int32_t CoopWaitsPerSecond();

namespace
{
  using clock = std::chrono::steady_clock;
  using ao2::text::Tr;

  constexpr int64_t kSilentWarnMs = 1500;
  constexpr int64_t kSilentDropMs = 3000;
  constexpr uint32_t kDesyncWarnFrames = 30;
  constexpr uint32_t kDesyncDropFrames = 300;
  constexpr int32_t kStutterWaits = 10;

  // From the game thread's sync ticks.
  std::atomic<int32_t> g_latency{0};
  std::atomic<uint32_t> g_bad{0};      // +308: frames with the checksums out of step
  std::atomic<uint32_t> g_bad_max{0};  // ... the most this session
  std::atomic<bool> g_started{false};

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("coop_net.log");
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

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }

  // The lockstep runs: the frame sync object's mode, read straight from
  // guest memory - while the game thread waits for the partner no tick
  // comes, but the mode stays.
  bool InLockstep()
  {
    auto *runtime = rex::Runtime::instance();
    const uint8_t *base = runtime ? runtime->virtual_membase() : nullptr;
    return base && ao2::coop::network_session.load(std::memory_order_relaxed) && ao2::coop::OnlineLockstep(base);
  }

  // The watch thread: pumps the link while a session runs (and a little
  // after, for the partner's bye) and sees a session end.
  void Watch()
  {
    bool session = false;
    bool decide = false;
    clock::time_point ended;
    bool lockstep_at_end = false;
    int64_t silent_at_end = -1;
    uint32_t bad_at_end = 0;
    uint32_t peer_at_end = 0;
    clock::time_point lockstep_seen;
    while (true)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      bool now_session = ao2::coop::network_session.load(std::memory_order_relaxed);
      auto now = clock::now();
      if (InLockstep())
      {
        lockstep_seen = now;
      }
      if (now_session && !session)
      {
        ao2::coop::link::ResetQuality();
        g_bad_max.store(0, std::memory_order_relaxed);
        decide = false;
        Logf("network session started");
      }
      else if (!now_session && session)
      {
        // How things stood when it ended.
        lockstep_at_end = lockstep_seen != clock::time_point{} && now - lockstep_seen < std::chrono::seconds(5);
        silent_at_end = ao2::coop::link::PartnerSilentMs();
        bad_at_end = g_bad_max.load(std::memory_order_relaxed);
        peer_at_end = ao2::coop::link::GamePeer();
        ended = now;
        decide = true;
        ao2::coop::link::SendBye();
        Logf("network session over: lockstep %d, partner silent %lld ms, out of sync %u frames at most",
             lockstep_at_end ? 1 : 0, (long long)silent_at_end, bad_at_end);
      }
      session = now_session;
      if (session || (decide && now - ended < std::chrono::seconds(5)))
      {
        ao2::coop::link::Pump();
      }
      // A second for a bye on its way.
      if (decide && now - ended >= std::chrono::seconds(1))
      {
        decide = false;
        bool bye = ao2::coop::link::PartnerSaidBye();
        bool silent = silent_at_end >= kSilentDropMs; // never heard: unknown, no drop
        bool desync = bad_at_end >= kDesyncDropFrames;
        if (lockstep_at_end && (desync || (silent && !bye)))
        {
          Logf("partner lost: %s", desync ? "out of sync" : "no connection");
          ao2::invite::OnPartnerLost(peer_at_end, desync && !silent);
        }
        else
        {
          Logf("no drop (partner said bye: %d)", bye ? 1 : 0);
        }
      }
    }
  }

  struct HudState
  {
    int32_t ping = -1;
    int64_t silent = -1;
    int32_t lead = 0;
    int32_t extra = 0;
    int32_t waits = 0;
    int32_t partner_waits = -1;
    uint32_t bad = 0;
    bool silent_warn = false, desync_warn = false, stutter_warn = false;
  };

  HudState Snapshot()
  {
    HudState h;
    h.ping = ao2::coop::link::PingMs();
    h.silent = ao2::coop::link::PartnerSilentMs();
    int32_t latency = g_latency.load(std::memory_order_relaxed);
    h.lead = CoopLeadFrames(latency);
    h.extra = CoopInputExtraMs();
    h.waits = CoopWaitsPerSecond();
    h.partner_waits = ao2::coop::link::PartnerWaits();
    h.bad = g_bad.load(std::memory_order_relaxed);
    h.silent_warn = h.silent >= kSilentWarnMs;
    h.desync_warn = h.bad >= kDesyncWarnFrames;
    h.stutter_warn = h.waits >= kStutterWaits;
    return h;
  }

  bool HasWarning(const HudState &h)
  {
    return h.silent_warn || h.desync_warn || h.stutter_warn;
  }
} // namespace

namespace ao2::coop::status
{
  void Start()
  {
    if (!g_started.exchange(true))
    {
      std::thread(Watch).detach();
    }
  }

  void OnSyncTick(const uint8_t *base, uint32_t sync)
  {
    if (!sync || !OnlineLockstep(base))
    {
      return;
    }
    g_latency.store(int32_t(LoadU32(base, sync + 260)), std::memory_order_relaxed);
    uint32_t bad = LoadU32(base, sync + 308);
    g_bad.store(bad, std::memory_order_relaxed);
    if (bad > g_bad_max.load(std::memory_order_relaxed))
    {
      g_bad_max.store(bad, std::memory_order_relaxed);
    }
    static uint32_t logged_bad = 0;
    if (bad >= kDesyncWarnFrames && logged_bad < kDesyncWarnFrames)
    {
      Logf("out of sync: checksums differ for %u frames (frame %u)", bad, LoadU32(base, sync + 92));
    }
    logged_bad = bad;
  }

  bool HudWanted()
  {
    int32_t mode = REXCVAR_GET(ao2_coop_net_hud);
    if (mode <= 0 || !InLockstep())
    {
      return false;
    }
    return mode >= 2 || HasWarning(Snapshot());
  }

  void DrawHud()
  {
    if (!HudWanted())
    {
      return;
    }
    HudState h = Snapshot();
    const ImVec4 kOk(0.55f, 0.85f, 0.6f, 1.0f), kWarn(1.0f, 0.82f, 0.35f, 1.0f), kBad(1.0f, 0.42f, 0.38f, 1.0f),
        kMuted(0.75f, 0.78f, 0.82f, 1.0f);
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 8.0f, vp->Pos.y + 8.0f), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##coop_net_hud", nullptr, flags))
    {
      // Ping: green to 80 ms, yellow to 160, red above.
      ImVec4 ping_color = h.ping < 0 ? kMuted : h.ping <= 80 ? kOk : h.ping <= 160 ? kWarn : kBad;
      if (h.ping >= 0)
      {
        ImGui::TextColored(ping_color, Tr("Ping %d ms"), h.ping);
      }
      else
      {
        ImGui::TextColored(kMuted, "%s", Tr("Ping ?"));
      }
      ImGui::SameLine(0.0f, 12.0f);
      if (h.extra > 0)
      {
        ImGui::TextColored(kMuted, Tr("Delay %d fr (+%d ms)"), h.lead, h.extra);
      }
      else
      {
        ImGui::TextColored(kMuted, Tr("Delay %d fr"), h.lead);
      }
      ImVec4 waits_color = h.waits >= kStutterWaits ? kBad : h.waits >= 3 ? kWarn : kMuted;
      if (h.partner_waits >= 0)
      {
        ImGui::TextColored(waits_color, Tr("Waits %d/s, partner %d/s"), h.waits, h.partner_waits);
      }
      else
      {
        ImGui::TextColored(waits_color, Tr("Waits %d/s"), h.waits);
      }
      if (h.silent_warn)
      {
        float t = float(ImGui::GetTime());
        ImVec4 c = kBad;
        c.w = 0.6f + 0.4f * std::sin(t * 6.0f);
        ImGui::TextColored(c, Tr("Partner not responding: %.0f s"), double(h.silent) / 1000.0);
      }
      if (h.desync_warn)
      {
        ImGui::TextColored(kBad, Tr("Out of sync: %u frames"), h.bad);
      }
      else if (h.stutter_warn && !h.silent_warn)
      {
        ImGui::TextColored(kWarn, "%s", Tr("Unstable connection"));
      }
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
  }
} // namespace ao2::coop::status
