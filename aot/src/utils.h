// utils.h - Utility functions for path handling and config loading
#pragma once

#include <filesystem>

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
  }
} // namespace utils
