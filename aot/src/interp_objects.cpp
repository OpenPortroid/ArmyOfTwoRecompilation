// interp_objects.cpp - Moving objects drawn between simulation steps.
//
// With ao2_decouple (decouple.cpp) the world changes only on simulation
// steps and the camera is drawn between the last two of them
// (interp_camera.cpp). Objects are placed the same way, so that what moves
// with the camera (the player's character, the partner, vehicles) does not
// shake against it.
//
// The rendering thread gets an object's place only through the
// UpdateTransform render command (vtable 0x8208629C, enqueued by
// FScene::UpdatePrimitiveTransform sub_8262F3E8). Its Execute sub_82643290
// (r3 = command: +0 FPrimitiveSceneInfo, +16 LocalToWorld, +80 WorldToLocal,
// +144 a third matrix, +208 bounds, +236 determinant) calls the proxy's
// SetTransform sub_825ECBE8, which keeps them in the proxy ([info +8]):
// +32 LocalToWorld, +96 WorldToLocal, +160 the third matrix, +224 the
// determinant, then calls the proxy's OnTransformChanged (vtable +68).
//
// A frame's scene is drawn on the rendering thread by a command that
// BeginRenderingViewFamily sub_8262FE08 (game thread) enqueues, one per view
// family: Execute -> sub_8262FC98 (r3 = FSceneRenderer) or sub_8262FC38
// (r3 = the command). Render commands run in order, so a host queue next to
// them carries each draw's step and alpha (decouple Alpha) from the game
// thread to the rendering thread: before the scene is drawn, every object
// that moved on that step gets its matrices placed between the two steps'
// (componentwise - one step is a small move), after it the step's own.
//
// An object's last place is checked before anything is written (the proxy
// may have been deleted since), jumps are not interpolated, and bounds are
// left alone (culling uses the step's place).
//
// A freed object still holds its last place until the memory is reused, so
// that check alone let render-only frames write into freed blocks and break
// the allocator's free lists (TU2: Malloc returned unmapped addresses after
// opening Select Mission). GMalloc's Free and Realloc are watched: an object
// whose proxy, scene info, mesh object or bone array is freed or moved is
// forgotten before anything is written again.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "decouple.h"
#include "heap_lock.h"
#include "log_files.h"

REXCVAR_DEFINE_BOOL(ao2_interp_objects, true, "Gameplay",
                    "With ao2_decouple: draw moving objects between simulation steps (with the camera)");
REXCVAR_DEFINE_BOOL(ao2_interp_objects_debug, false, "Debug",
                    "Log object interpolation statistics once a second to interp_objects.log");

namespace
{
  constexpr uint32_t kProxyMatrices = 32; // LocalToWorld, WorldToLocal, third: 3 x 64 bytes
  constexpr uint32_t kProxyDet = 224;
  constexpr uint32_t kProxyOnTransformChanged = 68; // vtable offset
  constexpr uint32_t kInfoProxy = 8;
  constexpr int kFloats = 48; // 3 matrices
  // Farther than this between two steps: a jump, not a move.
  constexpr float kMaxMove = 1500.0f; // world units

  float LoadF32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::bit_cast<float>(std::byteswap(v));
  }
  void StoreF32(uint8_t *base, uint32_t addr, float f)
  {
    uint32_t v = std::byteswap(std::bit_cast<uint32_t>(f));
    std::memcpy(base + addr, &v, 4);
  }
  uint32_t LoadU32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return std::byteswap(v);
  }

  struct Place
  {
    float m[kFloats];
    float det;

    void Read(const uint8_t *base, uint32_t proxy)
    {
      for (int i = 0; i < kFloats; i++)
      {
        m[i] = LoadF32(base, proxy + kProxyMatrices + 4 * i);
      }
      det = LoadF32(base, proxy + kProxyDet);
    }
    void Write(uint8_t *base, uint32_t proxy) const
    {
      for (int i = 0; i < kFloats; i++)
      {
        StoreF32(base, proxy + kProxyMatrices + 4 * i, m[i]);
      }
      StoreF32(base, proxy + kProxyDet, det);
    }
    bool Is(const uint8_t *base, uint32_t proxy) const
    {
      // LocalToWorld is enough to tell.
      for (int i = 0; i < 16; i++)
      {
        if (std::bit_cast<uint32_t>(LoadF32(base, proxy + kProxyMatrices + 4 * i)) != std::bit_cast<uint32_t>(m[i]))
        {
          return false;
        }
      }
      return true;
    }
    float Distance(const Place &o) const
    {
      float dx = m[12] - o.m[12], dy = m[13] - o.m[13], dz = m[14] - o.m[14];
      return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
  };

  // Rendering-thread state.
  struct Track
  {
    uint32_t info = 0;
    uint32_t step = 0; // the step cur was set on
    bool has_prev = false;
    Place prev, cur;
  };
  std::unordered_map<uint32_t, Track> g_tracks; // by proxy
  uint32_t g_rt_step = 0;                       // step of the last scene drawn
  std::vector<uint32_t> g_written;              // proxies holding an in-between place now

  // A GPU-skinned mesh object's bone matrices (3x4, 12 floats each) as its
  // vertex factories hold them, on the last two steps.
  struct Skin
  {
    uint32_t step = 0;
    bool has_prev = false;
    std::vector<uint32_t> arrays; // address, count pairs
    std::vector<float> prev, cur;
  };
  std::unordered_map<uint32_t, Skin> g_skins; // by mesh object
  std::vector<uint32_t> g_written_skins;

  // The tracks and skins above (rendering thread) and the guest allocations
  // they point at, against GMalloc's Free / Realloc on any thread.
  std::recursive_mutex g_mem;
  std::unordered_set<uint32_t> g_watched; // allocations the entries use
  std::unordered_set<uint32_t> g_freed;   // of those, freed or moved since

  // GMalloc's Free / Realloc run thousands of times a frame on every thread;
  // taking g_mem on each one cost a lot of frame time in fights. A bit per
  // hash of each watched address lets the frees of other blocks (nearly all)
  // return without the lock. Two filters: each new step's set is built into
  // the idle one and then published, so a still-watched address never reads
  // as clear.
  constexpr uint32_t kFilterBits = 18;
  constexpr uint32_t kFilterWords = (1u << kFilterBits) / 64;
  std::atomic<uint64_t> g_filter[2][kFilterWords];
  std::atomic<uint32_t> g_filter_active{0};

  uint32_t FilterBit(uint32_t address)
  {
    return (address * 0x9E3779B1u) >> (32 - kFilterBits);
  }
  void FilterSet(uint32_t filter, uint32_t address)
  {
    uint32_t bit = FilterBit(address);
    g_filter[filter][bit / 64].fetch_or(uint64_t(1) << (bit % 64), std::memory_order_seq_cst);
  }
  bool FilterHas(uint32_t address)
  {
    uint32_t bit = FilterBit(address);
    uint32_t filter = g_filter_active.load(std::memory_order_seq_cst);
    return (g_filter[filter][bit / 64].load(std::memory_order_seq_cst) >> (bit % 64)) & 1;
  }

  // Called with g_mem held.
  void Watch(uint32_t address)
  {
    g_watched.insert(address);
    FilterSet(g_filter_active.load(std::memory_order_relaxed), address);
  }

  // Called with g_mem held: drops the entries that use freed memory.
  void Forget()
  {
    if (g_freed.empty())
    {
      return;
    }
    auto freed = [](uint32_t a) { return g_freed.contains(a); };
    std::erase_if(g_tracks, [&](const auto &kv) { return freed(kv.first) || freed(kv.second.info); });
    std::erase_if(g_skins, [&](const auto &kv) {
      if (freed(kv.first))
      {
        return true;
      }
      for (size_t i = 0; i < kv.second.arrays.size(); i += 2)
      {
        if (freed(kv.second.arrays[i]))
        {
          return true;
        }
      }
      return false;
    });
    std::erase_if(g_written, [&](uint32_t proxy) { return !g_tracks.contains(proxy); });
    std::erase_if(g_written_skins, [&](uint32_t mesh) { return !g_skins.contains(mesh); });
    for (uint32_t a : g_freed)
    {
      g_watched.erase(a);
    }
    g_freed.clear();
  }

  // GMalloc is about to free or move the block at address.
  void NoteFreed(uint32_t address)
  {
    if (!address || !FilterHas(address))
    {
      return;
    }
    std::lock_guard lock(g_mem);
    if (g_watched.contains(address))
    {
      g_freed.insert(address);
    }
  }

  // Game thread -> rendering thread, one per enqueued scene draw.
  struct Draw
  {
    uint32_t step;
    float alpha; // < 0: no interpolation
  };
  std::mutex g_queue_mutex;
  std::deque<Draw> g_queue;

  struct Stats
  {
    double window_start = 0;
    uint32_t draws = 0, empty_queue = 0, dropped = 0, updates = 0, same_step = 0, jumps = 0, placed = 0, stale = 0,
             max_tracks = 0, skin_updates = 0, skins_posed = 0, skin_stale = 0, max_skins = 0;
  } g_stats;

  void Log(const char *fmt, ...)
  {
    static FILE *f = ao2::OpenLogFile("interp_objects.log");
    if (!f)
    {
      return;
    }
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fputc('\n', f);
    std::fflush(f);
  }

  double NowMs()
  {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
  }

  void Report()
  {
    if (!REXCVAR_GET(ao2_interp_objects_debug))
    {
      return;
    }
    double now = NowMs();
    if (g_stats.window_start == 0)
    {
      g_stats.window_start = now;
    }
    if (now - g_stats.window_start < 1000.0)
    {
      return;
    }
    Log("draws %u (no entry %u, dropped %u) | updates %u (same step %u, jumps %u) | placed %u (%.1f/draw), stale %u | "
        "tracked %u | skins: updates %u, posed %u (%.1f/draw), stale %u, tracked %u",
        g_stats.draws, g_stats.empty_queue, g_stats.dropped, g_stats.updates, g_stats.same_step, g_stats.jumps,
        g_stats.placed, g_stats.draws ? double(g_stats.placed) / g_stats.draws : 0.0, g_stats.stale,
        g_stats.max_tracks, g_stats.skin_updates, g_stats.skins_posed,
        g_stats.draws ? double(g_stats.skins_posed) / g_stats.draws : 0.0, g_stats.skin_stale, g_stats.max_skins);
    g_stats = Stats{};
    g_stats.window_start = now;
  }

  void OnTransformChanged(PPCContext &ctx, uint8_t *base, uint32_t proxy)
  {
    uint32_t fn = LoadU32(base, LoadU32(base, proxy) + kProxyOnTransformChanged);
    PPCContext saved = ctx;
    ctx.r3.u64 = proxy;
    rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
    ctx = saved;
  }

  // The bone matrix arrays UpdateDynamicData_RenderThread sub_8291B9F8 fills
  // (mesh object r3, its dynamic data at +56): LOD = [data +16]; LOD render
  // data = [mesh +28] + LOD * 396, LOD model = [[[mesh +16] +160] + LOD * 4];
  // per chunk ([model +16] of them) a vertex factory from the list at
  // render data +348 (+372 with active morphs, [data +32] > 0), and from
  // +360 (+384) as well when [mesh +24] != 0; its bones: TArray at +380.
  std::vector<uint32_t> SkinArrays(const uint8_t *base, uint32_t mesh)
  {
    std::vector<uint32_t> out;
    uint32_t data = LoadU32(base, mesh + 56);
    if (!data)
    {
      return out;
    }
    uint32_t lod = LoadU32(base, data + 16);
    bool morphs = int32_t(LoadU32(base, data + 32)) > 0;
    uint32_t render = LoadU32(base, mesh + 28) + lod * 396;
    uint32_t model = LoadU32(base, LoadU32(base, LoadU32(base, mesh + 16) + 160) + lod * 4);
    int32_t chunks = model ? int32_t(LoadU32(base, model + 16)) : 0;
    if (chunks <= 0 || chunks > 64)
    {
      return out;
    }
    auto add = [&](uint32_t list_offset) {
      uint32_t list = LoadU32(base, render + list_offset);
      for (int32_t c = 0; list && c < chunks; c++)
      {
        uint32_t factory = LoadU32(base, list + 4 * c);
        if (!factory)
        {
          continue;
        }
        uint32_t bones = LoadU32(base, factory + 380);
        int32_t count = int32_t(LoadU32(base, factory + 384));
        if (bones && count > 0 && count <= 256)
        {
          out.push_back(bones);
          out.push_back(uint32_t(count));
        }
      }
    };
    add(morphs ? 372 : 348);
    if (LoadU32(base, mesh + 24))
    {
      add(morphs ? 384 : 360);
    }
    return out;
  }

  void ReadSkin(const uint8_t *base, const std::vector<uint32_t> &arrays, std::vector<float> &out)
  {
    out.clear();
    for (size_t i = 0; i < arrays.size(); i += 2)
    {
      for (uint32_t f = 0; f < arrays[i + 1] * 12; f++)
      {
        out.push_back(LoadF32(base, arrays[i] + 4 * f));
      }
    }
  }

  void WriteSkin(uint8_t *base, const std::vector<uint32_t> &arrays, const std::vector<float> &v)
  {
    size_t k = 0;
    for (size_t i = 0; i < arrays.size(); i += 2)
    {
      for (uint32_t f = 0; f < arrays[i + 1] * 12; f++)
      {
        StoreF32(base, arrays[i] + 4 * f, v[k++]);
      }
    }
  }

  bool SkinIs(const uint8_t *base, const std::vector<uint32_t> &arrays, const std::vector<float> &v)
  {
    size_t k = 0;
    for (size_t i = 0; i < arrays.size(); i += 2)
    {
      for (uint32_t f = 0; f < arrays[i + 1] * 12; f++)
      {
        if (std::bit_cast<uint32_t>(LoadF32(base, arrays[i] + 4 * f)) != std::bit_cast<uint32_t>(v[k++]))
        {
          return false;
        }
      }
    }
    return true;
  }

  // Puts back the step's own place of the objects drawn in between.
  void PutBack(PPCContext &ctx, uint8_t *base)
  {
    std::lock_guard lock(g_mem);
    Forget();
    for (uint32_t mesh : g_written_skins)
    {
      auto it = g_skins.find(mesh);
      if (it != g_skins.end())
      {
        WriteSkin(base, it->second.arrays, it->second.cur);
      }
    }
    g_written_skins.clear();
    for (uint32_t proxy : g_written)
    {
      auto it = g_tracks.find(proxy);
      if (it == g_tracks.end())
      {
        continue;
      }
      it->second.cur.Write(base, proxy);
      OnTransformChanged(ctx, base, proxy);
    }
    g_written.clear();
  }

  // Before a scene draw on the rendering thread.
  void BeforeScene(PPCContext &ctx, uint8_t *base)
  {
    Draw d{0, -1.0f};
    {
      std::lock_guard lock(g_queue_mutex);
      // More than a couple waiting: some enqueue did not happen; keep the latest.
      while (g_queue.size() > 2)
      {
        g_queue.pop_front();
        g_stats.dropped++;
      }
      if (!g_queue.empty())
      {
        d = g_queue.front();
        g_queue.pop_front();
      }
      else
      {
        g_stats.empty_queue++;
      }
    }
    std::lock_guard lock(g_mem);
    Forget();
    g_stats.draws++;
    g_stats.max_tracks = std::max<uint32_t>(g_stats.max_tracks, uint32_t(g_tracks.size()));
    g_stats.max_skins = std::max<uint32_t>(g_stats.max_skins, uint32_t(g_skins.size()));
    if (d.alpha < 0)
    {
      return;
    }
    if (d.step != g_rt_step)
    {
      g_rt_step = d.step;
      // Forget objects that have not moved for a while.
      if (g_tracks.size() > 4096)
      {
        std::erase_if(g_tracks, [](const auto &kv) { return kv.second.step + 2 < g_rt_step; });
      }
      if (g_skins.size() > 512)
      {
        std::erase_if(g_skins, [](const auto &kv) { return kv.second.step + 2 < g_rt_step; });
      }
      // Only what the entries left still use, into the idle filter, then
      // that filter goes live.
      uint32_t idle = g_filter_active.load(std::memory_order_relaxed) ^ 1;
      for (std::atomic<uint64_t> &w : g_filter[idle])
      {
        w.store(0, std::memory_order_relaxed);
      }
      g_watched.clear();
      auto watch = [&](uint32_t address) {
        g_watched.insert(address);
        FilterSet(idle, address);
      };
      for (const auto &[proxy, t] : g_tracks)
      {
        watch(proxy);
        watch(t.info);
      }
      for (const auto &[mesh, s] : g_skins)
      {
        watch(mesh);
        for (size_t i = 0; i < s.arrays.size(); i += 2)
        {
          watch(s.arrays[i]);
        }
      }
      g_filter_active.store(idle, std::memory_order_seq_cst);
    }
    if (d.alpha >= 1.0f)
    {
      return;
    }
    for (auto &[proxy, t] : g_tracks)
    {
      if (t.step != d.step || !t.has_prev)
      {
        continue;
      }
      if (LoadU32(base, t.info + kInfoProxy) != proxy || !t.cur.Is(base, proxy))
      {
        g_stats.stale++;
        t.has_prev = false;
        continue;
      }
      Place p;
      for (int i = 0; i < kFloats; i++)
      {
        p.m[i] = t.prev.m[i] + (t.cur.m[i] - t.prev.m[i]) * d.alpha;
      }
      p.det = t.prev.det + (t.cur.det - t.prev.det) * d.alpha;
      p.Write(base, proxy);
      OnTransformChanged(ctx, base, proxy);
      g_written.push_back(proxy);
      g_stats.placed++;
    }
    std::vector<float> pose;
    for (auto &[mesh, s] : g_skins)
    {
      if (s.step != d.step || !s.has_prev)
      {
        continue;
      }
      if (!SkinIs(base, s.arrays, s.cur))
      {
        g_stats.skin_stale++;
        s.has_prev = false;
        continue;
      }
      pose.resize(s.cur.size());
      for (size_t i = 0; i < pose.size(); i++)
      {
        pose[i] = s.prev[i] + (s.cur[i] - s.prev[i]) * d.alpha;
      }
      WriteSkin(base, s.arrays, pose);
      g_written_skins.push_back(mesh);
      g_stats.skins_posed++;
    }
  }
} // namespace

// BeginRenderingViewFamily (game thread): one scene draw is enqueued.
extern "C" REX_FUNC(__imp__sub_8262FE08);
extern "C" REX_FUNC(sub_8262FE08)
{
  Draw d{0, -1.0f};
  if (REXCVAR_GET(ao2_interp_objects) && ao2::decouple::Active())
  {
    d.step = ao2::decouple::WorldStep();
    d.alpha = float(ao2::decouple::Alpha());
  }
  {
    std::lock_guard lock(g_queue_mutex);
    g_queue.push_back(d);
  }
  __imp__sub_8262FE08(ctx, base);
}

// UpdateTransform command Execute (rendering thread): remember the object's
// place on this step.
extern "C" REX_FUNC(__imp__sub_82643290);
extern "C" REX_FUNC(sub_82643290)
{
  uint32_t info = LoadU32(base, ctx.r3.u32);
  __imp__sub_82643290(ctx, base);
  uint32_t proxy = info ? LoadU32(base, info + kInfoProxy) : 0;
  if (!proxy || !REXCVAR_GET(ao2_interp_objects) || !ao2::decouple::Active())
  {
    return;
  }
  std::lock_guard lock(g_mem);
  Forget();
  g_stats.updates++;
  // Scenes of the step being simulated are drawn after this.
  uint32_t step = g_rt_step + 1;
  Place now;
  now.Read(base, proxy);
  Watch(proxy);
  Watch(info);
  auto [it, added] = g_tracks.try_emplace(proxy);
  Track &t = it->second;
  if (!added && t.info == info && t.step == step)
  {
    g_stats.same_step++;
    t.cur = now;
    return;
  }
  bool continues = !added && t.info == info && t.step + 1 == step;
  t.has_prev = continues && now.Distance(t.cur) <= kMaxMove;
  if (continues && !t.has_prev)
  {
    g_stats.jumps++;
  }
  t.prev = t.cur;
  t.cur = now;
  t.info = info;
  t.step = step;
}

// FSkeletalMeshObjectGPUSkin::UpdateDynamicData_RenderThread (r3 = mesh
// object, r4 = new dynamic data; GPU skin vtable 0x820DBD28 +32): remember
// the bone matrices it copied to the vertex factories on this step.
extern "C" REX_FUNC(__imp__sub_8291B9F8);
extern "C" REX_FUNC(sub_8291B9F8)
{
  uint32_t mesh = ctx.r3.u32;
  __imp__sub_8291B9F8(ctx, base);
  if (!REXCVAR_GET(ao2_interp_objects) || !ao2::decouple::Active())
  {
    return;
  }
  std::lock_guard lock(g_mem);
  Forget();
  g_stats.skin_updates++;
  uint32_t step = g_rt_step + 1;
  std::vector<uint32_t> arrays = SkinArrays(base, mesh);
  Watch(mesh);
  for (size_t i = 0; i < arrays.size(); i += 2)
  {
    Watch(arrays[i]);
  }
  auto [it, added] = g_skins.try_emplace(mesh);
  Skin &s = it->second;
  bool same_layout = !added && s.arrays == arrays;
  if (same_layout && s.step == step)
  {
    ReadSkin(base, arrays, s.cur);
    return;
  }
  bool continues = same_layout && s.step + 1 == step;
  s.prev.swap(s.cur);
  ReadSkin(base, arrays, s.cur);
  s.arrays = std::move(arrays);
  s.has_prev = continues && s.prev.size() == s.cur.size() && !s.cur.empty();
  s.step = step;
}

// A view family's scene (rendering thread): r3 = FSceneRenderer.
extern "C" REX_FUNC(__imp__sub_8262FC98);
extern "C" REX_FUNC(sub_8262FC98)
{
  BeforeScene(ctx, base);
  __imp__sub_8262FC98(ctx, base);
  PutBack(ctx, base);
  Report();
}

// The other scene draw command's Execute (rendering thread): r3 = command.
extern "C" REX_FUNC(__imp__sub_8262FC38);
extern "C" REX_FUNC(sub_8262FC38)
{
  BeforeScene(ctx, base);
  __imp__sub_8262FC38(ctx, base);
  PutBack(ctx, base);
  Report();
}

// GMalloc, the thread-safe proxy (its vtable at 0x8206E6A0 holds Malloc
// sub_8254A288, Realloc sub_8254A2F0, Free sub_8254A360; r4 = the block for
// both of these). A watched block is marked before the call, while nothing
// has reused it yet. The call itself goes through heap_lock.cpp.
extern "C" REX_FUNC(__imp__sub_8254A360);
extern "C" REX_FUNC(sub_8254A360)
{
  NoteFreed(ctx.r4.u32);
  ao2::heap::Method(ctx, base, 12, true, __imp__sub_8254A360);
}

extern "C" REX_FUNC(__imp__sub_8254A2F0);
extern "C" REX_FUNC(sub_8254A2F0)
{
  NoteFreed(ctx.r4.u32);
  ao2::heap::Method(ctx, base, 8, false, __imp__sub_8254A2F0);
}
