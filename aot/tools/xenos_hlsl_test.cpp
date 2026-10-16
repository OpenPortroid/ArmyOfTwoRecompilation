// xenos_hlsl_test - offline check of the native renderer's shader translator.
//
// Usage: xenos_hlsl_test <dir> [out_dir] [--all] [--no-cache]
//   <dir> contains <hash>_<modification>.<vs|ps>.bin dumped by the game with
//   --ao2_native_gpu_dump_shaders=true.
// Every shader is translated to HLSL and compiled with FXC; failures (HLSL,
// disassembly and errors) are written to out_dir (default <dir>/failed), all
// shaders with --all. Compiled DXBC goes to the game's disk cache
// (shader_cache next to this executable) unless --no-cache, so the game
// starts without compiling.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../src/gpu/native/shader_cache.h"

int main(int argc, char **argv)
{
  std::vector<std::string> positional;
  bool all = false, use_cache = true;
  for (int i = 1; i < argc; ++i)
  {
    if (!std::strcmp(argv[i], "--all"))
    {
      all = true;
    }
    else if (!std::strcmp(argv[i], "--no-cache"))
    {
      use_cache = false;
    }
    else
    {
      positional.push_back(argv[i]);
    }
  }
  if (positional.empty())
  {
    std::printf("usage: xenos_hlsl_test <dump dir> [out dir] [--all] [--no-cache]\n");
    return 1;
  }
  std::filesystem::path dir = positional[0];
  std::filesystem::path out = positional.size() > 1 ? std::filesystem::path(positional[1]) : dir / "failed";
  std::filesystem::remove_all(out);

  ao2::gpu::ShaderCache cache;
  cache.dump_dir = out.string();
  cache.dump_all = all;
  cache.disk_cache_dir =
      use_cache ? (std::filesystem::absolute(argv[0]).parent_path() / "shader_cache").string() : std::string();

  auto start = std::chrono::steady_clock::now();
  uint32_t total = 0;
  std::vector<std::pair<std::string, const ao2::gpu::CompiledShader *>> results;
  for (auto &entry : std::filesystem::directory_iterator(dir))
  {
    std::string name = entry.path().filename().string();
    if (entry.path().extension() != ".bin")
    {
      continue;
    }
    unsigned long long hash = 0, modification = 0;
    char stage[3] = {};
    if (std::sscanf(name.c_str(), "%16llx_%llx.%2s", &hash, &modification, stage) != 3)
    {
      continue;
    }
    std::ifstream f(entry.path(), std::ios::binary);
    std::vector<uint32_t> dwords((std::filesystem::file_size(entry.path()) + 3) / 4);
    f.read(reinterpret_cast<char *>(dwords.data()), std::streamsize(dwords.size() * 4));
    auto type = stage[0] == 'v' ? rex::graphics::xenos::ShaderType::kVertex
                                : rex::graphics::xenos::ShaderType::kPixel;
    results.emplace_back(name, cache.Get(type, dwords.data(), uint32_t(dwords.size()), hash, modification));
    ++total;
  }
  cache.WaitIdle();
  for (auto &[name, s] : results)
  {
    if (s && s->bytecode.empty())
    {
      std::printf("FAIL %s\n", name.c_str());
    }
  }
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count();
  std::printf("%u shaders: %u ok (%u from disk cache), %u failed (%lld ms)\n", total, cache.compiled_count(),
              cache.disk_hits(), cache.failed_count(), static_cast<long long>(ms));
  return cache.failed_count() ? 2 : 0;
}
