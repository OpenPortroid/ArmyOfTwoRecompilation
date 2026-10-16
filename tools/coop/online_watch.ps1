param([string]$Dir = "C:\aot2\dist\full\ArmyOfTwo", [int]$Rounds = 6)
# While the user plays online co-op: once the lockstep is in network mode
# (coop_debug.log "+16=1"), profile the game's main and render threads every
# 40 s and collect the frame stats -> tools\coop\online_watch.txt.
$out = "C:\aot2\tools\coop\online_watch.txt"
"watch start $(Get-Date -Format HH:mm:ss)" | Set-Content $out
while ($true) {
  $p = Get-Process armyoftworecomp -ErrorAction SilentlyContinue | Select-Object -First 1
  # Lockstep frames advancing in network mode (not the frozen counter of the
  # intro cutscene), in the log of the running process.
  $fresh = $p -and (Get-Item "$Dir\coop_debug.log" -ErrorAction SilentlyContinue).LastWriteTime -gt $p.StartTime
  if ($fresh -and (Select-String -Path "$Dir\coop_debug.log" -Pattern "frame=([6-9]\d\d|\d{4,}) peer=\d+ \+16=1 " -Quiet)) { break }
  Start-Sleep 5
}
"online at $(Get-Date -Format HH:mm:ss), pid $($p.Id)" | Add-Content $out
for ($i = 0; $i -lt $Rounds; $i++) {
  if ($p.HasExited) { "game exited" | Add-Content $out; break }
  "=== round $i $(Get-Date -Format HH:mm:ss)" | Add-Content $out
  Get-Content "$Dir\coop_debug.log" -Tail 4 | Add-Content $out
  Select-String -Path "$Dir\native_gpu.log" -Pattern " fps \|" | Select-Object -Last 3 | ForEach-Object { $_.Line.Substring(0, 12) } | Add-Content $out
  foreach ($t in "Main XThread", "XThread") {
    "--- $t" | Add-Content $out
    python C:\aot2\tools\sampler.py $p.Id $t 5 500 --top 25 --ours 2>&1 | Add-Content $out
  }
  Start-Sleep 35
}
"watch end $(Get-Date -Format HH:mm:ss)" | Add-Content $out
