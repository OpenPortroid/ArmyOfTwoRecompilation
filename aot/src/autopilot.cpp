// autopilot.cpp - Debug autopilot: the player runs along a route of points.
//
// The route comes from the file autopilot.path in the working directory
// (read and removed within 0.25 s): one point "x y z" per line (world
// units, '#' starts a comment), in the order to run them; a file with the
// word "stop" ends the run. tools/gps_path.py writes it from the level's GPS
// line (the orange ribbon meshes shown with Back / Tab).
//
// Each frame the player's pawn (PlayerController +452, Actor.Location +228)
// is projected on the route (searching forward from the last segment, the
// whole route after a jump such as a respawn) and the point
// ao2_autopilot_lookahead further along it is the target. The left stick
// points at it relative to the view (PlayerController.Rotation yaw, +244;
// 65536 = full turn, yaw 0 = +X, +Y to its right), the right stick turns the
// view towards it. It is pad input, so in online co-op it goes through the
// lockstep like a real pad. A pawn that does not move for a second while
// the stick is pushed gets Y (vault / climb over) and A (action: boost the
// partner, pull up) by turns. At the last point the player stands - the
// aimbot still fights, A now and then - until the next route or "stop".
// The aimbot shoots enemies in sight while the run goes on; in single player
// it sets the look rotation and ao2_autopilot_infinite_ammo keeps the weapon
// full, online its turn is lockstep input (mouse_aim.h TurnTowards).
//
// Vehicles (the hovercraft on the river in China): on foot, an empty boat
// (AO2HoverVehicle / AO2BoatVehicle, not hidden, no driver) within
// ao2_autopilot_board_range is walked to and boarded with A. At the wheel
// (the pawn is an AO2Vehicle) the left stick steers by the vehicle's own
// heading, RT is the gas (with ao2_autopilot_drive_corner less in sharp
// turns), the target is the average of ao2_autopilot_drive_points points over
// the stretch of route the vehicle covers in
// ao2_autopilot_drive_lookahead_time; stuck, it backs up (LT) with the wheel
// the other way. No aimbot, Y or A there (A would get out). Where the level
// shows a dock (a green boat outline) the boat is steered there and B gets
// out; at the end of a route with no dock and no new route, B too.
// Log: autopilot.log.
#include "autopilot.h"

#include "coop.h"
#include "cutscene_skip.h"
#include "log_files.h"
#include "mouse_aim.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DEFINE_DOUBLE(ao2_autopilot_lookahead, 350.0, "Debug",
                      "Autopilot: distance along the route to the point the player runs to (world units)");
REXCVAR_DEFINE_DOUBLE(ao2_autopilot_speed, 1.0, "Debug", "Autopilot: left stick deflection, 0..1");
REXCVAR_DEFINE_BOOL(ao2_autopilot_skip_cutscenes, true, "Debug",
                    "Autopilot: skip movies and cutscenes on the way (as Enter / Space; in online co-op for both)");
REXCVAR_DEFINE_BOOL(ao2_autopilot_aimbot, true, "Debug",
                    "Autopilot: stop and shoot visible enemies (aims the camera; not in online co-op)");
REXCVAR_DEFINE_BOOL(ao2_autopilot_aimbot_net, true, "Debug",
                    "Autopilot aimbot in online co-op too: the aim goes through the lockstep input like the mouse "
                    "(no infinite ammo there)");
REXCVAR_DEFINE_BOOL(ao2_autopilot_infinite_ammo, true, "Debug",
                    "Autopilot: the weapon's magazine and reserve stay full (not in online co-op)");
REXCVAR_DEFINE_DOUBLE(ao2_autopilot_aim_range, 2500.0, "Debug", "Autopilot aimbot: farthest target (world units)");
REXCVAR_DEFINE_DOUBLE(ao2_autopilot_aim_rate, 540.0, "Debug", "Autopilot aimbot: fastest turn (degrees per second)");
REXCVAR_DEFINE_DOUBLE(ao2_autopilot_turn, 1.5, "Debug",
                      "Autopilot: right stick per radian of view error (0 = the view is not turned)");
REXCVAR_DEFINE_BOOL(ao2_autopilot_drive, true, "Debug",
                    "Autopilot: board an empty boat (hovercraft) near the player and drive it along the route");
REXCVAR_DEFINE_DOUBLE(ao2_autopilot_board_range, 1500.0, "Debug",
                      "Autopilot: farthest empty boat that is walked to and boarded (world units)");
REXCVAR_DEFINE_DOUBLE(ao2_autopilot_drive_lookahead, 866.0, "Debug",
                      "Autopilot at the wheel: least distance along the route it looks ahead (world units)");
REXCVAR_DEFINE_DOUBLE(ao2_autopilot_drive_lookahead_time, 1.23, "Debug",
                      "Autopilot at the wheel: it looks ahead as far as the vehicle goes in this many seconds "
                      "(at least ao2_autopilot_drive_lookahead) and steers to the average of ao2_autopilot_drive_points points "
                      "over that stretch of the route");
REXCVAR_DEFINE_INT32(ao2_autopilot_drive_points, 1, "Debug",
                     "Autopilot at the wheel: how many points over the look-ahead stretch are averaged into the target");
REXCVAR_DEFINE_DOUBLE(ao2_autopilot_drive_corner, 0.0, "Debug",
                      "Autopilot at the wheel: gas off before bends - per radian the route turns over the next "
                      "ao2_autopilot_drive_corner_time seconds (0 = off)");
REXCVAR_DEFINE_DOUBLE(ao2_autopilot_drive_corner_time, 2.0, "Debug",
                      "Autopilot at the wheel: how far ahead (seconds at the current speed) bends slow it down");
REXCVAR_DEFINE_DOUBLE(ao2_autopilot_drive_min_gas, 0.49, "Debug", "Autopilot at the wheel: least gas while moving on");
REXCVAR_DEFINE_DOUBLE(ao2_autopilot_drive_slip, 0.0, "Debug",
                      "Autopilot at the wheel: 0 = steer by where the vehicle points, 1 = by where it moves (drift)");
REXCVAR_DEFINE_DOUBLE(ao2_autopilot_drive_predict, 0.59, "Debug",
                      "Autopilot at the wheel: steer from where the vehicle will be in this many seconds at its "
                      "current velocity (its momentum), not from where it is");
REXCVAR_DEFINE_BOOL(ao2_autopilot_drive_telemetry, false, "Debug",
                    "Autopilot at the wheel: write drive_telemetry.csv (30 Hz: time, position, heading, velocity, "
                    "inputs, route segment) for tools/boat_model.py");
REXCVAR_DEFINE_DOUBLE(ao2_autopilot_steer, 3.19, "Debug",
                      "Autopilot at the wheel: left stick per radian of heading error");
REXCVAR_DEFINE_DOUBLE(ao2_autopilot_gas, 1.0, "Debug", "Autopilot at the wheel: most gas (RT), 0..1");

namespace
{
  using clock = std::chrono::steady_clock;

  constexpr uint32_t kEngine = 0x831EFCBC; // GEngine
  constexpr uint32_t kControllerPawn = 452;
  constexpr uint32_t kActorLocation = 228;
  constexpr uint32_t kActorYaw = 244; // Rotation (pitch, yaw, roll) at +240
  constexpr uint32_t kActorVelocity = 252;
  constexpr uint32_t kMoviePlayer = 0x831E6E64;  // full-screen movie player (+88 playing), as cutscene_skip.cpp
  constexpr uint32_t kCinematicMode = 0x00020000; // PlayerController +808
  constexpr uint16_t kButtonY = 0x8000;
  constexpr float kArrive = 150.0f;      // the last point counts as reached this close
  constexpr float kLost = 1500.0f;       // farther from the route ahead: search all of it
  constexpr float kSearchAhead = 800.0f; // how far along the route the player may have got since the last frame
  constexpr float kStuckMove = 40.0f;    // moved less than this in kStuckTime with the stick pushed
  constexpr auto kStuckTime = std::chrono::milliseconds(1000);
  constexpr auto kPressTime = std::chrono::milliseconds(150);

  struct Point
  {
    float x, y, z;
  };

  std::vector<Point> g_route;
  size_t g_seg = 0;
  clock::time_point g_next_file_check;
  clock::time_point g_next_log;
  clock::time_point g_stuck_since;
  clock::time_point g_press_until;
  clock::time_point g_next_skip;
  float g_stuck_x = 0, g_stuck_y = 0;
  int g_stuck_presses = 0;

  std::atomic<bool> g_active{false};
  std::atomic<uint64_t> g_pad{0}; // lx ly rx ry (int16 each)
  std::atomic<uint16_t> g_buttons{0};
  std::atomic<uint16_t> g_triggers{0}; // LT << 8 | RT
  std::atomic<uint32_t> g_packet{0};

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("autopilot.log");
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

  // As cheats.cpp: GEngine +772 = GameViewportClient, vtable +268 = local
  // player of a controller id, LocalPlayer +64 = its PlayerController.
  uint32_t LocalController(PPCContext &ctx, uint8_t *base)
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
    ctx.r4.u64 = 0;
    rex::runtime::ResolveIndirectFunction(LoadU32(base, vtable + 268))(ctx, base);
    uint32_t player = ctx.r3.u32;
    ctx = saved;
    return player ? LoadU32(base, player + 64) : 0;
  }

  void SetPad(float lx, float ly, float rx, float ry, uint16_t buttons, uint8_t lt = 0, uint8_t rt = 0)
  {
    auto axis = [](float v) { return uint64_t(uint16_t(int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f))); };
    uint64_t pad = axis(lx) | axis(ly) << 16 | axis(rx) << 32 | axis(ry) << 48;
    uint16_t triggers = uint16_t(lt << 8 | rt);
    bool changed = g_pad.exchange(pad) != pad;
    changed |= g_buttons.exchange(buttons) != buttons;
    changed |= g_triggers.exchange(triggers) != triggers;
    if (changed)
    {
      g_packet.fetch_add(1);
    }
  }

  // ---- Aimbot: the nearest visible enemy, aimed at by the look rotation
  // (Aim, from UpdateLookingAction) and shot with LT (precision) + RT.

  constexpr uint32_t kObjects = 0x831E4E3C;    // GObjObjects (data, count)
  constexpr uint32_t kNames = 0x831D1724;      // FName::Names (data): entry +16 = chars
  constexpr uint32_t kPawnController = 464;    // Pawn.Controller
  constexpr uint32_t kPawnHealth = 708;        // Pawn.Health
  constexpr uint32_t kHeadOffset = 2780;       // AO2CharacterNative.CachedHeadBoneOffsetFromLocation
  constexpr uint32_t kTeamAI = 1088;           // AO2AIBaseNative.TeamAI
  constexpr uint32_t kPawnWeapon = 924;        // Pawn.Weapon
  constexpr uint32_t kWeaponAmmo = 692;        // UTWeapon.AmmoCount (the magazine)
  constexpr uint16_t kButtonX = 0x4000;
  constexpr uint16_t kButtonA = 0x1000;
  constexpr uint32_t kWeaponMaxAmmo = 700;     // UTWeapon.MaxAmmoCount
  constexpr uint32_t kWeaponReserve = 1980;    // AO2Weapon.ReserveAmmoCount
  constexpr uint32_t kWeaponMaxReserve = 1984; // AO2Weapon.MaxReserveAmmoCount
  clock::time_point g_next_fight_log, g_next_idle_a;
  constexpr auto kActionHold = std::chrono::seconds(10);
  clock::time_point g_hold_until;
  float g_hold_x = 0, g_hold_y = 0;
  uint16_t g_press_button = 0;
  clock::time_point g_next_reload, g_reload_until;
  constexpr uint32_t kCameraActor = 832;       // PlayerController -> AO2CameraActor
  constexpr uint32_t kLookHolder = 2024;       // [controller+2024]+3620: look rotation (float pitch, yaw, roll)
  constexpr float kUnitsPerRadian = 65536.0f / 6.2831853f;

  enum ClassFlag : uint8_t
  {
    kIsPawn = 1,
    kIsAIBase = 2,
    kIsPartner = 4,
    kIsCharacter = 8,
    kIsVehicle = 16, // AO2Vehicle: the pawn of whoever drives it
    kIsBoat = 32,    // AO2HoverVehicle / AO2BoatVehicle
  };

  std::vector<uint32_t> g_enemies; // pawns of the last scan
  clock::time_point g_next_scan;
  std::atomic<uint32_t> g_target{0};     // pawn aimed at, 0 = none
  std::atomic<bool> g_on_target{false}; // the aim is on it (fire)
  uint32_t g_target_health = 0;
  clock::time_point g_target_since, g_target_hit;
  struct Ignored
  {
    uint32_t pawn;
    clock::time_point until;
  };
  std::vector<Ignored> g_ignored; // targets that took no damage while shot at (behind a wall)

  std::string NameOf(const uint8_t *base, uint32_t object)
  {
    uint32_t names = LoadU32(base, kNames);
    uint32_t entry = names ? LoadU32(base, names + LoadU32(base, object + 44) * 4) : 0;
    if (!entry)
    {
      return {};
    }
    const char *s = reinterpret_cast<const char *>(base + entry + 16);
    return std::string(s, strnlen(s, 96));
  }

  uint8_t ClassFlags(const uint8_t *base, uint32_t cls)
  {
    static std::unordered_map<uint32_t, uint8_t> cache;
    auto it = cache.find(cls);
    if (it != cache.end())
    {
      return it->second;
    }
    uint8_t flags = 0;
    for (uint32_t c = cls, depth = 0; c && depth < 40; c = LoadU32(base, c + 60), ++depth)
    {
      std::string n = NameOf(base, c);
      flags |= n == "Pawn" ? kIsPawn : n == "AO2AIBaseNative" ? kIsAIBase : n == "AO2Partner" ? kIsPartner
               : n == "AO2CharacterNative" ? kIsCharacter : n == "AO2Vehicle" ? kIsVehicle
               : n == "AO2HoverVehicle" || n == "AO2BoatVehicle" ? kIsBoat : 0;
    }
    cache.emplace(cls, flags);
    return flags;
  }

  // Open UI scenes (a prompt over the level, the shop, the pause menu): the
  // UI scene client's ActiveScenes (TArray at +0xC0, count +0xC4; empty in
  // play), as tools/menustate.py reads it. The client is found by its class
  // name (...SceneClient) and kept while it stays that.
  bool UiOpen(const uint8_t *base, clock::time_point now)
  {
    static uint32_t client = 0;
    static clock::time_point next_search;
    auto is_client = [&](uint32_t o) {
      std::string c = NameOf(base, LoadU32(base, o + 52));
      return c.size() > 11 && c.compare(c.size() - 11, 11, "SceneClient") == 0 &&
             NameOf(base, o).rfind("Default__", 0) != 0;
    };
    if (client && !is_client(client))
    {
      client = 0;
    }
    if (!client && now >= next_search)
    {
      next_search = now + std::chrono::seconds(5);
      uint32_t objects = LoadU32(base, kObjects), count = LoadU32(base, kObjects + 4);
      for (uint32_t i = 0; objects && i < count && !client; ++i)
      {
        uint32_t o = LoadU32(base, objects + i * 4);
        if (o && is_client(o))
        {
          client = o;
        }
      }
    }
    uint32_t scenes = client ? LoadU32(base, client + 0xC4) : 0;
    return scenes > 0 && scenes < 64;
  }

  // Enemies: living pawns of AI controllers that are not on the player's
  // team (Controller.PlayerReplicationInfo -> Team against the AI's
  // TeamAI.Team; co-op has no AI partner to take the team from) - or, when
  // the player has no team, not on the AI partner's.
  constexpr uint32_t kControllerPRI = 456; // Controller.PlayerReplicationInfo
  constexpr uint32_t kPRITeam = 512;       // PlayerReplicationInfo.Team
  constexpr uint32_t kTeamAITeam = 452;    // AO2TeamAI.Team
  void ScanEnemies(const uint8_t *base, uint32_t controller)
  {
    uint32_t pri = LoadU32(base, controller + kControllerPRI);
    uint32_t player_team = pri ? LoadU32(base, pri + kPRITeam) : 0;
    uint32_t objects = LoadU32(base, kObjects), count = LoadU32(base, kObjects + 4);
    std::vector<uint32_t> bots;
    uint32_t partner_team = 0;
    for (uint32_t i = 0; objects && i < count; ++i)
    {
      uint32_t o = LoadU32(base, objects + i * 4);
      if (!o || !(ClassFlags(base, LoadU32(base, o + 52)) & kIsPawn))
      {
        continue;
      }
      uint32_t c = LoadU32(base, o + kPawnController);
      uint8_t cf = c ? ClassFlags(base, LoadU32(base, c + 52)) : 0;
      if (!(cf & kIsAIBase))
      {
        continue;
      }
      if (cf & kIsPartner)
      {
        partner_team = LoadU32(base, c + kTeamAI);
      }
      else if (int32_t(LoadU32(base, o + kPawnHealth)) > 0 &&
               (ClassFlags(base, LoadU32(base, o + 52)) & kIsCharacter)) // not turrets / vehicles
      {
        bots.push_back(o);
      }
    }
    g_enemies.clear();
    for (uint32_t o : bots)
    {
      uint32_t team_ai = LoadU32(base, LoadU32(base, o + kPawnController) + kTeamAI);
      if (player_team ? team_ai && LoadU32(base, team_ai + kTeamAITeam) != player_team
                      : partner_team && team_ai != partner_team)
      {
        g_enemies.push_back(o);
      }
    }
  }

  // ---- Boats: the nearest empty one in reach, rescanned once a second.
  constexpr uint32_t kActorFlags = 112;       // Actor.bHidden is bit 0x40000000 of this word
  constexpr uint32_t kHiddenMask = 0x40000000;
  constexpr uint32_t kVehicleDriver = 992;    // Vehicle.Driver
  constexpr float kBoardDistance = 250.0f;    // A boards from this close
  clock::time_point g_next_boat_scan, g_next_board_press;
  uint32_t g_boat = 0;
  uint32_t g_left_boat = 0; // got out of it at the end of a route: not boarded again

  bool EmptyBoat(const uint8_t *base, uint32_t o)
  {
    return (ClassFlags(base, LoadU32(base, o + 52)) & kIsBoat) && !(LoadU32(base, o + kActorFlags) & kHiddenMask) &&
           !LoadU32(base, o + kVehicleDriver) && NameOf(base, o).rfind("Default__", 0) != 0;
  }

  uint32_t BoatNear(const uint8_t *base, float x, float y, clock::time_point now)
  {
    float range = float(REXCVAR_GET(ao2_autopilot_board_range));
    if (now >= g_next_boat_scan)
    {
      g_next_boat_scan = now + std::chrono::seconds(1);
      uint32_t best = 0;
      float best_d2 = range * range;
      uint32_t objects = LoadU32(base, kObjects), count = LoadU32(base, kObjects + 4);
      for (uint32_t i = 0; objects && i < count; ++i)
      {
        uint32_t o = LoadU32(base, objects + i * 4);
        if (!o || o == g_left_boat || !EmptyBoat(base, o))
        {
          continue;
        }
        float dx = LoadF32(base, o + kActorLocation) - x, dy = LoadF32(base, o + kActorLocation + 4) - y;
        if (dx * dx + dy * dy < best_d2)
        {
          best_d2 = dx * dx + dy * dy, best = o;
        }
      }
      if (best != g_boat && best)
      {
        Logf("board: %s at %.0f\n", NameOf(base, best).c_str(), std::sqrt(best_d2));
      }
      g_boat = best;
    }
    return g_boat && EmptyBoat(base, g_boat) ? g_boat : 0;
  }

  // At the wheel: backing up after getting stuck, until this time.
  clock::time_point g_reverse_until;
  float g_reverse_steer = 0;
  // At the end of a route at the wheel: B (get out - the boat docks itself)
  // from this time on, unless a new route comes first.
  clock::time_point g_exit_from = clock::time_point::max(), g_next_exit_press;
  constexpr float kArriveDrive = 600.0f;      // the end counts as reached this close at the wheel
  constexpr float kArriveDriveStuck = 1600.0f; // ... or stuck this close (the end is on the bank)
  constexpr auto kExitWait = std::chrono::seconds(6);
  constexpr uint16_t kButtonB = 0x2000;

  // Docks: the green boat outline the level shows where the boat is to be
  // left (StaticMeshComponent of the mesh Hovercraft_GPS, material
  // GPS_green_maker). A shown one away from where the boat was boarded,
  // within kDockRange, is steered to (the route's yellow line ends on the
  // bank); kDockArrive from it B gets out.
  constexpr uint32_t kComponentFlags = 448;        // PrimitiveComponent: HiddenGame is bit 0x20000000
  constexpr uint32_t kHiddenGameMask = 0x20000000;
  constexpr uint32_t kComponentOwner = 76;
  constexpr uint32_t kLocalToWorldOrigin = 112 + 48; // PrimitiveComponent.LocalToWorld, 4th row
  constexpr uint32_t kStaticMesh = 680;
  constexpr float kDockRange = 8000.0f, kDockArrive = 300.0f, kDockAway = 2000.0f;
  struct Dock
  {
    float x, y;
  };
  std::vector<Dock> g_docks; // shown ones
  clock::time_point g_next_dock_scan;
  uint32_t g_driven = 0; // the vehicle driven, and where it was boarded
  float g_board_x = 0, g_board_y = 0;
  // This drive's numbers (logged at the dock): time, distance off the route
  // (sampled every 100 ms), times it backed up.
  clock::time_point g_drive_start, g_next_off_sample, g_next_telemetry;
  double g_off_sum = 0;
  float g_off_max = 0;
  int g_off_n = 0, g_backups = 0;

  void ScanDocks(const uint8_t *base, clock::time_point now)
  {
    if (now < g_next_dock_scan)
    {
      return;
    }
    g_next_dock_scan = now + std::chrono::seconds(1);
    g_docks.clear();
    uint32_t objects = LoadU32(base, kObjects), count = LoadU32(base, kObjects + 4);
    for (uint32_t i = 0; objects && i < count; ++i)
    {
      uint32_t o = LoadU32(base, objects + i * 4);
      if (!o || NameOf(base, LoadU32(base, o + 52)) != "StaticMeshComponent")
      {
        continue;
      }
      uint32_t mesh = LoadU32(base, o + kStaticMesh);
      if (!mesh || NameOf(base, mesh) != "Hovercraft_GPS" || (LoadU32(base, o + kComponentFlags) & kHiddenGameMask))
      {
        continue;
      }
      uint32_t owner = LoadU32(base, o + kComponentOwner);
      if (owner && (LoadU32(base, owner + kActorFlags) & kHiddenMask))
      {
        continue;
      }
      g_docks.push_back({LoadF32(base, o + kLocalToWorldOrigin), LoadF32(base, o + kLocalToWorldOrigin + 4)});
    }
  }

  void AimPoint(const uint8_t *base, uint32_t pawn, float &x, float &y, float &z)
  {
    x = LoadF32(base, pawn + kActorLocation), y = LoadF32(base, pawn + kActorLocation + 4);
    z = LoadF32(base, pawn + kActorLocation + 8);
    if (ClassFlags(base, LoadU32(base, pawn + 52)) & kIsCharacter)
    {
      x += LoadF32(base, pawn + kHeadOffset), y += LoadF32(base, pawn + kHeadOffset + 4);
      z += LoadF32(base, pawn + kHeadOffset + 8);
    }
    else
    {
      z += 50.0f;
    }
  }

  // The engine's own sight check, as the bots use it: AController::
  // LineOfSightTo(Other, bUseLOSFlag, chkLocation) (virtual, vtable +880;
  // called so by execLineOfSightTo, sub_826D8CD8) - traces from the pawn's
  // eyes to the other's location and head. Cached for kSightCache per target.
  constexpr uint32_t kLineOfSightTo = 880;
  constexpr auto kSightCache = std::chrono::milliseconds(200);
  struct Sight
  {
    clock::time_point when;
    bool visible;
  };
  std::unordered_map<uint32_t, Sight> g_sight;

  bool LineOfSight(PPCContext &ctx, uint8_t *base, uint32_t controller, uint32_t target, clock::time_point now)
  {
    auto it = g_sight.find(target);
    if (it != g_sight.end() && now - it->second.when < kSightCache)
    {
      return it->second.visible;
    }
    uint32_t vtable = LoadU32(base, controller);
    uint32_t fn = vtable ? LoadU32(base, vtable + kLineOfSightTo) : 0;
    bool visible = false;
    if (fn)
    {
      PPCContext saved = ctx;
      ctx.r1.u64 = (ctx.r1.u32 - 1024) & ~15u;
      ctx.r3.u64 = controller;
      ctx.r4.u64 = target;
      ctx.r5.u64 = 0;
      ctx.r6.u64 = 0;
      rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
      visible = ctx.r3.u32 != 0;
      ctx = saved;
    }
    g_sight[target] = {now, visible};
    return visible;
  }

  // The target for this frame (0 = none): the current one while it lives and
  // is in sight (LineOfSight), else the nearest enemy in range and in sight.
  // A target that takes no damage for 3 s while shot at (the trace sees it,
  // the bullets do not: thin cover) is ignored for a while.
  uint32_t PickTarget(PPCContext &ctx, uint8_t *base, uint32_t controller, uint32_t pawn, clock::time_point now)
  {
    if (now >= g_next_scan)
    {
      g_next_scan = now + std::chrono::milliseconds(250);
      ScanEnemies(base, controller);
      std::erase_if(g_ignored, [&](const Ignored &i) { return now >= i.until; });
      if (g_sight.size() > 256)
      {
        g_sight.clear();
      }
    }
    float px = LoadF32(base, pawn + kActorLocation), py = LoadF32(base, pawn + kActorLocation + 4);
    float range = float(REXCVAR_GET(ao2_autopilot_aim_range));
    auto usable = [&](uint32_t e) {
      if (int32_t(LoadU32(base, e + kPawnHealth)) <= 0 || !LoadU32(base, e + kPawnController))
      {
        return false;
      }
      for (const Ignored &i : g_ignored)
      {
        if (i.pawn == e)
        {
          return false;
        }
      }
      float dx = LoadF32(base, e + kActorLocation) - px, dy = LoadF32(base, e + kActorLocation + 4) - py;
      return dx * dx + dy * dy < range * range && LineOfSight(ctx, base, controller, e, now);
    };
    uint32_t current = g_target.load(std::memory_order_relaxed);
    if (current && std::find(g_enemies.begin(), g_enemies.end(), current) != g_enemies.end() && usable(current))
    {
      uint32_t health = LoadU32(base, current + kPawnHealth);
      if (health != g_target_health)
      {
        g_target_health = health, g_target_hit = now;
      }
      if (now - g_target_hit < std::chrono::milliseconds(3000))
      {
        return current;
      }
      Logf("aim: %08X takes no damage, ignored for a while\n", current);
      g_ignored.push_back({current, now + std::chrono::seconds(8)});
    }
    uint32_t best = 0;
    float best_d2 = INFINITY;
    for (uint32_t e : g_enemies)
    {
      if (!usable(e))
      {
        continue;
      }
      float dx = LoadF32(base, e + kActorLocation) - px, dy = LoadF32(base, e + kActorLocation + 4) - py;
      if (dx * dx + dy * dy < best_d2)
      {
        best_d2 = dx * dx + dy * dy, best = e;
      }
    }
    if (best && best != current)
    {
      g_target_health = LoadU32(base, best + kPawnHealth);
      g_target_since = g_target_hit = now;
      Logf("aim: target %08X (%s) at %.0f, health %d, %zu enemies\n", best, NameOf(base, best).c_str(),
           std::sqrt(best_d2), int32_t(g_target_health), g_enemies.size());
    }
    return best;
  }

  // How far the look rotation of `controller` is from `target`'s aim point
  // (rotation units), and how close counts as on target: ~2 degrees, or the
  // target's angular size up close. False without a camera.
  bool AimError(const uint8_t *base, uint32_t controller, uint32_t target, float &dyaw, float &dpitch,
                float &tolerance)
  {
    uint32_t camera = LoadU32(base, controller + kCameraActor);
    uint32_t holder = LoadU32(base, controller + kLookHolder);
    if (!target || !camera || !holder)
    {
      return false;
    }
    float tx, ty, tz;
    AimPoint(base, target, tx, ty, tz);
    float dx = tx - LoadF32(base, camera + kActorLocation), dy = ty - LoadF32(base, camera + kActorLocation + 4);
    float dz = tz - LoadF32(base, camera + kActorLocation + 8);
    float want_yaw = std::atan2(dy, dx) * kUnitsPerRadian;
    float want_pitch = std::atan2(dz, std::hypot(dx, dy)) * kUnitsPerRadian;
    uint32_t rotation = holder + 3620;
    dyaw = std::remainder(want_yaw - LoadF32(base, rotation + 4), 65536.0f);
    dpitch = std::remainder(want_pitch - LoadF32(base, rotation), 65536.0f);
    tolerance = std::max(2.0f * (65536.0f / 360.0f), std::atan2(25.0f, std::hypot(dx, dy)) * kUnitsPerRadian);
    return true;
  }

  // The route is over. `end`: the autopilot too ("stop"); else it stays on -
  // standing, the aimbot still fighting - until the next route.
  void Stop(const char *why, bool end = true)
  {
    if (!g_route.empty() || (end && g_active))
    {
      Logf("stop: %s\n", why);
    }
    g_route.clear();
    SetPad(0, 0, 0, 0, 0);
    if (end)
    {
      g_active = false;
      g_target.store(0, std::memory_order_relaxed);
    }
  }

  // Action points (autopilot.actions: "x y z button" per line, button = a b x
  // y; tools/aot.py mark records them): where the level wants a button
  // (climb a ladder, open a door), pressed when the player gets within
  // kActionRadius, then hands off for kActionHold while it plays.
  struct Action
  {
    Point at;
    uint16_t button;
    clock::time_point next;
  };
  std::vector<Action> g_actions;
  constexpr float kActionRadius = 150.0f;

  void ReadActionsFile()
  {
    std::ifstream f("autopilot.actions");
    if (!f)
    {
      return;
    }
    std::vector<Action> actions;
    std::string line;
    while (std::getline(f, line))
    {
      line = line.substr(0, line.find('#'));
      std::istringstream in(line);
      Action a{};
      std::string button;
      if (in >> a.at.x >> a.at.y >> a.at.z >> button)
      {
        a.button = button == "a" ? 0x1000 : button == "b" ? 0x2000 : button == "x" ? 0x4000 : button == "y" ? 0x8000 : 0;
        if (a.button)
        {
          actions.push_back(a);
        }
      }
    }
    f.close();
    std::remove("autopilot.actions");
    g_actions = std::move(actions);
    Logf("actions: %zu points\n", g_actions.size());
  }

  void ReadRouteFile()
  {
    std::ifstream f("autopilot.path");
    if (!f)
    {
      return;
    }
    std::vector<Point> route;
    bool stop = false;
    std::string line;
    while (std::getline(f, line))
    {
      line = line.substr(0, line.find('#'));
      std::istringstream in(line);
      Point p;
      if (in >> p.x >> p.y >> p.z)
      {
        route.push_back(p);
      }
      else if (line.find("stop") != std::string::npos)
      {
        stop = true;
      }
    }
    f.close();
    std::remove("autopilot.path");
    if (stop || route.size() < 2)
    {
      Stop(stop ? "autopilot.path: stop" : "autopilot.path: fewer than 2 points");
      return;
    }
    g_route = std::move(route);
    g_seg = 0; // from the first point (callers send the route from where the player is)
    g_stuck_since = clock::now();
    g_stuck_presses = 0;
    g_active = true;
    Logf("route: %zu points, (%.0f %.0f %.0f) -> (%.0f %.0f %.0f)\n", g_route.size(), g_route.front().x,
         g_route.front().y, g_route.front().z, g_route.back().x, g_route.back().y, g_route.back().z);
  }

  // Squared 2D distance from (x, y) to segment i, and where on it (0..1).
  float SegmentDistance(size_t i, float x, float y, float &t)
  {
    const Point &a = g_route[i], &b = g_route[i + 1];
    float dx = b.x - a.x, dy = b.y - a.y;
    float len2 = dx * dx + dy * dy;
    t = len2 > 0 ? std::clamp(((x - a.x) * dx + (y - a.y) * dy) / len2, 0.0f, 1.0f) : 0.0f;
    float px = a.x + dx * t - x, py = a.y + dy * t - y;
    return px * px + py * py;
  }

  size_t Nearest(size_t from, size_t to, float x, float y, float &t, float &d2)
  {
    size_t best = from;
    d2 = INFINITY;
    for (size_t i = from; i < to; ++i)
    {
      float ti, di = SegmentDistance(i, x, y, ti);
      if (di < d2)
      {
        d2 = di, best = i, t = ti;
      }
    }
    return best;
  }

  // Where (x, y) is on the route (g_seg: ahead of the last segment, or
  // anywhere when far off) and the point `ahead` further along it. False at
  // the end of the route.
  bool RouteTarget(float x, float y, float ahead, float &tx, float &ty, float &to_end, float &d2)
  {
    size_t segments = g_route.size() - 1;
    float t = 0;
    d2 = INFINITY;
    if (g_seg != SIZE_MAX)
    {
      // Ahead only up to kSearchAhead along the route: a route that comes back
      // past its start (a loop) must not count as done at once.
      size_t to = g_seg + 1;
      for (float along = 0; to < segments && along < kSearchAhead; ++to)
      {
        along += std::hypot(g_route[to].x - g_route[to - 1].x, g_route[to].y - g_route[to - 1].y);
      }
      size_t seg = Nearest(g_seg, std::min(segments, to), x, y, t, d2);
      if (d2 < kLost * kLost)
      {
        g_seg = seg;
      }
    }
    if (g_seg == SIZE_MAX || d2 >= kLost * kLost)
    {
      g_seg = Nearest(0, segments, x, y, t, d2);
    }
    const Point &last = g_route.back();
    to_end = std::hypot(last.x - x, last.y - y);
    if (g_seg == segments - 1 && to_end < kArrive)
    {
      return false;
    }
    size_t i = g_seg;
    tx = g_route[i].x + (g_route[i + 1].x - g_route[i].x) * t;
    ty = g_route[i].y + (g_route[i + 1].y - g_route[i].y) * t;
    while (ahead > 0)
    {
      float sx = g_route[i + 1].x - tx, sy = g_route[i + 1].y - ty;
      float len = std::hypot(sx, sy);
      if (len >= ahead)
      {
        tx += sx * ahead / len, ty += sy * ahead / len;
        break;
      }
      ahead -= len;
      tx = g_route[i + 1].x, ty = g_route[i + 1].y;
      if (++i >= segments)
      {
        break;
      }
    }
    return true;
  }

  // At the wheel: steer by the vehicle's own heading, RT for gas.
  void Drive(uint8_t *base, uint32_t pawn, float x, float y, clock::time_point now)
  {
    g_target.store(0, std::memory_order_relaxed);
    g_on_target.store(false, std::memory_order_relaxed);
    if (g_driven != pawn)
    {
      g_driven = pawn, g_board_x = x, g_board_y = y;
      g_drive_start = now, g_off_sum = 0, g_off_max = 0, g_off_n = 0, g_backups = 0;
      Logf("drive: at the wheel of %s at (%.0f %.0f)\n", NameOf(base, pawn).c_str(), x, y);
    }
    // B now and then (get out: the boat docks itself), standing still.
    auto get_out = [&](const char *why) {
      uint16_t buttons = 0;
      if (now >= g_next_exit_press)
      {
        g_next_exit_press = now + std::chrono::seconds(2);
        g_press_until = now + kPressTime;
        g_left_boat = pawn;
        Logf("drive: %s at (%.0f %.0f): B (get out)\n", why, x, y);
      }
      if (now < g_press_until)
      {
        buttons = kButtonB;
      }
      SetPad(0, 0, 0, 0, buttons);
    };

    // A dock shown away from where the boat was boarded: steer there.
    ScanDocks(base, now);
    const Dock *dock = nullptr;
    float dock_d = kDockRange;
    for (const Dock &d : g_docks)
    {
      float dd = std::hypot(d.x - x, d.y - y);
      if (dd < dock_d && std::hypot(d.x - g_board_x, d.y - g_board_y) > kDockAway)
      {
        dock = &d, dock_d = dd;
      }
    }
    if (dock && dock_d < kDockArrive)
    {
      if (g_left_boat != pawn)
      {
        Logf("drive: done in %.1f s, off avg %.0f max %.0f, backups %d\n",
             std::chrono::duration<double>(now - g_drive_start).count(), g_off_n ? g_off_sum / g_off_n : 0.0,
             g_off_max, g_backups);
      }
      get_out("at the dock");
      return;
    }

    // The target: the average of 4 points over the stretch of the route the
    // vehicle covers in ao2_autopilot_drive_lookahead_time (Actor.Velocity):
    // it turns early into a bend instead of chasing one point past it.
    float vx = LoadF32(base, pawn + kActorVelocity), vy = LoadF32(base, pawn + kActorVelocity + 4);
    float ahead = std::max(float(REXCVAR_GET(ao2_autopilot_drive_lookahead)),
                           std::hypot(vx, vy) * float(REXCVAR_GET(ao2_autopilot_drive_lookahead_time)));
    float speed = std::hypot(vx, vy);
    // Momentum: route and heading error from where it will be shortly.
    float predict = float(REXCVAR_GET(ao2_autopilot_drive_predict));
    float x0 = x, y0 = y;
    x += vx * predict, y += vy * predict;
    float tx = 0, ty = 0, to_end = 0, d2 = 0;
    bool route = !g_route.empty();
    int points = std::clamp(REXCVAR_GET(ao2_autopilot_drive_points), 1, 16);
    for (int k = 1; route && k <= points; ++k)
    {
      float px, py;
      route = RouteTarget(x, y, ahead * k / points, px, py, to_end, d2);
      tx += px / points, ty += py / points;
    }
    // How much the route turns over the next ao2_autopilot_drive_corner_time:
    // the direction of its first half against its second.
    float bend = 0;
    if (route && REXCVAR_GET(ao2_autopilot_drive_corner) > 0)
    {
      float far = std::max(400.0f, speed * float(REXCVAR_GET(ao2_autopilot_drive_corner_time)));
      float ax, ay, bx, by, cx, cy, e, dd;
      RouteTarget(x, y, 0, ax, ay, e, dd);
      RouteTarget(x, y, far / 2, bx, by, e, dd);
      RouteTarget(x, y, far, cx, cy, e, dd);
      bend = std::fabs(std::remainder(std::atan2(cy - by, cx - bx) - std::atan2(by - ay, bx - ax), 6.2831853f));
    }
    if (route && now >= g_next_off_sample)
    {
      g_next_off_sample = now + std::chrono::milliseconds(100);
      float off = std::sqrt(d2);
      g_off_sum += off, ++g_off_n, g_off_max = std::max(g_off_max, off);
    }
    if (route && g_seg + 2 >= g_route.size() && to_end < kArriveDrive)
    {
      route = false;
    }
    if (!route && !g_route.empty())
    {
      Stop("arrived", false);
      g_exit_from = now + kExitWait;
    }
    if (dock)
    {
      tx = dock->x, ty = dock->y;
    }
    else if (!route)
    {
      // No dock shown: with no new route for a while, B gets out.
      if (now >= g_exit_from)
      {
        get_out("end of the route");
      }
      else
      {
        SetPad(0, 0, 0, 0, 0);
      }
      return;
    }
    g_exit_from = route ? clock::time_point::max() : g_exit_from;
    float yaw = float(int32_t(LoadU32(base, pawn + kActorYaw))) * (6.2831853f / 65536.0f);
    // Drift: blend in the direction it moves.
    if (float slip = float(REXCVAR_GET(ao2_autopilot_drive_slip)); slip > 0 && speed > 200.0f)
    {
      yaw = std::atan2((1 - slip) * std::sin(yaw) + slip * vy / speed, (1 - slip) * std::cos(yaw) + slip * vx / speed);
    }
    float dx = tx - x, dy = ty - y;
    float error = std::atan2(-dx * std::sin(yaw) + dy * std::cos(yaw), dx * std::cos(yaw) + dy * std::sin(yaw));
    float steer = std::clamp(error * float(REXCVAR_GET(ao2_autopilot_steer)), -1.0f, 1.0f);
    // Full gas up to ~30 degrees off, down to a third at 90 and beyond; slow
    // down coming in to a dock.
    float min_gas = float(REXCVAR_GET(ao2_autopilot_drive_min_gas));
    float gas = float(REXCVAR_GET(ao2_autopilot_gas)) *
                std::clamp(1.0f - (std::fabs(error) - 0.5f) / 1.5f, min_gas, 1.0f);
    if (bend > 0.35f)
    {
      gas *= std::clamp(1.0f - float(REXCVAR_GET(ao2_autopilot_drive_corner)) * (bend - 0.35f), min_gas, 1.0f);
    }
    if (dock)
    {
      gas = std::min(gas, std::clamp(dock_d / 2000.0f, 0.25f, 1.0f));
    }

    x = x0, y = y0;
    // Stuck (a bank, a rock): back up for a while with the wheel the other way.
    constexpr float kStuckDrive = 60.0f;
    constexpr auto kStuckDriveTime = std::chrono::milliseconds(1500);
    constexpr auto kReverseTime = std::chrono::milliseconds(1200);
    // Boarding plays an animation with the boat still: not stuck then.
    constexpr auto kBoardingTime = std::chrono::seconds(3);
    if (std::hypot(x - g_stuck_x, y - g_stuck_y) > kStuckDrive || now - g_drive_start < kBoardingTime)
    {
      g_stuck_x = x, g_stuck_y = y, g_stuck_since = now;
    }
    else if (now - g_stuck_since > kStuckDriveTime && now >= g_reverse_until && !dock && to_end < kArriveDriveStuck &&
             g_seg + 2 >= g_route.size())
    {
      Stop("arrived (stuck near the end)", false);
      g_exit_from = now + kExitWait;
      SetPad(0, 0, 0, 0, 0);
      return;
    }
    else if (now - g_stuck_since > kStuckDriveTime && now >= g_reverse_until)
    {
      g_stuck_since = now + kReverseTime;
      g_reverse_until = now + kReverseTime;
      g_reverse_steer = steer >= 0 ? -1.0f : 1.0f;
      ++g_backups;
      Logf("drive: stuck at (%.0f %.0f), segment %zu: backing up\n", x, y, g_seg);
    }
    bool reverse = now < g_reverse_until;
    if (reverse)
    {
      SetPad(g_reverse_steer, 0, 0, 0, 0, 255, 0);
    }
    else
    {
      SetPad(steer, 0, 0, 0, 0, 0, uint8_t(gas * 255.0f));
    }
    if (REXCVAR_GET(ao2_autopilot_drive_telemetry) && now >= g_next_telemetry)
    {
      g_next_telemetry = now + std::chrono::milliseconds(33);
      static FILE *f = nullptr;
      if (!f && (f = ao2::OpenLogFile("drive_telemetry.csv")))
      {
        std::fprintf(f, "t,x,y,z,yaw,vx,vy,steer,gas,brake,seg,off,route_x,route_y\n");
      }
      if (f)
      {
        float rx, ry, e, dd;
        bool on = !g_route.empty() && RouteTarget(x, y, 0, rx, ry, e, dd);
        std::fprintf(f, "%.3f,%.1f,%.1f,%.1f,%.4f,%.1f,%.1f,%.3f,%.3f,%d,%zu,%.0f,%.1f,%.1f\n",
                     std::chrono::duration<double>(now - g_drive_start).count(), x, y,
                     LoadF32(base, pawn + kActorLocation + 8),
                     float(int32_t(LoadU32(base, pawn + kActorYaw))) * (6.2831853f / 65536.0f), vx, vy,
                     reverse ? g_reverse_steer : steer, reverse ? 0.0f : gas, reverse ? 1 : 0, g_seg,
                     on ? std::sqrt(dd) : -1.0f, on ? rx : 0.0f, on ? ry : 0.0f);
        std::fflush(f);
      }
    }
    if (now >= g_next_log)
    {
      g_next_log = now + std::chrono::milliseconds(500);
      Logf("drive (%.0f %.0f) seg %zu/%zu off %.0f target (%.0f %.0f)%s yaw %.0f err %.0f deg, speed %.0f bend %.0f deg, gas %.2f, "
           "to end %.0f\n",
           x, y, route ? g_seg : 0, route ? g_route.size() - 1 : 0, std::sqrt(d2), tx, ty, dock ? " dock" : "",
           yaw * 57.29578f, error * 57.29578f, speed, bend * 57.29578f, now < g_reverse_until ? -1.0f : gas,
           dock ? dock_d : to_end);
    }
  }
} // namespace

namespace ao2::autopilot
{
  void Tick(PPCContext &ctx, uint8_t *base)
  {
    auto now = clock::now();
    if (now >= g_next_file_check)
    {
      g_next_file_check = now + std::chrono::milliseconds(250);
      ReadActionsFile();
      ReadRouteFile();
    }
    if (!g_active.load(std::memory_order_relaxed))
    {
      return;
    }
    uint32_t controller = LocalController(ctx, base);
    // Online both players have a controller here and id 0 may be the
    // partner's: the one the lockstep mouse found for this machine.
    if (uint32_t mine = ao2::mouse::LockstepController(); mine && ao2::coop::OnlineLockstep(base))
    {
      controller = mine;
    }
    uint32_t pawn = controller ? LoadU32(base, controller + kControllerPawn) : 0;
    uint32_t movie = LoadU32(base, kMoviePlayer);
    bool cutscene = (movie && LoadU32(base, movie + 88)) || (controller && LoadU32(base, controller + 808) & kCinematicMode);
    if (!pawn || cutscene)
    {
      SetPad(0, 0, 0, 0, 0); // loading, dead, in a menu, a movie or cutscene: wait
      g_target.store(0, std::memory_order_relaxed);
      g_stuck_since = now;
      if (cutscene && REXCVAR_GET(ao2_autopilot_skip_cutscenes) && now >= g_next_skip)
      {
        g_next_skip = now + std::chrono::seconds(1);
        Logf("cutscene: skip\n");
        ao2::skip::Request();
      }
      return;
    }
    if (UiOpen(base, now))
    {
      SetPad(0, 0, 0, 0, 0); // a prompt / shop / pause menu: hands off (tools/aot.py closes prompts with B)
      g_target.store(0, std::memory_order_relaxed);
      g_stuck_since = now;
      g_next_idle_a = now + std::chrono::seconds(3);
      return;
    }
    float x = LoadF32(base, pawn + kActorLocation), y = LoadF32(base, pawn + kActorLocation + 4);
    float yaw = float(int32_t(LoadU32(base, controller + kActorYaw))) * (6.2831853f / 65536.0f);
    bool drive = REXCVAR_GET(ao2_autopilot_drive);
    if (drive && (ClassFlags(base, LoadU32(base, pawn + 52)) & kIsVehicle))
    {
      Drive(base, pawn, x, y, now);
      return;
    }

    bool network = ao2::coop::InNetworkGame(base);
    uint32_t weapon = LoadU32(base, pawn + kPawnWeapon);
    if (weapon && !network && REXCVAR_GET(ao2_autopilot_infinite_ammo))
    {
      // Magazine and reserve stay full (UTWeapon.AmmoCount / MaxAmmoCount,
      // AO2Weapon.ReserveAmmoCount / MaxReserveAmmoCount).
      for (auto [have, max] : {std::pair{kWeaponAmmo, kWeaponMaxAmmo}, std::pair{kWeaponReserve, kWeaponMaxReserve}})
      {
        uint32_t full = LoadU32(base, weapon + max);
        if (int32_t(full) > 0 && int32_t(full) < 100000 && LoadU32(base, weapon + have) != full)
        {
          StoreU32(base, weapon + have, full);
        }
      }
    }

    // An enemy in sight: aim (Aim turns the view), LT + RT in bursts - while
    // running on along the route (the left stick is relative to the view).
    uint32_t target = 0;
    if (REXCVAR_GET(ao2_autopilot_aimbot) && (!network || REXCVAR_GET(ao2_autopilot_aimbot_net)))
    {
      target = PickTarget(ctx, base, controller, pawn, now);
    }
    g_target.store(target, std::memory_order_relaxed);
    if (target && network)
    {
      // Online: the look rotation is the simulation's, the same on both
      // machines - the turn is sent as lockstep input (the mouse's path) and
      // comes back a few frames later, so what is on its way counts as done.
      static clock::time_point last_aim;
      float dt = std::clamp(std::chrono::duration<float>(now - last_aim).count(), 0.001f, 0.1f);
      last_aim = now;
      float dyaw, dpitch, tolerance;
      if (AimError(base, controller, target, dyaw, dpitch, tolerance))
      {
        ao2::mouse::TurnTowards(dyaw, dpitch, float(REXCVAR_GET(ao2_autopilot_aim_rate)) * (65536.0f / 360.0f) * dt);
        g_on_target.store(std::hypot(dyaw, dpitch) < tolerance, std::memory_order_relaxed);
      }
    }
    uint8_t lt = 0, rt = 0;
    uint16_t buttons = 0;
    if (target)
    {
      bool burst = (now.time_since_epoch() / std::chrono::milliseconds(150)) % 2 == 0;
      lt = 255;
      rt = g_on_target.load(std::memory_order_relaxed) && burst ? 255 : 0;
      // Empty magazine (Pawn.Weapon -> UTWeapon.AmmoCount): X reloads.
      if (weapon && int32_t(LoadU32(base, weapon + kWeaponAmmo)) <= 0)
      {
        rt = 0;
        if (now >= g_next_reload)
        {
          g_next_reload = now + std::chrono::milliseconds(1500);
          g_reload_until = now + kPressTime;
          Logf("aim: reload\n");
        }
      }
      if (now < g_reload_until)
      {
        buttons |= kButtonX;
      }
      if (now >= g_next_fight_log)
      {
        g_next_fight_log = now + std::chrono::milliseconds(500);
        Logf("fight: target %08X health %d, on target %d, magazine %d, %zu enemies\n", target,
             int32_t(LoadU32(base, target + kPawnHealth)), g_on_target.load() ? 1 : 0,
             weapon ? int32_t(LoadU32(base, weapon + kWeaponAmmo)) : -1, g_enemies.size());
      }
    }
    else
    {
      g_on_target.store(false, std::memory_order_relaxed);
    }

    // An action point here: its button, then hands off while it plays.
    for (Action &a : g_actions)
    {
      float z = LoadF32(base, pawn + kActorLocation + 8);
      if (now >= a.next && now >= g_hold_until && std::hypot(a.at.x - x, a.at.y - y) < kActionRadius &&
          std::fabs(a.at.z - z) < 300.0f)
      {
        a.next = now + std::chrono::seconds(20);
        g_press_until = now + kPressTime;
        g_press_button = a.button;
        g_hold_until = now + kActionHold;
        g_hold_x = x, g_hold_y = y;
        Logf("action at (%.0f %.0f %.0f): button %04X\n", a.at.x, a.at.y, a.at.z, a.button);
        break;
      }
    }

    if (g_route.empty())
    {
      // At the end of the route: stand (and fight) until the next one or
      // "stop"; A now and then - the end of a shown piece is often where the
      // level wants an action (boost the partner up a wall, open a door).
      if (!target && now >= g_next_idle_a && now >= g_hold_until)
      {
        g_next_idle_a = now + std::chrono::seconds(12);
        g_press_until = now + kPressTime;
        g_press_button = kButtonA;
      }
      if (now < g_press_until)
      {
        buttons |= g_press_button;
      }
      SetPad(0, 0, 0, 0, buttons, lt, rt);
      return;
    }

    float tx, ty, to_end, d2;
    if (!RouteTarget(x, y, float(REXCVAR_GET(ao2_autopilot_lookahead)), tx, ty, to_end, d2))
    {
      Stop("arrived", false);
      return;
    }
    // An empty boat near the player: walk to it, A from close by.
    if (uint32_t boat = drive && !target ? BoatNear(base, x, y, now) : 0)
    {
      tx = LoadF32(base, boat + kActorLocation), ty = LoadF32(base, boat + kActorLocation + 4);
      if (std::hypot(tx - x, ty - y) < kBoardDistance && now >= g_next_board_press)
      {
        g_next_board_press = now + std::chrono::milliseconds(1500);
        g_press_until = now + kPressTime;
        g_press_button = kButtonA;
        g_stuck_since = now;
        Logf("board: A at (%.0f %.0f)\n", x, y);
      }
    }
    size_t segments = g_route.size() - 1;

    float dx = tx - x, dy = ty - y;
    float len = std::max(std::hypot(dx, dy), 1.0f);
    dx /= len, dy /= len;
    float forward = dx * std::cos(yaw) + dy * std::sin(yaw);
    float right = -dx * std::sin(yaw) + dy * std::cos(yaw);
    float error = std::atan2(right, forward); // > 0: the target is to the right
    float speed = float(REXCVAR_GET(ao2_autopilot_speed));
    // The view turns to the route only when not aiming (then Aim turns it).
    float turn = target ? 0.0f : std::clamp(error * float(REXCVAR_GET(ao2_autopilot_turn)), -1.0f, 1.0f);
    if (std::fabs(error) < 0.05f)
    {
      turn = 0;
    }

    // Stuck: the stick is pushed but the pawn stays put -> Y (vault / climb)
    // and A (action: boost the partner, pull up, open) by turns.
    if (std::hypot(x - g_stuck_x, y - g_stuck_y) > kStuckMove)
    {
      g_stuck_x = x, g_stuck_y = y, g_stuck_since = now;
      g_stuck_presses = 0;
    }
    else if (now - g_stuck_since > kStuckTime && now >= g_hold_until)
    {
      g_stuck_since = now;
      g_press_until = now + kPressTime;
      g_press_button = g_stuck_presses % 2 ? kButtonA : kButtonY;
      ++g_stuck_presses;
      if (g_press_button == kButtonA)
      {
        // An action (boost the partner up a wall and be pulled up, ...) takes
        // a while and any other input cancels it: hands off meanwhile.
        g_hold_until = now + kActionHold;
      }
      Logf("stuck at (%.0f %.0f), segment %zu: %s (%d)\n", x, y, g_seg, g_press_button == kButtonA ? "A" : "Y",
           g_stuck_presses);
    }
    if (now < g_press_until)
    {
      buttons |= g_press_button;
    }
    if (now < g_hold_until)
    {
      g_stuck_x = x, g_stuck_y = y, g_stuck_since = now;
      if (std::hypot(x - g_hold_x, y - g_hold_y) > 300.0f)
      {
        g_hold_until = now; // moved (pulled up, climbed): go on
      }
      SetPad(0, 0, 0, 0, buttons, lt, rt);
    }
    else
    {
      g_hold_x = x, g_hold_y = y;
      SetPad(right * speed, forward * speed, turn, 0, buttons, lt, rt);
    }

    if (now >= g_next_log)
    {
      g_next_log = now + std::chrono::milliseconds(500);
      Logf("pos (%.0f %.0f %.0f) seg %zu/%zu off %.0f target (%.0f %.0f) yaw %.0f err %.0f deg, to end %.0f\n", x, y,
           LoadF32(base, pawn + kActorLocation + 8), g_seg, segments, std::sqrt(d2), tx, ty,
           yaw * 57.29578f, error * 57.29578f, to_end);
    }
  }

  void Aim(uint8_t *base, uint32_t controller, float dt)
  {
    uint32_t target = g_target.load(std::memory_order_relaxed);
    float dyaw, dpitch, tolerance;
    if (!g_active.load(std::memory_order_relaxed) || !AimError(base, controller, target, dyaw, dpitch, tolerance))
    {
      return;
    }
    uint32_t rotation = LoadU32(base, controller + kLookHolder) + 3620;
    float pitch = LoadF32(base, rotation), yaw = LoadF32(base, rotation + 4);
    float step = float(REXCVAR_GET(ao2_autopilot_aim_rate)) * (65536.0f / 360.0f) * std::clamp(dt, 0.001f, 0.1f);
    float len = std::hypot(dyaw, dpitch);
    float k = len > step ? step / len : 1.0f;
    auto store = [&](uint32_t addr, float v) {
      uint32_t u = std::byteswap(std::bit_cast<uint32_t>(v));
      std::memcpy(base + addr, &u, 4);
    };
    store(rotation + 4, yaw + dyaw * k);
    store(rotation, pitch + dpitch * k);
    g_on_target.store(len * (1.0f - k) < tolerance, std::memory_order_relaxed);
  }

  bool CurrentPad(Pad &pad)
  {
    if (!g_active.load(std::memory_order_relaxed))
    {
      return false;
    }
    uint64_t v = g_pad.load(std::memory_order_relaxed);
    pad.lx = int16_t(v), pad.ly = int16_t(v >> 16), pad.rx = int16_t(v >> 32), pad.ry = int16_t(v >> 48);
    pad.buttons = g_buttons.load(std::memory_order_relaxed);
    uint16_t triggers = g_triggers.load(std::memory_order_relaxed);
    pad.lt = uint8_t(triggers >> 8), pad.rt = uint8_t(triggers);
    pad.packet = g_packet.load(std::memory_order_relaxed);
    return true;
  }
} // namespace ao2::autopilot
