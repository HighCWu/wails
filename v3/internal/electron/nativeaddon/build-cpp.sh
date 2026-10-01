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
SRC=bridge.cc
case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*)
  # Windows builds the N-API addon (napi_bridge.cc): electron.exe exports
  # only the N-API surface (verified on 44.4.5: napi_*/uv_* only, zero
  # v8:: symbols) and no node.lib is published, so the v8-direct
  # bridge.cc cannot load there. No V8_DEFS needed — node_api.h is pure
  # C API. Wire serde comes from node's injected v8.serialize
  # (byte-compatible with the Go decoder; typed arrays ride the 0x5C
  # host-object form both sides already speak).
  SRC=napi_bridge.cc
  V8_DEFS=""
  # PE linking (mingw ld) rejects unresolved symbols, unlike ELF -shared
  # where dlopen binds them from the host's export table. Build an import
  # library straight from electron.exe's export table; at LoadLibrary time
  # the loader matches the recorded module name against the host process
  # and binds every napi/uv import there — same end state as node-gyp's
  # node.lib + delay-load hook, without the hook. NOTE: binds by the name
  # "electron.exe"; a renamed host exe would need this regenerated.
  EXE="${BRIDGE_WIN_ELECTRON_EXE:-C:/electron/electron.exe}"
  if [ ! -f "$EXE" ]; then
    echo "electron exe not found: $EXE (set BRIDGE_WIN_ELECTRON_EXE)" >&2
    exit 1
  fi
  TMPD="$(mktemp -d)"
  objdump -p "$EXE" \
    | awk '/Ordinal\/Name Pointer/{f=1;next} f && /\]/{sub(/^.*\] */,""); print}' \
    | sort -u > "$TMPD/names.txt"
  COUNT="$(wc -l < "$TMPD/names.txt")"
  if [ "$COUNT" -lt 100 ]; then
    echo "electron.exe export parse yielded only $COUNT names; objdump format changed?" >&2
    exit 1
  fi
  { echo "LIBRARY electron.exe"; echo "EXPORTS"; cat "$TMPD/names.txt"; } > "$TMPD/electron.def"
  # No -m: the mingw64 dlltool defaults to its own native machine
  # (x86_64) — its accepted -m spellings vary by build ('x86_64' was
  # rejected by binutils 15.2's dlltool), and the native default is
  # exactly what we want. -m i386:x86-64 (the BFD arch string) silently
  # produced 32-bit imports that ld skipped.
  dlltool -d "$TMPD/electron.def" -l "$TMPD/electron.lib"
  LINK_EXTRA="$TMPD/electron.lib"
  STATIC_LIBS="-static-libgcc -static-libstdc++"
  echo "import library built: $COUNT exports from $EXE"
  ;;
esac

g++ -std=c++20 -fno-rtti -O2 -fPIC -shared -o "$OUT" "$SRC" \
  -I"$HDR" $V8_DEFS $LINK_EXTRA -lpthread $STATIC_LIBS
echo "built $OUT from $SRC (headers: $HDR)"
