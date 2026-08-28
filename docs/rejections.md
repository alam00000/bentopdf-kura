# Rejection codes

When Kura cannot produce the output you asked for, the report carries `ok: false`, an `errorCode`, a human-readable `error`, and sometimes a `suggestedLevel`. This page lists every code, what it means, and what to do.

Everything not listed here converts, with the changes recorded as issues in the report.

## Input

| Code | Meaning | What to do |
|---|---|---|
| `PARSE_ERROR` | the bytes are not a PDF, or are damaged beyond qpdf's recovery | check the file; if it opens elsewhere, please report it with the file |
| `BAD_INPUT` | no input buffer was supplied | API misuse |
| `BAD_LEVEL` | the level string is not one Kura knows | see [The standards](/standards) |
| `IMAGE_INPUT_WRAPPED` | not an error: the input was a JPEG and was wrapped as a one-page PDF | nothing; reported as an issue |

## Encryption

| Code | Meaning | What to do |
|---|---|---|
| `PASSWORD_REQUIRED` | the document has a user password and none, or the wrong one, was supplied | pass `--password` |
| `ENCRYPTED_ADEPT` | the document is bound to Adobe ADEPT digital rights management; the content key is not in the file | no tool can decode this without the owner's key material |
| `ENCRYPTED_UNSUPPORTED` | a non-standard security handler that needs external key material | as above |

Standard encryption, RC4 and AES with an empty or supplied password, is removed automatically and is not a rejection.

## Content the target forbids

| Code | Meaning | What to do |
|---|---|---|
| `TRANSPARENCY_P1` | the document uses transparency and the target is PDF/A-1 or PDF/X-1a/X-3, which forbid it; the engine had no way to flatten it | use `suggestedLevel`, usually `2b` or `x4`; or `--allow-visual-risk` to force transparency off; or a build with PDFium, which rasterizes the affected pages |
| `JPX_IN_PDFA1` | a JPEG 2000 image could not be transcoded for PDF/A-1 | use a part 2 or later level |
| `CMYK_MIXED_P1` | PDF/A-1 with both RGB and CMYK content under a single output intent | use `suggestedLevel` |
| `RGB_UNDER_CMYK_P1` | RGB content under a CMYK output intent at PDF/A-1 | use `suggestedLevel` |
| `X1A_COLOR_UNCONVERTIBLE` | PDF/X-1a needs every colour in CMYK and something could not be converted, for example a mesh shading with per-vertex colour | the detail names the object; `suggestedLevel` is `x4`, which permits it |
| `FACTURX_REQUIRES_PDFA3` | an e-invoice was requested at a level that cannot carry attachments | use `3b`, `3u`, `3a` or `4f` |

## E-invoice payloads

| Code | Meaning | What to do |
|---|---|---|
| `EINVOICE_NOT_A_DOCUMENT` | the XML is not a recognised invoice or order document, or is not XML | check the payload |
| `EINVOICE_PROFILE_UNKNOWN` | the document declares no guideline | pass `--facturx-profile` if you know it |
| `EINVOICE_PROFILE_INVALID` | the declared level is outside the hybrid code list | fix the payload |
| `NO_EINVOICE` | `--extract-invoice` found no attachment | the file is not a hybrid invoice |

## Process

| Code | Meaning | What to do |
|---|---|---|
| `CONVERT_TIMEOUT` | the watchdog fired; exit status 3 | raise `PDFA_TIMEOUT`, or treat the file as hostile |
| `SERIALIZE_ERROR` | the converted document could not be written | please report with the file |
| `INTERNAL_ERROR` | an unexpected failure was caught at the boundary | please report with the file |
| `SIGN_NO_PAGES` | signing was requested on a document with no pages | |

## Findings that are not rejections

Two issue codes deserve a mention because they change how to read a report:

- `SCAN_INCOMPLETE`: a pass could not read part of the document, so its findings may be incomplete for that content. Treat the check as inconclusive rather than clean.
- Any issue whose detail ends in `(visual risk)`: a repair that can change appearance, made because `--allow-visual-risk` was set.
