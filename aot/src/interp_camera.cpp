// interp_camera.cpp - The camera drawn between simulation steps.
//
// With ao2_decouple (decouple.cpp) the world changes only on simulation
// steps; a frame drawn between them would show the same view again. Here
// the view of every drawn frame is placed between the views of the last two
// steps by how far the frame is past the last one (decouple Alpha): the
// picture runs one step behind and the camera moves on every frame.
//
// ULocalPlayer::CalcSceneView (sub_8284BF18, r3 = local player, r5 = view
// location, r6 = view rotation) gets the view point from the player
// controller's script event GetPlayerViewPoint (sub_82384248: r3 =
// controller, r4 = FVector location, r5 = rotation, three floats in rotator
// units, 65536 = 360 degrees) and the field of view from sub_823E06D8 (f1).
// The event also serves the simulation (aiming: UpdateLookingAction
// sub_82365150, sub_823D4198), so its result is only replaced inside
// CalcSceneView - the drawn view, which the simulation never reads.
//
// The first view of a new step is that step's; jumps (camera cuts,
// teleports, respawns) are not interpolated.
//
// Local camera prediction (ao2_predict_camera): the drawn view then also
// gets this player's mouse turn the simulation has not applied yet
// (mouse_aim.cpp PredictedTurn - online most of it is the input delay). The
// third-person camera orbits the character, so its location is turned with
// it around the pawn (Controller.Pawn, Actor.Location: offsets by name,
// ue3_reflect.h; skipped when that point is not near the camera) - the pawn
// as drawn, between its places on the last two steps like everything else.
// (Taking the pawn's place from how far the interpolation moved the camera
// was wrong while the simulation turned the view: that move is the camera
// going round the pawn, not the pawn moving, so the orbit's centre swung
// with it and the character shook in the view on every frame of a turn -
// the more the larger the predicted turn, i.e. the input delay.)
//
// Scene captures of the player's own view (the GPS overlay: a
// USceneCapture2DComponent the game moves to the camera on every step,
// UpdateTransform sub_82894AE8) are drawn from the drawn view too
// (ao2_interp_captures): their ViewMatrix (+160) is replaced and the
// capture is put into the scene again, as UpdateTransform does. Captures
// of other views (the partner's camera) do not match the player's view and
// are left alone. Their ProjMatrix (+224, made from the game's FOV +132 by
// sub_8289DC00 whenever the capture is put into the scene) gets the drawn
// view's FOV and lens shift (fov.cpp Projection), or with ao2_fov the
// highlighted objects were off the objects in the view. Done at the end of
// CalcSceneView: the FOV is taken after the view point. In split-screen each
// player has a capture of its own: one belongs to the player whose view the
// game placed it at (Capture::player), or every capture was put on the view
// made last and the first player's GPS showed the second player's camera.
//
// Camera shake (ao2_camera_shake; made some players sick): only the drawn view
// loses it. The shake is in the camera actor the simulation aims with and the
// lockstep checks, and its random phases are drawn from the shared generator:
// the game goes on shaking there. Two camera modifiers shake: Handycam, the
// hand-held camera's constant bobbing and lag on foot (sub_82341DE8), and
// ScreenShake, shots and explosions (sub_82342C30). Their ModifyCamera (vtable
// slot 67, r3 = modifier, r4 = camera: adds to the camera's location at +684,
// rotation at +696 and FOV at +460) tells how far it moved the view this step,
// and GetPlayerViewPoint and the FOV take the part to lose off before they are
// interpolated. SnipingOscillation, the scope's sway, stays: it is aiming.
#include <windows.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "decouple.h"
#include "fov.h"
#include "mouse_aim.h"
#include "ue3_reflect.h"
#include "log_files.h"

REXCVAR_DEFINE_BOOL(ao2_interp_camera, true, "Gameplay",
                    "With ao2_decouple: draw the camera between simulation steps (smooth at any frame rate, "
                    "one step behind)");
REXCVAR_DEFINE_INT32(ao2_interp_debug, 0, "Debug",
                     "Log this many interpolated views (step, alpha, yaw in and out) to logs/sim/interp.log");
REXCVAR_DEFINE_BOOL(ao2_predict_camera_orbit, true, "Input",
                    "With ao2_predict_camera: turn the camera location around the character with the predicted "
                    "turn (third-person orbit), not only the view direction");
REXCVAR_DEFINE_BOOL(ao2_interp_captures, true, "Gameplay",
                    "Scene captures that show the player's own view (the GPS overlay) follow the drawn view "
                    "(interpolated, predicted) instead of the last simulation step's");
REXCVAR_DEFINE_DOUBLE(ao2_camera_shake, 1.0, "Gameplay",
                      "How much of the game's camera shake is drawn: 1 = all of it, 0 = none. Only the drawn view, "
                      "the game state is not changed")
    .range(0.0, 1.0);

namespace
{
  // Farther than this between two steps: a cut, not a move.
  constexpr float kMaxMove = 1500.0f;          // world units
  constexpr float kMaxTurn = 65536.0f / 6.0f;  // rotator units (60 degrees)
  constexpr float kMaxFovChange = 40.0f;       // degrees
  // PlayerController +808: set while an in-engine cutscene plays.
  constexpr uint32_t kControllerFlags = 808;
  constexpr uint32_t kCinematicMode = 0x00020000;
  // The camera is this close to the pawn when it orbits it.
  // (on foot ~200, at the wheel of the hovercraft ~810).
  constexpr float kMaxOrbitRadius = 1500.0f;   // world units

  constexpr uint32_t kCapture2DVtable = 0x820CC9E8; // USceneCapture2DComponent
  constexpr uint32_t kComponentScene = 72;
  constexpr uint32_t kCaptureViewMatrix = 160;
  constexpr uint32_t kCaptureProjMatrix = 224;
  constexpr uint32_t kSceneAddCapture = 32;    // scene vtable: (scene, component)
  constexpr uint32_t kSceneRemoveCapture = 36; // scene vtable: (scene, component)
  // A capture the game has not moved for this many steps is forgotten.
  constexpr uint32_t kCaptureLife = 30;
  // A capture shows the player's view when its camera is this close to it.
  constexpr float kSameViewAxis = 0.02f;     // per matrix element
  constexpr float kSameViewDistance = 10.0f; // world units

  float LoadF32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::bit_cast<float>(std::byteswap(v));
  }
  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }
  void StoreF32(uint8_t *base, uint32_t addr, float f)
  {
    uint32_t v = std::byteswap(std::bit_cast<uint32_t>(f));
    std::memcpy(base + addr, &v, 4);
  }

  struct View
  {
    float location[3] = {};
    float rotation[3] = {};
    float fov = 0;
    bool has_location = false;
    bool has_fov = false;
    // The player's pawn on that step (the orbit's centre).
    uint32_t pawn = 0;
    float pawn_location[3] = {};
  };

  // The views of one local player at its last two steps.
  struct PlayerViews
  {
    uint32_t player = 0;
    uint32_t step = 0;
    View prev, cur;
    bool cut = true; // prev is no use (first step, or a jump)
  };
  PlayerViews g_players[4];

  struct Capture
  {
    uint32_t component = 0;
    uint32_t step = 0;     // the last step the game moved it on
    bool has_ours = false; // `ours` was written to it
    bool follows = false;  // it shows the view of `player`
    uint32_t player = 0;   // the local player
    float ours[16] = {};
    // The drawn projection against the game's (fov::Projection) that
    // sub_8289DC00 gives it.
    float keep = 1.0f, shift_x = 0.0f, shift_y = 0.0f;
  };
  Capture g_captures[8];

  thread_local uint32_t t_view_player = 0; // the local player in CalcSceneView

  // What a shaking modifier (Handycam, ScreenShake; a camera has up to two of
  // each) did to its camera's view on the last step. Game thread.
  struct Shake
  {
    uint32_t modifier = 0;
    uint32_t controller = 0; // the camera's CurrentPlayerController
    uint32_t step = 0;
    float location[3] = {};
    float rotation[3] = {};
    float fov = 0.0f;
  };
  Shake g_shakes[8];

  // The views GetPlayerViewPoint saw, for UpdateCaptures at the end of
  // CalcSceneView.
  struct CaptureViews
  {
    bool set = false;
    bool moved = false;
    bool has_prev = false;
    float sim[16], prev[16], drawn[16];
  };
  thread_local CaptureViews t_capture_views;

  FILE *InterpLog()
  {
    static FILE *f = ao2::OpenLogFile("interp.log");
    return f;
  }

  void DebugView(uint32_t step, double alpha, float yaw_in, float yaw_out, bool cut, float predict_yaw = 0.0f,
                 float predict_pitch = 0.0f, float orbit = -1.0f)
  {
    static int left = -1;
    if (left < 0)
    {
      left = REXCVAR_GET(ao2_interp_debug);
    }
    if (left <= 0)
    {
      return;
    }
    FILE *f = InterpLog();
    if (!f)
    {
      return;
    }
    LARGE_INTEGER t, freq;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&freq);
    std::fprintf(f, "%.3f step %u alpha %.3f yaw %.1f -> %.1f%s predict yaw %.1f pitch %.1f orbit %.0f\n",
                 double(t.QuadPart) * 1000.0 / double(freq.QuadPart), step, alpha, yaw_in, yaw_out, cut ? " cut" : "",
                 predict_yaw, predict_pitch, orbit);
    if (--left % 256 == 0)
    {
      std::fflush(f);
    }
  }

  bool On()
  {
    return REXCVAR_GET(ao2_interp_camera) && ao2::decouple::Active();
  }

  PlayerViews &Views(uint32_t player)
  {
    for (PlayerViews &p : g_players)
    {
      if (p.player == player)
      {
        return p;
      }
    }
    for (PlayerViews &p : g_players)
    {
      if (!p.player)
      {
        p.player = player;
        return p;
      }
    }
    g_players[0] = PlayerViews{};
    g_players[0].player = player;
    return g_players[0];
  }

  // Starts a new step's view when the world has ticked since the last one.
  void NextStep(PlayerViews &p)
  {
    uint32_t step = ao2::decouple::WorldStep();
    if (p.step == step)
    {
      return;
    }
    p.cut = p.step + 1 != step || !p.cur.has_location;
    p.step = step;
    p.prev = p.cur;
    p.cur = View{};
  }

  // Rotator difference by the shorter way round.
  float Turn(float from, float to)
  {
    float d = std::fmod(to - from, 65536.0f);
    if (d > 32768.0f)
    {
      d -= 65536.0f;
    }
    else if (d < -32768.0f)
    {
      d += 65536.0f;
    }
    return d;
  }

  // Unit axes (forward, right, up) of a rotator, as FRotationMatrix.
  void Axes(const float rotation[3], float axes[3][3])
  {
    constexpr double kRad = 3.14159265358979323846 / 32768.0;
    double sp = std::sin(rotation[0] * kRad), cp = std::cos(rotation[0] * kRad);
    double sy = std::sin(rotation[1] * kRad), cy = std::cos(rotation[1] * kRad);
    double sr = std::sin(rotation[2] * kRad), cr = std::cos(rotation[2] * kRad);
    double m[3][3] = {{cp * cy, cp * sy, sp},
                      {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp},
                      {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp}};
    for (int i = 0; i < 3; i++)
    {
      for (int j = 0; j < 3; j++)
      {
        axes[i][j] = float(m[i][j]);
      }
    }
  }

  // The game moved this scene capture on this step.
  void NoteCapture(uint32_t component)
  {
    uint32_t step = ao2::decouple::WorldStep();
    Capture *slot = nullptr;
    for (Capture &c : g_captures)
    {
      if (c.component == component)
      {
        c.step = step;
        return;
      }
      if (!slot || (slot->component && (!c.component || c.step < slot->step)))
      {
        slot = &c;
      }
    }
    *slot = Capture{};
    slot->component = component;
    slot->step = step;
  }

  // The view matrix of a camera (row vectors: world point * matrix = view
  // space; x = right, y = up, z = forward), as UpdateTransform makes it.
  void ViewMatrix(const float location[3], const float rotation[3], float m[16])
  {
    float axes[3][3]; // forward, right, up
    Axes(rotation, axes);
    for (int i = 0; i < 3; i++)
    {
      m[4 * i + 0] = axes[1][i];
      m[4 * i + 1] = axes[2][i];
      m[4 * i + 2] = axes[0][i];
      m[4 * i + 3] = 0.0f;
    }
    for (int k = 0; k < 3; k++)
    {
      const float *axis = axes[k == 0 ? 1 : k == 1 ? 2 : 0];
      m[12 + k] = -(location[0] * axis[0] + location[1] * axis[1] + location[2] * axis[2]);
    }
    m[15] = 1.0f;
  }

  // Whether two view matrices are the same camera, give or take a step's
  // move (false for NaN).
  bool SameView(const float a[16], const float b[16])
  {
    for (int i = 0; i < 3; i++)
    {
      for (int j = 0; j < 3; j++)
      {
        if (!(std::fabs(a[4 * i + j] - b[4 * i + j]) <= kSameViewAxis))
        {
          return false;
        }
      }
    }
    float distance2 = 0.0f;
    for (int i = 0; i < 3; i++)
    {
      float pa = -(a[12] * a[4 * i] + a[13] * a[4 * i + 1] + a[14] * a[4 * i + 2]);
      float pb = -(b[12] * b[4 * i] + b[13] * b[4 * i + 1] + b[14] * b[4 * i + 2]);
      distance2 += (pa - pb) * (pa - pb);
    }
    return distance2 < kSameViewDistance * kSameViewDistance;
  }

  void CallScene(PPCContext &ctx, uint8_t *base, uint32_t scene, uint32_t slot, uint32_t component)
  {
    uint32_t vtable = LoadU32(base, scene);
    uint32_t fn = vtable ? LoadU32(base, vtable + slot) : 0;
    if (!fn)
    {
      return;
    }
    PPCContext saved = ctx;
    ctx.r3.u64 = scene;
    ctx.r4.u64 = component;
    rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
    ctx = saved;
  }

  // `follows` < 0: the capture is forgotten.
  void CaptureLog(uint32_t component, int follows)
  {
    if (REXCVAR_GET(ao2_interp_debug) == 0)
    {
      return;
    }
    FILE *f = InterpLog();
    if (!f)
    {
      return;
    }
    if (follows < 0)
    {
      std::fprintf(f, "capture %08X forgotten\n", component);
    }
    else
    {
      std::fprintf(f, "capture %08X follows %d\n", component, follows);
    }
    std::fflush(f);
  }

  void CaptureProjLog(const Capture &c)
  {
    if (REXCVAR_GET(ao2_interp_debug) == 0 || !InterpLog())
    {
      return;
    }
    std::fprintf(InterpLog(), "capture %08X keep %.4f shift %.4f %.4f\n", c.component, c.keep, c.shift_x,
                 c.shift_y);
  }

  // Puts the scene captures that show the player's view onto the drawn view.
  // `sim` / `sim_prev` = the view matrices of the step (and the one before,
  // or null), `drawn` = the drawn view, `moved` = it differs from the step's.
  void UpdateCaptures(PPCContext &ctx, uint8_t *base, uint32_t player, const float sim[16], const float *sim_prev,
                      const float drawn[16], bool moved)
  {
    if (!REXCVAR_GET(ao2_interp_captures))
    {
      return;
    }
    float keep, shift_x, shift_y;
    ao2::fov::Projection(keep, shift_x, shift_y);
    static int32_t attached_offset = -1;
    static uint32_t attached_mask = 0;
    if (attached_offset < 0 &&
        !ao2::ue3::BoolProperty(base, "ActorComponent", "bAttached", attached_offset, attached_mask))
    {
      attached_offset = -1;
      return;
    }
    uint32_t step = ao2::decouple::WorldStep();
    for (Capture &c : g_captures)
    {
      if (!c.component)
      {
        continue;
      }
      uint32_t scene = LoadU32(base, c.component + kComponentScene);
      // Gone, detached, or not moved by the game for a while: forget it.
      if (step - c.step > kCaptureLife || LoadU32(base, c.component) != kCapture2DVtable || !scene ||
          !(LoadU32(base, c.component + attached_offset) & attached_mask))
      {
        CaptureLog(c.component, -1);
        c = Capture{};
        continue;
      }
      float m[16];
      for (int i = 0; i < 16; i++)
      {
        m[i] = LoadF32(base, c.component + kCaptureViewMatrix + 4 * i);
      }
      bool is_ours = c.has_ours && std::memcmp(m, c.ours, sizeof(m)) == 0;
      if (!is_ours)
      {
        // The game placed it (or it is another object now): whose view is it?
        bool follows = SameView(m, sim) || (sim_prev && SameView(m, sim_prev));
        // Not this player's view: another local player's maybe, whose view
        // decides that.
        if (follows || !c.player || c.player == player)
        {
          if (follows != c.follows)
          {
            CaptureLog(c.component, follows ? 1 : 0);
          }
          c.follows = follows;
          c.player = follows ? player : 0;
        }
      }
      if (!c.follows || c.player != player)
      {
        continue;
      }
      // The drawn view is the step's and the game's own matrix is in place:
      // the view stays.
      const float *view = !moved && !is_ours ? m : drawn;
      bool same_proj = c.keep == keep && c.shift_x == shift_x && c.shift_y == shift_y;
      if (std::memcmp(m, view, sizeof(m)) == 0 && same_proj)
      {
        continue;
      }
      for (int i = 0; i < 16; i++)
      {
        StoreF32(base, c.component + kCaptureViewMatrix + 4 * i, view[i]);
      }
      std::memcpy(c.ours, view, sizeof(c.ours));
      c.has_ours = true;
      c.keep = keep;
      c.shift_x = shift_x;
      c.shift_y = shift_y;
      CaptureProjLog(c);
      // As UpdateTransform: out of the scene and in again, so the render
      // thread gets the new matrix and the capture is drawn this frame.
      CallScene(ctx, base, scene, kSceneRemoveCapture, c.component);
      CallScene(ctx, base, scene, kSceneAddCapture, c.component);
    }
  }

  // Interpolates the view written by GetPlayerViewPoint in place. True when
  // it was placed between two steps (`alpha` of the way); `step` = the
  // drawn view's step.
  bool Interpolate(uint8_t *base, uint32_t controller, uint32_t location, uint32_t rotation, uint32_t &step,
                   float &alpha)
  {
    PlayerViews &p = Views(t_view_player);
    NextStep(p);
    step = p.step;
    if (!p.cur.has_location)
    {
      for (int i = 0; i < 3; i++)
      {
        p.cur.location[i] = LoadF32(base, location + 4 * i);
        p.cur.rotation[i] = LoadF32(base, rotation + 4 * i);
      }
      p.cur.has_location = true;
      // Controller.Pawn +452, Actor.Location +228.
      p.cur.pawn = LoadU32(base, controller + 452);
      for (int i = 0; p.cur.pawn && i < 3; i++)
      {
        p.cur.pawn_location[i] = LoadF32(base, p.cur.pawn + 228 + 4 * i);
      }
      if (!p.cut && p.prev.has_location)
      {
        float move = 0, turn = 0;
        for (int i = 0; i < 3; i++)
        {
          float d = p.cur.location[i] - p.prev.location[i];
          move += d * d;
          turn = std::max(turn, std::fabs(Turn(p.prev.rotation[i], p.cur.rotation[i])));
        }
        p.cut = std::sqrt(move) > kMaxMove || turn > kMaxTurn;
      }
      ao2::mouse::NoteView(base, controller, p.step, p.cur.rotation[0], p.cur.rotation[1]);
    }
    if (p.cut || !p.prev.has_location)
    {
      return false;
    }
    float a = float(ao2::decouple::Alpha());
    alpha = a;
    for (int i = 0; i < 3; i++)
    {
      StoreF32(base, location + 4 * i, p.prev.location[i] + (p.cur.location[i] - p.prev.location[i]) * a);
      StoreF32(base, rotation + 4 * i, p.prev.rotation[i] + Turn(p.prev.rotation[i], p.cur.rotation[i]) * a);
    }
    return true;
  }

  // Adds the predicted mouse turn to the drawn view; `alpha` >= 0: the view
  // is between the last two steps, that far. Returns the orbit radius used
  // (-1: none).
  float Predict(uint8_t *base, uint32_t controller, uint32_t location, uint32_t rotation, float alpha,
                float predict_pitch, float predict_yaw)
  {
    float old_rotation[3], new_rotation[3], drawn[3];
    for (int i = 0; i < 3; i++)
    {
      old_rotation[i] = LoadF32(base, rotation + 4 * i);
      drawn[i] = LoadF32(base, location + 4 * i);
    }
    new_rotation[0] = old_rotation[0] + predict_pitch;
    new_rotation[1] = old_rotation[1] + predict_yaw;
    new_rotation[2] = old_rotation[2];
    for (int i = 0; i < 2; i++)
    {
      StoreF32(base, rotation + 4 * i, new_rotation[i]);
    }
    static int32_t pawn_offset = -1, location_offset = -1;
    if (pawn_offset < 0 || location_offset < 0)
    {
      pawn_offset = ao2::ue3::PropertyOffset(base, "Controller", "Pawn");
      location_offset = ao2::ue3::PropertyOffset(base, "Actor", "Location");
    }
    uint32_t pawn = REXCVAR_GET(ao2_predict_camera_orbit) && pawn_offset >= 0 && location_offset >= 0
                        ? LoadU32(base, controller + pawn_offset)
                        : 0;
    if (!pawn)
    {
      return -1.0f;
    }
    // The pawn as drawn: between its places on the last two steps.
    float pivot[3];
    for (int i = 0; i < 3; i++)
    {
      pivot[i] = LoadF32(base, pawn + location_offset + 4 * i);
    }
    if (alpha >= 0.0f)
    {
      const PlayerViews &p = Views(t_view_player);
      if (p.prev.pawn == pawn && p.cur.pawn == pawn)
      {
        for (int i = 0; i < 3; i++)
        {
          pivot[i] = p.prev.pawn_location[i] + (p.cur.pawn_location[i] - p.prev.pawn_location[i]) * alpha;
        }
      }
    }
    float offset[3], radius2 = 0.0f;
    for (int i = 0; i < 3; i++)
    {
      offset[i] = drawn[i] - pivot[i];
      radius2 += offset[i] * offset[i];
    }
    float radius = std::sqrt(radius2);
    if (!(radius > 1.0f && radius < kMaxOrbitRadius))
    {
      return -1.0f;
    }
    float from[3][3], to[3][3];
    Axes(old_rotation, from);
    Axes(new_rotation, to);
    for (int i = 0; i < 3; i++)
    {
      float turned = 0.0f;
      for (int axis = 0; axis < 3; axis++)
      {
        float local = offset[0] * from[axis][0] + offset[1] * from[axis][1] + offset[2] * from[axis][2];
        turned += local * to[axis][i];
      }
      StoreF32(base, location + 4 * i, drawn[i] - offset[i] + turned);
    }
    return radius;
  }

  // The part of the camera shake not to draw (0..1); 0: nothing to do.
  float ShakeToLose()
  {
    return 1.0f - float(std::clamp(REXCVAR_GET(ao2_camera_shake), 0.0, 1.0));
  }

  // Takes it off the view `controller` is drawn from (location, rotation:
  // guest addresses of the view point, 0 not to change them) and returns what
  // the FOV loses. A modifier that ran no step lately is gone with its camera.
  float LoseShake(uint8_t *base, uint32_t controller, uint32_t location, uint32_t rotation)
  {
    float lose = ShakeToLose();
    if (lose <= 0.0f || !controller)
    {
      return 0.0f;
    }
    uint32_t step = ao2::decouple::WorldStep();
    float fov = 0.0f;
    for (const Shake &s : g_shakes)
    {
      if (!s.modifier || s.controller != controller || step - s.step > 1)
      {
        continue;
      }
      for (int i = 0; location && rotation && i < 3; i++)
      {
        StoreF32(base, location + 4 * i, LoadF32(base, location + 4 * i) - s.location[i] * lose);
        StoreF32(base, rotation + 4 * i, LoadF32(base, rotation + 4 * i) - s.rotation[i] * lose);
      }
      fov += s.fov * lose;
    }
    return fov;
  }
} // namespace

namespace
{
  // Runs a shaking modifier's ModifyCamera and notes how far it moved the view.
  template <typename Fn>
  void MeasureShake(PPCContext &ctx, uint8_t *base, Fn modify)
  {
    constexpr uint32_t kCameraLocation = 684;   // CameraActor.CameraModifiers_Location
    constexpr uint32_t kCameraRotation = 696;   // CameraActor.CameraModifiers_Rotation
    constexpr uint32_t kCameraFov = 460;        // CameraActor.FOVAngle
    constexpr uint32_t kCameraController = 732; // CameraActor.CurrentPlayerController
    uint32_t modifier = ctx.r3.u32, camera = ctx.r4.u32;
    if (ShakeToLose() <= 0.0f || !camera)
    {
      modify(ctx, base);
      return;
    }
    float location[3], rotation[3], fov = LoadF32(base, camera + kCameraFov);
    for (int i = 0; i < 3; i++)
    {
      location[i] = LoadF32(base, camera + kCameraLocation + 4 * i);
      rotation[i] = LoadF32(base, camera + kCameraRotation + 4 * i);
    }
    modify(ctx, base);
    Shake *slot = nullptr;
    for (Shake &s : g_shakes)
    {
      if (s.modifier == modifier)
      {
        slot = &s;
        break;
      }
      if (!slot || (slot->modifier && (!s.modifier || s.step < slot->step)))
      {
        slot = &s;
      }
    }
    slot->modifier = modifier;
    slot->controller = LoadU32(base, camera + kCameraController);
    slot->step = ao2::decouple::WorldStep();
    for (int i = 0; i < 3; i++)
    {
      slot->location[i] = LoadF32(base, camera + kCameraLocation + 4 * i) - location[i];
      slot->rotation[i] = LoadF32(base, camera + kCameraRotation + 4 * i) - rotation[i];
    }
    slot->fov = LoadF32(base, camera + kCameraFov) - fov;
  }
} // namespace

// AO2CameraModifier_ScreenShake::ModifyCamera.
extern "C" REX_FUNC(__imp__sub_82342C30);
extern "C" REX_FUNC(sub_82342C30)
{
  MeasureShake(ctx, base, __imp__sub_82342C30);
}

// AO2CameraModifier_Handycam::ModifyCamera.
extern "C" REX_FUNC(__imp__sub_82341DE8);
extern "C" REX_FUNC(sub_82341DE8)
{
  MeasureShake(ctx, base, __imp__sub_82341DE8);
}

// ULocalPlayer::CalcSceneView: remember whose view is being made.
extern "C" REX_FUNC(__imp__sub_8284BF18);
extern "C" REX_FUNC(sub_8284BF18)
{
  uint32_t outer = t_view_player;
  uint32_t player = ctx.r3.u32;
  t_view_player = player;
  t_capture_views.set = false;
  __imp__sub_8284BF18(ctx, base);
  t_view_player = outer;
  if (t_capture_views.set)
  {
    const CaptureViews &v = t_capture_views;
    UpdateCaptures(ctx, base, player, v.sim, v.has_prev ? v.prev : nullptr, v.drawn, v.moved);
    t_capture_views.set = false;
  }
  ao2::fov::ClearLensShift();
}

// eventGetPlayerViewPoint(controller, FVector *location, float rotation[3]).
extern "C" REX_FUNC(__imp__sub_82384248);
extern "C" REX_FUNC(sub_82384248)
{
  uint32_t controller = ctx.r3.u32, location = ctx.r4.u32, rotation = ctx.r5.u32;
  __imp__sub_82384248(ctx, base);
  if (!t_view_player || !location || !rotation)
  {
    return;
  }
  float sim_location[3], sim_rotation[3], yaw_in = LoadF32(base, rotation + 4);
  for (int i = 0; i < 3; i++)
  {
    sim_location[i] = LoadF32(base, location + 4 * i);
    sim_rotation[i] = LoadF32(base, rotation + 4 * i);
  }
  LoseShake(base, controller, location, rotation);
  uint32_t step = ao2::decouple::WorldStep();
  float alpha = 1.0f;
  bool interpolated = On() && Interpolate(base, controller, location, rotation, step, alpha);
  float yaw_out = LoadF32(base, rotation + 4);
  float predict_pitch = 0.0f, predict_yaw = 0.0f, orbit = -1.0f;
  if (ao2::mouse::PredictedTurn(base, controller, step, alpha, interpolated, predict_pitch, predict_yaw))
  {
    orbit = Predict(base, controller, location, rotation, interpolated ? alpha : -1.0f, predict_pitch, predict_yaw);
  }
  DebugView(step, interpolated ? alpha : -1.0, yaw_in, yaw_out, !interpolated, predict_yaw, predict_pitch, orbit);
  if (REXCVAR_GET(ao2_interp_debug) > 0 && InterpLog())
  {
    uint32_t pawn = LoadU32(base, controller + 452);
    std::fprintf(InterpLog(), "  sim %.1f %.1f %.1f pitch %.1f drawn %.1f %.1f %.1f pitch %.1f pawn %.1f %.1f %.1f yaw %d\n",
                 sim_location[0], sim_location[1], sim_location[2], sim_rotation[0], LoadF32(base, location),
                 LoadF32(base, location + 4), LoadF32(base, location + 8), LoadF32(base, rotation),
                 pawn ? LoadF32(base, pawn + 228) : 0.0f, pawn ? LoadF32(base, pawn + 232) : 0.0f,
                 pawn ? LoadF32(base, pawn + 236) : 0.0f, pawn ? int32_t(LoadU32(base, pawn + 244)) : 0);
  }

  // Scene captures of this view (the GPS overlay) follow the drawn view.
  float drawn_location[3], drawn_rotation[3];
  bool moved = false;
  for (int i = 0; i < 3; i++)
  {
    drawn_location[i] = LoadF32(base, location + 4 * i);
    drawn_rotation[i] = LoadF32(base, rotation + 4 * i);
    moved = moved || drawn_location[i] != sim_location[i] || drawn_rotation[i] != sim_rotation[i];
  }
  CaptureViews &v = t_capture_views;
  ViewMatrix(sim_location, sim_rotation, v.sim);
  ViewMatrix(drawn_location, drawn_rotation, v.drawn);
  v.has_prev = false;
  if (On())
  {
    const PlayerViews &p = Views(t_view_player);
    if (p.prev.has_location)
    {
      ViewMatrix(p.prev.location, p.prev.rotation, v.prev);
      v.has_prev = true;
    }
  }
  v.moved = moved;
  v.set = true;
}

// The view's field of view (controller) -> f1, degrees. Only CalcSceneView
// calls it. Interpolated between steps, then the player's FOV settings
// (fov.cpp) - not in cutscenes or the front end.
extern "C" REX_FUNC(__imp__sub_823E06D8);
extern "C" REX_FUNC(sub_823E06D8)
{
  uint32_t controller = ctx.r3.u32;
  __imp__sub_823E06D8(ctx, base);
  if (!t_view_player)
  {
    return;
  }
  ctx.f1.f64 -= LoseShake(base, controller, 0, 0);
  if (On())
  {
    PlayerViews &p = Views(t_view_player);
    NextStep(p);
    if (!p.cur.has_fov)
    {
      p.cur.fov = float(ctx.f1.f64);
      p.cur.has_fov = true;
    }
    if (!p.cut && p.prev.has_fov && std::fabs(p.cur.fov - p.prev.fov) <= kMaxFovChange)
    {
      ctx.f1.f64 = p.prev.fov + (p.cur.fov - p.prev.fov) * float(ao2::decouple::Alpha());
    }
  }
  if (controller && !(LoadU32(base, controller + kControllerFlags) & kCinematicMode) &&
      !ao2::mouse::InFrontEnd())
  {
    float game = float(ctx.f1.f64);
    float drawn = ao2::fov::Drawn(game);
    ao2::fov::NoteView(base, controller, game, drawn);
    ctx.f1.f64 = drawn;
  }
}

// USceneCapture2DComponent::UpdateTransform (component): the game moved a
// scene capture.
extern "C" REX_FUNC(__imp__sub_82894AE8);
extern "C" REX_FUNC(sub_82894AE8)
{
  uint32_t component = ctx.r3.u32;
  __imp__sub_82894AE8(ctx, base);
  if (component)
  {
    NoteCapture(component);
  }
}

// USceneCapture2DComponent's projection (component): ProjMatrix from the
// game's FieldOfView, on every AddCapture (the probe, sub_828B6118). A
// capture of the player's view gets the drawn one: clip x and y scaled,
// then the lens shift (row vectors: M[2][0] / M[2][1] add to clip x / y per
// unit of depth, as aspect_ratio.cpp does for the player's view).
extern "C" REX_FUNC(__imp__sub_8289DC00);
extern "C" REX_FUNC(sub_8289DC00)
{
  uint32_t component = ctx.r3.u32;
  __imp__sub_8289DC00(ctx, base);
  if (!component || !REXCVAR_GET(ao2_interp_captures))
  {
    return;
  }
  for (const Capture &c : g_captures)
  {
    if (c.component != component)
    {
      continue;
    }
    if (c.follows && (c.keep != 1.0f || c.shift_x != 0.0f || c.shift_y != 0.0f))
    {
      uint32_t m = component + kCaptureProjMatrix;
      StoreF32(base, m + 0, LoadF32(base, m + 0) * c.keep);
      StoreF32(base, m + 20, LoadF32(base, m + 20) * c.keep);
      StoreF32(base, m + 32, LoadF32(base, m + 32) + c.shift_x);
      StoreF32(base, m + 36, LoadF32(base, m + 36) + c.shift_y);
    }
    return;
  }
}
