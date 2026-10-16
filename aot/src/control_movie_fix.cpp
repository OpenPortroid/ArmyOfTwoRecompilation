// control_movie_fix.cpp - The endless "LOADING MAP" screen in Iraq.
//
// AO2's Kismet action ControlMovie (AO2SeqAct_ControlMovie, and the UI
// variant UIAction_ControlMovie) starts the full-screen movie on its Play
// input (0) and stops it on Stop (1): Activated (sub_822B8058 /
// sub_822B8078) passes InputLinks[0].bHasImpulse as "play" to sub_822B7D40,
// which sends ClientPlayMovie or ClientStopMovie to every controller.
//
// Iraq's streaming sequence plays the loading movie (LoadingCoin, looped) when
// the player is teleported between areas (remote event teleport_to_Storage)
// and stops it once the next area's levels are visible: Teleport -> Set
// Property -> Wait for Levels to be visible -> Toggle Cinematic Mode ->
// ControlMovie Stop. On the console the wait takes frames. Here the levels
// are already loaded and visible (streaming volumes brought them in, loading
// is fast), so the wait finishes at once and the Stop impulse reaches the
// same ControlMovie in the same Kismet update as Play. The op then runs once
// with both impulses, Play wins and the movie loops for good. With both
// impulses set the op runs as Stop here (Stop always comes after Play).
#include <bit>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "log_files.h"

REXCVAR_DECLARE(bool, ao2_movie_debug);

namespace
{
  constexpr uint32_t kInputLinks = 136;       // SequenceOp.InputLinks (TArray)
  constexpr uint32_t kInputLinkStride = 40;   // FSeqOpInputLink
  constexpr uint32_t kImpulseWord = 12;       // bHasImpulse bitfield word
  constexpr uint32_t kHasImpulse = 0x80000000;

  FILE *Log()
  {
    static FILE *f = ao2::OpenLogFile("control_movie.log");
    return f;
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

  // Runs Activated; with both Play and Stop impulses set, as Stop.
  template <void (*Original)(PPCContext &, uint8_t *)>
  void Activated(PPCContext &ctx, uint8_t *base, const char *kind)
  {
    uint32_t op = ctx.r3.u32;
    uint32_t links = LoadU32(base, op + kInputLinks);
    uint32_t count = LoadU32(base, op + kInputLinks + 4);
    if (!links || count < 2)
    {
      Original(ctx, base);
      return;
    }
    uint32_t play_word = links + kImpulseWord;
    uint32_t stop_word = links + kInputLinkStride + kImpulseWord;
    uint32_t play = LoadU32(base, play_word);
    bool stop = LoadU32(base, stop_word) & kHasImpulse;
    bool both = (play & kHasImpulse) && stop;
    if (both || REXCVAR_GET(ao2_movie_debug))
    {
      if (FILE *f = Log())
      {
        std::fprintf(f, "%s %08X play=%d stop=%d%s\n", kind, op, (play & kHasImpulse) ? 1 : 0, stop ? 1 : 0,
                     both ? " -> both in one update, running as Stop" : "");
        std::fflush(f);
      }
    }
    if (both)
    {
      StoreU32(base, play_word, play & ~kHasImpulse);
    }
    Original(ctx, base);
    if (both)
    {
      StoreU32(base, play_word, play);
    }
  }
} // namespace

extern "C" REX_FUNC(__imp__sub_822B8058);
extern "C" REX_FUNC(sub_822B8058)
{
  Activated<__imp__sub_822B8058>(ctx, base, "AO2SeqAct_ControlMovie");
}

extern "C" REX_FUNC(__imp__sub_822B8078);
extern "C" REX_FUNC(sub_822B8078)
{
  Activated<__imp__sub_822B8078>(ctx, base, "UIAction_ControlMovie");
}
