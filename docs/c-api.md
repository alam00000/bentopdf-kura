# C API

`libkura` exposes the engine as a small, stable C ABI for embedding in any language that can call C. Seven functions, no exceptions across the boundary, additive only within a major version. Every release attaches `libkura.a` for Linux, Windows and macOS alongside `kura.h`.

## The surface

```c
#include "kura/kura.h"

typedef struct {
  int ua;
  int allow_visual_risk;
  const char* doc_lang;
  const char* password;
  const char* output_condition_identifier;
  const char* dest_profile;
  size_t dest_profile_len;
  int verify_only;
  const unsigned char* invoice_xml;
  size_t invoice_xml_len;
  const char* invoice_profile;
  const char* invoice_filename;
  /* Added in 1.2; zero means off or unset. */
  const char* output_condition_info;
  const char* output_condition_registry;
  const char* vt_records;
  int analyze;
  int outline_fonts;
  int linearize;
  double image_max_ppi;
  double raster_dpi;
  int rasterize_all_pages;
  const char* preflight_profile;      /* profile JSON text */
  const unsigned char* embed_source;  /* attached as the source file, PDF/A-3 */
  size_t embed_source_len;
  const char* embed_source_name;
  const char* embed_source_mime;
  const char* font_folders;           /* newline-separated folders */
  const kura_ocr_page* ocr_pages;     /* words to lay down as invisible text */
  size_t ocr_page_count;
  const char* profile_vars;           /* JSON object {"name": value} for the profile's variables */
  const char* cjk_font_folder;        /* folder with NotoSans{SC,TC,JP,KR}-{Regular,Bold}.ttf */
} kura_options;

typedef struct {
  const char* text;
  double x, y, width, height;         /* PDF points, from the top-left corner of the page */
} kura_ocr_word;

typedef struct {
  int page_index;                     /* 0-based */
  const kura_ocr_word* words;
  size_t word_count;
} kura_ocr_page;

typedef struct {
  const char* code;
  const char* detail;
  int severity;      /* 1 info, 2 warning, 3 error */
  int fixed;
  const int* pages;  /* 1-based; empty = whole document */
  size_t page_count;
} kura_issue;

typedef struct {
  int ok;
  const unsigned char* pdf;
  size_t pdf_len;
  const char* error_code;
  const char* error;
  const char* suggested_level;
  int compliant;
  size_t findings;
  const kura_issue* issues;
  size_t issue_count;
  const kura_issue* analysis;
  size_t analysis_count;
} kura_result;

typedef struct {
  int ok;
  const char* error_code;
  const char* error;
  int einvoice;
  const char* standard;
  const char* profile;
  const char* document_type;
  const char* attachment;
  const unsigned char* xml;
  size_t xml_len;
  int consistent;
  const char* const* problems;
  size_t problem_count;
  const char* const* warnings;
  size_t warning_count;
} kura_invoice;

kura_result* kura_convert(const unsigned char* data, size_t size, const char* level,
                          const kura_options* options);
void kura_result_free(kura_result* result);
const char* kura_version(void);
int kura_verify_password(const unsigned char* data, size_t size, const char* password);
kura_invoice* kura_read_invoice(const unsigned char* data, size_t size, const char* password);
void kura_invoice_free(kura_invoice* invoice);
const char* kura_engine_name(void);
```

- `kura_convert` converts memory to memory. `level` is any [target](/standards) as a string, including the check-only flavours with `verify_only`; `none`, `ua1` and `ua2` are CLI-only and come back as `BAD_LEVEL`. `options` may be `NULL` for the defaults. It returns `NULL` only when the result object itself could not be allocated; every other failure, a bad level, an encrypted file, a rejected document, comes back on the result with `ok == 0` and `error_code` set.
- Zero the options struct before setting fields. A null pointer field means "not set".
- `verify_only` runs [check mode](/check-mode): `pdf` stays empty and `compliant` and `findings` are filled in.
- The `pdf` buffer is owned by the result. Copy it out before `kura_result_free`, which releases everything the result points at.
- `error_code` values are listed on [Rejection codes](/rejections); `suggested_level` is set for the rejections that have one.
- `issues` lists every finding of the run, `analysis` the census and profile hits when `analyze` or `preflight_profile` is set. Each entry carries its severity and the 1-based pages it concerns; an empty page list means the whole document.
- `preflight_profile` takes the JSON text of a profile; its fix steps apply unless `verify_only` is set. The `level` argument is always the target; a profile's `convert.level` is not applied. `profile_vars` sets its variables, for example `{"job": "4711"}`.
- The fields you are most likely to want, with their CLI equivalents: `output_condition_identifier`, `output_condition_info`, `output_condition_registry` (`--output-condition`, `--output-condition-info`, `--registry`); `dest_profile` with `dest_profile_len` (`--dest-profile`, the ICC bytes); `invoice_xml`, `invoice_profile`, `invoice_filename` (`--einvoice`, `--facturx-profile`, `--attach-xml-name`); `vt_records` (`--vt-records`); `image_max_ppi`, 0 to 10000 (`--image-max-ppi`); `raster_dpi`, 24 to 1200, 0 for the default 300 (`--raster-dpi`); `rasterize_all_pages` (`--rasterize-pages`); `outline_fonts` (`--outline-fonts`); `analyze` (`--analyze`). `linearize` writes a linearized file for fast web viewing. Values outside a range are ignored and the default is used.
- `cjk_font_folder` names the folder of the CJK font pack; when it is unset, `KURA_FONT_DIR` is used. Without either, Chinese, Japanese and Korean text that needs a substitute is handled as described under [Fonts](/cli#fonts). The world font pack (Arabic, Indic and other scripts) is not available through the C API.
- `font_folders` names folders searched, recursively, for a font file whose name matches a non-embedded font before a substitute is used.
- `ocr_pages` hands the engine recognized words in PDF points from the top-left corner of the page; they become an invisible text layer.
- `kura_verify_password` answers whether a password opens the document. `kura_read_invoice` extracts and checks an e-invoice payload; `consistent` is 0 when `problems` is non-empty.

## A complete example

```c
#include <stdio.h>
#include <stdlib.h>
#include "kura/kura.h"

int main(void) {
  FILE* f = fopen("in.pdf", "rb");
  if (!f) return 1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char* data = malloc(n);
  fread(data, 1, n, f);
  fclose(f);

  kura_options opt = {0};
  opt.ua = 1;
  opt.doc_lang = "en-US";

  kura_result* r = kura_convert(data, n, "2a", &opt);
  if (!r) return 1;
  if (!r->ok) {
    fprintf(stderr, "%s: %s\n", r->error_code, r->error);
    if (r->suggested_level) fprintf(stderr, "try level %s\n", r->suggested_level);
    kura_result_free(r);
    return 2;
  }
  FILE* out = fopen("out.pdf", "wb");
  fwrite(r->pdf, 1, r->pdf_len, out);
  fclose(out);
  printf("converted with %s %s\n", kura_engine_name(), kura_version());
  kura_result_free(r);
  free(data);
  return 0;
}
```

Compile against a release archive, which ships `libkura.a`, `libkura_raster.a` and `libpdfium.a` under `lib/`:

```bash
cc demo.c -I include -L lib -lkura -lkura_raster -lpdfium \
  -lqpdf -lfreetype -lopenjp2 -ljpeg -lpng -lz -llcms2 -lstdc++ -o demo
```

The three archives carry the engine and PDFium; qpdf, FreeType, OpenJPEG, libjpeg, libpng, zlib and Little CMS come from your system or from your own static builds of them. On macOS add `-framework CoreFoundation -framework CoreGraphics -framework AppKit` for PDFium.

## Checking, presets and colour

Check mode is `verify_only`:

```c
kura_options opt = {0};
opt.verify_only = 1;
kura_result* r = kura_convert(data, n, "2b", &opt);
if (r && r->ok) printf("compliant: %d, findings: %zu\n", r->compliant, r->findings);
for (size_t i = 0; r && i < r->issue_count; ++i) printf("%s: %s\n", r->issues[i].code, r->issues[i].detail);
kura_result_free(r);
```

A preset is a [preflight profile](/preflight): read the JSON file into a string and pass it as `preflight_profile`. Its hits come back in `analysis`, its fixes are applied unless `verify_only` is set.

The C API has no colour options of its own. Convert colour through a profile's `convert` section, which accepts the built-in targets `cmyk`, `gray` and `rgb`:

```c
opt.preflight_profile =
  "{\"kura-profile\": 1, \"name\": \"To CMYK\", \"convert\": {\"colors\": {\"to\": \"cmyk\", \"ink_limit\": 300}}}";
kura_result* r = kura_convert(data, n, "x4", &opt);
```

Profile names (`"to": "ISO Coated v2 (ECI)"`) cannot be looked up through the C API; pass the ICC bytes as `dest_profile` and use `"to": "cmyk"` instead.

## Threading

A `kura_result` is not shared between threads. Independent calls producing independent results may run concurrently, with one exception: rasterization. PDFium keeps process-global state, so calls that flatten transparency or rasterize pages must be serialized across the process. Builds without PDFium have no such restriction.

## What the C API does not expose

Signing is a host callback in the C++ interface; the C ABI does not carry function pointers, so sign the output with your own PKCS#7 tooling. OCR is done by you: run your recognizer and pass the words as `ocr_pages`. Two-document compare, `--pdf-version`, `--level none`, `--dual`, the fallbacks, bookmarks, reports and the `kura tool` jobs are CLI features. The ABI grows additively; fields are appended to the option struct, never reordered.
