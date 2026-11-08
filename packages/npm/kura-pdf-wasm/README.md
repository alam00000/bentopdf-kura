# kura-pdf-wasm

[Kura](https://github.com/alam00000/bentopdf-kura) is the open source PDF standards and preflight engine by [BentoPDF](https://www.bentopdf.com): it converts any PDF to PDF/A, PDF/UA, PDF/X, PDF/E or PDF/VT, runs print preflight with 685 bundled profiles, checks documents against those standards, and builds Factur-X, ZUGFeRD, XRechnung and Order-X e-invoices.

This package is the engine compiled to WebAssembly. No native binaries, no postinstall downloads; it runs on any platform Node 22 or newer runs on, and in browsers. On Linux x64, macOS arm64 and Windows x64, [`kura-pdf`](https://www.npmjs.com/package/kura-pdf) installs the native engine instead: the same API, several times faster, with raster flattening, signing and OCR.

Full documentation lives at [kura.bentopdf.com/docs](https://kura.bentopdf.com/docs/).

## Install

```bash
npm install kura-pdf-wasm
```

## Quick start

```js
import { readFile, writeFile } from 'node:fs/promises';
import { convert } from 'kura-pdf-wasm';

const input = new Uint8Array(await readFile('input.pdf'));
const result = await convert(input, '2b');

await writeFile('output.pdf', result.pdf);
for (const issue of result.issues) console.log(`${issue.code}: ${issue.detail}`);
```

The package also installs a `kura-wasm` command with the same JSON report and exit codes as the native CLI. It takes the conversion, check, PDF/UA, output intent, default profile, preflight profile, e-invoice, attachment, rasterizing and folder flags; colour conversion, `--pdf-version`, `--var`, the invoice readers and `kura tool` need the native `kura` (or the API below, which takes the colour and version options). The [CLI page](https://kura.bentopdf.com/docs/cli#the-kura-wasm-command) lists exactly what `kura-wasm` accepts.

```bash
npx kura-wasm --level 2b input.pdf output.pdf
npx kura-wasm --level 2a --ua input.pdf output.pdf
npx kura-wasm --check --level 2b input.pdf
npx kura-wasm --einvoice invoice.xml input.pdf output.pdf
npx kura-wasm --level 2b -r -d out/ inbox/
npx kura-wasm --help
```

The output path is optional: left out, the result is written next to the input as `<input>.<level>.pdf`. Exit status: 0 on success, 1 when check mode found findings, 2 when the input was rejected, 64 on a usage error.

## API

### convert(input, level, options?)

Converts one document. Returns a `Promise<KuraResult>`; throws `KuraError` when the document is rejected.

```ts
type Level = '1b' | '1a' | '2b' | '2u' | '2a' | '3b' | '3u' | '3a' | '4' | '4f' | '4e'
           | 'x1a' | 'x3' | 'x4' | 'x6' | 'e1' | 'vt1' | 'vt3' | 'none';

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
  convertColors?: 'cmyk' | 'gray' | 'rgb' | Uint8Array; // convert colour through ICC profiles
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
  vars?: Record<string, string | number | boolean>; // values for the profile's variables
  analyze?: boolean;            // add the document census to `analysis`
  pdfVersion?: '1.3' | '1.4' | '1.5' | '1.6' | '1.7' | '2.0'; // with level 'none'
  now?: string;                 // a fixed PDF date such as 'D:20260101120000Z' for the dates Kura writes
}

interface KuraResult {
  pdf: Uint8Array;
  level: string;
  engine: string;
  issues: { code: string; detail: string; fixed: boolean }[];
  analysis: { code: string; detail: string }[];
}
```

`level` `'none'` applies only the options (a colour conversion, a profile's fixes, a PDF version) without a standard. An empty level `''` with a `profile` lets the profile's `convert` section choose the target. `convertColors` takes `cmyk`, `gray`, `rgb` or the bytes of an ICC profile; profile names cannot be looked up in this build.

```js
const gray = await convert(input, 'none', { convertColors: 'gray' });
const profile = await readFile('sheetfed-offset-cmyk-check-and-fix.json', 'utf8');
const fixed = await convert(input, 'x4', { profile });
```

The bundled preflight profiles are in the repository under [`pdfa-engine/profiles/`](https://github.com/alam00000/bentopdf-kura/tree/main/pdfa-engine/profiles); see [Preflight profiles](https://kura.bentopdf.com/docs/preflight).

### check(input, level, options?)

Runs the whole detection pipeline and reports what a conversion would change, without producing a file. Also accepts the check-only flavours `x4p`, `x5g`, `x5n`, `x5pg`, `x6n`, `x6p` and `vt2`.

```js
import { check } from 'kura-pdf-wasm';

const report = await check(input, '2b');
console.log(report.compliant, report.findings, report.issues);
```

### Errors

```js
import { convert, KuraError } from 'kura-pdf-wasm';

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

### version(), load(), LEVELS, CHECK_ONLY_LEVELS

`version()` returns the engine name and version, for example `BentoPDF Kura Engine 1.3.0`. `load()` loads the WebAssembly module ahead of the first call; otherwise the first call loads it. `LEVELS` and `CHECK_ONLY_LEVELS` are the level names `convert()` and `check()` accept.

### Errors besides rejections

`BAD_LEVEL`, `BAD_INPUT` (input not a `Uint8Array` or `ArrayBuffer`), `BAD_OPTION` (an option out of range), `ENGINE_LOAD` (the module failed to load) and `ENGINE_ERROR`.

## Blocking and worker threads

Conversion runs synchronously inside your process once the module is loaded; a large document can hold the event loop for several seconds. In a server, run it inside a `worker_thread`:

```js
// worker.js
import { parentPort, workerData } from 'node:worker_threads';
import { convert } from 'kura-pdf-wasm';

const result = await convert(new Uint8Array(workerData.input), workerData.level, workerData.options);
parentPort.postMessage(result, [result.pdf.buffer]);
```

That keeps your server responsive and gives you crash isolation. For untrusted uploads at volume, the native binary from a [release](https://github.com/alam00000/bentopdf-kura/releases) in a subprocess with `PDFA_TIMEOUT` set is the more robust shape.

## License

AGPL-3.0-only. For use in proprietary products, commercial licenses are available as [annual subscriptions from $99/year](https://www.bentopdf.com/kura#pricing), under the [Kura Commercial License](https://github.com/alam00000/bentopdf-kura/blob/main/COMMERCIAL-LICENSE.md); for OEM terms, contact us at [contact@bentopdf.com](mailto:contact@bentopdf.com). Bundled third-party components keep their own licenses; see [NOTICE.md](https://github.com/alam00000/bentopdf-kura/blob/main/NOTICE.md).

## Locked files

A document with a user password is rejected with `PASSWORD_REQUIRED` unless the password is passed. To test a password before running a conversion, for instance behind an unlock box:

```js
import { verifyPassword, convert } from 'kura-pdf-wasm';

if (await verifyPassword(bytes, password)) {
  const { pdf } = await convert(bytes, '2b', { password });
}
```

`verifyPassword` returns `true` for a file that is not encrypted at all.
