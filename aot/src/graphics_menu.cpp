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
// The page's first row picks a section (display, effects), the seven below
// show its settings; rows a section has none for are hidden. Most changes
// apply at once (fullscreen, frame rate cap, vertical sync, FPS counter,
// anisotropy, anti-aliasing, ambient occlusion, motion blur, depth of field,
// shadow resolution: the renderer reads their cvars every frame). The render resolution waits for the page's
// Accept, which publishes every widget's value (SaveSubscriberValue): the
// native renderer then switches at the end of a frame
// (RendererSetResolutionScale; the cvar stays as the game started, the
// fonts' pages were made for it); a pick left with B is dropped. The aspect
// ratio needs a restart (the game's D3D device and its buffers are created
// with it), as does the resolution without the native renderer; the row's
// label says what is pending. Every change is saved to settings/hardware.toml and, when the
// launcher started the game (ao2_launcher_settings), to the launcher's own
// settings file, whose flags would otherwise win at the next start.
//
// The Controls page: on the Graphics page the device button stays,
// captioned "Controls", and opens a third instance (kControlsTag).
// Its first row picks a section (movement, combat, inventory, other), the
// rows below show that section's keyboard binds (keybind_*; the rows it has
// none for are hidden) and, in the last one, the mouse sensitivity. The
// inventory's weapon rows are the keys bound to both LB and a D-pad direction;
// the LB and D-pad rows show only the keys of their own; the device button is "Default keys" (pressed twice).
// On a bind's row A, Enter, right or a click on its right half waits for a key
// or a mouse button (kCaptureSeconds; a modifier alone binds when let go,
// with another key it is a combination), which becomes the bind's first key;
// left (the left half) adds it as one more. No other bind keeps the key.
// Esc and Enter are not taken: Esc stops waiting, Enter is ignored.
// While it waits every pad state is emptied (OnPadPoll, from the
// XInputGetState hook) and the menus' mouse is off, until the keys are let
// go again. Binds apply at once (the driver reads the cvars every poll) and
// are saved to settings/mapping.toml and the launcher's kb_* keys.
//
// The pause menu has its own Options screen (AllOptionsUIScene_IGPM_Xbox: no
// preferred character row, no device button). Its TV type row becomes a
// plain button, "Graphics and controls" / "Open" (bHideArrows, no value
// list; the mouse clicks it as a button): A or a click opens the Graphics
// page over it from the same template, and the Controls page from there.
// The A that opened it is swallowed until let go: the page would take it as
// its Accept.
#include "graphics_menu.h"

#include "aspect_ratio.h"
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
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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
  constexpr uint32_t kStateStack = 136;  // UIScreenObject.StateStack (TArray of UIState)
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
  constexpr uint32_t kControlsTag = 0x47465833;      // SceneTag number of the Controls instance

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
    const char *cvar;          // nullptr: the Controls page's section row
    const char *toml;          // its value in the file: "s" string, "b" bool, "n" number
    const char *launcher_key;  // the launcher's key, nullptr if it has none
    std::vector<Choice> choices;
    int running = 0;           // the choice the game runs with
    int chosen = 0;            // the one picked
    bool restart = false;      // accepted, but only a restart applies it
    const char *file = "hardware.toml"; // where it is saved in settings/
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
        {"Anti-aliasing", nullptr, When::kNow, "ao2_anti_aliasing", "s", "aa",
         {{"Off", "off"}, {"FXAA", "fxaa"}, {"SMAA", "smaa"}}},
        {"Ambient occlusion", nullptr, When::kNow, "ao2_ambient_occlusion", "s", "ao",
         {{"Off", "off"}, {"SSAO", "ssao"}, {"HBAO", "hbao"}, {"GTAO", "gtao"}}},
        {"Motion blur", nullptr, When::kNow, "ao2_motion_blur", "n", "motion_blur",
         {{"Off", "0"}, {"Low", "1"}, {"High", "2"}}},
        {"Depth of field", nullptr, When::kNow, "ao2_depth_of_field", "n", "dof",
         {{"Off", "0"}, {"On", "1"}}},
        {"Shadow resolution", nullptr, When::kNow, "ao2_shadow_resolution", "n", "shadow_res",
         {{"Auto", "0"}, {"1024", "1024"}, {"2048", "2048"}, {"4096", "4096"}}},
    };
    return settings;
  }

  // The Graphics page's first row picks a section; the seven rows below show
  // its settings (indices into Settings(), -1: the row is hidden).
  Setting &GraphicsSectionSetting()
  {
    static Setting section = {"Section", nullptr, When::kNow, nullptr, "n", nullptr,
                              {{"Display", "0"}, {"Effects", "1"}}};
    return section;
  }
  constexpr int kGraphicsRows = 7;
  constexpr int kGraphicsSections[2][kGraphicsRows] = {
      {0, 1, 2, 3, 4, 5, -1},
      {6, 7, 8, 9, 10, 11, -1},
  };

  // --- The Controls page --------------------------------------------

  // Its first row: which section the other seven show.
  Setting &SectionSetting()
  {
    static Setting section = {"Section", nullptr, When::kNow, nullptr, "n", nullptr,
                              {{"Movement", "0"}, {"Combat", "1"}, {"Inventory", "2"}, {"Other", "3"}}};
    return section;
  }

  // The settings among its rows (mapping.toml holds them).
  std::vector<Setting> &ControlSettings()
  {
    static std::vector<Setting> settings = {
        {"Mouse sensitivity", nullptr, When::kNow, "ao2_mouse_sensitivity", "n", "mouse_sens",
         {{"0.5", "0.5"}, {"0.75", "0.75"}, {"1", "1"}, {"1.25", "1.25"}, {"1.5", "1.5"}, {"2", "2"}, {"2.5", "2.5"},
          {"3", "3"}, {"4", "4"}, {"5", "5"}, {"6", "6"}, {"8", "8"}},
         0, 0, false, "mapping.toml"},
    };
    return settings;
  }

  // A row of a section: a key bind (keybind_<bind>) or one of
  // ControlSettings(); no label: the row is hidden in that section.
  //
  // LB and the D-pad share keys: one in both presses LB + that direction, a
  // weapon (1 primary, 2 secondary, 3 special, 4 / G grenades). The Inventory
  // section's weapon rows (`weapon`, bind the direction) show and set those;
  // the LB and D-pad rows only the keys of their own.
  struct ControlItem
  {
    const char *label = nullptr; // English, through Tr
    const char *bind = nullptr;  // nullptr for a setting
    int setting = 0;
    bool weapon = false;
  };
  constexpr int kSectionRows = 7;
  constexpr int kSectionCount = 4;
  constexpr ControlItem kSections[kSectionCount][kSectionRows] = {
      {{"Move forward", "lstick_up"},
       {"Move back", "lstick_down"},
       {"Move left", "lstick_left"},
       {"Move right", "lstick_right"},
       {"Crouch", "lstick_press"},
       {"Roll / cover / vault", "y"},
       {"Use / heal / confirm", "a"}},
      {{"Fire", "right_trigger"},
       {"Aim", "left_trigger"},
       {"Reload", "x"},
       {"Co-op commands", "right_shoulder"},
       {"Cancel / back", "b"},
       {"Right stick (R3)", "rstick_press"},
       {}},
      {{"Inventory (hold)", "left_shoulder"},
       {"Primary weapon", "dpad_up", 0, true},
       {"Secondary weapon", "dpad_left", 0, true},
       {"Special weapon", "dpad_right", 0, true},
       {"Grenades", "dpad_down", 0, true},
       {},
       {}},
      {{"Advance", "dpad_up"},
       {"Partner view", "dpad_down"},
       {"Regroup", "dpad_left"},
       {"Hold position", "dpad_right"},
       {"GPS", "back"},
       {"Pause", "start"},
       {"Mouse sensitivity", nullptr, 0}},
  };

  // The keyboard binds' defaults (utils.h): "Default keys" restores them.
  using utils::kDefaultBinds;

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
    for (auto *list : {&Settings(), &ControlSettings()})
    {
      for (Setting &s : *list)
      {
        s.running = s.chosen = CurrentChoice(s);
      }
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

  // Sets `key = value` in a flat key = value file (the line replaced, or
  // added). A comment after the old value stays, in its column.
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
        size_t comment = std::string::npos;
        bool quoted = false;
        for (size_t i = after + 1; i < line.size(); ++i)
        {
          if (line[i] == '"')
          {
            quoted = !quoted;
          }
          else if (line[i] == '#' && !quoted)
          {
            comment = i;
            break;
          }
        }
        std::string updated = key + " = " + value;
        if (comment != std::string::npos)
        {
          updated.append(comment > updated.size() + 1 ? comment - updated.size() : 1, ' ');
          updated += line.substr(comment);
        }
        line = updated;
        found = true;
      }
    }
    if (!found)
    {
      lines.push_back(key + " = " + value);
    }
    // Written whole to a file of its own, then put in place: a crash, or the
    // launcher reading it, in the middle never finds half a file.
    std::filesystem::path part = path;
    part += ".tmp";
    {
      std::ofstream out(part, std::ios::binary | std::ios::trunc);
      for (const std::string &line : lines)
      {
        out << line << "\r\n";
      }
      if (!out.flush())
      {
        throw std::runtime_error("cannot write " + rex::path_to_utf8(part));
      }
    }
    std::error_code ec;
    std::filesystem::rename(part, path, ec);
    if (ec)
    {
      // Held open by someone else (the launcher reading it): written in place.
      std::filesystem::remove(part, ec);
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      for (const std::string &line : lines)
      {
        out << line << "\r\n";
      }
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
      SetFileKey(utils::SettingsDir() / s.file, s.cvar, toml);
      if (std::string(s.cvar) == "ao2_fps_target")
      {
        SetFileKey(utils::SettingsDir() / "hardware.toml", "ao2_fps_unlock", "true");
      }      std::string launcher = REXCVAR_GET(ao2_launcher_settings);
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
    Logf("%s = %s%s\n", s.cvar, value.c_str(), ok ? "" : " (not set)");    if (std::string(s.cvar) == "fullscreen" && g_window)
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

  uint32_t g_options_scene = 0;  // the Options screen last seen
  uint32_t g_graphics_scene = 0; // the Graphics page
  uint32_t g_controls_scene = 0; // the Controls page
  uint32_t g_pause_options = 0;  // the pause menu's Options screen last seen
  bool g_opening = false;

  enum class Page
  {
    kGraphics,
    kControls,
    kPauseOptions, // its TV type row: "Graphics and controls"
  };

  // An option button of a page. On the Graphics and Controls pages `index` is
  // its row (0 the section, 1-7 the section's items).
  struct Row
  {
    Page page;
    int index;
    uint32_t label;
  };
  std::unordered_map<uint32_t, Row> g_rows; // option button -> its row

  int g_graphics_section = 0;                          // the section the Graphics page shows
  uint32_t g_graphics_buttons[1 + kGraphicsRows] = {}; // its rows' option buttons
  uint32_t g_graphics_labels[1 + kGraphicsRows] = {};  // and their labels
  uint32_t g_graphics_device = 0;                      // its "Controls" button
  int g_section = 0;                                  // the section the Controls page shows
  uint32_t g_control_buttons[1 + kSectionRows] = {}; // its rows' option buttons
  uint32_t g_control_labels[1 + kSectionRows] = {};  // and their labels
  uint32_t g_defaults_button = 0;                    // its "Default keys" button
  uint32_t g_defaults_armed = 0;                     // GetTickCount of a first press of it, 0 none

  bool IsSceneNamed(const uint8_t *base, uint32_t scene)
  {
    return scene && ao2::ue3::ObjectName(base, scene).rfind("AllOptionsUIScene", 0) == 0 &&
           ao2::ue3::Name(base, LoadU32(base, scene + kSceneTag)) == "AllOptionsUIScene";
  }

  // The pause menu's own Options screen (AllOptionsUIScene_IGPM_Xbox): no
  // device button there, its TV type row opens the Graphics page instead.
  bool IsPauseOptions(const uint8_t *base, uint32_t scene)
  {
    return scene && ao2::ue3::ObjectName(base, scene).rfind("AllOptionsUIScene_IGPM", 0) == 0;
  }

  bool IsPage(const uint8_t *base, uint32_t scene, Page page)
  {
    if (page == Page::kPauseOptions)
    {
      return scene && scene == g_pause_options && IsPauseOptions(base, scene);
    }
    uint32_t ours = page == Page::kGraphics ? g_graphics_scene : g_controls_scene;
    uint32_t tag = page == Page::kGraphics ? kGraphicsTag : kControlsTag;
    return scene && scene == ours && IsSceneNamed(base, scene) && LoadU32(base, scene + kSceneTag + 4) == tag;
  }

  // The row of an option button of one of the pages, nullptr if it is none.
  const Row *RowOf(const uint8_t *base, uint32_t button)
  {
    if (g_rows.empty())
    {
      return nullptr;
    }
    auto it = g_rows.find(button);
    if (it == g_rows.end() || !IsPage(base, LoadU32(base, button + kOwnerScene), it->second.page))
    {
      return nullptr;
    }
    return &it->second;
  }

  // The setting a row shows, nullptr for a key bind.
  Setting *SettingOf(const Row &row)
  {
    if (row.page == Page::kGraphics)
    {
      if (row.index == 0)
      {
        return &GraphicsSectionSetting();
      }
      int setting = kGraphicsSections[g_graphics_section][row.index - 1];
      return setting >= 0 ? &Settings()[setting] : nullptr;
    }
    if (row.page == Page::kPauseOptions)
    {
      return nullptr; // "Open"
    }
    if (row.index == 0)
    {
      return &SectionSetting();
    }
    const ControlItem &item = kSections[g_section][row.index - 1];
    return item.label && !item.bind ? &ControlSettings()[item.setting] : nullptr;
  }

  // The key bind a row shows, nullptr for a setting.
  const ControlItem *BindOf(const Row &row)
  {
    if (row.page != Page::kControls || row.index < 1 || row.index > kSectionRows)
    {
      return nullptr;
    }
    const ControlItem &item = kSections[g_section][row.index - 1];
    return item.bind ? &item : nullptr;
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

  bool Focused(const uint8_t *base, uint32_t widget)
  {
    static uint32_t focused_class = 0;
    if (!focused_class)
    {
      focused_class = ao2::ue3::FindClass(base, "UIState_Focused");
    }
    uint32_t data = LoadU32(base, widget + kStateStack);
    uint32_t count = LoadU32(base, widget + kStateStack + 4);
    for (uint32_t i = 0; data && i < count && i < 16; ++i)
    {
      uint32_t state = LoadU32(base, data + i * 4);
      if (state && focused_class && ao2::ue3::IsA(base, state, focused_class))
      {
        return true;
      }
    }
    return false;
  }

  void SetHidden(uint8_t *base, uint32_t widget, bool hidden)
  {
    uint32_t word = LoadU32(base, widget + kHiddenWord);
    StoreU32(base, widget + kHiddenWord, hidden ? word | kHiddenMask : word & ~kHiddenMask);
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

  // A button's caption, already in the game's 8-bit text: its markup becomes
  // the text (IUIDataStorePublisher at +848, vtable +4; the option buttons
  // show their values so).
  void ShowText(PPCContext &ctx, uint8_t *base, uint32_t button, const std::string &text)
  {
    Scratch s(ctx);
    MakeString(ctx, base, s, s.String(0), text);
    uint32_t publisher = button + kPublisher;
    CallGuest(ctx, base, LoadU32(base, LoadU32(base, publisher) + 4), s.top, publisher, s.String(0),
              static_cast<uint32_t>(-1));
    CallGuest(ctx, base, kFStringFree, s.top, s.String(0));
  }

  void ShowIndex(PPCContext &ctx, uint8_t *base, uint32_t button, const Setting &setting, int index)
  {
    StoreU32(base, button + kCurrentIndex, static_cast<uint32_t>(index));
    ShowText(ctx, base, button, GameTr(setting.choices[index].name));
  }

  void SetCaption(PPCContext &ctx, uint8_t *base, uint32_t button, const char *english)
  {
    ShowText(ctx, base, button, GameTr(english));
  }

  // The Options screen opened (each of its option buttons refreshes):
  // "Select Storage Device" becomes "Graphics and controls", once - while its caption is
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
      SetCaption(ctx, base, button, "Graphics and controls");
    }
  }

  // --- Key binds ----------------------------------------------------------------

  std::string BindValue(const char *bind) { return rex::cvar::GetFlagByName(std::string("keybind_") + bind); }

  // A bind's keys ("none" is nothing: the launcher's empty bind).
  std::vector<std::string> SplitBind(const std::string &value)
  {
    std::vector<std::string> keys;
    std::stringstream in(value);
    std::string key;
    while (std::getline(in, key, ','))
    {
      size_t a = key.find_first_not_of(" \t"), b = key.find_last_not_of(" \t");
      key = a == std::string::npos ? std::string() : key.substr(a, b - a + 1);
      if (!key.empty() && key != "none")
      {
        keys.push_back(key);
      }
    }
    return keys;
  }

  std::string JoinBind(const std::vector<std::string> &keys)
  {
    std::string out;
    for (const std::string &key : keys)
    {
      out += (out.empty() ? "" : ",") + key;
    }
    return out;
  }

  // A key as the page shows it ("Control+C" as "Ctrl+C"), in the game's text.
  std::string KeyCaption(const std::string &token)
  {
    size_t plus = token.find('+');
    if (plus != std::string::npos && plus + 1 < token.size())
    {
      return KeyCaption(token.substr(0, plus)) + "+" + KeyCaption(token.substr(plus + 1));
    }
    static const std::unordered_map<std::string, const char *> kCaptions = {
        {"LMB", "LMB"},         {"RMB", "RMB"},         {"MMB", "MMB"},        {"MB4", "Mouse 4"},
        {"MB5", "Mouse 5"},     {"Up", "Up"},           {"Down", "Down"},      {"Left", "Left"},
        {"Right", "Right"},     {"Control", "Ctrl"},    {"Return", "Enter"},   {"Escape", "Esc"},
        {"Backtick", "~"},      {"Minus", "-"},         {"Plus", "="},         {"Comma", ","},
        {"Period", "."},        {"Semicolon", ";"},     {"Slash", "/"},        {"Backslash", "\\"},
        {"LBracket", "["},      {"RBracket", "]"},      {"Quote", "'"},        {"PageUp", "PgUp"},
        {"PageDown", "PgDn"},   {"CapsLock", "Caps"},   {"ScrollLock", "ScrLk"}, {"NumpadPlus", "Num +"},
        {"NumpadMinus", "Num -"}, {"NumpadStar", "Num *"}, {"NumpadSlash", "Num /"}, {"NumpadEnter", "Num Enter"},
        {"Space", "Space"},
    };
    auto it = kCaptions.find(token);
    if (it != kCaptions.end())
    {
      return GameTr(it->second);
    }
    if (token.rfind("Numpad", 0) == 0)
    {
      return "Num " + token.substr(6);
    }
    return GameText(token.c_str());
  }

  std::vector<std::string> Keys(const char *bind) { return SplitBind(BindValue(bind)); }

  bool Has(const std::vector<std::string> &keys, const std::string &key)
  {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
  }

  constexpr const char *kLeftShoulder = "left_shoulder";
  constexpr const char *kDpad[] = {"dpad_up", "dpad_down", "dpad_left", "dpad_right"};

  bool IsDpad(const char *bind) { return std::strncmp(bind, "dpad_", 5) == 0; }

  // The keys a row stands for: a weapon row's are in LB and its direction,
  // LB's and a direction's are in no such pair, any other bind's are all.
  std::vector<std::string> OwnKeys(const ControlItem &item)
  {
    std::vector<std::string> keys = Keys(item.bind);
    std::vector<std::string> lb = Keys(kLeftShoulder);
    auto keep = [&](auto pred) {
      keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const std::string &k) { return !pred(k); }), keys.end());
    };
    if (item.weapon)
    {
      keep([&](const std::string &k) { return Has(lb, k); });
    }
    else if (std::strcmp(item.bind, kLeftShoulder) == 0)
    {
      keep([&](const std::string &k) {
        for (const char *d : kDpad)
        {
          if (Has(Keys(d), k))
          {
            return false;
          }
        }
        return true;
      });
    }
    else if (IsDpad(item.bind))
    {
      keep([&](const std::string &k) { return !Has(lb, k); });
    }
    return keys;
  }

  // A row's keys as the page shows them.
  std::string BindText(const ControlItem *item)
  {
    if (!item || !item->bind)
    {
      return {};
    }
    std::string out;
    for (const std::string &key : OwnKeys(*item))
    {
      out += (out.empty() ? "" : " / ") + KeyCaption(key);
    }
    return out.empty() ? std::string("-") : out;
  }

  // A bind takes `value` now and at the next start: mapping.toml, and the
  // launcher's settings, whose flags would otherwise win.
  void SetBind(const char *bind, const std::string &value)
  {
    std::string cvar = std::string("keybind_") + bind;
    // A command line flag needs a value: "none" is the launcher's empty bind.
    bool ok = rex::cvar::SetFlagByName(cvar, value.empty() ? "none" : value);
    Logf("%s = %s%s\n", cvar.c_str(), value.c_str(), ok ? "" : " (not set)");
    try
    {
      SetFileKey(utils::SettingsDir() / "mapping.toml", cvar, "\"" + value + "\"");
      std::string launcher = REXCVAR_GET(ao2_launcher_settings);
      if (!launcher.empty() && std::filesystem::exists(std::u8string(launcher.begin(), launcher.end())))
      {
        SetFileKey(std::filesystem::path(std::u8string(launcher.begin(), launcher.end())), std::string("kb_") + bind,
                   value);
      }
    }
    catch (const std::exception &e)
    {
      Logf("save %s: %s\n", cvar.c_str(), e.what());
    }
  }

  // `token` was pressed for `bind`: it becomes the bind's first key (or one
  // more, `add`), and no other bind keeps it.
  // A row's keys become `own`, the others of its binds stay: a weapon row's
  // go into LB and its direction, after the keys of their own.
  void SetOwnKeys(const ControlItem &item, const std::vector<std::string> &own)
  {
    std::vector<std::string> old = OwnKeys(item);
    auto rebuild = [&](const char *bind, bool own_first) {
      std::vector<std::string> keys = own_first ? own : std::vector<std::string>{};
      for (const std::string &k : Keys(bind))
      {
        if (!Has(old, k) && !Has(keys, k))
        {
          keys.push_back(k);
        }
      }
      for (const std::string &k : own)
      {
        if (!Has(keys, k))
        {
          keys.push_back(k);
        }
      }
      if (JoinBind(keys) != JoinBind(Keys(bind)))
      {
        SetBind(bind, JoinBind(keys));
      }
    };
    if (item.weapon)
    {
      rebuild(kLeftShoulder, false);
      rebuild(item.bind, false);
    }
    else
    {
      rebuild(item.bind, true);
    }
  }

  void TakeKey(const ControlItem &item, const std::string &token, bool add)
  {
    for (const auto &entry : kDefaultBinds)
    {
      if (std::strcmp(entry[0], item.bind) == 0 || (item.weapon && std::strcmp(entry[0], kLeftShoulder) == 0))
      {
        continue;
      }
      std::vector<std::string> keys = Keys(entry[0]);
      auto it = std::find(keys.begin(), keys.end(), token);
      if (it != keys.end())
      {
        keys.erase(it);
        SetBind(entry[0], JoinBind(keys));
      }
    }
    std::vector<std::string> own = OwnKeys(item);
    own.erase(std::remove(own.begin(), own.end(), token), own.end());
    if (add || own.empty())
    {
      own.push_back(token);
    }
    else
    {
      own[0] = token;
    }
    SetOwnKeys(item, own);
  }

  // --- Waiting for a key ----------------------------------------------------------

  constexpr int kCaptureSeconds = 5;
  constexpr uint16_t kButtonA = 0x1000;

  struct Capture
  {
    uint32_t button = 0;       // the row's option button; 0: not waiting
    const ControlItem *item = nullptr;
    bool add = false;          // the key is added to the row's
    uint32_t started = 0;      // GetTickCount
    bool armed = false;        // every key was let go since it began
    int shown = -1;            // the seconds left as shown
    int modifier = 0;          // a modifier held alone so far (VK), 0 none
    bool swallowing = false;   // after it: no input until every key is let go
    uint32_t swallow_end = 0;  // or until then
  };
  Capture g_capture;
  bool g_prev_a[4] = {};

  // The keys as the binds name them (rexruntime's key table), nullptr for one
  // they do not know. Shift, Control and Alt are modifiers.
  const char *KeyName(int vk)
  {
    static const char *names[256] = {};
    static bool done = false;
    if (!done)
    {
      done = true;
      static std::string storage[256];
      auto set = [](int code, std::string name) {
        storage[code] = std::move(name);
        names[code] = storage[code].c_str();
      };
      for (int c = 'A'; c <= 'Z'; ++c)
      {
        set(c, std::string(1, char(c)));
      }
      for (int c = '0'; c <= '9'; ++c)
      {
        set(c, std::string(1, char(c)));
      }
      for (int i = 0; i < 24; ++i)
      {
        set(VK_F1 + i, "F" + std::to_string(i + 1));
      }
      for (int i = 0; i < 10; ++i)
      {
        set(VK_NUMPAD0 + i, "Numpad" + std::to_string(i));
      }
      const std::pair<int, const char *> kOthers[] = {
          {VK_LBUTTON, "LMB"},       {VK_RBUTTON, "RMB"},         {VK_MBUTTON, "MMB"},
          {VK_XBUTTON1, "MB4"},      {VK_XBUTTON2, "MB5"},        {VK_OEM_3, "Backtick"},
          {VK_OEM_MINUS, "Minus"},   {VK_OEM_PLUS, "Plus"},       {VK_OEM_COMMA, "Comma"},
          {VK_OEM_PERIOD, "Period"}, {VK_OEM_1, "Semicolon"},     {VK_OEM_2, "Slash"},
          {VK_OEM_5, "Backslash"},   {VK_OEM_102, "Backslash"},   {VK_OEM_4, "LBracket"},
          {VK_OEM_6, "RBracket"},    {VK_OEM_7, "Quote"},         {VK_ESCAPE, "Escape"},
          {VK_RETURN, "Return"},     {VK_SPACE, "Space"},         {VK_TAB, "Tab"},
          {VK_BACK, "Backspace"},    {VK_DELETE, "Delete"},       {VK_INSERT, "Insert"},
          {VK_HOME, "Home"},         {VK_END, "End"},             {VK_PRIOR, "PageUp"},
          {VK_NEXT, "PageDown"},     {VK_LEFT, "Left"},           {VK_RIGHT, "Right"},
          {VK_UP, "Up"},             {VK_DOWN, "Down"},           {VK_SHIFT, "Shift"},
          {VK_CONTROL, "Control"},   {VK_MENU, "Alt"},            {VK_ADD, "NumpadPlus"},
          {VK_SUBTRACT, "NumpadMinus"}, {VK_MULTIPLY, "NumpadStar"}, {VK_DIVIDE, "NumpadSlash"},
          {VK_PAUSE, "Pause"},       {VK_CAPITAL, "CapsLock"},    {VK_NUMLOCK, "NumLock"},
          {VK_SCROLL, "ScrollLock"},
      };
      for (const auto &[code, name] : kOthers)
      {
        set(code, name);
      }
    }
    return vk > 0 && vk < 256 ? names[vk] : nullptr;
  }

  bool IsModifier(int vk) { return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU; }

  bool Down(int vk) { return GetAsyncKeyState(vk) & 0x8000; }

  bool AnyKeyDown()
  {
    for (int vk = 1; vk < 256; ++vk)
    {
      if (KeyName(vk) && Down(vk))
      {
        return true;
      }
    }
    return false;
  }

  bool GameInFront()
  {
    HWND hwnd = static_cast<HWND>(ao2::aspect::Window());
    return hwnd && GetForegroundWindow() == hwnd;
  }

  // The Controls page's rows show the section's items; the rows a section
  // has no item for are hidden. Up and down go along the shown rows and the
  // button under them, round.
  void RefreshControls(PPCContext &ctx, uint8_t *base)
  {
    std::vector<uint32_t> shown;
    for (int i = 0; i <= kSectionRows; ++i)
    {
      uint32_t button = g_control_buttons[i];
      const Row *row = button ? RowOf(base, button) : nullptr;
      if (!row)
      {
        continue;
      }
      bool hidden = i > 0 && !kSections[g_section][i - 1].label;
      SetHidden(base, button, hidden);
      if (g_control_labels[i])
      {
        SetHidden(base, g_control_labels[i], hidden);
      }
      if (hidden)
      {
        continue;
      }
      shown.push_back(button);
      if (i > 0 && g_control_labels[i])
      {
        SetLabel(ctx, base, g_control_labels[i], kSections[g_section][i - 1].label);
      }
      if (Setting *s = SettingOf(*row))
      {
        ShowIndex(ctx, base, button, *s, s->chosen);
      }
      else if (button != g_capture.button)
      {
        StoreU32(base, button + kCurrentIndex, 0);
        ShowText(ctx, base, button, BindText(BindOf(*row)));
      }
    }
    if (g_defaults_button)
    {
      shown.push_back(g_defaults_button);
    }
    for (size_t i = 0; shown.size() > 1 && i < shown.size(); ++i)
    {
      uint32_t next = shown[(i + 1) % shown.size()];
      StoreU32(base, shown[i] + kForcedNavigation + kFaceBottom * 4, next);
      StoreU32(base, next + kForcedNavigation + kFaceTop * 4, shown[i]);
    }
    if (g_controls_scene)
    {
      StoreU32(base, g_controls_scene + kSceneFlags,
               LoadU32(base, g_controls_scene + kSceneFlags) | kUpdateNavigation);
    }
  }

  void StartCapture(PPCContext &ctx, uint8_t *base, uint32_t button, const Row &row, bool add)
  {
    const ControlItem *item = BindOf(row);
    if (!item || g_capture.button)
    {
      return;
    }
    g_capture = Capture{};
    g_capture.button = button;
    g_capture.item = item;
    g_capture.add = add;
    g_capture.started = GetTickCount();
    g_capture.shown = kCaptureSeconds;
    ShowText(ctx, base, button, GameTr("Press a key...") + " " + std::to_string(kCaptureSeconds));
    Logf("waiting for a key: %s%s\n", item->bind, add ? " (one more)" : "");
  }

  // The wait ends: `token` pressed, or nothing (time ran out).
  void FinishCapture(PPCContext &ctx, uint8_t *base, const std::string &token)
  {
    const ControlItem *item = g_capture.item;
    bool add = g_capture.add;
    g_capture = Capture{};
    g_capture.swallowing = true;
    g_capture.swallow_end = GetTickCount() + 3000;
    if (!token.empty())
    {
      TakeKey(*item, token, add);
    }
    RefreshControls(ctx, base);
  }

  void Listen(PPCContext &ctx, uint8_t *base)
  {
    Capture &c = g_capture;
    int left = kCaptureSeconds - static_cast<int>((GetTickCount() - c.started) / 1000);
    if (left <= 0)
    {
      FinishCapture(ctx, base, {});
      return;
    }
    if (left != c.shown)
    {
      c.shown = left;
      ShowText(ctx, base, c.button, GameTr("Press a key...") + " " + std::to_string(left));
    }
    if (!c.armed)
    {
      // The key or button that began it goes up first.
      c.armed = !AnyKeyDown();
      return;
    }
    if (!GameInFront())
    {
      return;
    }
    int modifier = 0;
    for (int vk = 1; vk < 256; ++vk)
    {
      if (!KeyName(vk) || !Down(vk))
      {
        continue;
      }
      if (IsModifier(vk))
      {
        modifier = vk;
        continue;
      }
      // Esc and Enter are the menus' back and accept, never bound here: Esc
      // stops waiting, Enter does nothing.
      if (vk == VK_ESCAPE)
      {
        FinishCapture(ctx, base, {});
        return;
      }
      if (vk == VK_RETURN)
      {
        continue;
      }
      std::string token;
      if (Down(VK_CONTROL))
      {
        token += "Control+";
      }
      if (Down(VK_SHIFT))
      {
        token += "Shift+";
      }
      if (Down(VK_MENU))
      {
        token += "Alt+";
      }
      FinishCapture(ctx, base, token + KeyName(vk));
      return;
    }
    if (modifier)
    {
      c.modifier = modifier;
    }
    else if (c.modifier)
    {
      // A modifier let go alone binds itself.
      FinishCapture(ctx, base, KeyName(c.modifier));
    }
  }

  // "Default keys": the first press asks, a second one within a few seconds
  // puts every bind back.
  void DefaultsPressed(PPCContext &ctx, uint8_t *base)
  {
    if (!g_defaults_button)
    {
      return;
    }
    if (!g_defaults_armed)
    {
      g_defaults_armed = GetTickCount() | 1;
      SetCaption(ctx, base, g_defaults_button, "Press again to reset");
      return;
    }
    g_defaults_armed = 0;
    for (const auto &entry : kDefaultBinds)
    {
      if (BindValue(entry[0]) != entry[1] && !(entry[1][0] == 0 && BindValue(entry[0]) == "none"))
      {
        SetBind(entry[0], entry[1]);
      }
    }
    Logf("%s\n", "default keys");
    SetCaption(ctx, base, g_defaults_button, "Default keys");
    RefreshControls(ctx, base);
  }

  // Takes the pad state's buttons, triggers and sticks away.
  void Silence(PPCContext &ctx, uint8_t *base, uint32_t state)
  {
    if (ctx.r3.u32 == 0)
    {
      std::memset(base + state + 4, 0, 12);
    }
  }

  // --- The pages ----------------------------------------------------------------

  // The rows top to bottom: option button, its label.
  constexpr const char *kRowWidgets[][2] = {
      {"optbtnPreferChar", "lblPreferredChar"}, {"UIOptionButton_1", "UILabel_1"},
      {"UIOptionButton_2", "UILabel_2"},        {"UIOptionButton_3", "UILabel_3"},
      {"UIOptionButton_5", "UILabel_7"},        {"UIOptionButton_4", "UILabel_0"},
      {"UIOptionButton_0", "UILabel_6"},        {"TVTypeOptionButton", "UILabel_4"},
  };

  void ForgetRows(Page page)
  {
    for (auto it = g_rows.begin(); it != g_rows.end();)
    {
      it = it->second.page == page ? g_rows.erase(it) : std::next(it);
    }
  }

  // The Graphics page's rows show the section's settings; the rows it has
  // none for are hidden. Up and down go along the shown rows and the
  // "Controls" button under them, round.
  void RefreshGraphics(PPCContext &ctx, uint8_t *base)
  {
    std::vector<uint32_t> shown;
    for (int i = 0; i <= kGraphicsRows; ++i)
    {
      uint32_t button = g_graphics_buttons[i];
      const Row *row = button ? RowOf(base, button) : nullptr;
      if (!row)
      {
        continue;
      }
      Setting *s = SettingOf(*row);
      SetHidden(base, button, !s);
      if (g_graphics_labels[i])
      {
        SetHidden(base, g_graphics_labels[i], !s);
      }
      if (!s)
      {
        continue;
      }
      shown.push_back(button);
      if (g_graphics_labels[i])
      {
        SetLabel(ctx, base, g_graphics_labels[i], i == 0 ? s->label : LabelOf(*s));
      }
      ShowIndex(ctx, base, button, *s, s->chosen);
    }
    if (g_graphics_device)
    {
      shown.push_back(g_graphics_device);
    }
    for (size_t i = 0; shown.size() > 1 && i < shown.size(); ++i)
    {
      uint32_t next = shown[(i + 1) % shown.size()];
      StoreU32(base, shown[i] + kForcedNavigation + kFaceBottom * 4, next);
      StoreU32(base, next + kForcedNavigation + kFaceTop * 4, shown[i]);
    }
    if (g_graphics_scene)
    {
      StoreU32(base, g_graphics_scene + kSceneFlags,
               LoadU32(base, g_graphics_scene + kSceneFlags) | kUpdateNavigation);
    }
  }

  // Turns a fresh instance of the Options screen into the Graphics page.
  void SetupGraphics(PPCContext &ctx, uint8_t *base, uint32_t scene)
  {
    std::unordered_map<std::string, uint32_t> w;
    Widgets(base, scene, w);
    ForgetRows(Page::kGraphics);
    for (Setting &s : Settings())
    {
      if (s.when == When::kAccept && !s.restart)
      {
        s.chosen = s.running; // a pick left with B is dropped
      }
    }
    for (int i = 0; i <= kGraphicsRows; ++i)
    {
      uint32_t button = w.count(kRowWidgets[i][0]) ? w[kRowWidgets[i][0]] : 0;
      uint32_t label = w.count(kRowWidgets[i][1]) ? w[kRowWidgets[i][1]] : 0;
      g_graphics_buttons[i] = button;
      g_graphics_labels[i] = label;
      if (!button)
      {
        Logf("setup: no %s\n", kRowWidgets[i][0]);
        continue;
      }
      g_rows[button] = Row{Page::kGraphics, i, label};
    }
    for (const char *name : {"BrightnessSlider", "UILabel_5"})
    {
      if (w.count(name))
      {
        SetHidden(base, w[name], true);
      }
    }
    // The device button opens the Controls page.
    g_graphics_device = w.count("UILabelButton_0") ? w["UILabelButton_0"] : 0;
    if (g_graphics_device)
    {
      SetCaption(ctx, base, g_graphics_device, "Controls");
    }
    if (w.count("lblOptions"))
    {
      SetLabel(ctx, base, w["lblOptions"], "Graphics");
    }
    RefreshGraphics(ctx, base);
    Logf("setup: graphics scene %08X\n", scene);
  }

  // Turns a fresh instance of the Options screen into the Controls page.
  void SetupControls(PPCContext &ctx, uint8_t *base, uint32_t scene)
  {
    std::unordered_map<std::string, uint32_t> w;
    Widgets(base, scene, w);
    ForgetRows(Page::kControls);
    for (int i = 0; i <= kSectionRows; ++i)
    {
      uint32_t button = w.count(kRowWidgets[i][0]) ? w[kRowWidgets[i][0]] : 0;
      uint32_t label = w.count(kRowWidgets[i][1]) ? w[kRowWidgets[i][1]] : 0;
      g_control_buttons[i] = button;
      g_control_labels[i] = label;
      if (!button)
      {
        Logf("setup: no %s\n", kRowWidgets[i][0]);
        continue;
      }
      g_rows[button] = Row{Page::kControls, i, label};
    }
    if (g_control_labels[0])
    {
      SetLabel(ctx, base, g_control_labels[0], SectionSetting().label);
    }
    for (const char *name : {"BrightnessSlider", "UILabel_5"})
    {
      if (w.count(name))
      {
        SetHidden(base, w[name], true);
      }
    }
    g_defaults_button = w.count("UILabelButton_0") ? w["UILabelButton_0"] : 0;
    g_defaults_armed = 0;
    if (g_defaults_button)
    {
      SetCaption(ctx, base, g_defaults_button, "Default keys");
    }
    if (w.count("lblOptions"))
    {
      SetLabel(ctx, base, w["lblOptions"], "Controls");
    }
    RefreshControls(ctx, base);
    Logf("setup: controls scene %08X\n", scene);
  }

  // Another instance of the Options screen over the top scene, its SceneTag
  // number `tag`; 0 if none opened.
  bool OnStack(const uint8_t *base, uint32_t client, uint32_t scene)
  {
    uint32_t data = LoadU32(base, client + kActiveScenes);
    uint32_t count = LoadU32(base, client + kActiveScenes + 4);
    for (uint32_t i = 0; scene && data && i < count && i < 64; ++i)
    {
      if (LoadU32(base, data + i * 4) == scene)
      {
        return true;
      }
    }
    return false;
  }

  uint32_t OpenPage(PPCContext &ctx, uint8_t *base, uint32_t client, uint32_t over, uint32_t tag)
  {
    // The Options screen when it is open (from the pause menu it is not).
    uint32_t options = OnStack(base, client, g_options_scene) && IsSceneNamed(base, g_options_scene) &&
                               LoadU32(base, g_options_scene + kSceneTag + 4) == 0
                           ? g_options_scene
                           : 0;
    uint32_t scene_template = Template(base);
    if (!scene_template)
    {
      Logf("open: no %s\n", "AO2UISkin.AllOptionsUIScene");
      return 0;
    }
    InitSettings();
    Scratch s(ctx);
    StoreU32(base, s.Slot(), 0);
    // The scene client finds an open scene by the template's tag: not the
    // Options screen (the other pages have their own numbers).
    if (options)
    {
      StoreU32(base, options + kSceneTag + 4, kOpeningTag);
    }
    g_opening = true;
    uint32_t open = LoadU32(base, LoadU32(base, client) + kOpenScene);
    uint32_t ok = CallGuest(ctx, base, open, s.top, client, scene_template, LoadU32(base, over + kPlayerOwner),
                            s.Slot(), 0);
    g_opening = false;
    if (options)
    {
      StoreU32(base, options + kSceneTag + 4, 0);
    }
    uint32_t scene = LoadU32(base, s.Slot());
    Logf("open: client %08X template %08X -> %u, scene %08X\n", client, scene_template, ok, scene);
    if (!scene || (options && scene == options) || scene == over)
    {
      return 0;
    }
    StoreU32(base, scene + kSceneTag + 4, tag);
    return scene;
  }

  // "Select Storage Device": on the Options screen it opens the Graphics
  // page, on that the Controls page, on that it is "Default keys".
  bool DeviceButton(PPCContext &ctx, uint8_t *base)
  {
    uint32_t options = g_options_scene;
    uint32_t known = IsPage(base, g_controls_scene, Page::kControls)   ? g_controls_scene
                     : IsPage(base, g_graphics_scene, Page::kGraphics) ? g_graphics_scene
                     : IsSceneNamed(base, options) && LoadU32(base, options + kSceneTag + 4) == 0 ? options
                                                                                                    : 0;
    uint32_t client = known ? LoadU32(base, known + kSceneClient) : 0;
    uint32_t top = client ? TopScene(base, client) : 0;
    if (!top)
    {
      return false;
    }
    if (top == options && IsSceneNamed(base, options))
    {
      if (uint32_t scene = OpenPage(ctx, base, client, options, kGraphicsTag))
      {
        g_graphics_scene = scene;
        SetupGraphics(ctx, base, scene);
      }
      return true;
    }
    if (IsPage(base, top, Page::kGraphics))
    {
      if (uint32_t scene = OpenPage(ctx, base, client, top, kControlsTag))
      {
        g_controls_scene = scene;
        SetupControls(ctx, base, scene);
      }
      return true;
    }
    if (IsPage(base, top, Page::kControls))
    {
      DefaultsPressed(ctx, base);
      return true;
    }
    return false;
  }

  // A row's value when it is no setting: a bind's keys, or "Open".
  std::string RowText(const Row &row)
  {
    return row.page == Page::kPauseOptions ? GameTr("Open") : BindText(BindOf(row));
  }

  // The pause menu's Options screen: its "Graphics and controls" row opens
  // the Graphics page over it.
  void OpenFromPause(PPCContext &ctx, uint8_t *base)
  {
    uint32_t pause = g_pause_options;
    uint32_t client = IsPauseOptions(base, pause) ? LoadU32(base, pause + kSceneClient) : 0;
    if (!client || TopScene(base, client) != pause)
    {
      return;
    }
    if (uint32_t scene = OpenPage(ctx, base, client, pause, kGraphicsTag))
    {
      g_graphics_scene = scene;
      SetupGraphics(ctx, base, scene);
      // The A (Enter) that opened it is still held: the page would take it
      // as a new press, its Accept, and close. No input until it is let go.
      g_capture.swallowing = true;
      g_capture.swallow_end = GetTickCount() + 3000;
    }
  }

  // An option button of the pause menu's Options screen refreshes: its TV
  // type row becomes "Graphics and controls" (true: it is that row).
  bool NotePauseOptions(PPCContext &ctx, uint8_t *base, uint32_t button)
  {
    uint32_t scene = LoadU32(base, button + kOwnerScene);
    if (!IsPauseOptions(base, scene) || ao2::ue3::ObjectName(base, button) != "TVTypeOptionButton")
    {
      return false;
    }
    g_pause_options = scene;
    ForgetRows(Page::kPauseOptions);
    std::unordered_map<std::string, uint32_t> w;
    Widgets(base, scene, w);
    uint32_t label = w.count("UILabel_4") ? w["UILabel_4"] : 0;
    g_rows[button] = Row{Page::kPauseOptions, 0, label};
    if (label)
    {
      SetLabel(ctx, base, label, "Graphics and controls");
    }
    StoreU32(base, button + kCurrentIndex, 0);
    ShowText(ctx, base, button, GameTr("Open"));
    // A plain button: no arrows (UIOptionButton.bHideArrows).
    int32_t hide_word = 0;
    uint32_t hide_mask = 0;
    if (ao2::ue3::BoolProperty(base, "UIOptionButton", "bHideArrows", hide_word, hide_mask))
    {
      StoreU32(base, button + hide_word, LoadU32(base, button + hide_word) | hide_mask);
    }
    Logf("pause options %08X: TV type row is Graphics and controls\n", scene);
    return true;
  }

  void Choose(PPCContext &ctx, uint8_t *base, uint32_t button, const Row &row, int index)
  {
    Setting *chosen = SettingOf(row);
    if (!chosen)
    {
      return;
    }
    Setting &setting = *chosen;
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
    if (&setting == &SectionSetting())
    {
      setting.running = index;
      g_section = index;
      RefreshControls(ctx, base);
      return;
    }
    if (&setting == &GraphicsSectionSetting())
    {
      setting.running = index;
      g_graphics_section = index;
      RefreshGraphics(ctx, base);
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
} // namespace

namespace ao2::gfxmenu
{
  void Attach(rex::ui::Window *window) { g_window = window; }

  void SetCaption(PPCContext &ctx, uint8_t *base, uint32_t button, const char *english)
  {
    ::SetCaption(ctx, base, button, english);
  }

  bool Capturing() { return g_capture.button || g_capture.swallowing; }

  bool IsOpenButton(const uint8_t *base, uint32_t widget)
  {
    const Row *row = RowOf(base, widget);
    return row && row->page == Page::kPauseOptions;
  }

  bool OnPadPoll(PPCContext &ctx, uint8_t *base, uint32_t user, uint32_t state)
  {
    Capture &c = g_capture;
    if (c.button && !RowOf(base, c.button))
    {
      c = Capture{}; // the page is gone
    }
    if (c.button)
    {
      Listen(ctx, base);
      Silence(ctx, base, state);
      return true;
    }
    if (c.swallowing)
    {
      if (!AnyKeyDown() || static_cast<int32_t>(GetTickCount() - c.swallow_end) >= 0)
      {
        c.swallowing = false;
      }
      else
      {
        Silence(ctx, base, state);
        return true;
      }
    }

    // The Controls page on top: A on a key's row waits for a key
    // (the page would take it as Accept).
    uint32_t client = g_controls_scene && IsPage(base, g_controls_scene, Page::kControls)
                          ? LoadU32(base, g_controls_scene + kSceneClient)
                          : 0;
    bool on_top = client && TopScene(base, client) == g_controls_scene;
    if (on_top && g_defaults_armed && GetTickCount() - g_defaults_armed > 4000)
    {
      g_defaults_armed = 0;
      SetCaption(ctx, base, g_defaults_button, "Default keys");
    }
    uint8_t *p = base + state;
    bool a = ctx.r3.u32 == 0 && ((p[4] << 8 | p[5]) & kButtonA);
    bool &prev = g_prev_a[user & 3];
    bool pressed = a && !prev;
    prev = a;
    if (!pressed)
    {
      return false;
    }
    // The pause menu's Options on top: A on "Graphics and controls" opens
    // the Graphics page (the screen would take it as Accept and close).
    uint32_t pause_client = IsPage(base, g_pause_options, Page::kPauseOptions)
                                ? LoadU32(base, g_pause_options + kSceneClient)
                                : 0;
    if (pause_client && TopScene(base, pause_client) == g_pause_options)
    {
      for (const auto &[button, row] : g_rows)
      {
        if (row.page == Page::kPauseOptions && Focused(base, button))
        {
          Silence(ctx, base, state);
          OpenFromPause(ctx, base);
          return true;
        }
      }
      return false;
    }
    if (!on_top)
    {
      return false;
    }
    for (int i = 1; i <= kSectionRows; ++i)
    {
      uint32_t button = g_control_buttons[i];
      const Row *row = button ? RowOf(base, button) : nullptr;
      if (row && BindOf(*row) && Focused(base, button))
      {
        StartCapture(ctx, base, button, *row, false);
        Silence(ctx, base, state);
        return true;
      }
    }
    return false;
  }
} // namespace ao2::gfxmenu

// OnlineSubsystemLive::ShowDeviceSelectionUI(this, user, size needed, force).
extern "C" REX_FUNC(__imp__sub_82AF8380);
extern "C" REX_FUNC(sub_82AF8380)
{
  if (DeviceButton(ctx, base))
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
    const Setting *setting = SettingOf(*row);
    ctx.r3.u64 = setting ? setting->choices.size() : 2;
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
    const Setting *setting = SettingOf(*row);
    if (!setting)
    {
      ctx.r3.u64 = row->page != Page::kPauseOptions; // a key bind: left adds a key; "Open": no arrows
      return;
    }
    bool wrap = LoadU32(base, button + kWrapWord) & kWrapMask;
    int count = static_cast<int>(setting->choices.size());
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
    const Setting *setting = SettingOf(*row);
    if (!setting)
    {
      ctx.r3.u64 = row->page != Page::kPauseOptions; // a key bind: right replaces its key
      return;
    }
    bool wrap = LoadU32(base, button + kWrapWord) & kWrapMask;
    int count = static_cast<int>(setting->choices.size());
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
    const Setting *setting = SettingOf(r);
    if (!setting)
    {
      if (r.page == Page::kPauseOptions)
      {
        OpenFromPause(ctx, base);
        return;
      }
      StartCapture(ctx, base, button, r, true);
      return;
    }
    bool wrap = LoadU32(base, button + kWrapWord) & kWrapMask;
    int count = static_cast<int>(setting->choices.size());
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
    const Setting *setting = SettingOf(r);
    if (!setting)
    {
      if (r.page == Page::kPauseOptions)
      {
        OpenFromPause(ctx, base);
        return;
      }
      StartCapture(ctx, base, button, r, false);
      return;
    }
    bool wrap = LoadU32(base, button + kWrapWord) & kWrapMask;
    int count = static_cast<int>(setting->choices.size());
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
    const Setting *setting = SettingOf(*row);
    int index = static_cast<int32_t>(ctx.r4.u32);
    uint32_t out = ctx.r5.u32;
    int count = setting ? static_cast<int>(setting->choices.size()) : 2;
    if (index < 0 || index >= count || !out)
    {
      ctx.r3.u64 = 0;
      return;
    }
    Scratch s(ctx);
    MakeString(ctx, base, s, s.String(0),
               setting ? GameTr(setting->choices[index].name) : RowText(*row));
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
    if (const Setting *setting = SettingOf(*row))
    {
      ShowIndex(ctx, base, button, *setting, setting->chosen);
    }
    else if (button == g_capture.button)
    {
      ShowText(ctx, base, button, GameTr("Press a key...") + " " + std::to_string(g_capture.shown));
    }
    else
    {
      ShowText(ctx, base, button, RowText(*row));
    }
    ctx.r3.u64 = 1;
    return;
  }
  if (!g_opening && NotePauseOptions(ctx, base, button))
  {
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
    // A page's Accept publishes every widget's value (UIAction_PublishValue).
    Setting *setting = SettingOf(*row);
    if (row->page == Page::kGraphics && setting == &GraphicsSectionSetting())
    {
      // Once per Accept (the section row is always shown): every setting
      // waiting for it, whichever section it is in.
      for (Setting &s : Settings())
      {
        if (s.when == When::kAccept)
        {
          Logf("publish: %s\n", s.cvar);
          Accept(s);
        }
      }
    }
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_8235ADD8(ctx, base);
}
