#!/usr/bin/env bash
set -euo pipefail

QPDF_VERSION="${QPDF_VERSION:-12.3.2}"
export PDFIUM_BRANCH="${PDFIUM_BRANCH:-chromium/7961}"
QPDF_PREFIX="${KURA_QPDF_PREFIX:-$HOME/qpdf-install}"
DEPS_PREFIX="${KURA_DEPS_PREFIX:-$HOME/deps-install}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="$ROOT/pdfa-engine"
SUDO=""
if [[ "$(id -u)" != 0 ]] && command -v sudo >/dev/null 2>&1; then SUDO=sudo; fi

packages() {
  $SUDO apt-get update
  DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y --no-install-recommends \
    libglib2.0-dev clang lld gdb cmake ninja-build pkg-config make \
    zlib1g-dev libjpeg-dev libfreetype-dev libpng-dev libopenjp2-7-dev \
    liblcms2-dev libssl-dev libbz2-dev qpdf \
    git curl ca-certificates python3 xz-utils lsb-release file
}

qpdf() {
  curl -sSL -o /tmp/qpdf.tar.gz \
    "https://github.com/qpdf/qpdf/releases/download/v${QPDF_VERSION}/qpdf-${QPDF_VERSION}.tar.gz"
  tar -xzf /tmp/qpdf.tar.gz -C /tmp
  cmake -S "/tmp/qpdf-${QPDF_VERSION}" -B /tmp/qpdf-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DREQUIRE_CRYPTO_OPENSSL=ON -DUSE_IMPLICIT_CRYPTO=OFF \
    -DBUILD_DOC=OFF -DBUILD_EXAMPLES=OFF -DQPDF_TEST_COMPARE_IMAGES=OFF \
    -DCMAKE_INSTALL_PREFIX="$QPDF_PREFIX"
  cmake --build /tmp/qpdf-build --target libqpdf
  cmake --install /tmp/qpdf-build --component dev
  cmake --install /tmp/qpdf-build --component lib
  rm -rf /tmp/qpdf.tar.gz "/tmp/qpdf-${QPDF_VERSION}" /tmp/qpdf-build
}

deps() {
  curl -sSL -o /tmp/lcms2.tar.gz https://github.com/mm2/Little-CMS/releases/download/lcms2.16/lcms2-2.16.tar.gz
  tar -xzf /tmp/lcms2.tar.gz -C /tmp
  (cd /tmp/lcms2-2.16 && ./configure --prefix="$DEPS_PREFIX" --disable-shared --enable-static \
     --without-jpeg --without-tiff && make -j"$(nproc)" && make install)
  curl -sSL -o /tmp/openjpeg.tar.gz https://github.com/uclouvain/openjpeg/archive/refs/tags/v2.5.4.tar.gz
  tar -xzf /tmp/openjpeg.tar.gz -C /tmp
  cmake -S /tmp/openjpeg-2.5.4 -B /tmp/openjpeg-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
    -DBUILD_CODEC=OFF -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX"
  cmake --build /tmp/openjpeg-build
  cmake --install /tmp/openjpeg-build
  curl -sSL -o /tmp/freetype.tar.gz https://gitlab.freedesktop.org/freetype/freetype/-/archive/VER-2-13-2/freetype-VER-2-13-2.tar.gz
  tar -xzf /tmp/freetype.tar.gz -C /tmp
  cmake -S /tmp/freetype-VER-2-13-2 -B /tmp/freetype-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON \
    -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX"
  cmake --build /tmp/freetype-build
  cmake --install /tmp/freetype-build
  rm -rf /tmp/lcms2* /tmp/openjpeg* /tmp/freetype*
}

pdfium() {
  cd "$ENGINE"
  ./scripts/pdfium/sync.sh
  ./scripts/pdfium/build.sh linux-x64
  rm -rf third_party/pdfium-build/pdfium-src third_party/pdfium-build/depot_tools
}

engine() {
  cd "$ENGINE"
  cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
    -DPDFA_STATIC_DEPS=ON \
    -DCMAKE_PREFIX_PATH="$DEPS_PREFIX;$QPDF_PREFIX"
  cmake --build build --target kura kura_sdk
}

verify() {
  cd "$ENGINE"
  ldd build/cli/kura || true
  leaked=$(ldd build/cli/kura | grep '=>' \
    | grep -viE 'libc\.so|libm\.so|libdl\.so|libpthread\.so|librt\.so|libgcc_s\.so|libstdc\+\+\.so|ld-linux|linux-vdso' \
    | wc -l)
  echo "non-OS shared libraries: $leaked"
  test "$leaked" -eq 0
  test -z "$(ldd build/cli/kura | grep -i pdfium || true)"
  test -z "$(find build third_party -name 'libpdfium.so*' 2>/dev/null)"
  printf '%%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> >>\nendobj\ntrailer\n<< /Size 4 /Root 1 0 R >>\n%%%%EOF\n' > /tmp/in.pdf
  if ! report=$(./build/cli/kura --level 2b /tmp/in.pdf /tmp/out.pdf); then
    echo "conversion failed with status $?"
    if command -v gdb >/dev/null 2>&1; then
      gdb -batch -ex run -ex bt -ex 'info symbol $pc' -ex 'x/6i $pc' --args ./build/cli/kura --level 2b /tmp/in.pdf /tmp/out.pdf 2>&1 | tail -60
    fi
    exit 1
  fi
  echo "$report"
  echo "$report" | grep -q '"ok":true'
}

case "${1:-}" in
  packages|qpdf|deps|pdfium|engine|verify) "$1" ;;
  all) qpdf; deps; pdfium; engine; verify ;;
  *) echo "usage: $0 {packages|qpdf|deps|pdfium|engine|verify|all}" >&2; exit 64 ;;
esac
