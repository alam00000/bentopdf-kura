# The standards

Kura covers 25 conformance targets across five families. This page says what each one is for and what the engine does to reach it. Every target is selected with `--level` (or the second argument of `convert()`), and PDF/UA is a flag layered on top.

## PDF/A: archival

PDF/A (ISO 19005) is the standard for documents that must still open and read correctly decades from now. Every font is embedded, every colour is defined, metadata is complete and consistent, and anything that depends on the outside world, such as scripts, external links to fonts, or encryption, is forbidden.

| Level | Based on | Attachments | Meaning of the letter |
|---|---|---|---|
| `1b`, `1a` | PDF 1.4 | none | |
| `2b`, `2u`, `2a` | PDF 1.7 | only PDF/A files | |
| `3b`, `3u`, `3a` | PDF 1.7 | any file | |
| `4`, `4f`, `4e` | PDF 2.0 | `4f`: any file; `4e`: any file plus 3D and rich media | |

The letter is the conformance level: **b** means the file is visually reproducible, **u** adds that all text has a Unicode mapping so it is searchable and copyable, and **a** adds full structural tagging for accessibility. Part 4 has no letters; `4` is roughly the old `2u`, `4f` requires an embedded file, and `4e` is the engineering variant.

What conversion does: strips encryption, scripting, forbidden actions and annotations; embeds metric-compatible substitutes for missing fonts and repairs width tables, encodings and Unicode maps on embedded ones; adds an output intent and default colour spaces; rebuilds the XMP metadata from the document information; and, at part 1, flattens transparency and transcodes JPEG 2000, since PDF 1.4 has neither.

**Which to pick.** `2b` for almost everything. `2u` when you need searchable text guaranteed. `3b` when the document must carry a source file, spreadsheet or XML alongside the pages. `1b` only when a recipient demands it, because part 1 forbids transparency and Kura has to rasterize any page that uses it. `4` when your workflow is on PDF 2.0.

## PDF/UA: accessible

PDF/UA (ISO 14289) makes a document usable with assistive technology: every piece of content is tagged with its role, reading order is defined, images carry alternative text, and the document declares its language.

It is not a level of its own in Kura but a flag, `--ua`, applied on top of a PDF/A level. On parts 1 to 3 it produces PDF/UA-1; on part 4 it produces PDF/UA-2. The output validates against both the chosen PDF/A profile and the PDF/UA profile.

What conversion does: builds a structure tree from scratch when the document has none, tagging paragraphs, figures, links and form fields; repairs one that exists, merging split headings, numbering lists and marking decorative content as artifacts; sets the document language and title; and adds alternative text and structure elements for every annotation.

Machine conformance is guaranteed. The semantic depth of an automatically built tree, real headings and tables and reading order inferred from layout, is what every tool in this class leaves to assisted remediation.

## PDF/X: print

PDF/X (ISO 15930) is what printers ask for: a file whose colour is fully specified, whose page boxes are declared, and which carries nothing a press cannot use.

| Level | Colour | Transparency | Notes |
|---|---|---|---|
| `x1a` | CMYK and spot only | not allowed | the safest hand-off; Kura converts every RGB, Lab and calibrated colour to CMYK through ICC |
| `x3` | colour-managed; RGB kept under an output intent | not allowed | |
| `x4` | colour-managed | allowed | the modern default for print |
| `x6` | colour-managed, PDF 2.0 | allowed | |

What conversion does: adds the output intent and, for X-1a, converts images, vector operators, shadings, patterns, indexed lookups and separation alternates to CMYK through Little CMS; synthesizes TrimBox and BleedBox and repairs boxes that fall outside their parent; declares trapping; strips actions, forms and annotations that a press cannot honour.

The output intent defaults to a bundled CMYK profile with the identifier `Custom`. For production work, supply your press characterization with `--output-condition`, `--output-condition-info` and `--registry`, and the profile itself with `--dest-profile`.

## PDF/E: engineering

PDF/E-1 (ISO 24517) is for technical drawings and models: it keeps 3D annotations in U3D form, keeps attachments, and forbids scripting.

## PDF/VT: variable-data print

PDF/VT (ISO 16612) is PDF/X-4 plus a record structure for personalized print runs: which pages belong to which recipient. `vt1` and `vt3` add a document-part hierarchy; give the record boundaries with `--vt-records "1-3,4-6,…"` and Kura builds one part per record. Without records the whole document is reported as a single part.

## Check-only targets

Seven flavours reference an external press profile rather than embedding one: `x4p`, `x5g`, `x5n`, `x5pg`, `x6n`, `x6p` and `vt2`. Kura validates documents against them with `--check` but does not produce them, because producing one means asserting a press characterization the engine cannot know.

## E-invoices

Factur-X, ZUGFeRD, XRechnung and Order-X are not levels but a use of PDF/A-3: an invoice PDF with the machine-readable XML embedded and declared in the metadata. Kura reads the guideline out of the XML and derives everything the standard pins to it. See [E-invoices](/e-invoices).

## JPEG input

A bare JPEG file is accepted at every level: Kura wraps it as a single-page PDF at 300 dpi and converts that, which is how a scan becomes an archival document in one step.

## Known limitations

- At PDF/A-1 and PDF/X-1a/X-3, vector transparency is flattened by rasterizing the affected page. A build without PDFium cannot do that and rejects the document with `TRANSPARENCY_P1` and a suggested level instead.
- PDF/A-1 with both RGB and CMYK vector content under one output intent is rejected with a suggested level; only the CMYK-only path converts at part 1.
- JBIG2 images are passed through, which every part permits; they are not transcoded.
- The structure tree Kura builds for PDF/UA is machine-valid and semantically minimal. Real headings, tables and reading order inferred from layout are the assisted-remediation step every tool in this class leaves to a person.
- The signature applied by `--sign` is a basic PKCS#7 detached signature, not PAdES.
