# Conformance

This page records how Kura's output is judged against each standard, by whom, and on which files. Every number can be reproduced with the files and scripts in [`conformance/`](https://github.com/alam00000/bentopdf-kura/tree/main/conformance).

## Who judges

Kura does not grade its own output. Every converted file is checked by an independent validator:

| Standard | Independent judge |
|---|---|
| PDF/A-1a to 4f (11 levels), PDF/UA-1, PDF/UA-2 | [veraPDF](https://verapdf.org) 1.28, the reference PDF/A and PDF/UA validator |
| PDF/X-1a, X-3, X-4, X-6, PDF/E-1, PDF/VT-1, VT-3 | callas pdfToolbox 17, its "Verify compliance with …" profiles |

## Real-world documents, every level

4,860 PDFs from six public sources, picked by a fixed seed before any file was fetched: Common Crawl web PDFs, Govdocs1, PubMed Central, arXiv, EUR-Lex, and the pdf.js, veraPDF and PDFium test suites. The file list with SHA-256 checksums and source URLs is [`conformance/MANIFEST.csv`](https://github.com/alam00000/bentopdf-kura/blob/main/conformance/MANIFEST.csv).

Each file was converted to every level and each output was given to the judge above. Measured on 8 November 2026.

| Level | Judge | Passed | Pass rate | Rejected by Kura |
|---|---|---:|---:|---:|
| PDF/A-1a | veraPDF | 4,710 | 96.9% | 58 |
| PDF/A-1b | veraPDF | 4,743 | 97.6% | 58 |
| PDF/A-2a | veraPDF | 4,752 | 97.8% | 17 |
| PDF/A-2b | veraPDF | 4,782 | 98.4% | 17 |
| PDF/A-2u | veraPDF | 4,774 | 98.2% | 17 |
| PDF/A-3a | veraPDF | 4,736 | 97.4% | 17 |
| PDF/A-3b | veraPDF | 4,766 | 98.1% | 17 |
| PDF/A-3u | veraPDF | 4,758 | 97.9% | 17 |
| PDF/A-4 | veraPDF | 4,781 | 98.4% | 17 |
| PDF/A-4e | veraPDF | 4,780 | 98.4% | 17 |
| PDF/A-4f | veraPDF | 4,780 | 98.4% | 17 |
| PDF/UA-1 | veraPDF | 4,681 | 96.3% | 17 |
| PDF/UA-2 | veraPDF | 4,065 | 83.6% | 17 |
| PDF/X-1a | callas | 4,761 | 98.0% | 43 |
| PDF/X-3 | callas | 4,755 | 97.8% | 33 |
| PDF/X-4 | callas | 4,734 | 97.4% | 17 |
| PDF/X-6 | callas | 4,714 | 97.0% | 17 |
| PDF/E-1 | callas | 4,633 | 95.3% | 17 |
| PDF/VT-1 | callas | 4,734 | 97.4% | 17 |
| PDF/VT-3 | callas | 4,700 | 96.7% | 17 |

"Rejected" means Kura refused the file with an error code instead of writing output that would not conform: damaged files, files locked with a password, and, at PDF/A-1, PDF/X-1a and PDF/X-3, content those older levels cannot express. See [Rejection codes](/rejections). Fixes made after this run, most of them for PDF/UA-2, are not yet reflected in these numbers.

## Test suites

| Suite | Target | Result |
|---|---|---|
| veraPDF corpus | PDF/A-1b, 2b, 4 | 568 / 568, 985 / 985, 486 / 486 |
| Isartor | PDF/A-1b | 204 / 204 |
| BFO | PDF/A-1b, 2b | 34 / 34 |
| PDF/UA Reference Suite | PDF/UA-1 | 10 / 10 |
| Ghent Output Suite 5.0 | PDF/X-4 | 69 / 69 |

## Check mode

`kura --check` validates a file without changing it. Its verdicts were compared with the judges on files built to test one rule each:

| Comparison | Files | Same verdict as the judge |
|---|---:|---:|
| veraPDF's full test corpus, every folder | 2,694 | 100% |
| A failing and a passing file for every veraPDF PDF/A rule, 11 levels | 3,493 | 100% |
| A failing and a passing file for every veraPDF PDF/UA-1 and UA-2 rule, including the ISO 32005 structure table | 1,925 | 100% |

For PDF/X, PDF/E and PDF/VT, Kura's checks were compared with callas on about 1,900 rule-by-rule test files. Where the two still differ, Kura follows the ISO standard; known cases are callas reporting a signature error when no signature field exists, treating `GTS_Encapsulated false` as true, and an X-6p profile that no file can satisfy.

## Reproducing

```bash
python3 conformance/corpus.py web          # one command per tranche
python3 conformance/corpus.py merge        # writes MANIFEST.csv
CALLAS_PDFTOOLBOX=/path/to/pdfToolbox python3 conformance/run.py
```

`run.py` writes one row per file and level to `RESULTS.csv`: whether Kura converted or rejected the file, the judge, its verdict and the number of errors it found.
