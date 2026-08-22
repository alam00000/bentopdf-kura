#include "kura/kura.h"

#include <string>
#include <vector>

#include "pdfa/pdfa.hh"
#ifdef KURA_WITH_PDFIUM
#include "kura/raster.hh"
#endif

namespace {
struct KuraResultImpl {
  kura_result pub;
  std::vector<unsigned char> pdf;
  std::string error_code;
  std::string error;
  std::string suggested;
};

void fillFailure(KuraResultImpl* impl, const char* code, const char* text) {
  impl->pub.ok = 0;
  impl->pub.pdf = nullptr;
  impl->pub.pdf_len = 0;
  impl->pub.error_code = code;
  impl->pub.error = text;
  impl->pub.suggested_level = nullptr;
  impl->pub.compliant = 0;
  impl->pub.findings = 0;
}
}

extern "C" kura_result* kura_convert(const unsigned char* data, size_t size, const char* level,
                                     const kura_options* options) noexcept {
  KuraResultImpl* impl = nullptr;
  try {
    impl = new KuraResultImpl;
  } catch (...) {
    return nullptr;
  }
  try {
  pdfa::Options opt;
  pdfa::Level parsed;
  if (!level || !pdfa::levelFromString(level, parsed)) {
    impl->error_code = "BAD_LEVEL";
    impl->error = "unknown conformance level";
    impl->pub.ok = 0;
    impl->pub.pdf = nullptr;
    impl->pub.pdf_len = 0;
    impl->pub.error_code = impl->error_code.c_str();
    impl->pub.error = impl->error.c_str();
    impl->pub.suggested_level = nullptr;
    impl->pub.compliant = 0;
    impl->pub.findings = 0;
    return &impl->pub;
  }
  opt.level = parsed;
  if (options) {
    opt.ua = options->ua != 0;
    opt.allowVisualRisk = options->allow_visual_risk != 0;
    if (options->doc_lang) opt.docLang = options->doc_lang;
    if (options->password) opt.password = options->password;
    if (options->output_condition_identifier) {
      opt.outputConditionIdentifier = options->output_condition_identifier;
    }
    if (options->dest_profile && options->dest_profile_len > 0) {
      opt.destProfile.assign(options->dest_profile, options->dest_profile_len);
    }
    opt.verifyOnly = options->verify_only != 0;
    if (options->invoice_xml && options->invoice_xml_len > 0) {
      opt.attachXml.assign(reinterpret_cast<const char*>(options->invoice_xml),
                           options->invoice_xml_len);
    }
    if (options->invoice_profile) opt.facturxProfile = options->invoice_profile;
    if (options->invoice_filename) opt.attachXmlName = options->invoice_filename;
  }
#ifdef KURA_WITH_PDFIUM
  opt.rasterizePage = kura::makeRasterizer(data, size, opt.password);
#endif
  pdfa::Result r = pdfa::convert(data, size, opt);
  impl->pdf.assign(r.pdf.begin(), r.pdf.end());
  impl->error_code = r.errorCode;
  impl->error = r.error;
  impl->suggested = r.suggestedLevel;
  impl->pub.ok = r.ok ? 1 : 0;
  impl->pub.pdf = impl->pdf.empty() ? nullptr : impl->pdf.data();
  impl->pub.pdf_len = impl->pdf.size();
  impl->pub.error_code = impl->error_code.empty() ? nullptr : impl->error_code.c_str();
  impl->pub.error = impl->error.empty() ? nullptr : impl->error.c_str();
  impl->pub.suggested_level = impl->suggested.empty() ? nullptr : impl->suggested.c_str();
  impl->pub.compliant = r.compliant ? 1 : 0;
  impl->pub.findings = 0;
  for (const pdfa::Issue& i : r.issues) {
    if (i.fixed && !pdfa::issueIsNormalization(i.code)) ++impl->pub.findings;
  }
  return &impl->pub;
  } catch (...) {
    fillFailure(impl, "INTERNAL_ERROR", "conversion aborted by an unrecognized error");
    return &impl->pub;
  }
}

extern "C" void kura_result_free(kura_result* result) noexcept {
  if (result) delete reinterpret_cast<KuraResultImpl*>(result);
}

extern "C" const char* kura_version(void) noexcept { return pdfa::kEngineVersion; }

extern "C" const char* kura_engine_name(void) noexcept { return pdfa::kEngineName; }
