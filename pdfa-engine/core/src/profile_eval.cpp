#include "profile_eval.hh"
#include "profile_ext.hh"

#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFCryptoProvider.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QUtil.hh>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <numeric>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "fonts_ft.hh"
#include "textextract.hh"
#include "util.hh"

namespace pdfa {

bool boxesIntersect(const Box& a, const Box& b) {
  return a.valid && b.valid && a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

const PageFacts* pageFor(const Events& ev, int page) {
  for (const PageFacts& p : ev.pages) {
    if (p.page == page) return &p;
  }
  return nullptr;
}

std::u32string utf16Hex(const std::string& h) {
  std::vector<uint32_t> units;
  for (size_t i = 0; i + 4 <= h.size(); i += 4) units.push_back(static_cast<uint32_t>(std::strtoul(h.substr(i, 4).c_str(), nullptr, 16)));
  if (h.size() == 2) units.push_back(static_cast<uint32_t>(std::strtoul(h.c_str(), nullptr, 16)));
  std::u32string out;
  for (size_t i = 0; i < units.size(); ++i) {
    uint32_t u = units[i];
    if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
      out += static_cast<char32_t>(0x10000 + ((u - 0xD800) << 10) + (units[i + 1] - 0xDC00));
      ++i;
    } else {
      out += static_cast<char32_t>(u);
    }
  }
  return out;
}

const std::map<uint32_t, std::u32string>& toUnicodeMap(const FontFacts& f, const Events& ev) {
  auto it = ev.toUnicode.find(f.og);
  if (it != ev.toUnicode.end()) return it->second;
  std::map<uint32_t, std::u32string>& m = ev.toUnicode[f.og];
  QPDFObjectHandle tu = f.dict.isDictionary() ? f.dict.getKey("/ToUnicode") : QPDFObjectHandle();
  if (!tu.isStream()) return m;
  std::string t;
  try {
    auto b = tu.getStreamData(qpdf_dl_all);
    t.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
  } catch (...) {
    return m;
  }
  auto hexes = [&](size_t from, size_t to) {
    std::vector<std::pair<size_t, std::string>> v;
    size_t q = from;
    while (q < to) {
      size_t a = t.find_first_of("<[", q);
      if (a == std::string::npos || a >= to) break;
      if (t[a] == '[') {
        v.push_back({a, "["});
        q = a + 1;
        continue;
      }
      size_t b = t.find('>', a);
      if (b == std::string::npos || b > to) break;
      std::string h;
      for (size_t i = a + 1; i < b; ++i) {
        if (std::isxdigit(static_cast<unsigned char>(t[i]))) h += t[i];
      }
      v.push_back({a, h});
      q = b + 1;
    }
    return v;
  };
  size_t pos = 0;
  while ((pos = t.find("beginbfchar", pos)) != std::string::npos && m.size() < 70000) {
    size_t end = t.find("endbfchar", pos);
    if (end == std::string::npos) break;
    auto v = hexes(pos, end);
    for (size_t i = 0; i + 1 < v.size(); i += 2) {
      m[static_cast<uint32_t>(std::strtoul(v[i].second.c_str(), nullptr, 16))] = utf16Hex(v[i + 1].second);
    }
    pos = end;
  }
  pos = 0;
  while ((pos = t.find("beginbfrange", pos)) != std::string::npos && m.size() < 70000) {
    size_t end = t.find("endbfrange", pos);
    if (end == std::string::npos) break;
    size_t close = end;
    auto v = hexes(pos + 12, close);
    for (size_t i = 0; i + 2 < v.size();) {
      uint32_t lo = static_cast<uint32_t>(std::strtoul(v[i].second.c_str(), nullptr, 16));
      uint32_t hi = static_cast<uint32_t>(std::strtoul(v[i + 1].second.c_str(), nullptr, 16));
      if (hi < lo || hi - lo > 65535) break;
      if (v[i + 2].second == "[") {
        size_t k = i + 3;
        for (uint32_t c = lo; c <= hi && k < v.size() && v[k].second != "["; ++c, ++k) m[c] = utf16Hex(v[k].second);
        i = k;
      } else {
        std::u32string base = utf16Hex(v[i + 2].second);
        for (uint32_t c = lo; c <= hi && !base.empty(); ++c) {
          std::u32string s2 = base;
          s2.back() = static_cast<char32_t>(s2.back() + (c - lo));
          m[c] = s2;
        }
        i += 3;
      }
    }
    pos = end;
  }
  return m;
}

namespace {
constexpr double kInkEps = 0.001;

struct StepUse {
  std::map<QPDFObjGen, std::set<std::string>> stepSpots;
  std::map<std::string, std::set<QPDFObjGen>> spotSteps;
  std::set<std::string> printSpots;
  std::map<int, std::set<QPDFObjGen>> pageSteps;
  std::map<QPDFObjGen, QPDFObjectHandle> steps;
};

struct DocCache {
  const Events* ev = nullptr;
  const void* paints = nullptr;
  const void* pages = nullptr;
  size_t n = 0;
  bool stepsDone = false;
  StepUse steps;
  bool encodingDone = false;
  std::vector<std::string> encoding;
  bool sealDone = false;
  std::string seal;
  bool formsDone = false;
  std::vector<QPDFObjectHandle> forms;
  std::map<QPDFObjGen, std::string> programNames;
};

DocCache& docCache(const Events& ev) {
  thread_local DocCache c;
  if (c.ev != &ev || c.paints != ev.paints.data() || c.pages != ev.pages.data() || c.n != ev.paints.size() + ev.texts.size() + ev.images.size()) {
    c = DocCache();
    c.ev = &ev;
    c.paints = ev.paints.data();
    c.pages = ev.pages.data();
    c.n = ev.paints.size() + ev.texts.size() + ev.images.size();
  }
  return c;
}

const std::set<std::string>& processInks() {
  static const std::set<std::string> s = {"Cyan", "Magenta", "Yellow", "Black"};
  return s;
}

std::string pct(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", std::round(v * 10000.0) / 100.0);
  return buf;
}

std::string boolText(bool b) { return b ? "true" : "false"; }

struct Obj {
  const GsExtra* x = nullptr;
  const ColorInfo* ci = nullptr;
  const std::vector<double>* comps = nullptr;
  const Box* bbox = nullptr;
  const Box* clip = nullptr;
  int page = 0;
  bool fill = false, stroke = false;
  bool overprint = false;
  int opm = 0;
  bool clippedPart = false, clippedFull = false;
  bool layerHidden = false;
};

bool objOf(const Subject& s, Obj& o) {
  if (s.paint) {
    const PaintEvent& e = *s.paint;
    o = {&e.x, &e.color, &e.comps, &e.bbox, &e.clip, e.page, e.fillOp, e.stroke, e.overprint, e.opm, e.clippedPart,
         e.clippedFull, e.layerHidden};
    return true;
  }
  if (s.text) {
    const TextEvent& e = *s.text;
    int m = e.renderMode;
    bool f = m == 0 || m == 2 || m == 4 || m == 6;
    bool st = m == 1 || m == 2 || m == 5 || m == 6;
    o = {&e.x, &e.color, &e.comps, &e.bbox, &e.clip, e.page, f, st, e.overprint, e.opm, e.clippedPart, e.clippedFull,
         e.layerHidden};
    return true;
  }
  if (s.image) {
    const ImageEvent& e = *s.image;
    o = {&e.x, &e.color, &e.comps, &e.bbox, &e.clip, e.page, true, false, e.overprint, e.opm, e.clippedPart,
         e.clippedFull, e.layerHidden};
    return true;
  }
  return false;
}

bool paintSelected(const PfAtom& a, const Obj& o) {
  const std::string& p = param(a, "paint");
  if (p == "fill") return o.fill;
  if (p == "stroke") return o.stroke;
  return o.fill || o.stroke;
}

bool additiveSpace(const ColorInfo& ci) {
  return ci.cls == "rgb" || ci.cls == "lab" || (ci.cls == "cal" && ci.declaredComps != 1) ||
         (ci.cls == "icc" && ci.declaredComps == 3);
}

bool inksOf(const ColorInfo& ci, const std::vector<double>& c, std::vector<std::pair<std::string, double>>& out) {
  out.clear();
  if (ci.indexed || ci.cls == "pattern" || additiveSpace(ci)) return false;
  if (ci.cls == "cmyk" || (ci.cls == "icc" && ci.declaredComps == 4)) {
    if (c.size() != 4) return false;
    static const char* n[] = {"Cyan", "Magenta", "Yellow", "Black"};
    for (int i = 0; i < 4; ++i) out.push_back({n[i], c[i]});
    return true;
  }
  if (ci.cls == "gray" || (ci.cls == "icc" && ci.declaredComps == 1) || ci.cls == "cal") {
    if (c.empty()) return false;
    out.push_back({"Black", 1.0 - c[0]});
    return true;
  }
  if (ci.cls == "separation" || ci.cls == "devicen") {
    for (size_t i = 0; i < ci.colorants.size() && i < c.size(); ++i) {
      const std::string& n = ci.colorants[i];
      if (n == "None") continue;
      if (n == "All") {
        for (const std::string& p : processInks()) out.push_back({p, c[i]});
        out.push_back({"All", c[i]});
        continue;
      }
      out.push_back({n, c[i]});
    }
    return true;
  }
  return false;
}

bool objInks(const Subject& s, const PfAtom& a, std::vector<std::pair<std::string, double>>& inks) {
  Obj o;
  if (!objOf(s, o) || !paintSelected(a, o)) return false;
  return inksOf(*o.ci, *o.comps, inks);
}

bool isRegistration(const ColorInfo& ci) {
  for (const std::string& c : ci.colorants) {
    if (c == "All") return true;
  }
  return false;
}

int evInk(const PfAtom& a, const Subject& s, const Events&, bool&) {
  std::vector<std::pair<std::string, double>> inks;
  if (!objInks(s, a, inks)) return 0;
  const std::string& which = param(a, "ink");
  double v = 0;
  for (const auto& [n, x] : inks) {
    bool proc = processInks().count(n) > 0;
    bool take = which == "max" || (which == "any_spot" && !proc && n != "All") ||
                (which == "cmy_max" && (n == "Cyan" || n == "Magenta" || n == "Yellow")) ||
                (which == "cyan" && n == "Cyan") || (which == "magenta" && n == "Magenta") ||
                (which == "yellow" && n == "Yellow") || (which == "black" && n == "Black");
    if (take) v = std::max(v, x);
  }
  return compare(Value(v * 100.0), a);
}

std::string inkMix(const ColorInfo& ci, const std::vector<std::pair<std::string, double>>& inks) {
  if (isRegistration(ci)) return "registration";
  double c = 0, m = 0, y = 0, k = 0;
  bool spot = false;
  for (const auto& [n, v] : inks) {
    if (v <= kInkEps) continue;
    if (n == "Cyan") c = std::max(c, v);
    else if (n == "Magenta") m = std::max(m, v);
    else if (n == "Yellow") y = std::max(y, v);
    else if (n == "Black") k = std::max(k, v);
    else spot = true;
  }
  bool hc = c > kInkEps, hm = m > kInkEps, hy = y > kInkEps, hk = k > kInkEps;
  int cmy = hc + hm + hy;
  if (!spot && !cmy && !hk) return "none";
  if (spot) {
    if (!cmy && !hk) return "spot_only";
    if (!cmy) return "spot_and_black";
    return "process_and_spot";
  }
  if (!cmy) return k >= 1.0 - kInkEps ? "solid_black" : "black_only";
  if (!hk) {
    if (cmy == 3 && std::fabs(c - m) < 0.002 && std::fabs(m - y) < 0.002) return "neutral_cmy";
    if (cmy == 1) return "single_process";
    return "cmy_only";
  }
  return "process_only";
}

int evInkMix(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o) || !paintSelected(a, o)) return 0;
  if (additiveSpace(*o.ci)) {
    bool white = !o.comps->empty() && o.ci->cls != "lab" && !s.image;
    for (double v : *o.comps) white = white && v >= 1.0 - kInkEps;
    return compare(Value(white ? "none" : "additive"), a);
  }
  std::vector<std::pair<std::string, double>> inks;
  if (s.image && !s.image->mask) {
    ColorInfo ci = *o.ci;
    ci.indexed = false;
    if (additiveSpace(ci)) return compare(Value("additive"), a);
    if (!inksOf(ci, std::vector<double>(std::max<size_t>(ci.colorants.size(), ci.cls == "cmyk" || ci.declaredComps == 4 ? 4 : 1), 0.5), inks)) return 0;
    return compare(Value(inkMix(ci, inks)), a);
  }
  if (!inksOf(*o.ci, *o.comps, inks)) return 0;
  return compare(Value(inkMix(*o.ci, inks)), a);
}

int evInksUsed(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o) || !paintSelected(a, o)) return 0;
  const std::string& kind = param(a, "kind");
  if (additiveSpace(*o.ci)) {
    if (kind != "any") return 0;
    int n = 0;
    for (double v : *o.comps) n += v < 1.0 - kInkEps;
    return compare(Value(n), a);
  }
  std::vector<std::pair<std::string, double>> inks;
  if (!inksOf(*o.ci, *o.comps, inks)) return 0;
  std::set<std::string> used;
  for (const auto& [n, v] : inks) {
    if (v <= kInkEps || n == "All") continue;
    bool proc = processInks().count(n) > 0;
    if (kind == "process" && !proc) continue;
    if (kind == "spot" && proc) continue;
    used.insert(n);
  }
  return compare(Value(static_cast<int>(used.size())), a);
}

int evTotalInk(const PfAtom& a, const Subject& s, const Events&, bool&) {
  std::vector<std::pair<std::string, double>> inks;
  if (!objInks(s, a, inks)) return 0;
  bool procOnly = paramBool(a, "process_only");
  double t = 0;
  for (const auto& [n, v] : inks) {
    if (n == "All" || (procOnly && !processInks().count(n))) continue;
    t += v;
  }
  return compare(Value(t * 100.0), a);
}

int evChannelSpread(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o) || !paintSelected(a, o) || o.comps->size() < 2) return 0;
  auto mm = std::minmax_element(o.comps->begin(), o.comps->end());
  return compare(Value((*mm.second - *mm.first) * 100.0), a);
}

int altComponents(const std::string& alt) {
  if (alt.empty()) return 0;
  if (alt == "DeviceCMYK") return 4;
  if (alt == "DeviceRGB" || alt == "Lab" || alt == "CalRGB") return 3;
  if (alt == "ICCBased") return 0;
  return 1;
}

QPDFObjectHandle baseArray(const ColorInfo& ci) {
  QPDFObjectHandle cs = ci.csObj;
  if (cs.isArray() && cs.getArrayNItems() > 1 && (nameIs(cs.getArrayItem(0), "/Indexed") || nameIs(cs.getArrayItem(0), "/I"))) {
    cs = cs.getArrayItem(1);
  }
  return cs;
}

QPDFObjectHandle alternateArray(const ColorInfo& ci) {
  QPDFObjectHandle cs = baseArray(ci);
  if ((ci.cls == "separation" || ci.cls == "devicen") && cs.isArray() && cs.getArrayNItems() >= 3) return cs.getArrayItem(2);
  return QPDFObjectHandle();
}

int evComponentCount(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o) || !paintSelected(a, o)) return 0;
  if (param(a, "level") == "alternate") {
    int n = altComponents(o.ci->altName);
    QPDFObjectHandle alt = alternateArray(*o.ci);
    if (o.ci->altName == "ICCBased" && alt.isArray() && alt.getArrayNItems() > 1 && alt.getArrayItem(1).isStream()) {
      n = static_cast<int>(numOf(alt.getArrayItem(1).getDict().getKey("/N"), 0));
    }
    return compare(Value(n), a);
  }
  return compare(Value(o.ci->declaredComps), a);
}

QPDFObjectHandle devicenAttrs(const ColorInfo& ci) {
  QPDFObjectHandle cs = baseArray(ci);
  if (ci.cls != "devicen" || !cs.isArray() || cs.getArrayNItems() < 5) return QPDFObjectHandle();
  QPDFObjectHandle d = cs.getArrayItem(4);
  return d.isDictionary() ? d : QPDFObjectHandle();
}

int evDeviceN(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o) || o.ci->cls != "devicen") return 0;
  const ColorInfo& ci = *o.ci;
  std::vector<std::string> v;
  std::set<std::string> uniq(ci.colorants.begin(), ci.colorants.end());
  if (uniq.size() != ci.colorants.size()) v.push_back("duplicate_colorant");
  for (const std::string& c : ci.colorants) {
    if (processInks().count(c)) {
      v.push_back("process_as_devicen");
      break;
    }
  }
  QPDFObjectHandle at = devicenAttrs(ci);
  if (at.isDictionary()) {
    bool nch = nameIs(at.getKey("/Subtype"), "/NChannel");
    if (at.getKey("/Colorants").isDictionary()) v.push_back("has_colorants_dict");
    if (at.getKey("/MixingHints").isDictionary()) v.push_back("has_mixing_hints");
    QPDFObjectHandle proc = at.getKey("/Process");
    if (at.hasKey("/Process")) v.push_back("has_process_key");
    if (proc.isDictionary()) {
      v.push_back("has_process_dict");
      QPDFObjectHandle comps = proc.getKey("/Components");
      QPDFObjectHandle pcs = proc.getKey("/ColorSpace");
      int want = 0;
      std::string fam = pcs.isName() ? nameOf(pcs) : pcs.isArray() && pcs.getArrayNItems() ? nameOf(pcs.getArrayItem(0)) : "";
      if (fam == "/DeviceCMYK") want = 4;
      else if (fam == "/DeviceRGB" || fam == "/CalRGB" || fam == "/Lab") want = 3;
      else if (fam == "/DeviceGray" || fam == "/CalGray") want = 1;
      else if (fam == "/ICCBased" && pcs.isArray() && pcs.getArrayNItems() > 1 && pcs.getArrayItem(1).isStream()) {
        want = static_cast<int>(numOf(pcs.getArrayItem(1).getDict().getKey("/N"), 0));
      }
      int have = comps.isArray() ? comps.getArrayNItems() : 0;
      if (have == 0 || (want > 0 && have != want)) v.push_back("missing_process_colorants");
    }
    if (nch) v.push_back("subtype_nchannel");
  }
  return compare(Value(v), a);
}

int evDeviceNColorants(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o) || (o.ci->cls != "devicen" && o.ci->cls != "separation")) return 0;
  const std::string& which = param(a, "which");
  if (which == "all") return compare(Value(o.ci->colorants), a);
  QPDFObjectHandle at = devicenAttrs(*o.ci);
  std::vector<std::string> v;
  if (which == "process") {
    QPDFObjectHandle proc = at.isDictionary() ? at.getKey("/Process") : QPDFObjectHandle();
    QPDFObjectHandle comps = proc.isDictionary() ? proc.getKey("/Components") : QPDFObjectHandle();
    for (int i = 0; comps.isArray() && i < comps.getArrayNItems(); ++i) {
      std::string n = nameOf(comps.getArrayItem(i));
      if (n.size() > 1) v.push_back(n.substr(1));
    }
  } else {
    QPDFObjectHandle col = at.isDictionary() && nameIs(at.getKey("/Subtype"), "/NChannel") ? at.getKey("/Colorants") : QPDFObjectHandle();
    for (const std::string& k : col.isDictionary() ? col.getKeys() : std::set<std::string>()) v.push_back(k.substr(1));
  }
  return compare(Value(v), a);
}

int evOverprint(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o) || !paintSelected(a, o)) return 0;
  return compare(Value(!o.overprint ? "off" : o.opm == 1 ? "on_nonzero_mode" : "on"), a);
}

int evRenderingIntent(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o)) return 0;
  std::string intent;
  if (s.image && s.image->obj.isStream()) {
    QPDFObjectHandle i = s.image->obj.getDict().getKey("/Intent");
    if (i.isName()) intent = i.getName().substr(1);
  }
  if (intent.empty()) intent = o.x->intent;
  if (intent.empty()) {
    if (paramBool(a, "explicit_only")) return 0;
    intent = "RelativeColorimetric";
  }
  return compare(Value(intent), a);
}

int evBpc(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o)) return 0;
  return compare(Value(o.x->bpcValue.empty() ? (o.x->hasBPC ? "Default" : "absent") : o.x->bpcValue), a);
}

std::string spaceName(const ColorInfo& ci) {
  if (ci.cls == "gray") return "DeviceGray";
  if (ci.cls == "rgb") return "DeviceRGB";
  if (ci.cls == "cmyk") return "DeviceCMYK";
  if (ci.cls == "icc") return "ICCBased";
  if (ci.cls == "lab") return "Lab";
  if (ci.cls == "pattern") return "Pattern";
  if (ci.cls == "separation") return "Separation";
  if (ci.cls == "devicen") return nameIs(devicenAttrs(ci).isDictionary() ? devicenAttrs(ci).getKey("/Subtype") : QPDFObjectHandle(), "/NChannel") ? "NChannel" : "DeviceN";
  if (ci.cls == "cal") return ci.declaredComps == 1 ? "CalGray" : "CalRGB";
  return std::string();
}

std::string familyOf(QPDFObjectHandle cs) {
  std::string n = cs.isName() ? nameOf(cs) : cs.isArray() && cs.getArrayNItems() ? nameOf(cs.getArrayItem(0)) : std::string();
  if (n == "/G") return "DeviceGray";
  if (n == "/RGB") return "DeviceRGB";
  if (n == "/CMYK") return "DeviceCMYK";
  if (n == "/I") return "Indexed";
  return n.size() > 1 ? n.substr(1) : std::string();
}

PfAtom expandShorthands(const PfAtom& a, const std::map<std::string, std::vector<std::string>>& m) {
  PfAtom b = a;
  b.vals.clear();
  for (const std::string& v : a.vals) {
    auto it = m.find(v);
    if (it == m.end()) b.vals.push_back(v);
    else b.vals.insert(b.vals.end(), it->second.begin(), it->second.end());
  }
  if (b.vals.size() > 1 && (b.op == "==" || b.op == "!=")) b.op = b.op == "==" ? "in" : "not_in";
  return b;
}

int evColorSpace(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o) || (s.paint && !paintSelected(a, o))) return 0;
  const ColorInfo& ci = *o.ci;
  const std::string& level = param(a, "level");
  std::string name;
  int iccN = ci.cls == "icc" ? ci.declaredComps : 0;
  if (level == "direct") {
    name = ci.indexed ? "Indexed" : spaceName(ci);
  } else if (level == "base") {
    name = spaceName(ci);
  } else if (level == "alternate") {
    name = ci.altName;
    QPDFObjectHandle alt = alternateArray(ci);
    if (name == "ICCBased" && alt.isArray() && alt.getArrayNItems() > 1 && alt.getArrayItem(1).isStream()) {
      iccN = static_cast<int>(numOf(alt.getArrayItem(1).getDict().getKey("/N"), 0));
    }
  } else if (level == "process") {
    QPDFObjectHandle at = devicenAttrs(ci);
    QPDFObjectHandle proc = at.isDictionary() ? at.getKey("/Process") : QPDFObjectHandle();
    QPDFObjectHandle pcs = proc.isDictionary() ? proc.getKey("/ColorSpace") : QPDFObjectHandle();
    name = familyOf(pcs);
    iccN = name == "ICCBased" && pcs.isArray() && pcs.getArrayNItems() > 1 && pcs.getArrayItem(1).isStream()
               ? static_cast<int>(numOf(pcs.getArrayItem(1).getDict().getKey("/N"), 0))
               : 0;
  } else {
    QPDFObjectHandle cs = ci.cls == "icc" ? baseArray(ci) : QPDFObjectHandle();
    QPDFObjectHandle st = cs.isArray() && cs.getArrayNItems() > 1 ? cs.getArrayItem(1) : QPDFObjectHandle();
    if (st.isStream()) name = familyOf(st.getDict().getKey("/Alternate"));
    iccN = 0;
  }
  if (name.empty()) return 0;
  const std::string& want = param(a, "icc_components");
  if (!want.empty() && name == "ICCBased" && iccN != std::atoi(want.c_str())) return 0;
  static const std::map<std::string, std::vector<std::string>> shorthands = {
      {"device", {"DeviceGray", "DeviceRGB", "DeviceCMYK"}},
      {"device_independent", {"CalGray", "CalRGB", "Lab", "ICCBased"}}};
  return compare(Value(name), expandShorthands(a, shorthands));
}

std::string colourSystem(const std::string& n) {
  static const std::regex hks("HKS\\s*\\d+\\s*[KNEZ]\\b", std::regex::icase);
  if (lowerAscii(n).find("pantone") != std::string::npos) return "pantone";
  if (std::regex_search(n, hks)) return "hks";
  return "other";
}

std::string spotKind(const std::string& n) {
  if (n == "All") return "registration";
  if (n == "None") return "none";
  if (processInks().count(n)) return "process";
  return "spot";
}

int evSpot(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o) || (o.ci->cls != "separation" && o.ci->cls != "devicen")) return 0;
  const ColorInfo& ci = *o.ci;
  const std::string& prop = param(a, "property");
  if (prop == "alternate_value") {
    if (ci.cls != "separation") return 0;
    size_t idx = static_cast<size_t>(std::max(1, std::atoi(param(a, "component").c_str())) - 1);
    if (idx >= ci.altValues.size()) return 0;
    double v = ci.altValues[idx];
    if (ci.altName == "DeviceCMYK" || ci.altName == "DeviceGray" || ci.altName == "DeviceRGB") v *= 100;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", std::round(v * 1000.0) / 1000.0);
    return compare(Value(std::string(buf)), a);
  }
  std::vector<std::string> v;
  for (const std::string& n : ci.colorants) {
    if (prop == "name") v.push_back(n);
    else if (prop == "kind") v.push_back(spotKind(n));
    else if (prop == "colour_system") v.push_back(colourSystem(n));
    else v.push_back(boolText(validUtf8(n)));
  }
  if (ci.cls == "separation") return v.empty() ? 0 : compare(Value(v[0]), a);
  return compare(Value(v), a);
}

int evColorValue(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o) || !paintSelected(a, o) || o.comps->empty()) return 0;
  const std::string& comp = param(a, "component");
  if (!comp.empty()) {
    size_t k = static_cast<size_t>(std::atoi(comp.c_str()));
    if (k < 1 || k > o.comps->size()) return 0;
    return compare(Value(pct((*o.comps)[k - 1])), a);
  }
  std::string all;
  for (double c : *o.comps) all += (all.empty() ? "" : " ") + pct(c);
  return compare(Value(all), a);
}

const Box* namedBox(const PageFacts& p, const std::string& n) {
  if (n == "media") return &p.media;
  const Box& crop = p.crop.valid ? p.crop : p.media;
  if (n == "crop") return &crop;
  const Box& b = n == "trim" ? p.trim : n == "bleed" ? p.bleed : p.art;
  return b.valid ? &b : &crop;
}

std::string boxRelation(const Box& outer, const Box& inner) {
  const double t = 0.1;
  if (inner.x0 >= outer.x0 - t && inner.y0 >= outer.y0 - t && inner.x1 <= outer.x1 + t && inner.y1 <= outer.y1 + t) return "inside";
  if (inner.x1 < outer.x0 - t || inner.x0 > outer.x1 + t || inner.y1 < outer.y0 - t || inner.y0 > outer.y1 + t) return "outside";
  return "crosses";
}

const Box* subjectBox(const Subject& s) {
  if (s.annot) return &s.annot->rect;
  Obj o;
  return objOf(s, o) ? o.bbox : nullptr;
}

int evBoxRelation(const PfAtom& a, const Subject& s, const Events&, bool&) {
  const Box* bb = subjectBox(s);
  if (!bb || !bb->valid || !s.pageFacts) return 0;
  const Box* b = namedBox(*s.pageFacts, param(a, "box"));
  if (!b->valid) return 0;
  std::string r = boxRelation(*b, *bb);
  const std::string& also = param(a, "also");
  if (!also.empty()) {
    const Box* b2 = namedBox(*s.pageFacts, also);
    if (!b2->valid || boxRelation(*b2, *bb) != r) return 0;
  }
  return compare(Value(r), a);
}

int evClipped(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o)) return 0;
  bool full = o.clippedFull || (o.clip->valid && o.bbox->valid && !boxesIntersect(*o.clip, *o.bbox));
  return compare(Value(full ? "fully" : o.clippedPart ? "partly" : "none"), a);
}

int evContext(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o)) return 0;
  std::vector<std::string> v;
  if (o.x->inForm) v.push_back("form_xobject");
  if (o.x->inPattern) v.push_back("tiling_pattern");
  if (o.x->inType3) v.push_back("type3_glyph");
  if (o.x->bxDepth > 0) v.push_back("compatibility_section");
  return compare(Value(v), a);
}

struct DashInfo {
  bool solid = true;
  int dots = 0, longs = 0;
  std::vector<double> dashes, gaps;
};

DashInfo dashInfo(const PaintEvent& e) {
  DashInfo d;
  std::vector<double> pat = e.x.dash;
  if (pat.size() % 2 == 1) pat.insert(pat.end(), e.x.dash.begin(), e.x.dash.end());
  double sum = 0;
  for (double v : pat) sum += std::fabs(v);
  d.solid = pat.empty() || sum <= 0;
  double sc = matScale(e.x.ctm), w = e.width;
  for (size_t i = 0; !d.solid && i + 1 < pat.size(); i += 2) {
    double ds = std::fabs(pat[i]) * sc, g = std::fabs(pat[i + 1]) * sc;
    double ed = e.x.lineCap == 0 ? ds : ds + w;
    double eg = e.x.lineCap == 0 ? g : std::max(0.0, g - w);
    d.dashes.push_back(ed);
    d.gaps.push_back(eg);
    if (ed <= w + 1e-6) ++d.dots;
    else ++d.longs;
  }
  return d;
}

int evDashLength(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.paint || !s.paint->stroke) return 0;
  DashInfo d = dashInfo(*s.paint);
  const std::vector<double>& v = param(a, "part") == "gap" ? d.gaps : d.dashes;
  if (d.solid || v.empty()) return 0;
  const std::string& m = param(a, "measure");
  double r;
  if (m == "longest") r = *std::max_element(v.begin(), v.end());
  else if (m == "shortest") r = *std::min_element(v.begin(), v.end());
  else r = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
  return compare(Value(r), a);
}

int evDistanceToBox(const PfAtom& a, const Subject& s, const Events&, bool&) {
  const Box* bb = subjectBox(s);
  if (!bb || !bb->valid || !s.pageFacts) return 0;
  const Box* b = namedBox(*s.pageFacts, param(a, "box"));
  if (!b->valid) return 0;
  if (param(a, "side") == "inside" && boxRelation(*b, *bb) != "inside") return 0;
  double d = std::min({std::fabs(bb->x0 - b->x0), std::fabs(bb->y0 - b->y0), std::fabs(b->x1 - bb->x1), std::fabs(b->y1 - bb->y1)});
  return compare(Value(d), a);
}

int evDistorted(const PfAtom& a, const Subject& s, const Events&, bool&) {
  bool r = false;
  if (s.paint) {
    const Mat& m = s.paint->x.ctm;
    double sx = std::hypot(m.a, m.b), sy = std::hypot(m.c, m.d);
    r = std::fabs(sx - sy) > 0.001 * std::max(sx, sy);
  } else if (s.text) {
    const GsExtra& x = s.text->x;
    Mat m = mul(Mat{x.hScale / 100.0, 0, 0, 1, 0, 0}, mul(x.tm, x.ctm));
    double ux = std::hypot(m.a, m.b), vy = std::hypot(m.c, m.d);
    r = std::fabs(ux - vy) > 0.01 * std::max(ux, vy);
  } else if (s.image) {
    const ImageEvent& e = *s.image;
    double ux = std::hypot(e.x.ctm.a, e.x.ctm.b), vy = std::hypot(e.x.ctm.c, e.x.ctm.d);
    if (e.width > 0 && e.height > 0 && ux > 0 && vy > 0) {
      double rx = e.width * 72.0 / ux, ry = e.height * 72.0 / vy;
      r = std::fabs(rx - ry) * 100.0 / std::min(rx, ry) > 0.01;
    }
  } else {
    return 0;
  }
  return compare(Value(r), a);
}

int evKind(const PfAtom& a, const Subject& s, const Events&, bool&) {
  std::string k;
  if (s.paint) k = s.paint->shade ? "shading" : "path";
  else if (s.text) k = "text";
  else if (s.image) k = s.image->mask ? "image_mask" : "image";
  else return 0;
  static const std::map<std::string, std::vector<std::string>> shorthands = {{"vector", {"path", "shading"}}};
  return compare(Value(k), expandShorthands(a, shorthands));
}

int evOutsideShare(const PfAtom& a, const Subject& s, const Events&, bool&) {
  const Box* bb = subjectBox(s);
  if (!bb || !bb->valid || !s.pageFacts) return 0;
  const Box* b = namedBox(*s.pageFacts, param(a, "box"));
  double w = bb->x1 - bb->x0, h = bb->y1 - bb->y0;
  double area = std::max(w, 0.0) * std::max(h, 0.0);
  if (area <= 0 || !b->valid) return 0;
  double iw = std::max(0.0, std::min(b->x1, bb->x1) - std::max(b->x0, bb->x0));
  double ih = std::max(0.0, std::min(b->y1, bb->y1) - std::max(b->y0, bb->y0));
  return compare(Value(100.0 * (1.0 - iw * ih / area)), a);
}

bool samePath(const PaintEvent& x, const PaintEvent& y) {
  return x.page == y.page && x.pathNodes == y.pathNodes && x.bbox.valid == y.bbox.valid && x.bbox.x0 == y.bbox.x0 &&
         x.bbox.y0 == y.bbox.y0 && x.bbox.x1 == y.bbox.x1 && x.bbox.y1 == y.bbox.y1 && x.src == y.src;
}

bool pairedPaint(const PaintEvent& e, const Events& ev) {
  if (ev.paints.empty() || &e < ev.paints.data() || &e >= ev.paints.data() + ev.paints.size()) return false;
  size_t i = static_cast<size_t>(&e - ev.paints.data());
  if (e.stroke && i + 1 < ev.paints.size()) {
    const PaintEvent& n = ev.paints[i + 1];
    return n.fillOp && !n.shade && samePath(e, n);
  }
  if (e.fillOp && !e.shade && i > 0) {
    const PaintEvent& p = ev.paints[i - 1];
    return p.stroke && samePath(e, p);
  }
  return false;
}

int evPainting(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  std::string v;
  if (s.paint) {
    const PaintEvent& e = *s.paint;
    if (e.shade) return 0;
    if (e.noPaint) v = "none";
    else if (pairedPaint(e, ev)) {
      if (!e.stroke) return 0;
      v = "fill_and_stroke";
    }
    else v = e.stroke ? "stroke" : "fill";
  } else if (s.text) {
    int m = s.text->renderMode;
    v = m == 0 || m == 4 ? "fill" : m == 1 || m == 5 ? "stroke" : m == 2 || m == 6 ? "fill_and_stroke" : "none";
  } else {
    return 0;
  }
  return compare(Value(v), a);
}

std::vector<std::string> consecutivePoints(const std::vector<double>& g) {
  std::vector<std::string> out;
  double cx = 0, cy = 0, sx = 0, sy = 0;
  int n = 0;
  bool open = false;
  auto finish = [&]() {
    if (open) out.push_back(std::to_string(n));
    open = false;
  };
  size_t i = 0;
  while (i < g.size()) {
    int code = static_cast<int>(g[i]);
    if (code == 0 && i + 2 < g.size()) {
      finish();
      sx = cx = g[i + 1];
      sy = cy = g[i + 2];
      n = 1;
      open = true;
      i += 3;
    } else if ((code == 1 && i + 2 < g.size()) || (code == 2 && i + 6 < g.size())) {
      size_t k = code == 1 ? 1 : 5;
      double ex = g[i + k], ey = g[i + k + 1];
      i += code == 1 ? 3 : 7;
      if (!open) {
        n = 1;
        open = true;
      }
      if (std::hypot(ex - cx, ey - cy) < 1e-6) continue;
      ++n;
      cx = ex;
      cy = ey;
    } else if (code == 3) {
      ++i;
      if (open && std::hypot(sx - cx, sy - cy) < 1e-6 && n > 1) --n;
      finish();
      cx = sx;
      cy = sy;
    } else {
      break;
    }
  }
  finish();
  return out;
}

int evPathMeasure(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  const std::string& m = param(a, "measure");
  if (m == "page_points") {
    Obj o;
    if (!objOf(s, o)) return 0;
    long long n = 0;
    for (const PaintEvent& p : ev.paints) n += p.page == o.page ? p.pathNodes : 0;
    return compare(Value(n), a);
  }
  if (!s.paint) return 0;
  const PaintEvent& e = *s.paint;
  if (e.shade) return 0;
  if (m == "consecutive_points") return compare(Value(consecutivePoints(e.geo)), a);
  double v = m == "points" ? e.pathNodes
             : m == "subpaths" ? e.subpaths
             : m == "open_subpaths" ? e.subpaths - e.closedSubpaths
             : m == "closed_subpaths" ? e.closedSubpaths
             : m == "straight_segments" ? e.lines
             : m == "curves" ? e.curves
             : m == "rectangles" ? e.rects
             : m == "largest_subpath_points" ? e.maxSubPts
             : m == "smallest_subpath_points" ? e.minSubPts
             : e.pathNodes == 1 ? 1 : 0;
  return compare(Value(v), a);
}

int evShading(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.paint || s.paint->shadingType <= 0) return 0;
  const PaintEvent& e = *s.paint;
  const std::string& prop = param(a, "property");
  QPDFObjectHandle sd = e.shading.isStream() ? e.shading.getDict() : e.shading;
  if (prop == "type") {
    static const char* names[] = {"", "function", "axial", "radial", "free_form", "lattice", "coons", "tensor"};
    if (e.shadingType > 7) return 0;
    return compare(Value(names[e.shadingType]), a);
  }
  if (prop == "radial_without_radius") {
    QPDFObjectHandle c = sd.isDictionary() ? sd.getKey("/Coords") : QPDFObjectHandle();
    bool r = e.shadingType == 3 && c.isArray() && c.getArrayNItems() == 6 && numOf(c.getArrayItem(2), 0) == 0 &&
             numOf(c.getArrayItem(5), 0) == 0;
    return compare(Value(boolText(r)), a);
  }
  QPDFObjectHandle bb = sd.isDictionary() ? sd.getKey("/BBox") : QPDFObjectHandle();
  bool ok = bb.isArray() && bb.getArrayNItems() == 4;
  for (int i = 0; ok && i < 4; ++i) ok = bb.getArrayItem(i).isNumber();
  return compare(Value(boolText(ok)), a);
}

int evSize(const PfAtom& a, const Subject& s, const Events&, bool&) {
  const Box* bb = subjectBox(s);
  if (!bb || !bb->valid) return 0;
  double w = bb->x1 - bb->x0, h = bb->y1 - bb->y0;
  const std::string& d = param(a, "dimension");
  if (d == "area") {
    double f = lengthToPt("1", param(a, "unit"));
    return compare(Value(w * h / f), a);
  }
  return compare(Value(d == "width" ? w : d == "height" ? h : d == "shorter" ? std::min(w, h) : std::max(w, h)), a);
}

std::string dashArrayText(const std::vector<double>& d) {
  std::string s = "[";
  for (size_t i = 0; i < d.size(); ++i) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(static_cast<float>(d[i])));
    s += (i ? " " : "") + std::string(buf);
  }
  return s + "]";
}

int evStrokeStyle(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.paint || !s.paint->stroke) return 0;
  const PaintEvent& e = *s.paint;
  const std::string& p = param(a, "property");
  std::string v;
  char buf[32];
  if (p == "cap") v = std::to_string(e.x.lineCap);
  else if (p == "join") v = std::to_string(e.x.lineJoin);
  else if (p == "miter_limit" || p == "dash_phase") {
    std::snprintf(buf, sizeof buf, "%g", p == "miter_limit" ? e.x.miterLimit : e.x.dashPhase);
    v = buf;
  } else if (p == "dash_array") v = dashArrayText(e.x.dash);
  else if (p == "stroke_adjustment") v = boolText(e.x.strokeAdjust);
  else {
    DashInfo d = dashInfo(e);
    v = d.solid ? "solid" : d.dots && d.longs ? "mixed" : d.dots ? "dotted" : "dashed";
  }
  return compare(Value(v), a);
}

bool leavesNoInk(const ColorInfo& ci, const std::vector<double>& c, bool overprint) {
  std::vector<std::pair<std::string, double>> inks;
  if (additiveSpace(ci)) {
    for (double v : c) {
      if (v < 1.0 - kInkEps) return false;
    }
    return !c.empty() && !overprint;
  }
  if (!inksOf(ci, c, inks)) return false;
  for (const auto& [n, v] : inks) {
    if (v > kInkEps) return false;
  }
  return true;
}

int evStrokeWidth(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.paint || !s.paint->stroke) return 0;
  const PaintEvent& e = *s.paint;
  if (paramBool(a, "ignore_white") && leavesNoInk(e.color, e.comps, e.overprint)) return 0;
  return compare(Value(e.width), a);
}

int evDataAfterEof(const PfAtom& a, const Subject&, const Events& ev, bool&) { return compare(Value(ev.dataAfterEof), a); }
int evFileSize(const PfAtom& a, const Subject&, const Events& ev, bool&) { return compare(Value(ev.filesize), a); }
int evPageCount(const PfAtom& a, const Subject&, const Events& ev, bool&) { return compare(Value(ev.pageCount), a); }

int evPdfVersion(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  double header = std::atof(ev.header.c_str());
  QPDFObjectHandle v = ev.root.isDictionary() ? ev.root.getKey("/Version") : QPDFObjectHandle();
  double catalog = v.isName() && v.getName().size() > 1 ? std::atof(v.getName().c_str() + 1) : 0;
  const std::string& src = param(a, "source");
  if (src == "header") return header > 0 ? compare(Value(header), a) : 0;
  if (src == "catalog") return catalog > 0 ? compare(Value(catalog), a) : 0;
  double eff = std::max(header, catalog);
  return eff > 0 ? compare(Value(eff), a) : 0;
}

int evRequirements(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  bool r = paramBool(a, "pdf2_only") ? ev.requirementsPdf20 : ev.root.isDictionary() && ev.root.hasKey("/Requirements");
  return compare(Value(r), a);
}

int evAllTextUnicode(const PfAtom& a, const Subject& s, const Events&, bool&) {
  return s.font ? compare(Value(s.font->allUsedMapped), a) : 0;
}

int evEmbedding(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.font) return 0;
  const FontFacts& f = *s.font;
  return compare(Value(!f.embedded ? "not_embedded" : f.subsetName ? "embedded_subset" : "embedded_full"), a);
}

int evLicense(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.font || !s.font->embedded || s.font->type3) return 0;
  const FontFacts& f = *s.font;
  std::vector<std::string> v;
  if (!f.ftLoaded) {
    v.push_back("flag_missing");
  } else {
    int t = f.fsType;
    if (t & 0x0008) v.push_back("editable");
    else if (t & 0x0004) v.push_back("preview_print");
    else if (t & 0x0002) v.push_back("restricted");
    else v.push_back("installable");
    if (t & 0x0100) v.push_back("no_subsetting");
    if (t & 0x0200) v.push_back("bitmap_only");
    if (t & ~0x030f) v.push_back("unknown_value");
  }
  return compare(Value(v), a);
}

std::string stripSubset(const std::string& n) {
  if (n.size() > 7 && n[6] == '+') {
    for (int i = 0; i < 6; ++i) {
      if (!std::isupper(static_cast<unsigned char>(n[i]))) return n;
    }
    return n.substr(7);
  }
  return n;
}

QPDFObjectHandle fontDescriptor(const FontFacts& f) {
  if (!f.dict.isDictionary()) return QPDFObjectHandle();
  QPDFObjectHandle dfs = f.dict.getKey("/DescendantFonts");
  if (dfs.isArray() && dfs.getArrayNItems() && dfs.getArrayItem(0).isDictionary()) return dfs.getArrayItem(0).getKey("/FontDescriptor");
  return f.dict.getKey("/FontDescriptor");
}

std::string programNameUncached(const FontFacts& f) {
  QPDFObjectHandle prog = fontFileStream(fontDescriptor(f));
  if (!prog.isStream()) return std::string();
  FtLib lib;
  FtFace face;
  if (!lib.lib || !loadFace(lib, prog, face)) return std::string();
  const char* n = FT_Get_Postscript_Name(face.face);
  return n ? stripSubset(n) : std::string();
}

std::string programName(const FontFacts& f, const Events& ev) {
  DocCache& c = docCache(ev);
  auto it = c.programNames.find(f.og);
  if (it != c.programNames.end()) return it->second;
  return c.programNames[f.og] = programNameUncached(f);
}

int evFontName(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.font) return 0;
  std::string n = param(a, "source") == "pdf" ? stripSubset(s.font->baseFont) : programName(*s.font, ev);
  return n.empty() ? 0 : compare(Value(n), a);
}

int evNameProblem(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.font) return 0;
  const FontFacts& f = *s.font;
  std::vector<std::string> v;
  std::string base = stripSubset(f.baseFont);
  std::string prog = f.embedded ? programName(f, ev) : std::string();
  if (!prog.empty() && prog != base) v.push_back("differs_from_program");
  int share = 0;
  for (const FontFacts& o : ev.fonts) share += !base.empty() && stripSubset(o.baseFont) == base && o.og != f.og;
  if (share > 0) v.push_back("not_unique");
  if (!validUtf8(f.baseFont)) v.push_back("not_utf8");
  for (unsigned char c : f.baseFont) {
    if (c <= 0x20 || c == 0x7f || std::strchr("()<>[]{}/%", c)) {
      v.push_back("bad_escaping");
      break;
    }
  }
  return compare(Value(v), a);
}

int evResolution(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.image) return 0;
  const ImageEvent& e = *s.image;
  bool bitmap = e.mask || e.bpc == 1;
  const std::string& kind = param(a, "kind");
  if ((kind == "bitmap" && !bitmap) || (kind == "colour_or_grey" && bitmap)) return 0;
  double ux = std::hypot(e.x.ctm.a, e.x.ctm.b), vy = std::hypot(e.x.ctm.c, e.x.ctm.d);
  if (e.width <= 0 || e.height <= 0 || ux <= 0.01 || vy <= 0.01) return 0;
  double rx = e.width * 72.0 / ux, ry = e.height * 72.0 / vy;
  const std::string& ax = param(a, "axis");
  double v = ax == "horizontal" ? rx : ax == "vertical" ? ry : ax == "highest" ? std::max(rx, ry) : std::min(rx, ry);
  return compare(Value(v), a);
}

double angleOf(double a, double b) {
  double d = std::atan2(b, a) * 180.0 / M_PI;
  if (d < 0) d += 360.0;
  d = std::round(d * 100.0) / 100.0;
  return d >= 360.0 ? 0 : d;
}

int evImageRotation(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.image) return 0;
  double d = angleOf(s.image->x.ctm.a, s.image->x.ctm.b);
  if (paramBool(a, "right_angles_only") && std::fmod(d, 90.0) == 0) return 0;
  return compare(Value(d), a);
}

std::string ocStr(QPDFObjectHandle o) {
  if (o.isName()) return o.getName().substr(1);
  if (o.isString()) return o.getUTF8Value();
  return std::string();
}

bool secondClass(const std::string& n) {
  if (n.size() < 6 || n[4] != '_') return false;
  for (int i = 0; i < 4; ++i) {
    if (!std::isalnum(static_cast<unsigned char>(n[i]))) return false;
  }
  return true;
}

QPDFObjectHandle stepMeta(QPDFObjectHandle ocg) {
  QPDFObjectHandle m = ocg.isDictionary() ? ocg.getKey("/GTS_Metadata") : QPDFObjectHandle();
  return m.isDictionary() && (m.hasKey("/GTS_ProcStepsGroup") || nameIs(m.getKey("/Type"), "/GTS_ProcSteps")) ? m : QPDFObjectHandle();
}

std::string stepLabel(QPDFObjectHandle m) {
  std::string g = ocStr(m.getKey("/GTS_ProcStepsGroup")), t = ocStr(m.getKey("/GTS_ProcStepsType"));
  return t.empty() ? g : g.empty() ? t : g + "/" + t;
}

struct StepWalker : QPDFObjectHandle::ParserCallbacks {
  StepUse& u;
  int page;
  QPDFObjectHandle res;
  int depth;
  std::set<QPDFObjGen>& visiting;
  std::vector<QPDFObjectHandle> args;
  std::vector<std::vector<QPDFObjGen>> oc;
  std::vector<QPDFObjGen> outer;
  std::string fill, stroke;
  std::vector<std::pair<std::string, std::string>> saved;

  StepWalker(StepUse& use, int pg, QPDFObjectHandle r, int d, std::set<QPDFObjGen>& vis, std::vector<QPDFObjGen> o)
      : u(use), page(pg), res(r), depth(d), visiting(vis), outer(std::move(o)) {}

  std::vector<QPDFObjGen> stepsOf(QPDFObjectHandle cond) {
    std::vector<QPDFObjGen> out;
    std::vector<QPDFObjectHandle> list;
    if (cond.isDictionary() && nameIs(cond.getKey("/Type"), "/OCMD")) {
      QPDFObjectHandle g = cond.getKey("/OCGs");
      if (g.isDictionary()) list.push_back(g);
      for (int i = 0; g.isArray() && i < g.getArrayNItems(); ++i) list.push_back(g.getArrayItem(i));
    } else if (cond.isDictionary()) {
      list.push_back(cond);
    }
    for (QPDFObjectHandle g : list) {
      if (g.isIndirect() && stepMeta(g).isDictionary()) {
        out.push_back(g.getObjGen());
        u.steps[g.getObjGen()] = g;
      }
    }
    return out;
  }

  std::vector<QPDFObjGen> active() {
    std::vector<QPDFObjGen> c = outer;
    for (const auto& v : oc) c.insert(c.end(), v.begin(), v.end());
    return c;
  }

  std::string spotOf(QPDFObjectHandle name) {
    if (!name.isName() || !res.isDictionary()) return std::string();
    QPDFObjectHandle d = res.getKey("/ColorSpace");
    QPDFObjectHandle cs = d.isDictionary() ? d.getKey(name.getName()) : QPDFObjectHandle();
    if (cs.isArray() && cs.getArrayNItems() >= 2 && nameIs(cs.getArrayItem(0), "/Separation")) {
      std::string n = nameOf(cs.getArrayItem(1));
      if (n.size() > 1 && n != "/All" && n != "/None" && !processInks().count(n.substr(1))) return n.substr(1);
    }
    return std::string();
  }

  void painted(const std::string& spot) {
    std::vector<QPDFObjGen> st = active();
    for (const QPDFObjGen& g : st) u.pageSteps[page].insert(g);
    if (spot.empty()) return;
    if (st.empty()) u.printSpots.insert(spot);
    for (const QPDFObjGen& g : st) {
      u.stepSpots[g].insert(spot);
      u.spotSteps[spot].insert(g);
    }
  }

  void handleObject(QPDFObjectHandle o, size_t, size_t) override {
    if (!o.isOperator()) {
      args.push_back(o);
      return;
    }
    std::string op = o.getOperatorValue();
    QPDFObjectHandle a0 = args.empty() ? QPDFObjectHandle::newNull() : args.front();
    if (op == "q") {
      saved.push_back({fill, stroke});
    } else if (op == "Q") {
      if (!saved.empty()) {
        fill = saved.back().first;
        stroke = saved.back().second;
        saved.pop_back();
      }
    } else if (op == "cs") {
      fill = spotOf(a0);
    } else if (op == "CS") {
      stroke = spotOf(a0);
    } else if (op == "g" || op == "rg" || op == "k") {
      fill.clear();
    } else if (op == "G" || op == "RG" || op == "K") {
      stroke.clear();
    } else if (op == "f" || op == "F" || op == "f*" || op == "sh" || op == "Tj" || op == "TJ" || op == "'" || op == "\"" ||
               op == "EI") {
      painted(fill);
    } else if (op == "S" || op == "s") {
      painted(stroke);
    } else if (op == "B" || op == "B*" || op == "b" || op == "b*") {
      painted(fill);
      painted(stroke);
    } else if (op == "BDC" || op == "BMC") {
      std::vector<QPDFObjGen> st;
      if (op == "BDC" && args.size() >= 2 && nameIs(args[0], "/OC")) {
        QPDFObjectHandle c = args[1];
        if (c.isName() && res.isDictionary() && res.getKey("/Properties").isDictionary()) c = res.getKey("/Properties").getKey(c.getName());
        st = stepsOf(c);
      }
      oc.push_back(st);
    } else if (op == "EMC") {
      if (!oc.empty()) oc.pop_back();
    } else if (op == "Do" && a0.isName() && res.isDictionary() && res.getKey("/XObject").isDictionary()) {
      QPDFObjectHandle xo = res.getKey("/XObject").getKey(a0.getName());
      if (xo.isStream()) {
        QPDFObjectHandle d = xo.getDict();
        std::vector<QPDFObjGen> extra = stepsOf(d.getKey("/OC"));
        if (nameIs(d.getKey("/Subtype"), "/Form") && depth < 12 && !visiting.count(xo.getObjGen())) {
          std::vector<QPDFObjGen> o2 = active();
          o2.insert(o2.end(), extra.begin(), extra.end());
          QPDFObjectHandle r = d.getKey("/Resources");
          visiting.insert(xo.getObjGen());
          StepWalker w(u, page, r.isDictionary() ? r : res, depth + 1, visiting, o2);
          w.fill = fill;
          w.stroke = stroke;
          try {
            QPDFObjectHandle::parseContentStream(xo, &w);
          } catch (...) {
          }
          visiting.erase(xo.getObjGen());
        } else {
          for (const QPDFObjGen& g : extra) oc.push_back({g});
          painted(fill);
          for (size_t i = 0; i < extra.size(); ++i) oc.pop_back();
        }
      }
    }
    args.clear();
  }
  void handleEOF() override {}
};

const StepUse& stepUse(const Events& ev) {
  DocCache& c = docCache(ev);
  if (c.stepsDone) return c.steps;
  c.stepsDone = true;
  QPDF* q = ev.root.isDictionary() ? ev.root.getOwningQPDF() : nullptr;
  QPDFObjectHandle ocp = ev.root.isDictionary() ? ev.root.getKey("/OCProperties") : QPDFObjectHandle();
  if (!q || !ocp.isDictionary()) return c.steps;
  QPDFObjectHandle ocgs = ocp.getKey("/OCGs");
  for (int i = 0; ocgs.isArray() && i < ocgs.getArrayNItems(); ++i) {
    QPDFObjectHandle g = ocgs.getArrayItem(i);
    if (g.isIndirect() && stepMeta(g).isDictionary()) c.steps.steps[g.getObjGen()] = g;
  }
  for (const PageFacts& p : ev.pages) {
    if (!p.obj.isDictionary()) continue;
    try {
      QPDFPageObjectHelper ph(p.obj);
      std::set<QPDFObjGen> vis;
      StepWalker w(c.steps, p.page, ph.getAttribute("/Resources", false), 0, vis, {});
      ph.parseContents(&w);
    } catch (...) {
    }
  }
  return c.steps;
}

bool hasProcSteps(const Events& ev) { return !stepUse(ev).steps.empty(); }

int evProcessingStep(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  if (s.page <= 0 || !hasProcSteps(ev)) return compare(Value(std::vector<std::string>()), a);
  const StepUse& u = stepUse(ev);
  std::set<std::string> v;
  std::vector<QPDFObjectHandle> conds;
  if (s.paint || s.text || s.image) {
    if (!objectOcConds(s, ev, conds)) {
      supported = false;
      return 0;
    }
    for (QPDFObjectHandle c : conds) {
      for (QPDFObjectHandle g : ocgsOf(c)) {
        auto st = g.isIndirect() ? u.steps.find(g.getObjGen()) : u.steps.end();
        if (st != u.steps.end()) v.insert(stepLabel(stepMeta(st->second)));
      }
    }
    return compare(Value(std::vector<std::string>(v.begin(), v.end())), a);
  }
  auto it = u.pageSteps.find(s.page);
  if (it != u.pageSteps.end()) {
    for (const QPDFObjGen& g : it->second) {
      auto st = u.steps.find(g);
      if (st != u.steps.end()) v.insert(stepLabel(stepMeta(st->second)));
    }
  }
  return compare(Value(std::vector<std::string>(v.begin(), v.end())), a);
}

int evProcessingStepProblem(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const StepUse& u = stepUse(ev);
  std::set<std::string> v;
  std::set<QPDFObjGen> listed;
  QPDFObjectHandle ocp = ev.root.isDictionary() ? ev.root.getKey("/OCProperties") : QPDFObjectHandle();
  QPDFObjectHandle ocgs = ocp.isDictionary() ? ocp.getKey("/OCGs") : QPDFObjectHandle();
  for (int i = 0; ocgs.isArray() && i < ocgs.getArrayNItems(); ++i) {
    if (ocgs.getArrayItem(i).isIndirect()) listed.insert(ocgs.getArrayItem(i).getObjGen());
  }
  std::map<std::string, int> labels;
  static const std::set<std::string> known = {"/Type", "/GTS_ProcStepsGroup", "/GTS_ProcStepsType"};
  for (const auto& [og, g] : u.steps) {
    QPDFObjectHandle m = stepMeta(g);
    std::string grp = ocStr(m.getKey("/GTS_ProcStepsGroup"));
    if (!m.hasKey("/GTS_ProcStepsType") && (grp == "Structural" || grp == "Positions")) v.insert("missing_type");
    if (secondClass(ocStr(m.getKey("/GTS_ProcStepsGroup"))) || secondClass(ocStr(m.getKey("/GTS_ProcStepsType")))) {
      v.insert("custom_value");
    }
    for (const std::string& k : m.getKeys()) {
      if (!known.count(k) && !secondClass(k.substr(1))) v.insert("custom_key_not_second_class");
    }
    if (++labels[stepLabel(m)] > 1) v.insert("same_step_in_several_layers");
    if (!listed.count(og)) v.insert("layers_missing_on_page");
  }
  for (const auto& [spot, steps] : u.spotSteps) {
    if (steps.size() > 1) v.insert("spot_used_by_several_steps");
    if (u.printSpots.count(spot)) v.insert("spot_used_by_print_content");
  }
  for (const auto& [og, spots] : u.stepSpots) {
    if (spots.size() > 1) v.insert("step_uses_several_spots");
  }
  if (ev.xmpRaw.find("ProcSteps") != std::string::npos) v.insert("document_metadata");
  for (const PageFacts& p : ev.pages) {
    QPDFObjectHandle md = p.obj.isDictionary() ? p.obj.getKey("/Metadata") : QPDFObjectHandle();
    if (!md.isStream()) continue;
    try {
      auto b = md.getStreamData(qpdf_dl_all);
      std::string x(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
      if (x.find("ProcSteps") != std::string::npos) {
        v.insert("page_metadata");
        break;
      }
    } catch (...) {
    }
  }
  return compare(Value(std::vector<std::string>(v.begin(), v.end())), a);
}

int evLayerVisible(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  Obj o;
  if (!objOf(s, o)) return 0;
  bool vis = !o.layerHidden;
  if (!vis && param(a, "in") == "any_configuration") {
    QPDFObjectHandle ocp = ev.root.isDictionary() ? ev.root.getKey("/OCProperties") : QPDFObjectHandle();
    QPDFObjectHandle cf = ocp.isDictionary() ? ocp.getKey("/Configs") : QPDFObjectHandle();
    for (int i = 0; cf.isArray() && i < cf.getArrayNItems() && !vis; ++i) {
      QPDFObjectHandle c = cf.getArrayItem(i);
      if (!c.isDictionary()) continue;
      bool baseOff = nameIs(c.getKey("/BaseState"), "/OFF");
      QPDFObjectHandle off = c.getKey("/OFF");
      vis = !baseOff && !(off.isArray() && off.getArrayNItems() > 0);
    }
  }
  return compare(Value(vis), a);
}

int evOutputIntentCount(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle ois = ev.root.isDictionary() ? ev.root.getKey("/OutputIntents") : QPDFObjectHandle();
  const std::string& sub = param(a, "subtype");
  std::string want = sub == "pdfx" ? "/GTS_PDFX" : sub == "pdfa" ? "/GTS_PDFA1" : sub == "pdfe" ? "/ISO_PDFE1" : "";
  int n = 0;
  for (int i = 0; ois.isArray() && i < ois.getArrayNItems(); ++i) {
    QPDFObjectHandle oi = ois.getArrayItem(i);
    n += oi.isDictionary() && (want.empty() || nameIs(oi.getKey("/S"), want));
  }
  return compare(Value(n), a);
}

int pageRotation(const PageFacts& p) {
  int r = 0;
  if (p.obj.isDictionary()) {
    QPDFPageObjectHelper ph(p.obj);
    QPDFObjectHandle rot = ph.getAttribute("/Rotate", false);
    if (rot.isNumber()) r = static_cast<int>(std::lround(rot.getNumericValue()));
  }
  r %= 360;
  if (r < 0) r += 360;
  return (r / 90) * 90;
}

double userUnit(const PageFacts& p) {
  QPDFObjectHandle u = p.obj.isDictionary() ? p.obj.getKey("/UserUnit") : QPDFObjectHandle();
  return u.isNumber() && u.getNumericValue() > 0 ? u.getNumericValue() : 1.0;
}

int evAspectRatio(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts) return 0;
  const Box* b = namedBox(*s.pageFacts, param(a, "box"));
  double w = b->x1 - b->x0, h = b->y1 - b->y0;
  if (!b->valid || std::min(w, h) <= 0) return 0;
  return compare(Value(std::max(w, h) / std::min(w, h)), a);
}

int evBoxPresent(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts) return 0;
  const PageFacts& p = *s.pageFacts;
  const std::string& b = param(a, "box");
  bool r = b == "media" ? p.hasMediaBox : b == "crop" ? p.hasCropBox
           : p.obj.isDictionary() && p.obj.getKey(b == "trim" ? "/TrimBox" : b == "bleed" ? "/BleedBox" : "/ArtBox").isArray();
  return compare(Value(r), a);
}

bool pageInRange(int page, int count, const std::string& spec) {
  std::string cur;
  for (size_t i = 0; i <= spec.size(); ++i) {
    char c = i < spec.size() ? static_cast<char>(std::tolower(static_cast<unsigned char>(spec[i]))) : ',';
    if (c != ',' && c != ';' && c != ' ') {
      cur += c;
      continue;
    }
    if (cur.empty()) continue;
    if (cur == "odd" && page % 2 == 1) return true;
    if (cur == "even" && page % 2 == 0) return true;
    if (cur == "last" && page == count) return true;
    size_t dash = cur.find('-');
    auto num = [&](const std::string& t, int dflt) { return t.empty() ? dflt : t == "last" ? count : std::atoi(t.c_str()); };
    if (std::isdigit(static_cast<unsigned char>(cur[0])) || cur[0] == '-' || cur.rfind("last", 0) == 0) {
      int lo = dash == std::string::npos ? num(cur, 0) : num(cur.substr(0, dash), 1);
      int hi = dash == std::string::npos ? lo : num(cur.substr(dash + 1), count);
      if (page >= lo && page <= hi) return true;
    }
    cur.clear();
  }
  return false;
}

int evInRange(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.pageFacts) return 0;
  return compare(Value(pageInRange(s.pageFacts->page, ev.pageCount, param(a, "pages"))), a);
}

int evMostlyRotatedText(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.pageFacts) return 0;
  int rot = 0, total = 0;
  for (const TextEvent& te : ev.texts) {
    if (te.page != s.pageFacts->page) continue;
    ++total;
    Mat m = mul(Mat{te.x.hScale / 100.0, 0, 0, 1, 0, 0}, mul(te.x.tm, te.x.ctm));
    rot += angleOf(m.a, m.b) != 0;
  }
  return compare(Value(total > 0 && rot * 2 > total), a);
}

int evPageNumber(const PfAtom& a, const Subject& s, const Events&, bool&) {
  return s.pageFacts ? compare(Value(s.pageFacts->page), a) : 0;
}

int evParity(const PfAtom& a, const Subject& s, const Events&, bool&) {
  return s.pageFacts ? compare(Value(s.pageFacts->page % 2 ? "odd" : "even"), a) : 0;
}

int evPageRotation(const PfAtom& a, const Subject& s, const Events&, bool&) {
  return s.pageFacts ? compare(Value(pageRotation(*s.pageFacts)), a) : 0;
}

int evSingleImage(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.pageFacts) return 0;
  int pg = s.pageFacts->page, images = 0, other = 0;
  for (const ImageEvent& e : ev.images) images += e.page == pg;
  for (const PaintEvent& e : ev.paints) other += e.page == pg && !e.noPaint;
  for (const TextEvent& e : ev.texts) other += e.page == pg && e.renderMode != 3 && e.renderMode != 7;
  return compare(Value(images == 1 && other == 0), a);
}

int evPageSize(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts) return 0;
  const Box* b = namedBox(*s.pageFacts, param(a, "box"));
  if (!b->valid) return 0;
  return compare(Value::size(b->x1 - b->x0, b->y1 - b->y0), a);
}

int evTextContains(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.pageFacts) return 0;
  int pg = s.pageFacts->page;
  auto it = ev.pageText.find(pg);
  if (it == ev.pageText.end()) {
    std::string all;
    try {
      QPDFPageObjectHelper ph(s.pageFacts->obj);
      for (const TextRun& r : extractPageText(ph)) all += r.text;
    } catch (...) {
    }
    it = ev.pageText.emplace(pg, all).first;
  }
  return compare(Value(it->second), a);
}

int evUserUnit(const PfAtom& a, const Subject& s, const Events&, bool&) {
  return s.pageFacts ? compare(Value(userUnit(*s.pageFacts)), a) : 0;
}

int evBoxOffset(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts) return 0;
  const PageFacts& p = *s.pageFacts;
  const Box* b = namedBox(p, param(a, "box"));
  if (!b->valid) return 0;
  bool h = param(a, "axis") == "horizontal";
  double ref = param(a, "from") == "media" && p.media.valid ? (h ? p.media.x0 : p.media.y0) : 0;
  return compare(Value((h ? b->x0 : b->y0) - ref), a);
}

int evBoxSize(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts) return 0;
  const PageFacts& p = *s.pageFacts;
  const Box* b = namedBox(p, param(a, "box"));
  if (!b->valid) return 0;
  double w = b->x1 - b->x0, h = b->y1 - b->y0;
  if (!paramBool(a, "raw")) {
    if (pageRotation(p) % 180 == 90) std::swap(w, h);
    w *= userUnit(p);
    h *= userUnit(p);
  }
  const std::string& d = param(a, "dimension");
  return compare(Value(d == "width" ? w : d == "height" ? h : d == "longer" ? std::max(w, h) : std::min(w, h)), a);
}

int evFlatness(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  return objOf(s, o) ? compare(Value(o.x->flatness), a) : 0;
}

QPDFObjectHandle dictOf(QPDFObjectHandle h) { return h.isStream() ? h.getDict() : h; }

std::vector<QPDFObjectHandle> halftoneDicts(QPDFObjectHandle ht, const std::string& colorant) {
  std::vector<QPDFObjectHandle> out;
  QPDFObjectHandle d = dictOf(ht);
  if (!d.isDictionary()) return out;
  if (numOf(d.getKey("/HalftoneType"), 0) != 5) {
    out.push_back(d);
    return out;
  }
  if (colorant != "any") {
    std::string k = colorant;
    k[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(k[0])));
    QPDFObjectHandle c = dictOf(d.getKey("/" + k));
    if (!c.isDictionary()) c = dictOf(d.getKey("/Default"));
    if (c.isDictionary()) out.push_back(c);
    return out;
  }
  out.push_back(d);
  for (const std::string& k : d.getKeys()) {
    QPDFObjectHandle c = dictOf(d.getKey(k));
    if (c.isDictionary() && c.hasKey("/HalftoneType")) out.push_back(c);
  }
  return out;
}

int evHalftone(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o)) return 0;
  const std::string& prop = param(a, "property");
  std::vector<QPDFObjectHandle> ds = halftoneDicts(o.x->ht, param(a, "colorant"));
  if (prop == "present") return compare(Value(boolText(!ds.empty())), a);
  if (prop == "origin") return compare(Value(boolText(o.x->hasHalftoneOrigin)), a);
  if (prop == "phase") return compare(Value(boolText(o.x->hasHTP)), a);
  std::vector<std::string> v;
  for (QPDFObjectHandle d : ds) {
    QPDFObjectHandle x;
    if (prop == "type") x = d.getKey("/HalftoneType");
    else if (prop == "frequency") x = d.getKey("/Frequency");
    else if (prop == "angle") x = d.getKey("/Angle");
    else if (prop == "spot_function") x = d.getKey("/SpotFunction");
    else if (prop == "name") x = d.getKey("/HalftoneName");
    else x = d.getKey("/AccurateScreens");
    char buf[32];
    if (x.isNumber()) {
      std::snprintf(buf, sizeof buf, "%g", x.getNumericValue());
      v.push_back(buf);
    } else if (x.isName()) {
      v.push_back(x.getName().substr(1));
    } else if (x.isString()) {
      v.push_back(x.getUTF8Value());
    } else if (x.isBool()) {
      v.push_back(boolText(x.getBoolValue()));
    } else if (prop == "accurate_screens" && d.hasKey("/Frequency")) {
      v.push_back("false");
    }
  }
  if (v.empty()) return 0;
  return v.size() == 1 ? compare(Value(v[0]), a) : compare(Value(v), a);
}

bool halftoneTransferNotAllowed(QPDFObjectHandle ht) {
  QPDFObjectHandle d = dictOf(ht);
  if (!d.isDictionary()) return false;
  if (numOf(d.getKey("/HalftoneType"), 0) != 5) return d.hasKey("/TransferFunction");
  static const std::set<std::string> primary = {"/Cyan", "/Magenta", "/Yellow", "/Black", "/Red", "/Green", "/Blue", "/Gray", "/Default"};
  for (const std::string& k : d.getKeys()) {
    QPDFObjectHandle c = dictOf(d.getKey(k));
    if (c.isDictionary() && primary.count(k) && c.hasKey("/TransferFunction")) return true;
  }
  return false;
}

int evRenderingFunctions(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o)) return 0;
  const GsExtra& x = *o.x;
  std::vector<std::string> v;
  if ((x.hasTR && !x.trIdentity) || (x.hasTR2 && !x.tr2IsDefault && !x.tr2Identity)) v.push_back("transfer");
  if (x.hasTR && x.trIdentity) v.push_back("transfer_identity");
  if (x.hasTR2 && x.tr2IsDefault) v.push_back("transfer2_default");
  if (x.hasBG || (x.hasBG2 && !x.bg2Default)) v.push_back("black_generation");
  if (x.hasBG2 && x.bg2Default) v.push_back("black_generation2_default");
  if (x.hasUCR || (x.hasUCR2 && !x.ucr2Default)) v.push_back("undercolour_removal");
  if (x.hasUCR2 && x.ucr2Default) v.push_back("undercolour_removal2_default");
  if (halftoneTransferNotAllowed(x.ht)) v.push_back("halftone_transfer_not_allowed");
  return compare(Value(v), a);
}

std::string looseName(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (!std::isspace(static_cast<unsigned char>(c))) o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return o;
}

std::string lookKey(const ColorInfo& ci) {
  std::string k = ci.altName;
  char buf[32];
  for (double v : ci.altValues) {
    std::snprintf(buf, sizeof buf, "|%.3f", v);
    k += buf;
  }
  return k;
}

std::set<std::string> spotNaming(const Events& ev, int page) {
  std::map<std::string, std::set<std::string>> looks, procLooks;
  std::set<std::string> names;
  auto take = [&](const ColorInfo& ci) {
    if (ci.cls == "devicen") names.insert(ci.colorants.begin(), ci.colorants.end());
    if (ci.cls != "separation" || ci.spot.empty()) return;
    names.insert(ci.spot);
    if (ci.spot == "None") return;
    (processInks().count(ci.spot) || ci.spot == "All" ? procLooks : looks)[ci.spot].insert(lookKey(ci));
  };
  for (const PaintEvent& e : ev.paints) {
    if (page < 0 || e.page == page) take(e.color);
  }
  for (const TextEvent& e : ev.texts) {
    if (page < 0 || e.page == page) take(e.color);
  }
  for (const ImageEvent& e : ev.images) {
    if (page < 0 || e.page == page) take(e.color);
  }
  std::set<std::string> v;
  std::map<std::string, std::set<std::string>> byLook;
  for (const auto& [n, ls] : looks) {
    if (ls.size() > 1) v.insert("same_name_different_look");
    for (const std::string& l : ls) byLook[l].insert(n);
  }
  for (const auto& [l, ns] : byLook) {
    if (ns.size() > 1) v.insert("same_look_different_names");
  }
  for (const auto& [n, ls] : procLooks) {
    if (ls.size() > 1) v.insert("process_defined_differently");
  }
  std::map<std::string, std::set<std::string>> groups;
  for (const std::string& n : names) groups[looseName(n)].insert(n);
  for (const auto& [k, g] : groups) {
    if (g.size() > 1) v.insert("similar_names");
  }
  return v;
}

int evSpotNaming(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  std::set<std::string> v;
  if (param(a, "scope") == "document") {
    v = spotNaming(ev, -1);
  } else if (s.page > 0) {
    v = spotNaming(ev, s.page);
  } else {
    for (const PageFacts& p : ev.pages) {
      std::set<std::string> x = spotNaming(ev, p.page);
      v.insert(x.begin(), x.end());
    }
  }
  return compare(Value(std::vector<std::string>(v.begin(), v.end())), a);
}

std::string xmpTrapped(const std::string& x) {
  size_t p = x.find("pdf:Trapped");
  if (p == std::string::npos) return std::string();
  size_t q = p + 11;
  while (q < x.size() && (x[q] == ' ' || x[q] == '=' || x[q] == '"' || x[q] == '\'' || x[q] == '>')) ++q;
  size_t e = q;
  while (e < x.size() && std::isalpha(static_cast<unsigned char>(x[e]))) ++e;
  std::string v = x.substr(q, e - q);
  if (v == "true") return "True";
  if (v == "false") return "False";
  return v;
}

int evTrapped(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string& src = param(a, "source");
  std::string info = ev.infoTrapped;
  if (!info.empty() && info[0] == '/') info = info.substr(1);
  if (info == "true") info = "True";
  if (info == "false") info = "False";
  std::string xmp = xmpTrapped(ev.xmpRaw);
  std::string v = src == "info" ? info : src == "xmp" ? xmp : !info.empty() ? info : xmp;
  return compare(Value(v.empty() ? "missing" : v), a);
}

void hashInto(QPDFCryptoImpl& h, QPDFObjectHandle o, std::set<QPDFObjGen>& seen, int depth) {
  auto put = [&](const std::string& s) { h.SHA2_update(reinterpret_cast<const unsigned char*>(s.data()), s.size()); };
  if (depth > 64) return put("D");
  if (o.isIndirect()) {
    if (seen.count(o.getObjGen())) return put("R");
    seen.insert(o.getObjGen());
  }
  if (o.isStream()) {
    put("S<");
    QPDFObjectHandle d = o.getDict();
    for (auto& k : d.getKeys()) {
      if (k == "/Length" || k == "/Filter" || k == "/DecodeParms" || k == "/Parent" || k == "/P") continue;
      put(k);
      hashInto(h, d.getKey(k), seen, depth + 1);
    }
    std::string data;
    try {
      auto b = o.getStreamData(qpdf_dl_generalized);
      data.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
    } catch (std::exception&) {
      auto b = o.getRawStreamData();
      data.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
    }
    put(std::to_string(data.size()) + ">");
    put(data);
  } else if (o.isDictionary()) {
    put("<<");
    for (auto& k : o.getKeys()) {
      if (k == "/Parent" || k == "/P") continue;
      put(k);
      hashInto(h, o.getKey(k), seen, depth + 1);
    }
    put(">>");
  } else if (o.isArray()) {
    put("[");
    for (auto& it : o.aitems()) hashInto(h, it, seen, depth + 1);
    put("]");
  } else {
    put(o.unparse() + " ");
  }
}

std::string sealDigest(QPDF& pdf) {
  auto h = QPDFCryptoProvider::getImpl();
  h->SHA2_init(256);
  const auto& pages = pdf.getAllPages();
  std::string pc = "pages:" + std::to_string(pages.size()) + "\n";
  h->SHA2_update(reinterpret_cast<const unsigned char*>(pc.data()), pc.size());
  std::set<QPDFObjGen> seen;
  for (auto& pg : pages) {
    QPDFPageObjectHelper ph(pg);
    Pl_Buffer buf("seal");
    try {
      ph.pipeContents(&buf);
    } catch (std::exception&) {
    }
    std::string c = buf.getString();
    std::string hdr = "page:" + std::to_string(c.size()) + "\n";
    h->SHA2_update(reinterpret_cast<const unsigned char*>(hdr.data()), hdr.size());
    h->SHA2_update(reinterpret_cast<const unsigned char*>(c.data()), c.size());
    for (const char* k : {"/MediaBox", "/CropBox", "/Rotate"}) {
      std::string v = k + ph.getAttribute(k, false).unparseResolved();
      h->SHA2_update(reinterpret_cast<const unsigned char*>(v.data()), v.size());
    }
    hashInto(*h, ph.getAttribute("/Resources", false), seen, 0);
  }
  h->SHA2_finalize();
  return QUtil::hex_encode(h->SHA2_digest());
}

std::string sealState(const Events& ev) {
  QPDFObjectHandle pi = ev.root.isDictionary() ? ev.root.getKey("/PieceInfo") : QPDFObjectHandle();
  QPDFObjectHandle e = pi.isDictionary() ? pi.getKey("/BentoPDFKura") : QPDFObjectHandle();
  if (!e.isDictionary()) return "absent";
  QPDFObjectHandle p = e.getKey("/Private");
  if (!p.isDictionary() || !p.getKey("/Digest").isString() || !p.getKey("/Compliant").isBool() || !p.getKey("/Level").isString()) {
    return "invalid_metadata";
  }
  QPDF* q = ev.root.getOwningQPDF();
  if (q) {
    try {
      if (p.getKey("/Digest").getStringValue() != sealDigest(*q)) return "modified_since_sealed";
    } catch (...) {
      return "invalid_metadata";
    }
  }
  return p.getKey("/Compliant").getBoolValue() ? "present" : "sealed_with_errors";
}

int evSeal(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  DocCache& c = docCache(ev);
  if (!c.sealDone) {
    c.seal = sealState(ev);
    c.sealDone = true;
  }
  return compare(Value(c.seal), a);
}

void sigFields(QPDFObjectHandle f, int depth, bool& any, bool& signedOne) {
  if (!f.isDictionary() || depth > 32) return;
  if (nameIs(f.getKey("/FT"), "/Sig")) {
    any = true;
    if (f.getKey("/V").isDictionary()) signedOne = true;
  }
  QPDFObjectHandle k = f.getKey("/Kids");
  for (int i = 0; k.isArray() && i < k.getArrayNItems(); ++i) sigFields(k.getArrayItem(i), depth + 1, any, signedOne);
}

int evSignature(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  bool any = false, signedOne = false;
  QPDFObjectHandle acro = ev.root.isDictionary() ? ev.root.getKey("/AcroForm") : QPDFObjectHandle();
  QPDFObjectHandle fields = acro.isDictionary() ? acro.getKey("/Fields") : QPDFObjectHandle();
  for (int i = 0; fields.isArray() && i < fields.getArrayNItems(); ++i) sigFields(fields.getArrayItem(i), 0, any, signedOne);
  if (!paramBool(a, "signed_only") && ev.hasSigFields) any = true;
  return compare(Value(paramBool(a, "signed_only") ? signedOne : any), a);
}

bool isWs(unsigned char c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
bool isDelim(unsigned char c) { return std::strchr("()<>[]{}/%", c) != nullptr && c != 0; }

void scanStrings(const unsigned char* d, size_t n, bool& badHex, bool& oddHex, bool& badName) {
  size_t i = 0;
  while (i < n && !(badHex && oddHex && badName)) {
    unsigned char c = d[i];
    if (c == '%') {
      while (i < n && d[i] != '\r' && d[i] != '\n') ++i;
    } else if (c == '(') {
      int depth = 0;
      for (; i < n; ++i) {
        if (d[i] == '\\') {
          ++i;
          continue;
        }
        if (d[i] == '(') ++depth;
        if (d[i] == ')' && --depth == 0) break;
      }
      ++i;
    } else if (c == 's' && i + 6 < n && std::memcmp(d + i, "stream", 6) == 0 && (i == 0 || !std::isalpha(d[i - 1])) &&
               (d[i + 6] == '\r' || d[i + 6] == '\n' || d[i + 6] == ' ' || d[i + 6] == '\t')) {
      size_t k = i + 6;
      for (; k + 9 <= n; ++k) {
        if (d[k] == 'e' && std::memcmp(d + k, "endstream", 9) == 0) break;
      }
      i = k + 9;
    } else if (c == '<' && i + 1 < n && d[i + 1] == '<') {
      i += 2;
    } else if (c == '<') {
      size_t digits = 0;
      ++i;
      for (; i < n && d[i] != '>'; ++i) {
        if (std::isxdigit(d[i])) ++digits;
        else if (!isWs(d[i])) badHex = true;
      }
      if (digits % 2) oddHex = true;
      ++i;
    } else if (c == '/') {
      std::string name;
      ++i;
      while (i < n && !isWs(d[i]) && !isDelim(d[i])) {
        if (d[i] == '#' && i + 2 < n && std::isxdigit(d[i + 1]) && std::isxdigit(d[i + 2])) {
          name += static_cast<char>(std::strtol(std::string(reinterpret_cast<const char*>(d + i + 1), 2).c_str(), nullptr, 16));
          i += 3;
        } else {
          name += static_cast<char>(d[i++]);
        }
      }
      if (!validUtf8(name)) badName = true;
    } else {
      ++i;
    }
  }
}

std::vector<std::string> scanStringEncoding(const unsigned char* d, size_t n, const Events& ev) {
  bool badHex = false, oddHex = false, badName = false;
  scanStrings(d, n, badHex, oddHex, badName);
  QPDF* q = ev.root.isDictionary() ? ev.root.getOwningQPDF() : nullptr;
  std::vector<QPDFObjectHandle> streams;
  for (const PageFacts& p : ev.pages) {
    QPDFObjectHandle c = p.obj.isDictionary() ? p.obj.getKey("/Contents") : QPDFObjectHandle();
    if (c.isStream()) streams.push_back(c);
    for (int i = 0; c.isArray() && i < c.getArrayNItems(); ++i) streams.push_back(c.getArrayItem(i));
  }
  for (QPDFObjectHandle o : q ? q->getAllObjects() : std::vector<QPDFObjectHandle>()) {
    if (o.isStream() && nameIs(o.getDict().getKey("/Subtype"), "/Form")) streams.push_back(o);
  }
  for (QPDFObjectHandle st : streams) {
    if (!st.isStream() || (badHex && oddHex && badName)) continue;
    try {
      auto b = st.getStreamData(qpdf_dl_generalized);
      scanStrings(b->getBuffer(), b->getSize(), badHex, oddHex, badName);
    } catch (...) {
    }
  }
  std::vector<std::string> v;
  if (badHex) v.push_back("hex_invalid_character");
  if (oddHex) v.push_back("hex_odd_digits");
  if (badName) v.push_back("name_not_utf8");
  return v;
}

int evStringEncoding(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  DocCache& c = docCache(ev);
  if (!c.encodingDone) {
    if (ev.inputData) {
      c.encoding = scanStringEncoding(ev.inputData, ev.inputSize, ev);
    } else {
      std::string bytes;
      if (ev.inputPath.empty() || !QUtil::file_can_be_opened(ev.inputPath.c_str())) {
        supported = false;
        return 0;
      }
      try {
        bytes = QUtil::read_file_into_string(ev.inputPath.c_str());
      } catch (...) {
        supported = false;
        return 0;
      }
      c.encoding = scanStringEncoding(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), ev);
    }
    c.encodingDone = true;
  }
  return compare(Value(c.encoding), a);
}

int evUnknownOperator(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  static const std::set<std::string> v10 = {"b", "B", "b*", "B*", "BI", "c", "cm", "d", "d0", "d1", "Do", "EI", "f", "F",
                                            "f*", "G", "g", "h", "i", "ID", "j", "J", "K", "k", "l", "m", "M", "n", "q",
                                            "Q", "re", "RG", "rg", "s", "S", "T*", "Tc", "Td", "TD", "Tf", "TJ", "Tj",
                                            "TL", "Tm", "Tr", "Ts", "Tw", "Tz", "v", "w", "W", "W*", "y", "'", "\"", "BT", "ET"};
  static const std::set<std::string> v11 = {"BX", "EX", "cs", "CS", "sc", "SC", "ri"};
  static const std::set<std::string> v12 = {"gs", "scn", "SCN", "BMC", "BDC", "EMC", "MP", "DP"};
  static const std::set<std::string> v13 = {"sh"};
  int lvl = param(a, "version").back() - '0';
  bool unknown = false;
  for (const std::string& op : ev.contentOps) {
    bool known = v10.count(op) || (lvl >= 1 && v11.count(op)) || (lvl >= 2 && v12.count(op)) || (lvl >= 3 && v13.count(op));
    unknown = unknown || !known;
  }
  return compare(Value(unknown), a);
}

Mat textMatrix(const TextEvent& e) { return mul(Mat{e.x.hScale / 100.0, 0, 0, 1, 0, 0}, mul(e.x.tm, e.x.ctm)); }

int evTextGeometry(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.text) return 0;
  Mat m = textMatrix(*s.text);
  if (param(a, "measure") == "rotation") return compare(Value(angleOf(m.a, m.b)), a);
  double ux = std::hypot(m.a, m.b), vy = std::hypot(m.c, m.d);
  if (ux <= 0 || vy <= 0) return 0;
  double cosv = std::max(-1.0, std::min(1.0, (m.a * m.c + m.b * m.d) / (ux * vy)));
  double slant = std::fabs(90.0 - std::acos(cosv) * 180.0 / M_PI);
  return compare(Value(std::round(slant * 100.0) / 100.0), a);
}

int evGlyph(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.text) return 0;
  const TextEvent& e = *s.text;
  bool embedded = false;
  for (const FontFacts& f : ev.fonts) {
    if (f.og == e.fontOg) embedded = f.embedded;
  }
  std::vector<std::string> v;
  if (e.glyphUndefined) v.push_back(embedded ? "notdef" : "undefined");
  if (e.glyphWhitespace) v.push_back("whitespace");
  if (!e.glyphHasContour && !e.glyphWhitespace) v.push_back("empty_non_space");
  if (v.empty()) v.push_back("normal");
  if (e.x.actualText) v.push_back("has_actual_text");
  return compare(Value(v), a);
}

const FontFacts* textFont(const Subject& s, const Events& ev) {
  if (s.font) return s.font;
  if (!s.text) return nullptr;
  for (const FontFacts& f : ev.fonts) {
    if (f.og == s.text->fontOg) return &f;
  }
  return nullptr;
}

int evLegacyCodes(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.text) return 0;
  const FontFacts* ff = textFont(s, ev);
  bool legacy = false;
  if (ff && !ff->cid && !ff->type3 && ff->dict.isDictionary()) {
    QPDFObjectHandle enc = ff->dict.getKey("/Encoding");
    std::string base = enc.isName() ? enc.getName() : enc.isDictionary() ? nameOf(enc.getKey("/BaseEncoding")) : std::string();
    std::set<int> remapped;
    QPDFObjectHandle diffs = enc.isDictionary() ? enc.getKey("/Differences") : QPDFObjectHandle();
    int code = 0;
    for (int i = 0; diffs.isArray() && i < diffs.getArrayNItems(); ++i) {
      QPDFObjectHandle it = diffs.getArrayItem(i);
      if (it.isInteger()) code = static_cast<int>(it.getIntValue());
      else if (it.isName()) remapped.insert(code++);
    }
    for (unsigned char c : s.text->bytes) {
      if (remapped.count(c)) continue;
      if (base == "/WinAnsiEncoding" && (c == 173 || c == 160)) legacy = true;
      if (base == "/MacRomanEncoding" && c == 202) legacy = true;
    }
  }
  return compare(Value(legacy), a);
}

int evNegativeSize(const PfAtom& a, const Subject& s, const Events&, bool&) {
  return s.text ? compare(Value(s.text->x.negFontSize), a) : 0;
}

int evTextRendering(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.text) return 0;
  int m = s.text->renderMode;
  static const char* names[] = {"fill", "stroke", "fill_and_stroke", "invisible", "fill", "stroke", "fill_and_stroke", "clip_only"};
  return m >= 0 && m <= 7 ? compare(Value(names[m]), a) : 0;
}

int evTextSize(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.text) return 0;
  const TextEvent& e = *s.text;
  const std::string& m = param(a, "measure");
  if (m == "font_size") return compare(Value(std::fabs(e.sizePt)), a);
  const FontFacts* ff = textFont(s, ev);
  QPDFObjectHandle desc = ff ? fontDescriptor(*ff) : QPDFObjectHandle();
  double xh = desc.isDictionary() ? numOf(desc.getKey("/XHeight"), 0) : 0;
  if (xh <= 0) return 0;
  if (m == "x_height_unscaled") return compare(Value(xh), a);
  if (m == "x_height_percent") return compare(Value(xh / 10.0), a);
  Mat tm = textMatrix(e);
  return compare(Value(xh / 1000.0 * std::fabs(e.x.fontSize) * std::hypot(tm.c, tm.d)), a);
}

int evTextState(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.text) return 0;
  const GsExtra& x = s.text->x;
  const std::string& p = param(a, "parameter");
  double v = p == "render_mode" ? s.text->renderMode
             : p == "character_spacing" ? x.charSpacing
             : p == "word_spacing" ? x.wordSpacing
             : p == "horizontal_scaling" ? x.hScale
             : p == "leading" ? x.leading
             : x.rise;
  return compare(Value(v), a);
}

std::string firstOperator(QPDFObjectHandle st) {
  std::string t;
  try {
    auto b = st.getStreamData(qpdf_dl_all);
    t.assign(reinterpret_cast<const char*>(b->getBuffer()), std::min<size_t>(b->getSize(), 4096));
  } catch (...) {
    return std::string();
  }
  size_t i = 0;
  while (i < t.size()) {
    unsigned char c = static_cast<unsigned char>(t[i]);
    if (std::isspace(c)) {
      ++i;
    } else if (c == '%') {
      while (i < t.size() && t[i] != '\n' && t[i] != '\r') ++i;
    } else if (std::isalpha(c) || c == '\'' || c == '"') {
      size_t j = i;
      while (j < t.size() && (std::isalnum(static_cast<unsigned char>(t[j])) || t[j] == '*')) ++j;
      return t.substr(i, std::max<size_t>(j - i, 1));
    } else if (c == '[' || c == ']' || c == '(' || c == '<' || c == '/') {
      return std::string();
    } else {
      ++i;
    }
  }
  return std::string();
}

int evType3Operator(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.text) return 0;
  const FontFacts* ff = textFont(s, ev);
  QPDFObjectHandle cp = ff && ff->type3 && ff->dict.isDictionary() ? ff->dict.getKey("/CharProcs") : QPDFObjectHandle();
  if (!cp.isDictionary()) return 0;
  std::map<int, std::string> names;
  QPDFObjectHandle enc = ff->dict.getKey("/Encoding");
  QPDFObjectHandle diffs = enc.isDictionary() ? enc.getKey("/Differences") : QPDFObjectHandle();
  int code = 0;
  for (int i = 0; diffs.isArray() && i < diffs.getArrayNItems(); ++i) {
    QPDFObjectHandle it = diffs.getArrayItem(i);
    if (it.isInteger()) code = static_cast<int>(it.getIntValue());
    else if (it.isName()) names[code++] = it.getName();
  }
  std::set<std::string> ops;
  std::set<std::string> done;
  for (unsigned char c : s.text->bytes) {
    auto it = names.find(c);
    if (it == names.end() || !done.insert(it->second).second) continue;
    QPDFObjectHandle pr = cp.getKey(it->second);
    std::string op = pr.isStream() ? firstOperator(pr) : std::string();
    if (op == "d0" || op == "d1") ops.insert(op);
  }
  return compare(Value(std::vector<std::string>(ops.begin(), ops.end())), a);
}

int evTextUnicode(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.text) return 0;
  const TextEvent& e = *s.text;
  std::set<std::string> v;
  v.insert(e.mappedToUnicode ? "mapped" : "unmapped");
  const FontFacts* ff = textFont(s, ev);
  if (ff && !ff->type3 && ff->dict.isDictionary()) {
    std::string enc = nameOf(ff->dict.getKey("/Encoding"));
    bool twoByte = ff->cid;
    if (!twoByte || enc == "/Identity-H" || enc == "/Identity-V") {
      const std::map<uint32_t, std::u32string>& tum = toUnicodeMap(*ff, ev);
      static const std::set<std::u32string> ligs = {U"ff", U"fi", U"fl", U"ffi", U"ffl"};
      auto known = ev.toUnicodeDests.find(ff->og);
      if (known == ev.toUnicodeDests.end()) {
        known = ev.toUnicodeDests.emplace(ff->og, std::map<std::u32string, int>()).first;
        for (const auto& [c, u] : tum) {
          if (!u.empty()) ++known->second[u];
        }
      }
      const std::map<std::u32string, int>& dests = known->second;
      size_t step = twoByte ? 2 : 1;
      for (size_t i = 0; !tum.empty() && i + step <= e.bytes.size(); i += step) {
        uint32_t code = twoByte ? (static_cast<unsigned char>(e.bytes[i]) << 8) | static_cast<unsigned char>(e.bytes[i + 1])
                                : static_cast<unsigned char>(e.bytes[i]);
        auto it = tum.find(code);
        if (it == tum.end()) continue;
        const std::u32string& u = it->second;
        for (char32_t cp : u) {
          if (isPuaCodepoint(static_cast<uint32_t>(cp))) v.insert("private_use");
          if (cp >= 0xFB00 && cp <= 0xFB06) v.insert("ligature");
          if (cp == 0) v.insert("zero");
          if (cp == 0xFEFF || cp == 0xFFFE) v.insert("inappropriate");
        }
        if (ligs.count(u)) v.insert("ligature");
        auto d = dests.find(u);
        if (d != dests.end() && d->second > 1) v.insert("ambiguous");
      }
    }
  }
  return compare(Value(std::vector<std::string>(v.begin(), v.end())), a);
}

int evAlpha(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o) || !paintSelected(a, o)) return 0;
  return compare(Value(o.stroke && !o.fill ? o.x->alphaStroke : o.x->alphaFill), a);
}

int evBlendMode(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o)) return 0;
  static const std::map<std::string, std::vector<std::string>> shorthands = {
      {"separable", {"Normal", "Compatible", "Multiply", "Screen", "Overlay", "Darken", "Lighten", "ColorDodge", "ColorBurn",
                     "HardLight", "SoftLight", "Difference", "Exclusion"}},
      {"non_separable", {"Hue", "Saturation", "Color", "Luminosity"}}};
  PfAtom b = expandShorthands(a, shorthands);
  if (!paramBool(a, "inherited")) return compare(Value(o.x->blendMode), b);
  std::vector<std::string> modes = o.x->blendChain;
  modes.push_back(o.x->blendMode);
  return compare(Value(modes), b);
}

int evGroup(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o)) return 0;
  const GsExtra& x = *o.x;
  std::vector<std::string> v;
  bool in = x.inTransGroup || x.groupDepth > 0;
  if (in) v.push_back("member");
  if (x.groupDepth > 0 && x.groupIsolated) v.push_back("isolated");
  if (x.groupDepth > 0 && x.groupKnockout) v.push_back("knockout");
  if (x.tk) v.push_back("text_knockout");
  if (x.groupDepth > 0 && x.groupHasCS) v.push_back("has_colour_space");
  if (x.groupDepth > 0 && !x.groupCSInner.empty() && x.groupCSDevInd) v.push_back("device_independent_blend_space");
  return compare(Value(v), a);
}

int evGroupColourSpace(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o)) return 0;
  const GsExtra& x = *o.x;
  const std::string& w = param(a, "which");
  std::string v = w == "luminosity_mask" ? (x.hasSMask && x.smaskIsLuminosity ? x.smaskGroupCS : std::string())
                  : x.groupDepth <= 0 ? std::string()
                  : w == "outermost" ? x.groupCSOuter
                  : w == "base" ? x.groupBaseCS
                  : x.groupCSInner;
  return v.empty() ? 0 : compare(Value(v), a);
}

int evGroupDepth(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  return objOf(s, o) ? compare(Value(o.x->groupDepth), a) : 0;
}

int evKeysPresent(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  return objOf(s, o) ? compare(Value(o.x->transKeys || o.x->smaskExplicitNone || o.x->hasSMask), a) : 0;
}

int evSoftMask(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o)) return 0;
  const GsExtra& x = *o.x;
  std::vector<std::string> v;
  if (x.smaskExplicitNone) v.push_back("none");
  if (x.hasSMask) {
    if (x.smaskIsAlpha) v.push_back("alpha");
    if (x.smaskIsLuminosity) v.push_back("luminosity");
    if (x.smaskHasBC) v.push_back("backdrop_colour");
    if (x.smaskHasTR) v.push_back("transfer_function");
    if (x.smaskHasTR && x.smaskTRIdentity) v.push_back("identity_transfer");
  }
  return compare(Value(v), a);
}

int evTransparencyUsed(const PfAtom& a, const Subject& s, const Events&, bool&) {
  Obj o;
  if (!objOf(s, o) || !paintSelected(a, o)) return 0;
  const GsExtra& x = *o.x;
  bool sm = x.hasSMask;
  const std::string& p = param(a, "paint");
  bool strokeSide = p == "stroke" || (p == "any" && o.stroke && !o.fill);
  bool alpha = strokeSide ? x.alphaStroke < 1.0 : x.alphaFill < 1.0;
  if (p == "any" && o.fill && o.stroke) alpha = x.alphaFill < 1.0 || x.alphaStroke < 1.0;
  bool blend = !x.blendMode.empty() && x.blendMode != "Normal" && x.blendMode != "Compatible";
  bool img = false;
  if (s.image) {
    QPDFObjectHandle d = s.image->obj.isStream() ? s.image->obj.getDict() : QPDFObjectHandle();
    img = s.image->hasSMask || (d.isDictionary() && numOf(d.getKey("/SMaskInData"), 0) > 0);
  }
  const std::string& how = param(a, "how");
  bool r = how == "soft_mask" ? sm : how == "alpha" ? alpha : how == "blend_mode" ? blend : how == "image_soft_mask" ? img : sm || alpha || blend || img;
  return compare(Value(r), a);
}

bool formHasMcid(QPDFObjectHandle form) {
  thread_local std::map<std::pair<const QPDF*, QPDFObjGen>, bool> cache;
  if (!form.isStream()) return false;
  auto key = std::make_pair(static_cast<const QPDF*>(form.getOwningQPDF()), form.getObjGen());
  auto it = cache.find(key);
  if (it != cache.end() && form.isIndirect()) return it->second;
  bool has = false;
  try {
    auto b = form.getStreamData(qpdf_dl_all);
    std::string t(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
    has = t.find("/MCID") != std::string::npos;
  } catch (...) {
  }
  if (form.isIndirect()) cache[key] = has;
  return has;
}

int evFormProblem(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  Obj o;
  if (!objOf(s, o)) return 0;
  const GsExtra& x = *o.x;
  std::vector<std::string> v;
  if (x.resInherited) v.push_back("inherits_resources");
  QPDFObjectHandle fd = x.formDict;
  if (x.inForm && fd.isDictionary()) {
    DocCache& c = docCache(ev);
    if (!c.formsDone) {
      c.formsDone = true;
      QPDF* q = ev.root.isDictionary() ? ev.root.getOwningQPDF() : nullptr;
      for (QPDFObjectHandle st : q ? q->getAllObjects() : std::vector<QPDFObjectHandle>()) {
        if (st.isStream() && nameIs(st.getDict().getKey("/Subtype"), "/Form")) c.forms.push_back(st);
      }
    }
    QPDFObjectHandle form;
    for (QPDFObjectHandle st : c.forms) {
      if (st.getDict().isSameObjectAs(fd)) {
        form = st;
        break;
      }
    }
    if (form.isStream() && formHasMcid(form)) {
      v.push_back(fd.hasKey("/StructParents") ? "contains_referenced_marked_content" : "marked_content_ids_not_referenced");
    }
  }
  return compare(Value(v), a);
}

int evAnnotType(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.annot) return 0;
  return compare(Value(s.annot->knownType ? s.annot->subtype : std::string("unknown")), a);
}

int evUsesPlates(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.pageFacts) return 0;
  int pg = s.pageFacts->page;
  double minInk = paramNum(a, "min_ink") / 100.0;
  bool ignoreWhite = paramBool(a, "ignore_white");
  std::set<std::string> want;
  for (const std::string& p : paramList(a, "plates")) {
    if (ignoreWhite && (lowerAscii(p) == "white" || p == "None")) continue;
    want.insert(p);
  }
  bool used = false;
  std::vector<std::pair<std::string, double>> inks;
  auto check = [&](const ColorInfo& ci, const std::vector<double>& c) {
    if (used || !inksOf(ci, c, inks)) return;
    for (const auto& [n, v] : inks) {
      if (want.count(n) && v > std::max(minInk, kInkEps)) used = true;
    }
  };
  for (const PaintEvent& e : ev.paints) {
    if (e.page == pg && !e.noPaint) check(e.color, e.comps);
  }
  for (const TextEvent& e : ev.texts) {
    if (e.page == pg && e.renderMode != 3 && e.renderMode != 7) check(e.color, e.comps);
  }
  for (const ImageEvent& e : ev.images) {
    if (e.page != pg || used) continue;
    if (e.mask) {
      check(e.color, e.comps);
      continue;
    }
    const ColorInfo& ci = e.color;
    if (ci.cls == "cmyk" || (ci.cls == "icc" && ci.declaredComps == 4)) {
      for (const std::string& p : processInks()) used = used || want.count(p);
    } else if (ci.cls == "gray" || (ci.cls == "icc" && ci.declaredComps == 1)) {
      used = used || want.count("Black");
    } else if (ci.cls == "separation" || ci.cls == "devicen") {
      for (const std::string& n : ci.colorants) used = used || want.count(n);
    }
  }
  return compare(Value(used), a);
}
}

void registerColorChecks(CheckTable& t) {
  t.add("annotation.type", evAnnotType);
  t.add("color.black_point_compensation", evBpc);
  t.add("color.channel_spread", evChannelSpread);
  t.add("color.component_count", evComponentCount);
  t.add("color.devicen", evDeviceN);
  t.add("color.devicen_colorants", evDeviceNColorants);
  t.add("color.ink", evInk);
  t.add("color.ink_mix", evInkMix);
  t.add("color.inks_used", evInksUsed);
  t.add("color.overprint", evOverprint);
  t.add("color.rendering_intent", evRenderingIntent);
  t.add("color.space", evColorSpace);
  t.add("color.spot", evSpot);
  t.add("color.total_ink", evTotalInk);
  t.add("color.value", evColorValue);
  t.add("content.box_relation", evBoxRelation);
  t.add("content.clipped", evClipped);
  t.add("content.context", evContext);
  t.add("content.dash_length", evDashLength);
  t.add("content.distance_to_box", evDistanceToBox);
  t.add("content.distorted", evDistorted);
  t.add("content.kind", evKind);
  t.add("content.outside_share", evOutsideShare);
  t.add("content.painting", evPainting);
  t.add("content.path_measure", evPathMeasure);
  t.add("content.shading", evShading);
  t.add("content.size", evSize);
  t.add("content.stroke_style", evStrokeStyle);
  t.add("content.stroke_width", evStrokeWidth);
  t.add("document.data_after_eof", evDataAfterEof);
  t.add("document.file_size", evFileSize);
  t.add("document.page_count", evPageCount);
  t.add("document.pdf_version", evPdfVersion);
  t.add("document.requirements", evRequirements);
  t.add("font.all_text_unicode", evAllTextUnicode);
  t.add("font.embedding", evEmbedding);
  t.add("font.license", evLicense);
  t.add("font.name", evFontName);
  t.add("font.name_problem", evNameProblem);
  t.add("image.resolution", evResolution);
  t.add("image.rotation", evImageRotation);
  t.add("layer.processing_step", evProcessingStep);
  t.add("page.processing_steps", evProcessingStep);
  t.add("layer.processing_step_problem", evProcessingStepProblem);
  t.add("layer.visible", evLayerVisible);
  t.add("output_intent.count", evOutputIntentCount);
  t.add("page.aspect_ratio", evAspectRatio);
  t.add("page.box_present", evBoxPresent);
  t.add("page.in_range", evInRange);
  t.add("page.mostly_rotated_text", evMostlyRotatedText);
  t.add("page.number", evPageNumber);
  t.add("page.parity", evParity);
  t.add("page.rotation", evPageRotation);
  t.add("page.single_image", evSingleImage);
  t.add("page.size", evPageSize);
  t.add("page.text_contains", evTextContains);
  t.add("page.user_unit", evUserUnit);
  t.add("print.flatness", evFlatness);
  t.add("print.halftone", evHalftone);
  t.add("print.rendering_functions", evRenderingFunctions);
  t.add("print.spot_naming", evSpotNaming);
  t.add("print.trapped", evTrapped);
  t.add("print.uses_plates", evUsesPlates);
  t.add("seal.state", evSeal);
  t.add("signature.present", evSignature);
  t.add("syntax.string_encoding", evStringEncoding);
  t.add("syntax.unknown_operator", evUnknownOperator);
  t.add("text.geometry", evTextGeometry);
  t.add("text.glyph", evGlyph);
  t.add("text.legacy_codes", evLegacyCodes);
  t.add("text.negative_size", evNegativeSize);
  t.add("text.rendering", evTextRendering);
  t.add("text.size", evTextSize);
  t.add("text.state", evTextState);
  t.add("text.type3_glyph_operator", evType3Operator);
  t.add("text.unicode", evTextUnicode);
  t.add("transparency.alpha", evAlpha);
  t.add("transparency.blend_mode", evBlendMode);
  t.add("transparency.group", evGroup);
  t.add("transparency.group_colour_space", evGroupColourSpace);
  t.add("transparency.group_depth", evGroupDepth);
  t.add("transparency.keys_present", evKeysPresent);
  t.add("transparency.soft_mask", evSoftMask);
  t.add("transparency.used", evTransparencyUsed);
  t.add("xobject.form_problem", evFormProblem);
  t.add("page.box_offset", evBoxOffset);
  t.add("page.box_size", evBoxSize);
}
}
