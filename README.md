<p align="center"><img src="docs/public/images/logo.svg" width="80"></p>

<h1 align="center">Kura</h1>

<p align="center">
  <strong>The open source PDF standards and preflight engine.</strong>
</p>

Kura converts everyday PDFs into archival, print and accessible ones: every PDF/A level, PDF/UA, PDF/X, PDF/E and PDF/VT, from one engine. It repairs what is wrong, embeds what is missing, converts colour where the standard demands it, and writes a file that validates clean, or tells you exactly why it cannot.

It is also a full print preflight engine: 685 bundled profiles check and repair what a press cares about, such as hairlines, rich black, low-resolution images, overprint, transparency, page boxes, fonts and colour, and you can write your own in JSON from a published catalogue of checks and fixes.

The name comes from the 蔵 (*kura*), the traditional Japanese storehouse where a family kept the things meant to outlast them.

Kura is made by the [BentoPDF](https://github.com/alam00000/bentopdf) team. The same engine is available as a CLI, a C library, an npm package, a Docker image and a WebAssembly build that runs entirely in the browser.

Try the converter and preflight at [kura.bentopdf.com](https://kura.bentopdf.com). Full documentation lives at [kura.bentopdf.com/docs](https://kura.bentopdf.com/docs/).

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](https://github.com/alam00000/bentopdf-kura/blob/main/LICENSE) ![GitHub Stars](https://img.shields.io/github/stars/alam00000/bentopdf-kura?style=social)

---

## Table of Contents

* [Why Kura](#why-kura)
* [Does it actually work?](#does-it-actually-work)
* [Benchmark](#benchmark)
* [Install](#install)
* [Usage](#usage)

  * [CLI](#cli)
  * [npm package](#npm-package)
  * [Browser (WebAssembly)](#browser-webassembly)
  * [Self-hosted service](#self-hosted-service)
  * [C API](#c-api)
* [The standards](#the-standards)
* [Preflight](#preflight)
* [What the engine does](#what-the-engine-does)
* [Building from source](#building-from-source)
* [Licensing](#licensing)
* [Contributing](#contributing)

---

## Why Kura

Making a PDF conform to a standard has three problems: **tools that quietly produce a file which only claims to conform, tools that destroy content to get there, and tools that cover one standard and leave you to find another for the next.**

Kura is built around solving those three.

* **Honest output.** Every conversion is checked. When the engine cannot make a document conform, it refuses with a reason and, where one exists, the level that would work. It never writes a file that claims a standard it does not meet, and its check mode gives the same verdict as veraPDF on every file of veraPDF's test corpus and on a test file for every one of veraPDF's PDF/A and PDF/UA rules.

* **Content preserving.** Text stays searchable, attachments stay attached, signatures are re-applied in the same pass, transparency and overprint are flattened as vectors, and pages are rasterized only when a rule leaves no other way, never silently.

* **Fonts for every script.** Unembedded fonts are replaced by bundled open-licence fonts covering Latin, Greek, Cyrillic, Arabic, Hebrew, the Indic scripts, Thai, Chinese, Japanese, Korean and about 50 other scripts, shaped with HarfBuzz, so the converted page still reads the same.

* **Every standard, one engine.** All eleven PDF/A levels, PDF/UA-1 and UA-2, PDF/X-1a through X-6, PDF/E-1 and PDF/VT, plus Factur-X, ZUGFeRD, XRechnung and Order-X e-invoices, with the same options and the same report everywhere.

* **Preflight built in.** The same engine runs print preflight: 685 bundled profiles for hairlines, rich black, image resolution, overprint, transparency, page boxes, fonts and colour, each a set of checks with repairs where a repair exists, plus your own in JSON.

* **Runs anywhere.** The same engine powers the CLI, the C library, the npm package, the Docker image and the browser build, which runs entirely on your machine with nothing to install.

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

Real-world documents: 4,860 public PDFs (web, government, journals, arXiv, EU law, test suites) were converted to all 20 levels and every output was checked by an independent validator, veraPDF for PDF/A and PDF/UA and callas pdfToolbox for PDF/X, PDF/E and PDF/VT. 95 to 98% pass at every PDF/A, PDF/UA-1, PDF/X, PDF/E and PDF/VT level; the rest are mostly damaged or password-locked files, which Kura refuses with a reason. Every file, every number and the scripts to reproduce them are on [Conformance](https://kura.bentopdf.com/docs/conformance).

Check mode gives the same verdict as veraPDF on all 2,694 files of veraPDF's test corpus, and on a failing and a passing test file for every veraPDF rule at every PDF/A level and at PDF/UA-1 and UA-2.

For robustness, see the [benchmark](#benchmark): real-world files, every one listed, zero crashes. It is fuzzed continuously by ClusterFuzzLite in this repository's own CI through five harnesses, built and tested under AddressSanitizer and UndefinedBehaviorSanitizer on every push, and refuses pathological files with an error instead of falling over.

> [!NOTE]
> Check mode is tested rule by rule against veraPDF, but it is still Kura's own checker. For a formal acceptance decision, confirm with an independent validator.

---

## Benchmark

Two large runs, measuring different things:

| Run | Files | Levels | What it measures |
|---|---:|---|---|
| [Conformance](https://kura.bentopdf.com/docs/conformance) | 4,860 | all 20 convertible levels | is the output valid? every output checked by veraPDF or callas pdfToolbox |
| Robustness, below | 30,677 | PDF/A-2b | does the engine crash, hang or slow down on real files? |

**Robustness.** Kura was run over **30,677 real PDFs**: 966 files from the pdf.js regression corpus, 1,927 web PDFs fetched straight from the Common Crawl archive, 27,784 web PDFs from the DARPA SafeDocs archive of the 2021 crawl. Every file was converted to PDF/A-2b with the CLI exactly as shipped, one process per file, under the default 120-second watchdog, and a fixed one-in-ten sample of the outputs was validated independently with veraPDF. Every document is listed with its origin and SHA-256, so the run can be reproduced.

| tranche | files | converted | rejected with a reason | timeouts | crashes | veraPDF sample | median | p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| pdfjs | 966 | 938 | 28 | 0 | **0** | 86/94 pass | 0.02 s | 0.1 s |
| cc | 1,927 | 1,922 | 5 | 0 | **0** | 189/193 pass | 0.06 s | 0.2 s |
| safedocs | 27,784 | 27,576 | 208 | 0 | **0** | 2,760/2,779 pass | 0.05 s | 0.1 s |
| **all** | 30,677 | 30,436 | 241 | 0 | **0** | 3,035/3,066 pass | 0.05 s | 0.1 s |

Rejections are the engine saying no with a reason (139 carrying a font whose licence forbids embedding, 72 not parseable as PDF, 30 needing a password); a password-protected, unparseable or licence-locked file is supposed to land there. The 30 sampled outputs veraPDF does not accept were re-run one by one: 26 fail font rules (missing glyphs, glyph widths, use of .notdef, encodings), 6 fail colour-space rules and 2 contain an operator no PDF version defines, all in fonts and streams that arrived broken, and Kura's own check passes 27 of the 30. Those are checker blind spots as much as converter ones; they are listed by file in the report and are open engine work. One further sampled output could not be validated because veraPDF itself errored on it.

The full methodology, every document with its origin and SHA-256, and the per-file outcomes are in [benchmark.md](benchmark.md). The run is reproducible with `make bench`; `TARGET` sets how many web PDFs to draw.

---

## Install

Prebuilt binaries for Linux, Windows and macOS are attached to each [release](https://github.com/alam00000/bentopdf-kura/releases), with a `SHA256SUMS` file.

```bash
npm install -g kura-pdf          # the native engine for Node.js, plus the kura command (kura-pdf-wasm for everything else)
```

```bash
docker run -d -p 8080:8080 ghcr.io/alam00000/bentopdf-kura:latest   # web interface + HTTP API on your own server
```

To build the engine yourself, see [BUILDING.md](https://github.com/alam00000/bentopdf-kura/blob/main/BUILDING.md).

---

## Usage

### CLI

```bash
kura --level 2b in.pdf out.pdf                     # PDF/A-2b, the usual archival choice

kura --level 2b in.pdf                             # same, written next to the input as in.2b.pdf

kura --level 2a --ua in.pdf out.pdf                # add PDF/UA accessibility

kura --level x1a in.pdf out.pdf                    # PDF/X-1a, real ICC-based RGB to CMYK

kura --check --level 2b in.pdf                     # what would have to change? writes nothing

kura --check --level 2b --profile hairlines.json in.pdf   # print preflight: findings, writes nothing

kura --einvoice invoice.xml in.pdf out.pdf         # Factur-X / ZUGFeRD / XRechnung hybrid

kura --level 2b -r -d out/ inbox/                  # a whole folder, JSON array on stdout

kura --level x4 --convert-colors "Coated FOGRA39 (ISO 12647-2:2004)" in.pdf out.pdf   # CMYK for a named press profile

kura --level none --convert-colors gray in.pdf out.pdf   # colour conversion only, no standard

kura tool info in.pdf                              # one-shot jobs: info, plates, separations, certify, …

kura --help                                        # every flag
```

Every run prints a JSON report of what changed:

```json
{"file":"in.pdf","output":"out.pdf","ok":true,"level":"2b","engine":"BentoPDF Kura Engine 1.3.0","issues":[
  {"code":"OUTPUT_INTENT_ADDED","detail":"added sRGB PDF/A output intent","fixed":true,"severity":3,"pages":[]},
  {"code":"FONT_SUBSTITUTED","detail":"embedded LiberationSans as substitute for /Helvetica","fixed":true,"severity":3,"pages":[1]}]}
```

Exit status is 0 on success, 1 when check mode found findings, 2 when the input was rejected, 3 on timeout and 64 on a usage error, so it drops into a shell pipeline like any other tool. Every flag, tool and environment variable is on the [CLI page](https://kura.bentopdf.com/docs/cli); every code in a report is explained under [Issue codes](https://kura.bentopdf.com/docs/issue-codes) and [Rejection codes](https://kura.bentopdf.com/docs/rejections).

### npm package

```js
import { convert, check } from 'kura-pdf';

const result = await convert(pdfBytes, '2b', { ua: true, lang: 'en-US' });
const report = await check(result.pdf, '2b');
```

`kura-pdf` installs the native engine through a platform package for Linux x64, macOS arm64 or Windows x64 and runs it in a subprocess; every native feature is there, signing and OCR included. `kura-pdf-wasm` is the same API on the WebAssembly build for every other platform and for browsers. See the [npm packages](https://kura.bentopdf.com/docs/npm) page.

### Browser (WebAssembly)

The `site/` directory is a complete browser build: the converter and the preflight tool, running the engine in a Web Worker. Files never leave the machine; load the page, disconnect, and it still works.

```js
import createKuraModule from './kura.js';

const kura = await createKuraModule();
const result = kura.convert(new Uint8Array(pdfBytes), '2b', { ua: true });
if (result.ok) save(result.pdf);
```

### Self-hosted service

The Docker image includes the web interface (converter and preflight) at `http://localhost:8080` and an HTTP API, both backed by the native engine.

Your files are processed on your own server.

```bash
docker run -d -p 8080:8080 ghcr.io/alam00000/bentopdf-kura:latest
curl -o out.pdf --data-binary @in.pdf 'http://localhost:8080/api/convert?level=2b'
```

The same image doubles as the CLI: pass arguments and it converts instead of serving.

```bash
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" ghcr.io/alam00000/bentopdf-kura:latest --check --level 2b in.pdf
```

For more volume, the same image runs as a coordinator with any number of worker machines: a queue that survives restarts, automatic retries when a worker drops out, and watched folders.

See [SELF-HOSTING.md](https://github.com/alam00000/bentopdf-kura/blob/main/SELF-HOSTING.md) for the complete API, configuration options, and deployment notes.

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

Seven functions, no exceptions across the boundary, additive only within a major version. Results carry every finding with a severity and the pages it concerns; the options reach preflight profiles, the analysis census, font folders, OCR words, image limits and rasterization.

---

## The standards

| Family | Levels | What conversion does |
|---|---|---|
| **PDF/A** archival | 1b, 1a, 2b, 2u, 2a, 3b, 3u, 3a, 4, 4f, 4e | fonts embedded, colour managed, metadata rebuilt, forbidden content removed or repaired; 3 and 4f keep attachments; 4e keeps 3D |
| **PDF/UA** accessible | UA-1 on parts 1 to 3, UA-2 on part 4 | structure tree, language, alternative text and reading order added on top of any PDF/A level with `--ua` |
| **PDF/X** print | X-1a, X-3, X-4, X-6 | output intent, page boxes, trapping declared; X-1a converts every colour to CMYK through ICC |
| **PDF/E** engineering | E-1 | 3D annotations kept, attachments kept, scripting removed |
| **PDF/VT** variable print | VT-1, VT-3 | X-4 conformance plus a document-part record hierarchy |
| **Check only** | X-4p, X-5g, X-5n, X-5pg, X-6n, X-6p, VT-2 | flavours that reference external press profiles: validated, not produced (X-5n is produced when you give the press profile and its address) |
| **Check only** | UA-1, UA-2 (`--level ua1`, `ua2`) | PDF/UA on its own, without the PDF/A rules |

That is 25 targets. The [standards page](https://kura.bentopdf.com/docs/standards) explains what each one is for and which to pick.

---

## Preflight

Kura is as much a preflight engine as a conversion engine. A preflight profile is a set of checks to run against a document, optionally with repairs to apply, and 685 of them ship under `pdfa-engine/profiles/`, every one written by BentoPDF from public sources and generated from one script:

| Folder | Contents |
|---|---|
| `report` | report-only profiles: hairlines, small text, rich black, white objects, invisible text, images, spot colours, overprint, transparency, fonts, pages, ink and document health |
| `press`, `gwg`, `packaging` | check and check-and-fix profiles per print process, the Ghent Workgroup 2022 workflows written from the published specification, and packaging by process and product |
| `colour-conversion`, `print-conditions` | convert all colour to a named press condition, CMYK to CMYK between conditions, grey or sRGB, ink limits, neutral greys to black, tone adjustments |
| `online`, `archive`, `accessibility`, `standards`, `compatibility` | screen and download checks, conformance checks for every PDF/A, X, E and VT flavour, and compatibility fixes |
| `images`, `colour`, `objects`, `pages`, `document` | single-question profiles with the thresholds a shop actually asks for: resolution, ink, spot counts, hairlines, text sizes, boxes, versions, annotations, layers |
| `actions`, `place-content`, `plans` | repair-only profiles (rotation, boxes and bleed, overprint and knockout, flattening, layers, initial view), placing stamps, text and barcodes, and multi-step plans |

Run a profile in check mode to get findings without writing a file, or with a conversion to have its repairs applied in the same pass:

```bash
kura --check --profile pdfa-engine/profiles/report/report-hairlines.json in.pdf

kura --level x4 --profile pdfa-engine/profiles/press/sheetfed-offset-cmyk-check-and-fix.json in.pdf out.pdf
```

Findings come back under `analysis`, separate from the conformance `issues`, with the severity, the check, the hit count and every page it hit:

```json
{"code":"CHECK_HIT","detail":"Error: Stroke thinner than 0.125 pt (11 hit(s), pages 1-8, 10-12)"}
```

Profiles are plain JSON, so you can write your own from the [catalogue of checks and fixes](https://kura.bentopdf.com/docs/profile-checks):

```json
{"kura-profile":1,"name":"Hairlines","checks":[{"name":"Stroke thinner than 0.125 pt","severity":"error","all":[{"check":"content.stroke_width","op":"<=","value":0.125}]}],"fixes":[{"fix":"ink.min_line_width","width":"0.25pt"}]}
```

`--analyze` adds the engine's own document analysis (colour usage, transparency, overprint, fonts, page sizes) to any run. The browser preflight tool at [kura.bentopdf.com](https://kura.bentopdf.com) (Preflight in the top bar) runs a curated set of profiles on a dropped file and can apply the repairs. The [preflight page](https://kura.bentopdf.com/docs/preflight) documents the profile format.

---

## What the engine does

Depending on the document and the target, the engine can:

* embed metric-compatible substitutes for fonts that are not embedded, including Chinese, Japanese, Korean and about 50 other scripts, and repair width tables, encodings and Unicode maps on fonts that are
* convert colour through ICC profiles with Little CMS at any level: to CMYK for print, to grey or sRGB, with ink limits, black preservation and spot handling
* use any ICC profile you name: it is looked up in your `--icc-folder`, the computer's colour folders (Windows, macOS, Linux, and Adobe's folders if Adobe software is installed) and Kura's own profiles; a name that is not found is refused, never silently swapped
* flatten overprint so a file looks the same on a press as on screen
* add or repair output intents, default colour spaces and colourant names
* detect transparency anywhere in the document, including inside patterns and form XObjects, and flatten it as vectors, rasterizing only regions that cannot be expressed otherwise
* rebuild XMP metadata from the document information dictionary, exactly consistent
* strip or repair encryption, scripting, actions, annotations and embedded files according to what each part permits
* build a structure tree from scratch for PDF/UA, or repair one that exists
* embed an e-invoice, derive its file name, relationship and XMP schema from the guideline it declares, and pull it back out later
* lay an invisible OCR text layer over scanned pages so they become searchable
* apply a PKCS#7 signature in the same pass, so the signed file is still conformant
* run preflight profiles: 685 bundled checks and repairs, or your own in JSON
* wrap a bare JPEG as a single-page PDF and convert that

---

## Building from source

```bash
cmake -S pdfa-engine -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Needs a C++17 compiler, CMake, qpdf 12, FreeType, OpenJPEG, libjpeg, libpng, zlib and Little CMS. PDFium is optional, built from source once, and detected automatically. The WebAssembly module builds with `pdfa-engine/scripts/build-wasm.sh`.

See [BUILDING.md](https://github.com/alam00000/bentopdf-kura/blob/main/BUILDING.md) for every platform and every option.

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
