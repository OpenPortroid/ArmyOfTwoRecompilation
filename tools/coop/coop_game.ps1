param(
  [string[]]$ArgsA = @(),
  [string[]]$ArgsB = @(),
  [string[]]$Common = @("--fullscreen=false", "--ao2_native_gpu_resolution_scale=1", "--log_to_files=true", "--ao2_coop_debug=true", "--ao2_mouse_debug=true"),
  [int]$BootWait = 45
)
# One-PC online co-op round into gameplay with built-in EA server: A hosts
# (127.0.0.1), B joins (127.0.0.2), both go Campaign -> Public Co-op through
# input.script; A's lobby starts the game once B joined (the menu's last "a").
# Per-instance extra flags in -ArgsA / -ArgsB. Logs in C:\aot2\work\runA|runB.
$exe = "C:\aot2\aot\out\build\win-amd64-release\armyoftworecomp.exe"
Get-Process armyoftworecomp -ErrorAction SilentlyContinue | Stop-Process -Force
$inst = @(
  @{N = "A"; Ip = "127.0.0.1"; Args = @("--ao2_coop_server=true") + $ArgsA},
  @{N = "B"; Ip = "127.0.0.2"; Args = @("--ao2_coop_host=127.0.0.1") + $ArgsB})
foreach ($i in $inst) {
  $dir = "C:\aot2\work\run$($i.N)"
  Remove-Item "$dir\input.script", "$dir\capture.trigger", "$dir\mouse_aim.log", "$dir\coop_debug.log", "$dir\cutscene_skip.log" -ErrorAction SilentlyContinue
  $a = @("--net_bind_ip=$($i.Ip)", "--net_gamertag=Player$($i.N)", "--user_data_root=$dir\user", "--cache_root=$dir\cache") + $Common + $i.Args
  $p = Start-Process -FilePath $exe -ArgumentList $a -WorkingDirectory $dir -PassThru
  "$($i.N) pid=$($p.Id)"
}
Start-Sleep $BootWait
$menu = "start wait8000 a wait6000 down wait1000 down wait1000 down wait1000 a wait8000 a"
Set-Content C:\aot2\work\runA\input.script $menu -NoNewline
Start-Sleep 50
Set-Content C:\aot2\work\runB\input.script $menu -NoNewline
Start-Sleep 50
"started"
