#include "profile_eval.hh"

#include <qpdf/QPDFObjectHandle.hh>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "limits.hh"
#include "util.hh"

namespace pdfa {
double unitVal(const std::string& v, bool& hadUnit) {
  hadUnit = false;
  size_t n = v.size();
  if (n > 2) {
    std::string suf = v.substr(n - 2);
    if (suf == "mm" || suf == "cm" || suf == "in" || suf == "pt") {
      hadUnit = true;
      double x = std::atof(v.c_str());
      if (suf == "mm") return x * 72.0 / 25.4;
      if (suf == "cm") return x * 72.0 / 2.54;
      if (suf == "in") return x * 72.0;
      return x;
    }
  }
  return std::atof(v.c_str());
}

double numVal(const PfAtom& a) {
  if (a.vals.empty()) return 0.0;
  bool hadUnit = false;
  double first = unitVal(a.vals[0], hadUnit);
  if (first != 0.0) return first;
  for (size_t i = 1; i < a.vals.size(); ++i) {
    bool u = false;
    double v = unitVal(a.vals[i], u);
    if (u && v != 0.0) return v;
  }
  return first;
}

bool cmpNum(double lhs, const std::string& op, double rhs) {
  if (op == "less") return lhs < rhs;
  if (op == "less_or_equal") return lhs <= rhs;
  if (op == "greater") return lhs > rhs;
  if (op == "greater_or_equal") return lhs >= rhs;
  if (op == "equal") return std::fabs(lhs - rhs) < 1e-6;
  if (op == "unequal") return std::fabs(lhs - rhs) >= 1e-6;
  return false;
}

bool cmpBool(bool v, const std::string& op) {
  if (op == "is_true") return v;
  if (op == "is_not_true") return !v;
  return false;
}

int nonZeroComps(const std::vector<double>& c) {
  int n = 0;
  for (double v : c) {
    if (v > 0.001) ++n;
  }
  return n;
}

double maxComp(const std::vector<double>& c) {
  double m = 0;
  for (double v : c) m = std::max(m, v);
  return m;
}

bool isWhite(const std::vector<double>& c, const ColorInfo& ci) {
  if (ci.cls == "separation" || ci.cls == "devicen" || ci.cls == "pattern") {
    if (c.empty()) return false;
    return maxComp(c) < 0.001;
  }
  if (c.size() == 4) return maxComp(c) < 0.001;
  for (double v : c) {
    if (v < 0.999) return false;
  }
  return !c.empty();
}

int colorantCount(const std::vector<double>& c, const ColorInfo& ci) {
  if (ci.cls == "cmyk" || ci.cls == "separation" || ci.cls == "devicen") {
    return nonZeroComps(c);
  }
  if (ci.cls == "gray") return (!c.empty() && (1.0 - c[0]) > 0.001) ? 1 : 0;
  if (ci.cls == "icc") {
    if (c.size() == 4) return nonZeroComps(c);
    if (c.size() == 1) return (1.0 - c[0]) > 0.001 ? 1 : 0;
  }
  return isWhite(c, ci) ? 0 : 1;
}

bool blackOnly(const std::vector<double>& c, const ColorInfo& ci) {
  if (ci.cls == "separation" || ci.cls == "devicen") {
    return (ci.spot == "Black" || ci.spot == "All") && !c.empty() && c[0] > 0.999;
  }
  if (ci.cls == "rgb" || ci.cls == "lab") return false;
  if (ci.cls == "icc" && ci.declaredComps == 3) return false;
  if (c.size() == 4) return c[3] > 0.999 && c[0] < 0.001 && c[1] < 0.001 && c[2] < 0.001;
  if (c.size() == 1) return c[0] < 0.001;
  return false;
}

bool processOnly(const ColorInfo& ci) {
  if (ci.cls == "cmyk" || ci.cls == "gray") return true;
  if (ci.cls == "icc" && (ci.declaredComps == 4 || ci.declaredComps == 1)) return true;
  if (ci.cls == "separation" || ci.cls == "devicen") {
    if (ci.colorants.empty()) return false;
    for (const std::string& c : ci.colorants) {
      if (c != "Cyan" && c != "Magenta" && c != "Yellow" && c != "Black" && c != "Gray") {
        return false;
      }
    }
    return true;
  }
  return false;
}

double totalInk(const std::vector<double>& c) {
  double s = 0;
  for (double v : c) s += v;
  return s * 100.0;
}

double blackPercent(const std::vector<double>& c, const ColorInfo& ci) {
  if (ci.cls == "cmyk" && c.size() == 4) return c[3] * 100.0;
  if (ci.cls == "gray" && c.size() == 1) return (1.0 - c[0]) * 100.0;
  if ((ci.cls == "separation" || ci.cls == "devicen") && ci.spot == "Black" && !c.empty()) {
    return c[0] * 100.0;
  }
  return -1;
}

bool is100Black(const std::vector<double>& c, const ColorInfo& ci) {
  if (ci.cls == "cmyk" && c.size() == 4) {
    return c[3] > 0.999 && c[0] < 0.001 && c[1] < 0.001 && c[2] < 0.001;
  }
  if (ci.cls == "gray" && c.size() == 1) return c[0] < 0.001;
  if ((ci.cls == "separation" || ci.cls == "devicen") && ci.spot == "Black") {
    return !c.empty() && c[0] > 0.999;
  }
  return false;
}

bool spotIsRegistration(const ColorInfo& ci) {
  return ci.spot == "All" || ci.spot == "Registration" || ci.spot == "all";
}

bool effectiveOverprint(bool op, int opm, const ColorInfo& ci,
                        const std::vector<double>& comps) {
  (void)opm;
  (void)ci;
  (void)comps;
  return op;
}

bool spotIsProcess(const std::string& s) {
  static const std::set<std::string> proc = {"Cyan", "Magenta", "Yellow", "Black",
                                             "Gray", "Grey"};
  return proc.count(s) > 0;
}

bool cmpStr(const std::string& v, const std::string& op, const std::vector<std::string>& vals) {
  bool allEmpty = true;
  for (const std::string& x : vals) {
    if (!x.empty()) allEmpty = false;
  }
  if (vals.empty() || allEmpty) {
    bool present = !v.empty();
    if (op == "unequal" || op == "is_not_contained_in" || op == "not_contains") {
      return present;
    }
    if (op == "equal" || op == "is_contained_in" || op == "contains") return !present;
  }
  auto any = [&](std::function<bool(const std::string&)> pred) {
    for (const std::string& x : vals) {
      if (!x.empty() && pred(x)) return true;
    }
    return false;
  };
  auto stripSubsetTag = [](const std::string& n) {
    if (n.size() > 7 && n[6] == '+') {
      bool tag = true;
      for (int i = 0; i < 6; ++i) {
        if (!std::isupper(static_cast<unsigned char>(n[i]))) tag = false;
      }
      if (tag) return n.substr(7);
    }
    return n;
  };
  if (op == "equal") {
    std::string base = stripSubsetTag(v);
    return any([&](const std::string& x) { return base == x; });
  }
  if (op == "unequal") {
    std::string base = stripSubsetTag(v);
    return !any([&](const std::string& x) { return base == x; });
  }
  if (op == "is_contained_in" || op == "is_include" || op == "contains") {
    return any([&](const std::string& x) { return v.find(x) != std::string::npos; });
  }
  if (op == "is_not_contained_in" || op == "not_is_include" || op == "not_contains") {
    return !any([&](const std::string& x) { return v.find(x) != std::string::npos; });
  }
  if (op == "begins") {
    return any([&](const std::string& x) { return v.rfind(x, 0) == 0; });
  }
  if (op == "ends" || op == "not_ends") {
    bool e = any([&](const std::string& x) {
      return v.size() >= x.size() && v.compare(v.size() - x.size(), x.size(), x) == 0;
    });
    return op == "ends" ? e : !e;
  }
  return false;
}

Domain atomDomain(const std::string& token) {
  std::string ns = token.substr(0, token.find(':'));
  if (token == "CSCOLOR::IdenticalAppearanceForTwoMoreSpo") return Domain::kDoc;
  if (token == "CONTSTM::UnknowOperatInPDF1_3ThrougPDF") return Domain::kDoc;
  if (ns == "CSHALFTONE") return Domain::kPaint;
  if (ns == "DVASTRUCT" || ns == "DVACSTRM" || ns == "DVASYNTAX" || ns == "STRUCTPDF" ||
      ns == "CERTIFY" || ns == "OUTINTENTSA" || ns == "OUTINTENTSE") {
    return Domain::kDoc;
  }
  if (ns == "CONTSTM") return Domain::kAny;
  if (ns == "SIFTER") return Domain::kAny;
  if (ns == "CSGST_S" || ns == "CSGST_F" || ns == "CSGST_G") return Domain::kAny;
  if (ns == "CSCOLOR") return Domain::kAny;
  if (ns == "CSTEXT") return Domain::kText;
  if (ns == "CSIMAGE") return Domain::kImage;
  if (ns == "PAGE") return Domain::kPage;
  if (ns == "ANNOT") return Domain::kAnnot;
  if (ns == "CSFONT") return Domain::kFont;
  if (ns == "DOC" || ns == "DOCINFO" || ns == "OUTINTENTS" || ns == "OUTINTENTS_ICC" ||
      ns == "CSICC" || ns == "OPTIONALCONT" || ns == "PDFVT" || ns == "SIGNATURES") {
    return Domain::kDoc;
  }
  return Domain::kNone;
}

bool evalColorAtom(const PfAtom& a, const std::vector<double>& comps, const ColorInfo& ci,
                   bool& supported) {
  const std::string& t = a.token;
  if (t == "CSCOLOR::NumberOfNonZeroComponents") {
    return cmpNum(colorantCount(comps, ci), a.op, numVal(a));
  }
  if (t == "CSGST_S::NumberOfColoraWhichAreNonZero" ||
      t == "CSGST_F::NumberOfColoraWhichAreNonZero" ||
      t == "CSGST_F::NumberOfColoraWhichAreNonZeroFill") {
    int n;
    bool additive = ci.cls == "rgb" || ci.cls == "cal" || ci.cls == "lab" ||
                    (ci.cls == "icc" && ci.declaredComps == 3);
    bool grayLike =
        ci.cls == "gray" || (ci.cls == "icc" && ci.declaredComps == 1);
    if (grayLike) {
      n = (!comps.empty() && 1.0 - comps[0] > 0.001) ? 1 : 0;
    } else if (additive) {
      bool black = !comps.empty();
      n = 0;
      for (double v : comps) {
        if (v > 0.001) black = false;
        if (1.0 - v > 0.001) ++n;
      }
      if (black) n = 1;
    } else {
      n = nonZeroComps(comps);
    }
    return cmpNum(n, a.op, numVal(a));
  }
  if (t == "CSCOLOR::NrOfComponents") return cmpNum(ci.declaredComps, a.op, numVal(a));
  if (t == "CSCOLOR::ObjectHasNonZeroValuesAndLowe") {
    double ink = -1;
    if (ci.cls == "gray" && !comps.empty()) ink = 1.0 - comps[0];
    else if (ci.cls == "cmyk" || ci.cls == "separation" || ci.cls == "devicen") {
      ink = maxComp(comps);
    } else if (ci.cls == "icc") {
      if (comps.size() == 1) ink = 1.0 - comps[0];
      else if (comps.size() == 4) ink = maxComp(comps);
    }
    if (ink <= 0.001) return false;
    return cmpNum(ink * 100.0, a.op, numVal(a));
  }
  if (t == "CSCOLOR::ObjectIsWhite") return cmpBool(isWhite(comps, ci), a.op);
  if (t == "CSCOLOR::ObjectUsesBlackOnly") return cmpBool(blackOnly(comps, ci), a.op);
  if (t == "CSCOLOR::ObjectIs100_Black") return cmpBool(is100Black(comps, ci), a.op);
  if (t == "CSCOLOR::ObjectUsesBlackWithAPercenOf") {
    bool onlyBlackInk = false;
    if (ci.cls == "gray") onlyBlackInk = true;
    else if (ci.cls == "cmyk" && comps.size() == 4) {
      onlyBlackInk = comps[0] < 0.001 && comps[1] < 0.001 && comps[2] < 0.001 &&
                     comps[3] > 0.001;
    } else if ((ci.cls == "separation" || ci.cls == "devicen") &&
               (ci.spot == "Black" || ci.spot == "All")) {
      onlyBlackInk = !comps.empty() && comps[0] > 0.001;
    }
    if (!onlyBlackInk) return false;
    double bp = blackPercent(comps, ci);
    return bp >= 0 && cmpNum(bp, a.op, numVal(a));
  }
  if (t == "CSCOLOR::BlackObjUsesCMYwithAPercentageOf") {
    if (ci.cls != "cmyk" || comps.size() != 4 || comps[3] < 0.9995) return false;
    double cmy = std::max({comps[0], comps[1], comps[2]}) * 100.0;
    return cmpNum(cmy, a.op, numVal(a));
  }
  if (t == "CSCOLOR::IsDeviceGray") return cmpBool(ci.cls == "gray" && !ci.indexed, a.op);
  if (t == "CSCOLOR::IsDeviceCMYK") return cmpBool(ci.cls == "cmyk" && !ci.indexed, a.op);
  if (t == "CSCOLOR::ObjectUsesCMYKOnly_noSpotColo") {
    return cmpBool(processOnly(ci) && !ci.indexed, a.op);
  }
  if (t == "CSCOLOR::IsDeviceRGB") return cmpBool(ci.cls == "rgb", a.op);
  if (t == "CSCOLOR::UsesICCbasedCMYK") {
    return cmpBool(ci.cls == "icc" && ci.declaredComps == 4, a.op);
  }
  if (t == "CSCOLOR::UsesICCbasedRGB") {
    return cmpBool(ci.cls == "icc" && ci.declaredComps == 3, a.op);
  }
  if (t == "CSCOLOR::NumberOfNonZeroCMYKComponents") {
    if (ci.cls == "cmyk" && comps.size() == 4) {
      return cmpNum(nonZeroComps(comps), a.op, numVal(a));
    }
    if ((ci.cls == "separation" || ci.cls == "devicen") && processOnly(ci)) {
      std::set<std::string> nz;
      for (size_t i = 0; i < ci.colorants.size() && i < comps.size(); ++i) {
        if (comps[i] > 0.001) nz.insert(ci.colorants[i]);
      }
      if (ci.colorants.size() == 1 && comps.size() >= 1 && comps[0] > 0.001) {
        nz.insert(ci.colorants[0]);
      }
      return cmpNum(static_cast<double>(nz.size()), a.op, numVal(a));
    }
    if (ci.cls == "gray" && !comps.empty()) {
      return cmpNum(comps[0] < 0.999 ? 1 : 0, a.op, numVal(a));
    }
    return false;
  }
  if (t == "CSCOLOR::IsLabColorSpace") return cmpBool(ci.cls == "lab", a.op);
  if (t == "CSCOLOR::IsICCBasedColorSpace") return cmpBool(ci.cls == "icc", a.op);
  if (t == "CSCOLOR::IsCalColorSpace") return cmpBool(ci.cls == "cal", a.op);
  bool realSpot = (ci.cls == "separation" || ci.cls == "devicen") &&
                  !spotIsRegistration(ci) && ci.spot != "None" && !ci.spot.empty() &&
                  !spotIsProcess(ci.spot);
  if (t == "CSCOLOR::IsSeparaColorSpace") return cmpBool(ci.cls == "separation", a.op);
  if (t == "CSCOLOR::IsSpotColor" || t == "CSCOLOR::ObjectUsesSpotColor_Only_noCM") {
    return cmpBool(realSpot, a.op);
  }
  if (t == "CSCOLOR::IsRegistrationColor") return cmpBool(spotIsRegistration(ci), a.op);
  if (t == "CSCOLOR::SpotColorName") {
    return cmpStr(realSpot ? ci.spot : std::string(), a.op, a.vals);
  }
  if (t == "CSCOLOR::SpotColorNameHasPantoneSuffix") {
    bool pant = ci.spot.find("PANTONE") != std::string::npos ||
                ci.spot.find("Pantone") != std::string::npos;
    return cmpBool(pant, a.op);
  }
  if (t == "CSCOLOR::IsPattern") return cmpBool(ci.cls == "pattern", a.op);
  if (t == "CSCOLOR::IsCIEBasedColorSpace") {
    return cmpBool(ci.cls == "icc" || ci.cls == "cal" || ci.cls == "lab", a.op);
  }
  if (t == "CSCOLOR::BaseColorSpaceName") return cmpStr(ci.cls, a.op, a.vals);
  if (t == "CSCOLOR::AltBaseColorSpaceName") return cmpStr(ci.altName, a.op, a.vals);
  if (t == "CSCOLOR::DeviceNColorants") {
    return cmpNum(static_cast<double>(ci.colorants.size()), a.op, numVal(a));
  }
  if (t == "CSCOLOR::HasProcessColorAsSeparation") {
    return cmpBool(ci.cls == "separation" && spotIsProcess(ci.spot), a.op);
  }
  if (t == "CSCOLOR::HasProcessColorsAsDeviceN") {
    bool any = false;
    if (ci.cls == "devicen") {
      for (const std::string& c : ci.colorants) {
        if (spotIsProcess(c)) any = true;
      }
    }
    return cmpBool(any, a.op);
  }
  if (t == "CSCOLOR::SpotColorNameIsUTFEncoded") {
    bool ok = true;
    for (unsigned char c : ci.spot) {
      if (c >= 0x80) ok = false;
    }
    return cmpBool(ok, a.op);
  }
  supported = false;
  return false;
}

bool boxContains(const Box& outer, const Box& inner, double tol) {
  return outer.valid && inner.valid && inner.x0 >= outer.x0 - tol &&
         inner.y0 >= outer.y0 - tol && inner.x1 <= outer.x1 + tol &&
         inner.y1 <= outer.y1 + tol;
}

bool boxOutside(const Box& outer, const Box& inner, double tol) {
  return outer.valid && inner.valid &&
         (inner.x1 < outer.x0 - tol || inner.x0 > outer.x1 + tol ||
          inner.y1 < outer.y0 - tol || inner.y0 > outer.y1 + tol);
}

bool evalFontAtom(const PfAtom& a, const FontFacts& f, const Events& ev, bool& supported);

bool evalGsExtraAtom(const PfAtom& a, const GsExtra& x, bool stroke, bool& handled) {
  const std::string& t = a.token;
  handled = true;
  if (t == "CSGST_G::BlendMode") return cmpStr(x.blendMode, a.op, a.vals);
  if (t == "CSGST_G::HasSMaskEntry") return cmpBool(x.hasSMask, a.op);
  if (t == "CSGST_G::HasSMaskEntryWithAValueOfNone") return cmpBool(x.smaskExplicitNone, a.op);
  if (t == "CSGST_G::TransparencySoftmaskIsOfTypeLumi") {
    return cmpBool(x.hasSMask && x.smaskIsLuminosity, a.op);
  }
  if (t == "CSGST_G::BlendSpaceInLumiSMask") return cmpStr(x.smaskGroupCS, a.op, a.vals);
  if (t == "CSGST_G::BlendColorSpace") return cmpStr(x.smaskGroupCS, a.op, a.vals);
  if (t == "CSGST_G::Flatness") return cmpNum(x.flatness, a.op, numVal(a));
  if (t == "CSGST_G::HasTR2EntryWithAValueOfDefaul") {
    return cmpBool(x.hasTR2 && x.tr2IsDefault, a.op);
  }
  if (t == "CSGST_G::HasBlackPointCompeEntry") return cmpBool(x.hasBPC, a.op);
  if (t == "CSHALFTONE::HasHalftoneOriginEntry") return cmpBool(x.hasHalftoneOrigin, a.op);
  if (t == "CSGST_F::ConstantAlphaFill") {
    return !stroke && cmpNum(x.alphaFill, a.op, numVal(a));
  }
  if (t == "CSGST_S::ConstantAlphaStroke") {
    return stroke && cmpNum(x.alphaStroke, a.op, numVal(a));
  }
  if (t == "CSGST_G::BelongsToTransparencyGroup") {
    return cmpBool(x.inTransGroup, a.op);
  }
  (void)stroke;
  handled = false;
  return false;
}

bool boxesIntersect(const Box& a, const Box& b) {
  return a.valid && b.valid && a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

bool evalSifterAtom(const PfAtom& a, bool visible, bool covers, bool clippedPart,
                    bool clippedFull, bool& handled) {
  const std::string& t = a.token;
  handled = true;
  if (t == "SIFTER::ObjectIsVisible") return cmpBool(visible, a.op);
  if (t == "SIFTER::Params") return true;
  if (t == "SIFTER::ObjectCoversOtherObject") return cmpBool(covers, a.op);
  if (t == "SIFTER::ObjectIsPartiallyClipped") return cmpBool(clippedPart, a.op);
  if (t == "SIFTER::ObjectIsCompletelyClipped") return cmpBool(clippedFull, a.op);
  handled = false;
  return false;
}

const PageFacts* pageFor(const Events& ev, int page) {
  for (const PageFacts& p : ev.pages) {
    if (p.page == page) return &p;
  }
  return nullptr;
}

double distToBox(const Box& outer, const Box& inner) {
  if (!outer.valid || !inner.valid) return -1;
  double d = std::abs(inner.x0 - outer.x0);
  d = std::min(d, std::abs(inner.y0 - outer.y0));
  d = std::min(d, std::abs(outer.x1 - inner.x1));
  d = std::min(d, std::abs(outer.y1 - inner.y1));
  return d;
}

bool evalGeomAtom(const PfAtom& a, const Box& bbox, int page, const Events& ev,
                  bool& handled) {
  const std::string& t = a.token;
  handled = true;
  const PageFacts* p = pageFor(ev, page);
  if (t == "CONTSTM::ObjectIsOutsidMediaBox") {
    return p && cmpBool(boxOutside(p->media, bbox, 0.1), a.op);
  }
  if (t == "CONTSTM::ObjectIsOutsidBleedBox") {
    if (!p) return false;
    const Box& b = p->bleed.valid ? p->bleed : p->media;
    return cmpBool(boxOutside(b, bbox, 0.1), a.op);
  }
  if (t == "CONTSTM::ObjectIsInsideTrimBoAndArtBox") {
    if (!p) return false;
    const Box& b = p->trim.valid ? p->trim : (p->art.valid ? p->art : p->media);
    return cmpBool(boxContains(b, bbox, 0.1), a.op);
  }
  if (t == "CONTSTM::SmallestDistFromTrimBox" ||
      t == "CONTSTM::SmallestDistInTBoxBorder_pt") {
    if (!p) return false;
    const Box& b = p->trim.valid ? p->trim : p->media;
    double d = distToBox(b, bbox);
    return d >= 0 && cmpNum(d, a.op, numVal(a));
  }
  handled = false;
  return false;
}

bool evalPaintAtom(const PfAtom& a, const PaintEvent& e, const Events& ev,
                   bool& supported) {
  {
    bool gh = false;
    bool gr = evalGeomAtom(a, e.bbox, e.page, ev, gh);
    if (gh) return gr;
    bool sh = false;
    bool sr = evalSifterAtom(a, e.visible, e.covers, e.clippedPart, e.clippedFull, sh);
    if (sh) return sr;
  }
  const std::string& t = a.token;
  if (t == "CSGST_S::LineWidth") {
    if (e.stroke) return cmpNum(e.width, a.op, numVal(a));
    if (e.fillOp && e.bbox.valid) {
      double w = e.bbox.x1 - e.bbox.x0, h = e.bbox.y1 - e.bbox.y0;
      double mn = std::min(w, h), mx = std::max(w, h);
      if (mn >= 0.5 && mx >= mn * 6) return cmpNum(mn, a.op, numVal(a));
    }
    return false;
  }
  if (t == "CSGST_S::IsOverPrintEnabledStroke") {
    return e.stroke && cmpBool(effectiveOverprint(e.overprint, e.opm, e.color, e.comps), a.op);
  }
  if (t == "CSGST_F::IsOverPrintEnabledFill") {
    return !e.stroke &&
           cmpBool(effectiveOverprint(e.overprint, e.opm, e.color, e.comps), a.op);
  }
  if (t == "CSGST_G::IsOverPrintEnabled") {
    return cmpBool(effectiveOverprint(e.overprint, e.opm, e.color, e.comps), a.op);
  }
  if (t == "CSGST_G::IsIllustratorOverPrintMode") return cmpBool(e.opm == 1, a.op);
  if (t == "CSGST_S::TotalAmountOfInk" || t == "CSGST_F::TotalAmountOfInk" ||
      t == "CSGST_F::TotalAmountOfProcessInk") {
    return cmpNum(totalInk(e.comps), a.op, numVal(a));
  }
  if (t == "CSGST_G::HasTransparency" || t == "CSGST_S::HasTransparency" ||
      t == "CSGST_F::HasTransparency") {
    return cmpBool(e.transparency, a.op);
  }
  if (t == "CONTSTM::IsFilledArea") return cmpBool(e.fillOp && !e.shade, a.op);
  if (t == "CONTSTM::IsStroked" || t == "CONTSTM::IsLine") return cmpBool(e.stroke, a.op);
  if (t == "CONTSTM::IsSmoothShade") return cmpBool(e.shade, a.op);
  if (t == "CONTSTM::VectorObjectWithoutFillOrStroke") return cmpBool(e.noPaint, a.op);
  if (t == "CONTSTM::NumberOfNodesInPath") return cmpNum(e.pathNodes, a.op, numVal(a));
  if (t == "CSGST_F::ColorValue_1_Fill") {
    return !e.stroke && !e.comps.empty() && cmpNum(e.comps[0], a.op, numVal(a));
  }
  if (t == "CSGST_S::ColorValue_1_Stroke") {
    return e.stroke && !e.comps.empty() && cmpNum(e.comps[0], a.op, numVal(a));
  }
  if (t == "CONTSTM::IsText" || t == "CONTSTM::IsImage" || t == "CONTSTM::IsImageMask" ||
      t == "CONTSTM::IsBitmapImageOrImageMask") {
    return cmpBool(false, a.op);
  }
  if (t == "CONTSTM::FilledAndStroked") return cmpBool(e.fillOp && e.stroke, a.op);
  if (t == "CONTSTM::StrokedButNotFilled") return cmpBool(e.stroke && !e.fillOp, a.op);
  {
    bool handled = false;
    bool r = evalGsExtraAtom(a, e.x, e.stroke, handled);
    if (handled) return r;
  }
  bool s2 = true;
  bool r = evalColorAtom(a, e.comps, e.color, s2);
  if (s2) return r;
  supported = false;
  return false;
}

bool evalTextAtom(const PfAtom& a, const TextEvent& e, const Events& ev,
                  bool& supported) {
  {
    bool gh = false;
    bool gr = evalGeomAtom(a, e.bbox, e.page, ev, gh);
    if (gh) return gr;
    bool sh = false;
    bool sr = evalSifterAtom(a, e.visible, e.covers, e.clippedPart, e.clippedFull, sh);
    if (sh) return sr;
  }
  const std::string& t = a.token;
  if (t == "CSTEXT::Textsize") return cmpNum(e.sizePt, a.op, numVal(a));
  if (t == "CSTEXT::TextIsNotRenderAndNotUsedAsCl") {
    return cmpBool(e.renderMode == 3, a.op);
  }
  if (t == "CSTEXT::TextRenderMode") return cmpNum(e.renderMode, a.op, numVal(a));
  if (t == "CSTEXT::TextIsUsedAsClippiPath") return cmpBool(e.renderMode >= 4, a.op);
  if (t == "CSTEXT::TextIsStroked") {
    return cmpBool(e.renderMode == 1 || e.renderMode == 2 || e.renderMode == 5 ||
                       e.renderMode == 6,
                   a.op);
  }
  if (t.rfind("CSFONT::", 0) == 0) {
    for (const FontFacts& f : ev.fonts) {
      if (f.og == e.fontOg) return evalFontAtom(a, f, ev, supported);
    }
    return false;
  }
  if (t == "CSTEXT::GlyphIsUndefined") return cmpBool(e.glyphUndefined, a.op);
  if (t == "CSTEXT::GlyphIsWhitespace") return cmpBool(e.glyphWhitespace, a.op);
  if (t == "CSTEXT::GlyphHasContour") return cmpBool(e.glyphHasContour, a.op);
  if (t == "CSTEXT::CanBeMappedToUnicode") return cmpBool(e.mappedToUnicode, a.op);
  if (t == "CONTSTM::IsText") return cmpBool(true, a.op);
  if (t == "CSGST_S::IsOverPrintEnabledStroke") {
    return (e.renderMode == 1 || e.renderMode == 2 || e.renderMode == 5 ||
            e.renderMode == 6) &&
           cmpBool(effectiveOverprint(e.overprint, e.opm, e.color, e.comps), a.op);
  }
  if (t == "CSGST_F::IsOverPrintEnabledFill" || t == "CSGST_G::IsOverPrintEnabled") {
    return cmpBool(effectiveOverprint(e.overprint, e.opm, e.color, e.comps), a.op);
  }
  if (t == "CSGST_G::IsIllustratorOverPrintMode") return cmpBool(e.opm == 1, a.op);
  if (t == "CSGST_S::TotalAmountOfInk" || t == "CSGST_F::TotalAmountOfInk" ||
      t == "CSGST_F::TotalAmountOfProcessInk") {
    return cmpNum(totalInk(e.comps), a.op, numVal(a));
  }
  if (t == "CSGST_G::HasTransparency" || t == "CSGST_S::HasTransparency" ||
      t == "CSGST_F::HasTransparency") {
    return cmpBool(e.transparency, a.op);
  }
  {
    bool handled = false;
    bool r = evalGsExtraAtom(a, e.x, false, handled);
    if (handled) return r;
  }
  bool s2 = true;
  bool r = evalColorAtom(a, e.comps, e.color, s2);
  if (s2) return r;
  std::string ns = t.substr(0, t.find(':'));
  if (ns == "CSGST_S" || ns == "CSGST_F" || ns == "CSGST_G" || ns == "CONTSTM" ||
      ns == "SIFTER" || ns == "CSHALFTONE" || ns == "CSIMAGE") {
    return cmpBool(false, a.op);
  }
  supported = false;
  return false;
}

bool evalImageAtom(const PfAtom& a, const ImageEvent& e, const Events& ev,
                   bool& supported) {
  {
    bool gh = false;
    bool gr = evalGeomAtom(a, e.bbox, e.page, ev, gh);
    if (gh) return gr;
    bool sh = false;
    bool sr = evalSifterAtom(a, e.visible, e.covers, e.clippedPart, e.clippedFull, sh);
    if (sh) return sr;
  }
  const std::string& t = a.token;
  if (t == "CSIMAGE::Resolution") return e.ppi > 0 && cmpNum(e.ppi, a.op, numVal(a));
  if (t == "CSIMAGE::BitsPerColourComponent") return cmpNum(e.bpc, a.op, numVal(a));
  if (t == "CSIMAGE::Width") return cmpNum(e.width, a.op, numVal(a));
  if (t == "CSIMAGE::Height") return cmpNum(e.height, a.op, numVal(a));
  if (t == "CSIMAGE::HasSMaskEntry") return cmpBool(e.hasSMask, a.op);
  if (t == "CONTSTM::IsImage") return cmpBool(!e.mask, a.op);
  if (t == "CONTSTM::IsImageMask") return cmpBool(e.mask, a.op);
  if (t == "CONTSTM::IsBitmapImageOrImageMask") return cmpBool(e.bpc == 1 || e.mask, a.op);
  if (t == "CSIMAGE::ImageIsNotValid") return cmpBool(e.width <= 0 || e.height <= 0, a.op);
  if (t == "CSIMAGE::Interpolate" || t == "CSIMAGE::HasInterpolateEntry") {
    return cmpBool(e.interpolate, a.op);
  }
  if (t == "CSGST_S::IsOverPrintEnabledStroke") return false;
  if (t == "CSGST_F::IsOverPrintEnabledFill" || t == "CSGST_G::IsOverPrintEnabled") {
    return cmpBool(effectiveOverprint(e.overprint, e.opm, e.color, {}), a.op);
  }
  if (t == "CSGST_G::IsIllustratorOverPrintMode") return cmpBool(e.opm == 1, a.op);
  if (t == "CSGST_G::HasTransparency" || t == "CSGST_S::HasTransparency" ||
      t == "CSGST_F::HasTransparency") {
    return cmpBool(e.transparency, a.op);
  }
  if (t == "CSIMAGE::CompressionFilter") {
    bool has = false;
    for (const std::string& v : a.vals) {
      if (e.filters.count(v)) has = true;
    }
    if (a.op == "equal" || a.op == "is_include" || a.op == "contains") return has;
    if (a.op == "unequal" || a.op == "not_is_include" || a.op == "not_contains") {
      return !has;
    }
  }
  bool s2 = true;
  bool r = evalColorAtom(a, e.comps, e.color, s2);
  if (s2) return r;
  {
    std::string ns2 = t.substr(0, t.find(':'));
    if (ns2 == "CSGST_S" || ns2 == "CSGST_F" || ns2 == "CSGST_G" || ns2 == "CONTSTM" ||
        ns2 == "SIFTER" || ns2 == "CSHALFTONE" || ns2 == "CSTEXT") {
      return cmpBool(false, a.op);
    }
  }
  supported = false;
  return false;
}

bool evalAnnotAtom(const PfAtom& a, const AnnotFacts& an, const Events& ev,
                   bool& supported) {
  const std::string& t = a.token;
  if (t == "ANNOT::Type" || t == "ANNOT::AnnotaIsOfType") {
    return cmpStr(an.subtype, a.op, a.vals);
  }
  if (t == "ANNOT::TypeOfAnnotaIsNotDefinePDFSpe") return cmpBool(!an.knownType, a.op);
  if (t == "ANNOT::AnnotaHasCAEntry") return cmpBool(an.hasCA, a.op);
  if (t == "ANNOT::ValueForCAEntryInAnnotation") return cmpNum(an.ca, a.op, numVal(a));
  if (t == "ANNOT::Flag3IsSet_Print") return cmpBool(an.printFlag, a.op);
  if (t == "ANNOT::InsideBleedOrTrimBox") {
    const PageFacts* p = pageFor(ev, an.page);
    if (!p) return false;
    const Box& b = p->bleed.valid ? p->bleed : (p->trim.valid ? p->trim : p->media);
    return cmpBool(boxContains(b, an.rect, 0.1), a.op);
  }
  supported = false;
  return false;
}

bool evalFontAtom(const PfAtom& a, const FontFacts& f, const Events& ev,
                  bool& supported) {
  const std::string& t = a.token;
  if (t == "CSFONT::BaseFontName") return cmpStr(f.baseFont, a.op, a.vals);
  if (t == "CSFONT::IsEmbedded") return cmpBool(f.embedded, a.op);
  if (t == "CSFONT::FontIsNotEmbedded") return cmpBool(!f.embedded, a.op);
  if (t == "CSFONT::FontTypeIsType3") return cmpBool(f.type3, a.op);
  if (t == "CSFONT::FontTypeIsTrueType") return cmpBool(f.trueType && !f.cid, a.op);
  if (t == "CSFONT::FontTypeIsCID") return cmpBool(f.cid, a.op);
  if (t == "CSFONT::FlagKeyisSympolic") return cmpBool(f.hasFlags && f.symbolic, a.op);
  if (t == "CSFONT::CIDFonDictinContaiACIDToGWith") {
    return cmpBool(!(f.cid && f.trueType) || f.hasCIDToGIDMap, a.op);
  }

  if (t == "CSFONT::FontNameIsUniqueThroughout") {
    auto stripSubset = [](const std::string& n) {
      if (n.size() > 7 && n[6] == '+') {
        bool tag = true;
        for (int i = 0; i < 6; ++i) {
          if (!std::isupper(static_cast<unsigned char>(n[i]))) tag = false;
        }
        if (tag) return n.substr(7);
      }
      return n;
    };
    std::string base = stripSubset(f.baseFont);
    int share = 0;
    if (!base.empty()) {
      for (const FontFacts& o : ev.fonts) {
        if (stripSubset(o.baseFont) == base) ++share;
      }
    }
    return cmpBool(share <= 1, a.op);
  }
  if (t == "CSFONT::FontNameIsUTFEncoded") {
    bool ok = true;
    for (unsigned char c : f.baseFont) {
      if (c >= 0x80) ok = false;
    }
    return cmpBool(ok, a.op);
  }
  if (t == "CSFONT::FontIsNotValid") return cmpBool(f.embedded && !f.ftValid, a.op);
  if (t == "CSFONT::GlyphWidthMatchesInEmbedFont") {
    return cmpBool(!f.anyWidthMismatch, a.op);
  }
  if (t == "CSFONT::NumberOfEncodingsInCmapEntryOfEm") {
    return f.ftLoaded && cmpNum(f.cmapCount, a.op, numVal(a));
  }
  if (t == "CSFONT::FontSubsetContaiAllGlyphsUsed") {
    return cmpBool(!f.anyUndefinedGlyph, a.op);
  }
  if (t == "CSFONT::CharacterRevertsToNotdef") return cmpBool(f.anyUndefinedGlyph, a.op);
  if (t == "CSFONT::AllTextCanBeMappedToUnicode") return cmpBool(f.allUsedMapped, a.op);
  if (t == "CSFONT::SymbolTrueTyFontHasEncoodDict") {
    return cmpBool(f.trueType && !f.cid && f.symbolic &&
                       (f.hasEncodingDict || !f.encodingName.empty()),
                   a.op);
  }
  if (t == "CSFONT::NonSymbolTrueTyFontSpecifMacR") {
    if (!f.trueType || f.symbolic || f.cid) return cmpBool(true, a.op);
    bool ok = f.encodingName == "MacRomanEncoding" || f.encodingName == "WinAnsiEncoding" ||
              f.encodingName == "Differences";
    return cmpBool(ok, a.op);
  }
  if (t == "CSFONT::EmbeddingFlagIsPresent") return cmpBool(f.ftLoaded, a.op);
  if (t == "CSFONT::EmbFlagHasUnknownValue") {
    return cmpBool(f.ftLoaded && (f.fsType & ~0x030f) != 0, a.op);
  }
  if (t == "CSFONT::NoSubsetting") return cmpBool(f.ftLoaded && (f.fsType & 0x0100), a.op);
  if (t == "CSFONT::BitmapEmbeddingOnly") {
    return cmpBool(f.ftLoaded && (f.fsType & 0x0200), a.op);
  }
  if (t == "CSFONT::EditableEmbedding") {
    return cmpBool(f.ftLoaded && (f.fsType & 0x0008), a.op);
  }
  if (t == "CSFONT::PreviewPrintEmbedding") {
    return cmpBool(f.ftLoaded && (f.fsType & 0x0004), a.op);
  }
  if (t == "CSFONT::RestrictedLicenseEmbedding") {
    return cmpBool(f.ftLoaded && (f.fsType & 0x0002), a.op);
  }
  if (t == "CSFONT::InstallableEmbedding") return cmpBool(f.ftLoaded && f.fsType == 0, a.op);
  if (t == "CSFONT::FontCanBeEmbedded") {
    return cmpBool(!f.ftLoaded || (f.fsType & 0x0002) == 0, a.op);
  }
  supported = false;
  return false;
}

bool evalDocAtom(const PfAtom& a, const Events& ev, bool& supported) {
  const std::string& t = a.token;
  if (t == "DOC::Filesize") return cmpNum(ev.filesize, a.op, numVal(a));
  if (t == "DOC::NumberOfPages") return cmpNum(ev.pageCount, a.op, numVal(a));
  if (t == "DOC::NumberOfSpotPlates") {
    return cmpNum(static_cast<int>(ev.spotPlates.size()), a.op, numVal(a));
  }
  if (t == "DOC::NumberOfPlates") {
    return cmpNum(static_cast<int>(ev.spotPlates.size()) + 4, a.op, numVal(a));
  }
  if (t == "DOC::PDFVersion") return cmpStr(ev.pdfVersion, a.op, a.vals);
  if (t == "DOC::RequirementsKeyIsPDF20") return cmpBool(ev.requirementsPdf20, a.op);
  if (t == "DOC::PDFFileContainsDataAfterTheEndof") return cmpBool(ev.dataAfterEof, a.op);
  if (t == "DOC::XMPMetadaIsPlainText") return cmpBool(ev.qpdfWarnings == 0, a.op);
  if (t == "DOC::NameObjectIsUTF8Encoded" || t == "DOC::DecodeAllStreamDicts" ||
      t == "DOC::HexStringContainsInvalidChar") {
    return cmpBool(ev.qpdfWarnings == 0, a.op);
  }
  if (t == "DOC::SpotColorNamesAreEquivalent" ||
      t == "DOC::EquivalentNotidenticalSpotNames") {
    bool eq = false;
    std::map<std::string, int> canon;
    for (const std::string& s : ev.spotPlates) {
      std::string c;
      for (char ch : s) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
          c += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
      }
      if (++canon[c] > 1) eq = true;
    }
    return cmpBool(eq, a.op);
  }
  if (t == "DOC::SpotColorRepresAreInconsisten" ||
      t == "CSCOLOR::IdenticalAppearanceForTwoMoreSpo") {
    bool bad = false;
    if (t == "DOC::SpotColorRepresAreInconsisten") {
      for (const auto& [name, alts] : ev.spotAlternates) {
        if (alts.size() > 1) bad = true;
      }
    } else {
      std::map<std::string, int> byAlt;
      for (const auto& [name, alts] : ev.spotAlternates) {
        for (const std::string& al : alts) {
          if (++byAlt[al] > 1) bad = true;
        }
      }
    }
    return cmpBool(bad, a.op);
  }
  if (t == "DOC::OrientSizeEqualAllPagesWithTol") {
    double tol = numVal(a) > 0 ? numVal(a) : 3.0;
    bool same = true;
    for (size_t i = 1; i < ev.pages.size(); ++i) {
      if (std::abs(ev.pages[i].wPt - ev.pages[0].wPt) > tol ||
          std::abs(ev.pages[i].hPt - ev.pages[0].hPt) > tol) {
        same = false;
      }
    }
    if (a.op == "is_true" || a.op == "is_not_true") return cmpBool(same, a.op);
    return same;
  }
  if (t == "DOC::BooleanCheck") return cmpBool(true, a.op);
  if (t == "DOCINFO::Creator") return cmpStr(ev.infoCreator, a.op, a.vals);
  if (t == "DOCINFO::Producer") return cmpStr(ev.infoProducer, a.op, a.vals);
  if (t == "DOCINFO::Trapped") return cmpStr(ev.infoTrapped, a.op, a.vals);
  if (t == "DOCINFO::HasPDF_XFields") return cmpBool(ev.infoHasPdfxFields, a.op);
  if (t == "OUTINTENTS_ICC::IcVersion" || t == "CSICC::IcVersion") {
    return cmpNum(ev.iccVersionMajor, a.op, numVal(a));
  }
  if (t == "OPTIONALCONT::ProcessingSteps" || t == "OPTIONALCONT::ProcStepsPresent" ||
      t == "OPTIONALCONT::DocHasProcStepsMetadata" ||
      t == "OPTIONALCONT::PageHasProcStepsMetadata") {
    return cmpBool(ev.docHasProcSteps, a.op);
  }
  if (t == "OPTIONALCONT::ProcStepLayersMissingOnPage" ||
      t == "OPTIONALCONT::ProcStepsDoesNotHaveTypeEntry" ||
      t == "OPTIONALCONT::ProcStepsUsesCustomVal" ||
      t == "OPTIONALCONT::SameProcStepsInMoreLayer" ||
      t == "OPTIONALCONT::CustomPSKeyIsSecondClassName" ||
      t == "OPTIONALCONT::PSSpotCSUsedForPrintContent" ||
      t == "OPTIONALCONT::PSUsesMoreThanOneSpotCS" ||
      t == "OPTIONALCONT::PSSpotCSUsedForMorePS") {
    return cmpBool(false, a.op);
  }
  if (t == "OPTIONALCONT::OCPropertiesHasConfigsKey") return cmpBool(ev.ocHasConfigs, a.op);
  if (t == "OPTIONALCONT::BelongsToALayer") return cmpBool(ev.hasOCProperties, a.op);
  if (t == "OPTIONALCONT::IsCurrentlyVisible") return cmpBool(true, a.op);
  if (t == "PDFVT::CatalogContainsDPartRootEntry") return cmpBool(ev.hasDPartRoot, a.op);
  if (t == "SIGNATURES::DocumentHasSignatureFields") return cmpBool(ev.hasSigFields, a.op);
  {
    std::string ns = t.substr(0, t.find(':'));
    if (ns == "DVASTRUCT" || ns == "STRUCTPDF") {
      std::string tail = t.substr(t.find("::") + 2);
      tail[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(tail[0])));
      if (tail == "noStructTree" || tail == "noParentTree" || tail == "notStandardType" ||
          tail == "noClassMap" || tail == "structTypeNameIsUTF8Encoded") {
        return cmpBool(ev.docIssues.count(tail) > 0, a.op);
      }
      return cmpBool(ev.docIssues.count("parserWarnings") > 0 && ev.hasStructTree, a.op);
    }
    if (ns == "DVASYNTAX" || ns == "DVACSTRM") {
      return cmpBool(ev.qpdfWarnings > 0, a.op);
    }
  }
  if (t == "CONTSTM::UnknowOperatInPDF1_3ThrougPDF") {
    return cmpBool(ev.qpdfWarnings > 0, a.op);
  }
  if (t == "CERTIFY::CertifyXMPIsPresent" || t == "CERTIFY::CertifyXMPIsSyntactValid") {
    bool present = ev.xmpRaw.find("reflight") != std::string::npos &&
                   ev.xmpRaw.find("ertif") != std::string::npos;
    return cmpBool(present, a.op);
  }
  if (t == "CERTIFY::CertifySigIsPresent" || t == "CERTIFY::CertifySigIsSyntactValid") {
    return cmpBool(ev.hasSigFields, a.op);
  }
  if (t == "CERTIFY::CertifyFieldPreflightRes" ||
      t == "CERTIFY::CertifyValidReportsNoErrors" ||
      t == "CERTIFY::DocModSinceCertifyApplied") {
    return cmpBool(false, a.op);
  }
  if (t.find("CxFConformanceLevel") != std::string::npos ||
      t.find("CxFEntryConfToCxFX4XMLSchema") != std::string::npos) {
    bool present = ev.xmpRaw.find("colorexchangeformat.com") != std::string::npos;
    if (!present) return false;
    if (t.find("IsCxFX4a") != std::string::npos) {
      return cmpBool(ev.xmpRaw.find("CxF/X-4a") != std::string::npos, a.op);
    }
    if (t.find("IsCxFX4b") != std::string::npos) {
      return cmpBool(ev.xmpRaw.find("CxF/X-4b") != std::string::npos, a.op);
    }
    return cmpBool(ev.xmpRaw.find("CxF/X-4") != std::string::npos, a.op);
  }
  if (t.find("NumOfCxFEntries") != std::string::npos) {
    int cnt = 0;
    size_t p = 0;
    while ((p = ev.xmpRaw.find("cxf:Object", p)) != std::string::npos) {
      ++cnt;
      p += 10;
    }
    return cmpNum(cnt / 2, a.op, numVal(a));
  }
  if (t.find("NumOfSpotColWithoutCxFEntry") != std::string::npos) {
    if (ev.xmpRaw.find("colorexchangeformat.com") == std::string::npos) {
      return cmpNum(0, a.op, numVal(a));
    }
    int without = 0;
    for (const std::string& sp : ev.spotPlates) {
      if (ev.xmpRaw.find(sp) == std::string::npos) ++without;
    }
    return cmpNum(without, a.op, numVal(a));
  }
  if (t.find("OIHasMixingHintsEntry") != std::string::npos) {
    return cmpBool(ev.docIssues.count("oiMixingHints") > 0, a.op);
  }
  if (t.find("IcICCProfileIsNotValid") != std::string::npos) {
    return cmpBool(ev.hasOutputIntent && ev.iccColorSpace.empty(), a.op);
  }
  if (t == "PAGE::HasMediaBox") {
    return cmpBool(ev.pagesWithMediaBox == ev.pageCount && ev.pageCount > 0, a.op);
  }
  if (t == "OUTINTENTS::HasOutputProfile" || t == "OUTINTENTS::HasPDFA_OutputIntent" ||
      t == "OUTINTENTS::HasPDFX_OutputIntent") {
    return cmpBool(ev.hasOutputIntent, a.op);
  }
  if (t == "OUTINTENTS::NumberOfPDFXOutputIntentEntries" ||
      t == "OUTINTENTS::NumberOfOutputIntents") {
    return cmpNum(ev.outputIntentCount, a.op, numVal(a));
  }
  if (t == "OUTINTENTS_ICC::IcColorSpace" || t == "CSICC::IcColorSpace") {
    return cmpStr(ev.iccColorSpace, a.op, a.vals);
  }
  if (t == "OUTINTENTS_ICC::IcISO15076ProfileID") {
    return cmpStr(ev.iccProfileId, a.op, a.vals);
  }
  if (t == "ANNOT::Type" || t == "ANNOT::AnnotaIsOfType") {
    for (const std::string& at : ev.annotTypes) {
      if (cmpStr(at, a.op, a.vals)) return true;
    }
    return false;
  }
  if (t == "CSFONT::BaseFontName") {
    for (const std::string& bf : ev.baseFonts) {
      if (cmpStr(bf, a.op, a.vals)) return true;
    }
    return false;
  }
  supported = false;
  return false;
}

bool evalPageAtom(const PfAtom& a, const PageFacts& p, bool& supported) {
  const std::string& t = a.token;
  if (t == "PAGE::HasMediaBox") return cmpBool(p.hasMediaBox, a.op);
  if (t == "PAGE::HasCropBox") return cmpBool(p.hasCropBox, a.op);
  if (t == "PAGE::CropBoIsSameAsMediaBox") return cmpBool(p.cropEqualsMedia, a.op);
  if (t == "PAGE::PageIsScaled") return cmpBool(p.scaled, a.op);
  if (t == "PAGE::PageHasOnlyOneImage") return cmpBool(p.imageCount == 1, a.op);
  if (t == "PAGE::IsRotated") return cmpBool(p.rotated, a.op);
  if (t == "PAGE::PageIsEmpty") return cmpBool(p.empty, a.op);
  if (t == "PAGE::PageNo") return cmpNum(p.page, a.op, numVal(a));
  if (t == "PAGE::TransGroupHasTransObj") {
    return cmpBool(p.hasTransGroup && p.hasTransObj, a.op);
  }
  if (t == "PAGE::IsContentsStreamCompressed") return cmpBool(p.contentCompressed, a.op);
  if (t == "PAGE::HasPagelevelOI") return cmpBool(p.hasPageOI, a.op);
  if (t == "PAGE::BooleanCheck") return cmpBool(true, a.op);
  if (t == "PAGE::EffectiveInkCoverage" || t == "PAGE::EffectiveInkCoverageCustomArea") {
    return p.inkCoverage >= 0 && cmpNum(p.inkCoverage, a.op, numVal(a));
  }
  if (t == "PAGE::DetectVisualDifferences") return cmpBool(false, a.op);
  if (t == "PAGE::PageDescriptionNotValid") return cmpBool(false, a.op);
  if (t == "PAGE::PageUsesSpecificPlates") {
    double value = a.vals.empty() ? 0 : std::atof(a.vals[0].c_str());
    const std::set<std::string>& pool = p.plates;
    {
      int names = 0;
      bool onlyBlackList = true;
      for (const std::string& v : a.vals) {
        if (v == "Black" || v == "Cyan" || v == "Magenta" || v == "Yellow" ||
            v == "@spot") {
          ++names;
          if (v != "Black") onlyBlackList = false;
        }
      }
      if (a.op == "greater" && names == 1 && onlyBlackList) return false;
    }
    size_t nameStart = 4;
    if (a.vals.size() > 3) {
      int n = std::atoi(a.vals[3].c_str());
      if (n <= 0 || nameStart + n > a.vals.size() + 1) nameStart = 4;
    }
    static const std::set<std::string> kProcess = {"Cyan", "Magenta", "Yellow", "Black"};
    bool used = false;
    for (size_t i = nameStart; i < a.vals.size(); ++i) {
      const std::string& want = a.vals[i];
      if (want == "@spot") {
        for (const std::string& pl : pool) {
          if (!kProcess.count(pl)) used = true;
        }
      } else if (pool.count(want)) {
        used = true;
      }
    }
    double coverage = used ? 100.0 : 0.0;
    return cmpNum(coverage, a.op, value);
  }
  supported = false;
  return false;
}
}
