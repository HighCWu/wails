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
    powershell -NoProfile -Command 'Get-CimInstance Win32_Process -Filter "Name=''electron.exe''" | Where-Object { $_.CommandLine -match "type=renderer" } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }' && killed=1 && break
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
