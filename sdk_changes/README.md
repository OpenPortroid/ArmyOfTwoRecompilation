# ReXGlue SDK changes

The port needs a modified ReXGlue runtime (`rexruntime.dll`). This folder holds only the SDK files changed or added
for Army of Two, at their paths in the SDK tree (`files/`). Base: ReXGlue SDK commit `1406e1b` (0.10.0.8-dev).

What the changes cover:

- **Online co-op**: `xam_net.cpp`, `xlive_net.cpp/.h` (XNet, sessions), `xsocket.*` (sockets, traffic counters),
  `xgi_app.cpp`, `xlivebase_app.cpp`, `xam_user.cpp`
- **Keyboard, mouse and split-screen**: `mnk_input_driver.cpp` (modifiers, mouse without the right stick, mouse buttons 4-5), `keybinds.cpp` (their names), `input_system.*` (gamepad remapping, `padbind_*`), `device_assignment.cpp` (the players' devices, `input_player1/2`), `xam_user.cpp` (second player's gamertag)
- **Graphics**: GPU hooks for the native renderer (`command_processor.*`, `xboxkrnl_video.cpp`), high-performance
  adapter choice (`d3d12_provider.cpp`), `rex_app.cpp`
- **Filesystem**: `virtual_file_system.cpp` picks the device with the longest matching mount path, so a folder can be
  mounted over the game data (keyboard button prompts overlay `AO2Game\Localization`); `host_path_device.cpp` and
  `content_manager.cpp` let a content package's files be replaced by those of an overlay folder
  (`content_overlay_root`: the Russian text of the downloadable content's `.int` files)
- **Performance**: `timer_queue.cpp` waits on a high-resolution timer instead of spinning
- **Codegen** (`rexglue.exe`, not `rexruntime.dll`): `codegen/builders/context.cpp` emits a conditional tail call
  like an unconditional one, so with `non_volatile_as_local` a `share_registers` target gets the caller's
  registers through ctx and the save / restore helpers stay elided (D3D's `beq` into its epilogue fragment
  0x82B0E470). The manifest's `non_volatile_as_local = true` needs this `rexglue.exe`: build it with
  `cmake --build sdk\out\build\win-amd64 --config Release --target rexglue` and copy
  `sdk\out\win-amd64\Release\rexglue.exe` over `aot\rexglue\win-amd64\bin\rexglue.exe`.

Usage:

```
powershell -ExecutionPolicy Bypass -File sdk_changes\apply.ps1
build-sdk.cmd
```

`apply.ps1` clones the SDK into `sdk\` at the base commit (if it is not there yet) and copies these files over it.
`build-sdk.cmd` builds `rexruntime.dll` and copies it next to the game. After changing the SDK, refresh this folder
with `tools\sync_sdk_changes.ps1`.
