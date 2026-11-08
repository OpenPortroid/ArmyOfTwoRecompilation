// versus_rules.cpp - The host's rules of a versus match: the round's
// length, a cash goal, friendly fire and the respawn price (the
// ao2_versus_* cvars below, the F1 menu's Versus tab).
//
// They travel with the bot settings (versus_bots.cpp: the "vbots" lockstep
// command, the host's count) and are applied here on every machine at the
// same lockstep frame, from the simulation's state only:
// - Round length (ao2_versus_round_minutes; 0: the lobby's 10 / 15 / 20;
//   the F1 menu offers the lobby's or a number of minutes):
//   GameInfo.TimeLimit, and the time left (UTGame.RemainingTime) moved by
//   the difference; GameReplicationInfo's too. The HUD's timer reads
//   RemainingTime every tick (AO2MPPlayerController.PlayerTick).
// - Cash goal (ao2_versus_cash_goal; 0: none): a team whose cash (its
//   players' MPStats.MultiplayerCash, as AO2MPTeamInfo.CalculateTeamCash)
//   reaches it ends the match: the time left becomes 1 s and the game ends
//   it as at the time limit (AO2MPGame.CheckEndGame 'TimeLimit': the richer
//   team wins).
// - Friendly fire (ao2_versus_friendly_fire: 0 half, the game's own scale
//   of 0.5; 1 off; 2 full):
//   UTTeamGame.FriendlyFireScale, which AO2Character.GetDamageScaleForHacks
//   multiplies a teammate's damage by.
// - Respawn price (ao2_versus_respawn_cost; 0: the game's, 10000): the
//   AO2MPGame class default Score_Penalty_Respawn, which the respawn (A
//   while down: AO2Character.MultiplayerUnconsciousDoEvaluateTransitions)
//   takes from the player's cash and the HUD shows. A class default
//   outlives the match: back to the game's at every map load (Reset), so it
//   is the same on every machine until the host's rules come.
// - Dearer every respawn (ao2_versus_respawn_step): a player's k-th paid
//   respawn costs step * (k - 1) more, taken from its cash (not below 0) the
//   frame after the game took the price: a paid respawn is
//   AO2Character.MPNumTimesRevived up while the cash fell by about the
//   price. The HUD shows the price without it.
#include "versus_rules.h"

#include "coop.h"
#include "log_files.h"
#include "ue3_reflect.h"
#include "versus_bots.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(ao2_versus_round_minutes, 0, "Online",
                     "Versus (host): round length in minutes; 0 = the lobby's choice (10, 15 or 20)");
REXCVAR_DEFINE_INT32(ao2_versus_cash_goal, 0, "Online",
                     "Versus (host): a team with this much cash wins at once; 0 = no goal (the time limit only)");
REXCVAR_DEFINE_INT32(ao2_versus_friendly_fire, 0, "Online",
                     "Versus (host): friendly fire - 0 half damage (the game's), 1 off, 2 full damage");
REXCVAR_DEFINE_INT32(ao2_versus_respawn_cost, 0, "Online",
                     "Versus (host): what a respawn for money costs; 0 = the game's (10000)");
REXCVAR_DEFINE_INT32(ao2_versus_respawn_step, 0, "Online",
                     "Versus (host): each paid respawn of a player costs this much more than the one before");

namespace
{
  constexpr uint32_t kControllerPawn = 452; // Controller.Pawn
  constexpr uint32_t kControllerPRI = 456;  // Controller.PlayerReplicationInfo
  constexpr uint32_t kPRITeam = 512;        // PlayerReplicationInfo.Team
  constexpr uint32_t kObjectClass = 52;
  constexpr uint32_t kPlayersAgain = 300; // lockstep frames between looks for the players

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
  float LoadF32(const uint8_t *base, uint32_t addr)
  {
    return std::bit_cast<float>(LoadU32(base, addr));
  }
  void StoreF32(uint8_t *base, uint32_t addr, float value)
  {
    StoreU32(base, addr, std::bit_cast<uint32_t>(value));
  }

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("versus_rules.log");
    return f;
  }

  template <typename... Args>
  void Logf(const char *format, Args... args)
  {
    if (FILE *f = Log())
    {
      std::fprintf(f, format, args...);
      std::fflush(f);
    }
  }

  struct Props
  {
    int32_t game = -1;        // WorldInfo.Game
    int32_t gri = -1;         // GameInfo.GameReplicationInfo
    int32_t time_limit = -1;  // GameInfo.TimeLimit (minutes)
    int32_t remaining = -1;   // UTGame.RemainingTime (seconds)
    int32_t gri_limit = -1;   // GameReplicationInfo.TimeLimit
    int32_t gri_left = -1;    // GameReplicationInfo.RemainingTime
    int32_t gri_minute = -1;  // GameReplicationInfo.RemainingMinute
    int32_t ended = -1;       // GameInfo.bGameEnded (word)
    uint32_t ended_mask = 0;
    int32_t ff = -1;          // UTTeamGame.FriendlyFireScale
    int32_t penalty = -1;     // AO2MPGame.Score_Penalty_Respawn
    int32_t revived = -1;     // AO2Character.MPNumTimesRevived
    int32_t stats = -1;       // AO2MPPlayerController.MPStats
    int32_t cash = -1;        // MultiplayerStats.MultiplayerCash
    uint32_t world_class = 0, game_class = 0, controller_class = 0, character_class = 0;
  };

  const Props &Offsets(const uint8_t *base)
  {
    static Props p;
    static bool looked = false;
    if (looked)
    {
      return p;
    }
    looked = true;
    using ao2::ue3::PropertyOffset;
    p.game = PropertyOffset(base, "WorldInfo", "Game");
    p.gri = PropertyOffset(base, "GameInfo", "GameReplicationInfo");
    p.time_limit = PropertyOffset(base, "GameInfo", "TimeLimit");
    p.remaining = PropertyOffset(base, "UTGame", "RemainingTime");
    p.gri_limit = PropertyOffset(base, "GameReplicationInfo", "TimeLimit");
    p.gri_left = PropertyOffset(base, "GameReplicationInfo", "RemainingTime");
    p.gri_minute = PropertyOffset(base, "GameReplicationInfo", "RemainingMinute");
    if (!ao2::ue3::BoolProperty(base, "GameInfo", "bGameEnded", p.ended, p.ended_mask))
    {
      p.ended = -1;
    }
    p.ff = PropertyOffset(base, "UTTeamGame", "FriendlyFireScale");
    p.penalty = PropertyOffset(base, "AO2MPGame", "Score_Penalty_Respawn");
    p.revived = PropertyOffset(base, "AO2Character", "MPNumTimesRevived");
    p.stats = PropertyOffset(base, "AO2MPPlayerController", "MPStats");
    p.cash = PropertyOffset(base, "MultiplayerStats", "MultiplayerCash");
    p.world_class = ao2::ue3::FindClass(base, "WorldInfo");
    p.game_class = ao2::ue3::FindClass(base, "AO2MPGame");
    p.controller_class = ao2::ue3::FindClass(base, "AO2MPPlayerController");
    p.character_class = ao2::ue3::FindClass(base, "AO2Character");
    Logf("properties: WorldInfo.Game +%d, GameInfo.GameReplicationInfo +%d TimeLimit +%d bGameEnded +%d/%08X, "
         "UTGame.RemainingTime +%d, GameReplicationInfo TimeLimit +%d RemainingTime +%d RemainingMinute +%d, "
         "UTTeamGame.FriendlyFireScale +%d, AO2MPGame.Score_Penalty_Respawn +%d, AO2Character.MPNumTimesRevived "
         "+%d, AO2MPPlayerController.MPStats +%d, MultiplayerStats.MultiplayerCash +%d\n",
         p.game, p.gri, p.time_limit, p.ended, p.ended_mask, p.remaining, p.gri_limit, p.gri_left, p.gri_minute,
         p.ff, p.penalty, p.revived, p.stats, p.cash);
    return p;
  }

  bool IsDefault(const uint8_t *base, uint32_t object)
  {
    return ao2::ue3::ObjectName(base, object).rfind("Default__", 0) == 0;
  }

  // The level's WorldInfo (kept while it is one).
  uint32_t g_world = 0;
  uint32_t World(const uint8_t *base, const Props &p)
  {
    if (!p.world_class)
    {
      return 0;
    }
    if (g_world && LoadU32(base, g_world + kObjectClass) == p.world_class)
    {
      return g_world;
    }
    g_world = 0;
    uint32_t count = ao2::ue3::ObjectCount(base);
    for (uint32_t i = 0; i < count && !g_world; ++i)
    {
      uint32_t o = ao2::ue3::ObjectAt(base, i);
      if (o && LoadU32(base, o + kObjectClass) == p.world_class && !IsDefault(base, o))
      {
        g_world = o;
      }
    }
    return g_world;
  }

  // AO2MPGame's class default (Score_Penalty_Respawn) and the game's price
  // there, read the first time (before any rules set it).
  uint32_t g_defaults = 0;
  int32_t g_game_penalty = 0;
  bool g_have_game_penalty = false;
  uint32_t Defaults(const uint8_t *base, const Props &p)
  {
    if (g_defaults || !p.game_class || p.penalty < 0)
    {
      return g_defaults;
    }
    uint32_t count = ao2::ue3::ObjectCount(base);
    for (uint32_t i = 0; i < count && !g_defaults; ++i)
    {
      uint32_t o = ao2::ue3::ObjectAt(base, i);
      if (o && LoadU32(base, o + kObjectClass) == p.game_class && ao2::ue3::ObjectName(base, o) == "Default__AO2MPGame")
      {
        g_defaults = o;
      }
    }
    if (g_defaults && !g_have_game_penalty)
    {
      g_have_game_penalty = true;
      g_game_penalty = int32_t(LoadU32(base, g_defaults + p.penalty));
      Logf("AO2MPGame defaults %08X: Score_Penalty_Respawn %d\n", g_defaults, g_game_penalty);
    }
    return g_defaults;
  }

  // The match's players (their controllers) and what was seen of each.
  struct Player
  {
    uint32_t controller = 0;
    uint32_t pawn = 0;
    int32_t revived = 0;
    int32_t cash = 0;
    int paid = 0; // respawns paid for in this match
  };
  std::vector<Player> g_players;
  uint32_t g_players_frame = 0;
  bool g_players_looked = false;

  void FindPlayers(const uint8_t *base, const Props &p, uint32_t frame)
  {
    if (g_players_looked && frame - g_players_frame < kPlayersAgain)
    {
      return;
    }
    g_players_looked = true;
    g_players_frame = frame;
    std::vector<Player> found;
    uint32_t count = ao2::ue3::ObjectCount(base);
    for (uint32_t i = 0; i < count && p.controller_class; ++i)
    {
      uint32_t o = ao2::ue3::ObjectAt(base, i);
      if (!o || !ao2::ue3::IsA(base, o, p.controller_class) || IsDefault(base, o))
      {
        continue;
      }
      auto old = std::find_if(g_players.begin(), g_players.end(), [&](const Player &pl) { return pl.controller == o; });
      found.push_back(old != g_players.end() ? *old : Player{o});
    }
    if (found.size() != g_players.size())
    {
      Logf("frame %u: %zu players\n", frame, found.size());
    }
    g_players = std::move(found);
  }

  // What was applied in this match.
  int32_t g_lobby_minutes = 0; // the lobby's round length, before any rules changed it
  bool g_goal_reached = false;
  uint32_t g_game = 0;
  float g_game_ff = 0.0f; // the game's friendly fire scale
  int g_last_ff = 0;
  std::atomic<bool> g_ended{false}; // for the F1 menu (Ended)
} // namespace

namespace ao2::versus::rules
{
  void Reset(uint8_t *base)
  {
    const Props &p = Offsets(base);
    if (g_defaults && g_have_game_penalty && p.penalty >= 0 &&
        int32_t(LoadU32(base, g_defaults + p.penalty)) != g_game_penalty)
    {
      StoreU32(base, g_defaults + p.penalty, uint32_t(g_game_penalty));
      Logf("respawn price back to the game's (%d)\n", g_game_penalty);
    }
    g_players.clear();
    g_players_looked = false;
    g_lobby_minutes = 0;
    g_goal_reached = false;
    g_game = 0;
    g_last_ff = 0;
    g_world = 0;
    g_ended.store(false, std::memory_order_relaxed);
  }

  bool Ended()
  {
    return g_ended.load(std::memory_order_relaxed);
  }

  void Frame(PPCContext &ctx, uint8_t *base)
  {
    (void)ctx;
    const Props &p = Offsets(base);
    const ao2::versus::VersusSettings &s = ao2::versus::PlayedSettings();
    uint32_t frame = ao2::coop::SyncFrame(base);
    uint32_t world = World(base, p);
    uint32_t game = world && p.game >= 0 ? LoadU32(base, world + p.game) : 0;
    if (!game || !p.game_class || !ao2::ue3::IsA(base, game, p.game_class))
    {
      return;
    }
    if (game != g_game)
    {
      g_game = game;
      g_lobby_minutes = p.time_limit >= 0 ? int32_t(LoadU32(base, game + p.time_limit)) : 0;
      g_game_ff = p.ff >= 0 ? LoadF32(base, game + p.ff) : 0.0f;
      g_goal_reached = false;
      g_last_ff = 0;
      Logf("frame %u: game %08X, round %d min, friendly fire scale %.2f\n", frame, game, g_lobby_minutes, g_game_ff);
    }
    bool ended = p.ended >= 0 && (LoadU32(base, game + p.ended) & p.ended_mask);
    g_ended.store(ended, std::memory_order_relaxed);
    uint32_t gri = p.gri >= 0 ? LoadU32(base, game + p.gri) : 0;
    auto set_left = [&](int32_t left) {
      StoreU32(base, game + p.remaining, uint32_t(left));
      if (gri && p.gri_left >= 0)
      {
        StoreU32(base, gri + p.gri_left, uint32_t(left));
      }
      if (gri && p.gri_minute >= 0)
      {
        StoreU32(base, gri + p.gri_minute, uint32_t(left));
      }
    };

    // The round's length: the time left moved by the change.
    int32_t want = s.minutes > 0 ? s.minutes : g_lobby_minutes;
    if (!ended && p.time_limit >= 0 && p.remaining >= 0 && want > 0)
    {
      int32_t limit = int32_t(LoadU32(base, game + p.time_limit));
      if (limit > 0 && limit != want)
      {
        int32_t left = int32_t(LoadU32(base, game + p.remaining));
        int32_t now = std::max(1, left + (want - limit) * 60);
        StoreU32(base, game + p.time_limit, uint32_t(want));
        if (gri && p.gri_limit >= 0)
        {
          StoreU32(base, gri + p.gri_limit, uint32_t(want));
        }
        set_left(now);
        Logf("frame %u: round %d -> %d min, %d s left -> %d\n", frame, limit, want, left, now);
      }
    }

    // Friendly fire.
    if (p.ff >= 0 && s.ff != g_last_ff)
    {
      float scale = s.ff == 1 ? 0.0f : s.ff == 2 ? 1.0f : g_game_ff;
      StoreF32(base, game + p.ff, scale);
      Logf("frame %u: friendly fire %s (scale %.2f)\n", frame, s.ff == 1 ? "off" : s.ff == 2 ? "full" : "half (the game's)",
           scale);
      g_last_ff = s.ff;
    }

    // The respawn price.
    int32_t price = 0;
    if (uint32_t defaults = Defaults(base, p))
    {
      int32_t penalty = s.cost > 0 ? -s.cost : g_game_penalty;
      if (int32_t(LoadU32(base, defaults + p.penalty)) != penalty)
      {
        StoreU32(base, defaults + p.penalty, uint32_t(penalty));
        Logf("frame %u: respawn price %d\n", frame, std::abs(penalty));
      }
      price = std::abs(penalty);
    }

    // The players: dearer respawns, the teams' cash.
    if (p.stats < 0 || p.cash < 0)
    {
      return;
    }
    FindPlayers(base, p, frame);
    std::map<uint32_t, int64_t> team_cash;
    for (Player &pl : g_players)
    {
      uint32_t c = pl.controller;
      int32_t cash = int32_t(LoadU32(base, c + p.stats + p.cash));
      uint32_t pawn = LoadU32(base, c + kControllerPawn);
      bool character = pawn && p.character_class && p.revived >= 0 && ao2::ue3::IsA(base, pawn, p.character_class);
      int32_t revived = character ? int32_t(LoadU32(base, pawn + p.revived)) : pl.revived;
      if (character && pawn == pl.pawn && revived > pl.revived && price > 0 && pl.cash - cash >= price * 9 / 10)
      {
        ++pl.paid;
        int32_t extra = s.step > 0 ? s.step * (pl.paid - 1) : 0;
        int32_t take = std::clamp(extra, 0, std::max(cash, 0));
        if (take > 0)
        {
          cash -= take;
          StoreU32(base, c + p.stats + p.cash, uint32_t(cash));
        }
        Logf("frame %u: %08X respawn %d paid: %d + %d, cash now %d\n", frame, c, pl.paid, price, take, cash);
      }
      pl.pawn = pawn;
      pl.revived = revived;
      pl.cash = cash;
      uint32_t pri = LoadU32(base, c + kControllerPRI);
      if (uint32_t team = pri ? LoadU32(base, pri + kPRITeam) : 0)
      {
        team_cash[team] += cash;
      }
    }

    // The cash goal: the match ends as at its time limit.
    if (s.goal > 0 && !g_goal_reached && !ended && p.remaining >= 0)
    {
      for (const auto &[team, cash] : team_cash)
      {
        if (cash >= s.goal)
        {
          g_goal_reached = true;
          Logf("frame %u: team %08X has %lld of %d: the match ends\n", frame, team, (long long)cash, s.goal);
          if (int32_t(LoadU32(base, game + p.remaining)) > 1)
          {
            set_left(1);
          }
          break;
        }
      }
    }
  }
} // namespace ao2::versus::rules
