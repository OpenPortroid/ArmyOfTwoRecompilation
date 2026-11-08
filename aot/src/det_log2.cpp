// det_log2.cpp - log2f that gives the same bits on every PC.
//
// The recompiled game turns the PPC vector instruction vlogefp (the logarithm in its power curves) into log2f calls of
// the C runtime (the import of the C runtime DLL). The runtime picks the implementation by the CPU (FMA / AVX-512 paths),
// so two PCs of a lockstep game could get results a few ulps apart and the simulation split (the look rotations of
// players differed in the last bits long before the game went out of sync). At startup this executable's import of
// log2f is pointed at the function below: only + - * / on doubles (IEEE exact), accurate to ~1e-13.
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>

#include <windows.h>

namespace
{

float DetLog2(float value)
{
  constexpr double kInvLn2 = 1.4426950408889634074;
  constexpr double kSqrtHalf = 0.70710678118654752440;
  if (value != value)
  {
    return value;
  }
  if (value < 0.0f)
  {
    return std::numeric_limits<float>::quiet_NaN();
  }
  if (value == 0.0f)
  {
    return -std::numeric_limits<float>::infinity();
  }
  if (value == std::numeric_limits<float>::infinity())
  {
    return value;
  }
  // value = m * 2^e with m in [sqrt(1/2), sqrt(2)); denormals are normalised first.
  double x = double(value);
  int e = 0;
  uint64_t bits = std::bit_cast<uint64_t>(x);
  e = int((bits >> 52) & 0x7FF) - 1023;
  bits = (bits & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;
  double m = std::bit_cast<double>(bits); // [1, 2)
  if (m > 2.0 * kSqrtHalf)
  {
    m *= 0.5;
    ++e;
  }
  double s = (m - 1.0) / (m + 1.0);
  double s2 = s * s;
  double ln = 2.0 * s * (1.0 + s2 * (1.0 / 3.0 + s2 * (1.0 / 5.0 + s2 * (1.0 / 7.0 + s2 * (1.0 / 9.0 + s2 * (1.0 / 11.0 +
                  s2 * (1.0 / 13.0 + s2 * (1.0 / 15.0 + s2 * (1.0 / 17.0)))))))));
  return float(double(e) + ln * kInvLn2);
}

  // The executable's import table entry of `name` (any DLL) to `target`.
  bool PatchImport(const char *name, void *target)
  {
    auto *base = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
    {
      return false;
    }
    bool patched = false;
    for (auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name; ++imp)
    {
      auto *thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);
      auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                                                                          : imp->FirstThunk));
      for (; names->u1.AddressOfData; ++names, ++thunk)
      {
        if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
        {
          continue;
        }
        auto *entry = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
        if (std::strcmp(entry->Name, name) != 0)
        {
          continue;
        }
        DWORD old = 0;
        if (VirtualProtect(&thunk->u1.Function, sizeof(void *), PAGE_READWRITE, &old))
        {
          thunk->u1.Function = reinterpret_cast<ULONG_PTR>(target);
          VirtualProtect(&thunk->u1.Function, sizeof(void *), old, &old);
          patched = true;
        }
      }
    }
    return patched;
  }

  // Before main: the game's code runs only after startup.
  const bool g_patched = PatchImport("log2f", reinterpret_cast<void *>(&DetLog2));
} // namespace
