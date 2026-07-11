#include <dlfcn.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "pdfa/pdfa.hh"

namespace {
struct Pdfium {
  void* lib = nullptr;
  void (*initLibrary)() = nullptr;
  void (*destroyLibrary)() = nullptr;
  void* (*loadMemDocument)(const void*, int, const char*) = nullptr;
  void (*closeDocument)(void*) = nullptr;
  void* (*loadPage)(void*, int) = nullptr;
  void (*closePage)(void*) = nullptr;
  float (*pageWidth)(void*) = nullptr;
  float (*pageHeight)(void*) = nullptr;
  void* (*bitmapCreate)(int, int, int) = nullptr;
  void (*bitmapFillRect)(void*, int, int, int, int, unsigned long) = nullptr;
  void* (*bitmapGetBuffer)(void*) = nullptr;
  int (*bitmapGetStride)(void*) = nullptr;
  void (*bitmapDestroy)(void*) = nullptr;
  void (*renderPageBitmap)(void*, void*, int, int, int, int, int, int) = nullptr;

  bool load() {
    const char* candidates[] = {
        getenv("PDFA_PDFIUM_PATH"),
        "/usr/local/lib/pdfium/"
        "build-native/pdfium/out/mac-arm64/libpdfium.dylib",
        "libpdfium.dylib",
    };
    for (const char* c : candidates) {
      if (!c) continue;
      lib = dlopen(c, RTLD_LAZY | RTLD_LOCAL);
      if (lib) break;
    }
    if (!lib) return false;
    auto sym = [&](const char* n) { return dlsym(lib, n); };
    initLibrary = reinterpret_cast<void (*)()>(sym("FPDF_InitLibrary"));
    destroyLibrary = reinterpret_cast<void (*)()>(sym("FPDF_DestroyLibrary"));
    loadMemDocument = reinterpret_cast<void* (*)(const void*, int, const char*)>(
        sym("FPDF_LoadMemDocument"));
    closeDocument = reinterpret_cast<void (*)(void*)>(sym("FPDF_CloseDocument"));
    loadPage = reinterpret_cast<void* (*)(void*, int)>(sym("FPDF_LoadPage"));
    closePage = reinterpret_cast<void (*)(void*)>(sym("FPDF_ClosePage"));
    pageWidth = reinterpret_cast<float (*)(void*)>(sym("FPDF_GetPageWidthF"));
    pageHeight = reinterpret_cast<float (*)(void*)>(sym("FPDF_GetPageHeightF"));
    bitmapCreate = reinterpret_cast<void* (*)(int, int, int)>(sym("FPDFBitmap_Create"));
    bitmapFillRect = reinterpret_cast<void (*)(void*, int, int, int, int, unsigned long)>(
        sym("FPDFBitmap_FillRect"));
    bitmapGetBuffer = reinterpret_cast<void* (*)(void*)>(sym("FPDFBitmap_GetBuffer"));
    bitmapGetStride = reinterpret_cast<int (*)(void*)>(sym("FPDFBitmap_GetStride"));
    bitmapDestroy = reinterpret_cast<void (*)(void*)>(sym("FPDFBitmap_Destroy"));
    renderPageBitmap = reinterpret_cast<void (*)(void*, void*, int, int, int, int, int, int)>(
        sym("FPDF_RenderPageBitmap"));
    bool ok = initLibrary && loadMemDocument && closeDocument && loadPage && closePage &&
              pageWidth && pageHeight && bitmapCreate && bitmapFillRect && bitmapGetBuffer &&
              bitmapGetStride && bitmapDestroy && renderPageBitmap;
    if (ok) initLibrary();
    return ok;
  }

  bool render(const std::string& docBytes, const std::string& password, int pageIndex,
              double dpi, int& w, int& h, std::string& rgb) {
    void* doc = loadMemDocument(docBytes.data(), static_cast<int>(docBytes.size()),
                                password.empty() ? nullptr : password.c_str());
    if (!doc) return false;
    void* page = loadPage(doc, pageIndex);
    if (!page) {
      closeDocument(doc);
      return false;
    }
    double pw = pageWidth(page), ph = pageHeight(page);
    w = static_cast<int>(pw * dpi / 72.0 + 0.5);
    h = static_cast<int>(ph * dpi / 72.0 + 0.5);
    if (w <= 0 || h <= 0 || static_cast<long long>(w) * h > 50000000LL) {
      closePage(page);
      closeDocument(doc);
      return false;
    }
    void* bmp = bitmapCreate(w, h, 0);
    if (!bmp) {
      closePage(page);
      closeDocument(doc);
      return false;
    }
    bitmapFillRect(bmp, 0, 0, w, h, 0xFFFFFFFFul);
    renderPageBitmap(bmp, page, 0, 0, w, h, 0, 0x800);
    const unsigned char* buf = static_cast<const unsigned char*>(bitmapGetBuffer(bmp));
    int stride = bitmapGetStride(bmp);
    rgb.assign(static_cast<size_t>(w) * h * 3, '\0');
    for (int y = 0; y < h; ++y) {
      const unsigned char* row = buf + static_cast<size_t>(y) * stride;
      for (int x = 0; x < w; ++x) {
        size_t o = (static_cast<size_t>(y) * w + x) * 3;
        rgb[o] = static_cast<char>(row[x * 4 + 2]);
        rgb[o + 1] = static_cast<char>(row[x * 4 + 1]);
        rgb[o + 2] = static_cast<char>(row[x * 4]);
      }
    }
    bitmapDestroy(bmp);
    closePage(page);
    closeDocument(doc);
    return true;
  }
};

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
  json += ",\"engine\":\"" + std::string(pdfa::kEngineVersion) + "\"";
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
  std::cerr << "usage: pdfa-convert --level "
               "{1b,1a,2b,2u,2a,3b,3u,3a,4,4f,4e,x1a,x3,x4,x6,e1,vt1,vt3} [--ua] [--lang <tag>] "
               "[--output-condition <name>] [--output-condition-info <text>] "
               "[--registry <url>] [--vt-records <ranges>] [--allow-visual-risk] "
               "[--password <pw>] <input.pdf> <output.pdf>"
            << std::endl;
  return 1;
}
}

extern "C" void pdfa_watchdog(int) {
  static const char* msg =
      "{\n  \"ok\": false,\n  \"errorCode\": \"CONVERT_TIMEOUT\",\n"
      "  \"error\": \"conversion exceeded the time budget; the input may be "
      "malformed or crafted with pathological nesting\"\n}\n";
  ssize_t n = write(1, msg, std::strlen(msg));
  (void)n;
  _exit(0);
}

int main(int argc, char** argv) {
  std::signal(SIGALRM, pdfa_watchdog);
  const char* budget = std::getenv("PDFA_TIMEOUT");
  alarm(budget ? static_cast<unsigned>(std::atoi(budget)) : 120u);
  pdfa::Options opt;
  std::string input, output;
  bool haveLevel = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--version") {
      std::cout << "pdfa-convert " << pdfa::kEngineVersion << std::endl;
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

  static Pdfium pdfium;
  if (pdfium.load()) {
    std::string docBytes(reinterpret_cast<const char*>(data.data()), data.size());
    std::string pw = opt.password;
    opt.rasterizePage = [docBytes, pw](int pageIndex, double dpi, int& w, int& h,
                                       std::string& rgb) {
      return pdfium.render(docBytes, pw, pageIndex, dpi, w, h, rgb);
    };
  }

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
