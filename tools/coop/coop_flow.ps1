param(
  [string[]]$ExtraArgs = @("--log_to_files=true", "--ao2_coop_debug=true", "--mnk_mode=false", "--ao2_mouse_aim=false"),
  [int]$BootWait = 40,
  [switch]$NoStart
)
# Full local co-op round: starts the emulator and both instances, A hosts a
# Public Co-op game, B searches and joins, then A presses Start Game in the
# lobby. Front buffers go to tools\coop\shots\flow_*.png.
$snap = "C:\aot2\tools\coop\coop_snap.ps1"
$emuLog = "C:\aot2\tools\ea_emu.log"
& C:\aot2\tools\coop\coop_start.ps1 -ExtraArgs $ExtraArgs
$logStart = (Get-Item $emuLog).Length

function WaitEmu([string]$pattern, [int]$seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    $fs = [IO.File]::Open($emuLog, 'Open', 'Read', 'ReadWrite')
    $fs.Seek($script:logStart, 'Begin') | Out-Null
    $text = (New-Object IO.StreamReader($fs)).ReadToEnd()
    $fs.Close()
    if ($text -match $pattern) { return $true }
  }
  return $false
}

Start-Sleep -Seconds $BootWait
$menu = "start wait8000 a wait6000 down wait1000 down wait1000 down wait1000 a wait8000 a"
& $snap -Inst A -Steps $menu
if (-not (WaitEmu "game \d+ created by" 90)) { "host did not create a game"; & $snap -Inst A -Name flow_host; return }
"host game created"
& $snap -Inst B -Steps $menu
if (-not (WaitEmu "<- PENT .*'PID': '2'" 90)) { "client did not join"; & $snap -Inst B -Name flow_join; return }
"client joined"
Start-Sleep -Seconds 5
& $snap -Inst A -Name flow_lobby
if (-not $NoStart) {
  & $snap -Inst A -Steps "a"
  "host started the game"
}
Get-Process armyoftworecomp -ErrorAction SilentlyContinue | Select-Object Id, Responding
