#!/usr/bin/env bash
# Transparent-window click-through E2E for examples/clickthrough (Linux/X11).
# Drives the pointer with xdotool on a private Xvfb and asserts on the app's
# stdout markers. Scene constants must stay in sync with assets/index.html:
#   circle centre (200,160), transparent interior (460,320), card button (118,537)
# Usage: test-linux.sh <path-to-clickthrough-binary>
set -uo pipefail

APP="${1:?usage: test-linux.sh <app-path>}"
DISP="${CLICKTHROUGH_DISPLAY:-:97}"
LOG="$(mktemp /tmp/clickthrough-log.XXXXXX)"
ARTDIR="${CLICKTHROUGH_ARTIFACTS:-/tmp}"
FAIL=0

cleanup() {
  [[ -n "${APP_PID:-}" ]] && kill "$APP_PID" 2>/dev/null
  [[ -n "${WM_PID:-}" ]] && kill "$WM_PID" 2>/dev/null
  [[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null
}
trap cleanup EXIT

die() { echo "FAIL: $*"; tail -30 "$LOG"; FAIL=1; exit 1; }

app_died() {
  if ! kill -0 "$APP_PID" 2>/dev/null; then die "app process exited early"; fi
}

# wait_log <pattern> [timeout-seconds]
wait_log() {
  local pattern="$1" timeout="${2:-30}" start
  start=$(date +%s)
  until grep -q "$pattern" "$LOG"; do
    app_died
    if [ $(( $(date +%s) - start )) -gt "$timeout" ]; then
      die "timeout waiting for log marker: $pattern"
    fi
    sleep 0.2
  done
  echo "PASS: log marker '$pattern'"
}

count_flip() { grep -c "ignoring=$1" "$LOG" || true; }

# wait_flip <true|false> <baseline-count> — waits for one more flip to value
wait_flip() {
  local want="$1" base="$2" start
  start=$(date +%s)
  until [ "$(count_flip "$want")" -gt "$base" ]; do
    app_died
    if [ $(( $(date +%s) - start )) -gt 20 ]; then
      die "timeout waiting for flip ignoring=$want"
    fi
    sleep 0.2
  done
  echo "PASS: flip ignoring=$want"
}

move() { xdotool mousemove "$1" "$2"; sleep 0.6; }

# --- start private display and app -----------------------------------------
Xvfb "$DISP" -screen 0 1280x800x24 >/dev/null 2>&1 & XVFB_PID=$!
sleep 1
DISPLAY="$DISP" openbox >/dev/null 2>&1 & WM_PID=$!
sleep 1
DISPLAY="$DISP" "$APP" >"$LOG" 2>&1 & APP_PID=$!

# --- 1. frontend uploads the alpha mask ------------------------------------
wait_log "mask uploaded"

# --- 2. locate the overlay (frameless: match by size 480x640) --------------
OL=$(DISPLAY="$DISP" xwininfo -root -tree | grep -F '480x640' | head -1 | awk '{print $1}')
[ -n "$OL" ] || die "overlay window (480x640) not found in window tree"
OX=$(DISPLAY="$DISP" xwininfo -id "$OL" | awk '/Absolute upper-left X/{print $4}')
OY=$(DISPLAY="$DISP" xwininfo -id "$OL" | awk '/Absolute upper-left Y/{print $4}')
echo "overlay window $OL at $OX,$OY"
export DISPLAY="$DISP"

# --- 3. opaque region stays interactive -------------------------------------
B=$(count_flip false)
move $((OX+200)) $((OY+160))   # circle centre
wait_flip false "$B"

# --- 4. transparent region passes through -----------------------------------
B=$(count_flip true)
move $((OX+460)) $((OY+320))
wait_flip true "$B"
LOC=$(xdotool getmouselocation)
WIN=$(echo "$LOC" | grep -o 'window:[0-9]*' | cut -d: -f2)
WIN_HEX=$(printf '0x%x' "${WIN:-0}")
if [ "$WIN_HEX" = "$OL" ]; then
  die "pointer still attributed to the overlay ($LOC)"
else
  echo "PASS: pointer attributed below the overlay ($LOC)"
fi
xdotool click 1
sleep 0.5
wait_log "underlay-clicks=1"

# --- 5. opaque card receives clicks after flip back --------------------------
B=$(count_flip false)
move $((OX+118)) $((OY+537))   # card button
wait_flip false "$B"
xdotool click 1
sleep 0.5
wait_log "card-clicks=1"

# --- artifact ----------------------------------------------------------------
import -window root "$ARTDIR/clickthrough-linux-final.png" 2>/dev/null \
  || echo "note: root screenshot unavailable"
echo "ALL PASS"
exit 0
