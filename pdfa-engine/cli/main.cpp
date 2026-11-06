#include <algorithm>
#include <filesystem>
#include <cctype>
#include <system_error>
#ifdef _WIN32
#include <process.h>
#define kura_getpid _getpid
#else
#include <unistd.h>
#define kura_getpid getpid
#endif
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include "pdfa/pdfa.hh"
#include "../core/src/einvoice.hh"
#include "../core/src/profile_vars.hh"
#include "../core/src/uacheck.hh"
#include "signer.hh"
#include "tools.hh"

#ifdef KURA_WITH_PDFIUM
#include "kura/raster.hh"
#endif

namespace {
constexpr int kExitOk = 0;
constexpr int kExitFindings = 1;
constexpr int kExitRejected = 2;
constexpr int kExitUsage = 64;

std::atomic<long long> gDeadlineMs{0};
unsigned gWatchdogSeconds = 0;
std::string gReportPath, gReportDict, gReportLang = "en", gJobId;
std::mutex gTraceMu;
std::vector<std::string> gTraceLines;
std::ofstream gTraceFile;
bool gOnlyPdfa = false;
bool gPureUa = false;
bool gCertify = false;

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void armWatchdog(unsigned seconds) {
  gDeadlineMs.store(nowMs() + static_cast<long long>(seconds) * 1000);
}

std::string jsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20 || c > 0x7E) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

std::string issueMetaJson(const pdfa::Issue& is) {
  std::string out = ",\"severity\":" + std::to_string(is.severity) + ",\"pages\":[";
  for (size_t i = 0; i < is.pages.size(); ++i) {
    out += (i ? "," : "") + std::to_string(is.pages[i]);
  }
  return out + "]";
}

void printReport(const pdfa::Options& opt, const pdfa::Result& res,
                 const std::string& source, const std::string& output = "") {
  std::string json = "{";
  if (!source.empty()) json += "\"file\":\"" + jsonEscape(source) + "\",";
  if (!output.empty()) json += "\"output\":\"" + jsonEscape(output) + "\",";
  json += "\"ok\":" + std::string(res.ok ? "true" : "false");
  json += ",\"level\":\"" + (res.level.empty() ? pdfa::levelToString(opt.level) : res.level) + "\"";
  json += ",\"engine\":\"" + std::string(pdfa::kEngineName) + " " +
          std::string(pdfa::kEngineVersion) + "\"";
  if (!res.errorCode.empty()) {
    json += ",\"errorCode\":\"" + jsonEscape(res.errorCode) + "\"";
    json += ",\"error\":\"" + jsonEscape(res.error) + "\"";
  }
  if (!res.suggestedLevel.empty()) {
    json += ",\"suggestedLevel\":\"" + jsonEscape(res.suggestedLevel) + "\"";
  }
  if (opt.verifyOnly) {
    json += ",\"mode\":\"check\"";
    json += ",\"compliant\":" + std::string(res.compliant ? "true" : "false");
    size_t findings = 0;
    for (const pdfa::Issue& is : res.issues) {
      if (is.fixed && !pdfa::issueIsNormalization(is.code)) ++findings;
    }
    json += ",\"findings\":" + std::to_string(findings);
  }
  json += ",\"issues\":[";
  bool first = true;
  for (const pdfa::Issue& is : res.issues) {
    if (opt.verifyOnly && (!is.fixed || pdfa::issueIsNormalization(is.code))) continue;
    if (!first) json += ",";
    first = false;
    json += "{\"code\":\"" + jsonEscape(is.code) + "\",\"detail\":\"" + jsonEscape(is.detail) +
            "\",\"fixed\":" + (is.fixed ? "true" : "false") + issueMetaJson(is) + "}";
  }
  json += "]";
  if (opt.analyze || !opt.preflightProfile.empty()) {
    json += ",\"analysis\":[";
    bool firstA = true;
    for (const pdfa::Issue& is : res.analysis) {
      if (!firstA) json += ",";
      firstA = false;
      json += "{\"code\":\"" + jsonEscape(is.code) + "\",\"detail\":\"" +
              jsonEscape(is.detail) + "\"" + issueMetaJson(is) + "}";
    }
    json += "]";
  }
  if (!gJobId.empty()) json += ",\"jobId\":\"" + jsonEscape(gJobId) + "\"";
  json += "}";
  std::cout << json << std::endl;
  if (!gReportPath.empty()) {
    std::string err;
    if (!kuratools::writeReport(gReportPath, json, gReportDict, gReportLang, err)) {
      std::cerr << "report: " << err << std::endl;
    }
  }
}


std::string lowerOf(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string defaultOutputPath(const std::string& input, const pdfa::Options& opt) {
  std::string base = input;
  const std::string lower = lowerOf(input);
  for (const char* ext : {".pdf", ".jpeg", ".jpg"}) {
    const std::string e = ext;
    if (lower.size() > e.size() && lower.compare(lower.size() - e.size(), e.size(), e) == 0) {
      base = input.substr(0, input.size() - e.size());
      break;
    }
  }
  return base + "." + pdfa::levelToString(opt.level) + (opt.ua ? "-ua" : "") + ".pdf";
}

bool runTesseract(const std::string& exe, int, double, int w, int h, const std::string& rgb,
                  std::vector<pdfa::Options::OcrWord>& words) {
  std::filesystem::path tmp =
      std::filesystem::temp_directory_path() / ("kura-ocr-" + std::to_string(kura_getpid()));
  std::filesystem::path ppm = tmp;
  ppm += ".ppm";
  {
    std::ofstream f(ppm, std::ios::binary);
    if (!f) return false;
    f << "P6\n" << w << " " << h << "\n255\n";
    f.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
  }
  int rc = kuratools::runProcess({exe, ppm.string(), tmp.string(), "tsv"}, 0);
  std::filesystem::path tsv = tmp;
  tsv += ".tsv";
  bool ok = false;
  if (rc == 0) {
    std::ifstream in(tsv);
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
      std::vector<std::string> col;
      size_t start = 0;
      while (true) {
        size_t tab = line.find('\t', start);
        col.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
      }
      if (col.size() < 12) continue;
      const std::string& text = col[11];
      if (text.empty()) continue;
      bool printable = true;
      for (unsigned char c : text) {
        if (c < 0x20 || c > 0x7E) printable = false;
      }
      if (!printable) continue;
      pdfa::Options::OcrWord word;
      word.text = text;
      word.x = std::atof(col[6].c_str());
      word.y = std::atof(col[7].c_str());
      word.width = std::atof(col[8].c_str());
      word.height = std::atof(col[9].c_str());
      if (word.width > 0 && word.height > 0) words.push_back(word);
    }
    ok = !words.empty();
  }
  std::error_code ec;
  std::filesystem::remove(ppm, ec);
  std::filesystem::remove(tsv, ec);
  return ok;
}

bool readFile(const char* path, std::string& into) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::cerr << "cannot open " << path << std::endl;
    return false;
  }
  into.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return true;
}

bool parseVarFile(const std::string& txt, std::map<std::string, std::string>& into) {
  pdfa::Json j;
  std::string err;
  if (!pdfa::parseJson(txt, j, err) || j.type != pdfa::Json::kObj) {
    std::cerr << "--var-file needs a JSON object {\"name\": value}" << (err.empty() ? "" : ": " + err) << std::endl;
    return false;
  }
  for (const auto& kv : j.obj) into[kv.first] = kv.second.text();
  return true;
}

bool readProfile(const std::string& path, pdfa::Json& j) {
  std::string text, err;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return pdfa::parseJson(text, j, err) && j.type == pdfa::Json::kObj && j.get("kura-profile");
}

std::string fieldText(const pdfa::Json& j, const char* key) {
  const pdfa::Json* f = j.get(key);
  return f ? f->text() : std::string();
}

size_t entries(const pdfa::Json& j, const char* key) {
  const pdfa::Json* f = j.get(key);
  return f ? f->arr.size() : 0;
}

int listVars(const char* path) {
  pdfa::Json prof;
  if (!readProfile(path, prof)) {
    std::cerr << path << " is not a Kura JSON profile" << std::endl;
    return kExitRejected;
  }
  std::string json = "{\"ok\":true,\"profile\":\"" + jsonEscape(path) + "\",\"scriptEngine\":" +
                     (pdfa::scriptEngineAvailable() ? "true" : "false") + ",\"variables\":[";
  bool first = true;
  for (const pdfa::ProfileVariable& v : pdfa::listProfileVariables(prof)) {
    std::string choices;
    for (const std::string& c : v.choices) choices += (choices.empty() ? "\"" : ",\"") + jsonEscape(c) + "\"";
    json += std::string(first ? "" : ",") + "{\"name\":\"" + jsonEscape(v.name) + "\",\"label\":\"" +
            jsonEscape(v.label) + "\",\"type\":\"" + jsonEscape(v.type.empty() ? "text" : v.type) +
            "\",\"default\":\"" + jsonEscape(v.defaultValue) + "\",\"choices\":[" + choices +
            "],\"script\":" + (v.script.empty() ? "false" : "true") + "}";
    first = false;
  }
  std::cout << json << "]}" << std::endl;
  return 0;
}

int listProfiles(const char* dir) {
  std::error_code ec;
  std::vector<std::filesystem::path> files;
  for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file() && it->path().extension() == ".json") files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  std::string json = "{\"ok\":true,\"profiles\":[";
  bool first = true;
  for (const auto& f : files) {
    pdfa::Json prof;
    if (!readProfile(f.string(), prof)) continue;
    std::string kind = prof.get("plan") ? "plan" : entries(prof, "fixes") || prof.get("convert") ? "fix" : "check";
    std::string name = fieldText(prof, "name");
    json += std::string(first ? "" : ",") + "{\"file\":\"" + jsonEscape(f.string()) + "\",\"name\":\"" +
            jsonEscape(name.empty() ? f.stem().string() : name) + "\",\"description\":\"" +
            jsonEscape(fieldText(prof, "description")) + "\",\"kind\":\"" + kind +
            "\",\"checks\":" + std::to_string(entries(prof, "checks")) +
            ",\"fixes\":" + std::to_string(entries(prof, "fixes")) +
            ",\"variables\":" + std::to_string(entries(prof, "variables")) + "}";
    first = false;
  }
  std::cout << json << "]}" << std::endl;
  return 0;
}

std::string mergePdfs(const std::vector<std::string>& inputs, const std::string& password, std::string& err) {
  try {
    QPDF out;
    out.emptyPDF();
    QPDFPageDocumentHelper od(out);
    std::vector<std::unique_ptr<QPDF>> keep;
    for (const std::string& path : inputs) {
      auto in = std::make_unique<QPDF>();
      in->setSuppressWarnings(true);
      in->processFile(path.c_str(), password.c_str());
      for (auto& ph : QPDFPageDocumentHelper(*in).getAllPages()) od.addPage(ph, false);
      keep.push_back(std::move(in));
    }
    std::filesystem::path tmp = std::filesystem::temp_directory_path() /
                                ("kura-merge-" + std::to_string(kura_getpid()) + ".pdf");
    QPDFWriter w(out, tmp.string().c_str());
    w.write();
    return tmp.string();
  } catch (const std::exception& e) {
    err = std::string("merge failed: ") + e.what();
    return std::string();
  }
}

struct BatchOptions {
  int jobs = 1;
  bool watch = false;
  bool active = false;
  bool recursive = false;
  bool overwrite = false;
  std::string outDir;
  std::string suffix;
};

int runOne(pdfa::Options opt, bool embedSource, const std::string& input,
           const std::string& output) {
  if (gWatchdogSeconds) armWatchdog(gWatchdogSeconds);
  std::ifstream in(input, std::ios::binary);
  if (!in) {
    std::cerr << "cannot open " << input << std::endl;
    return 2;
  }
  std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  in.close();

  if (embedSource) {
    opt.embedSource.assign(reinterpret_cast<const char*>(data.data()), data.size());
    if (opt.embedSourceName.empty()) {
      size_t slash = input.find_last_of("/\\");
      opt.embedSourceName = slash == std::string::npos ? input : input.substr(slash + 1);
    }
    if (opt.embedSourceMime.empty()) {
      opt.embedSourceMime = (data.size() > 4 && data[0] == 0xFF && data[1] == 0xD8)
                                ? "image/jpeg"
                                : "application/pdf";
    }
  }

  if (gPureUa) {
    pdfa::Result ua;
    ua.level = "ua1";
    if (!opt.verifyOnly) {
      ua.errorCode = "LEVEL_VERIFY_ONLY";
      ua.error = "ua1 is a check-only level; convert with --level 2u --ua";
    } else {
      try {
        for (const pdfa::UaFinding& f : pdfa::uaCheck(data.data(), data.size(), opt.password)) {
          ua.issues.push_back({"UA_" + f.checkpoint, f.detail, true, 3, std::vector<int>(f.pages.begin(), f.pages.end())});
        }
        ua.ok = true;
        ua.compliant = ua.issues.empty();
      } catch (const std::exception& e) {
        ua.errorCode = "PARSE_ERROR";
        ua.error = e.what();
      }
    }
    printReport(opt, ua, input, "");
    return !ua.ok ? kExitRejected : ua.compliant ? kExitOk : kExitFindings;
  }

#ifdef KURA_WITH_PDFIUM
  opt.rasterizePage = kura::makeRasterizer(data.data(), data.size(), opt.password);
#endif

  pdfa::Result res = pdfa::convert(data.data(), data.size(), opt);
  pdfa::levelFromString(res.level.substr(0, res.level.find('+')), opt.level);
  if (gOnlyPdfa && res.ok && !opt.verifyOnly && pdfa::levelFamily(opt.level) == pdfa::Family::PDFA) {
    pdfa::Options v;
    v.level = opt.level;
    v.verifyOnly = true;
    v.password = opt.password;
    pdfa::Result chk = pdfa::convert(res.pdf.data(), res.pdf.size(), v);
    if (!chk.compliant) {
      res.ok = false;
      res.errorCode = "NOT_PDFA";
      res.error = "the result does not pass PDF/A validation; no output written";
      res.pdf.clear();
    }
  }
  if (res.ok && !opt.verifyOnly) {
    std::ofstream out(output, std::ios::binary);
    if (!out) {
      std::cerr << "cannot write " << output << std::endl;
      return 2;
    }
    out.write(reinterpret_cast<const char*>(res.pdf.data()),
              static_cast<std::streamsize>(res.pdf.size()));
  }
  if (gCertify && res.ok && !opt.verifyOnly) {
    std::string tmp = output + ".certify.tmp";
    std::vector<std::string> args{output, tmp, "--level", pdfa::levelToString(opt.level)};
    if (!opt.password.empty()) args.insert(args.end(), {"--password", opt.password});
    std::streambuf* old = std::cout.rdbuf(nullptr);
    int crc = kuratools::toolReport("certify", args);
    std::cout.rdbuf(old);
    std::error_code ec;
    if (crc == 0 || crc == 1) std::filesystem::rename(tmp, output, ec);
    if (ec || (crc != 0 && crc != 1)) {
      std::filesystem::remove(tmp, ec);
      res.issues.push_back({"CERTIFY_FAILED", "could not embed the check certificate", false, 2});
    } else {
      res.issues.push_back({"CERTIFIED", "embedded a check certificate (verify with kura tool verify-cert)", true, 1});
    }
  }
  printReport(opt, res, input, (res.ok && !opt.verifyOnly) ? output : "");
  if (!res.ok) return kExitRejected;
  return (opt.verifyOnly && !res.compliant) ? kExitFindings : kExitOk;
}

int einvoiceExtract(const std::string& input, const std::string& output,
                    const std::string& password) {
  std::ifstream in(input, std::ios::binary);
  if (!in) {
    std::cerr << "cannot open " << input << std::endl;
    return 2;
  }
  std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  pdfa::InvoiceRead r = pdfa::readInvoice(data.data(), data.size(), password);
  if (!r.ok) {
    std::cout << "{\"ok\":false,\"errorCode\":\"PARSE_ERROR\",\"error\":\""
              << jsonEscape(r.error) << "\"}" << std::endl;
    return 2;
  }
  if (r.xml.empty()) {
    std::cout << "{\"ok\":false,\"errorCode\":\"NO_EINVOICE\",\"error\":\"no Factur-X, "
                 "ZUGFeRD, XRechnung or Order-X attachment found\"}"
              << std::endl;
    return 1;
  }
  if (output.empty()) {
    std::cout << r.xml;
  } else {
    std::ofstream out(output, std::ios::binary);
    if (!out) {
      std::cerr << "cannot write " << output << std::endl;
      return 2;
    }
    out.write(r.xml.data(), static_cast<std::streamsize>(r.xml.size()));
    std::cout << "{\"ok\":true,\"file\":\"" << jsonEscape(r.filename) << "\",\"bytes\":"
              << r.xml.size() << "}" << std::endl;
  }
  return 0;
}

int einvoiceValidate(const std::string& input, const std::string& password) {
  std::ifstream in(input, std::ios::binary);
  if (!in) {
    std::cerr << "cannot open " << input << std::endl;
    return 2;
  }
  std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  pdfa::InvoiceCheck c = pdfa::checkInvoice(data.data(), data.size(), password);
  if (!c.ok) {
    std::cout << "{\"ok\":false,\"errorCode\":\"" << jsonEscape(c.errorCode) << "\",\"error\":\""
              << jsonEscape(c.error) << "\"}" << std::endl;
    return 2;
  }
  if (!c.einvoice) {
    std::cout << "{\"ok\":true,\"einvoice\":false,\"error\":\"no e-invoice attachment\"}"
              << std::endl;
    return 1;
  }
  std::string json = "{\"ok\":true,\"einvoice\":true";
  json += ",\"standard\":\"" + jsonEscape(c.standard) + "\"";
  json += ",\"profile\":\"" + jsonEscape(c.profile) + "\"";
  json += ",\"documentType\":\"" + jsonEscape(c.documentType) + "\"";
  json += ",\"attachment\":\"" + jsonEscape(c.attachment) + "\"";
  json += ",\"consistent\":" + std::string(c.consistent ? "true" : "false");
  json += ",\"problems\":[";
  for (size_t i = 0; i < c.problems.size(); ++i) {
    json += (i ? ",\"" : "\"") + jsonEscape(c.problems[i]) + "\"";
  }
  json += "],\"warnings\":[";
  for (size_t i = 0; i < c.warnings.size(); ++i) {
    json += (i ? ",\"" : "\"") + jsonEscape(c.warnings[i]) + "\"";
  }
  json += "]}";
  std::cout << json << std::endl;
  return c.consistent ? 0 : 1;
}

std::vector<std::string> systemIccFolders() {
  std::vector<std::string> dirs;
  [[maybe_unused]] const char* home = std::getenv("HOME");
#ifdef _WIN32
  const char* windir = std::getenv("WINDIR");
  dirs.push_back(std::string(windir ? windir : "C:\\Windows") + "\\System32\\spool\\drivers\\color");
#elif defined(__APPLE__)
  if (home) dirs.push_back(std::string(home) + "/Library/ColorSync/Profiles");
  dirs.push_back("/Library/ColorSync/Profiles");
  dirs.push_back("/System/Library/ColorSync/Profiles");
#else
  if (home) dirs.push_back(std::string(home) + "/.color/icc");
  if (home) dirs.push_back(std::string(home) + "/.local/share/color/icc");
  dirs.push_back("/usr/share/color/icc");
  dirs.push_back("/usr/local/share/color/icc");
#endif
  return dirs;
}

bool configureColorJob(pdfa::Options& opt, const std::string& target, int intent, bool keepSpots,
                       unsigned objects) {
  using R = pdfa::ColorRule;
  pdfa::ColorJob& j = opt.colorJob;
  j.enabled = true;
  std::string t = lowerOf(target);
  if (t == "device-link") {
    j.embed = pdfa::ColorJob::kEmbedNone;
  } else if (t == "cmyk") {
    j.destProfile = opt.destProfile;
  } else if (t == "gray" || t == "grey") {
    j.destName = "Dot Gain 15%";
    j.embed = pdfa::ColorJob::kEmbedNone;
  } else if (t == "rgb" || t == "srgb") {
    j.destName = "sRGB IEC61966-2.1";
  } else {
    std::error_code ec;
    if (std::filesystem::is_regular_file(target, ec)) {
      if (!readFile(target.c_str(), j.destProfile)) return false;
    } else {
      j.destName = target;
    }
  }
  if (!j.destProfile.empty() && j.destProfile.size() >= 20 && j.destProfile.substr(16, 4) == "CMYK") {
    opt.destProfile = j.destProfile;
  }
  bool gray = t == "gray" || t == "grey" || (j.destProfile.size() >= 20 && j.destProfile.substr(16, 4) == "GRAY");
  bool rgb = t == "rgb" || t == "srgb" || (j.destProfile.size() >= 20 && j.destProfile.substr(16, 4) == "RGB ");
  auto add = [&](unsigned sources, R::Action a) {
    R r;
    r.objects = objects;
    r.sources = sources;
    r.action = a;
    r.intent = intent;
    j.rules.push_back(r);
  };
  if (gray || rgb) {
    add(R::kAnySource & ~(R::kSpot | R::kRegistration), R::kConvert);
  } else {
    add(R::kUncCmyk, R::kNone);
    add(R::kCalCmyk, R::kDecalibrate);
    add(R::kUncGray, j.grayToBlackOnly ? R::kConvert : R::kNone);
    add(R::kUncRgb | R::kCalRgb | R::kCalGray | R::kLab, R::kConvert);
  }
  add(R::kSpot | R::kRegistration, keepSpots ? R::kConvertAlternate : R::kConvert);
  return true;
}

void printUsage(std::ostream& out) {
  out << "usage: kura --level "
               "{1b,1a,2b,2u,2a,3b,3u,3a,4,4f,4e,x1a,x3,x4,x6,e1,vt1,vt3} [--ua] [--lang <tag>] "
               "(check only: x4p,x5g,x5n,x5pg,x6n,x6p,vt2,ua1) "
               "[--output-condition <name>] [--output-condition-info <text>] "
               "[--registry <url>] [--vt-records <ranges>] [--allow-visual-risk] "
               "[--password <pw>] <input.pdf> [output.pdf]\n"
               "       colour: [--convert-colors cmyk|gray|rgb|<icc file>|<profile name>] "
               "[--intent perceptual|relative|saturation|absolute] [--no-bpc] [--keep-spots] "
               "[--convert-objects text,vector,shading,images,lossy,lossless] [--gray-to-k] "
               "[--rgb-gray-to-k <pct>] [--ink-limit <pct>] [--device-link <icc>] "
               "[--icc-folder <dir>] [--dual (with 2b: also PDF/X-4, with 4: also PDF/X-6)]\n"
               "       kura --check --level <level> [options] <input.pdf>\n"
               "       profiles: [--profile <profile.json>] (level comes from the profile's convert section when --level is left out) "
               "[--var name=value] [--var-file <json>] [--pages 1-3,5|odd|even] [--merge-inputs a.pdf b.pdf ... out.pdf]\n"
               "       fallbacks: [--reconvert] [--fallback-raster <dpi>] [--fallback-pages <dpi>] [--fallback-no-text] "
               "[--only-pdfa] [--certify] [--quick] [--bookmarks <json|headings>]\n"
               "       jobs: [--job-id <id>] [--trace <log>] [--report <file.html|xml|pdf|txt|json>] "
               "[--report-lang en|de|fr|es|nl|it] [--report-dict <json>]\n"
               "       kura --list-profiles <dir> | --list-vars <profile.json>\n"
               "       kura tool <name> --help (reports, certification, separations, metadata, dieline)\n"
               "       kura --einvoice <invoice.xml> [--level 3b|3u|3a] <input.pdf> [output.pdf]\n"
               "       kura --extract-invoice <input.pdf> [out.xml]\n"
               "       kura --check-invoice <input.pdf>\n"
               "       kura --verify-password [--password <pw>] <input.pdf>\n"
               "       kura --level <level> --batch [-r] [-d <dir>] [-s <suffix>] [-w] [--jobs N] [--watch] <folder>\n"
               "\n"
               "exit status: 0 ok, 1 check found findings, 2 input rejected, 3 timeout, "
               "64 usage error"
            << std::endl;
}

int usage() {
  printUsage(std::cerr);
  return kExitUsage;
}
}

void pdfa_watchdog() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    long long deadline = gDeadlineMs.load();
    if (deadline && nowMs() >= deadline) break;
  }
  std::string partial;
  {
    std::lock_guard<std::mutex> lock(gTraceMu);
    for (const std::string& l : gTraceLines) partial += (partial.empty() ? "\"" : ",\"") + jsonEscape(l) + "\"";
    if (gTraceFile.is_open()) {
      gTraceFile << "timeout" << std::endl;
      gTraceFile.flush();
    }
  }
  std::string out = "{\n  \"ok\": false,\n  \"errorCode\": \"CONVERT_TIMEOUT\",\n"
                    "  \"error\": \"conversion exceeded the time budget; the input may be "
                    "malformed or crafted with pathological nesting\"";
  if (!gJobId.empty()) out += ",\n  \"jobId\": \"" + jsonEscape(gJobId) + "\"";
  if (!partial.empty()) out += ",\n  \"partial\": [" + partial + "]";
  out += "\n}\n";
  std::fputs(out.c_str(), stdout);
  std::fflush(stdout);
  std::_Exit(3);
}

#ifdef KURA_WITH_PDFIUM
int runCompare(const std::string& fileA, const std::string& fileB) {
  auto load = [](const std::string& p, std::vector<unsigned char>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
  };
  std::vector<unsigned char> a, b;
  if (!load(fileA, a) || !load(fileB, b)) {
    std::cerr << "cannot open input(s)" << std::endl;
    return 2;
  }
  kura::Rasterizer ra = kura::makeRasterizer(a.data(), a.size(), "");
  kura::Rasterizer rb = kura::makeRasterizer(b.data(), b.size(), "");
  const double dpi = 72.0;
  std::cout << "{\n  \"compare\": true,\n  \"pages\": [\n";
  bool first = true;
  int page = 0;
  long long totalChanged = 0;
  for (;; ++page) {
    int wa = 0, ha = 0, wb = 0, hb = 0;
    std::string ba, bb;
    bool okA = ra(page, dpi, wa, ha, ba);
    bool okB = rb(page, dpi, wb, hb, bb);
    if (!okA && !okB) break;
    if (!first) std::cout << ",\n";
    first = false;
    if (okA != okB) {
      std::cout << "    {\"page\": " << (page + 1) << ", \"status\": \""
                << (okA ? "removed" : "added") << "\"}";
      totalChanged += 1;
      continue;
    }
    long long changed = 0, considered = 0;
    double maxDelta = 0;
    double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    int w = std::min(wa, wb), h = std::min(ha, hb);
    if (wa != wb || ha != hb) changed += 1;
    const unsigned char* pa = reinterpret_cast<const unsigned char*>(ba.data());
    const unsigned char* pb = reinterpret_cast<const unsigned char*>(bb.data());
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        size_t ia = (static_cast<size_t>(y) * wa + x) * 3;
        size_t ib = (static_cast<size_t>(y) * wb + x) * 3;
        if (ia + 2 >= ba.size() || ib + 2 >= bb.size()) continue;
        ++considered;
        int d = std::abs(pa[ia] - pb[ib]) + std::abs(pa[ia + 1] - pb[ib + 1]) +
                std::abs(pa[ia + 2] - pb[ib + 2]);
        if (d > 24) {
          ++changed;
          double dd = d / 765.0 * 100.0;
          if (dd > maxDelta) maxDelta = dd;
          double px = x / dpi * 72.0;
          double py = (h - 1 - y) / dpi * 72.0;
          minX = std::min(minX, px);
          minY = std::min(minY, py);
          maxX = std::max(maxX, px);
          maxY = std::max(maxY, py);
        }
      }
    }
    totalChanged += changed;
    double pct = considered ? (100.0 * changed / considered) : 0.0;
    std::cout << "    {\"page\": " << (page + 1) << ", \"changedPixels\": " << changed
              << ", \"changedPercent\": " << (std::round(pct * 100) / 100)
              << ", \"maxDeltaPercent\": " << (std::round(maxDelta * 10) / 10);
    if (changed > 0 && maxX >= minX) {
      std::cout << ", \"changedRegion\": [" << (std::round(minX * 10) / 10) << ", "
                << (std::round(minY * 10) / 10) << ", " << (std::round(maxX * 10) / 10)
                << ", " << (std::round(maxY * 10) / 10) << "]";
    }
    std::cout << "}";
  }
  std::cout << "\n  ],\n  \"identical\": " << (totalChanged == 0 ? "true" : "false")
            << ",\n  \"pageCount\": " << page << "\n}\n";
  return 0;
}
#endif

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "tool") {
    return kuratools::runTool(argc - 2, argv + 2);
  }
#ifdef KURA_WITH_PDFIUM
  if (argc >= 4 && std::string(argv[1]) == "--compare") {
    return runCompare(argv[2], argv[3]);
  }
#endif
  const char* budget = std::getenv("PDFA_TIMEOUT");
  unsigned seconds = budget ? static_cast<unsigned>(std::atoi(budget)) : 120u;
  gWatchdogSeconds = seconds;
  if (seconds) {
    armWatchdog(seconds);
    std::thread(pdfa_watchdog).detach();
  }
  pdfa::Options opt;
  std::string input, output;
  bool haveLevel = false;
  bool einvoice = false;
  bool embedSource = false;
  bool extractInvoice = false;
  bool checkInvoice = false;
  bool verifyPw = false;
  std::string signP12, signPw;
  bool ocr = false;
  std::string ocrExe = "tesseract";
  BatchOptions batch;
  std::string colorTarget;
  int colorIntent = -1;
  bool keepSpots = false;
  unsigned colorObjects = pdfa::ColorRule::kAllObjects;
  std::vector<std::string> iccFolders = systemIccFolders();
  std::vector<std::string> positional;
  bool mergeInputs = false;
  std::string mergedTemp;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--version" || arg == "-v") {
      std::cout << pdfa::kEngineName << " (kura) " << pdfa::kEngineVersion << std::endl;
      return kExitOk;
    } else if (arg == "--help" || arg == "-h") {
      printUsage(std::cout);
      return kExitOk;
    } else if (arg == "--level" && i + 1 < argc) {
      std::string lv = argv[++i];
      gPureUa = lv == "ua1";
      if (gPureUa) opt.level = pdfa::Level::A2U;
      else if (!pdfa::levelFromString(lv, opt.level)) return usage();
      haveLevel = true;
    } else if (arg == "--check") {
      opt.verifyOnly = true;
    } else if (arg == "--analyze") {
      opt.analyze = true;
    } else if (arg == "--outline-fonts") {
      opt.outlineFonts = true;
    } else if (arg == "--profile" && i + 1 < argc) {
      opt.profilePath = argv[++i];
      if (!readFile(argv[i], opt.preflightProfile)) return 1;
    } else if (arg == "--ocr") {
      ocr = true;
    } else if (arg == "--ocr-engine" && i + 1 < argc) {
      ocrExe = argv[++i];
    } else if (arg == "--sign" && i + 1 < argc) {
      signP12 = argv[++i];
    } else if (arg == "--sign-password" && i + 1 < argc) {
      signPw = argv[++i];
    } else if (arg == "--sign-name" && i + 1 < argc) {
      opt.signName = argv[++i];
    } else if (arg == "--sign-reason" && i + 1 < argc) {
      opt.signReason = argv[++i];
    } else if (arg == "--sign-location" && i + 1 < argc) {
      opt.signLocation = argv[++i];
    } else if (arg == "--extract-invoice") {
      extractInvoice = true;
    } else if (arg == "--check-invoice") {
      checkInvoice = true;
    } else if (arg == "--verify-password") {
      verifyPw = true;
    } else if (arg == "--batch") {
      batch.active = true;
    } else if (arg == "--recursive" || arg == "-r") {
      batch.active = true;
      batch.recursive = true;
    } else if ((arg == "--out-dir" || arg == "-d") && i + 1 < argc) {
      batch.outDir = argv[++i];
    } else if ((arg == "--suffix" || arg == "-s") && i + 1 < argc) {
      batch.suffix = argv[++i];
    } else if ((arg == "--jobs" || arg == "-j") && i + 1 < argc) {
      batch.jobs = std::max(1, std::atoi(argv[++i]));
      batch.active = true;
    } else if (arg == "--watch") {
      batch.watch = true;
      batch.active = true;
    } else if (arg == "--overwrite" || arg == "-w") {
      batch.overwrite = true;
    } else if (arg == "--embed-source") {
      embedSource = true;
    } else if (arg == "--embed-source-name" && i + 1 < argc) {
      opt.embedSourceName = argv[++i];
    } else if (arg == "--raster-dpi" && i + 1 < argc) {
      opt.rasterDpi = std::atof(argv[++i]);
      if (opt.rasterDpi < 24 || opt.rasterDpi > 1200) {
        std::cerr << "--raster-dpi must be between 24 and 1200" << std::endl;
        return 1;
      }
    } else if (arg == "--rasterize-pages") {
      opt.rasterizeAllPages = true;
    } else if (arg == "--font-folder" && i + 1 < argc) {
      opt.fontFolder = argv[++i];
    } else if (arg == "--substitute" && i + 1 < argc) {
      std::string spec = argv[++i];
      size_t eq = spec.find('=');
      if (eq == std::string::npos) {
        std::cerr << "--substitute expects <missing-font>=<replacement>" << std::endl;
        return 1;
      }
      opt.fontSubstitutions.emplace_back(spec.substr(0, eq), spec.substr(eq + 1));
    } else if (arg == "--default-rgb" && i + 1 < argc) {
      if (!readFile(argv[++i], opt.defaultRgbProfile)) return 1;
    } else if (arg == "--default-cmyk" && i + 1 < argc) {
      if (!readFile(argv[++i], opt.defaultCmykProfile)) return 1;
    } else if (arg == "--default-gray" && i + 1 < argc) {
      if (!readFile(argv[++i], opt.defaultGrayProfile)) return 1;
    } else if (arg == "--allow-visual-risk") {
      opt.allowVisualRisk = true;
    } else if (arg == "--ua") {
      opt.ua = true;
    } else if (arg == "--lang" && i + 1 < argc) {
      opt.docLang = argv[++i];
    } else if (arg == "--output-condition" && i + 1 < argc) {
      opt.outputConditionIdentifier = argv[++i];
    } else if (arg == "--output-condition-info" && i + 1 < argc) {
      opt.outputConditionInfo = argv[++i];
    } else if (arg == "--registry" && i + 1 < argc) {
      opt.outputConditionRegistry = argv[++i];
    } else if (arg == "--vt-records" && i + 1 < argc) {
      opt.vtRecords = argv[++i];
    } else if (arg == "--dest-profile" && i + 1 < argc) {
      std::ifstream pf(argv[++i], std::ios::binary);
      if (!pf) { std::cerr << "cannot open profile" << std::endl; return 1; }
      opt.destProfile.assign((std::istreambuf_iterator<char>(pf)),
                             std::istreambuf_iterator<char>());
    } else if (arg == "--einvoice" && i + 1 < argc) {
      std::ifstream xf(argv[++i], std::ios::binary);
      if (!xf) { std::cerr << "cannot open invoice xml" << std::endl; return 1; }
      opt.attachXml.assign((std::istreambuf_iterator<char>(xf)),
                           std::istreambuf_iterator<char>());
      einvoice = true;
    } else if (arg == "--attach-xml" && i + 1 < argc) {
      std::ifstream xf(argv[++i], std::ios::binary);
      if (!xf) { std::cerr << "cannot open xml" << std::endl; return 1; }
      opt.attachXml.assign((std::istreambuf_iterator<char>(xf)),
                           std::istreambuf_iterator<char>());
    } else if (arg == "--attach-xml-name" && i + 1 < argc) {
      opt.attachXmlName = argv[++i];
    } else if (arg == "--facturx-profile" && i + 1 < argc) {
      opt.facturxProfile = argv[++i];
    } else if (arg == "--image-max-ppi" && i + 1 < argc) {
      opt.imageMaxPpi = std::atof(argv[++i]);
      if (!(opt.imageMaxPpi >= 0) || opt.imageMaxPpi > 10000) {
        std::cerr << "--image-max-ppi must be between 0 and 10000" << std::endl;
        return kExitUsage;
      }
    } else if (arg == "--password" && i + 1 < argc) {
      opt.password = argv[++i];
    } else if (arg == "--profile-url" && i + 1 < argc) {
      opt.externalProfileUrl = argv[++i];
    } else if (arg == "--visible-layers") {
      opt.visibleLayersOnly = true;
    } else if (arg == "--hits-per-page" && i + 1 < argc) {
      opt.hitsPerPage = std::max(0, std::atoi(argv[++i]));
    } else if (arg == "--hits-per-doc" && i + 1 < argc) {
      opt.hitsPerDoc = std::max(0, std::atoi(argv[++i]));
    } else if (arg == "--dual") {
      opt.dualPrint = true;
    } else if (arg == "--convert-colors" && i + 1 < argc) {
      colorTarget = argv[++i];
    } else if (arg == "--intent" && i + 1 < argc) {
      std::string v = lowerOf(argv[++i]);
      colorIntent = v == "perceptual" ? 0 : v == "relative" ? 1 : v == "saturation" ? 2
                    : v == "absolute" ? 3 : -2;
      if (colorIntent == -2) {
        std::cerr << "--intent must be perceptual, relative, saturation or absolute" << std::endl;
        return kExitUsage;
      }
    } else if (arg == "--no-bpc") {
      opt.colorJob.blackPointCompensation = false;
    } else if (arg == "--keep-spots") {
      keepSpots = true;
    } else if (arg == "--convert-objects" && i + 1 < argc) {
      colorObjects = 0;
      std::string list = lowerOf(argv[++i]) + ",";
      size_t start = 0, comma;
      while ((comma = list.find(',', start)) != std::string::npos) {
        std::string w = list.substr(start, comma - start);
        start = comma + 1;
        if (w == "text") colorObjects |= pdfa::ColorRule::kText;
        else if (w == "vector") colorObjects |= pdfa::ColorRule::kVector;
        else if (w == "shading" || w == "shadings") colorObjects |= pdfa::ColorRule::kShading;
        else if (w == "images") colorObjects |= pdfa::ColorRule::kImageLossy | pdfa::ColorRule::kImageLossless;
        else if (w == "lossy") colorObjects |= pdfa::ColorRule::kImageLossy;
        else if (w == "lossless") colorObjects |= pdfa::ColorRule::kImageLossless;
        else if (!w.empty()) {
          std::cerr << "--convert-objects takes text, vector, shading, images, lossy, lossless" << std::endl;
          return kExitUsage;
        }
      }
    } else if (arg == "--gray-to-k") {
      opt.colorJob.grayToBlackOnly = true;
    } else if (arg == "--rgb-gray-to-k" && i + 1 < argc) {
      double pct = std::atof(argv[++i]);
      opt.colorJob.rgbGrayToBlackOnly = true;
      for (double& t : opt.colorJob.rgbGrayTolerance) t = pct / 100.0;
    } else if (arg == "--ink-limit" && i + 1 < argc) {
      opt.colorJob.inkLimit = std::atof(argv[++i]);
      if (!(opt.colorJob.inkLimit >= 100 && opt.colorJob.inkLimit <= 400)) {
        std::cerr << "--ink-limit must be between 100 and 400" << std::endl;
        return kExitUsage;
      }
    } else if (arg == "--device-link" && i + 1 < argc) {
      if (!readFile(argv[++i], opt.colorJob.deviceLink)) return 1;
      if (colorTarget.empty()) colorTarget = "device-link";
    } else if (arg == "--var" && i + 1 < argc) {
      std::string kv = argv[++i];
      size_t eq = kv.find('=');
      if (eq == std::string::npos) return usage();
      opt.profileVars[kv.substr(0, eq)] = kv.substr(eq + 1);
    } else if (arg == "--var-file" && i + 1 < argc) {
      std::string txt;
      if (!readFile(argv[++i], txt)) return 1;
      if (!parseVarFile(txt, opt.profileVars)) return kExitUsage;
    } else if (arg == "--pages" && i + 1 < argc) {
      opt.pageRange = argv[++i];
    } else if (arg == "--merge-inputs") {
      mergeInputs = true;
    } else if (arg == "--reconvert") {
      opt.reconvert = true;
    } else if ((arg == "--fallback-raster" || arg == "--fallback-pages") && i + 1 < argc) {
      opt.fallbackProblemPagesOnly = arg == "--fallback-pages";
      opt.fallbackDpi = std::atof(argv[++i]);
      if (opt.fallbackDpi < 24 || opt.fallbackDpi > 1200) {
        std::cerr << arg << " needs a resolution between 24 and 1200" << std::endl;
        return kExitUsage;
      }
    } else if (arg == "--fallback-no-text") {
      opt.rasterKeepText = false;
    } else if (arg == "--quick" || arg == "-q") {
      opt.quickCheck = true;
    } else if (arg == "--certify") {
      gCertify = true;
    } else if (arg == "--only-pdfa") {
      gOnlyPdfa = true;
    } else if (arg == "--bookmarks" && i + 1 < argc) {
      std::string b = argv[++i];
      if (b == "headings") opt.bookmarks = b;
      else if (!readFile(b.c_str(), opt.bookmarks)) return 1;
    } else if (arg == "--trace" && i + 1 < argc) {
      gTraceFile.open(argv[++i], std::ios::app);
      if (!gTraceFile) {
        std::cerr << "cannot write trace file" << std::endl;
        return 1;
      }
    } else if (arg == "--list-vars" && i + 1 < argc) {
      return listVars(argv[++i]);
    } else if (arg == "--list-profiles" && i + 1 < argc) {
      return listProfiles(argv[++i]);
    } else if (arg == "--report" && i + 1 < argc) {
      gReportPath = argv[++i];
    } else if (arg == "--report-dict" && i + 1 < argc) {
      gReportDict = argv[++i];
    } else if (arg == "--report-lang" && i + 1 < argc) {
      gReportLang = argv[++i];
    } else if (arg == "--job-id" && i + 1 < argc) {
      gJobId = argv[++i];
    } else if (arg == "--icc-folder" && i + 1 < argc) {
      iccFolders.insert(iccFolders.begin(), argv[++i]);
    } else {
      positional.push_back(arg);
    }
  }
  if (mergeInputs && positional.size() >= 2) {
    output = positional.back();
    positional.pop_back();
    std::string err;
    input = mergePdfs(positional, opt.password, err);
    if (input.empty()) {
      std::cerr << err << std::endl;
      return 2;
    }
    mergedTemp = input;
  } else if (positional.size() > 2) {
    return usage();
  } else {
    if (!positional.empty()) input = positional[0];
    if (positional.size() > 1) output = positional[1];
  }
  opt.inputName = input;
  opt.renderHtml = [](const std::string& src, const std::string& headScript, std::string& pdf) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("kura-html-" + std::to_string(kura_getpid()) + "-" + std::to_string(nowMs()));
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path page = dir / "index.html";
    if (src.size() > 4 && src.compare(0, 2, "PK") == 0) {
      std::ofstream(dir / "t.zip", std::ios::binary).write(src.data(), static_cast<std::streamsize>(src.size()));
#ifdef _WIN32
      std::vector<std::string> unzip{"tar", "-xf", (dir / "t.zip").string(), "-C", dir.string()};
#else
      std::vector<std::string> unzip{"unzip", "-qo", (dir / "t.zip").string(), "-d", dir.string()};
#endif
      if (kuratools::runProcess(unzip, 120) != 0) {
        std::filesystem::remove_all(dir, ec);
        return false;
      }
      if (!std::filesystem::exists(page)) {
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
          if (it->path().extension() == ".html") {
            page = it->path();
            break;
          }
        }
      }
    } else {
      std::ofstream(page, std::ios::binary).write(src.data(), static_cast<std::streamsize>(src.size()));
    }
    if (!headScript.empty()) {
      std::string doc;
      readFile(page.string().c_str(), doc);
      std::string tag = "<script>" + headScript + "</script>";
      std::string low = doc;
      for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      size_t at = std::string::npos;
      for (const char* t : {"<head>", "<head ", "<html>", "<html "}) {
        size_t h = low.find(t);
        if (h != std::string::npos && at == std::string::npos) at = low.find('>', h);
      }
      doc.insert(at == std::string::npos ? 0 : at + 1, tag);
      std::ofstream(page, std::ios::binary).write(doc.data(), static_cast<std::streamsize>(doc.size()));
    }
    std::string out = (dir / "out.pdf").string();
    bool ok = kuratools::renderHtmlFile(page.string(), out, dir.string()) && readFile(out.c_str(), pdf);
    std::filesystem::remove_all(dir, ec);
    return ok;
  };
  if (!haveLevel && !opt.preflightProfile.empty()) {
    opt.levelFromProfile = true;
    haveLevel = true;
  }
  if (gTraceFile.is_open() || gWatchdogSeconds) {
    opt.trace = [](const std::string& line) {
      std::lock_guard<std::mutex> lock(gTraceMu);
      long long t = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
      if (gTraceLines.size() < 2000) gTraceLines.push_back(line);
      if (gTraceFile.is_open()) {
        gTraceFile << t << (gJobId.empty() ? "" : " [" + gJobId + "]") << " " << line << "\n";
        gTraceFile.flush();
      }
    };
  }
  opt.inputPath = input;
  if (verifyPw) {
    if (input.empty()) return usage();
    std::string bytes;
    if (!readFile(input.c_str(), bytes)) return kExitRejected;
    const bool valid = pdfa::verifyPassword(
        reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), opt.password);
    std::cout << "{\"ok\":true,\"valid\":" << (valid ? "true" : "false") << "}" << std::endl;
    return valid ? kExitOk : kExitFindings;
  }
  if (extractInvoice || checkInvoice) {
    if (input.empty()) return usage();
    return extractInvoice ? einvoiceExtract(input, output, opt.password)
                          : einvoiceValidate(input, opt.password);
  }
  if (einvoice && !haveLevel) {
    opt.level = pdfa::Level::A3B;
    haveLevel = true;
  }
  if (!haveLevel || input.empty()) return usage();
  if (!batch.active && !batch.outDir.empty()) batch.active = true;
  if (batch.active) {
    if (!output.empty()) return usage();
    if (!opt.verifyOnly && batch.outDir.empty() && batch.suffix.empty() && !batch.overwrite) {
      batch.suffix = "_pdfa";
    }
  } else if (opt.verifyOnly) {
    if (!output.empty()) return usage();
  } else if (output.empty()) {
    output = defaultOutputPath(input, opt);
  }

  if (ocr) {
    std::string exe = ocrExe;
    opt.ocrPage = [exe](int page, double dpi, int w, int h, const std::string& rgb,
                        std::vector<pdfa::Options::OcrWord>& words) {
      return runTesseract(exe, page, dpi, w, h, rgb, words);
    };
  }

  if (!signP12.empty()) {
#ifdef KURA_WITH_SIGNING
    static kura::SigningKey sk;
    std::string err;
    if (!kura::loadPkcs12(signP12, signPw, sk, err)) {
      std::cerr << err << std::endl;
      return 1;
    }
    opt.signDocument = [](const std::string& data, std::string& der) {
      std::string e;
      if (!kura::signDetached(sk, data, der, e)) {
        std::cerr << e << std::endl;
        return false;
      }
      return true;
    };
#else
    std::cerr << "this build has no signing support (needs OpenSSL)" << std::endl;
    return 1;
#endif
  }

  opt.loadIcc = [iccFolders](const std::string& name, std::string& bytes) {
    return pdfa::loadIccFromFolders(iccFolders, name, bytes);
  };
  if (!colorTarget.empty()) {
    if (!configureColorJob(opt, colorTarget, colorIntent, keepSpots, colorObjects)) {
      std::cerr << "--convert-colors takes cmyk, gray, rgb, an ICC file or a profile name"
                << std::endl;
      return kExitUsage;
    }
  }

  if (!opt.fontFolder.empty()) {
    std::string folder = opt.fontFolder;
    opt.loadFont = [folder](const std::string& wanted, std::string& psName,
                            std::string& bytes) {
      return pdfa::loadFontFromFolder(folder, wanted, psName, bytes);
    };
  }

  if (batch.active) {
    auto collect = [&](std::vector<std::string>& inputs) {
      std::error_code ec;
      inputs.clear();
      if (std::filesystem::is_directory(input, ec)) {
        auto take = [&](const std::filesystem::directory_entry& e) {
          if (e.is_regular_file() && lowerOf(e.path().extension().string()) == ".pdf") inputs.push_back(e.path().string());
        };
        if (batch.recursive) {
          for (const auto& e : std::filesystem::recursive_directory_iterator(input, ec)) {
            if (batch.watch && e.path().generic_string().find("/done/") != std::string::npos) continue;
            take(e);
          }
        } else {
          for (const auto& e : std::filesystem::directory_iterator(input, ec)) take(e);
        }
      } else if (!batch.watch) {
        inputs.push_back(input);
      }
      std::sort(inputs.begin(), inputs.end());
    };
    auto dstFor = [&](const std::filesystem::path& src, std::filesystem::path& dst) {
      if (opt.verifyOnly) return true;
      std::error_code ec;
      std::filesystem::path dir = batch.outDir.empty() ? src.parent_path() : std::filesystem::path(batch.outDir);
      std::filesystem::create_directories(dir, ec);
      dst = dir / (src.stem().string() + batch.suffix + ".pdf");
      if (std::filesystem::exists(dst) && !batch.overwrite && std::filesystem::equivalent(dst, src, ec)) {
        std::cerr << "refusing to overwrite input " << dst << "; use --suffix or -w" << std::endl;
        return false;
      }
      return true;
    };
    std::vector<std::string> childArgs;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--batch" || a == "--recursive" || a == "-r" || a == "--overwrite" || a == "-w" || a == "--watch") continue;
      if (a == "--jobs" || a == "-j" || a == "--out-dir" || a == "-d" || a == "--suffix" || a == "-s" ||
          a == "--report" || a == "--trace") {
        ++i;
        continue;
      }
      if (a == input) continue;
      childArgs.push_back(a);
    }
    auto runBatch = [&](const std::vector<std::string>& inputs, int& failed, int& nonCompliant) {
      std::vector<std::string> outputs(inputs.size());
      std::vector<int> codes(inputs.size(), 0);
      if (batch.jobs > 1 && inputs.size() > 1) {
        std::atomic<size_t> next{0};
        std::string self = argv[0];
        auto worker = [&]() {
          for (;;) {
            size_t i = next++;
            if (i >= inputs.size()) return;
            std::filesystem::path dst;
            if (!dstFor(inputs[i], dst)) {
              codes[i] = 2;
              continue;
            }
            std::vector<std::string> cmd{self};
            cmd.insert(cmd.end(), childArgs.begin(), childArgs.end());
            cmd.push_back(inputs[i]);
            if (!opt.verifyOnly) cmd.push_back(dst.string());
            int rc = kuratools::runProcess(cmd, 0, &outputs[i]);
            codes[i] = rc >= 0 && rc <= 3 ? rc : 2;
            outputs[i].erase(std::remove(outputs[i].begin(), outputs[i].end(), '\r'), outputs[i].end());
            while (!outputs[i].empty() && (outputs[i].back() == '\n' || outputs[i].back() == '\r')) outputs[i].pop_back();
          }
        };
        std::vector<std::thread> pool;
        for (int t = 0; t < batch.jobs; ++t) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
        for (size_t i = 0; i < inputs.size(); ++i) {
          std::cout << (outputs[i].empty() ? "{\"file\":\"" + jsonEscape(inputs[i]) + "\",\"ok\":false}" : outputs[i])
                    << (i + 1 < inputs.size() ? "," : "") << std::endl;
        }
      } else {
        for (size_t i = 0; i < inputs.size(); ++i) {
          std::filesystem::path dst;
          if (!dstFor(inputs[i], dst)) {
            codes[i] = 2;
            continue;
          }
          codes[i] = runOne(opt, embedSource, inputs[i], dst.string());
          std::cout << (i + 1 < inputs.size() ? "," : "") << std::endl;
        }
      }
      for (int c : codes) {
        if (c == 2 || c == 3) ++failed;
        if (c == 1) ++nonCompliant;
      }
      return codes;
    };
    if (batch.watch) {
      std::filesystem::path done = std::filesystem::path(input) / "done";
      std::error_code ec;
      std::filesystem::create_directories(done, ec);
      gWatchdogSeconds = 0;
      gDeadlineMs.store(0);
      if (batch.jobs < 2) batch.jobs = 2;
      std::cerr << "watching " << input << " (processed files move to " << done.string() << ")" << std::endl;
      for (;;) {
        std::vector<std::string> inputs;
        collect(inputs);
        std::vector<std::string> ready;
        for (const std::string& f : inputs) {
          auto t = std::filesystem::last_write_time(f, ec);
          if (!ec && std::filesystem::file_time_type::clock::now() - t > std::chrono::seconds(2)) ready.push_back(f);
        }
        if (!ready.empty()) {
          int failed = 0, nonCompliant = 0;
          std::cout << "[" << std::endl;
          runBatch(ready, failed, nonCompliant);
          std::cout << "]" << std::endl;
          for (const std::string& f : ready) {
            std::filesystem::rename(f, done / std::filesystem::path(f).filename(), ec);
          }
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
    }
    std::vector<std::string> inputs;
    collect(inputs);
    if (inputs.empty()) {
      std::cerr << "no PDF files found under " << input << std::endl;
      return 1;
    }
    int failed = 0, nonCompliant = 0;
    std::cout << "[" << std::endl;
    runBatch(inputs, failed, nonCompliant);
    std::cout << "]" << std::endl;
    std::cerr << "batch: " << inputs.size() << " file(s), " << failed << " rejected, "
              << nonCompliant << " non-compliant" << std::endl;
    return failed ? 2 : (nonCompliant ? 1 : 0);
  }

  int rc = runOne(opt, embedSource, input, output);
  if (!mergedTemp.empty()) std::filesystem::remove(mergedTemp);
  return rc;
}
