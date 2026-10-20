// fast_hash.h - Hash for the GPU thread's maps keyed by addresses and hashes.
// std::hash<uint64_t> of the MSVC library is FNV-1a over the key's bytes; a
// multiply and two shifts mix the bits well enough for the power of two
// bucket counts and cost a fraction of that (the lookups run per draw).
#pragma once

#include <cstddef>
#include <cstdint>

namespace ao2::gpu
{
  struct FastHash
  {
    size_t operator()(uint64_t x) const noexcept
    {
      x ^= x >> 33;
      x *= 0xFF51AFD7ED558CCDull;
      x ^= x >> 33;
      return size_t(x);
    }
  };
} // namespace ao2::gpu
