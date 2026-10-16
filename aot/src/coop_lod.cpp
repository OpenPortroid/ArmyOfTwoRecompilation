// coop_lod.cpp - Skeletal LOD out of the online co-op simulation.
//
// The renderer picks each skeletal mesh's LOD from this machine's own view
// (USkeletalMeshComponent::PredictedLODLevel, by screen size) and the
// animation is evaluated on that LOD's bones, so a pawn's animation-driven
// movement depended on where this machine's camera was. Two-instance trace
// (2026-10-07, Iraq): grunt_1's PredictedLODLevel went 1 -> 0 on one machine
// only on frame 3454, its velocity differed in the low bits on the same
// frame, and the lockstep checksum followed ~1100 frames later ("Connection
// Lost" a few minutes into fights; the enemies near the players flip LODs).
//
// Whether the mesh was drawn lately on this machine (LastRenderTime) also
// changed it: the next run, with the LOD forced, grunt_1 had been drawn on one
// machine only when its velocity split. UE3 skips the skeleton / anim node /
// skeletal controller updates of meshes not rendered recently unless
// bUpdateSkelWhenNotRendered / bTickAnimNodesWhenNotRendered are set and
// bIgnoreControllersWhenNotRendered is not.
//
// The flags were not enough (elite_2 drawn on one machine only split the next
// run): anim nodes have their own "skip when not rendered" and script reads
// LastRenderTime. The rendering thread writes it (sub_8261E8B8: each visible
// primitive's component +624 and its owner actor +164 = the view's world
// time), so it says what this machine drew.
//
// While the lockstep runs, on each simulation step on both machines every
// pawn's mesh gets ForcedLodModel = the shared LOD + 1 (below), the flags set to
// "update as if rendered", and the pawn and its mesh LastRenderTime =
// WorldInfo.TimeSeconds: every pawn counts as just drawn on both machines.
// (The step runs before the draw of its state, whose rendering writes that
// same time.) Offsets by name (ue3_reflect.h); which flags this build has
// goes to coop_lod.log.
//
// ForcedLodModel only covers the rendering LOD. AO2's skeletal mesh has its
// own "logical" LOD for the animation (Organiquat): sub_828A4EF0 (from
// sub_826EBE38) takes the mesh bounds' screen size in each of this machine's
// views and writes the lowest LOD over the views into MinDesiredLODLevel
// (+1216, rendering) and LogicalLODLevel (+1220, animation; copied from the
// ParentAnimComponent +764 if any). Two-instance run (2026-10-07):
// elite_4's LogicalLODLevel was 0 on one machine and 1 on the other on frame
// 2160, its RootMotionDelta / RootMotionVelocity and velocity split on 2161.
// While the lockstep runs both are the same on both machines: after that
// function and, for meshes this machine does not draw, on each simulation
// step.
//
// Every object is checked before anything is written to it (aligned, and a
// Pawn / SkeletalMeshComponent / WorldInfo by class): an unchecked pointer
// (a controller's pawn, a PawnList link or Pawn.Mesh going stale) once put
// these floats into other objects' reference fields - LastRenderTime
// (TimeSeconds, 85.5 = 0x42AB0000) and the screen size (0.3288 =
// 0x3EA85AC0) - and the garbage collector crashed on them minutes later
// (sub_82531730, 2026-10-08/09).
//
// Shared LOD: LOD 0 for every pawn of the level cost most of the frame in
// fights (the rendering thread drew and skinned each enemy at full detail;
// 120 FPS fell to 40-60). Each step now picks every pawn mesh's LOD with the
// game's own rule from sub_828A4EF0 (size = scale / depth * SphereRadius *
// 1.375 against each LOD's DisplayFactor in the mesh's two tables), but from
// one view both machines share instead of this machine's camera: depth = the
// distance from Bounds.Origin to the nearest player's pawn, scale =
// ao2_coop_lod_scale. Locations and bounds are simulation state, so the
// result is the same bits on both machines. Far enemies get the low LODs,
// players and the enemies near them stay at full detail.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "coop.h"
#include "ue3_reflect.h"
#include "log_files.h"

REXCVAR_DEFINE_BOOL(ao2_coop_force_lod, true, "Online",
                    "Online co-op: the same skeletal LOD for every pawn on both machines - the view-dependent "
                    "LOD changed the simulation (desync)");
REXCVAR_DEFINE_DOUBLE(ao2_coop_lod_scale, 2.0, "Online",
                      "Online co-op: screen scale of the shared pawn LOD (higher = full detail farther away; "
                      "0 = every pawn at full detail). Must be the same on both PCs");

namespace
{
  constexpr uint32_t kEngine = 0x831EFCBC; // GEngine; +772 = GameViewportClient
  constexpr uint32_t kMinDesiredLod = 1216; // SkeletalMeshComponent.MinDesiredLODLevel
  constexpr uint32_t kLogicalLod = 1220;    // SkeletalMeshComponent.LogicalLODLevel
  // The rest of what sub_828A4EF0 reads and writes (component r3):
  constexpr uint32_t kSkelMesh = 660;     // SkeletalMesh
  constexpr uint32_t kParentAnim = 764;   // ParentAnimComponent
  constexpr uint32_t kMaxFactor = 852;    // the largest screen size over the views
  constexpr uint32_t kBoundsOrigin = 392; // Bounds.Origin x, y, z
  constexpr uint32_t kBoundsRadius = 416; // Bounds.SphereRadius
  // SkeletalMesh: the rendering LOD table (count +164, entries +212) and the
  // logical one (count +228, entries +224); 20-byte entries, DisplayFactor
  // first.
  constexpr uint32_t kRenderLodCount = 164, kRenderLods = 212;
  constexpr uint32_t kLogicalLodCount = 228, kLogicalLods = 224;

  struct Lod
  {
    uint32_t render = 0, logical = 0;
    float factor = 0.0f;
  };
  // Pawn meshes' LODs of this step (game thread), for sub_828A4EF0.
  std::unordered_map<uint32_t, Lod> g_lods;

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
  bool Plausible(uint32_t p)
  {
    return p >= 0x40000000 && p < 0x80000000 && !(p & 3);
  }

  // The first local player's PlayerController (GEngine +772 =
  // GameViewportClient, vtable +268 finds the local player of a controller
  // id, LocalPlayer +64 = its PlayerController).
  uint32_t FirstController(PPCContext &ctx, uint8_t *base)
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
    return Plausible(player) ? LoadU32(base, player + 64) : 0;
  }

  struct Vec
  {
    float x, y, z;
  };

  // The highest LOD whose DisplayFactor is above `size` (sub_828A4EF0's loop).
  uint32_t PickLod(const uint8_t *base, uint32_t count, uint32_t entries, float size)
  {
    uint32_t lod = 0;
    if (count > 16 || !Plausible(entries))
    {
      return 0;
    }
    for (uint32_t i = 1; i < count; ++i)
    {
      if (LoadF32(base, entries + i * 20) > size)
      {
        lod = i;
      }
    }
    return lod;
  }

  // sub_828A4EF0 with one view at the nearest player: same float operations
  // on both machines.
  Lod SharedLod(const uint8_t *base, uint32_t component, const std::vector<Vec> &players, float scale)
  {
    Lod lod;
    uint32_t mesh = LoadU32(base, component + kSkelMesh);
    if (!Plausible(mesh) || players.empty() || !(scale > 0.0f))
    {
      return lod;
    }
    float x = LoadF32(base, component + kBoundsOrigin), y = LoadF32(base, component + kBoundsOrigin + 4),
          z = LoadF32(base, component + kBoundsOrigin + 8);
    float nearest = INFINITY;
    for (const Vec &p : players)
    {
      float dx = x - p.x, dy = y - p.y, dz = z - p.z;
      nearest = std::fmin(nearest, dx * dx + dy * dy + dz * dz);
    }
    float depth = std::fmax(std::sqrt(nearest), 1.0f);
    float size = scale / depth * LoadF32(base, component + kBoundsRadius) * 1.375f;
    if (!std::isfinite(size))
    {
      return lod;
    }
    lod.render = PickLod(base, LoadU32(base, mesh + kRenderLodCount), LoadU32(base, mesh + kRenderLods), size);
    lod.logical = PickLod(base, LoadU32(base, mesh + kLogicalLodCount), LoadU32(base, mesh + kLogicalLods), size);
    lod.factor = size;
    return lod;
  }

  // Classes the writes are checked against (ForceLod looks them up).
  uint32_t g_pawn_class = 0, g_skel_class = 0, g_info_class = 0;

  // A live-looking object of class `cls` (or a subclass) at an aligned address.
  bool Valid(const uint8_t *base, uint32_t object, uint32_t cls)
  {
    return Plausible(object) && !(object & 3) && ao2::ue3::IsA(base, object, cls);
  }
  bool IsPawn(const uint8_t *base, uint32_t object)
  {
    return Valid(base, object, g_pawn_class);
  }

  void WriteLod(uint8_t *base, uint32_t component, const Lod &lod)
  {
    StoreU32(base, component + kMinDesiredLod, lod.render);
    StoreU32(base, component + kLogicalLod, lod.logical);
    StoreU32(base, component + kMaxFactor, std::bit_cast<uint32_t>(lod.factor));
  }
} // namespace

namespace ao2::coop
{
  // Once per simulation step on the game thread (engine pad poll).
  void ForceLod(PPCContext &ctx, uint8_t *base)
  {
    if (!REXCVAR_GET(ao2_coop_force_lod) || !OnlineLockstep(base))
    {
      return;
    }
    static int32_t pawn_off = -2, world_info = -1, pawn_list = -1, next_pawn = -1, mesh = -1, forced = -1;
    static int32_t time_seconds = -1, actor_render = -1, component_render = -1, location = -1, pawn_controller = -1;
    static uint32_t player_class = 0;
    if (pawn_off == -2 || forced < 0 || !g_pawn_class || !g_skel_class || !g_info_class)
    {
      g_pawn_class = ao2::ue3::FindClass(base, "Pawn");
      g_skel_class = ao2::ue3::FindClass(base, "SkeletalMeshComponent");
      g_info_class = ao2::ue3::FindClass(base, "WorldInfo");
      location = ao2::ue3::PropertyOffset(base, "Actor", "Location");
      pawn_controller = ao2::ue3::PropertyOffset(base, "Pawn", "Controller");
      player_class = ao2::ue3::FindClass(base, "PlayerController");
      pawn_off = ao2::ue3::PropertyOffset(base, "Controller", "Pawn");
      world_info = ao2::ue3::PropertyOffset(base, "Actor", "WorldInfo");
      pawn_list = ao2::ue3::PropertyOffset(base, "WorldInfo", "PawnList");
      next_pawn = ao2::ue3::PropertyOffset(base, "Pawn", "NextPawn");
      mesh = ao2::ue3::PropertyOffset(base, "Pawn", "Mesh");
      forced = ao2::ue3::PropertyOffset(base, "SkeletalMeshComponent", "ForcedLodModel");
      time_seconds = ao2::ue3::PropertyOffset(base, "WorldInfo", "TimeSeconds");
      actor_render = ao2::ue3::PropertyOffset(base, "Actor", "LastRenderTime");
      component_render = ao2::ue3::PropertyOffset(base, "PrimitiveComponent", "LastRenderTime");
    }
    if (pawn_off < 0 || world_info < 0 || pawn_list < 0 || next_pawn < 0 || mesh < 0 || forced < 0 ||
        !g_pawn_class || !g_skel_class || !g_info_class)
    {
      return;
    }
    // Bool flags: {offset, mask, value}.
    struct Flag
    {
      const char *name;
      bool value;
      int32_t offset = -1;
      uint32_t mask = 0;
    };
    static Flag flags[] = {{"bUpdateSkelWhenNotRendered", true},
                           {"bTickAnimNodesWhenNotRendered", true},
                           {"bIgnoreControllersWhenNotRendered", false},
                           {"bForceRefpose", false}};
    static bool flags_found = false;
    if (!flags_found)
    {
      flags_found = true;
      FILE *f = ao2::OpenLogFile("coop_lod.log");
      for (Flag &fl : flags)
      {
        ao2::ue3::BoolProperty(base, "SkeletalMeshComponent", fl.name, fl.offset, fl.mask);
        if (f)
        {
          std::fprintf(f, "SkeletalMeshComponent.%s: offset %d mask %08X\n", fl.name, fl.offset, fl.mask);
        }
      }
      if (f)
      {
        std::fprintf(f, "ForcedLodModel %d WorldInfo.TimeSeconds %d Actor.LastRenderTime %d "
                        "PrimitiveComponent.LastRenderTime %d Actor.Location %d Pawn.Controller %d "
                        "PlayerController %08X\n",
                     forced, time_seconds, actor_render, component_render, location, pawn_controller, player_class);
        std::fclose(f);
      }
      flags[3].offset = -1; // bForceRefpose: only logged
    }
    uint32_t controller = FirstController(ctx, base);
    uint32_t any = Plausible(controller) ? LoadU32(base, controller + pawn_off) : 0;
    uint32_t info = IsPawn(base, any) ? LoadU32(base, any + world_info) : 0;
    if (!Valid(base, info, g_info_class))
    {
      return;
    }
    uint32_t pawn = LoadU32(base, info + pawn_list);
    uint32_t now = time_seconds >= 0 ? LoadU32(base, info + time_seconds) : 0;
    // The players' pawns: the shared view's places.
    static std::vector<Vec> players;
    players.clear();
    if (location >= 0 && pawn_controller >= 0 && player_class)
    {
      uint32_t p = pawn;
      for (int i = 0; i < 512 && IsPawn(base, p); ++i)
      {
        uint32_t c = LoadU32(base, p + pawn_controller);
        if (Plausible(c) && ao2::ue3::IsA(base, c, player_class))
        {
          players.push_back({LoadF32(base, p + location), LoadF32(base, p + location + 4),
                             LoadF32(base, p + location + 8)});
        }
        p = LoadU32(base, p + next_pawn);
      }
    }
    float scale = float(REXCVAR_GET(ao2_coop_lod_scale));
    g_lods.clear();
    for (int i = 0; i < 512 && IsPawn(base, pawn); ++i)
    {
      if (now && actor_render >= 0)
      {
        StoreU32(base, pawn + actor_render, now);
      }
      uint32_t m = LoadU32(base, pawn + mesh);
      bool skel = Valid(base, m, g_skel_class);
      if (skel && now && component_render >= 0)
      {
        StoreU32(base, m + component_render, now);
      }
      if (skel)
      {
        Lod lod = SharedLod(base, m, players, scale);
        g_lods[m] = lod;
        if (LoadU32(base, m + forced) != lod.render + 1)
        {
          StoreU32(base, m + forced, lod.render + 1);
        }
        WriteLod(base, m, lod);
        for (const Flag &fl : flags)
        {
          if (fl.offset < 0 || !fl.mask)
          {
            continue;
          }
          uint32_t w = LoadU32(base, m + fl.offset), want = fl.value ? (w | fl.mask) : (w & ~fl.mask);
          if (w != want)
          {
            StoreU32(base, m + fl.offset, want);
          }
        }
      }
      pawn = LoadU32(base, pawn + next_pawn);
    }
    // Every 600 steps: how many pawn meshes got each rendering LOD.
    static uint32_t steps = 0;
    if (++steps % 600 == 0)
    {
      if (FILE *f = ao2::OpenLogFile("coop_lod.log", "a"))
      {
        uint32_t count[4] = {};
        for (const auto &[m, lod] : g_lods)
        {
          count[std::min<uint32_t>(lod.render, 3)]++;
        }
        std::fprintf(f, "step %u: players %zu scale %.3f, pawn meshes by LOD 0/1/2/3+: %u %u %u %u\n", steps,
                     players.size(), scale, count[0], count[1], count[2], count[3]);
        std::fclose(f);
      }
    }
  }
} // namespace ao2::coop

// USkeletalMeshComponent LOD from this machine's views (r3 = component, r4 =
// the views array): the LOD the same on both machines (see above) - a pawn
// mesh's from this step, a mesh animated by one its parent's, else 0.
extern "C" REX_FUNC(__imp__sub_828A4EF0);
extern "C" REX_FUNC(sub_828A4EF0)
{
  uint32_t component = ctx.r3.u32;
  uint32_t views = ctx.r4.u32;
  __imp__sub_828A4EF0(ctx, base);
  if (REXCVAR_GET(ao2_coop_force_lod) && ao2::coop::OnlineLockstep(base) && Valid(base, component, g_skel_class))
  {
    // This machine's own view scale (view +32), for ao2_coop_lod_scale.
    static int logged = 0;
    if (logged < 4 && Plausible(views) && LoadU32(base, views + 4) > 0 && Plausible(LoadU32(base, views)))
    {
      logged++;
      if (FILE *f = ao2::OpenLogFile("coop_lod.log", "a"))
      {
        std::fprintf(f, "view scale %.4f (this machine's camera)\n", LoadF32(base, LoadU32(base, views) + 32));
        std::fclose(f);
      }
    }
    auto it = g_lods.find(component);
    if (it == g_lods.end())
    {
      uint32_t parent = LoadU32(base, component + kParentAnim);
      it = Plausible(parent) ? g_lods.find(parent) : g_lods.end();
    }
    if (it != g_lods.end())
    {
      WriteLod(base, component, it->second);
    }
    else
    {
      StoreU32(base, component + kMinDesiredLod, 0);
      StoreU32(base, component + kLogicalLod, 0);
    }
  }
}
