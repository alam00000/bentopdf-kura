# Building

Most people never need this: every release ships prebuilt binaries for Linux,
Windows and macOS, a Docker image, and the `kura-pdf` npm package. Build from
source when you are changing the engine, packaging for another platform, or
verifying the binaries yourself.

## The native engine

You need a C++17 compiler, CMake 3.20 or newer, and these libraries with their
headers: qpdf 12, FreeType, OpenJPEG 2, libjpeg, libpng, zlib and Little CMS.
OpenSSL is optional and enables `--sign`.

macOS:

```bash
brew install cmake qpdf freetype openjpeg jpeg-turbo libpng little-cms2 openssl@3
cmake -S pdfa-engine -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Debian and Ubuntu ship qpdf 11, so build qpdf 12 from source first:

```bash
sudo apt-get install -y cmake g++ ninja-build pkg-config zlib1g-dev \
  libjpeg-dev libfreetype-dev libpng-dev libopenjp2-7-dev liblcms2-dev libssl-dev
curl -sSL https://github.com/qpdf/qpdf/releases/download/v12.3.2/qpdf-12.3.2.tar.gz | tar -xz -C /tmp
cmake -S /tmp/qpdf-12.3.2 -B /tmp/qpdf-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DREQUIRE_CRYPTO_OPENSSL=ON -DUSE_IMPLICIT_CRYPTO=OFF \
  -DBUILD_DOC=OFF -DBUILD_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$HOME/qpdf-install"
cmake --build /tmp/qpdf-build --target libqpdf
cmake --install /tmp/qpdf-build --component dev && cmake --install /tmp/qpdf-build --component lib
cmake -S pdfa-engine -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$HOME/qpdf-install"
cmake --build build -j
```

That gives you `build/cli/kura` and `build/capi/libkura.a`. Add
`-DPDFA_STATIC_DEPS=ON` for a binary that depends only on the operating
system, which is how the release binaries are built.

## PDFium

Kura links PDFium statically so the CLI, the C library and the WebAssembly
module all rasterize with nothing to install alongside them. No published PDFium
binary can be linked in this way, so it is built from source:

```bash
pdfa-engine/scripts/pdfium/sync.sh              # depot_tools and the pinned source, about 4.5 GB, once
pdfa-engine/scripts/pdfium/build.sh mac-arm64   # or linux-x64, linux-arm64, win-x64, mac-x64, wasm
```

The branch and the depot_tools revision are pinned at the top of `sync.sh`.
Output lands in `pdfa-engine/third_party/pdfium-build/dist/<target>/` and CMake
picks it up automatically. To build without it, configure with
`-DPDFA_WITH_PDFIUM=OFF`; transparency at PDF/A-1 and PDF/X-1a/X-3 is then
reported instead of flattened. To require it, use `-DPDFA_WITH_PDFIUM=ON`.

`build.sh` configures PDFium as a complete static library with V8, XFA and Skia
disabled, then runs `isolate.py`, which prefixes every exported symbol with
`pdfium_` so PDFium's embedded copies of libjpeg, zlib, lcms2, OpenJPEG and
FreeType cannot collide with the engine's own.

## WebAssembly

Needs Emscripten 4 on `PATH`:

```bash
pdfa-engine/scripts/build-wasm.sh
```

The script fetches and builds qpdf, OpenJPEG and Little CMS for the wasm
target (FreeType, libjpeg and zlib come from Emscripten's ports), then builds
the engine. Output is `pdfa-engine/build-wasm/wasm/kura.js` and `kura.wasm`.
`scripts/pack-wasm-npm.sh` then assembles the npm package from it, and
`site/` picks it up for the demo.

If PDFium was built for the `wasm` target it is linked in and the module
rasterizes; otherwise the module builds without it.

## Docker

```bash
docker build -t kura pdfa-engine
docker run --rm -v "$PWD:/work" kura --level 2b in.pdf out.pdf
```

The image builds qpdf 12 from a checksum-pinned tarball and runs as an
unprivileged user. It does not include PDFium.

## CMake options

| Option | Default | Effect |
|---|---|---|
| `PDFA_BUILD_CLI` | ON | the `kura` command |
| `PDFA_BUILD_SDK` | ON | `libkura`, the C library |
| `PDFA_WITH_PDFIUM` | AUTO | raster flattening; AUTO uses a PDFium build when one is present |
| `PDFA_STATIC_DEPS` | OFF | link every third-party library statically |
| `PDFA_BUILD_FUZZ` | OFF | the fuzz harnesses and replay binaries |
| `PDFA_FUZZ_LIBFUZZER` | OFF | link the harnesses against a fuzzing engine |
| `PDFA_WERROR` | OFF | treat warnings as errors, as CI does |
| `KURA_WITH_SIGNING` | ON | `--sign` in the CLI, needs OpenSSL |

## Checks

```bash
make check    # shell syntax, YAML, version lock, npm smoke test against the wasm build
make fuzz     # a short libFuzzer run over the PDF harness
```

The sanitizer build that CI runs:

```bash
cmake -S pdfa-engine -B build-asan -DPDFA_BUILD_FUZZ=ON -DPDFA_WITH_PDFIUM=OFF \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan
```
