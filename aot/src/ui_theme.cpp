// ui_theme.cpp - The port's own screens in the launcher's look.
//
// Flat and quiet, in the tones of the game's cover: deep navy panels, a warm
// paper accent, hairlines, and letter-spaced semibold capitals for titles and
// buttons (as on the launcher, launcher\src\Skin.cs). The fonts are Windows'
// Segoe UI and Segoe UI Semibold, both with Cyrillic. The SDK's ImGui drawers
// bake the atlas once (no dynamic font textures), so the fonts are baked at
// the screen's scale and drawn at the window's.
#include "ui_theme.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

namespace
{
  constexpr int kKinds = 5;
  constexpr float kBaseSize[kKinds] = {34.0f, 23.0f, 18.0f, 15.0f, 14.0f};
  // Letter spacing of Caps, as a share of the font size.
  constexpr float kTracking = 0.14f;

  struct AtlasFonts
  {
    ImFontAtlas *atlas = nullptr;
    ImFont *fonts[kKinds] = {};
  };
  std::vector<AtlasFonts> g_atlases; // UI thread

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
    for (const char *path : paths)
    {
      if (!Exists(path))
      {
        continue;
      }
      ImFontConfig config;
      config.OversampleH = 2;
      config.OversampleV = 1;
      if (ImFont *font = atlas->AddFontFromFileTTF(path, size, &config, kRanges))
      {
        return font;
      }
    }
    return nullptr;
  }
} // namespace

namespace ao2::ui
{
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
      fonts.fonts[k] =
          display ? Load(atlas, {"C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\segoeui.ttf",
                                 "C:\\Windows\\Fonts\\arialbd.ttf"},
                         kBaseSize[k] * screen)
                  : Load(atlas, {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf"},
                         kBaseSize[k] * screen);
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
    return std::clamp(ImGui::GetMainViewport()->Size.y / 1080.0f, 0.7f, 2.5f);
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

  ImVec2 CapsSize(FontKind kind, const char *text)
  {
    float size = FontSize(kind);
    int letters = 0;
    for (const char *p = text; *p; p += Utf8Length(p))
    {
      ++letters;
    }
    ImVec2 plain = Font(kind)->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    return ImVec2(plain.x + size * kTracking * float(std::max(letters - 1, 0)), plain.y);
  }

  void Caps(ImDrawList *dl, FontKind kind, ImVec2 pos, ImU32 col, const char *text)
  {
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

  void Corners(ImDrawList *dl, ImVec2 min, ImVec2 max, float length, float thickness, ImU32 col)
  {
    auto corner = [&](ImVec2 p, float dx, float dy) {
      ImVec2 pts[3] = {ImVec2(p.x + dx * length, p.y), p, ImVec2(p.x, p.y + dy * length)};
      dl->AddPolyline(pts, 3, col, ImDrawFlags_None, thickness);
    };
    corner(min, 1.0f, 1.0f);
    corner(ImVec2(max.x, min.y), -1.0f, 1.0f);
    corner(ImVec2(min.x, max.y), 1.0f, -1.0f);
    corner(max, -1.0f, -1.0f);
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

  float KeyCap(ImDrawList *dl, ImVec2 pos, float height, const char *key, float alpha)
  {
    float s = Scale();
    ImVec2 ts = CapsSize(FontKind::kLabel, key);
    float w = std::max(height, ts.x + height * 0.8f);
    ImVec2 max(pos.x + w, pos.y + height);
    dl->AddRect(pos, max, Fade(kAccent, 0.47f * alpha), 0.0f, ImDrawFlags_None, 1.0f * s);
    dl->AddLine(ImVec2(pos.x + 1.0f * s, max.y + 0.5f * s), ImVec2(max.x - 1.0f * s, max.y + 0.5f * s),
                Fade(kAccent, 0.47f * alpha), 1.0f * s);
    Caps(dl, FontKind::kLabel, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (height - ts.y) * 0.5f), Fade(kText, alpha),
         key);
    return w;
  }

  // As on the launcher: primary is a paper block with ink capitals, the rest
  // a hairline frame that fills lightly on hover; danger has a red frame.
  // A press darkens the block.
  bool Button(const char *id, ImVec2 pos, ImVec2 size, const char *label, ButtonStyle style, bool enabled,
              float alpha)
  {
    float s = Scale();
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(pos);
    bool clicked = ImGui::InvisibleButton("##button", size) && enabled;
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
    if (!enabled)
    {
      dl->AddRectFilled(min, max, Fade(kWhite, 0.05f * alpha));
      dl->AddRect(min, max, Fade(kWhite, 0.16f * alpha), 0.0f, ImDrawFlags_None, 1.0f * s);
      text = Fade(kMuted, 0.7f * alpha);
    }
    else if (style == ButtonStyle::kPrimary)
    {
      ImU32 base = IM_COL32(214, 210, 202, 255);
      dl->AddRectFilled(min, max, Fade(held ? kAccent : Mix(base, kAccentLight, h), alpha));
      if (h > 0.01f)
      {
        ImVec2 o(3.0f * s, 3.0f * s);
        dl->AddRect(ImVec2(min.x - o.x, min.y - o.y), ImVec2(max.x + o.x, max.y + o.y),
                    Fade(kAccentLight, 0.35f * h * alpha), 0.0f, ImDrawFlags_None, 1.0f * s);
      }
      text = Fade(kInk, alpha);
    }
    else
    {
      ImU32 frame = style == ButtonStyle::kDanger ? kRed : kAccent;
      dl->AddRectFilled(min, max, Fade(kWhite, ((held ? 0.18f : 0.0f) + 0.10f * h) * alpha));
      dl->AddRect(min, max, Fade(frame, (0.30f + 0.45f * h) * alpha), 0.0f, ImDrawFlags_None, 1.0f * s);
      text = Fade(Mix(kText, kAccentLight, h), alpha);
    }
    ImVec2 ts = CapsSize(FontKind::kLabel, label);
    Caps(dl, FontKind::kLabel, ImVec2(min.x + (size.x - ts.x) * 0.5f, min.y + (size.y - ts.y) * 0.5f), text, label);
    return clicked;
  }} // namespace ao2::ui
