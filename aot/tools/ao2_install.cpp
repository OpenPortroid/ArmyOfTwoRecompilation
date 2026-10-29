// ao2_install - reads the user's own game media for the launcher's install
// screen: a disc image (.iso, XDVDFS), a Games on Demand / title update /
// downloadable content package (STFS or SVOD). Uses the runtime's file system
// devices, so it reads exactly what the game itself could mount.
//
//   ao2_install probe <source>
//       KIND=game|tu|dlc|unknown   (by content: default.xex / default.xexp)
//       CONTAINER=iso|package      TITLE=<hex title id, packages only>
//       NAME=<display name, packages only>
//       FILES=<count>  BYTES=<total size of the files>
//   ao2_install extract <source> <dest dir> [--only=<path>] [--skip=<top level name>]...
//       PROGRESS <bytes done> <bytes total>   (about ten times a second)
//       OK
//   ao2_install cpu
//       X86_64_V3=1|0   (the main game build runs here: AVX2, FMA, BMI1/2, LZCNT,
//                        MOVBE, F16C, and the OS saves the AVX registers)
//       MISSING=<what lacks, space separated; empty when X86_64_V3=1>
// Output is UTF-8, one item per line; errors are "ERROR <text>", exit code 1.

#define _CRT_SECURE_NO_WARNINGS
#include <intrin.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <rex/filesystem/device.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/string.h>
#include <rex/system/xcontent.h>

namespace fs = std::filesystem;
using rex::filesystem::Device;
using rex::filesystem::Entry;

namespace
{

  std::string Utf8(const fs::path &p)
  {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
  }

  // \\?\ form: lifts the 260 character limit (a package's own folders on top of a long game path).
  fs::path Long(const fs::path &p)
  {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec).lexically_normal();
    abs.make_preferred();
    const std::wstring &s = abs.native();
    if (ec || s.rfind(L"\\\\?\\", 0) == 0)
    {
      return abs;
    }
    if (s.rfind(L"\\\\", 0) == 0)
    {
      return fs::path(L"\\\\?\\UNC\\" + s.substr(2));
    }
    return fs::path(L"\\\\?\\" + s);
  }

  // Package file names may hold what Windows does not allow in a name; they are made harmless.
  fs::path Safe(const fs::path &rel)
  {
    fs::path r;
    for (const auto &part : rel)
    {
      std::wstring s = part.native();
      for (wchar_t &c : s)
      {
        if (c < 32 || wcschr(L"<>:\"|?*", c))
        {
          c = L'_';
        }
      }
      while (!s.empty() && (s.back() == L' ' || s.back() == L'.'))
      {
        s.pop_back();
      }
      if (!s.empty())
      {
        r /= s;
      }
    }
    return r;
  }

  int Fail(const std::string &text)
  {
    std::printf("ERROR %s\n", text.c_str());
    std::fflush(stdout);
    return 1;
  }

  bool IsDiscImage(const fs::path &path)
  {
    // XDVDFS: "MICROSOFT*XBOX*MEDIA" at sector 32 of the game partition,
    // which starts at one of a few known offsets of the image.
    static const uint64_t kOffsets[] = {0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000};
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f)
    {
      return false;
    }
    bool found = false;
    for (uint64_t off : kOffsets)
    {
      char magic[20] = {};
      if (_fseeki64(f, int64_t(off + 32 * 2048), SEEK_SET) == 0 && std::fread(magic, 1, 20, f) == 20 &&
          std::memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20) == 0)
      {
        found = true;
        break;
      }
    }
    std::fclose(f);
    return found;
  }

  struct Source
  {
    std::unique_ptr<Device> device;
    Entry *root = nullptr;
    bool iso = false;
    uint32_t title_id = 0;
    std::string name;
  };

  bool Open(const fs::path &path, Source &src, std::string &error)
  {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
    {
      error = "not a file";
      return false;
    }
    if (IsDiscImage(path))
    {
      auto dev = std::make_unique<rex::filesystem::DiscImageDevice>("\\Device\\Install", path);
      if (!dev->Initialize())
      {
        error = "damaged disc image";
        return false;
      }
      src.iso = true;
      src.device = std::move(dev);
    }
    else
    {
      auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(path);
      if (!header)
      {
        error = "not a disc image or a content package";
        return false;
      }
      auto dev = std::make_unique<rex::filesystem::StfsContainerDevice>("\\Device\\Install", path);
      if (!dev->Initialize())
      {
        error = "damaged content package";
        return false;
      }
      src.title_id = dev->header().metadata.execution_info.title_id;
      src.name = rex::string::to_utf8(dev->header().metadata.display_name(rex::system::XLanguage::kEnglish));
      src.device = std::move(dev);
    }
    src.root = src.device->ResolvePath("");
    if (!src.root)
    {
      error = "empty file system";
      return false;
    }
    return true;
  }

  struct Item
  {
    Entry *entry;
    fs::path rel;
  };

  void List(Entry *dir, const fs::path &rel, std::vector<Item> &files, std::vector<fs::path> &dirs)
  {
    for (auto &child : dir->children())
    {
      fs::path p = rel / fs::path(std::u8string(child->name().begin(), child->name().end()));
      if (child->attributes() & rex::filesystem::kFileAttributeDirectory)
      {
        dirs.push_back(p);
        List(child.get(), p, files, dirs);
      }
      else
      {
        files.push_back({child.get(), p});
      }
    }
  }

  bool SameName(const fs::path &a, const std::string &b)
  {
    return _stricmp(Utf8(a).c_str(), b.c_str()) == 0;
  }

  int Probe(const fs::path &path)
  {
    Source src;
    std::string error;
    if (!Open(path, src, error))
    {
      return Fail(error);
    }
    std::vector<Item> files;
    std::vector<fs::path> dirs;
    List(src.root, {}, files, dirs);
    uint64_t bytes = 0;
    bool xex = false, xexp = false;
    for (auto &f : files)
    {
      bytes += f.entry->size();
      xex |= SameName(f.rel, "default.xex");
      xexp |= SameName(f.rel, "default.xexp");
    }
    const char *kind = xex ? "game" : xexp ? "tu" : (!src.iso && !files.empty()) ? "dlc" : "unknown";
    std::printf("KIND=%s\nCONTAINER=%s\nTITLE=%08X\nNAME=%s\nFILES=%zu\nBYTES=%llu\n", kind,
                src.iso ? "iso" : "package", src.title_id, src.name.c_str(), files.size(),
                static_cast<unsigned long long>(bytes));
    return 0;
  }

  int Extract(const fs::path &path, const fs::path &dest, const std::string &only,
              const std::vector<std::string> &skip)
  {
    Source src;
    std::string error;
    if (!Open(path, src, error))
    {
      return Fail(error);
    }
    std::vector<Item> all, files;
    std::vector<fs::path> dirs;
    List(src.root, {}, all, dirs);
    auto skipped = [&](const fs::path &rel)
    {
      const fs::path top = *rel.begin();
      for (auto &s : skip)
      {
        if (SameName(top, s))
        {
          return true;
        }
      }
      return false;
    };
    uint64_t total = 0;
    for (auto &f : all)
    {
      if (!only.empty() ? !SameName(f.rel, only) : skipped(f.rel))
      {
        continue;
      }
      total += f.entry->size();
      files.push_back(f);
    }
    if (files.empty())
    {
      return Fail(only.empty() ? "no files" : "no such file: " + only);
    }

    std::error_code ec;
    fs::create_directories(Long(dest), ec);
    if (ec)
    {
      return Fail("cannot create " + Utf8(dest) + ": " + ec.message());
    }
    if (only.empty())
    {
      for (auto &d : dirs)
      {
        if (!skipped(d))
        {
          fs::create_directories(Long(dest / Safe(d)), ec);
        }
      }
    }

    std::vector<uint8_t> buffer(4 << 20);
    uint64_t done = 0;
    auto last = std::chrono::steady_clock::now();
    std::printf("PROGRESS 0 %llu\n", static_cast<unsigned long long>(total));
    std::fflush(stdout);
    for (auto &f : files)
    {
      // --only puts the one file straight into the destination folder.
      fs::path out = dest / (only.empty() ? Safe(f.rel) : f.rel.filename());
      fs::create_directories(Long(out.parent_path()), ec);
      rex::filesystem::File *file = nullptr;
      if (f.entry->Open(rex::filesystem::FileAccess::kFileReadData, &file) != 0 || !file)
      {
        return Fail("cannot read " + Utf8(f.rel));
      }
      FILE *o = _wfopen(Long(out).c_str(), L"wb");
      if (!o)
      {
        const int err = errno;
        file->Destroy();
        return Fail("cannot write " + Utf8(out) + ": " + std::strerror(err));
      }
      const uint64_t size = f.entry->size();
      uint64_t pos = 0;
      bool ok = true;
      while (pos < size)
      {
        size_t want = static_cast<size_t>(std::min<uint64_t>(buffer.size(), size - pos));
        size_t got = 0;
        if (file->ReadSync(std::span<uint8_t>(buffer.data(), want), pos, &got) != 0 || got == 0)
        {
          error = "cannot read " + Utf8(f.rel);
          ok = false;
          break;
        }
        if (std::fwrite(buffer.data(), 1, got, o) != got)
        {
          error = "cannot write " + Utf8(out) + " (disk full?)";
          ok = false;
          break;
        }
        pos += got;
        done += got;
        auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::milliseconds(100))
        {
          last = now;
          std::printf("PROGRESS %llu %llu\n", static_cast<unsigned long long>(done),
                      static_cast<unsigned long long>(total));
          std::fflush(stdout);
        }
      }
      file->Destroy();
      if (std::fclose(o) != 0 && ok)
      {
        error = "cannot write " + Utf8(out) + " (disk full?)";
        ok = false;
      }
      if (!ok)
      {
        fs::remove(out, ec);
        return Fail(error);
      }
    }
    std::printf("PROGRESS %llu %llu\nOK\n", static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(total));
    std::fflush(stdout);
    return 0;
  }

  // XCR0. Only called when CPUID says OSXSAVE; this tool is built without
  // the xsave target feature (x86-64-v2), so it is enabled for this function.
  __attribute__((target("xsave"))) unsigned long long Xcr0()
  {
    return _xgetbv(0);
  }

  // What the main game build (x86-64-v3) needs, from CPUID: the launcher asks
  // this when Windows cannot tell (IsProcessorFeaturePresent knows AVX2 only
  // from Windows 10 2004 on).
  int Cpu()
  {
    int r[4];
    __cpuid(r, 0);
    int max_leaf = r[0];
    __cpuid(r, 1);
    int ecx1 = r[2];
    int ebx7 = 0;
    if (max_leaf >= 7)
    {
      __cpuidex(r, 7, 0);
      ebx7 = r[1];
    }
    __cpuid(r, static_cast<int>(0x80000000));
    int ecx81 = 0;
    if (static_cast<unsigned>(r[0]) >= 0x80000001u)
    {
      __cpuid(r, static_cast<int>(0x80000001));
      ecx81 = r[2];
    }
    // The OS saves XMM and YMM state (XCR0 bits 1 and 2): AVX is usable.
    bool os_avx = (ecx1 & (1 << 27)) && (Xcr0() & 6) == 6;
    struct
    {
      const char *name;
      bool present;
    } need[] = {
        {"AVX", (ecx1 & (1 << 28)) != 0 && os_avx},
        {"AVX2", (ebx7 & (1 << 5)) != 0 && os_avx},
        {"FMA", (ecx1 & (1 << 12)) != 0 && os_avx},
        {"F16C", (ecx1 & (1 << 29)) != 0},
        {"MOVBE", (ecx1 & (1 << 22)) != 0},
        {"BMI1", (ebx7 & (1 << 3)) != 0},
        {"BMI2", (ebx7 & (1 << 8)) != 0},
        {"LZCNT", (ecx81 & (1 << 5)) != 0},
    };
    std::string missing;
    for (const auto &n : need)
    {
      if (!n.present)
      {
        missing += missing.empty() ? n.name : std::string(" ") + n.name;
      }
    }
    std::printf("X86_64_V3=%d\nMISSING=%s\n", missing.empty() ? 1 : 0, missing.c_str());
    std::fflush(stdout);
    return 0;
  }

} // namespace

int wmain(int argc, wchar_t **argv)
{
  if (argc >= 2 && std::wcscmp(argv[1], L"cpu") == 0)
  {
    return Cpu();
  }
  if (argc >= 3 && std::wcscmp(argv[1], L"probe") == 0)
  {
    return Probe(argv[2]);
  }
  if (argc >= 4 && std::wcscmp(argv[1], L"extract") == 0)
  {
    std::string only;
    std::vector<std::string> skip;
    for (int i = 4; i < argc; i++)
    {
      std::string a = Utf8(fs::path(argv[i]));
      if (a.rfind("--only=", 0) == 0)
      {
        only = a.substr(7);
      }
      else if (a.rfind("--skip=", 0) == 0)
      {
        skip.push_back(a.substr(7));
      }
    }
    return Extract(argv[2], argv[3], only, skip);
  }
  std::printf("usage: ao2_install probe <source> | extract <source> <dest> [--only=<path>] [--skip=<name>] | cpu\n");
  return 2;
}
