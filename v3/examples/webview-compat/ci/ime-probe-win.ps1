# IME feasibility probe for the electron backend on Windows CI:
# activates the preinstalled Chinese (Simplified) MS Pinyin layout,
# focuses the compat page's IME input, types "ni" + Space, and checks
# the app's ImeReport markers (compositionstart / compositionend /
# committed value). This is an EXPERIMENT: runners may lack the IME
# binaries or refuse the layout activation — findings decide whether
# IME joins the CI matrix or stays real-machine-only.
# Usage: ime-probe-win.ps1 -AppPath <path-to-webview-compat.exe>
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
  [DllImport("user32.dll")] public static extern IntPtr LoadKeyboardLayout(string klid, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr GetKeyboardLayout(uint idThread);
  [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr hWnd, uint msg, UIntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lparam);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder text, int count);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
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
  public static void LeftClick(int x, int y) {
    SetCursorPos(x, y);
    System.Threading.Thread.Sleep(60);
    mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(60);
    mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);
  }
  public static void Key(byte vk) {
    keybd_event(vk, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(40);
    keybd_event(vk, 0, 0x0002, UIntPtr.Zero);
    System.Threading.Thread.Sleep(60);
  }
}
"@

$script:LogPath = [System.IO.Path]::GetTempFileName()
function Dump-Log { Get-Content $script:LogPath | Select-Object -Last 40 }
function Wait-Marker($pattern, $timeoutSec = 60) {
  $start = Get-Date
  while (((Get-Date) - $start).TotalSeconds -lt $timeoutSec) {
    if (Test-Path $script:LogPath) {
      $hit = Select-String -Path $script:LogPath -Pattern $pattern -SimpleMatch
      if ($hit) { Write-Output "PASS: log marker '$pattern'"; return $true }
    }
    Start-Sleep -Milliseconds 300
  }
  Write-Output "MISS: log marker '$pattern'"
  return $false
}

# ---- activate the preinstalled Chinese (Simplified) MS Pinyin layout
# KLID 00000804 = Chinese (Simplified, PRC) default → MS Pinyin on
# modern Windows via TSF.
$hkl = [Win32Input]::LoadKeyboardLayout("00000804", 1)  # KLF_ACTIVATE
Write-Output ("LoadKeyboardLayout(00000804) -> {0}" -f $hkl)
if ($hkl -eq [IntPtr]::Zero) { Write-Output "IME-PROBE-RESULT: layout activation failed"; exit 0 }

# ---- launch the app
$env:WAILS_WEBVIEW_BACKEND = "electron"
$env:WAILS_ELECTRON_DISABLE_SANDBOX = "1"
$env:WAILS_ELECTRON_EXPERIMENT = "native-ipc,native-http"
if ($env:WAILS_ELECTRON_NATIVE_ADDON -eq "") { $env:WAILS_ELECTRON_NATIVE_ADDON = "go_bridge_cpp.node" }

$runner = Join-Path $env:TEMP "run-ime.cmd"
Set-Content -Path $runner -Value "@echo off`r`n`"$AppPath`" > `"$script:LogPath`" 2>&1"
$proc = Start-Process -FilePath $runner -PassThru -WindowStyle Hidden

try {
  if (!(Wait-Marker "compat: backend=electron" 150)) { Write-Output "IME-PROBE-RESULT: app boot failed"; return }
  # let the internal suite finish — its window ops steal focus and would
  # break both the click-focus and the composition session
  Wait-Marker "compat: SUITE PASS" 180
  Start-Sleep -Seconds 2

  # locate + normalize the window, then click the IME input
  $hwnd = [IntPtr]::Zero
  foreach ($ep in (Get-Process electron -ErrorAction SilentlyContinue)) {
    $cands = [Win32Input]::WindowsOfPid([uint32]$ep.Id)
    if ($cands.Count -gt 0) { $hwnd = $cands[0]; break }
  }
  if ($hwnd -eq [IntPtr]::Zero) { Write-Output "IME-PROBE-RESULT: window not found"; return }
  [Win32Input]::SetWindowPos($hwnd, [IntPtr]::Zero, 100, 100, 900, 700, 0) | Out-Null
  Start-Sleep -Milliseconds 500
  # the #ime input sits at ~(100, 208) window-relative
  [Win32Input]::LeftClick(100 + 100, 100 + 208)
  Start-Sleep -Milliseconds 400

  # ---- ask the electron window's thread to activate the Chinese layout
  # (LoadKeyboardLayout alone only affects the calling thread)
  [Win32Input]::PostMessage($hwnd, 0x0050, [UIntPtr]::Zero, $hkl) | Out-Null  # WM_INPUTLANGCHANGEREQUEST
  Start-Sleep -Milliseconds 800

  # ---- type "ni" through the real keyboard pipeline (the IME intercepts)
  [Win32Input]::Key(0x4E)  # N
  [Win32Input]::Key(0x49)  # I
  Start-Sleep -Milliseconds 1200
  $compStarted = Wait-Marker "compat: IME compositionstart" 5

  # ---- Space commits the top candidate
  [Win32Input]::Key(0x20)  # Space
  Start-Sleep -Milliseconds 1200
  $compEnded = Wait-Marker "compat: IME compositionend" 5

  $raw = Get-Content $script:LogPath -Raw
  $imeLines = @(Select-String -Path $script:LogPath -Pattern "compat: IME" -SimpleMatch)
  $committed = $false
  $evidence = ($imeLines | ForEach-Object { $_.Line }) -join " | "
  foreach ($line in $imeLines) {
    if ($line.Line -match "value (\S+)") {
      if ($Matches[1] -match "[\u4e00-\u9fff]") { $committed = $true }
    }
  }
  $tail = ""
  if ($raw) {
    $rawLen = $raw.Length
    $tail = $raw.Substring([Math]::Max(0, $rawLen - 500)) -replace "[\r\n]+", " / "
  } else {
    $rawLen = 0
  }
  Write-Output ("compositionstart={0} compositionend={1} committedCJK={2} matches={3} fileLen={4} tail={5}" -f $compStarted, $compEnded, $committed, $imeLines.Count, $rawLen, $tail)
  if ($compStarted -and $compEnded -and $committed) {
    Write-Output "IME-PROBE-RESULT: PASS — real IME composition works on the runner"
  } else {
    Write-Output "IME-PROBE-RESULT: INFEASIBLE — composition did not reach the page"
  }
}
finally {
  if (!$proc.HasExited) {
    cmd /c "taskkill /PID $($proc.Id) /T /F" 2>&1 | Out-Null
  }
  Get-Process electron -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
