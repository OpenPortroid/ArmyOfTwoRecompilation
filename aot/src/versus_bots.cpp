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
//   Its ammo does not run out (ao2_versus_bots_infinite_ammo).
// - Lying (versus "dead": unconscious) it does nothing until its partner
//   heals it or it gets up on its own - no respawn for money. A partner
//   that lies is gone to and healed (A) when there is no target; enemies
//   that lie (invulnerable) are not shot at.
// - Without one it walks to the nearest enemy along the level's navigation
//   network (A* over the navigation points and the cover nodes, see below;
//   with no way there, to the point nearest to it it can reach), round what
//   a trace finds in the way, and looks where it goes. Walking is
//   checked with a box traced against the collision characters walk
//   against (invisible walls too), not a bullet's line. Not moving: Y
//   (vault, with LT let go: aiming it dodges) and B (out of cover) by
//   turns; Y too in a vaulting volume and held on a vault link (one through
//   a vaulting volume: nothing else can be vaulted), walked straight.
//   The way left along its path no shorter for a while: the link it
//   walks is barred from its paths for 30 s, it backs off from the wall and
//   goes another way.
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
#include <unordered_set>
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
REXCVAR_DEFINE_BOOL(ao2_versus_bots_infinite_ammo, true, "Online",
                    "Versus bots: their weapons never run out of ammo (as the game's AI partner)");
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
  constexpr uint32_t kWeaponMaxAmmo = 700;     // UTWeapon.MaxAmmoCount
  constexpr uint32_t kWeaponReserve = 1980;    // AO2Weapon.ReserveAmmoCount
  constexpr uint32_t kWeaponMaxReserve = 1984; // AO2Weapon.MaxReserveAmmoCount
  constexpr uint32_t kHeadOffset = 2780;     // AO2CharacterNative.CachedHeadBoneOffsetFromLocation
  constexpr uint32_t kTeamAI = 1088;         // AO2AIBaseNative.TeamAI
  constexpr uint32_t kTeamAITeam = 452;      // AO2TeamAI.Team
  constexpr uint32_t kLookHolder = 2024;     // controller: the character whose +3620 is the look rotation
  constexpr uint32_t kLookRotation = 3620;   // float pitch, yaw, roll (65536 = full turn)
  constexpr uint32_t kActorTrace = 0x828B8268;
  constexpr uint32_t kTraceFlagBullet = 1;
  constexpr uint32_t kTraceFlagBlocking = 8; // blocking volumes too (invisible walls)
  constexpr float kWalkExtent = 15.0f;       // half size of the box traced for walking (a character: radius 46; narrow ways)
  constexpr float kWalkTrim = 45.0f;         // ... not this near the ends

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
  constexpr float kHealRange = 3000.0f;   // a partner that lies this near is gone to and healed
  constexpr float kHealReach = 110.0f;    // ... from this close
  constexpr float kWallProbe = 250.0f;    // how far ahead the way must be clear
  constexpr uint32_t kProbeFrames = 3;    // the way ahead is probed this often
  constexpr uint32_t kProgressFrames = 150; // no nearer to its path point for this long: stuck
  constexpr float kProgressStep = 60.0f;    // ... nearer by this much
  constexpr uint32_t kBarFrames = 1800;     // a point it could not get to is left out of its paths so long
  constexpr uint32_t kEscapeFrames = 50;    // then it walks away from the wall so long

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

  // The first actor a trace from `from` to `to` hits (0 = none); `source`
  // is not hit (as autopilot.cpp TraceHit). By default a bullet's: a line,
  // the meshes' own triangles.
  uint32_t TraceHit(PPCContext &ctx, uint8_t *base, uint32_t source, const Vec &from, const Vec &to,
                    float extent = 0.0f, uint32_t flags = kTraceFlagBullet)
  {
    PPCContext saved = ctx;
    uint32_t sp = (ctx.r1.u32 - 1024) & ~15u;
    uint32_t args = sp + 128; // HitLocation, HitNormal, TraceEnd, TraceStart, Extent: 12 bytes each
    std::memset(base + args, 0, 60);
    StoreF32(base, args + 24, to.x), StoreF32(base, args + 28, to.y), StoreF32(base, args + 32, to.z);
    StoreF32(base, args + 36, from.x), StoreF32(base, args + 40, from.y), StoreF32(base, args + 44, from.z);
    StoreU32(base, sp, ctx.r1.u32);
    StoreF32(base, args + 48, extent), StoreF32(base, args + 52, extent), StoreF32(base, args + 56, extent);
    StoreU32(base, sp + 84, flags);
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

  // How a character lies: 0 standing, 1 wounded (crawls; its partner can
  // heal it), 2 unconscious or knocked down. In versus "dead" is unconscious
  // with 1 health (AO2Character.MultiplayerUnconsciousOnEntry), invulnerable,
  // until the player respawns (A, for money), its partner heals it, or it
  // gets up on its own after twice Dead_Revive_Time.
  uint8_t DownState(const uint8_t *base, uint32_t pawn)
  {
    static bool looked = false;
    static int32_t offsets[3];
    static uint32_t masks[3];
    if (!looked)
    {
      looked = true;
      const char *names[3] = {"bIsWounded", "bIsUnconscious", "bIsKnockedDown"};
      for (int i = 0; i < 3; ++i)
      {
        if (!ao2::ue3::BoolProperty(base, "AO2CharacterNative", names[i], offsets[i], masks[i]))
        {
          offsets[i] = -1;
        }
      }
    }
    auto flag = [&](int i) { return offsets[i] >= 0 && (LoadU32(base, pawn + offsets[i]) & masks[i]); };
    return flag(1) || flag(2) ? 2 : flag(0) ? 1 : 0;
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
    uint8_t down; // DownState
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
      g_characters.push_back(
          {o, c, TeamOf(base, c), IsAI(base, c), LoadVec(base, o + kActorLocation), DownState(base, o)});
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
    if (other.pawn == bot.pawn || !other.team || other.down == 2)
    {
      return false; // civilians, anybody with no team and the "dead" are left alone
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
  // Links walked by a vault (Y): only where a vaulting volume
  // (AO2GameplayMechanicsVolume, its brush's bounds) is - elsewhere nothing
  // can be vaulted, not even a low wall.
  std::unordered_set<uint64_t> g_vault_links;
  struct Box
  {
    Vec lo, hi;
  };
  std::vector<Box> g_vault_boxes;

  uint64_t LinkKey(int a, int b)
  {
    return uint64_t(uint32_t(a)) << 32 | uint32_t(b);
  }

  // The segment from a to b passes through the box (grown by `grow`).
  bool SegmentInBox(const Vec &a, const Vec &b, const Box &box, float grow)
  {
    float t0 = 0.0f, t1 = 1.0f;
    const float pa[3] = {a.x, a.y, a.z}, pb[3] = {b.x, b.y, b.z};
    const float lo[3] = {box.lo.x - grow, box.lo.y - grow, box.lo.z - grow};
    const float hi[3] = {box.hi.x + grow, box.hi.y + grow, box.hi.z + grow};
    for (int i = 0; i < 3; ++i)
    {
      float d = pb[i] - pa[i];
      if (std::fabs(d) < 1e-4f)
      {
        if (pa[i] < lo[i] || pa[i] > hi[i])
        {
          return false;
        }
        continue;
      }
      float u = (lo[i] - pa[i]) / d, v = (hi[i] - pa[i]) / d;
      t0 = std::max(t0, std::min(u, v));
      t1 = std::min(t1, std::max(u, v));
      if (t0 > t1)
      {
        return false;
      }
    }
    return true;
  }

  constexpr float kCoverBody = 32.0f;    // a cover node is this far below a character's middle
  constexpr float kLinkRange = 1100.0f;  // farthest link between points
  constexpr float kLinkClimb = 200.0f;   // most height between linked points
  constexpr float kVaultHeight = 70.0f;  // a low wall or ledge: the trace this much higher is clear (Y vaults it)
  constexpr float kVaultReach = 60.0f;   // ... and the link passes this near a vaulting volume
  constexpr float kTerrainLift = 50.0f;  // a slope: the line this much higher is clear
  constexpr float kTerrainSlope = 0.6f;  // ... no steeper than this
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

  // A character walks from one point to the other: a box traced between them
  // hits nothing but characters. Not a bullet's line: that goes through
  // invisible walls (blocking volumes block boxes only) and gaps the meshes'
  // simple collision, which characters walk against, has not.
  bool ClearLine(PPCContext &ctx, uint8_t *base, uint32_t source, const Vec &from, const Vec &to)
  {
    // A line the whole way; the box only between points kWalkTrim from the
    // ends: a cover node stands closer than the box's size to its wall, and
    // a box that starts in a wall hits it at once.
    uint32_t hit = TraceHit(ctx, base, source, from, to, 0.0f, kTraceFlagBlocking);
    if (hit && !IsCharacter(base, hit))
    {
      return false;
    }
    Vec d{to.x - from.x, to.y - from.y, to.z - from.z};
    float len = std::hypot(std::hypot(d.x, d.y), d.z);
    if (len <= 2.0f * kWalkTrim + 10.0f)
    {
      return true;
    }
    float k = kWalkTrim / len;
    Vec a{from.x + d.x * k, from.y + d.y * k, from.z + d.z * k}, b{to.x - d.x * k, to.y - d.y * k, to.z - d.z * k};
    hit = TraceHit(ctx, base, source, a, b, kWalkExtent, kTraceFlagBlocking);
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
    g_vault_links.clear();
    g_vault_boxes.clear();
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

    // The vaulting volumes' bounds.
    static int32_t brush_off = -2, bounds_off;
    if (brush_off == -2)
    {
      brush_off = ao2::ue3::PropertyOffset(base, "Brush", "BrushComponent");
      bounds_off = ao2::ue3::PropertyOffset(base, "PrimitiveComponent", "Bounds");
    }
    uint32_t vault_class = ao2::ue3::FindClass(base, "AO2GameplayMechanicsVolume");
    for (uint32_t i = 0; vault_class && brush_off >= 0 && bounds_off >= 0 && i < count; ++i)
    {
      uint32_t o = ao2::ue3::ObjectAt(base, i);
      uint32_t brush = o && ao2::ue3::IsA(base, o, vault_class) ? LoadU32(base, o + brush_off) : 0;
      if (!brush)
      {
        continue;
      }
      Vec c = LoadVec(base, brush + bounds_off), e = LoadVec(base, brush + bounds_off + 12);
      if (e.x > 0 && e.y > 0 && e.z > 0 && e.x < 5000 && e.y < 5000 && e.z < 5000)
      {
        g_vault_boxes.push_back({{c.x - e.x, c.y - e.y, c.z - e.z}, {c.x + e.x, c.y + e.y, c.z + e.z}});
      }
    }
    std::sort(g_vault_boxes.begin(), g_vault_boxes.end(),
              [](const Box &a, const Box &b) { return Before(a.lo, b.lo); });
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
    Logf("navigation: %zu points (%zu navigation points, %zu of their paths), %zu links to check, %zu vaulting "
         "volumes\n",
         g_nodes.size(), navs, specs, g_links.size(), g_vault_boxes.size());
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
      if (!clear && std::fabs(pa.z - pb.z) < kTerrainSlope * Dist2D(pa, pb))
      {
        // Over the bulge of a slope: the line hits the terrain only, one
        // kTerrainLift higher is clear.
        static uint32_t terrain_class = 0;
        terrain_class = terrain_class ? terrain_class : ao2::ue3::FindClass(base, "Terrain");
        uint32_t hit = TraceHit(ctx, base, source, pa, pb, 0.0f, kTraceFlagBlocking);
        clear = hit && terrain_class && ao2::ue3::IsA(base, hit, terrain_class) &&
                ClearLine(ctx, base, source, {pa.x, pa.y, pa.z + kTerrainLift}, {pb.x, pb.y, pb.z + kTerrainLift});
      }
      bool vault = false;
      for (size_t k = 0; !clear && k < g_vault_boxes.size() && !vault; ++k)
      {
        vault = SegmentInBox(pa, pb, g_vault_boxes[k], kVaultReach);
      }
      if (vault &&
          ClearLine(ctx, base, source, {pa.x, pa.y, pa.z + kVaultHeight}, {pb.x, pb.y, pb.z + kVaultHeight}))
      {
        clear = true;
        cost *= kVaultCost;
        g_vault_links.insert(LinkKey(a, b));
        g_vault_links.insert(LinkKey(b, a));
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
      Logf("navigation ready: %zu points, %zu links (%zu vaults), %zu parts (largest %zu %zu %zu)\n", g_nodes.size(),
           links, g_vault_links.size(), sizes.size(), sizes.size() > 0 ? sizes[0] : 0, sizes.size() > 1 ? sizes[1] : 0,
           sizes.size() > 2 ? sizes[2] : 0);
    }
  }

  // A link a bot found it could not walk: from point `from` (-1: from where
  // it stood, to its path's first point) to point `to`, until a frame.
  struct Bar
  {
    int from, to;
    uint32_t until;
  };

  bool Barred(const std::vector<Bar> &barred, int from, int to)
  {
    for (const Bar &b : barred)
    {
      if (b.from == from && b.to == to)
      {
        return true;
      }
    }
    return false;
  }

  // Of the few points nearest to `at` (not barred as a first point), the
  // nearest a trace from it reaches; else the nearest.
  int NearestNode(PPCContext &ctx, uint8_t *base, uint32_t source, const Vec &at, const std::vector<Bar> &barred)
  {
    std::vector<std::pair<float, int>> near;
    near.reserve(g_nodes.size());
    for (size_t i = 0; i < g_nodes.size(); ++i)
    {
      if (Barred(barred, -1, int(i)))
      {
        continue;
      }
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

  // A* from the node nearest to `from` to the one nearest to `to`, round
  // the barred links (ones the bot found it could not walk).
  std::vector<int> FindPath(PPCContext &ctx, uint8_t *base, uint32_t source, const Vec &from, const Vec &to,
                            const std::vector<Bar> &barred)
  {
    if (!g_nav_ready)
    {
      return {};
    }
    int start = NearestNode(ctx, base, source, from, barred), goal = NearestNode(ctx, base, source, to, {});
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
        if (Barred(barred, i, j))
        {
          continue;
        }
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
    int stuck = 0;               // times in a row it did not move
    uint32_t unaim_until = 0;    // LT let go until this frame
    uint32_t press_at = 0;       // ... and this button pressed at this frame
    uint32_t press = 0;
    uint32_t next_vault = 0;
    uint32_t maneuver_since = 0; // frame its roll or vault began (0: none)
    bool progress = false;   // counting
    float progress_left = 0; // the least way left to its goal along its path so far
    Vec progress_goal;       // where the goal was then
    uint32_t progress_until = 0;
    std::vector<Bar> barred;
    uint32_t escape_until = 0;
    float escape_dir = 0; // radians
    float walk_dir = 0;      // radians, the way chosen by the last probe
    int side = 1;            // obstacles are passed on this side first
    uint32_t next_probe = 0;
    uint32_t reload_until = 0, next_reload = 0;
    uint32_t next_respawn = 0;
    uint32_t next_log = 0;
    Vec last_at;             // where it was the frame before
    uint32_t still = 0;      // frames it has not moved with the stick pushed
  };
  std::array<Bot, 4> g_bots;

  struct Input
  {
    float lx = 0, ly = 0; // left stick: x right, y forward
    uint32_t buttons = 0;
  };

  // The bot's inventory gets infinite ammo, as the game's own AI gets it
  // (AO2AIBase.Possess: AO2InventoryManager(Pawn.InvManager).bInfiniteAmmo,
  // declared by UTInventoryManager).
  void InfiniteAmmo(uint8_t *base, uint32_t pawn)
  {
    static bool looked = false;
    static int32_t inventory = -1, flag = -1;
    static uint32_t mask = 0;
    if (!looked)
    {
      looked = true;
      inventory = ao2::ue3::PropertyOffset(base, "Pawn", "InvManager");
      if (!ao2::ue3::BoolProperty(base, "UTInventoryManager", "bInfiniteAmmo", flag, mask))
      {
        flag = -1;
      }
      Logf("infinite ammo: Pawn.InvManager +%d, UTInventoryManager.bInfiniteAmmo +%d mask %08X\n", inventory, flag,
           mask);
    }
    uint32_t manager = inventory >= 0 ? LoadU32(base, pawn + inventory) : 0;
    static uint32_t manager_class = 0, weapon_class = 0;
    manager_class = manager_class ? manager_class : ao2::ue3::FindClass(base, "AO2InventoryManager");
    weapon_class = weapon_class ? weapon_class : ao2::ue3::FindClass(base, "AO2Weapon");
    if (manager && flag >= 0 && manager_class && ao2::ue3::IsA(base, manager, manager_class))
    {
      StoreU32(base, manager + flag, LoadU32(base, manager + flag) | mask);
    }
    // That keeps the reserve; the magazine still empties and wants a
    // reload. It stays full too (as the autopilot's infinite ammo).
    uint32_t weapon = LoadU32(base, pawn + kPawnWeapon);
    if (weapon && weapon_class && ao2::ue3::IsA(base, weapon, weapon_class))
    {
      int32_t max = int32_t(LoadU32(base, weapon + kWeaponMaxAmmo));
      int32_t max_reserve = int32_t(LoadU32(base, weapon + kWeaponMaxReserve));
      if (max > 0 && max < 10000)
      {
        StoreU32(base, weapon + kWeaponAmmo, uint32_t(max));
      }
      if (max_reserve > 0 && max_reserve < 100000)
      {
        StoreU32(base, weapon + kWeaponReserve, uint32_t(max_reserve));
      }
    }
  }

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
          if (ClearLine(ctx, base, pawn, at, end))
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

  // Away from what holds the bot: of the ways back from `want` (radians),
  // the first clear one, the bot's side first.
  float EscapeDir(PPCContext &ctx, uint8_t *base, uint32_t pawn, const Vec &at, float want, int side)
  {
    for (float turn : {180.0f, 135.0f, 90.0f, 45.0f})
    {
      for (int s : {side, -side})
      {
        float a = want + float(s) * turn / 57.29578f;
        if (ClearLine(ctx, base, pawn, at, {at.x + std::cos(a) * 200.0f, at.y + std::sin(a) * 200.0f, at.z}))
        {
          return a;
        }
      }
    }
    return want + 3.1415927f;
  }

  // The character rolls or vaults (its state: the state frame's +4). Y
  // then starts a vault inside the roll, and the roll waits for its
  // animation to end for good (Suppression_Rolling_InRoll, AnimJustEnded).
  bool Maneuvering(const uint8_t *base, uint32_t pawn, std::string *state_name = nullptr)
  {
    uint32_t frame = LoadU32(base, pawn + 24);
    uint32_t state = frame ? LoadU32(base, frame + 4) : 0;
    if (!state)
    {
      return false;
    }
    std::string name = ao2::ue3::ObjectName(base, state);
    if (state_name)
    {
      *state_name = name;
    }
    return name.find("Rolling") != std::string::npos || name.find("Vault") != std::string::npos;
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
    Character me{pawn, controller, TeamOf(base, controller), false, LoadVec(base, pawn + kActorLocation),
                 DownState(base, pawn)};
    if (me.down == 2)
    {
      // Lies still until healed or up again; no paid respawn (the team's money is its score).
      bot.target = 0;
      bot.path.clear();
      bot.still = 0;
      bot.stuck_check = 0;
      return in;
    }
    std::erase_if(bot.barred, [&](const Bar &b) { return frame >= b.until; });
    std::string state;
    bool maneuvering = Maneuvering(base, pawn, &state);
    if (!maneuvering)
    {
      bot.maneuver_since = 0;
    }
    else if (!bot.maneuver_since)
    {
      bot.maneuver_since = frame;
    }
    else if (frame - bot.maneuver_since == 300)
    {
      Logf("frame %u: bot %u in %s for 300 frames\n", frame, id, state.c_str());
    }
    float view_yaw = LoadF32(base, holder + kLookRotation + 4);
    Vec eye = Head(base, pawn);
    if (REXCVAR_GET(ao2_versus_bots_infinite_ammo))
    {
      InfiniteAmmo(base, pawn);
    }

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
    if (bot.target && (!Alive(base, bot.target) || DownState(base, bot.target) == 2))
    {
      bot.target = 0;
    }

    // Where to go: towards the target; else to a partner that lies (to
    // heal it), else the nearest enemy anywhere.
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
    const Character *partner = nullptr;
    for (const Character &c : g_characters)
    {
      float d = Dist2D(me.at, c.at);
      if (!bot.target && !me.down && c.pawn != pawn && c.team && c.team == me.team && !c.ai && c.down &&
          d < kHealRange && (!partner || d < Dist2D(me.at, partner->at)))
      {
        partner = &c;
      }
    }
    if (partner)
    {
      goal = partner, goal_d = Dist2D(me.at, partner->at);
      if (goal_d < kHealReach && std::fabs(me.at.z - partner->at.z) < 150.0f)
      {
        // At its side: face it and press A (now and again: the heal starts
        // with a press, and its mini game takes presses).
        TurnTowards(base, holder, std::atan2(partner->at.y - me.at.y, partner->at.x - me.at.x) * kUnitsPerRadian,
                    -20.0f * kUnitsPerDegree, 0.0f);
        if (frame % 12 < 6)
        {
          in.buttons |= kA;
        }
        if (frame >= bot.next_log)
        {
          bot.next_log = frame + 150;
          Logf("frame %u: bot %u heals %08X (%s, down %u)\n", frame, id, partner->pawn,
               ao2::ue3::ObjectName(base, partner->pawn).c_str(), partner->down);
        }
        bot.still = 0;
        bot.stuck_check = 0;
        return in;
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
        bot.path = FindPath(ctx, base, pawn, me.at, goal->at, bot.barred);
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
      bool on_path = bot.path_at < bot.path.size() && Dist2D(me.at, goal->at) > 400.0f;
      Vec to = on_path ? g_nodes[bot.path[bot.path_at]].at : goal->at;
      int from_node = on_path && bot.path_at > 0 ? bot.path[bot.path_at - 1] : -1;
      bool vault_link = on_path && from_node >= 0 && g_vault_links.count(LinkKey(from_node, bot.path[bot.path_at]));
      // A vault: straight at the wall, not round it.
      Vec way = vault_link ? to : WalkTarget(ctx, base, pawn, me.at, to, bot, frame);
      if (frame < bot.escape_until)
      {
        way = {me.at.x + std::cos(bot.escape_dir) * 500.0f, me.at.y + std::sin(bot.escape_dir) * 500.0f, me.at.z};
        speed = 1.0f;
      }
      Steer(me.at, way, view_yaw, speed, in);
      // A ledge or a low wall on the way (a spawn's way out may be one): Y
      // in a vaulting volume, or on a vault link once the wall holds it (on
      // open ground Y rolls); LT let go (aiming, Y dodges). Walking to and
      // fro it would not stand still long enough for the stuck check's Y.
      static int32_t vaulting = ao2::ue3::PropertyOffset(base, "AO2CharacterNative", "ActiveVaultingVolume");
      bool in_volume = vaulting >= 0 && LoadU32(base, pawn + vaulting);
      bool held = Dist2D(me.at, bot.last_at) < 3.0f;
      if ((in_volume || (vault_link && held)) && frame >= bot.next_vault && frame >= bot.escape_until)
      {
        bot.next_vault = frame + 90;
        bot.unaim_until = frame + 20;
        bot.press_at = frame + 6;
        bot.press = kY;
      }
      if (!bot.target)
      {
        // Look where it goes.
        float want_yaw = std::atan2(way.y - me.at.y, way.x - me.at.x) * kUnitsPerRadian;
        TurnTowards(base, holder, want_yaw, 0.0f, 0.0f);
      }
      // Not moving: Y and B by turns. Y is the context button (vault, also
      // slide into cover or roll) but a dodge while aiming, so LT is let go
      // a moment first. B leaves cover: pushed against a wall that gives
      // cover, a character locks into it and then only slides along it.
      if (frame >= bot.stuck_check)
      {
        if (bot.stuck_check && Dist2D(me.at, bot.stuck_at) < kStuckMove)
        {
          ++bot.stuck;
          bot.unaim_until = frame + 20;
          bot.press_at = frame + 6;
          bot.press = bot.stuck % 2 ? kY : kB;
        }
        else
        {
          bot.stuck = 0;
        }
        bot.stuck_at = me.at;
        bot.stuck_check = frame + kStuckFrames;
      }
      // The way left to the goal along the path no shorter for
      // kProgressFrames (against a wall, or to and fro by a link it cannot
      // walk, back to the path's first point each time the path is found
      // again): the link it walks is barred from its paths for a while, it
      // walks away from the wall and finds another way.
      int node = on_path ? bot.path[bot.path_at] : -1;
      float left = Dist2D(me.at, to);
      if (on_path)
      {
        for (size_t k = bot.path_at; k + 1 < bot.path.size(); ++k)
        {
          left += Dist3D(g_nodes[bot.path[k]].at, g_nodes[bot.path[k + 1]].at);
        }
        left += Dist2D(g_nodes[bot.path.back()].at, goal->at);
      }
      if (!bot.progress || left < bot.progress_left - kProgressStep ||
          Dist2D(bot.progress_goal, goal->at) > 300.0f)
      {
        bot.progress = true;
        bot.progress_until = frame + kProgressFrames;
        bot.progress_left = left;
        bot.progress_goal = goal->at;
      }
      bot.progress_left = std::min(bot.progress_left, left);
      if (frame >= bot.progress_until && frame >= bot.escape_until)
      {
        float want = std::atan2(to.y - me.at.y, to.x - me.at.x);
        int from = on_path && bot.path_at > 0 ? bot.path[bot.path_at - 1] : -1;
        if (node >= 0)
        {
          bot.barred.push_back({from, node, frame + kBarFrames});
        }
        bot.side = -bot.side;
        bot.escape_dir = EscapeDir(ctx, base, pawn, me.at, want, bot.side);
        bot.escape_until = frame + kEscapeFrames;
        bot.next_path = bot.escape_until;
        bot.next_probe = 0;
        bot.progress = false;
        Logf("frame %u: bot %u gets no nearer: link %d -> %d (%.0f %.0f %.0f) from %.0f %.0f %.0f barred (%zu), "
             "backs off %.0f degrees\n",
             frame, id, from, node, to.x, to.y, to.z, me.at.x, me.at.y, me.at.z, bot.barred.size(),
             std::remainder(bot.escape_dir - want, 6.2831853f) * 57.29578f);
      }
    }
    else
    {
      bot.stuck_check = 0;
      bot.progress = false;
    }

    // Debug: what holds a bot that does not move with the stick pushed.
    {
      bool pushed = std::hypot(in.lx, in.ly) > 0.5f;
      bool moved = Dist2D(me.at, bot.last_at) > 2.0f;
      bot.last_at = me.at;
      bot.still = pushed && !moved ? bot.still + 1 : 0;
      if (bot.still == 60 || (bot.still && bot.still % 300 == 0))
      {
        static int32_t cover_node = -2, cover_ratio, vault_volume;
        if (cover_node == -2)
        {
          cover_node = ao2::ue3::PropertyOffset(base, "AO2PlayerControllerNative", "CoverEntry_CurrentCoverNode");
          cover_ratio = ao2::ue3::PropertyOffset(base, "AO2CharacterNative", "CoverRatio");
          vault_volume = ao2::ue3::PropertyOffset(base, "AO2CharacterNative", "ActiveVaultingVolume");
        }
        float yaw = view_yaw / kUnitsPerRadian;
        float dir = std::atan2(in.lx, in.ly) + yaw; // where the stick pushes, in the world
        Vec ahead{me.at.x + std::cos(dir) * 150.0f, me.at.y + std::sin(dir) * 150.0f, me.at.z};
        uint32_t hit = TraceHit(ctx, base, pawn, me.at, ahead);
        uint32_t node = cover_node >= 0 ? LoadU32(base, controller + cover_node) : 0;
        Logf("frame %u: bot %u still %u frames at %.0f %.0f %.0f: stick %.2f %.2f, buttons %X, physics %u, cover "
             "node %08X ratio %.2f, vault volume %08X, ahead %s (%s), path %zu/%zu\n",
             frame, id, bot.still, me.at.x, me.at.y, me.at.z, in.lx, in.ly, in.buttons, base[pawn + 88], node,
             cover_ratio >= 0 ? LoadF32(base, pawn + cover_ratio) : -1.0f,
             vault_volume >= 0 ? LoadU32(base, pawn + vault_volume) : 0, hit ? ao2::ue3::ObjectName(base, hit).c_str() : "-",
             hit ? ao2::ue3::ObjectName(base, LoadU32(base, hit + 52)).c_str() : "-", bot.path_at, bot.path.size());
      }
    }

    if (frame < bot.unaim_until)
    {
      in.buttons &= ~(kLT | kRT);
    }
    if (frame >= bot.press_at && frame < bot.press_at + 2 && !(bot.press == kY && maneuvering))
    {
      in.buttons |= bot.press;
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
           "stick %.2f %.2f, buttons %X, ammo %d\n",
           frame, id, me.at.x, me.at.y, me.at.z, me.team, bot.target, goal ? goal->pawn : 0,
           goal ? goal_d : 0.0f, bot.path_at, bot.path.size(), in.lx, in.ly, in.buttons,
           weapon ? int32_t(LoadU32(base, weapon + kWeaponAmmo)) : -1);
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
    g_vault_links.clear();
    g_vault_boxes.clear();
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
