#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "passes.hh"
#include "util.hh"

namespace pdfa {
namespace {
constexpr double kHairlinePt = 0.125;
constexpr double kRichBlackK = 0.9;
constexpr double kRichBlackInk = 0.05;
constexpr double kContoneMinPpi = 250.0;
constexpr double kContoneMaxPpi = 450.0;
constexpr double kBitonalMinPpi = 550.0;
constexpr double kBitonalMaxPpi = 3600.0;
constexpr double kSmallTextPt = 4.0;

struct Mat {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

Mat mul(const Mat& m, const Mat& n) {
  Mat r;
  r.a = m.a * n.a + m.b * n.c;
  r.b = m.a * n.b + m.b * n.d;
  r.c = m.c * n.a + m.d * n.c;
  r.d = m.c * n.b + m.d * n.d;
  r.e = m.e * n.a + m.f * n.c + n.e;
  r.f = m.e * n.b + m.f * n.d + n.f;
  return r;
}

struct Gs {
  Mat ctm;
  double lineWidth = 1.0;
  int renderMode = 0;
  double fill[4] = {0, 0, 0, 0};
  double stroke[4] = {0, 0, 0, 0};
  bool fillCmyk = false;
  bool strokeCmyk = false;
  double fontSize = 0;
  double tmScale = 1.0;
};

struct Tally {
  long long hairlines = 0;
  long long richBlack = 0;
  long long invisibleText = 0;
  long long lowRes = 0;
  long long highRes = 0;
  double minPpi = 0;
  double maxPpi = 0;
  std::set<int> hairlinePages, richPages, invisPages, lowPages, highPages;
  long long smallText = 0;
  double minTextPt = 0;
  std::set<int> smallTextPages;
  long long transparency = 0;
  std::set<int> transparencyPages;
  long long overprint = 0;
  std::set<int> overprintPages;
  long long rgbOps = 0, cmykOps = 0, grayOps = 0;
  std::set<std::string> colorants;
  std::map<std::string, long long> imageFilters;
  long long imageMasks = 0;
  long long deepImages = 0;
  std::set<QPDFObjGen> usedFonts;
  std::vector<QPDFObjectHandle> usedFontDicts;
};

bool richBlack(const double c[4]) {
  return c[3] > kRichBlackK &&
         (c[0] > kRichBlackInk || c[1] > kRichBlackInk || c[2] > kRichBlackInk);
}

double strokeScale(const Mat& m) {
  return (std::hypot(m.a, m.b) + std::hypot(m.c, m.d)) / 2.0;
}

void classifySpace(QPDFObjectHandle cs, Tally& t, int depth = 0) {
  if (depth > 4) return;
  if (cs.isName()) {
    std::string n = cs.getName();
    if (n == "/DeviceRGB" || n == "/CalRGB") ++t.rgbOps;
    else if (n == "/DeviceCMYK") ++t.cmykOps;
    else if (n == "/DeviceGray" || n == "/CalGray") ++t.grayOps;
    return;
  }
  if (!cs.isArray() || cs.getArrayNItems() < 1) return;
  std::string fam = nameOf(cs.getArrayItem(0));
  if (fam == "/ICCBased" && cs.getArrayNItems() >= 2 && cs.getArrayItem(1).isStream()) {
    QPDFObjectHandle n = cs.getArrayItem(1).getDict().getKey("/N");
    long long comps = n.isInteger() ? n.getIntValue() : 0;
    if (comps == 3) ++t.rgbOps;
    else if (comps == 4) ++t.cmykOps;
    else if (comps == 1) ++t.grayOps;
  } else if (fam == "/Separation" && cs.getArrayNItems() >= 2) {
    std::string col = nameOf(cs.getArrayItem(1));
    if (col.size() > 1 && col != "/All" && col != "/None") t.colorants.insert(col.substr(1));
  } else if (fam == "/DeviceN" && cs.getArrayNItems() >= 2 && cs.getArrayItem(1).isArray()) {
    QPDFObjectHandle arr = cs.getArrayItem(1);
    for (int i = 0; i < arr.getArrayNItems(); ++i) {
      std::string col = nameOf(arr.getArrayItem(i));
      if (col.size() > 1 && col != "/None") t.colorants.insert(col.substr(1));
    }
  } else if (fam == "/Indexed" || fam == "/I") {
    if (cs.getArrayNItems() >= 2) classifySpace(cs.getArrayItem(1), t, depth + 1);
  } else if (fam == "/CalRGB" || fam == "/Lab") {
    ++t.rgbOps;
  } else if (fam == "/CalGray") {
    ++t.grayOps;
  }
}

struct Scanner : QPDFObjectHandle::ParserCallbacks {
  Gs gs;
  std::vector<Gs> stack;
  std::vector<double> nums;
  std::string lastName;
  QPDFObjectHandle res;
  Tally& tally;
  int page;
  std::vector<std::pair<std::string, Gs>> draws;

  Scanner(QPDFObjectHandle resources, Tally& t, int pageNum, const Gs& initial)
      : gs(initial), res(resources), tally(t), page(pageNum) {}

  void markHairline() {
    double eff = gs.lineWidth * strokeScale(gs.ctm);
    if (eff < kHairlinePt) {
      ++tally.hairlines;
      tally.hairlinePages.insert(page);
    }
  }

  void markRich(const double c[4], bool isCmyk) {
    if (isCmyk && richBlack(c)) {
      ++tally.richBlack;
      tally.richPages.insert(page);
    }
  }

  void applyExtGState() {
    if (lastName.empty() || !res.isDictionary()) return;
    QPDFObjectHandle egs = res.getKey("/ExtGState");
    if (!egs.isDictionary()) return;
    QPDFObjectHandle g = egs.getKey(lastName);
    if (!g.isDictionary()) return;
    if (g.getKey("/LW").isNumber()) gs.lineWidth = g.getKey("/LW").getNumericValue();
    bool transparent = false;
    if (g.getKey("/CA").isNumber() && g.getKey("/CA").getNumericValue() < 1.0) {
      transparent = true;
    }
    if (g.getKey("/ca").isNumber() && g.getKey("/ca").getNumericValue() < 1.0) {
      transparent = true;
    }
    QPDFObjectHandle sm = g.getKey("/SMask");
    if (!sm.isNull() && !nameIs(sm, "/None")) transparent = true;
    QPDFObjectHandle bm = g.getKey("/BM");
    std::string bmName = nameOf(bm);
    if (bm.isArray() && bm.getArrayNItems() > 0) bmName = nameOf(bm.getArrayItem(0));
    if (!bmName.empty() && bmName != "/Normal" && bmName != "/Compatible") {
      transparent = true;
    }
    if (transparent) {
      ++tally.transparency;
      tally.transparencyPages.insert(page);
    }
    bool op = false;
    if (g.getKey("/OP").isBool() && g.getKey("/OP").getBoolValue()) op = true;
    if (g.getKey("/op").isBool() && g.getKey("/op").getBoolValue()) op = true;
    if (g.getKey("/OPM").isInteger() && g.getKey("/OPM").getIntValue() == 1) op = true;
    if (op) {
      ++tally.overprint;
      tally.overprintPages.insert(page);
    }
  }

  void resolveColorSpace(const std::string& name) {
    static const std::set<std::string> kDevice = {"/DeviceRGB", "/DeviceCMYK",
                                                  "/DeviceGray", "/Pattern"};
    if (name.empty()) return;
    if (kDevice.count(name)) {
      classifySpace(QPDFObjectHandle::newName(name), tally);
      return;
    }
    if (!res.isDictionary()) return;
    QPDFObjectHandle csd = res.getKey("/ColorSpace");
    if (csd.isDictionary() && !csd.getKey(name).isNull()) {
      classifySpace(csd.getKey(name), tally);
    }
  }

  void resolveFont(const std::string& name) {
    if (name.empty() || !res.isDictionary()) return;
    QPDFObjectHandle fd = res.getKey("/Font");
    if (!fd.isDictionary()) return;
    QPDFObjectHandle fnt = fd.getKey(name);
    if (!fnt.isDictionary()) return;
    if (fnt.isIndirect()) {
      if (!tally.usedFonts.insert(fnt.getObjGen()).second) return;
    }
    tally.usedFontDicts.push_back(fnt);
  }

  void handleObject(QPDFObjectHandle obj, size_t, size_t) override {
    if (!obj.isOperator()) {
      if (obj.isName()) {
        lastName = obj.getName();
      } else if (obj.isNumber()) {
        nums.push_back(obj.getNumericValue());
      }
      return;
    }
    std::string op = obj.getOperatorValue();
    if (op == "q") {
      stack.push_back(gs);
    } else if (op == "Q") {
      if (!stack.empty()) {
        gs = stack.back();
        stack.pop_back();
      }
    } else if (op == "cm" && nums.size() >= 6) {
      size_t n = nums.size();
      Mat m;
      m.a = nums[n - 6]; m.b = nums[n - 5]; m.c = nums[n - 4];
      m.d = nums[n - 3]; m.e = nums[n - 2]; m.f = nums[n - 1];
      gs.ctm = mul(m, gs.ctm);
    } else if (op == "w" && !nums.empty()) {
      gs.lineWidth = nums.back();
    } else if (op == "gs") {
      applyExtGState();
    } else if (op == "Tr" && !nums.empty()) {
      gs.renderMode = static_cast<int>(nums.back());
    } else if (op == "Tf" && !nums.empty()) {
      gs.fontSize = nums.back();
      resolveFont(lastName);
    } else if (op == "BT") {
      gs.tmScale = 1.0;
    } else if (op == "Tm" && nums.size() >= 6) {
      size_t n = nums.size();
      gs.tmScale = (std::hypot(nums[n - 6], nums[n - 5]) +
                    std::hypot(nums[n - 4], nums[n - 3])) /
                   2.0;
    } else if (op == "k" && nums.size() >= 4) {
      size_t n = nums.size();
      gs.fill[0] = nums[n - 4]; gs.fill[1] = nums[n - 3];
      gs.fill[2] = nums[n - 2]; gs.fill[3] = nums[n - 1];
      gs.fillCmyk = true;
      ++tally.cmykOps;
    } else if (op == "K" && nums.size() >= 4) {
      size_t n = nums.size();
      gs.stroke[0] = nums[n - 4]; gs.stroke[1] = nums[n - 3];
      gs.stroke[2] = nums[n - 2]; gs.stroke[3] = nums[n - 1];
      gs.strokeCmyk = true;
      ++tally.cmykOps;
    } else if (op == "rg" || op == "RG") {
      if (op == "rg") gs.fillCmyk = false;
      else gs.strokeCmyk = false;
      ++tally.rgbOps;
    } else if (op == "g" || op == "G") {
      if (op == "g") gs.fillCmyk = false;
      else gs.strokeCmyk = false;
      ++tally.grayOps;
    } else if (op == "cs" || op == "CS") {
      if (op == "cs") gs.fillCmyk = false;
      else gs.strokeCmyk = false;
      resolveColorSpace(lastName);
    } else if (op == "sc" || op == "scn") {
      gs.fillCmyk = false;
    } else if (op == "SC" || op == "SCN") {
      gs.strokeCmyk = false;
    } else if (op == "S" || op == "s") {
      markHairline();
      markRich(gs.stroke, gs.strokeCmyk);
    } else if (op == "B" || op == "B*" || op == "b" || op == "b*") {
      markHairline();
      markRich(gs.stroke, gs.strokeCmyk);
      markRich(gs.fill, gs.fillCmyk);
    } else if (op == "f" || op == "F" || op == "f*") {
      markRich(gs.fill, gs.fillCmyk);
    } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
      int m = gs.renderMode;
      if (m == 3) {
        ++tally.invisibleText;
        tally.invisPages.insert(page);
      } else {
        if (m == 0 || m == 2 || m == 4 || m == 6) markRich(gs.fill, gs.fillCmyk);
        if (m == 1 || m == 2 || m == 5 || m == 6) {
          markRich(gs.stroke, gs.strokeCmyk);
          markHairline();
        }
        double eff = gs.fontSize * gs.tmScale * strokeScale(gs.ctm);
        if (eff > 0.01 && eff < kSmallTextPt) {
          ++tally.smallText;
          tally.smallTextPages.insert(page);
          if (tally.minTextPt == 0 || eff < tally.minTextPt) tally.minTextPt = eff;
        }
      }
    } else if (op == "Do" && !lastName.empty()) {
      draws.push_back({lastName, gs});
    }
    nums.clear();
    lastName.clear();
  }

  void handleEOF() override {}
};

void scanContent(QPDFObjectHandle contents, QPDFObjectHandle res, const Gs& initial,
                 int page, int depth, Visited& seen, Tally& tally) {
  if (depth > 12) return;
  Scanner scan(res, tally, page, initial);
  try {
    QPDFObjectHandle::parseContentStream(contents, &scan);
  } catch (...) {
    return;
  }
  QPDFObjectHandle xod = res.isDictionary() ? res.getKey("/XObject")
                                            : QPDFObjectHandle::newNull();
  if (!xod.isDictionary()) return;
  for (const auto& d : scan.draws) {
    QPDFObjectHandle xo = xod.getKey(d.first);
    if (!xo.isStream()) continue;
    QPDFObjectHandle dict = xo.getDict();
    std::string sub = nameOf(dict.getKey("/Subtype"));
    if (sub == "/Image") {
      QPDFObjectHandle filt = dict.getKey("/Filter");
      std::vector<std::string> fnames;
      if (filt.isName()) fnames.push_back(filt.getName());
      if (filt.isArray()) {
        for (int fi = 0; fi < filt.getArrayNItems(); ++fi) {
          fnames.push_back(nameOf(filt.getArrayItem(fi)));
        }
      }
      if (fnames.empty()) fnames.push_back("/None");
      ++tally.imageFilters[fnames.back()];
      if (dict.getKey("/ImageMask").isBool() && dict.getKey("/ImageMask").getBoolValue()) {
        ++tally.imageMasks;
      }
      if (dict.getKey("/BitsPerComponent").isInteger() &&
          dict.getKey("/BitsPerComponent").getIntValue() > 8) {
        ++tally.deepImages;
      }
      classifySpace(dict.getKey("/ColorSpace"), tally);
      if (dict.getKey("/SMask").isStream()) {
        ++tally.transparency;
        tally.transparencyPages.insert(page);
      }
      int w = dict.getKey("/Width").isInteger()
                  ? static_cast<int>(dict.getKey("/Width").getIntValue()) : 0;
      int h = dict.getKey("/Height").isInteger()
                  ? static_cast<int>(dict.getKey("/Height").getIntValue()) : 0;
      if (w <= 0 || h <= 0) continue;
      double wpt = std::hypot(d.second.ctm.a, d.second.ctm.b);
      double hpt = std::hypot(d.second.ctm.c, d.second.ctm.d);
      if (wpt <= 0.01 || hpt <= 0.01) continue;
      double ppiX = w * 72.0 / wpt;
      double ppiY = h * 72.0 / hpt;
      double ppi = ppiX > ppiY ? ppiX : ppiY;
      int bpc = dict.getKey("/BitsPerComponent").isInteger()
                    ? static_cast<int>(dict.getKey("/BitsPerComponent").getIntValue()) : 8;
      bool mask = dict.getKey("/ImageMask").isBool() &&
                  dict.getKey("/ImageMask").getBoolValue();
      bool bitonal = mask || bpc == 1;
      double lo = bitonal ? kBitonalMinPpi : kContoneMinPpi;
      double hi = bitonal ? kBitonalMaxPpi : kContoneMaxPpi;
      if (ppi < lo) {
        ++tally.lowRes;
        tally.lowPages.insert(page);
        if (tally.minPpi == 0 || ppi < tally.minPpi) tally.minPpi = ppi;
      } else if (ppi > hi) {
        ++tally.highRes;
        tally.highPages.insert(page);
        if (ppi > tally.maxPpi) tally.maxPpi = ppi;
      }
    } else if (sub == "/Form") {
      if (!seen.enter(xo)) continue;
      QPDFObjectHandle grp = dict.getKey("/Group");
      if (grp.isDictionary() && nameIs(grp.getKey("/S"), "/Transparency")) {
        ++tally.transparency;
        tally.transparencyPages.insert(page);
      }
      Gs inner = d.second;
      QPDFObjectHandle mtx = dict.getKey("/Matrix");
      if (mtx.isArray() && mtx.getArrayNItems() == 6) {
        Mat m;
        m.a = numOf(mtx.getArrayItem(0), 1); m.b = numOf(mtx.getArrayItem(1), 0);
        m.c = numOf(mtx.getArrayItem(2), 0); m.d = numOf(mtx.getArrayItem(3), 1);
        m.e = numOf(mtx.getArrayItem(4), 0); m.f = numOf(mtx.getArrayItem(5), 0);
        inner.ctm = mul(m, inner.ctm);
      }
      QPDFObjectHandle sres = dict.getKey("/Resources");
      scanContent(xo, sres.isDictionary() ? sres : res, inner, page, depth + 1, seen,
                  tally);
    }
  }
}

std::string pageList(const std::set<int>& pages) {
  std::string out;
  int shown = 0;
  for (int p : pages) {
    if (shown == 8) {
      out += ", …";
      break;
    }
    if (shown) out += ", ";
    out += std::to_string(p);
    ++shown;
  }
  std::string label = pages.size() == 1 ? "page " : "pages ";
  return label + out;
}
}

void passAnalyze(Ctx& ctx) {
  Tally tally;
  std::map<std::string, std::set<int>> pageSizes;
  std::map<std::string, long long> annotTypes;
  try {
    QPDFPageDocumentHelper dh(ctx.pdf);
    std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
    int pageNum = 0;
    for (auto& ph : pages) {
      ++pageNum;
      QPDFObjectHandle page = ph.getObjectHandle();
      QPDFObjectHandle res = ph.getAttribute("/Resources", true);
      Visited seen;
      Gs initial;
      scanContent(page.getKey("/Contents"), res, initial, pageNum, 0, seen, tally);
      QPDFObjectHandle grp = page.getKey("/Group");
      if (grp.isDictionary() && nameIs(grp.getKey("/S"), "/Transparency")) {
        ++tally.transparency;
        tally.transparencyPages.insert(pageNum);
      }
      QPDFObjectHandle mb = ph.getAttribute("/MediaBox", true);
      if (mb.isArray() && mb.getArrayNItems() == 4) {
        double w = std::fabs(numOf(mb.getArrayItem(2), 0) - numOf(mb.getArrayItem(0), 0));
        double h = std::fabs(numOf(mb.getArrayItem(3), 0) - numOf(mb.getArrayItem(1), 0));
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.0f x %.0f pt", w, h);
        pageSizes[buf].insert(pageNum);
      }
      QPDFObjectHandle annots = page.getKey("/Annots");
      if (annots.isArray()) {
        for (int i = 0; i < annots.getArrayNItems(); ++i) {
          QPDFObjectHandle a = annots.getArrayItem(i);
          if (!a.isDictionary()) continue;
          std::string sub = nameOf(a.getKey("/Subtype"));
          if (sub.size() > 1) ++annotTypes[sub.substr(1)];
        }
      }
    }
  } catch (...) {
    return;
  }
  auto finding = [&](const std::string& code, const std::string& detail) {
    ctx.res.analysis.push_back({code, detail, false});
  };
  if (tally.hairlines) {
    finding("ANALYZE_HAIRLINE",
            std::to_string(tally.hairlines) + " stroke(s) thinner than 0.125 pt at their "
            "rendered size (" + pageList(tally.hairlinePages) + ")");
  }
  if (tally.richBlack) {
    finding("ANALYZE_RICH_BLACK",
            std::to_string(tally.richBlack) + " object(s) painted in rich black (CMYK "
            "black over 90% with additional C/M/Y ink) (" + pageList(tally.richPages) +
            ")");
  }
  if (tally.invisibleText) {
    finding("ANALYZE_INVISIBLE_TEXT",
            std::to_string(tally.invisibleText) + " text run(s) in rendering mode 3, "
            "invisible and not used for clipping (" + pageList(tally.invisPages) + ")");
  }
  if (tally.lowRes) {
    finding("ANALYZE_IMAGE_LOWRES",
            std::to_string(tally.lowRes) + " image(s) below the minimum analysis "
            "resolution at their placed size, 250 ppi contone or 550 ppi bitonal; "
            "lowest is " + std::to_string(static_cast<int>(tally.minPpi)) + " ppi (" +
            pageList(tally.lowPages) + ")");
  }
  if (tally.highRes) {
    finding("ANALYZE_IMAGE_HIGHRES",
            std::to_string(tally.highRes) + " image(s) above the maximum analysis "
            "resolution at their placed size, 450 ppi contone or 3600 ppi bitonal; "
            "highest is " + std::to_string(static_cast<int>(tally.maxPpi)) + " ppi (" +
            pageList(tally.highPages) + ")");
  }
  if (tally.smallText) {
    char minBuf[24];
    std::snprintf(minBuf, sizeof(minBuf), "%.1f", tally.minTextPt);
    finding("ANALYZE_SMALL_TEXT",
            std::to_string(tally.smallText) + " text run(s) below 4 pt at rendered size; "
            "smallest is " + minBuf + " pt (" + pageList(tally.smallTextPages) + ")");
  }
  if (tally.transparency) {
    finding("ANALYZE_TRANSPARENCY",
            std::to_string(tally.transparency) + " use(s) of transparency (soft masks, "
            "constant alpha below 1, non-normal blend modes or transparency groups) (" +
            pageList(tally.transparencyPages) + ")");
  }
  if (tally.overprint) {
    finding("ANALYZE_OVERPRINT",
            std::to_string(tally.overprint) + " graphics state(s) enabling overprint (" +
            pageList(tally.overprintPages) + ")");
  }
  if (tally.rgbOps || tally.cmykOps || tally.grayOps || !tally.colorants.empty()) {
    std::string detail = "colour usage: " + std::to_string(tally.rgbOps) + " RGB, " +
                         std::to_string(tally.cmykOps) + " CMYK, " +
                         std::to_string(tally.grayOps) + " grayscale selection(s)";
    if (!tally.colorants.empty()) {
      detail += "; spot colourant(s):";
      int shown = 0;
      for (const std::string& c : tally.colorants) {
        if (shown == 8) {
          detail += " …";
          break;
        }
        detail += (shown ? ", " : " ") + c;
        ++shown;
      }
    }
    finding("ANALYZE_COLOR_USAGE", detail);
  }
  if (!tally.imageFilters.empty()) {
    std::string detail = "image compression:";
    bool first = true;
    for (const auto& kv : tally.imageFilters) {
      detail += (first ? " " : ", ") + std::to_string(kv.second) + " " +
                (kv.first.size() > 1 ? kv.first.substr(1) : kv.first);
      first = false;
    }
    if (tally.imageMasks) {
      detail += "; " + std::to_string(tally.imageMasks) + " stencil mask(s)";
    }
    if (tally.deepImages) {
      detail += "; " + std::to_string(tally.deepImages) + " image(s) above 8 bits per "
                "component";
    }
    finding("ANALYZE_IMAGE_FORMATS", detail);
  }
  {
    int notEmbedded = 0, type3 = 0, noToUnicode = 0;
    std::string firstMissing;
    for (QPDFObjectHandle f : tally.usedFontDicts) {
      std::string sub = nameOf(f.getKey("/Subtype"));
      if (sub == "/Type3") {
        ++type3;
        continue;
      }
      QPDFObjectHandle fd = f.getKey("/FontDescriptor");
      if (sub == "/Type0" && f.getKey("/DescendantFonts").isArray() &&
          f.getKey("/DescendantFonts").getArrayNItems() == 1 &&
          f.getKey("/DescendantFonts").getArrayItem(0).isDictionary()) {
        fd = f.getKey("/DescendantFonts").getArrayItem(0).getKey("/FontDescriptor");
      }
      bool embedded = fd.isDictionary() &&
                      (fd.getKey("/FontFile").isStream() ||
                       fd.getKey("/FontFile2").isStream() ||
                       fd.getKey("/FontFile3").isStream());
      if (!embedded) {
        ++notEmbedded;
        if (firstMissing.empty()) {
          std::string base = nameOf(f.getKey("/BaseFont"));
          if (base.size() > 1) firstMissing = base.substr(1);
        }
      }
      if (!f.getKey("/ToUnicode").isStream()) ++noToUnicode;
    }
    if (notEmbedded) {
      finding("ANALYZE_FONT_NOT_EMBEDDED",
              std::to_string(notEmbedded) + " used font(s) not embedded" +
                  (firstMissing.empty() ? "" : " (first: " + firstMissing + ")"));
    }
    if (type3) {
      finding("ANALYZE_TYPE3_FONTS", std::to_string(type3) + " Type 3 font(s) in use");
    }
    if (noToUnicode) {
      finding("ANALYZE_FONT_NO_TOUNICODE",
              std::to_string(noToUnicode) + " used font(s) without a ToUnicode map "
              "(text extraction unreliable)");
    }
  }
  if (pageSizes.size() > 1) {
    std::string detail = std::to_string(pageSizes.size()) + " distinct page sizes:";
    int shown = 0;
    for (const auto& kv : pageSizes) {
      if (shown == 4) {
        detail += " …";
        break;
      }
      detail += (shown ? ", " : " ") + kv.first + " (" + pageList(kv.second) + ")";
      ++shown;
    }
    finding("ANALYZE_MIXED_PAGE_SIZES", detail);
  }
  if (!annotTypes.empty()) {
    std::string detail = "annotations:";
    bool first = true;
    for (const auto& kv : annotTypes) {
      detail += (first ? " " : ", ") + std::to_string(kv.second) + " " + kv.first;
      first = false;
    }
    finding("ANALYZE_ANNOTATIONS", detail);
  }
  {
    QPDFObjectHandle ois = ctx.pdf.getRoot().getKey("/OutputIntents");
    if (ois.isArray() && ois.getArrayNItems() > 0 && ois.getArrayItem(0).isDictionary()) {
      QPDFObjectHandle oi = ois.getArrayItem(0);
      std::string s = nameOf(oi.getKey("/S"));
      std::string ident = oi.getKey("/OutputConditionIdentifier").isString()
                              ? oi.getKey("/OutputConditionIdentifier").getUTF8Value()
                              : "";
      finding("ANALYZE_OUTPUT_INTENT",
              "output intent present" + (s.size() > 1 ? " (" + s.substr(1) : "(") +
                  (ident.empty() ? "" : ", " + ident) + ")");
    } else {
      finding("ANALYZE_OUTPUT_INTENT", "no output intent present");
    }
  }
}
}
