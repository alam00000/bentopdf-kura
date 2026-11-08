# Rejection codes

When Kura cannot produce the output you asked for, the report carries `ok: false`, an `errorCode`, a human-readable `error`, and sometimes a `suggestedLevel`. This page lists every code, what it means, and what to do.

Everything not listed here converts, with the changes recorded as issues in the report.

## Input

| Code | Meaning | What to do |
|---|---|---|
| `PARSE_ERROR` | the bytes are not a PDF, or are damaged beyond qpdf's recovery | check the file; if it opens elsewhere, please report it with the file |
| `BAD_INPUT` | no input buffer was supplied | API misuse |
| `BAD_LEVEL` | the level string is not one Kura knows | see [The standards](/standards) |
| `ICC_PROFILE_MISSING` | the colour profile named for conversion was not found in any profile folder | check the name, or pass the profile file, or its folder with `--icc-folder` |
| `IMAGE_INPUT_WRAPPED` | not an error: the input was a JPEG and was wrapped as a one-page PDF | nothing; reported as an issue |
| `NESTING_TOO_DEEP` | the page tree or resources are nested more than 500 levels deep, or loop back on themselves; the file is malformed or crafted to stall readers | treat the file as hostile; if it comes from a trusted source, please report it |
| `LEVEL_VERIFY_ONLY` | the level is a check-only flavour (`x4p`, `x5g`, `x5n`, `x5pg`, `x6n`, `x6p`, `vt2`, `ua1`, `ua2`) and a conversion was asked for | convert to `suggestedLevel` (`x4`, `x6` or `vt1`), or for `ua1`/`ua2` use `--level 2u --ua` or `--level 4 --ua`; or add `--check`. `x5n` can be produced when you pass `--profile-url` and an n-colour `--dest-profile` |
| `UA_UNSUPPORTED_LEVEL` | `--ua` was given with a level that is not PDF/A; PDF/UA layers only on PDF/A (UA-1 on parts 1 to 3, UA-2 on part 4) | use `suggestedLevel` (`2u`) or another PDF/A level |

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
| `JBIG2_NOT_ALLOWED` | PDF/X-1a and PDF/X-3 do not allow JBIG2 images, and this build cannot decode one to re-encode it | use `x4` or `x6`, or a build with PDFium |
| `JPX_UNREADABLE` | a JPEG 2000 image could not be decoded, so it could not be re-encoded for a level that forbids JPEG 2000 or the form it is in | use a part 2 or later level, or `x4` or `x6` for print |
| `CMYK_MIXED_P1` | PDF/A-1 with both RGB and CMYK content under a single output intent | use `suggestedLevel` |
| `RGB_UNDER_CMYK_P1` | RGB content under a CMYK output intent at PDF/A-1 | use `suggestedLevel` |
| `X1A_COLOR_UNCONVERTIBLE` | PDF/X-1a needs every colour in CMYK and something could not be converted, for example a mesh shading with per-vertex colour | the detail names the object; `suggestedLevel` is `x4`, which permits it |
| `FACTURX_REQUIRES_PDFA3` | an e-invoice was requested at a level that cannot carry attachments | use `3b`, `3u`, `3a` or `4f` |
| `FONT_FORMAT_NOT_ALLOWED` | an OpenType font cannot be stored at a PDF 1.4 based level (PDF/A-1, PDF/X-1a, PDF/X-3) | use a part 2 or later level, or `x4` |
| `FONT_UNEMBEDDABLE` | a page uses a font that can be neither embedded nor substituted, and the page cannot be rasterized | install the font, or use a build with PDFium |
| `IMAGE_DEPTH_UNSUPPORTED` | 16-bit image data could not be decoded for a level that needs it reduced | please report with the file |
| `EMBED_SOURCE_UNSUPPORTED_LEVEL` | embedding the source document needs a level that allows attachments | use `3b`, `3u`, `3a`, `4f` or `e1` |

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
| `SIGN_FAILED` | the signature could not be created; the detail gives the reason | check the certificate and password |
| `DEFAULT_PROFILE_MISMATCH` | the supplied default profile is not a valid ICC profile of the needed colour space | pass an RGB, CMYK or gray profile as asked |
| `X_COLOR_ENGINE` | the colour transform could not be set up | check the supplied profiles; otherwise please report with the file |
| `RASTERIZER_UNAVAILABLE` | page rasterization was requested but this build has no renderer | use a build with PDFium |
| `RASTERIZE_FAILED` | some pages could not be rendered | please report with the file |
| `NOT_PDFA` | `--only-pdfa` was set and the result did not pass the PDF/A check, so nothing was written | run without `--only-pdfa` to see the result and its issues, or try `--reconvert` or `--fallback-pages` |

## Tools

`kura tool <name>` prints `{"ok":false,"tool":…,"error":…}` on failure, and the report tools (`inventory`, `invoice-synopsis`, `visualize`, `certify`, `uncertify`, `verify-cert`) also carry an `errorCode`. The exit status is 64 for a usage mistake, 2 for anything else, and 1 where the tool's answer is "no" (for example `verify-cert` on a file without a certificate, or `extract-dieline` when no die line is found).

| Code | Meaning | What to do |
|---|---|---|
| `USAGE` | wrong arguments or an out-of-range option; the message shows the right form | run `kura tool <name> --help` |
| `IO_ERROR` | a file could not be read or written | check the paths and permissions |
| `PARSE_ERROR` | the PDF, or the result JSON given to `visualize`, could not be read | check the file; pass `--password` for an encrypted PDF |
| `DICT_ERROR` | the `--report-lang` language is unknown, or the `--report-dict` file could not be read | use `en`, `de`, `fr`, `es`, `nl` or `it`, or fix the dictionary file |
| `NO_RASTERIZER` | `visualize` needs a page renderer and this build has none | use a build with PDFium |
| `RASTER_ERROR` | `visualize` could not render the document or one of its pages | please report with the file |
| `CHECK_FAILED` | `certify` could not check the document (the engine's own code is shown instead when there is one) | see the rest of this page for that code |

## npm packages

The npm packages pass every code above through on `KuraError.code`, and add a few of their own:

| Code | Meaning | What to do |
|---|---|---|
| `BAD_LEVEL` | the level is not in `LEVELS` (or `CHECK_ONLY_LEVELS` for `check()`) | see [The standards](/standards) |
| `BAD_INPUT` | the input, or an option that takes bytes, is not a `Uint8Array` or `ArrayBuffer`; in the WebAssembly build also a bad `pdfVersion` | pass bytes |
| `BAD_OPTION` | an option value was refused, for example `inkLimit` outside 100 to 400 | fix the option; the message names it |
| `ENGINE_MISSING` | `kura-pdf` found no engine binary for this platform | install `kura-pdf-wasm`, or set `KURA_BIN` |
| `ENGINE_LOAD` | `kura-pdf-wasm` could not load its WebAssembly module | check the package is installed completely |
| `ENGINE_ERROR` | the engine stopped without a report | please report with the file |
| `TIMEOUT` | `kura-pdf` ran past `timeoutMs` | raise `timeoutMs`, or treat the file as hostile |

The HTTP API's own status codes are on [HTTP API](/http-api#errors).

## Findings that are not rejections

Every change and finding code in a report is listed on [Issue codes](/issue-codes). Two deserve a mention here because they change how to read a report:

- `SCAN_INCOMPLETE`: a pass could not read part of the document, so its findings may be incomplete for that content. Treat the check as inconclusive rather than clean.
- Any issue whose detail ends in `(visual risk)`: a repair that can change appearance, made because `--allow-visual-risk` was set.
