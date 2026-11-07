// audio_settings.cpp - The F1 menu's volumes and the pad's vibration.
//
// Master volume is the runtime's output gain (rex/audio/downmix.h): it covers
// everything the game plays, movies too.
//
// Music, effects and voices are UE3's sound groups (the ini's
// [Engine.AudioDevice] SoundGroups: Master has the children SFX, MusicMaster,
// Voice, Cinematic and UI). UAudioDevice keeps the ini's values in a TArray at
// +212, 44 bytes each: Properties.Volume at +0, GroupName at +24.
// sub_826ADE40 copies them to +200 and multiplies children by their parents;
// every device update then blends them into +188 (sub_82673FC8), which is what
// sounds play by (sub_82683B60).
//
// The game writes the values at +212 itself (sub_826AE5C8, from the mixer
// sub_82A173A8, inferred: a volume written there was back at 1 three minutes
// later). So they stay the game's: the parse runs with the top groups' values
// multiplied by the sliders' and put back after it, whoever asks for the parse.
// A moved slider asks for one. That drops what the current sound mode added
// until the next mode change, as SetAudioGroupVolume does.
//
// The vibration: the XDK's XInputSetState (sub_82B07B00) gets the motors'
// speeds scaled for the call.
#include "audio_settings.h"

#include "log_files.h"
#include "ue3_reflect.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <iterator>

#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_DOUBLE(ao2_volume_master, 1.0, "Audio", "Volume of everything the game plays, 0..1");
REXCVAR_DEFINE_DOUBLE(ao2_volume_music, 1.0, "Audio", "Volume of the music (sound group MusicMaster), 0..1");
REXCVAR_DEFINE_DOUBLE(ao2_volume_effects, 1.0, "Audio", "Volume of the sound effects (sound group SFX), 0..1");
REXCVAR_DEFINE_DOUBLE(ao2_volume_voices, 1.0, "Audio", "Volume of the voices (sound group Voice), 0..1");
REXCVAR_DEFINE_DOUBLE(ao2_pad_vibration, 1.0, "Input", "Strength of the gamepad's vibration, 0..1; 0 = none");

namespace
{
  constexpr uint32_t kGEngine = 0x831EFCBC;
  constexpr uint32_t kEngineClient = 756;       // UGameEngine.Client
  constexpr uint32_t kClientAudioDevice = 288;  // the client's vtable: GetAudioDevice()
  constexpr uint32_t kBaseGroups = 212;         // UAudioDevice: the ini's sound groups (TArray)
  constexpr uint32_t kGroupSize = 44;
  constexpr uint32_t kGroupName = 24;
  constexpr uint32_t kParseSoundGroups = 0x826ADE40;

  struct Group
  {
    const char *name;
    double (*get)();
    float applied;
  };
  Group g_groups[] = {
      {"MusicMaster", [] { return REXCVAR_GET(ao2_volume_music); }, 1.0f},
      {"SFX", [] { return REXCVAR_GET(ao2_volume_effects); }, 1.0f},
      {"Voice", [] { return REXCVAR_GET(ao2_volume_voices); }, 1.0f},
  };
  float g_master = 1.0f;

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

  uint32_t CallGuest(PPCContext &ctx, uint8_t *base, uint32_t fn, uint64_t r3)
  {
    PPCContext saved = ctx;
    ctx.r1.u64 = (ctx.r1.u32 - 2048) & ~15u;
    ctx.r3.u64 = r3;
    rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
    uint32_t result = ctx.r3.u32;
    ctx = saved;
    return result;
  }

  uint32_t AudioDevice(PPCContext &ctx, uint8_t *base)
  {
    uint32_t engine = LoadU32(base, kGEngine);
    uint32_t client = engine ? LoadU32(base, engine + kEngineClient) : 0;
    if (!client)
    {
      return 0;
    }
    return CallGuest(ctx, base, LoadU32(base, LoadU32(base, client) + kClientAudioDevice), client);
  }

  float Volume(double value)
  {
    return float(std::clamp(value, 0.0, 1.0));
  }

  void Log(const char *group, float volume)
  {
    static FILE *f = ao2::OpenLogFile("audio_settings.log");
    if (f)
    {
      std::fprintf(f, "%s = %.2f\n", group, volume);
      std::fflush(f);
    }
  }

  // The ini's entry of a group, 0 if none.
  uint32_t GroupEntry(const uint8_t *base, uint32_t device, const char *name)
  {
    uint32_t data = LoadU32(base, device + kBaseGroups);
    int32_t count = int32_t(LoadU32(base, device + kBaseGroups + 4));
    for (int32_t i = 0; i < count; ++i)
    {
      uint32_t entry = data + uint32_t(i) * kGroupSize;
      if (ao2::ue3::Name(base, LoadU32(base, entry + kGroupName)) == name)
      {
        return entry;
      }
    }
    return 0;
  }
} // namespace

namespace ao2::audio_settings
{
  void Tick(PPCContext &ctx, uint8_t *base)
  {
    float master = Volume(REXCVAR_GET(ao2_volume_master));
    if (master != g_master)
    {
      g_master = master;
      rex::audio::SetOutputGain(master);
      Log("Master", master);
    }

    bool moved = false;
    for (const Group &group : g_groups)
    {
      moved = moved || Volume(group.get()) != group.applied;
    }
    if (!moved)
    {
      return;
    }
    uint32_t device = AudioDevice(ctx, base);
    if (!device)
    {
      return;
    }
    for (Group &group : g_groups)
    {
      float wanted = Volume(group.get());
      if (wanted != group.applied)
      {
        group.applied = wanted;
        Log(group.name, wanted);
      }
    }
    CallGuest(ctx, base, kParseSoundGroups, device);
  }
} // namespace ao2::audio_settings

// UAudioDevice's parse of the sound groups (see above).
extern "C" REX_FUNC(__imp__sub_826ADE40);
extern "C" REX_FUNC(sub_826ADE40)
{
  uint32_t device = ctx.r3.u32;
  struct Saved
  {
    uint32_t entry, bits;
  } saved[std::size(g_groups)];
  size_t count = 0;
  for (const Group &group : g_groups)
  {
    float volume = Volume(group.get());
    uint32_t entry = volume < 1.0f ? GroupEntry(base, device, group.name) : 0;
    if (entry)
    {
      uint32_t bits = LoadU32(base, entry);
      saved[count++] = {entry, bits};
      StoreU32(base, entry, std::bit_cast<uint32_t>(std::bit_cast<float>(bits) * volume));
    }
  }
  __imp__sub_826ADE40(ctx, base);
  for (size_t i = 0; i < count; ++i)
  {
    StoreU32(base, saved[i].entry, saved[i].bits);
  }
}

// DWORD XInputSetState(DWORD user, XINPUT_VIBRATION *vibration): two
// big-endian 16 bit speeds. Put back after the call: the vibration is the
// caller's.
extern "C" REX_FUNC(__imp__sub_82B07B00);
extern "C" REX_FUNC(sub_82B07B00)
{
  uint32_t vibration = ctx.r4.u32;
  double strength = std::clamp(REXCVAR_GET(ao2_pad_vibration), 0.0, 1.0);
  if (!vibration || strength >= 1.0)
  {
    __imp__sub_82B07B00(ctx, base);
    return;
  }
  uint16_t motors[2], scaled[2];
  std::memcpy(motors, base + vibration, sizeof(motors));
  for (int i = 0; i < 2; ++i)
  {
    scaled[i] = std::byteswap(uint16_t(double(std::byteswap(motors[i])) * strength));
  }
  std::memcpy(base + vibration, scaled, sizeof(scaled));
  __imp__sub_82B07B00(ctx, base);
  std::memcpy(base + vibration, motors, sizeof(motors));
}
