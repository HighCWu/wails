#!/usr/bin/env bash
# Frameless edge-resize scenario (electron backend, linux): ozone conducts
# the resize natively for frameless+resizable windows, and it works under
# Xvfb+openbox with plain xdotool corner drags (verified: 900x700 →
# 1050x800, exact +150/+100). NOTE the drag (move) gesture is different:
# the WM-conducted move does not complete under Xvfb, so frameless drag
# stays a real-desktop check (see test-drag.sh).
set -uo pipefail
APP="${1:?usage: test-resize.sh <app-path>}"
DISP="${CLICKTHROUGH_DISPLAY:-:97}"
LOG="$(mktemp /tmp/resize-log.XXXXXX)"

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
sleep 2

# the suite may leave a maximised 1280x800 shell behind; pick the
# normal-state app window instead of measuring the shell. The window is
# created AFTER the backend marker — retry the search until it appears.
WID=""
for _ in $(seq 1 60); do
  for w in $(xdotool search --onlyvisible --name "^webview-compat$" 2>/dev/null)            $(xdotool search --onlyvisible --class "electron" 2>/dev/null); do
    eval "$(xdotool getwindowgeometry --shell "$w")"
    if [ "${WIDTH:-0}" -lt 1280 ] && [ "${HEIGHT:-0}" -le 800 ]; then WID=$w; fi
  done
  [ -n "$WID" ] && break
  sleep 0.5
done
[[ -n "$WID" ]] || die "app window not found"
xdotool key --window "$WID" alt+F5 2>/dev/null  # unmaximise if needed
sleep 1
xdotool windowactivate --sync "$WID"
sleep 1

# xwininfo's absolute origin is authoritative: xdotool's
# getwindowgeometry can report a stale position after the WM places the
# frame, and a stale grab misses the edge entirely (dW=0)
win_geom() {
  xwininfo -id "$WID" -stats | awk '/Absolute upper-left X/{x=$NF} /Absolute upper-left Y/{y=$NF} /Width:/{w=$NF} /Height:/{h=$NF} END{print x+0, y+0, w+0, h+0}'
}

drag_resize() {  # $1=dx $2=dy $3=edge-x-offset-fn $4=edge-y-offset-fn name unused
  xdotool mousemove "$1" "$2"
  sleep 0.3
  xdotool mousedown 1
  sleep 0.4
  xdotool mousemove_relative -- "$3" "$4"
  sleep 0.8
  xdotool mouseup 1
  sleep 1
}

# each edge gets up to 3 attempts: the press can land outside the ozone
# hit border on a loaded runner and the WM simply ignores the gesture
DW=0; DH=0
for attempt in 1 2 3; do
  read -r GX GY W0 H0 <<< "$(win_geom)"
  echo "attempt $attempt: before ${W0}x${H0} at $GX,$GY"

  # right edge: grab within the border (mid height), drag right
  drag_resize "$((GX + W0 - 2))" "$((GY + H0 / 2))" 120 0
  read -r _ _ W1 _ <<< "$(win_geom)"
  echo "after right-edge drag: ${W1}x${H0}"
  DW=$((W1 - W0))

  # bottom edge: grab at the bottom border midpoint, drag down
  drag_resize "$((GX + W1 / 2))" "$((GY + H0 - 2))" 0 90
  read -r _ _ W2 H2 <<< "$(win_geom)"
  echo "after bottom-edge drag: ${W2}x${H2}"
  DH=$((H2 - H0))

  if [ "$DW" -ge 60 ] && [ "$DH" -ge 50 ]; then
    break
  fi
done

if [ "$DW" -lt 60 ] || [ "$DH" -lt 50 ]; then
  die "edge resize did not take (dW=$DW dH=$DH)"
fi
echo "PASS: frameless edge resize via ozone native frameless (dW=$DW dH=$DH)"
