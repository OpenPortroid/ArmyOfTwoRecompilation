# Native renderer (replacing ReXGlue Xenos emulation)

Decision (2026-10-03): full native D3D12 renderer, Unleashed-Recompiled style —
hook the statically linked XDK D3D library, render with D3D12 directly,
translate Xenos shaders to HLSL. D3D12 only.

## Tools
- `tools/fnindex.py` — index of generated code (`build`, `info <addr> [--asm]`,
  `callers`, `tree`, `const`, `global`, `range`, `imports`). Uses `tools/image.bin`
  (memdump of guest 0x82000000..0x83420000) to print referenced strings.
- `tools/memdump.py <pid> 0x100000000 <guest> <size> <out>` — guest memory dump
  (virtual membase is 0x100000000).
- `tools/gen_d3d_trace.py` → `src/gpu/d3d_trace_list.inc`; `src/gpu/d3d_trace.cpp`
  wraps every D3D entry point (weak-symbol override). Run with
  `--ao2_d3d_trace=true --ao2_d3d_trace_every=300` → `d3d_trace.txt` in the game dir.
- `tools/heap40.bin` — dump of guest 0x40000000..0x40100000 in gameplay
  (contains the D3D device at 0x4004B900).

## Facts
- XDK 2.0.6534 (nov07 paths): D3D9, D3DX9, XGRAPHC statically linked.
- D3D library code: 0x82A40000–0x82A68000. PM4 is written only there
  (+ XGraphics state cache 0x82E3D7F8/9D0/BB0, never called at runtime).
- Device: 0x4004B900 (r3 of nearly all calls). Present frame end in EA layer:
  `82A584F0` (Resolve) → `82A567F0`/`82A567F8` (Swap → VdSwap).
- EA wrappers around D3D: 0x829D3000–0x829DB000; 249 engine functions call D3D directly.
- Gameplay frame: ~1800 draws, ~38 resolves, ~170 distinct D3D functions used.
- Generated functions are `weak` aliases of `__imp__sub_X` → override by
  defining `extern "C" REX_FUNC(sub_X)`.

### Perf finding
`82A433C0` = IDirect3DQuery9::GetData (type 9 occlusion, FLUSH), spun by EA
`829D49E0 GetQueryData(q, data, bWait)` — ~400k calls/frame, 1.2e9 total:
the game thread busy-waits for the GPU every frame. Native: return previous
result immediately.

## Status (2026-10-03, end of day)
Takeover mode works: rexgpu-xenos is not loaded, `NativeGraphicsSystem` + D3D12 renderer draw the
title, menus and gameplay correctly (see CLAUDE.md for the module list). Menu 60 FPS, gameplay ~43 FPS
(CPU cost of the GPU thread, ~11 us per draw).

Findings on the way (each one was a visible bug):
- D3D's CP interrupt handler (82A4FCE0) reads a callback slot that the ring fills before INTERRUPT and
  resets to 0x0BADF00D after a WAIT_REG_MEM: interrupts must run synchronously on the GPU thread and
  WAIT_REG_MEM must block -> ring is consumed by a dedicated XHostThread, like Xenia's CP.
- Host NDC Y is flipped relative to the guest (SDK passes origin_bottom_left = true).
- Per-frame texture hashing = 1 FPS; the runtime's physical memory write watches replace it.
- Overexposure: resolve `copy_dest_exp_bias` (scene resolved with -2) was ignored, and k_16_16_16_16
  render targets (-32..32 on Xenos, bloom chain) were written without the 1/32 scale while their resolved
  textures are sampled as UNORM raw bits.
- D3D clears color through an aliased depth surface (rect list, viewport = whole surface): needs EDRAM
  ownership + transfers (color<->depth tiles have swapped 40-sample halves), and the cleared extent must
  come from the rect-list vertices. Missing depth->depth transfers left the scene depth uncleared (black
  scene once color clears worked; before, stale color from the previous frame hid it).
- The 1x1 average luminance resolve is read by the CPU: small resolves are written back to guest memory.
- Exposure (2026-10-03, checked against a console frame): the CPU reads the 1x1 luminance and sets the
  tonemap `c3.y` = adapted luminance (clamped >= 0.25), `c3.z = c3.x / c3.y` (key 0.25). It was stuck at ~1 because
  (1) resolves into 16-bit normalized textures copied raw -32...32 bits, while the game writes HDR/8 into a
  k_16_16_16_16 target and multiplies the unsigned texture by 8 in the bright pass: resolves now convert to the
  destination number format (`copy_dest_number`); (2) resolved textures are pitch x height host resources but
  are sampled with normalized guest UVs (322x182 in a 352-pitch resource, a 4x4 in 32x8): `xe_tex_info.zw` =
  guest size / host size, the translator scales and edge-clamps the UVs.
- Display gamma ramp: the game writes a non-linear 256-entry `DC_LUT_30_COLOR` table (16 -> 24/1023, 128 -> 462);
  the present blit applies it (Pm4Consumer keeps the table, `gamma ramp` lines in native_gpu.log).
- Rect-list extents from vertex data are only valid for vertex shaders exporting the fetched x/y unchanged
  (D3D's clear VS); a depth restore VS turning 0..1 texcoords into NDC claimed a quarter of the screen.

## D3D function map (in progress)
| addr | per frame | guess |
|---|---|---|
| 82A567F8 | 1 | D3DDevice_Swap (VdSwap) |
| 82A584F0 | 38 | D3DDevice_Resolve |
| 82A510C0 | init | ring buffer init (ME_INIT, VdInitializeRingBuffer) |
| 82A433C0 | 400k | Query GetData |
| 82A4E660 / 82A4EB00 | 21 | DrawVertices (DRAW_INDX 1) / wrapper |
| 82A4EB48 / 82A4F088 | 336 | DrawIndexedVertices (DRAW_INDX 3) / wrapper |
| 82A4F128 | - | DrawVertices variant |
| 82A4F510 | 1110 | DrawIndexedVertices variant |
| 82A54848 / 82A54B20 | 96/140 | *UP draws (DRAW_INDX_2) |

## Resolution scale, overlay, perf (2026-10-03)
- `ao2_native_gpu_resolution_scale` (default 2): render targets and resolve destinations are S times larger;
  everything guest-side (EDRAM tiles, ownership, rect-list extents, resolve rectangles) stays in guest pixels and
  is scaled where recorded (viewport/scissor, clears, copies, transfer shader: host pixel -> guest pixel + sub-pixel).
  PS param_gen = floor(SV_Position / S). Resolved textures: `xe_tex_info.x` bits 16..23 = S so texel sizes are guest
  texels. CPU write-backs took one host pixel per guest texel (since 2026-10-11: the guest-sized copy). Since
  2026-10-11 the scale may be fractional, see "Fractional resolution scale" below.
- FPS overlay: seven-segment digits drawn by the present blit shader (`ao2_native_gpu_fps_overlay`). Replaced
  2026-10-09 by the ImGui frame rate overlay (`src/perf_overlay.cpp`, `ao2_fps_overlay` 0 off / 1 FPS / 2 detailed:
  frame times, GPU load and temperature, CPU load, VRAM, RAM; F2 = `ao2_fps_overlay_key`, also in the F1 menu). It
  is the lower layer of `overlay.h` (the F1 menu is the upper one), drawn by an ImGuiDrawer that is no input
  listener. The renderer only reports presents (`perf::OnPresent`) and its adapter LUID.
- Perf: per-frame `CreateCommittedResource` of resolve destinations (two formats/sizes alternating at 109D0000 and
  1FC09000) was ~20% of the GPU thread in kernel calls: destinations now keep spare variants per address. Draws skip
  redundant command list state; fetch constant -> texture is cached per frame; unchanged float constants reuse the
  previous upload (`RegisterFile::float_version`). Profile with `tools/sampler.py <pid> "native gpu commands"`.

## GPU load (2026-10-04)
- GPU time per category: `gpu ms/frame` stats line (timestamp queries written when the category changes,
  resolved at each command list close, read after the frame slot's fence). Traced frames also time every draw
  (`gpu_draws.txt`: totals by pixel/vertex shader, slowest draws).
- EDRAM transfers were 11 of 16 ms at 2x (depth destinations: one depth pass + 8 stencil-bit passes on 4x MSAA
  aliases). Now: no transfer for parts a draw fully overwrites in whole tiles; uniform tiles (after D3D clear
  rectangles or resolve clears) are cleared instead of transferred, value re-packed between formats; real
  transfers only over the remaining tiles; no stencil transfer into depth surfaces whose draws never enable
  stencil. Result ~1.2 ms, GPU load ~47% at 60 FPS (was 95%).
- NVIDIA driver crashes (fast fail in nvwgf2umx) on clears with many rectangles: merged and issued 8 at a time.

## Depth restore, occlusion queries (2026-10-04)
- "See-through" characters and the sun through walls: after the scene resolve (which clears depth) D3D restores
  depth with a rect list (VS 36F9E781, PS B160839A writing oDepth from the resolved 11108000). Rect lists were
  culled with whatever PA_SU_SC_MODE_CNTL the previous draws left (cull back + front CCW in some frames, and the
  rect-list GS also flipped the winding), so the restore was dropped, depth stayed 0 and everything drawn later
  passed the depth test. Rect lists now are never culled and always front facing (like the SDK's
  IsPrimitivePolygonal); the GS strip (c, a, b, f) keeps the guest winding.
- Pixel shaders writing depth get a 0..1 viewport depth range with the guest range applied in the vertex shader
  (D3D12 clamps SV_Depth to the viewport range, Xenos does not clamp oDepth).
- Resolve destination pages are not marked written when a CPU write elsewhere in their 256 KB block unwatches it.
- Occlusion queries are counted on the GPU (`ao2_native_gpu_occlusion_queries`): one D3D12 occlusion query per draw
  between the EVENT_WRITE_ZPD begin/end, sum / scale^2 written to the guest struct after the fence (0xFFFFFEED until
  then). The game waits for results, so the command thread submits the open command list mid-frame when idle or in
  WAIT_REG_MEM and publishes finished results (`RendererIdle`).
- Polygon offset (bullet hole decals z-fought): the constant part of PA_SU_POLY_OFFSET_* is an absolute depth
  offset, added in the vertex shader (ndc_offset.z += offset / viewport depth range). Host DepthBias is relative to
  the primitive's exponent with float depth, and the SDK's conversion assumes depth in [0.5, 1); with this game's
  reversed Z (walls at 0.01..0.1) it gave 8..32x too little offset. Slope -> SlopeScaledDepthBias (scale / 16 x S).
- Debugging: `ao2_native_gpu_capture_after_draws` (color before/after, depth, stencil of listed draws or VS hashes),
  `ao2_native_gpu_skip_shaders`, `ao2_native_gpu_debug_upload_vertices`; `gpu_draws.txt` lists every draw's GPU time.

## Aspect ratio (2026-10-10)
`ao2_aspect_ratio` (default `auto` = the window's client area, the monitor in fullscreen; or `16:10`, `21:9`, `1.6`, ...):
the game renders at that aspect instead of a letterboxed 16:9 (src/aspect_ratio.cpp). The engine's D3D init (TU2
sub_82AA30F8) already supports other back buffers (GScreenHeight = 720, GScreenWidth = 720 x display aspect clamped
to 900..1280, the 4:3 SD path); the size is replaced in the present parameters and GScreenWidth / GScreenHeight
(0x8318775C / 0x83187760) right before Direct3D_CreateDevice (sub_82B0FA20). Narrower than 16:9 keeps 720 lines,
wider keeps 1280 columns, so the predicated tiling still needs the same tile counts; the constant 1280x720 tiling
rectangles (0x82103118, 3 per mode, counts at 0x83187714) are rebuilt. FViewport's aspect getter (sub_82A9F540)
returns W / H instead of the stored 16/9. Hor+ (always on): FPerspectiveMatrix
(sub_823E01D0, all perspective projections) keeps the horizontal angle; for projections at the screen's aspect
wider than 16:9 the half angle becomes atan(tan(half) x aspect / (16/9)), the 16:9 vertical view. Tested: 16:10
(1152x720) and 21:9 (1280x544) menus, loading screen (letterboxed 16:9 art, by the game), gameplay and HUD.
Untested: movies (sub_8257BE38 computes their quad from 1280x720).

## Fractional resolution scale (2026-10-11)
`ao2_native_gpu_resolution_scale` is a number in steps of 0.25: 1.5 renders the game's 1280x720 commands as
1920x1080, a Full HD monitor's pixels one for one. Inside the renderer scales are in quarters (`scale4`: 4 = 1x,
6 = 1.5x, 8 = 2x).
- At any quarter scale an EDRAM tile (80x16 samples, 40x16 for 64bpp) is a whole number of host pixels, so
  ownership, uniform-tile clears and transfers stay exact per tile. The transfer shader works in host samples: the
  tile is the host sample divided by the host tile size, the position inside the tile carries over.
- Guest pixels are not all as many host pixels wide (at 1.5x: one, two, one, two...). A host pixel belongs to the
  guest pixel its centre is in - what the rasterizer covers of a guest rectangle stretched over the host surface.
  `ScaleEdge(x) = (x * scale4 + 1) >> 2` for rectangle edges (scissors, clears, copies, resolve rectangles),
  `ScaleSize` for surface sizes, `CenterPixel` for the one host pixel standing for a guest pixel (pixel probe).
  PS param_gen = floor((floor(SV_Position) * 4 + 2) / scale4).
- Small render targets (`SmallTarget`: at most `ao2_native_gpu_small_target_width` samples wide - bloom, blur, DOF,
  the luminance chain down to the 1x1 the CPU reads) take the next whole scale (2x at 1.5x). Every guest pixel of
  theirs has to be made of the same host pixels: otherwise each pass would shift the picture by a fraction of a
  pixel and the 1x1 average would weigh its texels unevenly. So a render target and a resolved texture carry their
  own scale (`RenderTarget::scale4`, `HostTexture::scale4`, a resolve destination has its source's); a transfer
  between surfaces of different scales (EDRAM aliases of dead content) takes the nearest sample.
- Resolved textures: `xe_tex_info.x` bits 16..23 = host texels per guest texel in quarters (4 for the guest-sized
  copies), `xe_resolution_scale.z` = scale4. The guest-sized copies are area-weighted (at 1.5x a guest pixel is a
  whole host pixel and half of the one it shares with its neighbour).
- CPU write-backs of scaled destinations come from the guest-sized copy (destinations up to 256x256; the host
  texel at each texel's centre otherwise). They used to take the first host texel of each guest texel, which at 2x
  is the picture sampled a quarter of a texel off: for the 1x1 average luminance a whole texel of its 4x4 source,
  so the top left host texel left out the right and bottom quarters of the screen and counted the left and top
  ones twice. In the warehouse of 05_04 the game read 0.189 where 1x gives 0.123 (the four host texels: 0.189,
  0.194, 0.053, 0.064 - their average is 0.125): the exposure followed the sky. Now 0.123 / 0.125 / 0.125 at
  1x / 1.5x / 2x. `tools` check: read the first float (big endian) at physical 1FBFB000..1FBFF000.

## Post effects, shadow resolution (2026-10-18)
Effects the console did not have, on the game's HDR scene (`post_effects.h` shaders, renderer.cpp "Post effects").
The gameplay frame (2026-10-10 trace, 21:9): opaque scene into EDRAM 544 (k_2_10_10_10_FLOAT) with depth 0
(D24FS8, reversed: viewport z scale -1 offset 1, cleared to 0) -> resolve 111D0000 (k_16_16_16_16_FLOAT, exp bias -2)
with a color clear, depth resolve 12C78000 with a depth clear -> depth restore (PS B160839A) and scene restore (draw
sampling 111D0000 into 544) -> shadow depth (below) and its projection, translucency -> the same resolve to 111D0000
-> bloom / luminance chain (small targets at 1088) -> tonemap (into 544 as k_8_8_8_8, sampling 111D0000 + the
322x138 blur + a 256x1 LUT) -> HUD -> frontbuffer.
- Camera: the base pass vertex shaders get UE3's ViewProjectionMatrix in c0-c3 (row vectors, translation in c3; the
  w column is the unit view direction) and CameraPosition in c4. The matrix most full-screen depth-writing draws use
  is the scene's (`NoteSceneCamera`). Positions are rebuilt camera-relative (the inverse's row 2 is the camera:
  P = w * (x * R0' + y * R1' + R3').xyz, w = view depth).
- Stage 1, the frame's first resolve of the whole float scene: the scene depth (still in its render target) is kept
  as view depth (R32F, host size); AO (SSAO = scalable ambient obscurance, HBAO, GTAO) at half the host resolution,
  a depth-aware blur, and a multiply into the resolved texture - the restore draws it back, translucency stays
  untouched.
- Stage 2, before the scene's last resolve is sampled into another surface (draws into the scene's own surface - the
  restore - do not count): camera motion blur (previous frame's view projection, scaled to a 1/60 s or 1/120 s
  exposure at any frame rate, nothing nearer than 250-450 units: the player's character), depth of field (far field
  only, autofocus on the screen centre, eased), FXAA (after FXAA 3.11 quality) or a morphological AA in the manner of
  SMAA 1x (luma edges with local contrast adaptation, runs followed up to 32 pixels, the silhouette rebuilt from the
  crossing edges at their ends - the area texture's L/Z/U shapes computed analytically). So bloom and tonemap work on
  the result and the HUD is untouched. Cvars: `ao2_ambient_occlusion` off/ssao/hbao/gtao, `ao2_anti_aliasing`
  off/fxaa/smaa, `ao2_motion_blur` 0-2, `ao2_depth_of_field` 0/1 (all live; anti-aliasing is also on the in-game
  Graphics page, its eighth and last row).
- MSAA is the game's own 2x, always off now: the xenia "Black Shading Fix" (game_patches.h) writes 4 into the
  predicated tiling mode index 0x831876D4 (5 modes, tile counts at 0x83187714: 4xAA 3 tiles, 2xAA 2 tiles, ...,
  4 = no AA). With MSAA on the opaque scene is drawn in two predicated tiles (rows 0-512 and
  512-720 at 16:9, the second with window offset 0,-512) and each tile is resolved with the destination base moved
  to its first row (109D0000 + 512 rows = 10ED0000; tiled textures are stored in 32-row stripes). The renderer took
  that for a texture of its own, so the lower part of the scene texture the game samples kept stale contents: the
  "dark artifacts" / black shading. Such a resolve now writes those rows of the texture the first tile was
  resolved to in the same frame (same format, pitch and height; `predicated tile` lines in a traced frame).
  Stage 1 of a tiled frame waits for the depth resolve that completes the scene depth texture (the depth render
  target only holds the last tile) and linearizes that texture instead.
- Cost at 2560x1440 (gpu ms/frame, category `post`): SSAO 0.75, GTAO 1.06, HBAO 1.42, FXAA 1.38, SMAA 1.9, motion
  blur high 1.4, depth of field ~2.7 at a stronger level since removed (24 taps; the remaining one takes 12) (each
  with the copy in and out of the scene texture).
- Debugging: `set <cvar> <value>` lines in console.txt change the cvars live (console_cmd.cpp);
  `ao2_post_debug=1` shows the occlusion instead of the scene; a traced frame captures `post_ao` (x = visibility,
  y = view depth), `post_depth`, and the scene before / after stage 2 (`post_in`, `post_out`).
- Shadow resolution (`ao2_shadow_resolution` 0 / 1024 / 2048 / 4096): UE3's shadow buffer is depth surface 1088
  (D24S8, pitch 800), each light's shadow drawn in a part of it (viewport 5,5 422x422 = resolution + SHADOW_BORDER 5)
  and resolved as depth into a 768x768 R32F texture (10EB4000). A depth surface resolved square into a square texture
  is taken for it (`NoteShadowResolve`); its render targets get a scale of their own so the buffer is at least that
  many host texels across (4096: 5.5x, 4224 texels), only as tall as it is resolved. Depth-only draws into it take that
  scale; a draw with color targets on it un-learns it. The projection samples the resolve at guest coordinates: the
  same PCF footprint over finer depth texels.
