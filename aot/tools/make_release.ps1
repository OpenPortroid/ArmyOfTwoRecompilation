param(
  [string]$Out = "",   # default: <repo>\dist\release\ArmyOfTwo
  [switch]$Zip         # also pack <Out>.zip
)
# Builds the public release package: only our own programs, no game files.
# Unlike make_dist.ps1 there are no assets and no shader cache (both derive
# from the game): the launcher installs the game from the user's own copy
# (ao2_install.exe reads the disc image and the TU / DLC packages) and the
# shader cache is built on the user's PC as they play.
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$root = (Resolve-Path "$repo\..").Path
$build = "$repo\out\build\win-amd64-release"
if (-not $Out) { $Out = "$root\dist\release\ArmyOfTwo" }
$crt = Get-ChildItem "C:\Program Files*\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" |
  Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "Visual C++ redistributable folder not found" }
$launcher = "$root\launcher\bin\ArmyOfTwoLauncher.exe"
foreach ($f in "$build\armyoftworecomp.exe", "$build\rexruntime.dll", "$build\ao2_install.exe", $launcher) {
  if (-not (Test-Path $f)) { throw "not built: $f" }
}

if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($f in "armyoftworecomp.exe", "rexruntime.dll", "ao2_install.exe", "ao2_lobby_server.exe", "gamecontrollerdb.txt") {
  Copy-Item "$build\$f" $Out -Force
}
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
Copy-Item "$repo\settings\*" "$Out\settings" -Force
# The Russian text and the Tektur font (our own translation, not the game's files).
robocopy "$repo\localization" "$Out\localization" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy localization failed: $LASTEXITCODE" }
Copy-Item "$repo\tools\dist\README_release.txt" "$Out\README.txt" -Force

# Nothing of the game may get into the package.
$bad = Get-ChildItem $Out -Recurse -File | Where-Object { $_.Name -match '\.(xex|xexp|xxx|bik|dxbc|iso)$' -or $_.Name -eq "pipelines.bin" }
if ($bad) { throw "game files in the package: $($bad.FullName -join ', ')" }

if ($Zip) {
  $zipPath = "$Out.zip"
  Remove-Item $zipPath -ErrorAction SilentlyContinue
  Push-Location (Split-Path $Out)
  & "$env:WINDIR\System32\tar.exe" -a -c -f $zipPath (Split-Path $Out -Leaf)
  Pop-Location
}
$size = (Get-ChildItem $Out -Recurse | Measure-Object Length -Sum).Sum / 1MB
"release: $Out ({0:N0} MB)" -f $size

exit 0
