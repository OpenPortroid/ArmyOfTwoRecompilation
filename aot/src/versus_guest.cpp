// versus_guest.cpp - Split-screen in versus: the second player of a PC joins
// a versus lobby as a guest (ao2_versus_guest), as on the console.
//
// The lobby's Y (Guest Sign-In) shows the guest note, then the Plasma
// controller's ShowGuestLoginUI (sub_82A209D0; the lobby's own
// sub_82A055C8 alike) opens the sign-in UI: XamShowSigninUI(1, 2). The
// runtime answers at once with XN_SYS_SIGNINCHANGED, and the game's
// sign-in handler (sub_82AF4B28) goes over the users 0-3: a signed-in user
// whose XUserGetSigninInfo (sub_82B07E10, r3 = user, r5 = the info) has
// XUSER_INFO_FLAG_GUEST (2, info +8) goes to the lobby's guest handler
// (sub_82A2F190: r4 = the user, r5 = its sign-in state), which adds it to
// this machine's lobby player (NetworkPlayerInfo.WithGuest,
// SimGuestControllerId) - "Game Full" when there is no place, "no guests in
// ranked matches" in a ranked one. The runtime signs users 1-3 in while a
// controller is theirs (input_player2, user_gamertag_2) but never as
// guests: here a user signed in when the guest sign-in UI opens becomes a
// guest, until its controller goes (its sign-in state 0) or this PC has been
// out of every lobby and match for 2 s (below).
//
// On the console the guest signs in with a controller of its own. Here the
// automatic device assignment gives the keyboard and the first gamepad to
// player 1, so with one gamepad nobody else has a controller: the guest
// sign-in then hands that pad to player 2 (rexruntime's
// rex_input_pad_guest; the keyboard and mouse stay player 1's). The runtime
// signs user 2 in a moment later (it looks at the devices every 250 ms) and
// sends XN_SYS_SIGNINCHANGED again: the user is a guest from then on. The
// pad goes back to player 1 once this PC has been out of every lobby and
// match (the Plasma lobby's +2793 / +2794, which the guest handler wants
// set) for 2 s.
//
// The match puts the local players in the order of their places and
// numbers them so, and they stay so after the match: a guest on a place
// before the host's made the guest's LocalPlayer GamePlayers[0] (the menus
// and going online follow it: the keyboard and mouse did nothing there and
// the next versus lobby was "connection lost"), and in the campaign's
// split-screen player 2 played another LocalPlayer than its own (Select
// Mission / New Game did nothing). At the same 2 s out of the lobby,
// GamePlayers gets the order and the ControllerIds it had at the guest
// sign-in back. The main menu's screen was laid out before that, for the
// match's numbering: until the next level GamePlayers[0] takes the part of
// the screen another player has (FixMenuLayout), or its scenes stay
// invisible.
#include "log_files.h"
#include "ue3_reflect.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(ao2_versus_guest, true, "Online",
                    "Versus split-screen: Guest Sign-In (Y in the lobby) takes the second local player into the "
                    "match as the host player's guest - its own controller (input_player2), or the only gamepad "
                    "while the keyboard and mouse stay player 1's");

namespace
{
  using clock = std::chrono::steady_clock;

  constexpr uint32_t kGuestFlag = 2;      // XUSER_INFO_FLAG_GUEST
  constexpr uint32_t kLobby = 0x831F3024; // the Plasma lobby singleton
  constexpr uint32_t kInLobby = 2793;     // bytes +2793 / +2794: the guest handler's condition

  std::atomic<uint32_t> g_guests{0};  // users 1-3 signed in as guests
  std::atomic<uint32_t> g_pending{0}; // users that become guests when they sign in
  std::atomic<bool> g_pad_lent{false};
  // Game thread: GamePlayers and their ControllerIds at the guest sign-in
  // (count 0: nothing to put back).
  struct SavedPlayers
  {
    uint32_t count = 0;
    uint32_t player[4] = {};
    uint32_t controller_id[4] = {};
  } g_saved;
  clock::time_point g_out_since;
  bool g_out = false;
  // Game thread: GamePlayers was put back, and the main menu's screen may
  // still be the match's (FixMenuLayout); cleared in the next level.
  bool g_fix_layout = false;
  bool g_seen_shell = false;

  void Log(const char *what, uint32_t user, uint32_t guests)
  {
    static FILE *log = ao2::OpenLogFile("versus_guest.log");
    REXLOG_INFO("versus_guest: user {} {} (guests {:X})", user, what, guests);
    if (log)
    {
      std::fprintf(log, "user %u %s (guests %X)\n", user, what, guests);
      std::fflush(log);
    }
  }

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }

  // rexruntime's rex_input_pad_guest (1 when one gamepad is shared the
  // automatic way); 0 from a runtime without it.
  uint32_t PadGuest(uint32_t on)
  {
    using Fn = uint32_t (*)(uint32_t);
    static Fn fn = []() -> Fn {
      HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
      return runtime ? reinterpret_cast<Fn>(GetProcAddress(runtime, "rex_input_pad_guest")) : nullptr;
    }();
    return fn ? fn(on) : 0;
  }

  uint32_t Gamepads()
  {
    using Fn = uint32_t (*)();
    static Fn fn = []() -> Fn {
      HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
      return runtime ? reinterpret_cast<Fn>(GetProcAddress(runtime, "rex_input_gamepads")) : nullptr;
    }();
    return fn ? fn() : 0;
  }

  // The runtime's devices, by name (for the log).
  std::string Devices()
  {
    using Fn = uint32_t (*)(char *, uint32_t);
    static Fn fn = []() -> Fn {
      HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
      return runtime ? reinterpret_cast<Fn>(GetProcAddress(runtime, "rex_input_devices")) : nullptr;
    }();
    using Counts = uint32_t (*)();
    static Counts counts = []() -> Counts {
      HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
      return runtime ? reinterpret_cast<Counts>(GetProcAddress(runtime, "rex_input_sdl_counts")) : nullptr;
    }();
    char names[512] = {};
    if (fn)
    {
      fn(names, sizeof(names));
    }
    using Error = uint32_t (*)(char *, uint32_t);
    static Error error = []() -> Error {
      HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
      return runtime ? reinterpret_cast<Error>(GetProcAddress(runtime, "rex_input_sdl_error")) : nullptr;
    }();
    char why[160] = {};
    if (error)
    {
      error(why, sizeof(why));
    }
    uint32_t sdl = counts ? counts() : 0;
    char tail[256];
    std::snprintf(tail, sizeof(tail), "; SDL: %s, %u joysticks, %u gamepads%s%s", sdl >> 31 ? "up" : "not up",
                  (sdl >> 16) & 0x7FFF, sdl & 0xFFFF, why[0] ? ", open failed: " : "", why);
    return std::string(names) + tail;
  }

  constexpr uint32_t kEngine = 0x831EFCBC; // GEngine

  // Engine.GamePlayers (TArray of LocalPlayer: data, count) and
  // LocalPlayer.ControllerId; false when not found.
  bool PlayersLayout(const uint8_t *base, uint32_t &array, uint32_t &controller_id)
  {
    static const int32_t players = ao2::ue3::PropertyOffset(base, "Engine", "GamePlayers");
    static const int32_t id = ao2::ue3::PropertyOffset(base, "LocalPlayer", "ControllerId");
    uint32_t engine = LoadU32(base, kEngine);
    if (players < 0 || id < 0 || !engine)
    {
      return false;
    }
    array = engine + uint32_t(players);
    controller_id = uint32_t(id);
    return true;
  }

  void SavePlayers(const uint8_t *base)
  {
    uint32_t array, id;
    if (!PlayersLayout(base, array, id))
    {
      return;
    }
    uint32_t data = LoadU32(base, array), count = LoadU32(base, array + 4);
    if (!data || !count || count > 4)
    {
      return;
    }
    g_saved.count = count;
    for (uint32_t i = 0; i < count; ++i)
    {
      g_saved.player[i] = LoadU32(base, data + i * 4);
      g_saved.controller_id[i] = g_saved.player[i] ? LoadU32(base, g_saved.player[i] + id) : 0;
    }
  }

  void StoreU32(uint8_t *base, uint32_t addr, uint32_t v)
  {
    v = std::byteswap(v);
    std::memcpy(base + addr, &v, 4);
  }

  // GamePlayers gets the players, in their order, and the ControllerIds it
  // had at the guest sign-in back (the same LocalPlayers still there).
  void RestorePlayers(uint8_t *base)
  {
    SavedPlayers saved = std::exchange(g_saved, SavedPlayers{});
    uint32_t array, id;
    if (!saved.count || !PlayersLayout(base, array, id))
    {
      return;
    }
    uint32_t data = LoadU32(base, array), count = LoadU32(base, array + 4);
    if (!data || count != saved.count)
    {
      Log("GamePlayers changed, not put back", 0, g_guests.load());
      return;
    }
    bool same = true, changed = false;
    for (uint32_t i = 0; i < count && same; ++i)
    {
      same = std::find(saved.player, saved.player + count, LoadU32(base, data + i * 4)) != saved.player + count;
    }
    if (!same)
    {
      Log("GamePlayers changed, not put back", 0, g_guests.load());
      return;
    }
    std::string what = "GamePlayers back:";
    for (uint32_t i = 0; i < count; ++i)
    {
      uint32_t player = saved.player[i];
      changed |= LoadU32(base, data + i * 4) != player || LoadU32(base, player + id) != saved.controller_id[i];
      char one[64];
      std::snprintf(one, sizeof(one), " [%u] %08X controller %u (was %u)", i, player, saved.controller_id[i],
                    LoadU32(base, player + id));
      what += one;
      StoreU32(base, data + i * 4, player);
      StoreU32(base, player + id, saved.controller_id[i]);
    }
    if (changed)
    {
      Log(what.c_str(), 0, g_guests.load());
      g_fix_layout = true;
      g_seen_shell = false;
    }
  }

  bool InShell(const uint8_t *base)
  {
    constexpr uint32_t kWorld = 0x831F2CC8; // GWorld; +40 its package
    uint32_t world = LoadU32(base, kWorld);
    uint32_t package = world ? LoadU32(base, world + 40) : 0;
    return package && ao2::ue3::ObjectName(base, package) == "Shell";
  }

  // The main menu's screen is laid out before GamePlayers is put back (the
  // menu level loads within the 2 s), for the match's numbering: the
  // player who had the host's id there (GamePlayers[1] with a guest on a
  // place before the host's) keeps the screen (LocalPlayer Origin 0,0) and
  // GamePlayers[0] - the keyboard's - is off it (Origin 1,0). Its scenes are
  // then neither drawn nor updated: in the campaign's split-screen Select
  // Mission / New Game opened a scene with nothing bound and nothing
  // focused, so nothing showed. Laying out again (bViewportLayoutDirty) gave
  // the same. Until the next level, GamePlayers[0] takes the part of the
  // screen another player has instead (they swap Origin and Size).
  void FixMenuLayout(uint8_t *base)
  {
    if (!InShell(base))
    {
      g_fix_layout = !g_seen_shell;
      return;
    }
    g_seen_shell = true;
    static const int32_t origin = ao2::ue3::PropertyOffset(base, "LocalPlayer", "Origin");
    static const int32_t size = ao2::ue3::PropertyOffset(base, "LocalPlayer", "Size");
    uint32_t array, id;
    if (origin < 0 || size < 0 || size != origin + 8 || !PlayersLayout(base, array, id))
    {
      return;
    }
    uint32_t data = LoadU32(base, array), count = LoadU32(base, array + 4);
    if (!data || count < 2 || count > 4)
    {
      return;
    }
    auto x = [&](uint32_t player) { return std::bit_cast<float>(LoadU32(base, player + uint32_t(origin))); };
    uint32_t first = LoadU32(base, data);
    if (!first || x(first) < 1.0f)
    {
      return;
    }
    for (uint32_t i = 1; i < count; ++i)
    {
      uint32_t other = LoadU32(base, data + i * 4);
      if (other && x(other) < 1.0f)
      {
        uint8_t a[16], b[16];
        std::memcpy(a, base + first + uint32_t(origin), 16);
        std::memcpy(b, base + other + uint32_t(origin), 16);
        std::memcpy(base + first + uint32_t(origin), b, 16);
        std::memcpy(base + other + uint32_t(origin), a, 16);
        char what[96];
        std::snprintf(what, sizeof(what), "main menu: GamePlayers[0] takes the screen of GamePlayers[%u]", i);
        Log(what, 0, g_guests.load());
        return;
      }
    }
  }

  // Once no lobby or match has been there for 2 s: the lent pad goes back
  // to player 1 and GamePlayers to its order and ControllerIds.
  void BackInMenus(uint8_t *base)
  {
    if (!g_pad_lent.load(std::memory_order_relaxed) && !g_saved.count && !g_guests.load(std::memory_order_relaxed))
    {
      return;
    }
    uint32_t lobby = LoadU32(base, kLobby);
    bool out = !lobby || (!base[lobby + kInLobby] && !base[lobby + kInLobby + 1]);
    auto now = clock::now();
    if (!out)
    {
      g_out = false;
      return;
    }
    if (!g_out)
    {
      g_out = true;
      g_out_since = now;
      return;
    }
    if (now - g_out_since < std::chrono::seconds(2))
    {
      return;
    }
    RestorePlayers(base);
    if (g_pad_lent.exchange(false))
    {
      PadGuest(0);
      g_pending.fetch_and(~2u);
      Log("gives the gamepad back to player 1 (out of the lobby)", 1, g_guests.load());
    }
    // With a controller of its own a guest stayed one for good, also in the
    // campaign's split-screen.
    if (uint32_t guests = g_guests.exchange(0))
    {
      g_pending.store(0);
      Log("and the others are no guests now (out of the lobby)", uint32_t(std::countr_zero(guests)), 0);
    }
  }
} // namespace

// XamUserGetSigninState (r3 = user): a pending guest that signed in is a
// guest, a guest whose controller went is no guest any more.
extern "C" REX_FUNC(__imp__sub_82B06D90);
extern "C" REX_FUNC(sub_82B06D90)
{
  uint32_t user = ctx.r3.u32;
  __imp__sub_82B06D90(ctx, base);
  if (user >= 4 || !user)
  {
    return;
  }
  uint32_t bit = 1u << user;
  if (ctx.r3.u32 && g_pending.load(std::memory_order_relaxed) & bit)
  {
    g_pending.fetch_and(~bit);
    Log("signed in as a guest", user, g_guests.fetch_or(bit) | bit);
  }
  else if (!ctx.r3.u32 && g_guests.load(std::memory_order_relaxed) & bit)
  {
    Log("signed out, no guest now", user, g_guests.fetch_and(~bit) & ~bit);
  }
}

// XUserGetSigninInfo (r3 = user, r4 = flags, r5 = XUSER_SIGNIN_INFO *): a
// guest says so.
extern "C" REX_FUNC(__imp__sub_82B07E10);
extern "C" REX_FUNC(sub_82B07E10)
{
  uint32_t user = ctx.r3.u32, info = ctx.r5.u32;
  __imp__sub_82B07E10(ctx, base);
  if (ctx.r3.u32 == 0 && info && user < 4 && g_guests.load(std::memory_order_relaxed) & (1u << user))
  {
    uint32_t flags;
    std::memcpy(&flags, base + info + 8, 4);
    flags = std::byteswap(std::byteswap(flags) | kGuestFlag);
    std::memcpy(base + info + 8, &flags, 4);
  }
}

namespace
{
  // The guest sign-in UI is opening: users 1-3 signed in now are guests;
  // with none, player 2 gets the only gamepad and is a guest once signed in.
  void GuestsSignIn(PPCContext &ctx, uint8_t *base)
  {
    if (!REXCVAR_GET(ao2_versus_guest))
    {
      return;
    }
    if (!g_saved.count)
    {
      SavePlayers(base);
    }
    g_out = false;
    PPCContext saved = ctx;
    bool any = false;
    for (uint32_t user = 1; user < 4; ++user)
    {
      ctx.r3.u64 = user;
      sub_82B06D90(ctx, base);
      if (!ctx.r3.u32)
      {
        continue;
      }
      any = true;
      if (!(g_guests.load(std::memory_order_relaxed) & (1u << user)))
      {
        Log("signed in as a guest", user, g_guests.fetch_or(1u << user) | (1u << user));
      }
    }
    ctx = saved;
    if (any)
    {
      return;
    }
    if (PadGuest(1))
    {
      g_pad_lent = true;
      g_pending.fetch_or(2u);
      Log("takes the only gamepad (the keyboard and mouse stay player 1's)", 1, g_guests.load());
    }
    else
    {
      PadGuest(0);
      char what[768];
      std::snprintf(what, sizeof(what),
                    "guest sign-in: no other player has a controller (%u gamepads; one is lent only with "
                    "input_player1 / input_player2 = auto; devices: %s)",
                    Gamepads(), Devices().c_str());
      Log(what, 0, g_guests.load());
    }
  }
} // namespace

namespace ao2::versus_guest
{
  uint32_t GuestCount()
  {
    return uint32_t(std::popcount(g_guests.load(std::memory_order_relaxed)));
  }

  // Game thread, every world step (UGameEngine::Tick, decouple.cpp).
  void Frame(uint8_t *base)
  {
    BackInMenus(base);
    if (g_fix_layout)
    {
      FixMenuLayout(base);
    }
  }
} // namespace ao2::versus_guest

// AO2PlasmaController::ShowGuestLoginUI.
extern "C" REX_FUNC(__imp__sub_82A209D0);
extern "C" REX_FUNC(sub_82A209D0)
{
  GuestsSignIn(ctx, base);
  __imp__sub_82A209D0(ctx, base);
}

// The lobby's guest sign-in UI (the same XamShowSigninUI(1, 2)).
extern "C" REX_FUNC(__imp__sub_82A055C8);
extern "C" REX_FUNC(sub_82A055C8)
{
  GuestsSignIn(ctx, base);
  __imp__sub_82A055C8(ctx, base);
}
