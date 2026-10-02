# Interactive E2E for the electron backend on Windows: real mouse/keyboard
# injection against the compat app — frameless drag, frameless edge resize,
# dialog keyboard flows, and context-menu selection. Same user32 injection
# pipeline as the clickthrough scenario (test-windows.ps1).
# Asserts on the app's stdout markers and GetWindowRect geometry.
# Usage: test-interactive-win.ps1 -AppPath <path-to-webview-compat.exe>
param(
  [Parameter(Mandatory = $true)][string]$AppPath
)

$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;
public class Win32Input {
  public struct RECT { public int Left, Top, Right, Bottom; }
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lparam);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lparam);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder text, int count);
  public static List<IntPtr> WindowsOfPid(uint pid) {
    var list = new List<IntPtr>();
    EnumWindows((h, l) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h)) {
        var sb = new StringBuilder(256); GetWindowText(h, sb, 256);
        if (sb.ToString().Length > 0) list.Add(h);
      }
      return true;
    }, IntPtr.Zero);
    return list;
  }
  public static void LeftDown() { mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero); }
  public static void LeftUp()   { mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero); }
  public static void Key(byte vk) {
    keybd_event(vk, 0, 0, [UIntPtr]::Zero);
    keybd_event(vk, 0, 0x0002, [UIntPtr]::Zero);  // KEYEVENTF_KEYUP
  }
}
"@

$script:LogPath = [System.IO.Path]::GetTempFileName()
function Dump-Log { Get-Content $script:LogPath | Select-Object -Last 30 }
function Wait-Marker($pattern, $timeoutSec = 45) {
  $start = Get-Date
  while (((Get-Date) - $start).TotalSeconds -lt $timeoutSec) {
    if (Test-Path $script:LogPath) {
      $hit = Select-String -Path $script:LogPath -Pattern $pattern -SimpleMatch
      if ($hit) { Write-Output "PASS: log marker '$pattern'"; return }
    }
    Start-Sleep -Milliseconds 300
  }
  throw "timeout waiting for log marker: $pattern; log:`n$(Dump-Log)"
}

# ---- launch the app with the interactive scenario flags
$env:WAILS_WEBVIEW_BACKEND = "electron"
$env:WAILS_ELECTRON_DISABLE_SANDBOX = "1"
$env:WAILS_ELECTRON_EXPERIMENT = "native-ipc,native-http"
if ($env:WAILS_ELECTRON_NATIVE_ADDON -eq "") { $env:WAILS_ELECTRON_NATIVE_ADDON = "go_bridge_cpp.node" }
$env:WAILS_COMPAT_FRAMELESS = "1"
$env:WAILS_COMPAT_DIALOGS = "1"
$env:WAILS_COMPAT_MENUS = "1"

$proc = Start-Process -FilePath $AppPath -PassThru -RedirectStandardOutput $script:LogPath -RedirectStandardError "$($script:LogPath).err"
try {
  Wait-Marker "compat: backend=electron" 60

  # locate the main visible window of the electron child (the Go process
  # spawns electron; the window belongs to the electron pid)
  Start-Sleep -Seconds 3
  $hwnd = [IntPtr]::Zero
  for ($try = 0; $try -lt 10 -and $hwnd -eq [IntPtr]::Zero; $try++) {
    foreach ($ep in (Get-Process electron -ErrorAction SilentlyContinue)) {
      $cands = [Win32Input]::WindowsOfPid([uint32]$ep.Id)
      if ($cands.Count -gt 0) { $hwnd = $cands[0]; break }
    }
    if ($hwnd -eq [IntPtr]::Zero) { Start-Sleep -Seconds 2 }
  }
  if ($hwnd -eq [IntPtr]::Zero) { throw "could not find the electron window" }
  $rect = New-Object Win32Input+RECT
  [Win32Input]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
  Write-Output ("window rect: {0},{1} {2}x{3}" -f $rect.Left, $rect.Top, ($rect.Right-$rect.Left), ($rect.Bottom-$rect.Top))

  # ---- wait for the internal suite to finish (its window ops would
  # dismiss overlays and fight the drag gestures)
  Wait-Marker "compat: SUITE PASS" 120

  function Move-Cursor([int]$x, [int]$y) {
    [Win32Input]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 40
  }

  # ---- frameless drag: press on the drag strip (top 24px), move, release
  $rect = New-Object Win32Input+RECT
  [Win32Input]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
  $grabX = $rect.Left + 150; $grabY = $rect.Top + 12
  Move-Cursor $grabX $grabY
  [Win32Input]::LeftDown()
  Start-Sleep -Milliseconds 120
  for ($i = 1; $i -le 10; $i++) {
    Move-Cursor ($grabX + 15 * $i) ($grabY + 8 * $i)
  }
  Start-Sleep -Milliseconds 120
  [Win32Input]::LeftUp()
  Start-Sleep -Milliseconds 500
  $rect2 = New-Object Win32Input+RECT
  [Win32Input]::GetWindowRect($hwnd, [ref]$rect2) | Out-Null
  $dx = $rect2.Left - $rect.Left; $dy = $rect2.Top - $rect.Top
  Write-Output ("drag delta: {0},{1}" -f $dx, $dy)
  if ([Math]::Abs($dx) -lt 80 -or [Math]::Abs($dy) -lt 40) { throw "frameless drag did not move the window (delta $dx,$dy)" }
  Write-Output "PASS: frameless drag"

  # ---- frameless edge resize: drag the bottom-right corner outward
  [Win32Input]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
  $grabX = $rect.Right - 4; $grabY = $rect.Bottom - 4
  Move-Cursor $grabX $grabY
  [Win32Input]::LeftDown()
  Start-Sleep -Milliseconds 120
  for ($i = 1; $i -le 8; $i++) {
    Move-Cursor ($grabX + 12 * $i) ($grabY + 9 * $i)
  }
  Start-Sleep -Milliseconds 120
  [Win32Input]::LeftUp()
  Start-Sleep -Milliseconds 500
  [Win32Input]::GetWindowRect($hwnd, [ref]$rect2) | Out-Null
  $dw = ($rect2.Right - $rect2.Left) - ($rect.Right - $rect.Left)
  $dh = ($rect2.Bottom - $rect2.Top) - ($rect.Bottom - $rect.Top)
  Write-Output ("resize delta: {0},{1}" -f $dw, $dh)
  if ($dw -lt 60 -or $dh -lt 40) { throw "frameless edge resize did not grow the window (delta $dw,$dh)" }
  Write-Output "PASS: frameless edge resize"

  # ---- dialogs: Escape cancels the chooser, Return confirms the box
  [System.IO.File]::WriteAllText((Join-Path $env:TEMP "compat-dialogs-go"), "go") | Out-Null
  Wait-Marker "compat: DIALOG-OPENED" 30
  Start-Sleep -Milliseconds 800
  [Win32Input]::Key(0x1B)  # Escape
  Wait-Marker "compat: DIALOG-RESULT canceled" 30
  Wait-Marker "compat: MSG-OPENED" 30
  Start-Sleep -Milliseconds 800
  [Win32Input]::Key(0x0D)  # Return
  Wait-Marker "compat: DIALOGS-DONE" 30
  Write-Output "PASS: dialogs"

  # ---- context menu: Down selects the first item, Return clicks it
  Wait-Marker "compat: MENU-OPENED" 30
  Start-Sleep -Milliseconds 800
  [Win32Input]::Key(0x28)  # Down
  Start-Sleep -Milliseconds 300
  [Win32Input]::Key(0x0D)  # Return
  Wait-Marker "compat: MENU-CLICKED" 30
  Write-Output "PASS: context menu"

  Write-Output "PASS: electron interactive scenario"
}
finally {
  if (!$proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
  Get-Process electron -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
