#!/usr/bin/env bash
# Interactive dialog + context-menu scenario for the electron backend:
# real dialogs/popups are driven with xdotool keystrokes and the app's
# own log markers assert the round trips.
#   test-dialogs.sh <app-path>
# Requires an X server (the script starts its own Xvfb + openbox).
set -uo pipefail
APP="${1:?usage: test-dialogs.sh <app-path>}"
DISP="${CLICKTHROUGH_DISPLAY:-:97}"
LOG="$(mktemp /tmp/compat-dialogs.XXXXXX)"

cleanup() {
  [[ -n "${APP_PID:-}" ]] && kill "$APP_PID" 2>/dev/null
  [[ -n "${WM_PID:-}" ]] && kill "$WM_PID" 2>/dev/null
  [[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null
}
trap cleanup EXIT

die() { echo "FAIL: $*"; echo "--- app log ($LOG):"; cat "$LOG"; exit 1; }

Xvfb "$DISP" -screen 0 1280x800x24 >/dev/null 2>&1 & XVFB_PID=$!
sleep 0.5
DISPLAY="$DISP" openbox >/dev/null 2>&1 & WM_PID=$!
sleep 0.5

export DISPLAY="$DISP"
export WAILS_WEBVIEW_BACKEND=electron
export WAILS_ELECTRON_DIR="${WAILS_ELECTRON_DIR:-/opt/electron}"
export WAILS_ELECTRON_DISABLE_SANDBOX=1
export WAILS_ELECTRON_EXPERIMENT=native-ipc,native-http
export WAILS_ELECTRON_NATIVE_ADDON="${WAILS_ELECTRON_NATIVE_ADDON:-/tmp/go_bridge_cpp.node}"
export WAILS_COMPAT_DIALOGS=1
export WAILS_COMPAT_MENUS=1

"$APP" >"$LOG" 2>&1 & APP_PID=$!

app_alive() { kill -0 "$APP_PID" 2>/dev/null; }
wait_log() {
  local pattern="$1" timeout="${2:-30}" start
  start=$(date +%s)
  until grep -q "$pattern" "$LOG"; do
    app_alive || die "app exited while waiting for '$pattern'"
    if [ $(( $(date +%s) - start )) -gt "$timeout" ]; then
      die "timeout waiting for '$pattern'"
    fi
    sleep 0.3
  done
  echo "PASS: '$pattern'"
}

# let the app + electron boot, and let the full suite finish first —
# its window ops (maximise/fullscreen storms) would dismiss the
# dialogs and popups if they overlapped
wait_log "compat: SUITE PASS" 120
touch /tmp/compat-dialogs-go

# ---- file dialog: open, Escape cancels, app reports canceled
wait_log "compat: DIALOG-OPENED"

# ---- file dialog: open, Escape cancels, app reports canceled
wait_log "compat: DIALOG-OPENED"
sleep 1 # let the chooser window take focus
xdotool key --clearmodifiers Escape
wait_log "compat: DIALOG-RESULT canceled" 20

# ---- message dialog: open, Return confirms the default button
wait_log "compat: MSG-OPENED"
sleep 1
xdotool key --clearmodifiers Return
wait_log "compat: DIALOGS-DONE" 20
touch /tmp/compat-dialogs-done

# ---- context menu: open, Down selects the first item, Return clicks it
wait_log "compat: MENU-OPENED"
sleep 1
if [ "${DEBUG_SHOT:-}" = "1" ]; then
  import -window root /tmp/dbg-menu-before.png
  xdotool getactivewindow getwindowname >> "$LOG" 2>&1
fi
xdotool key --clearmodifiers Down
if [ "${DEBUG_SHOT:-}" = "1" ]; then
  sleep 0.4
  import -window root /tmp/dbg-menu-after.png
fi
sleep 0.4
xdotool key --clearmodifiers Return
wait_log "compat: MENU-CLICKED" 20

# the app must still be healthy after all the interactive overlays
sleep 1
app_alive || die "app died after the dialog/menu scenario"

echo "PASS: electron dialogs + context menu scenario"
