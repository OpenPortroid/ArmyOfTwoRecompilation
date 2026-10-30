// utils.h - Utility functions for path handling and config loading
#pragma once

#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>

namespace utils
{
  // The folder with settings\, localization\ and assets\: the executable's own
  // in a release, the repository (aot\) for a build in out\build\<preset>.
  // Found by settings\ (or the manifest), not by assets\: the launcher's
  // install screen can put an assets\ next to a build's executable, and the
  // game would lose its key bindings and text. The launcher looks the same way
  // (Installer.DataRoot).
  static std::filesystem::path RepoRoot()
  {
    namespace fs = std::filesystem;
    const fs::path exe = rex::filesystem::GetExecutableFolder();
    std::error_code ec;
    fs::path dir = exe;
    for (int i = 0; i < 6 && !dir.empty(); ++i)
    {
      if (fs::exists(dir / "armyoftworecomp_manifest.toml", ec) || fs::is_directory(dir / "settings", ec))
      {
        return dir;
      }
      if (!dir.has_parent_path() || dir.parent_path() == dir)
      {
        break;
      }
      dir = dir.parent_path();
    }
    return exe;
  }

  // Get the settings directory path
  static std::filesystem::path SettingsDir() { return RepoRoot() / "settings"; }

  // Every keyboard bind and its default (settings/mapping.toml, the launcher's
  // Bindings.cs).
  inline constexpr const char *kDefaultBinds[][2] = {
      {"a", "E,Return"},       {"b", "Q,Backspace"},          {"x", "R"},
      {"y", "Space"},          {"left_trigger", "RMB"},       {"right_trigger", "LMB"},
      {"left_shoulder", "Z,1,2,3,4,G"}, {"right_shoulder", "F"}, {"lstick_up", "W"},
      {"lstick_down", "S"},    {"lstick_left", "A"},          {"lstick_right", "D"},
      {"lstick_press", "Control,C"}, {"rstick_up", ""},       {"rstick_down", ""},
      {"rstick_left", ""},     {"rstick_right", ""},          {"rstick_press", "MMB"},
      {"dpad_up", "Up,1"},     {"dpad_down", "Down,4,G"},     {"dpad_left", "Left,2"},
      {"dpad_right", "Right,3"}, {"back", "Tab"},             {"start", "Escape"},
      {"guide", ""},
  };

  // Without mapping.toml (the executable moved away from its settings\) the
  // SDK's defaults leave the keyboard and mouse off: only the menus' Enter and
  // Esc answer. The keyboard gets mapping.toml's values instead; a flag (the
  // launcher passes every keybind_*) still wins.
  static void ApplyInputDefaults()
  {
    auto set = [](const std::string &name, const char *value) {
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault)
      {
        rex::cvar::SetFlagByName(name, value);
      }
    };
    set("mnk_mode", "true");
    set("mnk_mouse", "true");
    set("mnk_mouse_to_stick", "false");
    for (const auto &entry : kDefaultBinds)
    {
      // A flag needs a value: "none" is the launcher's empty bind.
      set(std::string("keybind_") + entry[0], entry[1][0] ? entry[1] : "none");
    }
  }

  // Load all settings files from the settings directory
  static void LoadSettingsFiles()
  {
    namespace fs = std::filesystem;
    fs::path dir = SettingsDir();
    for (const char *file : {"hardware.toml", "mapping.toml", "online.toml"})
    {
      if (fs::path p = dir / file; fs::exists(p))
      {
        rex::cvar::LoadConfig(p);
      }
    }
    if (!fs::exists(dir / "mapping.toml"))
    {
      ApplyInputDefaults();
    }
  }
} // namespace utils
