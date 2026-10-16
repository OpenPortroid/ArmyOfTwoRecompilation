// cheats.cpp - Debug cheats.
//
// God mode is Unreal Engine 3's own: Controller.bGodMode (the BoolProperty
// at +468, bit 0x40000000 in this build, found through the property's
// reflection data) makes Pawn.InGodMode() true, and the game's damage path
// then drops the damage (the cheat manager's "God" and Kismet's
// SeqAct_ToggleGodMode set the same flag). The flag is set on the local
// player's controller every frame, so it survives respawns and level loads;
// turned off (the F1 debug menu), it is cleared once.
//
// Online co-op is a lockstep simulation (coop.h): damage ignored on one
// machine only would desync the game. There ao2_god_mode is the player's
// wish, sent to both machines through the lockstep input (coop_cmd.cpp):
// both turn the flag of that player's controller on or off on the same
// frame (SetPlayerGod) and keep it set at every replay of that player's
// frame record. ao2_coop_god_mode (for testing co-op) sets the flag on
// every player's controller on each simulation step - the same on both
// machines, so both PCs must have the same value.
#include "cheats.h"

#include "coop.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(ao2_god_mode, false, "Cheats",
                    "Debug: the player takes no damage (in online co-op through the lockstep input, for this player)");
REXCVAR_DEFINE_BOOL(ao2_coop_god_mode, false, "Cheats",
                    "Debug, online co-op: both players take no damage; must be the same on both PCs");

namespace
{
  constexpr uint32_t kEngine = 0x831EFCBC;          // GEngine
  constexpr uint32_t kControllerFlags = 468;        // AController bitfield word
  constexpr uint32_t kGodModeBit = 0x40000000;      // bGodMode

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

  // The PlayerController of the first local player: GEngine +772 =
  // GameViewportClient, vtable +268 finds the local player of a controller
  // id, LocalPlayer +64 = its PlayerController.
  uint32_t LocalController(PPCContext &ctx, uint8_t *base, uint32_t id = 0)
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

  // Online co-op god mode per controller id (coop_cmd.cpp; game thread).
  bool g_player_god[4] = {};

  void SetGodFlag(uint8_t *base, uint32_t controller, bool on)
  {
    if (!controller)
    {
      return;
    }
    uint32_t flags = LoadU32(base, controller + kControllerFlags);
    uint32_t wanted = on ? flags | kGodModeBit : flags & ~kGodModeBit;
    if (wanted != flags)
    {
      StoreU32(base, controller + kControllerFlags, wanted);
    }
  }
} // namespace

namespace ao2::cheats
{
  void Tick(PPCContext &ctx, uint8_t *base)
  {
    auto god = [&](uint32_t controller) { SetGodFlag(base, controller, true); };
    if (!ao2::coop::InNetworkGame(base))
    {
      std::fill(std::begin(g_player_god), std::end(g_player_god), false);
    }
    if (ao2::coop::OnlineLockstep(base))
    {
      // Every player (controller ids 0-3: both machines number them alike),
      // once per simulation step on both machines.
      if (REXCVAR_GET(ao2_coop_god_mode))
      {
        for (uint32_t id = 0; id < 4; ++id)
        {
          god(LocalController(ctx, base, id));
        }
      }
      return;
    }
    // Whether ao2_god_mode set the flag (and it was not cleared since).
    static bool applied = false;
    bool wanted = REXCVAR_GET(ao2_god_mode) && !ao2::coop::InNetworkGame(base);
    if (!wanted && !applied)
    {
      return;
    }
    uint32_t controller = LocalController(ctx, base);
    if (!controller)
    {
      return;
    }
    if (wanted)
    {
      god(controller);
    }
    else
    {
      SetGodFlag(base, controller, false);
    }
    applied = wanted;
  }

  void SetPlayerGod(PPCContext &ctx, uint8_t *base, uint32_t controller_id, bool on)
  {
    if (controller_id >= 4)
    {
      return;
    }
    g_player_god[controller_id] = on;
    if (on || !REXCVAR_GET(ao2_coop_god_mode))
    {
      SetGodFlag(base, LocalController(ctx, base, controller_id), on);
    }
  }

  void OnReplayedRecord(PPCContext &ctx, uint8_t *base, uint32_t controller_id)
  {
    if (controller_id < 4 && g_player_god[controller_id])
    {
      SetGodFlag(base, LocalController(ctx, base, controller_id), true);
    }
  }
} // namespace ao2::cheats
