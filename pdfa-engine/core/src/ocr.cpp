#include "ocr.hh"

#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <cmath>
#include <cstdio>
#include <map>

#include "assets/fonts_data.hh"
#include "util.hh"

namespace pdfa {
namespace {
const FontAsset* ocrFont() {
  for (unsigned int i = 0; i < kFontAssetCount; ++i) {
    if (std::string(kFontAssets[i].key) == "sans:ttf") return &kFontAssets[i];
  }
  for (unsigned int i = 0; i < kFontAssetCount; ++i) {
    if (std::string(kFontAssets[i].key) == "sans") return &kFontAssets[i];
  }
  return nullptr;
}

std::string utf8ToWinAnsi(const std::string& in) {
  static const std::map<uint32_t, unsigned char> kHigh = {
      {0x20AC, 0x80}, {0x201A, 0x82}, {0x0192, 0x83}, {0x201E, 0x84}, {0x2026, 0x85},
      {0x2020, 0x86}, {0x2021, 0x87}, {0x02C6, 0x88}, {0x2030, 0x89}, {0x0160, 0x8A},
      {0x2039, 0x8B}, {0x0152, 0x8C}, {0x017D, 0x8E}, {0x2018, 0x91}, {0x2019, 0x92},
      {0x201C, 0x93}, {0x201D, 0x94}, {0x2022, 0x95}, {0x2013, 0x96}, {0x2014, 0x97},
      {0x02DC, 0x98}, {0x2122, 0x99}, {0x0161, 0x9A}, {0x203A, 0x9B}, {0x0153, 0x9C},
      {0x017E, 0x9E}, {0x0178, 0x9F}};
  if (!validUtf8(in)) return in;
  std::string out;
  size_t i = 0;
  while (i < in.size()) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    int len = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
    if (i + static_cast<size_t>(len) > in.size()) break;
    uint32_t cp = c < 0x80 ? c : (c & (0xFF >> (len + 1)));
    for (int j = 1; j < len; ++j) {
      cp = (cp << 6) | (static_cast<unsigned char>(in[i + j]) & 0x3F);
    }
    i += len;
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
      out += static_cast<char>(static_cast<unsigned char>(cp));
    } else {
      auto it = kHigh.find(cp);
      out += it != kHigh.end() ? static_cast<char>(it->second) : '?';
    }
  }
  return out;
}

std::string escapeText(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '(' || c == ')' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}
}

void passOcr(Ctx& ctx) {
  if (!ctx.opt.ocrPage || !ctx.opt.rasterizePage) return;
  const FontAsset* asset = ocrFont();
  if (!asset) return;

  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
  QPDFObjectHandle fontRef;
  int totalWords = 0, ocredPages = 0;

  for (size_t i = 0; i < pages.size(); ++i) {
    int w = 0, h = 0;
    std::string rgb;
    if (!ctx.opt.rasterizePage(static_cast<int>(i), ctx.opt.rasterDpi, w, h, rgb)) continue;
    if (w <= 0 || h <= 0) continue;
    std::vector<Options::OcrWord> words;
    if (!ctx.opt.ocrPage(static_cast<int>(i), ctx.opt.rasterDpi, w, h, rgb, words)) continue;
    if (words.empty()) continue;

    QPDFObjectHandle page = pages[i].getObjectHandle();
    QPDFObjectHandle media = pages[i].getAttribute("/MediaBox", true);
    double px0 = 0, py0 = 0, pw = 612, ph = 792;
    if (media.isArray() && media.getArrayNItems() == 4) {
      px0 = media.getArrayItem(0).getNumericValue();
      py0 = media.getArrayItem(1).getNumericValue();
      pw = media.getArrayItem(2).getNumericValue() - px0;
      ph = media.getArrayItem(3).getNumericValue() - py0;
    }
    double sx = pw / static_cast<double>(w);
    double sy = ph / static_cast<double>(h);

    if (!fontRef.isInitialized() || fontRef.isNull()) {
      QPDFObjectHandle prog = QPDFObjectHandle::newStream(
          &ctx.pdf, std::string(reinterpret_cast<const char*>(asset->data), asset->len));
      prog.getDict().replaceKey("/Length1",
                                QPDFObjectHandle::newInteger(static_cast<long long>(asset->len)));
      QPDFObjectHandle fd = QPDFObjectHandle::newDictionary();
      fd.replaceKey("/Type", QPDFObjectHandle::newName("/FontDescriptor"));
      fd.replaceKey("/FontName", QPDFObjectHandle::newName("/" + std::string(asset->psName)));
      fd.replaceKey("/Flags", QPDFObjectHandle::newInteger(32));
      fd.replaceKey("/FontBBox", QPDFObjectHandle::parse("[-1000 -300 2000 1100]"));
      fd.replaceKey("/ItalicAngle", QPDFObjectHandle::newInteger(0));
      fd.replaceKey("/Ascent", QPDFObjectHandle::newInteger(900));
      fd.replaceKey("/Descent", QPDFObjectHandle::newInteger(-200));
      fd.replaceKey("/CapHeight", QPDFObjectHandle::newInteger(700));
      fd.replaceKey("/StemV", QPDFObjectHandle::newInteger(80));
      fd.replaceKey("/FontFile2", ctx.pdf.makeIndirectObject(prog));
      QPDFObjectHandle font = QPDFObjectHandle::newDictionary();
      font.replaceKey("/Type", QPDFObjectHandle::newName("/Font"));
      font.replaceKey("/Subtype", QPDFObjectHandle::newName("/TrueType"));
      font.replaceKey("/BaseFont", QPDFObjectHandle::newName("/" + std::string(asset->psName)));
      font.replaceKey("/FirstChar", QPDFObjectHandle::newInteger(32));
      font.replaceKey("/LastChar", QPDFObjectHandle::newInteger(255));
      QPDFObjectHandle widths = QPDFObjectHandle::newArray();
      for (int cch = 32; cch <= 255; ++cch) widths.appendItem(QPDFObjectHandle::newInteger(500));
      font.replaceKey("/Widths", widths);
      font.replaceKey("/Encoding", QPDFObjectHandle::newName("/WinAnsiEncoding"));
      font.replaceKey("/FontDescriptor", ctx.pdf.makeIndirectObject(fd));
      fontRef = ctx.pdf.makeIndirectObject(font);
    }

    std::string content = "q BT 3 Tr\n";
    for (const Options::OcrWord& word : words) {
      if (word.text.empty()) continue;
      if (!std::isfinite(word.x) || !std::isfinite(word.y) || !std::isfinite(word.width) ||
          !std::isfinite(word.height)) {
        continue;
      }
      if (!(word.width > 0) || !(word.height > 0)) continue;
      std::string text = utf8ToWinAnsi(word.text);
      if (text.empty()) continue;
      double x = px0 + word.x * sx;
      double y = py0 + ph - (word.y + word.height) * sy;
      double size = word.height * sy;
      if (size < 1) size = 1;
      double target = word.width * sx;
      double natural = 0.5 * size * static_cast<double>(text.size());
      double hscale = natural > 0 ? (target / natural) * 100.0 : 100.0;
      if (hscale < 1) hscale = 1;
      if (hscale > 1000) hscale = 1000;
      content += "/KuraOCR " + fmtFixed(size, 2) + " Tf " + fmtFixed(hscale, 2) +
                 " Tz 1 0 0 1 " + fmtFixed(x, 2) + " " + fmtFixed(y, 2) + " Tm (";
      content += escapeText(text);
      content += ") Tj\n";
      ++totalWords;
    }
    content += "ET Q\n";

    QPDFObjectHandle res = pages[i].getAttribute("/Resources", true);
    if (!res.isDictionary()) {
      res = QPDFObjectHandle::newDictionary();
      page.replaceKey("/Resources", res);
    }
    QPDFObjectHandle fonts = res.getKey("/Font");
    if (!fonts.isDictionary()) {
      fonts = QPDFObjectHandle::newDictionary();
      res.replaceKey("/Font", fonts);
    }
    fonts.replaceKey("/KuraOCR", fontRef);
    pages[i].addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, content), false);
    ++ocredPages;
  }

  if (ocredPages) {
    ctx.issue("OCR_TEXT_LAYER_ADDED",
              "added an invisible OCR text layer: " + std::to_string(totalWords) +
                  " word(s) across " + std::to_string(ocredPages) + " page(s)",
              true);
  }
}
}
