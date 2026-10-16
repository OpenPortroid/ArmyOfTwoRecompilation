# Dot-source: . C:\aot2\tools\winfind.ps1 ; Find-GameWindow -ProcessId <pid> [-Title <substring>] [-Strict]
# Returns the HWND of a visible top-level window of the process. Without -Title,
# the native renderer's shadow window ("AO2 native renderer") is skipped.
# -Strict: return IntPtr.Zero instead of falling back to MainWindowHandle.
if (-not ("WinFind" -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class WinFind {
  delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr p);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  public static IntPtr Find(uint pid, string title, string exclude) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, p) => {
      uint wp; GetWindowThreadProcessId(h, out wp);
      if (wp != pid || !IsWindowVisible(h)) return true;
      var sb = new StringBuilder(256); GetWindowText(h, sb, 256);
      string t = sb.ToString();
      if (t.Length == 0) return true;
      if (!string.IsNullOrEmpty(title) && t.IndexOf(title, StringComparison.OrdinalIgnoreCase) < 0) return true;
      if (string.IsNullOrEmpty(title) && !string.IsNullOrEmpty(exclude) && t.IndexOf(exclude, StringComparison.OrdinalIgnoreCase) >= 0) return true;
      found = h; return false;
    }, IntPtr.Zero);
    return found;
  }
}
"@
}
function Find-GameWindow([int]$ProcessId, [string]$Title = "", [switch]$Strict) {
  $h = [WinFind]::Find([uint32]$ProcessId, $Title, "native renderer")
  if ($h -eq [IntPtr]::Zero -and -not $Strict) {
    $p = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($p) { $h = $p.MainWindowHandle }
  }
  return $h
}
