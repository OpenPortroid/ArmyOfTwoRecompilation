// coop_sync_trace.cpp - Per-frame lockstep state, to find where a desync starts.
//
// The lockstep checksum of a frame (coop.h) is two 16-bit words: +44 = the
// shared rand() (sub_823EAF28) the frame sync tick draws at its start, XORed
// in, and +46 = a physics hash; the tick builds this machine's record of the
// frame with them (sub_82A2D708, r4 = frame) and sub_82A26290 compares them
// with the partner's. Once the rand() stream or the physics differ they stay
// different, so the first mismatching frame is long after the cause.
//
// ao2_coop_sync_trace: sync_trace.log gets one line per lockstep frame, at
// the record build:
//   F <frame> cs <+44> <+46> rand <state> calls <n> draw <n> | <players>
// - calls: rand() numbers the simulation took from the shared stream since
//   the last record; draw: numbers taken in extra draws (decouple.cpp puts
//   the state back after those, so they should not matter);
// - players: for each controller id 0-3 the look rotation (float bits) and
//   the pawn's name, location, rotation, velocity and health (offsets by
//   name, ue3_reflect.h);
// and a line with who took the numbers ("R <frame> site=count ..."): the
// caller's address, or for UnrealScript's Rand() / FRand() the running
// function and its object's class; and every pawn (enemies too) by name
// ("P <frame> name loc .. rot .. vel .. hp .."). tools/sync_diff.py compares the logs of
// both machines and prints the first frame that differs.
#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc.h>


#include "coop.h"
#include "decouple.h"
#include "ue3_reflect.h"
#include "log_files.h"

REXCVAR_DEFINE_INT32(ao2_coop_sync_dump, 0, "Debug",
                     "With ao2_coop_sync_trace: every N lockstep frames dump the raw memory of each enemy pawn, "
                     "its mesh and controller to sync_dump.bin (tools/sync_dump_diff.py); 0 = off");
REXCVAR_DEFINE_BOOL(ao2_coop_sync_trace, false, "Debug",
                    "Log the lockstep checksums, rand() callers and players of every frame to sync_trace.log "
                    "(compare two machines with tools/sync_diff.py)");

namespace
{
  constexpr uint32_t kFrameSync = 0x831F3008;
  constexpr uint32_t kRandState = 0x8317BA6C;
  constexpr uint32_t kSRandSeed = 0x8317BA68; // appSRand's seed (sub_823EAFE0 / sub_823EB050)
  constexpr uint32_t kEngine = 0x831EFCBC; // GEngine; +772 = GameViewportClient

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }
  uint32_t LoadU16(const uint8_t *base, uint32_t addr)
  {
    return uint32_t(base[addr]) << 8 | base[addr + 1];
  }

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("sync_trace.log");
    return f;
  }

  // Nothing to collect for when the log cannot be written (log_to_files off).
  bool On()
  {
    return REXCVAR_GET(ao2_coop_sync_trace) && Log();
  }

  bool Plausible(uint32_t p)
  {
    return p >= 0x40000000 && p < 0x80000000 && !(p & 3);
  }

  bool Readable(const uint8_t *base, uint32_t addr, uint32_t size)
  {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(base + addr, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
    {
      return false;
    }
    return base + addr + size <= static_cast<const uint8_t *>(info.BaseAddress) + info.RegionSize;
  }

  std::string Name(const uint8_t *base, uint32_t index)
  {
    if (index >= 0x100000)
    {
      return "?";
    }
    uint32_t entry = LoadU32(base, ao2::ue3::GNames(base) + index * 4);
    if (!Plausible(entry) || !Readable(base, entry + 16, 64))
    {
      return "?";
    }
    const char *s = reinterpret_cast<const char *>(base + entry + 16);
    size_t n = strnlen(s, 64);
    for (size_t i = 0; i < n; ++i)
    {
      if (s[i] < 33 || s[i] > 126)
      {
        return "?";
      }
    }
    return std::string(s, n);
  }

  // "Function@Class" of an UnrealScript frame (FFrame: +8 Node, +12 Object).
  std::string ScriptSite(const uint8_t *base, uint32_t frame)
  {
    if (!Plausible(frame) || !Readable(base, frame, 16))
    {
      return "script?";
    }
    // FFrame: +4 Node (the running UFunction), +8 Object, +24 PreviousFrame.
    uint32_t node = LoadU32(base, frame + 4), object = LoadU32(base, frame + 8);
    std::string fn = Plausible(node) && Readable(base, node, 56) ? Name(base, LoadU32(base, node + 44)) : "?";
    uint32_t cls = Plausible(object) && Readable(base, object, 56) ? LoadU32(base, object + 52) : 0;
    std::string cn = Plausible(cls) && Readable(base, cls, 56) ? Name(base, LoadU32(base, cls + 44)) : "?";
    return fn + "@" + cn;
  }

  // Game thread only.
  struct FrameData
  {
    uint32_t calls = 0, draw_calls = 0;
    std::map<std::string, uint32_t> sites;
    std::map<std::string, uint32_t> srand_sites; // the second generator (appSRand)
  };
  FrameData g_frame;
  uint32_t g_last_frame = ~0u;
  thread_local uint32_t t_script_frame = 0; // FFrame of a running Rand() / FRand()
  thread_local bool t_game_thread = false;  // the thread that builds the records
  uint32_t g_thread_rand_state = 0;         // the game thread's own rand() state address

  uint32_t CallGuest(PPCContext &ctx, uint8_t *base, uint32_t fn, uint32_t r3, uint32_t r4)
  {
    PPCContext saved = ctx;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
    uint32_t result = ctx.r3.u32;
    ctx = saved;
    return result;
  }

  // The PlayerController of a controller id, as the engine routes input
  // (mouse_aim.cpp ControllerOf).
  uint32_t ControllerOf(PPCContext &ctx, uint8_t *base, uint32_t id)
  {
    uint32_t engine = LoadU32(base, kEngine);
    uint32_t gvc = engine ? LoadU32(base, engine + 772) : 0;
    uint32_t vtable = gvc ? LoadU32(base, gvc) : 0;
    if (!vtable)
    {
      return 0;
    }
    uint32_t player = CallGuest(ctx, base, LoadU32(base, vtable + 268), gvc, id);
    return Plausible(player) ? LoadU32(base, player + 64) : 0;
  }

  // Property offsets found by name (ue3_reflect.h), -1 = not found.
  struct Offsets
  {
    int32_t location = -1, rotation = -1, velocity = -1, health = -1, pawn = -1;
    int32_t world_info = -1, pawn_list = -1, next_pawn = -1;
    bool found = false;
  };
  const Offsets &GetOffsets(const uint8_t *base)
  {
    static Offsets o;
    if (!o.found)
    {
      o.location = ao2::ue3::PropertyOffset(base, "Actor", "Location");
      o.rotation = ao2::ue3::PropertyOffset(base, "Actor", "Rotation");
      o.velocity = ao2::ue3::PropertyOffset(base, "Actor", "Velocity");
      o.health = ao2::ue3::PropertyOffset(base, "Pawn", "Health");
      o.pawn = ao2::ue3::PropertyOffset(base, "Controller", "Pawn");
      o.world_info = ao2::ue3::PropertyOffset(base, "Actor", "WorldInfo");
      o.pawn_list = ao2::ue3::PropertyOffset(base, "WorldInfo", "PawnList");
      o.next_pawn = ao2::ue3::PropertyOffset(base, "Pawn", "NextPawn");
      o.found = o.location >= 0 && o.health >= 0 && o.pawn >= 0 && o.world_info >= 0 && o.pawn_list >= 0 &&
                o.next_pawn >= 0;
      if (FILE *f = Log())
      {
        std::fprintf(f,
                     "# offsets: Actor.Location %d Rotation %d Velocity %d WorldInfo %d Pawn.Health %d NextPawn %d "
                     "Controller.Pawn %d WorldInfo.PawnList %d\n",
                     o.location, o.rotation, o.velocity, o.world_info, o.health, o.next_pawn, o.pawn, o.pawn_list);
      }
    }
    return o;
  }

  // " loc x y z rot p y r vel x y z hp n" (raw words: exact comparison).
  void AppendActor(const uint8_t *base, uint32_t actor, std::string &out)
  {
    const Offsets &o = GetOffsets(base);
    char buf[200];
    auto vec = [&](const char *what, int32_t off) {
      if (off >= 0)
      {
        std::snprintf(buf, sizeof(buf), " %s %08X %08X %08X", what, LoadU32(base, actor + off),
                      LoadU32(base, actor + off + 4), LoadU32(base, actor + off + 8));
        out += buf;
      }
    };
    vec("loc", o.location);
    vec("rot", o.rotation);
    vec("vel", o.velocity);
    if (o.health >= 0)
    {
      std::snprintf(buf, sizeof(buf), " hp %d", int32_t(LoadU32(base, actor + o.health)));
      out += buf;
    }
  }

  uint32_t g_any_pawn = 0; // a player's pawn of the last frame (AppendPawns)

  void AppendPlayers(PPCContext &ctx, uint8_t *base, std::string &out)
  {
    g_any_pawn = 0;
    char buf[256];
    for (uint32_t id = 0; id < 4; ++id)
    {
      uint32_t controller = ControllerOf(ctx, base, id);
      if (!Plausible(controller) || !Readable(base, controller, 2900))
      {
        continue;
      }
      uint32_t holder = LoadU32(base, controller + 2024);
      uint32_t look[2] = {};
      if (Plausible(holder) && Readable(base, holder + 3620, 12))
      {
        look[0] = LoadU32(base, holder + 3620);
        look[1] = LoadU32(base, holder + 3624);
      }
      std::snprintf(buf, sizeof(buf), " p%u look %08X %08X", id, look[0], look[1]);
      out += buf;
      const Offsets &o = GetOffsets(base);
      uint32_t pawn = o.pawn >= 0 ? LoadU32(base, controller + o.pawn) : 0;
      if (Plausible(pawn) && Readable(base, pawn, 1024))
      {
        out += " pawn " + ao2::ue3::ObjectName(base, pawn);
        g_any_pawn = pawn;
        AppendActor(base, pawn, out);
      }
      // The player's camera actor (controller +832, AAO2CameraActor; +460 =
      // FOV): what the AI may read of the player's view.
      uint32_t camera = LoadU32(base, controller + 832);
      if (Plausible(camera) && Readable(base, camera, 512))
      {
        char cam[160];
        std::snprintf(cam, sizeof(cam), " cam fov %08X", LoadU32(base, camera + 460));
        out += cam;
        if (o.location >= 0)
        {
          std::snprintf(cam, sizeof(cam), " cloc %08X %08X %08X", LoadU32(base, camera + o.location),
                        LoadU32(base, camera + o.location + 4), LoadU32(base, camera + o.location + 8));
          out += cam;
        }
        if (o.rotation >= 0)
        {
          std::snprintf(cam, sizeof(cam), " crot %08X %08X %08X", LoadU32(base, camera + o.rotation),
                        LoadU32(base, camera + o.rotation + 4), LoadU32(base, camera + o.rotation + 8));
          out += cam;
        }
      }
    }
  }

  // Raw memory of each pawn whose name has "iraqi", its mesh and controller
  // (record: u32 frame, u8 kind 0/1/2, char[48] name, u32 size, bytes; the
  // bytes as in guest memory, big endian).
  void DumpPawns(const uint8_t *base, uint32_t frame, uint32_t any_pawn)
  {
    static FILE *f = std::fopen("sync_dump.bin", "wb");
    const Offsets &o = GetOffsets(base);
    static int32_t mesh = -2, controller = -1;
    if (mesh == -2)
    {
      mesh = ao2::ue3::PropertyOffset(base, "Pawn", "Mesh");
      controller = ao2::ue3::PropertyOffset(base, "Pawn", "Controller");
    }
    if (!f || !o.found || !Plausible(any_pawn))
    {
      return;
    }
    uint32_t world_info = LoadU32(base, any_pawn + o.world_info);
    uint32_t pawn = Plausible(world_info) ? LoadU32(base, world_info + o.pawn_list) : 0;
    auto put = [&](uint8_t kind, const std::string &name, uint32_t addr, uint32_t size) {
      if (!Plausible(addr) || !Readable(base, addr, size))
      {
        return;
      }
      char n[48] = {};
      std::strncpy(n, name.c_str(), sizeof(n) - 1);
      std::fwrite(&frame, 4, 1, f);
      std::fwrite(&kind, 1, 1, f);
      std::fwrite(n, 1, sizeof(n), f);
      std::fwrite(&size, 4, 1, f);
      std::fwrite(base + addr, 1, size, f);
    };
    for (int i = 0; i < 512 && Plausible(pawn) && Readable(base, pawn, 1024); ++i)
    {
      std::string name = ao2::ue3::ObjectName(base, pawn);
      if (name.find("iraqi") != std::string::npos)
      {
        put(0, name, pawn, 4096);
        if (mesh >= 0)
        {
          put(1, name, LoadU32(base, pawn + mesh), 2048);
        }
        if (controller >= 0)
        {
          put(2, name, LoadU32(base, pawn + controller), 4096);
        }
      }
      pawn = LoadU32(base, pawn + o.next_pawn);
    }
  }

  // What this machine's rendering left in each pawn's skeletal mesh - its own
  // view, so it may differ between the machines; logged to see whether the
  // simulation follows it ("V <frame> name last <LastRenderTime> rr <0|1> lod <n>").
  // Floating things (AO2KActor): "K <frame> name r<0|1> phys<n>" whenever one
  // changes - r = drawn by this machine in the last second (Actor
  // LastRenderTime against WorldInfo.TimeSeconds), phys = Actor.Physics, role =
  // Actor.Role, tgn = Actor.TickGroupNum, then its BuoyancyComponent's
  // bAO2SkipTick / bAttached / TickGroup, and WorldInfo.MinTickGroupNum when it
  // changes (AO2's tick thinning, sub_827A6410). In
  // the hovercraft desync (2026-10-09) the buoyancy of five of them started
  // ticking on one machine only; this says whether that followed what each
  // machine drew. The list is rebuilt every 300 frames (a scan of every
  // object).
  void AppendFloating(const uint8_t *base, uint32_t any_pawn, uint32_t frame, std::string &out)
  {
    static uint32_t kactor_class = 0;
    static int32_t last_render = -2, physics = -1, world_info = -1, time_seconds = -1, role = -1;
    static int32_t tick_group_num = -1, min_tick_group = -1, buoyancy = -1, comp_tick_group = -1, skip_off = -1,
                   attached_off = -1;
    static uint32_t skip_mask = 0, attached_mask = 0;
    static std::vector<uint32_t> actors;
    static std::map<uint32_t, std::string> state; // actor -> its last description
    static std::string min_state;
    if (last_render == -2)
    {
      last_render = ao2::ue3::PropertyOffset(base, "Actor", "LastRenderTime");
      physics = ao2::ue3::PropertyOffset(base, "Actor", "Physics");
      world_info = ao2::ue3::PropertyOffset(base, "Actor", "WorldInfo");
      time_seconds = ao2::ue3::PropertyOffset(base, "WorldInfo", "TimeSeconds");
      role = ao2::ue3::PropertyOffset(base, "Actor", "Role");
      tick_group_num = ao2::ue3::PropertyOffset(base, "Actor", "TickGroupNum");
      min_tick_group = ao2::ue3::PropertyOffset(base, "WorldInfo", "MinTickGroupNum");
      buoyancy = ao2::ue3::PropertyOffset(base, "AO2KActor", "BuoyancyComponent");
      comp_tick_group = ao2::ue3::PropertyOffset(base, "ActorComponent", "TickGroup");
      ao2::ue3::BoolProperty(base, "ActorComponent", "bAO2SkipTick", skip_off, skip_mask);
      ao2::ue3::BoolProperty(base, "ActorComponent", "bAttached", attached_off, attached_mask);
    }
    if (!kactor_class)
    {
      kactor_class = ao2::ue3::FindClass(base, "AO2KActor");
    }
    if (!kactor_class || last_render < 0 || physics < 0 || world_info < 0 || time_seconds < 0 ||
        !Plausible(any_pawn))
    {
      return;
    }
    if (frame % 300 == 0 || actors.empty())
    {
      actors.clear();
      for (uint32_t i = 0, n = ao2::ue3::ObjectCount(base); i < n; ++i)
      {
        uint32_t o = ao2::ue3::ObjectAt(base, i);
        if (Plausible(o) && ao2::ue3::IsA(base, o, kactor_class))
        {
          actors.push_back(o);
        }
      }
    }
    uint32_t info = LoadU32(base, any_pawn + world_info);
    if (!Plausible(info) || !Readable(base, info + time_seconds, 4))
    {
      return;
    }
    float now = std::bit_cast<float>(LoadU32(base, info + time_seconds));
    if (min_tick_group >= 0)
    {
      std::string m = std::to_string(int32_t(LoadU32(base, info + min_tick_group)));
      if (m != min_state)
      {
        min_state = m;
        out += " MinTickGroupNum=" + m;
      }
    }
    for (uint32_t a : actors)
    {
      if (!Readable(base, a, uint32_t(std::max(last_render, physics)) + 4))
      {
        continue;
      }
      float last = std::bit_cast<float>(LoadU32(base, a + last_render));
      uint32_t r = now - last < 1.0f ? 1 : 0;
      uint32_t phys = base[a + physics];
      uint32_t net_role = role >= 0 ? base[a + role] : 0;
      std::string d = " r" + std::to_string(r) + " phys" + std::to_string(phys) + " role" + std::to_string(net_role);
      if (tick_group_num >= 0)
      {
        d += " tgn" + std::to_string(int32_t(LoadU32(base, a + tick_group_num)));
      }
      // Its buoyancy component: skip / attached flags and tick group.
      uint32_t comp = buoyancy >= 0 ? LoadU32(base, a + buoyancy) : 0;
      if (Plausible(comp) && Readable(base, comp, 96))
      {
        uint32_t flags = skip_off >= 0 ? LoadU32(base, comp + skip_off) : 0;
        d += std::string(" skip") + ((skip_mask && flags & skip_mask) ? "1" : "0");
        d += std::string(" att") + ((attached_mask && flags & attached_mask) ? "1" : "0");
        if (comp_tick_group >= 0)
        {
          d += " tg" + std::to_string(base[comp + comp_tick_group]);
        }
      }
      auto it = state.find(a);
      if (it == state.end() || it->second != d)
      {
        state[a] = d;
        out += " " + ao2::ue3::ObjectName(base, a) + d;
      }
    }
  }

  // Streaming levels: "L <frame> package sv<should be visible> v<visible>
  // ld<loaded> pend<load pending>" whenever one changes (coop_streaming.cpp:
  // the floating actors' components attached on different frames).
  void AppendStreaming(const uint8_t *base, std::string &out)
  {
    static int32_t levels = -2, package = -1, loaded = -1, flags = -1;
    static uint32_t sv_mask = 0, v_mask = 0, pend_mask = 0;
    static std::map<uint32_t, std::string> state;
    if (levels == -2)
    {
      levels = ao2::ue3::PropertyOffset(base, "WorldInfo", "StreamingLevels");
      package = ao2::ue3::PropertyOffset(base, "LevelStreaming", "PackageName");
      loaded = ao2::ue3::PropertyOffset(base, "LevelStreaming", "LoadedLevel");
      ao2::ue3::BoolProperty(base, "LevelStreaming", "bShouldBeVisible", flags, sv_mask);
      int32_t unused = -1;
      ao2::ue3::BoolProperty(base, "LevelStreaming", "bIsVisible", unused, v_mask);
      ao2::ue3::BoolProperty(base, "LevelStreaming", "bHasLoadRequestPending", unused, pend_mask);
    }
    if (levels < 0 || package < 0 || loaded < 0 || flags < 0)
    {
      return;
    }
    uint32_t world = LoadU32(base, 0x831F2CC8); // GWorld; +80 persistent level, +140 actors (WorldInfo first)
    uint32_t level = Plausible(world) ? LoadU32(base, world + 80) : 0;
    uint32_t actors = Plausible(level) ? LoadU32(base, level + 140) : 0;
    uint32_t info = Plausible(actors) ? LoadU32(base, actors) : 0;
    if (!Plausible(info) || !Readable(base, info + levels, 8))
    {
      return;
    }
    uint32_t data = LoadU32(base, info + levels), count = LoadU32(base, info + levels + 4);
    for (uint32_t i = 0; Plausible(data) && i < count && i < 256; ++i)
    {
      uint32_t s = LoadU32(base, data + i * 4);
      if (!Plausible(s) || !Readable(base, s, uint32_t(flags) + 4))
      {
        continue;
      }
      uint32_t f = LoadU32(base, s + flags);
      std::string d = std::string(" sv") + (f & sv_mask ? "1" : "0") + " v" + (f & v_mask ? "1" : "0") + " ld" +
                      (LoadU32(base, s + loaded) ? "1" : "0") + " pend" + (f & pend_mask ? "1" : "0");
      auto it = state.find(s);
      if (it == state.end() || it->second != d)
      {
        state[s] = d;
        out += " " + ao2::ue3::Name(base, LoadU32(base, s + package)) + d;
      }
    }
  }

  void AppendVisibility(const uint8_t *base, uint32_t any_pawn, std::string &out)
  {
    const Offsets &o = GetOffsets(base);
    static int32_t mesh = -2, last_render = -1, lod = -1, rr_off = -1;
    static uint32_t rr_mask = 0;
    if (mesh == -2)
    {
      mesh = ao2::ue3::PropertyOffset(base, "Pawn", "Mesh");
      last_render = ao2::ue3::PropertyOffset(base, "PrimitiveComponent", "LastRenderTime");
      lod = ao2::ue3::PropertyOffset(base, "SkeletalMeshComponent", "PredictedLODLevel");
      ao2::ue3::BoolProperty(base, "SkeletalMeshComponent", "bRecentlyRendered", rr_off, rr_mask);
      if (FILE *f = Log())
      {
        std::fprintf(f, "# offsets: Pawn.Mesh %d PrimitiveComponent.LastRenderTime %d PredictedLODLevel %d "
                        "bRecentlyRendered %d/%08X\n",
                     mesh, last_render, lod, rr_off, rr_mask);
      }
    }
    if (!o.found || mesh < 0 || !Plausible(any_pawn))
    {
      return;
    }
    uint32_t world_info = LoadU32(base, any_pawn + o.world_info);
    if (!Plausible(world_info) || !Readable(base, world_info + o.pawn_list, 4))
    {
      return;
    }
    std::map<std::string, uint32_t> pawns;
    uint32_t pawn = LoadU32(base, world_info + o.pawn_list);
    for (int i = 0; i < 512 && Plausible(pawn) && Readable(base, pawn, 1024); ++i)
    {
      pawns[ao2::ue3::ObjectName(base, pawn)] = pawn;
      pawn = LoadU32(base, pawn + o.next_pawn);
    }
    char buf[160];
    for (const auto &[name, p] : pawns)
    {
      uint32_t m = LoadU32(base, p + mesh);
      if (!Plausible(m) || !Readable(base, m, 1024))
      {
        continue;
      }
      std::snprintf(buf, sizeof(buf), " %s last %08X rr %d lod %d", name.c_str(),
                    last_render >= 0 ? LoadU32(base, m + last_render) : 0,
                    rr_off >= 0 ? (LoadU32(base, m + rr_off) & rr_mask) != 0 : -1,
                    lod >= 0 ? int32_t(LoadU32(base, m + lod)) : -1);
      out += buf;
    }
  }

  // Every pawn (enemies too), by name, from WorldInfo.PawnList (a player's
  // pawn gives the WorldInfo): "P <frame> name loc .. rot .. vel .. hp ..".
  void AppendPawns(const uint8_t *base, uint32_t any_pawn, std::string &out)
  {
    const Offsets &o = GetOffsets(base);
    if (!o.found || !Plausible(any_pawn))
    {
      return;
    }
    uint32_t world_info = LoadU32(base, any_pawn + o.world_info);
    if (!Plausible(world_info) || !Readable(base, world_info + o.pawn_list, 4))
    {
      return;
    }
    std::map<std::string, uint32_t> pawns;
    uint32_t pawn = LoadU32(base, world_info + o.pawn_list);
    for (int i = 0; i < 512 && Plausible(pawn) && Readable(base, pawn, 1024); ++i)
    {
      pawns[ao2::ue3::ObjectName(base, pawn)] = pawn;
      pawn = LoadU32(base, pawn + o.next_pawn);
    }
    for (const auto &[name, p] : pawns)
    {
      out += " " + name;
      AppendActor(base, p, out);
    }
  }
} // namespace

// rand(): the shared stream on the game thread while the lockstep runs.
extern "C" REX_FUNC(__imp__sub_823EAF28);
extern "C" REX_FUNC(sub_823EAF28)
{
  if (!On())
  {
    __imp__sub_823EAF28(ctx, base);
    return;
  }
  uint32_t lr = uint32_t(ctx.lr);
  // The shared state, or this thread's own one ([[r13] + 4]) - which one
  // rand() takes depends on [0x831B7558] and the thread.
  uint32_t tls = ctx.r13.u32 ? LoadU32(base, ctx.r13.u32) : 0;
  uint32_t before = LoadU32(base, kRandState), tls_before = tls ? LoadU32(base, tls + 4) : 0;
  __imp__sub_823EAF28(ctx, base);
  if (!t_game_thread || (LoadU32(base, kRandState) == before && (!tls || LoadU32(base, tls + 4) == tls_before)))
  {
    return; // another thread's stream
  }
  if (ao2::decouple::InExtraDraw())
  {
    g_frame.draw_calls++;
    return;
  }
  g_frame.calls++;
  char key[40];
  std::snprintf(key, sizeof(key), "%08X", lr);
  if (!t_script_frame)
  {
    // A native caller: also the two return addresses above it (each saved
    // at -8 from its caller's stack pointer), for wrappers such as
    // appFRand (sub_823EB010) that many callers share. A water desync
    // (2026-10-09, hovercraft) took 22 / 32 numbers through it.
    uint32_t sp = ctx.r1.u32;
    for (int depth = 0; depth < 2; ++depth)
    {
      uint32_t back = Plausible(sp) && Readable(base, sp, 4) ? LoadU32(base, sp) : 0;
      if (!Plausible(back) || back <= sp || !Readable(base, back - 8, 8))
      {
        break;
      }
      uint32_t ret = LoadU32(base, back - 8);
      if (ret < 0x82000000 || ret >= 0x84000000)
      {
        break; // not a return address (a leaf caller keeps no frame)
      }
      size_t len = std::strlen(key);
      std::snprintf(key + len, sizeof(key) - len, "<%08X", ret);
      sp = back;
    }
  }
  std::string site = t_script_frame ? ScriptSite(base, t_script_frame) + "/" + key : key;
  g_frame.sites[site]++;
}

// UnrealScript Rand() / FRand() natives (r3 = object, r4 = FFrame).
extern "C" REX_FUNC(__imp__sub_8240C5D8);
extern "C" REX_FUNC(sub_8240C5D8)
{
  uint32_t outer = t_script_frame;
  t_script_frame = ctx.r4.u32;
  __imp__sub_8240C5D8(ctx, base);
  t_script_frame = outer;
}
extern "C" REX_FUNC(__imp__sub_8240DAD8);
extern "C" REX_FUNC(sub_8240DAD8)
{
  uint32_t outer = t_script_frame;
  t_script_frame = ctx.r4.u32;
  __imp__sub_8240DAD8(ctx, base);
  t_script_frame = outer;
}

// The frame sync builds this machine's record of a frame (r3 = sync, r4 =
// frame) with the checksum at +44 / +46; again while waiting (same frame).
extern "C" REX_FUNC(__imp__sub_82A2D708);
extern "C" REX_FUNC(sub_82A2D708)
{
  uint32_t sync = ctx.r3.u32, frame = ctx.r4.u32;
  if (sync && sync == LoadU32(base, kFrameSync))
  {
    ao2::coop::StateHashFrame(base, frame);
  }
  if (On() && sync && sync == LoadU32(base, kFrameSync) && frame != g_last_frame && ao2::coop::OnlineLockstep(base))
  {
    g_last_frame = frame;
    t_game_thread = true;
    uint32_t tls = ctx.r13.u32 ? LoadU32(base, ctx.r13.u32) : 0;
    g_thread_rand_state = tls ? tls + 4 : 0;
    if (FILE *f = Log())
    {
      std::string players;
      AppendPlayers(ctx, base, players);
      std::fprintf(f, "F %u cs %04X %04X rand %08X/%08X calls %u draw %u |%s\n", frame, LoadU16(base, sync + 44),
                   LoadU16(base, sync + 46), LoadU32(base, kRandState),
                   g_thread_rand_state ? LoadU32(base, g_thread_rand_state) : 0, g_frame.calls, g_frame.draw_calls,
                   players.c_str());
      std::string sites;
      for (const auto &[site, count] : g_frame.sites)
      {
        sites += " " + site + "=" + std::to_string(count);
      }
      std::fprintf(f, "R %u%s\n", frame, sites.c_str());
      // The second generator (appSRand, seed at kSRandSeed): not part of the
      // checksum. "S <frame> seed <state> caller=count ..." (game thread).
      std::string srand_sites;
      for (const auto &[site, count] : g_frame.srand_sites)
      {
        srand_sites += " " + site + "=" + std::to_string(count);
      }
      std::fprintf(f, "S %u seed %08X%s\n", frame, LoadU32(base, kSRandSeed), srand_sites.c_str());
      std::string pawns;
      AppendPawns(base, g_any_pawn, pawns);
      std::fprintf(f, "P %u%s\n", frame, pawns.c_str());
      std::string seen;
      AppendVisibility(base, g_any_pawn, seen);
      std::fprintf(f, "V %u%s\n", frame, seen.c_str());
      std::string floating;
      AppendFloating(base, g_any_pawn, frame, floating);
      if (!floating.empty())
      {
        std::fprintf(f, "K %u%s\n", frame, floating.c_str());
      }
      std::string streaming;
      AppendStreaming(base, streaming);
      if (!streaming.empty())
      {
        std::fprintf(f, "L %u%s\n", frame, streaming.c_str());
      }
      if (int32_t every = REXCVAR_GET(ao2_coop_sync_dump); every > 0 && frame % uint32_t(every) == 0)
      {
        DumpPawns(base, frame, g_any_pawn);
      }
      if (frame % 60 == 0)
      {
        std::fflush(f);
      }
    }
    g_frame = FrameData{};
  }
  __imp__sub_82A2D708(ctx, base);
}

// UnrealScript Destroy() native (r3 = actor, r4 = FFrame): which actors go
// away on which lockstep frame, and the script that did it (a corpse removed
// on one machine only split the pawns first, 2026-10-07). "D <frame> <actor>
// <function@class> < <caller>...".
extern "C" REX_FUNC(__imp__sub_828967D0);
extern "C" REX_FUNC(sub_828967D0)
{
  if (On() && t_game_thread && ao2::coop::OnlineLockstep(base))
  {
    if (FILE *f = Log())
    {
      // The script calls that led here: function@class < caller < ...
      std::string chain;
      uint32_t frame = ctx.r4.u32;
      for (int depth = 0; depth < 8 && Plausible(frame) && Readable(base, frame, 28); ++depth)
      {
        chain += (depth ? " < " : " ") + ScriptSite(base, frame);
        frame = LoadU32(base, frame + 24);
      }
      std::fprintf(f, "D %u %s%s\n", g_last_frame + 1, ao2::ue3::ObjectName(base, ctx.r3.u32).c_str(),
                   chain.c_str());
    }
  }
  __imp__sub_828967D0(ctx, base);
}

// A buoyancy component's tick (r3 = component, +76 = owner, f1 = dt): the
// hovercraft desync (2026-10-09) ran its wave force (sub_823AC368, two
// appFRand) 13 times per frame on one machine and 18 on the other - a
// different set of floating things ticked. Counted per owner under the rand()
// sites as "buoy:<owner>", so sync_diff.py names the ones that differ.
extern "C" REX_FUNC(__imp__sub_823BB2F8);
extern "C" REX_FUNC(sub_823BB2F8)
{
  if (On() && t_game_thread && ao2::coop::OnlineLockstep(base))
  {
    uint32_t owner = LoadU32(base, ctx.r3.u32 + 76);
    // Where the tick came from: its return address and the one above it -
    // the extra ticks (one owner new, four twice per frame) were all on the
    // partner's machine, and nothing about drawing or physics differed.
    char from[32];
    uint32_t lr = uint32_t(ctx.lr), up = 0;
    uint32_t back = Plausible(ctx.r1.u32) && Readable(base, ctx.r1.u32, 4) ? LoadU32(base, ctx.r1.u32) : 0;
    if (Plausible(back) && Readable(base, back - 8, 8))
    {
      up = LoadU32(base, back - 8);
      up = up >= 0x82000000 && up < 0x84000000 ? up : 0;
    }
    std::snprintf(from, sizeof(from), "<%08X<%08X", lr, up);
    g_frame.sites["buoy:" + (Plausible(owner) ? ao2::ue3::ObjectName(base, owner) : std::string("?")) + from]++;
  }
  __imp__sub_823BB2F8(ctx, base);
}

// Two native rand() users that ran on one machine only in the hovercraft
// desync (2026-10-09, frames 8122-8123): counted per object under the rand()
// sites, so sync_diff.py names them.
// sub_823BCA80 (r3 = component, r4 = owner actor, f1 = dt; 6 appFRand),
// reached through a vtable from sub_8289D178.
extern "C" REX_FUNC(__imp__sub_823BCA80);
extern "C" REX_FUNC(sub_823BCA80)
{
  if (On() && t_game_thread && ao2::coop::OnlineLockstep(base))
  {
    uint32_t owner = ctx.r4.u32;
    g_frame.sites["fx823BCA80:" + (Plausible(owner) ? ao2::ue3::ObjectName(base, owner) : std::string("?"))]++;
  }
  __imp__sub_823BCA80(ctx, base);
}
// sub_823577E0 (r3 = owner, r4 = the new entry; its sub_82342D60 draws 7
// appFRand): what looks like a camera shake starting.
extern "C" REX_FUNC(__imp__sub_823577E0);
extern "C" REX_FUNC(sub_823577E0)
{
  if (On() && t_game_thread && ao2::coop::OnlineLockstep(base))
  {
    uint32_t owner = ctx.r3.u32;
    g_frame.sites["shake:" + (Plausible(owner) ? ao2::ue3::ObjectName(base, owner) : std::string("?"))]++;
  }
  __imp__sub_823577E0(ctx, base);
}

// appSRand, the engine's second generator (seed at kSRandSeed; sub_823EAFD0
// sets it, sub_823EAFE0 gives an int, sub_823EB050 a float): the lockstep
// seeds it (sub_82A28DF0) but its state is in no checksum, and bullet impact
// decals and effects - made for what this machine sees - draw from it, so the
// two machines' streams differ (2026-10-10: different seeds on the same
// frame). Counted per caller (and the two return addresses above) on the
// game thread, to see whether the simulation draws from it too.
namespace
{
  void NoteSRand(const PPCContext &ctx, const uint8_t *base, const char *what)
  {
    if (!On() || !t_game_thread || !ao2::coop::OnlineLockstep(base))
    {
      return;
    }
    char key[48];
    std::snprintf(key, sizeof(key), "%s%08X", what, uint32_t(ctx.lr));
    uint32_t sp = ctx.r1.u32;
    for (int depth = 0; depth < 2; ++depth)
    {
      uint32_t back = Plausible(sp) && Readable(base, sp, 4) ? LoadU32(base, sp) : 0;
      if (!Plausible(back) || back <= sp || !Readable(base, back - 8, 8))
      {
        break;
      }
      uint32_t ret = LoadU32(base, back - 8);
      if (ret < 0x82000000 || ret >= 0x84000000)
      {
        break;
      }
      size_t len = std::strlen(key);
      std::snprintf(key + len, sizeof(key) - len, "<%08X", ret);
      sp = back;
    }
    g_frame.srand_sites[ao2::decouple::InExtraDraw() ? std::string("draw:") + key : std::string(key)]++;
  }
} // namespace
extern "C" REX_FUNC(__imp__sub_823EAFD0);
extern "C" REX_FUNC(sub_823EAFD0)
{
  NoteSRand(ctx, base, "init:");
  __imp__sub_823EAFD0(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_823EAFE0);
extern "C" REX_FUNC(sub_823EAFE0)
{
  NoteSRand(ctx, base, "");
  __imp__sub_823EAFE0(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_823EB050);
extern "C" REX_FUNC(sub_823EB050)
{
  NoteSRand(ctx, base, "");
  __imp__sub_823EB050(ctx, base);
}
