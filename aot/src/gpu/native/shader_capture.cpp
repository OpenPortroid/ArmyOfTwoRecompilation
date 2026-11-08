// shader_capture.cpp - see shader_capture.h.
#include "shader_capture.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "fast_hash.h"

REXCVAR_DEFINE_BOOL(ao2_shader_capture_dump, false, "Debug",
                    "Append the microcode of every shader the game creates to shader_capture.bin");

namespace ao2::gpu
{
  uint64_t HashDwords(const uint32_t *p, uint32_t count); // pm4.cpp
}

namespace ao2::gpu::capture
{
  namespace
  {
    // Microcode of a shader is at most 0x6000 bytes (the D3D checks).
    constexpr uint32_t kMaxBytes = 0x8000;
    // Pending until the GPU thread takes them (it only starts with the first
    // draws); more is dropped.
    constexpr size_t kMaxPendingBytes = 64u << 20;

    std::mutex g_mutex;
    std::unordered_set<uint64_t, FastHash> g_seen;
    std::vector<Captured> g_pending;
    size_t g_pending_bytes = 0;

    uint32_t Load32(const uint8_t *base, uint32_t address)
    {
      uint32_t v;
      std::memcpy(&v, base + address, 4);
      return __builtin_bswap32(v);
    }
  } // namespace

  void FetchDwords(const uint32_t *dwords, uint32_t count, std::vector<uint32_t> &out)
  {
    out.clear();
    auto d = [&](uint32_t i) { return __builtin_bswap32(dwords[i]); };
    uint32_t slots = count / 3; // 96-bit instruction slots
    uint32_t cf_end = slots;    // control flow ends where the first clause starts
    for (uint32_t i = 0; i < cf_end * 2 && i / 2 < slots; ++i)
    {
      // Two 48-bit control flow instructions per slot.
      uint32_t s = (i / 2) * 3;
      uint64_t ins = i % 2 == 0 ? d(s) | uint64_t(d(s + 1) & 0xFFFF) << 32
                                : (d(s + 1) >> 16) | uint64_t(d(s + 2)) << 16;
      uint32_t op = uint32_t(ins >> 44) & 0xF;
      // exec, exec_end, cond_exec(_end), cond_pred_exec(_end), cond_exec_pred_clean(_end)
      if (!((op >= 1 && op <= 6) || op == 13 || op == 14))
      {
        continue;
      }
      uint32_t address = uint32_t(ins) & 0xFFF, clause = uint32_t(ins >> 12) & 7,
               sequence = uint32_t(ins >> 16) & 0xFFF;
      if (address && address < cf_end)
      {
        cf_end = address;
      }
      for (uint32_t k = 0; k < clause; ++k)
      {
        uint32_t a = (address + k) * 3;
        // A fetch, and its opcode a vertex fetch.
        if (((sequence >> (2 * k)) & 1) && a + 2 < count && (d(a) & 0x1F) == 0)
        {
          out.push_back(a);
          out.push_back(a + 1);
          out.push_back(a + 2);
        }
      }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
  }

  uint64_t FetchMaskedHash(const uint32_t *dwords, uint32_t count)
  {
    static thread_local std::vector<uint32_t> fetch, masked;
    FetchDwords(dwords, count, fetch);
    masked.assign(dwords, dwords + count);
    for (uint32_t i : fetch)
    {
      masked[i] = 0;
    }
    return HashDwords(masked.data(), count);
  }

  void Drain(std::vector<Captured> &out)
  {
    std::lock_guard lock(g_mutex);
    for (auto &c : g_pending)
    {
      out.push_back(std::move(c));
    }
    g_pending.clear();
    g_pending_bytes = 0;
  }

  uint32_t SeenCount()
  {
    std::lock_guard lock(g_mutex);
    return uint32_t(g_seen.size());
  }
} // namespace ao2::gpu::capture

// XGGetMicrocodeShaderParts(r3 = shader blob, r4 = parts): parts = {cached
// data, its size, physical data (blob + blob[1]), its size (blob[2])}. The
// FXe*Shader constructors copy the physical part unchanged into the memory
// the GPU loads from. The microcode is a part of it (literal constants come
// first): blob + blob[6] points at {its offset in the physical data, its
// size} (as the D3D's own parser, sub_82E76468, reads them).
extern "C" REX_FUNC(__imp__sub_82B163B8);
extern "C" REX_FUNC(sub_82B163B8)
{
  uint32_t blob = ctx.r3.u32, parts = ctx.r4.u32;
  __imp__sub_82B163B8(ctx, base);
  using namespace ao2::gpu::capture;
  uint32_t physical = Load32(base, parts + 8);
  uint32_t physical_bytes = Load32(base, parts + 12);
  uint32_t cached_bytes = Load32(base, parts + 4);
  uint32_t info = Load32(base, blob + 24);
  if (!physical || !physical_bytes || info + 8 > cached_bytes)
  {
    return;
  }
  uint32_t offset = Load32(base, blob + info);
  uint32_t bytes = Load32(base, blob + info + 4);
  if (!bytes || bytes > kMaxBytes || ((offset | bytes) & 3) || uint64_t(offset) + bytes > physical_bytes)
  {
    return;
  }
  const auto *dwords = reinterpret_cast<const uint32_t *>(base + physical + offset);
  uint32_t count = bytes / 4;
  uint64_t hash = ao2::gpu::HashDwords(dwords, count);
  std::lock_guard lock(g_mutex);
  if (g_pending_bytes + bytes > kMaxPendingBytes || !g_seen.insert(hash).second)
  {
    return;
  }
  if (REXCVAR_GET(ao2_shader_capture_dump))
  {
    static FILE *dump = std::fopen("shader_capture.bin", "wb");
    if (dump)
    {
      uint32_t head[4] = {Load32(base, blob), count, uint32_t(hash), uint32_t(hash >> 32)};
      std::fwrite(head, sizeof(head), 1, dump);
      std::fwrite(base + blob, 4, 16, dump); // blob header, raw
      std::fwrite(dwords, 4, count, dump);
      std::fflush(dump);
    }
  }
  // Blob magic 0x102A1101: vertex shader, 0x102A1100: pixel shader.
  uint64_t masked = (Load32(base, blob) & 1) ? FetchMaskedHash(dwords, count) : 0;
  g_pending.push_back({hash, masked, std::vector<uint32_t>(dwords, dwords + count)});
  g_pending_bytes += bytes;
}
