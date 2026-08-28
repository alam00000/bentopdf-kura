#!/usr/bin/env bash
set -euo pipefail

TARGET="${1:?usage: build.sh <mac-arm64|mac-x64|wasm|linux-x64|linux-arm64>}"

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../third_party/pdfium-build" && pwd)"
SRC="$ROOT/pdfium-src"
OUT="$SRC/out/$TARGET"
DIST="$ROOT/dist/$TARGET"

export PATH="$ROOT/depot_tools:$PATH"
export DEPOT_TOOLS_UPDATE=0

pick_python() {
  local c
  for c in "${KURA_BUILD_PYTHON:-}" python3.13 python3.12 python3.11 \
           /opt/homebrew/bin/python3.11 python3 /usr/bin/python3; do
    [[ -n "$c" ]] || continue
    c="$(command -v "$c" 2>/dev/null || true)"
    [[ -x "$c" ]] || continue
    "$c" - <<'EOF' 2>/dev/null && { echo "$c"; return 0; }
import plistlib, sys
sys.exit(0 if sys.version_info[:2] >= (3, 10) else 1)
EOF
  done
  return 1
}

PY="$(pick_python)" || {
  echo "need a python3 >= 3.10 with a working pyexpat (gn needs plistlib," \
       "emcc needs match statements); set KURA_BUILD_PYTHON" >&2; exit 6; }
PYSHIM="$ROOT/.pyshim"
mkdir -p "$PYSHIM"
ln -sf "$PY" "$PYSHIM/python3"
export PATH="$PYSHIM:$PATH"

case "$(uname -s)" in
  Darwin)      GN="$SRC/buildtools/mac/gn"; EXE="" ;;
  MINGW*|MSYS*|CYGWIN*) GN="$SRC/buildtools/win/gn.exe"; EXE=".exe" ;;
  *)           GN="$SRC/buildtools/linux64/gn"; EXE="" ;;
esac
NINJA="$SRC/third_party/ninja/ninja$EXE"
OBJCOPY="$SRC/third_party/llvm-build/Release+Asserts/bin/llvm-objcopy$EXE"
if [[ ! -x "$OBJCOPY" ]]; then
  for candidate in "$(command -v llvm-objcopy 2>/dev/null || true)" "/c/Program Files/LLVM/bin/llvm-objcopy.exe" "/usr/bin/llvm-objcopy" "/opt/homebrew/opt/llvm/bin/llvm-objcopy"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      OBJCOPY="$candidate"
      break
    fi
  done
fi

for tool in "$GN" "$NINJA" "$OBJCOPY"; do
  [[ -x "$tool" ]] || { echo "missing $tool (run sync.sh first)" >&2; exit 5; }
done

ARGS='is_debug=false treat_warnings_as_errors=false pdf_use_skia=false
 pdf_enable_xfa=false pdf_enable_v8=false is_component_build=false
 clang_use_chrome_plugins=false pdf_is_standalone=true use_debug_fission=false
 use_custom_libcxx=false use_sysroot=false pdf_is_complete_lib=true
 pdf_use_partition_alloc=false symbol_level=0'

case "$TARGET" in
  mac-arm64)   EXTRA=$'target_os="mac"\ntarget_cpu="arm64"';   ARGS="$ARGS is_clang=true" ;;
  mac-x64)     EXTRA=$'target_os="mac"\ntarget_cpu="x64"';     ARGS="$ARGS is_clang=true" ;;
  linux-x64)   EXTRA=$'target_os="linux"\ntarget_cpu="x64"';   ARGS="$ARGS is_clang=true use_glib=false" ;;
  linux-arm64) EXTRA=$'target_os="linux"\ntarget_cpu="arm64"'; ARGS="$ARGS is_clang=true use_glib=false" ;;
  win-x64)     EXTRA=$'target_os="win"\ntarget_cpu="x64"';     ARGS="$ARGS is_clang=true" ;;
  wasm)
    EMROOT="${EMSCRIPTEN_ROOT:-$(dirname "$(dirname "$(readlink "$(command -v emcc)" \
             || command -v emcc)")")/libexec}"
    [[ -x "$EMROOT/emcc" ]] || EMROOT="$(dirname "$(command -v emcc)")"
    EXTRA=$'target_os="emscripten"\ntarget_cpu="wasm"\nemscripten_path="'"$EMROOT"$'/"'
    ARGS="$ARGS is_clang=true use_glib=false"
    ;;
  *) echo "unknown target: $TARGET" >&2; exit 2 ;;
esac

python3 "$HERE/patch_tree.py" "$SRC" "$TARGET"

rm -rf "$OUT"
"$GN" gen "$OUT" --root="$SRC" --args="$(echo $ARGS)"
printf '%s\n' "$EXTRA" >> "$OUT/args.gn"
"$GN" gen "$OUT" --root="$SRC"

"$NINJA" -C "$OUT" pdfium

AR_IN="$OUT/obj/libpdfium.a"
[[ -f "$AR_IN" ]] || AR_IN="$OUT/obj/pdfium.lib"
[[ -f "$AR_IN" ]] || { echo "expected $AR_IN" >&2; exit 3; }

mkdir -p "$DIST/include"
rsync -a --delete "$SRC/public/" "$DIST/include/public/"
case "$TARGET" in
  win-*) cp -f "$AR_IN" "$DIST/pdfium.lib" ;;
  *)     cp -f "$AR_IN" "$DIST/libpdfium.a" ;;
esac

case "$TARGET" in
  win-*) python3 "$HERE/isolate.py" "$DIST/pdfium.lib" "$OBJCOPY" "$TARGET" ;;
  *)     python3 "$HERE/isolate.py" "$DIST/libpdfium.a" "$OBJCOPY" "$TARGET" ;;
esac

echo "built PDFium for $TARGET in $DIST"
