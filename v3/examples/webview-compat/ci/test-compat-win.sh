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
die() { echo "FAIL: $*"; tail -40 "$LOG"; exit 1; }

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

echo "PASS: windows electron compat (suite + events + bench)"
