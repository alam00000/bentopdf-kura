# CLI

The `kura` command is the native binary: attached to every [release](https://github.com/alam00000/bentopdf-kura/releases), inside the Docker image, and installed by `npm install -g kura-pdf`. Everything on this page applies to it.

`npm install -g kura-pdf-wasm` gives `kura-wasm`, the WebAssembly build for platforms without a native package. It produces the same output and the same report, but takes only a subset of the flags; see [The kura-wasm command](#the-kura-wasm-command) at the end.

## Your first conversion

```bash
kura --level 2b input.pdf output.pdf
```

```json
{"file":"input.pdf","output":"output.pdf","ok":true,"level":"2b","engine":"BentoPDF Kura Engine 1.3.0","issues":[
  {"code":"OUTPUT_INTENT_ADDED","detail":"added sRGB PDF/A output intent","fixed":true,"severity":3,"pages":[]},
  {"code":"FONT_SUBSTITUTED","detail":"embedded LiberationSans as substitute for /Helvetica","fixed":true,"severity":3,"pages":[1,2]},
  {"code":"XMP_REBUILT","detail":"regenerated XMP metadata with PDF/A identification","fixed":true,"severity":1,"pages":[]},
  {"code":"CONTENT_FILTERED","detail":"content streams normalized for conformance limits","fixed":true,"severity":1,"pages":[]}]}
```

One JSON object per run, on stdout. Anything that is not the report, such as "cannot open", goes to stderr.

## Usage

```
kura --level <level> [options] <input.pdf> [output.pdf]
kura --check --level <level> [options] <input.pdf>
kura --profile <profile.json> [options] <input.pdf> [output.pdf]
kura --einvoice <invoice.xml> [--level 3b|3u|3a|4f] <input.pdf> [output.pdf]
kura --extract-invoice <input.pdf> [out.xml]
kura --check-invoice <input.pdf>
kura --verify-password [--password <pw>] <input.pdf>
kura --level <level> --batch [-r] [-d <dir>] [-s <suffix>] [-w] [--jobs N] [--watch] <folder>
kura --list-profiles <dir>
kura --list-vars <profile.json>
kura --compare <a.pdf> <b.pdf>
kura tool <name> [arguments]
kura --help
kura --version
```

Options can come in any order, before or after the file names. The input may also be a JPEG; it is wrapped as a one-page PDF first.

The output path is optional. Leave it out and the result is written next to the input as `<input>.<level>.pdf`, for example `report.2b.pdf`, or `report.2a-ua.pdf` with `--ua`; the report's `output` field says where it went.

## Exit status

| Code | Meaning |
|---|---|
| `0` | success; in check mode, the input already conforms |
| `1` | check mode found findings; `--check-invoice` found inconsistencies or no invoice; `--verify-password` was given the wrong password; or a file named by an option (`--profile`, `--dest-profile`, `--default-rgb`, `--einvoice`, `--var-file`, …) could not be read |
| `2` | the input was rejected: unreadable, encrypted with an unknown key, or cannot be made to conform; the report says why |
| `3` | the watchdog fired; see `PDFA_TIMEOUT` |
| `64` | usage error: an unknown level, a value out of range, a missing argument |

In a batch, the exit status is the worst of any file: 2 if any file was rejected or timed out, else 1 if any had findings, else 0.

## The report

| Field | Present | Meaning |
|---|---|---|
| `file` | always | the input path |
| `output` | conversions | where the result was written, given or derived |
| `ok` | always | whether the run produced a result |
| `level` | always | the target; `2b+x4` when a second standard was written with `--dual` |
| `engine` | always | name and version |
| `errorCode`, `error` | on failure | see [Rejection codes](/rejections) |
| `suggestedLevel` | on some failures | a level that would accept this document |
| `mode`, `compliant`, `findings` | `--check` | see [Check mode](/check-mode) |
| `issues` | always | every change made, each `{code, detail, fixed, severity, pages}` |
| `analysis` | `--analyze` or `--profile` | census findings and profile hits, each `{code, detail, severity, pages}` |
| `jobId` | `--job-id` | the id you gave |

`severity` is 1 for information, 2 for a warning and 3 for an error. `pages` lists the 1-based pages an entry concerns; an empty list means the whole document. Every code is explained on [Issue codes](/issue-codes).

When the watchdog fires, the report is `{"ok":false,"errorCode":"CONVERT_TIMEOUT",…}` plus a `partial` list with the last steps the engine finished, which shows where it got stuck.

## Flags

### Target

`--level <level>` selects the target: `1b 1a 2b 2u 2a 3b 3u 3a 4 4f 4e x1a x3 x4 x6 e1 vt1 vt3`, or one of the check-only flavours `x4p x5g x5n x5pg x6n x6p vt2 ua1 ua2` with `--check`. See [The standards](/standards).

`ua1` and `ua2` check a document against PDF/UA-1 or PDF/UA-2 alone, without any PDF/A rules. Each finding is reported as `UA_<rule>`, for example `UA_06-003`. To produce a PDF/UA file, use `--level 2u --ua` (UA-1) or `--level 4 --ua` (UA-2).

`--level none` applies no standard: only the fixes of a `--profile`, a colour conversion or `--pdf-version` are applied, and the rest of the file is left as it is. When `--level` is left out and a `--profile` is given, the profile's `convert` section picks the target, and without one the run behaves as `none`.

`--pdf-version <v>` writes the file as PDF `1.3`, `1.4`, `1.5`, `1.6`, `1.7` or `2.0` with `--level none`, removing what that version does not support: below 1.5 layers are merged, 16-bit images reduced to 8 bits and JPEG 2000 re-encoded as JPEG, and below 1.4 transparency is flattened.

`--ua` layers PDF/UA on top of a PDF/A level: UA-1 on parts 1 to 3, UA-2 on part 4. On a level that is not PDF/A the run fails with `UA_UNSUPPORTED_LEVEL`.

`--lang <tag>` sets the document language for PDF/UA, as a BCP 47 tag such as `en-US`. Required when the document declares none.

`--dual` writes PDF/A and PDF/X into the same file: with a part 2 level (`2b`, `2u`, `2a`) the file also meets PDF/X-4, with a part 4 level it also meets PDF/X-6. The report's `level` then reads `2b+x4` or `4+x6`. It has no effect on other levels.

```bash
kura --level 2b --dual in.pdf out.pdf        # one file that is both PDF/A-2b and PDF/X-4
```

### Modes

`--check` runs the whole detection pipeline and writes nothing. Takes one path, not two. See [Check mode](/check-mode).

`--quick`, `-q` with `--check` keeps only the first finding, for a fast yes or no.

`--analyze` adds a document census to the report: page sizes, colour usage, transparency, fonts, images. Combines with `--check`.

`--pages <list>` limits the report to some pages: `1-3,5`, `4-` (page 4 to the end), `-3`, `odd` or `even`. Entries on other pages are dropped, and in check mode `compliant` only looks at the listed pages. Entries that concern the whole document are always kept. The whole document is still converted.

### Preflight profiles

`--profile <file>` runs a [preflight profile](/preflight) and reports its hits under `analysis`; its fixes apply during a conversion. Without `--level`, the target comes from the profile's `convert` section.

`--var name=value` sets a profile variable; repeat it for several. `--var-file <json>` reads several from a JSON object such as `{"job": "4711", "min_ppi": 300}`.

`--list-profiles <dir>` lists every profile in a folder and its subfolders, as `{"ok":true,"profiles":[{file, name, description, kind, checks, fixes, variables}]}`; `kind` is `check`, `fix` or `plan`. `--list-vars <file>` lists the variables of one profile with their `label`, `type`, `default` and `choices`, and whether this build can run variable scripts (`scriptEngine`).

`--hits-per-page <n>` and `--hits-per-doc <n>` stop counting a check's hits after this many on one page, or in the whole document. 0, the default, counts everything.

`--visible-layers` makes profile checks ignore content on layers that are hidden.

```bash
kura --check --profile press/sheetfed-offset-cmyk-check.json in.pdf
kura --profile stamp.json --var job=4711 in.pdf out.pdf
```

### Fallbacks

These are safety nets for documents that do not convert cleanly. They apply to PDF/A targets.

`--reconvert` converts the result once more when the first pass did not reach compliance.

`--fallback-raster <dpi>` turns every page into an image at that resolution, 24 to 1200, when the regular conversion did not reach compliance. `--fallback-pages <dpi>` does the same for only the pages that still had findings. The original text stays on top as invisible, searchable text, unless `--fallback-no-text` is given. Both are reported as `FALLBACK_RECONVERTED` or `FALLBACK_RASTERIZED`.

`--only-pdfa` checks a PDF/A result again before writing it. If it does not pass, nothing is written and the run fails with `NOT_PDFA`.

`--certify` embeds a check certificate in the output: the level, whether it passed, the number of findings, the date and a fingerprint of the content. `kura tool verify-cert` later tells you whether the file still matches it. See [Tools](#tools).

```bash
kura --level 2b --reconvert --fallback-pages 300 --only-pdfa in.pdf out.pdf
```

### Input handling

`--password <pw>` opens an encrypted document. Without it, an encrypted input fails with `PASSWORD_REQUIRED`.

`--allow-visual-risk` permits repairs that can change appearance: forcing transparency off, flattening blend modes, dropping soft masks. Without it, those documents are rejected with a suggested level that permits the content instead. Every such change is reported with `(visual risk)` in its detail.

`--rasterize-pages` renders every page to an image. `--raster-dpi <n>` sets the resolution for that and for the transparency fallback, 24 to 1200, default 300.

`--image-max-ppi <n>` downsamples images placed above this resolution to the size they are actually drawn at, 0 to 10000. 0, the default, leaves images alone.

`--outline-fonts` converts text that has no derivable Unicode mapping into vector outlines so a `u` or `a` level can still be reached. The affected text is no longer searchable, and the report says how many glyphs were affected.

`--bookmarks headings` builds bookmarks from the document's tagged headings. `--bookmarks <file.json>` builds them from a list you give:

```json
[{"title": "Introduction", "page": 1},
 {"title": "Results", "page": 4, "children": [{"title": "Tables", "page": 6}]}]
```

Existing bookmarks are replaced. The report says `BOOKMARKS_ADDED`, or `BOOKMARKS_NOT_CREATED` when there were no tagged headings or the file could not be read.

### Colour

`--output-condition <name>`, `--output-condition-info <text>`, `--registry <url>` identify the press characterization written into a PDF/X output intent. By default Kura writes the registered condition of the profile in use (for example `FOGRA39`, `CGATS TR 006` or `JC200103`) with the ICC registry, and its description as the condition.

Profiles can be given by name instead of by file, for example `--convert-colors "Coated FOGRA39 (ISO 12647-2:2004)"`. Kura looks them up by description in its bundled profiles, the system colour folders and the Adobe colour folders that Adobe applications install, plus any `--icc-folder`.

`--dest-profile <icc>` names the ICC file used as the output intent and, at X-1a, as the CMYK conversion target. The profile's colour space is checked against the target.

`--profile-url <url>` with `--level x5n` and an n-colour `--dest-profile` produces PDF/X-5n: the output intent refers to the profile at that address instead of embedding it.

`--default-rgb <icc>`, `--default-cmyk <icc>`, `--default-gray <icc>` replace the bundled profiles used for default colour spaces. A file of the wrong colour space fails with `DEFAULT_PROFILE_MISMATCH`.

#### Converting colour

At `x1a` every colour is converted to CMYK automatically. At any other level, `--convert-colors` converts the document's colour through ICC profiles (Little CMS), whatever level you write.

```bash
kura --level x4 --convert-colors cmyk --dest-profile press.icc in.pdf out.pdf
kura --level x1a --convert-colors cmyk --intent relative --ink-limit 300 --keep-spots in.pdf out.pdf
kura --level 2b --convert-colors gray in.pdf out.pdf
kura --level none --convert-colors "ISO Coated v2 (ECI)" in.pdf out.pdf
```

`--convert-colors <target>` takes one of:

| Target | Converts to | Profile |
|---|---|---|
| `cmyk` | CMYK | `--dest-profile` when given, the bundled CMYK profile otherwise; it also becomes the output intent |
| `gray` | grayscale | the built-in Dot Gain 15% |
| `rgb` | RGB | sRGB IEC61966-2.1 |
| an ICC file | that profile's colour space | the file; a CMYK profile also becomes the output intent |
| a profile name | that profile's colour space | looked up as described below |

With a CMYK target, RGB, calibrated RGB and gray, and Lab are converted; device CMYK is kept as it is; ICC-based CMYK is written as device CMYK under the output intent; device gray is kept unless `--gray-to-k` is given. Spot colours (Separation and DeviceN) become process colour unless `--keep-spots` is given. With a gray or RGB target, everything except spot colours is converted. Images, text, vector art and shadings are all converted, and the report lists the changes as `COLOR_CONVERTED`.

`--intent perceptual|relative|saturation|absolute` sets the rendering intent. By default each object keeps the intent the document gives it, and relative colorimetric is used where none is given. Black point compensation is on; `--no-bpc` turns it off.

`--convert-objects <list>` limits conversion to some kinds of object: a comma list of `text`, `vector`, `shading`, `images`, or `lossy` and `lossless` for JPEG and losslessly compressed images separately.

`--keep-spots` keeps spot colours as spot colours and converts only their alternate colour.

`--gray-to-k` puts gray on the black plate only. `--rgb-gray-to-k <pct>` does the same for RGB colours whose three channels lie within `<pct>` percent of each other.

`--ink-limit <pct>` caps total ink coverage (C + M + Y + K) at 100 to 400 percent, taking the excess out of cyan, magenta and yellow. It also applies to colour that is already CMYK: those values keep their numbers and only lose the ink above the limit.

`--device-link <icc>` converts through a device link profile instead of a source and destination pair.

`--icc-folder <dir>` adds a folder to search for profiles named on the command line. Repeat it for several.

#### Colour profiles: what is included and where Kura looks

You can use any ICC profile: pass the file, or give its name (the description inside the profile, for example `"ISO Coated v2 (ECI)"`) and Kura finds it. A name is looked up in this order, first match wins:

1. every `--icc-folder <dir>` you pass;
2. the computer's colour folders:
   - Windows: `C:\Windows\System32\spool\drivers\color` and the Adobe profile folder under Common Files;
   - macOS: `~/Library/ColorSync/Profiles`, `/Library/ColorSync/Profiles`, `/System/Library/ColorSync/Profiles` and the Adobe profile folders under Application Support;
   - Linux: `~/.color/icc`, `~/.local/share/color/icc`, `/usr/share/color/icc` and `/usr/local/share/color/icc`;
3. the profiles that ship with Kura, in the `icc` folder next to the binary (or `/usr/local/share/kura/icc` in Docker).

So a profile installed by Adobe applications, a printer driver or your operating system is used automatically, without any option.

Kura ships these profiles:

| Profiles | Source |
|---|---|
| Dot Gain 10%, 15%, 20%, 25%, 30%, Gray Gamma 1.8 and 2.2, ProPhoto RGB, Lab D50, PCS XYZ, Flat XYZ, Black & White | made by Kura from published formulas, free to use |
| Coated FOGRA27, Coated FOGRA39, Uncoated FOGRA29, Web Coated FOGRA28, Coated GRACoL 2006, U.S. Web Coated (SWOP) v2, U.S. Web Uncoated v2, Web Coated SWOP 2006 Grade 3 and 5, Japan Color 2001 Coated and Uncoated, 2002 Newspaper, 2003 Web Coated, Japan Web Coated, Adobe RGB (1998), Apple RGB, ColorMatch RGB, PAL/SECAM, SMPTE-C, HDTV, NTSC, PAL | Adobe, under Adobe's colour profile bundling licence (`ICC-PROFILE-TERMS.md`) |
| SWOP 2006 Coated 3 (IDEAlliance), ISO Coated v2 (ECI), sRGB IEC61966-2.1 | redistributable under their owners' terms |

Coated GRACoL 2006 (CMYK) and sRGB are also built into the engine, so conversion works with no profile files at all; the browser build relies on those. A name that is not found anywhere is refused with `ICC_PROFILE_MISSING`; Kura never silently swaps in another profile.

### Fonts

Fonts that are not embedded are replaced by matching substitutes chosen by the characters actually used, not by the font's name. Latin, Greek, Cyrillic and Hebrew use the bundled Liberation fonts, which are built into the engine. Chinese, Japanese and Korean use the Noto CJK pack in `fonts-cjk/`; Arabic, the Indic scripts, Thai, Lao, Khmer, Myanmar, Tibetan, Sinhala, Armenian, Georgian, Ethiopic, Syriac and about 30 more use the Noto world pack in `fonts-world/`. Arabic joining forms and Indic conjuncts are shaped with HarfBuzz, and every substitute keeps the original character widths so line breaks and spacing stay where they were. Each substitution is reported as `FONT_SUBSTITUTED`.

The two packs ship next to the binary in every release archive and npm platform package, and in `/usr/local/share/kura` in the Docker image. Kura looks for them in this order: `--cjk-font-folder` (CJK only), the folder in `KURA_FONT_DIR`, then `fonts-cjk/` and `fonts-world/` next to the binary, then `../share/kura/` relative to the binary. A folder counts when it holds `NotoSansSC-Regular.ttf` (CJK) or `NotoSans-Regular.ttf` (world). If you move the binary on its own, move the two folders with it or point `KURA_FONT_DIR` at them.

When a font can be neither embedded nor substituted, for example because the pack for its script is missing, Kura turns that page into an image and keeps the original text on top as invisible, searchable text (`FONT_PAGE_RASTERIZED`). If the font's character map cannot be kept, the text on that page is no longer selectable, and the report says so. A build without a page renderer cannot do this and rejects the document with `FONT_UNEMBEDDABLE`.

<Badge type="tip" text="native only" /> `--font-folder <dir>` embeds fonts from your own font files instead of the bundled substitutes. Files ending in `.ttf`, `.ttc` or `.otf` are matched by file name against the missing font's name, ignoring case, spaces, hyphens and underscores, so `Frutiger-Bold.ttf` matches the font `Frutiger-Bold`. Subfolders are searched too. Only fonts with TrueType outlines are used; an `.otf` with PostScript (CFF) outlines is skipped and the bundled substitute is used instead. `--substitute <missing>=<replacement>` maps a missing font name, written exactly as in the document, to a different file name in that folder, for example `--substitute Helvetica=Arial`.

<Badge type="tip" text="native only" /> `--cjk-font-folder <dir>` names a folder holding `NotoSans{SC,TC,JP,KR}-{Regular,Bold}.ttf` to use instead of the bundled CJK pack.

### Attachments

`--embed-source` attaches the original input to the output with `/AFRelationship /Source`. `--embed-source-name <name>` sets its file name. Needs a level that permits attachments.

`--attach-xml <file>`, `--attach-xml-name <name>` attach an arbitrary XML file. For invoices use `--einvoice` instead, which derives the name and relationship from the payload.

### E-invoices

`--einvoice <invoice.xml>` builds a hybrid e-invoice; the level defaults to `3b`. `--facturx-profile <name>` overrides the detected profile.

`--extract-invoice <in.pdf> [out.xml]` writes the embedded invoice XML to `out.xml`, or to stdout when no output is given. It exits 1 with `NO_EINVOICE` when there is none.

`--check-invoice <in.pdf>` reads an embedded invoice back and compares it with the PDF: `{"ok":true,"einvoice":true,"standard","profile","documentType","attachment","consistent","problems":[],"warnings":[]}`. Exit status 0 when consistent, 1 when there are problems or no invoice, 2 when the file cannot be read.

All three are explained on [E-invoices](/e-invoices).

### Passwords

`--verify-password` reports whether a password opens the file, as `{"ok":true,"valid":true}`, exit status 0 when it does and 1 when it does not. A file that is not encrypted is valid with any password. The self-hosted service uses it to check a password before queueing real work.

### PDF/VT

`--vt-records "1-3,4-6,…"` gives the page ranges that form each record; one document part is built per range.

### Folders

`--batch` processes a folder: the input argument is a directory, the output is left out, and the report becomes a JSON array with one object per file, plus a one-line summary on stderr. Only files ending in `.pdf` are taken.

| Flag | Meaning |
|---|---|
| `-r`, `--recursive` | include subfolders |
| `-d`, `--out-dir <dir>` | write the results to this folder, created if needed; with the same file names unless `-s` is given |
| `-s`, `--suffix <text>` | add this to each output name, so `a.pdf` becomes `a<suffix>.pdf` |
| `-w`, `--overwrite` | allow an output to replace its input |
| `-j`, `--jobs <n>` | convert this many files at once, each in its own process |
| `--watch` | keep watching the folder; see below |

Without `-d`, `-s` or `-w`, outputs are written beside the inputs with the suffix `_pdfa`. `-r`, `-d`, `--jobs` and `--watch` turn batch mode on by themselves. In check mode nothing is written, so only `-r` and `--jobs` matter.

`--watch` turns the folder into a hot folder: every 2 seconds Kura looks for PDF files that have not changed for 2 seconds, converts them, prints a JSON array for that round, and moves each input into a `done/` subfolder. It runs at least two jobs at once and keeps running until you stop it.

```bash
kura --level 2b -r -d out/ inbox/            # convert a whole tree into out/
kura --check --level 2b --jobs 4 inbox/      # check a folder, four files at a time
kura --level 2b --watch -d archive/ hot/     # convert whatever lands in hot/
```

### Reports and logs

`--report <file>` also writes the result as a readable report. The file's extension picks the format: `.html`, `.pdf`, `.xml`, `.txt` or `.json`. Besides the result it lists an inventory of the input (fonts, images, colour spaces) and, for an e-invoice, its key fields.

`--report-lang <lang>` writes the report in `en` (default), `de`, `fr`, `es`, `nl` or `it`.

`--report-dict <json>` replaces wording in the report. Keys are the report's own labels (`title`, `summary`, `file`, …), optionally under a language key such as `"de"`, and a `codes` object maps an issue code or detail to the text you want shown instead:

```json
{"title": "Prepress check", "codes": {"FONT_SUBSTITUTED": "A font was replaced"}}
```

`--job-id <id>` adds `"jobId"` to the report and to every trace line, to match logs from a job queue.

`--trace <file>` appends each stage of the run to a log file, one line per stage with a millisecond timestamp.

### Signing <Badge type="tip" text="native only" />

`--sign <cert.p12>` applies a PKCS#7 detached signature over the whole file in the same pass, so the signed output is still conformant. `--sign-password`, `--sign-name`, `--sign-reason` and `--sign-location` set the certificate password and the signature's declared fields. The signature is a basic `adbe.pkcs7.detached`, not PAdES. A certificate that cannot be loaded stops the run with exit status 1.

### OCR <Badge type="tip" text="native only" />

`--ocr` renders each page, runs it through Tesseract, and lays the recognized words back over the page as invisible text, so a scanned document becomes searchable and can reach a `u` level. `--ocr-engine <exe>` points at a Tesseract binary; the default is `tesseract` on `PATH`. Tesseract is not bundled; install it separately.

### Comparing two files

`kura --compare <a.pdf> <b.pdf>` renders both files page by page at 72 dpi and reports what changed. It must be the first argument, and needs a build with the page renderer, which every release, npm platform package and Docker image has.

```json
{"compare": true, "pages": [
  {"page": 1, "changedPixels": 0, "changedPercent": 0, "maxDeltaPercent": 0},
  {"page": 2, "changedPixels": 1840, "changedPercent": 0.38, "maxDeltaPercent": 12.5, "changedRegion": [72, 640.5, 310, 702]}],
 "identical": false, "pageCount": 2}
```

`changedRegion` is the box around the changed pixels, in points from the bottom-left corner. A page that exists in only one file is reported as `"status": "added"` or `"removed"`. Use it to confirm that a conversion did not change how a document looks.

### Information

`--help`, `-h` prints the usage to stdout. `--version`, `-v` prints the engine name and version. Both exit 0.

## Tools

`kura tool <name> <arguments>` runs a single job and prints one JSON result, `{"ok":true,"tool":"<name>",…}` on success. `kura tool <name> --help` shows a tool's arguments. Every tool takes `--password <pw>` for encrypted input, and `--pages` where shown takes a list such as `1-3,5` or `2-last`.

Exit status: 0 on success, 1 when the answer is "no" (no die line, no invoice, no document parts, a certificate that no longer matches), 2 when the file cannot be read or written, 64 for wrong arguments. A failure prints `{"ok":false,…,"error":"…"}`; the codes are on [Rejection codes](/rejections#tools).

### Inspecting a file

| Tool | What it does |
|---|---|
| `info <in.pdf>` | Title, author, subject, keywords, creator, producer, dates, PDF version, file size, page count and first page size, trim and bleed boxes, page mode, whether it is encrypted or tagged, and the standards the file claims (`pdfa`, `pdfx`, `pdfe`, `pdfvt`, `pdfua`) |
| `inventory <in.pdf> [out]` | A census of the document: fonts, images, colour spaces, ICC profiles, spot colours, layers, annotations and pages. Without `out` it prints JSON; with `out.html`, `.xml`, `.pdf` or `.json` it writes a report. Takes `--report-lang` and `--report-dict` |
| `visualize <in.pdf> <result.json> <outdir> [--dpi 100]` | Renders every page to `page-0001.png`… with a red, orange or blue frame on pages that have errors, warnings or information in a saved Kura report, and writes `index.html` listing each page's entries. `--dpi` is 10 to 600 |
| `invoice-synopsis <in.pdf> [out]` | The key fields of an embedded e-invoice (format, seller, lines, taxes, totals); JSON on stdout, or a report in `.html`, `.xml`, `.pdf` or `.json` |

```bash
kura --check --level 2b in.pdf > result.json
kura tool visualize in.pdf result.json review/   # then open review/index.html
```

### Print

| Tool | What it does |
|---|---|
| `plates <in.pdf>` | The colour plates each page really uses. A process plate counts when any area of `--area` mm square (default 10) reaches `--threshold` percent ink (default 1); spot plates are read from the painted colours. Reports pages using only black, using spot colours or using C, M or Y. `--dpi` (18 to 600, default 72) sets the analysis resolution and `--profile <cmyk.icc>` the CMYK profile used |
| `separations <in.pdf> <outdir>` | One greyscale PNG per process plate and page (`…-Cyan.png`, `…-Magenta.png`, `…-Yellow.png`, `…-Black.png`), with each plate's coverage in the result. `--dpi` (1 to 2400, default 72), `--profile <cmyk.icc>`, `--pages` |
| `cxf <in.pdf> [<outdir>]` | Lists the CxF colour data (ISO 17972) stored in the output intent, with its conformance (X-4, X-4a, X-4b), extracts it to `outdir` when given, and reports spot colours without colour data. `cxf <in.pdf> <out.pdf> --embed <dir>` adds the `.cxf` or `.xml` files of a folder; `cxf <in.pdf> <out.pdf> --remove` deletes them |
| `extract-dieline <in.pdf> <out.pdf\|out.json>` | The die line of a packaging file: every path painted in a spot colour whose name matches `--names` (a regular expression, default `die\|cut\|stanz\|crease`, any case). `--format pdf\|json` (default from the extension), `--pages` |

### Metadata and structure

| Tool | What it does |
|---|---|
| `extract-icc <in.pdf> <outdir>` | Writes every ICC profile in the file (`icc-<id>.icc`) and lists its use, description, colour space and size |
| `extract-xmp <in.pdf> <outdir> [--document-only]` | Writes the document's XMP as `document.xmp` and, unless `--document-only`, the XMP attached to pages, images and other objects |
| `set-xmp <in.pdf> <metadata.xmp> <out.pdf>` | Replaces the document's XMP with the given packet. The file is written as is; convert it again if it must keep a standard |
| `get-dparts <in.pdf> <out.json>` | The PDF/VT document part hierarchy as JSON: `recordLevel`, `nodeNames` and a `root` whose `parts` nest down to `{start, end}` page numbers, with any part `metadata` |
| `set-dparts <in.pdf> <dparts.json> <out.pdf>` | Writes a hierarchy in that same JSON form into the file, replacing any existing one |

### Check certificates

A check certificate records inside the file that Kura checked it: the level, whether it passed, the number of findings, the date and a fingerprint of the content.

| Tool | What it does |
|---|---|
| `certify <in.pdf> <out.pdf> [--level 2b]` | Checks the file against the level (default `2b`) and writes it with the certificate. Exit 1 if it did not pass; the certificate is still written and says so |
| `verify-cert <in.pdf>` | Reads the certificate back: `certified`, `unchanged` (the content still matches the fingerprint), `level`, `compliant`, `findings`, `date`, `engine`. Exit 0 only when it is unchanged and passed |
| `uncertify <in.pdf> <out.pdf>` | Removes the certificate |

`--certify` on a conversion does the same as `certify` on the result.

## Environment

| Variable | Effect |
|---|---|
| `PDFA_TIMEOUT` | the watchdog in seconds for each document, default 120. When it fires the run prints a `CONVERT_TIMEOUT` report and exits 3. Set it to 0 to disable, which is not recommended on untrusted input. In a batch it applies to each file |
| `KURA_FONT_DIR` | a folder holding the CJK and world font packs, searched before the folders next to the binary. See [Fonts](#fonts) |
| `KURA_CHROME` | the Chrome, Chromium or Edge binary used by the `place.html` profile fix to draw HTML stamps. By default Kura looks for one on `PATH` and in the usual install folders. Without one, the fix does nothing and is reported as `FIX_NOT_NEEDED` with "HTML could not be rendered" |
| `KURA_BIN` | for the npm package `kura-pdf` only: the `kura` binary to run instead of the one from the platform package |
| `PDFA_TIMING`, `PDFA_TRACE` | developer diagnostics printed to stderr. Their format is not stable |

## The kura-wasm command

`kura-wasm`, from `npm install -g kura-pdf-wasm`, runs the same engine as WebAssembly and prints the same report with the same exit codes, except that it has no watchdog and so never exits 3. It takes:

- `--level` (every level except `ua1` and `ua2`), `--check`, `--ua`, `--lang`, `--password`, `--allow-visual-risk`, `--analyze`, `--outline-fonts`
- `--rasterize-pages`, `--raster-dpi`
- `--output-condition`, `--output-condition-info`, `--registry`, `--dest-profile`, `--default-rgb`, `--default-cmyk`, `--default-gray`
- `--profile`, `--einvoice`, `--attach-xml`, `--attach-xml-name`, `--facturx-profile`, `--embed-source`, `--embed-source-name`, `--vt-records`
- `--batch`, `-r`, `-d`, `-s`, `-w`, `--help`, `--version`

`--sign`, `--ocr`, `--font-folder` and `--substitute` are refused with a message that they need the native binary. Any other flag on this page, including colour conversion, `--pdf-version`, `--var`, the invoice readers and `kura tool`, is a usage error (exit 64) in `kura-wasm`; use the native `kura`, or the [npm API](/npm), which takes the colour and version options. The CJK and world font packs are not part of the WebAssembly build, so text in those scripts takes the fallback described under [Fonts](#fonts). The native binary is also roughly four times faster on large documents.
