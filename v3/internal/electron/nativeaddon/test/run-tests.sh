#!/usr/bin/env bash
# Native addon unit tests (no Electron needed): the fast-serde corpus
# byte-compares against node's v8.serialize, the control-plane harness
# drives mainEntry over a fake stdio protocol, and the reconnect test
# proves a re-injection EOFs the old connection instead of leaking it.
#   run-tests.sh <electron-version>
# Builds a dedicated test addon (with the serde-test exports) from the
# same header cache the production build populates.
set -euo pipefail
EV="${1:?usage: run-tests.sh <electron-version>}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_ADDON="$(mktemp /tmp/go_bridge_test.XXXXXX.node)"
BRIDGE_SERDE_TEST=1 bash "$DIR/../build-cpp.sh" "$EV" "$TEST_ADDON"
export WAILS_ELECTRON_TEST_ADDON="$TEST_ADDON"
FAIL=0

echo "== serde corpus"
node "$DIR/napi_corpus.js" || FAIL=1

echo "== main-surface harness"
node "$DIR/main_driver.js" 2>&1 | grep -q "MAIN-HARNESS PASS" || { echo "FAIL: main harness"; FAIL=1; }

case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*)
  # the reconnect test dials a unix socket — windows CI skips it (the
  # named-pipe path rides the same handshake and is covered end to end
  # by the reload scenario)
  echo "== reconnect regression: skipped on windows"
  ;;
*)
  echo "== reconnect regression"
  node "$DIR/reconn_test.js" 2>&1 | grep -q "RECONN-TEST PASS" || { echo "FAIL: reconn"; FAIL=1; }
  ;;
esac

if [ "$FAIL" -ne 0 ]; then
  echo "ADDON TESTS FAILED"
  exit 1
fi
echo "ADDON TESTS PASS"
