// fov.h - Field of view settings for the drawn view (fov.cpp).
#pragma once

#include <cstdint>

namespace ao2::fov
{
  // The field of view the player's view is drawn with (degrees) for the
  // game's `game_fov`: ao2_fov on foot, the zoom narrowed by ao2_fov_aim,
  // the sniper scope left alone. The game's own when the settings are the
  // game's. Any thread.
  float Drawn(float game_fov);

  // CalcSceneView (interp_camera.cpp, game thread): the player's view of
  // `controller` is drawn with `drawn` degrees for the game's `game`. Sets
  // the lens shift of its projection (the next FPerspectiveMatrix on this
  // thread) that keeps the crosshair over the direction the shot goes.
  void NoteView(const uint8_t *base, uint32_t controller, float game, float drawn);
  // FPerspectiveMatrix (aspect_ratio.cpp): the lens shift to add to the
  // matrix being built (clip-space x and y per unit of depth), once; false
  // when there is none.
  bool TakeLensShift(float &x, float &y);
  // The player's projection as drawn against the game's (for scene captures
  // of the player's view, interp_camera.cpp): clip x and y scaled by `keep`,
  // then the lens shift added. keep 1 and no shift for the game's view. Game
  // thread, in CalcSceneView after the FOV was taken.
  void Projection(float &keep, float &x, float &y);
  // End of CalcSceneView: a shift no projection took is dropped.
  void ClearLensShift();
} // namespace ao2::fov
