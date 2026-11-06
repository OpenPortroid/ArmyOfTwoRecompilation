// ui_theme.cpp - The port's own screens in the launcher's look.
//
// Rounded panels of dark glass, one cold accent and sentence case (as on the
// launcher, launcher\src\Skin.cs). The fonts are Windows'
// Segoe UI and Segoe UI Semibold, both with Cyrillic; where they are missing
// (Wine, Proton) the Tektur font of the package, as ImGui's own has no Cyrillic.
// The SDK's ImGui drawers bake the atlas once (no dynamic font textures), so the
// fonts are baked at the screen's scale and drawn at the window's.
#include "ui_theme.h"

#include "utils.h"

#include <rex/cvar.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

REXCVAR_DEFINE_BOOL(ao2_ui_system_fonts, true, "Debug",
                    "Draw the port's screens with Windows' fonts; off: with the package's Tektur, as where they are "
                    "missing (Wine, Proton)");

namespace
{
  constexpr int kKinds = 5;
  constexpr float kBaseSize[kKinds] = {34.0f, 23.0f, 18.0f, 15.0f, 14.0f};
  // Letter spacing of Caps, as a share of the font size.
  constexpr float kTracking = 0.012f;

  // Words that stay capitals when text keyed in capitals goes to sentence case.
  constexpr const char *kKeepCaps[] = {"DLC", "FPS", "IP",  "LAN", "PC",  "HDR", "SSAO", "HBAO", "GTAO", "FXAA",
                                       "SMAA", "ESC", "TAB", "LB",  "RB",  "LT",  "RT",   "OK",   "VS"};

  // Text with no small letter (ASCII, Latin-1, Cyrillic), in sentence case.
  std::string SentenceCase(const char *text)
  {
    const auto *p = reinterpret_cast<const unsigned char *>(text);
    for (const unsigned char *q = p; *q; ++q)
    {
      bool ascii = *q >= 'a' && *q <= 'z';
      bool latin1 = *q == 0xC3 && q[1] >= 0xA0 && q[1] != 0xB7;
      bool cyrillic = (*q == 0xD0 && q[1] >= 0xB0) || *q == 0xD1;
      if (ascii || latin1 || cyrillic)
      {
        return text;
      }
    }
    std::string out(text);
    bool first = true;
    for (size_t at = 0; at < out.size();)
    {
      size_t end = out.find(' ', at);
      end = end == std::string::npos ? out.size() : end;
      std::string word = out.substr(at, end - at);
      bool letter = false, digit = false;
      for (unsigned char c : word)
      {
        letter = letter || (c >= 'A' && c <= 'Z') || c >= 0xC3;
        digit = digit || (c >= '0' && c <= '9');
      }
      bool keep = !letter || digit;
      for (const char *k : kKeepCaps)
      {
        keep = keep || word == k;
      }
      if (!keep)
      {
        bool capital = first;
        for (size_t i = at; i < end;)
        {
          unsigned char c = out[i];
          if (c < 0x80)
          {
            if (!capital && c >= 'A' && c <= 'Z')
            {
              out[i] = char(c + 32);
            }
            capital = capital && !(c >= 'A' && c <= 'Z');
            ++i;
          }
          else if (i + 1 < end && c == 0xC3 && (unsigned char)out[i + 1] < 0xA0)
          {
            if (!capital && (unsigned char)out[i + 1] != 0x97)
            {
              out[i + 1] = char((unsigned char)out[i + 1] + 0x20);
            }
            capital = false;
            i += 2;
          }
          else if (i + 1 < end && c == 0xD0)
          {
            unsigned char d = out[i + 1];
            if (!capital)
            {
              if (d == 0x81) // Yo
              {
                out[i] = char(0xD1);
                out[i + 1] = char(0x91);
              }
              else if (d >= 0x90 && d <= 0x9F) // A..Pe
              {
                out[i + 1] = char(d + 0x20);
              }
              else if (d >= 0xA0 && d <= 0xAF) // Er..Ya
              {
                out[i] = char(0xD1);
                out[i + 1] = char(d - 0x20);
              }
            }
            capital = false;
            i += 2;
          }
          else
          {
            ++i;
          }
        }
      }
      first = first && !letter;
      at = end + 1;
    }
    return out;
  }

  struct AtlasFonts
  {
    ImFontAtlas *atlas = nullptr;
    ImFont *fonts[kKinds] = {};
    ImFont *zoomed[kKinds] = {}; // baked kBakedZoom times bigger
  };
  std::vector<AtlasFonts> g_atlases; // UI thread
  float g_zoom = 1.0f;               // UI thread

  bool Exists(const char *path)
  {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
  }

  ImFont *Load(ImFontAtlas *atlas, std::initializer_list<const char *> paths, float size)
  {
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF, // Basic Latin, Latin-1
        0x0400, 0x04FF, // Cyrillic
        0x2010, 0x205E, // punctuation
        0x2190, 0x2193, // arrows
        0,
    };
    for (const std::string &path : ao2::ui::FontFiles(paths))
    {
      ImFontConfig config;
      config.OversampleH = 2;
      config.OversampleV = 1;
      if (ImFont *font = atlas->AddFontFromFileTTF(path.c_str(), size, &config, kRanges))
      {
        return font;
      }
    }
    return nullptr;
  }
} // namespace

namespace ao2::ui
{
  std::vector<std::string> FontFiles(std::initializer_list<const char *> system_paths)
  {
    std::vector<std::string> files;
    if (REXCVAR_GET(ao2_ui_system_fonts))
    {
      for (const char *path : system_paths)
      {
        if (Exists(path))
        {
          files.emplace_back(path);
        }
      }
    }
    std::string own = (utils::RepoRoot() / "localization" / "fonts" / "Tektur.ttf").string();
    if (Exists(own.c_str()))
    {
      files.push_back(std::move(own));
    }
    return files;
  }

  void AddFonts(ImFontAtlas *atlas)
  {
    // Baked for the screen the game most likely fills.
    float screen = std::clamp(float(GetSystemMetrics(SM_CYSCREEN)) / 1080.0f, 1.0f, 2.5f);
    ImFont *previous_default = ImGui::GetIO().FontDefault;
    if (!previous_default && atlas->Fonts.empty())
    {
      previous_default = atlas->AddFontDefault(); // the first font would be the default
    }
    AtlasFonts fonts;
    fonts.atlas = atlas;
    for (int k = 0; k < kKinds; ++k)
    {
      bool display = k <= int(FontKind::kHeading) || k == int(FontKind::kLabel);
      auto bake = [&](float size) {
        return display ? Load(atlas, {"C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\segoeui.ttf",
                                      "C:\\Windows\\Fonts\\arialbd.ttf"},
                              size)
                       : Load(atlas, {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf"}, size);
      };
      fonts.fonts[k] = bake(kBaseSize[k] * screen);
      fonts.zoomed[k] = bake(kBaseSize[k] * screen * kBakedZoom);
    }
    if (previous_default)
    {
      ImGui::GetIO().FontDefault = previous_default;
    }
    std::erase_if(g_atlases, [&](const AtlasFonts &a) { return a.atlas == atlas; });
    g_atlases.push_back(fonts);
  }

  ImFont *Font(FontKind kind)
  {
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    for (const AtlasFonts &a : g_atlases)
    {
      if (a.atlas == atlas && g_zoom > 1.01f && a.zoomed[int(kind)])
      {
        return a.zoomed[int(kind)];
      }
      if (a.atlas == atlas && a.fonts[int(kind)])
      {
        return a.fonts[int(kind)];
      }
    }
    return ImGui::GetFont();
  }

  float FontSize(FontKind kind)
  {
    return kBaseSize[int(kind)] * Scale();
  }

  float Scale()
  {
    return std::clamp(ImGui::GetMainViewport()->Size.y / 1080.0f, 0.7f, 2.5f) * g_zoom;
  }

  ZoomScope::ZoomScope(float zoom) : previous_(g_zoom)
  {
    g_zoom = zoom;
  }

  ZoomScope::~ZoomScope()
  {
    g_zoom = previous_;
  }

  ImU32 Fade(ImU32 col, float alpha)
  {
    uint32_t a = (col >> IM_COL32_A_SHIFT) & 0xFF;
    a = uint32_t(float(a) * Clamp01(alpha) + 0.5f);
    return (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
  }

  ImU32 Mix(ImU32 a, ImU32 b, float t)
  {
    t = Clamp01(t);
    ImU32 out = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
      float x = float((a >> shift) & 0xFF), y = float((b >> shift) & 0xFF);
      out |= ImU32(x + (y - x) * t + 0.5f) << shift;
    }
    return out;
  }

  float Clamp01(float t)
  {
    return std::clamp(t, 0.0f, 1.0f);
  }

  float EaseOutCubic(float t)
  {
    t = 1.0f - Clamp01(t);
    return 1.0f - t * t * t;
  }

  float EaseOutBack(float t)
  {
    t = Clamp01(t) - 1.0f;
    constexpr float c = 1.70158f;
    return 1.0f + (c + 1.0f) * t * t * t + c * t * t;
  }

  float Approach(float current, float target, float speed)
  {
    float k = 1.0f - std::exp(-speed * ImGui::GetIO().DeltaTime);
    return current + (target - current) * k;
  }

  ImVec2 TextSize(FontKind kind, const char *text)
  {
    return Font(kind)->CalcTextSizeA(FontSize(kind), FLT_MAX, 0.0f, text);
  }

  void Text(ImDrawList *dl, FontKind kind, ImVec2 pos, ImU32 col, const char *text)
  {
    dl->AddText(Font(kind), FontSize(kind), ImVec2(std::floor(pos.x), std::floor(pos.y)), col, text);
  }

  // Bytes of the UTF-8 character starting at `p`.
  static int Utf8Length(const char *p)
  {
    unsigned char c = static_cast<unsigned char>(*p);
    int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
    for (int i = 1; i < n; ++i)
    {
      if (!p[i])
      {
        return i;
      }
    }
    return n;
  }

  ImVec2 CapsSize(FontKind kind, const char *keyed)
  {
    std::string sentence = SentenceCase(keyed);
    const char *text = sentence.c_str();
    float size = FontSize(kind);
    int letters = 0;
    for (const char *p = text; *p; p += Utf8Length(p))
    {
      ++letters;
    }
    ImVec2 plain = Font(kind)->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    return ImVec2(plain.x + size * kTracking * float(std::max(letters - 1, 0)), plain.y);
  }

  void Caps(ImDrawList *dl, FontKind kind, ImVec2 pos, ImU32 col, const char *keyed)
  {
    std::string sentence = SentenceCase(keyed);
    const char *text = sentence.c_str();
    ImFont *font = Font(kind);
    float size = FontSize(kind), track = size * kTracking;
    float x = std::floor(pos.x), y = std::floor(pos.y);
    for (const char *p = text; *p;)
    {
      int n = Utf8Length(p);
      dl->AddText(font, size, ImVec2(x, y), col, p, p + n);
      x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, p, p + n).x + track;
      p += n;
    }
  }

  void Stripes(ImDrawList *dl, ImVec2 min, ImVec2 max, float width, float offset, ImU32 col)
  {
    float h = max.y - min.y;
    float period = width * 2.0f;
    dl->PushClipRect(min, max, true);
    for (float x = min.x - h - period + std::fmod(offset, period); x < max.x; x += period)
    {
      dl->AddQuadFilled(ImVec2(x, max.y), ImVec2(x + h, min.y), ImVec2(x + h + width, min.y),
                        ImVec2(x + width, max.y), col);
    }
    dl->PopClipRect();
  }

  void Spinner(ImDrawList *dl, ImVec2 center, float radius, float thickness, ImU32 col)
  {
    float a = float(ImGui::GetTime()) * 6.0f;
    dl->PathArcTo(center, radius, a, a + 4.2f, 24);
    dl->PathStroke(col, ImDrawFlags_None, thickness);
  }

  void Check(ImDrawList *dl, ImVec2 c, float size, float thickness, ImU32 col)
  {
    ImVec2 pts[3] = {ImVec2(c.x - 0.5f * size, c.y), ImVec2(c.x - 0.15f * size, c.y + 0.35f * size),
                     ImVec2(c.x + 0.5f * size, c.y - 0.35f * size)};
    dl->AddPolyline(pts, 3, col, ImDrawFlags_None, thickness);
  }

  void Cross(ImDrawList *dl, ImVec2 c, float size, float thickness, ImU32 col)
  {
    float h = 0.4f * size;
    dl->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col, thickness);
    dl->AddLine(ImVec2(c.x - h, c.y + h), ImVec2(c.x + h, c.y - h), col, thickness);
  }

  float Round(float px)
  {
    return px * Scale();
  }

  // A soft shadow under it.
  void Card(ImDrawList *dl, ImVec2 min, ImVec2 max, ImU32 fill)
  {
    float s = Scale();
    float r = Round(14.0f);
    float fa = float((fill >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    float sa = std::min(0.22f, fa * 3.0f);
    dl->AddRectFilled(ImVec2(min.x, min.y + 2.0f * s), ImVec2(max.x, max.y + 2.0f * s), Fade(kInk, sa), r);
    dl->AddRectFilled(min, max, fill, r);
  }

  void Frame(ImDrawList *dl, ImVec2 min, ImVec2 max, ImU32 col, float thickness)
  {
    dl->AddRect(min, max, col, Round(14.0f), ImDrawFlags_None, thickness);
  }

  void Bar(ImDrawList *dl, ImVec2 min, ImVec2 max, ImU32 col)
  {
    float s = Scale(), inset = std::min(16.0f * s, (max.y - min.y) * 0.3f);
    dl->AddRectFilled(ImVec2(min.x + 8.0f * s, min.y + inset), ImVec2(min.x + 11.0f * s, max.y - inset), col, 1.5f * s);
  }

  float KeyCap(ImDrawList *dl, ImVec2 pos, float height, const char *key, float alpha)
  {
    float s = Scale();
    ImVec2 ts = CapsSize(FontKind::kLabel, key);
    float w = std::max(height, ts.x + height * 0.8f);
    ImVec2 max(pos.x + w, pos.y + height);
    dl->AddRectFilled(pos, max, Fade(kAccentLight, alpha), 6.0f * s);
    Caps(dl, FontKind::kLabel, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (height - ts.y) * 0.5f), Fade(kInk, alpha),
         key);
    return w;
  }

  // As on the launcher: primary is a white block with dark lettering, the rest
  // faint glass that brightens on hover; danger has a red frame.
  // A press tints the block.
  bool Button(const char *id, ImVec2 pos, ImVec2 size, const char *label, ButtonStyle style, bool enabled,
              float alpha)
  {
    float s = Scale();
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(pos);
    bool clicked = ImGui::InvisibleButton("##button", size, ImGuiButtonFlags_EnableNav) && enabled;
    bool hovered = enabled && ImGui::IsItemHovered();
    bool held = enabled && ImGui::IsItemActive();
    float &h = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID("hover"), 0.0f);
    h = Approach(h, hovered ? 1.0f : 0.0f, 20.0f);
    ImGui::PopID();
    if (hovered)
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 min = pos, max(pos.x + size.x, pos.y + size.y);
    ImU32 text;
    const float r = 10.0f * s;
    if (!enabled)
    {
      dl->AddRectFilled(min, max, Fade(kWhite, 0.05f * alpha), r);
      dl->AddRect(min, max, Fade(kWhite, 0.16f * alpha), r, ImDrawFlags_None, 1.0f * s);
      text = Fade(kMuted, 0.7f * alpha);
    }
    else if (style == ButtonStyle::kPrimary)
    {
      ImU32 base = IM_COL32(226, 232, 240, 255);
      dl->AddRectFilled(ImVec2(min.x, min.y + 3.0f * s), ImVec2(max.x, max.y + 3.0f * s),
                        Fade(kInk, (0.35f + 0.15f * h) * alpha), r);
      dl->AddRectFilled(min, max, Fade(held ? kAccent : Mix(base, kAccentLight, h), alpha), r);
      if (h > 0.01f)
      {
        ImVec2 o(3.0f * s, 3.0f * s);
        dl->AddRect(ImVec2(min.x - o.x, min.y - o.y), ImVec2(max.x + o.x, max.y + o.y),
                    Fade(kAccentLight, 0.35f * h * alpha), r + o.x, ImDrawFlags_None, 1.0f * s);
      }
      text = Fade(kInk, alpha);
    }
    else
    {
      dl->AddRectFilled(min, max, Fade(kWhite, (0.09f + (held ? 0.14f : 0.0f) + 0.09f * h) * alpha), r);
      if (style == ButtonStyle::kDanger)
      {
        dl->AddRect(min, max, Fade(kRed, (0.45f + 0.45f * h) * alpha), r, ImDrawFlags_None, 1.0f * s);
      }
      text = Fade(Mix(kText, kAccentLight, h), alpha);
    }
    ImVec2 ts = CapsSize(FontKind::kLabel, label);
    Caps(dl, FontKind::kLabel, ImVec2(min.x + (size.x - ts.x) * 0.5f, min.y + (size.y - ts.y) * 0.5f), text, label);
    return clicked;
  }} // namespace ao2::ui
