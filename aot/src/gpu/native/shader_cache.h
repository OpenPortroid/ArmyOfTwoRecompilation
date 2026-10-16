// shader_cache.h - Xenos microcode -> analyzed Shader -> HLSL -> DXBC.
//
// Translation is synchronous (fast); FXC compilation runs on worker threads
// (Get returns a shader that is not ready yet, and the draw is skipped) and
// its results are kept in a disk cache keyed by the hash of the HLSL, so a
// translator change invalidates exactly the affected shaders.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/shader/shader.h>

#include "hlsl_translator.h"

namespace ao2::gpu
{
  struct CompiledShader
  {
    rex::graphics::Shader *shader = nullptr; // analysis results (bindings etc.)
    uint64_t modification = 0;
    std::vector<uint8_t> bytecode; // empty on failure; valid once ready
    std::string error;
    // Vertex shaders: fetch exported unchanged as the position, if any.
    HlslShaderTranslator::PositionFetch position_passthrough;
    std::atomic<bool> ready{false};

    bool ok() const { return ready.load(std::memory_order_acquire) && !bytecode.empty(); }
    bool pending() const { return !ready.load(std::memory_order_acquire); }
  };

  class ShaderCache
  {
  public:
    ShaderCache();
    ~ShaderCache();

    // `dwords` are big-endian (guest memory). Returns nullptr only for empty
    // input; the result may still be compiling (see CompiledShader::ready).
    const CompiledShader *Get(rex::graphics::xenos::ShaderType type, const uint32_t *dwords,
                              uint32_t dword_count, uint64_t hash, uint64_t modification);

    // Returns the analyzed shader (without translating).
    rex::graphics::Shader *GetShader(rex::graphics::xenos::ShaderType type, const uint32_t *dwords,
                                     uint32_t dword_count, uint64_t hash);

    // Blocks until all queued compilations are done.
    void WaitIdle();

    uint32_t compiled_count() const { return compiled_count_; }
    uint32_t failed_count() const { return failed_count_; }
    uint32_t disk_hits() const { return disk_hits_; }
    uint32_t pending_count() const { return pending_count_; }

    // Compile on worker threads (false: inside Get).
    bool async = true;
    // Directory of the DXBC cache (empty = disabled).
    std::string disk_cache_dir = "shader_cache";
    // Directory where failing (and, if dump_all, all) HLSL is written.
    std::string dump_dir = "shader_dump";
    bool dump_all = false;
    // Write raw big-endian microcode (<hash>_<modification>.<vs|ps>.bin) of
    // every translated variant, for tools/xenos_hlsl_test.
    bool dump_ucode = false;

  private:
    struct Job
    {
      CompiledShader *out;
      rex::graphics::xenos::ShaderType type;
      uint64_t hash;
      std::string hlsl;
      std::string cache_path;
    };

    void Compile(Job &job);
    void WorkerMain();

    std::unordered_map<uint64_t, std::unique_ptr<rex::graphics::Shader>> shaders_;
    std::unordered_map<uint64_t, std::unique_ptr<CompiledShader>> compiled_;
    HlslShaderTranslator translator_;
    std::atomic<uint32_t> compiled_count_{0}, failed_count_{0}, disk_hits_{0}, pending_count_{0};

    std::mutex jobs_mutex_;
    std::condition_variable jobs_cv_, idle_cv_;
    std::deque<Job> jobs_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
  };
} // namespace ao2::gpu
