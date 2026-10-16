// texture_loader.h - Guest (Xenos) textures -> linear host data.
//
// CPU side of the native renderer's texture cache: describes a texture from
// its fetch constant, maps the guest format to a DXGI format and converts one
// subresource (untiling, endian swap, format conversion) into host memory.
// Layout math comes from the SDK's texture_util (Xenia, BSD).
#pragma once

#include <dxgiformat.h>

#include <cstdint>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace ao2::gpu
{
  enum class TextureConversion : uint8_t
  {
    kUnsupported,
    kCopy,          // endian swap only
    kR5G5B5A1,      // -> B5G5R5A1 (red/blue swapped)
    kR5G6B5,        // -> B5G6R5
    kRGBA4,         // -> B4G4R4A4
    kR11G11B10,     // k_10_11_11 -> RGBA16
    kR10G11B11,     // k_11_11_10 -> RGBA16
    kDepthUnorm,    // 24_8 -> R32_FLOAT
    kDepthFloat,    // 24_8_FLOAT (20e4) -> R32_FLOAT
    kCTX1,          // -> R8G8 (decompressed)
    kDXT3A,         // -> R8 (decompressed)
  };

  struct HostTextureFormat
  {
    DXGI_FORMAT resource = DXGI_FORMAT_UNKNOWN; // possibly typeless
    DXGI_FORMAT unorm = DXGI_FORMAT_UNKNOWN;    // view for unsigned data
    DXGI_FORMAT snorm = DXGI_FORMAT_UNKNOWN;    // view for signed data, if any
    TextureConversion conversion = TextureConversion::kUnsupported;
    uint8_t host_block = 1;       // host block width/height (4 for BCn)
    uint8_t host_block_bytes = 0; // bytes per host block
    uint32_t swizzle = 0;         // host component of each guest component (XE_GPU_TEXTURE_SWIZZLE)
  };

  const HostTextureFormat &GetHostTextureFormat(rex::graphics::xenos::TextureFormat format);

  // Everything needed to locate a texture in guest memory.
  struct GuestTextureDesc
  {
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch;
    rex::graphics::xenos::DataDimension dimension;
    rex::graphics::xenos::TextureFormat format;
    uint32_t width = 0, height = 0;
    uint32_t depth_or_array_size = 1; // depth for 3D, slices otherwise (6 for cubes)
    uint32_t base_page = 0, mip_page = 0;
    uint32_t min_level = 0, max_level = 0; // guest levels present
    rex::graphics::texture_util::TextureGuestLayout layout;
    // Guest memory ranges (physical) covered by the base and the mips.
    uint32_t base_address = 0, base_size = 0, mip_address = 0, mip_size = 0;
  };

  // False for unusable fetch constants (wrong type, unsupported format, empty).
  bool DescribeGuestTexture(const rex::graphics::xenos::xe_gpu_texture_fetch_t &fetch, GuestTextureDesc &out);

  // Host extent of guest `level` (level 0 = desc.min_level), in texels.
  uint32_t HostLevelWidth(const GuestTextureDesc &desc, uint32_t host_level);
  uint32_t HostLevelHeight(const GuestTextureDesc &desc, uint32_t host_level);

  // Converts one host subresource: `host_level` (guest level min_level + host_level),
  // `slice` (array slice/cube face; 0 for 3D), all z slices for 3D. Rows of host
  // blocks are written `row_pitch` bytes apart, z slices `slice_pitch` apart;
  // `rows`/`row_bytes` bound the destination. Returns false if guest memory is
  // not readable.
  bool LoadGuestSubresource(const GuestTextureDesc &desc, uint32_t host_level, uint32_t slice, uint8_t *dst,
                            uint32_t row_pitch, uint32_t slice_pitch, uint32_t rows, uint32_t row_bytes);

  // The conversion LoadGuestSubresource does, from the texture's base and mip
  // data at host addresses. `runs` = false converts block by block
  // (tools/texture_loader_test.cpp compares the two).
  bool ConvertGuestSubresource(const GuestTextureDesc &desc, uint32_t host_level, uint32_t slice,
                               const uint8_t *base_data, const uint8_t *mip_data, uint8_t *dst, uint32_t row_pitch,
                               uint32_t slice_pitch, uint32_t rows, uint32_t row_bytes, bool runs = true);

  // texture_util::GetTiledOffset2D with the pitch already aligned to 32
  // blocks: the SDK's is a call into rexruntime.dll, and the loader needs it
  // for every block of every texture upload.
  inline int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t aligned_pitch, uint32_t bpb_log2)
  {
    int32_t macro = ((x >> 5) + (y >> 5) * int32_t(aligned_pitch >> 5)) << (bpb_log2 + 7);
    int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
    int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
    return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
  }

  // Hash of the guest memory backing the texture (0 if not readable).
  uint64_t HashGuestTexture(const GuestTextureDesc &desc);

  // True if [address, address + size) of guest physical memory is committed and readable.
  bool IsGuestPhysicalReadable(uint32_t address, uint32_t size);
  // Drops cached knowledge that the range is committed (memory released).
  void ForgetGuestPhysicalCommitted(uint32_t address, uint32_t size);

  // Combines the fetch constant swizzle with the host format swizzle into a
  // D3D12 Shader4ComponentMapping (without the always-set bit).
  uint32_t HostSwizzle(uint32_t guest_swizzle, uint32_t host_format_swizzle);
} // namespace ao2::gpu
