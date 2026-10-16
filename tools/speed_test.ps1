param(
  [string[]]$GameArgs = @("--fullscreen=false"),
  [string]$Tag = "run",
  [int[]]$Offsets = @(5, 15, 25)
)
# Game speed check: continue from the checkpoint, detect the end of loading
# (first stats line with more than 600 draws in native_gpu.log), then save the
# front buffer at fixed real-time offsets as tools\speed\<Tag>_<offset>.png.
$dir = "C:\aot2\aot\out\build\win-amd64-release"
$out = "C:\aot2\tools\speed"
New-Item -ItemType Directory -Force $out | Out-Null
Get-Process armyoftworecomp -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Remove-Item "$dir\input.script" -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$dir\armyoftworecomp.exe" -ArgumentList $GameArgs -WorkingDirectory $dir -PassThru
Start-Sleep -Seconds 40
Set-Content -Path "$dir\input.script" -Value "start wait6000 a wait5000 a wait6000 down wait1500 a wait6000 a" -NoNewline
$start = $null
for ($i = 0; $i -lt 600 -and -not $start; $i++) {
  Start-Sleep -Milliseconds 100
  $line = Get-Content "$dir\native_gpu.log" -Tail 3 -ErrorAction SilentlyContinue | Select-String "native frame \d+: draws=(\d+)" | Select-Object -Last 1
  if ($line -and [int]$line.Matches[0].Groups[1].Value -gt 600) { $start = Get-Date }
}
if (-not $start) { "gameplay not reached"; return }
foreach ($o in $Offsets) {
  $target = $start.AddSeconds($o)
  while ((Get-Date) -lt $target) { Start-Sleep -Milliseconds 50 }
  Remove-Item -Recurse -Force "$dir\capture" -ErrorAction SilentlyContinue
  New-Item "$dir\capture.trigger" -ItemType File | Out-Null
  for ($k = 0; $k -lt 40 -and -not (Test-Path "$dir\capture\index.txt"); $k++) { Start-Sleep -Milliseconds 50 }
  Start-Sleep -Milliseconds 1500
  python C:\aot2\tools\capture_to_png.py "$dir\capture" | Out-Null
  $f = Get-ChildItem "$dir\capture\*_1F5F8000_color.png" | Sort-Object Name | Select-Object -Last 1
  if ($f) { Copy-Item $f.FullName "$out\$($Tag)_$o.png" }
}
"done pid=$($p.Id)"
