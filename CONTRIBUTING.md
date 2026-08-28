# Contributing

Thanks for wanting to help. Two things to know before your first pull request:

1. **CLA.** BentoPDF uses a dual licensing model (AGPL-3.0 plus a commercial
   license), so we need a signed [Contributor License Agreement](ICLA.md)
   before we can merge anything. The CLA bot will prompt you on your first
   pull request; signing is a single comment and only happens once. Corporate
   contributors: see [CCLA.md](CCLA.md).
2. **Bug fixes come with their PDF.** The engine's behaviour depends almost
   entirely on the input document. A bug report or fix without a reproducing
   file is very hard to act on, and a fix without one cannot be kept fixed.
   Attach the smallest file that still shows the problem.

## Getting set up

```bash
make build        # the native CLI and C library
make check        # shell syntax, YAML, version lock, wasm smoke test
```

`make build` needs a C++17 compiler, CMake, qpdf 12, FreeType, OpenJPEG,
libjpeg, zlib and Little CMS. On macOS every one is in Homebrew; on Debian and
Ubuntu build qpdf 12 from source, as the CI workflows do, because the
distribution ships qpdf 11. The full instructions, including PDFium and the
WebAssembly build, are in [BUILDING.md](BUILDING.md).

PDFium is optional and detected automatically. Without it, transparency at
PDF/A-1 and PDF/X-1a/X-3 is reported instead of flattened, and everything else
works.

## Layout

- `pdfa-engine/core` is the engine: one buffer in, one buffer out, no
  filesystem, no network. Every conformance pass is one file under `src/`.
- `pdfa-engine/cli`, `capi`, `wasm` and `raster` are front ends over the same
  engine. Behaviour belongs in the engine; front ends stay thin.
- `pdfa-engine/profiles` is the preflight profile library.
- `packages/npm/kura-pdf` is the npm package, built from the WebAssembly module.
- `site` is the browser demo, `docs` the documentation site.

## Rules of the road

- **No comments.** The code in this repository is deliberately comment-free, in
  every language: C++, Python, shell, CMake, YAML. Write code that does not need
  them, and put the reasoning in the commit message.
- **One-line commits** with a conventional prefix: `feat:`, `fix:`, `perf:`,
  `refactor:`, `test:`, `build:`, `ci:`, `chore:`, `docs:`.
- **C++17, 2-space indent, 100 columns.** `.clang-format` is authoritative; run
  `clang-format -i` on the files you touch.
- **The C ABI is additive only** within a major version. `KURA_VERSION` in
  `pdfa-engine/core/include/kura/kura.h` must match `kEngineVersion` in
  `pdfa/pdfa.hh` and the npm package version; `make check` fails if they drift.
- **Never report clean when unsure.** A pass that could not finish must record
  a `SCAN_INCOMPLETE` finding rather than stay silent; a conversion that cannot
  make the document conform must refuse with a reason rather than write a file
  that only claims to conform.
- **Parsers get fuzzed.** If your change touches a decoder or parser, run the
  matching harness for a while before opening the pull request:

  ```bash
  make fuzz
  ```

## Before you open the pull request

- `make check` passes.
- The change is exercised by something: a new seed under
  `pdfa-engine/fuzz/gen_seeds.py` for parser work, or the reproducing PDF for a
  bug fix.
- For engine changes, the WebAssembly module is rebuilt (`make wasm`) so the
  demo site and npm package ship the same behaviour as the native binary.

## Security

Do not open public issues for suspected vulnerabilities. See
[SECURITY.md](SECURITY.md).
