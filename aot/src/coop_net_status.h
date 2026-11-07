// coop_net_status.h - Online co-op connection quality and drops (see coop_net_status.cpp).
#pragma once

#include <cstdint>

namespace ao2::coop::status
{
  // At startup (after ao2::invite::Start): the thread that keeps the link
  // pumped and notices a partner who dropped out.
  void Start();

  // Game thread, after every frame sync tick (frame_time.cpp), `sync` the
  // frame sync object.
  void OnSyncTick(const uint8_t *base, uint32_t sync);

  // Any thread (each present): the connection overlay has something to show.
  bool HudWanted();
  // UI thread, inside an ImGui frame of the overlay layer: the overlay.
  void DrawHud();
} // namespace ao2::coop::status
