// renderer.cpp - Native D3D12 renderer (see renderer.h).
//
// Model:
// - Render targets are host textures keyed by their EDRAM placement
//   (base tile, format, pitch, MSAA) - EDRAM itself is not emulated.
// - Resolves copy host render targets into host textures keyed by the
//   destination guest address; texture fetches of that address use them.
// - Vertex data is fetched by the translated shaders straight from guest
//   physical memory, which is exposed to the GPU as a D3D12 heap opened over
//   the guest memory file mapping (no copies, no tracking).
// - Index data is copied (with endian swap / primitive conversion) per draw.
#include "renderer.h"

#include "../../frame_spikes.h"

#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <intrin.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <bitset>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "fast_hash.h"
#include "hlsl_shared.h"
#include "native_graphics_system.h"
#include "post_effects.h"
#include "overlay.h"
#include "../../perf_overlay.h"
#include "pm4.h"
#include "shader_cache.h"
#include "texture_loader.h"
#include "../../cyrillic_fonts.h"
#include "../../aspect_ratio.h"
#include "../../keyboard_prompts.h"
#include "../../mouse_aim.h"
#include "../../precise_sleep.h"

#include <rex/graphics/pipeline/texture/info.h>

using Microsoft::WRL::ComPtr;
using rex::graphics::FormatInfo;

REXCVAR_DECLARE(int32_t, ao2_native_gpu_capture_frame);
REXCVAR_DECLARE(int32_t, ao2_native_gpu_capture_slow_us);
REXCVAR_DECLARE(bool, ao2_native_gpu_static_tex_post);
REXCVAR_DECLARE(int32_t, ao2_native_gpu_writeback_texels);
REXCVAR_DECLARE(bool, ao2_native_gpu_debug_layer);
REXCVAR_DECLARE(bool, ao2_native_gpu_async_pipelines);
REXCVAR_DEFINE_BOOL(ao2_prompts_debug, false, "Debug",
                    "Log where split-screen button pictures are drawn (native_gpu.log)");
REXCVAR_DECLARE(double, ao2_native_gpu_resolution_scale);
REXCVAR_DEFINE_STRING(ao2_ambient_occlusion, "off", "GPU",
                      "Ambient occlusion added to the scene: off, ssao, hbao or gtao");
REXCVAR_DEFINE_STRING(ao2_anti_aliasing, "off", "GPU",
                      "Post-process anti-aliasing of the scene: off, fxaa or smaa");
REXCVAR_DEFINE_INT32(ao2_motion_blur, 0, "GPU", "Camera motion blur: 0 off, 1 low, 2 high");
REXCVAR_DEFINE_INT32(ao2_depth_of_field, 0, "GPU",
                     "Depth of field (what lies beyond the aim point blurs): 0 off, 1 on");
REXCVAR_DEFINE_INT32(ao2_shadow_resolution, 0, "GPU",
                     "Shadow map resolution: 0 = as the render resolution, or 1024, 2048, 4096 (host pixels of the "
                     "game's shadow buffer)");
REXCVAR_DEFINE_BOOL(ao2_native_gpu_alias_stencil_clear, true, "GPU",
                    "D3D's stencil clears through a 4x MSAA alias of the scene depth clear the stencil in place "
                    "instead of transferring the depth over and back");
REXCVAR_DEFINE_BOOL(ao2_native_gpu_resolve_swap, true, "GPU",
                    "A color resolve followed by a clear hands the render target's resource to the texture and "
                    "gives the render target the texture's old one (no copy); the exponent bias is applied when "
                    "the texture is sampled");
REXCVAR_DEFINE_BOOL(ao2_native_gpu_resolve_lend, true, "GPU",
                    "A color resolve without a clear also hands the render target's resource to the texture; the "
                    "render target's contents are copied back from it only if it is used again while it still "
                    "holds EDRAM tiles (usually a draw of another format takes them first)");
REXCVAR_DEFINE_BOOL(ao2_native_gpu_resolve_pack, true, "GPU",
                    "A 7e3 float surface resolved as 2_10_10_10 bits is encoded straight into the texture and "
                    "keeps the EDRAM, and a draw copying such saved bits back unchanged is skipped (the hit "
                    "effect: no transfers between the two formats)");
REXCVAR_DEFINE_BOOL(ao2_native_gpu_skip_depth_restore, true, "GPU",
                    "A resolve's depth clear waits for the next draw; the game's restore of that depth from the "
                    "resolved texture then needs neither the clear nor itself");
REXCVAR_DEFINE_INT32(ao2_post_debug, 0, "Debug",
                     "Post effects: 1 = show the ambient occlusion instead of the scene");
REXCVAR_DECLARE(bool, ao2_native_gpu_vsync);
REXCVAR_DECLARE(int32_t, ao2_native_gpu_small_target_width);
REXCVAR_DECLARE(bool, ao2_native_gpu_capture_draws);
REXCVAR_DECLARE(std::string, ao2_native_gpu_capture_after_draws);
REXCVAR_DECLARE(std::string, ao2_native_gpu_skip_shaders);
REXCVAR_DECLARE(bool, ao2_native_gpu_debug_upload_vertices);
REXCVAR_DECLARE(int32_t, ao2_native_gpu_submit_draws);
REXCVAR_DECLARE(bool, ao2_native_gpu_frame_pacing);
REXCVAR_DECLARE(int32_t, ao2_native_gpu_pacing_margin_us);
REXCVAR_DECLARE(bool, ao2_fps_unlock);
int32_t FpsTarget(); // midasm_hooks.cpp

namespace ao2::gpu
{
  namespace xenos = rex::graphics::xenos;
  namespace reg = rex::graphics::reg;
  namespace texture_util = rex::graphics::texture_util;
  using namespace rex::graphics;

  namespace
  {
    constexpr uint32_t kFrameCount = 3;
    constexpr uint64_t kUploadSize = 256ull << 20;
    constexpr uint32_t kViewsPerFrame = 65536;
    constexpr uint32_t kSamplerHeapSize = 2048;
    constexpr uint32_t kCpuSamplerCount = 1024;
    constexpr uint32_t kTransientRtvCount = 64;
    constexpr uint64_t kReadbackSize = 4ull << 20;
    constexpr uint32_t kMaxTextureBindings = 16;
    constexpr uint32_t kPhysicalSize = 0x20000000;
    constexpr uint64_t kPhysicalFileOffset = 0x100000000ull;
    constexpr uint32_t kBackBufferCount = 3;

    // The menu probe (RendererSetMenuProbe): the draws that sample a texture
    // of the size asked for (the menu's render target, put on the slabs of
    // the Shell level) are drawn again with a variant of their pixel shader
    // that writes that texture's coordinates (HlslModification::probe_binding)
    // into g.probe.target, with the depth test but no writes; the texel under
    // the cursor is read back at the end of the frame.
    constexpr uint64_t kProbeNone = UINT64_MAX - 1;
    constexpr uint64_t kProbeOff = UINT64_MAX;
    constexpr DXGI_FORMAT kProbeFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
    std::atomic<uint32_t> g_probe_size{0};   // width | height << 16 of the texture, 0 = off
    std::atomic<uint32_t> g_probe_cursor{0}; // guest picture pixel x | y << 16
    // Last result: u | v << 16 (UNORM16), bit 32 = found, bits 33.. = the size it was for.
    std::atomic<uint64_t> g_probe_result{0};

    enum RootParam : uint32_t
    {
      kRootVSConstants,
      kRootPSConstants,
      kRootSystemConstants,
      kRootSharedMemory,
      kRootPSTextures,
      kRootPSSamplers,
      kRootVSTextures,
      kRootVSSamplers,
      kRootCount,
    };

    // Rectangle lists: expand each triangle into a quad (4th vertex completes
    // the parallelogram opposite the corner at the right angle). The strip
    // (c, a, b, f) keeps the winding of the guest triangle (c, a, b is a
    // rotation of it), so face culling treats the quad like the guest does.
    const char kRectGS[] = R"(
struct XeVertexOut {
  float4 o[16] : TEXCOORD0;
  float4 misc : TEXCOORD16;
  float4 position : SV_Position;
};
[maxvertexcount(4)]
void main(triangle XeVertexOut v[3], inout TriangleStream<XeVertexOut> s) {
  float2 p0 = v[0].position.xy / v[0].position.w;
  float2 p1 = v[1].position.xy / v[1].position.w;
  float2 p2 = v[2].position.xy / v[2].position.w;
  float d01 = dot(p1 - p0, p1 - p0), d12 = dot(p2 - p1, p2 - p1), d20 = dot(p0 - p2, p0 - p2);
  uint c = (d12 >= d01 && d12 >= d20) ? 0u : ((d20 >= d01) ? 1u : 2u);
  uint a = (c + 1u) % 3u, b = (c + 2u) % 3u;
  XeVertexOut f;
  [unroll] for (uint i = 0; i < 16; ++i) { f.o[i] = v[a].o[i] + v[b].o[i] - v[c].o[i]; }
  f.misc = v[c].misc;
  f.position = v[a].position + v[b].position - v[c].position;
  s.Append(v[c]); s.Append(v[a]); s.Append(v[b]); s.Append(f);
}
)";

    // Fullscreen triangle sampling the [0, uv_scale] part of the source, then
    // the guest display gamma ramp (256-entry DC_LUT table, 10 bits per
    // component: blue 0:9, green 10:19, red 20:29), interpolated because the
    // source is scaled. The ramp is a buffer, not a constant buffer: every
    // pixel reads other entries, and constant reads that differ across a wave
    // run one after another (0.3 ms of the present at 2560x1600).
    const char kBlitShader[] = R"(
Texture2D t : register(t0);
SamplerState smp : register(s0);
cbuffer c : register(b0) { float2 uv_scale; uint swap_red_blue; };
StructuredBuffer<uint> gamma_ramp : register(t1);
struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; };
VO vs(uint id : SV_VertexID) {
  VO o; o.uv = float2((id << 1) & 2, id & 2); o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
float Ramp(uint i, uint shift) { return float((gamma_ramp[i] >> shift) & 0x3FFu) * (1.0 / 1023.0); }
float4 ps(VO i) : SV_Target {
  float3 c = t.Sample(smp, i.uv * uv_scale).rgb;
  c = saturate(swap_red_blue != 0u ? c.bgr : c) * 255.0;
  float3 r;
  [unroll] for (uint k = 0; k < 3; ++k) {
    uint i0 = min(uint(c[k]), 254u);
    uint shift = (2u - k) * 10u;
    r[k] = lerp(Ramp(i0, shift), Ramp(i0 + 1u, shift), c[k] - float(i0));
  }
  return float4(r, 1.0);
}
)";

    // Dear ImGui overlay: ImDrawVert (position, uv, RGBA8 color) times the
    // texture, alpha blended over the presented picture.
    const char kOverlayShader[] = R"(
cbuffer c : register(b0) { float2 scale; float2 offset; };
Texture2D t : register(t0);
SamplerState smp : register(s0);
struct VI { float2 p : POSITION; float2 uv : TEXCOORD0; float4 c : COLOR0; };
struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 c : COLOR0; };
VO vs(VI i) { VO o; o.p = float4(i.p * scale + offset, 0, 1); o.uv = i.uv; o.c = i.c; return o; }
float4 ps(VO i) : SV_Target { return i.c * t.Sample(smp, i.uv); }
)";

    // Host render target standing for an EDRAM surface. Color resources are
    // typeless: `format` is the linear view (raw bits), gamma surfaces render
    // through `view_srgb`.
    struct RenderTarget
    {
      ComPtr<ID3D12Resource> resource;
      D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
      DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
      uint32_t width = 0, height = 0, samples = 1;
      uint32_t scale4 = 4; // host pixels per guest pixel, in quarters (see SurfaceScale4)
      bool depth = false;
      bool fixed16 = false; // k_16_16(_16_16): values stored /32 (EDRAM range -32...32)
      D3D12_CPU_DESCRIPTOR_HANDLE view = {};
      D3D12_CPU_DESCRIPTOR_HANDLE view_srgb = {};
      // EDRAM placement (see ClaimEdram).
      uint32_t edram_base = 0, edram_pitch = 0, edram_format = 0, edram_id = 0;
      bool is_64bpp = false;
      uint64_t claim_generation = UINT64_MAX;
      D3D12_RECT claimed = {};
      // EDRAM tiles known to hold one value (after clears), so transfers from
      // them become clears. Component 0: the color cell / depth bits (raw cell
      // with the stencil byte zero), 1: the stencil byte (depth only).
      std::bitset<2048> uniform_tiles[2];
      uint32_t uniform_value[2] = {};
      // Depth: some draw had the stencil test enabled (otherwise its stencil
      // is never transferred in).
      bool stencil_used = false;
      // Bumped whenever the resource's contents may change (draws, transfers
      // and clears into it, a resolve taking its resource): EdramSave.
      uint64_t writes = 0;
    };

    // Resolve destination (render-to-texture), keyed by guest address.
    struct HostTexture
    {
      ComPtr<ID3D12Resource> resource;
      D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
      DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // view for unsigned sampling
      DXGI_FORMAT snorm = DXGI_FORMAT_UNKNOWN;  // view for signed sampling, if any
      uint32_t swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
      uint32_t width = 0, height = 0; // guest texels
      uint32_t scale4 = 4;            // of the render target it is resolved from (host size = ScaleSize)
      uint64_t last_frame = 0;
      // Guest write epoch at the resolve and the watched destination range: if
      // the CPU later writes there, the memory holds the CPU's data, not ours.
      uint32_t epoch = 0;
      uint32_t checked_epoch = 0; // global write epoch of the last WrittenSince check
      uint32_t guest_size = 0;
      // The last resolve swapped red and blue (guest ARGB layout: the game's
      // fetch swizzle undoes it; the present blit and captures do too).
      bool red_blue_swapped = false;
      bool from_depth = false;
      bool small_source = false; // resolved from a small render target (see SmallTarget)
      // Applied when sampled: the resolve's exponent bias when the texture is
      // the render target's resource itself (SwapResolve), otherwise 1.
      float exp_scale = 1.0f;
      // Guest-sized (box-downscaled) copy for draws into small render targets
      // (see PrepareTextures), rebuilt when a resolve changed the texture.
      uint64_t generation = 0;
      ComPtr<ID3D12Resource> lowres;
      D3D12_RESOURCE_STATES lowres_state = D3D12_RESOURCE_STATE_COMMON;
      uint64_t lowres_generation = UINT64_MAX;
    };

    // Texture loaded from guest memory.
    struct CachedTexture
    {
      ComPtr<ID3D12Resource> resource;
      D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
      GuestTextureDesc desc;
      const HostTextureFormat *host = nullptr;
      uint32_t mip_levels = 1, array_size = 1, depth = 1;
      uint32_t epoch = 0;         // guest write epoch the data was loaded at
      uint32_t checked_epoch = 0; // global write epoch at the last check
      uint64_t validated_frame = UINT64_MAX;
      bool loaded = false, failed = false, cube = false;
      bool replaced = false; // our picture, not the guest's (keyboard_prompts)
      bool prompt = false;   // ...and that picture is a button's (not a font page's letters)
      uint32_t prompts_generation = 0; // keyboard_prompts' switch it was looked at for
      // Split-screen: the guest's own picture of a replaced button, for the
      // gamepad player's part of the screen (see keyboard_prompts.h).
      std::unique_ptr<CachedTexture> guest;
    };

    struct PipelineKey
    {
      const void *vs = nullptr, *ps = nullptr;
      uint8_t rt_formats[4] = {};
      uint8_t ds_format = 0;
      uint8_t samples = 1;
      uint8_t topology = 0; // D3D12_PRIMITIVE_TOPOLOGY_TYPE
      uint8_t rect_gs = 0;
      uint32_t blend[4] = {}; // RB_BLENDCONTROL, 0 = disabled
      uint8_t write_mask[4] = {};
      uint32_t depth_control = 0;
      uint32_t stencil_masks = 0; // read | write << 8 (front), bf << 16
      uint32_t raster = 0;
      float depth_bias_slope = 0.0f; // host SlopeScaledDepthBias (see PolygonOffset)
      uint32_t reserved = 0;         // no implicit padding (keys are compared and stored as bytes)
      bool operator==(const PipelineKey &o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
    };
    struct PipelineKeyHash
    {
      size_t operator()(const PipelineKey &k) const
      {
        // Looked up for every draw (in memory only: not stored).
        return size_t(XXH3_64bits(&k, sizeof(k)));
      }
    };

    // Resolve result to copy back into guest memory once the GPU is done
    // (the CPU reads some of them, e.g. the 1x1 average luminance).
    struct Writeback
    {
      uint64_t offset;                 // in the frame's readback buffer
      uint32_t row_pitch;              // of the read back rows
      uint32_t x0, y0, width, height;  // texels
      uint32_t dest_base, dest_pitch;  // guest (tiled) destination
      uint32_t bytes_per_texel;
      xenos::Endian128 endian;
      uint32_t scale4;                 // of the texture (the read back rows are its host texels)
      uint32_t host_x0, host_y0;       // host texel the read back rows start at
      std::vector<uint8_t> before;     // destination bytes when the resolve was recorded
      std::vector<uint32_t> addresses; // guest address of each texel (UINT32_MAX: not readable)
      uint32_t lowest = 0, highest = 0; // range of the readable texels' bytes
    };

    // GPU time categories (timestamps written when the category changes).
    enum GpuCategory : uint8_t
    {
      kGpuDraw,
      kGpuTransfer,
      kGpuResolve,
      kGpuClear,
      kGpuUpload,
      kGpuCopy,
      kGpuPresent,
      kGpuPost,
      kGpuIdle, // from the end of a list submitted mid-frame to the start of the next one
      kGpuCategoryCount,
      kGpuEnd = 0xFE, // last timestamp of a command list
      kGpuNone = 0xFF,
    };
    const char *const kGpuCategoryNames[kGpuCategoryCount] = {"draw", "transfer", "resolve", "clear",
                                                              "upload", "copy", "present", "post", "idle"};
    constexpr uint32_t kTimestampsPerFrame = 16384;
    constexpr uint32_t kOcclusionPerFrame = 4096; // host occlusion queries per frame slot

    // Guest occlusion query (between two EVENT_WRITE_ZPD). Each of its draws is
    // counted by a host occlusion query ("segment"; contiguous in the frame
    // slot's range), summed once the GPU is done and written to guest memory.
    struct OcclusionQuery
    {
      uint32_t address = 0;              // guest sample count struct of the end event
      uint32_t slot = 0, first = 0, count = 0;
      uint64_t epoch = 0;                // of the slot's range when the first segment was recorded
      uint64_t fence = 0;                // submission with the segments
      uint32_t draws = 0, recorded = 0;
      uint32_t fallback = 1000;          // samples reported when unknown
      bool unknown = false;              // a draw was not counted: report the fallback count
      uint32_t expect[8] = {};           // the struct as the end event left it (WriteOcclusionResult)
    };

    struct Frame
    {
      uint32_t oq_count = 0, oq_resolved = 0; // occlusion segments recorded / resolved
      // Timestamps of this frame slot: category starting at each one.
      uint32_t ts_count = 0, ts_resolved = 0;
      uint8_t ts_last = kGpuNone;
      uint8_t ts_category[kTimestampsPerFrame] = {};
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12Resource> upload;
      uint8_t *upload_mapped = nullptr;
      uint64_t upload_offset = 0;
      uint32_t views_used = 0;
      uint64_t fence_value = 0;
      ComPtr<ID3D12Resource> readback;
      uint8_t *readback_mapped = nullptr;
      uint64_t readback_offset = 0;
      std::vector<Writeback> writebacks;
      // The menu probe's texel in `readback` (kProbeNone: nothing drawn
      // there, kProbeOff: not asked for) and the texture it was for.
      uint64_t probe_offset = UINT64_MAX;
      uint32_t probe_size = 0;
    };

    struct State
    {
      bool initialized = false;
      bool failed = false;
      ComPtr<IDXGIFactory6> factory;
      ComPtr<ID3D12Device> device;
      ComPtr<ID3D12InfoQueue> info_queue;
      uint64_t debug_messages_logged = 0;
      ComPtr<ID3D12CommandQueue> queue;
      ComPtr<ID3D12GraphicsCommandList> list;
      ComPtr<ID3D12GraphicsCommandList1> list1;
      ComPtr<ID3D12Fence> fence;
      HANDLE fence_event = nullptr;
      uint64_t fence_value = 0;
      Frame frames[kFrameCount];
      ComPtr<ID3D12QueryHeap> timestamp_heap;
      ComPtr<ID3D12Resource> timestamp_readback;
      const uint64_t *timestamps = nullptr; // mapped readback
      uint64_t timestamp_frequency = 0;
      uint64_t gpu_ticks[kGpuCategoryCount] = {};
      // Guest occlusion queries.
      ComPtr<ID3D12QueryHeap> occlusion_heap;
      ComPtr<ID3D12Resource> occlusion_readback;
      const uint64_t *occlusion_results = nullptr; // mapped readback
      uint64_t occlusion_epoch = 0;                // incremented when a slot's range restarts
      bool occlusion_active = false;
      OcclusionQuery occlusion;                    // being recorded
      std::vector<OcclusionQuery> occlusion_ended; // in the open command list
      // Submitted, waiting for the GPU; shared with the occlusion thread (occlusion_mutex).
      std::deque<OcclusionQuery> occlusion_pending;
      std::mutex occlusion_mutex;
      std::condition_variable occlusion_submitted;
      std::thread occlusion_thread;
      std::atomic<uint64_t> occlusion_queries = 0, occlusion_visible = 0, occlusion_unknown = 0;
      std::atomic<uint64_t> occlusion_stale = 0, writeback_stale = 0; // results not written (memory reused)
      uint64_t occlusion_submits = 0;
      uint32_t draws_since_submit = 0; // recorded in the open command list
      uint64_t early_submits = 0, early_submits_idle = 0;
      // Traced frame: GPU time of every draw (written to gpu_draws.txt when done).
      struct DrawTiming
      {
        uint64_t draw, vs, ps;
        uint32_t begin, end; // timestamp indices in the frame slot
        uint32_t pixels;     // viewport area (guest pixels)
      };
      std::vector<DrawTiming> draw_timings;
      uint32_t draw_timings_slot = UINT32_MAX;
      // ao2_native_gpu_capture_slow_us: the next frame is traced (GPU time of
      // the slow frame that triggered it), the traced one is copied to slow_N_*.
      // Only after a run of slow frames: a lone spike is over by the next
      // frame, so its trace would show an ordinary one.
      bool slow_capture_pending = false;
      uint32_t slow_capture_us = 0, slow_traced_us = 0, slow_captures = 0, slow_run = 0;
      uint64_t slow_capture_frame = 0;
      uint32_t frame_index = 0;
      uint64_t frame_number = 0;
      bool list_open = false;

      // Swap chain on the game window, created on the first swap after the
      // window exists and resized with its client area.
      HWND hwnd = nullptr;
      ComPtr<IDXGISwapChain3> swapchain;
      ComPtr<ID3D12Resource> backbuffers[kBackBufferCount];
      ComPtr<ID3D12DescriptorHeap> backbuffer_rtv_heap;
      D3D12_CPU_DESCRIPTOR_HANDLE backbuffer_rtv[kBackBufferCount] = {};
      uint32_t swapchain_width = 0, swapchain_height = 0;
      bool tearing = false;

      // Resources replaced while earlier frames may still use them.
      std::vector<std::pair<uint64_t, ComPtr<ID3D12Resource>>> graveyard;

      ComPtr<ID3D12Heap> shared_heap;
      ComPtr<ID3D12Resource> shared_memory;
      // Fallback when guest memory can't be mapped: a GPU mirror of physical
      // memory, refreshed per draw with the vertex ranges the shader fetches.
      bool shared_memory_is_mirror = false;
      D3D12_RESOURCE_STATES mirror_state = D3D12_RESOURCE_STATE_COMMON;
      uint64_t mirror_bytes = 0;

      ComPtr<ID3D12RootSignature> root_signature;
      ComPtr<ID3D12DescriptorHeap> view_heap;    // shader visible
      ComPtr<ID3D12DescriptorHeap> sampler_heap; // shader visible
      ComPtr<ID3D12DescriptorHeap> cpu_view_heap;
      ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
      uint32_t view_size = 0, sampler_size = 0, rtv_size = 0, dsv_size = 0;
      uint32_t cpu_views_used = 0, rtvs_used = 0, dsvs_used = 0, samplers_used = 0;
      D3D12_CPU_DESCRIPTOR_HANDLE null_srv_2d = {}, null_srv_3d = {}, null_srv_cube = {};
      ComPtr<ID3D12DescriptorHeap> cpu_sampler_heap;
      std::unordered_map<uint32_t, uint32_t, FastHash> sampler_descriptors; // key -> cpu heap index
      std::unordered_map<uint64_t, uint32_t, FastHash> sampler_tables;      // keys hash -> visible heap index

      std::unordered_map<uint64_t, std::unique_ptr<CachedTexture>, FastHash> textures;
      std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
      std::vector<UINT> footprint_rows;
      std::vector<UINT64> footprint_row_bytes;

      std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>, FastHash> render_targets;
      std::unordered_map<uint32_t, std::unique_ptr<HostTexture>, FastHash> resolved;
      // Other formats/sizes recently resolved to the same address (games
      // alternate them within a frame - the scene and its 2_10_10_10 copy,
      // two views' downsamples): reused instead of recreated every time.
      std::unordered_multimap<uint32_t, std::unique_ptr<HostTexture>> resolved_spare;
      std::unordered_map<PipelineKey, ComPtr<ID3D12PipelineState>, PipelineKeyHash> pipelines;

      // Pipeline storage (see "Pipeline storage" below).
      struct StoredShader
      {
        uint8_t type;
        uint64_t hash, modification;
        std::vector<uint32_t> dwords; // big-endian, as in guest memory
      };
      struct StoredPipeline
      {
        uint64_t vs_hash, vs_modification, ps_hash, ps_modification;
        PipelineKey key; // vs / ps null
      };
      struct PsoJob
      {
        PipelineKey key;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
      };
      FILE *storage = nullptr;
      bool storage_loaded = false;
      std::unordered_set<uint64_t> stored_shaders, stored_pipelines;
      std::vector<StoredShader> prewarm_shaders;
      std::vector<StoredPipeline> prewarm_pipelines;
      size_t prewarm_shader_pos = 0;
      std::unordered_map<uint64_t, const CompiledShader *> prewarm_compiled;
      uint64_t prewarm_frame = ~0ull;
      uint32_t prewarm_created = 0;
      std::chrono::steady_clock::time_point prewarm_start;
      bool prewarm_logged = false;
      std::mutex pso_mutex;
      std::condition_variable pso_cv;
      std::deque<PsoJob> pso_jobs;
      std::vector<std::pair<PipelineKey, ComPtr<ID3D12PipelineState>>> pso_done;
      std::unordered_set<PipelineKey, PipelineKeyHash> pso_queued;
      bool pso_thread_started = false;
      std::vector<uint8_t> rect_gs;
      ComPtr<ID3D12PipelineState> blit_pso;
      ComPtr<ID3D12RootSignature> blit_root;
      // Dear ImGui overlay (overlay.h), created on first use.
      ComPtr<ID3D12PipelineState> overlay_pso;
      ComPtr<ID3D12RootSignature> overlay_root;
      bool overlay_failed = false;
      std::unordered_map<uint64_t, ComPtr<ID3D12Resource>> overlay_textures;

      ComPtr<ID3D12RootSignature> resolve_root;
      std::vector<uint8_t> resolve_vs, resolve_ps[6];
      std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> resolve_pipelines;
      ComPtr<ID3D12DescriptorHeap> transient_rtv_heap;
      uint32_t transient_rtvs_used = 0;
      uint64_t capture_index = 0;
      std::unordered_set<const void *> traced_textures; // CPU textures captured in the traced frame
      // Host pixels per guest pixel along each axis, in quarters (4 = 1x,
      // 6 = 1.5x, 8 = 2x): render targets and resolve destinations are that
      // much larger, guest-space rectangles are scaled when recorded. Small
      // render targets (at most `small_width` guest pixels wide, see
      // SmallTarget) use `small_scale4`, the next whole scale.
      uint32_t scale4 = 4, small_scale4 = 4, small_width = 0;
      // Command list state set by the last draw: redundant calls are skipped.
      // `valid` is cleared by anything else that sets state on the list.
      struct BoundState
      {
        bool valid = false;
        ID3D12PipelineState *pso = nullptr;
        uint32_t rtv_count = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {}, dsv = {};
        D3D12_VIEWPORT viewport = {};
        D3D12_RECT scissor = {};
        float blend[4] = {};
        uint32_t stencil_ref = 0;
        D3D12_GPU_VIRTUAL_ADDRESS cbv[3] = {};
        D3D12_GPU_DESCRIPTOR_HANDLE tables[4] = {};
        D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
      } bound;
      // The menu probe's target (host size and samples of the render target
      // the probed draws went to) and its single-sample copy for MSAA.
      struct
      {
        ComPtr<ID3D12Resource> target, resolved;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        D3D12_RESOURCE_STATES resolved_state = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        uint32_t width = 0, height = 0, samples = 0, scale4 = 4;
        uint64_t drawn_frame = UINT64_MAX; // frame_number it was cleared and drawn in
      } probe;
      // Float constants uploaded in this upload generation, per stage: reused
      // while the guest did not write them.
      struct ConstantUpload
      {
        uint64_t generation = UINT64_MAX;
        uint32_t base = 0, version0 = 0, version1 = 0;
        D3D12_GPU_VIRTUAL_ADDRESS address = 0;
      } constant_uploads[2];
      uint64_t upload_generation = 0; // incremented when an upload ring is reset
      // Fetch constant (6 dwords) -> texture validated this frame.
      struct FetchCacheEntry
      {
        uint32_t words[6];
        CachedTexture *texture;
        uint64_t frame;
      };
      std::unordered_map<uint64_t, FetchCacheEntry, FastHash> fetch_cache;
      // Guest display gamma ramp (DC_LUT 256-entry table), applied when presenting.
      uint32_t gamma_ramp[256] = {};
      uint64_t resolves_copied = 0, resolves_drawn = 0, resolves_swapped = 0, resolves_packed = 0, lowres_copies = 0;
      uint64_t transfers_skipped = 0; // claims of fully overwritten rectangles
      uint64_t transfers_cleared = 0; // transfers of uniform tiles done as clears
      uint64_t stencil_clears_redirected = 0; // ClearStencilOnOwner
      // A resolve's depth clear not done yet (FlushPendingDepthClear): rt null when there is none.
      struct PendingDepthClear
      {
        RenderTarget *rt = nullptr;
        D3D12_RECT rect = {};
        uint32_t dest = 0; // the resolve's destination
        float z = 0.0f;
        uint32_t raw = 0; // RB_DEPTH_CLEAR
      } pending_depth;
      // The last resolve that copied a color surface's bits unchanged into a
      // texture (see SkipCopyBack): `origin` held those bits in its resource
      // then (the surface itself, or the owner they were transferred from).
      struct EdramSave
      {
        RenderTarget *origin = nullptr;
        uint64_t origin_writes = 0;
        RenderTarget *surface = nullptr; // the resolve's source
        D3D12_RECT rect = {};            // guest pixels of the surface
        uint32_t dest = 0;    // the resolve's destination
        uint64_t generation = 0;
      } edram_save;
      uint64_t copy_backs_skipped = 0, copy_back_saves = 0;
      // Render targets whose resource a resolve without a clear handed to the
      // texture at `dest` (SwapResolve's lend): their contents are there.
      struct Lent
      {
        RenderTarget *rt;
        uint32_t dest;
        ID3D12Resource *resource; // the texture's resource holding them
      };
      std::vector<Lent> lent;
      uint64_t resolves_lent = 0, lent_returned = 0;
      uint64_t depth_restores_skipped = 0;
      uint64_t resources_created = 0; // committed resources created (kernel calls) since the last stats line

      RenderTarget *edram_owner[2048] = {};
      uint64_t edram_generation = 0;
      uint32_t next_rt_id = 1;
      ComPtr<ID3D12RootSignature> transfer_root;
      std::vector<uint8_t> transfer_vs, transfer_ps[8];
      std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> transfer_pipelines;
      uint64_t transfers = 0;
      uint64_t writebacks = 0;

      // Frame capture: textures read back at the end of the traced frame.
      struct Capture
      {
        ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        DXGI_FORMAT format;
        std::string name;
        bool red_blue_swapped = false;
      };
      std::vector<Capture> captures;
      // Pixel probe of the captured frame ("probe X Y" in capture.trigger):
      // render target 0 at guest pixel (X, Y) after every draw.
      struct ProbeEntry
      {
        uint64_t draw, vs, ps;
        uint32_t edram_base, edram_format;
        DXGI_FORMAT format;
        uint32_t offset;
      };
      int32_t probe_x = -1, probe_y = -1;
      ComPtr<ID3D12Resource> probe_buffer;
      std::vector<ProbeEntry> probes;

      // Guest physical memory write tracking (runtime write watches): the
      // epoch of the last CPU write per 4 KB page, and which pages of the GPU
      // mirror are current.
      std::unique_ptr<std::atomic<uint32_t>[]> page_epoch;
      std::unique_ptr<std::atomic<uint64_t>[]> mirror_valid;
      // Pages that are (or were) resolve destinations, and how often a write
      // elsewhere in their 256 KB block did not mark them as written.
      std::unique_ptr<std::atomic<uint64_t>[]> resolve_pages;
      std::atomic<uint64_t> resolve_pages_spared{0};
      uint64_t resolved_dropped = 0;
      std::atomic<uint32_t> write_epoch{1};
      void *invalidation_handle = nullptr;

      bool null_table_ready = false;
      uint32_t present_address = 0;
      bool frontbuffer_miss_logged = false;
      uint64_t draws = 0, draws_skipped = 0, draws_pending = 0;
      uint64_t flushes = 0, texture_uploads = 0, texture_upload_bytes = 0;
      uint64_t texture_binds = 0, resolved_binds = 0, null_binds = 0;
      // Accumulated over the stats interval.
      std::chrono::steady_clock::time_point last_stats = std::chrono::steady_clock::now();
      uint64_t stats_frame = 0, pso_created = 0;
      uint64_t time_pso_ns = 0, time_hash_ns = 0, time_upload_ns = 0;
      // This frame's stalls of the GPU thread (logged as a hitch when long).
      uint64_t frame_pso_ns = 0, frame_shader_ns = 0, frame_upload_ns = 0;
      uint32_t frame_pso_new = 0, frame_shader_slow = 0, frame_pso_queued = 0;
      std::chrono::steady_clock::time_point last_present;
      uint64_t phase_ns[10] = {};
      uint64_t time_fence_ns = 0;
      std::atomic<uint64_t> time_present_ns = 0;
    };

    State g;

    // ------------------------------------------------------------ Resolution scale

    // Scales are in quarters (`scale4`): at any of them an EDRAM tile is a
    // whole number of host pixels. At a fractional scale guest pixels are not
    // all as many host pixels wide (at 1.5x: one, two, one, two...): a host
    // pixel belongs to the guest pixel its centre is in, which is what the
    // rasterizer covers of a guest rectangle stretched over the host surface.

    // Guest pixel edge -> host pixel edge (the first host pixel at or after it).
    LONG ScaleEdge(LONG v, uint32_t scale4)
    {
      return (v * LONG(scale4) + 1) >> 2;
    }

    // Guest-space rectangle -> host pixels of a surface of that scale.
    D3D12_RECT ScaleRect(const D3D12_RECT &r, uint32_t scale4)
    {
      return {ScaleEdge(r.left, scale4), ScaleEdge(r.top, scale4), ScaleEdge(r.right, scale4),
              ScaleEdge(r.bottom, scale4)};
    }

    // Host size of a surface `size` guest pixels long.
    uint32_t ScaleSize(uint32_t size, uint32_t scale4)
    {
      return (size * scale4 + 3) / 4;
    }

    // The host pixel of guest pixel `v` nearest to its centre.
    uint32_t CenterPixel(uint32_t v, uint32_t scale4)
    {
      return ((2 * v + 1) * scale4 - 1) >> 3;
    }

    // Small render targets (downsampled copies, blur, bloom, DOF) are emulated
    // at guest resolution: their draws sample resolved textures through
    // guest-sized copies, and what is resolved from them is sampled that way
    // everywhere. Their passes step through the source in guest texels
    // (bilinear taps between texels, point samples on texel edges), so at a
    // scaled resolution neighbouring host pixels would take different texels:
    // the striped pause/DOF blur. A 4x surface is half as wide as the 1x view
    // of the same EDRAM (D3D clears a surface through its 4x alias): both are
    // of one kind.
    bool SmallTarget(uint32_t pitch, uint32_t samples)
    {
      return g.scale4 > 4 && pitch * (samples >= 4 ? 2u : 1u) <= g.small_width;
    }

    // Scale of the render targets of a surface. Small ones take a whole scale:
    // each of their guest pixels must be made of the same host pixels (their
    // passes reduce the picture down to the 1x1 luminance the CPU reads).
    uint32_t SurfaceScale4(uint32_t pitch, uint32_t samples)
    {
      return SmallTarget(pitch, samples) ? g.small_scale4 : g.scale4;
    }

    // Presents on a thread of its own: DXGI's Present costs ~0.5 ms of CPU
    // per frame (more when it throttles), which the command thread would
    // otherwise spend before it gets to the next frame's commands. One present
    // is in flight at most; the command thread waits for it before it touches
    // the swap chain again (back buffer index, resize).
    class Presenter
    {
    public:
      void Submit(IDXGISwapChain3 *swapchain, UINT flags)
      {
        std::unique_lock lock(mutex_);
        if (!thread_.joinable())
        {
          thread_ = std::thread([this] { Run(); });
          SetThreadDescription(thread_.native_handle(), L"Native GPU present");
          SetThreadPriority(thread_.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
        }
        swapchain_ = swapchain;
        flags_ = flags;
        pending_ = true;
        wake_.notify_one();
      }

      // Waits until the submitted present has been issued; returns its result.
      HRESULT Wait()
      {
        std::unique_lock lock(mutex_);
        done_.wait(lock, [this] { return !pending_; });
        return result_;
      }

    private:
      void Run()
      {
        std::unique_lock lock(mutex_);
        for (;;)
        {
          wake_.wait(lock, [this] { return pending_; });
          IDXGISwapChain3 *swapchain = swapchain_;
          UINT flags = flags_;
          lock.unlock();
          auto pace_start = std::chrono::steady_clock::now();
          Pace();
          auto start = std::chrono::steady_clock::now();
          if (ao2::spikes::Enabled())
          {
            ao2::spikes::Mark(ao2::spikes::kPaced, std::chrono::duration<double, std::milli>(start - pace_start).count());
          }
          // Vertical sync: wait for the refresh; tearing only without it.
          bool vsync = REXCVAR_GET(ao2_native_gpu_vsync);
          HRESULT hr = swapchain->Present(vsync ? 1 : 0, vsync ? flags & ~UINT(DXGI_PRESENT_ALLOW_TEARING) : flags);
          ao2::perf::OnPresent();
          if (ao2::spikes::Enabled())
          {
            auto end = std::chrono::steady_clock::now();
            ao2::spikes::Mark(ao2::spikes::kPresented, std::chrono::duration<double, std::milli>(end - start).count());
            ao2::spikes::AfterPresent(
                last_end_.time_since_epoch().count() ? std::chrono::duration<double, std::milli>(end - last_end_).count()
                                                     : 0.0,
                1000.0 / FpsTarget());
            last_end_ = end;
          }
          g.time_present_ns += uint64_t(
              std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
          lock.lock();
          result_ = hr;
          pending_ = false;
          done_.notify_all();
        }
      }

      // Frame pacing. The frame limiter evens out the starts of the game
      // thread's frames, but not how long each takes to get here: with
      // ao2_decouple a simulation step's frame comes milliseconds later
      // than a render-only one, so at twice the simulation rate the present
      // intervals took turns being short and long. Frames are presented on
      // a grid of slots one frame time apart; one that is early waits for
      // its slot.
      //
      // The grid is kept a margin after the latest frames' arrivals: a
      // simulation step's frame whose world tick or drawing takes a few
      // milliseconds longer than usual still makes its slot. Held only from
      // the last present (the grid's margin 0), each such frame was a
      // visible 12-15 ms spike at 120 FPS (frame_spikes.log). A late frame
      // is presented at once, and the grid moves on from it with a margin
      // that much wider; the margin shrinks again while it is not needed,
      // and the grid follows the arrivals both ways (the limiter's frames
      // are a little longer than the frame time).
      void Pace()
      {
        using clock = std::chrono::steady_clock;
        using ms_d = std::chrono::duration<double, std::milli>;
        if (REXCVAR_GET(ao2_native_gpu_frame_pacing) && REXCVAR_GET(ao2_fps_unlock))
        {
          const double period = 1000.0 / FpsTarget();
          const double max_margin = double(REXCVAR_GET(ao2_native_gpu_pacing_margin_us)) / 1000.0;
          auto arrival = clock::now();
          double slack = ms_d(next_slot_ - arrival).count();
          if (next_slot_.time_since_epoch().count() == 0 || slack < -4.0 * period || slack > 2.0 * period)
          {
            // First frame, or after a load or a hitch: a new grid.
            next_slot_ = arrival;
            slack = 0.0;
            margin_ = std::min(kMinMarginMs, max_margin);
            window_start_ = arrival;
            window_min_slack_ = 1e9;
          }
          else if (slack < 0.0)
          {
            // Late: presented now, the grid moves to it and keeps a wider margin.
            next_slot_ = arrival;
            margin_ = std::min(margin_ - slack, max_margin);
          }
          else
          {
            // The timer wakes ~0.5 ms late in the foreground, milliseconds late in
            // the background: the sleeper leaves that much, the rest is spun.
            sleeper_.Sleep(slack / 1000.0, 0.0006);
            while (clock::now() < next_slot_)
            {
              YieldProcessor();
            }
          }
          window_min_slack_ = std::min(window_min_slack_, std::max(slack, 0.0));
          if (ao2::spikes::Enabled())
          {
            ao2::spikes::Mark(ao2::spikes::kPaceSlot, slack);
            ao2::spikes::Mark(ao2::spikes::kPaceMargin, margin_);
          }
          if (ms_d(arrival - window_start_).count() >= kWindowMs)
          {
            // The grid moves when the margin is thinner or wider than wanted,
            // a little at a time (a jump later would itself be a long frame);
            // the margin shrinks slowly.
            double error = window_min_slack_ - margin_;
            next_slot_ -= std::chrono::duration_cast<clock::duration>(ms_d(std::clamp(error, -kLaterStepMs, kEarlierStepMs)));
            margin_ = std::max(std::min(kMinMarginMs, max_margin), margin_ - kMarginDecayMs);
            window_start_ = arrival;
            window_min_slack_ = 1e9;
          }
          next_slot_ += std::chrono::duration_cast<clock::duration>(ms_d(period));
        }
        auto now = clock::now();
        // How even the presents are: every 600, to the log.
        double ms = std::chrono::duration<double, std::milli>(now - last_present_).count();
        if (ms < 100.0)
        {
          ++paced_;
          interval_sum_ += ms;
          interval_sq_ += ms * ms;
          interval_max_ = std::max(interval_max_, ms);
          step_sum_ += std::abs(ms - last_interval_);
          last_interval_ = ms;
          if (paced_ == 600)
          {
            double mean = interval_sum_ / paced_;
            NativeLog("present intervals: avg %.2f ms (%.1f FPS), deviation %.2f, frame to frame %.2f, max %.1f\n",
                      mean, 1000.0 / mean, std::sqrt(std::max(interval_sq_ / paced_ - mean * mean, 0.0)),
                      step_sum_ / paced_, interval_max_);
            paced_ = 0;
            interval_sum_ = interval_sq_ = interval_max_ = step_sum_ = 0.0;
          }
        }
        last_present_ = now;
      }

      uint32_t paced_ = 0;
      double interval_sum_ = 0.0, interval_sq_ = 0.0, interval_max_ = 0.0, step_sum_ = 0.0, last_interval_ = 0.0;

      std::chrono::steady_clock::time_point last_present_;
      // The pacing grid (Pace).
      static constexpr double kMinMarginMs = 0.5;    // the margin it shrinks back to
      static constexpr double kWindowMs = 250.0;      // how often the grid and the margin are adjusted
      static constexpr double kEarlierStepMs = 0.05;  // at most this much earlier per window
      static constexpr double kLaterStepMs = 0.2;     // at most this much later per window
      static constexpr double kMarginDecayMs = 0.025; // the margin shrinks by this much per window
      std::chrono::steady_clock::time_point next_slot_, window_start_;
      double margin_ = kMinMarginMs, window_min_slack_ = 1e9;
      std::chrono::steady_clock::time_point last_end_; // after Present, as the overlay times it
      ao2::PreciseSleeper sleeper_;
      std::mutex mutex_;
      std::condition_variable wake_, done_;
      std::thread thread_;
      IDXGISwapChain3 *swapchain_ = nullptr;
      UINT flags_ = 0;
      bool pending_ = false;
      HRESULT result_ = S_OK;
    };
    Presenter g_presenter;

    // Recent command list operations, dumped when Close() reports an
    // invalid call (which would otherwise remove the device on execution).
    struct Breadcrumb
    {
      const char *what;
      uint64_t a, b, c, d;
    };
    Breadcrumb g_crumbs[256];
    uint32_t g_crumb_next = 0;

    // Starts GPU time category `category` in the open command list.
    void GpuMark(uint8_t category)
    {
      Frame &f = g.frames[g.frame_index];
      if (!g.timestamp_heap || category == f.ts_last || f.ts_count + 2 > kTimestampsPerFrame)
      {
        return;
      }
      g.list->EndQuery(g.timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                       g.frame_index * kTimestampsPerFrame + f.ts_count);
      f.ts_category[f.ts_count++] = category;
      f.ts_last = category;
    }

    // Writes a timestamp now (keeping `category`); returns its index or UINT32_MAX.
    uint32_t GpuStamp(uint8_t category)
    {
      Frame &f = g.frames[g.frame_index];
      if (!g.timestamp_heap || f.ts_count + 2 > kTimestampsPerFrame)
      {
        return UINT32_MAX;
      }
      g.list->EndQuery(g.timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                       g.frame_index * kTimestampsPerFrame + f.ts_count);
      f.ts_category[f.ts_count] = category;
      f.ts_last = category;
      return f.ts_count++;
    }

    void Crumb(const char *what, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0)
    {
      g_crumbs[g_crumb_next++ & 255] = {what, a, b, c, d};
    }

    // Closes the command list; on failure logs the recent operations and
    // returns false (the list must not be executed).
    bool CloseList();

    bool InitResolvePipelines();
    bool InitTransferPipelines();
    void ApplyWritebacks(Frame &f);
    void Transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES &state, D3D12_RESOURCE_STATES to);
    void ReturnLent(RenderTarget *rt, uint32_t dest = UINT32_MAX);
    void WaitIdle();
    // Post effects / shadow resolution (see "Post effects", "Shadow resolution").
    struct HostTexture;
    void PostBeforeSample(HostTexture &tex, uint32_t address);
    uint32_t ShadowScale4(uint32_t base, uint32_t format, uint32_t pitch, uint32_t *rows = nullptr);

    // The runtime's memory lives as long as the process (asked for per texel
    // and per page in places).
    rex::memory::Memory *Mem()
    {
      static rex::memory::Memory *const mem = rex::Runtime::instance()->memory();
      return mem;
    }

    uint64_t Elapsed(std::chrono::steady_clock::time_point start)
    {
      return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                          .count());
    }

    // Time spent in the phases of RendererDraw (stats line).
    const char *const kPhaseNames[10] = {"setup", "rt", "pso", "vertex", "tex", "smp", "tables", "edram", "const", "record"};
    // Nanoseconds per time stamp counter tick, measured once against the
    // steady clock. Draws time themselves with the TSC: a dozen
    // QueryPerformanceCounter calls per draw were ~5% of the GPU thread.
    double NsPerTsc()
    {
      static const double ns = [] {
        auto c0 = std::chrono::steady_clock::now();
        uint64_t t0 = __rdtsc();
        while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(5))
        {
        }
        uint64_t t1 = __rdtsc();
        auto c1 = std::chrono::steady_clock::now();
        return double(std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count()) / double(t1 - t0);
      }();
      return ns;
    }

    uint64_t TscElapsedNs(uint64_t start)
    {
      return uint64_t(double(__rdtsc() - start) * NsPerTsc());
    }

    struct PhaseTimer
    {
      double ns_per_tsc = NsPerTsc();
      uint64_t t = __rdtsc();
      void Mark(uint32_t phase)
      {
        uint64_t now = __rdtsc();
        g.phase_ns[phase] += uint64_t(double(now - t) * ns_per_tsc);
        t = now;
      }
    };

    // Writes new debug layer messages (warnings and errors) to the log.
    void FlushDebugMessages()
    {
      if (!g.info_queue)
      {
        return;
      }
      uint64_t count = g.info_queue->GetNumStoredMessages();
      for (uint64_t i = 0; i < count && g.debug_messages_logged < 2000; ++i)
      {
        SIZE_T size = 0;
        g.info_queue->GetMessage(i, nullptr, &size);
        std::vector<uint8_t> buffer(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(buffer.data());
        if (SUCCEEDED(g.info_queue->GetMessage(i, message, &size)) &&
            message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        {
          ++g.debug_messages_logged;
          NativeLog("D3D12 %s (frame %llu, draws %llu): %s\n",
                    message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" : "warning",
                    (unsigned long long)g.frame_number, (unsigned long long)g.draws, message->pDescription);
        }
      }
      g.info_queue->ClearStoredMessages();
    }

    // ------------------------------------------------------------ Frame trace

    // --ao2_native_gpu_capture_frame=N: every draw and resolve of frame N
    // goes to frame_trace.txt.
    FILE *g_trace = nullptr;

    void Trace(const char *format, ...)
    {
      if (!g_trace)
      {
        return;
      }
      va_list args;
      va_start(args, format);
      std::vfprintf(g_trace, format, args);
      va_end(args);
    }

    void UpdateTraceFrame()
    {
      int32_t capture = REXCVAR_GET(ao2_native_gpu_capture_frame);
      if (g_trace)
      {
        std::fclose(g_trace);
        g_trace = nullptr;
        NativeLog("frame %llu traced to frame_trace.txt\n", (unsigned long long)g.frame_number - 1);
      }
      // Also on demand: create capture.trigger in the working directory.
      bool triggered = false;
      if (g.frame_number % 30 == 0)
      {
        std::error_code ec;
        if (std::filesystem::exists("capture.trigger", ec))
        {
          // Optional "probe X Y": a pixel of render target 0 after every draw.
          g.probe_x = g.probe_y = -1;
          if (FILE *f = std::fopen("capture.trigger", "r"))
          {
            int x, y;
            if (std::fscanf(f, " probe %d %d", &x, &y) == 2)
            {
              g.probe_x = x;
              g.probe_y = y;
            }
            std::fclose(f);
          }
        }
        triggered = std::filesystem::remove("capture.trigger", ec);
      }
      if (g.slow_capture_pending && !triggered)
      {
        g.slow_capture_pending = false;
        g.slow_traced_us = g.slow_capture_us;
        triggered = true;
      }
      if ((capture > 0 && g.frame_number == uint64_t(capture)) || triggered)
      {
        g.capture_index = 0;
        g.traced_textures.clear();
        std::error_code ec;
        std::filesystem::remove_all("capture", ec);
        g_trace = std::fopen("frame_trace.txt", "w");
        Trace("frame %llu\n", (unsigned long long)g.frame_number);
      }
    }

    // Queues a read back of `resource` (subresource 0) for the frame capture.
    // --ao2_native_gpu_skip_shaders: comma separated "vsXXXXXXXX"/"psXXXXXXXX"
    // (first 8 hex digits of the shader hash); such draws are not executed.
    bool SkipDrawForDebug(uint64_t vs_hash, uint64_t ps_hash)
    {
      static std::string parsed;
      static std::unordered_set<uint64_t> vs_set, ps_set;
      const std::string &list = REXCVAR_GET(ao2_native_gpu_skip_shaders);
      if (list.empty())
      {
        return false;
      }
      if (list != parsed)
      {
        parsed = list;
        vs_set.clear();
        ps_set.clear();
        for (const char *p = list.c_str(); *p;)
        {
          bool vs = p[0] == 'v' && p[1] == 's', ps = p[0] == 'p' && p[1] == 's';
          char *end = nullptr;
          unsigned long long n = (vs || ps) ? std::strtoull(p + 2, &end, 16) : 0;
          if (!(vs || ps) || end == p + 2)
          {
            ++p;
            continue;
          }
          (vs ? vs_set : ps_set).insert(n);
          p = end;
        }
      }
      return vs_set.count(vs_hash >> 32) != 0 || (ps_hash && ps_set.count(ps_hash >> 32) != 0);
    }

    // --ao2_native_gpu_capture_after_draws: comma separated draw numbers, or
    // "vsXXXXXXXX" (the first 8 hex digits of a vertex shader hash).
    bool CaptureDrawRequested(uint64_t draw, uint64_t vs_hash)
    {
      static std::string parsed;
      static std::unordered_set<uint64_t> draws, shaders;
      const std::string &list = REXCVAR_GET(ao2_native_gpu_capture_after_draws);
      if (list.empty())
      {
        return false;
      }
      if (list != parsed)
      {
        parsed = list;
        draws.clear();
        shaders.clear();
        for (const char *p = list.c_str(); *p;)
        {
          bool shader = p[0] == 'v' && p[1] == 's';
          char *end = nullptr;
          unsigned long long n = std::strtoull(shader ? p + 2 : p, &end, shader ? 16 : 10);
          if (end == (shader ? p + 2 : p))
          {
            ++p;
            continue;
          }
          (shader ? shaders : draws).insert(n);
          p = end;
        }
      }
      return draws.count(draw) != 0 || shaders.count(vs_hash >> 32) != 0;
    }

    // `subresource` 1 of a depth/stencil target is its stencil plane.
    void CaptureTexture(ID3D12Resource *resource, D3D12_RESOURCE_STATES &state, DXGI_FORMAT format,
                        const std::string &name, UINT subresource = 0)
    {
      if (!g_trace)
      {
        return;
      }
      D3D12_RESOURCE_DESC rd = resource->GetDesc();
      if (rd.SampleDesc.Count > 1)
      {
        return; // multisampled: not copyable to a buffer
      }
      State::Capture c;
      UINT64 total = 0;
      g.device->GetCopyableFootprints(&rd, subresource, 1, 0, &c.footprint, nullptr, nullptr, &total);
      D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
      D3D12_RESOURCE_DESC bd = {};
      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      // Whole padded rows (the copyable size leaves the last row unpadded; the
      // read back maps RowPitch x Height).
      bd.Width = std::max<UINT64>(total, UINT64(c.footprint.Footprint.RowPitch) * c.footprint.Footprint.Height);
      bd.Height = 1;
      bd.DepthOrArraySize = 1;
      bd.MipLevels = 1;
      bd.SampleDesc.Count = 1;
      bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      HRESULT hr = g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&c.buffer));
      if (FAILED(hr))
      {
        NativeLog("capture %s skipped: readback buffer of %llu bytes failed (0x%08X)\n", name.c_str(),
                  (unsigned long long)total, unsigned(hr));
        return;
      }
      Transition(resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst = {c.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint = c.footprint;
      D3D12_TEXTURE_COPY_LOCATION src = {resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      src.SubresourceIndex = subresource;
      GpuMark(kGpuCopy);
      Crumb("capture copy", uint64_t(resource), format);
      g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      c.format = format;
      c.name = name;
      g.captures.push_back(std::move(c));
    }

    constexpr uint32_t kProbeStride = 256, kMaxProbes = 4096;

    // Copies the probed pixel of `rt` after draw `draw` (frame capture only).
    void ProbePixel(RenderTarget &rt, uint64_t draw, uint64_t vs, uint64_t ps)
    {
      if (!g_trace || g.probe_x < 0 || rt.samples > 1 || g.probes.size() >= kMaxProbes ||
          uint32_t(g.probe_x) >= rt.width || uint32_t(g.probe_y) >= rt.height)
      {
        return;
      }
      if (!g.probe_buffer)
      {
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = uint64_t(kProbeStride) * kMaxProbes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&g.probe_buffer))))
        {
          g.probe_x = -1;
          return;
        }
      }
      uint32_t offset = uint32_t(g.probes.size()) * kProbeStride;
      D3D12_TEXTURE_COPY_LOCATION dst = {g.probe_buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint.Offset = offset;
      dst.PlacedFootprint.Footprint = {rt.format, 1, 1, 1, kProbeStride};
      D3D12_TEXTURE_COPY_LOCATION src = {rt.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      UINT x = CenterPixel(uint32_t(g.probe_x), rt.scale4), y = CenterPixel(uint32_t(g.probe_y), rt.scale4);
      D3D12_BOX box = {x, y, 0, x + 1, y + 1, 1};
      Transition(rt.resource.Get(), rt.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
      g.probes.push_back({draw, vs, ps, rt.edram_base, rt.edram_format, rt.format, offset});
    }

    void WriteProbes()
    {
      if (g.probes.empty() || !g.probe_buffer)
      {
        g.probes.clear();
        return;
      }
      void *data = nullptr;
      D3D12_RANGE range = {0, SIZE_T(g.probes.size()) * kProbeStride};
      if (SUCCEEDED(g.probe_buffer->Map(0, &range, &data)))
      {
        std::error_code ec;
        std::filesystem::create_directories("capture", ec);
        if (FILE *f = std::fopen("capture/probe.txt", "w"))
        {
          std::fprintf(f, "pixel %d %d of render target 0 after each draw (raw bytes of the host format)\n",
                       g.probe_x, g.probe_y);
          for (const auto &e : g.probes)
          {
            const uint8_t *b = static_cast<const uint8_t *>(data) + e.offset;
            std::fprintf(f, "draw %llu vs %016llX ps %016llX rt %u fmt %u dxgi %u:", (unsigned long long)e.draw,
                         (unsigned long long)e.vs, (unsigned long long)e.ps, e.edram_base, e.edram_format,
                         unsigned(e.format));
            for (uint32_t i = 0; i < 16; ++i)
            {
              std::fprintf(f, " %02X", b[i]);
            }
            std::fprintf(f, "\n");
          }
          std::fclose(f);
        }
        g.probe_buffer->Unmap(0, nullptr);
      }
      g.probes.clear();
    }

    // Writes the read backs of the captured frame (after the GPU finished it).
    void WriteCaptures()
    {
      if (g.captures.empty() && g.probes.empty())
      {
        return;
      }
      WaitIdle();
      WriteProbes();
      std::error_code ec;
      std::filesystem::create_directories("capture", ec);
      FILE *index = std::fopen("capture/index.txt", "w");
      for (auto &c : g.captures)
      {
        void *data = nullptr;
        D3D12_RANGE range = {0, SIZE_T(c.footprint.Footprint.RowPitch) * c.footprint.Footprint.Height};
        HRESULT hr = c.buffer->Map(0, &range, &data);
        if (FAILED(hr))
        {
          NativeLog("capture %s not written: Map failed (0x%08X)\n", c.name.c_str(), unsigned(hr));
          continue;
        }
        std::string path = "capture/" + c.name + ".raw";
        if (FILE *f = std::fopen(path.c_str(), "wb"))
        {
          std::fwrite(data, 1, range.End, f);
          std::fclose(f);
        }
        c.buffer->Unmap(0, nullptr);
        if (index)
        {
          std::fprintf(index, "%s %u %u %u %u %u\n", c.name.c_str(), c.footprint.Footprint.Width,
                       c.footprint.Footprint.Height, c.footprint.Footprint.RowPitch, unsigned(c.format),
                       unsigned(c.red_blue_swapped));
        }
      }
      if (index)
      {
        std::fclose(index);
      }
      NativeLog("%zu textures captured to capture/\n", g.captures.size());
      g.captures.clear();
    }

    void TraceDraw(const RegisterFile &r, const DrawPacket &d, const CompiledShader *vs, const CompiledShader *ps)
    {
      if (!g_trace)
      {
        return;
      }
      auto surface = r.Get<reg::RB_SURFACE_INFO>();
      auto mode = r.Get<reg::RB_MODECONTROL>();
      auto dc = r.Get<reg::RB_DEPTHCONTROL>();
      auto di = r.Get<reg::RB_DEPTH_INFO>();
      Trace("draw %llu: prim %u count %u%s vs %016llX ps %016llX | pitch %u msaa %u mode %u mask %04X\n",
            (unsigned long long)g.draws, unsigned(d.prim_type), d.index_count, d.indexed ? " indexed" : "",
            vs ? (unsigned long long)vs->shader->ucode_data_hash() : 0ull,
            ps ? (unsigned long long)ps->shader->ucode_data_hash() : 0ull, surface.surface_pitch,
            unsigned(surface.msaa_samples), unsigned(mode.edram_mode), r.v[XE_GPU_REG_RB_COLOR_MASK] & 0xFFFF);
      Trace("    vertex index offset %u min %u max %u | mode cntl %08X | poly offset front %g/%g back %g/%g\n",
            r.v[XE_GPU_REG_VGT_INDX_OFFSET], r.v[XE_GPU_REG_VGT_MIN_VTX_INDX], r.v[XE_GPU_REG_VGT_MAX_VTX_INDX],
            r.v[XE_GPU_REG_PA_SU_SC_MODE_CNTL], r.F(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE),
            r.F(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET), r.F(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE),
            r.F(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET));
      Trace("    vtx cntl %08X vte cntl %08X clip cntl %08X | vport scale %.9g %.9g offset %.9g %.9g\n",
            r.v[XE_GPU_REG_PA_SU_VTX_CNTL], r.v[XE_GPU_REG_PA_CL_VTE_CNTL], r.v[XE_GPU_REG_PA_CL_CLIP_CNTL],
            r.F(XE_GPU_REG_PA_CL_VPORT_XSCALE), r.F(XE_GPU_REG_PA_CL_VPORT_YSCALE),
            r.F(XE_GPU_REG_PA_CL_VPORT_XOFFSET), r.F(XE_GPU_REG_PA_CL_VPORT_YOFFSET));
      for (uint32_t i = 0; i < 4; ++i)
      {
        if (!((r.v[XE_GPU_REG_RB_COLOR_MASK] >> (i * 4)) & 0xF) || !ps ||
            !(ps->shader->writes_color_targets() & (1u << i)))
        {
          continue;
        }
        auto ci = r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
        Trace("    rt%u base %u fmt %u exp_bias %d blend %08X\n", i, ci.color_base, unsigned(ci.color_format),
              int(ci.color_exp_bias), r.v[reg::RB_BLENDCONTROL::rt_register_indices[i]]);
      }
      Trace("    depth base %u fmt %u z %u write %u func %u stencil %u | alpha test %u ref %g | depthcontrol %08X "
            "stencilrefmask %08X\n",
            di.depth_base, unsigned(di.depth_format), dc.z_enable, dc.z_write_enable, unsigned(dc.zfunc),
            dc.stencil_enable, r.Get<reg::RB_COLORCONTROL>().alpha_test_enable, r.F(XE_GPU_REG_RB_ALPHA_REF),
            dc.value, r.v[XE_GPU_REG_RB_STENCILREFMASK]);
      for (const CompiledShader *s : {vs, ps})
      {
        if (!s)
        {
          continue;
        }
        uint32_t traced_fetches = 0;
        for (const auto &b : s->shader->texture_bindings())
        {
          if (traced_fetches & (1u << b.fetch_constant))
          {
            continue;
          }
          traced_fetches |= 1u << b.fetch_constant;
          auto f = r.Get<xenos::xe_gpu_texture_fetch_t>(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + b.fetch_constant * 6);
          bool resolved = g.resolved.count((f.base_address & 0x1FFFF) << 12) != 0;
          Trace("    %s tex%u fetch %u: base %08X fmt %u %ux%u dim %u tiled %u swz %03X signs %u%u%u%u exp %d "
                "filter %u/%u/%u clamp %u/%u%s\n",
                s == vs ? "vs" : "ps", unsigned(b.binding_index), b.fetch_constant, (f.base_address & 0x1FFFF) << 12,
                unsigned(f.format), f.size_2d.width + 1, f.size_2d.height + 1, unsigned(f.dimension), f.tiled,
                f.swizzle, unsigned(f.sign_x), unsigned(f.sign_y), unsigned(f.sign_z), unsigned(f.sign_w),
                int(f.exp_adjust), unsigned(f.mag_filter), unsigned(f.min_filter), unsigned(f.mip_filter),
                unsigned(f.clamp_x), unsigned(f.clamp_y), resolved ? " RESOLVED" : "");
        }
        // Float constants the shader reads (up to 48, without dynamic indexing).
        const auto &map = s->shader->constant_register_map();
        uint32_t base = (r.v[s == vs ? XE_GPU_REG_SQ_VS_CONST : XE_GPU_REG_SQ_PS_CONST] & 0x1FF);
        uint32_t traced = 0;
        for (uint32_t c = 0; c < 256 && traced < 48; ++c)
        {
          if (!(map.float_bitmap[c >> 6] & (uint64_t(1) << (c & 63))) || base + c >= 512)
          {
            continue;
          }
          const uint32_t *v = &r.v[0x4000 + (base + c) * 4];
          float x[4];
          std::memcpy(x, v, sizeof(x));
          Trace("    %s c%u = %g %g %g %g\n", s == vs ? "vs" : "ps", c, x[0], x[1], x[2], x[3]);
          ++traced;
        }
      }
    }

    // ------------------------------------------------------------ Guest write tracking

    constexpr uint32_t kPageShift = 12;
    constexpr uint32_t kPageCount = kPhysicalSize >> kPageShift;

    // Called by the runtime (with its global lock held, on the writing thread)
    // when the CPU writes to a watched page. Only atomics are touched here.
    std::pair<uint32_t, uint32_t> OnGuestWrite(void *, uint32_t start, uint32_t length, bool exact_range)
    {
      if (!length || start >= kPhysicalSize)
      {
        return {0, UINT32_MAX};
      }
      length = std::min(length, kPhysicalSize - start);
      uint32_t first = start >> kPageShift;
      uint32_t last = (start + length - 1) >> kPageShift;
      if (!exact_range)
      {
        // Unwatch (and treat as written) the rest of the aligned 256 KB block
        // too: far fewer access violations for big writes, like in the SDK's
        // SharedMemory. But not across a resolve destination's page the write
        // did not hit: a write elsewhere (e.g. a CPU buffer next to the scene
        // depth) must not discard what the GPU resolved there, and the page
        // has to stay watched - unwatched and not marked written, a later
        // write to it (memory reused for a CPU texture, a movie's frame)
        // would never be seen.
        auto resolve_page = [](uint32_t page) {
          return ((g.resolve_pages[page >> 6].load(std::memory_order_relaxed) >> (page & 63)) & 1) != 0;
        };
        while ((first & 63) && !resolve_page(first - 1))
        {
          --first;
        }
        while ((last & 63) != 63 && last + 1 < kPageCount && !resolve_page(last + 1))
        {
          ++last;
        }
      }
      else
      {
        // Explicit invalidation, e.g. on release: forget the commit status too.
        ForgetGuestPhysicalCommitted(start, length);
      }
      uint32_t epoch = g.write_epoch.fetch_add(1, std::memory_order_relaxed) + 1;
      for (uint32_t page = first; page <= last; ++page)
      {
        g.page_epoch[page].store(epoch, std::memory_order_relaxed);
      }
      if (!exact_range && ((first & 63) || (last & 63) != 63))
      {
        g.resolve_pages_spared.fetch_add(1, std::memory_order_relaxed);
      }
      for (uint32_t block = first >> 6; block <= last >> 6; ++block)
      {
        uint64_t bits = UINT64_MAX;
        if (block == first >> 6)
        {
          bits &= ~((uint64_t(1) << (first & 63)) - 1);
        }
        if (block == last >> 6 && (last & 63) != 63)
        {
          bits &= (uint64_t(1) << ((last & 63) + 1)) - 1;
        }
        g.mirror_valid[block].fetch_and(~bits, std::memory_order_relaxed);
      }
      return {first << kPageShift, (last - first + 1) << kPageShift};
    }

    void InitWriteTracking()
    {
      g.page_epoch = std::make_unique<std::atomic<uint32_t>[]>(kPageCount);
      g.mirror_valid = std::make_unique<std::atomic<uint64_t>[]>(kPageCount / 64);
      g.resolve_pages = std::make_unique<std::atomic<uint64_t>[]>(kPageCount / 64);
      for (uint32_t i = 0; i < kPageCount; ++i)
      {
        g.page_epoch[i].store(0, std::memory_order_relaxed);
      }
      for (uint32_t i = 0; i < kPageCount / 64; ++i)
      {
        g.mirror_valid[i].store(0, std::memory_order_relaxed);
        g.resolve_pages[i].store(0, std::memory_order_relaxed);
      }
      g.invalidation_handle = Mem()->RegisterPhysicalMemoryInvalidationCallback(OnGuestWrite, nullptr);
    }

    void MarkResolvePages(uint32_t address, uint32_t size)
    {
      if (!size || address >= kPhysicalSize)
      {
        return;
      }
      uint32_t last = (address + std::min(size, kPhysicalSize - address) - 1) >> kPageShift;
      for (uint32_t page = address >> kPageShift; page <= last; ++page)
      {
        g.resolve_pages[page >> 6].fetch_or(uint64_t(1) << (page & 63), std::memory_order_relaxed);
      }
    }

    // Requests a callback on the next CPU write to the range.
    void WatchGuestRange(uint32_t address, uint32_t size)
    {
      if (size && address < kPhysicalSize)
      {
        Mem()->EnablePhysicalMemoryAccessCallbacks(address, std::min(size, kPhysicalSize - address), true, false);
      }
    }

    // True if the CPU wrote to the range after `epoch`.
    bool WrittenSince(uint32_t address, uint32_t size, uint32_t epoch)
    {
      if (!size || address >= kPhysicalSize)
      {
        return false;
      }
      uint32_t last = (address + std::min(size, kPhysicalSize - address) - 1) >> kPageShift;
      for (uint32_t page = address >> kPageShift; page <= last; ++page)
      {
        if (g.page_epoch[page].load(std::memory_order_relaxed) > epoch)
        {
          return true;
        }
      }
      return false;
    }

    bool MirrorPageValid(uint32_t page)
    {
      return (g.mirror_valid[page >> 6].load(std::memory_order_relaxed) >> (page & 63)) & 1;
    }

    void SetMirrorPagesValid(uint32_t first, uint32_t end)
    {
      for (uint32_t page = first; page < end; ++page)
      {
        g.mirror_valid[page >> 6].fetch_or(uint64_t(1) << (page & 63), std::memory_order_relaxed);
      }
    }


    // ------------------------------------------------------------ Helpers

    void Transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES &state, D3D12_RESOURCE_STATES to)
    {
      if (state == to)
      {
        return;
      }
      Crumb("barrier", uint64_t(resource), state, to);
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = resource;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = state;
      b.Transition.StateAfter = to;
      g.list->ResourceBarrier(1, &b);
      state = to;
    }

    Frame &CurrentFrame()
    {
      return g.frames[g.frame_index];
    }

    // Keeps `resource` alive until the frame being recorded has completed.
    void DeferRelease(ComPtr<ID3D12Resource> resource)
    {
      if (resource)
      {
        g.graveyard.emplace_back(g.fence_value + 1, std::move(resource));
      }
    }

    void CollectGarbage()
    {
      uint64_t completed = g.fence->GetCompletedValue();
      std::erase_if(g.graveyard, [completed](const auto &e) { return e.first <= completed; });
    }

    // Waits for the fence to reach `value`, reporting long waits (GPU hangs).
    void WaitForFence(uint64_t value)
    {
      if (g.fence->GetCompletedValue() >= value)
      {
        return;
      }
      auto wait_start = std::chrono::steady_clock::now();
      struct AddTime
      {
        std::chrono::steady_clock::time_point start;
        ~AddTime()
        {
          g.time_fence_ns += Elapsed(start);
          if (ao2::spikes::Enabled())
          {
            ao2::spikes::Mark(ao2::spikes::kGpuFence, double(Elapsed(start)) / 1e6);
          }
        }
      } add_time{wait_start};
      g.fence->SetEventOnCompletion(value, g.fence_event);
      for (uint32_t seconds = 0; WaitForSingleObject(g.fence_event, 2000) == WAIT_TIMEOUT;)
      {
        seconds += 2;
        NativeLog("GPU wait for fence %llu: %us, completed %llu, device %08X\n",
                  (unsigned long long)value, seconds, (unsigned long long)g.fence->GetCompletedValue(),
                  unsigned(g.device->GetDeviceRemovedReason()));
        if (g.fence->GetCompletedValue() >= value)
        {
          break;
        }
      }
    }

    void WaitIdle()
    {
      ++g.fence_value;
      g.queue->Signal(g.fence.Get(), g.fence_value);
      WaitForFence(g.fence_value);
    }

    // Returns the GPU address of `size` bytes of upload memory filled from `data`.
    D3D12_GPU_VIRTUAL_ADDRESS Upload(const void *data, uint64_t size, uint64_t align = 256,
                                     void **mapped_out = nullptr)
    {
      Frame &f = CurrentFrame();
      uint64_t offset = (f.upload_offset + align - 1) & ~(align - 1);
      if (offset + size > kUploadSize)
      {
        return 0;
      }
      f.upload_offset = offset + size;
      if (data)
      {
        std::memcpy(f.upload_mapped + offset, data, size);
      }
      if (mapped_out)
      {
        *mapped_out = f.upload_mapped + offset;
      }
      return f.upload->GetGPUVirtualAddress() + offset;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CpuView(uint32_t index)
    {
      D3D12_CPU_DESCRIPTOR_HANDLE h = g.cpu_view_heap->GetCPUDescriptorHandleForHeapStart();
      h.ptr += uint64_t(index) * g.view_size;
      return h;
    }

    std::vector<uint8_t> Compile(const char *source, const char *entry, const char *target)
    {
      ComPtr<ID3DBlob> code, errors;
      HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, target,
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
      if (FAILED(hr))
      {
        NativeLog("internal shader %s failed: %s\n", entry,
                  errors ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
        return {};
      }
      auto *p = static_cast<const uint8_t *>(code->GetBufferPointer());
      return std::vector<uint8_t>(p, p + code->GetBufferSize());
    }

    // ------------------------------------------------------------ Init

    bool InitSharedMemory()
    {
      ComPtr<ID3D12Device3> device3;
      if (FAILED(g.device.As(&device3)))
      {
        NativeLog("shared memory: ID3D12Device3 unavailable\n");
        return false;
      }
      std::wstring name = L"Local\\" + Mem()->file_name().wstring();
      HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
      if (!mapping)
      {
        NativeLog("shared memory: OpenFileMapping failed (%lu)\n", GetLastError());
        return false;
      }
      HRESULT hr = device3->OpenExistingHeapFromFileMapping(mapping, IID_PPV_ARGS(&g.shared_heap));
      CloseHandle(mapping);
      if (FAILED(hr))
      {
        NativeLog("shared memory: OpenExistingHeapFromFileMapping failed (0x%08X)\n", unsigned(hr));
        return false;
      }
      D3D12_HEAP_DESC hd = g.shared_heap->GetDesc();
      NativeLog("shared memory heap: size 0x%llX flags 0x%X\n", (unsigned long long)hd.SizeInBytes,
                unsigned(hd.Flags));
      D3D12_RESOURCE_DESC rd = {};
      rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      rd.Width = kPhysicalSize;
      rd.Height = 1;
      rd.DepthOrArraySize = 1;
      rd.MipLevels = 1;
      rd.SampleDesc.Count = 1;
      rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      for (D3D12_RESOURCE_FLAGS flags : {D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_RESOURCE_FLAG_NONE})
      {
        rd.Flags = flags;
        hr = g.device->CreatePlacedResource(g.shared_heap.Get(), kPhysicalFileOffset, &rd,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&g.shared_memory));
        if (SUCCEEDED(hr))
        {
          NativeLog("shared memory buffer created (flags 0x%X)\n", unsigned(flags));
          return true;
        }
      }
      NativeLog("shared memory: CreatePlacedResource failed (0x%08X)\n", unsigned(hr));
      return false;
    }

    bool InitMirror()
    {
      D3D12_RESOURCE_DESC rd = {};
      rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      rd.Width = kPhysicalSize + (1u << 20); // slack for offsets past the end
      rd.Height = 1;
      rd.DepthOrArraySize = 1;
      rd.MipLevels = 1;
      rd.SampleDesc.Count = 1;
      rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
      g.mirror_state = D3D12_RESOURCE_STATE_COMMON;
      if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, g.mirror_state,
                                                   nullptr, IID_PPV_ARGS(&g.shared_memory))))
      {
        NativeLog("guest memory mirror creation failed\n");
        return false;
      }
      g.shared_memory_is_mirror = true;
      NativeLog("using a 512 MB GPU mirror of guest physical memory\n");
      return true;
    }

    // Copies the vertex buffer ranges fetched by `vs` into the mirror.
    // Makes the vertex buffers of `vs` (and the guest index buffer range
    // [index_address, +index_size) when indices are fetched by the shader)
    // current in the GPU mirror of guest memory.
    void UploadVertexRanges(const CompiledShader *vs, const RegisterFile &r, uint32_t index_address = 0,
                            uint32_t index_size = 0)
    {
      if (!g.shared_memory_is_mirror)
      {
        return;
      }
      // Pages already current in the mirror (and still watched) are skipped;
      // the rest is uploaded in runs of contiguous pages.
      bool copied = false;
      auto copy = [&](uint32_t address, uint32_t size) {
        void *dst = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS src = Upload(nullptr, size, 16, &dst);
        if (!src)
        {
          return false;
        }
        std::memcpy(dst, Mem()->TranslatePhysical(address), size);
        if (!copied)
        {
          Transition(g.shared_memory.Get(), g.mirror_state, D3D12_RESOURCE_STATE_COPY_DEST);
          copied = true;
        }
        g.list->CopyBufferRegion(g.shared_memory.Get(), address, CurrentFrame().upload.Get(),
                                 src - CurrentFrame().upload->GetGPUVirtualAddress(), size);
        g.mirror_bytes += size;
        return true;
      };
      const auto &bindings = vs->shader->vertex_bindings();
      for (size_t range = 0; range <= bindings.size(); ++range)
      {
        uint32_t address = index_address, size = index_size;
        if (range < bindings.size())
        {
          xenos::xe_gpu_vertex_fetch_t fetch;
          std::memcpy(&fetch, &r.v[0x4800 + bindings[range].fetch_constant * 2], sizeof(fetch));
          address = (fetch.address << 2) & 0x1FFFFFFF;
          size = fetch.size << 2;
        }
        if (!size || address + uint64_t(size) > kPhysicalSize)
        {
          continue;
        }
        uint32_t first = address >> kPageShift, end = ((address + size - 1) >> kPageShift) + 1;
        if (g_trace)
        {
          uint32_t valid = 0;
          for (uint32_t page = first; page < end; ++page)
          {
            valid += MirrorPageValid(page) ? 1 : 0;
          }
          char values[1024] = {};
          int n = 0;
          if (IsGuestPhysicalReadable(address, std::min(size, 96u)))
          {
            const uint8_t *p = Mem()->TranslatePhysical(address);
            for (uint32_t i = 0; i < std::min(size, 96u) / 4; ++i)
            {
              uint32_t be;
              std::memcpy(&be, p + i * 4, 4);
              uint32_t le = __builtin_bswap32(be);
              float fv;
              std::memcpy(&fv, &le, 4);
              n += std::snprintf(values + n, sizeof(values) - n, " %.8g", fv);
            }
          }
          Trace("    %s %08X size %u: %u/%u pages current in the mirror |%s\n",
                range < bindings.size() ? "vertices" : "indices", address, size, valid, end - first, values);
        }
        const bool always_upload = REXCVAR_GET(ao2_native_gpu_debug_upload_vertices);
        for (uint32_t page = first; page < end;)
        {
          if (MirrorPageValid(page) && !always_upload)
          {
            ++page;
            continue;
          }
          uint32_t run_end = page + 1;
          while (run_end < end && (always_upload || !MirrorPageValid(run_end)))
          {
            ++run_end;
          }
          uint32_t run_address = page << kPageShift, run_size = (run_end - page) << kPageShift;
          if (IsGuestPhysicalReadable(run_address, run_size))
          {
            // Watch first: a write during the copy invalidates the pages again.
            SetMirrorPagesValid(page, run_end);
            WatchGuestRange(run_address, run_size);
            if (!copy(run_address, run_size))
            {
              // Out of upload memory: retry on the next draw.
              for (uint32_t k = page; k < run_end; ++k)
              {
                g.mirror_valid[k >> 6].fetch_and(~(uint64_t(1) << (k & 63)), std::memory_order_relaxed);
              }
            }
          }
          else if (IsGuestPhysicalReadable(std::max(address, run_address), 1))
          {
            // Partially committed run: copy just the fetched bytes, untracked.
            uint32_t lo = std::max(address, run_address);
            uint32_t hi = std::min(address + size, run_address + run_size);
            if (IsGuestPhysicalReadable(lo, hi - lo))
            {
              copy(lo, hi - lo);
            }
          }
          page = run_end;
        }
      }
      Transition(g.shared_memory.Get(), g.mirror_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    bool InitRootSignature()
    {
      D3D12_DESCRIPTOR_RANGE ranges[4] = {};
      for (uint32_t i = 0; i < 4; ++i)
      {
        ranges[i].RangeType =
            (i & 1) ? D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = kMaxTextureBindings;
        ranges[i].BaseShaderRegister = 0;
      }
      D3D12_ROOT_PARAMETER p[kRootCount] = {};
      p[kRootVSConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      p[kRootVSConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
      p[kRootPSConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      p[kRootPSConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      p[kRootSystemConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      p[kRootSystemConstants].Descriptor.ShaderRegister = 1;
      p[kRootSystemConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      p[kRootSharedMemory].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
      p[kRootSharedMemory].Descriptor.RegisterSpace = 1;
      p[kRootSharedMemory].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
      const D3D12_SHADER_VISIBILITY vis[4] = {D3D12_SHADER_VISIBILITY_PIXEL, D3D12_SHADER_VISIBILITY_PIXEL,
                                              D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SHADER_VISIBILITY_VERTEX};
      for (uint32_t i = 0; i < 4; ++i)
      {
        auto &t = p[kRootPSTextures + i];
        t.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        t.DescriptorTable.NumDescriptorRanges = 1;
        t.DescriptorTable.pDescriptorRanges = &ranges[i];
        t.ShaderVisibility = vis[i];
      }
      D3D12_ROOT_SIGNATURE_DESC desc = {};
      desc.NumParameters = kRootCount;
      desc.pParameters = p;
      ComPtr<ID3DBlob> blob, error;
      if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
          FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&g.root_signature))))
      {
        NativeLog("root signature failed: %s\n",
                  error ? static_cast<const char *>(error->GetBufferPointer()) : "");
        return false;
      }

      // Blit: one table SRV, three root constants (uv scale, red/blue swap),
      // the gamma ramp, one static sampler.
      D3D12_DESCRIPTOR_RANGE br = {};
      br.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      br.NumDescriptors = 1;
      D3D12_ROOT_PARAMETER bp[3] = {};
      bp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      bp[0].DescriptorTable.NumDescriptorRanges = 1;
      bp[0].DescriptorTable.pDescriptorRanges = &br;
      bp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      bp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      bp[1].Constants.Num32BitValues = 3;
      bp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      // Gamma ramp (t1).
      bp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
      bp[2].Descriptor.ShaderRegister = 1;
      bp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      D3D12_STATIC_SAMPLER_DESC ss = {};
      ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
      ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      ss.MaxLOD = D3D12_FLOAT32_MAX;
      D3D12_ROOT_SIGNATURE_DESC bd = {};
      bd.NumParameters = 3;
      bd.pParameters = bp;
      bd.NumStaticSamplers = 1;
      bd.pStaticSamplers = &ss;
      blob.Reset();
      if (FAILED(D3D12SerializeRootSignature(&bd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
          FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&g.blit_root))))
      {
        return false;
      }
      auto vs = Compile(kBlitShader, "vs", "vs_5_1");
      auto ps = Compile(kBlitShader, "ps", "ps_5_1");
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
      pd.pRootSignature = g.blit_root.Get();
      pd.VS = {vs.data(), vs.size()};
      pd.PS = {ps.data(), ps.size()};
      pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pd.SampleMask = UINT_MAX;
      pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pd.NumRenderTargets = 1;
      pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      pd.SampleDesc.Count = 1;
      if (FAILED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g.blit_pso))))
      {
        NativeLog("blit pipeline failed\n");
        return false;
      }
      g.rect_gs = Compile(kRectGS, "main", "gs_5_1");
      return !g.rect_gs.empty();
    }

    bool Init()
    {
      g.initialized = true;
      // In steps of a quarter (see ScaleEdge); not a number -> 1.
      double scale = REXCVAR_GET(ao2_native_gpu_resolution_scale);
      g.scale4 = uint32_t(std::lround(std::clamp(scale >= 1.0 ? scale : 1.0, 1.0, 4.0) * 4.0));
      g.small_scale4 = (g.scale4 + 3) & ~3u;
      g.small_width = uint32_t(std::max(REXCVAR_GET(ao2_native_gpu_small_target_width), 0));
      NativeLog("resolution scale %.2f (render targets up to %u wide: %u)\n", double(g.scale4) * 0.25, g.small_width,
                g.small_scale4 / 4);
      if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&g.factory))))
      {
        return false;
      }
      if (REXCVAR_GET(ao2_native_gpu_debug_layer))
      {
        // Only valid before the device exists (the runtime creates none of its own).
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
          debug->EnableDebugLayer();
          NativeLog("D3D12 debug layer enabled\n");
        }
        else
        {
          NativeLog("D3D12 debug layer unavailable (Graphics Tools not installed?)\n");
        }
      }
      ComPtr<IDXGIAdapter1> adapter;
      g.factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
      BOOL tearing = FALSE;
      g.tearing = SUCCEEDED(g.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                                                           sizeof(tearing))) &&
                  tearing;
      if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device))))
      {
        NativeLog("D3D12CreateDevice failed\n");
        return false;
      }
      DXGI_ADAPTER_DESC1 ad = {};
      adapter->GetDesc1(&ad);
      char name[128] = {};
      WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
      NativeLog("native renderer adapter: %s\n", name);
      LUID luid = g.device->GetAdapterLuid();
      ao2::perf::SetAdapter(luid.LowPart, luid.HighPart);

      if (REXCVAR_GET(ao2_native_gpu_debug_layer))
      {
        g.device.As(&g.info_queue);
      }

      D3D12_COMMAND_QUEUE_DESC qd = {};
      qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
      g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));
      g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
      g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

      D3D12_HEAP_PROPERTIES upload_heap = {D3D12_HEAP_TYPE_UPLOAD};
      D3D12_RESOURCE_DESC ud = {};
      ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      ud.Width = kUploadSize;
      ud.Height = 1;
      ud.DepthOrArraySize = 1;
      ud.MipLevels = 1;
      ud.SampleDesc.Count = 1;
      ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      for (auto &f : g.frames)
      {
        g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator));
        if (FAILED(g.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &ud,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&f.upload))))
        {
          return false;
        }
        D3D12_RANGE none = {0, 0};
        f.upload->Map(0, &none, reinterpret_cast<void **>(&f.upload_mapped));
        D3D12_HEAP_PROPERTIES readback_heap = {D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rbd = ud;
        rbd.Width = kReadbackSize;
        if (FAILED(g.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &rbd,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&f.readback))))
        {
          return false;
        }
        D3D12_RANGE all = {0, SIZE_T(kReadbackSize)};
        f.readback->Map(0, &all, reinterpret_cast<void **>(&f.readback_mapped));
      }
      g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.frames[0].allocator.Get(),
                                  nullptr, IID_PPV_ARGS(&g.list));
      {
        // GPU time per category (stats line).
        D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTimestampsPerFrame * kFrameCount, 0};
        D3D12_RESOURCE_DESC tbd = ud;
        tbd.Width = uint64_t(kTimestampsPerFrame) * kFrameCount * 8;
        D3D12_HEAP_PROPERTIES readback_heap = {D3D12_HEAP_TYPE_READBACK};
        void *mapped = nullptr;
        D3D12_RANGE all = {0, SIZE_T(tbd.Width)};
        if (SUCCEEDED(g.queue->GetTimestampFrequency(&g.timestamp_frequency)) &&
            SUCCEEDED(g.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g.timestamp_heap))) &&
            SUCCEEDED(g.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &tbd,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&g.timestamp_readback))) &&
            SUCCEEDED(g.timestamp_readback->Map(0, &all, &mapped)))
        {
          g.timestamps = static_cast<const uint64_t *>(mapped);
        }
        else
        {
          g.timestamp_heap.Reset();
        }
      }
      {
        // Guest occlusion queries.
        D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_OCCLUSION, kOcclusionPerFrame * kFrameCount, 0};
        D3D12_RESOURCE_DESC obd = ud;
        obd.Width = uint64_t(kOcclusionPerFrame) * kFrameCount * 8;
        D3D12_HEAP_PROPERTIES readback_heap = {D3D12_HEAP_TYPE_READBACK};
        void *mapped = nullptr;
        D3D12_RANGE all = {0, SIZE_T(obd.Width)};
        if (SUCCEEDED(g.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g.occlusion_heap))) &&
            SUCCEEDED(g.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &obd,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&g.occlusion_readback))) &&
            SUCCEEDED(g.occlusion_readback->Map(0, &all, &mapped)))
        {
          g.occlusion_results = static_cast<const uint64_t *>(mapped);
        }
        else
        {
          g.occlusion_heap.Reset();
          NativeLog("occlusion query heap unavailable: queries report fixed counts\n");
        }
      }
      g.list->Close();
      if (FAILED(g.list.As(&g.list1)))
      {
        NativeLog("ID3D12GraphicsCommandList1 unavailable\n");
        return false;
      }

      auto heap = [](D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool visible,
                     ComPtr<ID3D12DescriptorHeap> &out) {
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.Type = type;
        d.NumDescriptors = count;
        d.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        return SUCCEEDED(g.device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&out)));
      };
      if (!heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewsPerFrame * kFrameCount + kMaxTextureBindings, true, g.view_heap) ||
          !heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize, true, g.sampler_heap) ||
          !heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kCpuSamplerCount, false, g.cpu_sampler_heap) ||
          !heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16384, false, g.cpu_view_heap) ||
          !heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024, false, g.rtv_heap) ||
          !heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 256, false, g.dsv_heap))
      {
        return false;
      }
      g.view_size = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      g.sampler_size = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
      g.rtv_size = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
      g.dsv_size = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

      // Null SRVs for unbound textures of each dimension.
      D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
      nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      nd.Texture2D.MipLevels = 1;
      g.null_srv_2d = CpuView(g.cpu_views_used++);
      g.device->CreateShaderResourceView(nullptr, &nd, g.null_srv_2d);
      nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      nd.Texture3D.MipLevels = 1;
      g.null_srv_3d = CpuView(g.cpu_views_used++);
      g.device->CreateShaderResourceView(nullptr, &nd, g.null_srv_3d);
      nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
      nd.TextureCube.MipLevels = 1;
      g.null_srv_cube = CpuView(g.cpu_views_used++);
      g.device->CreateShaderResourceView(nullptr, &nd, g.null_srv_cube);

      if (!InitRootSignature())
      {
        return false;
      }
      if (!InitSharedMemory() && !InitMirror())
      {
        return false;
      }
      InitWriteTracking();

      if (!heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBackBufferCount, false, g.backbuffer_rtv_heap) ||
          !heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kTransientRtvCount, false, g.transient_rtv_heap))
      {
        return false;
      }
      if (!InitResolvePipelines() || !InitTransferPipelines())
      {
        NativeLog("resolve/transfer pipelines failed\n");
        return false;
      }
      NativeLog("native renderer initialized\n");
      return true;
    }

    // ------------------------------------------------------------ Swap chain

    void CreateBackBufferViews()
    {
      for (uint32_t i = 0; i < kBackBufferCount; ++i)
      {
        g.swapchain->GetBuffer(i, IID_PPV_ARGS(&g.backbuffers[i]));
        g.backbuffer_rtv[i] = g.backbuffer_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        g.backbuffer_rtv[i].ptr += uint64_t(i) * g.rtv_size;
        g.device->CreateRenderTargetView(g.backbuffers[i].Get(), nullptr, g.backbuffer_rtv[i]);
      }
    }

    // Creates or resizes the swap chain to the game window's client area.
    // Returns false while there is nothing to present to (no window, minimized).
    bool EnsureSwapChain()
    {
      HWND hwnd = static_cast<HWND>(PresentWindow());
      if (!hwnd)
      {
        return false;
      }
      RECT rc = {};
      GetClientRect(hwnd, &rc);
      uint32_t w = uint32_t(std::max<LONG>(rc.right - rc.left, 0));
      uint32_t h = uint32_t(std::max<LONG>(rc.bottom - rc.top, 0));
      if (!w || !h)
      {
        return false;
      }
      if (g.swapchain && hwnd == g.hwnd && w == g.swapchain_width && h == g.swapchain_height)
      {
        return true;
      }
      UINT flags = g.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
      WaitIdle();
      for (auto &b : g.backbuffers)
      {
        b.Reset();
      }
      if (g.swapchain && hwnd == g.hwnd)
      {
        HRESULT hr = g.swapchain->ResizeBuffers(kBackBufferCount, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, flags);
        if (FAILED(hr))
        {
          NativeLog("swap chain resize to %ux%u failed (0x%08X)\n", w, h, unsigned(hr));
          g.swapchain.Reset();
          return false;
        }
      }
      else
      {
        g.swapchain.Reset();
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = w;
        sd.Height = h;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBackBufferCount;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = flags;
        ComPtr<IDXGISwapChain1> sc1;
        HRESULT hr = g.factory->CreateSwapChainForHwnd(g.queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
        if (FAILED(hr) || FAILED(sc1.As(&g.swapchain)))
        {
          NativeLog("swap chain creation failed (0x%08X)\n", unsigned(hr));
          return false;
        }
        // Fullscreen is handled by the window (borderless), never by DXGI.
        g.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        g.hwnd = hwnd;
      }
      g.swapchain_width = w;
      g.swapchain_height = h;
      CreateBackBufferViews();
      NativeLog("swap chain %ux%u (tearing %d)\n", w, h, int(g.tearing));
      return true;
    }

    // A resolution scale asked for while the game runs (scale4, 0 = none),
    // taken at the end of a frame (ApplyRequestedScale).
    std::atomic<uint32_t> g_requested_scale4{0};
    std::atomic<bool> g_running{false};

    // The new scale from the next frame on. Render targets are keyed by their
    // EDRAM surface, not by scale: all are dropped (with what EDRAM held - the
    // game redraws every frame) and made again at the new scale on first use.
    // Resolve destinations need nothing: one of another scale does not fit
    // (see the resolve) and is replaced; until then it is sampled at its own.
    void ApplyRequestedScale()
    {
      uint32_t want = g_requested_scale4.exchange(0, std::memory_order_acq_rel);
      if (!want || want == g.scale4)
      {
        return;
      }
      for (auto &entry : g.render_targets)
      {
        DeferRelease(std::move(entry.second->resource));
      }
      g.render_targets.clear();
      g.pending_depth = {};
      g.edram_save = {};
      g.lent.clear();
      std::fill(std::begin(g.edram_owner), std::end(g.edram_owner), nullptr);
      ++g.edram_generation;
      g.bound = {};
      g.scale4 = want;
      g.small_scale4 = (want + 3) & ~3u;
      NativeLog("resolution scale %.2f from frame %llu (render targets up to %u wide: %u)\n", double(want) * 0.25,
                (unsigned long long)g.frame_number, g.small_width, g.small_scale4 / 4);
    }

    bool Ready()
    {
      if (!g.initialized)
      {
        g.failed = !Init();
        if (g.failed)
        {
          NativeLog("native renderer initialization failed\n");
        }
      }
      return !g.failed;
    }

    // Writes a finished occlusion query's counts to its guest struct (layout as
    // the fixed-count path in pm4.cpp: Total_A and ZPass_A, little endian).
    // Results arrive frames after the guest saw the GPU pass the query (the
    // command thread runs ahead of the host GPU): a struct that is no longer
    // exactly as the end event left it (`expect`, all 32 bytes) was released
    // and its memory may belong to something else by now, so it is left alone.
    // Checking only for D3D's "not done" marker (0xFFFFFEED in any of words
    // 2-5) was not enough: a new allocation in the freed struct - a short
    // array of pointers that overwrote words 0-1 and left the old markers in
    // 2-5 - got sample counts written over its pointers, and the garbage
    // collector then freed "pointers" like 0x463C and crashed in the pool
    // allocator's Free (sub_8254A088, null + 12) while loading a level.
    void WriteOcclusionResult(uint32_t address, uint32_t samples, const uint32_t *expect)
    {
      uint8_t *mem = Mem()->TranslatePhysical(address);
      if (std::memcmp(mem, expect, 32) != 0)
      {
        ++g.occlusion_stale;
        return;
      }
      const uint32_t counts[8] = {samples, 0, 0, 0, samples, 0, 0, 0};
      // ZPass_A last: D3D polls it (0xFFFFFEED until the GPU is done).
      std::memcpy(mem, counts, 16);
      std::memcpy(mem + 20, counts + 5, 12);
      std::atomic_thread_fence(std::memory_order_release);
      std::memcpy(mem + 16, &samples, 4);
      ++g.occlusion_queries;
      g.occlusion_visible += samples ? 1 : 0;
    }

    volatile uint32_t g_occlusion_generation = 0;

    // Wakes guest threads waiting for occlusion results (occlusion_wait.cpp).
    void NotifyOcclusionResults()
    {
      _InterlockedIncrement(reinterpret_cast<volatile long *>(&g_occlusion_generation));
      WakeByAddressAll(const_cast<uint32_t *>(&g_occlusion_generation));
    }

    // Reports the occlusion queries whose submission the GPU has completed
    // (command thread or occlusion thread).
    void PublishOcclusion()
    {
      std::lock_guard lock(g.occlusion_mutex);
      if (g.occlusion_pending.empty())
      {
        return;
      }
      bool trace = g_trace && std::this_thread::get_id() != g.occlusion_thread.get_id();
      bool published = false;
      uint64_t completed = g.fence->GetCompletedValue();
      uint64_t host_per_guest16 = uint64_t(g.scale4) * g.scale4; // samples of the scaled targets, times 16
      while (!g.occlusion_pending.empty() && g.occlusion_pending.front().fence <= completed)
      {
        const OcclusionQuery &q = g.occlusion_pending.front();
        uint64_t samples = 0;
        if (q.unknown)
        {
          samples = q.fallback;
          ++g.occlusion_unknown;
        }
        else
        {
          const uint64_t *results = g.occlusion_results + uint64_t(q.slot) * kOcclusionPerFrame + q.first;
          for (uint32_t i = 0; i < q.count; ++i)
          {
            samples += results[i];
          }
          samples = (samples * 16 + host_per_guest16 - 1) / host_per_guest16;
        }
        WriteOcclusionResult(q.address, uint32_t(std::min<uint64_t>(samples, 0xFFFFFFFFu)), q.expect);
        if (trace)
        {
          Trace("occlusion result %08X: %llu samples%s\n", q.address, (unsigned long long)samples,
                q.unknown ? " (unknown)" : "");
        }
        g.occlusion_pending.pop_front();
        published = true;
      }
      if (published)
      {
        NotifyOcclusionResults();
      }
    }

    // The rendering thread waits for occlusion results (sub_82AA14D0) right
    // after it issued the queries: they are published as soon as the GPU
    // passes their submission, by a thread that waits for the fence, not when
    // the command thread happens to be idle (it polled every 1 ms) or begins
    // the next frame.
    void OcclusionThreadMain()
    {
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      for (;;)
      {
        uint64_t target;
        {
          std::unique_lock lock(g.occlusion_mutex);
          g.occlusion_submitted.wait(lock, [] { return !g.occlusion_pending.empty(); });
          target = g.occlusion_pending.front().fence;
        }
        if (g.fence->GetCompletedValue() < target && SUCCEEDED(g.fence->SetEventOnCompletion(target, event)))
        {
          WaitForSingleObject(event, 100);
        }
        PublishOcclusion();
      }
    }

    void StartOcclusionThread()
    {
      if (!g.occlusion_thread.joinable())
      {
        g.occlusion_thread = std::thread(OcclusionThreadMain);
        SetThreadDescription(g.occlusion_thread.native_handle(), L"Native GPU occlusion");
        SetThreadPriority(g.occlusion_thread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
      }
    }

    // Starts counting the next draw of the active guest occlusion query;
    // returns the host query index (UINT32_MAX: not counted).
    uint32_t BeginOcclusionSegment()
    {
      Frame &f = CurrentFrame();
      OcclusionQuery &q = g.occlusion;
      bool contiguous = !q.count || (q.epoch == g.occlusion_epoch && q.slot == g.frame_index &&
                                     q.first + q.count == f.oq_count);
      if (!contiguous || f.oq_count >= kOcclusionPerFrame)
      {
        q.unknown = true;
        return UINT32_MAX;
      }
      if (!q.count)
      {
        q.slot = g.frame_index;
        q.first = f.oq_count;
        q.epoch = g.occlusion_epoch;
      }
      ++q.count;
      ++q.recorded;
      uint32_t index = g.frame_index * kOcclusionPerFrame + f.oq_count++;
      g.list->BeginQuery(g.occlusion_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, index);
      return index;
    }

    // Submits what the open command list has recorded without ending the frame
    // (a guest thread may be waiting for the occlusion results in it).
    void SubmitOpenList()
    {
      if (!g.list_open)
      {
        return;
      }
      GpuMark(kGpuIdle);
      if (CloseList())
      {
        ID3D12CommandList *lists[] = {g.list.Get()};
        g.queue->ExecuteCommandLists(1, lists);
      }
      g.queue->Signal(g.fence.Get(), ++g.fence_value);
      // The allocator keeps the submitted commands; the list continues on it.
      HRESULT hr = g.list->Reset(CurrentFrame().allocator.Get(), nullptr);
      if (FAILED(hr))
      {
        NativeLog("command list reset after a submit failed: 0x%08X (frame %llu)\n", unsigned(hr),
                  (unsigned long long)g.frame_number);
      }
      ID3D12DescriptorHeap *heaps[] = {g.view_heap.Get(), g.sampler_heap.Get()};
      g.list->SetDescriptorHeaps(2, heaps);
      g.bound.valid = false;
      g.draws_since_submit = 0;
      ++g.occlusion_submits;
    }

    // The rendering thread reads occlusion results of the previous frame
    // early in the next one, and waits for them: the GPU must not sit idle
    // while the command thread records a whole frame and then get it all at
    // the swap (CPU and GPU would take turns). After a few dozen draws the
    // open list is submitted when the GPU has nothing left to do, or when it
    // holds ended occlusion queries.
    void MaybeSubmitEarly()
    {
      uint32_t every = uint32_t(std::max(REXCVAR_GET(ao2_native_gpu_submit_draws), 0));
      if (!every || ++g.draws_since_submit < every || (g.draws_since_submit & 7))
      {
        return;
      }
      bool ended = !g.occlusion_ended.empty();
      if (ended || g.fence->GetCompletedValue() >= g.fence_value)
      {
        SubmitOpenList();
        ++g.early_submits;
        g.early_submits_idle += !ended;
      }
    }

    void BeginFrameIfNeeded()
    {
      if (g.list_open)
      {
        return;
      }
      Frame &f = CurrentFrame();
      WaitForFence(f.fence_value);
      // This slot's occlusion results are read before its range is reused.
      PublishOcclusion();
      f.oq_count = f.oq_resolved = 0;
      ++g.occlusion_epoch;
      if (g.timestamps && f.ts_resolved)
      {
        const uint64_t *t = g.timestamps + uint64_t(g.frame_index) * kTimestampsPerFrame;
        uint64_t frame_ticks = 0;
        for (uint32_t i = 0; i + 1 < f.ts_resolved; ++i)
        {
          if (f.ts_category[i] < kGpuCategoryCount && t[i + 1] >= t[i])
          {
            g.gpu_ticks[f.ts_category[i]] += t[i + 1] - t[i];
            frame_ticks += t[i + 1] - t[i];
          }
        }
        int32_t slow_us = REXCVAR_GET(ao2_native_gpu_capture_slow_us);
        uint32_t frame_us = uint32_t(double(frame_ticks) * 1e6 / double(g.timestamp_frequency));
        constexpr uint32_t kSlowRun = 8;
        g.slow_run = slow_us > 0 && frame_us > uint32_t(slow_us) ? g.slow_run + 1 : 0;
        if (g.slow_run >= kSlowRun && g.slow_captures < 5 && !g.slow_capture_pending &&
            !g_trace && g.draw_timings_slot == UINT32_MAX &&
            (g.slow_capture_frame == 0 || g.frame_number > g.slow_capture_frame + 960))
        {
          g.slow_capture_pending = true;
          g.slow_capture_us = frame_us;
          g.slow_capture_frame = g.frame_number;
        }
      }
      if (g.timestamps && g.draw_timings_slot == g.frame_index && !g.draw_timings.empty())
      {
        const uint64_t *t = g.timestamps + uint64_t(g.frame_index) * kTimestampsPerFrame;
        double us_per_tick = 1e6 / double(g.timestamp_frequency);
        struct ShaderTotal
        {
          double us = 0;
          uint32_t draws = 0;
        };
        std::unordered_map<uint64_t, ShaderTotal> by_ps, by_vs;
        std::vector<std::pair<double, size_t>> order;
        double total = 0;
        for (size_t i = 0; i < g.draw_timings.size(); ++i)
        {
          const auto &dt = g.draw_timings[i];
          double us = dt.end < f.ts_resolved && t[dt.end] >= t[dt.begin] ? double(t[dt.end] - t[dt.begin]) * us_per_tick : 0;
          total += us;
          by_ps[dt.ps].us += us;
          ++by_ps[dt.ps].draws;
          by_vs[dt.vs].us += us;
          ++by_vs[dt.vs].draws;
          order.push_back({us, i});
        }
        if (FILE *out = std::fopen("gpu_draws.txt", "w"))
        {
          std::fprintf(out, "%zu draws, %.0f us of GPU time\n", g.draw_timings.size(), total);
          auto dump = [&](const char *title, std::unordered_map<uint64_t, ShaderTotal> &m) {
            std::vector<std::pair<double, uint64_t>> v;
            for (auto &[h, st] : m)
            {
              v.push_back({st.us, h});
            }
            std::sort(v.rbegin(), v.rend());
            std::fprintf(out, "--- by %s ---\n", title);
            for (size_t i = 0; i < v.size() && i < 30; ++i)
            {
              std::fprintf(out, "%8.0f us  %4u draws  %016llX\n", v[i].first, m[v[i].second].draws,
                           (unsigned long long)v[i].second);
            }
          };
          dump("pixel shader", by_ps);
          dump("vertex shader", by_vs);
          std::vector<std::pair<double, size_t>> in_order = order;
          std::sort(order.rbegin(), order.rend());
          std::fprintf(out, "--- slowest draws ---\n");
          for (size_t i = 0; i < order.size() && i < 40; ++i)
          {
            const auto &dt = g.draw_timings[order[i].second];
            std::fprintf(out, "%8.0f us  draw %llu vs %016llX ps %016llX viewport %u px\n", order[i].first,
                         (unsigned long long)dt.draw, (unsigned long long)dt.vs, (unsigned long long)dt.ps, dt.pixels);
          }
          std::fprintf(out, "--- all draws ---\n");
          for (const auto &[us, i] : in_order)
          {
            const auto &dt = g.draw_timings[i];
            std::fprintf(out, "%8.1f us  draw %llu vs %016llX ps %016llX\n", us, (unsigned long long)dt.draw,
                         (unsigned long long)dt.vs, (unsigned long long)dt.ps);
          }
          std::fclose(out);
        }
        if (g.slow_traced_us)
        {
          std::error_code ec;
          std::string n = "slow_" + std::to_string(++g.slow_captures) + "_";
          std::filesystem::copy_file("frame_trace.txt", n + "frame_trace.txt",
                                     std::filesystem::copy_options::overwrite_existing, ec);
          std::filesystem::copy_file("gpu_draws.txt", n + "gpu_draws.txt",
                                     std::filesystem::copy_options::overwrite_existing, ec);
          NativeLog("slow frame %u us: the next one traced to %sframe_trace.txt / %sgpu_draws.txt\n",
                    g.slow_traced_us, n.c_str(), n.c_str());
          g.slow_traced_us = 0;
        }
        g.draw_timings.clear();
        g.draw_timings_slot = UINT32_MAX;
      }
      f.ts_count = f.ts_resolved = 0;
      f.ts_last = kGpuNone;
      if (f.probe_offset != kProbeOff)
      {
        uint64_t result = uint64_t(f.probe_size) << 33;
        if (f.probe_offset != kProbeNone)
        {
          uint16_t texel[4];
          std::memcpy(texel, f.readback_mapped + f.probe_offset, sizeof(texel));
          if (texel[3] >= 0x8000)
          {
            result |= texel[0] | (uint64_t(texel[1]) << 16) | (1ull << 32);
          }
        }
        g_probe_result.store(result, std::memory_order_relaxed);
        f.probe_offset = kProbeOff;
      }
      ApplyWritebacks(f);
      CollectGarbage();
      HRESULT allocator_hr = f.allocator->Reset();
      HRESULT list_hr = g.list->Reset(f.allocator.Get(), nullptr);
      if (FAILED(allocator_hr) || FAILED(list_hr))
      {
        NativeLog("command list reset failed: allocator 0x%08X list 0x%08X (frame %llu)\n", unsigned(allocator_hr),
                  unsigned(list_hr), (unsigned long long)g.frame_number);
      }
      // The first 1 KB is the display gamma ramp of the present blit (never fails to fit).
      f.upload_offset = sizeof(g.gamma_ramp);
      ++g.upload_generation;
      g.bound.valid = false;
      f.views_used = 0;
      ID3D12DescriptorHeap *heaps[] = {g.view_heap.Get(), g.sampler_heap.Get()};
      g.list->SetDescriptorHeaps(2, heaps);
      g.list_open = true;
    }

    bool CloseList()
    {
      Frame &tf = CurrentFrame();
      if (g.timestamp_heap && tf.ts_count > tf.ts_resolved)
      {
        tf.ts_last = kGpuNone;
        GpuMark(kGpuEnd);
        tf.ts_last = kGpuNone;
        uint32_t first = g.frame_index * kTimestampsPerFrame + tf.ts_resolved;
        g.list->ResolveQueryData(g.timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first,
                                 tf.ts_count - tf.ts_resolved, g.timestamp_readback.Get(), uint64_t(first) * 8);
        tf.ts_resolved = tf.ts_count;
      }
      if (g.occlusion_heap && tf.oq_count > tf.oq_resolved)
      {
        uint32_t first = g.frame_index * kOcclusionPerFrame + tf.oq_resolved;
        g.list->ResolveQueryData(g.occlusion_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, first,
                                 tf.oq_count - tf.oq_resolved, g.occlusion_readback.Get(), uint64_t(first) * 8);
        tf.oq_resolved = tf.oq_count;
      }
      HRESULT hr = g.list->Close();
      // Occlusion queries ended in this list complete with its submission (every
      // close is followed by the next fence signal, executed or not).
      if (!g.occlusion_ended.empty())
      {
        StartOcclusionThread();
        std::lock_guard lock(g.occlusion_mutex);
        for (OcclusionQuery &q : g.occlusion_ended)
        {
          q.fence = g.fence_value + 1;
          q.unknown |= FAILED(hr);
          g.occlusion_pending.push_back(q);
        }
        g.occlusion_ended.clear();
        g.occlusion_submitted.notify_one();
      }
      if (SUCCEEDED(hr))
      {
        return true;
      }
      NativeLog("command list Close failed (0x%08X) in frame %llu, dropping it; recent operations:\n",
                unsigned(hr), (unsigned long long)g.frame_number);
      for (uint32_t i = 0; i < 256; ++i)
      {
        const Breadcrumb &c = g_crumbs[(g_crumb_next + i) & 255];
        if (c.what)
        {
          NativeLog("  %s %llX %llX %llX %llX\n", c.what, (unsigned long long)c.a, (unsigned long long)c.b,
                    (unsigned long long)c.c, (unsigned long long)c.d);
        }
      }
      return false;
    }

    // Invalid arguments make the command list fail to close (and, executed,
    // remove the device): checked before clears and copies, logged once each.
    void LogInvalidCall(const char *what, ID3D12Resource *resource, int64_t a, int64_t b, int64_t c, int64_t d)
    {
      static uint32_t logged = 0;
      if (logged++ < 32)
      {
        NativeLog("skipped invalid %s on %p: %lld %lld %lld %lld (frame %llu)\n", what, static_cast<void *>(resource),
                  (long long)a, (long long)b, (long long)c, (long long)d, (unsigned long long)g.frame_number);
      }
    }

    // True if `rect` is non-empty and inside the resource's top level.
    bool RectFits(ID3D12Resource *resource, const D3D12_RECT &rect, const char *what)
    {
      D3D12_RESOURCE_DESC desc = resource->GetDesc();
      if (rect.left < 0 || rect.top < 0 || rect.left >= rect.right || rect.top >= rect.bottom ||
          uint64_t(rect.right) > desc.Width || uint32_t(rect.bottom) > desc.Height)
      {
        LogInvalidCall(what, resource, rect.left, rect.top, rect.right, rect.bottom);
        return false;
      }
      return true;
    }

    bool BoxFits(ID3D12Resource *resource, const D3D12_BOX &box, const char *what)
    {
      return RectFits(resource, D3D12_RECT{LONG(box.left), LONG(box.top), LONG(box.right), LONG(box.bottom)}, what);
    }

    // ------------------------------------------------------------ Formats

    DXGI_FORMAT ColorFormat(xenos::ColorRenderTargetFormat f)
    {
      switch (f)
      {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
      case xenos::ColorRenderTargetFormat::k_16_16:
        return DXGI_FORMAT_R16G16_SNORM;
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        return DXGI_FORMAT_R16G16B16A16_SNORM;
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        return DXGI_FORMAT_R16G16_FLOAT;
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
        return DXGI_FORMAT_R32G32_FLOAT;
      default:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
      }
    }

    uint32_t BytesPerTexel(DXGI_FORMAT f)
    {
      switch (f)
      {
      case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
      case DXGI_FORMAT_R16G16B16A16_UNORM:
      case DXGI_FORMAT_R16G16B16A16_SNORM:
      case DXGI_FORMAT_R32G32_FLOAT: return 8;
      case DXGI_FORMAT_R8_UNORM: return 1;
      case DXGI_FORMAT_R8G8_UNORM:
      case DXGI_FORMAT_R16_FLOAT:
      case DXGI_FORMAT_R16_UNORM: return 2;
      default: return 4;
      }
    }

    uint32_t BytesPerPixel(DXGI_FORMAT f)
    {
      switch (f)
      {
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
      case DXGI_FORMAT_R16G16B16A16_SNORM:
      case DXGI_FORMAT_R32G32_FLOAT:
      case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return 8;
      default:
        return 4;
      }
    }

    // EDRAM formats sharing one host render target (same bits, different
    // interpretation when rendering).
    xenos::ColorRenderTargetFormat KeyColorFormat(xenos::ColorRenderTargetFormat f)
    {
      switch (f)
      {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: return xenos::ColorRenderTargetFormat::k_8_8_8_8;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
        return xenos::ColorRenderTargetFormat::k_2_10_10_10;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
        return xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT;
      default: return f;
      }
    }

    bool IsFixed16(xenos::ColorRenderTargetFormat f)
    {
      return f == xenos::ColorRenderTargetFormat::k_16_16 || f == xenos::ColorRenderTargetFormat::k_16_16_16_16;
    }

    DXGI_FORMAT TypelessFormat(DXGI_FORMAT f)
    {
      switch (f)
      {
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_R8G8B8A8_SNORM:
      case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
      case DXGI_FORMAT_R10G10B10A2_UNORM:
      case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
      case DXGI_FORMAT_R16G16B16A16_UNORM:
      case DXGI_FORMAT_R16G16B16A16_SNORM:
      case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
      case DXGI_FORMAT_R16G16_FLOAT:
      case DXGI_FORMAT_R16G16_UNORM:
      case DXGI_FORMAT_R16G16_SNORM:
      case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_TYPELESS;
      case DXGI_FORMAT_R16_FLOAT:
      case DXGI_FORMAT_R16_UNORM:
      case DXGI_FORMAT_R16_SNORM:
      case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_TYPELESS;
      case DXGI_FORMAT_R32_FLOAT:
      case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_TYPELESS;
      case DXGI_FORMAT_R32G32_FLOAT:
      case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_TYPELESS;
      case DXGI_FORMAT_R8_UNORM:
      case DXGI_FORMAT_R8_SNORM:
      case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_TYPELESS;
      case DXGI_FORMAT_R8G8_UNORM:
      case DXGI_FORMAT_R8G8_SNORM:
      case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_TYPELESS;
      default: return f;
      }
    }

    bool Is64bpp(xenos::ColorRenderTargetFormat f)
    {
      return f == xenos::ColorRenderTargetFormat::k_16_16_16_16 ||
             f == xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT ||
             f == xenos::ColorRenderTargetFormat::k_32_32_FLOAT;
    }

    DXGI_FORMAT DepthFormat(xenos::DepthRenderTargetFormat f)
    {
      return f == xenos::DepthRenderTargetFormat::kD24FS8 ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT
                                                           : DXGI_FORMAT_D24_UNORM_S8_UINT;
    }

    DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f)
    {
      return f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
                                                    : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    }

    // ------------------------------------------------------------ Render targets

    // Height a surface can have from `base` to the end of EDRAM.
    uint32_t SurfaceHeight(uint32_t base, uint32_t pitch, xenos::MsaaSamples msaa, bool is_64bpp)
    {
      uint32_t tile_w = 80 >> (is_64bpp ? 1 : 0);
      uint32_t tile_h = 16;
      if (msaa >= xenos::MsaaSamples::k2X)
      {
        tile_h = 8;
      }
      if (msaa >= xenos::MsaaSamples::k4X)
      {
        tile_w >>= 1;
      }
      uint32_t tiles_per_row = std::max((pitch + tile_w - 1) / tile_w, 1u);
      uint32_t rows = (2048 - std::min(base, 2047u)) / tiles_per_row * tile_h;
      return std::clamp(rows, 8u, 2048u);
    }

    RenderTarget *GetRenderTarget(uint32_t base, uint32_t format, bool depth, uint32_t pitch,
                                  xenos::MsaaSamples msaa)
    {
      if (!depth)
      {
        format = uint32_t(KeyColorFormat(xenos::ColorRenderTargetFormat(format)));
      }
      uint64_t key = uint64_t(base) | (uint64_t(format) << 12) | (uint64_t(pitch) << 17) |
                     (uint64_t(msaa) << 31) | (uint64_t(depth) << 33);
      auto it = g.render_targets.find(key);
      if (it != g.render_targets.end())
      {
        return it->second.get();
      }
      auto rt = std::make_unique<RenderTarget>();
      rt->depth = depth;
      rt->samples = 1u << uint32_t(msaa);
      rt->width = std::max(pitch, 1u);
      bool is_64bpp = !depth && Is64bpp(xenos::ColorRenderTargetFormat(format));
      rt->height = SurfaceHeight(base, pitch, msaa, is_64bpp);
      rt->format = depth ? DepthFormat(xenos::DepthRenderTargetFormat(format))
                         : ColorFormat(xenos::ColorRenderTargetFormat(format));
      rt->scale4 = SurfaceScale4(pitch, rt->samples);
      if (depth && rt->samples == 1)
      {
        // The shadow buffer at the shadow resolution, only as tall as it is used.
        uint32_t rows = 0;
        if (uint32_t shadow_scale4 = ShadowScale4(base, format, pitch, &rows))
        {
          rt->scale4 = shadow_scale4;
          rt->height = std::clamp((rows + 15) & ~15u, 16u, rt->height);
        }
      }
      D3D12_RESOURCE_DESC d = {};
      d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      d.Width = ScaleSize(rt->width, rt->scale4);
      d.Height = ScaleSize(rt->height, rt->scale4);
      d.DepthOrArraySize = 1;
      d.MipLevels = 1;
      d.SampleDesc.Count = rt->samples;
      d.Format = TypelessFormat(rt->format);
      rt->fixed16 = !depth && IsFixed16(xenos::ColorRenderTargetFormat(format));
      rt->edram_base = base;
      rt->edram_pitch = pitch;
      rt->edram_format = format;
      rt->edram_id = g.next_rt_id++;
      rt->is_64bpp = is_64bpp;
      if (depth)
      {
        d.Format = rt->format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? DXGI_FORMAT_R32G8X24_TYPELESS
                                                                  : DXGI_FORMAT_R24G8_TYPELESS;
      }
      d.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
      D3D12_CLEAR_VALUE cv = {};
      cv.Format = rt->format;
      if (depth)
      {
        cv.DepthStencil.Depth = 1.0f;
      }
      rt->state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
      ++g.resources_created;
      if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, rt->state, depth ? &cv : nullptr,
                                                   IID_PPV_ARGS(&rt->resource))))
      {
        NativeLog("render target creation failed (%ux%u fmt %u)\n", rt->width, rt->height, format);
        return nullptr;
      }
      if (depth)
      {
        rt->view = g.dsv_heap->GetCPUDescriptorHandleForHeapStart();
        rt->view.ptr += uint64_t(g.dsvs_used++ % 256) * g.dsv_size;
        D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = rt->format;
        dv.ViewDimension = rt->samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        g.device->CreateDepthStencilView(rt->resource.Get(), &dv, rt->view);
      }
      else
      {
        D3D12_RENDER_TARGET_VIEW_DESC rv = {};
        rv.Format = rt->format;
        rv.ViewDimension = rt->samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
        rt->view = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rt->view.ptr += uint64_t(g.rtvs_used++ % 1024) * g.rtv_size;
        g.device->CreateRenderTargetView(rt->resource.Get(), &rv, rt->view);
        rt->view_srgb = rt->view;
        if (rt->format == DXGI_FORMAT_R8G8B8A8_UNORM)
        {
          rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
          rt->view_srgb = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
          rt->view_srgb.ptr += uint64_t(g.rtvs_used++ % 1024) * g.rtv_size;
          g.device->CreateRenderTargetView(rt->resource.Get(), &rv, rt->view_srgb);
        }
      }
      NativeLog("render target base=%u fmt=%u depth=%d %ux%u x%u (host %ux%u)\n", base, format, int(depth),
                rt->width, rt->height, rt->samples, unsigned(d.Width), unsigned(d.Height));
      RenderTarget *result = rt.get();
      g.render_targets.emplace(key, std::move(rt));
      return result;
    }

    // ------------------------------------------------------------ Pipelines

    D3D12_BLEND Blend(xenos::BlendFactor f, bool alpha)
    {
      switch (f)
      {
      case xenos::BlendFactor::kZero: return D3D12_BLEND_ZERO;
      case xenos::BlendFactor::kOne: return D3D12_BLEND_ONE;
      case xenos::BlendFactor::kSrcColor: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
      case xenos::BlendFactor::kOneMinusSrcColor:
        return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
      case xenos::BlendFactor::kSrcAlpha: return D3D12_BLEND_SRC_ALPHA;
      case xenos::BlendFactor::kOneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
      case xenos::BlendFactor::kDstColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
      case xenos::BlendFactor::kOneMinusDstColor:
        return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
      case xenos::BlendFactor::kDstAlpha: return D3D12_BLEND_DEST_ALPHA;
      case xenos::BlendFactor::kOneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
      case xenos::BlendFactor::kConstantColor:
      case xenos::BlendFactor::kConstantAlpha: return D3D12_BLEND_BLEND_FACTOR;
      case xenos::BlendFactor::kOneMinusConstantColor:
      case xenos::BlendFactor::kOneMinusConstantAlpha: return D3D12_BLEND_INV_BLEND_FACTOR;
      case xenos::BlendFactor::kSrcAlphaSaturate: return D3D12_BLEND_SRC_ALPHA_SAT;
      default: return D3D12_BLEND_ONE;
      }
    }

    D3D12_BLEND_OP BlendOp(xenos::BlendOp op)
    {
      switch (op)
      {
      case xenos::BlendOp::kSubtract: return D3D12_BLEND_OP_SUBTRACT;
      case xenos::BlendOp::kMin: return D3D12_BLEND_OP_MIN;
      case xenos::BlendOp::kMax: return D3D12_BLEND_OP_MAX;
      case xenos::BlendOp::kRevSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
      default: return D3D12_BLEND_OP_ADD;
      }
    }

    D3D12_STENCIL_OP StencilOp(xenos::StencilOp op)
    {
      switch (op)
      {
      case xenos::StencilOp::kZero: return D3D12_STENCIL_OP_ZERO;
      case xenos::StencilOp::kReplace: return D3D12_STENCIL_OP_REPLACE;
      case xenos::StencilOp::kIncrementClamp: return D3D12_STENCIL_OP_INCR_SAT;
      case xenos::StencilOp::kDecrementClamp: return D3D12_STENCIL_OP_DECR_SAT;
      case xenos::StencilOp::kInvert: return D3D12_STENCIL_OP_INVERT;
      case xenos::StencilOp::kIncrementWrap: return D3D12_STENCIL_OP_INCR;
      case xenos::StencilOp::kDecrementWrap: return D3D12_STENCIL_OP_DECR;
      default: return D3D12_STENCIL_OP_KEEP;
      }
    }

    D3D12_COMPARISON_FUNC Compare(xenos::CompareFunction f)
    {
      return D3D12_COMPARISON_FUNC(uint32_t(f) + 1);
    }

    // Guest polygon offset (PA_SU_POLY_OFFSET_*): the front face's offset unless
    // front faces are culled, the front one via poly_offset_para_enable for
    // non-polygons (the SDK's GetPreferredFacePolygonOffset). The constant part
    // is an absolute depth offset: applied exactly in the vertex shader
    // (`offset_out`, added to the window depth). Host DepthBias is relative to
    // the primitive's exponent with a float depth buffer - the SDK's conversion
    // assumes depth in [0.5, 1), but this game's reversed Z keeps walls around
    // 0.01..0.1, where that gave 8..32x too little offset and bullet hole decals
    // z-fought per triangle. The slope part (scale in 1/16 subpixels) is the
    // host SlopeScaledDepthBias, per host pixel.
    void PolygonOffset(const RegisterFile &r, bool polygonal, uint32_t scale4, float &offset_out, float &slope_out)
    {
      auto mode = r.Get<reg::PA_SU_SC_MODE_CNTL>();
      float scale = 0.0f, offset = 0.0f;
      if (polygonal)
      {
        if (mode.poly_offset_front_enable && !mode.cull_front)
        {
          scale = r.F(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
          offset = r.F(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
        }
        if (mode.poly_offset_back_enable && !mode.cull_back && !scale && !offset)
        {
          scale = r.F(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
          offset = r.F(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
        }
      }
      else if (mode.poly_offset_para_enable)
      {
        scale = r.F(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        offset = r.F(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      }
      offset_out = offset;
      // Slopes per host pixel are smaller by the resolution scale.
      slope_out = scale * (1.0f / 16.0f) * (float(scale4) * 0.25f);
    }

    void RecordPipeline(const PipelineKey &key, const CompiledShader *vs, const CompiledShader *ps);
    void QueuePipeline(const PipelineKey &key, const CompiledShader *vs, const CompiledShader *ps);
    void BuildPipelineDesc(const PipelineKey &key, const CompiledShader *vs, const CompiledShader *ps,
                           D3D12_GRAPHICS_PIPELINE_STATE_DESC &d);

    ID3D12PipelineState *GetPipeline(const PipelineKey &key, const CompiledShader *vs,
                                     const CompiledShader *ps)
    {
      auto it = g.pipelines.find(key);
      if (it != g.pipelines.end())
      {
        return it->second.Get();
      }
      if (REXCVAR_GET(ao2_native_gpu_async_pipelines) && !g_trace)
      {
        // Created on the background thread; the draw is skipped until then.
        QueuePipeline(key, vs, ps);
        ++g.frame_pso_queued;
        return nullptr;
      }
      D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
      BuildPipelineDesc(key, vs, ps, d);
      ComPtr<ID3D12PipelineState> pso;
      auto pso_start = std::chrono::steady_clock::now();
      HRESULT hr = g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
      uint64_t pso_ns = Elapsed(pso_start);
      g.time_pso_ns += pso_ns;
      g.frame_pso_ns += pso_ns;
      ++g.frame_pso_new;
      ++g.pso_created;
      if (FAILED(hr))
      {
        NativeLog("pipeline creation failed (0x%08X) vs=%016llX ps=%016llX\n", unsigned(hr),
                  (unsigned long long)vs->shader->ucode_data_hash(),
                  ps ? (unsigned long long)ps->shader->ucode_data_hash() : 0ull);
      }
      else
      {
        RecordPipeline(key, vs, ps);
      }
      g.pipelines.emplace(key, pso);
      return pso.Get();
    }

    void BuildPipelineDesc(const PipelineKey &key, const CompiledShader *vs, const CompiledShader *ps,
                           D3D12_GRAPHICS_PIPELINE_STATE_DESC &d)
    {
      d = {};
      d.pRootSignature = g.root_signature.Get();
      d.VS = {vs->bytecode.data(), vs->bytecode.size()};
      if (ps)
      {
        d.PS = {ps->bytecode.data(), ps->bytecode.size()};
      }
      if (key.rect_gs)
      {
        d.GS = {g.rect_gs.data(), g.rect_gs.size()};
      }
      d.BlendState.IndependentBlendEnable = TRUE;
      for (uint32_t i = 0; i < 4; ++i)
      {
        auto &rt = d.BlendState.RenderTarget[i];
        rt.RenderTargetWriteMask = key.write_mask[i];
        reg::RB_BLENDCONTROL b;
        b.value = key.blend[i];
        if (key.blend[i])
        {
          rt.BlendEnable = TRUE;
          rt.SrcBlend = Blend(b.color_srcblend, false);
          rt.DestBlend = Blend(b.color_destblend, false);
          rt.BlendOp = BlendOp(b.color_comb_fcn);
          rt.SrcBlendAlpha = Blend(b.alpha_srcblend, true);
          rt.DestBlendAlpha = Blend(b.alpha_destblend, true);
          rt.BlendOpAlpha = BlendOp(b.alpha_comb_fcn);
        }
      }
      d.SampleMask = UINT_MAX;
      reg::PA_SU_SC_MODE_CNTL mode;
      mode.value = key.raster;
      auto &rs = d.RasterizerState;
      rs.FillMode = D3D12_FILL_MODE_SOLID;
      rs.CullMode = D3D12_CULL_MODE_NONE;
      // Rectangle lists are not polygons for culling and facing (like in the
      // SDK's IsPrimitivePolygonal): D3D draws them with whatever cull mode the
      // previous draws left (e.g. the full screen depth restore after the scene
      // resolve), and they always count as front facing.
      if (key.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && !key.rect_gs)
      {
        if (mode.cull_front && !mode.cull_back)
        {
          rs.CullMode = D3D12_CULL_MODE_FRONT;
        }
        else if (mode.cull_back && !mode.cull_front)
        {
          rs.CullMode = D3D12_CULL_MODE_BACK;
        }
      }
      rs.FrontCounterClockwise = mode.face ? FALSE : TRUE;
      rs.DepthClipEnable = (key.raster >> 31) ? FALSE : TRUE;
      rs.SlopeScaledDepthBias = key.depth_bias_slope;
      reg::RB_DEPTHCONTROL dc;
      dc.value = key.depth_control;
      auto &ds = d.DepthStencilState;
      if (key.ds_format)
      {
        ds.DepthEnable = dc.z_enable;
        ds.DepthWriteMask = dc.z_write_enable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = Compare(dc.zfunc);
        ds.StencilEnable = dc.stencil_enable;
        ds.StencilReadMask = uint8_t(key.stencil_masks);
        ds.StencilWriteMask = uint8_t(key.stencil_masks >> 8);
        ds.FrontFace = {StencilOp(dc.stencilfail), StencilOp(dc.stencilzfail), StencilOp(dc.stencilzpass),
                        Compare(dc.stencilfunc)};
        if (dc.backface_enable && !key.rect_gs)
        {
          ds.BackFace = {StencilOp(dc.stencilfail_bf), StencilOp(dc.stencilzfail_bf),
                         StencilOp(dc.stencilzpass_bf), Compare(dc.stencilfunc_bf)};
        }
        else
        {
          ds.BackFace = ds.FrontFace;
        }
        d.DSVFormat = DXGI_FORMAT(key.ds_format);
      }
      d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topology);
      for (uint32_t i = 0; i < 4; ++i)
      {
        if (key.rt_formats[i])
        {
          d.NumRenderTargets = i + 1;
          d.RTVFormats[i] = DXGI_FORMAT(key.rt_formats[i]);
        }
      }
      d.SampleDesc.Count = key.samples;
    }

    // ------------------------------------------------------------ Pipeline storage

    // Every shader variant and pipeline the game uses is appended to
    // shader_cache/pipelines.bin. In the next sessions they are translated
    // at startup (on this thread, a few ms per frame: the shader cache is not
    // thread safe; FXC runs on its workers or comes from the DXBC cache) and
    // the pipelines created on a background thread, so neither a new area
    // nor the first frames stall on them.
    constexpr uint32_t kStorageMagic = 0x50324F41; // "AO2P"
    constexpr uint32_t kStorageVersion = 1;
    constexpr const char *kStoragePath = "shader_cache/pipelines.bin";

    uint64_t StoredShaderId(uint8_t type, uint64_t hash, uint64_t modification)
    {
      return (hash ^ (uint64_t(type) << 63)) * 0x9E3779B97F4A7C15ull ^ modification;
    }

    uint64_t StoredPipelineId(const State::StoredPipeline &p)
    {
      const uint8_t *b = reinterpret_cast<const uint8_t *>(&p);
      uint64_t h = 0xCBF29CE484222325ull;
      for (size_t i = 0; i < sizeof(p); ++i)
      {
        h = (h ^ b[i]) * 0x100000001B3ull;
      }
      return h;
    }

    bool OpenStorageForAppend()
    {
      if (g.storage)
      {
        return true;
      }
      std::error_code ec;
      std::filesystem::create_directories("shader_cache", ec);
      bool fresh = !std::filesystem::exists(kStoragePath, ec) || std::filesystem::file_size(kStoragePath, ec) == 0;
      g.storage = std::fopen(kStoragePath, "ab");
      if (g.storage && fresh)
      {
        uint32_t header[3] = {kStorageMagic, kStorageVersion, uint32_t(sizeof(PipelineKey))};
        std::fwrite(header, sizeof(header), 1, g.storage);
      }
      return g.storage != nullptr;
    }

    void RecordShader(const CompiledShader *cs)
    {
      const auto &shader = *cs->shader;
      uint8_t type = uint8_t(shader.type());
      if (!g.stored_shaders.insert(StoredShaderId(type, shader.ucode_data_hash(), cs->modification)).second ||
          !OpenStorageForAppend())
      {
        return;
      }
      uint32_t count = uint32_t(shader.ucode_data().size());
      uint64_t hash = shader.ucode_data_hash(), modification = cs->modification;
      std::fputc('S', g.storage);
      std::fputc(type, g.storage);
      std::fwrite(&hash, 8, 1, g.storage);
      std::fwrite(&modification, 8, 1, g.storage);
      std::fwrite(&count, 4, 1, g.storage);
      std::vector<uint32_t> be(shader.ucode_data());
      for (uint32_t &v : be)
      {
        v = __builtin_bswap32(v);
      }
      std::fwrite(be.data(), 4, count, g.storage);
    }

    void RecordPipeline(const PipelineKey &key, const CompiledShader *vs, const CompiledShader *ps)
    {
      if (!g.storage_loaded)
      {
        return; // not before the stored set is known (no duplicates)
      }
      State::StoredPipeline sp = {};
      sp.vs_hash = vs->shader->ucode_data_hash();
      sp.vs_modification = vs->modification;
      sp.ps_hash = ps ? ps->shader->ucode_data_hash() : 0;
      sp.ps_modification = ps ? ps->modification : 0;
      sp.key = key;
      sp.key.vs = sp.key.ps = nullptr;
      if (!g.stored_pipelines.insert(StoredPipelineId(sp)).second)
      {
        return;
      }
      RecordShader(vs);
      if (ps)
      {
        RecordShader(ps);
      }
      if (!g.storage)
      {
        return;
      }
      std::fputc('P', g.storage);
      std::fwrite(&sp, sizeof(sp), 1, g.storage);
      std::fflush(g.storage);
    }

    void LoadStorage()
    {
      g.storage_loaded = true;
      g.prewarm_start = std::chrono::steady_clock::now();
      FILE *f = std::fopen(kStoragePath, "rb");
      if (!f)
      {
        return;
      }
      uint32_t header[3] = {};
      bool valid = std::fread(header, sizeof(header), 1, f) == 1 && header[0] == kStorageMagic &&
                   header[1] == kStorageVersion && header[2] == sizeof(PipelineKey);
      while (valid)
      {
        int tag = std::fgetc(f);
        if (tag == 'S')
        {
          State::StoredShader st;
          int type = std::fgetc(f);
          uint32_t count = 0;
          if (type == EOF || std::fread(&st.hash, 8, 1, f) != 1 || std::fread(&st.modification, 8, 1, f) != 1 ||
              std::fread(&count, 4, 1, f) != 1 || count == 0 || count > 65536)
          {
            break;
          }
          st.type = uint8_t(type);
          st.dwords.resize(count);
          if (std::fread(st.dwords.data(), 4, count, f) != count)
          {
            break;
          }
          if (g.stored_shaders.insert(StoredShaderId(st.type, st.hash, st.modification)).second)
          {
            g.prewarm_shaders.push_back(std::move(st));
          }
        }
        else if (tag == 'P')
        {
          State::StoredPipeline sp;
          if (std::fread(&sp, sizeof(sp), 1, f) != 1)
          {
            break;
          }
          if (g.stored_pipelines.insert(StoredPipelineId(sp)).second)
          {
            g.prewarm_pipelines.push_back(sp);
          }
        }
        else
        {
          break;
        }
      }
      std::fclose(f);
      if (!valid)
      {
        // Another layout: start over.
        std::error_code ec;
        std::filesystem::remove(kStoragePath, ec);
      }
      NativeLog("pipeline storage: %zu shaders, %zu pipelines to prepare\n", g.prewarm_shaders.size(),
                g.prewarm_pipelines.size());
    }

    void PsoThreadMain()
    {
      for (;;)
      {
        State::PsoJob job;
        {
          std::unique_lock lock(g.pso_mutex);
          g.pso_cv.wait(lock, [] { return !g.pso_jobs.empty(); });
          job = g.pso_jobs.front();
          g.pso_jobs.pop_front();
        }
        ComPtr<ID3D12PipelineState> pso;
        if (FAILED(g.device->CreateGraphicsPipelineState(&job.desc, IID_PPV_ARGS(&pso))))
        {
          pso = nullptr;
        }
        std::lock_guard lock(g.pso_mutex);
        g.pso_done.emplace_back(job.key, std::move(pso));
      }
    }

    // Once per frame on the GPU thread: translates stored shaders within a
    // time budget, hands pipelines whose shaders are ready to the background
    // thread, and takes in the pipelines it created.
    void PrewarmStep(ShaderCache &shaders)
    {
      if (g.prewarm_frame == g.frame_number)
      {
        return;
      }
      g.prewarm_frame = g.frame_number;
      if (!g.storage_loaded)
      {
        LoadStorage();
      }
      {
        std::lock_guard lock(g.pso_mutex);
        for (auto &done : g.pso_done)
        {
          g.pso_queued.erase(done.first);
          if (g.pipelines.find(done.first) != g.pipelines.end())
          {
            continue;
          }
          auto *vs = static_cast<const CompiledShader *>(done.first.vs);
          auto *ps = static_cast<const CompiledShader *>(done.first.ps);
          if (!done.second)
          {
            NativeLog("pipeline creation failed (background) vs=%016llX ps=%016llX\n",
                      (unsigned long long)vs->shader->ucode_data_hash(),
                      ps ? (unsigned long long)ps->shader->ucode_data_hash() : 0ull);
          }
          else if (g.storage_loaded)
          {
            RecordPipeline(done.first, vs, ps);
          }
          g.pipelines.emplace(done.first, std::move(done.second));
          ++g.prewarm_created;
        }
        g.pso_done.clear();
      }
      if (g.prewarm_shader_pos >= g.prewarm_shaders.size() && g.prewarm_pipelines.empty())
      {
        if (!g.prewarm_logged && g.pso_queued.empty() && (g.prewarm_created || g.prewarm_shader_pos))
        {
          g.prewarm_logged = true;
          NativeLog("pipeline storage: prepared %zu shaders, %u pipelines in %.1f s\n", g.prewarm_shader_pos,
                    g.prewarm_created,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - g.prewarm_start).count());
        }
        return;
      }
      auto start = std::chrono::steady_clock::now();
      auto budget = std::chrono::microseconds(3000);
      while (g.prewarm_shader_pos < g.prewarm_shaders.size() && std::chrono::steady_clock::now() - start < budget)
      {
        const auto &st = g.prewarm_shaders[g.prewarm_shader_pos++];
        const CompiledShader *cs = shaders.Get(xenos::ShaderType(st.type), st.dwords.data(),
                                               uint32_t(st.dwords.size()), st.hash, st.modification);
        if (cs)
        {
          g.prewarm_compiled[StoredShaderId(st.type, st.hash, st.modification)] = cs;
        }
      }
      // Pipelines whose shaders are translated and compiled.
      size_t kept = 0;
      for (size_t i = 0; i < g.prewarm_pipelines.size(); ++i)
      {
        const auto &sp = g.prewarm_pipelines[i];
        auto vs_it = g.prewarm_compiled.find(StoredShaderId(uint8_t(xenos::ShaderType::kVertex), sp.vs_hash,
                                                            sp.vs_modification));
        auto ps_it = sp.ps_hash ? g.prewarm_compiled.find(StoredShaderId(uint8_t(xenos::ShaderType::kPixel),
                                                                         sp.ps_hash, sp.ps_modification))
                                : g.prewarm_compiled.end();
        bool shaders_known = vs_it != g.prewarm_compiled.end() && (!sp.ps_hash || ps_it != g.prewarm_compiled.end());
        if (!shaders_known)
        {
          // Not translated yet: wait, unless all stored shaders are (then it
          // refers to a shader that is gone).
          if (g.prewarm_shader_pos < g.prewarm_shaders.size())
          {
            g.prewarm_pipelines[kept++] = sp;
          }
          continue;
        }
        const CompiledShader *vs = vs_it->second;
        const CompiledShader *ps = sp.ps_hash ? ps_it->second : nullptr;
        if (vs->pending() || (ps && ps->pending()))
        {
          g.prewarm_pipelines[kept++] = sp;
          continue;
        }
        if (!vs->ok() || (ps && !ps->ok()))
        {
          continue;
        }
        PipelineKey key = sp.key;
        key.vs = vs;
        key.ps = ps;
        if (g.pipelines.find(key) == g.pipelines.end())
        {
          QueuePipeline(key, vs, ps);
        }
      }
      g.prewarm_pipelines.resize(kept);
    }

    // Hands a pipeline to the background thread (once).
    void QueuePipeline(const PipelineKey &key, const CompiledShader *vs, const CompiledShader *ps)
    {
      if (!g.pso_queued.insert(key).second)
      {
        return;
      }
      if (!g.pso_thread_started)
      {
        g.pso_thread_started = true;
        std::thread(PsoThreadMain).detach();
      }
      State::PsoJob job;
      job.key = key;
      BuildPipelineDesc(key, vs, ps, job.desc);
      {
        std::lock_guard lock(g.pso_mutex);
        g.pso_jobs.push_back(job);
      }
      g.pso_cv.notify_one();
    }

    // ------------------------------------------------------------ Viewport

    struct Viewport
    {
      D3D12_VIEWPORT viewport;
      D3D12_RECT scissor;
      float ndc_scale[3], ndc_offset[3];
    };

    // Port of draw_util::GetHostViewportInfo / GetScissor (no resolution
    // scaling, top-left origin, no reverse Z). `ps_writes_depth`: the host
    // clamps SV_Depth to the viewport's depth range (the guest does not clamp
    // oDepth to it), so the range is applied in the vertex shader instead.
    Viewport ComputeViewport(const RegisterFile &r, uint32_t width, uint32_t height, bool ps_writes_depth)
    {
      Viewport out = {};
      auto clip = r.Get<reg::PA_CL_CLIP_CNTL>();
      auto vte = r.Get<reg::PA_CL_VTE_CNTL>();
      auto mode = r.Get<reg::PA_SU_SC_MODE_CNTL>();
      auto vtx = r.Get<reg::PA_SU_VTX_CNTL>();
      float scale_xy[2] = {vte.vport_x_scale_ena ? r.F(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f,
                           vte.vport_y_scale_ena ? r.F(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f};
      float scale_z = vte.vport_z_scale_ena ? r.F(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
      float offset_base_xy[2] = {vte.vport_x_offset_ena ? r.F(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f,
                                 vte.vport_y_offset_ena ? r.F(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f};
      float offset_z = vte.vport_z_offset_ena ? r.F(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
      float offset_add_xy[2] = {};
      auto window_offset = r.Get<reg::PA_SC_WINDOW_OFFSET>();
      if (mode.vtx_window_offset_enable)
      {
        offset_add_xy[0] += float(window_offset.window_x_offset);
        offset_add_xy[1] += float(window_offset.window_y_offset);
      }
      if (vtx.pix_center == xenos::PixelCenter::kD3DZero)
      {
        offset_add_xy[0] += 0.5f;
        offset_add_xy[1] += 0.5f;
      }
      uint32_t xy_max[2] = {width, height};
      float z_min = 0.0f, z_max = 1.0f;
      uint32_t vp_offset[2] = {}, vp_extent[2] = {};
      if (clip.clip_disable)
      {
        for (uint32_t i = 0; i < 2; ++i)
        {
          vp_extent[i] = xy_max[i];
          float extent = float(xy_max[i]);
          float pixels_to_ndc = 2.0f / extent;
          out.ndc_scale[i] = scale_xy[i] * pixels_to_ndc;
          out.ndc_offset[i] = (offset_base_xy[i] - extent * 0.5f + offset_add_xy[i]) * pixels_to_ndc;
        }
        out.ndc_scale[2] = scale_z;
        out.ndc_offset[2] = offset_z;
      }
      else
      {
        for (uint32_t i = 0; i < 2; ++i)
        {
          float offset_axis = offset_base_xy[i] + offset_add_xy[i];
          float scale_abs = std::abs(scale_xy[i]);
          float axis_max = float(xy_max[i]);
          uint32_t a0 = uint32_t(std::clamp(offset_axis - scale_abs, 0.0f, axis_max));
          uint32_t a1 = uint32_t(std::clamp(offset_axis + scale_abs, 0.0f, axis_max));
          uint32_t extent = a1 - a0;
          vp_offset[i] = a0;
          vp_extent[i] = extent;
          if (extent)
          {
            float e = float(extent);
            out.ndc_scale[i] = scale_xy[i] * 2.0f / e;
            out.ndc_offset[i] = (offset_axis - (float(a0) + e * 0.5f)) * 2.0f / e;
          }
          else
          {
            out.ndc_scale[i] = 1.0f;
            out.ndc_offset[i] = 0.0f;
          }
        }
        float host_offset_z, host_scale_z;
        if (clip.dx_clip_space_def)
        {
          host_offset_z = offset_z;
          host_scale_z = scale_z;
          out.ndc_scale[2] = 1.0f;
          out.ndc_offset[2] = 0.0f;
        }
        else
        {
          host_offset_z = offset_z - scale_z;
          host_scale_z = scale_z * 2.0f;
          out.ndc_scale[2] = 0.5f;
          out.ndc_offset[2] = 0.5f;
        }
        z_min = std::clamp(host_offset_z, 0.0f, 1.0f);
        z_max = std::clamp(host_offset_z + host_scale_z, 0.0f, 1.0f);
        if (z_min > z_max)
        {
          std::swap(z_min, z_max);
          out.ndc_scale[2] = -out.ndc_scale[2];
          out.ndc_offset[2] = 1.0f - out.ndc_offset[2];
        }
        if (ps_writes_depth)
        {
          // Host NDC z -> z_min + z * (z_max - z_min) in the shader, viewport 0..1.
          float range = z_max - z_min;
          out.ndc_scale[2] *= range;
          out.ndc_offset[2] = out.ndc_offset[2] * range + z_min;
          z_min = 0.0f;
          z_max = 1.0f;
        }
      }
      // Guest Y points down in NDC like the window, host NDC Y points up
      // (origin_bottom_left in the SDK's GetHostViewportInfo).
      out.ndc_scale[1] = -out.ndc_scale[1];
      out.ndc_offset[1] = -out.ndc_offset[1];
      out.viewport = {float(vp_offset[0]), float(vp_offset[1]), float(vp_extent[0]),
                      float(vp_extent[1]), z_min, z_max};

      auto tl = r.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
      auto br = r.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
      int32_t x0 = int32_t(tl.tl_x), y0 = int32_t(tl.tl_y), x1 = int32_t(br.br_x), y1 = int32_t(br.br_y);
      if (!tl.window_offset_disable)
      {
        x0 += window_offset.window_x_offset;
        y0 += window_offset.window_y_offset;
        x1 += window_offset.window_x_offset;
        y1 += window_offset.window_y_offset;
      }
      x0 = std::clamp(x0, 0, int32_t(width));
      x1 = std::clamp(x1, 0, int32_t(width));
      y0 = std::clamp(y0, 0, int32_t(height));
      y1 = std::clamp(y1, 0, int32_t(height));
      out.scissor = {x0, y0, std::max(x0, x1), std::max(y0, y1)};
      return out;
    }

    // Pixel extent of small rectangle-list draws: D3D's clears and other
    // helpers set the viewport and scissor to the whole EDRAM surface, only
    // the vertices tell what is actually covered. Only for vertex shaders
    // that export the fetched x/y unchanged (w = 1); others (e.g. a depth
    // restore that turns 0..1 texture coordinates into the position) cover
    // the viewport and scissor.
    bool RectListExtent(const RegisterFile &r, const CompiledShader *vs, const DrawPacket &d, const Viewport &vp,
                        D3D12_RECT &out)
    {
      // Also a 4-vertex triangle strip whose corners make an axis-aligned
      // rectangle (a full-screen quad drawn the D3D way), xy only (w is 1).
      bool strip = d.prim_type == xenos::PrimitiveType::kTriangleStrip && d.index_count == 4;
      if ((d.prim_type != xenos::PrimitiveType::kRectangleList && !strip) || d.indexed || !d.index_count ||
          d.index_count > 12)
      {
        return false;
      }
      const auto &pf = vs->position_passthrough;
      if (!pf.valid || (strip && pf.format != xenos::VertexFormat::k_32_32_FLOAT))
      {
        return false;
      }
      float corner_x[4], corner_y[4];
      xenos::xe_gpu_vertex_fetch_t fetch;
      std::memcpy(&fetch, &r.v[0x4800 + pf.fetch_constant * 2], sizeof(fetch));
      uint32_t base = (fetch.address << 2) & 0x1FFFFFFF;
      uint32_t stride = pf.stride * 4;
      uint32_t first = r.v[XE_GPU_REG_VGT_INDX_OFFSET];
      if (!stride || !IsGuestPhysicalReadable(base + (first + 0) * stride, stride * d.index_count))
      {
        return false;
      }
      float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
      for (uint32_t i = 0; i < d.index_count; ++i)
      {
        const uint8_t *p = Mem()->TranslatePhysical(base + (first + i) * stride + uint32_t(pf.offset) * 4);
        float xy[2];
        for (uint32_t c = 0; c < 2; ++c)
        {
          uint32_t raw;
          std::memcpy(&raw, p + c * 4, 4);
          raw = SwapEndian(raw, fetch.endian);
          std::memcpy(&xy[c], &raw, 4);
        }
        float nx = xy[0] * vp.ndc_scale[0] + vp.ndc_offset[0];
        float ny = xy[1] * vp.ndc_scale[1] + vp.ndc_offset[1];
        float px = vp.viewport.TopLeftX + (nx * 0.5f + 0.5f) * vp.viewport.Width;
        float py = vp.viewport.TopLeftY + (0.5f - ny * 0.5f) * vp.viewport.Height;
        if (strip)
        {
          corner_x[i] = px;
          corner_y[i] = py;
        }
        x0 = std::min(x0, px);
        y0 = std::min(y0, py);
        x1 = std::max(x1, px);
        y1 = std::max(y1, py);
      }
      if (!(x0 <= x1 && y0 <= y1) || std::abs(x0) > 65536.0f || std::abs(y1) > 65536.0f)
      {
        return false;
      }
      if (strip)
      {
        // Every vertex a corner of the box, the first and last opposite and
        // the middle two the other diagonal (the strip's two triangles then
        // fill the box, not a bow tie).
        for (uint32_t i = 0; i < 4; ++i)
        {
          if ((corner_x[i] != x0 && corner_x[i] != x1) || (corner_y[i] != y0 && corner_y[i] != y1))
          {
            return false;
          }
        }
        auto opposite = [&](uint32_t a, uint32_t b) {
          return corner_x[a] != corner_x[b] && corner_y[a] != corner_y[b];
        };
        if (!opposite(0, 3) || !opposite(1, 2) || (corner_x[1] == corner_x[0] && corner_y[1] == corner_y[0]) ||
            (corner_x[1] == corner_x[3] && corner_y[1] == corner_y[3]))
        {
          return false;
        }
      }
      out.left = std::max<LONG>(LONG(std::floor(x0)), vp.scissor.left);
      out.top = std::max<LONG>(LONG(std::floor(y0)), vp.scissor.top);
      out.right = std::min<LONG>(LONG(std::ceil(x1)), vp.scissor.right);
      out.bottom = std::min<LONG>(LONG(std::ceil(y1)), vp.scissor.bottom);
      return true;
    }

    // Split-screen button pictures (keyboard_prompts.h): the draw's triangles
    // grouped by where they are - 0 in the keyboard player's part of the
    // screen, 1 in the gamepad player's, 2 across both (a prompt the players
    // share) - with each group's box in guest pixels. UE3's canvas gives each
    // player's HUD its own pixels and moves them to the player's part of the
    // screen with its vertex shader's matrix (SimpleElement: the vertex times
    // the 4 constants c0-c3), so the position is the vertex through those
    // constants, either way round, whichever lands every vertex in clip
    // space. False when it cannot be worked out (the draw is then clipped to
    // the two parts instead).
    struct PromptRun
    {
      uint32_t start = 0, count = 0; // vertices (or indices) of the host draw
      int part = 0;
      float box[4] = {};
    };
    bool PromptRuns(const CompiledShader *vs, const RegisterFile &r, bool index_fetch,
                    const std::vector<uint32_t> &indices, uint32_t draw_count, D3D12_PRIMITIVE_TOPOLOGY topology,
                    const Viewport &vp, const float keys[4], const float pad[4], std::vector<PromptRun> &runs)
    {
      runs.clear();
      if (index_fetch || topology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST || draw_count % 3 || draw_count > 6144)
      {
        return false;
      }
      const auto &bindings = vs->shader->vertex_bindings();
      if (bindings.empty() || bindings[0].attributes.empty())
      {
        return false;
      }
      const auto *position = &bindings[0].attributes[0].fetch_instr.attributes;
      for (const auto &a : bindings[0].attributes)
      {
        if (a.fetch_instr.attributes.offset < position->offset)
        {
          position = &a.fetch_instr.attributes;
        }
      }
      if (position->data_format != xenos::VertexFormat::k_32_32_FLOAT &&
          position->data_format != xenos::VertexFormat::k_32_32_32_FLOAT &&
          position->data_format != xenos::VertexFormat::k_32_32_32_32_FLOAT)
      {
        return false;
      }
      xenos::xe_gpu_vertex_fetch_t fetch;
      std::memcpy(&fetch, &r.v[0x4800 + bindings[0].fetch_constant * 2], sizeof(fetch));
      uint32_t base = (fetch.address << 2) & 0x1FFFFFFF;
      uint32_t stride = bindings[0].stride_words * 4;
      uint32_t first = r.v[XE_GPU_REG_VGT_INDX_OFFSET];
      if (!stride)
      {
        return false;
      }
      uint32_t words = position->data_format == xenos::VertexFormat::k_32_32_FLOAT       ? 2
                       : position->data_format == xenos::VertexFormat::k_32_32_32_FLOAT ? 3
                                                                                          : 4;
      // The matrix: VS constants c0-c3.
      float m[4][4];
      uint32_t const_base = std::min(r.v[XE_GPU_REG_SQ_VS_CONST] & 0x1FF, 252u);
      for (uint32_t row = 0; row < 4; ++row)
      {
        for (uint32_t c = 0; c < 4; ++c)
        {
          m[row][c] = std::bit_cast<float>(r.v[0x4000 + (const_base + row) * 4 + c]);
        }
      }
      static thread_local std::vector<float> xy;
      xy.resize(size_t(draw_count) * 2);
      bool fits[2] = {true, true}; // clip_i = dot(v, c_i) / sum_j v_j c_j[i]
      static thread_local std::vector<std::array<float, 4>> verts;
      verts.resize(draw_count);
      for (uint32_t i = 0; i < draw_count; ++i)
      {
        uint32_t vertex = first + (indices.empty() ? i : indices[i]);
        uint32_t at = base + vertex * stride + uint32_t(position->offset) * 4;
        if (!IsGuestPhysicalReadable(at, words * 4))
        {
          return false;
        }
        const uint8_t *src = Mem()->TranslatePhysical(at);
        std::array<float, 4> v = {0.0f, 0.0f, 0.0f, 1.0f};
        for (uint32_t c = 0; c < words; ++c)
        {
          uint32_t raw;
          std::memcpy(&raw, src + c * 4, 4);
          raw = SwapEndian(raw, fetch.endian);
          std::memcpy(&v[c], &raw, 4);
          if (!(std::abs(v[c]) < 65536.0f))
          {
            return false;
          }
        }
        verts[i] = v;
        for (int way = 0; way < 2; ++way)
        {
          float clip[4];
          for (uint32_t k = 0; k < 4; ++k)
          {
            clip[k] = way == 0 ? v[0] * m[k][0] + v[1] * m[k][1] + v[2] * m[k][2] + v[3] * m[k][3]
                               : v[0] * m[0][k] + v[1] * m[1][k] + v[2] * m[2][k] + v[3] * m[3][k];
          }
          fits[way] = fits[way] && clip[3] > 1e-4f && std::abs(clip[0] / clip[3]) <= 1.25f &&
                      std::abs(clip[1] / clip[3]) <= 1.25f;
        }
      }
      if (!fits[0] && !fits[1])
      {
        return false;
      }
      int way = fits[0] ? 0 : 1;
      for (uint32_t i = 0; i < draw_count; ++i)
      {
        const auto &v = verts[i];
        float clip[4];
        for (uint32_t k = 0; k < 4; ++k)
        {
          clip[k] = way == 0 ? v[0] * m[k][0] + v[1] * m[k][1] + v[2] * m[k][2] + v[3] * m[k][3]
                             : v[0] * m[0][k] + v[1] * m[1][k] + v[2] * m[2][k] + v[3] * m[3][k];
        }
        xy[i * 2] = clip[0] / clip[3];
        xy[i * 2 + 1] = clip[1] / clip[3];
      }
      bool ndc = true;
      float gw = float(ao2::aspect::GuestWidth()), gh = float(ao2::aspect::GuestHeight());
      auto inside = [](const float b[4], const float part[4], float gw, float gh) {
        return b[0] >= part[0] * gw - 2 && b[2] <= part[2] * gw + 2 && b[1] >= part[1] * gh - 2 &&
               b[3] <= part[3] * gh + 2;
      };
      for (uint32_t t = 0; t < draw_count; t += 3)
      {
        float b[4] = {FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (uint32_t k = 0; k < 3; ++k)
        {
          float x = xy[(t + k) * 2], y = xy[(t + k) * 2 + 1];
          if (ndc)
          {
            float nx = x * vp.ndc_scale[0] + vp.ndc_offset[0], ny = y * vp.ndc_scale[1] + vp.ndc_offset[1];
            x = vp.viewport.TopLeftX + (nx * 0.5f + 0.5f) * vp.viewport.Width;
            y = vp.viewport.TopLeftY + (0.5f - ny * 0.5f) * vp.viewport.Height;
          }
          b[0] = std::min(b[0], x);
          b[1] = std::min(b[1], y);
          b[2] = std::max(b[2], x);
          b[3] = std::max(b[3], y);
        }
        int part = inside(b, keys, gw, gh) ? 0 : inside(b, pad, gw, gh) ? 1 : 2;
        if (!runs.empty() && runs.back().part == part && runs.back().start + runs.back().count == t)
        {
          PromptRun &run = runs.back();
          run.count += 3;
          run.box[0] = std::min(run.box[0], b[0]);
          run.box[1] = std::min(run.box[1], b[1]);
          run.box[2] = std::max(run.box[2], b[2]);
          run.box[3] = std::max(run.box[3], b[3]);
        }
        else
        {
          PromptRun run;
          run.start = t;
          run.count = 3;
          run.part = part;
          std::memcpy(run.box, b, sizeof(b));
          runs.push_back(run);
        }
      }
      // A shared prompt is a quad: its two triangles are one group already.
      if (REXCVAR_GET(ao2_prompts_debug))
      {
        static uint32_t logged = 0;
        if (logged < 4000)
        {
          ++logged;
          NativeLog("prompts: frame %llu way %d c0 %g %g %g %g c1 %g %g %g %g c3 %g %g %g %g v0 %g,%g vp %g,%g %gx%g\n",
                    (unsigned long long)g.frame_number, way, m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1],
                    m[1][2], m[1][3], m[3][0], m[3][1], m[3][2], m[3][3], verts[0][0], verts[0][1],
                    vp.viewport.TopLeftX, vp.viewport.TopLeftY, vp.viewport.Width, vp.viewport.Height);
          for (const PromptRun &run : runs)
          {
            NativeLog("  run %u+%u part %d box %.0f,%.0f-%.0f,%.0f\n", run.start, run.count, run.part, run.box[0],
                      run.box[1], run.box[2], run.box[3]);
          }
        }
      }
      return true;
    }

    // Values written by D3D's clear rectangles (VS A6266DE8: position xyz at
    // dword 0, color at dword 3 of each vertex; PS 5C92B939 outputs the
    // color, or no PS for depth/stencil clears), when all vertices agree.
    constexpr uint64_t kD3DClearVS = 0xA6266DE8A68D98CCull, kD3DClearPS = 0x5C92B939D26FC5C0ull;

    struct ClearDrawValues
    {
      bool vertices_ok = false, depth_ok = false, color_ok = false;
      float z = 0.0f;
      float color[4] = {};
      float Depth(const Viewport &vp) const
      {
        float ndc = z * vp.ndc_scale[2] + vp.ndc_offset[2];
        float lo = std::min(vp.viewport.MinDepth, vp.viewport.MaxDepth), hi = std::max(vp.viewport.MinDepth,
                                                                                     vp.viewport.MaxDepth);
        return std::clamp(vp.viewport.MinDepth + ndc * (vp.viewport.MaxDepth - vp.viewport.MinDepth), lo, hi);
      }
    };

    void GetClearDrawValues(const RegisterFile &r, const CompiledShader *vs, const CompiledShader *ps,
                            const DrawPacket &d, const Viewport &vp, ClearDrawValues &out)
    {
      out = {};
      const auto &pf = vs->position_passthrough;
      if (vs->shader->ucode_data_hash() != kD3DClearVS || !pf.valid || pf.stride < 7 ||
          (ps && ps->shader->ucode_data_hash() != kD3DClearPS) || d.indexed || !d.index_count || d.index_count > 12)
      {
        return;
      }
      xenos::xe_gpu_vertex_fetch_t fetch;
      std::memcpy(&fetch, &r.v[0x4800 + pf.fetch_constant * 2], sizeof(fetch));
      uint32_t base = (fetch.address << 2) & 0x1FFFFFFF;
      uint32_t stride = pf.stride * 4;
      uint32_t first = r.v[XE_GPU_REG_VGT_INDX_OFFSET];
      if (!IsGuestPhysicalReadable(base + first * stride, stride * d.index_count))
      {
        return;
      }
      uint32_t words[12][7];
      for (uint32_t i = 0; i < d.index_count; ++i)
      {
        const uint8_t *p = Mem()->TranslatePhysical(base + (first + i) * stride);
        for (uint32_t k = 0; k < 7; ++k)
        {
          uint32_t raw;
          std::memcpy(&raw, p + k * 4, 4);
          words[i][k] = SwapEndian(raw, fetch.endian);
        }
      }
      out.vertices_ok = true;
      out.depth_ok = out.color_ok = true;
      for (uint32_t i = 1; i < d.index_count; ++i)
      {
        out.depth_ok &= words[i][2] == words[0][2];
        for (uint32_t k = 3; k < 7; ++k)
        {
          out.color_ok &= words[i][k] == words[0][k];
        }
      }
      std::memcpy(&out.z, &words[0][2], 4);
      std::memcpy(out.color, &words[0][3], 16);
      out.color_ok &= ps != nullptr;
    }

    // ------------------------------------------------------------ Indices

    // Primitive types the host draws as is: vertex k of the draw is guest
    // index k (fetched by the vertex shader for indexed draws).
    D3D12_PRIMITIVE_TOPOLOGY DirectTopology(const DrawPacket &d, const RegisterFile &r, bool &rect)
    {
      rect = false;
      bool reset = d.indexed && r.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena;
      switch (d.prim_type)
      {
      case xenos::PrimitiveType::kPointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      case xenos::PrimitiveType::kLineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      case xenos::PrimitiveType::kTriangleList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      case xenos::PrimitiveType::kRectangleList: rect = true; return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      case xenos::PrimitiveType::kLineStrip: return reset ? D3D_PRIMITIVE_TOPOLOGY_UNDEFINED : D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
      case xenos::PrimitiveType::kTriangleStrip:
        return reset ? D3D_PRIMITIVE_TOPOLOGY_UNDEFINED : D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      default: return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
      }
    }

    // Builds the host index list (uint32) for the draw, converting primitive
    // types D3D12 lacks. Returns the host topology.
    D3D12_PRIMITIVE_TOPOLOGY BuildIndices(const DrawPacket &d, std::vector<uint32_t> &out, bool &rect)
    {
      rect = false;
      uint32_t n = d.index_count;
      std::vector<uint32_t> src(n);
      if (d.indexed)
      {
        const uint8_t *p = Mem()->TranslatePhysical(d.index_base);
        bool is32 = d.index_format == xenos::IndexFormat::kInt32;
        uint32_t available = d.index_words * (is32 ? 1u : 2u);
        n = std::min(n, available ? available : n);
        src.resize(n);
        for (uint32_t i = 0; i < n; ++i)
        {
          if (is32)
          {
            uint32_t v;
            std::memcpy(&v, p + i * 4, 4);
            switch (d.index_endian)
            {
            case xenos::Endian::k8in32: v = __builtin_bswap32(v); break;
            case xenos::Endian::k16in32: v = (v >> 16) | (v << 16); break;
            case xenos::Endian::k8in16: v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); break;
            default: break;
            }
            src[i] = v;
          }
          else
          {
            uint16_t v;
            std::memcpy(&v, p + i * 2, 2);
            if (d.index_endian == xenos::Endian::k8in16 || d.index_endian == xenos::Endian::k8in32)
            {
              v = uint16_t((v >> 8) | (v << 8));
            }
            src[i] = v;
          }
        }
      }
      else
      {
        for (uint32_t i = 0; i < n; ++i)
        {
          src[i] = i;
        }
      }
      out.clear();
      switch (d.prim_type)
      {
      case xenos::PrimitiveType::kPointList:
        out = std::move(src);
        return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      case xenos::PrimitiveType::kLineList:
        out = std::move(src);
        return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      case xenos::PrimitiveType::kLineStrip:
        out = std::move(src);
        return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
      case xenos::PrimitiveType::kTriangleList:
        out = std::move(src);
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      case xenos::PrimitiveType::kTriangleStrip:
        out = std::move(src);
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      case xenos::PrimitiveType::kTriangleFan:
      case xenos::PrimitiveType::kPolygon:
        for (uint32_t i = 2; i < n; ++i)
        {
          out.insert(out.end(), {src[0], src[i - 1], src[i]});
        }
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      case xenos::PrimitiveType::kQuadList:
        for (uint32_t i = 0; i + 3 < n; i += 4)
        {
          out.insert(out.end(), {src[i], src[i + 1], src[i + 2], src[i], src[i + 2], src[i + 3]});
        }
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      case xenos::PrimitiveType::kQuadStrip:
        for (uint32_t i = 0; i + 3 < n; i += 2)
        {
          out.insert(out.end(), {src[i], src[i + 1], src[i + 3], src[i], src[i + 3], src[i + 2]});
        }
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      case xenos::PrimitiveType::kRectangleList:
        rect = true;
        out = std::move(src);
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      default:
        return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
      }
    }

    D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(D3D12_PRIMITIVE_TOPOLOGY t)
    {
      switch (t)
      {
      case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
      case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      }
    }

    // ------------------------------------------------------------ Textures

    // Submits the open command list, waits for the GPU and reopens the list on
    // the same frame resources (used when per-frame rings run out mid-frame).
    void FlushAndWait()
    {
      if (g.list_open)
      {
        if (CloseList())
        {
          ID3D12CommandList *lists[] = {g.list.Get()};
          g.queue->ExecuteCommandLists(1, lists);
        }
        g.list_open = false;
      }
      WaitIdle();
      CurrentFrame().fence_value = 0;
      ++g.flushes;
      BeginFrameIfNeeded();
    }

    uint64_t TextureKey(const GuestTextureDesc &d)
    {
      struct
      {
        uint32_t base_page, mip_page, width, height, depth, min_level, max_level, misc;
      } k = {d.base_page,
             d.mip_page,
             d.width,
             d.height,
             d.depth_or_array_size,
             d.min_level,
             d.max_level,
             uint32_t(d.format) | (uint32_t(d.fetch.endianness) << 6) | (uint32_t(d.fetch.tiled) << 8) |
                 (uint32_t(d.fetch.packed_mips) << 9) | (uint32_t(d.dimension) << 10) |
                 (uint32_t(d.fetch.pitch) << 12)};
      return XXH3_64bits(&k, sizeof(k));
    }

    bool CreateTextureResource(CachedTexture &t)
    {
      const GuestTextureDesc &d = t.desc;
      const HostTextureFormat &host = *t.host;
      D3D12_RESOURCE_DESC rd = {};
      bool is_3d = d.dimension == xenos::DataDimension::k3D;
      rd.Dimension = is_3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      uint32_t align = host.host_block;
      rd.Width = (HostLevelWidth(d, 0) + align - 1) / align * align;
      rd.Height = (HostLevelHeight(d, 0) + align - 1) / align * align;
      rd.DepthOrArraySize = uint16_t(is_3d ? std::max(d.depth_or_array_size >> d.min_level, 1u)
                                           : d.depth_or_array_size);
      t.mip_levels = d.max_level - d.min_level + 1;
      rd.MipLevels = uint16_t(t.mip_levels);
      rd.Format = host.resource;
      rd.SampleDesc.Count = 1;
      D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
      t.state = D3D12_RESOURCE_STATE_COPY_DEST;
      ++g.resources_created;
      HRESULT hr = g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, t.state, nullptr,
                                                     IID_PPV_ARGS(&t.resource));
      if (FAILED(hr))
      {
        NativeLog("texture creation failed (0x%08X): fmt %u %ux%ux%u levels %u\n", unsigned(hr),
                  unsigned(d.format), unsigned(rd.Width), rd.Height, rd.DepthOrArraySize, t.mip_levels);
        return false;
      }
      t.array_size = is_3d ? 1 : d.depth_or_array_size;
      t.depth = is_3d ? rd.DepthOrArraySize : 1;
      return true;
    }

    // A texture whose picture is ours (keyboard_prompts' button page, a font
    // page with Cyrillic letters): RGBA8 `width` x `height`, straight alpha,
    // with box-filtered mips. Sampled through the guest's fetch constant like
    // the texture it replaces (normalized coordinates, so the size may differ).
    bool CreateReplacedTexture(CachedTexture &t, const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height)
    {
      t.host = &GetHostTextureFormat(xenos::TextureFormat::k_8_8_8_8);
      std::vector<std::vector<uint8_t>> levels{rgba};
      std::vector<std::pair<uint32_t, uint32_t>> dims{{width, height}};
      while (dims.back().first > 1 || dims.back().second > 1)
      {
        const std::vector<uint8_t> &src = levels.back();
        auto [sw, sh] = dims.back();
        uint32_t dw = std::max(sw / 2, 1u), dh = std::max(sh / 2, 1u);
        std::vector<uint8_t> dst(size_t(dw) * dh * 4);
        for (uint32_t y = 0; y < dh; ++y)
        {
          for (uint32_t x = 0; x < dw; ++x)
          {
            // Premultiplied average of the four texels.
            float acc[4] = {};
            for (uint32_t k = 0; k < 4; ++k)
            {
              uint32_t sx = std::min(x * 2 + k % 2, sw - 1), sy = std::min(y * 2 + k / 2, sh - 1);
              const uint8_t *p = &src[(size_t(sy) * sw + sx) * 4];
              float a = p[3] / 255.0f;
              acc[0] += p[0] * a;
              acc[1] += p[1] * a;
              acc[2] += p[2] * a;
              acc[3] += a;
            }
            uint8_t *o = &dst[(size_t(y) * dw + x) * 4];
            for (uint32_t c = 0; c < 3; ++c)
            {
              o[c] = acc[3] > 0.0f ? uint8_t(std::min(acc[c] / acc[3], 255.0f) + 0.5f) : 0;
            }
            o[3] = uint8_t(acc[3] / 4.0f * 255.0f + 0.5f);
          }
        }
        levels.push_back(std::move(dst));
        dims.push_back({dw, dh});
      }

      D3D12_RESOURCE_DESC rd = {};
      rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      rd.Width = width;
      rd.Height = height;
      rd.DepthOrArraySize = 1;
      rd.MipLevels = uint16_t(levels.size());
      rd.Format = t.host->resource;
      rd.SampleDesc.Count = 1;
      D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
      t.state = D3D12_RESOURCE_STATE_COPY_DEST;
      ++g.resources_created;
      if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, t.state, nullptr,
                                                   IID_PPV_ARGS(&t.resource))))
      {
        return false;
      }
      t.mip_levels = uint32_t(levels.size());
      t.array_size = 1;
      t.depth = 1;

      uint32_t subresources = t.mip_levels;
      g.footprints.resize(subresources);
      g.footprint_rows.resize(subresources);
      g.footprint_row_bytes.resize(subresources);
      UINT64 total = 0;
      g.device->GetCopyableFootprints(&rd, 0, subresources, 0, g.footprints.data(), g.footprint_rows.data(),
                                      g.footprint_row_bytes.data(), &total);
      void *mapped = nullptr;
      D3D12_GPU_VIRTUAL_ADDRESS gpu = Upload(nullptr, total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &mapped);
      if (!gpu)
      {
        FlushAndWait();
        gpu = Upload(nullptr, total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &mapped);
      }
      if (!gpu)
      {
        return false;
      }
      uint64_t base_offset = gpu - CurrentFrame().upload->GetGPUVirtualAddress();
      Transition(t.resource.Get(), t.state, D3D12_RESOURCE_STATE_COPY_DEST);
      for (uint32_t level = 0; level < t.mip_levels; ++level)
      {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = g.footprints[level];
        auto [level_w, level_h] = dims[level];
        for (uint32_t y = 0; y < level_h; ++y)
        {
          std::memcpy(static_cast<uint8_t *>(mapped) + fp.Offset + size_t(y) * fp.Footprint.RowPitch,
                      levels[level].data() + size_t(y) * level_w * 4, size_t(level_w) * 4);
        }
        fp.Offset += base_offset;
        D3D12_TEXTURE_COPY_LOCATION dl = {t.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dl.SubresourceIndex = level;
        D3D12_TEXTURE_COPY_LOCATION sl = {CurrentFrame().upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        sl.PlacedFootprint = fp;
        GpuMark(kGpuUpload);
        g.list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
      }
      Transition(t.resource.Get(), t.state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
      t.replaced = true;
      t.loaded = true;
      return true;
    }

    // Our picture for a guest texture: keyboard_prompts' button page or a font
    // page with Cyrillic letters (cyrillic_fonts). False if the texture is the
    // guest's own; with `rgba` null only says whether it is replaced.
    bool ReplacementPicture(const GuestTextureDesc &desc, std::vector<uint8_t> *rgba, uint32_t *width,
                            uint32_t *height, bool *prompt = nullptr)
    {
      const uint8_t *data = nullptr;
      if (prompt)
        *prompt = true;
      if (desc.width == 256 && desc.height == 256 && desc.base_size >= 256 * 256 &&
          IsGuestPhysicalReadable(desc.base_address, 256 * 256))
      {
        data = rex::Runtime::instance()->memory()->TranslatePhysical(desc.base_address);
        if (ao2::prompts::IsButtonPage(uint32_t(desc.format), desc.width, desc.height, data, desc.base_size))
        {
          if (!rgba)
            return true;
          uint32_t size = 0;
          if (!ao2::prompts::BuildButtonPage(data, *rgba, size))
            return false;
          *width = *height = size;
          return true;
        }
      }
      uint32_t button_bytes = ao2::prompts::ButtonTextureBytes(uint32_t(desc.format), desc.width, desc.height);
      if (button_bytes && desc.base_size >= button_bytes && IsGuestPhysicalReadable(desc.base_address, button_bytes))
      {
        data = rex::Runtime::instance()->memory()->TranslatePhysical(desc.base_address);
        int button =
            ao2::prompts::ButtonTextureOf(uint32_t(desc.format), desc.width, desc.height, data, button_bytes);
        if (button >= 0)
        {
          return !rgba || ao2::prompts::BuildButtonTexture(button, *rgba, *width, *height);
        }
      }
      if (prompt)
        *prompt = false;
      uint32_t bytes = ao2::cyrillic::PageBytes(uint32_t(desc.format), desc.width, desc.height);
      if (!bytes || desc.base_size < bytes || !IsGuestPhysicalReadable(desc.base_address, bytes))
        return false;
      data = rex::Runtime::instance()->memory()->TranslatePhysical(desc.base_address);
      int page = ao2::cyrillic::PageOf(uint32_t(desc.format), desc.width, desc.height, data, bytes);
      if (page < 0)
        return false;
      return !rgba || ao2::cyrillic::BuildPage(page, *rgba, *width, *height);
    }

    // Converts the guest data into upload memory and records the copies.
    bool UploadTexture(CachedTexture &t)
    {
      D3D12_RESOURCE_DESC rd = t.resource->GetDesc();
      uint32_t subresources = t.mip_levels * t.array_size;
      g.footprints.resize(subresources);
      g.footprint_rows.resize(subresources);
      g.footprint_row_bytes.resize(subresources);
      UINT64 total = 0;
      g.device->GetCopyableFootprints(&rd, 0, subresources, 0, g.footprints.data(), g.footprint_rows.data(),
                                      g.footprint_row_bytes.data(), &total);
      void *mapped = nullptr;
      D3D12_GPU_VIRTUAL_ADDRESS gpu = Upload(nullptr, total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &mapped);
      if (!gpu && total <= kUploadSize / 2)
      {
        FlushAndWait();
        gpu = Upload(nullptr, total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &mapped);
      }
      if (!gpu)
      {
        return false;
      }
      uint64_t base_offset = gpu - CurrentFrame().upload->GetGPUVirtualAddress();
      bool padded = t.host->host_block > 1;
      Transition(t.resource.Get(), t.state, D3D12_RESOURCE_STATE_COPY_DEST);
      for (uint32_t slice = 0; slice < t.array_size; ++slice)
      {
        for (uint32_t level = 0; level < t.mip_levels; ++level)
        {
          uint32_t sub = level + slice * t.mip_levels;
          D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = g.footprints[sub];
          uint8_t *dst = static_cast<uint8_t *>(mapped) + fp.Offset;
          uint32_t rows = g.footprint_rows[sub];
          uint32_t slice_pitch = fp.Footprint.RowPitch * rows;
          if (padded)
          {
            std::memset(dst, 0, size_t(slice_pitch) * fp.Footprint.Depth);
          }
          LoadGuestSubresource(t.desc, level, slice, dst, fp.Footprint.RowPitch, slice_pitch, rows,
                               uint32_t(g.footprint_row_bytes[sub]));
          fp.Offset += base_offset;
          D3D12_TEXTURE_COPY_LOCATION dl = {t.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
          dl.SubresourceIndex = sub;
          D3D12_TEXTURE_COPY_LOCATION sl = {CurrentFrame().upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
          sl.PlacedFootprint = fp;
          GpuMark(kGpuUpload);
          Crumb("texture upload", uint64_t(t.resource.Get()), sub, fp.Footprint.Width, fp.Footprint.Height);
          g.list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        }
      }
      Transition(t.resource.Get(), t.state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
      g.texture_upload_bytes += total;
      ++g.texture_uploads;
      return true;
    }

    // Returns the host texture for a fetch constant, (re)loaded if the guest
    // memory changed since it was last checked (once per frame).
    CachedTexture *GetTextureSlow(const xenos::xe_gpu_texture_fetch_t &fetch);

    // Split-screen: the guest's own picture of a replaced button texture,
    // loaded next to ours (and again when the guest's data changes).
    CachedTexture *GuestPicture(CachedTexture &t)
    {
      if (!t.guest)
      {
        auto g_tex = std::make_unique<CachedTexture>();
        g_tex->desc = t.desc;
        g_tex->host = &GetHostTextureFormat(t.desc.format);
        g_tex->cube = t.cube;
        if (!CreateTextureResource(*g_tex))
        {
          return nullptr;
        }
        g_tex->epoch = t.epoch - 1;
        t.guest = std::move(g_tex);
      }
      CachedTexture &o = *t.guest;
      o.desc.fetch = t.desc.fetch;
      if (!o.loaded || o.epoch != t.epoch)
      {
        const GuestTextureDesc &d = o.desc;
        if (!IsGuestPhysicalReadable(d.base_address, d.base_size) || !IsGuestPhysicalReadable(d.mip_address, d.mip_size))
        {
          return o.loaded ? &o : nullptr;
        }
        o.loaded = UploadTexture(o);
        o.epoch = t.epoch;
      }
      return o.loaded ? &o : nullptr;
    }

    CachedTexture *GetTexture(const xenos::xe_gpu_texture_fetch_t &fetch)
    {
      // Same fetch constant already described and validated this frame.
      uint32_t words[6];
      std::memcpy(words, &fetch, sizeof(words));
      uint64_t fetch_key = XXH3_64bits(words, sizeof(words));
      auto cached = g.fetch_cache.find(fetch_key);
      if (cached != g.fetch_cache.end() && cached->second.frame == g.frame_number &&
          !std::memcmp(cached->second.words, words, sizeof(words)))
      {
        return cached->second.texture;
      }
      CachedTexture *result = GetTextureSlow(fetch);
      if (result && result->validated_frame == g.frame_number)
      {
        if (g.fetch_cache.size() > 16384)
        {
          g.fetch_cache.clear();
        }
        State::FetchCacheEntry &e = g.fetch_cache[fetch_key];
        std::memcpy(e.words, words, sizeof(words));
        e.texture = result;
        e.frame = g.frame_number;
      }
      return result;
    }

    CachedTexture *GetTextureSlow(const xenos::xe_gpu_texture_fetch_t &fetch)
    {
      GuestTextureDesc desc;
      if (!DescribeGuestTexture(fetch, desc))
      {
        return nullptr;
      }
      uint64_t key = TextureKey(desc);
      auto &slot = g.textures[key];
      if (!slot)
      {
        slot = std::make_unique<CachedTexture>();
        slot->desc = desc;
        slot->host = &GetHostTextureFormat(desc.format);
        slot->cube = desc.dimension == xenos::DataDimension::kCube;
        slot->prompts_generation = ao2::prompts::Generation();
        std::vector<uint8_t> page;
        uint32_t page_w = 0, page_h = 0;
        if (ReplacementPicture(desc, &page, &page_w, &page_h, &slot->prompt))
        {
          slot->epoch = g.write_epoch.load(std::memory_order_relaxed);
          slot->checked_epoch = slot->epoch;
          WatchGuestRange(desc.base_address, desc.base_size);
          if (!CreateReplacedTexture(*slot, page, page_w, page_h))
          {
            slot->failed = true;
          }
          NativeLog("texture %08X (%ux%u) replaced by our picture (%ux%u)\n", desc.base_address, desc.width,
                    desc.height, page_w, page_h);
        }
        else if (!CreateTextureResource(*slot))
        {
          slot->failed = true;
        }
      }
      CachedTexture &t = *slot;
      if (t.failed)
      {
        return nullptr;
      }
      t.desc.fetch = fetch;
      // The button pictures switched between the keys and the controller's
      // buttons: a texture that can be one is looked at again.
      uint32_t prompts_generation = ao2::prompts::Generation();
      bool prompts_switched = t.prompts_generation != prompts_generation;
      if (t.replaced)
      {
        // Ours for as long as the guest's page is there: memory reused for
        // another texture of the same shape gets that texture.
        if (t.validated_frame != g.frame_number)
        {
          t.validated_frame = g.frame_number;
          uint32_t now = g.write_epoch.load(std::memory_order_relaxed);
          if ((t.checked_epoch != now && WrittenSince(desc.base_address, desc.base_size, t.epoch)) ||
              prompts_switched)
          {
            t.prompts_generation = prompts_generation;
            t.epoch = now;
            WatchGuestRange(desc.base_address, desc.base_size);
            std::vector<uint8_t> picture;
            uint32_t picture_w = 0, picture_h = 0;
            if (prompts_switched && t.prompt &&
                ReplacementPicture(desc, &picture, &picture_w, &picture_h, &t.prompt))
            {
              // Still ours, but the picture may be another (the split-screen page).
              DeferRelease(std::move(t.resource));
              if (!CreateReplacedTexture(t, picture, picture_w, picture_h))
              {
                t.failed = true;
                return nullptr;
              }
            }
            else if (!ReplacementPicture(desc, nullptr, nullptr, nullptr))
            {
              // Back to the guest's texture, in place (the fetch cache points at it).
              DeferRelease(std::move(t.resource));
              if (t.guest)
              {
                DeferRelease(std::move(t.guest->resource));
                t.guest.reset();
              }
              t.replaced = t.loaded = t.prompt = false;
              t.host = &GetHostTextureFormat(desc.format);
              t.validated_frame = UINT64_MAX;
              if (!CreateTextureResource(t))
              {
                t.failed = true;
                return nullptr;
              }
              return GetTextureSlow(fetch);
            }
          }
          if (t.replaced)
          {
            t.checked_epoch = now;
          }
        }
        if (t.replaced)
        {
          return &t;
        }
      }
      if (t.validated_frame != g.frame_number)
      {
        // Once per frame: reload if the CPU wrote to the texture's pages.
        t.validated_frame = g.frame_number;
        uint32_t now = g.write_epoch.load(std::memory_order_relaxed);
        bool reload = !t.loaded;
        if (prompts_switched)
        {
          t.prompts_generation = prompts_generation;
          reload = reload || ao2::prompts::MayBeButton(uint32_t(desc.format), desc.width, desc.height);
        }
        if (!reload && t.checked_epoch != now)
        {
          uint64_t check_start = __rdtsc();
          reload = WrittenSince(desc.base_address, desc.base_size, t.epoch) ||
                   WrittenSince(desc.mip_address, desc.mip_size, t.epoch);
          g.time_hash_ns += TscElapsedNs(check_start);
          t.checked_epoch = now;
        }
        if (reload)
        {
          if (!IsGuestPhysicalReadable(desc.base_address, desc.base_size) ||
              !IsGuestPhysicalReadable(desc.mip_address, desc.mip_size))
          {
            return t.loaded ? &t : nullptr;
          }
          // Watch before reading: a write during the upload marks it outdated again.
          t.epoch = g.write_epoch.load(std::memory_order_relaxed);
          WatchGuestRange(desc.base_address, desc.base_size);
          WatchGuestRange(desc.mip_address, desc.mip_size);
          // Data that arrived after the texture was first seen can be one we
          // replace (a button streamed into memory that held something else).
          std::vector<uint8_t> picture;
          uint32_t picture_w = 0, picture_h = 0;
          if (ReplacementPicture(desc, &picture, &picture_w, &picture_h, &t.prompt))
          {
            DeferRelease(std::move(t.resource));
            t.checked_epoch = t.epoch;
            if (!CreateReplacedTexture(t, picture, picture_w, picture_h))
            {
              t.failed = true;
              return nullptr;
            }
            NativeLog("texture %08X (%ux%u) replaced by our picture (%ux%u)\n", desc.base_address, desc.width,
                      desc.height, picture_w, picture_h);
            return &t;
          }
          auto upload_start = std::chrono::steady_clock::now();
          t.loaded = UploadTexture(t);
          uint64_t upload_ns = Elapsed(upload_start);
          g.time_upload_ns += upload_ns;
          g.frame_upload_ns += upload_ns;
          t.checked_epoch = t.epoch;
          if (!t.loaded)
          {
            t.validated_frame = UINT64_MAX;
          }
        }
      }
      return t.loaded ? &t : nullptr;
    }

    // A resolve destination sampled as a texture, if the guest memory there
    // was not rewritten by the CPU since the resolve.
    HostTexture *GetResolvedTexture(uint32_t address)
    {
      auto it = g.resolved.find(address);
      if (it == g.resolved.end() || !it->second)
      {
        return nullptr;
      }
      HostTexture &t = *it->second;
      // Checked again only after some CPU write happened anywhere.
      uint32_t now = g.write_epoch.load(std::memory_order_relaxed);
      if (t.checked_epoch == now)
      {
        return &t;
      }
      t.checked_epoch = now;
      if (WrittenSince(address, t.guest_size, t.epoch))
      {
        if (g.resolved_dropped++ < 32)
        {
          uint32_t page = address >> kPageShift;
          while (g.page_epoch[page].load(std::memory_order_relaxed) <= t.epoch)
          {
            ++page;
          }
          NativeLog("resolved %08X (%u bytes) dropped: the CPU wrote page %08X (frame %llu)\n", address, t.guest_size,
                    page << kPageShift, (unsigned long long)g.frame_number);
        }
        if (!g.lent.empty())
        {
          ReturnLent(nullptr, address);
        }
        DeferRelease(std::move(t.resource));
        DeferRelease(std::move(t.lowres));
        g.resolved.erase(it);
        return nullptr;
      }
      return &t;
    }

    bool DimensionMatches(xenos::FetchOpDimension shader, xenos::DataDimension texture, bool stacked)
    {
      switch (shader)
      {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return (texture == xenos::DataDimension::k1D || texture == xenos::DataDimension::k2DOrStacked) &&
               !stacked;
      case xenos::FetchOpDimension::k3DOrStacked:
        return texture == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::kCube:
        return texture == xenos::DataDimension::kCube;
      }
      return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE NullView(xenos::FetchOpDimension dimension)
    {
      switch (dimension)
      {
      case xenos::FetchOpDimension::k3DOrStacked: return g.null_srv_3d;
      case xenos::FetchOpDimension::kCube: return g.null_srv_cube;
      default: return g.null_srv_2d;
      }
    }

    // Textures of one shader resolved for a draw (phase 1, may flush the
    // command list while uploading), written to a descriptor table later.
    struct TextureTable
    {
      ID3D12Resource *resources[kMaxTextureBindings] = {};
      D3D12_SHADER_RESOURCE_VIEW_DESC views[kMaxTextureBindings] = {};
      D3D12_CPU_DESCRIPTOR_HANDLE nulls[kMaxTextureBindings] = {};
      bool any = false;
      // Bindings the shader declares: the only slots it reads (and the only
      // ones written to the descriptor heap).
      uint32_t used = 0;
      // Split-screen: slots with a button picture, and the guest's own
      // picture for them (the gamepad player's part of the screen).
      uint32_t dual = 0;
      ID3D12Resource *guest_resources[kMaxTextureBindings] = {};
      D3D12_SHADER_RESOURCE_VIEW_DESC guest_views[kMaxTextureBindings] = {};
    };

    bool EnsureLowres(HostTexture &t);

    // `lowres`: the draw renders into a small target (see SmallTarget).
    void PrepareTextures(const CompiledShader *s, const RegisterFile &r, TextureTable &table,
                         uint32_t tex_info[16][4], float tex_size[16][4], uint32_t &tex_info_used, bool lowres)
    {
      if (!s)
      {
        return;
      }
      uint32_t &seen = table.used;
      for (const auto &b : s->shader->texture_bindings())
      {
        uint32_t index = uint32_t(b.binding_index);
        if (index >= kMaxTextureBindings || (seen & (1u << index)))
        {
          continue;
        }
        // getGradients names a fetch constant but reads no texture (ddx/ddy
        // of its operand): whatever the constant was left pointing at must
        // not be loaded. The shadow depth shaders leave the scene depth
        // texture there, 3.7 MB converted from guest memory on every write.
        if (b.fetch_instr.opcode == ucode::FetchOpcode::kGetTextureGradients)
        {
          continue;
        }
        seen |= 1u << index;
        table.any = true;
        auto fetch = r.Get<xenos::xe_gpu_texture_fetch_t>(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                                          b.fetch_constant * 6);
        auto dimension = b.fetch_instr.dimension;
        uint32_t signs = texture_util::SwizzleSigns(fetch);
        float exp_scale = std::ldexp(1.0f, fetch.exp_adjust);
        // Guest texture size / host resource size (resolved textures are pitch x height).
        float uv_scale[2] = {1.0f, 1.0f};
        D3D12_SHADER_RESOURCE_VIEW_DESC &sd = table.views[index];
        if (fetch.type == xenos::FetchConstantType::kTexture)
        {
          HostTexture *resolved = (dimension == xenos::FetchOpDimension::k2D)
                                      ? GetResolvedTexture((fetch.base_address & 0x1FFFF) << 12)
                                      : nullptr;
          if (resolved && resolved->resource)
          {
            PostBeforeSample(*resolved, (fetch.base_address & 0x1FFFF) << 12);
            bool use_snorm = texture_util::IsAnySignSigned(uint8_t(signs)) && resolved->snorm != DXGI_FORMAT_UNKNOWN;
            bool guest_res = (lowres || resolved->small_source) && !use_snorm && !resolved->from_depth && EnsureLowres(*resolved);
            ID3D12Resource *resource = guest_res ? resolved->lowres.Get() : resolved->resource.Get();
            D3D12_RESOURCE_STATES &state = guest_res ? resolved->lowres_state : resolved->state;
            // Host texels per guest texel (in quarters) and the part of the
            // host resource the guest texture's texels take.
            uint32_t texel4 = guest_res ? 4u : resolved->scale4;
            uv_scale[0] = std::min(float((fetch.size_2d.width + 1) * texel4) /
                                       float(ScaleSize(resolved->width, texel4) * 4), 1.0f);
            uv_scale[1] = std::min(float((fetch.size_2d.height + 1) * texel4) /
                                       float(ScaleSize(resolved->height, texel4) * 4), 1.0f);
            signs |= texel4 << 16;
            exp_scale *= resolved->exp_scale;
            sd.Format = use_snorm ? resolved->snorm : resolved->format;
            if (use_snorm)
            {
              for (uint32_t i = 0; i < 4; ++i)
              {
                if (((signs >> (i * 2)) & 3) == uint32_t(xenos::TextureSign::kSigned))
                {
                  signs &= ~(3u << (i * 2));
                }
              }
            }
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            sd.Shader4ComponentMapping = HostSwizzle(fetch.swizzle, resolved->swizzle) |
                                         D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
            Transition(resource, state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            table.resources[index] = resource;
            ++g.resolved_binds;
          }
          else if (DimensionMatches(dimension, fetch.dimension, fetch.stacked))
          {
            CachedTexture *t = GetTexture(fetch);
            if (t && g_trace && t->loaded && t->desc.height <= 4 && g.traced_textures.insert(t).second)
            {
              // Lookup tables: capture what the loader made of them.
              const auto &gd = t->desc;
              Trace("    texture %08X %ux%u levels %u-%u packed %u base %08X+%X mip %08X+%X fetch %08X %08X %08X "
                    "%08X %08X %08X\n",
                    gd.base_address, gd.width, gd.height, gd.min_level, gd.max_level, unsigned(fetch.packed_mips),
                    gd.base_address, gd.base_size, gd.mip_address, gd.mip_size, r.v[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + b.fetch_constant * 6],
                    r.v[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + b.fetch_constant * 6 + 1],
                    r.v[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + b.fetch_constant * 6 + 2],
                    r.v[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + b.fetch_constant * 6 + 3],
                    r.v[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + b.fetch_constant * 6 + 4],
                    r.v[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + b.fetch_constant * 6 + 5]);
              char name[64];
              std::snprintf(name, sizeof(name), "%03llu_tex_%08X", (unsigned long long)g.capture_index++,
                            gd.base_address);
              CaptureTexture(t->resource.Get(), t->state, t->host->unorm, name);
              Transition(t->resource.Get(), t->state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            }
            if (t)
            {
              bool use_snorm = texture_util::IsAnySignSigned(uint8_t(signs)) && t->host->snorm != DXGI_FORMAT_UNKNOWN;
              sd.Format = use_snorm ? t->host->snorm : t->host->unorm;
              if (use_snorm)
              {
                // Signed components come from the SNORM view.
                for (uint32_t i = 0; i < 4; ++i)
                {
                  if (((signs >> (i * 2)) & 3) == uint32_t(xenos::TextureSign::kSigned))
                  {
                    signs &= ~(3u << (i * 2));
                  }
                }
              }
              sd.Shader4ComponentMapping = HostSwizzle(fetch.swizzle, t->host->swizzle) |
                                           D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
              switch (dimension)
              {
              case xenos::FetchOpDimension::k3DOrStacked:
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                sd.Texture3D.MipLevels = t->mip_levels;
                break;
              case xenos::FetchOpDimension::kCube:
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                sd.TextureCube.MipLevels = t->mip_levels;
                break;
              default:
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = t->mip_levels;
                break;
              }
              table.resources[index] = t->resource.Get();
              ++g.texture_binds;
              CachedTexture *o = t->replaced && t->prompt && dimension == xenos::FetchOpDimension::k2D &&
                                         ao2::prompts::SplitActive()
                                     ? GuestPicture(*t)
                                     : nullptr;
              if (o)
              {
                D3D12_SHADER_RESOURCE_VIEW_DESC &gv = table.guest_views[index];
                gv = sd;
                gv.Format = o->host->unorm;
                gv.Shader4ComponentMapping = HostSwizzle(fetch.swizzle, o->host->swizzle) |
                                             D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
                gv.Texture2D.MipLevels = o->mip_levels;
                Transition(o->resource.Get(), o->state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                table.guest_resources[index] = o->resource.Get();
                table.dual |= 1u << index;
              }
            }
          }
        }
        if (!table.resources[index])
        {
          table.nulls[index] = NullView(dimension);
          ++g.null_binds;
        }
        if (!(tex_info_used & (1u << index)))
        {
          tex_info_used |= 1u << index;
          tex_info[index][0] = signs;
          std::memcpy(&tex_info[index][1], &exp_scale, 4);
          std::memcpy(&tex_info[index][2], uv_scale, 8);
          // What GetDimensions gives: the bound resource's top level (no
          // resource: a null view, 0).
          if (ID3D12Resource *res = table.resources[index])
          {
            D3D12_RESOURCE_DESC rd = res->GetDesc();
            tex_size[index][0] = float(rd.Width);
            tex_size[index][1] = float(rd.Height);
            tex_size[index][2] = float(rd.DepthOrArraySize);
          }
        }
      }
    }

    // Phase 2: writes the table into the frame's shader-visible ring.
    bool SameView(const D3D12_SHADER_RESOURCE_VIEW_DESC &a, const D3D12_SHADER_RESOURCE_VIEW_DESC &b)
    {
      if (a.Format != b.Format || a.ViewDimension != b.ViewDimension ||
          a.Shader4ComponentMapping != b.Shader4ComponentMapping)
      {
        return false;
      }
      switch (a.ViewDimension)
      {
      case D3D12_SRV_DIMENSION_TEXTURE2D:
        return a.Texture2D.MostDetailedMip == b.Texture2D.MostDetailedMip &&
               a.Texture2D.MipLevels == b.Texture2D.MipLevels && a.Texture2D.PlaneSlice == b.Texture2D.PlaneSlice &&
               a.Texture2D.ResourceMinLODClamp == b.Texture2D.ResourceMinLODClamp;
      case D3D12_SRV_DIMENSION_TEXTURE3D:
        return a.Texture3D.MostDetailedMip == b.Texture3D.MostDetailedMip &&
               a.Texture3D.MipLevels == b.Texture3D.MipLevels &&
               a.Texture3D.ResourceMinLODClamp == b.Texture3D.ResourceMinLODClamp;
      case D3D12_SRV_DIMENSION_TEXTURECUBE:
        return a.TextureCube.MostDetailedMip == b.TextureCube.MostDetailedMip &&
               a.TextureCube.MipLevels == b.TextureCube.MipLevels &&
               a.TextureCube.ResourceMinLODClamp == b.TextureCube.ResourceMinLODClamp;
      default:
        return false;
      }
    }

    bool SameTable(const TextureTable &a, const TextureTable &b)
    {
      if (a.any != b.any || a.used != b.used)
      {
        return false;
      }
      for (uint32_t used = a.used; used; used &= used - 1)
      {
        uint32_t i = uint32_t(__builtin_ctz(used));
        if (a.resources[i] != b.resources[i])
        {
          return false;
        }
        if (a.resources[i] ? !SameView(a.views[i], b.views[i]) : a.nulls[i].ptr != b.nulls[i].ptr)
        {
          return false;
        }
      }
      return true;
    }

    // The table last written for a stage this frame: consecutive draws with
    // the same textures (one material) reuse it.
    struct LastTable
    {
      TextureTable table;
      D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
      uint64_t frame = ~0ull;
    };
    LastTable g_last_tables[2];

    D3D12_GPU_DESCRIPTOR_HANDLE WriteTextureTableNew(const TextureTable &table);

    // Tables written this frame by content (materials recur all over a frame;
    // the frame's descriptor range stays valid until its fence).
    std::unordered_map<uint64_t, D3D12_GPU_DESCRIPTOR_HANDLE, FastHash> g_frame_tables;
    uint64_t g_frame_tables_frame = ~0ull;

    uint64_t TableKey(const TextureTable &table)
    {
      struct Slot
      {
        ID3D12Resource *resource;
        uint64_t null_or_format;
        uint32_t dimension, mapping, mips, slot;
      } slots[kMaxTextureBindings];
      uint32_t n = 0;
      for (uint32_t used = table.used; used; used &= used - 1)
      {
        uint32_t i = uint32_t(__builtin_ctz(used));
        const D3D12_SHADER_RESOURCE_VIEW_DESC &v = table.views[i];
        Slot &sl = slots[n++];
        sl.resource = table.resources[i];
        sl.null_or_format = sl.resource ? uint64_t(v.Format) : table.nulls[i].ptr;
        sl.dimension = sl.resource ? uint32_t(v.ViewDimension) : 0;
        sl.mapping = sl.resource ? v.Shader4ComponentMapping : 0;
        // MipLevels sits at the same offset for 2D, 3D and cube views.
        sl.mips = sl.resource ? v.Texture2D.MipLevels : 0;
        sl.slot = i;
      }
      return XXH3_64bits(slots, n * sizeof(Slot)) ^ table.used;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE WriteTextureTable(const TextureTable &table, uint32_t stage)
    {
      LastTable &last = g_last_tables[stage];
      if (last.frame == g.frame_number && SameTable(last.table, table))
      {
        return last.gpu;
      }
      if (g_frame_tables_frame != g.frame_number)
      {
        g_frame_tables.clear();
        g_frame_tables_frame = g.frame_number;
      }
      uint64_t key = table.any ? TableKey(table) : 0;
      D3D12_GPU_DESCRIPTOR_HANDLE written;
      if (auto it = table.any ? g_frame_tables.find(key) : g_frame_tables.end(); it != g_frame_tables.end())
      {
        written = it->second;
      }
      else
      {
        written = WriteTextureTableNew(table);
        if (table.any)
        {
          g_frame_tables.emplace(key, written);
        }
      }
      last.table = table;
      last.gpu = written;
      last.frame = g.frame_number;
      return written;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE WriteTextureTableNew(const TextureTable &table)
    {
      Frame &f = CurrentFrame();
      uint32_t base;
      bool shared_null = !table.any || f.views_used + kMaxTextureBindings > kViewsPerFrame;
      if (shared_null)
      {
        // Shared table of 2D null descriptors past the per-frame rings.
        base = kViewsPerFrame * kFrameCount;
        if (g.null_table_ready)
        {
          D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.view_heap->GetGPUDescriptorHandleForHeapStart();
          gpu.ptr += uint64_t(base) * g.view_size;
          return gpu;
        }
        g.null_table_ready = true;
      }
      else
      {
        base = g.frame_index * kViewsPerFrame + f.views_used;
        f.views_used += kMaxTextureBindings;
      }
      // Only the slots the shader declares are read; the rest of the range
      // keeps whatever an earlier frame left there.
      for (uint32_t used = shared_null ? (1u << kMaxTextureBindings) - 1 : table.used; used; used &= used - 1)
      {
        uint32_t i = uint32_t(__builtin_ctz(used));
        D3D12_CPU_DESCRIPTOR_HANDLE dst = g.view_heap->GetCPUDescriptorHandleForHeapStart();
        dst.ptr += uint64_t(base + i) * g.view_size;
        if (!shared_null && table.resources[i])
        {
          g.device->CreateShaderResourceView(table.resources[i], &table.views[i], dst);
        }
        else
        {
          g.device->CopyDescriptorsSimple(1, dst, shared_null ? g.null_srv_2d : table.nulls[i],
                                          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
      }
      D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.view_heap->GetGPUDescriptorHandleForHeapStart();
      gpu.ptr += uint64_t(base) * g.view_size;
      return gpu;
    }

    // ------------------------------------------------------------ Samplers

    D3D12_TEXTURE_ADDRESS_MODE AddressMode(xenos::ClampMode m)
    {
      switch (m)
      {
      case xenos::ClampMode::kRepeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
      case xenos::ClampMode::kMirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
      case xenos::ClampMode::kMirrorClampToEdge:
      case xenos::ClampMode::kMirrorClampToHalfway: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
      case xenos::ClampMode::kClampToBorder:
      case xenos::ClampMode::kMirrorClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
      default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      }
    }

    // Sampler state of a binding: fetch constant with instruction overrides.
    uint32_t SamplerKey(const xenos::xe_gpu_texture_fetch_t &f, const rex::graphics::ParsedTextureFetchInstruction &instr)
    {
      const auto &a = instr.attributes;
      auto pick = [](xenos::TextureFilter i, xenos::TextureFilter c) {
        return uint32_t(i == xenos::TextureFilter::kUseFetchConst ? c : i);
      };
      uint32_t mag = pick(a.mag_filter, f.mag_filter) & 1;
      uint32_t min = pick(a.min_filter, f.min_filter) & 1;
      uint32_t mip = pick(a.mip_filter, f.mip_filter);
      uint32_t aniso = uint32_t(a.aniso_filter == xenos::AnisoFilter::kUseFetchConst ? f.aniso_filter
                                                                                      : a.aniso_filter);
      if (aniso > uint32_t(xenos::AnisoFilter::kMax_16_1))
      {
        aniso = 0;
      }
      return uint32_t(f.clamp_x) | (uint32_t(f.clamp_y) << 3) | (uint32_t(f.clamp_z) << 6) | (mag << 9) |
             (min << 10) | ((mip & 3) << 11) | (aniso << 13) |
             (uint32_t(f.border_color == xenos::BorderColor::k_ABGR_White) << 16) |
             ((uint32_t(f.lod_bias) & 0x3FF) << 17);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE SamplerDescriptor(uint32_t key)
    {
      auto it = g.sampler_descriptors.find(key);
      if (it == g.sampler_descriptors.end())
      {
        uint32_t index = uint32_t(g.sampler_descriptors.size());
        if (index >= kCpuSamplerCount)
        {
          index = kCpuSamplerCount - 1; // exhausted: reuse the last one
        }
        D3D12_SAMPLER_DESC sd = {};
        uint32_t aniso = (key >> 13) & 7;
        uint32_t mip = (key >> 11) & 3;
        bool base_map = mip == uint32_t(xenos::TextureFilter::kBaseMap);
        if (aniso > uint32_t(xenos::AnisoFilter::kMax_1_1))
        {
          sd.Filter = D3D12_FILTER_ANISOTROPIC;
          sd.MaxAnisotropy = 1u << (aniso - 1);
        }
        else
        {
          sd.Filter = D3D12_ENCODE_BASIC_FILTER((key >> 10) & 1, (key >> 9) & 1, base_map ? 0 : (mip & 1),
                                                D3D12_FILTER_REDUCTION_TYPE_STANDARD);
          sd.MaxAnisotropy = 1;
        }
        sd.AddressU = AddressMode(xenos::ClampMode(key & 7));
        sd.AddressV = AddressMode(xenos::ClampMode((key >> 3) & 7));
        sd.AddressW = AddressMode(xenos::ClampMode((key >> 6) & 7));
        int32_t bias = int32_t(key << 5) >> 22; // signed 10 bits at 17
        sd.MipLODBias = float(bias) / 32.0f;
        sd.MinLOD = 0.0f;
        sd.MaxLOD = base_map ? 0.0f : D3D12_FLOAT32_MAX;
        float border = ((key >> 16) & 1) ? 1.0f : 0.0f;
        sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = border;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g.cpu_sampler_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += uint64_t(index) * g.sampler_size;
        g.device->CreateSampler(&sd, h);
        it = g.sampler_descriptors.emplace(key, index).first;
      }
      D3D12_CPU_DESCRIPTOR_HANDLE h = g.cpu_sampler_heap->GetCPUDescriptorHandleForHeapStart();
      h.ptr += uint64_t(it->second) * g.sampler_size;
      return h;
    }

    // Makes room for `count` new sampler tables; when the shader-visible heap
    // is full, the GPU is drained and the tables are rebuilt. Must precede the
    // BindSamplers calls of a draw (which then never flush).
    void ReserveSamplerTables(uint32_t count)
    {
      if (g.samplers_used + count * kMaxTextureBindings > kSamplerHeapSize)
      {
        FlushAndWait();
        g.sampler_tables.clear();
        g.samplers_used = 0;
      }
    }

    // Returns a shader-visible sampler table for the shader's bindings, reusing
    // identical tables.
    D3D12_GPU_DESCRIPTOR_HANDLE BindSamplers(const CompiledShader *s, const RegisterFile &r)
    {
      uint32_t keys[kMaxTextureBindings] = {};
      uint32_t count = 1;
      if (s)
      {
        for (const auto &b : s->shader->texture_bindings())
        {
          if (b.binding_index >= kMaxTextureBindings)
          {
            continue;
          }
          auto fetch = r.Get<xenos::xe_gpu_texture_fetch_t>(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                                            b.fetch_constant * 6);
          keys[b.binding_index] = SamplerKey(fetch, b.fetch_instr);
          count = std::max(count, uint32_t(b.binding_index) + 1);
        }
      }
      uint64_t table_key = XXH3_64bits(keys, count * sizeof(uint32_t)) ^ count;
      auto it = g.sampler_tables.find(table_key);
      if (it == g.sampler_tables.end())
      {
        uint32_t base = g.samplers_used;
        g.samplers_used += kMaxTextureBindings;
        for (uint32_t i = 0; i < kMaxTextureBindings; ++i)
        {
          D3D12_CPU_DESCRIPTOR_HANDLE dst = g.sampler_heap->GetCPUDescriptorHandleForHeapStart();
          dst.ptr += uint64_t(base + i) * g.sampler_size;
          g.device->CopyDescriptorsSimple(1, dst, SamplerDescriptor(keys[i < count ? i : 0]),
                                          D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }
        it = g.sampler_tables.emplace(table_key, base).first;
      }
      D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.sampler_heap->GetGPUDescriptorHandleForHeapStart();
      gpu.ptr += uint64_t(it->second) * g.sampler_size;
      return gpu;
    }

    // ------------------------------------------------------------ Constants

    void FillSystemConstants(const RegisterFile &r, const Viewport &vp, const uint32_t tex_info[16][4],
                             const float tex_size[16][4],
                             const DrawPacket *index_fetch, uint32_t scale4, XeSystemConstants &c)
    {
      std::memset(&c, 0, sizeof(c));
      auto vte = r.Get<reg::PA_CL_VTE_CNTL>();
      if (vte.vtx_w0_fmt)
      {
        c.flags |= kSysFlagWNotReciprocal;
      }
      if (vte.vtx_xy_fmt)
      {
        c.flags |= kSysFlagXYDividedByW;
      }
      if (vte.vtx_z_fmt)
      {
        c.flags |= kSysFlagZDividedByW;
      }
      auto colorcontrol = r.Get<reg::RB_COLORCONTROL>();
      if (colorcontrol.alpha_test_enable)
      {
        c.flags |= kSysFlagAlphaTest;
      }
      c.alpha_func = uint32_t(colorcontrol.alpha_func);
      c.alpha_ref = r.F(XE_GPU_REG_RB_ALPHA_REF);
      c.vertex_index_offset = r.v[XE_GPU_REG_VGT_INDX_OFFSET];
      c.vertex_index_range[0] = r.v[XE_GPU_REG_VGT_MIN_VTX_INDX];
      c.vertex_index_range[1] = r.v[XE_GPU_REG_VGT_MAX_VTX_INDX];
      if (c.vertex_index_range[1] < c.vertex_index_range[0])
      {
        c.vertex_index_range[0] = 0;
        c.vertex_index_range[1] = 0xFFFFFF;
      }
      if (index_fetch)
      {
        // The vertex shader reads the guest index buffer (see the translator).
        c.vertex_index_range[2] = index_fetch->index_base & 0x1FFFFFFF;
        c.vertex_index_range[3] = 1u | (index_fetch->index_format == xenos::IndexFormat::kInt32 ? 2u : 0u) |
                                  ((uint32_t(index_fetch->index_endian) & 3u) << 2);
      }
      for (uint32_t i = 0; i < 3; ++i)
      {
        c.ndc_scale[i] = vp.ndc_scale[i];
        c.ndc_offset[i] = vp.ndc_offset[i];
      }
      for (uint32_t i = 0; i < 4; ++i)
      {
        auto ci = r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
        // 16-bit fixed surfaces hold -32...32: stored /32 in the host snorm
        // target so the bits match the guest's (resolves copy them raw).
        c.color_exp_bias[i] = std::ldexp(1.0f, int(ci.color_exp_bias) - (IsFixed16(ci.color_format) ? 5 : 0));
      }
      c.resolution_scale[0] = float(scale4) * 0.25f;
      c.resolution_scale[1] = 4.0f / float(scale4);
      c.resolution_scale[2] = float(scale4);
      std::memcpy(c.bool_loop, &r.v[0x4900], sizeof(c.bool_loop));
      std::memcpy(c.tex_info, tex_info, sizeof(c.tex_info));
      std::memcpy(c.vfetch, &r.v[0x4800], sizeof(c.vfetch));
      std::memcpy(c.tex_size, tex_size, sizeof(c.tex_size));
      // The guest size of each texture the way the shaders worked it out per
      // pixel: host size x (guest / host size) / host texels per guest texel.
      for (uint32_t i = 0; i < 16; ++i)
      {
        float ts[2];
        std::memcpy(ts, &tex_info[i][2], 8);
        float tscale = std::max(float((tex_info[i][0] >> 16) & 0xFFu) * 0.25f, 1.0f);
        float w = tex_size[i][0] * (ts[0] / tscale), h = tex_size[i][1] * (ts[1] / tscale);
        c.tex_rcp[i][0] = w > 0.0f ? 1.0f / w : 0.0f;
        c.tex_rcp[i][1] = h > 0.0f ? 1.0f / h : 0.0f;
      }
    }

    D3D12_GPU_VIRTUAL_ADDRESS UploadFloatConstants(const RegisterFile &r, bool pixel)
    {
      uint32_t base = pixel ? (r.v[XE_GPU_REG_SQ_PS_CONST] & 0x1FF) : (r.v[XE_GPU_REG_SQ_VS_CONST] & 0x1FF);
      base = std::min(base, 256u);
      State::ConstantUpload &c = g.constant_uploads[pixel ? 1 : 0];
      if (c.generation == g.upload_generation && c.base == base && c.version0 == r.float_version[0] &&
          c.version1 == r.float_version[1] && c.address)
      {
        return c.address;
      }
      c.address = Upload(&r.v[0x4000 + base * 4], 256 * 16);
      c.generation = g.upload_generation;
      c.base = base;
      c.version0 = r.float_version[0];
      c.version1 = r.float_version[1];
      return c.address;
    }
    // ------------------------------------------------------------ EDRAM ownership

    // Render targets are separate host textures, but on the guest they are
    // views of the same 10 MB of EDRAM: games reinterpret surfaces (D3D clears
    // color by drawing into a depth surface aliasing it, passes reuse memory
    // with other formats/pitches). Every 32bpp-cell tile remembers which host
    // render target wrote it last; when another one is used over those tiles,
    // their contents are transferred with the bits re-packed (like the SDK's
    // render target cache transfers).
    constexpr uint32_t kEdramTiles = 2048;

    struct EdramLayout
    {
      uint32_t base, pitch_tiles, scale_x, scale_y, tile_w; // tile_w in samples (80 or 40 for 64bpp)
    };

    EdramLayout GetEdramLayout(const RenderTarget &rt)
    {
      EdramLayout l;
      l.base = rt.edram_base;
      l.scale_x = rt.samples >= 4 ? 2 : 1;
      l.scale_y = rt.samples >= 2 ? 2 : 1;
      l.tile_w = rt.is_64bpp ? 40 : 80;
      l.pitch_tiles = ((rt.edram_pitch * l.scale_x + 79) / 80) << (rt.is_64bpp ? 1 : 0);
      return l;
    }

    // Calls fn(tile) for each EDRAM tile covering the pixel rectangle of `rt`.
    template <typename F>
    void ForEachTile(const RenderTarget &rt, const D3D12_RECT &rect, F &&fn)
    {
      EdramLayout l = GetEdramLayout(rt);
      int32_t x1 = std::min<int32_t>(rect.right, int32_t(rt.width));
      int32_t y1 = std::min<int32_t>(rect.bottom, int32_t(rt.height));
      if (rect.left >= x1 || rect.top >= y1)
      {
        return;
      }
      uint32_t c0 = uint32_t(rect.left) * l.scale_x / l.tile_w, c1 = (uint32_t(x1) * l.scale_x - 1) / l.tile_w;
      uint32_t r0 = uint32_t(rect.top) * l.scale_y / 16, r1 = (uint32_t(y1) * l.scale_y - 1) / 16;
      for (uint32_t row = r0; row <= r1; ++row)
      {
        for (uint32_t col = c0; col <= c1; ++col)
        {
          fn((l.base + row * l.pitch_tiles + col) % kEdramTiles);
        }
      }
    }

    // Pixel rectangle of an EDRAM tile in `rt` (32bpp surfaces).
    D3D12_RECT TileRect(const RenderTarget &rt, uint32_t tile)
    {
      EdramLayout l = GetEdramLayout(rt);
      uint32_t rel = (tile + kEdramTiles - l.base) % kEdramTiles;
      uint32_t row = rel / l.pitch_tiles, col = rel % l.pitch_tiles;
      return {LONG(col * l.tile_w / l.scale_x), LONG(row * 16 / l.scale_y), LONG((col + 1) * l.tile_w / l.scale_x),
              LONG((row + 1) * 16 / l.scale_y)};
    }

    // Host-pixel rectangles covering `tiles` of `rt` within `clip` (tiles in
    // row-major order; neighbours in a row are merged).
    void TileRects(const RenderTarget &rt, const std::vector<uint32_t> &tiles, const D3D12_RECT &clip,
                   std::vector<D3D12_RECT> &out)
    {
      out.clear();
      for (uint32_t tile : tiles)
      {
        D3D12_RECT t = TileRect(rt, tile);
        t = {std::max(t.left, clip.left), std::max(t.top, clip.top), std::min(t.right, clip.right),
             std::min(t.bottom, clip.bottom)};
        if (t.left >= t.right || t.top >= t.bottom)
        {
          continue;
        }
        if (!out.empty() && out.back().top == t.top && out.back().bottom == t.bottom && out.back().right == t.left)
        {
          out.back().right = t.right;
        }
        else
        {
          out.push_back(t);
        }
      }
      // Rows with the same extent stack into one rectangle.
      size_t merged = 0;
      for (size_t i = 0; i < out.size(); ++i)
      {
        if (merged && out[merged - 1].left == out[i].left && out[merged - 1].right == out[i].right &&
            out[merged - 1].bottom == out[i].top)
        {
          out[merged - 1].bottom = out[i].bottom;
        }
        else
        {
          out[merged++] = out[i];
        }
      }
      out.resize(merged);
      for (D3D12_RECT &r : out)
      {
        r = ScaleRect(r, rt.scale4);
      }
    }

    // Clears with many rectangles crash the NVIDIA driver (stack buffer
    // overrun inside nvwgf2umx): issue them a few at a time.
    constexpr size_t kMaxClearRects = 8;

    void ClearRtvRects(D3D12_CPU_DESCRIPTOR_HANDLE view, const float color[4], const std::vector<D3D12_RECT> &rects)
    {
      for (size_t i = 0; i < rects.size(); i += kMaxClearRects)
      {
        g.list->ClearRenderTargetView(view, color, UINT(std::min(kMaxClearRects, rects.size() - i)), &rects[i]);
      }
    }

    void ClearDsvRects(D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags, float depth, uint8_t stencil,
                       const std::vector<D3D12_RECT> &rects)
    {
      for (size_t i = 0; i < rects.size(); i += kMaxClearRects)
      {
        g.list->ClearDepthStencilView(view, flags, depth, stencil, UINT(std::min(kMaxClearRects, rects.size() - i)),
                                      &rects[i]);
      }
    }

    // Tiles of `rect` (guest pixels) completely inside it.
    // ForEachTile's walk with each tile's rectangle from its row and column
    // (TileRect's divisions per tile were a few % of the GPU thread); a
    // surface running past the end of EDRAM wraps, as TileRect sees it.
    void TilesInside(const RenderTarget &rt, const D3D12_RECT &rect, std::vector<uint32_t> &out)
    {
      out.clear();
      EdramLayout l = GetEdramLayout(rt);
      int32_t x1 = std::min<int32_t>(rect.right, int32_t(rt.width));
      int32_t y1 = std::min<int32_t>(rect.bottom, int32_t(rt.height));
      if (rect.left >= x1 || rect.top >= y1)
      {
        return;
      }
      uint32_t c0 = uint32_t(rect.left) * l.scale_x / l.tile_w, c1 = (uint32_t(x1) * l.scale_x - 1) / l.tile_w;
      uint32_t r0 = uint32_t(rect.top) * l.scale_y / 16, r1 = (uint32_t(y1) * l.scale_y - 1) / 16;
      auto inside = [&](const D3D12_RECT &t) {
        LONG right = std::min(t.right, LONG(rt.width)), bottom = std::min(t.bottom, LONG(rt.height));
        return t.left >= rect.left && t.top >= rect.top && right <= rect.right && bottom <= rect.bottom;
      };
      for (uint32_t row = r0; row <= r1; ++row)
      {
        LONG top = LONG(row * 16 / l.scale_y), bottom = LONG((row + 1) * 16 / l.scale_y);
        for (uint32_t col = c0; col <= c1; ++col)
        {
          uint32_t index = row * l.pitch_tiles + col;
          uint32_t tile = (l.base + index) % kEdramTiles;
          D3D12_RECT t = index < kEdramTiles && col < l.pitch_tiles ? D3D12_RECT{LONG(col * l.tile_w / l.scale_x), top,
                                                          LONG((col + 1) * l.tile_w / l.scale_x), bottom}
                                             : TileRect(rt, tile);
          if (inside(t))
          {
            out.push_back(tile);
          }
        }
      }
    }

    // Records that `tiles` of component `c` of `rt` now hold `value`; tiles
    // with another remembered value are forgotten.
    void SetUniform(RenderTarget &rt, uint32_t c, const std::vector<uint32_t> &tiles, uint32_t value)
    {
      if (rt.is_64bpp)
      {
        return;
      }
      if (rt.uniform_value[c] != value)
      {
        rt.uniform_tiles[c].reset();
        rt.uniform_value[c] = value;
      }
      for (uint32_t tile : tiles)
      {
        rt.uniform_tiles[c].set(tile);
      }
    }

    // Forgets the uniform state of component `c` under `rect` (written by
    // something else than a recognized clear).
    void ForgetUniform(RenderTarget &rt, uint32_t c, const D3D12_RECT &rect)
    {
      if (rt.uniform_tiles[c].any())
      {
        ForEachTile(rt, rect, [&](uint32_t tile) { rt.uniform_tiles[c].reset(tile); });
      }
    }

    // Raw 32-bit EDRAM cells <-> host render target values (ports of the
    // transfer shader's PackColor/UnpackColor) for the formats clears are
    // propagated through: 8_8_8_8, 2_10_10_10 (+AS), 7e3 (+AS), 32_FLOAT.
    bool UniformColorFormat(uint32_t format)
    {
      return format == 0 || format == 2 || format == 10 || format == 3 || format == 12 || format == 14;
    }

    uint32_t Encode7e3(float f)
    {
      f = std::clamp(f, 0.0f, 31.875f);
      uint32_t b;
      std::memcpy(&b, &f, 4);
      uint32_t biased =
          b < 0x3E800000u ? ((b & 0x7FFFFFu) | 0x800000u) >> std::min(125u - (b >> 23), 24u) : b + 0xC2000000u;
      return ((biased + 0x7FFFu + ((biased >> 16) & 1u)) >> 16) & 0x3FFu;
    }

    uint32_t PackColorRaw(const float c[4], uint32_t format)
    {
      auto unorm = [](float v, float max) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * max)); };
      switch (format)
      {
      case 2:
      case 10:
        return unorm(c[0], 1023.0f) | (unorm(c[1], 1023.0f) << 10) | (unorm(c[2], 1023.0f) << 20) |
               (unorm(c[3], 3.0f) << 30);
      case 3:
      case 12:
        return Encode7e3(c[0]) | (Encode7e3(c[1]) << 10) | (Encode7e3(c[2]) << 20) | (unorm(c[3], 3.0f) << 30);
      case 14:
      {
        uint32_t v;
        std::memcpy(&v, &c[0], 4);
        return v;
      }
      default:
        return unorm(c[0], 255.0f) | (unorm(c[1], 255.0f) << 8) | (unorm(c[2], 255.0f) << 16) |
               (unorm(c[3], 255.0f) << 24);
      }
    }

    void UnpackColorRaw(uint32_t v, uint32_t format, float out[4])
    {
      switch (format)
      {
      case 2:
      case 10:
        for (uint32_t i = 0; i < 3; ++i)
        {
          out[i] = float((v >> (i * 10)) & 0x3FF) / 1023.0f;
        }
        out[3] = float(v >> 30) / 3.0f;
        break;
      case 3:
      case 12:
        for (uint32_t i = 0; i < 3; ++i)
        {
          out[i] = xenos::Float7e3To32((v >> (i * 10)) & 0x3FF);
        }
        out[3] = float(v >> 30) / 3.0f;
        break;
      case 14:
        std::memcpy(&out[0], &v, 4);
        out[1] = out[2] = out[3] = 0.0f;
        break;
      default:
        for (uint32_t i = 0; i < 4; ++i)
        {
          out[i] = float((v >> (i * 8)) & 0xFF) / 255.0f;
        }
        break;
      }
    }

    uint32_t EncodeDepth24(float depth, uint32_t format)
    {
      depth = std::clamp(depth, 0.0f, 1.0f);
      return format == 1 ? xenos::Float32To20e4(depth, true) : uint32_t(std::lround(depth * 16777215.0f));
    }

    float DecodeDepth24(uint32_t d24, uint32_t format)
    {
      return format == 1 ? xenos::Float20e4To32(d24) : float(d24) / 16777215.0f;
    }

    const char kTransferShader[] = R"(
cbuffer c : register(b0) {
  uint dst_base; uint dst_pitch_tiles; uint dst_samples; uint dst_format;
  uint src_base; uint src_pitch_tiles; uint src_samples; uint src_format;
  uint owner_id; uint swap_halves; uint stencil_bit;
  uint scales; // host pixels per guest pixel in quarters: destination | source << 8
  uint tile_w; // samples per tile row: 80, or 40 for 64bpp surfaces (copied as is)
};
ByteAddressBuffer owners : register(t0, space1);
#if SRC_DEPTH
#if SRC_MS
Texture2DMS<float> src_depth : register(t0);
Texture2DMS<uint2> src_stencil : register(t1);
#define LOAD_DEPTH(p, s) src_depth.Load(int2(p), s)
#define LOAD_STENCIL(p, s) src_stencil.Load(int2(p), s).g
#else
Texture2D<float> src_depth : register(t0);
Texture2D<uint2> src_stencil : register(t1);
#define LOAD_DEPTH(p, s) src_depth.Load(int3(p, 0))
#define LOAD_STENCIL(p, s) src_stencil.Load(int3(p, 0)).g
#endif
#else
#if SRC_MS
Texture2DMS<float4> src_color : register(t0);
#define LOAD_COLOR(p, s) src_color.Load(int2(p), s)
#else
Texture2D<float4> src_color : register(t0);
#define LOAD_COLOR(p, s) src_color.Load(int3(p, 0))
#endif
#endif

float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

// Xenos 7e3 (2_10_10_10_FLOAT color) and 20e4 (D24FS8 depth) floats.
uint Encode7e3(float f) {
  uint b = asuint(clamp(f, 0.0, 31.875));
  uint biased = b < 0x3E800000u ? ((b & 0x7FFFFFu) | 0x800000u) >> min(125u - (b >> 23), 24u) : b + 0xC2000000u;
  return ((biased + 0x7FFFu + ((biased >> 16) & 1u)) >> 16) & 0x3FFu;
}
float Decode7e3(uint v) {
  uint m = v & 0x7Fu, e = (v >> 7) & 7u;
  return e == 0u ? float(m) / 512.0 : (1.0 + float(m) / 128.0) * exp2(float(e) - 3.0);
}
uint Encode20e4(float f) {
  uint b = asuint(clamp(f, 0.0, 1.99999988));
  uint biased = b < 0x38800000u ? ((b & 0x7FFFFFu) | 0x800000u) >> min(113u - (b >> 23), 24u) : b + 0xC8000000u;
  return ((biased + 3u + ((biased >> 3) & 1u)) >> 3) & 0xFFFFFFu;
}

// Raw 32-bit EDRAM cell <-> host values (formats are ColorRenderTargetFormat;
// 16-bit fixed surfaces hold value/32 on the host, i.e. the raw snorm16).
uint PackColor(float4 c, uint format) {
  switch (format) {
    case 2u: case 10u: {
      uint3 rgb = uint3(round(saturate(c.rgb) * 1023.0));
      return rgb.r | (rgb.g << 10) | (rgb.b << 20) | (uint(round(saturate(c.a) * 3.0)) << 30);
    }
    case 3u: case 12u:
      return Encode7e3(c.r) | (Encode7e3(c.g) << 10) | (Encode7e3(c.b) << 20) | (uint(round(saturate(c.a) * 3.0)) << 30);
    case 4u: {
      uint2 s = uint2(int2(round(clamp(c.rg, -1.0, 1.0) * 32767.0))) & 0xFFFFu;
      return s.r | (s.g << 16);
    }
    case 6u: return f32tof16(c.r) | (f32tof16(c.g) << 16);
    case 14u: return asuint(c.r);
    default: {
      uint4 b = uint4(round(saturate(c) * 255.0));
      return b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
    }
  }
}
float4 UnpackColor(uint v, uint format) {
  switch (format) {
    case 2u: case 10u:
      return float4(float(v & 0x3FFu), float((v >> 10) & 0x3FFu), float((v >> 20) & 0x3FFu), 0.0) /
             float4(1023.0, 1023.0, 1023.0, 1.0) + float4(0.0, 0.0, 0.0, float(v >> 30) / 3.0);
    case 3u: case 12u:
      return float4(Decode7e3(v & 0x3FFu), Decode7e3((v >> 10) & 0x3FFu), Decode7e3((v >> 20) & 0x3FFu),
                    float(v >> 30) / 3.0);
    case 4u: {
      int2 s = int2(v << 16, v) >> 16;
      return float4(max(float2(s) / 32767.0, -1.0), 0.0, 0.0);
    }
    case 6u: return float4(f16tof32(v), f16tof32(v >> 16), 0.0, 0.0);
    case 14u: return float4(asfloat(v), 0.0, 0.0, 0.0);
    default:
      return float4(float(v & 0xFFu), float((v >> 8) & 0xFFu), float((v >> 16) & 0xFFu), float(v >> 24)) / 255.0;
  }
}

// EDRAM sample position <-> host pixel and sample (2x: samples stacked
// vertically, 4x: 2x2).
uint2 SamplePos(uint2 px, uint s, uint samples) {
  if (samples >= 4u) return uint2(px.x * 2u + (s & 1u), px.y * 2u + (s >> 1));
  if (samples == 2u) return uint2(px.x, px.y * 2u + s);
  return px;
}
void PixelOf(uint2 sp, uint samples, out uint2 px, out uint s) {
  if (samples >= 4u) { px = sp >> 1; s = (sp.x & 1u) | ((sp.y & 1u) << 1); }
  else if (samples == 2u) { px = uint2(sp.x, sp.y >> 1); s = sp.y & 1u; }
  else { px = sp; s = 0u; }
}

float Decode20e4(uint v) {
  uint m = v & 0xFFFFFu, e = v >> 20;
  return e == 0u ? float(m) * exp2(-34.0) : (1.0 + float(m) / 1048576.0) * exp2(float(e) - 15.0);
}

#if DST_DEPTH
struct PsOut { float depth : SV_Depth; };
PsOut ps(float4 pos : SV_Position, uint sample : SV_SampleIndex) {
#else
float4 ps(float4 pos : SV_Position, uint sample : SV_SampleIndex) : SV_Target {
#endif
  // The EDRAM as the host has it: a surface's host samples are a grid `scale`
  // times finer than its guest samples (a tile is a whole number of them at
  // any scale in quarters), and the same place of that grid is the same
  // sample in every surface of that scale.
  uint dst_scale4 = scales & 0xFFu, src_scale4 = scales >> 8;
  uint2 hs = SamplePos(uint2(pos.xy), sample, dst_samples);
  uint2 dst_tile = uint2(tile_w, 16u) * dst_scale4 / 4u;
  uint2 tc = hs / dst_tile;
  uint tile = (dst_base + tc.y * dst_pitch_tiles + tc.x) & 2047u;
  if (owners.Load(tile * 4u) != owner_id) { discard; }
  uint2 in_tile = hs - tc * dst_tile;
  uint2 src_tile = uint2(tile_w, 16u) * src_scale4 / 4u;
  // Another scale (small render targets, see SmallTarget): the nearest sample.
  if (src_scale4 != dst_scale4) { in_tile = (in_tile * 2u + 1u) * src_scale4 / (dst_scale4 * 2u); }
  // Depth tiles store their two halves swapped relative to color.
  if (swap_halves != 0u) { in_tile.x = (in_tile.x + (src_tile.x >> 1)) % src_tile.x; }
  uint rel = (tile + 2048u - src_base) & 2047u;
  uint2 ss = uint2(rel % src_pitch_tiles, rel / src_pitch_tiles) * src_tile + in_tile;
  uint2 sp; uint si;
  PixelOf(ss, src_samples, sp, si);
#if SRC_DEPTH
  float d = LOAD_DEPTH(sp, si);
  uint d24 = src_format == 1u ? Encode20e4(d) : uint(round(saturate(d) * 16777215.0));
  uint raw = (d24 << 8) | (LOAD_STENCIL(sp, si) & 0xFFu);
#else
  float4 color = LOAD_COLOR(sp, si);
#if !DST_DEPTH
  // 64bpp: both surfaces have the same format.
  if (tile_w == 40u) { return color; }
#endif
  uint raw = PackColor(color, src_format);
#endif
#if DST_DEPTH
  // Depth pass (stencil_bit == 0), then one pass per stencil bit.
  if (stencil_bit != 0u && (raw & stencil_bit) == 0u) { discard; }
  PsOut o;
  o.depth = dst_format == 1u ? Decode20e4(raw >> 8) : float(raw >> 8) / 16777215.0;
  return o;
#else
  return UnpackColor(raw, dst_format);
#endif
}
)";

    bool InitTransferPipelines()
    {
      D3D12_DESCRIPTOR_RANGE range = {};
      range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      range.NumDescriptors = 2;
      D3D12_ROOT_PARAMETER p[3] = {};
      p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      p[0].DescriptorTable.NumDescriptorRanges = 1;
      p[0].DescriptorTable.pDescriptorRanges = &range;
      p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      p[1].Constants.Num32BitValues = 13;
      p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
      p[2].Descriptor.RegisterSpace = 1;
      p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      D3D12_ROOT_SIGNATURE_DESC desc = {};
      desc.NumParameters = 3;
      desc.pParameters = p;
      ComPtr<ID3DBlob> blob, error;
      if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
          FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&g.transfer_root))))
      {
        return false;
      }
      g.transfer_vs = Compile(kTransferShader, "vs", "vs_5_1");
      for (uint32_t variant = 0; variant < 8; ++variant)
      {
        D3D_SHADER_MACRO macros[] = {{"SRC_DEPTH", (variant & 2) ? "1" : "0"},
                                     {"SRC_MS", (variant & 1) ? "1" : "0"},
                                     {"DST_DEPTH", (variant & 4) ? "1" : "0"},
                                     {nullptr, nullptr}};
        ComPtr<ID3DBlob> code, errors;
        if (FAILED(D3DCompile(kTransferShader, std::strlen(kTransferShader), nullptr, macros, nullptr, "ps", "ps_5_1",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
        {
          NativeLog("transfer shader %u failed: %s\n", variant,
                    errors ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
          return false;
        }
        auto *b = static_cast<const uint8_t *>(code->GetBufferPointer());
        g.transfer_ps[variant].assign(b, b + code->GetBufferSize());
      }
      return !g.transfer_vs.empty();
    }

    // stencil_bit_index: 0 = color or depth pass, 1..8 = pass writing stencil bit (index - 1).
    ID3D12PipelineState *GetTransferPipeline(uint32_t variant, DXGI_FORMAT format, uint32_t samples,
                                             uint32_t stencil_bit_index)
    {
      uint32_t key = (stencil_bit_index << 24) | (variant << 16) | (samples << 8) | uint32_t(format);
      auto it = g.transfer_pipelines.find(key);
      if (it != g.transfer_pipelines.end())
      {
        return it->second.Get();
      }
      D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
      d.pRootSignature = g.transfer_root.Get();
      d.VS = {g.transfer_vs.data(), g.transfer_vs.size()};
      d.PS = {g.transfer_ps[variant].data(), g.transfer_ps[variant].size()};
      d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      d.SampleMask = UINT_MAX;
      d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      d.RasterizerState.DepthClipEnable = TRUE;
      d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      if (variant & 4)
      {
        d.DSVFormat = format;
        auto &dss = d.DepthStencilState;
        if (!stencil_bit_index)
        {
          dss.DepthEnable = TRUE;
          dss.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
          dss.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        }
        else
        {
          dss.StencilEnable = TRUE;
          dss.StencilReadMask = 0xFF;
          dss.StencilWriteMask = uint8_t(1u << (stencil_bit_index - 1));
          dss.FrontFace = {D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE,
                           D3D12_COMPARISON_FUNC_ALWAYS};
          dss.BackFace = dss.FrontFace;
        }
      }
      else
      {
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = format;
      }
      d.SampleDesc.Count = samples;
      ComPtr<ID3D12PipelineState> pso;
      if (FAILED(g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso))))
      {
        NativeLog("transfer pipeline failed (variant %u format %u samples %u)\n", variant, unsigned(format), samples);
      }
      g.transfer_pipelines.emplace(key, pso);
      return pso.Get();
    }

    // Copies the tiles of `rect` currently owned by `src` into `dst`.
    // `do_depth` / `do_stencil` select the parts of a depth destination;
    // `stencil_rects` (host pixels) are the source's tiles, where the
    // stencil is cleared before the per-bit passes.
    void TransferTiles(RenderTarget &dst, RenderTarget &src, const D3D12_RECT &rect, D3D12_GPU_VIRTUAL_ADDRESS owners,
                       bool do_depth = true, bool do_stencil = true,
                       const std::vector<D3D12_RECT> *stencil_rects = nullptr)
    {
      if (!g.lent.empty())
      {
        ReturnLent(&src);
        ReturnLent(&dst);
      }
      Frame &f = CurrentFrame();
      if (f.views_used + 2 > kViewsPerFrame)
      {
        return;
      }
      uint32_t variant = (dst.depth ? 4u : 0u) | (src.depth ? 2u : 0u) | (src.samples > 1 ? 1u : 0u);
      ID3D12PipelineState *pso = GetTransferPipeline(variant, dst.format, dst.samples, 0);
      if (!pso)
      {
        return;
      }
      uint32_t slot = g.frame_index * kViewsPerFrame + f.views_used;
      f.views_used += 2;
      D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.view_heap->GetCPUDescriptorHandleForHeapStart();
      cpu.ptr += uint64_t(slot) * g.view_size;
      D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.view_heap->GetGPUDescriptorHandleForHeapStart();
      gpu.ptr += uint64_t(slot) * g.view_size;
      bool ms = src.samples > 1;
      D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.ViewDimension = ms ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
      sd.Texture2D.MipLevels = 1;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      if (src.depth)
      {
        bool f32 = src.format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        sd.Format = f32 ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        g.device->CreateShaderResourceView(src.resource.Get(), &sd, cpu);
        sd.Format = f32 ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        if (!ms)
        {
          sd.Texture2D.PlaneSlice = 1;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu;
        cpu1.ptr += g.view_size;
        g.device->CreateShaderResourceView(src.resource.Get(), &sd, cpu1);
      }
      else
      {
        sd.Format = src.format;
        g.device->CreateShaderResourceView(src.resource.Get(), &sd, cpu);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu;
        cpu1.ptr += g.view_size;
        g.device->CopyDescriptorsSimple(1, cpu1, g.null_srv_2d, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      }
      EdramLayout dl = GetEdramLayout(dst), sl = GetEdramLayout(src);
      uint32_t constants[13] = {dl.base,      dl.pitch_tiles, dst.samples,                 dst.edram_format,
                                sl.base,      sl.pitch_tiles, src.samples,                 src.edram_format,
                                src.edram_id, uint32_t(src.depth != dst.depth), 0, dst.scale4 | (src.scale4 << 8),
                                dl.tile_w};
      Transition(src.resource.Get(), src.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      Transition(dst.resource.Get(), dst.state,
                 dst.depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
      auto *list = g.list.Get();
      if (dst.depth)
      {
        g.bound.valid = false;
        list->OMSetRenderTargets(0, nullptr, FALSE, &dst.view);
      }
      else
      {
        g.bound.valid = false;
        list->OMSetRenderTargets(1, &dst.view, FALSE, nullptr);
      }
      D3D12_VIEWPORT vp = {0.0f, 0.0f, float(ScaleSize(dst.width, dst.scale4)),
                           float(ScaleSize(dst.height, dst.scale4)), 0.0f, 1.0f};
      D3D12_RECT host_rect = ScaleRect(rect, dst.scale4);
      list->RSSetViewports(1, &vp);
      list->RSSetScissorRects(1, &host_rect);
      list->SetGraphicsRootSignature(g.transfer_root.Get());
      list->SetPipelineState(pso);
      list->SetGraphicsRootDescriptorTable(0, gpu);
      list->SetGraphicsRoot32BitConstants(1, 13, constants, 0);
      list->SetGraphicsRootShaderResourceView(2, owners);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      GpuMark(kGpuTransfer);
      Crumb("transfer", uint64_t(src.resource.Get()), uint64_t(dst.resource.Get()), variant,
            (uint64_t(rect.right) << 32) | uint64_t(rect.bottom));
      if (!dst.depth || do_depth)
      {
        list->DrawInstanced(3, 1, 0, 0);
      }
      if (dst.depth && do_stencil)
      {
        // Stencil: cleared, then one pass per bit (no shader stencil export).
        Crumb("transfer stencil clear", uint64_t(dst.resource.Get()), uint64_t(rect.right), uint64_t(rect.bottom));
        if (stencil_rects && !stencil_rects->empty())
        {
          ClearDsvRects(dst.view, D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, *stencil_rects);
        }
        else if (RectFits(dst.resource.Get(), host_rect, "transfer stencil clear"))
        {
          list->ClearDepthStencilView(dst.view, D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 1, &host_rect);
        }
        list->OMSetStencilRef(0xFF);
        for (uint32_t bit = 0; bit < 8; ++bit)
        {
          ID3D12PipelineState *stencil_pso = GetTransferPipeline(variant, dst.format, dst.samples, bit + 1);
          if (!stencil_pso)
          {
            break;
          }
          constants[10] = 1u << bit;
          list->SetPipelineState(stencil_pso);
          list->SetGraphicsRoot32BitConstants(1, 13, constants, 0);
          list->DrawInstanced(3, 1, 0, 0);
        }
      }
      ++g.transfers;
      Trace("  transfer -> %s base %u fmt %u pitch %u x%u <- %s base %u fmt %u pitch %u x%u rect %d,%d-%d,%d\n",
            dst.depth ? "depth" : "color", dst.edram_base, dst.edram_format, dst.edram_pitch, dst.samples,
            src.depth ? "depth" : "color", src.edram_base, src.edram_format, src.edram_pitch, src.samples,
            int(rect.left), int(rect.top), int(rect.right), int(rect.bottom));
    }

    // True if `rect` of `rt` consists of whole EDRAM tiles (up to the surface edge).
    bool TileAligned(const RenderTarget &rt, const D3D12_RECT &rect)
    {
      EdramLayout l = GetEdramLayout(rt);
      auto aligned = [](LONG v, LONG edge, uint32_t scale, uint32_t tile) {
        return v >= edge || (uint32_t(v) * scale) % tile == 0;
      };
      return aligned(rect.left, LONG(rt.width), l.scale_x, l.tile_w) &&
             aligned(rect.right, LONG(rt.width), l.scale_x, l.tile_w) &&
             aligned(rect.top, LONG(rt.height), l.scale_y, 16) && aligned(rect.bottom, LONG(rt.height), l.scale_y, 16);
    }

    // Brings what `src` left in `tiles` (owned by it, inside `clip`) into
    // `dst`: the parts in `needed` (1 = color / depth, 2 = stencil). Tiles
    // holding a known uniform value are cleared; only the others are
    // transferred, over their bounding rectangle.
    void TransferOrClear(RenderTarget &dst, RenderTarget &src, const std::vector<uint32_t> &tiles,
                         const D3D12_RECT &clip, uint32_t needed)
    {
      if (tiles.empty() || !needed)
      {
        return;
      }
      ++dst.writes;
      static std::vector<uint32_t> clear_tiles, pass_tiles;
      static std::vector<D3D12_RECT> rects;
      clear_tiles.clear();
      pass_tiles.clear();
      bool can_clear = !src.is_64bpp && !dst.is_64bpp && (src.depth || UniformColorFormat(src.edram_format)) &&
                       (dst.depth || UniformColorFormat(dst.edram_format));
      // Known bits of the raw cell: 8..31 (color / depth) and 0..7 (stencil).
      auto high_known = [&](uint32_t tile) { return src.uniform_tiles[0][tile]; };
      auto low_known = [&](uint32_t tile) { return src.uniform_tiles[src.depth ? 1 : 0][tile]; };
      bool want_high = !dst.depth || (needed & 1), want_low = !dst.depth || (needed & 2);
      for (uint32_t tile : tiles)
      {
        bool known = can_clear && (!want_high || high_known(tile)) && (!want_low || low_known(tile));
        (known ? clear_tiles : pass_tiles).push_back(tile);
      }
      uint32_t raw = src.depth ? ((src.uniform_value[0] & ~0xFFu) | (src.uniform_value[1] & 0xFFu)) : src.uniform_value[0];
      if (!clear_tiles.empty())
      {
        TileRects(dst, clear_tiles, clip, rects);
        GpuMark(kGpuClear);
        if (!dst.depth)
        {
          float c[4];
          UnpackColorRaw(raw, dst.edram_format, c);
          Transition(dst.resource.Get(), dst.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
          ClearRtvRects(dst.view, c, rects);
          SetUniform(dst, 0, clear_tiles, raw);
        }
        else
        {
          Transition(dst.resource.Get(), dst.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
          D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAGS(((needed & 1) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                      ((needed & 2) ? D3D12_CLEAR_FLAG_STENCIL : 0));
          ClearDsvRects(dst.view, flags, DecodeDepth24(raw >> 8, dst.edram_format), uint8_t(raw & 0xFF), rects);
          if (needed & 1)
          {
            SetUniform(dst, 0, clear_tiles, raw & ~0xFFu);
          }
          if (needed & 2)
          {
            SetUniform(dst, 1, clear_tiles, raw & 0xFFu);
          }
        }
        ++g.transfers_cleared;
      }
      if (pass_tiles.empty())
      {
        return;
      }
      // The rest: a real transfer limited to these tiles (owner snapshot with
      // only them) and their bounding rectangle.
      void *mapped = nullptr;
      D3D12_GPU_VIRTUAL_ADDRESS owners = Upload(nullptr, kEdramTiles * 4, 256, &mapped);
      if (!owners)
      {
        return;
      }
      auto *ids = static_cast<uint32_t *>(mapped);
      std::memset(ids, 0, kEdramTiles * 4);
      D3D12_RECT bounds = TileRect(dst, pass_tiles[0]);
      for (uint32_t tile : pass_tiles)
      {
        ids[tile] = src.edram_id;
        D3D12_RECT t = TileRect(dst, tile);
        bounds = {std::min(bounds.left, t.left), std::min(bounds.top, t.top), std::max(bounds.right, t.right),
                  std::max(bounds.bottom, t.bottom)};
      }
      bounds = {std::max(bounds.left, clip.left), std::max(bounds.top, clip.top), std::min(bounds.right, clip.right),
                std::min(bounds.bottom, clip.bottom)};
      if (bounds.left >= bounds.right || bounds.top >= bounds.bottom)
      {
        return;
      }
      if (!dst.depth)
      {
        TransferTiles(dst, src, bounds, owners);
        for (uint32_t tile : pass_tiles)
        {
          dst.uniform_tiles[0].reset(tile);
        }
        return;
      }
      // Depth destination: parts uniform in all of these tiles are cleared.
      bool high_all = can_clear, low_all = can_clear;
      for (uint32_t tile : pass_tiles)
      {
        high_all = high_all && high_known(tile);
        low_all = low_all && low_known(tile);
      }
      bool pass_depth = (needed & 1) && !high_all, pass_stencil = (needed & 2) && !low_all;
      TileRects(dst, pass_tiles, clip, rects);
      TransferTiles(dst, src, bounds, owners, pass_depth, pass_stencil, &rects);
      if (((needed & 1) && high_all) || ((needed & 2) && low_all))
      {
        GpuMark(kGpuClear);
        D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAGS((((needed & 1) && high_all) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                    (((needed & 2) && low_all) ? D3D12_CLEAR_FLAG_STENCIL : 0));
        ClearDsvRects(dst.view, flags, DecodeDepth24(raw >> 8, dst.edram_format), uint8_t(raw & 0xFF), rects);
      }
      for (uint32_t c = 0; c < 2; ++c)
      {
        if (!(needed & (1u << c)))
        {
          continue;
        }
        bool known = c == 0 ? high_all : low_all;
        if (known)
        {
          SetUniform(dst, c, pass_tiles, c == 0 ? (raw & ~0xFFu) : (raw & 0xFFu));
        }
        else
        {
          for (uint32_t tile : pass_tiles)
          {
            dst.uniform_tiles[c].reset(tile);
          }
        }
      }
    }

    // A copy draw putting back what a resolve saved (EdramSave). The hit
    // effect saves the scene's EDRAM bits into a texture, lets a 4x MSAA
    // depth/stencil surface use that EDRAM, then draws the texture back texel
    // for texel. The render target that held those bits still holds them if
    // nothing wrote it since: it gets the EDRAM tiles back instead of the
    // draw - and instead of the transfer of the draw's result into it when the
    // next draw wants the scene in its own format again (~0.3 ms at 1440p).
    // Coming from a 7e3 float surface, it keeps its 16-bit floats rather than
    // the 7e3 rounding the console's round trip makes.
    bool SkipCopyBack(const RegisterFile &r, const CompiledShader *vs, const CompiledShader *ps, const DrawPacket &d,
                      const Viewport &vp, RenderTarget *const color_rts[4], uint32_t rtv_count, const RenderTarget *ds,
                      uint32_t color_mask, uint32_t blend0, uint32_t rt_format0, const D3D12_RECT &covered)
    {
      const auto &s = g.edram_save;
      if (!s.origin || rtv_count != 1 || color_rts[0] != s.surface || ds || g.occlusion_active || !ps ||
          !ps->plain_texture_copy || ps->shader->kills_pixels() || r.Get<reg::RB_COLORCONTROL>().alpha_test_enable ||
          (color_mask & 0xF) != 0xF || blend0 != 0 || rt_format0 == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
          covered.left != s.rect.left || covered.top != s.rect.top || covered.right != s.rect.right ||
          covered.bottom != s.rect.bottom || s.origin->writes != s.origin_writes)
      {
        return false;
      }
      // The saved texture, unchanged since, read as it was stored: same bits
      // per texel, no swizzle, sign, exponent or filtering.
      auto fetch = r.Get<xenos::xe_gpu_texture_fetch_t>(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
      bool same_bits = false;
      switch (s.surface->edram_format)
      {
      case uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8):
        same_bits = fetch.format == xenos::TextureFormat::k_8_8_8_8 ||
                    fetch.format == xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16;
        break;
      case uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10):
        same_bits = fetch.format == xenos::TextureFormat::k_2_10_10_10 ||
                    fetch.format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
        break;
      default:
        break;
      }
      LONG w = s.rect.right - s.rect.left, h = s.rect.bottom - s.rect.top;
      if (!same_bits || ((fetch.base_address & 0x1FFFF) << 12) != s.dest ||
          fetch.dimension != xenos::DataDimension::k2DOrStacked || LONG(fetch.size_2d.width) + 1 != w ||
          LONG(fetch.size_2d.height) + 1 != h || fetch.mag_filter != xenos::TextureFilter::kPoint ||
          fetch.min_filter != xenos::TextureFilter::kPoint || fetch.exp_adjust ||
          fetch.swizzle != xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA || fetch.sign_x != xenos::TextureSign::kUnsigned ||
          fetch.sign_y != xenos::TextureSign::kUnsigned || fetch.sign_z != xenos::TextureSign::kUnsigned ||
          fetch.sign_w != xenos::TextureSign::kUnsigned)
      {
        return false;
      }
      HostTexture *tex = GetResolvedTexture(s.dest);
      if (!tex || tex->generation != s.generation || tex->exp_scale != 1.0f || tex->red_blue_swapped)
      {
        return false;
      }
      // Texel for texel: the quad's corners on the pixel grid's edges and its
      // texture coordinates 0 / 1 at them.
      const auto &pf = vs->position_passthrough, &tf = vs->texcoord_passthrough;
      if (!pf.valid || !tf.valid || pf.format != xenos::VertexFormat::k_32_32_FLOAT || d.prim_type != xenos::PrimitiveType::kTriangleStrip || d.index_count != 4)
      {
        return false;
      }
      xenos::xe_gpu_vertex_fetch_t vf;
      std::memcpy(&vf, &r.v[0x4800 + pf.fetch_constant * 2], sizeof(vf));
      uint32_t base = (vf.address << 2) & 0x1FFFFFFF, first = r.v[XE_GPU_REG_VGT_INDX_OFFSET];
      if (!pf.stride || pf.stride != tf.stride || !IsGuestPhysicalReadable(base + first * pf.stride * 4, pf.stride * 16))
      {
        return false;
      }
      auto load = [&](uint32_t vertex, int32_t offset, uint32_t c) {
        uint32_t raw;
        std::memcpy(&raw, Mem()->TranslatePhysical(base + (first + vertex) * pf.stride * 4 + uint32_t(offset + c) * 4), 4);
        raw = SwapEndian(raw, vf.endian);
        float f;
        std::memcpy(&f, &raw, 4);
        return f;
      };
      float px[4], py[4], u[4], v[4];
      for (uint32_t i = 0; i < 4; ++i)
      {
        px[i] = vp.viewport.TopLeftX + ((load(i, pf.offset, 0) * vp.ndc_scale[0] + vp.ndc_offset[0]) * 0.5f + 0.5f) *
                                           vp.viewport.Width;
        py[i] = vp.viewport.TopLeftY + (0.5f - (load(i, pf.offset, 1) * vp.ndc_scale[1] + vp.ndc_offset[1]) * 0.5f) *
                                           vp.viewport.Height;
        u[i] = load(i, tf.offset, 0);
        v[i] = load(i, tf.offset, 1);
      }
      float x0 = *std::min_element(px, px + 4), x1 = *std::max_element(px, px + 4);
      float y0 = *std::min_element(py, py + 4), y1 = *std::max_element(py, py + 4);
      auto near_edge = [](float a, LONG edge) { return a >= float(edge) - 0.5f - 1e-3f && a <= float(edge) + 1e-3f; };
      if (!near_edge(x0, s.rect.left) || !near_edge(x1, s.rect.right) || !near_edge(y0, s.rect.top) ||
          !near_edge(y1, s.rect.bottom))
      {
        return false;
      }
      for (uint32_t i = 0; i < 4; ++i)
      {
        if (u[i] != (px[i] == x0 ? 0.0f : 1.0f) || v[i] != (py[i] == y0 ? 0.0f : 1.0f))
        {
          return false;
        }
      }
      ForEachTile(*s.origin, s.rect, [&](uint32_t tile) { g.edram_owner[tile] = s.origin; });
      ++g.edram_generation;
      Trace("    copy back of %08X skipped: its EDRAM goes back to the surface it was saved from\n", s.dest);
      g.edram_save = {};
      return true;
    }

    // The one render target owning every EDRAM tile `rect` of `rt` covers, if
    // it is a 32bpp color surface laid out like `rt` (null otherwise).
    RenderTarget *SoleOwner(RenderTarget &rt, const D3D12_RECT &rect)
    {
      RenderTarget *owner = nullptr;
      bool one = true;
      ForEachTile(rt, rect, [&](uint32_t tile) {
        RenderTarget *o = g.edram_owner[tile];
        one = one && o && (!owner || o == owner);
        owner = owner ? owner : o;
      });
      if (!one || !owner || owner->depth || owner->is_64bpp || rt.is_64bpp || owner->edram_base != rt.edram_base ||
          owner->edram_pitch != rt.edram_pitch || owner->samples != rt.samples || owner->scale4 != rt.scale4)
      {
        return nullptr;
      }
      return owner;
    }

    // Makes `rt` the owner of the EDRAM tiles under `rect`, first bringing in
    // the contents other render targets left there - except the parts the
    // caller overwrites (`overwrite`: 1 = color / depth, 2 = stencil) in the
    // tiles `rect` covers completely, and the stencil of depth surfaces no
    // draw ever tested.
    void ClaimEdram(RenderTarget &rt, D3D12_RECT rect, uint32_t overwrite = 0)
    {
      if (!g.lent.empty())
      {
        ReturnLent(&rt);
      }
      rect.left = std::max<LONG>(rect.left, 0);
      rect.top = std::max<LONG>(rect.top, 0);
      rect.right = std::min<LONG>(rect.right, LONG(rt.width));
      rect.bottom = std::min<LONG>(rect.bottom, LONG(rt.height));
      if (rect.left >= rect.right || rect.top >= rect.bottom)
      {
        return;
      }
      // Fast path: nothing changed hands since this render target claimed a
      // rectangle containing this one.
      if (rt.claim_generation == g.edram_generation && rect.left >= rt.claimed.left &&
          rect.top >= rt.claimed.top && rect.right <= rt.claimed.right && rect.bottom <= rt.claimed.bottom)
      {
        return;
      }
      RenderTarget *sources[8] = {};
      uint32_t source_count = 0;
      bool changed = false;
      ForEachTile(rt, rect, [&](uint32_t tile) {
        RenderTarget *owner = g.edram_owner[tile];
        if (owner == &rt)
        {
          return;
        }
        changed = true;
        if (owner && source_count < 8 && std::find(sources, sources + source_count, owner) == sources + source_count)
        {
          sources[source_count++] = owner;
        }
      });
      uint32_t needed = rt.depth ? (rt.stencil_used ? 3u : 1u) : 1u;
      // 64bpp surfaces exchange contents only with 64bpp surfaces of the same
      // format (no re-packing); otherwise they only take ownership.
      if (source_count)
      {
        static std::vector<uint32_t> inside, interior_tiles, edge_tiles;
        static std::bitset<2048> inside_mask;
        inside_mask.reset();
        if (overwrite)
        {
          TilesInside(rt, rect, inside);
          for (uint32_t tile : inside)
          {
            inside_mask.set(tile);
          }
        }
        for (uint32_t i = 0; i < source_count; ++i)
        {
          RenderTarget &src = *sources[i];
          if (src.is_64bpp != rt.is_64bpp ||
              (rt.is_64bpp && (src.depth || rt.depth || src.edram_format != rt.edram_format || src.format != rt.format)))
          {
            continue;
          }
          interior_tiles.clear();
          edge_tiles.clear();
          ForEachTile(rt, rect, [&](uint32_t tile) {
            if (g.edram_owner[tile] != &src)
            {
              return;
            }
            bool covered = inside_mask[tile];
            (covered ? interior_tiles : edge_tiles).push_back(tile);
          });
          if (!interior_tiles.empty() && !(needed & ~overwrite))
          {
            ++g.transfers_skipped;
          }
          TransferOrClear(rt, src, interior_tiles, rect, needed & ~overwrite);
          // An edge tile changes hands whole, so all of it comes over, not
          // only its part inside `rect`: the shadow buffer's border (cleared
          // through a 4x MSAA surface over the same EDRAM, then a draw claims
          // only the 5-texel-inset viewport) was otherwise left with stale
          // depth, and the shadow projection sampled it - dark lines along the
          // edges of a light's shadow.
          TransferOrClear(rt, src, edge_tiles, D3D12_RECT{0, 0, LONG(rt.width), LONG(rt.height)}, needed);
        }
      }
      if (changed)
      {
        Trace("  claim %s base %u fmt %u pitch %u x%u rect %d,%d-%d,%d from %u owners\n", rt.depth ? "depth" : "color",
              rt.edram_base, rt.edram_format, rt.edram_pitch, rt.samples, int(rect.left), int(rect.top),
              int(rect.right), int(rect.bottom), source_count);
        ForEachTile(rt, rect, [&](uint32_t tile) { g.edram_owner[tile] = &rt; });
        ++g.edram_generation;
      }
      rt.claim_generation = g.edram_generation;
      rt.claimed = rect;
    }

    // D3D clears stencil by drawing a rect list into a 4x MSAA depth surface
    // over the same EDRAM (stencil replaced, no color, no depth test). Claiming
    // that surface brings the other surface's depth over and later back: two
    // full-screen transfers for the scene's depth each frame. When whole tiles
    // of one other depth surface with the same base, format and pitch in
    // samples lie under `rect` of `alias`, its stencil is cleared there instead
    // and it keeps the tiles. False when that is not the case.
    bool ClearStencilOnOwner(RenderTarget &alias, const D3D12_RECT &rect, uint8_t stencil)
    {
      if (alias.is_64bpp)
      {
        return false;
      }
      static std::vector<uint32_t> inside;
      TilesInside(alias, rect, inside);
      RenderTarget *owner = nullptr;
      size_t tiles = 0;
      bool ok = true;
      ForEachTile(alias, rect, [&](uint32_t tile) {
        RenderTarget *o = g.edram_owner[tile];
        ++tiles;
        ok = ok && o && o != &alias && (!owner || o == owner);
        owner = o;
      });
      if (!ok || !owner || tiles != inside.size() || !owner->depth || owner->is_64bpp ||
          owner->edram_base != alias.edram_base || owner->edram_format != alias.edram_format ||
          GetEdramLayout(*owner).pitch_tiles != GetEdramLayout(alias).pitch_tiles)
      {
        return false;
      }
      static std::vector<D3D12_RECT> rects;
      TileRects(*owner, inside, D3D12_RECT{0, 0, LONG(owner->width), LONG(owner->height)}, rects);
      Trace("    stencil %u cleared on depth base %u fmt %u pitch %u x%u instead (%zu tiles)\n", unsigned(stencil),
            owner->edram_base, owner->edram_format, owner->edram_pitch, owner->samples, inside.size());
      Transition(owner->resource.Get(), owner->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      GpuMark(kGpuClear);
      ClearDsvRects(owner->view, D3D12_CLEAR_FLAG_STENCIL, 0.0f, stencil, rects);
      owner->stencil_used = true;
      SetUniform(*owner, 1, inside, stencil);
      return true;
    }

    // The scene resolve clears depth and stencil, and the game draws the
    // depth back right away from the resolved texture (VS 36F9E781, PS
    // B160839A writing oDepth over the whole viewport; its vertex shader
    // makes the position, so the rect list's extent is not known): the depth
    // ends where it was (and loses its low bits on the way through the 24-bit
    // texture). The resolve clears
    // the stencil at once but leaves the depth clear pending: the next draw
    // that is that restore is skipped along with the clear; anything else
    // first does the clear.
    void FlushPendingDepthClear()
    {
      auto &p = g.pending_depth;
      if (!p.rt)
      {
        return;
      }
      RenderTarget &rt = *p.rt;
      Transition(rt.resource.Get(), rt.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      GpuMark(kGpuClear);
      D3D12_RECT host = ScaleRect(p.rect, rt.scale4);
      if (RectFits(rt.resource.Get(), host, "pending depth clear"))
      {
        g.list->ClearDepthStencilView(rt.view, D3D12_CLEAR_FLAG_DEPTH, std::min(p.z, 1.0f), 0, 1, &host);
        ForgetUniform(rt, 0, p.rect);
        static std::vector<uint32_t> inside;
        TilesInside(rt, p.rect, inside);
        SetUniform(rt, 0, inside, p.raw & ~0xFFu);
      }
      p = {};
    }

    // ------------------------------------------------------------ Resolve write back

    // Scaled destinations up to this size (guest texels) are written back
    // from their guest-sized copies; of larger ones (a full pass each for a
    // few texels) the host texel at each texel's centre is taken.
    constexpr uint32_t kWritebackReducedTexels = 256 * 256;

    // Copies texels [x0, x1) x [y0, y1) of a resolve destination (of scale
    // `scale4`) to the frame's readback buffer, to be stored to guest memory
    // when done.
    void SnapshotWriteback(Writeback &wb);

    void QueueWriteback(ID3D12Resource *texture, D3D12_RESOURCE_STATES &state, DXGI_FORMAT format, uint32_t scale4,
                        uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t dest_base, uint32_t dest_pitch,
                        uint32_t bytes_per_texel, xenos::Endian128 endian)
    {
      Frame &f = CurrentFrame();
      D3D12_RECT host = ScaleRect(D3D12_RECT{LONG(x0), LONG(y0), LONG(x1), LONG(y1)}, scale4);
      uint32_t w = uint32_t(host.right - host.left), h = uint32_t(host.bottom - host.top);
      uint32_t row_pitch = (w * bytes_per_texel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                           ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
      uint64_t offset = (f.readback_offset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                        ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
      if (offset + uint64_t(row_pitch) * h > kReadbackSize)
      {
        return;
      }
      f.readback_offset = offset + uint64_t(row_pitch) * h;
      Transition(texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst = {f.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint.Offset = offset;
      dst.PlacedFootprint.Footprint = {format, w, h, 1, row_pitch};
      D3D12_TEXTURE_COPY_LOCATION src = {texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_BOX box = {UINT(host.left), UINT(host.top), 0, UINT(host.right), UINT(host.bottom), 1};
      GpuMark(kGpuCopy);
      Crumb("writeback copy", uint64_t(texture), format, (uint64_t(x1) << 32) | x0, (uint64_t(y1) << 32) | y0);
      if (!BoxFits(texture, box, "writeback copy"))
      {
        f.readback_offset = offset;
        return;
      }
      g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
      f.writebacks.push_back({offset, row_pitch, x0, y0, x1 - x0, y1 - y0, dest_base, dest_pitch, bytes_per_texel,
                              endian, scale4, uint32_t(host.left), uint32_t(host.top), {}, {}});
      SnapshotWriteback(f.writebacks.back());
      ++g.writebacks;
    }

    uint32_t SwapWord(uint32_t v, xenos::Endian128 endian)
    {
      switch (endian)
      {
      case xenos::Endian128::k8in16: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
      case xenos::Endian128::k8in32: return __builtin_bswap32(v);
      case xenos::Endian128::k16in32: return (v >> 16) | (v << 16);
      default: return v;
      }
    }

    // Guest address of texel (x, y) of a write-back (tiled destination).
    uint32_t WritebackAddress(const Writeback &wb, uint32_t x, uint32_t y)
    {
      uint32_t bpp_log2 = wb.bytes_per_texel == 16 ? 4 : (wb.bytes_per_texel == 8 ? 3 : (wb.bytes_per_texel == 2 ? 1 : 2));
      uint32_t pitch_aligned = (std::max(wb.dest_pitch, 1u) + 31) & ~31u;
      return wb.dest_base +
             uint32_t(texture_util::GetTiledOffset2D(int32_t(wb.x0 + x), int32_t(wb.y0 + y), pitch_aligned, bpp_log2));
    }

    // The tiled addresses of a write-back's texels and their byte range, by
    // its layout (the same resolves come every frame).
    struct WritebackLayout
    {
      std::vector<uint32_t> addresses;
      uint32_t lowest = UINT32_MAX, highest = 0;
    };
    std::map<std::array<uint32_t, 7>, WritebackLayout> g_writeback_layouts;

    const WritebackLayout &GetWritebackLayout(const Writeback &wb)
    {
      std::array<uint32_t, 7> key = {wb.dest_base, wb.dest_pitch, wb.x0, wb.y0, wb.width, wb.height, wb.bytes_per_texel};
      auto it = g_writeback_layouts.find(key);
      if (it != g_writeback_layouts.end())
      {
        return it->second;
      }
      if (g_writeback_layouts.size() >= 256)
      {
        g_writeback_layouts.clear();
      }
      WritebackLayout &l = g_writeback_layouts[key];
      l.addresses.resize(size_t(wb.width) * wb.height);
      for (uint32_t y = 0, i = 0; y < wb.height; ++y)
      {
        for (uint32_t x = 0; x < wb.width; ++x, ++i)
        {
          uint32_t address = WritebackAddress(wb, x, y);
          l.addresses[i] = address;
          l.lowest = std::min(l.lowest, address);
          l.highest = std::max(l.highest, address + wb.bytes_per_texel);
        }
      }
      return l;
    }

    // N = bytes per texel when known at compile time (fixed-size copies and
    // compares instead of library calls per texel), 0 = wb.bytes_per_texel.
    template <uint32_t N>
    void SnapshotTexels(Writeback &wb, bool all)
    {
      const uint32_t n = N ? N : wb.bytes_per_texel;
      rex::memory::Memory *mem = Mem();
      uint8_t *out = wb.before.data();
      for (uint32_t &address : wb.addresses)
      {
        if (all || IsGuestPhysicalReadable(address, n))
        {
          std::memcpy(out, mem->TranslatePhysical(address), n);
        }
        else
        {
          address = UINT32_MAX;
        }
        out += n;
      }
    }

    // Texel addresses and what the destination holds now, texel by texel
    // (unreadable texels: zeros). Readability is checked once for the whole
    // range when it is all there (the usual case), texel by texel otherwise.
    void SnapshotWriteback(Writeback &wb)
    {
      const WritebackLayout &l = GetWritebackLayout(wb);
      size_t texels = l.addresses.size();
      wb.addresses = l.addresses;
      wb.before.assign(texels * wb.bytes_per_texel, 0);
      bool all = texels && IsGuestPhysicalReadable(l.lowest, l.highest - l.lowest);
      switch (wb.bytes_per_texel)
      {
      case 4: SnapshotTexels<4>(wb, all); break;
      case 8: SnapshotTexels<8>(wb, all); break;
      case 16: SnapshotTexels<16>(wb, all); break;
      default: SnapshotTexels<0>(wb, all); break;
      }
      wb.lowest = texels ? l.lowest : 0;
      wb.highest = texels ? l.highest : 0;
    }

    // Bytes the last write-back to each destination stored (texel order): the
    // next frames' resolves to the same place were recorded before it landed.
    std::unordered_map<uint32_t, std::vector<uint8_t>> g_written_back;

    // One write-back's texels (see ApplyWritebacks). `record` holds what is
    // to be remembered as ours, texel by texel; with `ours` it is the same
    // buffer (each texel is compared before it is overwritten). Returns
    // whether a texel had been changed by the CPU.
    template <uint32_t N>
    bool ApplyTexels(const Writeback &wb, const uint8_t *readback, const uint8_t *ours, uint8_t *record)
    {
      const uint32_t n = N ? N : wb.bytes_per_texel;
      rex::memory::Memory *mem = Mem();
      thread_local std::vector<uint32_t> columns;
      columns.resize(wb.width);
      for (uint32_t x = 0; x < wb.width; ++x)
      {
        columns[x] = (CenterPixel(wb.x0 + x, wb.scale4) - wb.host_x0) * n;
      }
      const uint32_t *address = wb.addresses.data();
      const uint8_t *before = wb.before.data();
      bool stale = false;
      for (uint32_t y = 0; y < wb.height; ++y)
      {
        const uint8_t *row = readback + wb.offset + uint64_t(CenterPixel(wb.y0 + y, wb.scale4) - wb.host_y0) * wb.row_pitch;
        for (uint32_t x = 0; x < wb.width; ++x, ++address, before += n, record += n, ours = ours ? ours + n : nullptr)
        {
          if (*address == UINT32_MAX)
          {
            std::memcpy(record, before, n);
            continue;
          }
          uint8_t *dst = mem->TranslatePhysical(*address);
          if (std::memcmp(dst, before, n) != 0 && (!ours || std::memcmp(dst, ours, n) != 0))
          {
            ++g.writeback_stale;
            stale = true;
            std::memcpy(record, before, n);
            continue;
          }
          const uint8_t *src = row + columns[x];
          for (uint32_t k = 0; k + 4 <= n; k += 4)
          {
            uint32_t v;
            std::memcpy(&v, src + k, 4);
            v = SwapWord(v, wb.endian);
            std::memcpy(dst + k, &v, 4);
          }
          std::memcpy(record, dst, n);
        }
      }
      return stale;
    }

    // Stores the read back texels of a completed frame into guest memory
    // (tiled, endian-swapped like the GPU's resolve would have). This runs
    // frames after the guest saw the resolve done: texels the CPU changed in
    // the meantime (the memory was freed and reused, e.g. by the heap) keep
    // the CPU's data. Unchanged = as recorded, or as our last write-back left it.
    void ApplyWritebacks(Frame &f)
    {
      for (const Writeback &wb : f.writebacks)
      {
        // The memory may have been released since the resolve.
        if (wb.highest > wb.lowest && !IsGuestPhysicalReadable(wb.lowest, wb.highest - wb.lowest))
        {
          continue;
        }
        std::vector<uint8_t> &last = g_written_back[wb.dest_base];
        const uint8_t *ours = nullptr;
        if (last.size() == wb.before.size())
        {
          ours = last.data();
        }
        else
        {
          last = wb.before;
        }
        // Read back at another scale than the guest's (see
        // kWritebackReducedTexels): of a texel's host texels the one nearest
        // to its centre is stored.
        bool stale;
        switch (wb.bytes_per_texel)
        {
        case 4: stale = ApplyTexels<4>(wb, f.readback_mapped, ours, last.data()); break;
        case 8: stale = ApplyTexels<8>(wb, f.readback_mapped, ours, last.data()); break;
        case 16: stale = ApplyTexels<16>(wb, f.readback_mapped, ours, last.data()); break;
        default: stale = ApplyTexels<0>(wb, f.readback_mapped, ours, last.data()); break;
        }
        // After a CPU change nothing counts as ours any more.
        if (stale)
        {
          last.clear();
        }
      }
      f.writebacks.clear();
      f.readback_offset = 0;
    }

    // ------------------------------------------------------------ Resolve helpers

    // Host format of a resolve destination (the guest texture format the
    // resolve writes): resource (possibly typeless), views for unsigned and
    // signed sampling, the format the resolve pass renders with, and the
    // swizzle that replicates missing components like the guest formats do.
    struct ResolveFormat
    {
      DXGI_FORMAT resource, unorm, snorm, rtv;
      uint32_t swizzle;
      bool fixed16; // 16-bit normalized: the view depends on the copy number format
    };

    ResolveFormat GetResolveFormat(uint32_t dest_format, bool depth)
    {
      const uint32_t rrrr = xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, rggg = xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
                     rgbb = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB, rgba = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
      if (depth)
      {
        return {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT, rrrr, false};
      }
      using T = xenos::TextureFormat;
      switch (T(dest_format))
      {
      case T::k_8:
      case T::k_8_A:
      case T::k_8_B:
        return {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8_UNORM, rrrr, false};
      case T::k_8_8:
        return {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8_UNORM,
                rggg, false};
      case T::k_2_10_10_10:
      case T::k_2_10_10_10_AS_16_16_16_16:
        return {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN,
                DXGI_FORMAT_R10G10B10A2_UNORM, rgba, false};
      case T::k_10_11_11:
      case T::k_11_11_10:
      case T::k_10_11_11_AS_16_16_16_16:
      case T::k_11_11_10_AS_16_16_16_16:
        return {DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_UNKNOWN,
                DXGI_FORMAT_R16G16B16A16_UNORM, rgbb, false};
      case T::k_16:
        return {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16_SNORM, rrrr,
                true};
      case T::k_16_16:
        return {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM,
                DXGI_FORMAT_R16G16_SNORM, rggg, true};
      case T::k_16_16_16_16:
        return {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM,
                DXGI_FORMAT_R16G16B16A16_SNORM, rgba, true};
      case T::k_16_FLOAT:
        return {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16_FLOAT, rrrr, false};
      case T::k_16_16_FLOAT:
        return {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16_FLOAT,
                rggg, false};
      case T::k_16_16_16_16_FLOAT:
        return {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN,
                DXGI_FORMAT_R16G16B16A16_FLOAT, rgba, false};
      case T::k_32_FLOAT:
        return {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT, rrrr, false};
      case T::k_32_32_FLOAT:
        return {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32G32_FLOAT,
                rggg, false};
      case T::k_32_32_32_32_FLOAT:
        return {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_UNKNOWN,
                DXGI_FORMAT_R32G32B32A32_FLOAT, rgba, false};
      default: // k_8_8_8_8 and anything unusual
        return {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM,
                DXGI_FORMAT_R8G8B8A8_UNORM, rgba, false};
      }
    }

    // Resolve pass: one fullscreen triangle per destination, the scissor
    // limits it to the resolved rectangle; source texel = destination pixel.
    const char kResolveShader[] = R"(
cbuffer c : register(b0) { float scale; uint sample_mask; uint sample_count; uint swap_red_blue; int2 dest_offset; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
#if VARIANT == 0
Texture2D<float4> src : register(t0);
float4 ps(float4 p : SV_Position) : SV_Target {
  float4 c = src.Load(int3(int2(p.xy) - dest_offset, 0)) * scale;
  return swap_red_blue != 0u ? c.bgra : c;
}
#elif VARIANT == 1
Texture2DMS<float4> src : register(t0);
float4 ps(float4 p : SV_Position) : SV_Target {
  float4 sum = 0.0; uint n = 0u;
  [unroll] for (uint i = 0u; i < 4u; ++i) {
    if (i < sample_count && ((sample_mask >> i) & 1u) != 0u) { sum += src.Load(int2(p.xy) - dest_offset, i); ++n; }
  }
  float4 c = sum / float(max(n, 1u)) * scale;
  return swap_red_blue != 0u ? c.bgra : c;
}
#elif VARIANT == 2
Texture2D<float> src : register(t0);
float ps(float4 p : SV_Position) : SV_Target { return src.Load(int3(int2(p.xy) - dest_offset, 0)); }
#elif VARIANT == 3
Texture2DMS<float> src : register(t0);
float ps(float4 p : SV_Position) : SV_Target { return src.Load(int2(p.xy) - dest_offset, 0); }
#elif VARIANT == 4
// Box downscale by the resolution scale (sample_count = scale in quarters):
// guest-sized copy of a scaled resolve destination. Each guest pixel is the
// average of the host pixels under it, weighted by how much of them it covers
// (in quarters: at 1.5x a whole pixel and half of the one shared with the
// neighbour).
Texture2D<float4> src : register(t0);
float4 ps(float4 p : SV_Position) : SV_Target {
  int s = int(sample_count);
  int2 lo = int2(p.xy) * s, hi = lo + s;
  float4 sum = 0.0;
  for (int y = lo.y >> 2; y * 4 < hi.y; ++y) {
    float wy = float(min(y * 4 + 4, hi.y) - max(y * 4, lo.y));
    for (int x = lo.x >> 2; x * 4 < hi.x; ++x) {
      sum += src.Load(int3(x, y, 0)) * (wy * float(min(x * 4 + 4, hi.x) - max(x * 4, lo.x)));
    }
  }
  return sum / float(s * s);
}
#else
// A 7e3 float surface read as 2_10_10_10 bits (both share the EDRAM cells):
// each channel's 7e3 encoding as a 10-bit unorm (the transfer's PackColor).
Texture2D<float4> src : register(t0);
uint Encode7e3(float f) {
  uint b = asuint(clamp(f, 0.0, 31.875));
  uint biased = b < 0x3E800000u ? ((b & 0x7FFFFFu) | 0x800000u) >> min(125u - (b >> 23), 24u) : b + 0xC2000000u;
  return ((biased + 0x7FFFu + ((biased >> 16) & 1u)) >> 16) & 0x3FFu;
}
float4 ps(float4 p : SV_Position) : SV_Target {
  float4 c = src.Load(int3(int2(p.xy) - dest_offset, 0));
  return float4(float(Encode7e3(c.r)), float(Encode7e3(c.g)), float(Encode7e3(c.b)), round(saturate(c.a) * 3.0)) /
         float4(1023.0, 1023.0, 1023.0, 3.0);
}
#endif
)";

    bool InitResolvePipelines()
    {
      D3D12_DESCRIPTOR_RANGE range = {};
      range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      range.NumDescriptors = 1;
      D3D12_ROOT_PARAMETER p[2] = {};
      p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      p[0].DescriptorTable.NumDescriptorRanges = 1;
      p[0].DescriptorTable.pDescriptorRanges = &range;
      p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      p[1].Constants.Num32BitValues = 6;
      p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      D3D12_ROOT_SIGNATURE_DESC desc = {};
      desc.NumParameters = 2;
      desc.pParameters = p;
      ComPtr<ID3DBlob> blob, error;
      if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
          FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&g.resolve_root))))
      {
        return false;
      }
      g.resolve_vs = Compile(kResolveShader, "vs", "vs_5_1");
      for (uint32_t variant = 0; variant < 6; ++variant)
      {
        char value[2] = {char('0' + variant), 0};
        D3D_SHADER_MACRO macros[] = {{"VARIANT", value}, {nullptr, nullptr}};
        ComPtr<ID3DBlob> code, errors;
        if (FAILED(D3DCompile(kResolveShader, std::strlen(kResolveShader), nullptr, macros, nullptr, "ps", "ps_5_1",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
        {
          NativeLog("resolve shader %u failed: %s\n", variant,
                    errors ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
          return false;
        }
        auto *b = static_cast<const uint8_t *>(code->GetBufferPointer());
        g.resolve_ps[variant].assign(b, b + code->GetBufferSize());
      }
      return !g.resolve_vs.empty();
    }

    ID3D12PipelineState *GetResolvePipeline(uint32_t variant, DXGI_FORMAT format)
    {
      uint32_t key = (variant << 8) | uint32_t(format);
      auto it = g.resolve_pipelines.find(key);
      if (it != g.resolve_pipelines.end())
      {
        return it->second.Get();
      }
      D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
      d.pRootSignature = g.resolve_root.Get();
      d.VS = {g.resolve_vs.data(), g.resolve_vs.size()};
      d.PS = {g.resolve_ps[variant].data(), g.resolve_ps[variant].size()};
      d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      d.SampleMask = UINT_MAX;
      d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      d.RasterizerState.DepthClipEnable = TRUE;
      d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      d.NumRenderTargets = 1;
      d.RTVFormats[0] = format;
      d.SampleDesc.Count = 1;
      ComPtr<ID3D12PipelineState> pso;
      if (FAILED(g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso))))
      {
        NativeLog("resolve pipeline failed (variant %u format %u)\n", variant, unsigned(format));
      }
      g.resolve_pipelines.emplace(key, pso);
      return pso.Get();
    }

    // Ends the lends (SwapResolve) of render target `rt` / of the texture at
    // `dest` before the render target is used or the texture changes: the
    // contents are copied back if the render target still holds EDRAM tiles
    // (otherwise nothing will read them).
    void ReturnLent(const State::Lent &l);
    void ReturnLent(RenderTarget *rt, uint32_t dest)
    {
      for (size_t i = 0; i < g.lent.size();)
      {
        if (g.lent[i].rt != rt && g.lent[i].dest != dest)
        {
          ++i;
          continue;
        }
        State::Lent l = g.lent[i];
        g.lent.erase(g.lent.begin() + i);
        ReturnLent(l);
      }
    }

    void ReturnLent(const State::Lent &l)
    {
      bool holds = false;
      ForEachTile(*l.rt, D3D12_RECT{0, 0, LONG(l.rt->width), LONG(l.rt->height)},
                  [&](uint32_t tile) { holds = holds || g.edram_owner[tile] == l.rt; });
      if (!holds)
      {
        return;
      }
      auto it = g.resolved.find(l.dest);
      HostTexture *t = it != g.resolved.end() ? it->second.get() : nullptr;
      if (!t || t->resource.Get() != l.resource)
      {
        NativeLog("lent render target contents lost (texture %08X changed)\n", l.dest);
        return;
      }
      Transition(t->resource.Get(), t->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      Transition(l.rt->resource.Get(), l.rt->state, D3D12_RESOURCE_STATE_COPY_DEST);
      GpuMark(kGpuCopy);
      Crumb("lent contents back", uint64_t(t->resource.Get()), uint64_t(l.rt->resource.Get()));
      g.list->CopyResource(l.rt->resource.Get(), t->resource.Get());
      ++l.rt->writes;
      ++g.lent_returned;
      Trace("  lent contents of color base %u fmt %u copied back from %08X\n", l.rt->edram_base, l.rt->edram_format,
            l.dest);
    }

    // Draws `rect` of the render target into the destination with the
    // exponent bias / sample averaging / format conversion applied, `dest_row`
    // guest rows lower (a predicated tile, see the resolve).
    // A color resolve with a clear: instead of copying the render target into
    // the texture and clearing it, the texture takes the render target's
    // resource and the render target the texture's old one (or a new one), which
    // the clear then fills. The texture is the render target's size (the guest
    // part is chosen by the UV scale) and gets the exponent bias at sampling,
    // exact for a power of two. Only where nothing else is in that resource:
    // `rect` from 0,0 and no EDRAM tile of the render target outside it.
    // `lend` (no clear): the render target keeps its contents, which stay in
    // the texture's resource until ReturnLent - its tiles outside `rect` too.
    bool SwapResolve(RenderTarget &src, HostTexture &tex, const D3D12_RECT &rect, float scale, bool lend = false)
    {
      if (rect.left || rect.top || src.samples != 1 || src.depth)
      {
        return false;
      }
      // Tiles the rectangle touches. One that it covers only in part lies on
      // its right or bottom edge (it starts at 0, 0): the part beyond is
      // outside the viewport (the surface pitch is a whole number of tiles,
      // 1152 wide -> 1200), never drawn, and sampling stays inside the
      // guest texture (the edge clamp of resolved textures).
      static std::bitset<2048> inside_mask;
      inside_mask.reset();
      ForEachTile(src, rect, [&](uint32_t tile) { inside_mask.set(tile); });
      bool outside = false;
      ForEachTile(src, D3D12_RECT{0, 0, LONG(src.width), LONG(src.height)}, [&](uint32_t tile) {
        outside = outside || (!lend && g.edram_owner[tile] == &src && !inside_mask[tile]);
      });
      if (outside)
      {
        return false;
      }
      D3D12_RESOURCE_DESC rd = src.resource->GetDesc();
      ComPtr<ID3D12Resource> spare = std::move(tex.resource);
      D3D12_RESOURCE_STATES spare_state = tex.state;
      if (spare)
      {
        D3D12_RESOURCE_DESC sd = spare->GetDesc();
        if (sd.Width != rd.Width || sd.Height != rd.Height || sd.Format != rd.Format || sd.Flags != rd.Flags ||
            sd.SampleDesc.Count != rd.SampleDesc.Count)
        {
          DeferRelease(std::move(spare));
        }
      }
      if (!spare)
      {
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
        spare_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ++g.resources_created;
        if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, spare_state, nullptr,
                                                     IID_PPV_ARGS(&spare))))
        {
          return false;
        }
      }
      tex.resource = std::move(src.resource);
      tex.state = src.state;
      tex.width = src.width;
      tex.height = src.height;
      tex.scale4 = src.scale4;
      tex.exp_scale = scale;
      src.resource = std::move(spare);
      src.state = spare_state;
      ++src.writes;
      D3D12_RENDER_TARGET_VIEW_DESC rv = {};
      rv.Format = src.format;
      rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      g.device->CreateRenderTargetView(src.resource.Get(), &rv, src.view);
      if (src.view_srgb.ptr != src.view.ptr)
      {
        rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        g.device->CreateRenderTargetView(src.resource.Get(), &rv, src.view_srgb);
      }
      // The command list's targets are views, rebound on the next draw.
      g.bound.valid = false;
      return true;
    }

    bool ResolveWithShader(RenderTarget &src, HostTexture &dest, const ResolveFormat &rf, const D3D12_RECT &rect,
                           float scale, xenos::CopySampleSelect sample_select, bool swap_red_blue = false,
                           uint32_t dest_row = 0, bool pack_7e3 = false)
    {
      Frame &f = CurrentFrame();
      if (f.views_used >= kViewsPerFrame)
      {
        return false;
      }
      bool ms = src.samples > 1;
      uint32_t variant = pack_7e3 ? 5u : (src.depth ? 2u : 0u) + (ms ? 1u : 0u);
      ID3D12PipelineState *pso = GetResolvePipeline(variant, rf.rtv);
      if (!pso)
      {
        return false;
      }
      uint32_t slot = g.frame_index * kViewsPerFrame + f.views_used++;
      D3D12_CPU_DESCRIPTOR_HANDLE srv = g.view_heap->GetCPUDescriptorHandleForHeapStart();
      srv.ptr += uint64_t(slot) * g.view_size;
      D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = g.view_heap->GetGPUDescriptorHandleForHeapStart();
      srv_gpu.ptr += uint64_t(slot) * g.view_size;
      D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = src.depth ? DepthSrvFormat(src.format) : src.format;
      sd.ViewDimension = ms ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
      sd.Texture2D.MipLevels = 1;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      g.device->CreateShaderResourceView(src.resource.Get(), &sd, srv);

      // Transient RTV: render target descriptors are consumed when recorded.
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.transient_rtv_heap->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += uint64_t(g.transient_rtvs_used++ % kTransientRtvCount) * g.rtv_size;
      D3D12_RENDER_TARGET_VIEW_DESC rd = {};
      rd.Format = rf.rtv;
      rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      g.device->CreateRenderTargetView(dest.resource.Get(), &rd, rtv);

      static const uint32_t kSampleMasks[] = {1, 2, 4, 8, 3, 12, 15, 15};
      int32_t offset_y = ScaleEdge(LONG(dest_row), dest.scale4);
      uint32_t constants[6];
      std::memcpy(&constants[0], &scale, 4);
      constants[1] = kSampleMasks[uint32_t(sample_select) & 7];
      constants[2] = src.samples;
      constants[3] = swap_red_blue ? 1u : 0u;
      constants[4] = 0;
      constants[5] = uint32_t(offset_y);

      Transition(src.resource.Get(), src.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      Transition(dest.resource.Get(), dest.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      auto *list = g.list.Get();
      g.bound.valid = false;
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      // The destination has the render target's scale: host pixel for host pixel.
      D3D12_VIEWPORT vp = {0.0f, 0.0f, float(ScaleSize(dest.width, dest.scale4)),
                           float(ScaleSize(dest.height, dest.scale4)), 0.0f, 1.0f};
      D3D12_RECT host_rect = ScaleRect(rect, dest.scale4);
      host_rect.top += offset_y;
      host_rect.bottom += offset_y;
      list->RSSetViewports(1, &vp);
      list->RSSetScissorRects(1, &host_rect);
      list->SetGraphicsRootSignature(g.resolve_root.Get());
      list->SetPipelineState(pso);
      list->SetGraphicsRootDescriptorTable(0, srv_gpu);
      list->SetGraphicsRoot32BitConstants(1, 6, constants, 0);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      GpuMark(kGpuResolve);
      Crumb("resolve draw", uint64_t(src.resource.Get()), uint64_t(dest.resource.Get()), variant, rf.rtv);
      list->DrawInstanced(3, 1, 0, 0);
      return true;
    }

    // Brings the guest-sized copy of a resolve destination up to date (box
    // filter over the host pixels under each guest pixel).
    bool EnsureLowres(HostTexture &t)
    {
      if (t.lowres && t.lowres_generation == t.generation)
      {
        return true;
      }
      Frame &f = CurrentFrame();
      if (f.views_used >= kViewsPerFrame)
      {
        return false;
      }
      ID3D12PipelineState *pso = GetResolvePipeline(4, t.format);
      if (!pso)
      {
        return false;
      }
      if (!t.lowres)
      {
        D3D12_RESOURCE_DESC td = t.resource->GetDesc();
        td.Width = t.width;
        td.Height = t.height;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
        t.lowres_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ++g.resources_created;
        if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.lowres_state, nullptr,
                                                     IID_PPV_ARGS(&t.lowres))))
        {
          NativeLog("guest-resolution copy creation failed (%ux%u fmt %u)\n", t.width, t.height, unsigned(td.Format));
          t.lowres.Reset();
          return false;
        }
      }
      uint32_t slot = g.frame_index * kViewsPerFrame + f.views_used++;
      D3D12_CPU_DESCRIPTOR_HANDLE srv = g.view_heap->GetCPUDescriptorHandleForHeapStart();
      srv.ptr += uint64_t(slot) * g.view_size;
      D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = g.view_heap->GetGPUDescriptorHandleForHeapStart();
      srv_gpu.ptr += uint64_t(slot) * g.view_size;
      D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = t.format;
      sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      sd.Texture2D.MipLevels = 1;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      g.device->CreateShaderResourceView(t.resource.Get(), &sd, srv);
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.transient_rtv_heap->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += uint64_t(g.transient_rtvs_used++ % kTransientRtvCount) * g.rtv_size;
      D3D12_RENDER_TARGET_VIEW_DESC rd = {};
      rd.Format = t.format;
      rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      g.device->CreateRenderTargetView(t.lowres.Get(), &rd, rtv);
      uint32_t constants[6] = {0, 0, t.scale4, 0, 0, 0};

      Transition(t.resource.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      Transition(t.lowres.Get(), t.lowres_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      auto *list = g.list.Get();
      g.bound.valid = false;
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      D3D12_VIEWPORT vp = {0.0f, 0.0f, float(t.width), float(t.height), 0.0f, 1.0f};
      D3D12_RECT rect = {0, 0, LONG(t.width), LONG(t.height)};
      list->RSSetViewports(1, &vp);
      list->RSSetScissorRects(1, &rect);
      list->SetGraphicsRootSignature(g.resolve_root.Get());
      list->SetPipelineState(pso);
      list->SetGraphicsRootDescriptorTable(0, srv_gpu);
      list->SetGraphicsRoot32BitConstants(1, 6, constants, 0);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      GpuMark(kGpuResolve);
      Crumb("guest-resolution copy", uint64_t(t.resource.Get()), uint64_t(t.lowres.Get()), t.width, t.height);
      list->DrawInstanced(3, 1, 0, 0);
      t.lowres_generation = t.generation;
      ++g.lowres_copies;
      return true;
    }

    // ------------------------------------------------------------ Post effects
    //
    // Effects the console never had, added to the game's own frame
    // (post_effects.h has the shaders):
    // - Stage 1, at the frame's first resolve of the whole HDR scene (the
    //   opaque scene: the game resolves it, clears and draws it back before the
    //   translucent pass): the scene depth is still in its render target and is
    //   kept as view depth for the rest of the frame; ambient occlusion is
    //   computed from it and multiplied into the resolved scene, which the
    //   game then draws back - translucent surfaces and the sky light come
    //   after and stay as they are.
    // - Stage 2, on the scene's last resolve, before anything samples it into
    //   another surface (the bloom downsample, the tonemap): motion blur,
    //   depth of field and anti-aliasing, so the game's bloom and tonemap
    //   work on the result and the HUD drawn after them is untouched.
    //   Draws that sample the scene into its own surface (the restore) do not
    //   count.
    // The camera comes from the scene draws: their vertex shaders get the view
    // projection in c0-c3 (UE3's ViewProjectionMatrix; the column of w is the
    // view direction, unit length). The one most draws over the whole screen
    // with depth writes use is the scene's. Its inverse turns depth back into
    // positions; the previous frame's projects them for camera motion blur.
    // Split screen (two smaller views) and scene captures are left alone.

    enum PostPass : uint32_t
    {
      kPassLinearize,
      kPassLinearizeMs,
      kPassAo,
      kPassAoBlur,
      kPassAoApply,
      kPassMotionBlur,
      kPassDofFocus,
      kPassDof,
      kPassFxaa,
      kPassSmaaEdges,
      kPassSmaaWeights,
      kPassSmaaBlend,
      kPassDepthHalf,
      kPassCount,
    };
    const char *const kPassEntries[kPassCount] = {
        "ps_linearize", "ps_linearize", "ps_ao",     "ps_ao_blur",    "ps_ao_apply",     "ps_motion_blur",
        "ps_dof_focus", "ps_dof",       "ps_fxaa",   "ps_smaa_edges", "ps_smaa_weights", "ps_smaa_blend",
        "ps_depth_half",
    };

    // cbuffer PostConstants (post_effects.h).
    struct PostConstants
    {
      float inv_vp[16];
      float vp[16];
      float prev_vp[16];
      float target_size[4];
      float view_size[4];
      float depth_params[4];
      float p0[4];
      float p1[4];
      uint32_t i0[4];
    };

    struct PostTexture
    {
      ComPtr<ID3D12Resource> resource;
      D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
      DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
      uint32_t width = 0, height = 0;
    };

    struct PostSettings
    {
      uint32_t ao = 0;  // 0 off, 1 SSAO, 2 HBAO, 3 GTAO (the shader's i0.x)
      uint32_t aa = 0;  // 0 off, 1 FXAA, 2 SMAA
      int32_t motion_blur = 0, dof = 0;
      bool Any() const { return ao || aa || motion_blur > 0 || dof > 0; }
      bool NeedsDepth() const { return ao || motion_blur > 0 || dof > 0; }
    };

    struct Post
    {
      bool failed = false;
      ComPtr<ID3D12RootSignature> root;
      // Made on the background thread (PostWorkerMain): a pass is skipped
      // until its pipeline is there. A null pipeline = it failed.
      std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> pipelines;
      std::unordered_set<uint64_t> queued;
      bool worker_started = false;
      std::mutex worker_mutex;
      std::condition_variable worker_cv;
      std::deque<uint64_t> jobs;                                        // under worker_mutex
      std::vector<std::pair<uint64_t, ComPtr<ID3D12PipelineState>>> done; // under worker_mutex
      // The background thread's own.
      std::vector<uint8_t> vs;
      bool vs_failed = false;
      std::vector<uint8_t> ps[kPassCount];
      bool ps_failed[kPassCount] = {};
      PostTexture depth, depth_half, ao[2], color[2], edges, weights, focus[2];
      uint32_t focus_index = 0;
      uint64_t focus_frame = UINT64_MAX; // frame the focus was last eased in

      PostSettings settings;
      uint64_t settings_frame = UINT64_MAX;

      // View projections of this frame's scene draws (see NoteSceneCamera).
      struct Candidate
      {
        float m[16];
        float z_scale, z_offset;
        uint32_t count;
        RenderTarget *depth;
      };
      Candidate candidates[4] = {};
      uint32_t candidate_count = 0;
      uint64_t candidates_frame = UINT64_MAX;

      // The scene's camera in this frame (stage 1) and in the frame before.
      double vp[16] = {}, prev[16] = {};
      bool has_vp = false, has_prev = false;
      uint64_t vp_frame = UINT64_MAX;
      float z_scale = 1.0f, z_offset = 0.0f;
      RenderTarget *vp_depth = nullptr; // the depth surface the scene was drawn with
      std::chrono::steady_clock::time_point vp_time;
      double frame_seconds = 1.0 / 60.0;

      uint64_t scene_frame = UINT64_MAX; // stage 1 ran in this frame
      bool depth_ready = false;          // `depth` holds this frame's scene
      // A tiled scene's color waiting for its depth resolve (PostDepthResolved).
      HostTexture *waiting_color = nullptr;
      uint64_t waiting_generation = 0;
      uint32_t waiting_base = 0;
      uint32_t width = 0, height = 0;    // the picture in host pixels

      // Stage 2 waits for the first draw that samples this resolve elsewhere.
      bool pending = false;
      const HostTexture *pending_texture = nullptr;
      uint64_t pending_generation = 0;
      uint32_t pending_base = 0, pending_src_base = 0, pending_src_format = 0;
      const RenderTarget *draw_target = nullptr; // color target 0 of the draw being prepared
      uint64_t logged = 0;
    };
    Post g_post;

    const PostSettings &CurrentPostSettings()
    {
      if (g_post.settings_frame == g.frame_number)
      {
        return g_post.settings;
      }
      g_post.settings_frame = g.frame_number;
      PostSettings s;
      std::string ao = REXCVAR_GET(ao2_ambient_occlusion);
      std::string aa = REXCVAR_GET(ao2_anti_aliasing);
      for (std::string *v : {&ao, &aa})
      {
        std::transform(v->begin(), v->end(), v->begin(), [](unsigned char c) { return char(std::tolower(c)); });
      }
      s.ao = ao == "ssao" ? 1u : ao == "hbao" ? 2u : ao == "gtao" ? 3u : 0u;
      s.aa = aa == "fxaa" ? 1u : aa == "smaa" ? 2u : 0u;
      // Never in the main menu and the lobby (the front end).
      s.motion_blur = ao2::mouse::InFrontEnd() ? 0 : std::clamp(REXCVAR_GET(ao2_motion_blur), 0, 2);
      s.dof = REXCVAR_GET(ao2_depth_of_field) > 0 ? 1 : 0;
      g_post.settings = s;
      return g_post.settings;
    }

    // A draw over the whole screen writing depth: notes the view projection
    // its vertex shader gets (c0-c3) when it looks like a perspective one.
    void NoteSceneCamera(const RegisterFile &r, RenderTarget *ds, const D3D12_VIEWPORT &viewport,
                         reg::RB_DEPTHCONTROL depth_control)
    {
      if (!ds || !depth_control.z_enable || !depth_control.z_write_enable || ds->scale4 != g.scale4 ||
          !CurrentPostSettings().NeedsDepth())
      {
        return;
      }
      float gw = float(ao2::aspect::GuestWidth()), gh = float(ao2::aspect::GuestHeight());
      if (viewport.TopLeftX > 0.0f || viewport.TopLeftY > 0.0f || viewport.Width < gw * 0.9f ||
          viewport.Height < gh * 0.9f)
      {
        return;
      }
      if (g_post.candidates_frame != g.frame_number)
      {
        g_post.candidates_frame = g.frame_number;
        g_post.candidate_count = 0;
      }
      uint32_t base = std::min(r.v[XE_GPU_REG_SQ_VS_CONST] & 0x1FFu, 252u);
      float m[16];
      std::memcpy(m, &r.v[0x4000 + base * 4], sizeof(m));
      // Column of w (row-vector matrices: clip.w = x * m[3] + y * m[7] + z * m[11] + m[15]).
      float wlen = std::sqrt(m[3] * m[3] + m[7] * m[7] + m[11] * m[11]);
      if (!(wlen > 0.95f && wlen < 1.05f))
      {
        return;
      }
      uint32_t vte = r.v[XE_GPU_REG_PA_CL_VTE_CNTL];
      float z_scale = (vte & (1u << 4)) ? r.F(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
      float z_offset = (vte & (1u << 5)) ? r.F(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
      if (z_scale == 0.0f)
      {
        return;
      }
      for (uint32_t i = 0; i < g_post.candidate_count; ++i)
      {
        Post::Candidate &c = g_post.candidates[i];
        if (!std::memcmp(c.m, m, sizeof(m)) && c.z_scale == z_scale && c.z_offset == z_offset)
        {
          ++c.count;
          c.depth = ds;
          return;
        }
      }
      if (g_post.candidate_count < std::size(g_post.candidates))
      {
        Post::Candidate &c = g_post.candidates[g_post.candidate_count++];
        std::memcpy(c.m, m, sizeof(m));
        c.z_scale = z_scale;
        c.z_offset = z_offset;
        c.count = 1;
        c.depth = ds;
      }
    }

    // 4x4 inverse (row-major), false when singular.
    bool Invert4(const double m[16], double out[16])
    {
      double a[4][8];
      for (int i = 0; i < 4; ++i)
      {
        for (int j = 0; j < 4; ++j)
        {
          a[i][j] = m[i * 4 + j];
          a[i][j + 4] = i == j ? 1.0 : 0.0;
        }
      }
      for (int c = 0; c < 4; ++c)
      {
        int pivot = c;
        for (int i = c + 1; i < 4; ++i)
        {
          if (std::fabs(a[i][c]) > std::fabs(a[pivot][c]))
          {
            pivot = i;
          }
        }
        if (std::fabs(a[pivot][c]) < 1e-12)
        {
          return false;
        }
        if (pivot != c)
        {
          for (int j = 0; j < 8; ++j)
          {
            std::swap(a[c][j], a[pivot][j]);
          }
        }
        double d = a[c][c];
        for (int j = 0; j < 8; ++j)
        {
          a[c][j] /= d;
        }
        for (int i = 0; i < 4; ++i)
        {
          if (i != c && a[i][c] != 0.0)
          {
            double f = a[i][c];
            for (int j = 0; j < 8; ++j)
            {
              a[i][j] -= f * a[c][j];
            }
          }
        }
      }
      for (int i = 0; i < 4; ++i)
      {
        for (int j = 0; j < 4; ++j)
        {
          out[i * 4 + j] = a[i][j + 4];
        }
      }
      return true;
    }

    // Camera position of a view projection: clip (0, 0, 1, 0) through its inverse.
    bool CameraOf(const double inv[16], double c[3])
    {
      if (std::fabs(inv[11]) < 1e-12)
      {
        return false;
      }
      for (int k = 0; k < 3; ++k)
      {
        c[k] = inv[8 + k] / inv[11];
      }
      return true;
    }

    // Takes the scene's camera for this frame from the noted candidates (the
    // previous frame's stays for motion blur unless the camera jumped).
    bool SelectSceneCamera()
    {
      const Post::Candidate *best = nullptr;
      if (g_post.candidates_frame == g.frame_number)
      {
        for (uint32_t i = 0; i < g_post.candidate_count; ++i)
        {
          if (!best || g_post.candidates[i].count > best->count)
          {
            best = &g_post.candidates[i];
          }
        }
      }
      auto now = std::chrono::steady_clock::now();
      bool consecutive = g_post.has_vp && g_post.vp_frame + 1 == g.frame_number;
      if (consecutive)
      {
        double seconds = std::chrono::duration<double>(now - g_post.vp_time).count();
        g_post.frame_seconds = std::clamp(seconds, 1.0 / 1000.0, 0.25);
      }
      g_post.vp_time = now;
      if (!best)
      {
        g_post.has_vp = g_post.has_prev = false;
        return false;
      }
      double vp[16];
      for (int k = 0; k < 16; ++k)
      {
        vp[k] = best->m[k];
      }
      g_post.has_prev = false;
      if (consecutive)
      {
        std::memcpy(g_post.prev, g_post.vp, sizeof(vp));
        // A cut (or a teleport): no blur from the old view.
        double inv_new[16], inv_old[16], c_new[3], c_old[3];
        if (Invert4(vp, inv_new) && Invert4(g_post.prev, inv_old) && CameraOf(inv_new, c_new) &&
            CameraOf(inv_old, c_old))
        {
          double dx = c_new[0] - c_old[0], dy = c_new[1] - c_old[1], dz = c_new[2] - c_old[2];
          double turn = vp[3] * g_post.prev[3] + vp[7] * g_post.prev[7] + vp[11] * g_post.prev[11];
          g_post.has_prev = dx * dx + dy * dy + dz * dz < 400.0 * 400.0 && turn > 0.8;
        }
      }
      std::memcpy(g_post.vp, vp, sizeof(vp));
      g_post.has_vp = true;
      g_post.vp_frame = g.frame_number;
      g_post.z_scale = best->z_scale;
      g_post.z_offset = best->z_offset;
      g_post.vp_depth = best->depth;
      return true;
    }

    // The constants every pass shares: the camera relative to its position
    // (float precision near the camera however far it is from the origin).
    bool FillPostCamera(PostConstants &c)
    {
      double inv[16], cam[3];
      if (!g_post.has_vp || !Invert4(g_post.vp, inv) || !CameraOf(inv, cam))
      {
        return false;
      }
      for (int i = 0; i < 4; ++i)
      {
        for (int k = 0; k < 3; ++k)
        {
          c.inv_vp[i * 4 + k] = i == 2 ? 0.0f : float(inv[i * 4 + k] - cam[k] * inv[i * 4 + 3]);
        }
        c.inv_vp[i * 4 + 3] = float(inv[i * 4 + 3]);
      }
      auto relative = [&](const double m[16], float out[16]) {
        for (int k = 0; k < 12; ++k)
        {
          out[k] = float(m[k]);
        }
        for (int j = 0; j < 4; ++j)
        {
          out[12 + j] = float(cam[0] * m[j] + cam[1] * m[4 + j] + cam[2] * m[8 + j] + m[12 + j]);
        }
      };
      relative(g_post.vp, c.vp);
      relative(g_post.has_prev ? g_post.prev : g_post.vp, c.prev_vp);
      float w = float(g_post.width), h = float(g_post.height);
      c.view_size[0] = w;
      c.view_size[1] = h;
      c.view_size[2] = 1.0f / w;
      c.view_size[3] = 1.0f / h;
      double py = std::sqrt(g_post.vp[1] * g_post.vp[1] + g_post.vp[5] * g_post.vp[5] + g_post.vp[9] * g_post.vp[9]);
      c.depth_params[0] = g_post.z_scale;
      c.depth_params[1] = g_post.z_offset;
      c.depth_params[2] = float(py * h * 0.5);
      c.p1[0] = 4.0f; // the scene texture holds a quarter of the brightness (resolve exponent bias -2)
      return true;
    }

    // Pixel shader of a pass (the background thread's).
    const std::vector<uint8_t> *PostShader(PostPass pass)
    {
      if (!g_post.ps[pass].empty())
      {
        return &g_post.ps[pass];
      }
      if (g_post.ps_failed[pass])
      {
        return nullptr;
      }
      D3D_SHADER_MACRO macros[] = {{"DEPTH_MS", "1"}, {nullptr, nullptr}};
      ComPtr<ID3DBlob> code, errors;
      auto start = std::chrono::steady_clock::now();
      if (FAILED(D3DCompile(kPostShader, std::strlen(kPostShader), nullptr,
                            pass == kPassLinearizeMs ? macros : macros + 1, nullptr, kPassEntries[pass], "ps_5_1",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
      {
        NativeLog("post effects: %s failed: %s\n", kPassEntries[pass],
                  errors ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
        g_post.ps_failed[pass] = true;
        return nullptr;
      }
      auto *b = static_cast<const uint8_t *>(code->GetBufferPointer());
      g_post.ps[pass].assign(b, b + code->GetBufferSize());
      NativeLog("post effects: %s compiled (%.0f ms)\n", kPassEntries[pass], Elapsed(start) / 1e6);
      return &g_post.ps[pass];
    }

    uint64_t PostPipelineKey(PostPass pass, DXGI_FORMAT format, uint32_t blend)
    {
      return (uint64_t(pass) << 40) | (uint64_t(blend) << 32) | uint64_t(format);
    }

    // A pass's pipeline (the background thread's); null when it failed.
    // blend: 0 none, 1 multiply (dest * src, alpha kept).
    ComPtr<ID3D12PipelineState> MakePostPipeline(uint64_t key)
    {
      auto pass = PostPass(key >> 40);
      uint32_t blend = uint32_t(key >> 32) & 0xFF;
      auto format = DXGI_FORMAT(uint32_t(key));
      if (g_post.vs.empty() && !g_post.vs_failed)
      {
        g_post.vs = Compile(kPostShader, "vs", "vs_5_1");
        g_post.vs_failed = g_post.vs.empty();
      }
      const std::vector<uint8_t> *ps = PostShader(pass);
      if (g_post.vs.empty() || !ps)
      {
        return nullptr;
      }
      D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
      d.pRootSignature = g_post.root.Get();
      d.VS = {g_post.vs.data(), g_post.vs.size()};
      d.PS = {ps->data(), ps->size()};
      D3D12_RENDER_TARGET_BLEND_DESC &rb = d.BlendState.RenderTarget[0];
      rb.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      if (blend == 1)
      {
        rb.BlendEnable = TRUE;
        rb.SrcBlend = D3D12_BLEND_ZERO;
        rb.DestBlend = D3D12_BLEND_SRC_COLOR;
        rb.BlendOp = D3D12_BLEND_OP_ADD;
        rb.SrcBlendAlpha = D3D12_BLEND_ZERO;
        rb.DestBlendAlpha = D3D12_BLEND_ONE;
        rb.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      }
      d.SampleMask = UINT_MAX;
      d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      d.RasterizerState.DepthClipEnable = TRUE;
      d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      d.NumRenderTargets = 1;
      d.RTVFormats[0] = format;
      d.SampleDesc.Count = 1;
      ComPtr<ID3D12PipelineState> pso;
      if (FAILED(g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso))))
      {
        NativeLog("post effects: pipeline %s failed (format %u)\n", kPassEntries[pass], unsigned(format));
        pso = nullptr;
      }
      return pso;
    }

    // The post effects' shaders (FXC: tens to hundreds of ms each) and
    // pipelines are made here, off the GPU thread, so turning an effect on
    // does not stall a frame: a pass runs from the first frame its pipeline
    // is there (until then the picture is without it).
    void PostWorkerMain()
    {
      for (;;)
      {
        uint64_t key;
        {
          std::unique_lock lock(g_post.worker_mutex);
          g_post.worker_cv.wait(lock, [] { return !g_post.jobs.empty(); });
          key = g_post.jobs.front();
          g_post.jobs.pop_front();
        }
        ComPtr<ID3D12PipelineState> pso = MakePostPipeline(key);
        std::lock_guard lock(g_post.worker_mutex);
        g_post.done.emplace_back(key, std::move(pso));
      }
    }

    // Hands a pipeline to the background thread (once).
    void QueuePostPipeline(uint64_t key)
    {
      if (!g_post.queued.insert(key).second)
      {
        return;
      }
      if (!g_post.worker_started)
      {
        g_post.worker_started = true;
        std::thread(PostWorkerMain).detach();
      }
      {
        std::lock_guard lock(g_post.worker_mutex);
        g_post.jobs.push_back(key);
      }
      g_post.worker_cv.notify_one();
    }

    // Whether the settings use a pass.
    bool PostPassUsed(PostPass pass, const PostSettings &s)
    {
      switch (pass)
      {
      case kPassLinearize:
      case kPassLinearizeMs: return s.NeedsDepth();
      case kPassDepthHalf:
      case kPassAo:
      case kPassAoBlur:
      case kPassAoApply: return s.ao != 0;
      case kPassMotionBlur: return s.motion_blur > 0;
      case kPassDofFocus:
      case kPassDof: return s.dof > 0;
      case kPassFxaa: return s.aa == 1;
      case kPassSmaaEdges:
      case kPassSmaaWeights:
      case kPassSmaaBlend: return s.aa == 2;
      default: return false;
      }
    }

    // The root signature; every pass's pipeline goes to the background thread,
    // those of the effects that are on first.
    bool InitPost()
    {
      if (g_post.root)
      {
        return true;
      }
      if (g_post.failed)
      {
        return false;
      }
      g_post.failed = true;
      D3D12_DESCRIPTOR_RANGE range = {};
      range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      range.NumDescriptors = 3;
      D3D12_ROOT_PARAMETER p[2] = {};
      p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      p[0].DescriptorTable.NumDescriptorRanges = 1;
      p[0].DescriptorTable.pDescriptorRanges = &range;
      p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
      for (uint32_t i = 0; i < 2; ++i)
      {
        samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      D3D12_ROOT_SIGNATURE_DESC desc = {};
      desc.NumParameters = 2;
      desc.pParameters = p;
      desc.NumStaticSamplers = 2;
      desc.pStaticSamplers = samplers;
      ComPtr<ID3DBlob> blob, error;
      if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
          FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&g_post.root))))
      {
        NativeLog("post effects: root signature failed\n");
        return false;
      }
      g_post.failed = false;
      // Each pass with the format it draws into (PostStage1, PostBeforeSample).
      static const struct
      {
        PostPass pass;
        DXGI_FORMAT format;
        uint32_t blend;
      } kPipelines[] = {
          {kPassLinearize, DXGI_FORMAT_R32_FLOAT, 0},
          {kPassLinearizeMs, DXGI_FORMAT_R32_FLOAT, 0},
          {kPassAo, DXGI_FORMAT_R16G16_FLOAT, 0},
          {kPassAoBlur, DXGI_FORMAT_R16G16_FLOAT, 0},
          {kPassAoApply, DXGI_FORMAT_R16G16B16A16_FLOAT, 1},
          {kPassMotionBlur, DXGI_FORMAT_R16G16B16A16_FLOAT, 0},
          {kPassDofFocus, DXGI_FORMAT_R32_FLOAT, 0},
          {kPassDof, DXGI_FORMAT_R16G16B16A16_FLOAT, 0},
          {kPassFxaa, DXGI_FORMAT_R16G16B16A16_FLOAT, 0},
          {kPassSmaaEdges, DXGI_FORMAT_R8G8_UNORM, 0},
          {kPassSmaaWeights, DXGI_FORMAT_R8G8B8A8_UNORM, 0},
          {kPassSmaaBlend, DXGI_FORMAT_R16G16B16A16_FLOAT, 0},
          {kPassDepthHalf, DXGI_FORMAT_R32_FLOAT, 0},
      };
      const PostSettings &s = CurrentPostSettings();
      for (bool used : {true, false})
      {
        for (const auto &p : kPipelines)
        {
          if (PostPassUsed(p.pass, s) == used)
          {
            QueuePostPipeline(PostPipelineKey(p.pass, p.format, p.blend));
          }
        }
      }
      return true;
    }

    // A pass's pipeline; null while the background thread is making it (the
    // pass is skipped) or when it failed.
    ID3D12PipelineState *GetPostPipeline(PostPass pass, DXGI_FORMAT format, uint32_t blend)
    {
      uint64_t key = PostPipelineKey(pass, format, blend);
      auto it = g_post.pipelines.find(key);
      if (it != g_post.pipelines.end())
      {
        return it->second.Get();
      }
      {
        std::lock_guard lock(g_post.worker_mutex);
        for (auto &d : g_post.done)
        {
          g_post.pipelines[d.first] = std::move(d.second);
        }
        g_post.done.clear();
      }
      it = g_post.pipelines.find(key);
      if (it != g_post.pipelines.end())
      {
        return it->second.Get();
      }
      QueuePostPipeline(key);
      return nullptr;
    }

    bool EnsurePostTexture(PostTexture &t, uint32_t width, uint32_t height, DXGI_FORMAT format)
    {
      if (t.resource && t.width == width && t.height == height && t.format == format)
      {
        return true;
      }
      DeferRelease(std::move(t.resource));
      D3D12_RESOURCE_DESC td = {};
      td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      td.Width = width;
      td.Height = height;
      td.DepthOrArraySize = 1;
      td.MipLevels = 1;
      td.SampleDesc.Count = 1;
      td.Format = format;
      td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
      t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
      ++g.resources_created;
      if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.state, nullptr,
                                                   IID_PPV_ARGS(&t.resource))))
      {
        NativeLog("post effects: texture creation failed (%ux%u fmt %u)\n", width, height, unsigned(format));
        t = {};
        return false;
      }
      t.width = width;
      t.height = height;
      t.format = format;
      return true;
    }

    struct PostInput
    {
      ID3D12Resource *resource;
      D3D12_RESOURCE_STATES *state;
      DXGI_FORMAT format;
      bool multisampled = false;
    };

    PostInput In(PostTexture &t)
    {
      return {t.resource.Get(), &t.state, t.format};
    }

    // One fullscreen pass into `target` (its whole `width` x `height`).
    bool PostDraw(PostPass pass, ID3D12Resource *target, D3D12_RESOURCE_STATES &target_state, DXGI_FORMAT format,
                  uint32_t width, uint32_t height, uint32_t blend, std::initializer_list<PostInput> inputs,
                  PostConstants &c)
    {
      Frame &f = CurrentFrame();
      if (!target || f.views_used + 3 > kViewsPerFrame)
      {
        return false;
      }
      ID3D12PipelineState *pso = GetPostPipeline(pass, format, blend);
      if (!pso)
      {
        return false;
      }
      c.target_size[0] = float(width);
      c.target_size[1] = float(height);
      c.target_size[2] = 1.0f / float(width);
      c.target_size[3] = 1.0f / float(height);
      D3D12_GPU_VIRTUAL_ADDRESS cbv = Upload(&c, sizeof(c));
      if (!cbv)
      {
        return false;
      }
      uint32_t slot = g.frame_index * kViewsPerFrame + f.views_used;
      f.views_used += 3;
      D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.view_heap->GetCPUDescriptorHandleForHeapStart();
      cpu.ptr += uint64_t(slot) * g.view_size;
      D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.view_heap->GetGPUDescriptorHandleForHeapStart();
      gpu.ptr += uint64_t(slot) * g.view_size;
      uint32_t k = 0;
      for (const PostInput &in : inputs)
      {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += uint64_t(k++) * g.view_size;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = in.format;
        sd.ViewDimension = in.multisampled ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        g.device->CreateShaderResourceView(in.resource, &sd, h);
        Transition(in.resource, *in.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
      for (; k < 3; ++k)
      {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += uint64_t(k) * g.view_size;
        g.device->CopyDescriptorsSimple(1, h, g.null_srv_2d, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      }
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.transient_rtv_heap->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += uint64_t(g.transient_rtvs_used++ % kTransientRtvCount) * g.rtv_size;
      D3D12_RENDER_TARGET_VIEW_DESC rd = {};
      rd.Format = format;
      rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      g.device->CreateRenderTargetView(target, &rd, rtv);
      Transition(target, target_state, D3D12_RESOURCE_STATE_RENDER_TARGET);

      auto *list = g.list.Get();
      g.bound.valid = false;
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      D3D12_VIEWPORT vp = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
      D3D12_RECT sc = {0, 0, LONG(width), LONG(height)};
      list->RSSetViewports(1, &vp);
      list->RSSetScissorRects(1, &sc);
      list->SetGraphicsRootSignature(g_post.root.Get());
      list->SetPipelineState(pso);
      list->SetGraphicsRootDescriptorTable(0, gpu);
      list->SetGraphicsRootConstantBufferView(1, cbv);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      GpuMark(kGpuPost);
      Crumb("post pass", pass, uint64_t(target), width, height);
      list->DrawInstanced(3, 1, 0, 0);
      return true;
    }

    bool PostDraw(PostPass pass, PostTexture &target, uint32_t blend, std::initializer_list<PostInput> inputs,
                  PostConstants &c)
    {
      return PostDraw(pass, target.resource.Get(), target.state, target.format, target.width, target.height, blend,
                      inputs, c);
    }

    // Copies the picture's part (`width` x `height` host pixels) between two
    // textures of one format.
    void PostCopy(ID3D12Resource *dst, D3D12_RESOURCE_STATES &dst_state, ID3D12Resource *src,
                  D3D12_RESOURCE_STATES &src_state, uint32_t width, uint32_t height)
    {
      Transition(src, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      Transition(dst, dst_state, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION s = {src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_TEXTURE_COPY_LOCATION t = {dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_BOX box = {0, 0, 0, width, height, 1};
      GpuMark(kGpuPost);
      Crumb("post copy", uint64_t(src), uint64_t(dst), width, height);
      g.list->CopyTextureRegion(&t, 0, 0, 0, &s, &box);
    }

    // Stage 1 with the scene depth in `depth` (its render target, or the
    // resolved depth texture of a tiled frame): the view depth, kept for the
    // frame, and ambient occlusion multiplied into the resolved scene.
    void PostStage1(HostTexture &tex, uint32_t width, uint32_t height, const PostInput &depth)
    {
      const PostSettings &s = CurrentPostSettings();
      PostConstants c = {};
      if (!FillPostCamera(c))
      {
        return;
      }
      if (EnsurePostTexture(g_post.depth, width, height, DXGI_FORMAT_R32_FLOAT) &&
          PostDraw(depth.multisampled ? kPassLinearizeMs : kPassLinearize, g_post.depth, 0, {depth}, c))
      {
        g_post.depth_ready = true;
      }
      if (!g_post.depth_ready || !s.ao)
      {
        return;
      }
      uint32_t hw = (width + 1) / 2, hh = (height + 1) / 2;
      float scale = float(g.scale4) * 0.25f;
      static const uint32_t kDirections[4] = {0, 16, 8, 3}, kSteps[4] = {0, 0, 6, 6}; // SSAO: 16 taps
      static const float kStrength[4] = {0.0f, 1.2f, 1.5f, 1.5f};
      c.p0[0] = 80.0f;            // radius, world units
      c.p0[1] = 80.0f * scale;    // at most, in pixels
      c.p0[2] = kStrength[s.ao];
      c.p0[3] = 8000.0f;          // gone by this view depth
      c.i0[0] = s.ao;
      c.i0[1] = kDirections[s.ao];
      c.i0[2] = kSteps[s.ao];
      c.p1[0] = float(hw);
      c.p1[1] = float(hh);
      bool ok = EnsurePostTexture(g_post.depth_half, hw, hh, DXGI_FORMAT_R32_FLOAT) &&
                EnsurePostTexture(g_post.ao[0], hw, hh, DXGI_FORMAT_R16G16_FLOAT) &&
                EnsurePostTexture(g_post.ao[1], hw, hh, DXGI_FORMAT_R16G16_FLOAT) &&
                PostDraw(kPassDepthHalf, g_post.depth_half, 0, {In(g_post.depth)}, c) &&
                PostDraw(kPassAo, g_post.ao[0], 0, {In(g_post.depth_half)}, c);
      c.i0[0] = 0;
      ok = ok && PostDraw(kPassAoBlur, g_post.ao[1], 0, {In(g_post.ao[0])}, c);
      c.i0[0] = 1;
      ok = ok && PostDraw(kPassAoBlur, g_post.ao[0], 0, {In(g_post.ao[1])}, c);
      // Debug: the occlusion alone (at a quarter, as the scene is stored).
      bool show = REXCVAR_GET(ao2_post_debug) == 1;
      c.p1[2] = show ? 0.25f : 1.0f;
      ok = ok && PostDraw(kPassAoApply, tex.resource.Get(), tex.state, tex.format, width, height, show ? 0 : 1,
                          {In(g_post.ao[0]), In(g_post.depth)}, c);
      if (ok && g_trace)
      {
        // Traced frame: the occlusion and the view depth (capture/).
        char name[64];
        std::snprintf(name, sizeof(name), "%03llu_post_ao", (unsigned long long)g.capture_index++);
        CaptureTexture(g_post.ao[0].resource.Get(), g_post.ao[0].state, g_post.ao[0].format, name);
        std::snprintf(name, sizeof(name), "%03llu_post_depth", (unsigned long long)g.capture_index++);
        CaptureTexture(g_post.depth.resource.Get(), g_post.depth.state, g_post.depth.format, name);
      }
      if (!ok && g_post.logged++ < 4)
      {
        NativeLog("post effects: ambient occlusion failed in frame %llu\n", (unsigned long long)g.frame_number);
      }
    }

    // A resolve of the scene's color: the whole picture at once, or the last
    // of its predicated tiles (the game's MSAA; `dest_row` = the tile's first
    // row). The frame's first one runs stage 1 - with a whole picture the
    // depth render target still holds the scene's depth; with tiles it holds
    // the last tile's only, so stage 1 waits for the depth resolve that
    // completes the scene depth texture (PostDepthResolved).
    void PostSceneResolved(RenderTarget &src, HostTexture &tex, const D3D12_RECT &rect, uint32_t dest_row,
                           uint32_t dest_base)
    {
      const PostSettings &s = CurrentPostSettings();
      if (!s.Any() || tex.format != DXGI_FORMAT_R16G16B16A16_FLOAT || rect.left || src.scale4 != g.scale4 ||
          src.depth)
      {
        return;
      }
      uint32_t gw = ao2::aspect::GuestWidth(), gh = ao2::aspect::GuestHeight();
      uint32_t top = uint32_t(rect.top) + dest_row, bottom = uint32_t(rect.bottom) + dest_row;
      bool whole = top == 0;
      if (uint32_t(rect.right) * 10 < gw * 9 || bottom * 10 < gh * 9 || (!whole && !dest_row) || !InitPost())
      {
        return;
      }
      uint32_t width = ScaleSize(uint32_t(rect.right), src.scale4), height = ScaleSize(bottom, src.scale4);
      if (g_post.scene_frame != g.frame_number)
      {
        g_post.scene_frame = g.frame_number;
        g_post.depth_ready = false;
        g_post.waiting_color = nullptr;
        g_post.width = width;
        g_post.height = height;
        if (s.NeedsDepth() && SelectSceneCamera())
        {
          RenderTarget *depth = g_post.vp_depth;
          if (!whole)
          {
            g_post.waiting_color = &tex;
            g_post.waiting_generation = tex.generation;
            g_post.waiting_base = dest_base;
          }
          else if (depth && depth->scale4 == src.scale4 && ScaleSize(depth->width, depth->scale4) >= width &&
                   ScaleSize(depth->height, depth->scale4) >= height)
          {
            ClaimEdram(*depth, rect);
            PostStage1(tex, width, height,
                       {depth->resource.Get(), &depth->state, DepthSrvFormat(depth->format), depth->samples > 1});
          }
        }
        if (g_post.logged < 2 && s.NeedsDepth())
        {
          ++g_post.logged;
          NativeLog("post effects: scene %08X %ux%u (host %ux%u)%s, camera %s from %u candidates, depth %s\n",
                    dest_base, uint32_t(rect.right), bottom, width, height, whole ? "" : " in tiles",
                    g_post.has_vp ? "found" : "not found", g_post.candidate_count,
                    g_post.depth_ready ? "kept" : (g_post.waiting_color ? "from its resolve" : "not kept"));
        }
      }
      if (s.aa || s.motion_blur || s.dof)
      {
        g_post.pending = true;
        g_post.pending_texture = &tex;
        g_post.pending_generation = tex.generation;
        g_post.pending_base = dest_base;
        g_post.pending_src_base = src.edram_base;
        g_post.pending_src_format = src.edram_format;
        g_post.width = width;
        g_post.height = height;
      }
    }

    // A depth resolve: in a tiled frame, the one that completes the scene
    // depth texture runs stage 1 for the scene color waiting for it.
    void PostDepthResolved(HostTexture &depth, const D3D12_RECT &rect, uint32_t dest_row)
    {
      HostTexture *color = g_post.waiting_color;
      if (!color || g_post.scene_frame != g.frame_number)
      {
        return;
      }
      uint32_t gw = ao2::aspect::GuestWidth(), gh = ao2::aspect::GuestHeight();
      if (rect.left || uint32_t(rect.right) * 10 < gw * 9 || (uint32_t(rect.bottom) + dest_row) * 10 < gh * 9)
      {
        return;
      }
      g_post.waiting_color = nullptr;
      auto it = g.resolved.find(g_post.waiting_base);
      if (it == g.resolved.end() || it->second.get() != color || color->generation != g_post.waiting_generation ||
          depth.format != DXGI_FORMAT_R32_FLOAT || depth.scale4 != g.scale4 ||
          ScaleSize(depth.width, depth.scale4) < g_post.width || ScaleSize(depth.height, depth.scale4) < g_post.height)
      {
        return;
      }
      PostStage1(*color, g_post.width, g_post.height, {depth.resource.Get(), &depth.state, depth.format});
      if (g_post.logged < 3)
      {
        ++g_post.logged;
        NativeLog("post effects: scene depth from its tiled resolve (%s)\n", g_post.depth_ready ? "kept" : "failed");
      }
    }

    // Stage 2: the scene's resolve is about to be sampled.
    void PostBeforeSample(HostTexture &tex, uint32_t address)
    {
      if (!g_post.pending || address != g_post.pending_base || &tex != g_post.pending_texture ||
          tex.generation != g_post.pending_generation)
      {
        return;
      }
      const RenderTarget *target = g_post.draw_target;
      if (target && target->edram_base == g_post.pending_src_base && target->edram_format == g_post.pending_src_format)
      {
        return; // drawn back into the scene: not the last resolve yet
      }
      g_post.pending = false;
      const PostSettings &s = CurrentPostSettings();
      uint32_t width = g_post.width, height = g_post.height;
      PostConstants c = {};
      bool camera = g_post.depth_ready && FillPostCamera(c);
      if (!camera)
      {
        c.view_size[0] = float(width);
        c.view_size[1] = float(height);
        c.view_size[2] = 1.0f / float(width);
        c.view_size[3] = 1.0f / float(height);
        c.p1[0] = 4.0f;
      }
      bool motion_blur = s.motion_blur > 0 && camera && g_post.has_prev;
      bool dof = s.dof > 0 && camera;
      if (!motion_blur && !dof && !s.aa)
      {
        return;
      }
      if (!EnsurePostTexture(g_post.color[0], width, height, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
          !EnsurePostTexture(g_post.color[1], width, height, DXGI_FORMAT_R16G16B16A16_FLOAT))
      {
        return;
      }
      float scale = float(g.scale4) * 0.25f;
      PostCopy(g_post.color[0].resource.Get(), g_post.color[0].state, tex.resource.Get(), tex.state, width, height);
      uint32_t cur = 0, passes = 0;
      auto capture = [&](const char *what) {
        if (g_trace)
        {
          char name[64];
          std::snprintf(name, sizeof(name), "%03llu_post_%s", (unsigned long long)g.capture_index++, what);
          CaptureTexture(g_post.color[cur].resource.Get(), g_post.color[cur].state, g_post.color[cur].format, name);
        }
      };
      capture("in");
      auto run = [&](PostPass pass, std::initializer_list<PostInput> extra) {
        PostTexture &from = g_post.color[cur], &to = g_post.color[cur ^ 1];
        bool ok = false;
        switch (extra.size())
        {
        case 0: ok = PostDraw(pass, to, 0, {In(from)}, c); break;
        case 1: ok = PostDraw(pass, to, 0, {In(from), extra.begin()[0]}, c); break;
        default: ok = PostDraw(pass, to, 0, {In(from), extra.begin()[0], extra.begin()[1]}, c); break;
        }
        if (ok)
        {
          cur ^= 1;
          ++passes;
        }
        return ok;
      };
      if (motion_blur)
      {
        // Blur of a 1/60 s exposure (high) or half of it (low) whatever the frame rate.
        double shutter = s.motion_blur >= 2 ? 1.0 : 0.5;
        c.p0[0] = float(std::min(shutter / (60.0 * g_post.frame_seconds), 2.0));
        c.p0[1] = (s.motion_blur >= 2 ? 48.0f : 24.0f) * scale;
        c.p0[2] = 250.0f;
        c.p0[3] = 450.0f;
        c.i0[0] = s.motion_blur >= 2 ? 16 : 10;
        run(kPassMotionBlur, {In(g_post.depth)});
      }
      if (dof && EnsurePostTexture(g_post.focus[0], 1, 1, DXGI_FORMAT_R32_FLOAT) &&
          EnsurePostTexture(g_post.focus[1], 1, 1, DXGI_FORMAT_R32_FLOAT))
      {
        PostTexture &prev = g_post.focus[g_post.focus_index], &next = g_post.focus[g_post.focus_index ^ 1];
        c.p0[0] = 150.0f;
        c.p0[1] = float(1.0 - std::exp(-g_post.frame_seconds * 6.0));
        c.i0[0] = g_post.focus_frame + 1 == g.frame_number ? 0u : 1u;
        if (PostDraw(kPassDofFocus, next, 0, {In(g_post.depth), In(prev)}, c))
        {
          g_post.focus_index ^= 1;
          g_post.focus_frame = g.frame_number;
          // A slight blur well behind the focus.
          c.p0[0] = 2.0f * scale;
          c.p0[1] = 1.3f;
          c.p0[2] = 0.8f;
          c.i0[0] = 12;
          run(kPassDof, {In(g_post.depth), In(next)});
        }
      }
      if (s.aa == 1)
      {
        c.p0[0] = 0.0312f;
        c.p0[1] = 0.125f;
        c.p0[2] = 0.75f;
        run(kPassFxaa, {});
      }
      else if (s.aa == 2 && EnsurePostTexture(g_post.edges, width, height, DXGI_FORMAT_R8G8_UNORM) &&
               EnsurePostTexture(g_post.weights, width, height, DXGI_FORMAT_R8G8B8A8_UNORM))
      {
        c.p0[0] = 0.07f;
        if (PostDraw(kPassSmaaEdges, g_post.edges, 0, {In(g_post.color[cur])}, c) &&
            PostDraw(kPassSmaaWeights, g_post.weights, 0, {In(g_post.edges)}, c))
        {
          run(kPassSmaaBlend, {In(g_post.weights)});
        }
      }
      // color[0] is a copy as well: an even number of passes ends there.
      if (passes)
      {
        capture("out");
        PostCopy(tex.resource.Get(), tex.state, g_post.color[cur].resource.Get(), g_post.color[cur].state, width,
                 height);
      }
    }

    // ------------------------------------------------------------ Shadow resolution
    //
    // ao2_shadow_resolution: the game's shadow depth surface (UE3's shadow
    // buffer, 768 texels square here, of which each light's shadow takes up
    // to the part its screen size asks for) is rendered with a scale of its
    // own, so the shadow map has about that many host texels across. Its
    // resolve keeps that scale and the shadow projection samples it at the
    // same coordinates: the same filter footprint with finer depth texels,
    // sharper and steadier edges. A surface counts as the shadow buffer once
    // a square part of it was resolved as depth into a square texture.

    struct ShadowSurface
    {
      uint32_t size = 0; // guest texels across the resolved shadow buffer
      uint32_t rows = 0; // rows of the surface ever resolved
      bool rejected = false; // also drawn with color targets: not a shadow buffer after all
    };
    std::unordered_map<uint64_t, ShadowSurface> g_shadow_surfaces; // base | format << 12 | pitch << 17
    int32_t g_shadow_setting = 0;
    bool g_shadow_dirty = false; // render targets to remake at the end of the frame

    uint64_t ShadowKey(uint32_t base, uint32_t format, uint32_t pitch)
    {
      return uint64_t(base) | (uint64_t(format) << 12) | (uint64_t(pitch) << 17);
    }

    // Scale of a depth surface's render targets, 0 = not a shadow surface (or
    // no shadow resolution chosen).
    uint32_t ShadowScale4(uint32_t base, uint32_t format, uint32_t pitch, uint32_t *rows)
    {
      if (g_shadow_setting <= 0)
      {
        return 0;
      }
      auto it = g_shadow_surfaces.find(ShadowKey(base, format, pitch));
      if (it == g_shadow_surfaces.end() || !it->second.size || it->second.rejected)
      {
        return 0;
      }
      if (rows)
      {
        *rows = it->second.rows;
      }
      uint32_t want = uint32_t(g_shadow_setting);
      return std::clamp((want * 4 + it->second.size - 1) / it->second.size, 4u, 64u);
    }

    // A depth resolve: notes the shadow buffer.
    void NoteShadowResolve(const RenderTarget &src, const D3D12_RECT &rect, uint32_t dest_pitch, uint32_t dest_height)
    {
      if (rect.right - rect.left != rect.bottom - rect.top || dest_pitch != dest_height || dest_pitch < 128 ||
          src.samples > 1)
      {
        return;
      }
      uint32_t format = src.edram_format;
      ShadowSurface &s = g_shadow_surfaces[ShadowKey(src.edram_base, format, src.edram_pitch)];
      if (s.rejected)
      {
        return;
      }
      uint32_t rows = std::max(s.rows, std::max(dest_height, uint32_t(rect.bottom)));
      if (s.size != dest_pitch || s.rows != rows)
      {
        NativeLog("shadow buffer: depth base %u fmt %u pitch %u, %u texels across (%u rows)\n", src.edram_base, format,
                  src.edram_pitch, dest_pitch, rows);
        s.size = dest_pitch;
        s.rows = rows;
        g_shadow_dirty = true;
      }
    }

    // Drops render targets (their EDRAM contents with them); they are made
    // again on first use.
    template <typename F>
    void DropRenderTargets(F &&which)
    {
      bool any = false;
      for (auto it = g.render_targets.begin(); it != g.render_targets.end();)
      {
        RenderTarget *rt = it->second.get();
        if (!which(*rt))
        {
          ++it;
          continue;
        }
        std::replace(std::begin(g.edram_owner), std::end(g.edram_owner), rt, static_cast<RenderTarget *>(nullptr));
        if (g.pending_depth.rt == rt)
        {
          g.pending_depth = {};
        }
        if (g.edram_save.origin == rt)
        {
          g.edram_save = {};
        }
        std::erase_if(g.lent, [&](const State::Lent &l) { return l.rt == rt; });
        DeferRelease(std::move(rt->resource));
        it = g.render_targets.erase(it);
        any = true;
      }
      if (any)
      {
        ++g.edram_generation;
        g.bound = {};
        g_post.candidate_count = 0;
        g_post.pending = false;
      }
    }

    // End of a frame: the shadow resolution asked for, the shadow surfaces
    // learnt.
    void ApplyShadowResolution()
    {
      int32_t want = REXCVAR_GET(ao2_shadow_resolution);
      want = want >= 4096 ? 4096 : want >= 2048 ? 2048 : want >= 1024 ? 1024 : 0;
      if (want == g_shadow_setting && !g_shadow_dirty)
      {
        return;
      }
      if (want != g_shadow_setting)
      {
        NativeLog("shadow resolution %d from frame %llu\n", want, (unsigned long long)g.frame_number);
      }
      g_shadow_setting = want;
      g_shadow_dirty = false;
      DropRenderTargets([](const RenderTarget &rt) {
        return rt.depth && g_shadow_surfaces.count(ShadowKey(rt.edram_base, rt.edram_format, rt.edram_pitch)) &&
               rt.samples == 1;
      });
    }

    // ------------------------------------------------------------ Menu probe

    // The probe target matching render target `rt` (made or remade for it,
    // unless this frame drew into another one already).
    bool EnsureProbeTarget(const RenderTarget &rt)
    {
      D3D12_RESOURCE_DESC rd = rt.resource->GetDesc();
      uint32_t w = uint32_t(rd.Width), h = rd.Height;
      auto &p = g.probe;
      if (p.target && p.width == w && p.height == h && p.samples == rt.samples)
      {
        p.scale4 = rt.scale4;
        return true;
      }
      if (p.drawn_frame == g.frame_number)
      {
        return false;
      }
      DeferRelease(std::move(p.target));
      DeferRelease(std::move(p.resolved));
      D3D12_RESOURCE_DESC d = {};
      d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      d.Width = w;
      d.Height = h;
      d.DepthOrArraySize = 1;
      d.MipLevels = 1;
      d.SampleDesc.Count = rt.samples;
      d.Format = kProbeFormat;
      d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
      p.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
      if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, p.state, nullptr,
                                                   IID_PPV_ARGS(&p.target))))
      {
        p.width = 0;
        return false;
      }
      if (rt.samples > 1)
      {
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        p.resolved_state = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, p.resolved_state, nullptr,
                                                     IID_PPV_ARGS(&p.resolved))))
        {
          p.target.Reset();
          p.width = 0;
          return false;
        }
      }
      p.width = w;
      p.height = h;
      p.samples = rt.samples;
      p.scale4 = rt.scale4;
      NativeLog("menu probe target %ux%u x%u\n", w, h, rt.samples);
      return true;
    }

    // Draws a probed draw again (`issue`, with its state bound) with the probe
    // variant of its pixel shader into the probe target: its depth test, no
    // depth or stencil writes, no blending.
    template <typename Issue>
    void DrawProbe(const PipelineKey &key, const CompiledShader *vs, const CompiledShader *probe_ps,
                   const RenderTarget &rt, const RenderTarget *ds, const Issue &issue)
    {
      if (!EnsureProbeTarget(rt))
      {
        return;
      }
      PipelineKey pk = key;
      pk.ps = probe_ps;
      std::memset(pk.rt_formats, 0, sizeof(pk.rt_formats));
      std::memset(pk.write_mask, 0, sizeof(pk.write_mask));
      std::memset(pk.blend, 0, sizeof(pk.blend));
      pk.rt_formats[0] = uint8_t(kProbeFormat);
      pk.write_mask[0] = 0xF;
      reg::RB_DEPTHCONTROL dc;
      dc.value = pk.depth_control;
      dc.z_write_enable = 0;
      pk.depth_control = dc.value;
      pk.stencil_masks &= 0x00FF00FFu;
      ID3D12PipelineState *pso = GetPipeline(pk, vs, probe_ps);
      if (!pso)
      {
        return;
      }
      auto &p = g.probe;
      auto *list = g.list.Get();
      Transition(p.target.Get(), p.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.transient_rtv_heap->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += uint64_t(g.transient_rtvs_used++ % kTransientRtvCount) * g.rtv_size;
      D3D12_RENDER_TARGET_VIEW_DESC rv = {};
      rv.Format = kProbeFormat;
      rv.ViewDimension = p.samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
      g.device->CreateRenderTargetView(p.target.Get(), &rv, rtv);
      if (p.drawn_frame != g.frame_number)
      {
        const float zero[4] = {};
        list->ClearRenderTargetView(rtv, zero, 0, nullptr);
        p.drawn_frame = g.frame_number;
      }
      list->OMSetRenderTargets(1, &rtv, FALSE, ds ? &ds->view : nullptr);
      list->SetPipelineState(pso);
      issue();
      // The next draw binds its own targets and pipeline again.
      g.bound.rtv_count = UINT32_MAX;
      g.bound.pso = nullptr;
    }

    // End of the frame: the probed coordinates under the cursor go to the
    // frame's readback buffer (read when the frame slot is reused).
    void ReadProbe()
    {
      Frame &f = CurrentFrame();
      uint32_t size = g_probe_size.load(std::memory_order_relaxed);
      f.probe_offset = size ? kProbeNone : kProbeOff;
      f.probe_size = size;
      auto &p = g.probe;
      if (!size || p.drawn_frame != g.frame_number || !p.target)
      {
        return;
      }
      uint32_t cursor = g_probe_cursor.load(std::memory_order_relaxed);
      uint32_t x = std::min((cursor & 0xFFFF) * p.scale4 / 4, p.width - 1);
      uint32_t y = std::min((cursor >> 16) * p.scale4 / 4, p.height - 1);
      uint64_t offset = (f.readback_offset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                        ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
      if (offset + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT > kReadbackSize)
      {
        return;
      }
      auto *list = g.list.Get();
      ID3D12Resource *source = p.target.Get();
      D3D12_RESOURCE_STATES *state = &p.state;
      if (p.samples > 1)
      {
        Transition(p.target.Get(), p.state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        Transition(p.resolved.Get(), p.resolved_state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        list->ResolveSubresource(p.resolved.Get(), 0, p.target.Get(), 0, kProbeFormat);
        source = p.resolved.Get();
        state = &p.resolved_state;
      }
      Transition(source, *state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst = {f.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint.Offset = offset;
      dst.PlacedFootprint.Footprint = {kProbeFormat, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
      D3D12_TEXTURE_COPY_LOCATION src = {source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_BOX box = {x, y, 0, x + 1, y + 1, 1};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
      f.readback_offset = offset + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
      f.probe_offset = offset;
    }

    // HlslModification::tex_post of the pixel shader for this draw: each
    // binding's post-processing from its fetch constant, as PrepareTextures
    // gives it to xe_tex_info (signed components it may clear do nothing in
    // the shader either way).
    uint64_t StaticTexPost(ShaderCache &shaders, const ShaderMicrocode &code, const RegisterFile &r)
    {
      Shader *shader = shaders.GetShader(xenos::ShaderType::kPixel, code.dwords, code.dword_count, code.hash);
      if (!shader)
      {
        return 0;
      }
      constexpr uint32_t kGamma = uint32_t(xenos::TextureSign::kGamma);
      constexpr uint32_t kBiased = uint32_t(xenos::TextureSign::kUnsignedBiased);
      uint64_t post = 0;
      for (const auto &tb : shader->texture_bindings())
      {
        if (tb.binding_index >= 16)
        {
          continue;
        }
        auto fetch = r.Get<xenos::xe_gpu_texture_fetch_t>(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + tb.fetch_constant * 6);
        uint32_t signs = texture_util::SwizzleSigns(fetch);
        uint32_t gamma = 0, biased = 0;
        for (uint32_t i = 0; i < 4; ++i)
        {
          uint32_t sign = (signs >> (i * 2)) & 3;
          gamma |= uint32_t(sign == kGamma) << i;
          biased |= uint32_t(sign == kBiased) << i;
        }
        uint64_t mode = biased ? 0 : (gamma == 0 ? 1 : (gamma == 0x7 ? 2 : 0));
        post |= mode << (2 * tb.binding_index);
      }
      return post;
    }
  } // namespace

  // ------------------------------------------------------------ Draw

  void RendererDraw(Pm4Consumer &pm4, const DrawPacket &d, ShaderCache &shaders)
  {
    if (!Ready())
    {
      return;
    }
    if (g.occlusion_active)
    {
      ++g.occlusion.draws; // a draw that returns before being counted makes the query unknown
    }
    const RegisterFile &r = pm4.regs();
    auto edram_mode = r.Get<reg::RB_MODECONTROL>().edram_mode;
    if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly)
    {
      return;
    }
    auto surface = r.Get<reg::RB_SURFACE_INFO>();
    if (!surface.surface_pitch || !d.index_count)
    {
      return;
    }
    const auto &vs_code = pm4.vertex_shader();
    if (!vs_code.dword_count)
    {
      return;
    }
    PrewarmStep(shaders);
    auto program_cntl = r.Get<reg::SQ_PROGRAM_CNTL>();
    auto context_misc = r.Get<reg::SQ_CONTEXT_MISC>();
    PhaseTimer timer;

    // List and strip draws go straight to the host (indices fetched in the
    // vertex shader); fans, quads and restarting strips get a host index list.
    static thread_local std::vector<uint32_t> indices;
    indices.clear();
    bool rect = false;
    uint32_t draw_count = 0;
    bool index_fetch = false;
    D3D12_PRIMITIVE_TOPOLOGY topology = DirectTopology(d, r, rect);
    if (topology != D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)
    {
      draw_count = d.index_count;
      if (d.indexed)
      {
        uint32_t available = d.index_words * (d.index_format == xenos::IndexFormat::kInt32 ? 1u : 2u);
        if (available)
        {
          draw_count = std::min(draw_count, available);
        }
        index_fetch = true;
      }
    }
    else
    {
      topology = BuildIndices(d, indices, rect);
      draw_count = uint32_t(indices.size());
    }
    if (topology == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED || !draw_count)
    {
      ++g.draws_skipped;
      return;
    }

    uint64_t shader_start = __rdtsc();
    HlslModification vm;
    vm.register_count = program_cntl.vs_num_reg + 1;
    const CompiledShader *vs =
        shaders.Get(xenos::ShaderType::kVertex, vs_code.dwords, vs_code.dword_count, vs_code.hash, vm.value);
    const CompiledShader *ps = nullptr;
    const CompiledShader *probe_ps = nullptr; // the menu probe's variant
    const auto &ps_code = pm4.pixel_shader();
    if (edram_mode == xenos::EdramMode::kColorDepth && ps_code.dword_count)
    {
      HlslModification pm;
      pm.register_count = program_cntl.ps_num_reg + 1;
      pm.param_gen_register = program_cntl.param_gen ? (context_misc.param_gen_pos & 31) : 31;
      auto colorcontrol = r.Get<reg::RB_COLORCONTROL>();
      pm.alpha_test = colorcontrol.alpha_test_enable && colorcontrol.alpha_func != xenos::CompareFunction::kAlways;
      if (REXCVAR_GET(ao2_native_gpu_static_tex_post))
      {
        pm.tex_post = StaticTexPost(shaders, ps_code, r);
      }
      ps = shaders.Get(xenos::ShaderType::kPixel, ps_code.dwords, ps_code.dword_count, ps_code.hash, pm.value);
      uint32_t probe_size = g_probe_size.load(std::memory_order_relaxed);
      if (ps && probe_size && ps->shader)
      {
        for (const auto &tb : ps->shader->texture_bindings())
        {
          if (tb.binding_index >= 16 || tb.fetch_instr.dimension != xenos::FetchOpDimension::k2D)
          {
            continue;
          }
          auto fetch = r.Get<xenos::xe_gpu_texture_fetch_t>(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                                            tb.fetch_constant * 6);
          if (fetch.type == xenos::FetchConstantType::kTexture &&
              ((fetch.size_2d.width + 1) | ((fetch.size_2d.height + 1) << 16)) == probe_size)
          {
            HlslModification probe = pm;
            probe.probe_binding = uint32_t(tb.binding_index) + 1;
            probe_ps = shaders.Get(xenos::ShaderType::kPixel, ps_code.dwords, ps_code.dword_count, ps_code.hash,
                                   probe.value);
            break;
          }
        }
      }
    }
    {
      uint64_t shader_ns = TscElapsedNs(shader_start);
      g.frame_shader_ns += shader_ns;
      if (shader_ns > 200000)
      {
        ++g.frame_shader_slow;
      }
    }
    if (!vs || vs->pending() || (ps && ps->pending()))
    {
      // Still compiling on the shader cache's worker threads.
      ++g.draws_pending;
      return;
    }
    if (!vs->ok() || (ps && !ps->ok()) ||
        SkipDrawForDebug(vs->shader->ucode_data_hash(), ps ? ps->shader->ucode_data_hash() : 0))
    {
      ++g.draws_skipped;
      return;
    }

    BeginFrameIfNeeded();
    timer.Mark(0);

    // Render targets.
    PipelineKey key;
    key.vs = vs;
    key.ps = ps;
    key.samples = uint8_t(1u << uint32_t(surface.msaa_samples));
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {};
    RenderTarget *color_rts[4] = {};
    uint32_t rtv_count = 0;
    uint32_t rt_width = surface.surface_pitch, rt_height = 2048;
    // The draw's render targets are of one surface: one resolution scale.
    uint32_t scale4 = SurfaceScale4(surface.surface_pitch, key.samples);
    auto color_mask = r.v[XE_GPU_REG_RB_COLOR_MASK];
    if (ps && edram_mode == xenos::EdramMode::kColorDepth)
    {
      uint32_t written = ps->shader->writes_color_targets();
      for (uint32_t i = 0; i < 4; ++i)
      {
        uint32_t mask = (color_mask >> (i * 4)) & 0xF;
        if (!(written & (1u << i)) || !mask)
        {
          continue;
        }
        auto ci = r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
        RenderTarget *rt = GetRenderTarget(ci.color_base, uint32_t(ci.color_format), false,
                                           surface.surface_pitch, surface.msaa_samples);
        if (!rt)
        {
          continue;
        }
        Transition(rt->resource.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        bool gamma = ci.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA;
        rtvs[i] = gamma ? rt->view_srgb : rt->view;
        color_rts[i] = rt;
        rtv_count = i + 1;
        key.rt_formats[i] = uint8_t(gamma ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : rt->format);
        key.write_mask[i] = uint8_t(mask);
        uint32_t blend = r.v[reg::RB_BLENDCONTROL::rt_register_indices[i]];
        reg::RB_BLENDCONTROL b;
        b.value = blend;
        bool opaque = b.color_srcblend == xenos::BlendFactor::kOne &&
                      b.color_destblend == xenos::BlendFactor::kZero &&
                      b.color_comb_fcn == xenos::BlendOp::kAdd &&
                      b.alpha_srcblend == xenos::BlendFactor::kOne &&
                      b.alpha_destblend == xenos::BlendFactor::kZero &&
                      b.alpha_comb_fcn == xenos::BlendOp::kAdd;
        key.blend[i] = opaque ? 0 : (blend ? blend : 0x80000000u);
        rt_height = std::min(rt_height, rt->height);
      }
      // Fill gaps in the RTV array with unused null descriptors is not allowed;
      // use the first bound one's format for holes with zero write mask.
      for (uint32_t i = 0; i < rtv_count; ++i)
      {
        if (!rtvs[i].ptr)
        {
          rtvs[i] = rtvs[rtv_count - 1];
          key.rt_formats[i] = key.rt_formats[rtv_count - 1];
          key.write_mask[i] = 0;
        }
      }
    }
    auto depth_control = r.Get<reg::RB_DEPTHCONTROL>();
    RenderTarget *ds = nullptr;
    float depth_offset = 0.0f; // guest polygon offset (constant part, absolute depth)
    if (depth_control.z_enable || depth_control.stencil_enable)
    {
      auto di = r.Get<reg::RB_DEPTH_INFO>();
      ds = GetRenderTarget(di.depth_base, uint32_t(di.depth_format), true, surface.surface_pitch,
                           surface.msaa_samples);
      if (ds && ds->scale4 != scale4)
      {
        // The shadow buffer has a scale of its own (ao2_shadow_resolution):
        // its depth-only passes take it. Drawn with color it was no shadow
        // buffer - its render targets are remade at the end of the frame.
        auto shadow = g_shadow_surfaces.find(ShadowKey(ds->edram_base, ds->edram_format, ds->edram_pitch));
        if (rtv_count == 0)
        {
          scale4 = ds->scale4;
        }
        else
        {
          if (shadow != g_shadow_surfaces.end() && !shadow->second.rejected)
          {
            NativeLog("shadow buffer: depth base %u fmt %u pitch %u drawn with color, not a shadow buffer\n",
                      ds->edram_base, ds->edram_format, ds->edram_pitch);
            shadow->second.rejected = true;
            g_shadow_dirty = true;
          }
          ++g.draws_skipped;
          return;
        }
      }
      if (ds)
      {
        Transition(ds->resource.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        key.ds_format = uint8_t(ds->format);
        key.depth_control = depth_control.value;
        auto sref = r.Get<reg::RB_STENCILREFMASK>();
        key.stencil_masks = sref.stencilmask | (sref.stencilwritemask << 8);
        rt_height = std::min(rt_height, ds->height);
        if (depth_control.z_enable)
        {
          PolygonOffset(r, TopologyType(topology) == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && !rect, scale4,
                        depth_offset, key.depth_bias_slope);
        }
      }
    }
    if (!rtv_count && !ds)
    {
      ++g.draws_skipped;
      return;
    }
    if (rtv_count == 0)
    {
      rt_height = ds->height;
    }

    timer.Mark(1);
    auto mode = r.Get<reg::PA_SU_SC_MODE_CNTL>();
    key.raster = (mode.value & 0x7) | (r.Get<reg::PA_CL_CLIP_CNTL>().clip_disable ? 0x80000000u : 0);
    key.topology = uint8_t(TopologyType(topology));
    key.rect_gs = rect;
    ID3D12PipelineState *pso = GetPipeline(key, vs, ps);
    if (!pso)
    {
      ++(g.pso_queued.count(key) ? g.draws_pending : g.draws_skipped);
      return;
    }

    timer.Mark(2);
    TraceDraw(r, d, vs, ps);
    UploadVertexRanges(vs, r, index_fetch ? (d.index_base & 0x1FFFFFFF) : 0,
                       index_fetch ? draw_count * (d.index_format == xenos::IndexFormat::kInt32 ? 4u : 2u) : 0);
    timer.Mark(3);

    // Everything that may submit the command list mid-frame (texture uploads,
    // sampler heap reset) happens before this draw's state is recorded.
    uint32_t tex_info[16][4] = {};
    float tex_size[16][4] = {};
    uint32_t tex_info_used = 0;
    TextureTable ps_textures, vs_textures;
    bool lowres = rtv_count && SmallTarget(surface.surface_pitch, key.samples);
    g_post.draw_target = color_rts[0];
    PrepareTextures(ps, r, ps_textures, tex_info, tex_size, tex_info_used, lowres);
    PrepareTextures(vs, r, vs_textures, tex_info, tex_size, tex_info_used, lowres);
    g_post.draw_target = nullptr;
    timer.Mark(4);
    ReserveSamplerTables(2);
    D3D12_GPU_DESCRIPTOR_HANDLE ps_samplers = BindSamplers(ps, r);
    D3D12_GPU_DESCRIPTOR_HANDLE vs_samplers = BindSamplers(vs, r);
    timer.Mark(5);
    D3D12_GPU_DESCRIPTOR_HANDLE ps_table = WriteTextureTable(ps_textures, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE vs_table = WriteTextureTable(vs_textures, 1);
    // Split-screen: a button picture is drawn once per player's part of the
    // screen, ours for the keyboard player, the guest's for the other.
    D3D12_GPU_DESCRIPTOR_HANDLE ps_guest_table = {};
    float split_keys[4], split_pad[4];
    bool split_draw = ps_textures.dual && ao2::prompts::SplitRegions(split_keys, split_pad);
    if (split_draw)
    {
      TextureTable guest = ps_textures;
      for (uint32_t dual = guest.dual; dual; dual &= dual - 1)
      {
        uint32_t i = uint32_t(__builtin_ctz(dual));
        guest.resources[i] = guest.guest_resources[i];
        guest.views[i] = guest.guest_views[i];
      }
      ps_guest_table = WriteTextureTable(guest, 0);
    }
    timer.Mark(6);

    Viewport vp = ComputeViewport(r, rt_width, rt_height, ps && ps->shader->writes_depth());
    NoteSceneCamera(r, ds, vp.viewport, depth_control);
    Trace("    viewport %.0f,%.0f %.0fx%.0f z %g..%g (guest z scale %g offset %g%s) scissor %d,%d-%d,%d\n",
          vp.viewport.TopLeftX, vp.viewport.TopLeftY, vp.viewport.Width, vp.viewport.Height, vp.viewport.MinDepth,
          vp.viewport.MaxDepth, r.F(XE_GPU_REG_PA_CL_VPORT_ZSCALE), r.F(XE_GPU_REG_PA_CL_VPORT_ZOFFSET),
          ps && ps->shader->writes_depth() ? ", ps writes depth" : "", int(vp.scissor.left), int(vp.scissor.top),
          int(vp.scissor.right), int(vp.scissor.bottom));
    // Bring in what other surfaces left in this draw's EDRAM area, then make
    // sure the targets are bound in the right states again.
    D3D12_RECT covered = vp.scissor;
    bool exact_rect = RectListExtent(r, vs, d, vp, covered);
    if (exact_rect)
    {
      Trace("    rect list covers %d,%d-%d,%d\n", int(covered.left), int(covered.top), int(covered.right),
            int(covered.bottom));
    }
    // A clear-like rectangle (D3D's fast clears) overwrites everything it
    // covers: what other surfaces left in that EDRAM need not be brought in.
    bool may_discard =
        exact_rect && !(ps && (ps->shader->kills_pixels() || r.Get<reg::RB_COLORCONTROL>().alpha_test_enable));
    // D3D's clear draws: values from the vertices (position z, color at dword 3).
    ClearDrawValues clear_values;
    if (exact_rect)
    {
      GetClearDrawValues(r, vs, ps, d, vp, clear_values);
    }
    if (REXCVAR_GET(ao2_native_gpu_resolve_pack) && SkipCopyBack(r, vs, ps, d, vp, color_rts, rtv_count, ds, color_mask, key.blend[0], key.rt_formats[0],
                     covered))
    {
      ++g.copy_backs_skipped;
      return;
    }
    static std::vector<uint32_t> covered_tiles;
    if (g.pending_depth.rt)
    {
      const D3D12_RECT &pr = g.pending_depth.rect;
      auto fetch = r.Get<xenos::xe_gpu_texture_fetch_t>(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
      auto keeps = [](xenos::StencilOp a, xenos::StencilOp b, xenos::StencilOp c) {
        return a == xenos::StencilOp::kKeep && b == xenos::StencilOp::kKeep && c == xenos::StencilOp::kKeep;
      };
      bool stencil_kept =
          !depth_control.stencil_enable ||
          (keeps(depth_control.stencilfail, depth_control.stencilzfail, depth_control.stencilzpass) &&
           (!depth_control.backface_enable ||
            keeps(depth_control.stencilfail_bf, depth_control.stencilzfail_bf, depth_control.stencilzpass_bf)));
      bool restore = ds == g.pending_depth.rt && !rtv_count && ps && ps->shader->writes_depth() &&
                     covered.left <= pr.left && covered.top <= pr.top && covered.right >= pr.right &&
                     covered.bottom >= pr.bottom && vp.viewport.TopLeftX <= float(pr.left) &&
                     vp.viewport.TopLeftY <= float(pr.top) &&
                     vp.viewport.TopLeftX + vp.viewport.Width >= float(pr.right) &&
                     vp.viewport.TopLeftY + vp.viewport.Height >= float(pr.bottom) && depth_control.z_enable &&
                     depth_control.z_write_enable &&
                     depth_control.zfunc == xenos::CompareFunction::kAlways && stencil_kept &&
                     ((fetch.base_address & 0x1FFFF) << 12) == g.pending_depth.dest && !g.occlusion_active;
      if (restore)
      {
        Trace("    depth restore from %08X skipped with its clear\n", g.pending_depth.dest);
        g.pending_depth = {};
        ++g.depth_restores_skipped;
        return;
      }
    }
    if (rtv_count)
    {
      FlushPendingDepthClear();
    }
    for (uint32_t i = 0; i < 4; ++i)
    {
      if (RenderTarget *rt = color_rts[i])
      {
        bool overwrite = may_discard && ((color_mask >> (i * 4)) & 0xF) == 0xF && key.blend[i] == 0;
        ClaimEdram(*rt, covered, overwrite ? 1u : 0u);
        ++rt->writes;
        auto ci = r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
        ForgetUniform(*rt, 0, covered);
        if (overwrite && clear_values.color_ok && !ci.color_exp_bias && UniformColorFormat(rt->edram_format) &&
            ci.color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)
        {
          TilesInside(*rt, covered, covered_tiles);
          SetUniform(*rt, 0, covered_tiles, PackColorRaw(clear_values.color, rt->edram_format));
        }
      }
    }
    if (ds)
    {
      auto sref = r.Get<reg::RB_STENCILREFMASK>();
      auto replaces = [](xenos::CompareFunction func, xenos::StencilOp zpass) {
        return func == xenos::CompareFunction::kAlways && zpass == xenos::StencilOp::kReplace;
      };
      bool depth_overwrite = may_discard && depth_control.z_enable && depth_control.z_write_enable &&
                             depth_control.zfunc == xenos::CompareFunction::kAlways;
      bool stencil_overwrite = may_discard && depth_control.stencil_enable && sref.stencilwritemask == 0xFF &&
                               replaces(depth_control.stencilfunc, depth_control.stencilzpass) &&
                               (!depth_control.backface_enable ||
                                replaces(depth_control.stencilfunc_bf, depth_control.stencilzpass_bf));
      // A stencil clear through an aliasing surface (ClearStencilOnOwner). Not
      // inside an occlusion query: a draw that returns early makes it unknown.
      if (stencil_overwrite && clear_values.vertices_ok && !rtv_count && !depth_control.z_enable &&
          !g.occlusion_active && REXCVAR_GET(ao2_native_gpu_alias_stencil_clear) &&
          ClearStencilOnOwner(*ds, covered, uint8_t(sref.stencilref)))
      {
        ++g.stencil_clears_redirected;
        return;
      }
      FlushPendingDepthClear();
      if (depth_control.stencil_enable)
      {
        ds->stencil_used = true;
      }
      ClaimEdram(*ds, covered, (depth_overwrite ? 1u : 0u) | (stencil_overwrite ? 2u : 0u));
      // Partially covered tiles hold mixed values: only whole ones become uniform.
      if (depth_control.z_enable && depth_control.z_write_enable)
      {
        ForgetUniform(*ds, 0, covered);
        if (depth_overwrite && clear_values.depth_ok)
        {
          TilesInside(*ds, covered, covered_tiles);
          SetUniform(*ds, 0, covered_tiles, EncodeDepth24(clear_values.Depth(vp), ds->edram_format) << 8);
        }
      }
      if (depth_control.stencil_enable && sref.stencilwritemask)
      {
        ForgetUniform(*ds, 1, covered);
        if (stencil_overwrite && clear_values.vertices_ok)
        {
          TilesInside(*ds, covered, covered_tiles);
          SetUniform(*ds, 1, covered_tiles, sref.stencilref);
        }
      }
      Transition(ds->resource.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    if (g_trace && color_rts[0] && CaptureDrawRequested(g.draws, vs->shader->ucode_data_hash()))
    {
      char name[64];
      std::snprintf(name, sizeof(name), "%03llu_before%llu_rt%u_fmt%u", (unsigned long long)g.capture_index++,
                    (unsigned long long)g.draws, color_rts[0]->edram_base, color_rts[0]->edram_format);
      CaptureTexture(color_rts[0]->resource.Get(), color_rts[0]->state, color_rts[0]->format, name);
    }
    for (RenderTarget *rt : color_rts)
    {
      if (rt)
      {
        Transition(rt->resource.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      }
    }
    timer.Mark(7);
    XeSystemConstants sys;
    FillSystemConstants(r, vp, tex_info, tex_size, index_fetch ? &d : nullptr, scale4, sys);
    if (depth_offset != 0.0f)
    {
      // Host NDC z [0, 1] spans the viewport's depth range: offset / range in
      // NDC (times w, like ndc_offset) is `depth_offset` in the depth buffer.
      float range = vp.viewport.MaxDepth - vp.viewport.MinDepth;
      if (range > 0.0f)
      {
        sys.ndc_offset[2] += depth_offset / range;
      }
    }
    D3D12_GPU_VIRTUAL_ADDRESS vs_constants = UploadFloatConstants(r, false);
    D3D12_GPU_VIRTUAL_ADDRESS ps_constants = UploadFloatConstants(r, true);
    D3D12_GPU_VIRTUAL_ADDRESS sys_constants = Upload(&sys, sizeof(sys));
    D3D12_GPU_VIRTUAL_ADDRESS ib = indices.empty() ? 0 : Upload(indices.data(), indices.size() * 4, 4);
    if (!vs_constants || !ps_constants || !sys_constants || (!indices.empty() && !ib))
    {
      ++g.draws_skipped;
      return;
    }
    timer.Mark(8);

    auto *list = g.list.Get();
    auto &b = g.bound;
    bool fresh = !b.valid;
    if (fresh)
    {
      list->SetGraphicsRootSignature(g.root_signature.Get());
      list->SetGraphicsRootShaderResourceView(kRootSharedMemory, g.shared_memory->GetGPUVirtualAddress());
      b.valid = true;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ds ? ds->view : D3D12_CPU_DESCRIPTOR_HANDLE{};
    if (fresh || b.rtv_count != rtv_count || b.dsv.ptr != dsv.ptr ||
        std::memcmp(b.rtvs, rtvs, rtv_count * sizeof(rtvs[0])))
    {
      list->OMSetRenderTargets(rtv_count, rtv_count ? rtvs : nullptr, FALSE, ds ? &ds->view : nullptr);
      b.rtv_count = rtv_count;
      std::memcpy(b.rtvs, rtvs, sizeof(rtvs));
      b.dsv = dsv;
    }
    D3D12_VIEWPORT host_viewport = vp.viewport;
    float host_scale = float(scale4) * 0.25f;
    host_viewport.TopLeftX *= host_scale;
    host_viewport.TopLeftY *= host_scale;
    host_viewport.Width *= host_scale;
    host_viewport.Height *= host_scale;
    D3D12_RECT host_scissor = ScaleRect(vp.scissor, scale4);
    if (fresh || std::memcmp(&b.viewport, &host_viewport, sizeof(b.viewport)))
    {
      list->RSSetViewports(1, &host_viewport);
      b.viewport = host_viewport;
    }
    if (fresh || std::memcmp(&b.scissor, &host_scissor, sizeof(b.scissor)))
    {
      list->RSSetScissorRects(1, &host_scissor);
      b.scissor = host_scissor;
    }
    float blend_factor[4] = {r.F(XE_GPU_REG_RB_BLEND_RED), r.F(XE_GPU_REG_RB_BLEND_GREEN),
                             r.F(XE_GPU_REG_RB_BLEND_BLUE), r.F(XE_GPU_REG_RB_BLEND_ALPHA)};
    if (fresh || std::memcmp(b.blend, blend_factor, sizeof(blend_factor)))
    {
      list->OMSetBlendFactor(blend_factor);
      std::memcpy(b.blend, blend_factor, sizeof(blend_factor));
    }
    uint32_t stencil_ref = r.Get<reg::RB_STENCILREFMASK>().stencilref;
    if (fresh || b.stencil_ref != stencil_ref)
    {
      list->OMSetStencilRef(stencil_ref);
      b.stencil_ref = stencil_ref;
    }
    if (fresh || b.pso != pso)
    {
      list->SetPipelineState(pso);
      b.pso = pso;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS cbvs[3] = {vs_constants, ps_constants, sys_constants};
    const UINT cbv_params[3] = {kRootVSConstants, kRootPSConstants, kRootSystemConstants};
    for (uint32_t i = 0; i < 3; ++i)
    {
      if (fresh || b.cbv[i] != cbvs[i])
      {
        list->SetGraphicsRootConstantBufferView(cbv_params[i], cbvs[i]);
        b.cbv[i] = cbvs[i];
      }
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE tables[4] = {ps_table, vs_table, ps_samplers, vs_samplers};
    const UINT table_params[4] = {kRootPSTextures, kRootVSTextures, kRootPSSamplers, kRootVSSamplers};
    for (uint32_t i = 0; i < 4; ++i)
    {
      if (fresh || b.tables[i].ptr != tables[i].ptr)
      {
        list->SetGraphicsRootDescriptorTable(table_params[i], tables[i]);
        b.tables[i] = tables[i];
      }
    }
    if (fresh || b.topology != topology)
    {
      list->IASetPrimitiveTopology(topology);
      b.topology = topology;
    }
    GpuMark(kGpuDraw);
    Crumb("draw", uint64_t(pso), draw_count, rtv_count, ds ? uint64_t(ds->resource.Get()) : 0);
    uint32_t stamp_begin = g_trace ? GpuStamp(kGpuDraw) : UINT32_MAX;
    // Only the draw itself is counted, not the EDRAM transfers recorded for it.
    uint32_t occlusion_index = g.occlusion_active ? BeginOcclusionSegment() : UINT32_MAX;
    if (!indices.empty())
    {
      D3D12_INDEX_BUFFER_VIEW ibv = {ib, UINT(indices.size() * 4), DXGI_FORMAT_R32_UINT};
      list->IASetIndexBuffer(&ibv);
    }
    auto issue_part = [&](uint32_t start, uint32_t count) {
      if (indices.empty())
      {
        list->DrawInstanced(count, 1, start, 0);
      }
      else
      {
        list->DrawIndexedInstanced(count, 1, start, 0, 0);
      }
    };
    auto issue = [&] { issue_part(0, indices.empty() ? draw_count : UINT(indices.size())); };
    static thread_local std::vector<PromptRun> runs;
    if (!split_draw)
    {
      issue();
    }
    else if (PromptRuns(vs, r, index_fetch, indices, draw_count, topology, vp, split_keys, split_pad, runs))
    {
      // Each prompt as its player sees it. One the players share (across
      // both parts, or in neither) is drawn twice side by side, centered
      // where it was: the key on the left, the gamepad's button on the right.
      for (const PromptRun &run : runs)
      {
        if (run.part < 2)
        {
          list->SetGraphicsRootDescriptorTable(kRootPSTextures, run.part == 0 ? ps_table : ps_guest_table);
          issue_part(run.start, run.count);
          continue;
        }
        float half = ((run.box[2] - run.box[0]) * 0.5f + 1.0f) * host_scale;
        for (int side = 0; side < 2; ++side)
        {
          D3D12_VIEWPORT moved = host_viewport;
          moved.TopLeftX += side == 0 ? -half : half;
          list->RSSetViewports(1, &moved);
          list->SetGraphicsRootDescriptorTable(kRootPSTextures, side == 0 ? ps_table : ps_guest_table);
          issue_part(run.start, run.count);
        }
        list->RSSetViewports(1, &host_viewport);
      }
      list->SetGraphicsRootDescriptorTable(kRootPSTextures, ps_table);
      b.tables[0] = ps_table;
    }
    else
    {
      // The players' parts of the guest back buffer, in host pixels.
      float gw = float(ao2::aspect::GuestWidth()), gh = float(ao2::aspect::GuestHeight());
      for (int pass = 0; pass < 2; ++pass)
      {
        const float *f = pass == 0 ? split_keys : split_pad;
        D3D12_RECT part = {LONG(std::lround(f[0] * gw)), LONG(std::lround(f[1] * gh)), LONG(std::lround(f[2] * gw)),
                           LONG(std::lround(f[3] * gh))};
        part = ScaleRect(part, scale4);
        D3D12_RECT clip = {std::max(part.left, host_scissor.left), std::max(part.top, host_scissor.top),
                           std::min(part.right, host_scissor.right), std::min(part.bottom, host_scissor.bottom)};
        if (clip.left >= clip.right || clip.top >= clip.bottom)
        {
          continue;
        }
        list->RSSetScissorRects(1, &clip);
        list->SetGraphicsRootDescriptorTable(kRootPSTextures, pass == 0 ? ps_table : ps_guest_table);
        issue();
      }
      list->RSSetScissorRects(1, &host_scissor);
      list->SetGraphicsRootDescriptorTable(kRootPSTextures, ps_table);
      b.scissor = host_scissor;
      b.tables[0] = ps_table;
    }
    if (occlusion_index != UINT32_MAX)
    {
      list->EndQuery(g.occlusion_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, occlusion_index);
    }
    if (probe_ps && probe_ps->ok() && !probe_ps->pending() && color_rts[0] && !split_draw)
    {
      DrawProbe(key, vs, probe_ps, *color_rts[0], ds, issue);
    }
    if (stamp_begin != UINT32_MAX)
    {
      uint32_t stamp_end = GpuStamp(kGpuDraw);
      if (stamp_end != UINT32_MAX)
      {
        g.draw_timings_slot = g.frame_index;
        g.draw_timings.push_back({g.draws, vs->shader->ucode_data_hash(), ps ? ps->shader->ucode_data_hash() : 0,
                                  stamp_begin, stamp_end,
                                  uint32_t(vp.viewport.Width) * uint32_t(vp.viewport.Height)});
      }
    }
    timer.Mark(9);
    if (g_trace && REXCVAR_GET(ao2_native_gpu_capture_draws) && g.draws % 100 == 0 && color_rts[0])
    {
      char name[64];
      std::snprintf(name, sizeof(name), "%03llu_draw%llu_rt%u_fmt%u", (unsigned long long)g.capture_index++,
                    (unsigned long long)g.draws, color_rts[0]->edram_base, color_rts[0]->edram_format);
      CaptureTexture(color_rts[0]->resource.Get(), color_rts[0]->state, color_rts[0]->format, name);
    }
    if (g_trace && color_rts[0])
    {
      ProbePixel(*color_rts[0], g.draws, vs->shader->ucode_data_hash(), ps ? ps->shader->ucode_data_hash() : 0);
    }
    if (g_trace && CaptureDrawRequested(g.draws, vs->shader->ucode_data_hash()))
    {
      // Targets after this draw (--ao2_native_gpu_capture_after_draws).
      char name[64];
      if (color_rts[0])
      {
        std::snprintf(name, sizeof(name), "%03llu_after%llu_rt%u_fmt%u", (unsigned long long)g.capture_index++,
                      (unsigned long long)g.draws, color_rts[0]->edram_base, color_rts[0]->edram_format);
        CaptureTexture(color_rts[0]->resource.Get(), color_rts[0]->state, color_rts[0]->format, name);
      }
      if (ds && ds->format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
      {
        std::snprintf(name, sizeof(name), "%03llu_after%llu_depth%u", (unsigned long long)g.capture_index++,
                      (unsigned long long)g.draws, ds->edram_base);
        CaptureTexture(ds->resource.Get(), ds->state, DXGI_FORMAT_R32_FLOAT, name, 0);
        std::snprintf(name, sizeof(name), "%03llu_after%llu_stencil%u", (unsigned long long)g.capture_index++,
                      (unsigned long long)g.draws, ds->edram_base);
        CaptureTexture(ds->resource.Get(), ds->state, DXGI_FORMAT_R8_UINT, name, 1);
      }
    }
    ++g.draws;
    MaybeSubmitEarly();
  }

  // ------------------------------------------------------------ Resolve

  void RendererResolve(Pm4Consumer &pm4, const DrawPacket &)
  {
    if (!Ready())
    {
      return;
    }
    FlushPendingDepthClear();
    const RegisterFile &r = pm4.regs();
    auto copy = r.Get<reg::RB_COPY_CONTROL>();
    auto surface = r.Get<reg::RB_SURFACE_INFO>();
    if (!surface.surface_pitch)
    {
      return;
    }
    BeginFrameIfNeeded();

    // Rectangle from the three vertices D3D puts in vertex fetch constant 0
    // (port of the SDK's draw_util::GetResolveInfo): the same coordinates
    // address the EDRAM surface and the destination texture.
    auto fetch = r.Get<xenos::xe_gpu_vertex_fetch_t>(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
    if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 6 ||
        !IsGuestPhysicalReadable(fetch.address << 2, 24))
    {
      return;
    }
    int32_t v[6];
    {
      const uint8_t *p = Mem()->TranslatePhysical(fetch.address << 2);
      float half_pixel = r.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
      for (uint32_t i = 0; i < 6; ++i)
      {
        uint32_t raw;
        std::memcpy(&raw, p + i * 4, 4);
        raw = SwapEndian(raw, fetch.endian);
        float f;
        std::memcpy(&f, &raw, 4);
        // To 16.8 fixed point like the rasterizer.
        v[i] = int32_t(std::clamp((f + half_pixel) * 256.0f, -8388608.0f, 8388607.0f));
      }
    }
    int32_t x0 = (std::min({v[0], v[2], v[4]}) + 127) >> 8;
    int32_t y0 = (std::min({v[1], v[3], v[5]}) + 127) >> 8;
    int32_t x1 = (std::max({v[0], v[2], v[4]}) + 127) >> 8;
    int32_t y1 = (std::max({v[1], v[3], v[5]}) + 127) >> 8;
    auto window_offset = r.Get<reg::PA_SC_WINDOW_OFFSET>();
    if (r.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable)
    {
      x0 += window_offset.window_x_offset;
      x1 += window_offset.window_x_offset;
      y0 += window_offset.window_y_offset;
      y1 += window_offset.window_y_offset;
    }
    {
      auto tl = r.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
      auto br = r.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
      int32_t sx0 = int32_t(tl.tl_x), sy0 = int32_t(tl.tl_y), sx1 = int32_t(br.br_x), sy1 = int32_t(br.br_y);
      if (!tl.window_offset_disable)
      {
        sx0 += window_offset.window_x_offset;
        sx1 += window_offset.window_x_offset;
        sy0 += window_offset.window_y_offset;
        sy1 += window_offset.window_y_offset;
      }
      sx0 = std::max(sx0, 0);
      sy0 = std::max(sy0, 0);
      x0 = std::clamp(x0, sx0, std::max(sx0, sx1));
      x1 = std::clamp(x1, sx0, std::max(sx0, sx1));
      y0 = std::clamp(y0, sy0, std::max(sy0, sy1));
      y1 = std::clamp(y1, sy0, std::max(sy0, sy1));
    }
    x0 &= ~7;
    y0 &= ~7;
    x1 = (x1 + 7) & ~7;
    y1 = (y1 + 7) & ~7;
    int32_t pitch_aligned = int32_t(surface.surface_pitch & ~7u);
    x0 = std::min(x0, pitch_aligned);
    x1 = std::min(x1, pitch_aligned);
    y1 = std::min(y1, y0 + 8192);

    bool is_depth = copy.copy_src_select >= 4;
    auto color_info = r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[copy.copy_src_select & 3]);
    auto depth_info = r.Get<reg::RB_DEPTH_INFO>();
    RenderTarget *src = is_depth ? GetRenderTarget(depth_info.depth_base, uint32_t(depth_info.depth_format), true,
                                                   surface.surface_pitch, surface.msaa_samples)
                                 : GetRenderTarget(color_info.color_base, uint32_t(color_info.color_format), false,
                                                   surface.surface_pitch, surface.msaa_samples);
    if (!src)
    {
      return;
    }
    x1 = std::min(x1, int32_t(src->width));
    y1 = std::min(y1, int32_t(src->height));
    auto dest_info = r.Get<reg::RB_COPY_DEST_INFO>();
    auto dest_pitch = r.Get<reg::RB_COPY_DEST_PITCH>();
    uint32_t dest_base = r.v[XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF;
    if (g_trace)
    {
      Trace("resolve: %s src base %u fmt %u %ux%u x%u rect %d,%d-%d,%d (window offset %d,%d) -> %08X pitch %u "
            "height %u fmt %u num %u exp_bias %d swap %u sample %u cmd %u clear c%u d%u color %08X/%08X depth %08X\n",
            is_depth ? "depth" : "color", is_depth ? depth_info.depth_base : color_info.color_base,
            is_depth ? unsigned(depth_info.depth_format) : unsigned(color_info.color_format), src->width, src->height,
            src->samples, x0, y0, x1, y1, int(window_offset.window_x_offset), int(window_offset.window_y_offset),
            dest_base, dest_pitch.copy_dest_pitch, dest_pitch.copy_dest_height, unsigned(dest_info.copy_dest_format),
            unsigned(dest_info.copy_dest_number), int(dest_info.copy_dest_exp_bias), unsigned(dest_info.copy_dest_swap), unsigned(copy.copy_sample_select), unsigned(copy.copy_command),
            copy.color_clear_enable, copy.depth_clear_enable, r.v[XE_GPU_REG_RB_COLOR_CLEAR],
            r.v[XE_GPU_REG_RB_COLOR_CLEAR_LO], r.v[XE_GPU_REG_RB_DEPTH_CLEAR]);
    }
    if (x1 <= x0 || y1 <= y0)
    {
      return;
    }
    // Who holds the bits before the claim brings them in (EdramSave).
    RenderTarget *bits_origin = is_depth ? nullptr : SoleOwner(*src, D3D12_RECT{x0, y0, x1, y1});
    // A 7e3 float surface resolved as plain 2_10_10_10 bits (the hit effect's
    // save of the scene): the bits are encoded from the float surface straight
    // into the texture, which keeps the EDRAM - no transfer into the
    // 2_10_10_10 surface first (~0.2 ms at 1440p), and the copy back
    // (SkipCopyBack) finds its origin unchanged.
    auto is_format = [](const RenderTarget *rt, xenos::ColorRenderTargetFormat a, xenos::ColorRenderTargetFormat b) {
      return rt->edram_format == uint32_t(a) || rt->edram_format == uint32_t(b);
    };
    bool pack_7e3 = false;
    if (bits_origin && bits_origin != src && src->samples == 1 && bits_origin->samples == 1 &&
        bits_origin->format == DXGI_FORMAT_R16G16B16A16_FLOAT && !copy.color_clear_enable &&
        (copy.copy_command == xenos::CopyCommand::kRaw || copy.copy_command == xenos::CopyCommand::kConvert) &&
        is_format(src, xenos::ColorRenderTargetFormat::k_2_10_10_10,
                  xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10) &&
        is_format(bits_origin, xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT,
                  xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16) &&
        x1 <= int32_t(bits_origin->width) && y1 <= int32_t(bits_origin->height) &&
        REXCVAR_GET(ao2_native_gpu_resolve_pack) && !CurrentPostSettings().Any())
    {
      auto info = r.Get<reg::RB_COPY_DEST_INFO>();
      pack_7e3 = !info.copy_dest_exp_bias && !info.copy_dest_swap &&
                 GetResolveFormat(uint32_t(info.copy_dest_format), false).rtv == DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    if (!pack_7e3)
    {
      ClaimEdram(*src, D3D12_RECT{x0, y0, x1, y1});
    }

    if (copy.copy_command == xenos::CopyCommand::kRaw || copy.copy_command == xenos::CopyCommand::kConvert)
    {
      ResolveFormat rf = GetResolveFormat(uint32_t(dest_info.copy_dest_format), is_depth);
      // Predicated tiling (the game's own MSAA): each tile is drawn with the
      // window offset moved up by the tile's first row and resolved with the
      // destination base moved down to that row (tiled textures are stored in
      // 32-row stripes: row 512 starts 512 rows in). Those rows belong to the
      // texture the first tile was resolved to in this frame - the one the
      // game samples - not to a texture of their own.
      uint32_t dest_row = 0;
      {
        uint32_t row_bytes =
            dest_pitch.copy_dest_pitch * (FormatInfo::Get(uint32_t(dest_info.copy_dest_format))->bits_per_pixel / 8);
        for (const auto &[address, t] : g.resolved)
        {
          if (row_bytes && address < dest_base && t && t->resource && t->last_frame == g.frame_number &&
              t->format == rf.unorm && t->scale4 == src->scale4 && t->width == dest_pitch.copy_dest_pitch &&
              t->height == dest_pitch.copy_dest_height && (dest_base - address) % (row_bytes * 32) == 0 &&
              (dest_base - address) / row_bytes + uint32_t(y1) <= t->height)
          {
            dest_row = (dest_base - address) / row_bytes;
            Trace("    predicated tile: rows from %u of %08X\n", dest_row, address);
            dest_base = address;
            break;
          }
        }
      }
      if (!g.lent.empty())
      {
        ReturnLent(pack_7e3 ? bits_origin : nullptr, dest_base);
      }
      if (pack_7e3 && dest_row)
      {
        ClaimEdram(*src, D3D12_RECT{x0, y0, x1, y1});
        pack_7e3 = false;
      }
      uint32_t w = std::max<uint32_t>(dest_pitch.copy_dest_pitch, uint32_t(x1));
      uint32_t h = std::max<uint32_t>(dest_pitch.copy_dest_height, uint32_t(y1));
      w = std::min(w, 8192u);
      h = std::min(h, 8192u);
      auto &tex = g.resolved[dest_base];
      auto fits = [&](const std::unique_ptr<HostTexture> &t) {
        return t && t->resource && t->format == rf.unorm && t->width >= w && t->height >= h &&
               t->scale4 == src->scale4;
      };
      if (!fits(tex))
      {
        std::unique_ptr<HostTexture> found;
        auto range = g.resolved_spare.equal_range(dest_base);
        size_t spares = 0;
        for (auto it = range.first; it != range.second; ++it, ++spares)
        {
          if (!found && fits(it->second))
          {
            found = std::move(it->second);
          }
        }
        std::erase_if(g.resolved_spare, [&](const auto &e) { return e.first == dest_base && !e.second; });
        if (tex && tex->resource)
        {
          if (spares >= 4)
          {
            DeferRelease(std::move(tex->resource));
            DeferRelease(std::move(tex->lowres));
          }
          else
          {
            g.resolved_spare.emplace(dest_base, std::move(tex));
          }
        }
        tex = std::move(found);
      }
      if (!tex)
      {
        tex = std::make_unique<HostTexture>();
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = ScaleSize(w, src->scale4);
        td.Height = ScaleSize(h, src->scale4);
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Format = rf.resource;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
        tex->state = D3D12_RESOURCE_STATE_COPY_DEST;
        ++g.resources_created;
        if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, tex->state, nullptr,
                                                     IID_PPV_ARGS(&tex->resource))))
        {
          NativeLog("resolve destination creation failed (%ux%u fmt %u)\n", w, h, unsigned(rf.resource));
          g.resolved.erase(dest_base);
          return;
        }
        tex->format = rf.unorm;
        tex->snorm = rf.snorm;
        tex->swizzle = rf.swizzle;
        tex->width = w;
        tex->height = h;
        tex->scale4 = src->scale4;
      }
      tex->last_frame = g.frame_number;
      ++tex->generation;
      tex->from_depth = is_depth;
      tex->small_source = SmallTarget(src->edram_pitch, src->samples);
      tex->epoch = g.write_epoch.load(std::memory_order_relaxed);
      tex->checked_epoch = 0;
      tex->guest_size = std::min(w * h * BytesPerPixel(rf.unorm), 64u << 20);
      MarkResolvePages(dest_base, tex->guest_size);
      WatchGuestRange(dest_base, tex->guest_size);

      float scale = 1.0f;
      bool src_fixed16 = !is_depth && src->fixed16;
      if (!is_depth)
      {
        scale = std::ldexp(1.0f, int(dest_info.copy_dest_exp_bias));
        // 16-bit fixed EDRAM values are stored /32 on the host (see FillSystemConstants).
        scale *= src_fixed16 ? 32.0f : 1.0f;
        if (rf.fixed16)
        {
          // 16-bit normalized destinations get the value converted to the
          // destination number format, not the -32...32 EDRAM bits: the game
          // writes HDR/8 into k_16_16_16_16 targets and its bright pass
          // multiplies the unsigned texture by 8 (and the luminance it reads
          // back drives the exposure).
          rf.rtv = dest_info.copy_dest_number == xenos::SurfaceNumberFormat::kSignedRepeatingFraction ? rf.snorm
                                                                                                       : rf.unorm;
        }
      }
      D3D12_RECT rect = {x0, y0, std::min(x1, int32_t(w)), std::min(y1, int32_t(h))};
      bool same_family = !is_depth && TypelessFormat(src->format) == TypelessFormat(rf.rtv);
      // Red and blue swapped on the way (e.g. ABGR render target -> ARGB texture).
      bool swap_red_blue = !is_depth && dest_info.copy_dest_swap;
      tex->red_blue_swapped = swap_red_blue;
      // Traced frame: the resolve's GPU time goes to gpu_draws.txt like a draw's.
      uint32_t stamp_begin = g_trace ? GpuStamp(kGpuResolve) : UINT32_MAX;
      // Only whole-screen surfaces: small ones (the luminance reduction) are
      // drawn again right after their resolve, and a copy back costs as much.
      bool lend = !copy.color_clear_enable && REXCVAR_GET(ao2_native_gpu_resolve_lend) && rect.right >= 640 &&
                  rect.bottom >= 360;
      bool swap = REXCVAR_GET(ao2_native_gpu_resolve_swap) && (copy.color_clear_enable || lend) && same_family &&
                  !src_fixed16 && !rf.fixed16 && !swap_red_blue && !dest_row && !CurrentPostSettings().Any() &&
                  rect.right <= LONG(src->width) && rect.bottom <= LONG(src->height);
      tex->exp_scale = 1.0f;
      if (pack_7e3 && ResolveWithShader(*bits_origin, *tex, rf, rect, 1.0f, copy.copy_sample_select, false, 0, true))
      {
        Trace("    encoded from the 7e3 surface, which keeps the EDRAM\n");
        ++g.resolves_drawn;
        ++g.resolves_packed;
        g.edram_save = {bits_origin, bits_origin->writes, src, rect, dest_base, tex->generation};
        ++g.copy_back_saves;
      }
      else if (pack_7e3)
      {
        // No pipeline or views left: the usual way.
        ClaimEdram(*src, D3D12_RECT{x0, y0, x1, y1});
        if (ResolveWithShader(*src, *tex, rf, rect, scale, copy.copy_sample_select, swap_red_blue, dest_row))
        {
          ++g.resolves_drawn;
        }
      }
      else if (swap && SwapResolve(*src, *tex, rect, scale, lend))
      {
        if (lend)
        {
          g.lent.push_back({src, dest_base, tex->resource.Get()});
          ++g.resolves_lent;
        }
        Trace("    %s the render target (exp scale %g)\n", lend ? "lent the resource of" : "swapped with", scale);
        ++g.resolves_swapped;
      }
      else if (same_family && src->samples == 1 && scale == 1.0f && !swap_red_blue)
      {
        // Plain copy of the bits.
        Transition(src->resource.Get(), src->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(tex->resource.Get(), tex->state, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION s = {src->resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION t = {tex->resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_RECT hrect = ScaleRect(rect, src->scale4);
        D3D12_BOX box = {UINT(hrect.left), UINT(hrect.top), 0, UINT(hrect.right), UINT(hrect.bottom), 1};
        LONG offset_y = ScaleEdge(LONG(dest_row), src->scale4);
        D3D12_RECT drect = {hrect.left, hrect.top + offset_y, hrect.right, hrect.bottom + offset_y};
        GpuMark(kGpuResolve);
        Crumb("resolve copy", uint64_t(src->resource.Get()), uint64_t(tex->resource.Get()),
              (uint64_t(rect.right) << 32) | uint64_t(rect.left), (uint64_t(rect.bottom) << 32) | uint64_t(rect.top));
        if (RectFits(src->resource.Get(), hrect, "resolve copy source") &&
            RectFits(tex->resource.Get(), drect, "resolve copy destination"))
        {
          g.list->CopyTextureRegion(&t, UINT(drect.left), UINT(drect.top), 0, &s, &box);
        }
        ++g.resolves_copied;
        // The bits went over unchanged: a draw copying them back may be skipped (SkipCopyBack).
        if (bits_origin && !dest_row && !CurrentPostSettings().Any())
        {
          g.edram_save = {bits_origin, bits_origin->writes, src, rect, dest_base, tex->generation};
          ++g.copy_back_saves;
        }
      }
      else if (ResolveWithShader(*src, *tex, rf, rect, scale, copy.copy_sample_select, swap_red_blue, dest_row))
      {
        ++g.resolves_drawn;
      }
      if (stamp_begin != UINT32_MAX)
      {
        uint32_t stamp_end = GpuStamp(kGpuResolve);
        if (stamp_end != UINT32_MAX)
        {
          // Listed with the draw it follows: "vs" 0xC (color) / 0xD (depth), "ps" the destination.
          g.draw_timings_slot = g.frame_index;
          g.draw_timings.push_back({g.draws, is_depth ? 0xDull : 0xCull, dest_base, stamp_begin, stamp_end,
                                    uint32_t(rect.right - rect.left) * uint32_t(rect.bottom - rect.top)});
        }
      }
      if (!is_depth && w >= 640 && h >= 360)
      {
        g.present_address = dest_base;
      }
      if (is_depth)
      {
        NoteShadowResolve(*src, rect, dest_pitch.copy_dest_pitch, dest_pitch.copy_dest_height);
        PostDepthResolved(*tex, rect, dest_row);
      }
      else
      {
        PostSceneResolved(*src, *tex, rect, dest_row, dest_base);
      }
      // Small results (luminance reductions...) may be read by the CPU.
      uint32_t guest_bpp = FormatInfo::Get(uint32_t(dest_info.copy_dest_format))->bits_per_pixel / 8;
      uint32_t area = uint32_t(rect.right - rect.left) * uint32_t(rect.bottom - rect.top);
      if (!is_depth && area <= uint32_t(REXCVAR_GET(ao2_native_gpu_writeback_texels)) &&
          guest_bpp == BytesPerTexel(rf.unorm))
      {
        // Of a scaled destination the guest gets the guest-sized copy: each
        // texel the average of its host texels, what the draws of small
        // render targets sample too. One host texel of each is the picture
        // sampled off the texel centres - the 1x1 average luminance weighed
        // towards a corner of the screen (at 2x the top left texel left out
        // the right and the bottom quarter of the screen and took the left
        // and the top one twice: the exposure followed the sky).
        bool reduced = tex->scale4 != 4 && rf.rtv == rf.unorm && tex->width * tex->height <= kWritebackReducedTexels &&
                       EnsureLowres(*tex);
        QueueWriteback(reduced ? tex->lowres.Get() : tex->resource.Get(), reduced ? tex->lowres_state : tex->state,
                       rf.unorm, reduced ? 4u : tex->scale4, uint32_t(rect.left), uint32_t(rect.top),
                       uint32_t(rect.right), uint32_t(rect.bottom), dest_base, dest_pitch.copy_dest_pitch, guest_bpp,
                       dest_info.copy_dest_endian);
      }
      if (g_trace)
      {
        char name[64];
        std::snprintf(name, sizeof(name), "%03llu_%08X_%s", (unsigned long long)g.capture_index++, dest_base,
                      is_depth ? "depth" : "color");
        CaptureTexture(tex->resource.Get(), tex->state, rf.unorm, name);
        if (!g.captures.empty())
        {
          g.captures.back().red_blue_swapped = tex->red_blue_swapped;
        }
      }
    }

    // Clears performed together with the resolve.
    D3D12_RECT rect = {x0, y0, x1, y1};
    if (copy.color_clear_enable && !is_depth)
    {
      uint32_t cv = r.v[XE_GPU_REG_RB_COLOR_CLEAR];
      float c[4] = {};
      for (uint32_t j = 0; j < 4; ++j)
      {
        c[j] = float((cv >> (j * 8)) & 0xFF) / 255.0f;
      }
      switch (color_info.color_format)
      {
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
        for (uint32_t j = 0; j < 3; ++j)
        {
          c[j] = float((cv >> (j * 10)) & 0x3FF) / 1023.0f;
        }
        c[3] = float(cv >> 30) / 3.0f;
        break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
        for (uint32_t j = 0; j < 3; ++j)
        {
          c[j] = xenos::Float7e3To32((cv >> (j * 10)) & 0x3FF);
        }
        c[3] = float(cv >> 30) / 3.0f;
        break;
      case xenos::ColorRenderTargetFormat::k_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
        // Wide clear values span RB_COLOR_CLEAR_LO; zero is by far the most common.
        c[0] = c[1] = c[2] = c[3] = 0.0f;
        break;
      default:
        break;
      }
      Transition(src->resource.Get(), src->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      GpuMark(kGpuClear);
      Crumb("resolve color clear", uint64_t(src->resource.Get()), (uint64_t(rect.right) << 32) | uint64_t(rect.left),
            (uint64_t(rect.bottom) << 32) | uint64_t(rect.top), src->width | (uint64_t(src->height) << 32));
      D3D12_RECT host_rect = ScaleRect(rect, src->scale4);
      if (RectFits(src->resource.Get(), host_rect, "resolve color clear"))
      {
        g.list->ClearRenderTargetView(src->view, c, 1, &host_rect);
      }
      ++src->writes;
      ForgetUniform(*src, 0, rect);
      uint32_t clear_format = uint32_t(KeyColorFormat(color_info.color_format));
      if (clear_format == 0 || clear_format == 2 || clear_format == 3)
      {
        static std::vector<uint32_t> inside;
        TilesInside(*src, rect, inside);
        SetUniform(*src, 0, inside, cv);
      }
    }
    if (copy.depth_clear_enable)
    {
      RenderTarget *depth = GetRenderTarget(depth_info.depth_base, uint32_t(depth_info.depth_format), true,
                                            surface.surface_pitch, surface.msaa_samples);
      if (depth)
      {
        uint32_t dv = r.v[XE_GPU_REG_RB_DEPTH_CLEAR];
        uint32_t d24 = (dv >> 8) & 0xFFFFFF;
        float z = depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8 ? xenos::Float20e4To32(d24)
                                                                                      : xenos::UNorm24To32(d24);
        D3D12_RECT drect = {x0, y0, std::min(x1, int32_t(depth->width)), std::min(y1, int32_t(depth->height))};
        ClaimEdram(*depth, drect);
        Transition(depth->resource.Get(), depth->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        GpuMark(kGpuClear);
        Crumb("resolve depth clear", uint64_t(depth->resource.Get()),
              (uint64_t(drect.right) << 32) | uint64_t(drect.left), (uint64_t(drect.bottom) << 32) | uint64_t(drect.top),
              depth->width | (uint64_t(depth->height) << 32));
        // The depth surface may be smaller than the color one (its EDRAM base
        // is further), leaving nothing - or an inverted rectangle - to clear.
        D3D12_RECT host_drect = ScaleRect(drect, depth->scale4);
        if (RectFits(depth->resource.Get(), host_drect, "resolve depth clear") &&
            REXCVAR_GET(ao2_native_gpu_skip_depth_restore) && is_depth && !g.occlusion_active)
        {
          g.list->ClearDepthStencilView(depth->view, D3D12_CLEAR_FLAG_STENCIL, 0.0f, uint8_t(dv & 0xFF), 1,
                                        &host_drect);
          ForgetUniform(*depth, 1, drect);
          static std::vector<uint32_t> inside;
          TilesInside(*depth, drect, inside);
          SetUniform(*depth, 1, inside, dv & 0xFFu);
          g.pending_depth = {depth, drect, dest_base, z, dv};
        }
        else if (RectFits(depth->resource.Get(), host_drect, "resolve depth clear"))
        {
          g.list->ClearDepthStencilView(depth->view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                        std::min(z, 1.0f), uint8_t(dv & 0xFF), 1, &host_drect);
          ForgetUniform(*depth, 0, drect);
          ForgetUniform(*depth, 1, drect);
          static std::vector<uint32_t> inside;
          TilesInside(*depth, drect, inside);
          SetUniform(*depth, 0, inside, dv & ~0xFFu);
          SetUniform(*depth, 1, inside, dv & 0xFFu);
        }
      }
    }
  }

  // ------------------------------------------------------------ Present

  void RendererSetGammaRamp(const uint32_t table[256], const uint32_t pwl[128][3], uint32_t luta_control)
  {
    if (!std::memcmp(g.gamma_ramp, table, sizeof(g.gamma_ramp)))
    {
      return;
    }
    std::memcpy(g.gamma_ramp, table, sizeof(g.gamma_ramp));
    // Log a summary: the ramp the game chose (brightness setting / display gamma).
    char line[512];
    int n = std::snprintf(line, sizeof(line), "gamma ramp (DC_LUTA_CONTROL %08X) r/g/b at", luta_control);
    for (uint32_t i : {0u, 16u, 32u, 64u, 96u, 128u, 160u, 192u, 224u, 255u})
    {
      n += std::snprintf(line + n, sizeof(line) - n, " %u:%u/%u/%u", i, (table[i] >> 20) & 0x3FF,
                         (table[i] >> 10) & 0x3FF, table[i] & 0x3FF);
    }
    NativeLog("%s | pwl[0] %08X pwl[64] %08X pwl[127] %08X\n", line, pwl[0][0], pwl[64][0], pwl[127][0]);
  }

  bool RendererOcclusionEvent(uint32_t address, bool end, uint32_t fallback_samples)
  {
    if (!g.initialized || g.failed || !g.occlusion_heap)
    {
      return false;
    }
    if (!end)
    {
      g.occlusion = {};
      g.occlusion_active = true;
      Trace("occlusion query begin\n");
      return true;
    }
    if (!g.occlusion_active)
    {
      // Begun before the renderer was ready: its draws were not counted.
      g.occlusion = {};
      g.occlusion.unknown = true;
    }
    g.occlusion_active = false;
    OcclusionQuery q = g.occlusion;
    q.address = address;
    q.fallback = fallback_samples;
    std::memcpy(q.expect, Mem()->TranslatePhysical(address), sizeof(q.expect));
    q.unknown |= q.draws != q.recorded;
    Trace("occlusion query end %08X: %u draws, %u counted%s\n", address, q.draws, q.recorded,
          q.unknown ? " (unknown)" : "");
    if (!q.count)
    {
      // Nothing for the GPU to count.
      if (q.unknown)
      {
        ++g.occlusion_unknown;
      }
      WriteOcclusionResult(address, q.unknown ? fallback_samples : 0, q.expect);
      NotifyOcclusionResults();
      return true;
    }
    g.occlusion_ended.push_back(q);
    return true;
  }

  volatile uint32_t &OcclusionResultGeneration()
  {
    return g_occlusion_generation;
  }

  void RendererNotifyOcclusion()
  {
    NotifyOcclusionResults();
  }

  bool RendererIdle()
  {
    if (!g.initialized || g.failed)
    {
      return false;
    }
    if (!g.occlusion_ended.empty())
    {
      SubmitOpenList();
    }
    PublishOcclusion();
    std::lock_guard lock(g.occlusion_mutex);
    return !g.occlusion_pending.empty();
  }

  namespace
  {
    bool CreateOverlayPipeline()
    {
      D3D12_DESCRIPTOR_RANGE range = {};
      range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      range.NumDescriptors = 1;
      D3D12_ROOT_PARAMETER rp[2] = {};
      // Position scale and offset to clip space.
      rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      rp[0].Constants.Num32BitValues = 4;
      rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
      rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      rp[1].DescriptorTable.NumDescriptorRanges = 1;
      rp[1].DescriptorTable.pDescriptorRanges = &range;
      rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      D3D12_STATIC_SAMPLER_DESC ss = {};
      ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
      ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
      ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      ss.MaxLOD = D3D12_FLOAT32_MAX;
      D3D12_ROOT_SIGNATURE_DESC rd = {};
      rd.NumParameters = 2;
      rd.pParameters = rp;
      rd.NumStaticSamplers = 1;
      rd.pStaticSamplers = &ss;
      rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
      ComPtr<ID3DBlob> blob, error;
      if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
          FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&g.overlay_root))))
      {
        NativeLog("overlay root signature failed\n");
        return false;
      }
      auto vs = Compile(kOverlayShader, "vs", "vs_5_1");
      auto ps = Compile(kOverlayShader, "ps", "ps_5_1");
      if (vs.empty() || ps.empty())
      {
        return false;
      }
      const D3D12_INPUT_ELEMENT_DESC layout[] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      };
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
      pd.pRootSignature = g.overlay_root.Get();
      pd.VS = {vs.data(), vs.size()};
      pd.PS = {ps.data(), ps.size()};
      auto &blend = pd.BlendState.RenderTarget[0];
      blend.BlendEnable = TRUE;
      blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
      blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
      blend.BlendOp = D3D12_BLEND_OP_ADD;
      blend.SrcBlendAlpha = D3D12_BLEND_ONE;
      blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
      blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pd.SampleMask = UINT_MAX;
      pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      pd.RasterizerState.DepthClipEnable = TRUE;
      pd.InputLayout = {layout, UINT(std::size(layout))};
      pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pd.NumRenderTargets = 1;
      pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      pd.SampleDesc.Count = 1;
      if (FAILED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g.overlay_pso))))
      {
        NativeLog("overlay pipeline failed\n");
        return false;
      }
      return true;
    }

    // The GPU copy of overlay texture `id`, uploaded in the open command list
    // on first use.
    ID3D12Resource *OverlayTexture(uint64_t id, const overlay::Texture &texture)
    {
      auto it = g.overlay_textures.find(id);
      if (it != g.overlay_textures.end())
      {
        return it->second.Get();
      }
      D3D12_HEAP_PROPERTIES hp = {};
      hp.Type = D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC rd = {};
      rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      rd.Width = texture.width;
      rd.Height = texture.height;
      rd.DepthOrArraySize = 1;
      rd.MipLevels = 1;
      rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      rd.SampleDesc.Count = 1;
      ComPtr<ID3D12Resource> resource;
      uint32_t pitch = (texture.width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                       ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
      void *mapped = nullptr;
      D3D12_GPU_VIRTUAL_ADDRESS source = 0;
      if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&resource))) ||
          !(source = Upload(nullptr, uint64_t(pitch) * texture.height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT,
                            &mapped)))
      {
        return nullptr;
      }
      for (uint32_t y = 0; y < texture.height; ++y)
      {
        std::memcpy(static_cast<uint8_t *>(mapped) + size_t(y) * pitch,
                    texture.rgba.data() + size_t(y) * texture.width * 4, size_t(texture.width) * 4);
      }
      Frame &f = CurrentFrame();
      D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
      dst.pResource = resource.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.pResource = f.upload.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Offset = source - f.upload->GetGPUVirtualAddress();
      src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, texture.width, texture.height, 1, pitch};
      g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
      Transition(resource.Get(), state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      return g.overlay_textures.emplace(id, std::move(resource)).first->second.Get();
    }

    // Draws the latest ImGui frame (overlay.h) over back buffer `bb_index`
    // (in the render target state).
    void DrawOverlay(uint32_t bb_index)
    {
      overlay::RequestFrame();
      overlay::Visit([bb_index](const overlay::Frame &frame, const overlay::TextureMap &textures) {
        // Textures the UI destroyed.
        for (auto it = g.overlay_textures.begin(); it != g.overlay_textures.end();)
        {
          if (!textures.count(it->first))
          {
            DeferRelease(std::move(it->second));
            it = g.overlay_textures.erase(it);
          }
          else
          {
            ++it;
          }
        }
        if (frame.draws.empty() || frame.vertices.empty() || frame.indices.empty() || !(frame.width > 0.0f) ||
            !(frame.height > 0.0f) || g.overlay_failed)
        {
          return;
        }
        if (!g.overlay_pso && !CreateOverlayPipeline())
        {
          g.overlay_failed = true;
          return;
        }
        uint64_t vertex_bytes = frame.vertices.size() * sizeof(frame.vertices[0]);
        uint64_t index_bytes = frame.indices.size() * sizeof(frame.indices[0]);
        D3D12_GPU_VIRTUAL_ADDRESS vertices = Upload(frame.vertices.data(), vertex_bytes, 16);
        D3D12_GPU_VIRTUAL_ADDRESS indices = Upload(frame.indices.data(), index_bytes, 16);
        if (!vertices || !indices)
        {
          return;
        }
        float sw = float(g.swapchain_width), sh = float(g.swapchain_height);
        D3D12_VIEWPORT vp = {0.0f, 0.0f, sw, sh, 0.0f, 1.0f};
        D3D12_VERTEX_BUFFER_VIEW vbv = {vertices, UINT(vertex_bytes), UINT(sizeof(frame.vertices[0]))};
        D3D12_INDEX_BUFFER_VIEW ibv = {indices, UINT(index_bytes), DXGI_FORMAT_R16_UINT};
        float constants[4] = {2.0f / frame.width, -2.0f / frame.height, -1.0f, 1.0f};
        g.bound.valid = false;
        g.list->OMSetRenderTargets(1, &g.backbuffer_rtv[bb_index], FALSE, nullptr);
        g.list->RSSetViewports(1, &vp);
        g.list->SetGraphicsRootSignature(g.overlay_root.Get());
        g.list->SetPipelineState(g.overlay_pso.Get());
        g.list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.list->IASetVertexBuffers(0, 1, &vbv);
        g.list->IASetIndexBuffer(&ibv);
        float sx = sw / frame.width, sy = sh / frame.height;
        std::unordered_map<uint64_t, D3D12_GPU_DESCRIPTOR_HANDLE> views;
        for (const overlay::Draw &d : frame.draws)
        {
          if (uint64_t(d.index_offset) + d.index_count > frame.indices.size())
          {
            continue;
          }
          auto view = views.find(d.texture);
          if (view == views.end())
          {
            auto texture = textures.find(d.texture);
            ID3D12Resource *resource =
                texture != textures.end() ? OverlayTexture(d.texture, *texture->second) : nullptr;
            Frame &f = CurrentFrame();
            if (!resource || f.views_used >= kViewsPerFrame)
            {
              continue;
            }
            uint32_t slot = g.frame_index * kViewsPerFrame + f.views_used++;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.view_heap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += uint64_t(slot) * g.view_size;
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.view_heap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += uint64_t(slot) * g.view_size;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            g.device->CreateShaderResourceView(resource, &sd, cpu);
            view = views.emplace(d.texture, gpu).first;
          }
          LONG left = LONG(std::clamp(std::floor(d.clip[0] * sx), 0.0f, sw));
          LONG top = LONG(std::clamp(std::floor(d.clip[1] * sy), 0.0f, sh));
          LONG right = LONG(std::clamp(std::ceil(d.clip[2] * sx), 0.0f, sw));
          LONG bottom = LONG(std::clamp(std::ceil(d.clip[3] * sy), 0.0f, sh));
          if (right <= left || bottom <= top)
          {
            continue;
          }
          D3D12_RECT sc = {left, top, right, bottom};
          g.list->RSSetScissorRects(1, &sc);
          g.list->SetGraphicsRootDescriptorTable(1, view->second);
          g.list->DrawIndexedInstanced(d.index_count, 1, d.index_offset, d.base_vertex, 0);
        }
        g.bound.valid = false;
      });
    }
  } // namespace

  void RendererSetMenuProbe(uint32_t width, uint32_t height, uint32_t x, uint32_t y)
  {
    g_probe_size.store(width && height && width < 4096 && height < 4096 ? width | (height << 16) : 0,
                       std::memory_order_relaxed);
    g_probe_cursor.store(std::min(x, 0xFFFFu) | (std::min(y, 0xFFFFu) << 16), std::memory_order_relaxed);
  }

  bool RendererMenuProbe(uint32_t width, uint32_t height, float &u, float &v)
  {
    uint64_t result = g_probe_result.load(std::memory_order_relaxed);
    if (!(result & (1ull << 32)) || (result >> 33) != (width | (height << 16)))
    {
      return false;
    }
    u = float(result & 0xFFFF) / 65535.0f;
    v = float((result >> 16) & 0xFFFF) / 65535.0f;
    return true;
  }

  bool RendererSetResolutionScale(double scale)
  {
    if (!g_running.load(std::memory_order_relaxed))
    {
      return false;
    }
    g_requested_scale4.store(uint32_t(std::lround(std::clamp(scale, 1.0, 4.0) * 4.0)), std::memory_order_release);
    return true;
  }

  void RendererSwap(uint32_t frontbuffer, uint32_t width, uint32_t height)
  {
    if (!Ready())
    {
      return;
    }
    if (ao2::spikes::Enabled())
    {
      ao2::spikes::Mark(ao2::spikes::kGpuSwap);
    }
    BeginFrameIfNeeded();

    // The frontbuffer is the destination of the frame's last resolve.
    HostTexture *source = nullptr;
    auto it = g.resolved.find(frontbuffer & 0x1FFFFFFF);
    if (it == g.resolved.end() || !it->second)
    {
      it = g.resolved.find(g.present_address);
      if (!g.frontbuffer_miss_logged)
      {
        g.frontbuffer_miss_logged = true;
        NativeLog("frontbuffer %08X (%ux%u) not resolved, presenting %08X\n", frontbuffer, width, height,
                  g.present_address);
      }
    }
    if (it != g.resolved.end() && it->second)
    {
      source = it->second.get();
    }

    // The previous frame's present (present thread) must be issued before the
    // back buffer index is read or the swap chain resized.
    auto present_wait_start = std::chrono::steady_clock::now();
    HRESULT present_hr = g_presenter.Wait();
    double present_wait_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - present_wait_start).count();
    if (FAILED(present_hr) && present_hr != DXGI_ERROR_WAS_STILL_DRAWING)
    {
      NativeLog("Present failed (0x%08X)\n", unsigned(present_hr));
    }
    bool present = EnsureSwapChain();
    if (present)
    {
      uint32_t bb_index = g.swapchain->GetCurrentBackBufferIndex();
      ID3D12Resource *bb = g.backbuffers[bb_index].Get();
      D3D12_RESOURCE_STATES bb_state = D3D12_RESOURCE_STATE_PRESENT;
      Transition(bb, bb_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      GpuMark(kGpuPresent);
      // Letterbox the guest frame into the window keeping its aspect ratio;
      // the bars are cleared (no clear when the frame covers the window).
      float sw = float(g.swapchain_width), sh = float(g.swapchain_height);
      float fit = width && height ? std::min(sw / float(width), sh / float(height)) : 0.0f;
      float vw = std::floor(float(width) * fit), vh = std::floor(float(height) * fit);
      if (!source || vw < sw || vh < sh)
      {
        g.list->ClearRenderTargetView(g.backbuffer_rtv[bb_index], black, 0, nullptr);
      }
      if (source && width && height)
      {
        Transition(source->resource.Get(), source->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Frame &f = CurrentFrame();
        uint32_t slot = g.frame_index * kViewsPerFrame + std::min(f.views_used++, kViewsPerFrame - 1);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.view_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += uint64_t(slot) * g.view_size;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.view_heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += uint64_t(slot) * g.view_size;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = source->format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g.device->CreateShaderResourceView(source->resource.Get(), &sd, cpu);

        D3D12_VIEWPORT vp = {std::floor((sw - vw) * 0.5f), std::floor((sh - vh) * 0.5f), vw, vh, 0, 1};
        D3D12_RECT sc = {0, 0, LONG(g.swapchain_width), LONG(g.swapchain_height)};
        // uv scale (the part of the host texture the frame takes), red/blue swap.
        float host_scale = float(source->scale4) * 0.25f;
        float blit_constants[3] = {
            std::min(float(width) * host_scale / float(ScaleSize(source->width, source->scale4)), 1.0f),
            std::min(float(height) * host_scale / float(ScaleSize(source->height, source->scale4)), 1.0f), 0.0f};
        uint32_t swap_flag = source->red_blue_swapped ? 1u : 0u;
        std::memcpy(&blit_constants[2], &swap_flag, 4);
        g.bound.valid = false;
        g.list->OMSetRenderTargets(1, &g.backbuffer_rtv[bb_index], FALSE, nullptr);
        g.list->RSSetViewports(1, &vp);
        g.list->RSSetScissorRects(1, &sc);
        g.list->SetGraphicsRootSignature(g.blit_root.Get());
        g.list->SetPipelineState(g.blit_pso.Get());
        g.list->SetGraphicsRootDescriptorTable(0, gpu);
        g.list->SetGraphicsRoot32BitConstants(1, 3, blit_constants, 0);
        std::memcpy(f.upload_mapped, g.gamma_ramp, sizeof(g.gamma_ramp));
        g.list->SetGraphicsRootShaderResourceView(2, f.upload->GetGPUVirtualAddress());
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.list->DrawInstanced(3, 1, 0, 0);
      }
      if (g_trace)
      {
        // What the window shows (letterboxed, display gamma ramp applied).
        char name[64];
        std::snprintf(name, sizeof(name), "%03llu_presented", (unsigned long long)g.capture_index++);
        CaptureTexture(bb, bb_state, DXGI_FORMAT_R8G8B8A8_UNORM, name);
      }
      // The ImGui overlay is not in the traced frame.
      Transition(bb, bb_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      DrawOverlay(bb_index);
      Transition(bb, bb_state, D3D12_RESOURCE_STATE_PRESENT);
    }
    ReadProbe();
    if (CloseList())
    {
      ID3D12CommandList *lists[] = {g.list.Get()};
      g.queue->ExecuteCommandLists(1, lists);
    }
    g.list_open = false;
    g.draws_since_submit = 0;
    FlushDebugMessages();
    if (present)
    {
      auto present_start = std::chrono::steady_clock::now();
      g_presenter.Submit(g.swapchain.Get(), g.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
      if (ao2::spikes::Enabled())
      {
        ao2::spikes::Mark(ao2::spikes::kGpuSubmitted, present_wait_ms);
      }
      // Frames the GPU thread spent long on shaders, pipelines or uploads.
      uint64_t stall = g.frame_pso_ns + g.frame_shader_ns + g.frame_upload_ns;
      double interval = g.last_present.time_since_epoch().count()
                            ? std::chrono::duration<double, std::milli>(present_start - g.last_present).count()
                            : 0.0;
      if (stall > 8000000 || interval > 50.0)
      {
        NativeLog("hitch: frame %llu interval %.1f ms | shaders %.1f ms (%u slow) pso %.1f ms (%u new, %u queued) uploads %.1f ms\n",
                  (unsigned long long)g.frame_number, interval, g.frame_shader_ns / 1e6, g.frame_shader_slow,
                  g.frame_pso_ns / 1e6, g.frame_pso_new, g.frame_pso_queued, g.frame_upload_ns / 1e6);
      }
      g.last_present = present_start;
      g.frame_pso_ns = g.frame_shader_ns = g.frame_upload_ns = 0;
      g.frame_pso_new = g.frame_shader_slow = g.frame_pso_queued = 0;
    }
    HRESULT removed = g.device->GetDeviceRemovedReason();
    if (FAILED(removed))
    {
      NativeLog("DEVICE REMOVED (0x%08X) after native frame %llu\n", unsigned(removed),
                (unsigned long long)g.frame_number);
      g.failed = true;
      return;
    }
    CurrentFrame().fence_value = ++g.fence_value;
    g.queue->Signal(g.fence.Get(), g.fence_value);
    g.frame_index = (g.frame_index + 1) % kFrameCount;
    ++g.frame_number;
    g_running.store(true, std::memory_order_relaxed);
    g_post.pending = false;
    g_post.waiting_color = nullptr;
    ApplyRequestedScale();
    ApplyShadowResolution();
    if (CurrentPostSettings().Any())
    {
      InitPost(); // the post effects' pipelines are made before the scene needs them
    }
    WriteCaptures();
    UpdateTraceFrame();
    auto now = std::chrono::steady_clock::now();
    // Once a second, the GPU line with the local time of decouple.log's lines,
    // so a slow second there can be matched with its GPU time.
    if (now - g.last_stats >= std::chrono::seconds(1))
    {
      double seconds = std::chrono::duration<double>(now - g.last_stats).count();
      g.last_stats = now;
      NativeLog("  %.1f fps | ms/s: pso %.0f (%llu new) tex check %.0f tex upload %.0f | resources created %llu\n",
                double(g.frame_number - g.stats_frame) / seconds, g.time_pso_ns / 1e6 / seconds,
                (unsigned long long)g.pso_created, g.time_hash_ns / 1e6 / seconds,
                g.time_upload_ns / 1e6 / seconds, (unsigned long long)g.resources_created);
      g.resources_created = 0;
      char phases[256];
      int n = 0;
      for (uint32_t i = 0; i < 10; ++i)
      {
        n += std::snprintf(phases + n, sizeof(phases) - n, " %s %.0f", kPhaseNames[i], g.phase_ns[i] / 1e6 / seconds);
        g.phase_ns[i] = 0;
      }
      NativeLog("  draw phases ms/s:%s | gpu fence wait %.0f present %.0f\n", phases,
                g.time_fence_ns / 1e6 / seconds, g.time_present_ns / 1e6 / seconds);
      if (g.timestamp_frequency)
      {
        // GPU time per frame by category.
        double frames = std::max<double>(double(g.frame_number - g.stats_frame), 1.0);
        double total = 0.0;
        n = 0;
        for (uint32_t i = 0; i < kGpuCategoryCount; ++i)
        {
          double ms = double(g.gpu_ticks[i]) * 1000.0 / double(g.timestamp_frequency) / frames;
          total += i == kGpuIdle ? 0.0 : ms;
          n += std::snprintf(phases + n, sizeof(phases) - n, " %s %.2f", kGpuCategoryNames[i], ms);
          g.gpu_ticks[i] = 0;
        }
        SYSTEMTIME st;
        GetLocalTime(&st);
        NativeLog("  gpu ms/frame:%s | total %.2f [%02u:%02u:%02u.%03u]\n", phases, total, st.wHour, st.wMinute,
                  st.wSecond, st.wMilliseconds);
      }
      g.time_fence_ns = g.time_present_ns = 0;
      g.stats_frame = g.frame_number;
      g.time_pso_ns = g.time_hash_ns = g.time_upload_ns = 0;
      g.pso_created = 0;
      NativeLog("native frame %llu: draws=%llu skipped=%llu pending=%llu pipelines=%zu rts=%zu resolved=%zu "
                "front=%08X present=%08X vertex_upload=%.1fMB textures=%zu binds tex/resolved/null="
                "%llu/%llu/%llu uploads=%llu (%.1fMB) flushes=%llu resolves copy/draw/swap/packed/lent=%llu/%llu/%llu/%llu/%llu (%llu copied back) guest-res copies=%llu transfers=%llu (skipped %llu, cleared %llu, stencil clears in place %llu) depth restores skipped %llu copy backs skipped %llu of %llu writebacks=%llu\n",
                (unsigned long long)g.frame_number, (unsigned long long)g.draws,
                (unsigned long long)g.draws_skipped, (unsigned long long)g.draws_pending, g.pipelines.size(), g.render_targets.size(),
                g.resolved.size(), frontbuffer, g.present_address, double(g.mirror_bytes) / 1048576.0,
                g.textures.size(), (unsigned long long)g.texture_binds, (unsigned long long)g.resolved_binds,
                (unsigned long long)g.null_binds, (unsigned long long)g.texture_uploads,
                double(g.texture_upload_bytes) / 1048576.0, (unsigned long long)g.flushes,
                (unsigned long long)g.resolves_copied, (unsigned long long)g.resolves_drawn,
                (unsigned long long)g.resolves_swapped, (unsigned long long)g.resolves_packed,
                (unsigned long long)g.resolves_lent, (unsigned long long)g.lent_returned,
                (unsigned long long)g.lowres_copies,
                (unsigned long long)g.transfers, (unsigned long long)g.transfers_skipped,
                (unsigned long long)g.transfers_cleared, (unsigned long long)g.stencil_clears_redirected,
                (unsigned long long)g.depth_restores_skipped, (unsigned long long)g.copy_backs_skipped,
                (unsigned long long)g.copy_back_saves,
                (unsigned long long)g.writebacks);
      NativeLog("  occlusion queries: %llu (%llu visible, %llu unknown, %llu stale), mid-frame submits %llu (early %llu, %llu of them for an idle GPU) | "
                "stale write-back texels %llu | resolve pages "
                "spared from block invalidation %llu, resolved textures dropped %llu\n",
                (unsigned long long)g.occlusion_queries.load(), (unsigned long long)g.occlusion_visible.load(),
                (unsigned long long)g.occlusion_unknown.load(), (unsigned long long)g.occlusion_stale.exchange(0),
                (unsigned long long)g.occlusion_submits,
                (unsigned long long)g.early_submits, (unsigned long long)g.early_submits_idle,
                (unsigned long long)g.writeback_stale.exchange(0),
                (unsigned long long)g.resolve_pages_spared.exchange(0), (unsigned long long)g.resolved_dropped);
      g.occlusion_queries = g.occlusion_visible = g.occlusion_unknown = 0;
      g.occlusion_submits = g.early_submits = g.early_submits_idle = 0;
    }
    g.draws = g.draws_skipped = g.draws_pending = 0;
    g.mirror_bytes = 0;
    g.texture_binds = g.resolved_binds = g.null_binds = 0;
    g.texture_uploads = g.texture_upload_bytes = 0;
    g.resolves_copied = g.resolves_drawn = g.resolves_swapped = g.resolves_packed = g.lowres_copies = 0;
    g.resolves_lent = g.lent_returned = 0;
    g.transfers = 0;
    g.transfers_skipped = 0;
    g.transfers_cleared = 0;
    g.stencil_clears_redirected = 0;
    g.depth_restores_skipped = 0;
    g.copy_backs_skipped = 0;
    g.copy_back_saves = 0;
    g.writebacks = 0;
  }

  void FitWindowToMonitor(void *native_window_handle)
  {
    HWND hwnd = static_cast<HWND>(native_window_handle);
    if (!hwnd || IsZoomed(hwnd))
    {
      return;
    }
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
    {
      return;
    }
    RECT wr = {}, cr = {};
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    if (EqualRect(&wr, &mi.rcMonitor))
    {
      return; // borderless fullscreen
    }
    const RECT &work = mi.rcWork;
    LONG border_w = (wr.right - wr.left) - (cr.right - cr.left);
    LONG border_h = (wr.bottom - wr.top) - (cr.bottom - cr.top);
    LONG work_w = work.right - work.left, work_h = work.bottom - work.top;
    if (wr.right - wr.left <= work_w && wr.bottom - wr.top <= work_h)
    {
      return;
    }
    // Largest client area of the same aspect (ao2_aspect_ratio "auto" follows it) within 90% of the work area.
    double cw = cr.right > 0 && cr.bottom > 0 ? double(cr.right) : 16.0;
    double ch = cr.right > 0 && cr.bottom > 0 ? double(cr.bottom) : 9.0;
    double unit = std::min(double(work_w - border_w) * 0.9 / cw, double(work_h - border_h) * 0.9 / ch);
    LONG w = LONG(unit * cw) + border_w, h = LONG(unit * ch) + border_h;
    SetWindowPos(hwnd, nullptr, work.left + (work_w - w) / 2, work.top + (work_h - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    NativeLog("window %ldx%ld does not fit the %ldx%ld work area: resized to %ldx%ld\n", wr.right - wr.left,
              wr.bottom - wr.top, work_w, work_h, w, h);
  }
} // namespace ao2::gpu
