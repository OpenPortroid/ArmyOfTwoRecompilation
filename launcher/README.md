# Army of Two launcher

`bin\ArmyOfTwoLauncher.exe` is a single file and needs only .NET Framework 4.x (built into Windows 10/11).
Put it next to `armyoftworecomp.exe` (or set the game path in the launcher itself).

- The look (`src\Skin.cs`): the art behind rounded panels of dark glass, one cold accent, sentence case in Segoe UI. The cover,
  the logo and the shading of the main screen are baked into cached backdrops, so a repaint takes a few milliseconds
  (`--bench=<file>` measures it per screen).
- Two screens. **Main**: the art and logo, the profile at the left, Play, Settings, Exit in the middle and links to Discord and the project on GitHub at the right.
  Play looks for a host, then joins or hosts; split-screen is chosen in the game. **Settings**: the Graphics, Controls, Keyboard, Gamepad, Split-Screen, Network, Advanced,
  Profiles and Achievements sections at the left, the rows in the middle, the row's description at the right; Back returns to the main screen. A section longer than the panel scrolls (wheel, or by moving
  the selection).
- **Achievements** (`src\Achievements.cs`): the current profile's achievements by section (as the game's own
  Achievements page has them), icons, gamerscore and when each was earned. The names, descriptions and icons are the
  game's: the game writes them to the shared cache (`Documents\armyoftworecomp_tu2\cache\achievements`) when it
  starts, so the tab is empty until it has run once; the unlocks are read from the profile's `achievements\4541084C.toml`.
- **Keyboard / Gamepad** (`src\BindPages.cs`, `src\Bindings.cs`): every key of the game's `keybind_*` and every gamepad
  button of `padbind_*` (plus the sticks swapped / inverted). Enter or a click waits for the key or mouse button (with
  Shift / Ctrl / Alt: a combination; a modifier let go alone binds itself), → or + adds one more key, Delete or × clears;
  a gamepad row takes ←→ or a press on an Xbox controller. All bindings go to the game as flags (`none` = nothing bound).
- **Split-Screen**: both gamertags (player 1's is the Network page's name, `--user_gamertag_2`). The devices are not
  chosen here (`--input_player1/2=auto --input_share_pads`): every device is player 1's and nobody else signs in until the game's
  Campaign Mode → Splitscreen window, where each player presses a button on their device, or a versus lobby's Guest Sign-In.
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
  `dlc\<package>` in the game folder (next to `assets`). DLC that earlier versions put in
  `Documents\armyoftworecomp_tu2\0000000000000000\4541084C\00000002` is moved there when the launcher starts
  (the game does the same if it still finds some). A desktop shortcut is created
  after the installation (can be turned off). Later: Settings → Advanced → Game files and DLC.
  `aot\tools\make_release.ps1 [-Zip]` builds the release package without game files.

- **Updates** (`src\Updates.cs`). On start the launcher asks the GitHub API for the latest release of
  OpenPortroid/ArmyOfTwoRecompilation (off the UI thread, 6 s timeout). The version at the top right reads `v1.0 · up-to-date`,
  or `v1.0 · update available: v1.1`, and then a window offers to update. UPDATE downloads the release's `.zip` to
  `update.tmp` next to the launcher, checks its SHA-256 against the release's `SHA256SUMS.txt`, unpacks it there, checks
  it is a release of the port and copies it over the package, with a progress bar for each step; then the new launcher
  is started. The user's `settings\*.toml` are kept; the running launcher is renamed to `ArmyOfTwoLauncher.exe.old` (a
  running exe cannot be overwritten) and removed by the next start. Without a release layout (no `armyoftworecomp.exe`
  next to the launcher) the browser downloads the archive instead. Offline only the version is shown.
  `Updates.Version` is the package's version: bump it before every release, the tag is `v` + it.
  - A release newer than 1.4.3 without `SHA256SUMS.txt`, or with a file not listed in it, is refused. So upload all three
    files `make_release.ps1 -Zip` writes, under the names it gives them. Older releases can still be installed from
    the release list, unchecked. The hash catches a damaged or swapped download; it does not protect against someone who
    can change the repository's releases, since the file comes from the same place.
  - Replacing the files is all or nothing: the old files wait in `update.tmp\backup` while the marker
    `update.tmp\installing` exists. A failure puts them back; after a crash or a power cut the next start does.
  - The launcher that installs an update is the old one: the checks work from the update after the first release
    that contains them.
  - Test: `tools\test_update.ps1` (fake packages and releases in a temporary folder; no game, no network).
- **What's new.** The first start after an update (`seen_version` in the launcher's ini differs from
  `Updates.Version`) fetches the description of release `v<Version>` from GitHub (`Updates.NotesFor`, markdown: headings,
  bullets, bold and links are reduced to plain lines) and shows it in a window (scroll: wheel, ↑↓, PgUp/PgDn; Enter / Esc /
  CLOSE). So the release description is the patch notes: write it before publishing. The PATCH NOTES button at the
  bottom right of the main screen shows them again. A fresh install, a release without a description, or being offline shows nothing (offline: asked
  again next start). Debug: `--notes-file=<md>` with `--snapshot` renders the window from a file.

Build: `build.cmd` (Roslyn from Visual Studio / Build Tools, or the csc that ships with Windows). The pictures
in `res\` are made by `tools\make_assets.py <background> [<logo>]`: the background is cropped to the main
screen's 2.5:1 art band (`bg.jpg`) and to the window's shape, blurred, for Settings (`bg_dim.jpg`).
`tools\make_logo.py` cuts the lettering of the README logo (`aot\docs\logo.png`) — the letters and their
bevel, without the rosette and the ground — into `res\logo.png` with a soft shadow (needs numpy and scipy).

Debugging: `--dump-args=<file> [--host=<ip>]` writes the game command line for the current settings (`--host`:
as if a host had been found); `--snapshot=<png> [--screen=main|settings|install] [--tab=N] [--sel=N] [--drop]` renders
the window to a file without showing it; `--probe=<file>` runs the host search and writes the found IP or
`none`; `--install=<log> --src=<path>[;<path>...]` installs from the given sources without the window;
`--check-update=<file>` asks GitHub for the latest release and writes `<version> <url> newer|current` or `none`;
`--latest=<tag>` takes this tag as the latest release instead of asking GitHub (with `--snapshot`: the update window);
`--pretend-version=<ver>` makes the launcher think it is version `<ver>`, to test the update against the real GitHub;
`--self-update=<log>` downloads and installs the latest release over this package without the window;
`--update-ui=downloading|installing|failed[:<0..1>]` (with `--snapshot --latest=<tag>`) draws the update window in that state.
