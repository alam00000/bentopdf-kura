# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[semantic versioning](https://semver.org/).

## [Unreleased]

## [1.2.0] - 2026-08-29

### Added

- `kura-pdf` on npm is now the native engine for Node.js: the binary arrives
  through `@bentopdf/kura-pdf-linux-x64`, `@bentopdf/kura-pdf-darwin-arm64`
  or `@bentopdf/kura-pdf-win32-x64`, runs in a subprocess per call, and the
  `kura` command is the native CLI with every flag, signing and OCR included.
  The API adds `sign`, `ocr`, `ocrEngine`, `fontFolder`, `timeoutMs` and
  `binaryPath()`.
- The npm packages are published by CI with provenance through npm's trusted
  publishing; no publishing token exists anywhere.

### Changed

- The WebAssembly build moved to `kura-pdf-wasm`, with the same API and a
  `kura-wasm` command.
- The converter's level menu shows the plain standard names, and the preflight
  page lives at `/preflight`.

## [1.1.0] - 2026-08-28

First tri-platform release: native CLI, C library and WebAssembly module,
covering 25 conversion targets across PDF/A, PDF/UA, PDF/X, PDF/E and PDF/VT,
with e-invoice, signing and OCR support.

### Added

- Self-hosting: a Docker image that serves the converter and preflight pages
  and an HTTP API on the native engine, with a compose file, optional
  bearer-token authentication, upload, concurrency and time limits, and a
  `/healthz` endpoint; the same image is the CLI when given arguments.
- `kura --verify-password`, which reports whether a password opens a file
  without converting it.
- Self-contained Linux, Windows and macOS binaries built and checked in CI:
  every conversion runs with no shared library beyond the operating system's.

- The `kura-pdf` npm package: the engine compiled to WebAssembly with a
  buffer-based API and a `kura` command that mirrors the native CLI.
- A documentation site under `docs/`, and the demo site restyled to match the
  rest of BentoPDF.
- A reproducible robustness benchmark, `make bench`, over public corpora with every
  file's origin and SHA-256 recorded, reported in `benchmark.md` with a veraPDF-verified
  sample of the outputs.
- A preflight library of 396 profiles written by BentoPDF from the Ghent Workgroup
  2022 specifications, the ISO standards and prepress practice, generated from
  one script, with readable property and built-in names in the JSON dialect.
- `verifyPassword` in the WebAssembly module and the npm package, and locked
  files on the demo site show an Unlock box on their own card, with the shared
  password on the right for batches that use one.
- `kura --help` and `-h`, and a documented exit-status contract: 0 ok, 1 check
  found findings, 2 input rejected, 3 timeout, 64 usage error.
- Release automation: tagged builds for Linux, Windows and macOS, the
  WebAssembly module, a Docker image and the npm package, each with checksums.
- CodeQL, dependency updates, a CLA check and container scanning in CI.

### Fixed

- The self-contained Linux binary crashed on its first rendering call: the
  rendering library's objects used a relocation format the distribution's
  linker did not understand, so its code was linked without relocations. They
  now use standard relocations.
- The Windows build did not compile, called a time function that Windows lacks,
  and lost the symbols of assembly-built members of the rendering library while
  isolating them from the engine's own libraries; the isolation now rewrites
  the symbol tables itself and rebuilds the archive with the linker's librarian.
- The conversion watchdog was one timer for the whole process, so a batch of
  many documents tripped it; it now covers each document separately.
- The Docker image copied a gitignored qpdf source tree and could not build from
  a clean clone; it now downloads a pinned release.
- `--image-max-ppi` accepted negative and non-numeric values.
- Malformed font width arrays, preflight profiles and ToUnicode maps could make
  conversion run forever; all three are now bounded.
- Tiling patterns and Type 3 glyph procedures were emptied during PDF/X-1a
  colour conversion.
- Overprint and knockout fixups wrote their graphics state between an
  operator's operands and the operator, discarding text.
- Shading patterns carrying transparency were invisible to the transparency
  scanner, so PDF/A-1 output could claim conformance it did not have.
- Values lifted from an input's XMP were written back unescaped and unvalidated.
- A declared `/Trapped true` was overwritten with false on every PDF/X
  conversion.
- Numbers were emitted through the C locale, so a German host produced
  `612,00` inside content streams.
- Hidden optional-content layers were made visible instead of discarded when a
  profile asked for layers to be flattened.
- A checked checkbox was rewritten to its off state when its appearance stream
  was normalized.
- Text render modes 4 to 7 lost their clipping path when text was outlined.
- Lab colour was converted as if it were RGB.
- Signing a document that already carried a `/Contents <hex>` string could
  overwrite the wrong one.
- Enrolling an unlisted attachment rebuilt the embedded-file name tree from the
  root node only, dropping every attachment held in a `/Kids` subtree.
- PDF/UA-2 structure destinations all resolved to the document element.
- A failed JPEG 2000 decode left a stale alpha buffer that a later image could
  read past the end of.
- A 64-bit JPEG 2000 box length wrapped its own bounds check.
- The CLI watchdog exited with status 0 on timeout; it now exits 3.
- An OCR image path containing a quote character broke the tesseract command
  line.
- `version()` in the WebAssembly build reported the engine's old internal name.
- Finding texts listed at most eight pages, eight spot colourants or four page
  sizes and then an ellipsis; every one is now listed, with consecutive pages
  shown as ranges.
- Five source files relied on standard headers reaching them through other
  includes, so GCC could not build `fonts_glyph.cpp`.

### Changed

- The preflight engine is split into parsing, event model, scanning, evaluation
  and fixup units, and every recursion and size cap is named in one header.
- PDFium is detected automatically; a build without it configures cleanly.
- The Linux release build is one script, `scripts/linux-static.sh`, shared by
  CI and the Docker image, and the caches of every expensive build step are
  saved as soon as that step finishes.
