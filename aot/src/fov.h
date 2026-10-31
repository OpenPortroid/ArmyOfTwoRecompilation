// fov.h - Field of view settings for the drawn view (fov.cpp).
#pragma once

namespace ao2::fov
{
  // The field of view the player's view is drawn with (degrees) for the
  // game's `game_fov`: ao2_fov on foot, the zoom narrowed by ao2_fov_aim,
  // the sniper scope left alone. The game's own when the settings are the
  // game's. Any thread.
  float Drawn(float game_fov);
} // namespace ao2::fov
