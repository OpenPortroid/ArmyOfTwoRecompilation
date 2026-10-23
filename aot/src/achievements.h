// achievements.h - The game's achievements in its language, and for the
// launcher (see achievements.cpp).
#pragma once

#include <filesystem>

namespace ao2::ach
{
  // After the game's language is set (OnPostSetup, after lang::Install): the
  // runtime's catalog in that language, and the catalog with every language
  // and the icons written to <cache_root>\achievements for the launcher.
  void Setup(const std::filesystem::path &cache_root);

  // Any thread (ao2_achievements_key, F7): opens the achievements menu, or
  // closes it when it is open.
  void ToggleMenu();
} // namespace ao2::ach
