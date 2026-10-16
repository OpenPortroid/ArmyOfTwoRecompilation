// cyrillic_fonts.h - Russian letters in the game's fonts (see cyrillic_fonts.cpp).
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ao2::cyrillic
{
  // At startup, when the game runs in Russian: reads the fonts from the disc's
  // packages and draws their Cyrillic letters on a worker thread.
  void Install(const std::filesystem::path &game_data_root, const std::filesystem::path &user_data_root);

  // Game thread, every frame: moves the fonts' characters À-ÿ, ¨ and ¸ (the
  // Russian text's CP1251 bytes) to the new letters once a font is loaded.
  void Tick(uint8_t *base);

  // Renderer: the guest bytes to look at for a texture of this shape, 0 if no
  // font page has it.
  uint32_t PageBytes(uint32_t format, uint32_t width, uint32_t height);
  // The font page these bytes are (-1 if none).
  int PageOf(uint32_t format, uint32_t width, uint32_t height, const uint8_t *guest_data, uint32_t bytes);
  // The page with its Cyrillic letters: RGBA8, straight alpha.
  bool BuildPage(int page, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height);
} // namespace ao2::cyrillic
