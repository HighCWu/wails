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
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT point);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder text, int count);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr hWnd, uint flags);
  [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO info);
  [DllImport("user32.dll")] public static extern IntPtr LoadCursor(IntPtr hInstance, int lpCursorName);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public POINT screenPos; }
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
# -NoNewWindow (not -WindowStyle Hidden): -WindowStyle puts SW_HIDE into
# STARTUPINFO and the app's first-created window (the underlay) inherits it,
# leaving it invisible and click-dead for the whole run.
$proc = Start-Process -FilePath $AppPath -RedirectStandardOutput $log `
  -RedirectStandardError $errlog -NoNewWindow -PassThru

# --- switch the session to mouse semantics -----------------------------------
# Hosted Server 2025 VMs expose a touch digitizer, which makes Windows
# suppress the cursor (GetCursorInfo flags=CURSOR_SUPPRESSED). Disable the
# digitizer, force the shell out of tablet mode, and wake pointer routing
# with an absolute mouse move — then verify the cursor is actually shown.
Write-Output ("SM_DIGITIZER={0} SM_MAXIMUMTOUCHES={1}" -f `
  [Win32Input]::GetSystemMetrics(94), [Win32Input]::GetSystemMetrics(95))
Write-Output "== pointing/display hardware =="
Get-CimInstance Win32_PointingDevice -ErrorAction SilentlyContinue |
  ForEach-Object { Write-Output "pointing: $($_.Name) status=$($_.Status)" }
Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue |
  ForEach-Object { Write-Output "display: $($_.Name) status=$($_.Status)" }
Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
  Where-Object { $_.Class -eq 'HIDClass' -and $_.FriendlyName -match 'touch|digitizer' } |
  ForEach-Object {
    Write-Output "disabling touch device: $($_.FriendlyName) [$($_.InstanceId)]"
    Disable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
  }
New-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\ImmersiveShell' `
  -Name TabletMode -Value 0 -PropertyType DWord -Force -ErrorAction SilentlyContinue | Out-Null
# absolute move to the screen centre (65535-normalised) wakes pointer routing
[Win32Input]::mouse_event(0x8001, 32767, 32767, 0, [UIntPtr]::Zero)
Start-Sleep -Seconds 2
function Test-CursorSuppressed {
  $c = New-Object Win32Input+CURSORINFO
  $c.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type][Win32Input+CURSORINFO])
  [Win32Input]::GetCursorInfo([ref]$c) | Out-Null
  Write-Output ("cursor state: flags={0} hCursor={1}" -f $c.flags, $c.hCursor)
  return (($c.flags -band 2) -ne 0)  # CURSOR_SUPPRESSED
}
$script:cursorSuppressed = Test-CursorSuppressed

# --- 1. mask uploaded (first webview start can be slow) ----------------------
Wait-LogMarker "mask uploaded" 120

# --- 2. locate the overlay: top-level window of our process sized 480x640 ---
$script:overlayHwnd = [IntPtr]::Zero
$script:underlayRect = $null
$script:underlayHwnd = [IntPtr]::Zero
$script:appWindows = New-Object System.Collections.ArrayList
$cb = [Win32Input+EnumWindowsProc]{
  param($h, $l)
  $winPid = 0
  [Win32Input]::GetWindowThreadProcessId($h, [ref]$winPid) | Out-Null
  if ($winPid -eq $script:proc.Id) {
    $r = New-Object Win32Input+RECT
    [Win32Input]::GetWindowRect($h, [ref]$r) | Out-Null
    $sb = New-Object System.Text.StringBuilder 256
    [Win32Input]::GetWindowText($h, $sb, 256) | Out-Null
    $visible = [Win32Input]::IsWindowVisible($h)
    $script:appWindows.Add("hwnd=$h vis=$visible rect=$($r.Left),$($r.Top) $($r.Right - $r.Left)x$($r.Bottom - $r.Top) title='$($sb.ToString())'") | Out-Null
    if ($visible) {
      $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
      if ($w -eq 480 -and $ht -eq 640) { $script:overlayHwnd = $h }
      if ($sb.ToString() -like "Underlay*" -or ($w -ge 690 -and $w -le 740 -and $ht -ge 490 -and $ht -le 560)) {
        $script:underlayRect = $r
        $script:underlayHwnd = $h
      }
    }
  }
  return $true
}
# windows may still be settling right after page load — poll until both appear
$deadline = (Get-Date).AddSeconds(20)
while (((Get-Date) -lt $deadline)) {
  $script:appWindows.Clear()
  [Win32Input]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
  if ($script:overlayHwnd -ne [IntPtr]::Zero -and $script:underlayRect) { break }
  Start-Sleep -Milliseconds 500
}
if ($script:overlayHwnd -eq [IntPtr]::Zero) {
  throw "overlay window (480x640) not found for pid $($script:proc.Id); windows: $($script:appWindows -join ' | ')"
}
Write-Output "process windows: $($script:appWindows -join ' | ')"
if (-not $script:underlayRect) { throw "underlay window not found; windows: $($script:appWindows -join ' | ')" }

# The hosted-compute-agent window sits above normal windows and swallows
# synthesized clicks; raise both test windows into the topmost band with the
# overlay above the underlay so clicks route overlay -> underlay.
$HWND_TOPMOST = [IntPtr](-1); $SWP_NOSIZE = 0x1; $SWP_NOMOVE = 0x2; $SWP_NOACTIVATE = 0x10
[Win32Input]::SetWindowPos($script:underlayHwnd, $HWND_TOPMOST, 0, 0, 0, 0, ($SWP_NOSIZE -bor $SWP_NOMOVE -bor $SWP_NOACTIVATE)) | Out-Null
Start-Sleep -Milliseconds 200
[Win32Input]::SetWindowPos($script:overlayHwnd, $HWND_TOPMOST, 0, 0, 0, 0, ($SWP_NOSIZE -bor $SWP_NOMOVE -bor $SWP_NOACTIVATE)) | Out-Null
Start-Sleep -Milliseconds 200
$rect = New-Object Win32Input+RECT
[Win32Input]::GetWindowRect($script:overlayHwnd, [ref]$rect) | Out-Null
$ox = $rect.Left; $oy = $rect.Top
Write-Output "overlay window $script:overlayHwnd at $ox,$oy"
$u = $script:underlayRect
Write-Output "underlay window at $($u.Left),$($u.Top) ($($u.Right - $u.Left)x$($u.Bottom - $u.Top))"

# --- 3. home the cursor outside the window: engine must go passthrough -------
Move-Cursor 10 10
Wait-State "true"

# --- 4. opaque region goes interactive ----------------------------------------
Move-Cursor ($ox + 200) ($oy + 160)
Wait-State "false"

# --- 5. transparent region passes through to the underlay ---------------------
# pick a transparent scene point that is also inside the underlay's rect
$candidates = @(@(460, 320), @(240, 300), @(460, 240), @(460, 620))
$px = $null; $py = $null
foreach ($c in $candidates) {
  $gx = $ox + $c[0]; $gy = $oy + $c[1]
  if (-not $script:underlayRect) { $px = $gx; $py = $gy; break }
  $u = $script:underlayRect
  if ($gx -gt $u.Left -and $gx -lt $u.Right -and $gy -gt $u.Top -and $gy -lt $u.Bottom) {
    $px = $gx; $py = $gy; break
  }
}
if ($null -eq $px) { throw "no transparent point intersects the underlay rect" }
Write-Output "transparent click point: $px,$py"
Move-Cursor $px $py
$pt = New-Object Win32Input+POINT
$pt.X = $px; $pt.Y = $py
$wfp = [Win32Input]::WindowFromPoint($pt)
$sb = New-Object System.Text.StringBuilder 256
[Win32Input]::GetWindowText($wfp, $sb, 256) | Out-Null
Write-Output ("WindowFromPoint({0},{1}) = {2} '{3}'" -f $px, $py, $wfp, $sb.ToString())
Move-And-Click $px $py
Wait-State "true"

# The underlay is a full-page text input, so with the cursor unsuppressed
# the pointer over the transparent region shows the I-beam set by the
# window below. On a cursor-suppressed session it cannot be observed.
if ($script:cursorSuppressed) {
  Write-Output "NOTE: cursor still suppressed after mouse-mode switch; I-beam routing not observable on this runner"
} else {
  $ci = New-Object Win32Input+CURSORINFO
  $ci.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type][Win32Input+CURSORINFO])
  [Win32Input]::GetCursorInfo([ref]$ci) | Out-Null
  $ibeam = [Win32Input]::LoadCursor([IntPtr]::Zero, 32513)  # IDC_IBEAM
  if ($ci.hCursor -ne $ibeam) {
    throw "cursor over the transparent region is not the I-beam from the underlay (hCursor=$($ci.hCursor), expected $ibeam)"
  }
  Write-Output "PASS: cursor over transparent region is the underlay's I-beam"
}

try {
  Wait-LogMarker "underlay-clicks=1" 15
} catch {
  $swallowed = Select-String -Path $log, $errlog -Pattern "overlay-clicks" -Quiet -ErrorAction SilentlyContinue
  if ($swallowed) { throw "passthrough FAILED: the click was consumed by the overlay; log:`n$(Dump-Log)" }
  throw
}

# --- 6. opaque card receives clicks after flipping back -----------------------
# The passthrough click activated the underlay, moving it to the top of the
# topmost band (above the overlay). Re-assert the overlay on top — clicking
# a window activates it, so verify routing right before the click.
$HWND_TOPMOST = [IntPtr](-1); $SWP = 0x13  # NOSIZE|NOMOVE|NOACTIVATE
function Assert-OverlayTop([int]$x, [int]$y) {
  for ($i = 0; $i -lt 3; $i++) {
    [Win32Input]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 400
    $pt = New-Object Win32Input+POINT
    $pt.X = $x; $pt.Y = $y
    $root = [Win32Input]::GetAncestor([Win32Input]::WindowFromPoint($pt), 2)
    if ($root -eq $script:overlayHwnd) { Write-Output "PASS: overlay is top at $x,$y"; return }
    [Win32Input]::SetWindowPos($script:overlayHwnd, $HWND_TOPMOST, 0, 0, 0, 0, $SWP) | Out-Null
    Start-Sleep -Milliseconds 300
  }
  throw "overlay is not the top window at $x,$y after re-raise attempts"
}
Assert-OverlayTop ($ox + 118) ($oy + 537)
Move-And-Click ($ox + 118) ($oy + 537)
Wait-State "false"
Wait-LogMarker "card-clicks=1"

Cleanup
Write-Output "ALL PASS"
exit 0
