# Conformance corpus

The files behind the published conformance results, and the scripts that produce them.

## The corpus

4,860 PDFs from six public sources, chosen by a fixed seed (`20261107`) before any file was fetched:

| Tranche | Files | Source | What it adds |
|---|---:|---|---|
| `web` | 2,000 | [Common Crawl PDFs](https://digitalcorpora.org/cc-main-2021-31-pdf-untruncated/) (Digital Corpora, SafeDocs) | web PDFs in many languages from many producers |
| `govdocs` | 1,000 | [Govdocs1](https://digitalcorpora.org/corpora/files/) (Digital Corpora) | US government scans, forms and office documents |
| `pmc` | 500 | [PubMed Central open access](https://registry.opendata.aws/ncbi-pmc/) (AWS Open Data) | publisher-made journal articles |
| `arxiv` | 500 | [arXiv](https://arxiv.org) | LaTeX, mathematics and figures |
| `eurlex` | 160 | [EUR-Lex](https://eur-lex.europa.eu) | EU law in the 24 official languages (EUR-Lex began refusing automated downloads after 160 files) |
| `testsuites` | 700 | pdf.js, veraPDF corpus and PDFium test files | deliberately difficult and damaged files |

`MANIFEST.csv` lists every file with its SHA-256, size and the exact URL it came from (a zip member is written `zip-url#member=name`).

## Reproducing

```bash
python3 conformance/corpus.py web        # one command per tranche; they can run in parallel
python3 conformance/corpus.py govdocs
python3 conformance/corpus.py pmc
python3 conformance/corpus.py arxiv
python3 conformance/corpus.py eurlex
python3 conformance/corpus.py testsuites
python3 conformance/corpus.py merge      # writes MANIFEST.csv
python3 conformance/run.py               # converts and judges, writes RESULTS.csv
```

The same seed picks the same candidates. Sources change over time (a PDF can be withdrawn), so check the downloaded files against the SHA-256 values in `MANIFEST.csv`; the published results apply to exactly those files.

`run.py` converts every file to every level with Kura and has each output judged by an independent checker:

| Levels | Judge |
|---|---|
| PDF/A-1a, 1b, 2a, 2b, 2u, 3a, 3b, 3u, 4, 4e, 4f; PDF/UA-1, UA-2 | [veraPDF](https://verapdf.org) |
| PDF/X-1a, X-3, X-4, X-6; PDF/E-1; PDF/VT-1, VT-3 | callas pdfToolbox, "Verify compliance with …" profiles (set `CALLAS_PDFTOOLBOX` to its CLI) |

`RESULTS.csv` has one row per file and level: whether Kura converted or rejected the file (with its error code), the judge, the verdict and the number of errors found.

Handle the files with care: the web and test-suite tranches contain malformed and possibly hostile PDFs.
