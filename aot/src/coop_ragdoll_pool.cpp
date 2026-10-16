// coop_ragdoll_pool.cpp - Which corpse goes at the corpse limit, out of this
// machine's view in online co-op.
//
// AO2GameObjectPool keeps the ragdolls (corpses) of dead pawns
// (RagdollPool_MaxRagdolls = 4 in coalesced.ini, AO2Game.AO2GameObjectPool).
// The pool's array is at +1136 (count +1140), 28-byte entries:
//   +0 actor, +4 time not seen, +8 age, +12 age / MaxDuration,
//   +16 distance to the nearest player / MaxDistanceSq, +20 time not seen /
//   MaxDuration, +24 bit 31 = not seen this tick.
// Adding a ragdoll (sub_822D35F0) over the limit destroys the first entries
// (sub_822A5CC0). The pool tick (sub_822D36B8, r3 = pool, r5 = views, f1 =
// dt) updates the entries and sorts them (sub_82309310) by
//   not seen + distance + time not seen + age
// - the most removable first. "Seen" is a frustum test of the ragdoll mesh
// bounds against each of this machine's views (r5: array of 192-byte view
// entries), so at the limit each machine removed a corpse it was not looking
// at: different corpses on the same lockstep frame (2026-10-07, A grunt_6 /
// B grunt_7 on frame 8779). In multiplayer (GameInfo +456 bit 31; +452 in the disc version) the game
// skips the test itself; co-op is not that mode.
//
// While the lockstep runs the tick gets an empty view list on both machines:
// no corpse counts as seen, the order is by age and distance to the players
// (both simulation state), and only the first tick of a lockstep frame ages
// them. Single player is unchanged.
//
// Two-instance run (2026-10-08, Iraq, 9585 frames, 7 removals at the limit):
// the same corpses removed on the same frames, no checksum or pawn split.
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "coop.h"
#include "log_files.h"
#include "ue3_reflect.h"

REXCVAR_DEFINE_BOOL(ao2_coop_ragdoll_pool_fix, true, "Online",
                    "Online co-op: the corpse limit picks the corpse to remove by age and distance only, not by "
                    "what this machine's camera sees (desync)");
REXCVAR_DEFINE_BOOL(ao2_coop_ragdoll_log, false, "Online",
                    "Online co-op: log the corpse pool (removals, order) to coop_ragdoll.log");

namespace
{
  constexpr uint32_t kPoolArray = 1136; // TArray<entry>: data, count
  constexpr uint32_t kEntrySize = 28;

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
    return p >= 0x40000000 && p < 0xA0000000;
  }

  FILE *Log(const uint8_t *base)
  {
    if (!REXCVAR_GET(ao2_coop_ragdoll_log) || !ao2::coop::OnlineLockstep(base))
    {
      return nullptr;
    }
    static FILE *f = ao2::OpenLogFile("coop_ragdoll.log");
    return f;
  }

  // "name(age,dist,unseen,flag) ..." for the pool's entries in array order.
  void LogPool(const uint8_t *base, FILE *f, const char *what, uint32_t pool)
  {
    uint32_t data = LoadU32(base, pool + kPoolArray), count = LoadU32(base, pool + kPoolArray + 4);
    std::fprintf(f, "%s %u n=%u", what, ao2::coop::SyncFrame(base), count);
    for (uint32_t i = 0; i < count && i < 64 && Plausible(data); ++i)
    {
      uint32_t e = data + i * kEntrySize, actor = LoadU32(base, e);
      std::fprintf(f, " %s(%.3f,%.3f,%.3f,%u)", Plausible(actor) ? ao2::ue3::ObjectName(base, actor).c_str() : "-",
                   LoadF32(base, e + 12), LoadF32(base, e + 16), LoadF32(base, e + 20), LoadU32(base, e + 24) >> 31);
    }
    std::fprintf(f, "\n");
    std::fflush(f);
  }
} // namespace

// Pool tick (r3 = pool, r5 = this machine's views, f1 = dt).
extern "C" REX_FUNC(__imp__sub_822D36B8);
extern "C" REX_FUNC(sub_822D36B8)
{
  if (!REXCVAR_GET(ao2_coop_ragdoll_pool_fix) || !ao2::coop::OnlineLockstep(base))
  {
    __imp__sub_822D36B8(ctx, base);
    return;
  }
  uint32_t pool = ctx.r3.u32;
  // An empty TArray in a frame of our own on the guest stack (the callee
  // saves registers below its incoming r1, the array is above it). Not in
  // the frame's first 128 bytes: a callee may spill its argument registers
  // into its caller's parameter save area (std r4,24(r1)) - r7/r8 would have
  // landed on an array at +48.
  uint32_t sp = ctx.r1.u32;
  uint32_t frame = sp - 256;
  uint32_t array = frame + 192;
  StoreU32(base, frame, sp); // back chain
  StoreU32(base, array, 0);
  StoreU32(base, array + 4, 0);
  StoreU32(base, array + 8, 0);
  ctx.r1.u64 = frame;
  ctx.r5.u64 = array;
  // While the lockstep frame stands still the pool still ticked, a different
  // number of times per machine (frame 1598: 53 + 26 ticks on one, 134 on
  // the other, 2026-10-08): those ticks do not age the ragdolls.
  static uint32_t aged_frame = ~0u;
  uint32_t now = ao2::coop::SyncFrame(base);
  double dt = ctx.f1.f64;
  if (now == aged_frame)
  {
    ctx.f1.f64 = 0.0;
  }
  aged_frame = now;
  __imp__sub_822D36B8(ctx, base);
  ctx.f1.f64 = dt;
  ctx.r1.u64 = sp;
  if (FILE *f = Log(base))
  {
    // Ticks per lockstep frame other than one (the ages would add up
    // differently if the tick ran with this machine's drawing).
    static uint32_t tick_frame = 0, ticks = 0, odd = 0;
    uint32_t now = ao2::coop::SyncFrame(base);
    if (now != tick_frame)
    {
      if (ticks != 1 && tick_frame && odd < 50)
      {
        ++odd;
        std::fprintf(f, "ticks frame %u: %u\n", tick_frame, ticks);
      }
      tick_frame = now;
      ticks = 0;
    }
    ++ticks;
    static uint32_t last_count = ~0u;
    uint32_t count = LoadU32(base, pool + kPoolArray + 4);
    if (count != last_count)
    {
      last_count = count;
      LogPool(base, f, "T", pool);
    }
  }
}

// Remove the first n entries (r3 = pool, r4 = n): the corpse limit.
extern "C" REX_FUNC(__imp__sub_822A5CC0);
extern "C" REX_FUNC(sub_822A5CC0)
{
  if (FILE *f = Log(base))
  {
    char what[32];
    std::snprintf(what, sizeof(what), "R%u", ctx.r4.u32);
    LogPool(base, f, what, ctx.r3.u32);
  }
  __imp__sub_822A5CC0(ctx, base);
}
