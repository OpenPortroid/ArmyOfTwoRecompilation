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
//
// The portraits (UAO2HudItem_MPHealth::Draw, sub_823A9C88): every AO2HUD's item draws
// the player's and the partner's energy with Energy_CurrentMaterialInstance (+148) and
// Energy_PartnerCurrentMaterialInstance (+152), the class defaults' material instances
// (energy_Rios_Mat, energy_Salem_Mat) that script Initialize takes as they are: one
// for every HUD. Draw sets their face and energy parameters, then puts the tile on the
// Canvas, which is drawn after every HUD of the frame: in a split-screen each part of
// the screen got the parameters of the HUD drawn last (the other team's player's
// portrait for player 2). Each item gets instances of its own, made as script's
// new(Outer) MaterialInstanceConstant would (StaticConstructObject, sub_82478F80), whose
// parent is the shared one (UMaterialInstanceConstant::SetParent, sub_825D0A18): they
// show what it shows until Draw sets their own parameters. The item holds them (an
// object property, so they live as long as it does); one that is not the item's own
// (its Outer) is replaced. The faces (texture parameter 'Character') are set once at
// the match's start by AO2MPGame.InitializeCharacterPortraits, before the first Draw
// and so on the shared instances (the last HUD's faces for every HUD): each item's own
// get the faces of its player and partner (char +3184, as Draw takes it) as that
// script does, MPFaceTextures[MPHealthPortraitIndex], whenever they change
// (UMaterialInstanceConstant::SetTextureParameterValue, sub_825D05D8).
// With two views Draw also takes one HUD (sub_823A9428 false) for the co-op partner's:
// its player's energy goes on the partner's instance and that tile is drawn as the
// player's own (in the campaign's split-screen player 2 is the partner). Players of
// different teams are told there is one view, as for the aggro meter: each draws its
// own portrait.
#include "ue3_reflect.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include <rex/logging.h>
#include <rex/ppc.h>

extern "C" REX_FUNC(sub_82478F80); // StaticConstructObject
extern "C" REX_FUNC(sub_825D0A18); // UMaterialInstanceConstant::SetParent
extern "C" REX_FUNC(sub_825D05D8); // UMaterialInstanceConstant::SetTextureParameterValue

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
  constexpr uint32_t kOuter = 40;            // UObject.Outer
  constexpr uint32_t kPortraits[2] = {148, 152}; // AO2HudItem_MPHealth: the player's, the partner's
  constexpr uint32_t kGError = 0x831B2084;   // GError (FOutputDevice *), StaticConstructObject's error device
  constexpr uint32_t kPlayerOwner = 80;      // AO2HudItem.PlayerOwner
  constexpr uint32_t kCharacter = 2024;      // AO2PlayerControllerNative.Character
  constexpr uint32_t kPartner = 3184;        // the character's partner, as MPHealth's Draw reads it
  constexpr uint32_t kFaceTextures = 156;    // AO2HudItem_MPHealth.MPFaceTextures[20]
  constexpr uint32_t kFaceCount = 20;
  constexpr uint32_t kPortraitIndex = 8160;  // AO2Character_Hero.MPHealthPortraitIndex

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

  void StoreU32(uint8_t *base, uint32_t addr, uint32_t value)
  {
    value = std::byteswap(value);
    std::memcpy(base + addr, &value, 4);
  }

  // A MaterialInstanceConstant of `item`'s own under `parent` (0: none made).
  uint32_t OwnInstance(PPCContext &ctx, uint8_t *base, uint32_t item, uint32_t parent)
  {
    static const uint32_t mic_class = ao2::ue3::FindClass(base, "MaterialInstanceConstant");
    if (!mic_class)
    {
      return 0;
    }
    PPCContext saved = ctx;
    ctx.r3.u64 = mic_class;
    ctx.r4.u64 = item;
    ctx.r5.u64 = 0; // NAME_None
    ctx.r6.u64 = 0; // no flags
    ctx.r7.u64 = 0; // the class default as the template
    ctx.r8.u64 = LoadU32(base, kGError);
    ctx.r9.u64 = 0;
    ctx.r10.u64 = 0;
    sub_82478F80(ctx, base);
    const uint32_t mic = ctx.r3.u32;
    if (mic)
    {
      ctx.r3.u64 = mic;
      ctx.r4.u64 = parent;
      sub_825D0A18(ctx, base);
    }
    ctx = saved;
    return mic;
  }

  std::unordered_map<uint32_t, uint32_t> g_shown; // game thread: an item's own instance -> the face set on it

  void Forget(uint32_t mic) { g_shown.erase(mic); }

  // The face `mic` (an item's own instance) shows: MPFaceTextures[`character`'s portrait index].
  void SetFace(PPCContext &ctx, uint8_t *base, uint32_t item, uint32_t mic, uint32_t character)
  {
    static const uint32_t hero = ao2::ue3::FindClass(base, "AO2Character_Hero");
    static const uint32_t name = ao2::ue3::FindName(base, "Character");
    if (!hero || !name || !character || !ao2::ue3::IsA(base, character, hero))
    {
      return;
    }
    const uint32_t index = LoadU32(base, character + kPortraitIndex);
    const uint32_t face = index < kFaceCount ? LoadU32(base, item + kFaceTextures + index * 4) : 0;
    auto it = g_shown.find(mic);
    if (!face || (it != g_shown.end() && it->second == face))
    {
      return;
    }
    g_shown[mic] = face;
    PPCContext saved = ctx;
    ctx.r3.u64 = mic;
    ctx.r4.u64 = uint64_t(name) << 32; // FName: index, number 0
    ctx.r5.u64 = face;
    sub_825D05D8(ctx, base);
    ctx = saved;
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

// UAO2HudItem_MPHealth::Draw (r3 = the item, r4 = the Canvas): the portraits'
// material instances are the item's own.
extern "C" REX_FUNC(__imp__sub_823A9C88);
extern "C" REX_FUNC(sub_823A9C88)
{
  const uint32_t item = ctx.r3.u32;
  for (uint32_t off : kPortraits)
  {
    const uint32_t shared = LoadU32(base, item + off);
    if (!shared || LoadU32(base, shared + kOuter) == item)
    {
      continue;
    }
    if (const uint32_t own = OwnInstance(ctx, base, item, shared))
    {
      Forget(own); // an instance freed before may have had this address
      StoreU32(base, item + off, own);
      REXLOG_INFO("split_hud: MPHealth {:08X} +{}: own material instance {:08X} (parent {:08X})", item, off, own,
                  shared);
    }
  }
  const uint32_t controller = LoadU32(base, item + kPlayerOwner);
  const uint32_t character = controller ? LoadU32(base, controller + kCharacter) : 0;
  const uint32_t partner = character ? LoadU32(base, character + kPartner) : 0;
  const uint32_t characters[2] = {character, partner};
  for (int i = 0; i < 2; ++i)
  {
    const uint32_t mic = LoadU32(base, item + kPortraits[i]);
    if (mic && LoadU32(base, mic + kOuter) == item)
    {
      SetFace(ctx, base, item, mic, characters[i]);
    }
  }
  t_one_view = OtherTeamOnScreen(base, item);
  __imp__sub_823A9C88(ctx, base);
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
