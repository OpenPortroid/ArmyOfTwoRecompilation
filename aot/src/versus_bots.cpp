// versus_bots.cpp - Bots in the empty places of a versus match
// (ao2_versus_bots).
//
// A versus match started with fewer than 4 players (versus_lobby.cpp) has
// a player for every place: the empty ones are AO2MPPlayerControllers with
// characters of their own, registered as remote players with the sim
// controller of their id, and the match counts them in its teams. Nobody
// sends input for them, so they stood where they spawned. A bot plays such
// a place with input of its own.
//
// Versus is a lockstep simulation like co-op (coop.h): every machine
// simulates every player from the frame records the lockstep replays. The
// replay of a frame (sub_82A26290) goes through the controller ids 0-3 and
// replays each record of that id (sub_82A0EEC8: key presses at +64, 12 bytes
// each - key table index, event, amount - count at +244; axes at +248, 8
// bytes each - key table index, int(value * 127) - count at +312; +48 the
// frame time, +588 / +592 the controller id). After the frame's first
// record, a record is made for every empty place - from that record, with
// the bot's keys and sticks - and replayed the same way. It has to be inside
// the replay: only there do the viewport's InputKey / InputAxis hand input to
// the controller (outside it they record the pads of this machine). They
// also look the controller id up among the lobby's players and drop input
// of an id nobody has: meanwhile the bot has a stand-in there
// (sub_82A02CD0). XboxTypeS_LeftY is negative forwards.
//
// The bot decides from the simulation's state only (positions, health, the
// level's collision and navigation network), counts time in lockstep frames
// and draws no random numbers but a hash of the frame and its id, so it
// decides alike on every machine. Its view turns directly (the look
// rotation, as UpdateLookingAction keeps it), the same on every machine, at
// a limited rate.
//
// What a bot does (each frame, while its character lives):
// - Target: the nearest enemy in range whose head or body a bullet trace
//   (AActor::Trace with actors, TRACEFLAG_Bullet, as autopilot.cpp) reaches.
//   Enemies: characters of the other team's players and AI soldiers whose
//   TeamAI is not on the bot's team.
// - With a target it aims (LT) at it, a little off at random, and shoots
//   (RT) once it has had it in view for a moment and the aim is close; far
//   away it walks closer, near it steps sideways. An empty magazine: X.
// - Without one it walks to the nearest enemy along the level's navigation
//   network (A* over the navigation points and the cover nodes, see below;
//   with no way there, to the point nearest to it it can reach), round what
//   a trace finds in the way, and looks where it goes. Stuck: Y (vault) and
//   A by turns, then a step aside, and obstacles passed on the other side.
// - Dead: A now and then.
//
// Every machine must have the same ao2_versus_bots (and bot settings): a
// bot on one machine only desyncs the match.
#include "versus_bots.h"

#include "coop.h"
#include "log_files.h"
#include "ue3_reflect.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(ao2_versus_bots, true, "Online",
                    "Versus: bots play the empty places of a match started with fewer than 4 players. Every player "
                    "of the match needs the same setting.");
REXCVAR_DEFINE_DOUBLE(ao2_versus_bots_range, 4000.0, "Online", "Versus bots: farthest enemy they shoot at (world units)");
REXCVAR_DEFINE_DOUBLE(ao2_versus_bots_turn, 9.0, "Online", "Versus bots: fastest turn of the view (degrees per frame)");
REXCVAR_DEFINE_DOUBLE(ao2_versus_bots_miss, 1.5, "Online",
                      "Versus bots: how far off the aim wanders (degrees); 0 = they aim true");
REXCVAR_DEFINE_INT32(ao2_versus_bots_reaction, 12, "Online",
                     "Versus bots: frames a target is in view before they shoot");

// The replay of one record (mouse_aim.cpp hooks it).
extern "C" REX_FUNC(sub_82A0EEC8);

namespace
{
  constexpr uint32_t kEngine = 0x831EFCBC;    // GEngine: +772 the game viewport client
  constexpr uint32_t kGameModes = 0x831F3010; // +48 multiplayer
  constexpr uint32_t kKeyTable = 0x83186320;  // 140 pointers to the key FNames
  constexpr uint32_t kKeyCount = 140;
  constexpr uint32_t kRecordSize = 596;
  constexpr uint32_t kMaxKeys = 15;
  constexpr uint32_t kPressed = 0, kReleased = 1; // EInputEvent

  // Actors (as autopilot.cpp).
  constexpr uint32_t kControllerPawn = 452;
  constexpr uint32_t kControllerPRI = 456; // Controller.PlayerReplicationInfo
  constexpr uint32_t kPRITeam = 512;       // PlayerReplicationInfo.Team
  constexpr uint32_t kActorLocation = 228;
  constexpr uint32_t kPawnController = 464;
  constexpr uint32_t kPawnHealth = 708;
  constexpr uint32_t kPawnWeapon = 924;
  constexpr uint32_t kWeaponAmmo = 692;      // UTWeapon.AmmoCount (the magazine)
  constexpr uint32_t kHeadOffset = 2780;     // AO2CharacterNative.CachedHeadBoneOffsetFromLocation
  constexpr uint32_t kTeamAI = 1088;         // AO2AIBaseNative.TeamAI
  constexpr uint32_t kTeamAITeam = 452;      // AO2TeamAI.Team
  constexpr uint32_t kLookHolder = 2024;     // controller: the character whose +3620 is the look rotation
  constexpr uint32_t kLookRotation = 3620;   // float pitch, yaw, roll (65536 = full turn)
  constexpr uint32_t kActorTrace = 0x828B8268;
  constexpr uint32_t kTraceFlagBullet = 1;

  constexpr float kUnitsPerRadian = 65536.0f / 6.2831853f;
  constexpr float kUnitsPerDegree = 65536.0f / 360.0f;
  constexpr float kMaxPitch = 70.0f * kUnitsPerDegree;

  // Timings in lockstep frames.
  constexpr uint32_t kScanFrames = 6;     // target choice
  constexpr uint32_t kPathFrames = 90;    // path again
  constexpr uint32_t kStuckFrames = 30;   // stuck check
  constexpr float kStuckMove = 30.0f;     // moved less than this in kStuckFrames
  constexpr float kNearFight = 1800.0f;   // nearer: steps aside, farther: walks closer
  constexpr float kWaypoint = 110.0f;     // a path node counts as reached this close
  constexpr float kWallProbe = 250.0f;    // how far ahead the way must be clear
  constexpr uint32_t kProbeFrames = 3;    // the way ahead is probed this often

  enum Button : uint32_t
  {
    kA = 1,
    kB = 2,
    kX = 4,
    kY = 8,
    kLT = 16,
    kRT = 32,
    kButtonCount = 6,
  };
  // Key names per button, then the axes.
  constexpr const char *kButtonKeys[kButtonCount] = {"XboxTypeS_A", "XboxTypeS_B", "XboxTypeS_X",
                                                     "XboxTypeS_Y", "XboxTypeS_LeftTrigger", "XboxTypeS_RightTrigger"};
  enum Axis
  {
    kLeftX,
    kLeftY,
    kRightX,
    kRightY,
    kLTAxis,
    kRTAxis,
    kAxisCount,
  };
  constexpr const char *kAxisKeys[kAxisCount] = {"XboxTypeS_LeftX", "XboxTypeS_LeftY", "XboxTypeS_RightX",
                                                 "XboxTypeS_RightY", "XboxTypeS_LeftTriggerAxis",
                                                 "XboxTypeS_RightTriggerAxis"};

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("versus_bots.log");
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
  void StoreU32(uint8_t *base, uint32_t addr, uint32_t value)
  {
    value = std::byteswap(value);
    std::memcpy(base + addr, &value, 4);
  }
  void StoreF32(uint8_t *base, uint32_t addr, float value)
  {
    StoreU32(base, addr, std::bit_cast<uint32_t>(value));
  }

  struct Vec
  {
    float x = 0, y = 0, z = 0;
  };
  Vec LoadVec(const uint8_t *base, uint32_t addr)
  {
    return {LoadF32(base, addr), LoadF32(base, addr + 4), LoadF32(base, addr + 8)};
  }
  float Dist2D(const Vec &a, const Vec &b)
  {
    return std::hypot(a.x - b.x, a.y - b.y);
  }

  // A number from the frame and the bot (the same on every machine), -1..1.
  float Noise(uint32_t a, uint32_t b)
  {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return float(h & 0xFFFF) / 32767.5f - 1.0f;
  }

  // ---- State shared with the hooks (game thread only).

  uint32_t g_places = 0;                         // empty places (bit = controller id)
  std::array<uint8_t, kRecordSize> g_template{}; // a record replayed this frame
  bool g_have_template = false;
  bool g_in_frame = false; // a frame's records are being replayed
  bool g_bots_ran = false; // the bots' records of that frame went
  bool g_injecting = false; // our own records are being replayed
  uint32_t g_stand_in = 0;  // the lobby player that stands in for the bot meanwhile (guest memory)
  int g_logged_records = 0;

  int g_button_keys[kButtonCount];
  int g_axis_keys[kAxisCount];
  bool g_keys_found = false;

  bool FindKeys(const uint8_t *base)
  {
    if (g_keys_found)
    {
      return true;
    }
    std::fill(std::begin(g_button_keys), std::end(g_button_keys), -1);
    std::fill(std::begin(g_axis_keys), std::end(g_axis_keys), -1);
    bool any = false;
    std::string all;
    for (uint32_t i = 0; i < kKeyCount; ++i)
    {
      uint32_t name = LoadU32(base, kKeyTable + i * 4);
      if (!name)
      {
        continue;
      }
      std::string n = ao2::ue3::Name(base, LoadU32(base, name));
      any |= !n.empty();
      all += std::to_string(i) + "=" + n + " ";
      for (uint32_t b = 0; b < kButtonCount; ++b)
      {
        g_button_keys[b] = n == kButtonKeys[b] ? int(i) : g_button_keys[b];
      }
      for (uint32_t a = 0; a < kAxisCount; ++a)
      {
        g_axis_keys[a] = n == kAxisKeys[a] ? int(i) : g_axis_keys[a];
      }
    }
    if (!any)
    {
      return false;
    }
    g_keys_found = true;
    Logf("key table: %s\n", all.c_str());
    for (uint32_t b = 0; b < kButtonCount; ++b)
    {
      Logf("button %s = key %d\n", kButtonKeys[b], g_button_keys[b]);
    }
    for (uint32_t a = 0; a < kAxisCount; ++a)
    {
      Logf("axis %s = key %d\n", kAxisKeys[a], g_axis_keys[a]);
    }
    return true;
  }

  // ---- Guest calls.

  // The player controller of a controller id (GEngine +772 = viewport
  // client, vtable +268 = its local player, LocalPlayer +64).
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

  // The first actor a bullet trace from `from` to `to` hits (0 = none);
  // `source` is not hit (as autopilot.cpp TraceHit).
  uint32_t TraceHit(PPCContext &ctx, uint8_t *base, uint32_t source, const Vec &from, const Vec &to)
  {
    PPCContext saved = ctx;
    uint32_t sp = (ctx.r1.u32 - 1024) & ~15u;
    uint32_t args = sp + 128; // HitLocation, HitNormal, TraceEnd, TraceStart, Extent: 12 bytes each
    std::memset(base + args, 0, 60);
    StoreF32(base, args + 24, to.x), StoreF32(base, args + 28, to.y), StoreF32(base, args + 32, to.z);
    StoreF32(base, args + 36, from.x), StoreF32(base, args + 40, from.y), StoreF32(base, args + 44, from.z);
    StoreU32(base, sp, ctx.r1.u32);
    StoreU32(base, sp + 84, kTraceFlagBullet);
    ctx.r1.u64 = sp;
    ctx.r3.u64 = source;
    ctx.r4.u64 = args;
    ctx.r5.u64 = args + 12;
    ctx.r6.u64 = args + 24;
    ctx.r7.u64 = args + 36;
    ctx.r8.u64 = 1; // bTraceActors
    ctx.r9.u64 = args + 48;
    ctx.r10.u64 = 0;
    rex::runtime::ResolveIndirectFunction(kActorTrace)(ctx, base);
    uint32_t hit = ctx.r3.u32;
    ctx = saved;
    return hit;
  }

  // ---- Classes and characters.

  enum ClassFlag : uint8_t
  {
    kIsCharacter = 1, // AO2CharacterNative
    kIsAI = 2,        // AO2AIBaseNative
  };

  // Per class, cached (classes stay while the game runs).
  uint8_t ClassFlags(const uint8_t *base, uint32_t cls)
  {
    static std::unordered_map<uint32_t, uint8_t> cache;
    static uint32_t character = 0, ai = 0;
    if (!cls)
    {
      return 0;
    }
    auto it = cache.find(cls);
    if (it != cache.end())
    {
      return it->second;
    }
    character = character ? character : ao2::ue3::FindClass(base, "AO2CharacterNative");
    ai = ai ? ai : ao2::ue3::FindClass(base, "AO2AIBaseNative");
    if (!character || !ai)
    {
      return 0;
    }
    uint8_t flags = 0;
    for (uint32_t c = cls, depth = 0; c && depth < 40; c = LoadU32(base, c + 60), ++depth)
    {
      flags |= c == character ? kIsCharacter : c == ai ? kIsAI : 0;
    }
    cache.emplace(cls, flags);
    return flags;
  }

  bool IsCharacter(const uint8_t *base, uint32_t object)
  {
    return object && (ClassFlags(base, LoadU32(base, object + 52)) & kIsCharacter);
  }

  bool IsAI(const uint8_t *base, uint32_t object)
  {
    return object && (ClassFlags(base, LoadU32(base, object + 52)) & kIsAI);
  }

  // A living character in play: its controller's pawn is it (a destroyed
  // one the garbage collector has not freed yet - which happens at a
  // different moment on every machine - is not).
  bool Alive(const uint8_t *base, uint32_t pawn)
  {
    if (!IsCharacter(base, pawn) || int32_t(LoadU32(base, pawn + kPawnHealth)) <= 0)
    {
      return false;
    }
    uint32_t controller = LoadU32(base, pawn + kPawnController);
    return controller && LoadU32(base, controller + kControllerPawn) == pawn;
  }

  Vec Head(const uint8_t *base, uint32_t pawn)
  {
    Vec v = LoadVec(base, pawn + kActorLocation);
    Vec h = LoadVec(base, pawn + kHeadOffset);
    if (std::fabs(h.x) + std::fabs(h.y) + std::fabs(h.z) < 1.0f || std::fabs(h.z) > 200.0f)
    {
      h = {0, 0, 60};
    }
    return {v.x + h.x, v.y + h.y, v.z + h.z};
  }

  // The team a controller plays for: its player's team, or an AI's TeamAI team.
  uint32_t TeamOf(const uint8_t *base, uint32_t controller)
  {
    if (!controller)
    {
      return 0;
    }
    if (IsAI(base, controller))
    {
      uint32_t team_ai = LoadU32(base, controller + kTeamAI);
      return team_ai ? LoadU32(base, team_ai + kTeamAITeam) : 0;
    }
    uint32_t pri = LoadU32(base, controller + kControllerPRI);
    return pri ? LoadU32(base, pri + kPRITeam) : 0;
  }

  // The level's characters, collected each frame the bots run from the
  // world's pawns (WorldInfo.PawnList, Pawn.NextPawn: a scan of every object
  // took a quarter of the game thread and slowed the match down), ordered
  // by position.
  struct Character
  {
    uint32_t pawn;
    uint32_t controller;
    uint32_t team;
    bool ai;
    Vec at;
  };
  std::vector<Character> g_characters;

  void CollectCharacters(const uint8_t *base, uint32_t world)
  {
    static int32_t list_off = -2, next_off = -2;
    if (list_off == -2)
    {
      list_off = ao2::ue3::PropertyOffset(base, "WorldInfo", "PawnList");
      next_off = ao2::ue3::PropertyOffset(base, "Pawn", "NextPawn");
      Logf("pawn list: WorldInfo +%d, Pawn +%d\n", list_off, next_off);
    }
    g_characters.clear();
    if (!world || list_off < 0 || next_off < 0)
    {
      return;
    }
    uint32_t o = LoadU32(base, world + list_off);
    for (uint32_t n = 0; o && n < 512; o = LoadU32(base, o + next_off), ++n)
    {
      if (!Alive(base, o))
      {
        continue;
      }
      uint32_t c = LoadU32(base, o + kPawnController);
      g_characters.push_back({o, c, TeamOf(base, c), IsAI(base, c), LoadVec(base, o + kActorLocation)});
    }
    std::sort(g_characters.begin(), g_characters.end(), [](const Character &a, const Character &b) {
      if (a.at.x != b.at.x)
      {
        return a.at.x < b.at.x;
      }
      if (a.at.y != b.at.y)
      {
        return a.at.y < b.at.y;
      }
      return a.at.z < b.at.z;
    });
  }

  bool Enemy(const Character &bot, const Character &other)
  {
    if (other.pawn == bot.pawn || !other.team)
    {
      return false; // civilians and anybody with no team are left alone
    }
    return other.team != bot.team;
  }

  // ---- The navigation network.
  //
  // The versus maps have few NavigationPoints (the player starts and a few
  // waypoints: 28 in Iraq); the game's AI moves between its cover nodes
  // (AO2CoverNode, ~1200 in Iraq, all over the map). The network: the
  // navigation points with their ReachSpecs, the cover nodes, and a link
  // between two points near each other wherever a trace at body height
  // between them is clear - checked kLinksPerFrame at a time, the same on
  // every machine (the points ordered by position, not by where they are in
  // memory).

  struct Node
  {
    Vec at;                                  // at body height
    std::vector<std::pair<int, float>> next; // node, cost
  };
  std::vector<Node> g_nodes;
  std::vector<std::pair<int, int>> g_links; // pairs to check
  size_t g_links_checked = 0;
  bool g_nav_ready = false;
  uint32_t g_nav_world = 0; // the WorldInfo the network is from
  uint32_t g_nav_first = 0;

  constexpr float kCoverBody = 32.0f;    // a cover node is this far below a character's middle
  constexpr float kLinkRange = 1100.0f;  // farthest link between points
  constexpr float kLinkClimb = 200.0f;   // most height between linked points
  constexpr float kVaultHeight = 70.0f;  // a low wall: the trace this much higher is clear (Y vaults it)
  constexpr float kVaultCost = 1.5f;     // a link over one costs this much more
  constexpr size_t kLinksPerPoint = 14;  // nearest points tried per point
  constexpr size_t kLinksPerFrame = 600; // traces per frame while the network is built

  uint32_t FindWorldInfo(const uint8_t *base)
  {
    static uint32_t cls = 0;
    cls = cls ? cls : ao2::ue3::FindClass(base, "WorldInfo");
    if (!cls)
    {
      return 0;
    }
    if (g_nav_world && LoadU32(base, g_nav_world + 52) == cls &&
        ao2::ue3::ObjectName(base, g_nav_world).rfind("Default__", 0) != 0)
    {
      return g_nav_world;
    }
    uint32_t count = ao2::ue3::ObjectCount(base);
    for (uint32_t i = 0; i < count; ++i)
    {
      uint32_t o = ao2::ue3::ObjectAt(base, i);
      if (o && LoadU32(base, o + 52) == cls && ao2::ue3::ObjectName(base, o).rfind("Default__", 0) != 0)
      {
        return o;
      }
    }
    return 0;
  }

  bool Before(const Vec &a, const Vec &b)
  {
    return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z;
  }

  // A trace between two points is clear: nothing but characters in the way.
  bool ClearLine(PPCContext &ctx, uint8_t *base, uint32_t source, const Vec &from, const Vec &to)
  {
    uint32_t hit = TraceHit(ctx, base, source, from, to);
    return !hit || IsCharacter(base, hit);
  }

  void AddLink(int a, int b, float cost)
  {
    for (auto &[to, c] : g_nodes[a].next)
    {
      if (to == b)
      {
        c = std::min(c, cost);
        return;
      }
    }
    g_nodes[a].next.push_back({b, cost});
  }

  float Dist3D(const Vec &a, const Vec &b)
  {
    return std::hypot(std::hypot(a.x - b.x, a.y - b.y), a.z - b.z);
  }

  void StartNavigation(const uint8_t *base, uint32_t first)
  {
    static int32_t next_off = -2, paths_off, end_off, flags_off;
    if (next_off == -2)
    {
      next_off = ao2::ue3::PropertyOffset(base, "NavigationPoint", "nextNavigationPoint");
      paths_off = ao2::ue3::PropertyOffset(base, "NavigationPoint", "PathList");
      end_off = ao2::ue3::PropertyOffset(base, "ReachSpec", "End");
      flags_off = ao2::ue3::PropertyOffset(base, "ReachSpec", "reachFlags");
    }
    g_nodes.clear();
    g_links.clear();
    g_links_checked = 0;
    g_nav_ready = false;

    // The points, by position.
    struct Point
    {
      Vec at;
      uint32_t actor;
      bool nav;
    };
    std::vector<Point> points;
    std::unordered_map<uint32_t, int> seen;
    for (uint32_t p = first; p && next_off >= 0 && points.size() < 30000 && !seen.count(p);
         p = LoadU32(base, p + next_off))
    {
      seen.emplace(p, 0);
      points.push_back({LoadVec(base, p + kActorLocation), p, true});
    }
    size_t navs = points.size();
    uint32_t cover = ao2::ue3::FindClass(base, "AO2CoverNode");
    uint32_t count = ao2::ue3::ObjectCount(base);
    for (uint32_t i = 0; cover && i < count && points.size() < 30000; ++i)
    {
      uint32_t o = ao2::ue3::ObjectAt(base, i);
      if (!o || !ao2::ue3::IsA(base, o, cover))
      {
        continue;
      }
      Vec at = LoadVec(base, o + kActorLocation);
      if (at.x == 0 && at.y == 0 && at.z == 0)
      {
        continue; // Default__
      }
      points.push_back({{at.x, at.y, at.z + kCoverBody}, o, false});
    }
    std::sort(points.begin(), points.end(), [](const Point &a, const Point &b) { return Before(a.at, b.at); });
    std::unordered_map<uint32_t, int> index;
    g_nodes.resize(points.size());
    for (size_t i = 0; i < points.size(); ++i)
    {
      g_nodes[i].at = points[i].at;
      index[points[i].actor] = int(i);
    }

    // The navigation points' own paths.
    constexpr uint32_t kNotWalked = 2 | 4 | 64 | 128; // R_FLY, R_SWIM, R_LADDER, R_PROSCRIBED
    size_t specs = 0;
    for (size_t i = 0; i < points.size() && paths_off >= 0 && end_off >= 0; ++i)
    {
      if (!points[i].nav)
      {
        continue;
      }
      uint32_t data = LoadU32(base, points[i].actor + paths_off);
      uint32_t n = LoadU32(base, points[i].actor + paths_off + 4);
      for (uint32_t k = 0; data && k < n && k < 64; ++k)
      {
        uint32_t spec = LoadU32(base, data + k * 4);
        auto it = spec ? index.find(LoadU32(base, spec + end_off)) : index.end();
        if (it == index.end() || (flags_off >= 0 && (LoadU32(base, spec + flags_off) & kNotWalked)))
        {
          continue;
        }
        AddLink(int(i), it->second, Dist3D(g_nodes[i].at, g_nodes[it->second].at));
        ++specs;
      }
    }

    // Links to check: each point's nearest ones in reach (the points are
    // sorted by x, so only a window of them is looked at).
    for (size_t i = 0; i < g_nodes.size(); ++i)
    {
      const Vec &a = g_nodes[i].at;
      std::vector<std::pair<float, int>> near;
      size_t lo = i;
      while (lo > 0 && a.x - g_nodes[lo - 1].at.x <= kLinkRange)
      {
        --lo;
      }
      for (size_t j = lo; j < g_nodes.size() && g_nodes[j].at.x - a.x <= kLinkRange; ++j)
      {
        const Vec &b = g_nodes[j].at;
        float d = Dist2D(a, b);
        if (j != i && d <= kLinkRange && std::fabs(a.z - b.z) <= kLinkClimb)
        {
          near.push_back({d, int(j)});
        }
      }
      std::sort(near.begin(), near.end());
      for (size_t k = 0; k < near.size() && k < kLinksPerPoint; ++k)
      {
        g_links.push_back({std::min(int(i), near[k].second), std::max(int(i), near[k].second)});
      }
    }
    std::sort(g_links.begin(), g_links.end());
    g_links.erase(std::unique(g_links.begin(), g_links.end()), g_links.end());
    Logf("navigation: %zu points (%zu navigation points, %zu of their paths), %zu links to check\n", g_nodes.size(),
         navs, specs, g_links.size());
  }

  void BuildNavigation(PPCContext &ctx, uint8_t *base, uint32_t source)
  {
    uint32_t world = FindWorldInfo(base);
    static int32_t list_off = -2;
    if (list_off == -2)
    {
      list_off = ao2::ue3::PropertyOffset(base, "WorldInfo", "NavigationPointList");
    }
    uint32_t first = world && list_off >= 0 ? LoadU32(base, world + list_off) : 0;
    if (world != g_nav_world || first != g_nav_first)
    {
      g_nav_world = world;
      g_nav_first = first;
      StartNavigation(base, first);
    }
    if (g_nav_ready || !source)
    {
      return;
    }
    size_t end = std::min(g_links.size(), g_links_checked + kLinksPerFrame);
    for (; g_links_checked < end; ++g_links_checked)
    {
      auto [a, b] = g_links[g_links_checked];
      const Vec &pa = g_nodes[a].at, &pb = g_nodes[b].at;
      float cost = Dist3D(pa, pb);
      bool clear = ClearLine(ctx, base, source, pa, pb);
      if (!clear && ClearLine(ctx, base, source, {pa.x, pa.y, pa.z + kVaultHeight}, {pb.x, pb.y, pb.z + kVaultHeight}))
      {
        clear = true;
        cost *= kVaultCost;
      }
      if (clear)
      {
        AddLink(a, b, cost);
        AddLink(b, a, cost);
      }
    }
    if (g_links_checked >= g_links.size())
    {
      g_nav_ready = true;
      size_t links = 0;
      for (const Node &n : g_nodes)
      {
        links += n.next.size();
      }
      // Parts of the network not linked to each other.
      std::vector<int> part(g_nodes.size(), -1);
      std::vector<size_t> sizes;
      for (size_t i = 0; i < g_nodes.size(); ++i)
      {
        if (part[i] >= 0)
        {
          continue;
        }
        std::vector<int> stack{int(i)};
        part[i] = int(sizes.size());
        size_t n = 0;
        while (!stack.empty())
        {
          int k = stack.back();
          stack.pop_back();
          ++n;
          for (auto [to, c] : g_nodes[k].next)
          {
            if (part[to] < 0)
            {
              part[to] = part[i];
              stack.push_back(to);
            }
          }
        }
        sizes.push_back(n);
      }
      std::sort(sizes.rbegin(), sizes.rend());
      Logf("navigation ready: %zu points, %zu links, %zu parts (largest %zu %zu %zu)\n", g_nodes.size(), links,
           sizes.size(), sizes.size() > 0 ? sizes[0] : 0, sizes.size() > 1 ? sizes[1] : 0,
           sizes.size() > 2 ? sizes[2] : 0);
    }
  }

  // Of the few points nearest to `at`, the nearest a trace from it reaches;
  // else the nearest.
  int NearestNode(PPCContext &ctx, uint8_t *base, uint32_t source, const Vec &at)
  {
    std::vector<std::pair<float, int>> near;
    near.reserve(g_nodes.size());
    for (size_t i = 0; i < g_nodes.size(); ++i)
    {
      const Vec &n = g_nodes[i].at;
      float dz = (n.z - at.z) * 3.0f;
      near.push_back({(n.x - at.x) * (n.x - at.x) + (n.y - at.y) * (n.y - at.y) + dz * dz, int(i)});
    }
    size_t k = std::min<size_t>(near.size(), 6);
    std::partial_sort(near.begin(), near.begin() + k, near.end());
    for (size_t i = 0; i < k; ++i)
    {
      if (ClearLine(ctx, base, source, at, g_nodes[near[i].second].at))
      {
        return near[i].second;
      }
    }
    return k ? near[0].second : -1;
  }

  // A* from the node nearest to `from` to the one nearest to `to`.
  std::vector<int> FindPath(PPCContext &ctx, uint8_t *base, uint32_t source, const Vec &from, const Vec &to)
  {
    if (!g_nav_ready)
    {
      return {};
    }
    int start = NearestNode(ctx, base, source, from), goal = NearestNode(ctx, base, source, to);
    if (start < 0 || goal < 0)
    {
      return {};
    }
    std::vector<float> cost(g_nodes.size(), INFINITY);
    std::vector<int> came(g_nodes.size(), -1);
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    auto h = [&](int i) {
      const Vec &a = g_nodes[i].at, &b = g_nodes[goal].at;
      return std::hypot(std::hypot(a.x - b.x, a.y - b.y), a.z - b.z);
    };
    cost[start] = 0;
    open.push({h(start), start});
    size_t visited = 0;
    while (!open.empty() && visited < 20000)
    {
      auto [f, i] = open.top();
      open.pop();
      ++visited;
      if (i == goal)
      {
        break;
      }
      if (f > cost[i] + h(i) + 0.5f)
      {
        continue; // an older entry
      }
      for (auto [j, c] : g_nodes[i].next)
      {
        float n = cost[i] + c;
        if (n < cost[j])
        {
          cost[j] = n, came[j] = i;
          open.push({n + h(j), j});
        }
      }
    }
    if (start != goal && came[goal] < 0)
    {
      // No way there: to the point it can reach nearest to it.
      int nearest = -1;
      float nearest_d = INFINITY;
      for (size_t i = 0; i < g_nodes.size(); ++i)
      {
        float d = Dist3D(g_nodes[i].at, g_nodes[goal].at);
        if ((int(i) == start || came[i] >= 0) && d < nearest_d)
        {
          nearest = int(i), nearest_d = d;
        }
      }
      static int logged = 0;
      if (logged < 40)
      {
        ++logged;
        const Vec &a = g_nodes[start].at, &b = g_nodes[goal].at;
        Logf("no way from point %d (%.0f %.0f %.0f, %zu links) to %d (%.0f %.0f %.0f, %zu links), %zu visited\n",
             start, a.x, a.y, a.z, g_nodes[start].next.size(), goal, b.x, b.y, b.z, g_nodes[goal].next.size(), visited);
      }
      if (nearest < 0 || nearest == start)
      {
        return {};
      }
      goal = nearest;
    }
    std::vector<int> path;
    for (int i = goal; i >= 0; i = i == start ? -1 : came[i])
    {
      path.push_back(i);
    }
    std::reverse(path.begin(), path.end());
    return path;
  }

  // ---- A bot.

  struct Bot
  {
    uint32_t held = 0; // buttons held in its last record
    uint32_t controller = 0;
    uint32_t target = 0;
    uint32_t target_since = 0; // frame
    bool target_body = false;
    uint32_t next_scan = 0;
    std::vector<int> path;
    size_t path_at = 0;
    Vec path_goal;
    uint32_t next_path = 0;
    Vec stuck_at;
    uint32_t stuck_check = 0;
    int stuck = 0;
    uint32_t aside_until = 0;
    float aside = 0;
    float walk_dir = 0;      // radians, the way chosen by the last probe
    int side = 1;            // obstacles are passed on this side first
    uint32_t next_probe = 0;
    uint32_t reload_until = 0, next_reload = 0;
    uint32_t next_respawn = 0;
    uint32_t next_log = 0;
  };
  std::array<Bot, 4> g_bots;

  struct Input
  {
    float lx = 0, ly = 0; // left stick: x right, y forward
    uint32_t buttons = 0;
  };

  // Turns the bot's view towards yaw / pitch (rotation units) by at most
  // the turn rate; true when it is there (within `tolerance`).
  bool TurnTowards(uint8_t *base, uint32_t holder, float want_yaw, float want_pitch, float tolerance)
  {
    uint32_t rotation = holder + kLookRotation;
    float pitch = LoadF32(base, rotation), yaw = LoadF32(base, rotation + 4);
    float dyaw = std::remainder(want_yaw - yaw, 65536.0f);
    float dpitch = std::remainder(std::clamp(want_pitch, -kMaxPitch, kMaxPitch) - std::remainder(pitch, 65536.0f),
                                  65536.0f);
    float step = float(REXCVAR_GET(ao2_versus_bots_turn)) * kUnitsPerDegree;
    float len = std::hypot(dyaw, dpitch);
    float k = len > step ? step / len : 1.0f;
    StoreF32(base, rotation + 4, yaw + dyaw * k);
    StoreF32(base, rotation, pitch + dpitch * k);
    return len * (1.0f - k) <= tolerance;
  }

  // The left stick towards a point, relative to the view's yaw.
  void Steer(const Vec &from, const Vec &to, float view_yaw, float speed, Input &in)
  {
    float want = std::atan2(to.y - from.y, to.x - from.x);
    float rel = want - view_yaw / kUnitsPerRadian; // + = to the right (yaw grows towards +Y)
    in.ly = std::cos(rel) * speed;
    in.lx = std::sin(rel) * speed;
  }

  // The way towards `to`: straight on when a trace kWallProbe ahead is
  // clear, else the clear direction nearest to it, turning to the bot's
  // side first. Probed every kProbeFrames frames; returns a point that way.
  Vec WalkTarget(PPCContext &ctx, uint8_t *base, uint32_t pawn, const Vec &at, const Vec &to, Bot &bot,
                 uint32_t frame)
  {
    if (frame >= bot.next_probe)
    {
      bot.next_probe = frame + kProbeFrames;
      float want = std::atan2(to.y - at.y, to.x - at.x);
      float probe = std::clamp(Dist2D(at, to) - 60.0f, 60.0f, kWallProbe);
      bot.walk_dir = want;
      bool found = false;
      for (int step = 0; step <= 6 && !found; ++step)
      {
        for (int side : {bot.side, -bot.side})
        {
          float a = want + float(side * step) * (30.0f / 57.29578f);
          Vec end{at.x + std::cos(a) * probe, at.y + std::sin(a) * probe, at.z};
          if (!TraceHit(ctx, base, pawn, at, end))
          {
            bot.walk_dir = a;
            found = true;
            break;
          }
          if (step == 0 || step == 6)
          {
            break; // straight on and straight back: one probe
          }
        }
      }
    }
    return {at.x + std::cos(bot.walk_dir) * 500.0f, at.y + std::sin(bot.walk_dir) * 500.0f, to.z};
  }

  bool InSight(PPCContext &ctx, uint8_t *base, uint32_t pawn, uint32_t target, bool &body)
  {
    Vec eye = Head(base, pawn);
    for (bool b : {false, true})
    {
      Vec to = b ? LoadVec(base, target + kActorLocation) : Head(base, target);
      uint32_t hit = TraceHit(ctx, base, pawn, eye, to);
      if (!hit || hit == target)
      {
        body = b;
        return true;
      }
    }
    return false;
  }

  Input Think(PPCContext &ctx, uint8_t *base, uint32_t id, Bot &bot, uint32_t frame)
  {
    Input in;
    uint32_t controller = bot.controller;
    uint32_t pawn = LoadU32(base, controller + kControllerPawn);
    uint32_t holder = LoadU32(base, controller + kLookHolder);
    if (!Alive(base, pawn) || holder != pawn)
    {
      bot.target = 0;
      bot.path.clear();
      if (frame >= bot.next_respawn)
      {
        bot.next_respawn = frame + 45;
        in.buttons |= kA;
      }
      return in;
    }
    Character me{pawn, controller, TeamOf(base, controller), false, LoadVec(base, pawn + kActorLocation)};
    float view_yaw = LoadF32(base, holder + kLookRotation + 4);
    Vec eye = Head(base, pawn);

    // Target.
    if (frame >= bot.next_scan)
    {
      bot.next_scan = frame + kScanFrames;
      float range = float(REXCVAR_GET(ao2_versus_bots_range));
      uint32_t found = 0;
      bool body = false;
      bool keep = false;
      std::vector<std::pair<float, const Character *>> near;
      for (const Character &c : g_characters)
      {
        float d = Dist2D(me.at, c.at);
        if (Enemy(me, c) && d < range)
        {
          near.push_back({d, &c});
          keep |= c.pawn == bot.target;
        }
      }
      std::stable_sort(near.begin(), near.end(),
                       [](const auto &a, const auto &b) { return a.first < b.first; });
      if (keep && InSight(ctx, base, pawn, bot.target, body))
      {
        found = bot.target;
      }
      for (size_t i = 0; !found && i < near.size() && i < 4; ++i)
      {
        if (InSight(ctx, base, pawn, near[i].second->pawn, body))
        {
          found = near[i].second->pawn;
        }
      }
      if (found != bot.target)
      {
        bot.target_since = frame;
        if (found)
        {
          Logf("frame %u: bot %u targets %08X (%s) at %.0f\n", frame, id, found,
               ao2::ue3::ObjectName(base, found).c_str(), Dist2D(me.at, LoadVec(base, found + kActorLocation)));
        }
      }
      bot.target = found;
      bot.target_body = body;
    }
    if (bot.target && !Alive(base, bot.target))
    {
      bot.target = 0;
    }

    // Where to go: towards the target, else the nearest enemy anywhere.
    const Character *goal = nullptr;
    float goal_d = INFINITY;
    for (const Character &c : g_characters)
    {
      float d = Dist2D(me.at, c.at);
      if (bot.target ? c.pawn == bot.target : Enemy(me, c) && d < goal_d)
      {
        goal = &c, goal_d = d;
      }
    }

    bool walking = false;
    float speed = 1.0f;
    if (bot.target)
    {
      Vec at = bot.target_body ? LoadVec(base, bot.target + kActorLocation) : Head(base, bot.target);
      float miss = float(REXCVAR_GET(ao2_versus_bots_miss)) * kUnitsPerDegree;
      float dx = at.x - eye.x, dy = at.y - eye.y, dz = at.z - eye.z;
      float want_yaw = std::atan2(dy, dx) * kUnitsPerRadian + miss * Noise(frame / 20, id * 2);
      float want_pitch = std::atan2(dz, std::hypot(dx, dy)) * kUnitsPerRadian + miss * Noise(frame / 20, id * 2 + 1);
      float tolerance = std::max(2.5f * kUnitsPerDegree, std::atan2(30.0f, std::hypot(dx, dy)) * kUnitsPerRadian);
      bool on = TurnTowards(base, holder, want_yaw, want_pitch, tolerance);
      in.buttons |= kLT;
      if (on && frame - bot.target_since >= uint32_t(std::max(0, REXCVAR_GET(ao2_versus_bots_reaction))) &&
          frame % 9 < 6)
      {
        in.buttons |= kRT; // pulled 6 frames of 9: single shot weapons fire again too
      }
      float d = Dist2D(me.at, LoadVec(base, bot.target + kActorLocation));
      if (d > kNearFight)
      {
        walking = true;
        speed = 0.6f;
      }
      else
      {
        // Sideways, now this way, now that.
        float side = Noise(frame / 50, id + 17) < 0 ? -0.7f : 0.7f;
        in.lx = side;
      }
    }
    else if (goal)
    {
      walking = true;
    }

    if (walking && goal)
    {
      if (bot.path.empty() || frame >= bot.next_path || Dist2D(bot.path_goal, goal->at) > 600.0f)
      {
        bot.next_path = frame + kPathFrames;
        bot.path = FindPath(ctx, base, pawn, me.at, goal->at);
        bot.path_at = 0;
        bot.path_goal = goal->at;
      }
      // Past a node already when the next is nearer than the node is to it.
      while (bot.path_at + 1 < bot.path.size())
      {
        const Vec &a = g_nodes[bot.path[bot.path_at]].at, &b = g_nodes[bot.path[bot.path_at + 1]].at;
        bool reached = Dist2D(me.at, a) < kWaypoint && std::fabs(me.at.z - a.z) < 150.0f;
        if (!reached && Dist2D(me.at, b) >= Dist2D(a, b))
        {
          break;
        }
        ++bot.path_at;
      }
      Vec to = bot.path_at < bot.path.size() && Dist2D(me.at, goal->at) > 400.0f ? g_nodes[bot.path[bot.path_at]].at
                                                                                  : goal->at;
      Vec way = WalkTarget(ctx, base, pawn, me.at, to, bot, frame);
      if (frame < bot.aside_until)
      {
        in.lx = bot.aside;
        in.ly = 0.3f;
      }
      else
      {
        Steer(me.at, way, view_yaw, speed, in);
      }
      if (!bot.target)
      {
        // Look where it goes.
        float want_yaw = std::atan2(way.y - me.at.y, way.x - me.at.x) * kUnitsPerRadian;
        TurnTowards(base, holder, want_yaw, 0.0f, 0.0f);
      }
      // Stuck: Y (vault), A, then a step aside and another path.
      if (frame >= bot.stuck_check)
      {
        if (bot.stuck_check && Dist2D(me.at, bot.stuck_at) < kStuckMove)
        {
          ++bot.stuck;
          in.buttons |= bot.stuck % 2 ? kY : kA;
          if (bot.stuck >= 3)
          {
            bot.side = -bot.side;
            bot.aside = Noise(frame, id + 31) < 0 ? -1.0f : 1.0f;
            bot.aside_until = frame + 40;
            bot.next_path = frame;
            if (bot.path_at + 1 < bot.path.size())
            {
              ++bot.path_at;
            }
            bot.stuck = 0;
          }
        }
        else
        {
          bot.stuck = 0;
        }
        bot.stuck_at = me.at;
        bot.stuck_check = frame + kStuckFrames;
      }
    }
    else
    {
      bot.stuck_check = 0;
    }

    // Reload an empty magazine.
    uint32_t weapon = LoadU32(base, pawn + kPawnWeapon);
    if (weapon && int32_t(LoadU32(base, weapon + kWeaponAmmo)) <= 0 && frame >= bot.next_reload)
    {
      bot.reload_until = frame + 5;
      bot.next_reload = frame + 90;
    }
    if (frame < bot.reload_until)
    {
      in.buttons = (in.buttons & ~kRT) | kX;
    }

    if (frame >= bot.next_log)
    {
      bot.next_log = frame + 150;
      Logf("frame %u: bot %u at %.0f %.0f %.0f team %08X, target %08X, goal %08X %.0f, path %zu/%zu, "
           "stick %.2f %.2f, buttons %X\n",
           frame, id, me.at.x, me.at.y, me.at.z, me.team, bot.target, goal ? goal->pawn : 0,
           goal ? goal_d : 0.0f, bot.path_at, bot.path.size(), in.lx, in.ly, in.buttons);
    }
    return in;
  }

  // The bot's record of this frame (from the template) replayed like the
  // players' records.
  void ReplayBot(PPCContext &ctx, uint8_t *base, uint32_t id, Bot &bot, const Input &in)
  {
    uint32_t record = (ctx.r1.u32 - 2048) & ~15u;
    std::memcpy(base + record, g_template.data(), kRecordSize);
    StoreU32(base, record + 60, 0);
    StoreU32(base, record + 572, 0);
    StoreU32(base, record + 588, id);
    StoreU32(base, record + 592, id);
    uint32_t keys = 0;
    auto key = [&](int index, uint32_t event, int32_t amount) {
      if (index < 0 || keys >= kMaxKeys)
      {
        return;
      }
      uint32_t entry = record + 64 + keys * 12;
      StoreU32(base, entry, uint32_t(index));
      StoreU32(base, entry + 4, event);
      StoreU32(base, entry + 8, uint32_t(amount));
      ++keys;
    };
    for (uint32_t b = 0; b < kButtonCount; ++b)
    {
      uint32_t bit = 1u << b;
      if ((in.buttons ^ bot.held) & bit)
      {
        key(g_button_keys[b], in.buttons & bit ? kPressed : kReleased, in.buttons & bit ? 127 : 0);
      }
    }
    bot.held = in.buttons;
    StoreU32(base, record + 244, keys);
    // XboxTypeS_LeftY is negative forwards.
    float values[kAxisCount] = {in.lx, -in.ly, 0, 0,
                                in.buttons & kLT ? 1.0f : 0.0f, in.buttons & kRT ? 1.0f : 0.0f};
    uint32_t axes = 0;
    for (uint32_t a = 0; a < kAxisCount && axes < 8; ++a)
    {
      if (g_axis_keys[a] < 0)
      {
        continue;
      }
      uint32_t entry = record + 248 + axes * 8;
      StoreU32(base, entry, uint32_t(g_axis_keys[a]));
      StoreU32(base, entry + 4, uint32_t(int32_t(std::lround(std::clamp(values[a], -1.0f, 1.0f) * 127.0f))));
      ++axes;
    }
    StoreU32(base, record + 312, axes);
    // The replay's InputKey / InputAxis look the controller id up among the
    // lobby's players (sub_82A02CD0, by player +0) and drop input of an id
    // nobody has; meanwhile the lookup finds a stand-in for the bot (+0 and
    // +4 its id, as a lobby player).
    g_stand_in = record + 608;
    std::memset(base + g_stand_in, 0, 64);
    StoreU32(base, g_stand_in, id);
    StoreU32(base, g_stand_in + 4, id);
    PPCContext saved = ctx;
    uint32_t sp = (record - 256) & ~15u;
    StoreU32(base, sp, ctx.r1.u32);
    ctx.r1.u64 = sp;
    ctx.r3.u64 = record;
    g_injecting = true;
    sub_82A0EEC8(ctx, base);
    g_injecting = false;
    g_stand_in = 0;
    ctx = saved;
  }

  void RunBots(PPCContext &ctx, uint8_t *base)
  {
    uint32_t modes = LoadU32(base, kGameModes);
    if (!REXCVAR_GET(ao2_versus_bots) || !g_places || !g_have_template || !modes || !LoadU32(base, modes + 48) ||
        !FindKeys(base))
    {
      return;
    }
    uint32_t frame = ao2::coop::SyncFrame(base);
    bool any = false;
    for (uint32_t id = 0; id < 4; ++id)
    {
      if (g_places & (1u << id))
      {
        g_bots[id].controller = ControllerOf(ctx, base, id);
        any |= g_bots[id].controller != 0;
      }
    }
    if (!any)
    {
      return;
    }
    CollectCharacters(base, FindWorldInfo(base));
    uint32_t source = 0; // a character the network's traces start from
    for (uint32_t id = 0; id < 4 && !source; ++id)
    {
      uint32_t pawn = g_bots[id].controller ? LoadU32(base, g_bots[id].controller + kControllerPawn) : 0;
      source = Alive(base, pawn) ? pawn : 0;
    }
    BuildNavigation(ctx, base, source);
    for (uint32_t id = 0; id < 4; ++id)
    {
      Bot &bot = g_bots[id];
      if (!(g_places & (1u << id)) || !bot.controller)
      {
        continue;
      }
      Input in = Think(ctx, base, id, bot, frame);
      ReplayBot(ctx, base, id, bot, in);
    }
  }
} // namespace

namespace ao2::versus
{
  void SetEmptyPlaces(uint32_t mask)
  {
    g_places = mask;
    g_bots = {};
    g_have_template = false;
    g_nav_world = g_nav_first = 0;
    g_nodes.clear();
    g_links.clear();
    g_nav_ready = false;
    if (mask)
    {
      Logf("empty places: %X%s\n", mask, REXCVAR_GET(ao2_versus_bots) ? ", bots play them" : "");
    }
  }

  void AfterReplayedRecord(PPCContext &ctx, uint8_t *base)
  {
    if (g_in_frame && !g_bots_ran && !g_injecting && g_places && g_have_template)
    {
      g_bots_ran = true;
      RunBots(ctx, base);
    }
  }

  void NoteReplayedRecord(const uint8_t *base, uint32_t record)
  {
    if (g_injecting || !record || !g_places)
    {
      return;
    }
    std::memcpy(g_template.data(), base + record, kRecordSize);
    g_have_template = true;
    // The first players' records with input, to see what the pads send.
    uint32_t keys = std::min(LoadU32(base, record + 244), kMaxKeys), axes = std::min(LoadU32(base, record + 312), 8u);
    bool input = keys > 0;
    for (uint32_t i = 0; i < axes; ++i)
    {
      input |= LoadU32(base, record + 252 + i * 8) != 0;
    }
    if (input && g_logged_records < 200)
    {
      ++g_logged_records;
      std::string s;
      for (uint32_t i = 0; i < keys; ++i)
      {
        uint32_t e = record + 64 + i * 12;
        s += " key " + std::to_string(LoadU32(base, e)) + "/" + std::to_string(LoadU32(base, e + 4)) + "/" +
             std::to_string(int32_t(LoadU32(base, e + 8)));
      }
      for (uint32_t i = 0; i < axes; ++i)
      {
        uint32_t e = record + 248 + i * 8;
        s += " axis " + std::to_string(LoadU32(base, e)) + "=" + std::to_string(int32_t(LoadU32(base, e + 4)));
      }
      Logf("record of %u (frame %u, +48 %d, +576 %08X %08X %08X):%s\n", LoadU32(base, record + 592),
           ao2::coop::SyncFrame(base), int32_t(LoadU32(base, record + 48)), LoadU32(base, record + 576),
           LoadU32(base, record + 580), LoadU32(base, record + 584), s.c_str());
    }
  }
} // namespace ao2::versus

// The lobby player of an id (r3 = the lobby's slots, r4 = id): while a
// bot's record is replayed, the bot's stand-in for its id.
extern "C" REX_FUNC(__imp__sub_82A02CD0);
extern "C" REX_FUNC(sub_82A02CD0)
{
  uint32_t id = ctx.r4.u32;
  __imp__sub_82A02CD0(ctx, base);
  if (!ctx.r3.u32 && g_injecting && g_stand_in && id < 4 && LoadU32(base, g_stand_in) == id)
  {
    ctx.r3.u64 = g_stand_in;
  }
}

// The lockstep's replay of a frame.
extern "C" REX_FUNC(__imp__sub_82A26290);
extern "C" REX_FUNC(sub_82A26290)
{
  g_in_frame = true;
  g_bots_ran = false;
  __imp__sub_82A26290(ctx, base);
  g_in_frame = false;
}
