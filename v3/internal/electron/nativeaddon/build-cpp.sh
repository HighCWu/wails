#!/usr/bin/env bash
# Build the C++/V8 renderer addon against Electron's own node headers.
# This addon is deliberately pinned to the Electron version: it uses the
# V8 C++ API directly (ValueSerializer/ValueDeserializer), which is what
# makes the renderer-side serialization JS-free. Rebuild on Electron bumps.
#   ./build-cpp.sh <electron-version> [output.node]
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

# V8 build-config macros MUST match the Electron binary (its config.gypi:
# pointer compression + sandbox + 31-bit Smis + external code space).
# Without them, tagged values decode wrong and the first args[i] access
# segfaults the renderer.
V8_DEFS="-DV8_COMPRESS_POINTERS -DV8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES \
  -DV8_ENABLE_SANDBOX -DV8_31BIT_SMIS_ON_64BIT_ARCH -DV8_EXTERNAL_CODE_SPACE"

LINK_EXTRA=""
STATIC_LIBS=""
WIN_DEFS=""
SRC=bridge.cc
case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*)
  # Windows builds the N-API addon (napi_bridge.cc): electron.exe exports
  # only a partial surface (core v8-direct entry points like
  # v8::Number::New / node::Buffer::Copy are absent) and no node.lib is
  # published, so the v8-direct bridge.cc cannot link or load there.
  # No V8_DEFS needed — node_api.h is pure C API. Wire serde comes from
  # node's injected v8.serialize (byte-compatible with the Go decoder).
  # No import library either: napi_bridge.cc binds the napi surface from
  # the host module at load time (see its NAPI_DYN_LIST block), so the
  # link has zero electron dependencies.
  SRC=napi_bridge.cc
  V8_DEFS=""
  STATIC_LIBS="-static-libgcc -static-libstdc++"
  ;;
esac

g++ -std=c++20 -fno-rtti -O2 -fPIC -shared -o "$OUT" "$SRC" \
  -I"$HDR" $V8_DEFS $WIN_DEFS $LINK_EXTRA -lpthread $STATIC_LIBS
echo "built $OUT from $SRC (headers: $HDR)"
