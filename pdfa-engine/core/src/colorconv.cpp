#include "colorconv.hh"

#include <lcms2.h>
#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "assets/cmyk_icc.hh"
#include "assets/srgb_icc.hh"
#include "images.hh"
#include "limits.hh"
#include "passes.hh"
#include "pdffunc.hh"
#include "util.hh"

namespace pdfa {
namespace {
std::string lowerNoSpace(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return o;
}

std::string profileBytes(cmsHPROFILE p) {
  cmsUInt32Number n = 0;
  if (!p || !cmsSaveProfileToMem(p, nullptr, &n) || n == 0) return std::string();
  std::string out(n, '\0');
  if (!cmsSaveProfileToMem(p, &out[0], &n)) return std::string();
  out.resize(n);
  return out;
}

cmsHPROFILE synthGray(double dotGain, double gamma) {
  cmsToneCurve* curve = nullptr;
  if (dotGain > 0) {
    float pts[256];
    double g = std::log(0.5 - std::min(dotGain, 0.49)) / std::log(0.5);
    for (int i = 0; i < 256; ++i) {
      double v = i / 255.0;
      pts[i] = static_cast<float>(std::max(std::pow(v, g), v / 16));
    }
    curve = cmsBuildTabulatedToneCurveFloat(nullptr, 256, pts);
  } else {
    curve = cmsBuildGamma(nullptr, gamma > 0 ? gamma : 2.2);
  }
  if (!curve) return nullptr;
  cmsHPROFILE p = cmsCreateGrayProfile(cmsD50_xyY(), curve);
  cmsFreeToneCurve(curve);
  return p;
}

cmsHPROFILE synthAdobeRgb() {
  cmsCIExyY white = {0.3127, 0.3290, 1.0};
  cmsCIExyYTRIPLE prim = {{0.64, 0.33, 1.0}, {0.21, 0.71, 1.0}, {0.15, 0.06, 1.0}};
  cmsToneCurve* g = cmsBuildGamma(nullptr, 563.0 / 256.0);
  if (!g) return nullptr;
  cmsToneCurve* curves[3] = {g, g, g};
  cmsHPROFILE p = cmsCreateRGBProfile(&white, &prim, curves);
  cmsFreeToneCurve(g);
  return p;
}

std::string builtinByName(const std::string& name) {
  std::string k = lowerNoSpace(name);
  if (k.find("srgb") != std::string::npos) {
    return std::string(reinterpret_cast<const char*>(kSrgbIcc), kSrgbIccLen);
  }
  if (k.find("adobergb") != std::string::npos) {
    cmsHPROFILE p = synthAdobeRgb();
    std::string b = profileBytes(p);
    if (p) cmsCloseProfile(p);
    return b;
  }
  if (k.rfind("dotgain", 0) == 0) {
    double pct = std::atof(k.c_str() + 7);
    if (pct <= 0 || pct > 60) pct = 20;
    cmsHPROFILE p = synthGray(pct / 100.0, 0);
    std::string b = profileBytes(p);
    if (p) cmsCloseProfile(p);
    return b;
  }
  if (k.rfind("graygamma", 0) == 0) {
    double g = std::atof(k.c_str() + 9);
    cmsHPROFILE p = synthGray(0, g > 0 ? g : 2.2);
    std::string b = profileBytes(p);
    if (p) cmsCloseProfile(p);
    return b;
  }
  if (k.find("gracol2006") != std::string::npos || k.find("coatedgracol") != std::string::npos ||
      k == "default" || k.empty()) {
    return std::string(reinterpret_cast<const char*>(kCmykIcc), kCmykIccLen);
  }
  return std::string();
}

bool isProcessName(const std::string& n) {
  return n == "/Cyan" || n == "/Magenta" || n == "/Yellow" || n == "/Black";
}

std::string fmtNum(double v) { return fmtFixed(v, 4); }

struct Space;
using SpacePtr = std::shared_ptr<Space>;

struct Space {
  enum Fam { kGray, kRgb, kCmyk, kLab, kIcc, kCalGray, kCalRgb, kSep, kDevN, kIndexed, kPattern,
             kOther };
  Fam fam = kOther;
  int n = 0;
  unsigned cls = 0;
  cmsHPROFILE prof = nullptr;
  QPDFObjectHandle obj;
  std::shared_ptr<PdfFunction> tint;
  SpacePtr alt;
  std::vector<std::string> colorants;
  SpacePtr base;
  std::string lookup;
  int hival = 0;
  double labRange[4] = {-100, 100, -100, 100};
  bool device() const { return fam == kGray || fam == kRgb || fam == kCmyk; }
};

struct Stats {
  int ops = 0;
  int images = 0;
  int shadings = 0;
  int spaces = 0;
  int alternates = 0;
  int groups = 0;
  int failures = 0;
  std::set<std::string> failReasons;
};

class Engine {
 public:
  Engine(Ctx& ctx, const ColorJob& job) : ctx(ctx), job(job) {}

  ~Engine() {
    for (auto& kv : xforms) cmsDeleteTransform(kv.second);
    for (cmsHPROFILE p : owned) cmsCloseProfile(p);
  }

  bool init(std::string& err) {
    std::string destBytes = job.destProfile;
    if (!job.deviceLink.empty()) {
      link = open(job.deviceLink);
      if (!link || cmsGetDeviceClass(link) != cmsSigLinkClass) {
        err = "the DeviceLink profile could not be read";
        return false;
      }
      linkIn = channelsOf(cmsGetColorSpace(link));
      destN = channelsOf(cmsGetPCS(link));
      if (linkIn == 0 || (destN != 1 && destN != 3 && destN != 4)) {
        err = "the DeviceLink profile has an unsupported colour space";
        return false;
      }
      if (destBytes.empty()) {
        if (destN == 4) destBytes.assign(reinterpret_cast<const char*>(kCmykIcc), kCmykIccLen);
        else if (destN == 3) destBytes.assign(reinterpret_cast<const char*>(kSrgbIcc), kSrgbIccLen);
      }
    }
    if (destBytes.empty() && !job.destName.empty()) resolveIccByName(ctx.opt, job.destName, destBytes);
    if (destBytes.empty()) destBytes.assign(reinterpret_cast<const char*>(kCmykIcc), kCmykIccLen);
    dest = open(destBytes);
    if (!dest) {
      err = "the destination profile could not be read";
      return false;
    }
    int dn = channelsOf(cmsGetColorSpace(dest));
    if (dn != 1 && dn != 3 && dn != 4) {
      err = "the destination profile is not a gray, RGB or CMYK profile";
      return false;
    }
    if (!link) destN = dn;
    destIcc = destBytes;
    workRgb = workingProfile(job.rgbName, 3);
    workCmyk = workingProfile(job.cmykName, 4);
    workGray = workingProfile(job.grayName, 1);
    if (!workRgb || !workCmyk || !workGray) {
      err = "a working colour profile could not be built";
      return false;
    }
    if (destN == 4) buildBlackLut();
    return true;
  }

  int destComponents() const { return destN; }

  std::string destDeviceName() const {
    return destN == 4 ? "/DeviceCMYK" : destN == 3 ? "/DeviceRGB" : "/DeviceGray";
  }

  QPDFObjectHandle destSpaceObject() {
    if (job.embed != ColorJob::kEmbedSource) return QPDFObjectHandle::newName(destDeviceName());
    if (!destIccObj.isInitialized()) {
      QPDFObjectHandle s = QPDFObjectHandle::newStream(&ctx.pdf, destIcc);
      s.getDict().replaceKey("/N", QPDFObjectHandle::newInteger(destN));
      QPDFObjectHandle arr = QPDFObjectHandle::newArray();
      arr.appendItem(QPDFObjectHandle::newName("/ICCBased"));
      arr.appendItem(ctx.pdf.makeIndirectObject(s));
      destIccObj = ctx.pdf.makeIndirectObject(arr);
    }
    return destIccObj;
  }

  QPDFObjectHandle workingSpaceObject(int n) {
    QPDFObjectHandle& slot = n == 4 ? tagCmyk : n == 3 ? tagRgb : tagGray;
    if (!slot.isInitialized()) {
      cmsHPROFILE p = n == 4 ? workCmyk : n == 3 ? workRgb : workGray;
      QPDFObjectHandle s = QPDFObjectHandle::newStream(&ctx.pdf, profileBytes(p));
      s.getDict().replaceKey("/N", QPDFObjectHandle::newInteger(n));
      QPDFObjectHandle arr = QPDFObjectHandle::newArray();
      arr.appendItem(QPDFObjectHandle::newName("/ICCBased"));
      arr.appendItem(ctx.pdf.makeIndirectObject(s));
      slot = ctx.pdf.makeIndirectObject(arr);
    }
    return slot;
  }

  SpacePtr resolve(QPDFObjectHandle cs, QPDFObjectHandle res, int depth = 0) {
    if (depth > kMaxColorSpaceNest) return nullptr;
    if (cs.isName()) {
      std::string n = cs.getName();
      auto mk = [&](Space::Fam f, int comps, unsigned cls, cmsHPROFILE p) {
        auto s = std::make_shared<Space>();
        s->fam = f;
        s->n = comps;
        s->cls = cls;
        s->prof = p;
        s->obj = cs;
        return s;
      };
      if (n == "/DeviceGray" || n == "/G") return mk(Space::kGray, 1, ColorRule::kUncGray, workGray);
      if (n == "/DeviceRGB" || n == "/RGB") return mk(Space::kRgb, 3, ColorRule::kUncRgb, workRgb);
      if (n == "/DeviceCMYK" || n == "/CMYK") return mk(Space::kCmyk, 4, ColorRule::kUncCmyk, workCmyk);
      if (n == "/Pattern") {
        auto s = mk(Space::kPattern, 0, 0, nullptr);
        return s;
      }
      if (n == "/Lab") return mk(Space::kLab, 3, ColorRule::kLab, labProfile());
      if (res.isDictionary()) {
        QPDFObjectHandle d = res.getKey("/ColorSpace");
        if (d.isDictionary() && d.hasKey(n)) return resolve(d.getKey(n), QPDFObjectHandle(), depth + 1);
      }
      return nullptr;
    }
    if (!cs.isArray() || cs.getArrayNItems() < 1) return nullptr;
    if (cs.isIndirect()) {
      auto it = cache.find(cs.getObjGen());
      if (it != cache.end()) return it->second;
    }
    SpacePtr s = resolveArray(cs, res, depth);
    if (s && cs.isIndirect()) cache[cs.getObjGen()] = s;
    return s;
  }

  bool decide(unsigned objectBit, const Space& s, ColorRule::Action& action, int& intent) const {
    unsigned cls = s.cls;
    if (s.fam == Space::kIndexed && s.base) cls = s.base->cls;
    for (const ColorRule& r : job.rules) {
      if ((r.objects & objectBit) && (r.sources & cls)) {
        action = r.action;
        intent = r.intent;
        return action != ColorRule::kNone;
      }
    }
    return false;
  }

  bool probe(const Space& s) {
    double in[32], out[4];
    int n = s.fam == Space::kLab ? 3 : s.n;
    for (int i = 0; i < n && i < 32; ++i) in[i] = s.fam == Space::kLab ? (i == 0 ? 50 : 0) : 1;
    return convertValues(s, in, out, ColorRule::kConvert, -1);
  }

  bool anyConverts(const Space& s) const {
    ColorRule::Action a;
    int i;
    for (unsigned b : {1u, 2u, 4u, 8u, 16u}) {
      if (decide(b, s, a, i)) return true;
    }
    return false;
  }

  bool convertValues(const Space& s, const double* in, double* out, ColorRule::Action action,
                     int intent) {
    double tmp[32];
    if (s.fam == Space::kSep || s.fam == Space::kDevN) {
      if (!s.tint || !s.alt || !s.tint->compute(in, tmp)) return false;
      return convertValues(*s.alt, tmp, out, action == ColorRule::kConvertAlternate ? ColorRule::kConvert : action, intent);
    }
    if (s.fam == Space::kIndexed) {
      if (!s.base) return false;
      int idx = static_cast<int>(std::lround(in[0]));
      idx = std::min(std::max(idx, 0), s.hival);
      int bn = s.base->n;
      for (int c = 0; c < bn; ++c) {
        size_t off = static_cast<size_t>(idx) * bn + c;
        unsigned char b = off < s.lookup.size() ? static_cast<unsigned char>(s.lookup[off]) : 0;
        tmp[c] = b / 255.0;
        if (s.base->fam == Space::kLab) {
          if (c == 0) tmp[c] *= 100;
          else tmp[c] = s.base->labRange[(c - 1) * 2] + tmp[c] * (s.base->labRange[(c - 1) * 2 + 1] - s.base->labRange[(c - 1) * 2]);
        }
      }
      return convertValues(*s.base, tmp, out, action, intent);
    }
    if (action == ColorRule::kInkLimit) {
      if (s.n != 4 || destN != 4 || s.fam == Space::kLab) return false;
      for (int c = 0; c < 4; ++c) out[c] = std::min(std::max(in[c], 0.0), 1.0);
      return finish(out);
    }
    cmsHPROFILE src = s.prof;
    if (action == ColorRule::kDecalibrateConvert) {
      src = s.n == 4 ? workCmyk : s.n == 3 ? workRgb : s.n == 1 ? workGray : s.prof;
    }
    if (!src) return false;
    if (job.preserveBlack && destN == 4 && s.n == 4 && in[0] <= 0.001 && in[1] <= 0.001 &&
        in[2] <= 0.001 && s.fam != Space::kLab) {
      out[0] = out[1] = out[2] = 0;
      out[3] = in[3];
      return finish(out);
    }
    if (destN == 4 && job.grayToBlackOnly && s.n == 1 && s.fam != Space::kLab) {
      out[0] = out[1] = out[2] = 0;
      out[3] = blackFor(src, in[0], 1);
      return finish(out);
    }
    if (destN == 4 && job.rgbGrayToBlackOnly && s.n == 3 && s.fam != Space::kLab) {
      double hi = std::max({in[0], in[1], in[2]});
      double lo = std::min({in[0], in[1], in[2]});
      double mean = (in[0] + in[1] + in[2]) / 3;
      double tol = mean > 0.66 ? job.rgbGrayTolerance[0]
                               : mean > 0.33 ? job.rgbGrayTolerance[1] : job.rgbGrayTolerance[2];
      if (hi - lo <= 2 * tol + 1e-9) {
        out[0] = out[1] = out[2] = 0;
        out[3] = blackFor(src, mean, 3);
        return finish(out);
      }
    }
    if (job.quick && s.n == 3 && destN == 4) {
      double c = 1 - in[0], m = 1 - in[1], y = 1 - in[2];
      double k = std::min({c, m, y}) * job.quickBlackGeneration;
      double ucr = k * job.quickUnderColorRemoval;
      out[0] = std::max(0.0, c - ucr);
      out[1] = std::max(0.0, m - ucr);
      out[2] = std::max(0.0, y - ucr);
      out[3] = k;
      return finish(out);
    }
    cmsHTRANSFORM xf = transformFor(src, s.fam == Space::kLab, intent);
    if (!xf) return false;
    double inBuf[4], outBuf[4];
    int sn = s.fam == Space::kLab ? 3 : s.n;
    for (int c = 0; c < sn && c < 4; ++c) {
      double v = in[c];
      if (s.fam != Space::kLab) {
        v = std::min(std::max(v, 0.0), 1.0);
        if (sn == 4) v *= 100;
      }
      inBuf[c] = v;
    }
    cmsDoTransform(xf, inBuf, outBuf, 1);
    for (int c = 0; c < destN; ++c) {
      double v = destN == 4 ? outBuf[c] / 100 : outBuf[c];
      out[c] = std::min(std::max(v, 0.0), 1.0);
    }
    return finish(out);
  }

  bool convertPixels(const Space& s, const unsigned char* in, unsigned char* out, size_t pixels,
                     ColorRule::Action action, int intent) {
    bool plain = (s.fam == Space::kGray || s.fam == Space::kRgb || s.fam == Space::kCmyk ||
                  s.fam == Space::kIcc || s.fam == Space::kCalGray || s.fam == Space::kCalRgb) &&
                 !job.quick && !(job.preserveBlack && destN == 4 && s.n == 4) &&
                 !(destN == 4 && (job.grayToBlackOnly && s.n == 1)) &&
                 !(destN == 4 && job.rgbGrayToBlackOnly && s.n == 3) && job.inkLimit <= 0 &&
                 job.toneCurves.empty();
    if (plain) {
      cmsHPROFILE src = s.prof;
      if (action == ColorRule::kDecalibrateConvert) {
        src = s.n == 4 ? workCmyk : s.n == 3 ? workRgb : workGray;
      }
      cmsHTRANSFORM xf = transform8For(src, s.n, intent);
      if (!xf) return false;
      unsigned threads = pixels >= (1u << 21) ? std::max(1u, std::min(8u, std::thread::hardware_concurrency())) : 1;
      size_t per = (pixels + threads - 1) / threads;
      std::vector<std::thread> pool;
      auto work = [&](size_t from, size_t count) {
        size_t done = 0;
        while (done < count) {
          size_t chunk = std::min<size_t>(count - done, 1u << 20);
          cmsDoTransform(xf, in + (from + done) * s.n, out + (from + done) * destN,
                         static_cast<cmsUInt32Number>(chunk));
          done += chunk;
        }
      };
      size_t started = 0;
      try {
        for (unsigned t = 1; t < threads; ++t) {
          size_t from = per * t;
          if (from >= pixels) break;
          pool.emplace_back(work, from, std::min(per, pixels - from));
          started = t;
        }
      } catch (const std::system_error&) {
      }
      work(0, std::min(per, pixels));
      for (auto& th : pool) th.join();
      for (unsigned t = static_cast<unsigned>(started) + 1; t < threads; ++t) {
        size_t from = per * t;
        if (from < pixels) work(from, std::min(per, pixels - from));
      }
      return true;
    }
    std::map<std::string, std::string> memo;
    int n = s.fam == Space::kLab ? 3 : s.n;
    double vin[32], vout[4];
    for (size_t p = 0; p < pixels; ++p) {
      const unsigned char* px = in + p * n;
      std::string key(reinterpret_cast<const char*>(px), n);
      auto it = memo.find(key);
      if (it != memo.end()) {
        std::memcpy(out + p * destN, it->second.data(), destN);
        continue;
      }
      for (int c = 0; c < n; ++c) {
        vin[c] = px[c] / 255.0;
        if (s.fam == Space::kLab) {
          if (c == 0) vin[c] *= 100;
          else vin[c] = s.labRange[(c - 1) * 2] + vin[c] * (s.labRange[(c - 1) * 2 + 1] - s.labRange[(c - 1) * 2]);
        }
      }
      if (!convertValues(s, vin, vout, action, intent)) return false;
      std::string val(destN, '\0');
      for (int c = 0; c < destN; ++c) val[c] = static_cast<char>(std::lround(vout[c] * 255));
      std::memcpy(out + p * destN, val.data(), destN);
      if (memo.size() < 65536) memo.emplace(std::move(key), std::move(val));
    }
    return true;
  }

  Ctx& ctx;
  const ColorJob& job;
  Stats stats;

 private:
  int channelsOf(cmsColorSpaceSignature sig) {
    switch (sig) {
      case cmsSigGrayData: return 1;
      case cmsSigRgbData: return 3;
      case cmsSigCmykData: return 4;
      case cmsSigLabData: return 3;
      default: return 0;
    }
  }

  cmsHPROFILE open(const std::string& bytes) {
    if (bytes.size() < 132) return nullptr;
    cmsHPROFILE p = cmsOpenProfileFromMem(bytes.data(), static_cast<cmsUInt32Number>(bytes.size()));
    if (p) owned.push_back(p);
    return p;
  }

  cmsHPROFILE workingProfile(const std::string& name, int n) {
    std::string b;
    if (!name.empty()) resolveIccByName(ctx.opt, name, b);
    if (b.empty() || channelsOfBytes(b) != n) {
      if (n == 4) b.assign(reinterpret_cast<const char*>(kCmykIcc), kCmykIccLen);
      else if (n == 3) b.assign(reinterpret_cast<const char*>(kSrgbIcc), kSrgbIccLen);
      else b = builtinByName("Dot Gain 15%");
    }
    return open(b);
  }

  static int channelsOfBytes(const std::string& b) {
    if (b.size() < 20) return 0;
    std::string sig = b.substr(16, 4);
    return sig == "GRAY" ? 1 : sig == "RGB " ? 3 : sig == "CMYK" ? 4 : 0;
  }

  int effectiveIntent(int intent) const {
    if (intent >= 0 && intent <= 3) return intent;
    return INTENT_RELATIVE_COLORIMETRIC;
  }

  cmsUInt32Number flags() const {
    return (job.blackPointCompensation ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0) | cmsFLAGS_NOCACHE;
  }

  static cmsUInt32Number dblFormat(int n, bool lab) {
    if (lab) return TYPE_Lab_DBL;
    return n == 4 ? TYPE_CMYK_DBL : n == 3 ? TYPE_RGB_DBL : TYPE_GRAY_DBL;
  }

  static cmsUInt32Number byteFormat(int n) {
    return n == 4 ? TYPE_CMYK_8 : n == 3 ? TYPE_RGB_8 : TYPE_GRAY_8;
  }

  bool linkApplies(cmsHPROFILE src, bool isLab) const {
    if (!link || isLab) return false;
    return channelsOfSig(cmsGetColorSpace(src)) == linkIn;
  }

  static int channelsOfSig(cmsColorSpaceSignature sig) {
    return sig == cmsSigGrayData ? 1 : sig == cmsSigRgbData ? 3 : sig == cmsSigCmykData ? 4 : 0;
  }

  cmsHTRANSFORM transformFor(cmsHPROFILE src, bool isLab, int intent) {
    int in = isLab ? 3 : channelsOfSig(cmsGetColorSpace(src));
    if (!isLab && in == 0) return nullptr;
    int ri = effectiveIntent(intent);
    std::string key = "d" + std::to_string(reinterpret_cast<uintptr_t>(src)) + ":" +
                      std::to_string(isLab) + ":" + std::to_string(ri);
    auto it = xforms.find(key);
    if (it != xforms.end()) return it->second;
    cmsHTRANSFORM xf = nullptr;
    if (linkApplies(src, isLab)) {
      xf = cmsCreateTransform(link, dblFormat(in, false), nullptr, dblFormat(destN, false), ri, flags());
    } else {
      cmsHPROFILE s = src;
      if (isLab) s = labProfile();
      xf = cmsCreateTransform(s, dblFormat(in, isLab), dest, dblFormat(destN, false), ri, flags());
    }
    if (xf) xforms[key] = xf;
    return xf;
  }

  cmsHTRANSFORM transform8For(cmsHPROFILE src, int n, int intent) {
    int ri = effectiveIntent(intent);
    std::string key = "b" + std::to_string(reinterpret_cast<uintptr_t>(src)) + ":" + std::to_string(ri);
    auto it = xforms.find(key);
    if (it != xforms.end()) return it->second;
    cmsHTRANSFORM xf = nullptr;
    if (linkApplies(src, false)) {
      xf = cmsCreateTransform(link, byteFormat(n), nullptr, byteFormat(destN), ri, flags());
    } else {
      xf = cmsCreateTransform(src, byteFormat(n), dest, byteFormat(destN), ri, flags());
    }
    if (xf) xforms[key] = xf;
    return xf;
  }

  cmsHPROFILE labProfile() {
    if (!lab) {
      lab = cmsCreateLab4Profile(nullptr);
      if (lab) owned.push_back(lab);
    }
    return lab;
  }

  double lightness(cmsHPROFILE p, int n, const double* v) {
    std::string key = "L" + std::to_string(reinterpret_cast<uintptr_t>(p));
    cmsHTRANSFORM xf;
    auto it = xforms.find(key);
    if (it != xforms.end()) {
      xf = it->second;
    } else {
      xf = cmsCreateTransform(p, dblFormat(n, false), labProfile(), TYPE_Lab_DBL,
                              INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
      if (!xf) return 0;
      xforms[key] = xf;
    }
    double in[4], out[3];
    for (int c = 0; c < n; ++c) in[c] = n == 4 ? v[c] * 100 : v[c];
    cmsDoTransform(xf, in, out, 1);
    return out[0];
  }

  void buildBlackLut() {
    blackL.assign(101, 0);
    for (int k = 0; k <= 100; ++k) {
      double v[4] = {0, 0, 0, k / 100.0};
      blackL[k] = lightness(dest, 4, v);
    }
  }

  double blackFor(cmsHPROFILE src, double value, int n) {
    if (blackL.empty()) return 1 - value;
    double v[3] = {value, value, value};
    double z[3] = {0, 0, 0};
    double o[3] = {1, 1, 1};
    double l = lightness(src, n, v);
    double l0 = lightness(src, n, z);
    double l1 = lightness(src, n, o);
    double t = l1 > l0 ? (l - l0) / (l1 - l0) : value;
    t = std::min(std::max(t, 0.0), 1.0);
    double paper = blackL[0], solid = blackL[100];
    if (paper <= solid) return 1 - t;
    for (int k = 1; k <= 100; ++k) {
      double tk = (blackL[k] - solid) / (paper - solid);
      double tp = (blackL[k - 1] - solid) / (paper - solid);
      if (tk <= t) {
        double f = tp > tk ? (tp - t) / (tp - tk) : 0;
        return std::min(1.0, (k - 1 + f) / 100.0);
      }
    }
    return 1;
  }

  bool finish(double* out) {
    if (destN == 4 && job.inkLimit > 0) {
      double sum = out[0] + out[1] + out[2] + out[3];
      double limit = job.inkLimit / 100.0;
      if (sum > limit) {
        double cmy = out[0] + out[1] + out[2];
        double excess = sum - limit;
        if (cmy > 0) {
          double f = std::max(0.0, (cmy - excess) / cmy);
          for (int c = 0; c < 3; ++c) out[c] *= f;
        }
        sum = out[0] + out[1] + out[2] + out[3];
        if (sum > limit) out[3] = std::max(0.0, out[3] - (sum - limit));
      }
    }
    for (const auto& tc : job.toneCurves) {
      for (int c = 0; c < destN; ++c) {
        if (!(tc.first & (1u << c))) continue;
        out[c] = applyCurve(tc.second, out[c]);
      }
    }
    return true;
  }

  static double applyCurve(const std::vector<double>& pts, double v) {
    if (pts.size() < 4) return v;
    double x = v * 100;
    for (size_t i = 2; i + 1 < pts.size(); i += 2) {
      if (x <= pts[i] || i + 3 >= pts.size()) {
        double x0 = pts[i - 2], y0 = pts[i - 1], x1 = pts[i], y1 = pts[i + 1];
        double y = x1 == x0 ? y1 : y0 + (x - x0) * (y1 - y0) / (x1 - x0);
        return std::min(std::max(y / 100, 0.0), 1.0);
      }
    }
    return v;
  }

  SpacePtr resolveArray(QPDFObjectHandle cs, QPDFObjectHandle res, int depth) {
    std::string fam = nameOf(cs.getArrayItem(0));
    auto s = std::make_shared<Space>();
    s->obj = cs;
    if (cs.getArrayNItems() == 1) return resolve(cs.getArrayItem(0), res, depth + 1);
    if (fam == "/ICCBased") {
      QPDFObjectHandle st = cs.getArrayItem(1);
      if (!st.isStream()) return nullptr;
      int n = st.getDict().getKey("/N").isInteger() ? static_cast<int>(st.getDict().getKey("/N").getIntValue()) : 0;
      if (n != 1 && n != 3 && n != 4) return nullptr;
      cmsHPROFILE p = nullptr;
      auto pit = iccProfiles.find(st.getObjGen());
      if (st.isIndirect() && pit != iccProfiles.end()) {
        p = pit->second;
      } else {
        try {
          auto buf = st.getStreamData(qpdf_dl_all);
          p = open(std::string(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize()));
        } catch (...) {
        }
        if (p && channelsOfSig(cmsGetColorSpace(p)) != n) p = nullptr;
        if (st.isIndirect()) iccProfiles[st.getObjGen()] = p;
      }
      if (!p) p = n == 4 ? workCmyk : n == 3 ? workRgb : workGray;
      s->fam = Space::kIcc;
      s->n = n;
      s->prof = p;
      s->cls = n == 4 ? ColorRule::kCalCmyk : n == 3 ? ColorRule::kCalRgb : ColorRule::kCalGray;
      return s;
    }
    if (fam == "/CalRGB" || fam == "/CalGray") {
      QPDFObjectHandle d = cs.getArrayItem(1);
      bool rgb = fam == "/CalRGB";
      s->fam = rgb ? Space::kCalRgb : Space::kCalGray;
      s->n = rgb ? 3 : 1;
      s->cls = rgb ? ColorRule::kCalRgb : ColorRule::kCalGray;
      s->prof = calProfile(d, rgb);
      if (!s->prof) s->prof = rgb ? workRgb : workGray;
      return s;
    }
    if (fam == "/Lab") {
      s->fam = Space::kLab;
      s->n = 3;
      s->cls = ColorRule::kLab;
      QPDFObjectHandle d = cs.getArrayItem(1);
      if (d.isDictionary() && d.getKey("/Range").isArray() && d.getKey("/Range").getArrayNItems() == 4) {
        for (int i = 0; i < 4; ++i) s->labRange[i] = numOf(d.getKey("/Range").getArrayItem(i), s->labRange[i]);
      }
      s->prof = labProfile();
      return s;
    }
    if ((fam == "/Separation" || fam == "/DeviceN") && cs.getArrayNItems() >= 4) {
      bool sep = fam == "/Separation";
      s->fam = sep ? Space::kSep : Space::kDevN;
      QPDFObjectHandle names = cs.getArrayItem(1);
      if (sep) {
        s->colorants.push_back(nameOf(names));
      } else if (names.isArray()) {
        for (int i = 0; i < names.getArrayNItems(); ++i) s->colorants.push_back(nameOf(names.getArrayItem(i)));
      }
      s->n = static_cast<int>(s->colorants.size());
      if (s->n < 1 || s->n > 32) return nullptr;
      s->alt = resolve(cs.getArrayItem(2), res, depth + 1);
      s->tint = std::shared_ptr<PdfFunction>(PdfFunction::loadArray(cs.getArrayItem(3)).release());
      bool registration = sep && (s->colorants[0] == "/All");
      bool process = !registration;
      for (const std::string& c : s->colorants) {
        if (!isProcessName(c) && c != "/None") process = false;
      }
      if (registration) s->cls = ColorRule::kRegistration;
      else if (process && job.processSeparationsAsCmyk) s->cls = ColorRule::kUncCmyk;
      else s->cls = ColorRule::kSpot;
      return s;
    }
    if ((fam == "/Indexed" || fam == "/I") && cs.getArrayNItems() >= 4) {
      s->fam = Space::kIndexed;
      s->n = 1;
      s->base = resolve(cs.getArrayItem(1), res, depth + 1);
      if (!s->base) return nullptr;
      s->cls = s->base->cls;
      s->hival = static_cast<int>(numOf(cs.getArrayItem(2), 0));
      if (s->hival < 0 || s->hival > 255) return nullptr;
      QPDFObjectHandle lk = cs.getArrayItem(3);
      if (lk.isString()) {
        s->lookup = lk.getStringValue();
      } else if (lk.isStream()) {
        try {
          auto buf = lk.getStreamData(qpdf_dl_all);
          s->lookup.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
        } catch (...) {
          return nullptr;
        }
      }
      return s;
    }
    if (fam == "/Pattern") {
      s->fam = Space::kPattern;
      s->base = resolve(cs.getArrayItem(1), res, depth + 1);
      if (s->base) {
        s->cls = s->base->cls;
        s->n = s->base->n;
      }
      return s;
    }
    return nullptr;
  }

  cmsHPROFILE calProfile(QPDFObjectHandle d, bool rgb) {
    if (!d.isDictionary()) return nullptr;
    QPDFObjectHandle wp = d.getKey("/WhitePoint");
    if (!wp.isArray() || wp.getArrayNItems() != 3) return nullptr;
    cmsCIEXYZ w = {numOf(wp.getArrayItem(0), 0.9505), numOf(wp.getArrayItem(1), 1),
                   numOf(wp.getArrayItem(2), 1.089)};
    double ws = w.X + w.Y + w.Z;
    if (ws <= 0) return nullptr;
    cmsCIExyY wxy = {w.X / ws, w.Y / ws, 1};
    cmsHPROFILE p = nullptr;
    if (!rgb) {
      cmsToneCurve* g = cmsBuildGamma(nullptr, numOf(d.getKey("/Gamma"), 1));
      if (!g) return nullptr;
      p = cmsCreateGrayProfile(&wxy, g);
      cmsFreeToneCurve(g);
    } else {
      double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
      QPDFObjectHandle ma = d.getKey("/Matrix");
      if (ma.isArray() && ma.getArrayNItems() == 9) {
        for (int i = 0; i < 9; ++i) m[i] = numOf(ma.getArrayItem(i), m[i]);
      }
      cmsCIExyY pr[3];
      for (int i = 0; i < 3; ++i) {
        double sum = m[i * 3] + m[i * 3 + 1] + m[i * 3 + 2];
        if (sum <= 0) return nullptr;
        pr[i] = {m[i * 3] / sum, m[i * 3 + 1] / sum, m[i * 3 + 1]};
      }
      cmsCIExyYTRIPLE prim = {pr[0], pr[1], pr[2]};
      double gm[3] = {1, 1, 1};
      QPDFObjectHandle ga = d.getKey("/Gamma");
      if (ga.isArray() && ga.getArrayNItems() == 3) {
        for (int i = 0; i < 3; ++i) gm[i] = numOf(ga.getArrayItem(i), 1);
      }
      cmsToneCurve* c[3] = {cmsBuildGamma(nullptr, gm[0]), cmsBuildGamma(nullptr, gm[1]),
                            cmsBuildGamma(nullptr, gm[2])};
      if (c[0] && c[1] && c[2]) p = cmsCreateRGBProfile(&wxy, &prim, c);
      for (auto* t : c) {
        if (t) cmsFreeToneCurve(t);
      }
    }
    if (p) owned.push_back(p);
    return p;
  }

  cmsHPROFILE dest = nullptr;
  cmsHPROFILE link = nullptr;
  cmsHPROFILE lab = nullptr;
  cmsHPROFILE workRgb = nullptr, workCmyk = nullptr, workGray = nullptr;
  int linkIn = 0;
  int destN = 4;
  std::string destIcc;
  QPDFObjectHandle destIccObj, tagRgb, tagCmyk, tagGray;
  std::vector<double> blackL;
  std::vector<cmsHPROFILE> owned;
  std::map<std::string, cmsHTRANSFORM> xforms;
  std::map<QPDFObjGen, SpacePtr> cache;
  std::map<QPDFObjGen, cmsHPROFILE> iccProfiles;
};

QPDFObjectHandle sampledFunction(QPDF& pdf, int inputs, const std::vector<double>& domain,
                                 int outputs,
                                 const std::function<bool(const double*, double*)>& fn) {
  int per = inputs == 1 ? 256 : inputs == 2 ? 33 : inputs == 3 ? 17 : inputs == 4 ? 9 : inputs == 5 ? 6 : 4;
  long long total = 1;
  for (int i = 0; i < inputs; ++i) total *= per;
  if (total > 2000000) return QPDFObjectHandle();
  std::string data;
  data.reserve(static_cast<size_t>(total * outputs * 2));
  std::vector<int> idx(inputs, 0);
  double in[32], out[32];
  for (long long k = 0; k < total; ++k) {
    long long r = k;
    for (int i = 0; i < inputs; ++i) {
      idx[i] = static_cast<int>(r % per);
      r /= per;
      double lo = domain[2 * i], hi = domain[2 * i + 1];
      in[i] = lo + (hi - lo) * idx[i] / (per - 1);
    }
    if (!fn(in, out)) return QPDFObjectHandle();
    for (int o = 0; o < outputs; ++o) {
      unsigned v = static_cast<unsigned>(std::lround(std::min(std::max(out[o], 0.0), 1.0) * 65535));
      data += static_cast<char>(v >> 8);
      data += static_cast<char>(v & 0xFF);
    }
  }
  QPDFObjectHandle st = QPDFObjectHandle::newStream(&pdf, data);
  QPDFObjectHandle d = st.getDict();
  d.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(0));
  QPDFObjectHandle dom = QPDFObjectHandle::newArray(), size = QPDFObjectHandle::newArray(),
                   range = QPDFObjectHandle::newArray();
  for (int i = 0; i < inputs; ++i) {
    dom.appendItem(QPDFObjectHandle::newReal(domain[2 * i], 4));
    dom.appendItem(QPDFObjectHandle::newReal(domain[2 * i + 1], 4));
    size.appendItem(QPDFObjectHandle::newInteger(per));
  }
  for (int o = 0; o < outputs; ++o) {
    range.appendItem(QPDFObjectHandle::newInteger(0));
    range.appendItem(QPDFObjectHandle::newInteger(1));
  }
  d.replaceKey("/Domain", dom);
  d.replaceKey("/Size", size);
  d.replaceKey("/Range", range);
  d.replaceKey("/BitsPerSample", QPDFObjectHandle::newInteger(16));
  return pdf.makeIndirectObject(st);
}

class BitReader {
 public:
  explicit BitReader(const std::string& d) : data(d) {}
  bool read(int bits, unsigned long long& v) {
    v = 0;
    for (int i = 0; i < bits; ++i) {
      if (pos >= data.size() * 8) return false;
      unsigned char b = static_cast<unsigned char>(data[pos >> 3]);
      v = (v << 1) | ((b >> (7 - (pos & 7))) & 1);
      ++pos;
    }
    return true;
  }
  void align() { pos = (pos + 7) & ~static_cast<size_t>(7); }
  bool atEnd() const { return data.size() * 8 - pos < 8; }
  bool more() const { return pos < data.size() * 8; }

 private:
  const std::string& data;
  size_t pos = 0;
};

class BitWriter {
 public:
  void write(int bits, unsigned long long v) {
    for (int i = bits - 1; i >= 0; --i) {
      cur = static_cast<unsigned char>((cur << 1) | ((v >> i) & 1));
      if (++n == 8) {
        out += static_cast<char>(cur);
        cur = 0;
        n = 0;
      }
    }
  }
  void align() {
    while (n) write(1, 0);
  }
  std::string out;

 private:
  unsigned char cur = 0;
  int n = 0;
};

class Converter {
 public:
  Converter(Ctx& ctx, Engine& e, const ColorRemap* remap = nullptr) : ctx(ctx), e(e), remap(remap) {}

  void page(QPDFPageObjectHelper& ph) {
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle res = ph.getAttribute("/Resources", true);
    resources(res, 0);
    if (!remap) group(page);
    rewrite(page, res, 0, true);
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (annots.isArray()) {
      for (int i = 0; i < annots.getArrayNItems(); ++i) {
        for (QPDFObjectHandle s : normalAppearanceStreams(annots.getArrayItem(i))) {
          form(s, res, 0);
        }
      }
    }
  }

  void fail(const std::string& why) {
    ++e.stats.failures;
    e.stats.failReasons.insert(why);
  }

  void group(QPDFObjectHandle holder) {
    QPDFObjectHandle g = holder.isStream() ? holder.getDict().getKey("/Group") : holder.getKey("/Group");
    if (!g.isDictionary() || !g.hasKey("/CS") || !e.job.blendSpaceToDest) return;
    SpacePtr s = e.resolve(g.getKey("/CS"), QPDFObjectHandle());
    if (!s || !e.anyConverts(*s)) return;
    g.replaceKey("/CS", QPDFObjectHandle::newName(e.destDeviceName()));
    ++e.stats.groups;
  }

  void resources(QPDFObjectHandle res, int depth) {
    if (!res.isDictionary() || depth > kMaxResourceNest || !visited.enter(res)) return;
    QPDFObjectHandle csd = res.getKey("/ColorSpace");
    if (csd.isDictionary() && !remap) {
      for (const std::string& k : csd.getKeys()) alternates(csd.getKey(k), res);
      pendingSpaces.push_back(res);
    }
    QPDFObjectHandle xo = res.getKey("/XObject");
    if (xo.isDictionary()) {
      for (const std::string& k : xo.getKeys()) {
        QPDFObjectHandle x = xo.getKey(k);
        if (!x.isStream()) continue;
        std::string st = nameOf(x.getDict().getKey("/Subtype"));
        if (st == "/Image" && !remap) image(x, res);
        else if (st == "/Form") form(x, res, depth + 1);
      }
    }
    QPDFObjectHandle sh = res.getKey("/Shading");
    if (sh.isDictionary() && !remap) {
      for (const std::string& k : sh.getKeys()) shading(sh.getKey(k), res);
    }
    QPDFObjectHandle pat = res.getKey("/Pattern");
    if (pat.isDictionary()) {
      for (const std::string& k : pat.getKeys()) {
        QPDFObjectHandle p = pat.getKey(k);
        if (p.isStream()) {
          if (!visited.enter(p)) continue;
          QPDFObjectHandle pr = p.getDict().getKey("/Resources");
          if (pr.isDictionary()) resources(pr, depth + 1);
          else pr = res;
          rewrite(p, pr, 0, false);
        } else if (p.isDictionary() && !remap) {
          shading(p.getKey("/Shading"), res);
        }
      }
    }
    QPDFObjectHandle gs = res.getKey("/ExtGState");
    if (gs.isDictionary()) {
      for (const std::string& k : gs.getKeys()) {
        QPDFObjectHandle g = gs.getKey(k);
        if (!g.isDictionary() || !visited.enter(g)) continue;
        QPDFObjectHandle sm = g.getKey("/SMask");
        if (sm.isDictionary() && (e.job.softMasks || remap)) {
          QPDFObjectHandle gf = sm.getKey("/G");
          if (gf.isStream()) {
            QPDFObjectHandle grp = gf.getDict().getKey("/Group");
            QPDFObjectHandle bc = sm.getKey("/BC");
            if (!remap && e.job.blendSpaceToDest && grp.isDictionary() && bc.isArray() && grp.hasKey("/CS")) {
              SpacePtr s = e.resolve(grp.getKey("/CS"), QPDFObjectHandle());
              if (s && s->n == bc.getArrayNItems() && e.anyConverts(*s)) {
                double in[4], out[4];
                for (int i = 0; i < s->n && i < 4; ++i) in[i] = numOf(bc.getArrayItem(i), 0);
                ColorRule::Action a = ColorRule::kConvert;
                if (e.convertValues(*s, in, out, a, -1)) {
                  QPDFObjectHandle nb = QPDFObjectHandle::newArray();
                  for (int i = 0; i < e.destComponents(); ++i) nb.appendItem(QPDFObjectHandle::newReal(out[i], 4));
                  sm.replaceKey("/BC", nb);
                }
              }
            }
            form(gf, res, depth + 1);
          }
        }
      }
    }
    QPDFObjectHandle fonts = res.getKey("/Font");
    if (fonts.isDictionary()) {
      for (const std::string& k : fonts.getKeys()) {
        QPDFObjectHandle f = fonts.getKey(k);
        if (!f.isDictionary()) continue;
        QPDFObjectHandle cp = f.getKey("/CharProcs");
        if (!cp.isDictionary() || !visited.enter(cp)) continue;
        QPDFObjectHandle fr = f.getKey("/Resources");
        if (fr.isDictionary()) resources(fr, depth + 1);
        else fr = res;
        for (const std::string& g : cp.getKeys()) {
          QPDFObjectHandle glyph = cp.getKey(g);
          if (glyph.isStream() && visited.enter(glyph)) rewrite(glyph, fr, ColorRule::kText, false);
        }
      }
    }
  }

  void retireSpaces() {
    for (QPDFObjectHandle res : pendingSpaces) {
      QPDFObjectHandle csd = res.getKey("/ColorSpace");
      if (!csd.isDictionary()) continue;
      for (const std::string& k : csd.getKeys()) {
        if (k.rfind("/Kura", 0) == 0) continue;
        SpacePtr s = e.resolve(csd.getKey(k), res);
        if (!s || s->fam == Space::kPattern || s->device()) continue;
        bool all = true;
        for (unsigned b : {1u, 2u, 4u, 8u, 16u}) {
          ColorRule::Action a;
          int i;
          if (!e.decide(b, *s, a, i) || a != ColorRule::kConvert) all = false;
        }
        if (!all || !e.probe(*s)) continue;
        csd.replaceKey(k, e.destSpaceObject());
        ++e.stats.spaces;
      }
    }
  }

  void form(QPDFObjectHandle f, QPDFObjectHandle parentRes, int depth) {
    if (!f.isStream() || depth > kMaxResourceNest || !visited.enter(f)) return;
    QPDFObjectHandle r = f.getDict().getKey("/Resources");
    if (r.isDictionary()) resources(r, depth + 1);
    else r = parentRes;
    if (!remap) group(f);
    rewrite(f, r, 0, false);
  }

  void alternates(QPDFObjectHandle cs, QPDFObjectHandle res) {
    if (!cs.isArray() || !visited.enter(cs)) return;
    SpacePtr s = e.resolve(cs, res);
    if (!s) return;
    if (s->fam == Space::kIndexed && s->base) {
      alternates(cs.getArrayItem(1), res);
      return;
    }
    if (s->fam != Space::kSep && s->fam != Space::kDevN) return;
    ColorRule::Action a = ColorRule::kNone;
    int intent = -1;
    bool hit = false;
    for (unsigned b : {1u, 2u, 4u, 8u, 16u}) {
      ColorRule::Action ab;
      int ib;
      if (e.decide(b, *s, ab, ib) && ab == ColorRule::kConvertAlternate) {
        a = ab;
        intent = ib;
        hit = true;
        break;
      }
    }
    if (!hit || !s->alt || !s->tint) return;
    if (!e.anyConverts(*s->alt)) return;
    std::vector<double> dom;
    for (int i = 0; i < s->n; ++i) {
      dom.push_back(0);
      dom.push_back(1);
    }
    Space altOnly = *s->alt;
    QPDFObjectHandle fn = sampledFunction(
        ctx.pdf, s->n, dom, e.destComponents(), [&](const double* in, double* out) {
          double mid[32];
          if (!s->tint->compute(in, mid)) return false;
          return e.convertValues(altOnly, mid, out, ColorRule::kConvert, intent);
        });
    if (!fn.isInitialized()) {
      fail("spot colour alternate could not be resampled");
      return;
    }
    cs.setArrayItem(2, e.destSpaceObject());
    cs.setArrayItem(3, fn);
    ++e.stats.alternates;
    (void)a;
  }

  unsigned imageClass(QPDFObjectHandle img) {
    QPDFObjectHandle f = img.getDict().getKey("/Filter");
    std::string last = nameOf(f);
    if (f.isArray() && f.getArrayNItems() > 0) last = nameOf(f.getArrayItem(f.getArrayNItems() - 1));
    return (last == "/DCTDecode" || last == "/DCT" || last == "/JPXDecode") ? ColorRule::kImageLossy
                                                                             : ColorRule::kImageLossless;
  }

  void image(QPDFObjectHandle img, QPDFObjectHandle res) {
    if (!visited.enter(img)) return;
    QPDFObjectHandle d = img.getDict();
    if (d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue()) return;
    QPDFObjectHandle cs = d.getKey("/ColorSpace");
    if (cs.isNull()) return;
    SpacePtr s = e.resolve(cs, res);
    if (!s || s->fam == Space::kPattern) return;
    unsigned cls = imageClass(img);
    ColorRule::Action a;
    int intent;
    if (!e.decide(cls, *s, a, intent)) return;
    if (intent < 0) intent = intentOf(d.getKey("/Intent"));
    if (a == ColorRule::kDecalibrate || a == ColorRule::kTag) {
      QPDFObjectHandle target = cs;
      bool indexed = s->fam == Space::kIndexed;
      Space& eff = indexed ? *s->base : *s;
      QPDFObjectHandle repl;
      if (a == ColorRule::kDecalibrate && (eff.fam == Space::kIcc || eff.fam == Space::kCalRgb || eff.fam == Space::kCalGray)) {
        repl = QPDFObjectHandle::newName(eff.n == 4 ? "/DeviceCMYK" : eff.n == 3 ? "/DeviceRGB" : "/DeviceGray");
      } else if (a == ColorRule::kTag && eff.device()) {
        repl = e.workingSpaceObject(eff.n);
      }
      if (!repl.isInitialized()) return;
      if (indexed) cs.setArrayItem(1, repl);
      else d.replaceKey("/ColorSpace", repl);
      ++e.stats.spaces;
      return;
    }
    if (a == ColorRule::kConvertAlternate) return;
    if (s->fam == Space::kIndexed && s->base && s->base->fam != Space::kLab) {
      int bn = s->base->n;
      int entries = s->hival + 1;
      std::string table;
      table.reserve(entries * e.destComponents());
      for (int i = 0; i < entries; ++i) {
        double in = i, out[4];
        if (!e.convertValues(*s, &in, out, a, intent)) {
          fail("indexed palette could not be converted");
          return;
        }
        for (int c = 0; c < e.destComponents(); ++c) table += static_cast<char>(std::lround(out[c] * 255));
      }
      (void)bn;
      QPDFObjectHandle ncs = QPDFObjectHandle::newArray();
      ncs.appendItem(QPDFObjectHandle::newName("/Indexed"));
      ncs.appendItem(e.destSpaceObject());
      ncs.appendItem(QPDFObjectHandle::newInteger(s->hival));
      ncs.appendItem(QPDFObjectHandle::newString(table));
      d.replaceKey("/ColorSpace", ncs);
      ++e.stats.images;
      return;
    }
    RawImage raw = decodeImage(img);
    int n = s->fam == Space::kIndexed ? (s->base ? s->base->n : 0) : s->n;
    if (!raw.ok || raw.comps != n) {
      fail("image could not be decoded (" + (raw.ok ? std::string("components") : raw.error) + ")");
      return;
    }
    SpacePtr eff = s->fam == Space::kIndexed ? s->base : s;
    size_t pixels = static_cast<size_t>(raw.width) * raw.height;
    std::string out(pixels * e.destComponents(), '\0');
    if (!e.convertPixels(*eff, reinterpret_cast<const unsigned char*>(raw.samples.data()),
                         reinterpret_cast<unsigned char*>(&out[0]), pixels, a, intent)) {
      fail("image pixels could not be converted");
      return;
    }
    bool lossy = cls == ColorRule::kImageLossy;
    std::string enc;
    const char* filter = nullptr;
    if (lossy && pixels > 4096) {
      enc = e.destComponents() == 4 ? encodeCmykJpeg(out, raw.width, raw.height, 90)
                                    : encodeJpeg(out, raw.width, raw.height, e.destComponents(), 90);
      if (!enc.empty()) filter = "/DCTDecode";
    }
    if (!filter) {
      enc = flateCompress(out, 1);
      if (!enc.empty()) filter = "/FlateDecode";
    }
    if (filter) {
      img.replaceStreamData(enc, QPDFObjectHandle::newName(filter), QPDFObjectHandle::newNull());
    } else {
      img.replaceStreamData(out, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    }
    d.replaceKey("/ColorSpace", e.destSpaceObject());
    d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    d.replaceKey("/Width", QPDFObjectHandle::newInteger(raw.width));
    d.replaceKey("/Height", QPDFObjectHandle::newInteger(raw.height));
    d.removeKey("/Decode");
    d.removeKey("/DecodeParms");
    if (d.getKey("/Mask").isArray()) d.removeKey("/Mask");
    ++e.stats.images;
  }

  static int intentOf(QPDFObjectHandle n) { return intentOfName(nameOf(n)); }

  static int intentOfName(const std::string& s) {
    if (s == "/Perceptual") return 0;
    if (s == "/RelativeColorimetric") return 1;
    if (s == "/Saturation") return 2;
    if (s == "/AbsoluteColorimetric") return 3;
    return -1;
  }

  void shading(QPDFObjectHandle sh, QPDFObjectHandle res) {
    QPDFObjectHandle d = sh.isStream() ? sh.getDict() : sh;
    if (!d.isDictionary() || !visited.enter(sh)) return;
    SpacePtr s = e.resolve(d.getKey("/ColorSpace"), res);
    if (!s || s->fam == Space::kPattern) return;
    ColorRule::Action a;
    int intent;
    if (!e.decide(ColorRule::kShading, *s, a, intent)) return;
    if (a == ColorRule::kConvertAlternate) return;
    if (a == ColorRule::kDecalibrate || a == ColorRule::kTag) {
      QPDFObjectHandle repl;
      if (a == ColorRule::kDecalibrate && (s->fam == Space::kIcc || s->fam == Space::kCalRgb || s->fam == Space::kCalGray)) {
        repl = QPDFObjectHandle::newName(s->n == 4 ? "/DeviceCMYK" : s->n == 3 ? "/DeviceRGB" : "/DeviceGray");
      } else if (a == ColorRule::kTag && s->device()) {
        repl = e.workingSpaceObject(s->n);
      }
      if (repl.isInitialized()) {
        d.replaceKey("/ColorSpace", repl);
        ++e.stats.spaces;
      }
      return;
    }
    int type = d.getKey("/ShadingType").isInteger() ? static_cast<int>(d.getKey("/ShadingType").getIntValue()) : 0;
    QPDFObjectHandle fnObj = d.getKey("/Function");
    int dn = e.destComponents();
    auto conv = [&](const double* in, double* out) { return e.convertValues(*s, in, out, a, intent); };
    if (!fnObj.isNull()) {
      std::unique_ptr<PdfFunction> fn = PdfFunction::loadArray(fnObj);
      if (!fn) {
        fail("shading function could not be read");
        return;
      }
      std::vector<double> dom;
      if (type == 1) {
        QPDFObjectHandle da = d.getKey("/Domain");
        dom = {0, 1, 0, 1};
        if (da.isArray() && da.getArrayNItems() == 4) {
          for (int i = 0; i < 4; ++i) dom[i] = numOf(da.getArrayItem(i), dom[i]);
        }
      } else if (type == 2 || type == 3) {
        QPDFObjectHandle da = d.getKey("/Domain");
        dom = {0, 1};
        if (da.isArray() && da.getArrayNItems() == 2) {
          dom[0] = numOf(da.getArrayItem(0), 0);
          dom[1] = numOf(da.getArrayItem(1), 1);
        }
      } else {
        QPDFObjectHandle dec = d.getKey("/Decode");
        dom = {0, 1};
        if (dec.isArray() && dec.getArrayNItems() >= 6) {
          int at = dec.getArrayNItems() - 2;
          dom[0] = numOf(dec.getArrayItem(at), 0);
          dom[1] = numOf(dec.getArrayItem(at + 1), 1);
        }
      }
      int ins = static_cast<int>(dom.size() / 2);
      PdfFunction* raw = fn.get();
      QPDFObjectHandle nf = sampledFunction(ctx.pdf, ins, dom, dn, [&](const double* in, double* out) {
        double mid[32];
        if (!raw->compute(in, mid)) return false;
        return conv(mid, out);
      });
      if (!nf.isInitialized()) {
        fail("shading function could not be resampled");
        return;
      }
      d.replaceKey("/Function", nf);
    } else if (type >= 4 && type <= 7 && sh.isStream()) {
      if (!meshVertices(sh, *s, conv)) {
        fail("mesh shading vertex colours could not be converted");
        return;
      }
    } else {
      return;
    }
    QPDFObjectHandle bg = d.getKey("/Background");
    if (bg.isArray() && bg.getArrayNItems() == s->n) {
      double in[32], out[4];
      for (int i = 0; i < s->n; ++i) in[i] = numOf(bg.getArrayItem(i), 0);
      if (conv(in, out)) {
        QPDFObjectHandle nb = QPDFObjectHandle::newArray();
        for (int i = 0; i < dn; ++i) nb.appendItem(QPDFObjectHandle::newReal(out[i], 4));
        d.replaceKey("/Background", nb);
      } else {
        d.removeKey("/Background");
      }
    }
    d.replaceKey("/ColorSpace", e.destSpaceObject());
    ++e.stats.shadings;
  }

  bool meshVertices(QPDFObjectHandle sh, const Space& s,
                    const std::function<bool(const double*, double*)>& conv) {
    QPDFObjectHandle d = sh.getDict();
    int type = static_cast<int>(d.getKey("/ShadingType").getIntValue());
    int bpc = static_cast<int>(numOf(d.getKey("/BitsPerCoordinate"), 0));
    int bpcomp = static_cast<int>(numOf(d.getKey("/BitsPerComponent"), 0));
    int bpf = type == 5 ? 0 : static_cast<int>(numOf(d.getKey("/BitsPerFlag"), 0));
    int perRow = type == 5 ? static_cast<int>(numOf(d.getKey("/VerticesPerRow"), 0)) : 0;
    QPDFObjectHandle dec = d.getKey("/Decode");
    int n = s.fam == Space::kIndexed ? 1 : s.n;
    if (bpc < 1 || bpc > 32 || bpcomp < 1 || bpcomp > 16 || !dec.isArray() ||
        dec.getArrayNItems() != 4 + 2 * n) {
      return false;
    }
    std::vector<double> decode;
    for (int i = 0; i < dec.getArrayNItems(); ++i) decode.push_back(numOf(dec.getArrayItem(i), 0));
    std::string data;
    try {
      auto buf = sh.getStreamData(qpdf_dl_all);
      data.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    } catch (...) {
      return false;
    }
    int dn = e.destComponents();
    double maxComp = static_cast<double>((1ULL << bpcomp) - 1);
    for (int alignEach : {type == 4 ? 1 : 0, type == 4 ? 0 : 1}) {
      BitReader r(data);
      BitWriter w;
      bool ok = true;
      bool first = true;
      while (r.more() && ok) {
        if (r.atEnd() && !first) break;
        first = false;
        unsigned long long flag = 0;
        if (bpf && !r.read(bpf, flag)) break;
        if (bpf) w.write(bpf, flag);
        int points = 1, colours = 1;
        if (type == 6 || type == 7) {
          int full = type == 6 ? 12 : 16;
          points = flag == 0 ? full : full - 4;
          colours = flag == 0 ? 4 : 2;
        }
        for (int p = 0; p < points && ok; ++p) {
          unsigned long long x, y;
          if (!r.read(bpc, x) || !r.read(bpc, y)) {
            ok = false;
            break;
          }
          w.write(bpc, x);
          w.write(bpc, y);
        }
        for (int c = 0; c < colours && ok; ++c) {
          double in[32], out[4];
          for (int k = 0; k < n; ++k) {
            unsigned long long v;
            if (!r.read(bpcomp, v)) {
              ok = false;
              break;
            }
            in[k] = decode[4 + 2 * k] + v / maxComp * (decode[5 + 2 * k] - decode[4 + 2 * k]);
          }
          if (!ok) break;
          if (!conv(in, out)) return false;
          for (int k = 0; k < dn; ++k) {
            w.write(bpcomp, static_cast<unsigned long long>(std::lround(out[k] * maxComp)));
          }
        }
        if (alignEach) {
          r.align();
          w.align();
        }
      }
      (void)perRow;
      if (!ok || !r.atEnd()) continue;
      w.align();
      QPDFObjectHandle nd = QPDFObjectHandle::newArray();
      for (int i = 0; i < 4; ++i) nd.appendItem(dec.getArrayItem(i));
      for (int k = 0; k < dn; ++k) {
        nd.appendItem(QPDFObjectHandle::newInteger(0));
        nd.appendItem(QPDFObjectHandle::newInteger(1));
      }
      sh.replaceStreamData(w.out, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
      d.replaceKey("/Decode", nd);
      return true;
    }
    return false;
  }

  class Filter : public QPDFObjectHandle::TokenFilter {
   public:
    Filter(Converter& c, QPDFObjectHandle res, unsigned forced) : c(c), res(res), forced(forced) {}

    void handleToken(QPDFTokenizer::Token const& tok) override {
      using TT = QPDFTokenizer;
      if (passthrough) {
        writeToken(tok);
        return;
      }
      TT::token_type_e t = tok.getType();
      if (t == TT::tt_inline_image) {
        flushPath();
        flushOperands();
        if (inlineIsMask) emitFor(ColorRule::kImageLossless, true, false);
        writeToken(tok);
        return;
      }
      if (t != TT::tt_word) {
        operands.push_back(tok);
        return;
      }
      std::string op = tok.getValue();
      if (op == "d1") {
        flushOperands();
        writeToken(tok);
        passthrough = true;
        return;
      }
      static const std::set<std::string> pathOps = {"m", "l", "c", "v", "y", "h", "re", "W", "W*"};
      if (pathOps.count(op)) {
        for (const auto& o : operands) path.push_back(o);
        operands.clear();
        path.push_back(tok);
        return;
      }
      static const std::set<std::string> fillOps = {"f", "F", "f*", "B", "B*", "b", "b*"};
      static const std::set<std::string> strokeOps = {"S", "s", "B", "B*", "b", "b*"};
      if (fillOps.count(op) || strokeOps.count(op) || op == "n") {
        if (op != "n") emitFor(forced ? forced : ColorRule::kVector, fillOps.count(op) > 0, strokeOps.count(op) > 0);
        flushPath();
        flushOperands();
        writeToken(tok);
        return;
      }
      flushPath();
      if (op == "q") {
        stack.push_back(cur);
      } else if (op == "Q") {
        if (!stack.empty()) {
          cur = stack.back();
          stack.pop_back();
        }
      } else if (op == "BI") {
        inlineIsMask = false;
      } else if (op == "ri") {
        cur.intent = intentOfName(lastName());
      } else if (op == "cs" || op == "CS") {
        Side& sd = op == "cs" ? cur.fill : cur.stroke;
        std::string nm = lastName();
        sd = Side();
        sd.inherited = false;
        sd.csName = nm;
        sd.space = c.e.resolve(QPDFObjectHandle::newName(nm), res);
        if (sd.space) sd.vals = initial(*sd.space);
        operands.clear();
        return;
      } else if (op == "sc" || op == "scn" || op == "SC" || op == "SCN") {
        Side& sd = (op == "sc" || op == "scn") ? cur.fill : cur.stroke;
        sd.vals = numbers();
        sd.pattern = lastIsName() ? lastName() : std::string();
        operands.clear();
        return;
      } else if (op == "g" || op == "G" || op == "rg" || op == "RG" || op == "k" || op == "K") {
        bool fill = op == "g" || op == "rg" || op == "k";
        Side& sd = fill ? cur.fill : cur.stroke;
        sd = Side();
        sd.inherited = false;
        sd.csName = op == "g" || op == "G" ? "/DeviceGray" : op == "rg" || op == "RG" ? "/DeviceRGB" : "/DeviceCMYK";
        sd.space = c.e.resolve(QPDFObjectHandle::newName(sd.csName), res);
        sd.vals = numbers();
        operands.clear();
        return;
      } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
        emitFor(forced ? forced : ColorRule::kText, true, true);
      } else if (op == "Do") {
        std::string nm = lastName();
        QPDFObjectHandle xo = res.isDictionary() ? res.getKey("/XObject") : QPDFObjectHandle();
        QPDFObjectHandle x = xo.isDictionary() ? xo.getKey(nm) : QPDFObjectHandle();
        if (x.isStream()) {
          QPDFObjectHandle xd = x.getDict();
          if (nameIs(xd.getKey("/Subtype"), "/Image")) {
            if (xd.getKey("/ImageMask").isBool() && xd.getKey("/ImageMask").getBoolValue()) {
              emitFor(ColorRule::kImageLossless, true, false);
            }
          } else {
            emitInherited();
          }
        }
      } else if (op == "ID") {
        for (const auto& o : operands) {
          if (o.getType() == TT::tt_name && (o.getValue() == "/IM" || o.getValue() == "/ImageMask")) {
            inlineIsMask = true;
          }
        }
      }
      flushOperands();
      writeToken(tok);
    }

    void handleEOF() override {
      flushPath();
      flushOperands();
    }

   private:
    struct Side {
      bool inherited = true;
      std::string csName;
      SpacePtr space;
      std::vector<double> vals;
      std::string pattern;
    };
    struct State {
      Side fill, stroke;
      std::string emittedFill, emittedStroke;
      int intent = -1;
    };

    static std::vector<double> initial(const Space& s) {
      if (s.fam == Space::kCmyk) return {0, 0, 0, 1};
      if (s.fam == Space::kSep || s.fam == Space::kDevN) return std::vector<double>(s.n, 1.0);
      if (s.fam == Space::kPattern) return {};
      if (s.fam == Space::kLab) return {0, 0, 0};
      return std::vector<double>(std::max(1, s.n), 0.0);
    }

    std::string original(const Side& sd, bool fill) const {
      std::string v;
      for (double x : sd.vals) v += fmtNum(x) + " ";
      if (sd.csName == "/DeviceGray" && sd.pattern.empty()) return v + (fill ? "g" : "G");
      if (sd.csName == "/DeviceRGB" && sd.pattern.empty()) return v + (fill ? "rg" : "RG");
      if (sd.csName == "/DeviceCMYK" && sd.pattern.empty()) return v + (fill ? "k" : "K");
      std::string s = sd.csName + (fill ? " cs " : " CS ");
      if (!v.empty() || !sd.pattern.empty()) {
        s += v + (sd.pattern.empty() ? "" : sd.pattern + " ") + (fill ? "scn" : "SCN");
      }
      return s;
    }

    std::string converted(const Side& sd, bool fill, unsigned cls) {
      if (sd.inherited) return std::string();
      if (!sd.space) return original(sd, fill);
      const Space& sp = *sd.space;
      if (c.remap) return remapped(sd, fill, cls);
      if (sp.fam == Space::kPattern) {
        if (sd.pattern.empty() || !sp.base) return original(sd, fill);
        ColorRule::Action a;
        int intent;
        if (!c.e.decide(cls, *sp.base, a, intent) || (a != ColorRule::kConvert && a != ColorRule::kInkLimit)) return original(sd, fill);
        double out[4];
        if (static_cast<int>(sd.vals.size()) < sp.base->n ||
            !c.e.convertValues(*sp.base, sd.vals.data(), out, a, intent < 0 ? cur.intent : intent)) {
          return original(sd, fill);
        }
        std::string key = patternSpaceName();
        std::string v;
        for (int i = 0; i < c.e.destComponents(); ++i) v += fmtNum(out[i]) + " ";
        ++c.e.stats.ops;
        return key + (fill ? " cs " : " CS ") + v + sd.pattern + (fill ? " scn" : " SCN");
      }
      ColorRule::Action a;
      int intent;
      if (!c.e.decide(cls, sp, a, intent)) return original(sd, fill);
      if (intent < 0) intent = cur.intent;
      if (a == ColorRule::kConvertAlternate) return original(sd, fill);
      if (a == ColorRule::kDecalibrate) {
        if (sp.fam != Space::kIcc && sp.fam != Space::kCalRgb && sp.fam != Space::kCalGray) return original(sd, fill);
        std::string v;
        for (double x : sd.vals) v += fmtNum(x) + " ";
        ++c.e.stats.ops;
        return v + (sp.n == 4 ? (fill ? "k" : "K") : sp.n == 3 ? (fill ? "rg" : "RG") : (fill ? "g" : "G"));
      }
      if (a == ColorRule::kTag) {
        if (!sp.device()) return original(sd, fill);
        std::string key = resourceName(c.e.workingSpaceObject(sp.n), sp.n == 4 ? "/KuraTagCMYK" : sp.n == 3 ? "/KuraTagRGB" : "/KuraTagGray");
        std::string v;
        for (double x : sd.vals) v += fmtNum(x) + " ";
        ++c.e.stats.ops;
        return key + (fill ? " cs " : " CS ") + v + (fill ? "scn" : "SCN");
      }
      int need = sp.fam == Space::kLab ? 3 : sp.n;
      if (static_cast<int>(sd.vals.size()) < need) return original(sd, fill);
      double out[4];
      if (!c.e.convertValues(sp, sd.vals.data(), out, a, intent)) {
        c.fail("colour value could not be converted");
        return original(sd, fill);
      }
      ++c.e.stats.ops;
      std::string v;
      int dn = c.e.destComponents();
      for (int i = 0; i < dn; ++i) v += fmtNum(out[i]) + " ";
      if (c.e.job.embed == ColorJob::kEmbedSource) {
        std::string key = resourceName(c.e.destSpaceObject(), "/KuraDest");
        return key + (fill ? " cs " : " CS ") + v + (fill ? "scn" : "SCN");
      }
      return v + (dn == 4 ? (fill ? "k" : "K") : dn == 3 ? (fill ? "rg" : "RG") : (fill ? "g" : "G"));
    }

    std::string remapped(const Side& sd, bool fill, unsigned cls) {
      const Space& sp = *sd.space;
      if (sp.fam == Space::kPattern) return original(sd, fill);
      ColorValue in, out;
      in.family = sp.fam == Space::kGray ? "/DeviceGray" : sp.fam == Space::kRgb ? "/DeviceRGB"
                  : sp.fam == Space::kCmyk ? "/DeviceCMYK" : sp.fam == Space::kSep ? "/Separation"
                  : sp.fam == Space::kDevN ? "/DeviceN" : sp.fam == Space::kLab ? "/Lab"
                  : sp.fam == Space::kIndexed ? "/Indexed" : sp.fam == Space::kIcc ? "/ICCBased"
                  : sp.fam == Space::kCalRgb ? "/CalRGB" : "/CalGray";
      in.colorants = sp.colorants;
      in.values = sd.vals;
      in.space = sp.obj;
      if (!(*c.remap)(in, cls, out)) return original(sd, fill);
      ++c.e.stats.ops;
      std::string v;
      for (double x : out.values) v += fmtNum(x) + " ";
      if (out.space.isInitialized() && !out.space.isNull()) {
        std::string key = "/KuraMap" + std::to_string(fnv1a32(out.space.unparse()) % 1000000);
        return resourceName(out.space, key) + (fill ? " cs " : " CS ") + v + (fill ? "scn" : "SCN");
      }
      if (out.family == "/DeviceCMYK") return v + (fill ? "k" : "K");
      if (out.family == "/DeviceRGB") return v + (fill ? "rg" : "RG");
      if (out.family == "/DeviceGray") return v + (fill ? "g" : "G");
      return original(sd, fill);
    }

    std::string resourceName(QPDFObjectHandle space, const std::string& want) {
      QPDFObjectHandle csd = res.getKey("/ColorSpace");
      if (!csd.isDictionary()) {
        csd = QPDFObjectHandle::newDictionary();
        res.replaceKey("/ColorSpace", csd);
      }
      if (!csd.hasKey(want)) csd.replaceKey(want, space);
      return want;
    }

    std::string patternSpaceName() {
      QPDFObjectHandle arr = QPDFObjectHandle::newArray();
      arr.appendItem(QPDFObjectHandle::newName("/Pattern"));
      arr.appendItem(c.e.destSpaceObject());
      return resourceName(arr, "/KuraPattern");
    }

    void emitFor(unsigned cls, bool fill, bool stroke) {
      if (!res.isDictionary()) return;
      std::string out;
      if (fill && !cur.fill.inherited) {
        std::string want = converted(cur.fill, true, cls);
        if (!want.empty() && want != cur.emittedFill) {
          out += want + "\n";
          cur.emittedFill = want;
        }
      }
      if (stroke && !cur.stroke.inherited) {
        std::string want = converted(cur.stroke, false, cls);
        if (!want.empty() && want != cur.emittedStroke) {
          out += want + "\n";
          cur.emittedStroke = want;
        }
      }
      if (!out.empty()) write("\n" + out);
    }

    void emitInherited() {
      std::string out;
      for (int side = 0; side < 2; ++side) {
        Side& sd = side == 0 ? cur.fill : cur.stroke;
        std::string& em = side == 0 ? cur.emittedFill : cur.emittedStroke;
        if (sd.inherited || !sd.space) continue;
        bool all = true;
        for (unsigned b : {1u, 2u}) {
          ColorRule::Action a;
          int i;
          if (!c.e.decide(b, *sd.space, a, i)) all = false;
        }
        std::string want = all ? converted(sd, side == 0, ColorRule::kVector) : original(sd, side == 0);
        if (!want.empty() && want != em) {
          out += want + "\n";
          em = want;
        }
      }
      if (!out.empty()) write("\n" + out);
    }

    void flushPath() {
      for (const auto& t : path) writeToken(t);
      path.clear();
    }

    void flushOperands() {
      for (const auto& t : operands) writeToken(t);
      operands.clear();
    }

    std::vector<double> numbers() const {
      std::vector<double> v;
      for (const auto& t : operands) {
        if (t.getType() == QPDFTokenizer::tt_integer || t.getType() == QPDFTokenizer::tt_real) {
          v.push_back(std::strtod(t.getValue().c_str(), nullptr));
        }
      }
      return v;
    }

    std::string lastName() const {
      for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
        if (it->getType() == QPDFTokenizer::tt_name) return it->getValue();
      }
      return std::string();
    }

    bool lastIsName() const {
      for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
        auto t = it->getType();
        if (t == QPDFTokenizer::tt_space || t == QPDFTokenizer::tt_comment) continue;
        return t == QPDFTokenizer::tt_name;
      }
      return false;
    }

    Converter& c;
    QPDFObjectHandle res;
    unsigned forced;
    bool passthrough = false;
    bool inlineIsMask = false;
    std::vector<QPDFTokenizer::Token> operands;
    std::vector<QPDFTokenizer::Token> path;
    State cur;
    std::vector<State> stack;
  };

  void rewrite(QPDFObjectHandle holder, QPDFObjectHandle res, unsigned forced, bool isPage) {
    if (!holder.isStream() && !isPage) return;
    if (holder.isStream() && !visitedStreams.insert(holder.getObjGen()).second && holder.isIndirect()) return;
    try {
      Filter f(*this, res, forced);
      Pl_Buffer buf("colour conversion");
      size_t sourceLen = 0;
      if (holder.isStream()) {
        auto src = holder.getStreamData(qpdf_dl_all);
        sourceLen = src->getSize();
        holder.filterAsContents(&f, &buf);
      } else {
        QPDFPageObjectHelper ph(holder);
        ph.filterContents(&f, &buf);
      }
      auto data = buf.getBufferSharedPointer();
      std::string out(reinterpret_cast<const char*>(data->getBuffer()), data->getSize());
      if (holder.isStream()) {
        if (out.empty() && sourceLen > 0) {
          fail("content stream rewrite produced no output");
          return;
        }
        holder.replaceStreamData(out, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
      } else {
        holder.replaceKey("/Contents", ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, out)));
      }
    } catch (std::exception const& ex) {
      fail(std::string("content stream could not be rewritten: ") + ex.what());
    } catch (...) {
      fail("content stream could not be rewritten");
    }
  }

  Ctx& ctx;
  Engine& e;
  const ColorRemap* remap;
  Visited visited;
  std::set<QPDFObjGen> visitedStreams;
  std::vector<QPDFObjectHandle> pendingSpaces;
};
}

bool resolveIccByName(const Options& opt, const std::string& name, std::string& bytes) {
  if (name.empty()) return false;
  if (opt.loadIcc && opt.loadIcc(name, bytes) && bytes.size() >= 132) return true;
  bytes = builtinByName(name);
  return !bytes.empty();
}

std::string iccDescription(const std::string& bytes) {
  if (bytes.size() < 132) return std::string();
  cmsHPROFILE p = cmsOpenProfileFromMem(bytes.data(), static_cast<cmsUInt32Number>(bytes.size()));
  if (!p) return std::string();
  char buf[512] = {0};
  cmsGetProfileInfoASCII(p, cmsInfoDescription, "en", "US", buf, sizeof(buf));
  cmsCloseProfile(p);
  return buf;
}

bool configureColorConversion(Options& opt, const std::string& target, const std::string& targetIcc,
                              int intent, bool keepSpots, unsigned objects) {
  using R = ColorRule;
  ColorJob& j = opt.colorJob;
  j.enabled = true;
  std::string t;
  for (char c : target) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (!targetIcc.empty()) {
    j.destProfile = targetIcc;
  } else if (t == "device-link") {
    j.embed = ColorJob::kEmbedNone;
  } else if (t == "cmyk") {
    j.destProfile = opt.destProfile;
  } else if (t == "gray" || t == "grey") {
    j.destName = "Dot Gain 15%";
    j.embed = ColorJob::kEmbedNone;
  } else if (t == "rgb" || t == "srgb") {
    j.destName = "sRGB IEC61966-2.1";
  } else if (!target.empty()) {
    j.destName = target;
  } else {
    return false;
  }
  const std::string sig = j.destProfile.size() >= 20 ? j.destProfile.substr(16, 4) : std::string();
  if (sig == "CMYK") opt.destProfile = j.destProfile;
  bool gray = (targetIcc.empty() && (t == "gray" || t == "grey")) || sig == "GRAY";
  bool rgb = (targetIcc.empty() && (t == "rgb" || t == "srgb")) || sig == "RGB ";
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

bool loadIccFromFolders(const std::vector<std::string>& folders, const std::string& name,
                        std::string& bytes) {
  const std::string want = lowerNoSpace(name);
  if (want.empty()) return false;
  std::string byDescription;
  for (const std::string& folder : folders) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) continue;
    for (const auto& e : std::filesystem::recursive_directory_iterator(
             folder, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (ec) break;
      if (!e.is_regular_file(ec)) continue;
      std::string ext = lowerNoSpace(e.path().extension().string());
      if (ext != ".icc" && ext != ".icm") continue;
      bool stemMatch = lowerNoSpace(e.path().stem().string()) == want ||
                       lowerNoSpace(e.path().filename().string()) == want;
      if (!stemMatch && !byDescription.empty()) continue;
      std::ifstream f(e.path(), std::ios::binary);
      if (!f) continue;
      std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (b.size() < 132) continue;
      if (stemMatch) {
        bytes = b;
        return true;
      }
      if (lowerNoSpace(iccDescription(b)) == want) byDescription = b;
    }
  }
  if (byDescription.empty()) return false;
  bytes = byDescription;
  return true;
}

QPDFObjectHandle sampledColorFunction(QPDF& pdf, int inputs, const std::vector<double>& domain, int outputs,
                                      const std::function<bool(const double*, double*)>& fn) {
  return sampledFunction(pdf, inputs, domain, outputs, fn);
}

bool colorValueToLab(const ColorValue& c, double lab[3]) {
  struct Xf {
    cmsHTRANSFORM gray = nullptr, rgb = nullptr, cmyk = nullptr;
    Xf() {
      cmsHPROFILE labP = cmsCreateLab4Profile(nullptr);
      cmsHPROFILE s = cmsOpenProfileFromMem(kSrgbIcc, kSrgbIccLen);
      cmsHPROFILE k = cmsOpenProfileFromMem(kCmykIcc, kCmykIccLen);
      cmsHPROFILE g = synthGray(0.15, 0);
      if (labP && s) rgb = cmsCreateTransform(s, TYPE_RGB_DBL, labP, TYPE_Lab_DBL, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
      if (labP && k) cmyk = cmsCreateTransform(k, TYPE_CMYK_DBL, labP, TYPE_Lab_DBL, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
      if (labP && g) gray = cmsCreateTransform(g, TYPE_GRAY_DBL, labP, TYPE_Lab_DBL, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
      for (cmsHPROFILE p : {labP, s, k, g}) {
        if (p) cmsCloseProfile(p);
      }
    }
  };
  static const Xf xf;
  double in[4] = {0, 0, 0, 0};
  for (size_t i = 0; i < c.values.size() && i < 4; ++i) in[i] = std::min(std::max(c.values[i], 0.0), 1.0);
  if (c.family == "/Lab") {
    for (int i = 0; i < 3 && i < static_cast<int>(c.values.size()); ++i) lab[i] = c.values[i];
    return c.values.size() >= 3;
  }
  if (c.family == "/DeviceRGB" && xf.rgb && c.values.size() >= 3) {
    cmsDoTransform(xf.rgb, in, lab, 1);
    return true;
  }
  if (c.family == "/DeviceCMYK" && xf.cmyk && c.values.size() >= 4) {
    for (double& v : in) v *= 100;
    cmsDoTransform(xf.cmyk, in, lab, 1);
    return true;
  }
  if (c.family == "/DeviceGray" && xf.gray && !c.values.empty()) {
    cmsDoTransform(xf.gray, in, lab, 1);
    return true;
  }
  if (c.family == "/CalRGB" || c.family == "/CalGray") {
    ColorValue d = c;
    d.family = c.family == "/CalRGB" ? "/DeviceRGB" : "/DeviceGray";
    return colorValueToLab(d, lab);
  }
  if (c.family == "/ICCBased" && c.space.isArray() && c.space.getArrayNItems() >= 2 &&
      c.space.getArrayItem(1).isStream()) {
    std::string data;
    try {
      auto b = c.space.getArrayItem(1).getStreamData(qpdf_dl_all);
      data.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
    } catch (...) {
      return false;
    }
    cmsHPROFILE p = cmsOpenProfileFromMem(data.data(), static_cast<cmsUInt32Number>(data.size()));
    if (!p) return false;
    cmsColorSpaceSignature sig = cmsGetColorSpace(p);
    int n = static_cast<int>(cmsChannelsOf(sig));
    cmsHPROFILE labP = cmsCreateLab4Profile(nullptr);
    cmsUInt32Number fmt = n == 1 ? TYPE_GRAY_DBL : n == 3 ? (sig == cmsSigLabData ? TYPE_Lab_DBL : TYPE_RGB_DBL)
                                                          : n == 4 ? TYPE_CMYK_DBL : 0;
    cmsHTRANSFORM t = fmt && labP ? cmsCreateTransform(p, fmt, labP, TYPE_Lab_DBL, INTENT_RELATIVE_COLORIMETRIC,
                                                       cmsFLAGS_NOCACHE)
                                  : nullptr;
    if (labP) cmsCloseProfile(labP);
    cmsCloseProfile(p);
    if (!t || static_cast<int>(c.values.size()) < n) {
      if (t) cmsDeleteTransform(t);
      return false;
    }
    if (n == 4) {
      for (double& v : in) v *= 100;
    }
    if (sig == cmsSigLabData) {
      for (int i = 0; i < 3; ++i) in[i] = c.values[i];
    }
    cmsDoTransform(t, in, lab, 1);
    cmsDeleteTransform(t);
    return true;
  }
  if ((c.family == "/Separation" || c.family == "/DeviceN") && c.space.isArray() && c.space.getArrayNItems() >= 4) {
    auto fn = PdfFunction::loadArray(c.space.getArrayItem(3));
    double tin[32] = {0}, tout[32] = {0};
    for (size_t i = 0; i < c.values.size() && i < 32; ++i) tin[i] = c.values[i];
    if (!fn || !fn->compute(tin, tout)) return false;
    QPDFObjectHandle alt = c.space.getArrayItem(2);
    ColorValue a;
    a.family = alt.isName() ? alt.getName() : alt.isArray() && alt.getArrayNItems() ? nameOf(alt.getArrayItem(0)) : "";
    if (a.family == "/ICCBased") {
      QPDFObjectHandle st = alt.getArrayItem(1);
      long long n = st.isStream() ? static_cast<long long>(numOf(st.getDict().getKey("/N"), 0)) : 0;
      a.family = n == 4 ? "/DeviceCMYK" : n == 3 ? "/DeviceRGB" : "/DeviceGray";
    }
    a.values.assign(tout, tout + fn->outputs());
    return colorValueToLab(a, lab);
  }
  return false;
}

bool colorValueToProcess(const ColorValue& c, int n, std::vector<double>& out) {
  struct Xf {
    cmsHTRANSFORM gray = nullptr, rgb = nullptr, cmyk = nullptr;
    Xf() {
      cmsHPROFILE labP = cmsCreateLab4Profile(nullptr);
      cmsHPROFILE s = cmsOpenProfileFromMem(kSrgbIcc, kSrgbIccLen);
      cmsHPROFILE k = cmsOpenProfileFromMem(kCmykIcc, kCmykIccLen);
      cmsHPROFILE g = synthGray(0.15, 0);
      if (labP && s) rgb = cmsCreateTransform(labP, TYPE_Lab_DBL, s, TYPE_RGB_DBL, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
      if (labP && k) cmyk = cmsCreateTransform(labP, TYPE_Lab_DBL, k, TYPE_CMYK_DBL, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
      if (labP && g) gray = cmsCreateTransform(labP, TYPE_Lab_DBL, g, TYPE_GRAY_DBL, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
      for (cmsHPROFILE p : {labP, s, k, g}) {
        if (p) cmsCloseProfile(p);
      }
    }
  };
  static const Xf xf;
  double lab[3] = {0, 0, 0};
  if (!colorValueToLab(c, lab)) return false;
  double o[4] = {0, 0, 0, 0};
  cmsHTRANSFORM t = n == 1 ? xf.gray : n == 3 ? xf.rgb : n == 4 ? xf.cmyk : nullptr;
  if (!t) return false;
  cmsDoTransform(t, lab, o, 1);
  out.assign(o, o + n);
  for (double& v : out) v = std::min(1.0, std::max(0.0, n == 4 ? v / 100 : v));
  return true;
}

double deltaE2000(const double* a, const double* b) {
  const double kPi = 3.14159265358979323846;
  double L1 = a[0], a1 = a[1], b1 = a[2], L2 = b[0], a2 = b[1], b2 = b[2];
  double C1 = std::hypot(a1, b1), C2 = std::hypot(a2, b2), Cm = (C1 + C2) / 2;
  double G = 0.5 * (1 - std::sqrt(std::pow(Cm, 7) / (std::pow(Cm, 7) + std::pow(25.0, 7))));
  double a1p = (1 + G) * a1, a2p = (1 + G) * a2;
  double C1p = std::hypot(a1p, b1), C2p = std::hypot(a2p, b2);
  auto hue = [&](double x, double y) {
    if (x == 0 && y == 0) return 0.0;
    double h = std::atan2(y, x) * 180 / kPi;
    return h < 0 ? h + 360 : h;
  };
  double h1p = hue(a1p, b1), h2p = hue(a2p, b2);
  double dLp = L2 - L1, dCp = C2p - C1p;
  double dhp = 0;
  if (C1p * C2p != 0) {
    dhp = h2p - h1p;
    if (dhp > 180) dhp -= 360;
    else if (dhp < -180) dhp += 360;
  }
  double dHp = 2 * std::sqrt(C1p * C2p) * std::sin(dhp * kPi / 360);
  double Lm = (L1 + L2) / 2, Cmp = (C1p + C2p) / 2;
  double hm = h1p + h2p;
  if (C1p * C2p != 0) {
    if (std::fabs(h1p - h2p) > 180) hm = h1p + h2p < 360 ? (h1p + h2p + 360) / 2 : (h1p + h2p - 360) / 2;
    else hm = (h1p + h2p) / 2;
  }
  double T = 1 - 0.17 * std::cos((hm - 30) * kPi / 180) + 0.24 * std::cos(2 * hm * kPi / 180) +
             0.32 * std::cos((3 * hm + 6) * kPi / 180) - 0.20 * std::cos((4 * hm - 63) * kPi / 180);
  double dTheta = 30 * std::exp(-std::pow((hm - 275) / 25, 2));
  double Rc = 2 * std::sqrt(std::pow(Cmp, 7) / (std::pow(Cmp, 7) + std::pow(25.0, 7)));
  double Sl = 1 + 0.015 * std::pow(Lm - 50, 2) / std::sqrt(20 + std::pow(Lm - 50, 2));
  double Sc = 1 + 0.045 * Cmp, Sh = 1 + 0.015 * Cmp * T;
  double Rt = -std::sin(2 * dTheta * kPi / 180) * Rc;
  return std::sqrt(std::pow(dLp / Sl, 2) + std::pow(dCp / Sc, 2) + std::pow(dHp / Sh, 2) + Rt * (dCp / Sc) * (dHp / Sh));
}

int remapContentColors(Ctx& ctx, const ColorRemap& fn) {
  static const ColorJob plain = [] {
    ColorJob j;
    j.enabled = true;
    return j;
  }();
  Engine e(ctx, plain);
  std::string err;
  if (!e.init(err)) return 0;
  QPDFPageDocumentHelper dh(ctx.pdf);
  Converter conv(ctx, e, &fn);
  int pageNo = 0;
  for (auto& ph : dh.getAllPages()) {
    PageScope scope(ctx, ++pageNo);
    conv.page(ph);
  }
  return e.stats.ops;
}

void prepareColorJob(Options& opt, std::vector<Issue>& notes) {
  ColorJob& j = opt.colorJob;
  if (j.rules.empty()) j.rules.push_back(ColorRule());
  if (j.destProfile.empty() && !j.destName.empty() && j.deviceLink.empty() &&
      !resolveIccByName(opt, j.destName, j.destProfile)) {
    notes.push_back({"ICC_PROFILE_MISSING",
                     "colour profile \"" + j.destName +
                         "\" was not found in the profile folders; check the name, or pass the profile file "
                         "or its folder with --icc-folder",
                     false});
    j.destName.clear();
  }
  if (j.destProfile.empty() && j.deviceLink.empty() && !opt.destProfile.empty()) {
    j.destProfile = opt.destProfile;
  }
  if (j.destProfile.empty() && j.deviceLink.empty()) {
    j.destProfile.assign(reinterpret_cast<const char*>(kCmykIcc), kCmykIccLen);
  }
  for (const std::string& n : {j.rgbName, j.cmykName, j.grayName}) {
    std::string b;
    if (!n.empty() && !resolveIccByName(opt, n, b)) {
      notes.push_back({"ICC_PROFILE_MISSING",
                       "working colour profile \"" + n +
                           "\" was not found; using the built-in profile of the same colour model",
                       false});
    }
  }
  std::string model = j.destProfile.size() >= 20 ? j.destProfile.substr(16, 4) : std::string();
  if ((j.inkLimit > 0 || !j.toneCurves.empty()) && model == "CMYK") {
    for (ColorRule& r : j.rules) {
      if ((r.sources & (ColorRule::kUncCmyk | ColorRule::kCalCmyk)) &&
          (r.action == ColorRule::kNone || r.action == ColorRule::kDecalibrate)) {
        r.action = ColorRule::kInkLimit;
      }
    }
  }
  bool intentFits = model == "CMYK" ||
                    (model == "RGB " && levelFamily(opt.level) != Family::PDFX &&
                     levelFamily(opt.level) != Family::PDFVT);
  if (j.embed == ColorJob::kEmbedOutputIntent && intentFits) {
    opt.destProfile = j.destProfile;
    if (opt.outputConditionInfo.empty()) {
      std::string desc = iccDescription(j.destProfile);
      opt.outputConditionInfo = desc.empty() ? j.destName : desc;
    }
  }
}

ColorJob defaultX1aJob(Ctx& ctx) {
  ColorJob j;
  j.enabled = true;
  j.embed = ColorJob::kEmbedNone;
  auto cmyk = [](const std::string& b) { return b.size() >= 132 && b.substr(16, 4) == "CMYK"; };
  if (cmyk(ctx.opt.destProfile)) {
    j.destProfile = ctx.opt.destProfile;
  } else {
    QPDFObjectHandle ois = ctx.pdf.getRoot().getKey("/OutputIntents");
    for (int i = 0; ois.isArray() && i < ois.getArrayNItems() && j.destProfile.empty(); ++i) {
      QPDFObjectHandle oi = ois.getArrayItem(i);
      if (!oi.isDictionary() || nameOf(oi.getKey("/S")) != "/GTS_PDFX") continue;
      QPDFObjectHandle p = oi.getKey("/DestOutputProfile");
      if (!p.isStream()) continue;
      try {
        auto buf = p.getStreamData(qpdf_dl_all);
        std::string b(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
        if (cmyk(b)) j.destProfile = b;
      } catch (...) {
      }
    }
  }
  using R = ColorRule;
  auto add = [&](unsigned sources, R::Action a) {
    R r;
    r.sources = sources;
    r.action = a;
    j.rules.push_back(r);
  };
  add(R::kUncCmyk | R::kUncGray, R::kNone);
  add(R::kCalCmyk, R::kDecalibrate);
  add(R::kUncRgb | R::kCalRgb | R::kCalGray | R::kLab, R::kConvert);
  add(R::kSpot | R::kRegistration, R::kConvertAlternate);
  return j;
}

void passColorConvert(Ctx& ctx) {
  if (ctx.opt.verifyOnly) return;
  ColorJob x1a;
  if (!ctx.opt.colorJob.enabled && ctx.x1a()) x1a = defaultX1aJob(ctx);
  const ColorJob& job = ctx.opt.colorJob.enabled ? ctx.opt.colorJob : x1a;
  if (!job.enabled) return;
  Engine e(ctx, job);
  std::string err;
  if (!e.init(err)) {
    ctx.issue("COLOR_CONVERSION_SKIPPED", "colour conversion not run: " + err, false);
    return;
  }
  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
  externalizeInlineImagesOnce(pages, 0);
  Converter conv(ctx, e);
  int pageNo = 0;
  for (auto& ph : pages) {
    PageScope scope(ctx, ++pageNo);
    conv.page(ph);
  }
  conv.retireSpaces();
  if (job.embed == ColorJob::kEmbedOutputIntent) {
    QPDFObjectHandle root = ctx.pdf.getRoot();
    if (root.hasKey("/OutputIntents")) root.removeKey("/OutputIntents");
  }
  const Stats& s = e.stats;
  std::string target = !job.deviceLink.empty() ? "the DeviceLink profile"
                       : job.destName.empty() ? "the destination profile"
                                              : "\"" + job.destName + "\"";
  if (s.ops || s.images || s.shadings || s.spaces || s.alternates || s.groups) {
    ctx.issue("COLOR_CONVERTED",
              "converted colour to " + target + ": " + std::to_string(s.ops) + " colour value(s), " +
                  std::to_string(s.images) + " image(s), " + std::to_string(s.shadings) +
                  " shading(s), " + std::to_string(s.alternates) + " spot alternate(s), " +
                  std::to_string(s.spaces) + " colour space(s), " + std::to_string(s.groups) +
                  " blending space(s)",
              true);
  }
  if (s.failures) {
    std::string why;
    for (const std::string& r : s.failReasons) why += (why.empty() ? "" : "; ") + r;
    ctx.issue("COLOR_CONVERSION_INCOMPLETE",
              std::to_string(s.failures) + " object(s) kept their original colour (" + why + ")",
              false);
  }
}

std::string builtinIccProfile(int components) {
  if (components == 4) return std::string(reinterpret_cast<const char*>(kCmykIcc), kCmykIccLen);
  if (components == 3) return std::string(reinterpret_cast<const char*>(kSrgbIcc), kSrgbIccLen);
  if (components == 1) return builtinByName("Dot Gain 15%");
  return std::string();
}
}
