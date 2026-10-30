# Rendering decoupled from the simulation (branch `modern_net`)

Goal: the simulation (and the online co-op lockstep) steps at a fixed rate,
the screen is drawn at the monitor's rate in between, with interpolation
(not done yet). Online co-op is a deterministic lockstep
([online_coop.md](online_coop.md)), so both machines must simulate the same
steps whatever each of them draws.

## How a frame is built

Found in the unpacked XEX image (strings of UE3 render commands, class
registration, vtables):

- Game-thread frame `sub_8247F3C0`: frame limiter `sub_8247D450` -> frame sync
  tick `sub_82964AC8` (lockstep) -> `GEngine->Tick` -> render command fence.
- `GEngine` = `[0x8311F58C]`, a `UGameEngine` (vtable `0x820B2918`); `Tick` is
  vtable +264 = `sub_826AFC78`. It pumps EA's network (`sub_829643E8`), runs
  `UWorld::Tick(LEVELTICK_All, dt)` `sub_826F7728` (`GWorld` = `[0x83122588]`;
  actors, physics, animation, cameras), ticks the game viewport client
  (`[GEngine +772]`, `sub_82754020`) and, as its last step, draws: `sub_8264FBE0`
  on `[GameViewport +68]` (views of the local players, the audio listener,
  `UGameViewportClient::Draw` `sub_827900D0`, the scene to the rendering thread).
- The rendering thread gets the simulated state only through render commands:
  `UpdateTransformCommand` (vtable `0x8207F214`) -> `sub_8257B400` (primitive
  proxy, LocalToWorld at +16, WorldToLocal +80, bounds +144, determinant +236),
  `SkelMeshObjectUpdateDataCommand` (GPU skin: `sub_8286FD68` builds the bone
  data, the command calls mesh object vtable +32), `ParticleUpdateDataCommand`
  (`sub_82765BA8`). These are where interpolation will hook in.
- The shared `rand()` the lockstep checksums (`sub_823E1BB8`, state
  `[0x830AB53C]`, shared on the main thread only) is also UnrealScript's
  `Rand()` / `FRand()` (`sub_82403180`, `sub_82404680` -> `sub_823E1CA0`), and
  the draw runs script (HUD, the camera's view point).
- Motion blur keeps a previous transform per primitive and the previous
  view-projection matrix (velocity buffer) - interpolation must keep them
  consistent.

## Stage 0: the split (`src/decouple.cpp`)

`--ao2_decouple=true --ao2_sim_hz=60` (with `ao2_fps_unlock`): a simulation
step runs when it is due on a fixed clock (`ao2_sim_hz`; more than 4 steps
behind restarts the clock); every other frame - still paced by the frame
limiter at `ao2_fps_target` - is render-only: the frame sync tick hook
(`frame_time.cpp`) returns without the tick, and `UGameEngine::Tick` only calls
the viewport draw, with the shared `rand()` state put back afterwards. Nothing
is interpolated, so a render-only frame repeats the last picture. Single
player steps by 1/`ao2_sim_hz`, or by as many periods as the step is late
(up to 4: below `ao2_sim_hz` frames a second one period a frame ran the game
in slow motion, x0.5 at 30 FPS); the lockstep keeps the game's own
step (the players now report the step length), and its input lead
(`coop_input_delay.cpp`) is counted in simulation steps.
`--ao2_decouple_debug=true` logs once a second to `decouple.log` (frames,
steps, step gaps, engine tick and render-only draw times, rand restores).

Tested 2026-10-06 on one PC (fresh user data, scale 1):

- Single player (training level, walking, turning, shooting): 120 FPS, 60
  steps/s, frame times 8.3-9 ms, `UGameEngine::Tick` ~3 ms, render-only draw
  ~0.2-0.3 ms on the game thread; the native renderer presents 120 full frames
  per second; game time x1.00 (`tools\gamespeed.py`).
- Online co-op, host at 120 FPS and partner at 90 FPS (60 and 30 render-only
  frames per second), both 60 steps/s, Somalia, scripted input on both:
  lockstep at 60, waits for the partner ~0 ms, 3.5 min without a checksum
  mismatch (`bad308` 0), `rand()` never had to be restored.

## Stage 1: co-op

- **One step for both machines.** The lockstep's own step is a 10-frame
  average of both players' whole milliseconds (16 ms at 60 steps/s: the game
  ran ~4% slow). With decoupling the step is exactly 1/`ao2_sim_hz` online
  too, so both machines must use the same settings: each sends its
  `ao2_decouple` / `ao2_sim_hz` with the link's hellos (`coop_link.cpp`,
  UDP 1002), and both use the lobby server's side's (else the lower link
  id's) - picked in the lobby, fixed while the lockstep runs.
- **The screen keeps going while waiting for the partner.** A step whose
  partner input is not there yet loops inside the frame sync tick (pump the
  network, `appSleep(0)` `sub_829C8978`, update the connections, check) and
  held the whole game thread. That sleep now draws the last state again at
  the frame rate (render command fence after each draw) - only on the game
  thread, inside the tick (`appSleep` also runs on the rendering thread:
  drawing there crashed).
- Tried and dropped: checking for the partner's input before the tick and
  drawing instead of ticking. The received records reach the players' lists
  (`sub_82955260` reads them) only in the connection update `sub_8296B160`,
  which also rebuilds this machine's outgoing record with the frame's
  checksum - zero between ticks, so calling it there would send a wrong
  record; without it the check never saw the input and both machines waited
  for each other (12 steps/s).

Tested 2026-10-06 on one PC (Somalia, scripted input on both):

- Host `--ao2_decouple=true` at 120 FPS, partner without the flag at 90 FPS:
  the partner took the host's settings in the lobby; both 60 steps/s, no
  waits, no checksum mismatch.
- Partner limited to 45 FPS (it cannot make 60 steps/s): the lockstep runs at
  45 steps/s and the game at x0.75 (= 45/60: the fixed step works); the host
  still shows ~120 frames/s, ~75 of them drawn while waiting; 3 min without
  a mismatch.

A machine that cannot keep up with `ao2_sim_hz` slows both down (as on a
console); lower `ao2_sim_hz` then (30-45).

## Stage 2: the camera (`src/interp_camera.cpp`)

The view (location, rotation, FOV) of every drawn frame is placed between the
last two steps' by decouple `Alpha()` - only inside
`ULocalPlayer::CalcSceneView`, so the simulation (aiming) never sees it.
Checked 2026-10-07 (training level, 144 FPS target, 60 steps/s, branch
`server_net`): 2-3 drawn frames per step, each at its own in-between angle;
the user found turning smooth ("отлично ощущается").

## Stage 3: moving objects (`src/interp_objects.cpp`)

The rendering thread keeps each moved primitive's matrices of the last two
steps (UpdateTransform command Execute `sub_8257B400` -> proxy +32
LocalToWorld, +96 WorldToLocal, +160 a third matrix, +224 determinant) and
places them in between (componentwise) around each scene draw
(`sub_825674C0` / `sub_82567460`), putting the step's own back afterwards.
Each draw's step and alpha travel from `BeginRenderingViewFamily`
`sub_82567630` in a host queue alongside the render commands. A proxy is only
written while it still holds the matrix remembered for it (deleted proxies);
jumps over 1500 units are not interpolated; bounds are not changed.
`--ao2_interp_objects_debug=true` -> `interp_objects.log` once a second.

Checked 2026-10-07 (training level, 144 FPS, walking and turning): every draw
gets its entry (none missing or dropped), ~35 objects placed per draw, no
stale proxies, picture intact.

## Stage 4: bones (`src/interp_objects.cpp`)

GPU skin `UpdateDynamicData_RenderThread` `sub_82856678` (vtable `0x820D4B90`
+32; mesh object +56 = dynamic data, whose +4 TArray is ReferenceToLocal)
copies the bone matrices, transposed to 3x4, into each chunk's vertex
factory (TArray at +380; factory lists at LOD render data `[mesh +28] + LOD *
396` +348 / +372 with morphs, +360 / +384 for the second set). Those arrays
of the last two steps are kept and placed in between around the scene draw
like the primitives' matrices (checked before writing, put back after).

Checked 2026-10-07 (training level, 144 FPS, walking and turning): ~33
skinned meshes posed per draw, none stale, characters intact.

## Stage 5: local camera prediction (`ao2_predict_camera`)

The mouse used to reach the screen only through the simulation: up to one
step until UpdateLookingAction took it, then the interpolated view showed
only alpha of that step's turn - and online the turn first travels the
lockstep (input delay, 4-5 steps at ~50 ms latency, ~90 ms in all). Now the
drawn view (only inside CalcSceneView, like stage 2) gets this player's turn
that the simulation has not applied yet (`mouse_aim.cpp` PredictedTurn):

- movement not taken yet (at the current zoom / invert);
- online: this frame's running turn and the finished frames' turn codes
  sent but not replayed yet (a queue; a replayed code pops it and any older
  ones that never made it; entries age out after 1.5 s);
- (1 - alpha) of the turn applied on the drawn step.

The pitch is kept within the game's look limits, read from the character:
aiming (`AO2Pawn.bIsInPrecisionMode`) `Precision_Aiming_Min/MaxPitch` (-77 / 77
degrees = -14017 / 14017 rotation units), otherwise its `CameraSettings`
`Suppression_Minimum/MaximumPitch` (-50 / 35.7 = -9102 / 6498); a narrower
limit (cover: 45 degrees) is learnt from the steps the game cuts short. The
third-person camera orbits the character, so its location is turned around
the pawn by the same rotation (`ao2_predict_camera_orbit`; Controller.Pawn
and Actor.Location found by name at runtime, `src/ue3_reflect.cpp` - +84
guessed from the UObject layout was wrong; the orbit is skipped when that
point is not within 800 units of the camera).
Only this machine's own player (LocalController online) and only while native
aiming runs (on foot). The simulation and the lockstep are not touched.
`--ao2_interp_debug=N` logs the predicted yaw / pitch and orbit radius.

Not tested in the game yet (2026-10-07).

## Next

Particles, material time, testing in co-op; CPU-skinned meshes
(`sub_82851178`) are not interpolated. Later: pick `ao2_sim_hz` from what both
machines can sustain.
