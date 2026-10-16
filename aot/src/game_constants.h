// game_constants.h - Game-specific constants
#pragma once

#include <cstdint>

namespace GameConstants
{
  constexpr uint32_t kCodeBase = 0x82210000;
  constexpr uint32_t kCodeEnd = 0x83169604;
}

namespace GameConstants::PatchConstants
{
  struct Patch
  {
    std::uintptr_t address;
    std::uint32_t value;
  };

  // "Black Shading Fix" - disables MSAA.
  constexpr Patch DisableMsaa()
  {
    return Patch{
        0x831876D7,
        0x04};
  }
}
