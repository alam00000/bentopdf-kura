#ifndef KURA_H
#define KURA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KURA_VERSION "1.1.0"

typedef struct {
  int ua;
  int allow_visual_risk;
  const char* doc_lang;
  const char* password;
  const char* output_condition_identifier;
  const char* dest_profile;
  size_t dest_profile_len;
} kura_options;

typedef struct {
  int ok;
  const unsigned char* pdf;
  size_t pdf_len;
  const char* error_code;
  const char* error;
  const char* suggested_level;
} kura_result;

kura_result* kura_convert(const unsigned char* data, size_t size, const char* level,
                          const kura_options* options);

void kura_result_free(kura_result* result);

const char* kura_version(void);

const char* kura_engine_name(void);

#ifdef __cplusplus
}
#endif

#endif
