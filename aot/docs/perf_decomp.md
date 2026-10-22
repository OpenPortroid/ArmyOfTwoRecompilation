# Performance from the decompiled game (2026-10-21..22, branch perf/decomp-findings)

Scene: China's river, checkpoint 07_011, the autopilot drives the hovercraft to the dock (`work/boat_fps.sh`), 1440p
(1280x720 x scale 2), AO off, decoupled at 60 steps/s, this PC (Ryzen 5 8400F + RTX 2060). Profiles: `tools/sampler.py`
on one thread for 20 s of the drive (`PROF=<thread>` in `boat_fps.sh`).

## How a second is spent there

With `ao2_decouple` the game thread runs 60 simulation steps a second (the engine tick, ~5-6 ms each) and draw-only
frames in between (0.14 ms of game thread each). A draw-only frame is limited by the GPU (~4.6 ms: draw 3.0, resolve
0.9, transfer 0.3), a simulation step by the tick, during which only one frame is drawn. Roughly
FPS = 60 + (1000 - 60 x tick) / 4.65: one millisecond of tick is worth ~13 FPS here.

## Done: the heap lock (`heap_lock.cpp`, `ao2_heap_host_lock`, on)

GMalloc is UE3's thread-safe proxy (vtable 0x8206E6A8, built by sub_8254A1C8): every method enters one critical
section (made without a spin count) around the real allocator. The runtime's critical section hands the lock to a
sleeping waiter, so the game thread and the rendering thread formed a lock convoy: 27% of the game thread was in the
heap, nearly all asleep in RtlEnterCriticalSection -> xeKeWaitForSingleObject, 22% of it under the particle data
manager (sub_825FF2F8 builds every particle system's dynamic data each tick, the rendering thread frees the last one).
The proxy's methods take a host SRW lock now (recursive): mutual exclusion as before, no change to game state.

| river, 2 runs each | off | on |
|---|---|---|
| engine tick avg | 6.20 / 6.27 ms | 5.43 / 5.38 ms |
| tick p50 / p90 | 6.01 / 8.48 | 5.45 / 6.79 |
| game thread load | 64% | 50% |
| FPS | 183.9 / 188.6 | 188.7 / 189.1 |

FPS moves little because the draw-only frames are GPU-bound on this PC; the tick gain shows on a faster GPU or with
`ao2_sim_hz` higher. Unlike the spin of 8edda88 it touches one lock only, never burns a core, and needs no CPU count
heuristic.

## Tried and dropped: unseen particle systems ticking every 4th step

UParticleSystemComponent::Tick (sub_8283BA58) of this build has no SecondsBeforeInactive test (AccumTickTime +744
grows right away). Throttling systems with LastRenderTime (+624) older than 0.5 s skipped 45-70% of the particle
system ticks but changed nothing (tick 5.10 vs 5.10 ms): the unseen ones are inactive and return early anyway; the
cost is in the visible ones. Code kept outside the repo (ao2decomp/attempts).

## What the game thread does now (river, heap lock on)

| | % of thread | functions |
|---|---|---|
| actor ticks | 30 | sub_827B1AA8 / sub_8279CEE0 |
| UnrealScript (actors' Tick events mostly) | 17.5 | sub_8243EC10 -> sub_82445C30 -> sub_82447A40 |
| component tick groups | 14 | sub_827A6410 (particle system tick 8.3: emitters sub_825FC450 6) |
| rigid body actors -> MoveActor | 11.7 | AActor::SyncActorToRBPhysics sub_82830540 (57 AO2KActor floating props + 3 hovercraft, through vtable +600: KActor sub_82845E38, vehicles sub_8239F1D8 -> sub_82845388) -> UWorld::MoveActor sub_827B0698 / sub_82792280 |
| of that: encroachment check over the octree | 5.7 | FPrimitiveOctree::ActorEncroachmentCheck sub_8279B570 -> node walk sub_82799D90 -> primitive checks sub_8266B610 (convex support sub_82813CD8, 3%) |
| component transform updates | 8.6 | sub_826787B8 (pawn UpdateComponents sub_822A0C78 -> sub_82681180: 5%) |
| asleep | 19 | render fence 4.7, frame limiter 4.5, physics wait 2.4 |

The profile is flat: no single function above 3% of self time. What is left in the tick is game logic whose results
the co-op lockstep compares (touch / encroach events, animation, script), so it can only be made faster with
bit-identical replacements, a few percent each.

## Rendering thread and GPU command thread (river)

- UE3 rendering thread: 32% asleep waiting for occlusion query results (sub_82AA14D0, the console behaviour, see
  ao2_occlusion_wait history), 21% in the scene's draw lists. It waits because the GPU is the limit, not the other way.
- Native GPU commands: 35% asleep; not the limit.
- GPU frame: scene color resolve 0x109D0000 (draws 110 / 168: 162 + 136 us at 2560x1440, bandwidth bound), depth
  resolve 0x11108000 74 us + depth restore 93 us, the water draws (vs 1B8BBD77 / ps 9C6E41D3) 340 us.

## Notes

- `IsChapterUnlocked` (UAO2ProfileSettings vtable +364, sub_8232EAE0) is a stub returning `(1 << n) & 1`, and
  `ReallyUnlockChapter` (+360) returns 0: the co-op map list comes from UIDataProvider_AO2Maps.bUnlocked (+168),
  set natively. For two-instance tests on Iraq, ao2decomp/scripts/unlock_maps.py keeps it set in both games' memory.
- appSRand (particles, decals) is machine-local in a network game (coop_sync_trace.cpp); rand() is the shared stream.

## Co-op check of the heap lock (2026-10-22)

Two instances on this PC (`tools/aot.py coop --map somalia`, then `console.txt` `checkpoint 05_00 0` loads Iraq on
both through the lockstep), `--ao2_coop_sync_trace=true`, heap lock on, both players standing: 11700 common lockstep
frames (~6.5 min) from Iraq's start (checksum 4AE1), `tools/sync_diff.py`: no difference in checksums, rand() state,
players or pawns, no Connection Lost. Only frame 0 counts rand() calls differently (9 / 7) with the same rand() state
(the level start). Not covered: a fight, two PCs.

## GPU side (branch perf/render-thread, 2026-10-22)

Both only change how the native renderer does the same EDRAM work; game state is untouched, so co-op is unaffected.

- `ao2_native_gpu_alias_stencil_clear` (on): D3D clears the stencil by drawing a rect list into a 4x MSAA alias of
  the scene depth. When every tile is owned by one depth target of the same base, format and pitch, the clear goes
  straight to that target (ClearDepthStencilView STENCIL) instead of a transfer into the alias and back. River:
  200 -> 209.5 FPS, transfer 0.58 -> 0.31 ms/frame.
- `ao2_native_gpu_skip_depth_restore` (on): the scene depth resolve clears depth, and the game redraws that depth
  from the resolved texture right away. The depth clear is left pending and skipped with the restore draw. River,
  2x2: 211.5 / 210.3 -> 213.3 / 211.8 FPS, draw 3.10 -> 3.05 ms; one restore skipped per frame.
- Tried, not kept: the scene color has the same resolve (exp bias -2, clear) + restore (PS D5EE530D, x4 on RGB only)
  pair. Skipping both gave +3.5 / +2.6 FPS on the river, but the scene color's alpha holds the scene depth (sky =
  fp16 max), the resolve scales alpha too and the restore does not undo it: skipping leaves alpha 4x larger, and the
  bloom downsample (PS 13A2D3FE -> tonemap 7A12CB96 multiplies by it) and depth of field read it. Doing it faithfully
  needs an alpha pass about as costly as the restore. Patch in ao2decomp/attempts/color_restore.
- `ao2_native_gpu_finite_mul` (on): the translator emitted Shader Model 3's 0 * x = 0 (`xe_mul`: abs, min, compare,
  select, and no mul + add fusing) for every multiplication, a third of the river's ground/water shader 9C6E41D3
  (201 -> 141 us with plain multiplication). It now tracks which register components may hold infinity or NaN
  (sources: rcp, rsq, log, exp, sqrt, vertex fetch, relative addressing; constants, interpolators and texture
  fetches count as finite; shaders with loops keep xe_mul everywhere) and multiplies the rest as IEEE - the same
  result. Water shader: 98 -> 44 xe_mul calls; all 1681 dumped shaders compile (xenos_hlsl_test). River, 2x2: GPU
  draw 3.06 / 3.07 -> 2.93 / 2.96 ms; screenshots: no new black/NaN pixels, static parts identical.
- Texture fetch offsets: each 2D fetch worked out the guest texture size per pixel (scale arithmetic and two
  divisions); FillSystemConstants now gives 1 / guest size per texture (xe_tex_rcp). River, 2 runs: draw 2.93 /
  2.96 -> 2.91 / 2.91 ms.
- River screenshots on/off (07_011, standing, WAIT=60 so the capture is not the loading screen): the same image
  apart from edges (the camera's idle sway between runs), the same count of black pixels.
- `ao2_native_gpu_shader_override_dir`: hand-written HLSL by the dump's names, for experiments.
