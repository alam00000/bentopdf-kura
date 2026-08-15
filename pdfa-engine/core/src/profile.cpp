#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "passes.hh"
#include "util.hh"

namespace pdfa {
namespace {
struct PfAtom {
  std::string token;
  std::string op;
  std::vector<std::string> vals;
};

struct PfCondition {
  std::vector<PfAtom> atoms;
};

struct PfRule {
  std::string id;
  std::string name;
  int severity = 1;
  int logic = 0;
  std::vector<std::string> condIds;
};

struct PfProfile {
  std::string name;
  std::map<std::string, PfCondition> conds;
  std::vector<PfRule> rules;
};

std::string tagText(const std::string& s, const std::string& tag, size_t from,
                    size_t limit) {
  size_t o = s.find("<" + tag + ">", from);
  if (o == std::string::npos || o > limit) return std::string();
  o += tag.size() + 2;
  size_t c = s.find("</" + tag + ">", o);
  if (c == std::string::npos || c > limit) return std::string();
  return s.substr(o, c - o);
}

std::string canonToken(std::string t) {
  size_t p = t.find("::");
  if (p != std::string::npos && p + 2 < t.size()) {
    t[p + 2] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[p + 2])));
  }
  return t;
}

std::string unescape(std::string v) {
  struct {
    const char* from;
    const char* to;
  } reps[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"},
              {"&amp;", "&"}};
  for (auto& r : reps) {
    size_t p = 0;
    while ((p = v.find(r.from, p)) != std::string::npos) {
      v.replace(p, std::strlen(r.from), r.to);
      ++p;
    }
  }
  return v;
}

struct Json {
  enum Type { kNull, kBool, kNum, kStr, kArr, kObj } type = kNull;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  const Json* get(const std::string& key) const {
    for (const auto& kv : obj) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }
};

struct JsonParser {
  const std::string& s;
  size_t i = 0;
  bool ok = true;

  explicit JsonParser(const std::string& text) : s(text) {}

  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
      ++i;
    }
  }

  Json parse() {
    ws();
    if (i >= s.size()) { ok = false; return {}; }
    char c = s[i];
    if (c == '{') return parseObj();
    if (c == '[') return parseArr();
    if (c == '"') return parseStr();
    if (c == 't' || c == 'f') return parseBool();
    if (c == 'n') { i += 4; return {}; }
    return parseNum();
  }

  Json parseObj() {
    Json v;
    v.type = Json::kObj;
    ++i;
    ws();
    if (i < s.size() && s[i] == '}') { ++i; return v; }
    while (i < s.size()) {
      ws();
      Json key = parseStr();
      ws();
      if (i >= s.size() || s[i] != ':') { ok = false; return v; }
      ++i;
      v.obj.push_back({key.str, parse()});
      ws();
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      break;
    }
    if (i < s.size() && s[i] == '}') ++i;
    else ok = false;
    return v;
  }

  Json parseArr() {
    Json v;
    v.type = Json::kArr;
    ++i;
    ws();
    if (i < s.size() && s[i] == ']') { ++i; return v; }
    while (i < s.size()) {
      v.arr.push_back(parse());
      ws();
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      break;
    }
    if (i < s.size() && s[i] == ']') ++i;
    else ok = false;
    return v;
  }

  Json parseStr() {
    Json v;
    v.type = Json::kStr;
    if (i >= s.size() || s[i] != '"') { ok = false; return v; }
    ++i;
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\' && i + 1 < s.size()) {
        char e = s[i + 1];
        if (e == 'n') v.str += '\n';
        else if (e == 't') v.str += '\t';
        else if (e == 'u' && i + 5 < s.size()) { v.str += '?'; i += 4; }
        else v.str += e;
        i += 2;
      } else {
        v.str += s[i++];
      }
    }
    ++i;
    return v;
  }

  Json parseBool() {
    Json v;
    v.type = Json::kBool;
    if (s.compare(i, 4, "true") == 0) { v.b = true; i += 4; }
    else { i += 5; }
    return v;
  }

  Json parseNum() {
    Json v;
    v.type = Json::kNum;
    size_t start = i;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' ||
                            s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E')) {
      ++i;
    }
    v.num = std::atof(s.substr(start, i - start).c_str());
    if (i == start) ok = false;
    return v;
  }
};

const std::map<std::string, std::string>& kuraPropMap() {
  static const std::map<std::string, std::string> m = {
      {"stroke.width", "CSGST_S::LineWidth"},
      {"paint.inkCount", "CSCOLOR::NumberOfNonZeroComponents"},
      {"paint.maxInkPercent", "CSCOLOR::ObjectHasNonZeroValuesAndLowe"},
      {"paint.isWhite", "CSCOLOR::ObjectIsWhite"},
      {"paint.isBlackOnly", "CSCOLOR::ObjectUsesBlackOnly"},
      {"paint.richBlackCmyPercent", "CSCOLOR::BlackObjUsesCMYwithAPercentageOf"},
      {"text.size", "CSTEXT::Textsize"},
      {"text.isInvisible", "CSTEXT::TextIsNotRenderAndNotUsedAsCl"},
      {"image.ppi", "CSIMAGE::Resolution"},
      {"image.bitsPerComponent", "CSIMAGE::BitsPerColourComponent"},
      {"image.filter", "CSIMAGE::CompressionFilter"},
      {"doc.fileSizeBytes", "DOC::Filesize"},
      {"page.allHaveMediaBox", "PAGE::HasMediaBox"},
  };
  return m;
}

std::string kuraOpMap(const std::string& op) {
  if (op == "<") return "less";
  if (op == "<=") return "less_or_equal";
  if (op == ">") return "greater";
  if (op == ">=") return "greater_or_equal";
  if (op == "==" || op == "is") return "equal";
  if (op == "!=" || op == "isNot") return "unequal";
  return op;
}

std::string propToToken(const std::string& prop) {
  auto it = kuraPropMap().find(prop);
  if (it != kuraPropMap().end()) return it->second;
  static const std::map<std::string, std::string> head = {
      {"paint", "CSCOLOR"}, {"stroke", "CSGST_S"}, {"fill", "CSGST_F"},
      {"gstate", "CSGST_G"}, {"text", "CSTEXT"}, {"font", "CSFONT"},
      {"image", "CSIMAGE"}, {"page", "PAGE"}, {"doc", "DOC"}, {"docinfo", "DOCINFO"},
      {"annot", "ANNOT"}, {"content", "CONTSTM"}, {"outputIntent", "OUTINTENTS"},
      {"layers", "OPTIONALCONT"}, {"halftone", "CSHALFTONE"}, {"icc", "CSICC"},
      {"syntax", "DVASYNTAX"}, {"contentSyntax", "DVACSTRM"}, {"structure", "DVASTRUCT"},
      {"certificate", "CERTIFY"}, {"vt", "PDFVT"}, {"compare", "SIFTER"},
      {"tagging", "STRUCTPDF"}, {"form", "ACROFORM"}, {"postscript", "POSTSCRIPT"},
  };
  size_t dot = prop.find('.');
  if (dot == std::string::npos) return "KURA::" + prop;
  auto hit = head.find(prop.substr(0, dot));
  if (hit == head.end()) return "KURA::" + prop;
  std::string tail = prop.substr(dot + 1);
  if (!tail.empty()) tail[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(tail[0])));
  return hit->second + "::" + tail;
}

bool parseKuraJson(const std::string& text, PfProfile& out) {
  JsonParser jp(text);
  Json root = jp.parse();
  if (!jp.ok || root.type != Json::kObj || !root.get("kura-profile")) return false;
  const Json* name = root.get("name");
  if (name) out.name = name->str;
  const Json* checks = root.get("checks");
  if (!checks || checks->type != Json::kArr) return false;
  int serial = 0;
  for (const Json& c : checks->arr) {
    if (c.type != Json::kObj) continue;
    PfRule rule;
    rule.id = "K" + std::to_string(++serial);
    const Json* rn = c.get("name");
    rule.name = rn ? rn->str : rule.id;
    const Json* sev = c.get("severity");
    std::string sv = sev ? sev->str : "info";
    rule.severity = sv == "error" ? 3 : (sv == "warning" ? 2 : 1);
    const Json* any = c.get("any");
    std::vector<const Json*> groups;
    if (any && any->type == Json::kArr) {
      rule.logic = 1;
      for (const Json& g : any->arr) {
        const Json* ga = g.get("all");
        if (ga && ga->type == Json::kArr) groups.push_back(ga);
      }
    } else {
      const Json* all = c.get("all");
      if (all && all->type == Json::kArr) groups.push_back(all);
    }
    if (groups.empty()) continue;
    int gi = 0;
    for (const Json* grp : groups) {
      PfCondition cond;
    for (const Json& a : grp->arr) {
      if (a.type != Json::kObj) continue;
      PfAtom atom;
      const Json* prop = a.get("prop");
      if (!prop) continue;
      atom.token = propToToken(prop->str);
      const Json* op = a.get("op");
      atom.op = kuraOpMap(op ? op->str : "==");
      const Json* val = a.get("value");
      if (val) {
        if (val->type == Json::kNum) {
          char buf[40];
          std::snprintf(buf, sizeof(buf), "%g", val->num);
          atom.vals.push_back(buf);
        } else if (val->type == Json::kStr) {
          atom.vals.push_back(val->str);
        } else if (val->type == Json::kBool) {
          atom.op = val->b ? (atom.op == "unequal" ? "is_not_true" : "is_true")
                          : (atom.op == "unequal" ? "is_true" : "is_not_true");
        }
      }
      cond.atoms.push_back(atom);
    }
      std::string cid = "C" + rule.id + "_" + std::to_string(gi++);
      out.conds[cid] = cond;
      rule.condIds.push_back(cid);
    }
    out.rules.push_back(rule);
  }
  return !out.rules.empty();
}

bool parsePreflightXml(const std::string& text, PfProfile& out) {
  size_t pos = 0;
  while ((pos = text.find("<condition>", pos)) != std::string::npos) {
    size_t end = text.find("</condition>", pos);
    if (end == std::string::npos) break;
    std::string id = tagText(text, "id1", pos, end);
    if (id.empty() || id[0] != 'C') {
      pos = end + 1;
      continue;
    }
    PfCondition cond;
    size_t apos = pos;
    while (true) {
      apos = text.find("<atom>", apos);
      if (apos == std::string::npos || apos > end) break;
      size_t aend = text.find("</atom>", apos);
      if (aend == std::string::npos || aend > end) break;
      PfAtom atom;
      atom.token = canonToken(tagText(text, "token", apos, aend));
      std::string op = tagText(text, "operator", apos, aend);
      if (op.rfind("OP::", 0) == 0) op = op.substr(4);
      atom.op = op;
      size_t vpos = apos;
      while (true) {
        vpos = text.find("<value", vpos);
        if (vpos == std::string::npos || vpos > aend) break;
        size_t vopen = text.find('>', vpos);
        size_t vclose = text.find("</value>", vopen);
        if (vopen == std::string::npos || vclose == std::string::npos || vclose > aend) {
          break;
        }
        atom.vals.push_back(unescape(text.substr(vopen + 1, vclose - vopen - 1)));
        vpos = vclose + 1;
      }
      cond.atoms.push_back(atom);
      apos = aend + 1;
    }
    out.conds[id] = cond;
    pos = end + 1;
  }
  pos = 0;
  while ((pos = text.find("<rule>", pos)) != std::string::npos) {
    size_t end = text.find("</rule>", pos);
    if (end == std::string::npos) break;
    PfRule rule;
    rule.id = tagText(text, "id1", pos, end);
    rule.name = unescape(tagText(text, "name", pos, end));
    size_t lp = text.find("condition_logic=\"", pos);
    if (lp != std::string::npos && lp < end) rule.logic = text[lp + 17] - '0';
    size_t cpos = text.find("<conditions>", pos);
    size_t cend = text.find("</conditions>", pos);
    if (cpos != std::string::npos && cend != std::string::npos && cend < end) {
      size_t p = cpos;
      while (true) {
        p = text.find("<condition>", p);
        if (p == std::string::npos || p > cend) break;
        size_t q = text.find("</condition>", p);
        rule.condIds.push_back(text.substr(p + 11, q - p - 11));
        p = q + 1;
      }
    }
    if (!rule.id.empty() && rule.id[0] == 'R') out.rules.push_back(rule);
    pos = end + 1;
  }
  pos = text.find("<rulesets>");
  if (pos != std::string::npos) {
    size_t end = text.find("</rulesets>", pos);
    size_t p = pos;
    while (true) {
      p = text.find("<rule check_severity=\"", p);
      if (p == std::string::npos || p > end) break;
      int sev = text[p + 22] - '0';
      size_t open = text.find('>', p);
      size_t close = text.find("</rule>", open);
      std::string rid = text.substr(open + 1, close - open - 1);
      for (auto& r : out.rules) {
        if (r.id == rid) r.severity = sev;
      }
      p = close + 1;
    }
  }
  size_t ppos = text.find("<profile>");
  if (ppos != std::string::npos) {
    out.name = unescape(tagText(text, "name", ppos, text.size()));
  }
  return !out.rules.empty();
}

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

double matScale(const Mat& m) {
  return (std::hypot(m.a, m.b) + std::hypot(m.c, m.d)) / 2.0;
}

struct ColorInfo {
  std::string cls = "gray";
  std::string spot;
  int declaredComps = 1;
};

struct PaintEvent {
  double width = 0;
  std::vector<double> comps;
  int page = 0;
  bool stroke = false;
  ColorInfo color;
  bool overprint = false;
  int opm = 0;
  bool transparency = false;
};

struct TextEvent {
  double sizePt = 0;
  int renderMode = 0;
  std::vector<double> comps;
  int page = 0;
  ColorInfo color;
  bool overprint = false;
  bool transparency = false;
};

struct ImageEvent {
  double ppi = 0;
  int bpc = 8;
  int width = 0;
  int height = 0;
  bool mask = false;
  bool hasSMask = false;
  std::set<std::string> filters;
  ColorInfo color;
  int page = 0;
};

struct PageFacts {
  bool hasMediaBox = false;
  bool hasCropBox = false;
  bool cropEqualsMedia = true;
  bool scaled = false;
  bool rotated = false;
  bool empty = true;
  int imageCount = 0;
  int page = 0;
};

struct Events {
  std::vector<PaintEvent> paints;
  std::vector<TextEvent> texts;
  std::vector<ImageEvent> images;
  std::vector<PageFacts> pages;
  std::set<std::string> baseFonts;
  std::set<std::string> annotTypes;
  std::set<std::string> spotPlates;
  double filesize = 0;
  int pagesWithMediaBox = 0;
  int pageCount = 0;
  bool hasOutputIntent = false;
  int outputIntentCount = 0;
  std::string iccColorSpace;
  std::string iccProfileId;
};

struct Gs {
  Mat ctm;
  double lineWidth = 1.0;
  int renderMode = 0;
  double fontSize = 0;
  double tmScale = 1.0;
  std::vector<double> fill{0};
  std::vector<double> stroke{0};
  ColorInfo fillColor;
  ColorInfo strokeColor;
  bool overprintFill = false;
  bool overprintStroke = false;
  int opm = 0;
  bool transparency = false;
};

ColorInfo classifyColor(QPDFObjectHandle cs, QPDFObjectHandle res, int depth = 0) {
  ColorInfo ci;
  if (depth > 4) return ci;
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
    return ci;
  }
  if (!cs.isArray() || cs.getArrayNItems() < 1) return ci;
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
  } else if (fam == "/DeviceN" && cs.getArrayNItems() >= 2 &&
             cs.getArrayItem(1).isArray()) {
    ci.cls = "devicen";
    ci.declaredComps = cs.getArrayItem(1).getArrayNItems();
    QPDFObjectHandle names = cs.getArrayItem(1);
    if (names.getArrayNItems() > 0) {
      std::string nm = nameOf(names.getArrayItem(0));
      if (nm.size() > 1) ci.spot = nm.substr(1);
    }
  } else if (fam == "/Indexed" || fam == "/I") {
    if (cs.getArrayNItems() >= 2) return classifyColor(cs.getArrayItem(1), res, depth + 1);
  } else if (fam == "/Pattern") {
    ci.cls = "pattern";
    ci.declaredComps = 0;
  }
  return ci;
}

struct EvScanner : QPDFObjectHandle::ParserCallbacks {
  Gs gs;
  std::vector<Gs> stack;
  std::vector<double> nums;
  std::string lastName;
  QPDFObjectHandle res;
  Events& ev;
  int page;
  std::vector<std::pair<std::string, Gs>> draws;

  EvScanner(QPDFObjectHandle resources, Events& events, int pageNum, const Gs& initial)
      : gs(initial), res(resources), ev(events), page(pageNum) {}

  void applyExtGState() {
    if (lastName.empty() || !res.isDictionary()) return;
    QPDFObjectHandle egs = res.getKey("/ExtGState");
    if (!egs.isDictionary()) return;
    QPDFObjectHandle g = egs.getKey(lastName);
    if (!g.isDictionary()) return;
    if (g.getKey("/LW").isNumber()) gs.lineWidth = g.getKey("/LW").getNumericValue();
    if (g.getKey("/OP").isBool()) gs.overprintStroke = g.getKey("/OP").getBoolValue();
    if (g.getKey("/op").isBool()) gs.overprintFill = g.getKey("/op").getBoolValue();
    else if (g.getKey("/OP").isBool()) gs.overprintFill = g.getKey("/OP").getBoolValue();
    if (g.getKey("/OPM").isInteger()) gs.opm = static_cast<int>(g.getKey("/OPM").getIntValue());
    bool tr = false;
    if (g.getKey("/CA").isNumber() && g.getKey("/CA").getNumericValue() < 1.0) tr = true;
    if (g.getKey("/ca").isNumber() && g.getKey("/ca").getNumericValue() < 1.0) tr = true;
    QPDFObjectHandle sm = g.getKey("/SMask");
    if (!sm.isNull() && !nameIs(sm, "/None")) tr = true;
    QPDFObjectHandle bm = g.getKey("/BM");
    std::string bmName = nameOf(bm);
    if (bm.isArray() && bm.getArrayNItems() > 0) bmName = nameOf(bm.getArrayItem(0));
    if (!bmName.empty() && bmName != "/Normal" && bmName != "/Compatible") tr = true;
    if (tr) gs.transparency = true;
  }

  void setSpace(bool stroke) {
    if (lastName.empty()) return;
    ColorInfo ci = classifyColor(QPDFObjectHandle::newName(lastName), res);
    if (stroke) gs.strokeColor = ci;
    else gs.fillColor = ci;
  }

  void addPaint(bool stroke, bool fill) {
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
      recordSpot(e.color);
      ev.paints.push_back(e);
    }
    if (fill) {
      PaintEvent e;
      e.comps = gs.fill;
      e.page = page;
      e.color = gs.fillColor;
      recordSpot(e.color);
      e.overprint = gs.overprintFill;
      e.opm = gs.opm;
      e.transparency = gs.transparency;
      ev.paints.push_back(e);
    }
  }

  void recordSpot(const ColorInfo& ci) {
    if ((ci.cls == "separation" || ci.cls == "devicen") && !ci.spot.empty() &&
        ci.spot != "All" && ci.spot != "None" && ci.spot != "Registration" &&
        ci.spot != "Cyan" && ci.spot != "Magenta" && ci.spot != "Yellow" &&
        ci.spot != "Black" && ci.spot != "Gray" && ci.spot != "Grey") {
      ev.spotPlates.insert(ci.spot);
    }
  }

  void setColor(bool stroke, int n) {
    std::vector<double> c;
    size_t sz = nums.size();
    for (int i = n; i >= 1; --i) c.push_back(sz >= static_cast<size_t>(i) ? nums[sz - i] : 0);
    if (stroke) gs.stroke = c;
    else gs.fill = c;
  }

  void handleObject(QPDFObjectHandle obj, size_t, size_t) override {
    if (!obj.isOperator()) {
      if (obj.isName()) lastName = obj.getName();
      else if (obj.isNumber()) nums.push_back(obj.getNumericValue());
      return;
    }
    std::string op = obj.getOperatorValue();
    if (op == "q") stack.push_back(gs);
    else if (op == "Q") {
      if (!stack.empty()) {
        gs = stack.back();
        stack.pop_back();
      }
    } else if (op == "cm" && nums.size() >= 6) {
      size_t n = nums.size();
      Mat m{nums[n - 6], nums[n - 5], nums[n - 4], nums[n - 3], nums[n - 2], nums[n - 1]};
      gs.ctm = mul(m, gs.ctm);
    } else if (op == "w" && !nums.empty()) {
      gs.lineWidth = nums.back();
    } else if (op == "Tr" && !nums.empty()) {
      gs.renderMode = static_cast<int>(nums.back());
    } else if (op == "BT") {
      gs.tmScale = 1.0;
    } else if (op == "Tm" && nums.size() >= 6) {
      size_t n = nums.size();
      gs.tmScale = (std::hypot(nums[n - 6], nums[n - 5]) +
                    std::hypot(nums[n - 4], nums[n - 3])) / 2.0;
    } else if (op == "Tf" && !lastName.empty() && res.isDictionary()) {
      gs.fontSize = nums.empty() ? gs.fontSize : nums.back();
      QPDFObjectHandle fd = res.getKey("/Font");
      if (fd.isDictionary()) {
        QPDFObjectHandle fnt = fd.getKey(lastName);
        if (fnt.isDictionary()) {
          std::string bf = nameOf(fnt.getKey("/BaseFont"));
          if (bf.size() > 1) ev.baseFonts.insert(bf.substr(1));
        }
      }
    } else if (op == "gs") {
      applyExtGState();
    } else if (op == "g" || op == "G") {
      setColor(op == "G", 1);
      (op == "G" ? gs.strokeColor : gs.fillColor) = ColorInfo{"gray", "", 1};
    } else if (op == "rg" || op == "RG") {
      setColor(op == "RG", 3);
      (op == "RG" ? gs.strokeColor : gs.fillColor) = ColorInfo{"rgb", "", 3};
    } else if (op == "k" || op == "K") {
      setColor(op == "K", 4);
      (op == "K" ? gs.strokeColor : gs.fillColor) = ColorInfo{"cmyk", "", 4};
    } else if (op == "cs" || op == "CS") {
      setSpace(op == "CS");
    } else if (op == "sc" || op == "scn" || op == "SC" || op == "SCN") {
      if (!nums.empty()) setColor(op == "SC" || op == "SCN",
                                  static_cast<int>(nums.size() > 4 ? 4 : nums.size()));
    } else if (op == "S" || op == "s") {
      addPaint(true, false);
    } else if (op == "f" || op == "F" || op == "f*") {
      addPaint(false, true);
    } else if (op == "B" || op == "B*" || op == "b" || op == "b*") {
      addPaint(true, true);
    } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
      TextEvent e;
      e.sizePt = gs.fontSize * gs.tmScale * matScale(gs.ctm);
      e.renderMode = gs.renderMode;
      e.comps = gs.fill;
      e.page = page;
      e.color = gs.fillColor;
      e.overprint = gs.overprintFill;
      e.transparency = gs.transparency;
      ev.texts.push_back(e);
    } else if (op == "Do" && !lastName.empty()) {
      draws.push_back({lastName, gs});
    }
    nums.clear();
    lastName.clear();
  }

  void handleEOF() override {}
};

void scanEvents(QPDFObjectHandle contents, QPDFObjectHandle res, const Gs& initial,
                int page, int depth, Visited& seen, Events& ev) {
  if (depth > 12) return;
  EvScanner scan(res, ev, page, initial);
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
      ImageEvent e;
      e.page = page;
      e.bpc = dict.getKey("/BitsPerComponent").isInteger()
                  ? static_cast<int>(dict.getKey("/BitsPerComponent").getIntValue()) : 8;
      e.mask = dict.getKey("/ImageMask").isBool() &&
               dict.getKey("/ImageMask").getBoolValue();
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
      e.color = classifyColor(dict.getKey("/ColorSpace"), res);
      double wpt = std::hypot(d.second.ctm.a, d.second.ctm.b);
      double hpt = std::hypot(d.second.ctm.c, d.second.ctm.d);
      if (w > 0 && h > 0 && wpt > 0.01 && hpt > 0.01) {
        e.ppi = std::max(w * 72.0 / wpt, h * 72.0 / hpt);
      }
      ev.images.push_back(e);
    } else if (sub == "/Form") {
      if (!seen.enter(xo)) continue;
      Gs inner = d.second;
      QPDFObjectHandle mtx = dict.getKey("/Matrix");
      if (mtx.isArray() && mtx.getArrayNItems() == 6) {
        Mat m{numOf(mtx.getArrayItem(0), 1), numOf(mtx.getArrayItem(1), 0),
              numOf(mtx.getArrayItem(2), 0), numOf(mtx.getArrayItem(3), 1),
              numOf(mtx.getArrayItem(4), 0), numOf(mtx.getArrayItem(5), 0)};
        inner.ctm = mul(m, inner.ctm);
      }
      QPDFObjectHandle sres = dict.getKey("/Resources");
      scanEvents(xo, sres.isDictionary() ? sres : res, inner, page, depth + 1, seen, ev);
    }
  }
}

double numVal(const PfAtom& a) {
  return a.vals.empty() ? 0.0 : std::atof(a.vals[0].c_str());
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

bool isWhite(const std::vector<double>& c) {
  if (c.size() == 4) return maxComp(c) < 0.001;
  for (double v : c) {
    if (v < 0.999) return false;
  }
  return !c.empty();
}

bool blackOnly(const std::vector<double>& c) {
  if (c.size() == 4) return c[3] > 0.001 && c[0] < 0.001 && c[1] < 0.001 && c[2] < 0.001;
  if (c.size() == 1) return c[0] < 0.999;
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
  return -1;
}

bool is100Black(const std::vector<double>& c, const ColorInfo& ci) {
  if (ci.cls == "cmyk" && c.size() == 4) {
    return c[3] > 0.999 && c[0] < 0.001 && c[1] < 0.001 && c[2] < 0.001;
  }
  if (ci.cls == "gray" && c.size() == 1) return c[0] < 0.001;
  return false;
}

bool spotIsRegistration(const ColorInfo& ci) {
  return ci.spot == "All" || ci.spot == "Registration" || ci.spot == "all";
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
  if (op == "equal" || op == "is_contained_in" || op == "is_include" || op == "contains") {
    return any([&](const std::string& x) { return v.find(x) != std::string::npos; });
  }
  if (op == "unequal" || op == "is_not_contained_in" || op == "not_is_include" ||
      op == "not_contains") {
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

enum class Domain { kNone, kPaint, kText, kImage, kDoc, kPage, kAny };

Domain atomDomain(const std::string& token) {
  std::string ns = token.substr(0, token.find(':'));
  if (token == "CONTSTM::IsText") return Domain::kText;
  if (token == "CONTSTM::IsImage" || token == "CONTSTM::IsImageMask") return Domain::kImage;
  if (token == "CONTSTM::IsFilledArea" || token == "CONTSTM::IsStroked" ||
      token == "CONTSTM::IsLine" || token == "CONTSTM::FilledAndStroked" ||
      token == "CONTSTM::StrokedButNotFilled") {
    return Domain::kPaint;
  }
  if (ns == "CSGST_S" || ns == "CSGST_F" || ns == "CSGST_G") return Domain::kPaint;
  if (ns == "CSCOLOR") return Domain::kAny;
  if (ns == "CSTEXT") return Domain::kText;
  if (ns == "CSIMAGE") return Domain::kImage;
  if (ns == "PAGE") return Domain::kPage;
  if (ns == "DOC" || ns == "CSFONT" || ns == "OUTINTENTS" || ns == "ANNOT") {
    return Domain::kDoc;
  }
  return Domain::kNone;
}

bool evalColorAtom(const PfAtom& a, const std::vector<double>& comps, const ColorInfo& ci,
                   bool& supported) {
  const std::string& t = a.token;
  if (t == "CSCOLOR::NumberOfNonZeroComponents" ||
      t == "CSGST_S::NumberOfColoraWhichAreNonZero" ||
      t == "CSGST_F::NumberOfColoraWhichAreNonZero" ||
      t == "CSGST_F::NumberOfColoraWhichAreNonZeroFill") {
    return cmpNum(nonZeroComps(comps), a.op, numVal(a));
  }
  if (t == "CSCOLOR::NrOfComponents") return cmpNum(ci.declaredComps, a.op, numVal(a));
  if (t == "CSCOLOR::ObjectHasNonZeroValuesAndLowe") {
    double m = maxComp(comps) * 100.0;
    return nonZeroComps(comps) > 0 && cmpNum(m, a.op, numVal(a));
  }
  if (t == "CSCOLOR::ObjectIsWhite") return cmpBool(isWhite(comps), a.op);
  if (t == "CSCOLOR::ObjectUsesBlackOnly") return cmpBool(blackOnly(comps), a.op);
  if (t == "CSCOLOR::ObjectIs100_Black") return cmpBool(is100Black(comps, ci), a.op);
  if (t == "CSCOLOR::ObjectUsesBlackWithAPercenOf") {
    double bp = blackPercent(comps, ci);
    return bp >= 0 && cmpNum(bp, a.op, numVal(a));
  }
  if (t == "CSCOLOR::BlackObjUsesCMYwithAPercentageOf") {
    if (comps.size() != 4 || comps[3] < 0.9) return false;
    double cmy = std::max({comps[0], comps[1], comps[2]}) * 100.0;
    return cmpNum(cmy, a.op, numVal(a));
  }
  if (t == "CSCOLOR::IsDeviceGray") return cmpBool(ci.cls == "gray", a.op);
  if (t == "CSCOLOR::IsDeviceCMYK" || t == "CSCOLOR::ObjectUsesCMYKOnly_noSpotColo") {
    return cmpBool(ci.cls == "cmyk", a.op);
  }
  if (t == "CSCOLOR::IsDeviceRGB") return cmpBool(ci.cls == "rgb", a.op);
  if (t == "CSCOLOR::UsesICCbasedCMYK") {
    return cmpBool(ci.cls == "icc" && ci.declaredComps == 4, a.op);
  }
  if (t == "CSCOLOR::UsesICCbasedRGB") {
    return cmpBool(ci.cls == "icc" && ci.declaredComps == 3, a.op);
  }
  if (t == "CSCOLOR::NumberOfNonZeroCMYKComponents") {
    if (ci.cls != "cmyk" || comps.size() != 4) return false;
    return cmpNum(nonZeroComps(comps), a.op, numVal(a));
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
  supported = false;
  return false;
}

bool evalPaintAtom(const PfAtom& a, const PaintEvent& e, bool& supported) {
  const std::string& t = a.token;
  if (t == "CSGST_S::LineWidth") return e.stroke && cmpNum(e.width, a.op, numVal(a));
  if (t == "CSGST_S::IsOverPrintEnabledStroke" || t == "CSGST_F::IsOverPrintEnabledFill" ||
      t == "CSGST_G::IsOverPrintEnabled") {
    return cmpBool(e.overprint, a.op);
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
  if (t == "CONTSTM::IsFilledArea") return cmpBool(!e.stroke, a.op);
  if (t == "CONTSTM::IsStroked" || t == "CONTSTM::IsLine") return cmpBool(e.stroke, a.op);
  if (t == "CONTSTM::IsText" || t == "CONTSTM::IsImage" || t == "CONTSTM::IsImageMask") {
    return cmpBool(false, a.op);
  }
  bool s2 = true;
  bool r = evalColorAtom(a, e.comps, e.color, s2);
  if (s2) return r;
  supported = false;
  return false;
}

bool evalTextAtom(const PfAtom& a, const TextEvent& e, bool& supported) {
  const std::string& t = a.token;
  if (t == "CSTEXT::Textsize") return cmpNum(e.sizePt, a.op, numVal(a));
  if (t == "CSTEXT::TextIsNotRenderAndNotUsedAsCl") {
    return cmpBool(e.renderMode == 3, a.op);
  }
  if (t == "CONTSTM::IsText") return cmpBool(true, a.op);
  if (t == "CONTSTM::IsImage" || t == "CONTSTM::IsImageMask" ||
      t == "CONTSTM::IsFilledArea" || t == "CONTSTM::IsStroked" || t == "CONTSTM::IsLine") {
    return cmpBool(false, a.op);
  }
  bool s2 = true;
  bool r = evalColorAtom(a, e.comps, e.color, s2);
  if (s2) return r;
  supported = false;
  return false;
}

bool evalImageAtom(const PfAtom& a, const ImageEvent& e, bool& supported) {
  const std::string& t = a.token;
  if (t == "CSIMAGE::Resolution") return e.ppi > 0 && cmpNum(e.ppi, a.op, numVal(a));
  if (t == "CSIMAGE::BitsPerColourComponent") return cmpNum(e.bpc, a.op, numVal(a));
  if (t == "CSIMAGE::Width") return cmpNum(e.width, a.op, numVal(a));
  if (t == "CSIMAGE::Height") return cmpNum(e.height, a.op, numVal(a));
  if (t == "CSIMAGE::HasSMaskEntry") return cmpBool(e.hasSMask, a.op);
  if (t == "CONTSTM::IsImage") return cmpBool(!e.mask, a.op);
  if (t == "CONTSTM::IsImageMask") return cmpBool(e.mask, a.op);
  if (t == "CONTSTM::IsText" || t == "CONTSTM::IsFilledArea" || t == "CONTSTM::IsStroked" ||
      t == "CONTSTM::IsLine") {
    return cmpBool(false, a.op);
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
  bool r = evalColorAtom(a, std::vector<double>(), e.color, s2);
  if (s2) return r;
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
  supported = false;
  return false;
}
}

void passProfile(Ctx& ctx, std::size_t inputSize) {
  if (ctx.opt.preflightProfile.empty()) return;
  PfProfile prof;
  size_t firstCh = ctx.opt.preflightProfile.find_first_not_of(" \t\r\n");
  bool isJson = firstCh != std::string::npos && ctx.opt.preflightProfile[firstCh] == '{';
  bool parsed = isJson ? parseKuraJson(ctx.opt.preflightProfile, prof)
                       : parsePreflightXml(ctx.opt.preflightProfile, prof);
  if (!parsed) {
    ctx.res.analysis.push_back(
        {"PROFILE_UNREADABLE", "the preflight profile could not be parsed", false});
    return;
  }
  Events ev;
  ev.filesize = static_cast<double>(inputSize);
  {
    QPDFObjectHandle oi = ctx.pdf.getRoot().getKey("/OutputIntents");
    ev.hasOutputIntent = oi.isArray() && oi.getArrayNItems() > 0;
    ev.outputIntentCount = oi.isArray() ? oi.getArrayNItems() : 0;
    if (oi.isArray() && oi.getArrayNItems() > 0 && oi.getArrayItem(0).isDictionary()) {
      QPDFObjectHandle prof = oi.getArrayItem(0).getKey("/DestOutputProfile");
      if (prof.isStream()) {
        try {
          auto buf = prof.getStreamData(qpdf_dl_all);
          const unsigned char* d = buf->getBuffer();
          size_t n = buf->getSize();
          if (n >= 100) {
            ev.iccColorSpace.assign(reinterpret_cast<const char*>(d + 16), 4);
            while (!ev.iccColorSpace.empty() && ev.iccColorSpace.back() == ' ') {
              ev.iccColorSpace.pop_back();
            }
            bool anyId = false;
            char hex[33];
            for (int i = 0; i < 16; ++i) {
              std::snprintf(hex + i * 2, 3, "%02x", d[84 + i]);
              if (d[84 + i]) anyId = true;
            }
            if (anyId) ev.iccProfileId = hex;
          }
        } catch (...) {
        }
      }
    }
  }
  auto boxEq = [](QPDFObjectHandle a, QPDFObjectHandle b) {
    if (!a.isArray() || !b.isArray() || a.getArrayNItems() != 4 ||
        b.getArrayNItems() != 4) {
      return false;
    }
    for (int i = 0; i < 4; ++i) {
      if (std::fabs(numOf(a.getArrayItem(i), 0) - numOf(b.getArrayItem(i), 0)) > 0.5) {
        return false;
      }
    }
    return true;
  };
  try {
    QPDFPageDocumentHelper dh(ctx.pdf);
    std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
    ev.pageCount = static_cast<int>(pages.size());
    int pageNum = 0;
    for (auto& ph : pages) {
      ++pageNum;
      QPDFObjectHandle page = ph.getObjectHandle();
      QPDFObjectHandle mb = ph.getAttribute("/MediaBox", true);
      QPDFObjectHandle cb = ph.getAttribute("/CropBox", false);
      PageFacts pf;
      pf.page = pageNum;
      pf.hasMediaBox = mb.isArray();
      pf.hasCropBox = cb.isArray();
      pf.cropEqualsMedia = !cb.isArray() || boxEq(cb, mb);
      QPDFObjectHandle rot = ph.getAttribute("/Rotate", true);
      pf.rotated = rot.isInteger() && (rot.getIntValue() % 360) != 0;
      if (mb.isArray()) ++ev.pagesWithMediaBox;
      QPDFObjectHandle annots = page.getKey("/Annots");
      if (annots.isArray()) {
        for (int i = 0; i < annots.getArrayNItems(); ++i) {
          QPDFObjectHandle an = annots.getArrayItem(i);
          if (an.isDictionary()) {
            std::string st = nameOf(an.getKey("/Subtype"));
            if (st.size() > 1) ev.annotTypes.insert(st.substr(1));
          }
        }
      }
      size_t imgBefore = ev.images.size();
      size_t paintBefore = ev.paints.size();
      size_t textBefore = ev.texts.size();
      QPDFObjectHandle res = ph.getAttribute("/Resources", true);
      Visited seen;
      Gs initial;
      scanEvents(page.getKey("/Contents"), res, initial, pageNum, 0, seen, ev);
      pf.imageCount = static_cast<int>(ev.images.size() - imgBefore);
      pf.empty = ev.images.size() == imgBefore && ev.paints.size() == paintBefore &&
                 ev.texts.size() == textBefore;
      ev.pages.push_back(pf);
    }
  } catch (...) {
    return;
  }

  const char* sevName[] = {"", "Info", "Warning", "Error"};
  for (const PfRule& rule : prof.rules) {
    std::vector<const PfCondition*> conds;
    std::vector<const PfAtom*> atoms;
    for (const std::string& cid : rule.condIds) {
      auto it = prof.conds.find(cid);
      if (it == prof.conds.end()) continue;
      conds.push_back(&it->second);
      for (const PfAtom& a : it->second.atoms) atoms.push_back(&a);
    }
    if (atoms.empty()) continue;
    Domain dom = Domain::kNone;
    bool mixed = false;
    bool sawAny = false;
    for (const PfAtom* a : atoms) {
      Domain d = atomDomain(a->token);
      if (d == Domain::kNone) mixed = true;
      else if (d == Domain::kDoc) continue;
      else if (d == Domain::kAny) sawAny = true;
      else if (dom == Domain::kNone) dom = d;
      else if (dom != d) mixed = true;
    }
    if (dom == Domain::kNone && sawAny) dom = Domain::kPaint;
    bool supported = !mixed;
    long long hits = 0;
    std::set<int> pages;

    auto evalAtom = [&](const PfAtom& a, const void* e) -> bool {
      if (atomDomain(a.token) == Domain::kDoc) return evalDocAtom(a, ev, supported);
      switch (dom) {
        case Domain::kPaint:
          return evalPaintAtom(a, *static_cast<const PaintEvent*>(e), supported);
        case Domain::kText:
          return evalTextAtom(a, *static_cast<const TextEvent*>(e), supported);
        case Domain::kImage:
          return evalImageAtom(a, *static_cast<const ImageEvent*>(e), supported);
        case Domain::kPage:
          return evalPageAtom(a, *static_cast<const PageFacts*>(e), supported);
        default:
          return evalDocAtom(a, ev, supported);
      }
    };
    auto ruleMatches = [&](const void* e) -> bool {
      bool combined = rule.logic != 1;
      for (const PfCondition* c : conds) {
        bool condResult = true;
        for (const PfAtom& a : c->atoms) {
          if (!evalAtom(a, e)) { condResult = false; break; }
          if (!supported) return false;
        }
        if (rule.logic == 1) {
          combined = combined || condResult;
          if (combined) break;
        } else {
          combined = combined && condResult;
          if (!combined) break;
        }
      }
      return combined;
    };

    auto sweep = [&](auto& collection) {
      for (const auto& e : collection) {
        if (!supported) return;
        if (ruleMatches(&e)) {
          ++hits;
          pages.insert(e.page);
        }
      }
    };
    if (supported && dom == Domain::kPaint) sweep(ev.paints);
    else if (supported && dom == Domain::kText) sweep(ev.texts);
    else if (supported && dom == Domain::kImage) sweep(ev.images);
    else if (supported && dom == Domain::kPage) sweep(ev.pages);
    else if (supported && dom == Domain::kNone) {
      if (ruleMatches(nullptr)) hits = 1;
    }
    if (!supported) {
      ctx.res.analysis.push_back(
          {"PROFILE_RULE_UNSUPPORTED",
           rule.name + ": uses checks Kura cannot evaluate yet", false});
      continue;
    }
    if (hits) {
      std::string detail = std::string(sevName[rule.severity < 4 && rule.severity > 0
                                                   ? rule.severity : 1]) +
                           ": " + rule.name + " (" + std::to_string(hits) + " hit(s)";
      if (!pages.empty()) {
        detail += ", page";
        detail += pages.size() > 1 ? "s " : " ";
        int shown = 0;
        for (int p : pages) {
          if (shown == 8) {
            detail += ", …";
            break;
          }
          detail += (shown ? ", " : "") + std::to_string(p);
          ++shown;
        }
      }
      detail += ")";
      ctx.res.analysis.push_back({"PROFILE_HIT", detail, false});
    }
  }
}
}
