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
//
// The crosshair: out of the aim (from the hip) it is not at the screen
// centre but at a point of the viewport the character's camera attributes
// give (AO2PlayerControllerNative.HUDControlParameters: ReticulePosition 7 =
// viewport location, ReticuleViewportLocation; AO2PlayerController.PlayerTick
// moves it towards the wanted one). The shot goes through that point of the
// game's view (CalcCameraSpaceAimingRotation), at a fixed angle off the
// camera's axis; a wider drawn view shows that direction nearer the centre,
// under the crosshair would be a point above where the bullets go. So the
// drawn projection is shifted (off-centre, like a lens shift) by
// c x (1 - tan(game / 2) / tan(drawn / 2)) in clip space, c = the crosshair's
// clip-space position: the direction of the shot is again under the
// crosshair, whatever the drawn FOV. At the centre (the aim) nothing moves.
#include "fov.h"

#include "log_files.h"
#include "ue3_reflect.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

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
REXCVAR_DEFINE_BOOL(ao2_fov_crosshair_fix, true, "Gameplay",
                    "With a field of view other than the game's: shift the picture so that the hip-fire crosshair "
                    "stays over where the shots go");
REXCVAR_DEFINE_INT32(ao2_fov_debug, 0, "Debug", "Log this many views (crosshair, lens shift) to fov.log");

namespace ao2::fov
{
  namespace
  {
    constexpr double kGameFov = 70.0;
    constexpr double kHalfDegree = 3.14159265358979323846 / 360.0;
    constexpr uint8_t kReticleInViewport = 7; // CROSSHAIRPOSITION_ViewportLocation

    struct Shift
    {
      bool on = false;
      float x = 0.0f, y = 0.0f;
    };
    thread_local Shift t_shift;

    float LoadF32(const uint8_t *base, uint32_t addr)
    {
      uint32_t v;
      std::memcpy(&v, base + addr, 4);
      return std::bit_cast<float>(std::byteswap(v));
    }

    void Debug(const char *fmt, double a, double b, double c, double d, double e, double f)
    {
      static int left = -1;
      if (left < 0)
      {
        left = REXCVAR_GET(ao2_fov_debug);
      }
      if (left <= 0)
      {
        return;
      }
      static FILE *log = ao2::OpenLogFile("fov.log");
      if (log)
      {
        std::fprintf(log, fmt, a, b, c, d, e, f);
        if (--left % 64 == 0)
        {
          std::fflush(log);
        }
      }
    }
  } // namespace

  void NoteView(const uint8_t *base, uint32_t controller, float game, float drawn)
  {
    t_shift = Shift{};
    if (!REXCVAR_GET(ao2_fov_crosshair_fix) || drawn == game || !(game > 1.0f && game < 170.0f))
    {
      return;
    }
    int32_t params = ao2::ue3::PropertyOffset(base, "AO2PlayerControllerNative", "HUDControlParameters");
    int32_t position = ao2::ue3::PropertyOffset(base, "SHUDControlParameters", "ReticulePosition");
    int32_t location = ao2::ue3::PropertyOffset(base, "SHUDControlParameters", "ReticuleViewportLocation");
    if (params < 0 || position < 0 || location < 0 || base[controller + params + position] != kReticleInViewport)
    {
      return;
    }
    // Viewport fractions, 0..1 from the top left.
    float u = LoadF32(base, controller + params + location);
    float v = LoadF32(base, controller + params + location + 4);
    bool fractions = u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
    if (fractions)
    {
      double keep = std::tan(game * kHalfDegree) / std::tan(drawn * kHalfDegree);
      double cx = u * 2.0 - 1.0, cy = 1.0 - v * 2.0;
      t_shift = {true, float(cx * (1.0 - keep)), float(cy * (1.0 - keep))};
    }
    Debug("fov %.1f -> %.1f crosshair %.3f %.3f shift %.4f %.4f\n", game, drawn, u, v, t_shift.x, t_shift.y);
  }

  bool TakeLensShift(float &x, float &y)
  {
    if (!t_shift.on)
    {
      return false;
    }
    x = t_shift.x;
    y = t_shift.y;
    t_shift = Shift{};
    return true;
  }

  void ClearLensShift()
  {
    t_shift = Shift{};
  }

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
