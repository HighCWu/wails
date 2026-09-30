#!/usr/bin/env bash
# Build the native-ipc transport addon against the local Node headers.
# N-API addons are ABI-stable, so the result loads in Electron's Node as is.
#   ./build.sh [output.node]   (default: ../../../../.. relative → /tmp not used)
set -euo pipefail
cd "$(dirname "$0")"

OUT="${1:-go-bridge.node}"
# Newest node-gyp header set available locally; N-API is version-stable.
HDR="$(ls -d "$HOME"/.cache/node-gyp/*/include/node 2>/dev/null | sort -V | tail -1)"
if [ -z "$HDR" ]; then
  echo "error: no node-gyp headers under ~/.cache/node-gyp (run 'node-gyp' once or install node headers)" >&2
  exit 1
fi

cc -O2 -fPIC -shared -o "$OUT" bridge.c -I"$HDR" -lpthread
echo "built $OUT (headers: $HDR)"
