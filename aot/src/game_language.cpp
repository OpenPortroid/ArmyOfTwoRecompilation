// game_language.cpp - The game's text in Russian and Brazilian Portuguese.
//
// The disc has neither. The game (UE3) reads its text from
// AO2Game\Localization\Coalesced.<lang>: a big-endian count of strings, then
// (file name, file text) pairs, each an int32 length (with the terminating
// zero; negative = UTF-16 characters) and the characters. In our languages
// the English Coalesced.int gets the text of localization\<dir>\*.int (rus,
// ptb; UTF-8, the English files' lines with the values translated): every
// Key=Value of the English .int whose section and key (and how many times the
// key came before in the section) our file has takes our value, in the
// language's code page. The fonts have Latin-1: Portuguese is in it (CP1252),
// Russian is CP1251 and cyrillic_fonts.cpp draws its letters where À-ÿ are.
// Xbox360TOC.txt gives every file's size and the engine reads that many
// bytes: our Coalesced.int is padded to the English one's size with empty
// lines; when it is over, the editor's .int files, which the game never
// reads, make room. The downloadable content's levels (09 Kiev, 10 Miami
// Alternate Ending) have no <package>.int, so their subtitles come from the
// packages in English: localization\<dir>\dlc\09_sound_vo_main.int and the
// like go into Coalesced whole.
//
// The downloadable content has whole copies of some of the files
// (DLC<n>__AO2Game.int, ...) that the game reads over the disc's: those get
// our text too (plus localization\<dir>\dlc\, the strings only they have),
// written to an overlay folder the SDK mounts in place of the package's own
// files (content_overlay_root).
//
// The result, with keyboard_prompts' rewrite, is mounted over
// game:\AO2Game\Localization (the VFS picks the longest matching mount path).
// The voices stay English; the language the game believes it runs in (int)
// does not change, so it keeps the English packages.
//
// The disc's own languages (fr, de, es, it) need nothing of that: the game
// runs in the console's language. Its statically linked XGetLanguage
// (sub_82B06730) reads XConfig user setting 9, which the SDK answers with the
// cvar user_language; the language extension (sub_82A9B620: INT, DEU, FRA,
// ESN, ITA by that number) then picks Coalesced.<ext> and the packages with
// _<EXT> (the voices, localized textures) over the English ones.
#include "game_language.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "cyrillic_fonts.h"
#include "dlc_folder.h"
#include "keyboard_prompts.h"
#include "utils.h"

REXCVAR_DEFINE_STRING(ao2_game_language, "auto", "General",
                      "Language of the game: auto (Windows' language when the game has it, else English), en, "
                      "ru (Russian text, English voices), pt (Brazilian Portuguese text, English voices), fr, de, "
                      "es, it (the disc's, text and voices). Co-op partners need the same voices: en, ru and pt "
                      "go together, the others only with themselves.");

namespace ao2::lang
{
  namespace
  {
    namespace fs = std::filesystem;

    constexpr const char *kMount = "\\Device\\Harddisk0\\Partition1\\AO2Game\\Localization";

    bool ReadFile(const fs::path &path, std::string &data)
    {
      std::ifstream in(path, std::ios::binary);
      if (!in)
        return false;
      data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      return true;
    }

    std::string ToLower(std::string s)
    {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      return s;
    }

    std::string Trim(std::string_view s)
    {
      while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
      while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
      return std::string(s);
    }

    // The languages with our text on the English game.
    struct OwnText
    {
      const char *code, *dir, *name;
      UINT codepage;
    };
    constexpr OwnText kOwnText[] = {
        {"ru", "rus", "Russian", 1251},
        {"pt", "ptb", "Brazilian Portuguese", 1252},
    };

    // UTF-8 to `codepage` ('?' for what it does not have).
    std::string ToCodePage(const std::string &utf8, UINT codepage)
    {
      int wide = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
      std::wstring w(size_t(wide), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), w.data(), wide);
      int narrow = WideCharToMultiByte(codepage, 0, w.data(), wide, nullptr, 0, "?", nullptr);
      std::string out(size_t(narrow), '\0');
      WideCharToMultiByte(codepage, 0, w.data(), wide, out.data(), narrow, "?", nullptr);
      return out;
    }

    // An .int's lines: (section, key, how many times the key came before in
    // the section) -> the line's value.
    using Key = std::tuple<std::string, std::string, int>;

    template <typename Fn> void ForEachLine(const std::string &text, Fn &&fn)
    {
      std::string section;
      std::map<std::pair<std::string, std::string>, int> seen;
      size_t pos = 0;
      while (pos <= text.size())
      {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        size_t eq = line.find('=');
        if (!line.empty() && line.front() == '[')
        {
          section = Trim(line);
          fn(line, nullptr, 0);
        }
        else if (line.empty() || line.front() == ';' || eq == std::string::npos)
        {
          fn(line, nullptr, 0);
        }
        else
        {
          std::string key = Trim(std::string_view(line).substr(0, eq));
          int n = seen[{section, key}]++;
          Key k{section, key, n};
          fn(line, &k, eq);
        }
      }
    }

    // Our values of an .int, in the language's code page:
    // localization\<dir>\<name>, then localization\<dir>\dlc\<name> (the
    // strings only the downloadable content's copy of the file has).
    std::map<Key, std::string> OwnValues(const fs::path &own_dir, UINT codepage, const std::string &name)
    {
      std::map<Key, std::string> values;
      for (const fs::path &path : {own_dir / name, own_dir / "dlc" / name})
      {
        std::string utf8;
        if (!ReadFile(path, utf8))
          continue;
        ForEachLine(ToCodePage(utf8, codepage), [&](const std::string &line, const Key *key, size_t eq) {
          if (key)
          {
            std::string value = line.substr(eq + 1);
            if (!value.empty() && value.back() == '\r')
              value.pop_back();
            values[*key] = std::move(value);
          }
        });
      }
      return values;
    }

    // The English .int with our values.
    std::string Merge(const std::string &english, const std::map<Key, std::string> &values, int &translated)
    {
      std::string out;
      out.reserve(english.size() + english.size() / 4);
      bool first = true;
      ForEachLine(english, [&](const std::string &line, const Key *key, size_t eq) {
        if (!first)
          out += '\n';
        first = false;
        auto it = key ? values.find(*key) : values.end();
        if (it == values.end())
        {
          out += line;
          return;
        }
        out.append(line, 0, eq + 1);
        out += it->second;
        if (!line.empty() && line.back() == '\r')
          out += '\r';
        ++translated;
      });
      return out;
    }

    uint32_t ReadBe32(const std::string &d, size_t at)
    {
      uint32_t v;
      std::memcpy(&v, d.data() + at, 4);
      return std::byteswap(v);
    }

    void AppendBe32(std::string &d, uint32_t v)
    {
      v = std::byteswap(v);
      d.append(reinterpret_cast<const char *>(&v), 4);
    }

    // Coalesced.int with our text; `data` itself when something is off.
    std::string OwnCoalesced(const std::string &data, const OwnText &own, const fs::path &own_dir)
    {
      struct Entry
      {
        int32_t length; // as stored (negative: UTF-16)
        std::string bytes;
      };
      std::vector<Entry> entries;
      if (data.size() < 4)
        return data;
      uint32_t count = ReadBe32(data, 0);
      size_t at = 4;
      for (uint32_t i = 0; i < count; ++i)
      {
        if (at + 4 > data.size())
          return data;
        int32_t n = int32_t(ReadBe32(data, at));
        size_t bytes = n < 0 ? size_t(-int64_t(n)) * 2 : size_t(n);
        if (at + 4 + bytes > data.size())
          return data;
        entries.push_back({n, data.substr(at + 4, bytes)});
        at += 4 + bytes;
      }
      if (at != data.size())
        return data;

      int files = 0, translated = 0;
      size_t last = 0;
      for (size_t i = 1; i < entries.size(); i += 2)
      {
        const Entry &name = entries[i - 1];
        Entry &text = entries[i];
        if (name.length <= 1 || text.length <= 1)
          continue;
        std::string path(name.bytes.data(), name.bytes.size() - 1);
        if (ToLower(path).find("\\ao2game\\localization\\") == std::string::npos)
          continue;
        std::map<Key, std::string> values =
            OwnValues(own_dir, own.codepage, path.substr(path.find_last_of('\\') + 1));
        if (values.empty())
          continue;
        std::string merged = Merge(text.bytes.substr(0, text.bytes.size() - 1), values, translated);
        merged += '\0';
        text.bytes = std::move(merged);
        text.length = int32_t(text.bytes.size());
        last = i;
        ++files;
      }
      if (!files)
      {
        REXLOG_WARN("game_language: no {} text in {}", own.name, own_dir.string());
        return data;
      }

      // The downloadable content's levels have no .int (their packages keep
      // the English subtitles, which the game takes when Coalesced has no
      // <package>.int): the files of localization\<dir>\dlc\ that Coalesced
      // lacks (09_sound_vo_main.int, ...) go in whole.
      auto base_name = [](const Entry &name) {
        std::string path(name.bytes.data(), name.bytes.size() - 1);
        return ToLower(path.substr(path.find_last_of('\\') + 1));
      };
      std::vector<std::string> present;
      for (size_t i = 0; i < entries.size(); i += 2)
        if (entries[i].length > 1)
          present.push_back(base_name(entries[i]));
      int added = 0;
      std::error_code ec;
      for (const auto &file : fs::directory_iterator(own_dir / "dlc", ec))
      {
        std::string name = file.path().filename().string();
        std::string lower = ToLower(name);
        std::string utf8;
        if (!lower.ends_with(".int") || std::find(present.begin(), present.end(), lower) != present.end() ||
            !ReadFile(file.path(), utf8))
          continue;
        std::string text;
        for (char c : ToCodePage(utf8, own.codepage))
          if (c != '\r')
            text += c;
        text += '\0';
        std::string path = "..\\AO2Game\\Localization\\INT\\" + name + '\0';
        entries.push_back({int32_t(path.size()), std::move(path)});
        entries.push_back({int32_t(text.size()), std::move(text)});
        ++added;
      }
      count = uint32_t(entries.size());

      auto serialize = [&] {
        std::string out;
        out.reserve(data.size());
        AppendBe32(out, count);
        for (const Entry &e : entries)
        {
          AppendBe32(out, uint32_t(e.length));
          out += e.bytes;
        }
        return out;
      };
      std::string out = serialize();
      // Room: the editor's text, which the game never reads.
      for (const char *editor : {"unrealed.int", "editor.int", "editortips.int"})
      {
        if (out.size() <= data.size())
          break;
        for (size_t i = 0; i < entries.size(); i += 2)
          if (entries[i].length > 1 && base_name(entries[i]) == editor)
          {
            entries.erase(entries.begin() + ptrdiff_t(i), entries.begin() + ptrdiff_t(i) + 2);
            if (last > i)
              last -= 2;
            break;
          }
        count = uint32_t(entries.size());
        out = serialize();
      }
      if (added)
        REXLOG_INFO("game_language: {} {} files of the downloadable content added", added, own.name);
      if (out.size() > data.size())
      {
        REXLOG_WARN("game_language: the {} text is {} bytes over the English, kept English", own.name,
                    out.size() - data.size());
        return data;
      }
      // Padded with empty lines in the last translated file.
      Entry &pad = entries[last];
      pad.bytes.insert(pad.bytes.size() - 1, data.size() - out.size(), '\n');
      pad.length = int32_t(pad.bytes.size());
      out = serialize();
      REXLOG_INFO("game_language: {} text, {} strings in {} files", own.name, translated, files);
      return out;
    }

    // The downloadable content's text (DLC<n>__<name>.int in a package folder:
    // the whole file, which the game reads over the disc's) in our language, under
    // overlay_root\<package folder>; the SDK mounts a package with the files of
    // its overlay folder in place of its own (content_overlay_root).
    int OwnContent(const fs::path &content_dir, const fs::path &overlay_root, const OwnText &own,
                   const fs::path &own_dir)
    {
      int files = 0, translated = 0;
      std::error_code ec;
      for (auto it = fs::recursive_directory_iterator(content_dir, fs::directory_options::skip_permission_denied, ec);
           it != fs::recursive_directory_iterator(); it.increment(ec))
      {
        if (ec)
          break;
        if (it->is_directory(ec))
        {
          // Packages sit at <content_dir>\<package> (ao2::dlc::Dir()).
          if (it.depth() >= 1)
            it.disable_recursion_pending();
          continue;
        }
        if (!it->is_regular_file(ec))
          continue;
        std::string name = it->path().filename().string();
        std::string lower = ToLower(name);
        size_t sep = name.find("__");
        if (!lower.starts_with("dlc") || !lower.ends_with(".int") || sep == std::string::npos)
          continue;
        std::map<Key, std::string> values = OwnValues(own_dir, own.codepage, name.substr(sep + 2));
        std::string english;
        if (values.empty() || !ReadFile(it->path(), english))
          continue;
        std::string merged = Merge(english, values, translated);
        fs::path out_dir = overlay_root / it->path().parent_path().filename();
        fs::create_directories(out_dir, ec);
        fs::path out = out_dir / name;
        std::string existing;
        if (!(ReadFile(out, existing) && existing == merged))
        {
          std::ofstream file(out, std::ios::binary | std::ios::trunc);
          file.write(merged.data(), std::streamsize(merged.size()));
        }
        ++files;
      }
      if (files)
        REXLOG_INFO("game_language: {} downloadable content text, {} strings in {} files", own.name, translated,
                    files);
      return files;
    }
  } // namespace

  const char *Code()
  {
    // The names the cvar takes for each language.
    struct Language
    {
      std::initializer_list<const char *> names;
      const char *code;
    };
    static const Language kLanguages[] = {
        {{"en", "int", "eng", "english"}, "en"},          {{"ru", "rus", "russian"}, "ru"},
        {{"pt", "ptb", "pt-br", "pt_br", "portuguese", "portugues", "brazilian"}, "pt"},
        {{"fr", "fra", "french", "francais"}, "fr"},       {{"de", "deu", "ger", "german", "deutsch"}, "de"},
        {{"es", "esn", "spa", "spanish", "espanol"}, "es"}, {{"it", "ita", "italian", "italiano"}, "it"},
    };
    std::string value = ToLower(REXCVAR_GET(ao2_game_language));
    for (const Language &language : kLanguages)
      for (const char *name : language.names)
        if (value == name)
          return language.code;
    if (value != "auto")
      REXLOG_WARN("game_language: unknown ao2_game_language '{}', auto", value);
    switch (PRIMARYLANGID(GetUserDefaultUILanguage()))
    {
    case LANG_RUSSIAN:
      return "ru";
    case LANG_PORTUGUESE:
      return "pt";
    case LANG_FRENCH:
      return "fr";
    case LANG_GERMAN:
      return "de";
    case LANG_SPANISH:
      return "es";
    case LANG_ITALIAN:
      return "it";
    default:
      return "en";
    }
  }

  bool Russian() { return std::strcmp(Code(), "ru") == 0; }

  bool OwnLanguage()
  {
    for (const OwnText &own : kOwnText)
      if (std::strcmp(Code(), own.code) == 0)
        return true;
    return false;
  }

  void Install(rex::Runtime *runtime, const fs::path &game_data_root, const fs::path &user_data_root)
  {
    // The console's language (XLanguage: 1 English, 3 German, 4 French,
    // 5 Spanish, 6 Italian); our languages run on English.
    static const std::pair<const char *, int> kConsole[] = {{"fr", 4}, {"de", 3}, {"es", 5}, {"it", 6}};
    int console = 1;
    for (const auto &[code, id] : kConsole)
      if (std::strcmp(Code(), code) == 0)
        console = id;
    rex::cvar::SetFlagByName("user_language", std::to_string(console));
    REXLOG_INFO("game_language: {} (console language {})", Code(), console);

    const OwnText *own = nullptr;
    for (const OwnText &t : kOwnText)
      if (std::strcmp(Code(), t.code) == 0)
        own = &t;
    bool prompts = ao2::prompts::Enabled();
    if ((!own && !prompts) || !runtime || !runtime->file_system())
      return;
    if (Russian())
      ao2::cyrillic::Install(game_data_root, user_data_root);
    fs::path source = game_data_root / "AO2Game" / "Localization";
    fs::path target = user_data_root / "localization" / "Localization";
    fs::path own_dir = utils::RepoRoot() / "localization" / (own ? own->dir : "");
    std::error_code ec;
    if (!fs::is_directory(source, ec))
      return;
    fs::create_directories(target, ec);

    // The overlay replaces the whole folder: every file of it is there.
    int rewritten = 0;
    for (const auto &entry : fs::directory_iterator(source, ec))
    {
      if (!entry.is_regular_file())
        continue;
      std::string data;
      if (!ReadFile(entry.path(), data))
      {
        REXLOG_WARN("game_language: cannot read {}", entry.path().string());
        return;
      }
      std::string name = entry.path().filename().string();
      std::string lower = ToLower(name);
      bool english = lower == "coalesced.int";
      if (english && own)
        data = OwnCoalesced(data, *own, own_dir);
      if (prompts && lower.starts_with("coalesced."))
        data = ao2::prompts::RewriteLocalization(data, english && !own, rewritten);

      fs::path out = target / name;
      std::string existing;
      if (ReadFile(out, existing) && existing == data)
        continue;
      std::ofstream file(out, std::ios::binary | std::ios::trunc);
      file.write(data.data(), std::streamsize(data.size()));
      if (!file)
      {
        REXLOG_WARN("game_language: cannot write {}", out.string());
        return;
      }
    }

    if (own)
    {
      fs::path overlays = user_data_root / "localization" / "content";
      if (OwnContent(ao2::dlc::Dir(), overlays, *own, own_dir))
        rex::cvar::SetFlagByName("content_overlay_root", overlays.string());
    }

    auto device = std::make_unique<rex::filesystem::HostPathDevice>(kMount, target, true);
    if (!device->Initialize() || !runtime->file_system()->RegisterDevice(std::move(device)))
    {
      REXLOG_WARN("game_language: cannot mount {}", target.string());
      return;
    }
    REXLOG_INFO("game_language: {}, {} strings without the controller's words ({})", own ? own->name : "English",
                rewritten, target.string());
  }
} // namespace ao2::lang
