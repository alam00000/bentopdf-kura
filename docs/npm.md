# npm packages

```bash
npm install kura-pdf
```

Two packages carry the engine, with the same API:

| Package | What it is | Use it when |
|---|---|---|
| `kura-pdf` | the native engine, installed through a platform package (`@bentopdf/kura-pdf-linux-x64`, `@bentopdf/kura-pdf-darwin-arm64`, `@bentopdf/kura-pdf-win32-x64`) and run in a subprocess | you are on Linux x64, macOS arm64 or Windows x64 and want speed, raster flattening, signing and OCR |
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
           | 'x1a' | 'x3' | 'x4' | 'x6' | 'e1' | 'vt1' | 'vt3';

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
  outputCondition?: string;     // PDF/X output intent identification
  outputConditionInfo?: string;
  registry?: string;
  destProfile?: Uint8Array;     // ICC profile for the output intent
  defaultRgb?: Uint8Array;      // replace the bundled default profiles
  defaultCmyk?: Uint8Array;
  defaultGray?: Uint8Array;
  vtRecords?: string;           // PDF/VT record ranges, "1-3,4-6"
  profile?: string;             // a preflight profile, JSON or XML text
  analyze?: boolean;            // add the document census to `analysis`
  sign?: SignOptions;           // native only: PKCS#7 signature
  ocr?: boolean;                // native only: run tesseract on image-only pages
  ocrEngine?: string;           // native only: path to tesseract
  fontFolder?: string;          // native only: extra fonts for substitution
  timeoutMs?: number;           // native only: time limit for this call
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

The options are the same as the [CLI flags](/cli) with the dashes turned into camelCase. Binary values, the ICC profiles and the invoice XML, are passed as `Uint8Array`. In `kura-pdf-wasm`, `embedSource` takes the bytes of the file to attach instead of a boolean, and the four native-only options are ignored.

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

Every rejection code is listed on [Rejection codes](/rejections). The package adds `BAD_LEVEL`, `BAD_INPUT`, `BAD_OPTION`, `ENGINE_MISSING` (no binary for this platform) and `TIMEOUT`.

## version() and binaryPath()

```js
import { version, binaryPath } from 'kura-pdf';
console.log(await version());   // "BentoPDF Kura Engine 1.1.0"
console.log(binaryPath());      // where the engine binary lives, or null
```

## The kura command

`kura-pdf` installs `kura`, which is the [native CLI](/cli) itself, every flag included:

```bash
npx kura --level 2b input.pdf output.pdf
npx kura --check --level 2b input.pdf
npx kura --einvoice invoice.xml input.pdf output.pdf
npx kura --sign key.p12 --sign-password secret --level 2b input.pdf output.pdf
npx kura --level 2b -r -d out/ inbox/
```

The output path is optional: left out, the result is written next to the input as `<input>.<level>.pdf`. `kura-pdf-wasm` installs `kura-wasm` with the same flags, the same JSON report and the same exit codes, minus the four flags that need the host (`--sign`, `--ocr`, `--font-folder`, `--substitute`).

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
