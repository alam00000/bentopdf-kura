/* Exercises every entry point of the C API against a real document.
 * usage: kura_sdk_smoke <input.pdf> [profile.json] [einvoice.pdf]
 * exit 0 when every check holds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kura/kura.h"

static unsigned char* read_file(const char* path, size_t* len) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char* buf = (unsigned char*)malloc((size_t)n + 1);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
  fclose(f);
  buf[n] = 0;
  *len = (size_t)n;
  return buf;
}

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); ++failures; } else { printf("ok: %s\n", msg); } } while (0)

static void print_issues(const char* label, const kura_issue* list, size_t n) {
  size_t i, p;
  for (i = 0; i < n; ++i) {
    printf("  %s %s sev=%d fixed=%d pages=[", label, list[i].code, list[i].severity, list[i].fixed);
    for (p = 0; p < list[i].page_count; ++p) printf("%s%d", p ? "," : "", list[i].pages[p]);
    printf("] %s\n", list[i].detail);
  }
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <input.pdf> [profile.json] [einvoice.pdf]\n", argv[0]); return 64; }
  size_t len = 0;
  unsigned char* data = read_file(argv[1], &len);
  if (!data) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
  printf("%s %s\n", kura_engine_name(), kura_version());

  /* 1. check mode: structured findings */
  kura_options opt;
  memset(&opt, 0, sizeof opt);
  opt.verify_only = 1;
  kura_result* r = kura_convert(data, len, "2b", &opt);
  CHECK(r && r->ok, "check mode runs");
  if (r) {
    CHECK(r->issue_count > 0 && r->issues != NULL, "check mode reports structured issues");
    size_t i, classified = 0;
    for (i = 0; i < r->issue_count; ++i) if (r->issues[i].severity >= 1 && r->issues[i].severity <= 3) ++classified;
    CHECK(classified == r->issue_count, "every issue carries a severity in 1..3");
    print_issues("issue", r->issues, r->issue_count);
    kura_result_free(r);
  }

  /* 2. analysis census */
  memset(&opt, 0, sizeof opt);
  opt.verify_only = 1;
  opt.analyze = 1;
  r = kura_convert(data, len, "2b", &opt);
  CHECK(r && r->ok && r->analysis_count > 0, "analysis mode reports the census");
  if (r) { print_issues("analysis", r->analysis, r->analysis_count); kura_result_free(r); }

  /* 3. external preflight profile */
  if (argc >= 3) {
    size_t plen = 0;
    unsigned char* prof = read_file(argv[2], &plen);
    CHECK(prof != NULL, "profile file readable");
    if (prof) {
      memset(&opt, 0, sizeof opt);
      opt.verify_only = 1;
      opt.preflight_profile = (const char*)prof;
      r = kura_convert(data, len, "2b", &opt);
      int hits = 0;
      if (r) { size_t i; for (i = 0; i < r->analysis_count; ++i) if (!strcmp(r->analysis[i].code, "PROFILE_HIT")) ++hits; }
      CHECK(r && r->ok && hits > 0, "profile check reports PROFILE_HIT findings");
      if (r) { print_issues("profile", r->analysis, r->analysis_count); kura_result_free(r); }
      free(prof);
    }
  }

  /* 4. password verification */
  CHECK(kura_verify_password(data, len, "") == 1, "empty password opens an unencrypted file");
  CHECK(kura_verify_password(data, len, NULL) == 1, "NULL password opens an unencrypted file");

  /* 5. invoice reading on a plain PDF */
  kura_invoice* inv = kura_read_invoice(data, len, NULL);
  CHECK(inv && inv->ok && !inv->einvoice, "plain PDF reports no e-invoice payload");
  if (inv) kura_invoice_free(inv);
  if (argc >= 4) {
    size_t ilen = 0;
    unsigned char* idata = read_file(argv[3], &ilen);
    CHECK(idata != NULL, "e-invoice file readable");
    if (idata) {
      inv = kura_read_invoice(idata, ilen, NULL);
      CHECK(inv && inv->ok && inv->einvoice && inv->xml_len > 0, "e-invoice payload extracted");
      if (inv) {
        printf("  invoice %s %s %s attachment=%s consistent=%d problems=%zu warnings=%zu\n",
               inv->standard, inv->profile, inv->document_type, inv->attachment, inv->consistent,
               inv->problem_count, inv->warning_count);
        kura_invoice_free(inv);
      }
      free(idata);
    }
  }

  /* 6. font folders remove substitutions when the real font exists */
  memset(&opt, 0, sizeof opt);
  r = kura_convert(data, len, "2b", &opt);
  size_t subst_without = 0, subst_with = 0;
  if (r) { size_t i; for (i = 0; i < r->issue_count; ++i) if (!strcmp(r->issues[i].code, "FONT_SUBSTITUTED")) ++subst_without; kura_result_free(r); }
  memset(&opt, 0, sizeof opt);
  opt.font_folders = "/System/Library/Fonts\n/Library/Fonts";
  r = kura_convert(data, len, "2b", &opt);
  CHECK(r && r->ok && r->pdf_len > 0, "conversion with font folders produces a PDF");
  if (r) { size_t i; for (i = 0; i < r->issue_count; ++i) if (!strcmp(r->issues[i].code, "FONT_SUBSTITUTED")) ++subst_with; kura_result_free(r); }
  printf("  FONT_SUBSTITUTED without folders=%zu with folders=%zu\n", subst_without, subst_with);
  CHECK(subst_with <= subst_without, "font folders never add substitutions");

  /* 7. OCR words become an invisible text layer */
  kura_ocr_word words[2];
  words[0].text = "Layered"; words[0].x = 60; words[0].y = 60; words[0].width = 80; words[0].height = 14;
  words[1].text = "Test";    words[1].x = 150; words[1].y = 60; words[1].width = 40; words[1].height = 14;
  kura_ocr_page page; page.page_index = 0; page.words = words; page.word_count = 2;
  memset(&opt, 0, sizeof opt);
  opt.ocr_pages = &page; opt.ocr_page_count = 1;
  r = kura_convert(data, len, "2b", &opt);
  CHECK(r && r->ok && r->pdf_len > 0, "conversion with OCR words produces a PDF");
  if (r) {
    size_t i; int ocr_issue = 0;
    for (i = 0; i < r->issue_count; ++i) if (strstr(r->issues[i].code, "OCR")) { ocr_issue = 1; printf("  %s: %s\n", r->issues[i].code, r->issues[i].detail); }
    CHECK(ocr_issue, "OCR layer is reported as an issue");
    kura_result_free(r);
  }

  /* 8. bad level and NULL options are handled */
  r = kura_convert(data, len, "nope", NULL);
  CHECK(r && !r->ok && r->error_code && !strcmp(r->error_code, "BAD_LEVEL"), "unknown level fails cleanly");
  if (r) kura_result_free(r);
  r = kura_convert(data, len, "2b", NULL);
  CHECK(r && r->ok, "NULL options behave like defaults");
  if (r) kura_result_free(r);

  free(data);
  printf("%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
