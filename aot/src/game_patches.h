// game_patches.h - Game-specific memory patches
#pragma once

#include <algorithm>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

#include "game_cvars.h"
#include "game_constants.h"

namespace game_patches
{
  // Ported from xenia-canary game-patches for Army of Two (4541084C)
  // https://github.com/xenia-canary/game-patches/blob/main/patches/4541084C%20-%20Army%20of%20Two%20(Europe).patch.toml
  // Only data patches live here. "Unlock FPS" and "16x Anisotropic Filtering"
  // patch instructions, which are already compiled to C++; they are applied
  // through mid-asm hooks instead (midasm_hooks.toml, src/midasm_hooks.cpp).

  // "Black Shading Fix" - disables MSAA (always: the game's MSAA is not offered).
  static void DisableMsaa()
  {
    auto *rt = rex::Runtime::instance();
    uint8_t *base = rt ? rt->virtual_membase() : nullptr;
    if (!base)
    {
      return;
    }
    auto memoryAddr = GameConstants::PatchConstants::DisableMsaa().address;
    auto memoryValue = GameConstants::PatchConstants::DisableMsaa().value;
    uint8_t *p = base + memoryAddr;

    rex::memory::PageAccess old_access{};
    rex::memory::Protect(p, 1, rex::memory::PageAccess::kReadWrite, &old_access);
    p[0] = static_cast<uint8_t>(memoryValue);
    rex::memory::Protect(p, 1, old_access, nullptr);
  }
} // namespace game_patches
