# Title Update 2

The `TU2` branch recompiles the game as patched by Title Update 2 (Europe,
title 4541084C, media 7E3E9BEA, version 0.0.2.1) instead of the disc
executable (0.0.0.1). TU2 is needed for the downloadable content: the
Veteran Map Pack (single player missions Kiev and Miami Alternate Ending,
versus map M07) and SSC Challenge: Versus Mode Pack 1 (Race / Gladiator mode,
maps G01-G04). Their packages import classes and functions that only the TU2
executable has (`AO2MPGame_Race`, `IsMPRaceMode`, `UIDataProvider_AO2GladiatorMaps`,
`DLC_SPMissionSelectScene`, the Kiev / Miami characters, ...): EA could only
ship `default.xexp` in the title update, so the script side of these features
is native code there.

## Files

- `assets/default.xex` - the disc executable, unchanged.
- `assets/default.xexp` - the only file of the TU2 package
  `TU_12K222C_0000004000000.00000000000G1` (STFS, "Army of Two Title Update #2").
  ReXGlue applies a sibling `.xexp` when it loads the executable, in codegen
  ("XEX patch applied successfully ... new version: 0.0.2.1") and in the game,
  so the recompiled code and the guest image both are TU2.
- User data: `Documents/armyoftworecomp_tu2` (armyoftworecomp_app.h), not the
  disc version's `Documents/armyoftworecomp`: the disc version enumerates the
  same content folder and could not use the DLC. TU2 writes its checkpoint to
  both `default_checkpoint.sav` and `default_checkpoint_tu2.sav` (the same
  bytes) and rewrites the profile.
- DLC: each package extracted to `<game>/dlc/<package file name>/`
  (dlc_folder.cpp; the SDK's `marketplace_content_root`). Earlier versions
  used `<user data>/0000000000000000/4541084C/00000002/`, moved from there at
  start. The packages were made for title 454107F8; the game only enumerates
  its own title (no alternate title ids in the XEX), and the dlc folder serves
  every title. The runtime lists directories only, it does not mount STFS
  files there.

## What changed in the executable

TU2 is a rebuild of the whole engine (`ue3maindev` agent -> agent2): code grew
by 0xCD060 bytes, 46588 -> 47153 functions (44292 identical modulo
relocations), every section after `.rdata` moved, the entry point is
0x82B08EA8. The D3D library (XDK nov07, 320 functions) is unchanged.

Every guest address in `src/`, `midasm_hooks.toml`, `default_functions.toml`
and `src/gpu/d3d_trace_list.inc` was moved to TU2:

- code: by the function the address is in when TU2 has it unchanged (the
  `.pdata` lists of both images aligned by normalized code), else by a unique
  match of the surrounding instructions with relocated immediates masked;
- data: by the code that references it (lis/addi constants propagated in both
  images, then the TU2 value at the matched instruction); FName globals by the
  name string they are built from;
- `default_functions.toml`: by an unchanged neighbouring function, a vtable
  slot or an exact code match (`# v1 0x...` keeps the old address), plus the
  tail-call targets codegen reported as UnresolvedCall.

Class layouts changed too. Checked against every field offset the hooks use
(statistics over all unchanged functions, then the hooked functions
themselves):

- the look rotation holder (`[controller+2024]`): the block at +3616 moved
  by 4, rotation is +3620 now (mouse_aim.cpp);
- a class with fields from +0x6B0 on moved by 0x430 (and later +0x438, +0x43C);
  no offset the hooks read is in it;
- unchanged: the frame sync object (+16, +44/+46, +92), UEngine / viewport /
  local player fields, AO2SeqAct_ControlMovie's InputLinks, the movie player,
  and the vtable slots used (+264, +268, ...).

Hooked functions that TU2 changed, reviewed: the frame sync tick (one more
field read, +0x28), the lockstep checksum compare (more cases), the movie
input handler (one more condition before skipping), ULocalPlayer::Exec (more
commands), ControlMovie's play/stop (an IsA check of the controller; the Iraq
LOADING MAP fix still applies), the FName init that sets MouseX / MouseY.

## Object interpolation and freed memory

With ao2_decouple + ao2_interp_objects, opening or leaving Select Mission
crashed: GMalloc returned unmapped addresses (memcpy into 0x035BE060 /
0x0462E060) or the allocator faulted on a broken free list. interp_objects.cpp
told a freed object from a live one only by its contents, which a freed block
keeps until it is reused, so render-only frames could write interpolated
places and bones into freed blocks. It now watches GMalloc's Free and Realloc
(sub_8254A360 / sub_8254A2F0) and forgets an object as soon as memory it uses
is freed or moved.

## SDK fix: DLC root name

With the DLC present the game crashed after the startup movies ("Dispatch
thread: deferred completion threw 'Invalid UTF-8'"): XamContentCreateEx with
an overlapped kept the root name ("DLC0") as a string_view into guest memory
and used it on the dispatch thread 100 ms later, after the game had reused the
buffer. sdk/src/kernel/xam/xam_content.cpp copies it now
(sdk_changes/files/src/kernel/xam/xam_content.cpp).

## Co-op work from main

This branch also has `main` up to b776f43 (co-op desync fixes: sync
trace, skeletal LOD / LastRenderTime / LogicalLODLevel, rand() state after
extra draws, corpse limit, camera prediction, log_to_files). The addresses
main added after the TU2 fork point (eba4003) were moved the same way:
functions by normalized code (relocatable immediates masked; unique hash,
then call slots of matched callers), data by the matched instructions that
build them; where the TU2 commit already had a pair, both agree.

| disc | TU2 | what |
|------|-----|------|
| sub_822A2B78 | sub_822A5CC0 | ragdoll pool: remove first N |
| sub_822CF838 | sub_822D35F0 | ragdoll pool: add |
| sub_822CF900 | sub_822D36B8 | ragdoll pool: tick (vtable; differs only in GameInfo +452 -> +456) |
| sub_823048F8 | sub_82309310 | ragdoll pool: sort |
| sub_823E1BB8 | sub_823EAF28 | rand() |
| sub_82403180 / sub_82404680 | sub_8240C5D8 / sub_8240DAD8 | execRand / execFRand |
| sub_82481B18 | sub_8254A088 | pool allocator Free |
| sub_825560D8 | sub_8261E8B8 | render thread: LastRenderTime |
| sub_82624028 / sub_827DF400 | sub_826EBE38 / sub_828A4EF0 | skeletal mesh logical LOD |
| sub_827D0BC0 | sub_828967D0 | execDestroy |
| sub_8295D1B0 / sub_82962528 | sub_82A26290 / sub_82A2D708 | lockstep checksum compare / frame record |
| 0x830AB53C / 0x830AB69C / 0x830E6E38 | 0x8317BA6C / 0x8317BBCC / 0x831B7558 | rand state / its thread / shared flag |
| 0x8311471C / 0x8311F58C / 0x831228C8 | 0x831E4E3C / 0x831EFCBC / 0x831F3008 | GObjects / GEngine / frame sync |

The hooked functions above are identical in TU2 apart from the noted field,
so the field offsets the hooks use (LOD +1216/+1220, pool +1136/+1140, 28-byte
entries) are unchanged. The look rotation reads in main's new code (camera
prediction pitch limit, sync trace `look`) use +3620 like mouse_aim.
Both co-op players need this same build.

## Not done

- Not run yet: the TU2 build needs a test of the menus, a checkpoint, co-op
  and both DLC missions.
- Co-op: both players must run TU2 (the lockstep and the "host on earlier
  version" check).
- Saves: TU2 keeps money collectibles in a new profile bitfield. The TU2
  user data folder started as a copy of the disc version's.
