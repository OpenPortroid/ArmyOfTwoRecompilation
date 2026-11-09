<p align="center">
  <img src="./aot/docs/logo.png" alt="Army of Two Recompilation">
  <br>
  <a href="https://github.com/OpenPortroid/ArmyOfTwoRecompilation/releases/latest"><img src="https://img.shields.io/github/v/release/OpenPortroid/ArmyOfTwoRecompilation?label=release" alt="Latest release"></a>
  <a href="https://github.com/OpenPortroid/ArmyOfTwoRecompilation/releases"><img src="https://img.shields.io/github/downloads/OpenPortroid/ArmyOfTwoRecompilation/total" alt="Downloads"></a>
  <a href="https://discord.gg/jp9QhKPz3Z"><img src="https://img.shields.io/badge/discord-join-5865F2?logo=discord&logoColor=white" alt="Discord"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT license"></a>
  <br>
  <img src="https://img.shields.io/badge/platform-Windows%20x64-0078D6?logo=windows&logoColor=white" alt="Windows x64">
  <img src="https://img.shields.io/badge/renderer-Direct3D%2012-76B900" alt="Direct3D 12">
  <img src="https://img.shields.io/badge/co--op%20%26%20versus-online-1E5BB8" alt="Online co-op and versus">
</p>

# Army of Two Recompilation

Unofficial PC port of the Xbox 360 version of Army of Two (2008), made by static recompilation. The game's PowerPC
code is translated to C++ with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and built as a native
Windows x64 executable; graphics go through the port's own Direct3D 12 renderer instead of GPU emulation.

The campaign can be played solo, in split-screen and in **online co-op**, and **versus** works online against other
players and bots, all without EA servers (over LAN, a virtual network or Steam).

**This repository and its releases contain no game files. You need your own copy of the game.**

[**Download the latest release**](https://github.com/OpenPortroid/ArmyOfTwoRecompilation/releases/latest) ·
[Discord](https://discord.gg/jp9QhKPz3Z) ·
[Report a bug or suggest something](https://github.com/OpenPortroid/ArmyOfTwoRecompilation/issues)
## Requirements

- Windows 10/11 x64 only, a GPU with Direct3D 12
- A CPU with AVX2; for CPUs without it there is an AVX1 version (`avx1-version.aot` in each release, the launcher
  installs it by itself)
- About 6 GB of disk space, plus 1.3 GB for the DLC
- Army of Two for Xbox 360 (European release), Title Update 2 and DLC (optional)

## Installation

1. Download the package from [Releases](https://github.com/OpenPortroid/ArmyOfTwoRecompilation/releases) and extract
   it to a writable folder (not `Program Files`).
2. Run `ArmyOfTwoLauncher.exe`. If the game is not installed, it opens the install screen.
3. Choose the disc image (`.iso`) or `default.xex` of an extracted game, and the Title Update 2 package
   `TU_12K222C_0000004000000.00000000000G1`. DLC packages are optional.
4. Press Install. The files are copied to `assets\`.

Don't have the files on a PC yet? See [Getting the game files from your Xbox 360](aot/docs/dumping.md).

Saves: `Documents\armyoftworecomp_tu2`. The launcher offers new versions by itself; or extract the new package over
the old one (settings and saves are kept).

On the first runs shaders are compiled, so there are short stutters.

Builds of this port are published only on the [Releases](https://github.com/OpenPortroid/ArmyOfTwoRecompilation/releases)
page of this repository. Be careful with copies from anywhere else: a program you run from another site may not be
what it says it is, and a package with game files in it is not ours.

## Features

#### Native Direct3D 12 renderer
Not GPU emulation: the Xbox 360's draws are translated to D3D12 by the port's own renderer, tuned for high frame
rates. Shaders and pipelines the game uses are precompiled during the
loading screens from a manifest shipped with the release (it holds hashes, not game code), so the first runs stutter
much less. 

#### Graphics settings in the game
Options → Graphics or F1: render resolution above the console's 720p, anti-aliasing, ambient occlusion, shadow
resolution, anisotropic filtering, FPS cap and more. Widescreen and ultrawide aspect ratios, and field of view
sliders.

#### Online co-op and versus without EA servers
Public and Private games, a server list with ping, invites, Steam lobbies and Join with code, versus bots that fill empty slots. Both run in deterministic lockstep with an adaptive input delay and an overlay that shows the connection quality. See [Co-op](#co-op) and [Versus](#versus).

#### Split-screen on one PC
Each player's keyboard, mouse or gamepad is chosen in a window in the game or in the launcher, also for a versus guest.

#### Keyboard, mouse and gamepad
Keyboard and mouse support with key glyphs in the on-screen button prompts, rebinding of keys and gamepad buttons in
the game and in the launcher, field of view and mouse settings, vibration and volume sliders in the F1 menu.

#### Launcher
Installs the game from your files, finds the co-op host on the network and picks the role, has all settings,
split-screen devices, the achievements and the time played per profile, and updates itself (updates are verified
and can be rolled back).
#### Languages
English, Spanish, French, German, Italian, Russian, Portuguese (Brazilian)

#### Quality of life
Skippable pre-rendered cutscenes (Enter, Space or Start), frame rate overlay (F2), the debug menu (F1) with
graphics, controls, sound, achievements, invites and cheats, and checkpoints that work in co-op.

## Controls

| Action                | Keyboard / mouse | Controller  |
| --------------------- | ---------------- | ----------- |
| Move                  | W A S D          | Left stick  |
| Camera / aim          | Mouse            | Right stick |
| Fire                  | LMB              | RT          |
| Precision aim         | RMB              | LT          |
| Reload                | R                | X           |
| Roll / cover / vault  | Space            | Y           |
| Use / confirm         | E / Enter        | A           |
| Back                  | Backspace / Q    | B           |
| Crouch                | Ctrl / C         | L3          |
| Inventory (hold)      | Z                | LB          |
| Co-op commands (hold) | F                | RB          |
| Weapons and grenades  | 1–4              | LB + D-pad  |
| Partner orders        | Arrows           | D-pad       |
| R3                    | MMB              | R3          |
| GPS                   | Tab              | Back        |
| Pause                 | Esc              | Start       |
| Skip a cutscene       | Enter, Space     | Start       |
| Port debug menu       | F1               | Back + Start |
| Frame rate overlay    | F2               |             |
| Accept an invite      | F6               |             |

Bindings: in the game, Options → Graphics → Controls (Enter or a click on a row, then the key; left adds
one more key); the launcher's Settings → Keyboard and Gamepad tabs (press the key or button to assign it, combinations
and mouse buttons 4–5 work), or `settings\mapping.toml` (`keybind_*`, `padbind_*`). Gamepads work as on the console.

## Co-op

- Works over LAN, a virtual network (Radmin VPN or ZeroTier) or **Steam**: with the Steam client running
  the game needs no VPN or forwarded ports. Steam friends running the port show up in the Players window and can be
  invited; others join a public game from the server list or by **Join with code** (F1 → Online), a 7-letter code
  of the host's Steam account.
- Both players start the game from the launcher. The launcher looks for a host on the local network and picks the
  role. Over the internet, enter the host's IP in the launcher's Settings → Network.
- Create a game: Campaign Mode → Create Co-op Game, then pick Private (invites only) or Public (also in the server
  list, F1 → Online → Servers, where anyone can join). The lobby's Invite button opens the Players window with the
  players found on the network (also the F1 menu's Invites tab); the partner accepts the invite with F6. It keeps
  checkpoints, so you can continue the game later.
- Find a game: Campaign Mode → Search Co-op Game opens the list of public games (host, mission, difficulty,
  players, ping): join one, Quick Join the one with the lowest ping, or create your own. Versus' Find Match (the disc's Quick Match) opens
  the same list of matches. Without Steam both run the game's own search.
- Both players need the same version of the port (both on the AVX1 version if one of them has no AVX2), the same
  DLC and the same game language.
- Allow the game through Windows Firewall once on both PCs: the launcher's Settings → Network → Windows Firewall.

Details: [aot/docs/online_coop.md](aot/docs/online_coop.md).

## Versus

- Versus works over the same networks as co-op: choose Versus in the main menu. Create Match asks Private or Public as
  Create Co-op Game does and takes invites the same way.
- A match can start with fewer than 4 players: bots fill the empty places. They carry hostages to the extraction,
  destroy depots, buy weapons and armor, lift their partner up walls and take cover under fire.
- Bot settings are in `settings\online.toml` (`ao2_versus_bots`, `ao2_versus_bots_buy` and the other
  `ao2_versus_bots_*` keys). Every player of a match needs the same bot settings.

## Split-screen

- Two players on one PC: in the game, Campaign Mode → Splitscreen. A window opens with the two players: player 1
  presses a button, then player 2, and what each pressed is theirs: a key or a mouse button gives the keyboard and
  mouse, a gamepad button that gamepad. Esc or B goes back. The launcher's Settings → Split-Screen page has the gamertags.
- Player 2 signs in with their own profile and saves while a device is assigned to them.
- The mouse aims and drives for the player who has the keyboard.
- Button prompts follow each player: keys in the keyboard player's half of the screen, Xbox buttons in the other's;
  a prompt they share shows both.
- Without the launcher: `input_player1`, `input_player2`, `input_share_pads`, `user_gamertag_2` and `max_local_users`
  in `settings\mapping.toml` (the devices for versus; the campaign's window overrides them).
- Versus in split-screen: in the Custom Match lobby press Y (Guest Sign-In), then A: player 2 joins as player 1's
  guest, on the same team, and the match is played on a split screen; bots fill the other places. Not in ranked
  matches, as on the console (`ao2_versus_guest` in `settings\online.toml`). The same window as in the campaign opens:
  each player presses a button on their device. The devices go back to player 1 after the match. Players on different
  teams each have their own aggro meter on their part of the screen, and the match timer stands on the line
  between the parts.

## Settings

TOML files in `settings\`. Most settings can also be changed in the game (Options → Graphics, the F1 menu) or in the
launcher, and are saved there.

File|Contents
-|-
`hardware.toml`|Window, fullscreen, render scale (`ao2_native_gpu_resolution_scale`), FPS cap (`ao2_fps_target`), aspect ratio (`ao2_aspect_ratio`), graphics effects, field of view (`ao2_fov`, `ao2_fov_aim`), language (`ao2_game_language`)
`mapping.toml`|Key and gamepad bindings, mouse sensitivity and inversion, split-screen players and devices
`online.toml`|Co-op and versus settings, versus bots (`ao2_versus_bots*`), gamertag

## Building

Needed: Visual Studio 2022 Build Tools, LLVM/Clang, CMake, Ninja, Git, and the game files with `default.xexp` from
TU2 in `aot/assets/` ([aot/assets/README.md](aot/assets/README.md)).

Folder|Contents
-|-
`aot/`|The port: manifest, sources (`aot/src`), renderer (`aot/src/gpu/native`), settings
`sdk_changes/`|Changed ReXGlue SDK files ([README](sdk_changes/README.md))
`launcher/`|Launcher ([README](launcher/README.md))
`tools/`|Debug and test tools

1. SDK with the port's changes:

   ```
   powershell -ExecutionPolicy Bypass -File sdk_changes\apply.ps1
   build-sdk.cmd
   ```

2. Code generation:

   ```
   cd aot
   rexglue\win-amd64\bin\rexglue.exe codegen armyoftworecomp_manifest.toml
   ```

3. Game build: `build.cmd` (release; other modes are listed at the top of the file, among them `compat` for the
   AVX1 version and the profile-guided optimization steps). It also copies the modified `rexruntime.dll` from `sdk\`
   next to the game.
4. Launcher: `launcher\build.cmd`.

Release package: `aot\tools\make_release.ps1 [-Zip]` (needs the release, `compat` and launcher builds). `-Zip` also
writes `SHA256SUMS.txt`: upload it with the zip and `avx1-version.aot`, or launchers refuse the release.

Checks without the game's files, also run by GitHub Actions: `launcher\tools\test_update.ps1` (the launcher's
self-update) and `tools\ci\cpp_tests.cmd` (the shader translator, texture conversion and lobby server, built against the
SDK with `sdk_changes` applied). They do not build the game, which is generated from your copy.
Renderer notes: [aot/docs/native_renderer.md](aot/docs/native_renderer.md).

## Special Thanks

- [Nikolay Gorb](https://github.com/nikolaygorb)
- [rexglue](https://github.com/rexglue/rexglue-sdk)
## Legal

A fan project, not affiliated with Electronic Arts. Army of Two and related names are trademarks of their owners.
The repository contains only the port's code and the recompilation config; the recompiled code is generated locally
from your copy of the game.
