param([int]$ProcessId, [string]$Out = "C:\aot2\tools\shot.png", [string]$Title = "")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int L, T, R, B; }
}
"@
[W]::SetProcessDPIAware() | Out-Null
. C:\aot2\tools\winfind.ps1
$h = Find-GameWindow -ProcessId $ProcessId -Title $Title
$r = New-Object W+RECT
[W]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
[W]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$small = New-Object System.Drawing.Bitmap $bmp, ([int]($w/2)), ([int]($hh/2))
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"saved ${w}x${hh}"
