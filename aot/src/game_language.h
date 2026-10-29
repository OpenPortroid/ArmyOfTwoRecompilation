// game_language.h - The game's language (see game_language.cpp).
#pragma once

#include <filesystem>

namespace rex
{
  class Runtime;
}

namespace ao2::lang
{
  // The language the game runs in (ao2_game_language): "en", "ru", "pt",
  // "fr", "de", "es", "it"; "auto" is Windows' language when it is one of those,
  // else "en".
  const char *Code();

  // Whether the game runs in Russian (the disc's English with our text).
  bool Russian();

  // Whether the game runs in a language with our text on the disc's English
  // (Russian, Brazilian Portuguese).
  bool OwnLanguage();

  // Before the game starts (OnPostSetup): sets the console's language the game
  // reads (the SDK's user_language), writes the game's localization (the
  // Russian or Portuguese text, keyboard_prompts' rewrite) under `user_data_root` and mounts
  // it over game:\AO2Game\Localization; in Russian also starts drawing the
  // fonts' Cyrillic letters.
  void Install(rex::Runtime *runtime, const std::filesystem::path &game_data_root,
               const std::filesystem::path &user_data_root);
} // namespace ao2::lang
