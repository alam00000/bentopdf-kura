# Getting Started

Kura can be used in a few different ways, but the engine underneath is always the same. Whichever surface you pick, you get the same [25 targets](/standards), the same [preflight profiles](/preflight), the same options, and the same JSON report from the same settings.

## The fastest way: your browser

If you just want to convert or check a PDF, you do not need to install anything.

1. Open [kura.bentopdf.com](https://kura.bentopdf.com).
2. Drop your PDF onto the page, or click to choose it.
3. Pick a target, PDF/A-2b for most archival work, and press Convert. Or turn on "Check only" to see what would change without producing a file.
4. Save the result.

The engine runs as WebAssembly inside your browser. Your file is never uploaded anywhere; load the page, disconnect from the internet, and it still works.

- **Check a file.** Turn on "Check only" and press the button: you get the list of what would change, and no file.
- **Run a preset.** Open [kura.bentopdf.com/preflight](https://kura.bentopdf.com/preflight), drop the PDF, pick a profile such as "Sheetfed offset, CMYK: check" and press Analyze to see the hits page by page, or Fix and download for a profile that repairs.
- **Convert colour.** Choose PDF/X-1a as the target: every colour becomes CMYK. Other colour conversions, to gray, to a named press profile or with an ink limit, need the CLI, npm or the HTTP API below.

## From the terminal

### With npm

```bash
npm install -g kura-pdf
kura --level 2b input.pdf output.pdf
```

`kura-pdf` installs the native engine through a platform package for Linux x64, macOS arm64 or Windows x64; the `kura` command is the native CLI itself. On any other platform, `npm install -g kura-pdf-wasm` gives the `kura-wasm` command on the WebAssembly build, with the same report and most of the flags ([what it takes](/cli#the-kura-wasm-command)).

### With the native binary

Prebuilt binaries for Linux x64, Windows x64 and macOS arm64 are attached to every [release](https://github.com/alam00000/bentopdf-kura/releases). The binary needs nothing beyond the operating system, and it is faster than the WebAssembly build and adds `--sign`, `--ocr` and a few other features that need the host. Download the archive for your platform, verify it against `SHA256SUMS`, unpack it, and put the folder on your `PATH`:

```bash
mkdir -p ~/kura && tar -xzf kura-v1.3.0-linux-x64.tar.gz -C ~/kura
export PATH="$HOME/kura:$PATH"
kura --version
```

Keep the `icc`, `fonts-cjk` and `fonts-world` folders next to `kura`: they hold the colour profiles and the fonts used for Chinese, Japanese, Korean, Arabic, Indic and other scripts. If you move `kura` on its own, point `KURA_FONT_DIR` at the font folders ([Fonts](/cli#fonts)).

Common variations, identical on both:

```bash
kura --level 2a --ua in.pdf out.pdf          # archival plus accessibility
kura --level x1a in.pdf out.pdf              # CMYK print
kura --check --level 2b in.pdf               # report only, write nothing
kura --einvoice invoice.xml in.pdf out.pdf   # hybrid e-invoice
kura --password hunter2 --level 2b in.pdf out.pdf
kura --level 2b -r -d out/ inbox/            # a whole folder
```

Every flag is documented on the [CLI page](/cli).

### Check, preset and colour on the CLI

```bash
kura --check --level 2b in.pdf; echo "exit $?"     # 0 conforms, 1 findings, 2 unreadable
```

A preset is a [preflight profile](/preflight), a JSON file. The bundled ones are in the repository under [`pdfa-engine/profiles/`](https://github.com/alam00000/bentopdf-kura/tree/main/pdfa-engine/profiles); download one and pass it:

```bash
curl -LO https://raw.githubusercontent.com/alam00000/bentopdf-kura/main/pdfa-engine/profiles/press/sheetfed-offset-cmyk-check.json
kura --check --profile sheetfed-offset-cmyk-check.json in.pdf     # hits arrive under "analysis"
```

Convert colour with `--convert-colors`, naming a target or a press profile:

```bash
kura --level x4 --convert-colors cmyk in.pdf out.pdf                                  # built-in GRACoL CMYK
kura --level x4 --convert-colors "Coated FOGRA39 (ISO 12647-2:2004)" in.pdf out.pdf   # a bundled profile by name
kura --level none --convert-colors gray in.pdf out.pdf                                # gray, no standard
```

[Converting colour](/cli#converting-colour) lists every colour option and where Kura looks for profiles.

## From Node.js code

```js
import { readFile, writeFile } from 'node:fs/promises';
import { convert } from 'kura-pdf';

const input = new Uint8Array(await readFile('input.pdf'));
const result = await convert(input, '2b', { ua: false });

await writeFile('output.pdf', result.pdf);
for (const issue of result.issues) console.log(issue.code, issue.detail);
```

Save that as `convert.mjs` next to an `input.pdf` and run `node convert.mjs`. Install it in your project first with `npm install kura-pdf` (or `kura-pdf-wasm` on other platforms; the code is the same).

Check, preset and colour:

```js
import { check } from 'kura-pdf';

const report = await check(input, '2b');
console.log(report.compliant, report.findings);

const profile = await readFile('sheetfed-offset-cmyk-check.json', 'utf8');   // a downloaded preset
const preflight = await check(input, 'x4', { profile });
for (const hit of preflight.analysis) console.log(hit.detail);

const cmyk = await convert(input, 'x4', { convertColors: 'cmyk', inkLimit: 300 });
```

The [npm page](/npm) walks through every option, error handling, e-invoices and running the engine in a worker.

## In a container

```bash
docker run -d -p 8080:8080 ghcr.io/alam00000/bentopdf-kura:latest
```

That serves the web interface at `http://localhost:8080` and an [HTTP API](/http-api) on the native engine, all on your own machine. The same image is the CLI when you pass arguments:

```bash
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" \
  ghcr.io/alam00000/bentopdf-kura:latest --level 2b in.pdf out.pdf
```

Check, preset and colour over HTTP:

```bash
curl -s --data-binary @in.pdf 'http://localhost:8080/api/check?level=2b'
curl -s --data-binary @in.pdf 'http://localhost:8080/api/check?level=x4&profile=press/sheetfed-offset-cmyk-check'
curl -o out.pdf --data-binary @in.pdf 'http://localhost:8080/api/convert?level=x4&convertColors=cmyk&inkLimit=300'
```

`GET /api/profiles` lists the ids of every bundled preset. Inside the image the presets are also files, at `/kura/pdfa-engine/profiles`, for the CLI form:

```bash
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" ghcr.io/alam00000/bentopdf-kura:latest \
  --check --profile /kura/pdfa-engine/profiles/press/sheetfed-offset-cmyk-check.json in.pdf
```

[HTTP API](/http-api) has every parameter; [Self-hosting](/self-hosting) covers configuration, TLS, updates and monitoring.

## From C, C++ or another language

Every release ships `libkura.a` and `kura.h` for linking the engine into your own program. One call converts or checks a document in memory; a preset is passed as the profile's JSON text, and colour conversion goes through a profile's `convert` section. The [C API page](/c-api) has a complete example and the link line.

## Picking a target

Most people want one of three:

| You want | Use |
|---|---|
| a document that will still open and read correctly in decades | `2b` |
| the same, plus text that is guaranteed searchable and copyable | `2u` |
| the same, plus accessibility for screen readers | `2a --ua` |

Add `3b`, `3u` or `3a` instead when the document must carry attachments, and `4` when you want the current PDF 2.0-based standard. Print, engineering and variable-data work use the PDF/X, PDF/E and PDF/VT families. The [standards page](/standards) explains every target and when to choose it.

## Three things to know before you start

1. **The output conforms or you get an error.** Kura never writes a file that claims a standard it does not meet. A rejection carries an error code and, where one exists, a `suggestedLevel` that would work; `TRANSPARENCY_P1` on a PDF/A-1 job, for instance, suggests `2b`.
2. **Every change is in the report.** The `issues` list is complete: fonts substituted, colour converted, pages rasterized, content removed. Nothing happens silently.
3. **Check mode is a preflight, not a validator.** `--check` reports exactly what the converter would repair, so it never gives a false finding, but it cannot see defects the converter does not know how to fix. Use an independent validator for certification.

## Where next

- Every flag and exit code: [CLI](/cli)
- The programming interface: [npm package](/npm), [HTTP API](/http-api) and [C API](/c-api)
- What each target means: [The standards](/standards)
- Every error code and what to do: [Rejection codes](/rejections)
- What each line of a report means: [Issue codes](/issue-codes)
- How Kura's results compare with independent validators: [Conformance](/conformance)
- Factur-X, ZUGFeRD, XRechnung, Order-X: [E-invoices](/e-invoices)
- Print and archival rules of your own: [Preflight profiles](/preflight)
