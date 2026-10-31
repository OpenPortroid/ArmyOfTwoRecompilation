// debug_menu.cpp - The F1 debug menu: graphics, achievements, invites,
// cheats, checkpoints.
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
// A full screen overlay in the launcher's look (ui_theme.h) with five tabs:
// - Graphics: the Options screen's Graphics page settings (graphics_menu.h),
//   applied at once and saved as the page saves them (the render resolution
//   too: the page waits for its Accept), and the quick settings' field of
//   view and mouse sensitivity (until the game is closed).
// - Achievements: the achievements menu's list (achievements.h).
// - Invites: the players window's content (coop_invite.h).
// - Cheats: god mode, infinite ammo, Unlock All, the bonus weapons (cheats.cpp).
// - Checkpoints: the files in AO2Game/Checkpoints and in the extracted DLC
//   packages (Kiev 09_*, Miami alternate ending 10_*; the name of each,
//   then its level), loaded with the console's "checkpoint <name> <difficulty>"
//   (console_cmd.cpp). The loaded checkpoint becomes the profile's current
//   one when the game saves it.
// - F2 (ao2_fps_overlay_key): the frame rate overlay's mode (perf_overlay.h).
// - F3 (ao2_quick_settings_key): field of view and mouse sensitivity
//   (quick_settings.h); it takes the SDK's own F3 overlay's key.
// - F7 (ao2_achievements_key): the achievements menu (achievements.h).
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
#include "quick_settings.h"
#include "ui_text.h"
#include "ui_theme.h"

#include <algorithm>
#include <cctype>
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
REXCVAR_DECLARE(double, ao2_fov);
REXCVAR_DECLARE(double, ao2_fov_aim);
REXCVAR_DECLARE(double, ao2_mouse_sensitivity);
REXCVAR_DECLARE(double, ao2_mouse_zoom_sensitivity);
REXCVAR_DECLARE(int32_t, ao2_start_difficulty);
REXCVAR_DECLARE(std::string, ao2_players_key);
REXCVAR_DECLARE(std::string, ao2_achievements_key);
REXCVAR_DECLARE(std::string, ao2_invite_accept_key);

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
    dl->AddRectFilled(mn, mx, ui::Fade(ui::kWhite, (0.025f + 0.045f * h) * a));
    dl->AddRectFilled(mn, ImVec2(mn.x + 2.0f * s, mx.y), ui::Fade(bar, (0.2f + 0.8f * h) * a));
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

  enum Tab
  {
    kTabGraphics,
    kTabAchievements,
    kTabInvites,
    kTabCheats,
    kTabCheckpoints,
    kTabCount,
  };
  constexpr const char *kTabNames[kTabCount] = {"Graphics", "Achievements", "Invites", "Cheats", "Checkpoints"};
  int g_last_tab = kTabGraphics; // the tab the menu opens on (the last one shown)

  const char *kDifficulties[] = {"Recruit", "Contractor", "Professional"};

  // The menu: a full screen overlay in the launcher's look (ui_theme.h), as
  // the achievements and players windows: a panel with a title, a row of
  // tabs (a click, 1-5, Tab / Shift+Tab) and a footer; the tabs' content
  // slides in. Esc, F1 and Close play it out.
  class DebugMenu final : public rex::ui::ImGuiDialog
  {
  public:
    static constexpr float kOpenTime = 0.3f;
    static constexpr float kCloseTime = 0.18f;

    explicit DebugMenu(rex::ui::ImGuiDrawer *drawer) : ImGuiDialog(drawer)
    {
      missions_ = ScanCheckpoints(g_checkpoint_dir, g_content_dir);
      difficulty_ = std::clamp(REXCVAR_GET(ao2_start_difficulty), 0, 2);
      tab_ = g_last_tab;
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
      dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                        ui::Fade(ui::kInk, 0.78f * a));
      const float pad = 32.0f * s;
      const float pw = std::min(1180.0f * s, vp->Size.x - 40.0f * s);
      const float ph = std::min(900.0f * s, vp->Size.y - 60.0f * s);
      ImVec2 center = vp->GetCenter();
      float x0 = center.x - pw * 0.5f, y0 = center.y - ph * 0.5f + (1.0f - a) * 30.0f * s;
      float x1 = x0 + pw, y1 = y0 + ph;
      float cx0 = x0 + pad, cx1 = x1 - pad;
      dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ui::Fade(ui::kPanel, 0.96f * a));
      dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), ui::Fade(ui::kWhite, 0.13f * a), 0.0f, ImDrawFlags_None, 1.0f * s);
      ui::Corners(dl, ImVec2(x0 - 4.0f * s, y0 - 4.0f * s), ImVec2(x1 + 4.0f * s, y1 + 4.0f * s), 18.0f * s,
                  2.0f * s, ui::Fade(ui::kAccent, 0.55f * a));

      // --- Header: kicker, title (wiped in), the co-op chip ---
      float y = y0 + pad;
      ui::Caps(dl, FontKind::kLabel, ImVec2(cx0, y), ui::Fade(ui::kMuted, a), "ARMY OF TWO  //  DEBUG");
      y += ui::FontSize(FontKind::kLabel) + 6.0f * s;
      std::string title = Upper(Tr("Debug menu"));
      ImVec2 title_size = ui::CapsSize(FontKind::kTitle, title.c_str());
      float wipe = ui::EaseOutCubic((since - 0.05f) / 0.45f);
      dl->PushClipRect(ImVec2(cx0, y), ImVec2(cx0 + title_size.x * wipe + 1.0f, y + title_size.y), true);
      ui::Caps(dl, FontKind::kTitle, ImVec2(cx0, y), ui::Fade(ui::kAccentLight, a), title.c_str());
      dl->PopClipRect();
      if (network_)
      {
        std::string chip = Upper(Tr("Online co-op")) + "  ·  " +
                           Upper(ready_ ? Tr("Both players") : Tr("Waiting for a level"));
        ImVec2 cs = ui::CapsSize(FontKind::kLabel, chip.c_str());
        float chip_h = cs.y + 12.0f * s;
        ImVec2 cmx(cx1, y + (title_size.y + chip_h) * 0.5f);
        ImVec2 cmn(cmx.x - cs.x - 36.0f * s, cmx.y - chip_h);
        ImU32 tone = ready_ ? ui::kGreen : IM_COL32(255, 190, 80, 255);
        float pulse = 0.5f + 0.5f * std::sin(float(now) * 4.0f);
        dl->AddRect(cmn, cmx, ui::Fade(ui::kWhite, 0.16f * a), 0.0f, ImDrawFlags_None, 1.0f * s);
        ImVec2 dc(cmn.x + 13.0f * s, (cmn.y + cmx.y) * 0.5f);
        dl->AddCircleFilled(dc, 4.0f * s, ui::Fade(tone, a));
        dl->AddCircle(dc, (4.0f + 4.0f * pulse) * s, ui::Fade(tone, (1.0f - pulse) * a), 16, 1.0f * s);
        ui::Caps(dl, FontKind::kLabel, ImVec2(cmn.x + 26.0f * s, cmn.y + 6.0f * s), ui::Fade(ui::kText, a),
                 chip.c_str());
      }
      y += title_size.y + 20.0f * s;

      // --- Tabs ---
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
      float tab_h = ui::FontSize(FontKind::kLabel) + 24.0f * s;
      float tx = cx0;
      for (int i = 0; i < kTabCount; ++i)
      {
        char number[4];
        std::snprintf(number, sizeof(number), "%02d", i + 1);
        std::string label = Upper(Tr(kTabNames[i]));
        float nw = ui::CapsSize(FontKind::kLabel, number).x;
        float lw = ui::CapsSize(FontKind::kLabel, label.c_str()).x;
        float w = nw + 10.0f * s + lw + 36.0f * s;
        ImVec2 mn(tx, y), mx(tx + w, y + tab_h);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(mn);
        if (ImGui::InvisibleButton("##tab", ImVec2(w, tab_h)) && live)
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
        if (h > 0.01f && !active)
        {
          dl->AddRectFilled(mn, mx, ui::Fade(ui::kWhite, 0.04f * h * a));
        }
        float ty = y + (tab_h - ui::FontSize(FontKind::kLabel)) * 0.5f - 1.0f * s;
        ui::Caps(dl, FontKind::kLabel, ImVec2(tx + 18.0f * s, ty),
                 ui::Fade(active ? ui::kAccent : ui::kMuted, (active ? 1.0f : 0.6f) * a), number);
        ui::Caps(dl, FontKind::kLabel, ImVec2(tx + 18.0f * s + nw + 10.0f * s, ty),
                 ui::Fade(active ? ui::kAccentLight : ui::Mix(ui::kMuted, ui::kText, h), a), label.c_str());
        if (active)
        {
          tab_x_target_ = tx;
          tab_w_target_ = w;
        }
        tx += w + 4.0f * s;
      }
      // The underline slides to the current tab.
      if (tab_w_ <= 0.0f)
      {
        tab_x_ = tab_x_target_, tab_w_ = tab_w_target_;
      }
      tab_x_ = ui::Approach(tab_x_, tab_x_target_, 16.0f);
      tab_w_ = ui::Approach(tab_w_, tab_w_target_, 16.0f);
      float line_y = y + tab_h;
      dl->AddLine(ImVec2(cx0, line_y), ImVec2(cx1, line_y), ui::Fade(ui::kWhite, 0.12f * a), 1.0f * s);
      dl->AddRectFilled(ImVec2(tab_x_, line_y - 1.5f * s), ImVec2(tab_x_ + tab_w_, line_y + 1.5f * s),
                        ui::Fade(ui::kAccent, a));
      y = line_y + 24.0f * s;

      // --- Footer: the keys ---
      const float kh = 24.0f * s;
      const float footer_y = y1 - pad - kh;
      dl->AddLine(ImVec2(cx0, footer_y - 14.0f * s), ImVec2(cx1, footer_y - 14.0f * s),
                  ui::Fade(ui::kWhite, 0.08f * a), 1.0f * s);
      {
        float fx = cx0;
        float label_y = footer_y + (kh - ui::FontSize(FontKind::kLabel)) * 0.5f - 1.0f * s;
        for (const char *key : {"1-5", "TAB"})
        {
          fx += ui::KeyCap(dl, ImVec2(fx, footer_y), kh, key, a) + 6.0f * s;
        }
        std::string tabs = Upper(Tr("Tabs"));
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

      // --- The tab's content: slides in from the side it was picked on ---
      float t = ui::EaseOutCubic(float(now - tab_changed_) / 0.3f);
      float ca = a * ui::Clamp01(float(now - tab_changed_) / 0.18f);
      float shift = (1.0f - t) * 30.0f * s * float(tab_dir_);
      ImVec2 cmn(cx0 + shift, y), cmx(cx1 + shift, footer_y - 30.0f * s);
      dl->PushClipRect(ImVec2(x0 + 1.0f, cmn.y - 4.0f * s), ImVec2(x1 - 1.0f, cmx.y + 4.0f * s), true);
      switch (tab_)
      {
      case kTabGraphics:
        Scrolling("graphics", cmn, cmx, fresh_, [&](ImVec2 o, float w) { return DrawGraphics(o, w, ca, live); });
        break;
      case kTabAchievements:
        ao2::ach::DrawTab(cmn, cmx, fresh_);
        break;
      case kTabInvites:
        DrawInvites(cmn, cmx, ca);
        break;
      case kTabCheats:
        Scrolling("cheats", cmn, cmx, fresh_, [&](ImVec2 o, float w) { return DrawCheats(o, w, ca, live); });
        break;
      case kTabCheckpoints:
        DrawCheckpoints(cmn, cmx, ca, live);
        break;
      }
      dl->PopClipRect();
      fresh_ = false;

      ImGui::End();
    }

    void OnClose() override
    {
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

    // A scrolling child window over [mn, mx], back at its top when `reset`;
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
      const float label_w = 320.0f * s;
      auto row = [&](const char *id, const char *label, const char *note, float &yy) {
        ImVec2 mn(x0, yy), mx(x1, yy + row_h);
        RowBack(dl, mn, mx, Eased(id, Hovering(mn, mx, live)), a);
        float ty = mn.y + (row_h - ui::FontSize(FontKind::kBody)) * 0.5f - 1.0f * s;
        if (note)
        {
          ty -= ui::FontSize(FontKind::kSmall) * 0.5f;
          ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + 20.0f * s, ty + ui::FontSize(FontKind::kBody) + 1.0f * s),
                   ui::Fade(IM_COL32(255, 190, 80, 255), a), note);
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
          row(id.c_str(), Tr(ao2::gfxmenu::SettingLabel(i)), restart ? Tr("After restart") : nullptr, y);
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

      // Field of view and mouse (quick_settings.cpp): until the game is closed.
      y = Section(dl, x0, x1, y, Tr("View and mouse"), a);
      struct SliderRow
      {
        const char *label;
        double lo, hi;
        const char *format;
        double (*get)();
        void (*set)(double);
      };
      static const SliderRow kSliders[] = {
          {"Field of view", 40.0, 130.0, "%.0f\xC2\xB0", [] { return REXCVAR_GET(ao2_fov); },
           [](double v) { REXCVAR_SET(ao2_fov, std::round(v)); }},
          {"Aiming zoom", 0.0, 1.0, "%.2f", [] { return REXCVAR_GET(ao2_fov_aim); },
           [](double v) { REXCVAR_SET(ao2_fov_aim, std::round(v * 100.0) / 100.0); }},
          {"Sensitivity", 0.1, 10.0, "%.2f", [] { return REXCVAR_GET(ao2_mouse_sensitivity); },
           [](double v) { REXCVAR_SET(ao2_mouse_sensitivity, std::round(v * 100.0) / 100.0); }},
          {"Zoomed sensitivity", 0.1, 3.0, "%.2f", [] { return REXCVAR_GET(ao2_mouse_zoom_sensitivity); },
           [](double v) { REXCVAR_SET(ao2_mouse_zoom_sensitivity, std::round(v * 100.0) / 100.0); }},
      };
      for (const SliderRow &sr : kSliders)
      {
        row(sr.label, Tr(sr.label), nullptr, y);
        double v = sr.get();
        char text[32];
        std::snprintf(text, sizeof(text), sr.format, v);
        float value_w = 70.0f * s;
        float sw = std::min(420.0f * s, x1 - 12.0f * s - value_w - (x0 + label_w));
        float sx = x1 - 12.0f * s - value_w - sw;
        if (Slider(sr.label, ImVec2(sx, y + (row_h - 28.0f * s) * 0.5f), sw, v, sr.lo, sr.hi, a, live))
        {
          sr.set(v);
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
      }
      ui::Text(dl, FontKind::kSmall, ImVec2(x0 + bw + 18.0f * s, y + 9.0f * s), ui::Fade(ui::kMuted, a),
               Tr("Field of view and mouse last until the game is closed; the rest is saved."));
      return y + 36.0f * s + 8.0f * s;
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
        bool on = c.get();
        ImVec2 mn(cx, cy), mx(cx + card_w, cy + card_h);
        ImGui::PushID(c.title);
        ImGui::SetCursorScreenPos(mn);
        bool clicked = ImGui::InvisibleButton("##card", ImVec2(card_w, card_h)) && enabled && live;
        bool hovered = enabled && live && ImGui::IsItemHovered();
        float h = Eased("hover", hovered);
        float onf = Eased("on", on, 16.0f);
        ImGui::PopID();
        if (hovered)
        {
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (clicked)
        {
          on = !on;
          c.set(on);
        }
        float ea = enabled ? a : 0.45f * a;
        dl->AddRectFilled(mn, mx, ui::Fade(ui::kWhite, (0.03f + 0.04f * h) * ea));
        dl->AddRectFilled(mn, mx, ui::Fade(ui::kGreen, 0.06f * onf * ea));
        dl->AddRect(mn, mx, ui::Fade(ui::Mix(ui::kWhite, ui::kGreen, onf), (0.10f + 0.25f * onf + 0.15f * h) * ea),
                    0.0f, ImDrawFlags_None, 1.0f * s);
        dl->AddRectFilled(mn, ImVec2(mn.x + 2.0f * s, mx.y), ui::Fade(ui::Mix(ui::kMuted, ui::kGreen, onf), ea));
        const char *title = &c == &kCheats[0] && network_ ? Tr("God mode (my character)") : Tr(c.title);
        float sw = 50.0f * s;
        dl->PushClipRect(mn, ImVec2(mx.x - sw - 30.0f * s, mx.y), true);
        ui::Text(dl, FontKind::kHeading, ImVec2(mn.x + 22.0f * s, mn.y + 18.0f * s), ui::Fade(ui::kText, ea), title);
        ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + 22.0f * s, mn.y + 22.0f * s + ui::FontSize(FontKind::kHeading)),
                 ui::Fade(ui::kMuted, ea), Tr(c.text));
        if (!enabled)
        {
          ui::Text(dl, FontKind::kSmall,
                   ImVec2(mn.x + 22.0f * s, mn.y + 26.0f * s + ui::FontSize(FontKind::kHeading) +
                                                ui::FontSize(FontKind::kSmall)),
                   ui::Fade(ui::kRed, a), Tr("Not in network games."));
        }
        dl->PopClipRect();
        Switch(dl, ImVec2(mx.x - sw - 20.0f * s, mn.y + (card_h - 26.0f * s) * 0.5f), onf, enabled, a);
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
      Scrolling("invites", mn, mx, fresh_, [&](ImVec2 o, float w) {
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
      Scrolling("missions", mn, ImVec2(mn.x + list_w, mx.y), fresh_, [&](ImVec2 o, float w) {
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
      fresh_list_ = false;
      if (fresh)
      {
        list_shown_ = ImGui::GetTime();
      }
      ImVec2 lmn(rx0, y), lmx(rx1, mx.y - hint_h);
      Scrolling("checkpoints", lmn, lmx, fresh, [&](ImVec2 o, float w) {
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
    float tab_x_ = 0.0f, tab_w_ = 0.0f, tab_x_target_ = 0.0f, tab_w_target_ = 0.0f;
    bool network_ = false, ready_ = false;
    bool close_requested_ = false;
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
      else if (IsKey(e, ao2::quick::Key()))
      {
        e.set_handled(true);
        if (!e.prev_state())
        {
          ao2::quick::Toggle();
        }
      }
      else if (IsKey(e, REXCVAR_GET(ao2_achievements_key)))
      {
        e.set_handled(true);
        if (!e.prev_state())
        {
          ao2::ach::ToggleMenu();
        }
      }
      else if (IsKey(e, REXCVAR_GET(ao2_players_key)))
      {
        e.set_handled(true);
        if (!e.prev_state())
        {
          ao2::invite::TogglePlayers();
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
