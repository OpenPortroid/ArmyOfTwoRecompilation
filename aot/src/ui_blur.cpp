// ui_blur.cpp - A menu blur left on after its message box closed.
//
// A message box that blurs what is behind it (UIScene_Confirmation with
// bShouldBlur: AO2PlayerController.SmallMessageBox, e.g. "Connecting to EA
// servers" when Versus is chosen) turns the blur on as it is shown
// (SetVisibility: AO2PlayerControllerNative.OverlayBlur(true, DrawGroup)) and
// off only when it is answered (A / B in CustomInputKey) or closed with
// UnHideHelpTextAndCloseScene. Closed by the game otherwise - the connection
// made, the box faded out and CloseScene'd - the blur stays on in every menu
// after it.
//
// OverlayBlur (exec sub_822EE820, native sub_823658A8) is the only thing that
// sets the blur: for each local player, bit 0x04000000 of +1808 of its
// controller's HUD (+884, AO2HUD; +1241 the draw group) and the camera's
// (+832) post process. Twice a second: a HUD blurs while no open scene is
// such a message box - the game's own OverlayBlur(false) is called.
#include "ui_blur.h"

#include "log_files.h"
#include "ue3_reflect.h"

#include <bit>
#include <cstdio>
#include <cstring>

#include <rex/ppc.h>

namespace
{
  constexpr uint32_t kEngine = 0x831EFCBC;      // GEngine
  constexpr uint32_t kEnginePlayers = 760;      // GamePlayers (TArray of LocalPlayer)
  constexpr uint32_t kEngineViewport = 772;     // GameViewport
  constexpr uint32_t kPlayerController = 64;    // Player.Actor
  constexpr uint32_t kControllerHud = 884;      // PlayerController.myHUD
  constexpr uint32_t kHudFlags = 1808;
  constexpr uint32_t kHudBlur = 0x04000000;     // ... the overlay blur on
  constexpr uint32_t kOverlayBlur = 0x823658A8; // r4 bEnabled, r5 DrawGroup
  constexpr uint32_t kCheckFrames = 30;

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("ui_blur.log");
    return f;
  }

  // A local player's HUD that blurs (0: none).
  uint32_t BlurringHud(const uint8_t *base)
  {
    static uint32_t hud_class = 0;
    hud_class = hud_class ? hud_class : ao2::ue3::FindClass(base, "AO2HUD");
    uint32_t engine = LoadU32(base, kEngine);
    uint32_t players = engine ? LoadU32(base, engine + kEnginePlayers) : 0;
    uint32_t count = engine ? LoadU32(base, engine + kEnginePlayers + 4) : 0;
    for (uint32_t i = 0; players && hud_class && i < count && i < 4; ++i)
    {
      uint32_t player = LoadU32(base, players + i * 4);
      uint32_t controller = player ? LoadU32(base, player + kPlayerController) : 0;
      uint32_t hud = controller ? LoadU32(base, controller + kControllerHud) : 0;
      if (hud && ao2::ue3::IsA(base, hud, hud_class) && (LoadU32(base, hud + kHudFlags) & kHudBlur))
      {
        return hud;
      }
    }
    return 0;
  }

  // An open scene is a message box that blurs (its blur is wanted). True as
  // well when the scenes cannot be read (the blur is then left alone).
  bool BlurringBoxOpen(const uint8_t *base, uint32_t &scenes)
  {
    static int32_t ui_controller = -2, scene_client = -2, active = -2, blur = -2;
    static uint32_t blur_mask = 0, box_class = 0;
    if (ui_controller == -2)
    {
      ui_controller = ao2::ue3::PropertyOffset(base, "GameViewportClient", "UIController");
      scene_client = ao2::ue3::PropertyOffset(base, "UIInteraction", "SceneClient");
      active = ao2::ue3::PropertyOffset(base, "GameUISceneClient", "ActiveScenes");
      if (!ao2::ue3::BoolProperty(base, "UIScene_Confirmation", "bShouldBlur", blur, blur_mask))
      {
        blur = -1;
      }
      box_class = ao2::ue3::FindClass(base, "UIScene_Confirmation");
      if (FILE *f = Log())
      {
        std::fprintf(f, "GameViewportClient.UIController +%d, UIInteraction.SceneClient +%d, "
                        "GameUISceneClient.ActiveScenes +%d, UIScene_Confirmation %08X bShouldBlur +%d mask %08X\n",
                     ui_controller, scene_client, active, box_class, blur, blur_mask);
        std::fflush(f);
      }
    }
    if (ui_controller < 0 || scene_client < 0 || active < 0 || blur < 0 || !box_class)
    {
      return true;
    }
    uint32_t engine = LoadU32(base, kEngine);
    uint32_t viewport = engine ? LoadU32(base, engine + kEngineViewport) : 0;
    uint32_t controller = viewport ? LoadU32(base, viewport + ui_controller) : 0;
    uint32_t client = controller ? LoadU32(base, controller + scene_client) : 0;
    if (!client)
    {
      return true;
    }
    uint32_t data = LoadU32(base, client + active);
    scenes = LoadU32(base, client + active + 4);
    for (uint32_t i = 0; data && i < scenes && i < 64; ++i)
    {
      uint32_t scene = LoadU32(base, data + i * 4);
      if (scene && ao2::ue3::IsA(base, scene, box_class) && (LoadU32(base, scene + blur) & blur_mask))
      {
        return true;
      }
    }
    return false;
  }
} // namespace

namespace ao2::ui_blur
{
  void Tick(PPCContext &ctx, uint8_t *base)
  {
    static uint32_t frames = 0;
    if (++frames < kCheckFrames)
    {
      return;
    }
    frames = 0;
    uint32_t hud = BlurringHud(base);
    uint32_t scenes = 0;
    if (!hud || BlurringBoxOpen(base, scenes))
    {
      return;
    }
    if (FILE *f = Log())
    {
      std::fprintf(f, "the blur is on (HUD %08X) with no message box that blurs open (%u scenes): off\n", hud, scenes);
      std::fflush(f);
    }
    PPCContext saved = ctx;
    ctx.r1.u64 = (ctx.r1.u32 - 1024) & ~15u;
    ctx.r3.u64 = 0; // the controller: not used
    ctx.r4.u64 = 0; // bEnabled
    ctx.r5.u64 = 0; // DrawGroup
    rex::runtime::ResolveIndirectFunction(kOverlayBlur)(ctx, base);
    ctx = saved;
  }
} // namespace ao2::ui_blur
