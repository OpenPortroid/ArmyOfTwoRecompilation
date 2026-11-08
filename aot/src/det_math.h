// det_math.h - Trigonometry that gives the same bits on every machine.
//
// The lockstep (online co-op and versus) needs the same result of every float operation on both PCs. The C runtime's
// atan2 / sin / cos / tan / hypot choose their implementation by the CPU (FMA or not, vendor), so the last bits can differ
// between two PCs - a versus bot aiming with them turned its character's look rotation by a few ulps differently and the
// game went out of sync. These use only + - * / and floor on doubles (IEEE exact, no library call), accurate to ~1e-9.
#pragma once

#include <cmath>

namespace ao2::det
{
  constexpr double kPi = 3.14159265358979323846;

  inline double AtanD(double x)
  {
    bool negative = x < 0.0;
    if (negative)
    {
      x = -x;
    }
    bool inverted = x > 1.0;
    if (inverted)
    {
      x = 1.0 / x;
    }
    constexpr double kSqrt3 = 1.7320508075688772;
    bool shifted = x > 0.2679491924311227; // tan(15 degrees): atan(x) = 30 degrees + atan((x sqrt3 - 1) / (sqrt3 + x))
    if (shifted)
    {
      x = (x * kSqrt3 - 1.0) / (kSqrt3 + x);
    }
    double x2 = x * x;
    double r = x * (1.0 - x2 * (1.0 / 3.0 - x2 * (1.0 / 5.0 - x2 * (1.0 / 7.0 - x2 * (1.0 / 9.0 - x2 * (1.0 / 11.0 -
                    x2 * (1.0 / 13.0 - x2 * (1.0 / 15.0))))))));
    if (shifted)
    {
      r += kPi / 6.0;
    }
    if (inverted)
    {
      r = kPi / 2.0 - r;
    }
    return negative ? -r : r;
  }

  inline float Atan(float x)
  {
    return float(AtanD(double(x)));
  }

  inline float Atan2(float y, float x)
  {
    double dy = double(y), dx = double(x);
    if (dx == 0.0)
    {
      return dy > 0.0 ? float(kPi / 2.0) : dy < 0.0 ? float(-kPi / 2.0) : 0.0f;
    }
    double a = AtanD(dy / dx);
    if (dx < 0.0)
    {
      a += dy >= 0.0 ? kPi : -kPi;
    }
    return float(a);
  }

  inline double SinD(double x)
  {
    // Into [-pi, pi], then [-pi/2, pi/2].
    x -= 2.0 * kPi * std::floor(x / (2.0 * kPi) + 0.5);
    if (x > kPi / 2.0)
    {
      x = kPi - x;
    }
    else if (x < -kPi / 2.0)
    {
      x = -kPi - x;
    }
    double x2 = x * x;
    return x * (1.0 - x2 / 6.0 * (1.0 - x2 / 20.0 * (1.0 - x2 / 42.0 * (1.0 - x2 / 72.0 * (1.0 - x2 / 110.0 *
                (1.0 - x2 / 156.0 * (1.0 - x2 / 210.0 * (1.0 - x2 / 272.0))))))));
  }

  inline float Sin(float x)
  {
    return float(SinD(double(x)));
  }

  inline float Cos(float x)
  {
    return float(SinD(double(x) + kPi / 2.0));
  }

  inline float Tan(float x)
  {
    double c = SinD(double(x) + kPi / 2.0);
    return float(SinD(double(x)) / c);
  }

  inline float Hypot(float a, float b)
  {
    return float(std::sqrt(double(a) * double(a) + double(b) * double(b)));
  }

  inline float Hypot(float a, float b, float c)
  {
    return float(std::sqrt(double(a) * double(a) + double(b) * double(b) + double(c) * double(c)));
  }
} // namespace ao2::det
