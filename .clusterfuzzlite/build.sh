#!/bin/bash -eu

DEPS=$WORK/deps-${SANITIZER:-address}
mkdir -p "$DEPS/lib"
export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export CMAKE_PREFIX_PATH="$DEPS:${CMAKE_PREFIX_PATH:-}"

have_lib() {
  [ -f "$DEPS/lib/$1" ]
}

build_cmake_dep() {
  local src=$1
  shift
  local bld
  bld=$(mktemp -d)
  cmake -S "$src" -B "$bld" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEPS" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_C_FLAGS="$CFLAGS" \
    -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
    "$@"
  cmake --build "$bld" -j"$(nproc)"
  cmake --install "$bld"
  rm -rf "$bld"
}

if ! have_lib libz.a; then
pushd "$SRC/zlib"
./configure --static --prefix="$DEPS"
make -j"$(nproc)" && make install
popd
fi

have_lib libjpeg.a || build_cmake_dep "$SRC/libjpeg-turbo" -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF

have_lib libpng16.a || build_cmake_dep "$SRC/libpng" -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
  -DZLIB_ROOT="$DEPS"

have_lib libfreetype.a || build_cmake_dep "$SRC/freetype" -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON \
  -DFT_DISABLE_BZIP2=ON -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON

if ! have_lib liblcms2.a; then
pushd "$SRC/lcms2"
if [ ! -x ./configure ]; then
  ./autogen.sh --prefix="$DEPS" --disable-shared --enable-static --without-jpeg --without-tiff
else
  ./configure --prefix="$DEPS" --disable-shared --enable-static --without-jpeg --without-tiff
fi
make -j"$(nproc)" && make install
popd
fi

have_lib libopenjp2.a || build_cmake_dep "$SRC/openjpeg" -DBUILD_CODEC=OFF -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_PKGCONFIG_FILES=ON

have_lib libqpdf.a || build_cmake_dep "$SRC/qpdf" -DBUILD_SHARED_LIBS=OFF -DUSE_IMPLICIT_CRYPTO=OFF \
  -DREQUIRE_CRYPTO_NATIVE=ON -DBUILD_DOC=OFF -DBUILD_DOC_DIST=OFF \
  -DBUILD_STATIC_LIBS=ON -DQTEST_SKIP_TESTS=ON

rm -rf "$WORK/build"
cmake -S "$SRC/kura/pdfa-engine" -B "$WORK/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_C_FLAGS="$CFLAGS" \
  -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
  -DCMAKE_PREFIX_PATH="$DEPS" \
  -DCMAKE_LIBRARY_PATH="$DEPS/lib" \
  -DCMAKE_EXE_LINKER_FLAGS="-L$DEPS/lib" \
  -DCMAKE_CXX_STANDARD_LIBRARIES="-lpng16 -lz" \
  -DPDFA_BUILD_CLI=OFF \
  -DPDFA_BUILD_SDK=OFF \
  -DPDFA_BUILD_FUZZ=ON \
  -DPDFA_FUZZ_LIBFUZZER=ON \
  -DPDFA_FUZZ_ENGINE="$LIB_FUZZING_ENGINE" \
  -DPDFA_WITH_PDFIUM=OFF \
  -DPDFA_WERROR=OFF

for t in convert profile invoice icc password; do
  cmake --build "$WORK/build" --target "fuzz_$t" -j"$(nproc)"
  cp "$WORK/build/fuzz/fuzz_$t" "$OUT/"
done

SEEDS=$WORK/seeds
python3 "$SRC/kura/pdfa-engine/fuzz/gen_seeds.py" "$SEEDS"

pack_seeds() {
  local target=$1
  shift
  local dir
  dir=$(mktemp -d)
  for pattern in "$@"; do
    cp "$SEEDS"/$pattern "$dir/" 2>/dev/null || true
  done
  if [ -z "$(ls -A "$dir")" ]; then
    echo "no seeds matched for $target: $*" >&2
    rm -rf "$dir"
    return 1
  fi
  (cd "$dir" && zip -q -r "$OUT/${target}_seed_corpus.zip" . )
  echo "packed $(ls -1 "$dir" | wc -l) seed(s) for $target"
  rm -rf "$dir"
}

pack_seeds fuzz_convert '*.pdf'
pack_seeds fuzz_profile 'profile_*'
pack_seeds fuzz_invoice 'invoice_*'
pack_seeds fuzz_icc 'icc_*'
pack_seeds fuzz_password 'pw_*'

cp "$SRC/kura/.clusterfuzzlite/fuzz_convert.dict" "$OUT/fuzz_convert.dict"
cp "$SRC/kura/.clusterfuzzlite/fuzz_profile.dict" "$OUT/fuzz_profile.dict"
cp "$SRC/kura/.clusterfuzzlite/fuzz_invoice.dict" "$OUT/fuzz_invoice.dict"
