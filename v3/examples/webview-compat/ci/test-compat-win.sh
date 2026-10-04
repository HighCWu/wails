#!/usr/bin/env bash
# Windows compat scenario driver (electron backend): the suite runs inside
# the app itself (window ops via the control protocol, events parity
# counters, bindings bench) — this driver only starts the app, waits for
# the log markers and asserts the verdict. No mouse/Xvfb tooling needed.
# Run under Git Bash on a windows runner:
#   test-compat-win.sh <app-path.exe>
set -uo pipefail
APP="${1:?usage: test-compat-win.sh <app-path.exe>}"
LOG="$(mktemp)"

APP_PID=""
cleanup() { [[ -n "$APP_PID" ]] && kill "$APP_PID" 2>/dev/null; }
trap cleanup EXIT
die() { echo "FAIL: $*"; echo "--- app log ($LOG):"; cat "$LOG"; exit 1; }

"$APP" >"$LOG" 2>&1 & APP_PID=$!

for _ in $(seq 1 240); do
  grep -q "compat: backend=" "$LOG" && break
  kill -0 "$APP_PID" 2>/dev/null || die "app exited before ready"
  sleep 0.5
done
grep -q "compat: backend=electron" "$LOG" || die "backend is not electron"

# suite: window API checks + window-event parity counters
for _ in $(seq 1 300); do
  grep -q "compat: SUITE" "$LOG" && break
  kill -0 "$APP_PID" 2>/dev/null || die "app exited during suite"
  sleep 0.5
done
grep -q "compat: SUITE PASS" "$LOG" || die "suite did not pass"

# bindings bench must finish without transport errors
for _ in $(seq 1 300); do
  grep -q "compat: BENCH-FINISHED" "$LOG" && break
  kill -0 "$APP_PID" 2>/dev/null || die "app exited during bench"
  sleep 0.5
done
grep -q "compat: BENCH-FINISHED" "$LOG" || die "bench did not finish"
if grep -q "compat: BENCH-ERROR" "$LOG"; then
  grep "compat: BENCH-ERROR" "$LOG"
  die "bench error(s)"
fi
grep -q "compat: events PASS" "$LOG" || die "window-event parity failed"

# crash-recovery leg (WAILS_COMPAT_CRASH=1): SIGKILL the electron
# renderer; the backend must report the crash and reload once
if [ "${WAILS_COMPAT_CRASH:-}" = "1" ]; then
  killed=""
  for attempt in 1 2 3 4 5; do
    if [ "$(uname -s)" = "Darwin" ]; then
      pkill -9 -f "type=renderer" && killed=1
    else
      powershell -NoProfile -Command 'Get-CimInstance Win32_Process | Where-Object { $_.Name -eq "electron.exe" -and $_.CommandLine -match "type=renderer" } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }' && killed=1
    fi
    [ -n "$killed" ] && break
    sleep 1
  done
  [ -n "$killed" ] || die "no electron renderer process found to kill"
  wait_log "compat: CRASH-EVENT" 30
  echo "PASS: crash recovery event fired"
fi

# churn leg (WAILS_COMPAT_CHURN=1): five create/close cycles must all end
# removed from the window manager
if [ "${WAILS_COMPAT_CHURN:-}" = "1" ]; then
  wait_log "compat: CHURN PASS" 90
  echo "PASS: window churn cycles complete"
fi

# apiext leg (WAILS_COMPAT_APIEXT=1): the input-free subset — the JS
# window.close chain and the programmatic menu resync. The keyboard
# chords (accelerators, Alt+F4) stay on the linux/windows drivers.
if [ "${WAILS_COMPAT_APIEXT:-}" = "1" ]; then
  wait_log "compat: WIN2-CLOSING" 60
  wait_log "compat: WIN2-GONE" 60
  echo "PASS: JS window.close chain"
  wait_log "compat: CHECK-SET" 30
  SETS=$(grep -c "menu-set id=" "$LOG" || true)
  if [ "$SETS" -lt 2 ]; then
    die "expected >= 2 menu-set events (initial + resync), got $SETS"
  fi
  echo "PASS: programmatic menu state resync ($SETS pushes)"
fi

# single-instance leg (WAILS_COMPAT_SINGLEINSTANCE=1): a second launch
# must exit quietly and the first instance must receive the callback
if [ "${WAILS_COMPAT_SINGLEINSTANCE:-}" = "1" ]; then
  wait_log "compat: SUITE PASS" 120
  "$APP" > /tmp/compat-second-instance.log 2>&1 & SECOND_PID=$!
  for _ in $(seq 1 40); do
    kill -0 "$SECOND_PID" 2>/dev/null || break
    sleep 0.5
  done
  if kill -0 "$SECOND_PID" 2>/dev/null; then
    kill "$SECOND_PID" 2>/dev/null
    die "second instance did not exit"
  fi
  wait_log "compat: SECOND-INSTANCE" 20
  echo "PASS: second instance exited and callback fired"
fi

# tray leg (WAILS_COMPAT_TRAY=1, electron backend): the Tray mapping
# lifecycle — create, setters, menu attach and the programmatic open
# must complete without a backend error marker
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

# reload leg (WAILS_COMPAT_RELOAD=1): the app reloads the page after the
# suite; the renderer re-injects, re-dials the named-pipe bridge endpoint
# and the re-run bench proves the new connection end to end
if [ "${WAILS_COMPAT_RELOAD:-}" = "1" ]; then
  for _ in $(seq 1 240); do
    grep -q "compat: RELOAD-OK\|compat: RELOAD-TIMEOUT" "$LOG" && break
    kill -0 "$APP_PID" 2>/dev/null || die "app exited during reload"
    sleep 0.5
  done
  grep -q "compat: RELOAD-OK" "$LOG" || { grep "compat: RELOAD" "$LOG"; die "reload reconnection failed"; }
  BENCHES=$(grep -c "compat: BENCH-FINISHED" "$LOG" || true)
  [ "$BENCHES" -ge 2 ] || die "expected >= 2 bench completions (pre + post reload), got $BENCHES"
  echo "PASS: same-process reload re-connected ($BENCHES bench runs)"
fi

echo "PASS: windows electron compat (suite + events + bench)"
