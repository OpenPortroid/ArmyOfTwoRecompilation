// heap_lock.h - The game's heap lock as a host lock (heap_lock.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace ao2::heap
{
  // The body of GMalloc's method at this vtable slot (r3 = the proxy, arguments as the caller set them):
  // the real allocator's method under the host lock, or the original method when the lock is off.
  // skip_null: the method does nothing for r4 = 0 (Free).
  void Method(PPCContext &ctx, uint8_t *base, uint32_t slot, bool skip_null, PPCFunc *original);
} // namespace ao2::heap
