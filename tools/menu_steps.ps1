param(
  [string[]]$Steps = @("start", "a", "a", "a"),
  [int]$StepWait = 6,
  [int]$BootWait = 40,
  [string[]]$GameArgs = @("--fullscreen=false"),
  [string]$Out = "C:\aot2\tools\menu"
)
# Boots the game and walks the menus one scripted pad step at a time
# (input.script), saving the front buffer after every step as
# <Out>\stepN.png (via capture.trigger + capture_to_png.py).
$dir = "C:\aot2\aot\out\build\win-amd64-release"
Get-Process armyoftworecomp -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Remove-Item "$dir\input.script" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $Out | Out-Null
Remove-Item "$Out\*.png" -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$dir\armyoftworecomp.exe" -ArgumentList $GameArgs -WorkingDirectory $dir -PassThru
Start-Sleep -Seconds $BootWait
function Snap([string]$name) {
  Remove-Item -Recurse -Force "$dir\capture" -ErrorAction SilentlyContinue
  New-Item "$dir\capture.trigger" -ItemType File | Out-Null
  for ($i = 0; $i -lt 40 -and -not (Test-Path "$dir\capture\index.txt"); $i++) { Start-Sleep -Milliseconds 250 }
  Start-Sleep -Milliseconds 1500
  python C:\aot2\tools\capture_to_png.py "$dir\capture" | Out-Null
  $front = Get-ChildItem "$dir\capture\*_1F5F8000_color.png" | Sort-Object Name | Select-Object -Last 1
  if ($front) { Copy-Item $front.FullName "$Out\$name.png" }
}
Snap "step0"
$n = 1
foreach ($s in $Steps) {
  if ($p.HasExited) { "game exited"; return }
  Set-Content -Path "$dir\input.script" -Value $s -NoNewline
  Start-Sleep -Seconds $StepWait
  Snap "step$n"
  $n++
}
"pid=$($p.Id)"
