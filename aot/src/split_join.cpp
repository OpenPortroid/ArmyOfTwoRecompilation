// split_join.cpp - Split-screen: who plays with what.
//
// The Splitscreen button of Campaign Mode runs, in its Kismet sequence:
// UIAction_GetLoggedInPlayerCount (fewer than two signed in: "needs two players"),
// UIAction_MapSecondController (LocalPlayer 1 gets the second signed-in user),
// then the console command "causeevent MissionSetup_Splitscreen", which comes to
// the script function PlayerController.CauseEvent of every PlayerController. Both
// script functions are caught at UObject::ProcessInternal (script_trace.cpp).
//
// At the count a window opens with the two players, and player 2 is signed in for
// the sequence to go on: with the keyboard for now, which works without any
// gamepad (the window is modal, so nothing is played with it). The event is held
// back. Player 1 presses a button, then player 2. What
// each pressed is theirs: a key or a mouse button gives the keyboard and the
// mouse, a gamepad button that gamepad (rexruntime's rex_input_split_players).
// A device nobody pressed plays nobody. Esc or B undoes the last player, and with
// nobody in closes the window: the menu stays where it was.
//
// With both in, the command runs again and goes through once player 2 is signed
// in (the runtime looks at the devices every 250 ms; the event may ask for player 2
// at once). Another Campaign Mode button, or the main menu after a level, gives
// every device back to the automatic assignment.
//
// A versus lobby's Guest Sign-In opens the same window (OpenForGuest, from
// versus_guest.cpp): nobody is signed in for the time being and nothing waits for
// it. With both in, the runtime signs player 2 in, who becomes the guest. The
// devices go back when versus_guest.cpp says so (CloseGuest: out of every lobby
// and match), not in the main menu, where the lobby also is.
#include "console_cmd.h"
#include "coop.h"
#include "debug_menu.h"
#include "log_files.h"
#include "ue3_reflect.h"
#include "ui_text.h"
#include "ui_theme.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ppc.h>

extern "C" REX_FUNC(sub_82B06D90); // XamUserGetSigninState (versus_guest.cpp)

namespace ao2::split_join
{
  // The UFunctions, 0 until found (script_trace.cpp compares with them).
  extern uint32_t g_cause_event;
  extern uint32_t g_login_status;
}

namespace
{
  using ao2::text::Tr;
  namespace ui = ao2::ui;
  using ui::FontKind;

  constexpr uint32_t kNoUser = 0xFFFFFFFF;
  constexpr double kGrace = 0.35;   // what selected the menu's button is no press here
  constexpr double kDoneShow = 0.6; // both in: the window stays a moment

  std::atomic<bool> g_window_open{false};
  std::atomic<bool> g_resume{false}; // the next Splitscreen command is the window's own
  std::atomic<bool> g_active{false}; // the players have the devices they pressed on
  std::atomic<bool> g_left_shell{false}; // a level was entered since
  std::atomic<bool> g_guest_devices{false}; // a versus guest's window gave them
  std::atomic<bool> g_guest_close{false};   // the lobby went while its window was open
  std::chrono::steady_clock::time_point g_pass_until; // game thread: Splitscreen events go through until then

  void Log(const char *what)
  {
    static FILE *log = ao2::OpenLogFile("split_join.log");
    REXLOG_INFO("split_join: {}", what);
    if (log)
    {
      std::fprintf(log, "%s\n", what);
      std::fflush(log);
    }
  }

  template <typename Fn>
  Fn Runtime(const char *name)
  {
    HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
    return runtime ? reinterpret_cast<Fn>(GetProcAddress(runtime, name)) : nullptr;
  }

  void SplitPlayers(uint32_t on, uint32_t keyboard_user, uint32_t p1_pads, uint32_t p2_pads)
  {
    using Fn = void (*)(uint32_t, uint32_t, uint32_t, uint32_t);
    static Fn fn = Runtime<Fn>("rex_input_split_players");
    if (fn)
    {
      fn(on, keyboard_user, p1_pads, p2_pads);
    }
  }

  uint32_t Gamepads()
  {
    using Fn = uint32_t (*)();
    static Fn fn = Runtime<Fn>("rex_input_gamepads");
    return fn ? fn() : 0;
  }

  // A key or mouse button went down this frame. Esc is the window's own; Alt, Tab
  // and the Windows keys leave the game; the rest of the named keys up to the
  // gamepad's are the keyboard's.
  bool KeyboardPressed()
  {
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
      return true;
    }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; ++k)
    {
      ImGuiKey key = ImGuiKey(k);
      if (key == ImGuiKey_Escape || key == ImGuiKey_Tab || key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
          key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper)
      {
        continue;
      }
      if (ImGui::IsKeyPressed(key, false))
      {
        return true;
      }
    }
    return false;
  }

  void DevicesBack(const char *why)
  {
    if (g_active.exchange(false))
    {
      SplitPlayers(0, 0, 0, 0);
      Log(why);
    }
  }

  class JoinWindow
  {
  public:
    // on_cancel: a versus guest's window (campaign: null), called when it is closed
    // without both players.
    explicit JoinWindow(void (*on_cancel)() = nullptr) : guest_(on_cancel != nullptr), on_cancel_(on_cancel) {}

    // UI thread, inside an ImGui frame; false closes the window.
    bool Draw()
    {
      double now = ImGui::GetTime();
      if (guest_ && g_guest_close.exchange(false))
      {
        Cancel();
        return false;
      }
      uint32_t back = 0;
      uint32_t held = ao2::menu::PressedPads(&back);
      if (opened_ < 0.0)
      {
        opened_ = now;
        held_ = held; // the pad button that chose Splitscreen may still be down
      }
      uint32_t pressed = held & ~held_;
      back &= pressed;
      held_ = held;
      if (done_ < 0.0 && !Input(now, pressed, back))
      {
        Cancel();
        return false;
      }
      if (done_ >= 0.0 && now - done_ >= kDoneShow)
      {
        Finish();
        return false;
      }
      Paint(now);
      return true;
    }

  private:
    struct Slot
    {
      bool keyboard = false;
      uint32_t pad = 0;
      double at = 0.0;
    };

    bool Taken(bool keyboard, uint32_t pad) const
    {
      for (int i = 0; i < count_; ++i)
      {
        if (slots_[i].keyboard == keyboard && (keyboard || slots_[i].pad == pad))
        {
          return true;
        }
      }
      return false;
    }

    void Join(bool keyboard, uint32_t pad, double now)
    {
      if (count_ < 2 && !Taken(keyboard, pad))
      {
        slots_[count_++] = Slot{keyboard, pad, now};
      }
    }

    // False: nobody is in and the player went back.
    bool Input(double now, uint32_t pressed, uint32_t back)
    {
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || back)
      {
        if (count_ == 0)
        {
          return false;
        }
        --count_;
        return true;
      }
      if (now - opened_ < kGrace)
      {
        return true;
      }
      for (uint32_t rest = pressed; rest; rest &= rest - 1)
      {
        Join(false, uint32_t(std::countr_zero(rest)), now);
      }
      if (KeyboardPressed())
      {
        Join(true, 0, now);
      }
      if (count_ == 2)
      {
        done_ = now;
      }
      return true;
    }

    void Cancel()
    {
      g_window_open = false;
      if (guest_)
      {
        on_cancel_();
      }
      else
      {
        DevicesBack("cancelled: devices back");
      }
    }

    void Finish()
    {
      uint32_t keyboard_user = kNoUser, pads[2] = {};
      for (uint32_t i = 0; i < 2; ++i)
      {
        if (slots_[i].keyboard)
        {
          keyboard_user = i;
        }
        else
        {
          pads[i] = 1u << slots_[i].pad;
        }
      }
      SplitPlayers(1, keyboard_user, pads[0], pads[1]);
      char what[96];
      std::snprintf(what, sizeof(what), "players in: keyboard user %d, pads %X and %X", int(keyboard_user), pads[0], pads[1]);
      Log(what);
      if (guest_)
      {
        g_guest_devices = true;
        g_window_open = false;
        return;
      }
      g_left_shell = false;
      g_active = true;
      g_resume = true;
      g_window_open = false;
      ao2::console::Queue("exec causeevent MissionSetup_Splitscreen");
    }

    void Paint(double now)
    {
      const ImGuiViewport *vp = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(vp->Pos);
      ImGui::SetNextWindowSize(vp->Size);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
      ImGui::Begin("##split_join", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::PopStyleVar(2);

      ImDrawList *dl = ImGui::GetWindowDrawList();
      const float s = ui::Scale();
      const float since = float(now - opened_);
      const float a = ui::Clamp01(since / 0.2f);
      const float pulse = 0.5f + 0.5f * std::sin(float(now) * 4.0f);
      dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), ui::Fade(ui::kInk, 0.62f * a));

      const float gap = 16.0f * s, pad = 32.0f * s, card_h = 150.0f * s;
      const float width = std::min(800.0f * s, vp->Size.x - 40.0f * s);
      const float panel_h = 300.0f * s;
      const ImVec2 c = vp->GetCenter();
      const ImVec2 mn(c.x - width * 0.5f, c.y - panel_h * 0.5f + (1.0f - ui::EaseOutCubic(since / 0.3f)) * 24.0f * s);
      const ImVec2 mx(mn.x + width, mn.y + panel_h);
      ui::Card(dl, mn, mx, ui::Fade(ui::kPanel, 0.97f * a));

      float y = mn.y + pad - 6.0f * s;
      ui::Caps(dl, FontKind::kTitle, ImVec2(mn.x + pad, y), ui::Fade(ui::kAccentLight, a), Tr("SPLIT-SCREEN"));
      y += ui::FontSize(FontKind::kTitle) + 6.0f * s;
      ui::Text(dl, FontKind::kSmall, ImVec2(mn.x + pad, y), ui::Fade(ui::kMuted, a),
               Tr("Each player presses a button on their device: the keyboard or a gamepad."));
      y += ui::FontSize(FontKind::kSmall) + 18.0f * s;

      const float card_w = (width - pad * 2.0f - gap) * 0.5f;
      for (int i = 0; i < 2; ++i)
      {
        const ImVec2 cmn(mn.x + pad + float(i) * (card_w + gap), y);
        const ImVec2 cmx(cmn.x + card_w, cmn.y + card_h);
        const bool in = i < count_, current = i == count_;
        ui::Card(dl, cmn, cmx, ui::Fade(ui::kWhite, (in ? 0.09f : current ? 0.05f + 0.04f * pulse : 0.03f) * a));
        if (current)
        {
          ui::Frame(dl, cmn, cmx, ui::Fade(ui::kAccent, (0.45f + 0.55f * pulse) * a), 1.5f * s);
        }
        else if (in)
        {
          ui::Bar(dl, cmn, cmx, ui::Fade(ui::kGreen, a));
        }
        const float tx = cmn.x + 22.0f * s;
        ui::Caps(dl, FontKind::kHeading, ImVec2(tx, cmn.y + 20.0f * s), ui::Fade(in ? ui::kAccentLight : ui::kText, a),
                 Tr(i == 0 ? "PLAYER 1" : "PLAYER 2"));
        const ImVec2 line(tx, cmn.y + 76.0f * s);
        if (in)
        {
          char device[96];
          if (slots_[i].keyboard)
          {
            std::snprintf(device, sizeof(device), "%s", Tr("Keyboard and mouse"));
          }
          else
          {
            std::snprintf(device, sizeof(device), Tr("Gamepad %u"), slots_[i].pad + 1);
          }
          const float pop = ui::EaseOutBack(float(now - slots_[i].at) / 0.25f);
          const float mark = 10.0f * s;
          ui::Check(dl, ImVec2(line.x + mark, line.y + ui::FontSize(FontKind::kBody) * 0.55f), mark * 2.0f * pop, 2.0f * s,
                    ui::Fade(ui::kGreen, a));
          ui::Text(dl, FontKind::kBody, ImVec2(line.x + mark * 2.0f + 10.0f * s, line.y), ui::Fade(ui::kText, a), device);
        }
        else if (current)
        {
          ui::Text(dl, FontKind::kBody, line, ui::Fade(ui::kAccent, (0.65f + 0.35f * pulse) * a), Tr("Press any button"));
        }
        else
        {
          ui::Text(dl, FontKind::kBody, line, ui::Fade(ui::kMuted, 0.7f * a), Tr("Waiting for player 1"));
        }
      }
      y += card_h + 18.0f * s;

      // The other player's device may not be there yet.
      const bool no_device = count_ == 1 && slots_[0].keyboard && Gamepads() == 0;
      float fx = mn.x + pad;
      const float cap = ui::FontSize(FontKind::kSmall) + 8.0f * s;
      fx += ui::KeyCap(dl, ImVec2(fx, y), cap, "Esc", a) + 6.0f * s;
      fx += ui::KeyCap(dl, ImVec2(fx, y), cap, "B", a) + 8.0f * s;
      ui::Text(dl, FontKind::kSmall, ImVec2(fx, y + 4.0f * s), ui::Fade(ui::kMuted, a), Tr("Back"));
      if (no_device)
      {
        const char *hint = Tr("Connect a gamepad for player 2.");
        ui::Text(dl, FontKind::kSmall, ImVec2(mx.x - pad - ui::TextSize(FontKind::kSmall, hint).x, y + 4.0f * s),
                 ui::Fade(ui::kRed, a), hint);
      }
      ImGui::End();
    }

    bool guest_;
    void (*on_cancel_)();
    Slot slots_[2];
    int count_ = 0;
    double opened_ = -1.0, done_ = -1.0;
    uint32_t held_ = 0;
  };

  // Player 2 is signed in a moment after getting a device.
  void WaitForPlayer2(PPCContext &ctx, uint8_t *base)
  {
    PPCContext saved = ctx;
    bool in = false;
    for (int i = 0; i < 30; ++i)
    {
      ctx.r3.u64 = 1;
      sub_82B06D90(ctx, base);
      in = ctx.r3.u32 != 0;
      if (in)
      {
        break;
      }
      Sleep(20);
    }
    ctx = saved;
    Log(in ? "player 2 is signed in" : "player 2 is not signed in after 600 ms");
  }

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }

  bool InShell(const uint8_t *base)
  {
    constexpr uint32_t kWorld = 0x831F2CC8; // GWorld; +40 its package
    uint32_t world = LoadU32(base, kWorld);
    uint32_t package = world ? LoadU32(base, world + 40) : 0;
    return package && ao2::ue3::ObjectName(base, package) == "Shell";
  }

  // The UFunctions, once their package is loaded.
  void FindFunctions(const uint8_t *base)
  {
    static int wait = 0;
    if (wait-- > 0)
    {
      return;
    }
    wait = 300;
    constexpr uint32_t kOuter = 40, kClass = 52;
    const uint32_t function_class = ao2::ue3::FindClass(base, "Function");
    if (!function_class)
    {
      return;
    }
    const uint32_t count = ao2::ue3::ObjectCount(base);
    for (uint32_t i = 0; i < count; ++i)
    {
      uint32_t object = ao2::ue3::ObjectAt(base, i);
      if (!object || LoadU32(base, object + kClass) != function_class)
      {
        continue;
      }
      const std::string name = ao2::ue3::ObjectName(base, object);
      if (name == "CauseEvent" && ao2::ue3::ObjectName(base, LoadU32(base, object + kOuter)) == "PlayerController")
      {
        ao2::split_join::g_cause_event = object;
        Log("found PlayerController.CauseEvent");
      }
      else if (name == "GetLoginStatus" &&
               ao2::ue3::ObjectName(base, LoadU32(base, object + kOuter)) == "UIAction_GetLoggedInPlayerCount")
      {
        ao2::split_join::g_login_status = object;
        Log("found UIAction_GetLoggedInPlayerCount.GetLoginStatus");
      }
    }
  }

  // Whether a UI action is in the sequence of the widget `widget` (Action <
  // UIStateSequence < UIState < widget).
  bool InWidget(const uint8_t *base, uint32_t object, const char *widget)
  {
    constexpr uint32_t kOuter = 40;
    for (int i = 0; i < 6 && object; ++i)
    {
      object = LoadU32(base, object + kOuter);
      if (object && ao2::ue3::ObjectName(base, object) == widget)
      {
        return true;
      }
    }
    return false;
  }

  // The event's name, the function's only parameter: an FName at the start of
  // the FFrame's Locals (+16 after Node, Object and Code; +20 is tried too: the
  // layout is read from ProcessInternal, not from a source).
  std::string EventName(const uint8_t *base, uint32_t frame)
  {
    std::string first;
    for (uint32_t off : {16u, 20u})
    {
      uint32_t locals = LoadU32(base, frame + off);
      if (locals < 0x40000000 || locals >= 0xA0000000)
      {
        continue;
      }
      std::string name = ao2::ue3::Name(base, LoadU32(base, locals));
      if (name.rfind("MissionSetup_", 0) == 0)
      {
        return name;
      }
      if (first.empty())
      {
        first = std::move(name);
      }
    }
    return first;
  }
} // namespace

namespace ao2::split_join
{
  uint32_t g_cause_event = 0;
  uint32_t g_login_status = 0;

  std::string CauseEventName(const uint8_t *base, uint32_t frame) { return EventName(base, frame); }

  // Game thread, every world step (UGameEngine::Tick, decouple.cpp).
  void Frame(uint8_t *base)
  {
    if (!g_cause_event || !g_login_status)
    {
      FindFunctions(base);
    }
    if (!g_active.load(std::memory_order_relaxed))
    {
      return;
    }
    if (!InShell(base))
    {
      g_left_shell = true;
    }
    else if (g_left_shell)
    {
      DevicesBack("devices back (main menu after a level)");
    }
  }

  void OnLoginStatus(PPCContext &ctx, uint8_t *base)
  {
    if (!InWidget(base, ctx.r3.u32, "Splitscreen") || g_window_open.exchange(true))
    {
      return;
    }
    Log("Splitscreen: the window opens");
    auto window = std::make_shared<JoinWindow>();
    ao2::menu::OpenWindow("split_join", [window]() { return window->Draw(); }, /*own_escape=*/true);
    PPCContext saved = ctx;
    ctx.r3.u64 = 1;
    sub_82B06D90(ctx, base);
    const bool signed_in = ctx.r3.u32 != 0;
    ctx = saved;
    if (!signed_in)
    {
      SplitPlayers(1, 1, 0, 0);
      g_left_shell = false;
      g_active = true;
      WaitForPlayer2(ctx, base);
    }
  }

  void OpenForGuest(void (*on_cancel)())
  {
    if (g_window_open.exchange(true))
    {
      return;
    }
    Log("Guest Sign-In: the window opens");
    g_guest_close = false;
    auto window = std::make_shared<JoinWindow>(on_cancel);
    ao2::menu::OpenWindow("split_join", [window]() { return window->Draw(); }, /*own_escape=*/true);
  }

  void CloseGuest()
  {
    if (g_guest_devices.exchange(false))
    {
      SplitPlayers(0, 0, 0, 0);
      Log("devices back (out of the versus lobby)");
    }
    else if (g_window_open.load())
    {
      g_guest_close = true;
    }
  }

  bool OnCauseEvent(PPCContext &ctx, uint8_t *base)
  {
    const std::string event = EventName(base, ctx.r4.u32);
    char what[128];
    std::snprintf(what, sizeof(what), "CauseEvent %s", event.c_str());
    Log(what);
    // Back to the main menu from Multiplayer or Campaign Mode: no network
    // session is left. EA's game manager starts listening as soon as the
    // Multiplayer menu connects (coop.h network_session) and, with no game
    // made, nothing ended it: cheats and checkpoints stayed "network only".
    if ((event == "Multiplayer_BackSelected" || event == "MissionSetup_BackSelected") &&
        ao2::coop::network_session.exchange(false))
    {
      Log("network session over (back to the main menu)");
    }
    constexpr char kPrefix[] = "MissionSetup_";
    if (event.rfind(kPrefix, 0) != 0)
    {
      return false;
    }
    const std::string button = event.substr(sizeof(kPrefix) - 1);
    if (button == "SplitscreenPS3")
    {
      return false;
    }
    if (button != "Splitscreen")
    {
      DevicesBack("devices back (another Campaign Mode button)");
      return false;
    }
    // The button's event comes once per PlayerController: the window's own
    // command goes through, like the others right after it.
    auto now = std::chrono::steady_clock::now();
    if (now < g_pass_until)
    {
      return false;
    }
    if (g_resume.exchange(false))
    {
      WaitForPlayer2(ctx, base);
      g_pass_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
      return false;
    }
    // The window opened at the count; the sequence stops here until it is done.
    return g_window_open.load(std::memory_order_relaxed);
  }
} // namespace ao2::split_join
