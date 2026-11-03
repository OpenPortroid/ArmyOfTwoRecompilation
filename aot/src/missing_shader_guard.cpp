// missing_shader_guard.cpp - A material without one of its mesh shaders draws
// with that shader of the default material instead of crashing the render
// thread.
//
// Every drawing policy constructor (TEmissiveDrawingPolicy<...> in
// sub_82598730, the base pass ones like sub_826BEDA0, ...) takes its shaders
// from the material's mesh shader map for the vertex factory: sub_825BC830
// (FMaterialShaderMap, FVertexFactoryType) gives the map, sub_827D0B20 (TMap
// Find: element = next index, key, value; 12 bytes) the shader by FShaderType,
// and the result is dereferenced at once: retail materials have every shader
// the renderer asks for. Materials of the cut levels of the Oct 2007 prototype
// (work/cut_restore) lack some (TEmissive*Shader<FIrradPointLightPolicy> for
// one, guest 0x831E70AC / 0x831E7ABC) or hold NULL for them, and the render
// thread crashed with a read of guest 0 or 0x2C.
//
// A lookup in a mesh shader map that finds nothing (or NULL) is answered with
// the same shader type from GEngine.DefaultMaterial's mesh shader map for the
// same vertex factory (it is cooked for every vertex factory and light map
// policy, and never unloaded), or, for the shaders an opaque material has
// none of (translucency, distortion), from a material of an always loaded
// 00_* package that has it, else from any loaded material that has it.
// Without one there either, the shader comes out NULL and a policy
// without shaders gets no bound shader state (sub_826BBB78, sub_825928D8), and
// the irradiance point light and distortion ones are not drawn (sub_82594170 /
// sub_82594220, sub_82592838 / sub_82592690) nor added to the static draw list
// (sub_825ABA70).
#include "log_files.h"
#include "ue3_reflect.h"

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(ao2_missing_shader_guard, true, "Debug",
                    "A material without one of its shaders (prototype levels) uses that shader of the default "
                    "material instead of crashing the render thread");

namespace
{
  constexpr uint32_t kVertexShader = 16; // drawing policy +16, +20: the two shaders
  constexpr uint32_t kPixelShader = 20;
  constexpr uint32_t kEngine = 0x831EFCBC;      // GEngine
  constexpr uint32_t kDefaultMaterial = 216;    // Engine.DefaultMaterial
  constexpr uint32_t kMaterialResource = 536;   // UMaterial -> FMaterialResource
  constexpr uint32_t kShaderMap = 48;           // FMaterial -> FMaterialShaderMap
  constexpr uint32_t kMeshShaderMaps = 112;     // FMaterialShaderMap: TMap<FVertexFactoryType*, mesh shader map*>
  constexpr uint32_t kOuter = 40;               // UObject
  constexpr uint32_t kClass = 52;
  constexpr uint32_t kObjectIndex = 4;

  thread_local uint32_t t_mesh_map = 0; // the last mesh shader map sub_825BC830 returned
  thread_local uint32_t t_factory = 0;  // and its vertex factory type

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
  bool HasShaders(const uint8_t *base, uint32_t policy)
  {
    return LoadU32(base, policy + kVertexShader) && LoadU32(base, policy + kPixelShader);
  }

  // TMap<pointer, pointer> lookup (sub_827D0B20 / sub_827D0BB8) on the host:
  // the value or 0.
  uint32_t Find(const uint8_t *base, uint32_t map, uint32_t key)
  {
    uint32_t data = LoadU32(base, map), num = LoadU32(base, map + 4);
    uint32_t hash = LoadU32(base, map + 12), hash_size = LoadU32(base, map + 16);
    if (!Plausible(data) || !Plausible(hash) || num == 0 || num > 4096 || hash_size == 0 || hash_size > 4096 ||
        (hash_size & (hash_size - 1)))
    {
      return 0;
    }
    uint32_t id = LoadU32(base, hash + (LoadU32(base, key + 4) & (hash_size - 1)) * 4);
    for (int steps = 0; id != 0xFFFFFFFF && id < num && steps < 64; ++steps)
    {
      uint32_t element = data + id * 12;
      if (LoadU32(base, element + 4) == key)
      {
        return LoadU32(base, element + 8);
      }
      id = LoadU32(base, element);
    }
    return 0;
  }

  // A UMaterial's shader of `type` for `factory`, 0 if none.
  uint32_t MaterialShader(const uint8_t *base, uint32_t material, uint32_t factory, uint32_t type)
  {
    uint32_t resource = Plausible(material) ? LoadU32(base, material + kMaterialResource) : 0;
    uint32_t shader_map = Plausible(resource) ? LoadU32(base, resource + kShaderMap) : 0;
    uint32_t mesh_map = Plausible(shader_map) ? Find(base, shader_map + kMeshShaderMaps, factory) : 0;
    uint32_t shader = Plausible(mesh_map) ? Find(base, mesh_map, type) : 0;
    return Plausible(shader) ? shader : 0;
  }

  bool LiveObject(const uint8_t *base, uint32_t object)
  {
    uint32_t index = LoadU32(base, object + kObjectIndex);
    return Plausible(object) && index < ao2::ue3::ObjectCount(base) && ao2::ue3::ObjectAt(base, index) == object;
  }

  // A material with a shader of `type` for `factory`: one of an always loaded
  // 00_* package if there is one, else any loaded one; 0 if none. One scan of
  // every object per pair, again when the material found is gone or, after
  // none was found, 5 s later.
  uint32_t DonorMaterial(const uint8_t *base, uint32_t factory, uint32_t type)
  {
    struct Found
    {
      uint32_t material = 0;
      std::chrono::steady_clock::time_point at;
    };
    static std::mutex lock;
    static std::map<std::pair<uint32_t, uint32_t>, Found> found;
    static uint32_t material_class = ao2::ue3::FindClass(base, "Material");
    std::lock_guard guard(lock);
    auto now = std::chrono::steady_clock::now();
    auto [it, added] = found.try_emplace({factory, type});
    Found &f = it->second;
    if (!added && (f.material ? LiveObject(base, f.material) && LoadU32(base, f.material + kClass) == material_class
                              : now - f.at < std::chrono::seconds(5)))
    {
      return f.material;
    }
    f = {0, now};
    for (uint32_t i = 0, n = material_class ? ao2::ue3::ObjectCount(base) : 0; i < n; ++i)
    {
      uint32_t o = ao2::ue3::ObjectAt(base, i);
      if (!o || LoadU32(base, o + kClass) != material_class || !MaterialShader(base, o, factory, type))
      {
        continue;
      }
      uint32_t package = o;
      while (Plausible(LoadU32(base, package + kOuter)))
      {
        package = LoadU32(base, package + kOuter);
      }
      bool resident = ao2::ue3::ObjectName(base, package).starts_with("00_");
      if (resident || !f.material)
      {
        f.material = o;
      }
      if (resident)
      {
        break;
      }
    }
    return f.material;
  }

  uint32_t Donor(const uint8_t *base, uint32_t factory, uint32_t type)
  {
    uint32_t engine = LoadU32(base, kEngine);
    uint32_t material = Plausible(engine) ? LoadU32(base, engine + kDefaultMaterial) : 0;
    if (uint32_t shader = MaterialShader(base, material, factory, type))
    {
      return shader;
    }
    return MaterialShader(base, DonorMaterial(base, factory, type), factory, type);
  }

  // Once per shader type, vertex factory and outcome.
  void Log(uint32_t type, uint32_t factory, uint32_t donor)
  {
    static std::mutex lock;
    static std::set<std::tuple<uint32_t, uint32_t, bool>> logged;
    {
      std::lock_guard guard(lock);
      if (!logged.insert({type, factory, donor != 0}).second)
      {
        return;
      }
    }
    if (FILE *f = ao2::OpenLogFile("missing_shader_guard.log", "a"))
    {
      std::fprintf(f, "shader type %08X (vertex factory type %08X) missing: %s %08X\n", type, factory,
                   donor ? "uses the donor material's" : "none found, pass skipped", donor);
      std::fclose(f);
    }
  }
} // namespace

extern "C" REX_FUNC(__imp__sub_825BC830);
extern "C" REX_FUNC(sub_825BC830)
{
  uint32_t factory = ctx.r4.u32;
  __imp__sub_825BC830(ctx, base);
  t_mesh_map = ctx.r3.u32;
  t_factory = factory;
}

extern "C" REX_FUNC(__imp__sub_827D0B20);
extern "C" REX_FUNC(sub_827D0B20)
{
  uint32_t map = ctx.r3.u32, key = ctx.r4.u32;
  __imp__sub_827D0B20(ctx, base);
  if (map != t_mesh_map || !map || (ctx.r3.u32 && LoadU32(base, ctx.r3.u32)) || !REXCVAR_GET(ao2_missing_shader_guard))
  {
    return;
  }
  // The caller loads the shader from the returned pointer at once: hand it a
  // word in free stack below its frame.
  uint32_t type = LoadU32(base, key);
  uint32_t donor = type && t_factory ? Donor(base, t_factory, type) : 0; // both in the image, not the heap
  uint32_t slot = (ctx.r1.u32 - 0x400) & ~3u;
  uint32_t value = std::byteswap(donor);
  std::memcpy(base + slot, &value, 4);
  ctx.r3.u64 = slot;
  Log(type, t_factory, donor);
}

// Bound shader state of a policy (out r3, policy r4): sub_826BBB78 for most
// policies, sub_825928D8 for TDistortionMeshDrawingPolicy.
static bool SkipBoundState(PPCContext &ctx, uint8_t *base)
{
  if (!REXCVAR_GET(ao2_missing_shader_guard) || HasShaders(base, ctx.r4.u32))
  {
    return false;
  }
  std::memset(base + ctx.r3.u32, 0, 16); // released by sub_82563F90, which skips NULL
  return true;
}

extern "C" REX_FUNC(__imp__sub_826BBB78);
extern "C" REX_FUNC(sub_826BBB78)
{
  if (!SkipBoundState(ctx, base))
  {
    __imp__sub_826BBB78(ctx, base);
  }
}

extern "C" REX_FUNC(__imp__sub_825928D8);
extern "C" REX_FUNC(sub_825928D8)
{
  if (!SkipBoundState(ctx, base))
  {
    __imp__sub_825928D8(ctx, base);
  }
}

// The distortion policy's draw steps (policy in r3).
extern "C" REX_FUNC(__imp__sub_82592838);
extern "C" REX_FUNC(sub_82592838)
{
  if (HasShaders(base, ctx.r3.u32))
  {
    __imp__sub_82592838(ctx, base);
  }
}

extern "C" REX_FUNC(__imp__sub_82592690);
extern "C" REX_FUNC(sub_82592690)
{
  if (HasShaders(base, ctx.r3.u32))
  {
    __imp__sub_82592690(ctx, base);
  }
}

extern "C" REX_FUNC(__imp__sub_82594170);
extern "C" REX_FUNC(sub_82594170)
{
  if (HasShaders(base, ctx.r3.u32))
  {
    __imp__sub_82594170(ctx, base);
  }
}

extern "C" REX_FUNC(__imp__sub_82594220);
extern "C" REX_FUNC(sub_82594220)
{
  if (HasShaders(base, ctx.r3.u32))
  {
    __imp__sub_82594220(ctx, base);
  }
}

extern "C" REX_FUNC(__imp__sub_825ABA70);
extern "C" REX_FUNC(sub_825ABA70)
{
  if (HasShaders(base, ctx.r6.u32))
  {
    __imp__sub_825ABA70(ctx, base);
  }
}
