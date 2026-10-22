// shader_cache.cpp - see shader_cache.h.
#include "shader_cache.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include <d3dcompiler.h>
#include <fmt/format.h>

#include <rex/hash.h>
#include <rex/string/buffer.h>

namespace ao2::gpu
{
  using namespace rex::graphics;

  namespace
  {
    bool ReadFile(const std::string &path, std::vector<uint8_t> &out)
    {
      FILE *f = std::fopen(path.c_str(), "rb");
      if (!f)
      {
        return false;
      }
      std::fseek(f, 0, SEEK_END);
      long size = std::ftell(f);
      std::fseek(f, 0, SEEK_SET);
      out.resize(size > 0 ? size_t(size) : 0);
      bool ok = size > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
      std::fclose(f);
      return ok;
    }

    void WriteFile(const std::string &path, const void *data, size_t size)
    {
      // Write to a temporary name first so a crash never leaves a truncated entry.
      std::string temp = path + ".tmp";
      if (FILE *f = std::fopen(temp.c_str(), "wb"))
      {
        bool ok = std::fwrite(data, 1, size, f) == size;
        std::fclose(f);
        std::error_code ec;
        if (ok)
        {
          std::filesystem::rename(temp, path, ec);
        }
        else
        {
          std::filesystem::remove(temp, ec);
        }
      }
    }
  } // namespace

  ShaderCache::ShaderCache()
  {
    uint32_t threads = std::clamp(std::thread::hardware_concurrency() / 4, 2u, 6u);
    for (uint32_t i = 0; i < threads; ++i)
    {
      workers_.emplace_back([this] { WorkerMain(); });
    }
  }

  ShaderCache::~ShaderCache()
  {
    {
      std::lock_guard lock(jobs_mutex_);
      stop_ = true;
    }
    jobs_cv_.notify_all();
    for (auto &t : workers_)
    {
      t.join();
    }
  }

  void ShaderCache::WorkerMain()
  {
    for (;;)
    {
      Job job;
      {
        std::unique_lock lock(jobs_mutex_);
        jobs_cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
        if (jobs_.empty())
        {
          return;
        }
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      Compile(job);
      {
        std::lock_guard lock(jobs_mutex_);
        --pending_count_;
      }
      idle_cv_.notify_all();
    }
  }

  void ShaderCache::WaitIdle()
  {
    std::unique_lock lock(jobs_mutex_);
    idle_cv_.wait(lock, [this] { return pending_count_ == 0; });
  }

  void ShaderCache::Compile(Job &job)
  {
    CompiledShader &out = *job.out;
    ID3DBlob *code = nullptr;
    ID3DBlob *errors = nullptr;
    HRESULT hr = D3DCompile(job.hlsl.data(), job.hlsl.size(), nullptr, nullptr, nullptr, "main",
                            job.type == xenos::ShaderType::kVertex ? "vs_5_1" : "ps_5_1",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (SUCCEEDED(hr) && code)
    {
      auto *p = static_cast<const uint8_t *>(code->GetBufferPointer());
      out.bytecode.assign(p, p + code->GetBufferSize());
      if (!job.cache_path.empty())
      {
        WriteFile(job.cache_path, out.bytecode.data(), out.bytecode.size());
      }
    }
    if (errors)
    {
      out.error.append(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize());
      errors->Release();
    }
    if (code)
    {
      code->Release();
    }

    bool ok = !out.bytecode.empty();
    ok ? ++compiled_count_ : ++failed_count_;
    if (!ok || dump_all)
    {
      const char *stage = job.type == xenos::ShaderType::kVertex ? "vs" : "ps";
      std::error_code ec;
      std::filesystem::create_directories(dump_dir, ec);
      std::string base = fmt::format("{}/{:016X}_{:X}.{}", dump_dir, job.hash, out.modification, stage);
      WriteFile(base + ".hlsl", job.hlsl.data(), job.hlsl.size());
      std::string text = out.shader->ucode_disassembly() + "\n--- errors ---\n" + out.error;
      WriteFile(base + ".txt", text.data(), text.size());
    }
    out.ready.store(true, std::memory_order_release);
  }

  Shader *ShaderCache::GetShader(xenos::ShaderType type, const uint32_t *dwords, uint32_t dword_count,
                                 uint64_t hash)
  {
    if (!dwords || !dword_count)
    {
      return nullptr;
    }
    uint64_t key = hash ^ (uint64_t(type) << 63);
    auto it = shaders_.find(key);
    if (it != shaders_.end())
    {
      return it->second.get();
    }
    auto shader = std::make_unique<Shader>(type, hash, dwords, dword_count, std::endian::big);
    rex::string::StringBuffer disasm;
    shader->AnalyzeUcode(disasm);
    Shader *result = shader.get();
    shaders_.emplace(key, std::move(shader));
    return result;
  }

  const CompiledShader *ShaderCache::Get(xenos::ShaderType type, const uint32_t *dwords, uint32_t dword_count,
                                         uint64_t hash, uint64_t modification)
  {
    Shader *shader = GetShader(type, dwords, dword_count, hash);
    if (!shader)
    {
      return nullptr;
    }
    uint64_t key = (hash ^ (uint64_t(type) << 63)) * 0x9E3779B97F4A7C15ull ^ modification;
    auto it = compiled_.find(key);
    if (it != compiled_.end())
    {
      return it->second.get();
    }
    auto owned = std::make_unique<CompiledShader>();
    CompiledShader *out = owned.get();
    compiled_.emplace(key, std::move(owned));
    out->shader = shader;
    out->modification = modification;

    const char *stage = type == xenos::ShaderType::kVertex ? "vs" : "ps";
    if (dump_ucode)
    {
      std::error_code ec;
      std::filesystem::create_directories(dump_dir, ec);
      std::string name = fmt::format("{}/{:016X}_{:X}.{}.bin", dump_dir, hash, modification, stage);
      WriteFile(name, dwords, size_t(dword_count) * 4);
    }

    Shader::Translation *translation = shader->GetOrCreateTranslation(modification);
    bool translated = translator_.Translate(*translation);
    if (type == xenos::ShaderType::kVertex)
    {
      out->position_passthrough = translator_.position_passthrough();
    }
    for (const auto &e : translation->errors())
    {
      out->error += e.message;
      out->error += "\n";
    }
    Job job{out, type, hash, translation->GetTranslatedBinaryString(), {}};
    if (!override_dir.empty() && translated)
    {
      std::vector<uint8_t> text;
      if (ReadFile(fmt::format("{}/{:016X}_{:X}.{}.hlsl", override_dir, hash, modification, stage), text))
      {
        job.hlsl.assign(text.begin(), text.end());
      }
    }
    if (dump_ucode)
    {
      // Also when the DXBC comes from the disk cache and Compile never runs.
      std::string base = fmt::format("{}/{:016X}_{:X}.{}", dump_dir, hash, modification, stage);
      WriteFile(base + ".hlsl", job.hlsl.data(), job.hlsl.size());
      std::string text = shader->ucode_disassembly();
      WriteFile(base + ".txt", text.data(), text.size());
    }
    if (!translated)
    {
      ++failed_count_;
      out->ready.store(true, std::memory_order_release);
      return out;
    }

    if (!disk_cache_dir.empty())
    {
      job.cache_path = fmt::format("{}/{:016X}.{}.dxbc", disk_cache_dir,
                                   XXH3_64bits(job.hlsl.data(), job.hlsl.size()), stage);
      if (ReadFile(job.cache_path, out->bytecode))
      {
        ++disk_hits_;
        ++compiled_count_;
        out->ready.store(true, std::memory_order_release);
        return out;
      }
      out->bytecode.clear();
      std::error_code ec;
      std::filesystem::create_directories(disk_cache_dir, ec);
    }

    if (!async)
    {
      Compile(job);
      return out;
    }
    {
      std::lock_guard lock(jobs_mutex_);
      ++pending_count_;
      jobs_.push_back(std::move(job));
    }
    jobs_cv_.notify_one();
    return out;
  }
} // namespace ao2::gpu
