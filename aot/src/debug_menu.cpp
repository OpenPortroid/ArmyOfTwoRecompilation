// debug_menu.cpp - The F1 debug menu: god mode, load any checkpoint.
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
// - God mode: the ao2_god_mode cvar (cheats.cpp).
// - The frame rate overlay's mode (perf_overlay.h; also its own key, F2).
// - Checkpoints: the files in AO2Game/Checkpoints and in the extracted DLC
//   packages (Kiev 09_*, Miami alternate ending 10_*; the name of each,
//   then its level), loaded with the console's "checkpoint <name> <difficulty>"
//   (console_cmd.cpp). The loaded checkpoint becomes the profile's current
//   one when the game saves it.
//
// Online co-op is a lockstep simulation (coop.h): there both go through the
// lockstep input to both machines (coop_cmd.cpp) - god mode for this
// player, the checkpoint for both; only while a level runs.
//
// The frame rate overlay (native renderer only) is a dialog of a second
// ImGuiDrawer drawing into the overlay's lower layer. That drawer is no input
// listener (it would capture the mouse on clicks): the overlay takes no input.
#include "debug_menu.h"

#include "console_cmd.h"
#include "coop.h"
#include "coop_cmd.h"
#include "coop_invite.h"
#include "dlc_folder.h"
#include "gpu/native/overlay.h"
#include "perf_overlay.h"
#include "ui_text.h"
#include "ui_theme.h"

#include <algorithm>
#include <cctype>
#include <atomic>
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
REXCVAR_DECLARE(int32_t, ao2_start_difficulty);
REXCVAR_DECLARE(std::string, ao2_players_key);
REXCVAR_DECLARE(std::string, ao2_invite_accept_key);

REXCVAR_DEFINE_STRING(ao2_debug_menu_key, "F1", "Debug",
                      "Key that opens the debug menu (god mode, checkpoints); empty = none");
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
  std::atomic<bool> g_notice_on{false};

  // The game gets no input: the menu or a port window is open.
  bool Blocking()
  {
    return g_open.load(std::memory_order_relaxed) || g_modal.load(std::memory_order_relaxed) > 0;
  }
  // Native renderer: UI frames are wanted / one is queued to the UI thread.
  std::atomic<bool> g_frames_wanted{false}, g_frame_queued{false};

  class DebugMenu final : public rex::ui::ImGuiDialog
  {
  public:
    explicit DebugMenu(rex::ui::ImGuiDrawer *drawer) : ImGuiDialog(drawer)
    {
      missions_ = ScanCheckpoints(g_checkpoint_dir, g_content_dir);
      difficulty_ = std::clamp(REXCVAR_GET(ao2_start_difficulty), 0, 2);
    }

  protected:
    void OnDraw(ImGuiIO &io) override
    {
      const ImGuiViewport *viewport = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowSize(ImVec2(560.0f, std::min(680.0f, io.DisplaySize.y * 0.9f)), ImGuiCond_Appearing);
      bool open = true;
      std::string title = std::string(Tr("Debug menu")) + "###debug_menu";
      if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse))
      {
        const uint8_t *base = GuestBase();
        bool network = base && ao2::coop::cmd::Routed(base);
        bool ready = base && ao2::coop::cmd::Ready(base);
        if (network)
        {
          ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                             ready ? Tr("Online co-op: actions go to both players.")
                                   : Tr("Online co-op: available while a level is running."));
        }

        bool god = REXCVAR_GET(ao2_god_mode);
        if (ImGui::Checkbox(network ? Tr("God mode (my character)") : Tr("God mode"), &god))
        {
          REXCVAR_SET(ao2_god_mode, god);
        }

        ImGui::SeparatorText(Tr("FPS counter"));
        int mode = ao2::perf::GetMode();
        bool changed = ImGui::RadioButton(Tr("Off"), &mode, ao2::perf::kOff);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("FPS", &mode, ao2::perf::kFps);
        ImGui::SameLine();
        changed |= ImGui::RadioButton(Tr("Detailed"), &mode, ao2::perf::kDetailed);
        if (changed)
        {
          ao2::perf::SetMode(mode);
        }
        std::string key = REXCVAR_GET(ao2_fps_overlay_key);
        if (!g_perf_drawer)
        {
          ImGui::TextDisabled("%s", Tr("Native renderer only (ao2_native_gpu)."));
        }
        else if (!key.empty())
        {
          ImGui::TextDisabled(Tr("%s toggles it in game too. Detailed: frame time,"), key.c_str());
          ImGui::TextDisabled("%s", Tr("GPU load and temperature, CPU load, video and system memory."));
        }

        ImGui::SeparatorText(Tr("Load checkpoint"));
        static const char *kDifficulties[] = {"Recruit", "Contractor", "Professional"};
        ImGui::SetNextItemWidth(200.0f);
        ImGui::Combo(Tr("Difficulty"), &difficulty_, kDifficulties, IM_ARRAYSIZE(kDifficulties));

        float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 2.0f;
        bool load = false;
        if (ImGui::BeginChild("checkpoints", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders))
        {
          if (missions_.empty())
          {
            ImGui::TextDisabled(Tr("No files in %s"), g_checkpoint_dir.string().c_str());
          }
          for (const Mission &mission : missions_)
          {
            const char *title = MissionTitle(mission.prefix);
            std::string header = title ? mission.prefix + "  " + title : Tr("Mission ") + mission.prefix;
            if (!ImGui::CollapsingHeader(header.c_str()))
            {
              continue;
            }
            for (const Checkpoint &checkpoint : mission.checkpoints)
            {
              std::string label = checkpoint.name;
              if (checkpoint.name.ends_with("_100"))
              {
                label += Tr("  (start)");
              }
              if (!checkpoint.level.empty())
              {
                label += "    " + checkpoint.level;
              }
              ImGui::PushID(checkpoint.name.c_str());
              if (ImGui::Selectable(label.c_str(), selected_ == checkpoint.name,
                                    ImGuiSelectableFlags_AllowDoubleClick))
              {
                selected_ = checkpoint.name;
                load = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
              }
              ImGui::PopID();
            }
          }
        }
        ImGui::EndChild();

        bool can_load = !network || ready;
        ImGui::BeginDisabled(selected_.empty() || !can_load);
        std::string load_label = selected_.empty() ? Tr("Load") : std::string(Tr("Load")) + " " + selected_;
        load |= ImGui::Button(load_label.c_str());
        ImGui::EndDisabled();
        if (network)
        {
          ImGui::TextDisabled("%s", Tr("Loads for both. Your partner needs the same"));
          ImGui::TextDisabled("%s", Tr("files (a DLC checkpoint without the DLC breaks his game)."));
        }
        else
        {
          ImGui::TextDisabled("%s", Tr("Double-click loads too. Saving the game writes"));
          ImGui::TextDisabled("%s", Tr("the loaded checkpoint to the profile as the current one."));
        }

        if (load && can_load && !selected_.empty())
        {
          std::string line = "checkpoint " + selected_ + " " + std::to_string(difficulty_);
          if (network)
          {
            ao2::coop::cmd::Send(line);
          }
          else
          {
            ao2::console::Queue(line);
          }
          open = false;
        }
      }
      ImGui::End();
      if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      {
        Close();
      }
    }

    void OnClose() override
    {
      g_menu = nullptr;
      g_open = false;
    }

  private:
    std::vector<Mission> missions_;
    std::string selected_;
    int difficulty_ = 0;
  };

  void Toggle()
  {
    if (!g_drawer)
    {
      return;
    }
    if (g_menu)
    {
      delete g_menu;
      g_menu = nullptr;
      g_open = false;
      if (g_own_drawer)
      {
        ao2::gpu::overlay::Clear(ao2::gpu::overlay::kMenuLayer);
      }
      return;
    }
    g_menu = new DebugMenu(g_drawer);
    g_open = true;
    g_frames_wanted = g_own_drawer != nullptr;
  }

  // UI thread: draws the notice (SetNotice); clears it once it is over.
  void DrawNotice()
  {
    std::function<bool()> draw;
    {
      std::lock_guard lock(g_notice_mutex);
      draw = g_notice;
    }
    if (draw && !draw())
    {
      std::lock_guard lock(g_notice_mutex);
      g_notice = nullptr;
      g_notice_on = false;
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
  void Attach(rex::ui::Window *window, rex::ui::ImGuiDrawer *sdk_drawer, rex::input::InputSystem *input,
              const std::filesystem::path &game_data_root)
  {
    if (!window || g_window)
    {
      return;
    }
    g_window = window;
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

  void SetNotice(std::function<bool()> draw)
  {
    {
      std::lock_guard lock(g_notice_mutex);
      g_notice = std::move(draw);
      g_notice_on = g_notice != nullptr;
    }
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
