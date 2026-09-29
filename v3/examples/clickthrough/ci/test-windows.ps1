# Transparent-window click-through E2E for examples/clickthrough (Windows).
# Drives the cursor with user32 and asserts on the app's stdout markers.
# Scene constants must stay in sync with assets/index.html:
#   circle centre (200,160), transparent interior (460,320), card button (118,537)
# Usage: test-windows.ps1 -AppPath <path-to-clickthrough.exe>
param(
  [Parameter(Mandatory = $true)][string]$AppPath
)

$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Input {
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lparam);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$log = Join-Path $env:TEMP ("clickthrough-log-" + [guid]::NewGuid().ToString('N') + ".txt")
$errlog = $log + ".err"
$script:proc = $null

function Cleanup {
  if ($script:proc -and -not $script:proc.HasExited) { $script:proc.Kill() }
}
trap { Cleanup }

# Wails logs markers to stderr on some paths — always search both streams.
function Dump-Log {
  (Get-Content $log, $errlog -ErrorAction SilentlyContinue) -join "`n"
}

function Wait-LogMarker([string]$pattern, [int]$timeoutSec = 30) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    if ($script:proc -and $script:proc.HasExited) { throw "app exited early; log:`n$(Dump-Log)" }
    if ((Test-Path $log) -and (Select-String -Path $log, $errlog -Pattern $pattern -Quiet)) {
      Write-Output "PASS: log marker '$pattern'"
      return
    }
    Start-Sleep -Milliseconds 300
  }
  throw "timeout waiting for log marker: $pattern; log:`n$(Dump-Log)"
}

function Wait-Flip([string]$want, [int]$baseline) {
  $deadline = (Get-Date).AddSeconds(20)
  while ((Get-Date) -lt $deadline) {
    if ($script:proc -and $script:proc.HasExited) { throw "app exited early" }
    $matches = Select-String -Path $log, $errlog -Pattern ("ignoring=" + $want) -ErrorAction SilentlyContinue
    if ($matches -and $matches.Count -gt $baseline) { Write-Output "PASS: flip ignoring=$want"; return }
    Start-Sleep -Milliseconds 300
  }
  throw "timeout waiting for flip ignoring=$want"
}

function Count-Flip([string]$want) {
  $m = Select-String -Path $log, $errlog -Pattern ("ignoring=" + $want) -ErrorAction SilentlyContinue
  if ($m) { return $m.Count } else { return 0 }
}

function Click-At([int]$x, [int]$y) {
  [Win32Input]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 500
  [Win32Input]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)  # LEFTDOWN
  Start-Sleep -Milliseconds 60
  [Win32Input]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)  # LEFTUP
  Start-Sleep -Milliseconds 400
}

# --- launch -----------------------------------------------------------------
$proc = Start-Process -FilePath $AppPath -RedirectStandardOutput $log `
  -RedirectStandardError ($log + ".err") -WindowStyle Hidden -PassThru

# --- 1. mask uploaded --------------------------------------------------------
Wait-LogMarker "mask uploaded"

# --- 2. locate the overlay: top-level window of our process sized 480x640 ---
# (frameless windows may not expose their title to FindWindow)
$script:overlayHwnd = [IntPtr]::Zero
$cb = [Win32Input+EnumWindowsProc]{
  param($h, $l)
  $winPid = 0
  [Win32Input]::GetWindowThreadProcessId($h, [ref]$winPid) | Out-Null
  if ($winPid -eq $script:proc.Id -and [Win32Input]::IsWindowVisible($h)) {
    $r = New-Object Win32Input+RECT
    [Win32Input]::GetWindowRect($h, [ref]$r) | Out-Null
    if (($r.Right - $r.Left) -eq 480 -and ($r.Bottom - $r.Top) -eq 640) {
      $script:overlayHwnd = $h
      return $false
    }
  }
  return $true
}
[Win32Input]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:overlayHwnd -eq [IntPtr]::Zero) { throw "overlay window (480x640) not found for pid $($script:proc.Id)" }
$rect = New-Object Win32Input+RECT
[Win32Input]::GetWindowRect($script:overlayHwnd, [ref]$rect) | Out-Null
$ox = $rect.Left; $oy = $rect.Top
Write-Output "overlay window $script:overlayHwnd at $ox,$oy"

# --- 3. opaque region stays interactive ---------------------------------------
$b = Count-Flip "false"
Click-At ($ox + 200) ($oy + 160)
Wait-Flip "false" $b

# --- 4. transparent region passes through to the underlay ---------------------
$b = Count-Flip "true"
Click-At ($ox + 460) ($oy + 320)
Wait-Flip "true" $b
Wait-LogMarker "underlay-clicks=1"

# --- 5. opaque card receives clicks after flipping back -----------------------
$b = Count-Flip "false"
Click-At ($ox + 118) ($oy + 537)
Wait-Flip "false" $b
Wait-LogMarker "card-clicks=1"

Cleanup
Write-Output "ALL PASS"
exit 0
