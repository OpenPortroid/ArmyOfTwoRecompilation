// input_script.cpp - Scripted gamepad input for automated testing.
//
// The game polls the pad through the XDK's XInputGetState (sub_82B07AF0,
// r3 = user, r4 = XINPUT_STATE*). Buttons from a script are merged into its
// result, so tests can drive the menus without the window having focus. The
// mouse's right-stick fallback (mouse_aim.cpp) is merged here too, and the
// left trigger becomes a switch with ao2_toggle_aim (toggle aim).
//
// Script: space-separated steps, from --ao2_input_script=... at startup or
// from the file input.script in the working directory (read and deleted when
// it appears). Steps: a b x y start back up down left right lb rb ls rs lt rt
// (tap for 150 ms, then 150 ms released), the same names with + or - to hold
// or release until further notice (lt+ ... lt-), waitNNN (milliseconds),
// mxN / myN (move the mouse by N counts, as if the mouse moved), skip (the
// cutscene skip key, cutscene_skip.cpp), lxN / lyN / rxN / ryN (stick axis
// held at N, -32768..32767, until set again; lx0 releases).
#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "autopilot.h"
#include "cutscene_skip.h"
#include "graphics_menu.h"
#include "menu_mouse.h"
#include "mouse_aim.h"

REXCVAR_DEFINE_BOOL(ao2_vehicle_wasd, true, "Input",
                    "Driving a vehicle: the left stick forward / back (W / S) also presses the right / left "
                    "trigger - the game's gas and brake");
REXCVAR_DEFINE_BOOL(ao2_toggle_aim, false, "Input",
                    "Aim (LT, the right mouse button) toggles: one press starts aiming, the next ends it, instead "
                    "of holding the button. On foot, on turrets and at a vehicle's gun; at the wheel, in the menus "
                    "and cutscenes the trigger is left as it is");
REXCVAR_DEFINE_STRING(ao2_input_script, "", "Debug",
                      "Scripted pad input (also from input.script): a b x y start back up down left right lb rb ls "
                      "rs lt rt [+|-] waitNNN mxN myN skip lxN lyN rxN ryN");

namespace
{
  using clock = std::chrono::steady_clock;

  // Inputs: XINPUT buttons in the low 16 bits, then the triggers.
  constexpr uint32_t kLeftTrigger = 1u << 16;
  constexpr uint32_t kRightTrigger = 1u << 17;

  struct Step
  {
    enum Kind : uint8_t
    {
      kHold,    // inputs held for ms (0 inputs = wait)
      kLatch,   // inputs held from now on
      kRelease, // latched inputs released
      kMouse,   // mouse moved by dx, dy
      kSkip,    // the cutscene skip key
      kStick,   // stick axis (inputs = 0..3: lx ly rx ry) held at ms (as int16)
    } kind;
    uint32_t inputs;
    uint32_t ms;
    float dx = 0.0f;
    float dy = 0.0f;
  };

  std::mutex g_mutex;
  std::deque<Step> g_steps;
  bool g_loaded_cvar = false;
  clock::time_point g_step_end;
  bool g_step_active = false;
  uint32_t g_latched = 0;
  int16_t g_sticks[4] = {}; // lx ly rx ry
  uint32_t g_last_inputs = 0;
  uint32_t g_packet = 0x10000;
  clock::time_point g_next_file_check;

  // Toggle aim, per pad: the trigger held at the last poll, aiming latched,
  // the pawn it is latched on (the character, a turret, a vehicle seat), and
  // fire (the right trigger) held with a grenade while aiming.
  struct ToggleAim
  {
    bool down = false;
    bool on = false;
    uint32_t pawn = 0;
    bool throwing = false;
  };
  ToggleAim g_toggle_aim[4];

  // The left trigger of a pad state `p` (XINPUT_STATE) as a switch: each press
  // flips aiming (the zoom on a turret), the trigger reads fully pulled while
  // it is on. At the wheel, in the menus and cutscenes the trigger is left
  // alone; there and on another pawn (on foot -> a turret) aiming starts over
  // unlatched, and a trigger held across that change counts as a press only
  // once released and pressed again. Before the scripted and autopilot input
  // is merged, so those still hold it as they please.
  void ApplyToggleAim(PPCContext &ctx, uint8_t *base, uint32_t user, uint8_t *p)
  {
    constexpr uint8_t kPressed = 30; // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
    ToggleAim &t = g_toggle_aim[user];
    bool down = p[6] >= kPressed;
    bool press = down && !t.down;
    t.down = down;
    uint32_t pawn = ao2::mouse::AimTogglePawn(ctx, base, user);
    if (pawn != t.pawn)
    {
      t.on = false;
      t.throwing = false;
      t.pawn = pawn;
    }
    if (!pawn)
    {
      ao2::mouse::NoteToggleAim(user, p[6], false);
      return;
    }
    if (press)
    {
      t.on = !t.on;
    }
    // A grenade thrown (fire pressed and let go while aiming with it) ends
    // the aiming.
    bool fire = p[7] >= kPressed;
    if (t.on && fire && !t.throwing && ao2::mouse::HoldsGrenade(base, pawn))
    {
      t.throwing = true;
    }
    else if (t.throwing && !fire)
    {
      t.throwing = false;
      t.on = false;
    }
    if (!t.on)
    {
      t.throwing = false;
    }
    ao2::mouse::NoteToggleAim(user, p[6], t.on);
    p[6] = t.on ? 0xFF : 0;
  }

  uint32_t InputMask(const std::string &name)
  {
    static const struct
    {
      const char *name;
      uint32_t mask;
    } kInputs[] = {{"up", 0x0001},   {"down", 0x0002}, {"left", 0x0004},      {"right", 0x0008},
                   {"start", 0x0010}, {"back", 0x0020}, {"ls", 0x0040},        {"rs", 0x0080},
                   {"lb", 0x0100},   {"rb", 0x0200},   {"a", 0x1000},         {"b", 0x2000},
                   {"x", 0x4000},    {"y", 0x8000},    {"lt", kLeftTrigger}, {"rt", kRightTrigger}};
    for (const auto &b : kInputs)
    {
      if (name == b.name)
      {
        return b.mask;
      }
    }
    return 0;
  }

  void Parse(const std::string &script)
  {
    std::istringstream in(script);
    std::string token;
    while (in >> token)
    try
    {
      if (token.rfind("wait", 0) == 0)
      {
        g_steps.push_back({Step::kHold, 0, uint32_t(std::stoul(token.substr(4)))});
      }
      else if (token.size() > 2 && (token.rfind("mx", 0) == 0 || token.rfind("my", 0) == 0))
      {
        float n = std::stof(token.substr(2));
        g_steps.push_back({Step::kMouse, 0, 0, token[1] == 'x' ? n : 0.0f, token[1] == 'y' ? n : 0.0f});
      }
      else if (token.size() > 2 && (token[0] == 'l' || token[0] == 'r') && (token[1] == 'x' || token[1] == 'y') &&
               (token[2] == '-' || (token[2] >= '0' && token[2] <= '9')))
      {
        uint32_t axis = (token[0] == 'r' ? 2u : 0u) + (token[1] == 'y' ? 1u : 0u);
        int v = std::clamp(std::stoi(token.substr(2)), -32768, 32767);
        g_steps.push_back({Step::kStick, axis, uint32_t(int32_t(v))});
      }
      else if (token == "skip")
      {
        g_steps.push_back({Step::kSkip, 0, 0});
      }
      else if (char last = token.back(); (last == '+' || last == '-') && InputMask(token.substr(0, token.size() - 1)))
      {
        g_steps.push_back({last == '+' ? Step::kLatch : Step::kRelease, InputMask(token.substr(0, token.size() - 1)), 0});
      }
      else if (uint32_t mask = InputMask(token))
      {
        g_steps.push_back({Step::kHold, mask, 150});
        g_steps.push_back({Step::kHold, 0, 150});
      }
    }
    catch (const std::exception &)
    {
      // A malformed number ("ly-", "wait"): skip the step.
    }
  }

  void PollSources(clock::time_point now)
  {
    if (!g_loaded_cvar)
    {
      g_loaded_cvar = true;
      Parse(REXCVAR_GET(ao2_input_script));
    }
    if (now < g_next_file_check)
    {
      return;
    }
    g_next_file_check = now + std::chrono::milliseconds(500);
    std::error_code ec;
    if (!std::filesystem::exists("input.script", ec))
    {
      return;
    }
    std::ifstream f("input.script");
    std::stringstream text;
    text << f.rdbuf();
    f.close();
    std::filesystem::remove("input.script", ec);
    Parse(text.str());
  }

  // Inputs the script holds right now; false when no script is running.
  bool CurrentInputs(uint32_t &inputs)
  {
    std::lock_guard lock(g_mutex);
    auto now = clock::now();
    PollSources(now);
    uint32_t held = 0;
    while (!g_steps.empty())
    {
      Step &step = g_steps.front();
      if (step.kind != Step::kHold)
      {
        if (step.kind == Step::kLatch)
        {
          g_latched |= step.inputs;
        }
        else if (step.kind == Step::kRelease)
        {
          g_latched &= ~step.inputs;
        }
        else if (step.kind == Step::kStick)
        {
          g_sticks[step.inputs & 3] = int16_t(int32_t(step.ms));
          ++g_packet;
        }
        else if (step.kind == Step::kSkip)
        {
          ao2::skip::Request();
        }
        else
        {
          ao2::mouse::InjectDelta(step.dx, step.dy);
        }
        g_steps.pop_front();
        continue;
      }
      if (!g_step_active)
      {
        g_step_active = true;
        g_step_end = now + std::chrono::milliseconds(step.ms);
      }
      if (now < g_step_end)
      {
        held = step.inputs;
        break;
      }
      g_steps.pop_front();
      g_step_active = false;
    }
    bool sticks = g_sticks[0] || g_sticks[1] || g_sticks[2] || g_sticks[3];
    if (g_steps.empty() && !g_latched && !sticks)
    {
      g_last_inputs = 0;
      return false;
    }
    inputs = held | g_latched;
    if (inputs != g_last_inputs)
    {
      g_last_inputs = inputs;
      ++g_packet;
    }
    return true;
  }

  void Store16(uint8_t *p, uint16_t v)
  {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
  }
  void Store32(uint8_t *p, uint32_t v)
  {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
  }
  void AddThumb(uint8_t *p, int32_t delta)
  {
    int32_t v = int16_t((p[0] << 8) | p[1]) + delta;
    Store16(p, uint16_t(int16_t(std::clamp(v, -32768, 32767))));
  }

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    const uint8_t *p = base + addr;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  }

  constexpr uint32_t kObjects = 0x831E4E3C; // GObjObjects (data, count)
  constexpr uint32_t kNames = 0x831D1724;   // FName::Names (data): entry +16 = chars

  std::string NameOf(const uint8_t *base, uint32_t object)
  {
    uint32_t names = LoadU32(base, kNames);
    uint32_t entry = names ? LoadU32(base, names + LoadU32(base, object + 44) * 4) : 0;
    if (!entry)
    {
      return {};
    }
    const char *s = reinterpret_cast<const char *>(base + entry + 16);
    return std::string(s, strnlen(s, 96));
  }

  // The "Press START" screen is the top UI scene: the UI scene client's
  // ActiveScenes (TArray at +0xC0, count +0xC4), as tools/menustate.py
  // reads it. The client is found by its class name (...SceneClient).
  bool PressStartOnTop(const uint8_t *base)
  {
    static uint32_t client = 0;
    static clock::time_point next_search;
    auto is_client = [&](uint32_t o) {
      std::string c = NameOf(base, LoadU32(base, o + 52));
      return c.size() > 11 && c.compare(c.size() - 11, 11, "SceneClient") == 0 &&
             NameOf(base, o).rfind("Default__", 0) != 0;
    };
    if (client && !is_client(client))
    {
      client = 0;
    }
    auto now = clock::now();
    if (!client && now >= next_search)
    {
      next_search = now + std::chrono::seconds(2);
      uint32_t objects = LoadU32(base, kObjects), count = LoadU32(base, kObjects + 4);
      for (uint32_t i = 0; objects && i < count && !client; ++i)
      {
        uint32_t o = LoadU32(base, objects + i * 4);
        if (o && is_client(o))
        {
          client = o;
        }
      }
    }
    uint32_t scenes = client ? LoadU32(base, client + 0xC0) : 0;
    uint32_t count = client ? LoadU32(base, client + 0xC4) : 0;
    if (!scenes || count == 0 || count > 64)
    {
      return false;
    }
    uint32_t top = LoadU32(base, scenes + (count - 1) * 4);
    return top && NameOf(base, top) == "StartMenuUIScene";
  }
} // namespace

// DWORD XInputGetState(DWORD user, XINPUT_STATE *state)
extern "C" REX_FUNC(__imp__sub_82B07AF0);
extern "C" REX_FUNC(sub_82B07AF0)
{
  uint32_t user = ctx.r3.u32;
  uint32_t state = ctx.r4.u32;
  __imp__sub_82B07AF0(ctx, base);
  user &= 0xFF;
  // Scripted input and the autopilot drive player 1; the mouse (the right
  // stick where it does not aim) and W / S at the wheel go to the player with
  // the keyboard - another one in split-screen.
  bool keyboard = user == ao2::mouse::KeyboardUser();
  if (state && (user == 0 || keyboard))
  {
    ao2::menumouse::OnPadPoll(ctx, base, state, keyboard);
  }
  // The Controls page waiting for a key takes every player's input.
  if (state && ao2::gfxmenu::OnPadPoll(ctx, base, user, state))
  {
    return;
  }
  if (state && user < 4 && ctx.r3.u32 == 0 && REXCVAR_GET(ao2_toggle_aim))
  {
    ApplyToggleAim(ctx, base, user, base + state);
  }
  if ((user != 0 && !keyboard) || !state)
  {
    return;
  }
  uint32_t inputs = 0;
  bool scripted = user == 0 && CurrentInputs(inputs);
  int32_t rx = 0, ry = 0;
  bool mouse = keyboard && ctx.r3.u32 == 0 && ao2::mouse::TakeStickFallback(rx, ry);
  ao2::autopilot::Pad pilot{};
  bool autopilot = user == 0 && ao2::autopilot::CurrentPad(pilot);
  // XINPUT_STATE: dwPacketNumber, wButtons, triggers, thumbsticks (big-endian).
  uint8_t *p = base + state;
  // At the wheel the game takes the gas and the brake from the triggers
  // (the mouse buttons); the left stick forward / back - W / S - presses
  // them too.
  if (keyboard && ctx.r3.u32 == 0 && REXCVAR_GET(ao2_vehicle_wasd) && ao2::mouse::Driving())
  {
    int32_t ly = int16_t((p[10] << 8) | p[11]);
    constexpr int32_t kDeadZone = 8000;
    uint8_t push = uint8_t(std::min((std::abs(ly) - kDeadZone) * 255 / (32767 - kDeadZone), 255));
    if (ly > kDeadZone)
    {
      p[7] = std::max(p[7], push);
    }
    else if (ly < -kDeadZone)
    {
      p[6] = std::max(p[6], push);
    }
  }
  // "Press START" takes only Start (Esc); A there (Enter, E, the pad's A)
  // presses it too. Nothing else listens to A on that screen.
  constexpr uint16_t kButtonA = 0x1000, kButtonStart = 0x0010;
  if (user == 0 && ctx.r3.u32 == 0 && (p[4] << 8 | p[5]) & kButtonA && PressStartOnTop(base))
  {
    Store16(p + 4, uint16_t((p[4] << 8 | p[5]) | kButtonStart));
  }
  if (!scripted && !mouse && !autopilot)
  {
    return;
  }
  if (ctx.r3.u32 != 0)
  {
    // No real pad: report a connected, idle one.
    std::memset(p, 0, 16);
  }
  uint16_t real = uint16_t((p[4] << 8) | p[5]);
  if (autopilot)
  {
    inputs |= pilot.buttons;
  }
  Store32(p, g_packet + pilot.packet + (ctx.r3.u32 == 0 ? (p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]) : 0));
  Store16(p + 4, uint16_t(real | inputs));
  if (inputs & kLeftTrigger)
  {
    p[6] = 0xFF;
  }
  if (inputs & kRightTrigger)
  {
    p[7] = 0xFF;
  }
  AddThumb(p + 12, rx);
  AddThumb(p + 14, ry);
  if (autopilot)
  {
    p[6] = std::max(p[6], pilot.lt);
    p[7] = std::max(p[7], pilot.rt);
    AddThumb(p + 8, pilot.lx);
    AddThumb(p + 10, pilot.ly);
    AddThumb(p + 12, pilot.rx);
    AddThumb(p + 14, pilot.ry);
  }
  if (scripted)
  {
    std::lock_guard lock(g_mutex);
    for (uint32_t i = 0; i < 4; ++i)
    {
      AddThumb(p + 8 + i * 2, g_sticks[i]);
    }
  }
  ctx.r3.u64 = 0;
}
