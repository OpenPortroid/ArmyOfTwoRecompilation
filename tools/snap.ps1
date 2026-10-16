param(
  [string]$Steps = "",
  [int]$Wait = 0,
  [string]$Name = "snap"
)
# Optional scripted input (input.script), wait, then a front buffer capture
# copied to tools\snaps\<Name>.png (main game folder instance).
$dir = "C:\aot2\aot\out\build\win-amd64-release"
$out = "C:\aot2\tools\snaps"
New-Item -ItemType Directory -Force $out | Out-Null
if ($Steps) { Set-Content -Path "$dir\input.script" -Value $Steps -NoNewline }
if ($Wait) { Start-Sleep -Milliseconds $Wait }
$before = Get-Date
New-Item "$dir\capture.trigger" -ItemType File -Force | Out-Null
for ($i = 0; $i -lt 40; $i++) {
  Start-Sleep -Milliseconds 250
  $idx = Get-Item "$dir\capture\index.txt" -ErrorAction SilentlyContinue
  if ($idx -and $idx.LastWriteTime -gt $before -and -not (Test-Path "$dir\capture.trigger")) { break }
}
Start-Sleep -Milliseconds 500
Push-Location $dir
python C:\aot2\tools\capture_to_png.py capture | Out-Null
Pop-Location
$png = Get-ChildItem "$dir\capture\*_1F5F8000_color.png" | Sort-Object Name | Select-Object -Last 1
if ($png) { Copy-Item $png.FullName "$out\$Name.png" -Force; "$out\$Name.png" } else { "no front buffer" }
