// lobby_visibility.cpp - Create Co-op Game / Create Match: private or public.
//
// Campaign Mode's "Create Co-op Game" (the disc's Private Co-op Game, game_language.cpp
// renames it) and Versus' "Create Match" (the disc's Custom Match) run, in their Kismet
// sequence, the console command "causeevent MissionSetup_HostPrivate" /
// "causeevent Multiplayer_CustomMatch", which comes to the script function
// PlayerController.CauseEvent of every PlayerController (caught at
// UObject::ProcessInternal, script_trace.cpp; split_join.cpp finds the function).
//
// The button's sequence closes its scene (MissionSetupScene, the versus menu) right after
// the command; the event opens the next one. A window closed without a pick therefore
// opens the menu again with the event the main menu uses for it (BackToMenu:
// MainMenu_OperationsSelected / MainMenu_MultiplayerSelected).
//
// The event is held back and a window asks who finds the game: Private (by invitation
// only, as the console's private game) or Public (in the server list, F1 > Online >
// Servers: a Steam lobby anyone may enter and is then invited). The pick runs the
// command again, which goes through; Esc or B closes the window and the menu stays.
//
// The game itself always hosts the console's way (a private co-op game, a versus
// party): only who sees it changes. The pick stands in for the hosted game's
// B-U-Private where the port shows the game to others (coop_invite.cpp MyHostedGame:
// the Steam lobby's type and its "private" key, the invites, the players window), until
// another button hosts or searches: Search Co-op Game (the disc's Public Co-op Game,
// MissionSetup_HostPublic) and Quick Match (Multiplayer_QuickMatch) go back to the
// game's own flag.
//
// Those two are held back as well while Steam runs: they open the list of public games
// (game_browser.cpp) instead of the disc's search; its Create runs Create Co-op Game /
// Create Match with Public picked (CreatePublic). Without Steam they go through.
#include "console_cmd.h"
#include "debug_menu.h"
#include "log_files.h"
#include "steam_net.h"
#include "ui_text.h"
#include "ui_theme.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ppc.h>

namespace ao2::split_join
{
  std::string CauseEventName(const uint8_t *base, uint32_t frame); // split_join.cpp
}
namespace ao2::browser
{
  bool Open(bool versus); // game_browser.cpp
  bool IsOpen();
}
namespace ao2::lobby_visibility
{
  void BackToMenu(bool versus);
}

namespace
{
  using ao2::text::Tr;
  namespace ui = ao2::ui;
  using ui::FontKind;

  constexpr char kCoopEvent[] = "MissionSetup_HostPrivate";
  constexpr char kVersusEvent[] = "Multiplayer_CustomMatch";
  constexpr double kGrace = 0.25; // what chose the menu's button is no pick here
  constexpr double kDoneShow = 0.22;

  std::atomic<int> g_override{-1}; // -1: the game's own flag, 0 public, 1 private
  std::atomic<bool> g_window_open{false};
  std::atomic<bool> g_resume{false};
  std::atomic<bool> g_last_private{true}; // the window starts on the last pick
  std::chrono::steady_clock::time_point g_pass_until; // game thread: the event goes through until then

  void Log(const std::string &what)
  {
    static FILE *log = ao2::OpenLogFile("lobby_visibility.log");
    REXLOG_INFO("lobby_visibility: {}", what);
    if (log)
    {
      std::fprintf(log, "%s\n", what.c_str());
      std::fflush(log);
    }
  }

  class VisibilityWindow
  {
  public:
    VisibilityWindow(std::string event, bool versus) : event_(std::move(event)), versus_(versus)
    {
      choice_private_ = g_last_private.load();
    }

    // UI thread, inside an ImGui frame; false closes the window.
    bool Draw()
    {
      const double now = ImGui::GetTime();
      int16_t stick = 0;
      uint16_t pad = ao2::menu::PadButtons(&stick);
      constexpr int kLean = 16000;
      if (stick < -kLean)
      {
        pad |= 0x0004;
      }
      else if (stick > kLean)
      {
        pad |= 0x0008;
      }
      if (opened_ < 0.0)
      {
        opened_ = now;
        ignored_ = pad; // the A that chose the button may still be down
      }
      ignored_ &= pad;
      pad &= uint16_t(~ignored_);
      const uint16_t pressed = pad & uint16_t(~prev_);
      prev_ = pad;

      if (done_ < 0.0)
      {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (pressed & 0x2000))
        {
          Log(event_ + ": cancelled");
          g_window_open = false;
          ao2::lobby_visibility::BackToMenu(versus_);
          return false;
        }
        if (now - opened_ >= kGrace)
        {
          Input(pressed, now);
        }
      }
      else if (now - done_ >= kDoneShow)
      {
        Finish();
        return false;
      }
      Paint(now);
      return true;
    }

  private:
    void Input(uint16_t pressed, double now)
    {
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) || ImGui::IsKeyPressed(ImGuiKey_A, false) ||
          (pressed & 0x0004))
      {
        choice_private_ = true;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) || ImGui::IsKeyPressed(ImGuiKey_D, false) ||
          (pressed & 0x0008))
      {
        choice_private_ = false;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Tab, false))
      {
        choice_private_ = !choice_private_;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
          ImGui::IsKeyPressed(ImGuiKey_Space, false) || (pressed & 0x1000))
      {
        done_ = now;
      }
      for (int i = 0; i < 2; ++i)
      {
        if (hovered_[i] && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
          choice_private_ = i == 0;
          done_ = now;
        }
      }
    }

    void Finish()
    {
      g_last_private = choice_private_;
      g_override = choice_private_ ? 1 : 0;
      g_resume = true;
      g_window_open = false;
      Log(event_ + ": " + (choice_private_ ? "private" : "public"));
      ao2::console::Queue("exec causeevent " + event_);
    }

    void Paint(double now)
    {
      const ImGuiViewport *vp = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(vp->Pos);
      ImGui::SetNextWindowSize(vp->Size);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
      ImGui::Begin("##lobby_visibility", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::PopStyleVar(2);

      ImDrawList *dl = ImGui::GetWindowDrawList();
      const float s = ui::Scale();
      const float since = float(now - opened_);
      const float closing = done_ < 0.0 ? 0.0f : ui::Clamp01(float(now - done_) / float(kDoneShow));
      const float a = ui::Clamp01(since / 0.2f) * (1.0f - closing);
      const float pulse = 0.5f + 0.5f * std::sin(float(now) * 4.0f);
      dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), ui::Fade(ui::kInk, 0.62f * a));

      const float gap = 16.0f * s, pad = 32.0f * s, card_h = 170.0f * s;
      const float width = std::min(800.0f * s, vp->Size.x - 40.0f * s);
      const float panel_h = 320.0f * s;
      const ImVec2 c = vp->GetCenter();
      const ImVec2 mn(c.x - width * 0.5f, c.y - panel_h * 0.5f + (1.0f - ui::EaseOutCubic(since / 0.3f)) * 24.0f * s);
      const ImVec2 mx(mn.x + width, mn.y + panel_h);
      ui::Card(dl, mn, mx, ui::Fade(ui::kPanel, 0.97f * a));

      float y = mn.y + pad - 6.0f * s;
      ui::Caps(dl, FontKind::kTitle, ImVec2(mn.x + pad, y), ui::Fade(ui::kAccentLight, a),
               Tr(versus_ ? "CREATE MATCH" : "CREATE CO-OP GAME"));
      y += ui::FontSize(FontKind::kTitle) + 6.0f * s;
      ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + pad, y), ui::Fade(ui::kMuted, a), Tr("Who can find your game?"));
      y += ui::FontSize(FontKind::kSmall) + 18.0f * s;

      const float card_w = (width - pad * 2.0f - gap) * 0.5f;
      const bool live = done_ < 0.0 && now - opened_ >= kGrace;
      for (int i = 0; i < 2; ++i)
      {
        const bool is_private = i == 0;
        const ImVec2 cmn(mn.x + pad + float(i) * (card_w + gap), y);
        const ImVec2 cmx(cmn.x + card_w, cmn.y + card_h);
        hovered_[i] = live && ImGui::IsMouseHoveringRect(cmn, cmx);
        if (hovered_[i] && (std::abs(ImGui::GetIO().MouseDelta.x) + std::abs(ImGui::GetIO().MouseDelta.y)) > 0.0f)
        {
          choice_private_ = is_private;
        }
        const bool on = choice_private_ == is_private;
        float &lift = lift_[i];
        lift = ui::Approach(lift, on ? 1.0f : 0.0f, 14.0f);
        ui::Card(dl, cmn, cmx, ui::Fade(ui::kWhite, (0.03f + 0.06f * lift) * a));
        if (on)
        {
          ui::Frame(dl, cmn, cmx, ui::Fade(ui::kAccent, (done_ >= 0.0 ? 1.0f : 0.55f + 0.45f * pulse) * a), 1.5f * s);
        }
        ui::Bar(dl, cmn, cmx, ui::Fade(is_private ? ui::kGreen : ui::kAccent, (0.3f + 0.7f * lift) * a));
        const float tx = cmn.x + 22.0f * s;
        ui::Caps(dl, FontKind::kHeading, ImVec2(tx, cmn.y + 20.0f * s),
                 ui::Fade(on ? ui::kAccentLight : ui::kText, a), Tr(is_private ? "PRIVATE" : "PUBLIC"));
        float ly = cmn.y + 76.0f * s;
        const ImU32 body = ui::Fade(on ? ui::kText : ui::kMuted, a);
        ui::Text(dl, FontKind::kBody, ImVec2(tx, ly), body, Tr(is_private ? "By invitation only." : "In the server list:"));
        ly += ui::FontSize(FontKind::kBody) + 6.0f * s;
        ui::Text(dl, FontKind::kBody, ImVec2(tx, ly), body, Tr(is_private ? "Not in the server list." : "anyone can join."));
      }
      y += card_h + 18.0f * s;

      float fx = mn.x + pad;
      const float cap = ui::FontSize(FontKind::kSmall) + 8.0f * s;
      fx += ui::KeyCap(dl, ImVec2(fx, y), cap, "Enter", a) + 6.0f * s;
      fx += ui::KeyCap(dl, ImVec2(fx, y), cap, "A", a) + 8.0f * s;
      ui::Text(dl, FontKind::kSmall, ImVec2(fx, y + 4.0f * s), ui::Fade(ui::kMuted, a), Tr("Select"));
      fx += ui::TextSize(FontKind::kSmall, Tr("Select")).x + 24.0f * s;
      fx += ui::KeyCap(dl, ImVec2(fx, y), cap, "Esc", a) + 6.0f * s;
      fx += ui::KeyCap(dl, ImVec2(fx, y), cap, "B", a) + 8.0f * s;
      ui::Text(dl, FontKind::kSmall, ImVec2(fx, y + 4.0f * s), ui::Fade(ui::kMuted, a), Tr("Back"));
      ImGui::End();
    }

    std::string event_;
    bool versus_;
    bool choice_private_ = true;
    bool hovered_[2] = {};
    float lift_[2] = {};
    double opened_ = -1.0, done_ = -1.0;
    uint16_t ignored_ = 0, prev_ = 0;
  };
} // namespace

namespace ao2::lobby_visibility
{
  int PrivateOverride() { return g_override.load(std::memory_order_relaxed); }

  // Any thread: the menu the held button's sequence closed, open again.
  void BackToMenu(bool versus)
  {
    const char *event = versus ? "MainMenu_MultiplayerSelected" : "MainMenu_OperationsSelected";
    Log(std::string("back to the menu: ") + event);
    ao2::console::Queue(std::string("exec causeevent ") + event);
  }

  // Any thread: the list's Create - the create button's event again, public.
  void CreatePublic(bool versus)
  {
    const std::string event = versus ? kVersusEvent : kCoopEvent;
    g_override = 0;
    g_resume = true;
    Log(event + ": public (from the list)");
    ao2::console::Queue("exec causeevent " + event);
  }

  // Game thread: PlayerController.CauseEvent (r4 = the FFrame). True: held back.
  bool OnCauseEvent(PPCContext &ctx, uint8_t *base)
  {
    const std::string event = ao2::split_join::CauseEventName(base, ctx.r4.u32);
    if (event == "MissionSetup_HostPublic" || event == "Multiplayer_QuickMatch")
    {
      if (g_override.exchange(-1) != -1)
      {
        Log(event + ": the game's own visibility again");
      }
      // The list instead of the disc's search (once per PlayerController: held
      // back while it is open).
      if (ao2::steam::Running())
      {
        ao2::browser::Open(event == "Multiplayer_QuickMatch");
        return true;
      }
      return false;
    }
    if (event != kCoopEvent && event != kVersusEvent)
    {
      return false;
    }
    // The event comes once per PlayerController: the window's own command goes
    // through, like the others right after it.
    auto now = std::chrono::steady_clock::now();
    if (now < g_pass_until)
    {
      return false;
    }
    if (g_resume.exchange(false))
    {
      g_pass_until = now + std::chrono::milliseconds(500);
      return false;
    }
    if (!g_window_open.exchange(true))
    {
      Log(event + ": the window opens");
      auto window = std::make_shared<VisibilityWindow>(event, event == kVersusEvent);
      ao2::menu::OpenWindow("lobby_visibility", [window]() { return window->Draw(); }, /*own_escape=*/true);
    }
    return true;
  }
} // namespace ao2::lobby_visibility
