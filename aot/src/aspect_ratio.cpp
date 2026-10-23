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
#include <string>

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
namespace
{
  constexpr uint32_t kMovieBlit = 0x8257C004, kMovieTile = 0x8257BFD0, kMovieIcon = 0x8257C26C;

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

// FPerspectiveMatrix(this, HalfFOV f1, Width f2, Height f3, MinZ f4): Hor+ for the screen's aspect.
extern "C" REX_FUNC(__imp__sub_823E01D0);
extern "C" REX_FUNC(sub_823E01D0)
{
  constexpr double k16by9 = 16.0 / 9.0;
  uint32_t width = g_width, height = g_height;
  double screen = double(width) / double(height);
  if (screen > k16by9 + 0.01 && ctx.f3.f64 > 0.0)
  {
    double aspect = ctx.f2.f64 / ctx.f3.f64;
    if (std::abs(aspect / screen - 1.0) < 0.01)
    {
      ctx.f1.f64 = std::atan(std::tan(ctx.f1.f64) * aspect / k16by9);
    }
  }
  __imp__sub_823E01D0(ctx, base);
}
