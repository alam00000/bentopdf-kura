#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NPM="$ROOT/packages/npm"
SRC="${1:-}"

fill() {
  local pkg=$1 bin=$2 from=$3
  mkdir -p "$NPM/$pkg/bin"
  cp "$from" "$NPM/$pkg/bin/$bin"
  chmod +x "$NPM/$pkg/bin/$bin"
  cp "$ROOT/LICENSE" "$NPM/$pkg/LICENSE"
  cp "$ROOT/NOTICE.md" "$NPM/$pkg/NOTICE.md"
  echo "filled $pkg from $from"
}

extract() {
  local suffix=$1 pkg=$2 bin=$3 archive tmp
  archive=$(find "$SRC" -maxdepth 1 -name "kura-*-$suffix.tar.gz" -o -maxdepth 1 -name "kura-*-$suffix.zip" | head -1)
  if [ -z "$archive" ]; then
    echo "skipping $pkg: no $suffix archive under $SRC"
    return 0
  fi
  tmp=$(mktemp -d)
  case "$archive" in
    *.zip) unzip -q "$archive" "$bin" -d "$tmp" ;;
    *) tar -xzf "$archive" -C "$tmp" "./$bin" 2>/dev/null || tar -xzf "$archive" -C "$tmp" "$bin" ;;
  esac
  fill "$pkg" "$bin" "$tmp/$bin"
  rm -rf "$tmp"
}

if [ "$SRC" = "--local" ]; then
  case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) pkg=kura-pdf-darwin-arm64 ;;
    Linux-x86_64) pkg=kura-pdf-linux-x64 ;;
    *) echo "no platform package for $(uname -s)-$(uname -m)" >&2; exit 1 ;;
  esac
  fill "$pkg" kura "$ROOT/pdfa-engine/build/cli/kura"
elif [ -d "$SRC" ]; then
  extract linux-x64 kura-pdf-linux-x64 kura
  extract macos-arm64 kura-pdf-darwin-arm64 kura
  extract windows-x64 kura-pdf-win32-x64 kura.exe
else
  echo "usage: $0 <directory with release archives> | --local" >&2
  exit 64
fi

cp "$ROOT/LICENSE" "$NPM/kura-pdf/LICENSE"
cp "$ROOT/NOTICE.md" "$NPM/kura-pdf/NOTICE.md"
echo "assembled kura-pdf"
