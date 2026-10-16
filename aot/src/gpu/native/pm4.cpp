// pm4.cpp - see pm4.h. Packet semantics follow the ReXGlue/Xenia command
// processor (sdk/src/graphics/command_processor.cpp), minus GPU emulation.
#include "pm4.h"

#include <chrono>

#include "renderer.h"
#include <cstring>

#include <unordered_map>

#include <rex/hash.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

namespace ao2::gpu
{
  namespace xenos = rex::graphics::xenos;
  namespace reg = rex::graphics::reg;
  using namespace rex::graphics; // XE_GPU_REG_* constants

  namespace
  {
    enum : uint32_t
    {
      kMeInit = 0x48,
      kNop = 0x10,
      kInterrupt = 0x54,
      kXeSwap = 0x64,
      kIndirectBuffer = 0x3F,
      kIndirectBufferPfd = 0x37,
      kWaitRegMem = 0x3C,
      kRegRmw = 0x21,
      kRegToMem = 0x3E,
      kMemWrite = 0x3D,
      kCondWrite = 0x45,
      kEventWrite = 0x46,
      kEventWriteShd = 0x58,
      kEventWriteExt = 0x5A,
      kEventWriteZpd = 0x5B,
      kDrawIndx = 0x22,
      kDrawIndx2 = 0x36,
      kSetConstant = 0x2D,
      kSetConstant2 = 0x55,
      kLoadAluConstant = 0x2F,
      kSetShaderConstants = 0x56,
      kImLoad = 0x27,
      kImLoadImmediate = 0x2B,
      kInvalidateState = 0x3B,
      kVizQuery = 0x23,
      kSetBinMaskLo = 0x60,
      kSetBinMaskHi = 0x61,
      kSetBinSelectLo = 0x62,
      kSetBinSelectHi = 0x63,
      kSetBinMask = 0x50,
      kSetBinSelect = 0x51,
    };

    uint8_t *Physical(uint32_t address)
    {
      return rex::Runtime::instance()->memory()->TranslatePhysical(address);
    }

    void StoreGuest(uint32_t address_and_endian, uint32_t value)
    {
      auto endian = xenos::Endian(address_and_endian & 3);
      uint32_t v = SwapEndian(value, endian);
      std::memcpy(Physical(address_and_endian & ~3u), &v, 4);
    }

    uint32_t LoadGuest(uint32_t address_and_endian)
    {
      uint32_t v;
      std::memcpy(&v, Physical(address_and_endian & ~3u), 4);
      return SwapEndian(v, xenos::Endian(address_and_endian & 3));
    }

    uint32_t ConstantBase(uint32_t type)
    {
      switch (type)
      {
      case 0: return 0x4000; // ALU
      case 1: return 0x4800; // fetch
      case 2: return 0x4900; // bool
      case 3: return 0x4908; // loop
      case 4: return 0x2000; // registers
      default: return UINT32_MAX;
      }
    }

    bool Compare(uint32_t func, uint32_t value, uint32_t ref)
    {
      switch (func & 7)
      {
      case 1: return value < ref;
      case 2: return value <= ref;
      case 3: return value == ref;
      case 4: return value != ref;
      case 5: return value >= ref;
      case 6: return value > ref;
      case 7: return true;
      default: return false;
      }
    }
  } // namespace

  uint32_t LoadBE32(const void *p)
  {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
  }

  // Converts between big-endian guest memory and host order per Xenos endian
  // mode; also used in reverse (the swaps are involutions).
  uint32_t SwapEndian(uint32_t v, xenos::Endian endian)
  {
    switch (endian)
    {
    case xenos::Endian::k8in16:
      return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    case xenos::Endian::k8in32:
      return __builtin_bswap32(v);
    case xenos::Endian::k16in32:
      return (v >> 16) | (v << 16);
    default:
      return v;
    }
  }

  // Shader microcode hash (FNV-1a; keys the shader caches and pipelines.bin,
  // so it must not change).
  uint64_t HashDwords(const uint32_t *p, uint32_t count)
  {
    uint64_t h = 0xCBF29CE484222325ull ^ count;
    for (uint32_t i = 0; i < count; ++i)
    {
      h ^= p[i];
      h *= 0x100000001B3ull;
    }
    return h;
  }

  // Shaders are loaded for nearly every draw: the dword-serial FNV chain is
  // computed once per distinct microcode, found again by its (much faster) XXH3.
  uint64_t ShaderHash(const uint32_t *p, uint32_t count)
  {
    static thread_local std::unordered_map<uint64_t, uint64_t> known;
    uint64_t fast = XXH3_64bits(p, size_t(count) * 4) ^ (uint64_t(count) << 48);
    auto it = known.find(fast);
    if (it != known.end())
    {
      return it->second;
    }
    if (known.size() > 65536)
    {
      known.clear();
    }
    uint64_t h = HashDwords(p, count);
    known.emplace(fast, h);
    return h;
  }

  void Pm4Consumer::WriteRegister(uint32_t index, uint32_t value)
  {
    if (index >= kRegisterCount)
    {
      return;
    }
    regs_.v[index] = value;
    if (index - 0x4000u < 0x800u)
    {
      ++regs_.float_version[(index - 0x4000u) >> 10];
    }
    else if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG0 + 7)
    {
      // Scratch register writeback to memory (used by D3D for fences).
      uint32_t scratch = index - XE_GPU_REG_SCRATCH_REG0;
      if (regs_.v[XE_GPU_REG_SCRATCH_UMSK] & (1u << scratch))
      {
        uint32_t address = regs_.v[XE_GPU_REG_SCRATCH_ADDR] + scratch * 4;
        uint32_t be = __builtin_bswap32(value);
        std::memcpy(Physical(address), &be, 4);
      }
    }
    else if (index == XE_GPU_REG_COHER_STATUS_HOST)
    {
      // Memory is always coherent: the request completes immediately.
      regs_.v[index] &= ~0x80000000u;
    }
    else if (index >= XE_GPU_REG_DC_LUT_RW_INDEX && index <= XE_GPU_REG_DC_LUT_30_COLOR)
    {
      // Gamma ramp (see the SDK's CommandProcessor::WriteRegister).
      uint32_t rw_index = regs_.v[XE_GPU_REG_DC_LUT_RW_INDEX] & 0xFF;
      uint32_t enable = regs_.v[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 7; // blue, green, red
      auto next_index = [&](uint32_t i) { regs_.v[XE_GPU_REG_DC_LUT_RW_INDEX] = i; };
      switch (index)
      {
      case XE_GPU_REG_DC_LUT_RW_INDEX:
        gamma_component_ = 0;
        break;
      case XE_GPU_REG_DC_LUT_SEQ_COLOR:
      {
        // Red, green, blue in sequence, 10 bits in 6:15.
        if (enable & (1u << (2 - gamma_component_)))
        {
          uint32_t shift = (2 - gamma_component_) * 10;
          gamma_table[rw_index] = (gamma_table[rw_index] & ~(0x3FFu << shift)) | (((value >> 6) & 0x3FF) << shift);
          ++gamma_version;
        }
        if (++gamma_component_ >= 3)
        {
          gamma_component_ = 0;
          next_index((rw_index + 1) & 0xFF);
        }
        break;
      }
      case XE_GPU_REG_DC_LUT_PWL_DATA:
      {
        if (enable & (1u << (2 - gamma_component_)))
        {
          gamma_pwl[rw_index & 0x7F][gamma_component_] = value & 0xFFC0FFC0u;
          ++gamma_version;
        }
        if (++gamma_component_ >= 3)
        {
          gamma_component_ = 0;
          next_index((rw_index & 0x80) | ((rw_index + 1) & 0x7F));
        }
        break;
      }
      case XE_GPU_REG_DC_LUT_30_COLOR:
      {
        uint32_t keep = 0;
        for (uint32_t c = 0; c < 3; ++c)
        {
          if (!(enable & (1u << c)))
          {
            keep |= 0x3FFu << (c * 10);
          }
        }
        if (enable)
        {
          gamma_table[rw_index] = (gamma_table[rw_index] & keep) | (value & 0x3FFFFFFF & ~keep);
          ++gamma_version;
        }
        gamma_component_ = 0;
        next_index((rw_index + 1) & 0xFF);
        break;
      }
      default:
        break;
      }
    }
  }

  void Pm4Consumer::WriteRegisters(uint32_t base, const uint32_t *values_be, uint32_t count, bool same)
  {
    // Registers WriteRegister handles specially (everything else is a store).
    auto special = [](uint32_t index) {
      return (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG0 + 7) ||
             index == XE_GPU_REG_COHER_STATUS_HOST ||
             (index >= XE_GPU_REG_DC_LUT_RW_INDEX && index <= XE_GPU_REG_DC_LUT_30_COLOR) || index >= kRegisterCount;
    };
    if (!count)
    {
      return;
    }
    if (!same && base >= 0x4000u && count <= 0x800u && base + count <= 0x4800u)
    {
      // Float constants (most of the volume): bulk copy.
      for (uint32_t m = 0; m < count; ++m)
      {
        regs_.v[base + m] = LoadBE32(values_be + m);
      }
      ++regs_.float_version[(base - 0x4000u) >> 10];
      ++regs_.float_version[(base + count - 1 - 0x4000u) >> 10];
      return;
    }
    for (uint32_t m = 0; m < count; ++m)
    {
      uint32_t index = same ? base : base + m;
      uint32_t value = LoadBE32(values_be + m);
      if (special(index))
      {
        WriteRegister(index, value);
        continue;
      }
      regs_.v[index] = value;
      if (index - 0x4000u < 0x800u)
      {
        ++regs_.float_version[(index - 0x4000u) >> 10];
      }
    }
  }

  Pm4Consumer::Pm4Consumer()
  {
    for (uint32_t i = 0; i < 256; ++i)
    {
      uint32_t v = i * 0x3FF / 0xFF;
      gamma_table[i] = v | (v << 10) | (v << 20);
    }
    for (uint32_t i = 0; i < 128; ++i)
    {
      uint32_t base = (i * 0xFFFF / 0x7F) & ~0x3Fu;
      uint32_t delta = i < 0x7F ? 0x200 : 0;
      gamma_pwl[i][0] = gamma_pwl[i][1] = gamma_pwl[i][2] = base | (delta << 16);
    }
  }

  ShaderMicrocode Pm4Consumer::LoadShader(const uint32_t *dwords, uint32_t count, uint32_t address)
  {
    ShaderMicrocode s;
    s.dwords = dwords;
    s.dword_count = count;
    s.guest_address = address;
    s.hash = ShaderHash(dwords, count);
    return s;
  }

  void Pm4Consumer::Draw(const uint32_t *p, uint32_t count)
  {
    if (!count)
    {
      ++parse_errors;
      return;
    }
    reg::VGT_DRAW_INITIATOR init;
    init.value = LoadBE32(p);
    WriteRegister(XE_GPU_REG_VGT_DRAW_INITIATOR, init.value);
    DrawPacket d{};
    d.prim_type = init.prim_type;
    d.index_count = init.num_indices;
    if (init.source_select == xenos::SourceSelect::kDMA)
    {
      if (count < 3)
      {
        ++parse_errors;
        return;
      }
      reg::VGT_DMA_SIZE size;
      d.indexed = true;
      d.index_base = LoadBE32(p + 1);
      size.value = LoadBE32(p + 2);
      d.index_words = size.num_words;
      d.index_endian = size.swap_mode;
      d.index_format = init.index_size;
      WriteRegister(XE_GPU_REG_VGT_DMA_BASE, d.index_base);
      WriteRegister(XE_GPU_REG_VGT_DMA_SIZE, size.value);
    }
    else if (init.source_select != xenos::SourceSelect::kAutoIndex)
    {
      ++parse_errors;
      return;
    }
    auto viz = regs_.Get<reg::PA_SC_VIZ_QUERY>();
    if (viz.viz_query_ena && viz.kill_pix_post_hi_z)
    {
      return;
    }
    if (regs_.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy)
    {
      if (on_resolve)
      {
        on_resolve(d);
      }
    }
    else if (on_draw)
    {
      on_draw(d);
    }
  }

  bool Pm4Consumer::ExecutePacket3(uint32_t packet, const uint32_t *p, uint32_t n)
  {
    uint32_t op = (packet >> 8) & 0x7F;
    if ((packet & 1) && (!(bin_select_ & bin_mask_) || op == kXeSwap))
    {
      // Predicated out (tiling bins); predicated swaps are never valid.
      return true;
    }
    switch (op)
    {
    case kDrawIndx:
      // First dword is the viz query token.
      Draw(p + 1, n - 1);
      break;
    case kDrawIndx2:
      Draw(p, n);
      break;
    case kSetConstant:
    {
      uint32_t offset_type = LoadBE32(p);
      uint32_t base = ConstantBase((offset_type >> 16) & 0xFF);
      if (base == UINT32_MAX)
      {
        ++unknown_opcodes;
        break;
      }
      base += offset_type & 0x7FF;
      if (n > 1)
      {
        WriteRegisters(base, p + 1, n - 1);
      }
      break;
    }
    case kSetConstant2:
    case kSetShaderConstants:
    {
      uint32_t index = LoadBE32(p) & 0xFFFF;
      if (n > 1)
      {
        WriteRegisters(index, p + 1, n - 1);
      }
      break;
    }
    case kLoadAluConstant:
    {
      uint32_t address = LoadBE32(p) & 0x3FFFFFFF;
      uint32_t offset_type = LoadBE32(p + 1);
      uint32_t size = LoadBE32(p + 2) & 0xFFF;
      uint32_t base = ConstantBase((offset_type >> 16) & 0xFF);
      if (base == UINT32_MAX)
      {
        ++unknown_opcodes;
        break;
      }
      base += offset_type & 0x7FF;
      WriteRegisters(base, reinterpret_cast<const uint32_t *>(Physical(address)), size);
      break;
    }
    case kImLoad:
    {
      uint32_t addr_type = LoadBE32(p);
      uint32_t size = LoadBE32(p + 1) & 0xFFFF;
      uint32_t addr = addr_type & ~3u;
      auto s = LoadShader(reinterpret_cast<const uint32_t *>(Physical(addr)), size, addr);
      ((addr_type & 3) == uint32_t(xenos::ShaderType::kVertex) ? vs_ : ps_) = s;
      break;
    }
    case kImLoadImmediate:
    {
      uint32_t shader_type = LoadBE32(p);
      uint32_t size = LoadBE32(p + 1) & 0xFFFF;
      if (size + 2 > n)
      {
        ++parse_errors;
        break;
      }
      auto s = LoadShader(p + 2, size, 0);
      (shader_type == uint32_t(xenos::ShaderType::kVertex) ? vs_ : ps_) = s;
      break;
    }
    case kIndirectBuffer:
    case kIndirectBufferPfd:
    {
      uint32_t address = LoadBE32(p) & 0x1FFFFFFF;
      uint32_t size = LoadBE32(p + 1) & 0xFFFFF;
      if (ib_depth_ < 4)
      {
        ++ib_depth_;
        Consume(reinterpret_cast<const uint32_t *>(Physical(address)), size);
        --ib_depth_;
      }
      break;
    }
    case kRegRmw:
    {
      // Bit 31: and-mask is a register index, bit 30: or-mask is a register index.
      uint32_t setting = LoadBE32(p);
      uint32_t and_mask = LoadBE32(p + 1);
      uint32_t or_mask = LoadBE32(p + 2);
      uint32_t r = setting & 0x1FFF;
      if ((setting >> 31) & 1)
      {
        and_mask = regs_.v[and_mask & 0x1FFF];
      }
      if ((setting >> 30) & 1)
      {
        or_mask = regs_.v[or_mask & 0x1FFF];
      }
      WriteRegister(r, (regs_.v[r] & and_mask) | or_mask);
      break;
    }
    case kRegToMem:
      StoreGuest(LoadBE32(p + 1), ReadRegister(LoadBE32(p)));
      break;
    case kMemWrite:
    {
      uint32_t address = LoadBE32(p);
      for (uint32_t m = 1; m < n; ++m)
      {
        StoreGuest(address, LoadBE32(p + m));
        address += 4;
      }
      break;
    }
    case kCondWrite:
    {
      uint32_t wait_info = LoadBE32(p);
      uint32_t poll = LoadBE32(p + 1);
      uint32_t ref = LoadBE32(p + 2);
      uint32_t mask = LoadBE32(p + 3);
      uint32_t write_addr = LoadBE32(p + 4);
      uint32_t write_data = LoadBE32(p + 5);
      uint32_t value = (wait_info & 0x10) ? LoadGuest(poll) : ReadRegister(poll);
      if (Compare(wait_info, value & mask, ref))
      {
        if (wait_info & 0x100)
        {
          StoreGuest(write_addr, write_data);
        }
        else
        {
          WriteRegister(write_addr, write_data);
        }
      }
      break;
    }
    case kWaitRegMem:
      WaitRegMem(p);
      break;
    case kEventWrite:
      WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, LoadBE32(p) & 0x3F);
      break;
    case kEventWriteShd:
    {
      uint32_t initiator = LoadBE32(p);
      uint32_t address = LoadBE32(p + 1);
      uint32_t value = LoadBE32(p + 2);
      WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
      StoreGuest(address, ((initiator >> 31) & 1) ? counter.load() : value);
      break;
    }
    case kEventWriteExt:
    {
      // Screen extents of previous draws: report the whole screen.
      uint32_t initiator = LoadBE32(p);
      uint32_t address = LoadBE32(p + 1) & ~3u;
      WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
      const uint16_t extents[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
      uint8_t *dst = Physical(address);
      for (uint32_t i = 0; i < 6; ++i)
      {
        uint16_t be = uint16_t((extents[i] >> 8) | (extents[i] << 8));
        std::memcpy(dst + i * 2, &be, 2);
      }
      break;
    }
    case kEventWriteZpd:
    {
      // Occlusion query end: D3D writes 0xFFFFFEED to ZPass/ZFail and waits
      // for the GPU to replace it.
      WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, LoadBE32(p) & 0x3F);
      uint32_t address = regs_.v[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
      if (!address)
      {
        break;
      }
      uint32_t counts[8];
      uint8_t *mem = Physical(address);
      std::memcpy(counts, mem, sizeof(counts));
      const uint32_t finished = __builtin_bswap32(0xFFFFFEEDu);
      bool is_end = counts[4] == finished || counts[5] == finished || counts[2] == finished ||
                    counts[3] == finished;
      if (on_occlusion && on_occlusion(address, is_end))
      {
        // Begin: the counts start from zero. End: left 0xFFFFFEED until the
        // GPU has counted the draws.
        if (!is_end)
        {
          std::memset(mem, 0, sizeof(counts));
        }
        break;
      }
      std::memset(counts, 0, sizeof(counts));
      if (is_end)
      {
        counts[0] = fake_occlusion_samples; // Total_A
        counts[4] = fake_occlusion_samples; // ZPass_A
      }
      std::memcpy(mem, counts, sizeof(counts));
      if (is_end)
      {
        RendererNotifyOcclusion();
      }
      break;
    }
    case kInterrupt:
      if (on_interrupt)
      {
        on_interrupt(LoadBE32(p));
      }
      break;
    case kXeSwap:
      if (n >= 4 && on_swap)
      {
        on_swap(LoadBE32(p + 1), LoadBE32(p + 2), LoadBE32(p + 3));
      }
      ++counter;
      break;
    case kVizQuery:
    {
      uint32_t d = LoadBE32(p);
      uint32_t id = d & 0x3F;
      if (d & 0x100)
      {
        // End: report visible.
        regs_.v[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0 + (id >> 5)] |= 1u << (id & 31);
      }
      break;
    }
    case kSetBinMaskLo:
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | LoadBE32(p);
      break;
    case kSetBinMaskHi:
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(LoadBE32(p)) << 32);
      break;
    case kSetBinSelectLo:
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | LoadBE32(p);
      break;
    case kSetBinSelectHi:
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(LoadBE32(p)) << 32);
      break;
    case kSetBinMask:
      bin_mask_ = (uint64_t(LoadBE32(p)) << 32) | LoadBE32(p + 1);
      break;
    case kSetBinSelect:
      bin_select_ = (uint64_t(LoadBE32(p)) << 32) | LoadBE32(p + 1);
      break;
    case kMeInit:
    case kNop:
    case kInvalidateState:
      break;
    default:
      ++unknown_opcodes;
      break;
    }
    return true;
  }

  // Waits until a register or memory value satisfies the condition (e.g. the
  // CPU acknowledging an interrupt, or a fence written by the CPU).
  void Pm4Consumer::WaitRegMem(const uint32_t *p)
  {
    uint32_t wait_info = LoadBE32(p);
    uint32_t poll = LoadBE32(p + 1);
    uint32_t ref = LoadBE32(p + 2);
    uint32_t mask = LoadBE32(p + 3);
    uint32_t wait = LoadBE32(p + 4);
    bool counted = false;
    std::chrono::steady_clock::time_point start;
    uint32_t reported_seconds = 0;
    for (;;)
    {
      uint32_t value = (wait_info & 0x10) ? LoadGuest(poll) : ReadRegister(poll);
      if (Compare(wait_info, value & mask, ref))
      {
        if (reported_seconds)
        {
          NativeLog("WAIT_REG_MEM satisfied after %us\n", reported_seconds);
        }
        if (counted)
        {
          uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count());
          wait_ns += ns;
          if (wait_stats_reset.exchange(false))
          {
            std::memset(wait_stats, 0, sizeof(wait_stats));
          }
          WaitStat *slot = nullptr;
          for (auto &w : wait_stats)
          {
            if (w.count && w.poll == poll && w.info == wait_info)
            {
              slot = &w;
              break;
            }
            if (!w.count && !slot)
            {
              slot = &w;
            }
          }
          if (slot)
          {
            if (!slot->count)
            {
              slot->poll = poll;
              slot->info = wait_info;
            }
            slot->ref = ref;
            ++slot->count;
            slot->ns += ns;
          }
        }
        return;
      }
      if (!counted)
      {
        counted = true;
        ++waits;
        start = std::chrono::steady_clock::now();
      }
      uint32_t seconds = uint32_t(
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count());
      if (seconds >= reported_seconds + 2)
      {
        reported_seconds = seconds;
        NativeLog("WAIT_REG_MEM %us: info %08X poll %08X ref %08X mask %08X value %08X\n", seconds, wait_info,
                  poll, ref, mask, value);
      }
      if (keep_waiting && !keep_waiting())
      {
        return;
      }
      if (wait >= 0x100)
      {
        rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));
      }
      else
      {
        rex::thread::MaybeYield();
      }
      rex::thread::SyncMemory();
    }
  }

  void Pm4Consumer::Consume(const uint32_t *dw, uint32_t count)
  {
    uint32_t i = 0;
    while (i < count)
    {
      uint32_t packet = LoadBE32(dw + i++);
      if (packet == 0 || packet == 0x0BADF00D || packet == 0xCDCDCDCD)
      {
        continue;
      }
      switch (packet >> 30)
      {
      case 0:
      {
        uint32_t n = ((packet >> 16) & 0x3FFF) + 1;
        uint32_t base = packet & 0x7FFF;
        bool one = (packet >> 15) & 1;
        if (i + n > count)
        {
          ++parse_errors;
          return;
        }
        WriteRegisters(base, dw + i, n, one);
        i += n;
        break;
      }
      case 1:
        if (i + 2 > count)
        {
          ++parse_errors;
          return;
        }
        WriteRegister(packet & 0x7FF, LoadBE32(dw + i));
        WriteRegister((packet >> 11) & 0x7FF, LoadBE32(dw + i + 1));
        i += 2;
        break;
      case 2:
        break;
      case 3:
      {
        uint32_t n = ((packet >> 16) & 0x3FFF) + 1;
        if (i + n > count)
        {
          ++parse_errors;
          return;
        }
        ExecutePacket3(packet, dw + i, n);
        i += n;
        break;
      }
      }
    }
  }
} // namespace ao2::gpu
