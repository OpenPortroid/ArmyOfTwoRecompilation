// versus_lobby.cpp - A versus match starts with fewer than 4 players
// (ao2_versus_min_players).
//
// The ready room's Start (AO2PlayerController.PStartGame, script) calls the
// lobby's native StartGame and, when AreAllPlayersReady is false, shows
// "Need More Players: the game cannot be started until 3 other players are
// present and ready". Both are AO2PlasmaController virtuals (vtable
// 0x820F0428: +300 StartGame sub_82A1F270, +376 AreAllPlayersReady
// sub_82A20868) over the Plasma lobby singleton (0x831F3024, 4584 bytes);
// StartGame starts the match only when sub_82A0D1B8 (r3 = lobby) says all
// players are ready: in the multiplayer mode the count starts at 1 (the
// host) and every player in the lobby's slots 2-5 (lobby +4016: the slots'
// list, a player pointer each; the host's own is among them, in but never
// ready) counts when it is in and ready (+16 and +24 set; +44 set counts as
// well), and the count must equal the lobby's places (+2788, 4 in a 2-on-2
// match). With ao2_versus_min_players the host may start once every player
// but one (the host) is ready and at least that many are in the lobby; the
// places left empty stay empty: the game has no bots in versus, ours play
// them (versus_bots.cpp).
//
// Co-op (not the multiplayer mode), ranked matches and a lobby with the
// count already taken from +2788 (+5) keep the game's rule.
#include "versus_bots.h"

#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_INT32(ao2_versus_min_players, 1, "Online",
                     "Versus (custom and private matches): the host can start once everybody in the lobby is ready "
                     "and at least this many players are there (1-4); 0 = only with every place taken, as on the "
                     "console. Bots play the empty places (ao2_versus_bots).");

namespace
{
  constexpr uint32_t kGameModes = 0x831F3010; // singleton, 84 bytes: +48 multiplayer, +52 ranked
  constexpr uint32_t kPlayers = 4016;         // the lobby's player slots (a list object)
  constexpr uint32_t kPlaces = 2788;          // the players a match takes
  constexpr uint32_t kStarting = 4044;        // byte: skips the count (as the game does)
  constexpr uint32_t kFixedCount = 5;         // byte: the count is the places
  constexpr uint32_t kLobby = 0x831F3024;     // the Plasma lobby singleton
  constexpr uint32_t kStarted = 4449;         // byte: StartGame went through
  constexpr uint32_t kWaiting = 4450;         // byte: the match waits for its players

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


  // A short match may start: the players in the lobby (out: present) when
  // all but the host are ready and the game's own rule would wait for more.
  bool ShortMatchReady(const uint8_t *base, uint32_t lobby, int &present)
  {
    int min = REXCVAR_GET(ao2_versus_min_players);
    uint32_t modes = LoadU32(base, kGameModes);
    if (min <= 0 || !lobby || !modes || !LoadU32(base, modes + 48) || LoadU32(base, modes + 52) ||
        base[lobby + kStarting] || base[lobby + kFixedCount])
    {
      return false;
    }
    uint32_t list = LoadU32(base, lobby + kPlayers);
    if (!list)
    {
      return false;
    }
    int ready = 1; // the host's 1, as the game counts
    present = 0;
    for (uint32_t slot = 2; slot <= 5; ++slot)
    {
      uint32_t player = LoadU32(base, list + 4 * slot);
      if (!player)
      {
        continue;
      }
      ++present;
      if (LoadU32(base, player + 44) || (LoadU32(base, player + 16) && LoadU32(base, player + 24)))
      {
        ++ready;
      }
    }
    int places = int(LoadU32(base, lobby + kPlaces));
    return ready >= present && present >= min && present < places;
  }
} // namespace

// The lobby's "all players ready" (r3 = lobby, returns r3 = bool).
extern "C" REX_FUNC(__imp__sub_82A0D1B8);
extern "C" REX_FUNC(sub_82A0D1B8)
{
  uint32_t lobby = ctx.r3.u32;
  __imp__sub_82A0D1B8(ctx, base);
  int present = 0;
  if (!ctx.r3.u32 && ShortMatchReady(base, lobby, present))
  {
    ctx.r3.u64 = 1;
  }
}

// AO2PlasmaController::StartGame (r3 = controller): a short match is
// started for the players there - the lobby's places become their number,
// so the match does not wait for the empty ones.
extern "C" REX_FUNC(__imp__sub_82A1F270);
extern "C" REX_FUNC(sub_82A1F270)
{
  uint32_t lobby = LoadU32(base, kLobby);
  int present = 0;
  if (lobby && !base[lobby + kStarted] && ShortMatchReady(base, lobby, present))
  {
    REXLOG_INFO("versus_lobby: starting with {} of {} players", present, LoadU32(base, lobby + kPlaces));
    StoreU32(base, lobby + kPlaces, uint32_t(present));
  }
  __imp__sub_82A1F270(ctx, base);
}

// The match start on every machine (sub_82A14E08, r3 = lobby) sets +4450:
// the lobby's update (sub_82A30178) then waits for the players who report
// in (+2800) to be the places (+2788) and gives up after 15 s
// (sub_82A2BE00 with r4 = 0: the player leaves, "Connection Lost"). Only
// the host's StartGame made the places the players there: on the other
// machines the places become them here.
extern "C" REX_FUNC(__imp__sub_82A14E08);
extern "C" REX_FUNC(sub_82A14E08)
{
  uint32_t lobby = ctx.r3.u32;
  __imp__sub_82A14E08(ctx, base);
  int min = REXCVAR_GET(ao2_versus_min_players);
  uint32_t modes = LoadU32(base, kGameModes);
  uint32_t list = lobby ? LoadU32(base, lobby + kPlayers) : 0;
  if (min > 0 && list && modes && LoadU32(base, modes + 48) && !LoadU32(base, modes + 52) && base[lobby + kWaiting])
  {
    int present = 0;
    for (uint32_t slot = 2; slot <= 5; ++slot)
    {
      present += LoadU32(base, list + 4 * slot) != 0;
    }
    int places = int(LoadU32(base, lobby + kPlaces));
    if (present >= min && present < places)
    {
      REXLOG_INFO("versus_lobby: the match starts with {} of {} players", present, places);
      StoreU32(base, lobby + kPlaces, uint32_t(present));
    }
  }
}

// The match's travel URL (sub_82A1A3E0, r3 = lobby: "<map>?Game=...
// ?MPPlayer1Type=%d...?MPPlayer4Type=%d") reads the type (+48: the
// character, 1-20, package AO2MPPlayerCharN) of the players with ids 0-3
// through sub_82A02D50 (r3 = slots, r4 = id; NULL when no slot has the id)
// without a check. While it runs, a missing player is a stand-in: a copy of
// the player there with the lowest id (+4 the missing id) with a character
// of its own - none of the players' and no other empty place's, picked by a
// hash of the id and the players' characters (each machine builds the URL:
// it must be the same everywhere).
namespace
{
  constexpr uint32_t kType = 48;
  constexpr uint32_t kCharacters = 20;
  constexpr uint32_t kStandInSize = 256;
  thread_local bool g_filling = false;
  thread_local uint32_t g_stand_ins = 0; // guest: a stand-in per id, kStandInSize each

  uint32_t Mix(uint32_t h)
  {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
  }

  // The character of each empty place (0 for the players' ids).
  void EmptyPlaceTypes(const uint8_t *base, uint32_t list, uint32_t types[4])
  {
    bool used[kCharacters + 1] = {};
    bool taken[4] = {};
    uint32_t seed = 0x2545F491u;
    for (uint32_t slot = 2; slot <= 5; ++slot)
    {
      uint32_t player = LoadU32(base, list + 4 * slot);
      if (!player)
      {
        continue;
      }
      uint32_t id = LoadU32(base, player + 4), type = LoadU32(base, player + kType);
      taken[id & 3] |= id < 4;
      used[type <= kCharacters ? type : 0] = true;
      seed = Mix(seed ^ (id * 0x9E3779B1u + type));
    }
    for (uint32_t id = 0; id < 4; ++id)
    {
      types[id] = 0;
      if (taken[id])
      {
        continue;
      }
      uint32_t free[kCharacters], n = 0;
      for (uint32_t c = 1; c <= kCharacters; ++c)
      {
        if (!used[c])
        {
          free[n++] = c;
        }
      }
      if (!n)
      {
        continue;
      }
      types[id] = free[Mix(seed + id * 0x85EBCA77u) % n];
      used[types[id]] = true;
    }
  }
} // namespace

extern "C" REX_FUNC(__imp__sub_82A02D50);
extern "C" REX_FUNC(sub_82A02D50)
{
  uint32_t list = ctx.r3.u32, id = ctx.r4.u32;
  __imp__sub_82A02D50(ctx, base);
  if (!g_filling || ctx.r3.u32 || !list || id >= 4)
  {
    return;
  }
  // The slots' order differs between the machines: the player with the
  // lowest id gives every machine the same copy.
  uint32_t first = 0;
  for (uint32_t slot = 2; slot <= 5; ++slot)
  {
    uint32_t player = LoadU32(base, list + 4 * slot);
    if (player && (!first || LoadU32(base, player + 4) < LoadU32(base, first + 4)))
    {
      first = player;
    }
  }
  if (!first)
  {
    return;
  }
  uint32_t types[4];
  EmptyPlaceTypes(base, list, types);
  if (!g_stand_ins || !types[id])
  {
    ctx.r3.u64 = first;
    return;
  }
  uint32_t stand_in = g_stand_ins + id * kStandInSize;
  std::memcpy(base + stand_in, base + first, kStandInSize);
  StoreU32(base, stand_in + 4, id);
  StoreU32(base, stand_in + kType, types[id]);
  REXLOG_INFO("versus_lobby: empty place {} plays character {}", id, types[id]);
  ctx.r3.u64 = stand_in;
}

// The stand-ins live in this frame, above the stack the URL is built on.
extern "C" REX_FUNC(__imp__sub_82A1A3E0);
extern "C" REX_FUNC(sub_82A1A3E0)
{
  g_filling = REXCVAR_GET(ao2_versus_min_players) > 0;
  uint32_t sp = ctx.r1.u32;
  uint32_t area = (sp - 4 * kStandInSize - 64) & ~15u;
  ctx.r1.u64 = (area - 64) & ~15u;
  StoreU32(base, ctx.r1.u32, sp); // back chain
  g_stand_ins = area;
  __imp__sub_82A1A3E0(ctx, base);
  ctx.r1.u64 = sp;
  g_stand_ins = 0;
  g_filling = false;
}


// The level's players (sub_82A153C0, r3 = lobby, on the map load): every
// player of the lobby's slots that is not this machine's goes to the
// controller's (+2784) RegisterRemotePlayers event (sub_82A35200: r4 = the
// player's id, r5 = its XUID, r6 = TArray<int> of its sim controllers, from
// lobby +4072 + 16 * id). The level has all 4 players (local players with
// controllers 0-3) and the viewport client draws the controllers nobody
// registered as this machine's own: with places left empty they all were,
// the local viewport got no size and the screen stayed black. The empty
// places (the ids 0-3 no slot has) are registered too, each with the
// controller of its id: their characters stand idle where they spawn.
namespace
{
  constexpr uint32_t kRegisterRemote = 0x82A35200;
  constexpr uint32_t kArrayAdd = 0x82741028;  // r4 = TArray *: a new element, r3 = its address
  constexpr uint32_t kArrayFree = 0x82480D20; // r3 = TArray *
  constexpr uint32_t kController = 2784;

  uint32_t CallGuest(PPCContext &ctx, uint8_t *base, uint32_t fn, uint32_t stack_top, uint64_t r3, uint64_t r4 = 0,
                     uint64_t r5 = 0, uint64_t r6 = 0)
  {
    PPCContext saved = ctx;
    ctx.r1.u64 = stack_top;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
    uint32_t result = ctx.r3.u32;
    ctx = saved;
    return result;
  }
} // namespace

extern "C" REX_FUNC(__imp__sub_82A153C0);
extern "C" REX_FUNC(sub_82A153C0)
{
  uint32_t lobby = ctx.r3.u32;
  __imp__sub_82A153C0(ctx, base);
  ao2::versus::SetEmptyPlaces(0);
  uint32_t modes = LoadU32(base, kGameModes);
  uint32_t controller = lobby ? LoadU32(base, lobby + kController) : 0;
  uint32_t list = lobby ? LoadU32(base, lobby + kPlayers) : 0;
  if (REXCVAR_GET(ao2_versus_min_players) <= 0 || !controller || !list || !modes || !LoadU32(base, modes + 48))
  {
    return;
  }
  bool taken[4] = {};
  for (uint32_t slot = 2; slot <= 5; ++slot)
  {
    if (uint32_t player = LoadU32(base, list + 4 * slot); player && LoadU32(base, player + 4) < 4)
    {
      taken[LoadU32(base, player + 4)] = true;
    }
  }
  uint32_t top = (ctx.r1.u32 - 1024) & ~15u;
  uint32_t ids = top + 256; // TArray<int> {data, count, max}
  uint32_t empty = 0;
  for (uint32_t id = 0; id < 4; ++id)
  {
    if (taken[id])
    {
      continue;
    }
    std::memset(base + ids, 0, 12);
    if (uint32_t element = CallGuest(ctx, base, kArrayAdd, top, 4, ids))
    {
      StoreU32(base, element, id);
    }
    CallGuest(ctx, base, kRegisterRemote, top, controller, id, 0, ids);
    CallGuest(ctx, base, kArrayFree, top, ids);
    REXLOG_INFO("versus_lobby: empty place {} registered as a remote player (controller {})", id, id);
    empty |= 1u << id;
  }
  ao2::versus::SetEmptyPlaces(empty);
}
