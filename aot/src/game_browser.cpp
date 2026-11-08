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

namespace
{
  using ao2::text::Tr;
  namespace ui = ao2::ui;
  using ui::FontKind;
  using ao2::steam::Lobby;

  constexpr double kGrace = 0.25;   // what chose the menu's button is no press here
  constexpr double kRefresh = 15.0; // the list is asked for again this often
  constexpr double kJoinWait = 15.0;   // to enter the lobby
  constexpr double kInviteWait = 30.0; // from the join to the host's game
  constexpr int kVisibleRows = 6;

  // XINPUT_GAMEPAD buttons.
  constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

  std::atomic<bool> g_open{false};

  void Log(const std::string &what)
  {
    static FILE *log = ao2::OpenLogFile("lobby_visibility.log");
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
        if (now - joining_since_ > (in_lobby_ ? kInviteWait : kJoinWait))
        {
          Log(in_lobby_ ? "join: no invite in time" : "join: no lobby in time");
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

    const Lobby *Best() const
    {
      for (const Lobby &l : rows_)
      {
        if (Open(l))
        {
          return &l; // the open ones come first, by ping
        }
      }
      return nullptr;
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

    // "$500K", "$1.5M".
    static std::string Money(int value)
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

    // The match's details next to the mouse: the game's choices and the host's rules
    // (steam_net.cpp "rules", coop_invite.cpp UpdateSteamHosting).
    // The selected row's match (a mouse over a row selects it too, so the keyboard and a pad get it as well):
    // beside the list (panel_mn / panel_mx), level with the row (row_y).
    void Details(ImDrawList *dl, const Lobby &l, float a, ImVec2 panel_mn, ImVec2 panel_mx, float row_y)
    {
      const float s = ui::Scale();
      std::map<std::string, int> r;
      for (size_t at = 0; at < l.rules.size();)
      {
        size_t end = l.rules.find(';', at);
        if (end == std::string::npos)
        {
          end = l.rules.size();
        }
        std::string item = l.rules.substr(at, end - at);
        size_t eq = item.find('=');
        if (eq != std::string::npos)
        {
          r[item.substr(0, eq)] = std::atoi(item.c_str() + eq + 1);
        }
        at = end + 1;
      }
      auto get = [&](const char *key, int fallback) {
        auto it = r.find(key);
        return it == r.end() ? fallback : it->second;
      };
      std::vector<std::pair<std::string, std::string>> lines;
      lines.emplace_back(Tr("Map"), ao2::steam::MissionName(l.map, true));
      lines.emplace_back(Tr("Mode"), ao2::steam::ModeName(true, l.game_type));
      lines.emplace_back(Tr("Players"), std::to_string(std::min(l.players, l.max)) + " / " + std::to_string(l.max));
      lines.emplace_back(Tr("Host"), l.in_game           ? Tr("in a match")
                                     : l.menu == "setup" ? Tr("in Game Setup")
                                     : l.menu == "ready" ? Tr("in the ready room")
                                                         : "\xE2\x80\x94");
      if (!r.empty())
      {
        char text[64];
        const int round = get("r", 0);
        if (round > 0)
        {
          std::snprintf(text, sizeof(text), "%d %s", round, Tr("min"));
        }
        lines.emplace_back(Tr("Round length"), round > 0 ? text : Tr("Lobby's"));
        const int cash = get("c", 0);
        lines.emplace_back(Tr("Cash goal"), cash > 0 ? Money(cash) : Tr("Off"));
        const int ff = get("f", 0);
        lines.emplace_back(Tr("Friendly fire"), Tr(ff == 1 ? "Off" : ff == 2 ? "Full" : "Half"));
        const int price = get("p", 0);
        lines.emplace_back(Tr("Respawn price"), Money(price > 0 ? price : 10000));
        const int step = get("s", 0);
        lines.emplace_back(Tr("Respawn price increase"), step > 0 ? "+" + Money(step) : Tr("None"));
        std::string bots = Tr("Off");
        if (get("b", 0))
        {
          static const char *kDifficulty[] = {"Easy", "Normal", "Hard"};
          const int d = get("d", -1);
          bots = Tr(d >= 0 && d < 3 ? kDifficulty[d] : "Custom");
          if (get("u", 0))
          {
            bots += std::string("  \xC2\xB7  ") + Tr("buy");
          }
          if (get("a", 0))
          {
            bots += std::string("  \xC2\xB7  ") + Tr("infinite ammo");
          }
        }
        lines.emplace_back(Tr("Bots"), bots);
      }

      const float pad = 18.0f * s, lh = ui::FontSize(FontKind::kSmall) + 8.0f * s, col_gap = 24.0f * s;
      const char *title = Tr("MATCH DETAILS");
      float label_w = 0.0f, value_w = 0.0f;
      for (const auto &[label, value] : lines)
      {
        label_w = std::max(label_w, ui::TextSize(FontKind::kSmall, label.c_str()).x);
        value_w = std::max(value_w, ui::TextSize(FontKind::kSmall, value.c_str()).x);
      }
      const char *older = r.empty() ? Tr("The host's game is an older version: no match rules listed.") : nullptr;
      float width = std::max(label_w + col_gap + value_w, ui::CapsSize(FontKind::kLabel, title).x);
      if (older)
      {
        width = std::max(width, ui::TextSize(FontKind::kSmall, older).x);
      }
      const ImVec2 size(width + pad * 2.0f, pad * 2.0f + ui::FontSize(FontKind::kLabel) + 12.0f * s +
                                                 lh * float(lines.size()) + (older ? lh + 6.0f * s : 0.0f));
      // Right of the list, else left of it, else over the list's right edge; kept on the screen.
      const ImGuiViewport *vp = ImGui::GetMainViewport();
      const float gap = 16.0f * s;
      ImVec2 mn(panel_mx.x + gap, row_y);
      if (mn.x + size.x > vp->Pos.x + vp->Size.x - 8.0f * s)
      {
        mn.x = panel_mn.x - gap - size.x;
        if (mn.x < vp->Pos.x + 8.0f * s)
        {
          mn.x = panel_mx.x - 24.0f * s - size.x;
        }
      }
      if (mn.y + size.y > vp->Pos.y + vp->Size.y - 8.0f * s)
      {
        mn.y = vp->Pos.y + vp->Size.y - 8.0f * s - size.y;
      }
      mn.x = std::max(mn.x, vp->Pos.x + 8.0f * s);
      mn.y = std::max(mn.y, vp->Pos.y + 8.0f * s);
      const ImVec2 mx(mn.x + size.x, mn.y + size.y);
      dl->AddRectFilled(ImVec2(mn.x + 4.0f * s, mn.y + 6.0f * s), ImVec2(mx.x + 4.0f * s, mx.y + 6.0f * s),
                        ui::Fade(ui::kInk, 0.5f * a));
      ui::Card(dl, mn, mx, ui::Fade(ui::kPanel, 0.98f * a));
      ui::Frame(dl, mn, mx, ui::Fade(ui::kAccent, 0.35f * a), 1.0f * s);
      float y = mn.y + pad - 4.0f * s;
      ui::Caps(dl, FontKind::kLabel, ImVec2(mn.x + pad, y), ui::Fade(ui::kAccentLight, a), title);
      y += ui::FontSize(FontKind::kLabel) + 12.0f * s;
      for (const auto &[label, value] : lines)
      {
        ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + pad, y), ui::Fade(ui::kMuted, a), label.c_str());
        ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + pad + label_w + col_gap, y), ui::Fade(ui::kText, a),
                 value.c_str());
        y += lh;
      }
      if (older)
      {
        ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + pad, y + 6.0f * s), ui::Fade(ui::kMuted, 0.8f * a), older);
      }
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
      int details = -1; // the selected row: its match's details
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
        if (versus_ && live && i == sel_)
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
        Details(dl, rows_[details], a, mn, mx, details_y);
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
