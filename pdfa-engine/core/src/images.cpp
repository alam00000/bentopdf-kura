#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <map>
#include <set>
#include "images.hh"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "limits.hh"
#include "util.hh"

namespace pdfa {
namespace {
int componentsForSpace(QPDFObjectHandle cs, QPDFObjectHandle* indexedBase,
                       QPDFObjectHandle* indexedLookup, int* hival, int depth = 0);

int componentsForName(const std::string& name) {
  if (name == "/DeviceGray" || name == "/CalGray" || name == "/G") return 1;
  if (name == "/DeviceRGB" || name == "/CalRGB" || name == "/RGB") return 3;
  if (name == "/DeviceCMYK" || name == "/CMYK") return 4;
  return 0;
}

int componentsForSpace(QPDFObjectHandle cs, QPDFObjectHandle* indexedBase,
                       QPDFObjectHandle* indexedLookup, int* hival, int depth) {
  if (depth > kMaxColorSpaceNest) return 0;
  if (cs.isName()) return componentsForName(cs.getName());
  if (!cs.isArray() || cs.getArrayNItems() < 1) return 0;
  std::string family = nameOf(cs.getArrayItem(0));
  if (family == "/ICCBased" && cs.getArrayNItems() >= 2) {
    QPDFObjectHandle prof = cs.getArrayItem(1);
    if (prof.isStream() && prof.getDict().getKey("/N").isInteger()) {
      return static_cast<int>(prof.getDict().getKey("/N").getIntValue());
    }
    return 0;
  }
  if ((family == "/Indexed" || family == "/I") && cs.getArrayNItems() >= 4) {
    if (indexedBase) *indexedBase = cs.getArrayItem(1);
    if (indexedLookup) *indexedLookup = cs.getArrayItem(3);
    if (hival && cs.getArrayItem(2).isInteger()) {
      *hival = static_cast<int>(cs.getArrayItem(2).getIntValue());
    }
    return -1;
  }
  if (family == "/CalRGB" || family == "/Lab") return 3;
  if (family == "/CalGray") return 1;
  if (family == "/DeviceN" && cs.getArrayNItems() >= 2 && cs.getArrayItem(1).isArray()) {
    return cs.getArrayItem(1).getArrayNItems();
  }
  if (family == "/Separation") return 1;
  return 0;
}

std::string expandBits(const std::string& in, int width, int height, int comps, int bpc) {
  std::string out;
  out.reserve(static_cast<size_t>(width) * height * comps);
  size_t rowBytes = (static_cast<size_t>(width) * comps * bpc + 7) / 8;
  if (bpc == 8) {
    size_t rows = std::min(static_cast<size_t>(height), in.size() / std::max<size_t>(rowBytes, 1));
    return in.substr(0, rows * rowBytes);
  }
  for (int y = 0; y < height; ++y) {
    size_t rowStart = y * rowBytes;
    if (rowStart + rowBytes > in.size()) break;
    const unsigned char* row = reinterpret_cast<const unsigned char*>(in.data()) + rowStart;
    int total = width * comps;
    for (int i = 0; i < total; ++i) {
      unsigned v = 0;
      if (bpc == 8) {
        v = row[i];
      } else if (bpc == 1) {
        v = ((row[i / 8] >> (7 - i % 8)) & 1) * 255;
      } else if (bpc == 2) {
        v = ((row[i / 4] >> ((3 - i % 4) * 2)) & 3) * 85;
      } else if (bpc == 4) {
        v = ((row[i / 2] >> ((1 - i % 2) * 4)) & 15) * 17;
      } else if (bpc == 16) {
        v = row[i * 2];
      }
      out += static_cast<char>(v);
    }
  }
  return out;
}

void applyDecode(std::string& samples, QPDFObjectHandle decode, int comps) {
  if (!decode.isArray() || decode.getArrayNItems() < comps * 2) return;
  std::vector<double> lo(comps), hi(comps);
  bool identity = true;
  for (int c = 0; c < comps; ++c) {
    lo[c] = numOf(decode.getArrayItem(c * 2), 0);
    hi[c] = numOf(decode.getArrayItem(c * 2 + 1), 1);
    if (lo[c] != 0.0 || hi[c] != 1.0) identity = false;
  }
  if (identity) return;
  for (size_t i = 0; i < samples.size(); ++i) {
    int c = static_cast<int>(i % comps);
    double v = static_cast<unsigned char>(samples[i]) / 255.0;
    double mapped = lo[c] + v * (hi[c] - lo[c]);
    int out = static_cast<int>(std::lround(mapped * 255.0));
    samples[i] = static_cast<char>(std::clamp(out, 0, 255));
  }
}
}

namespace {
std::vector<std::string> filterList(QPDFObjectHandle d) {
  std::vector<std::string> names;
  QPDFObjectHandle f = d.getKey("/Filter");
  if (f.isName()) names.push_back(f.getName());
  if (f.isArray()) {
    for (int i = 0; i < f.getArrayNItems(); ++i) names.push_back(nameOf(f.getArrayItem(i)));
  }
  return names;
}

bool specialDecodeBytes(QPDFObjectHandle image, const std::vector<std::string>& filters,
                        std::string& out, std::string& err) {
  std::string raw;
  try {
    auto buf = image.getRawStreamData();
    raw.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    err = "stream unreadable";
    return false;
  }
  for (size_t i = 0; i + 1 < filters.size(); ++i) {
    if (filters[i] == "/FlateDecode" || filters[i] == "/Fl") {
      std::string inflated;
      if (!inflateData(raw, inflated)) {
        err = "flate prefix failed";
        return false;
      }
      raw = std::move(inflated);
    } else if (filters[i] == "/ASCII85Decode" || filters[i] == "/A85" || filters[i] == "/ASCIIHexDecode" ||
               filters[i] == "/AHx") {
      try {
        QPDFObjectHandle tmp = QPDFObjectHandle::newStream(&image.getQPDF(), raw);
        tmp.getDict().replaceKey("/Filter", QPDFObjectHandle::newName(filters[i]));
        auto b = tmp.getStreamData(qpdf_dl_generalized);
        raw.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
      } catch (...) {
        err = "ascii prefix failed";
        return false;
      }
    } else {
      err = "unsupported filter chain " + filters[i];
      return false;
    }
  }
  out = std::move(raw);
  return true;
}
}

RawImage decodeImage(QPDFObjectHandle image) {
  RawImage out;
  QPDFObjectHandle d = image.getDict();
  {
    std::vector<std::string> filters = filterList(d);
    std::string terminal = filters.empty() ? std::string() : filters.back();
    if (terminal == "/JPXDecode") {
      std::string bytes, err;
      if (!specialDecodeBytes(image, filters, bytes, err)) {
        out.error = err;
        return out;
      }
      std::string alpha;
      return decodeJpxData(bytes, alpha);
    }
    if (terminal == "/DCTDecode" || terminal == "/DCT") {
      std::string bytes, err;
      if (!specialDecodeBytes(image, filters, bytes, err)) {
        out.error = err;
        return out;
      }
      bool inverted = false;
      RawImage img = decodeDctData(bytes, inverted);
      if (img.ok && inverted) {
        for (size_t i = 0; i < img.samples.size(); ++i) {
          img.samples[i] = static_cast<char>(255 - static_cast<unsigned char>(img.samples[i]));
        }
      }
      if (img.ok) applyDecode(img.samples, d.getKey("/Decode"), img.comps);
      return img;
    }
  }
  int width = d.getKey("/Width").isInteger() ? static_cast<int>(d.getKey("/Width").getIntValue()) : 0;
  int height = d.getKey("/Height").isInteger() ? static_cast<int>(d.getKey("/Height").getIntValue()) : 0;
  int bpc = d.getKey("/BitsPerComponent").isInteger()
                ? static_cast<int>(d.getKey("/BitsPerComponent").getIntValue())
                : 8;
  if (width <= 0 || height <= 0 || static_cast<long long>(width) * height > kMaxImagePixels) {
    out.error = "unsupported image dimensions";
    return out;
  }
  if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) {
    out.error = "unsupported bit depth";
    return out;
  }
  QPDFObjectHandle indexedBase = QPDFObjectHandle::newNull();
  QPDFObjectHandle lookup = QPDFObjectHandle::newNull();
  int hival = 0;
  int comps = componentsForSpace(d.getKey("/ColorSpace"), &indexedBase, &lookup, &hival);
  bool isMask = d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue();
  if (!isMask && ((comps < 1 && comps != -1) || comps > 32 ||
                  static_cast<long long>(width) * height * std::max(comps, 4) > kMaxImageSamples)) {
    out.error = "unsupported component count";
    return out;
  }
  if (isMask) {
    out.error = "stencil mask";
    return out;
  }
  if (comps == 0) {
    out.error = "unsupported color space";
    return out;
  }
  std::string raw;
  try {
    auto buf = image.getStreamData(qpdf_dl_all);
    raw.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    out.error = "stream not decodable";
    return out;
  }
  if (comps == -1) {
    size_t needIdx = ((static_cast<size_t>(width) * bpc + 7) / 8) * height;
    if (raw.size() < needIdx) {
      out.error = "truncated indexed image data";
      return out;
    }
    std::string indices = expandBits(raw, width, height, 1, bpc);
    int baseComps = componentsForSpace(indexedBase, nullptr, nullptr, nullptr, 1);
    if (baseComps <= 0) {
      out.error = "unsupported indexed base space";
      return out;
    }
    std::string table;
    if (lookup.isString()) {
      table = lookup.getStringValue();
    } else if (lookup.isStream()) {
      try {
        auto buf = lookup.getStreamData(qpdf_dl_all);
        table.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
      } catch (...) {
        out.error = "indexed lookup not decodable";
        return out;
      }
    } else {
      out.error = "missing indexed lookup";
      return out;
    }
    std::string expanded;
    expanded.reserve(indices.size() * baseComps);
    for (unsigned char idx : indices) {
      size_t off = static_cast<size_t>(idx) * baseComps;
      for (int c = 0; c < baseComps; ++c) {
        expanded += off + c < table.size() ? table[off + c] : '\0';
      }
    }
    out.samples = std::move(expanded);
    out.comps = baseComps;
  } else {
    size_t expected = ((static_cast<size_t>(width) * comps * bpc + 7) / 8) * height;
    if (raw.size() < expected) {
      out.error = "truncated image data";
      return out;
    }
    out.samples = expandBits(raw, width, height, comps, bpc);
    out.comps = comps;
    applyDecode(out.samples, d.getKey("/Decode"), comps);
  }
  out.width = width;
  out.height = height;
  out.ok = true;
  return out;
}

namespace {
bool imageUnusable(Ctx& ctx, bool mustSucceed, const char* code, const std::string& detail) {
  if (mustSucceed) {
    ctx.fatal(code, detail);
    return false;
  }
  ctx.issueMerged("IMAGE_KEPT_UNDECODABLE", detail + "; the image was kept unchanged", false);
  return true;
}
}

bool transcodeJpxImage(Ctx& ctx, QPDFObjectHandle image, bool mustSucceed) {
  if (ctx.opt.verifyOnly && ctx.isA() && ctx.part == 1) {
    ctx.issueMerged("JPX_IMAGE_NOTED", "JPEG2000 image found; PDF/A-1 sets no rule on it, so it is reported for information only", false);
    return true;
  }
  QPDFObjectHandle d = image.getDict();
  QPDFObjectHandle filters = d.getKey("/Filter");
  bool jpxOnly = nameIs(filters, "/JPXDecode") ||
                 (filters.isArray() && filters.getArrayNItems() == 1 &&
                  nameIs(filters.getArrayItem(0), "/JPXDecode"));
  if (!jpxOnly) {
    return imageUnusable(ctx, mustSucceed, "JPX_UNREADABLE", "JPEG2000 image with chained filters cannot be transcoded");
  }
  std::string raw;
  try {
    auto buf = image.getRawStreamData();
    raw.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return imageUnusable(ctx, mustSucceed, "JPX_UNREADABLE", "JPEG2000 image data unreadable");
  }
  std::string alpha;
  RawImage img = decodeJpxData(raw, alpha);
  if (!img.ok) {
    return imageUnusable(ctx, mustSucceed, "JPX_UNREADABLE", "JPEG2000 image could not be transcoded (" + img.error + ")");
  }
  size_t pixelCount = static_cast<size_t>(img.width) * img.height;
  if (alpha.size() < pixelCount) alpha.clear();
  if (!alpha.empty()) {
    bool cmyk = img.comps == 4;
    size_t pixels = pixelCount;
    for (size_t p = 0; p < pixels; ++p) {
      unsigned a = static_cast<unsigned char>(alpha[p]);
      for (int c = 0; c < img.comps; ++c) {
        unsigned v = static_cast<unsigned char>(img.samples[p * img.comps + c]);
        unsigned bg = cmyk ? 0 : 255;
        img.samples[p * img.comps + c] = static_cast<char>((v * a + bg * (255 - a) + 127) / 255);
      }
    }
    ctx.issue("TRANSPARENCY_FLATTENED",
              "composited JPEG2000 alpha channel over white during transcode", true);
  }
  image.replaceStreamData(img.samples, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
  d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
  d.replaceKey("/Width", QPDFObjectHandle::newInteger(img.width));
  d.replaceKey("/Height", QPDFObjectHandle::newInteger(img.height));
  const char* cs = img.comps == 1 ? "/DeviceGray" : (img.comps == 4 ? "/DeviceCMYK" : "/DeviceRGB");
  d.replaceKey("/ColorSpace", QPDFObjectHandle::newName(cs));
  d.removeKey("/Filter");
  d.removeKey("/DecodeParms");
  d.removeKey("/Decode");
  d.removeKey("/SMaskInData");
  ctx.issue("JPX_TRANSCODED", "re-encoded JPEG2000 image as lossless Flate", true);
  return true;
}

bool jpxPrintDepthOk(const std::string& data) {
  size_t i = data.find("ihdr");
  if (i == std::string::npos || i + 15 > data.size()) return false;
  unsigned char b = static_cast<unsigned char>(data[i + 14]);
  if (b == 0xFF) {
    size_t j = data.find("bpcc");
    if (j == std::string::npos || j + 5 > data.size()) return false;
    b = static_cast<unsigned char>(data[j + 4]);
  }
  int depth = (b & 0x7F) + 1;
  return depth == 1 || depth == 8 || depth == 16;
}

void retagDctColorSpace(Ctx& ctx, QPDFObjectHandle image) {
  QPDFObjectHandle d = image.getDict();
  if (d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue()) return;
  std::vector<std::string> f = filterList(d);
  if (f.size() != 1 || (f[0] != "/DCTDecode" && f[0] != "/DCT")) return;
  int comps = componentsForSpace(d.getKey("/ColorSpace"), nullptr, nullptr, nullptr);
  if (comps <= 0) return;
  std::string raw;
  try {
    auto buf = image.getRawStreamData();
    raw.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return;
  }
  int nf = 0, jh = 0, jw = 0;
  for (size_t p = 2; p + 10 < raw.size();) {
    if (static_cast<unsigned char>(raw[p]) != 0xFF) {
      ++p;
      continue;
    }
    unsigned char m = static_cast<unsigned char>(raw[p + 1]);
    if (m == 0xFF || m == 0x01 || (m >= 0xD0 && m <= 0xD8)) {
      ++p;
      continue;
    }
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      nf = static_cast<unsigned char>(raw[p + 9]);
      jh = (static_cast<unsigned char>(raw[p + 5]) << 8) | static_cast<unsigned char>(raw[p + 6]);
      jw = (static_cast<unsigned char>(raw[p + 7]) << 8) | static_cast<unsigned char>(raw[p + 8]);
      break;
    }
    if (m == 0xD9 || m == 0xDA) break;
    size_t len = (static_cast<unsigned char>(raw[p + 2]) << 8) | static_cast<unsigned char>(raw[p + 3]);
    p += 2 + len;
  }
  bool sizeWrong = jw > 0 && jh > 0 &&
      (!d.getKey("/Width").isInteger() || d.getKey("/Width").getIntValue() != jw ||
       !d.getKey("/Height").isInteger() || d.getKey("/Height").getIntValue() != jh);
  if (sizeWrong || ctx.isX() || ctx.isE()) {
    bool inverted = false;
    RawImage img = decodeDctData(raw, inverted);
    if (img.ok && img.damaged) {
      if (inverted) {
        for (char& c : img.samples) c = static_cast<char>(255 - static_cast<unsigned char>(c));
      }
      std::string jpeg = img.comps == 4 ? encodeCmykJpeg(img.samples, img.width, img.height, 95)
                                        : encodeJpeg(img.samples, img.width, img.height, img.comps, 95);
      if (!jpeg.empty()) {
        image.replaceStreamData(jpeg, QPDFObjectHandle::newName("/DCTDecode"), QPDFObjectHandle::newNull());
        d.replaceKey("/Width", QPDFObjectHandle::newInteger(img.width));
        d.replaceKey("/Height", QPDFObjectHandle::newInteger(img.height));
        d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
        d.replaceKey("/ColorSpace", QPDFObjectHandle::newName(img.comps == 1 ? "/DeviceGray" : img.comps == 3 ? "/DeviceRGB" : "/DeviceCMYK"));
        d.removeKey("/Decode");
        ctx.issue("JPEG_DATA_REPAIRED", "re-encoded a JPEG image whose compressed data was damaged, keeping the pixels a reader recovers from it", true);
        return;
      }
    }
  }
  if (sizeWrong) {
    d.replaceKey("/Width", QPDFObjectHandle::newInteger(jw));
    d.replaceKey("/Height", QPDFObjectHandle::newInteger(jh));
    ctx.issue("IMAGE_SIZE_CORRECTED", "set the declared size of a JPEG image to the " +
                  std::to_string(jw) + " x " + std::to_string(jh) + " pixels it actually holds", true);
  }
  if (nf == comps || (nf != 1 && nf != 3 && nf != 4)) return;
  d.replaceKey("/ColorSpace", QPDFObjectHandle::newName(nf == 1 ? "/DeviceGray" : nf == 3 ? "/DeviceRGB" : "/DeviceCMYK"));
  d.removeKey("/Decode");
  ctx.issue("IMAGE_COLORSPACE_RETAGGED", "set the colour space of a JPEG image to match its " + std::to_string(nf) + " component(s)", true);
}

bool reduceImageDepth(Ctx& ctx, QPDFObjectHandle image) {
  QPDFObjectHandle d = image.getDict();
  std::string raw;
  try {
    auto buf = image.getStreamData(qpdf_dl_all);
    raw.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    ctx.fatal("IMAGE_DEPTH_UNSUPPORTED", "16-bit image data could not be decoded");
    return false;
  }
  if (raw.size() % 2) raw.pop_back();
  std::string out(raw.size() / 2, '\0');
  for (size_t i = 0; i < out.size(); ++i) out[i] = raw[i * 2];
  image.replaceStreamData(flateCompress(out), QPDFObjectHandle::newName("/FlateDecode"),
                          QPDFObjectHandle::newNull());
  d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
  QPDFObjectHandle mask = d.getKey("/Mask");
  if (mask.isArray()) {
    QPDFObjectHandle scaled = QPDFObjectHandle::newArray();
    for (int i = 0; i < mask.getArrayNItems(); ++i) {
      QPDFObjectHandle v = mask.getArrayItem(i);
      scaled.appendItem(v.isInteger() ? QPDFObjectHandle::newInteger(v.getIntValue() >> 8) : v);
    }
    d.replaceKey("/Mask", scaled);
  }
  ctx.issue("IMAGE_DEPTH_REDUCED", "reduced a 16-bit image to 8 bits per component, as PDF 1.4 allows", true);
  return true;
}

bool repackImageBits(QPDFObjectHandle image, int target) {
  QPDFObjectHandle d = image.getDict();
  int width = d.getKey("/Width").isInteger() ? static_cast<int>(d.getKey("/Width").getIntValue()) : 0;
  int height = d.getKey("/Height").isInteger() ? static_cast<int>(d.getKey("/Height").getIntValue()) : 0;
  int bpc = d.getKey("/BitsPerComponent").isInteger() ? static_cast<int>(d.getKey("/BitsPerComponent").getIntValue()) : 0;
  QPDFObjectHandle cs = d.getKey("/ColorSpace");
  bool mask = target == 1 || (d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue());
  bool indexed = cs.isArray() && cs.getArrayNItems() >= 1 && (nameIs(cs.getArrayItem(0), "/Indexed") || nameIs(cs.getArrayItem(0), "/I"));
  QPDFObjectHandle base, lookup;
  int hival = 0;
  int comps = mask || indexed ? 1 : componentsForSpace(cs, &base, &lookup, &hival);
  if (width <= 0 || height <= 0 || bpc < 1 || bpc > 16 || comps <= 0 ||
      static_cast<long long>(width) * height * comps > kMaxImagePixels) {
    return false;
  }
  std::string raw;
  try {
    auto buf = image.getStreamData(qpdf_dl_generalized);
    raw.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return false;
  }
  size_t rowBits = static_cast<size_t>(width) * comps * bpc;
  size_t rowBytes = (rowBits + 7) / 8;
  if (raw.size() < rowBytes * height) raw.resize(rowBytes * height, '\0');
  unsigned maxIn = (1u << bpc) - 1;
  size_t outRow = target == 1 ? (static_cast<size_t>(width) + 7) / 8 : static_cast<size_t>(width) * comps;
  std::string out(outRow * height, '\0');
  for (int y = 0; y < height; ++y) {
    const unsigned char* row = reinterpret_cast<const unsigned char*>(raw.data()) + y * rowBytes;
    for (int i = 0; i < width * comps; ++i) {
      unsigned v = 0;
      size_t bit = static_cast<size_t>(i) * bpc;
      for (int b = 0; b < bpc; ++b, ++bit) v = (v << 1) | ((row[bit / 8] >> (7 - bit % 8)) & 1);
      if (target == 1) {
        if (v * 2 > maxIn) out[y * outRow + i / 8] = static_cast<char>(out[y * outRow + i / 8] | (0x80 >> (i % 8)));
      } else {
        out[y * outRow + i] = static_cast<char>(indexed ? std::min(v, 255u) : (v * 255 + maxIn / 2) / maxIn);
      }
    }
  }
  image.replaceStreamData(flateCompress(out), QPDFObjectHandle::newName("/FlateDecode"), QPDFObjectHandle::newNull());
  d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(target));
  return true;
}

bool reencodeJbig2Image(Ctx& ctx, QPDFObjectHandle image, bool mustSucceed) {
  QPDFObjectHandle d = image.getDict();
  int width = d.getKey("/Width").isInteger() ? static_cast<int>(d.getKey("/Width").getIntValue()) : 0;
  int height = d.getKey("/Height").isInteger() ? static_cast<int>(d.getKey("/Height").getIntValue()) : 0;
  if (!ctx.opt.renderPdf || width <= 0 || height <= 0) {
    return imageUnusable(ctx, mustSucceed, "JBIG2_NOT_ALLOWED", "JBIG2 images are not allowed at this level and this build cannot decode them");
  }
  std::string pdf;
  try {
    QPDF tiny;
    tiny.emptyPDF();
    QPDFObjectHandle copy = tiny.copyForeignObject(image);
    QPDFObjectHandle res = QPDFObjectHandle::parse("<< /XObject << >> >>");
    res.getKey("/XObject").replaceKey("/Im0", copy);
    std::string content = "q " + std::to_string(width) + " 0 0 " + std::to_string(height) + " 0 0 cm /Im0 Do Q";
    QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page >>");
    page.replaceKey("/MediaBox", QPDFObjectHandle::parse("[0 0 " + std::to_string(width) + " " + std::to_string(height) + "]"));
    page.replaceKey("/Resources", res);
    page.replaceKey("/Contents", tiny.newStream(content));
    QPDFPageDocumentHelper(tiny).addPage(QPDFPageObjectHelper(tiny.makeIndirectObject(page)), false);
    QPDFWriter w(tiny);
    w.setOutputMemory();
    w.write();
    auto buf = w.getBufferSharedPointer();
    pdf.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return imageUnusable(ctx, mustSucceed, "JBIG2_NOT_ALLOWED", "a JBIG2 image could not be prepared for re-encoding");
  }
  int w = 0, h = 0;
  std::string rgb;
  if (!ctx.opt.renderPdf(pdf, 72.0, w, h, rgb) || w != width || h != height ||
      rgb.size() < static_cast<size_t>(w) * h * 3) {
    return imageUnusable(ctx, mustSucceed, "JBIG2_NOT_ALLOWED", "a JBIG2 image could not be decoded for re-encoding");
  }
  size_t rowBytes = (static_cast<size_t>(w) + 7) / 8;
  std::string bits(rowBytes * h, '\0');
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      size_t o = (static_cast<size_t>(y) * w + x) * 3;
      int gray = (static_cast<unsigned char>(rgb[o]) + static_cast<unsigned char>(rgb[o + 1]) +
                  static_cast<unsigned char>(rgb[o + 2])) / 3;
      if (gray >= 128) bits[y * rowBytes + x / 8] |= static_cast<char>(0x80 >> (x % 8));
    }
  }
  image.replaceStreamData(flateCompress(bits), QPDFObjectHandle::newName("/FlateDecode"),
                          QPDFObjectHandle::newNull());
  d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(1));
  d.removeKey("/Decode");
  ctx.issue("JBIG2_REENCODED", "re-encoded a JBIG2 image losslessly as Flate for print exchange", true);
  return true;
}

bool flattenImageSMask(Ctx& ctx, QPDFObjectHandle image) {
  QPDFObjectHandle d = image.getDict();
  QPDFObjectHandle mask = d.getKey("/SMask");
  if (!mask.isStream()) return true;

  {
    QPDFObjectHandle md = mask.getDict();
    int mbpc = md.getKey("/BitsPerComponent").isInteger()
                   ? static_cast<int>(md.getKey("/BitsPerComponent").getIntValue())
                   : 0;
    bool jbig2 = false;
    QPDFObjectHandle mf = md.getKey("/Filter");
    if (nameIs(mf, "/JBIG2Decode")) jbig2 = true;
    if (mf.isArray()) {
      for (int i = 0; i < mf.getArrayNItems(); ++i) {
        if (nameIs(mf.getArrayItem(i), "/JBIG2Decode")) jbig2 = true;
      }
    }
    bool sameSize =
        md.getKey("/Width").isInteger() && d.getKey("/Width").isInteger() &&
        md.getKey("/Width").getIntValue() == d.getKey("/Width").getIntValue() &&
        md.getKey("/Height").isInteger() && d.getKey("/Height").isInteger() &&
        md.getKey("/Height").getIntValue() == d.getKey("/Height").getIntValue();
    if ((mbpc == 1 || jbig2) && sameSize && !d.hasKey("/Mask")) {
      QPDFObjectHandle dec = md.getKey("/Decode");
      bool inverted = dec.isArray() && dec.getArrayNItems() == 2 &&
                      numOf(dec.getArrayItem(0), 0) == 1.0;
      if (inverted) {
        md.removeKey("/Decode");
      } else {
        QPDFObjectHandle flip = QPDFObjectHandle::newArray();
        flip.appendItem(QPDFObjectHandle::newInteger(1));
        flip.appendItem(QPDFObjectHandle::newInteger(0));
        md.replaceKey("/Decode", flip);
      }
      md.replaceKey("/ImageMask", QPDFObjectHandle::newBool(true));
      md.removeKey("/ColorSpace");
      md.removeKey("/Matte");
      md.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(1));
      d.replaceKey("/Mask", mask);
      d.removeKey("/SMask");
      ctx.issue("SMASK_TO_STENCIL",
                "converted binary soft mask to a PDF 1.4 stencil mask (lossless)", true);
      return true;
    }
  }

  RawImage m = decodeImage(mask);
  if (!m.ok || m.comps != 1) {
    ctx.fatal("TRANSPARENCY_P1",
              "image soft mask cannot be flattened (" + (m.ok ? "not grayscale" : m.error) +
                  "); target PDF/A-2 or 3");
    return false;
  }

  bool opaque = true;
  for (unsigned char v : m.samples) {
    if (v != 0xFF) {
      opaque = false;
      break;
    }
  }
  if (opaque) {
    d.removeKey("/SMask");
    ctx.issue("SMASK_OPAQUE_REMOVED", "removed fully opaque soft mask (no visual change)", true);
    return true;
  }

  RawImage img = decodeImage(image);
  if (!img.ok) {
    ctx.fatal("TRANSPARENCY_P1",
              "image with soft mask cannot be flattened (" + img.error + "); target PDF/A-2 or 3");
    return false;
  }

  {
    QPDFObjectHandle cs = d.getKey("/ColorSpace");
    QPDFObjectHandle base = QPDFObjectHandle::newNull();
    int hv = 0;
    int declared = componentsForSpace(cs, &base, nullptr, &hv);
    if (declared == -1) {
      int baseComps = componentsForSpace(base, nullptr, nullptr, nullptr, 1);
      if (base.isArray() && nameOf(base.getArrayItem(0)) == "/ICCBased" &&
          baseComps == img.comps) {
        d.replaceKey("/ColorSpace", base);
      } else {
        const char* n = img.comps == 1 ? "/DeviceGray"
                                       : (img.comps == 4 ? "/DeviceCMYK" : "/DeviceRGB");
        d.replaceKey("/ColorSpace", QPDFObjectHandle::newName(n));
      }
    } else if (declared != img.comps) {
      const char* n = img.comps == 1 ? "/DeviceGray"
                                     : (img.comps == 4 ? "/DeviceCMYK" : "/DeviceRGB");
      d.replaceKey("/ColorSpace", QPDFObjectHandle::newName(n));
    }
  }

  bool cmyk = img.comps == 4;
  size_t pixels = static_cast<size_t>(img.width) * img.height;
  for (size_t p = 0; p < pixels; ++p) {
    size_t mx = p % img.width;
    size_t my = p / img.width;
    size_t sx = mx * m.width / img.width;
    size_t sy = my * m.height / img.height;
    unsigned a = static_cast<unsigned char>(m.samples[sy * m.width + sx]);
    for (int c = 0; c < img.comps; ++c) {
      unsigned v = static_cast<unsigned char>(img.samples[p * img.comps + c]);
      unsigned bg = cmyk ? 0 : 255;
      unsigned outV = (v * a + bg * (255 - a) + 127) / 255;
      img.samples[p * img.comps + c] = static_cast<char>(outV);
    }
  }

  image.replaceStreamData(img.samples, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
  d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
  d.removeKey("/SMask");
  d.removeKey("/Decode");
  d.removeKey("/DecodeParms");
  d.removeKey("/Filter");
  ctx.issue("TRANSPARENCY_FLATTENED",
            "composited image soft mask over white background (visual difference possible if "
            "content lies underneath)",
            true);
  return true;
}

namespace {
struct PlacementScanner : public QPDFObjectHandle::ParserCallbacks {
  struct Mat { double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; };

  void handleObject(QPDFObjectHandle obj, size_t, size_t) override {
    if (obj.isOperator()) {
      std::string op = obj.getOperatorValue();
      if (op == "q") {
        stack.push_back(cur);
      } else if (op == "Q") {
        if (!stack.empty()) {
          cur = stack.back();
          stack.pop_back();
        }
      } else if (op == "cm" && nums.size() >= 6) {
        size_t n = nums.size();
        Mat m;
        m.a = nums[n - 6]; m.b = nums[n - 5]; m.c = nums[n - 4];
        m.d = nums[n - 3]; m.e = nums[n - 2]; m.f = nums[n - 1];
        cur = multiply(m, cur);
      } else if (op == "Do" && !lastName.empty()) {
        draws.push_back({lastName, cur});
      }
      nums.clear();
      lastName.clear();
      return;
    }
    if (obj.isName()) {
      lastName = obj.getName();
      return;
    }
    if (obj.isNumber()) nums.push_back(obj.getNumericValue());
  }

  void handleEOF() override {}

  static Mat multiply(const Mat& m, const Mat& n) {
    Mat r;
    r.a = m.a * n.a + m.b * n.c;
    r.b = m.a * n.b + m.b * n.d;
    r.c = m.c * n.a + m.d * n.c;
    r.d = m.c * n.b + m.d * n.d;
    r.e = m.e * n.a + m.f * n.c + n.e;
    r.f = m.e * n.b + m.f * n.d + n.f;
    return r;
  }

  std::vector<std::pair<std::string, Mat>> draws;

 private:
 public:
  Mat cur;

 private:
  std::vector<Mat> stack;
  std::vector<double> nums;
  std::string lastName;
};

bool boxDownsample(const RawImage& in, int outW, int outH, std::string& out) {
  if (outW <= 0 || outH <= 0 || in.comps <= 0) return false;
  if (in.samples.size() < static_cast<size_t>(in.width) * in.height * in.comps) return false;
  out.assign(static_cast<size_t>(outW) * outH * in.comps, '\0');
  const unsigned char* src = reinterpret_cast<const unsigned char*>(in.samples.data());
  for (int y = 0; y < outH; ++y) {
    int y0 = static_cast<int>(static_cast<long long>(y) * in.height / outH);
    int y1 = static_cast<int>(static_cast<long long>(y + 1) * in.height / outH);
    if (y1 <= y0) y1 = y0 + 1;
    if (y1 > in.height) y1 = in.height;
    for (int x = 0; x < outW; ++x) {
      int x0 = static_cast<int>(static_cast<long long>(x) * in.width / outW);
      int x1 = static_cast<int>(static_cast<long long>(x + 1) * in.width / outW);
      if (x1 <= x0) x1 = x0 + 1;
      if (x1 > in.width) x1 = in.width;
      for (int c = 0; c < in.comps; ++c) {
        unsigned long long sum = 0;
        unsigned long long n = 0;
        for (int yy = y0; yy < y1; ++yy) {
          const unsigned char* row = src + (static_cast<size_t>(yy) * in.width) * in.comps;
          for (int xx = x0; xx < x1; ++xx) {
            sum += row[static_cast<size_t>(xx) * in.comps + c];
            ++n;
          }
        }
        out[(static_cast<size_t>(y) * outW + x) * in.comps + c] =
            static_cast<char>(n ? static_cast<unsigned char>(sum / n) : 0);
      }
    }
  }
  return true;
}
}

struct ImageUse {
  QPDFObjectHandle image;
  double wpt = 0;
  double hpt = 0;
};

void collectImageUses(QPDFObjectHandle stream, QPDFObjectHandle res,
                      PlacementScanner::Mat ctm, int depth, Visited& seen,
                      std::map<std::string, ImageUse>& uses) {
  if (depth > kMaxContentNest || !res.isDictionary()) return;
  PlacementScanner scan;
  scan.cur = ctm;
  try {
    QPDFObjectHandle::parseContentStream(stream, &scan);
  } catch (...) {
    return;
  }
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (!xod.isDictionary()) return;

  for (const auto& d : scan.draws) {
    QPDFObjectHandle xo = xod.getKey(d.first);
    if (!xo.isStream()) continue;
    QPDFObjectHandle dict = xo.getDict();
    std::string sub = nameOf(dict.getKey("/Subtype"));
    if (sub == "/Image") {
      if (dict.getKey("/ImageMask").isBool() && dict.getKey("/ImageMask").getBoolValue()) continue;
      if (!xo.isIndirect()) continue;
      std::string key = std::to_string(xo.getObjGen().getObj()) + "_" +
                        std::to_string(xo.getObjGen().getGen());
      double wpt = std::sqrt(d.second.a * d.second.a + d.second.b * d.second.b);
      double hpt = std::sqrt(d.second.c * d.second.c + d.second.d * d.second.d);
      if (!std::isfinite(wpt) || !std::isfinite(hpt)) continue;
      ImageUse& u = uses[key];
      u.image = xo;
      if (wpt > u.wpt) u.wpt = wpt;
      if (hpt > u.hpt) u.hpt = hpt;
    } else if (sub == "/Form") {
      if (!seen.enter(xo)) continue;
      PlacementScanner::Mat inner = d.second;
      QPDFObjectHandle mtx = dict.getKey("/Matrix");
      if (mtx.isArray() && mtx.getArrayNItems() == 6) {
        PlacementScanner::Mat m;
        m.a = numOf(mtx.getArrayItem(0), 1); m.b = numOf(mtx.getArrayItem(1), 0);
        m.c = numOf(mtx.getArrayItem(2), 0); m.d = numOf(mtx.getArrayItem(3), 1);
        m.e = numOf(mtx.getArrayItem(4), 0); m.f = numOf(mtx.getArrayItem(5), 0);
        inner = PlacementScanner::multiply(m, inner);
      }
      QPDFObjectHandle sres = dict.getKey("/Resources");
      collectImageUses(xo, sres.isDictionary() ? sres : res, inner, depth + 1, seen, uses);
    }
  }
}

void passImageResolution(Ctx& ctx) {
  if (ctx.opt.imageMaxPpi <= 0 && ctx.opt.imageUpsampleBelowPpi <= 0) return;
  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
  std::map<std::string, ImageUse> uses;

  int kuraPageNo = 0;
  for (auto& ph : pages) {
    PageScope kuraScope(ctx, ++kuraPageNo);
    QPDFObjectHandle res = ph.getAttribute("/Resources", true);
    if (!res.isDictionary()) continue;
    Visited seen;
    collectImageUses(ph.getObjectHandle().getKey("/Contents"), res,
                     PlacementScanner::Mat(), 0, seen, uses);
  }

  int shrunk = 0, upsampled = 0;
  [[maybe_unused]] long long saved = 0;
  for (auto& kv : uses) {
    ImageUse& u = kv.second;
    if (!std::isfinite(u.wpt) || !std::isfinite(u.hpt)) continue;
    if (u.wpt <= 0.01 || u.hpt <= 0.01) continue;
    QPDFObjectHandle img = u.image;
    QPDFObjectHandle d = img.getDict();
    int pw = d.getKey("/Width").isInteger() ? static_cast<int>(d.getKey("/Width").getIntValue()) : 0;
    int phh = d.getKey("/Height").isInteger() ? static_cast<int>(d.getKey("/Height").getIntValue()) : 0;
    if (pw <= 0 || phh <= 0) continue;

    double ppiX = pw * 72.0 / u.wpt;
    double ppiY = phh * 72.0 / u.hpt;
    double ppi = ppiX > ppiY ? ppiX : ppiY;
    if (ctx.opt.imageUpsampleBelowPpi > 0 && ppi < ctx.opt.imageUpsampleBelowPpi) {
      double target = ctx.opt.imageUpsampleToPpi > ppi ? ctx.opt.imageUpsampleToPpi : ctx.opt.imageUpsampleBelowPpi;
      double up = target / ppi;
      int nw = static_cast<int>(pw * up + 0.5), nh = static_cast<int>(phh * up + 0.5);
      if (nw <= pw || nh <= phh || static_cast<long long>(nw) * nh > 60000000LL) continue;
      RawImage src = decodeImage(img);
      if (!src.ok || src.width != pw || src.height != phh) continue;
      if (src.comps != 1 && src.comps != 3 && src.comps != 4) continue;
      std::string big(static_cast<size_t>(nw) * nh * src.comps, '\0');
      const unsigned char* sp = reinterpret_cast<const unsigned char*>(src.samples.data());
      for (int y = 0; y < nh; ++y) {
        double fy = std::max(0.0, (y + 0.5) * phh / nh - 0.5);
        int y0 = std::min(static_cast<int>(fy), phh - 1), y1 = std::min(y0 + 1, phh - 1);
        double wy = fy - y0;
        for (int x = 0; x < nw; ++x) {
          double fx = std::max(0.0, (x + 0.5) * pw / nw - 0.5);
          int x0 = std::min(static_cast<int>(fx), pw - 1), x1 = std::min(x0 + 1, pw - 1);
          double wx = fx - x0;
          for (int c = 0; c < src.comps; ++c) {
            auto at = [&](int xx, int yy) { return sp[(static_cast<size_t>(yy) * pw + xx) * src.comps + c]; };
            double v = (1 - wy) * ((1 - wx) * at(x0, y0) + wx * at(x1, y0)) + wy * ((1 - wx) * at(x0, y1) + wx * at(x1, y1));
            big[(static_cast<size_t>(y) * nw + x) * src.comps + c] = static_cast<char>(std::lround(v));
          }
        }
      }
      std::string lf = nameOf(d.getKey("/Filter"));
      bool lossy = lf == "/DCTDecode" || lf == "/JPXDecode";
      std::string packed = lossy && src.comps != 4 ? encodeJpeg(big, nw, nh, src.comps, 90) : std::string();
      std::string filter = "/DCTDecode";
      if (packed.empty()) {
        packed = flateCompress(big);
        filter = "/FlateDecode";
      }
      if (packed.empty()) continue;
      QPDFObjectHandle cs = d.getKey("/ColorSpace");
      bool indexed = cs.isArray() && cs.getArrayNItems() > 0 && (nameIs(cs.getArrayItem(0), "/Indexed") || nameIs(cs.getArrayItem(0), "/I"));
      img.replaceStreamData(packed, QPDFObjectHandle::newName(filter), QPDFObjectHandle::newNull());
      d.replaceKey("/Width", QPDFObjectHandle::newInteger(nw));
      d.replaceKey("/Height", QPDFObjectHandle::newInteger(nh));
      d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
      d.removeKey("/Decode");
      d.removeKey("/DecodeParms");
      d.removeKey("/SMaskInData");
      if (indexed || cs.isNull()) {
        d.replaceKey("/ColorSpace", QPDFObjectHandle::newName(src.comps == 1 ? "/DeviceGray" : src.comps == 3 ? "/DeviceRGB" : "/DeviceCMYK"));
      }
      ++upsampled;
      continue;
    }
    double above = ctx.opt.imageDownsampleAbovePpi > 0 ? ctx.opt.imageDownsampleAbovePpi : ctx.opt.imageMaxPpi * 1.05;
    if (ctx.opt.imageMaxPpi <= 0 || ppi <= above) continue;
    const std::string& kind = ctx.opt.imageDownsampleKind;
    int bpcIn = d.getKey("/BitsPerComponent").isInteger() ? static_cast<int>(d.getKey("/BitsPerComponent").getIntValue()) : 8;
    bool bitmap = bpcIn == 1 || (d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue());
    if (!kind.empty() && bitmap != (kind == "bitmap")) continue;

    double scale = ctx.opt.imageMaxPpi / ppi;
    int nw = static_cast<int>(pw * scale + 0.5);
    int nh = static_cast<int>(phh * scale + 0.5);
    if (nw < 8 || nh < 8) continue;
    if (nw >= pw && nh >= phh) continue;

    RawImage src = decodeImage(img);
    if (!src.ok || src.width != pw || src.height != phh) continue;
    if (src.comps != 1 && src.comps != 3 && src.comps != 4) continue;
    if ((kind == "colour" && src.comps == 1) || ((kind == "grey" || kind == "bitmap") && src.comps != 1)) continue;

    std::string small;
    if (!boxDownsample(src, nw, nh, small)) continue;
    if (kind == "bitmap") {
      std::string bits(static_cast<size_t>((nw + 7) / 8) * nh, '\0');
      for (int y = 0; y < nh; ++y) {
        for (int x = 0; x < nw; ++x) {
          if (static_cast<unsigned char>(small[static_cast<size_t>(y) * nw + x]) >= 128) {
            bits[static_cast<size_t>(y) * ((nw + 7) / 8) + x / 8] |= static_cast<char>(0x80 >> (x % 8));
          }
        }
      }
      std::string packed = flateCompress(bits);
      if (packed.empty()) continue;
      img.replaceStreamData(packed, QPDFObjectHandle::newName("/FlateDecode"), QPDFObjectHandle::newNull());
      d.replaceKey("/Width", QPDFObjectHandle::newInteger(nw));
      d.replaceKey("/Height", QPDFObjectHandle::newInteger(nh));
      d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(1));
      d.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceGray"));
      d.removeKey("/Decode");
      d.removeKey("/DecodeParms");
      ++shrunk;
      continue;
    }

    size_t before = 0;
    try {
      before = img.getRawStreamData()->getSize();
    } catch (...) {
      before = 0;
    }

    std::string packed = flateCompress(small);
    std::string filter = "/FlateDecode";
    if (src.comps == 1 || src.comps == 3) {
      std::string jpg = encodeJpeg(small, nw, nh, src.comps, 85);
      if (!jpg.empty() && (packed.empty() || jpg.size() < packed.size())) {
        packed = jpg;
        filter = "/DCTDecode";
      }
    }
    if (packed.empty() || (before && packed.size() >= before)) continue;

    img.replaceStreamData(packed, QPDFObjectHandle::newName(filter),
                          QPDFObjectHandle::newNull());
    d.replaceKey("/Width", QPDFObjectHandle::newInteger(nw));
    d.replaceKey("/Height", QPDFObjectHandle::newInteger(nh));
    d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    d.removeKey("/Decode");
    d.removeKey("/DecodeParms");
    d.removeKey("/Interpolate");
    if (src.comps == 1) d.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceGray"));
    else if (src.comps == 3) d.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
    else d.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceCMYK"));
    if (before > packed.size()) saved += static_cast<long long>(before - packed.size());
    ++shrunk;
  }

  if (upsampled) {
    ctx.issue("IMAGE_UPSAMPLED",
              "upsampled " + std::to_string(upsampled) + " image(s) to at least " +
                  std::to_string(static_cast<int>(ctx.opt.imageUpsampleToPpi > 0 ? ctx.opt.imageUpsampleToPpi : ctx.opt.imageUpsampleBelowPpi)) +
                  " ppi at their placed size",
              true);
  }
  if (shrunk) {
    ctx.issue("IMAGE_DOWNSAMPLED",
              "downsampled " + std::to_string(shrunk) + " image(s) to at most " +
                  std::to_string(static_cast<int>(ctx.opt.imageMaxPpi)) +
                  " ppi at their placed size",
              true);
  }
}

void externalizeInlineImagesOnce(std::vector<QPDFPageObjectHelper>& pages, size_t minSize) {
  auto hasInline = [](QPDFObjectHandle s) {
    try {
      auto b = s.getStreamData(qpdf_dl_generalized);
      std::string d(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
      return d.find("BI") != std::string::npos && d.find("ID") != std::string::npos;
    } catch (...) {
      return true;
    }
  };
  auto fits = [](QPDFObjectHandle owner, QPDFObjectHandle content) {
    size_t n = 0;
    try {
      auto b = content.getStreamData(qpdf_dl_generalized);
      std::string d(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
      for (size_t i = d.find("BI"); i != std::string::npos; i = d.find("BI", i + 2)) {
        bool before = i == 0 || std::isspace(static_cast<unsigned char>(d[i - 1]));
        bool after = i + 2 < d.size() && (std::isspace(static_cast<unsigned char>(d[i + 2])) || d[i + 2] == '/');
        if (before && after) ++n;
      }
    } catch (...) {
    }
    QPDFObjectHandle res = owner.isStream() ? owner.getDict().getKey("/Resources") : owner.getKey("/Resources");
    QPDFObjectHandle xo = res.isDictionary() ? res.getKey("/XObject") : QPDFObjectHandle::newNull();
    return n + (xo.isDictionary() ? xo.getKeys().size() : 0) <= 4095;
  };
  std::set<QPDFObjGen> seen;
  std::vector<QPDFObjectHandle> stack;
  for (auto& ph : pages) {
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle all = QPDFObjectHandle::newStream(&page.getQPDF());
    try {
      std::string joined;
      for (auto& c : ph.getPageContents()) {
        auto b = c.getStreamData(qpdf_dl_generalized);
        joined.append(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
        joined.push_back('\n');
      }
      all.replaceStreamData(joined, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
      if (fits(page, all)) ph.externalizeInlineImages(minSize, true);
    } catch (...) {
    }
    stack.push_back(ph.getAttribute("/Resources", false));
  }
  while (!stack.empty()) {
    QPDFObjectHandle res = stack.back();
    stack.pop_back();
    std::vector<QPDFObjectHandle> holders;
    for (const char* key : {"/XObject", "/Pattern"}) {
      QPDFObjectHandle dict = res.isDictionary() ? res.getKey(key) : QPDFObjectHandle::newNull();
      for (const std::string& k : dict.isDictionary() ? dict.getKeys() : std::set<std::string>()) holders.push_back(dict.getKey(k));
    }
    for (QPDFObjectHandle f : holders) {
      if (!f.isStream() || !(nameIs(f.getDict().getKey("/Subtype"), "/Form") || f.getDict().getKey("/PatternType").isInteger())) continue;
      if (f.isIndirect() && !seen.insert(f.getObjGen()).second) continue;
      if (hasInline(f) && fits(f, f)) {
        bool pattern = !f.getDict().hasKey("/Subtype");
        if (pattern) f.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
        try {
          QPDFPageObjectHelper(f).externalizeInlineImages(minSize, true);
        } catch (...) {
        }
        if (pattern) f.getDict().removeKey("/Subtype");
      }
      stack.push_back(f.getDict().getKey("/Resources"));
    }
    QPDFObjectHandle fonts = res.isDictionary() ? res.getKey("/Font") : QPDFObjectHandle::newNull();
    for (const std::string& k : fonts.isDictionary() ? fonts.getKeys() : std::set<std::string>()) {
      QPDFObjectHandle fnt = fonts.getKey(k);
      if (!fnt.isDictionary() || !nameIs(fnt.getKey("/Subtype"), "/Type3")) continue;
      if (fnt.isIndirect() && !seen.insert(fnt.getObjGen()).second) continue;
      QPDFObjectHandle procs = fnt.getKey("/CharProcs");
      for (const std::string& g : procs.isDictionary() ? procs.getKeys() : std::set<std::string>()) {
        QPDFObjectHandle proc = procs.getKey(g);
        if (!proc.isStream() || !hasInline(proc)) continue;
        QPDFObjectHandle fres = fnt.getKey("/Resources");
        if (!fres.isDictionary()) fres = res.isDictionary() ? res.shallowCopy() : QPDFObjectHandle::newDictionary();
        if (!fres.isIndirect()) fres = fnt.getQPDF().makeIndirectObject(fres);
        fnt.replaceKey("/Resources", fres);
        QPDFObjectHandle pd = proc.getDict();
        QPDFObjectHandle ownRes = pd.getKey("/Resources");
        pd.replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
        pd.replaceKey("/Resources", fres);
        try {
          if (fits(proc, proc)) QPDFPageObjectHelper(proc).externalizeInlineImages(minSize, true);
        } catch (...) {
        }
        pd.removeKey("/Subtype");
        if (pd.getKey("/Resources").isDictionary()) fnt.replaceKey("/Resources", pd.getKey("/Resources"));
        if (ownRes.isNull()) pd.removeKey("/Resources");
        else pd.replaceKey("/Resources", ownRes);
      }
      stack.push_back(fnt.getKey("/Resources"));
    }
  }
}

namespace {
void markDeep(GraphicsUse& u, QPDFObjectHandle o, int depth) {
  if (depth > kMaxColorSpaceNest || !o.isInitialized() || o.isNull() || u.has(o)) return;
  if (!o.isArray() && !o.isDictionary() && !o.isStream()) return;
  u.add(o);
  if (o.isArray() && o.getArrayNItems() >= 2 && nameIs(o.getArrayItem(0), "/ICCBased") && o.getArrayItem(1).isStream() &&
      !u.has(o.getArrayItem(1))) {
    u.iccProfiles.push_back(o.getArrayItem(1));
  }
  if (o.isArray()) {
    for (int i = 0; i < o.getArrayNItems(); ++i) markDeep(u, o.getArrayItem(i), depth + 1);
  } else {
    QPDFObjectHandle d = o.isStream() ? o.getDict() : o;
    for (const std::string& k : d.getKeys()) {
      if (k != "/Length" && k != "/Metadata") markDeep(u, d.getKey(k), depth + 1);
    }
  }
}

thread_local bool* tPaintTransparent = nullptr;
thread_local bool tInGlyph = false;

struct AlphaState {
  bool alpha = false, blend = false, mask = false;
  bool any() const { return alpha || blend || mask; }
};

class UseWalker : public QPDFObjectHandle::ParserCallbacks {
 public:
  UseWalker(GraphicsUse& u, QPDFObjectHandle res, QPDFObjectHandle pageRes, int depth, std::set<QPDFObjGen>& active,
            AlphaState st)
      : u(u), res(res), pageRes(pageRes), depth(depth), active(active), st(st) {}

  static void walk(GraphicsUse& u, QPDFObjectHandle holder, QPDFObjectHandle res, QPDFObjectHandle pageRes, int depth,
                   std::set<QPDFObjGen>& active, AlphaState st = AlphaState()) {
    if (depth > kMaxContentNest) return;
    if (holder.isIndirect() && !active.insert(holder.getObjGen()).second) return;
    if (!res.isDictionary()) res = QPDFObjectHandle::newDictionary();
    if (res.getKey("/ColorSpace").isDictionary()) {
      QPDFObjectHandle csd = res.getKey("/ColorSpace");
      for (const char* k : {"/DefaultRGB", "/DefaultCMYK", "/DefaultGray"}) markDeep(u, csd.getKey(k), 0);
    }
    UseWalker w(u, res, pageRes, depth, active, st);
    try {
      if (holder.isStream()) holder.parseAsContents(&w);
      else QPDFPageObjectHelper(holder).parseContents(&w);
    } catch (...) {
    }
    if (holder.isIndirect()) active.erase(holder.getObjGen());
  }

  static void walkForm(GraphicsUse& u, QPDFObjectHandle form, QPDFObjectHandle pageRes, int depth,
                       std::set<QPDFObjGen>& active, AlphaState st = AlphaState()) {
    if (!form.isStream()) return;
    u.add(form);
    QPDFObjectHandle d = form.getDict();
    QPDFObjectHandle grp = d.getKey("/Group");
    if (grp.isDictionary()) {
      u.add(grp);
      markDeep(u, grp.getKey("/CS"), 0);
    }
    walk(u, form, d.getKey("/Resources"), pageRes, depth + 1, active, st);
  }

  void handleObject(QPDFObjectHandle obj) override {
    if (obj.isInlineImage()) {
      QPDFObjectHandle dict = QPDFObjectHandle::newDictionary();
      for (size_t i = 0; i + 1 < ops.size(); i += 2) {
        if (ops[i].isName()) dict.replaceKey(ops[i].getName(), ops[i + 1]);
      }
      for (const char* k : {"/CS", "/ColorSpace"}) {
        QPDFObjectHandle cs = dict.getKey(k);
        if (cs.isName()) markDeep(u, lookup("/ColorSpace", cs.getName()), 0);
        else markDeep(u, cs, 0);
      }
      u.inlineImages.push_back(dict);
      painted(false);
      ops.clear();
      return;
    }
    if (!obj.isOperator()) {
      ops.push_back(obj);
      return;
    }
    std::string op = obj.getOperatorValue();
    if (op == "ID") return;
    QPDFObjectHandle last = ops.empty() ? QPDFObjectHandle::newNull() : ops.back();
    static const std::set<std::string> kPaint = {"f", "F", "f*", "B", "B*", "b", "b*", "S", "s", "Tj", "TJ", "'", "\"", "sh"};
    if (kPaint.count(op)) painted(false);
    if (op == "q") {
      fonts.push_back(font);
      states.push_back(st);
    } else if (op == "Q" && !fonts.empty()) {
      font = fonts.back();
      fonts.pop_back();
      st = states.back();
      states.pop_back();
    } else if (op == "Tf" && ops.size() >= 2 && ops[ops.size() - 2].isName()) {
      font = lookup("/Font", ops[ops.size() - 2].getName());
    } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
      useFont();
    } else if (op == "gs" && last.isName()) {
      QPDFObjectHandle gs = lookup("/ExtGState", last.getName());
      if (gs.isDictionary()) {
        u.add(gs);
        if (gs.hasKey("/CA") || gs.hasKey("/ca")) {
          st.alpha = numOf(gs.getKey("/CA"), 1.0) < 1.0 - 1e-6 || numOf(gs.getKey("/ca"), 1.0) < 1.0 - 1e-6;
        }
        if (gs.hasKey("/BM")) {
          QPDFObjectHandle bm = gs.getKey("/BM");
          std::string n = bm.isArray() && bm.getArrayNItems() > 0 ? nameOf(bm.getArrayItem(0)) : nameOf(bm);
          st.blend = !n.empty() && n != "/Normal" && n != "/Compatible";
        }
        if (gs.hasKey("/SMask")) st.mask = gs.getKey("/SMask").isDictionary();
      }
    } else if ((op == "cs" || op == "CS") && last.isName()) {
      markDeep(u, lookup("/ColorSpace", last.getName()), 0);
    } else if (op == "sh" && last.isName()) {
      QPDFObjectHandle sh = lookup("/Shading", last.getName());
      u.add(sh);
      markDeep(u, (sh.isStream() ? sh.getDict() : sh).isDictionary() ? (sh.isStream() ? sh.getDict() : sh).getKey("/ColorSpace") : QPDFObjectHandle::newNull(), 0);
    } else if ((op == "scn" || op == "SCN") && last.isName()) {
      QPDFObjectHandle pat = lookup("/Pattern", last.getName());
      if ((pat.isStream() || pat.isDictionary()) && !u.has(pat)) {
        u.add(pat);
        QPDFObjectHandle pd = pat.isStream() ? pat.getDict() : pat;
        QPDFObjectHandle sh = pd.getKey("/Shading");
        if (sh.isStream() || sh.isDictionary()) {
          u.add(sh);
          QPDFObjectHandle shd = sh.isStream() ? sh.getDict() : sh;
          markDeep(u, shd.getKey("/ColorSpace"), 0);
        }
        if (pat.isStream() && !pd.hasKey("/Shading")) walk(u, pat, pd.getKey("/Resources"), pageRes, depth + 1, active);
      }
    } else if (op == "Do" && last.isName()) {
      QPDFObjectHandle xo = lookup("/XObject", last.getName());
      if (xo.isStream() && !u.has(xo)) {
        QPDFObjectHandle d = xo.getDict();
        std::string sub = nameOf(d.getKey("/Subtype"));
        if (sub == "/Form") {
          QPDFObjectHandle grp = d.getKey("/Group");
          painted(grp.isDictionary() && nameIs(grp.getKey("/S"), "/Transparency"));
          walkForm(u, xo, pageRes, depth, active, st);
        } else {
          u.add(xo);
          if (sub == "/Image") useImage(xo, 0);
        }
      } else if (xo.isStream() && nameIs(xo.getDict().getKey("/Subtype"), "/Form")) {
        QPDFObjectHandle grp = xo.getDict().getKey("/Group");
        painted(grp.isDictionary() && nameIs(grp.getKey("/S"), "/Transparency"));
      }
      if (xo.isStream() && nameIs(xo.getDict().getKey("/Subtype"), "/Image")) {
        QPDFObjectHandle d = xo.getDict();
        QPDFObjectHandle f = d.getKey("/Filter");
        bool jpx = nameIs(f, "/JPXDecode") || (f.isArray() && f.getArrayNItems() > 0 && nameIs(f.getArrayItem(f.getArrayNItems() - 1), "/JPXDecode"));
        painted(d.getKey("/SMask").isStream() || (jpx && numOf(d.getKey("/SMaskInData"), 0) > 0));
      }
    }
    ops.clear();
  }

  void handleEOF() override {}

 private:
  QPDFObjectHandle lookup(const char* cat, const std::string& name) {
    for (QPDFObjectHandle r : {res, pageRes}) {
      QPDFObjectHandle d = r.isDictionary() ? r.getKey(cat) : QPDFObjectHandle::newNull();
      if (d.isDictionary() && d.hasKey(name) && !d.getKey(name).isNull()) return d.getKey(name);
    }
    static const std::set<std::string> kBuiltin = {"/DeviceRGB", "/DeviceCMYK", "/DeviceGray", "/Pattern",
                                                   "/RGB", "/CMYK", "/G", "/I", "/Indexed"};
    if (tInGlyph && !(std::string(cat) == "/ColorSpace" && kBuiltin.count(name))) u.glyphUndefined = true;
    return QPDFObjectHandle::newNull();
  }

  void painted(bool self) {
    if (tPaintTransparent && (self || st.any())) *tPaintTransparent = true;
  }

  void useImage(QPDFObjectHandle img, int level) {
    if (level > 2) return;
    QPDFObjectHandle d = img.getDict();
    markDeep(u, d.getKey("/ColorSpace"), 0);
    QPDFObjectHandle sm = d.getKey("/SMask");
    if (sm.isStream() && !u.has(sm)) {
      u.add(sm);
      useImage(sm, level + 1);
    }
    QPDFObjectHandle mk = d.getKey("/Mask");
    if (mk.isStream() && !u.has(mk)) {
      u.add(mk);
      u.maskImages.push_back(mk);
    }
  }

  void useFont() {
    if (!font.isDictionary() || u.has(font)) return;
    u.add(font);
    if (!nameIs(font.getKey("/Subtype"), "/Type3")) return;
    QPDFObjectHandle procs = font.getKey("/CharProcs");
    QPDFObjectHandle fres = font.getKey("/Resources");
    for (const std::string& g : procs.isDictionary() ? procs.getKeys() : std::set<std::string>()) {
      QPDFObjectHandle proc = procs.getKey(g);
      if (!proc.isStream()) continue;
      u.add(proc);
      QPDFObjectHandle own = proc.getDict().getKey("/Resources");
      bool saved = tInGlyph;
      tInGlyph = true;
      walk(u, proc, own.isDictionary() ? own : fres, pageRes, depth + 1, active);
      tInGlyph = saved;
    }
  }

  GraphicsUse& u;
  QPDFObjectHandle res;
  QPDFObjectHandle pageRes;
  int depth;
  std::set<QPDFObjGen>& active;
  QPDFObjectHandle font;
  std::vector<QPDFObjectHandle> fonts;
  std::vector<QPDFObjectHandle> ops;
  AlphaState st;
  std::vector<AlphaState> states;
};
}

GraphicsUse collectGraphicsUse(QPDF& pdf) {
  GraphicsUse u;
  std::set<QPDFObjGen> active;
  for (auto& ph : QPDFPageDocumentHelper(pdf).getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle res = ph.getAttribute("/Resources", false);
    bool transparent = false;
    tPaintTransparent = &transparent;
    QPDFObjectHandle grp = page.getKey("/Group");
    if (grp.isDictionary()) {
      u.add(grp);
      markDeep(u, grp.getKey("/CS"), 0);
    }
    UseWalker::walk(u, page, res, res, 0, active);
    QPDFObjectHandle annots = page.getKey("/Annots");
    for (int i = 0; annots.isArray() && i < annots.getArrayNItems(); ++i) {
      QPDFObjectHandle ap = annots.getArrayItem(i).isDictionary() ? annots.getArrayItem(i).getKey("/AP") : QPDFObjectHandle::newNull();
      QPDFObjectHandle abm = annots.getArrayItem(i).isDictionary() ? annots.getArrayItem(i).getKey("/BM") : QPDFObjectHandle::newNull();
      if (abm.isName() && abm.getName() != "/Normal") transparent = true;
      for (const char* k : {"/N", "/D", "/R"}) {
        QPDFObjectHandle e = ap.isDictionary() ? ap.getKey(k) : QPDFObjectHandle::newNull();
        std::vector<QPDFObjectHandle> streams;
        if (e.isStream()) streams.push_back(e);
        for (const std::string& s : e.isDictionary() ? e.getKeys() : std::set<std::string>()) {
          if (e.getKey(s).isStream()) streams.push_back(e.getKey(s));
        }
        for (QPDFObjectHandle st : streams) {
          if (!u.has(st)) UseWalker::walkForm(u, st, res, -1, active);
        }
      }
    }
    tPaintTransparent = nullptr;
    u.pageTransparent.push_back(transparent);
  }
  return u;
}
}
