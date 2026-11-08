// recomp_memory.h - Cheaper guest stack accesses in the recompiled code.
//
// Included after the generated precompiled header (CMakeLists.txt). On
// Windows every guest load and store adds REX_PHYS_HOST_OFFSET: the
// 0xE0000000 physical view sits 4 KB further on the host, so each access
// pays a compare, setae, shift and add. A quarter of all accesses are to the
// stack, written by the code generator as "ctx.r1.u32 + <constant>", and
// guest stacks always lie in 0x70000000-0x7F000000 (XThread
// kStackAddressRangeBegin/End; the game never switches stacks: no fibers, no
// KeSetCurrentStackPointers), so for them the offset is 0. The macro
// argument's text is checked at compile time; everything else (volatile
// accesses, byte swapping) stays exactly as generated.
#pragma once

#include <type_traits>

namespace ao2::recomp
{
  // "ctx.r1.u32" or "ctx.r1.u32 + <decimal>" / "ctx.r1.u32 + -<decimal>".
  constexpr bool IsStackAddress(const char *s)
  {
    const char prefix[] = "ctx.r1.u32";
    for (int i = 0; i < 10; ++i)
    {
      if (s[i] != prefix[i])
      {
        return false;
      }
    }
    s += 10;
    if (*s == 0)
    {
      return true;
    }
    if (s[0] != ' ' || s[1] != '+' || s[2] != ' ')
    {
      return false;
    }
    s += 3;
    if (*s == '-')
    {
      ++s;
    }
    if (*s < '0' || *s > '9')
    {
      return false;
    }
    int digits = 0;
    while (*s >= '0' && *s <= '9')
    {
      ++s;
      ++digits;
    }
    // At most 5 digits: |offset| < 100000, far from leaving the stack range.
    return *s == 0 && digits <= 5;
  }

  static_assert(IsStackAddress("ctx.r1.u32 + -564"));
  static_assert(IsStackAddress("ctx.r1.u32 + 80"));
  static_assert(IsStackAddress("ctx.r1.u32"));
  static_assert(!IsStackAddress("ctx.r11.u32 + 80"));
  static_assert(!IsStackAddress("ctx.r1.u32 + ctx.r9.u32"));
  static_assert(!IsStackAddress("ctx.r1.u32 + 80 + 4"));
} // namespace ao2::recomp

#define AO2_HOST_OFFSET(x, text) \
  (std::integral_constant<bool, ::ao2::recomp::IsStackAddress(text)>::value ? 0u : REX_PHYS_HOST_OFFSET(x))

#undef REX_LOAD_U8
#undef REX_LOAD_U16
#undef REX_LOAD_U32
#undef REX_LOAD_U64
#undef REX_STORE_U8
#undef REX_STORE_U16
#undef REX_STORE_U32
#undef REX_STORE_U64

#define REX_LOAD_U8(x) (*(volatile u8 *)(base + (u32)(x) + AO2_HOST_OFFSET(x, #x)))
#define REX_LOAD_U16(x) __builtin_bswap16(*(volatile u16 *)(base + (u32)(x) + AO2_HOST_OFFSET(x, #x)))
#define REX_LOAD_U32(x) __builtin_bswap32(*(volatile u32 *)(base + (u32)(x) + AO2_HOST_OFFSET(x, #x)))
#define REX_LOAD_U64(x) __builtin_bswap64(*(volatile u64 *)(base + (u32)(x) + AO2_HOST_OFFSET(x, #x)))

#define REX_STORE_U8(x, y) (*(volatile u8 *)(base + (u32)(x) + AO2_HOST_OFFSET(x, #x)) = (y))
#define REX_STORE_U16(x, y) (*(volatile u16 *)(base + (u32)(x) + AO2_HOST_OFFSET(x, #x)) = __builtin_bswap16(y))
#define REX_STORE_U32(x, y) (*(volatile u32 *)(base + (u32)(x) + AO2_HOST_OFFSET(x, #x)) = __builtin_bswap32(y))
#define REX_STORE_U64(x, y) (*(volatile u64 *)(base + (u32)(x) + AO2_HOST_OFFSET(x, #x)) = __builtin_bswap64(y))

// Vector dot products (vmsum3fp128 / vmsum4fp128: simde_mm_dp_ps with 0xEF / 0xFF) on the CPU's DPPS: the order DPPS adds
// the four products in is not the same on every CPU (vendors differ), so the poses of skeletons (matrix and vector
// math) came out a few ulps apart on two PCs of a lockstep game, a bullet that grazed a character hit on one and missed
// on the other and the game went out of sync (versus traces 2026-11-07: the first difference was a hit). The same
// products added in a fixed order, (p0 + p1) + (p2 + p3), on every machine.
#include <simde/x86/sse4.1.h>

namespace ao2::recomp
{
  template <int Imm>
  inline simde__m128 DotProduct(simde__m128 a, simde__m128 b)
  {
    static_assert(Imm == 0xEF || Imm == 0xFF, "only the dot products the code generator emits");
    simde__m128 p = simde_mm_mul_ps(a, b);
    if constexpr (Imm == 0xEF)
    {
      // Elements 1..3 only; the result goes to all four.
      p = simde_mm_and_ps(p, simde_mm_castsi128_ps(simde_mm_set_epi32(-1, -1, -1, 0)));
    }
    // (p0 + p1, p1 + p0, p2 + p3, p3 + p2), then (p0 + p1) + (p2 + p3) in every element (the adds are commutative).
    simde__m128 t = simde_mm_add_ps(p, simde_mm_shuffle_ps(p, p, SIMDE_MM_SHUFFLE(2, 3, 0, 1)));
    return simde_mm_add_ps(t, simde_mm_shuffle_ps(t, t, SIMDE_MM_SHUFFLE(1, 0, 3, 2)));
  }
} // namespace ao2::recomp

#undef simde_mm_dp_ps
#undef _mm_dp_ps
#define simde_mm_dp_ps(a, b, imm8) ::ao2::recomp::DotProduct<(imm8)>((a), (b))
