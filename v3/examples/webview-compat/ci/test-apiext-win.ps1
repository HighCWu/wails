# API extensions scenario (electron backend, windows): the accelerator
# feed (before-input-event -> Go bindings), the menubar item accelerator
# and the intercepted Alt+F4 close chain, driven with real key events.
# The rendered-menubar pointer-click assert stays linux-only — the
# windows menubar sits in the titlebar where synthetic clicks are
# brittle; the item accelerator exercises the same serialization path.
param([string]$AppPath = "webview-compat.exe")

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;
public class Win32Input {
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lparam);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lparam);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder text, int count);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
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
  public static void KeyDown(byte vk) { keybd_event(vk, 0, 0, UIntPtr.Zero); }
  public static void KeyUp(byte vk)   { keybd_event(vk, 0, 0x0002, UIntPtr.Zero); }
}
"@

$script:LogPath = [System.IO.Path]::GetTempFileName()
$script:Hwnd = [IntPtr]::Zero

# press a modifier chord with retries: on a loaded runner the foreground
# switch can silently fail and the keys land nowhere
function Send-Chord([byte[]]$vks, [string]$marker, [int]$timeoutSec = 10) {
  for ($r = 1; $r -le 3; $r++) {
    [Win32Input]::SetForegroundWindow($script:Hwnd) | Out-Null
    Start-Sleep -Milliseconds 400
    foreach ($vk in $vks) {
      [Win32Input]::KeyDown($vk); Start-Sleep -Milliseconds 60
    }
    for ($j = $vks.Length - 1; $j -ge 0; $j--) {
      [Win32Input]::KeyUp($vks[$j]); Start-Sleep -Milliseconds 60
    }
    $start = Get-Date
    while (((Get-Date) - $start).TotalSeconds -lt $timeoutSec) {
      $hit = Select-String -Path $script:LogPath -Pattern $marker -SimpleMatch
      if ($hit) { Write-Output "PASS: log marker '$marker' (attempt $r)"; return }
      Start-Sleep -Milliseconds 250
    }
    Write-Output "retry $r: '$marker' not seen"
  }
  throw "timeout waiting for log marker: $marker"
}
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

$env:WAILS_WEBVIEW_BACKEND = "electron"
$env:WAILS_ELECTRON_DISABLE_SANDBOX = "1"
$env:WAILS_ELECTRON_EXPERIMENT = "native-ipc,native-http"
if ($env:WAILS_ELECTRON_NATIVE_ADDON -eq "") { $env:WAILS_ELECTRON_NATIVE_ADDON = "go_bridge_cpp.node" }
$env:WAILS_COMPAT_APIEXT = "1"

# batch-file launch: pwsh's Start-Process mangles embedded quotes
$runner = Join-Path $env:TEMP "run-compat-apiext.cmd"
Set-Content -Path $runner -Value "@echo off`r`n`"$AppPath`" > `"$script:LogPath`" 2>&1"
$proc = Start-Process -FilePath $runner -PassThru -WindowStyle Hidden
try {
  Wait-Marker "compat: APIEXT-ARMED" 150

  $hwnd = [IntPtr]::Zero
  for ($try = 0; $try -lt 10 -and $hwnd -eq [IntPtr]::Zero; $try++) {
    foreach ($ep in (Get-Process electron -ErrorAction SilentlyContinue)) {
      $cands = [Win32Input]::WindowsOfPid([uint32]$ep.Id)
      if ($cands.Count -gt 0) { $hwnd = $cands[0]; break }
    }
    if ($hwnd -eq [IntPtr]::Zero) { Start-Sleep -Seconds 2 }
  }
  if ($hwnd -eq [IntPtr]::Zero) { throw "could not find the electron window" }
  $script:Hwnd = $hwnd
  [Win32Input]::SetForegroundWindow($hwnd) | Out-Null
  Start-Sleep -Seconds 1

  # Ctrl+Shift+K -> window key binding through before-input-event
  Send-Chord ([byte[]]@(0x11, 0x10, 0x4B)) "compat: ACCEL-FIRED"

  # Ctrl+Shift+M -> menubar item accelerator (menu-click pipeline)
  Start-Sleep -Milliseconds 500
  Send-Chord ([byte[]]@(0x11, 0x10, 0x4D)) "compat: MENUBAR-CLICKED"

  # JS window.close() on the secondary window rides the same chain
  Wait-Marker "compat: WIN2-CLOSING" 30
  Wait-Marker "compat: WIN2-GONE" 30
  Write-Output "PASS: JS window.close chain"

  # Alt+F4 -> intercepted close: WindowClosing fires, then the app exits
  # through the closing chain (destroy via the default listener)
  Start-Sleep -Milliseconds 500
  Send-Chord ([byte[]]@(0x12, 0x73)) "compat: CLOSING-EVENT" 15

  if (-not $proc.WaitForExit(20000)) {
    Write-Output "remaining processes:"
    Get-Process | Where-Object { $_.ProcessName -match "electron|webview-compat" } |
      ForEach-Object { Write-Output ("  {0} pid={1}" -f $_.ProcessName, $_.Id) }
    Write-Output "app log tail:"
    Dump-Log | ForEach-Object { Write-Output "  $_" }
    throw "app still running 20s after the Alt+F4 close"
  }
  Write-Output "PASS: app exited through the closing chain"
  Write-Output "ALL PASS (windows apiext)"
} finally {
  if (-not $proc.HasExited) {
    cmd /c "taskkill /PID $($proc.Id) /T /F" 2>&1 | Out-Null
  }
}
