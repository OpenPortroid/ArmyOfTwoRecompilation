// cutscene_skip.cpp - Skip cutscenes and movies with Enter or Space.
//
// The game has two kinds of cutscenes, and the skip key ends whichever is
// playing:
//
// - Full-screen Bink movies (the logos at startup, level intros). The movie
//   player is a singleton at [0x831E6E64]; its input handler sub_82586F48
//   (r3 = player + 4, r6 = key FName, r7 = input event, 1 = released) skips
//   the current movie when it is marked skippable (player +96, set from
//   [FullScreenMovie] SkippableMovies; the startup logos are not in that
//   list) and the key fits: XboxTypeS keys at 0x831EFDC0 / 0x831F0C98 for
//   LVL0_Temp_Intro, the one at 0x831F11E0 with player +108 set for the
//   others. The skip itself goes to the render thread as
//   SkipGearsMovieCommand. The movie is marked skippable and the handler
//   is called with those keys.
//
// - In-engine cinematics (Kismet matinees, USeqAct_Interp). Unreal Engine 3
//   ends them with the console command CANCELMATINEE (ULocalPlayer::Exec,
//   sub_8286A720, called through the local player's FExec at +60): while the
//   player's controller is in cinematic mode (+808 & 0x20000) every playing
//   and skippable matinee (+264: 0x80000000 playing, 0x00200000 skippable)
//   with the player in one of its groups jumps to its end (SetPosition), and
//   the game info gets MatineeCancelled. The game never sends the command
//   itself and most of its matinees are not marked skippable, so playing
//   matinees are marked skippable for the duration of the command.
//
// Online co-op is a lockstep simulation (coop.h): a skip on one machine
// only desyncs the game (or leaves the partner in the movie until the
// connection drops), so there both machines skip together:
//
// - While the lockstep runs, the key goes into this player's frame record
//   as a press of F12 (a key the console game never sees). Both machines
//   replay the record on the same frame (sub_82A0EEC8) and skip there - the
//   matinee through that record's player, so the same on both.
// - While it stands still (the level intro movie: no frames, nothing in the
//   records) the skip goes to the partner over the side channel
//   (coop_link.cpp) and both end the movie.
#include "cutscene_skip.h"

#include "coop.h"
#include "coop_link.h"
#include "log_files.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DEFINE_BOOL(ao2_skip_cutscenes, true, "Input",
                    "Enter or Space skips the playing cutscene or movie (in online co-op for both players)");
REXCVAR_DEFINE_BOOL(ao2_skip_unskippable, true, "Input",
                    "The skip key also ends cutscenes and movies the game itself does not let skip "
                    "(startup logos, most in-engine cinematics)");

namespace
{
  using clock = std::chrono::steady_clock;

  // A press nobody acted on within this time is dropped (not in a cutscene).
  constexpr auto kRequestLifetime = std::chrono::milliseconds(500);

  // Movie player.
  constexpr uint32_t kMoviePlayer = 0x831E6E64;
  constexpr uint32_t kMovieInputKey = 0x82586F48;
  constexpr uint32_t kMovieKeyIntro = 0x831EFDC0; // LVL0_Temp_Intro
  constexpr uint32_t kMovieKeyOther = 0x831F11E0; // the other skippable movies
  constexpr uint32_t kXInputGetState = 0x82B07AF0;   // (user, XINPUT_STATE*)
  constexpr uint32_t kButtonA = 0x1000, kButtonStart = 0x0010;

  // Matinees.
  constexpr uint32_t kEngine = 0x831EFCBC;        // GEngine
  constexpr uint32_t kObjects = 0x831E4E3C;       // GObjObjects (data, count)
  constexpr uint32_t kInterpClass = 0x831F1620;   // cached USeqAct_Interp::StaticClass()
  constexpr uint32_t kLocalPlayerExec = 0x8286A720;
  constexpr uint32_t kCinematicModeBit = 0x00020000; // PlayerController +808
  constexpr uint32_t kInterpPlaying = 0x80000000;    // USeqAct_Interp +264
  constexpr uint32_t kInterpSkippable = 0x00200000;

  // Online co-op: the skip as a key in the lockstep frame record.
  constexpr uint32_t kFrameSync = 0x831F3008;      // frame sync object singleton
  constexpr uint32_t kSyncIsLocalPad = 0x82A1E8E8; // (sync, controller id) -> records this pad
  constexpr uint32_t kSyncRecordKey = 0x82A2D570;  // (sync, controller id, key, event, f1 amount)
  constexpr uint32_t kNameF12 = 0x831F0E3C;        // FName F12 (key table entry 111)
  constexpr uint32_t kKeyF12 = 111;
  constexpr uint32_t kPressed = 0; // IE_Pressed
  // The lockstep counts as running while its frame moved this recently.
  constexpr auto kLockstepIdle = std::chrono::milliseconds(250);

  std::atomic<int64_t> g_request{0}; // clock ticks of the last press, 0 = none
  rex::ui::Window *g_window = nullptr;

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("cutscene_skip.log");
    return f;
  }

  template <typename... Args>
  void Logf(const char *format, Args... args)
  {
    if (FILE *f = Log())
    {
      std::fprintf(f, format, args...);
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
  uint64_t LoadName(const uint8_t *base, uint32_t addr)
  {
    return uint64_t(LoadU32(base, addr)) << 32 | LoadU32(base, addr + 4);
  }

  // Calls a guest function (r3..r7) on this thread; the context is restored
  // afterwards. The callee gets its own stack below `stack_top`.
  uint32_t CallGuest(PPCContext &ctx, uint8_t *base, uint32_t fn, uint32_t stack_top, uint64_t r3,
                     uint64_t r4 = 0, uint64_t r5 = 0, uint64_t r6 = 0, uint64_t r7 = 0)
  {
    PPCContext saved = ctx;
    ctx.r1.u64 = stack_top;
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

  // Below the caller's frame and its red zone; scratch memory goes between
  // this and the caller's stack pointer.
  uint32_t ScratchStackTop(const PPCContext &ctx)
  {
    return (ctx.r1.u32 - 1024) & ~15u;
  }

  class KeyListener : public rex::ui::WindowInputListener
  {
  public:
    void OnKeyDown(rex::ui::KeyEvent &e) override
    {
      auto key = e.virtual_key();
      if ((key == rex::ui::VirtualKey::kReturn || key == rex::ui::VirtualKey::kSpace) && !e.prev_state() &&
          g_window && g_window->HasFocus())
      {
        ao2::skip::Request();
      }
    }
  };

  KeyListener g_listener;

  // Skips the playing full-screen movie. False when none is playing.
  // `force`: also movies the game does not let skip.
  bool SkipMovie(PPCContext &ctx, uint8_t *base, bool force)
  {
    uint32_t player = LoadU32(base, kMoviePlayer);
    if (!player || !LoadU32(base, player + 88))
    {
      return false;
    }
    bool skippable = LoadU32(base, player + 96) != 0;
    // A movie outside the playlist (index -1) is the game's own wait (the
    // LOADING MAP screen, also seen between co-op levels): skipping it in
    // co-op left both games stuck with no input and no pause menu, so there
    // only the game may end it. In a single-player game it is skipped (the
    // test tool does it to get to the level quickly).
    if (!skippable && int32_t(LoadU32(base, player + 124)) < 0)
    {
      if (ao2::coop::InNetworkGame(base))
      {
        Logf("movie: not skippable (index -1)\n");
        return true;
      }
      force = true;
    }
    if (!skippable && !force)
    {
      Logf("movie: not skippable\n");
      return true;
    }
    // Read by the render thread when the skip command runs: stays set.
    StoreU32(base, player + 96, 1);
    uint32_t top = ScratchStackTop(ctx);
    uint32_t handled = CallGuest(ctx, base, kMovieInputKey, top, player + 4, 0, 0, LoadName(base, kMovieKeyIntro), 1);
    if (!handled)
    {
      uint32_t allow = LoadU32(base, player + 108);
      StoreU32(base, player + 108, 1);
      handled = CallGuest(ctx, base, kMovieInputKey, top, player + 4, 0, 0, LoadName(base, kMovieKeyOther), 1);
      StoreU32(base, player + 108, allow);
    }
    Logf("movie: player=%08X skippable=%d index=%d/%d -> %s\n", player, skippable ? 1 : 0,
         int32_t(LoadU32(base, player + 124)), int32_t(LoadU32(base, player + 116)), handled ? "skipped" : "not playing");
    return handled != 0;
  }

  bool IsA(const uint8_t *base, uint32_t object, uint32_t cls)
  {
    // UObject::Class at +52, UStruct::SuperField at +60.
    uint32_t c = LoadU32(base, object + 52);
    for (int depth = 0; c && depth < 32; ++depth, c = LoadU32(base, c + 60))
    {
      if (c == cls)
      {
        return true;
      }
    }
    return false;
  }

  // Ends the playing in-engine cinematic, as the local player of
  // `controller_id` (0 = this machine's player). False when that player is
  // not watching one (not in cinematic mode).
  bool SkipMatinee(PPCContext &ctx, uint8_t *base, uint32_t controller_id = 0)
  {
    uint32_t top = ScratchStackTop(ctx);
    uint32_t engine = LoadU32(base, kEngine);
    uint32_t client = engine ? LoadU32(base, engine + 772) : 0;
    uint32_t vtable = client ? LoadU32(base, client) : 0;
    uint32_t local_player = vtable ? CallGuest(ctx, base, LoadU32(base, vtable + 268), top, client, controller_id) : 0;
    uint32_t controller = local_player ? LoadU32(base, local_player + 64) : 0;
    if (!controller)
    {
      Logf("matinee: no local player\n");
      return false;
    }
    if (!(LoadU32(base, controller + 808) & kCinematicModeBit))
    {
      Logf("matinee: controller %08X not in cinematic mode\n", controller);
      return false;
    }
    // The command goes through the local player's FExec (+60); make sure
    // that is the function this was written against.
    uint32_t exec_vtable = LoadU32(base, local_player + 60);
    bool exec_found = false;
    for (uint32_t i = 0; exec_vtable && i < 16 && !exec_found; ++i)
    {
      exec_found = LoadU32(base, exec_vtable + i * 4) == kLocalPlayerExec;
    }
    if (!exec_found)
    {
      Logf("matinee: local player %08X has no Exec %08X in its FExec vtable %08X\n", local_player, kLocalPlayerExec,
           exec_vtable);
      return false;
    }

    // Mark the playing matinees skippable for the command.
    std::vector<uint32_t> marked;
    uint32_t playing = 0;
    uint32_t cls = LoadU32(base, kInterpClass);
    uint32_t objects = LoadU32(base, kObjects);
    uint32_t count = LoadU32(base, kObjects + 4);
    for (uint32_t i = 0; cls && objects && i < count; ++i)
    {
      uint32_t object = LoadU32(base, objects + i * 4);
      if (!object || !IsA(base, object, cls))
      {
        continue;
      }
      uint32_t flags = LoadU32(base, object + 264);
      if (!(flags & kInterpPlaying))
      {
        continue;
      }
      ++playing;
      if (!(flags & kInterpSkippable) && REXCVAR_GET(ao2_skip_unskippable))
      {
        StoreU32(base, object + 264, flags | kInterpSkippable);
        marked.push_back(object);
      }
    }

    // L"CANCELMATINEE" (big-endian UTF-16) in scratch memory above the
    // callee's stack.
    static const char kCommand[] = "CANCELMATINEE";
    uint32_t text = top + 512;
    for (size_t i = 0; i < sizeof(kCommand); ++i)
    {
      base[text + i * 2] = 0;
      base[text + i * 2 + 1] = uint8_t(kCommand[i]);
    }
    uint32_t handled = CallGuest(ctx, base, kLocalPlayerExec, top, local_player + 60, text, 0);

    uint32_t still_playing = 0;
    for (uint32_t object : marked)
    {
      uint32_t flags = LoadU32(base, object + 264);
      StoreU32(base, object + 264, flags & ~kInterpSkippable);
    }
    for (uint32_t i = 0; cls && objects && i < count; ++i)
    {
      uint32_t object = LoadU32(base, objects + i * 4);
      if (object && IsA(base, object, cls) && (LoadU32(base, object + 264) & kInterpPlaying))
      {
        ++still_playing;
      }
    }
    Logf("matinee: controller=%08X class=%08X playing=%u (marked skippable %zu) exec=%u -> still playing %u\n",
         controller, cls, playing, marked.size(), handled, still_playing);
    return true;
  }

  bool MoviePlaying(const uint8_t *base)
  {
    uint32_t player = LoadU32(base, kMoviePlayer);
    return player && LoadU32(base, player + 88);
  }

  // Online co-op: the lockstep simulates frames (its counter moves). It
  // stands still while the level loads and its intro movie plays.
  bool LockstepRunning(const uint8_t *base)
  {
    static std::atomic<uint32_t> last_frame{0};
    static std::atomic<int64_t> last_change{0};
    if (!ao2::coop::OnlineLockstep(base))
    {
      return false;
    }
    uint32_t frame = ao2::coop::SyncFrame(base);
    int64_t now = clock::now().time_since_epoch().count();
    if (last_frame.exchange(frame, std::memory_order_relaxed) != frame)
    {
      last_change.store(now, std::memory_order_relaxed);
    }
    return clock::duration(now - last_change.load(std::memory_order_relaxed)) < kLockstepIdle;
  }

  // Online co-op: the skip goes into this player's lockstep frame record as
  // a press of F12 (as the gameplay viewport's InputKey, sub_8284BBE8, records
  // pad buttons); OnReplayedRecord acts on it on both machines.
  void RecordSkip(PPCContext &ctx, uint8_t *base)
  {
    uint32_t sync = LoadU32(base, kFrameSync);
    uint32_t top = ScratchStackTop(ctx);
    if (!sync || !CallGuest(ctx, base, kSyncIsLocalPad, top, sync, 0))
    {
      Logf("online co-op: the pad is not recorded now\n");
      return;
    }
    PPCContext saved = ctx;
    ctx.r1.u64 = top;
    ctx.r3.u64 = sync;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = LoadName(base, kNameF12);
    ctx.r6.u64 = kPressed;
    ctx.f1.f64 = 1.0;
    rex::runtime::ResolveIndirectFunction(kSyncRecordKey)(ctx, base);
    uint32_t recorded = ctx.r3.u32;
    ctx = saved;
    Logf("online co-op: skip recorded on frame %u -> %u\n", ao2::coop::SyncFrame(base), recorded);
  }
} // namespace

namespace ao2::skip
{
  void Attach(rex::ui::Window *window)
  {
    if (!window || g_window)
    {
      return;
    }
    g_window = window;
    // Next to the mouse listener (32): above the input drivers (0), below
    // the ImGui overlays (64).
    window->AddInputListener(&g_listener, 33);
  }

  void Request()
  {
    g_request.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
  }

  void Poll(PPCContext &ctx, uint8_t *base)
  {
    // skip.trigger (a file in the working directory, removed when taken): the same skip as Enter / Space, without
    // the game window having focus. tools/aot.py uses it. Checked here because this is polled by every thread that
    // runs while a movie plays, and also in a network game (the skip is synchronised below).
    static clock::time_point next_trigger_check{};
    if (clock::now() >= next_trigger_check)
    {
      next_trigger_check = clock::now() + std::chrono::milliseconds(200);
      if (std::remove("skip.trigger") == 0)
      {
        Request();
      }
    }
    bool network = ao2::coop::InNetworkGame(base);
    bool lockstep = network && LockstepRunning(base);
    if (network)
    {
      ao2::coop::link::Pump();
      // The partner skipped the intro movie: here too (no frames run, so
      // when does not matter).
      if (ao2::coop::link::TakeRemoteSkipMovie() && !lockstep && REXCVAR_GET(ao2_skip_cutscenes))
      {
        Logf("online co-op: partner skipped the movie\n");
        SkipMovie(ctx, base, REXCVAR_GET(ao2_skip_unskippable));
      }
    }
    int64_t request = g_request.exchange(0, std::memory_order_relaxed);
    if (!request || !REXCVAR_GET(ao2_skip_cutscenes))
    {
      return;
    }
    if (clock::now() - clock::time_point(clock::duration(request)) > kRequestLifetime)
    {
      return;
    }
    if (lockstep)
    {
      RecordSkip(ctx, base);
      return;
    }
    if (network)
    {
      if (MoviePlaying(base))
      {
        Logf("online co-op: skipping the movie for both\n");
        ao2::coop::link::SendSkipMovie();
        SkipMovie(ctx, base, REXCVAR_GET(ao2_skip_unskippable));
      }
      return;
    }
    if (!SkipMovie(ctx, base, REXCVAR_GET(ao2_skip_unskippable)))
    {
      SkipMatinee(ctx, base);
    }
  }

  void OnReplayedRecord(PPCContext &ctx, uint8_t *base, uint32_t record, uint32_t controller_id)
  {
    if (!REXCVAR_GET(ao2_skip_cutscenes) || !ao2::coop::OnlineLockstep(base))
    {
      return;
    }
    // Pressed keys: +64, 12 bytes each (key table index, event, amount),
    // count at +244.
    uint32_t count = std::min<uint32_t>(LoadU32(base, record + 244), 15);
    for (uint32_t i = 0; i < count; ++i)
    {
      uint32_t entry = record + 64 + i * 12;
      if (LoadU32(base, entry) == kKeyF12 && LoadU32(base, entry + 4) == kPressed)
      {
        Logf("online co-op: skip of player %u on frame %u\n", controller_id, ao2::coop::SyncFrame(base));
        // The matinee as the player who pressed, else as the other one
        // (only one of them may be in it). Same order on both machines.
        if (!SkipMovie(ctx, base, REXCVAR_GET(ao2_skip_unskippable)) && !SkipMatinee(ctx, base, controller_id))
        {
          for (uint32_t other = 0; other < 2; ++other)
          {
            if (other != controller_id && SkipMatinee(ctx, base, other))
            {
              break;
            }
          }
        }
        return;
      }
    }
  }

  bool IsMovieReceiver(const uint8_t *base, uint32_t receiver)
  {
    uint32_t player = LoadU32(base, kMoviePlayer);
    return player && receiver == player + 4;
  }

  void PollWhileMovie(PPCContext &ctx, uint8_t *base)
  {
    if (MoviePlaying(base))
    {
      Poll(ctx, base);
    }
  }

  void PollMovieThread(PPCContext &ctx, uint8_t *base)
  {
    Poll(ctx, base);
    // The pad: A or Start skips a skippable movie, as the game's handler -
    // not in a network game: a movie ended on one machine only (the level
    // intro) desynced the lockstep and the partner left.
    if (ao2::coop::InNetworkGame(base))
    {
      return;
    }
    uint32_t top = ScratchStackTop(ctx);
    uint32_t state = top + 512;
    std::memset(base + state, 0, 16);
    if (CallGuest(ctx, base, kXInputGetState, top, 0, state) != 0)
    {
      return;
    }
    static std::atomic<uint32_t> last_buttons{0};
    uint32_t buttons = uint32_t(base[state + 4]) << 8 | base[state + 5];
    uint32_t pressed = buttons & ~last_buttons.exchange(buttons);
    if (pressed & (kButtonA | kButtonStart))
    {
      SkipMovie(ctx, base, false);
    }
  }
} // namespace ao2::skip
