// ue3_reflect.cpp - Reading UE3 objects by class and property name (see the header).
#include "ue3_reflect.h"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace
{
  constexpr uint32_t kGObjects = 0x831E4E3C;
  constexpr uint32_t kOuter = 40, kName = 44, kNameNumber = 48, kClass = 52, kSuperField = 60;
  constexpr uint32_t kPropertyOffset = 0x64;
  constexpr uint32_t kBoolMask = 0x80; // UBoolProperty::BitMask

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }

  bool Plausible(uint32_t p)
  {
    return p >= 0x40000000 && p < 0x90000000 && !(p & 3);
  }

  // Committed readable regions seen by the current lookup (a scan of every
  // object): VirtualQuery over the guest's huge address space costs enough
  // that one VirtualQuery per object made a property lookup take minutes
  // (the game thread stalled at the level start). Cleared by each top-level
  // lookup (ScanCache), so memory freed between lookups is queried again.
  thread_local std::vector<std::pair<const uint8_t *, const uint8_t *>> t_readable;

  struct ScanCache
  {
    ScanCache() { t_readable.clear(); }
    ~ScanCache() { t_readable.clear(); }
  };

  bool Readable(const uint8_t *base, uint32_t addr, uint32_t size)
  {
    const uint8_t *begin = base + addr, *end = begin + size;
    for (auto &[lo, hi] : t_readable)
    {
      if (begin >= lo && end <= hi)
      {
        return true;
      }
    }
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(begin, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
    {
      return false;
    }
    const uint8_t *region_end = static_cast<const uint8_t *>(info.BaseAddress) + info.RegionSize;
    if (t_readable.size() < 4096)
    {
      t_readable.emplace_back(static_cast<const uint8_t *>(info.BaseAddress), region_end);
    }
    return end <= region_end;
  }

  // The chars of a GNames entry, "" when unreadable.
  std::string NameString(const uint8_t *base, uint32_t index)
  {
    if (index >= 0x100000)
    {
      return {};
    }
    uint32_t entry = LoadU32(base, ao2::ue3::GNames(base) + index * 4);
    if (!Plausible(entry) || !Readable(base, entry + 16, 64))
    {
      return {};
    }
    const char *s = reinterpret_cast<const char *>(base + entry + 16);
    size_t n = strnlen(s, 64);
    for (size_t i = 0; i < n; ++i)
    {
      if (s[i] < 32 || s[i] > 126)
      {
        return {};
      }
    }
    return std::string(s, n);
  }

  bool NameIs(const uint8_t *base, uint32_t object, const char *name)
  {
    return Plausible(object) && Readable(base, object, 56) && NameString(base, LoadU32(base, object + kName)) == name;
  }

  std::mutex g_mutex;
  std::map<std::string, std::pair<uint32_t, uint64_t>> g_classes; // class, when looked for
  std::map<std::string, int32_t> g_offsets;
  std::map<std::string, uint32_t> g_masks;
} // namespace

namespace ao2::ue3
{
  uint32_t ObjectCount(const uint8_t *base)
  {
    return LoadU32(base, kGObjects + 4);
  }

  uint32_t ObjectAt(const uint8_t *base, uint32_t index)
  {
    uint32_t data = LoadU32(base, kGObjects);
    return data ? LoadU32(base, data + index * 4) : 0;
  }

  uint32_t FindClass(const uint8_t *base, const char *name)
  {
    std::lock_guard lock(g_mutex);
    // A class not found (not loaded yet) is looked for again after 5 s,
    // not on every call (a scan of every object).
    uint64_t now = GetTickCount64();
    if (auto it = g_classes.find(name);
        it != g_classes.end() && (it->second.first || now - it->second.second < 5000))
    {
      return it->second.first;
    }
    ScanCache cache;
    uint32_t found = 0;
    for (uint32_t i = 0, n = ObjectCount(base); i < n && !found; ++i)
    {
      uint32_t o = ObjectAt(base, i);
      if (!NameIs(base, o, name))
      {
        continue;
      }
      uint32_t cls = LoadU32(base, o + kClass);
      if (Plausible(cls) && NameIs(base, cls, "Class"))
      {
        found = o;
      }
    }
    g_classes[name] = {found, now};
    return found;
  }

  int32_t PropertyOffset(const uint8_t *base, const char *cls, const char *prop)
  {
    std::string key = std::string(cls) + "." + prop;
    {
      std::lock_guard lock(g_mutex);
      if (auto it = g_offsets.find(key); it != g_offsets.end())
      {
        return it->second;
      }
    }
    uint32_t owner = FindClass(base, cls);
    ScanCache cache;
    int32_t offset = -1;
    for (uint32_t i = 0, n = owner ? ObjectCount(base) : 0; i < n && offset < 0; ++i)
    {
      uint32_t o = ObjectAt(base, i);
      if (!NameIs(base, o, prop) || LoadU32(base, o + kOuter) != owner)
      {
        continue;
      }
      uint32_t pc = LoadU32(base, o + kClass);
      std::string pcn = Plausible(pc) && Readable(base, pc, 56) ? NameString(base, LoadU32(base, pc + kName)) : "";
      if (pcn.size() > 8 && pcn.ends_with("Property"))
      {
        offset = int32_t(LoadU32(base, o + kPropertyOffset));
        if (pcn == "BoolProperty")
        {
          std::lock_guard lock(g_mutex);
          g_masks[key] = LoadU32(base, o + kBoolMask);
        }
      }
    }
    if (owner)
    {
      std::lock_guard lock(g_mutex);
      g_offsets[key] = offset; // a class not loaded yet is looked up again
    }
    return offset;
  }

  std::vector<Property> Properties(const uint8_t *base, uint32_t cls)
  {
    ScanCache cache;
    std::vector<Property> out;
    std::vector<uint32_t> chain;
    for (uint32_t c = cls; Plausible(c) && Readable(base, c, 64) && chain.size() < 32; c = LoadU32(base, c + kSuperField))
    {
      chain.push_back(c);
    }
    for (uint32_t i = 0, n = chain.empty() ? 0 : ObjectCount(base); i < n; ++i)
    {
      uint32_t o = ObjectAt(base, i);
      if (!Plausible(o) || !Readable(base, o, 0x68))
      {
        continue;
      }
      uint32_t outer = LoadU32(base, o + kOuter);
      if (std::find(chain.begin(), chain.end(), outer) == chain.end())
      {
        continue;
      }
      uint32_t pc = LoadU32(base, o + kClass);
      std::string type = Plausible(pc) && Readable(base, pc, 56) ? NameString(base, LoadU32(base, pc + kName)) : "";
      if (type.size() <= 8 || !type.ends_with("Property"))
      {
        continue;
      }
      out.push_back({NameString(base, LoadU32(base, o + kName)), type, NameString(base, LoadU32(base, outer + kName)),
                     int32_t(LoadU32(base, o + kPropertyOffset))});
    }
    std::sort(out.begin(), out.end(), [](const Property &a, const Property &b) { return a.offset < b.offset; });
    return out;
  }

  bool BoolProperty(const uint8_t *base, const char *cls, const char *prop, int32_t &offset, uint32_t &mask)
  {
    offset = PropertyOffset(base, cls, prop);
    std::lock_guard lock(g_mutex);
    auto it = g_masks.find(std::string(cls) + "." + prop);
    mask = it != g_masks.end() ? it->second : 0;
    return offset >= 0 && mask != 0;
  }

  bool IsA(const uint8_t *base, uint32_t object, uint32_t cls)
  {
    if (!cls || !Plausible(object) || !Readable(base, object, 56))
    {
      return false;
    }
    uint32_t c = LoadU32(base, object + kClass);
    for (int depth = 0; Plausible(c) && depth < 32; ++depth, c = LoadU32(base, c + kSuperField))
    {
      if (c == cls)
      {
        return true;
      }
      if (!Readable(base, c, 64))
      {
        return false;
      }
    }
    return false;
  }

  std::string Name(const uint8_t *base, uint32_t index)
  {
    ScanCache cache;
    return NameString(base, index);
  }

  std::string ObjectName(const uint8_t *base, uint32_t object)
  {
    if (!Plausible(object) || !Readable(base, object, 56))
    {
      return "?";
    }
    std::string s = NameString(base, LoadU32(base, object + kName));
    uint32_t number = LoadU32(base, object + kNameNumber);
    if (number > 0)
    {
      s += "_" + std::to_string(number - 1);
    }
    return s.empty() ? "?" : s;
  }
} // namespace ao2::ue3
