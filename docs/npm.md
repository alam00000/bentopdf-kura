# npm packages

```bash
npm install kura-pdf
```

Two packages carry the engine, with the same API:

| Package | What it is | Use it when |
|---|---|---|
| `kura-pdf` | the native engine, installed through a platform package (`@bentopdf/kura-pdf-linux-x64`, `@bentopdf/kura-pdf-darwin-arm64`, `@bentopdf/kura-pdf-win32-x64`) and run in a subprocess | you are on Linux x64, macOS arm64 or Windows x64 and want speed, signing, OCR, your own fonts and profile names |
| `kura-pdf-wasm` | the engine compiled to WebAssembly, no native binaries, no postinstall downloads | any other platform, browsers, or environments that cannot ship native binaries |

Both need Node 22 or newer and are ESM only. `kura-pdf` installs the `kura` command, the native CLI itself; `kura-pdf-wasm` installs `kura-wasm`. On a platform without a native package, `kura-pdf` throws `ENGINE_MISSING`; set `KURA_BIN` to a binary you built to use it anyway.

## A complete first program

```js
import { readFile, writeFile } from 'node:fs/promises';
import { convert } from 'kura-pdf';

const input = await readFile('input.pdf');
const result = await convert(input, '2b');

await writeFile('output.pdf', result.pdf);
for (const issue of result.issues) console.log(`${issue.code}: ${issue.detail}`);
```

Run with `node convert.mjs`. Swap the import for `kura-pdf-wasm` and the program is unchanged; the first call then loads the WebAssembly module, which takes a moment and stays loaded for the life of the process.

## convert(input, level, options?)

Converts one document. Returns a `Promise<KuraResult>`; throws `KuraError` when the document is rejected.

```ts
type Level = '1b' | '1a' | '2b' | '2u' | '2a' | '3b' | '3u' | '3a' | '4' | '4f' | '4e'
           | 'x1a' | 'x3' | 'x4' | 'x6' | 'e1' | 'vt1' | 'vt3' | 'none';

interface KuraOptions {
  ua?: boolean;                 // layer PDF/UA on a PDF/A level
  lang?: string;                // document language, BCP 47, for PDF/UA
  password?: string;            // for encrypted input
  allowVisualRisk?: boolean;    // permit repairs that can change appearance
  rasterizePages?: boolean;     // render every page to an image
  rasterDpi?: number;           // 24 to 1200, default 300; also the transparency fallback
  outlineFonts?: boolean;       // outline text with no Unicode mapping
  imageMaxPpi?: number;         // downsample images above this resolution
  attachXml?: Uint8Array | string; // e-invoice payload; see /e-invoices
  attachXmlName?: string;
  facturxProfile?: string;
  embedSource?: boolean;        // attach the input document with /AFRelationship /Source
  embedSourceName?: string;
  pdfVersion?: string;          // '1.3'..'2.0', with level 'none'; see below
  outputCondition?: string;     // PDF/X output intent identification
  outputConditionInfo?: string;
  registry?: string;
  destProfile?: Uint8Array;     // ICC profile for the output intent
  defaultRgb?: Uint8Array;      // replace the bundled default profiles
  defaultCmyk?: Uint8Array;
  defaultGray?: Uint8Array;
  convertColors?: 'cmyk' | 'gray' | 'rgb' | Uint8Array; // convert colour; an ICC profile as bytes also works,
                                // and in kura-pdf a profile name such as 'ISO Coated v2 (ECI)'
  intent?: 'perceptual' | 'relative' | 'saturation' | 'absolute';
  convertObjects?: string[];    // any of text, vector, shading, images, lossy, lossless
  keepSpots?: boolean;          // keep spot colours, convert only their alternate
  noBpc?: boolean;              // turn off black point compensation
  grayToK?: boolean;            // gray on the black plate only
  rgbGrayToK?: number;          // neutral RGB within this percentage on the black plate only
  inkLimit?: number;            // total ink coverage limit, 100 to 400
  deviceLink?: Uint8Array;      // convert through a device link profile
  vtRecords?: string;           // PDF/VT record ranges, "1-3,4-6"
  profile?: string;             // a preflight profile, as JSON text
  analyze?: boolean;            // add the document census to `analysis`
  sign?: SignOptions;           // native only: PKCS#7 signature
  ocr?: boolean;                // native only: run tesseract on image-only pages
  ocrEngine?: string;           // native only: path to tesseract
  fontFolder?: string;          // native only: extra fonts for substitution
  timeoutMs?: number;           // native only: time limit for this call
}

// kura-pdf-wasm only:
interface KuraWasmExtras {
  embedSource?: Uint8Array;     // the bytes of the file to attach (instead of a boolean)
  embedSourceMime?: string;     // its MIME type, for example 'application/pdf'
  vars?: Record<string, string | number | boolean>; // values for the profile's variables
  now?: string;                 // a fixed PDF date such as 'D:20260101120000Z' for the dates Kura writes
}

interface SignOptions {
  p12: Uint8Array | string;     // the PKCS#12 file, as bytes or a path
  password?: string;
  name?: string;
  reason?: string;
  location?: string;
}

interface KuraResult {
  pdf: Uint8Array;              // the output document
  level: string;                // what it conforms to
  engine: string;               // name and version
  issues: Issue[];              // every change made
  analysis: Finding[];          // census and profile hits, when requested
}

interface Issue { code: string; detail: string; fixed: boolean }
interface Finding { code: string; detail: string }
```

The options are the same as the [CLI flags](/cli) with the dashes turned into camelCase. Binary values, the ICC profiles and the invoice XML, are passed as `Uint8Array`. Options left out take the CLI defaults.

The two packages differ in a few options:

| Option | `kura-pdf` | `kura-pdf-wasm` |
|---|---|---|
| `embedSource` | `true` attaches the input itself | the bytes of the file to attach; `embedSourceMime` sets its type |
| `convertColors` | `cmyk`, `gray`, `rgb`, ICC bytes, or a profile name looked up as on the [CLI](/cli#colour-profiles-what-is-included-and-where-kura-looks) | `cmyk`, `gray`, `rgb` or ICC bytes; a name throws `BAD_OPTION` |
| `imageMaxPpi`, `timeoutMs`, `sign`, `ocr`, `ocrEngine`, `fontFolder` | supported | ignored |
| `vars`, `now` | not available | supported |
| Text in a font the file does not embed | substituted from Kura's bundled fonts, so Arabic, Indic, Chinese, Japanese, Korean and other scripts stay text | the bundled script fonts are too large to ship in WebAssembly, so a page with such text in a non-Latin script is turned into an image instead; it still conforms, but that text can no longer be selected or searched |

`issues` and `analysis` entries carry `code`, `detail` and, for issues, `fixed`. The severity and page numbers the CLI prints are not passed through; run the CLI with `--check` when you need them.

### Without a standard

Pass the level `'none'` to apply only a colour conversion, a profile's fixes or a PDF version, and leave the rest of the file alone:

```js
const { pdf } = await convert(input, 'none', { pdfVersion: '1.4' });               // save as PDF 1.4
const gray = await convert(input, 'none', { convertColors: 'gray' });              // just make it grayscale
```

### Running a preflight profile

`profile` takes the JSON text of a [preflight profile](/preflight). The bundled ones live in the repository under [`pdfa-engine/profiles/`](https://github.com/alam00000/bentopdf-kura/tree/main/pdfa-engine/profiles); download the file you want, or write your own:

```js
const profile = await readFile('sheetfed-offset-cmyk-check-and-fix.json', 'utf8');

const report = await check(input, 'x4', { profile });                 // its checks, in report.analysis
const fixed = await convert(input, 'x4', { profile });                // its checks and fixes
```

The level you pass is the target: a profile's `convert.level` is not applied through the npm API. In `kura-pdf-wasm` only, passing an empty level, `convert(input, '', { profile })`, lets the profile's `convert` section choose the target, as `kura --profile` does without `--level`. Profile variables can be set with `vars` in `kura-pdf-wasm`; `kura-pdf` always uses their defaults.

To print-ready CMYK with your press profile as the output intent:

```js
const press = await readFile('press.icc');
const { pdf } = await convert(input, 'x4', { convertColors: 'cmyk', destProfile: press, intent: 'relative', inkLimit: 300 });
```

[Converting colour](/cli#converting-colour) explains what each colour option does. An out-of-range value throws `KuraError` with code `BAD_OPTION`.

## check(input, level, options?)

Runs [check mode](/check-mode): the whole detection pipeline with the output discarded.

```js
import { check } from 'kura-pdf';

const report = await check(input, '2b');
if (!report.compliant) {
  console.log(`${report.findings} finding(s)`);
  for (const issue of report.issues) console.log(issue.code, issue.detail);
}
```

```ts
interface KuraCheckResult {
  compliant: boolean;
  findings: number;
  level: string;
  engine: string;
  issues: Issue[];
  analysis: Finding[];
}
```

`check()` also accepts the seven check-only flavours `x4p`, `x5g`, `x5n`, `x5pg`, `x6n`, `x6p` and `vt2`. It throws only when the document cannot be read at all; a non-conforming document is a normal result with `compliant: false`. With `analyze: true` or a `profile`, the [preflight](/preflight) findings arrive in `analysis`.

## Locked files

A document with a user password is rejected with `PASSWORD_REQUIRED` unless the password is passed. To test a password before running a conversion, for instance behind an unlock box:

```js
import { verifyPassword, convert } from 'kura-pdf';

if (await verifyPassword(bytes, password)) {
  const { pdf } = await convert(bytes, '2b', { password });
}
```

`verifyPassword` returns `true` for a file that is not encrypted at all.

## Errors

Every failure is a `KuraError`:

```ts
class KuraError extends Error {
  code: string;                 // PASSWORD_REQUIRED, TRANSPARENCY_P1, PARSE_ERROR, …
  suggestedLevel: string | null; // a level that would accept this document, when one exists
  issues: Issue[];              // what the engine managed before it stopped
}
```

```js
import { convert, KuraError } from 'kura-pdf';

try {
  await convert(input, '1b');
} catch (e) {
  if (e instanceof KuraError && e.suggestedLevel) {
    return convert(input, e.suggestedLevel);
  }
  throw e;
}
```

Every rejection code is listed on [Rejection codes](/rejections). The packages add `BAD_LEVEL` (a level not in `LEVELS` or `CHECK_ONLY_LEVELS`), `BAD_INPUT` (the input or an option is not a `Uint8Array` or `ArrayBuffer`), `BAD_OPTION` (an option value out of range), `ENGINE_MISSING` (no binary for this platform, `kura-pdf`), `ENGINE_LOAD` (the WebAssembly module failed to load, `kura-pdf-wasm`), `ENGINE_ERROR` (the engine stopped without a report) and `TIMEOUT` (`timeoutMs` ran out).

## version(), load() and binaryPath()

```js
import { version, load, binaryPath, LEVELS, CHECK_ONLY_LEVELS } from 'kura-pdf';
console.log(await version());   // "BentoPDF Kura Engine 1.3.0"
console.log(binaryPath());      // where the engine binary lives, or null (kura-pdf only)
await load();                   // throws now rather than on the first call if the engine is missing
console.log(LEVELS);            // ['1b', '1a', …, 'vt3', 'none']
console.log(CHECK_ONLY_LEVELS); // ['x4p', 'x5g', 'x5n', 'x5pg', 'x6n', 'x6p', 'vt2']
```

`load()` resolves once the engine is ready: in `kura-pdf-wasm` it loads the WebAssembly module ahead of the first call, in `kura-pdf` it only checks that a binary exists. `binaryPath()` looks at `KURA_BIN` first, then the platform package.

## The kura command

`kura-pdf` installs `kura`, which is the [native CLI](/cli) itself, every flag included:

```bash
npx kura --level 2b input.pdf output.pdf
npx kura --check --level 2b input.pdf
npx kura --einvoice invoice.xml input.pdf output.pdf
npx kura --sign key.p12 --sign-password secret --level 2b input.pdf output.pdf
npx kura --level 2b -r -d out/ inbox/
```

The output path is optional: left out, the result is written next to the input as `<input>.<level>.pdf`. `kura-pdf-wasm` installs `kura-wasm` with the same JSON report and exit codes and a subset of the flags; [The kura-wasm command](/cli#the-kura-wasm-command) lists them.

## Servers and concurrency

`kura-pdf` runs the engine in a subprocess per call, so the event loop stays free and a crash on hostile input ends that process, not yours; set `timeoutMs` for untrusted uploads. Run as many calls in parallel as you have cores to spare.

`kura-pdf-wasm` runs synchronously inside your process once the module is loaded; a large document can hold the event loop for several seconds. In a server, run it inside a worker thread:

```js
// worker.js
import { parentPort, workerData } from 'node:worker_threads';
import { convert } from 'kura-pdf-wasm';

const result = await convert(new Uint8Array(workerData.input), workerData.level, workerData.options);
parentPort.postMessage(result, [result.pdf.buffer]);
```

```js
// main.js
import { Worker } from 'node:worker_threads';
import { readFile } from 'node:fs/promises';

const input = await readFile('input.pdf');
const worker = new Worker('./worker.js', { workerData: { input, level: '2b', options: {} } });
worker.on('message', (result) => console.log('converted,', result.pdf.length, 'bytes'));
```

## In the browser

`kura-pdf-wasm` is the module that powers [kura.bentopdf.com](https://kura.bentopdf.com), running in a Web Worker so visitors' files never leave their machines. The [repository's `site/` directory](https://github.com/alam00000/bentopdf-kura/tree/main/site) is the reference integration. For your own build, load `engine/kura.js` from a worker, call the module's `convert(bytes, level, options)` directly, and post the result back; the package's `index.js` shows the mapping from the module's raw result to `KuraResult`.

## Troubleshooting

**`ENGINE_MISSING`.** No platform package matched this machine; install `kura-pdf-wasm`, or build the engine and point `KURA_BIN` at it.

**Out of memory on very large files with the WebAssembly build.** The module holds input, working set and output inside a 4 GB address space. For files in the hundreds of megabytes, use `kura-pdf`.

**Rasterization does nothing in the WebAssembly build.** The module only rasterizes when it was built with PDFium, which the published package is. If you built the module yourself without a PDFium wasm build, pages that need flattening are reported instead.
