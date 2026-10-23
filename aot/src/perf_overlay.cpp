// perf_overlay.cpp - The frame rate overlay: an ImGui window in the corner of
// the native renderer's picture (drawn on its own overlay layer, without
// input, by debug_menu.cpp). F2 (ao2_fps_overlay_key) and the F1 menu switch
// it between off, the frame rate and the detailed view:
//
// - FPS: frames presented in the last second; "1%": the 1% low (the 99th
//   percentile frame time of the last 1024 frames).
// - Frame time: average and longest of the last second, and a graph of the
//   last frames (the renderer's present times, OnPresent).
// - GPU load: as Task Manager has it - the busiest engine of the renderer's
//   adapter, all processes summed (PDH "GPU Engine" counters).
// - GPU temperature: the adapter's perf data (D3DKMT, also Task Manager's
//   source; drivers that report none show a dash).
// - Video memory: the adapter's dedicated memory in use (PDH "GPU Adapter
//   Memory") of its total, and the game's own (DXGI QueryVideoMemoryInfo).
// - CPU load: all cores, and the game's share of them (GetSystemTimes,
//   GetProcessTimes); RAM: in use of total, and the game's working set.
//
// The system figures come from a thread of their own, once a second, only
// while the detailed view is shown (PDH collection takes milliseconds).
#include "perf_overlay.h"

#include "frame_spikes.h"
#include "ui_text.h"
#include "ui_theme.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h> // NTSTATUS for d3dkmthk.h
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(ao2_fps_overlay, 1, "GPU",
                     "Frame rate overlay: 0 = off, 1 = FPS, 2 = FPS, frame times, GPU/CPU load, GPU temperature, "
                     "video and system memory");

namespace ao2::perf
{
  namespace
  {
    using Microsoft::WRL::ComPtr;

    constexpr size_t kFrames = 1024;   // present times kept (the 1% low's window)
    constexpr int kGraphFrames = 160;  // frame times in the graph
    constexpr int64_t kSecond = 1000000000;

    // Unknown figures are negative.
    struct SystemStats
    {
      float cpu = -1.0f, cpu_game = -1.0f;
      float gpu = -1.0f, gpu_temperature = -1.0f;
      double ram_used = -1.0, ram_total = -1.0, ram_game = -1.0; // bytes
      double vram_used = -1.0, vram_total = -1.0, vram_game = -1.0;
    };

    struct FrameStats
    {
      float fps = 0.0f, low = 0.0f, average_ms = 0.0f, max_ms = 0.0f;
    };

    // Never destroyed: the sampler thread outlives static destruction.
    struct State
    {
      std::mutex frames_mutex;
      std::array<int64_t, kFrames> presents{}; // steady clock, ns
      uint64_t present_count = 0;
      std::condition_variable presented;

      std::mutex system_mutex;
      SystemStats system;
      std::atomic<int64_t> sample_until{0}; // the sampler runs until then
      std::once_flag sampler_started;

      std::atomic<uint64_t> luid{0};
      std::atomic<bool> luid_set{false};

      // UI thread.
      FrameStats shown;
      int64_t shown_time = 0;
      float graph[kGraphFrames] = {};
      int graph_count = 0;
    };

    State &S()
    {
      static State *state = new State;
      return *state;
    }

    // Until the next present (at most 50 ms: none while loading).
    void WaitForPresent()
    {
      State &s = S();
      std::unique_lock lock(s.frames_mutex);
      uint64_t count = s.present_count;
      s.presented.wait_for(lock, std::chrono::milliseconds(50), [&] { return s.present_count != count; });
    }

    int64_t NowNs()
    {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now().time_since_epoch())
          .count();
    }

    uint64_t FileTime(const FILETIME &t)
    {
      return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    }

    std::wstring Lower(std::wstring s)
    {
      for (wchar_t &c : s)
      {
        c = wchar_t(std::towlower(c));
      }
      return s;
    }

    // PDH counter values of all instances whose name contains `tag`
    // (lowercase), by instance name.
    std::vector<std::pair<std::wstring, double>> CounterValues(PDH_HCOUNTER counter, const std::wstring &tag)
    {
      std::vector<std::pair<std::wstring, double>> values;
      DWORD size = 0, count = 0;
      if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr) !=
              PDH_MORE_DATA ||
          !size)
      {
        return values;
      }
      std::vector<uint8_t> buffer(size);
      auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
      if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) !=
          ERROR_SUCCESS)
      {
        return values;
      }
      for (DWORD i = 0; i < count; ++i)
      {
        if (!items[i].szName || (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                                 items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA))
        {
          continue;
        }
        std::wstring name = Lower(items[i].szName);
        if (name.find(tag) != std::wstring::npos)
        {
          values.emplace_back(std::move(name), items[i].FmtValue.doubleValue);
        }
      }
      return values;
    }

    // NVIDIA's management library (nvml.dll, with the driver), loaded at run time.
    class Nvml
    {
    public:
      // The device on PCI bus/device/function; false without NVML.
      bool Open(uint32_t bus, uint32_t device, uint32_t function)
      {
        HMODULE dll = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll)
        {
          return false;
        }
        auto init = reinterpret_cast<int (*)()>(GetProcAddress(dll, "nvmlInit_v2"));
        auto by_bus = reinterpret_cast<int (*)(const char *, void **)>(
            GetProcAddress(dll, "nvmlDeviceGetHandleByPciBusId_v2"));
        temperature_ = reinterpret_cast<int (*)(void *, int, unsigned *)>(
            GetProcAddress(dll, "nvmlDeviceGetTemperature"));
        char bus_id[32];
        std::snprintf(bus_id, sizeof(bus_id), "00000000:%02X:%02X.%X", bus, device, function);
        if (!init || !by_bus || !temperature_ || init() != 0 || by_bus(bus_id, &device_) != 0)
        {
          device_ = nullptr;
          return false;
        }
        return true;
      }

      // The core temperature in degrees, or 0.
      unsigned Temperature() const
      {
        unsigned t = 0;
        return device_ && temperature_(device_, 0 /* NVML_TEMPERATURE_GPU */, &t) == 0 ? t : 0;
      }

    private:
      void *device_ = nullptr;
      int (*temperature_)(void *, int, unsigned *) = nullptr;
    };

    // The renderer's adapter: load and dedicated memory (PDH), temperature
    // (NVML, else D3DKMT), the game's video memory and the total (DXGI).
    //
    // The temperature takes 4-8 ms in the driver on an RTX 2060, through
    // NVML as through D3DKMT's adapter performance data, and a present
    // issued meanwhile waits as long: with the detailed overlay a long frame
    // now and then, in step with the samples (frame_spikes.log). It is asked
    // only every kTemperatureSamples samples, right after a present, so that
    // it is over before the next one at up to ~120 FPS.
    class GpuSampler
    {
    public:
      void Sample(SystemStats &s)
      {
        if (!opened_ && !Open())
        {
          return;
        }
        auto t0 = std::chrono::steady_clock::now();
        auto mark = [&](ao2::spikes::Event event) {
          auto t1 = std::chrono::steady_clock::now();
          if (ao2::spikes::Enabled())
          {
            ao2::spikes::Mark(event, std::chrono::duration<double, std::milli>(t1 - t0).count());
          }
          t0 = t1;
        };
        if (query_ && PdhCollectQueryData(query_) == ERROR_SUCCESS)
        {
          // Instances: pid_<pid>_luid_0x<high>_0x<low>_phys_<n>_eng_<n>_engtype_<type>.
          std::map<std::wstring, double> engines;
          for (const auto &[name, value] : CounterValues(engines_, tag_))
          {
            engines[name.substr(name.find(tag_) + tag_.size())] += value;
          }
          if (!engines.empty())
          {
            double busiest = 0.0;
            for (const auto &[engine, value] : engines)
            {
              busiest = std::max(busiest, value);
            }
            s.gpu = float(std::clamp(busiest, 0.0, 100.0));
          }
          // Instances: luid_0x<high>_0x<low>_phys_<n>.
          auto memory = CounterValues(memory_, tag_);
          if (!memory.empty())
          {
            s.vram_used = 0.0;
            for (const auto &[name, value] : memory)
            {
              s.vram_used += value;
            }
          }
        }
        mark(ao2::spikes::kSamplePdh);
        if ((nvml_open_ || kmt_adapter_) && temperature_samples_++ % kTemperatureSamples == 0)
        {
          WaitForPresent();
          t0 = std::chrono::steady_clock::now();
          if (nvml_open_)
          {
            if (unsigned t = nvml_.Temperature())
            {
              temperature_ = float(t);
            }
          }
          else
          {
            D3DKMT_ADAPTER_PERFDATA data = {};
            D3DKMT_QUERYADAPTERINFO query = {};
            query.hAdapter = kmt_adapter_;
            query.Type = KMTQAITYPE_ADAPTERPERFDATA;
            query.pPrivateDriverData = &data;
            query.PrivateDriverDataSize = sizeof(data);
            if (D3DKMTQueryAdapterInfo(&query) >= 0 && data.Temperature > 0)
            {
              temperature_ = float(data.Temperature) / 10.0f;
            }
          }
          mark(ao2::spikes::kSampleTemperature);
        }
        s.gpu_temperature = temperature_;
        if (adapter_)
        {
          DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
          if (SUCCEEDED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
          {
            s.vram_game = double(info.CurrentUsage);
          }
          s.vram_total = vram_total_;
          mark(ao2::spikes::kSampleDxgi);
        }
      }

    private:
      bool Open()
      {
        if (!S().luid_set.load(std::memory_order_acquire))
        {
          return false;
        }
        opened_ = true;
        uint64_t packed = S().luid.load(std::memory_order_relaxed);
        LUID luid;
        luid.LowPart = DWORD(packed);
        luid.HighPart = LONG(packed >> 32);
        wchar_t tag[64];
        swprintf(tag, 64, L"luid_0x%08x_0x%08x", unsigned(luid.HighPart), unsigned(luid.LowPart));
        tag_ = tag;

        // English counter names: the localized ones differ per Windows language.
        if (PdhOpenQueryW(nullptr, 0, &query_) == ERROR_SUCCESS)
        {
          if (PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engines_) !=
                  ERROR_SUCCESS ||
              PdhAddEnglishCounterW(query_, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &memory_) !=
                  ERROR_SUCCESS)
          {
            PdhCloseQuery(query_);
            query_ = nullptr;
          }
          else
          {
            // Utilization is a rate: the first collection is the baseline.
            PdhCollectQueryData(query_);
          }
        }

        D3DKMT_OPENADAPTERFROMLUID open = {};
        open.AdapterLuid = luid;
        if (D3DKMTOpenAdapterFromLuid(&open) >= 0)
        {
          kmt_adapter_ = open.hAdapter;
          D3DKMT_ADAPTERADDRESS address = {};
          D3DKMT_QUERYADAPTERINFO query = {};
          query.hAdapter = kmt_adapter_;
          query.Type = KMTQAITYPE_ADAPTERADDRESS;
          query.pPrivateDriverData = &address;
          query.PrivateDriverDataSize = sizeof(address);
          nvml_open_ = D3DKMTQueryAdapterInfo(&query) >= 0 &&
                       nvml_.Open(address.BusNumber, address.DeviceNumber, address.FunctionNumber);
        }

        ComPtr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter_))))
        {
          DXGI_ADAPTER_DESC1 desc = {};
          adapter_->GetDesc1(&desc);
          vram_total_ = double(desc.DedicatedVideoMemory);
        }
        return true;
      }

      bool opened_ = false;
      std::wstring tag_;
      PDH_HQUERY query_ = nullptr;
      PDH_HCOUNTER engines_ = nullptr, memory_ = nullptr;
      D3DKMT_HANDLE kmt_adapter_ = 0;
      static constexpr uint32_t kTemperatureSamples = 5;
      uint32_t temperature_samples_ = 0;
      float temperature_ = 0.0f;
      Nvml nvml_;
      bool nvml_open_ = false;
      ComPtr<IDXGIAdapter3> adapter_;
      double vram_total_ = -1.0;
    };

    // CPU load since the last sample; RAM.
    class CpuSampler
    {
    public:
      void Sample(SystemStats &s, int64_t now)
      {
        FILETIME idle, kernel, user, created, exited, process_kernel, process_user;
        if (GetSystemTimes(&idle, &kernel, &user) &&
            GetProcessTimes(GetCurrentProcess(), &created, &exited, &process_kernel, &process_user))
        {
          // Kernel time includes the idle time; all in 100 ns units.
          uint64_t idle_t = FileTime(idle), total_t = FileTime(kernel) + FileTime(user);
          uint64_t process_t = FileTime(process_kernel) + FileTime(process_user);
          if (valid_ && total_t > total_)
          {
            double total = double(total_t - total_);
            s.cpu = float(std::clamp(100.0 * (1.0 - double(idle_t - idle_) / total), 0.0, 100.0));
            double wall = double(now - time_) / 100.0 * double(processors_);
            if (wall > 0.0)
            {
              s.cpu_game = float(std::clamp(100.0 * double(process_t - process_) / wall, 0.0, 100.0));
            }
          }
          idle_ = idle_t;
          total_ = total_t;
          process_ = process_t;
          time_ = now;
          valid_ = true;
        }

        MEMORYSTATUSEX memory = {};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory))
        {
          s.ram_total = double(memory.ullTotalPhys);
          s.ram_used = double(memory.ullTotalPhys - memory.ullAvailPhys);
        }
        PROCESS_MEMORY_COUNTERS counters = {};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        {
          s.ram_game = double(counters.WorkingSetSize);
        }
      }

      // The next load is measured from the next sample.
      void Reset() { valid_ = false; }

    private:
      bool valid_ = false;
      uint64_t idle_ = 0, total_ = 0, process_ = 0;
      int64_t time_ = 0;
      DWORD processors_ = std::max<DWORD>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 1);
    };

    void SamplerThread()
    {
      SetThreadDescription(GetCurrentThread(), L"ao2 perf overlay");
      GpuSampler gpu;
      CpuSampler cpu;
      bool active = false;
      for (;;)
      {
        int64_t now = NowNs();
        if (now > S().sample_until.load(std::memory_order_relaxed))
        {
          if (active)
          {
            active = false;
            cpu.Reset();
            std::lock_guard lock(S().system_mutex);
            S().system = {};
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
          continue;
        }
        active = true;
        SystemStats stats;
        cpu.Sample(stats, now);
        gpu.Sample(stats);
        {
          std::lock_guard lock(S().system_mutex);
          S().system = stats;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }

    // UI thread: the shown figures (twice a second) and the graph (every frame).
    void UpdateFrameStats(int64_t now)
    {
      State &s = S();
      std::vector<int64_t> presents;
      {
        std::lock_guard lock(s.frames_mutex);
        size_t count = size_t(std::min<uint64_t>(s.present_count, kFrames));
        presents.reserve(count);
        for (uint64_t i = s.present_count - count; i < s.present_count; ++i)
        {
          presents.push_back(s.presents[i % kFrames]);
        }
      }
      std::vector<float> intervals; // ms, oldest first
      for (size_t i = 1; i < presents.size(); ++i)
      {
        intervals.push_back(float(double(presents[i] - presents[i - 1]) / 1e6));
      }

      s.graph_count = std::min<int>(int(intervals.size()), kGraphFrames);
      std::copy(intervals.end() - s.graph_count, intervals.end(), s.graph);

      if (now - s.shown_time < kSecond / 2)
      {
        return;
      }
      s.shown_time = now;
      FrameStats f;
      // The last second's frames (none when presents stopped, e.g. loading).
      double sum = 0.0;
      int frames = 0;
      for (size_t i = intervals.size(); i-- > 0;)
      {
        if (presents[i + 1] < now - kSecond)
        {
          break;
        }
        sum += intervals[i];
        f.max_ms = std::max(f.max_ms, intervals[i]);
        ++frames;
      }
      if (frames > 0 && sum > 0.0)
      {
        f.average_ms = float(sum / frames);
        f.fps = float(1000.0 * frames / sum);
        std::vector<float> sorted = intervals;
        size_t at = std::min(sorted.size() - 1, size_t(double(sorted.size()) * 0.99));
        std::nth_element(sorted.begin(), sorted.begin() + at, sorted.end());
        f.low = sorted[at] > 0.0f ? 1000.0f / sorted[at] : 0.0f;
      }
      s.shown = f;
    }

    const ImVec4 kLabel(0.62f, 0.68f, 0.75f, 1.0f);
    const ImVec4 kDim(0.62f, 0.62f, 0.62f, 1.0f);
    const ImVec4 kFpsColor(0.45f, 1.0f, 0.45f, 1.0f);

    float g_value_x = 0.0f;

    void Label(const char *label)
    {
      ImGui::TextColored(kLabel, "%s", label);
      ImGui::SameLine(g_value_x);
    }

    // A dim remark after the value.
    template <typename... Args> void Note(const char *format, Args... args)
    {
      ImGui::SameLine(0.0f, ImGui::CalcTextSize(" ").x * 2.0f);
      ImGui::TextColored(kDim, format, args...);
    }

    void Unknown()
    {
      ImGui::TextColored(kDim, "вЂ”");
    }

    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
  } // namespace

  int32_t GetMode()
  {
    return std::clamp(REXCVAR_GET(ao2_fps_overlay), int32_t(kOff), int32_t(kDetailed));
  }

  void SetMode(int32_t mode)
  {
    REXCVAR_SET(ao2_fps_overlay, std::clamp(mode, int32_t(kOff), int32_t(kDetailed)));
  }

  void CycleMode()
  {
    SetMode((GetMode() + 1) % (kDetailed + 1));
  }

  void SetAdapter(uint32_t luid_low, int32_t luid_high)
  {
    S().luid.store((uint64_t(uint32_t(luid_high)) << 32) | luid_low, std::memory_order_relaxed);
    S().luid_set.store(true, std::memory_order_release);
  }

  void OnPresent()
  {
    int64_t now = NowNs();
    State &s = S();
    std::lock_guard lock(s.frames_mutex);
    s.presents[s.present_count % kFrames] = now;
    ++s.present_count;
    s.presented.notify_all();
  }

  void Draw()
  {
    int32_t mode = GetMode();
    if (mode == kOff)
    {
      return;
    }
    State &s = S();
    int64_t now = NowNs();
    SystemStats system;
    if (mode == kDetailed)
    {
      s.sample_until.store(now + 3 * kSecond, std::memory_order_relaxed);
      std::call_once(s.sampler_started, [] { std::thread(SamplerThread).detach(); });
      std::lock_guard lock(s.system_mutex);
      system = s.system;
    }
    UpdateFrameStats(now);
    const FrameStats &f = s.shown;

    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##perf_overlay", nullptr, flags))
    {
      if (mode == kFps)
      {
        ImGui::TextColored(kFpsColor, "%.0f FPS", f.fps);
      }
      else
      {
        // Wide enough for the longest label ("Frame" is translated).
        std::string frame_label = std::string(ao2::text::Tr("Frame")) + "  ";
        g_value_x = ImGui::GetStyle().WindowPadding.x +
                    std::max(ImGui::CalcTextSize("VRAM  ").x, ImGui::CalcTextSize(frame_label.c_str()).x);
        Label("FPS");
        ImGui::TextColored(kFpsColor, "%5.1f", f.fps);
        Note("1%% %.1f", f.low);

        Label(ao2::text::Tr("Frame"));
        ImGui::Text(ao2::text::Tr("%5.1f ms"), f.average_ms);
        Note(ao2::text::Tr("max %.1f"), f.max_ms);

        // Spikes past twice the average stay at the top.
        float scale = std::max(f.average_ms * 2.0f, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_PlotLines, kFpsColor);
        ImGui::PlotLines("##frame_times", s.graph, s.graph_count, 0, nullptr, 0.0f, scale,
                         ImVec2(g_value_x + ImGui::CalcTextSize("000.0  1% 000.0").x, 34.0f));
        ImGui::PopStyleColor(2);

        Label("GPU");
        if (system.gpu >= 0.0f)
        {
          ImGui::Text("%3.0f%%", system.gpu);
        }
        else
        {
          Unknown();
        }
        if (system.gpu_temperature > 0.0f)
        {
          ImVec4 color = system.gpu_temperature >= 85.0f   ? ImVec4(1.0f, 0.4f, 0.35f, 1.0f)
                         : system.gpu_temperature >= 75.0f ? ImVec4(1.0f, 0.85f, 0.35f, 1.0f)
                                                           : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
          ImGui::SameLine(0.0f, ImGui::CalcTextSize(" ").x * 2.0f);
          ImGui::TextColored(color, "%.0fВ°C", system.gpu_temperature);
        }

        Label("VRAM");
        if (system.vram_used >= 0.0 && system.vram_total > 0.0)
        {
          ImGui::Text(ao2::text::Tr("%.2f / %.1f GB"), system.vram_used / kGiB, system.vram_total / kGiB);
        }
        else if (system.vram_total > 0.0)
        {
          ImGui::Text(ao2::text::Tr("? / %.1f GB"), system.vram_total / kGiB);
        }
        else
        {
          Unknown();
        }
        if (system.vram_game >= 0.0)
        {
          Note(ao2::text::Tr("game %.2f"), system.vram_game / kGiB);
        }

        Label("CPU");
        if (system.cpu >= 0.0f)
        {
          ImGui::Text("%3.0f%%", system.cpu);
          if (system.cpu_game >= 0.0f)
          {
            Note(ao2::text::Tr("game %.0f%%"), system.cpu_game);
          }
        }
        else
        {
          Unknown();
        }

        Label("RAM");
        if (system.ram_used >= 0.0 && system.ram_total > 0.0)
        {
          ImGui::Text(ao2::text::Tr("%.1f / %.1f GB"), system.ram_used / kGiB, system.ram_total / kGiB);
          if (system.ram_game >= 0.0)
          {
            Note(ao2::text::Tr("game %.2f"), system.ram_game / kGiB);
          }
        }
        else
        {
          Unknown();
        }
      }
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
  }

  void ConfigureFonts(ImFontAtlas *atlas)
  {
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF, // Basic Latin, Latin-1 (В°)
        0x0400, 0x04FF, // Cyrillic
        0x2010, 0x205E, // punctuation (вЂ”)
        0,
    };
    // Monospaced, so the figures stay put.
    for (const char *path : {"C:\\Windows\\Fonts\\consola.ttf", "C:\\Windows\\Fonts\\segoeui.ttf"})
    {
      if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
      {
        continue;
      }
      ImFontConfig config;
      config.OversampleH = 2;
      config.OversampleV = 1;
      if (ImFont *font = atlas->AddFontFromFileTTF(path, 16.0f, &config, kRanges))
      {
        ImGui::GetIO().FontDefault = font;
        break;
      }
    }
    // The notices drawn in this layer (invites, coop_invite.cpp).
    ao2::ui::AddFonts(atlas);
  }
} // namespace ao2::perf
