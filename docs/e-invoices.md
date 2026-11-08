# E-invoices

Factur-X, ZUGFeRD, XRechnung and Order-X are hybrid documents: a PDF/A-3 invoice a person can read, with the machine-readable XML embedded inside and declared in the metadata. Kura builds them, reads them back, and validates the container.

What it does, in one list:

- **builds** a hybrid invoice from a PDF and the invoice XML with one flag, deriving the file name, relationship and metadata from the XML ([Building one](#building-one))
- **covers** Factur-X (MINIMUM to EXTENDED), ZUGFeRD 2.x and 1.0, XRechnung, and Order-X orders, changes and responses, in CII or UBL, invoices and credit notes
- **writes** a PDF/A-3 container that passes veraPDF, from a PDF, a scanned invoice with OCR, or a JPEG scan, and can sign it in the same pass ([Scanned and signed invoices](#scanned-and-signed-invoices))
- **refuses** XML that is not an invoice or declares no profile, and warns about profiles the German mandate does not accept ([What is refused](#what-is-refused))
- **extracts** the XML from any hybrid invoice and **checks** that the PDF and the XML agree ([Reading one back](#reading-one-back))
- **summarises** an embedded invoice as a readable report in six languages, and lets preflight profiles require or inspect one ([Reports and preflight](#reports-and-preflight))
- works the same from the CLI, npm, the HTTP API, the C API and the browser ([In the other surfaces](#in-the-other-surfaces))

## Building one

```bash
kura --einvoice invoice.xml in.pdf out.pdf
```

One flag. The engine reads the guideline identifier out of the XML and derives everything the standard pins to it: the embedded file name, the attachment relationship, the XMP namespace and version. There is nothing else to configure.

| Detected guideline | Standard | Embedded as | Relationship |
|---|---|---|---|
| `urn:factur-x.eu:1p0:minimum` | Factur-X MINIMUM | `factur-x.xml` | `/Data` |
| `urn:factur-x.eu:1p0:basicwl` | Factur-X BASIC WL | `factur-x.xml` | `/Data` |
| `…:1p0:basic` | Factur-X BASIC | `factur-x.xml` | `/Alternative` |
| `urn:cen.eu:en16931:2017` | Factur-X EN 16931 | `factur-x.xml` | `/Alternative` |
| `…:1p0:extended` | Factur-X EXTENDED | `factur-x.xml` | `/Alternative` |
| `…kosit:xrechnung_3.0` | XRechnung | `xrechnung.xml` | `/Alternative` |
| `urn:ferd:…:1p0:comfort` | ZUGFeRD 1.0 | `ZUGFeRD-invoice.xml` | `/Alternative` |
| `…urn:zugferd.de:2p0:…` | ZUGFeRD 2.0 | `zugferd-invoice.xml` | `/Alternative` |
| `urn:order-x.eu:1p0:…` | Order-X | `order-x.xml` | `/Alternative` |

The guideline is read from `GuidelineSpecifiedDocumentContextParameter/ID` in a CII payload, or `cbc:CustomizationID` in a UBL one. Order-X, the same mechanism for purchase orders, sets the document type from the payload's `TypeCode`: `220` is an order, `230` an order change, `231` an order response.

`/Data` on the two header-only Factur-X profiles is required by the standard because the page carries more invoice detail than the XML does. That is also why the German mandate does not accept MINIMUM or BASIC WL as e-invoices: the structured part does not carry the full invoice. Kura still builds and validates them, and reports `EINVOICE_PROFILE_LIMITED` so nobody finds out from a rejected invoice; for the mandate, use BASIC, EN 16931 or EXTENDED. `/Source` is never emitted, because it would assert the page was rendered from the XML, which a converter cannot know.

### Container level

The level defaults to `3b`. `3b`, `3u`, `3a` and `4f` are accepted; every other level is refused with `FACTURX_REQUIRES_PDFA3`. PDF/A-4f is permitted by Factur-X 1.07 but is ahead of most recipients' tooling, so choosing it adds an `EINVOICE_PDFA4_CONTAINER` advisory and `3b` stays the default.

### Overrides

`--facturx-profile <name>` states the profile when the XML declares nothing usable. `--attach-xml-name <name>` overrides the file name. Use them only when you know the payload is right and the declaration is missing.

## What is refused

The payload is checked structurally before anything is embedded:

| Code | Why |
|---|---|
| `EINVOICE_NOT_A_DOCUMENT` | the root element is not a CII `CrossIndustryInvoice`, `CrossIndustryDocument`, `SCRDMCCBDACIOMessageStructure`, or a UBL `Invoice` or `CreditNote`; this also catches a file that is not XML |
| `EINVOICE_PROFILE_UNKNOWN` | a recognised document declares no guideline; Kura will not embed it as a guessed EN 16931 invoice |
| `EINVOICE_PROFILE_INVALID` | the declared conformance level is outside the hybrid code list |
| `FACTURX_REQUIRES_PDFA3` | the chosen level cannot carry attachments |
| `EINVOICE_PROFILE_LIMITED` | a note, not a rejection: MINIMUM and BASIC WL are valid profiles, but the German mandate only accepts profiles whose XML carries the full invoice |

This is container-level validation. Kura does not run the XSD or the EN 16931 business rules over the payload; that is what Mustang and similar validators are for, and the project tests its output against Mustang.

## Reading one back

```bash
kura --extract-invoice invoice.pdf                # XML on stdout
kura --extract-invoice invoice.pdf out.xml        # to a file
```

```bash
kura --check-invoice invoice.pdf
```

```json
{"ok":true,"einvoice":true,"standard":"Factur-X","profile":"EN 16931","documentType":"INVOICE",
 "attachment":"factur-x.xml","consistent":true,"problems":[],"warnings":[]}
```

`--check-invoice` compares what the attachment declares with what the container says: the file name, the relationship, the catalog's `/AF` array, and the XMP extension schema's file name, conformance level and document type. Exit status is 0 when consistent, 1 when there are problems, 2 when the file cannot be read.

## Scanned and signed invoices

The input does not have to be a clean PDF. A JPEG scan becomes a one-page PDF first, and `--ocr` lays a searchable text layer over scanned pages (native build, with Tesseract installed):

```bash
kura --einvoice invoice.xml scan.jpg e-invoice.pdf
kura --einvoice invoice.xml --ocr scanned.pdf e-invoice.pdf
```

`--sign <id.p12>` signs the finished e-invoice in the same pass, so the signature covers the PDF/A-3 file with the XML already inside:

```bash
kura --einvoice invoice.xml --sign id.p12 --sign-password '…' invoice.pdf e-invoice.pdf
```

## Reports and preflight

`kura tool invoice-synopsis <in.pdf> [out]` prints the key fields of an embedded invoice: format, number, dates, currency, seller and buyer with address and VAT ID, line items, taxes and totals. With an output file it writes the same as an `.html`, `.pdf`, `.xml` or `.json` report. `--report` adds the same section to any run on an e-invoice, and `--report-lang` writes it in English, German, French, Spanish, Dutch or Italian:

```bash
kura tool invoice-synopsis e-invoice.pdf summary.pdf
kura --check --level 3b --report check.html --report-lang de e-invoice.pdf
```

`--check --level 3b` checks the PDF/A-3 container with the same verdict as veraPDF. In a [preflight profile](/preflight), `einvoice.present`, `einvoice.valid`, `einvoice.relationship_valid` and `einvoice.metadata` require or inspect an invoice; see [Profile checks](/profile-checks).

## In the other surfaces

The npm package takes `attachXml` (the XML bytes), `attachXmlName` and `facturxProfile` in the options of `convert()`. The C API takes `invoice_xml`, `invoice_xml_len`, `invoice_profile` and `invoice_filename` on `kura_options`. The browser build uses the same names as the npm package.
