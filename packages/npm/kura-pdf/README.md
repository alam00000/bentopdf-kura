# kura-pdf

[Kura](https://github.com/alam00000/bentopdf-kura) is the open source PDF standards and preflight engine by [BentoPDF](https://www.bentopdf.com): it converts any PDF to PDF/A, PDF/UA, PDF/X, PDF/E or PDF/VT, runs print preflight with 396 bundled profiles, checks documents against those standards, and builds Factur-X, ZUGFeRD, XRechnung and Order-X e-invoices.

This package is the engine compiled to WebAssembly. No native binaries, no postinstall downloads; it runs on any platform Node 22 or newer runs on, and in browsers.

Full documentation lives at [kura.bentopdf.com/docs](https://kura.bentopdf.com/docs/).

## Install

```bash
npm install kura-pdf
```

## Quick start

```js
import { readFile, writeFile } from 'node:fs/promises';
import { convert } from 'kura-pdf';

const input = new Uint8Array(await readFile('input.pdf'));
const result = await convert(input, '2b');

await writeFile('output.pdf', result.pdf);
for (const issue of result.issues) console.log(`${issue.code}: ${issue.detail}`);
```

The package also installs a `kura` command with the same flags and the same JSON report as the native CLI:

```bash
npx kura --level 2b input.pdf output.pdf
npx kura --level 2a --ua input.pdf output.pdf
npx kura --check --level 2b input.pdf
npx kura --einvoice invoice.xml input.pdf output.pdf
npx kura --level 2b -r -d out/ inbox/
npx kura --help
```

Exit status: 0 on success, 1 when check mode found findings, 2 when the input was rejected, 64 on a usage error.

## API

### convert(input, level, options?)

Converts one document. Returns a `Promise<KuraResult>`; throws `KuraError` when the document is rejected.

```ts
type Level = '1b' | '1a' | '2b' | '2u' | '2a' | '3b' | '3u' | '3a' | '4' | '4f' | '4e'
           | 'x1a' | 'x3' | 'x4' | 'x6' | 'e1' | 'vt1' | 'vt3';

interface KuraOptions {
  ua?: boolean;                 // layer PDF/UA on a PDF/A level
  lang?: string;                // document language, BCP 47
  password?: string;            // for encrypted input
  allowVisualRisk?: boolean;    // permit repairs that can change appearance
  rasterizePages?: boolean;     // render every page to an image
  rasterDpi?: number;           // 24 to 1200, default 300
  outlineFonts?: boolean;       // outline text with no Unicode mapping
  attachXml?: Uint8Array;       // e-invoice payload
  attachXmlName?: string;
  facturxProfile?: string;
  embedSource?: Uint8Array;     // attach a file as the source
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
  pdf: Uint8Array;
  level: string;
  engine: string;
  issues: { code: string; detail: string; fixed: boolean }[];
  analysis: { code: string; detail: string }[];
}
```

### check(input, level, options?)

Runs the whole detection pipeline and reports what a conversion would change, without producing a file. Also accepts the check-only flavours `x4p`, `x5g`, `x5n`, `x5pg`, `x6n`, `x6p` and `vt2`.

```js
import { check } from 'kura-pdf';

const report = await check(input, '2b');
console.log(report.compliant, report.findings, report.issues);
```

### Errors

```js
import { convert, KuraError } from 'kura-pdf';

try {
  await convert(input, '1b');
} catch (e) {
  if (e instanceof KuraError && e.suggestedLevel) {
    return convert(input, e.suggestedLevel);   // e.g. TRANSPARENCY_P1 suggests 2b
  }
  throw e;
}
```

`KuraError` carries `code`, `suggestedLevel` when one exists, and the `issues` the engine recorded before it stopped. Every code is documented at [kura.bentopdf.com/docs/rejections](https://kura.bentopdf.com/docs/rejections).

### version()

Returns the engine name and version, for example `BentoPDF Kura Engine 1.1.0`.

## Blocking and worker threads

Conversion runs synchronously inside your process once the module is loaded; a large document can hold the event loop for several seconds. In a server, run it inside a `worker_thread`:

```js
// worker.js
import { parentPort, workerData } from 'node:worker_threads';
import { convert } from 'kura-pdf';

const result = await convert(new Uint8Array(workerData.input), workerData.level, workerData.options);
parentPort.postMessage(result, [result.pdf.buffer]);
```

That keeps your server responsive and gives you crash isolation. For untrusted uploads at volume, the native binary from a [release](https://github.com/alam00000/bentopdf-kura/releases) in a subprocess with `PDFA_TIMEOUT` set is the more robust shape.

## License

AGPL-3.0-only. For use in proprietary products, a commercial license is available; contact us at [contact@bentopdf.com](mailto:contact@bentopdf.com). Bundled third-party components keep their own licenses; see [NOTICE.md](https://github.com/alam00000/bentopdf-kura/blob/main/NOTICE.md).

## Locked files

A document with a user password is rejected with `PASSWORD_REQUIRED` unless the password is passed. To test a password before running a conversion, for instance behind an unlock box:

```js
import { verifyPassword, convert } from 'kura-pdf';

if (await verifyPassword(bytes, password)) {
  const { pdf } = await convert(bytes, '2b', { password });
}
```

`verifyPassword` returns `true` for a file that is not encrypted at all.
