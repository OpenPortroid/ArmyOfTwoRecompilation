// audio_listener_guard.cpp - The audio listeners never hand the garbage
// collector a dead PortalVolume.
//
// UAudioDevice.Listeners (XeAudioDevice +136: TArray of FListener, 52 bytes:
// PortalVolume* at +0, then Location / Up / Right / Front) is walked by the
// collector's token stream like any object reference. Online co-op crashed
// in the collector over and over (2026-10-08..10), always while it was at
// XeAudioDevice_0 or at a "object" one hop from it: 2026-10-10 11:35 the
// partner's device had 2 listeners and Listeners[1].PortalVolume = 6 (the
// host, one listener). The second listener's volume is not refreshed every
// frame, so it keeps a pointer to a volume that is long gone and whose
// memory now holds anything (text "000", "1%00", floats 67.0 = 0x42860000
// in earlier crashes).
//
// Before every collection (sub_82531730) a listener's PortalVolume that is
// not a live object in GObjObjects (UObject index at +4) of class
// PortalVolume is cleared: no volume is what the audio code sees outside any
// volume anyway.
#include "log_files.h"
#include "ue3_reflect.h"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(ao2_audio_listener_guard, true, "Online",
                    "Clear audio listeners' dead PortalVolume references before garbage collection (a stale one "
                    "crashed the collector in co-op)");

namespace
{
  constexpr uint32_t kEngine = 0x831EFCBC;  // GEngine
  constexpr uint32_t kEngineClient = 756;   // GameEngine -> XenonClient
  constexpr uint32_t kClientAudio = 152;    // XenonClient -> XeAudioDevice
  constexpr uint32_t kListeners = 136;      // AudioDevice.Listeners
  constexpr uint32_t kListenerSize = 52;
  constexpr uint32_t kObjects = 0x831E4E3C; // GObjObjects TArray
  constexpr uint32_t kObjectIndex = 4;

  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }
  bool Plausible(uint32_t p)
  {
    return p >= 0x40000000 && p < 0x80000000 && !(p & 3);
  }
  bool LiveObject(const uint8_t *base, uint32_t object)
  {
    if (!Plausible(object))
    {
      return false;
    }
    uint32_t index = LoadU32(base, object + kObjectIndex);
    uint32_t data = LoadU32(base, kObjects), count = LoadU32(base, kObjects + 4);
    return Plausible(data) && index < count && LoadU32(base, data + index * 4) == object;
  }
} // namespace

extern "C" REX_FUNC(__imp__sub_82531730);
extern "C" REX_FUNC(sub_82531730)
{
  if (REXCVAR_GET(ao2_audio_listener_guard))
  {
    uint32_t engine = LoadU32(base, kEngine);
    uint32_t client = Plausible(engine) ? LoadU32(base, engine + kEngineClient) : 0;
    uint32_t audio = Plausible(client) ? LoadU32(base, client + kClientAudio) : 0;
    if (LiveObject(base, audio))
    {
      static uint32_t volume_class = 0;
      if (!volume_class)
      {
        volume_class = ao2::ue3::FindClass(base, "PortalVolume");
      }
      uint32_t data = LoadU32(base, audio + kListeners), count = LoadU32(base, audio + kListeners + 4);
      for (uint32_t i = 0; Plausible(data) && i < count && i < 16; ++i)
      {
        uint32_t at = data + i * kListenerSize;
        uint32_t volume = LoadU32(base, at);
        if (!volume || (LiveObject(base, volume) && volume_class && ao2::ue3::IsA(base, volume, volume_class)))
        {
          continue;
        }
        std::memset(base + at, 0, 4);
        if (FILE *f = ao2::OpenLogFile("audio_listener_guard.log", "a"))
        {
          std::fprintf(f, "listener %u of %u: dead PortalVolume %08X cleared\n", i, count, volume);
          std::fclose(f);
        }
      }
    }
  }
  __imp__sub_82531730(ctx, base);
}
