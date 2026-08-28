# Building the bundled PDFium

Kura links PDFium statically so the CLI, the SDK and the WebAssembly build all
rasterize with no sidecar files. No published PDFium binary can be linked in —
they are all shared libraries or a standalone Emscripten module — so it is
built from source here.

## Usage

```
./sync.sh                 # depot_tools + pdfium source (~4.5 GB, once)
./build.sh mac-arm64      # -> third_party/pdfium-build/dist/mac-arm64/
./build.sh wasm
./build.sh linux-x64
```

Both the PDFium branch and the depot_tools revision are pinned at the top of
`sync.sh`. Outputs land in `third_party/pdfium-build/dist/<target>/` as
`libpdfium.a` plus the public headers; CMake picks them up through
`PDFA_WITH_PDFIUM` (on by default) and `KURA_PDFIUM_DIR`.

## What the scripts do

`build.sh` configures with `pdf_is_complete_lib=true` and V8, XFA and Skia
disabled — Kura only ever renders — then runs the isolation step below.

`patch_tree.py` applies four small patches, needed only for wasm:

| patch | why |
| --- | --- |
| BUILDCONFIG default toolchain | upstream asserts that emscripten is only valid as a *secondary* toolchain |
| fxge font backend | wasm needs the linux font implementation |
| skia gn_check guard | `//skia` does not support a wasm target CPU |
| toolchain posix flags | PDFium's OpenJPEG needs `fseeko`; libjpeg needs the wasm longjmp ABI to match the engine's `-fwasm-exceptions` |

Chromium's build already ships a wasm toolchain, so the gn patches circulated
by other projects are written against older trees and will fail `gn gen` here.

`isolate.py` handles symbol collisions. A complete static PDFium embeds its own
libjpeg, lcms2, OpenJPEG, libpng, zlib and a custom-configured FreeType, and
the engine links its own build of each, so a static link sees two definitions
of names like `jpeg_natural_order`. On native targets every colliding symbol
PDFium defines is renamed with a `pdfium_` prefix via
`llvm-objcopy --redefine-syms`, applied across the whole archive so definitions
and internal references stay consistent. Each library then keeps its own vetted
code and neither can see the other. Only strong, C-linkage definitions are
compared: weak and C++ mangled symbols are coalesced by the linker by design,
and matching on those would flag PDFium's own objects.

`llvm-objcopy` cannot rewrite symbols in wasm objects, so the WebAssembly build
instead relies on link order — the engine's libraries precede `libpdfium.a`, so
PDFium binds to the engine's copies, which is the configuration PDFium's own
`use_system_*` build args produce.

## Linux targets

`Dockerfile.linux` + `build_in_container.sh` build PDFium for Linux in a
container, since the macOS checkout carries mac-only toolchain binaries.

```
docker build -f scripts/pdfium/Dockerfile.linux -t kura-pdfium-linux scripts/pdfium
docker run --rm -v "$PWD/third_party/pdfium-build/dist:/out" kura-pdfium-linux
```

The image pins `linux/amd64`. That is not optional: Chromium publishes its
prebuilt Linux clang (`third_party/llvm-build`) for x86-64 only, so an arm64
container cannot execute the compiler at all — it fails with
`rosetta error: failed to open elf at /lib64/ld-linux-x86-64.so.2`. On an
Apple Silicon host the whole build therefore runs emulated and takes hours;
run it on a native x86-64 machine or in CI instead.

The image also strips the `buildtools/reclient` entry from DEPS before
syncing (`strip_reclient.py`). That CIPD package has no `linux-arm64` build
and no gclient condition guarding it, so a sync fails without this; it is
only needed for distributed builds, which this recipe does not use.

The Linux archive has not been produced yet — see above for why it is
impractical on an arm64 host.

## Windows

`build.sh win-x64` runs under Git Bash on a Windows host and needs
`DEPOT_TOOLS_WIN_TOOLCHAIN=0` so the local Visual Studio install is used
rather than Google's internal toolchain. The archive is `pdfium.lib` rather
than `libpdfium.a`. Symbol renaming is skipped, as it is for wasm, so the
link resolves collisions by order.

The Windows target has not been produced yet; the workflow in
`.github/workflows/windows-static.yml` is a first cut.

## Host requirements

A Python 3.10+ with a working `pyexpat`: `gn` shells out to `python3` and
imports `plistlib`, while `emcc` needs `match` statements. `build.sh` picks a
suitable interpreter automatically and `KURA_BUILD_PYTHON` overrides it.

The `gn` and `ninja` wrappers in depot_tools need a bootstrap this recipe
skips, so the real binaries from the DEPS checkout are invoked directly.

Do not run other `gclient` commands while `sync.sh` is running; concurrent
invocations race over the shared CIPD bootstrap cache.
