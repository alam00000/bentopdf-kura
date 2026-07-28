#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "pdfa/pdfa.hh"
#ifdef KURA_WITH_PDFIUM
#include "kura/raster.hh"
#endif

namespace {
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

void printReport(const pdfa::Options& opt, const pdfa::Result& res) {
  std::string json = "{";
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
  json += ",\"issues\":[";
  for (size_t i = 0; i < res.issues.size(); ++i) {
    const pdfa::Issue& is = res.issues[i];
    if (i) json += ",";
    json += "{\"code\":\"" + jsonEscape(is.code) + "\",\"detail\":\"" + jsonEscape(is.detail) +
            "\",\"fixed\":" + (is.fixed ? "true" : "false") + "}";
  }
  json += "]}";
  std::cout << json << std::endl;
}

int usage() {
  std::cerr << "usage: kura --level "
               "{1b,1a,2b,2u,2a,3b,3u,3a,4,4f,4e,x1a,x3,x4,x6,e1,vt1,vt3} [--ua] [--lang <tag>] "
               "[--output-condition <name>] [--output-condition-info <text>] "
               "[--registry <url>] [--vt-records <ranges>] [--allow-visual-risk] "
               "[--password <pw>] <input.pdf> <output.pdf>"
            << std::endl;
  return 1;
}
}

void pdfa_watchdog(unsigned seconds) {
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  std::fputs(
      "{\n  \"ok\": false,\n  \"errorCode\": \"CONVERT_TIMEOUT\",\n"
      "  \"error\": \"conversion exceeded the time budget; the input may be "
      "malformed or crafted with pathological nesting\"\n}\n",
      stdout);
  std::fflush(stdout);
  std::_Exit(0);
}

int main(int argc, char** argv) {
  const char* budget = std::getenv("PDFA_TIMEOUT");
  unsigned seconds = budget ? static_cast<unsigned>(std::atoi(budget)) : 120u;
  if (seconds) std::thread(pdfa_watchdog, seconds).detach();
  pdfa::Options opt;
  std::string input, output;
  bool haveLevel = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--version") {
      std::cout << pdfa::kEngineName << " (kura) " << pdfa::kEngineVersion << std::endl;
      return 0;
    } else if (arg == "--level" && i + 1 < argc) {
      if (!pdfa::levelFromString(argv[++i], opt.level)) return usage();
      haveLevel = true;
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
    } else if (arg == "--attach-xml" && i + 1 < argc) {
      std::ifstream xf(argv[++i], std::ios::binary);
      if (!xf) { std::cerr << "cannot open xml" << std::endl; return 1; }
      opt.attachXml.assign((std::istreambuf_iterator<char>(xf)),
                           std::istreambuf_iterator<char>());
    } else if (arg == "--attach-xml-name" && i + 1 < argc) {
      opt.attachXmlName = argv[++i];
    } else if (arg == "--facturx-profile" && i + 1 < argc) {
      opt.facturxProfile = argv[++i];
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
  if (!haveLevel || input.empty() || output.empty()) return usage();

  std::ifstream in(input, std::ios::binary);
  if (!in) {
    std::cerr << "cannot open " << input << std::endl;
    return 1;
  }
  std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  in.close();

#ifdef KURA_WITH_PDFIUM
  opt.rasterizePage = kura::makeRasterizer(data.data(), data.size(), opt.password);
#endif

  pdfa::Result res = pdfa::convert(data.data(), data.size(), opt);
  if (res.ok) {
    std::ofstream out(output, std::ios::binary);
    if (!out) {
      std::cerr << "cannot write " << output << std::endl;
      return 1;
    }
    out.write(reinterpret_cast<const char*>(res.pdf.data()),
              static_cast<std::streamsize>(res.pdf.size()));
  }
  printReport(opt, res);
  return res.ok ? 0 : 2;
}
