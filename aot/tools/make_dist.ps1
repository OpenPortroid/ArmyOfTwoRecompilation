param(
  [string]$Out = "C:\aot2\dist\ArmyOfTwo",
  [switch]$NoAssets,   # program only (for updating a copy that already has assets\)
  [switch]$Zip         # also pack <Out>.zip (with Windows' tar, zip64)
)
# Builds a self-contained copy of the game for another PC: the exe and every
# DLL it needs (Visual C++ runtime, D3DCompiler), settings, the game files,
# the compiled shader cache, the co-op lobby server, launchers and a README.
# The exact build's PDB is kept in C:\aot2\dist\symbols\<date> to read crash
# dumps from the other PC.
$ErrorActionPreference = "Stop"
$build = "C:\aot2\aot\out\build\win-amd64-release"
$repo = "C:\aot2\aot"
$crt = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" |
  Sort-Object FullName | Select-Object -Last 1

New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($f in "armyoftworecomp.exe", "rexruntime.dll", "ao2_lobby_server.exe", "gamecontrollerdb.txt") {
  Copy-Item "$build\$f" $Out -Force
}
foreach ($f in "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
                "msvcp140_atomic_wait.dll", "concrt140.dll") {
  Copy-Item "$($crt.FullName)\$f" $Out -Force
}
Copy-Item "C:\Windows\System32\D3DCompiler_47.dll" $Out -Force

New-Item -ItemType Directory -Force "$Out\settings" | Out-Null
Copy-Item "$repo\settings\*" "$Out\settings" -Force
robocopy "$repo\localization" "$Out\localization" /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy "$build\shader_cache" "$Out\shader_cache" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if (-not $NoAssets) {
  robocopy "$repo\assets" "$Out\assets" /E /NFL /NDL /NJH /NJS /NP /MT:8 | Out-Null
}
Copy-Item "$repo\tools\dist\*" $Out -Force

$sym = "C:\aot2\dist\symbols\" + (Get-Date -Format "yyyy-MM-dd_HHmm")
New-Item -ItemType Directory -Force $sym | Out-Null
Copy-Item "$build\armyoftworecomp.exe", "$build\armyoftworecomp.pdb", "$build\rexruntime.dll" $sym -Force

if ($Zip) {
  $zipPath = "$Out.zip"
  Remove-Item $zipPath -ErrorAction SilentlyContinue
  Push-Location (Split-Path $Out)
  & "$env:WINDIR\System32\tar.exe" -a -c -f $zipPath (Split-Path $Out -Leaf)
  Pop-Location
}
$size = (Get-ChildItem $Out -Recurse | Measure-Object Length -Sum).Sum / 1GB
"dist: $Out ({0:N2} GB), symbols: $sym" -f $size

exit 0
