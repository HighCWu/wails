#!/usr/bin/env bash
# Build the native-ipc transport addon against the local Node headers.
# N-API addons are ABI-stable, so the result loads in Electron's Node as is.
#   ./build.sh [output.node]   (default: ../../../../.. relative → /tmp not used)
set -euo pipefail
cd "$(dirname "$0")"

OUT="${1:-go-bridge.node}"
# Header dir override for CI; N-API is ABI/version-stable so any recent
# node headers work, including inside Electron.
HDR="${NODE_HEADERS:-}"
if [ -z "$HDR" ]; then
  HDR="$(ls -d "$HOME"/.cache/node-gyp/*/include/node 2>/dev/null | sort -V | tail -1)"
fi
if [ -z "$HDR" ]; then
  echo "error: no node headers (set NODE_HEADERS or populate ~/.cache/node-gyp)" >&2
  exit 1
fi

cc -O2 -fPIC -shared -o "$OUT" bridge.c -I"$HDR" -lpthread
echo "built $OUT (headers: $HDR)"
