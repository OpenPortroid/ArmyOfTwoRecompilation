# Getting the game files from your Xbox 360

The port contains no game files and this project does not provide them. You need your own copy of Army of Two for
Xbox 360. This page is about copying it from your own disc and console.

## What the launcher needs

| File | Required | What it is |
| ---- | -------- | ---------- |
| The game | Yes | The **European** release (title `4541084C`): a disc image `.iso`, a Games on Demand package, or the `default.xex` of an extracted game with its `AO2Game` folder next to it |
| Title Update 2 | Yes | The file `TU_12K222C_0000004000000.00000000000G1` |
| DLC | No | Veteran Map Pack and SSC Challenge: Versus Mode Pack 1 packages. They can also be added later: Settings → Advanced → Game files and DLC |

Archives (`.zip`, `.7z`, `.rar`) are not read: extract them first. Other regions are not supported, see the
[FAQ](../../README.md#faq).

The launcher checks the files itself when you install them, so you cannot get this wrong silently. To check by hand,
the SHA-1 of the extracted files must be:

| File | SHA-1 |
| ---- | ----- |
| `default.xex` | `ca5c53e21b93ca2a3391fe0b1a190ee954ac6d66` |
| `default.xexp` (the only file inside the Title Update 2 package) | `a1347695439c0f14dda26dc9bf33eb1ead2b78e0` |

## Where the files are on the console

| File | Path |
| ---- | ---- |
| Title Update 2 | `Content\0000000000000000\4541084C\000B0000`, or in the system cache as a file whose name starts with `TU_12K222C` |
| DLC | `Content\0000000000000000\454107F8\00000002` |

## Copying them

These are the usual ways of reading an Xbox 360's files on a PC. The project has not tried each of them on every
console, so if the launcher refuses what you copied, please [open an issue](https://github.com/OpenPortroid/ArmyOfTwoRecompilation/issues)
and say which way you used.

### A console with custom firmware (RGH / JTAG)

Connect to the console over FTP or open it in a file manager on the console, and copy the title update, the DLC and the
game from the paths above.

### An unmodified console

You need a hard drive transfer cable (or a USB storage device the console has set up as its system drive) and
[Velocity](https://github.com/Gualdimar/Velocity), a PC tool that reads Xbox 360 storage.

1. **Install the game on the console.** With the disc in the tray: on the Dashboard, select the disc, press X for
   Game Details, then Install. Choose the drive you are going to read on the PC.
2. **Get the title update.** Start the game while connected to Xbox Live; the console offers the update if it does not
   have it yet.
3. **Connect the drive to the PC** and start Velocity as administrator. Open the Device Content Viewer when it asks.
4. **Copy the game and the DLC** from `/Shared Items/Games/` and `/Shared Items/DLC/` with Copy Selected to Local Disk.
5. **Copy the title update.** Tools → Device Tools → Raw Device Viewer, go to `/System Cache/Cache/` and copy the file
   whose name starts with `TU_12K222C`.

Be careful with a drive shared with the console: back up what you need before the console reformats a USB device as a
system drive, since that erases it.

## Then

Start `ArmyOfTwoLauncher.exe`, choose the files on the install screen and press Install. You can also drop them on the
window. The launcher works out which file is which, and the originals are not needed afterwards.
