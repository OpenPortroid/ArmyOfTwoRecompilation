param(
  [string[]]$Instances = @("A", "B"),
  [string[]]$ExtraArgs = @(),
  [switch]$PythonEmu,
  [switch]$Fresh
)
# Local online co-op test bench: one game instance per name in -Instances,
# each in its own working directory C:\aot2\work\run<Name> (input.script,
# capture.trigger, logs) and bound to its own loopback address (A = 127.0.0.1,
# B = 127.0.0.2, ...). The first instance hosts the built-in EA server
# (--ao2_coop_server), the others connect to it (--ao2_coop_host);
# -PythonEmu uses the old tools\ea_emu.py server instead.
# -Fresh re-copies the user data (saves/profile) from the default profile.
$exe = "C:\aot2\aot\out\build\win-amd64-release\armyoftworecomp.exe"
$cache = "C:\aot2\aot\out\build\win-amd64-release\shader_cache"
$userSrc = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "armyoftworecomp"

Get-Process armyoftworecomp -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Get-CimInstance Win32_Process -Filter "Name like 'python%'" |
  Where-Object { $_.CommandLine -match 'ea_emu' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -Confirm:$false }
if ($PythonEmu) {
  Start-Process -FilePath python -ArgumentList "C:\aot2\tools\ea_emu.py" -WorkingDirectory "C:\aot2\tools" `
    -WindowStyle Hidden -RedirectStandardError "C:\aot2\tools\ea_emu.err"
}

$i = 1
foreach ($name in $Instances) {
  $dir = "C:\aot2\work\run$name"
  New-Item -ItemType Directory -Force $dir | Out-Null
  Remove-Item "$dir\input.script", "$dir\capture.trigger" -ErrorAction SilentlyContinue
  if ($Fresh -or -not (Test-Path "$dir\user")) {
    Remove-Item -Recurse -Force "$dir\user" -ErrorAction SilentlyContinue
    Copy-Item -Recurse $userSrc "$dir\user"
    Remove-Item -Recurse -Force "$dir\user\cache" -ErrorAction SilentlyContinue
  }
  if (-not (Test-Path "$dir\shader_cache")) { Copy-Item -Recurse $cache "$dir\shader_cache" }
  $ip = "127.0.0.$i"
  $gargs = @("--fullscreen=false", "--net_bind_ip=$ip", "--net_gamertag=Player$name",
            "--user_data_root=$dir\user", "--cache_root=$dir\cache",
            "--ao2_native_gpu_resolution_scale=1") + $ExtraArgs
  if ($PythonEmu) { $gargs += "--net_title_server_ip=127.0.0.1" }
  elseif ($i -eq 1) { $gargs += "--ao2_coop_server=true" }
  else { $gargs += "--ao2_coop_host=127.0.0.1" }
  $p = Start-Process -FilePath $exe -ArgumentList $gargs -WorkingDirectory $dir -PassThru
  "$name pid=$($p.Id) ip=$ip dir=$dir"
  $i++
}
