param([int]$Seconds = 5)
# CPU load of the running game: whole process and the busiest threads (% of
# one core), and CPU time per presented frame (ms/frame, FPS from native_gpu.log).
$log = "C:\aot2\aot\out\build\win-amd64-release\native_gpu.log"
$p = Get-Process armyoftworecomp
$t0 = @{}
foreach ($t in $p.Threads) { $t0[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds }
$c0 = $p.TotalProcessorTime.TotalMilliseconds
Start-Sleep $Seconds
$p = Get-Process armyoftworecomp
$fpsMatch = Select-String -Path $log -Pattern " fps \|" | Select-Object -Last 1
$fps = if ($fpsMatch) { [double]($fpsMatch.Line.Trim().Split(' ')[0]) } else { 0.0 }
$proc = ($p.TotalProcessorTime.TotalMilliseconds - $c0) / $Seconds
"process: {0}% of one core, {1:N2} ms/frame at {2} fps" -f [math]::Round($proc / 10, 0), ($proc / [math]::Max($fps, 1)), $fps
$rows = foreach ($t in $p.Threads) {
  if ($t0.ContainsKey($t.Id)) {
    [pscustomobject]@{ Id = $t.Id; Ms = ($t.TotalProcessorTime.TotalMilliseconds - $t0[$t.Id]) / $Seconds }
  }
}
"threads ms/frame: " + (($rows | Sort-Object Ms -Descending | Select-Object -First 4 | ForEach-Object { "{0:N2}" -f ($_.Ms / [math]::Max($fps, 1)) }) -join "  ")
