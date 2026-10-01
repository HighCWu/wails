#!/usr/bin/env bash
# Frameless drag/resize scenario (electron backend): a frameless window with
# a `--wails-draggable: drag` strip; real mouse events through xdotool must
# reach the runtime, ride "wails:drag" into the host, and conduct an X11
# _NET_WM_MOVERESIZE so the window manager moves the window.
set -uo pipefail
APP="${1:?usage: test-drag.sh <app-path>}"
DISP="${CLICKTHROUGH_DISPLAY:-:98}"
LOG="$(mktemp /tmp/drag-log.XXXXXX)"

APP_PID=""; WM_PID=""; XVFB_PID=""
cleanup() {
  for p in "$APP_PID" "$WM_PID" "$XVFB_PID"; do
    [[ -n "$p" ]] && kill "$p" 2>/dev/null
  done
}
trap cleanup EXIT
die() { echo "FAIL: $*"; tail -20 "$LOG"; exit 1; }

Xvfb "$DISP" -screen 0 1280x800x24 >/dev/null 2>&1 & XVFB_PID=$!
sleep 1
export DISPLAY="$DISP"
openbox >/dev/null 2>&1 & WM_PID=$!
sleep 1

env WAILS_WEBVIEW_BACKEND=electron \
    WAILS_COMPAT_FRAMELESS=1 \
    ${WAILS_ELECTRON_DIR:+WAILS_ELECTRON_DIR=$WAILS_ELECTRON_DIR} \
    ${WAILS_ELECTRON_DISABLE_SANDBOX:+WAILS_ELECTRON_DISABLE_SANDBOX=$WAILS_ELECTRON_DISABLE_SANDBOX} \
    ${WAILS_ELECTRON_EXPERIMENT:+WAILS_ELECTRON_EXPERIMENT=$WAILS_ELECTRON_EXPERIMENT} \
    ${WAILS_ELECTRON_NATIVE_ADDON:+WAILS_ELECTRON_NATIVE_ADDON=$WAILS_ELECTRON_NATIVE_ADDON} \
    "$APP" >"$LOG" 2>&1 & APP_PID=$!

for _ in $(seq 1 200); do
  grep -q "compat: backend=" "$LOG" && break
  kill -0 "$APP_PID" 2>/dev/null || die "app exited before ready"
  sleep 0.3
done
grep -q "compat: backend=electron" "$LOG" || die "backend is not electron"
sleep 2

WID=$(xdotool search --onlyvisible --name "^webview-compat$" | head -1)
[[ -n "$WID" ]] || die "app window not found"
xdotool windowactivate --sync "$WID"
sleep 1

eval "$(xdotool getwindowgeometry --shell "$WID")"
X0=$X; Y0=$Y
# the drag strip sits ~40px from the window's top-left (24px h2 above it)
xdotool mousemove "$((X0 + 200))" "$((Y0 + 48))" click 1   # warm the runtime
sleep 0.5
xdotool mousemove "$((X0 + 200))" "$((Y0 + 48))" mousedown 1
sleep 0.4
xdotool mousemove_relative -- 130 60
sleep 0.4
xdotool mouseup 1
sleep 1.2

eval "$(xdotool getwindowgeometry --shell "$WID")"
DX=$((X - X0)); DY=$((Y - Y0))
echo "drag delta: ${DX},${DY}"
if [ "${DX#-}" -lt 60 ] || [ "${DY#-}" -lt 25 ]; then
  die "window did not follow the drag (delta ${DX},${DY})"
fi
echo "PASS: frameless drag strip moved the window (${DX},${DY})"
