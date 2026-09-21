#ifndef KURA_H
#define KURA_H

#include <stddef.h>

#ifdef __cplusplus
#define KURA_NOEXCEPT noexcept
extern "C" {
#else
#define KURA_NOEXCEPT
#endif

#define KURA_VERSION "1.2.1"

/* One OCR word in PDF points, x/y from the top-left corner of the page as it
 * renders (y grows downwards).  Kura maps them onto the page it rasterizes. */
typedef struct {
  const char* text;
  double x;
  double y;
  double width;
  double height;
} kura_ocr_word;

/* Recognized words for one page (0-based index). */
typedef struct {
  int page_index;
  const kura_ocr_word* words;
  size_t word_count;
} kura_ocr_page;

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
  /* Added in 1.2: every field below defaults to off / empty when zeroed. */
  const char* output_condition_info;
  const char* output_condition_registry;
  const char* vt_records;
  int analyze;              /* run the analysis census and report it */
  int outline_fonts;        /* replace text without Unicode by outlines */
  int linearize;            /* write a linearized file */
  double image_max_ppi;     /* downsample images above this, 0 = keep */
  double raster_dpi;        /* rasterizer resolution, 0 = engine default */
  int rasterize_all_pages;  /* rasterize every page */
  const char* preflight_profile;   /* profile JSON text; its fixes apply unless verify_only */
  const unsigned char* embed_source;  /* source file to attach (PDF/A-3) */
  size_t embed_source_len;
  const char* embed_source_name;
  const char* embed_source_mime;
  const char* font_folders;  /* newline-separated folders searched for missing fonts */
  const kura_ocr_page* ocr_pages;  /* words to lay down as invisible text */
  size_t ocr_page_count;
} kura_options;

/* A finding.  severity: 1 info, 2 warning, 3 error.  pages are 1-based; an
 * empty list means the finding concerns the whole document. */
typedef struct {
  const char* code;
  const char* detail;
  int severity;
  int fixed;
  const int* pages;
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
  /* Added in 1.2. */
  const kura_issue* issues;
  size_t issue_count;
  const kura_issue* analysis;
  size_t analysis_count;
} kura_result;

/* Result of reading an e-invoice PDF.  ok = the PDF was readable; einvoice =
 * an invoice payload was found; consistent = no problems. */
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
                          const kura_options* options) KURA_NOEXCEPT;

void kura_result_free(kura_result* result) KURA_NOEXCEPT;

/* 1 when `password` opens the document (or it needs none), 0 otherwise. */
int kura_verify_password(const unsigned char* data, size_t size,
                         const char* password) KURA_NOEXCEPT;

kura_invoice* kura_read_invoice(const unsigned char* data, size_t size,
                                const char* password) KURA_NOEXCEPT;

void kura_invoice_free(kura_invoice* invoice) KURA_NOEXCEPT;

const char* kura_version(void) KURA_NOEXCEPT;

const char* kura_engine_name(void) KURA_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif
