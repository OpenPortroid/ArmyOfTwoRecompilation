// ui_theme.h - The port's own screens in the launcher's look (see ui_theme.cpp):
// rounded panels of dark glass, one cold accent, sentence case, and small
// animations.
#pragma once

#include <imgui.h>

#include <initializer_list>
#include <string>
#include <vector>

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

  // The font files to try for a screen, the best first: those of `system_paths` that exist, then the package's Tektur,
  // for a system without them (Wine, Proton). Each has Latin-1 and Cyrillic.
  std::vector<std::string> FontFiles(std::initializer_list<const char *> system_paths);

  // ImGui font setup of a drawer (inside its ConfigureFonts, its context
  // current): adds the theme's fonts to `atlas`. Leaves FontDefault alone.
  void AddFonts(ImFontAtlas *atlas);

  // Inside an ImGui frame: the font of the current context (its default font
  // when the theme's could not be loaded) and its size on this screen.
  ImFont *Font(FontKind kind);
  float FontSize(FontKind kind);
  // The screen's scale against 1080 lines, and the zoom of what is being drawn.
  float Scale();
  // While alive, Scale() and the fonts are `zoom` times bigger (1..): a screen that wants to be larger
  // than the others (its text stays sharp up to kBakedZoom).
  constexpr float kBakedZoom = 1.3f;
  struct ZoomScope
  {
    explicit ZoomScope(float zoom);
    ~ZoomScope();
    ZoomScope(const ZoomScope &) = delete;
    ZoomScope &operator=(const ZoomScope &) = delete;

  private:
    float previous_;
  };

  // Colours (alpha 255).
  constexpr ImU32 kAccent = IM_COL32(143, 211, 255, 255);
  constexpr ImU32 kAccentLight = IM_COL32(244, 247, 251, 255);
  constexpr ImU32 kInk = IM_COL32(7, 16, 31, 255);             // text on the accent
  constexpr ImU32 kPanel = IM_COL32(14, 26, 48, 255);
  constexpr ImU32 kText = IM_COL32(226, 232, 240, 255);
  constexpr ImU32 kMuted = IM_COL32(176, 188, 204, 255);
  constexpr ImU32 kGreen = IM_COL32(120, 200, 140, 255);
  constexpr ImU32 kRed = IM_COL32(232, 104, 96, 255);
  constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);

  // A corner radius for `px` (at 1080 lines): the boxes' slight rounding.
  float Round(float px);
  // A row's or box's back (`fill`, with a soft shadow), its frame, and the
  // state mark inside its left edge.
  void Card(ImDrawList *dl, ImVec2 min, ImVec2 max, ImU32 fill);
  void Frame(ImDrawList *dl, ImVec2 min, ImVec2 max, ImU32 col, float thickness);
  void Bar(ImDrawList *dl, ImVec2 min, ImVec2 max, ImU32 col);

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
  // Titles, labels, buttons: text keyed in capitals is drawn in sentence case
  // (abbreviations and words with digits stay), like the launcher's captions.
  ImVec2 CapsSize(FontKind kind, const char *text);
  void Caps(ImDrawList *dl, FontKind kind, ImVec2 pos, ImU32 col, const char *text);
  // Diagonal hazard stripes filling a rectangle, scrolled by `offset` px.
  void Stripes(ImDrawList *dl, ImVec2 min, ImVec2 max, float width, float offset, ImU32 col);
  // A spinning arc (busy).
  void Spinner(ImDrawList *dl, ImVec2 center, float radius, float thickness, ImU32 col);
  void Check(ImDrawList *dl, ImVec2 center, float size, float thickness, ImU32 col);
  void Cross(ImDrawList *dl, ImVec2 center, float size, float thickness, ImU32 col);
  // A key cap ("F6") at `pos` (top left), `height` tall; returns its width.
  float KeyCap(ImDrawList *dl, ImVec2 pos, float height, const char *key, float alpha,
               FontKind kind = FontKind::kLabel);

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
