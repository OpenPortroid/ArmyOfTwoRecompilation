// coop_cmd.cpp - Debug commands for both online co-op machines.
//
// Online co-op is a lockstep simulation (coop.h): god mode or a checkpoint
// load on one machine only desyncs the game. So the F1 debug menu's actions
// go through the exchanged input, like the cutscene skip (cutscene_skip.cpp):
// the command's text rides in this player's frame record as key presses,
// and both machines run it when they replay that record - on the same frame.
//
// A record holds up to 15 key presses (+64, 12 bytes each: key table index,
// event, amount; count at +244). A block of a command is
//   [F11] [tag] [seq] [n + 10 if the command starts here] [sum] [n symbols]
// with every value a key table index, so it survives the trip to the partner
// like any press. F11 is a key the pads never send (as F12 for the skip); its
// index is looked up by name in the key table. Everything after it is a
// symbol below 100 (text: char - 32, 95 ends the command), never F11 or F12.
// tag: random per run, sum: of the other symbols, mod 100.
//
// What the record of a frame holds is not always what was put into it: when
// the input delay changes, frames are merged or dropped (the first command
// of a test arrived with two blocks swapped and a byte lost). So one block is
// in flight at a time: the next goes only once this machine has replayed its
// own record with the block in it (the tag says whose), and it goes again
// after kResendFrames. The receiver takes a block whose sum fits and whose
// seq follows the last one, ignores a repeat and drops a command with a gap.
// All of that depends only on the replayed records, so both machines decide
// alike. 6 symbols per block, 4 entries are left for the player's presses.
//
// The blocks are taken out of the record before the game and the cutscene
// skip see it, and put back afterwards: a record sent again later is whole.
//
// Commands (one stream per player, by the record's controller id):
// - "god 0|1": that player's god mode (cheats.cpp). Sent whenever
//   ao2_god_mode (the menu's checkbox) differs from what went out last.
// - "aimassist 0|1": that player's aim assist (aim_assist.cpp). Sent whenever
//   ao2_aim_assist differs from what went out last (at first: from the
//   default, off).
// - "checkpoint <name> <difficulty>": both load the checkpoint
//   (console_cmd.cpp, the co-op lobby's URL). Both need the file: with DLC
//   on one PC only, a DLC checkpoint desyncs the game. The load runs at the
//   next engine pad poll, not in the replay: run there, it tore the level
//   down while the frame sync was still replaying the other player's record
//   into it, and the garbage collector crashed a minute later on what that
//   left behind (the audio device's references, 2026-10-09; each such crash
//   came after a debug menu load). The pad poll is where the game's own UI
//   starts its loads, between the same two lockstep frames on both machines.
//   The load goes through the co-op manager's travel, as the game's own co-op
//   restart (console_cmd.cpp CoopTravel): the plain script travel event on
//   both machines still crashed the garbage collector minutes later. Only
//   the machine that sent the command (its tag) starts it: the manager sends
//   the travel to the partner itself, and a start on both loaded the level
//   twice on both.
//
// --ao2_coop_test_local=true runs all of this in single player (whose frame
// sync replays the player's own records too).
#include "coop_cmd.h"

#include "aim_assist.h"
#include "cheats.h"
#include "console_cmd.h"
#include "coop.h"
#include "log_files.h"
#include "ue3_reflect.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, ao2_god_mode);
REXCVAR_DECLARE(bool, ao2_aim_assist);
REXCVAR_DECLARE(bool, ao2_coop_test_local);

namespace
{
  constexpr uint32_t kFrameSync = 0x831F3008;        // frame sync object singleton
  constexpr uint32_t kSyncIsLocalPad = 0x82A1E8E8;   // (sync, controller id) -> records this pad
  constexpr uint32_t kSyncRecordHolder = 0x82A2A440; // (sync, controller id) -> record holder (record at +4)
  constexpr uint32_t kKeyTable = 0x83186320;         // 140 pointers to the key FNames
  constexpr uint32_t kKeyCount = 140;
  constexpr uint32_t kMaxEntries = 15;
  constexpr uint32_t kKeepFree = 4;   // entries left for the player's presses
  constexpr uint32_t kHeader = 5;     // F11, tag, seq, flags, sum
  constexpr uint32_t kMaxSymbols = 6; // per block
  constexpr uint32_t kSymbols = 100;  // symbol values 0..99
  constexpr uint32_t kEnd = 95;       // ends a command
  constexpr uint32_t kStart = 10;     // flags: the command starts in this block
  constexpr uint32_t kResendFrames = 30;
  constexpr size_t kMaxCommand = 96;
  constexpr const char *kMarkName = "F11";

  std::mutex g_queue_mutex;
  std::string g_queue; // symbols of commands not yet sent, each ending with kEnd

  // Game thread only.
  struct Block
  {
    uint32_t seq = 0;
    bool start = false;
    std::string symbols;
  };
  std::string g_out;       // symbols not yet in a block
  bool g_out_start = true; // g_out begins a command
  Block g_block;           // in flight
  bool g_in_flight = false;
  uint32_t g_next_seq = 0;
  uint32_t g_waited = 0;       // lockstep frames since the block went out
  uint32_t g_last_frame = ~0u; // lockstep frame of the last poll
  uint32_t g_tag = 0;
  int g_sent_god = 0; // the god mode wish that went out last
  int g_sent_assist = 0; // the aim assist choice that went out last (0: the default)
  int g_mark = -1;    // key table index of F11

  struct Stream
  {
    int last_seq = -1; // of the last block taken, -1 = none
    bool synced = false;
    bool mine = false; // the command being taken was sent by this machine
    std::string text;
  };
  std::map<uint32_t, Stream> g_streams; // per controller id

  // A checkpoint load replayed this frame, run at the next Poll.
  struct PendingLoad
  {
    bool set = false;
    std::string name;
    int difficulty = 0;
    bool network = false;
    bool mine = false; // sent by this machine
  };
  PendingLoad g_load;

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("coop_cmd.log");
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

  uint32_t CallGuest(PPCContext &ctx, uint8_t *base, uint32_t fn, uint32_t stack_top, uint64_t r3, uint64_t r4)
  {
    PPCContext saved = ctx;
    ctx.r1.u64 = stack_top;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
    uint32_t result = ctx.r3.u32;
    ctx = saved;
    return result;
  }

  bool LocalTest(const uint8_t *base)
  {
    return REXCVAR_GET(ao2_coop_test_local) && ao2::coop::LocalLockstep(base);
  }

  // The key table index of F11, -1 until the key names are set up (or when
  // F11 could be a symbol).
  int Mark(const uint8_t *base)
  {
    if (g_mark >= 0)
    {
      return g_mark < int(kKeyCount) ? g_mark : -1;
    }
    for (uint32_t i = 0; i < kKeyCount; ++i)
    {
      uint32_t name = LoadU32(base, kKeyTable + i * 4);
      if (name && ao2::ue3::Name(base, LoadU32(base, name)) == kMarkName)
      {
        Logf("%s is key %u%s\n", kMarkName, i, i < kSymbols ? " - below the symbols, commands are off" : "");
        g_mark = i < kSymbols ? kKeyCount : int(i);
        break;
      }
    }
    return g_mark < int(kKeyCount) ? g_mark : -1;
  }

  uint32_t Sum(uint32_t tag, uint32_t seq, uint32_t flags, const uint32_t *symbols, uint32_t n)
  {
    uint32_t sum = tag + seq + flags;
    for (uint32_t i = 0; i < n; ++i)
    {
      sum += symbols[i];
    }
    return sum % kSymbols;
  }

  // The next block of g_out (up to the end of its command).
  void TakeBlock()
  {
    size_t end = g_out.find(char(kEnd));
    size_t n = std::min<size_t>({kMaxSymbols, g_out.size(), end == std::string::npos ? g_out.size() : end + 1});
    g_block.seq = g_next_seq;
    g_next_seq = (g_next_seq + 1) % kSymbols;
    g_block.start = g_out_start;
    g_block.symbols = g_out.substr(0, n);
    g_out.erase(0, n);
    g_out_start = uint8_t(g_block.symbols.back()) == kEnd;
    g_in_flight = true;
  }

  // g_block into this player's record for the frame being collected; false
  // when it cannot go now.
  bool WriteBlock(PPCContext &ctx, uint8_t *base, uint32_t mark)
  {
    uint32_t sync = LoadU32(base, kFrameSync);
    uint32_t top = (ctx.r1.u32 - 1024) & ~15u;
    // As sub_82A2D570 (record a key): nothing while +36 is set.
    if (!sync || LoadU32(base, sync + 36) || !CallGuest(ctx, base, kSyncIsLocalPad, top, sync, 0))
    {
      return false;
    }
    uint32_t holder = CallGuest(ctx, base, kSyncRecordHolder, top, sync, 0);
    if (!holder)
    {
      return false;
    }
    uint32_t record = holder + 4;
    uint32_t count = LoadU32(base, record + 244);
    uint32_t n = uint32_t(g_block.symbols.size());
    if (count + kHeader + n + kKeepFree > kMaxEntries)
    {
      return false;
    }
    uint32_t symbols[kMaxSymbols];
    for (uint32_t i = 0; i < n; ++i)
    {
      symbols[i] = uint8_t(g_block.symbols[i]);
    }
    uint32_t flags = n + (g_block.start ? kStart : 0);
    auto put = [&](uint32_t key) {
      uint32_t entry = record + 64 + count * 12;
      StoreU32(base, entry, key);
      StoreU32(base, entry + 4, 0); // IE_Pressed
      StoreU32(base, entry + 8, 0);
      ++count;
    };
    put(mark);
    put(g_tag);
    put(g_block.seq);
    put(flags);
    put(Sum(g_tag, g_block.seq, flags, symbols, n));
    for (uint32_t i = 0; i < n; ++i)
    {
      put(symbols[i]);
    }
    StoreU32(base, record + 244, count);
    Logf("frame %u: block seq %u (%u symbols%s) into record %08X, %zu symbols queued\n", ao2::coop::SyncFrame(base),
         g_block.seq, n, g_block.start ? ", start" : "", record, g_out.size());
    return true;
  }

  bool ValidName(const std::string &name)
  {
    return !name.empty() && name.size() <= 32 &&
           std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(uint8_t(c)) || c == '_'; });
  }

  // A block of controller `controller_id`'s stream.
  void TakeSymbols(Stream &stream, uint32_t seq, bool start, bool mine, const uint32_t *symbols, uint32_t n,
                   std::vector<std::pair<std::string, bool>> &commands)
  {
    if (stream.last_seq == int(seq))
    {
      return; // sent again: already taken
    }
    if (start)
    {
      stream.synced = true;
      stream.mine = mine;
      stream.text.clear();
    }
    else if (stream.last_seq < 0 || seq != (uint32_t(stream.last_seq) + 1) % kSymbols)
    {
      if (stream.synced)
      {
        Logf("gap before seq %u (last %d): command dropped\n", seq, stream.last_seq);
      }
      stream.synced = false;
    }
    stream.last_seq = int(seq);
    if (!stream.synced)
    {
      stream.text.clear();
      return;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
      if (symbols[i] == kEnd)
      {
        if (!stream.text.empty())
        {
          commands.emplace_back(stream.text, stream.mine);
        }
        stream.text.clear();
      }
      else if (symbols[i] < kEnd && stream.text.size() < kMaxCommand)
      {
        stream.text += char(symbols[i] + 32);
      }
      else
      {
        stream.synced = false;
        stream.text.clear();
        return;
      }
    }
  }

  // Runs on both machines on the same frame: whatever it decides must only
  // depend on the command and the shared game state.
  void Run(PPCContext &ctx, uint8_t *base, uint32_t controller_id, const std::string &command, bool mine)
  {
    Logf("frame %u: player %u: %s\n", ao2::coop::SyncFrame(base), controller_id, command.c_str());
    int on = 0;
    char name[40];
    int difficulty = 0;
    if (std::sscanf(command.c_str(), "god %d", &on) == 1)
    {
      ao2::cheats::SetPlayerGod(ctx, base, controller_id, on != 0);
    }
    else if (std::sscanf(command.c_str(), "aimassist %d", &on) == 1)
    {
      ao2::aim_assist::SetPlayer(controller_id, on != 0);
    }
    else if (std::sscanf(command.c_str(), "checkpoint %39s %d", name, &difficulty) == 2 && ValidName(name) &&
             difficulty >= 0 && difficulty <= 2)
    {
      g_load = {true, name, difficulty, ao2::coop::OnlineLockstep(base), mine};
    }
    else
    {
      Logf("unknown command\n");
    }
  }
} // namespace

namespace ao2::coop::cmd
{
  void Send(std::string line)
  {
    std::string symbols;
    for (char c : line)
    {
      symbols += char(c >= 32 && c < 127 ? c - 32 : '?' - 32);
    }
    symbols += char(kEnd);
    std::lock_guard lock(g_queue_mutex);
    g_queue += symbols;
  }

  bool Routed(const uint8_t *base)
  {
    return ao2::coop::InNetworkGame(base) || LocalTest(base);
  }

  bool Ready(const uint8_t *base)
  {
    return ao2::coop::OnlineLockstep(base) || LocalTest(base);
  }

  void Poll(PPCContext &ctx, uint8_t *base)
  {
    if (!Routed(base))
    {
      // The game is over: on both machines, nothing of it is left.
      if (g_in_flight || !g_out.empty() || !g_streams.empty() || g_sent_god || g_sent_assist)
      {
        Logf("network game over\n");
      }
      g_out.clear();
      g_out_start = true;
      g_in_flight = false;
      g_streams.clear();
      g_sent_god = 0;
      g_sent_assist = 0;
      ao2::aim_assist::Reset();
      g_load = {};
      std::lock_guard lock(g_queue_mutex);
      g_queue.clear();
      return;
    }
    if (g_load.set)
    {
      PendingLoad load = g_load;
      g_load = {};
      if (load.network && !load.mine)
      {
        // The partner's load: its machine starts it through the co-op
        // manager, which sends it here (two starts loaded the level twice
        // on both machines).
        Logf("frame %u: %s - the partner's load, it comes over the network\n", ao2::coop::SyncFrame(base),
             load.name.c_str());
      }
      else
      {
        Logf("frame %u: loading %s\n", ao2::coop::SyncFrame(base), load.name.c_str());
        ao2::console::LoadCheckpoint(ctx, base, load.name, load.difficulty, load.network);
      }
    }
    if (!g_tag)
    {
      g_tag = 1 + std::random_device{}() % (kSymbols - 1);
      Logf("tag %u\n", g_tag);
    }
    int mark = Mark(base);
    if (!Ready(base) || mark < 0)
    {
      return;
    }
    int god = REXCVAR_GET(ao2_god_mode) ? 1 : 0;
    if (god != g_sent_god)
    {
      g_sent_god = god;
      Send("god " + std::to_string(god));
    }
    int assist = REXCVAR_GET(ao2_aim_assist) ? 1 : 0;
    if (assist != g_sent_assist)
    {
      g_sent_assist = assist;
      Send("aimassist " + std::to_string(assist));
    }
    // Once per lockstep frame.
    uint32_t frame = ao2::coop::SyncFrame(base);
    if (frame == g_last_frame)
    {
      return;
    }
    g_last_frame = frame;
    if (g_in_flight)
    {
      if (++g_waited < kResendFrames)
      {
        return;
      }
      Logf("frame %u: seq %u not replayed yet, again\n", frame, g_block.seq);
    }
    else
    {
      if (g_out.empty())
      {
        std::lock_guard lock(g_queue_mutex);
        g_out.swap(g_queue);
      }
      if (g_out.empty())
      {
        return;
      }
      TakeBlock();
    }
    if (WriteBlock(ctx, base, uint32_t(mark)))
    {
      g_waited = 0;
    }
  }

  void BeforeReplay(uint8_t *base, uint32_t record, uint32_t controller_id, Replay &replay)
  {
    replay.record = record;
    replay.controller_id = controller_id;
    int mark = Ready(base) ? Mark(base) : -1;
    if (mark < 0)
    {
      return;
    }
    uint32_t count = std::min(LoadU32(base, record + 244), kMaxEntries);
    uint32_t keys[kMaxEntries];
    bool any = false;
    for (uint32_t i = 0; i < count; ++i)
    {
      keys[i] = LoadU32(base, record + 64 + i * 12);
      any |= keys[i] == uint32_t(mark);
    }
    if (!any)
    {
      return;
    }
    replay.stripped = true;
    replay.count = LoadU32(base, record + 244);
    std::memcpy(replay.entries, base + record + 64, sizeof(replay.entries));
    if (Log())
    {
      Logf("frame %u: player %u record %08X:", ao2::coop::SyncFrame(base), controller_id, record);
      for (uint32_t i = 0; i < count; ++i)
      {
        Logf(" %u", keys[i]);
      }
      Logf("\n");
    }

    Stream &stream = g_streams[controller_id];
    uint8_t *entries = base + record + 64;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count;)
    {
      if (keys[i] != uint32_t(mark))
      {
        if (kept != i)
        {
          std::memmove(entries + kept * 12, entries + i * 12, 12);
        }
        ++kept;
        ++i;
        continue;
      }
      // A block: everything up to its end (or the record's) goes.
      uint32_t flags = i + 3 < count ? keys[i + 3] : 0;
      uint32_t n = flags % kStart;
      uint32_t end = std::min(i + kHeader + n, count);
      bool valid = i + kHeader + n <= count && n >= 1 && n <= kMaxSymbols && flags < kStart * 2;
      for (uint32_t j = i + 1; j < end && valid; ++j)
      {
        valid = keys[j] < kSymbols;
      }
      const uint32_t *symbols = keys + i + kHeader;
      valid = valid && keys[i + 4] == Sum(keys[i + 1], keys[i + 2], flags, symbols, n);
      if (!valid)
      {
        Logf("  bad block at %u\n", i);
      }
      else
      {
        uint32_t tag = keys[i + 1], seq = keys[i + 2];
        if (g_in_flight && tag == g_tag && seq == g_block.seq)
        {
          g_in_flight = false; // ours, replayed: the next one may go
        }
        TakeSymbols(stream, seq, flags >= kStart, tag == g_tag, symbols, n, replay.commands);
      }
      i = std::max(end, i + 1);
    }
    std::memset(entries + kept * 12, 0, (count - kept) * 12);
    StoreU32(base, record + 244, kept);
  }

  void AfterReplay(PPCContext &ctx, uint8_t *base, Replay &replay)
  {
    if (replay.stripped)
    {
      std::memcpy(base + replay.record + 64, replay.entries, sizeof(replay.entries));
      StoreU32(base, replay.record + 244, replay.count);
    }
    for (const auto &[command, mine] : replay.commands)
    {
      Run(ctx, base, replay.controller_id, command, mine);
    }
    ao2::cheats::OnReplayedRecord(ctx, base, replay.controller_id);
  }
} // namespace ao2::coop::cmd
