# npm package

```bash
npm install kura-pdf
```

`kura-pdf` is the engine compiled to WebAssembly with a small JavaScript API around it. There are no native binaries and no postinstall downloads; the module ships inside the package and runs on any platform Node 22 or newer runs on, and in browsers.

## A complete first program

```js
import { readFile, writeFile } from 'node:fs/promises';
import { convert } from 'kura-pdf';

const input = new Uint8Array(await readFile('input.pdf'));
const result = await convert(input, '2b');

await writeFile('output.pdf', result.pdf);
for (const issue of result.issues) console.log(`${issue.code}: ${issue.detail}`);
```

Run with `node convert.mjs`. The first call loads the WebAssembly module, which takes a moment; it stays loaded for the life of the process.

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
  attachXml?: Uint8Array;       // e-invoice payload; see /e-invoices
  attachXmlName?: string;
  facturxProfile?: string;
  embedSource?: Uint8Array;     // attach a file with /AFRelationship /Source
  embedSourceName?: string;
  embedSourceMime?: string;
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

The options are the same as the [CLI flags](/cli) with the dashes turned into camelCase. Binary values, the ICC profiles, the invoice XML and the embedded source, are passed as `Uint8Array`.

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

`check()` also accepts the seven check-only flavours `x4p`, `x5g`, `x5n`, `x5pg`, `x6n`, `x6p` and `vt2`. It throws only when the document cannot be read at all; a non-conforming document is a normal result with `compliant: false`.


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

Every code is listed on [Rejection codes](/rejections).

## version()

```js
import { version } from 'kura-pdf';
console.log(await version());   // "BentoPDF Kura Engine 1.1.0"
```

## The kura command

The package installs a `kura` command with the same flags, the same JSON report and the same exit codes as the [native CLI](/cli), minus the four flags that need the host (`--sign`, `--ocr`, `--font-folder`, `--substitute`):

```bash
npx kura --level 2b input.pdf output.pdf
npx kura --check --level 2b input.pdf
npx kura --einvoice invoice.xml input.pdf output.pdf
npx kura --level 2b -r -d out/ inbox/
```

## Blocking, and how to avoid it

Conversion runs synchronously inside your process once the module is loaded; a large document can hold the Node event loop for several seconds. Fine in a script; in a server, run it inside a worker thread:

```js
// worker.js
import { parentPort, workerData } from 'node:worker_threads';
import { convert } from 'kura-pdf';

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

This keeps your server responsive and gives you crash isolation: hostile input takes down the worker thread, not the process. For untrusted uploads at volume, the native binary in a subprocess with `PDFA_TIMEOUT` set is the more robust shape.

## In the browser

The same module powers [kura.bentopdf.com](https://kura.bentopdf.com), running in a Web Worker so visitors' files never leave their machines. The [repository's `site/` directory](https://github.com/alam00000/bentopdf-kura/tree/main/site) is the reference integration. For your own build, load `engine/kura.js` from a worker, call the module's `convert(bytes, level, options)` directly, and post the result back; the package's `index.js` shows the mapping from the module's raw result to `KuraResult`.

## Troubleshooting

**Out of memory on very large files.** The module holds input, working set and output inside a 4 GB address space. For files in the hundreds of megabytes, use the native binary.

**Rasterization does nothing.** The module only rasterizes when it was built with PDFium, which the published package is. If you built the module yourself without a PDFium wasm build, pages that need flattening are reported instead.

**The event loop stalls.** Expected in-process; use the worker thread pattern above.
