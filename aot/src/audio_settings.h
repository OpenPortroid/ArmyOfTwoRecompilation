// audio_settings.h - The F1 menu's volumes (audio_settings.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace ao2::audio_settings
{
  // Every game thread pad poll: applies the volumes that changed.
  void Tick(PPCContext &ctx, uint8_t *base);
} // namespace ao2::audio_settings
