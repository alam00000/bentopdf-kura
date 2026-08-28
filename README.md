<p align="center"><img src="docs/public/images/logo.svg" width="80"></p>

<h1 align="center">Kura</h1>

<p align="center">
  <strong>The open source PDF standards and preflight engine.</strong>
</p>

Kura converts everyday PDFs into archival, print and accessible ones: every PDF/A level, PDF/UA, PDF/X, PDF/E and PDF/VT, from one engine. It repairs what is wrong, embeds what is missing, converts colour where the standard demands it, and writes a file that validates clean, or tells you exactly why it cannot.

It is also a full print preflight engine: 396 bundled profiles check and repair what a press cares about, such as hairlines, rich black, low-resolution images, overprint, transparency, page boxes, fonts and colour, and you can write your own in JSON or bring the XML dialect you already use.

The name comes from the 蔵 (*kura*), the traditional Japanese storehouse where a family kept the things meant to outlast them. That is what PDF/A is for.

Kura is part of the [BentoPDF](https://github.com/alam00000/bentopdf) suite. The same engine is available as a CLI, a C library, an npm package, a Docker image and a WebAssembly build that runs entirely in the browser.

Try the converter at [kura.bentopdf.com](https://kura.bentopdf.com) and the preflight tool at [kura.bentopdf.com/preflight.html](https://kura.bentopdf.com/preflight.html). Full documentation lives at [kura.bentopdf.com/docs](https://kura.bentopdf.com/docs/).

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](https://github.com/alam00000/bentopdf-kura/blob/main/LICENSE) ![GitHub Stars](https://img.shields.io/github/stars/alam00000/bentopdf-kura?style=social)

---

## Table of Contents

* [Why Kura](#why-kura)
* [Does it actually work?](#does-it-actually-work)
* [Install](#install)
* [Usage](#usage)

  * [CLI](#cli)
  * [npm package](#npm-package)
  * [Browser (WebAssembly)](#browser-webassembly)
  * [Docker](#docker)
  * [C API](#c-api)
* [The standards](#the-standards)
* [Preflight](#preflight)
* [What the engine does](#what-the-engine-does)
* [What the engine refuses to do](#what-the-engine-refuses-to-do)
* [Building from source](#building-from-source)
* [Project layout](#project-layout)
* [Licensing](#licensing)
* [Contributing](#contributing)

---

## Why Kura

Making a PDF conform to a standard has three problems: **tools that quietly produce a file which only claims to conform, tools that destroy content to get there, and tools that cover one standard and leave you to find another for the next.**

Kura is built around solving those three.

* **Honest output.** Every conversion is checked. When the engine cannot make a document conform, it refuses with a reason and, where one exists, the level that would work. It never writes a file that claims a standard it does not meet, and its check mode has never reported a false finding in testing.

* **Content preserving.** Text stays searchable, attachments stay attached, signatures are re-applied in the same pass, and pages are rasterized only when a rule leaves no other way, never silently.

* **Every standard, one engine.** All eleven PDF/A levels, PDF/UA-1 and UA-2, PDF/X-1a through X-6, PDF/E-1 and PDF/VT, plus Factur-X, ZUGFeRD, XRechnung and Order-X e-invoices, with the same options and the same report everywhere.

* **Preflight built in.** The same engine runs print preflight: 396 bundled profiles for hairlines, rich black, image resolution, overprint, transparency, page boxes, fonts and colour, each a set of checks with repairs where a repair exists, plus your own in JSON or the XML dialect you already use.

* **Runs anywhere.** About twenty thousand lines of C++ on qpdf, FreeType, Little CMS, OpenJPEG and PDFium, linked statically. The CLI is one file, the browser build is one `.wasm`, and both rasterize with nothing extra to install.

---

## Does it actually work?

Kura validates clean against every public conformance suite there is:

| Suite | What it tests | Result |
|---|---|---|
| veraPDF corpus | PDF/A, all parts, 11 flavours | 0 red |
| Isartor | PDF/A-1b | 204 / 204 |
| BFO | PDF/A-1b and 2b | 34 / 34 |
| Ghent Output Suite 5.0 | PDF/X-4 print | 69 / 69 |
| PDF/UA Reference Suite | accessibility | 10 / 10 |
| Cal Poly PDF/VT | variable-data print | 24 / 24 |

Check mode agrees with veraPDF on 568 of 568 files at PDF/A-1b and 954 of 970 at 2b, and every disagreement is a defect the converter cannot repair rather than a false finding.

For robustness, the engine converted **thirty thousand real-world PDFs from the open web without a crash**. It is fuzzed continuously by ClusterFuzzLite in this repository's own CI through five harnesses, built and tested under AddressSanitizer and UndefinedBehaviorSanitizer on every push, and refuses pathological files with an error instead of falling over.

> [!NOTE]
> Check mode sees exactly what the converter can repair. That means it never reports a false finding, but it is blind to any defect the converter itself cannot detect. It is a preflight, not a replacement for an independent validator.

---

## Install

Prebuilt binaries for Linux, Windows and macOS are attached to each [release](https://github.com/alam00000/bentopdf-kura/releases), with a `SHA256SUMS` file.

```bash
npm install -g kura-pdf          # the engine as WebAssembly, plus the kura command
```

```bash
docker run --rm -v "$PWD:/work" ghcr.io/alam00000/bentopdf-kura:latest --level 2b in.pdf out.pdf
```

To build the engine yourself, see [BUILDING.md](https://github.com/alam00000/bentopdf-kura/blob/main/BUILDING.md).

---

## Usage

### CLI

```bash
kura --level 2b in.pdf out.pdf                     # PDF/A-2b, the usual archival choice

kura --level 2a --ua in.pdf out.pdf                # add PDF/UA accessibility

kura --level x1a in.pdf out.pdf                    # PDF/X-1a, real ICC-based RGB to CMYK

kura --check --level 2b in.pdf                     # what would have to change? writes nothing

kura --check --level 2b --profile hairlines.json in.pdf   # print preflight: findings, writes nothing

kura --einvoice invoice.xml in.pdf out.pdf         # Factur-X / ZUGFeRD / XRechnung hybrid

kura --level 2b -r -d out/ inbox/                  # a whole folder, JSON array on stdout

kura --help                                        # every flag
```

Every run prints a JSON report of what changed:

```json
{"ok":true,"level":"2b","engine":"BentoPDF Kura Engine 1.1.0","issues":[
  {"code":"OUTPUT_INTENT_ADDED","detail":"added sRGB PDF/A output intent","fixed":true},
  {"code":"FONT_SUBSTITUTED","detail":"embedded LiberationSans as substitute for /Helvetica","fixed":true}]}
```

Exit status is 0 on success, 1 when check mode found findings, 2 when the input was rejected, 3 on timeout and 64 on a usage error, so it drops into a shell pipeline like any other tool.

### npm package

```js
import { convert, check } from 'kura-pdf';

const { pdf, issues } = await convert(inputBytes, '2b');
const { compliant, findings } = await check(inputBytes, '2b');
```

The package is the engine compiled to WebAssembly, so it installs on any platform Node 22 runs on with no native binaries and no postinstall downloads. It also installs a `kura` command with the same flags and the same JSON report as the native CLI.

Errors are thrown as `KuraError` with a typed `code` and, for rejections that have one, a `suggestedLevel`.

### Browser (WebAssembly)

The `site/` directory is a complete browser build: the converter and the preflight tool, running the engine in a Web Worker. Files never leave the machine; load the page, disconnect, and it still works.

```js
import createKuraModule from './kura.js';

const kura = await createKuraModule();
const result = kura.convert(new Uint8Array(pdfBytes), '2b', { ua: true });
if (result.ok) save(result.pdf);
```

### Docker

```bash
docker run --rm -v "$PWD:/work" ghcr.io/alam00000/bentopdf-kura:latest --check --level 2b in.pdf
```

The image runs as an unprivileged user and processes files on your own machine.

### C API

`libkura` exposes a small, stable C ABI for embedding in any language with a foreign function interface:

```c
#include "kura/kura.h"

kura_options opt = {0};
kura_result* r = kura_convert(bytes, len, "2b", &opt);
if (r->ok) save(r->pdf, r->pdf_len);
else       report(r->error_code, r->error);
kura_result_free(r);
```

Four functions, no exceptions across the boundary, additive only within a major version.

---

## The standards

| Family | Levels | What conversion does |
|---|---|---|
| **PDF/A** archival | 1b, 1a, 2b, 2u, 2a, 3b, 3u, 3a, 4, 4f, 4e | fonts embedded, colour managed, metadata rebuilt, forbidden content removed or repaired; 3 and 4f keep attachments; 4e keeps 3D |
| **PDF/UA** accessible | UA-1 on parts 1 to 3, UA-2 on part 4 | structure tree, language, alternative text and reading order added on top of any PDF/A level with `--ua` |
| **PDF/X** print | X-1a, X-3, X-4, X-6 | output intent, page boxes, trapping declared; X-1a converts every colour to CMYK through ICC |
| **PDF/E** engineering | E-1 | 3D annotations kept, attachments kept, scripting removed |
| **PDF/VT** variable print | VT-1, VT-3 | X-4 conformance plus a document-part record hierarchy |
| **Check only** | X-4p, X-5g, X-5n, X-5pg, X-6n, X-6p, VT-2 | flavours that reference external press profiles: validated, not produced |

That is 25 targets. The [standards page](https://kura.bentopdf.com/docs/standards) explains what each one is for and which to pick.

---

## Preflight

Kura is as much a preflight engine as a conversion engine. A preflight profile is a set of checks to run against a document, optionally with repairs to apply, and 396 of them ship under `pdfa-engine/profiles/`, every one written by BentoPDF from public sources and generated from one script:

| Folder | Contents |
|---|---|
| `profiles/report` | report-only profiles: hairlines, small text, rich black, white objects, invisible text, images, spot colours, overprint, transparency, fonts, pages, ink and document health |
| `profiles/press`, `profiles/gwg` | check and check-and-fix profiles per print process, and the Ghent Workgroup 2022 workflows written from the published specification |
| `profiles/online`, `archive`, `accessibility`, `standards` | screen and download checks, and conformance checks for every PDF/A, X, E and VT flavour |
| `profiles/images`, `colour`, `objects`, `pages`, `document` | single-question profiles with the thresholds a shop actually asks for: resolution, ink, spot counts, hairlines, text sizes, boxes, versions, annotations, layers |
| `profiles/actions` | repair-only profiles: rotation, boxes and bleed, overprint and knockout, hairlines, stamps, layers, initial view |

Run a profile in check mode to get findings without writing a file, or with a conversion to have its repairs applied in the same pass:

```bash
kura --check --level 2b --profile pdfa-engine/profiles/report/report-hairlines.json in.pdf

kura --level x4 --profile pdfa-engine/profiles/press/sheetfed-offset-cmyk-check-and-fix.json in.pdf out.pdf
```

Findings come back under `analysis`, separate from the conformance `issues`, with the severity, the check, the hit count and every page it hit:

```json
{"code":"PROFILE_HIT","detail":"Error: Stroke thinner than 0.125 pt (11 hit(s), pages 1-8, 10-12)"}
```

Profiles are plain JSON, so you can write your own, and the XML preflight dialect many print shops already have is accepted as well. `--analyze` adds the engine's own document analysis (colour usage, transparency, overprint, fonts, page sizes) to any run. The browser preflight tool at [kura.bentopdf.com/preflight.html](https://kura.bentopdf.com/preflight.html) runs a curated set of profiles on a dropped file and can apply the repairs. The [preflight page](https://kura.bentopdf.com/docs/preflight) documents the profile format.

---

## What the engine does

Depending on the document and the target, the engine can:

* embed metric-compatible substitutes for fonts that are not embedded, and repair width tables, encodings and Unicode maps on fonts that are
* convert RGB, Lab and calibrated colour to CMYK through Little CMS for PDF/X-1a
* add or repair output intents, default colour spaces and colourant names
* detect transparency anywhere in the document, including inside patterns and form XObjects, and flatten it by rasterizing only the pages that need it
* rebuild XMP metadata from the document information dictionary, exactly consistent
* strip or repair encryption, scripting, actions, annotations and embedded files according to what each part permits
* build a structure tree from scratch for PDF/UA, or repair one that exists
* embed an e-invoice, derive its file name, relationship and XMP schema from the guideline it declares, and pull it back out later
* lay an invisible OCR text layer over scanned pages so they become searchable
* apply a PKCS#7 signature in the same pass, so the signed file is still conformant
* run preflight profiles: 396 bundled checks and repairs, or your own in JSON or the XML dialect
* wrap a bare JPEG as a single-page PDF and convert that

---

## What the engine refuses to do

* **Claim conformance it did not achieve.** A conversion that cannot make the document conform is refused with an error code and, where one exists, the level that would work. Check mode reports `SCAN_INCOMPLETE` when a pass could not finish rather than staying quiet.
* **Rasterize when it can avoid it.** Flattening is the last resort for a page, never the first, and never without a line in the report.
* **Decrypt what it has no key for.** Standard encryption is removed automatically; user passwords are accepted; files bound to external key material are reported as such, not silently emptied.
* **Guess an invoice profile.** An e-invoice that declares no guideline is rejected rather than embedded as a guess.

The full list of rejection codes and what to do about each is in the [documentation](https://kura.bentopdf.com/docs/rejections).

---

## Building from source

```bash
cmake -S pdfa-engine -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Needs a C++17 compiler, CMake, qpdf 12, FreeType, OpenJPEG, libjpeg, libpng, zlib and Little CMS. PDFium is optional, built from source once, and detected automatically. The WebAssembly module builds with `pdfa-engine/scripts/build-wasm.sh`.

See [BUILDING.md](https://github.com/alam00000/bentopdf-kura/blob/main/BUILDING.md) for every platform and every option.

---

## Project layout

```
pdfa-engine/core      the engine: buffer in, buffer out, one file per conformance pass
pdfa-engine/cli       the kura command
pdfa-engine/capi      libkura, the C library
pdfa-engine/wasm      the WebAssembly bindings
pdfa-engine/raster    the PDFium rasterizer
pdfa-engine/profiles  396 preflight profiles and the script that generates them
pdfa-engine/fuzz      five fuzz harnesses and their seeds
packages/npm/kura-pdf the npm package
site                  the browser demo
docs                  the documentation site
```

---

## Licensing

Kura is licensed under the [GNU AGPL v3](https://github.com/alam00000/bentopdf-kura/blob/main/LICENSE).

If you need to use Kura in a proprietary or closed-source product, a commercial license is available. Contact us at [contact@bentopdf.com](mailto:contact@bentopdf.com).

Bundled third-party components retain their own licenses. See [NOTICE.md](https://github.com/alam00000/bentopdf-kura/blob/main/NOTICE.md) for the full list.

---

## Contributing

Contributions are welcome.

Before opening your first pull request, there are two things to know:

1. Contributors need to sign the [Contributor License Agreement](https://github.com/alam00000/bentopdf-kura/blob/main/ICLA.md). The bot will prompt you automatically.
2. Bug fixes should include the PDF that reproduces the issue.

See [CONTRIBUTING.md](https://github.com/alam00000/bentopdf-kura/blob/main/CONTRIBUTING.md) for the full contribution guide.

For security vulnerabilities, please see [SECURITY.md](https://github.com/alam00000/bentopdf-kura/blob/main/SECURITY.md).
