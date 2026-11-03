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
// What a bot does (each frame, while its character lives), most urgent first:
// - Its partner (AO2CharacterNative.Partner: the one a heal or a co-op move
//   is with) lies: it goes to it and heals it (A), whatever it fights.
// - Its partner waits to step jump or pull up (StepJump_PreCoop_Lifter /
//   PullUp_PreCoop_Lifter): it goes to the join position (by it; under the
//   ledge for a pull up), lets LT go and presses A; in the move it leaves
//   the stick alone while it is lifted and pushes it up once it is at the
//   top (the jumper climbs over).
// - Lifted up a wall while its partner, a player, is still below: it waits
//   by the ledge ~8 s pressing A (where a pull up can be done that makes it
//   the lifter) to pull the partner up after it, then goes on.
// - Its partner, a player, stands still a moment in a step jump volume (by
//   a wall) without lifting: the bot goes to it and presses A there (the
//   first to press is the lifter), waits at the wall for it to join, and
//   lifts it with the stick held up all the way (it sets the height).
//   A partner that was the lifter there a moment ago is waited by without
//   A (it may lift again), but only ~3 s once the bot stands by it: then
//   the bot lifts it. Joining, it stands within the game's join position
//   (under 110 away, 50 in height: IsInStepJumpJoinPosition), following a
//   lifter that walks to the wall.
//   Going to join or lift its partner, it does not aim or fire.
// - A player's partner keeps near the player (follow), fighting what comes;
//   the objectives below are for bots whose partner is a bot.
// - The objectives its team is shown (the HUD's markers, AO2MPTeamInfo.
//   MPTeamObjectiveLocators): the nearest. A hostage: A by it (dragging
//   it, the bot aims at any enemy it sees); a depot to blow up: shot at
//   from where a bullet's line reaches its mesh (the middle of its
//   collision bounds, not its origin) with the weapon for its distance; a
//   character to kill: hunted as a target;
//   a place to hold or defend:
//   kept near, fighting what comes; a place to go to: gone to. Carrying a
//   hostage, the extraction only: into the middle of its volume
//   (AO2MPPlayerOccupiedTriggerVolume, whose event fires when a player steps
//   into it empty); still carrying it there after ~4 s, out and in again.
//   Not carrying one, the extraction only while one of its side carries one.
// - No objective (deathmatch): the nearest enemy.
// - Target: the objective's enemy in sight, else the nearest enemy in range
//   whose body or head a bullet trace (AActor::Trace with actors,
//   TRACEFLAG_Bullet, as autopilot.cpp) reaches. Enemies: characters of the
//   other team's players and AI soldiers whose TeamAI is not on the bot's
//   team; hostages (AO2AICivilianController, an AI of a team too) never,
//   nor a shot that passes one.
// - With a target it aims at its body (its head when only that shows), a
//   few degrees off, wandering slowly (ao2_versus_bots_miss), and fires in
//   bursts once it has had it in view for a moment; LT when it is near or
//   the bot stands (farther on the way, from the hip). Hunting: far away it
//   walks closer, near it steps sideways (not into a wall). The magazine
//   empties: X (also with a part-empty one and no target); the reserve does
//   not run out (ao2_versus_bots_infinite_ammo).
// - Under fire (hit lately, badly hurt) or in a fight at long range, with
//   no partner to heal or join and no hostage to take: cover. Of the cover
//   nodes near it, the nearest (not towards the enemy, not another bot's)
//   where a crouching character is out of the enemy's sight and one rising
//   or leaning out sees it (traces). It runs there (from the hip), pushes
//   against it a moment (the character takes it), then ducks (no LT, L3:
//   crouched - it does not crouch of itself) and rises to fire (LT) by turns - down at once when hit up, reloading down;
//   left when the enemy is gone, after 10-15 s once well, when the enemy
//   sees it ducked (another is looked for), or when its partner, a player,
//   goes on (B). Badly hurt with no cover: back to its partner.
// - Hit in the open: now and then a dodge aside (Y aiming). Hit by one it
//   sees (Pawn.LastHitBy), that one is its target at once; unseen, its
//   threat (cover from it).
// - A grenade on the ground whose blast reaches it (bounced, or slow: as
//   the game's AI, AO2DangerousEntity_Grenade): it runs away from it (out of
//   cover first), the way clear.
// - In its partner's line of fire (a player standing or walking slowly,
//   where it looks): a step aside.
// - A grenade (LB + D-pad down, LT, RT let go; then its weapon again) at an
//   enemy that hides or at enemies close together, 500-1600 away, none of
//   its own team (nor a hostage) there, the curve clear: aimed from the
//   grenade's InitialProjSpeed / InitialPitchOffset and the world's gravity
//   at 0.88 of the way (it rolls on).
// - The weapon for the enemy's distance (LB + D-pad): the slot whose
//   aiMinGoodRange - aiMaxGoodRange it is in, the primary first, the pistol
//   last.
// - Buying (ao2_versus_bots_buy): out of a fight, with cash for a dearer
//   kit (or armor) within ao2_versus_bots_buy_share of its cash, by a buy
//   zone its team is shown (one within 3000 is gone to) or, with none shown,
//   near a spawn: it opens the buy menu (bMPBuyingWeapons, as the station's
//   A does) and buys with the menu's input - the game takes the cash and
//   gives the kit. A menu it did not open is closed (B).
// - Lying (versus "dead": unconscious) it waits for its partner to heal
//   it while that is up and near; else, once a respawn can be bought
//   (Dead_Revive_Time after it fell), it buys one (A) when its cash covers
//   the price (the host's, dearer each time with ao2_versus_respawn_step)
//   and some left (ao2_versus_bots_buy: it spends the team's cash); or it
//   gets up on its own after twice that. Enemies that
//   lie (invulnerable) are not shot at.
// - It walks along the level's navigation network (A* over the navigation
//   points and the cover nodes, see below; with no way there, to the point
//   nearest to it it can reach), round what a probe finds in the way, and
//   looks where it goes. Links are checked with a box traced against the
//   collision characters walk against (invisible walls too), not a
//   bullet's line; the way ahead with a box from the bot's middle. Not
//   moving: Y (vault, with LT let go: aiming it dodges) and B (out of
//   cover) by turns; Y too in a vaulting volume and held on a vault link
//   (one through a vaulting volume: nothing else can be vaulted), walked
//   straight. Held still with the stick pushed for a moment, or the way
//   left along its path no shorter for a while: the link it walks is barred
//   from its paths for 30 s, it backs off from the wall and goes another
//   way.
// - Dead: A now and then.
//
// The settings (ao2_versus_bots and the ao2_versus_bots_* cvars, the F1
// menu's Versus tab) must be the same on every machine: a bot on one machine
// only desyncs the match. So every machine sends its own through the
// lockstep commands (coop_cmd.cpp: "vbots ...", whole numbers only: a
// decimal point parsed in another locale would differ) once the match runs
// and whenever they change, and all take them on the same frame. The bots
// play by the host's (the lowest id that says it hosts, else the lowest id
// that sent), and by the defaults until any came: the same everywhere.
#include "versus_bots.h"

#include "coop.h"
#include "coop_cmd.h"
#include "log_files.h"
#include "ue3_reflect.h"
#include "versus_rules.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
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
REXCVAR_DEFINE_DOUBLE(ao2_versus_bots_turn, 6.0, "Online", "Versus bots: fastest turn of the view (degrees per frame)");
REXCVAR_DEFINE_DOUBLE(ao2_versus_bots_miss, 2.5, "Online",
                      "Versus bots: how far off the aim wanders (degrees); 0 = they aim true");
REXCVAR_DEFINE_BOOL(ao2_versus_bots_buy, true, "Online",
                    "Versus bots: buy weapon kits and armor with their cash (at a buy zone or near a spawn), and "
                    "respawns when down");
REXCVAR_DEFINE_DOUBLE(ao2_versus_bots_buy_share, 0.5, "Online",
                      "Versus bots: the most of their cash a purchase may take (the cash is the team's score)");
REXCVAR_DEFINE_BOOL(ao2_versus_bots_infinite_ammo, true, "Online",
                    "Versus bots: their weapons never run out of ammo (as the game's AI partner)");
REXCVAR_DEFINE_INT32(ao2_versus_bots_reaction, 20, "Online",
                     "Versus bots: frames a target is in view before they shoot");
REXCVAR_DECLARE(bool, ao2_coop_server);
REXCVAR_DECLARE(int32_t, ao2_versus_round_minutes);
REXCVAR_DECLARE(int32_t, ao2_versus_cash_goal);
REXCVAR_DECLARE(int32_t, ao2_versus_friendly_fire);
REXCVAR_DECLARE(int32_t, ao2_versus_respawn_cost);
REXCVAR_DECLARE(int32_t, ao2_versus_respawn_step);

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
  constexpr float kLinkExtent = 28.0f;       // ... for the network's links (a gap too narrow is no way)

  constexpr float kUnitsPerRadian = 65536.0f / 6.2831853f;
  constexpr float kUnitsPerDegree = 65536.0f / 360.0f;
  constexpr float kMaxPitch = 70.0f * kUnitsPerDegree;

  // Timings in lockstep frames.
  constexpr uint32_t kScanFrames = 6;     // target choice
  constexpr uint32_t kPathFrames = 90;    // path again (when due: see kPathMoved)
  constexpr float kPathMoved = 150.0f;    // ... for a goal that moved this far
  constexpr uint32_t kStuckFrames = 30;   // stuck check
  constexpr float kStuckMove = 30.0f;     // moved less than this in kStuckFrames
  constexpr float kAimRange = 2500.0f;    // a target this near is aimed at (LT) on the way too
  constexpr float kNearFight = 1800.0f;   // nearer: steps aside, farther: walks closer
  constexpr float kWaypoint = 110.0f;     // a path node counts as reached this close
  constexpr float kHealRange = 3500.0f;   // a partner that lies this near is gone to and healed (it has ~20 s)
  constexpr float kHealReach = 110.0f;    // ... from this close (the game's heal: 120)
  constexpr float kJoinRange = 2500.0f;   // a partner this near waiting to step jump / pull up is gone to
  constexpr float kJoinReach = 80.0f;     // ... this close (the game's join position: 110, see kJoinNear)
  constexpr float kUseReach = 130.0f;     // a hostage or a charge is used (A) this close
  constexpr float kShootReach = 1000.0f;  // a depot to destroy is shot at from this near (in sight)
  constexpr float kUseFar = 320.0f;       // ... or held against it this close
  constexpr float kUseLeave = 350.0f;     // ... until this far
  constexpr uint32_t kPressedFrames = 10; // held still so long with the stick pushed: against it
  constexpr float kHoldReach = 350.0f;    // a place to hold or defend: kept this near
  constexpr float kGoReach = 150.0f;      // a place to go to: there this near
  constexpr uint32_t kExtractWait = 240;  // in the extraction so long still carrying the hostage: out and in again
  constexpr uint32_t kExtractOut = 45;    // ... out of it so long
  constexpr uint32_t kExtractLeaveFrames = 400; // ... at most so long on the way out
  constexpr uint32_t kGiveAsk = 70;       // a player standing still so long in a step jump volume: lifted
  constexpr float kGiveRange = 1200.0f;   // ... by its bot partner this near
  constexpr float kGiveReach = 140.0f;    // ... which presses A this near it in the volume
  constexpr uint32_t kGiveFrames = 900;   // ... and gives up after so long
  constexpr uint32_t kGiveWait = 600;     // ... waits at the wall so long for the player to join
  constexpr uint32_t kLiftMemory = 1200;  // a partner that was the lifter in a volume: wants to lift there so long
  constexpr uint32_t kReviveFrames = 880;   // AO2MPGame.Dead_Revive_Time (15 s) in lockstep frames, and a little
  constexpr int32_t kGamePenalty = 10000;    // the game's respawn price (AO2MPGame.Score_Penalty_Respawn)
  constexpr int32_t kRespawnReserve = 20000; // a respawn bought only with this much cash left after it
  constexpr uint32_t kLiftPatience = 180; // ... unless it stands by the bot so long without lifting: it is lifted
  constexpr float kJoinNear = 108.0f;     // by a partner to join: there up to this far (the game: under 110)
  constexpr float kJoinNearZ = 48.0f;     // ... and this far in height (the game: under 50)
  constexpr uint32_t kPutDownBuy = 900;   // a hostage put down: no buying so long
  constexpr uint32_t kTakeUpRetry = 50;   // a press to take up a hostage that did nothing: again after so long
  constexpr uint32_t kCarryStart = 150;   // taking up a hostage the character stands: no stuck check so long
  constexpr float kClearRange = 1500.0f;  // in its partner's line of fire this near...
  constexpr float kClearWidth = 55.0f;    // ... this near the line: steps aside
  constexpr float kClearSlow = 250.0f;    // ... while the partner moves slower than this
  constexpr float kHostageMargin = 70.0f; // no shot passes a hostage this near
  constexpr float kProbeExtent = 25.0f;   // half size of the box the bot probes its way with
  constexpr float kProbeLift = 15.0f;     // ... this much above its middle
  constexpr float kStillMove = 0.6f;      // moved less in a frame: still (~57 frames a second; walking
                                          // aiming is 1-2 a frame, against a wall ~0)
  constexpr uint32_t kWallFrames = 24;    // held still so long with the stick pushed: a wall
  constexpr uint32_t kHeldFrames = 90;    // ... a wall even rolling, vaulting or on a vault link
  constexpr uint32_t kWanderFrames = 15;  // the aim wanders to a new point this often
  constexpr uint32_t kBurstFrames = 60;   // fires 3/5 of this, then pauses
  constexpr float kCoopRise = 60.0f;      // lifted this high: up (the jumper climbs over at the top)
  constexpr uint32_t kCoopTop = 15;       // ... and no higher for this long
  constexpr float kDirectNear = 1500.0f;  // no way to a goal this near on its level: straight at it
  constexpr uint32_t kDirectOff = 1800;   // ... that failed: held so long
  constexpr float kOffLevel = 150.0f;     // a goal this much higher or lower is on another level
  constexpr float kFollowReach = 200.0f;  // a player's partner keeps this near it
  constexpr float kFollowLead = 350.0f;   // ... or ahead of it, this far, when it runs
  constexpr float kFollowMoving = 100.0f; // ... faster than this
  constexpr float kFollowRadius = 2000.0f; // a partner nearer is followed
  constexpr float kLadderReach = 150.0f;   // A for a ladder this near its end
  constexpr float kFollowLeave = 2800.0f;  // ... one farther is not (the bot goes for the objectives)
  constexpr uint32_t kActorVelocity = 252;
  constexpr float kOfferBelow = 80.0f;    // a partner this far below: offered a pull up
  constexpr uint32_t kOfferFrames = 460;  // ... for so long (~8 s)
  constexpr uint32_t kCoopMoveFrames = 2400; // a step jump / pull up taking longer is left (B; ~57 frames a second)
  constexpr float kWallProbe = 250.0f;    // how far ahead the way must be clear
  constexpr uint32_t kProbeFrames = 3;    // the way ahead is probed this often
  constexpr uint32_t kProgressFrames = 150; // no nearer to its path point for this long: stuck
  constexpr float kProgressStep = 60.0f;    // ... nearer by this much
  constexpr float kDetour = 200.0f;         // a path found again this much longer: a way round
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
    kUp = 64,     // D-pad: the weapons (EIWC_Primary)
    kLeft = 128,  // ... EIWC_Secondary
    kRight = 256, // ... EIWC_Special
    kDown = 512,  // ... EIWC_Explosives (grenades)
    kLB = 1024,   // held: the D-pad picks a weapon (else it gives co-op commands)
    kL3 = 2048,   // crouch (on / off)
    kButtonCount = 12,
  };
  // Key names per button, then the axes.
  constexpr const char *kButtonKeys[kButtonCount] = {
      "XboxTypeS_A",           "XboxTypeS_B",          "XboxTypeS_X",         "XboxTypeS_Y",
      "XboxTypeS_LeftTrigger", "XboxTypeS_RightTrigger", "XboxTypeS_DPad_Up",  "XboxTypeS_DPad_Left",
      "XboxTypeS_DPad_Right",  "XboxTypeS_DPad_Down",  "XboxTypeS_LeftShoulder", "XboxTypeS_LeftThumbstick"};
  // The D-pad button that selects an inventory slot (AO2InventoryManager.
  // ActiveAO2Weapons, by EInventoryWeaponClass).
  constexpr uint32_t kSlotButtons[4] = {kUp, kLeft, kRight, kDown};
  constexpr int kSlotGrenade = 3;
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
  bool g_rules_ran = false; // the match's rules were applied in that frame (versus_rules.cpp)
  bool g_injecting = false; // our own records are being replayed
  uint32_t g_stand_in = 0;  // the lobby player that stands in for the bot meanwhile (guest memory)
  int g_logged_records = 0;

  // ---- The match's bot settings (see the top): what each id sent, and
  // what the bots play by.
  constexpr uint32_t kLobby = 0x831F3024;  // the Plasma lobby singleton
  constexpr uint32_t kLobbyStarted = 4449; // byte: this machine's StartGame went through (the host's)
  struct Sent
  {
    bool have = false;
    bool host = false;
    bool mine = false; // sent by this machine
    ao2::versus::VersusSettings settings;
  };
  std::array<Sent, 4> g_sent_by{};
  ao2::versus::VersusSettings g_settings; // what the bots play by
  int g_settings_from = -1;            // the id whose settings they are; -1: the defaults
  std::string g_sent_line;             // what this machine sent in this match
  std::mutex g_info_mutex;             // the menu's copy (UI thread)
  ao2::versus::MatchSettingsInfo g_info;

  void PublishInfo()
  {
    std::lock_guard lock(g_info_mutex);
    g_info = {};
    g_info.from = g_settings_from;
    if (g_settings_from >= 0)
    {
      const Sent &s = g_sent_by[g_settings_from];
      g_info.host = s.host;
      g_info.mine = s.mine;
      g_info.settings = s.settings;
    }
  }

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
                    float extent = 0.0f, uint32_t flags = kTraceFlagBullet, Vec *hit_at = nullptr,
                    Vec *hit_normal = nullptr)
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
    if (hit_at)
    {
      *hit_at = LoadVec(base, args); // HitLocation
    }
    if (hit_normal)
    {
      *hit_normal = LoadVec(base, args + 12);
    }
    ctx = saved;
    return hit;
  }

  // ---- Classes and characters.

  enum ClassFlag : uint8_t
  {
    kIsCharacter = 1, // AO2CharacterNative
    kIsAI = 2,        // AO2AIBaseNative
    kIsCivilian = 4,  // AO2AICivilianController (a hostage)
  };

  // Per class, cached (classes stay while the game runs).
  uint8_t ClassFlags(const uint8_t *base, uint32_t cls)
  {
    static std::unordered_map<uint32_t, uint8_t> cache;
    static uint32_t character = 0, ai = 0, civilian = 0;
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
    civilian = civilian ? civilian : ao2::ue3::FindClass(base, "AO2AICivilianController");
    if (!character || !ai)
    {
      return 0;
    }
    uint8_t flags = 0;
    for (uint32_t c = cls, depth = 0; c && depth < 40; c = LoadU32(base, c + 60), ++depth)
    {
      flags |= c == character ? kIsCharacter : c == ai ? kIsAI : c == civilian && civilian ? kIsCivilian : 0;
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

  // A hostage's controller (an AI of a team too: shot at like a soldier).
  bool IsCivilian(const uint8_t *base, uint32_t object)
  {
    return object && (ClassFlags(base, LoadU32(base, object + 52)) & kIsCivilian);
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
    bool civilian;
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
          {o, c, TeamOf(base, c), IsAI(base, c), LoadVec(base, o + kActorLocation), DownState(base, o), IsCivilian(base, c)});
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
    if (other.pawn == bot.pawn || !other.team || other.down == 2 || other.civilian)
    {
      return false; // hostages, anybody with no team and the "dead" are left alone
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
  // between them is clear (one that climbs: and the floor along it has no
  // ledge on the way up; down a ledge it is walked one way) - checked
  // kLinksPerFrame at a time, the same on every machine (the points ordered
  // by position, not by where they are in memory).

  struct Node
  {
    Vec at;                                  // at body height
    std::vector<std::pair<int, float>> next; // node, cost
    uint32_t cover = 0;                      // its AO2CoverNode (0: another point)
  };
  std::vector<Node> g_nodes;
  std::vector<std::pair<int, int>> g_links; // pairs to check
  size_t g_links_checked = 0;
  // Floor points: a grid over the floors near the level's points (traced
  // down, every floor of a column), so stairs and ramps between levels -
  // where the level has no points - are walked too.
  struct Grid
  {
    std::vector<std::pair<int, int>> columns; // cells, by kGridStep
    size_t done = 0;
    float top = 0, bottom = 0;
    float body = 0; // a character's middle above its floor (0: not measured yet)
    std::vector<Vec> found;
    bool merged = false;
  };
  Grid g_grid;
  size_t g_ledge_links = 0; // links walked one way only (down a ledge) or not at all
  std::vector<int> g_part;  // the part of the network of each point (points linked to each other)
  // Links found not walkable (LinkFailed), and points walked from to no
  // avail: how often, and the frame of the last time. Forgotten after a
  // while (kFailMemory): kept for the match, a bot's stumbles on stairs and
  // at doors (and a stop to take up a hostage) barred more of the network
  // with every minute, and paths went round the whole map.
  struct Fails
  {
    uint8_t n = 0;
    uint32_t at = 0;
  };
  std::unordered_map<uint64_t, Fails> g_link_fails;
  std::unordered_map<int, Fails> g_point_fails;
  // Links a bot carrying a hostage fell on (and put it down): drops the
  // points' heights did not show (a ledge between two points of a slope,
  // M01 Escort: four times in a row, ~1000 frames each). Kept for the match.
  std::unordered_set<uint64_t> g_carry_falls;
  uint32_t g_nav_now = 0; // the frame (for kFailMemory)
  uint32_t g_last_block = 0; // what the last ClearLine that was not clear hit
  struct FailedLink
  {
    int a, b;
    uint32_t hit; // 0: a ledge
  };
  std::vector<FailedLink> g_failed; // for versus_nav.log

  FILE *NavLog()
  {
    static FILE *f = ao2::OpenLogFile("versus_nav.log");
    return f;
  }
  bool g_nav_ready = false;
  uint32_t g_nav_world = 0; // the WorldInfo the network is from
  uint32_t g_nav_first = 0;
  // Links walked by a vault (Y): only where a vaulting volume
  // (AO2GameplayMechanicsVolume, its brush's bounds) is - elsewhere nothing
  // can be vaulted, not even a low wall.
  std::unordered_set<uint64_t> g_vault_links;
  std::unordered_map<uint64_t, Vec> g_via; // links walked round a corner: the point beside them
  // Ladders (AO2GameplayModifier_ClimbLadder: its bottom and top): a link
  // between their ends, climbed (A at an end, then the stick up or down).
  std::vector<std::pair<Vec, Vec>> g_ladder_ends;
  std::unordered_set<uint64_t> g_ladder_links;
  struct Box
  {
    Vec lo, hi;
  };
  std::vector<Box> g_vault_boxes;
  // The volumes a hostage is taken to (AO2MPPlayerOccupiedTriggerVolume: its
  // level script's event fires when a player steps into it empty, and the
  // extraction's checks whether the player carries a hostage).
  struct Volume
  {
    uint32_t actor;
    Box box;
  };
  std::vector<Volume> g_occupied_volumes;
  std::vector<Vec> g_player_starts; // the spawns (PlayerStart): a kit may be bought near one

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
  constexpr float kViaMax = 450.0f;      // a link this short blocked: tried round a corner
  constexpr float kViaNear = 100.0f;     // ... by a point this far beside its middle
  constexpr float kViaFar = 180.0f;      // ... or this far
  constexpr float kStepCheck = 40.0f;    // a link climbing more: its floor is checked
  constexpr float kGroundStep = 30.0f;   // ... this often (a ledge from a stair's steps)
  constexpr float kGroundDepth = 400.0f; // ... this far down
  constexpr float kStepUp = 55.0f;       // a step a character walks up (stairs, a slope)
  constexpr float kDropMax = 450.0f;     // a drop it jumps down
  constexpr size_t kLinksPerPoint = 14;  // nearest points tried per point
  constexpr size_t kLinksPerFrame = 150; // links checked per frame while the network is built
  constexpr float kGridStep = 150.0f;    // floor points this far apart
  constexpr float kGridNear = 300.0f;    // ... no farther from the level's points
  constexpr float kGridMargin = 400.0f;  // columns traced from this above the highest point to below the lowest
  constexpr float kFloorNormal = 0.6f;   // a floor: its normal this near up
  constexpr float kFloorPass = 30.0f;    // the next floor down traced from this below one
  constexpr int kFloorsPerColumn = 8;
  constexpr float kFloorClear = 40.0f;
  constexpr float kLadderCost = 2.0f;    // a ladder's link costs this much per unit climbed   // ... this far from any wall
  constexpr size_t kColumnsPerFrame = 40;

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
  bool ClearLine(PPCContext &ctx, uint8_t *base, uint32_t source, const Vec &from, const Vec &to,
                 float extent = kWalkExtent)
  {
    // A line the whole way; the box only between points kWalkTrim from the
    // ends: a cover node stands closer than the box's size to its wall, and
    // a box that starts in a wall hits it at once.
    uint32_t hit = TraceHit(ctx, base, source, from, to, 0.0f, kTraceFlagBlocking);
    if (hit && !IsCharacter(base, hit))
    {
      g_last_block = hit;
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
    hit = TraceHit(ctx, base, source, a, b, extent, kTraceFlagBlocking);
    g_last_block = hit;
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
    g_via.clear();
    g_vault_boxes.clear();
    g_ladder_ends.clear();
    g_ladder_links.clear();
    g_links_checked = 0;
    g_ledge_links = 0;
    g_part.clear();
    g_link_fails.clear();
    g_point_fails.clear();
    g_carry_falls.clear();
    g_failed.clear();
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
    g_player_starts.clear();
    g_occupied_volumes.clear();
    uint32_t occupied_class = ao2::ue3::FindClass(base, "AO2MPPlayerOccupiedTriggerVolume");
    for (uint32_t i = 0; occupied_class && brush_off >= 0 && bounds_off >= 0 && i < count; ++i)
    {
      uint32_t o = ao2::ue3::ObjectAt(base, i);
      uint32_t brush = o && ao2::ue3::IsA(base, o, occupied_class) ? LoadU32(base, o + brush_off) : 0;
      if (!brush)
      {
        continue;
      }
      Vec c = LoadVec(base, brush + bounds_off), e = LoadVec(base, brush + bounds_off + 12);
      if (e.x > 0 && e.y > 0 && e.z > 0 && e.x < 5000 && e.y < 5000 && e.z < 5000)
      {
        g_occupied_volumes.push_back({o, {{c.x - e.x, c.y - e.y, c.z - e.z}, {c.x + e.x, c.y + e.y, c.z + e.z}}});
        Logf("occupied volume %s: %.0f %.0f %.0f, half size %.0f %.0f %.0f\n", ao2::ue3::ObjectName(base, o).c_str(),
             c.x, c.y, c.z, e.x, e.y, e.z);
      }
    }
    std::sort(g_occupied_volumes.begin(), g_occupied_volumes.end(),
              [](const Volume &a, const Volume &b) { return Before(a.box.lo, b.box.lo); });
    static int32_t ladder_bottom_off = -2, ladder_top_off;
    if (ladder_bottom_off == -2)
    {
      ladder_bottom_off = ao2::ue3::PropertyOffset(base, "AO2GameplayModifier_ClimbLadder", "LadderBottomLoc");
      ladder_top_off = ao2::ue3::PropertyOffset(base, "AO2GameplayModifier_ClimbLadder", "LadderTopLoc");
    }
    uint32_t ladder_class = ao2::ue3::FindClass(base, "AO2GameplayModifier_ClimbLadder");
    for (uint32_t i = 0; ladder_class && ladder_bottom_off >= 0 && ladder_top_off >= 0 && i < count; ++i)
    {
      uint32_t o = ao2::ue3::ObjectAt(base, i);
      if (!o || !ao2::ue3::IsA(base, o, ladder_class) ||
          ao2::ue3::ObjectName(base, o).rfind("Default__", 0) == 0)
      {
        continue;
      }
      Vec lo = LoadVec(base, o + ladder_bottom_off), hi = LoadVec(base, o + ladder_top_off);
      Logf("ladder %s: bottom %.0f %.0f %.0f, top %.0f %.0f %.0f\n", ao2::ue3::ObjectName(base, o).c_str(), lo.x,
           lo.y, lo.z, hi.x, hi.y, hi.z);
      if (hi.z - lo.z > kStepUp && (lo.x != 0 || lo.y != 0))
      {
        g_ladder_ends.push_back({lo, hi});
      }
    }
    std::sort(g_ladder_ends.begin(), g_ladder_ends.end(),
              [](const auto &a, const auto &b) { return Before(a.first, b.first); });
    std::sort(g_vault_boxes.begin(), g_vault_boxes.end(),
              [](const Box &a, const Box &b) { return Before(a.lo, b.lo); });
    std::unordered_map<uint32_t, int> index;
    g_nodes.resize(points.size());
    for (size_t i = 0; i < points.size(); ++i)
    {
      g_nodes[i].at = points[i].at;
      g_nodes[i].cover = points[i].nav ? 0 : points[i].actor;
      static uint32_t start_class = 0;
      start_class = start_class ? start_class : ao2::ue3::FindClass(base, "PlayerStart");
      if (points[i].nav && start_class && ao2::ue3::IsA(base, points[i].actor, start_class))
      {
        g_player_starts.push_back(points[i].at);
      }
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

    // The floor points' columns: cells near the points.
    g_grid = {};
    float lo = INFINITY, hi = -INFINITY;
    for (const Node &n : g_nodes)
    {
      lo = std::min(lo, n.at.z), hi = std::max(hi, n.at.z);
      int r = int(std::ceil(kGridNear / kGridStep));
      int cx = int(std::floor(n.at.x / kGridStep)), cy = int(std::floor(n.at.y / kGridStep));
      for (int dx = -r; dx <= r; ++dx)
      {
        for (int dy = -r; dy <= r; ++dy)
        {
          float x = (float(cx + dx) + 0.5f) * kGridStep, y = (float(cy + dy) + 0.5f) * kGridStep;
          if (std::hypot(x - n.at.x, y - n.at.y) <= kGridNear)
          {
            g_grid.columns.push_back({cx + dx, cy + dy});
          }
        }
      }
    }
    std::sort(g_grid.columns.begin(), g_grid.columns.end());
    g_grid.columns.erase(std::unique(g_grid.columns.begin(), g_grid.columns.end()), g_grid.columns.end());
    g_grid.top = hi + kGridMargin, g_grid.bottom = lo - kGridMargin;
    Logf("navigation: %zu points (%zu navigation points, %zu of their paths), %zu floor columns, %zu vaulting "
         "volumes\n",
         g_nodes.size(), navs, specs, g_grid.columns.size(), g_vault_boxes.size());
  }

  // Links to check: each point's nearest ones in reach (the points are
  // sorted by x, so only a window of them is looked at).
  void LinkCandidates()
  {
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
  }

  // The floor's height under (x, y), traced down from z (NAN: none near).
  float GroundZ(PPCContext &ctx, uint8_t *base, uint32_t source, float x, float y, float z)
  {
    Vec at;
    uint32_t hit = TraceHit(ctx, base, source, {x, y, z}, {x, y, z - kGroundDepth}, 0.0f, kTraceFlagBlocking, &at);
    return hit && !IsCharacter(base, hit) ? at.z : NAN;
  }

  // A link that climbs or falls: the floor along it, every kGroundStep.
  // A rise of more than kStepUp between two of them is a ledge (the line at
  // body height passes over its edge, the feet do not): not walked up. A
  // drop is jumped down, up to kDropMax.
  void GroundSteps(PPCContext &ctx, uint8_t *base, uint32_t source, const Vec &a, const Vec &b, bool &ab, bool &ba)
  {
    int n = std::max(2, int(std::ceil(Dist2D(a, b) / kGroundStep)));
    float prev = NAN, rise = 0, drop = 0; // walking from a to b
    for (int i = 0; i <= n; ++i)
    {
      float k = float(i) / float(n);
      float z = GroundZ(ctx, base, source, a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.z + (b.z - a.z) * k + 50.0f);
      if (std::isnan(z))
      {
        continue;
      }
      if (!std::isnan(prev))
      {
        rise = std::max(rise, z - prev);
        drop = std::max(drop, prev - z);
      }
      prev = z;
    }
    ab = rise <= kStepUp && drop <= kDropMax;
    ba = drop <= kStepUp && rise <= kDropMax;
  }

  // The floor points, kColumnsPerFrame columns at a time; then they join
  // the level's points (all sorted by position again) and the links to
  // check are chosen.
  void FloorPoints(PPCContext &ctx, uint8_t *base, uint32_t source)
  {
    if (g_grid.body == 0)
    {
      // A character's middle above its floor: as the level's points stand.
      std::vector<float> heights;
      for (size_t i = 0; i < g_nodes.size() && heights.size() < 64; i += std::max<size_t>(1, g_nodes.size() / 64))
      {
        const Vec &at = g_nodes[i].at;
        float z = GroundZ(ctx, base, source, at.x, at.y, at.z);
        if (!std::isnan(z) && at.z - z > 20.0f && at.z - z < 150.0f)
        {
          heights.push_back(at.z - z);
        }
      }
      std::sort(heights.begin(), heights.end());
      g_grid.body = heights.empty() ? 50.0f : heights[heights.size() / 2];
    }
    size_t end = std::min(g_grid.columns.size(), g_grid.done + kColumnsPerFrame);
    for (; g_grid.done < end; ++g_grid.done)
    {
      auto [cx, cy] = g_grid.columns[g_grid.done];
      float x = (float(cx) + 0.5f) * kGridStep, y = (float(cy) + 0.5f) * kGridStep;
      float z = g_grid.top;
      for (int floor = 0; floor < kFloorsPerColumn && z > g_grid.bottom; ++floor)
      {
        Vec at, normal;
        uint32_t hit =
            TraceHit(ctx, base, source, {x, y, z}, {x, y, g_grid.bottom}, 0.0f, kTraceFlagBlocking, &at, &normal);
        if (!hit)
        {
          break;
        }
        if (at.z >= z - 1.0f)
        {
          z -= kFloorPass; // started inside something
          continue;
        }
        // Up enough to stand on, a character's height clear above it.
        float head = at.z + 2.0f * g_grid.body;
        // Off a wall: moved away from walls nearer than kFloorClear (a
        // character standing against one is pushed off it, the links from it
        // would start in it) - a narrow stair keeps its point in its middle.
        bool room = !IsCharacter(base, hit) && normal.z >= kFloorNormal &&
                    !TraceHit(ctx, base, source, {x, y, at.z + 5.0f}, {x, y, head}, 0.0f, kTraceFlagBlocking);
        Vec mid{x, y, at.z + g_grid.body};
        float push_x = 0, push_y = 0;
        for (int side = 0; side < 4 && room; ++side)
        {
          float a = float(side) * 1.5707963f;
          Vec wall;
          if (TraceHit(ctx, base, source, mid, {x + std::cos(a) * kFloorClear, y + std::sin(a) * kFloorClear, mid.z},
                       0.0f, kTraceFlagBlocking, &wall))
          {
            float d = std::hypot(wall.x - x, wall.y - y);
            push_x -= std::cos(a) * (kFloorClear - d), push_y -= std::sin(a) * (kFloorClear - d);
          }
        }
        if (room && (push_x != 0 || push_y != 0))
        {
          Vec moved{x + push_x, y + push_y, mid.z};
          float ground = TraceHit(ctx, base, source, mid, moved, 0.0f, kTraceFlagBlocking)
                             ? NAN
                             : GroundZ(ctx, base, source, moved.x, moved.y, mid.z);
          room = !std::isnan(ground) && std::fabs(ground - at.z) < kStepUp;
          mid = {moved.x, moved.y, ground + g_grid.body};
        }
        if (room)
        {
          g_grid.found.push_back(mid);
        }
        z = at.z - kFloorPass;
      }
    }
    if (g_grid.done < g_grid.columns.size())
    {
      return;
    }
    // In with the level's points; their paths (the only links yet) follow.
    std::vector<Node> all = std::move(g_nodes);
    for (const Vec &v : g_grid.found)
    {
      all.push_back({v, {}});
    }
    // A ladder's ends at a character's middle: on the floor by it (the
    // top's floor may be off the ladder's line: over the drop it is far down).
    std::vector<std::pair<int, int>> ladders;
    for (const auto &[lo, hi] : g_ladder_ends)
    {
      Vec ends[2] = {lo, hi};
      for (Vec &e : ends)
      {
        float z = GroundZ(ctx, base, source, e.x, e.y, e.z + 50.0f);
        e.z = !std::isnan(z) && e.z + 50.0f - z < 150.0f ? z + g_grid.body : e.z + g_grid.body;
      }
      ladders.push_back({int(all.size()), int(all.size()) + 1});
      all.push_back({ends[0], {}});
      all.push_back({ends[1], {}});
    }
    std::vector<int> order(all.size());
    for (size_t i = 0; i < order.size(); ++i)
    {
      order[i] = int(i);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
      return Before(all[a].at, all[b].at) || (!Before(all[b].at, all[a].at) && a < b);
    });
    std::vector<int> moved(all.size());
    for (size_t i = 0; i < order.size(); ++i)
    {
      moved[order[i]] = int(i);
    }
    g_nodes.assign(all.size(), {});
    for (size_t i = 0; i < all.size(); ++i)
    {
      Node &n = g_nodes[moved[i]];
      n.at = all[i].at;
      n.cover = all[i].cover;
      for (auto [to, c] : all[i].next)
      {
        n.next.push_back({moved[to], c});
      }
    }
    for (auto [lo, hi] : ladders)
    {
      int a = moved[lo], b = moved[hi];
      float cost = kLadderCost * (g_nodes[b].at.z - g_nodes[a].at.z) + kLadderCost * 100.0f;
      AddLink(a, b, cost);
      AddLink(b, a, cost);
      g_ladder_links.insert(LinkKey(a, b));
      g_ladder_links.insert(LinkKey(b, a));
    }
    g_grid.merged = true;
    LinkCandidates();
    Logf("navigation: %zu floor points (a character's middle %.0f above its floor), %zu points, %zu links to "
         "check\n",
         g_grid.found.size(), g_grid.body, g_nodes.size(), g_links.size());
    Logf("navigation: %zu ladders\n", ladders.size());
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
    if (!g_grid.merged)
    {
      FloorPoints(ctx, base, source);
      return;
    }
    size_t end = std::min(g_links.size(), g_links_checked + kLinksPerFrame);
    for (; g_links_checked < end; ++g_links_checked)
    {
      auto [a, b] = g_links[g_links_checked];
      const Vec &pa = g_nodes[a].at, &pb = g_nodes[b].at;
      float cost = Dist3D(pa, pb);
      bool clear = ClearLine(ctx, base, source, pa, pb, kLinkExtent);
      if (!clear && std::fabs(pa.z - pb.z) < kTerrainSlope * Dist2D(pa, pb))
      {
        // Over the bulge of a slope: the line hits the terrain only, or the
        // mesh one of its ends stands on (a floor of meshes: in China one
        // cut 823 level links and the network fell apart into 320 parts),
        // and one kTerrainLift higher is clear.
        static uint32_t terrain_class = 0;
        terrain_class = terrain_class ? terrain_class : ao2::ue3::FindClass(base, "Terrain");
        uint32_t hit = TraceHit(ctx, base, source, pa, pb, kLinkExtent, kTraceFlagBlocking);
        auto floor_of = [&](const Vec &p) {
          return TraceHit(ctx, base, source, p, {p.x, p.y, p.z - g_grid.body - 60.0f}, 0.0f, kTraceFlagBlocking);
        };
        bool bulge = hit && ((terrain_class && ao2::ue3::IsA(base, hit, terrain_class)) || hit == floor_of(pa) ||
                             hit == floor_of(pb));
        clear = bulge && ClearLine(ctx, base, source, {pa.x, pa.y, pa.z + kTerrainLift},
                                   {pb.x, pb.y, pb.z + kTerrainLift}, kLinkExtent);
      }
      if (!clear && Dist2D(pa, pb) < kViaMax && std::fabs(pa.z - pb.z) < kStepCheck)
      {
        // Round a corner: cover points sit against a wall, and the line
        // from one to the next past the wall's end grazes the end. Walked by
        // a point beside the line's middle (g_via) when both halves are clear.
        Vec d{pb.x - pa.x, pb.y - pa.y, 0.0f};
        float len = std::max(1.0f, std::hypot(d.x, d.y));
        Vec side{-d.y / len, d.x / len, 0.0f};
        Vec mid{(pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f, (pa.z + pb.z) * 0.5f};
        for (float off : {kViaNear, -kViaNear, kViaFar, -kViaFar})
        {
          Vec v{mid.x + side.x * off, mid.y + side.y * off, mid.z};
          if (ClearLine(ctx, base, source, pa, v, kLinkExtent) && ClearLine(ctx, base, source, v, pb, kLinkExtent))
          {
            clear = true;
            cost = Dist3D(pa, v) + Dist3D(v, pb);
            g_via[LinkKey(a, b)] = v;
            g_via[LinkKey(b, a)] = v;
            break;
          }
        }
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
      if (!clear && NavLog())
      {
        g_failed.push_back({a, b, g_last_block});
      }
      if (clear)
      {
        bool ab = true, ba = true;
        if (!vault && std::fabs(pa.z - pb.z) > kStepCheck)
        {
          GroundSteps(ctx, base, source, pa, pb, ab, ba);
          g_ledge_links += !ab || !ba;
          if ((!ab || !ba) && NavLog())
          {
            g_failed.push_back({ab ? b : a, ab ? a : b, 0}); // the way up
          }
        }
        if (ab)
        {
          AddLink(a, b, cost);
        }
        if (ba)
        {
          AddLink(b, a, cost);
        }
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
      g_part = part;
      if (FILE *f = NavLog())
      {
        // The network for offline study: points (with their part and links),
        // the links found not walkable and what blocked them.
        std::fprintf(f, "map %s: %zu points\n", ao2::ue3::ObjectName(base, g_nav_world).c_str(), g_nodes.size());
        for (size_t i = 0; i < g_nodes.size(); ++i)
        {
          const Vec &v = g_nodes[i].at;
          std::fprintf(f, "point %zu %.0f %.0f %.0f part %d links", i, v.x, v.y, v.z, part[i]);
          for (auto [to, c] : g_nodes[i].next)
          {
            std::fprintf(f, " %d", to);
          }
          std::fprintf(f, "\n");
        }
        for (const FailedLink &l : g_failed)
        {
          std::fprintf(f, "blocked %d %d %s (%s)\n", l.a, l.b,
                       l.hit ? ao2::ue3::ObjectName(base, l.hit).c_str() : "ledge",
                       l.hit ? ao2::ue3::ObjectName(base, LoadU32(base, l.hit + 52)).c_str() : "up");
        }
        std::fflush(f);
        g_failed.clear();
      }
      std::sort(sizes.rbegin(), sizes.rend());
      Logf("navigation ready: %zu points, %zu links (%zu vaults, %zu round a corner, %zu over a ledge: one way or "
           "none), %zu parts "
           "(largest %zu %zu %zu)\n",
           g_nodes.size(), links, g_vault_links.size(), g_via.size() / 2, g_ledge_links, sizes.size(), sizes.size() > 0 ? sizes[0] : 0, sizes.size() > 1 ? sizes[1] : 0,
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

  // Links the bots found not walkable, for all of them (the same on every
  // machine: the bots think in the same order): one that failed kLinkFails
  // times is out of the network, a point kPointFails links from which
  // failed is not walked through (a pocket by a wall) - for kFailMemory.
  constexpr uint8_t kLinkFails = 2;
  constexpr uint8_t kPointFails = 3;
  constexpr uint32_t kFailMemory = 3600; // frames (a minute) after the last fail: forgotten

  uint8_t FailCount(const Fails &f)
  {
    return g_nav_now - f.at < kFailMemory ? f.n : 0;
  }

  void LinkFailed(int from, int to)
  {
    if (from >= 0 && to >= 0)
    {
      for (Fails *f : {&g_link_fails[LinkKey(from, to)], &g_point_fails[from]})
      {
        f->n = uint8_t(std::min(255, FailCount(*f) + 1));
        f->at = g_nav_now;
      }
    }
  }

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

  // The point nearest to `at` (height counted three times), -1: none.
  int NearestPoint(const Vec &at)
  {
    int best = -1;
    float best_d = INFINITY;
    for (size_t i = 0; i < g_nodes.size(); ++i)
    {
      const Vec &n = g_nodes[i].at;
      float dz = (n.z - at.z) * 3.0f;
      float d = (n.x - at.x) * (n.x - at.x) + (n.y - at.y) * (n.y - at.y) + dz * dz;
      if (d < best_d)
      {
        best = int(i), best_d = d;
      }
    }
    return best;
  }

  int PartOf(int point)
  {
    return point >= 0 && size_t(point) < g_part.size() ? g_part[point] : -1;
  }

  // Of the few points nearest to `at` (not barred as a first point; as a
  // first point, not one with no links: a path from it was "no way"), the
  // nearest a trace from it reaches; else the nearest.
  int NearestNode(PPCContext &ctx, uint8_t *base, uint32_t source, const Vec &at, const std::vector<Bar> &barred,
                  bool start = false)
  {
    std::vector<std::pair<float, int>> near;
    near.reserve(g_nodes.size());
    for (size_t i = 0; i < g_nodes.size(); ++i)
    {
      if (Barred(barred, -1, int(i)) || (start && g_nodes[i].next.empty()))
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

  // The point a path to `to` ends at: of the points near it (kGoalNear) the
  // nearest with links in the start's part of the network a trace from `to`
  // reaches, else the nearest such; else the nearest point (NearestNode).
  constexpr float kGoalNear = 800.0f;
  constexpr float kGoalFar = 300.0f; // a path's last point farther from its goal: it ends short (partial)
  int NearestGoal(PPCContext &ctx, uint8_t *base, uint32_t source, const Vec &to, int start)
  {
    int part = PartOf(start);
    std::vector<std::pair<float, int>> near;
    for (size_t i = 0; part >= 0 && i < g_nodes.size(); ++i)
    {
      const Vec &n = g_nodes[i].at;
      if (g_nodes[i].next.empty() || PartOf(int(i)) != part || Dist2D(n, to) > kGoalNear)
      {
        continue;
      }
      float dz = (n.z - to.z) * 3.0f;
      near.push_back({(n.x - to.x) * (n.x - to.x) + (n.y - to.y) * (n.y - to.y) + dz * dz, int(i)});
    }
    size_t k = std::min<size_t>(near.size(), 6);
    std::partial_sort(near.begin(), near.begin() + k, near.end());
    for (size_t i = 0; i < k; ++i)
    {
      if (ClearLine(ctx, base, source, to, g_nodes[near[i].second].at))
      {
        return near[i].second;
      }
    }
    return k ? near[0].second : NearestNode(ctx, base, source, to, {});
  }

  // A* from the node nearest to `from` to the one nearest to `to`, round
  // the barred links (ones the bot found it could not walk).
  // Carrying a hostage (`no_drops`), a link down a drop costs kDropCost more:
  // falling, a character puts the hostage down (NormalFalling) and takes it
  // up again below (~335 frames in Somalia, the walk back to it with them:
  // ~1000 units of walking). A way round longer than that: down the drop.
  // A fall of ~55 already puts it down (M01); a link a carrier fell on
  // (g_carry_falls) costs kFellCost more: it is no way down, it leads back.
  constexpr float kCarryDrop = 50.0f;
  constexpr float kDropCost = 1000.0f;
  constexpr float kFellCost = 6000.0f;
  // The links and points the bots failed on are left out (kLinkFails,
  // kPointFails); with them no way there at all (a pocket whose way out they
  // were: in Afghanistan two bots at a choke cut ~200 points off for every
  // bot), found again with them in, dearer.
  std::vector<int> FindPath(PPCContext &ctx, uint8_t *base, uint32_t source, const Vec &from, const Vec &to,
                            const std::vector<Bar> &barred, bool *partial = nullptr, bool no_drops = false,
                            bool use_fails = true)
  {
    if (partial)
    {
      *partial = false;
    }
    if (!g_nav_ready)
    {
      return {};
    }
    int start = NearestNode(ctx, base, source, from, barred, true);
    int goal = start >= 0 ? NearestGoal(ctx, base, source, to, start) : -1;
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
    bool skipped = false; // a failed link or point left out
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
        auto lf = g_link_fails.find(LinkKey(i, j));
        uint8_t fails = lf == g_link_fails.end() ? 0 : FailCount(lf->second);
        auto pf = j == goal ? g_point_fails.end() : g_point_fails.find(j);
        if (fails >= kLinkFails || (pf != g_point_fails.end() && FailCount(pf->second) >= kPointFails))
        {
          if (use_fails)
          {
            skipped = true;
            continue;
          }
          fails = std::max(fails, kLinkFails);
        }
        float n = cost[i] + c * (1.0f + 2.0f * float(fails));
        if (no_drops && g_nodes[i].at.z - g_nodes[j].at.z > kCarryDrop)
        {
          n += kDropCost;
        }
        if (no_drops && g_carry_falls.count(LinkKey(i, j)))
        {
          n += kFellCost;
        }
        if (n < cost[j])
        {
          cost[j] = n, came[j] = i;
          open.push({n + h(j), j});
        }
      }
    }
    // A goal point far from where the path is to end (no points near it: a
    // hostage lying where the floor was not sampled, 1080 from the nearest
    // in Kiev): the path ends short of it, as with no way there.
    if (partial && Dist2D(g_nodes[goal].at, to) > kGoalFar)
    {
      *partial = true;
    }
    if (start != goal && came[goal] < 0 && skipped)
    {
      return FindPath(ctx, base, source, from, to, barred, partial, no_drops, false);
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
      if (partial)
      {
        *partial = true;
      }
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
    Vec progress_at;         // ... and where the bot was
    uint32_t progress_until = 0;
    std::vector<Bar> barred;
    uint32_t escape_until = 0;
    float escape_dir = 0; // radians
    float walk_dir = 0;      // radians, the way chosen by the last probe
    int side = 1;            // obstacles are passed on this side first
    uint32_t next_probe = 0;
    uint32_t reload_until = 0, next_reload = 0;
    uint32_t next_respawn = 0;
    uint32_t down_since = 0; // lying (versus "dead") since (0: up)
    int respawns_paid = 0;   // respawns it bought in this match
    int32_t cash_down = 0;   // its cash while up (to tell a bought respawn)
    uint32_t next_log = 0;
    Vec last_at;             // where it was the frame before
    uint32_t still = 0;      // frames it has not moved with the stick pushed
    uint32_t next_strafe = 0;
    int strafe_side = 0;     // sideways: -1 left, 1 right, 0 both ways walled
    int strafe_flip = 1;     // the side its noise picks, flipped by a wall
    uint32_t objective = 0;  // the objective's actor (logged when it changes)
    uint32_t join_partner = 0, join_since = 0; // a partner waiting to lift or pull it, since
    uint32_t lift_volume = 0, lift_seen = 0;   // the step jump volume the partner was the lifter in, when
    bool join_waiting = false;                 // ... and it waits by it to be lifted (logged once)
    uint32_t join_wait_since = 0;              // ... since (0: not)
    uint32_t coop_since = 0; // frame its step jump / pull up began (0: none)
    std::vector<int> reached; // path points got to since it last got nearer (a new one is progress)
    bool partner_logged = false;
    bool offer_pending = false;                // out of a move it was lifted in: offer a pull up
    uint32_t offer_since = 0, offer_until = 0; // offering its partner a pull up
    uint32_t used = 0;       // a hostage it stands against and uses
    uint32_t next_use = 0;   // ... A pressed for it again no sooner (a press takes it up)
    uint32_t carried = 0;    // the hostage it carries (logged when that changes)
    uint32_t carry_start = 0; // frame it took it up
    uint32_t put_down_at = 0; // ... it put it down (not off to buy right after: it takes it up again)
    uint32_t extract_in_since = 0; // ... in the extraction's volume with it since (0: not in it)
    bool extract_leave = false;    // ... still carrying it there: out of the volume, then in again
    uint32_t extract_leave_at = 0, extract_out_since = 0;
    uint32_t extract_logged = 0;   // ... the extraction (logged when it changes)
    Vec extract_out;               // ... the point out of it
    // Its partner, a player, waits by a step jump wall: the bot lifts it.
    uint32_t give_since = 0;       // the partner stood still in a step jump volume since (0: not)
    uint32_t give_until = 0;       // ... the bot goes to it and lifts until then
    uint32_t next_give = 0;        // ... not again before
    bool coop_there = false;       // by its partner to join or lift (kept a little farther off)
    uint32_t flee_from = 0, flee_until = 0; // a grenade on the ground it runs from, until
    float flee_dir = 0;                     // ... which way (radians)
    uint32_t clear_until = 0;      // stepping out of its partner's line of fire until
    float clear_dir = 0;           // ... which way (radians)
    // Buying: the menu opened (a step after it), the kit and armor bought.
    int buy_step = 0;              // 0 none, 1 the kit, 2 armor
    uint32_t buy_at = 0;           // frame the step began
    int buy_kit = -1;              // the kit (-1: none, armor only)
    bool buy_armor = false;
    uint32_t next_buy = 300;       // looked at again no sooner
    bool buy_going = false;        // on its way to a buy zone
    uint32_t buy_going_until = 0;
    Vec buy_zone;
    uint32_t menu_logged = 0;      // a menu it did not open (logged once a while)
    uint32_t kit_failed = 0;       // kits it could not take (bits): not tried again
    uint32_t objective_seen = 0; // a depot to destroy it sees from where it stands
    Vec objective_aim;           // ... the point of it shot at
    uint32_t depot_logged = 0;   // the depot whose aim was logged
    bool depot_hits = false;     // ... and whether the line hit the depot itself
    // A depot out of its reach: the point it is shot at from (Vantage).
    uint32_t vantage_for = 0;      // the depot
    int vantage = -1;              // the point (-1: none found yet)
    std::vector<int> vantage_list; // points to look from, nearest the bot first
    size_t vantage_next_i = 0;     // ... the next one looked from
    uint32_t vantage_next = 0;     // looked again no sooner
    uint32_t vantage_at = 0;       // frame it stood there not seeing it since (0: not)
    std::vector<int> vantage_bad;  // points it was not seen from after all
    int vantage_logged = -1;
    std::string coop_state;  // its state in the move (logged when it changes)
    float coop_z0 = 0, coop_zmax = 0; // its height when the move began, the highest since
    uint32_t coop_rose_at = 0;        // frame it last rose
    bool force_path = false; // the path is found again when next due
    bool path_partial = false; // the path ends at the point nearest the goal (no way there)
    bool holding = false;      // ... and it holds there
    bool direct = false;       // ... or walks straight at the goal from there (kDirectNear)
    uint32_t direct_off_until = 0; // straight on failed: held till then
    bool following = true;     // a player's partner near it
    bool on_ladder = false;
    bool ladder_up = false;    // the ladder it takes: climbed up
    bool detour_ok = false;  // a longer path found again is a way round (once after a bar)
    uint64_t via_link = 0;   // the link it walks (LinkKey)
    bool via_passed = true;  // ... past its point round a corner (g_via) or none
    // Fire it takes, the enemy it fights.
    int32_t last_health = 0;
    int32_t health_top = 0;        // the most health its character had (its full health)
    uint32_t health_pawn = 0;      // ... that character
    uint32_t next_cover_log = 0;
    uint32_t hurt_at = 0;          // frame its health last fell (0: not yet)
    uint32_t threat = 0;           // the enemy it last had in sight
    uint32_t threat_seen = 0;      // ... frame
    Vec threat_at;                 // ... where
    // Cover (a cover node the threat does not see it at, from where it
    // sees the threat when it rises or leans out: LT).
    int cover = -1;                // its node (-1: none)
    uint32_t cover_since = 0;      // frame it set off for it
    uint32_t cover_arrived = 0;    // ... got there (0: on its way)
    uint32_t cover_until = 0;      // stays at most so long
    uint32_t next_cover = 0;       // looked for again from this frame
    uint32_t hide_until = 0, peek_until = 0; // ducked down, then up (aiming) by turns
    uint32_t hide_since = 0;
    uint32_t next_crouch = 0;      // L3 pressed again no sooner
    int exposed = 0;               // checks in a row the threat saw it hidden
    int bad_cover = -1;            // a node the threat saw it at (left out ...)
    uint32_t bad_until = 0;        // ... until
    uint32_t next_expose_check = 0;
    // A dodge (Y aiming) aside when hit in the open.
    uint32_t dodge_at = 0, next_dodge = 0;
    int dodge_side = 1;
    // A grenade: selected (D-pad down), aimed (LT), thrown (RT let go),
    // then the weapon taken again.
    int nade_step = 0;             // 0 none, 1 selecting, 2 aiming, 3 throwing, 4 done
    uint32_t nade_at = 0;          // frame the step began
    uint32_t next_nade = 600;
    Vec nade_to;
    float nade_pitch = 0;          // rotation units
    // The weapon for the target's distance (by its aiMinGoodRange /
    // aiMaxGoodRange): a slot of the inventory.
    int slot_want = 0;
    uint32_t slot_want_since = 0, next_switch = 0;
    uint32_t inventory_logged = 0; // the character whose inventory was logged
    uint32_t dpad = 0, dpad_at = 0; // a weapon picked: LB held, this D-pad button pressed
    uint32_t nade_projectile = 0, nade_watch_until = 0; // a thrown grenade, logged where it goes
    Vec nade_last;
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
    // That keeps the reserve (kept full here too); the magazine empties
    // and the bot reloads as a player does.
    uint32_t weapon = LoadU32(base, pawn + kPawnWeapon);
    if (weapon && weapon_class && ao2::ue3::IsA(base, weapon, weapon_class))
    {
      int32_t max_reserve = int32_t(LoadU32(base, weapon + kWeaponMaxReserve));
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
    float step = float(g_settings.turn) * kUnitsPerDegree;
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

  // A short walk from where the bot stands is clear: a line, and a box
  // nearly a character's width from its middle on. Not ClearLine: that
  // leaves out the box's first kWalkTrim, and pressed against a wall the box
  // then starts past the wall's face and finds the way clear.
  bool ProbeClear(PPCContext &ctx, uint8_t *base, uint32_t pawn, const Vec &at, const Vec &to)
  {
    uint32_t hit = TraceHit(ctx, base, pawn, at, to, 0.0f, kTraceFlagBlocking);
    if (hit && !IsCharacter(base, hit))
    {
      return false;
    }
    Vec a{at.x, at.y, at.z + kProbeLift}, b{to.x, to.y, to.z + kProbeLift};
    hit = TraceHit(ctx, base, pawn, a, b, kProbeExtent, kTraceFlagBlocking);
    return !hit || IsCharacter(base, hit);
  }

  // A point `dist` from `at` in direction `a` (radians), rising or falling
  // as the way to `to` does (a probe level with the bot hits a slope up).
  Vec ProbeEnd(const Vec &at, const Vec &to, float a, float dist)
  {
    float climb = (to.z - at.z) * std::min(1.0f, dist / std::max(1.0f, Dist2D(at, to)));
    return {at.x + std::cos(a) * dist, at.y + std::sin(a) * dist, at.z + climb};
  }

  // The way towards `to`: straight on when a probe kWallProbe ahead is
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
          if (ProbeClear(ctx, base, pawn, at, ProbeEnd(at, to, a, probe)))
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
        if (ProbeClear(ctx, base, pawn, at, {at.x + std::cos(a) * 200.0f, at.y + std::sin(a) * 200.0f, at.z}))
        {
          return a;
        }
      }
    }
    return want + 3.1415927f;
  }

  // The state a character is in (its state frame's +4: the innermost state
  // of the character's state machine, e.g. Suppression_Rolling_InRoll),
  // "" when none.
  std::string StateName(const uint8_t *base, uint32_t pawn)
  {
    uint32_t frame = LoadU32(base, pawn + 24);
    uint32_t state = frame ? LoadU32(base, frame + 4) : 0;
    return state ? ao2::ue3::ObjectName(base, state) : std::string();
  }

  // The character rolls or vaults. Y then starts a vault inside the roll,
  // and the roll waits for its animation to end for good
  // (Suppression_Rolling_InRoll, AnimJustEnded).
  bool Maneuvering(const std::string &state)
  {
    return state.find("Rolling") != std::string::npos || state.find("Vault") != std::string::npos;
  }

  // A co-op move with the partner: a step jump (one lifts the other up a
  // wall) or a pull up (one pulls the other up a ledge).
  bool CoopMove(const std::string &state)
  {
    return state.rfind("StepJump", 0) == 0 || state.rfind("PullUp", 0) == 0;
  }

  // Where a character is shot at: its middle a little up, its head when
  // only that shows.
  Vec Body(const uint8_t *base, uint32_t pawn)
  {
    Vec v = LoadVec(base, pawn + kActorLocation);
    return {v.x, v.y, v.z + 15.0f};
  }

  // Where a depot to blow up is shot at: the middle of its collision
  // component's bounds (PrimitiveComponent.Bounds: Origin, BoxExtent,
  // SphereRadius) - an InterpActor's origin need not be on its mesh, and a
  // line to it passed by touching nothing (taken for "in sight": the bots
  // fired at the air). A little above the origin when there are none.
  // `extent` (out): the bounds' half height (0: none).
  Vec ObjectiveAim(const uint8_t *base, uint32_t actor, float *extent = nullptr)
  {
    static int32_t collision = -2, bounds = -1;
    if (collision == -2)
    {
      collision = ao2::ue3::PropertyOffset(base, "Actor", "CollisionComponent");
      bounds = ao2::ue3::PropertyOffset(base, "PrimitiveComponent", "Bounds");
      Logf("properties: Actor.CollisionComponent +%d, PrimitiveComponent.Bounds +%d\n", collision, bounds);
    }
    Vec v = LoadVec(base, actor + kActorLocation);
    uint32_t component = collision >= 0 ? LoadU32(base, actor + collision) : 0;
    if (component && bounds >= 0)
    {
      Vec origin = LoadVec(base, component + bounds);
      Vec box = LoadVec(base, component + bounds + 12);
      float radius = LoadF32(base, component + bounds + 24);
      if (radius > 1.0f && radius < 3000.0f && Dist3D(origin, v) < 3000.0f)
      {
        if (extent)
        {
          *extent = box.z;
        }
        return origin;
      }
    }
    if (extent)
    {
      *extent = 0.0f;
    }
    return {v.x, v.y, v.z + 40.0f};
  }

  // A depot the bot does not reach (on a roof, a floor up: the bots stood
  // under it seeing only the ceiling) is shot at from a point of the bot's
  // part of the network kShootReach of it a trace from it reaches it.
  // A few traces a step; the list again (the depot may move) when looked
  // through. -1 while none is found.
  constexpr int kVantageTraces = 6;
  constexpr uint32_t kVantageStep = 15;
  constexpr uint32_t kVantageAgain = 120;
  constexpr uint32_t kVantageBlind = 90; // there not seeing it this long: another point
  template <typename Bot>
  int Vantage(PPCContext &ctx, uint8_t *base, uint32_t pawn, Bot &bot, uint32_t actor, const Vec &me, uint32_t frame)
  {
    if (bot.vantage_for != actor)
    {
      bot.vantage_for = actor;
      bot.vantage = -1;
      bot.vantage_list.clear();
      bot.vantage_bad.clear();
      bot.vantage_next = 0;
      bot.vantage_at = 0;
    }
    if (bot.vantage >= 0 || frame < bot.vantage_next)
    {
      return bot.vantage;
    }
    bot.vantage_next = frame + kVantageStep;
    Vec aim = ObjectiveAim(base, actor);
    if (bot.vantage_next_i >= bot.vantage_list.size())
    {
      bot.vantage_list.clear();
      bot.vantage_next_i = 0;
      int part = PartOf(NearestPoint(me));
      std::vector<std::pair<float, int>> near;
      for (size_t i = 0; part >= 0 && i < g_nodes.size(); ++i)
      {
        const Vec &n = g_nodes[i].at;
        if (g_nodes[i].next.empty() || PartOf(int(i)) != part || Dist3D(n, aim) > kShootReach * 0.9f ||
            std::find(bot.vantage_bad.begin(), bot.vantage_bad.end(), int(i)) != bot.vantage_bad.end())
        {
          continue;
        }
        near.push_back({Dist2D(n, me), int(i)});
      }
      std::sort(near.begin(), near.end());
      for (auto &[d, i] : near)
      {
        bot.vantage_list.push_back(i);
      }
      if (bot.vantage_list.empty())
      {
        bot.vantage_next = frame + kVantageAgain;
        return -1;
      }
    }
    for (int k = 0; k < kVantageTraces && bot.vantage_next_i < bot.vantage_list.size(); ++k)
    {
      int i = bot.vantage_list[bot.vantage_next_i++];
      Vec from = g_nodes[i].at;
      from.z += 50.0f;
      uint32_t hit = TraceHit(ctx, base, pawn, from, aim);
      if (!hit || hit == actor)
      {
        bot.vantage = i;
        return i;
      }
    }
    if (bot.vantage_next_i >= bot.vantage_list.size())
    {
      bot.vantage_next = frame + kVantageAgain; // none: the list again later
    }
    return -1;
  }

  // The body first: a head shot every time was a cheat's aim.
  bool InSight(PPCContext &ctx, uint8_t *base, uint32_t pawn, uint32_t target, bool &body)
  {
    Vec eye = Head(base, pawn);
    for (bool b : {true, false})
    {
      Vec to = b ? Body(base, target) : Head(base, target);
      uint32_t hit = TraceHit(ctx, base, pawn, eye, to);
      if (!hit || hit == target)
      {
        body = b;
        return true;
      }
    }
    return false;
  }

  float SegmentDistance(const Vec &p, const Vec &a, const Vec &b)
  {
    Vec d{b.x - a.x, b.y - a.y, b.z - a.z};
    float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
    float t = len2 > 0 ? std::clamp(((p.x - a.x) * d.x + (p.y - a.y) * d.y + (p.z - a.z) * d.z) / len2, 0.0f, 1.0f)
                       : 0.0f;
    return Dist3D(p, {a.x + d.x * t, a.y + d.y * t, a.z + d.z * t});
  }

  // No hostage near the line of fire (the aim wanders, a hostage dragged
  // by an enemy is at its side) - but the one the target carries (shot in
  // the head over it) and the bot's own (on its shoulder, by its eyes).
  bool ClearShot(const Vec &eye, const Vec &at, uint32_t carried = 0, uint32_t own = 0)
  {
    for (const Character &c : g_characters)
    {
      if (c.civilian && c.pawn != carried && c.pawn != own && (SegmentDistance(c.at, eye, at) < kHostageMargin ||
                         SegmentDistance({c.at.x, c.at.y, c.at.z + 60.0f}, eye, at) < kHostageMargin))
      {
        return false;
      }
    }
    return true;
  }

  // Character and objective properties, looked up once.
  struct Props
  {
    int32_t partner = -1, drag = -1, carry = -1, wall_height = -1, vaulting = -1, step_volume = -1;
    int32_t locators = -1, current = -1, target = -1, type = -1;
    uint32_t team_class = 0;
  };
  const Props &Properties(const uint8_t *base)
  {
    static Props p;
    static bool looked = false;
    if (!looked)
    {
      looked = true;
      p.partner = ao2::ue3::PropertyOffset(base, "AO2CharacterNative", "Partner");
      p.drag = ao2::ue3::PropertyOffset(base, "AO2CharacterNative", "DragPartnerOtherCharacter");
      // A hostage is carried on the shoulder: FiremanCarryGameplayInfo
      // (SFiremanCarryGameplayInfo), +4 OtherCharacter - the hostage on the
      // carrier, the carrier on the hostage.
      p.carry = ao2::ue3::PropertyOffset(base, "AO2CharacterNative", "FiremanCarryGameplayInfo");
      Logf("properties: FiremanCarryGameplayInfo +%d\n", p.carry);
      p.wall_height = ao2::ue3::PropertyOffset(base, "AO2CharacterNative", "PullUpWallHeight");
      p.vaulting = ao2::ue3::PropertyOffset(base, "AO2CharacterNative", "ActiveVaultingVolume");
      p.step_volume = ao2::ue3::PropertyOffset(base, "AO2CharacterNative", "ActiveStepJumpVolume");
      Logf("properties: ActiveStepJumpVolume +%d\n", p.step_volume);
      p.locators = ao2::ue3::PropertyOffset(base, "AO2MPTeamInfo", "MPTeamObjectiveLocators");
      p.current = ao2::ue3::PropertyOffset(base, "AO2MPObjectiveLocatorList", "CurrentMPObjectiveLocators");
      p.target = ao2::ue3::PropertyOffset(base, "AO2HudMPObjectiveLocator", "ObjectiveTarget");
      p.type = ao2::ue3::PropertyOffset(base, "AO2HudMPObjectiveLocator", "eObjectiveType");
      p.team_class = ao2::ue3::FindClass(base, "AO2MPTeamInfo");
      Logf("properties: Partner +%d, DragPartnerOtherCharacter +%d, PullUpWallHeight +%d, ActiveVaultingVolume +%d, "
           "MPTeamObjectiveLocators +%d, CurrentMPObjectiveLocators +%d, ObjectiveTarget +%d, eObjectiveType +%d, "
           "AO2MPTeamInfo %08X\n",
           p.partner, p.drag, p.wall_height, p.vaulting, p.locators, p.current, p.target, p.type, p.team_class);
    }
    return p;
  }

  // The character one carries on its shoulder (a hostage) or drags (a
  // wounded partner) - on a hostage, who carries it; 0 for none.
  uint32_t Carried(const uint8_t *base, uint32_t pawn)
  {
    const Props &p = Properties(base);
    uint32_t other = p.carry >= 0 ? LoadU32(base, pawn + p.carry + 4) : 0;
    if (!other && p.drag >= 0)
    {
      other = LoadU32(base, pawn + p.drag);
    }
    return other && IsCharacter(base, other) ? other : 0;
  }

  // Who carries (drags) a hostage: 0 for none.
  uint32_t CarrierOf(const uint8_t *base, uint32_t hostage)
  {
    for (const Character &c : g_characters)
    {
      if (!c.civilian && Carried(base, c.pawn) == hostage)
      {
        return c.pawn;
      }
    }
    return 0;
  }

  // The objectives a team is shown (the HUD's markers: AO2MPSeqAct_
  // AttachObjectiveLocator adds an AO2HudMPObjectiveLocator to the team's
  // AO2MPTeamInfo.MPTeamObjectiveLocators; its ObjectiveTarget is the actor
  // marked, eObjectiveType an AO2MPObjectiveIconType.EObjectiveIconType).
  enum ObjectiveType : uint8_t
  {
    kObjAssassinate, // a character to kill (or, of the bot's own team, to guard)
    kObjHostage,     // a hostage to take (A by it; it is then dragged along)
    kObjDetonate,    // a depot or cache to blow up (shot at)
    kObjControl,     // a place to hold
    kObjBuyZone,
    kObjEnemyTeam,   // an enemy shown on the map
    kObjExtraction,  // where a hostage is taken
    kObjDefend,
    kObjWaypoint,
  };
  struct Objective
  {
    uint32_t actor;
    uint8_t type;
    Vec at;
  };

  // The buy zones (kObjBuyZone) are left out, or only they are kept.
  std::vector<Objective> TeamObjectives(const uint8_t *base, uint32_t team, bool buy_zones = false)
  {
    const Props &p = Properties(base);
    std::vector<Objective> list;
    if (!team || p.locators < 0 || p.current < 0 || p.target < 0 || p.type < 0 || !p.team_class ||
        !ao2::ue3::IsA(base, team, p.team_class))
    {
      return list;
    }
    uint32_t locators = LoadU32(base, team + p.locators);
    uint32_t data = locators ? LoadU32(base, locators + p.current) : 0;
    uint32_t n = locators ? LoadU32(base, locators + p.current + 4) : 0;
    for (uint32_t i = 0; data && i < n && i < 32; ++i)
    {
      uint32_t locator = LoadU32(base, data + i * 4);
      uint32_t actor = locator ? LoadU32(base, locator + p.target) : 0;
      if (!actor)
      {
        continue;
      }
      uint8_t type = base[locator + p.type];
      if ((type == kObjBuyZone) != buy_zones || type > kObjWaypoint)
      {
        continue;
      }
      if (IsCharacter(base, actor) && !Alive(base, actor))
      {
        continue; // killed
      }
      list.push_back({actor, type, LoadVec(base, actor + kActorLocation)});
    }
    return list;
  }

  // The extraction's volume: the occupied volume that is the marker; null
  // for none (a Trigger: TriggerCylinder).
  const Volume *ExtractionVolume(uint32_t marker_actor, const Vec &marker)
  {
    // Only the marker itself. Not one it is near or in: the occupied volumes
    // are zones of other uses (in Afghanistan the extraction Trigger_14
    // stands in one 4800 x 3900 wide, Trigger_9 on a roof above another).
    for (const Volume &v : g_occupied_volumes)
    {
      if (v.actor == marker_actor)
      {
        return &v;
      }
    }
    return nullptr;
  }

  // A Trigger's cylinder (Trigger.CylinderComponent: CollisionRadius,
  // CollisionHeight); false for none.
  bool TriggerCylinder(const uint8_t *base, uint32_t actor, float &radius, float &height)
  {
    static int32_t cylinder = -2, radius_off, height_off;
    static uint32_t trigger_class = 0;
    if (cylinder == -2)
    {
      cylinder = ao2::ue3::PropertyOffset(base, "Trigger", "CylinderComponent");
      radius_off = ao2::ue3::PropertyOffset(base, "CylinderComponent", "CollisionRadius");
      height_off = ao2::ue3::PropertyOffset(base, "CylinderComponent", "CollisionHeight");
      trigger_class = ao2::ue3::FindClass(base, "Trigger");
      Logf("Trigger.CylinderComponent +%d, CylinderComponent CollisionRadius +%d CollisionHeight +%d\n", cylinder,
           radius_off, height_off);
    }
    uint32_t c = cylinder >= 0 && radius_off >= 0 && height_off >= 0 && trigger_class &&
                         ao2::ue3::IsA(base, actor, trigger_class)
                     ? LoadU32(base, actor + cylinder)
                     : 0;
    radius = c ? LoadF32(base, c + radius_off) : 0.0f;
    height = c ? LoadF32(base, c + height_off) : 0.0f;
    return radius > 1.0f && radius < 5000.0f && height > 1.0f && height < 5000.0f;
  }

  // The volume counts the controller in (AO2MPPlayerOccupiedTriggerVolume.
  // arVolumeContains: the players in it, its event fires only for the first).
  bool Occupies(const uint8_t *base, uint32_t volume, uint32_t controller)
  {
    static int32_t contains = -2;
    if (contains == -2)
    {
      contains = ao2::ue3::PropertyOffset(base, "AO2MPPlayerOccupiedTriggerVolume", "arVolumeContains");
      Logf("AO2MPPlayerOccupiedTriggerVolume.arVolumeContains +%d\n", contains);
    }
    uint32_t data = contains >= 0 ? LoadU32(base, volume + contains) : 0;
    uint32_t n = data ? LoadU32(base, volume + contains + 4) : 0;
    for (uint32_t i = 0; i < n && i < 8; ++i)
    {
      if (LoadU32(base, data + i * 4) == controller)
      {
        return true;
      }
    }
    return false;
  }

  // `at` is in one of the extractions among `objectives` (its volume, its
  // trigger's cylinder, or near its marker).
  bool InExtraction(const uint8_t *base, const std::vector<Objective> &objectives, const Vec &at)
  {
    for (const Objective &e : objectives)
    {
      if (e.type != kObjExtraction)
      {
        continue;
      }
      float radius = 0, height = 0;
      if (const Volume *v = ExtractionVolume(e.actor, e.at))
      {
        if (at.x > v->box.lo.x && at.x < v->box.hi.x && at.y > v->box.lo.y && at.y < v->box.hi.y &&
            at.z > v->box.lo.z - 100.0f && at.z < v->box.hi.z + 100.0f)
        {
          return true;
        }
      }
      else if (TriggerCylinder(base, e.actor, radius, height))
      {
        if (Dist2D(at, e.at) < radius + 80.0f && std::fabs(at.z - e.at.z) < height + 100.0f)
        {
          return true;
        }
      }
      else if (Dist3D(at, e.at) < 200.0f)
      {
        return true;
      }
    }
    return false;
  }

  bool IsBotController(uint32_t controller)
  {
    for (uint32_t id = 0; id < 4; ++id)
    {
      if ((g_places & (1u << id)) && g_bots[id].controller == controller)
      {
        return true;
      }
    }
    return false;
  }

  const Character *FindCharacter(uint32_t pawn)
  {
    for (const Character &c : g_characters)
    {
      if (c.pawn == pawn)
      {
        return &c;
      }
    }
    return nullptr;
  }

  // ---- Combat: cover, grenades, the weapon for the distance.

  constexpr uint32_t kHurtFrames = 120;      // hit this long ago: under fire
  constexpr uint32_t kThreatFrames = 600;    // an enemy out of sight so long is no threat
  constexpr float kCoverSearch = 1000.0f;    // cover nodes this near are looked at
  constexpr float kCoverClimb = 120.0f;      // ... no higher or lower than this
  constexpr float kCoverFromThreat = 450.0f; // ... not this near the threat
  constexpr size_t kCoverTries = 6;          // ... the nearest so many checked with traces
  constexpr float kCoverFight = 1400.0f;     // a target in sight this far: fought from cover too
  constexpr float kCoverReach = 25.0f;       // there this near (farther off it the cover hides less)
  constexpr uint32_t kCoverWalkFrames = 360; // not there by then: left
  constexpr uint32_t kCoverRest = 300;       // left: none looked for so long (its task first)
  constexpr float kRetreatHealth = 0.35f;    // hurt this badly with no cover: back to its partner
  constexpr float kNadeMin = 500.0f;         // grenades thrown this far...
  constexpr float kNadeMax = 1600.0f;        // ... to this far
  constexpr float kNadeFriend = 500.0f;      // not with one of its team this near where it lands
  constexpr float kNadeRoll = 0.88f;         // it lands short and rolls on (AO2Grenade.FLIGHT_TO_ROLL_RATIO)
  constexpr float kNadeGroup = 400.0f;       // enemies this near each other: a grenade for them
  constexpr uint32_t kNadeApart = 180;       // the bots' grenades this many frames apart at least
  uint32_t g_last_nade = 0;                  // frame a bot last threw one

  struct CombatProps
  {
    int32_t health_max = -1, inventory = -1, slots = -1, good_min = -1, good_max = -1;
    int32_t nade_speed = -1, nade_pitch = -1, nade_projectile = -1;
    int32_t gravity = -1, default_gravity = -1, gravity_scale = -1;
    uint32_t weapon_class = 0, grenade_class = 0;
  };
  const CombatProps &Combat(const uint8_t *base)
  {
    static CombatProps p;
    static bool looked = false;
    if (!looked)
    {
      looked = true;
      p.health_max = ao2::ue3::PropertyOffset(base, "Pawn", "HealthMax");
      p.inventory = ao2::ue3::PropertyOffset(base, "Pawn", "InvManager");
      p.slots = ao2::ue3::PropertyOffset(base, "AO2InventoryManager", "ActiveAO2Weapons");
      p.good_min = ao2::ue3::PropertyOffset(base, "AO2Weapon", "aiMinGoodRange");
      p.good_max = ao2::ue3::PropertyOffset(base, "AO2Weapon", "aiMaxGoodRange");
      p.nade_speed = ao2::ue3::PropertyOffset(base, "AO2Grenade", "InitialProjSpeed");
      p.nade_pitch = ao2::ue3::PropertyOffset(base, "AO2Grenade", "InitialPitchOffset");
      p.nade_projectile = ao2::ue3::PropertyOffset(base, "AO2Grenade", "GrenadeSpawnedProjectile");
      p.gravity = ao2::ue3::PropertyOffset(base, "WorldInfo", "WorldGravityZ");
      p.default_gravity = ao2::ue3::PropertyOffset(base, "WorldInfo", "DefaultGravityZ");
      p.gravity_scale = ao2::ue3::PropertyOffset(base, "WorldInfo", "RBPhysicsGravityScaling");
      p.weapon_class = ao2::ue3::FindClass(base, "AO2Weapon");
      p.grenade_class = ao2::ue3::FindClass(base, "AO2Grenade");
      Logf("combat: Pawn.HealthMax +%d, InvManager +%d, AO2InventoryManager.ActiveAO2Weapons +%d, AO2Weapon "
           "aiMinGoodRange +%d aiMaxGoodRange +%d, AO2Grenade InitialProjSpeed +%d InitialPitchOffset +%d "
           "GrenadeSpawnedProjectile +%d, WorldInfo WorldGravityZ +%d DefaultGravityZ +%d RBPhysicsGravityScaling "
           "+%d, AO2Weapon %08X, AO2Grenade %08X\n",
           p.health_max, p.inventory, p.slots, p.good_min, p.good_max, p.nade_speed, p.nade_pitch,
           p.nade_projectile, p.gravity, p.default_gravity, p.gravity_scale, p.weapon_class, p.grenade_class);
    }
    return p;
  }

  // The character's inventory slots (AO2InventoryManager.ActiveAO2Weapons, by
  // EInventoryWeaponClass: primary, secondary, special, explosives, a
  // temporary one) and the slot of the weapon in its hands (-1: none of
  // them, a switch going on).
  struct Inventory
  {
    uint32_t slot[5] = {};
    int current = -1;
  };
  Inventory InventoryOf(const uint8_t *base, uint32_t pawn)
  {
    const CombatProps &p = Combat(base);
    Inventory inv;
    uint32_t manager = p.inventory >= 0 ? LoadU32(base, pawn + p.inventory) : 0;
    if (!manager || p.slots < 0 || !p.weapon_class)
    {
      return inv;
    }
    uint32_t held = LoadU32(base, pawn + kPawnWeapon);
    for (int i = 0; i < 5; ++i)
    {
      uint32_t w = LoadU32(base, manager + p.slots + i * 4);
      inv.slot[i] = w && ao2::ue3::IsA(base, w, p.weapon_class) ? w : 0;
      inv.current = inv.slot[i] && inv.slot[i] == held ? i : inv.current;
    }
    return inv;
  }

  bool Crouched(const uint8_t *base, uint32_t pawn)
  {
    static int32_t offset = -2;
    static uint32_t mask = 0;
    if (offset == -2 && !ao2::ue3::BoolProperty(base, "Pawn", "bIsCrouched", offset, mask))
    {
      offset = -1;
    }
    return offset >= 0 && (LoadU32(base, pawn + offset) & mask);
  }

  bool HoldsGrenade(const uint8_t *base, uint32_t pawn)
  {
    uint32_t held = LoadU32(base, pawn + kPawnWeapon);
    return held && Combat(base).grenade_class && ao2::ue3::IsA(base, held, Combat(base).grenade_class);
  }

  // The slot for a target `d` away: the weapon whose good range (the AI's
  // aiMinGoodRange - aiMaxGoodRange) it is in, or is nearest to; the
  // primary first, the pistol only with nothing else.
  int SlotFor(const uint8_t *base, const Inventory &inv, float d)
  {
    const CombatProps &p = Combat(base);
    int best = -1;
    float best_cost = 0;
    for (int s : {0, 2, 1})
    {
      uint32_t w = inv.slot[s];
      if (!w)
      {
        continue;
      }
      float lo = p.good_min >= 0 ? LoadF32(base, w + p.good_min) : 0.0f;
      float hi = p.good_max >= 0 ? LoadF32(base, w + p.good_max) : 0.0f;
      float cost = hi > lo && hi < 100000.0f ? std::max({0.0f, lo - d, d - hi}) : 1000.0f;
      cost += s == 1 ? 5000.0f : 0.0f; // a rifle near beats a pistol
      if (best < 0 || cost < best_cost - 50.0f)
      {
        best = s, best_cost = cost;
      }
    }
    return best;
  }

  // The world's gravity for a grenade (AO2Grenade.GetRigidBodyGravityVector:
  // RBPhysicsGravityScaling * GetGravityZ()), as a positive number.
  float Gravity(const uint8_t *base)
  {
    const CombatProps &p = Combat(base);
    uint32_t world = FindWorldInfo(base);
    float g = world && p.gravity >= 0 ? LoadF32(base, world + p.gravity) : 0.0f;
    if (g == 0 && world && p.default_gravity >= 0)
    {
      g = LoadF32(base, world + p.default_gravity);
    }
    float scale = world && p.gravity_scale >= 0 ? LoadF32(base, world + p.gravity_scale) : 1.0f;
    g = std::fabs(g * (scale > 0 && scale < 10 ? scale : 1.0f));
    return g > 50 && g < 5000 ? g : 520.0f;
  }

  // The pitch of the aim (rotation units) that throws a grenade from `from`
  // to `to`: thrown at `speed` that much above the aim (InitialPitchOffset,
  // degrees), the lower of the two curves. False when it cannot reach.
  bool ThrowPitch(float speed, float offset, float g, const Vec &from, const Vec &to, float &pitch)
  {
    float r = Dist2D(from, to), h = to.z - from.z;
    float v2 = speed * speed;
    float disc = v2 * v2 - g * (g * r * r + 2.0f * h * v2);
    if (r < 1.0f || disc < 0)
    {
      return false;
    }
    float theta = std::atan((v2 - std::sqrt(disc)) / (g * r));
    pitch = (theta * 57.29578f - offset) * kUnitsPerDegree;
    return true;
  }

  // A world surface (not a character) between two points.
  bool Blocked(PPCContext &ctx, uint8_t *base, uint32_t pawn, const Vec &a, const Vec &b, uint32_t target = 0)
  {
    uint32_t hit = TraceHit(ctx, base, pawn, a, b);
    return hit && hit != target && !IsCharacter(base, hit);
  }

  // Cover from `threat` (its eye at `eye`): of the cover nodes near the bot
  // (and near `near_to`, its partner, when it keeps by one), on its level,
  // not too near the threat, nor another bot's, the nearest (one towards
  // the threat costs more) where a crouching character's eyes (~35 above
  // its middle crouched, its middle ~30 lower) are out of the threat's
  // sight and one rising or leaning out (left or right) sees it.
  // -1 for none.
  struct CoverSearch
  {
    int near = 0, seen = 0, blind = 0; // nodes near, of the checked: seen crouching, no shot from
  };
  int FindCover(PPCContext &ctx, uint8_t *base, uint32_t pawn, uint32_t id, const Bot &bot, const Vec &at,
                uint32_t threat, const Vec &eye, const Vec *near_to, uint32_t frame, CoverSearch &why)
  {
    std::vector<std::pair<float, int>> candidates;
    float threat_d = Dist2D(at, eye);
    for (int i = 0; i < int(g_nodes.size()); ++i)
    {
      const Node &n = g_nodes[i];
      float d = Dist2D(at, n.at);
      if (!n.cover || d > kCoverSearch || std::fabs(n.at.z - at.z) > kCoverClimb ||
          Dist2D(eye, n.at) < kCoverFromThreat || (near_to && Dist2D(*near_to, n.at) > kCoverSearch) ||
          (i == bot.bad_cover && frame < bot.bad_until))
      {
        continue;
      }
      bool taken = false;
      for (uint32_t o = 0; o < 4; ++o)
      {
        taken |= o != id && (g_places & (1u << o)) && g_bots[o].cover >= 0 &&
                 Dist2D(g_nodes[g_bots[o].cover].at, n.at) < 100.0f;
      }
      if (!taken)
      {
        ++why.near;
        candidates.push_back({d + (Dist2D(eye, n.at) < threat_d - 200.0f ? 300.0f : 0.0f), i});
      }
    }
    std::sort(candidates.begin(), candidates.end());
    for (size_t k = 0; k < candidates.size() && k < kCoverTries; ++k)
    {
      const Vec &c = g_nodes[candidates[k].second].at;
      Vec body{eye.x, eye.y, eye.z - 45.0f};
      if (!Blocked(ctx, base, pawn, {c.x, c.y, c.z + 10.0f}, eye, threat))
      {
        ++why.seen;
        continue; // seen crouching
      }
      float a = std::atan2(eye.y - c.y, eye.x - c.x);
      float sx = -std::sin(a) * 70.0f, sy = std::cos(a) * 70.0f;
      Vec up{c.x, c.y, c.z + 55.0f}, mid{c.x, c.y, c.z + 35.0f};
      bool fires = !Blocked(ctx, base, pawn, up, body, threat);
      for (float s : {1.0f, -1.0f})
      {
        Vec side{c.x + sx * s, c.y + sy * s, c.z + 35.0f};
        fires = fires || (!Blocked(ctx, base, pawn, mid, side) && !Blocked(ctx, base, pawn, side, body, threat));
      }
      if (fires)
      {
        return candidates[k].second;
      }
      ++why.blind;
    }
    return -1;
  }

  // ---- Grenades on the ground: fled from.
  //
  // The game's AI flees a grenade once it has bounced and the character is
  // in its damage radius (AO2DangerousEntity_Grenade.CanHurtPawn); the bots
  // the same. The grenades are looked for among all objects every
  // kGrenadeScan frames (a lockstep frame: alike on every machine), read
  // each frame.
  constexpr uint32_t kGrenadeScan = 10;
  constexpr float kGrenadeMargin = 150.0f;   // fled this much beyond its damage radius
  constexpr float kGrenadeRadius = 450.0f;   // ... the radius when none is read
  constexpr float kGrenadeLanded = 250.0f;   // slower than this: on the ground (no bounce count read)
  constexpr uint32_t kFleeFrames = 75;       // runs from it so long at least
  constexpr uint32_t kGrenadeLife = 360;     // a grenade first seen longer ago does not go off
  struct Grenades
  {
    std::vector<uint32_t> list;
    uint32_t scanned = ~0u;
    uint32_t world = 0;
    uint32_t proj_class = 0, phys_class = 0;
    int32_t bounces = -2, radius = -2;
    std::unordered_map<uint32_t, bool> is_grenade; // by class: a grenade's (or a subclass)
    // Frame each grenade object was first seen; 0: there at the map's first
    // scan (lying on the level, it never goes off: in Somalia two such lay
    // in the bots' way and they fled them again and again).
    std::unordered_map<uint32_t, uint32_t> seen;
    bool first_scan = true;
    bool Is(const uint8_t *base, uint32_t object, uint32_t cls)
    {
      auto [it, added] = is_grenade.try_emplace(cls, false);
      if (added)
      {
        it->second = (proj_class && ao2::ue3::IsA(base, object, proj_class)) ||
                     (phys_class && ao2::ue3::IsA(base, object, phys_class));
      }
      return it->second;
    }
  };
  Grenades g_grenades;

  void ScanGrenades(const uint8_t *base, uint32_t frame)
  {
    Grenades &g = g_grenades;
    if (g.bounces == -2)
    {
      g.proj_class = ao2::ue3::FindClass(base, "AO2Proj_Grenade");
      g.phys_class = ao2::ue3::FindClass(base, "AO2PhysProj_Grenade");
      g.bounces = ao2::ue3::PropertyOffset(base, "AO2Proj_Grenade", "BounceCount");
      g.radius = ao2::ue3::PropertyOffset(base, "Projectile", "DamageRadius");
      Logf("grenades: AO2Proj_Grenade %08X BounceCount +%d, AO2PhysProj_Grenade %08X, Projectile.DamageRadius +%d\n",
           g.proj_class, g.bounces, g.phys_class, g.radius);
    }
    if (g.scanned != ~0u && frame - g.scanned < kGrenadeScan && g.world == g_nav_world)
    {
      return;
    }
    if (g.world != g_nav_world)
    {
      g.world = g_nav_world; // another map: its classes elsewhere
      g.is_grenade.clear();
      g.seen.clear();
      g.first_scan = true;
    }
    g.scanned = frame;
    g.list.clear();
    // Only a grenade that appeared lately (thrown) is a danger: one that has
    // lain longer than kGrenadeLife (a fuse is ~3 s) does not go off.
    std::unordered_map<uint32_t, uint32_t> seen;
    uint32_t count = ao2::ue3::ObjectCount(base);
    for (uint32_t i = 0; i < count; ++i)
    {
      uint32_t o = ao2::ue3::ObjectAt(base, i);
      uint32_t cls = o ? LoadU32(base, o + 52) : 0;
      if (cls && g.Is(base, o, cls) && ao2::ue3::ObjectName(base, o).rfind("Default__", 0) != 0)
      {
        auto it = g.seen.find(o);
        uint32_t first = it != g.seen.end() ? it->second : g.first_scan ? 0u : frame;
        seen[o] = first;
        if (first && frame - first <= kGrenadeLife && g.list.size() < 16)
        {
          g.list.push_back(o);
        }
      }
    }
    if (g.first_scan && !seen.empty())
    {
      Logf("grenades: %zu lie on the level (never fled from)\n", seen.size());
    }
    g.seen = std::move(seen);
    g.first_scan = false;
  }

  // The grenade on the ground nearest `at` whose blast reaches it (0: none);
  // where it is and how far its blast goes.
  uint32_t GrenadeNear(const uint8_t *base, const Vec &at, Vec &where, float &reach)
  {
    Grenades &g = g_grenades;
    uint32_t best = 0;
    float best_d = 0;
    for (uint32_t o : g.list)
    {
      uint32_t cls = LoadU32(base, o + 52);
      if (!cls || !g.Is(base, o, cls))
      {
        continue; // gone off (its memory another object's now)
      }
      Vec p = LoadVec(base, o + kActorLocation);
      float speed = std::hypot(LoadF32(base, o + kActorVelocity), LoadF32(base, o + kActorVelocity + 4),
                               LoadF32(base, o + kActorVelocity + 8));
      bool landed = (g.bounces >= 0 && cls == g.proj_class && int32_t(LoadU32(base, o + g.bounces)) > 0) ||
                    speed < kGrenadeLanded;
      float r = g.radius >= 0 ? LoadF32(base, o + g.radius) : 0.0f;
      r = (r > 50.0f && r < 3000.0f ? r : kGrenadeRadius) + kGrenadeMargin;
      float d = Dist3D(p, at);
      if (landed && d < r && std::fabs(p.z - at.z) < 300.0f && (!best || d < best_d))
      {
        best = o, best_d = d, where = p, reach = r;
      }
    }
    return best;
  }

  // ---- Buying (ao2_versus_bots_buy).
  //
  // A player buys at a buy station: A there (a context action) runs its
  // AO2MPSeqAct_BuyWeapons, AO2MPPlayerController.OnBuyWeapons sets
  // bMPBuyingWeapons (CurWeaponLoadoutIndex 0) and the controller's input
  // goes to the buy menu (control mode MultiplayerBuyWeapons, HandleBuyingWeapons):
  // the left stick picks one of the 8 kits (north 0, then clockwise to
  // north-west 7), A buys it (ChangeCash(-Price) the first time, a kit bought
  // once is taken again for nothing; EquipWeaponLoadout, MPReplenishWeapons;
  // the menu closes), X buys the next armor level (AO2MPGame.
  // MPArmorLevelPrice), Y ammo, B closes it. The bot opens the menu by
  // setting the flag itself where a player could buy (by a buy zone its team
  // is shown, or with none shown, near a spawn) and buys with the menu's own
  // input: the game takes the cash and gives the kit, alike on every machine.
  constexpr float kBuyReach = 350.0f;      // by a buy zone this near
  constexpr float kBuyStart = 1024.0f;     // ... or a spawn (AO2MPPlayerController.NearPlayerStart)
  constexpr float kBuyDetour = 3000.0f;    // a buy zone this near is gone to for a purchase
  constexpr uint32_t kBuyDetourFrames = 1200; // ... given up after so long
  constexpr uint32_t kBuyAgain = 900;      // looked at again after a purchase (or none) so long
  constexpr float kBuyFightNear = 1500.0f; // not with an enemy in sight this near
  struct BuyProps
  {
    int32_t loadouts = -1, bought = -1, equipped = -1, current = -1, stats = -1, cash = -1;
    int32_t price = -1, stride = 0, armor = -1, armor_price = -1, game = -1, flag = -1;
    uint32_t flag_mask = 0;
  };
  const BuyProps &Buying(const uint8_t *base)
  {
    static BuyProps p;
    static bool looked = false;
    if (!looked)
    {
      looked = true;
      p.loadouts = ao2::ue3::PropertyOffset(base, "AO2MPPlayerController", "WeaponLoadouts");
      p.bought = ao2::ue3::PropertyOffset(base, "AO2MPPlayerController", "WeaponLoadoutsBought");
      p.equipped = ao2::ue3::PropertyOffset(base, "AO2MPPlayerController", "EquippedWeaponLoadoutIndex");
      p.current = ao2::ue3::PropertyOffset(base, "AO2MPPlayerController", "CurWeaponLoadoutIndex");
      p.stats = ao2::ue3::PropertyOffset(base, "AO2MPPlayerController", "MPStats");
      p.cash = ao2::ue3::PropertyOffset(base, "MultiplayerStats", "MultiplayerCash");
      p.price = ao2::ue3::PropertyOffset(base, "WeaponLoadout", "Price");
      // The kits' stride: past the field that ends farthest (the order of
      // the fields in memory is not the config's: FourthReserveSize is not
      // the last). Strings take 12 bytes, the rest 4.
      static const char *const kStrings[] = {"Name", "PrimaryWeapon", "SecondWeapon", "ThirdWeapon"};
      static const char *const kWords[] = {
          "Price",           "ReloadCost",       "DamagePrimary",     "DamageSecond",  "DamageThird",
          "PrimaryClipSize", "PrimaryReserveSize", "SecondClipSize",  "SecondReserveSize", "ThirdClipSize",
          "ThirdReserveSize", "FourthClipSize",  "FourthReserveSize", "StaticPrimary", "StaticSecondary",
          "StaticThird"};
      int32_t end = 0;
      for (const char *f : kStrings)
      {
        int32_t o = ao2::ue3::PropertyOffset(base, "WeaponLoadout", f);
        end = o >= 0 ? std::max(end, o + 12) : end;
      }
      for (const char *f : kWords)
      {
        int32_t o = ao2::ue3::PropertyOffset(base, "WeaponLoadout", f);
        end = o >= 0 ? std::max(end, o + 4) : end;
      }
      p.stride = (end + 3) & ~3;
      p.armor = ao2::ue3::PropertyOffset(base, "AO2CharacterNative", "MPArmorLevel");
      p.armor_price = ao2::ue3::PropertyOffset(base, "AO2MPGame", "MPArmorLevelPrice");
      p.game = ao2::ue3::PropertyOffset(base, "WorldInfo", "Game");
      if (!ao2::ue3::BoolProperty(base, "AO2PlayerControllerNative", "bMPBuyingWeapons", p.flag, p.flag_mask))
      {
        p.flag = -1;
      }
      Logf("buying: AO2MPPlayerController WeaponLoadouts +%d WeaponLoadoutsBought +%d EquippedWeaponLoadoutIndex "
           "+%d CurWeaponLoadoutIndex +%d MPStats +%d (MultiplayerCash +%d), WeaponLoadout Price +%d stride %d, "
           "MPArmorLevel +%d, AO2MPGame.MPArmorLevelPrice +%d, WorldInfo.Game +%d, bMPBuyingWeapons +%d mask %08X\n",
           p.loadouts, p.bought, p.equipped, p.current, p.stats, p.cash, p.price, p.stride, p.armor, p.armor_price,
           p.game, p.flag, p.flag_mask);
    }
    return p;
  }
  bool CanBuy(const BuyProps &p)
  {
    return p.loadouts >= 0 && p.bought >= 0 && p.equipped >= 0 && p.current >= 0 && p.stats >= 0 && p.cash >= 0 &&
           p.price >= 0 && p.flag >= 0;
  }
  bool BuyMenuOpen(const uint8_t *base, const BuyProps &p, uint32_t controller)
  {
    return p.flag >= 0 && (LoadU32(base, controller + p.flag) & p.flag_mask);
  }
  void SetBuyMenu(uint8_t *base, const BuyProps &p, uint32_t controller, bool open)
  {
    uint32_t v = LoadU32(base, controller + p.flag);
    StoreU32(base, controller + p.flag, open ? v | p.flag_mask : v & ~p.flag_mask);
    StoreU32(base, controller + p.current, open ? 0u : uint32_t(-1));
  }
  int32_t Cash(const uint8_t *base, const BuyProps &p, uint32_t controller)
  {
    return int32_t(LoadU32(base, controller + p.stats + p.cash));
  }
  // The kits' stride, found from their prices once (0: not yet, -1: none
  // fits): the struct's size is not reflected and its fields' offsets did
  // not give it (80, then 204: kit 3's price read 15). The stride at which
  // the 8 prices are whole thousands, rising, kit 7's above 0 (the config's:
  // 0, 0, 65000 ... 475000).
  int32_t g_kit_stride = 0;
  int32_t KitStride(const uint8_t *base, const BuyProps &p, uint32_t controller)
  {
    if (g_kit_stride)
    {
      return g_kit_stride;
    }
    uint32_t data = LoadU32(base, controller + p.loadouts);
    uint32_t n = LoadU32(base, controller + p.loadouts + 4);
    if (!data || n < 8)
    {
      return 0;
    }
    g_kit_stride = -1;
    bool priced = false; // any price there yet (the kits are filled in after the controller is made)
    for (int32_t s = p.price + 4; s <= 512 && g_kit_stride < 0; s += 4)
    {
      int32_t last = 0;
      bool fits = true;
      for (int k = 0; k < 8 && fits; ++k)
      {
        int32_t price = int32_t(LoadU32(base, data + k * s + p.price));
        fits = price >= last && price < 100000000 && price % 1000 == 0;
        last = price;
        priced |= price > 0;
      }
      g_kit_stride = fits && last > 0 ? s : -1;
    }
    if (g_kit_stride < 0 && !priced)
    {
      g_kit_stride = 0; // no prices yet: looked at again (read too early it stayed -1 for good)
      return 0;
    }
    Logf("buying: kit stride %d (from the fields: %d), prices", g_kit_stride, p.stride);
    for (int k = 0; k < 8 && g_kit_stride > 0; ++k)
    {
      Logf(" %d", int32_t(LoadU32(base, data + k * g_kit_stride + p.price)));
    }
    Logf("\n");
    return g_kit_stride;
  }

  // A kit's price, -1 for none.
  int32_t KitPrice(const uint8_t *base, const BuyProps &p, uint32_t controller, int kit)
  {
    uint32_t data = LoadU32(base, controller + p.loadouts);
    uint32_t n = LoadU32(base, controller + p.loadouts + 4);
    int32_t stride = KitStride(base, p, controller);
    if (!data || kit < 0 || kit >= 8 || uint32_t(kit) >= n || stride <= 0)
    {
      return -1;
    }
    int32_t price = int32_t(LoadU32(base, data + kit * stride + p.price));
    return price >= 0 && price < 100000000 ? price : -1;
  }
  bool KitOwned(const uint8_t *base, const BuyProps &p, uint32_t controller, int kit)
  {
    return KitPrice(base, p, controller, kit) == 0 || LoadU32(base, controller + p.bought + kit * 4) != 0;
  }
  // The kit to take: of those it owns or may pay for (at most `budget`), the
  // dearest (the dearer, the better its weapons); -1 for none.
  int ChooseKit(const uint8_t *base, const BuyProps &p, uint32_t controller, int32_t budget, uint32_t failed)
  {
    // The prices read must look like prices (the config's are in thousands):
    // read at a wrong stride kit 7 cost 13.
    static bool bad_logged = false;
    for (int k = 0; k < 8; ++k)
    {
      int32_t price = KitPrice(base, p, controller, k);
      if (price < 0 || price % 1000 != 0)
      {
        if (!bad_logged)
        {
          bad_logged = true;
          Logf("buying: kit %d's price reads %d (stride %d?): no buying\n", k, price, p.stride);
        }
        return -1;
      }
    }
    int best = -1;
    int32_t best_price = -1;
    for (int k = 0; k < 8; ++k)
    {
      int32_t price = KitPrice(base, p, controller, k);
      if (!(failed & (1u << k)) && (KitOwned(base, p, controller, k) || price <= budget) && price > best_price)
      {
        best = k, best_price = price;
      }
    }
    return best;
  }
  // The next armor level's price for the character, -1 for none (at the top).
  int32_t ArmorPrice(const uint8_t *base, const BuyProps &p, uint32_t pawn)
  {
    uint32_t world = FindWorldInfo(base);
    uint32_t game = world && p.game >= 0 ? LoadU32(base, world + p.game) : 0;
    int32_t level = p.armor >= 0 ? int32_t(LoadU32(base, pawn + p.armor)) : 2;
    if (!game || p.armor_price < 0 || level < 0 || level >= 2)
    {
      return -1;
    }
    int32_t price = int32_t(LoadU32(base, game + p.armor_price + (level + 1) * 4));
    return price > 0 && price < 100000000 ? price : -1;
  }
  // The left stick to pick kit k in the menu (north 0, clockwise).
  void KitStick(int k, Input &in)
  {
    static const float dir[8][2] = {{0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}};
    in.lx = dir[k & 7][0] * 0.95f;
    in.ly = dir[k & 7][1] * 0.95f;
  }

  // What the bot is about, most urgent first.
  enum class Task
  {
    kHeal,      // its partner lies: to it, A by it
    kJoin,      // its partner waits for it to step jump / pull up: to it, A by it
    kGive,      // its partner, a player, waits by a step jump wall: to it, A by it (the bot lifts)
    kBuy,       // a kit or armor to buy: to a buy zone
    kFollow,    // its partner is a player: kept near it
    kObjective, // to the objective, A by it (a hostage), shoot it (a depot), or hold it
    kHunt,      // no objective: the nearest enemy
    kCover,     // under fire: to a cover node, fights from there
    kRetreat,   // badly hurt with no cover: back to its partner
  };
  const char *TaskName(Task t)
  {
    return t == Task::kHeal        ? "heal"
           : t == Task::kJoin      ? "join"
           : t == Task::kGive      ? "give"
           : t == Task::kBuy       ? "buy"
           : t == Task::kFollow    ? "follow"
           : t == Task::kObjective ? "objective"
           : t == Task::kCover     ? "cover"
           : t == Task::kRetreat   ? "retreat"
                                   : "hunt";
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
      bot.cover = -1;
      bot.nade_step = 0;
      if (frame >= bot.next_respawn)
      {
        bot.next_respawn = frame + 45;
        in.buttons |= kA;
      }
      return in;
    }
    Character me{pawn, controller, TeamOf(base, controller), false, LoadVec(base, pawn + kActorLocation),
                 DownState(base, pawn), false};
    if (me.down == 2)
    {
      // Lies still until healed, up again, or a respawn bought: once one can
      // be (kReviveFrames after it fell), with no partner up and near to
      // heal it, and its cash covering the price and kRespawnReserve more.
      bot.target = 0;
      bot.path.clear();
      bot.cover = -1;
      bot.nade_step = 0;
      bot.still = 0;
      bot.stuck_check = 0;
      bot.down_since = bot.down_since ? bot.down_since : frame;
      const Props &lying = Properties(base);
      uint32_t partner_pawn = lying.partner >= 0 ? LoadU32(base, pawn + lying.partner) : 0;
      const Character *partner = partner_pawn ? FindCharacter(partner_pawn) : nullptr;
      bool healer = partner && !partner->down && partner->team == me.team && Dist2D(partner->at, me.at) < kHealRange;
      const ao2::versus::VersusSettings &rules = g_settings;
      int32_t price = (rules.cost > 0 ? rules.cost : kGamePenalty) + rules.step * bot.respawns_paid;
      int32_t cash = Cash(base, Buying(base), controller);
      if (g_settings.buy && !healer && frame - bot.down_since > kReviveFrames && cash >= price + kRespawnReserve &&
          frame >= bot.next_respawn)
      {
        bot.next_respawn = frame + 45;
        in.buttons |= kA;
        if (frame - bot.down_since < kReviveFrames + 50)
        {
          Logf("frame %u: bot %u buys a respawn for %d (cash %d, %s)\n", frame, id, price, cash,
               !partner ? "no partner" : partner->down ? "its partner lies too" : "its partner is far");
        }
      }
      return in;
    }
    if (bot.down_since)
    {
      // Up again: bought (the cash fell by the price) or healed / on its own.
      int32_t price = (g_settings.cost > 0 ? g_settings.cost : kGamePenalty) + g_settings.step * bot.respawns_paid;
      int32_t cash = Cash(base, Buying(base), controller);
      if (bot.cash_down - cash >= price * 9 / 10)
      {
        ++bot.respawns_paid;
      }
      bot.down_since = 0;
    }
    bot.cash_down = Cash(base, Buying(base), controller);
    const Props &props = Properties(base);
    // Hit: its health fell.
    const CombatProps &combat = Combat(base);
    int32_t health = int32_t(LoadU32(base, pawn + kPawnHealth));
    if (bot.health_pawn != pawn)
    {
      bot.health_pawn = pawn;
      bot.health_top = 0;
      bot.last_health = health;
    }
    if (health < bot.last_health)
    {
      bot.hurt_at = frame;
    }
    bot.last_health = health;
    // Pawn.HealthMax is not in the reflection: full health is the most it had.
    bot.health_top = std::max(bot.health_top, health);
    int32_t health_max =
        combat.health_max >= 0 ? int32_t(LoadU32(base, pawn + combat.health_max)) : bot.health_top;
    float health_part = health_max > 0 ? std::clamp(float(health) / float(health_max), 0.0f, 1.0f) : 1.0f;
    bool hurt = bot.hurt_at && frame - bot.hurt_at < kHurtFrames;
    Inventory inventory = InventoryOf(base, pawn);
    if (bot.inventory_logged != pawn)
    {
      bot.inventory_logged = pawn;
      std::string list;
      for (int i = 0; i < 5; ++i)
      {
        uint32_t w = inventory.slot[i];
        char range[64] = "";
        if (w && combat.good_min >= 0 && combat.good_max >= 0)
        {
          std::snprintf(range, sizeof(range), " (%.0f-%.0f)", LoadF32(base, w + combat.good_min),
                        LoadF32(base, w + combat.good_max));
        }
        list += " " + std::to_string(i) + "=" + (w ? ao2::ue3::ObjectName(base, w) : std::string("-")) + range;
      }
      Logf("frame %u: bot %u health %d/%d, inventory:%s, holds slot %d\n", frame, id, health, health_max,
           list.c_str(), inventory.current);
    }
    std::erase_if(bot.barred, [&](const Bar &b) { return frame >= b.until; });
    std::string state = StateName(base, pawn);
    bool maneuvering = Maneuvering(state);
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
    if (g_settings.ammo)
    {
      InfiniteAmmo(base, pawn);
    }

    // On a ladder: the stick up or down (the way it took the ladder) until
    // it is off at the other end.
    if (state.rfind("ClimbLadder", 0) == 0)
    {
      if (!bot.on_ladder)
      {
        bot.on_ladder = true;
        Logf("frame %u: bot %u on a ladder at %.0f %.0f %.0f, %s\n", frame, id, me.at.x, me.at.y, me.at.z,
             bot.ladder_up ? "up" : "down");
      }
      bot.target = 0;
      bot.still = 0;
      bot.stuck_check = 0;
      bot.progress = false;
      bot.last_at = me.at;
      in.ly = bot.ladder_up ? 1.0f : -1.0f;
      return in;
    }
    if (bot.on_ladder)
    {
      bot.on_ladder = false;
      Logf("frame %u: bot %u off the ladder at %.0f %.0f %.0f\n", frame, id, me.at.x, me.at.y, me.at.z);
    }

    // In a step jump or a pull up with its partner: the stick let go while
    // the partner lifts it (held at the bottom, it reads as down: the move
    // is left), pushed up now and again once it is up and no longer rising
    // (at the top the jumper climbs over); B (out of it) only if it takes
    // very long.
    if (CoopMove(state))
    {
      if (!bot.coop_since)
      {
        bot.coop_since = frame;
        bot.coop_z0 = bot.coop_zmax = me.at.z;
        bot.coop_rose_at = frame;
      }
      if (me.at.z > bot.coop_zmax + 2.0f)
      {
        bot.coop_zmax = me.at.z;
        bot.coop_rose_at = frame;
      }
      if (state != bot.coop_state)
      {
        bot.coop_state = state;
        Logf("frame %u: bot %u in %s (%u frames into the move, %.0f up)\n", frame, id, state.c_str(),
             frame - bot.coop_since, me.at.z - bot.coop_z0);
      }
      bot.target = 0;
      bot.path.clear();
      bot.still = 0;
      bot.stuck_check = 0;
      bot.last_at = me.at;
      // The bot as the lifter (its offer to pull its partner up, below):
      // waiting at the ledge until the offer runs out, then the stick
      // pushed (that calls the wait off); in the move it pulls (stick up).
      bool lifter = state.find("Lifter") != std::string::npos;
      if (lifter)
      {
        bot.give_until = 0;
        bot.next_give = frame + kGiveWait;
      }
      if (lifter && state.find("PreCoop") != std::string::npos)
      {
        in.ly = frame >= bot.offer_until ? 1.0f : 0.0f;
        return in;
      }
      // Lifting, the stick sets how high (StepJumpLifter_DesiredHeightRatio,
      // smoothed): held up all the way (let go by turns it lowered the
      // jumper again and a high ledge was out of its reach). Lifted, the
      // stick up climbs over: held, let go a moment now and then (pushed
      // too early it may not take).
      bool up = lifter || (bot.coop_zmax - bot.coop_z0 > kCoopRise && frame - bot.coop_rose_at > kCoopTop);
      if (up && state.find("_Coop") != std::string::npos && (lifter || frame % 60 < 50))
      {
        in.ly = 1.0f;
      }
      if (frame - bot.coop_since > kCoopMoveFrames && frame % 30 < 2)
      {
        in.buttons |= kB;
      }
      return in;
    }
    if (bot.coop_since)
    {
      Logf("frame %u: bot %u out of the move after %u frames (%.0f up), now %s\n", frame, id,
           frame - bot.coop_since, me.at.z - bot.coop_z0, state.c_str());
      // Lifted up a wall: it offers to pull its partner up after it.
      bot.offer_pending = me.at.z - bot.coop_z0 > kCoopRise;
    }
    bot.coop_since = 0;
    bot.coop_state.clear();

    // Buying (see Buying): in the menu, nothing but its input - the stick
    // to the kit, A; the menu again for armor, X, then B.
    const BuyProps &buy = Buying(base);
    bool menu = BuyMenuOpen(base, buy, controller);
    if (bot.buy_step)
    {
      uint32_t t = frame - bot.buy_at;
      if (bot.buy_step == 1)
      {
        KitStick(bot.buy_kit, in);
        in.buttons |= t >= 10 && t < 12 ? kA : 0;
        if (t >= 18)
        {
          int32_t equipped = int32_t(LoadU32(base, controller + buy.equipped));
          Logf("frame %u: bot %u %s kit %d (%d), cash now %d\n", frame, id,
               equipped == bot.buy_kit ? "took" : "could not take (not tried again)", bot.buy_kit,
               KitPrice(base, buy, controller, bot.buy_kit), Cash(base, buy, controller));
          if (equipped != bot.buy_kit && bot.buy_kit >= 0)
          {
            bot.kit_failed |= 1u << bot.buy_kit;
          }
          if (menu)
          {
            SetBuyMenu(base, buy, controller, false);
          }
          bot.buy_kit = -1;
          bot.buy_step = bot.buy_armor ? 2 : 0;
          bot.buy_at = frame;
          if (bot.buy_armor)
          {
            SetBuyMenu(base, buy, controller, true);
          }
        }
      }
      else
      {
        int32_t level = buy.armor >= 0 ? int32_t(LoadU32(base, pawn + buy.armor)) : -1;
        in.buttons |= t >= 6 && t < 8 ? kX : t >= 16 && t < 18 ? kB : 0;
        if (t >= 24)
        {
          Logf("frame %u: bot %u armor level %d, cash now %d\n", frame, id, level, Cash(base, buy, controller));
          if (BuyMenuOpen(base, buy, controller))
          {
            SetBuyMenu(base, buy, controller, false);
          }
          bot.buy_step = 0;
          bot.buy_armor = false;
        }
      }
      if (!bot.buy_step)
      {
        bot.next_buy = frame + kBuyAgain;
        bot.buy_going = false;
      }
      bot.still = 0;
      bot.stuck_check = 0;
      bot.last_at = me.at;
      return in;
    }
    if (menu)
    {
      // Opened by an A at a buy station on its way: closed (in it, X - its
      // reload - buys armor and Y ammo).
      if (frame >= bot.menu_logged)
      {
        bot.menu_logged = frame + 600;
        Logf("frame %u: bot %u in a buy menu it did not open: closes it\n", frame, id);
      }
      in.buttons |= frame % 10 < 2 ? kB : 0;
      return in;
    }
    bool target_near = bot.target && Dist2D(me.at, LoadVec(base, bot.target + kActorLocation)) < kBuyFightNear;
    if (g_settings.buy && CanBuy(buy) && frame >= bot.next_buy && !Carried(base, pawn) &&
        (!bot.put_down_at || frame - bot.put_down_at > kPutDownBuy) &&
        !maneuvering && !target_near && (!bot.hurt_at || frame - bot.hurt_at > kHurtFrames))
    {
      int32_t cash = Cash(base, buy, controller);
      int32_t budget = int32_t(double(std::max(cash, 0)) * g_settings.share);
      int equipped = int32_t(LoadU32(base, controller + buy.equipped));
      int kit = ChooseKit(base, buy, controller, budget, bot.kit_failed);
      int32_t kit_cost = kit >= 0 && !KitOwned(base, buy, controller, kit) ? KitPrice(base, buy, controller, kit) : 0;
      bool want_kit = kit >= 0 && kit != equipped &&
                      KitPrice(base, buy, controller, kit) > KitPrice(base, buy, controller, equipped);
      int32_t armor_cost = ArmorPrice(base, buy, pawn);
      bool want_armor = armor_cost > 0 && armor_cost <= budget - (want_kit ? kit_cost : 0);
      if (!want_kit && !want_armor)
      {
        bot.next_buy = frame + kBuyAgain;
        bot.buy_going = false;
      }
      else
      {
        // Where: by a buy zone its team is shown; with none shown, near a spawn.
        std::vector<Objective> zones = TeamObjectives(base, me.team, true);
        const Objective *zone = nullptr;
        for (const Objective &z : zones)
        {
          if (!zone || Dist2D(me.at, z.at) < Dist2D(me.at, zone->at))
          {
            zone = &z;
          }
        }
        bool by = zone ? Dist2D(me.at, zone->at) < kBuyReach && std::fabs(me.at.z - zone->at.z) < 200.0f : false;
        for (const Vec &s : g_player_starts)
        {
          by |= !zone && Dist2D(me.at, s) < kBuyStart && std::fabs(me.at.z - s.z) < 200.0f;
        }
        if (by)
        {
          Logf("frame %u: bot %u buys %s%s%s (cash %d, budget %d, kit now %d)%s\n", frame, id,
               want_kit ? "kit " : "", want_kit ? std::to_string(kit).c_str() : "",
               want_armor ? (want_kit ? " and armor" : "armor") : "", cash, budget, equipped,
               zone ? " at a buy zone" : " near a spawn");
          bot.buy_kit = want_kit ? kit : -1;
          bot.buy_armor = want_armor;
          bot.buy_step = want_kit ? 1 : 2;
          bot.buy_at = frame;
          bot.buy_going = false;
          SetBuyMenu(base, buy, controller, true);
          bot.last_at = me.at;
          return in;
        }
        if (zone && Dist2D(me.at, zone->at) < kBuyDetour && !bot.buy_going)
        {
          bot.buy_going = true;
          bot.buy_going_until = frame + kBuyDetourFrames;
          bot.buy_zone = zone->at;
          Logf("frame %u: bot %u goes to a buy zone %.0f away (%s, cash %d)\n", frame, id, Dist2D(me.at, zone->at),
               ao2::ue3::ObjectName(base, zone->actor).c_str(), cash);
        }
        else if (!bot.buy_going)
        {
          bot.next_buy = frame + kBuyAgain / 3; // none near: later, maybe after a respawn
        }
      }
    }
    if (bot.buy_going && frame >= bot.buy_going_until)
    {
      bot.buy_going = false;
      bot.next_buy = frame + kBuyAgain;
    }
    float view_yaw = LoadF32(base, holder + kLookRotation + 4);
    Vec eye = Head(base, pawn);

    // The partner: the character's own (AO2CharacterNative.Partner, the one
    // it heals and does co-op moves with), else the nearest player of its team.
    uint32_t partner_pawn = props.partner >= 0 ? LoadU32(base, pawn + props.partner) : 0;
    const Character *partner = partner_pawn ? FindCharacter(partner_pawn) : nullptr;
    if (partner && partner->team != me.team && !bot.partner_logged)
    {
      bot.partner_logged = true;
      Logf("frame %u: bot %u: its partner %08X is of another team (%08X, its own %08X)\n", frame, id,
           partner->pawn, partner->team, me.team);
    }
    if (!partner || partner->team != me.team)
    {
      partner = nullptr;
      for (const Character &c : g_characters)
      {
        if (c.pawn != pawn && c.team && c.team == me.team && !c.ai &&
            (!partner || Dist2D(me.at, c.at) < Dist2D(me.at, partner->at)))
        {
          partner = &c;
        }
      }
    }

    // The task, and where it takes the bot.
    Task task = Task::kHunt;
    Vec goal_at;
    bool have_goal = false;
    float arrive = 0;        // there this near (2D)
    float arrive_z = 150.0f; // ... and this near in height
    uint32_t goal_actor = 0; // faced when there
    uint8_t objective_type = 0;
    // A hostage on its shoulder: no heal, no co-op move (A puts it down),
    // no B or Y when stuck (B too), no crouching.
    bool carrying = Carried(base, pawn) != 0;
    if (!carrying)
    {
      bot.extract_in_since = 0;
      bot.extract_leave = false;
    }
    if (partner && partner->down && Dist2D(me.at, partner->at) < kHealRange && !carrying)
    {
      task = Task::kHeal;
      goal_at = partner->at, have_goal = true, goal_actor = partner->pawn;
      arrive = kHealReach;
    }
    if (!have_goal && partner && Dist2D(me.at, partner->at) < kJoinRange && !carrying)
    {
      std::string partner_state = StateName(base, partner->pawn);
      if (partner_state.find("PreCoop_Lifter") != std::string::npos)
      {
        if (props.step_volume >= 0)
        {
          if (uint32_t volume = LoadU32(base, partner->pawn + props.step_volume))
          {
            bot.lift_volume = volume;
            bot.lift_seen = frame;
          }
        }
        task = Task::kJoin;
        goal_at = partner->at, have_goal = true, goal_actor = partner->pawn;
        arrive = kJoinReach;
        arrive_z = 45.0f;
        if (partner_state.rfind("PullUp", 0) == 0 && props.wall_height >= 0)
        {
          goal_at.z -= LoadF32(base, partner->pawn + props.wall_height); // below the ledge it pulls from
          arrive_z = 90.0f;
        }
        if (bot.join_partner != partner->pawn)
        {
          bot.join_partner = partner->pawn;
          bot.join_since = frame;
          Logf("frame %u: bot %u joins %08X (%s) at %.0f\n", frame, id, partner->pawn, partner_state.c_str(),
               Dist2D(me.at, partner->at));
        }
      }
    }
    if (task != Task::kJoin)
    {
      bot.join_partner = 0;
    }

    // A player's partner: after it was lifted up a wall and its partner is
    // still below, it waits by the ledge a while and offers to pull it up
    // (A: the context action makes it the lifter where a pull up can be
    // done); then it goes on. Else it keeps near its partner while that is
    // near: fights by it, heals it, joins its moves.
    bool player_partner = partner && partner->team == me.team && !IsBotController(partner->controller);
    if (bot.offer_pending)
    {
      bot.offer_pending = false;
      if (player_partner && partner->at.z < me.at.z - kOfferBelow)
      {
        bot.offer_since = frame;
        bot.offer_until = frame + kOfferFrames;
        Logf("frame %u: bot %u offers its partner a pull up (%.0f below)\n", frame, id, me.at.z - partner->at.z);
      }
    }
    if (frame < bot.offer_until && !have_goal)
    {
      if (!player_partner || std::fabs(partner->at.z - me.at.z) < kOfferBelow)
      {
        bot.offer_until = 0; // up too (or gone)
      }
      else
      {
        TurnTowards(base, holder, std::atan2(partner->at.y - me.at.y, partner->at.x - me.at.x) * kUnitsPerRadian,
                    -30.0f * kUnitsPerDegree, 0.0f);
        uint32_t since = frame - bot.offer_since;
        in.buttons |= since >= 10 && since % 60 < 3 ? kA : 0;
        bot.still = 0;
        bot.stuck_check = 0;
        bot.progress = false;
        bot.last_at = me.at;
        return in;
      }
    }
    // Its partner, a player, stands still a moment in a step jump volume (by
    // a wall it cannot climb alone) and does not lift itself: it waits to be
    // lifted. The first to press A there is the lifter (AO2Character.
    // SendingCommunication), so the bot goes to it and presses A in the
    // volume, then waits at the wall for it to join (the move, above). The
    // partner leaving the volume calls it off.
    // A partner that was the lifter in that volume a moment ago (its lift
    // ran out before the bot came: the bot was far) waits to lift again: the
    // bot comes and stands by it without A (pressed first, the bot was the
    // lifter and the player went up instead), and joins once it lifts.
    bool join_wait = false;
    if (player_partner && !have_goal && !carrying && props.step_volume >= 0 && !partner->down)
    {
      uint32_t volume = LoadU32(base, partner->pawn + props.step_volume);
      float speed = std::hypot(LoadF32(base, partner->pawn + kActorVelocity),
                               LoadF32(base, partner->pawn + kActorVelocity + 4));
      bool lifter = volume && volume == bot.lift_volume && frame - bot.lift_seen < kLiftMemory;
      // Waited by it kLiftPatience and it does not lift again (or the bot
      // got up the wall itself): it wants to be lifted now. One press of A
      // made the player the lifter, and the bot only waited by it for the
      // rest of kLiftMemory, never lifting it.
      bool by_it = Dist3D(me.at, partner->at) < kGiveRange;
      if (lifter && (me.at.z - partner->at.z > kOfferBelow ||
                     (bot.join_wait_since && frame - bot.join_wait_since >= kLiftPatience && speed < 20.0f)))
      {
        Logf("frame %u: bot %u: its partner %08X does not lift again in %s: it is lifted\n", frame, id, partner->pawn,
             ao2::ue3::ObjectName(base, volume).c_str());
        bot.lift_volume = 0;
        lifter = false;
      }
      bool beside = Dist2D(me.at, partner->at) < kGiveReach * 1.5f; // counted from when it got there
      bot.join_wait_since = lifter && beside ? (bot.join_wait_since ? bot.join_wait_since : frame) : 0;
      if (lifter && by_it)
      {
        if (bot.give_until)
        {
          bot.give_until = 0;
          bot.next_give = frame + kGiveWait;
        }
        if (!bot.join_waiting)
        {
          bot.join_waiting = true;
          Logf("frame %u: bot %u waits by its partner %08X to be lifted (it was the lifter in %s)\n", frame, id,
               partner->pawn, ao2::ue3::ObjectName(base, volume).c_str());
        }
        task = Task::kJoin;
        goal_at = partner->at, have_goal = true, goal_actor = partner->pawn;
        arrive = kJoinReach;
        arrive_z = 45.0f;
        join_wait = true;
      }
      bot.join_waiting = join_wait;
      bool waits = volume && !lifter && speed < 20.0f && !CoopMove(StateName(base, partner->pawn)) &&
                   Dist3D(me.at, partner->at) < kGiveRange;
      bot.give_since = waits ? (bot.give_since ? bot.give_since : frame) : 0;
      if (waits && !bot.give_until && frame >= bot.next_give && frame - bot.give_since >= kGiveAsk)
      {
        bot.give_until = frame + kGiveFrames;
        Logf("frame %u: bot %u lifts its partner %08X waiting in step jump volume %s, %.0f away\n", frame, id,
             partner->pawn, ao2::ue3::ObjectName(base, volume).c_str(), Dist3D(me.at, partner->at));
      }
      if (bot.give_until && (!volume || frame >= bot.give_until))
      {
        Logf("frame %u: bot %u lifts its partner no more: %s\n", frame, id,
             !volume ? "it left the volume" : "it took too long");
        bot.give_until = 0;
        bot.next_give = frame + kGiveWait;
      }
      if (bot.give_until)
      {
        task = Task::kGive;
        goal_at = partner->at, have_goal = true, goal_actor = partner->pawn;
        arrive = kGiveReach;
        // There only in the volume itself (A elsewhere does nothing).
        if (!LoadU32(base, pawn + props.step_volume))
        {
          arrive = 60.0f;
        }
      }
    }
    else
    {
      bot.give_since = 0;
      bot.give_until = 0;
    }
    // A purchase it can afford with a buy zone near: there first (it buys
    // once by it, see Buying).
    if (!have_goal && bot.buy_going && !carrying)
    {
      task = Task::kBuy;
      goal_at = bot.buy_zone, have_goal = true, goal_actor = 0;
      arrive = kBuyReach * 0.6f;
    }
    // A hostage no one carries yet, or one the bot carries: the bot's own
    // job (picked up, carried to the extraction), not following.
    bool hostage_job = Carried(base, pawn) != 0;
    if (player_partner && !have_goal && !hostage_job)
    {
      for (const Objective &o : TeamObjectives(base, me.team))
      {
        hostage_job |= o.type == kObjHostage && !CarrierOf(base, o.actor);
      }
    }
    // Following only while its partner is near: one gone far off (out of
    // kFollowLeave) leaves the bot to the objectives until it is back in
    // kFollowRadius.
    if (player_partner)
    {
      float d = Dist3D(me.at, partner->at);
      if (bot.following ? d > kFollowLeave : d < kFollowRadius)
      {
        bot.following = !bot.following;
        Logf("frame %u: bot %u %s its partner, %.0f away\n", frame, id, bot.following ? "follows" : "leaves", d);
      }
    }
    if (!have_goal && player_partner && bot.following && !hostage_job)
    {
      // Ahead of it where it runs (as far as a wall lets): the bot runs
      // with it, at most kFollowLead + kFollowReach in front.
      task = Task::kFollow;
      goal_at = partner->at, have_goal = true, goal_actor = partner->pawn;
      arrive = kFollowReach;
      float vx = LoadF32(base, partner->pawn + kActorVelocity), vy = LoadF32(base, partner->pawn + kActorVelocity + 4);
      float speed = std::hypot(vx, vy);
      if (speed > kFollowMoving)
      {
        for (float lead : {kFollowLead, kFollowLead * 0.5f})
        {
          Vec ahead{partner->at.x + vx / speed * lead, partner->at.y + vy / speed * lead, partner->at.z};
          if (ProbeClear(ctx, base, pawn, partner->at, ahead))
          {
            goal_at = ahead;
            break;
          }
        }
      }
    }
    uint32_t objective_target = 0; // an enemy the objective is to kill
    if (!have_goal)
    {
      uint32_t dragging = Carried(base, pawn);
      const Objective *best = nullptr;
      std::vector<Objective> objectives = TeamObjectives(base, me.team);
      // Carrying a hostage: the extraction only (another objective nearer
      // turned it away from it). Not carrying one: the extraction is gone to
      // only with one of its side carrying one (kept by it).
      bool to_extraction = false, side_carries = false;
      for (const Objective &o : objectives)
      {
        to_extraction |= o.type == kObjExtraction;
      }
      for (const Character &c : g_characters)
      {
        const Character *h = c.pawn != pawn && !c.civilian && c.team == me.team ? FindCharacter(Carried(base, c.pawn))
                                                                                  : nullptr;
        side_carries |= h && h->civilian;
      }
      for (const Objective &o : objectives)
      {
        if (o.type == kObjExtraction ? !dragging && !side_carries : dragging && to_extraction)
        {
          continue;
        }
        const Character *c = IsCharacter(base, o.actor) ? FindCharacter(o.actor) : nullptr;
        if (o.type == kObjHostage && !CarrierOf(base, o.actor) && InExtraction(base, objectives, o.at))
        {
          continue; // delivered: left where it lies
        }
        if (o.type == kObjHostage)
        {
          // Taken already: by this bot, or by one of its side. Carried by
          // an enemy: the carrier is fought (below).
          uint32_t carrier = CarrierOf(base, o.actor);
          const Character *by = carrier ? FindCharacter(carrier) : nullptr;
          if (dragging || (c && Carried(base, o.actor)) ||
              (carrier && (!by || !Enemy(me, *by))))
          {
            continue;
          }
        }
        if ((o.type == kObjAssassinate || o.type == kObjEnemyTeam) && (!c || c->down == 2))
        {
          continue;
        }
        if (o.type == kObjEnemyTeam && c && !Enemy(me, *c))
        {
          continue;
        }
        if (!best || Dist3D(me.at, o.at) < Dist3D(me.at, best->at))
        {
          best = &o;
        }
      }
      if (best)
      {
        task = Task::kObjective;
        goal_at = best->at, have_goal = true, goal_actor = best->actor;
        objective_type = best->type;
        const Character *c = IsCharacter(base, best->actor) ? FindCharacter(best->actor) : nullptr;
        switch (best->type)
        {
        case kObjHostage:
          if (uint32_t carrier = CarrierOf(base, best->actor))
          {
            // An enemy carries it off: not picked up (A by the carrier was a
            // blow that cannot be struck), the carrier is shot.
            objective_target = carrier;
            goal_at = LoadVec(base, carrier + kActorLocation), goal_actor = carrier;
            objective_type = kObjAssassinate;
            arrive = bot.target == carrier ? kNearFight : 0.0f;
          }
          else
          {
            arrive = kUseReach;
          }
          break;
        case kObjDetonate:
          // A cache or a depot: destroyed by fire, shot at from where the
          // bot sees it (bot.objective_seen, looked at with the scan), in
          // height too (one a floor up is seen from below at a distance).
          // Not seen: gone to a point it is seen from (Vantage), else to
          // it.
          if (bot.objective_seen == best->actor)
          {
            arrive = kShootReach;
            arrive_z = kShootReach;
            bot.vantage_at = 0;
          }
          else if (int v = Vantage(ctx, base, pawn, bot, best->actor, me.at, frame); v >= 0)
          {
            if (bot.vantage_logged != v)
            {
              bot.vantage_logged = v;
              Logf("frame %u: bot %u shoots at %08X from point %d (%.0f %.0f %.0f), %.0f from it\n", frame, id,
                   best->actor, v, g_nodes[v].at.x, g_nodes[v].at.y, g_nodes[v].at.z,
                   Dist3D(g_nodes[v].at, best->at));
            }
            goal_at = g_nodes[v].at;
            if (Dist2D(me.at, goal_at) < 80.0f)
            {
              // There and not seeing it after all: another point.
              bot.vantage_at = bot.vantage_at ? bot.vantage_at : frame;
              if (frame - bot.vantage_at > kVantageBlind)
              {
                bot.vantage_bad.push_back(v);
                bot.vantage = -1;
                bot.vantage_at = 0;
              }
            }
            else
            {
              bot.vantage_at = 0;
            }
            arrive = 0.0f; // walked to the point, stopping when it sees the depot
          }
          else
          {
            arrive = 0.0f;
          }
          break;
        case kObjAssassinate:
        case kObjEnemyTeam:
          if (c && Enemy(me, *c))
          {
            objective_target = c->pawn;
            arrive = bot.target == c->pawn ? kNearFight : 0.0f; // near it in sight: fought as a target
          }
          else
          {
            arrive = kHoldReach; // one of its own: guarded
          }
          break;
        case kObjControl:
        case kObjDefend:
          arrive = kHoldReach;
          break;
        case kObjExtraction:
          if (!dragging)
          {
            arrive = kHoldReach; // one of its side carries a hostage there: kept near
            break;
          }
          {
            // Into the middle of its volume (the marker may stand at its
            // edge): the event fires as the bot steps in. Still carrying
            // the hostage a while in it (stepped in before the extraction was
            // open, or with another player in it already: no event), out of
            // it and in again.
            // The extraction is an occupied volume (in it: arVolumeContains)
            // or a Trigger (in its cylinder), else the marker's point.
            const Volume *v = ExtractionVolume(best->actor, best->at);
            Vec mid = best->at;
            float radius = 0, height = 0;
            bool cylinder = !v && TriggerCylinder(base, best->actor, radius, height);
            if (v)
            {
              mid = {(v->box.lo.x + v->box.hi.x) * 0.5f, (v->box.lo.y + v->box.hi.y) * 0.5f,
                     std::clamp(best->at.z, v->box.lo.z, v->box.hi.z)};
            }
            bool inside = v          ? Occupies(base, v->actor, controller)
                          : cylinder ? Dist2D(me.at, mid) < radius + 45.0f && std::fabs(me.at.z - mid.z) < height + 60.0f
                                     : Dist2D(me.at, mid) < 100.0f;
            if (bot.extract_logged != best->actor)
            {
              bot.extract_logged = best->actor;
              Logf("frame %u: bot %u takes %08X to the extraction %s (%s) at %.0f %.0f %.0f%s\n", frame, id,
                   dragging, ao2::ue3::ObjectName(base, best->actor).c_str(),
                   v ? ao2::ue3::ObjectName(base, v->actor).c_str() : cylinder ? "trigger" : "a point", mid.x, mid.y,
                   mid.z,
                   cylinder ? (", radius " + std::to_string(int(radius)) + " height " + std::to_string(int(height))).c_str()
                            : "");
            }
            if (inside && !bot.extract_in_since)
            {
              bot.extract_in_since = frame;
              Logf("frame %u: bot %u in the extraction (%s) with %08X\n", frame, id,
                   v ? ao2::ue3::ObjectName(base, v->actor).c_str() : "no volume", dragging);
            }
            else if (!inside)
            {
              bot.extract_in_since = 0;
            }
            if (inside && !bot.extract_leave && frame - bot.extract_in_since > kExtractWait)
            {
              bot.extract_leave = true;
              bot.extract_leave_at = frame;
              bot.extract_out_since = 0;
              // Out just past its edge the way the bot is from its middle.
              float a = std::atan2(me.at.y - mid.y, me.at.x - mid.x);
              float cx = std::cos(a), cy = std::sin(a);
              float edge = cylinder ? radius : 100.0f;
              if (v)
              {
                float hx = (v->box.hi.x - v->box.lo.x) * 0.5f, hy = (v->box.hi.y - v->box.lo.y) * 0.5f;
                edge = std::min(std::fabs(cx) > 0.01f ? hx / std::fabs(cx) : 1e9f,
                                std::fabs(cy) > 0.01f ? hy / std::fabs(cy) : 1e9f);
              }
              bot.extract_out = {mid.x + cx * (edge + 250.0f), mid.y + cy * (edge + 250.0f), me.at.z};
              Logf("frame %u: bot %u still carries %08X %u frames in the extraction: out of it and in again\n", frame,
                   id, dragging, frame - bot.extract_in_since);
            }
            if (bot.extract_leave)
            {
              bot.extract_out_since = inside ? 0 : bot.extract_out_since ? bot.extract_out_since : frame;
              if ((bot.extract_out_since && frame - bot.extract_out_since > kExtractOut) ||
                  frame - bot.extract_leave_at > kExtractLeaveFrames)
              {
                bot.extract_leave = false;
              }
            }
            goal_at = bot.extract_leave ? bot.extract_out : mid;
            if (cylinder && !bot.extract_leave)
            {
              // Its middle is up in the air (half its height): the goal is
              // at the bot's height within it, on its level.
              goal_at.z = std::clamp(me.at.z, mid.z - height, mid.z + height);
            }
            goal_actor = 0;
            arrive = bot.extract_leave ? 80.0f : 60.0f;
          }
          break;
        default:
          arrive = kGoReach;
          break;
        }
        if (bot.objective != best->actor)
        {
          bot.objective = best->actor;
          int its = NearestPoint(best->at), mine = NearestPoint(me.at);
          Logf("frame %u: bot %u objective %u %08X (%s) at %.0f %.0f %.0f, %.0f away (%zu shown); network point %d "
               "part %d, the bot's %d part %d\n",
               frame, id, best->type, best->actor, ao2::ue3::ObjectName(base, best->actor).c_str(), best->at.x,
               best->at.y, best->at.z, Dist2D(me.at, best->at), objectives.size(), its, PartOf(its), mine,
               PartOf(mine));
        }
      }
      else
      {
        bot.objective = 0;
      }
    }

    // Target: the objective's enemy when it is in sight, else the nearest
    // enemy in range in sight.
    if (frame >= bot.next_scan)
    {
      bot.next_scan = frame + kScanFrames;
      float range = float(g_settings.range);
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
      if (objective_target && InSight(ctx, base, pawn, objective_target, body))
      {
        found = objective_target;
      }
      if (!found && keep && InSight(ctx, base, pawn, bot.target, body))
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
      // A depot to destroy: in sight from here (a bullet's line reaches it).
      bot.objective_seen = 0;
      if (task == Task::kObjective && objective_type == kObjDetonate &&
          Dist3D(me.at, LoadVec(base, goal_actor + kActorLocation)) < kShootReach * 1.2f)
      {
        float half = 0.0f;
        Vec middle = ObjectiveAim(base, goal_actor, &half);
        uint32_t clear_hit = ~0u;
        Vec clear_at = middle;
        for (float k : {0.0f, 0.5f, -0.4f, 0.8f})
        {
          Vec at{middle.x, middle.y, middle.z + half * k};
          uint32_t hit = TraceHit(ctx, base, pawn, eye, at);
          if (hit == goal_actor)
          {
            bot.objective_seen = goal_actor;
            bot.objective_aim = at;
            break;
          }
          if (!hit && clear_hit == ~0u)
          {
            clear_hit = 0;
            clear_at = at;
          }
          if (!half)
          {
            break;
          }
        }
        if (!bot.objective_seen && clear_hit == 0)
        {
          bot.objective_seen = goal_actor;
          bot.objective_aim = clear_at;
        }
        bool hits = bot.objective_seen && clear_hit != 0;
        if (bot.objective_seen && (bot.depot_logged != goal_actor || bot.depot_hits != hits))
        {
          bot.depot_logged = goal_actor;
          bot.depot_hits = hits;
          Logf("frame %u: bot %u aims at %08X (%s) at %.0f %.0f %.0f (origin %.0f %.0f %.0f, bounds half height "
               "%.0f): %s\n",
               frame, id, goal_actor, ao2::ue3::ObjectName(base, goal_actor).c_str(), bot.objective_aim.x,
               bot.objective_aim.y, bot.objective_aim.z, LoadVec(base, goal_actor + kActorLocation).x,
               LoadVec(base, goal_actor + kActorLocation).y, LoadVec(base, goal_actor + kActorLocation).z, half,
               hits ? "a bullet's line hits it" : "a bullet's line touches nothing there");
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
    // The threat: the target while in sight, kept a while after (where it was last seen).
    if (bot.target)
    {
      bot.threat = bot.target;
      bot.threat_seen = frame;
      bot.threat_at = LoadVec(base, bot.target + kActorLocation);
    }
    if (bot.threat && (!Alive(base, bot.threat) || DownState(base, bot.threat) == 2 ||
                       frame - bot.threat_seen > kThreatFrames))
    {
      bot.threat = 0;
    }
    // Hit: by whom (Pawn.LastHitBy, the controller that last hurt it). The
    // one shooting it, in sight, is fought at once (before the nearest);
    // out of sight it is the threat (cover is taken from it).
    const Character *shooter = nullptr;
    if (bot.hurt_at == frame)
    {
      static int32_t hit_by = -2;
      if (hit_by == -2)
      {
        hit_by = ao2::ue3::PropertyOffset(base, "Pawn", "LastHitBy");
        Logf("Pawn.LastHitBy +%d\n", hit_by);
      }
      uint32_t by_controller = hit_by >= 0 ? LoadU32(base, pawn + hit_by) : 0;
      uint32_t by_pawn = by_controller ? LoadU32(base, by_controller + kControllerPawn) : 0;
      shooter = by_pawn ? FindCharacter(by_pawn) : nullptr;
      shooter = shooter && Enemy(me, *shooter) ? shooter : nullptr;
    }
    if (shooter && shooter->pawn != bot.target)
    {
      bool body = false;
      if (InSight(ctx, base, pawn, shooter->pawn, body))
      {
        Logf("frame %u: bot %u hit by %08X (%s) at %.0f: fights it\n", frame, id, shooter->pawn,
             ao2::ue3::ObjectName(base, shooter->pawn).c_str(), Dist2D(me.at, shooter->at));
        bot.target = shooter->pawn;
        bot.target_since = frame;
        bot.target_body = body;
      }
      if (!bot.target || bot.target == shooter->pawn)
      {
        bot.threat = shooter->pawn;
        bot.threat_seen = frame;
        bot.threat_at = shooter->at;
      }
    }
    if (!bot.threat && bot.hurt_at == frame)
    {
      // Hit by one it does not know: the nearest enemy in range.
      const Character *by = nullptr;
      for (const Character &c : g_characters)
      {
        if (Enemy(me, c) && Dist2D(me.at, c.at) < float(g_settings.range) &&
            (!by || Dist2D(me.at, c.at) < Dist2D(me.at, by->at)))
        {
          by = &c;
        }
      }
      if (by)
      {
        bot.threat = by->pawn;
        bot.threat_seen = frame;
        bot.threat_at = by->at;
      }
    }

    // Hunting: towards the target, else the nearest enemy anywhere.
    if (!have_goal)
    {
      const Character *enemy = nullptr;
      for (const Character &c : g_characters)
      {
        if (bot.target ? c.pawn == bot.target : Enemy(me, c) && (!enemy || Dist2D(me.at, c.at) < Dist2D(me.at, enemy->at)))
        {
          enemy = &c;
        }
      }
      if (enemy)
      {
        goal_at = enemy->at, have_goal = true;
        arrive = bot.target ? kNearFight : 0.0f;
      }
    }
    // Cover: under fire (hit lately or badly hurt) with an enemy about, or
    // in a fight at long range - not on its way to heal or join its
    // partner, nor with a hostage to take or carry off unless badly hurt.
    // It goes to cover the enemy does not see it at, and fights from there
    // (below). Left when the enemy is gone, after a while once it is well,
    // when the enemy sees it there (another is looked for), or when its
    // partner, a player, goes on.
    if (uint32_t now = Carried(base, pawn); now != bot.carried)
    {
      Logf("frame %u: bot %u %s %08X, in %s\n", frame, id, now ? "carries" : "puts down", now ? now : bot.carried,
           state.c_str());
      if (!now && state.find("Falling") != std::string::npos && bot.path_at > 0 && bot.path_at < bot.path.size())
      {
        // Fell with it: the link it walked (and the one before, the fall
        // may have begun there) is no way for a carrier.
        for (size_t k = bot.path_at; k >= 1 && k + 2 > bot.path_at; --k)
        {
          g_carry_falls.insert(LinkKey(bot.path[k - 1], bot.path[k]));
        }
        Logf("frame %u: bot %u fell carrying it: link %d -> %d left out of carriers' paths\n", frame, id,
             bot.path[bot.path_at - 1], bot.path[bot.path_at]);
        bot.force_path = true;
      }
      if (!now)
      {
        bot.put_down_at = frame;
        Logf("frame %u: bot %u put it down with health %d, buttons held %X, task %s, %.0f from its goal\n", frame, id,
             int32_t(LoadU32(base, pawn + kPawnHealth)), bot.held, TaskName(task), have_goal ? Dist2D(me.at, goal_at) : -1.0f);
      }
      bot.carried = now;
      bot.carry_start = now ? frame : 0;
    }
    // Taking it up the character stands a moment (GetIntoPosition, the
    // weapon holstered): not stuck. Counted, the link it was to walk was
    // barred at every take up, and the way to the extraction went round.
    if (carrying && frame - bot.carry_start < kCarryStart)
    {
      bot.progress = false;
      bot.still = 0;
      bot.stuck_check = 0;
    }
    bool can_cover = task != Task::kHeal && task != Task::kJoin && task != Task::kGive && !carrying &&
                     !(task == Task::kObjective && (objective_type == kObjHostage || objective_type == kObjExtraction) &&
                       health_part > kRetreatHealth);
    Vec threat_eye = bot.threat ? Head(base, bot.threat) : Vec{};
    bool want_cover = can_cover && bot.threat &&
                      (hurt || health_part < 0.6f || (bot.target && Dist2D(me.at, bot.threat_at) > kCoverFight));
    if (bot.cover >= 0)
    {
      const char *why = nullptr;
      if (!can_cover)
      {
        why = "it has to go";
      }
      else if (!bot.threat && !hurt)
      {
        why = "no enemy about";
      }
      else if (bot.cover_arrived && frame >= bot.cover_until && health_part > 0.7f)
      {
        why = "it stayed long enough";
      }
      else if (!bot.cover_arrived && frame - bot.cover_since > kCoverWalkFrames)
      {
        why = "it did not get there";
      }
      else if (bot.exposed >= 2)
      {
        why = "the enemy sees it there";
        bot.bad_cover = bot.cover;
        bot.bad_until = frame + 900;
      }
      else if (player_partner && bot.following && Dist2D(partner->at, g_nodes[bot.cover].at) > kFollowLeave)
      {
        why = "its partner goes on";
      }
      if (why)
      {
        Logf("frame %u: bot %u leaves cover %d: %s\n", frame, id, bot.cover, why);
        if (bot.cover_arrived)
        {
          bot.press_at = frame + 1;
          bot.press = kB; // out of cover
        }
        bot.next_cover = frame + (bot.exposed >= 2 ? 0 : kCoverRest);
        bot.cover = -1;
        bot.exposed = 0;
        bot.path.clear();
        bot.progress = false;
      }
    }
    if (bot.cover < 0 && want_cover && frame >= bot.next_cover)
    {
      bot.next_cover = frame + 30;
      Vec partner_at = player_partner && bot.following ? partner->at : Vec{};
      CoverSearch why;
      int c = FindCover(ctx, base, pawn, id, bot, me.at, bot.threat, threat_eye,
                        player_partner && bot.following ? &partner_at : nullptr, frame, why);
      if (c < 0 && frame >= bot.next_cover_log)
      {
        bot.next_cover_log = frame + 300;
        Logf("frame %u: bot %u finds no cover from %08X %.0f away: %d nodes near, %d checked seen crouching, %d "
             "with no shot\n",
             frame, id, bot.threat, Dist2D(me.at, threat_eye), why.near, why.seen, why.blind);
      }
      if (c >= 0)
      {
        bot.cover = c;
        bot.cover_since = frame;
        bot.cover_arrived = 0;
        bot.exposed = 0;
        bot.path.clear();
        bot.progress = false;
        Logf("frame %u: bot %u takes cover at %d (%.0f %.0f %.0f), %.0f away, from %08X %.0f away (health %.0f%%%s)\n",
             frame, id, c, g_nodes[c].at.x, g_nodes[c].at.y, g_nodes[c].at.z, Dist2D(me.at, g_nodes[c].at),
             bot.threat, Dist2D(me.at, threat_eye), health_part * 100.0f, hurt ? ", hit" : "");
      }
    }
    if (bot.cover >= 0)
    {
      task = Task::kCover;
      goal_at = g_nodes[bot.cover].at, have_goal = true, goal_actor = 0;
      arrive = kCoverReach;
      arrive_z = 80.0f;
    }
    else if (want_cover && health_part < kRetreatHealth && partner && !partner->down &&
             Dist2D(me.at, partner->at) > 500.0f)
    {
      task = Task::kRetreat; // badly hurt, no cover: back to its partner
      goal_at = partner->at, have_goal = true, goal_actor = 0;
      arrive = 300.0f;
    }
    float goal_d = have_goal ? Dist2D(me.at, goal_at) : 0.0f;
    bool there = have_goal && goal_d < arrive && std::fabs(me.at.z - goal_at.z) < arrive_z;
    // By a partner to join or lift it stays there a little farther off too
    // (at the edge, by turns there pressing A and away aiming, it did
    // neither). To join, only within the game's join position
    // (AO2Character.IsInStepJumpJoinPosition: under 110 away, 50 in
    // height): it stood 120-150 off pressing A while the lifter walked to
    // the wall (Lifter_Positioning), and the lift began at the third try.
    bool coop_task = task == Task::kJoin || task == Task::kGive;
    bool join_reach = task != Task::kJoin || arrive_z > kJoinNearZ ||
                      (goal_d < kJoinNear && std::fabs(me.at.z - goal_at.z) < kJoinNearZ);
    if (coop_task && bot.coop_there && join_reach && goal_d < arrive + 50.0f &&
        std::fabs(me.at.z - goal_at.z) < arrive_z + 30.0f)
    {
      there = true;
    }
    bot.coop_there = coop_task && there;
    // A charge (a solid mesh) or a hostage is used from its side: its middle
    // may be farther than kUseReach. Held still against it near enough, the
    // bot is there until it walks off (not a wall to back off from).
    bool use_goal = task == Task::kObjective && objective_type == kObjHostage;
    if (use_goal && goal_actor == bot.used && goal_d < kUseLeave)
    {
      there = true;
    }
    else if (use_goal && goal_d < kUseFar && bot.still >= kPressedFrames)
    {
      there = true;
      bot.used = goal_actor;
      Logf("frame %u: bot %u stands against %08X (%s), %.0f from its middle: uses it from there\n", frame, id,
           goal_actor, ao2::ue3::ObjectName(base, goal_actor).c_str(), goal_d);
    }
    if (bot.used && (!use_goal || goal_actor != bot.used || goal_d >= kUseLeave))
    {
      bot.used = 0;
    }

    // There: A by a partner that lies (now and again: the heal starts with a
    // press, its mini game takes presses), by a partner waiting to lift or
    // pull it (a moment after it stands by it, without aiming: the partner
    // is ready only for a character that is not), by a hostage. Facing it,
    // LT let go.
    // No way to the goal (up a wall, down a drop too high): at the point
    // nearest to it the bot can reach it holds - fights from there, walks
    // into no wall - until the goal moves.
    bool hold = have_goal && !there && bot.path_partial && !bot.path.empty() &&
                Dist2D(bot.path_goal, goal_at) <= kPathMoved &&
                Dist2D(me.at, g_nodes[bot.path.back()].at) < kWaypoint * (bot.holding ? 2.5f : 1.5f) &&
                std::fabs(me.at.z - g_nodes[bot.path.back()].at.z) < 150.0f;
    // ... unless the goal is near on its level: the network may just not
    // reach it (no floor points by an extraction in Afghanistan: the bot
    // held 580 short of it with the hostage). Then straight at it, walking
    // round walls by probes, till that makes no progress (held after all).
    if (bot.direct && (!have_goal || there || !bot.path_partial || Dist2D(bot.path_goal, goal_at) > kPathMoved ||
                       goal_d > kDirectNear * 1.3f))
    {
      bot.direct = false;
    }
    if (hold && !bot.direct && frame >= bot.direct_off_until && goal_d < kDirectNear &&
        std::fabs(me.at.z - goal_at.z) < kOffLevel)
    {
      bot.direct = true;
      Logf("frame %u: bot %u no way to its goal %.0f %.0f %.0f, %.0f away: straight at it\n", frame, id,
           goal_at.x, goal_at.y, goal_at.z, goal_d);
    }
    hold = hold && !bot.direct;
    if (hold && !bot.holding)
    {
      Logf("frame %u: bot %u holds at %.0f %.0f %.0f: no way to its goal %.0f %.0f %.0f\n", frame, id, me.at.x,
           me.at.y, me.at.z, goal_at.x, goal_at.y, goal_at.z);
    }
    bot.holding = hold;
    bool use = there && (task == Task::kHeal || coop_task || use_goal);
    if (use)
    {
      Vec a = goal_actor ? LoadVec(base, goal_actor + kActorLocation) : goal_at;
      TurnTowards(base, holder, std::atan2(a.y - me.at.y, a.x - me.at.x) * kUnitsPerRadian,
                  task == Task::kHeal ? -20.0f * kUnitsPerDegree : 0.0f, 0.0f);
      if (task == Task::kJoin)
      {
        in.buttons |= !join_wait && frame - bot.join_since > 30 && frame % 40 < 3 ? kA : 0;
      }
      else if (task == Task::kGive)
      {
        // A (the bot is the lifter), then it waits at the wall (the move).
        if (frame % 40 < 3)
        {
          in.buttons |= kA;
          bot.offer_until = frame + kGiveWait;
        }
      }
      else if (use_goal)
      {
        // A hostage: one press, then the time to take it up (another press
        // meanwhile put it down again; it is carried ~5 frames after the
        // press, so a press that did nothing is tried again after
        // kTakeUpRetry). Not while the bot still falls or gets up: a press
        // then does nothing (after a fall it took 3 presses, 300 frames).
        bool settling = state.find("Falling") != std::string::npos || state.find("Unconscious") != std::string::npos ||
                        frame - bot.put_down_at < 20;
        if (frame >= bot.next_use && !settling)
        {
          bot.next_use = frame + kTakeUpRetry;
          Logf("frame %u: bot %u takes up %08X (%s), %.0f away, in %s\n", frame, id, goal_actor,
               ao2::ue3::ObjectName(base, goal_actor).c_str(), goal_d, state.c_str());
        }
        in.buttons |= !settling && frame - (bot.next_use - kTakeUpRetry) < 3 ? kA : 0;
      }
      else
      {
        in.buttons |= frame % 12 < 6 ? kA : 0;
      }
      if (frame >= bot.next_log)
      {
        bot.next_log = frame + 150;
        Logf("frame %u: bot %u %s by %08X (%s)%s\n", frame, id, TaskName(task), goal_actor,
             ao2::ue3::ObjectName(base, goal_actor).c_str(),
             task == Task::kHeal ? (partner->down == 2 ? ", it lies" : ", it is wounded") : "");
      }
      bot.still = 0;
      bot.stuck_check = 0;
      bot.progress = false;
      bot.last_at = me.at;
      return in;
    }

    // In cover: ducked down (no LT: the character crouches behind it) and up
    // (LT: it rises or leans out and aims) by turns; down at once when hit
    // while up. Down, it checks the threat does not see it (or hit it).
    bool in_cover = task == Task::kCover && there;
    if (task == Task::kCover && !there && bot.cover_arrived && goal_d > 150.0f)
    {
      bot.cover_arrived = 0; // pushed off it: back
    }
    bool hiding = false;
    if (in_cover)
    {
      if (!bot.cover_arrived)
      {
        bot.cover_arrived = frame;
        bot.cover_until = frame + 600 + uint32_t((Noise(frame, id + 29) + 1.0f) * 150.0f);
        bot.peek_until = 0;
        Logf("frame %u: bot %u in cover %d after %u frames\n", frame, id, bot.cover, frame - bot.cover_since);
      }
      auto cycle = [&](uint32_t hide) {
        bot.hide_since = frame;
        bot.hide_until = frame + hide;
        bot.peek_until = bot.hide_until + 60 + uint32_t((Noise(frame, id + 37) + 1.0f) * 40.0f);
      };
      if (frame >= bot.peek_until)
      {
        cycle(45 + uint32_t((Noise(frame, id + 31) + 1.0f) * 30.0f));
      }
      else if (frame >= bot.hide_until + 15 && bot.hurt_at == frame)
      {
        cycle(60);
      }
      hiding = frame < bot.hide_until;
      if (hiding && bot.threat && frame - bot.hide_since >= 25 &&
          (Crouched(base, pawn) || frame - bot.hide_since >= 60))
      {
        if (bot.hurt_at == frame)
        {
          ++bot.exposed;
        }
        else if (frame >= bot.next_expose_check)
        {
          bot.next_expose_check = frame + 20;
          bot.exposed = Blocked(ctx, base, pawn, eye, threat_eye, bot.threat) ? 0 : bot.exposed + 1;
          if (bot.exposed)
          {
            static int32_t cover_ratio = -2, crouch_ratio;
            if (cover_ratio == -2)
            {
              cover_ratio = ao2::ue3::PropertyOffset(base, "AO2CharacterNative", "CoverRatio");
              crouch_ratio = ao2::ue3::PropertyOffset(base, "AO2CharacterNative", "AnimStandToCrouchRatio");
            }
            Logf("frame %u: bot %u seen in cover %d (%u frames down): state %s, cover ratio %.2f, crouch %.2f, eye "
                 "%.0f above it, %.0f from the node\n",
                 frame, id, bot.cover, frame - bot.hide_since, state.c_str(),
                 cover_ratio >= 0 ? LoadF32(base, pawn + cover_ratio) : -1.0f,
                 crouch_ratio >= 0 ? LoadF32(base, pawn + crouch_ratio) : -1.0f, eye.z - me.at.z, goal_d);
          }
        }
      }
    }

    // Aim and fire at the target: LT once it is near or the bot stands
    // (aiming, a character walks slowly; farther on the way it fires from
    // the hip). The aim wanders a few degrees, slowly; it fires in bursts,
    // after a moment's look, and not with a hostage in the way.
    // With no enemy in sight, a depot to destroy in sight is the target.
    // Dragging a hostage it aims (LT) at any target: it fires with its free
    // hand.
    float target_d = bot.target ? Dist2D(me.at, LoadVec(base, bot.target + kActorLocation)) : 0.0f;
    bool shoot_objective = !bot.target && there && task == Task::kObjective && objective_type == kObjDetonate &&
                           bot.objective_seen == goal_actor;
    bool dragging = carrying;
    // Not on its way to join or lift its partner: aiming, the partner's
    // move does not take it (and it stopped by turns to fire).
    if ((bot.target || shoot_objective) && !bot.nade_step && !coop_task)
    {
      // A target carrying a hostage is shot in the head, over the hostage.
      uint32_t carried = bot.target ? Carried(base, bot.target) : 0;
      uint32_t own_hostage = Carried(base, pawn);
      Vec at = shoot_objective                ? bot.objective_aim
               : bot.target_body && !carried ? Body(base, bot.target)
                                              : Head(base, bot.target);
      if (shoot_objective)
      {
        target_d = goal_d;
        if (frame >= bot.next_log)
        {
          bot.next_log = frame + 150;
          Logf("frame %u: bot %u shoots at %08X (%s) %.0f away\n", frame, id, goal_actor,
               ao2::ue3::ObjectName(base, goal_actor).c_str(), goal_d);
        }
      }
      float miss = float(g_settings.miss) * kUnitsPerDegree * (shoot_objective ? 0.35f : 1.0f);
      uint32_t step = frame / kWanderFrames;
      float k = float(frame % kWanderFrames) / float(kWanderFrames);
      auto wander = [&](uint32_t seed) {
        return Noise(step, seed) * (1.0f - k) + Noise(step + 1, seed) * k;
      };
      float dx = at.x - eye.x, dy = at.y - eye.y, dz = at.z - eye.z;
      float want_yaw = std::atan2(dy, dx) * kUnitsPerRadian + miss * wander(id * 2);
      float want_pitch = std::atan2(dz, std::hypot(dx, dy)) * kUnitsPerRadian + miss * wander(id * 2 + 1);
      float tolerance = std::max(3.0f * kUnitsPerDegree, std::atan2(30.0f, std::hypot(dx, dy)) * kUnitsPerRadian);
      bool on = TurnTowards(base, holder, want_yaw, want_pitch, tolerance);
      bool moving_on = have_goal && !there && task != Task::kHunt;
      // Aiming slows the walk: not while catching up with its partner.
      // Nor running to cover or back to it.
      bool catching_up =
          moving_on && (task == Task::kFollow || task == Task::kCover || task == Task::kRetreat);
      if (!catching_up && (!moving_on || target_d < kAimRange || dragging))
      {
        in.buttons |= kLT;
      }
      bool burst = (frame + id * 13) % kBurstFrames < kBurstFrames * 3 / 5;
      bool looked = shoot_objective ||
                    frame - bot.target_since >= uint32_t(g_settings.reaction);
      if (on && burst && looked && frame % 9 < 6 && ClearShot(eye, at, carried, own_hostage))
      {
        in.buttons |= kRT; // pulled 6 frames of 9: single shot weapons fire again too
      }
    }
    if (in_cover && bot.threat && !bot.target && !bot.nade_step)
    {
      // The enemy out of sight: aimed where it was last seen, up or down.
      Vec at{bot.threat_at.x, bot.threat_at.y, bot.threat_at.z + 40.0f};
      float dx = at.x - eye.x, dy = at.y - eye.y;
      TurnTowards(base, holder, std::atan2(dy, dx) * kUnitsPerRadian,
                  std::atan2(at.z - eye.z, std::hypot(dx, dy)) * kUnitsPerRadian, 0.0f);
      in.buttons |= kLT;
    }
    if (hiding)
    {
      in.buttons &= ~(kLT | kRT);
    }

    // Moving: along a path to the goal while not there; there (or hunting
    // near the target) sideways, now this way, now that, round the goal and
    // not into a wall.
    bool walking = have_goal && !there && !hold;
    float speed = bot.target && task == Task::kHunt ? 0.6f : 1.0f;
    if (in_cover)
    {
      // Against it a moment (towards the threat: the cover is between):
      // the character takes it; then still.
      if (bot.threat && frame - bot.cover_arrived < 20)
      {
        Steer(me.at, bot.threat_at, view_yaw, 0.5f, in);
      }
      bot.stuck_check = 0;
      bot.progress = false;
    }
    else if (!walking && bot.target)
    {
      if (frame >= bot.next_strafe)
      {
        bot.next_strafe = frame + kProbeFrames;
        int side = (Noise(frame / 50, id + 17) < 0 ? -1 : 1) * bot.strafe_flip;
        float yaw = view_yaw / kUnitsPerRadian;
        bot.strafe_side = 0;
        for (int s : {side, -side})
        {
          float a = yaw + float(s) * 1.5707964f;
          if (ProbeClear(ctx, base, pawn, me.at, {me.at.x + std::cos(a) * 150.0f, me.at.y + std::sin(a) * 150.0f, me.at.z}))
          {
            bot.strafe_side = s;
            break;
          }
        }
      }
      in.lx = 0.7f * float(bot.strafe_side);
      if (have_goal && arrive > 0 && goal_d > arrive * 0.8f && task != Task::kHunt)
      {
        Steer(me.at, goal_at, view_yaw, 0.7f, in); // back towards what it holds
      }
      bot.stuck_check = 0;
      bot.progress = false;
    }

    if (walking)
    {
      bool repathed = false;
      // The path is kept while it leads to the goal: found again now and
      // then only for a goal that moved (from where the bot stands another
      // first point may be nearest, and a path from each in turn walks it to
      // and fro), after it backed off a wall, or for a goal moved far.
      bool due = frame >= bot.next_path && (bot.force_path || Dist2D(bot.path_goal, goal_at) > kPathMoved);
      if (bot.path.empty() || due || Dist2D(bot.path_goal, goal_at) > 600.0f)
      {
        bot.force_path = false;
        bot.next_path = frame + kPathFrames;
        bot.path = FindPath(ctx, base, pawn, me.at, goal_at, bot.barred, &bot.path_partial, carrying);
        bot.path_at = 0;
        bot.path_goal = goal_at;
        repathed = true;
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
        int got = bot.path[bot.path_at - 1];
        if (std::find(bot.reached.begin(), bot.reached.end(), got) == bot.reached.end())
        {
          // Somewhere new: counted afresh. Not one it got to before (to and
          // fro between two points, each the last, is no progress).
          bot.reached.push_back(got);
          bot.progress = false;
        }
      }
      // Straight at the goal only near it on its level (not under a roof it is on).
      bool on_path = !bot.direct && bot.path_at < bot.path.size() &&
                     (goal_d > 400.0f || std::fabs(me.at.z - goal_at.z) > kOffLevel);
      Vec to = on_path ? g_nodes[bot.path[bot.path_at]].at : goal_at;
      int from_node = on_path && bot.path_at > 0 ? bot.path[bot.path_at - 1] : -1;
      // A link round a corner: to its point beside the line first.
      if (from_node >= 0)
      {
        uint64_t key = LinkKey(from_node, bot.path[bot.path_at]);
        auto via = g_via.find(key);
        if (bot.via_link != key)
        {
          bot.via_link = key;
          bot.via_passed = via == g_via.end();
        }
        if (!bot.via_passed && Dist2D(me.at, via->second) < kWaypoint)
        {
          bot.via_passed = true;
        }
        if (!bot.via_passed)
        {
          to = via->second;
        }
      }
      bool vault_link = on_path && from_node >= 0 && g_vault_links.count(LinkKey(from_node, bot.path[bot.path_at]));
      // A ladder: at its end, facing it, A (the context action takes it).
      bool ladder_link =
          on_path && from_node >= 0 && g_ladder_links.count(LinkKey(from_node, bot.path[bot.path_at]));
      if (ladder_link && Dist2D(me.at, g_nodes[from_node].at) < kLadderReach && frame % 30 < 3 &&
          frame >= bot.escape_until)
      {
        bot.ladder_up = to.z > me.at.z;
        bot.unaim_until = frame + 20;
        bot.press_at = frame + 1;
        bot.press = kA;
      }
      // A vault: straight at the wall, not round it.
      Vec way = vault_link || ladder_link ? to : WalkTarget(ctx, base, pawn, me.at, to, bot, frame);
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
      bool in_volume = props.vaulting >= 0 && LoadU32(base, pawn + props.vaulting);
      bool held = Dist2D(me.at, bot.last_at) < kStillMove;
      if ((in_volume || (vault_link && held)) && frame >= bot.next_vault && frame >= bot.escape_until)
      {
        bot.next_vault = frame + 90;
        bot.unaim_until = frame + 20;
        bot.press_at = frame + 6;
        bot.press = kY;
      }
      if (!bot.target && !bot.nade_step)
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
      // again), or held still with the stick pushed for kWallFrames (into a
      // wall): the link it walks is barred from its paths for a while, it
      // walks away from the wall and finds another way.
      int node = on_path ? bot.path[bot.path_at] : -1;
      float left = Dist2D(me.at, to);
      if (on_path)
      {
        for (size_t k = bot.path_at; k + 1 < bot.path.size(); ++k)
        {
          left += Dist3D(g_nodes[bot.path[k]].at, g_nodes[bot.path[k + 1]].at);
        }
        left += Dist2D(g_nodes[bot.path.back()].at, goal_at);
      }
      // A path found again round a barred link is longer: counted afresh
      // too (else each bar makes the way longer and bars the next link).
      // A goal that moved (an enemy hunted) counts afresh only for a bot
      // that moved too: one held by a wall is not let off each time.
      bool goal_moved = Dist2D(bot.progress_goal, goal_at) > 300.0f && Dist2D(bot.progress_at, me.at) > 100.0f;
      if (!bot.progress || left < bot.progress_left - kProgressStep || goal_moved ||
          (repathed && bot.detour_ok && left > bot.progress_left + kDetour))
      {
        bot.detour_ok = false;
        bot.progress = true;
        bot.progress_until = frame + kProgressFrames;
        bot.progress_left = left;
        if (goal_moved || Dist2D(bot.progress_goal, goal_at) > 300.0f)
        {
          bot.reached.clear();
        }
        bot.progress_goal = goal_at;
        bot.progress_at = me.at;
      }
      bot.progress_left = std::min(bot.progress_left, left);
      // Held still a moment: a wall, unless it rolls or vaults (or Y over
      // a vault link is still to come); held long, a wall all the same (a
      // roll it is stuck in, a vault that does not go).
      bool walled = bot.still >= kWallFrames &&
                    ((!maneuvering && !in_volume && !vault_link && !ladder_link) || bot.still >= kHeldFrames);
      if ((frame >= bot.progress_until || walled) && frame >= bot.escape_until && bot.direct)
      {
        bot.direct = false;
        bot.direct_off_until = frame + kDirectOff;
        bot.progress = false;
        bot.still = 0;
        Logf("frame %u: bot %u gets no nearer straight at its goal, %.0f away: holds\n", frame, id, goal_d);
      }
      if ((frame >= bot.progress_until || walled) && frame >= bot.escape_until)
      {
        float want = std::atan2(to.y - me.at.y, to.x - me.at.x);
        if (node >= 0)
        {
          bot.barred.push_back({from_node, node, frame + kBarFrames});
          LinkFailed(from_node, node);
        }
        bot.side = -bot.side;
        bot.escape_dir = EscapeDir(ctx, base, pawn, me.at, want, bot.side);
        bot.escape_until = frame + kEscapeFrames;
        bot.next_path = bot.escape_until;
        bot.force_path = true; // round the barred link
        bot.detour_ok = true;  // ... which may be longer once
        bot.next_probe = 0;
        bot.progress = false;
        bot.still = 0;
        bot.reached.clear();
        Logf("frame %u: bot %u %s: link %d -> %d (%.0f %.0f %.0f) from %.0f %.0f %.0f barred (%zu), "
             "backs off %.0f degrees\n",
             frame, id, walled ? "walks into a wall" : "gets no nearer", from_node, node, to.x, to.y, to.z, me.at.x,
             me.at.y, me.at.z, bot.barred.size(), std::remainder(bot.escape_dir - want, 6.2831853f) * 57.29578f);
      }
    }
    else if (!bot.target)
    {
      bot.stuck_check = 0;
      bot.progress = false;
    }

    // Held still with the stick pushed: counted (a wall), and what holds it logged.
    {
      bool pushed = std::hypot(in.lx, in.ly) > 0.5f;
      bool moved = Dist2D(me.at, bot.last_at) > kStillMove;
      bot.last_at = me.at;
      bot.still = pushed && !moved ? bot.still + 1 : 0;
      if (!walking && bot.still >= kWallFrames)
      {
        bot.next_strafe = 0; // sideways into a wall: the other way
        bot.strafe_flip = -bot.strafe_flip;
        bot.still = 0;
      }
      if (bot.still == 60 || (bot.still && bot.still % 300 == 0))
      {
        static int32_t cover_node = -2, cover_ratio;
        if (cover_node == -2)
        {
          cover_node = ao2::ue3::PropertyOffset(base, "AO2PlayerControllerNative", "CoverEntry_CurrentCoverNode");
          cover_ratio = ao2::ue3::PropertyOffset(base, "AO2CharacterNative", "CoverRatio");
        }
        float yaw = view_yaw / kUnitsPerRadian;
        float dir = std::atan2(in.lx, in.ly) + yaw; // where the stick pushes, in the world
        Vec ahead{me.at.x + std::cos(dir) * 150.0f, me.at.y + std::sin(dir) * 150.0f, me.at.z};
        uint32_t hit = TraceHit(ctx, base, pawn, me.at, ahead);
        uint32_t node = cover_node >= 0 ? LoadU32(base, controller + cover_node) : 0;
        Logf("frame %u: bot %u still %u frames at %.0f %.0f %.0f: stick %.2f %.2f, buttons %X, physics %u, cover "
             "node %08X ratio %.2f, vault volume %08X, ahead %s (%s), path %zu/%zu, state %s\n",
             frame, id, bot.still, me.at.x, me.at.y, me.at.z, in.lx, in.ly, in.buttons, base[pawn + 88], node,
             cover_ratio >= 0 ? LoadF32(base, pawn + cover_ratio) : -1.0f,
             props.vaulting >= 0 ? LoadU32(base, pawn + props.vaulting) : 0,
             hit ? ao2::ue3::ObjectName(base, hit).c_str() : "-",
             hit ? ao2::ue3::ObjectName(base, LoadU32(base, hit + 52)).c_str() : "-", bot.path_at, bot.path.size(),
             state.c_str());
      }
    }

    // Hit in the open with a target in sight: now and then a dodge aside
    // (Y aiming), where the way is clear.
    if (bot.hurt_at == frame && bot.target && task != Task::kCover && frame >= bot.next_dodge && !maneuvering &&
        !carrying && !bot.nade_step && Noise(frame, id + 41) > -0.2f)
    {
      bot.next_dodge = frame + 200 + uint32_t((Noise(frame, id + 47) + 1.0f) * 60.0f);
      float yaw = view_yaw / kUnitsPerRadian;
      int first = Noise(frame, id + 43) < 0 ? -1 : 1;
      for (int s : {first, -first})
      {
        float a = yaw + float(s) * 1.5707964f;
        if (ProbeClear(ctx, base, pawn, me.at, {me.at.x + std::cos(a) * 250.0f, me.at.y + std::sin(a) * 250.0f, me.at.z}))
        {
          bot.dodge_at = frame + 1;
          bot.dodge_side = s;
          Logf("frame %u: bot %u hit (health %.0f%%): dodges %s\n", frame, id, health_part * 100.0f,
               s > 0 ? "right" : "left");
          break;
        }
      }
    }
    if (bot.dodge_at && frame >= bot.dodge_at && frame < bot.dodge_at + 15)
    {
      in.lx = float(bot.dodge_side);
      in.ly = 0;
      in.buttons |= kLT;
      in.buttons |= frame < bot.dodge_at + 2 ? kY : 0;
    }

    // In its partner's line of fire (a player standing or walking slowly:
    // where it looks), near and in front of it: a step aside, out of it, to
    // the side it is on already.
    if (player_partner && !in_cover && !partner->down)
    {
      uint32_t look = LoadU32(base, partner->controller + kLookHolder);
      float speed = std::hypot(LoadF32(base, partner->pawn + kActorVelocity),
                               LoadF32(base, partner->pawn + kActorVelocity + 4));
      if (look == partner->pawn && speed < kClearSlow && frame >= bot.clear_until + 15)
      {
        float pitch = LoadF32(base, look + kLookRotation) / kUnitsPerRadian;
        float yaw = LoadF32(base, look + kLookRotation + 4) / kUnitsPerRadian;
        Vec from = Head(base, partner->pawn);
        Vec dir{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
        Vec mid = Body(base, pawn);
        float along = (mid.x - from.x) * dir.x + (mid.y - from.y) * dir.y + (mid.z - from.z) * dir.z;
        Vec far{from.x + dir.x * kClearRange, from.y + dir.y * kClearRange, from.z + dir.z * kClearRange};
        if (along > 80.0f && along < kClearRange && SegmentDistance(mid, from, far) < kClearWidth)
        {
          float side = (mid.x - from.x) * -dir.y + (mid.y - from.y) * dir.x < 0 ? -1.0f : 1.0f;
          for (float s : {side, -side})
          {
            float a = yaw + s * 1.5707964f;
            if (ProbeClear(ctx, base, pawn, me.at, {me.at.x + std::cos(a) * 200.0f, me.at.y + std::sin(a) * 200.0f, me.at.z}))
            {
              bot.clear_until = frame + 30;
              bot.clear_dir = a;
              Logf("frame %u: bot %u in its partner's line of fire %.0f in front: steps %s\n", frame, id, along,
                   s > 0 ? "left" : "right");
              break;
            }
          }
        }
      }
    }
    if (frame < bot.clear_until)
    {
      Steer(me.at, {me.at.x + std::cos(bot.clear_dir) * 300.0f, me.at.y + std::sin(bot.clear_dir) * 300.0f, me.at.z},
            view_yaw, 1.0f, in);
    }

    // A grenade on the ground near it: away from it, out of its blast (out
    // of cover first: B), LT let go to run; nothing else meanwhile.
    ScanGrenades(base, frame);
    Vec nade_at;
    float nade_reach = 0;
    uint32_t danger = GrenadeNear(base, me.at, nade_at, nade_reach);
    if (danger && frame >= bot.flee_until)
    {
      float d = Dist2D(me.at, nade_at);
      float away = d > 10.0f ? std::atan2(me.at.y - nade_at.y, me.at.x - nade_at.x)
                             : view_yaw / kUnitsPerRadian + 3.1415927f;
      float run = std::max(250.0f, nade_reach - d + 100.0f);
      bot.flee_dir = away;
      for (float turn : {0.0f, 0.6f, -0.6f, 1.2f, -1.2f, 1.9f, -1.9f})
      {
        float a = away + turn;
        if (ProbeClear(ctx, base, pawn, me.at, {me.at.x + std::cos(a) * run, me.at.y + std::sin(a) * run, me.at.z}))
        {
          bot.flee_dir = a;
          break;
        }
      }
      bot.flee_from = danger;
      bot.flee_until = frame + kFleeFrames;
      if (bot.cover >= 0)
      {
        in.buttons |= bot.cover_arrived ? kB : 0;
        bot.cover = -1;
        bot.exposed = 0;
        bot.next_cover = frame + kFleeFrames;
        bot.path.clear();
        bot.progress = false;
      }
      Logf("frame %u: bot %u flees grenade %08X (%s) %.0f away (blast %.0f), %.0f degrees off straight away\n", frame,
           id, danger, ao2::ue3::ObjectName(base, danger).c_str(), d, nade_reach,
           std::remainder(bot.flee_dir - away, 6.2831853f) * 57.29578f);
    }
    if (frame < bot.flee_until)
    {
      Steer(me.at, {me.at.x + std::cos(bot.flee_dir) * 500.0f, me.at.y + std::sin(bot.flee_dir) * 500.0f, me.at.z},
            view_yaw, 1.0f, in);
      in.buttons &= ~(kLT | kL3);
      bot.stuck_check = 0;
      bot.progress = false;
    }

    // A grenade: at an enemy that hides (out of sight a moment ago, or only
    // its head shows) or at enemies close together, kNadeMin - kNadeMax
    // away, where none of its own team (nor a hostage) is, along a curve
    // nothing is in the way of; from cover too. Aimed short (it rolls on).
    uint32_t grenade = inventory.slot[kSlotGrenade];
    float nade_speed = grenade && combat.nade_speed >= 0 ? LoadF32(base, grenade + combat.nade_speed) : 0.0f;
    float nade_offset = grenade && combat.nade_pitch >= 0 ? LoadF32(base, grenade + combat.nade_pitch) : 0.0f;
    if (!bot.nade_step && frame >= bot.next_nade && bot.threat && grenade && nade_speed > 100.0f &&
        nade_speed < 10000.0f && std::fabs(nade_offset) < 60.0f && !carrying && !maneuvering &&
        (task != Task::kCover || in_cover) && task != Task::kRetreat && task != Task::kHeal && task != Task::kJoin &&
        frame >= bot.reload_until && Noise(frame / 20, id + 53) > 0.0f)
    {
      bot.next_nade = frame + 20;
      float body = g_grid.body > 0 ? g_grid.body : 45.0f;
      Vec to{bot.threat_at.x, bot.threat_at.y, bot.threat_at.z - body};
      float d = Dist2D(me.at, to);
      bool hides = (!bot.target && frame - bot.threat_seen >= 20 && frame - bot.threat_seen < 300) ||
                   (bot.target && !bot.target_body);
      int group = 0;
      bool friend_near = false;
      for (const Character &c : g_characters)
      {
        group += Enemy(me, c) && Dist2D(c.at, bot.threat_at) < kNadeGroup ? 1 : 0;
        friend_near |= (c.civilian || (c.team && c.team == me.team)) && Dist3D(c.at, to) < kNadeFriend;
      }
      Vec aim_to{me.at.x + (to.x - me.at.x) * kNadeRoll, me.at.y + (to.y - me.at.y) * kNadeRoll, to.z};
      float g = Gravity(base), pitch = 0;
      if (d >= kNadeMin && d <= kNadeMax && std::fabs(to.z - me.at.z) < 400.0f && (hides || group >= 2) &&
          !friend_near && ThrowPitch(nade_speed, nade_offset, g, eye, aim_to, pitch))
      {
        // The curve's top half way: clear from the hand to it and from it down.
        float theta = (pitch / kUnitsPerDegree + nade_offset) / 57.29578f;
        float x = Dist2D(eye, aim_to) * 0.5f;
        float c = std::cos(theta);
        float y = x * std::tan(theta) - g * x * x / (2.0f * nade_speed * nade_speed * c * c);
        Vec top{(eye.x + aim_to.x) * 0.5f, (eye.y + aim_to.y) * 0.5f, eye.z + y};
        if ((!g_last_nade || frame - g_last_nade >= kNadeApart) && !Blocked(ctx, base, pawn, eye, top) &&
            !Blocked(ctx, base, pawn, top, {to.x, to.y, to.z + 80.0f}))
        {
          g_last_nade = frame;
          bot.nade_step = 1;
          bot.nade_at = frame;
          bot.nade_to = aim_to;
          bot.nade_pitch = pitch;
          Logf("frame %u: bot %u throws a grenade at %08X (%s), %.0f away, to %.0f %.0f %.0f: speed %.0f, pitch "
               "%.1f + %.1f, gravity %.0f, %d enemies there\n",
               frame, id, bot.threat, hides ? "it hides" : "a group", d, aim_to.x, aim_to.y, aim_to.z, nade_speed,
               pitch / kUnitsPerDegree, nade_offset, g, group);
        }
      }
    }
    if (bot.nade_step)
    {
      // Selected (LB + D-pad down), aimed (LT), thrown (RT pulled and let
      // go), then the weapon taken again. Standing still meanwhile.
      uint32_t t = frame - bot.nade_at;
      bool holds = HoldsGrenade(base, pawn);
      in.buttons = 0;
      in.lx = in.ly = 0;
      auto next = [&](int step) {
        bot.nade_step = step;
        bot.nade_at = frame;
      };
      auto end = [&](const char *why, uint32_t again) {
        Logf("frame %u: bot %u grenade: %s\n", frame, id, why);
        bot.nade_step = 0;
        bot.next_nade = frame + again;
        if (bot.slot_want >= 0)
        {
          bot.dpad = kSlotButtons[bot.slot_want];
          bot.dpad_at = frame + 1;
          bot.next_switch = frame + 60;
        }
      };
      if (bot.nade_step >= 2)
      {
        TurnTowards(base, holder,
                    std::atan2(bot.nade_to.y - eye.y, bot.nade_to.x - eye.x) * kUnitsPerRadian, bot.nade_pitch,
                    0.0f);
      }
      float yaw = LoadF32(base, holder + kLookRotation + 4), pitch = LoadF32(base, holder + kLookRotation);
      float want = std::atan2(bot.nade_to.y - eye.y, bot.nade_to.x - eye.x) * kUnitsPerRadian;
      bool on = std::fabs(std::remainder(want - yaw, 65536.0f)) < kUnitsPerDegree &&
                std::fabs(std::remainder(bot.nade_pitch - pitch, 65536.0f)) < kUnitsPerDegree;
      switch (bot.nade_step)
      {
      case 1:
        if (t == 0)
        {
          bot.dpad = kDown;
          bot.dpad_at = frame;
        }
        if (holds && t >= 12)
        {
          next(2);
        }
        else if (t > 90)
        {
          end("none in its hand (none left?)", 3600);
        }
        break;
      case 2:
        in.buttons |= kLT;
        if (!holds)
        {
          end("not in its hand any more", 600);
        }
        else if (t >= 25 && on)
        {
          next(3);
        }
        else if (t > 120)
        {
          end("could not aim", 300);
        }
        break;
      case 3:
        in.buttons |= kLT | kRT;
        if (t >= 10)
        {
          next(4);
        }
        break;
      default:
        in.buttons |= t < 6 ? kLT : 0;
        if (t == 20)
        {
          // The grenade in flight (for the log only): the one nearest the
          // bot (a scan of every object, once a throw).
          static uint32_t proj_class = 0, phys_class = 0;
          proj_class = proj_class ? proj_class : ao2::ue3::FindClass(base, "AO2Proj_Grenade");
          phys_class = phys_class ? phys_class : ao2::ue3::FindClass(base, "AO2PhysProj_Grenade");
          bot.nade_projectile = 0;
          float best = 800.0f;
          uint32_t count = ao2::ue3::ObjectCount(base);
          for (uint32_t i = 0; i < count; ++i)
          {
            uint32_t o = ao2::ue3::ObjectAt(base, i);
            if (!o || !((proj_class && ao2::ue3::IsA(base, o, proj_class)) ||
                        (phys_class && ao2::ue3::IsA(base, o, phys_class))))
            {
              continue;
            }
            float d = Dist3D(LoadVec(base, o + kActorLocation), eye);
            if (d < best)
            {
              best = d;
              bot.nade_projectile = o;
            }
          }
          bot.nade_watch_until = frame + 240;
          Logf("frame %u: bot %u grenade in flight: %08X %.0f away\n", frame, id, bot.nade_projectile, best);
        }
        if (t >= 50)
        {
          end("thrown", 900 + uint32_t((Noise(frame, id + 59) + 1.0f) * 150.0f));
        }
        break;
      }
    }
    // Where a thrown grenade goes (to see the aim is right).
    if (bot.nade_projectile && frame < bot.nade_watch_until && frame % 15 == 0)
    {
      uint32_t cls = LoadU32(base, bot.nade_projectile + 52);
      static uint32_t actor_class = 0;
      actor_class = actor_class ? actor_class : ao2::ue3::FindClass(base, "Actor");
      if (cls && actor_class && ao2::ue3::IsA(base, bot.nade_projectile, actor_class))
      {
        Vec at = LoadVec(base, bot.nade_projectile + kActorLocation);
        if (Dist3D(at, bot.nade_last) < 1.0f)
        {
          bot.nade_projectile = 0; // still: it went off (or lies)
        }
        bot.nade_last = at;
        Logf("frame %u: bot %u grenade at %.0f %.0f %.0f, %.0f from where it was aimed\n", frame, id, at.x, at.y,
             at.z, Dist2D(at, bot.nade_to));
      }
      else
      {
        bot.nade_projectile = 0;
      }
    }

    // The weapon for the threat's distance (the depot's while it shoots at
    // one: a shotgun did nothing at 900; the primary with none); not while
    // it throws or reloads, nor over a weapon picked up (slot 4).
    if (!bot.nade_step && frame >= bot.reload_until && !maneuvering && !carrying)
    {
      int want = bot.threat         ? SlotFor(base, inventory, Dist2D(me.at, bot.threat_at))
                 : shoot_objective  ? SlotFor(base, inventory, goal_d)
                 : inventory.slot[0] ? 0
                                     : SlotFor(base, inventory, 1000.0f);
      if (want != bot.slot_want)
      {
        bot.slot_want = want;
        bot.slot_want_since = frame;
      }
      if (want >= 0 && inventory.current >= 0 && inventory.current != 4 && inventory.current != want &&
          frame - bot.slot_want_since >= 30 && frame >= bot.next_switch)
      {
        bot.dpad = kSlotButtons[want];
        bot.dpad_at = frame;
        bot.next_switch = frame + 150;
        Logf("frame %u: bot %u takes slot %d (%s) for %.0f\n", frame, id, want,
             ao2::ue3::ObjectName(base, inventory.slot[want]).c_str(),
             bot.threat ? Dist2D(me.at, bot.threat_at) : 0.0f);
      }
    }
    // Down in cover it crouches (L3: the character does not of itself),
    // up and out of cover it stands; L3 a moment after the last press, the
    // state read back (Pawn.bIsCrouched).
    {
      bool want = in_cover && hiding && frame - bot.hide_since >= 3;
      if (Crouched(base, pawn) != want && frame >= bot.next_crouch && !maneuvering && !bot.nade_step && !carrying)
      {
        bot.next_crouch = frame + 30;
        in.buttons |= kL3;
      }
    }

    // A weapon picked: LB held a moment, the D-pad pressed meanwhile.
    if (bot.dpad && frame >= bot.dpad_at && frame < bot.dpad_at + 10)
    {
      in.buttons |= kLB | (frame >= bot.dpad_at + 3 && frame < bot.dpad_at + 6 ? bot.dpad : 0);
    }

    if (frame < bot.unaim_until && !bot.nade_step)
    {
      in.buttons &= ~(kLT | kRT);
    }
    if (frame >= bot.press_at && frame < bot.press_at + 2 && !(bot.press == kY && maneuvering) && !bot.nade_step &&
        !carrying)
    {
      in.buttons |= bot.press;
    }

    // Reload: an empty magazine; a part-empty one with no target, or ducked
    // in cover; not with a grenade in its hand.
    uint32_t weapon = LoadU32(base, pawn + kPawnWeapon);
    int32_t ammo = weapon ? int32_t(LoadU32(base, weapon + kWeaponAmmo)) : -1;
    int32_t max_ammo = weapon ? int32_t(LoadU32(base, weapon + kWeaponMaxAmmo)) : 0;
    bool part = max_ammo > 0 && max_ammo < 10000;
    bool reload = weapon && !bot.nade_step && !HoldsGrenade(base, pawn) &&
                  (ammo <= 0 || (!bot.target && part && ammo < max_ammo / 3) || (hiding && part && ammo < max_ammo / 2));
    if (reload && frame >= bot.next_reload)
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
      Logf("frame %u: bot %u at %.0f %.0f %.0f team %08X, %s, target %08X, goal %.0f %.0f %.0f %.0f away%s, path "
           "%zu/%zu, stick %.2f %.2f, buttons %X, ammo %d\n",
           frame, id, me.at.x, me.at.y, me.at.z, me.team, TaskName(task), bot.target, goal_at.x, goal_at.y, goal_at.z,
           goal_d, there ? " (there)" : "", bot.path_at, bot.path.size(), in.lx, in.ly, in.buttons, ammo);
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
    if (!g_settings.on || !g_places || !g_have_template || !modes || !LoadU32(base, modes + 48) ||
        !FindKeys(base))
    {
      return;
    }
    uint32_t frame = ao2::coop::SyncFrame(base);
    g_nav_now = frame;
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
    g_via.clear();
    g_vault_boxes.clear();
    g_ladder_ends.clear();
    g_ladder_links.clear();
    g_part.clear();
    g_grid = {};
    g_link_fails.clear();
    g_point_fails.clear();
    g_nav_ready = false;
    g_sent_by = {};
    g_settings = {};
    g_settings_from = -1;
    g_sent_line.clear();
    PublishInfo();
    if (mask)
    {
      Logf("empty places: %X%s\n", mask, REXCVAR_GET(ao2_versus_bots) ? ", bots play them" : "");
    }
  }

  void PollSettings(const uint8_t *base)
  {
    uint32_t modes = LoadU32(base, kGameModes);
    if (!modes || !LoadU32(base, modes + 48) || LoadU32(base, modes + 52))
    {
      return; // not a versus match (or a ranked one: no bots there)
    }
    uint32_t lobby = LoadU32(base, kLobby);
    bool host = (lobby && base[lobby + kLobbyStarted]) || REXCVAR_GET(ao2_coop_server);
    auto hundredths = [](double v, double lo, double hi) { return int(std::lround(std::clamp(v, lo, hi) * 100.0)); };
    char line[160];
    std::snprintf(line, sizeof(line), "vbots %d %d %d %d %d %d %d %d %d %d %d %d %d %d", host ? 1 : 0,
                  REXCVAR_GET(ao2_versus_bots) ? 1 : 0, hundredths(REXCVAR_GET(ao2_versus_bots_miss), 0.0, 20.0),
                  std::clamp(REXCVAR_GET(ao2_versus_bots_reaction), 0, 300),
                  hundredths(REXCVAR_GET(ao2_versus_bots_turn), 0.5, 45.0),
                  int(std::lround(std::clamp(REXCVAR_GET(ao2_versus_bots_range), 500.0, 20000.0))),
                  REXCVAR_GET(ao2_versus_bots_buy) ? 1 : 0, hundredths(REXCVAR_GET(ao2_versus_bots_buy_share), 0.0, 1.0),
                  REXCVAR_GET(ao2_versus_bots_infinite_ammo) ? 1 : 0,
                  std::clamp(REXCVAR_GET(ao2_versus_round_minutes), 0, 120),
                  std::clamp(REXCVAR_GET(ao2_versus_cash_goal), 0, 100000000) / 1000,
                  std::clamp(REXCVAR_GET(ao2_versus_friendly_fire), 0, 2),
                  std::clamp(REXCVAR_GET(ao2_versus_respawn_cost), 0, 10000000) / 100,
                  std::clamp(REXCVAR_GET(ao2_versus_respawn_step), 0, 10000000) / 100);
    // Once per match: the rules and bots stay as the match began (the F1
    // menu does not change them mid-match either).
    if (g_sent_line.empty())
    {
      g_sent_line = line;
      ao2::coop::cmd::Send(line);
    }
  }

  void ReceiveSettings(uint32_t controller_id, const char *args, bool mine)
  {
    int host, on, miss, reaction, turn, range, buy, share, ammo, minutes = 0, goal = 0, ff = 0, cost = 0, step = 0;
    int n = controller_id < 4 ? std::sscanf(args, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d", &host, &on, &miss,
                                            &reaction, &turn, &range, &buy, &share, &ammo, &minutes, &goal, &ff,
                                            &cost, &step)
                              : 0;
    if (n != 9 && n != 14)
    {
      Logf("bad settings from %u: %s\n", controller_id, args);
      return;
    }
    Sent &s = g_sent_by[controller_id];
    s.have = true;
    s.host = host != 0;
    s.mine = mine;
    s.settings.on = on != 0;
    s.settings.miss = std::clamp(miss, 0, 2000) / 100.0;
    s.settings.reaction = std::clamp(reaction, 0, 300);
    s.settings.turn = std::clamp(turn, 50, 4500) / 100.0;
    s.settings.range = double(std::clamp(range, 500, 20000));
    s.settings.buy = buy != 0;
    s.settings.share = std::clamp(share, 0, 100) / 100.0;
    s.settings.ammo = ammo != 0;
    s.settings.minutes = std::clamp(minutes, 0, 120);
    s.settings.goal = std::clamp(goal, 0, 100000) * 1000;
    s.settings.ff = std::clamp(ff, 0, 2);
    s.settings.cost = std::clamp(cost, 0, 100000) * 100;
    s.settings.step = std::clamp(step, 0, 100000) * 100;
    int from = -1;
    for (int pass = 0; pass < 2 && from < 0; ++pass)
    {
      for (int id = 0; id < 4 && from < 0; ++id)
      {
        from = g_sent_by[id].have && (pass || g_sent_by[id].host) ? id : -1;
      }
    }
    g_settings_from = from;
    g_settings = from >= 0 ? g_sent_by[from].settings : ao2::versus::VersusSettings{};
    Logf("settings of %u%s: %s; the bots play by %d's: %s, miss %.2f, reaction %d, turn %.2f, range %.0f, buy %d "
         "(share %.2f), infinite ammo %d; round %d min, cash goal %d, friendly fire %d, respawn %d + %d\n",
         controller_id, host ? " (host)" : "", args, from, g_settings.on ? "on" : "off", g_settings.miss,
         g_settings.reaction, g_settings.turn, g_settings.range, g_settings.buy ? 1 : 0, g_settings.share,
         g_settings.ammo ? 1 : 0, g_settings.minutes, g_settings.goal, g_settings.ff, g_settings.cost,
         g_settings.step);
    PublishInfo();
  }

  const VersusSettings &PlayedSettings()
  {
    return g_settings;
  }

  MatchSettingsInfo MatchSettings()
  {
    std::lock_guard lock(g_info_mutex);
    return g_info;
  }

  void AfterReplayedRecord(PPCContext &ctx, uint8_t *base)
  {
    if (g_in_frame && !g_rules_ran && !g_injecting)
    {
      g_rules_ran = true;
      uint32_t modes = LoadU32(base, kGameModes);
      if (modes && LoadU32(base, modes + 48) && !LoadU32(base, modes + 52))
      {
        ao2::versus::rules::Frame(ctx, base);
      }
    }
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
  g_rules_ran = false;
  __imp__sub_82A26290(ctx, base);
  g_in_frame = false;
}
