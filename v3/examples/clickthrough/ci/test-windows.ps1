# Transparent-window click-through E2E for examples/clickthrough (Windows).
# Drives the cursor with user32 and asserts on the app's stdout markers.
# Scene constants must stay in sync with assets/index.html:
#   circle centre (200,160), transparent interior (460,320), card button (118,537)
# Usage: test-windows.ps1 -AppPath <path-to-clickthrough.exe>
param(
  [Parameter(Mandatory = $true)][string]$AppPath
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Input {
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$log = Join-Path $env:TEMP ("clickthrough-log-" + [guid]::NewGuid().ToString('N') + ".txt")
$script:proc = $null

function Cleanup {
  if ($script:proc -and -not $script:proc.HasExited) { $script:proc.Kill() }
}
trap { Cleanup }

function Wait-LogMarker([string]$pattern, [int]$timeoutSec = 30) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    if ($script:proc -and $script:proc.HasExited) { throw "app exited early; log:`n$(Get-Content $log -Raw)" }
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern $pattern -Quiet)) {
      Write-Output "PASS: log marker '$pattern'"
      return
    }
    Start-Sleep -Milliseconds 300
  }
  throw "timeout waiting for log marker: $pattern; log:`n$(Get-Content $log -Raw -ErrorAction SilentlyContinue)"
}

function Wait-Flip([string]$want, [int]$baseline) {
  $deadline = (Get-Date).AddSeconds(20)
  while ((Get-Date) -lt $deadline) {
    if ($script:proc -and $script:proc.HasExited) { throw "app exited early" }
    $matches = Select-String -Path $log -Pattern ("ignoring=" + $want)
    if ($matches -and $matches.Count -gt $baseline) { Write-Output "PASS: flip ignoring=$want"; return }
    Start-Sleep -Milliseconds 300
  }
  throw "timeout waiting for flip ignoring=$want"
}

function Count-Flip([string]$want) {
  $m = Select-String -Path $log -Pattern ("ignoring=" + $want) -ErrorAction SilentlyContinue
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

# --- 2. locate the overlay by title ------------------------------------------
$hwnd = [Win32Input]::FindWindow($null, "Overlay (transparent hit-test)")
if ($hwnd -eq [IntPtr]::Zero) { throw "overlay window not found by title" }
$rect = New-Object Win32Input+RECT
[Win32Input]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
$ox = $rect.Left; $oy = $rect.Top
Write-Output "overlay window at $ox,$oy"

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
