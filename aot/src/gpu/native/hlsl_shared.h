// hlsl_shared.h - Layout shared between C++ and the generated HLSL (cbuffer b1).
#pragma once

#include <cfloat>
#include <cstdint>

namespace ao2::gpu
{
  enum XeSystemFlags : uint32_t
  {
    kSysFlagWNotReciprocal = 1u << 0,
    kSysFlagXYDividedByW = 1u << 1,
    kSysFlagZDividedByW = 1u << 2,
    kSysFlagAlphaTest = 1u << 3,
  };

  struct alignas(16) XeSystemConstants
  {
    uint32_t flags;               // 0
    uint32_t vertex_index_offset; // 4
    uint32_t alpha_func;          // 8
    float alpha_ref;              // 12
    float ndc_scale[4];           // 16
    float ndc_offset[4];          // 32
    float color_exp_bias[4];      // 48
    uint32_t vertex_index_range[4]; // 64: x = min, y = max
    float resolution_scale[4];    // 80: x = host pixels per guest pixel, y = 1 / x, z = 4 * x
    uint32_t bool_loop[40];       // 96: 8 bool dwords, 32 loop constants
    uint32_t tex_info[16][4];     // 256: x = per-component mode, y = exp scale (float bits)
    uint32_t vfetch[192];         // 512: 96 vertex fetch constants
  };
  static_assert(sizeof(XeSystemConstants) == 1280);
} // namespace ao2::gpu
