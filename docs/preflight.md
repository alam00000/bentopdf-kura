# Preflight profiles

A preflight profile is a set of checks to run against a document, optionally with repairs to apply. Kura ships 397 of them under `pdfa-engine/profiles/`, every one written by BentoPDF from public sources and generated from one script, and it accepts your own.

```bash
kura --check --profile pdfa-engine/profiles/report/report-hairlines.json in.pdf
```

```json
{"ok":true,"level":"2b","mode":"check","compliant":true,"findings":0,"issues":[],
 "analysis":[
  {"code":"CHECK_HIT","detail":"Error: Stroke thinner than 0.125 pt (11 hit(s), pages 1-8, 10-12)","severity":3,"pages":[1,2,3,4,5,6,7,8,10,11,12]}]}
```

Profile results come back under `analysis`, separate from the conformance `issues`, because a profile hit is an observation about the document rather than a standards violation. Each `CHECK_HIT` carries the severity, the check name, the hit count and every page it hit, with consecutive pages shown as ranges. A profile with repairs is applied during a conversion:

```bash
kura --level x4 --profile pdfa-engine/profiles/press/sheetfed-offset-cmyk-check-and-fix.json in.pdf out.pdf
```

Each repair that changed the file is reported as a `FIX_DONE` issue. A profile the engine cannot read is reported as `PROFILE_INVALID` with the field at fault, and nothing in it runs.

## The bundled library

| Folder | Profiles | What is in it |
|---|---|---|
| `profiles/report` | 19 | report-only profiles: hairlines, small text, rich black, white objects, invisible text, images, spot colours, overprint, transparency, fonts, pages, annotations, layers, ink, document health, and one that runs everything |
| `profiles/press` | 30 | one check profile and one check-and-fix profile for each print process: sheetfed offset, web offset, newspaper, gravure, flexography, screen, digital toner and inkjet, large format, packaging, labels, book text |
| `profiles/gwg` | 80 | the Ghent Workgroup 2022 workflows, written from the published specification: PDF/X-4 conformance plus each workflow's resolution, ink, hairline, text, font, colour and page requirements, including the CMYK + RGB variants, web offset with spot colours, magazine and newspaper ads, and the 18 packaging variants (offset, flexo and gravure for folding carton boxes, corrugated boxes, corrugated displays, labels, leaflets and flexible packaging, each with the processing steps that product may carry) |
| `profiles/online` | 5 | files meant for screens, downloads, email and phones |
| `profiles/archive` | 13 | conformance checks for every PDF/A level and for embedded files |
| `profiles/accessibility` | 4 | the tagged archival levels and an accessibility readiness report |
| `profiles/standards` | 15 | conformance checks for every PDF/X, PDF/E and PDF/VT flavour, and PDF/X-4 with one layer view per language |
| `profiles/print-conditions` | 96 | conversion to PDF/X-1a, X-3, X-4 and X-6, and to PDF/X-4 + PDF/A-2b or PDF/X-6 + PDF/A-4 in one file, for 16 printing conditions (FOGRA27, 28, 29, 39 and 51, GRACoL 2006 and 2013, SWOP, Japan Color 2001 to 2011), each writing the registered condition into the output intent |
| `profiles/compatibility` | 5 | save as PDF 1.3 to 1.7 without applying a standard, removing what the older version cannot hold |
| `profiles/packaging` | 4 | processing steps (ISO 19593-1): put die lines, creases, varnish, white, Braille and other technical inks on step layers by their spot colour names, prepare a file, check it and report its steps |
| `profiles/images` | 42 | resolution thresholds, compression filters, bit depths, soft masks, interpolation, pixel sizes |
| `profiles/colour` | 46 | colour spaces, spot colour limits, registration colour, rich black, total ink thresholds, overprint and knockout |
| `profiles/objects` | 50 | hairlines, small text, render modes, transparency, blend modes, alpha, path complexity, safety margins |
| `profiles/pages` | 42 | page sizes, empty and rotated pages, boxes, page counts, and the geometry repairs: boxes, bleed, clipping, rotation, scaling to standard sizes |
| `profiles/document` | 72 | file size, PDF version, encryption, damage, syntax, annotations by type, layers, output intents, fonts, halftones, transfer curves, signatures |
| `profiles/actions` | 49 | repair-only profiles: rotation, trapped flag, rendering intents, blending space, cleanup of flatness and curves, spot colour merging, overprint and knockout, flattening overprint, hairline thickening, stamps, layers by object type, transparency or overprint, flattening annotations and form fields, initial view |
| `profiles/colour-conversion` | 101 | conversion without a standard: to each of the 14 printing conditions keeping spot colours or turning them into process colour, to grey, to sRGB, from one printing condition to another, and total ink reduction to 240, 260, 280, 300, 320 or 340%, neutral RGB greys to black only, a midtone tone curve, registration to black, spot colours to CMYK keeping overprint looks, and replacing one colour with another |
| `profiles/place-content` | 7 | stamps placed on every page: date, file name, page numbers on odd and even pages, a centred text, a job name, an EAN-13 barcode and a QR code, with the text taken from variables |
| `profiles/plans` | 5 | multi-step plans: check then convert to PDF/A-2b, convert to CMYK and flatten transparency, find complex pages, add 3 mm bleed, scale to A4 |

The browser preflight tool shows a curated 45 of them plus the conversion targets. Every profile comes from `pdfa-engine/profiles/build_library.py`; the thresholds are named there, so a change to the script regenerates the library.

## Writing your own

A profile is one JSON document:

```json
{
  "kura-profile": 1,
  "name": "Sheetfed check",
  "description": "Hairlines, small text, low-resolution images and missing fonts.",
  "checks": [
    {
      "name": "Stroke thinner than 0.125 pt",
      "severity": "error",
      "all": [
        { "check": "content.stroke_width", "op": "<=", "value": 0.125 }
      ]
    },
    {
      "name": "Text smaller than 5 pt",
      "severity": "warning",
      "scope": "trim",
      "all": [
        { "check": "text.size", "op": "<", "value": "5pt" }
      ]
    },
    {
      "name": "Image below 150 ppi",
      "severity": "error",
      "all": [
        { "check": "image.resolution", "op": "<", "value": 150, "kind": "colour_or_grey" }
      ]
    },
    {
      "name": "Font not embedded",
      "severity": "error",
      "all": [
        { "check": "font.embedding", "op": "==", "value": "not_embedded" }
      ]
    },
    {
      "name": "Page is not A4",
      "severity": "warning",
      "any": [
        { "all": [{ "check": "page.box_size", "box": "trim", "dimension": "width", "op": "!=", "value": "210mm", "tolerance": 1 }] },
        { "all": [{ "check": "page.box_size", "box": "trim", "dimension": "height", "op": "!=", "value": "297mm", "tolerance": 1 }] }
      ]
    }
  ],
  "fixes": [
    { "fix": "ink.min_line_width", "width": "0.25pt", "only_where": ["Stroke thinner than 0.125 pt"] },
    { "fix": "page.set_box", "box": "trim", "from": "crop", "when": "missing" }
  ]
}
```

The top-level fields are `kura-profile` (always 1), `name`, `description`, `variables`, `shapes`, `checks`, `fixes`, `convert` and `plan`. Only `kura-profile` and `name` are required.

### Checks

A check has a unique `name`, a `severity` of `error`, `warning` or `info`, and conditions. It hits an object when every condition under `all` holds for that object, or when one of the groups under `any` holds. Optional fields:

| Field | Meaning |
|---|---|
| `description` | text added to each hit |
| `scope` | `page` (default), `trim` or `bleed`: only look at content inside that box |
| `report_when` | `{"document_hits": "> 3"}` or `{"page_hits": ">= 2"}`: report only when the count passes the test |
| `max_hits_per_page` | stop counting after this many hits on one page |
| `silent` | run the check but do not report it; useful for `only_where` and plans |

### Conditions

A condition names a check from the catalogue, an operator and a value, plus any named settings of that check:

```json
{ "check": "page.box_size", "box": "trim", "dimension": "width", "op": ">=", "value": "210mm" }
```

The operators are `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`, `is` (for true or false), `in` and `not_in` (a list of values), `contains`, `starts_with`, `ends_with`, `matches` and `finds` (regular expressions), `occurs_in`, `includes`, `excludes`, `count`, `fits_within` and `exceeds`. Every condition also takes `negate`, `case_sensitive` and `tolerance`. Lengths are in the check's unit unless the value carries one, such as `"3mm"`, `"1cm"`, `"0.5in"` or `"0.25pt"`.

Each check in the catalogue lists the operators and values it accepts and its settings with their defaults. A setting left out takes its default. An unknown check, setting, operator or value makes the whole profile invalid, so a typo never turns into a check that silently passes.

**The full list of checks and fixes is in [Profile checks and fixes](/profile-checks).** It is generated from the same catalogue the engine reads, so it always matches the engine.

### Fixes

A fix names a catalogue fix and its settings. `only_where` limits it to pages where one of the named checks hit:

```json
{ "fix": "ink.min_line_width", "width": "0.25pt", "only_where": ["Stroke thinner than 0.125 pt"] }
```

Fixes run during a conversion, never in check mode. Each one is reported as `FIX_SCHEDULED` when the run starts and `FIX_DONE` when it changed the file; `FIX_NOT_NEEDED` means its `only_where` checks found nothing.

### Variables

Any text in a profile can use `{name}` to read a variable. A variable has a `name`, and optionally a `label`, a `type` (`text`, `number`, `bool` or `choice`), `choices`, a `default` and a `script` that works the value out:

```json
{
  "kura-profile": 1,
  "name": "Stamp the job number",
  "variables": [
    { "name": "job", "label": "Job number", "type": "text", "default": "0000" }
  ],
  "fixes": [
    { "fix": "place.text", "text": "Job {job}", "size": "8pt", "anchor": "lower_left", "x": "5mm", "y": "5mm" }
  ],
  "convert": { "level": "pdfx-4" }
}
```

```bash
kura --list-vars stamp.json
kura --profile stamp.json --var job=4711 in.pdf out.pdf
```

`--var-file <json>` reads several values at once. Each value used is reported as `VARIABLE_SET`.

A `script` is a short piece of JavaScript. It can read `kura.variable(name, default)`, `kura.variables`, `kura.document` (the name, page count and page boxes) and, inside a plan, `kura.results` with the hits and pages of earlier steps. Scripts run for at most 5 seconds with 64 MB of memory and cannot reach files or the network.

### Converting

`convert` sets the target standard, so `--level` can be left out:

```json
"convert": { "level": "pdfx-4", "reconvert": true, "raster_fallback": { "resolution": 300, "pages": "problem_pages" } }
```

`profile` names the press profile for the output intent and any colour conversion, by its description (for example `"Coated FOGRA39 (ISO 12647-2:2004)"`); Kura finds it among its bundled profiles and the computer's colour folders. `pdf_version` writes the file as that PDF version without a standard. `colors` converts colour the way `--convert-colors` does: `to` (`cmyk`, `gray`, `rgb` or a profile name), `from` (the profile the document's untagged CMYK was made for), `keep_spots`, `intent`, `gray_to_black` and `device_link`.

`also` writes a second standard into the same file, such as PDF/A and PDF/X together. `reconvert` runs the conversion once more on the result when it is not yet compliant. `raster_fallback` turns pages that cannot be fixed into images, `all` of them or only the `problem_pages`, keeping the text invisible on top unless `keep_text` is false. The chosen target is reported as `PROFILE_TARGET`.

### Plans

A plan runs checks and fixes in an order you choose, and picks the next step from the result of the last one:

```json
{
  "kura-profile": 1,
  "name": "Check, then fix only what failed",
  "variables": [{ "name": "min_ppi", "type": "number", "default": 150 }],
  "checks": [
    { "name": "Thin strokes", "severity": "error", "all": [{ "check": "content.stroke_width", "op": "<", "value": 0.25 }] },
    { "name": "Low resolution", "severity": "warning", "all": [{ "check": "image.resolution", "op": "<", "value": "{min_ppi}" }] }
  ],
  "plan": {
    "steps": [
      { "id": "strokes", "do": "check", "check": "Thin strokes", "next": { "pass": "images", "error": "thicken" } },
      { "id": "thicken", "do": "fix", "fix": "ink.min_line_width", "next": { "default": "images" } },
      { "id": "images", "do": "check", "check": "Low resolution", "next": { "default": "end" } }
    ]
  }
}
```

A step's `do` is `check`, `fix`, `profile` (run another profile file), `variable` or `revert`. `next` names the step to go to after a clean result (`pass`) or after `info`, `warning` or `error` hits, with `default` as the fallback and `end` to stop. Each step is reported as `PLAN_STEP` with its result and the next step. A converting step that fails is reported as `PLAN_STEP_FAILED`, and a plan that never converts reports `PLAN_NO_CONVERSION`.

## HTML stamps

The `place.html` fix draws an HTML page onto every page of the document: slug lines, job tickets, barcodes. `template` is the path of an `.html` file, relative to the profile, or the HTML itself:

```json
{ "fix": "place.html", "template": "slug.html", "anchor": "bottom", "relative_to": "media", "y": "3mm" }
```

The page's script sees `window.kura.document` (`name`, `pageCount`, `variables` and the `inks` in use) and `window.kura.pages`, one entry per page with its `number`, `rotation` and the `mediaBox`, `trimBox`, `bleedBox`, `cropBox` and `artBox` as `{left, bottom, right, top}` in points. `kura.onPages(callback)` registers a function that is called once per page with the page and its index; it updates the page and calls `kura.printPages(1)` to stamp it. An `<object type="application/barcode">` with a `type` and a `data` param draws a barcode:

```html
<!doctype html>
<html>
<body style="margin:0;font:7pt sans-serif">
  <div><span id="name"></span>, page <span id="page"></span> of <span id="count"></span>, job <span id="job"></span></div>
  <object type="application/barcode" style="width:40mm;height:8mm">
    <param name="type" value="Code 128">
    <param name="data" id="code" value="">
  </object>
  <script>
    kura.onPages(function (page) {
      document.getElementById('name').textContent = kura.document.name;
      document.getElementById('page').textContent = page.number;
      document.getElementById('count').textContent = kura.document.pageCount;
      document.getElementById('job').textContent = kura.document.variables.job;
      document.getElementById('code').setAttribute('value', kura.document.variables.job + '-' + page.number);
      kura.printPages(1);
    });
  </script>
</body>
</html>
```

## Result codes

| Code | Meaning |
|---|---|
| `CHECK_HIT` | a check found something: `<Severity>: <check name> (<n> hit(s), pages <ranges>)` |
| `CHECK_NOT_EVALUATED` | a condition could not be evaluated for this file |
| `PROFILE_INVALID` | the profile is not valid; the detail names the field |
| `PROFILE_EMPTY` | check mode was asked for, but the profile has no checks |
| `PROFILE_TARGET` | the profile's `convert` section chose the target standard |
| `VARIABLE_SET` | a variable received a value |
| `FIX_SCHEDULED` | a fix will run during the conversion |
| `FIX_DONE` | a fix changed the document |
| `FIX_NOT_NEEDED` | a fix's `only_where` checks found nothing |
| `FIX_UNKNOWN` | the fix is not in the catalogue |
| `ICC_PROFILE_MISSING` | a named ICC profile was not found; the conversion is refused |
| `PLAN_STEP`, `PLAN_STEP_FAILED`, `PLAN_NO_CONVERSION` | plan progress, as above |

`kura --list-profiles <dir>` lists every profile in a folder with its name and description.

## In the other surfaces

The npm package and the browser build take the profile text as `profile` in the options of `convert()` or `check()`. The C API takes it as `preflight_profile`, with variable values as a JSON object in `profile_vars`. The HTTP API takes a bundled profile's id as `profile=` or your own as a `profile` field.

## Provenance

The library was written by BentoPDF from three public sources: the Ghent Workgroup 2022 specifications for the `gwg` folder, the ISO standards Kura implements for the conformance checks, and standard prepress practice for the thresholds in the print workflows. No third-party profile files were used, and the generator script is the single source of every profile, so the pedigree of each one is visible in the repository. Profiles that reference a Ghent Workgroup specification implement the published requirements; they are not certified by the Ghent Workgroup.
