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
// are left alone.
#include <windows.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "decouple.h"
#include "mouse_aim.h"
#include "ue3_reflect.h"
#include "log_files.h"

REXCVAR_DEFINE_BOOL(ao2_interp_camera, true, "Gameplay",
                    "With ao2_decouple: draw the camera between simulation steps (smooth at any frame rate, "
                    "one step behind)");
REXCVAR_DEFINE_INT32(ao2_interp_debug, 0, "Debug",
                     "Log this many interpolated views (step, alpha, yaw in and out) to interp.log");
REXCVAR_DEFINE_BOOL(ao2_predict_camera_orbit, true, "Input",
                    "With ao2_predict_camera: turn the camera location around the character with the predicted "
                    "turn (third-person orbit), not only the view direction");
REXCVAR_DEFINE_BOOL(ao2_interp_captures, true, "Gameplay",
                    "Scene captures that show the player's own view (the GPS overlay) follow the drawn view "
                    "(interpolated, predicted) instead of the last simulation step's");

namespace
{
  // Farther than this between two steps: a cut, not a move.
  constexpr float kMaxMove = 1500.0f;          // world units
  constexpr float kMaxTurn = 65536.0f / 6.0f;  // rotator units (60 degrees)
  constexpr float kMaxFovChange = 40.0f;       // degrees
  // The camera is this close to the pawn when it orbits it.
  // (on foot ~200, at the wheel of the hovercraft ~810).
  constexpr float kMaxOrbitRadius = 1500.0f;   // world units

  constexpr uint32_t kCapture2DVtable = 0x820CC9E8; // USceneCapture2DComponent
  constexpr uint32_t kComponentScene = 72;
  constexpr uint32_t kCaptureViewMatrix = 160;
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
    bool follows = false;  // it shows the player's view
    float ours[16] = {};
  };
  Capture g_captures[8];

  thread_local uint32_t t_view_player = 0; // the local player in CalcSceneView

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

  // Puts the scene captures that show the player's view onto the drawn view.
  // `sim` / `sim_prev` = the view matrices of the step (and the one before,
  // or null), `drawn` = the drawn view, `moved` = it differs from the step's.
  void UpdateCaptures(PPCContext &ctx, uint8_t *base, const float sim[16], const float *sim_prev,
                      const float drawn[16], bool moved)
  {
    if (!REXCVAR_GET(ao2_interp_captures))
    {
      return;
    }
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
        if (follows != c.follows)
        {
          CaptureLog(c.component, follows ? 1 : 0);
        }
        c.follows = follows;
      }
      if (!c.follows)
      {
        continue;
      }
      // Nothing to change: the drawn view is the step's and the game's own
      // matrix is in place.
      if (!moved && !is_ours)
      {
        continue;
      }
      if (std::memcmp(m, drawn, sizeof(m)) == 0)
      {
        continue;
      }
      for (int i = 0; i < 16; i++)
      {
        StoreF32(base, c.component + kCaptureViewMatrix + 4 * i, drawn[i]);
      }
      std::memcpy(c.ours, drawn, sizeof(c.ours));
      c.has_ours = true;
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
} // namespace

// ULocalPlayer::CalcSceneView: remember whose view is being made.
extern "C" REX_FUNC(__imp__sub_8284BF18);
extern "C" REX_FUNC(sub_8284BF18)
{
  uint32_t outer = t_view_player;
  t_view_player = ctx.r3.u32;
  __imp__sub_8284BF18(ctx, base);
  t_view_player = outer;
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
  float sim_view[16], prev_view[16], drawn_view[16];
  ViewMatrix(sim_location, sim_rotation, sim_view);
  ViewMatrix(drawn_location, drawn_rotation, drawn_view);
  const float *prev = nullptr;
  if (On())
  {
    const PlayerViews &p = Views(t_view_player);
    if (p.prev.has_location)
    {
      ViewMatrix(p.prev.location, p.prev.rotation, prev_view);
      prev = prev_view;
    }
  }
  UpdateCaptures(ctx, base, sim_view, prev, drawn_view, moved);
}

// The view's field of view (controller) -> f1, degrees. Only CalcSceneView
// calls it.
extern "C" REX_FUNC(__imp__sub_823E06D8);
extern "C" REX_FUNC(sub_823E06D8)
{
  __imp__sub_823E06D8(ctx, base);
  if (!t_view_player || !On())
  {
    return;
  }
  PlayerViews &p = Views(t_view_player);
  NextStep(p);
  if (!p.cur.has_fov)
  {
    p.cur.fov = float(ctx.f1.f64);
    p.cur.has_fov = true;
  }
  if (p.cut || !p.prev.has_fov || std::fabs(p.cur.fov - p.prev.fov) > kMaxFovChange)
  {
    return;
  }
  ctx.f1.f64 = p.prev.fov + (p.cur.fov - p.prev.fov) * float(ao2::decouple::Alpha());
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
