param([int]$ProcessId, [string]$Keys, [int]$DelayMs = 400, [int]$HoldMs = 250)
# Keys: space-separated names: w a s d space back enter tab esc x z f3, or waitNNN.
# Input is injected globally (keybd_event), so every key is sent only while the
# game window is alive and in the foreground; otherwise the script aborts
# without pressing anything (keys must never land in another app).
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class K {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@
$map = @{ w = 0x11; a = 0x1E; s = 0x1F; d = 0x20; space = 0x39; back = 0x0E; enter = 0x1C; tab = 0x0F; esc = 0x01; x = 0x2D; z = 0x2C; f3 = 0x3D }
$vk  = @{ w = 0x57; a = 0x41; s = 0x53; d = 0x44; space = 0x20; back = 0x08; enter = 0x0D; tab = 0x09; esc = 0x1B; x = 0x58; z = 0x5A; f3 = 0x72 }
. C:\aot2\tools\winfind.ps1

function Fail([string]$why) { Write-Output "aborted: $why"; exit 1 }

if (-not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) { Fail "process $ProcessId is not running" }
$h = Find-GameWindow -ProcessId $ProcessId -Strict
if (-not $h -or $h -eq [IntPtr]::Zero) { Fail "game window not found" }

function Ensure-Focus {
  if (-not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) { Fail "process exited" }
  if (-not [K]::IsWindow($h)) { Fail "game window closed" }
  if ([K]::GetForegroundWindow() -eq $h) { return }
  [K]::ShowWindow($h, 9) | Out-Null
  [K]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 300
  if ([K]::GetForegroundWindow() -ne $h) { Fail "game window is not in the foreground" }
}

Ensure-Focus
foreach ($k in $Keys.Split(' ')) {
  if ($k -match '^wait(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
  if (-not $map.ContainsKey($k)) { Fail "unknown key '$k'" }
  Ensure-Focus
  $sc = [byte]$map[$k]; $v = [byte]$vk[$k]
  [K]::keybd_event($v, $sc, 0x0008, [UIntPtr]::Zero)               # KEYEVENTF_SCANCODE
  Start-Sleep -Milliseconds $HoldMs
  [K]::keybd_event($v, $sc, 0x0008 -bor 0x0002, [UIntPtr]::Zero)   # key up
  Start-Sleep -Milliseconds $DelayMs
}
"sent"
