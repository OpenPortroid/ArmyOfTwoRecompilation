// ue3_reflect.h - Reading UE3 objects by class and property name (game memory).
//
// UObject: +40 Outer, +44 Name (FName: index into GNames, +48 number), +52
// Class; UStruct +60 SuperField; UProperty +0x64 Offset. GObjObjects (TArray of
// every UObject) at 0x831E4E3C, GNames (FName::Names: TArray of entry pointers,
// chars at entry +16) at 0x831D1724 (tools/ue3probe.py, tools/ue3net.py).
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ao2::ue3
{
  // The GNames entry pointer array. Read from FName::Names each time: its data
  // is on the heap (0x6D4D0000 in the disc version, 0x6D4B0000 on TU2).
  constexpr uint32_t kGNamesArray = 0x831D1724;
  inline uint32_t GNames(const uint8_t *base)
  {
    uint32_t v;
    std::memcpy(&v, base + kGNamesArray, 4);
    return std::byteswap(v);
  }

  // The UClass named `name`, 0 if none (looked up once, then cached).
  uint32_t FindClass(const uint8_t *base, const char *name);

  // Offset of property `prop` declared by class `cls` (e.g. "Actor",
  // "Location"), -1 if not found. Cached.
  int32_t PropertyOffset(const uint8_t *base, const char *cls, const char *prop);

  // A bool property's word offset and bit (UBoolProperty +0x80); false if not
  // found.
  bool BoolProperty(const uint8_t *base, const char *cls, const char *prop, int32_t &offset, uint32_t &mask);

  // Whether `object`'s class is `cls` or derives from it.
  bool IsA(const uint8_t *base, uint32_t object, uint32_t cls);

  // A reflected property of a class.
  struct Property
  {
    std::string name;  // e.g. AudioComponents
    std::string type;  // its UProperty class, e.g. ArrayProperty
    std::string owner; // the class that declares it
    int32_t offset = -1;
  };
  // Every property of `cls` and of its superclasses (one scan of every
  // object; for crash reports, not per frame).
  std::vector<Property> Properties(const uint8_t *base, uint32_t cls);

  // The text of an FName (its GNames index), "" when unreadable.
  std::string Name(const uint8_t *base, uint32_t index);

  // "Name" or "Name_N" of an object.
  std::string ObjectName(const uint8_t *base, uint32_t object);

  // GObjObjects: the number of slots and one slot (0 = free).
  uint32_t ObjectCount(const uint8_t *base);
  uint32_t ObjectAt(const uint8_t *base, uint32_t index);
} // namespace ao2::ue3
