#!/usr/bin/env bash
# Window API compatibility + RPC latency scenario for examples/webview-compat.
#   test-compat.sh <app-path> <system|electron>
# The app self-asserts the Window API (compat: <name> PASS/FAIL markers on
# stdout) and the frontend reports RPC round-trip latency (compat: RPC).
# The driver waits for the suite verdict and fails on any FAIL marker.
set -uo pipefail

APP="${1:?usage: test-compat.sh <app-path> <backend>}"
BACKEND="${2:?backend required: system|electron}"
DISP="${CLICKTHROUGH_DISPLAY:-:98}"
LOG="$(mktemp /tmp/compat-log.XXXXXX)"

cleanup() {
  [[ -n "${APP_PID:-}" ]] && kill "$APP_PID" 2>/dev/null
  [[ -n "${WM_PID:-}" ]] && kill "$WM_PID" 2>/dev/null
  [[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null
}
trap cleanup EXIT

die() { echo "FAIL: $*"; tail -40 "$LOG"; exit 1; }

app_died() {
  if ! kill -0 "$APP_PID" 2>/dev/null; then die "app process exited early"; fi
}

wait_log() {
  local pattern="$1" timeout="${2:-90}" start
  start=$(date +%s)
  until grep -q "$pattern" "$LOG"; do
    app_died
    if [ $(( $(date +%s) - start )) -gt "$timeout" ]; then
      die "timeout waiting for log marker: $pattern"
    fi
    sleep 0.3
  done
  echo "PASS: log marker '$pattern'"
}

Xvfb "$DISP" -screen 0 1280x800x24 >/dev/null 2>&1 & XVFB_PID=$!
sleep 1
DISPLAY="$DISP" openbox >/dev/null 2>&1 & WM_PID=$!
sleep 1

env WAILS_WEBVIEW_BACKEND="$BACKEND" \
    ${WAILS_ELECTRON_DIR:+WAILS_ELECTRON_DIR=$WAILS_ELECTRON_DIR} \
    ${WAILS_ELECTRON_DISABLE_SANDBOX:+WAILS_ELECTRON_DISABLE_SANDBOX=$WAILS_ELECTRON_DISABLE_SANDBOX} \
    ${WAILS_ELECTRON_EXPERIMENT:+WAILS_ELECTRON_EXPERIMENT=$WAILS_ELECTRON_EXPERIMENT} \
    ${WAILS_ELECTRON_NATIVE_ADDON:+WAILS_ELECTRON_NATIVE_ADDON=$WAILS_ELECTRON_NATIVE_ADDON} \
    DISPLAY="$DISP" "$APP" >"$LOG" 2>&1 & APP_PID=$!

wait_log "compat: backend=" 120
BACKEND_REPORTED=$(grep -o 'backend=[a-z]*' "$LOG" | tail -1 | cut -d= -f2)
if [ "$BACKEND_REPORTED" != "$BACKEND" ]; then
  die "backend mismatch: requested $BACKEND, app reports $BACKEND_REPORTED"
fi
echo "PASS: backend is $BACKEND_REPORTED"

# the frontend transport matrix benchmark must complete after the suite;
# every row of the latency table and every error is asserted from the log
wait_log "compat: SUITE" 120
wait_log "compat: BENCH-FINISHED" 180
BENCH_ERRORS=$(grep -c "compat: BENCH-ERROR" "$LOG" || true)
if [ "$BENCH_ERRORS" -ne 0 ]; then
  grep "compat: BENCH-ERROR" "$LOG"
  die "$BENCH_ERRORS bench error(s)"
fi
# native-ipc mode: the renderer addon must have benched every payload size
# over the UDS transport (5 sizes), proving connect + handshake + echo.
# A reload (e.g. the crash-recovery leg) reruns the bench, so >= 5.
case ",${WAILS_ELECTRON_EXPERIMENT:-}," in
  *,native-ipc,*)
    NATIVE_ROWS=$(grep -c "compat: BENCH path=native-uds" "$LOG" || true)
    if [ "$NATIVE_ROWS" -lt 5 ]; then
      grep "native" "$LOG" | head -20
      die "expected >= 5 native-uds bench rows, got $NATIVE_ROWS"
    fi
    echo "PASS: native-uds bench rows complete ($NATIVE_ROWS/5)"
    ;;
esac
# churn leg (WAILS_COMPAT_CHURN=1): five create/close cycles (even
# windows closed settled, odd ones closed pending-creation) must all end
# up removed from the window manager.
if [ "${WAILS_COMPAT_CHURN:-}" = "1" ]; then
  wait_log "compat: CHURN PASS" 60
  echo "PASS: window churn cycles complete"
fi
# crash-recovery leg (WAILS_COMPAT_CRASH=1, electron backend): once the
# bench is done, SIGKILL the renderer and require the recovery event.
# The backend must report electron:rendererCrashed and reload once.
if [ "${WAILS_COMPAT_CRASH:-}" = "1" ]; then
  if [ "$BACKEND" != "electron" ]; then
    die "WAILS_COMPAT_CRASH is electron-only"
  fi
  killed_any=""
  for attempt in 1 2 3 4 5; do
    for pid in $(pgrep -x electron); do
      if grep -qa "type=renderer" "/proc/$pid/cmdline" 2>/dev/null; then
        kill -9 "$pid" && killed_any=1
      fi
    done
    [ -n "$killed_any" ] && break
    sleep 1
  done
  [ -n "$killed_any" ] || die "no electron renderer process found to kill"
  wait_log "compat: CRASH-EVENT" 20
  echo "PASS: crash recovery event fired"
fi
# reload leg (WAILS_COMPAT_RELOAD=1): after the suite passes, the app
# reloads the page; the renderer re-injects and re-dials the bridge
# endpoint, and the re-run bench over the new connection must complete.
if [ "${WAILS_COMPAT_RELOAD:-}" = "1" ]; then
  wait_log "compat: RELOAD-OK" 90
  RELOAD_BENCHES=$(grep -c "compat: BENCH-FINISHED" "$LOG" || true)
  if [ "$RELOAD_BENCHES" -lt 2 ]; then
    die "expected >= 2 bench completions (pre + post reload), got $RELOAD_BENCHES"
  fi
  echo "PASS: same-process reload re-connected ($RELOAD_BENCHES bench runs)"
fi
# apiext leg (WAILS_COMPAT_APIEXT=1, electron backend): accelerator feed,
# menubar item accelerators, and the intercepted close → WindowClosing chain.
# Ends with Alt+F4, which must log CLOSING-EVENT and exit the app.
if [ "${WAILS_COMPAT_APIEXT:-}" = "1" ]; then
  wait_log "compat: APIEXT-ARMED" 30
  export DISPLAY="$DISP"
  WID=""
  for _ in $(seq 1 60); do
    WID=$(xdotool search --onlyvisible --name "^webview-compat$" | head -1)
    [ -n "$WID" ] && break
    sleep 0.5
  done
  [ -n "$WID" ] || die "app window not found"
  xdotool windowactivate --sync "$WID"
  sleep 1
  xdotool key ctrl+shift+k
  wait_log "compat: ACCEL-FIRED" 10
  echo "PASS: accelerator fired through before-input-event"
  sleep 1 # let the key release settle before the pointer click
  # real mouse click on the rendered menubar item (top-left, ~25px strip):
  # exercises the full menu-click pipeline (JS click -> stdout -> Go).
  # xdotool's getwindowgeometry can report a stale origin after the
  # suite's fullscreen/maximise churn — xwininfo's absolute position is
  # authoritative, and a low click would hit the page's drag strip.
  ABS=$(xwininfo -id "$WID" -stats | grep -E "^ +Absolute upper-left" | awk '{print $NF}')
  AX=$(echo "$ABS" | head -1)
  AY=$(echo "$ABS" | tail -1)
  xdotool mousemove "$((AX + 60))" "$((AY + 12))" sleep 0.5 click 1
  wait_log "compat: MENUBAR-CLICKED" 10
  echo "PASS: menubar item clicked (menu-click pipeline)"
  xdotool key ctrl+shift+m
  wait_log "compat: MENUBAR-CLICKED" 10
  CLICKS=$(grep -c "compat: MENUBAR-CLICKED" "$LOG" || true)
  if [ "$CLICKS" -lt 2 ]; then
    die "expected 2 menubar clicks (mouse + accelerator), got $CLICKS"
  fi
  echo "PASS: menubar item accelerator fired"
  # global shortcut: the OS-level grab catches the key without focus
  xdotool key ctrl+shift+g
  wait_log "compat: GLOBAL-SHORTCUT-FIRED" 10
  echo "PASS: global shortcut fired (X11 grab)"
  # programmatic checkbox change must re-push the menubar: the initial
  # setMenu emits one menu-set event, the SetChecked resync a second
  wait_log "compat: CHECK-SET" 15
  SETS=$(grep -c "menu-set id=" "$LOG" || true)
  if [ "$SETS" -lt 2 ]; then
    die "expected >= 2 menu-set events (initial + resync), got $SETS"
  fi
  echo "PASS: programmatic menu state resync ($SETS pushes)"
  xdotool key alt+F4
  wait_log "compat: CLOSING-EVENT" 10
  echo "PASS: user close emits WindowClosing"
  # the window must actually close now (destroy via the closing chain)
  for _ in $(seq 1 40); do
    kill -0 "$APP_PID" 2>/dev/null || break
    sleep 0.5
  done
  if kill -0 "$APP_PID" 2>/dev/null && xdotool search --onlyvisible --name "^webview-compat$" >/dev/null 2>&1; then
    die "window still visible after Alt+F4 close"
  fi
  echo "PASS: app exited through the closing chain"
fi
# tray leg (WAILS_COMPAT_TRAY=1, electron backend): the Tray mapping
# lifecycle — create, icon/tooltip/menu setters and the programmatic
# openMenu call must all complete without a backend error marker. Visual
# presence and tray-icon clicks need a real tray host (none on Xvfb).
if [ "${WAILS_COMPAT_TRAY:-}" = "1" ]; then
  wait_log "compat: TRAY-ARMED" 30
  if grep -q "electron: tray" "$LOG"; then
    grep "electron: tray" "$LOG"
    die "tray backend errors"
  fi
  wait_log "compat: TRAY-OPEN-CALLED" 10
  TOGGLES=$(grep -c "compat: TRAY-TOGGLE visible=true" "$LOG" || true)
  if [ "$TOGGLES" -lt 1 ]; then
    grep "compat: TRAY-TOGGLE" "$LOG"
    die "attached window did not show via ToggleWindow"
  fi
  grep -q "compat: TRAY-TOGGLE2 visible=false" "$LOG" || die "second toggle did not hide the attached window"
  echo "PASS: electron tray lifecycle (create + setters + menu + attach)"
fi
FAILS=$(grep -c "compat:.*FAIL" "$LOG" || true)
if [ "$FAILS" -ne 0 ]; then
  grep "compat:" "$LOG"
  die "$FAILS compat check(s) failed"
fi
echo "RPC/transport latency table:"
grep "compat: BENCH " "$LOG"
echo "ALL PASS ($BACKEND)"
exit 0
