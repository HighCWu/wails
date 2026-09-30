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
FAILS=$(grep -c "compat:.*FAIL" "$LOG" || true)
if [ "$FAILS" -ne 0 ]; then
  grep "compat:" "$LOG"
  die "$FAILS compat check(s) failed"
fi
echo "RPC/transport latency table:"
grep "compat: BENCH " "$LOG"
echo "ALL PASS ($BACKEND)"
exit 0
