// aspect_ratio.cpp - The game renders at the window's aspect ratio instead of
// a 16:9 picture letterboxed into it (ao2_aspect_ratio).
//
// The engine already handles back buffers other than 1280x720: its D3D init
// (sub_82AA30F8) sets GScreenHeight = 720 and GScreenWidth = 720 x the display
// aspect clamped to 900..1280 (a 4:3 display gets 960x720), and the render
// targets, EDRAM budgets, resolves and viewports all follow GScreenWidth /
// GScreenHeight. The display aspect comes from XGetVideoMode, which is always
// 16:9 here, and only 900..1280 x 720 is possible that way.
//
// Instead, right before the init creates the device (Direct3D_CreateDevice,
// sub_82B0FA20, its only caller) the size it was given is replaced: the
// present parameters' BackBufferWidth / BackBufferHeight and GScreenWidth /
// GScreenHeight, which the rest of the init and the engine read afterwards.
// Narrower than 16:9 keeps 720 lines (16:10 = 1152x720, 4:3 = 960x720),
// wider keeps 1280 columns (21:9 = 1280x544): the picture never has more
// pixels than 1280x720, so it still fits the EDRAM in the same number of
// predicated tiles.
//
// Two places assume 1280x720 regardless:
// - The predicated tiling rectangles: a constant table of three D3DRECTs per
//   tiling mode (4xAA 3 tiles, 2xAA 2 tiles, ...), 1280 wide, 256 / 384 / 720
//   lines per tile. The game takes the tiling surface size from the last
//   rectangle of the mode. They are rebuilt for the new size, the same number
//   of tiles with equal heights (32-line multiples).
// - FViewport stores 16/9 as its aspect ratio when the display is widescreen
//   (sub_82A9CD60, +1944); its getter (sub_82A9F540, a vtable slot) returns
//   GScreenWidth / GScreenHeight instead.
//
// Field of view: UE3's FPerspectiveMatrix (sub_823E01D0, every perspective
// projection: the player's view, scene captures, spot light shadows) keeps
// the horizontal field of view, so wider than 16:9 would see less above and
// below than the console did. So (Hor+, always) projections at the
// screen's aspect wider than 16:9 get the half angle atan(tan(half) x aspect /
// (16/9)): the 16:9 vertical view, more at the sides. Narrower aspects keep the
// horizontal angle (16:10 sees more above and below). The view frustum, culling
// and the aim (screen centre) follow the matrix.
//
// The size is decided once, when the game creates its device: "auto" follows
// the window's client area at that time (the monitor in fullscreen). The
// native renderer presents whatever size the game resolves, letterboxed only
// when the window's aspect differs.
#include "aspect_ratio.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_STRING(ao2_aspect_ratio, "auto", "GPU",
                      "Render aspect ratio: auto (the window's), or a ratio such as 16:9, 16:10, 21:9, 4:3, 1.6");

void D3DTraceEnter(uint32_t addr, PPCContext &ctx);
REXCVAR_DECLARE(bool, ao2_d3d_trace);

namespace
{
  constexpr uint32_t kCreateDevice = 0x82B0FA20;
  constexpr uint32_t kScreenWidth = 0x8318775C;  // GScreenWidth
  constexpr uint32_t kScreenHeight = 0x83187760; // GScreenHeight
  constexpr uint32_t kTileCounts = 0x83187714;   // tiles per tiling mode
  constexpr uint32_t kTileRects = 0x82103118;    // D3DRECT[3] per tiling mode
  constexpr uint32_t kTilingModes = 5;
  constexpr uint32_t kRectsPerMode = 3;
  constexpr uint32_t kOriginalWidth = 1280, kOriginalHeight = 720;

  std::atomic<HWND> g_window{nullptr};
  std::atomic<uint32_t> g_width{kOriginalWidth}, g_height{kOriginalHeight};

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

  // Window client area, else its monitor, else the primary monitor.
  double WindowAspect()
  {
    HWND hwnd = g_window.load();
    RECT r = {};
    if (hwnd && GetClientRect(hwnd, &r) && r.right > 0 && r.bottom > 0 && !IsIconic(hwnd))
    {
      return double(r.right) / double(r.bottom);
    }
    MONITORINFO mi = {sizeof(mi)};
    HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY)
                            : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (GetMonitorInfoW(monitor, &mi) && mi.rcMonitor.right > mi.rcMonitor.left &&
        mi.rcMonitor.bottom > mi.rcMonitor.top)
    {
      return double(mi.rcMonitor.right - mi.rcMonitor.left) / double(mi.rcMonitor.bottom - mi.rcMonitor.top);
    }
    return 16.0 / 9.0;
  }

  // "auto", "16:10", "16/10", "16x10" or "1.6"; 0 if not understood.
  double RequestedAspect()
  {
    std::string v = REXCVAR_GET(ao2_aspect_ratio);
    v.erase(std::remove_if(v.begin(), v.end(), [](unsigned char c) { return std::isspace(c); }), v.end());
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (v.empty() || v == "auto")
    {
      return WindowAspect();
    }
    const char *s = v.c_str();
    char *end = nullptr;
    double a = std::strtod(s, &end);
    if (end == s || !(a > 0.0))
    {
      return 0.0;
    }
    if (*end == 0)
    {
      return a;
    }
    if (*end != ':' && *end != '/' && *end != 'x')
    {
      return 0.0;
    }
    const char *second = end + 1;
    double b = std::strtod(second, &end);
    return end != second && *end == 0 && b > 0.0 ? a / b : 0.0;
  }

  uint32_t Round16(double x)
  {
    return uint32_t(std::lround(x / 16.0)) * 16u;
  }

  // Rebuilds the tiling rectangles for width x height; false (nothing written)
  // if the table is not the 1280x720 one this was written for.
  bool PatchTileRects(uint8_t *base, uint32_t width, uint32_t height)
  {
    uint32_t counts[kTilingModes];
    for (uint32_t m = 0; m < kTilingModes; ++m)
    {
      counts[m] = LoadU32(base, kTileCounts + m * 4);
      uint32_t last = kTileRects + (m * kRectsPerMode + counts[m] - 1) * 16;
      if (counts[m] < 1 || counts[m] > kRectsPerMode || LoadU32(base, last + 8) != kOriginalWidth ||
          LoadU32(base, last + 12) != kOriginalHeight)
      {
        return false;
      }
    }
    uint8_t *table = base + kTileRects;
    constexpr size_t kTableSize = kTilingModes * kRectsPerMode * 16;
    rex::memory::PageAccess old_access{};
    rex::memory::Protect(table, kTableSize, rex::memory::PageAccess::kReadWrite, &old_access);
    for (uint32_t m = 0; m < kTilingModes; ++m)
    {
      uint32_t rows = ((height + counts[m] - 1) / counts[m] + 31) & ~31u;
      for (uint32_t k = 0; k < counts[m]; ++k)
      {
        uint32_t rect = kTileRects + (m * kRectsPerMode + k) * 16;
        StoreU32(base, rect + 0, 0);
        StoreU32(base, rect + 4, k * rows);
        StoreU32(base, rect + 8, width);
        StoreU32(base, rect + 12, std::min(height, (k + 1) * rows));
      }
    }
    rex::memory::Protect(table, kTableSize, old_access, nullptr);
    return true;
  }

  // Called with the device about to be created: sets the back buffer size.
  void ApplyAspect(PPCContext &ctx, uint8_t *base)
  {
    double aspect = RequestedAspect();
    if (aspect <= 0.0)
    {
      REXLOG_WARN("ao2_aspect_ratio '{}' not understood: 16:9", REXCVAR_GET(ao2_aspect_ratio));
      return;
    }
    aspect = std::clamp(aspect, 1.0, 4.0);
    uint32_t width = kOriginalWidth, height = kOriginalHeight;
    if (aspect < 16.0 / 9.0)
    {
      width = Round16(kOriginalHeight * aspect);
    }
    else
    {
      height = Round16(kOriginalWidth / aspect);
    }
    if (width == kOriginalWidth && height == kOriginalHeight)
    {
      return;
    }
    // D3DPRESENT_PARAMETERS: BackBufferWidth, BackBufferHeight.
    uint32_t params = ctx.r7.u32;
    if (LoadU32(base, params) != kOriginalWidth || LoadU32(base, params + 4) != kOriginalHeight ||
        LoadU32(base, kScreenWidth) != kOriginalWidth || LoadU32(base, kScreenHeight) != kOriginalHeight)
    {
      REXLOG_WARN("aspect ratio: the device is not created at 1280x720, left as it is");
      return;
    }
    if (!PatchTileRects(base, width, height))
    {
      REXLOG_WARN("aspect ratio: unexpected tiling rectangles, rendering stays 1280x720");
      return;
    }
    StoreU32(base, params, width);
    StoreU32(base, params + 4, height);
    StoreU32(base, kScreenWidth, width);
    StoreU32(base, kScreenHeight, height);
    g_width = width;
    g_height = height;
    REXLOG_INFO("aspect ratio {:.4f}: rendering at {}x{}", aspect, width, height);
  }
} // namespace

namespace ao2::aspect
{
  void SetWindow(void *native_window_handle)
  {
    g_window = static_cast<HWND>(native_window_handle);
  }
  void *Window()
  {
    return g_window.load();
  }
  uint32_t GuestWidth()
  {
    return g_width;
  }
  uint32_t GuestHeight()
  {
    return g_height;
  }
} // namespace ao2::aspect

// Direct3D_CreateDevice(adapter, type, window, flags, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **).
extern "C" REX_FUNC(__imp__sub_82B0FA20);
extern "C" REX_FUNC(sub_82B0FA20)
{
  ApplyAspect(ctx, base);
  if (REXCVAR_GET(ao2_d3d_trace)) [[unlikely]]
  {
    D3DTraceEnter(kCreateDevice, ctx);
  }
  __imp__sub_82B0FA20(ctx, base);
}

// FViewport::GetDesiredAspectRatio: the back buffer's, not a fixed 16:9.
extern "C" REX_FUNC(__imp__sub_82A9F540);
extern "C" REX_FUNC(sub_82A9F540)
{
  uint32_t width = g_width, height = g_height;
  if (width == kOriginalWidth && height == kOriginalHeight)
  {
    __imp__sub_82A9F540(ctx, base);
    return;
  }
  ctx.f1.f64 = double(float(width) / float(height));
}

// Full-screen movies (sub_8257BE38, the Bink player's draw): at a viewport
// that is not 16:9 the movie keeps the viewport's width and gets the height
// W x 720 / 1280, centred: y = (H - that) / 2, unsigned. Narrower than 16:9
// that letterboxes; wider the movie is taller than the screen, the
// subtraction wraps and y is ~2^31: a black screen. Its draws are moved to
// the movie fitted by height instead, centred left to right:
// - the movie through the Bink blitter sub_826D62B8 (x f1, y f2, width r5,
//   height r6; return 0x8257C004),
// - or through a canvas tile sub_82709A40 (x f1, y f2, width f3, height f4;
//   return 0x8257BFD0),
// - and the loading icon in its corner, blitted (return 0x8257C26C): size
//   r5 = r6 = movie height x 100 / 720 at x 0.9 W, y 0.92 x movie height.
// - the loading screen's texts, wrapped into a rectangle by sub_82935718
//   (canvas r4, text r5, int rect x0 y0 x1 y1 r6, font r7): the tip in
//   0.2 W, 0.2 M .. 0.8 W, 0.8 M (0.9 M when the movie is blitted; return
//   0x8257C120) and the second line at the point 0.2 W, 0.17 M (return
//   0x8257C1CC), M the movie height. Its y had the wrapped offset added and
//   was saturated to INT_MAX: the text was never on screen.
namespace
{
  constexpr uint32_t kMovieBlit = 0x8257C004, kMovieTile = 0x8257BFD0, kMovieIcon = 0x8257C26C;
  constexpr uint32_t kMovieTip = 0x8257C120, kMovieTipLine = 0x8257C1CC;
  // The last movie frame went through the canvas tile (else the blitter).
  std::atomic<bool> g_movie_tiled{false};

  struct MovieFit
  {
    double scale; // the game's movie size -> the fitted one
    double x;     // the fitted movie's left edge
    uint32_t height; // the game's movie height (W x 720 / 1280)
  };

  // False unless the screen is wider than 16:9 (the movie's height wraps).
  bool FitMovie(MovieFit &fit)
  {
    uint32_t width = g_width, height = g_height;
    uint32_t movie = width * kOriginalHeight / kOriginalWidth;
    if (movie <= height)
    {
      return false;
    }
    fit.height = movie;
    fit.scale = double(height) / double(movie);
    fit.x = (double(width) - double(width) * fit.scale) * 0.5;
    return true;
  }
} // namespace

// Bink texture blit (device r3, textures r4, x f1, y f2, x scale f3, y scale f4, width r5, height r6, alpha f5).
extern "C" REX_FUNC(__imp__sub_826D62B8);
extern "C" REX_FUNC(sub_826D62B8)
{
  MovieFit fit;
  uint32_t lr = uint32_t(ctx.lr);
  if (lr == kMovieBlit)
  {
    g_movie_tiled = false;
  }
  if ((lr == kMovieBlit || lr == kMovieIcon) && FitMovie(fit))
  {
    if (lr == kMovieBlit && ctx.r5.u32 == g_width && ctx.r6.u32 == fit.height)
    {
      ctx.f1.f64 = fit.x;
      ctx.f2.f64 = 0.0;
      ctx.r5.u64 = uint32_t(std::lround(double(g_width) * fit.scale));
      ctx.r6.u64 = g_height;
    }
    else if (lr == kMovieIcon && ctx.r5.u32 == ctx.r6.u32 && ctx.r6.u32 == fit.height * 100 / kOriginalHeight)
    {
      // y had the wrapped offset added in single precision: placed again.
      uint32_t size = ctx.r6.u32;
      ctx.f1.f64 = double(float(fit.x + ctx.f1.f64 * fit.scale));
      ctx.f2.f64 = double(float((double(fit.height) * 0.92 - double(size >> 1)) * fit.scale));
      ctx.r5.u64 = ctx.r6.u64 = uint32_t(std::lround(double(size) * fit.scale));
    }
  }
  __imp__sub_826D62B8(ctx, base);
}

// Canvas tile (canvas r3, x f1, y f2, width f3, height f4, u v f5-f8, ...): the movie when it is drawn that way.
extern "C" REX_FUNC(__imp__sub_82709A40);
extern "C" REX_FUNC(sub_82709A40)
{
  MovieFit fit;
  if (uint32_t(ctx.lr) == kMovieTile)
  {
    g_movie_tiled = true;
  }
  if (uint32_t(ctx.lr) == kMovieTile && FitMovie(fit) && ctx.f3.f64 == double(g_width) &&
      ctx.f4.f64 == double(fit.height))
  {
    ctx.f1.f64 = double(float(fit.x));
    ctx.f2.f64 = 0.0;
    ctx.f3.f64 = double(float(double(g_width) * fit.scale));
    ctx.f4.f64 = double(g_height);
  }
  __imp__sub_82709A40(ctx, base);
}

// Wrapped text in a rectangle (canvas r4, text r5, int rect x0 y0 x1 y1 r6, font r7, r8): the loading screen's.
extern "C" REX_FUNC(__imp__sub_82935718);
extern "C" REX_FUNC(sub_82935718)
{
  MovieFit fit;
  uint32_t lr = uint32_t(ctx.lr);
  if ((lr == kMovieTip || lr == kMovieTipLine) && FitMovie(fit))
  {
    // Fractions of the movie: x from the game's own x, y as the game made it.
    double h = double(g_height);
    double y0 = lr == kMovieTip ? 0.2 : 0.17;
    double y1 = lr == kMovieTip ? (g_movie_tiled ? 0.8 : 0.9) : 0.17;
    uint32_t rect = ctx.r6.u32;
    auto x = [&](uint32_t at) {
      int32_t game = int32_t(LoadU32(base, rect + at));
      StoreU32(base, rect + at, uint32_t(int32_t(fit.x + double(game) * fit.scale)));
    };
    x(0);
    x(8);
    StoreU32(base, rect + 4, uint32_t(int32_t(h * y0)));
    StoreU32(base, rect + 12, uint32_t(int32_t(h * y1)));
  }
  __imp__sub_82935718(ctx, base);
}

// The UI's viewport (GameUISceneClient: RenderViewport->GetSizeX / GetSizeY
// into r5, the size every UIScene lays out in, UIScene.CurrentViewportSize,
// and the one UpdateCanvasToScreen sub_82963558 builds the UI's canvas
// transform from). Its scenes were made for 720 lines: 4:3 safe region
// panels as high as the viewport, and in them pictures, frames, the weapon
// shop's labels sized and placed in pixels. At 1280x544 those pixels are
// 1.32 times too big next to the rest (the waiting screen's background
// moved down and right, the shop's labels out of place). Wider than 16:9
// the UI gets a viewport 720 high of the screen's aspect (1694x720 for
// 1280x544) instead: laid out as the console did, its canvas transform
// maps it onto the frame, so it is drawn scaled down evenly, text included.
extern "C" REX_FUNC(__imp__sub_82962EF0);
extern "C" REX_FUNC(sub_82962EF0)
{
  uint32_t out = ctx.r5.u32;
  __imp__sub_82962EF0(ctx, base);
  uint32_t width = g_width, height = g_height;
  if (ctx.r3.u32 == 0 || height >= kOriginalHeight)
  {
    return;
  }
  auto load = [&](uint32_t at) { return std::bit_cast<float>(LoadU32(base, out + at)); };
  if (load(0) != float(width) || load(4) != float(height))
  {
    return;
  }
  float ui_width = float(std::lround(double(width) * kOriginalHeight / height));
  StoreU32(base, out, std::bit_cast<uint32_t>(ui_width));
  StoreU32(base, out + 4, std::bit_cast<uint32_t>(float(kOriginalHeight)));
}

// In that viewport a widget whose left and right faces are fractions of it
// (UIScreenObject.Position, UIScreenValue_Bounds: Value[4] +60, ScaleType[4]
// +76, EVALPOS_PercentageViewport = 4) was made for a 16:9 one: its width
// grows with the viewport, its height does not (the weapon dealers'
// portraits, ContactsScene's DealerPicture, 1.32 times too wide at 21:9).
// Those faces are put into the viewport's 16:9 middle instead
// (v -> 0.5 + (v - 0.5) x 16:9 / aspect), where the Shell's slabs and globe
// are (Hor+): the dealer's panel, labels and pictures keep their places on
// them. Widgets across the whole width stay as they are. Each widget once:
// its faces as put are kept, other values there (another widget at that
// address, a script's SetPosition) are put in again.
namespace
{
  constexpr uint32_t kUiChildren = 96, kUiPosition = 60, kUiScaleType = 76;
  constexpr uint32_t kUiRenderTarget = 360, kUiSceneFlags = 412, kUiViewportSize = 424;
  constexpr uint32_t kUiUpdatePositions = 0xC0000000; // bUpdateDockingStack, bUpdateScenePositions
  constexpr uint8_t kPercentageViewport = 4;

  // Widget -> its left and right faces as last put (float bits). The pad poll's thread only.
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> g_fitted;

  bool FitWidgets(uint8_t *base, uint32_t object, float k, int depth)
  {
    bool changed = false;
    uint32_t data = LoadU32(base, object + kUiChildren);
    uint32_t count = LoadU32(base, object + kUiChildren + 4);
    for (uint32_t i = 0; data && i < count && i < 256 && depth < 8; ++i)
    {
      uint32_t child = LoadU32(base, data + i * 4);
      if (!child || (child & 3))
      {
        continue;
      }
      if (base[child + kUiScaleType] == kPercentageViewport && base[child + kUiScaleType + 2] == kPercentageViewport)
      {
        std::pair<uint32_t, uint32_t> faces{LoadU32(base, child + kUiPosition), LoadU32(base, child + kUiPosition + 8)};
        auto it = g_fitted.find(child);
        if (it == g_fitted.end() || it->second != faces)
        {
          float left = std::bit_cast<float>(faces.first), right = std::bit_cast<float>(faces.second);
          if (left > 0.01f || right < 0.99f)
          {
            faces.first = std::bit_cast<uint32_t>(0.5f + (left - 0.5f) * k);
            faces.second = std::bit_cast<uint32_t>(0.5f + (right - 0.5f) * k);
            StoreU32(base, child + kUiPosition, faces.first);
            StoreU32(base, child + kUiPosition + 8, faces.second);
            changed = true;
          }
          g_fitted[child] = faces;
        }
      }
      changed = FitWidgets(base, child, k, depth + 1) || changed;
    }
    return changed;
  }
} // namespace

void ao2::aspect::FitUiScene(uint8_t *base, uint32_t scene)
{
  if (!scene || LoadU32(base, scene + kUiRenderTarget))
  {
    return; // drawn into a texture: laid out in its size
  }
  float vw = std::bit_cast<float>(LoadU32(base, scene + kUiViewportSize));
  float vh = std::bit_cast<float>(LoadU32(base, scene + kUiViewportSize + 4));
  if (!(vh > 0.0f) || vw / vh < 16.0f / 9.0f + 0.01f)
  {
    return;
  }
  if (FitWidgets(base, scene, (16.0f / 9.0f) / (vw / vh), 0))
  {
    StoreU32(base, scene + kUiSceneFlags, LoadU32(base, scene + kUiSceneFlags) | kUiUpdatePositions);
  }
}

// Render target textures are limited to the screen when they are set up
// (UTextureRenderTarget2D: SizeX +188 <= GScreenWidth, SizeY +192 <=
// GScreenHeight, sub_82649900 and after loading sub_8264FDC8). The console's
// limit was 720 lines; at 1280x544 a target asked for taller is made 544
// high. That stays (scene captures are drawn within the screen-sized scene
// buffers, which do not grow), but what is drawn into it was laid out for
// the height asked for, and the texture is put on its mesh whole:
// - the UI scenes drawn into one (RenderUI's ortho transform for the
//   target, sub_826EF848 returning to 0x8298235C, from its size) were cut at
//   544 lines: the weapon shop's list of weapons (450x600) lost the last one
//   and the rest was stretched down on the paper;
// - scene captures (FPerspectiveMatrix with the target's size) saw less
//   above and below: the shop's gun and its price (RenderToTexture_weapons_cust,
//   1024x720) too big and too high on the shop's screen, the price cut.
// Both are given the height that was asked for, so the picture is the
// console's, squeezed into the lines there are.
namespace
{
  constexpr uint32_t kRtSizeY = 192;
  constexpr uint32_t kRenderUiOrtho = 0x8298235C;

  // Targets made lower than asked: width, the height they got, the height asked for.
  struct CutTarget
  {
    uint32_t width, height, wanted;
  };
  std::mutex g_cut_mutex;
  std::vector<CutTarget> g_cut;
  std::atomic<bool> g_any_cut{false};

  void NoteCutTarget(const uint8_t *base, uint32_t rt, uint32_t wanted)
  {
    uint32_t height = g_height;
    if (height >= kOriginalHeight || LoadU32(base, rt + kRtSizeY) != height || wanted <= height)
    {
      return;
    }
    uint32_t width = LoadU32(base, rt + kRtSizeY - 4);
    if (width == g_width)
    {
      return; // the screen's own size: not told apart from the screen's views
    }
    wanted = std::min(wanted, 2048u);
    std::lock_guard lock(g_cut_mutex);
    for (const CutTarget &c : g_cut)
    {
      if (c.width == width && c.height == height)
      {
        return;
      }
    }
    g_cut.push_back({width, height, wanted});
    g_any_cut = true;
  }

  // The height asked for by a target cut to width x height, 0 if none was.
  uint32_t WantedHeight(uint32_t width, uint32_t height)
  {
    if (!g_any_cut || height != g_height)
    {
      return 0;
    }
    std::lock_guard lock(g_cut_mutex);
    for (const CutTarget &c : g_cut)
    {
      if (c.width == width && c.height == height)
      {
        return c.wanted;
      }
    }
    return 0;
  }
} // namespace

uint32_t ao2::aspect::LayoutHeight(uint32_t width, uint32_t height)
{
  uint32_t wanted = WantedHeight(width, height);
  return wanted ? wanted : height;
}

// UTextureRenderTarget2D: SizeX, SizeY aligned, clamped to 1..2048 and to the screen.
extern "C" REX_FUNC(__imp__sub_82649900);
extern "C" REX_FUNC(sub_82649900)
{
  uint32_t rt = ctx.r3.u32;
  uint32_t wanted = LoadU32(base, rt + kRtSizeY);
  __imp__sub_82649900(ctx, base);
  NoteCutTarget(base, rt, wanted);
}

// The same limit after loading.
extern "C" REX_FUNC(__imp__sub_8264FDC8);
extern "C" REX_FUNC(sub_8264FDC8)
{
  uint32_t rt = ctx.r3.u32;
  uint32_t wanted = LoadU32(base, rt + kRtSizeY);
  __imp__sub_8264FDC8(ctx, base);
  NoteCutTarget(base, rt, wanted);
}

// 2D ortho transform (matrix r3, width r4, height r5, ...): RenderUI's for a UI scene's target.
extern "C" REX_FUNC(__imp__sub_826EF848);
extern "C" REX_FUNC(sub_826EF848)
{
  if (uint32_t(ctx.lr) == kRenderUiOrtho)
  {
    if (uint32_t wanted = WantedHeight(ctx.r4.u32, ctx.r5.u32))
    {
      ctx.r5.u64 = wanted;
    }
  }
  __imp__sub_826EF848(ctx, base);
}

// FPerspectiveMatrix(this, HalfFOV f1, Width f2, Height f3, MinZ f4, MaxZ f5):
// scene captures' (UpdateTransform, from their target's size).
extern "C" REX_FUNC(__imp__sub_828FF430);
extern "C" REX_FUNC(sub_828FF430)
{
  if (ctx.f2.f64 > 0.0 && ctx.f3.f64 > 0.0)
  {
    if (uint32_t wanted = WantedHeight(uint32_t(std::lround(ctx.f2.f64)), uint32_t(std::lround(ctx.f3.f64))))
    {
      ctx.f3.f64 = double(wanted);
    }
  }
  __imp__sub_828FF430(ctx, base);
}

// FPerspectiveMatrix(this, HalfFOV f1, Width f2, Height f3, MinZ f4): Hor+ for the screen's aspect.
// Not for the canvases' base transforms (sub_826EAC10, returning to
// 0x826EAC64: the UI's and render targets'), which map canvas pixels onto
// the target one to one whatever the field of view: a wider one shrank the UI.
extern "C" REX_FUNC(__imp__sub_823E01D0);
extern "C" REX_FUNC(sub_823E01D0)
{
  constexpr double k16by9 = 16.0 / 9.0;
  constexpr uint32_t kCanvasTransform = 0x826EAC64;
  uint32_t width = g_width, height = g_height;
  double screen = double(width) / double(height);
  if (screen > k16by9 + 0.01 && ctx.f3.f64 > 0.0 && uint32_t(ctx.lr) != kCanvasTransform)
  {
    double aspect = ctx.f2.f64 / ctx.f3.f64;
    if (std::abs(aspect / screen - 1.0) < 0.01)
    {
      ctx.f1.f64 = std::atan(std::tan(ctx.f1.f64) * aspect / k16by9);
    }
  }
  __imp__sub_823E01D0(ctx, base);
}
