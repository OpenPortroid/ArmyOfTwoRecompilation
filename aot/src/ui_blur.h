// ui_blur.h - A menu blur left on after its message box closed (see ui_blur.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace ao2::ui_blur
{
  // Game thread, once per frame (engine pad polling).
  void Tick(PPCContext &ctx, uint8_t *base);
}
