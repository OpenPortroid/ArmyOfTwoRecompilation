// coop_state_hash.cpp - Every reflected property of the level's objects, to
// name the first field that differs between two co-op machines.
//
// The lockstep checksum (coop.h) covers rand() and a physics hash, and
// sync_trace.log a few fields of the pawns: a desync shows long after its
// cause and only in those fields. ao2_coop_state_hash=N writes, every N
// lockstep frames (at the frame's record build, the same point of the frame
// on both machines), the value of every UnrealScript-visible property of
// every object that lives in a level (Outer chain reaches a Level: actors,
// their components, Kismet) to state_hash.log - but UObject's own fields and
// native data (structs Pointer and *_Mirror); tools/state_diff.py compares
// the logs of two machines and prints the first frame, object and property
// that differ.
//
// Only what changed since the object's last sample is written (a new object:
// what is not zero - the same rule on both machines), so a sample costs a
// pass over the properties and a few kilobytes. Values are written in a form that is
// the same on both machines: an object reference as the object's path, an
// FName as its text (addresses and name indices differ per machine).
//
//   G <mode>                          a game starts (the lockstep mode)
//   C <class id> <class name>
//   T <class id> <leaf> <type> <name> a property (or a member of a struct):
//                                     type = f/i/b letters of a plain value
//                                     ("fff" = Vector), or Bool, Name, Object,
//                                     Str, Array, Delegate
//   F <frame>                         a sample starts
//   N <object id> <class id> <path>   a new object
//   X <object id>                     an object is gone
//   <object id> <leaf>=<value> ...    changed properties
//   E <frame> <objects> <changed> <ms>
//
// UProperty: +0x44 ArrayDim, +0x48 ElementSize, +0x64 Offset; +0x80 = a bool's
// bit mask / the struct of a struct property / the inner property of an array
// (tools/ue3class.py).
#include <windows.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>

#include "coop.h"
#include "ue3_reflect.h"

REXCVAR_DEFINE_INT32(ao2_coop_state_hash, 0, "Debug",
                     "Every N lockstep frames write every changed property of the level's objects to "
                     "state_hash.log (compare two machines with tools/state_diff.py); 0 = off");
REXCVAR_DEFINE_BOOL(ao2_coop_state_hash_solo, false, "Debug",
                    "ao2_coop_state_hash also in single player (its local lockstep)");
REXCVAR_DEFINE_STRING(ao2_coop_state_hash_skip, "ParticleSystemComponent,AudioComponent", "Debug",
                      "ao2_coop_state_hash: classes (and their subclasses) to leave out, comma separated");

namespace
{
  constexpr uint32_t kOuter = 40, kName = 44, kNumber = 48, kClass = 52, kSuper = 60;
  constexpr uint32_t kArrayDim = 0x44, kElementSize = 0x48, kPropOffset = 0x64, kPropExtra = 0x80;
  constexpr uint32_t kObjectSize = 60; // UObject
  constexpr uint32_t kMaxElements = 1024; // of an array that are looked at
  constexpr uint32_t kMaxString = 256;

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
  bool PlausibleData(uint32_t p)
  {
    return p >= 0x40000000 && p < 0xA0000000;
  }

  // Committed regions seen during this sample (one VirtualQuery per region,
  // not per object).
  std::map<uintptr_t, uintptr_t> g_regions; // begin -> end
  bool Readable(const uint8_t *base, uint32_t addr, uint32_t size)
  {
    uintptr_t begin = reinterpret_cast<uintptr_t>(base) + addr, end = begin + size;
    auto it = g_regions.upper_bound(begin);
    if (it != g_regions.begin() && end <= std::prev(it)->second)
    {
      return true;
    }
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(reinterpret_cast<const void *>(begin), &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
    {
      return false;
    }
    uintptr_t region = reinterpret_cast<uintptr_t>(info.BaseAddress);
    g_regions[region] = region + info.RegionSize;
    return end <= region + info.RegionSize;
  }

  uint64_t HashBytes(const uint8_t *p, size_t n, uint64_t h = 0x243F6A8885A308D3ull)
  {
    while (n >= 8)
    {
      uint64_t w;
      std::memcpy(&w, p, 8);
      h = (h ^ w) * 0x9E3779B97F4A7C15ull;
      h ^= h >> 29;
      p += 8;
      n -= 8;
    }
    uint64_t w = 0;
    std::memcpy(&w, p, n);
    h = (h ^ w ^ (uint64_t(n) << 56)) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 32);
  }

  void AppendHex(std::string &out, const uint8_t *p, size_t n)
  {
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; ++i)
    {
      out += digits[p[i] >> 4];
      out += digits[p[i] & 15];
    }
  }

  // The text of a name index (entries never change).
  std::unordered_map<uint32_t, std::string> g_names;
  const std::string &NameText(const uint8_t *base, uint32_t index)
  {
    auto it = g_names.find(index);
    if (it == g_names.end())
    {
      std::string s = ao2::ue3::Name(base, index);
      for (char &c : s)
      {
        if (c == ' ' || c == '=')
        {
          c = '_';
        }
      }
      it = g_names.emplace(index, s.empty() ? "?" : s).first;
    }
    return it->second;
  }

  // "Name" / "Name_N" from an FName in memory (index, number).
  void AppendName(const uint8_t *base, uint32_t addr, std::string &out)
  {
    out += NameText(base, LoadU32(base, addr));
    if (uint32_t number = LoadU32(base, addr + 4))
    {
      out += '_';
      out += std::to_string(number - 1);
    }
  }

  // Whether a class is, or extends, one of ao2_coop_state_hash_skip: this
  // machine's own objects (sound, particles).
  bool SkippedClass(const uint8_t *base, uint32_t cls);

  // An object's path, outermost first; the same on both machines (unlike its
  // address). Cached per address, checked against the object's name and outer.
  struct PathEntry
  {
    uint32_t name = 0, number = 0, outer = 0;
    std::string path;
  };
  std::unordered_map<uint32_t, PathEntry> g_paths;
  const std::string &ObjectPath(const uint8_t *base, uint32_t object, int depth = 0)
  {
    static const std::string none = "None", bad = "?";
    if (!object)
    {
      return none;
    }
    if (!Plausible(object) || !Readable(base, object, 64) || depth > 12)
    {
      return bad;
    }
    uint32_t name = LoadU32(base, object + kName), number = LoadU32(base, object + kNumber),
             outer = LoadU32(base, object + kOuter);
    PathEntry &e = g_paths[object];
    if (e.path.empty() || e.name != name || e.number != number || e.outer != outer)
    {
      std::string path;
      if (outer)
      {
        path = ObjectPath(base, outer, depth + 1); // may rehash g_paths: `e` is looked up again below
        path += '.';
      }
      path += NameText(base, name);
      // This machine's own objects are numbered by this machine (a sound more
      // on one of them): no number.
      if (SkippedClass(base, LoadU32(base, object + kClass)))
      {
        path += "_*";
      }
      else if (number)
      {
        path += '_';
        path += std::to_string(number - 1);
      }
      PathEntry &again = g_paths[object];
      again = {name, number, outer, std::move(path)};
      return again.path;
    }
    return e.path;
  }

  // The name of an object's class.
  std::unordered_map<uint32_t, std::string> g_class_names;
  const std::string &ClassName(const uint8_t *base, uint32_t cls)
  {
    static const std::string bad = "?";
    auto it = g_class_names.find(cls);
    if (it != g_class_names.end())
    {
      return it->second;
    }
    if (!Plausible(cls) || !Readable(base, cls, 64))
    {
      return bad;
    }
    return g_class_names.emplace(cls, NameText(base, LoadU32(base, cls + kName))).first->second;
  }

  bool SkippedClass(const uint8_t *base, uint32_t cls)
  {
    static std::unordered_map<uint32_t, bool> known;
    static std::vector<std::string> skip;
    static bool read = false;
    if (!read)
    {
      read = true;
      std::string list = REXCVAR_GET(ao2_coop_state_hash_skip);
      for (size_t pos = 0; pos <= list.size();)
      {
        size_t end = std::min(list.find(',', pos), list.size());
        if (end > pos)
        {
          skip.push_back(list.substr(pos, end - pos));
        }
        pos = end + 1;
      }
    }
    if (auto it = known.find(cls); it != known.end())
    {
      return it->second;
    }
    bool skipped = false;
    int guard = 0;
    for (uint32_t c = cls; Plausible(c) && Readable(base, c, 64) && guard < 32; c = LoadU32(base, c + kSuper), ++guard)
    {
      skipped = skipped || std::find(skip.begin(), skip.end(), ClassName(base, c)) != skip.end();
    }
    known[cls] = skipped;
    return skipped;
  }

  // ---------------------------------------------------------------------
  // Layouts: the properties of a class as a flat list of leaves
  // ---------------------------------------------------------------------

  enum class Kind : uint8_t
  {
    Raw,      // bytes that mean the same on both machines (int, float, byte, plain structs)
    Bool,     // one bit of a word
    Name,     // FName
    Object,   // a pointer to an object
    String,   // FString
    Array,    // TArray
    Delegate, // object + FName
  };
  struct Leaf
  {
    uint32_t offset = 0, size = 0;
    Kind kind = Kind::Raw;
    uint32_t mask = 0;    // Bool
    int32_t element = -1; // Array: index into g_elements
    std::string name, type;
  };
  // What an array holds.
  struct Element
  {
    uint32_t size = 0;
    bool plain = true; // no strings or arrays inside: its bytes change when its value does
    std::vector<Leaf> leaves;
  };
  struct Layout
  {
    uint32_t id = 0, size = 0;
    bool skip = false;
    std::vector<Leaf> leaves;
  };

  std::unordered_map<uint32_t, std::vector<uint32_t>> g_properties; // struct / class -> its own properties
  uint32_t g_index_count = 0;                                       // objects when it was built
  bool g_index_fresh = false;                                       // built during this sample
  std::deque<Element> g_elements;
  std::unordered_map<uint32_t, int32_t> g_element_of;          // inner property -> element
  std::unordered_map<uint32_t, std::vector<Leaf>> g_structs;   // struct -> leaves from offset 0
  std::unordered_map<uint32_t, Layout> g_layouts;              // class -> layout
  std::set<std::string> g_unknown_types;
  std::string g_out; // the sample being written

  // One pass over every object: which properties each struct declares.
  void BuildIndex(const uint8_t *base)
  {
    g_properties.clear();
    std::unordered_map<uint32_t, bool> is_property; // by the object's class
    uint32_t count = ao2::ue3::ObjectCount(base);
    for (uint32_t i = 0; i < count; ++i)
    {
      uint32_t o = ao2::ue3::ObjectAt(base, i);
      if (!Plausible(o) || !Readable(base, o, 0x84))
      {
        continue;
      }
      uint32_t cls = LoadU32(base, o + kClass);
      auto it = is_property.find(cls);
      if (it == is_property.end())
      {
        it = is_property.emplace(cls, ClassName(base, cls).ends_with("Property")).first;
      }
      if (it->second)
      {
        g_properties[LoadU32(base, o + kOuter)].push_back(o);
      }
    }
    g_index_count = count;
    g_index_fresh = true;
  }

  void BuildProperty(const uint8_t *base, uint32_t prop, uint32_t at, const std::string &prefix,
                     std::vector<Leaf> &out, int depth);

  // The leaves of a struct (or class) and of what it extends, from offset 0.
  std::vector<Leaf> StructLeaves(const uint8_t *base, uint32_t st, int depth)
  {
    std::vector<Leaf> leaves;
    int guard = 0;
    for (uint32_t s = st; Plausible(s) && Readable(base, s, 64) && guard < 32; s = LoadU32(base, s + kSuper), ++guard)
    {
      auto it = g_properties.find(s);
      if (it == g_properties.end())
      {
        continue;
      }
      for (uint32_t prop : it->second)
      {
        BuildProperty(base, prop, 0, "", leaves, depth);
      }
    }
    std::stable_sort(leaves.begin(), leaves.end(), [](const Leaf &a, const Leaf &b) {
      return a.offset != b.offset ? a.offset < b.offset : a.mask < b.mask;
    });
    return leaves;
  }

  int32_t ElementOf(const uint8_t *base, uint32_t inner, int depth)
  {
    if (auto it = g_element_of.find(inner); it != g_element_of.end())
    {
      return it->second;
    }
    Element e;
    if (Plausible(inner) && Readable(base, inner, 0x84))
    {
      e.size = LoadU32(base, inner + kElementSize);
      BuildProperty(base, inner, 0u - LoadU32(base, inner + kPropOffset), "", e.leaves, depth + 1);
      for (const Leaf &l : e.leaves)
      {
        e.plain = e.plain && l.kind != Kind::String && l.kind != Kind::Array;
      }
    }
    g_elements.push_back(std::move(e));
    int32_t index = int32_t(g_elements.size() - 1);
    g_element_of[inner] = index;
    return index;
  }

  void BuildProperty(const uint8_t *base, uint32_t prop, uint32_t at, const std::string &prefix,
                     std::vector<Leaf> &out, int depth)
  {
    const std::string &type = ClassName(base, LoadU32(base, prop + kClass));
    uint32_t dim = LoadU32(base, prop + kArrayDim), size = LoadU32(base, prop + kElementSize);
    uint32_t extra = LoadU32(base, prop + kPropExtra);
    std::string pname = prefix + NameText(base, LoadU32(base, prop + kName));
    if (dim == 0 || dim > 256 || size == 0 || size > 0x10000 || depth > 6)
    {
      return;
    }
    for (uint32_t d = 0; d < dim; ++d)
    {
      Leaf leaf;
      leaf.offset = at + LoadU32(base, prop + kPropOffset) + d * size;
      leaf.size = size;
      leaf.name = dim > 1 ? pname + "[" + std::to_string(d) + "]" : pname;
      if (type == "FloatProperty" || type == "IntProperty" || type == "ByteProperty")
      {
        leaf.type = type[0] == 'F' ? "f" : type[0] == 'I' ? "i" : "b";
      }
      else if (type == "BoolProperty")
      {
        leaf.kind = Kind::Bool;
        leaf.mask = extra;
        leaf.type = "Bool";
      }
      else if (type == "NameProperty")
      {
        leaf.kind = Kind::Name;
        leaf.type = "Name";
      }
      else if (type == "ObjectProperty" || type == "ClassProperty" || type == "ComponentProperty" ||
               type == "InterfaceProperty")
      {
        leaf.kind = Kind::Object;
        leaf.type = "Object";
      }
      else if (type == "StrProperty")
      {
        leaf.kind = Kind::String;
        leaf.type = "Str";
      }
      else if (type == "DelegateProperty")
      {
        leaf.kind = Kind::Delegate;
        leaf.type = "Delegate";
      }
      else if (type == "ArrayProperty")
      {
        leaf.kind = Kind::Array;
        leaf.type = "Array";
        leaf.element = ElementOf(base, extra, depth);
      }
      else if (type == "StructProperty")
      {
        // Native data behind a script-visible name ("pointer" variables,
        // native maps and arrays): addresses, this machine's own.
        const std::string &sname = Plausible(extra) && Readable(base, extra, 64)
                                       ? NameText(base, LoadU32(base, extra + kName)) : type;
        if (sname == "Pointer" || sname.ends_with("_Mirror"))
        {
          return;
        }
        auto it = g_structs.find(extra);
        if (it == g_structs.end())
        {
          it = g_structs.emplace(extra, std::vector<Leaf>{}).first; // a struct inside itself ends here
          std::vector<Leaf> leaves = StructLeaves(base, extra, depth + 1);
          it = g_structs.find(extra);
          it->second = std::move(leaves);
        }
        const std::vector<Leaf> &members = it->second;
        // A small struct of plain values without padding is one value
        // (Vector, Rotator, Color); anything else is its members.
        uint32_t total = 0;
        bool plain = !members.empty();
        std::string letters;
        for (const Leaf &m : members)
        {
          plain = plain && m.kind == Kind::Raw && m.offset == total;
          total += m.size;
          letters += m.type;
        }
        if (plain && total == size && size <= 16)
        {
          leaf.type = letters;
        }
        else
        {
          for (Leaf m : members)
          {
            m.offset += leaf.offset;
            m.name = leaf.name + "." + m.name;
            out.push_back(std::move(m));
          }
          continue;
        }
      }
      else
      {
        g_unknown_types.insert(type); // MapProperty and the like: native containers
        return;
      }
      out.push_back(std::move(leaf));
    }
  }

  void AppendText(const uint8_t *base, uint32_t addr, const Leaf &leaf, std::string &out, int depth);

  Layout &LayoutOf(const uint8_t *base, uint32_t cls)
  {
    static uint32_t next_id = 1;
    if (auto it = g_layouts.find(cls); it != g_layouts.end())
    {
      return it->second;
    }
    // A class loaded after the index was built (a streamed level's package).
    if (!g_index_fresh && ao2::ue3::ObjectCount(base) != g_index_count)
    {
      BuildIndex(base);
    }
    Layout layout;
    layout.id = next_id++;
    layout.skip = SkippedClass(base, cls);
    if (!layout.skip)
    {
      layout.leaves = StructLeaves(base, cls, 0);
      // Not UObject's own fields (hash links, flags, the linker: the engine's).
      std::erase_if(layout.leaves, [](const Leaf &l) { return l.offset < kObjectSize; });
      for (const Leaf &l : layout.leaves)
      {
        layout.size = std::max(layout.size, l.offset + l.size);
      }
      g_out += "C " + std::to_string(layout.id) + " " + ClassName(base, cls) + "\n";
      for (size_t i = 0; i < layout.leaves.size(); ++i)
      {
        const Leaf &l = layout.leaves[i];
        g_out += "T " + std::to_string(layout.id) + " " + std::to_string(i) + " " + l.type + " " + l.name + "\n";
      }
    }
    return g_layouts.emplace(cls, std::move(layout)).first->second;
  }

  // ---------------------------------------------------------------------
  // Values
  // ---------------------------------------------------------------------

  // A value's text, the same on both machines; no spaces.
  void AppendText(const uint8_t *base, uint32_t addr, const Leaf &leaf, std::string &out, int depth)
  {
    switch (leaf.kind)
    {
    case Kind::Raw:
      AppendHex(out, base + addr, leaf.size);
      break;
    case Kind::Bool:
      out += LoadU32(base, addr) & leaf.mask ? '1' : '0';
      break;
    case Kind::Name:
      AppendName(base, addr, out);
      break;
    case Kind::Object:
      out += ObjectPath(base, LoadU32(base, addr));
      break;
    case Kind::Delegate:
      out += ObjectPath(base, LoadU32(base, addr));
      out += ':';
      AppendName(base, addr + 4, out);
      break;
    case Kind::String:
    {
      uint32_t data = LoadU32(base, addr), num = LoadU32(base, addr + 4);
      out += '"';
      if (num && num < 0x100000 && PlausibleData(data) && Readable(base, data, std::min(num, kMaxString)))
      {
        for (uint32_t i = 0; i < std::min(num, kMaxString) && base[data + i]; ++i)
        {
          char c = char(base[data + i]);
          out += c > 32 && c < 127 ? c : '_';
        }
      }
      out += '"';
      break;
    }
    case Kind::Array:
    {
      uint32_t data = LoadU32(base, addr), num = LoadU32(base, addr + 4);
      out += 'n';
      out += std::to_string(num);
      const Element &e = g_elements[leaf.element];
      uint32_t count = std::min(num, kMaxElements);
      if (!count || !e.size || e.leaves.empty() || depth > 3)
      {
        break;
      }
      if (num > 0x1000000 || !PlausibleData(data) || !Readable(base, data, count * e.size))
      {
        out += ":?";
        break;
      }
      std::string items;
      for (uint32_t i = 0; i < count; ++i)
      {
        items += i ? ';' : '[';
        for (size_t k = 0; k < e.leaves.size(); ++k)
        {
          if (k)
          {
            items += ',';
          }
          AppendText(base, data + i * e.size + e.leaves[k].offset, e.leaves[k], items, depth + 1);
        }
      }
      items += ']';
      // A long array as a hash of its text; state_diff.py only needs equality.
      if (items.size() <= 120)
      {
        out += ':';
        out += items;
      }
      else
      {
        char hash[24];
        std::snprintf(hash, sizeof(hash), "#%016llX",
                      static_cast<unsigned long long>(HashBytes(reinterpret_cast<const uint8_t *>(items.data()), items.size())));
        out += hash;
      }
      break;
    }
    }
  }

  // Changes when the value does; this machine's own (addresses, name
  // indices), cheap. Zeroed memory gives 0 for every kind but long raw values.
  uint64_t LocalHash(const uint8_t *base, uint32_t addr, const Leaf &leaf)
  {
    switch (leaf.kind)
    {
    case Kind::Bool:
      return (LoadU32(base, addr) & leaf.mask) != 0;
    case Kind::Raw:
    case Kind::Name:
    case Kind::Object:
    case Kind::Delegate:
    {
      if (leaf.size <= 8)
      {
        uint64_t v = 0;
        std::memcpy(&v, base + addr, leaf.size);
        return v;
      }
      return HashBytes(base + addr, leaf.size);
    }
    case Kind::String:
    {
      uint32_t data = LoadU32(base, addr), num = LoadU32(base, addr + 4);
      if (!num || num > 0x100000 || !PlausibleData(data) || !Readable(base, data, std::min(num, kMaxString)))
      {
        return num;
      }
      return HashBytes(base + data, std::min(num, kMaxString), num);
    }
    case Kind::Array:
    {
      uint32_t data = LoadU32(base, addr), num = LoadU32(base, addr + 4);
      const Element &e = g_elements[leaf.element];
      uint32_t count = std::min(num, kMaxElements);
      if (!count || !e.size || num > 0x1000000 || !PlausibleData(data) || !Readable(base, data, count * e.size))
      {
        return num;
      }
      if (e.plain)
      {
        return HashBytes(base + data, size_t(count) * e.size, num);
      }
      std::string text;
      AppendText(base, addr, leaf, text, 0);
      return HashBytes(reinterpret_cast<const uint8_t *>(text.data()), text.size(), num);
    }
    }
    return 0;
  }

  // ---------------------------------------------------------------------
  // Objects
  // ---------------------------------------------------------------------

  // A slot of GObjObjects.
  struct Slot
  {
    uint32_t addr = 0, name = 0, number = 0, cls = 0, outer = 0;
    uint32_t id = 0; // 0 = not one of the level's objects
    const Layout *layout = nullptr;
    std::vector<uint64_t> last; // LocalHash of each leaf
  };
  std::vector<Slot> g_slots;
  uint32_t g_next_object = 1;

  // Whether the object lives in a level (its Outer chain reaches a Level).
  bool InLevel(const uint8_t *base, uint32_t outer)
  {
    for (int depth = 0; depth < 12 && Plausible(outer) && Readable(base, outer, 64); ++depth)
    {
      if (ClassName(base, LoadU32(base, outer + kClass)) == "Level")
      {
        return true;
      }
      outer = LoadU32(base, outer + kOuter);
    }
    return false;
  }

  void Forget(Slot &slot)
  {
    if (slot.id)
    {
      g_out += "X " + std::to_string(slot.id) + "\n";
    }
    slot = Slot{};
  }

  void Sample(const uint8_t *base, uint32_t frame, FILE *file)
  {
    auto start = std::chrono::steady_clock::now();
    g_regions.clear();
    g_index_fresh = false;
    if (g_properties.empty())
    {
      BuildIndex(base);
    }
    g_out += "F " + std::to_string(frame) + "\n";
    uint32_t count = ao2::ue3::ObjectCount(base), objects = 0, changed = 0;
    if (count > 0x400000)
    {
      count = 0;
    }
    for (size_t i = count; i < g_slots.size(); ++i)
    {
      Forget(g_slots[i]);
    }
    g_slots.resize(count);
    std::string line;
    for (uint32_t i = 0; i < count; ++i)
    {
      Slot &slot = g_slots[i];
      uint32_t o = ao2::ue3::ObjectAt(base, i);
      if (!Plausible(o) || !Readable(base, o, 64))
      {
        Forget(slot);
        continue;
      }
      uint32_t name = LoadU32(base, o + kName), number = LoadU32(base, o + kNumber), cls = LoadU32(base, o + kClass),
               outer = LoadU32(base, o + kOuter);
      if (o != slot.addr || name != slot.name || number != slot.number || cls != slot.cls || outer != slot.outer)
      {
        Forget(slot);
        slot.addr = o, slot.name = name, slot.number = number, slot.cls = cls, slot.outer = outer;
        if (InLevel(base, outer))
        {
          const Layout &layout = LayoutOf(base, cls);
          if (!layout.skip)
          {
            slot.id = g_next_object++;
            slot.layout = &layout;
            slot.last.assign(layout.leaves.size(), 0);
            g_out += "N " + std::to_string(slot.id) + " " + std::to_string(layout.id) + " " + ObjectPath(base, o) + "\n";
          }
        }
      }
      if (!slot.id || !Readable(base, o, std::max(slot.layout->size, 64u)))
      {
        continue;
      }
      ++objects;
      line.clear();
      const std::vector<Leaf> &leaves = slot.layout->leaves;
      for (size_t k = 0; k < leaves.size(); ++k)
      {
        uint64_t h = LocalHash(base, o + leaves[k].offset, leaves[k]);
        if (h != slot.last[k])
        {
          slot.last[k] = h;
          line += ' ';
          line += std::to_string(k);
          line += '=';
          AppendText(base, o + leaves[k].offset, leaves[k], line, 0);
          ++changed;
        }
      }
      if (!line.empty())
      {
        g_out += std::to_string(slot.id);
        g_out += line;
        g_out += '\n';
      }
    }
    for (const std::string &type : g_unknown_types)
    {
      g_out += "# not read: " + type + "\n";
    }
    g_unknown_types.clear();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    g_out += "E " + std::to_string(frame) + " " + std::to_string(objects) + " " + std::to_string(changed) + " " +
             std::to_string(ms) + "\n";
    std::fwrite(g_out.data(), 1, g_out.size(), file);
    std::fflush(file);
    g_out.clear();
  }
} // namespace

namespace ao2::coop
{
  void StateHashFrame(const uint8_t *base, uint32_t frame)
  {
    static uint32_t last_frame = ~0u, last_mode = 0;
    int32_t every = REXCVAR_GET(ao2_coop_state_hash);
    if (every <= 0 || frame == last_frame)
    {
      return;
    }
    uint32_t mode = OnlineLockstep(base) ? 1 : LocalLockstep(base) && REXCVAR_GET(ao2_coop_state_hash_solo) ? 4 : 0;
    if (!mode)
    {
      return;
    }
    // A debug dump asked for by its flag, not a log (log_files.h).
    static FILE *file = std::fopen("state_hash.log", "w");
    if (!file)
    {
      return;
    }
    // A new game: the frames start again (or the mode changed). Everything is
    // written anew, so state_diff.py can compare each game on its own.
    if (last_frame == ~0u || frame < last_frame || mode != last_mode)
    {
      for (Slot &slot : g_slots)
      {
        slot = Slot{};
      }
      // Classes may have been unloaded with the last level: read them again.
      g_paths.clear();
      g_layouts.clear();
      g_structs.clear();
      g_elements.clear();
      g_element_of.clear();
      g_properties.clear();
      g_class_names.clear();
      g_out += "G " + std::to_string(mode) + "\n";
    }
    last_frame = frame;
    last_mode = mode;
    if (frame % uint32_t(every) == 0)
    {
      Sample(base, frame, file);
    }
  }
} // namespace ao2::coop
