// graphics_menu.cpp - A Graphics page in the game's own Options screen.
//
// The Options screen (AO2UISkin.AllOptionsUIScene) is one page: preferred
// character, invert Y, vibration, aim sensitivity, fading health / ammo,
// subtitles, TV type (UIOptionButtons bound to <OnlinePlayerData:ProfileData.*>),
// brightness (a UISlider) and "Select Storage Device" (a UILabelButton whose
// Kismet runs UIAction_ShowDeviceSelectionUI). The storage device means nothing
// on PC, so that button is captioned "Graphics" and opens the Graphics page
// instead: the online subsystem's ShowDeviceSelectionUI (sub_82AF8380) is
// caught while the Options screen is the top scene, and a second instance of
// the same scene is opened over it (the scene client's OpenScene, vtable
// +316). Its rows become graphics settings; the rest (TV type, brightness,
// the device button) is hidden. Everything else is the game's own: fonts,
// sounds, focus, the button bar, A accepting and B closing the page back to
// Options.
//
// The rows' option buttons keep their bindings; for them the UIOptionButton
// natives answer from the table below instead of the data provider (count
// sub_8233B598, HasPrev/HasNext sub_8233B680/sub_8233B710, Prev/Next
// sub_8235AC38/sub_8235ACF8, SetCurrentIndex sub_82353FC8, the value's text
// sub_82353980, RefreshSubscriberValue sub_82353A78, SaveSubscriberValue
// sub_8235ADD8): the profile is never read or written through them and their
// OptionChanged Kismet events (console commands) never fire. A button shows
// its value as its caption (IUIDataStorePublisher at +848, vtable +4).
//
// The graphics instance is told apart by its SceneTag number (kGraphicsTag):
// the scene client would otherwise find it by tag as the Options screen. The
// Options screen's tag is changed during the open for the same reason.
//
// Most changes apply at once (fullscreen, frame rate cap, vertical sync, FPS
// counter, anisotropy). The render resolution waits for the page's
// Accept, which publishes every widget's value (SaveSubscriberValue): the
// native renderer then switches at the end of a frame
// (RendererSetResolutionScale; the cvar stays as the game started, the
// fonts' pages were made for it); a pick left with B is dropped. The aspect
// ratio needs a restart (the game's D3D device and its buffers are created
// with it), as does the resolution without the native renderer; the row's
// label says what is pending. Every change is saved to settings/hardware.toml and, when the
// launcher started the game (ao2_launcher_settings), to the launcher's own
// settings file, whose flags would otherwise win at the next start.
#include "graphics_menu.h"

#include "game_language.h"
#include "gpu/native/renderer.h"
#include "log_files.h"
#include "ue3_reflect.h"
#include "ui_text.h"
#include "utils.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_STRING(ao2_launcher_settings, "", "General",
                      "The launcher's settings file (the launcher passes it): the in-game Graphics page saves "
                      "its changes there too");


namespace
{
  // UObject / UI layout (TU2; tools/ue3class.py).
  constexpr uint32_t kOuter = 40;
  constexpr uint32_t kName = 44;
  constexpr uint32_t kNameNumber = 48;
  constexpr uint32_t kHiddenWord = 92;   // UIScreenObject.bHidden
  constexpr uint32_t kHiddenMask = 0x80000000;
  constexpr uint32_t kChildren = 96;     // UIScreenObject.Children (TArray)
  constexpr uint32_t kOwnerScene = 368;  // UIObject.OwnerScene
  constexpr uint32_t kSceneTag = 340;    // UIScene.SceneTag (FName: index, number)
  constexpr uint32_t kSceneClient = 348; // UIScene.SceneClient
  constexpr uint32_t kPlayerOwner = 356; // UIScene.PlayerOwner
  constexpr uint32_t kSceneFlags = 412;  // UIScene bitfield
  constexpr uint32_t kUpdateNavigation = 0x20000000; // bUpdateNavigationLinks
  // UIObject.NavigationTargets (+404): NavigationTarget[4], then
  // ForcedNavigationTarget[4]; faces left, top, right, bottom.
  constexpr uint32_t kForcedNavigation = 404 + 16;
  constexpr uint32_t kFaceTop = 1;
  constexpr uint32_t kFaceBottom = 3;
  constexpr uint32_t kActiveScenes = 0xC0;           // GameUISceneClient.ActiveScenes (TArray)
  constexpr uint32_t kOpenScene = 316;               // scene client vtable: OpenScene(scene, owner, &opened, priority)
  constexpr uint32_t kLabelMarkup = 836;             // UILabel.DataSource.MarkupString
  constexpr uint32_t kPublisher = 848;               // UILabelButton: IUIDataStorePublisher
  constexpr uint32_t kCaptionMarkup = 864;           // UILabelButton.CaptionDataSource.MarkupString
  constexpr uint32_t kCurrentIndex = 972;            // UIOptionButton.CurrentIndex
  constexpr uint32_t kWrapWord = 1072;               // UIOptionButton.bWrapOptions
  constexpr uint32_t kWrapMask = 0x80000000;
  constexpr uint32_t kGraphicsTag = 0x47465831;      // SceneTag number of the Graphics instance
  constexpr uint32_t kOpeningTag = 0x47465832;       // the Options screen's while it opens

  constexpr uint32_t kFStringPrintf = 0x8242A860; // (FString *out, const char *format, ...)
  constexpr uint32_t kFStringFree = 0x82309A60;   // FString destructor
  constexpr uint32_t kFStringAssign = 0x82302D08; // FString::operator=(this, const FString *)
  constexpr uint32_t kLabelSetValue = 0x82974648; // UILabel::SetValue(this, const FString *)

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("graphics_menu.log");
    return f;
  }

  template <typename... Args>
  void Logf(const char *format, Args... args)
  {
    if (FILE *f = Log())
    {
      std::fprintf(f, format, args...);
      std::fflush(f);
    }
  }

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }
  void StoreU32(uint8_t *base, uint32_t addr, uint32_t value)
  {
    value = std::byteswap(value);
    std::memcpy(base + addr, &value, 4);
  }

  uint32_t CallGuest(PPCContext &ctx, uint8_t *base, uint32_t fn, uint32_t stack_top, uint64_t r3, uint64_t r4 = 0,
                     uint64_t r5 = 0, uint64_t r6 = 0, uint64_t r7 = 0)
  {
    PPCContext saved = ctx;
    ctx.r1.u64 = stack_top;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    ctx.r7.u64 = r7;
    rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
    uint32_t result = ctx.r3.u32;
    ctx = saved;
    return result;
  }

  // Scratch guest memory below the caller's frame: the callee's stack at
  // `top`, our data above its parameter save area.
  struct Scratch
  {
    uint32_t top;
    explicit Scratch(const PPCContext &ctx) : top((ctx.r1.u32 - 2048) & ~15u) {}
    uint32_t String(int i) const { return top + 256 + i * 16; } // FStrings {data, count, max}
    uint32_t Text() const { return top + 512; }                // up to 1 KB of text
    uint32_t Slot() const { return top + 320; }                // a pointer out-parameter
  };

  // UTF-8 -> the game's 8-bit text: CP1251 in Russian (game_language.cpp),
  // Latin-1 otherwise. No '%': the text is a Printf format.
  std::string GameText(const char *utf8)
  {
    bool russian = ao2::lang::Russian();
    std::string out;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(utf8); *p;)
    {
      uint32_t c = *p++;
      if (c >= 0xC0)
      {
        int extra = c >= 0xF0 ? 3 : (c >= 0xE0 ? 2 : 1);
        c &= 0x3F >> extra;
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; ++i)
        {
          c = (c << 6) | (*p++ & 0x3F);
        }
      }
      char ch = '?';
      if (c < 0x80)
      {
        ch = c == '%' ? ' ' : static_cast<char>(c);
      }
      else if (russian && c >= 0x410 && c <= 0x44F)
      {
        ch = static_cast<char>(0xC0 + (c - 0x410));
      }
      else if (russian && (c == 0xAB || c == 0xBB))
      {
        ch = static_cast<char>(c); // « » are the same bytes in CP1251
      }
      else if (russian && (c == 0x401 || c == 0x451))
      {
        ch = static_cast<char>(c == 0x401 ? 0xA8 : 0xB8);
      }
      else if (!russian && c < 0x100)
      {
        ch = static_cast<char>(c);
      }
      else if (c == 0xD7)
      {
        ch = 'x';
      }
      out += ch;
    }
    return out;
  }

  // `english` translated to the language of the game's own text (not the
  // port menus' ao2_ui_language: this is drawn in the game's fonts), as bytes.
  std::string GameTr(const char *english)
  {
    return GameText(ao2::text::TrIn(ao2::text::Game(), english));
  }

  // An FString holding `text` at `string` (constructed there).
  void MakeString(PPCContext &ctx, uint8_t *base, const Scratch &s, uint32_t string, const std::string &text)
  {
    size_t n = std::min<size_t>(text.size(), 1000);
    std::memcpy(base + s.Text(), text.data(), n);
    base[s.Text() + n] = 0;
    std::memset(base + string, 0, 12);
    CallGuest(ctx, base, kFStringPrintf, s.top, string, s.Text());
  }

  // --- The settings -------------------------------------------------------

  struct Choice
  {
    const char *name; // English, through Tr
    const char *value;
  };

  enum class When
  {
    kNow,     // as soon as it is picked
    kAccept,  // when the page is accepted (A), dropped with B
    kRestart, // at the next start (saved at once)
  };

  struct Setting
  {
    const char *label;         // English, through Tr
    const char *label_pending; // while a change waits for Accept or a restart, or nullptr
    When when;
    const char *cvar;
    const char *toml;          // hardware.toml value: "s" string, "b" bool, "n" number
    const char *launcher_key;  // the launcher's key, nullptr if it has none
    std::vector<Choice> choices;
    int running = 0;           // the choice the game runs with
    int chosen = 0;            // the one picked
    bool restart = false;      // accepted, but only a restart applies it
  };

  std::vector<Setting> &Settings()
  {
    static std::vector<Setting> settings = {
        {"Display mode", nullptr, When::kNow, "fullscreen", "b", "fullscreen",
         {{"Fullscreen", "true"}, {"Windowed", "false"}}},
        {"Render resolution", "Resolution: applies on Accept", When::kAccept, "ao2_native_gpu_resolution_scale", "n",
         "render_scale",
         {{"720p", "1"}, {"900p", "1.25"}, {"1080p", "1.5"}, {"1440p", "2"}, {"1800p", "2.5"}, {"4K", "3"},
          {"5K", "4"}}},
        {"Aspect ratio", "Aspect ratio: after restart", When::kRestart, "ao2_aspect_ratio", "s", "aspect_ratio",
         {{"Auto", "auto"}, {"5:4", "5:4"}, {"4:3", "4:3"}, {"3:2", "3:2"}, {"16:10", "16:10"}, {"16:9", "16:9"},
          {"2:1", "2:1"}, {"21:9", "21:9"}, {"32:9", "32:9"}}},
        {"FPS limit", nullptr, When::kNow, "ao2_fps_target", "n", "fps",
         {{"30", "30"}, {"60", "60"}, {"75", "75"}, {"90", "90"}, {"120", "120"}, {"144", "144"}, {"165", "165"},
          {"240", "240"}}},
        {"Vertical sync", nullptr, When::kNow, "ao2_native_gpu_vsync", "b", "vsync",
         {{"Off", "false"}, {"On", "true"}}},
        {"FPS counter", nullptr, When::kNow, "ao2_fps_overlay", "n", "fps_overlay",
         {{"Off", "0"}, {"FPS", "1"}, {"Detailed", "2"}}},
        {"Anisotropic filtering", nullptr, When::kNow, "ao2_anisotropic_16x", "b", "aniso",
         {{"Off", "false"}, {"16x", "true"}}},
    };
    return settings;
  }

  // The choice nearest to the cvar's current value.
  int CurrentChoice(const Setting &s)
  {
    std::string v = rex::cvar::GetFlagByName(s.cvar);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
    int best = 0;
    double best_d = 1e30;
    for (size_t i = 0; i < s.choices.size(); ++i)
    {
      std::string c = s.choices[i].value;
      if (c == v || (c == "true" && v == "1") || (c == "false" && v == "0"))
      {
        return static_cast<int>(i);
      }
      char *end_v = nullptr, *end_c = nullptr;
      double dv = std::strtod(v.c_str(), &end_v), dc = std::strtod(c.c_str(), &end_c);
      if (end_v != v.c_str() && end_c != c.c_str() && std::fabs(dv - dc) < best_d)
      {
        best_d = std::fabs(dv - dc);
        best = static_cast<int>(i);
      }
    }
    return best;
  }

  void InitSettings()
  {
    static bool done = false;
    if (done)
    {
      return;
    }
    done = true;
    for (Setting &s : Settings())
    {
      s.running = s.chosen = CurrentChoice(s);
    }
  }

  const char *LabelOf(const Setting &s)
  {
    if (s.restart && s.when == When::kAccept)
    {
      return "Resolution: after restart";
    }
    return s.label_pending && s.chosen != s.running ? s.label_pending : s.label;
  }

  // --- Saving ---------------------------------------------------------------

  // Sets `key = value` in a flat key = value file (the line replaced, or added).
  void SetFileKey(const std::filesystem::path &path, const std::string &key, const std::string &value)
  {
    std::vector<std::string> lines;
    {
      std::ifstream in(path, std::ios::binary);
      std::string line;
      while (std::getline(in, line))
      {
        if (!line.empty() && line.back() == '\r')
        {
          line.pop_back();
        }
        lines.push_back(line);
      }
    }
    bool found = false;
    for (std::string &line : lines)
    {
      size_t start = line.find_first_not_of(" \t");
      if (start == std::string::npos || line.compare(start, key.size(), key) != 0)
      {
        continue;
      }
      size_t after = line.find_first_not_of(" \t", start + key.size());
      if (after != std::string::npos && line[after] == '=')
      {
        line = key + " = " + value;
        found = true;
      }
    }
    if (!found)
    {
      lines.push_back(key + " = " + value);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (const std::string &line : lines)
    {
      out << line << "\r\n";
    }
  }

  void Save(const Setting &s)
  {
    std::string value = s.choices[s.chosen].value;
    std::string toml = value;
    if (s.toml[0] == 's')
    {
      toml = "\"" + value + "\"";
    }
    try
    {
      SetFileKey(utils::SettingsDir() / "hardware.toml", s.cvar, toml);
      if (std::string(s.cvar) == "ao2_fps_target")
      {
        SetFileKey(utils::SettingsDir() / "hardware.toml", "ao2_fps_unlock", "true");
      }
      std::string launcher = REXCVAR_GET(ao2_launcher_settings);
      if (s.launcher_key && !launcher.empty() && std::filesystem::exists(std::u8string(launcher.begin(), launcher.end())))
      {
        std::string v = value == "true" ? "1" : (value == "false" ? "0" : value);
        SetFileKey(std::filesystem::path(std::u8string(launcher.begin(), launcher.end())), s.launcher_key, v);
      }
    }
    catch (const std::exception &e)
    {
      Logf("save %s: %s\n", s.cvar, e.what());
    }
  }

  rex::ui::Window *g_window = nullptr;

  // A change of a kNow setting: the game takes it at once.
  void Apply(Setting &s)
  {
    Save(s);
    s.running = s.chosen;
    std::string value = s.choices[s.chosen].value;
    bool ok = rex::cvar::SetFlagByName(s.cvar, value);
    Logf("%s = %s%s\n", s.cvar, value.c_str(), ok ? "" : " (not set)");
    if (std::string(s.cvar) == "fullscreen" && g_window)
    {
      bool fullscreen = value == "true";
      rex::ui::Window *window = g_window;
      window->app_context().CallInUIThreadDeferred([window, fullscreen]() { window->SetFullscreen(fullscreen); });
    }
  }

  // The page was accepted: a kAccept setting (the render resolution) takes
  // its pick. The native renderer switches at the end of a frame; without it
  // (another GPU backend) only a restart does. The cvar keeps its start value
  // either way: the fonts' pages were made for it.
  void Accept(Setting &s)
  {
    if (s.chosen == s.running)
    {
      return;
    }
    Save(s);
    double scale = std::strtod(s.choices[s.chosen].value, nullptr);
    if (ao2::gpu::RendererSetResolutionScale(scale))
    {
      s.running = s.chosen;
      s.restart = false;
      Logf("accepted: resolution scale %.2f\n", scale);
    }
    else
    {
      s.restart = true;
      Logf("accepted: resolution scale %.2f at the next start\n", scale);
    }
  }

  // --- The scenes -----------------------------------------------------------

  uint32_t g_options_scene = 0;   // the Options screen last seen
  uint32_t g_graphics_scene = 0;  // our instance
  bool g_opening = false;
  struct Row
  {
    int setting;
    uint32_t label;
  };
  std::unordered_map<uint32_t, Row> g_rows; // option button -> its row

  bool IsSceneNamed(const uint8_t *base, uint32_t scene)
  {
    return scene && ao2::ue3::ObjectName(base, scene).rfind("AllOptionsUIScene", 0) == 0 &&
           ao2::ue3::Name(base, LoadU32(base, scene + kSceneTag)) == "AllOptionsUIScene";
  }

  bool IsGraphicsScene(const uint8_t *base, uint32_t scene)
  {
    return scene && scene == g_graphics_scene && IsSceneNamed(base, scene) &&
           LoadU32(base, scene + kSceneTag + 4) == kGraphicsTag;
  }

  // The row of an option button of the Graphics page, nullptr if it is none.
  const Row *RowOf(const uint8_t *base, uint32_t button)
  {
    if (g_rows.empty())
    {
      return nullptr;
    }
    auto it = g_rows.find(button);
    if (it == g_rows.end() || !IsGraphicsScene(base, LoadU32(base, button + kOwnerScene)))
    {
      return nullptr;
    }
    return &it->second;
  }

  // The scene on top of the scene client's stack.
  uint32_t TopScene(const uint8_t *base, uint32_t client)
  {
    uint32_t data = LoadU32(base, client + kActiveScenes);
    uint32_t count = LoadU32(base, client + kActiveScenes + 4);
    return data && count && count < 64 ? LoadU32(base, data + (count - 1) * 4) : 0;
  }

  // The scene template AO2UISkin.AllOptionsUIScene.
  uint32_t Template(const uint8_t *base)
  {
    static uint32_t cached = 0;
    auto is_template = [base](uint32_t o) {
      if (!o || ao2::ue3::ObjectName(base, o) != "AllOptionsUIScene")
      {
        return false;
      }
      uint32_t outer = LoadU32(base, o + kOuter);
      return outer && ao2::ue3::ObjectName(base, outer) == "AO2UISkin";
    };
    if (is_template(cached))
    {
      return cached;
    }
    cached = 0;
    uint32_t count = ao2::ue3::ObjectCount(base);
    for (uint32_t i = 0; i < count; ++i)
    {
      uint32_t o = ao2::ue3::ObjectAt(base, i);
      if (is_template(o))
      {
        cached = o;
        break;
      }
    }
    return cached;
  }

  void Widgets(const uint8_t *base, uint32_t object, std::unordered_map<std::string, uint32_t> &out, int depth = 0)
  {
    uint32_t data = LoadU32(base, object + kChildren);
    uint32_t count = LoadU32(base, object + kChildren + 4);
    for (uint32_t i = 0; data && i < count && i < 256 && depth < 8; ++i)
    {
      uint32_t child = LoadU32(base, data + i * 4);
      if (child)
      {
        out[ao2::ue3::ObjectName(base, child)] = child;
        Widgets(base, child, out, depth + 1);
      }
    }
  }

  void SetLabel(PPCContext &ctx, uint8_t *base, uint32_t label, const char *english)
  {
    Scratch s(ctx);
    std::string text = GameTr(english);
    // The binding's markup too: a refresh would bring the old text back.
    CallGuest(ctx, base, kFStringFree, s.top, label + kLabelMarkup);
    MakeString(ctx, base, s, label + kLabelMarkup, text);
    MakeString(ctx, base, s, s.String(0), text);
    CallGuest(ctx, base, kLabelSetValue, s.top, label, s.String(0));
    CallGuest(ctx, base, kFStringFree, s.top, s.String(0));
  }

  void ShowIndex(PPCContext &ctx, uint8_t *base, uint32_t button, const Setting &setting, int index)
  {
    StoreU32(base, button + kCurrentIndex, static_cast<uint32_t>(index));
    Scratch s(ctx);
    MakeString(ctx, base, s, s.String(0), GameTr(setting.choices[index].name));
    uint32_t publisher = button + kPublisher;
    CallGuest(ctx, base, LoadU32(base, LoadU32(base, publisher) + 4), s.top, publisher, s.String(0),
              static_cast<uint32_t>(-1));
    CallGuest(ctx, base, kFStringFree, s.top, s.String(0));
  }

  // A label button's caption: its markup becomes the text (IUIDataStorePublisher
  // at +848, vtable +4, as the option buttons show their values).
  void SetCaption(PPCContext &ctx, uint8_t *base, uint32_t button, const char *english)
  {
    Scratch s(ctx);
    MakeString(ctx, base, s, s.String(0), GameTr(english));
    uint32_t publisher = button + kPublisher;
    CallGuest(ctx, base, LoadU32(base, LoadU32(base, publisher) + 4), s.top, publisher, s.String(0),
              static_cast<uint32_t>(-1));
    CallGuest(ctx, base, kFStringFree, s.top, s.String(0));
  }

  // The Options screen opened (each of its option buttons refreshes):
  // "Select Storage Device" becomes "Graphics", once - while its caption is
  // still the game's markup (<Strings:...SelectDevice>).
  void NoteOptionsScene(PPCContext &ctx, uint8_t *base, uint32_t scene)
  {
    g_options_scene = scene;
    std::unordered_map<std::string, uint32_t> w;
    Widgets(base, scene, w);
    uint32_t button = w.count("UILabelButton_0") ? w["UILabelButton_0"] : 0;
    uint32_t markup = button ? LoadU32(base, button + kCaptionMarkup) : 0;
    if (markup && LoadU32(base, button + kCaptionMarkup + 4) > 1 && base[markup] == '<')
    {
      SetCaption(ctx, base, button, "Graphics");
    }
  }

  void Choose(PPCContext &ctx, uint8_t *base, uint32_t button, const Row &row, int index)
  {
    Setting &setting = Settings()[row.setting];
    int count = static_cast<int>(setting.choices.size());
    if (index < 0 || index >= count)
    {
      return;
    }
    bool changed = index != setting.chosen;
    setting.chosen = index;
    ShowIndex(ctx, base, button, setting, index);
    if (!changed)
    {
      return;
    }
    if (setting.when == When::kNow)
    {
      Apply(setting);
    }
    else if (setting.when == When::kRestart)
    {
      Save(setting);
    }
    if (setting.when != When::kNow && row.label)
    {
      SetLabel(ctx, base, row.label, LabelOf(setting));
    }
  }

  // Turns a fresh instance of the Options screen into the Graphics page.
  void Setup(PPCContext &ctx, uint8_t *base, uint32_t scene)
  {
    std::unordered_map<std::string, uint32_t> w;
    Widgets(base, scene, w);
    // The rows top to bottom: option button, its label.
    static const char *const kRowWidgets[][2] = {
        {"optbtnPreferChar", "lblPreferredChar"}, {"UIOptionButton_1", "UILabel_1"},
        {"UIOptionButton_2", "UILabel_2"},        {"UIOptionButton_3", "UILabel_3"},
        {"UIOptionButton_5", "UILabel_7"},        {"UIOptionButton_4", "UILabel_0"},
        {"UIOptionButton_0", "UILabel_6"},        {"TVTypeOptionButton", "UILabel_4"},
    };
    static const char *const kHidden[] = {"BrightnessSlider", "UILabel_5", "UILabelButton_0"};

    g_rows.clear();
    auto &settings = Settings();
    for (size_t i = 0; i < std::size(kRowWidgets); ++i)
    {
      uint32_t button = w.count(kRowWidgets[i][0]) ? w[kRowWidgets[i][0]] : 0;
      uint32_t label = w.count(kRowWidgets[i][1]) ? w[kRowWidgets[i][1]] : 0;
      if (i >= settings.size())
      {
        for (uint32_t o : {button, label})
        {
          if (o)
          {
            StoreU32(base, o + kHiddenWord, LoadU32(base, o + kHiddenWord) | kHiddenMask);
          }
        }
        continue;
      }
      if (!button)
      {
        Logf("setup: no %s\n", kRowWidgets[i][0]);
        continue;
      }
      g_rows[button] = Row{static_cast<int>(i), label};
      Setting &s = settings[i];
      if (s.when == When::kAccept && !s.restart)
      {
        s.chosen = s.running; // a pick left with B is dropped
      }
      if (label)
      {
        SetLabel(ctx, base, label, LabelOf(s));
      }
      ShowIndex(ctx, base, button, s, s.chosen);
    }
    for (const char *name : kHidden)
    {
      if (w.count(name))
      {
        StoreU32(base, w[name] + kHiddenWord, LoadU32(base, w[name] + kHiddenWord) | kHiddenMask);
      }
    }
    if (w.count("lblOptions"))
    {
      SetLabel(ctx, base, w["lblOptions"], "Graphics");
    }
    // Down from the last row would go to the hidden ones (and stop there):
    // the last and the first row wrap to each other, as the full screen does.
    uint32_t first = w.count(kRowWidgets[0][0]) ? w[kRowWidgets[0][0]] : 0;
    size_t last_index = std::min(settings.size(), std::size(kRowWidgets)) - 1;
    uint32_t last = w.count(kRowWidgets[last_index][0]) ? w[kRowWidgets[last_index][0]] : 0;
    if (first && last && first != last)
    {
      StoreU32(base, last + kForcedNavigation + kFaceBottom * 4, first);
      StoreU32(base, first + kForcedNavigation + kFaceTop * 4, last);
    }
    StoreU32(base, scene + kSceneFlags, LoadU32(base, scene + kSceneFlags) | kUpdateNavigation);
    Logf("setup: scene %08X, %zu rows\n", scene, g_rows.size());
  }

  // "Select Storage Device" on the Options screen: opens the Graphics page.
  bool OpenGraphics(PPCContext &ctx, uint8_t *base)
  {
    uint32_t options = g_options_scene;
    if (!IsSceneNamed(base, options) || LoadU32(base, options + kSceneTag + 4) != 0)
    {
      return false;
    }
    uint32_t client = LoadU32(base, options + kSceneClient);
    if (!client || TopScene(base, client) != options)
    {
      return false;
    }
    uint32_t scene_template = Template(base);
    if (!scene_template)
    {
      Logf("open: no %s\n", "AO2UISkin.AllOptionsUIScene");
      return false;
    }
    InitSettings();
    Scratch s(ctx);
    StoreU32(base, s.Slot(), 0);
    StoreU32(base, options + kSceneTag + 4, kOpeningTag);
    g_opening = true;
    uint32_t open = LoadU32(base, LoadU32(base, client) + kOpenScene);
    uint32_t ok = CallGuest(ctx, base, open, s.top, client, scene_template, LoadU32(base, options + kPlayerOwner),
                            s.Slot(), 0);
    g_opening = false;
    StoreU32(base, options + kSceneTag + 4, 0);
    uint32_t scene = LoadU32(base, s.Slot());
    Logf("open: client %08X template %08X -> %u, scene %08X\n", client, scene_template, ok, scene);
    if (!scene || scene == options)
    {
      return scene == options; // already there: nothing more to open
    }
    StoreU32(base, scene + kSceneTag + 4, kGraphicsTag);
    g_graphics_scene = scene;
    Setup(ctx, base, scene);
    return true;
  }
} // namespace

namespace ao2::gfxmenu
{
  void Attach(rex::ui::Window *window) { g_window = window; }
} // namespace ao2::gfxmenu

// OnlineSubsystemLive::ShowDeviceSelectionUI(this, user, size needed, force).
extern "C" REX_FUNC(__imp__sub_82AF8380);
extern "C" REX_FUNC(sub_82AF8380)
{
  if (OpenGraphics(ctx, base))
  {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82AF8380(ctx, base);
}

// UIOptionButton: number of values.
extern "C" REX_FUNC(__imp__sub_8233B598);
extern "C" REX_FUNC(sub_8233B598)
{
  if (const Row *row = RowOf(base, ctx.r3.u32))
  {
    ctx.r3.u64 = Settings()[row->setting].choices.size();
    return;
  }
  __imp__sub_8233B598(ctx, base);
}

// UIOptionButton::HasPrevValue.
extern "C" REX_FUNC(__imp__sub_8233B680);
extern "C" REX_FUNC(sub_8233B680)
{
  uint32_t button = ctx.r3.u32;
  if (const Row *row = RowOf(base, button))
  {
    bool wrap = LoadU32(base, button + kWrapWord) & kWrapMask;
    int count = static_cast<int>(Settings()[row->setting].choices.size());
    int index = static_cast<int32_t>(LoadU32(base, button + kCurrentIndex));
    ctx.r3.u64 = wrap ? count > 1 : index > 0;
    return;
  }
  __imp__sub_8233B680(ctx, base);
}

// UIOptionButton::HasNextValue.
extern "C" REX_FUNC(__imp__sub_8233B710);
extern "C" REX_FUNC(sub_8233B710)
{
  uint32_t button = ctx.r3.u32;
  if (const Row *row = RowOf(base, button))
  {
    bool wrap = LoadU32(base, button + kWrapWord) & kWrapMask;
    int count = static_cast<int>(Settings()[row->setting].choices.size());
    int index = static_cast<int32_t>(LoadU32(base, button + kCurrentIndex));
    ctx.r3.u64 = wrap ? count > 1 : index < count - 1;
    return;
  }
  __imp__sub_8233B710(ctx, base);
}

// UIOptionButton::SetPrevValue.
extern "C" REX_FUNC(__imp__sub_8235AC38);
extern "C" REX_FUNC(sub_8235AC38)
{
  uint32_t button = ctx.r3.u32;
  if (const Row *row = RowOf(base, button))
  {
    Row r = *row;
    bool wrap = LoadU32(base, button + kWrapWord) & kWrapMask;
    int count = static_cast<int>(Settings()[r.setting].choices.size());
    int index = static_cast<int32_t>(LoadU32(base, button + kCurrentIndex));
    if (index > 0)
    {
      Choose(ctx, base, button, r, index - 1);
    }
    else if (wrap && count > 1)
    {
      Choose(ctx, base, button, r, count - 1);
    }
    return;
  }
  __imp__sub_8235AC38(ctx, base);
}

// UIOptionButton::SetNextValue.
extern "C" REX_FUNC(__imp__sub_8235ACF8);
extern "C" REX_FUNC(sub_8235ACF8)
{
  uint32_t button = ctx.r3.u32;
  if (const Row *row = RowOf(base, button))
  {
    Row r = *row;
    bool wrap = LoadU32(base, button + kWrapWord) & kWrapMask;
    int count = static_cast<int>(Settings()[r.setting].choices.size());
    int index = static_cast<int32_t>(LoadU32(base, button + kCurrentIndex));
    if (index < count - 1)
    {
      Choose(ctx, base, button, r, index + 1);
    }
    else if (wrap && count > 1)
    {
      Choose(ctx, base, button, r, 0);
    }
    return;
  }
  __imp__sub_8235ACF8(ctx, base);
}

// UIOptionButton::SetCurrentIndex(this, index).
extern "C" REX_FUNC(__imp__sub_82353FC8);
extern "C" REX_FUNC(sub_82353FC8)
{
  uint32_t button = ctx.r3.u32;
  if (const Row *row = RowOf(base, button))
  {
    Row r = *row;
    Choose(ctx, base, button, r, static_cast<int32_t>(ctx.r4.u32));
    return;
  }
  __imp__sub_82353FC8(ctx, base);
}

// UIOptionButton: the text of value `index` into an FString (this, index, out).
extern "C" REX_FUNC(__imp__sub_82353980);
extern "C" REX_FUNC(sub_82353980)
{
  uint32_t button = ctx.r3.u32;
  if (const Row *row = RowOf(base, button))
  {
    const Setting &setting = Settings()[row->setting];
    int index = static_cast<int32_t>(ctx.r4.u32);
    uint32_t out = ctx.r5.u32;
    if (index < 0 || index >= static_cast<int>(setting.choices.size()) || !out)
    {
      ctx.r3.u64 = 0;
      return;
    }
    Scratch s(ctx);
    MakeString(ctx, base, s, s.String(0), GameTr(setting.choices[index].name));
    CallGuest(ctx, base, kFStringAssign, s.top, out, s.String(0));
    CallGuest(ctx, base, kFStringFree, s.top, s.String(0));
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82353980(ctx, base);
}

// UIOptionButton::RefreshSubscriberValue. Also notes the Options screen.
extern "C" REX_FUNC(__imp__sub_82353A78);
extern "C" REX_FUNC(sub_82353A78)
{
  uint32_t button = ctx.r3.u32;
  if (const Row *row = RowOf(base, button))
  {
    const Setting &setting = Settings()[row->setting];
    Logf("refresh: %s\n", setting.cvar);
    ShowIndex(ctx, base, button, setting, setting.chosen);
    ctx.r3.u64 = 1;
    return;
  }
  if (!g_opening)
  {
    uint32_t scene = LoadU32(base, button + kOwnerScene);
    if (IsSceneNamed(base, scene) && LoadU32(base, scene + kSceneTag + 4) == 0)
    {
      NoteOptionsScene(ctx, base, scene);
    }
  }
  __imp__sub_82353A78(ctx, base);
}

// UIOptionButton::SaveSubscriberValue (through IUIDataStorePublisher, this + 848).
extern "C" REX_FUNC(__imp__sub_8235ADD8);
extern "C" REX_FUNC(sub_8235ADD8)
{
  if (const Row *row = RowOf(base, ctx.r3.u32 - kPublisher))
  {
    // The page's Accept publishes every widget's value (UIAction_PublishValue).
    Setting &setting = Settings()[row->setting];
    Logf("publish: %s\n", setting.cvar);
    if (setting.when == When::kAccept)
    {
      Accept(setting);
    }
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_8235ADD8(ctx, base);
}
