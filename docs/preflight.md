# Preflight profiles

A preflight profile is a set of checks to run against a document, optionally with repairs to apply. Kura ships 396 of them under `pdfa-engine/profiles/`, every one written by BentoPDF from public sources and generated from one script, and it accepts your own.

```bash
kura --check --level 2b --profile pdfa-engine/profiles/report/report-hairlines.json in.pdf
```

```json
{"ok":true,"level":"2b","mode":"check","compliant":true,"findings":0,"issues":[],
 "analysis":[
  {"code":"PROFILE_HIT","detail":"Error: Stroke thinner than 0.125 pt (11 hit(s), pages 1-8, 10-12)"}]}
```

Profile results come back under `analysis`, separate from the conformance `issues`, because a profile hit is an observation about the document rather than a standards violation. Each `PROFILE_HIT` carries the severity, the check name, the hit count and every page it hit, with consecutive pages shown as ranges. A profile with repairs is applied during a conversion:

```bash
kura --level x4 --profile pdfa-engine/profiles/press/sheetfed-offset-cmyk-check-and-fix.json in.pdf out.pdf
```

Each repair that ran is reported as a `PROFILE_FIX_DONE` issue; one the engine could not run is reported as `PROFILE_FIX_UNSUPPORTED` and skipped, never silently.

## The bundled library

| Folder | Profiles | What is in it |
|---|---|---|
| `profiles/report` | 18 | report-only profiles: hairlines, small text, rich black, white objects, invisible text, images, spot colours, overprint, transparency, fonts, pages, annotations, layers, ink, document health, and one that runs everything |
| `profiles/press` | 30 | one check profile and one check-and-fix profile for each print process: sheetfed offset, web offset, newspaper, gravure, flexography, screen, digital toner and inkjet, large format, packaging, labels, book text |
| `profiles/gwg` | 24 | the Ghent Workgroup 2022 workflows, written from the published specification: PDF/X-4 conformance plus each workflow's resolution, ink, hairline, text, font, colour and page requirements |
| `profiles/online` | 5 | files meant for screens, downloads, email and phones |
| `profiles/archive` | 13 | conformance checks for every PDF/A level and for embedded files |
| `profiles/accessibility` | 3 | the tagged archival levels and an accessibility readiness report |
| `profiles/standards` | 14 | conformance checks for every PDF/X, PDF/E and PDF/VT flavour |
| `profiles/images` | 37 | resolution thresholds, compression filters, bit depths, soft masks, interpolation, pixel sizes |
| `profiles/colour` | 46 | colour spaces, spot colour limits, registration colour, rich black, total ink thresholds, overprint and knockout |
| `profiles/objects` | 50 | hairlines, small text, render modes, transparency, blend modes, alpha, path complexity, safety margins |
| `profiles/pages` | 40 | page sizes, empty and rotated pages, boxes, page counts, and the geometry repairs: boxes, bleed, clipping, rotation, scaling to standard sizes |
| `profiles/document` | 72 | file size, PDF version, encryption, damage, syntax, annotations by type, layers, output intents, fonts, halftones, transfer curves, signatures |
| `profiles/actions` | 44 | repair-only profiles: rotation, trapped flag, rendering intents, blending space, cleanup of flatness and curves, spot colour merging, overprint and knockout, hairline thickening, stamps, layers, initial view |

The browser preflight tool shows a curated 45 of them plus the conversion targets. Every profile comes from `pdfa-engine/profiles/build_library.py`; the thresholds are named there, so a change to the script regenerates the library.

## Writing your own

A profile is JSON:

```json
{
  "kura-profile": 1,
  "name": "Report hairlines",
  "description": "Finds strokes too thin to print reliably.",
  "checks": [
    {
      "name": "Stroke thinner than 0.125 pt",
      "severity": "error",
      "all": [
        { "prop": "stroke.width", "op": "<=", "value": 0.125 }
      ]
    }
  ],
  "builtins": [
    { "name": "imageResolutionBelow", "severity": "warning", "params": { "ppi": 300 } },
    { "name": "conformsTo", "severity": "error", "level": "x4" }
  ],
  "fixes": [
    { "op": "increaselinewidth", "params": ["0.25", "", "pt"] }
  ]
}
```

A check fires when every condition under `all` holds, or when any group under `any` holds (`"any": [{"all": [...]}, {"all": [...]}]`). Conditions compare a property with `op` against a value: `<`, `<=`, `==`, `!=`, `>=`, `>`, `contains`, `begins`, `ends`. Booleans compare with `true` or `false`; lengths are in points unless the value is a string with a unit, such as `"3mm"`, `"1cm"` or `"0.5in"`. `severity` is `error`, `warning` or `info`, and an optional `scope` of `page`, `trim` or `bleed` limits a check to objects inside that box.

Built-in checks look at the document as a whole and take their thresholds under `params`; `conformsTo` and `embeddedFilesConformTo` take a `level` instead. Repairs are operations with positional parameters, applied during a conversion rather than a check.

## Properties

### Graphics state

| Property | Meaning |
|---|---|
| `stroke.width` | stroke line width in points |
| `stroke.overprint` | stroke overprint is on |
| `stroke.transparency` | stroke is drawn with transparency |
| `stroke.alpha` | stroke constant alpha, 0 to 1 |
| `stroke.totalInk` | total ink of the stroke colour in percent |
| `stroke.inkCount` | inks with a non-zero value in the stroke colour |
| `fill.overprint` | fill overprint is on |
| `fill.transparency` | fill is drawn with transparency |
| `fill.alpha` | fill constant alpha, 0 to 1 |
| `fill.totalInk` | total ink of the fill colour in percent |
| `fill.processInk` | total process ink of the fill colour in percent |
| `fill.inkCount` | inks with a non-zero value in the fill colour |
| `gstate.overprint` | overprint is on in the graphics state |
| `gstate.overprintModeIllustrator` | overprint mode 1 is set |
| `gstate.transparency` | the graphics state uses transparency |
| `gstate.blendMode` | blend mode name, such as Normal or Multiply |
| `gstate.blendColorspace` | blending colour space of the enclosing group |
| `gstate.inTransparencyGroup` | object sits inside a transparency group |
| `gstate.hasSoftMask` | a soft mask is set |
| `gstate.flatness` | flatness tolerance |
| `gstate.hasBlackPointCompensation` | black point compensation is set |

### Colour

| Property | Meaning |
|---|---|
| `paint.inkCount` | inks with a non-zero value in the paint colour |
| `paint.maxInkPercent` | highest single ink value in percent |
| `paint.isWhite` | paint is white |
| `paint.isBlackOnly` | paint uses the black plate only |
| `paint.richBlackCmyPercent` | CMY added under black, in percent |
| `paint.isRgb` | DeviceRGB |
| `paint.isCmyk` | DeviceCMYK |
| `paint.isGray` | DeviceGray |
| `paint.isIccBased` | ICC-based colour |
| `paint.isLab` | Lab colour |
| `paint.isCalibrated` | CalRGB or CalGray |
| `paint.isDeviceIndependent` | any CIE-based colour space |
| `paint.isSpot` | a spot colour |
| `paint.isSeparation` | a Separation colour space |
| `paint.isPattern` | a pattern |
| `paint.isRegistration` | the registration colour |
| `paint.spotName` | spot colour name |
| `paint.spotNameHasPantoneSuffix` | spot name ends in a Pantone suffix |
| `paint.colorspaceName` | colour space family name |
| `paint.altColorspaceName` | alternate colour space of a spot |
| `paint.componentCount` | number of colour components |
| `paint.nonZeroCmykCount` | non-zero CMYK components |
| `paint.is100Black` | exactly 100% black |
| `paint.blackPercent` | black component in percent |
| `paint.cmykOnly` | process inks only, no spots |
| `paint.spotOnly` | spot inks only, no process |
| `paint.usesIccCmyk` | ICC-based CMYK |
| `paint.usesIccRgb` | ICC-based RGB |
| `paint.processColourAsSpot` | a process ink defined as a spot |
| `paint.processColoursAsDeviceN` | process inks defined through DeviceN |
| `paint.deviceNColorants` | number of DeviceN colourants |

### Text

| Property | Meaning |
|---|---|
| `text.size` | rendered text size in points |
| `text.isInvisible` | text in invisible render mode that is not a clip |
| `text.renderMode` | text rendering mode, 0 to 7 |
| `text.isStroked` | text is stroked |
| `text.isClippingPath` | text is used as a clipping path |
| `text.hasUnicode` | the glyph maps to Unicode |
| `text.glyphUndefined` | the glyph is missing from the font |
| `text.glyphHasContour` | the glyph has an outline |
| `text.glyphIsWhitespace` | the glyph is whitespace |

### Fonts

| Property | Meaning |
|---|---|
| `font.embedded` | font program is embedded |
| `font.notEmbedded` | font program is not embedded |
| `font.name` | base font name |
| `font.isType3` | Type 3 font |
| `font.isTrueType` | TrueType font |
| `font.isCid` | CID-keyed font |
| `font.subsetComplete` | the subset holds every glyph the text uses |
| `font.unicodeComplete` | every character maps to Unicode |
| `font.invalid` | font program does not parse |
| `font.notdefUsed` | a character falls back to .notdef |
| `font.bitmapOnly` | bitmap-only embedding |
| `font.restrictedLicense` | licence forbids embedding |
| `font.canBeEmbedded` | licence permits embedding |
| `font.notSubset` | font is embedded whole |
| `font.widthsMatch` | declared widths match the program |
| `font.nameUnique` | font name is unique in the file |

### Images

| Property | Meaning |
|---|---|
| `image.ppi` | effective resolution in pixels per inch |
| `image.bitsPerComponent` | bits per colour component |
| `image.bpc` | same as image.bitsPerComponent |
| `image.filter` | compression filter name, such as DCTDecode |
| `image.width` | width in pixels |
| `image.height` | height in pixels |
| `image.interpolate` | interpolation flag is true |
| `image.hasInterpolateEntry` | an interpolation entry is present |
| `image.hasSoftMask` | a soft mask is attached |
| `image.invalid` | the image cannot be decoded |

### Content objects

| Property | Meaning |
|---|---|
| `content.isImage` | object is an image |
| `content.isImageMask` | object is a stencil mask |
| `content.isBitmap` | 1-bit image or mask |
| `content.isText` | object is text |
| `content.isLine` | object is a line |
| `content.isStroked` | object is stroked |
| `content.isFilled` | object is filled |
| `content.isFilledAndStroked` | object is both filled and stroked |
| `content.isStrokedOnly` | stroked but not filled |
| `content.isShading` | object is a shading |
| `content.outsideMediaBox` | entirely outside the media box |
| `content.outsideBleedBox` | entirely outside the bleed box |
| `content.insideTrimAndArtBox` | inside both trim and art box |
| `content.distanceFromTrimBox` | distance from the trim box in points |
| `content.distanceInsideTrimBox` | distance to the trim edge from inside, in points |
| `content.pathNodes` | number of nodes in the path |
| `content.unknownOperator` | operator no PDF version defines |
| `content.emptyVector` | path that neither fills nor strokes |

### Pages

| Property | Meaning |
|---|---|
| `page.allHaveMediaBox` | every page has a media box |
| `page.hasMediaBox` | page has a media box |
| `page.hasCropBox` | page has a crop box |
| `page.cropEqualsMedia` | crop box equals media box |
| `page.isRotated` | page has a rotation |
| `page.isEmpty` | page paints nothing |
| `page.number` | page number |
| `page.inkCoverage` | effective ink coverage of the page in percent |
| `page.singleImage` | page holds one image only |
| `page.contentCompressed` | content stream is compressed |
| `page.hasOutputIntent` | page carries its own output intent |
| `page.usesPlates` | page uses the named plates |
| `page.transparencyGroupHasTransparency` | page group actually contains transparency |

### Document

| Property | Meaning |
|---|---|
| `doc.pages` | number of pages |
| `doc.fileSizeBytes` | file size in bytes |
| `doc.pdfVersion` | PDF version number |
| `doc.plates` | number of plates |
| `doc.spotPlates` | number of spot plates |
| `doc.pagesSameSize` | all pages share size and orientation |
| `doc.dataAfterEof` | bytes follow the final %%EOF |
| `doc.spotNamesEquivalent` | two spot names are the same ink |
| `doc.spotNamesNotIdentical` | equivalent spot names that are not identical |
| `doc.spotRepresentationsInconsistent` | a spot is defined differently in two places |
| `doc.xmpIsPlainText` | XMP packet is plain text |
| `doc.requiresPdf20` | file declares a PDF 2.0 requirement |
| `doc.namesUtf8` | name objects are valid UTF-8 |
| `doc.hexStringInvalid` | a hex string has invalid characters |
| `docinfo.creator` | Creator field |
| `docinfo.producer` | Producer field |
| `docinfo.trapped` | Trapped field, True or False |
| `docinfo.hasPdfxFields` | PDF/X identification fields present |

### Output intents

| Property | Meaning |
|---|---|
| `outputIntent.count` | number of output intents |
| `outputIntent.hasProfile` | output intent embeds an ICC profile |
| `outputIntent.isPdfx` | a PDF/X output intent |
| `outputIntent.isPdfa` | a PDF/A output intent |
| `outputIntent.pdfxEntries` | number of PDF/X output intents |
| `outputIntent.icc.colorspace` | colour space of the output profile |
| `outputIntent.icc.version` | ICC version of the output profile |

### Annotations

| Property | Meaning |
|---|---|
| `annot.type` | annotation subtype, such as Link |
| `annot.isType` | annotation is of the given subtype |
| `annot.prints` | print flag is set |
| `annot.hasOpacity` | an opacity value is set |
| `annot.opacity` | opacity value |
| `annot.insideBleedOrTrim` | annotation lies inside bleed or trim box |
| `annot.unknownType` | subtype the specification does not define |

### Layers

| Property | Meaning |
|---|---|
| `layers.onLayer` | content belongs to a layer |
| `layers.visible` | the layer is on by default |
| `layers.hasConfigs` | alternate layer configurations exist |
| `layers.processingSteps` | processing-steps metadata |
| `layers.hasProcessingSteps` | processing steps are present |

### Other

| Property | Meaning |
|---|---|
| `halftone.hasOrigin` | halftone dictionary fixes an origin |
| `icc.version` | ICC profile version |
| `icc.colorspace` | ICC profile colour space |
| `signature.hasFields` | document has signature fields |
| `vt.hasDocumentParts` | PDF/VT document-part hierarchy present |

## Built-in checks

| Name | Parameters | Reports |
|---|---|---|
| `imageResolutionBelow` | `ppi` | colour and grayscale images below the resolution |
| `imageResolutionAbove` | `ppi` | colour and grayscale images above the resolution |
| `bitmapResolutionBelow` | `ppi` | 1-bit images below the resolution |
| `bitmapResolutionAbove` | `ppi` | 1-bit images above the resolution |
| `colourPlatesUsed` |  | objects that produce output on the cyan, magenta or yellow plates |
| `deviceIndependentColour` |  | objects painted in Lab, calibrated or ICC-based colour |
| `rgbUsed` |  | objects painted in RGB |
| `spotColoursMoreThan` | `count` | pages using more spot colours than the count |
| `spotNamesInconsistent` |  | spot colours named inconsistently |
| `fontsNotEmbedded` |  | fonts without an embedded program |
| `fontsEmbedded` |  | fonts with an embedded program |
| `type1CidFonts` |  | CID-keyed Type 1 fonts |
| `trueTypeCidFonts` |  | CID-keyed TrueType fonts |
| `openTypeFonts` |  | OpenType fonts |
| `encrypted` |  | the file is encrypted |
| `damaged` |  | the file needed repair to parse |
| `syntaxProblems` |  | structural problems found while parsing |
| `pdfVersionBelow` | `version` | the file's PDF version is below the value |
| `uncompressedImages` |  | images stored without compression |
| `pageCount` |  | the page count, always reported |
| `pagesDifferInSize` |  | pages differ in size or orientation |
| `emptyPage` |  | pages with no visible content |
| `transferCurves` |  | transfer functions in use |
| `halftones` |  | custom halftones in use |
| `postscript` |  | PostScript XObjects |
| `transparencyUsed` |  | transparency anywhere in the file |
| `hairlinesBelow` | `points` | strokes thinner than the value |
| `conformsTo` | `level` | the file does not conform to the standard; `level` takes any conformance level Kura converts to or checks, such as `2b` or `x4` |
| `embeddedFilesConformTo` | `level` | embedded PDFs that do not conform to the archival level |

## Repairs

| Operation | Parameters | Effect |
|---|---|---|
| `rotatepages` | angle | rotates every page by 90, 180 or 270 degrees |
| `removepagescaling` |  | removes the user unit |
| `scalepagesex` | width, height, unit | scales pages proportionally to fit the size |
| `setpagebox` | box, `RelativeToCropBox` (or another box), left, bottom, right, top, unit, `Always` | sets a page box from another box with offsets; only where missing unless `Always` |
| `setpageboxesbasedonmarks` |  | sets the trim box from the crop box |
| `generatebleed` | Auto or Amount, amount, unit | sets a bleed box around the trim box |
| `removeobjectsoutofbox` | box | clips content to the box |
| `removepdfuakeys` |  | removes the PDF/UA marker |
| `settitle` | IfEmpty or Always, title | sets the document title |
| `trappedkey` | true or false | sets the trapped flag |
| `setinitialviewdocumentoptions` | page mode, page layout | sets how the file opens, such as `UseOutlines` and `TwoPageRight` |
| `setinitialviewuioptions` | hide toolbar, hide menubar, hide window UI | viewer preferences, each true or false |
| `setinitialviewwindowoptions` | fit window, center window, display title | viewer preferences, each true or false |
| `settransparencyblendcs` | CMYK or sRGB | sets the page blending colour space |
| `modifyinterpolateentry` | Remove | removes the image interpolation flag |
| `removeflatness` |  | removes flatness tolerances |
| `removesmoothness` |  | removes smoothness tolerances |
| `transfercurves` |  | removes transfer functions |
| `removebg` |  | removes black generation |
| `removeucr` |  | removes undercolour removal |
| `removerenderingintents` |  | removes rendering intents |
| `setrenderingintent` | intent | sets the rendering intent on every graphics state |
| `removeunnecessarytransparencygroups` |  | drops page groups without transparent content |
| `mergespotcolornames` |  | merges spot names that differ only in spelling |
| `makecustomspotcolornamesconsistent` |  | gives every use of a spot the same definition |
| `mapspotcolors` | to, , from | renames one spot colour to another |
| `convertregistrationcolortoblack` |  | repaints registration colour as black |
| `convertnchtodevn` |  | converts NChannel to DeviceN |
| `placetext` | text or Date, , size | places text near the bottom-left of every page |
| `annotation` | selector, action | selector `All`, a subtype, `AllMultimedia` or `Unknown`; action `Remove`, `SetToNoPrint` or `MoveOutOfBleedBox` |
| `putobjectsonlayer` | name, label | wraps page content in a layer |
| `putobjpsteps` | name, label | wraps page content in a processing-steps layer |
| `dscdhdnlycntfltnvsblyrs` |  | discards hidden layers and flattens the visible ones |
| `knockoutwhite` | Text or empty | switches overprint off for white objects |
| `overprintblack` | Text or empty | switches overprint on for 100% black objects |
| `setoverprintandknockout` |  | black overprint and white knockout together |
| `increaselinewidth` | width, , unit | raises thinner strokes to the width |
| `settextrendermode` | mode | forces a text rendering mode |

## The XML dialect

Profiles in the XML preflight dialect, with `<check name="…" check_severity="…">` and `<fixup><fcfg>…</fcfg></fixup>` elements, are accepted as well and mapped onto the same engine, so a shop can bring the profiles it already has. Severity in that dialect is numbered the other way round, 0 for error; Kura normalizes it.

## In the other surfaces

The npm package and the browser build take the profile text as `profile` in the options of `convert()` or `check()`. The C API does not currently expose profiles.

## Provenance

The library was written by BentoPDF from three public sources: the Ghent Workgroup 2022 specifications for the `gwg` folder, the ISO standards Kura implements for the conformance checks, and standard prepress practice for the thresholds in the print workflows. No third-party profile files were used, and the generator script is the single source of every profile, so the pedigree of each one is visible in the repository. Profiles that reference a Ghent Workgroup specification implement the published requirements; they are not certified by the Ghent Workgroup.
