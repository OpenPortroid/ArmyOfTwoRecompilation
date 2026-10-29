param(
  [string]$Exe = "C:\aot2\aot\out\build\win-amd64-release\armyoftworecomp.exe",
  [string]$Label = "build",
  [string[]]$GameArgs = @("--fullscreen=false", "--ao2_god_mode=true", "--ao2_fps_unlock=true", "--ao2_fps_target=240")
)
# Checkpoint benchmark: loads the checkpoint with $Exe (working directory =
# the release folder), stands still for a while, then reports the scene
# render time (perf_probe.log), FPS and CPU load per thread.
$dir = "C:\aot2\aot\out\build\win-amd64-release"
Remove-Item "$dir\perf_probe.log" -ErrorAction SilentlyContinue
$env:AO2_EXE = $Exe
& C:\aot2\tools\to_checkpoint.ps1 -WorkDir $dir -GameArgs $GameArgs | Out-Null
Remove-Item Env:AO2_EXE
Start-Sleep 12
"=== $Label"
Get-Content "$dir\perf_probe.log" -ErrorAction SilentlyContinue | Select-Object -Last 2
& C:\aot2\tools\cpuload.ps1 -Name ([IO.Path]::GetFileNameWithoutExtension($Exe))
