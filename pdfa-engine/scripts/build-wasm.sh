#!/usr/bin/env bash
set -euo pipefail

ENGINE="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ENGINE/third_party"
QPDF_VERSION=12.3.2
OPENJPEG_VERSION=2.5.4
LCMS2_VERSION=2.16
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

command -v emcmake >/dev/null 2>&1 || { echo "emscripten is not on PATH (emcmake not found)" >&2; exit 1; }
mkdir -p "$TP"

fetch() {
  local url=$1 dest=$2
  [ -f "$dest" ] && return 0
  echo "==> fetching $(basename "$dest")"
  curl -sSL --fail -o "$dest" "$url"
}

embuilder build zlib libjpeg freetype libpng >/dev/null
SYSROOT="$(em-config CACHE)/sysroot"
PORTS_INC="$SYSROOT/include"
PORTS_LIB="$SYSROOT/lib/wasm32-emscripten"

if [ ! -f "$TP/build-qpdf-wasm/libqpdf/libqpdf.a" ]; then
  fetch "https://github.com/qpdf/qpdf/releases/download/v${QPDF_VERSION}/qpdf-${QPDF_VERSION}.tar.gz" "$TP/qpdf.tar.gz"
  [ -d "$TP/qpdf-${QPDF_VERSION}" ] || tar -xzf "$TP/qpdf.tar.gz" -C "$TP"
  echo "==> building qpdf for wasm"
  emcmake cmake -S "$TP/qpdf-${QPDF_VERSION}" -B "$TP/build-qpdf-wasm" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
    -DREQUIRE_CRYPTO_NATIVE=ON -DUSE_IMPLICIT_CRYPTO=OFF \
    -DBUILD_DOC=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
    -DCMAKE_CXX_FLAGS=-fwasm-exceptions \
    -DZLIB_INCLUDE_DIR="$PORTS_INC" -DZLIB_LIBRARY="$PORTS_LIB/libz.a" \
    -DJPEG_INCLUDE_DIR="$PORTS_INC" -DJPEG_LIBRARY="$PORTS_LIB/libjpeg.a" >/dev/null
  cmake --build "$TP/build-qpdf-wasm" --target libqpdf -j"$JOBS"
fi

if [ ! -f "$TP/build-openjpeg-wasm/bin/libopenjp2.a" ]; then
  fetch "https://github.com/uclouvain/openjpeg/archive/refs/tags/v${OPENJPEG_VERSION}.tar.gz" "$TP/openjpeg.tar.gz"
  [ -d "$TP/openjpeg-${OPENJPEG_VERSION}" ] || tar -xzf "$TP/openjpeg.tar.gz" -C "$TP"
  echo "==> building openjpeg for wasm"
  emcmake cmake -S "$TP/openjpeg-${OPENJPEG_VERSION}" -B "$TP/build-openjpeg-wasm" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
    -DBUILD_CODEC=OFF -DBUILD_TESTING=OFF -DBUILD_DOC=OFF >/dev/null
  cmake --build "$TP/build-openjpeg-wasm" -j"$JOBS"
fi

if [ ! -f "$TP/build-lcms2-wasm/src/liblcms2.a" ]; then
  fetch "https://github.com/mm2/Little-CMS/archive/refs/tags/lcms${LCMS2_VERSION}.tar.gz" "$TP/lcms2.tar.gz"
  if [ ! -d "$TP/lcms2" ]; then
    tar -xzf "$TP/lcms2.tar.gz" -C "$TP"
    mv "$TP/Little-CMS-lcms${LCMS2_VERSION}" "$TP/lcms2"
  fi
  echo "==> building lcms2 for wasm"
  mkdir -p "$TP/build-lcms2-wasm"
  (cd "$TP/build-lcms2-wasm" && emconfigure "$TP/lcms2/configure" --host=wasm32-unknown-emscripten \
      --disable-shared --enable-static --without-jpeg --without-tiff --without-zlib >/dev/null \
    && emmake make -j"$JOBS" -C src >/dev/null)
fi

echo "==> building the engine"
emcmake cmake -S "$ENGINE" -B "$ENGINE/build-wasm" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPDFA_BUILD_CLI=OFF -DPDFA_BUILD_SDK=OFF -DPDFA_BUILD_FUZZ=OFF >/dev/null
cmake --build "$ENGINE/build-wasm" --target pdfa_wasm -j"$JOBS"

ls -la "$ENGINE/build-wasm/wasm/kura.js" "$ENGINE/build-wasm/wasm/kura.wasm"
if grep -q 'PDFA_WITH_PDFIUM:.*=ON' "$ENGINE/build-wasm/CMakeCache.txt"; then
  echo "==> built with PDFium: the module rasterizes"
else
  echo "==> built without PDFium: pages that need flattening are reported, not rasterized"
fi
