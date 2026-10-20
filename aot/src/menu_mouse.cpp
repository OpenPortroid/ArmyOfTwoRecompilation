// menu_mouse.cpp - The mouse in the game's menus, and an Exit item in the main menu.
//
// The UI is UE3's (GameUISceneClient, UIScene, UIObject) and only ever saw a
// pad: the mouse there was the right stick or nothing. While a menu takes
// input the SDK driver lets the cursor go (SetMouseLookActive(false): shown,
// not captured; rex_input_set_mouse_look) and here:
//  - moving the cursor over a button, option or slider walks the focus to it
//    with the D-pad, as the pad would (some menus' scripts follow the
//    navigation, not the focus: the weapon upgrades' preview). Not every
//    item on the way: the focus is put on the item next to it and one step
//    goes in (on the Options screen and its pages it is set at once); where a step
//    does not move it, it is set (UIScreenObject::SetFocus, vtable +328, as
//    execSetFocus 0x82727C18 calls it with the player index from sub_82973AF0);
//  - the left button presses A on it (on an option button or a slider: left
//    of its middle D-pad left, else D-pad right). The right button keeps its
//    binding (LT: the weapon shop's stats);
//  - Enter and Esc are A and B, accept and back (Esc is otherwise Start).
// The presses go into the pad state of the player with the keyboard
// (XInputGetState hook, input_script.cpp), so the game takes them as its own
// input. Mouse buttons do not reach the triggers they are bound to there.
//
// A menu is the top scene of the scene client that takes input
// (UIScene.SceneInputMode +420 not INPUTMODE_None) and has a visible focusable
// widget. Widgets are hit by UIObject.RenderBounds (+496: left, top, right,
// bottom in the scene's viewport, UIScene.CurrentViewportSize +424). The main
// menu and its submenus (MainMenuScene, ExtrasScene...) are drawn into a
// texture (UIScene.RenderTarget +360, main_menu_t_rtt, 608 x 400) put on the
// stone slabs of the Shell level (pieces at different depths and angles, the
// camera moving between menus), so their bounds are in that texture. The
// renderer tells which texel of it is under the cursor (RendererMenuProbe:
// the slabs are drawn again writing the texture's coordinates); the nearest
// button within kSlabReach texels of it is pointed at, so its whole piece of
// paper is.
//
// Exit: the main menu gets a button under Extras, a copy of it
// (UIScreenObject::CreateWidget sub_829D6BC8 with Extras as the archetype,
// then InsertChild, vtable +276) with its own name, tag and ID. The texture
// ends right under Extras, so the button is invisible (Opacity 0) and only
// takes the focus; a second copy in the help text scene (drawn straight to
// the screen) shows it on the piece of paper below, focused with it
// (SetFocus / KillFocus, vtable +328 / +336). Its own events (the copy's
// Kismet opens Extras) never run: A on it is taken out of the pad state and
// closes the window instead.
//
// Online co-op: the pause menu is this machine's UI and its focus is not part
// of the simulation (the checksum is the shared rand() and physics), and the
// presses go through the pad input like the keys' (Enter / Esc already did),
// so the mouse works there too.
#include "menu_mouse.h"

#include "aspect_ratio.h"
#include "coop.h"
#include "debug_menu.h"
#include "gpu/native/renderer.h"
#include "graphics_menu.h"
#include "log_files.h"
#include "ue3_reflect.h"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_BOOL(ao2_menu_mouse, true, "Input",
                    "The mouse in the menus: the cursor is shown, pointing focuses, the left button selects, the "
                    "right one goes back");
REXCVAR_DEFINE_BOOL(ao2_menu_exit, true, "General", "An Exit item at the bottom of the main menu");
REXCVAR_DEFINE_STRING(ao2_menu_mouse_test_file, "", "Debug",
                      "Testing: a file whose \"x y\" (picture pixels) is the cursor in the menus instead of the "
                      "mouse; what is pointed at goes to menu_mouse.log");

namespace
{
  constexpr uint32_t kClass = 52;
  constexpr uint32_t kPosition = 60;     // UIScreenObject.Position (UIScreenValue_Bounds: Value[4], ScaleType[4])
  constexpr uint32_t kHiddenWord = 92;   // UIScreenObject.bHidden
  constexpr uint32_t kHiddenMask = 0x80000000;
  constexpr uint32_t kChildren = 96;     // UIScreenObject.Children (TArray)
  constexpr uint32_t kStateStack = 136;  // UIScreenObject.StateStack (TArray of UIState)
  constexpr uint32_t kDockTargets = 444; // UIObject.DockTargets: OwnerWidget, TargetWidget[4], ...
  constexpr uint32_t kForcedNavigation = 404 + 16; // UIObject.NavigationTargets.ForcedNavigationTarget[4]
  constexpr uint32_t kFaceTop = 1;
  constexpr uint32_t kFaceBottom = 3;
  constexpr uint32_t kOwnerScene = 368;    // UIObject.OwnerScene
  // UIObject.NavigationTargets: the widgets D-pad left, up, right, down go to.
  constexpr uint32_t kNavigationTargets = 404;
  constexpr uint32_t kRenderBounds = 496;  // UIObject.RenderBounds[4]
  constexpr uint32_t kRenderTarget = 360;  // UIScene.RenderTarget
  constexpr uint32_t kSceneFlags = 412;    // UIScene bitfield
  constexpr uint32_t kUpdateDocking = 0x80000000;    // bUpdateDockingStack
  constexpr uint32_t kUpdatePositions = 0x40000000;  // bUpdateScenePositions
  constexpr uint32_t kUpdateNavigation = 0x20000000; // bUpdateNavigationLinks
  constexpr uint32_t kSceneInputMode = 420;          // UIScene.SceneInputMode (byte)
  constexpr uint32_t kViewportSize = 424;            // UIScene.CurrentViewportSize (float x, y)
  constexpr uint32_t kActiveScenes = 0xC0;           // GameUISceneClient.ActiveScenes
  constexpr uint32_t kSetFocus = 328;                // UIScreenObject vtable: SetFocus(sender, player index)
  constexpr uint32_t kInsertChild = 276;             // UIScreenObject vtable: InsertChild(child, index, rename)
  constexpr uint32_t kCreateWidget = 0x829D6BC8;     // UIScreenObject::CreateWidget(owner, class, archetype, name)
  constexpr uint32_t kBestPlayerIndex = 0x82973AF0;  // UIScreenObject::GetBestPlayerIndex(this)
  constexpr uint32_t kObjects = 0x831E4E3C;          // GObjObjects

  constexpr uint32_t kOpacity = 176;    // UIScreenObject.Opacity
  constexpr uint32_t kRotation = 512;   // UIObject.Rotation: Rotator, TransformMatrix +16, RotationAmount +80
  constexpr uint32_t kKillFocus = 336;  // UIScreenObject vtable: KillFocus(sender, player index)

  // Where the Exit label goes, in a 1280 x 720 picture: on the piece of
  // paper under Extras, which the main menu's texture does not reach. The
  // Shell camera keeps its horizontal field of view: at other aspect ratios
  // the picture is that one scaled to the width, centred.
  constexpr float kExitX0 = 103.0f, kExitY0 = 507.0f, kExitX1 = 253.0f, kExitY1 = 539.0f;
  // Its tilt (degrees, clockwise), as the slab text around it.
  constexpr float kExitTilt = 15.0f;

  // A point on the slabs this close (texels) to a button's bounds is on its
  // piece of paper: the papers reach further along the text than across it.
  constexpr float kSlabReach = 24.0f;
  constexpr float kSlabReachAlong = 8.0f; // times further sideways

  constexpr uint16_t kDpadLeft = 0x0004, kDpadRight = 0x0008;
  constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002;
  constexpr uint16_t kButtonStart = 0x0010;
  constexpr uint16_t kButtonA = 0x1000, kButtonB = 0x2000;

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("menu_mouse.log");
    return f;
  }

  template <typename... Args>
  void Logf(const char *format, Args... args)
  {
    if (FILE *f = Log())
    {
      if constexpr (sizeof...(args) == 0)
      {
        std::fputs(format, f);
      }
      else
      {
        std::fprintf(f, format, args...);
      }
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
  float LoadF32(const uint8_t *base, uint32_t addr) { return std::bit_cast<float>(LoadU32(base, addr)); }
  void StoreF32(uint8_t *base, uint32_t addr, float v) { StoreU32(base, addr, std::bit_cast<uint32_t>(v)); }

  uint32_t CallGuest(PPCContext &ctx, uint8_t *base, uint32_t fn, uint64_t r3, uint64_t r4 = 0, uint64_t r5 = 0,
                     uint64_t r6 = 0, uint64_t r7 = 0)
  {
    PPCContext saved = ctx;
    ctx.r1.u64 = (ctx.r1.u32 - 2048) & ~15u;
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

  uint32_t Virtual(const uint8_t *base, uint32_t object, uint32_t offset)
  {
    return LoadU32(base, LoadU32(base, object) + offset);
  }

  rex::ui::Window *g_window = nullptr;

  // --- The scenes ---------------------------------------------------------

  uint32_t SceneClient(const uint8_t *base)
  {
    static uint32_t client = 0;
    static uint32_t next_search = 0;
    auto is_client = [base](uint32_t o) {
      if (!o || (o & 3))
      {
        return false;
      }
      // AO2's own subclass of GameUISceneClient: by the name's end.
      uint32_t cls = LoadU32(base, o + kClass);
      std::string c = cls ? ao2::ue3::ObjectName(base, cls) : std::string();
      return c.size() > 11 && c.compare(c.size() - 11, 11, "SceneClient") == 0 &&
             ao2::ue3::ObjectName(base, o).rfind("Default__", 0) != 0;
    };
    if (is_client(client))
    {
      return client;
    }
    client = 0;
    uint32_t now = GetTickCount();
    if (int32_t(now - next_search) < 0)
    {
      return 0;
    }
    next_search = now + 2000;
    uint32_t objects = LoadU32(base, kObjects), count = LoadU32(base, kObjects + 4);
    for (uint32_t i = 0; objects && i < count && !client; ++i)
    {
      uint32_t o = LoadU32(base, objects + i * 4);
      if (is_client(o))
      {
        client = o;
      }
    }
    return client;
  }

  bool Hidden(const uint8_t *base, uint32_t o) { return LoadU32(base, o + kHiddenWord) & kHiddenMask; }

  // What a click does to a widget.
  enum class Kind
  {
    kButton, // A
    kValue,  // left / right (option buttons, sliders)
  };

  struct Widget
  {
    uint32_t object;
    Kind kind;
    bool slab;            // drawn on the slabs: its bounds are in the texture
    float vw, vh;         // its scene's viewport
    float x0, y0, x1, y1; // where it is hit
    // Its rotation (Rotation.TransformMatrix: row vectors, from its top left
    // corner), inverted: a point relative to x0, y0 back to the unturned widget.
    float inv[4] = {1.0f, 0.0f, 0.0f, 1.0f}, tx = 0.0f, ty = 0.0f;
  };

  bool Focusable(const uint8_t *base, uint32_t o, Kind &kind)
  {
    static const struct
    {
      const char *name;
      Kind kind;
    } kClasses[] = {{"UIOptionButton", Kind::kValue}, {"UISlider", Kind::kValue}, {"UIButton", Kind::kButton},
                    {"UIList", Kind::kButton},        {"UIEditBox", Kind::kButton}};
    static uint32_t classes[std::size(kClasses)] = {};
    for (size_t i = 0; i < std::size(kClasses); ++i)
    {
      if (!classes[i])
      {
        classes[i] = ao2::ue3::FindClass(base, kClasses[i].name);
      }
      if (classes[i] && ao2::ue3::IsA(base, o, classes[i]))
      {
        kind = kClasses[i].kind;
        return true;
      }
    }
    return false;
  }

  void Collect(const uint8_t *base, uint32_t object, const Widget &scene, std::vector<Widget> &out, int depth = 0)
  {
    uint32_t data = LoadU32(base, object + kChildren);
    uint32_t count = LoadU32(base, object + kChildren + 4);
    for (uint32_t i = 0; data && i < count && i < 256 && depth < 8; ++i)
    {
      uint32_t child = LoadU32(base, data + i * 4);
      if (!child || (child & 3) || Hidden(base, child))
      {
        continue;
      }
      Kind kind;
      // The main menu's Exit is hit at its label (below).
      if (Focusable(base, child, kind) && !(scene.slab && ao2::ue3::ObjectName(base, child) == "Exit"))
      {
        if (kind == Kind::kValue && ao2::gfxmenu::IsOpenButton(base, child))
        {
          kind = Kind::kButton; // "Graphics and controls": a click is A
        }
        Widget w = scene;
        w.object = child;
        w.kind = kind;
        w.x0 = LoadF32(base, child + kRenderBounds);
        w.y0 = LoadF32(base, child + kRenderBounds + 4);
        w.x1 = LoadF32(base, child + kRenderBounds + 8);
        w.y1 = LoadF32(base, child + kRenderBounds + 12);
        float m[16];
        for (int k = 0; k < 16; ++k)
        {
          m[k] = LoadF32(base, child + kRotation + 16 + k * 4);
        }
        float det = m[0] * m[5] - m[1] * m[4];
        if (std::fabs(det) > 0.5f && std::fabs(det) < 2.0f)
        {
          // x' = x m0 + y m4 + m12, y' = x m1 + y m5 + m13.
          w.inv[0] = m[5] / det;
          w.inv[1] = -m[1] / det;
          w.inv[2] = -m[4] / det;
          w.inv[3] = m[0] / det;
          w.tx = m[12];
          w.ty = m[13];
        }
        if (w.x1 > w.x0 && w.y1 > w.y0)
        {
          if (kind == Kind::kValue)
          {
            // The arrows of an option button are drawn outside its bounds.
            w.x0 -= 40.0f;
            w.x1 += 40.0f;
          }
          out.push_back(w);
        }
      }
      Collect(base, child, scene, out, depth + 1);
    }
  }

  bool IsMainMenu(const uint8_t *base, uint32_t scene)
  {
    return ao2::ue3::ObjectName(base, scene).rfind("MainMenuScene", 0) == 0;
  }

  // The scenes on the scene client's stack, bottom to top.
  std::vector<uint32_t> Scenes(const uint8_t *base)
  {
    std::vector<uint32_t> scenes;
    uint32_t client = SceneClient(base);
    uint32_t data = client ? LoadU32(base, client + kActiveScenes) : 0;
    uint32_t count = client ? LoadU32(base, client + kActiveScenes + 4) : 0;
    for (uint32_t i = 0; data && i < count && i < 64; ++i)
    {
      if (uint32_t scene = LoadU32(base, data + i * 4))
      {
        scenes.push_back(scene);
      }
    }
    return scenes;
  }

  // The menu taking input: its scene and focusable widgets; 0 when none.
  uint32_t ActiveMenu(const uint8_t *base, const std::vector<uint32_t> &scenes, std::vector<Widget> &widgets)
  {
    widgets.clear();
    for (size_t i = scenes.size(); i-- > 0;)
    {
      uint32_t scene = scenes[i];
      if (base[scene + kSceneInputMode] == 0)
      {
        continue;
      }
      Widget space{};
      space.vw = LoadF32(base, scene + kViewportSize);
      space.vh = LoadF32(base, scene + kViewportSize + 4);
      if (uint32_t target = LoadU32(base, scene + kRenderTarget))
      {
        // Drawn into a texture on the slabs: its bounds are that texture's texels.
        static int32_t size_x = ao2::ue3::PropertyOffset(base, "TextureRenderTarget2D", "SizeX");
        static int32_t size_y = ao2::ue3::PropertyOffset(base, "TextureRenderTarget2D", "SizeY");
        space.slab = true;
        space.vw = size_x > 0 ? float(LoadU32(base, target + size_x)) : 0.0f;
        space.vh = size_y > 0 ? float(LoadU32(base, target + size_y)) : 0.0f;
      }
      Collect(base, scene, space, widgets);
      return widgets.empty() ? 0 : scene; // the top scene taking input, if it has anything to point at
    }
    return 0;
  }

  // A menu takes the keys (Enter, Esc): a scene taking input is on the
  // stack - in the game it is empty - other than the title screen's, where
  // Esc stays Start (Press START, the attract movie). Confirmations and the
  // objectives have nothing to point at but take B as back.
  bool KeysMenu(const uint8_t *base, const std::vector<uint32_t> &scenes)
  {
    bool any = false;
    for (uint32_t scene : scenes)
    {
      std::string name = ao2::ue3::ObjectName(base, scene);
      if (name.rfind("StartMenuUIScene", 0) == 0 || name.rfind("AttractLauncher", 0) == 0)
      {
        return false;
      }
      any = any || base[scene + kSceneInputMode] != 0;
    }
    return any;
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

  void Focus(PPCContext &ctx, uint8_t *base, uint32_t widget, bool focus = true)
  {
    uint32_t player = CallGuest(ctx, base, kBestPlayerIndex, widget);
    CallGuest(ctx, base, Virtual(base, widget, focus ? kSetFocus : kKillFocus), widget, 0, player);
  }

  // --- The cursor ---------------------------------------------------------

  // The cursor on the picture: u, v from 0 to 1, and the picture's height
  // over its width; false when it is off the picture or the window is not in
  // front.
  struct Cursor
  {
    float u, v, aspect;
  };

  bool GameInFront()
  {
    HWND hwnd = static_cast<HWND>(ao2::aspect::Window());
    return hwnd && GetForegroundWindow() == hwnd;
  }

  bool CursorOnPicture(Cursor &c)
  {
    HWND hwnd = static_cast<HWND>(ao2::aspect::Window());
    if (!GameInFront())
    {
      return false;
    }
    POINT p;
    RECT r;
    if (!GetCursorPos(&p) || !ScreenToClient(hwnd, &p) || !GetClientRect(hwnd, &r) || r.right <= 0 || r.bottom <= 0)
    {
      return false;
    }
    // The frame is letterboxed into the window (renderer.cpp, the present).
    float gw = float(ao2::aspect::GuestWidth()), gh = float(ao2::aspect::GuestHeight());
    float sw = float(r.right), sh = float(r.bottom);
    float scale = std::min(sw / gw, sh / gh);
    float vw = std::floor(gw * scale), vh = std::floor(gh * scale);
    c.u = (float(p.x) - std::floor((sw - vw) * 0.5f)) / vw;
    c.v = (float(p.y) - std::floor((sh - vh) * 0.5f)) / vh;
    c.aspect = gh / gw;
    return c.u >= 0.0f && c.u <= 1.0f && c.v >= 0.0f && c.v <= 1.0f;
  }

  // A point of the 1280 x 720 Shell picture (above) in a scene's viewport.
  void FromSlabPicture(float x, float y, float vw, float vh, float &out_x, float &out_y)
  {
    out_x = x * vw / 1280.0f;
    out_y = vh * 0.5f + (y - 360.0f) * vw / 1280.0f;
  }

  // The cursor in a widget's space; false if it is not on it (on the slabs:
  // not on that texture).
  bool PointIn(const Widget &w, const Cursor &c, float &x, float &y)
  {
    if (!w.slab)
    {
      x = c.u * w.vw;
      y = c.v * w.vh;
      return true;
    }
    float u, v;
    if (!ao2::gpu::RendererMenuProbe(uint32_t(w.vw), uint32_t(w.vh), u, v))
    {
      return false;
    }
    x = u * w.vw;
    y = v * w.vh;
    return true;
  }

  // The widget under the cursor and the cursor in its space. On the slabs
  // the nearest button within kSlabReach of the point.
  const Widget *Hit(const std::vector<Widget> &widgets, const Cursor &c, float &x, float &y)
  {
    const Widget *nearest = nullptr;
    float nearest_d = kSlabReach, nearest_x = 0.0f, nearest_y = 0.0f;
    for (const Widget &w : widgets)
    {
      float px, py;
      if (!PointIn(w, c, px, py))
      {
        continue;
      }
      // Unturned, relative to the widget.
      float rx = px - w.x0 - w.tx, ry = py - w.y0 - w.ty;
      float lx = rx * w.inv[0] + ry * w.inv[2], ly = rx * w.inv[1] + ry * w.inv[3];
      float dx = std::max({-lx, 0.0f, lx - (w.x1 - w.x0)});
      float dy = std::max({-ly, 0.0f, ly - (w.y1 - w.y0)});
      dx /= kSlabReachAlong;
      float d = w.slab ? std::sqrt(dx * dx + dy * dy) : (dx + dy > 0.0f ? kSlabReach : 0.0f);
      if (d == 0.0f || d < nearest_d)
      {
        nearest = &w;
        nearest_d = d;
        nearest_x = px;
        nearest_y = py;
        if (d == 0.0f)
        {
          break;
        }
      }
    }
    x = nearest_x;
    y = nearest_y;
    if (!nearest)
    {
      // For the log: where it is, on whatever it is on.
      for (const Widget &w : widgets)
      {
        if (w.slab && PointIn(w, c, x, y))
        {
          break;
        }
      }
    }
    return nearest;
  }

  // ao2_menu_mouse_test_file: the cursor from that file (read every half second).
  bool TestCursor(Cursor &c)
  {
    static std::string path = REXCVAR_GET(ao2_menu_mouse_test_file);
    static uint32_t next_read = 0;
    static float x = -1.0f, y = -1.0f;
    if (path.empty())
    {
      return false;
    }
    if (int32_t(GetTickCount() - next_read) >= 0)
    {
      next_read = GetTickCount() + 500;
      x = y = -1.0f;
      if (FILE *f = std::fopen(path.c_str(), "r"))
      {
        if (std::fscanf(f, "%f %f", &x, &y) != 2)
        {
          x = y = -1.0f;
        }
        std::fclose(f);
      }
    }
    float gw = float(ao2::aspect::GuestWidth()), gh = float(ao2::aspect::GuestHeight());
    if (x < 0.0f || y < 0.0f)
    {
      return false;
    }
    c.u = x / gw;
    c.v = y / gh;
    c.aspect = gh / gw;
    return true;
  }

  bool g_menu = false; // the cursor is the menus'
  void SetMenuMode(bool menu)
  {
    if (menu == g_menu)
    {
      return;
    }
    g_menu = menu;
    // rexruntime.dll's rex_input_set_mouse_look: the driver lets the cursor go
    // or captures it again (nothing with a runtime without it).
    using Fn = void (*)(uint32_t);
    static Fn set_mouse_look = []() -> Fn {
      HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
      return runtime ? reinterpret_cast<Fn>(GetProcAddress(runtime, "rex_input_set_mouse_look")) : nullptr;
    }();
    if (set_mouse_look)
    {
      set_mouse_look(menu ? 0 : 1);
    }
    if (menu && g_window)
    {
      rex::ui::Window *window = g_window;
      window->app_context().CallInUIThreadDeferred(
          [window]() { window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible); });
    }
    Logf("menu mode %d\n", menu);
  }

  // --- Exit -----------------------------------------------------------------

  struct Children
  {
    std::vector<uint32_t> list;
    uint32_t Find(const uint8_t *base, const char *name) const
    {
      for (uint32_t o : list)
      {
        if (ao2::ue3::ObjectName(base, o) == name)
        {
          return o;
        }
      }
      return 0;
    }
  };

  Children ChildrenOf(const uint8_t *base, uint32_t object)
  {
    Children c;
    uint32_t data = LoadU32(base, object + kChildren);
    uint32_t count = LoadU32(base, object + kChildren + 4);
    for (uint32_t i = 0; data && i < count && i < 256; ++i)
    {
      if (uint32_t o = LoadU32(base, data + i * 4))
      {
        c.list.push_back(o);
      }
    }
    return c;
  }

  // The Exit button of a main menu scene, 0 if it has none (yet). Looked up
  // each time: a new main menu may take the place of the last one.
  uint32_t ExitOf(const uint8_t *base, uint32_t scene)
  {
    return ChildrenOf(base, scene).Find(base, "Exit");
  }

  // The FName index of `text` (number 0), 0 if it is not a name.
  uint32_t NameIndex(const uint8_t *base, const char *text)
  {
    uint32_t count = LoadU32(base, ao2::ue3::kGNamesArray + 4);
    for (uint32_t i = 1; i < count && i < 0x100000; ++i)
    {
      if (ao2::ue3::Name(base, i) == text)
      {
        return i;
      }
    }
    return 0;
  }

  // A copy of the main menu's Extras button, named Exit, added to `scene`
  // (the main menu itself, or the help text scene for its label); 0 if it
  // could not be made.
  uint32_t CloneExtras(PPCContext &ctx, uint8_t *base, uint32_t scene, uint32_t extras)
  {
    uint32_t cls = LoadU32(base, extras + kClass);
    // Named, or the copy would take the name Extras (and Extras another).
    static uint32_t name = NameIndex(base, "Exit");
    uint32_t button = CallGuest(ctx, base, kCreateWidget, scene, scene, cls, extras, uint64_t(name) << 32);
    if (!button)
    {
      Logf("exit: CreateWidget failed in %08X\n", scene);
      return 0;
    }
    // Its own WidgetID (+340, a GUID) and WidgetTag (+356, an FName): with
    // Extras' InsertChild renamed Extras.
    std::random_device random;
    for (uint32_t i = 0; i < 16; i += 4)
    {
      StoreU32(base, button + 340 + i, random());
    }
    StoreU32(base, button + 356, name);
    StoreU32(base, button + 360, 0);
    int32_t index = int32_t(CallGuest(ctx, base, Virtual(base, scene, kInsertChild), scene, button,
                                      uint32_t(-1), 1));
    if (index < 0)
    {
      Logf("exit: InsertChild failed in %08X\n", scene);
      return 0;
    }
    for (int face = 0; face < 4; ++face)
    {
      StoreU32(base, button + kDockTargets + 4 + face * 4, 0);
    }
    ao2::gfxmenu::SetCaption(ctx, base, button, "Exit");
    return button;
  }

  void UpdatePositions(uint8_t *base, uint32_t scene)
  {
    StoreU32(base, scene + kSceneFlags,
             LoadU32(base, scene + kSceneFlags) | kUpdateDocking | kUpdatePositions | kUpdateNavigation);
  }

  // The main menu's Exit: an invisible button under Extras that takes the
  // focus (pad and keyboard navigation reach it as any other), 0 if it could
  // not be made (tried again after 2 s). It is drawn as a label in the help
  // text scene (ExitLabel): the menu's texture ends right under Extras.
  uint32_t AddExit(PPCContext &ctx, uint8_t *base, uint32_t scene)
  {
    static uint32_t next_try = 0;
    if (int32_t(GetTickCount() - next_try) < 0)
    {
      return 0;
    }
    next_try = GetTickCount() + 2000;
    uint32_t extras = ChildrenOf(base, scene).Find(base, "Extras");
    uint32_t button = extras ? CloneExtras(ctx, base, scene, extras) : 0;
    if (!button)
    {
      return 0;
    }
    StoreF32(base, button + kOpacity, 0.0f);
    StoreU32(base, extras + kForcedNavigation + kFaceBottom * 4, button);
    StoreU32(base, button + kForcedNavigation + kFaceTop * 4, extras);
    StoreU32(base, button + kForcedNavigation + kFaceBottom * 4, 0);
    UpdatePositions(base, scene);
    Logf("exit: button %08X in %08X\n", button, scene);
    return button;
  }

  // The label of the main menu's Exit in the help text scene `help` (drawn
  // straight to the screen), made on first use; 0 if there is none.
  uint32_t ExitLabel(PPCContext &ctx, uint8_t *base, uint32_t help, uint32_t menu)
  {
    if (uint32_t label = ChildrenOf(base, help).Find(base, "Exit"))
    {
      return label;
    }
    static uint32_t next_try = 0;
    if (!menu || int32_t(GetTickCount() - next_try) < 0)
    {
      return 0;
    }
    next_try = GetTickCount() + 2000;
    uint32_t extras = ChildrenOf(base, menu).Find(base, "Extras");
    uint32_t label = extras ? CloneExtras(ctx, base, help, extras) : 0;
    if (!label)
    {
      return 0;
    }
    // On the piece of paper under Extras, not turned (Extras' rotation is
    // about a point of the menu's texture).
    float vw = LoadF32(base, help + kViewportSize), vh = LoadF32(base, help + kViewportSize + 4);
    float x0, y0, x1, y1;
    FromSlabPicture(kExitX0, kExitY0, vw, vh, x0, y0);
    FromSlabPicture(kExitX1, kExitY1, vw, vh, x1, y1);
    const float bounds[4] = {x0, y0, x1, y1};
    for (int face = 0; face < 4; ++face)
    {
      StoreF32(base, label + kPosition + face * 4, bounds[face]);
    }
    // Tilted as the text on the slabs: Rotation.TransformMatrix (row vectors,
    // widget space) turns about the widget's centre, as Extras' own does.
    std::memset(base + label + kRotation, 0, 96);
    float angle = kExitTilt * 3.14159265f / 180.0f, c = std::cos(angle), s = std::sin(angle);
    float cx = (x1 - x0) * 0.5f, cy = (y1 - y0) * 0.5f;
    const float matrix[16] = {c,  s,  0.0f, 0.0f, -s, c, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                              cx - (cx * c - cy * s), cy - (cx * s + cy * c), 0.0f, 1.0f};
    for (int i = 0; i < 16; ++i)
    {
      StoreF32(base, label + kRotation + 16 + i * 4, matrix[i]);
    }
    StoreF32(base, label + kRotation + 80, kExitTilt);
    UpdatePositions(base, help);
    Logf("exit: label %08X in %08X at %.0f %.0f %.0f %.0f\n", label, help, x0, y0, x1, y1);
    return label;
  }

  void SetHidden(uint8_t *base, uint32_t widget, bool hidden)
  {
    uint32_t word = LoadU32(base, widget + kHiddenWord);
    uint32_t want = hidden ? (word | kHiddenMask) : (word & ~kHiddenMask);
    if (want != word)
    {
      StoreU32(base, widget + kHiddenWord, want);
    }
  }

  void Quit()
  {
    static bool quitting = false;
    if (quitting)
    {
      return;
    }
    quitting = true;
    Logf("exit\n");
    if (rex::ui::Window *window = g_window)
    {
      window->app_context().CallInUIThreadDeferred([window]() { window->RequestClose(); });
    }
  }

  // --- The clicks -----------------------------------------------------------

  bool g_left = false;                      // the left mouse button as last seen
  uint16_t g_inject = 0;                    // pad buttons held for the menu
  int g_inject_polls = 0;                   // for this many more polls
  uint32_t g_hover = 0;                     // the widget under the cursor
  float g_last_u = -1.0f, g_last_v = -1.0f; // the cursor as last seen
  uint32_t g_moved = 0;                     // when it last moved (GetTickCount)
  // Pointing counts this long after a move: the slabs' answer comes a few
  // frames later. A cursor resting where a menu opens leaves its first item.
  constexpr uint32_t kMoveWindow = 300;
  bool g_enter = false, g_esc = false;      // the keys as last seen...
  bool g_enter_menu = false, g_esc_menu = false; // ...and whether a menu was up when they were pressed

  // The focus goes to a pointed widget the way the pad takes it, one D-pad
  // step at a time: some menus' scripts follow the navigation, not the
  // focus (the weapon upgrades show the part navigated to). If a step does
  // not move the focus, the widget simply gets it (SetFocus).
  struct Walk
  {
    uint32_t target = 0;
    uint16_t then = 0;   // pressed once there (a click), 0 = none
    uint16_t button = 0; // the D-pad direction pressed
    int phase = 0;       // 1: the button held, -1: let go, 0: the next step
    uint32_t since = 0;  // when the phase began (GetTickCount)
    uint32_t from = 0;   // the focus when it was pressed
    int steps = 0, stuck = 0;
    bool jumped = false; // the focus was put next to the target
  } g_walk;
  // A step is held and let go about as long as a finger does: a menu's own
  // script may read the pad apart from the UI's navigation and miss a
  // press of a frame or two (the objectives).
  constexpr uint32_t kWalkPressMs = 50, kWalkReleaseMs = 50;
  // The objectives move their pick with the left stick, the D-pad steps
  // there do not reach it: the step is the stick pushed up or down.
  bool g_walk_stick = false;
  constexpr int kWalkSteps = 16;

  const Widget *Find(const std::vector<Widget> &widgets, uint32_t object)
  {
    for (const Widget &w : widgets)
    {
      if (w.object == object)
      {
        return &w;
      }
    }
    return nullptr;
  }

  uint32_t FocusedWidget(const uint8_t *base, const std::vector<Widget> &widgets)
  {
    for (const Widget &w : widgets)
    {
      if (Focused(base, w.object))
      {
        return w.object;
      }
    }
    return 0;
  }

  // The Options screen and its Graphics and Controls pages: nothing follows
  // the navigation there, so a pointed row gets the focus at once instead of
  // the focus passing every row on the way.
  // The objectives (the pause menu's, the briefing's): left and right turn
  // their pages, so the focus only steps up and down there. They keep their
  // own pick (its description shown) and move it only with the D-pad's
  // steps: a focus set directly goes back to it.
  bool Objectives(const uint8_t *base, uint32_t widget)
  {
    uint32_t scene = LoadU32(base, widget + kOwnerScene);
    return scene && ao2::ue3::ObjectName(base, scene).find("Objective") != std::string::npos;
  }

  bool FocusAtOnce(const uint8_t *base, uint32_t widget)
  {
    uint32_t scene = LoadU32(base, widget + kOwnerScene);
    return scene && ao2::ue3::ObjectName(base, scene).rfind("AllOptionsUIScene", 0) == 0;
  }

  // The widget one D-pad step from `target` (up or down first: from an option
  // a side step changes its value), the one nearest to the focus `now`; 0
  // if none is pointed at.
  uint32_t NeighbourOf(const uint8_t *base, const std::vector<Widget> &widgets, const Widget &target, uint32_t now)
  {
    const Widget *focus = now ? Find(widgets, now) : nullptr;
    uint32_t best = 0;
    float best_d = 1e30f;
    for (int face : {1, 3, 0, 2})
    {
      uint32_t next = LoadU32(base, target.object + kNavigationTargets + face * 4);
      const Widget *n = next && next != target.object ? Find(widgets, next) : nullptr;
      bool side = face == 0 || face == 2;
      if (!n || n->slab != target.slab || (side && (n->kind == Kind::kValue || Objectives(base, target.object))))
      {
        continue;
      }
      if (next == now)
      {
        return 0; // one step already
      }
      float d = focus ? std::fabs(n->x0 + n->x1 - focus->x0 - focus->x1) + std::fabs(n->y0 + n->y1 - focus->y0 - focus->y1)
                      : 0.0f;
      if (d < best_d)
      {
        best_d = d;
        best = next;
      }
      if (face == 3 && best)
      {
        break; // above or below found: no side step
      }
    }
    return best;
  }

  void WalkTo(uint32_t target, uint16_t then)
  {
    if (g_walk.target != target)
    {
      g_walk = Walk{};
      g_walk.target = target;
    }
    g_walk.then = then ? then : g_walk.then;
  }

  // The pad buttons of this poll's step.
  uint16_t WalkStep(PPCContext &ctx, uint8_t *base, const std::vector<Widget> &widgets)
  {
    Walk &w = g_walk;
    const Widget *target = w.target ? Find(widgets, w.target) : nullptr;
    if (!target)
    {
      w = Walk{};
      return 0;
    }
    uint32_t tick = GetTickCount();
    if (w.phase > 0)
    {
      if (tick - w.since < kWalkPressMs)
      {
        return w.button;
      }
      w.phase = -1;
      w.since = tick;
    }
    if (w.phase < 0)
    {
      if (tick - w.since < kWalkReleaseMs)
      {
        return 0;
      }
      w.phase = 0;
    }
    bool objectives = Objectives(base, w.target);
    uint32_t now = FocusedWidget(base, widgets);
    if (w.steps)
    {
      w.stuck = now == w.from ? w.stuck + 1 : 0;
    }
    auto arrived = [&]() {
      if (w.then)
      {
        g_inject = w.then;
        g_inject_polls = 2;
      }
      w = Walk{};
    };
    if (now != w.target && FocusAtOnce(base, w.target))
    {
      Focus(ctx, base, w.target);
      now = w.target;
    }
    // The objectives keep their own pick and put the focus back on it: only
    // the D-pad's steps move it there, one at a time.
    if (now != w.target && !w.steps && !w.jumped && !objectives)
    {
      // Not every row on the way: the focus is put next to the target and one
      // step goes in, so a menu following the navigation sees it arrive.
      w.jumped = true;
      if (uint32_t next = NeighbourOf(base, widgets, *target, now))
      {
        Focus(ctx, base, next);
        now = next;
      }
    }
    if (now == w.target)
    {
      arrived();
      return 0;
    }
    if (objectives && !now && !w.steps)
    {
      // Nothing picked yet (the menu just opened): the first step picks.
      w.button = kDpadDown;
      w.phase = 1;
      w.since = tick;
      g_walk_stick = true;
      ++w.steps;
      return w.button;
    }
    const Widget *from = now ? Find(widgets, now) : nullptr;
    if (!from || from->slab != target->slab || w.stuck >= 2 || w.steps >= kWalkSteps)
    {
      if (objectives)
      {
        Logf("objectives: stopped at %s, target %s\n", now ? ao2::ue3::ObjectName(base, now).c_str() : "nothing",
             ao2::ue3::ObjectName(base, w.target).c_str());
        w = Walk{}; // set, it would go back: as far as the steps got
        return 0;
      }
      Focus(ctx, base, w.target);
      arrived();
      return 0;
    }
    float dx = (target->x0 + target->x1 - from->x0 - from->x1) * 0.5f;
    float dy = (target->y0 + target->y1 - from->y0 - from->y1) * 0.5f;
    // Left / right change an option's value: from one, only up and down.
    // In the objectives they turn the page.
    if (std::fabs(dy) >= std::fabs(dx) * 0.5f || from->kind == Kind::kValue || objectives)
    {
      w.button = dy < 0.0f ? kDpadUp : kDpadDown; // the same place (Exit under Extras): down
    }
    else
    {
      w.button = dx < 0.0f ? kDpadLeft : kDpadRight;
    }
    w.from = now;
    w.phase = 1;
    w.since = tick;
    g_walk_stick = objectives;
    if (objectives)
    {
      Logf("objectives: %s from %s to %s\n", w.button == kDpadUp ? "up" : "down",
           ao2::ue3::ObjectName(base, now).c_str(), ao2::ue3::ObjectName(base, w.target).c_str());
    }
    ++w.steps;
    return w.button;
  }
} // namespace

namespace ao2::menumouse
{
  void Attach(rex::ui::Window *window) { g_window = window; }

  void OnPadPoll(PPCContext &ctx, uint8_t *base, uint32_t state, bool keyboard)
  {
    if (ao2::menu::IsOpen() || ao2::gfxmenu::Capturing())
    {
      // The players window (invites) or the debug menu is up, or the
      // Controls page waits for a key: the mouse and Enter / Esc
      // are theirs, nothing reaches the game's menus.
      ao2::gpu::RendererSetMenuProbe(0, 0, 0, 0);
      g_inject_polls = 0;
      g_hover = 0;
      g_walk = Walk{};
      g_last_u = g_last_v = -1.0f;
      g_left = GetAsyncKeyState(VK_LBUTTON) & 0x8000;
      g_enter = GetAsyncKeyState(VK_RETURN) & 0x8000;
      g_esc = GetAsyncKeyState(VK_ESCAPE) & 0x8000;
      g_enter_menu = g_esc_menu = false;
      return;
    }
    uint8_t *p = base + state;
    uint16_t buttons = uint16_t((p[4] << 8) | p[5]);
    auto store_buttons = [&]() {
      if (ctx.r3.u32 != 0)
      {
        // No real pad: a connected, idle one.
        std::memset(p, 0, 16);
        ctx.r3.u64 = 0;
      }
      p[4] = uint8_t(buttons >> 8);
      p[5] = uint8_t(buttons);
    };

    std::vector<uint32_t> scenes = Scenes(base);
    std::vector<Widget> widgets;
    uint32_t scene = REXCVAR_GET(ao2_menu_mouse) || REXCVAR_GET(ao2_menu_exit) ? ActiveMenu(base, scenes, widgets) : 0;

    // Exit: the button in the main menu, its label in the help text scene,
    // shown while the main menu is the menu and focused with its button.
    uint32_t exit = 0, label = 0;
    bool main_menu = scene && REXCVAR_GET(ao2_menu_exit) && IsMainMenu(base, scene);
    if (main_menu && !(exit = ExitOf(base, scene)))
    {
      exit = AddExit(ctx, base, scene);
    }
    for (uint32_t help : scenes)
    {
      if (ao2::ue3::ObjectName(base, help).rfind("HelpTextUIScene", 0) == 0)
      {
        label = exit ? ExitLabel(ctx, base, help, scene) : ChildrenOf(base, help).Find(base, "Exit");
        break;
      }
    }
    if (label)
    {
      SetHidden(base, label, !exit);
      if (exit && Focused(base, exit) != Focused(base, label))
      {
        Focus(ctx, base, label, Focused(base, exit));
      }
      if (exit)
      {
        // Hit at the label, as the help text scene's widget.
        Widget w{};
        w.object = exit;
        w.kind = Kind::kButton;
        uint32_t help = LoadU32(base, label + kOwnerScene);
        w.vw = LoadF32(base, help + kViewportSize);
        w.vh = LoadF32(base, help + kViewportSize + 4);
        w.x0 = LoadF32(base, label + kRenderBounds) - 10.0f;
        w.y0 = LoadF32(base, label + kRenderBounds + 4) - 8.0f;
        w.x1 = LoadF32(base, label + kRenderBounds + 8) + 10.0f;
        w.y1 = LoadF32(base, label + kRenderBounds + 12) + 8.0f;
        if (w.x1 > w.x0 && w.y1 > w.y0)
        {
          widgets.insert(widgets.begin(), w);
        }
      }
    }
    if (keyboard)
    {
      // Enter and Esc in the menus are accept and back (A and B), whatever
      // they are bound to (Esc is Start, the pause menu). Decided when one
      // is pressed: the Esc that opened the pause menu stays Start until let go.
      bool front = GameInFront();
      bool enter = front && (GetAsyncKeyState(VK_RETURN) & 0x8000);
      bool esc = front && (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
      if ((enter && !g_enter) || (esc && !g_esc))
      {
        bool menu = KeysMenu(base, scenes);
        g_enter_menu = enter && !g_enter ? menu : g_enter_menu;
        g_esc_menu = esc && !g_esc ? menu : g_esc_menu;
      }
      g_enter = enter;
      g_esc = esc;
      if ((enter && g_enter_menu) || (esc && g_esc_menu))
      {
        if (enter && g_enter_menu)
        {
          buttons |= kButtonA;
        }
        if (esc && g_esc_menu)
        {
          buttons = uint16_t((buttons & ~kButtonStart) | kButtonB);
        }
        store_buttons();
      }
    }
    if (exit && (buttons & kButtonA) && Focused(base, exit))
    {
      // Its own Kismet (Extras') must not see the press.
      buttons &= ~kButtonA;
      store_buttons();
      Quit();
    }
    if (!keyboard)
    {
      return;
    }

    bool menu = scene && REXCVAR_GET(ao2_menu_mouse);
    // Between two menus (the camera moving along the slabs, the press of
    // Start on the title screen) one scene is closed before the next opens:
    // only scenes taking no input are left (UIScene_0, the help text). The
    // cursor stays the menus' until the stack is empty, as it is in the game.
    SetMenuMode(menu || (g_menu && !scenes.empty() && REXCVAR_GET(ao2_menu_mouse)));
    bool left = GetAsyncKeyState(VK_LBUTTON) & 0x8000;
    bool left_pressed = left && !g_left;
    g_left = left;
    if (!menu)
    {
      ao2::gpu::RendererSetMenuProbe(0, 0, 0, 0);
      g_inject_polls = 0;
      g_hover = 0;
      g_walk = Walk{};
      g_last_u = g_last_v = -1.0f;
      return;
    }
    Cursor cursor;
    bool on_picture = CursorOnPicture(cursor);
    bool test = TestCursor(cursor);
    on_picture = on_picture || test;
    if (test)
    {
      // The button is the real mouse's, maybe clicking another window.
      left_pressed = left = false;
    }
    // The slabs: the renderer finds the menu's texture under the cursor.
    const Widget *slab = nullptr;
    for (const Widget &w : widgets)
    {
      if (w.slab)
      {
        slab = &w;
        break;
      }
    }
    ao2::gpu::RendererSetMenuProbe(slab && on_picture ? uint32_t(slab->vw) : 0, slab ? uint32_t(slab->vh) : 0,
                                   uint32_t(std::max(cursor.u, 0.0f) * float(ao2::aspect::GuestWidth())),
                                   uint32_t(std::max(cursor.v, 0.0f) * float(ao2::aspect::GuestHeight())));
    if (on_picture)
    {
      if (g_last_u < 0.0f)
      {
        // Menu mode just began: where the cursor is is no move.
        g_last_u = cursor.u;
        g_last_v = cursor.v;
      }
      if (std::fabs(cursor.u - g_last_u) + std::fabs(cursor.v - g_last_v) > 0.002f)
      {
        g_moved = GetTickCount();
        g_last_u = cursor.u;
        g_last_v = cursor.v;
      }
      bool moving = g_moved && GetTickCount() - g_moved < kMoveWindow;
      float x, y;
      const Widget *hit = Hit(widgets, cursor, x, y);
      if (test)
      {
        static uint32_t next_log = 0;
        if (int32_t(GetTickCount() - next_log) >= 0)
        {
          next_log = GetTickCount() + 1000;
          Logf("test cursor %.0f %.0f: %s at %.1f %.1f\n", cursor.u * ao2::aspect::GuestWidth(),
               cursor.v * ao2::aspect::GuestHeight(),
               hit ? ao2::ue3::ObjectName(base, hit->object).c_str() : "nothing", x, y);
        }
      }
      // Walked to when the cursor comes onto it (the slabs' answer comes a
      // few frames after the move) or clicks it; not while it rests there
      // and the keys move the focus elsewhere.
      bool entered = hit && hit->object != g_hover && moving;
      if (moving || !hit)
      {
        g_hover = hit ? hit->object : 0;
      }
      if (hit && left_pressed && exit && hit->object == exit)
      {
        Quit();
      }
      else if (hit && left_pressed)
      {
        uint16_t press = hit->kind == Kind::kButton ? kButtonA
                                                    : (x < (hit->x0 + hit->x1) * 0.5f ? kDpadLeft : kDpadRight);
        if (Focused(base, hit->object) && g_walk.target == 0)
        {
          g_inject = press;
          g_inject_polls = 2;
        }
        else
        {
          WalkTo(hit->object, press);
        }
      }
      else if (hit && entered && !Focused(base, hit->object))
      {
        // The objectives' page arrows are left alone.
        if (!Objectives(base, hit->object) || hit->kind == Kind::kButton)
        {
          WalkTo(hit->object, 0);
        }
      }
    }
    uint16_t walk = g_inject_polls > 0 ? 0 : WalkStep(ctx, base, widgets);
    // The left button is bound to a trigger (fire): not here. The right one
    // keeps its own (the weapon shop's stats).
    if (left)
    {
      p[7] = 0;
    }
    if (g_inject_polls > 0 || walk)
    {
      if (g_inject_polls > 0)
      {
        --g_inject_polls;
        buttons |= g_inject;
      }
      bool stick = walk && g_walk_stick;
      buttons |= stick ? 0 : walk;
      store_buttons();
      if (stick)
      {
        // XINPUT_GAMEPAD.sThumbLY, big-endian.
        p[10] = walk == kDpadUp ? 0x7F : 0x80;
        p[11] = walk == kDpadUp ? 0xFF : 0x00;
      }
    }
  }
} // namespace ao2::menumouse
