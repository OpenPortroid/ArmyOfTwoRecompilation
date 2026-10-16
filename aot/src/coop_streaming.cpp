// coop_streaming.cpp - Streaming levels appear on the same frame on both
// online co-op machines.
//
// A hovercraft desync in China (2026-10-09, six sync traces): the buoyancy
// components of five floating AO2KActors (AO2KActor_13, _2, _5, _8, _9)
// attached a few frames apart on the two machines. Attached, they enter the
// component tick list (sub_827A6410; a component ticks twice on the frame it
// attaches), their buoyancy wave (sub_823AC368) draws appFRand from the
// lockstep's rand() stream, and the streams split. The sync trace's L lines
// showed why: the streaming level 07_map_river_b_Shell was asked for and
// finished loading on the same frames on both (9023, 9074 - the lockstep
// keeps those together), but became visible on frame 9078 on one machine and
// 9084 on the other: UE3 adds a loaded level to the world a slice of
// components at a time against a real time budget.
//
// While a network game's lockstep runs and a streaming level is loaded and
// wanted visible but not visible yet, the world gets
// bRequestedBlockOnAsyncLoading for that step: the engine then adds the
// level whole, on the step it was loaded. Nothing is asked to block on its
// load (marking every level bShouldBlockOnLoad hung entering China online:
// the lockstep stood at frame -1).
//
// Off by default: even this way the level start stopped at lockstep frame 68
// on both machines, 07_map_sound's load never finishing (2026-10-09; it loads
// on frame 70 without the flag).
#include "coop.h"
#include "log_files.h"
#include "ue3_reflect.h"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(ao2_coop_block_streaming, false, "Online",
                    "Online co-op: a loaded streaming level is added to the world whole on one step, on both machines "
                    "(time-sliced, it appeared on different frames: desync)");

namespace
{
  constexpr uint32_t kWorld = 0x831F2CC8; // GWorld; +80 persistent level, its +140 actors (WorldInfo first)

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
  bool Plausible(uint32_t p)
  {
    return p >= 0x40000000 && p < 0x80000000 && !(p & 3);
  }
} // namespace

namespace ao2::coop
{
  // Once per simulation step on the game thread (engine pad poll).
  void BlockStreaming(PPCContext &, uint8_t *base)
  {
    // A network session as well as the lockstep's mode: at startup the frame
    // sync object reads as mode 1 before any game.
    if (!REXCVAR_GET(ao2_coop_block_streaming) || !network_session.load(std::memory_order_relaxed) ||
        !OnlineLockstep(base))
    {
      return;
    }
    static uint32_t info_class = 0;
    static int32_t streaming_levels = -2, loaded = -1, flags = -1, request_off = -1;
    static uint32_t want_mask = 0, visible_mask = 0, request_mask = 0;
    if (streaming_levels == -2 || !info_class)
    {
      info_class = ao2::ue3::FindClass(base, "WorldInfo");
      streaming_levels = ao2::ue3::PropertyOffset(base, "WorldInfo", "StreamingLevels");
      loaded = ao2::ue3::PropertyOffset(base, "LevelStreaming", "LoadedLevel");
      int32_t visible_off = -1;
      ao2::ue3::BoolProperty(base, "LevelStreaming", "bShouldBeVisible", flags, want_mask);
      ao2::ue3::BoolProperty(base, "LevelStreaming", "bIsVisible", visible_off, visible_mask);
      ao2::ue3::BoolProperty(base, "WorldInfo", "bRequestedBlockOnAsyncLoading", request_off, request_mask);
      if (visible_off != flags)
      {
        visible_mask = 0; // not in the same word: leave it alone
      }
      if (FILE *f = ao2::OpenLogFile("coop_streaming.log"))
      {
        std::fprintf(f, "WorldInfo %08X StreamingLevels %d LoadedLevel %d flags %d want %08X visible %08X "
                        "bRequestedBlockOnAsyncLoading %d/%08X\n",
                     info_class, streaming_levels, loaded, flags, want_mask, visible_mask, request_off, request_mask);
        std::fclose(f);
      }
    }
    if (!info_class || streaming_levels < 0 || loaded < 0 || flags < 0 || !want_mask || !visible_mask ||
        request_off < 0 || !request_mask)
    {
      return;
    }
    uint32_t world = LoadU32(base, kWorld);
    uint32_t level = Plausible(world) ? LoadU32(base, world + 80) : 0;
    uint32_t actors = Plausible(level) ? LoadU32(base, level + 140) : 0;
    uint32_t info = Plausible(actors) ? LoadU32(base, actors) : 0;
    if (!Plausible(info) || !ao2::ue3::IsA(base, info, info_class))
    {
      return;
    }
    uint32_t data = LoadU32(base, info + streaming_levels);
    uint32_t count = LoadU32(base, info + streaming_levels + 4);
    bool appearing = false;
    for (uint32_t i = 0; Plausible(data) && i < count && i < 256 && !appearing; ++i)
    {
      uint32_t streaming = LoadU32(base, data + i * 4);
      if (!Plausible(streaming))
      {
        continue;
      }
      uint32_t f = LoadU32(base, streaming + flags);
      appearing = (f & want_mask) && !(f & visible_mask) && LoadU32(base, streaming + loaded);
    }
    uint32_t v = LoadU32(base, info + request_off);
    if (appearing && !(v & request_mask))
    {
      StoreU32(base, info + request_off, v | request_mask);
    }
  }
} // namespace ao2::coop

// UWorld::AddToWorld (sub_82984A40: r3 = world, r4 = LevelStreaming, r5 =
// offset delta; called by UpdateLevelStreaming sub_829B4EE8 while the level
// is loaded and wanted visible but not visible). It works in steps - move the
// actors, update the actors' components against a time budget (sub_827C4798,
// the budget in f1), the BSP components 50 at a time against a cost budget,
// then the rest - and returns after a step that ran out; bIsVisible is set
// only when the last one is done. The time budget is what lets the same level
// appear on different frames on the two machines (07_map_river_b_Shell 9078 /
// 9084, 07_map_canal_lock_m 9977 / 9973 in 2026-10-09 traces) and with it
// what the level's actors do on their first frames (rand(): desync).
//
// While a network game's lockstep runs it is called again until the level is
// visible, the way the engine's own FlushLevelStreaming loops on
// UpdateLevelStreaming: the level appears whole on the step its load finished
// (which the lockstep already keeps the same on both machines). A longer
// frame then, instead of a few short ones. Nothing touches
// bRequestedBlockOnAsyncLoading (the engine takes it for a pause).
REXCVAR_DEFINE_BOOL(ao2_coop_whole_level_visibility, true, "Online",
                    "Online co-op: a loaded streaming level is added to the world in one frame (time-sliced, it "
                    "appeared on different frames on the two machines: desync)");

extern "C" REX_FUNC(__imp__sub_82984A40);
extern "C" REX_FUNC(sub_82984A40)
{
  if (!REXCVAR_GET(ao2_coop_whole_level_visibility) || !ao2::coop::network_session.load(std::memory_order_relaxed) ||
      !ao2::coop::OnlineLockstep(base))
  {
    __imp__sub_82984A40(ctx, base);
    return;
  }
  constexpr uint32_t kFlags = 96;               // LevelStreaming flags word
  constexpr uint32_t kIsVisible = 0x80000000;   // bIsVisible (written at the end of AddToWorld)
  constexpr uint32_t kLoadedLevel = 68;         // LevelStreaming.LoadedLevel
  constexpr uint32_t kPendingVisibility = 88;   // UWorld: the level being made visible
  const uint32_t world = ctx.r3.u32, streaming = ctx.r4.u32, offset = ctx.r5.u32;
  for (int i = 0; i < 4096; ++i)
  {
    ctx.r3.u32 = world;
    ctx.r4.u32 = streaming;
    ctx.r5.u32 = offset;
    __imp__sub_82984A40(ctx, base);
    if (!Plausible(world) || !Plausible(streaming) || (LoadU32(base, streaming + kFlags) & kIsVisible))
    {
      break;
    }
    // Another level is still being made visible: AddToWorld does nothing for
    // this one until that is done.
    uint32_t pending = LoadU32(base, world + kPendingVisibility);
    if (pending && pending != LoadU32(base, streaming + kLoadedLevel))
    {
      break;
    }
  }
}

// The buoyancy wave (sub_823AC368: r3 = AO2BuoyancyComponent, r5 = owner;
// two appFRand). Written before the AddToWorld loop above, when both ways of
// forcing the level to appear on one frame had stalled the level load, and
// kept as a second guard: the wave is kept off the lockstep's rand() stream
// instead: while a network game's
// lockstep runs, the game thread's rand() state ([[r13] + 4]) and the shared
// one are put back after it. It still gets random numbers - the same on both
// machines for the same stream position - but the floating things that
// appeared a few frames earlier on one machine no longer move the stream
// (2026-10-09 trace: only the rand() half of the checksum split, the physics
// half stayed equal through the frames the level was visible on one machine
// only).
REXCVAR_DEFINE_BOOL(ao2_coop_isolate_buoyancy_rand, true, "Online",
                    "Online co-op: the buoyancy wave of floating objects does not take numbers from the lockstep's "
                    "random stream (a streaming level appearing on different frames split it: desync)");

extern "C" REX_FUNC(__imp__sub_823AC368);
extern "C" REX_FUNC(sub_823AC368)
{
  if (!REXCVAR_GET(ao2_coop_isolate_buoyancy_rand) || !ao2::coop::network_session.load(std::memory_order_relaxed) ||
      !ao2::coop::OnlineLockstep(base))
  {
    __imp__sub_823AC368(ctx, base);
    return;
  }
  constexpr uint32_t kSharedRandState = 0x8317BA6C;
  uint32_t tls = ctx.r13.u32 ? LoadU32(base, ctx.r13.u32) : 0;
  uint32_t thread_state = tls ? tls + 4 : 0; // TLS blocks are below the heaps (0x3xxxxxxx)
  uint32_t saved_thread = thread_state ? LoadU32(base, thread_state) : 0;
  uint32_t saved_shared = LoadU32(base, kSharedRandState);
  __imp__sub_823AC368(ctx, base);
  if (thread_state)
  {
    StoreU32(base, thread_state, saved_thread);
  }
  StoreU32(base, kSharedRandState, saved_shared);
}
