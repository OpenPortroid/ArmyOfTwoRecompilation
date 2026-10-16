// coop.h - Online co-op state shared by the input hooks.
#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/ppc.h>

namespace ao2::coop
{
  // Army of Two runs online co-op in deterministic lockstep: both machines
  // simulate the same frames from the exchanged pad input, and every frame
  // carries a checksum of the shared rand() stream and of physics state
  // (sync object +44/+46, compared in sub_82A26290). After 600 mismatching
  // frames the client leaves ("Connection Lost"). Anything that changes the
  // simulation on one machine only - such as native mouse aiming writing the
  // camera rotation directly - must stay off while a network game runs.
  //
  // The frame sync object (sub_82A30DE8 ticks it) is a singleton at
  // [0x831F3008]; its mode at +16 is 1 in a network game (4 in single player
  // and the menus).
  inline bool OnlineLockstep(const uint8_t *base)
  {
    auto load = [&](uint32_t addr) {
      uint32_t v;
      std::memcpy(&v, base + addr, 4);
      return std::byteswap(v);
    };
    uint32_t sync = load(0x831F3008);
    return sync != 0 && load(sync + 16) == 1;
  }

  // Single player also runs the frame sync (mode 4, the player's own input
  // replayed); for testing the online input paths there (ao2_coop_test_local).
  inline bool LocalLockstep(const uint8_t *base)
  {
    auto load = [&](uint32_t addr) {
      uint32_t v;
      std::memcpy(&v, base + addr, 4);
      return std::byteswap(v);
    };
    uint32_t sync = load(0x831F3008);
    return sync != 0 && load(sync + 16) == 4;
  }

  // A network co-op session is on: from the moment EA's game manager starts
  // (lobby) until the game is destroyed - unlike OnlineLockstep this
  // includes the level start (intro movie) before the lockstep runs.
  // Set from the DirtySock debug output hooks (coop_debug.cpp).
  inline std::atomic<bool> network_session{false};

  // Anything local-only (skipping a movie or cutscene, cheats) must be off.
  inline bool InNetworkGame(const uint8_t *base)
  {
    return network_session.load(std::memory_order_relaxed) || OnlineLockstep(base);
  }

  // Number of the current lockstep frame (frame sync object +92); the same
  // on both machines while a frame is simulated.
  inline uint32_t SyncFrame(const uint8_t *base)
  {
    auto load = [&](uint32_t addr) {
      uint32_t v;
      std::memcpy(&v, base + addr, 4);
      return std::byteswap(v);
    };
    uint32_t sync = load(0x831F3008);
    return sync ? load(sync + 92) : 0;
  }
  // Every pawn's skeletal mesh at LOD 0 while the lockstep runs (coop_lod.cpp);
  // once per simulation step on the game thread.
  void ForceLod(PPCContext &ctx, uint8_t *base);
  // ao2_coop_state_hash: every reflected property of the level's objects to
  // state_hash.log (coop_state_hash.cpp); at each lockstep frame's record
  // build on the game thread.
  void StateHashFrame(const uint8_t *base, uint32_t frame);
  // Streaming levels block on load and appear whole while the lockstep runs
  // (coop_streaming.cpp); once per simulation step on the game thread.
  void BlockStreaming(PPCContext &ctx, uint8_t *base);
} // namespace ao2::coop
