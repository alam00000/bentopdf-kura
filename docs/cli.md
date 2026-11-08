# CLI

The `kura` command is the native binary: attached to every [release](https://github.com/alam00000/bentopdf-kura/releases), inside the Docker image, and installed by `npm install -g kura-pdf`. `npm install -g kura-pdf-wasm` gives `kura-wasm`, the WebAssembly build with identical flags and identical output for platforms without a native package. A handful of flags need the host operating system and exist only on the native binary; they are marked below.

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
kura --level <level> [options] <input.pdf> [output.pdf]
kura --check --level <level> [options] <input.pdf>
kura --einvoice <invoice.xml> [--level 3b|3u|3a|4f] <input.pdf> [output.pdf]
kura --extract-invoice <input.pdf> [out.xml]
kura --check-invoice <input.pdf>
kura --verify-password [--password <pw>] <input.pdf>
kura --level <level> --batch [-r] [-d <dir>] [-s <suffix>] [-w] <folder>
kura --help
kura --version
```

The output path is optional. Leave it out and the result is written next to the input as `<input>.<level>.pdf`, for example `report.2b.pdf`, or `report.2a-ua.pdf` with `--ua`; the report's `output` field says where it went.

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
| `output` | conversions | where the result was written, given or derived |
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

`--level none` applies no standard: only the fixes of a `--profile`, a colour conversion or `--pdf-version` are applied, and the rest of the file is left as it is. A profile without a `convert` section runs this way when `--level` is left out.

`--pdf-version <v>` writes the file as PDF `1.3`, `1.4`, `1.5`, `1.6`, `1.7` or `2.0` with `--level none`, removing what that version does not support: below 1.5 layers are merged, 16-bit images reduced to 8 bits and JPEG 2000 re-encoded as JPEG, and below 1.4 transparency is flattened.

`--ua` layers PDF/UA on top of a PDF/A level: UA-1 on parts 1 to 3, UA-2 on part 4.

`--lang <tag>` sets the document language for PDF/UA, as a BCP 47 tag such as `en-US`. Required when the document declares none.

### Modes

`--check` runs the whole detection pipeline and writes nothing. Takes one path, not two. See [Check mode](/check-mode).

`--analyze` adds a document census to the report: page sizes, colour usage, transparency, fonts, images. Combines with `--check`.

`--profile <file>` runs a preflight profile and reports its hits under `analysis`; its fixes apply during a conversion. Without `--level`, the target comes from the profile's `convert` section. `--var name=value` sets a profile variable, `--var-file <json>` sets several. `--list-profiles <dir>` lists the profiles in a folder and `--list-vars <file>` the variables of one. See [Preflight profiles](/preflight).

`--batch`, `-r`, `-d <dir>`, `-s <suffix>`, `-w` process a folder. The input argument is a directory, output is omitted, and the report becomes a JSON array with one object per file plus a one-line summary on stderr. `-r` recurses. Outputs go to `-d`, or beside the inputs with suffix `_pdfa` unless you give `-s` or `-w` to overwrite. Exit status is the worst of any file.

### Input handling

`--password <pw>` opens an encrypted document. Without it, an encrypted input fails with `PASSWORD_REQUIRED`.

`--allow-visual-risk` permits repairs that can change appearance: forcing transparency off, flattening blend modes, dropping soft masks. Without it, those documents are rejected with a suggested level that permits the content instead. Every such change is reported with `(visual risk)` in its detail.

`--rasterize-pages` renders every page to an image. `--raster-dpi <n>` sets the resolution for that and for the transparency fallback, 24 to 1200, default 300.

`--image-max-ppi <n>` downsamples images placed above this resolution to the size they are actually drawn at. 0, the default, leaves images alone.

`--outline-fonts` converts text that has no derivable Unicode mapping into vector outlines so a `u` or `a` level can still be reached. The affected text is no longer searchable, and the report says how many glyphs were affected.

### Colour

`--output-condition <name>`, `--output-condition-info <text>`, `--registry <url>` identify the press characterization written into a PDF/X output intent. By default Kura writes the registered condition of the profile in use (for example `FOGRA39`, `CGATS TR 006` or `JC200103`) with the ICC registry, and its description as the condition.

Profiles can be given by name instead of by file, for example `--convert-colors "Coated FOGRA39 (ISO 12647-2:2004)"`. Kura looks them up by description in its bundled profiles, the system colour folders and the Adobe colour folders that Adobe applications install, plus any `--icc-folder`.

`--dest-profile <icc>` names the ICC file used as the output intent and, at X-1a, as the CMYK conversion target. The profile's colour space is checked against the target.

`--default-rgb <icc>`, `--default-cmyk <icc>`, `--default-gray <icc>` replace the bundled profiles used for default colour spaces.

#### Converting colour

At `x1a` every colour is converted to CMYK automatically. At any other level, `--convert-colors` converts the document's colour through ICC profiles (Little CMS), whatever level you write.

```bash
kura --level x4 --convert-colors cmyk --dest-profile press.icc in.pdf out.pdf
kura --level x1a --convert-colors cmyk --intent relative --ink-limit 300 --keep-spots in.pdf out.pdf
kura --level 2b --convert-colors gray in.pdf out.pdf
```

`--convert-colors <target>` takes one of:

| Target | Converts to | Profile |
|---|---|---|
| `cmyk` | CMYK | `--dest-profile` when given, the bundled CMYK profile otherwise; it also becomes the output intent |
| `gray` | grayscale | the built-in Dot Gain 15% |
| `rgb` | RGB | sRGB IEC61966-2.1 |
| an ICC file | that profile's colour space | the file; a CMYK profile also becomes the output intent |
| a profile name | that profile's colour space | looked up by file name or description in the system colour folders and `--icc-folder` |

With a CMYK target, RGB, calibrated RGB and gray, and Lab are converted; device CMYK is kept as it is; ICC-based CMYK is written as device CMYK under the output intent; device gray is kept unless `--gray-to-k` is given. Spot colours (Separation and DeviceN) become process colour unless `--keep-spots` is given. With a gray or RGB target, everything except spot colours is converted. Images, text, vector art and shadings are all converted, and the report lists the changes as `COLOR_CONVERTED`.

`--intent perceptual|relative|saturation|absolute` sets the rendering intent. By default each object keeps the intent the document gives it, and relative colorimetric is used where none is given. Black point compensation is on; `--no-bpc` turns it off.

`--convert-objects <list>` limits conversion to some kinds of object: a comma list of `text`, `vector`, `shading`, `images`, or `lossy` and `lossless` for JPEG and losslessly compressed images separately.

`--keep-spots` keeps spot colours as spot colours and converts only their alternate colour.

`--gray-to-k` puts gray on the black plate only. `--rgb-gray-to-k <pct>` does the same for RGB colours whose three channels lie within `<pct>` percent of each other.

`--ink-limit <pct>` caps total ink coverage (C + M + Y + K) at 100 to 400 percent, taking the excess out of cyan, magenta and yellow. It also applies to colour that is already CMYK: those values keep their numbers and only lose the ink above the limit.

`--device-link <icc>` converts through a device link profile instead of a source and destination pair.

`--icc-folder <dir>` adds a folder to search for profiles named on the command line.

### Attachments

`--embed-source` attaches the original input to the output with `/AFRelationship /Source`. `--embed-source-name <name>` sets its file name. Needs a level that permits attachments.

`--attach-xml <file>`, `--attach-xml-name <name>` attach an arbitrary XML file. For invoices use `--einvoice` instead, which derives the name and relationship from the payload.

### E-invoices

`--verify-password` reports whether a password opens the file, as `{"ok":true,"valid":true}`, exit status 0 when it does and 1 when it does not; the self-hosted service uses it to check a password before queueing real work.

`--einvoice <invoice.xml>` builds a hybrid e-invoice; the level defaults to `3b`. `--facturx-profile <name>` overrides the detected profile. `--extract-invoice` and `--check-invoice` read an existing invoice back. All three are explained on [E-invoices](/e-invoices).

### PDF/VT

`--vt-records "1-3,4-6,…"` gives the page ranges that form each record; one document part is built per range.

### Signing <Badge type="tip" text="native only" />

`--sign <cert.p12>` applies a PKCS#7 detached signature over the whole file in the same pass, so the signed output is still conformant. `--sign-password`, `--sign-name`, `--sign-reason` and `--sign-location` set the certificate password and the signature's declared fields. The signature is a basic `adbe.pkcs7.detached`, not PAdES.

### OCR <Badge type="tip" text="native only" />

`--ocr` renders each page, runs it through Tesseract, and lays the recognized words back over the page as invisible text, so a scanned document becomes searchable and can reach a `u` level. `--ocr-engine <exe>` points at a Tesseract binary; the default is `tesseract` on `PATH`.

### Fonts <Badge type="tip" text="native only" />

`--font-folder <dir>` embeds fonts from your own TrueType files instead of the bundled substitutes, matched by file name. `--substitute <missing>=<replacement>` maps a missing font name to one in that folder.

Fonts that are not embedded are replaced by matching substitutes chosen by the characters actually used, not by the font's name. Latin, Greek, Cyrillic and Hebrew use the bundled Liberation fonts; Chinese, Japanese and Korean use the Noto CJK pack in `fonts-cjk/`; Arabic, the Indic scripts, Thai, Lao, Khmer, Myanmar, Tibetan, Sinhala, Armenian, Georgian, Ethiopic, Syriac and about 30 more use the Noto world pack in `fonts-world/`. Arabic joining forms and Indic conjuncts are shaped with HarfBuzz, and every substitute keeps the original character widths so line breaks and spacing stay where they were. A page whose text cannot be mapped at all is turned into an image with the original text kept as invisible, searchable text.

### Information

`--help`, `-h` prints the usage to stdout. `--version`, `-v` prints the engine name and version. Both exit 0.

## Tools

`kura tool <name> <arguments>` runs a single job and prints a JSON result. `kura tool <name> --help` shows a tool's arguments; every tool takes `--password`.

| Tool | What it does |
|---|---|
| `info <in.pdf>` | Title, author, dates, producer, PDF version, file size, page count and size, trim and bleed boxes, and the standards the file claims (PDF/A, PDF/X, PDF/E, PDF/VT, PDF/UA) |
| `plates <in.pdf>` | The colour plates each page really uses: a process plate counts when any 10 × 10 mm area reaches 1% ink (`--threshold`, `--area`), spot plates are read from the painted colours; reports pages using only black, using spot colours or using C, M or Y |
| `separations <in.pdf> <outdir>` | One greyscale image per process plate and page |
| `cxf <in.pdf> [<outdir>]` | Lists and extracts the CxF colour data (ISO 17972) stored in the output intent, with its conformance (X-4, X-4a, X-4b), and reports spot colours without colour data; `--embed <dir>` adds the CxF files of a folder, `--remove` deletes them |
| `extract-icc`, `extract-xmp`, `set-xmp` | Colour profiles and XMP metadata out of, or into, a file |
| `extract-dieline` | The die line of a packaging file as PDF or JSON |
| `get-dparts`, `set-dparts` | Document part (DPart) hierarchy as JSON |
| `inventory`, `visualize` | Object census and a visual report of where problems sit |
| `certify`, `verify-cert`, `uncertify` | Embed, check or remove a check certificate |
| `invoice-synopsis` | The key fields of an embedded e-invoice |

## Environment

`PDFA_TIMEOUT` sets the watchdog in seconds for each document, default 120. When it fires the run prints a `CONVERT_TIMEOUT` report and exits 3. Set it to 0 to disable, which is not recommended on untrusted input.

## Where the two builds differ

The native binary and the npm `kura` share the engine, the flags above, the report and the exit codes. Only `--sign`, `--ocr`, `--font-folder` and `--substitute` need the host and are absent from the npm build. The native binary is also roughly four times faster on large documents.
