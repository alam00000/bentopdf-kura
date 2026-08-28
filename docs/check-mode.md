# Check mode

```bash
kura --check --level 2b in.pdf
```

Check mode runs the entire detection pipeline against a document, writes nothing, and reports what would have to change for the input to conform. It is the same engine and the same passes as a conversion, with the output discarded.

```json
{"file":"in.pdf","ok":true,"level":"2b","engine":"BentoPDF Kura Engine 1.1.0",
 "mode":"check","compliant":false,"findings":2,"issues":[
  {"code":"OUTPUT_INTENT_ADDED","detail":"added sRGB PDF/A output intent","fixed":true},
  {"code":"FONT_SUBSTITUTED","detail":"embedded LiberationSans as substitute for /Helvetica","fixed":true}]}
```

`compliant` is true when the document already conforms. `findings` counts the repairs a conversion would make, and `issues` lists them. Exit status is 0 when compliant, 1 when there are findings, and 2 when the file cannot be read at all, so it drops into a shell pipeline like any other preflight tool.

## What counts as a finding

A finding is a repair the converter would apply, minus the three normalizations it performs on every document regardless: `XMP_REBUILT`, `CONTENT_FILTERED` and `OUTPUT_INTENT_PRESENT`. Those three are not evidence of a defect, so a document that needs nothing else reports `findings: 0` and `compliant: true`.

Findings marked `(visual risk)` are repairs that need `--allow-visual-risk` on a real conversion.

## What it cannot see

Check mode sees exactly what the converter can repair. That has a consequence worth stating plainly:

- It never reports a false finding. Measured against veraPDF over 568 PDF/A-1b files it agrees on every one, and over 970 PDF/A-2b files it agrees on 954.
- It is blind to any defect the converter itself does not know how to fix. Every one of the 16 disagreements above is a file veraPDF rejects that Kura does not know how to repair.

If a pass could not finish, because a content stream or metadata packet could not be read, the report carries a `SCAN_INCOMPLETE` finding rather than staying silent. Treat it as "unknown", not "clean".

So: use check mode as a preflight, to decide whether a document needs conversion and to see what conversion would do. Use an independent validator for certification.

## Check-only flavours

`x4p`, `x5g`, `x5n`, `x5pg`, `x6n`, `x6p` and `vt2` are PDF/X and PDF/VT flavours that reference an external press profile rather than embedding one. Kura validates against them but does not produce them, so they are only accepted with `--check`.

## Idempotence

Converting a document and then checking the result always reports `compliant: true` with zero findings. This is asserted in the project's test lanes for every level.

## In the other surfaces

The npm package exposes it as [`check()`](/npm#check-input-level-options); the C API as `verify_only` on `kura_options`; the browser build as `{ check: true }` in the options object.
