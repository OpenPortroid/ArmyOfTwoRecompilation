// console_cmd.cpp - Unreal Engine 3 console commands from outside the game.
//
// Debug tool: commands go through the first local player's console path,
// ULocalPlayer::Exec (sub_8286A720, through the player's FExec at +60, the
// same call as CANCELMATINEE in cutscene_skip.cpp), on the game thread at
// the engine's pad polling.
//
// - console.txt in the game folder: one command per line, run on the next
//   frame (the file is deleted). "checkpoint <name> [difficulty]" loads a
//   campaign checkpoint the way the game's own loads do (sub_8237D080, the
//   console's "open" is not there): the URL
//   Checkpoint?LoadSaveGame?CheckpointToLoad=<name>?... goes to the travel
//   event sub_8238C8E8; "travel <url>" sends any URL that way.
//   "set <cvar> <value>" changes a port setting (e.g. the renderer's post
//   effects, which apply from the next frame).
// - Queue(): the same lines from the app (the F1 debug menu).
// - --ao2_start_checkpoint=<name>: the same at startup - the startup movies
//   are skipped, then the checkpoint loads from the "Press Start" screen.
//
// Checkpoint names are the Kismet ones of the levels, e.g. Iraq (05_*):
// 05_100 (start), 05_00, 05_001, 05_01, 05_011 (US base), 05_02, 05_021,
// 05_022 (ruins), 05_03, 05_031 (fields), 05_10 (Iraqi base), 05_04
// (storage), 05_05, 05_06 (facility). Other missions: 01_* Somalia,
// 04_* Afghanistan, 06_* carrier, 07_* China, 08_* Miami.
//
// The loaded checkpoint becomes the profile's current one when the game
// saves it - use a separate --user_data_root to keep a real campaign intact.
// Not in online co-op (a load on one machine only): there both machines load
// it on the same lockstep frame (coop_cmd.cpp, LoadCheckpoint with the URL
// of the co-op lobby's maps: no local player count, no AI partner), through
// the co-op manager's travel like the game's own co-op restart (CoopTravel).
#include "console_cmd.h"

#include "coop.h"
#include "coop_cmd.h"
#include "cutscene_skip.h"
#include "log_files.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(ao2_start_checkpoint, "", "Debug",
                      "Debug: load this campaign checkpoint (e.g. 05_04) as soon as the main menu is up");
REXCVAR_DEFINE_INT32(ao2_start_difficulty, 0, "Debug",
                     "Debug: difficulty for ao2_start_checkpoint and the console.txt checkpoint command (0 = Recruit)");

namespace
{
  using clock = std::chrono::steady_clock;

  constexpr uint32_t kEngine = 0x831EFCBC;      // GEngine
  constexpr uint32_t kMoviePlayer = 0x831E6E64; // full-screen movie player (+88 playing)
  constexpr uint32_t kLocalPlayerExec = 0x8286A720;
  constexpr uint32_t kWorld = 0x831F2CC8;        // GWorld
  constexpr uint32_t kFStringPrintf = 0x8242A860; // (FString *out, const char *format, ...)
  constexpr uint32_t kFStringFree = 0x82309A60;   // FString destructor
  constexpr uint32_t kTravelEvent = 0x8238C8E8;   // (object, const FString *url)
  // Online co-op: the co-op manager (singleton, 4584 bytes) and its travel
  // (manager, const char *url) - what the game's own co-op checkpoint
  // restart (sub_82A168B8) calls on both machines once both voted: the host
  // keeps the URL (+2848) and sends it to the partner (message 66), the
  // partner hands it on (sub_82A0D848); +2804 marks a travel under way.
  constexpr uint32_t kCoopManager = 0x831F3024;
  constexpr uint32_t kCoopTravel = 0x82A16488;
  constexpr auto kMenuSettle = std::chrono::seconds(2);

  std::mutex g_queue_mutex;
  std::vector<std::string> g_queue;

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("console_cmd.log");
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

  uint32_t CallGuest(PPCContext &ctx, uint8_t *base, uint32_t fn, uint32_t stack_top, uint64_t r3, uint64_t r4 = 0,
                     uint64_t r5 = 0)
  {
    PPCContext saved = ctx;
    ctx.r1.u64 = stack_top;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
    uint32_t result = ctx.r3.u32;
    ctx = saved;
    return result;
  }

  // The first local player, 0 if none (GEngine +772 = GameViewportClient,
  // vtable +268 = local player of a controller id).
  uint32_t LocalPlayer(PPCContext &ctx, uint8_t *base, uint32_t top)
  {
    uint32_t engine = LoadU32(base, kEngine);
    uint32_t client = engine ? LoadU32(base, engine + 772) : 0;
    uint32_t vtable = client ? LoadU32(base, client) : 0;
    return vtable ? CallGuest(ctx, base, LoadU32(base, vtable + 268), top, client, 0) : 0;
  }

  // Travels to `url` as the game's own checkpoint loads do (sub_8237D080):
  // FString::Printf the URL, then the script event of sub_8238C8E8 on
  // [[[GWorld +80] +140]] with it.
  bool Travel(PPCContext &ctx, uint8_t *base, const std::string &url)
  {
    uint32_t world = LoadU32(base, kWorld);
    uint32_t a = world ? LoadU32(base, world + 80) : 0;
    uint32_t b = a ? LoadU32(base, a + 140) : 0;
    uint32_t target = b ? LoadU32(base, b) : 0;
    if (!target || url.find('%') != std::string::npos || url.size() > 400)
    {
      Logf("travel: no target (world %08X) or bad URL: %s\n", world, url.c_str());
      return false;
    }
    uint32_t top = (ctx.r1.u32 - 1024) & ~15u;
    // Above the callee's parameter save area (Printf is variadic and spills
    // r5..r10 to top + 32..79).
    uint32_t string = top + 256; // FString {data, count, max}
    uint32_t format = top + 320; // the URL as the format (TCHAR is char here)
    std::memset(base + string, 0, 12);
    std::memcpy(base + format, url.c_str(), url.size() + 1);
    CallGuest(ctx, base, kFStringPrintf, top, string, format);
    CallGuest(ctx, base, kTravelEvent, top, target, string);
    CallGuest(ctx, base, kFStringFree, top, string);
    Logf("travel: %s (target %08X)\n", url.c_str(), target);
    return true;
  }

  // Online co-op: travels to `url` through the co-op manager, as the game's
  // own co-op restart does (see kCoopTravel). The script travel event above
  // on both machines bypassed it: the level was torn down under the lockstep
  // and the garbage collector crashed minutes later on references left in
  // the HUD and the audio device (2026-10-09).
  bool CoopTravel(PPCContext &ctx, uint8_t *base, const std::string &url)
  {
    uint32_t manager = LoadU32(base, kCoopManager);
    if (!manager || url.find('%') != std::string::npos || url.size() > 400)
    {
      Logf("co-op travel: no co-op manager or bad URL: %s\n", url.c_str());
      return false;
    }
    uint32_t top = (ctx.r1.u32 - 1024) & ~15u;
    uint32_t string = top + 256; // FString {data, count, max}
    uint32_t format = top + 320;
    std::memset(base + string, 0, 12);
    std::memcpy(base + format, url.c_str(), url.size() + 1);
    CallGuest(ctx, base, kFStringPrintf, top, string, format);
    uint32_t data = LoadU32(base, string);
    if (data)
    {
      CallGuest(ctx, base, kCoopTravel, top, manager, data);
    }
    CallGuest(ctx, base, kFStringFree, top, string);
    Logf("co-op travel: %s (manager %08X, busy %u)\n", url.c_str(), manager, base[manager + 2804]);
    return data != 0;
  }

  bool Exec(PPCContext &ctx, uint8_t *base, const std::string &line)
  {
    char name[64];
    int difficulty = REXCVAR_GET(ao2_start_difficulty);
    if (std::sscanf(line.c_str(), "checkpoint %63s %d", name, &difficulty) >= 1)
    {
      return ao2::console::LoadCheckpoint(ctx, base, name, difficulty, false);
    }
    if (line.rfind("travel ", 0) == 0)
    {
      return Travel(ctx, base, line.substr(7));
    }
    if (line.rfind("set ", 0) == 0)
    {
      // A port setting (cvar) by name, e.g. "set ao2_anti_aliasing smaa".
      char cvar[128], value[256];
      bool ok = std::sscanf(line.c_str(), "set %127s %255s", cvar, value) == 2 && rex::cvar::SetFlagByName(cvar, value);
      Logf("%s: %s\n", line.c_str(), ok ? "set" : "not set");
      return ok;
    }
    const std::string &command = line;
    uint32_t top = (ctx.r1.u32 - 1024) & ~15u;
    uint32_t player = LocalPlayer(ctx, base, top);
    if (!player)
    {
      Logf("no local player for: %s\n", command.c_str());
      return false;
    }
    // The text in scratch memory above the callee's stack and parameter
    // save area, below the caller's.
    size_t length = std::min<size_t>(command.size(), 600);
    uint32_t text = top + 256;
    std::memcpy(base + text, command.c_str(), length);
    base[text + length] = 0;
    uint32_t handled = CallGuest(ctx, base, kLocalPlayerExec, top, player + 60, text, 0);
    Logf("exec \"%s\" -> %u\n", command.c_str(), handled);
    return true;
  }

  std::vector<std::string> TakeFile()
  {
    std::vector<std::string> lines;
    FILE *f = std::fopen("console.txt", "rb");
    if (!f)
    {
      return lines;
    }
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), f))
    {
      std::string line = buffer;
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
      {
        line.pop_back();
      }
      size_t start = line.find_first_not_of(" \t\xEF\xBB\xBF");
      if (start != std::string::npos && line[start] != '#')
      {
        lines.push_back(line.substr(start));
      }
    }
    std::fclose(f);
    std::remove("console.txt");
    return lines;
  }
} // namespace

namespace ao2::console
{
  void Poll(PPCContext &ctx, uint8_t *base)
  {
    if (ao2::coop::InNetworkGame(base))
    {
      // console.txt in a network game: a checkpoint load goes to both machines through the lockstep
      // (coop_cmd.cpp, as the F1 menu's), a port setting is this machine's own; the rest stays local-only
      // game state, which would desync - left out.
      static clock::time_point next_check{};
      auto now = clock::now();
      if (now < next_check)
      {
        return;
      }
      next_check = now + std::chrono::milliseconds(250);
      for (const std::string &line : TakeFile())
      {
        char name[64];
        int difficulty = REXCVAR_GET(ao2_start_difficulty);
        if (std::sscanf(line.c_str(), "checkpoint %63s %d", name, &difficulty) >= 1)
        {
          ao2::coop::cmd::Send("checkpoint " + std::string(name) + " " + std::to_string(difficulty));
          Logf("co-op: \"%s\" sent to both machines\n", line.c_str());
        }
        else if (line.rfind("set ", 0) == 0)
        {
          Exec(ctx, base, line);
        }
        else
        {
          Logf("co-op: \"%s\" ignored (local game state would desync)\n", line.c_str());
        }
      }
      return;
    }

    // The start checkpoint: the startup movies are skipped, and once they
    // are over (the "Press Start" screen is enough) the checkpoint loads.
    static bool start_done = false;
    static clock::time_point quiet_since{}, next_skip{};
    if (!start_done)
    {
      std::string checkpoint = REXCVAR_GET(ao2_start_checkpoint);
      auto now = clock::now();
      uint32_t movie = LoadU32(base, kMoviePlayer);
      bool playing = movie && LoadU32(base, movie + 88);
      if (checkpoint.empty())
      {
        start_done = true;
      }
      else if (playing)
      {
        quiet_since = {};
        if (now >= next_skip)
        {
          next_skip = now + std::chrono::seconds(1);
          ao2::skip::Request();
        }
      }
      else if (!LocalPlayer(ctx, base, (ctx.r1.u32 - 1024) & ~15u))
      {
        quiet_since = {};
      }
      else if (quiet_since == clock::time_point{})
      {
        quiet_since = now;
      }
      else if (now - quiet_since > kMenuSettle)
      {
        start_done = true;
        Exec(ctx, base, "checkpoint " + checkpoint);
      }
    }

    std::vector<std::string> queued;
    {
      std::lock_guard lock(g_queue_mutex);
      queued.swap(g_queue);
    }
    for (const std::string &line : queued)
    {
      Exec(ctx, base, line);
    }

    // console.txt, checked a few times per second.
    static clock::time_point next_check{};
    auto now = clock::now();
    if (now < next_check)
    {
      return;
    }
    next_check = now + std::chrono::milliseconds(250);
    for (const std::string &line : TakeFile())
    {
      Exec(ctx, base, line);
    }
  }

  bool LoadCheckpoint(PPCContext &ctx, uint8_t *base, const std::string &name, int difficulty, bool network)
  {
    char url[256];
    if (network)
    {
      std::snprintf(url, sizeof(url), "Checkpoint?LoadSaveGame?CheckpointToLoad=%s?Difficulty=%d?", name.c_str(),
                    difficulty);
    }
    else
    {
      std::snprintf(url, sizeof(url),
                    "Checkpoint?LoadSaveGame?CheckpointToLoad=%s?LocalPlayers=1?AutoSpawnPAI=1?Difficulty=%d",
                    name.c_str(), difficulty);
    }
    return network ? CoopTravel(ctx, base, url) : Travel(ctx, base, url);
  }

  void Queue(std::string line)
  {
    std::lock_guard lock(g_queue_mutex);
    g_queue.push_back(std::move(line));
  }
} // namespace ao2::console

// The travel event (r3 = object, r4 = FString *url: data, count): every
// URL the game itself travels to goes to console_cmd.log with the lockstep
// frame - the game's own co-op checkpoint loads, to compare with ours.
extern "C" REX_FUNC(__imp__sub_8238C8E8);
extern "C" REX_FUNC(sub_8238C8E8)
{
  uint32_t string = ctx.r4.u32;
  uint32_t data = string ? LoadU32(base, string) : 0;
  uint32_t count = string ? LoadU32(base, string + 4) : 0;
  if (data && count > 0 && count < 512)
  {
    const char *chars = reinterpret_cast<const char *>(base + data);
    std::string url(chars, strnlen(chars, count));
    Logf("travel event (frame %u, %s): %s\n", ao2::coop::SyncFrame(base),
         ao2::coop::OnlineLockstep(base) ? "online" : "local", url.c_str());
  }
  __imp__sub_8238C8E8(ctx, base);
}
