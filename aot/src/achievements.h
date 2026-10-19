// achievements.h - The game's achievements in its language, by section, for
// the Achievements page and the launcher (see achievements.cpp).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ao2::ach
{
  // After the game's language is set (OnPostSetup, after lang::Install): the
  // runtime's catalog in that language, and the catalog with every language
  // and the icons written to <cache_root>\achievements for the launcher.
  void Setup(const std::filesystem::path &cache_root);

  struct Item
  {
    uint32_t id = 0;
    std::string label;       // UTF-8, in the game's language
    std::string description; // what it is for; while locked a secret one's says only that it is secret
    uint32_t gamerscore = 0;
    bool unlocked = false;
  };

  constexpr int kSectionCount = 7;
  constexpr int kSectionSize = 7; // at most, the page's rows

  // A section's name, English (ui_text.cpp translates it).
  const char *SectionName(int section);

  // A section's achievements, in order, as they stand now.
  std::vector<Item> Section(int section);

  struct Totals
  {
    int unlocked = 0, count = 0;
    int gamerscore = 0, max_gamerscore = 0;
  };
  Totals Count(int section = -1); // -1: all of them
} // namespace ao2::ach
