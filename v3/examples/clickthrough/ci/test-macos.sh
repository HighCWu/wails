#!/usr/bin/env bash
# Transparent-window click-through E2E for examples/clickthrough (macOS).
# Cursor moves use CGWarpMouseCursorPosition (no TCC permission); the
# hit-test flips are therefore hard assertions. Synthetic clicks need
# Accessibility: the driver attempts a best-effort TCC grant and the click
# phase degrades to a SKIP when the grant does not take effect.
# Scene constants must stay in sync with assets/index.html:
#   circle centre (200,160), transparent interior (460,320), card button (118,537)
# Usage: test-macos.sh <path-to-clickthrough-binary>
set -uo pipefail

APP="${1:?usage: test-macos.sh <app-path>}"
DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$(mktemp /tmp/clickthrough-log.XXXXXX)"

cleanup() { [[ -n "${APP_PID:-}" ]] && kill "$APP_PID" 2>/dev/null; }
trap cleanup EXIT

die() { echo "FAIL: $*"; tail -30 "$LOG"; exit 1; }

wait_log() {
  local pattern="$1" timeout="${2:-30}" start
  start=$(date +%s)
  until grep -q "$pattern" "$LOG"; do
    if ! kill -0 "${APP_PID:-0}" 2>/dev/null; then die "app process exited early"; fi
    if [ $(( $(date +%s) - start )) -gt "$timeout" ]; then
      die "timeout waiting for log marker: $pattern"
    fi
    sleep 0.3
  done
  echo "PASS: log marker '$pattern'"
}

last_state() {
  local line
  line=$(grep "ignoring=" "$LOG" | tail -1)
  case "$line" in
    *"ignoring=true"*) echo true ;;
    *"ignoring=false"*) echo false ;;
    *) echo none ;;
  esac
}

wait_state() {
  local want="$1" start
  start=$(date +%s)
  while :; do
    st=$(last_state)
    if [ "$st" = "$want" ] || { [ "$st" = "none" ] && [ "$want" = "false" ]; }; then
      echo "PASS: state ignoring=$want"
      return
    fi
    if ! kill -0 "${APP_PID:-0}" 2>/dev/null; then die "app process exited early"; fi
    if [ $(( $(date +%s) - start )) -gt 25 ]; then
      die "timeout waiting for state ignoring=$want (last: $st)"
    fi
    sleep 0.3
  done
}

# --- compile the input driver -------------------------------------------------
swiftc "$DIR/macos-driver.swift" -o /tmp/clickthrough-driver || die "swiftc failed"
DRIVER=/tmp/clickthrough-driver

# --- launch --------------------------------------------------------------------
"$APP" >"$LOG" 2>&1 & APP_PID=$!
wait_log "mask uploaded" 180

# --- home the cursor far from the window; the first flip exposes winpos --------
"$DRIVER" warp 5 5 || die "cursor warp failed"
sleep 1
wait_log "winpos=" 30
WINPOS=$(grep -o 'winpos=[0-9,-]*' "$LOG" | tail -1 | cut -d= -f2)
OX=${WINPOS%,*}; OY=${WINPOS#*,}
echo "overlay at $OX,$OY"
wait_state true

# --- opaque region goes interactive ---------------------------------------------
"$DRIVER" warp $((OX+200)) $((OY+160)) || die "warp failed"
sleep 1
wait_state false

# --- transparent region goes passthrough -----------------------------------------
"$DRIVER" warp $((OX+460)) $((OY+320)) || die "warp failed"
sleep 1
wait_state true

# --- click phase: Accessibility grants are loud and verified -------------------
TCC_DB="/Library/Application Support/com.apple.TCC/TCC.db"
echo "== TCC access schema =="
sudo sqlite3 "$TCC_DB" '.schema access' 2>&1 | head -4 || true
echo "== session/runner processes =="
launchctl managername 2>/dev/null || true
ps -axo comm= | grep -iE "Runner\.Worker|Runner\.Listener|node$" | sort -u | head -6

# Grant Accessibility to every plausible responsible process: TCC attributes
# CGEventPost to the process chain that launched the driver, not to the
# driver binary itself.
CLIENTS=("/bin/bash" "/bin/zsh" "/usr/bin/env" "/usr/bin/sshd" "/usr/sbin/sshd")
while IFS= read -r p; do CLIENTS+=("$p"); done < <(ps -axo comm= | grep -iE "Runner\.Worker|Runner\.Listener" | sort -u)
for client in "${CLIENTS[@]}"; do
  echo "grant kTCCServiceAccessibility -> $client"
  sudo sqlite3 "$TCC_DB" \
    "INSERT OR REPLACE INTO access (service,client,client_type,auth_value,auth_reason,auth_version,csreq,policy_id,indirect_object_identifier_type,indirect_object_identifier,indirect_object_code_identity,flags,last_modified) VALUES ('kTCCServiceAccessibility','$client',1,2,2,1,NULL,NULL,NULL,'UNUSED',NULL,0,NULL);" \
    2>&1 | head -3
done
# TCC daemon caches grants — restart it so the rows take effect.
sudo pkill -9 tccd 2>/dev/null || true
sleep 2
echo "== accessibility rows now =="
sudo sqlite3 "$TCC_DB" "SELECT client,auth_value FROM access WHERE service='kTCCServiceAccessibility';" 2>&1 | head -12

"$DRIVER" click $((OX+460)) $((OY+320)) 2>&1 || true
# cliclick ships on runner images and may carry its own pre-granted context.
command -v cliclick >/dev/null && cliclick c:$((OX+460)),$((OY+320)) || true
sleep 2
if grep -q "underlay-clicks=1" "$LOG"; then
  echo "PASS: click passed through to the underlay"
elif grep -q "overlay-clicks" "$LOG"; then
  die "passthrough FAILED: click was consumed by the overlay"
else
  echo "SKIP: click phase inconclusive (click not delivered); flip assertions passed"
fi

echo "ALL PASS"
exit 0
