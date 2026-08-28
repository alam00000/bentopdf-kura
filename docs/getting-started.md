# Getting Started

Kura can be used in a few different ways, but the engine underneath is always the same. Whichever surface you pick, you get the same [25 targets](/standards), the same [preflight profiles](/preflight), the same options, and the same JSON report from the same settings.

## The fastest way: your browser

If you just want to convert or check a PDF, you do not need to install anything.

1. Open [kura.bentopdf.com](https://kura.bentopdf.com).
2. Drop your PDF onto the page, or click to choose it.
3. Pick a target, PDF/A-2b for most archival work, and press Convert. Or turn on "Check only" to see what would change without producing a file.
4. Save the result.

The engine runs as WebAssembly inside your browser. Your file is never uploaded anywhere; load the page, disconnect from the internet, and it still works.

## From the terminal

### With npm

Node.js 22 or newer:

```bash
npm install -g kura-pdf
kura --level 2b input.pdf output.pdf
```

You should see a JSON report:

```json
{"file":"input.pdf","ok":true,"level":"2b","engine":"BentoPDF Kura Engine 1.1.0","issues":[
  {"code":"OUTPUT_INTENT_ADDED","detail":"added sRGB PDF/A output intent","fixed":true},
  {"code":"FONT_SUBSTITUTED","detail":"embedded LiberationSans as substitute for /Helvetica","fixed":true},
  {"code":"XMP_REBUILT","detail":"regenerated XMP metadata with PDF/A identification","fixed":true}]}
```

Reading it: `ok` says the conversion succeeded, `level` is what the output conforms to, and `issues` is every change the engine made, each with a code you can match on. This `kura` runs the WebAssembly build of the engine, so it works on any platform Node runs on.

### With the native binary

Prebuilt binaries for Linux, Windows and macOS are attached to every [release](https://github.com/alam00000/bentopdf-kura/releases). They are single files with no dependencies beyond the operating system, and they are faster than the npm build and add `--sign`, `--ocr` and a few other features that need the host. Download the archive for your platform, verify it against `SHA256SUMS`, and put `kura` on your `PATH`.

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

## From Node.js code

```js
import { readFile, writeFile } from 'node:fs/promises';
import { convert } from 'kura-pdf';

const input = new Uint8Array(await readFile('input.pdf'));
const result = await convert(input, '2b', { ua: false });

await writeFile('output.pdf', result.pdf);
for (const issue of result.issues) console.log(issue.code, issue.detail);
```

Save that as `convert.mjs` next to an `input.pdf` and run `node convert.mjs`. The [npm page](/npm) walks through `check()`, error handling, e-invoices and running the engine in a worker.

## In a container

```bash
docker run --rm -v "$PWD:/work" ghcr.io/alam00000/bentopdf-kura:latest --level 2b in.pdf out.pdf
```

The image contains the native CLI, runs as an unprivileged user, and processes files on your own machine.

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
- The programming interface: [npm package](/npm) and [C API](/c-api)
- What each target means: [The standards](/standards)
- Every error code and what to do: [Rejection codes](/rejections)
- Factur-X, ZUGFeRD, XRechnung, Order-X: [E-invoices](/e-invoices)
- Print and archival rules of your own: [Preflight profiles](/preflight)
