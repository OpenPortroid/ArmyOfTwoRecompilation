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
//
// Teams: the travel URL gives each id its team (?SimControllerId0=0
// ?SimControllerId1=0 ?SimControllerId2=1 ?SimControllerId3=1), and the ready
// room swaps a player's team only with one of the other team who wants it
// too (AO2PlayerController.MPTeamSwap: the Plasma player's WantsToSwapTeam):
// with bots in the other team's places nobody can come over. A player who
// asks goes over at once to a free place of the other team
// (ao2_versus_free_team_swap, below). versus_lobby.log dumps every player of
// the slots (their first 96 bytes) whenever one changes, in the ready room.
#include "log_files.h"
#include "versus_bots.h"
#include "versus_rules.h"

#include <algorithm>
#include <array>
#include <bit>
#include <utility>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_INT32(ao2_versus_min_players, 1, "Online",
                     "Versus (custom and private matches): the host can start once everybody in the lobby is ready "
                     "and at least this many players are there (1-4); 0 = only with every place taken, as on the "
                     "console. Bots play the empty places (ao2_versus_bots).");
REXCVAR_DEFINE_BOOL(ao2_versus_free_team_swap, true, "Online",
                    "Versus ready room: Swap team takes a free place of the other team at once; the game swaps "
                    "only with a player of the other team who asks too (used when that team is full)");

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

namespace
{
  // The lobby's players, dumped when any of them changes (see the top).
  constexpr uint32_t kDumpBytes = 96;
  std::array<std::array<uint8_t, kDumpBytes>, 4> g_dumped{};
  std::array<uint32_t, 4> g_dumped_at{};

  FILE *LobbyLog()
  {
    static FILE *log = ao2::OpenLogFile("versus_lobby.log");
    return log;
  }

  void DumpPlayers(const uint8_t *base, uint32_t lobby)
  {
    FILE *log = LobbyLog();
    uint32_t list = lobby ? LoadU32(base, lobby + kPlayers) : 0;
    if (!log || !list)
    {
      return;
    }
    for (uint32_t slot = 2; slot <= 5; ++slot)
    {
      uint32_t i = slot - 2;
      uint32_t player = LoadU32(base, list + 4 * slot);
      std::array<uint8_t, kDumpBytes> now{};
      if (player)
      {
        std::memcpy(now.data(), base + player, kDumpBytes);
      }
      if (player == g_dumped_at[i] && now == g_dumped[i])
      {
        continue;
      }
      g_dumped_at[i] = player;
      g_dumped[i] = now;
      std::fprintf(log, "slot %u: player %08X", slot, player);
      for (uint32_t b = 0; player && b < kDumpBytes; b += 4)
      {
        std::fprintf(log, "%s%08X", b % 32 ? " " : "\n  ", LoadU32(base, player + b));
      }
      std::fprintf(log, "\n");
      std::fflush(log);
    }
  }
} // namespace

// The lobby's "all players ready" (r3 = lobby, returns r3 = bool).
extern "C" REX_FUNC(__imp__sub_82A0D1B8);
extern "C" REX_FUNC(sub_82A0D1B8)
{
  uint32_t lobby = ctx.r3.u32;
  DumpPlayers(base, lobby);
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
  // It clears +4449 first: only here can the host's StartGame still be seen.
  ao2::versus::NoteMatchStart(lobby && base[lobby + kStarted]);
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
    // The players by id: the slots' order differs between the machines, and a seed mixed in slot order gave the
    // bots other characters on each PC (versus traces 2026-11-07: AO2MPPlayerChar12 / 3 on one, 16 / 13 on the
    // other) - other meshes and ragdolls, so a bot that went down moved differently and the game went out of sync.
    std::pair<uint32_t, uint32_t> present[4];
    uint32_t count = 0;
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
      present[count++] = {id, type};
    }
    std::sort(present, present + count);
    uint32_t seed = 0x2545F491u;
    for (uint32_t i = 0; i < count; ++i)
    {
      seed = Mix(seed ^ (present[i].first * 0x9E3779B1u + present[i].second));
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
  ao2::versus::rules::Reset(base); // the respawn price back to the game's, on every machine
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

// The host's player attributes (sub_82A29A68, r3 = lobby, r4 = the message
// of a player's; the host's own too): after the game's swaps of two players
// who both asked (+40 WantsToSwapTeam, ids 0-1 one team and 2-3 the other;
// none while anybody is ready, +24), a player still asking goes to a free
// id of the other team - also while others are ready (a split-screen guest
// is ready at once, and a move to a free place changes nobody else). Sent as the game sends a swap (sub_82A29988, to
// every machine and to this one's ready room): first with the wish cleared
// at the old id (its swap mark goes), then at the new one; a machine whose
// player changed id empties the old place (sub_82A24670) - this one's
// player was changed here, so the place is emptied here
// (AO2PlasmaController OnPlayGroupMemberLeave, sub_82A33138).
namespace
{
  constexpr uint32_t kSendPlayer = 0x82A29988; // r3 = lobby, r4 = player
  constexpr uint32_t kMemberLeave = 0x82A33138; // r3 = controller, r4 = id
  constexpr uint32_t kId = 4, kSwap = 40;

  void FreeTeamSwap(PPCContext &ctx, uint8_t *base, uint32_t lobby)
  {
    uint32_t modes = LoadU32(base, kGameModes);
    uint32_t list = lobby ? LoadU32(base, lobby + kPlayers) : 0;
    uint32_t controller = lobby ? LoadU32(base, lobby + kController) : 0;
    if (!REXCVAR_GET(ao2_versus_free_team_swap) || !list || !controller || !modes || !LoadU32(base, modes + 48) ||
        LoadU32(base, modes + 52) || LoadU32(base, lobby + kPlaces) != 4)
    {
      return;
    }
    uint32_t players[4] = {};
    bool taken[4] = {};
    for (uint32_t slot = 2; slot <= 5; ++slot)
    {
      uint32_t player = LoadU32(base, list + 4 * slot);
      players[slot - 2] = player;
      if (player && LoadU32(base, player + kId) < 4)
      {
        taken[LoadU32(base, player + kId)] = true;
      }
    }
    uint32_t top = (ctx.r1.u32 - 256) & ~15u;
    for (uint32_t player : players)
    {
      uint32_t id = player ? LoadU32(base, player + kId) : 4;
      // A split-screen pair (+20: a player with a guest, and the guest)
      // swaps one by one too: the game refuses them every swap.
      if (id >= 4 || !LoadU32(base, player + kSwap))
      {
        continue;
      }
      uint32_t first = id < 2 ? 2 : 0, to = taken[first] ? first + 1 : first;
      if (taken[to])
      {
        continue; // the other team is full: the game's swap
      }
      StoreU32(base, player + kSwap, 0);
      CallGuest(ctx, base, kSendPlayer, top, lobby, player);
      StoreU32(base, player + kId, to);
      CallGuest(ctx, base, kSendPlayer, top, lobby, player);
      CallGuest(ctx, base, kMemberLeave, top, controller, id);
      taken[id] = false;
      taken[to] = true;
      REXLOG_INFO("versus_lobby: player {} went over to the free place {}", id, to);
      if (FILE *log = LobbyLog())
      {
        std::fprintf(log, "player %u went over to the free place %u\n", id, to);
        std::fflush(log);
      }
    }
  }
} // namespace

extern "C" REX_FUNC(__imp__sub_82A29A68);
extern "C" REX_FUNC(sub_82A29A68)
{
  uint32_t lobby = ctx.r3.u32;
  __imp__sub_82A29A68(ctx, base);
  FreeTeamSwap(ctx, base, lobby);
}
