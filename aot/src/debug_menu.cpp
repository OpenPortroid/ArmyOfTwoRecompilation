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
// A full screen overlay in the launcher's look (ui_theme.h) with seven tabs:
// - Graphics: the Options screen's Graphics page settings (graphics_menu.h),
//   applied at once and saved as the page saves them (the render resolution
//   too: the page waits for its Accept), and the menu's own scale.
// - Controls: mouse, aiming, camera and the pad's vibration, applied at once
//   and saved, also for the launcher where it passes the value at every start.
// - Sound: the volumes (audio_settings.cpp).
// - Online: the players window's content (coop_invite.h; the menu opens on it
//   while an invite waits for an answer), joining with a code, the servers.
// - Versus: the match's rules (versus_rules.cpp: round length, cash goal,
//   friendly fire, respawn price and how much dearer each respawn gets) and
//   the bots of its empty places (versus_bots.cpp): on or off, buying,
//   infinite ammo, a difficulty (presets of the aim error, reaction, turn
//   speed and range, or those values one by one); saved to
//   settings/online.toml as they change. In a match every machine sends its
//   settings and the match plays by the host's (coop_cmd.cpp); the tab says
//   whose they are.
// - Debug: the checkpoints, the files in AO2Game/Checkpoints and in the
//   extracted DLC packages (Kiev 09_*, Miami alternate ending 10_*; the name of
//   each, then its level), loaded with the console's "checkpoint <name>
//   <difficulty>" (console_cmd.cpp; the loaded checkpoint becomes the profile's
//   current one when the game saves it), and the cheats: god mode, infinite
//   ammo, Unlock All, the bonus weapons (cheats.cpp).
// - Achievements: every achievement with its unlock (achievements.h).
// - The tabs carry badges: achievements unlocked, an invite waiting, the
//   cheats on.
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
#include "menu_mouse.h"

#include "achievements.h"
#include "console_cmd.h"
#include "coop.h"
#include "coop_cmd.h"
#include "coop_invite.h"
#include "coop_net_status.h"
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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

namespace ao2::browser
{
  // game_browser.cpp: a versus match's details card beside a server list's row (drawn over everything).
  void MatchDetails(const ao2::steam::Lobby &l, float alpha, ImVec2 list_mn, ImVec2 list_mx, float row_y, float row_h);
  void MatchDetailsHidden();
}
#include <imgui_internal.h>

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

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

REXCVAR_DECLARE(bool, ao2_god_mode);
REXCVAR_DECLARE(bool, ao2_infinite_ammo);
REXCVAR_DECLARE(bool, ao2_unlock_all);
REXCVAR_DECLARE(bool, ao2_unlock_bonus_weapons);
REXCVAR_DECLARE(int32_t, ao2_fps_target);
REXCVAR_DECLARE(double, ao2_fov);
REXCVAR_DECLARE(double, ao2_fov_aim);
REXCVAR_DECLARE(double, ao2_camera_shake);
REXCVAR_DECLARE(double, ao2_mouse_sensitivity);
REXCVAR_DECLARE(double, ao2_mouse_zoom_sensitivity);
REXCVAR_DECLARE(double, ao2_mouse_stick_sensitivity);
REXCVAR_DECLARE(bool, ao2_mouse_invert_y);
REXCVAR_DECLARE(bool, ao2_aim_assist);
REXCVAR_DECLARE(bool, ao2_toggle_aim);
REXCVAR_DECLARE(double, ao2_pad_vibration);
REXCVAR_DECLARE(double, ao2_volume_master);
REXCVAR_DECLARE(double, ao2_volume_music);
REXCVAR_DECLARE(double, ao2_volume_effects);
REXCVAR_DECLARE(double, ao2_volume_voices);
REXCVAR_DECLARE(double, ao2_achievement_sound);
REXCVAR_DECLARE(int32_t, ao2_start_difficulty);
REXCVAR_DECLARE(std::string, ao2_invite_accept_key);
REXCVAR_DECLARE(bool, ao2_versus_bots);
REXCVAR_DECLARE(bool, ao2_versus_bots_buy);
REXCVAR_DECLARE(bool, ao2_versus_bots_infinite_ammo);
REXCVAR_DECLARE(double, ao2_versus_bots_miss);
REXCVAR_DECLARE(int32_t, ao2_versus_bots_reaction);
REXCVAR_DECLARE(double, ao2_versus_bots_turn);
REXCVAR_DECLARE(double, ao2_versus_bots_range);
REXCVAR_DECLARE(int32_t, ao2_versus_round_minutes);
REXCVAR_DECLARE(int32_t, ao2_versus_cash_goal);
REXCVAR_DECLARE(int32_t, ao2_versus_friendly_fire);
REXCVAR_DECLARE(int32_t, ao2_versus_respawn_cost);
REXCVAR_DECLARE(int32_t, ao2_versus_respawn_step);

REXCVAR_DEFINE_BOOL(ao2_servers_versus, false, "Online",
                    "The F1 menu's Servers list shows versus matches (else co-op games); kept as last picked there");
REXCVAR_DEFINE_BOOL(ao2_debug_menu_pad, true, "Debug",
                    "Select + Start on a gamepad opens and closes the menu of the menu key. Off: Start and Select "
                    "alone are not held back ~0.1 s to see whether the other follows");
REXCVAR_DEFINE_DOUBLE(ao2_menu_scale, 1.3, "Debug",
                      "Size of the menu key's menu against the port's other screens (1 to 1.6); changes at once");
REXCVAR_DEFINE_STRING(ao2_debug_menu_key, "F1", "Debug",
                      "Key that opens the debug menu (graphics, achievements, invites, cheats, checkpoints); empty = none");
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
  std::function<bool()> g_hint;   // under g_notice_mutex (SetHint)
  uint64_t g_notice_gen = 0, g_toast_gen = 0, g_banner_gen = 0, g_hint_gen = 0; // under g_notice_mutex: each set
  std::atomic<bool> g_notice_on{false}; // any of them
  rex::ui::ImmediateDrawer *g_sdk_immediate = nullptr;

  // The game gets no input: the menu or a port window is open.
  bool Blocking()
  {
    return g_open.load(std::memory_order_relaxed) || g_modal.load(std::memory_order_relaxed) > 0 ||
           ao2::steam::OverlayBlocksInput();
  }
  // Native renderer: UI frames are wanted / one is queued to the UI thread.
  std::atomic<bool> g_frames_wanted{false}, g_frame_queued{false};
  // Per player: the game's polls left to see the pad let go after the menu (OnPadPoll).
  std::atomic<int> g_pad_swallow[4];
  // Keys pressed while the game got no input, until let go (InputBlocker).
  std::atomic<bool> g_key_held[256];
  constexpr ImGuiKey kPadKeys[5] = {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft,
                                    ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadFaceDown};

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

  // A switch's colour when on, as the launcher's.
  constexpr ImU32 kOnColor = IM_COL32(47, 134, 201, 255);

  // A section's caption over its group; returns the y under it.
  float Section(ImDrawList *dl, float x0, float x1, float y, const char *label, float a)
  {
    float s = ui::Scale();
    ui::Text(dl, FontKind::kLabel, ImVec2(x0 + 4.0f * s, y), ui::Fade(ui::kMuted, a), label);
    return y + ui::FontSize(FontKind::kLabel) + 12.0f * s;
  }

  // A row's back: a faint lift, brighter when hovered (`h`); `bar` is its state mark.
  void RowBack(ImDrawList *dl, ImVec2 mn, ImVec2 mx, float h, float a, ImU32 bar = ui::kAccent)
  {
    ui::Card(dl, mn, mx, ui::Fade(ui::kWhite, (0.035f + 0.045f * h) * a));
    ui::Bar(dl, mn, mx, ui::Fade(bar, (0.25f + 0.75f * h) * a));
  }

  // --- The gamepad in the menu: ImGui's own navigation goes over the buttons, which all are
  // InvisibleButtons (those with ImGuiButtonFlags_EnableNav: others it passes by); what is hovered
  // with the mouse is focused with the pad (ImGui's items and Hovering).
  bool g_pad_mode = false;     // the pad, not the mouse and keys, is in use (UI thread)
  bool g_open_by_pad = false;  // the menu being opened starts in that mode
  ImVec2 g_pad_focus_mn, g_pad_focus_mx; // the focused item, last frame
  bool g_pad_focus_valid = false;
  int g_pad_step = 0;          // a focused slider's move this frame
  bool g_pad_lr_held = false;  // left or right is down: a slider saves when let go
  bool g_pad_on_slider = false; // a slider is focused: it has left and right, navigation does not
  thread_local bool g_pad_reading = false; // this thread reads the pad past the active callback

  struct PadRead
  {
    uint16_t buttons = 0;
    uint8_t triggers[2] = {};
    int16_t thumbs[4] = {}; // lx, ly, rx, ry
  };

  // rexruntime.dll's rex_input_read_pads: false without a pad or from a runtime without it.
  bool ReadPads(PadRead &out)
  {
    using Fn = uint32_t (*)(uint16_t *, uint8_t *, int16_t *);
    static Fn fn = []() -> Fn {
      HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
      return runtime ? reinterpret_cast<Fn>(GetProcAddress(runtime, "rex_input_read_pads")) : nullptr;
    }();
    if (!fn)
    {
      return false;
    }
    g_pad_reading = true;
    bool any = fn(&out.buttons, out.triggers, out.thumbs) != 0;
    g_pad_reading = false;
    return any;
  }

  bool Hovering(ImVec2 mn, ImVec2 mx, bool live)
  {
    if (g_pad_mode)
    {
      float cx = (g_pad_focus_mn.x + g_pad_focus_mx.x) * 0.5f, cy = (g_pad_focus_mn.y + g_pad_focus_mx.y) * 0.5f;
      return live && g_pad_focus_valid && cx >= mn.x && cx < mx.x && cy >= mn.y && cy < mx.y;
    }
    return live && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(mn, mx);
  }

  // A button's picture in the footer: a white disc (`round`) or a rounded block with its name; returns its width.
  float PadCap(ImDrawList *dl, ImVec2 pos, float h, const char *label, bool round, float a)
  {
    ImVec2 ts = ui::TextSize(FontKind::kLabel, label);
    float w = round ? h : std::max(h, ts.x + h * 0.7f);
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ui::Fade(ui::kAccentLight, a), round ? h * 0.5f : h * 0.28f);
    ui::Text(dl, FontKind::kLabel, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f - 1.0f * ui::Scale()),
             ui::Fade(ui::kInk, a), label);
    return w;
  }

  // The directional pad's picture; returns its width.
  float PadCross(ImDrawList *dl, ImVec2 pos, float h, float a)
  {
    float arm = h * 0.34f, r = h * 0.09f;
    ImU32 col = ui::Fade(ui::kAccentLight, a);
    dl->AddRectFilled(ImVec2(pos.x + (h - arm) * 0.5f, pos.y), ImVec2(pos.x + (h + arm) * 0.5f, pos.y + h), col, r);
    dl->AddRectFilled(ImVec2(pos.x, pos.y + (h - arm) * 0.5f), ImVec2(pos.x + h, pos.y + (h + arm) * 0.5f), col, r);
    return h;
  }

  float ChipWidth(const char *label)
  {
    return std::max(ui::TextSize(FontKind::kSmall, label).x + 26.0f * ui::Scale(), 58.0f * ui::Scale());
  }

  // A choice: faint glass, the picked one in the accent; `flat`: nothing behind the others, for a
  // segmented row on a track of its own. True when clicked.
  bool Chip(const char *id, ImVec2 pos, ImVec2 size, const char *label, bool on, bool enabled, float a, bool flat = false)
  {
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(pos);
    bool clicked = ImGui::InvisibleButton("##chip", size, ImGuiButtonFlags_EnableNav) && enabled;
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
    float r = size.y * 0.32f;
    float rest = flat ? 0.0f : 0.07f;
    dl->AddRectFilled(pos, mx, ui::Fade(ui::kWhite, ((held ? 0.16f : rest) + 0.08f * h) * (1.0f - o) * a), r);
    dl->AddRectFilled(pos, mx, ui::Fade(ui::Mix(ui::kAccent, ui::kAccentLight, 0.45f * h), o * a * (enabled ? 1.0f : 0.6f)), r);
    // Dark on the accent, also when it cannot be changed (a chosen rule during a match).
    ImU32 text = ui::Mix(enabled ? ui::Mix(ui::kText, ui::kAccentLight, h) : ui::kMuted, ui::kInk, o);
    ImVec2 ts = ui::TextSize(FontKind::kSmall, label);
    dl->PushClipRect(pos, mx, true);
    ui::Text(dl, FontKind::kSmall, ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f - 1.0f * ui::Scale()),
             ui::Fade(text, a), label);
    dl->PopClipRect();
    return clicked;
  }

  // A slider over [lo, hi] at `pos`, `w` wide; true when `value` changed.
  // On the pad left and right move it by `pad_step` (0: a fiftieth of the range).
  bool Slider(const char *id, ImVec2 pos, float w, double &value, double lo, double hi, float a, bool live,
              double pad_step = 0.0)
  {
    float s = ui::Scale();
    float hgt = 28.0f * s;
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##slider", ImVec2(w, hgt), ImGuiButtonFlags_EnableNav);
    bool active = live && !g_pad_mode && ImGui::IsItemActive();
    bool hovered = live && ImGui::IsItemHovered();
    bool focused = live && g_pad_mode && ImGui::IsItemFocused();
    float h = Eased("hover", hovered || active, 20.0f);
    ImGui::PopID();
    if (hovered || active)
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    bool changed = false;
    if (focused)
    {
      g_pad_on_slider = true;
      if (g_pad_step)
      {
        double v = std::clamp(value + double(g_pad_step) * (pad_step > 0.0 ? pad_step : (hi - lo) / 50.0), lo, hi);
        changed = v != value;
        value = v;
      }
    }
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
    float cy = pos.y + hgt * 0.5f, th = 3.0f * s;
    dl->AddRectFilled(ImVec2(pos.x, cy - th), ImVec2(pos.x + w, cy + th), ui::Fade(ui::kWhite, 0.13f * a), th);
    if (t > 0.0f)
    {
      dl->AddRectFilled(ImVec2(pos.x, cy - th), ImVec2(pos.x + std::max(w * t, 2.0f * th), cy + th), ui::Fade(ui::kAccent, a), th);
    }
    ImVec2 knob(pos.x + w * t, cy);
    if (h > 0.01f)
    {
      dl->AddCircleFilled(knob, (10.0f + 6.0f * h) * s, ui::Fade(ui::kAccent, 0.20f * h * a), 32);
    }
    dl->AddCircleFilled(ImVec2(knob.x, knob.y + 1.5f * s), 10.0f * s, ui::Fade(ui::kInk, 0.45f * a), 32);
    dl->AddCircleFilled(knob, 9.0f * s, ui::Fade(ui::kAccentLight, a), 32);
    return changed;
  }

  // An on / off switch, its track at `pos`; `o` is its eased state.
  void Switch(ImDrawList *dl, ImVec2 pos, float o, bool enabled, float a)
  {
    float s = ui::Scale();
    float w = 50.0f * s, h = 26.0f * s, r = h * 0.5f;
    ImVec2 mx(pos.x + w, pos.y + h);
    ImU32 track = ui::Mix(ui::Fade(ui::kWhite, 0.20f), kOnColor, o);
    dl->AddRectFilled(pos, mx, ui::Fade(track, enabled ? a : 0.4f * a), r);
    ImVec2 knob(pos.x + r + (w - h) * o, pos.y + r);
    dl->AddCircleFilled(ImVec2(knob.x, knob.y + 1.0f * s), r - 3.0f * s, ui::Fade(ui::kInk, 0.35f * a), 28);
    dl->AddCircleFilled(knob, r - 3.0f * s, ui::Fade(enabled ? ui::kAccentLight : ui::kMuted, a), 28);
  }

  // The sections, in the bar's order.
  enum Tab
  {
    kTabGraphics,
    kTabControls,
    kTabSound,
    kTabOnline,
    kTabVersus,
    kTabDebug,
    kTabAchievements,
    kTabCount,
  };
  // The Online section's pages (chips over its content).
  enum OnlinePage
  {
    kOnlinePlayers, // the partners: Steam friends, the LAN, a VPN
    kOnlineCode,
    kOnlineServers,
  };
  // The Debug section's pages.
  enum DebugPage
  {
    kPageCheckpoints,
    kPageCheats,
  };
  // A section's icon in the sidebar, drawn in lines about 18 px across
  // around `c`.
  void TabIcon(ImDrawList *dl, int tab, ImVec2 c, ImU32 col)
  {
    float s = ui::Scale();
    float t = 1.6f * s;
    auto P = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };
    switch (tab)
    {
    case kTabGraphics: // sliders
      for (int k = 0; k < 3; ++k)
      {
        float y = -5.0f + 5.0f * float(k), kx = k == 0 ? -3.0f : (k == 1 ? 4.0f : -1.0f);
        dl->AddLine(P(-8, y), P(8, y), col, t);
        dl->AddCircleFilled(P(kx, y), 2.6f * s, col);
      }
      break;
    case kTabOnline: // two players
      dl->AddCircle(P(-4, -4), 3.2f * s, col, 16, t);
      dl->PathArcTo(P(-4, 8), 6.0f * s, 3.14159f, 6.28318f, 12);
      dl->PathStroke(col, ImDrawFlags_None, t);
      dl->AddCircle(P(5, -3), 2.6f * s, col, 16, t);
      dl->PathArcTo(P(5, 8), 4.8f * s, 3.14159f, 6.28318f, 12);
      dl->PathStroke(col, ImDrawFlags_None, t);
      break;
    case kTabControls: // a gamepad
      dl->AddRect(P(-9, -5), P(9, 6), col, 5.0f * s, ImDrawFlags_None, t);
      dl->AddLine(P(-6, 0.5f), P(-2, 0.5f), col, t);
      dl->AddLine(P(-4, -1.5f), P(-4, 2.5f), col, t);
      dl->AddCircleFilled(P(4.5f, 0.5f), 1.5f * s, col);
      break;
    case kTabSound: // a speaker
    {
      ImVec2 pts[6] = {P(-8, -3), P(-4, -3), P(1, -7), P(1, 7), P(-4, 3), P(-8, 3)};
      dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
      dl->PathArcTo(P(1, 0), 4.0f * s, -0.9f, 0.9f, 8);
      dl->PathStroke(col, ImDrawFlags_None, t);
      dl->PathArcTo(P(1, 0), 7.5f * s, -0.9f, 0.9f, 10);
      dl->PathStroke(col, ImDrawFlags_None, t);
      break;
    }
    case kTabVersus: // crossed blades
      dl->AddLine(P(-7, 7), P(7, -7), col, t);
      dl->AddLine(P(7, 7), P(-7, -7), col, t);
      dl->AddLine(P(-7, 2), P(-2, 7), col, t);
      dl->AddLine(P(7, 2), P(2, 7), col, t);
      break;
    case kTabDebug: // a bolt
    {
      ImVec2 pts[6] = {P(2, -9), P(-6, 1), P(-1, 1), P(-3, 9), P(6, -2), P(1, -2)};
      dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
      break;
    }
    default: // achievements: a cup
      dl->PathLineTo(P(-6, -8));
      dl->PathLineTo(P(6, -8));
      dl->PathArcTo(P(0, -3), 6.0f * s, 0.0f, 3.14159f, 12);
      dl->PathStroke(col, ImDrawFlags_Closed, t);
      dl->AddLine(P(0, 3), P(0, 6), col, t);
      dl->AddLine(P(-4, 7.5f), P(4, 7.5f), col, t);
      break;
    }
  }

  constexpr const char *kTabNames[kTabCount] = {"Graphics", "Controls", "Sound",       "Online",
                                                "Versus",   "Debug",    "Achievements"};
  // The bar: the sections in order and their keys.
  struct SideItem
  {
    int tab, key;
  };
  constexpr SideItem kSide[] = {{kTabGraphics, 1}, {kTabControls, 2}, {kTabSound, 3},       {kTabOnline, 4},
                                {kTabVersus, 5},   {kTabDebug, 6}, {kTabAchievements, 7}};
  constexpr const char *kOnlinePageNames[] = {"Players", "Join with code", "Servers"};
  constexpr const char *kPageNames[] = {"Checkpoints", "Cheats"};
  int g_last_tab = kTabGraphics; // the tab the menu opens on (the last one shown)
  int g_last_page = kPageCheckpoints;
  int g_last_online_page = kOnlinePlayers;

  constexpr ImU32 kBadgeGold = IM_COL32(227, 185, 90, 255);
  constexpr ImU32 kPending = IM_COL32(255, 190, 80, 255); // "after restart"

  // The Controls and Sound tabs' rows (fov.cpp, mouse_aim.cpp, audio_settings.cpp):
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
    double reset = 1.0; // what Reset to defaults sets
    double shown = 1.0; // the value's text is the value times this
  };
  double Hundredths(double v)
  {
    return std::round(v * 100.0) / 100.0;
  }
  const SliderRow kMouseSliders[] = {
      {"Sensitivity", 0.1, 10.0, "%.2f", [] { return REXCVAR_GET(ao2_mouse_sensitivity); },
       [](double v) { REXCVAR_SET(ao2_mouse_sensitivity, Hundredths(v)); }, "ao2_mouse_sensitivity", "mapping.toml",
       "mouse_sens", 2.0},
      {"Zoomed sensitivity", 0.1, 3.0, "%.2f", [] { return REXCVAR_GET(ao2_mouse_zoom_sensitivity); },
       [](double v) { REXCVAR_SET(ao2_mouse_zoom_sensitivity, Hundredths(v)); }, "ao2_mouse_zoom_sensitivity",
       "mapping.toml", "mouse_zoom"},
      {"Mouse in vehicles and turrets", 0.25, 3.0, "%.2f", [] { return REXCVAR_GET(ao2_mouse_stick_sensitivity); },
       [](double v) { REXCVAR_SET(ao2_mouse_stick_sensitivity, Hundredths(v)); }, "ao2_mouse_stick_sensitivity",
       "mapping.toml", "stick_sens"},
  };
  const SliderRow kCameraSliders[] = {
      {"Field of view", 40.0, 130.0, "%.0f\xC2\xB0", [] { return REXCVAR_GET(ao2_fov); },
       [](double v) { REXCVAR_SET(ao2_fov, std::round(v)); }, "ao2_fov", "hardware.toml", "fov", 70.0},
      {"Aiming zoom", 0.0, 1.0, "%.2f", [] { return REXCVAR_GET(ao2_fov_aim); },
       [](double v) { REXCVAR_SET(ao2_fov_aim, Hundredths(v)); }, "ao2_fov_aim", "hardware.toml", "fov_aim"},
      {"Camera shake", 0.0, 1.0, "%.2f", [] { return REXCVAR_GET(ao2_camera_shake); },
       [](double v) { REXCVAR_SET(ao2_camera_shake, Hundredths(v)); }, "ao2_camera_shake", "hardware.toml",
       "camera_shake"},
  };
  const SliderRow kPadSliders[] = {
      {"Vibration", 0.0, 1.0, "%.0f%%", [] { return REXCVAR_GET(ao2_pad_vibration); },
       [](double v) { REXCVAR_SET(ao2_pad_vibration, Hundredths(v)); }, "ao2_pad_vibration", "mapping.toml", nullptr,
       1.0, 100.0},
  };
  const SliderRow kSoundSliders[] = {
      {"Master volume", 0.0, 1.0, "%.0f%%", [] { return REXCVAR_GET(ao2_volume_master); },
       [](double v) { REXCVAR_SET(ao2_volume_master, Hundredths(v)); }, "ao2_volume_master", "hardware.toml", nullptr,
       1.0, 100.0},
      {"Music", 0.0, 1.0, "%.0f%%", [] { return REXCVAR_GET(ao2_volume_music); },
       [](double v) { REXCVAR_SET(ao2_volume_music, Hundredths(v)); }, "ao2_volume_music", "hardware.toml", nullptr,
       1.0, 100.0},
      {"Sound effects", 0.0, 1.0, "%.0f%%", [] { return REXCVAR_GET(ao2_volume_effects); },
       [](double v) { REXCVAR_SET(ao2_volume_effects, Hundredths(v)); }, "ao2_volume_effects", "hardware.toml", nullptr,
       1.0, 100.0},
      {"Voices", 0.0, 1.0, "%.0f%%", [] { return REXCVAR_GET(ao2_volume_voices); },
       [](double v) { REXCVAR_SET(ao2_volume_voices, Hundredths(v)); }, "ao2_volume_voices", "hardware.toml", nullptr,
       1.0, 100.0},
      {"Achievement sound", 0.0, 1.0, "%.0f%%", [] { return REXCVAR_GET(ao2_achievement_sound); },
       [](double v) { REXCVAR_SET(ao2_achievement_sound, Hundredths(v)); }, "ao2_achievement_sound", "hardware.toml",
       nullptr, 0.4, 100.0},
  };

  // The Controls tab's switches; off is the default of them all.
  struct ToggleRow
  {
    const char *label;
    bool (*get)();
    void (*set)(bool);
    const char *cvar;
    const char *launcher_key;
  };
  const ToggleRow kAimToggles[] = {
      {"Invert Y axis", [] { return REXCVAR_GET(ao2_mouse_invert_y); }, [](bool on) { REXCVAR_SET(ao2_mouse_invert_y, on); },
       "ao2_mouse_invert_y", "invert_y"},
      {"Aim assist", [] { return REXCVAR_GET(ao2_aim_assist); }, [](bool on) { REXCVAR_SET(ao2_aim_assist, on); },
       "ao2_aim_assist", "aim_assist"},
      {"Toggle aim", [] { return REXCVAR_GET(ao2_toggle_aim); }, [](bool on) { REXCVAR_SET(ao2_toggle_aim, on); },
       "ao2_toggle_aim", "toggle_aim"},
  };

  void SaveToggle(const ToggleRow &row)
  {
    ao2::gfxmenu::SaveValue(row.cvar, row.get() ? "true" : "false", "mapping.toml", row.launcher_key);
  }

  // What Reset effects sets: the launcher's defaults.
  const std::pair<const char *, const char *> kEffectDefaults[] = {
      {"Anisotropic filtering", "Off"}, {"Anti-aliasing", "Off"}, {"Ambient occlusion", "Off"},
      {"Motion blur", "Off"},           {"Depth of field", "Off"}, {"Shadow resolution", "Auto"},
  };

  int SettingOf(const char *label)
  {
    for (int i = 0; i < ao2::gfxmenu::SettingCount(); ++i)
    {
      if (std::strcmp(ao2::gfxmenu::SettingLabel(i), label) == 0)
      {
        return i;
      }
    }
    return -1;
  }

  // Picks the choice called `name` of the setting, unless it is the chosen one.
  void PickNamed(const char *label, const char *name)
  {
    int setting = SettingOf(label);
    for (int c = 0; setting >= 0 && c < ao2::gfxmenu::ChoiceCount(setting); ++c)
    {
      if (std::strcmp(ao2::gfxmenu::ChoiceName(setting, c), name) == 0 && ao2::gfxmenu::Chosen(setting) != c)
      {
        ao2::gfxmenu::Pick(setting, c);
      }
    }
  }

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
       {{0, "Lobby's"}},
       1, 1, 120, "min"},
      {"Cash goal", "ao2_versus_cash_goal", [] { return REXCVAR_GET(ao2_versus_cash_goal); },
       [](int32_t v) { REXCVAR_SET(ao2_versus_cash_goal, v); },
       {{0, "Off"}, {500000, "$500K"}, {1000000, "$1M"}, {2000000, "$2M"}, {5000000, "$5M"}},
       1000, 10, 100000, "$K"},
      {"Friendly fire", "ao2_versus_friendly_fire", [] { return REXCVAR_GET(ao2_versus_friendly_fire); },
       [](int32_t v) { REXCVAR_SET(ao2_versus_friendly_fire, v); }, {{1, "Off"}, {0, "Half"}, {2, "Full"}}},
      {"Respawn price", "ao2_versus_respawn_cost", [] { return REXCVAR_GET(ao2_versus_respawn_cost); },
       [](int32_t v) { REXCVAR_SET(ao2_versus_respawn_cost, v); },
       {{5000, "$5K"}, {0, "$10K"}, {20000, "$20K"}, {50000, "$50K"}},
       1000, 1, 10000, "$K"},
      {"Respawn price increase", "ao2_versus_respawn_step", [] { return REXCVAR_GET(ao2_versus_respawn_step); },
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
  };
  constexpr int kBotReactionRow = 1;

  // What a Graphics tab row does (English, through Tr), by its label.
  const char *RowHelp(const char *label)
  {
    static const std::map<std::string, const char *> kHelp = {
        {"Display mode", "Fullscreen or a window."},
        {"Render resolution", "Renders the game's 1280x720 picture at this resolution. Applies immediately."},
        {"Aspect ratio", "The picture's aspect ratio. Requires a game restart."},
        {"FPS limit", "The maximum frame rate."},
        {"Vertical sync", "Syncs frames to the monitor: no tearing, slightly more input lag."},
        {"FPS counter", "Shows the frame rate in the corner; Detailed adds GPU, CPU and memory stats."},
        {"Anisotropic filtering", "Sharper textures on surfaces seen at an angle."},
        {"Anti-aliasing", "Smooths jagged edges. SMAA is sharper, FXAA cheaper."},
        {"Ambient occlusion", "Soft shadows in corners and under objects. GTAO looks best, SSAO costs least."},
        {"Motion blur", "Blur from camera movement."},
        {"Depth of field", "Blurs objects that are out of focus."},
        {"Shadow resolution", "Sharper shadow edges at a GPU cost. Auto uses the game's default."},
        {"Field of view", "Field of view on foot. The game's default is 70\xC2\xB0."},
        {"Aiming zoom", "How much aiming zooms in: 1 is the game's default, 0 is no zoom. Does not affect the sniper scope."},
        {"Camera shake", "Camera shake strength: 1 is the game's default, 0 turns it off. Bullet spread is unchanged."},
        {"Sensitivity", "How fast the mouse turns the camera."},
        {"Zoomed sensitivity", "Mouse speed while aiming, relative to the normal speed."},
        {"Mouse in vehicles and turrets", "Mouse speed where it acts as the right stick."},
        {"Invert Y axis", "Moving the mouse up looks down."},
        {"Aim assist", "When you aim, the camera snaps to the nearest target and shots are pulled toward targets."},
        {"Toggle aim", "Press the aim button once to start aiming and again to stop."},
        {"Vibration", "How strongly the gamepad vibrates. 0 turns it off."},
        {"Menu scale", "Size of this menu. Applies when you release the slider."},
        {"Master volume", "All game audio, including movies."},
        {"Music", "Music, including during missions."},
        {"Sound effects", "Weapons, explosions, footsteps and the world around you."},
        {"Voices", "Character dialogue, in gameplay and cutscenes."},
        {"Achievement sound", "The sound of achievement notices."},
        // Versus
        {"Round length", "How long a round lasts. Lobby's: the length picked in the game's own lobby."},
        {"Cash goal", "The first team to have this much cash wins the match at once. Off: only the rounds count."},
        {"Friendly fire", "Damage to your own team: Off, Half (the game's default) or Full."},
        {"Respawn price", "What a player's first respawn costs the team."},
        {"Respawn price increase", "How much dearer each later respawn of the same player gets."},
        {"Aim error", "How far the bots' shots stray from the target. More is easier."},
        {"Reaction time", "How long a bot takes to react to an enemy it sees. More is easier."},
        {"Turn speed", "How fast the bots turn to aim. More is harder."},
        {"Firing range", "How far away the bots start to shoot."},
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
  // the achievements and players windows: a panel with a title, a bar of
  // sections (a click, 1-7, Tab / Shift+Tab),
  // the section's content and a footer; the content slides in. Esc, F1 and
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
      bool invite = ao2::invite::HasPendingInvite();
      // Versus' Game Setup: its F1 hint (versus_lobby.cpp SetupHint) leads to the match rules.
      tab_ = invite ? kTabOnline : ao2::menumouse::VersusMenu() == 1 ? int(kTabVersus) : g_last_tab;
      page_ = g_last_page;
      online_page_ = invite ? int(kOnlinePlayers) : g_last_online_page;
      g_pad_mode = g_open_by_pad;
      pad_focus_first_ = g_pad_mode;
    }

    // Any time on the UI thread (the menu key): plays the menu out.
    void RequestClose() { close_requested_ = true; }

  protected:
    void OnDraw(ImGuiIO &io) override
    {
      ui::ZoomScope zoom(float(std::clamp(REXCVAR_GET(ao2_menu_scale), 1.0, 1.6)));
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
      PadInput(io, now, live);

      const uint8_t *base = GuestBase();
      network_ = base && ao2::coop::cmd::Routed(base);
      ready_ = base && ao2::coop::cmd::Ready(base);

      const ImGuiViewport *vp = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(vp->Pos);
      ImGui::SetNextWindowSize(vp->Size);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      // ImGui's square cursor stays off: DrawPadFocus draws the focus.
      ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
      ImGui::Begin("##debug_menu", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::PopStyleVar(2);
      if (live && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      {
        BeginClose();
      }
      ImDrawList *dl = ImGui::GetWindowDrawList();

      // The runtime passes Ctrl and Shift as keys but not as modifiers, so
      // text fields saw no Ctrl+V / Ctrl+C / Shift+arrows: given here (they
      // count from the next frame).
      {
        ImGuiIO &io = ImGui::GetIO();
        bool ctrl = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
        bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
        if (ctrl != ImGui::IsKeyDown(ImGuiMod_Ctrl))
        {
          io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        }
        if (shift != ImGui::IsKeyDown(ImGuiMod_Shift))
        {
          io.AddKeyEvent(ImGuiMod_Shift, shift);
        }
      }

      // --- Backdrop: the game darkened behind ---
      {
        ImVec2 p0 = vp->Pos, p1(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
        float band = vp->Size.y * 0.32f;
        dl->AddRectFilled(p0, p1, ui::Fade(ui::kInk, 0.58f * a));
        ImU32 edge = ui::Fade(ui::kInk, 0.34f * a), none = ui::Fade(ui::kInk, 0.0f);
        dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, p0.y + band), edge, edge, none, none);
        dl->AddRectFilledMultiColor(ImVec2(p0.x, p1.y - band), p1, none, none, edge, edge);
      }

      // From the top: the sections' bar, the sheet, the keys.
      const float pad = 32.0f * s, gap = 14.0f * s;
      const float bar_h = 58.0f * s, keys_h = 28.0f * s;
      const float pw = std::min(1480.0f * s, vp->Size.x - 80.0f * s);
      const float ph = std::min(880.0f * s, vp->Size.y - 48.0f * s - bar_h - keys_h - 2.0f * gap);
      ImVec2 center = vp->GetCenter();
      float rest_top = center.y - (bar_h + gap + ph + gap + keys_h) * 0.5f;
      float top = rest_top - (1.0f - a) * 16.0f * s;
      float x0 = center.x - pw * 0.5f, y0 = rest_top + bar_h + gap + (1.0f - a) * 30.0f * s;
      float x1 = x0 + pw, y1 = y0 + ph;
      float cx0 = x0 + pad, cx1 = x1 - pad;
      float r = 24.0f * s;
      for (int k = 1; k <= 12; ++k)
      {
        float o = float(k) * 2.0f * s;
        dl->AddRect(ImVec2(x0 - o, y0 - o + 6.0f * s), ImVec2(x1 + o, y1 + o + 6.0f * s),
                    ui::Fade(ui::kInk, 0.05f * (1.0f - float(k) / 13.0f) * a), r + o, ImDrawFlags_None, 2.0f * s);
      }
      dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ui::Fade(ui::kPanel, 0.95f * a), r);
      // The sheet's sheen: thin layers, rounded as the sheet, so it has no edges of its own.
      constexpr int kSheenLayers = 24;
      for (int k = 1; k <= kSheenLayers; ++k)
      {
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + r + (150.0f * s - r) * float(k) / float(kSheenLayers)),
                          ui::Fade(ui::kWhite, 0.035f * a / float(kSheenLayers)), r, ImDrawFlags_RoundCornersTop);
      }
      dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), ui::Fade(ui::kWhite, 0.12f * a), r, ImDrawFlags_None, 1.0f * s);

      // --- The sections: keys 1-7, Tab through all of them ---
      if (live && !io.WantTextInput)
      {
        for (const SideItem &it : kSide)
        {
          if (it.key && (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_0 + it.key), false) ||
                         ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_Keypad0 + it.key), false)))
          {
            SetTab(it.tab, now);
          }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Tab, false))
        {
          // The next (Shift: previous).
          StepTab(io.KeyShift ? -1 : 1, now);
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
      {
        struct Pill
        {
          int tab;
          const char *label;
          std::string badge;
          ImU32 badge_col;
          bool dot;
          float w;
        };
        std::vector<Pill> pills;
        const float side_pad = 16.0f * s, icon_w = 28.0f * s, item_gap = 2.0f * s, inner = 5.0f * s;
        const float item_h = bar_h - 2.0f * inner;
        float total = 0.0f;
        for (const SideItem &it : kSide)
        {
          Pill pill{it.tab, Tr(kTabNames[it.tab]), {}, ui::kAccent, false, 0.0f};
          if (it.tab == kTabOnline)
          {
            pill.dot = ao2::invite::HasPendingInvite();
          }
          else if (it.tab == kTabDebug && cheats_on > 0)
          {
            pill.badge = std::to_string(cheats_on);
            pill.badge_col = ui::kGreen;
          }
          else if (it.tab == kTabAchievements && ach_total_ > 0)
          {
            pill.badge = std::to_string(ach_got_) + "/" + std::to_string(ach_total_);
            pill.badge_col = kBadgeGold;
          }
          pill.w = 2.0f * side_pad + icon_w + ui::TextSize(FontKind::kBody, pill.label).x;
          if (!pill.badge.empty())
          {
            pill.w += ui::TextSize(FontKind::kSmall, pill.badge.c_str()).x + 10.0f * s;
          }
          total += pill.w + item_gap;
          pills.push_back(std::move(pill));
        }
        total -= item_gap;
        // A narrow screen or long words: the sections share what there is.
        float fit = std::min(1.0f, (pw - 2.0f * inner - item_gap * float(pills.size() - 1)) / (total - item_gap * float(pills.size() - 1)));
        float bar_w = std::min(pw, total + 2.0f * inner);
        float bx0 = center.x - bar_w * 0.5f;
        dl->AddRectFilled(ImVec2(bx0, top), ImVec2(bx0 + bar_w, top + bar_h), ui::Fade(ui::kPanel, 0.92f * a), bar_h * 0.5f);
        dl->AddRect(ImVec2(bx0, top), ImVec2(bx0 + bar_w, top + bar_h), ui::Fade(ui::kWhite, 0.14f * a), bar_h * 0.5f,
                    ImDrawFlags_None, 1.0f * s);

        // The current one's block slides to it, under the others' text.
        float px = bx0 + inner;
        for (const Pill &pill : pills)
        {
          if (pill.tab == tab_)
          {
            mark_target_ = px;
            mark_width_target_ = pill.w * fit;
          }
          px += pill.w * fit + item_gap;
        }
        mark_x_ = mark_x_ < 0.0f ? mark_target_ : ui::Approach(mark_x_, mark_target_, 18.0f);
        mark_w_ = mark_w_ < 0.0f ? mark_width_target_ : ui::Approach(mark_w_, mark_width_target_, 18.0f);
        dl->AddRectFilled(ImVec2(mark_x_ - 2.0f * s, top + inner - 2.0f * s),
                          ImVec2(mark_x_ + mark_w_ + 2.0f * s, top + inner + item_h + 2.0f * s), ui::Fade(ui::kAccent, 0.20f * a),
                          item_h * 0.5f + 2.0f * s);
        dl->AddRectFilled(ImVec2(mark_x_, top + inner), ImVec2(mark_x_ + mark_w_, top + inner + item_h), ui::Fade(ui::kAccent, a),
                          item_h * 0.5f);

        px = bx0 + inner;
        float mid = top + bar_h * 0.5f;
        for (const Pill &pill : pills)
        {
          float w = pill.w * fit;
          ImVec2 mn(px, top + inner), mx(px + w, top + inner + item_h);
          ImGui::PushID(pill.tab);
          ImGui::SetCursorScreenPos(mn);
          // No navigation here: on the pad the sections are LB and RB.
          if (ImGui::InvisibleButton("##tab", ImVec2(w, item_h)) && live)
          {
            SetTab(pill.tab, now);
          }
          bool hovered = live && ImGui::IsItemHovered();
          float h = Eased("tab_hover", hovered, 18.0f);
          ImGui::PopID();
          if (hovered)
          {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
          }
          bool active = pill.tab == tab_;
          if (h > 0.01f && !active)
          {
            dl->AddRectFilled(mn, mx, ui::Fade(ui::kWhite, 0.10f * h * a), item_h * 0.5f);
          }
          ImU32 col = active ? ui::kInk : ui::kAccentLight;
          dl->PushClipRect(mn, mx, true);
          TabIcon(dl, pill.tab, ImVec2(px + side_pad + 10.0f * s, mid), ui::Fade(col, a));
          float lx = px + side_pad + icon_w;
          ui::Text(dl, FontKind::kBody, ImVec2(lx, mid - ui::FontSize(FontKind::kBody) * 0.5f - 1.0f * s), ui::Fade(col, a),
                   pill.label);
          float bx = lx + ui::TextSize(FontKind::kBody, pill.label).x + 8.0f * s;
          if (!pill.badge.empty())
          {
            ui::Text(dl, FontKind::kSmall, ImVec2(bx, mid - ui::FontSize(FontKind::kSmall) * 0.5f - 1.0f * s),
                     ui::Fade(active ? ui::kInk : pill.badge_col, a), pill.badge.c_str());
          }
          dl->PopClipRect();
          if (pill.dot)
          {
            float pulse = 0.5f + 0.5f * std::sin(float(now) * 4.0f);
            ImVec2 dc(mx.x - 12.0f * s, top + inner + 12.0f * s);
            ImU32 dcol = active ? ui::kInk : ui::kAccent;
            dl->AddCircleFilled(dc, 3.5f * s, ui::Fade(dcol, a));
            dl->AddCircle(dc, (3.5f + 4.0f * pulse) * s, ui::Fade(dcol, (1.0f - pulse) * a), 16, 1.0f * s);
          }
          px += w + item_gap;
        }
      }

      // --- Under the sheet: the keys ---
      const float kh = 26.0f * s;
      const float footer_y = y1 + gap + (keys_h - kh) * 0.5f;
      {
        float fx = cx0;
        float label_y = footer_y + (kh - ui::FontSize(FontKind::kLabel)) * 0.5f - 1.0f * s;
        // The prompts follow the hands: the pad's buttons from its first press on.
        auto legend = [&](const char *text) {
          ui::Caps(dl, FontKind::kLabel, ImVec2(fx + 2.0f * s, label_y), ui::Fade(ui::kText, a), text);
          fx += 2.0f * s + ui::CapsSize(FontKind::kLabel, text).x + 26.0f * s;
        };
        if (g_pad_mode)
        {
          for (const char *key : {"LB", "RB"})
          {
            fx += PadCap(dl, ImVec2(fx, footer_y), kh, key, false, a) + 6.0f * s;
          }
          legend(Tr("Sections"));
          fx += PadCross(dl, ImVec2(fx, footer_y), kh, a) + 6.0f * s;
          legend(Tr("Move"));
          fx += PadCap(dl, ImVec2(fx, footer_y), kh, "A", true, a) + 6.0f * s;
          legend(Tr("Select"));
        }
        else
        {
          for (const char *key : {"1-5", "TAB"})
          {
            fx += ui::KeyCap(dl, ImVec2(fx, footer_y), kh, key, a) + 6.0f * s;
          }
          legend(Tr("Sections"));
        }

        const char *label = Tr("Close");
        float label_w = ui::CapsSize(FontKind::kLabel, label).x;
        float key_w = g_pad_mode ? kh : std::max(kh, ui::CapsSize(FontKind::kLabel, "ESC").x + kh * 0.8f); // as ui::KeyCap
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
        if (g_pad_mode)
        {
          PadCap(dl, pos, kh, "B", true, a);
        }
        else
        {
          ui::KeyCap(dl, pos, kh, "ESC", a);
        }
        ui::Caps(dl, FontKind::kLabel, ImVec2(pos.x + key_w + 8.0f * s, label_y),
                 ui::Fade(hovered ? ui::kAccentLight : ui::kText, a), label);
      }

      // --- The sheet: the section's name, then its content, sliding in from
      // the side it was picked on ---
      float t = ui::EaseOutCubic(float(now - tab_changed_) / 0.3f);
      float ca = a * ui::Clamp01(float(now - tab_changed_) / 0.18f);
      content_alpha_ = ca;
      float shift = (1.0f - t) * 24.0f * s * float(tab_dir_);
      float content_x = cx0;
      float y = y0 + 24.0f * s;
      ui::Text(dl, FontKind::kTitle, ImVec2(content_x, y + shift * 0.5f), ui::Fade(ui::kAccentLight, ca), Tr(kTabNames[tab_]));
      y += ui::FontSize(FontKind::kTitle) + 22.0f * s;
      float content_y1 = y1 - 20.0f * s;
      ImVec2 cmn(content_x, y + shift), cmx(cx1, content_y1 + shift);
      dl->PushClipRect(ImVec2(content_x - 4.0f * s, y - 4.0f * s), ImVec2(x1 - 1.0f, content_y1 + 4.0f * s), true);
      switch (tab_)
      {
      case kTabGraphics:
        tip_ = nullptr;
        Scrolling("graphics", cmn, cmx, false, [&](ImVec2 o, float w) { return DrawGraphics(o, w, ca, live); });
        break;
      case kTabControls:
        tip_ = nullptr;
        Scrolling("controls", cmn, cmx, false, [&](ImVec2 o, float w) { return DrawControls(o, w, ca, live); });
        break;
      case kTabSound:
        tip_ = nullptr;
        Scrolling("sound", cmn, cmx, false, [&](ImVec2 o, float w) { return DrawSound(o, w, ca, live); });
        break;
      case kTabOnline:
        DrawOnline(cmn, cmx, ca, live);
        break;
      case kTabVersus:
        tip_ = nullptr;
        Scrolling("versus", cmn, cmx, false, [&](ImVec2 o, float w) { return DrawVersus(o, w, ca, live); });
        break;
      case kTabDebug:
        DrawDebug(cmn, cmx, ca, live);
        break;
      case kTabAchievements:
        ao2::ach::DrawTab(cmn, cmx, fresh_);
        break;
      }
      dl->PopClipRect();
      fresh_ = false;

      // Over the content, the tip over both.
      DrawPadFocus(ImVec2(content_x - 4.0f * s, y - 4.0f * s), ImVec2(x1 - 1.0f, content_y1 + 4.0f * s), ca, live);
      if (tab_ == kTabGraphics || tab_ == kTabControls || tab_ == kTabSound || tab_ == kTabVersus)
      {
        DrawTip(ca);
      }

      ImGui::End();
      ImGui::PopStyleColor();
    }

    void OnClose() override
    {
      g_last_tab = tab_;
      g_last_page = page_;
      g_last_online_page = online_page_;
      // The drawer's other windows navigate no more, the pad's keys are up.
      ImGuiIO &io = GetIO();
      io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
      io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
      for (int i = 0; i < 5; ++i)
      {
        io.AddKeyEvent(kPadKeys[i], false);
      }
      // What closed it (B, Select + Start) is still down.
      if (g_pad_mode)
      {
        for (auto &swallow : g_pad_swallow)
        {
          swallow = 2;
        }
      }
      g_pad_mode = false;
      g_pad_focus_valid = false;
      g_pad_step = 0;
      g_pad_lr_held = false;
      g_menu = nullptr;
      g_open = false;
    }

  private:
    // The gamepad (PadInput).
    bool pad_first_read_ = true;
    uint16_t pad_ignored_ = 0, pad_prev_ = 0;
    bool pad_keys_[5] = {};
    bool pad_focus_first_ = false; // the next scrolling list's first item takes the focus
    int pad_lr_ = 0;
    double pad_lr_next_ = 0.0;
    float pad_scroll_ = 0.0f;
    bool pad_ring_on_ = false;
    ImVec2 pad_ring_mn_, pad_ring_mx_;

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
      pad_focus_first_ = g_pad_mode;
    }

    // The section `dir` (1, -1) from this one, round the end.
    void StepTab(int dir, double now)
    {
      int n = int(std::size(kSide)), at = 0;
      for (int i = 0; i < n; ++i)
      {
        at = kSide[i].tab == tab_ ? i : at;
      }
      SetTab(kSide[(at + n + dir) % n].tab, now);
    }

    // Reads the pad, whatever the game gets, and acts on it. Directions and A go to ImGui's
    // navigation, which takes them from the next frame.
    void PadInput(ImGuiIO &io, double now, bool live)
    {
      io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
      io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
      PadRead pad;
      bool read = g_window->HasFocus() && g_modal.load(std::memory_order_relaxed) == 0 && ReadPads(pad);
      uint16_t b = read ? pad.buttons : 0;
      // Down when the menu opened (Select + Start): nothing until let go.
      if (pad_first_read_)
      {
        pad_first_read_ = false;
        pad_ignored_ = b;
      }
      pad_ignored_ &= b;
      b &= uint16_t(~pad_ignored_);
      uint16_t pressed = b & uint16_t(~pad_prev_);
      pad_prev_ = b;

      constexpr int kLean = 16000; // of 32767: a stick counts as a direction
      constexpr int kDead = 8000;
      bool up = (b & 0x0001) || pad.thumbs[1] > kLean, down = (b & 0x0002) || pad.thumbs[1] < -kLean;
      bool left = (b & 0x0004) || pad.thumbs[0] < -kLean, right = (b & 0x0008) || pad.thumbs[0] > kLean;
      float rs = std::abs(pad.thumbs[3]) > kDead ? float(pad.thumbs[3]) / 32767.0f : 0.0f;
      bool used = b != 0 || up || down || left || right || rs != 0.0f;

      // Whichever was used last has the highlight and the prompts.
      ImGuiContext &g = *ImGui::GetCurrentContext();
      if (used && !g_pad_mode)
      {
        g_pad_mode = true;
        pad_focus_first_ = true;
      }
      else if (g_pad_mode && !used)
      {
        bool others = std::abs(io.MouseDelta.x) + std::abs(io.MouseDelta.y) > 1.0f || ImGui::IsAnyMouseDown();
        for (int k = ImGuiKey_NamedKey_BEGIN; !others && k < ImGuiKey_GamepadStart; ++k)
        {
          others = ImGui::IsKeyPressed(ImGuiKey(k), false);
        }
        if (others)
        {
          g_pad_mode = false;
          g.NavCursorVisible = false;
          g.NavHighlightItemUnderNav = false;
        }
      }
      if (g_pad_mode)
      {
        g.NavCursorVisible = true;
        g.NavHighlightItemUnderNav = true;
      }

      // A slider's left and right are its own (the first move at once, held they repeat).
      bool on_slider = g_pad_on_slider;
      g_pad_on_slider = false;
      int lr = left == right ? 0 : left ? -1 : 1;
      g_pad_step = 0;
      if (lr != pad_lr_)
      {
        pad_lr_ = lr;
        pad_lr_next_ = now + 0.35;
        g_pad_step = lr;
      }
      else if (lr && now >= pad_lr_next_)
      {
        pad_lr_next_ = now + 0.05;
        g_pad_step = lr;
      }
      g_pad_lr_held = lr != 0;

      const bool down_now[5] = {up, down, left && !on_slider, right && !on_slider, (b & 0x1000) != 0};
      for (int i = 0; i < 5; ++i)
      {
        bool on = live && down_now[i];
        if (on != pad_keys_[i])
        {
          pad_keys_[i] = on;
          io.AddKeyEvent(kPadKeys[i], on);
        }
      }

      float s = ui::Scale();
      pad_scroll_ = -rs * 1100.0f * s * io.DeltaTime;
      if (tab_ == kTabAchievements)
      {
        // A list without items to focus: the directions move it as well.
        ao2::ach::ScrollBy(pad_scroll_ + (down ? 1.0f : up ? -1.0f : 0.0f) * 900.0f * s * io.DeltaTime);
      }
      if (!live)
      {
        return;
      }
      if (pressed & 0x0100)
      {
        StepTab(-1, now);
      }
      if (pressed & 0x0200)
      {
        StepTab(1, now);
      }
      if ((pressed & 0x2000) || ((b & 0x0030) == 0x0030 && (pressed & 0x0030)))
      {
        BeginClose();
      }
    }

    // A frame that glides to the focused item; remembers that item for Hovering and the tip.
    void DrawPadFocus(ImVec2 clip_mn, ImVec2 clip_mx, float a, bool live)
    {
      ImGuiContext &g = *ImGui::GetCurrentContext();
      g_pad_focus_valid = g_pad_mode && g.NavWindow && g.NavId && g.NavIdIsAlive;
      if (!g_pad_focus_valid)
      {
        // The item went (a list changed), or nothing was ever focused here.
        pad_focus_first_ = g_pad_mode && live;
        pad_ring_on_ = false;
        return;
      }
      ImRect r = ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[ImGuiNavLayer_Main]);
      g_pad_focus_mn = r.Min;
      g_pad_focus_mx = r.Max;
      float s = ui::Scale();
      ImVec2 mn(r.Min.x - 3.0f * s, r.Min.y - 3.0f * s), mx(r.Max.x + 3.0f * s, r.Max.y + 3.0f * s);
      if (!pad_ring_on_)
      {
        pad_ring_on_ = true;
        pad_ring_mn_ = mn;
        pad_ring_mx_ = mx;
      }
      pad_ring_mn_ = ImVec2(ui::Approach(pad_ring_mn_.x, mn.x, 30.0f), ui::Approach(pad_ring_mn_.y, mn.y, 30.0f));
      pad_ring_mx_ = ImVec2(ui::Approach(pad_ring_mx_.x, mx.x, 30.0f), ui::Approach(pad_ring_mx_.y, mx.y, 30.0f));
      ImDrawList *fg = ImGui::GetForegroundDrawList();
      float rounding = std::min((pad_ring_mx_.y - pad_ring_mn_.y) * 0.36f, 16.0f * s);
      fg->PushClipRect(clip_mn, clip_mx, true);
      fg->AddRectFilled(pad_ring_mn_, pad_ring_mx_, ui::Fade(ui::kAccentLight, 0.08f * a), rounding);
      fg->AddRect(pad_ring_mn_, pad_ring_mx_, ui::Fade(ui::kAccentLight, a), rounding, ImDrawFlags_None, 2.0f * s);
      fg->PopClipRect();
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
      ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 3.0f * s);
      ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ui::Fade(ui::kWhite, 0.04f));
      ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ui::Fade(ui::kAccent, 0.45f));
      ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ui::Fade(ui::kAccent, 0.7f));
      ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ui::kAccent);
      if (ImGui::BeginChild(id, ImVec2(mx.x - mn.x, mx.y - mn.y), ImGuiChildFlags_NavFlattened,
                            ImGuiWindowFlags_NoBackground))
      {
        if (reset)
        {
          ImGui::SetScrollY(0.0f);
        }
        if (pad_focus_first_)
        {
          // SetKeyboardFocusHere without its activating: a text field would take the pad's directions.
          pad_focus_first_ = false;
          ImGuiContext &g = *ImGui::GetCurrentContext();
          ImGui::SetNavWindow(g.CurrentWindow);
          ImGui::NavMoveRequestSubmit(ImGuiDir_None, ImGuiDir_Down,
                                      ImGuiNavMoveFlags_IsTabbing | ImGuiNavMoveFlags_FocusApi |
                                          ImGuiNavMoveFlags_NoSetNavCursorVisible,
                                      ImGuiScrollFlags_KeepVisibleEdgeX | ImGuiScrollFlags_KeepVisibleEdgeY);
          g.NavTabbingDir = 1;
          g.NavTabbingCounter = 1;
        }
        if (pad_scroll_ != 0.0f && ImGui::IsWindowFocused())
        {
          ImGui::SetScrollY(ImGui::GetScrollY() + pad_scroll_);
        }
        ImVec2 o = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x - 14.0f * s;
        float bottom = draw(o, w);
        ImGui::SetCursorScreenPos(ImVec2(o.x, bottom));
        ImGui::Dummy(ImVec2(1.0f, 4.0f * s));
        // What is scrolled past fades into the sheet instead of being cut.
        const float fade = 28.0f * s;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float wx0 = ImGui::GetWindowPos().x, wx1 = wx0 + w, wy0 = ImGui::GetWindowPos().y, wy1 = wy0 + ImGui::GetWindowHeight();
        float above = ui::Clamp01(ImGui::GetScrollY() / fade), below = ui::Clamp01((ImGui::GetScrollMaxY() - ImGui::GetScrollY()) / fade);
        ImU32 clear = ui::Fade(ui::kPanel, 0.0f);
        if (above > 0.0f)
        {
          ImU32 top = ui::Fade(ui::kPanel, content_alpha_ * above);
          dl->AddRectFilledMultiColor(ImVec2(wx0, wy0), ImVec2(wx1, wy0 + fade), top, top, clear, clear);
        }
        if (below > 0.0f)
        {
          ImU32 low = ui::Fade(ui::kPanel, content_alpha_ * below);
          dl->AddRectFilledMultiColor(ImVec2(wx0, wy1 - fade), ImVec2(wx1, wy1), clear, clear, low, low);
        }
      }
      ImGui::EndChild();
      ImGui::PopStyleColor(4);
      ImGui::PopStyleVar(3);
    }

    // --- The settings tabs' rows.

    static float RowHeight()
    {
      return 60.0f * ui::Scale();
    }
    static float RowLabelWidth()
    {
      return 290.0f * ui::Scale();
    }

    // The rows of a section share one rounded back, with hairlines between them.
    void RowGroup(float x0, float x1, float y, int rows, float a)
    {
      float s = ui::Scale(), row_h = RowHeight();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      ImVec2 mn(x0, y), mx(x1, y + row_h * float(rows));
      float gr = 16.0f * s;
      dl->AddRectFilled(mn, mx, ui::Fade(ui::kWhite, 0.035f * a), gr);
      dl->AddRect(mn, mx, ui::Fade(ui::kWhite, 0.07f * a), gr, ImDrawFlags_None, 1.0f * s);
      for (int i = 1; i < rows; ++i)
      {
        float ly = y + row_h * float(i);
        dl->AddLine(ImVec2(x0 + 20.0f * s, ly), ImVec2(x1 - 20.0f * s, ly), ui::Fade(ui::kWhite, 0.07f * a), 1.0f * s);
      }
    }

    // A row's back, label and note; the hovered one's help (RowHelp of `help`) shows as a tip.
    void RowBase(float x0, float x1, float y, const char *id, const char *help, const char *label, const char *note,
                 float a, bool live)
    {
      float s = ui::Scale(), row_h = RowHeight();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      ImVec2 mn(x0, y), mx(x1, y + row_h);
      bool hovered = Hovering(mn, mx, live);
      if (hovered)
      {
        tip_ = help;
      }
      float h = Eased(id, hovered);
      if (h > 0.01f)
      {
        dl->AddRectFilled(ImVec2(mn.x + 4.0f * s, mn.y + 4.0f * s), ImVec2(mx.x - 4.0f * s, mx.y - 4.0f * s),
                          ui::Fade(ui::kWhite, 0.05f * h * a), 12.0f * s);
      }
      float ty = mn.y + (row_h - ui::FontSize(FontKind::kBody)) * 0.5f - 1.0f * s;
      if (note)
      {
        ty -= ui::FontSize(FontKind::kSmall) * 0.5f;
        ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + 20.0f * s, ty + ui::FontSize(FontKind::kBody) + 1.0f * s),
                 ui::Fade(kPending, a), note);
      }
      ui::Text(dl, FontKind::kBody, ImVec2(mn.x + 20.0f * s, ty), ui::Fade(ui::kText, a), label);
    }

    // The choices, right aligned on a track, narrowed to fit when there are many. The one
    // clicked, -1: none.
    int RowChoices(float x0, float x1, float y, const char *id, const std::vector<const char *> &names, int chosen,
                   float a, bool live)
    {
      float s = ui::Scale(), row_h = RowHeight();
      const float chip_h = 36.0f * s, gap = 2.0f * s, track = 3.0f * s, edge = 12.0f * s;
      ImDrawList *dl = ImGui::GetWindowDrawList();
      int n = int(names.size());
      std::vector<float> widths(n);
      float total = 0.0f;
      for (int c = 0; c < n; ++c)
      {
        widths[c] = ChipWidth(names[c]);
        total += widths[c] + (c ? gap : 0.0f);
      }
      float room = x1 - edge - 2.0f * track - (x0 + RowLabelWidth());
      if (total > room)
      {
        std::fill(widths.begin(), widths.end(), (room - gap * float(n - 1)) / float(n));
        total = room;
      }
      float cx = x1 - edge - track - total, cy = y + (row_h - chip_h) * 0.5f;
      dl->AddRectFilled(ImVec2(cx - track, cy - track), ImVec2(cx + total + track, cy + chip_h + track),
                        ui::Fade(ui::kInk, 0.55f * a), chip_h * 0.32f + track);
      int clicked = -1;
      ImGui::PushID(id);
      for (int c = 0; c < n; ++c)
      {
        std::string cid = std::to_string(c);
        if (Chip(cid.c_str(), ImVec2(cx, cy), ImVec2(widths[c], chip_h), names[c], c == chosen, live, a, true))
        {
          clicked = c;
        }
        cx += widths[c] + gap;
      }
      ImGui::PopID();
      return clicked;
    }

    // A slider and its value's text, right aligned; true when moved.
    bool RowSliderControl(float x0, float x1, float y, const char *id, double &value, double lo, double hi,
                          const char *text, float a, bool live)
    {
      float s = ui::Scale(), row_h = RowHeight();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      float value_w = 84.0f * s;
      float sw = std::min(460.0f * s, x1 - 20.0f * s - value_w - (x0 + RowLabelWidth()));
      float sx = x1 - 20.0f * s - value_w - sw;
      bool moved = Slider(id, ImVec2(sx, y + (row_h - 28.0f * s) * 0.5f), sw, value, lo, hi, a, live);
      ImVec2 ts = ui::TextSize(FontKind::kBody, text);
      ui::Text(dl, FontKind::kBody, ImVec2(x1 - 20.0f * s - ts.x, y + (row_h - ts.y) * 0.5f - 1.0f * s),
               ui::Fade(ui::kAccentLight, a), text);
      return moved;
    }

    // A section of slider rows, applied as they move, saved once let go.
    template <size_t N>
    float SliderSection(float x0, float x1, float y, const char *title, const SliderRow (&rows)[N], float a, bool live)
    {
      float s = ui::Scale();
      y = Section(ImGui::GetWindowDrawList(), x0, x1, y, Tr(title), a);
      RowGroup(x0, x1, y, int(N), a);
      for (const SliderRow &sr : rows)
      {
        RowBase(x0, x1, y, sr.label, sr.label, Tr(sr.label), nullptr, a, live);
        double v = sr.get();
        char text[32];
        std::snprintf(text, sizeof(text), sr.format, v * sr.shown);
        if (RowSliderControl(x0, x1, y, sr.label, v, sr.lo, sr.hi, text, a, live))
        {
          sr.set(v);
          slider_dirty_.insert(&sr);
        }
        if (slider_dirty_.count(&sr) && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !g_pad_lr_held)
        {
          slider_dirty_.erase(&sr);
          SaveSlider(sr);
        }
        y += RowHeight();
      }
      return y + 26.0f * s;
    }

    template <size_t N>
    void ResetSliders(const SliderRow (&rows)[N])
    {
      for (const SliderRow &sr : rows)
      {
        sr.set(sr.reset);
        SaveSlider(sr);
      }
    }

    // An outlined button under a tab's rows; true when clicked.
    bool ResetButton(const char *id, const char *english, float x0, float &y, float a, bool live)
    {
      float s = ui::Scale();
      std::string text = Upper(Tr(english));
      float bw = ui::CapsSize(FontKind::kLabel, text.c_str()).x + 40.0f * s;
      bool clicked = ui::Button(id, ImVec2(x0, y), ImVec2(bw, 40.0f * s), text.c_str(), ui::ButtonStyle::kOutline, live, a);
      y += 40.0f * s + 8.0f * s;
      return clicked;
    }

    // --- Graphics: the Options page's settings (graphics_menu.h), applied at
    // once, and the menu's scale.
    float DrawGraphics(ImVec2 o, float w, float a, bool live)
    {
      float s = ui::Scale(), row_h = RowHeight();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      float x0 = o.x, x1 = o.x + w, y = o.y;

      for (int section = 0; section < 2; ++section)
      {
        y = Section(dl, x0, x1, y, Tr(section == 0 ? "Display" : "Effects"), a);
        int in_section = 0;
        for (int i = 0; i < ao2::gfxmenu::SettingCount(); ++i)
        {
          in_section += ao2::gfxmenu::SettingSection(i) == section ? 1 : 0;
        }
        RowGroup(x0, x1, y, in_section, a);
        for (int i = 0; i < ao2::gfxmenu::SettingCount(); ++i)
        {
          if (ao2::gfxmenu::SettingSection(i) != section)
          {
            continue;
          }
          std::string id = "setting" + std::to_string(i);
          bool restart = ao2::gfxmenu::WaitsForRestart(i);
          RowBase(x0, x1, y, id.c_str(), ao2::gfxmenu::SettingLabel(i), Tr(ao2::gfxmenu::SettingLabel(i)),
                  restart ? Tr("After restart") : nullptr, a, live);
          std::vector<const char *> names;
          for (int c = 0; c < ao2::gfxmenu::ChoiceCount(i); ++c)
          {
            names.push_back(Tr(ao2::gfxmenu::ChoiceName(i, c)));
          }
          int clicked = RowChoices(x0, x1, y, id.c_str(), names, ao2::gfxmenu::Chosen(i), a, live);
          if (clicked >= 0)
          {
            ao2::gfxmenu::Pick(i, clicked);
          }
          y += row_h;
        }
        y += 26.0f * s;
      }

      // The menu's scale: only once let go, the menu would change under the slider.
      y = Section(dl, x0, x1, y, Tr("Menu"), a);
      RowGroup(x0, x1, y, 1, a);
      RowBase(x0, x1, y, "menu_scale", "Menu scale", Tr("Menu scale"), nullptr, a, live);
      double scale = menu_scale_ >= 0.0 ? menu_scale_ : std::clamp(REXCVAR_GET(ao2_menu_scale), 1.0, 1.6);
      char text[32];
      std::snprintf(text, sizeof(text), "%.0f%%", scale * 100.0);
      if (RowSliderControl(x0, x1, y, "menu_scale", scale, 1.0, 1.6, text, a, live))
      {
        menu_scale_ = std::round(scale * 20.0) / 20.0;
      }
      if (menu_scale_ >= 0.0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !g_pad_lr_held)
      {
        REXCVAR_SET(ao2_menu_scale, menu_scale_);
        std::snprintf(text, sizeof(text), "%g", menu_scale_);
        ao2::gfxmenu::SaveValue("ao2_menu_scale", text, "hardware.toml", nullptr);
        menu_scale_ = -1.0;
      }
      y += row_h + 26.0f * s;

      if (ResetButton("reset_effects", "Reset effects", x0, y, a, live))
      {
        for (const auto &[label, name] : kEffectDefaults)
        {
          PickNamed(label, name);
        }
      }
      return y;
    }

    // --- Controls: mouse, aiming, camera, the pad.
    float DrawControls(ImVec2 o, float w, float a, bool live)
    {
      float s = ui::Scale();
      float x0 = o.x, x1 = o.x + w, y = o.y;
      y = SliderSection(x0, x1, y, "Mouse", kMouseSliders, a, live);

      y = Section(ImGui::GetWindowDrawList(), x0, x1, y, Tr("Aiming"), a);
      RowGroup(x0, x1, y, int(std::size(kAimToggles)), a);
      for (const ToggleRow &row : kAimToggles)
      {
        RowBase(x0, x1, y, row.label, row.label, Tr(row.label), nullptr, a, live);
        int clicked = RowChoices(x0, x1, y, row.cvar, {Tr("Off"), Tr("On")}, row.get() ? 1 : 0, a, live);
        if (clicked >= 0 && (clicked == 1) != row.get())
        {
          row.set(clicked == 1);
          SaveToggle(row);
        }
        y += RowHeight();
      }
      y += 26.0f * s;

      y = SliderSection(x0, x1, y, "Camera", kCameraSliders, a, live);
      y = SliderSection(x0, x1, y, "Gamepad", kPadSliders, a, live);
      if (ResetButton("reset_controls", "Reset to defaults", x0, y, a, live))
      {
        ResetSliders(kMouseSliders);
        ResetSliders(kCameraSliders);
        ResetSliders(kPadSliders);
        for (const ToggleRow &row : kAimToggles)
        {
          row.set(false);
          SaveToggle(row);
        }
      }
      return y;
    }

    // --- Sound: the volumes (audio_settings.cpp).
    float DrawSound(ImVec2 o, float w, float a, bool live)
    {
      float x0 = o.x, x1 = o.x + w, y = o.y;
      y = SliderSection(x0, x1, y, "Volume", kSoundSliders, a, live);
      if (ResetButton("reset_sound", "Reset to defaults", x0, y, a, live))
      {
        ResetSliders(kSoundSliders);
      }
      return y;
    }

    // The hovered settings row's help (RowHelp) by the mouse, or by the pad's focus, after a
    // moment on it. As wide as its lines.
    void DrawTip(float a)
    {
      double now = ImGui::GetTime();
      if (tip_ != tip_shown_)
      {
        tip_shown_ = tip_;
        tip_since_ = now;
      }
      const char *help = tip_ ? RowHelp(tip_) : "";
      float t = ui::Clamp01(float(now - tip_since_ - 0.35) / 0.15f);
      if (!*help || t <= 0.0f)
      {
        return;
      }
      float s = ui::Scale();
      ImDrawList *fg = ImGui::GetForegroundDrawList();
      const ImGuiViewport *vp = ImGui::GetMainViewport();
      float wrap = 360.0f * s, pad = 12.0f * s;
      ImFont *font = ui::Font(FontKind::kSmall);
      ImVec2 ts = font->CalcTextSizeA(ui::FontSize(FontKind::kSmall), FLT_MAX, wrap, help);
      float bw = std::ceil(ts.x) + 2.0f * pad, bh = ts.y + 2.0f * pad, margin = 8.0f * s;
      // Under the mouse or the focused item, above it where there is no room.
      ImVec2 under = g_pad_mode ? ImVec2(g_pad_focus_mn.x, g_pad_focus_mx.y + 10.0f * s)
                                : ImVec2(ImGui::GetMousePos().x + 18.0f * s, ImGui::GetMousePos().y + 22.0f * s);
      float above = g_pad_mode ? g_pad_focus_mn.y - 10.0f * s - bh : ImGui::GetMousePos().y - 10.0f * s - bh;
      float right = vp->Pos.x + vp->Size.x - margin, bottom = vp->Pos.y + vp->Size.y - margin;
      ImVec2 mn(std::max(vp->Pos.x + margin, std::min(under.x, right - bw)), under.y + bh > bottom ? above : under.y);
      ImVec2 mx(mn.x + bw, mn.y + bh);
      float ta = a * t;
      fg->AddRectFilled(ImVec2(mn.x, mn.y + 3.0f * s), ImVec2(mx.x, mx.y + 3.0f * s), ui::Fade(ui::kInk, 0.35f * ta),
                        ui::Round(10.0f));
      fg->AddRectFilled(mn, mx, ui::Fade(ui::kInk, 0.98f * ta), ui::Round(10.0f));
      fg->AddRect(mn, mx, ui::Fade(ui::kWhite, 0.16f * ta), ui::Round(10.0f), ImDrawFlags_None, 1.0f * s);
      Wrapped(fg, FontKind::kSmall, ImVec2(mn.x + pad, mn.y + pad), wrap, ui::Fade(ui::kText, ta), help);
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
      bool clicked = ImGui::InvisibleButton("##card", ImVec2(mx.x - mn.x, card_h), ImGuiButtonFlags_EnableNav) && enabled && live;
      bool hovered = enabled && live && ImGui::IsItemHovered();
      float h = Eased("hover", hovered);
      float onf = Eased("on", on, 16.0f);
      ImGui::PopID();
      if (hovered)
      {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      }
      float ea = enabled ? a : 0.45f * a;
      ui::Card(dl, mn, mx, ui::Fade(ui::kWhite, (0.04f + 0.04f * h) * ea));
      dl->AddRectFilled(mn, mx, ui::Fade(ui::kAccent, 0.07f * onf * ea), ui::Round(14.0f));
      dl->AddRect(mn, mx, ui::Fade(ui::Mix(ui::kWhite, ui::kAccent, onf), (0.08f + 0.30f * onf + 0.12f * h) * ea), ui::Round(14.0f),
                  ImDrawFlags_None, 1.0f * s);
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
          {"Unlock all", "Unlocks Professional difficulty and gives the profile $9,999,999.",
           [] { return REXCVAR_GET(ao2_unlock_all); }, [](bool on) { REXCVAR_SET(ao2_unlock_all, on); }, true},
          {"Bonus weapons", "Adds the BST-V1 and SAL-86 to the store.",
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
                 Tr("Online co-op: god mode applies on both PCs, to your character only."));
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
      // Locked only while a match's level runs: the lobby after a match is
      // still a network session and still has that match's settings until
      // the next map load, but what is chosen there is for the next match.
      ao2::versus::MatchSettingsInfo match = ao2::versus::MatchSettings();
      const uint8_t *base = GuestBase();
      bool running = network_ && base && ao2::versus::MatchRunning(base);
      std::string status, note;
      ImU32 tone = ui::kMuted;
      if (running && match.from >= 0)
      {
        const ao2::versus::VersusSettings &m = match.settings;
        int preset = BotPresetOf(m.miss, m.reaction, m.turn, m.range);
        status = Tr(match.mine ? "This match plays by your rules and bot settings." :
                                 "This match plays by the host's rules and bot settings.");
        status += "  \xC2\xB7  ";
        status += !m.on ? Tr("Bots off") : preset >= 0 ? Tr(kBotPresets[preset].name) : Tr("Custom");
        note = Tr("The settings are locked until the match ends.");
        tone = ui::kGreen;
        // The match keeps the settings it began with (PollSettings sends them
        // once): nothing here changes mid-match.
        live = false;
      }
      else if (running)
      {
        status = Tr("Waiting for the match settings.");
        tone = kPending;
      }
      else
      {
        status = Tr("In a match, the host's settings apply to every player; yours apply when you host.");
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
          bool rule_hovered = Hovering(mn, mx, live);
          if (rule_hovered)
          {
            tip_ = r.label;
          }
          RowBack(dl, mn, mx, Eased(r.cvar, rule_hovered), a);
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
                    Tr("The first team to reach the cash goal wins at once. The respawn price shown in game is for the "
                       "first respawn; each later respawn of a player costs the step more.")) +
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
          {"Bots", "Bots fill the empty slots of a versus match.", "ao2_versus_bots",
           [] { return REXCVAR_GET(ao2_versus_bots); }, [](bool on) { REXCVAR_SET(ao2_versus_bots, on); }},
          {"Buying", "Bots buy weapon kits, armor and respawns with the team's cash.", "ao2_versus_bots_buy",
           [] { return REXCVAR_GET(ao2_versus_bots_buy); }, [](bool on) { REXCVAR_SET(ao2_versus_bots_buy, on); }},
          {"Infinite ammo", "The bots' weapons never run out, like the AI partner's.", "ao2_versus_bots_infinite_ammo",
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
      y = Section(dl, x0, x1, y, Tr("Bot difficulty"), a);
      int preset = BotPresetOf(REXCVAR_GET(ao2_versus_bots_miss), REXCVAR_GET(ao2_versus_bots_reaction),
                               REXCVAR_GET(ao2_versus_bots_turn), REXCVAR_GET(ao2_versus_bots_range));
      // Custom opens the values one by one, and is what values of no preset are.
      const bool custom_on = bot_custom_ || preset < 0;
      const float bh = 36.0f * s;
      float cx = x0;
      for (int i = 0; i < int(std::size(kBotPresets)); ++i)
      {
        const BotPreset &p = kBotPresets[i];
        const char *name = Tr(p.name);
        float cw = ChipWidth(name) + 24.0f * s;
        if (Chip(p.name, ImVec2(cx, y), ImVec2(cw, bh), name, !custom_on && i == preset, live, a))
        {
          bot_custom_ = false;
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
      {
        const char *custom = Tr("Custom");
        float cw = ChipWidth(custom) + 24.0f * s;
        if (Chip("custom", ImVec2(cx, y), ImVec2(cw, bh), custom, custom_on, live, a))
        {
          bot_custom_ = true;
        }
      }
      y += bh + 28.0f * s;
      if (!custom_on)
      {
        return y;
      }

      // The values one by one, saved once the slider is let go.
      y = Section(dl, x0, x1, y, Tr("Fine tuning"), a);
      const float row_h = 52.0f * s, label_w = 260.0f * s;
      for (int k = 0; k < int(std::size(kBotSliders)); ++k)
      {
        const SliderRow &sr = kBotSliders[k];
        ImVec2 mn(x0, y), mx(x1, y + row_h);
        bool slider_hovered = Hovering(mn, mx, live);
        if (slider_hovered)
        {
          tip_ = sr.label;
        }
        RowBack(dl, mn, mx, Eased(sr.label, slider_hovered), a);
        ui::Text(dl, FontKind::kBody, ImVec2(mn.x + 20.0f * s, mn.y + (row_h - ui::FontSize(FontKind::kBody)) * 0.5f -
                                                                   1.0f * s),
                 ui::Fade(ui::kText, a), Tr(sr.label));
        double v = sr.get();
        double shown = k == kBotReactionRow ? v * kMsPerLockstepFrame : v;
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
        if (bot_slider_dirty_[k] && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !g_pad_lr_held)
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
               Tr("Settings are saved as you change them."));
      return y + ui::FontSize(FontKind::kSmall) + 8.0f * s;
    }

    // --- Servers: the games hosted over Steam (steam_net.h). The filters,
    // a table sorted by a click on its header (the lowest ping first), JOIN
    // or a double click on a row; QUICK JOIN the open game of this version
    // that QuickScore picks (the fullest with a good ping). Joined, the host's game invites this one (and it
    // accepts by itself): the menu closes for the game to go there.
    // Playing with someone met elsewhere (Discord): this player's code to
    // give, and a field for the host's (ao2::steam::JoinByCode).
    // Steam running; else a box saying so (false).
    bool SteamUp(float x0, float x1, float y, float a)
    {
      if (ao2::steam::Running())
      {
        return true;
      }
      float s = ui::Scale();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      float fh = ui::FontSize(FontKind::kSmall);
      ImVec2 mn(x0, y), mx(x1, y + fh * 2.0f + 26.0f * s);
      ui::Card(dl, mn, mx, ui::Fade(ui::kWhite, 0.03f * a));
      ui::Bar(dl, mn, mx, ui::Fade(kPending, a));
      ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + 20.0f * s, mn.y + 11.0f * s), ui::Fade(ui::kText, a),
               Tr("Steam is not running."));
      ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + 20.0f * s, mn.y + 15.0f * s + fh), ui::Fade(ui::kMuted, a),
               Tr("Start the Steam client, then restart the game to play over Steam."));
      return false;
    }

    float DrawCode(float x0, float x1, float y, float a, bool live)
    {
      float s = ui::Scale();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      double now = ImGui::GetTime();
      const float h = 64.0f * s, bh = 36.0f * s;
      ImVec2 mn(x0, y), mx(x1, y + h);
      ui::Card(dl, mn, mx, ui::Fade(ui::kWhite, 0.03f * a));
      ui::Bar(dl, mn, mx, ui::Fade(ui::kAccent, a));
      float by = y + (h - bh) * 0.5f;

      // Left: this player's code and COPY.
      std::string code = ao2::steam::JoinCode();
      float lx = mn.x + 20.0f * s;
      ui::Text(dl, FontKind::kSmall, ImVec2(lx, y + 10.0f * s), ui::Fade(ui::kMuted, a), Tr("Your code"));
      ui::Caps(dl, FontKind::kHeading, ImVec2(lx, y + 14.0f * s + ui::FontSize(FontKind::kSmall)),
               ui::Fade(ui::kAccentLight, a), code.c_str());
      float cw = std::max(ui::CapsSize(FontKind::kHeading, code.c_str()).x,
                          ui::TextSize(FontKind::kSmall, Tr("Your code")).x);
      bool copied = now < code_copied_until_;
      if (ui::Button("code_copy", ImVec2(lx + cw + 18.0f * s, by), ImVec2(130.0f * s, bh),
                     copied ? Tr("COPIED") : Tr("COPY"), ui::ButtonStyle::kOutline, live && !code.empty(), a))
      {
        ImGui::SetClipboardText(code.c_str());
        code_copied_until_ = now + 2.0;
      }

      // Right: the host's code and JOIN.
      ao2::steam::CodeJoin state = ao2::steam::CodeJoinState();
      bool knocking = state == ao2::steam::CodeJoin::kKnocking;
      const float jw = 130.0f * s, pw = 130.0f * s, fw = 200.0f * s;
      float jx = mx.x - 14.0f * s - jw, px = jx - 8.0f * s - pw, fx = px - 8.0f * s - fw;
      // PASTE: the clipboard's code into the field (and asked for at once).
      bool pasted = false;
      if (ui::Button("code_paste", ImVec2(px, by), ImVec2(pw, bh), Tr("PASTE"), ui::ButtonStyle::kOutline,
                     live && !knocking && !code.empty(), a))
      {
        const char *clip = ImGui::GetClipboardText();
        std::string text;
        for (const char *c = clip ? clip : ""; *c && text.size() + 1 < sizeof(code_text_); ++c)
        {
          if (!std::isspace(static_cast<unsigned char>(*c)))
          {
            text += char(std::toupper(static_cast<unsigned char>(*c)));
          }
        }
        std::snprintf(code_text_, sizeof(code_text_), "%s", text.c_str());
        pasted = code_text_[0] != 0;
      }
      ImGui::PushFont(ui::Font(FontKind::kBody), ui::FontSize(FontKind::kBody));
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f * s, (bh - ui::FontSize(FontKind::kBody)) * 0.5f));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * s);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * s);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::Fade(ui::kWhite, 0.05f));
      ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ui::Fade(ui::kWhite, 0.08f));
      ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ui::Fade(ui::kWhite, 0.10f));
      ImGui::PushStyleColor(ImGuiCol_Border, ui::Fade(ui::kAccent, 0.45f));
      ImGui::PushStyleColor(ImGuiCol_Text, ui::kAccentLight);
      ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ui::Fade(ui::kAccent, 0.35f));
      ImGui::SetCursorScreenPos(ImVec2(fx, by));
      ImGui::SetNextItemWidth(fw);
      ImGui::BeginDisabled(!live || code.empty());
      bool enter = ImGui::InputTextWithHint("##join_code", Tr("Their code"), code_text_, sizeof(code_text_),
                                            ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue |
                                                ImGuiInputTextFlags_AutoSelectAll);
      ImGui::EndDisabled();
      ImGui::PopStyleColor(6);
      ImGui::PopStyleVar(4);
      ImGui::PopFont();
      bool join = ui::Button("code_join", ImVec2(jx, by), ImVec2(jw, bh), knocking ? Tr("ASKING") : Tr("JOIN"),
                             ui::ButtonStyle::kPrimary, live && !knocking && code_text_[0], a);
      if ((join || enter || pasted) && !knocking && code_text_[0])
      {
        code_bad_ = !ao2::steam::JoinByCode(code_text_);
        code_wait_ = !code_bad_;
      }
      y = mx.y + 8.0f * s;

      // What became of it.
      std::string host = ao2::steam::CodeJoinHost();
      const char *who = host.empty() ? Tr("The host") : host.c_str();
      char line[320] = {};
      ImU32 tone = ui::kMuted;
      if (code_bad_)
      {
        std::snprintf(line, sizeof(line), "%s", Tr("That's not a code: 7 letters and digits, like ABC-DEFG."));
        tone = ui::kRed;
      }
      else if (code_wait_ || state != ao2::steam::CodeJoin::kNone)
      {
        switch (state)
        {
        case ao2::steam::CodeJoin::kKnocking:
          std::snprintf(line, sizeof(line), "%s", Tr("Asking the host's game for an invite..."));
          tone = ui::kText;
          break;
        case ao2::steam::CodeJoin::kInvited:
          std::snprintf(line, sizeof(line), Tr("%s's game invited you. Joining..."), who);
          tone = ui::kGreen;
          if (code_wait_)
          {
            code_wait_ = false;
            BeginClose();
          }
          break;
        case ao2::steam::CodeJoin::kNoGame:
          std::snprintf(line, sizeof(line),
                        Tr("%s isn't hosting a game right now. Ask them to create one (Online), then enter the code again."),
                        who);
          tone = ui::kRed;
          break;
        case ao2::steam::CodeJoin::kFull:
          std::snprintf(line, sizeof(line), Tr("%s's game is full."), who);
          tone = ui::kRed;
          break;
        case ao2::steam::CodeJoin::kStarted:
          std::snprintf(line, sizeof(line), Tr("%s's match has already started. Wait for it to end."), who);
          tone = ui::kRed;
          break;
        case ao2::steam::CodeJoin::kNoAnswer:
          std::snprintf(line, sizeof(line), "%s",
                        Tr("No answer. Check the code; the host's game must be running with Steam."));
          tone = ui::kRed;
          break;
        case ao2::steam::CodeJoin::kOwnCode:
          std::snprintf(line, sizeof(line), "%s", Tr("That's your own code. Enter the host's code."));
          tone = ui::kRed;
          break;
        default:
          break;
        }
        if (state != ao2::steam::CodeJoin::kKnocking && state != ao2::steam::CodeJoin::kInvited)
        {
          code_wait_ = false;
        }
      }
      if (!line[0])
      {
        std::snprintf(line, sizeof(line), "%s",
                      Tr("Met someone on Discord? One player creates a game (Online) and shares their code; the other "
                         "enters it here. You don't need to be Steam friends."));
      }
      y = Wrapped(dl, FontKind::kSmall, ImVec2(x0, y), x1 - x0, ui::Fade(tone, a), line);
      return y + 18.0f * s;
    }

    float DrawServers(ImVec2 o, float w, float a, bool live)
    {
      float s = ui::Scale();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      float x0 = o.x, x1 = o.x + w, y = o.y;
      double now = ImGui::GetTime();

      if (!SteamUp(x0, x1, y, a))
      {
        return y + 80.0f * s;
      }

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
      // Co-op games or versus matches, as the chips on the right pick.
      std::erase_if(all, [&](const ao2::steam::Lobby &l) { return l.versus != servers_versus_; });
      auto open = [](const ao2::steam::Lobby &l) {
        return !l.joined && !l.in_game && l.players < l.max && !ao2::invite::IsOtherBuild(l.build);
      };

      // --- Toolbar: quick join, refresh; the filters on the right ---
      const float bh = 36.0f * s;
      const ao2::steam::Lobby *best = nullptr;
      for (const ao2::steam::Lobby &l : all)
      {
        if (open(l) && (!best || ao2::steam::QuickScore(l) < ao2::steam::QuickScore(*best)))
        {
          best = &l; // the fullest open game with a good ping (QuickScore)
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
      // The mode: co-op or versus.
      {
        const char *labels[2] = {Tr("CO-OP"), Tr("VERSUS")};
        float cw[2] = {ChipWidth(labels[0]) + 12.0f * s, ChipWidth(labels[1]) + 12.0f * s};
        float cx = x1 - cw[0] - cw[1] - 8.0f * s;
        for (int i = 0; i < 2; ++i)
        {
          char id[16];
          std::snprintf(id, sizeof(id), "srvmode%d", i);
          if (Chip(id, ImVec2(cx, y), ImVec2(cw[i], bh), labels[i], servers_versus_ == (i == 1), live, a) &&
              servers_versus_ != (i == 1))
          {
            // Kept for the next time the menu opens (it is made anew each time) and the next start.
            servers_versus_ = i == 1;
            REXCVAR_SET(ao2_servers_versus, servers_versus_);
            ao2::gfxmenu::SaveValue("ao2_servers_versus", servers_versus_ ? "true" : "false", "online.toml", nullptr);
          }
          cx += cw[i] + 8.0f * s;
        }
      }
      y += bh + 10.0f * s;
      if (joining_ || join_failed_)
      {
        ui::Text(dl, FontKind::kSmall, ImVec2(x0, y), ui::Fade(join_failed_ ? ui::kRed : ui::kText, a),
                 join_failed_ ? Tr("Couldn't join that game. It may have started or filled up; try another one.")
                              : Tr("Joining... The host's game sends an invite, and your game follows it automatically."));
        y += ui::FontSize(FontKind::kSmall) + 8.0f * s;
      }
      y += 8.0f * s;

      // --- The table ---
      // All of them, the lowest ping first (as Lobbies() gives them).
      std::vector<const ao2::steam::Lobby *> rows;
      for (const ao2::steam::Lobby &l : all)
      {
        rows.push_back(&l);
      }

      const float action_w = 150.0f * s;
      const float cols[5] = {x0 + 20.0f * s, x0 + w * 0.32f, x0 + w * 0.52f, x0 + w * 0.66f, x0 + w * 0.76f};
      const float col_end[5] = {cols[1], cols[2], cols[3], cols[4], x1 - action_w - 20.0f * s};
      const char *heads[5] = {Tr("HOST"), Tr(servers_versus_ ? "MAP" : "MISSION"),
                              Tr(servers_versus_ ? "MODE" : "DIFFICULTY"), Tr("PLAYERS"), Tr("PING")};
      for (int c = 0; c < 5; ++c)
      {
        ui::Caps(dl, FontKind::kLabel, ImVec2(cols[c], y), ui::Fade(ui::kMuted, 0.8f * a), heads[c]);
      }
      y += ui::FontSize(FontKind::kLabel) + 10.0f * s;

      if (rows.empty())
      {
        float eh = 120.0f * s;
        ImVec2 mn(x0, y), mx(x1, y + eh);
        ui::Frame(dl, mn, mx, ui::Fade(ui::kWhite, 0.08f * a), 1.0f * s);
        const char *line1 = refreshing        ? Tr("Looking for games...")
                            : servers_versus_ ? Tr("No public matches right now.")
                                              : Tr("No public co-op games right now.");
        const char *line2 = Tr("Create a public game and it shows up here for everyone.");
        ImVec2 s1 = ui::TextSize(FontKind::kBody, line1), s2 = ui::TextSize(FontKind::kSmall, line2);
        ui::Text(dl, FontKind::kBody, ImVec2((x0 + x1 - s1.x) * 0.5f, y + eh * 0.5f - s1.y - 2.0f * s),
                 ui::Fade(ui::kText, a), line1);
        ui::Text(dl, FontKind::kSmall, ImVec2((x0 + x1 - s2.x) * 0.5f, y + eh * 0.5f + 4.0f * s),
                 ui::Fade(ui::kMuted, a), line2);
        return y + eh + 8.0f * s;
      }
      const ao2::steam::Lobby *details = nullptr; // the versus row under the mouse: its match's details
      float details_y = 0.0f, details_h = 0.0f;
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
        if (row_hovered && l.versus)
        {
          details = &l;
          details_y = mn.y;
          details_h = rh;
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
        // No notes: the name in the middle of the row, as the other cells.
        const bool other_build = ao2::invite::IsOtherBuild(l.build) && !l.own;
        ui::Text(dl, FontKind::kBody, ImVec2(cols[0], notes.empty() && !other_build ? ty : mn.y + 8.0f * s),
                 ui::Fade(ui::kText, a), l.name.c_str());
        ui::Text(dl, FontKind::kSmall, ImVec2(cols[0], mn.y + 32.0f * s), ui::Fade(ui::kMuted, a), notes.c_str());
        if (other_build)
        {
          float nx = cols[0] + ui::TextSize(FontKind::kSmall, notes.c_str()).x;
          std::string other = std::string(notes.empty() ? "" : "  \xC2\xB7  ") + Tr("different version");
          ui::Text(dl, FontKind::kSmall, ImVec2(nx, mn.y + 32.0f * s), ui::Fade(ui::kRed, a), other.c_str());
        }
        dl->PopClipRect();
        cell(1, ui::kText, ao2::steam::MissionName(l.map, l.versus));
        if (l.versus)
        {
          cell(2, ui::kText, ao2::steam::ModeName(true, l.game_type));
        }
        else
        {
          const char *difficulty = ao2::steam::DifficultyName(l.difficulty);
          cell(2, ui::kText, *difficulty ? difficulty : "\xE2\x80\x94");
        }
        bool full = l.players >= l.max;
        cell(3, !full && !l.in_game ? ui::kText : ui::kMuted,
             std::to_string(std::min(l.players, l.max)) + " / " + std::to_string(l.max));
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
      if (details)
      {
        ao2::browser::MatchDetails(*details, a, ImVec2(x0, y), ImVec2(x1, y), details_y, details_h);
      }
      else
      {
        ao2::browser::MatchDetailsHidden();
      }
      y += 6.0f * s;
      ui::Text(dl, FontKind::kSmall, ImVec2(x0, y), ui::Fade(ui::kMuted, a),
               Tr("Double-click a game to join it. The list refreshes every 20 seconds."));
      return y + ui::FontSize(FontKind::kSmall) + 8.0f * s;
    }

    // --- Online: the partners (Steam friends, the LAN, a VPN:
    // coop_invite.cpp).
    float DrawPartners(ImVec2 o, float w, float a, bool live, bool &keep)
    {
      float s = ui::Scale();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      float x0 = o.x;
      float y = o.y;
      if (ao2::invite::Running())
      {
        ImGui::SetCursorScreenPos(ImVec2(x0, y));
        keep = ao2::invite::DrawTab(ImVec2(x0, y), w, fresh_);
        y = ImGui::GetCursorScreenPos().y + 10.0f * s;
      }
      else
      {
        ui::Text(dl, FontKind::kSmall, ImVec2(x0, y), ui::Fade(ui::kMuted, a), Tr("Invites are off (ao2_invites)."));
        y += ui::FontSize(FontKind::kSmall) + 18.0f * s;
      }
      return y;
    }

    // --- Debug: the checkpoints or the cheats, picked with the chips over
    // them.
    // A section's pages as a row of chips; returns the top of what is under them.
    template <size_t N>
    ImVec2 PageChips(const char *const (&names)[N], int &page, ImVec2 mn, float a, bool live)
    {
      float s = ui::Scale();
      const float ch = 34.0f * s, gap = 8.0f * s;
      float cx = mn.x;
      page = std::clamp(page, 0, int(N) - 1);
      for (int p = 0; p < int(N); ++p)
      {
        const char *label = Tr(names[p]);
        float cw = ChipWidth(label) + 12.0f * s;
        char id[16];
        std::snprintf(id, sizeof(id), "page%d", p);
        if (Chip(id, ImVec2(cx, mn.y), ImVec2(cw, ch), label, page == p, live, a) && page != p)
        {
          page = p;
          fresh_ = true;
        }
        cx += cw + gap;
      }
      return ImVec2(mn.x, mn.y + ch + 20.0f * s);
    }

    void DrawOnline(ImVec2 mn, ImVec2 mx, float a, bool live)
    {
      float s = ui::Scale();
      ImVec2 cmn = PageChips(kOnlinePageNames, online_page_, mn, a, live);
      switch (online_page_)
      {
      case kOnlinePlayers:
      {
        bool keep = true;
        Scrolling("online", cmn, mx, false, [&](ImVec2 o, float w) { return DrawPartners(o, w, a, live, keep); });
        if (!keep)
        {
          BeginClose(); // an invite accepted: the game goes on to join it
        }
        break;
      }
      case kOnlineCode:
        Scrolling("code", cmn, mx, false, [&](ImVec2 o, float w) {
          if (!SteamUp(o.x, o.x + w, o.y, a))
          {
            return o.y + 80.0f * s;
          }
          return DrawCode(o.x, o.x + w, o.y, a, live);
        });
        break;
      default:
        Scrolling("servers", cmn, mx, false, [&](ImVec2 o, float w) { return DrawServers(o, w, a, live); });
        break;
      }
    }

    void DrawDebug(ImVec2 mn, ImVec2 mx, float a, bool live)
    {
      ImVec2 cmn = PageChips(kPageNames, page_, mn, a, live);
      switch (page_)
      {
      case kPageCheckpoints:
        DrawCheckpoints(cmn, mx, a, live);
        break;
      default:
        Scrolling("cheats", cmn, mx, false, [&](ImVec2 o, float w) { return DrawCheats(o, w, a, live); });
        break;
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
          if (ImGui::InvisibleButton("##mission", ImVec2(w, rh), ImGuiButtonFlags_EnableNav) && live)
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
          if (ImGui::InvisibleButton("##checkpoint", ImVec2(w, rh), ImGuiButtonFlags_EnableNav) && live)
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
                                   : Tr("Double-click also loads. Saving the game stores");
      const char *hint2 = network_ ? Tr("files (a DLC checkpoint without that DLC breaks their game).")
                                   : Tr("the loaded checkpoint in the profile as the current one.");
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
    float mark_x_ = -1.0f, mark_target_ = 0.0f;       // the sections' bar: the current one's block (x)
    float mark_w_ = -1.0f, mark_width_target_ = 0.0f; // ... and its width
    // Online: the games list's refresh, the join under way.
    double servers_refreshed_ = -100.0;
    bool servers_versus_ = REXCVAR_GET(ao2_servers_versus); // the games list shows versus matches (else co-op games)
    char code_text_[24] = {};       // the host's code (Join with code)
    bool code_bad_ = false;         // ... was not a code
    bool code_wait_ = false;        // ... asked for here: closed once invited
    double code_copied_until_ = 0.0;
    uint64_t joining_ = 0;          // the lobby being joined
    double joining_since_ = 0.0;
    bool join_failed_ = false;
    bool network_ = false, ready_ = false;
    bool close_requested_ = false;
    double badges_time_ = 0.0;
    int ach_got_ = 0, ach_total_ = 0;
    const char *tip_ = nullptr, *tip_shown_ = nullptr; // the settings row hovered (its label), the one tipped
    double tip_since_ = 0.0;
    int page_ = kPageCheckpoints;      // the Debug section's page
    int online_page_ = kOnlinePlayers; // the Online section's page
    bool bot_custom_ = false;          // the bots' Custom difficulty picked (the values may still be a preset's)
    float content_alpha_ = 1.0f;       // the content's alpha this frame
    double menu_scale_ = -1.0;         // the menu scale's slider while held, -1: not moved
    std::set<const SliderRow *> slider_dirty_; // sliders moved and not yet saved
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
    std::function<bool()> notice, toast, banner, hint;
    uint64_t notice_gen, toast_gen, banner_gen, hint_gen;
    {
      std::lock_guard lock(g_notice_mutex);
      notice = g_notice;
      toast = g_toast;
      banner = g_banner;
      hint = g_hint;
      notice_gen = g_notice_gen;
      toast_gen = g_toast_gen;
      banner_gen = g_banner_gen;
      hint_gen = g_hint_gen;
    }
    bool hint_over = hint && !hint();
    bool banner_over = banner && !banner();
    bool notice_over = notice && !notice();
    bool toast_over = toast && !toast();
    if (notice_over || toast_over || banner_over || hint_over)
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
      if (hint_over && g_hint_gen == hint_gen)
      {
        g_hint = nullptr;
      }
      g_notice_on = g_notice || g_toast || g_banner || g_hint;
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
      ao2::coop::status::DrawHud();
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
      // What closed it (Esc, B) is still down: the pads give the game nothing until
      // let go (the keys: InputBlocker).
      if (--g_modal == 0 && !g_open.load())
      {
        for (auto &swallow : g_pad_swallow)
        {
          swallow = 2;
        }
      }
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
    bool on = ao2::perf::GetMode() != ao2::perf::kOff || g_notice_on.load(std::memory_order_relaxed) ||
              ao2::coop::status::HudWanted();
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
    g_frames_wanted = g_perf_dialog || g_own_drawer->HasDialogs() || g_notice_on || ao2::coop::status::HudWanted();
  }

  // Native renderer, at each present (GPU thread).
  void RequestOwnFrame()
  {
    bool wanted = g_frames_wanted.load(std::memory_order_relaxed) || ao2::perf::GetMode() != ao2::perf::kOff ||
                  g_notice_on.load(std::memory_order_relaxed) || ao2::coop::status::HudWanted();
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
  // A key pressed while blocked stays from the game until it is let go: the key
  // repeat of the Esc that closed a window was a fresh Esc for the menu behind it.
  class InputBlocker final : public rex::ui::WindowInputListener
  {
  public:
    void OnKeyDown(rex::ui::KeyEvent &e) override
    {
      const uint32_t vk = uint32_t(e.virtual_key()) & 0xFF;
      if (Blocking())
      {
        g_key_held[vk] = true;
      }
      e.set_handled(g_key_held[vk].load());
    }
    void OnKeyUp(rex::ui::KeyEvent &e) override { g_key_held[uint32_t(e.virtual_key()) & 0xFF] = false; }
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
  bool HeldFromMenu(uint32_t vk) { return g_key_held[vk & 0xFF].load(std::memory_order_relaxed); }

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
        if (g_pad_reading)
        {
          return true; // the menu's own read (ReadPads)
        }
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

  uint32_t PressedPads(uint32_t *back)
  {
    using Fn = uint32_t (*)(uint32_t *);
    static Fn fn = []() -> Fn {
      HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
      return runtime ? reinterpret_cast<Fn>(GetProcAddress(runtime, "rex_input_pressed_pads")) : nullptr;
    }();
    *back = 0;
    if (!fn || !g_window || !g_window->HasFocus())
    {
      return 0;
    }
    g_pad_reading = true;
    uint32_t pressed = fn(back);
    g_pad_reading = false;
    return pressed;
  }

  uint16_t PadButtons(int16_t *stick_x)
  {
    *stick_x = 0;
    PadRead pad;
    if (!g_window || !g_window->HasFocus() || !ReadPads(pad))
    {
      return 0;
    }
    *stick_x = pad.thumbs[0];
    return pad.buttons;
  }

  void OnPadPoll(uint32_t user, uint8_t *state)
  {
    if (user >= 4 || Blocking())
    {
      return; // the game gets an untouched pad anyway
    }
    constexpr uint16_t kPair = 0x0030; // Select, Start
    using Clock = std::chrono::steady_clock;
    static constexpr auto kWait = std::chrono::milliseconds(100), kTap = std::chrono::milliseconds(50);
    struct Chord
    {
      bool latched = false;  // the pair was pressed: kept from the game until let go
      uint16_t waiting = 0;  // held back for the other one to come
      uint16_t passing = 0;  // given to the game
      uint16_t tap = 0;      // let go while waiting: the game still gets its press
      Clock::time_point since, tap_until;
    };
    static Chord chords[4];
    uint16_t b = uint16_t(state[4] << 8 | state[5]);
    auto write = [&](uint16_t v) {
      state[4] = uint8_t(v >> 8);
      state[5] = uint8_t(v);
    };

    // After the menu: nothing until the hands are off. Two polls in a row, as one may have started
    // while the menu was open and read an empty pad.
    int swallow = g_pad_swallow[user].load(std::memory_order_relaxed);
    if (swallow > 0)
    {
      bool off = b == 0 && state[6] < 30 && state[7] < 30;
      g_pad_swallow[user].store(off ? swallow - 1 : 2, std::memory_order_relaxed);
      write(0);
      state[6] = 0;
      state[7] = 0;
      chords[user] = Chord{};
      return;
    }
    if (!REXCVAR_GET(ao2_debug_menu_pad))
    {
      return;
    }

    Chord &c = chords[user];
    uint16_t pair = b & kPair;
    auto now = Clock::now();
    if (c.latched)
    {
      c.latched = pair != 0;
      write(b & uint16_t(~kPair));
      return;
    }
    if (pair == kPair)
    {
      c = Chord{};
      c.latched = true;
      write(b & uint16_t(~kPair));
      if (g_window)
      {
        g_window->app_context().CallInUIThreadDeferred([] {
          g_open_by_pad = !g_menu;
          Toggle();
          g_open_by_pad = false;
        });
      }
      return;
    }
    c.passing &= pair;
    if (c.waiting && !(pair & c.waiting))
    {
      c.tap = c.waiting;
      c.tap_until = now + kTap;
      c.waiting = 0;
    }
    if (!c.waiting && (pair & uint16_t(~c.passing)))
    {
      c.waiting = pair & uint16_t(~c.passing);
      c.since = now;
    }
    if (c.waiting && now - c.since >= kWait)
    {
      c.passing |= c.waiting;
      c.waiting = 0;
    }
    if (c.tap && now >= c.tap_until)
    {
      c.tap = 0;
    }
    write(uint16_t((b & ~kPair) | c.passing | c.tap));
  }

  void ConfigureFonts(ImFontAtlas *atlas)
  {
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF, // Basic Latin, Latin-1
        0x0400, 0x04FF, // Cyrillic
        0x2010, 0x205E, // punctuation
        0,
    };
    for (const std::string &path : ao2::ui::FontFiles({"C:\\Windows\\Fonts\\segoeui.ttf"}))
    {
      ImFontConfig config;
      config.OversampleH = 2;
      config.OversampleV = 1;
      if (ImFont *font = atlas->AddFontFromFileTTF(path.c_str(), 18.0f, &config, kRanges))
      {
        ImGui::GetIO().FontDefault = font;
        break;
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
      g_notice_on = g_notice || g_toast || g_banner || g_hint;
    }
    ShowNoticeDialog();
  }

  void SetToast(std::function<bool()> draw)
  {
    {
      std::lock_guard lock(g_notice_mutex);
      g_toast = std::move(draw);
      ++g_toast_gen;
      g_notice_on = g_notice || g_toast || g_banner || g_hint;
    }
    ShowNoticeDialog();
  }

  void SetBanner(std::function<bool()> draw)
  {
    {
      std::lock_guard lock(g_notice_mutex);
      g_banner = std::move(draw);
      ++g_banner_gen;
      g_notice_on = g_notice || g_toast || g_banner || g_hint;
    }
    ShowNoticeDialog();
  }

  void SetHint(std::function<bool()> draw)
  {
    {
      std::lock_guard lock(g_notice_mutex);
      g_hint = std::move(draw);
      ++g_hint_gen;
      g_notice_on = g_notice || g_toast || g_banner || g_hint;
    }
    ShowNoticeDialog();
  }

  int BotPreset()
  {
    return BotPresetOf(REXCVAR_GET(ao2_versus_bots_miss), REXCVAR_GET(ao2_versus_bots_reaction),
                       REXCVAR_GET(ao2_versus_bots_turn), REXCVAR_GET(ao2_versus_bots_range));
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
