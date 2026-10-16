param([int]$TimeoutSec = 30)
# Renames the game windows of the co-op test instances (started by
# coop_start.ps1 from C:\aot2\work\run<Name>) to "A: HOST" / "B: CLIENT" so the
# person driving them can tell which is which.
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class CoopWin {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowText(IntPtr h, string s);
}
"@ -ErrorAction SilentlyContinue
$labels = @{ "runA" = "Army of Two - A: ХОСТ (PlayerA)"; "runB" = "Army of Two - B: КЛИЕНТ (PlayerB)" }
$names = @{}
Get-CimInstance Win32_Process -Filter "Name = 'armyoftworecomp.exe'" | ForEach-Object {
  foreach ($k in $labels.Keys) { if ($_.CommandLine -match "\\$k\\user") { $names[[int]$_.ProcessId] = $labels[$k] } }
}
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$done = @{}
while ((Get-Date) -lt $deadline -and $done.Count -lt $names.Count) {
  $cb = [CoopWin+EnumProc]{ param($h, $p)
    $procId = 0; [CoopWin]::GetWindowThreadProcessId($h, [ref]$procId) | Out-Null
    if ($names.ContainsKey([int]$procId) -and [CoopWin]::IsWindowVisible($h)) {
      $sb = New-Object System.Text.StringBuilder 256; [CoopWin]::GetWindowText($h, $sb, 256) | Out-Null
      if ($sb.ToString() -like "armyoftworecomp*") { [CoopWin]::SetWindowText($h, $names[[int]$procId]) | Out-Null; $done[[int]$procId] = 1 }
    }
    return $true }
  [CoopWin]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
  if ($done.Count -lt $names.Count) { Start-Sleep -Milliseconds 500 }
}
Get-Process armyoftworecomp | Select-Object Id, MainWindowTitle
