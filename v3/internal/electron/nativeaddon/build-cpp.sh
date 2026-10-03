#!/usr/bin/env bash
# Build the electron backend's native bridge addon (pure N-API, both the
# renderer and main-process surfaces). The addon is version-agnostic: it
# compiles against node_api.h (a stable C API) with no V8 build-config
# macros, and binds the handful of host functions it needs from the
# running module at load time — the same binary idea works across
# Electron bands. Wire serde rides node's v8 module injected by the
# preload (fallback path) or the C-side wire writer (fast path), both
# byte-compatible with the Go decoder.
#   ./build-cpp.sh <electron-version> [output.node]
#   (or BRIDGE_NODE_HEADERS to an include dir)
# Headers are fetched from electronjs.org (cached under /tmp).
set -euo pipefail
cd "$(dirname "$0")"

EV="${1:-}"
OUT="${2:-go_bridge_cpp.node}"
HDR="${BRIDGE_NODE_HEADERS:-}"
if [ -z "$EV" ] && [ -n "$HDR" ]; then
  : # explicit header dir wins
elif [ -z "$EV" ]; then
  echo "usage: build-cpp.sh <electron-version> [output.node]" >&2
  echo "       (or set BRIDGE_NODE_HEADERS to a node headers include dir)" >&2
  exit 1
fi
if [ -z "$HDR" ]; then
  CACHE="/tmp/electron-headers-$EV"
  if [ ! -d "$CACHE/node_headers/include/node" ]; then
    mkdir -p "$CACHE"
    curl -sL --max-time 120 -o "$CACHE/h.tar.gz" \
      "https://www.electronjs.org/headers/v$EV/node-v$EV-headers.tar.gz"
    tar -xzf "$CACHE/h.tar.gz" -C "$CACHE"
  fi
  HDR="$CACHE/node_headers/include/node"
fi

EXTRA=""
LINKER_FLAGS="-shared"
# BRIDGE_SERDE_TEST=1 exports the _fastSerialize/_fastDeserialize hooks
# (node's v8.serialize byte-compare in nativeaddon/test/); production
# builds stay lean.
if [ "${BRIDGE_SERDE_TEST:-}" = "1" ]; then
  EXTRA="-DWAILS_BRIDGE_SERDE_TEST"
fi
case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*)
  # windows.h hygiene + static CRT strings; the napi surface itself is
  # bound at load time (NAPI_DYN_LIST), so the link has zero electron
  # dependencies.
  EXTRA="$EXTRA -static-libgcc -static-libstdc++"
  ;;
Darwin)
  # mach-o two-level namespaces need the lookup escape: the napi symbols
  # resolve against the host process when electron loads the addon
  LINKER_FLAGS="-dynamiclib -undefined dynamic_lookup"
  ;;
esac

g++ -std=c++20 -fno-rtti -O2 -fPIC $LINKER_FLAGS -o "$OUT" bridge.cc \
  -I"$HDR" -lpthread $EXTRA
echo "built $OUT from bridge.cc (headers: $HDR)"
