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
#include <string>
#include <thread>
#include <vector>

#include "pdfa/pdfa.hh"
#include "../core/src/einvoice.hh"
#include "signer.hh"
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

void printReport(const pdfa::Options& opt, const pdfa::Result& res,
                 const std::string& source, const std::string& output = "") {
  std::string json = "{";
  if (!source.empty()) json += "\"file\":\"" + jsonEscape(source) + "\",";
  if (!output.empty()) json += "\"output\":\"" + jsonEscape(output) + "\",";
  json += "\"ok\":" + std::string(res.ok ? "true" : "false");
  json += ",\"level\":\"" + pdfa::levelToString(opt.level) + "\"";
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
            "\",\"fixed\":" + (is.fixed ? "true" : "false") + "}";
  }
  json += "]";
  if (opt.analyze || !opt.preflightProfile.empty()) {
    json += ",\"analysis\":[";
    bool firstA = true;
    for (const pdfa::Issue& is : res.analysis) {
      if (!firstA) json += ",";
      firstA = false;
      json += "{\"code\":\"" + jsonEscape(is.code) + "\",\"detail\":\"" +
              jsonEscape(is.detail) + "\"}";
    }
    json += "]";
  }
  json += "}";
  std::cout << json << std::endl;
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

bool loadFontFromFolder(const std::string& folder, const std::string& wanted,
                        std::string& psName, std::string& bytes) {
  std::error_code ec;
  std::string want = lowerOf(wanted);
  want.erase(std::remove_if(want.begin(), want.end(),
                            [](unsigned char c) { return c == ' ' || c == '-' || c == '_'; }),
             want.end());
  for (const auto& e : std::filesystem::recursive_directory_iterator(folder, ec)) {
    if (ec) break;
    if (!e.is_regular_file()) continue;
    std::string ext = lowerOf(e.path().extension().string());
    if (ext != ".ttf" && ext != ".ttc" && ext != ".otf") continue;
    std::string stem = lowerOf(e.path().stem().string());
    stem.erase(std::remove_if(stem.begin(), stem.end(),
                              [](unsigned char c) { return c == ' ' || c == '-' || c == '_'; }),
               stem.end());
    if (stem != want) continue;
    std::ifstream f(e.path(), std::ios::binary);
    if (!f) continue;
    bytes.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    psName = e.path().stem().string();
    return !bytes.empty();
  }
  return false;
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
  auto quoted = [](const std::string& raw) {
    std::string out = "'";
    for (char c : raw) {
      if (c == '\'') out += "'\\''";
      else out += c;
    }
    return out + "'";
  };
  std::string cmd = quoted(exe) + " " + quoted(ppm.string()) + " " + quoted(tmp.string()) +
                    " tsv 2>/dev/null";
  int rc = std::system(cmd.c_str());
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

struct BatchOptions {
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

#ifdef KURA_WITH_PDFIUM
  opt.rasterizePage = kura::makeRasterizer(data.data(), data.size(), opt.password);
#endif

  pdfa::Result res = pdfa::convert(data.data(), data.size(), opt);
  if (res.ok && !opt.verifyOnly) {
    std::ofstream out(output, std::ios::binary);
    if (!out) {
      std::cerr << "cannot write " << output << std::endl;
      return 2;
    }
    out.write(reinterpret_cast<const char*>(res.pdf.data()),
              static_cast<std::streamsize>(res.pdf.size()));
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
  pdfa::InvoiceRead r = pdfa::readInvoice(data.data(), data.size(), password);
  std::vector<std::string> problems, warnings;
  if (!r.ok) {
    std::cout << "{\"ok\":false,\"errorCode\":\"PARSE_ERROR\",\"error\":\""
              << jsonEscape(r.error) << "\"}" << std::endl;
    return 2;
  }
  if (r.xml.empty()) {
    std::cout << "{\"ok\":true,\"einvoice\":false,\"error\":\"no e-invoice attachment\"}"
              << std::endl;
    return 1;
  }
  pdfa::InvoiceProfile want = pdfa::detectInvoice(r.xml, "", "");
  if (!want.detected) problems.push_back("payload declares no recognised guideline URN");
  if (r.filename != want.filename) {
    problems.push_back("attachment is named \"" + r.filename + "\" but " + want.standard +
                       " " + want.profile + " requires \"" + want.filename + "\"");
  }
  if (r.relationship != want.relationship) {
    bool headerOnly = want.profile == "MINIMUM" || want.profile == "BASIC WL";
    std::string msg = "AFRelationship is " + r.relationship + " but " + want.profile +
                      " normally uses " + want.relationship;
    if (headerOnly) {
      warnings.push_back(msg +
                         " (Factur-X 6.2.2 ties this to whether the page carries more "
                         "invoice data than the XML, which a reader cannot verify)");
    } else {
      problems.push_back(msg);
    }
  }
  if (!r.hasAf) problems.push_back("catalog has no /AF array");
  std::string xmpName = pdfa::xmpValue(r.xmp, "DocumentFileName");
  std::string xmpConf = pdfa::xmpValue(r.xmp, "ConformanceLevel");
  std::string xmpType = pdfa::xmpValue(r.xmp, "DocumentType");
  if (xmpName.empty() && xmpConf.empty()) {
    problems.push_back("XMP carries no e-invoice extension schema");
  } else {
    if (xmpName != r.filename) {
      problems.push_back("XMP DocumentFileName \"" + xmpName +
                         "\" does not match the attachment \"" + r.filename + "\"");
    }
    if (!xmpConf.empty() && xmpConf != want.profile) {
      problems.push_back("XMP ConformanceLevel \"" + xmpConf + "\" but the payload declares " +
                         want.profile);
    }
    if (!xmpType.empty() && xmpType != want.documentType) {
      problems.push_back("XMP DocumentType \"" + xmpType + "\" but the payload is a " +
                         want.documentType);
    }
  }
  std::string json = "{\"ok\":true,\"einvoice\":true";
  json += ",\"standard\":\"" + jsonEscape(want.standard) + "\"";
  json += ",\"profile\":\"" + jsonEscape(want.profile) + "\"";
  json += ",\"documentType\":\"" + jsonEscape(want.documentType) + "\"";
  json += ",\"attachment\":\"" + jsonEscape(r.filename) + "\"";
  json += ",\"consistent\":" + std::string(problems.empty() ? "true" : "false");
  json += ",\"problems\":[";
  for (size_t i = 0; i < problems.size(); ++i) {
    json += (i ? ",\"" : "\"") + jsonEscape(problems[i]) + "\"";
  }
  json += "],\"warnings\":[";
  for (size_t i = 0; i < warnings.size(); ++i) {
    json += (i ? ",\"" : "\"") + jsonEscape(warnings[i]) + "\"";
  }
  json += "]}";
  std::cout << json << std::endl;
  return problems.empty() ? 0 : 1;
}

void printUsage(std::ostream& out) {
  out << "usage: kura --level "
               "{1b,1a,2b,2u,2a,3b,3u,3a,4,4f,4e,x1a,x3,x4,x6,e1,vt1,vt3} [--ua] [--lang <tag>] "
               "(check only: x4p,x5g,x5n,x5pg,x6n,x6p,vt2) "
               "[--output-condition <name>] [--output-condition-info <text>] "
               "[--registry <url>] [--vt-records <ranges>] [--allow-visual-risk] "
               "[--password <pw>] <input.pdf> [output.pdf]\n"
               "       kura --check --level <level> [options] <input.pdf>\n"
               "       kura --einvoice <invoice.xml> [--level 3b|3u|3a] <input.pdf> [output.pdf]\n"
               "       kura --extract-invoice <input.pdf> [out.xml]\n"
               "       kura --check-invoice <input.pdf>\n"
               "       kura --verify-password [--password <pw>] <input.pdf>\n"
               "       kura --level <level> --batch [-r] [-d <dir>] [-s <suffix>] [-w] <folder>\n"
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
  std::fputs(
      "{\n  \"ok\": false,\n  \"errorCode\": \"CONVERT_TIMEOUT\",\n"
      "  \"error\": \"conversion exceeded the time budget; the input may be "
      "malformed or crafted with pathological nesting\"\n}\n",
      stdout);
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
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--version" || arg == "-v") {
      std::cout << pdfa::kEngineName << " (kura) " << pdfa::kEngineVersion << std::endl;
      return kExitOk;
    } else if (arg == "--help" || arg == "-h") {
      printUsage(std::cout);
      return kExitOk;
    } else if (arg == "--level" && i + 1 < argc) {
      if (!pdfa::levelFromString(argv[++i], opt.level)) return usage();
      haveLevel = true;
    } else if (arg == "--check") {
      opt.verifyOnly = true;
    } else if (arg == "--analyze") {
      opt.analyze = true;
    } else if (arg == "--outline-fonts") {
      opt.outlineFonts = true;
    } else if (arg == "--profile" && i + 1 < argc) {
      if (!readFile(argv[++i], opt.preflightProfile)) return 1;
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
    } else if (input.empty()) {
      input = arg;
    } else if (output.empty()) {
      output = arg;
    } else {
      return usage();
    }
  }
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

  if (!opt.fontFolder.empty()) {
    std::string folder = opt.fontFolder;
    opt.loadFont = [folder](const std::string& wanted, std::string& psName,
                            std::string& bytes) {
      return loadFontFromFolder(folder, wanted, psName, bytes);
    };
  }

  if (batch.active) {
    std::vector<std::string> inputs;
    std::error_code ec;
    if (std::filesystem::is_directory(input, ec)) {
      if (batch.recursive) {
        for (const auto& e : std::filesystem::recursive_directory_iterator(input, ec)) {
          if (e.is_regular_file() && lowerOf(e.path().extension().string()) == ".pdf") {
            inputs.push_back(e.path().string());
          }
        }
      } else {
        for (const auto& e : std::filesystem::directory_iterator(input, ec)) {
          if (e.is_regular_file() && lowerOf(e.path().extension().string()) == ".pdf") {
            inputs.push_back(e.path().string());
          }
        }
      }
    } else {
      inputs.push_back(input);
    }
    std::sort(inputs.begin(), inputs.end());
    if (inputs.empty()) {
      std::cerr << "no PDF files found under " << input << std::endl;
      return 1;
    }
    int failed = 0, nonCompliant = 0;
    std::cout << "[" << std::endl;
    for (size_t i = 0; i < inputs.size(); ++i) {
      std::filesystem::path src(inputs[i]);
      std::filesystem::path dst;
      if (!opt.verifyOnly) {
        std::filesystem::path dir =
            batch.outDir.empty() ? src.parent_path() : std::filesystem::path(batch.outDir);
        std::filesystem::create_directories(dir, ec);
        dst = dir / (src.stem().string() + batch.suffix + ".pdf");
        if (std::filesystem::exists(dst) && !batch.overwrite &&
            std::filesystem::equivalent(dst, src, ec)) {
          std::cerr << "refusing to overwrite input " << dst << "; use --suffix or -w"
                    << std::endl;
          ++failed;
          continue;
        }
      }
      int rc = runOne(opt, embedSource, inputs[i], dst.string());
      if (rc == 2) ++failed;
      if (rc == 1) ++nonCompliant;
      std::cout << (i + 1 < inputs.size() ? "," : "") << std::endl;
    }
    std::cout << "]" << std::endl;
    std::cerr << "batch: " << inputs.size() << " file(s), " << failed << " rejected, "
              << nonCompliant << " non-compliant" << std::endl;
    return failed ? 2 : (nonCompliant ? 1 : 0);
  }

  return runOne(opt, embedSource, input, output);
}
