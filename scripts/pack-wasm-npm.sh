#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG="$ROOT/packages/npm/kura-pdf"
SRC="${1:-$ROOT/pdfa-engine/build-wasm/wasm}"

for f in kura.js kura.wasm; do
  if [ ! -f "$SRC/$f" ]; then
    echo "$SRC/$f not found; run make wasm first, or pass the directory holding kura.js and kura.wasm" >&2
    exit 1
  fi
done

rm -rf "$PKG/engine"
mkdir -p "$PKG/engine"
cp "$SRC/kura.js" "$SRC/kura.wasm" "$PKG/engine/"
cp "$ROOT/LICENSE" "$PKG/LICENSE"
cp "$ROOT/NOTICE.md" "$PKG/NOTICE.md"

ENGINE_VERSION="$(sed -n 's/.*kEngineVersion = "\(.*\)".*/\1/p' "$ROOT/pdfa-engine/core/include/pdfa/pdfa.hh")"
PKG_VERSION="$(node -p "require('$PKG/package.json').version")"
if [ "$ENGINE_VERSION" != "$PKG_VERSION" ]; then
  echo "version drift: engine $ENGINE_VERSION, package $PKG_VERSION" >&2
  exit 1
fi

echo "assembled $PKG ($PKG_VERSION) from $SRC"
