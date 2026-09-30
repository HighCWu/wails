#!/usr/bin/env bash
# Builds the go-bridge N-API addon (benchmark PoC).
#   build.sh [output-dir]
# Requires: gcc, node (for the header version), curl/tar.
set -euo pipefail
OUT_DIR="${1:-$(pwd)}"
NODE_VERSION=$(node -p "process.versions.node")
HDR="/tmp/node-v$NODE_VERSION-headers"
if [ ! -f "$HDR/include/node/node_api.h" ]; then
  mkdir -p "$HDR"
  curl -sL "https://nodejs.org/dist/v$NODE_VERSION/node-v$NODE_VERSION-headers.tar.gz" | tar -xz -C "$HDR"
fi
gcc -O2 -shared -fPIC -pthread \
  -I"$HDR/include/node" \
  "$(dirname "$0")/bridge.c" \
  -o "$OUT_DIR/go-bridge.node" \
  -lpthread
echo "built $OUT_DIR/go-bridge.node (node headers v$NODE_VERSION, N-API stable ABI)"
