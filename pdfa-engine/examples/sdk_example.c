#include <stdio.h>
#include <stdlib.h>
#include "kura/kura.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <in.pdf> <level> [out.pdf]\n", argv[0]);
    return 2;
  }
  FILE* f = fopen(argv[1], "rb");
  if (!f) { perror("open"); return 1; }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char* buf = (unsigned char*)malloc(n);
  if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); return 1; }
  fclose(f);

  printf("%s %s\n", kura_engine_name(), kura_version());
  kura_options opt = {0};
  kura_result* r = kura_convert(buf, n, argv[2], &opt);
  free(buf);

  if (r->ok) {
    printf("converted to PDF/%s: %zu bytes\n", argv[2], r->pdf_len);
    if (argc >= 4) {
      FILE* o = fopen(argv[3], "wb");
      fwrite(r->pdf, 1, r->pdf_len, o);
      fclose(o);
    }
  } else {
    printf("rejected: %s (%s)\n", r->error_code ? r->error_code : "?",
           r->error ? r->error : "");
  }
  kura_result_free(r);
  return 0;
}
