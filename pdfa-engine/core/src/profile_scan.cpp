#include <qpdf/QPDF.hh>
#include <qpdf/QPDFNumberTreeObjectHelper.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "fonts_ft.hh"
#include "limits.hh"
#include "profile_events.hh"
#include "pdffunc.hh"
#include "util.hh"

namespace pdfa {
bool hasDefaultSpace(const std::string& cls, QPDFObjectHandle res) {
  QPDFObjectHandle csd = res.isDictionary() ? res.getKey("/ColorSpace") : QPDFObjectHandle();
  if (!csd.isDictionary()) return false;
  const char* key = cls == "gray" ? "/DefaultGray" : cls == "rgb" ? "/DefaultRGB" : cls == "cmyk" ? "/DefaultCMYK" : nullptr;
  return key && !csd.getKey(key).isNull();
}

std::string blendSpaceLabel(QPDFObjectHandle cs, QPDFObjectHandle res, std::string& base, bool& devInd) {
  devInd = false;
  if (cs.isName()) {
    std::string n = cs.getName();
    if (n != "/DeviceGray" && n != "/DeviceRGB" && n != "/DeviceCMYK" && res.isDictionary()) {
      QPDFObjectHandle csd = res.getKey("/ColorSpace");
      if (csd.isDictionary() && !csd.getKey(n).isNull()) return blendSpaceLabel(csd.getKey(n), QPDFObjectHandle(), base, devInd);
    }
    base = n.size() > 1 ? n.substr(1) : n;
    return base;
  }
  if (!cs.isArray() || cs.getArrayNItems() < 1) return std::string();
  std::string fam = nameOf(cs.getArrayItem(0));
  if (fam == "/ICCBased" && cs.getArrayNItems() >= 2 && cs.getArrayItem(1).isStream()) {
    devInd = true;
    QPDFObjectHandle st = cs.getArrayItem(1);
    int n = static_cast<int>(numOf(st.getDict().getKey("/N"), 0));
    std::string sig;
    try {
      auto b = st.getStreamData(qpdf_dl_all);
      if (b->getSize() >= 20) sig.assign(reinterpret_cast<const char*>(b->getBuffer()) + 16, 4);
    } catch (...) {
    }
    QPDFObjectHandle alt = st.getDict().getKey("/Alternate");
    base = alt.isName() ? alt.getName().substr(1) : n == 4 ? "DeviceCMYK" : n == 1 ? "DeviceGray" : "DeviceRGB";
    if (sig == "Lab ") return "ICCBasedLab";
    if (sig == "CMYK" || (sig.empty() && n == 4)) return "ICCBasedCMYK";
    if (sig == "RGB " || (sig.empty() && n == 3)) return "ICCBasedRGB";
    if (sig == "GRAY" || (sig.empty() && n == 1)) return "ICCBasedGray";
    return "ICCBased";
  }
  if (fam == "/CalRGB" || fam == "/CalGray") {
    devInd = true;
    base = fam == "/CalRGB" ? "DeviceRGB" : "DeviceGray";
    return fam.substr(1);
  }
  if (fam == "/Lab") {
    base = "Lab";
    return "Lab";
  }
  base = fam.size() > 1 ? fam.substr(1) : fam;
  return base;
}

void enterGroup(GsExtra& x, QPDFObjectHandle grp, QPDFObjectHandle res) {
  if (!grp.isDictionary() || !nameIs(grp.getKey("/S"), "/Transparency")) return;
  x.inTransGroup = true;
  ++x.groupDepth;
  QPDFObjectHandle cs = grp.getKey("/CS");
  x.groupHasCS = !cs.isNull();
  std::string base;
  bool devInd = false;
  std::string label = x.groupHasCS ? blendSpaceLabel(cs, res, base, devInd) : std::string();
  if (x.groupDepth == 1) x.groupCSOuter = label;
  if (x.groupHasCS) {
    x.groupCSObj = cs;
    if (cs.isName() && res.isDictionary() && res.getKey("/ColorSpace").isDictionary() && !res.getKey("/ColorSpace").getKey(cs.getName()).isNull()) {
      x.groupCSObj = res.getKey("/ColorSpace").getKey(cs.getName());
    }
    x.groupCSInner = label;
    x.groupBaseCS = base;
    x.groupCSDevInd = devInd;
  }
  x.groupIsolated = grp.getKey("/I").isBool() && grp.getKey("/I").getBoolValue();
  x.groupKnockout = grp.getKey("/K").isBool() && grp.getKey("/K").getBoolValue();
}

ColorInfo deviceColor(const char* cls, int n, QPDFObjectHandle res) {
  ColorInfo ci;
  ci.cls = cls;
  ci.declaredComps = n;
  ci.hasDefault = hasDefaultSpace(ci.cls, res);
  return ci;
}

ColorInfo classifyColor(QPDFObjectHandle cs, QPDFObjectHandle res, int depth) {
  ColorInfo ci;
  if (depth > kMaxColorSpaceNest) return ci;
  if (cs.isName()) {
    std::string n = cs.getName();
    if (n == "/DeviceGray" || n == "/G") { ci.cls = "gray"; ci.declaredComps = 1; }
    else if (n == "/DeviceRGB" || n == "/RGB") { ci.cls = "rgb"; ci.declaredComps = 3; }
    else if (n == "/DeviceCMYK" || n == "/CMYK") { ci.cls = "cmyk"; ci.declaredComps = 4; }
    else if (n == "/Pattern") { ci.cls = "pattern"; ci.declaredComps = 0; }
    else if (res.isDictionary()) {
      QPDFObjectHandle csd = res.getKey("/ColorSpace");
      if (csd.isDictionary() && !csd.getKey(n).isNull()) {
        return classifyColor(csd.getKey(n), res, depth + 1);
      }
    }
    ci.hasDefault = hasDefaultSpace(ci.cls, res);
    return ci;
  }
  if (!cs.isArray() || cs.getArrayNItems() < 1) return ci;
  ci.csObj = cs;
  std::string fam = nameOf(cs.getArrayItem(0));
  if (fam == "/ICCBased" && cs.getArrayNItems() >= 2 && cs.getArrayItem(1).isStream()) {
    ci.cls = "icc";
    QPDFObjectHandle nk = cs.getArrayItem(1).getDict().getKey("/N");
    ci.declaredComps = nk.isInteger() ? static_cast<int>(nk.getIntValue()) : 3;
  } else if (fam == "/CalRGB") { ci.cls = "cal"; ci.declaredComps = 3; }
  else if (fam == "/CalGray") { ci.cls = "cal"; ci.declaredComps = 1; }
  else if (fam == "/Lab") { ci.cls = "lab"; ci.declaredComps = 3; }
  else if (fam == "/Separation" && cs.getArrayNItems() >= 2) {
    ci.cls = "separation";
    ci.declaredComps = 1;
    std::string nm = nameOf(cs.getArrayItem(1));
    if (nm.size() > 1) ci.spot = nm.substr(1);
    ci.colorants.push_back(ci.spot);
    if (cs.getArrayNItems() >= 3) {
      QPDFObjectHandle alt = cs.getArrayItem(2);
      std::string an = alt.isName() ? alt.getName()
                       : (alt.isArray() && alt.getArrayNItems() > 0)
                           ? nameOf(alt.getArrayItem(0)) : std::string();
      if (an.size() > 1) ci.altName = an.substr(1);
      if (cs.getArrayNItems() >= 4) {
        auto fn = PdfFunction::loadArray(cs.getArrayItem(3));
        double in = 1, out[32] = {0};
        if (fn && fn->outputs() <= 32 && fn->compute(&in, out)) ci.altValues.assign(out, out + fn->outputs());
      }
    }
  } else if (fam == "/DeviceN" && cs.getArrayNItems() >= 2 &&
             cs.getArrayItem(1).isArray()) {
    ci.cls = "devicen";
    ci.declaredComps = cs.getArrayItem(1).getArrayNItems();
    QPDFObjectHandle names = cs.getArrayItem(1);
    for (int i = 0; i < names.getArrayNItems(); ++i) {
      std::string nm = nameOf(names.getArrayItem(i));
      if (nm.size() > 1) ci.colorants.push_back(nm.substr(1));
    }
    if (!ci.colorants.empty()) ci.spot = ci.colorants[0];
    if (cs.getArrayNItems() >= 3) {
      QPDFObjectHandle alt = cs.getArrayItem(2);
      std::string an = alt.isName() ? alt.getName()
                       : (alt.isArray() && alt.getArrayNItems() > 0)
                           ? nameOf(alt.getArrayItem(0)) : std::string();
      if (an.size() > 1) ci.altName = an.substr(1);
    }
  } else if (fam == "/Indexed" || fam == "/I") {
    if (cs.getArrayNItems() >= 2) {
      ci = classifyColor(cs.getArrayItem(1), res, depth + 1);
      ci.indexed = true;
      if (!ci.csObj.isInitialized() || ci.csObj.isNull()) ci.csObj = cs;
      return ci;
    }
  } else if (fam == "/Pattern") {
    ci.cls = "pattern";
    ci.declaredComps = 0;
  }
  return ci;
}

struct EvScanner : QPDFObjectHandle::ParserCallbacks {
  Gs gs;
  std::vector<Gs> stack;
  int qSuppressed = 0;
  std::map<QPDFObjGen, std::string> programCache;
  std::vector<double> nums;
  std::string lastName;
  QPDFObjectHandle res;
  Events& ev;
  int page;
  std::vector<std::pair<std::string, Gs>> draws;
  std::vector<std::pair<std::string, Gs>> patternUses;
  std::vector<std::pair<QPDFObjectHandle, Gs>> type3Uses;
  Box pathBox;
  int pathNodes = 0;
  bool pathIsClip = false;
  std::set<std::string> directFontsSeen;
  double tmX = 0, tmY = 0;
  int biW = 0, biH = 0, biBpc = 0;
  bool biMask = false;
  bool biFiltered = false;
  std::string biLastKey;
  QPDFObjectHandle curFont;
  struct PathStat {
    int subpaths = 0, closed = 0, curves = 0, lines = 0, rects = 0, maxPts = 0, minPts = 0, curPts = 0;
    double length = 0, sx = 0, sy = 0, cx = 0, cy = 0;
    std::vector<double> geo;
  } ps;
  int bxDepth = 0;
  mutable int zLocal = 0;

  void geoAdd(std::initializer_list<double> v) {
    if (ps.geo.size() + v.size() <= 24000) ps.geo.insert(ps.geo.end(), v);
  }
  Mat tlm;
  std::vector<bool> actualMarks;
  bool srcOk = false;
  std::vector<size_t> drawOffs;
  QPDFObjectHandle parentTreeEntry;
  bool parentTreeLoaded = false;

  std::pair<double, double> dev(double x, double y) const {
    return {gs.ctm.a * x + gs.ctm.c * y + gs.ctm.e, gs.ctm.b * x + gs.ctm.d * y + gs.ctm.f};
  }

  void finishSub() {
    if (ps.curPts <= 0) return;
    ps.maxPts = std::max(ps.maxPts, ps.curPts);
    ps.minPts = ps.minPts == 0 ? ps.curPts : std::min(ps.minPts, ps.curPts);
    ps.curPts = 0;
  }

  void moveTo(double x, double y) {
    finishSub();
    ++ps.subpaths;
    ps.curPts = 1;
    auto p = dev(x, y);
    ps.sx = ps.cx = p.first;
    ps.sy = ps.cy = p.second;
    geoAdd({0, p.first, p.second});
  }

  void lineTo(double x, double y) {
    if (ps.curPts == 0) return moveTo(x, y);
    auto p = dev(x, y);
    ps.length += std::hypot(p.first - ps.cx, p.second - ps.cy);
    ps.cx = p.first;
    ps.cy = p.second;
    geoAdd({1, p.first, p.second});
    ++ps.lines;
    ++ps.curPts;
  }

  void curveTo(std::pair<double, double> c1, std::pair<double, double> c2, std::pair<double, double> e) {
    if (ps.curPts == 0) {
      ++ps.subpaths;
      ps.curPts = 1;
      ps.sx = ps.cx = c1.first;
      ps.sy = ps.cy = c1.second;
      geoAdd({0, ps.cx, ps.cy});
    }
    geoAdd({2, c1.first, c1.second, c2.first, c2.second, e.first, e.second});
    double px = ps.cx, py = ps.cy, len = 0;
    for (int i = 1; i <= 16; ++i) {
      double t = i / 16.0, u = 1 - t;
      double qx = u * u * u * ps.cx + 3 * u * u * t * c1.first + 3 * u * t * t * c2.first + t * t * t * e.first;
      double qy = u * u * u * ps.cy + 3 * u * u * t * c1.second + 3 * u * t * t * c2.second + t * t * t * e.second;
      len += std::hypot(qx - px, qy - py);
      px = qx;
      py = qy;
    }
    ps.length += len;
    ps.cx = e.first;
    ps.cy = e.second;
    ++ps.curves;
    ++ps.curPts;
  }

  void closeSub() {
    if (ps.curPts == 0) return;
    ++ps.closed;
    geoAdd({3});
    double d = std::hypot(ps.cx - ps.sx, ps.cy - ps.sy);
    if (d > 1e-9) {
      ps.length += d;
      ++ps.lines;
    }
    ps.cx = ps.sx;
    ps.cy = ps.sy;
    finishSub();
  }

  void rectStat(double x, double y, double w, double h) {
    finishSub();
    auto p0 = dev(x, y), p1 = dev(x + w, y), p2 = dev(x + w, y + h), p3 = dev(x, y + h);
    ps.length += std::hypot(p1.first - p0.first, p1.second - p0.second) + std::hypot(p2.first - p1.first, p2.second - p1.second) +
                 std::hypot(p3.first - p2.first, p3.second - p2.second) + std::hypot(p0.first - p3.first, p0.second - p3.second);
    geoAdd({0, p0.first, p0.second, 1, p1.first, p1.second, 1, p2.first, p2.second, 1, p3.first, p3.second, 3});
    ++ps.subpaths;
    ++ps.closed;
    ++ps.rects;
    ps.curPts = 4;
    ps.sx = ps.cx = p0.first;
    ps.sy = ps.cy = p0.second;
    finishSub();
  }

  void fillPathStats(PaintEvent& e) {
    finishSub();
    e.subpaths = ps.subpaths;
    e.closedSubpaths = ps.closed;
    e.curves = ps.curves;
    e.lines = ps.lines;
    e.rects = ps.rects;
    e.maxSubPts = ps.maxPts;
    e.minSubPts = ps.minPts;
    e.length = ps.length;
    e.geo = ps.geo;
  }

  GsExtra snap() const {
    GsExtra x = gs.x;
    x.ctm = gs.ctm;
    x.bxDepth = bxDepth;
    x.zp.push_back(zLocal++);
    x.fontSize = gs.fontSize;
    for (bool b : actualMarks) x.actualText = x.actualText || b;
    return x;
  }

  bool mcidHasActualText(int mcid) {
    if (!parentTreeLoaded) {
      parentTreeLoaded = true;
      try {
        QPDFObjectHandle root = ev.root;
        QPDFObjectHandle str = root.isDictionary() ? root.getKey("/StructTreeRoot") : QPDFObjectHandle();
        QPDFObjectHandle pt = str.isDictionary() ? str.getKey("/ParentTree") : QPDFObjectHandle();
        QPDFObjectHandle pg;
        if (root.getOwningQPDF()) {
          std::vector<QPDFObjectHandle> all = root.getOwningQPDF()->getAllPages();
          if (page >= 1 && page <= static_cast<int>(all.size())) pg = all[page - 1];
        }
        if (pt.isDictionary() && pg.isDictionary() && pg.getKey("/StructParents").isInteger()) {
          QPDFNumberTreeObjectHelper nt(pt, *root.getOwningQPDF());
          QPDFObjectHandle v;
          if (nt.findObject(pg.getKey("/StructParents").getIntValue(), v)) parentTreeEntry = v;
        }
      } catch (...) {
      }
    }
    if (!parentTreeEntry.isArray() || mcid < 0 || mcid >= parentTreeEntry.getArrayNItems()) return false;
    QPDFObjectHandle el = parentTreeEntry.getArrayItem(mcid);
    for (int i = 0; i < 64 && el.isDictionary(); ++i) {
      if (el.getKey("/ActualText").isString()) return true;
      el = el.getKey("/P");
    }
    return false;
  }

  EvScanner(QPDFObjectHandle resources, Events& events, int pageNum, const Gs& initial)
      : gs(initial), res(resources), ev(events), page(pageNum) {}

  void addPt(double x, double y) {
    double tx = gs.ctm.a * x + gs.ctm.c * y + gs.ctm.e;
    double ty = gs.ctm.b * x + gs.ctm.d * y + gs.ctm.f;
    if (!pathBox.valid) {
      pathBox = {tx, ty, tx, ty, true};
    } else {
      pathBox.x0 = std::min(pathBox.x0, tx);
      pathBox.y0 = std::min(pathBox.y0, ty);
      pathBox.x1 = std::max(pathBox.x1, tx);
      pathBox.y1 = std::max(pathBox.y1, ty);
    }
  }

  void takePts(size_t n) {
    size_t sz = nums.size();
    if (sz < n) return;
    for (size_t i = sz - n; i + 1 < sz; i += 2) addPt(nums[i], nums[i + 1]);
    ++pathNodes;
  }

  void applyExtGState() {
    if (lastName.empty() || !res.isDictionary()) return;
    QPDFObjectHandle egs = res.getKey("/ExtGState");
    if (!egs.isDictionary()) return;
    QPDFObjectHandle g = egs.getKey(lastName);
    if (!g.isDictionary()) return;
    if (g.getKey("/LW").isNumber()) gs.lineWidth = g.getKey("/LW").getNumericValue();
    if (g.getKey("/LC").isNumber()) gs.x.lineCap = static_cast<int>(g.getKey("/LC").getNumericValue());
    if (g.getKey("/LJ").isNumber()) gs.x.lineJoin = static_cast<int>(g.getKey("/LJ").getNumericValue());
    if (g.getKey("/ML").isNumber()) gs.x.miterLimit = g.getKey("/ML").getNumericValue();
    if (g.getKey("/SA").isBool()) gs.x.strokeAdjust = g.getKey("/SA").getBoolValue();
    if (g.getKey("/AIS").isBool()) gs.x.ais = g.getKey("/AIS").getBoolValue();
    if (g.getKey("/TK").isBool()) gs.x.tk = g.getKey("/TK").getBoolValue();
    if (!g.getKey("/RI").isNull()) gs.x.riExplicit = true;
    if (g.getKey("/RI").isName()) gs.x.intent = g.getKey("/RI").getName().substr(1);
    if (!g.getKey("/HTP").isNull()) gs.x.hasHTP = true;
    if (g.getKey("/UseBlackPtComp").isName()) gs.x.bpcValue = g.getKey("/UseBlackPtComp").getName().substr(1);
    {
      QPDFObjectHandle dd = g.getKey("/D");
      if (dd.isArray() && dd.getArrayNItems() == 2 && dd.getArrayItem(0).isArray()) {
        gs.x.dash.clear();
        QPDFObjectHandle da = dd.getArrayItem(0);
        for (int i = 0; i < da.getArrayNItems() && i < 64; ++i) gs.x.dash.push_back(numOf(da.getArrayItem(i), 0));
        gs.x.dashPhase = numOf(dd.getArrayItem(1), 0);
      }
    }
    if (!g.getKey("/BG").isNull()) gs.x.hasBG = true;
    if (!g.getKey("/BG2").isNull()) {
      gs.x.hasBG2 = true;
      gs.x.bg2Default = nameIs(g.getKey("/BG2"), "/Default");
    }
    if (!g.getKey("/UCR").isNull()) gs.x.hasUCR = true;
    if (!g.getKey("/UCR2").isNull()) {
      gs.x.hasUCR2 = true;
      gs.x.ucr2Default = nameIs(g.getKey("/UCR2"), "/Default");
    }
    if (!g.getKey("/TR").isNull()) {
      gs.x.hasTR = true;
      gs.x.trIdentity = nameIs(g.getKey("/TR"), "/Identity");
    }
    if (!g.getKey("/HT").isNull()) gs.x.ht = nameIs(g.getKey("/HT"), "/Default") ? QPDFObjectHandle() : g.getKey("/HT");
    if (!g.getKey("/BM").isNull() || !g.getKey("/CA").isNull() || !g.getKey("/ca").isNull()) gs.x.transKeys = true;
    if (g.getKey("/OP").isBool()) gs.overprintStroke = g.getKey("/OP").getBoolValue();
    if (g.getKey("/op").isBool()) gs.overprintFill = g.getKey("/op").getBoolValue();
    else if (g.getKey("/OP").isBool()) gs.overprintFill = g.getKey("/OP").getBoolValue();
    if (g.getKey("/OPM").isInteger()) gs.opm = static_cast<int>(g.getKey("/OPM").getIntValue());
    bool tr = false;
    if (g.getKey("/CA").isNumber()) {
      gs.x.alphaStroke = g.getKey("/CA").getNumericValue();
      if (gs.x.alphaStroke < 1.0) tr = true;
    }
    if (g.getKey("/ca").isNumber()) {
      gs.x.alphaFill = g.getKey("/ca").getNumericValue();
      if (gs.x.alphaFill < 1.0) tr = true;
    }
    if (g.getKey("/FL").isNumber()) gs.x.flatness = g.getKey("/FL").getNumericValue();
    QPDFObjectHandle tr2 = g.getKey("/TR2");
    if (!tr2.isNull()) {
      gs.x.hasTR2 = true;
      gs.x.tr2IsDefault = nameIs(tr2, "/Default");
      gs.x.tr2Identity = nameIs(tr2, "/Identity");
    }
    if (!g.getKey("/UseBlackPtComp").isNull() || !g.getKey("/BPC").isNull()) {
      gs.x.hasBPC = true;
    }
    QPDFObjectHandle ht = g.getKey("/HT");
    if (!g.getKey("/HTO").isNull() ||
        (ht.isDictionary() && !ht.getKey("/HalftoneOrigin").isNull())) {
      gs.x.hasHalftoneOrigin = true;
    }
    QPDFObjectHandle sm = g.getKey("/SMask");
    if (nameIs(sm, "/None")) {
      gs.x.smaskExplicitNone = true;
      gs.smaskActive = false;
      gs.x.hasSMask = false;
      gs.x.smaskIsLuminosity = gs.x.smaskIsAlpha = gs.x.smaskHasBC = gs.x.smaskHasTR = gs.x.smaskTRIdentity = false;
    }
    if (!sm.isNull() && !nameIs(sm, "/None")) {
      tr = true;
      gs.smaskActive = true;
      gs.x.hasSMask = true;
      if (sm.isDictionary()) {
        gs.x.smaskIsLuminosity = nameIs(sm.getKey("/S"), "/Luminosity");
        gs.x.smaskIsAlpha = nameIs(sm.getKey("/S"), "/Alpha");
        gs.x.smaskHasBC = sm.getKey("/BC").isArray();
        gs.x.smaskHasTR = !sm.getKey("/TR").isNull();
        gs.x.smaskTRIdentity = nameIs(sm.getKey("/TR"), "/Identity");
        QPDFObjectHandle gstream = sm.getKey("/G");
        if (gstream.isStream()) {
          QPDFObjectHandle grp = gstream.getDict().getKey("/Group");
          if (grp.isDictionary()) {
            QPDFObjectHandle gcs = grp.getKey("/CS");
            std::string cn = gcs.isName() ? gcs.getName()
                             : (gcs.isArray() && gcs.getArrayNItems() > 0)
                                 ? nameOf(gcs.getArrayItem(0)) : std::string();
            if (cn.size() > 1) gs.x.smaskGroupCS = cn.substr(1);
          }
        }
      }
    }
    QPDFObjectHandle bm = g.getKey("/BM");
    std::string bmName = nameOf(bm);
    if (bm.isArray() && bm.getArrayNItems() > 0) bmName = nameOf(bm.getArrayItem(0));
    if (!bmName.empty()) {
      gs.x.blendMode = bmName.substr(1);
      if (bmName != "/Normal" && bmName != "/Compatible") tr = true;
    }
    bool blendTransparent = !gs.x.blendMode.empty() && gs.x.blendMode != "Normal" &&
                            gs.x.blendMode != "Compatible";
    gs.transparency = tr || gs.smaskActive || blendTransparent || gs.x.alphaFill < 1.0 ||
                      gs.x.alphaStroke < 1.0;
  }

  void setSpace(bool stroke) {
    if (lastName.empty()) return;
    ColorInfo ci = classifyColor(QPDFObjectHandle::newName(lastName), res);
    if (stroke) gs.strokeColor = ci;
    else gs.fillColor = ci;
  }

  void applyPendingClip() {
    if (!pathIsClip || !pathBox.valid) return;
    if (!gs.clip.valid) {
      gs.clip = pathBox;
    } else {
      gs.clip.x0 = std::max(gs.clip.x0, pathBox.x0);
      gs.clip.y0 = std::max(gs.clip.y0, pathBox.y0);
      gs.clip.x1 = std::min(gs.clip.x1, pathBox.x1);
      gs.clip.y1 = std::min(gs.clip.y1, pathBox.y1);
    }
  }

  void addPaint(bool stroke, bool fill) {
    if (std::getenv("KURA_EV_DEBUG") && (fill || stroke)) {
      std::string c;
      for (double v : (fill ? gs.fill : gs.stroke)) c += std::to_string(v).substr(0, 4) + " ";
      std::fprintf(stderr, "[ev] %s cls=%s comps=[%s] opF=%d opS=%d\n",
                   fill ? "fill" : "stroke", (fill ? gs.fillColor : gs.strokeColor).cls.c_str(),
                   c.c_str(), gs.overprintFill ? 1 : 0, gs.overprintStroke ? 1 : 0);
    }
    if (stroke) {
      PaintEvent e;
      e.width = gs.lineWidth * matScale(gs.ctm);
      e.comps = gs.stroke;
      e.page = page;
      e.stroke = true;
      e.color = gs.strokeColor;
      e.overprint = gs.overprintStroke;
      e.opm = gs.opm;
      e.transparency = gs.transparency;
      e.x = snap();
      e.bbox = pathBox;
      e.clip = gs.clip;
      e.pathNodes = pathNodes;
      fillPathStats(e);
      patternShading(gs.strokePattern, e);
      recordSpot(e.color);
      ev.paints.push_back(e);
    }
    if (fill) {
      PaintEvent e;
      e.comps = gs.fill;
      e.page = page;
      e.fillOp = true;
      e.color = gs.fillColor;
      recordSpot(e.color);
      e.overprint = gs.overprintFill;
      e.opm = gs.opm;
      e.transparency = gs.transparency;
      e.x = snap();
      e.bbox = pathBox;
      e.clip = gs.clip;
      e.pathNodes = pathNodes;
      fillPathStats(e);
      patternShading(gs.fillPattern, e);
      ev.paints.push_back(e);
    }
    applyPendingClip();
    pathBox = Box();
    pathNodes = 0;
    pathIsClip = false;
    ps = PathStat();
  }

  void patternShading(const std::string& name, PaintEvent& e) {
    if (name.empty() || !res.isDictionary()) return;
    QPDFObjectHandle pd = res.getKey("/Pattern");
    QPDFObjectHandle pat = pd.isDictionary() ? pd.getKey(name) : QPDFObjectHandle();
    if (!pat.isDictionary() || pat.isStream()) return;
    QPDFObjectHandle sh = pat.getKey("/Shading");
    QPDFObjectHandle sd = sh.isStream() ? sh.getDict() : sh;
    if (!sd.isDictionary()) return;
    e.shading = sh;
    e.shadingType = static_cast<int>(numOf(sd.getKey("/ShadingType"), 0));
  }

  void recordFont(QPDFObjectHandle fnt) {
    if (!fnt.isDictionary()) return;
    if (fnt.isIndirect()) {
      QPDFObjGen og = fnt.getObjGen();
      for (const FontFacts& f : ev.fonts) {
        if (f.og == og) return;
      }
    } else {
      std::string sig = nameOf(fnt.getKey("/Subtype")) + "|" + nameOf(fnt.getKey("/BaseFont")) +
                        "|" + nameOf(fnt.getKey("/Encoding"));
      if (!directFontsSeen.insert(sig).second) return;
    }
    QPDFObjGen og = fnt.isIndirect() ? fnt.getObjGen() : QPDFObjGen();
    FontFacts ff;
    ff.og = og;
    ff.dict = fnt;
    std::string bf = nameOf(fnt.getKey("/BaseFont"));
    if (bf.size() > 1) ff.baseFont = bf.substr(1);
    std::string st = nameOf(fnt.getKey("/Subtype"));
    if (st.size() > 1) ff.subtype = st.substr(1);
    ff.type3 = ff.subtype == "Type3";
    ff.trueType = ff.subtype == "TrueType";
    QPDFObjectHandle target = fnt;
    if (ff.subtype == "Type0") {
      QPDFObjectHandle df = fnt.getKey("/DescendantFonts");
      if (df.isArray() && df.getArrayNItems() > 0 && df.getArrayItem(0).isDictionary()) {
        target = df.getArrayItem(0);
        ff.cid = true;
        std::string dst = nameOf(target.getKey("/Subtype"));
        if (dst == "/CIDFontType0") ff.cid0 = true;
        if (dst == "/CIDFontType2") {
          ff.trueType = true;
          QPDFObjectHandle c2g = target.getKey("/CIDToGIDMap");
          ff.hasCIDToGIDMap = nameIs(c2g, "/Identity") || c2g.isStream();
        }
      }
    }
    QPDFObjectHandle desc = target.getKey("/FontDescriptor");
    if (desc.isDictionary()) {
      for (const char* k : {"/FontFile", "/FontFile2", "/FontFile3"}) {
        QPDFObjectHandle ffs = desc.getKey(k);
        if (ffs.isStream()) {
          ff.embedded = true;
          try {
            if (ffs.isIndirect()) {
              auto hit = programCache.find(ffs.getObjGen());
              if (hit != programCache.end()) {
                ff.fontProgram = hit->second;
                ff.openType = ff.fontProgram.rfind("OTTO", 0) == 0 ||
                              nameIs(ffs.getDict().getKey("/Subtype"), "/OpenType");
                break;
              }
            }
            auto buf = ffs.getStreamData(qpdf_dl_all);
            ff.fontProgram.assign(reinterpret_cast<const char*>(buf->getBuffer()),
                                  buf->getSize());
            if (ffs.isIndirect() && programCache.size() < 512) {
              programCache[ffs.getObjGen()] = ff.fontProgram;
            }
            if (ff.fontProgram.rfind("OTTO", 0) == 0 ||
                nameIs(ffs.getDict().getKey("/Subtype"), "/OpenType")) {
              ff.openType = true;
            }
          } catch (...) {
            ff.ftValid = false;
          }
          break;
        }
      }
      QPDFObjectHandle flags = desc.getKey("/Flags");
      if (flags.isInteger()) {
        ff.hasFlags = true;
        ff.symbolic = (flags.getIntValue() & 4) != 0;
      }
      QPDFObjectHandle asc = desc.getKey("/Ascent");
      QPDFObjectHandle dsc = desc.getKey("/Descent");
      if (asc.isNumber() && dsc.isNumber()) {
        double av = asc.getNumericValue() / 1000.0;
        double dv = dsc.getNumericValue() / 1000.0;
        if (av > 0 && av < 2 && dv > -1 && dv <= 0) {
          ff.ascentEm = av;
          ff.descentEm = dv;
          ff.hasVMetrics = true;
        }
      }
    }
    ff.hasToUnicode = fnt.getKey("/ToUnicode").isStream();
    QPDFObjectHandle enc = fnt.getKey("/Encoding");
    if (enc.isDictionary()) {
      ff.hasEncodingDict = true;
      std::string be = nameOf(enc.getKey("/BaseEncoding"));
      if (be.size() > 1) ff.encodingName = be.substr(1);
      else if (enc.getKey("/Differences").isArray()) ff.encodingName = "Differences";
    } else if (enc.isName() && enc.getName().size() > 1) {
      ff.encodingName = enc.getName().substr(1);
    }
    ff.subsetName = ff.baseFont.size() > 7 && ff.baseFont[6] == '+';
    QPDFObjectHandle fc = target.getKey("/FirstChar");
    if (fc.isInteger()) ff.firstChar = static_cast<int>(fc.getIntValue());
    QPDFObjectHandle wd = target.getKey("/Widths");
    if (wd.isArray()) {
      for (int i = 0; i < wd.getArrayNItems(); ++i) {
        ff.widths.push_back(numOf(wd.getArrayItem(i), -1));
      }
    }
    if (ff.type3) ff.embedded = fnt.getKey("/CharProcs").isDictionary();
    ev.fonts.push_back(ff);
  }

  void recordSpot(const ColorInfo& ci) {
    if ((ci.cls == "separation" || ci.cls == "devicen") && !ci.spot.empty() &&
        ci.spot != "All" && ci.spot != "None" && ci.spot != "Registration" &&
        ci.spot != "Cyan" && ci.spot != "Magenta" && ci.spot != "Yellow" &&
        ci.spot != "Black" && ci.spot != "Gray" && ci.spot != "Grey") {
      ev.spotPlates.insert(ci.spot);
      if (!ci.altName.empty()) ev.spotAlternates[ci.spot].insert(ci.altName);
    }
  }

  void setColor(bool stroke, int n) {
    std::vector<double> c;
    size_t sz = nums.size();
    for (int i = n; i >= 1; --i) c.push_back(sz >= static_cast<size_t>(i) ? nums[sz - i] : 0);
    if (stroke) gs.stroke = c;
    else gs.fill = c;
  }

  std::string lastString;
  std::vector<double> dashNums;
  bool inBI = false;
  int biTokens = 0;
  std::vector<std::string> biNames;
  std::vector<double> biNums;
  std::string biCsName;

  void resetInlineImage() {
    inBI = false;
    biTokens = 0;
    biW = biH = biBpc = 0;
    biMask = false;
    biFiltered = false;
    biCsName.clear();
    biLastKey.clear();
  }

  std::vector<bool> ocMarks;
  std::vector<std::string> opNames;
  QPDFObjectHandle opDict;

  bool hiddenNow() const {
    if (gs.ocHidden) return true;
    for (bool b : ocMarks) {
      if (b) return true;
    }
    return false;
  }

  bool ocgHidden(QPDFObjectHandle g) const {
    QPDFObjectHandle ocp = ev.root.isDictionary() ? ev.root.getKey("/OCProperties") : QPDFObjectHandle();
    QPDFObjectHandle d = ocp.isDictionary() ? ocp.getKey("/D") : QPDFObjectHandle();
    if (!d.isDictionary() || !g.isIndirect()) return false;
    auto in = [&](const char* key) {
      QPDFObjectHandle arr = d.getKey(key);
      for (int i = 0; arr.isArray() && i < arr.getArrayNItems(); ++i) {
        if (arr.getArrayItem(i).isIndirect() && arr.getArrayItem(i).getObjGen() == g.getObjGen()) return true;
      }
      return false;
    };
    if (in("/OFF")) return true;
    return nameIs(d.getKey("/BaseState"), "/OFF") && !in("/ON");
  }

  bool ocHiddenFor(QPDFObjectHandle oc) const {
    if (!oc.isDictionary()) return false;
    if (nameIs(oc.getKey("/Type"), "/OCMD")) {
      QPDFObjectHandle ocgs = oc.getKey("/OCGs");
      std::vector<QPDFObjectHandle> list;
      if (ocgs.isDictionary()) list.push_back(ocgs);
      for (int i = 0; ocgs.isArray() && i < ocgs.getArrayNItems(); ++i) list.push_back(ocgs.getArrayItem(i));
      if (list.empty()) return false;
      std::string p = nameOf(oc.getKey("/P"));
      int on = 0;
      for (auto& g : list) on += !ocgHidden(g);
      int n = static_cast<int>(list.size());
      if (p == "/AllOn") return on != n;
      if (p == "/AnyOff") return on == n;
      if (p == "/AllOff") return on != 0;
      return on == 0;
    }
    return ocgHidden(oc);
  }

  void handleObject(QPDFObjectHandle obj, size_t a, size_t b) override {
    if (!obj.isInlineImage() && obj.isOperator()) {
      std::string op = obj.getOperatorValue();
      if (op == "BDC" || op == "BMC") {
        bool hide = false;
        if (op == "BDC" && !opNames.empty() && opNames.front() == "/OC") {
          QPDFObjectHandle oc = opDict;
          if (!oc.isDictionary() && opNames.size() >= 2) {
            QPDFObjectHandle props = res.isDictionary() ? res.getKey("/Properties") : QPDFObjectHandle();
            oc = props.isDictionary() ? props.getKey(opNames.back()) : QPDFObjectHandle();
          }
          hide = ocHiddenFor(oc);
        }
        ocMarks.push_back(hide);
        bool at = false;
        if (op == "BDC") {
          QPDFObjectHandle pd = opDict;
          if (!pd.isDictionary() && opNames.size() >= 2) {
            QPDFObjectHandle props = res.isDictionary() ? res.getKey("/Properties") : QPDFObjectHandle();
            pd = props.isDictionary() ? props.getKey(opNames.back()) : QPDFObjectHandle();
          }
          if (pd.isDictionary() && pd.getKey("/AF").isArray()) {
            QPDFObjectHandle af = pd.getKey("/AF");
            for (int i = 0; i < af.getArrayNItems() && ev.bdcFiles.size() < 100000; ++i) {
              if (af.getArrayItem(i).isIndirect()) ev.bdcFiles.insert(af.getArrayItem(i).getObjGen());
              else if (af.getArrayItem(i).isDictionary()) ++ev.bdcDirectFiles;
            }
          }
          if (pd.isDictionary()) {
            at = pd.getKey("/ActualText").isString() || (pd.getKey("/MCID").isInteger() && mcidHasActualText(static_cast<int>(pd.getKey("/MCID").getIntValue())));
          }
        }
        actualMarks.push_back(at);
      } else if (op == "EMC" && !ocMarks.empty()) {
        ocMarks.pop_back();
        if (!actualMarks.empty()) actualMarks.pop_back();
      }
    } else if (obj.isName()) {
      opNames.push_back(obj.getName());
    } else if (obj.isDictionary()) {
      opDict = obj;
    }
    size_t np = ev.paints.size(), nt = ev.texts.size(), ni = ev.images.size();
    handleObjectImpl(obj, a, b);
    if (ev.trackSrc) {
      std::vector<size_t> src = ev.srcPath;
      src.push_back(srcOk ? a : SIZE_MAX);
      for (size_t i = np; i < ev.paints.size(); ++i) ev.paints[i].src = src;
      for (size_t i = nt; i < ev.texts.size(); ++i) ev.texts[i].src = src;
      for (size_t i = ni; i < ev.images.size(); ++i) ev.images[i].src = src;
      drawOffs.resize(draws.size(), srcOk ? a : SIZE_MAX);
    }
    if (hiddenNow()) {
      for (size_t i = np; i < ev.paints.size(); ++i) ev.paints[i].layerHidden = true;
      for (size_t i = nt; i < ev.texts.size(); ++i) ev.texts[i].layerHidden = true;
      for (size_t i = ni; i < ev.images.size(); ++i) ev.images[i].layerHidden = true;
    }
    if (obj.isOperator()) {
      opNames.clear();
      opDict = QPDFObjectHandle();
    }
  }

  void handleObjectImpl(QPDFObjectHandle obj, size_t, size_t) {
    if (obj.isInlineImage()) {
      ImageEvent e;
      e.page = page;
      for (size_t i = 0; i + 1 <= biNames.size(); ++i) {
      }
      e.width = biW;
      e.height = biH;
      e.bpc = biBpc > 0 ? biBpc : 8;
      e.mask = biMask;
      if (biMask) {
        e.color = gs.fillColor;
        e.comps = gs.fill;
        e.overprint = gs.overprintFill;
        e.opm = gs.opm;
      } else if (!biCsName.empty()) {
        e.color = classifyColor(QPDFObjectHandle::newName(biCsName), res);
      }
      double wpt = std::hypot(gs.ctm.a, gs.ctm.b);
      double hpt = std::hypot(gs.ctm.c, gs.ctm.d);
      if (e.width > 0 && e.height > 0 && wpt > 0.01 && hpt > 0.01) {
        e.ppi = std::min(e.width * 72.0 / wpt, e.height * 72.0 / hpt);
      }
      {
        const Mat& m = gs.ctm;
        double xs[4] = {m.e, m.a + m.e, m.c + m.e, m.a + m.c + m.e};
        double ys[4] = {m.f, m.b + m.f, m.d + m.f, m.b + m.d + m.f};
        e.bbox = {*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4),
                  *std::max_element(xs, xs + 4), *std::max_element(ys, ys + 4), true};
      }
      e.clip = gs.clip;
      e.x = snap();
      e.inlineImg = true;
      e.inlineSize = obj.getInlineImageValue().size();
      if (!biFiltered && !biMask && e.bpc == 8 && e.inlineSize <= (1u << 22)) e.inlineSamples = obj.getInlineImageValue();
      e.transparency = gs.transparency;
      ev.images.push_back(e);
      resetInlineImage();
      return;
    }
    if (!obj.isOperator()) {
      if (inBI && ++biTokens > 64) resetInlineImage();
      if (inBI) {
        if (obj.isName()) {
          std::string n = obj.getName();
          if (biLastKey.empty() && (n == "/W" || n == "/Width" || n == "/H" ||
                                    n == "/Height" || n == "/BPC" ||
                                    n == "/BitsPerComponent" || n == "/CS" ||
                                    n == "/ColorSpace" || n == "/IM" || n == "/ImageMask" ||
                                    n == "/F" || n == "/Filter" || n == "/D" || n == "/DP")) {
            biLastKey = n;
            if (n == "/F" || n == "/Filter") biFiltered = true;
          } else if (!biLastKey.empty()) {
            if (biLastKey == "/CS" || biLastKey == "/ColorSpace") biCsName = n;
            biLastKey.clear();
          }
          return;
        }
        if (obj.isNumber() && !biLastKey.empty()) {
          double v = obj.getNumericValue();
          if (biLastKey == "/W" || biLastKey == "/Width") biW = static_cast<int>(v);
          else if (biLastKey == "/H" || biLastKey == "/Height") biH = static_cast<int>(v);
          else if (biLastKey == "/BPC" || biLastKey == "/BitsPerComponent") {
            biBpc = static_cast<int>(v);
          }
          biLastKey.clear();
          return;
        }
        if (obj.isBool() && (biLastKey == "/IM" || biLastKey == "/ImageMask")) {
          biMask = obj.getBoolValue();
          biLastKey.clear();
          return;
        }
        biLastKey.clear();
      }
      if (obj.isName()) lastName = obj.getName();
      else if (obj.isNumber()) nums.push_back(obj.getNumericValue());
      else if (obj.isString()) {
        lastString += obj.getStringValue();
        ev.maxContentString = std::max(ev.maxContentString, obj.getStringValue().size());
      }
      else if (obj.isArray()) {
        dashNums.clear();
        for (int i = 0; i < obj.getArrayNItems(); ++i) {
          if (obj.getArrayItem(i).isString()) {
            lastString += obj.getArrayItem(i).getStringValue();
            ev.maxContentString = std::max(ev.maxContentString, obj.getArrayItem(i).getStringValue().size());
          }
          if (obj.getArrayItem(i).isNumber() && dashNums.size() < 64) dashNums.push_back(obj.getArrayItem(i).getNumericValue());
        }
      }
      return;
    }
    std::string op = obj.getOperatorValue();
    if (ev.contentOps.size() < 512) ev.contentOps.insert(op);
    if (op == "BI") {
      inBI = true;
      biTokens = 0;
      biLastKey.clear();
      nums.clear();
      lastName.clear();
      return;
    }
    if (op == "ID" && inBI) return;
    if (inBI) resetInlineImage();
    if (op == "q") {
      if (stack.size() < 256) stack.push_back(gs);
      else ++qSuppressed;
      ++gs.x.qDepth;
    }
    else if (op == "Q") {
      if (qSuppressed > 0) {
        --qSuppressed;
      } else if (!stack.empty()) {
        gs = stack.back();
        stack.pop_back();
      }
    } else if (op == "cm" && nums.size() >= 6) {
      size_t n = nums.size();
      Mat m{nums[n - 6], nums[n - 5], nums[n - 4], nums[n - 3], nums[n - 2], nums[n - 1]};
      gs.ctm = mul(m, gs.ctm);
    } else if (op == "w" && !nums.empty()) {
      gs.lineWidth = nums.back();
    } else if (op == "J" && !nums.empty()) {
      gs.x.lineCap = static_cast<int>(nums.back());
    } else if (op == "j" && !nums.empty()) {
      gs.x.lineJoin = static_cast<int>(nums.back());
    } else if (op == "M" && !nums.empty()) {
      gs.x.miterLimit = nums.back();
    } else if (op == "d") {
      gs.x.dash = dashNums;
      gs.x.dashPhase = nums.empty() ? 0 : nums.back();
    } else if (op == "ri") {
      gs.x.riExplicit = true;
      if (lastName.size() > 1) gs.x.intent = lastName.substr(1);
    } else if (op == "Tc" && !nums.empty()) {
      gs.x.charSpacing = nums.back();
    } else if (op == "Tw" && !nums.empty()) {
      gs.x.wordSpacing = nums.back();
    } else if (op == "Tz" && !nums.empty()) {
      gs.x.hScale = nums.back();
    } else if (op == "TL" && !nums.empty()) {
      gs.x.leading = nums.back();
    } else if (op == "Ts" && !nums.empty()) {
      gs.x.rise = nums.back();
    } else if (op == "BX") {
      ++bxDepth;
    } else if (op == "EX") {
      if (bxDepth > 0) --bxDepth;
    } else if (op == "Tr" && !nums.empty()) {
      gs.renderMode = static_cast<int>(nums.back());
    } else if (op == "BT") {
      gs.tmScale = 1.0;
      tmX = 0;
      tmY = 0;
      tlm = Mat();
      gs.x.tm = Mat();
    } else if (op == "Tm" && nums.size() >= 6) {
      size_t n = nums.size();
      tlm = Mat{nums[n - 6], nums[n - 5], nums[n - 4], nums[n - 3], nums[n - 2], nums[n - 1]};
      gs.x.tm = tlm;
      gs.tmScale = (std::hypot(nums[n - 6], nums[n - 5]) +
                    std::hypot(nums[n - 4], nums[n - 3])) / 2.0;
      tmX = nums[n - 2];
      tmY = nums[n - 1];
    } else if ((op == "Td" || op == "TD") && nums.size() >= 2) {
      tmX += nums[nums.size() - 2];
      tmY += nums[nums.size() - 1];
      if (op == "TD") gs.x.leading = -nums[nums.size() - 1];
      tlm = mul(Mat{1, 0, 0, 1, nums[nums.size() - 2], nums[nums.size() - 1]}, tlm);
      gs.x.tm = tlm;
    } else if ((op == "m" || op == "l") && nums.size() >= 2) {
      size_t n = nums.size();
      if (op == "m") moveTo(nums[n - 2], nums[n - 1]);
      else lineTo(nums[n - 2], nums[n - 1]);
      takePts(2);
    } else if (op == "c" && nums.size() >= 6) {
      size_t n = nums.size();
      curveTo(dev(nums[n - 6], nums[n - 5]), dev(nums[n - 4], nums[n - 3]), dev(nums[n - 2], nums[n - 1]));
      takePts(6);
    } else if ((op == "v" || op == "y") && nums.size() >= 4) {
      size_t n = nums.size();
      auto p1 = dev(nums[n - 4], nums[n - 3]), p2 = dev(nums[n - 2], nums[n - 1]);
      if (op == "v") curveTo({ps.cx, ps.cy}, p1, p2);
      else curveTo(p1, p2, p2);
      takePts(4);
    } else if (op == "h") {
      closeSub();
    } else if (op == "re" && nums.size() >= 4) {
      size_t n = nums.size();
      rectStat(nums[n - 4], nums[n - 3], nums[n - 2], nums[n - 1]);
      addPt(nums[n - 4], nums[n - 3]);
      addPt(nums[n - 4] + nums[n - 2], nums[n - 3] + nums[n - 1]);
      ++pathNodes;
    } else if (op == "W" || op == "W*") {
      pathIsClip = true;
    } else if (op == "n") {
      applyPendingClip();
      if (pathNodes > 0 && !pathIsClip) {
        PaintEvent e;
        e.page = page;
        e.noPaint = true;
        e.pathNodes = pathNodes;
        e.comps = gs.fill;
        e.color = gs.fillColor;
        e.x = snap();
        e.bbox = pathBox;
        fillPathStats(e);
        ev.paints.push_back(e);
      }
      pathBox = Box();
      pathNodes = 0;
      pathIsClip = false;
      ps = PathStat();
    } else if (op == "sh") {
      PaintEvent e;
      e.page = page;
      e.shade = true;
      e.fillOp = true;
      e.comps = gs.fill;
      e.color = gs.fillColor;
      e.overprint = gs.overprintFill;
      e.opm = gs.opm;
      e.transparency = gs.transparency;
      e.x = snap();
      if (!lastName.empty() && res.isDictionary() && res.getKey("/Shading").isDictionary()) {
        QPDFObjectHandle sh = res.getKey("/Shading").getKey(lastName);
        QPDFObjectHandle sd = sh.isStream() ? sh.getDict() : sh;
        if (sd.isDictionary()) {
          e.shading = sh;
          e.shadingType = static_cast<int>(numOf(sd.getKey("/ShadingType"), 0));
        }
      }
      ev.paints.push_back(e);
    } else if (op == "i" && !nums.empty()) {
      gs.x.flatness = nums.back();
    } else if (op == "Tf" && !lastName.empty() && res.isDictionary()) {
      gs.fontSize = nums.empty() ? gs.fontSize : std::fabs(nums.back());
      if (!nums.empty()) gs.x.negFontSize = nums.back() < 0;
      QPDFObjectHandle fd = res.getKey("/Font");
      if (fd.isDictionary()) {
        QPDFObjectHandle fnt = fd.getKey(lastName);
        if (fnt.isDictionary()) {
          std::string bf = nameOf(fnt.getKey("/BaseFont"));
          if (bf.size() > 1) {
            ev.baseFonts.insert(bf.substr(1));
            gs.fontName = bf.substr(1);
          }
          gs.fontOg = fnt.isIndirect() ? fnt.getObjGen() : QPDFObjGen();
          curFont = fnt;
        }
      }
    } else if (op == "gs") {
      applyExtGState();
    } else if (op == "g" || op == "G") {
      setColor(op == "G", 1);
      (op == "G" ? gs.strokeColor : gs.fillColor) = deviceColor("gray", 1, res);
    } else if (op == "rg" || op == "RG") {
      setColor(op == "RG", 3);
      (op == "RG" ? gs.strokeColor : gs.fillColor) = deviceColor("rgb", 3, res);
    } else if (op == "k" || op == "K") {
      setColor(op == "K", 4);
      (op == "K" ? gs.strokeColor : gs.fillColor) = deviceColor("cmyk", 4, res);
    } else if (op == "cs" || op == "CS") {
      setSpace(op == "CS");
      (op == "CS" ? gs.strokePattern : gs.fillPattern).clear();
    } else if (op == "sc" || op == "scn" || op == "SC" || op == "SCN") {
      bool strokeOp = op == "SC" || op == "SCN";
      if (!nums.empty()) {
        const ColorInfo& ci = strokeOp ? gs.strokeColor : gs.fillColor;
        int want = static_cast<int>(nums.size());
        if (ci.declaredComps > 0 && ci.declaredComps < want) want = ci.declaredComps;
        if (want > 32) want = 32;
        setColor(strokeOp, want);
      }
      if ((op == "scn" || op == "SCN") && !lastName.empty()) {
        const ColorInfo& ci = op == "SCN" ? gs.strokeColor : gs.fillColor;
        if (ci.cls == "pattern") {
          patternUses.push_back({lastName, gs});
          (op == "SCN" ? gs.strokePattern : gs.fillPattern) = lastName;
        }
      }
    } else if (op == "S" || op == "s") {
      if (op == "s") closeSub();
      addPaint(true, false);
    } else if (op == "f" || op == "F" || op == "f*") {
      addPaint(false, true);
    } else if (op == "B" || op == "B*" || op == "b" || op == "b*") {
      if (op == "b" || op == "b*") closeSub();
      addPaint(true, true);
    } else if (op == "T*") {
      tmY -= gs.x.leading;
      tlm = mul(Mat{1, 0, 0, 1, 0, -gs.x.leading}, tlm);
      gs.x.tm = tlm;
    } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
      if (op == "\"" && nums.size() >= 2) {
        gs.x.wordSpacing = nums[nums.size() - 2];
        gs.x.charSpacing = nums[nums.size() - 1];
      }
      if (op == "'" || op == "\"") {
        tmY -= gs.x.leading;
        tlm = mul(Mat{1, 0, 0, 1, 0, -gs.x.leading}, tlm);
        gs.x.tm = tlm;
      }
      if (curFont.isInitialized() && curFont.isDictionary()) recordFont(curFont);
      if (curFont.isInitialized() && curFont.isDictionary() && nameIs(curFont.getKey("/Subtype"), "/Type3") &&
          type3Uses.size() < 64) {
        Gs at = gs;
        at.ctm = mul(tlm, gs.ctm);
        at.x.zp.push_back(zLocal++);
        type3Uses.push_back({curFont, at});
      }
      TextEvent e;
      e.sizePt = gs.fontSize * gs.tmScale * matScale(gs.ctm);
      e.renderMode = gs.renderMode;
      e.comps = gs.fill;
      e.page = page;
      e.color = gs.fillColor;
      e.overprint = gs.overprintFill;
      e.opm = gs.opm;
      e.transparency = gs.transparency;
      e.x = snap();
      e.clip = gs.clip;
      e.fontName = gs.fontName;
      e.fontOg = gs.fontOg;
      e.bytes = lastString.substr(0, 256);
      double tx = gs.ctm.a * tmX + gs.ctm.c * tmY + gs.ctm.e;
      double ty = gs.ctm.b * tmX + gs.ctm.d * tmY + gs.ctm.f;
      double sz = e.sizePt > 0 ? e.sizePt : 12;
      const FontFacts* ff = nullptr;
      for (const FontFacts& f : ev.fonts) {
        if (f.og == gs.fontOg) {
          ff = &f;
          break;
        }
      }
      double advEm = 0;
      if (ff && !ff->cid && !ff->widths.empty()) {
        for (unsigned char c : lastString) {
          int wi = static_cast<int>(c) - ff->firstChar;
          advEm += (wi >= 0 && wi < static_cast<int>(ff->widths.size()) &&
                    ff->widths[wi] >= 0)
                       ? ff->widths[wi] / 1000.0
                       : 0.5;
        }
      } else {
        int glyphs = ff && ff->cid ? static_cast<int>(lastString.size()) / 2
                                   : static_cast<int>(lastString.size());
        advEm = glyphs * 0.5;
      }
      double adv = advEm * sz;
      double asc = (ff ? ff->ascentEm : 0.80) * sz;
      double dsc = (ff ? ff->descentEm : -0.20) * sz;
      e.bbox = {std::min(tx, tx + adv), ty + dsc, std::max(tx, tx + adv), ty + asc,
                true};
      recordSpot(e.color);
      ev.texts.push_back(e);
    } else if (op == "Do" && !lastName.empty()) {
      Gs at = gs;
      at.ocHidden = hiddenNow();
      at.x.zp.push_back(zLocal++);
      draws.push_back({lastName, at});
    }
    nums.clear();
    lastName.clear();
    lastString.clear();
    dashNums.clear();
  }

  void handleEOF() override {}
};

void scanEvents(QPDFObjectHandle contents, QPDFObjectHandle res, const Gs& initial,
                int page, int depth, Visited& seen, Events& ev) {
  if (depth > kMaxContentNest) return;
  EvScanner scan(res, ev, page, initial);
  scan.srcOk = ev.trackSrc && ev.srcPath.size() == static_cast<size_t>(depth);
  try {
    QPDFObjectHandle::parseContentStream(contents, &scan);
  } catch (...) {
    return;
  }
  if (res.isDictionary()) {
    QPDFObjectHandle patd = res.getKey("/Pattern");
    for (const auto& pu : scan.patternUses) {
      if (!patd.isDictionary()) break;
      QPDFObjectHandle pat = patd.getKey(pu.first);
      if (!pat.isStream() || !seen.enter(pat)) continue;
      QPDFObjectHandle pd = pat.getDict();
      Gs inner = pu.second;
      inner.x.inPattern = true;
      inner.x.resInherited = !pd.getKey("/Resources").isDictionary();
      inner.fillColor = ColorInfo();
      inner.strokeColor = ColorInfo();
      QPDFObjectHandle mtx = pd.getKey("/Matrix");
      if (mtx.isArray() && mtx.getArrayNItems() == 6) {
        Mat m{numOf(mtx.getArrayItem(0), 1), numOf(mtx.getArrayItem(1), 0),
              numOf(mtx.getArrayItem(2), 0), numOf(mtx.getArrayItem(3), 1),
              numOf(mtx.getArrayItem(4), 0), numOf(mtx.getArrayItem(5), 0)};
        inner.ctm = mul(m, inner.ctm);
      }
      QPDFObjectHandle pres = pd.getKey("/Resources");
      scanEvents(pat, pres.isDictionary() ? pres : res, inner, page, depth + 1, seen, ev);
    }
    for (const auto& tu : scan.type3Uses) {
      QPDFObjectHandle cp = tu.first.getKey("/CharProcs");
      if (!cp.isDictionary() || !seen.enter(tu.first)) continue;
      QPDFObjectHandle fm = tu.first.getKey("/FontMatrix");
      Gs inner = tu.second;
      if (fm.isArray() && fm.getArrayNItems() == 6) {
        Mat m{numOf(fm.getArrayItem(0), 0.001), numOf(fm.getArrayItem(1), 0),
              numOf(fm.getArrayItem(2), 0), numOf(fm.getArrayItem(3), 0.001),
              numOf(fm.getArrayItem(4), 0), numOf(fm.getArrayItem(5), 0)};
        Mat scale{inner.fontSize, 0, 0, inner.fontSize, 0, 0};
        inner.ctm = mul(m, mul(scale, inner.ctm));
      }
      QPDFObjectHandle t3res = tu.first.getKey("/Resources");
      inner.x.inType3 = true;
      inner.x.resInherited = !t3res.isDictionary();
      int glyphs = 0;
      for (const std::string& gk : cp.getKeys()) {
        if (++glyphs > 40) break;
        QPDFObjectHandle proc = cp.getKey(gk);
        if (proc.isStream() && seen.enter(proc)) {
          scanEvents(proc, t3res.isDictionary() ? t3res : res, inner, page, depth + 1,
                     seen, ev);
        }
      }
    }
  }
  QPDFObjectHandle xod = res.isDictionary() ? res.getKey("/XObject")
                                            : QPDFObjectHandle::newNull();
  if (!xod.isDictionary()) return;
  for (const auto& d : scan.draws) {
    size_t drawIdx = static_cast<size_t>(&d - scan.draws.data());
    size_t drawOff = drawIdx < scan.drawOffs.size() ? scan.drawOffs[drawIdx] : SIZE_MAX;
    QPDFObjectHandle xo = xod.getKey(d.first);
    if (!xo.isStream()) continue;
    QPDFObjectHandle dict = xo.getDict();
    std::string sub = nameOf(dict.getKey("/Subtype"));
    if (sub == "/Image") {
      ImageEvent e;
      e.page = page;
      e.mask = (dict.getKey("/ImageMask").isBool() &&
                dict.getKey("/ImageMask").getBoolValue()) ||
               (dict.getKey("/ColorSpace").isNull() &&
                !dict.getKey("/BitsPerComponent").isInteger());
      e.bpc = dict.getKey("/BitsPerComponent").isInteger()
                  ? static_cast<int>(dict.getKey("/BitsPerComponent").getIntValue())
                  : (e.mask ? 1 : 8);
      QPDFObjectHandle filt = dict.getKey("/Filter");
      if (filt.isName()) e.filters.insert(filt.getName().substr(1));
      if (filt.isArray()) {
        for (int i = 0; i < filt.getArrayNItems(); ++i) {
          std::string n = nameOf(filt.getArrayItem(i));
          if (n.size() > 1) e.filters.insert(n.substr(1));
        }
      }
      int w = dict.getKey("/Width").isInteger()
                  ? static_cast<int>(dict.getKey("/Width").getIntValue()) : 0;
      int h = dict.getKey("/Height").isInteger()
                  ? static_cast<int>(dict.getKey("/Height").getIntValue()) : 0;
      e.width = w;
      e.height = h;
      e.hasSMask = dict.getKey("/SMask").isStream();
      e.interpolate = dict.getKey("/Interpolate").isBool() &&
                      dict.getKey("/Interpolate").getBoolValue();
      e.color = e.mask ? d.second.fillColor : classifyColor(dict.getKey("/ColorSpace"), res);
      if (e.mask) e.comps = d.second.fill;
      double wpt = std::hypot(d.second.ctm.a, d.second.ctm.b);
      double hpt = std::hypot(d.second.ctm.c, d.second.ctm.d);
      if (w > 0 && h > 0 && wpt > 0.01 && hpt > 0.01) {
        e.ppi = std::min(w * 72.0 / wpt, h * 72.0 / hpt);
      }
      {
        const Mat& m = d.second.ctm;
        double xs[4] = {m.e, m.a + m.e, m.c + m.e, m.a + m.c + m.e};
        double ys[4] = {m.f, m.b + m.f, m.d + m.f, m.b + m.d + m.f};
        e.bbox = {*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4),
                  *std::max_element(xs, xs + 4), *std::max_element(ys, ys + 4), true};
      }
      e.clip = d.second.clip;
      e.overprint = d.second.overprintFill;
      e.opm = d.second.opm;
      e.transparency = d.second.transparency;
      e.x = d.second.x;
      e.x.ctm = d.second.ctm;
      e.x.bxDepth = scan.bxDepth;
      e.obj = xo;
      if (ev.trackSrc) {
        e.src = ev.srcPath;
        e.src.push_back(drawOff);
      }
      ev.images.push_back(e);
    } else if (sub == "/Form") {
      if (!ev.formPath.insert(xo.getObjGen()).second) continue;
      if (++ev.formScans > 4096) {
        ev.formPath.erase(xo.getObjGen());
        continue;
      }
      Gs inner = d.second;
      inner.x.inForm = true;
      inner.x.formDict = dict;
      ++inner.x.formDepth;
      ev.maxFormNest = std::max(ev.maxFormNest, inner.x.formDepth);
      inner.x.resInherited = !dict.getKey("/Resources").isDictionary();
      inner.x.blendChain.push_back(inner.x.blendMode);
      QPDFObjectHandle fgrp = dict.getKey("/Group");
      QPDFObjectHandle fres = dict.getKey("/Resources");
      enterGroup(inner.x, fgrp, fres.isDictionary() ? fres : res);
      QPDFObjectHandle mtx = dict.getKey("/Matrix");
      if (mtx.isArray() && mtx.getArrayNItems() == 6) {
        Mat m{numOf(mtx.getArrayItem(0), 1), numOf(mtx.getArrayItem(1), 0),
              numOf(mtx.getArrayItem(2), 0), numOf(mtx.getArrayItem(3), 1),
              numOf(mtx.getArrayItem(4), 0), numOf(mtx.getArrayItem(5), 0)};
        inner.ctm = mul(m, inner.ctm);
      }
      QPDFObjectHandle fbox = dict.getKey("/BBox");
      if (fbox.isArray() && fbox.getArrayNItems() == 4) {
        double bx0 = numOf(fbox.getArrayItem(0), 0), by0 = numOf(fbox.getArrayItem(1), 0);
        double bx1 = numOf(fbox.getArrayItem(2), 0), by1 = numOf(fbox.getArrayItem(3), 0);
        const Mat& m = inner.ctm;
        double cx[4] = {bx0, bx1, bx0, bx1};
        double cy[4] = {by0, by0, by1, by1};
        double xs[4], ys[4];
        for (int i = 0; i < 4; ++i) {
          xs[i] = m.a * cx[i] + m.c * cy[i] + m.e;
          ys[i] = m.b * cx[i] + m.d * cy[i] + m.f;
        }
        Box bb{*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4),
               *std::max_element(xs, xs + 4), *std::max_element(ys, ys + 4), true};
        if (!inner.clip.valid) {
          inner.clip = bb;
        } else {
          inner.clip.x0 = std::max(inner.clip.x0, bb.x0);
          inner.clip.y0 = std::max(inner.clip.y0, bb.y0);
          inner.clip.x1 = std::min(inner.clip.x1, bb.x1);
          inner.clip.y1 = std::min(inner.clip.y1, bb.y1);
        }
      }
      QPDFObjectHandle sres = dict.getKey("/Resources");
      if (ev.trackSrc && (drawOff == SIZE_MAX || std::count(ev.srcPath.begin(), ev.srcPath.end(), SIZE_MAX))) {
        ev.foreignForms.insert(xo.getObjGen());
      }
      ev.srcPath.push_back(drawOff);
      scanEvents(xo, sres.isDictionary() ? sres : res, inner, page, depth + 1, seen, ev);
      ev.srcPath.pop_back();
      ev.formPath.erase(xo.getObjGen());
    }
  }
}
}
