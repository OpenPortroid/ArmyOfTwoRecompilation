// achievements.h - The game's achievements in its language, and for the
// launcher (see achievements.cpp).
#pragma once

#include <filesystem>

#include <imgui.h>

namespace ao2::ach
{
  // After the game's language is set (OnPostSetup, after lang::Install): the
  // runtime's catalog in that language, and the catalog with every language
  // and the icons written to <cache_root>\achievements for the launcher.
  void Setup(const std::filesystem::path &cache_root);

  // Any thread: how many achievements are unlocked, of how many.
  void Progress(int &got, int &total);

  // UI thread, inside an ImGui window (the F1 debug menu's tab): the
  // achievements' counts and list in [min, max]; `appearing` starts it over (its fade-in,
  // the scroll at the top).
  void DrawTab(ImVec2 min, ImVec2 max, bool appearing);
  // Moves the tab's list (the gamepad; the next DrawTab takes it).
  void ScrollBy(float pixels);
} // namespace ao2::ach
