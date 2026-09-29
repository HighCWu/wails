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

# The engine's most recently logged state; no flip yet counts as false
# (pre-mask fail-open is interactive).
function Get-LastState {
  $m = Select-String -Path $log, $errlog -Pattern "ignoring=(true|false)" -ErrorAction SilentlyContinue |
    Select-Object -Last 1
  if (-not $m) { return "none" }
  if ($m.Line -match "ignoring=(true|false)") { return $Matches[1] }
  return "none"
}

function Wait-State([string]$want, [int]$timeoutSec = 25) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ($true) {
    $st = Get-LastState
    if ($st -eq $want -or ($st -eq "none" -and $want -eq "false")) {
      Write-Output "PASS: state ignoring=$want"
      return
    }
    if ($script:proc -and $script:proc.HasExited) { throw "app exited early" }
    if ((Get-Date) -gt $deadline) { throw "timeout waiting for state ignoring=$want (last: $st); log:`n$(Dump-Log)" }
    Start-Sleep -Milliseconds 300
  }
}

function Move-And-Click([int]$x, [int]$y) {
  [Win32Input]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 600
  [Win32Input]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)  # LEFTDOWN
  Start-Sleep -Milliseconds 60
  [Win32Input]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)  # LEFTUP
  Start-Sleep -Milliseconds 400
}

function Move-Cursor([int]$x, [int]$y) {
  [Win32Input]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 600
}

# --- launch -----------------------------------------------------------------
$proc = Start-Process -FilePath $AppPath -RedirectStandardOutput $log `
  -RedirectStandardError $errlog -WindowStyle Hidden -PassThru

# --- 1. mask uploaded (first webview start can be slow) ----------------------
Wait-LogMarker "mask uploaded" 120

# --- 2. locate the overlay: top-level window of our process sized 480x640 ---
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

# --- 3. home the cursor outside the window: engine must go passthrough -------
Move-Cursor 10 10
Wait-State "true"

# --- 4. opaque region goes interactive ----------------------------------------
Move-Cursor ($ox + 200) ($oy + 160)
Wait-State "false"

# --- 5. transparent region passes through to the underlay ---------------------
Move-And-Click ($ox + 460) ($oy + 320)
Wait-State "true"
Wait-LogMarker "underlay-clicks=1"

# --- 6. opaque card receives clicks after flipping back -----------------------
Move-And-Click ($ox + 118) ($oy + 537)
Wait-State "false"
Wait-LogMarker "card-clicks=1"

Cleanup
Write-Output "ALL PASS"
exit 0
