# CLI

The `kura` command comes two ways with identical flags and identical output: the native binary attached to every [release](https://github.com/alam00000/bentopdf-kura/releases), and the WebAssembly build installed by `npm install -g kura-pdf`. A handful of flags need the host operating system and exist only on the native binary; they are marked below.

## Your first conversion

```bash
kura --level 2b input.pdf output.pdf
```

```json
{"file":"input.pdf","ok":true,"level":"2b","engine":"BentoPDF Kura Engine 1.1.0","issues":[
  {"code":"OUTPUT_INTENT_ADDED","detail":"added sRGB PDF/A output intent","fixed":true},
  {"code":"FONT_SUBSTITUTED","detail":"embedded LiberationSans as substitute for /Helvetica","fixed":true},
  {"code":"XMP_REBUILT","detail":"regenerated XMP metadata with PDF/A identification","fixed":true},
  {"code":"CONTENT_FILTERED","detail":"content streams normalized for conformance limits","fixed":true}]}
```

One JSON object per run, on stdout. Anything that is not the report, such as "cannot open", goes to stderr.

## Usage

```
kura --level <level> [options] <input.pdf> <output.pdf>
kura --check --level <level> [options] <input.pdf>
kura --einvoice <invoice.xml> [--level 3b|3u|3a|4f] <input.pdf> <output.pdf>
kura --extract-invoice <input.pdf> [out.xml]
kura --check-invoice <input.pdf>
kura --level <level> --batch [-r] [-d <dir>] [-s <suffix>] [-w] <folder>
kura --help
kura --version
```

## Exit status

| Code | Meaning |
|---|---|
| `0` | success; in check mode, the input already conforms |
| `1` | check mode found findings, or `--check-invoice` found inconsistencies |
| `2` | the input was rejected: unreadable, encrypted with an unknown key, or cannot be made to conform |
| `3` | the watchdog fired; see `PDFA_TIMEOUT` |
| `64` | usage error |

## The report

| Field | Present | Meaning |
|---|---|---|
| `file` | always | the input path |
| `ok` | always | whether the run produced a result |
| `level` | always | the target |
| `engine` | always | name and version |
| `errorCode`, `error` | on failure | see [Rejection codes](/rejections) |
| `suggestedLevel` | on some failures | a level that would accept this document |
| `mode`, `compliant`, `findings` | `--check` | see [Check mode](/check-mode) |
| `issues` | always | every change made, each `{code, detail, fixed}` |
| `analysis` | `--analyze` or `--profile` | census findings and profile hits, each `{code, detail}` |

## Flags

### Target

`--level <level>` selects the target: `1b 1a 2b 2u 2a 3b 3u 3a 4 4f 4e x1a x3 x4 x6 e1 vt1 vt3`, or one of the check-only flavours `x4p x5g x5n x5pg x6n x6p vt2` with `--check`. See [The standards](/standards).

`--ua` layers PDF/UA on top of a PDF/A level: UA-1 on parts 1 to 3, UA-2 on part 4.

`--lang <tag>` sets the document language for PDF/UA, as a BCP 47 tag such as `en-US`. Required when the document declares none.

### Modes

`--check` runs the whole detection pipeline and writes nothing. Takes one path, not two. See [Check mode](/check-mode).

`--analyze` adds a document census to the report: page sizes, colour usage, transparency, fonts, images. Combines with `--check`.

`--profile <file>` runs a preflight profile, in JSON or XML, and reports its hits under `analysis`. See [Preflight profiles](/preflight).

`--batch`, `-r`, `-d <dir>`, `-s <suffix>`, `-w` process a folder. The input argument is a directory, output is omitted, and the report becomes a JSON array with one object per file plus a one-line summary on stderr. `-r` recurses. Outputs go to `-d`, or beside the inputs with suffix `_pdfa` unless you give `-s` or `-w` to overwrite. Exit status is the worst of any file.

### Input handling

`--password <pw>` opens an encrypted document. Without it, an encrypted input fails with `PASSWORD_REQUIRED`.

`--allow-visual-risk` permits repairs that can change appearance: forcing transparency off, flattening blend modes, dropping soft masks. Without it, those documents are rejected with a suggested level that permits the content instead. Every such change is reported with `(visual risk)` in its detail.

`--rasterize-pages` renders every page to an image. `--raster-dpi <n>` sets the resolution for that and for the transparency fallback, 24 to 1200, default 300.

`--image-max-ppi <n>` downsamples images placed above this resolution to the size they are actually drawn at. 0, the default, leaves images alone.

`--outline-fonts` converts text that has no derivable Unicode mapping into vector outlines so a `u` or `a` level can still be reached. The affected text is no longer searchable, and the report says how many glyphs were affected.

### Colour

`--output-condition <name>`, `--output-condition-info <text>`, `--registry <url>` identify the press characterization written into a PDF/X output intent. Defaults to the bundled profile with identifier `Custom`.

`--dest-profile <icc>` supplies the ICC profile for the output intent and, at X-1a, for the CMYK conversion. The profile's colour space is checked against the target.

`--default-rgb <icc>`, `--default-cmyk <icc>`, `--default-gray <icc>` replace the bundled profiles used for default colour spaces.

### Attachments

`--embed-source` attaches the original input to the output with `/AFRelationship /Source`. `--embed-source-name <name>` sets its file name. Needs a level that permits attachments.

`--attach-xml <file>`, `--attach-xml-name <name>` attach an arbitrary XML file. For invoices use `--einvoice` instead, which derives the name and relationship from the payload.

### E-invoices

`--einvoice <invoice.xml>` builds a hybrid e-invoice; the level defaults to `3b`. `--facturx-profile <name>` overrides the detected profile. `--extract-invoice` and `--check-invoice` read an existing invoice back. All three are explained on [E-invoices](/e-invoices).

### PDF/VT

`--vt-records "1-3,4-6,…"` gives the page ranges that form each record; one document part is built per range.

### Signing <Badge type="tip" text="native only" />

`--sign <cert.p12>` applies a PKCS#7 detached signature over the whole file in the same pass, so the signed output is still conformant. `--sign-password`, `--sign-name`, `--sign-reason` and `--sign-location` set the certificate password and the signature's declared fields. The signature is a basic `adbe.pkcs7.detached`, not PAdES.

### OCR <Badge type="tip" text="native only" />

`--ocr` renders each page, runs it through Tesseract, and lays the recognized words back over the page as invisible text, so a scanned document becomes searchable and can reach a `u` level. `--ocr-engine <exe>` points at a Tesseract binary; the default is `tesseract` on `PATH`.

### Fonts <Badge type="tip" text="native only" />

`--font-folder <dir>` embeds fonts from your own TrueType files instead of the bundled substitutes, matched by file name. `--substitute <missing>=<replacement>` maps a missing font name to one in that folder.

### Information

`--help`, `-h` prints the usage to stdout. `--version`, `-v` prints the engine name and version. Both exit 0.

## Environment

`PDFA_TIMEOUT` sets the watchdog in seconds for each document, default 120. When it fires the run prints a `CONVERT_TIMEOUT` report and exits 3. Set it to 0 to disable, which is not recommended on untrusted input.

## Where the two builds differ

The native binary and the npm `kura` share the engine, the flags above, the report and the exit codes. Only `--sign`, `--ocr`, `--font-folder` and `--substitute` need the host and are absent from the npm build. The native binary is also roughly four times faster on large documents.
