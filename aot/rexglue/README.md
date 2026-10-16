# rexglue - ReXGlue SDK (not checked in)

This folder holds the prebuilt ReXGlue SDK release used to build and run
ArmyOfTwoRecomp. Everything under here except this file is gitignored (see
`.gitignore`: `rexglue/*` / `!rexglue/README.md`) - the SDK is ~200 MB of
binaries and every contributor fetches their own copy.

## Setup

1. Download the release archive for your platform matching the SDK version
   pinned in [`armyoftworecomp_manifest.toml`](../armyoftworecomp_manifest.toml)
   (`sdk_version`, currently `0.10.0`) from the
   [ReXGlue SDK releases page](https://github.com/rexglue/rexglue-sdk/releases).
   The SDK's CI publishes five platform archives: `win-amd64`, `linux-amd64`,
   `linux-arm64`, `mac-amd64`, `mac-arm64` (no `win-arm64` - the SDK doesn't
   build or publish that target).
2. Extract it here so the layout looks like (`win-amd64` shown, same shape
   for any other platform folder):
   ```
   rexglue/
     README.md          (this file)
     win-amd64/
       bin/
       include/
       lib/
       share/
       ...
   ```
3. That's it - [`CMakeUserPresets.json`](../CMakeUserPresets.json) already
   points `CMAKE_PREFIX_PATH` at `rexglue/win-amd64`, relative to the repo
   root, so no absolute paths need editing. Building for another platform
   needs its own entry there pointing at the matching `rexglue/<platform>`
   folder - see the root [README](../README.md#cross-platform-builds) for
   the template and the constraints on which platforms this dev setup can
   actually build.

See the root [`README.md`](../README.md) for full build/run instructions.
