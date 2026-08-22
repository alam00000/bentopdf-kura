#include "colorx.hh"

#include <lcms2.h>
#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "assets/cmyk_icc.hh"
#include "assets/srgb_icc.hh"
#include "images.hh"
#include "util.hh"

namespace pdfa {
namespace {
struct Xform {
  cmsHPROFILE rgb = nullptr;
  cmsHPROFILE cmyk = nullptr;
  cmsHTRANSFORM rgb2cmyk = nullptr;
  cmsHTRANSFORM lab2cmyk = nullptr;

  explicit Xform(const std::string& destProfile = std::string()) {
    rgb = cmsOpenProfileFromMem(kSrgbIcc, kSrgbIccLen);
    if (destProfile.size() >= 132 && destProfile.substr(16, 4) == "CMYK") {
      cmyk = cmsOpenProfileFromMem(destProfile.data(),
                                   static_cast<cmsUInt32Number>(destProfile.size()));
    }
    if (!cmyk) cmyk = cmsOpenProfileFromMem(kCmykIcc, kCmykIccLen);
    cmsHPROFILE lab = cmsCreateLab4Profile(nullptr);
    if (rgb && cmyk) {
      rgb2cmyk = cmsCreateTransform(rgb, TYPE_RGB_8, cmyk, TYPE_CMYK_8,
                                    INTENT_RELATIVE_COLORIMETRIC,
                                    cmsFLAGS_BLACKPOINTCOMPENSATION);
    }
    if (lab && cmyk) {
      lab2cmyk = cmsCreateTransform(lab, TYPE_Lab_DBL, cmyk, TYPE_CMYK_8,
                                    INTENT_RELATIVE_COLORIMETRIC,
                                    cmsFLAGS_BLACKPOINTCOMPENSATION);
    }
    if (lab) cmsCloseProfile(lab);
  }

  ~Xform() {
    if (rgb2cmyk) cmsDeleteTransform(rgb2cmyk);
    if (lab2cmyk) cmsDeleteTransform(lab2cmyk);
    if (rgb) cmsCloseProfile(rgb);
    if (cmyk) cmsCloseProfile(cmyk);
  }

  bool ok() const { return rgb2cmyk != nullptr; }

  void rgbBuffer(const unsigned char* in, unsigned char* out, size_t pixels) const {
    if (pixels >= 1u << 21) {
      unsigned n = std::min(4u, std::thread::hardware_concurrency());
      if (n > 1) {
        std::vector<std::thread> ts;
        size_t chunk = pixels / n;
        size_t started = 0;
        try {
          for (unsigned t = 0; t < n; ++t) {
            size_t off = t * chunk;
            size_t cnt = t == n - 1 ? pixels - off : chunk;
            ts.emplace_back([this, in, out, off, cnt] {
              cmsDoTransform(rgb2cmyk, in + off * 3, out + off * 4,
                             static_cast<cmsUInt32Number>(cnt));
            });
            started = off + cnt;
          }
        } catch (const std::system_error&) {
        }
        for (auto& th : ts) {
          if (th.joinable()) th.join();
        }
        if (started >= pixels) return;
        cmsDoTransform(rgb2cmyk, in + started * 3, out + started * 4,
                       static_cast<cmsUInt32Number>(pixels - started));
        return;
      }
    }
    cmsDoTransform(rgb2cmyk, in, out, static_cast<cmsUInt32Number>(pixels));
  }

  void rgb1(double r, double g, double b, double out[4]) const {
    unsigned char in[3] = {static_cast<unsigned char>(std::lround(std::min(std::max(r, 0.0), 1.0) * 255)),
                           static_cast<unsigned char>(std::lround(std::min(std::max(g, 0.0), 1.0) * 255)),
                           static_cast<unsigned char>(std::lround(std::min(std::max(b, 0.0), 1.0) * 255))};
    unsigned char o[4];
    cmsDoTransform(rgb2cmyk, in, o, 1);
    for (int i = 0; i < 4; ++i) out[i] = o[i] / 255.0;
  }
};

enum class SpaceClass { Keep, Rgb, Gray, CmykIcc, Lab, IndexedRgb, SepRgbAlt, ShadeHandled };

std::string fmtReal(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4f", v);
  std::string s = buf;
  size_t last = s.find_last_not_of('0');
  if (last != std::string::npos && s[last] == '.') --last;
  return s.substr(0, last + 1);
}

bool isRgbClassName(const std::string& n) {
  return n == "/DeviceRGB" || n == "/RGB" || n == "/CalRGB";
}

int iccComponents(QPDFObjectHandle cs) {
  if (!cs.isArray() || cs.getArrayNItems() < 2) return 0;
  QPDFObjectHandle prof = cs.getArrayItem(1);
  if (prof.isStream() && prof.getDict().getKey("/N").isInteger()) {
    return static_cast<int>(prof.getDict().getKey("/N").getIntValue());
  }
  return 0;
}

QPDFObjectHandle convertFunction(Ctx& ctx, const Xform& xf, QPDFObjectHandle fn, bool& naive);

QPDFObjectHandle convertType0(Ctx& ctx, const Xform& xf, QPDFObjectHandle fn) {
  QPDFObjectHandle d = fn.getDict();
  std::string data;
  try {
    auto buf = fn.getStreamData(qpdf_dl_all);
    data.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return QPDFObjectHandle();
  }
  int bps = d.getKey("/BitsPerSample").isInteger()
                ? static_cast<int>(d.getKey("/BitsPerSample").getIntValue())
                : 8;
  if (bps < 1 || bps > 32) return QPDFObjectHandle();
  QPDFObjectHandle range = d.getKey("/Range");
  QPDFObjectHandle size = d.getKey("/Size");
  if (!range.isArray() || range.getArrayNItems() < 6 || !size.isArray()) {
    return QPDFObjectHandle();
  }
  long long samples = 1;
  for (int i = 0; i < size.getArrayNItems(); ++i) {
    samples *= size.getArrayItem(i).isInteger() ? size.getArrayItem(i).getIntValue() : 0;
  }
  if (samples <= 0 || samples > 4000000) return QPDFObjectHandle();
  double maxIn = bps >= 32 ? 4294967295.0 : ((1ULL << bps) - 1);
  std::vector<double> lo(3), hi(3);
  for (int c = 0; c < 3; ++c) {
    lo[c] = numOf(range.getArrayItem(c * 2), 0);
    hi[c] = numOf(range.getArrayItem(c * 2 + 1), 1);
  }
  auto readSample = [&](long long idx, int comp) -> double {
    long long bitPos = (idx * 3 + comp) * bps;
    long long bytePos = bitPos / 8;
    unsigned long long acc = 0;
    int need = (bps + 7) / 8 + 1;
    for (int i = 0; i < need; ++i) {
      acc = (acc << 8) |
            (bytePos + i < static_cast<long long>(data.size())
                 ? static_cast<unsigned char>(data[bytePos + i])
                 : 0);
    }
    int shift = need * 8 - static_cast<int>(bitPos % 8) - bps;
    unsigned long long raw = (acc >> shift) & ((bps >= 64 ? ~0ULL : (1ULL << bps) - 1));
    return raw / maxIn;
  };
  std::string outData;
  outData.reserve(static_cast<size_t>(samples) * 4);
  for (long long i = 0; i < samples; ++i) {
    double r = lo[0] + readSample(i, 0) * (hi[0] - lo[0]);
    double g = lo[1] + readSample(i, 1) * (hi[1] - lo[1]);
    double b = lo[2] + readSample(i, 2) * (hi[2] - lo[2]);
    double cmyk[4];
    xf.rgb1(r, g, b, cmyk);
    for (int c = 0; c < 4; ++c) {
      outData += static_cast<char>(std::lround(cmyk[c] * 255.0));
    }
  }
  QPDFObjectHandle nf = QPDFObjectHandle::newStream(&ctx.pdf, outData);
  QPDFObjectHandle nd = nf.getDict();
  nd.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(0));
  nd.replaceKey("/Domain", d.getKey("/Domain"));
  nd.replaceKey("/Size", size);
  nd.replaceKey("/BitsPerSample", QPDFObjectHandle::newInteger(8));
  if (d.hasKey("/Encode")) nd.replaceKey("/Encode", d.getKey("/Encode"));
  QPDFObjectHandle newRange = QPDFObjectHandle::newArray();
  for (int c = 0; c < 4; ++c) {
    newRange.appendItem(QPDFObjectHandle::newInteger(0));
    newRange.appendItem(QPDFObjectHandle::newInteger(1));
  }
  nd.replaceKey("/Range", newRange);
  return ctx.pdf.makeIndirectObject(nf);
}

QPDFObjectHandle convertType2(Ctx& ctx, const Xform& xf, QPDFObjectHandle fn) {
  QPDFObjectHandle c0 = fn.getKey("/C0");
  QPDFObjectHandle c1 = fn.getKey("/C1");
  auto endpoint = [&](QPDFObjectHandle arr, double dflt) {
    double rgb[3] = {dflt, dflt, dflt};
    if (arr.isArray() && arr.getArrayNItems() >= 3) {
      for (int i = 0; i < 3; ++i) rgb[i] = numOf(arr.getArrayItem(i), dflt);
    }
    double cmyk[4];
    xf.rgb1(rgb[0], rgb[1], rgb[2], cmyk);
    QPDFObjectHandle out = QPDFObjectHandle::newArray();
    for (int i = 0; i < 4; ++i) out.appendItem(QPDFObjectHandle::newReal(cmyk[i], 4));
    return out;
  };
  QPDFObjectHandle nf = QPDFObjectHandle::newDictionary();
  nf.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(2));
  nf.replaceKey("/Domain", fn.hasKey("/Domain") ? fn.getKey("/Domain")
                                                : QPDFObjectHandle::parse("[0 1]"));
  nf.replaceKey("/N", fn.hasKey("/N") ? fn.getKey("/N") : QPDFObjectHandle::newInteger(1));
  nf.replaceKey("/C0", endpoint(c0, 0.0));
  nf.replaceKey("/C1", endpoint(c1, 1.0));
  return ctx.pdf.makeIndirectObject(nf);
}

const char* kPsRgb2Cmyk =
    " 1 exch sub 3 1 roll 1 exch sub 3 1 roll 1 exch sub 3 1 roll"
    " 3 copy 2 copy gt { exch } if pop 2 copy gt { exch } if pop"
    " 4 1 roll 3 index sub 3 1 roll 3 index sub 3 1 roll 3 index sub 3 1 roll 4 -1 roll ";

QPDFObjectHandle convertType4(Ctx& ctx, QPDFObjectHandle fn, bool& naive) {
  std::string body;
  try {
    auto buf = fn.getStreamData(qpdf_dl_all);
    body.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return QPDFObjectHandle();
  }
  size_t open = body.find('{');
  size_t close = body.rfind('}');
  if (open == std::string::npos || close == std::string::npos || close <= open) {
    return QPDFObjectHandle();
  }
  std::string inner = body.substr(open + 1, close - open - 1);
  std::string newBody = "{ " + inner + kPsRgb2Cmyk + "}";
  QPDFObjectHandle nf = QPDFObjectHandle::newStream(&ctx.pdf, newBody);
  QPDFObjectHandle nd = nf.getDict();
  nd.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(4));
  nd.replaceKey("/Domain", fn.getDict().getKey("/Domain"));
  QPDFObjectHandle newRange = QPDFObjectHandle::newArray();
  for (int c = 0; c < 4; ++c) {
    newRange.appendItem(QPDFObjectHandle::newInteger(0));
    newRange.appendItem(QPDFObjectHandle::newInteger(1));
  }
  nd.replaceKey("/Range", newRange);
  naive = true;
  return ctx.pdf.makeIndirectObject(nf);
}

int& x1aDepth() {
  static thread_local int d = 0;
  return d;
}

struct X1aGuard {
  bool over;
  X1aGuard() : over(x1aDepth() > 200) { ++x1aDepth(); }
  ~X1aGuard() { --x1aDepth(); }
};

QPDFObjectHandle convertFunction(Ctx& ctx, const Xform& xf, QPDFObjectHandle fn, bool& naive) {
  X1aGuard g_;
  if (g_.over) return QPDFObjectHandle();

  QPDFObjectHandle d = fn.isStream() ? fn.getDict() : fn;
  if (!d.isDictionary()) return QPDFObjectHandle();
  long long type = d.getKey("/FunctionType").isInteger()
                       ? d.getKey("/FunctionType").getIntValue()
                       : -1;
  if (type == 0 && fn.isStream()) return convertType0(ctx, xf, fn);
  if (type == 2) return convertType2(ctx, xf, fn);
  if (type == 3) {
    QPDFObjectHandle funcs = d.getKey("/Functions");
    if (!funcs.isArray()) return QPDFObjectHandle();
    QPDFObjectHandle newFuncs = QPDFObjectHandle::newArray();
    for (int i = 0; i < funcs.getArrayNItems(); ++i) {
      QPDFObjectHandle sub = convertFunction(ctx, xf, funcs.getArrayItem(i), naive);
      if (!sub.isInitialized()) return QPDFObjectHandle();
      newFuncs.appendItem(sub);
    }
    QPDFObjectHandle nf = QPDFObjectHandle::newDictionary();
    nf.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(3));
    nf.replaceKey("/Domain", d.getKey("/Domain"));
    nf.replaceKey("/Functions", newFuncs);
    if (d.hasKey("/Bounds")) nf.replaceKey("/Bounds", d.getKey("/Bounds"));
    if (d.hasKey("/Encode")) nf.replaceKey("/Encode", d.getKey("/Encode"));
    QPDFObjectHandle newRange = QPDFObjectHandle::newArray();
    for (int c = 0; c < 4; ++c) {
      newRange.appendItem(QPDFObjectHandle::newInteger(0));
      newRange.appendItem(QPDFObjectHandle::newInteger(1));
    }
    nf.replaceKey("/Range", newRange);
    return ctx.pdf.makeIndirectObject(nf);
  }
  if (type == 4 && fn.isStream()) return convertType4(ctx, fn, naive);
  return QPDFObjectHandle();
}

struct ConvertState {
  explicit ConvertState(const std::string& destProfile) : xf(destProfile) {}
  Xform xf;
  Visited imagesDone;
  Visited spacesDone;
  Visited streamsDone;
  int images = 0;
  int ops = 0;
  int spaces = 0;
  int shadings = 0;
  bool naiveFunction = false;
  bool failed = false;
  std::string failReason;
};

bool spaceIsRgbLike(QPDFObjectHandle cs, int depth = 0);

bool spaceIsRgbLike(QPDFObjectHandle cs, int depth) {
  if (depth > 6) return false;
  if (cs.isName()) return isRgbClassName(cs.getName());
  if (!cs.isArray() || cs.getArrayNItems() < 1) return false;
  std::string family = nameOf(cs.getArrayItem(0));
  if (family == "/CalRGB" || family == "/Lab") return true;
  if (family == "/ICCBased") return iccComponents(cs) == 3;
  return false;
}

void convertImage(Ctx& ctx, ConvertState& st, QPDFObjectHandle image) {
  if (!st.imagesDone.enter(image)) return;
  QPDFObjectHandle d = image.getDict();
  QPDFObjectHandle cs = d.getKey("/ColorSpace");
  bool indexed = false;
  QPDFObjectHandle base;
  if (cs.isArray() && cs.getArrayNItems() >= 2 &&
      (nameOf(cs.getArrayItem(0)) == "/Indexed" || nameOf(cs.getArrayItem(0)) == "/I")) {
    indexed = true;
    base = cs.getArrayItem(1);
  }
  QPDFObjectHandle effective = indexed ? base : cs;
  bool rgbLike = spaceIsRgbLike(effective);
  bool grayIcc = false;
  bool cmykIcc = false;
  if (effective.isArray() && nameOf(effective.getArrayItem(0)) == "/ICCBased") {
    int n = iccComponents(effective);
    grayIcc = n == 1;
    cmykIcc = n == 4;
  }
  if (effective.isArray() && nameOf(effective.getArrayItem(0)) == "/CalGray") grayIcc = true;
  if (!rgbLike && !grayIcc && !cmykIcc) return;

  if (grayIcc || cmykIcc) {
    QPDFObjectHandle name = QPDFObjectHandle::newName(grayIcc ? "/DeviceGray" : "/DeviceCMYK");
    if (indexed) {
      cs.setArrayItem(1, name);
    } else {
      d.replaceKey("/ColorSpace", name);
    }
    ++st.spaces;
    return;
  }

  RawImage img = decodeImage(image);
  if (!img.ok || img.comps != 3) {
    st.failed = true;
    st.failReason = "image not convertible (" + (img.ok ? "components" : img.error) + ")";
    return;
  }
  size_t pixels = static_cast<size_t>(img.width) * img.height;
  std::string out(pixels * 4, '\0');
  st.xf.rgbBuffer(reinterpret_cast<const unsigned char*>(img.samples.data()),
                  reinterpret_cast<unsigned char*>(&out[0]), pixels);
  std::string encoded;
  const char* newFilter = nullptr;
  if (pixels > 262144) {
    encoded = encodeCmykJpeg(out, img.width, img.height, 90);
    if (!encoded.empty()) newFilter = "/DCTDecode";
  }
  if (!newFilter) {
    std::string flated = flateCompress(out);
    if (!flated.empty() && (encoded.empty() || flated.size() < encoded.size())) {
      encoded = flated;
      newFilter = "/FlateDecode";
    }
  }
  if (newFilter) {
    image.replaceStreamData(encoded, QPDFObjectHandle::newName(newFilter),
                            QPDFObjectHandle::newNull());
  } else {
    image.replaceStreamData(out, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    d.removeKey("/Filter");
  }
  d.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceCMYK"));
  d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
  d.replaceKey("/Width", QPDFObjectHandle::newInteger(img.width));
  d.replaceKey("/Height", QPDFObjectHandle::newInteger(img.height));
  d.removeKey("/Decode");
  d.removeKey("/DecodeParms");
  d.removeKey("/Mask");
  d.removeKey("/Intent");
  ++st.images;
}

void convertIndexedLookup(Ctx& ctx, ConvertState& st, QPDFObjectHandle cs) {
  QPDFObjectHandle base = cs.getArrayItem(1);
  QPDFObjectHandle hival = cs.getArrayItem(2);
  QPDFObjectHandle lookup = cs.getArrayItem(3);
  if (!spaceIsRgbLike(base)) return;
  std::string table;
  if (lookup.isString()) {
    table = lookup.getStringValue();
  } else if (lookup.isStream()) {
    try {
      auto buf = lookup.getStreamData(qpdf_dl_all);
      table.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    } catch (...) {
      st.failed = true;
      st.failReason = "indexed lookup unreadable";
      return;
    }
  } else {
    return;
  }
  int entries = static_cast<int>(table.size() / 3);
  std::string out;
  out.reserve(entries * 4);
  std::vector<unsigned char> cmyk(entries * 4);
  st.xf.rgbBuffer(reinterpret_cast<const unsigned char*>(table.data()), cmyk.data(), entries);
  out.assign(reinterpret_cast<const char*>(cmyk.data()), cmyk.size());
  cs.setArrayItem(1, QPDFObjectHandle::newName("/DeviceCMYK"));
  cs.setArrayItem(3, QPDFObjectHandle::newString(out));
  ++st.spaces;
}

bool convertSpaceObject(Ctx& ctx, ConvertState& st, QPDFObjectHandle holder,
                        const std::string& key, QPDFObjectHandle cs,
                        std::map<std::string, bool>* opFix);

void convertShading(Ctx& ctx, ConvertState& st, QPDFObjectHandle sh) {
  X1aGuard g_;
  if (g_.over) { st.failed = true; st.failReason = "shading nesting too deep"; return; }
  QPDFObjectHandle d = sh.isStream() ? sh.getDict() : sh;
  if (!d.isDictionary() || !st.spacesDone.enter(sh)) return;
  QPDFObjectHandle cs = d.getKey("/ColorSpace");
  bool indexed = cs.isArray() && cs.getArrayNItems() >= 4 &&
                 (nameOf(cs.getArrayItem(0)) == "/Indexed" || nameOf(cs.getArrayItem(0)) == "/I");
  if (indexed) {
    convertIndexedLookup(ctx, st, cs);
    return;
  }
  if (!spaceIsRgbLike(cs)) return;
  QPDFObjectHandle fn = d.getKey("/Function");
  bool okAll = true;
  if (fn.isArray()) {
    QPDFObjectHandle newArr = QPDFObjectHandle::newArray();
    for (int i = 0; i < fn.getArrayNItems(); ++i) {
      QPDFObjectHandle nf = convertFunction(ctx, st.xf, fn.getArrayItem(i), st.naiveFunction);
      if (!nf.isInitialized()) {
        okAll = false;
        break;
      }
      newArr.appendItem(nf);
    }
    if (okAll) d.replaceKey("/Function", newArr);
  } else if (fn.isDictionary() || fn.isStream()) {
    QPDFObjectHandle nf = convertFunction(ctx, st.xf, fn, st.naiveFunction);
    okAll = nf.isInitialized();
    if (okAll) d.replaceKey("/Function", nf);
  }
  if (!okAll) {
    st.failed = true;
    st.failReason = "shading function not convertible";
    return;
  }
  d.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceCMYK"));
  QPDFObjectHandle bg = d.getKey("/Background");
  if (bg.isArray() && bg.getArrayNItems() >= 3) {
    double out[4];
    st.xf.rgb1(numOf(bg.getArrayItem(0), 0), numOf(bg.getArrayItem(1), 0),
               numOf(bg.getArrayItem(2), 0), out);
    QPDFObjectHandle nbg = QPDFObjectHandle::newArray();
    for (int i = 0; i < 4; ++i) nbg.appendItem(QPDFObjectHandle::newReal(out[i], 4));
    d.replaceKey("/Background", nbg);
  }
  ++st.shadings;
}

bool convertSpaceObject(Ctx& ctx, ConvertState& st, QPDFObjectHandle holder,
                        const std::string& key, QPDFObjectHandle cs,
                        std::map<std::string, bool>* opFix) {
  X1aGuard g_;
  if (g_.over) { st.failed = true; st.failReason = "colour space nesting too deep"; return false; }
  if (cs.isName()) {
    if (isRgbClassName(cs.getName())) {
      holder.replaceKey(key, QPDFObjectHandle::newName("/DeviceCMYK"));
      if (opFix) (*opFix)[key] = true;
      ++st.spaces;
    }
    return true;
  }
  if (!cs.isArray() || cs.getArrayNItems() < 1) return true;
  std::string family = nameOf(cs.getArrayItem(0));
  if (family == "/CalRGB" || family == "/Lab") {
    holder.replaceKey(key, QPDFObjectHandle::newName("/DeviceCMYK"));
    if (opFix) (*opFix)[key] = true;
    ++st.spaces;
    return true;
  }
  if (family == "/CalGray") {
    holder.replaceKey(key, QPDFObjectHandle::newName("/DeviceGray"));
    ++st.spaces;
    return true;
  }
  if (family == "/ICCBased") {
    int n = iccComponents(cs);
    if (n == 3) {
      holder.replaceKey(key, QPDFObjectHandle::newName("/DeviceCMYK"));
      if (opFix) (*opFix)[key] = true;
      ++st.spaces;
    } else if (n == 1) {
      holder.replaceKey(key, QPDFObjectHandle::newName("/DeviceGray"));
      ++st.spaces;
    } else if (n == 4) {
      holder.replaceKey(key, QPDFObjectHandle::newName("/DeviceCMYK"));
      ++st.spaces;
    }
    return true;
  }
  if (family == "/Indexed" || family == "/I") {
    if (cs.getArrayNItems() >= 4) convertIndexedLookup(ctx, st, cs);
    return true;
  }
  if ((family == "/Separation" || family == "/DeviceN") && cs.getArrayNItems() >= 3) {
    QPDFObjectHandle alt = cs.getArrayItem(2);
    if (spaceIsRgbLike(alt)) {
      if (cs.getArrayNItems() < 4) {
        st.failed = true;
        st.failReason = "separation with RGB alternate lacks tint transform";
        return false;
      }
      QPDFObjectHandle nf = convertFunction(ctx, st.xf, cs.getArrayItem(3), st.naiveFunction);
      if (!nf.isInitialized()) {
        st.failed = true;
        st.failReason = "tint transform not convertible";
        return false;
      }
      cs.setArrayItem(2, QPDFObjectHandle::newName("/DeviceCMYK"));
      cs.setArrayItem(3, nf);
      ++st.spaces;
    }
    return true;
  }
  if (family == "/Pattern" && cs.getArrayNItems() >= 2) {
    QPDFObjectHandle under = cs.getArrayItem(1);
    if (spaceIsRgbLike(under)) {
      cs.setArrayItem(1, QPDFObjectHandle::newName("/DeviceCMYK"));
      if (opFix) (*opFix)[key] = true;
      ++st.spaces;
    }
    return true;
  }
  return true;
}

class RgbOpFilter : public QPDFObjectHandle::TokenFilter {
 public:
  RgbOpFilter(ConvertState& st, const std::map<std::string, bool>& fixNames)
      : st(st), fixNames(fixNames) {}

  void handleToken(QPDFTokenizer::Token const& token) override {
    QPDFTokenizer::token_type_e type = token.getType();
    if (type != QPDFTokenizer::tt_word) {
      operands.push_back(token);
      return;
    }
    std::string op = token.getValue();
    if (op == "q") {
      stack.push_back(cur);
    } else if (op == "Q") {
      if (!stack.empty()) {
        cur = stack.back();
        stack.pop_back();
      }
    } else if (op == "rg" || op == "RG") {
      double v[3];
      if (lastNumbers(3, v)) {
        double out[4];
        st.xf.rgb1(v[0], v[1], v[2], out);
        dropLastNumbers(3);
        flush();
        write(fmtReal(out[0]) + " " + fmtReal(out[1]) + " " + fmtReal(out[2]) + " " +
              fmtReal(out[3]) + (op == "rg" ? " k" : " K"));
        ++st.ops;
        return;
      }
    } else if (op == "cs" || op == "CS") {
      std::string name = lastName();
      bool fix = false;
      if (name == "/DeviceRGB") {
        replaceLastName("/DeviceCMYK");
        fix = true;
        ++st.ops;
      } else if (fixNames.count(name)) {
        fix = true;
      }
      (op == "cs" ? cur.fillRgb : cur.strokeRgb) = fix;
    } else if (op == "sc" || op == "scn" || op == "SC" || op == "SCN") {
      bool fix = (op == "sc" || op == "scn") ? cur.fillRgb : cur.strokeRgb;
      double v[3];
      if (fix && !lastIsName() && lastNumbers(3, v) && numericCount() == 3) {
        double out[4];
        st.xf.rgb1(v[0], v[1], v[2], out);
        dropLastNumbers(3);
        flush();
        write(fmtReal(out[0]) + " " + fmtReal(out[1]) + " " + fmtReal(out[2]) + " " +
              fmtReal(out[3]) + " " + op);
        ++st.ops;
        return;
      }
    }
    flush();
    writeToken(token);
  }

  void handleEOF() override { flush(); }

 private:
  struct Cur {
    bool fillRgb = false;
    bool strokeRgb = false;
  };

  bool lastIsName() {
    for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
      auto t = it->getType();
      if (t == QPDFTokenizer::tt_space || t == QPDFTokenizer::tt_comment) continue;
      return t == QPDFTokenizer::tt_name;
    }
    return false;
  }

  int numericCount() {
    int n = 0;
    for (const auto& t : operands) {
      auto ty = t.getType();
      if (ty == QPDFTokenizer::tt_integer || ty == QPDFTokenizer::tt_real) ++n;
    }
    return n;
  }

  std::string lastName() {
    for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
      if (it->getType() == QPDFTokenizer::tt_name) return it->getValue();
    }
    return std::string();
  }

  void replaceLastName(const std::string& n) {
    for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
      if (it->getType() == QPDFTokenizer::tt_name) {
        *it = QPDFTokenizer::Token(QPDFTokenizer::tt_name, n);
        return;
      }
    }
  }

  bool lastNumbers(int n, double* out) {
    int found = 0;
    for (auto it = operands.rbegin(); it != operands.rend() && found < n; ++it) {
      auto ty = it->getType();
      if (ty == QPDFTokenizer::tt_space || ty == QPDFTokenizer::tt_comment) continue;
      if (ty == QPDFTokenizer::tt_integer || ty == QPDFTokenizer::tt_real) {
        out[n - 1 - found] = std::strtod(it->getValue().c_str(), nullptr);
        ++found;
      } else {
        return false;
      }
    }
    return found == n;
  }

  void dropLastNumbers(int n) {
    int dropped = 0;
    while (!operands.empty() && dropped < n) {
      auto ty = operands.back().getType();
      operands.pop_back();
      if (ty == QPDFTokenizer::tt_integer || ty == QPDFTokenizer::tt_real) ++dropped;
    }
  }

  void flush() {
    for (const auto& t : operands) writeToken(t);
    operands.clear();
  }

  ConvertState& st;
  const std::map<std::string, bool>& fixNames;
  std::vector<QPDFTokenizer::Token> operands;
  Cur cur;
  std::vector<Cur> stack;
};

void rewriteContent(Ctx& ctx, ConvertState& st, QPDFObjectHandle holder,
                    const std::map<std::string, bool>& fixNames) {
  if (!st.streamsDone.enter(holder)) return;
  try {
    RgbOpFilter filter(st, fixNames);
    Pl_Buffer buf("x1a color rewrite");
    size_t sourceLen = 0;
    if (holder.isStream()) {
      auto src = holder.getStreamData(qpdf_dl_all);
      sourceLen = src->getSize();
      holder.filterAsContents(&filter, &buf);
    } else {
      QPDFPageObjectHelper ph(holder);
      ph.filterContents(&filter, &buf);
    }
    auto data = buf.getBufferSharedPointer();
    std::string rewritten(reinterpret_cast<const char*>(data->getBuffer()), data->getSize());
    if (holder.isStream()) {
      if (rewritten.empty() && sourceLen > 0) {
        st.failed = true;
        st.failReason = "content stream rewrite produced no output";
        return;
      }
      holder.replaceStreamData(rewritten, QPDFObjectHandle::newNull(),
                               QPDFObjectHandle::newNull());
    } else {
      holder.replaceKey(
          "/Contents",
          ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, rewritten)));
    }
  } catch (...) {
    st.failed = true;
    st.failReason = "content stream rewrite failed";
  }
}

void processResources(Ctx& ctx, ConvertState& st, QPDFObjectHandle res, Visited& visited,
                      std::map<std::string, bool>& fixNames, int depth = 0);

struct RecurGuard {
  int& d;
  bool over;
  explicit RecurGuard(int& c) : d(c), over(c > 250) { ++d; }
  ~RecurGuard() { --d; }
};

void processResources(Ctx& ctx, ConvertState& st, QPDFObjectHandle res, Visited& visited,
                      std::map<std::string, bool>& fixNames, int depth) {
  static thread_local int liveDepth = 0;
  RecurGuard guard(liveDepth);
  if (guard.over || depth > 64) return;
  if (!res.isDictionary() || !visited.enter(res)) return;
  QPDFObjectHandle csd = res.getKey("/ColorSpace");
  if (csd.isDictionary()) {
    for (const std::string& k : csd.getKeys()) {
      convertSpaceObject(ctx, st, csd, k, csd.getKey(k), &fixNames);
      if (st.failed) return;
    }
  }
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (xod.isDictionary()) {
    for (const std::string& k : xod.getKeys()) {
      QPDFObjectHandle xo = xod.getKey(k);
      if (!xo.isStream()) continue;
      std::string subtype = nameOf(xo.getDict().getKey("/Subtype"));
      if (subtype == "/Image") {
        convertImage(ctx, st, xo);
        if (st.failed) return;
      } else if (subtype == "/Form" && visited.enter(xo)) {
        std::map<std::string, bool> inner;
        processResources(ctx, st, xo.getDict().getKey("/Resources"), visited, inner, depth + 1);
        if (st.failed) return;
        rewriteContent(ctx, st, xo, inner);
        if (st.failed) return;
      }
    }
  }
  QPDFObjectHandle shd = res.getKey("/Shading");
  if (shd.isDictionary()) {
    for (const std::string& k : shd.getKeys()) {
      convertShading(ctx, st, shd.getKey(k));
      if (st.failed) return;
    }
  }
  QPDFObjectHandle pat = res.getKey("/Pattern");
  if (pat.isDictionary()) {
    for (const std::string& k : pat.getKeys()) {
      QPDFObjectHandle p = pat.getKey(k);
      if (p.isStream() && visited.enter(p)) {
        std::map<std::string, bool> inner;
        processResources(ctx, st, p.getDict().getKey("/Resources"), visited, inner, depth + 1);
        if (st.failed) return;
        rewriteContent(ctx, st, p, inner);
        if (st.failed) return;
      } else if (p.isDictionary()) {
        QPDFObjectHandle sh = p.getKey("/Shading");
        if (!sh.isNull()) convertShading(ctx, st, sh);
        if (st.failed) return;
      }
    }
  }
  QPDFObjectHandle fonts = res.getKey("/Font");
  if (fonts.isDictionary()) {
    for (const std::string& k : fonts.getKeys()) {
      QPDFObjectHandle fnt = fonts.getKey(k);
      if (!fnt.isDictionary()) continue;
      QPDFObjectHandle cp = fnt.getKey("/CharProcs");
      if (cp.isDictionary() && visited.enter(cp)) {
        std::map<std::string, bool> inner;
        QPDFObjectHandle fres = fnt.getKey("/Resources");
        processResources(ctx, st, fres.isDictionary() ? fres : res, visited, inner, depth + 1);
        if (st.failed) return;
        for (const std::string& g : cp.getKeys()) {
          QPDFObjectHandle glyph = cp.getKey(g);
          if (glyph.isStream() && visited.enter(glyph)) {
            rewriteContent(ctx, st, glyph, inner);
            if (st.failed) return;
          }
        }
      }
    }
  }
}
}

void convertColorsX1a(Ctx& ctx) {
  ConvertState st(ctx.opt.destProfile);
  if (!st.xf.ok()) {
    ctx.fatal("X_COLOR_ENGINE", "color transform initialization failed");
    return;
  }
  QPDFPageDocumentHelper dh(ctx.pdf);
  Visited visited;
  for (auto& ph : dh.getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    std::map<std::string, bool> fixNames;
    QPDFObjectHandle pres = ph.getAttribute("/Resources", false);
    processResources(ctx, st, pres.isDictionary() ? pres : page.getKey("/Resources"),
                     visited, fixNames);
    if (st.failed) break;
    QPDFObjectHandle group = page.getKey("/Group");
    if (group.isDictionary() && group.hasKey("/CS")) {
      convertSpaceObject(ctx, st, group, "/CS", group.getKey("/CS"), nullptr);
    }
    rewriteContent(ctx, st, page, fixNames);
    if (st.failed) break;
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (annots.isArray()) {
      for (int i = 0; i < annots.getArrayNItems() && !st.failed; ++i) {
        QPDFObjectHandle a = annots.getArrayItem(i);
        if (!a.isDictionary()) continue;
        QPDFObjectHandle ap = a.getKey("/AP");
        if (!ap.isDictionary()) continue;
        QPDFObjectHandle n = ap.getKey("/N");
        std::vector<QPDFObjectHandle> streams;
        if (n.isStream()) streams.push_back(n);
        else if (n.isDictionary()) {
          for (const std::string& k : n.getKeys()) {
            if (n.getKey(k).isStream()) streams.push_back(n.getKey(k));
          }
        }
        for (QPDFObjectHandle s : streams) {
          if (!visited.enter(s)) continue;
          std::map<std::string, bool> inner;
          processResources(ctx, st, s.getDict().getKey("/Resources"), visited, inner);
          if (st.failed) break;
          rewriteContent(ctx, st, s, inner);
        }
      }
    }
    if (st.failed) break;
  }
  if (st.failed) {
    ctx.fatal("X1A_COLOR_UNCONVERTIBLE",
              "RGB content could not be fully converted to CMYK (" + st.failReason +
                  "); target PDF/X-3 or PDF/X-4 which permit colour-managed RGB");
    return;
  }
  if (st.images || st.ops || st.spaces || st.shadings) {
    ctx.issue("X1A_COLOR_CONVERTED",
              "converted RGB content to CMYK for PDF/X-1a: " + std::to_string(st.images) +
                  " image(s), " + std::to_string(st.ops) + " operator(s), " +
                  std::to_string(st.spaces) + " colour space(s), " +
                  std::to_string(st.shadings) + " shading(s)",
              true);
  }
  if (st.naiveFunction) {
    ctx.issue("X1A_FUNCTION_APPROXIMATED",
              "PostScript calculator tint/shading functions converted with an arithmetic "
              "RGB->CMYK approximation rather than ICC transform",
              true);
  }
}
}
