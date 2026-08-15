#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
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
    const Json* all = c.get("all");
    if (!all || all->type != Json::kArr) continue;
    PfCondition cond;
    for (const Json& a : all->arr) {
      if (a.type != Json::kObj) continue;
      PfAtom atom;
      const Json* prop = a.get("prop");
      if (!prop) continue;
      auto it = kuraPropMap().find(prop->str);
      atom.token = it != kuraPropMap().end() ? it->second : ("KURA::" + prop->str);
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
    std::string cid = "C" + rule.id;
    out.conds[cid] = cond;
    rule.condIds.push_back(cid);
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
      atom.token = tagText(text, "token", apos, aend);
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

struct PaintEvent {
  double width = 0;
  std::vector<double> comps;
  int page = 0;
  bool stroke = false;
};

struct TextEvent {
  double sizePt = 0;
  int renderMode = 0;
  std::vector<double> comps;
  int page = 0;
};

struct ImageEvent {
  double ppi = 0;
  int bpc = 8;
  bool mask = false;
  std::set<std::string> filters;
  int page = 0;
};

struct Events {
  std::vector<PaintEvent> paints;
  std::vector<TextEvent> texts;
  std::vector<ImageEvent> images;
  double filesize = 0;
  int pagesWithMediaBox = 0;
  int pageCount = 0;
};

struct Gs {
  Mat ctm;
  double lineWidth = 1.0;
  int renderMode = 0;
  double fontSize = 0;
  double tmScale = 1.0;
  std::vector<double> fill{0};
  std::vector<double> stroke{0};
};

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

  void addPaint(bool stroke, bool fill) {
    if (stroke) {
      PaintEvent e;
      e.width = gs.lineWidth * matScale(gs.ctm);
      e.comps = gs.stroke;
      e.page = page;
      e.stroke = true;
      ev.paints.push_back(e);
    }
    if (fill) {
      PaintEvent e;
      e.comps = gs.fill;
      e.page = page;
      ev.paints.push_back(e);
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
    } else if (op == "Tf" && !nums.empty()) {
      gs.fontSize = nums.back();
    } else if (op == "BT") {
      gs.tmScale = 1.0;
    } else if (op == "Tm" && nums.size() >= 6) {
      size_t n = nums.size();
      gs.tmScale = (std::hypot(nums[n - 6], nums[n - 5]) +
                    std::hypot(nums[n - 4], nums[n - 3])) / 2.0;
    } else if (op == "g" || op == "G") {
      setColor(op == "G", 1);
    } else if (op == "rg" || op == "RG") {
      setColor(op == "RG", 3);
    } else if (op == "k" || op == "K") {
      setColor(op == "K", 4);
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

enum class Domain { kNone, kPaint, kText, kImage, kDoc };

Domain atomDomain(const std::string& token) {
  std::string ns = token.substr(0, token.find(':'));
  if (ns == "CSGST_S" || ns == "CSGST_F" || ns == "CSCOLOR") return Domain::kPaint;
  if (ns == "CSTEXT") return Domain::kText;
  if (ns == "CSIMAGE") return Domain::kImage;
  if (ns == "DOC" || ns == "PAGE") return Domain::kDoc;
  return Domain::kNone;
}

bool evalPaintAtom(const PfAtom& a, const PaintEvent& e, bool& supported) {
  const std::string& t = a.token;
  if (t == "CSGST_S::LineWidth") return e.stroke && cmpNum(e.width, a.op, numVal(a));
  if (t == "CSGST_S::NumberOfColoraWhichAreNonZero" ||
      t == "CSCOLOR::NumberOfNonZeroComponents") {
    return cmpNum(nonZeroComps(e.comps), a.op, numVal(a));
  }
  if (t == "CSCOLOR::ObjectHasNonZeroValuesAndLowe") {
    double m = maxComp(e.comps) * 100.0;
    return nonZeroComps(e.comps) > 0 && cmpNum(m, a.op, numVal(a));
  }
  if (t == "CSCOLOR::ObjectIsWhite") return cmpBool(isWhite(e.comps), a.op);
  if (t == "CSCOLOR::ObjectUsesBlackOnly") return cmpBool(blackOnly(e.comps), a.op);
  if (t == "CSCOLOR::BlackObjUsesCMYwithAPercentageOf") {
    if (e.comps.size() != 4 || e.comps[3] < 0.9) return false;
    double cmy = std::max({e.comps[0], e.comps[1], e.comps[2]}) * 100.0;
    return cmpNum(cmy, a.op, numVal(a));
  }
  supported = false;
  return false;
}

bool evalTextAtom(const PfAtom& a, const TextEvent& e, bool& supported) {
  const std::string& t = a.token;
  if (t == "CSTEXT::Textsize") return cmpNum(e.sizePt, a.op, numVal(a));
  if (t == "CSTEXT::TextIsNotRenderAndNotUsedAsCl") {
    return cmpBool(e.renderMode == 3, a.op);
  }
  supported = false;
  return false;
}

bool evalImageAtom(const PfAtom& a, const ImageEvent& e, bool& supported) {
  const std::string& t = a.token;
  if (t == "CSIMAGE::Resolution") return e.ppi > 0 && cmpNum(e.ppi, a.op, numVal(a));
  if (t == "CSIMAGE::BitsPerColourComponent") return cmpNum(e.bpc, a.op, numVal(a));
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
  supported = false;
  return false;
}

bool evalDocAtom(const PfAtom& a, const Events& ev, bool& supported) {
  const std::string& t = a.token;
  if (t == "DOC::Filesize") return cmpNum(ev.filesize, a.op, numVal(a));
  if (t == "PAGE::HasMediaBox") {
    return cmpBool(ev.pagesWithMediaBox == ev.pageCount && ev.pageCount > 0, a.op);
  }
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
  try {
    QPDFPageDocumentHelper dh(ctx.pdf);
    std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
    ev.pageCount = static_cast<int>(pages.size());
    int pageNum = 0;
    for (auto& ph : pages) {
      ++pageNum;
      if (ph.getAttribute("/MediaBox", true).isArray()) ++ev.pagesWithMediaBox;
      QPDFObjectHandle res = ph.getAttribute("/Resources", true);
      Visited seen;
      Gs initial;
      scanEvents(ph.getObjectHandle().getKey("/Contents"), res, initial, pageNum, 0,
                 seen, ev);
    }
  } catch (...) {
    return;
  }

  const char* sevName[] = {"", "Info", "Warning", "Error"};
  for (const PfRule& rule : prof.rules) {
    std::vector<const PfAtom*> atoms;
    for (const std::string& cid : rule.condIds) {
      auto it = prof.conds.find(cid);
      if (it == prof.conds.end()) continue;
      for (const PfAtom& a : it->second.atoms) atoms.push_back(&a);
    }
    if (atoms.empty()) continue;
    Domain dom = Domain::kNone;
    bool mixed = false;
    for (const PfAtom* a : atoms) {
      Domain d = atomDomain(a->token);
      if (d == Domain::kNone) mixed = true;
      else if (d == Domain::kDoc) continue;
      else if (dom == Domain::kNone) dom = d;
      else if (dom != d) mixed = true;
    }
    bool supported = !mixed;
    long long hits = 0;
    std::set<int> pages;
    if (supported && dom == Domain::kPaint) {
      for (const PaintEvent& e : ev.paints) {
        bool all = true;
        for (const PfAtom* a : atoms) {
          if (atomDomain(a->token) == Domain::kDoc) {
            if (!evalDocAtom(*a, ev, supported)) all = false;
          } else if (!evalPaintAtom(*a, e, supported)) {
            all = false;
          }
          if (!supported || !all) break;
        }
        if (supported && all) {
          ++hits;
          pages.insert(e.page);
        }
      }
    } else if (supported && dom == Domain::kText) {
      for (const TextEvent& e : ev.texts) {
        bool all = true;
        for (const PfAtom* a : atoms) {
          if (atomDomain(a->token) == Domain::kDoc) {
            if (!evalDocAtom(*a, ev, supported)) all = false;
          } else if (!evalTextAtom(*a, e, supported)) {
            all = false;
          }
          if (!supported || !all) break;
        }
        if (supported && all) {
          ++hits;
          pages.insert(e.page);
        }
      }
    } else if (supported && dom == Domain::kImage) {
      for (const ImageEvent& e : ev.images) {
        bool all = true;
        for (const PfAtom* a : atoms) {
          if (atomDomain(a->token) == Domain::kDoc) {
            if (!evalDocAtom(*a, ev, supported)) all = false;
          } else if (!evalImageAtom(*a, e, supported)) {
            all = false;
          }
          if (!supported || !all) break;
        }
        if (supported && all) {
          ++hits;
          pages.insert(e.page);
        }
      }
    } else if (supported && dom == Domain::kNone) {
      bool all = true;
      for (const PfAtom* a : atoms) {
        if (!evalDocAtom(*a, ev, supported)) all = false;
        if (!supported || !all) break;
      }
      if (supported && all) hits = 1;
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
