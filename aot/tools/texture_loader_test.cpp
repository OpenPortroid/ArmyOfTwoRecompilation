// texture_loader_test.cpp - Checks the native renderer's texture conversion
// (src/gpu/native/texture_loader.cpp) on random guest data and times it.
//
//   texture_loader_test.exe [--bench]
//
// 1. TiledOffset2D (inlined) against the SDK's texture_util::GetTiledOffset2D
//    for every block of a range of pitches and block sizes.
// 2. ConvertGuestSubresource with runs (adjacent guest blocks copied and
//    swapped together) against block by block, byte for byte, for every
//    supported format, endianness, tiled / linear, 2D / 3D / cube, all mips.
// 3. --bench: MB/s of both for a few large textures.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/math.h>

#include "../src/gpu/native/texture_loader.h"

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;
using ao2::gpu::GuestTextureDesc;

namespace
{
  std::mt19937 g_rng(12345);
  uint32_t g_failures = 0;

  bool CheckTiledOffsets()
  {
    uint32_t bad = 0;
    for (uint32_t bpb_log2 = 0; bpb_log2 <= 4; ++bpb_log2)
    {
      for (uint32_t pitch = 1; pitch <= 520; pitch += (pitch < 70 ? 1 : 37))
      {
        uint32_t aligned = (pitch + 31) & ~31u;
        for (int32_t y = 0; y < 200; ++y)
        {
          for (int32_t x = 0; x < int32_t(aligned); ++x)
          {
            int32_t want = texture_util::GetTiledOffset2D(x, y, pitch, bpb_log2);
            int32_t got = ao2::gpu::TiledOffset2D(x, y, aligned, bpb_log2);
            if (want != got && bad++ < 5)
            {
              std::printf("TiledOffset2D x=%d y=%d pitch=%u bpb_log2=%u: %d, SDK %d\n", x, y, pitch, bpb_log2, got,
                          want);
            }
          }
        }
      }
    }
    std::printf("tiled offsets: %s\n", bad ? "MISMATCH" : "ok");
    return !bad;
  }

  struct Texture
  {
    GuestTextureDesc desc;
    std::vector<uint8_t> base, mips;
  };

  bool MakeTexture(xenos::TextureFormat format, xenos::DataDimension dim, uint32_t w, uint32_t h, uint32_t d,
                   bool tiled, xenos::Endian endian, uint32_t max_level, Texture &out)
  {
    xenos::xe_gpu_texture_fetch_t f = {};
    f.type = xenos::FetchConstantType::kTexture;
    f.format = format;
    f.endianness = endian;
    f.tiled = tiled;
    f.dimension = dim;
    f.packed_mips = 1;
    f.base_address = 0x100;
    f.mip_address = max_level ? 0x800 : 0;
    f.mip_min_level = 0;
    f.mip_max_level = max_level;
    // Pitch in texels >> 5: whole 32-texel tiles, rows of 256 bytes at least.
    const rex::graphics::FormatInfo *info = rex::graphics::FormatInfo::Get(format);
    uint32_t bw = info->block_width, bpb = info->bytes_per_block();
    uint32_t pitch_texels = (w + 31) & ~31u;
    if (!tiled)
    {
      uint32_t row_align_texels = std::max(256u / bpb, 1u) * bw;
      pitch_texels = (w + row_align_texels - 1) / row_align_texels * row_align_texels;
    }
    f.pitch = pitch_texels >> 5;
    if (dim == xenos::DataDimension::k3D)
    {
      f.size_3d.width = w - 1;
      f.size_3d.height = h - 1;
      f.size_3d.depth = d - 1;
    }
    else
    {
      f.size_2d.width = w - 1;
      f.size_2d.height = h - 1;
      f.size_2d.stack_depth = d - 1;
    }
    if (!ao2::gpu::DescribeGuestTexture(f, out.desc))
    {
      return false;
    }
    auto fill = [](std::vector<uint8_t> &v, uint32_t size) {
      v.resize(size + 64);
      for (uint8_t &b : v)
      {
        b = uint8_t(g_rng());
      }
    };
    fill(out.base, out.desc.base_size);
    fill(out.mips, out.desc.mip_size);
    return true;
  }

  // Destination layout of one host subresource, as the renderer's copyable
  // footprints give it (rows of host blocks, 256-byte row pitch).
  struct Footprint
  {
    uint32_t rows, row_bytes, row_pitch, depth;
  };
  Footprint HostFootprint(const GuestTextureDesc &desc, uint32_t host_level)
  {
    const ao2::gpu::HostTextureFormat &host = ao2::gpu::GetHostTextureFormat(desc.format);
    uint32_t hb = host.host_block;
    uint32_t w = ao2::gpu::HostLevelWidth(desc, host_level), h = ao2::gpu::HostLevelHeight(desc, host_level);
    Footprint fp;
    fp.rows = (h + hb - 1) / hb;
    fp.row_bytes = (w + hb - 1) / hb * host.host_block_bytes;
    fp.row_pitch = (fp.row_bytes + 255) & ~255u;
    uint32_t level = desc.min_level + host_level;
    fp.depth = desc.dimension == xenos::DataDimension::k3D ? std::max(desc.depth_or_array_size >> level, 1u) : 1;
    return fp;
  }

  bool CompareTexture(const Texture &t, const char *what)
  {
    uint32_t slices = t.desc.dimension == xenos::DataDimension::k3D ? 1 : t.desc.depth_or_array_size;
    for (uint32_t slice = 0; slice < slices; ++slice)
    {
      for (uint32_t level = 0; level + t.desc.min_level <= t.desc.max_level; ++level)
      {
        Footprint fp = HostFootprint(t.desc, level);
        size_t size = size_t(fp.row_pitch) * fp.rows * fp.depth;
        std::vector<uint8_t> a(size, 0xCD), b(size, 0xCD);
        bool ra = ao2::gpu::ConvertGuestSubresource(t.desc, level, slice, t.base.data(), t.mips.data(), a.data(),
                                                    fp.row_pitch, fp.row_pitch * fp.rows, fp.rows, fp.row_bytes,
                                                    true);
        bool rb = ao2::gpu::ConvertGuestSubresource(t.desc, level, slice, t.base.data(), t.mips.data(), b.data(),
                                                    fp.row_pitch, fp.row_pitch * fp.rows, fp.rows, fp.row_bytes,
                                                    false);
        if (ra != rb || a != b)
        {
          size_t i = 0;
          while (i < size && a[i] == b[i])
          {
            ++i;
          }
          std::printf("MISMATCH %s slice %u level %u (result %d/%d, first byte %zu of %zu)\n", what, slice, level,
                      ra, rb, i, size);
          ++g_failures;
          return false;
        }
      }
    }
    return true;
  }

  void Bench(xenos::TextureFormat format, uint32_t w, uint32_t h, bool tiled, xenos::Endian endian, const char *name)
  {
    Texture t;
    if (!MakeTexture(format, xenos::DataDimension::k2DOrStacked, w, h, 1, tiled, endian, 0, t))
    {
      std::printf("bench %s: not describable\n", name);
      return;
    }
    Footprint fp = HostFootprint(t.desc, 0);
    std::vector<uint8_t> dst(size_t(fp.row_pitch) * fp.rows);
    for (bool runs : {false, true})
    {
      auto start = std::chrono::steady_clock::now();
      int n = 0;
      double elapsed = 0;
      do
      {
        ao2::gpu::ConvertGuestSubresource(t.desc, 0, 0, t.base.data(), t.mips.data(), dst.data(), fp.row_pitch,
                                          fp.row_pitch * fp.rows, fp.rows, fp.row_bytes, runs);
        ++n;
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      } while (elapsed < 0.5);
      std::printf("bench %-34s %s: %7.3f ms per texture, %7.0f MB/s\n", name, runs ? "runs " : "block",
                  elapsed * 1000.0 / n, double(t.desc.base_size) * n / elapsed / 1e6);
    }
  }
} // namespace

int main(int argc, char **argv)
{
  bool bench = argc > 1 && !std::strcmp(argv[1], "--bench");
  bool ok = CheckTiledOffsets();

  const xenos::Endian endians[] = {xenos::Endian::kNone, xenos::Endian::k8in16, xenos::Endian::k8in32,
                                   xenos::Endian::k16in32};
  struct Shape
  {
    xenos::DataDimension dim;
    uint32_t w, h, d, levels;
  };
  const Shape shapes[] = {{xenos::DataDimension::k2DOrStacked, 1, 1, 1, 0},     {xenos::DataDimension::k2DOrStacked, 7, 5, 1, 2},
                          {xenos::DataDimension::k2DOrStacked, 64, 64, 1, 6},   {xenos::DataDimension::k2DOrStacked, 300, 170, 1, 8},
                          {xenos::DataDimension::k2DOrStacked, 1280, 720, 1, 0}, {xenos::DataDimension::k2DOrStacked, 128, 32, 3, 4},
                          {xenos::DataDimension::kCube, 32, 32, 6, 5},  {xenos::DataDimension::k3D, 32, 16, 8, 3}};
  uint32_t textures = 0;
  for (uint32_t fi = 0; fi < 64; ++fi)
  {
    auto format = xenos::TextureFormat(fi);
    if (ao2::gpu::GetHostTextureFormat(format).conversion == ao2::gpu::TextureConversion::kUnsupported)
    {
      continue;
    }
    for (const Shape &s : shapes)
    {
      for (bool tiled : {false, true})
      {
        for (xenos::Endian e : endians)
        {
          Texture t;
          if (!MakeTexture(format, s.dim, s.w, s.h, s.d, tiled, e, s.levels, t))
          {
            continue;
          }
          char what[96];
          std::snprintf(what, sizeof(what), "format %u %ux%ux%u dim %u %s endian %u", fi, s.w, s.h, s.d,
                        unsigned(s.dim), tiled ? "tiled" : "linear", unsigned(e));
          ok &= CompareTexture(t, what);
          ++textures;
        }
      }
    }
  }
  std::printf("conversion: %u textures, %u mismatches\n", textures, g_failures);

  if (bench)
  {
    Bench(xenos::TextureFormat::k_8, 1280, 720, false, xenos::Endian::kNone, "k_8 1280x720 linear (movie plane)");
    Bench(xenos::TextureFormat::k_8_8_8_8, 1024, 1024, true, xenos::Endian::k8in32, "k_8_8_8_8 1024^2 tiled");
    Bench(xenos::TextureFormat::k_DXT1, 2048, 2048, true, xenos::Endian::k8in16, "DXT1 2048^2 tiled");
    Bench(xenos::TextureFormat::k_DXT4_5, 1024, 1024, true, xenos::Endian::k8in16, "DXT4_5 1024^2 tiled");
    Bench(xenos::TextureFormat::k_DXT4_5, 1280, 720, false, xenos::Endian::k8in16, "DXT4_5 1280x720 linear");
    Bench(xenos::TextureFormat::k_5_6_5, 512, 512, true, xenos::Endian::k8in16, "5_6_5 512^2 tiled (per texel)");
  }
  std::printf("%s\n", ok && !g_failures ? "PASS" : "FAIL");
  return ok && !g_failures ? 0 : 1;
}
