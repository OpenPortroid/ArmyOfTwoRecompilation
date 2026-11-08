param(
  [string]$Out = "",   # default: <repo>\dist\release\ArmyOfTwo
  [switch]$Zip,        # also pack <Out>.zip
  [switch]$NoAvx1      # no avx1-version.aot (and no armyoftworecomp_compat.exe needed): a test build
)
# Builds the public release package: only our own programs, no game files.
# Unlike make_dist.ps1 there are no assets and no shader cache (both derive
# from the game): the launcher installs the game from the user's own copy
# (ao2_install.exe reads the disc image and the TU / DLC packages) and the
# shader cache is built on the user's PC: the shipped shader_manifest.bin says
# which of the shaders the game creates while loading to compile right away.
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$root = (Resolve-Path "$repo\..").Path
$build = "$repo\out\build\win-amd64-release"
if (-not $Out) { $Out = "$root\dist\release\ArmyOfTwo" }
$crt = Get-ChildItem "C:\Program Files*\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" |
  Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "Visual C++ redistributable folder not found" }
$launcher = "$root\launcher\bin\ArmyOfTwoLauncher.exe"
# armyoftworecomp_compat.exe: the same game for CPUs without AVX2 (build.cmd compat), packed as avx1-version.aot.
$needed = @("$build\armyoftworecomp.exe", "$build\rexruntime.dll", "$build\ao2_install.exe", $launcher)
if (-not $NoAvx1) { $needed += "$build\armyoftworecomp_compat.exe" }
foreach ($f in $needed) {
  if (-not (Test-Path $f)) { throw "not built: $f" }
}
if (-not $NoAvx1 -and (Get-Item "$build\armyoftworecomp_compat.exe").LastWriteTime -lt (Get-Item "$build\armyoftworecomp.exe").LastWriteTime.AddHours(-1)) {
  throw "armyoftworecomp_compat.exe is older than armyoftworecomp.exe - run build.cmd compat"
}

if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($f in "armyoftworecomp.exe", "rexruntime.dll", "ao2_install.exe", "ao2_lobby_server.exe", "gamecontrollerdb.txt") {
  Copy-Item "$build\$f" $Out -Force
}
# Steam (src/steam_net.cpp), when the build has it.
if (Test-Path "$build\steam_api64.dll") { Copy-Item "$build\steam_api64.dll" $Out -Force }
Copy-Item $launcher $Out -Force
foreach ($f in "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
                "msvcp140_atomic_wait.dll", "concrt140.dll") {
  Copy-Item "$($crt.FullName)\$f" $Out -Force
}
# The Windows SDK's redistributable D3DCompiler, not System32's: the Windows 11
# 24H2 copy imports __std_parallel_algorithms_hw_threads from msvcp_win.dll,
# which Windows 10 lacks ("Entry Point Not Found" at start).
$d3dc = "${env:ProgramFiles(x86)}\Windows Kits\10\Redist\D3D\x64\d3dcompiler_47.dll"
if (-not (Test-Path $d3dc)) { throw "Windows SDK D3DCompiler redist not found: $d3dc" }
Copy-Item $d3dc "$Out\D3DCompiler_47.dll" -Force
New-Item -ItemType Directory -Force "$Out\settings" | Out-Null
# The committed settings, not the working copies: the game saves to settings/ (the developer's own values).
$tracked = git -C $root ls-files "aot/settings"
if (-not $tracked) { throw "no tracked files in aot/settings" }
foreach ($f in $tracked) {
  $dest = "$Out\settings\$(Split-Path $f -Leaf)"
  cmd /c "git -C `"$root`" show HEAD:$f > `"$dest`""
  if ($LASTEXITCODE -ne 0) { throw "git show HEAD:$f failed" }
}
# The Russian text and the Tektur font (our own translation, not the game's files).
robocopy "$repo\localization" "$Out\localization" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy localization failed: $LASTEXITCODE" }
Copy-Item "$repo\tools\dist\README_release.txt" "$Out\README.txt" -Force
# Shader manifest: microcode hashes, variants and pipeline states only (no game code), so the
# first launch compiles the shaders on the loading screens (tools/make_shader_manifest.py).
Copy-Item "$repo\tools\dist\shader_manifest.bin" "$Out\shader_manifest.bin" -Force

# Nothing of the game may get into the package.
$bad = Get-ChildItem $Out -Recurse -File | Where-Object { $_.Name -match '\.(xex|xexp|xxx|bik|dxbc|iso)$' -or $_.Name -eq "pipelines.bin" }
if ($bad) { throw "game files in the package: $($bad.FullName -join ', ')" }

# The version the launcher will report: the release asset's name and its line in SHA256SUMS.txt depend on it.
$updates = Get-Content "$root\launcher\src\Updates.cs" -Raw
if ($updates -notmatch 'const string Version = "([^"]+)"') { throw "no Version in launcher\src\Updates.cs" }
$version = $Matches[1]
$zipPath = $null
if ($Zip) {
  # Upload it under this name: the launcher looks the asset's name up in SHA256SUMS.txt.
  $zipPath = Join-Path (Split-Path $Out) "ArmyOfTwo-v$version.zip"
  Remove-Item $zipPath -ErrorAction SilentlyContinue
  Push-Location (Split-Path $Out)
  & "$env:WINDIR\System32\tar.exe" -a -c -f $zipPath (Split-Path $Out -Leaf)
  $tarExit = $LASTEXITCODE
  Pop-Location
  if ($tarExit -ne 0) { throw "tar failed: $tarExit" }
}
$size = (Get-ChildItem $Out -Recurse | Measure-Object Length -Sum).Sum / 1MB
"release: $Out ({0:N0} MB)" -f $size

# avx1-version.aot, a release asset of its own next to the package: a zip with the game built for CPUs
# without AVX2, as armyoftworecomp.exe. Not named .zip: every launcher takes a release's first .zip for
# the package. Launchers in AVX1 mode download it (Updates.InstallAvx1) and offer updates only to
# releases that have it - upload it with every release.
$aot = Join-Path (Split-Path $Out) "avx1-version.aot"
$stage = Join-Path (Split-Path $Out) "avx1-stage"
if ($NoAvx1) {
  # A stale one next to the package would pass for this build's.
  Remove-Item $aot -ErrorAction SilentlyContinue
  "AVX1 build: none (-NoAvx1)"
} else {
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$build\armyoftworecomp_compat.exe" "$stage\armyoftworecomp.exe" -Force
Remove-Item $aot, "$aot.zip" -ErrorAction SilentlyContinue
Push-Location $stage
& "$env:WINDIR\System32\tar.exe" -a -c -f "$aot.zip" "armyoftworecomp.exe"
Pop-Location
if ($LASTEXITCODE -ne 0) { throw "tar failed: $LASTEXITCODE" }
Move-Item "$aot.zip" $aot
Remove-Item $stage -Recurse -Force
"AVX1 build: $aot ({0:N0} MB)" -f ((Get-Item $aot).Length / 1MB)
}

# SHA256SUMS.txt, a release asset too: launchers refuse a release newer than 1.4.3 without it
# (Updates.ExpectedHash). The format of sha256sum, so `sha256sum -c` checks it as well.
if ($Zip) {
  $sums = Join-Path (Split-Path $Out) "SHA256SUMS.txt"
  $assets = if ($NoAvx1) { @($zipPath) } else { @($zipPath, $aot) }
  $lines = foreach ($f in $assets) {
    "{0} *{1}" -f (Get-FileHash $f -Algorithm SHA256).Hash.ToLowerInvariant(), (Split-Path $f -Leaf)
  }
  [IO.File]::WriteAllText($sums, (($lines -join "`n") + "`n"))
  "Upload with the release: $($assets -join ', '), $sums"
}

exit 0
