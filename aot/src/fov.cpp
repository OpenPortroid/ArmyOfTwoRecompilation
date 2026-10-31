// fov.cpp - Field of view of the drawn view.
//
// The game sets its camera's field of view (AAO2CameraActor +460, degrees):
// on foot 70, precision aim 40, back to back 60/30, the sniper scope ~22.
// The narrow aim made some players sick, and on a wide screen 70 is narrow
// too. Only the drawn view changes: interp_camera.cpp passes the FOV that
// ULocalPlayer::CalcSceneView takes (sub_823E06D8) through Drawn(). The
// simulation keeps the game's FOV (the camera actor is in the lockstep
// checksum, the AI may read it); what the drawn view changes in the world
// on its own machine (skeletal LOD, LastRenderTime) is made alike in co-op
// by coop_lod.cpp, as for different aspect ratios.
//
// - ao2_fov: the on-foot FOV (70 = the game's). Every FOV is scaled by it in
//   tangent space, tan(fov' / 2) = tan(fov / 2) x tan(ao2_fov / 2) /
//   tan(70 / 2), so a zoom magnifies by the same factor as in the game.
// - ao2_fov_aim: how much of the zoom's narrowing is kept, 1 = the game's,
//   0 = none: fov' = 70 - (70 - fov) x this, before the scaling above.
// - Under ao2_fov_scope degrees (the sniper scope, its overlay drawn for the
//   game's angle) the FOV is left as it is.
//
// Cutscenes and the front end keep the game's FOV (interp_camera.cpp).
// Mouse aiming follows the drawn FOV (mouse_aim.cpp ZoomScale).
#include "fov.h"

#include <cmath>

#include <rex/cvar.h>

REXCVAR_DEFINE_DOUBLE(ao2_fov, 70.0, "Gameplay",
                      "Field of view on foot in degrees (the game's: 70); zoomed views are widened in proportion. "
                      "Only the drawn view, the game state is not changed")
    .range(40.0, 130.0);
REXCVAR_DEFINE_DOUBLE(ao2_fov_aim, 1.0, "Gameplay",
                      "How much aiming narrows the field of view: 1 = as the game (precision aim 40 deg), "
                      "0.5 = half as much, 0 = not at all. The sniper scope is not changed")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(ao2_fov_scope, 25.0, "Gameplay",
                      "The game's field of view under this many degrees is a scope: ao2_fov and ao2_fov_aim leave "
                      "it alone")
    .range(0.0, 70.0);

namespace ao2::fov
{
  namespace
  {
    constexpr double kGameFov = 70.0;
    constexpr double kHalfDegree = 3.14159265358979323846 / 360.0;
  } // namespace

  float Drawn(float game_fov)
  {
    double hip = REXCVAR_GET(ao2_fov);
    double aim = REXCVAR_GET(ao2_fov_aim);
    if ((hip == kGameFov && aim == 1.0) || !(game_fov > 1.0f && game_fov < 170.0f) ||
        game_fov < REXCVAR_GET(ao2_fov_scope))
    {
      return game_fov;
    }
    double fov = game_fov;
    if (fov < kGameFov)
    {
      fov = kGameFov - (kGameFov - fov) * aim;
    }
    double t = std::tan(fov * kHalfDegree) * std::tan(hip * kHalfDegree) / std::tan(kGameFov * kHalfDegree);
    return float(std::atan(t) / kHalfDegree);
  }
} // namespace ao2::fov
