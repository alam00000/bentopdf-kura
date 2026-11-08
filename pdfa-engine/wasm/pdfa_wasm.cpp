#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <set>
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

bool optPresent(emscripten::val opts, const char* key) {
  if (opts.isUndefined() || opts.isNull()) return false;
  emscripten::val v = opts[key];
  return !v.isUndefined() && !v.isNull();
}

std::string configureColors(emscripten::val opts, pdfa::Options& opt) {
  using R = pdfa::ColorRule;
  pdfa::ColorJob& j = opt.colorJob;
  if (optBool(opts, "noBpc")) j.blackPointCompensation = false;
  if (optBool(opts, "grayToK")) j.grayToBlackOnly = true;
  if (optPresent(opts, "rgbGrayToK")) {
    double pct = opts["rgbGrayToK"].as<double>();
    if (!(pct >= 0 && pct <= 100)) return "rgbGrayToK must be between 0 and 100";
    j.rgbGrayToBlackOnly = true;
    for (double& t : j.rgbGrayTolerance) t = pct / 100.0;
  }
  if (optPresent(opts, "inkLimit")) {
    j.inkLimit = opts["inkLimit"].as<double>();
    if (!(j.inkLimit >= 100 && j.inkLimit <= 400)) return "inkLimit must be between 100 and 400";
  }
  j.deviceLink = optBytes(opts, "deviceLink");
  std::string target;
  std::string targetIcc;
  if (optPresent(opts, "convertColors")) {
    if (opts["convertColors"].isString()) target = opts["convertColors"].as<std::string>();
    else targetIcc = optBytes(opts, "convertColors");
  } else if (!j.deviceLink.empty()) {
    target = "device-link";
  }
  if (target.empty() && targetIcc.empty()) {
    if (optPresent(opts, "intent") || optPresent(opts, "convertObjects") || optBool(opts, "keepSpots"))
      return "intent, convertObjects and keepSpots need convertColors";
    return std::string();
  }
  int intent = -1;
  if (optPresent(opts, "intent")) {
    std::string v = optString(opts, "intent");
    intent = v == "perceptual" ? 0 : v == "relative" ? 1 : v == "saturation" ? 2 : v == "absolute" ? 3 : -2;
    if (intent == -2) return "intent must be perceptual, relative, saturation or absolute";
  }
  unsigned objects = R::kAllObjects;
  if (optPresent(opts, "convertObjects")) {
    objects = 0;
    std::string list = optString(opts, "convertObjects") + ",";
    size_t start = 0, comma;
    while ((comma = list.find(',', start)) != std::string::npos) {
      std::string w = list.substr(start, comma - start);
      start = comma + 1;
      if (w == "text") objects |= R::kText;
      else if (w == "vector") objects |= R::kVector;
      else if (w == "shading" || w == "shadings") objects |= R::kShading;
      else if (w == "images") objects |= R::kImageLossy | R::kImageLossless;
      else if (w == "lossy") objects |= R::kImageLossy;
      else if (w == "lossless") objects |= R::kImageLossless;
      else if (!w.empty()) return "convertObjects takes text, vector, shading, images, lossy, lossless";
    }
  }
  if (!pdfa::configureColorConversion(opt, target, targetIcc, intent, optBool(opts, "keepSpots"), objects))
    return "convertColors takes cmyk, gray, rgb or an ICC profile";
  return std::string();
}

emscripten::val pagesVal(const pdfa::Issue& is) {
  emscripten::val arr = emscripten::val::array();
  for (size_t i = 0; i < is.pages.size(); ++i) arr.set(i, is.pages[i]);
  return arr;
}

emscripten::val convertJs(emscripten::val data, const std::string& level,
                          emscripten::val opts) {
  emscripten::val result = emscripten::val::object();
  pdfa::Options opt;
  opt.levelFromProfile = level.empty();
  opt.noStandard = level == "none";
  if (!level.empty() && !opt.noStandard && !pdfa::levelFromString(level, opt.level)) {
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
  opt.preflightProfile = optString(opts, "profile");
  if (opt.levelFromProfile && !opt.preflightProfile.empty()) opt.noStandard = true;
  opt.pdfVersion = optString(opts, "pdfVersion");
  static const std::set<std::string> kVersions = {"", "1.3", "1.4", "1.5", "1.6", "1.7", "2.0"};
  if (!kVersions.count(opt.pdfVersion)) {
    result.set("ok", false);
    result.set("errorCode", std::string("BAD_INPUT"));
    result.set("error", std::string("pdfVersion takes 1.3 to 2.0"));
    return result;
  }
  if (!opts.isUndefined() && !opts.isNull() && !opts["vars"].isUndefined() && !opts["vars"].isNull()) {
    emscripten::val vars = opts["vars"];
    emscripten::val keys = emscripten::val::global("Object").call<emscripten::val>("keys", vars);
    for (unsigned k = 0, n = keys["length"].as<unsigned>(); k < n; ++k) {
      std::string name = keys[k].as<std::string>();
      opt.profileVars[name] = emscripten::val::global("String")(vars[name]).as<std::string>();
    }
  }
  {
    emscripten::val dpi = opts.isUndefined() || opts.isNull() ? emscripten::val::undefined()
                                                             : opts["rasterDpi"];
    if (!dpi.isUndefined() && !dpi.isNull()) {
      double v = dpi.as<double>();
      if (v >= 24 && v <= 1200) opt.rasterDpi = v;
    }
  }
  {
    std::string colorError = configureColors(opts, opt);
    if (!colorError.empty()) {
      result.set("ok", false);
      result.set("errorCode", std::string("BAD_OPTION"));
      result.set("error", colorError);
      return result;
    }
  }

  std::vector<uint8_t> input = emscripten::convertJSArrayToNumberVector<uint8_t>(data);
#ifdef KURA_WITH_PDFIUM
  opt.rasterizePage = kura::makeRasterizer(input.data(), input.size(), opt.password);
  opt.renderPdf = kura::renderFirstPage;
#endif
  pdfa::Result res = pdfa::convert(input.data(), input.size(), opt);
  result.set("ok", res.ok);
  result.set("level", res.level.empty() ? pdfa::levelToString(opt.level) : res.level);
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
    item.set("severity", is.severity);
    item.set("pages", pagesVal(is));
    issues.set(n++, item);
  }
  result.set("issues", issues);
  if (opt.analyze || !opt.preflightProfile.empty()) {
    emscripten::val analysis = emscripten::val::array();
    size_t an = 0;
    for (const pdfa::Issue& is : res.analysis) {
      emscripten::val item = emscripten::val::object();
      item.set("code", is.code);
      item.set("detail", is.detail);
      item.set("severity", is.severity);
      item.set("pages", pagesVal(is));
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

bool verifyPasswordJs(emscripten::val data, const std::string& password) {
  std::vector<uint8_t> input = emscripten::convertJSArrayToNumberVector<uint8_t>(data);
  return pdfa::verifyPassword(input.data(), input.size(), password);
}

std::string versionJs() { return std::string(pdfa::kEngineName) + " " + pdfa::kEngineVersion; }
}

EMSCRIPTEN_BINDINGS(pdfa) {
  emscripten::function("convert", &convertJs);
  emscripten::function("verifyPassword", &verifyPasswordJs);
  emscripten::function("version", &versionJs);
}
