// mouse_aim.cpp - Native mouse aiming.
//
// The SDK's keyboard/mouse driver captures the cursor (mnk_mouse) but does not
// turn the mouse into the right stick (mnk_mouse_to_stick = false): the stick is
// a turn rate with a dead zone, acceleration and a speed cap, while the mouse
// is a displacement. Instead the mouse turns the camera directly.
//
// The game turns the on-foot camera in sub_82365150, the native behind
// AAO2PlayerControllerNative::UpdateLookingAction (r3 = controller, f1 = delta
// time). It reads the look rotation from [controller+2024]+3620 (float
// FRotator: pitch, yaw, roll; 65536 = full turn), adds the per-frame delta of
// the AimingVelocity_Yaw/Pitch intentions (the right stick after the aiming
// graph: sensitivity, acceleration, aim friction) and sets the result. The
// mouse is added to that rotation right before the call, so it skips the
// stick's velocity shaping but still goes through the game's own limits and
// look-at overrides. The in-game "Invert Y" option (controller+2772, top bit)
// applies to the mouse too.
//
// Zooming narrows the camera's field of view (on foot 70 deg, precision 40,
// back-to-back 60/30, sniper scope down to ~22). The mouse turn is scaled by
// tan(fov / 2) / tan(70 / 2), so the picture moves under the mouse at the
// same rate at the screen center whatever the zoom. The FOV is read from the
// player's camera actor: [controller+832] (an AAO2CameraActor, +460 = FOV in
// degrees; AAO2CameraActor::GetVerticalFOVAngle reads it the same way).
//
// Where that function doesn't run (vehicles, menus, cutscenes), the mouse
// falls back to the right stick, merged in the XInputGetState hook.
//
// Online co-op is a lockstep simulation (coop.h): a rotation written on one
// machine only desyncs the game. There the mouse travels with the pad input
// instead: right after the engine polls the pads (sub_82A95F70) the turn of
// this lockstep frame - already in rotation units, with this player's
// sensitivity, zoom and invert - goes into the gameplay viewport's InputAxis
// (sub_827F86C0) as the keys MouseX / MouseY, which the frame record stores
// like the sticks (key table entries 24 / 25, int(value * 127)). When the
// frame is replayed on both machines, the engine routes the axis to the
// player's controller (sub_82756040, the player manager interaction); it is
// taken there and added to that controller's rotation in UpdateLookingAction
// on the same frame, identically on both machines.
//
// Local camera prediction (ao2_predict_camera, PredictedTurn below): the
// drawn view gets this player's mouse turn that the simulation has not
// applied yet - movement not taken yet, and online the turns sent but not
// replayed (the input delay) - so the picture follows the mouse at once.
// Only the drawn view changes (interp_camera.cpp); the simulation catches up
// by itself. Not in cutscenes (the controller's cinematic mode).
#include "autopilot.h"
#include "cheats.h"
#include "console_cmd.h"
#include "coop.h"
#include "coop_cmd.h"
#include "cutscene_skip.h"
#include "decouple.h"
#include "mouse_aim.h"
#include "cyrillic_fonts.h"
#include "keyboard_prompts.h"
#include "log_files.h"
#include "ue3_reflect.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <map>
#include <mutex>

#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DEFINE_BOOL(ao2_mouse_aim, true, "Input",
                    "Native mouse aiming: the mouse turns the camera directly instead of through the right "
                    "stick (needs mnk_mode, mnk_mouse; mnk_mouse_to_stick = false)");
REXCVAR_DEFINE_DOUBLE(ao2_mouse_sensitivity, 2.0, "Input",
                      "Mouse aiming sensitivity: camera turn per mouse count = 0.022 deg x this (as in Source "
                      "games)")
    .range(0.01, 100.0);
REXCVAR_DEFINE_DOUBLE(ao2_mouse_zoom_sensitivity, 1.0, "Input",
                      "Extra mouse sensitivity factor while zoomed in (camera FOV under 70 deg); the turn is "
                      "already scaled to the FOV")
    .range(0.01, 10.0);
REXCVAR_DEFINE_BOOL(ao2_mouse_invert_y, false, "Input",
                    "Invert mouse Y on top of the in-game Invert Y option");
REXCVAR_DEFINE_DOUBLE(ao2_mouse_stick_sensitivity, 1.0, "Input",
                      "Mouse sensitivity where it drives the right stick (turrets, vehicles, menus): full stick "
                      "speed at 1500 mouse counts per second / this")
    .range(0.01, 10.0);
REXCVAR_DEFINE_BOOL(ao2_predict_camera, true, "Input",
                    "Draw this player's mouse turn at once, before the simulation has it (online co-op: before "
                    "the input delay); only the drawn view, the game state is not changed");
REXCVAR_DEFINE_BOOL(ao2_mouse_debug, false, "Debug", "Log native mouse aiming to mouse_aim.log");
REXCVAR_DEFINE_BOOL(ao2_coop_test_local, false, "Debug",
                    "Send the mouse through the lockstep input in single player too (tests the online co-op path)");

namespace
{
  using clock = std::chrono::steady_clock;

  // Native aiming counts as idle after this long without a camera update.
  constexpr auto kNativeIdle = std::chrono::milliseconds(250);
  // Movement nobody took for this long (loading screens) is dropped instead of
  // landing as one jump afterwards.
  constexpr auto kStaleDelta = std::chrono::milliseconds(250);
  constexpr double kRotationUnitsPerDegree = 65536.0 / 360.0;
  constexpr double kDegreesPerCount = 0.022;
  // On-foot camera FOV, the one ao2_mouse_sensitivity is set for.
  constexpr double kBaseFov = 70.0;
  // A larger pitch step per frame could wrap past +-180 deg and land on the
  // wrong limit.
  constexpr float kMaxPitchStep = 8192.0f;
  // The game's cached AAO2CameraActor::StaticClass() (filled by its IsA casts).
  constexpr uint32_t kCameraActorClass = 0x831B1794;
  // PlayerController +808: set while an in-engine cutscene plays.
  constexpr uint32_t kCinematicMode = 0x00020000;

  std::mutex g_mutex;
  float g_dx = 0.0f; // Mouse counts not applied yet.
  float g_dy = 0.0f;
  clock::time_point g_last_take;
  std::atomic<int64_t> g_native_seen{0}; // clock ticks of the last camera update

  rex::ui::Window *g_window = nullptr;

  bool NativeActive()
  {
    auto seen = clock::time_point(clock::duration(g_native_seen.load(std::memory_order_relaxed)));
    return REXCVAR_GET(ao2_mouse_aim) && clock::now() - seen < kNativeIdle;
  }

  void TakeDelta(float &dx, float &dy)
  {
    std::lock_guard lock(g_mutex);
    dx = g_dx;
    dy = g_dy;
    g_dx = 0.0f;
    g_dy = 0.0f;
    g_last_take = clock::now();
  }

  class MouseListener : public rex::ui::WindowInputListener
  {
  public:
    void OnMouseMove(rex::ui::MouseEvent &e) override
    {
      // Only while the SDK driver holds the cursor (hidden, relative mode).
      if (!g_window || !g_window->HasFocus() ||
          g_window->GetCursorVisibility() != rex::ui::Window::CursorVisibility::kHidden)
      {
        return;
      }
      ao2::mouse::InjectDelta(e.dx(), e.dy());
    }
  };

  MouseListener g_listener;

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

  // FOV of the player's camera in degrees, 0 when there is no camera actor.
  float CameraFov(const uint8_t *base, uint32_t controller)
  {
    uint32_t camera = LoadU32(base, controller + 832);
    uint32_t want = LoadU32(base, kCameraActorClass);
    if (!camera || !want)
    {
      return 0.0f;
    }
    // UObject::Class at +52, UStruct::SuperField at +60.
    uint32_t cls = LoadU32(base, camera + 52);
    for (int depth = 0; cls && depth < 32; ++depth, cls = LoadU32(base, cls + 60))
    {
      if (cls == want)
      {
        return LoadF32(base, camera + 460);
      }
    }
    return 0.0f;
  }

  // Mouse sensitivity factor for the current zoom.
  double ZoomScale(float fov)
  {
    if (!(fov > 1.0f && fov < 170.0f))
    {
      return 1.0;
    }
    constexpr double kHalfDegree = 3.14159265358979323846 / 360.0;
    double scale = std::tan(fov * kHalfDegree) / std::tan(kBaseFov * kHalfDegree);
    if (fov < kBaseFov)
    {
      scale *= REXCVAR_GET(ao2_mouse_zoom_sensitivity);
    }
    return scale;
  }

  // The mouse goes through the lockstep input (online co-op, or testing).
  bool LockstepMouse(const uint8_t *base)
  {
    return ao2::coop::OnlineLockstep(base) || (REXCVAR_GET(ao2_coop_test_local) && ao2::coop::LocalLockstep(base));
  }

  FILE *DebugLog()
  {
    static FILE *f = ao2::OpenLogFile("mouse_aim.log");
    return f;
  }

  // Turn per mouse count in rotation units for this player's settings.
  double UnitsPerCount(float fov)
  {
    return kDegreesPerCount * REXCVAR_GET(ao2_mouse_sensitivity) * kRotationUnitsPerDegree * ZoomScale(fov);
  }

  // ---------------------------------------------------------------------
  // Mouse through the lockstep input (online co-op)
  // ---------------------------------------------------------------------

  // FName globals of the MouseX / MouseY keys (set at startup, sub_82706BA8).
  constexpr uint32_t kNameMouseX = 0x831F06EC;
  constexpr uint32_t kNameMouseY = 0x831EFF04;
  // The gameplay viewport's InputAxis (records into the lockstep frame).
  constexpr uint32_t kGameplayInputAxis = 0x827F86C0;
  // GEngine; +772 = GameViewportClient, whose vtable +268 finds the local
  // player of a controller id; LocalPlayer +64 = its PlayerController.
  constexpr uint32_t kEngine = 0x831EFCBC;
  // The record sends axes to the partner as signed bytes (the low byte of
  // the value), so a frame's turn travels as a byte code: steps of one
  // rotation unit up to 32, then 5 % larger per code up to ~18 deg per frame.
  // What a code cannot represent is carried into the next frame, so the sum
  // stays exact. Both machines decode with the same integer table.
  constexpr int kTurnCodeMax = 127;

  const std::array<int32_t, kTurnCodeMax + 1> &TurnTable()
  {
    static const auto table = [] {
      std::array<int32_t, kTurnCodeMax + 1> t{};
      for (int c = 0; c <= kTurnCodeMax; ++c)
      {
        int32_t v = c <= 32 ? c : int32_t(std::lround(32.0 * std::pow(1.05, c - 32)));
        t[c] = c > 0 ? std::max(v, t[c - 1] + 1) : 0;
      }
      return t;
    }();
    return table;
  }

  double DecodeTurn(int code)
  {
    code = std::clamp(code, -kTurnCodeMax, kTurnCodeMax);
    double v = TurnTable()[std::abs(code)];
    return code < 0 ? -v : v;
  }

  // The code whose turn is nearest to `units` (rotation units).
  int EncodeTurn(double units)
  {
    const auto &t = TurnTable();
    double a = std::fabs(units);
    int best = 0;
    for (int c = 1; c <= kTurnCodeMax; ++c)
    {
      if (std::fabs(t[c] - a) < std::fabs(t[best] - a))
      {
        best = c;
      }
    }
    return units < 0 ? -best : best;
  }

  uint64_t LoadName(const uint8_t *base, uint32_t addr)
  {
    return uint64_t(LoadU32(base, addr)) << 32 | LoadU32(base, addr + 4);
  }

  // Calls a guest function by address with (r3, r4), returns r3; the
  // context is restored afterwards.
  uint32_t CallGuest(PPCContext &ctx, uint8_t *base, uint32_t fn, uint32_t r3, uint32_t r4)
  {
    PPCContext saved = ctx;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
    uint32_t result = ctx.r3.u32;
    ctx = saved;
    return result;
  }

  // The PlayerController of a controller id, as the engine routes input.
  uint32_t ControllerOf(PPCContext &ctx, uint8_t *base, uint32_t controller_id)
  {
    uint32_t engine = LoadU32(base, kEngine);
    uint32_t gvc = engine ? LoadU32(base, engine + 772) : 0;
    uint32_t vtable = gvc ? LoadU32(base, gvc) : 0;
    if (!vtable)
    {
      return 0;
    }
    uint32_t player = CallGuest(ctx, base, LoadU32(base, vtable + 268), gvc, controller_id);
    return player ? LoadU32(base, player + 64) : 0;
  }

  // This machine's player. In a network game both players have a local
  // player and a controller on both machines, numbered by the network - not
  // always 0 for the host - so controller 0 can be the partner's (then the
  // partner's zoom scaled this player's mouse, and the partner's turret sent
  // it to the right stick).
  // Only the player signed in here gets the UI data stores (controller +800
  // PlayerOwnerDataStore, also +804, +1236): the partner's are null.
  uint32_t LocalController(PPCContext &ctx, uint8_t *base)
  {
    uint32_t first = 0, found = 0;
    uint32_t seen[4] = {}, stores[4] = {};
    for (uint32_t id = 0; id < 4; ++id)
    {
      uint32_t controller = ControllerOf(ctx, base, id);
      seen[id] = controller;
      stores[id] = controller ? LoadU32(base, controller + 800) : 0;
      if (!controller)
      {
        continue;
      }
      if (stores[id] && !found)
      {
        found = controller;
      }
      if (!first)
      {
        first = controller;
      }
    }
    uint32_t result = found ? found : first;
    static uint32_t last = ~0u;
    if (result != last && REXCVAR_GET(ao2_mouse_debug) && DebugLog())
    {
      std::fprintf(DebugLog(), "local controller %08X (ids 0-3: %08X/%08X %08X/%08X %08X/%08X %08X/%08X)\n", result,
                   seen[0], stores[0], seen[1], stores[1], seen[2], stores[2], seen[3], stores[3]);
    }
    last = result;
    return result;
  }

  // The main menu and the lobby: the world of the package Shell. Their
  // camera is a player controller's too and UpdateLookingAction turns it,
  // but the mouse is for the menus there.
  bool InShell(const uint8_t *base)
  {
    constexpr uint32_t kWorld = 0x831F2CC8; // GWorld; +40 its package
    uint32_t world = LoadU32(base, kWorld);
    uint32_t package = world ? LoadU32(base, world + 40) : 0;
    return package && ao2::ue3::ObjectName(base, package) == "Shell";
  }

  // Split-screen: the controller of the player the keyboard and mouse belong
  // to (0: nobody's, or not known yet).
  uint32_t KeyboardController(PPCContext &ctx, uint8_t *base)
  {
    uint32_t user = ao2::mouse::KeyboardUser();
    return user < 4 ? ControllerOf(ctx, base, user) : 0;
  }

  // Outside online co-op every local player's camera runs the hooks below:
  // only the keyboard player's takes the mouse. Without a local player of
  // that user (single player started from another pad), or while it has no
  // character (the main menu, whose camera is another controller's), the
  // one there is takes it: given to nobody, the mouse went to the right
  // stick and turned the main menu's camera.
  bool TakesMouse(PPCContext &ctx, uint8_t *base, uint32_t controller)
  {
    uint32_t user = ao2::mouse::KeyboardUser();
    if (user >= 4 || InShell(base))
    {
      return false;
    }
    uint32_t owner = ControllerOf(ctx, base, user);
    return !owner || owner == controller || !LoadU32(base, owner + 2024);
  }

  // Sender: turn of the current lockstep frame (in rotation units).
  uint32_t g_send_frame = ~0u;
  double g_send_yaw = 0.0, g_send_pitch = 0.0;   // this frame + carry
  int g_sent_yaw = 0, g_sent_pitch = 0;          // codes the record holds
  uint32_t g_local_controller = 0;               // this machine's player

  // Receiver: turn replayed for a controller (game thread only).
  struct PendingTurn
  {
    uint32_t frame = 0;
    int yaw = 0, pitch = 0; // turn codes
  };
  std::map<uint32_t, PendingTurn> g_pending;

  // Prediction (game thread). Turns this machine's player sent (codes of
  // finished lockstep frames) that the replay has not applied yet, oldest
  // first; lost ones age out.
  struct SentTurn
  {
    int yaw = 0, pitch = 0;
    clock::time_point when;
  };
  constexpr auto kSentTurnLife = std::chrono::milliseconds(1500);
  constexpr size_t kMaxSentTurns = 128;
  std::deque<SentTurn> g_unapplied;
  // The mouse turn the simulation applied on its last step (rotation units),
  // and for whom: the drawn view between two steps shows only part of it.
  uint32_t g_applied_step = ~0u;
  uint32_t g_applied_controller = 0;
  float g_applied_yaw = 0.0f, g_applied_pitch = 0.0f;
  uint32_t g_single_controller = 0; // single player: whose camera UpdateLookingAction turned

  void DropOldSent()
  {
    auto now = clock::now();
    while (!g_unapplied.empty() &&
           (now - g_unapplied.front().when > kSentTurnLife || g_unapplied.size() > kMaxSentTurns))
    {
      g_unapplied.pop_front();
    }
  }

  void NoteSent(int yaw, int pitch)
  {
    if (yaw || pitch)
    {
      g_unapplied.push_back(SentTurn{yaw, pitch, clock::now()});
      DropOldSent();
    }
  }

  // The replay applied one of this player's turns: it and any older ones
  // that never made it (frames the camera was not turned on) are done.
  void NoteReplayed(int yaw, int pitch)
  {
    for (size_t i = 0; i < g_unapplied.size(); ++i)
    {
      if (g_unapplied[i].yaw == yaw && g_unapplied[i].pitch == pitch)
      {
        g_unapplied.erase(g_unapplied.begin(), g_unapplied.begin() + i + 1);
        return;
      }
    }
  }

  void NoteApplied(uint32_t controller, float yaw, float pitch)
  {
    g_applied_step = ao2::decouple::WorldStep();
    g_applied_controller = controller;
    g_applied_yaw = yaw;
    g_applied_pitch = pitch;
  }

  // The look pitch's limits. The game takes the look back within them later
  // in the step than the mouse's turn is added (the rotation reads 7100 after
  // UpdateLookingAction, 6498 when the next step starts), so only the part of
  // a turn within them shows; the drawn view (PredictedTurn) must not add
  // more, or it looks past the limit and springs back. The on-foot limits
  // are the character's (ModePitchLimits); a narrower one (cover, another
  // camera) is learnt while the game cuts the turns short at it.
  float g_pitch_top = 6498.0f, g_pitch_bottom = -9102.0f;
  bool g_pitch_asked = false;    // the last step turned the pitch...
  float g_pitch_asked_to = 0.0f; // ...to this

  // The on-foot look limits of character `holder` (rotation units), from the
  // game's settings: aiming (AO2Pawn.bIsInPrecisionMode) the character's
  // Precision_Aiming_Min/MaxPitch (-77 / 77 degrees), otherwise its
  // CameraSettings' Suppression_Minimum/MaximumPitch (-50 / 35.7). Aiming
  // looks almost twice as far down; the prediction held to the limits
  // without aim did not turn the view below them and it stuttered. The
  // measured values while the properties are not found.
  void ModePitchLimits(const uint8_t *base, uint32_t holder, float &bottom, float &top)
  {
    static int32_t precision = -1, aim_min = -1, aim_max = -1, settings = -1, min = -1, max = -1;
    static uint32_t precision_mask = 0;
    if (precision < 0 || aim_min < 0 || aim_max < 0 || settings < 0 || min < 0 || max < 0)
    {
      ao2::ue3::BoolProperty(base, "AO2Pawn", "bIsInPrecisionMode", precision, precision_mask);
      aim_min = ao2::ue3::PropertyOffset(base, "AO2Character", "Precision_Aiming_MinPitch");
      aim_max = ao2::ue3::PropertyOffset(base, "AO2Character", "Precision_Aiming_MaxPitch");
      settings = ao2::ue3::PropertyOffset(base, "AO2Character", "CameraSettings");
      min = ao2::ue3::PropertyOffset(base, "AO2CameraSettings", "Suppression_MinimumPitch");
      max = ao2::ue3::PropertyOffset(base, "AO2CameraSettings", "Suppression_MaximumPitch");
    }
    constexpr float kUnitsPerDegree = 65536.0f / 360.0f;
    bool aiming = precision >= 0 && precision_mask && (LoadU32(base, holder + precision) & precision_mask);
    uint32_t camera = settings >= 0 ? LoadU32(base, holder + settings) : 0;
    if (aiming && aim_min >= 0 && aim_max >= 0)
    {
      bottom = LoadF32(base, holder + aim_min) * kUnitsPerDegree;
      top = LoadF32(base, holder + aim_max) * kUnitsPerDegree;
    }
    else if (aiming)
    {
      bottom = -14017.0f;
      top = 14017.0f;
    }
    else if (camera && min >= 0 && max >= 0)
    {
      bottom = LoadF32(base, camera + min) * kUnitsPerDegree;
      top = LoadF32(base, camera + max) * kUnitsPerDegree;
    }
    else
    {
      bottom = -9102.0f;
      top = 6498.0f;
    }
    if (!(bottom < 0.0f && top > 0.0f && top - bottom < 65536.0f))
    {
      bottom = aiming ? -14017.0f : -9102.0f;
      top = aiming ? 14017.0f : 6498.0f;
    }
  }

  // The local player's pitch as a step starts, before its turn: a turn the
  // game cut short marks the limit there, otherwise the character's hold.
  void LearnPitchLimits(const uint8_t *base, uint32_t holder, float pitch)
  {
    float bottom, top;
    ModePitchLimits(base, holder, bottom, top);
    pitch = std::remainder(pitch, 65536.0f);
    bool cut_top = g_pitch_asked && pitch > 0.0f && g_pitch_asked_to > pitch + 1.0f;
    bool cut_bottom = g_pitch_asked && pitch < 0.0f && g_pitch_asked_to < pitch - 1.0f;
    g_pitch_top = cut_top ? pitch : top;
    g_pitch_bottom = cut_bottom ? pitch : bottom;
    g_pitch_asked = false;
  }

  // Of a pitch turn from `pitch`, the part within the limits.
  float KeptPitch(float pitch, float turn)
  {
    float now = std::remainder(pitch, 65536.0f);
    float lo = std::min(g_pitch_bottom, now), hi = std::max(g_pitch_top, now);
    return std::clamp(now + turn, lo, hi) - now;
  }

  // The local player's pitch turned by `turn` from `pitch` in this step.
  void NotePitchTurn(float pitch, float turn)
  {
    g_pitch_asked = turn != 0.0f;
    g_pitch_asked_to = std::remainder(pitch, 65536.0f) + turn;
  }

  // UpdateLookingAction turns the view from the stick only while the
  // character's current action lets the player look: the action object
  // ([character+1736], current one through vtable +264) has the top bit of
  // +72 set. Otherwise (climbing over an obstacle etc.) a script event sets
  // the rotation and the stick is ignored. A mouse turn does not show then
  // either, so the camera prediction must not draw it (it sprang back).
  // The local player's state as of the last UpdateLookingAction.
  bool g_view_locked = false;

  bool PlayerTurnsView(PPCContext &ctx, uint8_t *base, uint32_t holder)
  {
    uint32_t actions = LoadU32(base, holder + 1736);
    uint32_t vtable = actions ? LoadU32(base, actions) : 0;
    uint32_t current = vtable ? LoadU32(base, vtable + 264) : 0;
    if (!current)
    {
      return false;
    }
    PPCContext saved = ctx;
    ctx.r3.u64 = actions;
    rex::runtime::ResolveIndirectFunction(current)(ctx, base);
    uint32_t action = ctx.r3.u32;
    ctx = saved;
    return action && (LoadU32(base, action + 72) & 0x80000000) != 0;
  }

  // After a camera move of its own (holding RB looks at the partner) the
  // game turns the view back to the rotation it had before: controller
  // +2868 top bit, the rotation at +2872. UpdateLookingAction ends that
  // return once the stick turns the view by a rotation unit in a frame; a
  // mouse turn written straight into the rotation it kept pulling back - the
  // view stayed fixed until an aim (LT) ended the return. The mouse ends it
  // like the stick.
  void EndViewReturn(uint8_t *base, uint32_t controller, float yaw, float pitch)
  {
    constexpr uint32_t kViewReturn = 2868;
    uint32_t flags = LoadU32(base, controller + kViewReturn);
    if ((flags & 0x80000000) && (std::fabs(yaw) >= 1.0f || std::fabs(pitch) >= 1.0f))
    {
      flags = std::byteswap(flags & 0x7FFFFFFF);
      std::memcpy(base + controller + kViewReturn, &flags, 4);
    }
  }

  // A turret's gun (and the view on it) follows the controller's rotation
  // through skeletal controls (AO2SkelControl_Turret: TurretRotate /
  // TurretPitch) at LagDegreesPerSecond = 360 at most: enough for the stick,
  // a speed limit under the mouse (and a gun that goes the long way round
  // after a fast half turn). The controls of a turret a player sits on follow
  // at once. Set from the simulation (the aiming graph call of the turret's
  // script), for every player's turret alike, so both machines of a co-op
  // game do the same.
  //
  // The controls are on the mesh of the turret the player's pawn is, or of
  // the vehicle the pawn is a seat of (the hovercraft's gunner: an
  // AO2VehicleSeatPawn, Actor.Base = the vehicle).
  //
  // They also hold how far the gun turns from that vehicle's own rotation
  // (MinAngle / MaxAngle: pitch, yaw in degrees). A turn past that never
  // shows, so the camera prediction must not draw it (TurretTurn).
  struct Turret
  {
    uint32_t pawn = 0;
    uint32_t vehicle = 0; // whose mesh has the controls, 0 = none found
    std::array<uint32_t, 8> controls{};
    float yaw_min = 0, yaw_max = 0, pitch_min = 0, pitch_max = 0; // rotation units from the vehicle's rotation
  };
  std::array<Turret, 4> g_turrets; // by pawn, the oldest is replaced
  size_t g_next_turret = 0;
  uint32_t g_turret_control_class = 0;

  const Turret *TurretOf(uint32_t pawn)
  {
    for (const Turret &t : g_turrets)
    {
      if (pawn && t.pawn == pawn)
      {
        return &t;
      }
    }
    return nullptr;
  }

  // The part of a turn (rotation units) the turret of this controller can
  // make from where it is.
  void TurretTurn(const uint8_t *base, uint32_t controller, uint32_t pawn, float &yaw, float &pitch)
  {
    const Turret *t = TurretOf(pawn);
    if (!t || !t->vehicle)
    {
      return;
    }
    // Actor.Rotation: pitch, yaw (int) at +240.
    auto part = [&](uint32_t offset, float lo, float hi, float &turn) {
      // No limits, or all the way round.
      if (!(lo < hi) || hi - lo >= 65535.0f)
      {
        return;
      }
      float now = std::remainder(float(int32_t(LoadU32(base, controller + offset)) -
                                       int32_t(LoadU32(base, t->vehicle + offset))), 65536.0f);
      turn = std::clamp(now + turn, lo, hi) - std::clamp(now, lo, hi);
    };
    part(244, t->yaw_min, t->yaw_max, yaw);
    part(240, t->pitch_min, t->pitch_max, pitch);
  }

  // Where the gun of this controller's turret points: the controller's
  // rotation within the gun's limits.
  void GunRotation(const uint8_t *base, uint32_t controller, uint32_t pawn, float &pitch, float &yaw)
  {
    pitch = float(int32_t(LoadU32(base, controller + 240)));
    yaw = float(int32_t(LoadU32(base, controller + 244)));
    const Turret *t = TurretOf(pawn);
    if (!t || !t->vehicle)
    {
      return;
    }
    auto part = [&](uint32_t offset, float lo, float hi, float &angle) {
      if (!(lo < hi) || hi - lo >= 65535.0f)
      {
        return;
      }
      float from = float(int32_t(LoadU32(base, t->vehicle + offset)));
      angle = from + std::clamp(std::remainder(angle - from, 65536.0f), lo, hi);
    };
    part(240, t->pitch_min, t->pitch_max, pitch);
    part(244, t->yaw_min, t->yaw_max, yaw);
  }

  // Getting on a turret the camera goes from the character's view to the
  // gun's with the animation (some 20 steps), while the turret's rotation
  // already turns with the mouse: the turn does not show, its prediction
  // turned the drawn view and the next step's view took it back. On the gun
  // the view is where the gun points (within 2 rotation units). Judged on
  // every step's view (NoteView): the local player's turret pawn, the step,
  // and whether its view was on the gun.
  uint32_t g_gun_view_pawn = 0;
  uint32_t g_gun_view_step = ~0u;
  bool g_gun_view_on = true;

  void UnlagTurret(uint8_t *base, uint32_t pawn)
  {
    constexpr uint32_t kObjects = 0x831E4E3C; // GObjObjects (data, count)
    constexpr uint32_t kNames = 0x831D1724;   // FName::Names (data): entry +16 = chars
    constexpr uint32_t kSkelComponent = 60;   // SkelControlBase.SkelComponent
    constexpr uint32_t kLag = 232;            // UTSkelControl_TurretConstrained.LagDegreesPerSecond
    constexpr uint32_t kActorBase = 96;       // Actor.Base
    constexpr float kNoLag = 36000.0f;
    const Turret *known = TurretOf(pawn);
    if (!known)
    {
      Turret t;
      t.pawn = pawn;
      uint32_t seat_of = LoadU32(base, pawn + kActorBase);
      size_t found = 0;
      uint32_t objects = LoadU32(base, kObjects), count = LoadU32(base, kObjects + 4);
      uint32_t names = LoadU32(base, kNames);
      g_turret_control_class = 0;
      for (uint32_t i = 0; objects && names && i < count && found < t.controls.size(); ++i)
      {
        uint32_t o = LoadU32(base, objects + i * 4);
        uint32_t component = o ? LoadU32(base, o + kSkelComponent) : 0;
        uint32_t cls = o ? LoadU32(base, o + 52) : 0;
        if (!cls || (g_turret_control_class && cls != g_turret_control_class))
        {
          continue;
        }
        if (!g_turret_control_class)
        {
          uint32_t entry = LoadU32(base, names + LoadU32(base, cls + 44) * 4);
          if (!entry || std::strcmp(reinterpret_cast<const char *>(base + entry + 16), "AO2SkelControl_Turret") != 0)
          {
            continue;
          }
          g_turret_control_class = cls;
        }
        // The component's Outer is the vehicle. The pawn's own controls
        // first; a seat's are its vehicle's.
        uint32_t owner = component ? LoadU32(base, component + 40) : 0;
        if (owner && (owner == pawn || (owner == seat_of && t.vehicle != pawn)))
        {
          if (owner == pawn && t.vehicle != pawn)
          {
            found = 0;
          }
          t.vehicle = owner;
          t.controls[found++] = o;
        }
      }
      // MaxAngle +196, MinAngle +208: pitch, yaw, roll (int degrees); a
      // control of one axis has zeros for the other.
      for (size_t i = 0; i < found; ++i)
      {
        auto units = [&](uint32_t offset) {
          return float(int32_t(LoadU32(base, t.controls[i] + offset)) * kRotationUnitsPerDegree);
        };
        t.pitch_max = std::max(t.pitch_max, units(196));
        t.yaw_max = std::max(t.yaw_max, units(200));
        t.pitch_min = std::min(t.pitch_min, units(208));
        t.yaw_min = std::min(t.yaw_min, units(212));
      }
      for (size_t i = found; i < t.controls.size(); ++i)
      {
        t.controls[i] = 0;
      }
      if (REXCVAR_GET(ao2_mouse_debug) && DebugLog())
      {
        std::fprintf(DebugLog(), "turret %08X: %zu controls on %08X, yaw %.0f..%.0f pitch %.0f..%.0f\n", pawn, found,
                     t.vehicle, t.yaw_min, t.yaw_max, t.pitch_min, t.pitch_max);
      }
      g_turrets[g_next_turret] = t;
      known = &g_turrets[g_next_turret];
      g_next_turret = (g_next_turret + 1) % g_turrets.size();
    }
    for (uint32_t o : known->controls)
    {
      // Still that vehicle's control (not an object freed and reused since).
      uint32_t component = o && LoadU32(base, o + 52) == g_turret_control_class ? LoadU32(base, o + kSkelComponent) : 0;
      if (component && LoadU32(base, component + 40) == known->vehicle && LoadF32(base, o + kLag) != kNoLag)
      {
        StoreF32(base, o + kLag, kNoLag);
      }
    }
  }

  // The pawn is a vehicle its player drives: an SVehicle that is not a
  // turret and not a seat of another vehicle (UTWeaponPawn), by its class
  // names (FName::Names 0x831D1724, entry +16 = chars; UStruct super +60).
  const char *ClassName(const uint8_t *base, uint32_t object)
  {
    constexpr uint32_t kNames = 0x831D1724;
    uint32_t names = LoadU32(base, kNames), cls = object ? LoadU32(base, object + 52) : 0;
    uint32_t entry = names && cls ? LoadU32(base, names + LoadU32(base, cls + 44) * 4) : 0;
    return entry ? reinterpret_cast<const char *>(base + entry + 16) : "-";
  }

  bool IsDrivenVehicle(const uint8_t *base, uint32_t pawn)
  {
    constexpr uint32_t kNames = 0x831D1724;
    static std::map<uint32_t, bool> by_class;
    uint32_t cls = LoadU32(base, pawn + 52);
    auto it = by_class.find(cls);
    if (it != by_class.end())
    {
      return it->second;
    }
    uint32_t names = LoadU32(base, kNames);
    bool vehicle = false, other = false;
    for (uint32_t c = cls, depth = 0; names && c && depth < 24; c = LoadU32(base, c + 60), ++depth)
    {
      uint32_t entry = LoadU32(base, names + LoadU32(base, c + 44) * 4);
      const char *name = entry ? reinterpret_cast<const char *>(base + entry + 16) : "";
      vehicle = vehicle || std::strcmp(name, "SVehicle") == 0;
      other = other || std::strstr(name, "Turret") || std::strcmp(name, "UTWeaponPawn") == 0;
    }
    if (by_class.size() > 256)
    {
      by_class.clear();
    }
    return by_class[cls] = vehicle && !other;
  }

  std::atomic<bool> g_driving{false};
  // The world is the front end (InShell), as of the game thread's last pad poll.
  std::atomic<bool> g_in_shell{false};
  // Clock ticks of the last UpdateLookingAction of any branch: it runs on
  // foot, in vehicles and on turrets, not in the menus (where the players'
  // controllers still have the menu's characters as pawns) or when paused.
  std::atomic<int64_t> g_look_seen{0};

  // At the wheel the view is the vehicle's look-around camera: the vehicle's
  // rotation plus the Offset (+80: pitch, yaw, roll; int) of a camera modifier
  // (AO2CameraModifier_RelativeRotationOffset), which the input turns and
  // which keeps each axis within MinLimit +108 / MaxLimit +120 where they
  // differ (the hovercraft: pitch -25..+5 deg, yaw free). The modifier of
  // the last driver turned by the mouse, for the camera prediction.
  uint32_t g_driver_controller = 0;
  uint32_t g_driver_offset = 0;

  // The part of a turn (rotation units) the driver's camera can make.
  void DriverTurn(const uint8_t *base, uint32_t controller, float &yaw, float &pitch)
  {
    if (controller != g_driver_controller || !g_driver_offset)
    {
      return;
    }
    auto part = [&](uint32_t axis, float &turn) {
      float lo = float(int32_t(LoadU32(base, g_driver_offset + 108 + axis)));
      float hi = float(int32_t(LoadU32(base, g_driver_offset + 120 + axis)));
      if (!(lo < hi))
      {
        return;
      }
      float now = std::remainder(float(int32_t(LoadU32(base, g_driver_offset + 80 + axis))), 65536.0f);
      turn = std::clamp(now + turn, lo, hi) - std::clamp(now, lo, hi);
    };
    part(0, pitch);
    part(4, yaw);
  }

  // Key table (0x83186320) indices of MouseX / MouseY in the frame records.
  constexpr uint32_t kKeyMouseX = 24, kKeyMouseY = 25;
  constexpr uint32_t kFrameSync = 0x831F3008;      // frame sync object singleton
  constexpr uint32_t kSyncIsNetwork = 0x82A0EC20;  // (sync) -> network game

  // A replayed turn code of a controller id for this lockstep frame (set,
  // not added: a frame's code may arrive more than once).
  void ReplayedTurn(PPCContext &ctx, uint8_t *base, uint32_t controller_id, bool x, int code)
  {
    uint32_t controller = ControllerOf(ctx, base, controller_id);
    if (!controller)
    {
      return;
    }
    PendingTurn &p = g_pending[controller];
    uint32_t frame = ao2::coop::SyncFrame(base);
    if (p.frame != frame)
    {
      p = PendingTurn{frame, 0, 0};
    }
    (x ? p.yaw : p.pitch) = code;
    if (REXCVAR_GET(ao2_mouse_debug) && DebugLog())
    {
      std::fprintf(DebugLog(), "replay frame=%u ctrl_id=%u ctrl=%08X %s=%d\n", frame, controller_id, controller,
                   x ? "yaw" : "pitch", code);
    }
  }

  void InjectAxis(PPCContext &ctx, uint8_t *base, uint32_t axis_obj, uint32_t client, uint32_t name_global,
                  int code)
  {
    // Recorded as int(value * 127), replayed as int / 127: a quarter step
    // away from zero survives both roundings.
    double value = (double(code) + (code < 0 ? -0.25 : 0.25)) / 127.0;
    PPCContext saved = ctx;
    ctx.r3.u64 = axis_obj;
    ctx.r4.u64 = client;
    ctx.r5.u64 = 0; // the pad of this machine's player
    ctx.r6.u64 = LoadName(base, name_global);
    ctx.f1.f64 = double(float(value));
    ctx.f2.f64 = 1.0 / 60.0;
    rex::runtime::ResolveIndirectFunction(kGameplayInputAxis)(ctx, base);
    ctx = saved;
  }

  // After the engine polled the pads: put this frame's mouse turn into the
  // lockstep input record.
  void SendLockstepMouse(PPCContext &ctx, uint8_t *base, uint32_t client)
  {
    uint32_t axis_obj = client ? LoadU32(base, client + 52) : 0;
    uint32_t vtable = axis_obj ? LoadU32(base, axis_obj) : 0;
    if (!vtable || LoadU32(base, vtable + 24) != kGameplayInputAxis)
    {
      return;
    }
    uint32_t frame = ao2::coop::SyncFrame(base);
    if (frame != g_send_frame)
    {
      g_local_controller = LocalController(ctx, base);
      // The finished record's turn is on its way (prediction).
      NoteSent(g_sent_yaw, g_sent_pitch);
      // A new record: carry what the old one could not represent.
      g_send_yaw -= DecodeTurn(g_sent_yaw);
      g_send_pitch -= DecodeTurn(g_sent_pitch);
      g_sent_yaw = g_sent_pitch = 0;
      g_send_frame = frame;
    }
    // Where this player's camera is not turned by UpdateLookingAction
    // (vehicles, turrets, cutscenes) the mouse stays on the right stick,
    // which is lockstep input as well.
    if (!NativeActive())
    {
      return;
    }
    float dx, dy;
    TakeDelta(dx, dy);
    if (dx != 0.0f || dy != 0.0f)
    {
      uint32_t controller = g_local_controller;
      float fov = controller ? CameraFov(base, controller) : 0.0f;
      bool invert = controller && (LoadU32(base, controller + 2772) & 0x80000000) != 0;
      if (REXCVAR_GET(ao2_mouse_invert_y))
      {
        invert = !invert;
      }
      double units = UnitsPerCount(fov);
      g_send_yaw += dx * units;
      g_send_pitch += (invert ? dy : -dy) * units;
    }
    // An axis overwrites the record's previous value of this frame, so the
    // frame's running total is (re)sent on every poll while it is not zero.
    int yaw = EncodeTurn(g_send_yaw);
    int pitch = EncodeTurn(g_send_pitch);
    if (yaw != 0 || g_sent_yaw != 0)
    {
      InjectAxis(ctx, base, axis_obj, client, kNameMouseX, yaw);
    }
    if (pitch != 0 || g_sent_pitch != 0)
    {
      InjectAxis(ctx, base, axis_obj, client, kNameMouseY, pitch);
    }
    g_sent_yaw = yaw;
    g_sent_pitch = pitch;
    if (REXCVAR_GET(ao2_mouse_debug) && (yaw || pitch) && DebugLog())
    {
      uint32_t controller = g_local_controller;
      std::fprintf(DebugLog(), "send frame=%u yaw=%d pitch=%d (codes; left %.1f %.1f) ctrl=%08X fov=%.1f\n", frame,
                   yaw, pitch, g_send_yaw - DecodeTurn(yaw), g_send_pitch - DecodeTurn(pitch), controller,
                   controller ? CameraFov(base, controller) : 0.0f);
    }
  }
} // namespace

namespace ao2::mouse
{
  void Attach(rex::ui::Window *window)
  {
    if (!window || g_window)
    {
      return;
    }
    g_window = window;
    // Above the input drivers (0), below the ImGui overlays (64).
    window->AddInputListener(&g_listener, 32);
  }

  void InjectDelta(float dx, float dy)
  {
    std::lock_guard lock(g_mutex);
    if (clock::now() - g_last_take > kStaleDelta)
    {
      g_dx = 0.0f;
      g_dy = 0.0f;
    }
    g_dx += dx;
    g_dy += dy;
  }

  uint32_t LockstepController()
  {
    return g_local_controller;
  }

  void TurnTowards(float yaw, float pitch, float max_step)
  {
    // What this frame's record and the records not replayed yet will turn.
    double dyaw = yaw - g_send_yaw, dpitch = pitch - g_send_pitch;
    DropOldSent();
    for (const SentTurn &t : g_unapplied)
    {
      dyaw -= DecodeTurn(t.yaw);
      dpitch -= DecodeTurn(t.pitch);
    }
    double len = std::hypot(dyaw, dpitch);
    double k = len > max_step ? max_step / len : 1.0;
    g_send_yaw += dyaw * k;
    g_send_pitch += dpitch * k;
  }

  bool PredictedTurn(const uint8_t *base, uint32_t controller, uint32_t step, double alpha, bool interpolated,
                     float &pitch, float &yaw)
  {
    pitch = yaw = 0.0f;
    if (!REXCVAR_GET(ao2_mouse_aim) || !REXCVAR_GET(ao2_predict_camera) || !controller || !NativeActive())
    {
      return false;
    }
    bool lockstep = LockstepMouse(base);
    if (controller != (lockstep ? g_local_controller : g_single_controller))
    {
      return false;
    }
    // A cutscene's camera is not the player's: the mouse must not turn it.
    if (LoadU32(base, controller + 808) & kCinematicMode)
    {
      return false;
    }
    uint32_t holder = LoadU32(base, controller + 2024);
    if (!holder)
    {
      return false;
    }
    // The character's action sets the view (climbing over an obstacle).
    if (g_view_locked && LoadU32(base, controller + 452) == holder)
    {
      return false;
    }
    // Movement not taken yet, at this player's current zoom and invert.
    float dx, dy;
    {
      std::lock_guard lock(g_mutex);
      bool stale = clock::now() - g_last_take > kStaleDelta;
      dx = stale ? 0.0f : g_dx;
      dy = stale ? 0.0f : g_dy;
    }
    bool invert = (LoadU32(base, controller + 2772) & 0x80000000) != 0;
    if (REXCVAR_GET(ao2_mouse_invert_y))
    {
      invert = !invert;
    }
    double units = UnitsPerCount(CameraFov(base, controller));
    double ahead_yaw = dx * units;
    double ahead_pitch = (invert ? dy : -dy) * units;
    if (lockstep)
    {
      // This frame's turn (with what earlier codes could not carry) and the
      // finished frames' turns the replay has not reached.
      ahead_yaw += g_send_yaw;
      ahead_pitch += g_send_pitch;
      DropOldSent();
      for (const SentTurn &t : g_unapplied)
      {
        ahead_yaw += DecodeTurn(t.yaw);
        ahead_pitch += DecodeTurn(t.pitch);
      }
    }
    // The game keeps the look pitch within its limits; the prediction must
    // not look past them (it would spring back). The rotation holds all
    // turns applied so far.
    // On a turret or in a vehicle the limits are the gun's.
    uint32_t pawn = LoadU32(base, controller + 452);
    if (pawn && pawn != holder)
    {
      bool driven = IsDrivenVehicle(base, pawn);
      // Still getting on the turret (the view not on the gun yet).
      if (!driven && pawn == g_gun_view_pawn && step - g_gun_view_step <= 2 && !g_gun_view_on)
      {
        return false;
      }
      float turn_yaw = float(ahead_yaw), turn_pitch = float(ahead_pitch);
      TurretTurn(base, controller, pawn, turn_yaw, turn_pitch);
      if (driven)
      {
        DriverTurn(base, controller, turn_yaw, turn_pitch);
      }
      ahead_yaw = turn_yaw;
      ahead_pitch = turn_pitch;
    }
    else
    {
      ahead_pitch = KeptPitch(LoadF32(base, holder + 3620), float(ahead_pitch));
    }
    // A view between the last two steps shows only alpha of the turn applied
    // on the last one.
    if (interpolated && g_applied_step == step && g_applied_controller == controller)
    {
      ahead_yaw += (1.0 - alpha) * g_applied_yaw;
      ahead_pitch += (1.0 - alpha) * g_applied_pitch;
    }
    yaw = float(ahead_yaw);
    pitch = std::clamp(float(ahead_pitch), -kMaxPitchStep, kMaxPitchStep);
    return yaw != 0.0f || pitch != 0.0f;
  }

  void NoteView(const uint8_t *base, uint32_t controller, uint32_t step, float pitch, float yaw)
  {
    if (!controller || controller != (LockstepMouse(base) ? g_local_controller : g_single_controller))
    {
      return;
    }
    uint32_t pawn = LoadU32(base, controller + 452), holder = LoadU32(base, controller + 2024);
    if (!pawn || pawn == holder || IsDrivenVehicle(base, pawn))
    {
      g_gun_view_pawn = 0; // off the turret: getting on again starts over
      return;
    }
    // Mid-way the view is 30-60 units off still, an animation's start
    // thousands.
    constexpr float kOnGun = 24.0f; // rotation units
    float gun_pitch, gun_yaw;
    GunRotation(base, controller, pawn, gun_pitch, gun_yaw);
    float off_pitch = std::remainder(pitch - gun_pitch, 65536.0f), off_yaw = std::remainder(yaw - gun_yaw, 65536.0f);
    // Firing shakes the view off the gun as far (30-60 units every few
    // steps): once on the gun it stays on, or the prediction went on and
    // off with the shots and the view jumped by the whole predicted turn.
    bool on = (pawn == g_gun_view_pawn && g_gun_view_on) ||
              (std::fabs(off_pitch) <= kOnGun && std::fabs(off_yaw) <= kOnGun);
    if ((on != g_gun_view_on || pawn != g_gun_view_pawn) && REXCVAR_GET(ao2_mouse_debug) && DebugLog())
    {
      std::fprintf(DebugLog(), "step %u turret %08X view %s the gun (off by %.0f,%.0f)\n", step, pawn,
                   on ? "on" : "not on", off_pitch, off_yaw);
      std::fflush(DebugLog());
    }
    g_gun_view_pawn = pawn;
    g_gun_view_step = step;
    g_gun_view_on = on;
  }

  bool Driving()
  {
    return g_driving.load(std::memory_order_relaxed);
  }

  bool InFrontEnd()
  {
    return g_in_shell.load(std::memory_order_relaxed);
  }

  uint32_t AimTogglePawn(PPCContext &ctx, uint8_t *base, uint32_t user)
  {
    auto looked = clock::time_point(clock::duration(g_look_seen.load(std::memory_order_relaxed)));
    if (user >= 4)
    {
      return 0;
    }
    bool in_play = clock::now() - looked < kNativeIdle;
    uint32_t controller = LockstepMouse(base) ? LocalController(ctx, base) : ControllerOf(ctx, base, user);
    uint32_t holder = controller ? LoadU32(base, controller + 2024) : 0;
    uint32_t pawn = holder ? LoadU32(base, controller + 452) : 0;
    bool cinematic = controller && (LoadU32(base, controller + 808) & kCinematicMode);
    bool driven = pawn && pawn != holder && IsDrivenVehicle(base, pawn);
    uint32_t result = in_play && pawn && !cinematic && !driven ? pawn : 0;
    static uint32_t last_pawn = ~0u, last_result = ~0u;
    static bool last_play = false;
    if (REXCVAR_GET(ao2_mouse_debug) && DebugLog() &&
        (pawn != last_pawn || result != last_result || in_play != last_play))
    {
      std::fprintf(DebugLog(), "toggle aim pad %u: in play %d ctrl=%08X holder=%08X pawn=%08X (%s) cinematic %d driven %d -> %08X\n",
                   user, in_play ? 1 : 0, controller, holder, pawn, ClassName(base, pawn), cinematic ? 1 : 0,
                   driven ? 1 : 0, result);
      std::fflush(DebugLog());
    }
    last_pawn = pawn;
    last_result = result;
    last_play = in_play;
    return result;
  }

  bool HoldsGrenade(const uint8_t *base, uint32_t pawn)
  {
    constexpr uint32_t kPawnWeapon = 924; // Pawn.Weapon
    constexpr uint32_t kNames = 0x831D1724;
    static std::map<uint32_t, bool> by_class;
    uint32_t weapon = pawn ? LoadU32(base, pawn + kPawnWeapon) : 0;
    uint32_t cls = weapon ? LoadU32(base, weapon + 52) : 0;
    if (!cls)
    {
      return false;
    }
    auto it = by_class.find(cls);
    if (it != by_class.end())
    {
      return it->second;
    }
    uint32_t names = LoadU32(base, kNames);
    bool grenade = false;
    for (uint32_t c = cls, depth = 0; names && c && depth < 24 && !grenade; c = LoadU32(base, c + 60), ++depth)
    {
      uint32_t entry = LoadU32(base, names + LoadU32(base, c + 44) * 4);
      grenade = entry && std::strncmp(reinterpret_cast<const char *>(base + entry + 16), "AO2Grenade", 10) == 0;
    }
    if (REXCVAR_GET(ao2_mouse_debug) && DebugLog())
    {
      std::fprintf(DebugLog(), "weapon class %s: grenade %d\n", ClassName(base, weapon), grenade ? 1 : 0);
      std::fflush(DebugLog());
    }
    if (by_class.size() > 256)
    {
      by_class.clear();
    }
    return by_class[cls] = grenade;
  }

  void NoteToggleAim(uint32_t user, uint8_t trigger, bool on)
  {
    static bool last_down[4] = {}, last_on[4] = {};
    bool down = trigger >= 30;
    if (user < 4 && (down != last_down[user] || on != last_on[user]) && REXCVAR_GET(ao2_mouse_debug) && DebugLog())
    {
      std::fprintf(DebugLog(), "toggle aim pad %u: trigger %u -> aiming %d\n", user, trigger, on ? 1 : 0);
      std::fflush(DebugLog());
    }
    if (user < 4)
    {
      last_down[user] = down;
      last_on[user] = on;
    }
  }

  uint32_t KeyboardUser()
  {
    using Fn = uint32_t (*)();
    static Fn fn = []() -> Fn {
      HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
      return runtime ? reinterpret_cast<Fn>(GetProcAddress(runtime, "rex_input_keyboard_user")) : nullptr;
    }();
    return fn ? fn() : 0;
  }

  bool TakeStickFallback(int32_t &rx, int32_t &ry)
  {
    // The game turns turrets, vehicle cameras etc. from the stick through
    // its aiming graph (AIMS_*: dead zone 0.11-0.12, then the push to the
    // power 2-3, times LinearScale, and an acceleration while pushed past
    // ~0.9). A stick deflection proportional to the mouse movement lost
    // small movements in the dead zone and made larger ones jump. The mouse
    // speed is turned into the deflection that undoes that curve, so the
    // turn speed follows the mouse linearly: full speed at
    // kStickFullCountsPerSecond counts per second (x ao2_mouse_stick_sensitivity),
    // kept under the acceleration threshold.
    constexpr double kDeadZone = 0.125;
    constexpr double kExponent = 2.0;
    constexpr double kMaxPush = 0.88;
    constexpr double kStickFullCountsPerSecond = 1500.0;
    static clock::time_point last;
    static int32_t held_x = 0, held_y = 0;
    if (NativeActive())
    {
      held_x = held_y = 0;
      return false;
    }
    // In the main menu and the lobby the right stick turns their camera:
    // the mouse is for the menus there, its movement is dropped.
    auto looked = clock::time_point(clock::duration(g_look_seen.load(std::memory_order_relaxed)));
    if (clock::now() - looked >= kNativeIdle)
    {
      float dx, dy;
      TakeDelta(dx, dy);
      held_x = held_y = 0;
      return false;
    }
    auto now = clock::now();
    double dt = std::chrono::duration<double>(now - last).count();
    if (dt < 0.008 && (held_x || held_y))
    {
      // Polled again within the frame: the same deflection.
      rx = held_x;
      ry = held_y;
      return true;
    }
    last = now;
    float dx, dy;
    TakeDelta(dx, dy);
    if (dx == 0.0f && dy == 0.0f)
    {
      held_x = held_y = 0;
      return false;
    }
    dt = std::clamp(dt, 0.008, 0.05);
    double full = kStickFullCountsPerSecond / REXCVAR_GET(ao2_mouse_stick_sensitivity);
    auto push = [&](double counts) -> int32_t {
      if (counts == 0.0)
      {
        return 0;
      }
      double speed = std::min(std::fabs(counts) / dt / full, 1.0);
      double deflection = kDeadZone + (kMaxPush - kDeadZone) * std::pow(speed, 1.0 / kExponent);
      return int32_t(std::copysign(deflection * 32767.0, counts));
    };
    held_x = rx = push(dx);
    held_y = ry = push(-dy);
    return true;
  }
} // namespace ao2::mouse

// Engine pad polling (r3 = client): sticks and buttons go to the viewport.
extern "C" REX_FUNC(__imp__sub_82A95F70);
extern "C" REX_FUNC(sub_82A95F70)
{
  uint32_t client = ctx.r3.u32;
  // During the startup and loading movies other threads (the movie and
  // loading loops) poll the pads too: in a loop with nothing that waits (tens
  // of thousands of times per second, a whole core), sending every stick and
  // button to the viewport client's input receiver (+52) - the movie player,
  // which takes itself out (null) when the movie ends, or a client the game
  // thread is replacing. Now and then such a thread caught that in the middle
  // and crashed, more often the faster the code runs. Only the game thread
  // (Main XThread, once per frame) goes through the engine now; the other
  // threads sleep a little per poll and only skip movies (PollMovieThread:
  // the player's only use of input).
  thread_local int main_thread = -1;
  if (main_thread < 0)
  {
    main_thread = 0;
    PWSTR name = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &name)) && name)
    {
      main_thread = std::wcsstr(name, L"Main XThread") ? 1 : 0;
      LocalFree(name);
    }
  }
  uint32_t receiver = client ? LoadU32(base, client + 52) : 0;
  if (!main_thread)
  {
    if (receiver && ao2::skip::IsMovieReceiver(base, receiver))
    {
      ao2::skip::PollMovieThread(ctx, base);
    }
    else
    {
      ao2::skip::PollWhileMovie(ctx, base);
    }
    Sleep(1);
    return;
  }
  g_in_shell.store(InShell(base), std::memory_order_relaxed);
  ao2::prompts::Tick(base);
  ao2::cyrillic::Tick(base);
  if (!receiver)
  {
    return;
  }
  ao2::cheats::Tick(ctx, base);
  ao2::coop::ForceLod(ctx, base);
  ao2::coop::BlockStreaming(ctx, base);
  ao2::skip::Poll(ctx, base);
  ao2::console::Poll(ctx, base);
  ao2::coop::cmd::Poll(ctx, base);
  ao2::autopilot::Tick(ctx, base);
  __imp__sub_82A95F70(ctx, base);
  if (REXCVAR_GET(ao2_mouse_aim) && LockstepMouse(base))
  {
    SendLockstepMouse(ctx, base, client);
  }
  // For the pad hook (input_script.cpp): W / S as the triggers at the wheel,
  // for the player with the keyboard.
  uint32_t player = 0;
  if (!LockstepMouse(base))
  {
    player = KeyboardController(ctx, base);
  }
  if (!player)
  {
    player = LocalController(ctx, base);
  }
  uint32_t pawn = player ? LoadU32(base, player + 452) : 0;
  g_driving.store(pawn && pawn != LoadU32(base, player + 2024) && IsDrivenVehicle(base, pawn),
                  std::memory_order_relaxed);
}

// Pad polling: one button or axis to the viewport's input receiver (r3 =
// viewport client - 4, receiver at +56). During the startup movies the
// receiver can still be (or again be) null while input is polled; a key,
// button or mouse movement then crashed calling through it.
extern "C" REX_FUNC(__imp__sub_82A95D70);
extern "C" REX_FUNC(sub_82A95D70)
{
  if (!ctx.r3.u32 || !LoadU32(base, ctx.r3.u32 + 56))
  {
    return;
  }
  __imp__sub_82A95D70(ctx, base);
}

// Player manager interaction InputAxis (r4 = controller id, r5 = key name,
// f1 = value): replayed lockstep axes reach the player's controller here.
extern "C" REX_FUNC(__imp__sub_82756040);
extern "C" REX_FUNC(sub_82756040)
{
  uint64_t key = ctx.r5.u64;
  bool x = key == LoadName(base, kNameMouseX);
  if (!(x || key == LoadName(base, kNameMouseY)) || !LockstepMouse(base))
  {
    __imp__sub_82756040(ctx, base);
    return;
  }
  int code = int(std::clamp<long long>(std::llround(ctx.f1.f64 * 127.0), -kTurnCodeMax, kTurnCodeMax));
  ReplayedTurn(ctx, base, ctx.r4.u32, x, code);
  ctx.r3.u64 = 1; // handled
}

// Lockstep replay of one player's frame record (r3 = record). The record's
// axes (+248, 8 bytes each: key table index, int(value * 127); count at
// +312) go through the viewport's input chain, where an active UI scene
// takes MouseX - in the co-op snipe the yaw stayed blocked until both
// players had aimed. The mouse turn is taken from the record itself.
extern "C" REX_FUNC(__imp__sub_82A0EEC8);
extern "C" REX_FUNC(sub_82A0EEC8)
{
  uint32_t record = ctx.r3.u32;
  bool online = record && ao2::coop::OnlineLockstep(base);
  bool mouse = record && REXCVAR_GET(ao2_mouse_aim) && LockstepMouse(base);
  bool commands_on = record && ao2::coop::cmd::Ready(base);
  uint32_t controller_id = 0;
  if (online || mouse || commands_on)
  {
    // The record's controller id: +592 in a network game, else +588 (as
    // the replay picks it, sub_82A0EC20 on the frame sync object).
    PPCContext saved = ctx;
    ctx.r3.u64 = LoadU32(base, kFrameSync);
    rex::runtime::ResolveIndirectFunction(kSyncIsNetwork)(ctx, base);
    bool network = ctx.r3.u32 != 0;
    ctx = saved;
    controller_id = LoadU32(base, record + (network ? 592 : 588));
  }
  if (mouse)
  {
    uint32_t count = std::min<uint32_t>(LoadU32(base, record + 312), 8);
    for (uint32_t i = 0; i < count; ++i)
    {
      uint32_t key = LoadU32(base, record + 248 + i * 8);
      if (key == kKeyMouseX || key == kKeyMouseY)
      {
        int code = std::clamp(int(int32_t(LoadU32(base, record + 252 + i * 8))), -kTurnCodeMax, kTurnCodeMax);
        ReplayedTurn(ctx, base, controller_id, key == kKeyMouseX, code);
      }
    }
  }
  // Debug menu commands in the record (coop_cmd.cpp): out of it while the
  // game replays it.
  ao2::coop::cmd::Replay commands;
  if (commands_on)
  {
    ao2::coop::cmd::BeforeReplay(base, record, controller_id, commands);
  }
  PPCContext saved = ctx;
  __imp__sub_82A0EEC8(ctx, base);
  if (online || commands_on)
  {
    PPCContext after = ctx;
    ctx.r1 = saved.r1;
    if (online)
    {
      // A cutscene skip in the record (cutscene_skip.cpp), before the
      // commands are back in it (a symbol can be F12's key index).
      ao2::skip::OnReplayedRecord(ctx, base, record, controller_id);
    }
    if (commands_on)
    {
      ao2::coop::cmd::AfterReplay(ctx, base, commands);
    }
    ctx = after;
  }
}

// AAO2PlayerControllerNative::UpdateLookingAction (r3 = controller, f1 = dt).
extern "C" REX_FUNC(__imp__sub_82365150);
extern "C" REX_FUNC(sub_82365150)
{
  uint32_t controller = ctx.r3.u32;
  uint32_t holder = controller ? LoadU32(base, controller + 2024) : 0;
  if (!InShell(base))
  {
    g_look_seen.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
  }
  // Driving a vehicle (the controller's pawn +452 is the vehicle, not the
  // character) this still runs, but the view is the vehicle's camera: a
  // mouse turn added here showed nowhere, and its prediction turned the
  // drawn view for a moment and let it spring back.
  if (!REXCVAR_GET(ao2_mouse_aim) || !holder || LoadU32(base, controller + 452) != holder)
  {
    __imp__sub_82365150(ctx, base);
    return;
  }
  if (LockstepMouse(base))
  {
    // Online co-op: the turn this controller's player sent for this frame,
    // replayed identically on both machines (see the top of this file).
    if (controller == g_local_controller)
    {
      g_native_seen.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
      g_view_locked = !PlayerTurnsView(ctx, base, holder);
      LearnPitchLimits(base, holder, LoadF32(base, holder + 3620));
    }
    auto it = g_pending.find(controller);
    if (it != g_pending.end())
    {
      PendingTurn p = it->second;
      g_pending.erase(it);
      if (p.frame == ao2::coop::SyncFrame(base) && (p.yaw || p.pitch))
      {
        uint32_t rotation = holder + 3620;
        float pitch_step =
            std::clamp(float(DecodeTurn(p.pitch)), -kMaxPitchStep, kMaxPitchStep);
        float pitch = LoadF32(base, rotation);
        StoreF32(base, rotation + 4, LoadF32(base, rotation + 4) + float(DecodeTurn(p.yaw)));
        StoreF32(base, rotation, pitch + pitch_step);
        EndViewReturn(base, controller, float(DecodeTurn(p.yaw)), pitch_step);
        if (controller == g_local_controller)
        {
          NoteReplayed(p.yaw, p.pitch);
          NoteApplied(controller, float(DecodeTurn(p.yaw)), KeptPitch(pitch, pitch_step));
          NotePitchTurn(pitch, pitch_step);
        }
      }
    }
    __imp__sub_82365150(ctx, base);
    return;
  }
  if (!TakesMouse(ctx, base, controller))
  {
    __imp__sub_82365150(ctx, base);
    return;
  }
  g_native_seen.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
  g_view_locked = !PlayerTurnsView(ctx, base, holder);

  float dx, dy;
  TakeDelta(dx, dy);
  uint32_t rotation = holder + 3620;
  float pitch = LoadF32(base, rotation);
  float yaw = LoadF32(base, rotation + 4);
  LearnPitchLimits(base, holder, pitch);
  bool invert = (LoadU32(base, controller + 2772) & 0x80000000) != 0;
  if (REXCVAR_GET(ao2_mouse_invert_y))
  {
    invert = !invert;
  }
  float fov = CameraFov(base, controller);
  if (dx != 0.0f || dy != 0.0f)
  {
    double units =
        kDegreesPerCount * REXCVAR_GET(ao2_mouse_sensitivity) * kRotationUnitsPerDegree * ZoomScale(fov);
    float pitch_step = std::clamp(float((invert ? dy : -dy) * units), -kMaxPitchStep, kMaxPitchStep);
    StoreF32(base, rotation + 4, float(yaw + dx * units));
    StoreF32(base, rotation, pitch + pitch_step);
    EndViewReturn(base, controller, float(dx * units), pitch_step);
    NoteApplied(controller, float(dx * units), KeptPitch(pitch, pitch_step));
    NotePitchTurn(pitch, pitch_step);
  }
  else
  {
    NoteApplied(controller, 0.0f, 0.0f);
  }
  g_single_controller = controller;
  ao2::autopilot::Aim(base, controller, float(ctx.f1.f64));

  bool debug = REXCVAR_GET(ao2_mouse_debug);
  static uint32_t calls = 0;
  bool log = debug && (dx != 0.0f || dy != 0.0f || calls % 60 == 0);
  ++calls;
  double dt = ctx.f1.f64, f2 = ctx.f2.f64;
  uint32_t r5 = ctx.r5.u32, r7 = ctx.r7.u32;

  __imp__sub_82365150(ctx, base);

  if (log && DebugLog())
  {
    uint32_t aim = LoadU32(base, controller + 2004);
    std::fprintf(DebugLog(),
                 "#%u ctrl=%08X holder=%08X aim=%08X dt=%.4f f2=%.3f r5=%08X r7=%u mouse=(%.1f,%.1f) "
                 "fov=%.1f rot (%.1f,%.1f) -> (%.1f,%.1f,%.1f) out (%.1f,%.1f) inv=%d",
                 calls, controller, holder, aim, dt, f2, r5, r7, dx, dy, fov, pitch, yaw, LoadF32(base, rotation),
                 LoadF32(base, rotation + 4), LoadF32(base, rotation + 8), LoadF32(base, controller + 1760),
                 LoadF32(base, controller + 1764), invert ? 1 : 0);
    if (aim)
    {
      std::fprintf(DebugLog(), " params");
      for (uint32_t i = 0; i < 10; ++i)
      {
        std::fprintf(DebugLog(), " %.3f", LoadF32(base, aim + 220 + i * 4));
      }
    }
    std::fprintf(DebugLog(), "\n");
    std::fflush(DebugLog());
  }
}

// Script native of the aiming graph ([controller+2004]): (input object, yaw
// intention, pitch intention, dt) -> this frame's turn from the right stick
// (x = yaw, y = pitch, rotation units). UpdateLookingAction calls the graph
// itself (sub_823329C0); the scripts call it through this thunk where they
// turn the view themselves - on a turret or in a vehicle, when the
// controller's pawn (+452) is not the character (+2024). The mouse turn is
// added to the stick's there.
extern "C" REX_FUNC(__imp__sub_822F66C0);
extern "C" REX_FUNC(sub_822F66C0)
{
  uint32_t aim = ctx.r3.u32;
  uint32_t result = ctx.r5.u32;
  __imp__sub_822F66C0(ctx, base);
  if (!REXCVAR_GET(ao2_mouse_aim) || !aim || !result)
  {
    return;
  }
  PPCContext saved = ctx;
  uint32_t controller = 0;
  for (uint32_t id = 0; id < 4 && !controller; ++id)
  {
    uint32_t c = ControllerOf(ctx, base, id);
    if (c && LoadU32(base, c + 2004) == aim)
    {
      controller = c;
    }
  }
  ctx = saved;
  uint32_t pawn = controller ? LoadU32(base, controller + 452) : 0;
  uint32_t holder = controller ? LoadU32(base, controller + 2024) : 0;
  bool debug = REXCVAR_GET(ao2_mouse_debug) && DebugLog();
  if (!pawn || !holder || pawn == holder)
  {
    return;
  }
  // On a turret UpdateLookingAction does not run: this is the sign of play.
  if (!InShell(base))
  {
    g_look_seen.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
  }
  UnlagTurret(base, pawn);
  float yaw = 0.0f, pitch = 0.0f;
  if (LockstepMouse(base))
  {
    if (controller == g_local_controller)
    {
      g_native_seen.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    }
    auto it = g_pending.find(controller);
    if (it == g_pending.end())
    {
      return;
    }
    PendingTurn p = it->second;
    g_pending.erase(it);
    if (p.frame != ao2::coop::SyncFrame(base))
    {
      return;
    }
    yaw = float(DecodeTurn(p.yaw));
    pitch = std::clamp(float(DecodeTurn(p.pitch)), -kMaxPitchStep, kMaxPitchStep);
    if (controller == g_local_controller && (p.yaw || p.pitch))
    {
      NoteReplayed(p.yaw, p.pitch);
    }
  }
  else
  {
    if (!TakesMouse(ctx, base, controller))
    {
      return;
    }
    g_native_seen.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    g_single_controller = controller;
    float dx, dy;
    TakeDelta(dx, dy);
    bool invert = (LoadU32(base, controller + 2772) & 0x80000000) != 0;
    if (REXCVAR_GET(ao2_mouse_invert_y))
    {
      invert = !invert;
    }
    double units = UnitsPerCount(CameraFov(base, controller));
    yaw = float(dx * units);
    pitch = std::clamp(float((invert ? dy : -dy) * units), -kMaxPitchStep, kMaxPitchStep);
  }
  if (controller == (LockstepMouse(base) ? g_local_controller : g_single_controller))
  {
    // What of it will show: the game stops the rotation at the gun's limits.
    float shown_yaw = yaw, shown_pitch = pitch;
    TurretTurn(base, controller, pawn, shown_yaw, shown_pitch);
    NoteApplied(controller, shown_yaw, shown_pitch);
  }
  if (yaw != 0.0f || pitch != 0.0f)
  {
    StoreF32(base, result, LoadF32(base, result) + yaw);
    StoreF32(base, result + 4, LoadF32(base, result + 4) + pitch);
    if (debug)
    {
      std::fprintf(DebugLog(), "graph mouse rot %d,%d ctrl=%08X yaw=%.1f pitch=%.1f fov=%.1f\n", int32_t(LoadU32(base, controller + 240)),
                   int32_t(LoadU32(base, controller + 244)), controller, yaw, pitch,
                   CameraFov(base, controller));
    }
  }
}

// The camera modifier that turns a camera from the player's input
// (AO2CameraModifier_ApplyPlayerInput, r3; r4 = the camera actor, f1 = dt):
// at the wheel of a vehicle the right stick turns the driver's look-around
// camera through it (AO2CameraModel_VehicleDriver_Freelook's RotateCam). It
// hands the input's turn to the modifier after it in the camera's list
// (+740, count +744) - the rotation offset (DriverTurn).
thread_local uint32_t t_input_modifier = 0, t_input_camera = 0;
extern "C" REX_FUNC(__imp__sub_82382EF8);
extern "C" REX_FUNC(sub_82382EF8)
{
  uint32_t outer_modifier = t_input_modifier, outer_camera = t_input_camera;
  t_input_modifier = ctx.r3.u32;
  t_input_camera = ctx.r4.u32;
  __imp__sub_82382EF8(ctx, base);
  t_input_modifier = outer_modifier;
  t_input_camera = outer_camera;
}

// AO2PlayerInput: the turn speed from two intentions (r3 = result FVector:
// x = the first, yaw, y = the second, pitch - rotation units per second once
// the modifiers at r8 have scaled the stick; r4 = the input object, whose
// Outer +40 is the controller; r5, r6 = the intentions' names; f1 = dt). The
// modifier above gives it to its camera's rotation offset as the velocity
// of this frame: the mouse turn of a driver, as the speed that makes it in
// dt, is added to the stick's.
extern "C" REX_FUNC(__imp__sub_8233FE78);
extern "C" REX_FUNC(sub_8233FE78)
{
  uint32_t result = ctx.r3.u32, input = ctx.r4.u32;
  float dt = float(ctx.f1.f64);
  __imp__sub_8233FE78(ctx, base);
  uint32_t camera = t_input_camera;
  if (!REXCVAR_GET(ao2_mouse_aim) || !result || !input || !camera || !(dt > 0.0001f))
  {
    return;
  }
  // The modifier after the input's in the camera's list.
  uint32_t offset = 0;
  uint32_t list = LoadU32(base, camera + 740), count = std::min<uint32_t>(LoadU32(base, camera + 744), 32);
  for (uint32_t i = 0; list && i + 1 < count; ++i)
  {
    if (LoadU32(base, list + i * 4) == t_input_modifier)
    {
      offset = LoadU32(base, list + i * 4 + 4);
      break;
    }
  }
  if (!offset)
  {
    return;
  }
  uint32_t controller = LoadU32(base, input + 40);
  if (!controller || LoadU32(base, controller + 1136) != input)
  {
    return;
  }
  uint32_t pawn = LoadU32(base, controller + 452);
  uint32_t holder = LoadU32(base, controller + 2024);
  if (!pawn || !holder || pawn == holder || !IsDrivenVehicle(base, pawn) ||
      (LoadU32(base, controller + 808) & kCinematicMode))
  {
    return;
  }
  float yaw = 0.0f, pitch = 0.0f;
  if (LockstepMouse(base))
  {
    if (controller == g_local_controller)
    {
      g_native_seen.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    }
    auto it = g_pending.find(controller);
    if (it == g_pending.end())
    {
      return;
    }
    PendingTurn turn = it->second;
    g_pending.erase(it);
    if (turn.frame != ao2::coop::SyncFrame(base))
    {
      return;
    }
    yaw = float(DecodeTurn(turn.yaw));
    pitch = std::clamp(float(DecodeTurn(turn.pitch)), -kMaxPitchStep, kMaxPitchStep);
    if (controller == g_local_controller && (turn.yaw || turn.pitch))
    {
      NoteReplayed(turn.yaw, turn.pitch);
    }
  }
  else
  {
    if (!TakesMouse(ctx, base, controller))
    {
      return;
    }
    g_native_seen.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    g_single_controller = controller;
    float dx, dy;
    TakeDelta(dx, dy);
    bool invert = (LoadU32(base, controller + 2772) & 0x80000000) != 0;
    if (REXCVAR_GET(ao2_mouse_invert_y))
    {
      invert = !invert;
    }
    double units = UnitsPerCount(CameraFov(base, controller));
    yaw = float(dx * units);
    pitch = std::clamp(float((invert ? dy : -dy) * units), -kMaxPitchStep, kMaxPitchStep);
  }
  if (controller == (LockstepMouse(base) ? g_local_controller : g_single_controller))
  {
    g_driver_controller = controller;
    g_driver_offset = offset;
    // What of it will show: the offset stops at its limits.
    float shown_yaw = yaw, shown_pitch = pitch;
    DriverTurn(base, controller, shown_yaw, shown_pitch);
    NoteApplied(controller, shown_yaw, shown_pitch);
  }
  if (yaw != 0.0f || pitch != 0.0f)
  {
    if (REXCVAR_GET(ao2_mouse_debug) && DebugLog())
    {
      std::fprintf(DebugLog(), "driver ctrl=%08X offset %d,%d stick %.1f,%.1f mouse %.1f,%.1f dt=%.4f\n", controller,
                   int32_t(LoadU32(base, offset + 80)), int32_t(LoadU32(base, offset + 84)), LoadF32(base, result),
                   LoadF32(base, result + 4), yaw, pitch, dt);
    }
    StoreF32(base, result, LoadF32(base, result) + yaw / dt);
    StoreF32(base, result + 4, LoadF32(base, result + 4) + pitch / dt);
  }
}
