// ui_theme.h - The port's own screens in the launcher's look (see ui_theme.cpp):
// the cover's deep navy, a warm paper accent, hairlines, letter-spaced
// capitals, and small animations.
#pragma once

#include <imgui.h>

namespace ao2::ui
{
  enum class FontKind
  {
    kTitle,   // Segoe UI Semibold: screen titles
    kHeading, // Segoe UI Semibold: names, card titles
    kBody,    // Segoe UI: text, buttons
    kSmall,   // Segoe UI: labels, hints
    kLabel,   // Segoe UI Semibold: captions in letter-spaced capitals (buttons, sections)
  };

  // ImGui font setup of a drawer (inside its ConfigureFonts, its context
  // current): adds the theme's fonts to `atlas`. Leaves FontDefault alone.
  void AddFonts(ImFontAtlas *atlas);

  // Inside an ImGui frame: the font of the current context (its default font
  // when the theme's could not be loaded) and its size on this screen.
  ImFont *Font(FontKind kind);
  float FontSize(FontKind kind);
  // The screen's scale against 1080 lines.
  float Scale();

  // Colours (alpha 255).
  constexpr ImU32 kAccent = IM_COL32(224, 220, 212, 255);      // the cover's paper
  constexpr ImU32 kAccentLight = IM_COL32(246, 244, 238, 255);
  constexpr ImU32 kInk = IM_COL32(6, 16, 34, 255);             // text on the accent
  constexpr ImU32 kPanel = IM_COL32(10, 24, 48, 255);
  constexpr ImU32 kText = IM_COL32(220, 226, 236, 255);
  constexpr ImU32 kMuted = IM_COL32(140, 152, 170, 255);
  constexpr ImU32 kGreen = IM_COL32(120, 200, 140, 255);
  constexpr ImU32 kRed = IM_COL32(232, 104, 96, 255);
  constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);

  // `col` with its alpha multiplied by `alpha` (0..1).
  ImU32 Fade(ImU32 col, float alpha);
  ImU32 Mix(ImU32 a, ImU32 b, float t);

  float Clamp01(float t);
  float EaseOutCubic(float t);
  float EaseOutBack(float t);
  // Moves `current` toward `target` at `speed` (1/s), frame rate independent.
  float Approach(float current, float target, float speed);

  ImVec2 TextSize(FontKind kind, const char *text);
  void Text(ImDrawList *dl, FontKind kind, ImVec2 pos, ImU32 col, const char *text);
  // Capitals with extra space between the letters (titles, labels, buttons),
  // like the launcher's captions.
  ImVec2 CapsSize(FontKind kind, const char *text);
  void Caps(ImDrawList *dl, FontKind kind, ImVec2 pos, ImU32 col, const char *text);
  // Military corner brackets around a rectangle.
  void Corners(ImDrawList *dl, ImVec2 min, ImVec2 max, float length, float thickness, ImU32 col);
  // Diagonal hazard stripes filling a rectangle, scrolled by `offset` px.
  void Stripes(ImDrawList *dl, ImVec2 min, ImVec2 max, float width, float offset, ImU32 col);
  // A spinning arc (busy).
  void Spinner(ImDrawList *dl, ImVec2 center, float radius, float thickness, ImU32 col);
  void Check(ImDrawList *dl, ImVec2 center, float size, float thickness, ImU32 col);
  void Cross(ImDrawList *dl, ImVec2 center, float size, float thickness, ImU32 col);
  // A key cap ("F6") at `pos` (top left), `height` tall; returns its width.
  float KeyCap(ImDrawList *dl, ImVec2 pos, float height, const char *key, float alpha);

  enum class ButtonStyle
  {
    kPrimary, // paper, filled
    kOutline, // hairline frame
    kDanger,  // red outline
  };
  // A button at `pos` (screen) of `size`, in the current window; true when
  // clicked. Disabled: greyed, never clicked.
  bool Button(const char *id, ImVec2 pos, ImVec2 size, const char *label, ButtonStyle style, bool enabled,
              float alpha);
} // namespace ao2::ui
