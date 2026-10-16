# Army of Two launcher

`bin\ArmyOfTwoLauncher.exe` is a single file and needs only .NET Framework 4.x (built into Windows 10/11).
Put it next to `armyoftworecomp.exe` (or set the game path in the launcher itself).

- The look (`src\Skin.cs`): flat and minimal in the cover's tones — deep navy, warm grey paper for the main button
  and accents, hairlines, letter-spaced Segoe UI Semibold captions. The cover and the logo are baked into cached
  backdrops, so a repaint takes a few milliseconds (`--bench=<file>` measures it per screen).
- Two screens. **Main**: the art and logo, then Settings, Play and Exit. **Settings**: the Graphics, Controls, Network and
  Advanced tabs; Back returns to the main screen.
- Game files are not changed: settings go to the game as command line flags, which take precedence over
  `settings\*.toml`. The launcher keeps its own settings in `ArmyOfTwoLauncher.ini` next to itself
  (in `%APPDATA%\ArmyOfTwoLauncher` when that folder is read-only).
- **Co-op: the role is chosen automatically.** On Play the launcher looks for a host on the local network for
  about a second (UDP broadcast, port 18490; works over Radmin VPN too). If one answers the game starts as the
  partner (`--ao2_coop_host=<ip>`), otherwise as the host (`--ao2_coop_server`). The second player presses Play a
  few seconds after the first player's game has started, then both go to Campaign Mode → Public Co-op Game.
  If the host is not found (over the internet, for example), enter its address in Network → Host IP.
- Controls. Main screen: ←→/↑↓/Tab select, Enter/Space press, Esc quit; gamepad: D-pad/stick, A/Start, B quit.
  Settings: ↑↓ row, ←→ value, Tab/Shift+Tab/PageUp/PageDown/Q/E tabs, Enter/Space select, Esc back;
  gamepad: LB/RB tabs, A select, B back.
- **Installing the game** (`src\Install.cs`, `src\InstallScreen.cs`). A release has no game files: when
  `assets\default.xex` + `assets\default.xexp` are not next to the game (checked by SHA-1: the European disc and
  Title Update 2), Play is replaced by the install screen. The user chooses (or drops on the window) their own
  disc image `.iso` / Games on Demand package / `default.xex` of an extracted folder, the TU2 package and,
  optionally, DLC packages; the launcher works out which is which. Images and packages are read by
  `ao2_install.exe` in the game folder (`aot\tools\ao2_install.cpp`, the file system devices of rexruntime).
  The game is copied to `assets.tmp` and moved to `assets` only after it has been checked; DLC goes to
  `Documents\armyoftworecomp_tu2\0000000000000000\4541084C\00000002\<package>`. A desktop shortcut is created
  after the installation (can be turned off). Later: Settings → Advanced → Game files and DLC.
  `aot\tools\make_release.ps1 [-Zip]` builds the release package without game files.

- **Updates** (`src\Updates.cs`). On start the launcher asks the GitHub API for the latest release of
  OpenPortroid/ArmyOfTwoRecompilation (off the UI thread, 6 s timeout). The version under Play reads `v1.0 · up to date`,
  or `v1.0 · update available: v1.1`, and then a window offers to download the new archive (the browser downloads
  the release's `.zip`). Offline only the version is shown. `Updates.Current` is the package's version: bump it before
  every release, the tag is `v` + it.

Build: `build.cmd` (Roslyn from Visual Studio / Build Tools, or the csc that ships with Windows). The pictures
in `res\` are made by `tools\make_assets.py <background> [<logo>]`: the background is cropped to the main
screen's 2.5:1 art band (`bg.jpg`) and to the window's shape, blurred, for Settings (`bg_dim.jpg`).
`tools\make_logo.py` cuts the lettering of the README logo (`aot\docs\logo.png`) — the letters and their
bevel, without the rosette and the ground — into `res\logo.png` with a soft shadow (needs numpy and scipy).

Debugging: `--dump-args=<file> [--host=<ip>]` writes the game command line for the current settings (`--host`:
as if a host had been found); `--snapshot=<png> [--screen=main|settings|install] [--tab=N] [--sel=N]` renders
the window to a file without showing it; `--probe=<file>` runs the host search and writes the found IP or
`none`; `--install=<log> --src=<path>[;<path>...]` installs from the given sources without the window;
`--check-update=<file>` asks GitHub for the latest release and writes `<version> <url> newer|current` or `none`;
`--latest=<tag>` takes this tag as the latest release instead of asking GitHub (with `--snapshot`: the update window).
