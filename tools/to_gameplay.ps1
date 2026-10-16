param(
  [string[]]$GameArgs = @(),
  [int]$SettleSec = 10
)
# Boots the game and reaches gameplay: Campaign -> Single Player -> New Campaign
# -> Rios -> Recruit -> accept checkpoint overwrite -> skip intro movie.
# Stops at the first step whose keys could not be delivered to the game.
Get-Process armyoftworecomp -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
$out = & C:\aot2\tools\run.ps1 -Nav campaign -BootWait 40 -GameArgs $GameArgs
$last = ($out | Select-Object -Last 1)
if ($last -notmatch '^pid=(\d+)$') { "run failed: $last"; return }
$procId = [int]$Matches[1]
$steps = @(
  "wait800 space wait5000",     # Single Player
  "wait800 space wait4000",     # New Campaign
  "wait800 space wait4000",     # Rios
  "wait800 space wait3000",     # Recruit
  "wait800 space wait40000",    # Accept overwrite, load
  "wait800 x wait2000 space wait2000 enter wait30000"  # skip intro movie
)
foreach ($s in $steps) {
  $r = & C:\aot2\tools\keys.ps1 -ProcessId $procId -Keys $s -HoldMs 300
  if ($r -match '^aborted') { "stopped: $r"; "pid=$procId"; return }
}
Start-Sleep -Seconds $SettleSec
if (Get-Process -Id $procId -ErrorAction SilentlyContinue) {
  & C:\aot2\tools\shot.ps1 -ProcessId $procId | Out-Null
}
"pid=$procId"
