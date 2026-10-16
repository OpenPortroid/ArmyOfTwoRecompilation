param(
  [string[]]$GameArgs = @(),
  [int]$BootWait = 40,
  [int]$LoadWait = 35,
  [int]$SettleSec = 10,
  [string]$WorkDir = ""
)
# Boots the game and continues the campaign from the last checkpoint:
# Start -> Campaign -> Single Player -> (New Campaign is selected by default,
# so "down") Continue From Checkpoint -> Select Difficulty: Recruit -> loading.
# Never let the last "a" land on New Campaign: it overwrites the progress.
# Verified frame by frame with tools\menu_steps.ps1 (2026-10-03).
# Input goes through the game's scripted pad (input.script, see
# aot/src/input_script.cpp), so the window does not need focus and no keys
# reach other applications.
# -WorkDir: another working directory (input.script, logs, shader_cache).
$exe = if ($env:AO2_EXE) { $env:AO2_EXE } else { "C:\aot2\aot\out\build\win-amd64-release\armyoftworecomp.exe" }
$dir = if ($WorkDir) { $WorkDir } else { Split-Path $exe }
Get-Process armyoftworecomp -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Remove-Item "$dir\input.script" -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -ArgumentList $GameArgs -WorkingDirectory $dir -PassThru
Start-Sleep -Seconds $BootWait
if ($p.HasExited) { "game exited during boot"; return }
Set-Content -Path "$dir\input.script" -Value "start wait6000 a wait5000 a wait6000 down wait1500 a wait6000 a" -NoNewline
Start-Sleep -Seconds (30 + $LoadWait + $SettleSec)
if ($p.HasExited) { "game exited"; return }
"pid=$($p.Id)"
