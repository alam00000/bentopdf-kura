#!/usr/bin/env bash
set -euo pipefail

TARGET="${1:-linux-$(uname -m | sed "s/aarch64/arm64/;s/x86_64/x64/")}"
SRC=/work/pdfium-src
OUT="$SRC/out/$TARGET"
DIST="/out/$TARGET"

case "$TARGET" in
  linux-x64)   CPU=x64 ;;
  linux-arm64) CPU=arm64 ;;
  *) echo "unknown target $TARGET" >&2; exit 2 ;;
esac

ARGS='is_debug=false treat_warnings_as_errors=false pdf_use_skia=false
 pdf_enable_xfa=false pdf_enable_v8=false is_component_build=false
 clang_use_chrome_plugins=false pdf_is_standalone=true use_debug_fission=false
 use_custom_libcxx=false use_sysroot=false pdf_is_complete_lib=true
 pdf_use_partition_alloc=false symbol_level=0 is_clang=true'

"$SRC/buildtools/linux64/gn" gen "$OUT" --root="$SRC" --args="$(echo $ARGS)"
printf 'target_os="linux"\ntarget_cpu="%s"\n' "$CPU" >> "$OUT/args.gn"
"$SRC/buildtools/linux64/gn" gen "$OUT" --root="$SRC"

"$SRC/third_party/ninja/ninja" -C "$OUT" pdfium

mkdir -p "$DIST/include"
rsync -a --delete "$SRC/public/" "$DIST/include/public/"
cp -f "$OUT/obj/libpdfium.a" "$DIST/libpdfium.a"
echo "built $DIST/libpdfium.a"
