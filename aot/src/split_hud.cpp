// split_hud.cpp - The HUD of a split-screen: the aggro meter and the match timer.
//
// Every AO2HUD draws its AO2HudItem_Aggro (UAO2HudItem_Aggro::Draw, sub_823AF0A8,
// vtable +272; the player's own Character.GlobalAggroRating). With two views in the
// frame (sub_82611D58: the SceneView's family has 2 views) the meter is one the two
// players share: only the lower part of the screen draws it (SceneView +32 not 0),
// sideways at the seam. Two players on different teams do not share their aggro, so
// for them Draw is told there is one view: each part of the screen gets the meter of
// a whole screen, at the top of that part, for its own player.
//
// Teams: PlayerReplicationInfo.Team of the two players on the screen (LocalPlayer
// Origin x < 1: the others of a versus are off it). Guests are the only way to a
// split-screen in versus, so nothing is looked up without one. The MP faces on the
// meter (AO2HudItem_MPAggroIcons) still skip every split-screen.
//
// The timer (UAO2HudItem_ObjectiveTimer::Draw, sub_823B3748) is drawn by the lower
// part alone, half a line under its top. It goes on the line between the parts, where
// the meter does not cover it: the text's CurY, set by Draw, is moved up by the height
// of the text just before it is drawn (sub_8270A9A8).
#include "ue3_reflect.h"

#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/ppc.h>

namespace ao2::versus_guest
{
  uint32_t GuestCount(); // versus_guest.cpp
}

namespace
{
  constexpr uint32_t kEngine = 0x831EFCBC; // GEngine
  constexpr uint32_t kMaxPlayers = 4;

  constexpr uint32_t kHudOwner = 76;      // AO2HudItem
  constexpr uint32_t kSafeZoneTop = 1792; // AO2HUD
  constexpr uint32_t kOrgY = 76, kCurY = 92; // Canvas

  thread_local bool t_one_view = false; // in the meter's Draw, for players of different teams
  thread_local uint32_t t_timer = 0;    // in the timer's Draw: the item
  thread_local bool t_timer_two_views = false;

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }

  float LoadF32(const uint8_t *base, uint32_t addr)
  {
    return std::bit_cast<float>(LoadU32(base, addr));
  }

  void StoreF32(uint8_t *base, uint32_t addr, float value)
  {
    const uint32_t v = std::byteswap(std::bit_cast<uint32_t>(value));
    std::memcpy(base + addr, &v, 4);
  }

  bool OtherTeamOnScreen(const uint8_t *base, uint32_t item)
  {
    if (!ao2::versus_guest::GuestCount())
    {
      return false;
    }
    static const int32_t owner = ao2::ue3::PropertyOffset(base, "AO2HudItem", "PlayerOwner");
    static const int32_t pri = ao2::ue3::PropertyOffset(base, "Controller", "PlayerReplicationInfo");
    static const int32_t team = ao2::ue3::PropertyOffset(base, "PlayerReplicationInfo", "Team");
    static const int32_t players = ao2::ue3::PropertyOffset(base, "Engine", "GamePlayers");
    static const int32_t actor = ao2::ue3::PropertyOffset(base, "Player", "Actor");
    static const int32_t origin = ao2::ue3::PropertyOffset(base, "LocalPlayer", "Origin");
    if (owner < 0 || pri < 0 || team < 0 || players < 0 || actor < 0 || origin < 0)
    {
      return false;
    }
    auto team_of = [&](uint32_t controller) {
      uint32_t info = controller ? LoadU32(base, controller + uint32_t(pri)) : 0;
      return info ? LoadU32(base, info + uint32_t(team)) : 0;
    };
    const uint32_t mine = LoadU32(base, item + uint32_t(owner));
    const uint32_t my_team = team_of(mine);
    const uint32_t engine = LoadU32(base, kEngine);
    if (!my_team || !engine)
    {
      return false;
    }
    const uint32_t data = LoadU32(base, engine + uint32_t(players));
    const uint32_t count = LoadU32(base, engine + uint32_t(players) + 4);
    if (!data || count > kMaxPlayers)
    {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
      const uint32_t player = LoadU32(base, data + i * 4);
      if (!player || std::bit_cast<float>(LoadU32(base, player + uint32_t(origin))) >= 1.0f)
      {
        continue;
      }
      const uint32_t controller = LoadU32(base, player + uint32_t(actor));
      const uint32_t other_team = controller != mine ? team_of(controller) : 0;
      if (other_team && other_team != my_team)
      {
        return true;
      }
    }
    return false;
  }
} // namespace

// Whether the frame has two views.
extern "C" REX_FUNC(__imp__sub_82611D58);
extern "C" REX_FUNC(sub_82611D58)
{
  if (t_one_view)
  {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82611D58(ctx, base);
  if (t_timer && ctx.r3.u32)
  {
    t_timer_two_views = true;
  }
}

// UAO2HudItem_Aggro::Draw (r3 = the item, r4 = the Canvas).
extern "C" REX_FUNC(__imp__sub_823AF0A8);
extern "C" REX_FUNC(sub_823AF0A8)
{
  t_one_view = OtherTeamOnScreen(base, ctx.r3.u32);
  __imp__sub_823AF0A8(ctx, base);
  t_one_view = false;
}

// UAO2HudItem_ObjectiveTimer::Draw (r3 = the item, r4 = the Canvas).
extern "C" REX_FUNC(__imp__sub_823B3748);
extern "C" REX_FUNC(sub_823B3748)
{
  t_timer = ctx.r3.u32;
  t_timer_two_views = false;
  __imp__sub_823B3748(ctx, base);
  t_timer = 0;
}

// UCanvas text (r3 = the Canvas). With two views the timer's CurY is OrgY + SafeZoneTop
// + half the text's height.
extern "C" REX_FUNC(__imp__sub_8270A9A8);
extern "C" REX_FUNC(sub_8270A9A8)
{
  if (t_timer && t_timer_two_views)
  {
    const uint32_t canvas = ctx.r3.u32;
    const uint32_t hud = LoadU32(base, t_timer + kHudOwner);
    const float y = LoadF32(base, canvas + kCurY);
    const float half = y - LoadF32(base, canvas + kOrgY) - LoadF32(base, hud + kSafeZoneTop);
    StoreF32(base, canvas + kCurY, y - half * 2.0f);
  }
  __imp__sub_8270A9A8(ctx, base);
}
