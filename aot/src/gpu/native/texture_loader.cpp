// texture_loader.cpp - see texture_loader.h. Format mapping follows the SDK's
// D3D12 texture cache (host_formats_), loads its texture_load_*.hlsl shaders.
#include "texture_loader.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/hash.h>
#include <rex/math.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace ao2::gpu
{
  namespace xenos = rex::graphics::xenos;
  namespace texture_util = rex::graphics::texture_util;
  using rex::graphics::FormatInfo;
  using xenos::TextureFormat;

  namespace
  {
    constexpr uint32_t kPhysicalSize = 0x20000000;

    const uint8_t *Physical(uint32_t address)
    {
      return rex::Runtime::instance()->memory()->TranslatePhysical(address);
    }

    struct FormatTable
    {
      HostTextureFormat f[64];

      void Set(TextureFormat format, DXGI_FORMAT resource, DXGI_FORMAT unorm, DXGI_FORMAT snorm,
               TextureConversion conversion, uint8_t host_block, uint8_t host_block_bytes, uint32_t swizzle)
      {
        f[uint32_t(format)] = {resource, unorm, snorm, conversion, host_block, host_block_bytes, swizzle};
      }

      FormatTable()
      {
        using C = TextureConversion;
        const uint32_t rrrr = xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, rggg = xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
                       rgbb = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB, rgba = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
        for (TextureFormat t : {TextureFormat::k_8, TextureFormat::k_8_A})
        {
          Set(t, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, C::kCopy, 1, 1, rrrr);
        }
        Set(TextureFormat::k_1_5_5_5, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_UNKNOWN,
            C::kR5G5B5A1, 1, 2, rgba);
        Set(TextureFormat::k_5_6_5, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_UNKNOWN,
            C::kR5G6B5, 1, 2, rgbb);
        for (TextureFormat t : {TextureFormat::k_8_8_8_8, TextureFormat::k_8_8_8_8_AS_16_16_16_16})
        {
          Set(t, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM, C::kCopy,
              1, 4, rgba);
        }
        for (TextureFormat t : {TextureFormat::k_2_10_10_10, TextureFormat::k_2_10_10_10_AS_16_16_16_16})
        {
          Set(t, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN, C::kCopy, 1,
              4, rgba);
        }
        Set(TextureFormat::k_8_8, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM,
            C::kCopy, 1, 2, rggg);
        Set(TextureFormat::k_4_4_4_4, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_UNKNOWN,
            C::kRGBA4, 1, 2, rgba);
        for (TextureFormat t : {TextureFormat::k_10_11_11, TextureFormat::k_10_11_11_AS_16_16_16_16})
        {
          Set(t, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_UNKNOWN,
              C::kR11G11B10, 1, 8, rgbb);
        }
        for (TextureFormat t : {TextureFormat::k_11_11_10, TextureFormat::k_11_11_10_AS_16_16_16_16})
        {
          Set(t, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_UNKNOWN,
              C::kR10G11B11, 1, 8, rgbb);
        }
        for (TextureFormat t : {TextureFormat::k_DXT1, TextureFormat::k_DXT1_AS_16_16_16_16})
        {
          Set(t, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_UNKNOWN, C::kCopy, 4, 8, rgba);
        }
        for (TextureFormat t : {TextureFormat::k_DXT2_3, TextureFormat::k_DXT2_3_AS_16_16_16_16})
        {
          Set(t, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_UNKNOWN, C::kCopy, 4, 16, rgba);
        }
        for (TextureFormat t : {TextureFormat::k_DXT4_5, TextureFormat::k_DXT4_5_AS_16_16_16_16})
        {
          Set(t, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_UNKNOWN, C::kCopy, 4, 16, rgba);
        }
        Set(TextureFormat::k_DXN, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_UNKNOWN, C::kCopy, 4,
            16, rggg);
        Set(TextureFormat::k_DXT5A, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_UNKNOWN, C::kCopy, 4,
            8, rrrr);
        Set(TextureFormat::k_DXT3A, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN, C::kDXT3A, 1,
            1, rrrr);
        Set(TextureFormat::k_CTX1, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_UNKNOWN, C::kCTX1, 1,
            2, rggg);
        Set(TextureFormat::k_24_8, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
            C::kDepthUnorm, 1, 4, rrrr);
        Set(TextureFormat::k_24_8_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
            C::kDepthFloat, 1, 4, rrrr);
        Set(TextureFormat::k_16, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM, C::kCopy,
            1, 2, rrrr);
        Set(TextureFormat::k_16_16, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM,
            DXGI_FORMAT_R16G16_SNORM, C::kCopy, 1, 4, rggg);
        Set(TextureFormat::k_16_16_16_16, DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
            DXGI_FORMAT_R16G16B16A16_SNORM, C::kCopy, 1, 8, rgba);
        for (TextureFormat t : {TextureFormat::k_16_EXPAND, TextureFormat::k_16_FLOAT})
        {
          Set(t, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, C::kCopy, 1, 2, rrrr);
        }
        for (TextureFormat t : {TextureFormat::k_16_16_EXPAND, TextureFormat::k_16_16_FLOAT})
        {
          Set(t, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, C::kCopy, 1, 4,
              rggg);
        }
        for (TextureFormat t : {TextureFormat::k_16_16_16_16_EXPAND, TextureFormat::k_16_16_16_16_FLOAT})
        {
          Set(t, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
              C::kCopy, 1, 8, rgba);
        }
        Set(TextureFormat::k_32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
            C::kCopy, 1, 4, rrrr);
        Set(TextureFormat::k_32_32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
            DXGI_FORMAT_R32G32_FLOAT, C::kCopy, 1, 8, rggg);
        Set(TextureFormat::k_32_32_32_32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
            DXGI_FORMAT_R32G32B32A32_FLOAT, C::kCopy, 1, 16, rgba);
      }
    };

    // Swaps `length` bytes (a multiple of the swap unit) per the Xenos endian mode.
    void SwapCopy(xenos::Endian endian, uint8_t *dst, const uint8_t *src, uint32_t length)
    {
      switch (endian)
      {
      case xenos::Endian::k8in16:
        for (uint32_t i = 0; i + 1 < length; i += 2)
        {
          dst[i] = src[i + 1];
          dst[i + 1] = src[i];
        }
        break;
      case xenos::Endian::k8in32:
        for (uint32_t i = 0; i + 3 < length; i += 4)
        {
          uint32_t v;
          std::memcpy(&v, src + i, 4);
          v = __builtin_bswap32(v);
          std::memcpy(dst + i, &v, 4);
        }
        break;
      case xenos::Endian::k16in32:
        for (uint32_t i = 0; i + 3 < length; i += 4)
        {
          uint32_t v;
          std::memcpy(&v, src + i, 4);
          v = (v >> 16) | (v << 16);
          std::memcpy(dst + i, &v, 4);
        }
        break;
      default:
        std::memcpy(dst, src, length);
        break;
      }
    }

    uint16_t Load16(const uint8_t *p)
    {
      uint16_t v;
      std::memcpy(&v, p, 2);
      return v;
    }
    uint32_t Load32(const uint8_t *p)
    {
      uint32_t v;
      std::memcpy(&v, p, 4);
      return v;
    }

    // Unorm bit replication to 16 bits.
    uint16_t Expand11(uint32_t v) { return uint16_t((v << 5) | (v >> 6)); }
    uint16_t Expand10(uint32_t v) { return uint16_t((v << 6) | (v >> 4)); }

    // Converts one swapped guest texel/block of a per-texel format into `out`.
    void ConvertTexel(TextureConversion c, const uint8_t *in, uint8_t *out, uint32_t host_bytes)
    {
      switch (c)
      {
      case TextureConversion::kR5G5B5A1:
      {
        uint16_t v = Load16(in);
        v = uint16_t((v & 0x83E0) | ((v & 0x1F) << 10) | ((v >> 10) & 0x1F));
        std::memcpy(out, &v, 2);
        break;
      }
      case TextureConversion::kR5G6B5:
      {
        uint16_t v = Load16(in);
        v = uint16_t((v & 0x07E0) | ((v & 0x1F) << 11) | ((v >> 11) & 0x1F));
        std::memcpy(out, &v, 2);
        break;
      }
      case TextureConversion::kRGBA4:
      {
        uint16_t v = Load16(in);
        v = uint16_t((v & 0xF0F0) | ((v & 0xF) << 8) | ((v >> 8) & 0xF));
        std::memcpy(out, &v, 2);
        break;
      }
      case TextureConversion::kR11G11B10:
      {
        uint32_t v = Load32(in);
        uint16_t o[4] = {Expand11(v & 0x7FF), Expand11((v >> 11) & 0x7FF), Expand10(v >> 22), 0xFFFF};
        std::memcpy(out, o, 8);
        break;
      }
      case TextureConversion::kR10G11B11:
      {
        uint32_t v = Load32(in);
        uint16_t o[4] = {Expand10(v & 0x3FF), Expand11((v >> 10) & 0x7FF), Expand11(v >> 21), 0xFFFF};
        std::memcpy(out, o, 8);
        break;
      }
      case TextureConversion::kDepthUnorm:
      {
        float f = xenos::UNorm24To32(Load32(in) >> 8);
        std::memcpy(out, &f, 4);
        break;
      }
      case TextureConversion::kDepthFloat:
      {
        float f = xenos::Float20e4To32(Load32(in) >> 8);
        std::memcpy(out, &f, 4);
        break;
      }
      default:
        std::memcpy(out, in, host_bytes);
        break;
      }
    }

    // SwapCopy with the endianness known at compile time.
    template <xenos::Endian E>
    inline void SwapT(uint8_t *dst, const uint8_t *src, uint32_t length)
    {
      if constexpr (E == xenos::Endian::k8in16)
      {
        for (uint32_t i = 0; i + 1 < length; i += 2)
        {
          uint16_t v;
          std::memcpy(&v, src + i, 2);
          v = __builtin_bswap16(v);
          std::memcpy(dst + i, &v, 2);
        }
      }
      else if constexpr (E == xenos::Endian::k8in32 || E == xenos::Endian::k16in32)
      {
        for (uint32_t i = 0; i + 3 < length; i += 4)
        {
          uint32_t v;
          std::memcpy(&v, src + i, 4);
          v = E == xenos::Endian::k8in32 ? __builtin_bswap32(v) : (v >> 16) | (v << 16);
          std::memcpy(dst + i, &v, 4);
        }
      }
      else
      {
        std::memcpy(dst, src, length);
      }
    }

    // One subresource for ConvertRuns.
    struct RunJob
    {
      const uint8_t *src;
      uint8_t *dst;
      uint32_t extent, bpb, bpb_log2, host_bytes;
      uint32_t cols, rows, depth, row_pitch, slice_pitch;
      bool tiled, is_3d;
      uint32_t xo, yo, zo, slice_offset, z_stride, guest_row_pitch, pitch_blocks, aligned_pitch, z_slice_rows;

      uint32_t Offset(uint32_t bx, uint32_t by, uint32_t z) const
      {
        if (!tiled)
        {
          return slice_offset + (z + zo) * z_stride + (by + yo) * guest_row_pitch + (bx + xo) * bpb;
        }
        if (is_3d)
        {
          return uint32_t(texture_util::GetTiledOffset3D(int32_t(bx + xo), int32_t(by + yo), int32_t(z + zo),
                                                         pitch_blocks, z_slice_rows, bpb_log2));
        }
        return slice_offset + uint32_t(TiledOffset2D(int32_t(bx + xo), int32_t(by + yo), aligned_pitch, bpb_log2));
      }
    };

    // Swaps and converts `n` adjacent guest blocks.
    template <xenos::Endian E, TextureConversion C>
    inline void EmitRun(const RunJob &j, uint8_t *out, const uint8_t *in, uint32_t n)
    {
      if constexpr (C == TextureConversion::kCopy)
      {
        SwapT<E>(out, in, n * j.bpb);
      }
      else
      {
        uint8_t texel[16];
        for (uint32_t i = 0; i < n; ++i)
        {
          SwapT<E>(texel, in + i * j.bpb, j.bpb);
          ConvertTexel(C, texel, out + i * j.host_bytes, j.host_bytes);
        }
      }
    }

    // Converts a subresource run by run: a linear row is one run; in a tiled
    // 2D row the blocks of each aligned group of min(8, 16 / bpb) are adjacent
    // (one 16-byte micro tile row); tiled 3D rows are joined where offsets
    // follow. The blocks within the level's extent are a prefix of each run.
    template <xenos::Endian E, TextureConversion C>
    void ConvertRunsT(const RunJob &j)
    {
      const uint32_t group = std::max(std::min(8u, 16u / j.bpb), 1u);
      auto fitting = [&](uint32_t offset, uint32_t n) {
        return offset >= j.extent ? 0u : std::min(n, (j.extent - offset) / j.bpb);
      };
      for (uint32_t z = 0; z < j.depth; ++z)
      {
        for (uint32_t by = 0; by < j.rows; ++by)
        {
          uint8_t *dst_row = j.dst + size_t(z) * j.slice_pitch + size_t(by) * j.row_pitch;
          if (!j.tiled)
          {
            uint32_t offset = j.Offset(0, by, z);
            if (uint32_t n = fitting(offset, j.cols))
            {
              EmitRun<E, C>(j, dst_row, j.src + offset, n);
            }
            continue;
          }
          if (!j.is_3d)
          {
            for (uint32_t bx = 0; bx < j.cols;)
            {
              uint32_t n = std::min(group - (bx + j.xo) % group, j.cols - bx);
              uint32_t offset = j.Offset(bx, by, z);
              if (uint32_t fit = fitting(offset, n))
              {
                EmitRun<E, C>(j, dst_row + bx * j.host_bytes, j.src + offset, fit);
              }
              bx += n;
            }
            continue;
          }
          uint32_t run_src = 0, run_bx = 0, run_n = 0;
          auto flush = [&] {
            if (run_n)
            {
              EmitRun<E, C>(j, dst_row + run_bx * j.host_bytes, j.src + run_src, run_n);
              run_n = 0;
            }
          };
          for (uint32_t bx = 0; bx < j.cols; ++bx)
          {
            uint32_t offset = j.Offset(bx, by, z);
            if (offset + j.bpb > j.extent)
            {
              flush();
              continue;
            }
            if (run_n && offset == run_src + run_n * j.bpb)
            {
              ++run_n;
              continue;
            }
            flush();
            run_src = offset;
            run_bx = bx;
            run_n = 1;
          }
          flush();
        }
      }
    }

    template <xenos::Endian E>
    bool ConvertRunsE(TextureConversion c, const RunJob &j)
    {
      using C = TextureConversion;
      switch (c)
      {
      case C::kCopy:
        ConvertRunsT<E, C::kCopy>(j);
        return true;
      case C::kR5G5B5A1:
        ConvertRunsT<E, C::kR5G5B5A1>(j);
        return true;
      case C::kR5G6B5:
        ConvertRunsT<E, C::kR5G6B5>(j);
        return true;
      case C::kRGBA4:
        ConvertRunsT<E, C::kRGBA4>(j);
        return true;
      case C::kR11G11B10:
        ConvertRunsT<E, C::kR11G11B10>(j);
        return true;
      case C::kR10G11B11:
        ConvertRunsT<E, C::kR10G11B11>(j);
        return true;
      case C::kDepthUnorm:
        ConvertRunsT<E, C::kDepthUnorm>(j);
        return true;
      case C::kDepthFloat:
        ConvertRunsT<E, C::kDepthFloat>(j);
        return true;
      default:
        return false;
      }
    }

    // False if the conversion has no run form (the decompressing ones).
    bool ConvertRuns(xenos::Endian e, TextureConversion c, const RunJob &j)
    {
      switch (e)
      {
      case xenos::Endian::k8in16:
        return ConvertRunsE<xenos::Endian::k8in16>(c, j);
      case xenos::Endian::k8in32:
        return ConvertRunsE<xenos::Endian::k8in32>(c, j);
      case xenos::Endian::k16in32:
        return ConvertRunsE<xenos::Endian::k16in32>(c, j);
      default:
        return ConvertRunsE<xenos::Endian::kNone>(c, j);
      }
    }
  } // namespace

  const HostTextureFormat &GetHostTextureFormat(TextureFormat format)
  {
    static const FormatTable table;
    return table.f[uint32_t(format) & 63];
  }

  uint32_t HostSwizzle(uint32_t guest_swizzle, uint32_t host_format_swizzle)
  {
    uint32_t result = 0;
    for (uint32_t i = 0; i < 4; ++i)
    {
      uint32_t c = (guest_swizzle >> (3 * i)) & 7;
      // 4 = constant 0, 5 = constant 1 (6 and 7 folded into those).
      uint32_t host = c >= xenos::XE_GPU_TEXTURE_SWIZZLE_0 ? (c & 5) : (host_format_swizzle >> (3 * c)) & 7;
      result |= host << (3 * i);
    }
    return result;
  }

  namespace
  {
    // 64 KB granules of physical memory known to be committed. Host memory of
    // the guest physical range is only ever committed (views of a file
    // mapping), so the cache only grows, except for ranges the runtime
    // reports as released (ForgetGuestPhysicalCommitted).
    constexpr uint32_t kGranuleShift = 16;
    std::atomic<uint64_t> g_committed[(kPhysicalSize >> kGranuleShift) / 64];

    bool QueryReadable(const uint8_t *p, const uint8_t *end, const uint8_t **region_end)
    {
      while (p < end)
      {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mbi.Protect)
        {
          return false;
        }
        p = static_cast<const uint8_t *>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end)
        {
          *region_end = p;
          return true;
        }
      }
      return true;
    }
  } // namespace

  bool IsGuestPhysicalReadable(uint32_t address, uint32_t size)
  {
    if (!size)
    {
      return true;
    }
    if (address >= kPhysicalSize || size > kPhysicalSize - address)
    {
      return false;
    }
    const uint8_t *base = Physical(0);
    uint32_t last = address + size - 1;
    for (uint32_t gi = address >> kGranuleShift; gi <= last >> kGranuleShift; ++gi)
    {
      if (g_committed[gi >> 6].load(std::memory_order_relaxed) & (uint64_t(1) << (gi & 63)))
      {
        continue;
      }
      uint32_t granule_start = gi << kGranuleShift;
      const uint8_t *region_end = nullptr;
      if (QueryReadable(base + granule_start, base + granule_start + 1, &region_end))
      {
        // Whole granules covered by the committed region become known.
        uint32_t end_granule = uint32_t((region_end - base) >> kGranuleShift);
        for (uint32_t k = gi; k < end_granule && k < (kPhysicalSize >> kGranuleShift); ++k)
        {
          g_committed[k >> 6].fetch_or(uint64_t(1) << (k & 63), std::memory_order_relaxed);
        }
        if (end_granule > gi)
        {
          continue;
        }
      }
      // Partially committed granule: check exactly the bytes needed.
      uint32_t lo = std::max(address, granule_start);
      uint32_t hi = std::min(last, granule_start + (1u << kGranuleShift) - 1);
      if (!QueryReadable(base + lo, base + hi + 1, nullptr))
      {
        return false;
      }
    }
    return true;
  }

  void ForgetGuestPhysicalCommitted(uint32_t address, uint32_t size)
  {
    if (!size || address >= kPhysicalSize)
    {
      return;
    }
    uint32_t last = std::min(address + (size - 1), kPhysicalSize - 1);
    for (uint32_t gi = address >> kGranuleShift; gi <= last >> kGranuleShift; ++gi)
    {
      g_committed[gi >> 6].fetch_and(~(uint64_t(1) << (gi & 63)), std::memory_order_relaxed);
    }
  }

  bool DescribeGuestTexture(const xenos::xe_gpu_texture_fetch_t &fetch, GuestTextureDesc &out)
  {
    if (fetch.type != xenos::FetchConstantType::kTexture)
    {
      return false;
    }
    const HostTextureFormat &host = GetHostTextureFormat(fetch.format);
    if (host.conversion == TextureConversion::kUnsupported)
    {
      return false;
    }
    out.fetch = fetch;
    out.dimension = fetch.dimension;
    out.format = fetch.format;
    uint32_t w1, h1, d1;
    texture_util::GetSubresourcesFromFetchConstant(fetch, &w1, &h1, &d1, &out.base_page, &out.mip_page,
                                                   &out.min_level, &out.max_level);
    out.width = w1 + 1;
    out.height = out.dimension == xenos::DataDimension::k1D ? 1 : h1 + 1;
    out.depth_or_array_size = d1 + 1;
    if (!out.base_page && !out.mip_page)
    {
      return false;
    }
    if (out.width > 16384 || out.height > 16384 || out.depth_or_array_size > 2048)
    {
      return false;
    }
    out.layout = texture_util::GetGuestTextureLayout(out.dimension, fetch.pitch, out.width, out.height,
                                                     out.depth_or_array_size, fetch.tiled, out.format,
                                                     fetch.packed_mips, out.base_page != 0, out.max_level);
    out.max_level = std::min(out.max_level, out.layout.max_level);
    out.min_level = std::min(out.min_level, out.max_level);
    out.base_address = out.base_page << 12;
    out.base_size = out.base_page ? out.layout.base.level_data_extent_bytes : 0;
    out.mip_address = out.mip_page << 12;
    out.mip_size = (out.mip_page && out.max_level) ? out.layout.mips_total_extent_bytes : 0;
    return out.base_size || out.mip_size;
  }

  uint32_t HostLevelWidth(const GuestTextureDesc &desc, uint32_t host_level)
  {
    return std::max(desc.width >> (desc.min_level + host_level), 1u);
  }

  uint32_t HostLevelHeight(const GuestTextureDesc &desc, uint32_t host_level)
  {
    return std::max(desc.height >> (desc.min_level + host_level), 1u);
  }

  uint64_t HashGuestTexture(const GuestTextureDesc &desc)
  {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    if (desc.base_size)
    {
      if (!IsGuestPhysicalReadable(desc.base_address, desc.base_size))
      {
        return 0;
      }
      h = XXH3_64bits_withSeed(Physical(desc.base_address), desc.base_size, h);
    }
    if (desc.mip_size)
    {
      if (!IsGuestPhysicalReadable(desc.mip_address, desc.mip_size))
      {
        return 0;
      }
      h = XXH3_64bits_withSeed(Physical(desc.mip_address), desc.mip_size, h);
    }
    return h ? h : 1;
  }

  bool LoadGuestSubresource(const GuestTextureDesc &desc, uint32_t host_level, uint32_t slice, uint8_t *dst,
                            uint32_t row_pitch, uint32_t slice_pitch, uint32_t rows, uint32_t row_bytes)
  {
    const auto &layout = desc.layout;
    const uint32_t level = desc.min_level + host_level;
    const uint32_t stored = std::min(level, layout.packed_level);
    const auto &ll = level == 0 ? layout.base : layout.mips[stored];
    const uint32_t level_address =
        level == 0 ? desc.base_address : desc.mip_address + layout.mip_offsets_bytes[stored];
    if (!ll.row_pitch_bytes || !IsGuestPhysicalReadable(level_address, ll.level_data_extent_bytes))
    {
      return false;
    }
    return ConvertGuestSubresource(desc, host_level, slice, Physical(desc.base_address), Physical(desc.mip_address),
                                   dst, row_pitch, slice_pitch, rows, row_bytes);
  }

  bool ConvertGuestSubresource(const GuestTextureDesc &desc, uint32_t host_level, uint32_t slice,
                               const uint8_t *base_data, const uint8_t *mip_data, uint8_t *dst, uint32_t row_pitch,
                               uint32_t slice_pitch, uint32_t rows, uint32_t row_bytes, bool runs)
  {
    const HostTextureFormat &host = GetHostTextureFormat(desc.format);
    const FormatInfo *info = FormatInfo::Get(desc.format);
    const uint32_t bw = info->block_width, bh = info->block_height, bpb = info->bytes_per_block();
    const uint32_t bpb_log2 = rex::log2_floor(bpb);
    const auto &layout = desc.layout;
    const uint32_t level = desc.min_level + host_level;
    const bool is_3d = desc.dimension == xenos::DataDimension::k3D;
    const uint32_t depth = is_3d ? std::max(desc.depth_or_array_size >> level, 1u) : 1;
    const uint32_t w_blocks = (std::max(desc.width >> level, 1u) + bw - 1) / bw;
    const uint32_t h_blocks = (std::max(desc.height >> level, 1u) + bh - 1) / bh;

    const uint32_t stored = std::min(level, layout.packed_level);
    const auto &ll = level == 0 ? layout.base : layout.mips[stored];
    const uint8_t *src = level == 0 ? base_data : (mip_data ? mip_data + layout.mip_offsets_bytes[stored] : nullptr);
    if (!ll.row_pitch_bytes || !src)
    {
      return false;
    }
    uint32_t xo = 0, yo = 0, zo = 0;
    if (level >= layout.packed_level)
    {
      texture_util::GetPackedMipOffset(desc.width, desc.height, is_3d ? desc.depth_or_array_size : 1,
                                       desc.format, level, xo, yo, zo);
    }
    const uint32_t extent = ll.level_data_extent_bytes;
    const uint32_t pitch_blocks = ll.row_pitch_bytes / bpb;
    const uint32_t slice_offset = slice * ll.array_slice_stride_bytes;
    const uint32_t z_stride = ll.row_pitch_bytes * ll.z_slice_stride_block_rows;
    const bool tiled = desc.fetch.tiled;
    const xenos::Endian endian = desc.fetch.endianness;
    const bool decompress = host.conversion == TextureConversion::kCTX1 ||
                            host.conversion == TextureConversion::kDXT3A;

    const uint32_t aligned_pitch = (pitch_blocks + 31) & ~31u;
    auto source_offset = [&](uint32_t bx, uint32_t by, uint32_t z) -> uint32_t {
      if (!tiled)
      {
        return slice_offset + (z + zo) * z_stride + (by + yo) * ll.row_pitch_bytes + (bx + xo) * bpb;
      }
      if (is_3d)
      {
        return uint32_t(texture_util::GetTiledOffset3D(int32_t(bx + xo), int32_t(by + yo), int32_t(z + zo),
                                                       pitch_blocks, ll.z_slice_stride_block_rows, bpb_log2));
      }
      return slice_offset + uint32_t(TiledOffset2D(int32_t(bx + xo), int32_t(by + yo), aligned_pitch, bpb_log2));
    };

    // Blocks adjacent in guest memory are swapped and converted as one run
    // (see ConvertRuns). A run's swap is the blocks' own as long as a block
    // holds whole swap units.
    const uint32_t swap_unit = endian == xenos::Endian::k8in16 ? 2 : endian == xenos::Endian::kNone ? 1 : 4;
    if (runs && !decompress && bpb >= swap_unit && bpb <= 16 &&
        (host.conversion != TextureConversion::kCopy || host.host_block_bytes == bpb))
    {
      RunJob job;
      job.src = src;
      job.dst = dst;
      job.extent = extent;
      job.bpb = bpb;
      job.bpb_log2 = bpb_log2;
      job.host_bytes = host.host_block_bytes;
      job.cols = std::min(w_blocks, row_bytes / host.host_block_bytes);
      job.rows = std::min(h_blocks, rows);
      job.depth = depth;
      job.row_pitch = row_pitch;
      job.slice_pitch = slice_pitch;
      job.tiled = tiled;
      job.is_3d = is_3d;
      job.xo = xo;
      job.yo = yo;
      job.zo = zo;
      job.slice_offset = slice_offset;
      job.z_stride = z_stride;
      job.guest_row_pitch = ll.row_pitch_bytes;
      job.pitch_blocks = pitch_blocks;
      job.aligned_pitch = aligned_pitch;
      job.z_slice_rows = ll.z_slice_stride_block_rows;
      if (ConvertRuns(endian, host.conversion, job))
      {
        return true;
      }
    }

    uint8_t block[16];
    for (uint32_t z = 0; z < depth; ++z)
    {
      uint8_t *dst_slice = dst + size_t(z) * slice_pitch;
      for (uint32_t by = 0; by < h_blocks; ++by)
      {
        for (uint32_t bx = 0; bx < w_blocks; ++bx)
        {
          uint32_t offset = source_offset(bx, by, z);
          if (offset + bpb > extent)
          {
            continue;
          }
          SwapCopy(endian, block, src + offset, bpb);
          if (!decompress)
          {
            // Host block == guest block (texel or BCn block).
            if (by >= rows || (bx + 1) * host.host_block_bytes > row_bytes)
            {
              continue;
            }
            ConvertTexel(host.conversion, block, dst_slice + size_t(by) * row_pitch + bx * host.host_block_bytes,
                         host.host_block_bytes);
            continue;
          }
          // 4x4 guest block -> 4x4 host texels.
          if (by * 4 + 3 >= rows || (bx * 4 + 4) * host.host_block_bytes > row_bytes)
          {
            continue;
          }
          uint8_t *out = dst_slice + size_t(by) * 4 * row_pitch + bx * 4 * host.host_block_bytes;
          if (host.conversion == TextureConversion::kCTX1)
          {
            rex::graphics::texture_conversion::ConvertTexelCTX1ToR8G8(xenos::Endian::kNone, out, block,
                                                                       row_pitch);
          }
          else
          {
            // DXT3A: 4 bits of alpha per texel, rows of 16 bits.
            for (uint32_t ty = 0; ty < 4; ++ty)
            {
              uint16_t row = Load16(block + ty * 2);
              for (uint32_t tx = 0; tx < 4; ++tx)
              {
                out[ty * row_pitch + tx] = uint8_t(((row >> (tx * 4)) & 0xF) * 17);
              }
            }
          }
        }
      }
    }
    return true;
  }
} // namespace ao2::gpu
