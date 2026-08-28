# kura-pdf

[Kura](https://github.com/alam00000/bentopdf-kura) is the open source PDF standards and preflight engine by [BentoPDF](https://www.bentopdf.com): it converts any PDF to PDF/A, PDF/UA, PDF/X, PDF/E or PDF/VT, runs print preflight with 396 bundled profiles, checks documents against those standards, signs them, and builds Factur-X, ZUGFeRD, XRechnung and Order-X e-invoices.

This package is the native engine for Node.js. Full documentation lives at [kura.bentopdf.com/docs](https://kura.bentopdf.com/docs/).

## Install

```bash
npm install kura-pdf
```

Node 22 or newer, ESM only. The engine binary arrives automatically through a platform package (`@bentopdf/kura-pdf-darwin-arm64`, `@bentopdf/kura-pdf-linux-x64` or `@bentopdf/kura-pdf-win32-x64`); npm installs the one matching your machine. To use a binary you built yourself, set `KURA_BIN` to its path and it takes precedence.

On platforms without a native package, or when you cannot ship native binaries at all, use [`kura-pdf-wasm`](https://www.npmjs.com/package/kura-pdf-wasm) instead: the same engine compiled to WebAssembly with the same API, running anywhere Node runs and in browsers, several times slower and without raster flattening, signing and OCR.

## Quick start

```js
import { readFile, writeFile } from 'node:fs/promises';
import { convert } from 'kura-pdf';

const input = await readFile('input.pdf');
const result = await convert(input, '2b');

await writeFile('output.pdf', result.pdf);
for (const issue of result.issues) console.log(`${issue.code}: ${issue.detail}`);
```

The package also installs the `kura` command, the native CLI itself:

```bash
npx kura --level 2b input.pdf output.pdf
npx kura --level 2a --ua input.pdf output.pdf
npx kura --check --level 2b input.pdf
npx kura --einvoice invoice.xml input.pdf output.pdf
npx kura --sign key.p12 --sign-password secret --level 2b input.pdf output.pdf
npx kura --level 2b -r -d out/ inbox/
npx kura --help
```

Exit status: 0 on success, 1 when check mode found findings, 2 when the input was rejected, 3 on timeout, 64 on a usage error.

## API

Every call runs the engine in a subprocess, so nothing blocks your event loop and hostile input can only take down that process.

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
  imageMaxPpi?: number;         // downsample images above this resolution
  attachXml?: Uint8Array | string; // e-invoice payload
  attachXmlName?: string;
  facturxProfile?: string;
  embedSource?: boolean;        // attach the input document as the source
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
  sign?: { p12: Uint8Array | string; password?: string; name?: string; reason?: string; location?: string };
  ocr?: boolean;                // run tesseract on image-only pages
  ocrEngine?: string;           // path to tesseract
  fontFolder?: string;          // extra fonts for substitution
  timeoutMs?: number;           // time limit for this call
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

### verifyPassword(input, password)

Returns whether the password opens the file, without converting it; `true` for a file that is not encrypted at all.

### version()

Returns the engine name and version, for example `BentoPDF Kura Engine 1.1.0`.

### binaryPath()

Returns the path of the engine binary this package will run, or `null` when none is available for the platform.

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

`KuraError` carries `code`, `suggestedLevel` when one exists, and the `issues` the engine recorded before it stopped. `ENGINE_MISSING` means no binary is available for this platform, `TIMEOUT` that `timeoutMs` ran out. Every rejection code is documented at [kura.bentopdf.com/docs/rejections](https://kura.bentopdf.com/docs/rejections).

## License

AGPL-3.0-only. For use in proprietary products, a commercial license is available; contact us at [contact@bentopdf.com](mailto:contact@bentopdf.com). Bundled third-party components keep their own licenses; see [NOTICE.md](https://github.com/alam00000/bentopdf-kura/blob/main/NOTICE.md).
