#include "barcode.hh"

#include <algorithm>
#include <cctype>

#ifdef KURA_WITH_ZXING
#include <ZXing/BarcodeFormat.h>
#include <ZXing/CreateBarcode.h>
#include <ZXing/ReadBarcode.h>
#include <ZXing/WriteBarcode.h>
#endif

namespace pdfa {
namespace {
const char* kCode128[107] = {
    "212222", "222122", "222221", "121223", "121322", "131222", "122213", "122312", "132212",
    "221213", "221312", "231212", "112232", "122132", "122231", "113222", "123122", "123221",
    "223211", "221132", "221231", "213212", "223112", "312131", "311222", "321122", "321221",
    "312212", "322112", "322211", "212123", "212321", "232121", "111323", "131123", "131321",
    "112313", "132113", "132311", "211313", "231113", "231311", "112133", "112331", "132131",
    "113123", "113321", "133121", "313121", "211331", "231131", "213113", "213311", "213131",
    "311123", "311321", "331121", "312113", "312311", "332111", "314111", "221411", "431111",
    "111224", "111422", "121124", "121421", "141122", "141221", "112214", "112412", "122114",
    "122411", "142112", "142211", "241211", "221114", "413111", "241112", "134111", "111242",
    "121142", "121241", "114212", "124112", "124211", "411212", "421112", "421211", "212141",
    "214121", "412121", "111143", "111341", "131141", "114113", "114311", "411113", "411311",
    "113141", "114131", "311141", "411131", "211412", "211214", "211232", "2331112"};

BarcodeMatrix code128(const std::string& data) {
  BarcodeMatrix m;
  std::vector<int> vals;
  bool allDigits = data.size() >= 2 && data.size() % 2 == 0 &&
                   data.find_first_not_of("0123456789") == std::string::npos;
  if (allDigits) {
    vals.push_back(105);
    for (size_t i = 0; i < data.size(); i += 2) vals.push_back((data[i] - '0') * 10 + (data[i + 1] - '0'));
  } else {
    vals.push_back(104);
    for (unsigned char c : data) {
      if (c < 32 || c > 127) {
        m.error = "Code 128 set B cannot encode this text";
        return m;
      }
      vals.push_back(c - 32);
    }
  }
  int sum = vals[0];
  for (size_t i = 1; i < vals.size(); ++i) sum += vals[i] * static_cast<int>(i);
  vals.push_back(sum % 103);
  vals.push_back(106);
  bool bar = true;
  for (int v : vals) {
    for (const char* p = kCode128[v]; *p; ++p) {
      for (int k = 0; k < *p - '0'; ++k) m.dark.push_back(bar ? 1 : 0);
      bar = !bar;
    }
  }
  m.width = static_cast<int>(m.dark.size());
  m.height = 1;
  m.linear = true;
  return m;
}

BarcodeMatrix ean13(std::string d) {
  BarcodeMatrix m;
  if (d.size() == 12 && d.find_first_not_of("0123456789") == std::string::npos) {
    int sum = 0;
    for (int i = 0; i < 12; ++i) sum += (d[i] - '0') * (i % 2 ? 3 : 1);
    d += static_cast<char>('0' + (10 - sum % 10) % 10);
  }
  if (d.size() != 13 || d.find_first_not_of("0123456789") != std::string::npos) {
    m.error = "EAN-13 needs 12 or 13 digits";
    return m;
  }
  static const char* L[10] = {"0001101", "0011001", "0010011", "0111101", "0100011",
                              "0110001", "0101111", "0111011", "0110111", "0001011"};
  static const char* parity[10] = {"LLLLLL", "LLGLGG", "LLGGLG", "LLGGGL", "LGLLGG",
                                   "LGGLLG", "LGGGLL", "LGLGLG", "LGLGGL", "LGGLGL"};
  std::string bits = "101";
  const char* par = parity[d[0] - '0'];
  for (int i = 1; i <= 6; ++i) {
    std::string l = L[d[i] - '0'];
    if (par[i - 1] == 'G') {
      std::string r;
      for (char c : l) r += c == '0' ? '1' : '0';
      l = std::string(r.rbegin(), r.rend());
    }
    bits += l;
  }
  bits += "01010";
  for (int i = 7; i <= 12; ++i) {
    for (char c : std::string(L[d[i] - '0'])) bits += c == '0' ? '1' : '0';
  }
  bits += "101";
  for (char c : bits) m.dark.push_back(c == '1');
  m.width = static_cast<int>(m.dark.size());
  m.height = 1;
  m.linear = true;
  return m;
}

std::string lowerNoPunct(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c))) o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return o;
}
}

BarcodeMatrix makeBarcode(const std::string& type, const std::string& data) {
  std::string t = lowerNoPunct(type);
#ifdef KURA_WITH_ZXING
  try {
    ZXing::BarcodeFormat fmt = ZXing::BarcodeFormat::None;
    try {
      fmt = ZXing::BarcodeFormatFromString(type);
    } catch (...) {
    }
    if (fmt == ZXing::BarcodeFormat::None) {
      static const std::pair<const char*, const char*> alias[] = {
          {"qr", "QRCode"}, {"qrcode", "QRCode"}, {"datamatrix", "DataMatrix"}, {"pdf417", "PDF417"},
          {"aztec", "Aztec"}, {"code128", "Code128"}, {"code39", "Code39"}, {"ean13", "EAN-13"},
          {"ean8", "EAN-8"}, {"upca", "UPC-A"}, {"upce", "UPC-E"}, {"itf", "ITF"}, {"codabar", "Codabar"},
          {"code93", "Code93"}, {"microqr", "MicroQRCode"}};
      for (auto& a : alias) {
        if (t == a.first) fmt = ZXing::BarcodeFormatFromString(a.second);
      }
    }
    if (fmt != ZXing::BarcodeFormat::None) {
      ZXing::Barcode bc = ZXing::CreateBarcodeFromText(data, ZXing::CreatorOptions(fmt));
      if (bc.isValid()) {
        ZXing::Image img = ZXing::WriteBarcodeToImage(bc, ZXing::WriterOptions().scale(1).addQuietZones(false).addHRT(false));
        BarcodeMatrix m;
        m.linear = fmt <= ZXing::BarcodeFormat::AllLinear;
        m.width = img.width();
        m.height = m.linear ? 1 : img.height();
        for (int y = 0; y < m.height; ++y) {
          for (int x = 0; x < m.width; ++x) m.dark.push_back(*img.data(x, y) < 128 ? 1 : 0);
        }
        if (m.width > 0 && m.height > 0) return m;
      }
    }
  } catch (...) {
  }
#endif
  if (t.find("ean") != std::string::npos) return ean13(data);
  if (t.empty() || t.find("128") != std::string::npos) return code128(data);
  BarcodeMatrix m;
  m.error = "barcode type \"" + type + "\" needs a build with barcode support";
  return m;
}

bool barcodeReaderAvailable() {
#ifdef KURA_WITH_ZXING
  return true;
#else
  return false;
#endif
}

std::vector<BarcodeHit> readBarcodes(const std::string& rgb, int width, int height) {
  std::vector<BarcodeHit> out;
#ifdef KURA_WITH_ZXING
  if (width <= 0 || height <= 0 || rgb.size() < static_cast<size_t>(width) * height * 3) return out;
  try {
    ZXing::ImageView view(reinterpret_cast<const uint8_t*>(rgb.data()), width, height, ZXing::ImageFormat::RGB);
    ZXing::ReaderOptions opts;
    opts.setTryHarder(true).setTryRotate(true).setReturnErrors(true);
    for (const ZXing::Barcode& b : ZXing::ReadBarcodes(view, opts)) {
      BarcodeHit h;
      h.format = std::string(ZXing::Name(b.format()));
      h.text = b.text();
      h.valid = b.isValid();
      h.rotation = b.orientation();
      h.linear = b.format() <= ZXing::BarcodeFormat::AllLinear;
      auto pos = b.position();
      int minx = width, miny = height, maxx = 0, maxy = 0;
      for (int i = 0; i < 4; ++i) {
        minx = std::min(minx, pos[i].x);
        miny = std::min(miny, pos[i].y);
        maxx = std::max(maxx, pos[i].x);
        maxy = std::max(maxy, pos[i].y);
      }
      h.x0 = minx;
      h.y0 = miny;
      h.x1 = maxx;
      h.y1 = maxy;
      out.push_back(h);
    }
  } catch (...) {
  }
#else
  (void)rgb;
  (void)width;
  (void)height;
#endif
  return out;
}
}
