// guest_fast.cpp - Native versions of small, hot guest helpers.
//
// The rendering thread (UE3) is the frame rate limit above ~120 FPS, and its
// profile is flat; these are the leaf helpers that showed up near the top
// whose recompiled form is much slower than the job they do. Each one gives
// the same memory and register results as the original (results of the
// guest's own conversion code, not the host's).
#include <cstdint>

#include <rex/ppc.h>

namespace
{
  // Guest address -> host pointer, as the recompiled loads and stores do it
  // (the 0xE0000000 physical view is 4 KB further on Windows).
  inline uint8_t *Host(uint8_t *base, uint32_t address)
  {
    return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
  }

  inline uint16_t Load16(uint8_t *base, uint32_t address)
  {
    uint16_t v;
    __builtin_memcpy(&v, Host(base, address), 2);
    return __builtin_bswap16(v);
  }

  inline void Store32(uint8_t *base, uint32_t address, uint32_t value)
  {
    value = __builtin_bswap32(value);
    __builtin_memcpy(Host(base, address), &value, 4);
  }
} // namespace

// Half floats -> floats (r3 = float destination, r4 = half source, r5 = count),
// one at a time through vupkd3d128 type 12 in the original: zero exponents give
// a signed zero, the maximum exponent stays a finite number (no inf / NaN).
extern "C" REX_FUNC(sub_82B45028)
{
  uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, count = ctx.r5.u32;
  for (uint32_t i = 0; i < count; ++i)
  {
    uint32_t h = Load16(base, src + i * 2);
    uint32_t sign = (h & 0x8000u) << 16;
    uint32_t f = (h & 0x7C00u) ? sign | (((h & 0x7C00u) + 0x1C000u) << 13) | ((h & 0x03FFu) << 13) : sign;
    Store32(base, dst + i * 4, f);
  }
  // Registers as the original loop leaves them.
  ctx.r10.u64 = uint32_t(dst + count * 4);
  ctx.r11.u64 = uint32_t(src + count * 2);
  ctx.r5.u64 = 0;
}
