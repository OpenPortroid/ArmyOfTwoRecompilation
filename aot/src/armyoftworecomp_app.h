// armyoftworecomp - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>

#include "aspect_ratio.h"
#include "debug_menu.h"
#include "debug_tools.h"
#include "dlc_folder.h"
#include "coop_invite.h"
#include "ea_server.h"
#include "game_cvars.h"
#include "hang_watchdog.h"
#include "game_language.h"
#include "graphics_menu.h"
#include "menu_mouse.h"
#include "gpu/native/native_graphics_system.h"
#include "gpu/native/renderer.h"
#include "game_patches.h"
#include "cutscene_skip.h"
#include "mouse_aim.h"
#include "ui_text.h"
#include "utils.h"

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
#include <rex/perf/counter.h>
#endif

REXCVAR_DECLARE(std::string, ao2_native_gpu);

class ArmyoftworecompApp : public rex::ReXApp
{
public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext &ctx)
  {
    return std::unique_ptr<ArmyoftworecompApp>(new ArmyoftworecompApp(ctx, "armyoftworecomp",
                                                                                  PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig &config) override
  {
    if (REXCVAR_GET(ao2_native_gpu) == "on")
    {
      // Native D3D12 renderer: the Xenos emulation plugin is never loaded.
      config.graphics = ao2::gpu::CreateNativeGraphicsSystem();
      return;
    }
    std::string backend = REXCVAR_GET(graphics_backend);
    if (backend != "any" && !config.gpu_plugin.empty())
    {
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, backend);
      if (!config.graphics)
      {
        REXLOG_WARN("graphics_backend '{}' unavailable, falling back to automatic selection",
                    backend);
      }
    }
  }

  // void OnLoadXexImage(std::string& xex_image) override {}
  void OnPostLoadXexImage() override
  {
    game_patches::DisableMsaa();
  }

  void OnPostSetup() override
  {
    utils::LoadSettingsFiles();
    // After the runtime's own setup, so its filter does not replace ours.
    ao2::hang::InstallCrashHandler();
    // The port's own menus speak the game's language.
    ao2::text::Init();

    // The downloadable content: in the game's folder (moved there from the
    // user data folder of earlier versions).
    ao2::dlc::Setup(user_data_root());

    // The game's text (Russian, keyboard keys in place of the controller
    // buttons): mounted before the game reads its localization.
    ao2::lang::Install(runtime(), game_data_root(), user_data_root());

    // Online co-op: where the game finds EA's (emulated) servers, and the
    // built-in server when this player hosts.
    ao2::ea::Start();
    // Invites (Private Co-op): finds the other players, carries invites.
    ao2::invite::Start();

    if (window())
    {
      ao2::gpu::SetPresentWindow(window()->GetNativeWindowHandle());
      ao2::aspect::SetWindow(window()->GetNativeWindowHandle());
      ao2::mouse::Attach(window());
      ao2::skip::Attach(window());
      ao2::gfxmenu::Attach(window());
      ao2::menumouse::Attach(window());
      ao2::menu::Attach(window(), imgui_drawer(),
                        static_cast<rex::input::InputSystem *>(runtime()->input_system()), game_data_root());
      if (!window()->IsFullscreen())
      {
        // The default window (1280x720 logical at the display's DPI) can be
        // larger than the screen, which crops the picture.
        ao2::gpu::FitWindowToMonitor(window()->GetNativeWindowHandle());
      }
    }

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    // SDK exposes the perf_log_csv cvar but never wires it up; do it here so
    // --perf_log_csv=<path> actually produces per-frame CSV output.
    // Use GetFlagByName to avoid needing a cross-DLL declaration of the cvar.
    rex::perf::SetCsvLogPath(rex::cvar::GetFlagByName("perf_log_csv"));
#endif

    // Run debug tools only if enabled via hardware.toml (dev_debug_runtime = true)
    if (REXCVAR_GET(dev_debug_runtime))
    {
      debug_tools::PerformMissingFunctionScan();
      debug_tools::PerformStubSweep();
    }
  }

  void OnConfigureFonts(ImFontAtlas *atlas) override
  {
    ao2::menu::ConfigureFonts(atlas);
  }

  void OnShutdown() override
  {
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    rex::perf::FlushCsv();
#endif
  }

  void OnConfigurePaths(rex::PathConfig &paths) override
  {
    // --game_data_root / REX_GAME_DATA_ROOT still win: this cvar is read
    // before any config file loads, so it's already non-empty here if set.
    if (paths.game_data_root.empty())
    {
      paths.game_data_root = utils::RepoRoot() / "assets";
    }

    // TU2 keeps its own saves and profile next to the disc version's
    // (Documents\armyoftworecomp); the downloadable content is in the game's
    // folder (dlc_folder.cpp). --user_data_root / --cache_root still win.
    if (REXCVAR_GET(user_data_root).empty())
    {
      paths.user_data_root = paths.user_data_root.parent_path() / "armyoftworecomp_tu2";
      if (REXCVAR_GET(cache_root).empty())
      {
        paths.cache_root = paths.user_data_root / "cache";
      }
    }

    // Hardware/rendering settings become the SDK's primary config file, so
    // the in-game Settings overlay's "Save to config" also lands here.
    paths.config_path = utils::SettingsDir() / "hardware.toml";

    // Input mapping lives in its own file, loaded manually below (the SDK
    // only auto-loads the single path above).
    utils::LoadSettingsFiles();
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
