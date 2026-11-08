// shader_capture.h - Shader microcode as the game creates its shaders.
//
// UE3 creates the D3D shaders of a package when it loads it (FXeVertexShader /
// FXePixelShader, sub_82AA5468 / sub_82AA55A8), long before their first draw.
// Both get the microcode from XGGetMicrocodeShaderParts (sub_82B163B8): the
// override here copies every distinct microcode for the GPU thread, which
// compiles the variants and pipelines the shipped manifest lists for it
// during the loading screen (renderer.cpp, "Shader manifest").
#pragma once

#include <cstdint>
#include <vector>

namespace ao2::gpu::capture
{
  struct Captured
  {
    uint64_t hash; // as the PM4 consumer computes it (HashDwords)
    // Vertex shaders: the D3D rewrites their vertex fetches for the vertex
    // declaration when it binds them (format, stride, offset...), so the
    // microcode the GPU gets differs from the created one there only. Hash
    // with the fetch instructions zeroed (FetchMaskedHash), 0 for pixel shaders.
    uint64_t masked_hash;
    std::vector<uint32_t> dwords; // big-endian, as in guest memory
  };

  // Dword indices of the vertex fetch instructions (all three dwords each),
  // from the exec clauses of the control flow. `dwords` big-endian.
  void FetchDwords(const uint32_t *dwords, uint32_t count, std::vector<uint32_t> &out);
  // HashDwords with the dwords of FetchDwords as 0.
  uint64_t FetchMaskedHash(const uint32_t *dwords, uint32_t count);

  // Moves the shaders captured since the last call into `out`.
  void Drain(std::vector<Captured> &out);

  // Number of shaders the game created so far (distinct microcode).
  uint32_t SeenCount();
} // namespace ao2::gpu::capture
