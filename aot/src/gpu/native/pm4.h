// pm4.h - PM4 command processor of the native renderer.
//
// NativeGraphicsSystem feeds it the guest ring buffer on its GPU command
// thread (like the hardware CP, asynchronously to the guest). Memory writes,
// fences, interrupts and query results are produced as the packets execute;
// WAIT_REG_MEM really waits. Draws, resolves and swaps are forwarded to the
// D3D12 renderer.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

namespace ao2::gpu
{
  constexpr uint32_t kRegisterCount = 0x6000;

  struct RegisterFile
  {
    uint32_t v[kRegisterCount] = {};
    // Incremented on writes to the float constants 0-255 / 256-511
    // (0x4000-0x43FF / 0x4400-0x47FF), so unchanged constants are not uploaded again.
    uint32_t float_version[2] = {};

    template <typename T>
    T Get() const
    {
      return reinterpret_cast<const T &>(v[T::register_index]);
    }
    template <typename T>
    T Get(uint32_t index) const
    {
      return reinterpret_cast<const T &>(v[index]);
    }
    float F(uint32_t index) const
    {
      return reinterpret_cast<const float &>(v[index]);
    }
  };

  // Microcode loaded with IM_LOAD / IM_LOAD_IMMEDIATE.
  struct ShaderMicrocode
  {
    const uint32_t *dwords = nullptr; // big-endian, in guest memory
    uint32_t dword_count = 0;
    uint32_t guest_address = 0; // physical, 0 for immediate
    uint64_t hash = 0;
  };

  struct DrawPacket
  {
    rex::graphics::xenos::PrimitiveType prim_type;
    uint32_t index_count;
    bool indexed;
    uint32_t index_base;   // physical
    uint32_t index_words;  // VGT_DMA_SIZE.num_words
    rex::graphics::xenos::IndexFormat index_format;
    rex::graphics::xenos::Endian index_endian;
  };

  class Pm4Consumer
  {
  public:
    // Callbacks invoked synchronously while parsing.
    std::function<void(const DrawPacket &)> on_draw;    // RB_MODECONTROL != copy
    std::function<void(const DrawPacket &)> on_resolve; // RB_MODECONTROL == copy
    std::function<void(uint32_t frontbuffer, uint32_t width, uint32_t height)> on_swap;
    std::function<void(uint32_t cpu_mask)> on_interrupt;
    // Polled while WAIT_REG_MEM waits; returning false abandons the wait.
    std::function<bool()> keep_waiting;
    // Occlusion query begin/end (EVENT_WRITE_ZPD, sample count struct address).
    // Returning true: the end struct is written later by the callee; false:
    // fake_occlusion_samples are reported at once.
    std::function<bool(uint32_t address, bool end)> on_occlusion;

    // Parses `count` big-endian dwords at `dwords` (host pointer into guest memory).
    void Consume(const uint32_t *dwords, uint32_t count);

    void WriteRegister(uint32_t index, uint32_t value);
    // `count` big-endian values to consecutive registers from `base` (or all to
    // `base` with `same`): plain registers are stored directly, the few with side
    // effects go through WriteRegister.
    void WriteRegisters(uint32_t base, const uint32_t *values_be, uint32_t count, bool same = false);
    uint32_t ReadRegister(uint32_t index) const { return index < kRegisterCount ? regs_.v[index] : 0; }

    const RegisterFile &regs() const { return regs_; }
    RegisterFile &regs() { return regs_; }
    const ShaderMicrocode &vertex_shader() const { return vs_; }
    const ShaderMicrocode &pixel_shader() const { return ps_; }

    // Incremented on vblank and swap (EVENT_WRITE_SHD counter writes).
    std::atomic<uint32_t> counter{0};
    // Samples reported for occlusion queries that are not counted on the GPU.
    uint32_t fake_occlusion_samples = 1000;

    uint64_t unknown_opcodes = 0;
    uint64_t parse_errors = 0;
    uint64_t waits = 0; // WAIT_REG_MEM that had to wait
    std::atomic<uint64_t> wait_ns{0};
    // Waits by polled register/address (for the stats line): key -> {count, ns}.
    struct WaitStat
    {
      uint32_t poll, ref, info;
      uint64_t count, ns;
    };
    WaitStat wait_stats[8] = {};
    std::atomic<bool> wait_stats_reset{false};

    // Display gamma ramp (DC_LUT_*, written by D3D's SetGammaRamp/SetPWLGamma
    // through MMIO or the ring), applied by the renderer when presenting:
    // 256-entry table (DC_LUT_30_COLOR layout: blue 0:9, green 10:19, red
    // 20:29) and 128-entry PWL per component (DC_LUT_PWL_DATA: base 0:15,
    // delta 16:31). Initialized to identity like the SDK.
    Pm4Consumer();
    uint32_t gamma_table[256];
    uint32_t gamma_pwl[128][3];
    std::atomic<uint32_t> gamma_version{0}; // incremented on every ramp write

  private:
    uint32_t gamma_component_ = 0; // sequential DC_LUT_SEQ_COLOR / PWL component
    void Draw(const uint32_t *p, uint32_t count);
    ShaderMicrocode LoadShader(const uint32_t *dwords, uint32_t count, uint32_t address);
    bool ExecutePacket3(uint32_t packet, const uint32_t *p, uint32_t n);
    void WaitRegMem(const uint32_t *p);

    RegisterFile regs_;
    uint32_t ib_depth_ = 0;
    uint64_t bin_mask_ = ~0ull;
    uint64_t bin_select_ = ~0ull;
    ShaderMicrocode vs_;
    ShaderMicrocode ps_;
  };

  uint32_t LoadBE32(const void *p);
  uint32_t SwapEndian(uint32_t value, rex::graphics::xenos::Endian endian);
  uint64_t HashDwords(const uint32_t *p, uint32_t count);
} // namespace ao2::gpu
