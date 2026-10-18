// aim_assist.cpp - Turning the game's aim assist off (ao2_aim_assist).
//
// The game helps the player aim in two ways, both only while aiming
// (precision, the left trigger / right mouse button) and only with a weapon
// whose AO2Weapon.bAllowAimAssist is set:
// - Snap on entry: entering the aim, the camera starts on a target in a cone
//   around the view (AO2CharacterNative.Precision_Camera_TargetOnEntry_*: 5
//   deg, 4-150 m). The script AO2Character.PreInitCameraModel asks the
//   player controller's native FindTargetInCone (exec sub_823066F8, returns
//   a float; > 0 = found) and sets the camera model's StartingFocusPoint to
//   the target. Nothing else in the loaded script calls FindTargetInCone.
// - Bullet magnetism: the shot's aim point moves onto a target within
//   AO2PlayerControllerNative.Precision_AimAssist_MaxAngleDegrees (+2652, 2
//   deg, up to 100 m) of the crosshair. sub_8233EF00 (r3 = controller)
//   computes the aim point; it skips the assist when that angle is not above
//   0 (CalculateAimAssistTarget, sub_8233E130).
// The third, sticky aim (the stick turns slower over a target, sub_8233DEE8
// in UpdateLookingAction), only scales the stick and is set to no effect in
// this game (Precision_StickyAim_MaxAngleDegrees 0); the mouse turns the
// camera past it anyway (mouse_aim.cpp).
//
// Both change the simulation (the camera's rotation, where the shot goes),
// so in online co-op (a lockstep simulation, coop.h) both machines must know
// each player's choice: it goes through the lockstep input as the command
// "aimassist 0|1" (coop_cmd.cpp), and the hooks below look up the player of
// the controller. Each player keeps their own setting. Off by default (also
// the co-op default until a player's command arrives; both machines alike).
#include "aim_assist.h"

#include "coop.h"
#include "coop_cmd.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(ao2_aim_assist, false, "Input",
                    "The game's aim assist while aiming: the camera starts on a target and shots are pulled onto "
                    "targets near the crosshair (in online co-op each player's own)");

namespace
{
  constexpr uint32_t kEngine = 0x831EFCBC;                  // GEngine
  constexpr uint32_t kPrecisionAimAssistMaxAngle = 2652;    // AO2PlayerControllerNative, float degrees

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }
  void StoreU32(uint8_t *base, uint32_t addr, uint32_t value)
  {
    value = std::byteswap(value);
    std::memcpy(base + addr, &value, 4);
  }

  // Online co-op: each player's aim assist by controller id (game thread).
  // Until a player's command arrives it is off (ao2_aim_assist's default) -
  // the same on both machines.
  bool g_player_assist[4] = {};

  // The PlayerController of a controller id: GEngine +772 =
  // GameViewportClient, vtable +268 finds the local player of a controller
  // id, LocalPlayer +64 = its PlayerController.
  uint32_t ControllerOf(PPCContext &ctx, uint8_t *base, uint32_t id)
  {
    uint32_t engine = LoadU32(base, kEngine);
    uint32_t client = engine ? LoadU32(base, engine + 772) : 0;
    uint32_t vtable = client ? LoadU32(base, client) : 0;
    if (!vtable)
    {
      return 0;
    }
    PPCContext saved = ctx;
    ctx.r3.u64 = client;
    ctx.r4.u64 = id;
    rex::runtime::ResolveIndirectFunction(LoadU32(base, vtable + 268))(ctx, base);
    uint32_t player = ctx.r3.u32;
    ctx = saved;
    return player ? LoadU32(base, player + 64) : 0;
  }

  // Whether the player of this controller has the aim assist.
  bool AssistOn(PPCContext &ctx, uint8_t *base, uint32_t controller)
  {
    if (!ao2::coop::cmd::Routed(base))
    {
      // Every player of this PC (split-screen too).
      return REXCVAR_GET(ao2_aim_assist);
    }
    for (uint32_t id = 0; id < 4; ++id)
    {
      if (controller && ControllerOf(ctx, base, id) == controller)
      {
        return g_player_assist[id];
      }
    }
    return false;
  }
} // namespace

namespace ao2::aim_assist
{
  void SetPlayer(uint32_t controller_id, bool on)
  {
    if (controller_id < 4)
    {
      g_player_assist[controller_id] = on;
    }
  }

  void Reset()
  {
    std::fill(std::begin(g_player_assist), std::end(g_player_assist), false);
  }
} // namespace ao2::aim_assist

// The shot's aim point (r3 = controller): without the assist, as with a
// zero cone (the value is put back afterwards).
extern "C" REX_FUNC(__imp__sub_8233EF00);
extern "C" REX_FUNC(sub_8233EF00)
{
  uint32_t controller = ctx.r3.u32;
  if (!controller || AssistOn(ctx, base, controller))
  {
    __imp__sub_8233EF00(ctx, base);
    return;
  }
  uint32_t angle = LoadU32(base, controller + kPrecisionAimAssistMaxAngle);
  StoreU32(base, controller + kPrecisionAimAssistMaxAngle, 0); // 0.0f
  __imp__sub_8233EF00(ctx, base);
  StoreU32(base, controller + kPrecisionAimAssistMaxAngle, angle);
}

// AO2PlayerControllerNative::execFindTargetInCone (r3 = controller, r5 =
// result): without the assist no target is found (0.0f).
extern "C" REX_FUNC(__imp__sub_823066F8);
extern "C" REX_FUNC(sub_823066F8)
{
  uint32_t controller = ctx.r3.u32;
  uint32_t result = ctx.r5.u32;
  bool on = AssistOn(ctx, base, controller);
  __imp__sub_823066F8(ctx, base);
  if (!on && result)
  {
    StoreU32(base, result, 0);
  }
}
