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
- `ao2_native_gpu_resolve_swap` (on), the first step of "resolve semantics" (see ao2decomp/hle-feasibility.md:
  a full D3D/RHI-level renderer would not remove the engine's resolve + clear + restore patterns): a color
  resolve followed by a clear hands the render target's resource to the texture (the exponent bias moves to
  sampling) and the render target gets the texture's previous one. Fires on the scene color resolve (1/frame
  on the river and in Iraq). River 2x2: resolve 0.855 / 0.850 -> 0.742 / 0.740 ms but draw 2.93 / 2.95 -> 3.04 /
  3.03: the frame total only 4.256 / 4.275 -> 4.247 / 4.249 ms. The reads stay (the restore reads the scene
  either way), only the copy's write goes: zero copy is worth much less here than the copies' times suggest.
- `ao2_native_gpu_shader_override_dir`: hand-written HLSL by the dump's names, for experiments.

## Native replacements of hot engine functions (2026-10-22)
Bit-exact native C++ in place of the recompiled code, checked against it call by call
(`ao2_native_*` = 2 runs both and logs mismatches).
- sub_82813CD8 (convex hull vs axis projection, collision; src/native_sat.cpp, `ao2_native_sat`): verify run on the
  river, 14.0 M calls, 0 mismatches (r3, overlap, normal). Same arithmetic order as the recompiled code (float(fma) in
  double, fsel, the unrolled loop's per-lane summation order), so co-op lockstep is unaffected.
  River with the sampler, 2 runs each: function 3.39/3.54% of the game thread -> 0.86%; tick 6.71/6.64 -> 6.48/6.42 ms
  (-3%), fps 195/196 -> 199/199 (GPU-bound). The next candidates are flatter (UnrealScript VM sub_82447A40 1.4%,
  sub_82885870 1.2%, sub_82408DF0 0.9%): about 1% each, or less.

## What limits the river at 1440p (2026-10-22, 2217668)

`work/boat_fps.sh` (river route, `SCALE=` and `FPS=` set the resolution scale and ao2_fps_target), one run each:

| run | FPS avg / min | tick | GPU ms/frame (draw / resolve / transfer) | GPU commands thread | UE3 rendering thread | game thread |
|---|---|---|---|---|---|---|
| scale 2, cap 240 | 220.6 / 186.7 | 4.91 | 4.20 (2.91 / 0.74 / 0.34) | 49% | 43% | 75% |
| scale 1, cap 240 | 223.5 / 191.8 | 4.90 | 2.03 (1.55 / 0.19 / 0.13) | 47% | 42% | 78% |
| scale 1, cap 1000 | 387.7 / 286.0 | 5.09 | 1.98 | 75% (1.9 ms/frame) | 65% (1.7) | 41% |
| scale 2, cap 1000 | 243.7 / 195.7 | 4.99 | 3.89 (2.61 / 0.74 / 0.32) | 65% (2.7) | 46% (1.9) | 40% |

- At 1440p the GPU is the limit, and it is pixels: scale 1 halves the GPU frame (draw, resolve and transfer all
  scale), while the command thread (~2 ms a frame, PM4 + state + recording) and the rendering thread (~1.8 ms)
  stay below it. Draw overhead and submission are not the limit at 1440p; they would be near 500 FPS.
- With the cap at 240 the frame limiter is the limit as well: it waits 1/240 s from the previous frame, with no
  catch-up, and the simulation step's frame takes the whole tick (~5 ms > 4.17). Hence
  FPS <= 60 + (1000 - 60 x 5.0) / 4.17 ~ 228 whatever the GPU does (scale 1 at cap 240: 223.5).
- The capped GPU frame is longer than the uncapped one (4.20 vs 3.89 ms): the GPU idles between frames.
- With the cap, GPU gains do not show in FPS (the draw-only frames already wait for the limiter): measure GPU
  changes with `FPS=1000`, and judge GPU ms/frame.

## The frame limiter catches up (`ao2_limiter_catch_up`, on)

With ao2_decouple the limiter keeps a schedule of 1 / ao2_fps_target: a frame waits until it is due, not 1 / target
after the previous frame's end, so the frames after a simulation step's (the whole tick, ~5 ms) come sooner. More
than a frame off the schedule restarts it; a wait is never shorter than a quarter frame. Only with decoupling: the
step length is fixed there, so frame times do not reach the simulation (co-op unaffected). No added latency.

| river, cap 240, 2 runs each | off | on |
|---|---|---|
| FPS avg | 221.3 / 221.3 | 230.5 / 230.8 |
| FPS median / 10th percentile of seconds | 224 / 207, 223 / 206 | 235 / 214, 235 / 214 |
| seconds at 235+ | 10% / 9% | 50% / 50% |
| tick | 4.93 / 4.90 ms | 4.97 / 4.99 ms |
| GPU ms/frame | 4.19 / 4.20 | 4.06 / 4.06 |
| present interval deviation | 0.66 / 0.66 ms | 0.65 / 0.61 ms |

## The resolves of a river frame and what they cost (2026-10-22)

`--ao2_resolve_trace=N` (d3d_trace.cpp) logs every D3D Resolve of N frames with the engine's call site (through
RHICopyToResolveTarget sub_82AA4370 or directly) in the order of the native renderer's frame trace. River at 1440p
(the window is 1152x720 here, x2): 24 resolves a frame.

| # | site | what | GPU (capture) |
|---|---|---|---|
| 0 | sub_82AA2A90 (82AA2B70) | scene color, exp bias -2, clear -> 10B40000; restored right after (PS D5EE530D, 192 us) | 169 us |
| 1 | sub_82AA2A90 (82AA2BB4) | scene depth + clear -> 111C4000 (the restore is skipped) | - |
| 2-4 | 82625F10 | 448x448 depth (shadows) | small |
| 5 | 8262E11C | scene color again, no clear (after translucency) -> 10B40000, read by the post chain | 166 us |
| 6-22 | 826139E4 | post-process chain: 296x184 ... 24x16 and the 64..8 luminance reductions | ~80 us together |
| 23 | 82AA71C4 | front buffer | 63 us |

Ceiling of the two scene color copies (standing at the river start, uncapped, a debug switch that skipped the copy,
wrong picture): base 3.746 / 3.755 ms GPU (255 FPS); without #5's copy 3.562 (-0.19); without #0's 3.487 (-0.26).

`ao2_native_gpu_resolve_swap` never fired at this window size: the surface pitch is whole tiles (1152 -> 1200), so
the right column of tiles was only partly inside the resolve and the swap refused. Tiles the rectangle touches now
count as inside (the part beyond is outside the viewport, never drawn; resolved textures are sampled with the edge
clamp of the guest size). Standing, uncapped, 2 runs each: off 3.739 / 3.746 ms GPU (255.6 / 254.8 FPS), on 3.679 /
3.687 ms (259.7 / 258.7 FPS): resolve 0.81 -> 0.70, draw +0.05. Most of the copy's ceiling stays: handing the render
target over still makes the GPU turn it into a shader resource (a decompression as costly as reading it), so a
zero copy of #5 (no clear) would gain little more. Screenshots (standing, 2 each): the swap's differ from the copy's
only at edges, like the runs with the swap among themselves (camera sway), no brightness shift (mean +0.1 / 255),
the same count of black pixels.

## Texture post-processing only when a fetch needs it (2026-10-22)

`xe_tex_post` (biased / gamma components, per fetch) tested each component's mode twice even when none had one;
one uniform test of the fetch's mode bits skips them (the same result). The river's ground/water shader 9C6E41D3 is
961 DXBC instructions for 10 fetches, most of it this per-fetch code. Standing at the river start, uncapped, window
1280x720 x2 (the desktop changed between this and the measurements above), 3 alternating runs each: GPU 3.796 /
3.783 / 3.779 -> 3.767 / 3.770 / 3.771 ms (draw 2.661 -> 2.646 avg), 252.2 -> 253.1 FPS. All 1467 dumped shaders
compile (xenos_hlsl_test --all --no-cache).

Ceiling of hand-written ground/water shaders (9C6E41D3 and 38DB42CF replaced by a constant color, wrong picture):
GPU 3.651 -> 3.395 ms, but only draw -0.14 of it; resolve -0.12 is the flat picture compressing better. A rewrite
can win at most ~0.14 ms there; the other heavy passes (restore D5EE530D, tonemap 7A12CB96, 3747E634) have 1-5
fetches over a 2304x1440 fp16 target and are bandwidth bound.

## Iraq's fight: why the frame rate drops (2026-10-22)

`work/iraq_fight.sh` = tools/route_bench.sh (05_00, god mode, autopilot up the hill into the first fight; the game
closes after it) with the sampler, thread load, tick and actor profiles in the fight; AO off, 1440p (1280x720 x2),
cap 240, catch-up limiter on.

Two different drops on the route:
- The climb (to end ~5000-2000): 166-197 FPS with the tick at 4.5-5.5 ms and the GPU at 3.9-4.6 ms/frame: the
  rendering side (UE3 rendering thread and GPU command thread, ~5.5-6 ms/frame each there, see CLAUDE.md), not the
  simulation.
- The fight: when the enemies engage (one run: 57 s of it) the engine tick goes to 9-14 ms (avg ~11) and the frame
  rate to 83-160 FPS. Decoupled at 60 steps/s, FPS ~ 60 + (1000 - 60 x tick) / GPU frame: tick 11 ms -> ~150,
  13 ms -> ~120. It is the simulation.

Where the fight's tick goes (sampler on the game thread, 75 s of the fight; actor_profile per 5 s):
- actor ticks 375 ms/s in the heavy windows (130 in a calm one): ~6 ms of every 60 Hz step. Iraqi grunts
  (AO2Char_irq05_iraqi_grunt, ~11 of them) 159 ms/s, their AI controllers (AO2Bot_Grunt) 73 ms/s, the two
  players 24-37 ms/s each: ~0.4 ms of a step per fighting enemy.
- In the tick: TickActors 57%, of it the AO2 pawns' Tick (sub_822861A8, vtable 0x82021900 +656 -> AActor::Tick
  sub_8279CEE0) 30% and the actors' script Tick events (sub_82881480 -> ProcessEvent sub_8243EC10) 21%; the
  UnrealScript VM (sub_82447A40 / sub_82445C30) ~30% across both; animation, collision, particles below that.
- Flat: no leaf above ~2% of the tick. Native rewrites of single functions give ~1% each.
- Outside the tick the game thread waits for the render fence (19.5% of samples, the draw-only frames' GPU pace)
  and the limiter (10.7%); the tick itself barely waits (1.9%).

What would move it: the game logic was made for the console's 30 steps a second and runs 60 here, so a fight costs
twice its console CPU time per second. Levers: a 30 Hz simulation with complete interpolation (particles,
material time, CPU-skinned meshes are not interpolated yet; ao2_sim_hz=30 gave ~120 -> ~155 FPS in the heavy zone
before), a faster codegen (CR emulation, byteswaps: the whole game thread), then native replacements.

**Correction (same day):** the 9-14 ms ticks above were measured with the sampler on the game thread (1000
suspensions a second) and the tick profile on; both slow the tick down. Without them (only the thread load,
interp debug log) the fight's tick is 4.2-5.2 ms per 60 Hz step and the fight 207-219 FPS; see the next section.
The per-actor shares above still hold (the same ratios), the absolute times don't.

## Fewer xe_mul: vertex formats that can't be infinite, cnd (2026-10-22)

Ceiling first (a temporary switch, wrong in the 0 * inf cases): plain multiplication instead of xe_mul in every
shader gave draw -0.12 / -0.21 ms (2 runs); with xe_tex_post off too, -0.30 ms (3 runs, 2.676 -> 2.380). The
finiteness analysis counted every vertex fetch as possibly infinite, so nearly every multiplication in the vertex
shaders was xe_mul. Normalized and integer vertex formats (8888, 2_10_10_10, 16_16, 32-bit integers...) can't hold
infinity or NaN: only the 16- and 32-bit float ones taint now. cnd's result is operand 1 or 2, so the condition's
taint no longer reaches it. Vertex shaders: 6463 -> 4537 xe_mul calls over the 1467 dumped shaders (pixel shaders
unchanged); all compile. Standing at the river start, uncapped, 3 alternating runs each: draw 2.626 / 2.638 / 2.644
-> 2.612 / 2.588 / 2.596 ms, GPU 3.759 -> 3.722 ms avg, 253.9 -> 256.1 FPS.

## 30 Hz simulation with materials and particles drawn between steps (2026-10-22)

`ao2_sim_hz=30` halves the world ticks, but drew animated materials and particles at 30 Hz. Now:
- Material time (`interp_time.cpp`, `ao2_interp_time`, on): the view family's world/real time (FSceneViewFamily ctor
  sub_826179A0, f1/f2) is the step's TimeSeconds moved on by alpha x the last step's time delta. Moved forward,
  not placed between steps, because the rendering thread writes it into LastRenderTime (sub_8261E8B8), which the
  simulation reads; it stays between the last step's TimeSeconds and the next one's. Helps 60 Hz as well.
- Sprite and SubUV particles (`interp_objects.cpp`, `ao2_interp_particles`, on): during the vertex fills
  sub_828064A0 / sub_82806A78 of a scene drawn between steps every particle's OldLocation (+0) and Location (+16)
  are moved back by (1 - alpha) of its move over the step, then put back. Layout check from the debug log: the
  move over a step is Velocity (+48) x 0.0333 s exactly (e.g. 45.27 / 1358.1). Mesh, beam and trail emitters and
  CPU-skinned meshes are still drawn at the step's place.

Iraq 05_00 route into the fight, AO off, 1440p, cap 240, no sampler, alternating runs (`work/iraq_fight/hz*_?`):

| run | avg FPS | fight (last 40 s) | fight tick / step | seconds < 200 | slowest 10 s |
|---|---|---|---|---|---|
| 60 Hz #1 | 225.3 | 206.7 | 4.45 ms | 6% | 180-198 |
| 30 Hz #1 | 231.6 | 224.7 | 4.94 ms | 2% | 181-216 |
| 60 Hz #2 | 227.1 | 218.8 | 4.19 ms | 13% | 171-188 |
| 30 Hz #2 | 228.6 | 214.1 | 5.16 ms | 2% | 192-207 |

30 Hz mostly lifts the dips (the slowest seconds +15-20 FPS, seconds below 200: 6-13% -> 2%); the average is
bounded by the GPU (~4 ms/frame) and the cap. The price: game actions answer ~8 ms later on average than at 60 Hz
and the logic runs at the console's rate (the mouse view is covered by ao2_predict_camera), so 60 Hz stays the
default.


## Cheaper object interpolation on the rendering thread (2026-10-23)

`interp_objects.cpp` placed ~110 objects and posed ~85 GPU skins (~20k bone floats) before every scene drawn between
steps and put them all back after it: 0.163 ms of the UE3 rendering thread per frame in Iraq's fight, which the game
thread then waits for (the render fence) - the fight tick was ~0.35 ms longer with it (720p, 2+2 runs). Now:
- the in-between place stays after the scene; the step's next UpdateTransform / skin update overwrites it, and only
  what does not move on the next scene's step gets its own back first (0-20 a second instead of ~100 a frame);
- a skin's pose is one fused lerp + byteswap + store loop per bone array (vectorized; the separate loops through
  StoreF32 took ~3 ns a float), and the stale check compares the first and last bone only;
- OnTransformChanged saves r3-r10, lr and ctr instead of copying the whole PPCContext twice per call.

0.163 -> ~0.075 ms per frame (skins ~0.045), no stale objects in any run. Iraq fight at 1440p, 2+2 alternating
runs (`work/iraq_fight/q_*`): with / without interpolation 227 / 211 vs 228 / 206 FPS, slowest seconds 185 / 182
vs 176 / 173 - no longer measurable against the run-to-run spread.

## Iraq's fight on the GPU: the hit effect and lent resolves (2026-10-23)

The seconds at 165-180 FPS in Iraq's fight (1440p, AO off) are GPU bound: 5.3-5.8 ms of GPU work per frame,
the GPU ~95% busy, no gaps between the frame's submits (`idle` in the `gpu ms/frame` line, now measured). They
have no more draws than the 240 FPS seconds of the same fight; what they add is smoke (soft particles 7D2EA5ED,
up to 0.26 ms), more characters (1FCCC634, 11 fetches: 0.29 ms for 11 draws, all of it texture fetches -
`ao2_native_gpu_finite_mul` off changes nothing) and the hit effect. The hit effect saves the 7e3 scene as
2_10_10_10 bits (resolve fmt 10 -> 109D0000), lets a 4x MSAA depth/stencil use that EDRAM, and draws the bits back
(PS 3150CB22): two fullscreen format transfers (3 -> 2 at the resolve, 2 -> 3 at the next draw, ~0.19 ms each)
plus the copy and the copy back, in every frame it runs.

- `ao2_native_gpu_resolve_pack`: the resolve encodes the float surface straight into the texture (resolve shader
  variant 5, the transfer's Encode7e3) and the float surface keeps the EDRAM; the copy back (a plain texel-for-texel
  copy of the saved texture, `SkipCopyBack`) gives the tiles back to the surface instead of drawing. Side effect:
  the scene keeps its 16-bit floats - the console's round trip clamped them to 7e3's 31.875, so the bloom got
  weaker in hit frames only; now it is the same as in the other frames (where our fp16 surface never clamps).
- `ao2_native_gpu_resolve_lend`: a whole-screen resolve without a clear hands the render target's resource to the
  texture like the swap (exponent bias at sampling) and the render target takes the texture's old one; its
  contents are copied back (`ReturnLent`) only if it is used while it still holds EDRAM tiles. In the frame
  the scene's second resolve (after translucency, exp bias -2) is lent and the depth/stencil clears that follow take
  its tiles, so nothing is copied back. Small surfaces are not lent: the luminance reduction draws into them right
  after each resolve, and the copy back cost as much as the copy.

Measured on one fight frame frozen (`FREEZE_AB` in work/iraq_fight.sh: at the first slow seconds the game stops,
the same frame is drawn, and a setting is switched every 8 s; tools/freeze_ab.py): pack + skip 189.2 -> 197.9
FPS (GPU 5.10 -> 4.82 ms, transfer 0.53 -> 0.22); lend 224.3 -> 231.7 FPS (GPU 4.14 -> 3.93 ms) - the earlier
guess that a zero copy of this resolve would gain little did not hold. Freeze with `FREEZE_CMD=pause` (the UE3
console command): the pause menu adds its own fullscreen transfer and resolve, and in its frame the lent surface
is needed again. Pictures (FREEZE_CAPTURE_EACH, tools/compare_captures.py): with and without lending the presented
frame is identical; two captures of the same setting differ by ~0.2-0.9% of pixels (something still animates).

Also measured on a frozen fight frame: occlusion queries on/off and `finite_mul` change nothing; static texture
post-processing is worth +4.5%; without early submits (`submit_draws 0`) the frame rate halves.

Full route, 2 runs: slowest seconds 189 189 192 194 195 / 193 194 195 196 198, minimum 189 / 193 (the same day
before: 170-183 in 5 runs).

## The scene as R11G11B10 (2026-10-23)

The console draws the scene into 7e3 (2_10_10_10_FLOAT, 32 bits); we had it as R16G16B16A16_FLOAT, twice the
bytes for every draw, blend (the smoke) and resolve of the scene. `ao2_native_gpu_scene_r11g11b10` (on) makes the
7e3 render targets R11G11B10_FLOAT and their float resolves (fmt 32, the scene copy 109D0000) a texture of the same
format, so swapping, lending and the hit effect's packing keep working. Lost: alpha and a mantissa bit (6/6/5
against 7e3's 7). Nothing reads the scene's alpha: the blends never use the destination alpha, the tonemap
(7A12CB96) fetches the copy as xyz1, the refraction (6BB12ED4) and the bloom (13A2D3FE -> 80EC273B) use its color,
and the restore (D5EE530D) only writes it back. Post effects draw in the scene's format.

Live fight A/B (the setting switched every 8 s without freezing, `FREEZE_LIVE=1`, `freeze_ab.py --live`), GPU
3.47 -> 3.11 ms/frame (5 pairs: each -0.25..-0.4 ms but one); frozen frame 3.88 -> 3.61 ms. The presented frame
(frozen, compare_captures.py): 99.3% of pixels within 1 level of 255, at most 6 levels in the bright sky and smoke;
side by side no difference visible. D24_UNORM instead of D32_FLOAT for the float depth gained ~0.1 ms (not
reliable) at the cost of depth precision - not kept.

## The command list recorded on its own thread (2026-10-23)

Profile of the GPU command thread in Iraq's fight (sampler, 20 s at the 240 cap): a third waiting for the previous
present, 35% our emulation (state, EDRAM, textures, guest memory), 30% inside the D3D12 runtime and the driver
(state changes, barriers, draws, copies, submits, descriptors) - about 45% of its busy time.

`ao2_native_gpu_deferred_list` (on, command_recorder.h): the command thread records every command list call into a
stream of small structs, and the thread "Native GPU record" makes them on the real list. In the same order go what
must stay ordered with the list: ExecuteCommandLists and fence signals on the queue, command allocator resets, the
render target / depth views (read when a command is recorded, and rewritten in place when a resolve swaps or lends a
render target's resource), the frame's shader-visible texture descriptors, and the present (the presenter is handed
the frame after its submit; the next swap waits until the replay got past it). A signal sends the stream on at
once, so waiting for a fence also waits for the replay; nothing else changed (resources were already released
through DeferRelease, upload memory and per-frame descriptors are reused after the frame's fence).

Frozen fight frame, no frame cap: at 1440p (GPU-bound, 4.16 ms/frame) 218.7 -> 221.0 FPS, the command thread
77% -> 57% of a core plus the record thread 25%; at 720p (CPU-bound) 265.2 -> 301.3 FPS (+13.6%), the command
thread 91% -> 66% plus 40%, and the game's rendering thread (88%) is the limit now. Pictures with all post effects
on: identical (0.000-0.002% of pixels, the noise floor).
