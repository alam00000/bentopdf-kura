# C API

`libkura` exposes the engine as a small, stable C ABI for embedding in any language that can call C. Four functions, no exceptions across the boundary, additive only within a major version. Every release attaches `libkura.a` for Linux, Windows and macOS alongside `kura.h`.

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
} kura_options;

typedef struct {
  int ok;
  const unsigned char* pdf;
  size_t pdf_len;
  const char* error_code;
  const char* error;
  const char* suggested_level;
  int compliant;
  size_t findings;
} kura_result;

kura_result* kura_convert(const unsigned char* data, size_t size, const char* level,
                          const kura_options* options);
void kura_result_free(kura_result* result);
const char* kura_version(void);
const char* kura_engine_name(void);
```

- `kura_convert` converts memory to memory. `level` is any [target](/standards) as a string, `options` may be `NULL` for the defaults. It returns `NULL` only when the result object itself could not be allocated; every other failure, a bad level, an encrypted file, a rejected document, comes back on the result with `ok == 0` and `error_code` set.
- Zero the options struct before setting fields. A null pointer field means "not set".
- `verify_only` runs [check mode](/check-mode): `pdf` stays empty and `compliant` and `findings` are filled in.
- The `pdf` buffer is owned by the result. Copy it out before `kura_result_free`, which releases everything the result points at.
- `error_code` values are listed on [Rejection codes](/rejections); `suggested_level` is set for the rejections that have one.

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

## Threading

A `kura_result` is not shared between threads. Independent calls producing independent results may run concurrently, with one exception: rasterization. PDFium keeps process-global state, so calls that flatten transparency or rasterize pages must be serialized across the process. Builds without PDFium have no such restriction.

## What the C API does not expose

Signing, OCR and custom font folders are host callbacks in the C++ interface; the C ABI does not carry function pointers yet. Preflight profiles and the document census are likewise C++-only for now. The ABI grows additively; fields are appended to the option struct, never reordered.
