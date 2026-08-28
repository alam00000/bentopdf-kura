# Benchmark

This document records a robustness benchmark of the Kura engine (BentoPDF Kura Engine (kura) 1.1.0) on 30,677 real PDFs from public corpora, on Darwin arm64, Apple M4. Every file was converted to PDF/A-2b with the CLI exactly as shipped, one process per file, with the default 120-second watchdog. The raw per-file data is in [bench/RESULTS.csv](bench/RESULTS.csv); every document's origin and SHA-256 are in [bench/MANIFEST.csv](bench/MANIFEST.csv), so the run can be reproduced with `make bench`.

## What is measured

A conversion engine that takes untrusted files has two ways to fail badly: it can crash or hang, and it can write a file that claims a standard it does not meet. So each run is graded on four outcomes, in order of severity:

- **crash**: the process died or produced no report. This is the number that matters most, and the target is zero.
- **timeout**: the watchdog fired. A hang is a crash that has not happened yet.
- **rejected**: the engine refused with an error code and a reason. A password-protected or unparseable file is supposed to land here; it is not a failure of the engine, and the code says why.
- **converted**: a conforming file was written. A fixed one-in-ten sample of these outputs is validated independently with veraPDF, the reference validator for the standard, and the pass rate is reported.

Conversion is the hardest path through the engine: it parses, repairs, embeds, converts colour, rebuilds metadata and serializes, so it exercises far more code than a check does.

## Corpora

| tranche | source | files | what it stresses |
|---|---|---:|---|
| pdfjs | The pdf.js regression corpus | 966 | the test files the pdf.js viewer project has collected from bug reports: broken cross-reference tables, exotic fonts, damaged streams |
| cc | Web PDFs, Common Crawl CC-MAIN-2021-31 | 1,927 | PDFs as they sit on the open web, fetched straight from the July 2021 crawl archive: every producer, every era, every state of repair |
| safedocs | Web PDFs, DARPA SafeDocs CC-MAIN-2021-31 | 27,784 | a sample of the 7.9 million PDFs crawled from the open web in 2021 and archived by DARPA SafeDocs, in every state of repair |

The web sample is pre-registered: the Common Crawl CC-MAIN-2021-31 index was read directly from its published shards, taking index blocks at a fixed stride under ten top-level domain prefixes (gov, edu, org, com, uk, de, fr, jp, au, ca), every record in those blocks served as PDF with HTTP status 200 and an archive record of at most 512 KB, in index order until each prefix had its share, and nothing was excluded afterwards. Each file's archive locator is in MANIFEST.csv, so the exact bytes can be fetched again. The SafeDocs sample is pre-registered: zip files `0000` to `0999` of the corpus were taken at a fixed stride of 20, and inside each chosen zip every PDF member of at most 512 KB was taken in directory order until the target was reached; nothing was excluded afterwards. Each file's origin names the zip and the member, so the exact bytes can be fetched again. The pdf.js files are the complete set of PDFs checked into that project's test directory, nothing excluded.

## Results

| tranche | files | converted | rejected | timeouts | crashes | verified sample | median time | p95 time |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| pdfjs | 966 | 938 | 28 | 0 | **0** | 86/94 | 0.02 s | 0.1 s |
| cc | 1,927 | 1,922 | 5 | 0 | **0** | 189/193 | 0.06 s | 0.2 s |
| safedocs | 27,784 | 27,576 | 208 | 0 | **0** | 2,760/2,779 | 0.05 s | 0.1 s |
| **all** | 30,677 | 30,436 | 241 | 0 | **0** | 3,035/3,066 | 0.05 s | 0.1 s |

### Why files were rejected

| code | meaning | files |
|---|---|---:|
| `FONT_UNEMBEDDABLE` | a font whose licence forbids embedding, so no conforming file can be written | 139 |
| `PARSE_ERROR` | not a parseable PDF | 72 |
| `PASSWORD_REQUIRED` | needs a password | 30 |

### Verified sample: files that did not pass veraPDF

| tranche | file |
|---|---|
| pdfjs | Type3WordSpacing.pdf |
| pdfjs | calrgb.pdf |
| pdfjs | issue14881.pdf |
| pdfjs | issue215.pdf |
| pdfjs | issue5334.pdf |
| pdfjs | issue6894.pdf |
| pdfjs | issue9262_reduced.pdf |
| pdfjs | text_clip_cff_cid.pdf |
| cc | cc-00190-3vgmig42by.pdf |
| cc | cc-00350-knsowcrdt4.pdf |
| cc | cc-01554-w4lwjdh6oy.pdf |
| cc | cc-01645-vjp5mj3f7v.pdf |
| safedocs | 0000-0000626.pdf |
| safedocs | 0020-0020305.pdf |
| safedocs | 0020-0020810.pdf |
| safedocs | 0100-0100813.pdf |
| safedocs | 0140-0140581.pdf |
| safedocs | 0220-0220947.pdf |
| safedocs | 0280-0280207.pdf |
| safedocs | 0280-0280830.pdf |
| safedocs | 0320-0320533.pdf |
| safedocs | 0360-0360000.pdf |
| safedocs | 0360-0360414.pdf |
| safedocs | 0380-0380235.pdf |
| safedocs | 0460-0460932.pdf |
| safedocs | 0520-0520046.pdf |
| safedocs | 0640-0640324.pdf |
| safedocs | 0640-0640892.pdf |
| safedocs | 0680-0680518.pdf |
| safedocs | 0720-0720465.pdf |

## Appendix: every document tested

The per-file lists are split into parts of up to 2,000 rows so they stay readable on GitHub. Each row gives the file, its page count, its size, the outcome and where it came from.

- [The pdf.js regression corpus, part 1 of 1](bench/documents/pdfjs-01.md) (966 files)
- [Web PDFs, Common Crawl CC-MAIN-2021-31, part 1 of 1](bench/documents/cc-01.md) (1,927 files)
- [Web PDFs, DARPA SafeDocs CC-MAIN-2021-31, part 1 of 14](bench/documents/safedocs-01.md) (2,000 files)
- [Web PDFs, DARPA SafeDocs CC-MAIN-2021-31, part 2 of 14](bench/documents/safedocs-02.md) (2,000 files)
- [Web PDFs, DARPA SafeDocs CC-MAIN-2021-31, part 3 of 14](bench/documents/safedocs-03.md) (2,000 files)
- [Web PDFs, DARPA SafeDocs CC-MAIN-2021-31, part 4 of 14](bench/documents/safedocs-04.md) (2,000 files)
- [Web PDFs, DARPA SafeDocs CC-MAIN-2021-31, part 5 of 14](bench/documents/safedocs-05.md) (2,000 files)
- [Web PDFs, DARPA SafeDocs CC-MAIN-2021-31, part 6 of 14](bench/documents/safedocs-06.md) (2,000 files)
- [Web PDFs, DARPA SafeDocs CC-MAIN-2021-31, part 7 of 14](bench/documents/safedocs-07.md) (2,000 files)
- [Web PDFs, DARPA SafeDocs CC-MAIN-2021-31, part 8 of 14](bench/documents/safedocs-08.md) (2,000 files)
- [Web PDFs, DARPA SafeDocs CC-MAIN-2021-31, part 9 of 14](bench/documents/safedocs-09.md) (2,000 files)
- [Web PDFs, DARPA SafeDocs CC-MAIN-2021-31, part 10 of 14](bench/documents/safedocs-10.md) (2,000 files)
- [Web PDFs, DARPA SafeDocs CC-MAIN-2021-31, part 11 of 14](bench/documents/safedocs-11.md) (2,000 files)
- [Web PDFs, DARPA SafeDocs CC-MAIN-2021-31, part 12 of 14](bench/documents/safedocs-12.md) (2,000 files)
- [Web PDFs, DARPA SafeDocs CC-MAIN-2021-31, part 13 of 14](bench/documents/safedocs-13.md) (2,000 files)
- [Web PDFs, DARPA SafeDocs CC-MAIN-2021-31, part 14 of 14](bench/documents/safedocs-14.md) (1,784 files)
