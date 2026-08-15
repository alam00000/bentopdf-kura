#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <lcms2.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "images.hh"
#include "passes.hh"
#include "util.hh"

namespace pdfa {
extern const unsigned char kSrgbIcc[];
extern const unsigned int kSrgbIccLen;
extern const unsigned char kCmykIcc[];
extern const unsigned int kCmykIccLen;

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
  int scope = 0;
  std::vector<std::string> condIds;
};

struct PfBuiltin {
  std::string name;
  int severity = 1;
  std::map<std::string, double> params;
};

struct PfProfile {
  std::string name;
  std::vector<PfBuiltin> builtins;
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
      {"outputIntent.icc", "OUTINTENTS_ICC"}, {"outputIntentA.icc", "OUTINTENTSA_ICC"},
      {"outputIntentA", "OUTINTENTSA"}, {"outputIntentE", "OUTINTENTSE"},
      {"docinfo", "DOCINFO"}, {"signature", "SIGNATURES"},
      {"layers", "OPTIONALCONT"}, {"halftone", "CSHALFTONE"}, {"icc", "CSICC"},
      {"syntax", "DVASYNTAX"}, {"contentSyntax", "DVACSTRM"}, {"structure", "DVASTRUCT"},
      {"certificate", "CERTIFY"}, {"vt", "PDFVT"}, {"compare", "SIFTER"},
      {"tagging", "STRUCTPDF"}, {"form", "ACROFORM"}, {"postscript", "POSTSCRIPT"},
  };
  size_t dot = prop.find('.');
  if (dot == std::string::npos) return "KURA::" + prop;
  size_t dot2 = prop.find('.', dot + 1);
  auto hit = head.end();
  std::string tail;
  if (dot2 != std::string::npos) {
    hit = head.find(prop.substr(0, dot2));
    if (hit != head.end()) tail = prop.substr(dot2 + 1);
  }
  if (hit == head.end()) {
    hit = head.find(prop.substr(0, dot));
    if (hit != head.end()) tail = prop.substr(dot + 1);
  }
  if (hit == head.end()) return "KURA::" + prop;
  if (!tail.empty()) tail[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(tail[0])));
  return hit->second + "::" + tail;
}

bool parseKuraJson(const std::string& text, PfProfile& out) {
  JsonParser jp(text);
  Json root = jp.parse();
  if (!jp.ok || root.type != Json::kObj || !root.get("kura-profile")) return false;
  const Json* name = root.get("name");
  if (name) out.name = name->str;
  const Json* bts = root.get("builtins");
  if (bts && bts->type == Json::kArr) {
    for (const Json& b : bts->arr) {
      if (b.type != Json::kObj) continue;
      const Json* bn = b.get("name");
      if (!bn || bn->type != Json::kStr) continue;
      PfBuiltin pb;
      pb.name = bn->str;
      const Json* sv = b.get("severity");
      if (sv && sv->type == Json::kNum) pb.severity = static_cast<int>(sv->num);
      const Json* pr = b.get("params");
      if (pr && pr->type == Json::kObj) {
        for (const auto& kv : pr->obj) {
          if (kv.second.type == Json::kNum) pb.params[kv.first] = kv.second.num;
        }
      }
      out.builtins.push_back(pb);
    }
  }
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
    const Json* sc = c.get("scope");
    if (sc && sc->type == Json::kStr) {
      rule.scope = sc->str == "trim" ? 2 : (sc->str == "bleed" ? 3 : 0);
    }
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
  return true;
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
      size_t ipos = apos;
      while (true) {
        ipos = text.find("<item", ipos);
        if (ipos == std::string::npos || ipos > aend) break;
        size_t iopen = text.find('>', ipos);
        size_t iclose = text.find("</item>", iopen);
        if (iopen == std::string::npos || iclose == std::string::npos || iclose > aend) {
          break;
        }
        std::string iv = unescape(text.substr(iopen + 1, iclose - iopen - 1));
        size_t b = iv.find_first_not_of(" \t\r\n");
        if (b != std::string::npos) {
          atom.vals.push_back(iv.substr(b, iv.find_last_not_of(" \t\r\n") - b + 1));
        }
        ipos = iclose + 1;
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
    size_t ip = text.find("ignore_objects=\"", pos);
    if (ip != std::string::npos && ip < end) rule.scope = text[ip + 16] - '0';
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
  std::map<std::string, std::vector<std::pair<int, std::string>>> rulesetDefs;
  pos = text.find("<rulesets>");
  if (pos != std::string::npos) {
    size_t end = text.find("</rulesets>", pos);
    size_t sp = pos;
    while (true) {
      sp = text.find("<ruleset>", sp);
      if (sp == std::string::npos || sp > end) break;
      size_t send = text.find("</ruleset>", sp);
      std::string sid = tagText(text, "id1", sp, send);
      size_t p = sp;
      while (true) {
        p = text.find("<rule check_severity=\"", p);
        if (p == std::string::npos || p > send) break;
        int sev = text[p + 22] - '0';
        size_t open = text.find('>', p);
        size_t close = text.find("</rule>", open);
        rulesetDefs[sid].push_back({sev, text.substr(open + 1, close - open - 1)});
        p = close + 1;
      }
      sp = send + 1;
    }
  }
  size_t ppos = text.find("<profile>");
  if (ppos != std::string::npos) {
    size_t pend2 = text.find("</profile>", ppos);
    size_t cp = ppos;
    while (true) {
      cp = text.find("<check name=\"", cp);
      if (cp == std::string::npos || (pend2 != std::string::npos && cp > pend2)) break;
      size_t nend = text.find('"', cp + 13);
      PfBuiltin b;
      b.name = text.substr(cp + 13, nend - cp - 13);
      size_t sevp = text.find("check_severity=\"", cp);
      if (sevp != std::string::npos && sevp < cp + 120) b.severity = text[sevp + 16] - '0';
      size_t cend = text.find("</check>", cp);
      size_t selfclose = text.find("/>", cp);
      size_t bodyEnd = cend != std::string::npos ? cend : text.size();
      if (selfclose != std::string::npos && (cend == std::string::npos || selfclose < text.find('>', cp))) {
        bodyEnd = selfclose;
      }
      size_t dp = cp;
      while (true) {
        dp = text.find("<data name=\"", dp);
        if (dp == std::string::npos || dp > bodyEnd) break;
        size_t dn = text.find('"', dp + 12);
        std::string key = text.substr(dp + 12, dn - dp - 12);
        size_t vopen = text.find('>', dn);
        size_t vclose = text.find("</data>", vopen);
        if (vopen != std::string::npos && vclose != std::string::npos && vclose <= bodyEnd + 200) {
          b.params[key] = std::atof(text.substr(vopen + 1, vclose - vopen - 1).c_str());
        }
        dp = (vclose == std::string::npos ? dp + 12 : vclose + 1);
      }
      out.builtins.push_back(b);
      cp = cend == std::string::npos ? cp + 13 : cend + 1;
    }
  }
  std::set<std::string> activeRuleIds;
  std::map<std::string, int> activeSev;
  if (ppos != std::string::npos) {
    out.name = unescape(tagText(text, "name", ppos, text.size()));
    size_t pend = text.find("</profile>", ppos);
    size_t rp = ppos;
    while (true) {
      rp = text.find("<ruleset>", rp);
      if (rp == std::string::npos || (pend != std::string::npos && rp > pend)) break;
      size_t rclose = text.find("</ruleset>", rp);
      std::string sid = text.substr(rp + 9, rclose - rp - 9);
      size_t b = sid.find_first_not_of(" \t\r\n");
      if (b != std::string::npos) {
        sid = sid.substr(b, sid.find_last_not_of(" \t\r\n") - b + 1);
      }
      auto it = rulesetDefs.find(sid);
      if (it != rulesetDefs.end()) {
        for (const auto& [sev, rid] : it->second) {
          activeRuleIds.insert(rid);
          activeSev[rid] = sev;
        }
      }
      rp = rclose + 1;
    }
  }
  if (!activeRuleIds.empty()) {
    std::vector<PfRule> kept;
    for (auto& r : out.rules) {
      if (activeRuleIds.count(r.id)) {
        r.severity = activeSev[r.id];
        kept.push_back(r);
      }
    }
    out.rules = kept;
  } else {
    for (const auto& [sid, entries] : rulesetDefs) {
      for (const auto& [sev, rid] : entries) {
        for (auto& r : out.rules) {
          if (r.id == rid) r.severity = sev;
        }
      }
    }
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
  std::string altName;
  std::vector<std::string> colorants;
  int declaredComps = 1;
  bool indexed = false;
};

struct GsExtra {
  double flatness = 0;
  double alphaFill = 1.0;
  double alphaStroke = 1.0;
  std::string blendMode = "Normal";
  bool hasSMask = false;
  bool smaskExplicitNone = false;
  bool smaskIsLuminosity = false;
  std::string smaskGroupCS;
  bool hasTR2 = false;
  bool tr2IsDefault = false;
  bool hasBPC = false;
  bool hasHalftoneOrigin = false;
};

struct Box {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool valid = false;
};

struct PaintEvent {
  double width = 0;
  std::vector<double> comps;
  int page = 0;
  bool stroke = false;
  bool fillOp = false;
  bool shade = false;
  bool noPaint = false;
  int pathNodes = 0;
  ColorInfo color;
  bool overprint = false;
  int opm = 0;
  bool transparency = false;
  GsExtra x;
  Box bbox;
  Box clip;
  bool visible = true;
  bool covers = false;
  bool clippedPart = false;
  bool clippedFull = false;
};

struct TextEvent {
  double sizePt = 0;
  int renderMode = 0;
  std::vector<double> comps;
  int page = 0;
  ColorInfo color;
  bool overprint = false;
  int opm = 0;
  bool transparency = false;
  GsExtra x;
  Box bbox;
  Box clip;
  bool visible = true;
  bool covers = false;
  bool clippedPart = false;
  bool clippedFull = false;
  std::string fontName;
  std::string bytes;
  QPDFObjGen fontOg;
  bool glyphUndefined = false;
  bool glyphWhitespace = false;
  bool glyphHasContour = true;
  bool mappedToUnicode = true;
};

struct ImageEvent {
  double ppi = 0;
  int bpc = 8;
  int width = 0;
  int height = 0;
  bool mask = false;
  bool hasSMask = false;
  bool interpolate = false;
  bool overprint = false;
  int opm = 0;
  bool transparency = false;
  QPDFObjectHandle obj;
  std::set<std::string> filters;
  ColorInfo color;
  int page = 0;
  Box bbox;
  Box clip;
  bool visible = true;
  bool covers = false;
  bool clippedPart = false;
  bool clippedFull = false;
};

struct FontFacts {
  std::string baseFont;
  std::string subtype;
  bool embedded = false;
  bool symbolic = false;
  bool hasFlags = false;
  bool type3 = false;
  bool cid = false;
  bool cid0 = false;
  bool openType = false;
  bool trueType = false;
  bool hasCIDToGIDMap = true;
  bool hasToUnicode = false;
  bool hasEncodingDict = false;
  std::string encodingName;
  bool subsetName = false;
  std::string fontProgram;
  std::vector<double> widths;
  int firstChar = 0;
  double ascentEm = 0.80;
  double descentEm = -0.20;
  bool hasVMetrics = false;
  bool ftValid = true;
  bool ftLoaded = false;
  int fsType = 0;
  int cmapCount = 0;
  bool anyUndefinedGlyph = false;
  bool anyWidthMismatch = false;
  bool allUsedMapped = true;
  QPDFObjGen og;
};

struct AnnotFacts {
  std::string subtype;
  bool hasCA = false;
  double ca = 1.0;
  bool printFlag = false;
  bool knownType = true;
  Box rect;
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
  bool hasTransGroup = false;
  bool hasTransObj = false;
  bool contentCompressed = true;
  bool hasPageOI = false;
  bool hasProcStepsLayer = false;
  double wPt = 0, hPt = 0;
  double inkCoverage = -1;
  Box media, trim, bleed, art;
  std::set<std::string> plates;
};

struct Events {
  std::vector<PaintEvent> paints;
  std::vector<TextEvent> texts;
  std::vector<ImageEvent> images;
  std::vector<PageFacts> pages;
  std::vector<FontFacts> fonts;
  std::vector<AnnotFacts> annots;
  std::set<std::string> baseFonts;
  std::set<std::string> annotTypes;
  std::set<std::string> spotPlates;
  std::map<std::string, std::set<std::string>> spotAlternates;
  double filesize = 0;
  int pagesWithMediaBox = 0;
  int pageCount = 0;
  bool hasOutputIntent = false;
  int outputIntentCount = 0;
  std::string iccColorSpace;
  std::string iccProfileId;
  int iccVersionMajor = 0;
  std::string pdfVersion;
  std::string infoCreator, infoProducer, infoTrapped;
  bool infoHasPdfxFields = false;
  bool dataAfterEof = false;
  bool requirementsPdf20 = false;
  bool hasDPartRoot = false;
  bool hasOCProperties = false;
  bool ocHasConfigs = false;
  bool hasSigFields = false;
  bool docHasProcSteps = false;
  bool hasStructTree = false;
  bool hasParentTree = false;
  bool hasTransferCurve = false;
  bool hasHalftoneDict = false;
  bool hasPostScriptXObject = false;
  bool tpBlend = false;
  bool tpSMaskImg = false;
  bool tpSMaskGs = false;
  bool tpAlphaFill = false;
  bool tpAlphaStroke = false;
  bool hasTransparencyAnywhere = false;
  bool encrypted = false;
  bool damaged = false;
  int qpdfWarnings = 0;
  std::string xmpRaw;
  std::set<std::string> docIssues;
  std::set<int> pageParseFailed;
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
  GsExtra x;
  std::string fontName;
  QPDFObjGen fontOg;
  Box clip;
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
    ci.colorants.push_back(ci.spot);
    if (cs.getArrayNItems() >= 3) {
      QPDFObjectHandle alt = cs.getArrayItem(2);
      std::string an = alt.isName() ? alt.getName()
                       : (alt.isArray() && alt.getArrayNItems() > 0)
                           ? nameOf(alt.getArrayItem(0)) : std::string();
      if (an.size() > 1) ci.altName = an.substr(1);
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
  double tmX = 0, tmY = 0;
  int biW = 0, biH = 0, biBpc = 0;
  bool biMask = false;
  std::string biLastKey;
  QPDFObjectHandle curFont;

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
    if (nameIs(sm, "/None")) gs.x.smaskExplicitNone = true;
    if (!sm.isNull() && !nameIs(sm, "/None")) {
      tr = true;
      gs.x.hasSMask = true;
      if (sm.isDictionary()) {
        gs.x.smaskIsLuminosity = nameIs(sm.getKey("/S"), "/Luminosity");
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
    if (tr) gs.transparency = true;
  }

  void setSpace(bool stroke) {
    if (lastName.empty()) return;
    ColorInfo ci = classifyColor(QPDFObjectHandle::newName(lastName), res);
    if (stroke) gs.strokeColor = ci;
    else gs.fillColor = ci;
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
      e.x = gs.x;
      e.bbox = pathBox;
      e.clip = gs.clip;
      e.pathNodes = pathNodes;
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
      e.x = gs.x;
      e.bbox = pathBox;
      e.clip = gs.clip;
      e.pathNodes = pathNodes;
      ev.paints.push_back(e);
    }
    pathBox = Box();
    pathNodes = 0;
    pathIsClip = false;
  }

  void recordFont(QPDFObjectHandle fnt) {
    if (!fnt.isIndirect()) return;
    QPDFObjGen og = fnt.getObjGen();
    for (const FontFacts& f : ev.fonts) {
      if (f.og == og) return;
    }
    FontFacts ff;
    ff.og = og;
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
            auto buf = ffs.getStreamData(qpdf_dl_all);
            ff.fontProgram.assign(reinterpret_cast<const char*>(buf->getBuffer()),
                                  buf->getSize());
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
  bool inBI = false;
  std::vector<std::string> biNames;
  std::vector<double> biNums;
  std::string biCsName;

  void handleObject(QPDFObjectHandle obj, size_t, size_t) override {
    if (obj.isInlineImage()) {
      ImageEvent e;
      e.page = page;
      for (size_t i = 0; i + 1 <= biNames.size(); ++i) {
      }
      e.width = biW;
      e.height = biH;
      e.bpc = biBpc > 0 ? biBpc : 8;
      e.mask = biMask;
      if (!biCsName.empty()) {
        e.color = classifyColor(QPDFObjectHandle::newName(biCsName), res);
      }
      double wpt = std::hypot(gs.ctm.a, gs.ctm.b);
      double hpt = std::hypot(gs.ctm.c, gs.ctm.d);
      if (e.width > 0 && e.height > 0 && wpt > 0.01 && hpt > 0.01) {
        e.ppi = std::max(e.width * 72.0 / wpt, e.height * 72.0 / hpt);
      }
      {
        const Mat& m = gs.ctm;
        double xs[4] = {m.e, m.a + m.e, m.c + m.e, m.a + m.c + m.e};
        double ys[4] = {m.f, m.b + m.f, m.d + m.f, m.b + m.d + m.f};
        e.bbox = {*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4),
                  *std::max_element(xs, xs + 4), *std::max_element(ys, ys + 4), true};
      }
      e.clip = gs.clip;
      ev.images.push_back(e);
      inBI = false;
      biW = biH = biBpc = 0;
      biMask = false;
      biCsName.clear();
      biLastKey.clear();
      return;
    }
    if (!obj.isOperator()) {
      if (inBI) {
        if (obj.isName()) {
          std::string n = obj.getName();
          if (biLastKey.empty() && (n == "/W" || n == "/Width" || n == "/H" ||
                                    n == "/Height" || n == "/BPC" ||
                                    n == "/BitsPerComponent" || n == "/CS" ||
                                    n == "/ColorSpace" || n == "/IM" || n == "/ImageMask" ||
                                    n == "/F" || n == "/Filter" || n == "/D" || n == "/DP")) {
            biLastKey = n;
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
      else if (obj.isString()) lastString += obj.getStringValue();
      else if (obj.isArray()) {
        for (int i = 0; i < obj.getArrayNItems(); ++i) {
          if (obj.getArrayItem(i).isString()) lastString += obj.getArrayItem(i).getStringValue();
        }
      }
      return;
    }
    std::string op = obj.getOperatorValue();
    if (op == "BI") {
      inBI = true;
      biLastKey.clear();
      nums.clear();
      lastName.clear();
      return;
    }
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
      tmX = 0;
      tmY = 0;
    } else if (op == "Tm" && nums.size() >= 6) {
      size_t n = nums.size();
      gs.tmScale = (std::hypot(nums[n - 6], nums[n - 5]) +
                    std::hypot(nums[n - 4], nums[n - 3])) / 2.0;
      tmX = nums[n - 2];
      tmY = nums[n - 1];
    } else if ((op == "Td" || op == "TD") && nums.size() >= 2) {
      tmX += nums[nums.size() - 2];
      tmY += nums[nums.size() - 1];
    } else if (op == "m" || op == "l") {
      takePts(2);
    } else if (op == "c") {
      takePts(6);
    } else if (op == "v" || op == "y") {
      takePts(4);
    } else if (op == "re" && nums.size() >= 4) {
      size_t n = nums.size();
      addPt(nums[n - 4], nums[n - 3]);
      addPt(nums[n - 4] + nums[n - 2], nums[n - 3] + nums[n - 1]);
      ++pathNodes;
    } else if (op == "W" || op == "W*") {
      pathIsClip = true;
    } else if (op == "n") {
      if (pathIsClip && pathBox.valid) {
        if (!gs.clip.valid) {
          gs.clip = pathBox;
        } else {
          gs.clip.x0 = std::max(gs.clip.x0, pathBox.x0);
          gs.clip.y0 = std::max(gs.clip.y0, pathBox.y0);
          gs.clip.x1 = std::min(gs.clip.x1, pathBox.x1);
          gs.clip.y1 = std::min(gs.clip.y1, pathBox.y1);
        }
      }
      if (pathNodes > 0 && !pathIsClip) {
        PaintEvent e;
        e.page = page;
        e.noPaint = true;
        e.pathNodes = pathNodes;
        e.comps = gs.fill;
        e.color = gs.fillColor;
        e.x = gs.x;
        e.bbox = pathBox;
        ev.paints.push_back(e);
      }
      pathBox = Box();
      pathNodes = 0;
      pathIsClip = false;
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
      e.x = gs.x;
      ev.paints.push_back(e);
    } else if (op == "i" && !nums.empty()) {
      gs.x.flatness = nums.back();
    } else if (op == "Tf" && !lastName.empty() && res.isDictionary()) {
      gs.fontSize = nums.empty() ? gs.fontSize : nums.back();
      QPDFObjectHandle fd = res.getKey("/Font");
      if (fd.isDictionary()) {
        QPDFObjectHandle fnt = fd.getKey(lastName);
        if (fnt.isDictionary()) {
          std::string bf = nameOf(fnt.getKey("/BaseFont"));
          if (bf.size() > 1) {
            ev.baseFonts.insert(bf.substr(1));
            gs.fontName = bf.substr(1);
          }
          if (fnt.isIndirect()) gs.fontOg = fnt.getObjGen();
          curFont = fnt;
        }
      }
    } else if (op == "gs") {
      applyExtGState();
    } else if (op == "g" || op == "G") {
      setColor(op == "G", 1);
      (op == "G" ? gs.strokeColor : gs.fillColor) = ColorInfo{"gray", "", "", {}, 1, false};
    } else if (op == "rg" || op == "RG") {
      setColor(op == "RG", 3);
      (op == "RG" ? gs.strokeColor : gs.fillColor) = ColorInfo{"rgb", "", "", {}, 3, false};
    } else if (op == "k" || op == "K") {
      setColor(op == "K", 4);
      (op == "K" ? gs.strokeColor : gs.fillColor) = ColorInfo{"cmyk", "", "", {}, 4, false};
    } else if (op == "cs" || op == "CS") {
      setSpace(op == "CS");
    } else if (op == "sc" || op == "scn" || op == "SC" || op == "SCN") {
      if (!nums.empty()) setColor(op == "SC" || op == "SCN",
                                  static_cast<int>(nums.size() > 4 ? 4 : nums.size()));
      if ((op == "scn" || op == "SCN") && !lastName.empty()) {
        const ColorInfo& ci = op == "SCN" ? gs.strokeColor : gs.fillColor;
        if (ci.cls == "pattern") patternUses.push_back({lastName, gs});
      }
    } else if (op == "S" || op == "s") {
      addPaint(true, false);
    } else if (op == "f" || op == "F" || op == "f*") {
      addPaint(false, true);
    } else if (op == "B" || op == "B*" || op == "b" || op == "b*") {
      addPaint(true, true);
    } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
      if (curFont.isInitialized() && curFont.isDictionary()) recordFont(curFont);
      TextEvent e;
      e.sizePt = gs.fontSize * gs.tmScale * matScale(gs.ctm);
      e.renderMode = gs.renderMode;
      e.comps = gs.fill;
      e.page = page;
      e.color = gs.fillColor;
      e.overprint = gs.overprintFill;
      e.opm = gs.opm;
      e.transparency = gs.transparency;
      e.x = gs.x;
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
      draws.push_back({lastName, gs});
    }
    nums.clear();
    lastName.clear();
    lastString.clear();
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
  if (res.isDictionary()) {
    QPDFObjectHandle patd = res.getKey("/Pattern");
    for (const auto& pu : scan.patternUses) {
      if (!patd.isDictionary()) break;
      QPDFObjectHandle pat = patd.getKey(pu.first);
      if (!pat.isStream() || !seen.enter(pat)) continue;
      QPDFObjectHandle pd = pat.getDict();
      Gs inner = pu.second;
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
      double wpt = std::hypot(d.second.ctm.a, d.second.ctm.b);
      double hpt = std::hypot(d.second.ctm.c, d.second.ctm.d);
      if (w > 0 && h > 0 && wpt > 0.01 && hpt > 0.01) {
        e.ppi = std::max(w * 72.0 / wpt, h * 72.0 / hpt);
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
      e.obj = xo;
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

enum class Domain { kNone, kPaint, kText, kImage, kDoc, kPage, kAny, kAnnot, kFont };

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
  if (t == "CSCOLOR::NumberOfNonZeroComponents" ||
      t == "CSGST_S::NumberOfColoraWhichAreNonZero" ||
      t == "CSGST_F::NumberOfColoraWhichAreNonZero" ||
      t == "CSGST_F::NumberOfColoraWhichAreNonZeroFill") {
    return cmpNum(colorantCount(comps, ci), a.op, numVal(a));
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
    if (comps.size() != 4 || comps[3] < 0.9) return false;
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
    return cmpBool(x.hasSMask || x.alphaFill < 1.0 || x.alphaStroke < 1.0 ||
                       (x.blendMode != "Normal" && x.blendMode != "Compatible"),
                   a.op);
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
  if (t == "CSGST_S::IsOverPrintEnabledStroke" || t == "CSGST_F::IsOverPrintEnabledFill" ||
      t == "CSGST_G::IsOverPrintEnabled") {
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
  if (t == "CSGST_S::IsOverPrintEnabledStroke" || t == "CSGST_F::IsOverPrintEnabledFill" ||
      t == "CSGST_G::IsOverPrintEnabled") {
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
  if (t == "CSGST_S::IsOverPrintEnabledStroke" || t == "CSGST_F::IsOverPrintEnabledFill" ||
      t == "CSGST_G::IsOverPrintEnabled") {
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
  bool r = evalColorAtom(a, std::vector<double>(), e.color, s2);
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
    std::set<std::string> distinct;
    for (const FontFacts& o : ev.fonts) {
      if (stripSubset(o.baseFont) == base) distinct.insert(o.baseFont);
    }
    return cmpBool(distinct.size() <= 1, a.op);
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
    bool present = ev.xmpRaw.find("the reference tool") != std::string::npos &&
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
        for (const std::string& pl : p.plates) {
          if (!kProcess.count(pl)) used = true;
        }
      } else if (p.plates.count(want)) {
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

namespace {
struct PfFix {
  std::string op;
  std::vector<std::string> params;
};

std::vector<PfFix> collectFixes(const std::string& text) {
  std::vector<PfFix> fixes;
  size_t firstCh = text.find_first_not_of(" \t\r\n");
  bool isJson = firstCh != std::string::npos && text[firstCh] == '{';
  if (isJson) {
    JsonParser jp(text);
    Json root = jp.parse();
    if (!jp.ok || root.type != Json::kObj) return fixes;
    const Json* fx = root.get("fixes");
    if (!fx || fx->type != Json::kArr) return fixes;
    for (const Json& f : fx->arr) {
      if (f.type != Json::kObj) continue;
      const Json* op = f.get("op");
      if (!op || op->type != Json::kStr) continue;
      PfFix fix;
      fix.op = op->str;
      const Json* params = f.get("params");
      if (params && params->type == Json::kArr) {
        for (const Json& p : params->arr) {
          if (p.type == Json::kStr) fix.params.push_back(p.str);
          else if (p.type == Json::kNum) {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%g", p.num);
            fix.params.push_back(buf);
          }
        }
      }
      fixes.push_back(fix);
    }
    return fixes;
  }
  size_t pos = 0;
  while ((pos = text.find("<ffeat>", pos)) != std::string::npos) {
    size_t end = text.find("</ffeat>", pos);
    if (end == std::string::npos) break;
    PfFix fix;
    fix.op = text.substr(pos + 7, end - pos - 7);
    size_t pstart = text.find("<fparams>", end);
    size_t nextf = text.find("<ffeat>", end);
    if (pstart != std::string::npos && (nextf == std::string::npos || pstart < nextf)) {
      size_t pend = text.find("</fparams>", pstart);
      size_t p = pstart;
      while (pend != std::string::npos) {
        p = text.find("<fparam", p);
        if (p == std::string::npos || p > pend) break;
        size_t gt = text.find('>', p);
        size_t close = text.find("</fparam>", gt);
        if (gt == std::string::npos || close == std::string::npos || close > pend) break;
        fix.params.push_back(text.substr(gt + 1, close - gt - 1));
        p = close + 1;
      }
    }
    fixes.push_back(fix);
    pos = end + 1;
  }
  return fixes;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}

void passProfile(Ctx& ctx, const unsigned char* inputData, std::size_t inputSize) {
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
  if (inputData && inputSize > 5) {
    std::string tailBytes(reinterpret_cast<const char*>(inputData) +
                              (inputSize > 2048 ? inputSize - 2048 : 0),
                          std::min<std::size_t>(inputSize, 2048));
    size_t eof = tailBytes.rfind("%%EOF");
    if (eof != std::string::npos) {
      size_t after = eof + 5;
      while (after < tailBytes.size() &&
             (tailBytes[after] == '\r' || tailBytes[after] == '\n' ||
              tailBytes[after] == ' ' || tailBytes[after] == '\t' ||
              tailBytes[after] == '\0')) {
        ++after;
      }
      ev.dataAfterEof = after < tailBytes.size();
    }
  }
  try {
    ev.qpdfWarnings = static_cast<int>(ctx.pdf.getWarnings().size());
  } catch (...) {
  }
  {
    QPDFObjectHandle root = ctx.pdf.getRoot();
    QPDFObjectHandle oi = root.getKey("/OutputIntents");
    ev.hasOutputIntent = oi.isArray() && oi.getArrayNItems() > 0;
    ev.outputIntentCount = oi.isArray() ? oi.getArrayNItems() : 0;
    if (oi.isArray()) {
      for (int i = 0; i < oi.getArrayNItems(); ++i) {
        QPDFObjectHandle o = oi.getArrayItem(i);
        if (o.isDictionary() && !o.getKey("/MixingHints").isNull()) {
          ev.docIssues.insert("oiMixingHints");
        }
      }
    }
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
            ev.iccVersionMajor = d[8];
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
    try {
      ev.pdfVersion = ctx.pdf.getPDFVersion();
      QPDFObjectHandle ver = root.getKey("/Version");
      if (ver.isName() && ver.getName().size() > 1) ev.pdfVersion = ver.getName().substr(1);
    } catch (...) {
    }
    QPDFObjectHandle info = ctx.pdf.getTrailer().getKey("/Info");
    if (info.isDictionary()) {
      if (info.getKey("/Creator").isString()) {
        ev.infoCreator = info.getKey("/Creator").getUTF8Value();
      }
      if (info.getKey("/Producer").isString()) {
        ev.infoProducer = info.getKey("/Producer").getUTF8Value();
      }
      QPDFObjectHandle tr = info.getKey("/Trapped");
      if (tr.isName() && tr.getName().size() > 1) ev.infoTrapped = tr.getName().substr(1);
      for (const std::string& k : info.getKeys()) {
        if (k.rfind("/GTS_", 0) == 0) ev.infoHasPdfxFields = true;
      }
    }
    QPDFObjectHandle req = root.getKey("/Requirements");
    if (req.isArray()) {
      for (int i = 0; i < req.getArrayNItems(); ++i) {
        QPDFObjectHandle r = req.getArrayItem(i);
        if (r.isDictionary() && nameOf(r.getKey("/S")).find("PDF20") != std::string::npos) {
          ev.requirementsPdf20 = true;
        }
      }
    }
    ev.hasDPartRoot = root.getKey("/DPartRoot").isDictionary();
    QPDFObjectHandle ocp = root.getKey("/OCProperties");
    ev.hasOCProperties = ocp.isDictionary();
    if (ocp.isDictionary()) {
      ev.ocHasConfigs = ocp.getKey("/Configs").isArray();
      std::string ocRaw = ocp.unparseResolved();
      if (ocRaw.find("GTS_") != std::string::npos ||
          ocRaw.find("/ProcSteps") != std::string::npos) {
        ev.docHasProcSteps = true;
      }
    }
    QPDFObjectHandle acro = root.getKey("/AcroForm");
    if (acro.isDictionary()) {
      QPDFObjectHandle sf = acro.getKey("/SigFlags");
      if (sf.isInteger() && sf.getIntValue() != 0) ev.hasSigFields = true;
      QPDFObjectHandle flds = acro.getKey("/Fields");
      if (flds.isArray()) {
        for (int i = 0; i < flds.getArrayNItems(); ++i) {
          QPDFObjectHandle f = flds.getArrayItem(i);
          if (f.isDictionary() && nameIs(f.getKey("/FT"), "/Sig")) ev.hasSigFields = true;
        }
      }
    }
    QPDFObjectHandle str = root.getKey("/StructTreeRoot");
    ev.hasStructTree = str.isDictionary();
    if (str.isDictionary()) ev.hasParentTree = str.getKey("/ParentTree").isDictionary();
    ev.encrypted = ctx.pdf.isEncrypted();
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (obj.isDictionary() && nameIs(obj.getKey("/Type"), "/ExtGState")) {
        QPDFObjectHandle tr = obj.getKey("/TR");
        QPDFObjectHandle tr2 = obj.getKey("/TR2");
        if ((!tr.isNull() && !nameIs(tr, "/Identity")) ||
            (!tr2.isNull() && !nameIs(tr2, "/Identity") && !nameIs(tr2, "/Default"))) {
          ev.hasTransferCurve = true;
        }
        QPDFObjectHandle ht = obj.getKey("/HT");
        if (ht.isDictionary() || ht.isStream()) {
          QPDFObjectHandle htType = (ht.isStream() ? ht.getDict() : ht).getKey("/HalftoneType");
          if (htType.isInteger() && htType.getIntValue() != 1) ev.hasHalftoneDict = true;
        }
      }
      if (obj.isStream()) {
        QPDFObjectHandle sd = obj.getDict();
        if (nameIs(sd.getKey("/Subtype"), "/PS") || !sd.getKey("/PS").isNull()) {
          ev.hasPostScriptXObject = true;
        }
        if (sd.getKey("/SMask").isStream() && sd.getKey("/Width").isInteger()) {
          ev.tpSMaskImg = true;
        }
      }
      if (obj.isDictionary()) {
        bool gstateLike = nameIs(obj.getKey("/Type"), "/ExtGState") ||
                          !obj.getKey("/BM").isNull() || obj.getKey("/ca").isNumber() ||
                          obj.getKey("/CA").isNumber();
        if (gstateLike) {
          QPDFObjectHandle bm = obj.getKey("/BM");
          std::string bmn =
              nameOf(bm.isArray() && bm.getArrayNItems() ? bm.getArrayItem(0) : bm);
          if (!bmn.empty() && bmn != "/Normal" && bmn != "/Compatible") ev.tpBlend = true;
          QPDFObjectHandle sm = obj.getKey("/SMask");
          if (sm.isDictionary()) ev.tpSMaskGs = true;
          if (obj.getKey("/ca").isNumber() && obj.getKey("/ca").getNumericValue() < 1.0) {
            ev.tpAlphaFill = true;
          }
          if (obj.getKey("/CA").isNumber() && obj.getKey("/CA").getNumericValue() < 1.0) {
            ev.tpAlphaStroke = true;
          }
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
      auto readBox = [](QPDFObjectHandle b) {
        Box out;
        if (b.isArray() && b.getArrayNItems() == 4) {
          double v[4];
          for (int i = 0; i < 4; ++i) v[i] = numOf(b.getArrayItem(i), 0);
          out = {std::min(v[0], v[2]), std::min(v[1], v[3]),
                 std::max(v[0], v[2]), std::max(v[1], v[3]), true};
        }
        return out;
      };
      pf.media = readBox(mb);
      pf.trim = readBox(ph.getAttribute("/TrimBox", false));
      pf.bleed = readBox(ph.getAttribute("/BleedBox", false));
      pf.art = readBox(ph.getAttribute("/ArtBox", false));
      if (pf.media.valid) {
        pf.wPt = pf.media.x1 - pf.media.x0;
        pf.hPt = pf.media.y1 - pf.media.y0;
      }
      QPDFObjectHandle grp = page.getKey("/Group");
      pf.hasTransGroup = grp.isDictionary() && nameIs(grp.getKey("/S"), "/Transparency");
      pf.hasPageOI = page.getKey("/OutputIntents").isArray();
      {
        QPDFObjectHandle cont = page.getKey("/Contents");
        auto compressed = [](QPDFObjectHandle st) {
          return st.isStream() && !st.getDict().getKey("/Filter").isNull();
        };
        if (cont.isStream()) pf.contentCompressed = compressed(cont);
        else if (cont.isArray() && cont.getArrayNItems() > 0) {
          pf.contentCompressed = true;
          for (int i = 0; i < cont.getArrayNItems(); ++i) {
            if (!compressed(cont.getArrayItem(i))) pf.contentCompressed = false;
          }
        }
      }
      QPDFObjectHandle annots = page.getKey("/Annots");
      if (annots.isArray()) {
        static const std::set<std::string> kStdAnnots = {
            "Text", "Link", "FreeText", "Line", "Square", "Circle", "Polygon",
            "PolyLine", "Highlight", "Underline", "Squiggly", "StrikeOut", "Stamp",
            "Caret", "Ink", "Popup", "FileAttachment", "Sound", "Movie", "Widget",
            "Screen", "PrinterMark", "TrapNet", "Watermark", "3D", "Redact",
            "Projection", "RichMedia"};
        for (int i = 0; i < annots.getArrayNItems(); ++i) {
          QPDFObjectHandle an = annots.getArrayItem(i);
          if (an.isDictionary()) {
            std::string st = nameOf(an.getKey("/Subtype"));
            AnnotFacts af;
            af.page = pageNum;
            if (st.size() > 1) {
              af.subtype = st.substr(1);
              ev.annotTypes.insert(af.subtype);
              af.knownType = kStdAnnots.count(af.subtype) > 0;
            } else {
              af.knownType = false;
            }
            QPDFObjectHandle ca = an.getKey("/CA");
            if (ca.isNumber()) {
              af.hasCA = true;
              af.ca = ca.getNumericValue();
            }
            QPDFObjectHandle fl = an.getKey("/F");
            if (fl.isInteger()) af.printFlag = (fl.getIntValue() & 4) != 0;
            af.rect = readBox(an.getKey("/Rect"));
            ev.annots.push_back(af);
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
      auto addPlates = [&pf](const std::vector<double>& comps, const ColorInfo& ci) {
        auto addCMYK = [&]() {
          pf.plates.insert("Cyan");
          pf.plates.insert("Magenta");
          pf.plates.insert("Yellow");
          pf.plates.insert("Black");
        };
        if (ci.cls == "cmyk" && comps.size() == 4) {
          const char* names[4] = {"Cyan", "Magenta", "Yellow", "Black"};
          for (int ch = 0; ch < 4; ++ch) {
            if (comps[ch] > 0.001) pf.plates.insert(names[ch]);
          }
        } else if (ci.cls == "icc" || ci.cls == "cal" || ci.cls == "lab") {
          bool white = !comps.empty();
          for (double v : comps) {
            if (v < 0.999) white = false;
          }
          if (!white) addCMYK();
        } else if (ci.cls == "gray") {
          if (!comps.empty() && comps[0] < 0.999) pf.plates.insert("Black");
        } else if (ci.cls == "separation" || ci.cls == "devicen") {
          for (size_t i = 0; i < ci.colorants.size(); ++i) {
            double tint = i < comps.size() ? comps[i] : (comps.empty() ? 1.0 : comps[0]);
            if (tint <= 0.001) continue;
            const std::string& c = ci.colorants[i];
            if (c == "All" || c == "Registration") {
              pf.plates.insert("Cyan");
              pf.plates.insert("Magenta");
              pf.plates.insert("Yellow");
              pf.plates.insert("Black");
            } else {
              pf.plates.insert(c);
            }
          }
        } else if (!comps.empty()) {
          bool white = true, grayish = true;
          for (double v : comps) {
            if (v < 0.999) white = false;
          }
          for (size_t i = 1; i < comps.size(); ++i) {
            if (std::abs(comps[i] - comps[0]) > 0.02) grayish = false;
          }
          if (!white) {
            if (grayish) {
              pf.plates.insert("Black");
            } else {
              pf.plates.insert("Cyan");
              pf.plates.insert("Magenta");
              pf.plates.insert("Yellow");
              pf.plates.insert("Black");
            }
          }
        }
      };
      auto bigEnough = [](const Box& b) {
        if (!b.valid) return false;
        return (b.x1 - b.x0) >= 28.35 && (b.y1 - b.y0) >= 28.35;
      };
      for (size_t i = paintBefore; i < ev.paints.size(); ++i) {
        const PaintEvent& pe = ev.paints[i];
        if (pe.transparency) pf.hasTransObj = true;
        if (pe.noPaint) continue;
        if (!bigEnough(pe.bbox)) continue;
        addPlates(pe.comps, pe.color);
      }
      for (size_t i = textBefore; i < ev.texts.size(); ++i) {
        const TextEvent& te = ev.texts[i];
        if (te.transparency) pf.hasTransObj = true;
        if (te.renderMode == 3) continue;
        if (!bigEnough(te.bbox)) continue;
        addPlates(te.comps, te.color);
      }
      for (size_t i = imgBefore; i < ev.images.size(); ++i) {
        const ImageEvent& ie = ev.images[i];
        const ColorInfo& ci = ie.color;
        if (ie.mask) {
          addPlates(ev.paints.empty() ? std::vector<double>{0.0}
                                      : std::vector<double>{0.0},
                    ColorInfo{"gray", "", "", {}, 1, false});
        } else if (ci.cls == "gray" || (ci.cls == "icc" && ci.declaredComps == 1)) {
          pf.plates.insert("Black");
        } else if (ci.cls == "separation" || ci.cls == "devicen") {
          for (const std::string& c : ci.colorants) {
            if (c == "All" || c == "Registration") continue;
            pf.plates.insert(c);
          }
        } else {
          pf.plates.insert("Cyan");
          pf.plates.insert("Magenta");
          pf.plates.insert("Yellow");
          pf.plates.insert("Black");
        }
      }
      ev.pages.push_back(pf);
    }
  } catch (...) {
    return;
  }

  {
    bool needInk = false;
    for (const auto& [cid, cond] : prof.conds) {
      for (const PfAtom& a : cond.atoms) {
        if (a.token.rfind("PAGE::EffectiveInkCoverage", 0) == 0) needInk = true;
      }
    }
    if (needInk) {
      cmsHPROFILE rgbP = cmsOpenProfileFromMem(kSrgbIcc, kSrgbIccLen);
      cmsHPROFILE cmykP = cmsOpenProfileFromMem(kCmykIcc, kCmykIccLen);
      cmsHTRANSFORM tDbl = nullptr, t8 = nullptr;
      if (rgbP && cmykP) {
        tDbl = cmsCreateTransform(rgbP, TYPE_RGB_DBL, cmykP, TYPE_CMYK_DBL,
                                  INTENT_RELATIVE_COLORIMETRIC,
                                  cmsFLAGS_BLACKPOINTCOMPENSATION);
        t8 = cmsCreateTransform(rgbP, TYPE_RGB_8, cmykP, TYPE_CMYK_8,
                                INTENT_RELATIVE_COLORIMETRIC,
                                cmsFLAGS_BLACKPOINTCOMPENSATION);
      }
      auto rgbInk = [&](double r, double g, double b) {
        double sum;
        if (!tDbl) {
          sum = (3.0 - r - g - b) * 100.0;
        } else {
          double in[3] = {r, g, b};
          double out[4] = {0, 0, 0, 0};
          cmsDoTransform(tDbl, in, out, 1);
          sum = out[0] + out[1] + out[2] + out[3];
        }
        return std::min(sum, 300.0);
      };
      auto inkOf = [&](const std::vector<double>& c, const ColorInfo& ci) -> double {
        if (c.empty()) return 0;
        if (ci.cls == "cmyk" || (ci.cls == "icc" && ci.declaredComps == 4 && c.size() == 4)) {
          double sum = 0;
          for (double v : c) sum += v;
          return sum * 100.0;
        }
        if (ci.cls == "gray" || (ci.cls == "icc" && ci.declaredComps == 1) ||
            (ci.cls == "cal" && c.size() == 1)) {
          return (1.0 - c[0]) * 100.0;
        }
        if (ci.cls == "separation" || ci.cls == "devicen") {
          double sum = 0;
          for (double v : c) sum += v;
          return sum * 100.0;
        }
        if ((ci.cls == "rgb" || ci.cls == "cal" || ci.cls == "icc") && c.size() >= 3) {
          return rgbInk(c[0], c[1], c[2]);
        }
        return 0;
      };
      struct InkObj {
        double ink;
        Box bbox;
        bool cmyk;
        std::vector<double> comps;
        bool overprint;
        int opm;
        bool spot;
      };
      std::map<int, std::vector<InkObj>> perPage;
      for (const PaintEvent& e : ev.paints) {
        if (e.noPaint || e.x.alphaFill <= 0.001) continue;
        perPage[e.page].push_back({inkOf(e.comps, e.color), e.bbox,
                                   e.color.cls == "cmyk", e.comps, e.overprint, e.opm,
                                   e.color.cls == "separation" || e.color.cls == "devicen"});
      }
      for (const TextEvent& e : ev.texts) {
        if (e.renderMode == 3) continue;
        perPage[e.page].push_back({inkOf(e.comps, e.color), e.bbox,
                                   e.color.cls == "cmyk", e.comps, e.overprint, false,
                                   e.color.cls == "separation" || e.color.cls == "devicen"});
      }
      for (const ImageEvent& e : ev.images) {
        double best = 0;
        RawImage raw = e.obj.isStream() ? decodeImage(e.obj) : RawImage();
        if (raw.ok && raw.width > 0 && raw.height > 0 && raw.comps > 0) {
          size_t px = static_cast<size_t>(raw.width) * raw.height;
          size_t stride = px > 20000 ? px / 20000 : 1;
          const unsigned char* d =
              reinterpret_cast<const unsigned char*>(raw.samples.data());
          size_t avail = raw.samples.size() / raw.comps;
          for (size_t i = 0; i < avail && i < px; i += stride) {
            const unsigned char* p = d + i * raw.comps;
            double ink = 0;
            if (raw.comps == 4) ink = (p[0] + p[1] + p[2] + p[3]) / 255.0 * 100.0;
            else if (raw.comps == 3) ink = rgbInk(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0);
            else if (raw.comps == 1) {
              ink = e.color.cls == "separation" ? p[0] / 255.0 * 100.0
                                                : (255 - p[0]) / 255.0 * 100.0;
            }
            best = std::max(best, ink);
          }
        } else if (e.color.cls == "cmyk" || (e.color.cls == "icc" && e.color.declaredComps == 4)) {
          best = -1;
        }
        if (best >= 0) {
          perPage[e.page].push_back({best, e.bbox, false, {}, e.overprint, 0, false});
        }
      }
      for (PageFacts& pf : ev.pages) {
        double maxTac = 0;
        auto& objs = perPage[pf.page];
        for (const InkObj& o : objs) maxTac = std::max(maxTac, o.ink);
        if (objs.size() <= 2000) {
          for (const InkObj& o : objs) {
            if (!o.overprint) continue;
            for (const InkObj& u : objs) {
              if (&o == &u || !boxesIntersect(o.bbox, u.bbox)) continue;
              double combined;
              if (o.spot) {
                combined = o.ink + u.ink;
              } else if (o.cmyk && o.opm == 1 && u.cmyk && o.comps.size() == 4 &&
                         u.comps.size() == 4) {
                combined = 0;
                for (int ch = 0; ch < 4; ++ch) {
                  combined += (o.comps[ch] > 0.001 ? o.comps[ch] : u.comps[ch]) * 100.0;
                }
              } else {
                continue;
              }
              maxTac = std::max(maxTac, std::min(combined, 400.0));
            }
          }
        }
        pf.inkCoverage = maxTac;
      }
      std::set<int> shadePages;
      for (const PaintEvent& e : ev.paints) {
        if (e.shade || e.color.cls == "pattern") shadePages.insert(e.page);
      }
      if (ctx.opt.rasterizePage && t8) {
        for (PageFacts& pf : ev.pages) {
          if (!shadePages.count(pf.page)) continue;
          int w = 0, h = 0;
          std::string rgb;
          if (!ctx.opt.rasterizePage(pf.page - 1, 72.0, w, h, rgb)) continue;
          if (w <= 0 || h <= 0 || rgb.size() < static_cast<size_t>(w) * h * 3) continue;
          std::vector<unsigned char> cmykRow(static_cast<size_t>(w) * 4);
          double maxTac = pf.inkCoverage < 0 ? 0 : pf.inkCoverage;
          const unsigned char* px = reinterpret_cast<const unsigned char*>(rgb.data());
          for (int row = 0; row < h; ++row) {
            cmsDoTransform(t8, px + static_cast<size_t>(row) * w * 3, cmykRow.data(),
                           static_cast<cmsUInt32Number>(w));
            for (int i = 0; i < w; ++i) {
              double ink = (cmykRow[i * 4] + cmykRow[i * 4 + 1] + cmykRow[i * 4 + 2] +
                            cmykRow[i * 4 + 3]) / 255.0 * 100.0;
              if (ink > maxTac) maxTac = ink;
            }
          }
          pf.inkCoverage = maxTac;
        }
      }
      if (tDbl) cmsDeleteTransform(tDbl);
      if (t8) cmsDeleteTransform(t8);
      if (rgbP) cmsCloseProfile(rgbP);
      if (cmykP) cmsCloseProfile(cmykP);
    }
    auto finish = [&](auto& vec) {
      for (auto& e : vec) {
        const PageFacts* p = pageFor(ev, e.page);
        bool vis = true;
        if (e.bbox.valid && e.clip.valid) {
          e.clippedFull = !boxesIntersect(e.bbox, e.clip);
          e.clippedPart = !e.clippedFull && !boxContains(e.clip, e.bbox, 0.01);
        }
        if (e.clippedFull) vis = false;
        if (p && p->media.valid && e.bbox.valid && !boxesIntersect(e.bbox, p->media)) {
          vis = false;
        }
        e.visible = vis;
      }
    };
    finish(ev.paints);
    finish(ev.texts);
    finish(ev.images);
    for (TextEvent& e : ev.texts) {
      if (e.renderMode == 3) e.visible = false;
    }
    for (PaintEvent& e : ev.paints) {
      if (e.noPaint || e.x.alphaFill <= 0.001) e.visible = false;
    }
    size_t total = ev.paints.size() + ev.texts.size() + ev.images.size();
    if (total > 0 && total <= 6000) {
      struct Occ {
        Box bbox;
        int page;
        long seq;
        bool opaque;
        bool areaFill;
        bool* coversOut;
        bool* visibleOut;
      };
      std::vector<Occ> objs;
      long seq = 0;
      auto opaqueOf = [](const GsExtra& x, bool fill) {
        double a = fill ? x.alphaFill : x.alphaStroke;
        return a >= 0.999 && (x.blendMode == "Normal" || x.blendMode == "Compatible") &&
               !x.hasSMask;
      };
      for (PaintEvent& e : ev.paints) {
        bool areaFill = e.fillOp && !e.shade && !e.noPaint;
        objs.push_back({e.bbox, e.page, seq++, opaqueOf(e.x, true) && !e.overprint,
                        areaFill, &e.covers, &e.visible});
      }
      for (TextEvent& e : ev.texts) {
        objs.push_back({e.bbox, e.page, seq++, opaqueOf(e.x, true), false, &e.covers,
                        &e.visible});
      }
      for (ImageEvent& e : ev.images) {
        bool opaque = !e.hasSMask && !e.mask;
        objs.push_back({e.bbox, e.page, seq++, opaque, true, &e.covers, &e.visible});
      }
      for (Occ& o : objs) {
        bool covers = false, occluded = false;
        for (const Occ& u : objs) {
          if (&o == &u || u.page != o.page || !boxesIntersect(o.bbox, u.bbox)) continue;
          if (o.seq > u.seq && o.opaque && o.areaFill) covers = true;
          if (u.seq > o.seq && u.opaque && u.areaFill &&
              boxContains(u.bbox, o.bbox, 0.5)) {
            occluded = true;
          }
        }
        *o.coversOut = covers;
        if (occluded) *o.visibleOut = false;
      }
    }
  }

  {
    bool needGlyphs = false, needStruct = false, needXmp = false;
    for (const auto& [cid, cond] : prof.conds) {
      for (const PfAtom& a : cond.atoms) {
        const std::string& t = a.token;
        if (t.rfind("CSFONT::", 0) == 0 || t.find("Glyph") != std::string::npos ||
            t.find("glyph") != std::string::npos || t.find("Unicode") != std::string::npos ||
            t.find("Embedding") != std::string::npos || t.find("Subset") != std::string::npos) {
          needGlyphs = true;
        }
        if (t.rfind("DVASTRUCT::", 0) == 0 || t.rfind("DVACSTRM::", 0) == 0 ||
            t.rfind("DVASYNTAX::", 0) == 0 || t.rfind("STRUCTPDF::", 0) == 0 ||
            t == "CONTSTM::UnknowOperatInPDF1_3ThrougPDF" ||
            t == "PAGE::PageDescriptionNotValid") {
          needStruct = true;
        }
        if (t.rfind("CERTIFY::", 0) == 0 || t.find("CxF") != std::string::npos) {
          needXmp = true;
        }
      }
    }
    if (needGlyphs) {
      FT_Library lib = nullptr;
      if (FT_Init_FreeType(&lib) == 0) {
        std::map<std::string, FT_Face> faces;
        for (FontFacts& f : ev.fonts) {
          if (f.fontProgram.empty()) continue;
          FT_Face face = nullptr;
          if (FT_New_Memory_Face(lib,
                                 reinterpret_cast<const FT_Byte*>(f.fontProgram.data()),
                                 static_cast<FT_Long>(f.fontProgram.size()), 0,
                                 &face) != 0) {
            f.ftValid = false;
            continue;
          }
          f.ftLoaded = true;
          f.cmapCount = face->num_charmaps;
          TT_OS2* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
          if (os2) f.fsType = os2->fsType;
          char key[32];
          std::snprintf(key, sizeof(key), "%d,%d", f.og.getObj(), f.og.getGen());
          faces[key] = face;
        }
        for (TextEvent& e : ev.texts) {
          char key[32];
          std::snprintf(key, sizeof(key), "%d,%d", e.fontOg.getObj(), e.fontOg.getGen());
          auto it = faces.find(key);
          FontFacts* ff = nullptr;
          for (FontFacts& f : ev.fonts) {
            if (f.og == e.fontOg) ff = &f;
          }
          if (ff && !ff->hasToUnicode &&
              ((ff->encodingName.empty() && ff->symbolic) || ff->cid)) {
            e.mappedToUnicode = false;
            ff->allUsedMapped = false;
          }
          if (it == faces.end() || !ff || ff->cid) continue;
          FT_Face face = it->second;
          for (unsigned char c : e.bytes) {
            if (c == ' ') e.glyphWhitespace = true;
            FT_UInt gid = FT_Get_Char_Index(face, c);
            if (gid == 0) {
              for (int cm = 0; cm < face->num_charmaps && gid == 0; ++cm) {
                if (FT_Set_Charmap(face, face->charmaps[cm]) == 0) {
                  gid = FT_Get_Char_Index(face, c);
                  if (gid == 0 && c < 0xF0) {
                    gid = FT_Get_Char_Index(face, 0xF000 + c);
                  }
                }
              }
            }
            if (gid == 0 && ff->symbolic && ff->trueType &&
                static_cast<long>(c) < face->num_glyphs) {
              gid = c;
            }
            if (gid == 0) {
              e.glyphUndefined = true;
              ff->anyUndefinedGlyph = true;
              continue;
            }
            if (FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE) == 0) {
              if (face->glyph->format == FT_GLYPH_FORMAT_OUTLINE &&
                  face->glyph->outline.n_points == 0 && c != ' ' &&
                  face->glyph->advance.x == 0) {
                e.glyphHasContour = false;
              }
              int wi = c - ff->firstChar;
              if (wi >= 0 && wi < static_cast<int>(ff->widths.size()) &&
                  ff->widths[wi] >= 0 && face->units_per_EM > 0) {
                double progW = face->glyph->advance.x * 1000.0 / face->units_per_EM;
                if (std::abs(progW - ff->widths[wi]) > 2.0) ff->anyWidthMismatch = true;
              }
            }
          }
        }
        for (auto& [k, face] : faces) FT_Done_Face(face);
        FT_Done_FreeType(lib);
      }
    }
    if (needStruct) {
      QPDFObjectHandle root = ctx.pdf.getRoot();
      QPDFObjectHandle str = root.getKey("/StructTreeRoot");
      if (!str.isDictionary()) {
        ev.docIssues.insert("noStructTree");
      } else {
        if (!str.getKey("/ParentTree").isDictionary()) ev.docIssues.insert("noParentTree");
        static const std::set<std::string> kStd = {
            "Document", "Part", "Art", "Sect", "Div", "BlockQuote", "Caption", "TOC",
            "TOCI", "Index", "NonStruct", "Private", "P", "H", "H1", "H2", "H3", "H4",
            "H5", "H6", "L", "LI", "Lbl", "LBody", "Table", "TR", "TH", "TD", "THead",
            "TBody", "TFoot", "Span", "Quote", "Note", "Reference", "BibEntry", "Code",
            "Link", "Annot", "Ruby", "RB", "RT", "RP", "Warichu", "WT", "WP", "Figure",
            "Formula", "Form"};
        QPDFObjectHandle roleMap = str.getKey("/RoleMap");
        bool hasClassRef = false;
        std::vector<QPDFObjectHandle> stack{str};
        Visited seen;
        int guard = 0;
        while (!stack.empty() && ++guard < 200000) {
          QPDFObjectHandle n = stack.back();
          stack.pop_back();
          if (!n.isDictionary() || !seen.enter(n)) continue;
          QPDFObjectHandle st = n.getKey("/S");
          if (st.isName()) {
            std::string ty = st.getName().substr(1);
            bool mapped = roleMap.isDictionary() && !roleMap.getKey("/" + ty).isNull();
            if (!kStd.count(ty) && !mapped) ev.docIssues.insert("notStandardType");
            bool utf = true;
            for (unsigned char c : ty) {
              if (c >= 0x80) utf = false;
            }
            if (!utf) ev.docIssues.insert("structTypeNameIsUTF8Encoded");
          }
          if (!n.getKey("/C").isNull() && !str.getKey("/ClassMap").isDictionary()) {
            hasClassRef = true;
          }
          QPDFObjectHandle kids = n.getKey("/K");
          if (kids.isArray()) {
            for (int i = 0; i < kids.getArrayNItems(); ++i) {
              stack.push_back(kids.getArrayItem(i));
            }
          } else if (kids.isDictionary()) {
            stack.push_back(kids);
          }
        }
        if (hasClassRef) ev.docIssues.insert("noClassMap");
      }
      if (ev.qpdfWarnings > 0) ev.docIssues.insert("parserWarnings");
    }
    if (needXmp) {
      QPDFObjectHandle md = ctx.pdf.getRoot().getKey("/Metadata");
      if (md.isStream()) {
        try {
          auto buf = md.getStreamData(qpdf_dl_all);
          size_t n = std::min<size_t>(buf->getSize(), 4u << 20);
          ev.xmpRaw.assign(reinterpret_cast<const char*>(buf->getBuffer()), n);
        } catch (...) {
        }
      }
    }
  }

  {
    auto sevLabel = [](int sev) {
      if (sev == 0 || sev >= 3) return "Error";
      return sev == 1 ? "Warning" : "Info";
    };
    auto emitB = [&](const PfBuiltin& b, long long n, const std::set<int>& pages,
                     const std::string& what) {
      if (n <= 0) return;
      std::string detail = std::string(sevLabel(b.severity)) + ": " + what + " (" +
                           std::to_string(n) + " hit(s)";
      if (!pages.empty()) detail += ", page " + std::to_string(*pages.begin());
      detail += ")";
      ctx.res.analysis.push_back({"PROFILE_HIT", detail, false});
    };
    auto isDevIndep = [](const ColorInfo& ci) {
      return ci.cls == "icc" || ci.cls == "cal" || ci.cls == "lab";
    };
    auto onColourPlates = [](const ColorInfo& ci, const std::vector<double>& comps) {
      if (ci.cls == "rgb" || ci.cls == "lab") return true;
      if (ci.cls == "icc" || ci.cls == "cal") return true;
      if (ci.cls == "cmyk") {
        if (comps.size() == 4) {
          return comps[0] > 0.001 || comps[1] > 0.001 || comps[2] > 0.001;
        }
        return true;
      }
      if (ci.cls == "separation" || ci.cls == "devicen") {
        for (const std::string& c : ci.colorants) {
          if (c != "Black" && c != "Gray" && c != "None") return true;
        }
      }
      return false;
    };
    for (const PfBuiltin& b : prof.builtins) {
      const std::string& nm = b.name;
      long long n = 0;
      std::set<int> pg;
      if (nm == "PRCWzPage_SizeOrientDifferent") {
        for (size_t i = 1; i < ev.pages.size(); ++i) {
          if (std::abs(ev.pages[i].wPt - ev.pages[0].wPt) > 3 ||
              std::abs(ev.pages[i].hPt - ev.pages[0].hPt) > 3) {
            ++n;
            pg.insert(ev.pages[i].page);
          }
        }
        emitB(b, n, pg, "Pages differ in size or orientation");
      } else if (nm == "PRCWzPage_OnePageEmpty") {
        for (const PageFacts& p : ev.pages) {
          if (p.empty) {
            ++n;
            pg.insert(p.page);
          }
        }
        emitB(b, n, pg, "Empty page");
      } else if (nm == "PRCWzImag_ResImgLower" || nm == "PRCWzImag_ResImgUpper" ||
                 nm == "PRCWzImag_ResBmpLower" || nm == "PRCWzImag_ResBmpUpper") {
        bool bmp = nm.find("Bmp") != std::string::npos;
        bool lower = nm.find("Lower") != std::string::npos;
        double ppi = 0;
        auto it = b.params.find("PixelsPerInch");
        if (it != b.params.end()) ppi = it->second;
        if (ppi > 0) {
          for (const ImageEvent& e : ev.images) {
            bool isBmp = e.bpc == 1 || e.mask;
            if (isBmp != bmp || e.ppi <= 0) continue;
            if ((lower && e.ppi < ppi) || (!lower && e.ppi > ppi)) {
              ++n;
              pg.insert(e.page);
            }
          }
          emitB(b, n, pg,
                std::string(bmp ? "Bitmap" : "Image") + " resolution " +
                    (lower ? "below " : "above ") + std::to_string((int)ppi) + " ppi");
        }
      } else if (nm == "PRCWzColr_CMYSeparations") {
        for (const PaintEvent& e : ev.paints) {
          if (!e.noPaint && onColourPlates(e.color, e.comps)) { ++n; pg.insert(e.page); }
        }
        for (const TextEvent& e : ev.texts) {
          if (e.renderMode != 3 && onColourPlates(e.color, e.comps)) { ++n; pg.insert(e.page); }
        }
        for (const ImageEvent& e : ev.images) {
          if (!e.mask && onColourPlates(e.color, {})) { ++n; pg.insert(e.page); }
        }
        emitB(b, n, pg, "Objects produce colour plate output (CMY)");
      } else if (nm == "PRCWzColr_DICS") {
        for (const PaintEvent& e : ev.paints) {
          if (!e.noPaint && isDevIndep(e.color)) { ++n; pg.insert(e.page); }
        }
        for (const TextEvent& e : ev.texts) {
          if (e.renderMode != 3 && isDevIndep(e.color)) { ++n; pg.insert(e.page); }
        }
        for (const ImageEvent& e : ev.images) {
          if (isDevIndep(e.color)) { ++n; pg.insert(e.page); }
        }
        emitB(b, n, pg, "Device-independent colour in use");
      } else if (nm == "PRCWzColr_RGB") {
        for (const PaintEvent& e : ev.paints) {
          if (!e.noPaint && e.color.cls == "rgb") { ++n; pg.insert(e.page); }
        }
        for (const TextEvent& e : ev.texts) {
          if (e.renderMode != 3 && e.color.cls == "rgb") { ++n; pg.insert(e.page); }
        }
        for (const ImageEvent& e : ev.images) {
          if (e.color.cls == "rgb") { ++n; pg.insert(e.page); }
        }
        emitB(b, n, pg, "Object uses RGB");
      } else if (nm == "PRCWzColr_MoreThan") {
        double lim = 0;
        auto it = b.params.find("SpotColorSepsOnPage");
        if (it != b.params.end()) lim = it->second;
        if (static_cast<double>(ev.spotPlates.size()) > lim) {
          n = static_cast<long long>(ev.spotPlates.size());
          emitB(b, n, pg, "More spot colour separations than allowed");
        }
      } else if (nm == "PRCWzFont_NotEmbedded") {
        for (const FontFacts& f : ev.fonts) {
          if (!f.embedded) ++n;
        }
        emitB(b, n, pg, "Font not embedded");
      } else if (nm == "PRCWzFont_Type1CID") {
        for (const FontFacts& f : ev.fonts) {
          if (f.cid0) ++n;
        }
        emitB(b, n, pg, "Uses CID Type 1 font");
      } else if (nm == "PRCWzFont_TrueTypeCID") {
        for (const FontFacts& f : ev.fonts) {
          if (f.cid && f.trueType) ++n;
        }
        emitB(b, n, pg, "Uses CID Type 2 font");
      } else if (nm == "PRCWzFont_OpenType") {
        for (const FontFacts& f : ev.fonts) {
          if (f.openType) ++n;
        }
        emitB(b, n, pg, "Uses OpenType font");
      } else if (nm == "PRCWzColr_InconsistentNaming") {
        for (const auto& [name, alts] : ev.spotAlternates) {
          if (alts.size() > 1) ++n;
        }
        emitB(b, n, pg, "Spot colour with inconsistent representation");
      } else if (nm == "PRCWzDocu_Encrypted") {
        emitB(b, ev.encrypted ? 1 : 0, pg, "Document is encrypted");
      } else if (nm == "PRCWzDocu_Damaged") {
        emitB(b, ev.qpdfWarnings > 0 ? 1 : 0, pg, "Document structure needed repair");
      } else if (nm == "PRCWzDocu_SyntaxChecks") {
        emitB(b, ev.qpdfWarnings, pg, "PDF syntax issue");
      } else if (nm == "PRCWzDocu_RequiresAtLeast") {
        double want = 0;
        auto it = b.params.find("AcroVers");
        if (it != b.params.end()) want = it->second;
        double have = 0;
        if (!ev.pdfVersion.empty()) {
          double v = std::atof(ev.pdfVersion.c_str());
          have = v >= 2.0 ? 8 : (v >= 1.7 ? 8 : (v - 1.0) * 10 + 1);
        }
        emitB(b, have < want ? 1 : 0, pg, "Requires a newer PDF version");
      } else if (nm == "PRCWzFont_Embedded") {
        for (const FontFacts& f : ev.fonts) {
          if (f.embedded) ++n;
        }
        emitB(b, n, pg, "Font is embedded");
      } else if (nm == "PRCWzImag_NotUncompressed") {
        for (const ImageEvent& e : ev.images) {
          if (!e.filters.empty()) ++n;
        }
        emitB(b, n, pg, "Image is compressed");
      } else if (nm == "PRCWzPage_NumPages") {
        emitB(b, ev.pageCount, pg, "Document page count");
      } else if (nm == "PRCWzRend_Curve") {
        emitB(b, ev.hasTransferCurve ? 1 : 0, pg, "Transfer curve in use");
      } else if (nm == "PRCWzRend_Halftone") {
        emitB(b, ev.hasHalftoneDict ? 1 : 0, pg, "Halftone screening in use");
      } else if (nm == "PRCWzRend_Postscript") {
        emitB(b, ev.hasPostScriptXObject ? 1 : 0, pg, "PostScript XObject in use");
      } else if (nm == "PRCWzRend_Transparency") {
        for (const PaintEvent& e : ev.paints) {
          if (e.transparency) { ++n; pg.insert(e.page); }
        }
        for (const TextEvent& e : ev.texts) {
          if (e.transparency) { ++n; pg.insert(e.page); }
        }
        emitB(b, n, pg, "Transparency in use");
      } else if (nm == "PRCWzRend_Thickness") {
        double pts = 0.14;
        auto it = b.params.find("Points");
        if (it != b.params.end()) pts = it->second;
        for (const PaintEvent& e : ev.paints) {
          if (e.stroke && e.width > 0 && e.width < pts) { ++n; pg.insert(e.page); }
        }
        emitB(b, n, pg, "Line thickness below the minimum");
      }
    }
  }

  const char* sevName[] = {"", "Info", "Warning", "Error"};
  auto humanTail = [](const std::string& token) {
    size_t p = token.find("::");
    std::string t = p == std::string::npos ? token : token.substr(p + 2);
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
      char c = t[i];
      if (i && std::isupper(static_cast<unsigned char>(c)) &&
          std::islower(static_cast<unsigned char>(t[i - 1]))) {
        out += ' ';
      }
      out += (i == 0) ? c : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (char& c : out) {
      if (c == '_') c = ' ';
    }
    return out;
  };
  auto opPhrase = [](const std::string& op) {
    if (op == "less") return std::string("below");
    if (op == "less_or_equal") return std::string("at most");
    if (op == "greater") return std::string("above");
    if (op == "greater_or_equal") return std::string("at least");
    if (op == "equal") return std::string("of");
    if (op == "unequal") return std::string("other than");
    if (op == "is_not_true") return std::string("not");
    return std::string();
  };
  auto displayName = [&](const PfRule& rule,
                         const std::vector<const PfCondition*>& conds) {
    if (rule.name.rfind("R_", 0) != 0 && rule.name.rfind("RR", 0) != 0 &&
        rule.name.rfind("P_", 0) != 0) {
      return rule.name;
    }
    std::vector<std::string> parts;
    for (const PfCondition* c : conds) {
      for (const PfAtom& a : c->atoms) {
        if (parts.size() >= 3) break;
        std::string p = humanTail(a.token);
        std::string ph = opPhrase(a.op);
        if (a.op == "is_true" || a.op == "is_not_true") {
          parts.push_back(ph.empty() ? p : ph + " " + p);
        } else if (!a.vals.empty() && !a.vals[0].empty()) {
          parts.push_back(p + (ph.empty() ? " " : " " + ph + " ") + a.vals[0]);
        } else {
          parts.push_back(p);
        }
      }
    }
    if (parts.empty()) return rule.name;
    std::string out = parts[0];
    for (size_t i = 1; i < parts.size() && i < 3; ++i) out += "; " + parts[i];
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    if (out.size() > 90) out = out.substr(0, 87) + "...";
    return out;
  };
  std::vector<bool> usedPaint(ev.paints.size(), false);
  std::vector<bool> usedText(ev.texts.size(), false);
  std::vector<bool> usedImage(ev.images.size(), false);
  auto presenceHit = [&](const PfRule& rule) -> int {
    std::vector<const PfAtom*> at;
    for (const std::string& cid : rule.condIds) {
      auto it = prof.conds.find(cid);
      if (it == prof.conds.end()) continue;
      for (const PfAtom& a : it->second.atoms) at.push_back(&a);
    }
    if (at.size() != 1) return -1;
    const std::string& t = at[0]->token;
    const std::string& op = at[0]->op;
    bool pos = op == "is_true";
    bool listNot = op == "is_not_contained_in" || op == "not_contains";
    if (t == "CSIMAGE::HasSMaskEntry" && pos && ev.tpSMaskImg) return 1;
    if (t == "CSGST_G::HasSMaskEntry" && pos && ev.tpSMaskGs) return 1;
    if (t == "CSGST_G::BlendMode" && listNot && ev.tpBlend) return 1;
    return -1;
  };
  for (const PfRule& rule : prof.rules) {
    {
      int ph = presenceHit(rule);
      if (ph >= 0) {
        if (ph > 0) {
          std::vector<const PfCondition*> pc;
          for (const std::string& cid : rule.condIds) {
            auto it = prof.conds.find(cid);
            if (it != prof.conds.end()) pc.push_back(&it->second);
          }
          const char* sv[] = {"", "Info", "Warning", "Error"};
          int s = rule.severity < 4 && rule.severity > 0 ? rule.severity : 1;
          ctx.res.analysis.push_back(
              {"PROFILE_HIT",
               std::string(sv[s]) + ": " + displayName(rule, pc) + " (1 hit(s))", false});
        }
        continue;
      }
    }
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
    bool sawFont = false;
    for (const PfAtom* a : atoms) {
      Domain d = atomDomain(a->token);
      if (d == Domain::kNone) mixed = true;
      else if (d == Domain::kDoc) continue;
      else if (d == Domain::kAny) sawAny = true;
      else if (d == Domain::kFont) sawFont = true;
      else if (dom == Domain::kNone) dom = d;
      else if (dom != d && d == Domain::kPage) continue;
      else if (dom != d && dom == Domain::kPage) dom = d;
      else if (dom != d) mixed = true;
    }
    if (sawFont) {
      if (dom == Domain::kNone) dom = sawAny ? Domain::kText : Domain::kFont;
      else if (dom != Domain::kText) mixed = true;
    }
    bool anyFallback = false;
    if (dom == Domain::kNone && sawAny) {
      dom = Domain::kPaint;
      anyFallback = true;
      for (const PfAtom* a : atoms) {
        bool positive = a->op == "is_true";
        if (positive && a->token == "CONTSTM::IsText") {
          dom = Domain::kText;
          anyFallback = false;
        }
        if (positive && (a->token == "CONTSTM::IsImage" || a->token == "CONTSTM::IsImageMask" ||
                         a->token == "CONTSTM::IsBitmapImageOrImageMask")) {
          dom = Domain::kImage;
          anyFallback = false;
        }
      }
    }
    bool supported = !mixed;
    long long hits = 0;
    std::set<int> pages;

    int curPage = 0;
    std::string unsupTok;
    auto evalAtom = [&](const PfAtom& a, const void* e) -> bool {
      bool wasSupported = supported;
      if (atomDomain(a.token) == Domain::kDoc) return evalDocAtom(a, ev, supported);
      if (atomDomain(a.token) == Domain::kPage && dom != Domain::kPage) {
        const PageFacts* p = pageFor(ev, curPage);
        return p ? evalPageAtom(a, *p, supported) : false;
      }
      switch (dom) {
        case Domain::kPaint: {
          bool r = evalPaintAtom(a, *static_cast<const PaintEvent*>(e), ev, supported);
          if (wasSupported && !supported) unsupTok = a.token;
          return r;
        }
        case Domain::kText: {
          bool r = evalTextAtom(a, *static_cast<const TextEvent*>(e), ev, supported);
          if (wasSupported && !supported) unsupTok = a.token;
          return r;
        }
        case Domain::kImage: {
          bool r = evalImageAtom(a, *static_cast<const ImageEvent*>(e), ev, supported);
          if (wasSupported && !supported) unsupTok = a.token;
          return r;
        }
        case Domain::kPage:
          return evalPageAtom(a, *static_cast<const PageFacts*>(e), supported);
        case Domain::kAnnot:
          return evalAnnotAtom(a, *static_cast<const AnnotFacts*>(e), ev, supported);
        case Domain::kFont:
          return evalFontAtom(a, *static_cast<const FontFacts*>(e), ev, supported);
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

    auto inScope = [&](const Box& bbox, int page) {
      if (rule.scope < 2 || !bbox.valid) return true;
      const PageFacts* p = pageFor(ev, page);
      if (!p) return true;
      Box area;
      if (rule.scope == 2) area = p->trim.valid ? p->trim : (p->hasCropBox ? p->media : p->media);
      else area = p->bleed.valid ? p->bleed : (p->trim.valid ? p->trim : p->media);
      if (!area.valid) return true;
      return boxesIntersect(area, bbox);
    };
    auto bboxOf = [&](const void* e) -> const Box& {
      static Box none;
      switch (dom) {
        case Domain::kPaint: return static_cast<const PaintEvent*>(e)->bbox;
        case Domain::kText: return static_cast<const TextEvent*>(e)->bbox;
        case Domain::kImage: return static_cast<const ImageEvent*>(e)->bbox;
        default: return none;
      }
    };
    auto sweep = [&](auto& collection, std::vector<bool>* used) {
      for (size_t i = 0; i < collection.size(); ++i) {
        if (!supported) return;
        if (used && (*used)[i]) continue;
        const auto& e = collection[i];
        curPage = e.page;
        if ((dom == Domain::kPaint || dom == Domain::kText || dom == Domain::kImage) &&
            !inScope(bboxOf(&e), e.page)) {
          continue;
        }
        if (ruleMatches(&e)) {
          ++hits;
          pages.insert(e.page);
          if (used) (*used)[i] = true;
        }
      }
    };
    if (supported && dom == Domain::kPaint && anyFallback) {
      sweep(ev.paints, &usedPaint);
      if (supported) {
        dom = Domain::kText;
        sweep(ev.texts, &usedText);
      }
      if (supported) {
        dom = Domain::kImage;
        sweep(ev.images, &usedImage);
      }
      dom = Domain::kPaint;
    } else if (supported && dom == Domain::kPaint) {
      sweep(ev.paints, &usedPaint);
    } else if (supported && dom == Domain::kText) sweep(ev.texts, &usedText);
    else if (supported && dom == Domain::kImage) sweep(ev.images, &usedImage);
    else if (supported && dom == Domain::kPage) sweep(ev.pages, nullptr);
    else if (supported && dom == Domain::kAnnot) {
      for (const auto& e : ev.annots) {
        if (!supported) break;
        if (ruleMatches(&e)) {
          ++hits;
          pages.insert(e.page);
        }
      }
    } else if (supported && dom == Domain::kFont) {
      for (const auto& e : ev.fonts) {
        if (!supported) break;
        if (ruleMatches(&e)) ++hits;
      }
    }
    else if (supported && dom == Domain::kNone) {
      if (ruleMatches(nullptr)) hits = 1;
    }
    if (!supported) {
      ctx.res.analysis.push_back(
          {"PROFILE_RULE_UNSUPPORTED",
           rule.name + ": uses checks Kura cannot evaluate yet" +
               (unsupTok.empty() ? "" : " (" + unsupTok + ")"),
           false});
      continue;
    }
    if (hits) {
      std::string detail = std::string(sevName[rule.severity < 4 && rule.severity > 0
                                                   ? rule.severity : 1]) +
                           ": " + displayName(rule, conds) + " (" + std::to_string(hits) +
                           " hit(s)";
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

void applyProfileFixes(Options& opt, std::vector<Issue>& notes) {
  std::vector<PfFix> fixes = collectFixes(opt.preflightProfile);
  if (fixes.empty()) return;
  static const std::set<std::string> coveredByConversion = {
      "removenotdef", "embeddoutputintent", "correctpageboxes", "fixglyphwidthinfo",
      "removenoncompliantpdfametadata", "pdfversion", "usflttencdnnencddstrms",
      "objctcmprssnoptns", "removejavascript", "halftones", "embedmissingfonts",
      "embedfonts", "subsetfonts", "syncdocinfo", "removecidset", "fixcidset",
      "fixcharset", "removecharset", "insertcmapforcidfonts", "fixcidtogidmap",
      "correctcidsysteminfo", "removeembeddedpostscript", "removeopi",
      "removealternateimages", "dscrdembdddthmbnls", "dscrdprvtdtofothrapps",
      "removejobtickets", "rmvunrfrncdnmddstntns", "rmvinvldbkmrks", "rmvinvldlnks",
      "removeactions", "dscrdfrmactns", "reduceimagebitdepth", "rcmprsslzwtflt",
      "removepdfakeys", "removepdfxkeys", "removepdfekeys", "removeembeddedfiles",
      "removeoutputintent", "disambiguateapproxj2k", "makefontnameunique",
      "addmissingspaceglyphs", "removeinvalidglyph", "removeaddcmapsfromsymttf",
      "repairfs", "setpdfua_1entry", "setdoclangfromtagging", "settabordertodocstruct",
      "marknonstructasartifact", "markheadfootaspagartifact", "mergeadjacentheadings",
      "correctlangforlistlable", "setlabelsinunorderedlists", "adduniqueidtonotese",
      "removeemptyble", "createcontentinlinkannot", "createbookmarkfromheading",
      "setstructelemtype", "repairtaggingidtree", "removerolemapdeftags",
      "convertallnamestoutf8", "removeallxmpmanifest", "adjustlayersforpdfx4", "forms",
      "flattentransparency",
  };
  static const std::set<std::string> colourEngine = {
      "ccsettings", "ccpolicy", "ccdestination", "devicelinkconversion",
      "managecolorandmodifyoi", "quickcolorconversion", "mapcolors", "adjustdotgain",
  };
  static const std::set<std::string> directOps = {
      "rotatepages", "scalepagesex", "removepagescaling", "setpagebox",
      "setpageboxesbasedonmarks", "generatebleed", "settitle", "trappedkey",
      "setinitialviewdocumentoptions", "setinitialviewuioptions",
      "setinitialviewwindowoptions", "modifyinterpolateentry", "removeflatness",
      "removesmoothness", "transfercurves", "removebg", "removeucr",
      "removerenderingintents", "setrenderingintent",
      "removeunnecessarytransparencygroups", "mergespotcolornames",
      "makecustomspotcolornamesconsistent", "mksptclrappcnsistent", "mapspotcolors",
      "convertregistrationcolortoblack", "convertnchtodevn", "knockoutwhite",
      "overprintblack", "setoverprintandknockout", "increaselinewidth",
      "settextrendermode", "removeobjectsoutofbox", "placetext", "annotation",
      "putobjectsonlayer", "putobjpsteps", "dscdhdnlycntfltnvsblyrs",
  };
  static const std::set<std::string> partialOps = {
      "duplicatetextasinvisible", "removecontentbyimage", "dtctandmrgimgfrgmnts",
      "bringtofront",
  };
  std::map<std::string, int> unsupported;
  std::set<std::string> notedCovered, notedColour, notedPartial, notedDirect;
  double dsTarget = 0;
  bool outline = false;
  for (const PfFix& f : fixes) {
    std::string op = lower(f.op);
    if (op == "dsrcmpclrimgs" || op == "dsrcmpgscimgs" || op == "dsrcmpmchimgs") {
      double target = f.params.size() > 1 ? std::atof(f.params[1].c_str()) : 0;
      if (target > 0) dsTarget = std::max(dsTarget, target);
      continue;
    }
    if (op == "convertfontstooutlines" || op == "converttruetypetocff") {
      outline = true;
      continue;
    }
    if (op == "optmzefrfstwbvw") {
      opt.linearize = true;
      notes.push_back({"PROFILE_FIX_APPLIED",
                       "fast web view enabled (output will be linearized)", true});
      continue;
    }
    if (coveredByConversion.count(op)) {
      if (notedCovered.insert(op).second) {
        notes.push_back({"PROFILE_FIX_COVERED",
                         f.op + ": performed as part of standard conversion", true});
      }
      continue;
    }
    if (colourEngine.count(op)) {
      if (notedColour.insert(op).second) {
        notes.push_back(
            {"PROFILE_FIX_COVERED",
             f.op + ": colour normalization performed by conversion for the target "
                    "standard (device-link profiles approximated by ICC pairs)",
             true});
      }
      continue;
    }
    if (directOps.count(op)) {
      opt.profileFixOps.push_back({op, f.params});
      if (notedDirect.insert(op).second) {
        notes.push_back({"PROFILE_FIX_APPLIED", f.op + ": scheduled", true});
      }
      continue;
    }
    if (partialOps.count(op)) {
      if (notedPartial.insert(op).second) {
        notes.push_back({"PROFILE_FIX_PARTIAL",
                         f.op + ": content-level rewrite not performed; closest "
                                "normalization applied by conversion",
                         false});
      }
      continue;
    }
    ++unsupported[f.op];
  }
  if (dsTarget > 0) {
    if (opt.imageMaxPpi <= 0 || dsTarget < opt.imageMaxPpi) opt.imageMaxPpi = dsTarget;
    char buf[120];
    std::snprintf(buf, sizeof(buf),
                  "image downsampling enabled at %g ppi from the profile's fix steps",
                  dsTarget);
    notes.push_back({"PROFILE_FIX_APPLIED", buf, true});
  }
  if (outline) {
    opt.outlineFonts = true;
    notes.push_back({"PROFILE_FIX_APPLIED",
                     "font outlining enabled from the profile's fix steps", true});
  }
  for (const auto& [op, n] : unsupported) {
    notes.push_back({"PROFILE_FIX_UNSUPPORTED",
                     op + ": fix operation Kura cannot run yet" +
                         (n > 1 ? " (x" + std::to_string(n) + ")" : ""),
                     false});
  }
}

namespace {
double unitPt(const std::string& u) {
  if (u == "mm") return 72.0 / 25.4;
  if (u == "cm") return 72.0 / 2.54;
  if (u == "in" || u == "inch") return 72.0;
  return 1.0;
}

class OverprintFilter : public QPDFObjectHandle::TokenFilter {
 public:
  OverprintFilter(bool knockWhite, bool opBlack, bool textOnly, bool vectorOnly,
                  double minWidth, int forceTr)
      : knockWhite_(knockWhite), opBlack_(opBlack), textOnly_(textOnly),
        vectorOnly_(vectorOnly), minWidth_(minWidth), forceTr_(forceTr) {}

  void handleToken(QPDFTokenizer::Token const& tok) override {
    using TT = QPDFTokenizer;
    if (tok.getType() == TT::tt_integer || tok.getType() == TT::tt_real) {
      nums_.push_back(std::atof(tok.getValue().c_str()));
      pending_.push_back(tok);
      return;
    }
    if (tok.getType() != TT::tt_word) {
      pending_.push_back(tok);
      flushPending();
      return;
    }
    std::string op = tok.getValue();
    bool isFillOp = op == "f" || op == "F" || op == "f*" || op == "B" || op == "B*" ||
                    op == "b" || op == "b*";
    bool isStrokeOp = op == "S" || op == "s" || op == "B" || op == "B*" || op == "b" ||
                      op == "b*";
    bool isTextOp = op == "Tj" || op == "TJ" || op == "'" || op == "\"";
    if (op == "G" && nums_.size() >= 1) stroke_ = {nums_.back()};
    else if (op == "RG" && nums_.size() >= 3) stroke_ = {nums_[nums_.size()-3], nums_[nums_.size()-2], nums_.back()};
    else if (op == "K" && nums_.size() >= 4) stroke_ = {nums_[nums_.size()-4], nums_[nums_.size()-3], nums_[nums_.size()-2], nums_.back()};
    else if ((op == "SC" || op == "SCN") && !nums_.empty()) stroke_ = nums_;
    else if (op == "g" && nums_.size() >= 1) fill_ = {nums_.back()};
    else if (op == "rg" && nums_.size() >= 3) fill_ = {nums_[nums_.size()-3], nums_[nums_.size()-2], nums_.back()};
    else if (op == "k" && nums_.size() >= 4) fill_ = {nums_[nums_.size()-4], nums_[nums_.size()-3], nums_[nums_.size()-2], nums_.back()};
    else if ((op == "sc" || op == "scn") && !nums_.empty()) fill_ = nums_;
    if (minWidth_ > 0 && op == "w" && !nums_.empty() && nums_.back() < minWidth_) {
      pending_.clear();
      nums_.clear();
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%g w", minWidth_);
      writeToken(QPDFTokenizer::Token(TT::tt_word, buf));
      write("\n");
      return;
    }
    if (forceTr_ >= 0 && op == "Tr" && !nums_.empty()) {
      pending_.clear();
      nums_.clear();
      char buf[24];
      std::snprintf(buf, sizeof(buf), "%d Tr", forceTr_);
      write(buf);
      write("\n");
      return;
    }
    bool actOn = (isFillOp || isStrokeOp) ? !textOnly_ : (isTextOp ? !vectorOnly_ : false);
    if (actOn && (isFillOp || isStrokeOp || isTextOp)) {
      const std::vector<double>& c = (isStrokeOp && !isFillOp) ? stroke_ : fill_;
      if (knockWhite_ && isWhiteVec(c)) write("/KuraKO gs\n");
      else if (opBlack_ && is100kVec(c)) write("/KuraOB gs\n");
    }
    pending_.push_back(tok);
    flushPending();
    nums_.clear();
  }

  void handleEOF() override { flushPending(); }

 private:
  static bool isWhiteVec(const std::vector<double>& c) {
    if (c.empty()) return false;
    if (c.size() == 4) {
      for (double v : c) {
        if (v > 0.001) return false;
      }
      return true;
    }
    for (double v : c) {
      if (v < 0.999) return false;
    }
    return true;
  }
  static bool is100kVec(const std::vector<double>& c) {
    if (c.size() == 4) {
      return c[3] > 0.999 && c[0] < 0.001 && c[1] < 0.001 && c[2] < 0.001;
    }
    if (c.size() == 1) return c[0] < 0.001;
    return false;
  }
  void flushPending() {
    for (auto& t : pending_) {
      writeToken(t);
      write(" ");
    }
    pending_.clear();
  }
  bool knockWhite_, opBlack_, textOnly_, vectorOnly_;
  double minWidth_;
  int forceTr_;
  std::vector<double> nums_;
  std::vector<QPDFTokenizer::Token> pending_;
  std::vector<double> fill_{0};
  std::vector<double> stroke_{0};
};

void scrubExtGStates(Ctx& ctx, const std::set<std::string>& keys) {
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (!obj.isDictionary() || !nameIs(obj.getKey("/Type"), "/ExtGState")) continue;
    for (const std::string& k : keys) {
      if (obj.hasKey(k)) obj.removeKey(k);
    }
  }
}

QPDFObjectHandle boxOnPage(QPDFPageObjectHelper& ph, const std::string& name) {
  QPDFObjectHandle b = ph.getAttribute("/" + name, name == "MediaBox");
  if (!b.isArray() && name != "MediaBox") {
    b = ph.getAttribute(name == "TrimBox" ? "/CropBox" : "/MediaBox", true);
  }
  if (!b.isArray()) b = ph.getAttribute("/MediaBox", true);
  return b;
}
}

void passProfileFixups(Ctx& ctx) {
  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
  auto note = [&](const std::string& what) {
    ctx.issue("PROFILE_FIX_DONE", what, true);
  };
  bool knockWhite = false, opBlack = false, textOnly = false, vectorOnly = false;
  double minWidth = 0;
  int forceTr = -1;
  for (const auto& [op, params] : ctx.opt.profileFixOps) {
    auto p = [&](size_t i) { return i < params.size() ? params[i] : std::string(); };
    if (op == "rotatepages") {
      int ang = std::atoi(p(0).c_str());
      if (ang % 90 == 0 && ang % 360 != 0) {
        for (auto& ph : pages) {
          QPDFObjectHandle page = ph.getObjectHandle();
          int cur = page.getKey("/Rotate").isInteger()
                        ? static_cast<int>(page.getKey("/Rotate").getIntValue()) : 0;
          page.replaceKey("/Rotate", QPDFObjectHandle::newInteger(((cur + ang) % 360 + 360) % 360));
        }
        note("rotated pages by " + std::to_string(ang) + " degrees");
      }
    } else if (op == "removepagescaling") {
      int n = 0;
      for (auto& ph : pages) {
        if (ph.getObjectHandle().hasKey("/UserUnit")) {
          ph.getObjectHandle().removeKey("/UserUnit");
          ++n;
        }
      }
      if (n) note("removed page scaling (/UserUnit) from " + std::to_string(n) + " page(s)");
    } else if (op == "scalepagesex") {
      double tw = std::atof(p(0).c_str()) * unitPt(p(2));
      double th = std::atof(p(1).c_str()) * unitPt(p(2));
      if (tw > 1 && th > 1) {
        for (auto& ph : pages) {
          QPDFObjectHandle mb = ph.getAttribute("/MediaBox", true);
          if (!mb.isArray() || mb.getArrayNItems() != 4) continue;
          double w = numOf(mb.getArrayItem(2), 0) - numOf(mb.getArrayItem(0), 0);
          double h = numOf(mb.getArrayItem(3), 0) - numOf(mb.getArrayItem(1), 0);
          if (w < 1 || h < 1) continue;
          double sc = std::min(tw / w, th / h);
          if (std::abs(sc - 1.0) < 0.001) continue;
          char buf[80];
          std::snprintf(buf, sizeof(buf), "q %g 0 0 %g 0 0 cm\n", sc, sc);
          ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, std::string(buf)), true);
          ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, std::string("\nQ")), false);
          QPDFObjectHandle nb = QPDFObjectHandle::newArray();
          for (double v : {0.0, 0.0, w * sc, h * sc}) {
            nb.appendItem(QPDFObjectHandle::newReal(v, 2));
          }
          QPDFObjectHandle page = ph.getObjectHandle();
          page.replaceKey("/MediaBox", nb);
          for (const char* bx : {"/CropBox", "/TrimBox", "/BleedBox", "/ArtBox"}) {
            if (page.hasKey(bx)) page.removeKey(bx);
          }
        }
        note("scaled pages to fit the requested size");
      }
    } else if (op == "setpagebox" || op == "setpageboxesbasedonmarks") {
      std::string target = op == "setpagebox" && !p(0).empty() ? p(0) : "TrimBox";
      bool onlyMissing = true;
      for (const std::string& prm : params) {
        if (prm == "Always") onlyMissing = false;
      }
      double u = unitPt(p(6));
      double o0 = std::atof(p(2).c_str()) * u, o1 = std::atof(p(3).c_str()) * u;
      double o2 = std::atof(p(4).c_str()) * u, o3 = std::atof(p(5).c_str()) * u;
      for (auto& ph : pages) {
        QPDFObjectHandle page = ph.getObjectHandle();
        if (onlyMissing && page.hasKey("/" + target)) continue;
        std::string refName = p(1).rfind("RelativeTo", 0) == 0 ? p(1).substr(10) : "CropBox";
        QPDFObjectHandle ref = boxOnPage(ph, refName);
        if (!ref.isArray() || ref.getArrayNItems() != 4) continue;
        QPDFObjectHandle nb = QPDFObjectHandle::newArray();
        nb.appendItem(QPDFObjectHandle::newReal(numOf(ref.getArrayItem(0), 0) + o0, 2));
        nb.appendItem(QPDFObjectHandle::newReal(numOf(ref.getArrayItem(1), 0) + o1, 2));
        nb.appendItem(QPDFObjectHandle::newReal(numOf(ref.getArrayItem(2), 0) - o2, 2));
        nb.appendItem(QPDFObjectHandle::newReal(numOf(ref.getArrayItem(3), 0) - o3, 2));
        page.replaceKey("/" + target, nb);
      }
      note("set " + target + " on pages");
    } else if (op == "generatebleed") {
      double amt = p(0) == "Auto" ? 9.0 : std::atof(p(1).c_str()) * unitPt(p(2));
      if (amt <= 0) amt = 9.0;
      for (auto& ph : pages) {
        QPDFObjectHandle page = ph.getObjectHandle();
        QPDFObjectHandle tb = boxOnPage(ph, "TrimBox");
        QPDFObjectHandle mb = ph.getAttribute("/MediaBox", true);
        if (!tb.isArray() || !mb.isArray()) continue;
        QPDFObjectHandle nb = QPDFObjectHandle::newArray();
        nb.appendItem(QPDFObjectHandle::newReal(
            std::max(numOf(tb.getArrayItem(0), 0) - amt, numOf(mb.getArrayItem(0), 0)), 2));
        nb.appendItem(QPDFObjectHandle::newReal(
            std::max(numOf(tb.getArrayItem(1), 0) - amt, numOf(mb.getArrayItem(1), 0)), 2));
        nb.appendItem(QPDFObjectHandle::newReal(
            std::min(numOf(tb.getArrayItem(2), 0) + amt, numOf(mb.getArrayItem(2), 0)), 2));
        nb.appendItem(QPDFObjectHandle::newReal(
            std::min(numOf(tb.getArrayItem(3), 0) + amt, numOf(mb.getArrayItem(3), 0)), 2));
        page.replaceKey("/BleedBox", nb);
      }
      note("generated bleed box from the trim box");
    } else if (op == "settitle") {
      QPDFObjectHandle info = ctx.pdf.getTrailer().getKey("/Info");
      if (!info.isDictionary()) {
        info = QPDFObjectHandle::newDictionary();
        ctx.pdf.getTrailer().replaceKey("/Info", ctx.pdf.makeIndirectObject(info));
      }
      bool ifEmpty = p(0) == "IfEmpty";
      std::string title = p(1);
      if (!(ifEmpty && info.getKey("/Title").isString() &&
            !info.getKey("/Title").getUTF8Value().empty()) && !title.empty()) {
        info.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(title));
        note("set the document title");
      }
    } else if (op == "trappedkey") {
      QPDFObjectHandle info = ctx.pdf.getTrailer().getKey("/Info");
      if (info.isDictionary()) {
        info.replaceKey("/Trapped", QPDFObjectHandle::newName(
            lower(p(0)) == "true" ? "/True" : "/False"));
        note("set the trapped flag");
      }
    } else if (op == "setinitialviewdocumentoptions") {
      QPDFObjectHandle root = ctx.pdf.getRoot();
      static const std::set<std::string> modes = {"UseNone", "UseOutlines", "UseThumbs",
                                                  "FullScreen", "UseOC", "UseAttachments"};
      static const std::set<std::string> layouts = {"SinglePage", "OneColumn",
                                                    "TwoColumnLeft", "TwoColumnRight",
                                                    "TwoPageLeft", "TwoPageRight"};
      for (const std::string& prm : params) {
        if (modes.count(prm)) root.replaceKey("/PageMode", QPDFObjectHandle::newName("/" + prm));
        if (layouts.count(prm)) root.replaceKey("/PageLayout", QPDFObjectHandle::newName("/" + prm));
      }
      note("set initial view document options");
    } else if (op == "setinitialviewuioptions" || op == "setinitialviewwindowoptions") {
      QPDFObjectHandle root = ctx.pdf.getRoot();
      QPDFObjectHandle vp = root.getKey("/ViewerPreferences");
      if (!vp.isDictionary()) {
        vp = QPDFObjectHandle::newDictionary();
        root.replaceKey("/ViewerPreferences", vp);
      }
      const char* uiKeys[] = {"/HideToolbar", "/HideMenubar", "/HideWindowUI"};
      const char* winKeys[] = {"/FitWindow", "/CenterWindow", "/DisplayDocTitle"};
      const char** keys = op == "setinitialviewuioptions" ? uiKeys : winKeys;
      for (size_t i = 0; i < 3 && i < params.size(); ++i) {
        std::string v = lower(params[i]);
        if (v == "true" || v == "false") {
          vp.replaceKey(keys[i], QPDFObjectHandle::newBool(v == "true"));
        }
      }
      note("set initial view preferences");
    } else if (op == "modifyinterpolateentry") {
      bool remove = p(0) == "Remove";
      int n = 0;
      for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
        if (obj.isStream() && nameIs(obj.getDict().getKey("/Subtype"), "/Image")) {
          QPDFObjectHandle d = obj.getDict();
          if (remove && d.hasKey("/Interpolate")) {
            d.removeKey("/Interpolate");
            ++n;
          } else if (!remove) {
            d.replaceKey("/Interpolate", QPDFObjectHandle::newBool(true));
            ++n;
          }
        }
      }
      if (n) note((remove ? "removed" : "set") + std::string(" interpolation on ") +
                  std::to_string(n) + " image(s)");
    } else if (op == "removeflatness") {
      scrubExtGStates(ctx, {"/FL"});
      note("removed flatness overrides");
    } else if (op == "removesmoothness") {
      scrubExtGStates(ctx, {"/SM"});
      note("removed smoothness overrides");
    } else if (op == "transfercurves") {
      scrubExtGStates(ctx, {"/TR", "/TR2"});
      note("removed transfer curves");
    } else if (op == "removebg") {
      scrubExtGStates(ctx, {"/BG", "/BG2"});
      note("removed black generation overrides");
    } else if (op == "removeucr") {
      scrubExtGStates(ctx, {"/UCR", "/UCR2"});
      note("removed undercolour removal overrides");
    } else if (op == "removerenderingintents") {
      scrubExtGStates(ctx, {"/RI"});
      for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
        if (obj.isStream() && nameIs(obj.getDict().getKey("/Subtype"), "/Image") &&
            obj.getDict().hasKey("/Intent")) {
          obj.getDict().removeKey("/Intent");
        }
      }
      note("removed rendering intents");
    } else if (op == "setrenderingintent") {
      std::string in = p(0).empty() ? "RelativeColorimetric" : p(0);
      for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
        if (obj.isDictionary() && nameIs(obj.getKey("/Type"), "/ExtGState")) {
          obj.replaceKey("/RI", QPDFObjectHandle::newName("/" + in));
        }
      }
      note("set rendering intent to " + in);
    } else if (op == "removeunnecessarytransparencygroups") {
      int n = 0;
      for (auto& ph : pages) {
        QPDFObjectHandle page = ph.getObjectHandle();
        QPDFObjectHandle grp = page.getKey("/Group");
        if (!grp.isDictionary() || !nameIs(grp.getKey("/S"), "/Transparency")) continue;
        QPDFObjectHandle res = ph.getAttribute("/Resources", true);
        bool hasTrans = false;
        QPDFObjectHandle egs = res.isDictionary() ? res.getKey("/ExtGState")
                                                  : QPDFObjectHandle::newNull();
        if (egs.isDictionary()) {
          for (const std::string& k : egs.getKeys()) {
            QPDFObjectHandle g = egs.getKey(k);
            if (!g.isDictionary()) continue;
            if ((g.getKey("/CA").isNumber() && g.getKey("/CA").getNumericValue() < 1.0) ||
                (g.getKey("/ca").isNumber() && g.getKey("/ca").getNumericValue() < 1.0) ||
                (!g.getKey("/SMask").isNull() && !nameIs(g.getKey("/SMask"), "/None")) ||
                (g.getKey("/BM").isName() && !nameIs(g.getKey("/BM"), "/Normal") &&
                 !nameIs(g.getKey("/BM"), "/Compatible"))) {
              hasTrans = true;
            }
          }
        }
        if (!hasTrans) {
          page.removeKey("/Group");
          ++n;
        }
      }
      if (n) note("removed " + std::to_string(n) + " unnecessary transparency group(s)");
    } else if (op == "mergespotcolornames" || op == "makecustomspotcolornamesconsistent" ||
               op == "mksptclrappcnsistent" || op == "mapspotcolors" ||
               op == "convertregistrationcolortoblack") {
      std::map<std::string, std::string> canon;
      std::string mapFrom, mapTo;
      if (op == "mapspotcolors" && params.size() >= 3) {
        mapFrom = p(2);
        mapTo = p(0);
      }
      int renamed = 0;
      for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
        if (!obj.isArray() || obj.getArrayNItems() < 2) continue;
        std::string fam = nameOf(obj.getArrayItem(0));
        if (fam != "/Separation" && fam != "/DeviceN") continue;
        auto renameName = [&](QPDFObjectHandle holder, int idx) {
          QPDFObjectHandle nm = holder.getArrayItem(idx);
          if (!nm.isName() || nm.getName().size() < 2) return;
          std::string name = nm.getName().substr(1);
          std::string newName = name;
          if (op == "convertregistrationcolortoblack") {
            if (name == "All" || name == "Registration") newName = "Black";
          } else if (!mapFrom.empty()) {
            if (name == mapFrom) newName = mapTo;
          } else {
            std::string key;
            for (char c : name) {
              if (!std::isspace(static_cast<unsigned char>(c))) {
                key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
              }
            }
            auto it = canon.find(key);
            if (it == canon.end()) canon[key] = name;
            else newName = it->second;
          }
          if (newName != name) {
            holder.setArrayItem(idx, QPDFObjectHandle::newName("/" + newName));
            ++renamed;
          }
        };
        if (fam == "/Separation") renameName(obj, 1);
        else if (obj.getArrayItem(1).isArray()) {
          QPDFObjectHandle names = obj.getArrayItem(1);
          for (int i = 0; i < names.getArrayNItems(); ++i) renameName(names, i);
        }
      }
      if (renamed) note("unified " + std::to_string(renamed) + " spot colourant name(s)");
    } else if (op == "convertnchtodevn") {
      int n = 0;
      for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
        if (obj.isArray() && obj.getArrayNItems() >= 5 &&
            nameIs(obj.getArrayItem(0), "/DeviceN") &&
            obj.getArrayItem(4).isDictionary() &&
            nameIs(obj.getArrayItem(4).getKey("/Subtype"), "/NChannel")) {
          obj.getArrayItem(4).removeKey("/Subtype");
          ++n;
        }
      }
      if (n) note("converted " + std::to_string(n) + " NChannel space(s) to plain DeviceN");
    } else if (op == "knockoutwhite" || op == "overprintblack" ||
               op == "setoverprintandknockout") {
      if (op != "overprintblack") knockWhite = true;
      if (op != "knockoutwhite") opBlack = true;
      if (p(0) == "Text") textOnly = true;
      if (p(0) == "Vector" || p(0) == "Vector objects") vectorOnly = true;
    } else if (op == "increaselinewidth") {
      double mw = std::atof(p(0).c_str()) * unitPt(p(2));
      if (mw > 0) minWidth = std::max(minWidth, mw);
    } else if (op == "settextrendermode") {
      forceTr = std::atoi(p(0).c_str());
    } else if (op == "removeobjectsoutofbox") {
      std::string bx = p(0).empty() ? "MediaBox" : p(0);
      for (auto& ph : pages) {
        QPDFObjectHandle b = boxOnPage(ph, bx);
        if (!b.isArray() || b.getArrayNItems() != 4) continue;
        char buf[160];
        double x0 = numOf(b.getArrayItem(0), 0), y0 = numOf(b.getArrayItem(1), 0);
        std::snprintf(buf, sizeof(buf), "q %g %g %g %g re W n\n", x0, y0,
                      numOf(b.getArrayItem(2), 0) - x0, numOf(b.getArrayItem(3), 0) - y0);
        ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, std::string(buf)), true);
        ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, std::string("\nQ")), false);
      }
      note("clipped page content to the " + bx);
    } else if (op == "placetext") {
      std::string text = p(0) == "Date" ? (ctx.opt.nowOverride.empty() ? "D:converted"
                                                                        : ctx.opt.nowOverride)
                                        : p(0);
      double size = std::atof(p(2).c_str());
      if (size <= 0) size = 12;
      for (auto& ph : pages) {
        QPDFObjectHandle res = ph.getAttribute("/Resources", true);
        if (!res.isDictionary()) continue;
        QPDFObjectHandle fonts = res.getKey("/Font");
        if (!fonts.isDictionary()) {
          fonts = QPDFObjectHandle::newDictionary();
          res.replaceKey("/Font", fonts);
        }
        QPDFObjectHandle helv = QPDFObjectHandle::newDictionary();
        helv.replaceKey("/Type", QPDFObjectHandle::newName("/Font"));
        helv.replaceKey("/Subtype", QPDFObjectHandle::newName("/Type1"));
        helv.replaceKey("/BaseFont", QPDFObjectHandle::newName("/Helvetica"));
        fonts.replaceKey("/KuraStampF", ctx.pdf.makeIndirectObject(helv));
        QPDFObjectHandle mb = ph.getAttribute("/MediaBox", true);
        double x = 36, y = 36;
        if (mb.isArray() && mb.getArrayNItems() == 4) {
          x = numOf(mb.getArrayItem(0), 0) + 18;
          y = numOf(mb.getArrayItem(1), 0) + 18;
        }
        std::string esc;
        for (char c : text) {
          if (c == '(' || c == ')' || c == '\\') esc += '\\';
          esc += c;
        }
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "\nq BT /KuraStampF %g Tf %g %g Td (%s) Tj ET Q", size, x, y,
                      esc.c_str());
        ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, std::string(buf)), false);
      }
      note("placed text on pages");
    } else if (op == "annotation") {
      std::string sel = p(0), act = p(1);
      int n = 0;
      static const std::set<std::string> multimedia = {"Screen", "Movie", "Sound",
                                                       "RichMedia", "3D"};
      static const std::set<std::string> known = {
          "Text", "Link", "FreeText", "Line", "Square", "Circle", "Polygon", "PolyLine",
          "Highlight", "Underline", "Squiggly", "StrikeOut", "Stamp", "Caret", "Ink",
          "Popup", "FileAttachment", "Sound", "Movie", "Widget", "Screen", "PrinterMark",
          "TrapNet", "Watermark", "3D", "Redact", "Projection", "RichMedia"};
      for (auto& ph : pages) {
        QPDFObjectHandle page = ph.getObjectHandle();
        QPDFObjectHandle annots = page.getKey("/Annots");
        if (!annots.isArray()) continue;
        for (int i = annots.getArrayNItems() - 1; i >= 0; --i) {
          QPDFObjectHandle an = annots.getArrayItem(i);
          if (!an.isDictionary()) continue;
          std::string st = nameOf(an.getKey("/Subtype"));
          if (st.size() > 1) st = st.substr(1);
          bool match = sel == "All" || sel == st ||
                       (sel == "AllMultimedia" && multimedia.count(st)) ||
                       (sel == "Unknown" && !known.count(st));
          if (!match) continue;
          if (act == "Remove") {
            annots.eraseItem(i);
            ++n;
          } else if (act == "SetToNoPrint") {
            long long fl = an.getKey("/F").isInteger() ? an.getKey("/F").getIntValue() : 0;
            if (fl & 4) {
              an.replaceKey("/F", QPDFObjectHandle::newInteger(fl & ~4));
              ++n;
            }
          } else if (act == "MoveOutOfBleedBox") {
            QPDFObjectHandle rect = an.getKey("/Rect");
            QPDFObjectHandle mb = ph.getAttribute("/MediaBox", true);
            if (rect.isArray() && rect.getArrayNItems() == 4 && mb.isArray()) {
              double mx1 = numOf(mb.getArrayItem(2), 0);
              double w = numOf(rect.getArrayItem(2), 0) - numOf(rect.getArrayItem(0), 0);
              rect.setArrayItem(0, QPDFObjectHandle::newReal(mx1 + 36, 2));
              rect.setArrayItem(2, QPDFObjectHandle::newReal(mx1 + 36 + w, 2));
              ++n;
            }
          }
        }
      }
      if (n) note("annotation fix (" + act + "): adjusted " + std::to_string(n) +
                  " annotation(s)");
    } else if (op == "putobjectsonlayer" || op == "putobjpsteps") {
      std::string label = p(1).empty() ? (p(0).empty() ? "Layer" : p(0)) : p(1);
      QPDFObjectHandle ocg = QPDFObjectHandle::newDictionary();
      ocg.replaceKey("/Type", QPDFObjectHandle::newName("/OCG"));
      ocg.replaceKey("/Name", QPDFObjectHandle::newUnicodeString(label));
      if (op == "putobjpsteps") {
        QPDFObjectHandle md = QPDFObjectHandle::newDictionary();
        md.replaceKey("/Type", QPDFObjectHandle::newName("/GTS_ProcSteps"));
        md.replaceKey("/GTS_ProcStepsGroup", QPDFObjectHandle::newName("/" + label));
        ocg.replaceKey("/GTS_Metadata", md);
      }
      QPDFObjectHandle ocgRef = ctx.pdf.makeIndirectObject(ocg);
      QPDFObjectHandle root = ctx.pdf.getRoot();
      QPDFObjectHandle ocp = root.getKey("/OCProperties");
      if (!ocp.isDictionary()) {
        ocp = QPDFObjectHandle::newDictionary();
        ocp.replaceKey("/OCGs", QPDFObjectHandle::newArray());
        QPDFObjectHandle d = QPDFObjectHandle::newDictionary();
        d.replaceKey("/Order", QPDFObjectHandle::newArray());
        ocp.replaceKey("/D", d);
        root.replaceKey("/OCProperties", ocp);
      }
      ocp.getKey("/OCGs").appendItem(ocgRef);
      if (ocp.getKey("/D").isDictionary() && ocp.getKey("/D").getKey("/Order").isArray()) {
        ocp.getKey("/D").getKey("/Order").appendItem(ocgRef);
      }
      for (auto& ph : pages) {
        QPDFObjectHandle res = ph.getAttribute("/Resources", true);
        if (!res.isDictionary()) continue;
        QPDFObjectHandle props = res.getKey("/Properties");
        if (!props.isDictionary()) {
          props = QPDFObjectHandle::newDictionary();
          res.replaceKey("/Properties", props);
        }
        props.replaceKey("/KuraOC1", ocgRef);
        ph.addPageContents(
            QPDFObjectHandle::newStream(&ctx.pdf, std::string("/OC /KuraOC1 BDC\n")), true);
        ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, std::string("\nEMC")),
                           false);
      }
      note("placed page content on layer " + label);
    } else if (op == "dscdhdnlycntfltnvsblyrs") {
      QPDFObjectHandle root = ctx.pdf.getRoot();
      QPDFObjectHandle ocp = root.getKey("/OCProperties");
      if (ocp.isDictionary()) {
        root.removeKey("/OCProperties");
        note("flattened layers (layer switching removed; visible content kept)");
      }
    }
  }
  if (knockWhite || opBlack || minWidth > 0 || forceTr >= 0) {
    for (auto& ph : pages) {
      QPDFObjectHandle res = ph.getAttribute("/Resources", true);
      if (res.isDictionary() && (knockWhite || opBlack)) {
        QPDFObjectHandle egs = res.getKey("/ExtGState");
        if (!egs.isDictionary()) {
          egs = QPDFObjectHandle::newDictionary();
          res.replaceKey("/ExtGState", egs);
        }
        if (knockWhite) {
          QPDFObjectHandle ko = QPDFObjectHandle::newDictionary();
          ko.replaceKey("/Type", QPDFObjectHandle::newName("/ExtGState"));
          ko.replaceKey("/OP", QPDFObjectHandle::newBool(false));
          ko.replaceKey("/op", QPDFObjectHandle::newBool(false));
          egs.replaceKey("/KuraKO", ctx.pdf.makeIndirectObject(ko));
        }
        if (opBlack) {
          QPDFObjectHandle ob = QPDFObjectHandle::newDictionary();
          ob.replaceKey("/Type", QPDFObjectHandle::newName("/ExtGState"));
          ob.replaceKey("/OP", QPDFObjectHandle::newBool(true));
          ob.replaceKey("/op", QPDFObjectHandle::newBool(true));
          ob.replaceKey("/OPM", QPDFObjectHandle::newInteger(1));
          egs.replaceKey("/KuraOB", ctx.pdf.makeIndirectObject(ob));
        }
      }
      auto filter = std::make_shared<OverprintFilter>(knockWhite, opBlack, textOnly,
                                                      vectorOnly, minWidth, forceTr);
      ph.addContentTokenFilter(filter);
    }
    if (knockWhite) ctx.issue("PROFILE_FIX_DONE", "white objects set to knock out", true);
    if (opBlack) ctx.issue("PROFILE_FIX_DONE", "solid black set to overprint", true);
    if (minWidth > 0) {
      ctx.issue("PROFILE_FIX_DONE", "thin strokes raised to the minimum width", true);
    }
    if (forceTr >= 0) ctx.issue("PROFILE_FIX_DONE", "text render mode normalized", true);
  }
}
}
