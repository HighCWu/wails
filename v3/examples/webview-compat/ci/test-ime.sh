#!/usr/bin/env bash
# IME scenario (electron backend): a private Xvfb + dbus + ibus-daemon with
# the deterministic wails-compat engine (F6 starts 中文输入 preedit, Space
# commits it), then real key events through xdotool. The assertions are the
# host log lines "compat: IME compositionstart/compositionend/value" — the
# full preedit -> composition events -> commit -> bindings RPC chain.
# Ported from the CEF backend's smoke.py --ime (cef-backend branch).
set -uo pipefail
APP="${1:?usage: test-ime.sh <app-path>}"
DISP="${CLICKTHROUGH_DISPLAY:-:98}"
LOG="$(mktemp /tmp/ime-log.XXXXXX)"
DIR="$(cd "$(dirname "$0")" && pwd)"

APP_PID=""; WM_PID=""; ENGINE_PID=""; IBUS_PID=""; DBUS_PID=""; XVFB_PID=""
cleanup() {
  for p in "$APP_PID" "$WM_PID" "$ENGINE_PID" "$IBUS_PID" "$DBUS_PID" "$XVFB_PID"; do
    [[ -n "$p" ]] && kill "$p" 2>/dev/null
  done
}
trap cleanup EXIT

die() { echo "FAIL: $*"; tail -40 "$LOG"; exit 1; }

Xvfb "$DISP" -screen 0 1280x800x24 >/dev/null 2>&1 & XVFB_PID=$!
sleep 1
export DISPLAY="$DISP"
export XDG_CONFIG_HOME="$(mktemp -d)"
export XDG_RUNTIME_DIR="$(mktemp -d)"
chmod 700 "$XDG_RUNTIME_DIR"

dbus-daemon --session --nofork --print-address=1 > "$XDG_RUNTIME_DIR/bus-addr" 2>/dev/null & DBUS_PID=$!
sleep 1
export DBUS_SESSION_BUS_ADDRESS="$(cat "$XDG_RUNTIME_DIR/bus-addr")"

export GTK_IM_MODULE=ibus
export XMODIFIERS="@im=ibus"
IBUS_SOCKET="$XDG_RUNTIME_DIR/ibus.socket"
rm -f "$IBUS_SOCKET"
export IBUS_ADDRESS="unix:path=$IBUS_SOCKET"
ibus-daemon --single --xim --address="$IBUS_ADDRESS" \
  --panel=disable --config=disable --emoji-extension=disable \
  >/dev/null 2>&1 & IBUS_PID=$!
sleep 1
/usr/bin/python3 "$DIR/ime_engine.py" > "$XDG_RUNTIME_DIR/ime-engine.log" 2>&1 & ENGINE_PID=$!
sleep 1
# Hard isolation guard: the engine selection MUST address the private
# test daemon — a stale/missing IBUS_ADDRESS would fall back to the
# user's session bus and switch THEIR input engine.
case "${IBUS_ADDRESS:-}" in
  "unix:path=$XDG_RUNTIME_DIR/"*) ;;
  *) die "IBUS_ADDRESS did not land in the private runtime dir" ;;
esac
ibus engine wails-compat >/dev/null 2>&1 || die "could not select the wails-compat engine"
openbox >/dev/null 2>&1 & WM_PID=$!
sleep 1

env WAILS_WEBVIEW_BACKEND=electron \
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

WID=$(xdotool search --onlyvisible --name "^webview-compat$" | head -1)
[[ -n "$WID" ]] || die "app window not found"
xdotool windowactivate --sync "$WID"
sleep 1.5

# F6 -> deterministic preedit 中文输入 (compositionstart); Space -> commit
# (compositionend + input value). The page keeps the IME input focused.
xdotool key F6
sleep 0.8
xdotool key space
sleep 1

for _ in $(seq 1 100); do
  grep -q "compat: IME value" "$LOG" && break
  kill -0 "$APP_PID" 2>/dev/null || die "app exited during IME leg"
  sleep 0.3
done

grep -q "compat: IME compositionstart" "$LOG" || die "compositionstart not reported"
grep -q "compat: IME compositionend 中文输入" "$LOG" || die "compositionend 中文输入 not reported"
grep -q "compat: IME value 中文输入" "$LOG" || die "committed value 中文输入 not reported"
echo "PASS: IBus Chinese preedit, composition events, commit and bindings RPC"
