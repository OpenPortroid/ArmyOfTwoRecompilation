param(
  [string[]]$GameArgs = @(),
  [string]$Nav = "coop",   # none | campaign | coop (Private Co-op) | public (Public Co-op)
  [string]$WorkDir = "C:\aot2\aot\out\build\win-amd64-release",
  [int]$BootWait = 40
)
# Launches the game and navigates: title -> Campaign Mode -> co-op entry.
# Navigation stops as soon as keys.ps1 can't deliver input to the game window.
$p = Start-Process -FilePath "C:\aot2\aot\out\build\win-amd64-release\armyoftworecomp.exe" `
  -ArgumentList $GameArgs -WorkingDirectory $WorkDir -PassThru
Start-Sleep -Seconds $BootWait
if ($Nav -ne "none") {
  $steps = @("wait500 x wait1000 enter wait6000", "wait800 space wait5000")
  if ($Nav -eq "coop") { $steps += "wait800 s wait800 s wait800 space wait6000" }
  elseif ($Nav -eq "public") { $steps += "wait800 s wait800 s wait800 s wait800 space wait6000" }
  foreach ($s in $steps) {
    $r = & C:\aot2\tools\keys.ps1 -ProcessId $p.Id -Keys $s -HoldMs 300
    if ($r -match '^aborted') { Write-Output "navigation stopped: $r"; break }
  }
}
"pid=$($p.Id)"
