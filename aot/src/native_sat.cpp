// native_sat.cpp - sub_82813CD8 as native code.
//
// The convex hull test of the collision checks (its only caller is
// sub_82885870, under FPrimitiveOctree::ActorEncroachmentCheck): project the
// hull's vertices on an axis, and if a point (f1) lies within the projection
// widened by f2, write the overlap along the axis (r7) and the axis
// direction (r8) where it is smaller than what r7 held. 3% of the game thread
// on China's river.
//
//   r3 hull {u32 vertices (x, y, z floats), s32 count}, r4 axis (x, y, z),
//   f1 point, f2 radius, r7 in/out overlap, r8 out normal; r3 = 0 / 1.
//
// The arithmetic is the recompiled code's, operation for operation: every
// single-precision result rounded to float, fmadds as float(fma) in double,
// fsel as `x >= 0 ? a : b`, the unrolled loop's own summation order (each of
// its four vertices differently) - the same bits, which the co-op lockstep
// needs. ao2_native_sat 2 runs both and compares every call.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "log_files.h"

REXCVAR_DEFINE_INT32(ao2_native_sat, 1, "Debug",
                     "sub_82813CD8 (convex hull projection): 0 recompiled, 1 native, 2 both, compared")
    .range(0, 2);

namespace
{
  float LoadF(const uint8_t *base, uint32_t address)
  {
    uint32_t v;
    std::memcpy(&v, base + address, 4);
    return std::bit_cast<float>(std::byteswap(v));
  }

  uint32_t LoadU(const uint8_t *base, uint32_t address)
  {
    uint32_t v;
    std::memcpy(&v, base + address, 4);
    return std::byteswap(v);
  }

  void StoreF(uint8_t *base, uint32_t address, float f)
  {
    uint32_t v = std::byteswap(std::bit_cast<uint32_t>(f));
    std::memcpy(base + address, &v, 4);
  }

  // Single-precision results as the recompiled code makes them.
  double R(double x)
  {
    return double(float(x));
  }

  double Fma(double a, double b, double c)
  {
    return double(float(std::fma(a, b, c)));
  }

  struct Result
  {
    uint32_t r3 = 0;
    bool depth_written = false;
    float depth = 0.0f;
    float normal[3] = {};
  };

  Result Native(const uint8_t *base, uint32_t hull, uint32_t axis, double point, double radius, uint32_t depth_ptr)
  {
    Result out;
    uint32_t vertices = LoadU(base, hull);
    int32_t count = int32_t(LoadU(base, hull + 4));
    double ax = LoadF(base, axis), ay = LoadF(base, axis + 4), az = LoadF(base, axis + 8);
    double lo = LoadF(base, 0x82030074);  // 3.4e38
    double hi = LoadF(base, 0x8207635C);  // -3.4e38
    auto update = [&](double d) {
      double below = R(d - lo), above = R(d - hi);
      lo = below >= 0.0 ? lo : d;
      hi = above >= 0.0 ? d : hi;
    };
    int32_t i = 0;
    if (count >= 4)
    {
      uint32_t groups = (uint32_t(count - 4) >> 2) + 1;
      i = int32_t(groups * 4);
      uint32_t p = vertices;
      for (uint32_t g = 0; g < groups; ++g, p += 48)
      {
        double x0 = LoadF(base, p), y0 = LoadF(base, p + 4), z0 = LoadF(base, p + 8);
        double x1 = LoadF(base, p + 12), y1 = LoadF(base, p + 16), z1 = LoadF(base, p + 20);
        double x2 = LoadF(base, p + 24), y2 = LoadF(base, p + 28), z2 = LoadF(base, p + 32);
        double x3 = LoadF(base, p + 36), y3 = LoadF(base, p + 40), z3 = LoadF(base, p + 44);
        double a = Fma(x0, ax, R(z0 * az));
        double b = Fma(z1, az, R(x1 * ax));
        double c = Fma(y2, ay, R(z2 * az));
        double e = Fma(y3, ay, R(z3 * az));
        update(Fma(y0, ay, a));
        update(Fma(y1, ay, b));
        update(Fma(x2, ax, c));
        update(Fma(x3, ax, e));
      }
    }
    for (; i < count; ++i)
    {
      uint32_t p = vertices + uint32_t(i) * 12;
      double x = LoadF(base, p), y = LoadF(base, p + 4), z = LoadF(base, p + 8);
      update(Fma(ax, x, Fma(y, ay, R(z * az))));
    }
    double low = R(lo - radius), high = R(hi + radius);
    if (point < low || point > high)
    {
      return out;
    }
    out.r3 = 1;
    double length2 = Fma(ax, ax, Fma(ay, ay, R(az * az)));
    double to_low = R(point - low), to_high = R(high - point);
    double inverse = R(1.0 / std::sqrt(length2));
    double limit = R(double(LoadF(base, depth_ptr)) / inverse);
    if (to_low < limit)
    {
      out.depth_written = true;
      out.depth = float(R(to_low * inverse));
      out.normal[0] = float(R(-ax * inverse));
      out.normal[1] = float(R(-ay * inverse));
      out.normal[2] = float(R(-az * inverse));
    }
    if (to_high < limit)
    {
      out.depth_written = true;
      out.depth = float(R(to_high * inverse));
      out.normal[0] = float(R(ax * inverse));
      out.normal[1] = float(R(ay * inverse));
      out.normal[2] = float(R(az * inverse));
    }
    return out;
  }

  void Apply(uint8_t *base, const Result &r, uint32_t depth_ptr, uint32_t normal_ptr)
  {
    if (r.depth_written)
    {
      StoreF(base, depth_ptr, r.depth);
      for (uint32_t k = 0; k < 3; ++k)
      {
        StoreF(base, normal_ptr + k * 4, r.normal[k]);
      }
    }
  }

  uint64_t g_checked = 0, g_mismatches = 0;
} // namespace

extern "C" REX_FUNC(__imp__sub_82813CD8);
extern "C" REX_FUNC(sub_82813CD8)
{
  int32_t mode = REXCVAR_GET(ao2_native_sat);
  if (mode == 0)
  {
    __imp__sub_82813CD8(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  uint32_t hull = ctx.r3.u32, axis = ctx.r4.u32, depth_ptr = ctx.r7.u32, normal_ptr = ctx.r8.u32;
  Result r = Native(base, hull, axis, ctx.f1.f64, ctx.f2.f64, depth_ptr);
  if (mode == 1)
  {
    Apply(base, r, depth_ptr, normal_ptr);
    ctx.r3.u64 = r.r3;
    return;
  }
  // Checked: what the recompiled code leaves in memory and r3 against what
  // the native one would have.
  uint8_t before_depth[4], before_normal[12];
  std::memcpy(before_depth, base + depth_ptr, 4);
  std::memcpy(before_normal, base + normal_ptr, 12);
  __imp__sub_82813CD8(ctx, base);
  uint8_t after_depth[4], after_normal[12];
  std::memcpy(after_depth, base + depth_ptr, 4);
  std::memcpy(after_normal, base + normal_ptr, 12);
  // The native result applied to the memory as it was before.
  uint8_t expect_depth[4], expect_normal[12];
  std::memcpy(expect_depth, before_depth, 4);
  std::memcpy(expect_normal, before_normal, 12);
  if (r.depth_written)
  {
    uint32_t v = std::byteswap(std::bit_cast<uint32_t>(r.depth));
    std::memcpy(expect_depth, &v, 4);
    for (uint32_t k = 0; k < 3; ++k)
    {
      v = std::byteswap(std::bit_cast<uint32_t>(r.normal[k]));
      std::memcpy(expect_normal + k * 4, &v, 4);
    }
  }
  ++g_checked;
  bool mismatch = ctx.r3.u32 != r.r3 || std::memcmp(after_depth, expect_depth, 4) ||
                  std::memcmp(after_normal, expect_normal, 12);
  g_mismatches += mismatch;
  if ((mismatch && g_mismatches <= 20) || (g_checked & 0xFFFF) == 1)
  {
    if (FILE *log = ao2::OpenLogFile("native_sat.log", "a"))
    {
      std::fprintf(log, "%llu calls checked, %llu mismatches", (unsigned long long)g_checked,
                   (unsigned long long)g_mismatches);
      if (mismatch)
      {
        std::fprintf(log, ": r3 %u vs %u, depth %08X vs %08X", ctx.r3.u32, r.r3, LoadU(base, depth_ptr),
                     std::byteswap(std::bit_cast<uint32_t>(r.depth)));
      }
      std::fprintf(log, "\n");
      std::fclose(log);
    }
  }
}
