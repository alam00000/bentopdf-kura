# Building from source

The complete build documentation lives in [BUILDING.md](https://github.com/alam00000/bentopdf-kura/blob/main/BUILDING.md) in the repository. This page is the orientation.

Most people never need this: prebuilt binaries ship with [every release](https://github.com/alam00000/bentopdf-kura/releases), the Docker image is on GitHub's registry, and the WebAssembly build is on npm. Build from source when you are changing the engine, packaging for another platform, or verifying the binaries yourself.

## What you need

- a C++17 compiler and CMake 3.20 or newer
- qpdf 12, FreeType, OpenJPEG 2, libjpeg, libpng, zlib and Little CMS, with headers
- OpenSSL, only for `--sign`
- Google's depot_tools, python3 and about 5 GB of disk, only for the PDFium rasterizer
- Emscripten 4, only for the WebAssembly module
- Node.js 22, only for the npm package, the demo site and these docs

## The native engine

```bash
cmake -S pdfa-engine -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

That produces `build/cli/kura` and `build/capi/libkura.a`. On macOS every dependency is a `brew install` away; Debian and Ubuntu ship qpdf 11, so build qpdf 12 from source first, exactly as the CI workflows do.

Add `-DPDFA_STATIC_DEPS=ON` for a binary that depends on nothing but the operating system.

## PDFium

Kura links PDFium statically so every surface can rasterize with nothing to install alongside it. It is built from source once:

```bash
pdfa-engine/scripts/pdfium/sync.sh
pdfa-engine/scripts/pdfium/build.sh linux-x64     # or mac-arm64, mac-x64, linux-arm64, win-x64, wasm
```

CMake detects the result automatically. Without it the engine still builds and converts; transparency at PDF/A-1 and PDF/X-1a/X-3 is then reported instead of flattened.

## WebAssembly

```bash
pdfa-engine/scripts/build-wasm.sh
```

Fetches and builds qpdf, OpenJPEG and Little CMS for the wasm target, then the engine, into `pdfa-engine/build-wasm/wasm/`. `make npm-pack` assembles the npm package from it and `make site-sync` updates the demo.

## Checks

```bash
make check     # shell syntax, YAML, version lock, npm smoke test
make fuzz      # a short libFuzzer run
```

The project's conformance test lanes run against the veraPDF corpus and the public suites; they need veraPDF and several gigabytes of test files and are not part of the repository.
