// game_browser.cpp - Search Co-op Game / Quick Match: a list of the public games.
//
// The disc's searches (Public Co-op Game: a mission and a difficulty to filter on, then
// the quick match; versus' Quick Match: a matchmaking session that waits for four
// players) are replaced, while Steam runs, by this window: the public Steam lobbies of
// the mode (co-op or versus, steam_net.cpp: host, mission, difficulty or game type,
// players, ping), the lowest ping first and the open ones on top. A game is joined as
// in the F1 menu's Servers tab: its lobby is entered, the host's game sends the invite
// and this one accepts it by itself (steam::TakeAutoAccept); the window closes once the
// invite is accepted (steam::AutoAccepted; co-op also when its network session starts).
// Search Co-op Game's sequence closes MissionSetupScene under the window, so a co-op list
// closed without a game opens it again (lobby_visibility.cpp BackToMenu); Quick Match's
// leaves the versus menu open (versus is in a network session in all its menus). "Create" hosts a public game instead (Create Co-op Game / Create
// Match with Public picked, lobby_visibility.cpp).
//
// lobby_visibility.cpp holds the menu's event back and opens the window (Open); without
// Steam the event goes through and the disc's search runs.
#include "coop.h"
#include "coop_invite.h"
#include "debug_menu.h"
#include "log_files.h"
#include "steam_net.h"
#include "ui_text.h"
#include "ui_theme.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/logging.h>

namespace ao2::lobby_visibility
{
  void CreatePublic(bool versus); // lobby_visibility.cpp
  void BackToMenu(bool versus);
}

namespace ao2::browser
{
  void MatchDetails(const ao2::steam::Lobby &l, float alpha, ImVec2 list_mn, ImVec2 list_mx, float row_y, float row_h);
  void MatchDetailsHidden();
}

namespace
{
  using ao2::text::Tr;
  namespace ui = ao2::ui;
  using ui::FontKind;
  using ao2::steam::Lobby;

  constexpr double kGrace = 0.25;   // what chose the menu's button is no press here
  constexpr double kRefresh = 15.0; // the list is asked for again this often
  constexpr double kInviteWait = 30.0; // from the join to the host's game
  constexpr int kVisibleRows = 6;

  // XINPUT_GAMEPAD buttons.
  constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

  std::atomic<bool> g_open{false};

  void Log(const std::string &what)
  {
    static FILE *log = ao2::OpenLogFile("game_browser.log"); // not lobby_visibility.log: two "w" opens clobber
    REXLOG_INFO("game_browser: {}", what);
    if (log)
    {
      std::fprintf(log, "browser: %s\n", what.c_str());
      std::fflush(log);
    }
  }

  bool Open(const Lobby &l)
  {
    return !l.joined && !l.in_game && l.players < l.max && !ao2::invite::IsOtherBuild(l.build);
  }

  int Ping(const Lobby &l) { return l.ping < 0 ? 999 : l.ping; }

  class Browser
  {
  public:
    explicit Browser(bool versus) : versus_(versus) {}

    // UI thread, inside an ImGui frame; false closes the window.
    bool Draw()
    {
      const double now = ImGui::GetTime();
      if (opened_ < 0.0)
      {
        opened_ = now;
        accepted_ = ao2::steam::AutoAccepted();
        session_ = ao2::coop::network_session.load(std::memory_order_relaxed);
        refreshed_ = now;
        ao2::steam::RefreshLobbies();
      }
      else if (now - refreshed_ > kRefresh && !joining_)
      {
        Refresh(now);
      }
      Collect();

      // The pad: what was down when the window opened waits to be let go.
      int16_t stick = 0; // the d-pad moves the list
      uint16_t pad = ao2::menu::PadButtons(&stick);
      if (first_read_)
      {
        first_read_ = false;
        ignored_ = pad;
      }
      ignored_ &= pad;
      pad &= uint16_t(~ignored_);
      const uint16_t pressed = pad & uint16_t(~prev_);
      prev_ = pad;

      if (ao2::steam::AutoAccepted() != accepted_ ||
          (!session_ && ao2::coop::network_session.load(std::memory_order_relaxed)))
      {
        Log("the host's invite is accepted: the list closes");
        g_open = false;
        return false;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (pressed & kB))
      {
        Log(joining_ ? "closed while joining" : "closed");
        g_open = false;
        if (!versus_)
        {
          ao2::lobby_visibility::BackToMenu(false);
        }
        return false;
      }
      if (joining_)
      {
        auto it = std::find_if(rows_.begin(), rows_.end(), [&](const Lobby &l) { return l.id == joining_; });
        if (it != rows_.end() && it->joined && !in_lobby_)
        {
          in_lobby_ = true;
          Log("in the lobby of " + it->name + ": waiting for the invite");
        }
        // "joined" is the host's word that this player is in its game (steam_net.cpp): it comes after the invite,
        // so one wait covers the Steam lobby, the invite and the game's own join.
        if (now - joining_since_ > kInviteWait)
        {
          Log(in_lobby_ ? "join: no invite in time" : "join: not in the host's game in time");
          joining_ = 0;
          in_lobby_ = false;
          join_failed_ = true;
        }
      }
      const bool live = now - opened_ >= kGrace && !joining_;
      if (live && Input(pressed, now))
      {
        return false; // Create
      }
      Paint(now, live);
      return true;
    }

  private:
    // This mode's lobbies: the open ones first, each part by ping.
    void Collect()
    {
      std::vector<Lobby> all = ao2::steam::Lobbies();
      uint64_t selected = sel_ < int(rows_.size()) ? rows_[sel_].id : 0;
      rows_.clear();
      for (Lobby &l : all)
      {
        if (l.versus == versus_)
        {
          rows_.push_back(std::move(l));
        }
      }
      std::stable_sort(rows_.begin(), rows_.end(), [](const Lobby &a, const Lobby &b) {
        if (Open(a) != Open(b))
        {
          return Open(a);
        }
        return Ping(a) < Ping(b);
      });
      sel_ = 0;
      for (int i = 0; i < int(rows_.size()); ++i)
      {
        if (rows_[i].id == selected)
        {
          sel_ = i;
        }
      }
      Scroll();
    }

    void Scroll()
    {
      const int n = int(rows_.size());
      sel_ = std::clamp(sel_, 0, std::max(n - 1, 0));
      if (sel_ < top_)
      {
        top_ = sel_;
      }
      if (sel_ >= top_ + kVisibleRows)
      {
        top_ = sel_ - kVisibleRows + 1;
      }
      top_ = std::clamp(top_, 0, std::max(n - kVisibleRows, 0));
    }

    void Refresh(double now)
    {
      refreshed_ = now;
      ao2::steam::RefreshLobbies();
    }

    void Join(const Lobby &l, double now)
    {
      if (!Open(l))
      {
        return;
      }
      Log("joining " + l.name + "'s lobby");
      joining_ = l.id;
      joining_since_ = now;
      join_failed_ = false;
      ao2::steam::JoinLobby(l.id);
    }

    // Quick join: the fullest open game with a good ping (steam::QuickScore).
    const Lobby *Best() const
    {
      const Lobby *best = nullptr;
      for (const Lobby &l : rows_)
      {
        if (Open(l) && (!best || ao2::steam::QuickScore(l) < ao2::steam::QuickScore(*best)))
        {
          best = &l;
        }
      }
      return best;
    }

    void Create()
    {
      Log(versus_ ? "create a public match" : "create a public co-op game");
      g_open = false;
      ao2::lobby_visibility::CreatePublic(versus_);
    }

    // True: the window closes (Create).
    bool Input(uint16_t pressed, double now)
    {
      const int n = int(rows_.size());
      if (n && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_W) || (pressed & kUp)))
      {
        sel_ = (sel_ + n - 1) % n;
        Scroll();
      }
      if (n && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_S) || (pressed & kDown)))
      {
        sel_ = (sel_ + 1) % n;
        Scroll();
      }
      if (n && ImGui::GetIO().MouseWheel != 0.0f)
      {
        top_ = std::clamp(top_ - int(ImGui::GetIO().MouseWheel), 0, std::max(n - kVisibleRows, 0));
      }
      if (n && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                (pressed & kA)))
      {
        Join(rows_[sel_], now);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_R, false) || ImGui::IsKeyPressed(ImGuiKey_F5, false) || (pressed & kX))
      {
        Refresh(now);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_C, false) || (pressed & kY))
      {
        Create();
        return true;
      }
      return false;
    }

    void Paint(double now, bool live)
    {
      const ImGuiViewport *vp = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(vp->Pos);
      ImGui::SetNextWindowSize(vp->Size);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
      ImGui::Begin("##game_browser", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::PopStyleVar(2);

      ImDrawList *dl = ImGui::GetWindowDrawList();
      const float s = ui::Scale();
      const float since = float(now - opened_);
      const float a = ui::Clamp01(since / 0.2f);
      dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), ui::Fade(ui::kInk, 0.62f * a));

      const float pad = 32.0f * s, rh = 56.0f * s, row_gap = 6.0f * s, bh = 38.0f * s;
      const float width = std::min(1100.0f * s, vp->Size.x - 40.0f * s);
      const float list_h = kVisibleRows * (rh + row_gap);
      const float panel_h = pad * 2.0f + ui::FontSize(FontKind::kTitle) + ui::FontSize(FontKind::kSmall) + 28.0f * s +
                            bh + 20.0f * s + ui::FontSize(FontKind::kLabel) + 10.0f * s + list_h +
                            ui::FontSize(FontKind::kSmall) + 30.0f * s;
      const ImVec2 c = vp->GetCenter();
      const ImVec2 mn(c.x - width * 0.5f, c.y - panel_h * 0.5f + (1.0f - ui::EaseOutCubic(since / 0.3f)) * 24.0f * s);
      const ImVec2 mx(mn.x + width, mn.y + panel_h);
      ui::Card(dl, mn, mx, ui::Fade(ui::kPanel, 0.97f * a));
      const float x0 = mn.x + pad, x1 = mx.x - pad;

      float y = mn.y + pad - 6.0f * s;
      ui::Caps(dl, FontKind::kTitle, ImVec2(x0, y), ui::Fade(ui::kAccentLight, a),
               Tr(versus_ ? "FIND A MATCH" : "FIND A CO-OP GAME"));
      y += ui::FontSize(FontKind::kTitle) + 6.0f * s;
      const bool failed = join_failed_;
      const char *status = joining_ ? Tr("Joining... The host's game sends an invite, and your game follows it automatically.")
                           : failed ? Tr("Couldn't join that game. It may have started or filled up; try another one.")
                                    : Tr("Public games over Steam, lowest ping first.");
      ui::Text(dl, FontKind::kSmall, ImVec2(x0, y), ui::Fade(failed ? ui::kRed : ui::kMuted, a), status);
      y += ui::FontSize(FontKind::kSmall) + 22.0f * s;

      // --- Buttons: quick join, refresh, create ---
      const Lobby *best = Best();
      float bx = x0;
      const float qw = 190.0f * s, rw = 150.0f * s;
      if (ui::Button("quick_join", ImVec2(bx, y), ImVec2(qw, bh), Tr("QUICK JOIN"), ui::ButtonStyle::kPrimary,
                     live && best, a))
      {
        Join(*best, now);
      }
      bx += qw + 8.0f * s;
      const bool refreshing = ao2::steam::Refreshing();
      if (ui::Button("refresh", ImVec2(bx, y), ImVec2(rw, bh), refreshing ? Tr("SEARCHING") : Tr("REFRESH"),
                     ui::ButtonStyle::kOutline, live && !refreshing, a))
      {
        Refresh(now);
      }
      const char *create = Tr(versus_ ? "CREATE MATCH" : "CREATE CO-OP GAME");
      const float cw = std::max(220.0f * s, ui::CapsSize(FontKind::kLabel, create).x + 40.0f * s);
      bool created = false;
      if (ui::Button("create", ImVec2(x1 - cw, y), ImVec2(cw, bh), create, ui::ButtonStyle::kOutline, live, a))
      {
        created = true;
      }
      y += bh + 20.0f * s;

      // --- The table ---
      const float w = x1 - x0, action_w = 140.0f * s;
      const float cols[5] = {x0 + 20.0f * s, x0 + w * 0.32f, x0 + w * 0.52f, x0 + w * 0.68f, x0 + w * 0.78f};
      const float col_end[5] = {cols[1], cols[2], cols[3], cols[4], x1 - action_w - 20.0f * s};
      const char *heads[5] = {Tr("HOST"), Tr(versus_ ? "MAP" : "MISSION"), Tr(versus_ ? "MODE" : "DIFFICULTY"),
                              Tr("PLAYERS"), Tr("PING")};
      for (int i = 0; i < 5; ++i)
      {
        ui::Caps(dl, FontKind::kLabel, ImVec2(cols[i], y), ui::Fade(ui::kMuted, 0.8f * a), heads[i]);
      }
      y += ui::FontSize(FontKind::kLabel) + 10.0f * s;

      if (rows_.empty())
      {
        ImVec2 emn(x0, y), emx(x1, y + list_h - row_gap);
        ui::Frame(dl, emn, emx, ui::Fade(ui::kWhite, 0.08f * a), 1.0f * s);
        const char *line1 = refreshing ? Tr("Looking for games...")
                                       : Tr(versus_ ? "No public matches right now." : "No public co-op games right now.");
        const char *line2 = Tr("Create one and it shows up here for everyone.");
        ImVec2 s1 = ui::TextSize(FontKind::kBody, line1), s2 = ui::TextSize(FontKind::kSmall, line2);
        const float cy = (emn.y + emx.y) * 0.5f;
        if (refreshing)
        {
          ui::Spinner(dl, ImVec2((x0 + x1) * 0.5f, cy - s1.y - 30.0f * s), 10.0f * s, 2.5f * s, ui::Fade(ui::kAccent, a));
        }
        ui::Text(dl, FontKind::kBody, ImVec2((x0 + x1 - s1.x) * 0.5f, cy - s1.y - 2.0f * s), ui::Fade(ui::kText, a),
                 line1);
        if (!refreshing)
        {
          ui::Text(dl, FontKind::kSmall, ImVec2((x0 + x1 - s2.x) * 0.5f, cy + 4.0f * s), ui::Fade(ui::kMuted, a), line2);
        }
      }
      const bool mouse_moved = std::abs(ImGui::GetIO().MouseDelta.x) + std::abs(ImGui::GetIO().MouseDelta.y) > 0.0f;
      int details = -1; // the row under the mouse: its match's details
      float details_y = 0.0f;
      for (int i = top_; i < int(rows_.size()) && i < top_ + kVisibleRows; ++i)
      {
        const Lobby &l = rows_[i];
        const float ry = y + float(i - top_) * (rh + row_gap);
        ImVec2 rmn(x0, ry), rmx(x1, ry + rh);
        const bool hovered = live && ImGui::IsMouseHoveringRect(rmn, ImVec2(x1 - action_w - 24.0f * s, rmx.y));
        if (hovered && mouse_moved)
        {
          sel_ = i;
        }
        if (versus_ && hovered)
        {
          details = i;
          details_y = ry;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
          Join(l, now);
        }
        const bool on = i == sel_;
        float &lift = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID(("row" + std::to_string(l.id)).c_str()), 0.0f);
        lift = ui::Approach(lift, on || l.id == joining_ ? 1.0f : 0.0f, 14.0f);
        ui::Card(dl, rmn, rmx, ui::Fade(ui::kWhite, (0.035f + 0.06f * lift) * a));
        ui::Bar(dl, rmn, rmx, ui::Fade(l.own ? ui::kGreen : ui::kAccent, (0.25f + 0.75f * lift) * a));
        if (on && live)
        {
          ui::Frame(dl, rmn, rmx, ui::Fade(ui::kAccent, 0.55f * a), 1.0f * s);
        }
        const bool open = Open(l);
        const ImU32 text = open || l.own || l.joined ? ui::kText : ui::kMuted;
        const float ty = rmn.y + (rh - ui::FontSize(FontKind::kBody)) * 0.5f - 1.0f * s;
        auto cell = [&](int c, ImU32 col, const std::string &value) {
          dl->PushClipRect(ImVec2(cols[c], rmn.y), ImVec2(col_end[c] - 10.0f * s, rmx.y), true);
          ui::Text(dl, FontKind::kBody, ImVec2(cols[c], ty), ui::Fade(col, a), value.c_str());
          dl->PopClipRect();
        };
        // The host, with its notes under it.
        dl->PushClipRect(rmn, ImVec2(col_end[0] - 10.0f * s, rmx.y), true);
        ui::Text(dl, FontKind::kBody, ImVec2(cols[0], rmn.y + 8.0f * s), ui::Fade(text, a), l.name.c_str());
        std::string notes;
        auto note = [&](const char *t) { notes += (notes.empty() ? "" : "  \xC2\xB7  ") + std::string(t); };
        if (l.in_game)
        {
          note(Tr("in game"));
        }
        else if (l.menu == "setup")
        {
          note(Tr("host in Game Setup"));
        }
        else if (l.menu == "ready")
        {
          note(Tr("host in the ready room"));
        }
        if (l.is_private)
        {
          note(Tr("friends only"));
        }
        const bool other = ao2::invite::IsOtherBuild(l.build) && !l.own;
        if (other)
        {
          note(Tr("different version"));
        }
        ui::Text(dl, FontKind::kSmall, ImVec2(cols[0], rmn.y + 32.0f * s), ui::Fade(other ? ui::kRed : ui::kMuted, a),
                 notes.c_str());
        dl->PopClipRect();
        cell(1, text, ao2::steam::MissionName(l.map, l.versus));
        if (versus_)
        {
          cell(2, text, ao2::steam::ModeName(true, l.game_type));
        }
        else
        {
          const char *difficulty = ao2::steam::DifficultyName(l.difficulty);
          cell(2, text, *difficulty ? difficulty : "\xE2\x80\x94");
        }
        cell(3, open ? ui::kText : ui::kMuted, std::to_string(std::min(l.players, l.max)) + " / " + std::to_string(l.max));
        cell(4, l.ping < 0 ? ui::kMuted : l.ping < 80 ? ui::kGreen : l.ping < 150 ? ui::kAccent : ui::kRed,
             l.ping >= 0 ? std::to_string(l.ping) + " ms" : std::string("\xE2\x80\x94"));
        // The action.
        const float abh = 34.0f * s, mid = rmn.y + rh * 0.5f;
        auto right_caps = [&](const char *t, ImU32 col) {
          ImVec2 ts = ui::CapsSize(FontKind::kLabel, t);
          ui::Caps(dl, FontKind::kLabel, ImVec2(x1 - 22.0f * s - ts.x, mid - ts.y * 0.5f), ui::Fade(col, a), t);
        };
        if (l.id == joining_)
        {
          ui::Spinner(dl, ImVec2(x1 - 28.0f * s, mid), 8.0f * s, 2.5f * s, ui::Fade(ui::kAccent, a));
          ImVec2 ts = ui::CapsSize(FontKind::kLabel, Tr("JOINING"));
          ui::Caps(dl, FontKind::kLabel, ImVec2(x1 - 46.0f * s - ts.x, mid - ts.y * 0.5f), ui::Fade(ui::kText, a),
                   Tr("JOINING"));
        }
        else if (l.own)
        {
          right_caps(Tr("YOURS"), ui::kGreen);
        }
        else if (l.joined)
        {
          right_caps(Tr("JOINED"), ui::kGreen);
        }
        else if (ui::Button(("join" + std::to_string(l.id)).c_str(), ImVec2(x1 - 12.0f * s - action_w, mid - abh * 0.5f),
                            ImVec2(action_w, abh), Tr("JOIN"), on ? ui::ButtonStyle::kPrimary : ui::ButtonStyle::kOutline,
                            live && open, a))
        {
          Join(l, now);
        }
      }
      if (int(rows_.size()) > kVisibleRows)
      {
        char more[32];
        std::snprintf(more, sizeof(more), "%d-%d / %d", top_ + 1, std::min(top_ + kVisibleRows, int(rows_.size())),
                      int(rows_.size()));
        ImVec2 ts = ui::TextSize(FontKind::kSmall, more);
        ui::Text(dl, FontKind::kSmall, ImVec2(x1 - ts.x, y + list_h + 2.0f * s), ui::Fade(ui::kMuted, a), more);
      }
      y += list_h + 6.0f * s;

      // --- The keys ---
      float fx = x0;
      const float cap = ui::FontSize(FontKind::kSmall) + 8.0f * s;
      auto hint = [&](std::initializer_list<const char *> keys, const char *what) {
        for (const char *k : keys)
        {
          fx += ui::KeyCap(dl, ImVec2(fx, y), cap, k, a) + 6.0f * s;
        }
        fx += 2.0f * s;
        ui::Text(dl, FontKind::kSmall, ImVec2(fx, y + 4.0f * s), ui::Fade(ui::kMuted, a), what);
        fx += ui::TextSize(FontKind::kSmall, what).x + 24.0f * s;
      };
      hint({"Enter", "A"}, Tr("Join"));
      hint({"R", "X"}, Tr("Refresh"));
      hint({"C", "Y"}, Tr("Create"));
      hint({"Esc", "B"}, Tr("Back"));
      if (details >= 0)
      {
        ao2::browser::MatchDetails(rows_[details], a, mn, mx, details_y, rh);
      }
      else
      {
        ao2::browser::MatchDetailsHidden();
      }
      ImGui::End();
      if (created)
      {
        Create();
        created_ = true;
      }
    }

  public:
    bool created_ = false;

  private:
    bool versus_;
    std::vector<Lobby> rows_;
    int sel_ = 0, top_ = 0;
    double opened_ = -1.0, refreshed_ = 0.0, joining_since_ = 0.0;
    uint64_t joining_ = 0;
    bool in_lobby_ = false; // the joined lobby is entered, the invite is on its way
    uint32_t accepted_ = 0; // steam::AutoAccepted when the list opened
    bool session_ = false;  // in a network session when it opened (versus' menus)
    bool join_failed_ = false;
    bool first_read_ = true;
    uint16_t ignored_ = 0, prev_ = 0;
  };
} // namespace

namespace ao2::browser
{
  // Any thread: opens the list of `versus` or co-op games; false when it is
  // open already.
  bool Open(bool versus)
  {
    if (g_open.exchange(true))
    {
      return false;
    }
    Log(versus ? "Quick Match: the list opens" : "Search Co-op Game: the list opens");
    auto window = std::make_shared<Browser>(versus);
    ao2::menu::OpenWindow(
        "game_browser",
        [window]() {
          if (!window->Draw())
          {
            return false;
          }
          return !window->created_;
        },
        /*own_escape=*/true);
    return true;
  }

  bool IsOpen() { return g_open.load(); }
} // namespace ao2::browser

// ---------------------------------------------------------------------------
// The match details card (Find a Match here, F1 > Online > Servers in debug_menu.cpp)
// ---------------------------------------------------------------------------
namespace
{
  // "$500K", "$1.5M".
  std::string Money(int value)
  {
    char text[32];
    if (value >= 1000000)
    {
      std::snprintf(text, sizeof(text), value % 1000000 ? "$%.1fM" : "$%.0fM", value / 1000000.0);
    }
    else
    {
      std::snprintf(text, sizeof(text), "$%dK", value / 1000);
    }
    return text;
  }

  // The lobby's "rules" (coop_invite.cpp UpdateSteamHosting: "r=..;c=..;...") as key -> number.
  std::map<std::string, int> ParseRules(const std::string &rules)
  {
    std::map<std::string, int> r;
    for (size_t at = 0; at < rules.size();)
    {
      size_t end = rules.find(';', at);
      if (end == std::string::npos)
      {
        end = rules.size();
      }
      std::string item = rules.substr(at, end - at);
      size_t eq = item.find('=');
      if (eq != std::string::npos)
      {
        r[item.substr(0, eq)] = std::atoi(item.c_str() + eq + 1);
      }
      at = end + 1;
    }
    return r;
  }

  // The card's motion: it fades and slides in when it comes and follows the row under the mouse smoothly (UI thread
  // only).
  struct DetailsMotion
  {
    double shown_at = -1.0; // the current lobby's card came
    double drawn_at = -1.0; // the last frame it was drawn (a card not drawn for a while comes in anew)
    float y = 0.0f;         // the card's top, eased toward its target
    bool placed = false;
  };
  DetailsMotion g_details;
} // namespace

namespace ao2::browser
{
  void MatchDetailsHidden() { g_details.drawn_at = -1.0; }

  // The details of `l`'s match beside the list (list_mn / list_mx), pointing at its row (row_y, row_h): what the
  // lobby says of itself (map, mode, the host's menu, players, ping) and the host's rules. Drawn over everything
  // (the foreground list): the F1 menu's list scrolls inside a child window that would clip it.
  void MatchDetails(const ao2::steam::Lobby &l, float alpha, ImVec2 list_mn, ImVec2 list_mx, float row_y, float row_h)
  {
    namespace ui = ao2::ui;
    using ui::FontKind;
    using ao2::text::Tr;
    const double now = ImGui::GetTime();
    DetailsMotion &m = g_details;
    // From one row to the next the card stays and glides over; it comes in anew after a while unseen.
    if (now - m.drawn_at > 0.2)
    {
      m.shown_at = now;
      m.placed = false;
    }
    m.drawn_at = now;
    const float since = float(now - m.shown_at);
    const float in = ui::EaseOutCubic(since / 0.22f);
    const float a = alpha * ui::Clamp01(since / 0.15f);

    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float s = ui::Scale();
    const std::map<std::string, int> r = ParseRules(l.rules);
    auto get = [&](const char *key, int fallback) {
      auto it = r.find(key);
      return it == r.end() ? fallback : it->second;
    };

    // --- What it says ---
    const std::string map = ao2::steam::MissionName(l.map, true);
    const std::string mode = ao2::steam::ModeName(true, l.game_type);
    const char *status = l.in_game           ? Tr("MATCH IN PROGRESS")
                         : l.menu == "setup" ? Tr("IN GAME SETUP")
                         : l.menu == "ready" ? Tr("IN THE READY ROOM")
                                             : Tr("IN THE LOBBY");
    const ImU32 status_col = l.in_game ? ui::kRed : l.menu == "ready" ? ui::kGreen : ui::kAccent;
    struct Tile
    {
      std::string label, value;
      bool off = false; // a muted value ("Off", "None")
    };
    std::vector<Tile> tiles;
    std::string bots;
    std::vector<std::string> bot_tags;
    bool bots_on = false;
    if (!r.empty())
    {
      char text[64];
      const int round = get("r", 0);
      std::snprintf(text, sizeof(text), "%d %s", round, Tr("min"));
      tiles.push_back({Tr("Round length"), round > 0 ? text : Tr("Lobby's")});
      const int cash = get("c", 0);
      tiles.push_back({Tr("Cash goal"), cash > 0 ? Money(cash) : Tr("Off"), cash <= 0});
      const int ff = get("f", 0);
      tiles.push_back({Tr("Friendly fire"), Tr(ff == 1 ? "Off" : ff == 2 ? "Full" : "Half"), ff == 1});
      const int price = get("p", 0);
      tiles.push_back({Tr("Respawn price"), Money(price > 0 ? price : 10000)});
      const int step = get("s", 0);
      tiles.push_back({Tr("Respawn price increase"), step > 0 ? "+" + Money(step) : Tr("None"), step <= 0});
      bots_on = get("b", 0) != 0;
      if (bots_on)
      {
        static const char *kDifficulty[] = {"Easy", "Normal", "Hard"};
        const int d = get("d", -1);
        bots = Tr(d >= 0 && d < 3 ? kDifficulty[d] : "Custom");
        if (get("u", 0))
        {
          bot_tags.push_back(Tr("buy"));
        }
        if (get("a", 0))
        {
          bot_tags.push_back(Tr("infinite ammo"));
        }
      }
      else
      {
        bots = Tr("Off");
      }
    }
    const bool other = ao2::invite::IsOtherBuild(l.build) && !l.own;

    // --- Layout ---
    const float pad = 22.0f * s, gap = 10.0f * s;
    const float width = std::min(400.0f * s, vp->Size.x - 32.0f * s);
    const float inner = width - pad * 2.0f;
    const float tile_w = (inner - gap) * 0.5f;
    const float tile_h = ui::FontSize(FontKind::kLabel) + ui::FontSize(FontKind::kBody) + 26.0f * s;
    const float head_h = ui::FontSize(FontKind::kLabel) + 8.0f * s + ui::FontSize(FontKind::kHeading) + 6.0f * s +
                         ui::FontSize(FontKind::kSmall);
    const float chip_h = 30.0f * s;
    const int tile_rows = int((tiles.size() + 1) / 2);
    const char *older = r.empty() ? Tr("The host's game is an older version: no match rules listed.") : nullptr;
    float height = pad + head_h + 16.0f * s + chip_h + 18.0f * s;
    if (!tiles.empty())
    {
      height += 1.0f * s + 18.0f * s + float(tile_rows) * (tile_h + gap) + tile_h; // divider, tiles, the bots
    }
    if (older)
    {
      height += ui::FontSize(FontKind::kSmall) + 8.0f * s;
    }
    if (other)
    {
      height += ui::FontSize(FontKind::kSmall) + 12.0f * s;
    }
    height += pad;

    // Beside the list (right, else left), else over its right side under or above the row; level with the row.
    const float margin = 18.0f * s, edge = 10.0f * s;
    const float row_mid = row_y + row_h * 0.5f;
    float x = 0.0f, target_y = row_mid - height * 0.35f;
    int side = 0; // +1: right of the list (the arrow on its left), -1: left of it, 0: over it
    if (list_mx.x + margin + width <= vp->Pos.x + vp->Size.x - edge)
    {
      x = list_mx.x + margin;
      side = 1;
    }
    else if (list_mn.x - margin - width >= vp->Pos.x + edge)
    {
      x = list_mn.x - margin - width;
      side = -1;
    }
    else
    {
      x = list_mx.x - width - 24.0f * s;
      const bool below = row_y + row_h + 8.0f * s + height <= vp->Pos.y + vp->Size.y - edge;
      target_y = below ? row_y + row_h + 8.0f * s : row_y - 8.0f * s - height;
    }
    target_y = std::clamp(target_y, vp->Pos.y + edge,
                          std::max(vp->Pos.y + edge, vp->Pos.y + vp->Size.y - edge - height));
    if (!m.placed)
    {
      m.y = target_y;
      m.placed = true;
    }
    m.y = ui::Approach(m.y, target_y, 18.0f);
    if (a <= 0.0f)
    {
      return;
    }
    const float slide = (1.0f - in) * 14.0f * s * float(side);
    const ImVec2 mn(x + slide, m.y), mx(x + slide + width, m.y + height);

    // --- The card ---
    for (int i = 3; i >= 1; --i) // a soft shadow
    {
      const float g = float(i) * 4.0f * s;
      dl->AddRectFilled(ImVec2(mn.x - g * 0.5f, mn.y + g * 0.3f), ImVec2(mx.x + g * 0.5f, mx.y + g),
                        ui::Fade(ui::kInk, 0.16f * a), 12.0f * s);
    }
    ui::Card(dl, mn, mx, ui::Fade(ui::kPanel, 0.985f * a));
    // A glow of the status colour from the top.
    dl->AddRectFilledMultiColor(mn, ImVec2(mx.x, mn.y + 110.0f * s), ui::Fade(status_col, 0.13f * a),
                                ui::Fade(status_col, 0.13f * a), ui::Fade(status_col, 0.0f), ui::Fade(status_col, 0.0f));
    ui::Frame(dl, mn, mx, ui::Fade(status_col, 0.40f * a), 1.0f * s);
    if (side != 0)
    {
      // The arrow to the row.
      const float ay = std::clamp(row_mid, mn.y + 22.0f * s, mx.y - 22.0f * s);
      const float ax = side > 0 ? mn.x : mx.x, d = 9.0f * s * float(side);
      dl->AddTriangleFilled(ImVec2(ax - d, ay), ImVec2(ax + 1.0f * s * float(side), ay - 9.0f * s),
                            ImVec2(ax + 1.0f * s * float(side), ay + 9.0f * s), ui::Fade(ui::kPanel, 0.985f * a));
      dl->AddLine(ImVec2(ax - d, ay), ImVec2(ax, ay - 9.0f * s), ui::Fade(status_col, 0.40f * a), 1.0f * s);
      dl->AddLine(ImVec2(ax - d, ay), ImVec2(ax, ay + 9.0f * s), ui::Fade(status_col, 0.40f * a), 1.0f * s);
    }

    const float x0 = mn.x + pad, x1 = mx.x - pad;
    float y = mn.y + pad - 2.0f * s;
    // The kicker with the ping, the map, the mode and host.
    ui::Caps(dl, FontKind::kLabel, ImVec2(x0, y), ui::Fade(ui::kMuted, 0.9f * a), Tr("MATCH DETAILS"));
    if (l.ping >= 0)
    {
      char ping[24];
      std::snprintf(ping, sizeof(ping), "%d ms", l.ping);
      const ImU32 pc = l.ping < 80 ? ui::kGreen : l.ping < 150 ? ui::kAccent : ui::kRed;
      const ImVec2 ts = ui::TextSize(FontKind::kSmall, ping);
      const float dot = 4.0f * s;
      ui::Text(dl, FontKind::kSmall, ImVec2(x1 - ts.x, y - 1.0f * s), ui::Fade(pc, a), ping);
      dl->AddCircleFilled(ImVec2(x1 - ts.x - dot - 6.0f * s, y + ts.y * 0.5f - 1.0f * s), dot, ui::Fade(pc, a));
    }
    y += ui::FontSize(FontKind::kLabel) + 8.0f * s;
    dl->PushClipRect(ImVec2(x0, y), ImVec2(x1, y + ui::FontSize(FontKind::kHeading) + 4.0f * s), true);
    ui::Text(dl, FontKind::kHeading, ImVec2(x0, y), ui::Fade(ui::kAccentLight, a), map.c_str());
    dl->PopClipRect();
    y += ui::FontSize(FontKind::kHeading) + 6.0f * s;
    const std::string sub = mode + "  \xC2\xB7  " + l.name;
    dl->PushClipRect(ImVec2(x0, y), ImVec2(x1, y + ui::FontSize(FontKind::kSmall) + 4.0f * s), true);
    ui::Text(dl, FontKind::kSmall, ImVec2(x0, y), ui::Fade(ui::kText, 0.85f * a), sub.c_str());
    dl->PopClipRect();
    y += ui::FontSize(FontKind::kSmall) + 16.0f * s;

    // The host's state as a chip, the places as pips.
    {
      const ImVec2 cs = ui::CapsSize(FontKind::kLabel, status);
      const float cw = cs.x + 34.0f * s;
      const ImVec2 cmn(x0, y), cmx(x0 + cw, y + chip_h);
      dl->AddRectFilled(cmn, cmx, ui::Fade(status_col, 0.16f * a), chip_h * 0.5f);
      dl->AddRect(cmn, cmx, ui::Fade(status_col, 0.55f * a), chip_h * 0.5f, 0, 1.0f * s);
      const float pulse = 0.5f + 0.5f * std::sin(float(now) * 4.0f);
      const ImVec2 dc(cmn.x + 13.0f * s, cmn.y + chip_h * 0.5f);
      dl->AddCircleFilled(dc, 3.5f * s, ui::Fade(status_col, a));
      dl->AddCircle(dc, (3.5f + 3.0f * pulse) * s, ui::Fade(status_col, (1.0f - pulse) * 0.8f * a), 16, 1.0f * s);
      ui::Caps(dl, FontKind::kLabel, ImVec2(cmn.x + 24.0f * s, cmn.y + (chip_h - cs.y) * 0.5f),
               ui::Fade(status_col, a), status);

      const int max = std::max(l.max, 1), players = std::clamp(l.players, 0, max);
      char count[16];
      std::snprintf(count, sizeof(count), "%d / %d", players, max);
      const ImVec2 ns = ui::TextSize(FontKind::kBody, count);
      const float pip = 6.0f * s, pip_gap = 6.0f * s;
      float px = x1 - ns.x;
      ui::Text(dl, FontKind::kBody, ImVec2(px, y + (chip_h - ns.y) * 0.5f - 1.0f * s), ui::Fade(ui::kText, a), count);
      px -= 12.0f * s + float(max) * (pip * 2.0f + pip_gap) - pip_gap;
      for (int i = 0; i < max; ++i)
      {
        const ImVec2 c(px + pip + float(i) * (pip * 2.0f + pip_gap), y + chip_h * 0.5f);
        if (i < players)
        {
          dl->AddCircleFilled(c, pip, ui::Fade(ui::kAccent, a));
        }
        else
        {
          dl->AddCircle(c, pip - 0.5f * s, ui::Fade(ui::kMuted, 0.6f * a), 16, 1.5f * s);
        }
      }
    }
    y += chip_h + 18.0f * s;

    // The rules: tiles two by two (an odd last one across), then the bots across.
    if (!tiles.empty())
    {
      dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + 1.0f * s), ui::Fade(ui::kWhite, 0.10f * a));
      y += 1.0f * s + 18.0f * s;
      auto tile = [&](ImVec2 tmn, float tw, const std::string &label, const std::string &value, bool off) {
        const ImVec2 tmx(tmn.x + tw, tmn.y + tile_h);
        dl->AddRectFilled(tmn, tmx, ui::Fade(ui::kWhite, 0.045f * a), 8.0f * s);
        dl->AddRect(tmn, tmx, ui::Fade(ui::kWhite, 0.07f * a), 8.0f * s, 0, 1.0f * s);
        dl->PushClipRect(tmn, ImVec2(tmx.x - 8.0f * s, tmx.y), true);
        ui::Caps(dl, FontKind::kLabel, ImVec2(tmn.x + 12.0f * s, tmn.y + 9.0f * s), ui::Fade(ui::kMuted, 0.85f * a),
                 label.c_str());
        ui::Text(dl, FontKind::kBody, ImVec2(tmn.x + 12.0f * s, tmn.y + 13.0f * s + ui::FontSize(FontKind::kLabel)),
                 ui::Fade(off ? ui::kMuted : ui::kText, a), value.c_str());
        dl->PopClipRect();
      };
      for (size_t i = 0; i < tiles.size(); ++i)
      {
        const float tx = x0 + float(i % 2) * (tile_w + gap);
        const float ty = y + float(i / 2) * (tile_h + gap);
        const bool wide = i + 1 == tiles.size() && i % 2 == 0;
        tile(ImVec2(tx, ty), wide ? inner : tile_w, tiles[i].label, tiles[i].value, tiles[i].off);
      }
      y += float(tile_rows) * (tile_h + gap);
      // The bots: their difficulty, and what they may do as tags.
      const ImVec2 bmn(x0, y), bmx(x1, y + tile_h);
      const ImU32 bc = bots_on ? ui::kAccent : ui::kWhite;
      dl->AddRectFilled(bmn, bmx, ui::Fade(bc, (bots_on ? 0.08f : 0.045f) * a), 8.0f * s);
      dl->AddRect(bmn, bmx, ui::Fade(bc, (bots_on ? 0.30f : 0.07f) * a), 8.0f * s, 0, 1.0f * s);
      ui::Caps(dl, FontKind::kLabel, ImVec2(bmn.x + 12.0f * s, bmn.y + 9.0f * s), ui::Fade(ui::kMuted, 0.85f * a),
               Tr("Bots"));
      ui::Text(dl, FontKind::kBody, ImVec2(bmn.x + 12.0f * s, bmn.y + 13.0f * s + ui::FontSize(FontKind::kLabel)),
               ui::Fade(bots_on ? ui::kText : ui::kMuted, a), bots.c_str());
      float tx = bmx.x - 10.0f * s;
      for (auto it = bot_tags.rbegin(); it != bot_tags.rend(); ++it)
      {
        const ImVec2 ts = ui::TextSize(FontKind::kSmall, it->c_str());
        const float th = ts.y + 6.0f * s, tw = ts.x + 16.0f * s;
        const ImVec2 tmn(tx - tw, bmn.y + (tile_h - th) * 0.5f), tmx(tx, tmn.y + th);
        dl->AddRectFilled(tmn, tmx, ui::Fade(ui::kAccent, 0.16f * a), th * 0.5f);
        ui::Text(dl, FontKind::kSmall, ImVec2(tmn.x + 8.0f * s, tmn.y + 3.0f * s), ui::Fade(ui::kAccentLight, a),
                 it->c_str());
        tx -= tw + 6.0f * s;
      }
      y += tile_h;
    }
    if (older)
    {
      ui::Text(dl, FontKind::kSmall, ImVec2(x0, y + 8.0f * s), ui::Fade(ui::kMuted, 0.85f * a), older);
      y += ui::FontSize(FontKind::kSmall) + 8.0f * s;
    }
    if (other)
    {
      ui::Text(dl, FontKind::kSmall, ImVec2(x0, y + 12.0f * s), ui::Fade(ui::kRed, a),
               Tr("Different version: update the game to join."));
    }
  }
} // namespace ao2::browser
