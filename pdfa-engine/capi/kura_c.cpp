#include "kura/kura.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pdfa/einvoice.hh"
#include "pdfa/pdfa.hh"
#ifdef KURA_WITH_PDFIUM
#include "kura/raster.hh"
#endif

namespace {

struct IssueStore {
  std::string code;
  std::string detail;
  std::vector<int> pages;
};

struct KuraResultImpl {
  kura_result pub{};
  std::vector<unsigned char> pdf;
  std::string error_code;
  std::string error;
  std::string suggested;
  std::vector<IssueStore> issueStore;
  std::vector<IssueStore> analysisStore;
  std::vector<kura_issue> issues;
  std::vector<kura_issue> analysis;
};

struct KuraInvoiceImpl {
  kura_invoice pub{};
  std::string error_code;
  std::string error;
  std::string standard;
  std::string profile;
  std::string document_type;
  std::string attachment;
  std::string xml;
  std::vector<std::string> problems;
  std::vector<std::string> warnings;
  std::vector<const char*> problemPtrs;
  std::vector<const char*> warningPtrs;
};

void fillFailure(KuraResultImpl* impl, const char* code, const char* text) {
  impl->pub = kura_result{};
  impl->error_code = code;
  impl->error = text;
  impl->pub.error_code = impl->error_code.c_str();
  impl->pub.error = impl->error.c_str();
}

void storeIssues(const std::vector<pdfa::Issue>& in, std::vector<IssueStore>& store,
                 std::vector<kura_issue>& out) {
  store.clear();
  out.clear();
  store.reserve(in.size());
  for (const pdfa::Issue& i : in) store.push_back({i.code, i.detail, i.pages});
  out.reserve(in.size());
  for (size_t k = 0; k < in.size(); ++k) {
    kura_issue ki{};
    ki.code = store[k].code.c_str();
    ki.detail = store[k].detail.c_str();
    ki.severity = in[k].severity;
    ki.fixed = in[k].fixed ? 1 : 0;
    ki.pages = store[k].pages.empty() ? nullptr : store[k].pages.data();
    ki.page_count = store[k].pages.size();
    out.push_back(ki);
  }
}

std::vector<std::string> splitLines(const char* text) {
  std::vector<std::string> out;
  if (!text) return out;
  std::string cur;
  for (const char* p = text; *p; ++p) {
    if (*p == '\n' || *p == '\r') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += *p;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

struct OcrPageWords {
  std::vector<pdfa::Options::OcrWord> words;  // in PDF points, top-left origin
};

void applyOptions(const kura_options* options, pdfa::Options& opt) {
  if (!options) return;
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
  if (options->output_condition_info) opt.outputConditionInfo = options->output_condition_info;
  if (options->output_condition_registry) {
    opt.outputConditionRegistry = options->output_condition_registry;
  }
  if (options->vt_records) opt.vtRecords = options->vt_records;
  opt.analyze = options->analyze != 0;
  opt.outlineFonts = options->outline_fonts != 0;
  opt.linearize = options->linearize != 0;
  if (options->image_max_ppi > 0 && options->image_max_ppi <= 10000) {
    opt.imageMaxPpi = options->image_max_ppi;
  }
  if (options->raster_dpi >= 24 && options->raster_dpi <= 1200) {
    opt.rasterDpi = options->raster_dpi;
  }
  opt.rasterizeAllPages = options->rasterize_all_pages != 0;
  if (options->preflight_profile) opt.preflightProfile = options->preflight_profile;
  if (options->embed_source && options->embed_source_len > 0) {
    opt.embedSource.assign(reinterpret_cast<const char*>(options->embed_source),
                           options->embed_source_len);
    if (options->embed_source_name) opt.embedSourceName = options->embed_source_name;
    if (options->embed_source_mime) opt.embedSourceMime = options->embed_source_mime;
  }
  std::vector<std::string> folders = splitLines(options->font_folders);
  if (!folders.empty()) {
    opt.loadFont = [folders](const std::string& wanted, std::string& psName,
                             std::string& bytes) {
      for (const std::string& folder : folders) {
        if (pdfa::loadFontFromFolder(folder, wanted, psName, bytes)) return true;
      }
      return false;
    };
  }
  if (options->ocr_pages && options->ocr_page_count > 0) {
    auto pages = std::make_shared<std::map<int, OcrPageWords>>();
    for (size_t p = 0; p < options->ocr_page_count; ++p) {
      const kura_ocr_page& page = options->ocr_pages[p];
      if (page.page_index < 0 || !page.words) continue;
      OcrPageWords& dst = (*pages)[page.page_index];
      for (size_t w = 0; w < page.word_count; ++w) {
        const kura_ocr_word& src = page.words[w];
        if (!src.text || !*src.text || !(src.width > 0) || !(src.height > 0)) continue;
        pdfa::Options::OcrWord word;
        word.text = src.text;
        word.x = src.x;
        word.y = src.y;
        word.width = src.width;
        word.height = src.height;
        dst.words.push_back(std::move(word));
      }
    }
    opt.ocrPage = [pages](int pageIndex, double dpi, int, int, const std::string&,
                          std::vector<pdfa::Options::OcrWord>& words) {
      auto it = pages->find(pageIndex);
      if (it == pages->end() || !(dpi > 0)) return false;
      const double scale = dpi / 72.0;
      for (const pdfa::Options::OcrWord& w : it->second.words) {
        pdfa::Options::OcrWord px = w;
        px.x = w.x * scale;
        px.y = w.y * scale;
        px.width = w.width * scale;
        px.height = w.height * scale;
        words.push_back(std::move(px));
      }
      return !words.empty();
    };
  }
}

}  // namespace

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
      fillFailure(impl, "BAD_LEVEL", "unknown conformance level");
      return &impl->pub;
    }
    opt.level = parsed;
    applyOptions(options, opt);
#ifdef KURA_WITH_PDFIUM
    opt.rasterizePage = kura::makeRasterizer(data, size, opt.password);
#endif
    pdfa::Result r = pdfa::convert(data, size, opt);
    impl->pdf.assign(r.pdf.begin(), r.pdf.end());
    impl->error_code = r.errorCode;
    impl->error = r.error;
    impl->suggested = r.suggestedLevel;
    storeIssues(r.issues, impl->issueStore, impl->issues);
    storeIssues(r.analysis, impl->analysisStore, impl->analysis);
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
    impl->pub.issues = impl->issues.empty() ? nullptr : impl->issues.data();
    impl->pub.issue_count = impl->issues.size();
    impl->pub.analysis = impl->analysis.empty() ? nullptr : impl->analysis.data();
    impl->pub.analysis_count = impl->analysis.size();
    return &impl->pub;
  } catch (...) {
    fillFailure(impl, "INTERNAL_ERROR", "conversion aborted by an unrecognized error");
    return &impl->pub;
  }
}

extern "C" void kura_result_free(kura_result* result) noexcept {
  if (result) delete reinterpret_cast<KuraResultImpl*>(result);
}

extern "C" int kura_verify_password(const unsigned char* data, size_t size,
                                    const char* password) noexcept {
  try {
    return pdfa::verifyPassword(data, size, password ? std::string(password) : std::string())
               ? 1
               : 0;
  } catch (...) {
    return 0;
  }
}

extern "C" kura_invoice* kura_read_invoice(const unsigned char* data, size_t size,
                                           const char* password) noexcept {
  KuraInvoiceImpl* impl = nullptr;
  try {
    impl = new KuraInvoiceImpl;
  } catch (...) {
    return nullptr;
  }
  try {
    pdfa::InvoiceCheck c =
        pdfa::checkInvoice(data, size, password ? std::string(password) : std::string());
    impl->error_code = c.errorCode;
    impl->error = c.error;
    impl->standard = c.standard;
    impl->profile = c.profile;
    impl->document_type = c.documentType;
    impl->attachment = c.attachment;
    impl->xml = c.xml;
    impl->problems = c.problems;
    impl->warnings = c.warnings;
    for (const std::string& s : impl->problems) impl->problemPtrs.push_back(s.c_str());
    for (const std::string& s : impl->warnings) impl->warningPtrs.push_back(s.c_str());
    impl->pub.ok = c.ok ? 1 : 0;
    impl->pub.error_code = impl->error_code.empty() ? nullptr : impl->error_code.c_str();
    impl->pub.error = impl->error.empty() ? nullptr : impl->error.c_str();
    impl->pub.einvoice = c.einvoice ? 1 : 0;
    impl->pub.standard = impl->standard.c_str();
    impl->pub.profile = impl->profile.c_str();
    impl->pub.document_type = impl->document_type.c_str();
    impl->pub.attachment = impl->attachment.c_str();
    impl->pub.xml = impl->xml.empty()
                        ? nullptr
                        : reinterpret_cast<const unsigned char*>(impl->xml.data());
    impl->pub.xml_len = impl->xml.size();
    impl->pub.consistent = c.consistent ? 1 : 0;
    impl->pub.problems = impl->problemPtrs.empty() ? nullptr : impl->problemPtrs.data();
    impl->pub.problem_count = impl->problemPtrs.size();
    impl->pub.warnings = impl->warningPtrs.empty() ? nullptr : impl->warningPtrs.data();
    impl->pub.warning_count = impl->warningPtrs.size();
    return &impl->pub;
  } catch (...) {
    impl->pub = kura_invoice{};
    impl->error_code = "INTERNAL_ERROR";
    impl->error = "reading the invoice aborted by an unrecognized error";
    impl->pub.error_code = impl->error_code.c_str();
    impl->pub.error = impl->error.c_str();
    return &impl->pub;
  }
}

extern "C" void kura_invoice_free(kura_invoice* invoice) noexcept {
  if (invoice) delete reinterpret_cast<KuraInvoiceImpl*>(invoice);
}

extern "C" const char* kura_version(void) noexcept { return pdfa::kEngineVersion; }
extern "C" const char* kura_engine_name(void) noexcept { return pdfa::kEngineName; }
