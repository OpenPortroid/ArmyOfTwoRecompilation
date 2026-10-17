// keyboard_prompts.cpp - Keyboard keys instead of controller buttons wherever
// the game shows a button (ao2_keyboard_prompts).
//
// Every button picture of the game - in tutorials, hints, menu button bars,
// the HUD - is a character of the font 00_fonts.Xbox360_18pt: its first page
// (DXT5 256x256) is an atlas of Xbox 360 buttons, each character a rectangle of
// it (StartU, StartV, USize, VSize in page texels, all 26 high). Text asks for
// one with UI markup: "press <Fonts:00_fonts.Xbox360_18pt>A<Fonts:/>".
// Characters: A B X Y the face buttons; ] w left trigger, ^ x right trigger,
// _ y left bumper, ` z right bumper; : < Back, ; > Start; k and L-T the left
// stick (T-pose and directions), i and C-K the right stick; j / h the left /
// right stick click; l m o q s the D-pad (all, up, right, down, left). The rest
// of the page (arrows, envelopes, the guide button) stays as it is.
//
// The native renderer recognizes the page by its data (the disc's 64 KB) and
// samples a replacement instead: the original page at 2-4x with the button
// pictures cleared and a picture of each button's key (the first key of its
// keybind_* in mapping.toml) drawn in the free room - a keycap with the key's
// name in the heavy black letters of the controller's own pictures (Arial
// Black, as LB / RT) or an arrow, a mouse with the button lit. Once that page
// exists, the game thread moves the font's button characters to the new pictures, which
// are as wide as their key needs ("Backspace" is wider than "E").
//
// Some buttons are textures of their own, not characters: the context
// action's A ("climb", "sit"), reload's X, the co-op move's cancel B, the
// minigames', the multiplayer and shell menus' (00_hud, 00_shell, Startup).
// The renderer knows them by the XXH3 of their base level (the packages'
// cooked data is what the guest has in memory) and samples a picture of the
// bound key drawn where the button was instead.
//
// The text around a button names the controller ("press and hold left bumper
// <LB> and press D-pad down <D-pad down>"). Most tutorial strings have a PS3
// twin (Key_PS3 next to Key or Key_Xbox360) that names only the buttons ("hold
// <LB> and press <D-pad down>"); a string whose twin shows the same buttons
// takes the twin's text. English strings without a twin lose the controller
// words before a button ("D-pad right ", "left trigger "). The strings come from
// AO2Game\Localization\Coalesced.<lang> (each one a list of .int files: name,
// then the file's text, both length-prefixed). Xbox360TOC.txt gives every
// file's size and the engine reads that many bytes, so a rewritten file keeps
// its size: the text only gets shorter, the rest of each changed .int is
// filled with empty lines. game_language.cpp writes and mounts the result.
//
// The pictures follow player 1's hands (the runtime's rex_input_last_input):
// while a gamepad is in use, or was just plugged in, the renderer samples the
// guest's own page and button textures again and the font's characters go
// back to the disc's rectangles; a key press or a mouse move brings the keys
// back. The text stays rewritten either way ("hold <LB>" reads well with both).
//
// Split-screen with one player on the keyboard and the other on a gamepad
// (SplitRegions): each half of the screen shows its own player's pictures.
// The renderer finds where each prompt is on the screen (its vertices
// through the HUD's matrix) and which player's part of it (the local
// players' Origin / Size) that is: the keys in the keyboard player's, the
// guest's own texture in the other's; a prompt the players share is drawn
// twice side by side where it was, the key on the left, the button on the
// right. The font's characters stay on the disc's rectangles then, so both
// pages share them: the keys' page draws each button character's key into
// that character's own rectangle (a long key name gets narrow letters).
#include "keyboard_prompts.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "gpu/native/texture_loader.h"
#include "mouse_aim.h"
#include "ue3_reflect.h"

REXCVAR_DEFINE_BOOL(ao2_keyboard_prompts, true, "Input",
                    "Show keyboard keys (the first key of each keybind_*) instead of controller buttons: "
                    "tutorials, hints, menu button bars, the HUD");
REXCVAR_DEFINE_BOOL(ao2_keyboard_prompts_dump, false, "Debug",
                    "Debug: write the controller button pictures and their key pictures to the folder "
                    "keyboard_prompts_dump (BMP over a checkerboard)");

namespace ao2::prompts
{
  namespace
  {
    namespace fs = std::filesystem;

    // ---------------------------------------------------------------------
    // Keys of the binds
    // ---------------------------------------------------------------------

    std::string Trim(std::string_view s)
    {
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
      return std::string(s);
    }

    // A key of a bind ("Shift+Return") as it reads on the keyboard ("Shift+Enter").
    std::string KeyName(std::string_view token)
    {
      static const std::map<std::string, std::string, std::less<>> kNames = {
          {"Return", "Enter"}, {"NumpadEnter", "Enter"}, {"Escape", "Esc"},   {"Control", "Ctrl"},
          {"Semicolon", ";"},  {"Quote", "'"},           {"Comma", ","},      {"Period", "."},
          {"Slash", "/"},      {"Backslash", "\\"},      {"Minus", "-"},      {"Plus", "="},
          {"Backtick", "`"},   {"LBracket", "["},        {"RBracket", "]"},   {"PageUp", "PgUp"},
          {"PageDown", "PgDn"}, {"CapsLock", "Caps"},    {"Delete", "Del"},   {"Insert", "Ins"},
      };
      std::string out;
      while (!token.empty())
      {
        size_t plus = token.find('+');
        // "+" alone, or at the end, is a key name.
        std::string_view part = plus == std::string_view::npos || plus == 0 ? token : token.substr(0, plus);
        token.remove_prefix(part.size() == token.size() ? token.size() : plus + 1);
        auto it = kNames.find(part);
        if (!out.empty())
          out += '+';
        out += it != kNames.end() ? it->second : std::string(part);
      }
      return out;
    }

    // The first key of a keybind_* cvar as written in the bind ("Shift+Up"), "" if unbound.
    std::string FirstToken(const char *cvar)
    {
      std::string bind = rex::cvar::GetFlagByName(cvar);
      std::string_view rest = bind;
      while (!rest.empty())
      {
        size_t comma = rest.find(',');
        std::string key = Trim(rest.substr(0, comma));
        rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 1);
        // "none": nothing bound (the launcher's empty bind, as a command line
        // flag needs a value).
        if (!key.empty() && key != "none")
          return key;
      }
      return {};
    }

    // ---------------------------------------------------------------------
    // The pictures and where they go on the page
    // ---------------------------------------------------------------------

    struct Cap
    {
      enum Kind
      {
        kKey,    // keycap with `text`
        kArrow,  // keycap with an arrow (`arrow`: 0 up, 1 right, 2 down, 3 left)
        kArrows, // keycap with four arrows (the arrow keys)
        kMouse,  // mouse, `mouse` lit: 0 none, 1 left, 2 right, 3 wheel
      } kind = kKey;
      std::string text;
      int arrow = 0, mouse = 0;
      int u = 0, v = 0, w = 0; // page texels (height 26)
      bool operator<(const Cap &o) const
      {
        return std::tie(kind, text, arrow, mouse) < std::tie(o.kind, o.text, o.arrow, o.mouse);
      }
    };

    // Picture height in page texels, inside the 26-texel character cell.
    constexpr int kCellHeight = 26, kCapTop = 2, kCapHeight = 22;

    // Room on the page once the button pictures are gone: (u, width) per
    // character row (v); the pictures that stay keep a texel around them.
    struct FreeSpan
    {
      int v, u, w;
    };
    constexpr FreeSpan kFree[] = {
        {82, 0, 256},  {109, 0, 162}, {163, 79, 177}, {136, 32, 106}, {55, 44, 90},  {109, 195, 61},
        {55, 200, 56}, {190, 203, 53}, {217, 0, 57},  {55, 148, 30},  {190, 25, 26}, {190, 77, 26},
        {190, 129, 26}, {28, 237, 19}, {136, 242, 14}, {217, 238, 18},
    };

    // The characters each bind's picture replaces.
    struct ButtonChars
    {
      const char *chars;
      const char *cvar;
      const char *unbound; // the picture of a button without a key: its name
    };
    constexpr ButtonChars kButtons[] = {
        {"A", "keybind_a", "A"},
        {"B", "keybind_b", "B"},
        {"X", "keybind_x", "X"},
        {"Y", "keybind_y", "Y"},
        {"]w", "keybind_left_trigger", "LT"},
        {"^x", "keybind_right_trigger", "RT"},
        {"_y", "keybind_left_shoulder", "LB"},
        {"`z", "keybind_right_shoulder", "RB"},
        {":<", "keybind_back", "Back"},
        {";>", "keybind_start", "Start"},
        {"j", "keybind_lstick_press", "L3"},
        {"h", "keybind_rstick_press", "R3"},
        {"m", "keybind_dpad_up", "D-Up"},
        {"o", "keybind_dpad_right", "D-Right"},
        {"q", "keybind_dpad_down", "D-Down"},
        {"s", "keybind_dpad_left", "D-Left"},
    };
    constexpr const char *kLeftStick = "kLMNOPQRST";
    constexpr const char *kRightStick = "iCDEFGHIJK";
    constexpr const char *kDpad = "l";

    std::optional<Cap> CapForToken(const std::string &token)
    {
      if (token.empty())
        return std::nullopt;
      Cap cap;
      if (token == "LMB" || token == "RMB" || token == "MMB")
      {
        cap.kind = Cap::kMouse;
        cap.mouse = token == "LMB" ? 1 : token == "RMB" ? 2 : 3;
      }
      else if (token == "Up" || token == "Right" || token == "Down" || token == "Left")
      {
        cap.kind = Cap::kArrow;
        cap.arrow = token == "Up" ? 0 : token == "Right" ? 1 : token == "Down" ? 2 : 3;
      }
      else
      {
        cap.text = KeyName(token);
      }
      return cap;
    }

    // Four direction binds (up, left, down, right) as one picture: the arrow
    // keys, "WASD" when they are letters, else "1/2/3/4".
    std::optional<Cap> CapForDirections(const char *up, const char *left, const char *down, const char *right)
    {
      std::string tokens[] = {FirstToken(up), FirstToken(left), FirstToken(down), FirstToken(right)};
      if (tokens[0] == "Up" && tokens[1] == "Left" && tokens[2] == "Down" && tokens[3] == "Right")
      {
        Cap cap;
        cap.kind = Cap::kArrows;
        return cap;
      }
      bool letters = std::all_of(std::begin(tokens), std::end(tokens), [](const std::string &t) { return t.size() == 1; });
      Cap cap;
      for (const std::string &t : tokens)
      {
        if (t.empty())
          continue;
        if (!cap.text.empty() && !letters)
          cap.text += '/';
        cap.text += KeyName(t);
      }
      if (cap.text.empty())
        return std::nullopt;
      return cap;
    }

    // Page pixels per page texel of the replacement.
    int PageScale()
    {
      double render_scale = std::atof(rex::cvar::GetFlagByName("ao2_native_gpu_resolution_scale").c_str());
      return std::clamp(int(std::ceil(render_scale)), 2, 4);
    }

    // GDI text at `height` pixels: its width, and (with `alpha`) its coverage
    // in a w x h box, centered.
    struct TextRenderer
    {
      HDC dc = nullptr;
      TextRenderer() { dc = CreateCompatibleDC(nullptr); }
      ~TextRenderer()
      {
        if (dc)
          DeleteDC(dc);
      }
      // The letters of the controller's own pictures (LB, RT, the sticks):
      // heavy and black, Arial Black.
      static int FontHeight(const std::string &text, int face_height)
      {
        size_t n = text.size();
        return int(face_height * (n <= 1 ? 0.62 : n <= 3 ? 0.48 : 0.40));
      }
      HFONT MakeFont(int height)
      {
        return CreateFontW(-height, 0, 0, 0, FW_BLACK, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Arial Black");
      }
      static std::wstring Wide(const std::string &s) { return std::wstring(s.begin(), s.end()); }
      int Width(const std::string &text, int height)
      {
        HFONT font = MakeFont(height);
        HGDIOBJ old = SelectObject(dc, font);
        std::wstring w = Wide(text);
        SIZE size = {};
        GetTextExtentPoint32W(dc, w.c_str(), int(w.size()), &size);
        SelectObject(dc, old);
        DeleteObject(font);
        return size.cx;
      }
      std::vector<uint8_t> Coverage(const std::string &text, int height, int w, int h)
      {
        std::vector<uint8_t> out(size_t(w) * h);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void *bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp)
          return out;
        HGDIOBJ old_bmp = SelectObject(dc, bmp);
        std::memset(bits, 0, size_t(w) * h * 4);
        HFONT font = MakeFont(height);
        HGDIOBJ old_font = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        std::wstring s = Wide(text);
        RECT r = {0, 0, w, h};
        DrawTextW(dc, s.c_str(), int(s.size()), &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        const uint8_t *px = static_cast<const uint8_t *>(bits);
        for (size_t i = 0; i < out.size(); ++i)
          out[i] = std::max({px[i * 4], px[i * 4 + 1], px[i * 4 + 2]});
        SelectObject(dc, old_font);
        DeleteObject(font);
        SelectObject(dc, old_bmp);
        DeleteObject(bmp);
        return out;
      }
    };

    // Measured at 4 pixels per texel, so the layout does not depend on the
    // page's scale.
    constexpr int kMeasureScale = 4;

    // Width in pixels of the picture of `cap` `face` pixels high.
    int CapPixels(const Cap &cap, TextRenderer &text, int face)
    {
      switch (cap.kind)
      {
      case Cap::kKey:
      {
        int tw = text.Width(cap.text, TextRenderer::FontHeight(cap.text, face));
        return std::max(face, tw + int(face * 0.55));
      }
      case Cap::kArrow:
      case Cap::kArrows: return face;
      case Cap::kMouse: return int(face * 0.72);
      }
      return face;
    }

    int CapWidth(const Cap &cap, TextRenderer &text)
    {
      int pixels = CapPixels(cap, text, kCapHeight * kMeasureScale);
      return (pixels + kMeasureScale - 1) / kMeasureScale + 2; // a texel of margin on each side
    }

    struct Layout
    {
      std::vector<Cap> caps;
      std::array<int, 256> cap_of_char; // index into caps, -1 = stays
      bool valid = false;
    };

    // Which picture each button character gets and where (once: the binds
    // are read at startup).
    const Layout &GetLayout()
    {
      static Layout layout;
      static std::once_flag once;
      std::call_once(once, [] {
        layout.cap_of_char.fill(-1);
        std::map<Cap, int> index;
        // Every character of the free room gets a picture: its old one is gone.
        auto assign = [&](const char *chars, std::optional<Cap> cap, const char *unbound) {
          if (!cap)
          {
            cap = Cap{};
            cap->text = unbound;
          }
          auto [it, added] = index.emplace(*cap, int(layout.caps.size()));
          if (added)
            layout.caps.push_back(*cap);
          for (const char *c = chars; *c; ++c)
            layout.cap_of_char[uint8_t(*c)] = it->second;
        };
        for (const ButtonChars &b : kButtons)
          assign(b.chars, CapForToken(FirstToken(b.cvar)), b.unbound);
        assign(kLeftStick, CapForDirections("keybind_lstick_up", "keybind_lstick_left", "keybind_lstick_down",
                                            "keybind_lstick_right"),
               "LS");
        std::optional<Cap> right;
        if (rex::cvar::GetFlagByName("mnk_mouse") == "true")
        {
          right = Cap{};
          right->kind = Cap::kMouse;
        }
        else
        {
          right = CapForDirections("keybind_rstick_up", "keybind_rstick_left", "keybind_rstick_down",
                                   "keybind_rstick_right");
        }
        assign(kRightStick, right, "RS");
        assign(kDpad, CapForDirections("keybind_dpad_up", "keybind_dpad_left", "keybind_dpad_down",
                                       "keybind_dpad_right"),
               "D-pad");

        // First fit, widest first, into the free room.
        TextRenderer text;
        std::vector<int> order(layout.caps.size());
        for (size_t i = 0; i < order.size(); ++i)
        {
          order[i] = int(i);
          layout.caps[i].w = CapWidth(layout.caps[i], text);
        }
        std::sort(order.begin(), order.end(), [&](int a, int b) { return layout.caps[a].w > layout.caps[b].w; });
        std::vector<FreeSpan> free(std::begin(kFree), std::end(kFree));
        for (int i : order)
        {
          Cap &cap = layout.caps[i];
          auto span = std::find_if(free.begin(), free.end(), [&](const FreeSpan &f) { return f.w >= cap.w; });
          if (span == free.end())
          {
            REXLOG_WARN("keyboard_prompts: no room on the button page for a {}-texel key", cap.w);
            return;
          }
          cap.u = span->u;
          cap.v = span->v;
          span->u += cap.w;
          span->w -= cap.w;
        }
        layout.valid = true;
      });
      return layout;
    }

    // ---------------------------------------------------------------------
    // Drawing
    // ---------------------------------------------------------------------

    struct Color
    {
      float r, g, b;
    };

    // Straight-alpha RGBA float canvas, "over" compositing.
    struct Canvas
    {
      int size;
      std::vector<float> px; // r, g, b, a
      explicit Canvas(int s) : size(s), px(size_t(s) * s * 4, 0.0f) {}
      void Blend(int x, int y, Color c, float coverage)
      {
        if (x < 0 || y < 0 || x >= size || y >= size || coverage <= 0.0f)
          return;
        float *p = &px[(size_t(y) * size + x) * 4];
        float a = std::min(coverage, 1.0f), da = p[3];
        float oa = a + da * (1.0f - a);
        if (oa <= 0.0f)
          return;
        p[0] = (c.r * a + p[0] * da * (1.0f - a)) / oa;
        p[1] = (c.g * a + p[1] * da * (1.0f - a)) / oa;
        p[2] = (c.b * a + p[2] * da * (1.0f - a)) / oa;
        p[3] = oa;
      }
      void Clear(int x0, int y0, int x1, int y1)
      {
        for (int y = std::max(y0, 0); y < std::min(y1, size); ++y)
          for (int x = std::max(x0, 0); x < std::min(x1, size); ++x)
            std::fill_n(&px[(size_t(y) * size + x) * 4], 4, 0.0f);
      }
    };

    struct Box
    {
      float x0, y0, x1, y1;
      Box Inset(float l, float t, float r, float b) const { return {x0 + l, y0 + t, x1 - r, y1 - b}; }
      float W() const { return x1 - x0; }
      float H() const { return y1 - y0; }
    };

    // Signed distance from a pixel center to a rounded box.
    float RoundBoxDistance(const Box &b, float radius, float px, float py)
    {
      float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
      radius = std::min({radius, b.W() * 0.5f, b.H() * 0.5f});
      float qx = std::abs(px - cx) - (b.W() * 0.5f - radius), qy = std::abs(py - cy) - (b.H() * 0.5f - radius);
      float outside = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f));
      return outside + std::min(std::max(qx, qy), 0.0f) - radius;
    }

    // Fills a rounded box; `color_at` gives the color at a row (gradients),
    // `clip` limits it to a part of the box.
    template <typename ColorAt>
    void FillRoundBox(Canvas &c, const Box &b, float radius, ColorAt color_at, const Box *clip = nullptr)
    {
      for (int y = int(std::floor(b.y0)); y < int(std::ceil(b.y1)); ++y)
      {
        for (int x = int(std::floor(b.x0)); x < int(std::ceil(b.x1)); ++x)
        {
          float px = x + 0.5f, py = y + 0.5f;
          if (clip && (px < clip->x0 || px > clip->x1 || py < clip->y0 || py > clip->y1))
            continue;
          float coverage = std::clamp(0.5f - RoundBoxDistance(b, radius, px, py), 0.0f, 1.0f);
          c.Blend(x, y, color_at((py - b.y0) / b.H()), coverage);
        }
      }
    }

    void FillRoundBox(Canvas &c, const Box &b, float radius, Color color, const Box *clip = nullptr)
    {
      FillRoundBox(c, b, radius, [&](float) { return color; }, clip);
    }

    // A triangle pointing in `dir` (0 up, 1 right, 2 down, 3 left), centered
    // at (cx, cy), `size` from tip to base; 4x4 samples per pixel.
    void FillArrow(Canvas &c, float cx, float cy, float size, int dir, Color color)
    {
      // Up-pointing, then rotated.
      float h = size, half = size * 0.62f;
      float pts[3][2] = {{0.0f, -h * 0.5f}, {half, h * 0.5f}, {-half, h * 0.5f}};
      for (auto &p : pts)
      {
        float x = p[0], y = p[1];
        for (int i = 0; i < dir; ++i)
        {
          float t = x;
          x = -y;
          y = t;
        }
        p[0] = cx + x;
        p[1] = cy + y;
      }
      auto edge = [](const float *a, const float *b, float x, float y) {
        return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
      };
      float area = edge(pts[0], pts[1], pts[2][0], pts[2][1]);
      int x0 = int(std::floor(std::min({pts[0][0], pts[1][0], pts[2][0]}))),
          x1 = int(std::ceil(std::max({pts[0][0], pts[1][0], pts[2][0]})));
      int y0 = int(std::floor(std::min({pts[0][1], pts[1][1], pts[2][1]}))),
          y1 = int(std::ceil(std::max({pts[0][1], pts[1][1], pts[2][1]})));
      for (int y = y0; y < y1; ++y)
      {
        for (int x = x0; x < x1; ++x)
        {
          int inside = 0;
          for (int sy = 0; sy < 4; ++sy)
          {
            for (int sx = 0; sx < 4; ++sx)
            {
              float px = x + (sx + 0.5f) / 4.0f, py = y + (sy + 0.5f) / 4.0f;
              float e0 = edge(pts[0], pts[1], px, py), e1 = edge(pts[1], pts[2], px, py),
                    e2 = edge(pts[2], pts[0], px, py);
              if (area > 0 ? (e0 >= 0 && e1 >= 0 && e2 >= 0) : (e0 <= 0 && e1 <= 0 && e2 <= 0))
                ++inside;
            }
          }
          c.Blend(x, y, color, inside / 16.0f);
        }
      }
    }

    constexpr Color kInk = {0.08f, 0.08f, 0.08f};
    constexpr Color kSide = {0.55f, 0.55f, 0.56f};
    constexpr Color kFaceTop = {0.98f, 0.98f, 0.98f};
    constexpr Color kFaceBottom = {0.80f, 0.80f, 0.81f};
    constexpr Color kLit = {0.96f, 0.64f, 0.12f};

    Color Lerp(Color a, Color b, float t)
    {
      return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
    }

    // A keycap in `b`; returns its top face (where the label goes).
    Box DrawKeycap(Canvas &c, const Box &b)
    {
      float h = b.H(), outline = std::max(1.5f, h * 0.07f), radius = h * 0.22f;
      FillRoundBox(c, b, radius, kInk);
      Box side = b.Inset(outline, outline, outline, outline);
      FillRoundBox(c, side, radius - outline, kSide);
      Box face = side.Inset(h * 0.06f, h * 0.03f, h * 0.06f, h * 0.15f);
      FillRoundBox(c, face, radius * 0.75f, [](float t) { return Lerp(kFaceTop, kFaceBottom, t); });
      return face;
    }

    void DrawText(Canvas &c, TextRenderer &text, const Box &face, const std::string &label, int face_height)
    {
      int w = int(face.W()), h = int(face.H());
      if (w <= 0 || h <= 0)
        return;
      int font = TextRenderer::FontHeight(label, face_height);
      // Narrower than the label at its size (a long name in a tight cap): shrink.
      int tw = text.Width(label, font);
      int room = w - std::max(2, int(face_height * 0.12f));
      if (tw > room)
        font = std::max(6, font * room / std::max(tw, 1));
      std::vector<uint8_t> coverage = text.Coverage(label, font, w, h);
      int x0 = int(face.x0), y0 = int(face.y0);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          c.Blend(x0 + x, y0 + y, kInk, coverage[size_t(y) * w + x] / 255.0f);
    }

    void DrawMouse(Canvas &c, const Box &cell, int lit)
    {
      float h = cell.H(), w = h * 0.64f, outline = std::max(1.5f, h * 0.07f);
      float cx = (cell.x0 + cell.x1) * 0.5f;
      Box body = {cx - w * 0.5f, cell.y0, cx + w * 0.5f, cell.y1};
      float radius = w * 0.5f;
      FillRoundBox(c, body, radius, kInk);
      Box inner = body.Inset(outline, outline, outline, outline);
      float inner_radius = radius - outline;
      FillRoundBox(c, inner, inner_radius, [](float t) { return Lerp(kFaceTop, kFaceBottom, t); });
      float split = body.y0 + h * 0.44f;
      if (lit == 1 || lit == 2)
      {
        Box half = lit == 1 ? Box{inner.x0, inner.y0, cx, split} : Box{cx, inner.y0, inner.x1, split};
        FillRoundBox(c, inner, inner_radius, kLit, &half);
      }
      // The buttons' seams.
      float seam = std::max(1.0f, h * 0.05f);
      Box across = {inner.x0, split - seam * 0.5f, inner.x1, split + seam * 0.5f};
      FillRoundBox(c, inner, inner_radius, kInk, &across);
      Box down = {cx - seam * 0.5f, inner.y0, cx + seam * 0.5f, split};
      FillRoundBox(c, inner, inner_radius, kInk, &down);
      // The wheel.
      Box wheel = {cx - w * 0.11f, body.y0 + h * 0.12f, cx + w * 0.11f, body.y0 + h * 0.34f};
      FillRoundBox(c, wheel, w * 0.11f, kInk);
      if (lit == 3)
        FillRoundBox(c, wheel.Inset(seam, seam, seam, seam), w * 0.11f - seam, kLit);
    }

    // A picture of `cap` filling `cell`.
    void DrawCapIn(Canvas &c, TextRenderer &text, const Cap &cap, const Box &cell)
    {
      if (cap.kind == Cap::kMouse)
      {
        DrawMouse(c, cell, cap.mouse);
        return;
      }
      Box face = DrawKeycap(c, cell);
      float cx = (face.x0 + face.x1) * 0.5f, cy = (face.y0 + face.y1) * 0.5f;
      switch (cap.kind)
      {
      case Cap::kKey: DrawText(c, text, face, cap.text, int(cell.H())); break;
      case Cap::kArrow: FillArrow(c, cx, cy, face.H() * 0.5f, cap.arrow, kInk); break;
      case Cap::kArrows:
      {
        float s = face.H() * 0.24f, d = face.H() * 0.27f;
        FillArrow(c, cx, cy - d, s, 0, kInk);
        FillArrow(c, cx + d, cy, s, 1, kInk);
        FillArrow(c, cx, cy + d, s, 2, kInk);
        FillArrow(c, cx - d, cy, s, 3, kInk);
        break;
      }
      default: break;
      }
    }

    void DrawCap(Canvas &c, TextRenderer &text, const Cap &cap, int scale)
    {
      // The cell without its texel of margin on each side.
      Box cell = {float((cap.u + 1) * scale), float((cap.v + kCapTop) * scale), float((cap.u + cap.w - 1) * scale),
                  float((cap.v + kCapTop + kCapHeight) * scale)};
      DrawCapIn(c, text, cap, cell);
    }

    // A guest DXT1 / DXT5 texture (tiled, 16-bit big-endian words, the pitch
    // 32 blocks at least) as straight-alpha RGBA, size x size.
    std::vector<float> DecodeDxt(const uint8_t *guest, bool dxt1, int size)
    {
      int blocks = size / 4;
      uint32_t pitch = uint32_t(std::max(blocks, 32));
      std::vector<float> out(size_t(size) * size * 4);
      for (int by = 0; by < blocks; ++by)
      {
        for (int bx = 0; bx < blocks; ++bx)
        {
          const uint8_t *src = guest + ao2::gpu::TiledOffset2D(bx, by, pitch, dxt1 ? 3 : 4);
          uint8_t b[16] = {};
          // DXT1 blocks are the color half of a DXT5 one.
          uint8_t *dst = dxt1 ? b + 8 : b;
          for (int i = 0; i < (dxt1 ? 8 : 16); i += 2)
          {
            dst[i] = src[i + 1];
            dst[i + 1] = src[i];
          }
          uint16_t c0 = uint16_t(b[8] | (b[9] << 8)), c1 = uint16_t(b[10] | (b[11] << 8));
          uint32_t cbits = uint32_t(b[12]) | (uint32_t(b[13]) << 8) | (uint32_t(b[14]) << 16) | (uint32_t(b[15]) << 24);
          auto rgb = [](uint16_t v) {
            return std::array<float, 3>{float((v >> 11) & 31) / 31.0f, float((v >> 5) & 63) / 63.0f,
                                        float(v & 31) / 31.0f};
          };
          std::array<float, 3> p[4] = {rgb(c0), rgb(c1)};
          bool three = dxt1 && c0 <= c1; // three colors and transparent black
          for (int k = 0; k < 3; ++k)
          {
            p[2][k] = three ? (p[0][k] + p[1][k]) / 2.0f : (2 * p[0][k] + p[1][k]) / 3.0f;
            p[3][k] = three ? 0.0f : (p[0][k] + 2 * p[1][k]) / 3.0f;
          }
          int alpha[8] = {255, 255, 255, 255, 255, 255, 255, 255};
          uint64_t abits = 0;
          if (!dxt1)
          {
            int a0 = b[0], a1 = b[1];
            alpha[0] = a0;
            alpha[1] = a1;
            for (int i = 0; i < 6; ++i)
              alpha[2 + i] = a0 > a1 ? ((6 - i) * a0 + (i + 1) * a1) / 7 : 0;
            if (a0 <= a1)
            {
              for (int i = 0; i < 4; ++i)
                alpha[2 + i] = ((4 - i) * a0 + (i + 1) * a1) / 5;
              alpha[6] = 0;
              alpha[7] = 255;
            }
            for (int i = 0; i < 6; ++i)
              abits |= uint64_t(b[2 + i]) << (8 * i);
          }
          for (int i = 0; i < 16; ++i)
          {
            float *d = &out[((size_t(by) * 4 + i / 4) * size + bx * 4 + i % 4) * 4];
            int index = (cbits >> (2 * i)) & 3;
            const auto &col = p[index];
            d[0] = col[0];
            d[1] = col[1];
            d[2] = col[2];
            d[3] = three && index == 3 ? 0.0f : float(alpha[(abits >> (3 * i)) & 7]) / 255.0f;
          }
        }
      }
      return out;
    }

    // Debugging (ao2_keyboard_prompts_dump): a straight-alpha picture over a
    // grey checkerboard, as a 24-bit BMP in keyboard_prompts_dump\.
    void WriteDump(const std::string &name, const float *px, int w, int h)
    {
      std::error_code ec;
      fs::path dir = fs::current_path(ec) / "keyboard_prompts_dump";
      fs::create_directories(dir, ec);
      int row = (w * 3 + 3) & ~3;
      std::vector<uint8_t> file(54 + size_t(row) * h);
      auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&file[at], &v, 4); };
      file[0] = 'B';
      file[1] = 'M';
      put32(2, uint32_t(file.size()));
      put32(10, 54);
      put32(14, 40);
      put32(18, uint32_t(w));
      put32(22, uint32_t(h));
      file[26] = 1;
      file[28] = 24;
      put32(34, uint32_t(size_t(row) * h));
      for (int y = 0; y < h; ++y)
      {
        uint8_t *out = &file[54 + size_t(h - 1 - y) * row];
        for (int x = 0; x < w; ++x)
        {
          const float *p = px + (size_t(y) * w + x) * 4;
          float bg = ((x / 8 + y / 8) & 1) ? 0.30f : 0.40f;
          for (int k = 0; k < 3; ++k)
          {
            float v = std::clamp(p[2 - k], 0.0f, 1.0f) * p[3] + bg * (1.0f - p[3]);
            out[x * 3 + k] = uint8_t(v * 255.0f + 0.5f);
          }
        }
      }
      std::ofstream(dir / (name + ".bmp"), std::ios::binary).write(reinterpret_cast<const char *>(file.data()),
                                                                     std::streamsize(file.size()));
    }

    // The original page (guest data: DXT5) as straight-alpha RGBA, 256x256.
    std::vector<float> DecodeOriginalPage(const uint8_t *guest)
    {
      return DecodeDxt(guest, false, 256);
    }

    // Bilinear upscale of the straight-alpha page (premultiplied while filtering).
    void Upscale(const std::vector<float> &src, Canvas &dst, int scale)
    {
      for (int y = 0; y < dst.size; ++y)
      {
        float sy = (y + 0.5f) / scale - 0.5f;
        int y0 = std::clamp(int(std::floor(sy)), 0, 255), y1 = std::min(y0 + 1, 255);
        float fy = std::clamp(sy - std::floor(sy), 0.0f, 1.0f);
        for (int x = 0; x < dst.size; ++x)
        {
          float sx = (x + 0.5f) / scale - 0.5f;
          int x0 = std::clamp(int(std::floor(sx)), 0, 255), x1 = std::min(x0 + 1, 255);
          float fx = std::clamp(sx - std::floor(sx), 0.0f, 1.0f);
          float acc[4] = {};
          auto add = [&](int xx, int yy, float wgt) {
            const float *s = &src[(size_t(yy) * 256 + xx) * 4];
            acc[0] += s[0] * s[3] * wgt;
            acc[1] += s[1] * s[3] * wgt;
            acc[2] += s[2] * s[3] * wgt;
            acc[3] += s[3] * wgt;
          };
          add(x0, y0, (1 - fx) * (1 - fy));
          add(x1, y0, fx * (1 - fy));
          add(x0, y1, (1 - fx) * fy);
          add(x1, y1, fx * fy);
          float *d = &dst.px[(size_t(y) * dst.size + x) * 4];
          d[3] = acc[3];
          for (int k = 0; k < 3; ++k)
            d[k] = acc[3] > 0 ? acc[k] / acc[3] : 0.0f;
        }
      }
    }

    constexpr uint64_t kButtonPageHash = 0x6423CAE6484B0D52ull; // XXH3 of the disc's page
    constexpr uint32_t kPageBytes = 256 * 256;                   // DXT5: a byte per texel
    // The renderer drew the keys' page (cleared when the controller's buttons
    // come back, set again by the next BuildButtonPage).
    std::atomic<bool> g_page_built{false};

    // Keys or the controller's buttons (the game thread sets it, the renderer
    // reads it); the generation counts the switches.
    std::atomic<bool> g_keys{true};
    std::atomic<uint32_t> g_generation{0};

    // Split-screen: the keyboard player's and the gamepad player's parts of
    // the screen (x0, y0, x1, y1 as fractions of it), set by the game thread.
    std::mutex g_split_mutex;
    bool g_split = false;
    float g_split_keys[4] = {}, g_split_pad[4] = {};
    std::atomic<bool> g_split_active{false};

    bool KeysShown()
    {
      return REXCVAR_GET(ao2_keyboard_prompts) && g_keys.load(std::memory_order_relaxed);
    }

    // What player 1 used last (rexruntime.dll's rex_input_last_input): 0
    // nothing yet, 1 keyboard / mouse, 2 a gamepad; 0 from a runtime without it.
    uint32_t LastInput()
    {
      using Fn = uint32_t (*)();
      static Fn fn = []() -> Fn {
        HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
        return runtime ? reinterpret_cast<Fn>(GetProcAddress(runtime, "rex_input_last_input")) : nullptr;
      }();
      return fn ? fn() : 0;
    }

    // ---------------------------------------------------------------------
    // Button textures (HUD, menus)
    // ---------------------------------------------------------------------

    constexpr uint32_t kDxt1 = 18;  // xenos::TextureFormat::k_DXT1
    constexpr uint32_t kDxt45 = 20; // xenos::TextureFormat::k_DXT4_5

    // A texture that is one button's picture, by the XXH3 of its base level
    // (the disc's data, as cooked in the packages: tiled, 32x32 blocks at
    // least). x0..y1: the button in texels; the key's picture takes its
    // height and, when the key's name needs it, more of the texture's width.
    struct ButtonTexture
    {
      uint64_t hash;
      uint32_t format, size; // square
      int button;            // index into kButtons
      int x0, y0, x1, y1;
      const char *name; // the texture, for the log
    };
    constexpr int kA = 0, kB = 1, kX = 2, kY = 3, kLT = 4, kRT = 5;
    constexpr ButtonTexture kButtonTextures[] = {
        // 00_hud: the context action (A), reload (X), co-op move cancel (B)
        {0x6D10FDE23F982EA0ull, kDxt45, 128, kA, 42, 41, 88, 88, "action_menu_button"},
        // reload: the key a third smaller than the button (two thirds of it), same center
        {0x962ABAD6AAF49533ull, kDxt45, 128, kX, 50, 49, 81, 80, "reload_button"},
        {0x28E5AA2DA9C11D01ull, kDxt1, 32, kB, 3, 3, 29, 29, "coop_call_xb"},
        {0x0D195EECC3842E4Cull, kDxt45, 64, kA, 4, 1, 62, 59, "coop_call_icon_backtoback_btn"},
        {0x6DFEC5007DA3A21Dull, kDxt45, 128, kA, 18, 9, 109, 102, "minigame_button_a_top"},
        {0xA28C4E81A455D50Full, kDxt1, 128, kB, 16, 8, 109, 102, "minigame_button_b_top"},
        {0xA53805E450804494ull, kDxt1, 128, kX, 18, 9, 109, 100, "minigame_button_x_top"},
        {0x90A583C286763913ull, kDxt1, 128, kY, 18, 9, 109, 95, "minigame_button_y_top"},
        {0x81090F9D24AD5791ull, kDxt45, 64, kA, 9, 7, 57, 58, "mp_button_a_small"},
        {0x8D9B1AACEB02D21Aull, kDxt45, 64, kB, 8, 7, 57, 59, "mp_button_b_small"},
        {0x0EC695C3FC413A2Bull, kDxt45, 64, kX, 8, 7, 57, 59, "mp_button_x_small"},
        {0x2190A8393C287234ull, kDxt45, 64, kY, 9, 8, 56, 57, "mp_button_y_small"},
        {0xAEAE4569EA3601D2ull, kDxt45, 128, kB, 21, 15, 108, 103, "pausemenu_button"},
        // 00_shell / shell: the shell's menus and the weapon shop
        {0x558E8E5154441A2Full, kDxt45, 64, kA, 6, 6, 60, 59, "button_x360_A"},
        {0xF4B2E72D343701D2ull, kDxt45, 64, kB, 5, 6, 59, 60, "button_x360_B"},
        {0xA05B10A4C074FA7Dull, kDxt45, 64, kX, 5, 6, 59, 60, "button_x360_X"},
        {0x641EA25BE4D1C7A1ull, kDxt45, 64, kY, 5, 4, 59, 58, "button_x360_Y"},
        {0x58E436D716E126CBull, kDxt45, 64, kLT, 14, 8, 54, 57, "button_x360_lt"},
        {0xEA8FB5C03802DC94ull, kDxt45, 64, kRT, 13, 8, 53, 58, "button_x360_rt"},
        {0x895AE95E13AAEE97ull, kDxt45, 64, kA, 8, 9, 56, 57, "x360_A_button"},
        {0xC3ADC09E7DB5DBB3ull, kDxt45, 64, kB, 7, 8, 55, 56, "x360_B_button"},
        {0x6371F7F82A4A9D38ull, kDxt45, 64, kX, 6, 8, 55, 57, "x360_X_button"},
        {0x6F3359E0A0E4CD93ull, kDxt45, 64, kY, 8, 9, 56, 57, "x360_Y_button"},
        // Startup / 00_shell: outlined A and B
        {0x9404083965C68E92ull, kDxt45, 32, kA, 1, 1, 31, 31, "b_A"},
        {0xD23D9CE47A9E6466ull, kDxt45, 32, kB, 1, 1, 31, 31, "b_B"},
    };

    // Bytes of the base level of a format/size the table has (0 if none):
    // 32x32 blocks at least.
    uint32_t BaseBytes(uint32_t format, uint32_t size)
    {
      uint32_t blocks = std::max(size / 4, 32u);
      return blocks * blocks * (format == kDxt1 ? 8 : 16);
    }

    // The picture of bind `button`'s first key (its name when unbound).
    Cap CapOfButton(int button)
    {
      const ButtonChars &b = kButtons[button];
      std::optional<Cap> cap = CapForToken(FirstToken(b.cvar));
      if (!cap)
      {
        cap = Cap{};
        cap->text = b.unbound;
      }
      return *cap;
    }

    // ---------------------------------------------------------------------
    // The font's characters (game thread)
    // ---------------------------------------------------------------------

    uint32_t Load32(const uint8_t *base, uint32_t addr)
    {
      uint32_t v;
      std::memcpy(&v, base + addr, 4);
      return std::byteswap(v);
    }

    void Store32(uint8_t *base, uint32_t addr, uint32_t value)
    {
      value = std::byteswap(value);
      std::memcpy(base + addr, &value, 4);
    }

    constexpr uint32_t kCharStride = 20; // FFontCharacter: StartU, StartV, USize, VSize, TextureIndex (padded)

    // The disc's entries of the characters PatchFont moves, to put them back.
    std::array<std::array<uint8_t, kCharStride>, 256> g_disc_chars{};
    std::atomic<bool> g_disc_chars_saved{false}; // the renderer reads them for the split-screen page
    uint32_t g_font = 0; // Xbox360_18pt, as last found

    // Moves the button characters of Xbox360_18pt to the keys (`to_keys`) or
    // back to the disc's rectangles; true once they are where asked.
    bool PatchFont(uint8_t *base, bool to_keys)
    {
      uint32_t font_class = ao2::ue3::FindClass(base, "Font");
      int32_t characters = ao2::ue3::PropertyOffset(base, "Font", "Characters");
      if (!font_class || characters < 0)
        return false;
      auto is_font = [&](uint32_t object) {
        return object && Load32(base, object + 52) == font_class &&
               ao2::ue3::ObjectName(base, object) == "Xbox360_18pt";
      };
      if (!is_font(g_font))
      {
        g_font = 0;
        for (uint32_t i = 0, n = ao2::ue3::ObjectCount(base); i < n && !g_font; ++i)
        {
          uint32_t object = ao2::ue3::ObjectAt(base, i);
          if (is_font(object))
            g_font = object;
        }
        if (!g_font)
          return false;
      }
      const Layout &layout = GetLayout();
      uint32_t data = Load32(base, g_font + uint32_t(characters));
      uint32_t count = Load32(base, g_font + uint32_t(characters) + 4);
      if (!data || count < 128)
        return false;
      // The layout of the array checked on 'A': the disc's rectangle or ours.
      uint32_t a = data + 'A' * kCharStride;
      const Cap *a_cap = layout.cap_of_char['A'] >= 0 ? &layout.caps[layout.cap_of_char['A']] : nullptr;
      bool original = Load32(base, a) == 201 && Load32(base, a + 4) == 55 && Load32(base, a + 8) == 24 &&
                      Load32(base, a + 12) == 26;
      bool ours = a_cap && Load32(base, a) == uint32_t(a_cap->u) && Load32(base, a + 4) == uint32_t(a_cap->v);
      if (!original && !ours)
      {
        REXLOG_WARN("keyboard_prompts: Xbox360_18pt's characters are not where expected");
        return true; // do not keep looking
      }
      if (original && !g_disc_chars_saved.load(std::memory_order_relaxed))
      {
        for (int c = 0; c < 256 && c < int(count); ++c)
          std::memcpy(g_disc_chars[c].data(), base + data + uint32_t(c) * kCharStride, kCharStride);
        g_disc_chars_saved.store(true, std::memory_order_release);
        // The split-screen page needs them: the renderer looks again.
        g_generation.fetch_add(1, std::memory_order_release);
      }
      if (ours == to_keys || (!to_keys && !g_disc_chars_saved))
        return true;
      for (int c = 0; c < 256 && c < int(count); ++c)
      {
        int index = layout.cap_of_char[c];
        if (index < 0)
          continue;
        uint32_t entry = data + uint32_t(c) * kCharStride;
        if (!to_keys)
        {
          std::memcpy(base + entry, g_disc_chars[c].data(), kCharStride);
          continue;
        }
        const Cap &cap = layout.caps[index];
        Store32(base, entry, uint32_t(cap.u));
        Store32(base, entry + 4, uint32_t(cap.v));
        Store32(base, entry + 8, uint32_t(cap.w));
        Store32(base, entry + 12, uint32_t(kCellHeight));
        base[entry + 16] = 0; // the first page
      }
      REXLOG_INFO("keyboard_prompts: button characters of Xbox360_18pt moved to {}",
                  to_keys ? "the keys" : "the controller's buttons");
      return true;
    }

    // ---------------------------------------------------------------------
    // Split-screen: whose part of the screen is whose (game thread)
    // ---------------------------------------------------------------------

    float LoadF32(const uint8_t *base, uint32_t addr)
    {
      return std::bit_cast<float>(Load32(base, addr));
    }

    // The local players' parts of the screen (Engine GamePlayers: LocalPlayer
    // Origin / Size, fractions of the viewport; 2 players: 0,0 1x0.5 and
    // 0,0.5 1x0.5) and which one has the keyboard; false when not split
    // between a keyboard player and a gamepad player.
    bool ReadSplit(const uint8_t *base, float keys[4], float pad[4])
    {
      constexpr uint32_t kEngine = 0x831EFCBC; // GEngine
      static const int32_t players = ao2::ue3::PropertyOffset(base, "Engine", "GamePlayers");
      static const int32_t controller_id = ao2::ue3::PropertyOffset(base, "LocalPlayer", "ControllerId");
      static const int32_t origin = ao2::ue3::PropertyOffset(base, "LocalPlayer", "Origin");
      static const int32_t size = ao2::ue3::PropertyOffset(base, "LocalPlayer", "Size");
      if (players < 0 || controller_id < 0 || origin < 0 || size < 0)
      {
        static bool logged = false;
        if (!logged)
        {
          logged = true;
          REXLOG_WARN("keyboard_prompts: LocalPlayer's layout not found ({} {} {} {})", players, controller_id,
                      origin, size);
        }
        return false;
      }
      uint32_t keyboard = ao2::mouse::KeyboardUser();
      uint32_t engine = Load32(base, kEngine);
      if (keyboard >= 4 || !engine)
        return false;
      uint32_t data = Load32(base, engine + uint32_t(players)), count = Load32(base, engine + uint32_t(players) + 4);
      if (!data || count < 2 || count > 4)
        return false;
      bool have_keys = false, have_pad = false;
      for (uint32_t i = 0; i < count; ++i)
      {
        uint32_t player = Load32(base, data + i * 4);
        if (!player)
          continue;
        float x = LoadF32(base, player + uint32_t(origin)), y = LoadF32(base, player + uint32_t(origin) + 4);
        float w = LoadF32(base, player + uint32_t(size)), h = LoadF32(base, player + uint32_t(size) + 4);
        if (!(w > 0.0f && h > 0.0f && w <= 1.0f && h <= 1.0f) || (w >= 1.0f && h >= 1.0f))
          return false; // not split (yet)
        float *rect = Load32(base, player + uint32_t(controller_id)) == keyboard ? keys : pad;
        bool &have = rect == keys ? have_keys : have_pad;
        if (have)
          continue;
        have = true;
        rect[0] = x;
        rect[1] = y;
        rect[2] = x + w;
        rect[3] = y + h;
      }
      return have_keys && have_pad;
    }

    // The keys' page for split-screen: the disc's page with every button
    // character's rectangle showing that character's key.
    bool BuildSplitPage(const uint8_t *guest_data, std::vector<uint8_t> &rgba, uint32_t &size)
    {
      const Layout &layout = GetLayout();
      if (!layout.valid || !g_disc_chars_saved.load(std::memory_order_acquire))
        return false;
      static std::mutex built_mutex;
      static std::vector<uint8_t> built;
      static uint32_t built_size = 0;
      std::lock_guard<std::mutex> lock(built_mutex);
      if (built.empty())
      {
        int scale = PageScale();
        Canvas canvas(256 * scale);
        Upscale(DecodeOriginalPage(guest_data), canvas, scale);
        TextRenderer text;
        auto field = [](const std::array<uint8_t, kCharStride> &e, int at) {
          uint32_t v;
          std::memcpy(&v, e.data() + at, 4);
          return int32_t(std::byteswap(v));
        };
        for (int c = 0; c < 256; ++c)
        {
          int index = layout.cap_of_char[c];
          if (index < 0)
            continue;
          const auto &e = g_disc_chars[c];
          int u = field(e, 0), v = field(e, 4), w = field(e, 8), h = field(e, 12);
          if (w <= 2 || h <= 2 || u < 0 || v < 0 || u + w > 256 || v + h > 256)
            continue;
          canvas.Clear(u * scale, v * scale, (u + w) * scale, (v + h) * scale);
          const Cap &cap = layout.caps[index];
          // The key's picture as in the keys' page (22 of the cell's 26), as
          // wide as it needs within the character.
          float y0 = float((v + kCapTop * h / kCellHeight) * scale);
          float y1 = float((v + (kCapTop + kCapHeight) * h / kCellHeight) * scale);
          float room = float((w - 1) * scale);
          float cw = std::min(float(CapPixels(cap, text, int(y1 - y0))), room);
          float x0 = (u + w * 0.5f) * scale - cw * 0.5f;
          DrawCapIn(canvas, text, cap, Box{x0, y0, x0 + cw, y1});
        }
        built_size = uint32_t(canvas.size);
        built.resize(size_t(built_size) * built_size * 4);
        for (size_t i = 0; i < built.size(); ++i)
          built[i] = uint8_t(std::clamp(canvas.px[i], 0.0f, 1.0f) * 255.0f + 0.5f);
        if (REXCVAR_GET(ao2_keyboard_prompts_dump))
          WriteDump("page_split_keys", canvas.px.data(), canvas.size, canvas.size);
      }
      rgba = built;
      size = built_size;
      return true;
    }

    // ---------------------------------------------------------------------
    // Localization: the controller's words
    // ---------------------------------------------------------------------

    constexpr std::string_view kOpen = "<Fonts:00_fonts.Xbox360_18pt>";
    constexpr std::string_view kClose = "<Fonts:/>";

    // The buttons of a string ("A", "]^", ...), sorted.
    std::vector<std::string> Buttons(std::string_view value)
    {
      std::vector<std::string> out;
      for (size_t at = value.find(kOpen); at != std::string_view::npos; at = value.find(kOpen, at + 1))
      {
        size_t begin = at + kOpen.size();
        size_t end = value.find(kClose, begin);
        if (end == std::string_view::npos)
          break;
        out.emplace_back(value.substr(begin, end - begin));
      }
      std::sort(out.begin(), out.end());
      return out;
    }

    // English strings without a PS3 twin: the controller words before a
    // button go, the verb becomes "press".
    std::string CleanEnglish(std::string value)
    {
      static const std::string kButton = "(?=<Fonts:00_fonts\\.Xbox360_18pt>)";
      static const std::pair<std::regex, const char *> kRules[] = {
          {std::regex("(the )?(left|right) (bumper|trigger|stick)( button)? " + kButton, std::regex::icase), ""},
          {std::regex("D-pad (up|down|left|right) " + kButton, std::regex::icase), ""},
          {std::regex("the BACK button " + kButton, std::regex::icase), ""},
          {std::regex("(<Fonts:00_fonts\\.Xbox360_18pt>[^<]*<Fonts:/>) Button\\)"), "$1)"},
          {std::regex("\\bPull and hold " + kButton), "Press and hold "},
          {std::regex("\\b(Pull|Click) " + kButton), "Press "},
      };
      for (const auto &[re, with] : kRules)
        value = std::regex_replace(value, re, with);
      return value;
    }

    // One .int file's text (Latin-1).
    std::string RewriteIni(const std::string &text, bool english, int &rewritten)
    {
      struct Line
      {
        std::string section, key, head, value; // head: "Key=" (or the whole line)
      };
      std::vector<Line> lines;
      std::map<std::pair<std::string, std::string>, std::vector<size_t>> index;
      std::string section;
      size_t pos = 0;
      while (pos <= text.size())
      {
        size_t nl = text.find('\n', pos);
        std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        Line line;
        if (!raw.empty() && raw.front() == '[')
          section = raw;
        size_t eq = raw.find('=');
        if (raw.empty() || raw.front() == '[' || raw.front() == ';' || eq == std::string::npos)
        {
          line.head = raw;
        }
        else
        {
          line.key = Trim(std::string_view(raw).substr(0, eq));
          line.head = raw.substr(0, eq + 1);
          line.value = raw.substr(eq + 1);
          index[{section, line.key}].push_back(lines.size());
        }
        line.section = section;
        lines.push_back(std::move(line));
      }

      // The twins' text before any of it changes (Key_PS3 can come first).
      std::vector<std::string> original;
      original.reserve(lines.size());
      for (const Line &line : lines)
        original.push_back(line.value);

      for (Line &line : lines)
      {
        if (line.value.find(kOpen) == std::string::npos || line.key.ends_with("_PS3"))
          continue;
        std::string before = line.value;
        bool from_twin = false;
        const auto &own = index[{line.section, line.key}];
        // "Key" / "Key_PS3" (English), "Key_Xbox360" / "Key_PS3" (the others).
        std::string twin_key = line.key;
        if (twin_key.ends_with("_Xbox360"))
          twin_key.resize(twin_key.size() - std::strlen("_Xbox360"));
        auto twin = index.find({line.section, twin_key + "_PS3"});
        if (own.size() == 1 && twin != index.end() && twin->second.size() == 1)
        {
          // Keep the line's own "\r".
          std::string twin_value = original[twin->second[0]];
          bool cr = !line.value.empty() && line.value.back() == '\r';
          if (!twin_value.empty() && twin_value.back() == '\r')
            twin_value.pop_back();
          // Not a twin that names the PS3's button ("Drücke SELECT <Back>").
          if (Buttons(twin_value) == Buttons(line.value) && twin_value.find("SELECT") == std::string::npos)
          {
            line.value = twin_value + (cr ? "\r" : "");
            from_twin = true;
          }
        }
        if (!from_twin && english)
          line.value = CleanEnglish(line.value);
        if (line.value != before)
          ++rewritten;
      }

      std::string out;
      out.reserve(text.size());
      for (size_t i = 0; i < lines.size(); ++i)
      {
        if (i)
          out += '\n';
        out += lines[i].head;
        out += lines[i].value;
      }
      return out;
    }

    uint32_t ReadBe32(const std::string &d, size_t at)
    {
      uint32_t v;
      std::memcpy(&v, d.data() + at, 4);
      return std::byteswap(v);
    }

    // Coalesced.<lang>: big-endian count of strings, then (file name, file
    // text) pairs, each an int32 length (with the terminating zero; negative =
    // UTF-16 characters) and the characters. Returns `data` unchanged when it
    // does not parse.
    std::string RewriteCoalesced(const std::string &data, bool english, int &rewritten)
    {
      if (data.size() < 4)
        return data;
      uint32_t count = ReadBe32(data, 0);
      std::string out = data.substr(0, 4);
      size_t at = 4;
      for (uint32_t i = 0; i < count; ++i)
      {
        if (at + 4 > data.size())
          return data;
        int32_t n = static_cast<int32_t>(ReadBe32(data, at));
        size_t bytes = n < 0 ? size_t(-int64_t(n)) * 2 : size_t(n);
        if (at + 4 + bytes > data.size())
          return data;
        std::string_view str(data.data() + at + 4, bytes);
        bool is_text = (i & 1) && n > 1 && str.back() == '\0' && str.find(kOpen) != std::string_view::npos;
        out.append(data, at, 4);
        if (is_text)
        {
          int here = 0;
          std::string text = RewriteIni(std::string(str.substr(0, bytes - 1)), english, here);
          if (text.size() <= bytes - 1)
          {
            text.resize(bytes - 1, '\n');
            out += text;
            out += '\0';
            rewritten += here;
          }
          else
          {
            REXLOG_WARN("keyboard_prompts: a rewritten file got longer, kept as is");
            out += str;
          }
        }
        else
        {
          out += str;
        }
        at += 4 + bytes;
      }
      return at == data.size() ? out : data;
    }
  } // namespace

  bool Enabled() { return REXCVAR_GET(ao2_keyboard_prompts); }

  std::string RewriteLocalization(const std::string &data, bool english, int &rewritten)
  {
    return RewriteCoalesced(data, english, rewritten);
  }

  void Tick(uint8_t *base)
  {
    if (!REXCVAR_GET(ao2_keyboard_prompts))
      return;
    // Split-screen between a keyboard and a gamepad player: looked at a few
    // times a second (players join and leave in the menus).
    static uint32_t split_frame = 0;
    if (split_frame++ % 15 == 0)
    {
      float keys_rect[4] = {}, pad_rect[4] = {};
      bool split = ReadSplit(base, keys_rect, pad_rect);
      std::lock_guard<std::mutex> lock(g_split_mutex);
      bool moved = split && (std::memcmp(keys_rect, g_split_keys, sizeof(keys_rect)) ||
                             std::memcmp(pad_rect, g_split_pad, sizeof(pad_rect)));
      if (split != g_split || moved)
      {
        g_split = split;
        std::memcpy(g_split_keys, keys_rect, sizeof(keys_rect));
        std::memcpy(g_split_pad, pad_rect, sizeof(pad_rect));
        g_split_active.store(split, std::memory_order_release);
        g_generation.fetch_add(1, std::memory_order_release);
        if (split)
          REXLOG_INFO("keyboard_prompts: split-screen, keys at {:.2f},{:.2f}-{:.2f},{:.2f}, gamepad buttons at "
                      "{:.2f},{:.2f}-{:.2f},{:.2f}",
                      keys_rect[0], keys_rect[1], keys_rect[2], keys_rect[3], pad_rect[0], pad_rect[1], pad_rect[2],
                      pad_rect[3]);
        else
          REXLOG_INFO("keyboard_prompts: not split between keys and a gamepad");
      }
    }
    bool split = g_split_active.load(std::memory_order_relaxed);
    bool keys = split || LastInput() != 2;
    if (keys != g_keys.load(std::memory_order_relaxed))
    {
      g_keys.store(keys, std::memory_order_relaxed);
      if (!keys)
        g_page_built.store(false, std::memory_order_release);
      g_generation.fetch_add(1, std::memory_order_release);
      REXLOG_INFO("keyboard_prompts: showing {}", keys ? "the keys" : "the controller's buttons");
    }
    // The font's characters: on the keys once the renderer drew their page,
    // on the disc's rectangles otherwise. Moved right after a switch, looked
    // for every 30 frames while the font is missing, then checked every 600
    // (a reloaded font gets moved again).
    static uint32_t frame = 0;
    static bool done = false, done_keys = false;
    // Split-screen: the disc's rectangles, which both halves' pages share.
    bool to_keys = keys && !split && g_page_built.load(std::memory_order_acquire);
    if (done && done_keys != to_keys)
      frame = 0;
    else if (++frame % (done ? 600 : 30) != 0)
      return;
    done = PatchFont(base, to_keys);
    done_keys = to_keys;
  }

  uint32_t Generation() { return g_generation.load(std::memory_order_acquire); }

  bool SplitActive() { return g_split_active.load(std::memory_order_acquire); }

  bool SplitRegions(float keys[4], float pad[4])
  {
    std::lock_guard<std::mutex> lock(g_split_mutex);
    if (!g_split)
      return false;
    std::memcpy(keys, g_split_keys, sizeof(g_split_keys));
    std::memcpy(pad, g_split_pad, sizeof(g_split_pad));
    return true;
  }

  bool MayBeButton(uint32_t format, uint32_t width, uint32_t height)
  {
    if (!REXCVAR_GET(ao2_keyboard_prompts) || width != height)
      return false;
    if (format == kDxt45 && width == 256)
      return true;
    for (const ButtonTexture &t : kButtonTextures)
      if (t.format == format && t.size == width)
        return true;
    return false;
  }

  bool IsButtonPage(uint32_t format, uint32_t width, uint32_t height, const uint8_t *guest_data, uint32_t size)
  {
    return KeysShown() && format == kDxt45 && width == 256 && height == 256 && size >= kPageBytes && guest_data &&
           XXH3_64bits(guest_data, kPageBytes) == kButtonPageHash;
  }

  bool BuildButtonPage(const uint8_t *guest_data, std::vector<uint8_t> &rgba, uint32_t &size)
  {
    const Layout &layout = GetLayout();
    if (!KeysShown() || !layout.valid)
      return false;
    if (SplitActive())
      return BuildSplitPage(guest_data, rgba, size);
    // Drawn once: the page is the disc's and the binds do not change.
    static std::mutex built_mutex;
    static std::vector<uint8_t> built;
    static uint32_t built_size = 0;
    std::lock_guard<std::mutex> lock(built_mutex);
    if (!built.empty())
    {
      rgba = built;
      size = built_size;
      g_page_built.store(true, std::memory_order_release);
      return true;
    }
    int scale = PageScale();
    Canvas canvas(256 * scale);
    std::vector<float> original = DecodeOriginalPage(guest_data);
    if (REXCVAR_GET(ao2_keyboard_prompts_dump))
    {
      WriteDump("page_original", original.data(), 256, 256);
    }
    Upscale(original, canvas, scale);
    for (const FreeSpan &f : kFree)
      canvas.Clear(f.u * scale, f.v * scale, (f.u + f.w) * scale, (f.v + kCellHeight) * scale);
    TextRenderer text;
    for (const Cap &cap : layout.caps)
      DrawCap(canvas, text, cap, scale);

    size = uint32_t(canvas.size);
    rgba.resize(size_t(size) * size * 4);
    for (size_t i = 0; i < size_t(size) * size * 4; ++i)
      rgba[i] = uint8_t(std::clamp(canvas.px[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    if (REXCVAR_GET(ao2_keyboard_prompts_dump))
      WriteDump("page_keys", canvas.px.data(), canvas.size, canvas.size);
    built = rgba;
    built_size = size;
    g_page_built.store(true, std::memory_order_release);
    return true;
  }

  uint32_t ButtonTextureBytes(uint32_t format, uint32_t width, uint32_t height)
  {
    if (!KeysShown() || width != height)
      return 0;
    for (const ButtonTexture &t : kButtonTextures)
      if (t.format == format && t.size == width)
        return BaseBytes(format, width);
    return 0;
  }

  int ButtonTextureOf(uint32_t format, uint32_t width, uint32_t height, const uint8_t *guest_data, uint32_t bytes)
  {
    if (!guest_data || bytes == 0 || bytes != ButtonTextureBytes(format, width, height))
      return -1;
    uint64_t hash = XXH3_64bits(guest_data, bytes);
    for (size_t i = 0; i < std::size(kButtonTextures); ++i)
    {
      const ButtonTexture &t = kButtonTextures[i];
      if (t.hash == hash && t.format == format && t.size == width)
      {
        static std::array<std::atomic<bool>, std::size(kButtonTextures)> dumped{};
        if (REXCVAR_GET(ao2_keyboard_prompts_dump) && !dumped[i].exchange(true))
        {
          std::vector<float> px = DecodeDxt(guest_data, format == kDxt1, int(width));
          WriteDump(std::string(t.name) + "_original", px.data(), int(width), int(width));
        }
        return int(i);
      }
    }
    return -1;
  }

  bool BuildButtonTexture(int index, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
  {
    if (!KeysShown() || index < 0 || index >= int(std::size(kButtonTextures)))
      return false;
    const ButtonTexture &t = kButtonTextures[index];
    // Small pictures drawn large: at least 256 pixels across.
    int scale = std::max(PageScale(), int(256 / t.size));
    int size = int(t.size) * scale;
    Canvas canvas(size);
    TextRenderer text;
    Cap cap = CapOfButton(t.button);

    // The button's height; as wide as the key needs, within the texture
    // (a texel of margin), centered on the button.
    float y0 = float(t.y0 * scale), y1 = float(t.y1 * scale);
    float h = y1 - y0, cx = (t.x0 + t.x1) * 0.5f * scale;
    float margin = float(scale);
    float w = std::min(float(CapPixels(cap, text, int(h))), size - 2.0f * margin);
    float x0 = std::clamp(cx - w * 0.5f, margin, size - margin - w);
    DrawCapIn(canvas, text, cap, Box{x0, y0, x0 + w, y1});

    width = height = uint32_t(size);
    rgba.resize(size_t(size) * size * 4);
    for (size_t i = 0; i < rgba.size(); ++i)
      rgba[i] = uint8_t(std::clamp(canvas.px[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    if (REXCVAR_GET(ao2_keyboard_prompts_dump))
      WriteDump(std::string(t.name) + "_keys", canvas.px.data(), size, size);
    REXLOG_INFO("keyboard_prompts: {} drawn as a key", t.name);
    return true;
  }
} // namespace ao2::prompts
