# Running on Linux and Steam Deck through Proton

**Status: not tested yet.** No Linux machine has run this build. This page says what to try and what to report, and
[#44](https://github.com/OpenPortroid/ArmyOfTwoRecompilation/issues/44) is the place for the results. Only Windows is
supported until the checklist below has been filled in.

What was checked without Linux:

- Every function `armyoftworecomp.exe`, `rexruntime.dll`, `ao2_install.exe` and `ao2_lobby_server.exe` import from Wine's
  own DLLs exists in Wine's export lists (master branch) and none is a stub. That says nothing about how well each one
  works, and a Proton built on an older Wine can differ.
- The renderer needs plain Direct3D 12 and DXGI: no mesh shaders, no vendor APIs, no DirectStorage.
- The package has Microsoft's `D3DCompiler_47.dll` next to the game. It should load instead of Wine's own compiler,
  which does not implement all of HLSL.
- The port's screens fall back to the package's Tektur font where Windows' fonts are missing.
  `--ao2_ui_system_fonts=false` shows that fallback on Windows.

## Trying it

You need your own copy of the game, as on Windows (see [dumping.md](dumping.md)).

1. Unpack the release to a folder on the Linux drive, for example `~/Games/ArmyOfTwo`.
2. In Steam: **Add a Non-Steam Game**, choose `ArmyOfTwoLauncher.exe`.
3. In the shortcut's properties: **Compatibility**, force a Proton version. Note which one.
4. Add this to its **Launch Options**, so that a failure leaves a log:

   ```
   PROTON_LOG=1 %command%
   ```

5. Start it and install the game as on Windows. The Linux file system is drive `Z:` in the launcher's file dialogs.
6. Add the line `log_to_files = true` to `settings/hardware.toml` in the game folder: without it the game writes none of
   the logs named below.
7. Press Play.

If the launcher does not work (it needs .NET Framework, which Proton replaces with Wine Mono), install on a Windows PC
and copy the whole game folder, `assets` included. Then point the shortcut at `armyoftworecomp.exe` instead and add
`--fullscreen=true` after `%command%`.

## What to report

Your distribution or SteamOS version, GPU and driver, the Proton version, and for each line what happened:

| # | Check | Look at |
|---|-------|---------|
| 1 | The launcher opens and its text is readable | |
| 2 | The launcher's gamepad navigation works | |
| 3 | The launcher installs the game | `ao2_install` output on the launcher's screen |
| 4 | The game starts and reaches the main menu | `crash_*.txt`, `steam-<appid>.log` in your home folder |
| 5 | Shaders compile: no missing or broken surfaces, no long freezes after the first minutes | `native_gpu.log` |
| 6 | A level plays for ten minutes; frame rate and frame pacing | the performance overlay (F2) |
| 7 | Sound | |
| 8 | Keyboard, mouse and gamepad in game | |
| 9 | The F1 menu and invite cards are readable, in your language | |
| 10 | Russian text in the game itself, if you use it (its letters are drawn through GDI) | a screenshot |
| 11 | Saving and loading a checkpoint | |
| 12 | Co-op over a LAN or VPN with a Windows player | `invite.log`, `net.log` |
| 13 | Co-op over Steam | `steam.log` |
| 14 | The launcher finds and installs an update | `logs/launcher.log` |

Line 13 is the one most likely to fail. The Steam bridge binds each player's address on the game's ports next to the
game's sockets on `0.0.0.0`; Windows allows that, while Linux normally refuses it. `not bound` in `steam.log` means this
is what happened. Co-op over a LAN or VPN does not use the bridge.
