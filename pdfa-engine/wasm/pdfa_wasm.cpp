#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <string>
#include <vector>

#include "pdfa/pdfa.hh"
#ifdef KURA_WITH_PDFIUM
#include "kura/raster.hh"
#endif

namespace {
std::string optString(emscripten::val opts, const char* key) {
  if (opts.isUndefined() || opts.isNull()) return std::string();
  emscripten::val v = opts[key];
  if (v.isUndefined() || v.isNull()) return std::string();
  return v.as<std::string>();
}

bool optBool(emscripten::val opts, const char* key) {
  if (opts.isUndefined() || opts.isNull()) return false;
  emscripten::val v = opts[key];
  if (v.isUndefined() || v.isNull()) return false;
  return v.as<bool>();
}

std::string optBytes(emscripten::val opts, const char* key) {
  if (opts.isUndefined() || opts.isNull()) return std::string();
  emscripten::val v = opts[key];
  if (v.isUndefined() || v.isNull()) return std::string();
  std::vector<uint8_t> bytes = emscripten::convertJSArrayToNumberVector<uint8_t>(v);
  return std::string(bytes.begin(), bytes.end());
}

emscripten::val convertJs(emscripten::val data, const std::string& level,
                          emscripten::val opts) {
  emscripten::val result = emscripten::val::object();
  pdfa::Options opt;
  if (!pdfa::levelFromString(level, opt.level)) {
    result.set("ok", false);
    result.set("errorCode", std::string("BAD_LEVEL"));
    result.set("error", std::string("unknown conformance level: ") + level);
    return result;
  }
  opt.allowVisualRisk = optBool(opts, "allowVisualRisk");
  opt.ua = optBool(opts, "ua");
  opt.password = optString(opts, "password");
  opt.docLang = optString(opts, "lang");
  opt.outputConditionIdentifier = optString(opts, "outputCondition");
  opt.outputConditionInfo = optString(opts, "outputConditionInfo");
  opt.outputConditionRegistry = optString(opts, "registry");
  opt.vtRecords = optString(opts, "vtRecords");
  opt.attachXmlName = optString(opts, "attachXmlName");
  opt.facturxProfile = optString(opts, "facturxProfile");
  opt.nowOverride = optString(opts, "now");
  opt.destProfile = optBytes(opts, "destProfile");
  opt.attachXml = optBytes(opts, "attachXml");
  opt.verifyOnly = optBool(opts, "check");
  opt.analyze = optBool(opts, "analyze");
  opt.outlineFonts = optBool(opts, "outlineFonts");
  opt.embedSource = optBytes(opts, "embedSource");
  opt.embedSourceName = optString(opts, "embedSourceName");
  opt.embedSourceMime = optString(opts, "embedSourceMime");
  opt.defaultRgbProfile = optBytes(opts, "defaultRgb");
  opt.defaultCmykProfile = optBytes(opts, "defaultCmyk");
  opt.defaultGrayProfile = optBytes(opts, "defaultGray");
  opt.rasterizeAllPages = optBool(opts, "rasterizePages");
  {
    emscripten::val dpi = opts.isUndefined() || opts.isNull() ? emscripten::val::undefined()
                                                             : opts["rasterDpi"];
    if (!dpi.isUndefined() && !dpi.isNull()) {
      double v = dpi.as<double>();
      if (v >= 24 && v <= 1200) opt.rasterDpi = v;
    }
  }

  std::vector<uint8_t> input = emscripten::convertJSArrayToNumberVector<uint8_t>(data);
#ifdef KURA_WITH_PDFIUM
  opt.rasterizePage = kura::makeRasterizer(input.data(), input.size(), opt.password);
#endif
  pdfa::Result res = pdfa::convert(input.data(), input.size(), opt);
  result.set("ok", res.ok);
  result.set("level", pdfa::levelToString(opt.level));
  result.set("engine", std::string(pdfa::kEngineVersion));
  if (!res.errorCode.empty()) {
    result.set("errorCode", res.errorCode);
    result.set("error", res.error);
  }
  if (!res.suggestedLevel.empty()) result.set("suggestedLevel", res.suggestedLevel);
  emscripten::val issues = emscripten::val::array();
  size_t n = 0;
  for (const pdfa::Issue& is : res.issues) {
    if (opt.verifyOnly && (!is.fixed || pdfa::issueIsNormalization(is.code))) continue;
    emscripten::val item = emscripten::val::object();
    item.set("code", is.code);
    item.set("detail", is.detail);
    item.set("fixed", is.fixed);
    issues.set(n++, item);
  }
  result.set("issues", issues);
  if (opt.analyze) {
    emscripten::val analysis = emscripten::val::array();
    size_t an = 0;
    for (const pdfa::Issue& is : res.analysis) {
      emscripten::val item = emscripten::val::object();
      item.set("code", is.code);
      item.set("detail", is.detail);
      analysis.set(an++, item);
    }
    result.set("analysis", analysis);
  }
  if (opt.verifyOnly) {
    result.set("mode", std::string("check"));
    result.set("compliant", res.compliant);
    result.set("findings", n);
  }
  if (res.ok && !opt.verifyOnly) {
    emscripten::val view = emscripten::val(emscripten::typed_memory_view(
        res.pdf.size(), reinterpret_cast<const unsigned char*>(res.pdf.data())));
    emscripten::val out = emscripten::val::global("Uint8Array").new_(res.pdf.size());
    out.call<void>("set", view);
    result.set("pdf", out);
  }
  return result;
}

std::string versionJs() { return std::string("pdfa-engine ") + pdfa::kEngineVersion; }
}

EMSCRIPTEN_BINDINGS(pdfa) {
  emscripten::function("convert", &convertJs);
  emscripten::function("version", &versionJs);
}
