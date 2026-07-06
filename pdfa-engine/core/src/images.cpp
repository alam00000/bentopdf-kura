#include "images.hh"

#include <qpdf/QPDF.hh>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

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
  if (depth > 4) return 0;
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
      return img;
    }
  }
  int width = d.getKey("/Width").isInteger() ? static_cast<int>(d.getKey("/Width").getIntValue()) : 0;
  int height = d.getKey("/Height").isInteger() ? static_cast<int>(d.getKey("/Height").getIntValue()) : 0;
  int bpc = d.getKey("/BitsPerComponent").isInteger()
                ? static_cast<int>(d.getKey("/BitsPerComponent").getIntValue())
                : 8;
  if (width <= 0 || height <= 0 || static_cast<long long>(width) * height > 100000000LL) {
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

bool transcodeJpxImage(Ctx& ctx, QPDFObjectHandle image) {
  QPDFObjectHandle d = image.getDict();
  QPDFObjectHandle filters = d.getKey("/Filter");
  bool jpxOnly = nameIs(filters, "/JPXDecode") ||
                 (filters.isArray() && filters.getArrayNItems() == 1 &&
                  nameIs(filters.getArrayItem(0), "/JPXDecode"));
  if (!jpxOnly) {
    ctx.fatal("JPX_IN_PDFA1", "JPEG2000 image with chained filters cannot be transcoded");
    return false;
  }
  std::string raw;
  try {
    auto buf = image.getRawStreamData();
    raw.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    ctx.fatal("JPX_IN_PDFA1", "JPEG2000 image data unreadable");
    return false;
  }
  std::string alpha;
  RawImage img = decodeJpxData(raw, alpha);
  if (!img.ok) {
    ctx.fatal("JPX_IN_PDFA1", "JPEG2000 image could not be transcoded (" + img.error + ")");
    return false;
  }
  if (!alpha.empty()) {
    bool cmyk = img.comps == 4;
    size_t pixels = static_cast<size_t>(img.width) * img.height;
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
  ctx.issue("JPX_TRANSCODED", "re-encoded JPEG2000 image as lossless Flate for PDF/A-1", true);
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
}
