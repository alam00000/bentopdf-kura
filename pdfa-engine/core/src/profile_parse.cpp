#include "profile_types.hh"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "check_registry.hh"
#include "limits.hh"

namespace pdfa {

const Json* Json::get(const std::string& key) const {
  for (const auto& kv : obj) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

std::string Json::text() const {
  if (type == kStr) return str;
  if (type == kBool) return b ? "true" : "false";
  if (type == kNum) {
    char buf[64];
    if (std::fabs(num) < 1e15 && num == std::floor(num)) {
      std::snprintf(buf, sizeof buf, "%.0f", num);
    } else {
      std::snprintf(buf, sizeof buf, "%.15g", num);
    }
    return buf;
  }
  return std::string();
}

namespace {
struct JsonReader {
  const std::string& s;
  size_t i = 0;
  int depth = 0;
  std::string err;

  explicit JsonReader(const std::string& text) : s(text) {}

  bool fail(const std::string& what) {
    if (err.empty()) err = what + " at byte " + std::to_string(i);
    return false;
  }

  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }

  bool value(Json& v) {
    ws();
    if (i >= s.size()) return fail("unexpected end of JSON");
    if (depth >= kMaxObjectWalk) return fail("JSON nested too deeply");
    char c = s[i];
    if (c == '{') return object(v);
    if (c == '[') return array(v);
    if (c == '"') {
      v.type = Json::kStr;
      return string(v.str);
    }
    if (s.compare(i, 4, "true") == 0) {
      v.type = Json::kBool;
      v.b = true;
      i += 4;
      return true;
    }
    if (s.compare(i, 5, "false") == 0) {
      v.type = Json::kBool;
      i += 5;
      return true;
    }
    if (s.compare(i, 4, "null") == 0) {
      v.type = Json::kNull;
      i += 4;
      return true;
    }
    return number(v);
  }

  bool object(Json& v) {
    v.type = Json::kObj;
    ++depth;
    ++i;
    ws();
    if (i < s.size() && s[i] == '}') {
      ++i;
      --depth;
      return true;
    }
    while (true) {
      ws();
      std::string key;
      if (i >= s.size() || s[i] != '"') return fail("expected a quoted key");
      if (!string(key)) return false;
      ws();
      if (i >= s.size() || s[i] != ':') return fail("expected ':'");
      ++i;
      Json child;
      if (!value(child)) return false;
      v.obj.emplace_back(key, std::move(child));
      ws();
      if (i < s.size() && s[i] == ',') {
        ++i;
        continue;
      }
      if (i < s.size() && s[i] == '}') {
        ++i;
        --depth;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  bool array(Json& v) {
    v.type = Json::kArr;
    ++depth;
    ++i;
    ws();
    if (i < s.size() && s[i] == ']') {
      ++i;
      --depth;
      return true;
    }
    while (true) {
      Json child;
      if (!value(child)) return false;
      v.arr.push_back(std::move(child));
      ws();
      if (i < s.size() && s[i] == ',') {
        ++i;
        continue;
      }
      if (i < s.size() && s[i] == ']') {
        ++i;
        --depth;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  static void utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool hex4(unsigned& cp) {
    if (i + 4 > s.size()) return fail("bad \\u escape");
    cp = 0;
    for (int k = 0; k < 4; ++k) {
      char h = s[i++];
      cp <<= 4;
      if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
      else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
      else return fail("bad \\u escape");
    }
    return true;
  }

  bool string(std::string& out) {
    ++i;
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= s.size()) break;
      char e = s[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && s.compare(i, 2, "\\u") == 0) {
            i += 2;
            unsigned lo = 0;
            if (!hex4(lo)) return false;
            if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            else cp = 0xFFFD;
          } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = 0xFFFD;
          }
          utf8(out, cp);
          break;
        }
        default:
          return fail("bad escape in string");
      }
    }
    if (i >= s.size()) return fail("unterminated string");
    ++i;
    return true;
  }

  bool number(Json& v) {
    size_t start = i;
    if (i < s.size() && s[i] == '-') ++i;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                            s[i] == '+' || s[i] == '-')) {
      ++i;
    }
    std::string t = s.substr(start, i - start);
    char* end = nullptr;
    double d = t.empty() ? 0 : std::strtod(t.c_str(), &end);
    if (t.empty() || !end || *end || !std::isfinite(d)) {
      i = start;
      return fail("unexpected character");
    }
    v.type = Json::kNum;
    v.num = d;
    return true;
  }
};

bool scalarList(const Json& v, std::vector<std::string>& out) {
  if (v.type == Json::kArr) {
    for (const Json& x : v.arr) {
      if (x.type != Json::kStr && x.type != Json::kNum && x.type != Json::kBool) return false;
      out.push_back(x.text());
    }
    return true;
  }
  if (v.type != Json::kStr && v.type != Json::kNum && v.type != Json::kBool) return false;
  out.push_back(v.text());
  return true;
}

std::string joinList(const std::vector<std::string>& v) {
  std::string out;
  for (size_t k = 0; k < v.size(); ++k) out += (k ? "\n" : "") + v[k];
  return out;
}

bool hitsRule(const std::string& s) {
  size_t k = 0;
  while (k < s.size() && s[k] == ' ') ++k;
  size_t opEnd = k;
  while (opEnd < s.size() && (s[opEnd] == '<' || s[opEnd] == '>' || s[opEnd] == '=' || s[opEnd] == '!')) ++opEnd;
  std::string op = s.substr(k, opEnd - k);
  if (op != "<" && op != "<=" && op != "==" && op != "!=" && op != ">=" && op != ">") return false;
  std::string rest = s.substr(opEnd);
  char* end = nullptr;
  std::strtod(rest.c_str(), &end);
  if (end == rest.c_str()) return false;
  while (*end == ' ') ++end;
  return *end == 0;
}

struct Parser {
  PfProfile& out;
  std::string err;

  bool fail(const std::string& e) {
    if (err.empty()) err = e;
    return false;
  }

  bool atom(const Json& a, const std::string& where, PfAtom& atom) {
    if (a.type != Json::kObj) return fail(where + ": expected an object");
    const Json* tol = nullptr;
    for (const auto& [key, v] : a.obj) {
      std::string at = where + "." + key;
      if (key == "check") {
        if (v.type != Json::kStr) return fail(at + ": expected a check id");
        atom.token = v.str;
      } else if (key == "op") {
        if (v.type != Json::kStr) return fail(at + ": expected an operator");
        atom.op = v.str;
      } else if (key == "compare") {
        if (v.type != Json::kStr) return fail(at + ": expected an operator");
        atom.countOp = v.str;
      } else if (key == "value") {
        if (!scalarList(v, atom.vals)) return fail(at + ": expected a value or a list of values");
      } else if (key == "negate") {
        if (v.type != Json::kBool) return fail(at + ": expected true or false");
        atom.negate = v.b;
      } else if (key == "case_sensitive") {
        if (v.type != Json::kBool) return fail(at + ": expected true or false");
        atom.icase = !v.b;
      } else if (key == "tolerance") {
        if (v.type != Json::kNum && v.type != Json::kStr) return fail(at + ": expected a number");
        tol = &v;
      } else if (key == "objects") {
        if (v.type != Json::kArr) return fail(at + ": expected a list of check names");
        for (const Json& r : v.arr) {
          if (r.type != Json::kStr) return fail(at + ": expected a list of check names");
          atom.refs.push_back(r.str);
        }
      } else {
        std::vector<std::string> items;
        if (!scalarList(v, items)) return fail(at + ": expected a value or a list of values");
        atom.params[key] = joinList(items);
      }
    }
    if (atom.token.empty()) return fail(where + ".check: missing");
    int ci = checkIndex(atom.token);
    if (tol && ci >= 0 && findParam(checkDef(ci).params, checkDef(ci).paramCount, "tolerance")) {
      atom.params["tolerance"] = tol->text();
      tol = nullptr;
    }
    std::string e = checkAtom(atom, where);
    if (!e.empty()) return fail(e);
    if (tol) {
      std::string type = checkDef(atom.check).value;
      bool ok = true;
      double t = parseQuantity(tol->text(), type == "size" ? "length" : type,
                               param(atom, "unit").empty() ? "pt" : param(atom, "unit"), &ok);
      if (!ok) return fail(where + ".tolerance: " + tol->text() + " is not a " + type);
      atom.tol = std::fabs(t);
    }
    return true;
  }

  bool condition(const Json* list, const std::string& where, PfCondition& cond) {
    if (!list || list->type != Json::kArr || list->arr.empty()) {
      return fail(where + ": expected a non-empty list of conditions");
    }
    for (size_t k = 0; k < list->arr.size(); ++k) {
      PfAtom a;
      if (!atom(list->arr[k], where + "[" + std::to_string(k) + "]", a)) return false;
      cond.atoms.push_back(std::move(a));
    }
    return true;
  }

  bool check(const Json& c, size_t index, std::set<std::string>& names) {
    std::string where = "checks[" + std::to_string(index) + "]";
    if (c.type != Json::kObj) return fail(where + ": expected an object");
    PfRule rule;
    rule.id = "K" + std::to_string(index + 1);
    static const std::set<std::string> known = {"name", "severity", "description", "all", "any", "scope",
                                                "report_when", "max_hits_per_page", "silent"};
    for (const auto& kv : c.obj) {
      if (!known.count(kv.first)) return fail(where + "." + kv.first + ": unknown field");
    }
    const Json* name = c.get("name");
    if (!name || name->type != Json::kStr || name->str.empty()) return fail(where + ".name: missing");
    rule.name = name->str;
    if (!names.insert(rule.name).second) return fail(where + ".name: duplicate check name \"" + rule.name + "\"");
    where = "check \"" + rule.name + "\"";
    if (const Json* sv = c.get("severity")) {
      std::string s = sv->type == Json::kStr ? sv->str : std::string();
      if (s != "error" && s != "warning" && s != "info") return fail(where + ".severity: expected error, warning or info");
      rule.severity = s == "error" ? 3 : (s == "warning" ? 2 : 1);
    }
    if (const Json* d = c.get("description")) {
      if (d->type != Json::kStr) return fail(where + ".description: expected text");
      rule.description = d->str;
    }
    if (const Json* sc = c.get("scope")) {
      std::string s = sc->type == Json::kStr ? sc->str : std::string();
      if (s != "page" && s != "trim" && s != "bleed") return fail(where + ".scope: expected page, trim or bleed");
      rule.scope = s == "trim" ? 2 : (s == "bleed" ? 3 : 0);
    }
    if (const Json* rw = c.get("report_when")) {
      if (rw->type != Json::kObj) return fail(where + ".report_when: expected an object");
      for (const auto& [k, v] : rw->obj) {
        if (k != "document_hits" && k != "page_hits") return fail(where + ".report_when." + k + ": unknown field");
        if (v.type != Json::kStr || !hitsRule(v.str)) {
          return fail(where + ".report_when." + k + ": expected a comparison such as \"> 3\"");
        }
        (k == "document_hits" ? rule.reportDocHits : rule.reportPageHits) = v.str;
      }
    }
    if (const Json* m = c.get("max_hits_per_page")) {
      if (m->type != Json::kNum || m->num < 0 || m->num != std::floor(m->num)) {
        return fail(where + ".max_hits_per_page: expected a whole number");
      }
      rule.maxHitsPerPage = static_cast<long long>(m->num);
    }
    if (const Json* s = c.get("silent")) {
      if (s->type != Json::kBool) return fail(where + ".silent: expected true or false");
      rule.silent = s->b;
    }
    const Json* all = c.get("all");
    const Json* any = c.get("any");
    if ((all != nullptr) == (any != nullptr)) return fail(where + ": needs either all or any");
    std::vector<std::pair<const Json*, std::string>> groups;
    if (any) {
      if (any->type != Json::kArr || any->arr.empty()) return fail(where + ".any: expected a non-empty list");
      rule.logic = 1;
      for (size_t g = 0; g < any->arr.size(); ++g) {
        const Json& alt = any->arr[g];
        std::string at = where + ".any[" + std::to_string(g) + "]";
        if (alt.type != Json::kObj || alt.obj.size() != 1 || !alt.get("all")) return fail(at + ": expected {\"all\": [...]}");
        groups.push_back({alt.get("all"), at + ".all"});
      }
    } else {
      groups.push_back({all, where + ".all"});
    }
    for (size_t g = 0; g < groups.size(); ++g) {
      PfCondition cond;
      if (!condition(groups[g].first, groups[g].second, cond)) return false;
      std::string cid = "C" + rule.id + "_" + std::to_string(g);
      out.conds[cid] = std::move(cond);
      rule.condIds.push_back(cid);
    }
    out.ruleIndex[rule.id] = rule;
    out.ruleIndex[rule.name] = rule;
    out.rules.push_back(rule);
    return true;
  }

  bool fix(const Json& f, size_t index) {
    std::string where = "fixes[" + std::to_string(index) + "]";
    if (f.type != Json::kObj) return fail(where + ": expected an object");
    PfFix fx;
    for (const auto& [key, v] : f.obj) {
      std::string at = where + "." + key;
      if (key == "fix") {
        if (v.type != Json::kStr || v.str.empty()) return fail(at + ": expected a fix id");
        fx.id = v.str;
      } else if (key == "only_where") {
        std::vector<std::string> names;
        if (v.type != Json::kArr || !scalarList(v, names)) return fail(at + ": expected a list of check names");
        for (const std::string& n : names) {
          auto it = out.ruleIndex.find(n);
          if (it == out.ruleIndex.end() || it->second.name != n) return fail(at + ": no check named \"" + n + "\"");
          fx.onlyWhere.push_back(n);
        }
      } else {
        std::vector<std::string> items;
        if (!scalarList(v, items)) return fail(at + ": expected a value or a list of values");
        fx.args[key] = joinList(items);
      }
    }
    if (fx.id.empty()) return fail(where + ".fix: missing");
    std::string e = checkFix(fx, where);
    if (!e.empty()) return fail(e);
    out.fixes.push_back(std::move(fx));
    return true;
  }

  bool profile(const Json& root) {
    if (root.type != Json::kObj) return fail("profile: expected a JSON object");
    const Json* ver = root.get("kura-profile");
    if (!ver) return fail("kura-profile: missing");
    if (ver->type != Json::kNum || ver->num != 1) return fail("kura-profile: expected 1");
    static const std::set<std::string> known = {"kura-profile", "name", "description", "variables", "shapes",
                                                "checks", "fixes", "convert", "plan"};
    for (const auto& kv : root.obj) {
      if (!known.count(kv.first)) return fail(kv.first + ": unknown field");
    }
    if (const Json* n = root.get("name")) {
      if (n->type != Json::kStr) return fail("name: expected text");
      out.name = n->str;
    }
    if (const Json* d = root.get("description")) {
      if (d->type != Json::kStr) return fail("description: expected text");
      out.description = d->str;
    }
    std::set<std::string> names;
    if (const Json* checks = root.get("checks")) {
      if (checks->type != Json::kArr) return fail("checks: expected a list");
      for (size_t k = 0; k < checks->arr.size(); ++k) {
        if (!check(checks->arr[k], k, names)) return false;
      }
    }
    if (const Json* shapes = root.get("shapes")) {
      if (shapes->type != Json::kObj) return fail("shapes: expected an object");
      for (const auto& [name, d] : shapes->obj) {
        std::string at = "shapes." + name;
        if (d.type != Json::kObj) return fail(at + ": expected an object");
        PfShape sh;
        for (const auto& [key, v] : d.obj) {
          if (key == "box") {
            static const std::set<std::string> boxes = {"media", "crop", "bleed", "trim", "art"};
            if (!boxes.count(v.text())) return fail(at + ".box: \"" + v.text() + "\" is not one of media,crop,bleed,trim,art");
            sh.box = v.text();
          } else if (key == "check") {
            if (!names.count(v.text())) return fail(at + ".check: no check named \"" + v.text() + "\"");
            sh.check = v.text();
          } else if (key == "offset") {
            bool ok = true;
            sh.offset = lengthToPt(v.text(), "pt", &ok);
            if (!ok) return fail(at + ".offset: " + v.text() + " is not a length");
          } else {
            return fail(at + "." + key + ": unknown field");
          }
        }
        if (sh.box.empty() == sh.check.empty()) return fail(at + ": give either box or check");
        out.shapes[name] = sh;
      }
    }
    for (const auto& [cid, cond] : out.conds) {
      for (const PfAtom& a : cond.atoms) {
        if (a.token != "content.shape_relation") continue;
        const std::string& sh = param(a, "shape");
        static const std::set<std::string> boxes = {"media", "crop", "bleed", "trim", "art"};
        if (sh.find('{') == std::string::npos && !boxes.count(sh) && !out.shapes.count(sh) && !names.count(sh)) {
          return fail("content.shape_relation.shape: \"" + sh + "\" is not a page box, a shape or a check name");
        }
      }
    }
    if (const Json* fixes = root.get("fixes")) {
      if (fixes->type != Json::kArr) return fail("fixes: expected a list");
      for (size_t k = 0; k < fixes->arr.size(); ++k) {
        if (!fix(fixes->arr[k], k)) return false;
      }
    }
    return true;
  }
};
}

bool parseJson(const std::string& text, Json& out, std::string& error) {
  JsonReader r(text);
  out = Json();
  if (!r.value(out)) {
    error = r.err;
    return false;
  }
  r.ws();
  if (r.i != text.size()) {
    r.fail("unexpected text after the JSON value");
    error = r.err;
    return false;
  }
  return true;
}

bool parseKuraJson(const std::string& text, PfProfile& out) {
  out = PfProfile();
  Json root;
  std::string err;
  if (!parseJson(text, root, err)) {
    out.error = "not valid JSON: " + err;
    return false;
  }
  Parser p{out, std::string()};
  if (!p.profile(root)) {
    std::string e = p.err;
    out = PfProfile();
    out.error = e;
    return false;
  }
  return true;
}

}
