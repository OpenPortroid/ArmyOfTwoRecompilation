param(
  [Parameter(Mandatory)][string]$Inst,
  [string]$Name = "",
  [string]$Steps = "",
  [int]$Wait = 0
)
# Optionally sends scripted pad steps to instance <Inst> (input.script), waits
# -Wait seconds, then saves its front buffer as C:\aot2\tools\coop\shots\<Inst>_<Name>.png.
$dir = "C:\aot2\work\run$Inst"
$out = "C:\aot2\tools\coop\shots"
New-Item -ItemType Directory -Force $out | Out-Null
if ($Steps) { Set-Content -Path "$dir\input.script" -Value $Steps -NoNewline }
if ($Wait) { Start-Sleep -Seconds $Wait }
if (-not $Name) { return }
Remove-Item -Recurse -Force "$dir\capture" -ErrorAction SilentlyContinue
New-Item "$dir\capture.trigger" -ItemType File -Force | Out-Null
for ($i = 0; $i -lt 60 -and -not (Test-Path "$dir\capture\index.txt"); $i++) { Start-Sleep -Milliseconds 250 }
Start-Sleep -Milliseconds 1500
python C:\aot2\tools\capture_to_png.py "$dir\capture" | Out-Null
$front = Get-ChildItem "$dir\capture\*_1F5F8000_color.png" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
if ($front) { Copy-Item $front.FullName "$out\${Inst}_$Name.png"; "$out\${Inst}_$Name.png" }
else { "no front buffer captured for $Inst" }
