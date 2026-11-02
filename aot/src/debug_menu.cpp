// debug_menu.cpp - The F1 debug menu: graphics, achievements, invites,
// cheats, checkpoints, versus.
//
// A Dear ImGui dialog. With the Xenos presenter it joins the SDK's own ImGui
// drawer (the F3/F4 overlays); with the native renderer, which has no
// presenter, it gets an ImGuiDrawer of its own whose frames the renderer
// draws over the picture (gpu/native/overlay.h): each present asks for the
// next UI frame, built on the UI thread like the SDK's overlays.
//
// While the menu is open the game gets no input: the input system's active
// callback is false (pads and keyboard are neutral, and the keyboard/mouse
// driver releases the mouse, so the cursor works in the menu), and a listener
// under ImGui's keeps key presses and mouse movement from the mouse aiming and
// cutscene skip listeners. Key releases still pass, so no key stays held.
//
// A full screen overlay in the launcher's look (ui_theme.h) with six tabs:
// - Graphics: the Options screen's Graphics page settings (graphics_menu.h),
//   applied at once and saved as the page saves them (the render resolution
//   too: the page waits for its Accept), and the field of view and mouse
//   sensitivity (saved too); a column beside them tells what
//   the hovered row does, with the frame rate (perf_overlay.h).
// - The tabs carry badges: achievements unlocked, an invite waiting, the
//   cheats on.
// - Achievements: every achievement with its unlock (achievements.h).
// - Invites: the players window's content (coop_invite.h); the menu opens on
//   it while an invite waits for an answer.
// - Cheats: god mode, infinite ammo, Unlock All, the bonus weapons (cheats.cpp).
// - Checkpoints: the files in AO2Game/Checkpoints and in the extracted DLC
//   packages (Kiev 09_*, Miami alternate ending 10_*; the name of each,
//   then its level), loaded with the console's "checkpoint <name> <difficulty>"
//   (console_cmd.cpp). The loaded checkpoint becomes the profile's current
//   one when the game saves it.
// - Versus: the match's rules (versus_rules.cpp: round length, cash goal,
//   friendly fire, respawn price and how much dearer each respawn gets) and
//   the bots of its empty places (versus_bots.cpp): on or off, buying,
//   infinite ammo, a difficulty (presets of the aim error, reaction, turn
//   speed and range) and those values one by one; saved to
//   settings/online.toml as they change. In a match every machine sends its
//   settings and the match plays by the host's (coop_cmd.cpp); the tab says
//   whose they are.
// - F2 (ao2_fps_overlay_key): the frame rate overlay's mode (perf_overlay.h).
//
// Online co-op is a lockstep simulation (coop.h): there both go through the
// lockstep input to both machines (coop_cmd.cpp) - god mode for this
// player, the checkpoint for both; only while a level runs.
//
// The frame rate overlay (native renderer only) is a dialog of a second
// ImGuiDrawer drawing into the overlay's lower layer. That drawer is no input
// listener (it would capture the mouse on clicks): the overlay takes no input.
#include "debug_menu.h"

#include "achievements.h"
#include "console_cmd.h"
#include "coop.h"
#include "coop_cmd.h"
#include "coop_invite.h"
#include "dlc_folder.h"
#include "gpu/native/overlay.h"
#include "graphics_menu.h"
#include "perf_overlay.h"
#include "steam_net.h"
#include "ui_text.h"
#include "ui_theme.h"
#include "versus_bots.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DECLARE(bool, ao2_god_mode);
REXCVAR_DECLARE(bool, ao2_infinite_ammo);
REXCVAR_DECLARE(bool, ao2_unlock_all);
REXCVAR_DECLARE(bool, ao2_unlock_bonus_weapons);
REXCVAR_DECLARE(int32_t, ao2_fps_target);
REXCVAR_DECLARE(double, ao2_fov);
REXCVAR_DECLARE(double, ao2_fov_aim);
REXCVAR_DECLARE(double, ao2_mouse_sensitivity);
REXCVAR_DECLARE(double, ao2_mouse_zoom_sensitivity);
REXCVAR_DECLARE(int32_t, ao2_start_difficulty);
REXCVAR_DECLARE(std::string, ao2_invite_accept_key);
REXCVAR_DECLARE(bool, ao2_versus_bots);
REXCVAR_DECLARE(bool, ao2_versus_bots_buy);
REXCVAR_DECLARE(bool, ao2_versus_bots_infinite_ammo);
REXCVAR_DECLARE(double, ao2_versus_bots_miss);
REXCVAR_DECLARE(int32_t, ao2_versus_bots_reaction);
REXCVAR_DECLARE(double, ao2_versus_bots_turn);
REXCVAR_DECLARE(double, ao2_versus_bots_range);
REXCVAR_DECLARE(double, ao2_versus_bots_buy_share);
REXCVAR_DECLARE(int32_t, ao2_versus_round_minutes);
REXCVAR_DECLARE(int32_t, ao2_versus_cash_goal);
REXCVAR_DECLARE(int32_t, ao2_versus_friendly_fire);
REXCVAR_DECLARE(int32_t, ao2_versus_respawn_cost);
REXCVAR_DECLARE(int32_t, ao2_versus_respawn_step);

REXCVAR_DEFINE_STRING(ao2_debug_menu_key, "F1", "Debug",
                      "Key that opens the debug menu (graphics, achievements, invites, cheats, checkpoints); empty = none");
REXCVAR_DEFINE_STRING(ao2_menu_style, "classic", "Debug",
                      "Look of the F1 menu and the port's own screens: glass (translucent rounded panels over the "
                      "blurred game) or classic (the flat navy panel)");
REXCVAR_DEFINE_STRING(ao2_fps_overlay_key, "F2", "GPU",
                      "Key that switches the frame rate overlay (off, FPS, detailed); empty = none");

namespace
{
  using ao2::text::Tr;

  struct Checkpoint
  {
    std::string name;  // e.g. 05_04
    std::string level; // e.g. 05_map_storage_t
  };

  struct Mission
  {
    std::string prefix; // e.g. 05
    std::vector<Checkpoint> checkpoints;
  };

  const char *MissionTitle(const std::string &prefix)
  {
    static const std::map<std::string, const char *> kTitles = {
        {"01", "Somalia"}, {"04", "Afghanistan"}, {"05", "Iraq"},
        {"06", "Aircraft carrier"}, {"07", "China"}, {"08", "Miami"},
        {"09", "Kiev (DLC)"}, {"10", "Miami, alternate ending (DLC)"},
    };
    auto it = kTitles.find(prefix);
    return it != kTitles.end() ? Tr(it->second) : nullptr;
  }

  // Start (_100) first, then the file order, End last.
  int CheckpointRank(const std::string &name)
  {
    if (name.ends_with("_100"))
    {
      return 0;
    }
    return name.ends_with("_End") ? 2 : 1;
  }

  // A checkpoint file in a downloadable content package (next to its maps):
  // "NN_<id>", no extension, a few hundred bytes.
  bool IsCheckpointFile(const std::filesystem::directory_entry &entry)
  {
    std::error_code ec;
    std::string name = entry.path().filename().string();
    return name.size() >= 4 && name.size() <= 16 && std::isdigit(uint8_t(name[0])) &&
           std::isdigit(uint8_t(name[1])) && name[2] == '_' &&
           std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(uint8_t(c)) || c == '_'; }) &&
           entry.file_size(ec) < 4096 && !ec;
  }

  // The level a checkpoint file loads: the string before the checkpoint's
  // own name (both length-prefixed, big endian, with the terminator) - e.g.
  // 05_map_storage_t. Some (all of the DLC's) only name the persistent level
  // there (09_map_world, 08_map_persistent), which says nothing: empty then.
  std::string CheckpointLevel(const std::filesystem::path &file, const std::string &name)
  {
    FILE *f = _wfopen(file.c_str(), L"rb");
    if (!f)
    {
      return {};
    }
    uint8_t data[512];
    size_t size = std::fread(data, 1, sizeof(data), f);
    std::fclose(f);
    auto be32 = [&](size_t at) {
      return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) |
             data[at + 3];
    };
    for (size_t at = 4; at + 4 + name.size() + 1 <= size; ++at)
    {
      if (be32(at) != name.size() + 1 || std::memcmp(data + at + 4, name.data(), name.size()) != 0 ||
          data[at + 4 + name.size()] != 0)
      {
        continue;
      }
      // `at` is right after the level string's terminator: find its length.
      if (at < 6 || data[at - 1] != 0)
      {
        return {};
      }
      for (size_t start = at - 2; start >= 4 && start + 128 >= at; --start)
      {
        if (be32(start - 4) != at - start)
        {
          continue;
        }
        std::string level(reinterpret_cast<const char *>(data + start), at - start - 1);
        bool printable = !level.empty() &&
                         std::all_of(level.begin(), level.end(), [](char c) { return c > 0x20 && c < 0x7F; });
        if (!printable || level.ends_with("_map_world") || level.ends_with("_map_permanent") ||
            level.ends_with("_map_persistent"))
        {
          return {};
        }
        return level;
      }
      return {};
    }
    return {};
  }

  // The disc's checkpoints (AO2Game/Checkpoints) and those of the extracted
  // downloadable content packages (<content_dir>/<package>).
  std::vector<Mission> ScanCheckpoints(const std::filesystem::path &dir, const std::filesystem::path &content_dir)
  {
    std::map<std::string, Mission> missions;
    auto add = [&](const std::filesystem::path &file) {
      std::string name = file.filename().string();
      size_t underscore = name.find('_');
      if (underscore == std::string::npos || underscore == 0 || name.find_first_of(" ?%") != std::string::npos)
      {
        return;
      }
      Mission &mission = missions[name.substr(0, underscore)];
      mission.prefix = name.substr(0, underscore);
      if (std::none_of(mission.checkpoints.begin(), mission.checkpoints.end(),
                       [&](const Checkpoint &c) { return c.name == name; }))
      {
        mission.checkpoints.push_back({name, CheckpointLevel(file, name)});
      }
    };
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
    {
      if (entry.is_regular_file(ec))
      {
        add(entry.path());
      }
    }
    if (!content_dir.empty())
    {
      for (const auto &package : std::filesystem::directory_iterator(content_dir, ec))
      {
        std::error_code ec2;
        for (const auto &entry : std::filesystem::directory_iterator(package.path(), ec2))
        {
          if (entry.is_regular_file(ec2) && IsCheckpointFile(entry))
          {
            add(entry.path());
          }
        }
      }
    }
    std::vector<Mission> list;
    for (auto &[prefix, mission] : missions)
    {
      std::sort(mission.checkpoints.begin(), mission.checkpoints.end(), [](const Checkpoint &a, const Checkpoint &b) {
        int ra = CheckpointRank(a.name), rb = CheckpointRank(b.name);
        return ra != rb ? ra < rb : a.name < b.name;
      });
      list.push_back(std::move(mission));
    }
    return list;
  }

  const uint8_t *GuestBase()
  {
    auto *runtime = rex::Runtime::instance();
    return runtime ? runtime->virtual_membase() : nullptr;
  }

  class DebugMenu;

  rex::ui::Window *g_window = nullptr;
  rex::ui::ImGuiDrawer *g_drawer = nullptr; // the SDK's or g_own_drawer
  std::unique_ptr<rex::ui::ImmediateDrawer> g_own_immediate;
  std::unique_ptr<rex::ui::ImGuiDrawer> g_own_drawer;
  // Native renderer: the frame rate overlay's drawer and dialog (UI thread).
  std::unique_ptr<rex::ui::ImmediateDrawer> g_perf_immediate;
  std::unique_ptr<rex::ui::ImGuiDrawer> g_perf_drawer;
  std::unique_ptr<rex::ui::ImGuiDialog> g_perf_dialog;
  std::filesystem::path g_checkpoint_dir, g_content_dir;
  DebugMenu *g_menu = nullptr; // UI thread
  std::atomic<bool> g_open{false};
  // Open port windows (OpenWindow; modal like the menu) and the notice.
  std::atomic<int> g_modal{0};
  std::mutex g_notice_mutex;
  std::function<bool()> g_notice; // under g_notice_mutex
  std::function<bool()> g_toast;  // under g_notice_mutex (SetToast)
  std::function<bool()> g_banner; // under g_notice_mutex (SetBanner)
  uint64_t g_notice_gen = 0, g_toast_gen = 0, g_banner_gen = 0; // under g_notice_mutex: each set
  std::atomic<bool> g_notice_on{false}; // any of them
  rex::ui::ImmediateDrawer *g_sdk_immediate = nullptr;

  // The game gets no input: the menu or a port window is open.
  bool Blocking()
  {
    return g_open.load(std::memory_order_relaxed) || g_modal.load(std::memory_order_relaxed) > 0;
  }
  // Native renderer: UI frames are wanted / one is queued to the UI thread.
  std::atomic<bool> g_frames_wanted{false}, g_frame_queued{false};

  // --- Drawing ----------------------------------------------------------------

  namespace ui = ao2::ui;
  using ui::FontKind;

  // `text` (UTF-8) in capitals: ASCII, Latin-1 and Cyrillic letters.
  std::string Upper(const char *text)
  {
    std::string out;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p;)
    {
      uint32_t c = *p++;
      if (c >= 0xC0)
      {
        int extra = c >= 0xF0 ? 3 : (c >= 0xE0 ? 2 : 1);
        c &= 0x3F >> extra;
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; ++i)
        {
          c = (c << 6) | (*p++ & 0x3F);
        }
      }
      if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x430 && c <= 0x44F))
      {
        c -= 0x20;
      }
      else if (c >= 0x450 && c <= 0x45F)
      {
        c -= 0x50;
      }
      if (c < 0x80)
      {
        out += char(c);
      }
      else if (c < 0x800)
      {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
      }
      else
      {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
      }
    }
    return out;
  }

  // A hover state kept by ImGui for `id` (0..1, eased).
  float Eased(const char *id, bool on, float speed = 14.0f)
  {
    float &h = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID(id), 0.0f);
    h = ui::Approach(h, on ? 1.0f : 0.0f, speed);
    return h;
  }

  // A section's caption in capitals with a hairline after it (as the players
  // window's); returns the y under it.
  float Section(ImDrawList *dl, float x0, float x1, float y, const char *label, float a)
  {
    float s = ui::Scale();
    if (ui::Glass())
    {
      // Glass: the caption as written, a little brighter, and the hairline.
      ui::Text(dl, FontKind::kSmall, ImVec2(x0, y - 2.0f * s), ui::Fade(ui::kText, 0.75f * a), label);
      float lx = x0 + ui::TextSize(FontKind::kSmall, label).x + 12.0f * s;
      float ly = y + ui::FontSize(FontKind::kSmall) * 0.55f;
      dl->AddLine(ImVec2(lx, ly), ImVec2(x1, ly), ui::Fade(ui::kWhite, 0.12f * a), 1.0f * s);
      return y + ui::FontSize(FontKind::kSmall) + 14.0f * s;
    }
    std::string caps = Upper(label);
    ui::Caps(dl, FontKind::kLabel, ImVec2(x0, y), ui::Fade(ui::kMuted, a), caps.c_str());
    float lx = x0 + ui::CapsSize(FontKind::kLabel, caps.c_str()).x + 12.0f * s;
    float ly = y + ui::FontSize(FontKind::kLabel) * 0.65f;
    dl->AddLine(ImVec2(lx, ly), ImVec2(x1, ly), ui::Fade(ui::kWhite, 0.10f * a), 1.0f * s);
    return y + ui::FontSize(FontKind::kLabel) + 14.0f * s;
  }

  // A row's back: a faint lift and a paper bar on the left, both brighter
  // when hovered (`h`), as the launcher's rows.
  void RowBack(ImDrawList *dl, ImVec2 mn, ImVec2 mx, float h, float a, ImU32 bar = ui::kAccent)
  {
    float s = ui::Scale();
    ui::Card(dl, mn, mx, ui::Fade(ui::kWhite, (0.025f + 0.045f * h) * a));
    ui::Bar(dl, mn, mx, ui::Fade(bar, (0.2f + 0.8f * h) * a));
  }

  bool Hovering(ImVec2 mn, ImVec2 mx, bool live)
  {
    return live && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(mn, mx);
  }

  float ChipWidth(const char *label)
  {
    return std::max(ui::TextSize(FontKind::kSmall, label).x + 26.0f * ui::Scale(), 58.0f * ui::Scale());
  }

  // A choice: a hairline box, the picked one a paper block; true when clicked.
  bool Chip(const char *id, ImVec2 pos, ImVec2 size, const char *label, bool on, bool enabled, float a)
  {
    float s = ui::Scale();
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(pos);
    bool clicked = ImGui::InvisibleButton("##chip", size) && enabled;
    bool hovered = enabled && ImGui::IsItemHovered();
    bool held = enabled && ImGui::IsItemActive();
    float h = Eased("hover", hovered, 20.0f);
    float o = Eased("on", on, 16.0f);
    ImGui::PopID();
    if (hovered)
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 mx(pos.x + size.x, pos.y + size.y);
    if (ui::Glass())
    {
      // Glass: a pill, the picked one white with ink text.
      float r = size.y * 0.5f;
      dl->AddRectFilled(pos, mx, ui::Fade(ui::kWhite, ((held ? 0.16f : 0.06f) + 0.07f * h) * (1.0f - o) * a), r);
      dl->AddRectFilled(pos, mx, ui::Fade(ui::kWhite, (0.86f + 0.10f * h) * o * a), r);
      dl->AddRect(pos, mx, ui::Fade(ui::kWhite, (0.16f + 0.25f * h) * (1.0f - o) * a), r, ImDrawFlags_None, 1.0f * s);
      ImU32 text = enabled ? ui::Mix(ui::Mix(ui::kText, ui::kAccentLight, h), ui::kInk, o) : ui::kMuted;
      ImVec2 ts = ui::TextSize(FontKind::kSmall, label);
      dl->PushClipRect(pos, mx, true);
      ui::Text(dl, FontKind::kSmall,
               ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f - 1.0f * s), ui::Fade(text, a),
               label);
      dl->PopClipRect();
      return clicked;
    }
    dl->AddRectFilled(pos, mx, ui::Fade(ui::kWhite, ((held ? 0.14f : 0.03f) + 0.07f * h) * (1.0f - o) * a));
    dl->AddRectFilled(pos, mx, ui::Fade(ui::Mix(ui::kAccent, ui::kAccentLight, h), o * a));
    dl->AddRect(pos, mx, ui::Fade(ui::kAccent, (0.16f + 0.45f * h) * (1.0f - o) * a), 0.0f, ImDrawFlags_None,
                1.0f * s);
    ImU32 text = enabled ? ui::Mix(ui::Mix(ui::kText, ui::kAccentLight, h), ui::kInk, o) : ui::kMuted;
    ImVec2 ts = ui::TextSize(FontKind::kSmall, label);
    dl->PushClipRect(pos, mx, true);
    ui::Text(dl, FontKind::kSmall, ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f - 1.0f * s),
             ui::Fade(text, a), label);
    dl->PopClipRect();
    return clicked;
  }

  // A slider over [lo, hi] at `pos`, `w` wide; true when `value` changed.
  bool Slider(const char *id, ImVec2 pos, float w, double &value, double lo, double hi, float a, bool live)
  {
    float s = ui::Scale();
    float hgt = 28.0f * s;
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##slider", ImVec2(w, hgt));
    bool active = live && ImGui::IsItemActive();
    bool hovered = live && ImGui::IsItemHovered();
    float h = Eased("hover", hovered || active, 20.0f);
    ImGui::PopID();
    if (hovered || active)
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    bool changed = false;
    if (active)
    {
      double t = std::clamp(double(ImGui::GetIO().MousePos.x - pos.x) / double(w), 0.0, 1.0);
      double v = lo + (hi - lo) * t;
      if (v != value)
      {
        value = v;
        changed = true;
      }
    }
    float t = float(std::clamp((value - lo) / (hi - lo), 0.0, 1.0));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float cy = pos.y + hgt * 0.5f, th = 2.0f * s;
    dl->AddRectFilled(ImVec2(pos.x, cy - th), ImVec2(pos.x + w, cy + th), ui::Fade(ui::kWhite, 0.10f * a));
    dl->AddRectFilled(ImVec2(pos.x, cy - th), ImVec2(pos.x + w * t, cy + th), ui::Fade(ui::kAccent, a));
    ImVec2 knob(pos.x + w * t, cy);
    if (h > 0.01f)
    {
      dl->AddCircleFilled(knob, (9.0f + 5.0f * h) * s, ui::Fade(ui::kAccent, 0.18f * h * a));
    }
    dl->AddCircleFilled(knob, 8.0f * s, ui::Fade(ui::Mix(ui::kAccent, ui::kAccentLight, h), a));
    dl->AddCircle(knob, 8.0f * s, ui::Fade(ui::kInk, 0.6f * a), 20, 1.5f * s);
    return changed;
  }

  // An on / off switch, its track at `pos`; `o` is its eased state.
  void Switch(ImDrawList *dl, ImVec2 pos, float o, bool enabled, float a)
  {
    float s = ui::Scale();
    float w = 50.0f * s, h = 26.0f * s, r = h * 0.5f;
    ImVec2 mx(pos.x + w, pos.y + h);
    ImU32 track = ui::Mix(ui::Fade(ui::kWhite, 0.10f), ui::Fade(ui::kGreen, 0.85f), o);
    dl->AddRectFilled(pos, mx, ui::Fade(track, enabled ? a : 0.4f * a), r);
    dl->AddRect(pos, mx, ui::Fade(ui::kWhite, 0.18f * (1.0f - o) * a), r, ImDrawFlags_None, 1.0f * s);
    ImVec2 knob(pos.x + r + (w - h) * o, pos.y + r);
    dl->AddCircleFilled(knob, r - 4.0f * s, ui::Fade(enabled ? ui::kAccentLight : ui::kMuted, a));
  }

  // The sections, in the sidebar's order (keys 1-7): the settings, online
  // play, the game.
  enum Tab
  {
    kTabGraphics,
    kTabServers,
    kTabInvites,
    kTabVersus,
    kTabCheckpoints,
    kTabCheats,
    kTabAchievements,
    kTabCount,
  };
  constexpr const char *kTabNames[kTabCount] = {"Graphics", "Servers", "Invites",     "Versus",
                                                "Checkpoints", "Cheats", "Achievements"};
  // What each section is for: the line over its content (English, through Tr).
  constexpr const char *kTabHelp[kTabCount] = {
      "The picture, performance, field of view and mouse.",
      "Games hosted over Steam. Join one, or let Quick join pick the open one with the lowest ping.",
      "Players on the LAN, a VPN and Steam: invite them to your game, answer their invites.",
      "The rules of the versus matches you host, and the bots that play the empty places.",
      "Start any mission from any of its checkpoints.",
      "God mode, infinite ammo and everything unlocked.",
      "The game's achievements and your progress.",
  };
  // The sidebar's groups: a caption and the sections under it.
  struct TabGroup
  {
    const char *name;
    int first, count;
  };
  constexpr TabGroup kTabGroups[] = {{"Settings", kTabGraphics, 1}, {"Online", kTabServers, 3}, {"Game", kTabCheckpoints, 3}};
  int g_last_tab = kTabGraphics; // the tab the menu opens on (the last one shown)

  constexpr ImU32 kBadgeGold = IM_COL32(227, 185, 90, 255);
  constexpr ImU32 kPending = IM_COL32(255, 190, 80, 255); // "after restart"

  // The Graphics tab's field of view and mouse rows (fov.cpp, mouse_aim.cpp):
  // saved once let go, to settings/`file` and the launcher's `launcher_key`
  // (the launcher passes the sensitivities at every start: unsaved there, they
  // were back to its values in the next game).
  struct SliderRow
  {
    const char *label;
    double lo, hi;
    const char *format;
    double (*get)();
    void (*set)(double);
    const char *cvar;
    const char *file;
    const char *launcher_key;
  };
  const SliderRow kSliders[] = {
      {"Field of view", 40.0, 130.0, "%.0f\xC2\xB0", [] { return REXCVAR_GET(ao2_fov); },
       [](double v) { REXCVAR_SET(ao2_fov, std::round(v)); }, "ao2_fov", "hardware.toml", nullptr},
      {"Aiming zoom", 0.0, 1.0, "%.2f", [] { return REXCVAR_GET(ao2_fov_aim); },
       [](double v) { REXCVAR_SET(ao2_fov_aim, std::round(v * 100.0) / 100.0); }, "ao2_fov_aim", "hardware.toml",
       nullptr},
      {"Sensitivity", 0.1, 10.0, "%.2f", [] { return REXCVAR_GET(ao2_mouse_sensitivity); },
       [](double v) { REXCVAR_SET(ao2_mouse_sensitivity, std::round(v * 100.0) / 100.0); }, "ao2_mouse_sensitivity",
       "mapping.toml", "mouse_sens"},
      {"Zoomed sensitivity", 0.1, 3.0, "%.2f", [] { return REXCVAR_GET(ao2_mouse_zoom_sensitivity); },
       [](double v) { REXCVAR_SET(ao2_mouse_zoom_sensitivity, std::round(v * 100.0) / 100.0); },
       "ao2_mouse_zoom_sensitivity", "mapping.toml", "mouse_zoom"},
  };
  constexpr int kSliderInfo = 100; // info_ of the first slider row

  void SaveSlider(const SliderRow &sr)
  {
    char value[32];
    std::snprintf(value, sizeof(value), "%g", sr.get());
    ao2::gfxmenu::SaveValue(sr.cvar, value, sr.file, sr.launcher_key);
  }

  // The Versus tab's bot difficulties: the aim error (degrees), the frames a
  // target is in view before they shoot, the turn speed (degrees per frame)
  // and the farthest enemy they shoot at. Normal is the cvars' defaults.
  struct BotPreset
  {
    const char *name;
    double miss;
    int32_t reaction;
    double turn;
    double range;
  };
  const BotPreset kBotPresets[] = {
      {"Easy", 5.0, 40, 3.5, 3000.0},
      {"Normal", 2.5, 20, 6.0, 4000.0},
      {"Hard", 1.2, 10, 9.0, 5000.0},
  };

  // The preset the values are, -1: none (custom values).
  int BotPresetOf(double miss, int32_t reaction, double turn, double range)
  {
    for (int i = 0; i < int(std::size(kBotPresets)); ++i)
    {
      const BotPreset &p = kBotPresets[i];
      if (std::fabs(miss - p.miss) < 0.01 && reaction == p.reaction && std::fabs(turn - p.turn) < 0.01 &&
          std::fabs(range - p.range) < 0.5)
      {
        return i;
      }
    }
    return -1;
  }

  void SaveBotValue(const char *cvar, double value)
  {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", value);
    ao2::gfxmenu::SaveValue(cvar, text, "online.toml", nullptr);
  }

  void SaveBotSwitch(const char *cvar, bool on)
  {
    ao2::gfxmenu::SaveValue(cvar, on ? "true" : "false", "online.toml", nullptr);
  }

  // The Versus tab's match rules: a row of choices each (versus_rules.cpp),
  // and for a number a Custom chip with a field for any value (in `unit`s,
  // lo..hi: the range versus_bots.cpp PollSettings sends); a value not
  // among the choices lights Custom.
  struct RuleChoice
  {
    int32_t value;
    const char *name; // English, through Tr
  };
  struct RuleRow
  {
    const char *label, *cvar;
    int32_t (*get)();
    void (*set)(int32_t);
    std::vector<RuleChoice> choices;
    int32_t unit = 0; // 0: no custom value
    int32_t lo = 0, hi = 0;
    const char *suffix = nullptr; // after the field (through Tr)
  };
  const RuleRow kRuleRows[] = {
      {"Round length", "ao2_versus_round_minutes", [] { return REXCVAR_GET(ao2_versus_round_minutes); },
       [](int32_t v) { REXCVAR_SET(ao2_versus_round_minutes, v); },
       {{0, "Lobby's"}, {10, "10 min"}, {15, "15 min"}, {20, "20 min"}, {30, "30 min"}, {45, "45 min"}, {60, "60 min"}},
       1, 1, 120, "min"},
      {"Cash goal", "ao2_versus_cash_goal", [] { return REXCVAR_GET(ao2_versus_cash_goal); },
       [](int32_t v) { REXCVAR_SET(ao2_versus_cash_goal, v); },
       {{0, "Off"}, {500000, "$500K"}, {1000000, "$1M"}, {2000000, "$2M"}, {5000000, "$5M"}},
       1000, 10, 100000, "$K"},
      {"Friendly fire", "ao2_versus_friendly_fire", [] { return REXCVAR_GET(ao2_versus_friendly_fire); },
       [](int32_t v) { REXCVAR_SET(ao2_versus_friendly_fire, v); }, {{0, "Game's"}, {1, "Off"}, {2, "On"}}},
      {"Respawn price", "ao2_versus_respawn_cost", [] { return REXCVAR_GET(ao2_versus_respawn_cost); },
       [](int32_t v) { REXCVAR_SET(ao2_versus_respawn_cost, v); },
       {{5000, "$5K"}, {0, "$10K"}, {20000, "$20K"}, {50000, "$50K"}},
       1000, 1, 10000, "$K"},
      {"Each respawn dearer by", "ao2_versus_respawn_step", [] { return REXCVAR_GET(ao2_versus_respawn_step); },
       [](int32_t v) { REXCVAR_SET(ao2_versus_respawn_step, v); },
       {{0, "None"}, {5000, "+$5K"}, {10000, "+$10K"}, {25000, "+$25K"}},
       1000, 0, 10000, "$K"},
  };

  // The Versus tab's fine tuning rows, saved once let go. The lockstep runs
  // ~57 frames a second: the reaction is shown in milliseconds.
  constexpr double kMsPerLockstepFrame = 1000.0 / 57.0;
  const SliderRow kBotSliders[] = {
      {"Aim error", 0.0, 8.0, "%.1f\xC2\xB0", [] { return REXCVAR_GET(ao2_versus_bots_miss); },
       [](double v) { REXCVAR_SET(ao2_versus_bots_miss, std::round(v * 10.0) / 10.0); }, "ao2_versus_bots_miss",
       "online.toml", nullptr},
      {"Reaction time", 0.0, 60.0, "%.0f ms", [] { return double(REXCVAR_GET(ao2_versus_bots_reaction)); },
       [](double v) { REXCVAR_SET(ao2_versus_bots_reaction, int32_t(std::lround(v))); }, "ao2_versus_bots_reaction",
       "online.toml", nullptr},
      {"Turn speed", 1.0, 15.0, "%.1f\xC2\xB0", [] { return REXCVAR_GET(ao2_versus_bots_turn); },
       [](double v) { REXCVAR_SET(ao2_versus_bots_turn, std::round(v * 10.0) / 10.0); }, "ao2_versus_bots_turn",
       "online.toml", nullptr},
      {"Firing range", 1500.0, 8000.0, "%.0f", [] { return REXCVAR_GET(ao2_versus_bots_range); },
       [](double v) { REXCVAR_SET(ao2_versus_bots_range, std::round(v / 50.0) * 50.0); }, "ao2_versus_bots_range",
       "online.toml", nullptr},
      {"Cash per purchase", 0.0, 1.0, "%.0f%%", [] { return REXCVAR_GET(ao2_versus_bots_buy_share); },
       [](double v) { REXCVAR_SET(ao2_versus_bots_buy_share, std::round(v * 20.0) / 20.0); },
       "ao2_versus_bots_buy_share", "online.toml", nullptr},
  };
  constexpr int kBotReactionRow = 1;
  constexpr int kBotShareRow = 4;

  // What a Graphics tab row does (English, through Tr), by its label.
  const char *RowHelp(const char *label)
  {
    static const std::map<std::string, const char *> kHelp = {
        {"Display mode", "Fullscreen or a window."},
        {"Render resolution", "The game's 1280x720 picture drawn at this size. Switches at once."},
        {"Aspect ratio", "The picture's shape. The game needs a restart for it."},
        {"FPS limit", "The highest frame rate the game runs at."},
        {"Vertical sync", "Frames wait for the monitor: no tearing, a little more input lag."},
        {"FPS counter", "The frame rate in the corner; Detailed adds the GPU, CPU and memory."},
        {"Anisotropic filtering", "Sharper textures on surfaces seen at an angle."},
        {"Anti-aliasing", "Smooths jagged edges. SMAA is sharper, FXAA cheaper."},
        {"Ambient occlusion", "Soft shadows in corners and under objects. GTAO looks best, SSAO costs least."},
        {"Motion blur", "Blur of the camera's own movement."},
        {"Depth of field", "Blurs what is out of focus."},
        {"Shadow resolution", "Sharper shadow edges for more GPU time. Auto is the game's own."},
        {"Field of view", "How wide the view is on foot. The game's is 70\xC2\xB0."},
        {"Aiming zoom", "How much aiming narrows the view: 1 as in the game, 0 not at all. Not the sniper scope."},
        {"Sensitivity", "How fast the mouse turns the camera."},
        {"Zoomed sensitivity", "How fast the mouse turns while aiming, against the normal speed."},
    };
    auto it = kHelp.find(label);
    return it != kHelp.end() ? Tr(it->second) : "";
  }

  // `text` wrapped to `width` at `pos`; returns the y under it.
  float Wrapped(ImDrawList *dl, FontKind kind, ImVec2 pos, float width, ImU32 col, const char *text)
  {
    ImFont *font = ui::Font(kind);
    float size = ui::FontSize(kind);
    dl->AddText(font, size, ImVec2(std::floor(pos.x), std::floor(pos.y)), col, text, nullptr, width);
    return pos.y + font->CalcTextSizeA(size, FLT_MAX, width, text).y;
  }

  const char *kDifficulties[] = {"Recruit", "Contractor", "Professional"};

  // The menu: a full screen overlay in the launcher's look (ui_theme.h), as
  // the achievements and players windows: a panel with a title, a sidebar of
  // sections in groups (a click, 1-7, Tab / Shift+Tab), what the section is
  // for over its content, and a footer; the content slides in. Esc, F1 and
  // Close play it out.
  class DebugMenu final : public rex::ui::ImGuiDialog
  {
  public:
    static constexpr float kOpenTime = 0.3f;
    static constexpr float kCloseTime = 0.18f;

    explicit DebugMenu(rex::ui::ImGuiDrawer *drawer) : ImGuiDialog(drawer)
    {
      missions_ = ScanCheckpoints(g_checkpoint_dir, g_content_dir);
      difficulty_ = std::clamp(REXCVAR_GET(ao2_start_difficulty), 0, 2);
      // An invite waiting for an answer (its notice says the menu key lists it).
      tab_ = ao2::invite::HasPendingInvite() ? kTabInvites : g_last_tab;
    }

    // Any time on the UI thread (the menu key): plays the menu out.
    void RequestClose() { close_requested_ = true; }

  protected:
    void OnDraw(ImGuiIO &io) override
    {
      double now = ImGui::GetTime();
      if (close_requested_)
      {
        BeginClose();
      }
      if (opened_ < 0.0)
      {
        opened_ = now;
        tab_changed_ = now;
      }
      float a = ui::EaseOutCubic(float(now - opened_) / kOpenTime);
      if (closing_ >= 0.0)
      {
        float c = float(now - closing_) / kCloseTime;
        if (c >= 1.0f)
        {
          Close();
          return;
        }
        a *= 1.0f - ui::EaseOutCubic(c);
      }
      bool live = closing_ < 0.0;
      float s = ui::Scale();
      float since = float(now - opened_);

      const uint8_t *base = GuestBase();
      network_ = base && ao2::coop::cmd::Routed(base);
      ready_ = base && ao2::coop::cmd::Ready(base);

      const ImGuiViewport *vp = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(vp->Pos);
      ImGui::SetNextWindowSize(vp->Size);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::Begin("##debug_menu", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::PopStyleVar(2);
      if (live && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      {
        BeginClose();
      }
      ImDrawList *dl = ImGui::GetWindowDrawList();

      // --- Backdrop and panel ---
      // Glass: the game blurred behind (the renderer, overlay.h) and only
      // lightly darkened; classic: darkened well.
      bool glass = REXCVAR_GET(ao2_menu_style) != "classic";
      ui::SetGlass(glass);
      ao2::gpu::overlay::SetBackdropBlur(glass ? a : 0.0f);
      dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                        ui::Fade(ui::kInk, (glass ? 0.38f : 0.78f) * a));
      const float pad = 32.0f * s;
      const float pw = std::min(1360.0f * s, vp->Size.x - 40.0f * s);
      const float ph = std::min(900.0f * s, vp->Size.y - 60.0f * s);
      ImVec2 center = vp->GetCenter();
      float x0 = center.x - pw * 0.5f, y0 = center.y - ph * 0.5f + (1.0f - a) * 30.0f * s;
      float x1 = x0 + pw, y1 = y0 + ph;
      float cx0 = x0 + pad, cx1 = x1 - pad;
      if (glass)
      {
        // A sheet of tinted glass: dark enough to read on over a bright
        // scene, with a light rim.
        float r = 22.0f * s;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ui::Fade(ui::kPanel, 0.58f * a), r);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ui::Fade(ui::kWhite, 0.04f * a), r);
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), ui::Fade(ui::kWhite, 0.22f * a), r, ImDrawFlags_None, 1.0f * s);
      }
      else
      {
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ui::Fade(ui::kPanel, 0.96f * a));
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), ui::Fade(ui::kWhite, 0.13f * a), 0.0f, ImDrawFlags_None,
                    1.0f * s);
        ui::Corners(dl, ImVec2(x0 - 4.0f * s, y0 - 4.0f * s), ImVec2(x1 + 4.0f * s, y1 + 4.0f * s), 18.0f * s,
                    2.0f * s, ui::Fade(ui::kAccent, 0.55f * a));
      }

      // --- Header: kicker, title (wiped in) ---
      float y = y0 + pad;
      ui::Caps(dl, FontKind::kLabel, ImVec2(cx0, y), ui::Fade(ui::kMuted, a), "ARMY OF TWO  //  DEBUG");
      y += ui::FontSize(FontKind::kLabel) + 6.0f * s;
      // Glass: the title as written; classic: letter-spaced capitals.
      std::string title = glass ? std::string(Tr("Debug menu")) : Upper(Tr("Debug menu"));
      ImVec2 title_size = glass ? ui::TextSize(FontKind::kTitle, title.c_str())
                                : ui::CapsSize(FontKind::kTitle, title.c_str());
      float wipe = ui::EaseOutCubic((since - 0.05f) / 0.45f);
      dl->PushClipRect(ImVec2(cx0, y), ImVec2(cx0 + title_size.x * wipe + 1.0f, y + title_size.y), true);
      if (glass)
      {
        ui::Text(dl, FontKind::kTitle, ImVec2(cx0, y), ui::Fade(ui::kAccentLight, a), title.c_str());
      }
      else
      {
        ui::Caps(dl, FontKind::kTitle, ImVec2(cx0, y), ui::Fade(ui::kAccentLight, a), title.c_str());
      }
      dl->PopClipRect();
      y += title_size.y + 20.0f * s;

      // --- Sidebar: the sections in their groups ---
      if (live && !io.WantTextInput)
      {
        for (int i = 0; i < kTabCount; ++i)
        {
          if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_1 + i), false) ||
              ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_Keypad1 + i), false))
          {
            SetTab(i, now);
          }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Tab, false))
        {
          SetTab((tab_ + (io.KeyShift ? kTabCount - 1 : 1)) % kTabCount, now);
        }
      }
      // Badges: the achievements unlocked, an invite waiting, the cheats on.
      if (now >= badges_time_)
      {
        badges_time_ = now + 1.0;
        ao2::ach::Progress(ach_got_, ach_total_);
      }
      int cheats_on = 0;
      for (bool on : {REXCVAR_GET(ao2_god_mode), REXCVAR_GET(ao2_infinite_ammo), REXCVAR_GET(ao2_unlock_all),
                      REXCVAR_GET(ao2_unlock_bonus_weapons)})
      {
        cheats_on += on ? 1 : 0;
      }
      dl->AddLine(ImVec2(cx0, y), ImVec2(cx1, y), ui::Fade(ui::kWhite, glass ? 0.0f : 0.10f * a), 1.0f * s);
      y += glass ? 6.0f * s : 22.0f * s;
      const float side_w = 250.0f * s, item_h = 44.0f * s;
      const float side_x1 = cx0 + side_w;
      const float item_r = ui::Round(10.0f);
      {
        float sy = y;
        if (glass)
        {
          // The tray (as tall as the sections in it).
          float tray_h = 10.0f * s;
          for (const TabGroup &g : kTabGroups)
          {
            tray_h += ui::FontSize(FontKind::kSmall) + 10.0f * s + float(g.count) * (item_h + 2.0f * s) + 16.0f * s;
          }
          dl->AddRectFilled(ImVec2(cx0 - 8.0f * s, sy - 10.0f * s), ImVec2(side_x1 + 8.0f * s, sy + tray_h),
                            ui::Fade(ui::kWhite, 0.045f * a), 16.0f * s);
        }
        for (const TabGroup &g : kTabGroups)
        {
          if (glass)
          {
            ui::Text(dl, FontKind::kSmall, ImVec2(cx0 + 12.0f * s, sy), ui::Fade(ui::kMuted, a), Tr(g.name));
            sy += ui::FontSize(FontKind::kSmall) + 10.0f * s;
          }
          else
          {
            std::string caption = Upper(Tr(g.name));
            ui::Caps(dl, FontKind::kLabel, ImVec2(cx0 + 4.0f * s, sy), ui::Fade(ui::kMuted, 0.75f * a),
                     caption.c_str());
            sy += ui::FontSize(FontKind::kLabel) + 10.0f * s;
          }
          for (int i = g.first; i < g.first + g.count; ++i)
          {
            std::string badge;
            ImU32 badge_col = ui::kAccent;
            bool dot = false;
            if (i == kTabAchievements && ach_total_ > 0)
            {
              badge = std::to_string(ach_got_) + "/" + std::to_string(ach_total_);
              badge_col = kBadgeGold;
            }
            else if (i == kTabInvites)
            {
              dot = ao2::invite::HasPendingInvite();
            }
            else if (i == kTabCheats && cheats_on > 0)
            {
              badge = std::to_string(cheats_on) + " " + Upper(Tr("On"));
              badge_col = ui::kGreen;
            }
            ImVec2 mn(cx0, sy), mx(side_x1, sy + item_h);
            ImGui::PushID(i);
            ImGui::SetCursorScreenPos(mn);
            if (ImGui::InvisibleButton("##tab", ImVec2(side_w, item_h)) && live)
            {
              SetTab(i, now);
            }
            bool hovered = live && ImGui::IsItemHovered();
            float h = Eased("tab_hover", hovered, 18.0f);
            ImGui::PopID();
            if (hovered)
            {
              ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            bool active = i == tab_;
            if (active)
            {
              mark_target_ = sy;
            }
            if (h > 0.01f && !active)
            {
              ui::Card(dl, mn, mx, ui::Fade(ui::kWhite, (glass ? 0.06f : 0.035f) * h * a));
            }
            float ty = sy + (item_h - ui::FontSize(FontKind::kBody)) * 0.5f - 1.0f * s;
            const char *label = Tr(kTabNames[i]);
            ui::Text(dl, FontKind::kBody, ImVec2(cx0 + 20.0f * s, ty),
                     ui::Fade(active ? ui::kAccentLight : ui::Mix(ui::kMuted, ui::kText, 0.55f + 0.45f * h), a), label);
            // Its key on the right; a badge after the name.
            char key[4];
            std::snprintf(key, sizeof(key), "%d", i + 1);
            ImVec2 ks = ui::CapsSize(FontKind::kLabel, key);
            ui::Caps(dl, FontKind::kLabel, ImVec2(side_x1 - 14.0f * s - ks.x, sy + (item_h - ks.y) * 0.5f),
                     ui::Fade(ui::kMuted, (active ? 0.9f : 0.45f) * a), key);
            float bx = cx0 + 20.0f * s + ui::TextSize(FontKind::kBody, label).x + 10.0f * s, by = sy + item_h * 0.5f;
            if (!badge.empty())
            {
              ImVec2 bs = ui::TextSize(FontKind::kSmall, badge.c_str());
              ImVec2 bmn(bx, by - bs.y * 0.5f - 1.0f * s), bmx(bx + bs.x + 12.0f * s, by + bs.y * 0.5f + 1.0f * s);
              dl->AddRect(bmn, bmx, ui::Fade(badge_col, 0.45f * a), 0.0f, ImDrawFlags_None, 1.0f * s);
              ui::Text(dl, FontKind::kSmall, ImVec2(bx + 6.0f * s, bmn.y + 1.0f * s), ui::Fade(badge_col, a),
                       badge.c_str());
            }
            else if (dot)
            {
              float pulse = 0.5f + 0.5f * std::sin(float(now) * 4.0f);
              dl->AddCircleFilled(ImVec2(bx + 3.5f * s, by), 3.5f * s, ui::Fade(ui::kAccent, a));
              dl->AddCircle(ImVec2(bx + 3.5f * s, by), (3.5f + 4.0f * pulse) * s,
                            ui::Fade(ui::kAccent, (1.0f - pulse) * a), 16, 1.0f * s);
            }
            sy += item_h + 2.0f * s;
          }
          sy += 16.0f * s;
        }
        // The current section: a lifted block with a paper bar, sliding to it.
        mark_y_ = mark_y_ < 0.0f ? mark_target_ : ui::Approach(mark_y_, mark_target_, 16.0f);
        dl->AddRectFilled(ImVec2(cx0, mark_y_), ImVec2(side_x1, mark_y_ + item_h),
                          ui::Fade(ui::kWhite, (glass ? 0.16f : 0.07f) * a), item_r);
        if (!glass)
        {
          dl->AddRectFilled(ImVec2(cx0, mark_y_), ImVec2(cx0 + 3.0f * s, mark_y_ + item_h), ui::Fade(ui::kAccent, a));
        }
      }

      // --- Footer: the keys ---
      const float kh = 24.0f * s;
      const float footer_y = y1 - pad - kh;
      dl->AddLine(ImVec2(cx0, footer_y - 14.0f * s), ImVec2(cx1, footer_y - 14.0f * s),
                  ui::Fade(ui::kWhite, 0.08f * a), 1.0f * s);
      {
        float fx = cx0;
        float label_y = footer_y + (kh - ui::FontSize(FontKind::kLabel)) * 0.5f - 1.0f * s;
        for (const char *key : {"1-7", "TAB"})
        {
          fx += ui::KeyCap(dl, ImVec2(fx, footer_y), kh, key, a) + 6.0f * s;
        }
        std::string tabs = Upper(Tr("Sections"));
        ui::Caps(dl, FontKind::kLabel, ImVec2(fx + 2.0f * s, label_y), ui::Fade(ui::kMuted, a), tabs.c_str());

        std::string label = Upper(Tr("Close"));
        float label_w = ui::CapsSize(FontKind::kLabel, label.c_str()).x;
        float key_w = std::max(kh, ui::CapsSize(FontKind::kLabel, "ESC").x + kh * 0.8f); // as ui::KeyCap
        float total = key_w + 8.0f * s + label_w;
        ImVec2 pos(cx1 - total, footer_y);
        ImGui::SetCursorScreenPos(pos);
        if (ImGui::InvisibleButton("##close", ImVec2(total, kh)) && live)
        {
          BeginClose();
        }
        bool hovered = ImGui::IsItemHovered();
        if (hovered)
        {
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        ui::KeyCap(dl, pos, kh, "ESC", a);
        ui::Caps(dl, FontKind::kLabel, ImVec2(pos.x + key_w + 8.0f * s, label_y),
                 ui::Fade(hovered ? ui::kAccentLight : ui::kText, a), label.c_str());
      }

      // --- The section's content, right of the sidebar: what it is for,
      // then the content, sliding in from the side it was picked on ---
      float t = ui::EaseOutCubic(float(now - tab_changed_) / 0.3f);
      float ca = a * ui::Clamp01(float(now - tab_changed_) / 0.18f);
      float shift = (1.0f - t) * 24.0f * s * float(tab_dir_);
      float content_x = side_x1 + 36.0f * s;
      dl->AddLine(ImVec2(side_x1 + 18.0f * s, y), ImVec2(side_x1 + 18.0f * s, footer_y - 30.0f * s),
                  ui::Fade(ui::kWhite, glass ? 0.0f : 0.08f * a), 1.0f * s);
      {
        if (glass)
        {
          ui::Text(dl, FontKind::kHeading, ImVec2(content_x, y + shift * 0.5f), ui::Fade(ui::kAccentLight, ca),
                   Tr(kTabNames[tab_]));
        }
        else
        {
          std::string heading = Upper(Tr(kTabNames[tab_]));
          ui::Caps(dl, FontKind::kHeading, ImVec2(content_x, y + shift * 0.5f), ui::Fade(ui::kAccentLight, ca),
                   heading.c_str());
        }
        float hy = y + ui::FontSize(FontKind::kHeading) + 8.0f * s;
        hy = Wrapped(dl, FontKind::kSmall, ImVec2(content_x, hy), cx1 - content_x, ui::Fade(ui::kMuted, ca),
                     Tr(kTabHelp[tab_]));
        y = hy + 18.0f * s;
      }
      ImVec2 cmn(content_x, y + shift), cmx(cx1, footer_y - 30.0f * s + shift);
      dl->PushClipRect(ImVec2(content_x - 4.0f * s, y - 4.0f * s), ImVec2(x1 - 1.0f, footer_y - 26.0f * s), true);
      switch (tab_)
      {
      case kTabServers:
        Scrolling("servers", cmn, cmx, false, [&](ImVec2 o, float w) { return DrawServers(o, w, ca, live); });
        break;
      case kTabGraphics:
      {
        const float info_w = std::min(300.0f * s, (cmx.x - cmn.x) * 0.3f), info_gap = 28.0f * s;
        Scrolling("graphics", cmn, ImVec2(cmx.x - info_w - info_gap, cmx.y), false,
                  [&](ImVec2 o, float w) { return DrawGraphics(o, w, ca, live); });
        DrawInfo(ImVec2(cmx.x - info_w, cmn.y), cmx, ca);
      }
        break;
      case kTabAchievements:
        ao2::ach::DrawTab(cmn, cmx, fresh_);
        break;
      case kTabInvites:
        DrawInvites(cmn, cmx, ca);
        break;
      case kTabCheats:
        Scrolling("cheats", cmn, cmx, false, [&](ImVec2 o, float w) { return DrawCheats(o, w, ca, live); });
        break;
      case kTabCheckpoints:
        DrawCheckpoints(cmn, cmx, ca, live);
        break;
      case kTabVersus:
        Scrolling("versus", cmn, cmx, false, [&](ImVec2 o, float w) { return DrawVersus(o, w, ca, live); });
        break;
      }
      dl->PopClipRect();
      fresh_ = false;

      ImGui::End();
    }

    void OnClose() override
    {
      ao2::gpu::overlay::SetBackdropBlur(0.0f);
      g_last_tab = tab_;
      g_menu = nullptr;
      g_open = false;
    }

  private:
    void BeginClose()
    {
      if (closing_ < 0.0)
      {
        closing_ = ImGui::GetTime();
      }
    }

    void SetTab(int tab, double now)
    {
      if (tab == tab_)
      {
        return;
      }
      tab_dir_ = tab > tab_ ? 1 : -1;
      tab_ = tab;
      tab_changed_ = now;
      fresh_ = true;
    }

    // A scrolling child window over [mn, mx], back at its top when `reset`
    // (else where it was: ImGui keeps it between frames, tabs and openings);
    // `draw(origin, width)` draws from its top and returns the y under what
    // it drew.
    template <typename Draw>
    void Scrolling(const char *id, ImVec2 mn, ImVec2 mx, bool reset, Draw draw)
    {
      float s = ui::Scale();
      ImGui::SetCursorScreenPos(mn);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f * s);
      ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
      ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ui::Fade(ui::kWhite, 0.04f));
      ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ui::Fade(ui::kAccent, 0.45f));
      ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ui::Fade(ui::kAccent, 0.7f));
      ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ui::kAccent);
      if (ImGui::BeginChild(id, ImVec2(mx.x - mn.x, mx.y - mn.y), ImGuiChildFlags_None,
                            ImGuiWindowFlags_NoBackground))
      {
        if (reset)
        {
          ImGui::SetScrollY(0.0f);
        }
        ImVec2 o = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x - 14.0f * s;
        float bottom = draw(o, w);
        ImGui::SetCursorScreenPos(ImVec2(o.x, bottom));
        ImGui::Dummy(ImVec2(1.0f, 4.0f * s));
      }
      ImGui::EndChild();
      ImGui::PopStyleColor(4);
      ImGui::PopStyleVar(3);
    }

    // --- Graphics: the Options page's settings (graphics_menu.h), applied at
    // once, and the quick settings' field of view and mouse.
    float DrawGraphics(ImVec2 o, float w, float a, bool live)
    {
      float s = ui::Scale();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      float x0 = o.x, x1 = o.x + w, y = o.y;
      const float row_h = 52.0f * s, chip_h = 32.0f * s, gap = 6.0f * s;
      const float label_w = 260.0f * s;
      // A row's back, label and note; the hovered one is the info column's.
      auto row = [&](const char *id, int info, const char *label, const char *note, float &yy) {
        ImVec2 mn(x0, yy), mx(x1, yy + row_h);
        bool hovered = Hovering(mn, mx, live);
        if (hovered)
        {
          info_ = info;
        }
        float h = Eased(id, hovered);
        ImGui::PushID(id);
        float sel = Eased("sel", info_ == info, 16.0f);
        ImGui::PopID();
        RowBack(dl, mn, mx, std::max(h, sel), a);
        ui::Card(dl, mn, mx, ui::Fade(ui::kWhite, 0.03f * sel * a));
        float ty = mn.y + (row_h - ui::FontSize(FontKind::kBody)) * 0.5f - 1.0f * s;
        if (note)
        {
          ty -= ui::FontSize(FontKind::kSmall) * 0.5f;
          ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + 20.0f * s, ty + ui::FontSize(FontKind::kBody) + 1.0f * s),
                   ui::Fade(kPending, a), note);
        }
        ui::Text(dl, FontKind::kBody, ImVec2(mn.x + 20.0f * s, ty), ui::Fade(ui::kText, a), label);
      };

      for (int section = 0; section < 2; ++section)
      {
        y = Section(dl, x0, x1, y, Tr(section == 0 ? "Display" : "Effects"), a);
        for (int i = 0; i < ao2::gfxmenu::SettingCount(); ++i)
        {
          if (ao2::gfxmenu::SettingSection(i) != section)
          {
            continue;
          }
          std::string id = "setting" + std::to_string(i);
          bool restart = ao2::gfxmenu::WaitsForRestart(i);
          row(id.c_str(), i, Tr(ao2::gfxmenu::SettingLabel(i)), restart ? Tr("After restart") : nullptr, y);
          // The choices, right aligned; narrowed to fit when there are many.
          int n = ao2::gfxmenu::ChoiceCount(i), chosen = ao2::gfxmenu::Chosen(i);
          std::vector<float> widths(n);
          float total = 0.0f;
          for (int c = 0; c < n; ++c)
          {
            widths[c] = ChipWidth(Tr(ao2::gfxmenu::ChoiceName(i, c)));
            total += widths[c] + (c ? gap : 0.0f);
          }
          float room = x1 - 12.0f * s - (x0 + label_w);
          if (total > room)
          {
            std::fill(widths.begin(), widths.end(), (room - gap * float(n - 1)) / float(n));
            total = room;
          }
          float cx = x1 - 12.0f * s - total, cy = y + (row_h - chip_h) * 0.5f;
          ImGui::PushID(id.c_str());
          for (int c = 0; c < n; ++c)
          {
            std::string cid = std::to_string(c);
            if (Chip(cid.c_str(), ImVec2(cx, cy), ImVec2(widths[c], chip_h), Tr(ao2::gfxmenu::ChoiceName(i, c)),
                     c == chosen, live, a))
            {
              ao2::gfxmenu::Pick(i, c);
            }
            cx += widths[c] + gap;
          }
          ImGui::PopID();
          y += row_h + 6.0f * s;
        }
        y += 18.0f * s;
      }

      // Field of view (fov.cpp) and mouse (mouse_aim.cpp): saved once the
      // slider is let go (not each frame of the drag).
      y = Section(dl, x0, x1, y, Tr("View and mouse"), a);
      for (int k = 0; k < int(std::size(kSliders)); ++k)
      {
        const SliderRow &sr = kSliders[k];
        row(sr.label, kSliderInfo + k, Tr(sr.label), nullptr, y);
        double v = sr.get();
        char text[32];
        std::snprintf(text, sizeof(text), sr.format, v);
        float value_w = 70.0f * s;
        float sw = std::min(420.0f * s, x1 - 12.0f * s - value_w - (x0 + label_w));
        float sx = x1 - 12.0f * s - value_w - sw;
        if (Slider(sr.label, ImVec2(sx, y + (row_h - 28.0f * s) * 0.5f), sw, v, sr.lo, sr.hi, a, live))
        {
          sr.set(v);
          slider_dirty_[k] = true;
        }
        if (slider_dirty_[k] && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
          slider_dirty_[k] = false;
          SaveSlider(sr);
        }
        ImVec2 ts = ui::TextSize(FontKind::kBody, text);
        ui::Text(dl, FontKind::kBody, ImVec2(x1 - 12.0f * s - ts.x, y + (row_h - ts.y) * 0.5f - 1.0f * s),
                 ui::Fade(ui::kAccentLight, a), text);
        y += row_h + 6.0f * s;
      }
      y += 6.0f * s;
      std::string reset = Upper(Tr("Game's field of view"));
      float bw = ui::CapsSize(FontKind::kLabel, reset.c_str()).x + 40.0f * s;
      if (ui::Button("reset_fov", ImVec2(x0, y), ImVec2(bw, 36.0f * s), reset.c_str(), ui::ButtonStyle::kOutline, live,
                     a))
      {
        REXCVAR_SET(ao2_fov, 70.0);
        REXCVAR_SET(ao2_fov_aim, 1.0);
        SaveSlider(kSliders[0]);
        SaveSlider(kSliders[1]);
      }
      y += 36.0f * s + 30.0f * s;

      // The menu's own look (glass, or the classic panel), saved at once.
      y = Section(dl, x0, x1, y, Tr("Menu"), a);
      {
        ImVec2 mn(x0, y), mx(x1, y + row_h);
        RowBack(dl, mn, mx, Eased("menu_style", Hovering(mn, mx, live)), a);
        ui::Text(dl, FontKind::kBody, ImVec2(mn.x + 20.0f * s, mn.y + (row_h - ui::FontSize(FontKind::kBody)) * 0.5f -
                                                                   1.0f * s),
                 ui::Fade(ui::kText, a), Tr("Menu style"));
        const char *names[2] = {Tr("Glass"), Tr("Classic")};
        const char *values[2] = {"glass", "classic"};
        bool glass_now = REXCVAR_GET(ao2_menu_style) != "classic";
        float widths[2] = {ChipWidth(names[0]), ChipWidth(names[1])};
        float cx = x1 - 12.0f * s - widths[0] - widths[1] - gap, cy = y + (row_h - chip_h) * 0.5f;
        for (int c = 0; c < 2; ++c)
        {
          if (Chip(values[c], ImVec2(cx, cy), ImVec2(widths[c], chip_h), names[c], (c == 0) == glass_now, live, a))
          {
            REXCVAR_SET(ao2_menu_style, std::string(values[c]));
            ao2::gfxmenu::SaveValue("ao2_menu_style", std::string("\"") + values[c] + "\"", "hardware.toml", nullptr);
          }
          cx += widths[c] + gap;
        }
        y += row_h + 6.0f * s;
      }
      return y + 8.0f * s;
    }

    // The Graphics tab's info column: the hovered (or last hovered) row's
    // name, value and what it does; the frame rate and frame time under it.
    void DrawInfo(ImVec2 mn, ImVec2 mx, float a)
    {
      float s = ui::Scale();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      dl->AddLine(ImVec2(mn.x - 14.0f * s, mn.y), ImVec2(mn.x - 14.0f * s, mx.y), ui::Fade(ui::kWhite, 0.08f * a),
                  1.0f * s);
      const char *label = nullptr;
      std::string value;
      bool restart = false;
      if (info_ >= kSliderInfo && info_ < kSliderInfo + int(std::size(kSliders)))
      {
        const SliderRow &sr = kSliders[info_ - kSliderInfo];
        label = sr.label;
        char text[32];
        std::snprintf(text, sizeof(text), sr.format, sr.get());
        value = text;
      }
      else if (info_ >= 0 && info_ < ao2::gfxmenu::SettingCount())
      {
        label = ao2::gfxmenu::SettingLabel(info_);
        value = Tr(ao2::gfxmenu::ChoiceName(info_, ao2::gfxmenu::Chosen(info_)));
        restart = ao2::gfxmenu::WaitsForRestart(info_);
      }
      float y = mn.y, w = mx.x - mn.x;
      if (label)
      {
        std::string caps = Upper(Tr(label));
        ui::Caps(dl, FontKind::kLabel, ImVec2(mn.x, y), ui::Fade(ui::kMuted, a), caps.c_str());
        y += ui::FontSize(FontKind::kLabel) + 10.0f * s;
        ui::Text(dl, FontKind::kTitle, ImVec2(mn.x, y), ui::Fade(ui::kAccentLight, a), value.c_str());
        y += ui::FontSize(FontKind::kTitle) + 10.0f * s;
        if (restart)
        {
          ui::Text(dl, FontKind::kSmall, ImVec2(mn.x, y), ui::Fade(kPending, a), Tr("After restart"));
          y += ui::FontSize(FontKind::kSmall) + 8.0f * s;
        }
        y = Wrapped(dl, FontKind::kSmall, ImVec2(mn.x, y), w, ui::Fade(ui::kText, 0.8f * a), RowHelp(label));
        y += 28.0f * s;
      }

      // The frame rate against the limit, and the frame time.
      float fps = 0.0f, ms = 0.0f, low = 0.0f;
      ao2::perf::FrameRate(fps, ms, low);
      int32_t limit = REXCVAR_GET(ao2_fps_target);
      float top = float(limit > 0 ? limit : 240);
      auto meter = [&](const char *name, const std::string &text, float frac, ImU32 col) {
        ui::Text(dl, FontKind::kSmall, ImVec2(mn.x, y), ui::Fade(ui::kMuted, a), name);
        ImVec2 ts = ui::TextSize(FontKind::kSmall, text.c_str());
        ui::Text(dl, FontKind::kSmall, ImVec2(mx.x - ts.x, y), ui::Fade(ui::kText, a), text.c_str());
        y += ui::FontSize(FontKind::kSmall) + 6.0f * s;
        float &shown = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID(name), 0.0f);
        shown = ui::Approach(shown, ui::Clamp01(frac), 8.0f);
        dl->AddRectFilled(ImVec2(mn.x, y), ImVec2(mx.x, y + 4.0f * s), ui::Fade(ui::kWhite, 0.08f * a));
        dl->AddRectFilled(ImVec2(mn.x, y), ImVec2(mn.x + w * shown, y + 4.0f * s), ui::Fade(col, a));
        y += 4.0f * s + 14.0f * s;
      };
      if (fps > 0.0f)
      {
        char text[48];
        std::snprintf(text, sizeof(text), "%.0f FPS", fps);
        meter(Tr("Frame rate"), text, fps / top, ui::kGreen);
        std::snprintf(text, sizeof(text), "%.1f ms", ms);
        meter(Tr("Frame time"), text, ms / 33.3f, ui::kAccent);
        std::snprintf(text, sizeof(text), "%.0f FPS", low);
        meter(Tr("1% low"), text, low / top, ui::Mix(ui::kGreen, ui::kAccent, 0.5f));
      }
      Wrapped(dl, FontKind::kSmall, ImVec2(mn.x, std::max(y + 6.0f * s, mx.y - 3.0f * ui::FontSize(FontKind::kSmall))),
              w, ui::Fade(ui::kMuted, a), Tr("Settings are saved as they are changed."));
    }

    // A card with a title, a line of text, a red line under it (`warning`,
    // when set) and a switch; true when clicked.
    bool SwitchCard(const char *id, ImVec2 mn, ImVec2 mx, const char *title, const char *text, const char *warning,
                    bool on, bool enabled, float a, bool live)
    {
      float s = ui::Scale();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      float card_h = mx.y - mn.y;
      ImGui::PushID(id);
      ImGui::SetCursorScreenPos(mn);
      bool clicked = ImGui::InvisibleButton("##card", ImVec2(mx.x - mn.x, card_h)) && enabled && live;
      bool hovered = enabled && live && ImGui::IsItemHovered();
      float h = Eased("hover", hovered);
      float onf = Eased("on", on, 16.0f);
      ImGui::PopID();
      if (hovered)
      {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      }
      float ea = enabled ? a : 0.45f * a;
      ui::Card(dl, mn, mx, ui::Fade(ui::kWhite, (0.03f + 0.04f * h) * ea));
      ui::Card(dl, mn, mx, ui::Fade(ui::kGreen, 0.06f * onf * ea));
      dl->AddRect(mn, mx, ui::Fade(ui::Mix(ui::kWhite, ui::kGreen, onf), (0.10f + 0.25f * onf + 0.15f * h) * ea), 0.0f,
                  ImDrawFlags_None, 1.0f * s);
      ui::Bar(dl, mn, mx, ui::Fade(ui::Mix(ui::kMuted, ui::kGreen, onf), ea));
      float sw = 50.0f * s;
      dl->PushClipRect(mn, ImVec2(mx.x - sw - 30.0f * s, mx.y), true);
      ui::Text(dl, FontKind::kHeading, ImVec2(mn.x + 22.0f * s, mn.y + 18.0f * s), ui::Fade(ui::kText, ea), title);
      ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + 22.0f * s, mn.y + 22.0f * s + ui::FontSize(FontKind::kHeading)),
               ui::Fade(ui::kMuted, ea), text);
      if (warning)
      {
        ui::Text(dl, FontKind::kSmall,
                 ImVec2(mn.x + 22.0f * s,
                        mn.y + 26.0f * s + ui::FontSize(FontKind::kHeading) + ui::FontSize(FontKind::kSmall)),
                 ui::Fade(ui::kRed, a), warning);
      }
      dl->PopClipRect();
      Switch(dl, ImVec2(mx.x - sw - 20.0f * s, mn.y + (card_h - 26.0f * s) * 0.5f), onf, enabled, a);
      return clicked;
    }

    // --- Cheats: cards with a switch each.
    float DrawCheats(ImVec2 o, float w, float a, bool live)
    {
      float s = ui::Scale();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      struct Cheat
      {
        const char *title, *text;
        bool (*get)();
        void (*set)(bool);
        bool local; // only outside network games
      };
      static const Cheat kCheats[] = {
          {"God mode", "The player takes no damage.", [] { return REXCVAR_GET(ao2_god_mode); },
           [](bool on) { REXCVAR_SET(ao2_god_mode, on); }, false},
          {"Infinite ammo", "The magazine and the reserve stay full.", [] { return REXCVAR_GET(ao2_infinite_ammo); },
           [](bool on) { REXCVAR_SET(ao2_infinite_ammo, on); }, true},
          {"Unlock all", "The Professional difficulty and 9,999,999 money in the profile.",
           [] { return REXCVAR_GET(ao2_unlock_all); }, [](bool on) { REXCVAR_SET(ao2_unlock_all, on); }, true},
          {"Bonus weapons", "The BST-V1 and SAL-86 in the store.",
           [] { return REXCVAR_GET(ao2_unlock_bonus_weapons); },
           [](bool on) { REXCVAR_SET(ao2_unlock_bonus_weapons, on); }, true},
      };
      float y = Section(dl, o.x, o.x + w, o.y, Tr("Cheats"), a);
      const float gap = 14.0f * s, card_h = 104.0f * s;
      const int columns = w > 760.0f * s ? 2 : 1;
      const float card_w = (w - gap * float(columns - 1)) / float(columns);
      int n = 0;
      for (const Cheat &c : kCheats)
      {
        int col = n % columns;
        float cx = o.x + float(col) * (card_w + gap), cy = y + float(n / columns) * (card_h + gap);
        ++n;
        bool enabled = !(c.local && network_);
        const char *title = &c == &kCheats[0] && network_ ? Tr("God mode (my character)") : Tr(c.title);
        if (SwitchCard(c.title, ImVec2(cx, cy), ImVec2(cx + card_w, cy + card_h), title, Tr(c.text),
                       enabled ? nullptr : Tr("Not in network games."), c.get(), enabled, a, live))
        {
          c.set(!c.get());
        }
      }
      y += float((n + columns - 1) / columns) * (card_h + gap);
      if (network_)
      {
        ui::Text(dl, FontKind::kSmall, ImVec2(o.x, y + 4.0f * s), ui::Fade(ui::kMuted, a),
                 Tr("Online co-op: god mode goes to both machines, for your character."));
        y += ui::FontSize(FontKind::kSmall) + 12.0f * s;
      }
      return y;
    }

    // --- Versus: the match's rules (versus_rules.cpp) and the bots'
    // settings (versus_bots.cpp). Whose settings the match plays by on top;
    // the rules' choices; switch cards; the difficulty's chips (the one the
    // values match lit, Custom when none); the values one by one.
    // A match rule's own value (kRuleRows[ri], in its units), set when the
    // field is left or Enter pressed: clamped, applied and saved.
    void CustomRuleField(int ri, ImVec2 pos, ImVec2 size, float a, bool live)
    {
      const RuleRow &r = kRuleRows[ri];
      float s = ui::Scale();
      char *text = rule_text_[ri];
      if (!rule_editing_[ri])
      {
        std::snprintf(text, sizeof(rule_text_[ri]), "%d", r.get() / r.unit);
      }
      ImGui::PushFont(ui::Font(FontKind::kBody), ui::FontSize(FontKind::kBody));
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                          ImVec2(10.0f * s, (size.y - ui::FontSize(FontKind::kBody)) * 0.5f));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * s);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::Fade(ui::kWhite, 0.05f));
      ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ui::Fade(ui::kWhite, 0.08f));
      ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ui::Fade(ui::kWhite, 0.10f));
      ImGui::PushStyleColor(ImGuiCol_Border, ui::Fade(ui::kAccent, 0.45f));
      ImGui::PushStyleColor(ImGuiCol_Text, ui::kAccentLight);
      ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ui::Fade(ui::kAccent, 0.35f));
      ImGui::SetCursorScreenPos(pos);
      ImGui::SetNextItemWidth(size.x);
      if (rule_focus_ == ri)
      {
        ImGui::SetKeyboardFocusHere();
        rule_focus_ = -1;
      }
      ImGui::BeginDisabled(!live);
      ImGui::InputText("##value", text, sizeof(rule_text_[ri]),
                       ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
      bool done = ImGui::IsItemDeactivatedAfterEdit();
      rule_editing_[ri] = ImGui::IsItemActive();
      ImGui::EndDisabled();
      ImGui::PopStyleColor(6);
      ImGui::PopStyleVar(4);
      ImGui::PopFont();
      if (done && text[0])
      {
        int32_t v = std::clamp(int32_t(std::strtol(text, nullptr, 10)), r.lo, r.hi) * r.unit;
        r.set(v);
        ao2::gfxmenu::SaveValue(r.cvar, std::to_string(v), "online.toml", nullptr);
      }
    }

    float DrawVersus(ImVec2 o, float w, float a, bool live)
    {
      float s = ui::Scale();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      float x0 = o.x, x1 = o.x + w, y = o.y;

      // Whose settings count.
      ao2::versus::MatchSettingsInfo match = ao2::versus::MatchSettings();
      std::string status, note;
      ImU32 tone = ui::kMuted;
      if (network_ && match.from >= 0)
      {
        const ao2::versus::VersusSettings &m = match.settings;
        int preset = BotPresetOf(m.miss, m.reaction, m.turn, m.range);
        status = Tr(match.mine ? "This match plays by your rules and bot settings." :
                                 "This match plays by the host's rules and bot settings.");
        status += "  \xC2\xB7  ";
        status += !m.on ? Tr("Bots off") : preset >= 0 ? Tr(kBotPresets[preset].name) : Tr("Custom");
        note = match.mine ? "" : Tr("Changes here apply when you host.");
        tone = ui::kGreen;
      }
      else if (network_)
      {
        status = Tr("Waiting for the match's settings.");
        tone = kPending;
      }
      else
      {
        status = Tr("In a match the host's settings go to every player; yours count when you host.");
      }
      {
        float h = ui::FontSize(FontKind::kSmall) * (note.empty() ? 1.0f : 2.0f) + 22.0f * s +
                  (note.empty() ? 0.0f : 4.0f * s);
        ImVec2 mn(x0, y), mx(x1, y + h);
        ui::Card(dl, mn, mx, ui::Fade(ui::kWhite, 0.03f * a));
        ui::Bar(dl, mn, mx, ui::Fade(tone, a));
        ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + 20.0f * s, mn.y + 11.0f * s), ui::Fade(ui::kText, a),
                 status.c_str());
        if (!note.empty())
        {
          ui::Text(dl, FontKind::kSmall,
                   ImVec2(mn.x + 20.0f * s, mn.y + 15.0f * s + ui::FontSize(FontKind::kSmall)),
                   ui::Fade(ui::kMuted, a), note.c_str());
        }
        y = mx.y + 22.0f * s;
      }

      // The match's rules: a row of choices each, right aligned (narrowed
      // to fit when there are many).
      y = Section(dl, x0, x1, y, Tr("Match rules"), a);
      {
        const float rule_h = 52.0f * s, chip_h = 32.0f * s, gap = 6.0f * s, rule_label_w = 240.0f * s;
        for (int ri = 0; ri < int(std::size(kRuleRows)); ++ri)
        {
          const RuleRow &r = kRuleRows[ri];
          int32_t now = r.get();
          bool listed = std::any_of(r.choices.begin(), r.choices.end(), [&](const RuleChoice &c) { return c.value == now; });
          bool custom = r.unit > 0 && (rule_custom_[ri] || !listed);
          ImVec2 mn(x0, y), mx(x1, y + rule_h);
          RowBack(dl, mn, mx, Eased(r.cvar, Hovering(mn, mx, live)), a);
          ui::Text(dl, FontKind::kBody,
                   ImVec2(mn.x + 20.0f * s, mn.y + (rule_h - ui::FontSize(FontKind::kBody)) * 0.5f - 1.0f * s),
                   ui::Fade(ui::kText, a), Tr(r.label));
          int n = int(r.choices.size());
          int chips = n + (r.unit > 0 ? 1 : 0); // the last: Custom
          std::vector<float> widths(chips);
          float total = 0.0f;
          for (int c = 0; c < chips; ++c)
          {
            widths[c] = ChipWidth(c < n ? Tr(r.choices[c].name) : Tr("Custom"));
            total += widths[c] + (c ? gap : 0.0f);
          }
          // Custom: the field (and its unit) left of the chips.
          const char *suffix = r.suffix ? Tr(r.suffix) : "";
          float field_w = 96.0f * s, suffix_w = ui::TextSize(FontKind::kBody, suffix).x;
          float field_room = custom ? field_w + 8.0f * s + suffix_w + 16.0f * s : 0.0f;
          float room = x1 - 12.0f * s - (x0 + rule_label_w) - field_room;
          if (total > room)
          {
            std::fill(widths.begin(), widths.end(), (room - gap * float(n - 1)) / float(n));
            total = room;
          }
          float cx = x1 - 12.0f * s - total, cy = y + (rule_h - chip_h) * 0.5f;
          ImGui::PushID(r.cvar);
          if (custom)
          {
            float fx = cx - field_room;
            CustomRuleField(ri, ImVec2(fx, cy), ImVec2(field_w, chip_h), a, live);
            ui::Text(dl, FontKind::kBody,
                     ImVec2(fx + field_w + 8.0f * s, cy + (chip_h - ui::FontSize(FontKind::kBody)) * 0.5f - 1.0f * s),
                     ui::Fade(ui::kMuted, a), suffix);
          }
          for (int c = 0; c < chips; ++c)
          {
            if (c == n)
            {
              if (Chip("custom", ImVec2(cx, cy), ImVec2(widths[c], chip_h), Tr("Custom"), custom, live, a) && !custom)
              {
                rule_custom_[ri] = true;
                rule_focus_ = ri;
              }
              break;
            }
            const RuleChoice &choice = r.choices[c];
            if (Chip(choice.name, ImVec2(cx, cy), ImVec2(widths[c], chip_h), Tr(choice.name),
                     choice.value == now && !custom, live, a))
            {
              rule_custom_[ri] = false;
              r.set(choice.value);
              ao2::gfxmenu::SaveValue(r.cvar, std::to_string(choice.value), "online.toml", nullptr);
            }
            cx += widths[c] + gap;
          }
          ImGui::PopID();
          y += rule_h + 6.0f * s;
        }
        y = Wrapped(dl, FontKind::kSmall, ImVec2(x0, y + 4.0f * s), x1 - x0, ui::Fade(ui::kMuted, a),
                    Tr("A team with the cash goal wins at once. The respawn price on screen is the first one; each "
                       "later respawn of a player costs the step more.")) +
            24.0f * s;
      }

      // The switches.
      struct BotSwitch
      {
        const char *title, *text, *cvar;
        bool (*get)();
        void (*set)(bool);
      };
      static const BotSwitch kSwitches[] = {
          {"Bots", "Bots play the empty places of a versus match.", "ao2_versus_bots",
           [] { return REXCVAR_GET(ao2_versus_bots); }, [](bool on) { REXCVAR_SET(ao2_versus_bots, on); }},
          {"Buying", "Bots buy weapon kits, armor and respawns with the team's cash.", "ao2_versus_bots_buy",
           [] { return REXCVAR_GET(ao2_versus_bots_buy); }, [](bool on) { REXCVAR_SET(ao2_versus_bots_buy, on); }},
          {"Infinite ammo", "The bots' weapons never run out, as the AI partner's.", "ao2_versus_bots_infinite_ammo",
           [] { return REXCVAR_GET(ao2_versus_bots_infinite_ammo); },
           [](bool on) { REXCVAR_SET(ao2_versus_bots_infinite_ammo, on); }},
      };
      y = Section(dl, x0, x1, y, Tr("Bots"), a);
      const float gap = 14.0f * s, card_h = 92.0f * s;
      const int columns = w > 760.0f * s ? 2 : 1;
      const float card_w = (w - gap * float(columns - 1)) / float(columns);
      int n = 0;
      for (const BotSwitch &b : kSwitches)
      {
        int col = n % columns;
        float cx = x0 + float(col) * (card_w + gap), cy = y + float(n / columns) * (card_h + gap);
        ++n;
        if (SwitchCard(b.cvar, ImVec2(cx, cy), ImVec2(cx + card_w, cy + card_h), Tr(b.title), Tr(b.text), nullptr,
                       b.get(), true, a, live))
        {
          b.set(!b.get());
          SaveBotSwitch(b.cvar, b.get());
        }
      }
      y += float((n + columns - 1) / columns) * (card_h + gap) + 10.0f * s;

      // The difficulty.
      y = Section(dl, x0, x1, y, Tr("Difficulty"), a);
      int preset = BotPresetOf(REXCVAR_GET(ao2_versus_bots_miss), REXCVAR_GET(ao2_versus_bots_reaction),
                               REXCVAR_GET(ao2_versus_bots_turn), REXCVAR_GET(ao2_versus_bots_range));
      const float bh = 36.0f * s;
      float cx = x0;
      for (int i = 0; i < int(std::size(kBotPresets)); ++i)
      {
        const BotPreset &p = kBotPresets[i];
        const char *name = Tr(p.name);
        float cw = ChipWidth(name) + 24.0f * s;
        if (Chip(p.name, ImVec2(cx, y), ImVec2(cw, bh), name, i == preset, live, a))
        {
          REXCVAR_SET(ao2_versus_bots_miss, p.miss);
          REXCVAR_SET(ao2_versus_bots_reaction, p.reaction);
          REXCVAR_SET(ao2_versus_bots_turn, p.turn);
          REXCVAR_SET(ao2_versus_bots_range, p.range);
          SaveBotValue("ao2_versus_bots_miss", p.miss);
          SaveBotValue("ao2_versus_bots_reaction", p.reaction);
          SaveBotValue("ao2_versus_bots_turn", p.turn);
          SaveBotValue("ao2_versus_bots_range", p.range);
        }
        cx += cw + 6.0f * s;
      }
      if (preset < 0)
      {
        const char *custom = Tr("Custom");
        float cw = ChipWidth(custom) + 24.0f * s;
        Chip("custom", ImVec2(cx, y), ImVec2(cw, bh), custom, true, false, a);
      }
      y += bh + 28.0f * s;

      // The values one by one, saved once the slider is let go.
      y = Section(dl, x0, x1, y, Tr("Fine tuning"), a);
      const float row_h = 52.0f * s, label_w = 260.0f * s;
      for (int k = 0; k < int(std::size(kBotSliders)); ++k)
      {
        const SliderRow &sr = kBotSliders[k];
        ImVec2 mn(x0, y), mx(x1, y + row_h);
        RowBack(dl, mn, mx, Eased(sr.label, Hovering(mn, mx, live)), a);
        ui::Text(dl, FontKind::kBody, ImVec2(mn.x + 20.0f * s, mn.y + (row_h - ui::FontSize(FontKind::kBody)) * 0.5f -
                                                                   1.0f * s),
                 ui::Fade(ui::kText, a), Tr(sr.label));
        double v = sr.get();
        double shown = k == kBotReactionRow ? v * kMsPerLockstepFrame : k == kBotShareRow ? v * 100.0 : v;
        char text[32];
        std::snprintf(text, sizeof(text), sr.format, shown);
        float value_w = 80.0f * s;
        float sw = std::min(420.0f * s, x1 - 12.0f * s - value_w - (x0 + label_w));
        float sx = x1 - 12.0f * s - value_w - sw;
        if (Slider(sr.label, ImVec2(sx, y + (row_h - 28.0f * s) * 0.5f), sw, v, sr.lo, sr.hi, a, live))
        {
          sr.set(v);
          bot_slider_dirty_[k] = true;
        }
        if (bot_slider_dirty_[k] && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
          bot_slider_dirty_[k] = false;
          SaveBotValue(sr.cvar, sr.get());
        }
        ImVec2 ts = ui::TextSize(FontKind::kBody, text);
        ui::Text(dl, FontKind::kBody, ImVec2(x1 - 12.0f * s - ts.x, y + (row_h - ts.y) * 0.5f - 1.0f * s),
                 ui::Fade(ui::kAccentLight, a), text);
        y += row_h + 6.0f * s;
      }
      y += 6.0f * s;
      ui::Text(dl, FontKind::kSmall, ImVec2(x0, y), ui::Fade(ui::kMuted, a),
               Tr("Settings are saved as they are changed."));
      return y + ui::FontSize(FontKind::kSmall) + 8.0f * s;
    }

    // --- Servers: the games hosted over Steam (steam_net.h). The filters,
    // a table sorted by a click on its header (the lowest ping first), JOIN
    // or a double click on a row; QUICK JOIN the open game of this version
    // with the lowest ping. Joined, the host's game invites this one (and it
    // accepts by itself): the menu closes for the game to go there.
    float DrawServers(ImVec2 o, float w, float a, bool live)
    {
      float s = ui::Scale();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      float x0 = o.x, x1 = o.x + w, y = o.y;
      double now = ImGui::GetTime();

      auto status_box = [&](ImU32 tone, const std::string &line1, const char *line2) {
        float fh = ui::FontSize(FontKind::kSmall);
        float h = fh * (line2 ? 2.0f : 1.0f) + 22.0f * s + (line2 ? 4.0f * s : 0.0f);
        ImVec2 mn(x0, y), mx(x1, y + h);
        ui::Card(dl, mn, mx, ui::Fade(ui::kWhite, 0.03f * a));
        ui::Bar(dl, mn, mx, ui::Fade(tone, a));
        ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + 20.0f * s, mn.y + 11.0f * s), ui::Fade(ui::kText, a), line1.c_str());
        if (line2)
        {
          ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + 20.0f * s, mn.y + 15.0f * s + fh), ui::Fade(ui::kMuted, a), line2);
        }
        y = mx.y + 18.0f * s;
      };
      if (!ao2::steam::Running())
      {
        status_box(kPending, Tr("Steam is not running."),
                   Tr("Start the Steam client, then restart the game to play over Steam."));
        return y;
      }
      char signed_in[160];
      std::snprintf(signed_in, sizeof(signed_in), Tr("Signed in to Steam as %s."), ao2::steam::PersonaName().c_str());
      status_box(ui::kGreen, signed_in,
                 Tr("A game you create in the game shows here for everyone (a private one for your friends)."));

      if (fresh_ || now - servers_refreshed_ > 20.0)
      {
        servers_refreshed_ = now;
        ao2::steam::RefreshLobbies();
      }
      std::vector<ao2::steam::Lobby> all = ao2::steam::Lobbies();

      // A join under way: done once the lobby is this game's (the invite
      // follows), given up after 15 s.
      if (joining_)
      {
        auto it = std::find_if(all.begin(), all.end(), [&](const ao2::steam::Lobby &l) { return l.id == joining_; });
        if (it != all.end() && it->joined)
        {
          joining_ = 0;
          BeginClose();
        }
        else if (now - joining_since_ > 15.0)
        {
          joining_ = 0;
          join_failed_ = true;
        }
      }
      auto join = [&](uint64_t id) {
        joining_ = id;
        joining_since_ = now;
        join_failed_ = false;
        ao2::steam::JoinLobby(id);
      };
      auto open = [](const ao2::steam::Lobby &l) {
        return !l.joined && !l.in_game && l.players < l.max && !ao2::invite::IsOtherBuild(l.build);
      };

      // --- Toolbar: quick join, refresh; the filters on the right ---
      const float bh = 36.0f * s;
      const ao2::steam::Lobby *best = nullptr;
      for (const ao2::steam::Lobby &l : all)
      {
        auto ping = [](const ao2::steam::Lobby &x) { return x.ping < 0 ? 999 : x.ping; };
        if (open(l) && (!best || ping(l) < ping(*best)))
        {
          best = &l;
        }
      }
      float bx = x0;
      if (ui::Button("quick_join", ImVec2(bx, y), ImVec2(170.0f * s, bh), Tr("QUICK JOIN"), ui::ButtonStyle::kPrimary,
                     live && best && !joining_, a))
      {
        join(best->id);
      }
      bx += 170.0f * s + 8.0f * s;
      bool refreshing = ao2::steam::Refreshing();
      if (ui::Button("refresh", ImVec2(bx, y), ImVec2(140.0f * s, bh), refreshing ? Tr("SEARCHING") : Tr("REFRESH"),
                     ui::ButtonStyle::kOutline, live && !refreshing, a))
      {
        servers_refreshed_ = now;
        ao2::steam::RefreshLobbies();
      }
      {
        // Right aligned: the mode, then the two toggles.
        const char *modes[] = {Tr("All"), Tr("co-op"), Tr("versus")};
        const char *free_label = Tr("Free places");
        const char *same_label = Tr("This version");
        float ch = 32.0f * s, cy = y + (bh - ch) * 0.5f, gap = 6.0f * s;
        float widths[5] = {ChipWidth(modes[0]), ChipWidth(modes[1]), ChipWidth(modes[2]), ChipWidth(free_label),
                           ChipWidth(same_label)};
        float total = widths[0] + widths[1] + widths[2] + 18.0f * s + widths[3] + widths[4] + gap * 3.0f;
        float cx = std::max(bx + 150.0f * s, x1 - total);
        for (int m = 0; m < 3; ++m)
        {
          char id[16];
          std::snprintf(id, sizeof(id), "mode%d", m);
          if (Chip(id, ImVec2(cx, cy), ImVec2(widths[m], ch), modes[m], server_mode_ == m, live, a))
          {
            server_mode_ = m;
          }
          cx += widths[m] + (m < 2 ? gap : 18.0f * s);
        }
        if (Chip("free", ImVec2(cx, cy), ImVec2(widths[3], ch), free_label, server_free_, live, a))
        {
          server_free_ = !server_free_;
        }
        cx += widths[3] + gap;
        if (Chip("same", ImVec2(cx, cy), ImVec2(widths[4], ch), same_label, server_same_, live, a))
        {
          server_same_ = !server_same_;
        }
      }
      y += bh + 10.0f * s;
      if (joining_ || join_failed_)
      {
        ui::Text(dl, FontKind::kSmall, ImVec2(x0, y), ui::Fade(join_failed_ ? ui::kRed : ui::kText, a),
                 join_failed_ ? Tr("Couldn't join that game. It may have started or filled up; try another one.")
                              : Tr("Joining. The host's game sends the invite, and this one goes there by itself."));
        y += ui::FontSize(FontKind::kSmall) + 8.0f * s;
      }
      y += 8.0f * s;

      // --- The table ---
      std::vector<const ao2::steam::Lobby *> rows;
      for (const ao2::steam::Lobby &l : all)
      {
        if ((server_mode_ == 1 && l.versus) || (server_mode_ == 2 && !l.versus) ||
            (server_free_ && (l.in_game || l.players >= l.max) && !l.joined) ||
            (server_same_ && ao2::invite::IsOtherBuild(l.build) && !l.own))
        {
          continue;
        }
        rows.push_back(&l);
      }
      auto key = [&](const ao2::steam::Lobby &l, int col) -> std::string {
        char num[16];
        switch (col)
        {
        case 0:
          return l.name;
        case 1:
          return ao2::steam::MissionName(l.map, l.versus);
        case 2:
          return ao2::steam::ModeName(l.versus, l.game_type);
        case 3:
          std::snprintf(num, sizeof(num), "%06d", std::max(0, l.max - l.players));
          return num;
        default:
          std::snprintf(num, sizeof(num), "%06d", l.ping < 0 ? 99999 : l.ping);
          return num;
        }
      };
      std::stable_sort(rows.begin(), rows.end(), [&](const ao2::steam::Lobby *p, const ao2::steam::Lobby *q) {
        std::string kp = key(*p, server_sort_), kq = key(*q, server_sort_);
        return server_asc_ ? kp < kq : kq < kp;
      });

      const float action_w = 150.0f * s;
      const float cols[5] = {x0 + 20.0f * s, x0 + w * 0.32f, x0 + w * 0.52f, x0 + w * 0.66f, x0 + w * 0.76f};
      const float col_end[5] = {cols[1], cols[2], cols[3], cols[4], x1 - action_w - 20.0f * s};
      const char *heads[5] = {Tr("HOST"), Tr("MISSION"), Tr("MODE"), Tr("FREE"), Tr("PING")};
      for (int c = 0; c < 5; ++c)
      {
        ImVec2 ts = ui::CapsSize(FontKind::kLabel, heads[c]);
        ImGui::PushID(c);
        ImGui::SetCursorScreenPos(ImVec2(cols[c], y - 4.0f * s));
        if (ImGui::InvisibleButton("##sort", ImVec2(ts.x + 18.0f * s, ts.y + 8.0f * s)) && live)
        {
          // Again on the same column: the other way round; a new one: its usual way.
          server_asc_ = server_sort_ == c ? !server_asc_ : c != 3;
          server_sort_ = c;
        }
        bool hovered = live && ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered)
        {
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        bool sorted = server_sort_ == c;
        ui::Caps(dl, FontKind::kLabel, ImVec2(cols[c], y),
                 ui::Fade(sorted || hovered ? ui::kAccentLight : ui::kMuted, (sorted ? 1.0f : 0.8f) * a), heads[c]);
        if (sorted)
        {
          float tx = cols[c] + ts.x + 7.0f * s, tmid = y + ts.y * 0.5f, r = 3.5f * s;
          ImU32 col = ui::Fade(ui::kAccent, a);
          if (server_asc_)
          {
            dl->AddTriangleFilled(ImVec2(tx, tmid + r), ImVec2(tx + 2.0f * r, tmid + r), ImVec2(tx + r, tmid - r), col);
          }
          else
          {
            dl->AddTriangleFilled(ImVec2(tx, tmid - r), ImVec2(tx + 2.0f * r, tmid - r), ImVec2(tx + r, tmid + r), col);
          }
        }
      }
      y += ui::FontSize(FontKind::kLabel) + 10.0f * s;

      if (rows.empty())
      {
        float eh = 120.0f * s;
        ImVec2 mn(x0, y), mx(x1, y + eh);
        ui::Frame(dl, mn, mx, ui::Fade(ui::kWhite, 0.08f * a), 1.0f * s);
        const char *line1 = refreshing && all.empty() ? Tr("Looking for games...")
                            : all.empty()             ? Tr("No games over Steam right now.")
                                                      : Tr("No games match the filters.");
        const char *line2 = all.empty() ? Tr("Create a Public game in the game: it shows here for everyone.")
                                        : Tr("Turn a filter off above to see the others.");
        ImVec2 s1 = ui::TextSize(FontKind::kBody, line1), s2 = ui::TextSize(FontKind::kSmall, line2);
        ui::Text(dl, FontKind::kBody, ImVec2((x0 + x1 - s1.x) * 0.5f, y + eh * 0.5f - s1.y - 2.0f * s),
                 ui::Fade(ui::kText, a), line1);
        ui::Text(dl, FontKind::kSmall, ImVec2((x0 + x1 - s2.x) * 0.5f, y + eh * 0.5f + 4.0f * s),
                 ui::Fade(ui::kMuted, a), line2);
        return y + eh + 8.0f * s;
      }
      for (const ao2::steam::Lobby *lp : rows)
      {
        const ao2::steam::Lobby &l = *lp;
        float rh = 56.0f * s;
        ImVec2 mn(x0, y), mx(x1, y + rh);
        std::string id = "row" + std::to_string(l.id);
        ImGui::SetCursorScreenPos(mn);
        ImGui::InvisibleButton(id.c_str(), ImVec2(w - action_w - 24.0f * s, rh));
        bool row_hovered = live && ImGui::IsItemHovered();
        if (row_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && open(l) && !joining_)
        {
          join(l.id);
        }
        RowBack(dl, mn, mx, Eased(id.c_str(), row_hovered || l.id == joining_), a, l.own ? ui::kGreen : ui::kAccent);
        float ty = mn.y + (rh - ui::FontSize(FontKind::kBody)) * 0.5f - 1.0f * s;
        auto cell = [&](int c, ImU32 col, const std::string &text) {
          dl->PushClipRect(ImVec2(cols[c], mn.y), ImVec2(col_end[c] - 10.0f * s, mx.y), true);
          ui::Text(dl, FontKind::kBody, ImVec2(cols[c], ty), ui::Fade(col, a), text.c_str());
          dl->PopClipRect();
        };
        // The host, with its notes under it.
        dl->PushClipRect(mn, ImVec2(col_end[0] - 10.0f * s, mx.y), true);
        ui::Text(dl, FontKind::kBody, ImVec2(cols[0], mn.y + 8.0f * s), ui::Fade(ui::kText, a), l.name.c_str());
        std::string notes;
        auto note = [&](const char *t) { notes += (notes.empty() ? "" : "  \xC2\xB7  ") + std::string(t); };
        if (l.in_game)
        {
          note(Tr("in game"));
        }
        if (l.is_private)
        {
          note(Tr("friends only"));
        }
        ui::Text(dl, FontKind::kSmall, ImVec2(cols[0], mn.y + 32.0f * s), ui::Fade(ui::kMuted, a), notes.c_str());
        if (ao2::invite::IsOtherBuild(l.build) && !l.own)
        {
          float nx = cols[0] + ui::TextSize(FontKind::kSmall, notes.c_str()).x;
          std::string other = std::string(notes.empty() ? "" : "  \xC2\xB7  ") + Tr("different version");
          ui::Text(dl, FontKind::kSmall, ImVec2(nx, mn.y + 32.0f * s), ui::Fade(ui::kRed, a), other.c_str());
        }
        dl->PopClipRect();
        cell(1, ui::kText, ao2::steam::MissionName(l.map, l.versus));
        cell(2, ui::kText, ao2::steam::ModeName(l.versus, l.game_type));
        int free = std::max(0, l.max - l.players);
        cell(3, free && !l.in_game ? ui::kText : ui::kMuted,
             free ? std::to_string(free) + " / " + std::to_string(l.max) : std::string(Tr("FULL")));
        cell(4, l.ping < 0 ? ui::kMuted : l.ping < 80 ? ui::kGreen : l.ping < 150 ? ui::kAccent : ui::kRed,
             l.ping >= 0 ? std::to_string(l.ping) + " ms" : std::string("\xE2\x80\x94"));
        // The action.
        float abh = 34.0f * s;
        ImVec2 bp(x1 - 12.0f * s - action_w, mn.y + (rh - abh) * 0.5f);
        float mid = mn.y + rh * 0.5f;
        auto right_caps = [&](const char *text, ImU32 col) {
          ImVec2 ts = ui::CapsSize(FontKind::kLabel, text);
          ui::Caps(dl, FontKind::kLabel, ImVec2(x1 - 22.0f * s - ts.x, mid - ts.y * 0.5f), ui::Fade(col, a), text);
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
        else if (ui::Button(("join" + std::to_string(l.id)).c_str(), bp, ImVec2(action_w, abh), Tr("JOIN"),
                            ui::ButtonStyle::kPrimary, live && open(l) && !joining_, a))
        {
          join(l.id);
        }
        y += rh + 6.0f * s;
      }
      y += 6.0f * s;
      ui::Text(dl, FontKind::kSmall, ImVec2(x0, y), ui::Fade(ui::kMuted, a),
               Tr("Double click a game to join it. The list refreshes every 20 seconds."));
      return y + ui::FontSize(FontKind::kSmall) + 8.0f * s;
    }

    // --- Invites: the players window's content (coop_invite.cpp).
    void DrawInvites(ImVec2 mn, ImVec2 mx, float a)
    {
      if (!ao2::invite::Running())
      {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ui::Text(dl, FontKind::kBody, mn, ui::Fade(ui::kMuted, a), Tr("Invites are off (ao2_invites)."));
        return;
      }
      bool keep = true;
      Scrolling("invites", mn, mx, false, [&](ImVec2 o, float w) {
        keep = ao2::invite::DrawTab(o, w, fresh_);
        return ImGui::GetCursorScreenPos().y;
      });
      if (!keep)
      {
        BeginClose(); // an invite accepted: the game goes on to join it
      }
    }

    // --- Checkpoints: the missions on the left, the picked one's
    // checkpoints on the right, the difficulty and Load above them.
    void DrawCheckpoints(ImVec2 mn, ImVec2 mx, float a, bool live)
    {
      float s = ui::Scale();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      if (missions_.empty())
      {
        char text[512];
        std::snprintf(text, sizeof(text), Tr("No files in %s"), g_checkpoint_dir.string().c_str());
        ui::Text(dl, FontKind::kBody, mn, ui::Fade(ui::kMuted, a), text);
        return;
      }
      mission_ = std::clamp(mission_, 0, int(missions_.size()) - 1);
      const float list_w = std::min(340.0f * s, (mx.x - mn.x) * 0.38f), gap = 24.0f * s;

      // The missions.
      float hint_h = 2.0f * ui::FontSize(FontKind::kSmall) + 12.0f * s;
      Scrolling("missions", mn, ImVec2(mn.x + list_w, mx.y), false, [&](ImVec2 o, float w) {
        float y = Section(dl, o.x, o.x + w, o.y, Tr("Missions"), a);
        for (int i = 0; i < int(missions_.size()); ++i)
        {
          const Mission &m = missions_[i];
          const float rh = 54.0f * s;
          ImVec2 rmn(o.x, y), rmx(o.x + w, y + rh);
          ImGui::PushID(i);
          ImGui::SetCursorScreenPos(rmn);
          if (ImGui::InvisibleButton("##mission", ImVec2(w, rh)) && live)
          {
            mission_ = i;
            fresh_list_ = true;
          }
          bool hovered = live && ImGui::IsItemHovered();
          float h = Eased("hover", hovered);
          float sel = Eased("sel", i == mission_, 16.0f);
          ImGui::PopID();
          if (hovered)
          {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
          }
          RowBack(dl, rmn, rmx, std::max(h, sel), a);
          dl->AddRectFilled(rmn, rmx, ui::Fade(ui::kAccent, 0.07f * sel * a));
          ui::Caps(dl, FontKind::kHeading, ImVec2(rmn.x + 18.0f * s, rmn.y + (rh - ui::FontSize(FontKind::kHeading)) * 0.5f - 1.0f * s),
                   ui::Fade(ui::Mix(ui::kMuted, ui::kAccent, sel), a), m.prefix.c_str());
          const char *title = MissionTitle(m.prefix);
          std::string name = title ? title : Tr("Mission ") + m.prefix;
          float nx = rmn.x + 62.0f * s;
          dl->PushClipRect(rmn, ImVec2(rmx.x - 40.0f * s, rmx.y), true);
          ui::Text(dl, FontKind::kBody, ImVec2(nx, rmn.y + (rh - ui::FontSize(FontKind::kBody)) * 0.5f - 1.0f * s),
                   ui::Fade(ui::Mix(ui::kText, ui::kAccentLight, sel), a), name.c_str());
          dl->PopClipRect();
          std::string count = std::to_string(m.checkpoints.size());
          ImVec2 cs = ui::TextSize(FontKind::kSmall, count.c_str());
          ui::Text(dl, FontKind::kSmall, ImVec2(rmx.x - 16.0f * s - cs.x, rmn.y + (rh - cs.y) * 0.5f),
                   ui::Fade(ui::kMuted, a), count.c_str());
          y += rh + 4.0f * s;
        }
        return y;
      });

      // The difficulty and Load.
      float rx0 = mn.x + list_w + gap, rx1 = mx.x;
      float y = Section(dl, rx0, rx1, mn.y, Tr("Difficulty"), a);
      const float bh = 36.0f * s;
      float cx = rx0;
      for (int d = 0; d < 3; ++d)
      {
        const char *name = Tr(kDifficulties[d]);
        float cw = ChipWidth(name) + 12.0f * s;
        if (Chip(kDifficulties[d], ImVec2(cx, y), ImVec2(cw, bh), name, d == difficulty_, live, a))
        {
          difficulty_ = d;
        }
        cx += cw + 6.0f * s;
      }
      bool can_load = !network_ || ready_;
      std::string load = Upper(Tr("Load"));
      if (!selected_.empty())
      {
        load += "  " + selected_;
      }
      float lw = std::max(180.0f * s, ui::CapsSize(FontKind::kLabel, load.c_str()).x + 48.0f * s);
      bool load_now = ui::Button("load", ImVec2(rx1 - lw, y), ImVec2(lw, bh), load.c_str(), ui::ButtonStyle::kPrimary,
                                 live && can_load && !selected_.empty(), a);
      y += bh + 20.0f * s;

      // The mission's checkpoints.
      const Mission &mission = missions_[mission_];
      bool fresh = fresh_ || fresh_list_;
      bool new_list = fresh_list_;
      fresh_list_ = false;
      if (fresh)
      {
        list_shown_ = ImGui::GetTime();
      }
      ImVec2 lmn(rx0, y), lmx(rx1, mx.y - hint_h);
      Scrolling("checkpoints", lmn, lmx, new_list, [&](ImVec2 o, float w) {
        float yy = o.y;
        for (size_t i = 0; i < mission.checkpoints.size(); ++i)
        {
          const Checkpoint &c = mission.checkpoints[i];
          const float rh = 46.0f * s;
          // Rows come in one after another when the list changes.
          float rt = ui::EaseOutCubic(float(ImGui::GetTime() - list_shown_ - 0.035 * double(i)) / 0.3f);
          float ra = a * rt;
          ImVec2 rmn(o.x + (1.0f - rt) * 24.0f * s, yy), rmx(rmn.x + w, yy + rh);
          ImGui::PushID(c.name.c_str());
          ImGui::SetCursorScreenPos(ImVec2(o.x, yy));
          if (ImGui::InvisibleButton("##checkpoint", ImVec2(w, rh)) && live)
          {
            selected_ = c.name;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
              load_now = can_load;
            }
          }
          bool hovered = live && ImGui::IsItemHovered();
          float h = Eased("hover", hovered);
          float sel = Eased("sel", selected_ == c.name, 16.0f);
          ImGui::PopID();
          if (hovered)
          {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
          }
          RowBack(dl, rmn, rmx, std::max(h, sel), ra);
          dl->AddRectFilled(rmn, rmx, ui::Fade(ui::kAccent, 0.08f * sel * ra));
          if (sel > 0.01f)
          {
            dl->AddRect(rmn, rmx, ui::Fade(ui::kAccent, 0.5f * sel * ra), 0.0f, ImDrawFlags_None, 1.0f * s);
          }
          float ty = rmn.y + (rh - ui::FontSize(FontKind::kBody)) * 0.5f - 1.0f * s;
          ui::Text(dl, FontKind::kBody, ImVec2(rmn.x + 20.0f * s, ty), ui::Fade(ui::kText, ra), c.name.c_str());
          float tx = rmn.x + 20.0f * s + ui::TextSize(FontKind::kBody, "00_0000").x + 16.0f * s;
          const char *tag = c.name.ends_with("_100") ? Tr("Start") : c.name.ends_with("_End") ? Tr("End") : nullptr;
          if (tag)
          {
            std::string caps = Upper(tag);
            ImVec2 ts = ui::CapsSize(FontKind::kLabel, caps.c_str());
            ImVec2 pmn(tx, rmn.y + (rh - ts.y - 8.0f * s) * 0.5f), pmx(tx + ts.x + 16.0f * s, pmn.y + ts.y + 8.0f * s);
            dl->AddRect(pmn, pmx, ui::Fade(ui::kAccent, 0.5f * ra), 0.0f, ImDrawFlags_None, 1.0f * s);
            ui::Caps(dl, FontKind::kLabel, ImVec2(pmn.x + 8.0f * s, pmn.y + 4.0f * s), ui::Fade(ui::kAccent, ra),
                     caps.c_str());
          }
          if (!c.level.empty())
          {
            ImVec2 ls = ui::TextSize(FontKind::kSmall, c.level.c_str());
            ui::Text(dl, FontKind::kSmall, ImVec2(rmx.x - 18.0f * s - ls.x, rmn.y + (rh - ls.y) * 0.5f),
                     ui::Fade(ui::kMuted, ra), c.level.c_str());
          }
          yy += rh + 4.0f * s;
        }
        return yy;
      });

      // The hint under them.
      float hy = mx.y - hint_h + 10.0f * s;
      const char *hint1 = network_ ? Tr("Loads for both. Your partner needs the same")
                                   : Tr("Double-click loads too. Saving the game writes");
      const char *hint2 = network_ ? Tr("files (a DLC checkpoint without the DLC breaks his game).")
                                   : Tr("the loaded checkpoint to the profile as the current one.");
      ui::Text(dl, FontKind::kSmall, ImVec2(rx0, hy), ui::Fade(ui::kMuted, a), hint1);
      ui::Text(dl, FontKind::kSmall, ImVec2(rx0, hy + ui::FontSize(FontKind::kSmall) + 2.0f * s),
               ui::Fade(ui::kMuted, a), hint2);

      if (load_now && can_load && !selected_.empty() && live)
      {
        std::string line = "checkpoint " + selected_ + " " + std::to_string(difficulty_);
        if (network_)
        {
          ao2::coop::cmd::Send(line);
        }
        else
        {
          ao2::console::Queue(line);
        }
        BeginClose();
      }
    }

    std::vector<Mission> missions_;
    int mission_ = 0;
    std::string selected_;
    int difficulty_ = 0;
    int tab_ = 0, tab_dir_ = 1;
    bool fresh_ = true;       // the tab's first frame: its content starts over
    bool fresh_list_ = false; // another mission picked
    double opened_ = -1.0, closing_ = -1.0, tab_changed_ = 0.0, list_shown_ = 0.0;
    float mark_y_ = -1.0f, mark_target_ = 0.0f; // the sidebar's current section block (y)
    // Servers: the filters, the sort, the join under way.
    int server_mode_ = 0;           // 0 all, 1 co-op, 2 versus
    bool server_free_ = false;      // only games with a free place
    bool server_same_ = true;       // only this version
    int server_sort_ = 4;           // column: 0 host, 1 mission, 2 mode, 3 players, 4 ping
    bool server_asc_ = true;
    double servers_refreshed_ = -100.0;
    uint64_t joining_ = 0;          // the lobby being joined
    double joining_since_ = 0.0;
    bool join_failed_ = false;
    bool network_ = false, ready_ = false;
    bool close_requested_ = false;
    double badges_time_ = 0.0;
    int ach_got_ = 0, ach_total_ = 0;
    int info_ = 0; // the Graphics tab's row the info column tells about (kSliderInfo + n: a slider)
    bool slider_dirty_[4] = {}; // a slider moved and not yet saved (kSliders)
    bool bot_slider_dirty_[std::size(kBotSliders)] = {}; // the same for kBotSliders
    bool rule_custom_[std::size(kRuleRows)] = {};        // Custom picked (the value may still be a choice's)
    char rule_text_[std::size(kRuleRows)][16] = {};      // their fields' text
    bool rule_editing_[std::size(kRuleRows)] = {};       // ... being typed in (kept, not refreshed)
    int rule_focus_ = -1;                                 // the field to take the keyboard next frame
  };

  void Toggle()
  {
    if (!g_drawer)
    {
      return;
    }
    if (g_menu)
    {
      g_menu->RequestClose();
      return;
    }
    g_menu = new DebugMenu(g_drawer);
    g_open = true;
    g_frames_wanted = g_own_drawer != nullptr;
  }

  // UI thread: draws the banner (SetBanner), the notice (SetNotice) and the
  // toast (SetToast); clears each once it is over.
  void DrawNotice()
  {
    std::function<bool()> notice, toast, banner;
    uint64_t notice_gen, toast_gen, banner_gen;
    {
      std::lock_guard lock(g_notice_mutex);
      notice = g_notice;
      toast = g_toast;
      banner = g_banner;
      notice_gen = g_notice_gen;
      toast_gen = g_toast_gen;
      banner_gen = g_banner_gen;
    }
    bool banner_over = banner && !banner();
    bool notice_over = notice && !notice();
    bool toast_over = toast && !toast();
    if (notice_over || toast_over || banner_over)
    {
      std::lock_guard lock(g_notice_mutex);
      // Unless set again meanwhile.
      if (notice_over && g_notice_gen == notice_gen)
      {
        g_notice = nullptr;
      }
      if (toast_over && g_toast_gen == toast_gen)
      {
        g_toast = nullptr;
      }
      if (banner_over && g_banner_gen == banner_gen)
      {
        g_banner = nullptr;
      }
      g_notice_on = g_notice || g_toast || g_banner;
    }
  }

  class PerfDialog final : public rex::ui::ImGuiDialog
  {
  public:
    explicit PerfDialog(rex::ui::ImGuiDrawer *drawer) : ImGuiDialog(drawer) {}

  protected:
    void OnDraw(ImGuiIO &) override
    {
      if (ao2::perf::GetMode() != ao2::perf::kOff)
      {
        ao2::perf::Draw();
      }
      DrawNotice();
    }
  };

  // Xenos presenter (no layer of our own): the notice as a dialog of the
  // SDK's drawer; it closes itself when the notice is over.
  class NoticeDialog final : public rex::ui::ImGuiDialog
  {
  public:
    explicit NoticeDialog(rex::ui::ImGuiDrawer *drawer) : ImGuiDialog(drawer) {}

  protected:
    void OnDraw(ImGuiIO &) override
    {
      DrawNotice();
      if (!g_notice_on)
      {
        Close();
      }
    }
    void OnClose() override { g_notice_dialog = false; }

  public:
    static inline bool g_notice_dialog = false; // UI thread
  };

  // A port window (OpenWindow).
  class PortWindow;
  std::map<std::string, PortWindow *> g_windows; // UI thread

  class PortWindow final : public rex::ui::ImGuiDialog
  {
  public:
    PortWindow(rex::ui::ImGuiDrawer *drawer, std::string id, std::function<bool()> draw, bool own_escape)
        : ImGuiDialog(drawer), id_(std::move(id)), draw_(std::move(draw)), own_escape_(own_escape)
    {
      ++g_modal;
    }
    ~PortWindow() override
    {
      Forget();
    }

  protected:
    void OnDraw(ImGuiIO &) override
    {
      if (!draw_() || (!own_escape_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
      {
        Close();
      }
    }
    void OnClose() override { Forget(); }

  private:
    void Forget()
    {
      if (forgotten_)
      {
        return;
      }
      forgotten_ = true;
      --g_modal;
      auto it = g_windows.find(id_);
      if (it != g_windows.end() && it->second == this)
      {
        g_windows.erase(it);
      }
    }

    std::string id_;
    std::function<bool()> draw_;
    bool own_escape_;
    bool forgotten_ = false;
  };

  // Native renderer, UI thread: the frame rate overlay's dialog exists while
  // the overlay or a notice is on.
  void SyncPerfDialog()
  {
    bool on = ao2::perf::GetMode() != ao2::perf::kOff || g_notice_on.load(std::memory_order_relaxed);
    if (on && !g_perf_dialog)
    {
      g_perf_dialog = std::make_unique<PerfDialog>(g_perf_drawer.get());
      // Its first dialog made the drawer an input listener.
      g_window->RemoveInputListener(g_perf_drawer.get());
    }
    else if (!on && g_perf_dialog)
    {
      g_perf_dialog.reset();
      ao2::gpu::overlay::Clear(ao2::gpu::overlay::kPerfLayer);
    }
  }

  // Native renderer, UI thread: the next ImGui frame of each layer.
  void DrawOwnFrame()
  {
    g_frame_queued = false;
    SyncPerfDialog();
    rex::ui::AppUIDrawContext context(g_window->GetActualPhysicalWidth(), g_window->GetActualPhysicalHeight());
    if (g_perf_dialog)
    {
      g_perf_drawer->Draw(context);
    }
    if (g_own_drawer->HasDialogs())
    {
      g_own_drawer->Draw(context);
    }
    if (!g_own_drawer->HasDialogs())
    {
      ao2::gpu::overlay::Clear(ao2::gpu::overlay::kMenuLayer);
    }
    g_frames_wanted = g_perf_dialog || g_own_drawer->HasDialogs() || g_notice_on;
  }

  // Native renderer, at each present (GPU thread).
  void RequestOwnFrame()
  {
    bool wanted = g_frames_wanted.load(std::memory_order_relaxed) || ao2::perf::GetMode() != ao2::perf::kOff ||
                  g_notice_on.load(std::memory_order_relaxed);
    if (!wanted || g_frame_queued.exchange(true))
    {
      return;
    }
    if (!g_window->app_context().CallInUIThreadDeferred(DrawOwnFrame))
    {
      g_frame_queued = false;
    }
  }

  // Above ImGui (64): the menu key and the frame rate overlay's.
  class KeyListener final : public rex::ui::WindowInputListener
  {
  public:
    void OnKeyDown(rex::ui::KeyEvent &e) override
    {
      if (IsKey(e, REXCVAR_GET(ao2_debug_menu_key)))
      {
        e.set_handled(true);
        if (!e.prev_state())
        {
          Toggle();
        }
      }
      else if (IsKey(e, REXCVAR_GET(ao2_fps_overlay_key)))
      {
        e.set_handled(true);
        if (!e.prev_state())
        {
          ao2::perf::CycleMode();
        }
      }
      else if (IsKey(e, REXCVAR_GET(ao2_invite_accept_key)) && ao2::invite::HasPendingInvite())
      {
        e.set_handled(true);
        if (!e.prev_state())
        {
          ao2::invite::AcceptPendingInvite();
        }
      }
    }

  private:
    static bool IsKey(const rex::ui::KeyEvent &e, const std::string &key)
    {
      return !key.empty() && e.virtual_key() == rex::ui::ParseVirtualKey(key);
    }
  };

  // Under ImGui (64), above the mouse aiming (32), cutscene skip (33) and
  // the input drivers (0): nothing but key releases gets past the open menu.
  class InputBlocker final : public rex::ui::WindowInputListener
  {
  public:
    void OnKeyDown(rex::ui::KeyEvent &e) override { e.set_handled(Blocking()); }
    void OnKeyChar(rex::ui::KeyEvent &e) override { e.set_handled(Blocking()); }
    void OnMouseDown(rex::ui::MouseEvent &e) override { e.set_handled(Blocking()); }
    void OnMouseMove(rex::ui::MouseEvent &e) override { e.set_handled(Blocking()); }
    void OnMouseWheel(rex::ui::MouseEvent &e) override { e.set_handled(Blocking()); }
  };

  KeyListener g_key_listener;
  InputBlocker g_input_blocker;
} // namespace

namespace ao2::menu
{
  void Attach(rex::ui::Window *window, rex::ui::ImGuiDrawer *sdk_drawer, rex::ui::ImmediateDrawer *sdk_immediate,
              rex::input::InputSystem *input, const std::filesystem::path &game_data_root)
  {
    if (!window || g_window)
    {
      return;
    }
    g_window = window;
    g_sdk_immediate = sdk_drawer ? sdk_immediate : nullptr;
    g_checkpoint_dir = game_data_root / "AO2Game" / "Checkpoints";
    g_content_dir = ao2::dlc::Dir();
    g_drawer = sdk_drawer;
    if (!g_drawer)
    {
      g_own_immediate = ao2::gpu::overlay::CreateImmediateDrawer(ao2::gpu::overlay::kMenuLayer);
      g_own_drawer = std::make_unique<rex::ui::ImGuiDrawer>(window, 64, ConfigureFonts);
      g_own_drawer->SetPresenterAndImmediateDrawer(nullptr, g_own_immediate.get());
      g_drawer = g_own_drawer.get();
      g_perf_immediate = ao2::gpu::overlay::CreateImmediateDrawer(ao2::gpu::overlay::kPerfLayer);
      g_perf_drawer = std::make_unique<rex::ui::ImGuiDrawer>(window, 63, ao2::perf::ConfigureFonts);
      g_perf_drawer->SetPresenterAndImmediateDrawer(nullptr, g_perf_immediate.get());
      ao2::gpu::overlay::SetFrameRequest(RequestOwnFrame);
    }
    window->AddInputListener(&g_key_listener, 100);
    window->AddInputListener(&g_input_blocker, 48);

    if (input)
    {
      // Replaces the SDK's callback (window focus; with its overlays open,
      // whether ImGui wants the mouse).
      input->SetActiveCallback([window, sdk_drawer]() {
        if (!window->HasFocus() || Blocking())
        {
          return false;
        }
        if (sdk_drawer && sdk_drawer->HasDialogs())
        {
          return !sdk_drawer->GetIO().WantCaptureMouse;
        }
        return true;
      });
    }
  }

  bool IsOpen()
  {
    return Blocking();
  }

  void ConfigureFonts(ImFontAtlas *atlas)
  {
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF, // Basic Latin, Latin-1
        0x0400, 0x04FF, // Cyrillic
        0x2010, 0x205E, // punctuation
        0,
    };
    const char *kFont = "C:\\Windows\\Fonts\\segoeui.ttf";
    std::error_code ec;
    if (std::filesystem::exists(kFont, ec))
    {
      ImFontConfig config;
      config.OversampleH = 2;
      config.OversampleV = 1;
      if (ImFont *font = atlas->AddFontFromFileTTF(kFont, 18.0f, &config, kRanges))
      {
        ImGui::GetIO().FontDefault = font;
      }
    }
    ao2::ui::AddFonts(atlas); // the players window (coop_invite.cpp)
  }

  void OpenWindow(const std::string &id, std::function<bool()> draw, bool own_escape)
  {
    if (!g_window)
    {
      return;
    }
    g_window->app_context().CallInUIThreadDeferred([id, draw = std::move(draw), own_escape]() mutable {
      if (!g_drawer)
      {
        return;
      }
      auto it = g_windows.find(id);
      if (it != g_windows.end())
      {
        delete it->second; // forgets itself
      }
      g_windows[id] = new PortWindow(g_drawer, id, std::move(draw), own_escape);
      g_frames_wanted = g_own_drawer != nullptr;
    });
  }

  void CloseWindow(const std::string &id)
  {
    if (!g_window)
    {
      return;
    }
    g_window->app_context().CallInUIThreadDeferred([id]() {
      auto it = g_windows.find(id);
      if (it != g_windows.end())
      {
        delete it->second;
        if (g_own_drawer && !g_own_drawer->HasDialogs())
        {
          ao2::gpu::overlay::Clear(ao2::gpu::overlay::kMenuLayer);
        }
      }
    });
  }

  void ShowNoticeDialog();

  void SetNotice(std::function<bool()> draw)
  {
    {
      std::lock_guard lock(g_notice_mutex);
      g_notice = std::move(draw);
      ++g_notice_gen;
      g_notice_on = g_notice || g_toast || g_banner;
    }
    ShowNoticeDialog();
  }

  void SetToast(std::function<bool()> draw)
  {
    {
      std::lock_guard lock(g_notice_mutex);
      g_toast = std::move(draw);
      ++g_toast_gen;
      g_notice_on = g_notice || g_toast || g_banner;
    }
    ShowNoticeDialog();
  }

  void SetBanner(std::function<bool()> draw)
  {
    {
      std::lock_guard lock(g_notice_mutex);
      g_banner = std::move(draw);
      ++g_banner_gen;
      g_notice_on = g_notice || g_toast || g_banner;
    }
    ShowNoticeDialog();
  }

  rex::ui::ImmediateDrawer *NoticeImmediateDrawer()
  {
    return g_perf_drawer ? g_perf_immediate.get() : g_sdk_immediate;
  }

  // Xenos presenter: the dialog that draws the notices on the SDK's drawer.
  void ShowNoticeDialog()
  {
    if (!g_window || g_perf_drawer)
    {
      return; // native renderer: the next frame request draws it
    }
    g_window->app_context().CallInUIThreadDeferred([]() {
      if (g_drawer && g_notice_on && !NoticeDialog::g_notice_dialog)
      {
        NoticeDialog::g_notice_dialog = true;
        new NoticeDialog(g_drawer);
      }
    });
  }
} // namespace ao2::menu
