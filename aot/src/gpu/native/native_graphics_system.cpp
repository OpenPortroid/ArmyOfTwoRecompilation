// native_graphics_system.cpp - see native_graphics_system.h.
//
// Guest-visible behavior reproduced here (from the SDK's GraphicsSystem and
// CommandProcessor, without the emulation):
// - GPU registers at 0x7FC80000 (MMIO): CP_RB_WPTR moves the ring write
//   pointer, a few status registers read back fixed values.
// - Ring buffer: consumed by a dedicated guest-visible host thread (the
//   command processor), which writes the read pointer back to guest memory.
//   Command-stream interrupts run synchronously on that thread: D3D's handler
//   reads callback slots the command stream fills right before the interrupt
//   and resets right after a WAIT_REG_MEM on the handler's acknowledgement.
// - Vblank (60 Hz) interrupts come from a second thread.
#include "native_graphics_system.h"
#include "../../frame_spikes.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include "../../aspect_ratio.h"
#include "../../hang_watchdog.h"
#include "pm4.h"
#include "renderer.h"
#include "shader_cache.h"
#include "../../log_files.h"

REXCVAR_DEFINE_STRING(ao2_native_gpu, "on", "GPU",
                      "Renderer: on (native D3D12), off (ReXGlue Xenos emulation plugin)")
    .allowed({"on", "off"});
REXCVAR_DEFINE_BOOL(ao2_native_gpu_dump_shaders, false, "GPU",
                    "Native renderer: dump microcode of every shader to shader_dump/");
REXCVAR_DEFINE_STRING(ao2_native_gpu_shader_override_dir, "", "GPU",
                      "Native renderer: directory of hand-written HLSL used instead of the translation "
                      "(<hash>_<modification>.<vs|ps>.hlsl, as in shader_dump/)");
REXCVAR_DEFINE_BOOL(ao2_native_gpu_async_pipelines, true, "GPU",
                    "Native renderer: create pipelines never seen before on a background thread (the draw "
                    "is skipped meanwhile) instead of stalling the frame");
REXCVAR_DEFINE_BOOL(ao2_native_gpu_occlusion_queries, true, "GPU",
                    "Native renderer: count occlusion query samples on the GPU (false = always report "
                    "ao2_native_gpu_occlusion_samples)");
REXCVAR_DEFINE_INT32(ao2_native_gpu_submit_draws, 48, "GPU",
                     "Native renderer: submit the frame's commands early, after at least this many draws, when "
                     "the GPU is idle or occlusion queries were ended (0 = only at idle/swap)");
REXCVAR_DEFINE_INT32(ao2_native_gpu_occlusion_samples, 1000, "GPU",
                     "Native renderer: samples reported by occlusion queries that are not counted on the GPU");
REXCVAR_DEFINE_INT32(ao2_native_gpu_capture_frame, 0, "GPU",
                     "Native renderer: write every draw/resolve of this frame to frame_trace.txt (0 = off)");
REXCVAR_DEFINE_BOOL(ao2_native_gpu_static_tex_post, true, "GPU",
                    "Native renderer: pixel shader variants per texture post-processing of the draw (none / gamma "
                    "on RGB) instead of per-component tests in every fetch; the same result");
REXCVAR_DEFINE_INT32(ao2_native_gpu_capture_slow_us, 0, "GPU",
                     "Native renderer: trace the frame after one whose GPU time was above this many microseconds "
                     "(at most 5, 4 s apart; slow_N_frame_trace.txt / slow_N_gpu_draws.txt; 0 = off)");
REXCVAR_DEFINE_INT32(ao2_native_gpu_writeback_texels, 4096, "GPU",
                     "Native renderer: resolves of up to this many texels are copied back to guest memory");
REXCVAR_DEFINE_BOOL(ao2_native_gpu_capture_draws, false, "GPU",
                    "Native renderer: in captured frames also save the render target every 100 draws (large at 2x)");
REXCVAR_DEFINE_STRING(ao2_native_gpu_capture_after_draws, "", "GPU",
                      "Native renderer: in captured frames save color, depth and stencil after these draws "
                      "(comma separated draw numbers of frame_trace.txt)");
REXCVAR_DEFINE_STRING(ao2_native_gpu_skip_shaders, "", "GPU",
                      "Native renderer debugging: skip draws of these shaders (comma separated vsXXXXXXXX / "
                      "psXXXXXXXX, first 8 hex digits of the hash)");
REXCVAR_DEFINE_BOOL(ao2_native_gpu_debug_upload_vertices, false, "GPU",
                    "Native renderer debugging: upload fetched vertex pages on every draw (ignore write tracking)");
REXCVAR_DEFINE_BOOL(ao2_native_gpu_debug_layer, false, "GPU",
                    "Native renderer: enable the D3D12 debug layer, messages go to native_gpu.log (slow)");
REXCVAR_DEFINE_INT32(ao2_native_gpu_stats_every, 600, "GPU",
                     "Native renderer: write native_gpu.log stats every N frames (0 = never)");
REXCVAR_DEFINE_DOUBLE(ao2_native_gpu_resolution_scale, 2.0, "GPU",
                      "Native renderer: internal resolution multiplier in steps of 0.25 (1 = 1280x720 at 16:9, "
                      "1.5 = 1920x1080, 2 = 2560x1440, up to 4)");
REXCVAR_DEFINE_INT32(ao2_native_gpu_small_target_width, 480, "GPU",
                     "Native renderer: render targets at most this wide (guest pixels: downsampled copies, blur, "
                     "bloom, depth of field) are emulated at guest resolution - their draws and what is resolved "
                     "from them sample resolved textures through guest-sized copies (0 = never)");
REXCVAR_DEFINE_INT32(ao2_native_gpu_vblank_hz, 60, "GPU",
                     "Native renderer: vertical blank rate seen by the game (with a 1-vblank presentation interval the "
                     "frame rate follows it; see ao2_fps_unlock)")
    .range(30, 240);
REXCVAR_DEFINE_BOOL(ao2_native_gpu_vsync, false, "GPU",
                    "Native renderer: vertical sync - presents wait for the monitor's refresh (no tearing; the frame "
                    "rate stays at or below the refresh rate)");
REXCVAR_DEFINE_BOOL(ao2_native_gpu_frame_pacing, true, "GPU",
                    "Native renderer: with ao2_fps_unlock a frame that is ready early is held until the frame time "
                    "of ao2_fps_target has passed since the last one (even frame times when frames take turns "
                    "being long and short, as with ao2_decouple)");
REXCVAR_DEFINE_INT32(ao2_native_gpu_pacing_margin_us, 4000, "GPU",
                     "Native renderer: ao2_native_gpu_frame_pacing holds frames up to this many microseconds longer "
                     "after a late one, so that the next frames that come as late still make their slot (0 = only "
                     "from the last present)")
    .range(0, 10000);

namespace ao2::gpu
{
  using rex::X_STATUS;

  namespace
  {
    FILE *g_log = nullptr;
    std::mutex g_log_mutex;
    std::atomic<void *> g_present_window{nullptr};

    constexpr uint32_t kNoWritePointer = 0xBAADF00D;

    // Adds the lifetime of the scope to a nanosecond counter.
    struct ScopedTime
    {
      std::atomic<uint64_t> &counter;
      std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      explicit ScopedTime(std::atomic<uint64_t> &c) : counter(c) {}
      ~ScopedTime()
      {
        counter += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
      }
    };

    class NativeGraphicsSystem final : public rex::system::IGraphicsSystem
    {
    public:
      X_STATUS SetupPresentation(rex::ui::WindowedAppContext *) override
      {
        return X_STATUS_SUCCESS;
      }

      X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher *dispatcher,
                             rex::system::KernelState *kernel_state) override
      {
        dispatcher_ = dispatcher;
        kernel_state_ = kernel_state;
        memory_ = dispatcher->memory();
        shaders_.dump_ucode = REXCVAR_GET(ao2_native_gpu_dump_shaders);
        shaders_.override_dir = REXCVAR_GET(ao2_native_gpu_shader_override_dir);
        pm4_.fake_occlusion_samples = uint32_t(REXCVAR_GET(ao2_native_gpu_occlusion_samples));
        write_ptr_event_ = rex::thread::Event::CreateAutoResetEvent(false);

        pm4_.on_draw = [this](const DrawPacket &d) {
          ++frame_draws_;
          if (first_draw_pending_ && ao2::spikes::Enabled())
          {
            ao2::spikes::Mark(ao2::spikes::kGpuFirstDraw);
          }
          first_draw_pending_ = false;
          ScopedTime t(draw_ns_);
          RendererDraw(pm4_, d, shaders_);
        };
        pm4_.on_resolve = [this](const DrawPacket &d) {
          ++frame_resolves_;
          ScopedTime t(draw_ns_);
          RendererResolve(pm4_, d);
        };
        pm4_.on_swap = [this](uint32_t frontbuffer, uint32_t width, uint32_t height) {
          uint32_t gamma_version = pm4_.gamma_version.load();
          if (gamma_version != presented_gamma_version_)
          {
            presented_gamma_version_ = gamma_version;
            RendererSetGammaRamp(pm4_.gamma_table, pm4_.gamma_pwl, pm4_.ReadRegister(rex::graphics::XE_GPU_REG_DC_LUTA_CONTROL));
          }
          {
            ScopedTime t(swap_ns_);
            RendererSwap(frontbuffer, width, height);
          }
          ++swaps_;
          first_draw_pending_ = true;
          EndFrameStats();
        };
        pm4_.on_interrupt = [this](uint32_t cpu_mask) {
          for (uint32_t n = 0; n < 6; ++n)
          {
            if (cpu_mask & (1u << n))
            {
              Dispatch(1, n);
            }
          }
        };
        pm4_.keep_waiting = [this]() {
          // The guest may wait for occlusion results while the ring waits for it.
          RendererIdle();
          return running_.load(std::memory_order_relaxed);
        };
        if (REXCVAR_GET(ao2_native_gpu_occlusion_queries))
        {
          pm4_.on_occlusion = [this](uint32_t address, bool end) {
            return RendererOcclusionEvent(address, end, pm4_.fake_occlusion_samples);
          };
        }

        memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
                                       reinterpret_cast<rex::runtime::MMIOReadCallback>(ReadThunk),
                                       reinterpret_cast<rex::runtime::MMIOWriteCallback>(WriteThunk));

        running_ = true;
        command_thread_ = rex::system::object_ref<rex::system::XHostThread>(
            new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
              CommandThreadMain();
              return 0;
            }));
        command_thread_->set_name("Native GPU commands");
        command_thread_->Create();

        vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(
            new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
              VblankThreadMain();
              return 0;
            }));
        vblank_thread_->set_name("Native GPU vblank");
        vblank_thread_->Create();
        NativeLog("native graphics system: guest GPU interface ready\n");
        return X_STATUS_SUCCESS;
      }

      bool has_presentation() const override { return true; }

      void SetInterruptCallback(uint32_t callback, uint32_t user_data) override
      {
        interrupt_callback_data_ = user_data;
        interrupt_callback_ = callback;
        NativeLog("interrupt callback %08X data %08X\n", callback, user_data);
      }

      void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override
      {
        ring_base_ = ptr & 0x1FFFFFFF;
        read_index_ = 0;
        ring_dwords_ = (uint32_t(1) << (size_log2 + 3)) / 4;
        NativeLog("ring buffer %08X, %u dwords\n", ring_base_.load(), ring_dwords_.load());
      }

      void EnableReadPointerWriteBack(uint32_t ptr, uint32_t) override
      {
        read_writeback_ = ptr;
      }

      void Shutdown() override
      {
        running_ = false;
        if (write_ptr_event_)
        {
          write_ptr_event_->Set();
        }
        for (auto *thread : {&command_thread_, &vblank_thread_})
        {
          if (*thread)
          {
            (*thread)->Wait(0, 0, 0, nullptr);
            thread->reset();
          }
        }
      }

    private:
      static uint32_t ReadThunk(void *, NativeGraphicsSystem *self, uint32_t addr)
      {
        return self->ReadRegister(addr);
      }
      static void WriteThunk(void *, NativeGraphicsSystem *self, uint32_t addr, uint32_t value)
      {
        self->WriteRegister(addr, value);
      }

      uint32_t ReadRegister(uint32_t addr)
      {
        uint32_t r = (addr & 0xFFFF) / 4;
        switch (r)
        {
        case 0x0F00: // RB_EDRAM_TIMING
          return 0x08100748;
        case 0x0F01: // RB_BC_CONTROL
          return 0x0000200E;
        case 0x194C: // D1MODE_V_COUNTER
          return 720;
        case 0x1951: // interrupt status: vblank
          return 1;
        case 0x1961: // D1MODE_VIEWPORT_SIZE
          return (ao2::aspect::GuestWidth() << 16) | ao2::aspect::GuestHeight();
        default:
          break;
        }
        return pm4_.ReadRegister(r);
      }

      void WriteRegister(uint32_t addr, uint32_t value)
      {
        uint32_t r = (addr & 0xFFFF) / 4;
        if (r == 0x01C5) // CP_RB_WPTR
        {
          write_index_ = value;
          write_ptr_event_->Set();
          return;
        }
        pm4_.WriteRegister(r, value);
      }

      bool HasCommands(uint32_t write_index) const
      {
        uint32_t ring_dwords = ring_dwords_;
        return write_index != kNoWritePointer && ring_dwords && write_index % ring_dwords != read_index_;
      }

      void CommandThreadMain()
      {
        while (running_)
        {
          uint32_t write_index = write_index_;
          if (!HasCommands(write_index))
          {
            // Spin briefly (new commands usually follow quickly), then sleep on the event.
            ScopedTime idle(idle_ns_);
            struct SpikeMark
            {
              std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
              ~SpikeMark()
              {
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (ms > 0.2 && ao2::spikes::Enabled())
                {
                  ao2::spikes::Mark(ao2::spikes::kGpuIdle, ms);
                }
              }
            } spike_mark;
            for (uint32_t spins = 0; running_; ++spins)
            {
              if (spins > 500)
              {
                // A guest thread may be waiting for occlusion results: submit
                // and report them, polling the GPU more often while some are pending.
                bool results_pending = RendererIdle();
                rex::thread::Wait(write_ptr_event_.get(), false,
                                  std::chrono::milliseconds(results_pending ? 1 : 5));
              }
              else
              {
                rex::thread::MaybeYield();
              }
              write_index = write_index_;
              if (HasCommands(write_index))
              {
                break;
              }
            }
            continue;
          }
          ExecuteRing(write_index % ring_dwords_);
        }
      }

      // Executes the ring from the read pointer up to `write_index` (dwords).
      void ExecuteRing(uint32_t write_index)
      {
        uint32_t ring_dwords = ring_dwords_;
        auto *ring = reinterpret_cast<const uint32_t *>(memory_->TranslatePhysical(ring_base_));
        if (write_index >= read_index_)
        {
          pm4_.Consume(ring + read_index_, write_index - read_index_);
        }
        else
        {
          // Wrapped: packets may straddle the end, so execute a contiguous copy.
          scratch_.assign(ring + read_index_, ring + ring_dwords);
          scratch_.insert(scratch_.end(), ring, ring + write_index);
          pm4_.Consume(scratch_.data(), uint32_t(scratch_.size()));
        }
        read_index_ = write_index;
        if (uint32_t writeback = read_writeback_)
        {
          uint32_t be = __builtin_bswap32(write_index);
          std::memcpy(memory_->TranslatePhysical(writeback), &be, 4);
        }
      }

      // Runs the guest interrupt callback on the calling guest-visible host thread.
      void Dispatch(uint32_t source, uint32_t cpu)
      {
        uint32_t callback = interrupt_callback_;
        if (!callback)
        {
          return;
        }
        auto *thread = rex::system::XThread::GetCurrentThread();
        thread->SetActiveCpu(uint8_t(cpu));
        uint64_t args[] = {source, interrupt_callback_data_};
        dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
      }

      void VblankThreadMain()
      {
        using clock = std::chrono::steady_clock;
        const int64_t hz = std::clamp<int64_t>(REXCVAR_GET(ao2_native_gpu_vblank_hz), 30, 240);
        const auto interval = std::chrono::nanoseconds(1000000000ll / hz);
        auto next = clock::now() + interval;
        uint32_t vblanks = 0;
        while (running_)
        {
          auto now = clock::now();
          if (now >= next)
          {
            ++pm4_.counter;
            if (++vblanks % uint32_t(hz * 10) == 0) // every 10 s (pm4_.counter also counts swaps)
            {
              auto ms = [](std::atomic<uint64_t> &ns) { return unsigned(ns.exchange(0) / 1000000); };
              for (const auto &w : pm4_.wait_stats)
              {
                if (w.count)
                {
                  NativeLog("  waits on %s %08X (info %X ref %08X): %llu, %llu ms\n", (w.info & 0x10) ? "mem" : "reg",
                            w.poll, w.info, w.ref, (unsigned long long)w.count,
                            (unsigned long long)(w.ns / 1000000));
                }
              }
              pm4_.wait_stats_reset = true;
              NativeLog("heartbeat: vblanks=%u swaps=%llu rptr=%u wptr=%u | last 10s ms: draw %u swap %u "
                        "wait_reg_mem %u idle %u\n",
                        vblanks, (unsigned long long)swaps_.load(), read_index_.load(),
                        write_index_.load(), ms(draw_ns_), ms(swap_ns_), ms(pm4_.wait_ns), ms(idle_ns_));
              char dump[64];
              if (ao2::hang::OnHeartbeat(swaps_.load(), dump, sizeof(dump)))
              {
                NativeLog("hang: no frame for 30 s, thread stacks written to %s\n%s", dump,
                          ao2::hang::GuestStacks().c_str());
              }
            }
            Dispatch(0, 2);
            next += interval;
            if (now > next + interval * 4)
            {
              next = now + interval;
            }
          }
          rex::thread::Sleep(std::chrono::milliseconds(1));
        }
      }

      void EndFrameStats()
      {
        ++frame_;
        int every = REXCVAR_GET(ao2_native_gpu_stats_every);
        if (every > 0 && frame_ % uint64_t(every) == 0)
        {
          NativeLog("frame %llu: draws=%llu resolves=%llu shaders_ok=%u (disk %u) failed=%u pending=%u "
                    "unknown_ops=%llu parse_errors=%llu waits=%llu\n",
                    (unsigned long long)frame_, (unsigned long long)frame_draws_,
                    (unsigned long long)frame_resolves_, shaders_.compiled_count(), shaders_.disk_hits(),
                    shaders_.failed_count(), shaders_.pending_count(), (unsigned long long)pm4_.unknown_opcodes,
                    (unsigned long long)pm4_.parse_errors, (unsigned long long)pm4_.waits);
        }
        frame_draws_ = frame_resolves_ = 0;
      }

      rex::runtime::FunctionDispatcher *dispatcher_ = nullptr;
      rex::system::KernelState *kernel_state_ = nullptr;
      rex::memory::Memory *memory_ = nullptr;

      // Command processor state (command thread only, except the atomics).
      Pm4Consumer pm4_;
      ShaderCache shaders_;
      std::vector<uint32_t> scratch_;
      std::atomic<uint32_t> ring_base_{0}, ring_dwords_{0}, read_writeback_{0};
      std::atomic<uint32_t> write_index_{kNoWritePointer};
      std::atomic<uint32_t> read_index_{0};
      std::atomic<uint64_t> swaps_{0};
      std::atomic<uint64_t> draw_ns_{0}, swap_ns_{0}, idle_ns_{0};
      std::unique_ptr<rex::thread::Event> write_ptr_event_;

      std::atomic<uint32_t> interrupt_callback_{0}, interrupt_callback_data_{0};
      std::atomic<bool> running_{false};
      rex::system::object_ref<rex::system::XHostThread> command_thread_;
      rex::system::object_ref<rex::system::XHostThread> vblank_thread_;

      uint64_t frame_ = 0, frame_draws_ = 0, frame_resolves_ = 0;
      bool first_draw_pending_ = true; // command thread: no draw since the last swap
      uint32_t presented_gamma_version_ = UINT32_MAX;
    };
  } // namespace

  std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeGraphicsSystem()
  {
    return std::make_unique<NativeGraphicsSystem>();
  }

  void SetPresentWindow(void *native_window_handle)
  {
    g_present_window = native_window_handle;
  }

  void *PresentWindow()
  {
    return g_present_window;
  }

  void NativeLog(const char *format, ...)
  {
    std::lock_guard lock(g_log_mutex);
    if (!g_log)
    {
      g_log = ao2::OpenLogFile("native_gpu.log");
      if (!g_log)
      {
        return;
      }
    }
    va_list args;
    va_start(args, format);
    std::vfprintf(g_log, format, args);
    va_end(args);
    std::fflush(g_log);
  }
} // namespace ao2::gpu
