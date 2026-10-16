// coop_net_pump.cpp - Send the co-op lockstep input as soon as it is made.
//
// The game's peer-to-peer traffic (VDP UDP 1000) goes through DirtySock's
// ProtoTunnel ('ptun', created by sub_830E79C0): sends into the tunnel are
// buffered, and ProtoTunnelUpdate (sub_830E72A0, r3 = tunnel manager) puts
// them on the wire when NetTick() - [+32] last flush >= [+28] rate (0 here),
// i.e. on every update. The update is pumped through a vtable
// (sub_8224ECE0 -> sub_830E72A0) - measured ~33 packets/s each way at any
// frame rate, so a frame's input waited up to ~30 ms for the next pump.
// With the lead the lockstep allows (2 frames = 17 ms at 120 FPS on a LAN,
// coop_input_delay.cpp) both games kept stopping for each other's late input:
// 25-35 frames a second waited, 20-45 ms at a time (the stutter).
//
// ao2_coop_flush_tunnel: right after the frame sync tick (which made this
// frame's input record) flush the tunnel on the game thread. Thread safety
// is ProtoTunnel's own (its flush runs under the manager's two critical
// sections). This only changes when packets leave, not what is simulated.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "coop.h"

REXCVAR_DEFINE_BOOL(ao2_coop_flush_tunnel, true, "Online",
                    "Online co-op: send this frame's input right after the frame sync tick instead of on the "
                    "network pump (~30 times a second) - fewer waits for the partner above 30 FPS");

void CoopDebugPrintf(const char *fmt, ...);
double CoopDebugNowMs();
REXCVAR_DECLARE(bool, ao2_coop_debug);

namespace
{
  constexpr uint32_t kTunnelUpdate = 0x830E72A0;

  // The manager the game pumps; cleared by ProtoTunnelDestroy under the same
  // lock the forced flush holds (the network thread may destroy it).
  std::mutex g_tunnel_mutex;
  std::atomic<uint32_t> g_tunnel{0};
  thread_local bool t_forced = false;

  // Per-second statistics (ao2_coop_debug).
  struct Stats
  {
    std::atomic<uint32_t> pumps{0}, forced{0}, sends{0}, forced_sends{0};
    std::atomic<uint32_t> pump_thread{0};
    double last_pump = 0, pump_gap_max = 0, last_send = 0, send_gap_max = 0, window = 0;
  };
  Stats g_stats;

  uint32_t Load32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return __builtin_bswap32(v);
  }

  // A guest stack read that may run off the top of the stack (unmapped page).
  bool Readable(const uint8_t *base, uint32_t addr)
  {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(base + addr, &info, sizeof(info)) || info.State != MEM_COMMIT)
    {
      return false;
    }
    return !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           base + addr + 4 <= static_cast<const uint8_t *>(info.BaseAddress) + info.RegionSize;
  }

  void Report()
  {
    double now = CoopDebugNowMs();
    if (g_stats.window == 0)
    {
      g_stats.window = now;
      return;
    }
    if (now - g_stats.window < 1000.0)
    {
      return;
    }
    CoopDebugPrintf("net pump: tunnel updates %u/s (thread %u, max gap %.1f ms), forced flushes %u | game sends %u/s "
                    "(%u from forced flushes), max gap %.1f ms",
                    g_stats.pumps.exchange(0), g_stats.pump_thread.load(), g_stats.pump_gap_max,
                    g_stats.forced.exchange(0), g_stats.sends.exchange(0), g_stats.forced_sends.exchange(0),
                    g_stats.send_gap_max);
    g_stats.window = now;
    g_stats.pump_gap_max = g_stats.send_gap_max = 0;
  }
} // namespace

// The game's sendto of a packet to the partner (coop_link.cpp hook).
void CoopNetNoteGameSend(PPCContext &ctx, uint8_t *base)
{
  if (!REXCVAR_GET(ao2_coop_debug))
  {
    return;
  }
  double now = CoopDebugNowMs();
  double gap = g_stats.last_send > 0 ? now - g_stats.last_send : 0;
  g_stats.send_gap_max = std::max(g_stats.send_gap_max, gap);
  // Who builds the packets: the guest call chain of the first sends of a
  // lockstep game and then every 300th (back chain [sp], saved LR at
  // [caller sp - 8]).
  static uint32_t lockstep_sends = 0;
  if (ao2::coop::OnlineLockstep(base) && (lockstep_sends++ < 40 || lockstep_sends % 300 == 0))
  {
    char chain[256];
    int used = std::snprintf(chain, sizeof(chain), "%08X", ctx.lr ? uint32_t(ctx.lr) : 0u);
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < 12 && sp >= 0x70000000u && sp < 0x80000000u; i++)
    {
      if (!Readable(base, sp))
      {
        break;
      }
      uint32_t back = Load32(base, sp);
      if (back <= sp || back >= 0x80000000u || !Readable(base, back - 8))
      {
        break;
      }
      used += std::snprintf(chain + used, sizeof(chain) - used, " %08X", Load32(base, back - 8));
      sp = back;
      if (used >= int(sizeof(chain)) - 10)
      {
        break;
      }
    }
    CoopDebugPrintf("game send #%u gap %.1f ms%s, thread %u: %s", lockstep_sends, gap, t_forced ? " (forced flush)" : "",
                    GetCurrentThreadId(), chain);
  }
  g_stats.last_send = now;
  g_stats.sends++;
  if (t_forced)
  {
    g_stats.forced_sends++;
  }
}

// After the frame sync tick on the game thread (frame_time.cpp).
void CoopNetAfterSyncTick(PPCContext &ctx, uint8_t *base)
{
  if (REXCVAR_GET(ao2_coop_debug))
  {
    Report();
  }
  if (!REXCVAR_GET(ao2_coop_flush_tunnel) || !ao2::coop::OnlineLockstep(base))
  {
    return;
  }
  std::lock_guard lock(g_tunnel_mutex);
  uint32_t tunnel = g_tunnel.load(std::memory_order_relaxed);
  if (!tunnel)
  {
    return;
  }
  PPCContext saved = ctx;
  ctx.r3.u64 = tunnel;
  t_forced = true;
  rex::runtime::ResolveIndirectFunction(kTunnelUpdate)(ctx, base);
  t_forced = false;
  ctx = saved;
  g_stats.forced++;
}

// ProtoTunnelUpdate(r3 = tunnel manager).
extern "C" REX_FUNC(__imp__sub_830E72A0);
extern "C" REX_FUNC(sub_830E72A0)
{
  if (!t_forced)
  {
    g_tunnel.store(ctx.r3.u32, std::memory_order_relaxed);
    if (REXCVAR_GET(ao2_coop_debug))
    {
      double now = CoopDebugNowMs();
      if (g_stats.last_pump > 0)
      {
        g_stats.pump_gap_max = std::max(g_stats.pump_gap_max, now - g_stats.last_pump);
      }
      g_stats.last_pump = now;
      g_stats.pumps++;
      g_stats.pump_thread = GetCurrentThreadId();
    }
  }
  __imp__sub_830E72A0(ctx, base);
}

// ProtoTunnelDestroy(r3 = tunnel manager).
extern "C" REX_FUNC(__imp__sub_830E6AB0);
extern "C" REX_FUNC(sub_830E6AB0)
{
  {
    std::lock_guard lock(g_tunnel_mutex);
    uint32_t expected = ctx.r3.u32;
    g_tunnel.compare_exchange_strong(expected, 0);
  }
  __imp__sub_830E6AB0(ctx, base);
}
