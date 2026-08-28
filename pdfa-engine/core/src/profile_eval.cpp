#include "profile_eval.hh"

#include <qpdf/QPDFObjectHandle.hh>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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
    if (op == "equal" || op == "in" || op == "contains") return !present;
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
  if (op == "in" || op == "occurs_in" || op == "contains") {
    return any([&](const std::string& x) { return v.find(x) != std::string::npos; });
  }
  if (op == "is_not_contained_in" || op == "occurs_in" || op == "not_contains") {
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
  if (token == "print.spot_naming") return Domain::kDoc;
  if (token == "syntax.unknown_operator") return Domain::kDoc;
  if (ns == "grp_13332") return Domain::kPaint;
  if (ns == "grp_12910" || ns == "grp_78948" || ns == "grp_79118" || ns == "grp_54741" ||
      ns == "grp_88767" || ns == "grp_77532" || ns == "grp_16769") {
    return Domain::kDoc;
  }
  if (ns == "grp_78804") return Domain::kAny;
  if (ns == "grp_92045") return Domain::kAny;
  if (ns == "grp_45919" || ns == "grp_75736" || ns == "grp_57935") return Domain::kAny;
  if (ns == "grp_37816") return Domain::kAny;
  if (ns == "grp_37222") return Domain::kText;
  if (ns == "grp_7226") return Domain::kImage;
  if (ns == "grp_46892") return Domain::kPage;
  if (ns == "grp_3254") return Domain::kAnnot;
  if (ns == "CSFONT") return Domain::kFont;
  if (ns == "grp_86055" || ns == "grp_7639" || ns == "grp_75300" || ns == "grp_39296" ||
      ns == "grp_2412" || ns == "grp_34967" || ns == "grp_2184" || ns == "grp_38257") {
    return Domain::kDoc;
  }
  return Domain::kNone;
}

bool evalColorAtom(const PfAtom& a, const std::vector<double>& comps, const ColorInfo& ci,
                   bool& supported) {
  const std::string& t = a.token;
  if (t == "color.inks_used") {
    return cmpNum(colorantCount(comps, ci), a.op, numVal(a));
  }
  if (t == "color.inks_used" ||
      t == "color.inks_used" ||
      t == "color.inks_used") {
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
  if (t == "color.component_count") return cmpNum(ci.declaredComps, a.op, numVal(a));
  if (t == "color.ink") {
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
  if (t == "color.ink_mix") return cmpBool(isWhite(comps, ci), a.op);
  if (t == "color.ink_mix") return cmpBool(blackOnly(comps, ci), a.op);
  if (t == "color.ink_mix") return cmpBool(is100Black(comps, ci), a.op);
  if (t == "color.ink") {
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
  if (t == "color.ink") {
    if (ci.cls != "cmyk" || comps.size() != 4 || comps[3] < 0.9995) return false;
    double cmy = std::max({comps[0], comps[1], comps[2]}) * 100.0;
    return cmpNum(cmy, a.op, numVal(a));
  }
  if (t == "color.space") return cmpBool(ci.cls == "gray" && !ci.indexed, a.op);
  if (t == "color.space") return cmpBool(ci.cls == "cmyk" && !ci.indexed, a.op);
  if (t == "color.ink_mix") {
    return cmpBool(processOnly(ci) && !ci.indexed, a.op);
  }
  if (t == "color.space") return cmpBool(ci.cls == "rgb", a.op);
  if (t == "color.space") {
    return cmpBool(ci.cls == "icc" && ci.declaredComps == 4, a.op);
  }
  if (t == "color.space") {
    return cmpBool(ci.cls == "icc" && ci.declaredComps == 3, a.op);
  }
  if (t == "color.inks_used") {
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
  if (t == "color.space") return cmpBool(ci.cls == "lab", a.op);
  if (t == "color.space") return cmpBool(ci.cls == "icc", a.op);
  if (t == "color.space") return cmpBool(ci.cls == "cal", a.op);
  bool realSpot = (ci.cls == "separation" || ci.cls == "devicen") &&
                  !spotIsRegistration(ci) && ci.spot != "None" && !ci.spot.empty() &&
                  !spotIsProcess(ci.spot);
  if (t == "color.space") return cmpBool(ci.cls == "separation", a.op);
  if (t == "color.spot" || t == "color.ink_mix") {
    return cmpBool(realSpot, a.op);
  }
  if (t == "color.spot") return cmpBool(spotIsRegistration(ci), a.op);
  if (t == "color.spot") {
    return cmpStr(realSpot ? ci.spot : std::string(), a.op, a.vals);
  }
  if (t == "color.spot") {
    bool pant = ci.spot.find("PANTONE") != std::string::npos ||
                ci.spot.find("Pantone") != std::string::npos;
    return cmpBool(pant, a.op);
  }
  if (t == "color.space") return cmpBool(ci.cls == "pattern", a.op);
  if (t == "color.space") {
    return cmpBool(ci.cls == "icc" || ci.cls == "cal" || ci.cls == "lab", a.op);
  }
  if (t == "color.space") return cmpStr(ci.cls, a.op, a.vals);
  if (t == "color.space") return cmpStr(ci.altName, a.op, a.vals);
  if (t == "color.devicen_colorants") {
    return cmpNum(static_cast<double>(ci.colorants.size()), a.op, numVal(a));
  }
  if (t == "color.spot") {
    return cmpBool(ci.cls == "separation" && spotIsProcess(ci.spot), a.op);
  }
  if (t == "color.devicen") {
    bool any = false;
    if (ci.cls == "devicen") {
      for (const std::string& c : ci.colorants) {
        if (spotIsProcess(c)) any = true;
      }
    }
    return cmpBool(any, a.op);
  }
  if (t == "color.spot") {
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
  if (t == "transparency.blend_mode") return cmpStr(x.blendMode, a.op, a.vals);
  if (t == "transparency.used") return cmpBool(x.hasSMask, a.op);
  if (t == "transparency.soft_mask") return cmpBool(x.smaskExplicitNone, a.op);
  if (t == "transparency.soft_mask") {
    return cmpBool(x.hasSMask && x.smaskIsLuminosity, a.op);
  }
  if (t == "transparency.group_colour_space") return cmpStr(x.smaskGroupCS, a.op, a.vals);
  if (t == "transparency.group_colour_space") return cmpStr(x.smaskGroupCS, a.op, a.vals);
  if (t == "print.flatness") return cmpNum(x.flatness, a.op, numVal(a));
  if (t == "print.rendering_functions") {
    return cmpBool(x.hasTR2 && x.tr2IsDefault, a.op);
  }
  if (t == "color.black_point_compensation") return cmpBool(x.hasBPC, a.op);
  if (t == "print.halftone") return cmpBool(x.hasHalftoneOrigin, a.op);
  if (t == "transparency.alpha") {
    return !stroke && cmpNum(x.alphaFill, a.op, numVal(a));
  }
  if (t == "transparency.alpha") {
    return stroke && cmpNum(x.alphaStroke, a.op, numVal(a));
  }
  if (t == "transparency.group") {
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
  if (t == "content.visibility") return cmpBool(visible, a.op);
  if (t == "content.relation") return true;
  if (t == "content.relation") return cmpBool(covers, a.op);
  if (t == "content.clipped") return cmpBool(clippedPart, a.op);
  if (t == "content.clipped") return cmpBool(clippedFull, a.op);
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
  if (t == "content.box_relation") {
    return p && cmpBool(boxOutside(p->media, bbox, 0.1), a.op);
  }
  if (t == "content.box_relation") {
    if (!p) return false;
    const Box& b = p->bleed.valid ? p->bleed : p->media;
    return cmpBool(boxOutside(b, bbox, 0.1), a.op);
  }
  if (t == "content.box_relation") {
    if (!p) return false;
    const Box& b = p->trim.valid ? p->trim : (p->art.valid ? p->art : p->media);
    return cmpBool(boxContains(b, bbox, 0.1), a.op);
  }
  if (t == "content.distance_to_box" ||
      t == "content.distance_to_box") {
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
  if (t == "content.stroke_width") {
    if (e.stroke) return cmpNum(e.width, a.op, numVal(a));
    if (e.fillOp && e.bbox.valid) {
      double w = e.bbox.x1 - e.bbox.x0, h = e.bbox.y1 - e.bbox.y0;
      double mn = std::min(w, h), mx = std::max(w, h);
      if (mn >= 0.5 && mx >= mn * 6) return cmpNum(mn, a.op, numVal(a));
    }
    return false;
  }
  if (t == "color.overprint") {
    return e.stroke && cmpBool(effectiveOverprint(e.overprint, e.opm, e.color, e.comps), a.op);
  }
  if (t == "color.overprint") {
    return !e.stroke &&
           cmpBool(effectiveOverprint(e.overprint, e.opm, e.color, e.comps), a.op);
  }
  if (t == "color.overprint") {
    return cmpBool(effectiveOverprint(e.overprint, e.opm, e.color, e.comps), a.op);
  }
  if (t == "color.overprint") return cmpBool(e.opm == 1, a.op);
  if (t == "color.total_ink" || t == "color.total_ink" ||
      t == "color.total_ink") {
    return cmpNum(totalInk(e.comps), a.op, numVal(a));
  }
  if (t == "transparency.used" || t == "transparency.used" ||
      t == "transparency.used") {
    return cmpBool(e.transparency, a.op);
  }
  if (t == "content.painting") return cmpBool(e.fillOp && !e.shade, a.op);
  if (t == "content.painting" || t == "content.kind") return cmpBool(e.stroke, a.op);
  if (t == "content.kind") return cmpBool(e.shade, a.op);
  if (t == "content.painting") return cmpBool(e.noPaint, a.op);
  if (t == "content.path_measure") return cmpNum(e.pathNodes, a.op, numVal(a));
  if (t == "color.value") {
    return !e.stroke && !e.comps.empty() && cmpNum(e.comps[0], a.op, numVal(a));
  }
  if (t == "color.value") {
    return e.stroke && !e.comps.empty() && cmpNum(e.comps[0], a.op, numVal(a));
  }
  if (t == "content.kind" || t == "content.kind" || t == "content.kind" ||
      t == "content.kind") {
    return cmpBool(false, a.op);
  }
  if (t == "content.painting") return cmpBool(e.fillOp && e.stroke, a.op);
  if (t == "content.painting") return cmpBool(e.stroke && !e.fillOp, a.op);
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
  if (t == "text.size") return cmpNum(e.sizePt, a.op, numVal(a));
  if (t == "text.rendering") {
    return cmpBool(e.renderMode == 3, a.op);
  }
  if (t == "text.state") return cmpNum(e.renderMode, a.op, numVal(a));
  if (t == "text.rendering") return cmpBool(e.renderMode >= 4, a.op);
  if (t == "text.rendering") {
    return cmpBool(e.renderMode == 1 || e.renderMode == 2 || e.renderMode == 5 ||
                       e.renderMode == 6,
                   a.op);
  }
  if (t.rfind("groups:font,text", 0) == 0) {
    for (const FontFacts& f : ev.fonts) {
      if (f.og == e.fontOg) return evalFontAtom(a, f, ev, supported);
    }
    return false;
  }
  if (t == "text.glyph") return cmpBool(e.glyphUndefined, a.op);
  if (t == "text.glyph") return cmpBool(e.glyphWhitespace, a.op);
  if (t == "text.glyph") return cmpBool(e.glyphHasContour, a.op);
  if (t == "text.unicode") return cmpBool(e.mappedToUnicode, a.op);
  if (t == "content.kind") return cmpBool(true, a.op);
  if (t == "color.overprint") {
    return (e.renderMode == 1 || e.renderMode == 2 || e.renderMode == 5 ||
            e.renderMode == 6) &&
           cmpBool(effectiveOverprint(e.overprint, e.opm, e.color, e.comps), a.op);
  }
  if (t == "color.overprint" || t == "color.overprint") {
    return cmpBool(effectiveOverprint(e.overprint, e.opm, e.color, e.comps), a.op);
  }
  if (t == "color.overprint") return cmpBool(e.opm == 1, a.op);
  if (t == "color.total_ink" || t == "color.total_ink" ||
      t == "color.total_ink") {
    return cmpNum(totalInk(e.comps), a.op, numVal(a));
  }
  if (t == "transparency.used" || t == "transparency.used" ||
      t == "transparency.used") {
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
  if (ns == "grp_45919" || ns == "grp_75736" || ns == "grp_57935" || ns == "grp_78804" ||
      ns == "grp_92045" || ns == "grp_13332" || ns == "grp_7226") {
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
  if (t == "image.resolution") return e.ppi > 0 && cmpNum(e.ppi, a.op, numVal(a));
  if (t == "image.property") return cmpNum(e.bpc, a.op, numVal(a));
  if (t == "image.property") return cmpNum(e.width, a.op, numVal(a));
  if (t == "image.property") return cmpNum(e.height, a.op, numVal(a));
  if (t == "image.property") return cmpBool(e.hasSMask, a.op);
  if (t == "content.kind") return cmpBool(!e.mask, a.op);
  if (t == "content.kind") return cmpBool(e.mask, a.op);
  if (t == "content.kind") return cmpBool(e.bpc == 1 || e.mask, a.op);
  if (t == "image.property") return cmpBool(e.width <= 0 || e.height <= 0, a.op);
  if (t == "image.property" || t == "image.property") {
    return cmpBool(e.interpolate, a.op);
  }
  if (t == "color.overprint") return false;
  if (t == "color.overprint" || t == "color.overprint") {
    return cmpBool(effectiveOverprint(e.overprint, e.opm, e.color, {}), a.op);
  }
  if (t == "color.overprint") return cmpBool(e.opm == 1, a.op);
  if (t == "transparency.used" || t == "transparency.used" ||
      t == "transparency.used") {
    return cmpBool(e.transparency, a.op);
  }
  if (t == "image.property") {
    bool has = false;
    for (const std::string& v : a.vals) {
      if (e.filters.count(v)) has = true;
    }
    if (a.op == "equal" || a.op == "occurs_in" || a.op == "contains") return has;
    if (a.op == "unequal" || a.op == "occurs_in" || a.op == "not_contains") {
      return !has;
    }
  }
  bool s2 = true;
  bool r = evalColorAtom(a, e.comps, e.color, s2);
  if (s2) return r;
  {
    std::string ns2 = t.substr(0, t.find(':'));
    if (ns2 == "grp_45919" || ns2 == "grp_75736" || ns2 == "grp_57935" || ns2 == "grp_78804" ||
        ns2 == "grp_92045" || ns2 == "grp_13332" || ns2 == "grp_37222") {
      return cmpBool(false, a.op);
    }
  }
  supported = false;
  return false;
}

bool evalAnnotAtom(const PfAtom& a, const AnnotFacts& an, const Events& ev,
                   bool& supported) {
  const std::string& t = a.token;
  if (t == "annotation.type" || t == "annotation.type") {
    return cmpStr(an.subtype, a.op, a.vals);
  }
  if (t == "annotation.type") return cmpBool(!an.knownType, a.op);
  if (t == "annotation.entry") return cmpBool(an.hasCA, a.op);
  if (t == "annotation.entry") return cmpNum(an.ca, a.op, numVal(a));
  if (t == "annotation.flags") return cmpBool(an.printFlag, a.op);
  if (t == "content.box_relation") {
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
  if (t == "font.name") return cmpStr(f.baseFont, a.op, a.vals);
  if (t == "font.embedding") return cmpBool(f.embedded, a.op);
  if (t == "font.embedding") return cmpBool(!f.embedded, a.op);
  if (t == "font.type") return cmpBool(f.type3, a.op);
  if (t == "font.type") return cmpBool(f.trueType && !f.cid, a.op);
  if (t == "font.type") return cmpBool(f.cid, a.op);
  if (t == "font.flags") return cmpBool(f.hasFlags && f.symbolic, a.op);
  if (t == "font.problem") {
    return cmpBool(!(f.cid && f.trueType) || f.hasCIDToGIDMap, a.op);
  }

  if (t == "font.name_problem") {
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
  if (t == "font.name_problem") {
    bool ok = true;
    for (unsigned char c : f.baseFont) {
      if (c >= 0x80) ok = false;
    }
    return cmpBool(ok, a.op);
  }
  if (t == "font.problem") return cmpBool(f.embedded && !f.ftValid, a.op);
  if (t == "font.problem") {
    return cmpBool(!f.anyWidthMismatch, a.op);
  }
  if (t == "font.problem") {
    return f.ftLoaded && cmpNum(f.cmapCount, a.op, numVal(a));
  }
  if (t == "font.problem") {
    return cmpBool(!f.anyUndefinedGlyph, a.op);
  }
  if (t == "text.glyph") return cmpBool(f.anyUndefinedGlyph, a.op);
  if (t == "font.all_text_unicode") return cmpBool(f.allUsedMapped, a.op);
  if (t == "font.problem") {
    return cmpBool(f.trueType && !f.cid && f.symbolic &&
                       (f.hasEncodingDict || !f.encodingName.empty()),
                   a.op);
  }
  if (t == "font.problem") {
    if (!f.trueType || f.symbolic || f.cid) return cmpBool(true, a.op);
    bool ok = f.encodingName == "MacRomanEncoding" || f.encodingName == "WinAnsiEncoding" ||
              f.encodingName == "Differences";
    return cmpBool(ok, a.op);
  }
  if (t == "font.license") return cmpBool(f.ftLoaded, a.op);
  if (t == "font.license") {
    return cmpBool(f.ftLoaded && (f.fsType & ~0x030f) != 0, a.op);
  }
  if (t == "font.license") return cmpBool(f.ftLoaded && (f.fsType & 0x0100), a.op);
  if (t == "font.license") {
    return cmpBool(f.ftLoaded && (f.fsType & 0x0200), a.op);
  }
  if (t == "font.license") {
    return cmpBool(f.ftLoaded && (f.fsType & 0x0008), a.op);
  }
  if (t == "font.license") {
    return cmpBool(f.ftLoaded && (f.fsType & 0x0004), a.op);
  }
  if (t == "font.license") {
    return cmpBool(f.ftLoaded && (f.fsType & 0x0002), a.op);
  }
  if (t == "font.license") return cmpBool(f.ftLoaded && f.fsType == 0, a.op);
  if (t == "font.license") {
    return cmpBool(!f.ftLoaded || (f.fsType & 0x0002) == 0, a.op);
  }
  supported = false;
  return false;
}

bool evalDocAtom(const PfAtom& a, const Events& ev, bool& supported) {
  const std::string& t = a.token;
  if (t == "document.file_size") return cmpNum(ev.filesize, a.op, numVal(a));
  if (t == "document.page_count") return cmpNum(ev.pageCount, a.op, numVal(a));
  if (t == "print.plates") {
    return cmpNum(static_cast<int>(ev.spotPlates.size()), a.op, numVal(a));
  }
  if (t == "print.plates") {
    return cmpNum(static_cast<int>(ev.spotPlates.size()) + 4, a.op, numVal(a));
  }
  if (t == "document.pdf_version") return cmpStr(ev.pdfVersion, a.op, a.vals);
  if (t == "document.requirements") return cmpBool(ev.requirementsPdf20, a.op);
  if (t == "document.data_after_eof") return cmpBool(ev.dataAfterEof, a.op);
  if (t == "metadata.xmp_problem") return cmpBool(ev.qpdfWarnings == 0, a.op);
  if (t == "syntax.string_encoding" || t == "document.stream_compression" ||
      t == "syntax.string_encoding") {
    return cmpBool(ev.qpdfWarnings == 0, a.op);
  }
  if (t == "print.spot_naming" ||
      t == "print.spot_naming") {
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
  if (t == "print.spot_naming" ||
      t == "print.spot_naming") {
    bool bad = false;
    if (t == "print.spot_naming") {
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
  if (t == "document.pages_uniform") {
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
  if (t == "scope.every") return cmpBool(true, a.op);
  if (t == "metadata.info") return cmpStr(ev.infoCreator, a.op, a.vals);
  if (t == "metadata.info") return cmpStr(ev.infoProducer, a.op, a.vals);
  if (t == "print.trapped") return cmpStr(ev.infoTrapped, a.op, a.vals);
  if (t == "metadata.info_keys") return cmpBool(ev.infoHasPdfxFields, a.op);
  if (t == "icc.header" || t == "icc.header") {
    return cmpNum(ev.iccVersionMajor, a.op, numVal(a));
  }
  if (t == "layer.processing_step" || t == "layer.processing_step" ||
      t == "layer.processing_step_problem" ||
      t == "layer.processing_step_problem") {
    return cmpBool(ev.docHasProcSteps, a.op);
  }
  if (t == "layer.processing_step_problem" ||
      t == "layer.processing_step_problem" ||
      t == "layer.processing_step_problem" ||
      t == "layer.processing_step_problem" ||
      t == "layer.processing_step_problem" ||
      t == "layer.processing_step_problem" ||
      t == "layer.processing_step_problem" ||
      t == "layer.processing_step_problem") {
    return cmpBool(false, a.op);
  }
  if (t == "layer.configuration") return cmpBool(ev.ocHasConfigs, a.op);
  if (t == "layer.membership") return cmpBool(ev.hasOCProperties, a.op);
  if (t == "layer.visible") return cmpBool(true, a.op);
  if (t == "variable_data.document_parts") return cmpBool(ev.hasDPartRoot, a.op);
  if (t == "signature.present") return cmpBool(ev.hasSigFields, a.op);
  {
    std::string ns = t.substr(0, t.find(':'));
    if (ns == "grp_12910" || ns == "grp_54741") {
      std::string tail = t.substr(t.find("::") + 2);
      tail[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(tail[0])));
      if (tail == "noStructTree" || tail == "noParentTree" || tail == "notStandardType" ||
          tail == "noClassMap" || tail == "structTypeNameIsUTF8Encoded") {
        return cmpBool(ev.docIssues.count(tail) > 0, a.op);
      }
      return cmpBool(ev.docIssues.count("parserWarnings") > 0 && ev.hasStructTree, a.op);
    }
    if (ns == "grp_79118" || ns == "grp_78948") {
      return cmpBool(ev.qpdfWarnings > 0, a.op);
    }
  }
  if (t == "syntax.unknown_operator") {
    return cmpBool(ev.qpdfWarnings > 0, a.op);
  }
  if (t == "seal.state" || t == "seal.state") {
    bool present = ev.xmpRaw.find("reflight") != std::string::npos &&
                   ev.xmpRaw.find("ertif") != std::string::npos;
    return cmpBool(present, a.op);
  }
  if (t == "seal.state" || t == "seal.state") {
    return cmpBool(ev.hasSigFields, a.op);
  }
  if (t == "seal.record" ||
      t == "seal.state" ||
      t == "seal.state") {
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
  if (t == "page.box_present") {
    return cmpBool(ev.pagesWithMediaBox == ev.pageCount && ev.pageCount > 0, a.op);
  }
  if (t == "output_intent.entry" || t == "output_intent.count" ||
      t == "output_intent.count") {
    return cmpBool(ev.hasOutputIntent, a.op);
  }
  if (t == "output_intent.count" ||
      t == "output_intent.count") {
    return cmpNum(ev.outputIntentCount, a.op, numVal(a));
  }
  if (t == "icc.header" || t == "icc.header") {
    return cmpStr(ev.iccColorSpace, a.op, a.vals);
  }
  if (t == "icc.header") {
    return cmpStr(ev.iccProfileId, a.op, a.vals);
  }
  if (t == "annotation.type" || t == "annotation.type") {
    for (const std::string& at : ev.annotTypes) {
      if (cmpStr(at, a.op, a.vals)) return true;
    }
    return false;
  }
  if (t == "font.name") {
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
  if (t == "page.box_present") return cmpBool(p.hasMediaBox, a.op);
  if (t == "page.box_present") return cmpBool(p.hasCropBox, a.op);
  if (t == "page.box_relation") return cmpBool(p.cropEqualsMedia, a.op);
  if (t == "page.user_unit") return cmpBool(p.scaled, a.op);
  if (t == "page.single_image") return cmpBool(p.imageCount == 1, a.op);
  if (t == "page.rotation") return cmpBool(p.rotated, a.op);
  if (t == "page.empty") return cmpBool(p.empty, a.op);
  if (t == "page.number") return cmpNum(p.page, a.op, numVal(a));
  if (t == "page.transparency_group") {
    return cmpBool(p.hasTransGroup && p.hasTransObj, a.op);
  }
  if (t == "page.content_stream") return cmpBool(p.contentCompressed, a.op);
  if (t == "page.output_intent") return cmpBool(p.hasPageOI, a.op);
  if (t == "scope.every") return cmpBool(true, a.op);
  if (t == "print.ink_coverage" || t == "print.ink_coverage") {
    return p.inkCoverage >= 0 && cmpNum(p.inkCoverage, a.op, numVal(a));
  }
  if (t == "REMOVED") return cmpBool(false, a.op);
  if (t == "REMOVED") return cmpBool(false, a.op);
  if (t == "print.uses_plates") {
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
