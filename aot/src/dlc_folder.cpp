// dlc_folder.cpp - The downloadable content in the game's folder.
//
// The SDK keeps content packages under the user data folder
// (<user data>\<xuid>\<title>\<type>\<package>), and so did the port: the DLC
// sat in Documents\armyoftworecomp_tu2\0000000000000000\4541084C\00000002.
// Now it is <game>\dlc\<package>, next to the game files (assets\): the SDK's
// marketplace_content_root makes every player's and title's marketplace
// content come from there. Saves and the game profile stay in Documents.
//
// Packages left at the old place (installed by an earlier version) are moved
// here when the game starts. The launcher does the same when it starts (the
// usual way after an update); both take the mutex AO2_DlcMove, so the game
// waits for the launcher's move to end. A package moves by rename on the same
// drive, else is copied to <package>.tmp, renamed and only then deleted at the
// old place, so a package in dlc\ is always whole: one already there wins and
// the old copy is deleted (an empty folder of its name is no package and is
// replaced).
// If dlc\ cannot be written (the game in a protected folder) and nothing is in
// it, the game keeps reading the old place.

#include "dlc_folder.h"

#include <string>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "utils.h"

namespace fs = std::filesystem;

namespace ao2::dlc
{
  namespace
  {
    const char *kCvar = "marketplace_content_root";

    fs::path g_dir; // where the packages are read from

    // Package folders in dir; the launcher's unfinished ones (<package>.tmp) are not.
    int CountPackages(const fs::path &dir)
    {
      int n = 0;
      std::error_code ec;
      for (const auto &e : fs::directory_iterator(dir, ec))
      {
        if (e.is_directory(ec) && e.path().extension() != ".tmp")
          ++n;
      }
      return n;
    }

    bool MovePackage(const fs::path &from, const fs::path &to)
    {
      std::error_code ec;
      fs::rename(from, to, ec);
      if (!ec)
        return true;
      // Another drive: copy, then delete the old one.
      fs::path part = to;
      part += ".tmp";
      fs::remove_all(part, ec);
      fs::copy(from, part, fs::copy_options::recursive, ec);
      if (!ec)
        fs::rename(part, to, ec);
      if (ec)
      {
        REXLOG_WARN("dlc: cannot move {} to {}: {}", rex::path_to_utf8(from), rex::path_to_utf8(to), ec.message());
        std::error_code ec2;
        fs::remove_all(part, ec2);
        return false;
      }
      fs::remove_all(from, ec);
      return true;
    }

    // Removes dir and its parents up to (not including) stop while they are empty.
    void RemoveEmpty(fs::path dir, const fs::path &stop)
    {
      std::error_code ec;
      while (dir != stop && dir.has_parent_path() && fs::is_empty(dir, ec) && !ec)
      {
        fs::remove(dir, ec);
        dir = dir.parent_path();
      }
    }

    void MoveOld(const fs::path &old_dir, const fs::path &dir, const fs::path &user_data_root)
    {
      std::error_code ec;
      if (!fs::is_directory(old_dir, ec))
        return;
      int moved = 0, kept = 0;
      for (const auto &e : fs::directory_iterator(old_dir, ec))
      {
        std::error_code ec2;
        if (!e.is_directory(ec2) || e.path().extension() == ".tmp")
          continue;
        fs::path to = dir / e.path().filename();
        bool there = fs::exists(to, ec2);
        std::error_code ec3;
        if (there && fs::is_directory(to, ec3) && !fs::is_empty(to, ec3) && !ec3)
        {
          // Already here (installed again, or a move cut short after the rename): the old copy
          // is a duplicate.
          fs::remove_all(e.path(), ec2);
          if (ec2)
            ++kept;
          else
            ++moved;
          continue;
        }
        if (there)
        {
          // An empty folder (or a file) of that name is no package: the old copy takes its place.
          fs::remove(to, ec2);
          if (ec2)
          {
            ++kept;
            continue;
          }
        }
        fs::create_directories(dir, ec2);
        if (MovePackage(e.path(), to))
          ++moved;
        else
          ++kept;
      }
      RemoveEmpty(old_dir, user_data_root);
      if (moved || kept)
        REXLOG_INFO("dlc: {} package(s) moved (or already there) from {} to {}, {} left there", moved,
                    rex::path_to_utf8(old_dir), rex::path_to_utf8(dir), kept);
    }
  } // namespace

  fs::path Dir()
  {
    return g_dir;
  }

  void Setup(const fs::path &user_data_root)
  {
    if (std::string root = rex::cvar::GetFlagByName(kCvar); !root.empty())
    {
      g_dir = rex::to_path(root); // set on the command line: the user's choice
      return;
    }
    fs::path dir = utils::RepoRoot() / "dlc";
    fs::path old_dir = user_data_root / "0000000000000000" / "4541084C" / "00000002";

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"AO2_DlcMove");
    if (mutex)
      WaitForSingleObject(mutex, INFINITE);
    MoveOld(old_dir, dir, user_data_root);
    if (mutex)
    {
      ReleaseMutex(mutex);
      CloseHandle(mutex);
    }

    if (CountPackages(dir) == 0 && CountPackages(old_dir) > 0)
    {
      REXLOG_WARN("dlc: {} cannot be used, the downloadable content stays in {}", rex::path_to_utf8(dir),
                  rex::path_to_utf8(old_dir));
      g_dir = old_dir;
      return;
    }
    g_dir = dir;
    rex::cvar::SetFlagByName(kCvar, rex::path_to_utf8(dir));
  }
} // namespace ao2::dlc
