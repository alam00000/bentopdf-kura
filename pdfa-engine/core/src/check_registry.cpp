#include "check_registry.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "profile_eval.hh"

namespace pdfa {
namespace {
#include "registry_data.inc"

template <typename T, size_t N>
constexpr int countOf(const T (&)[N]) {
  return static_cast<int>(N);
}

std::vector<std::string> splitOn(const std::string& s, char sep) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  size_t b = 0;
  while (true) {
    size_t e = s.find(sep, b);
    out.push_back(s.substr(b, e == std::string::npos ? std::string::npos : e - b));
    if (e == std::string::npos) break;
    b = e + 1;
  }
  return out;
}

bool listed(const char* bar, const std::string& v) {
  for (const std::string& x : splitOn(bar ? bar : "", '|')) {
    if (x == v) return true;
  }
  return false;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

bool readNumber(const std::string& s, double& v, std::string& rest) {
  std::string t = trim(s);
  const char* p = t.c_str();
  char* end = nullptr;
  v = std::strtod(p, &end);
  if (end == p) return false;
  rest = lower(trim(std::string(end)));
  return std::isfinite(v);
}

double unitFactor(const std::string& u) {
  if (u == "mm") return 72.0 / 25.4;
  if (u == "cm") return 72.0 / 2.54;
  if (u == "in") return 72.0;
  return 1.0;
}

double byteFactor(const std::string& u) {
  if (u == "kb") return 1024.0;
  if (u == "mb") return 1024.0 * 1024.0;
  return 1.0;
}

const std::string kEmpty;

std::string fmtNumber(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.6g", v);
  return buf;
}

const ParamDef* paramsOf(const PfAtom& a, int& n) {
  n = 0;
  if (a.check < 0) return nullptr;
  n = kChecks[a.check].paramCount;
  return kChecks[a.check].params;
}

std::string quote(const std::string& s) { return "\"" + s + "\""; }

std::string checkValue(const ParamDef& p, const std::string& v) {
  std::string t = p.type;
  if (t == "enum") {
    if (!listed(p.enums, v)) {
      std::string all = p.enums;
      std::replace(all.begin(), all.end(), '|', ',');
      return quote(v) + " is not one of " + all;
    }
    return std::string();
  }
  if (t == "bool") return v == "true" || v == "false" ? std::string() : "expects true or false";
  if (t == "text" || t == "list" || t == "date") return std::string();
  bool ok = true;
  parseQuantity(v, t, t == "length" ? "pt" : std::string(), &ok);
  return ok ? std::string() : quote(v) + " is not a " + t;
}

std::string fillParams(const ParamDef* defs, int n, std::map<std::string, std::string>& params,
                       const std::string& owner, const std::string& where) {
  for (const auto& kv : params) {
    const ParamDef* p = findParam(defs, n, kv.first);
    if (!p) return where + "." + kv.first + ": unknown parameter for " + owner;
    if (std::string(p->type) == "list") continue;
    std::string err = checkValue(*p, kv.second);
    if (!err.empty()) return where + "." + kv.first + ": " + err;
  }
  for (int i = 0; i < n; ++i) {
    const ParamDef& p = defs[i];
    if (params.count(p.name)) continue;
    if (p.required) return where + "." + p.name + ": required by " + owner;
    params[p.name] = p.hasDefault ? p.defaultValue : "";
  }
  return std::string();
}

bool numericOp(const std::string& op) {
  return op == "<" || op == "<=" || op == "==" || op == "!=" || op == ">=" || op == ">" ||
         op == "multiple_of";
}

bool numericType(const std::string& t) {
  return t == "number" || t == "count" || t == "length" || t == "percent" || t == "angle" ||
         t == "resolution" || t == "ratio" || t == "bytes" || t == "version";
}

bool cmpNumbers(double l, const std::string& op, double r, double tol) {
  bool eq = std::fabs(l - r) < 1e-6 || std::fabs(l - r) <= tol;
  if (op == "<") return l < r - tol;
  if (op == "<=") return l <= r + tol;
  if (op == ">") return l > r + tol;
  if (op == ">=") return l >= r - tol;
  if (op == "==" || op == "in") return eq;
  if (op == "!=") return !eq;
  if (op == "multiple_of") {
    if (r == 0 || std::fabs(l) < std::fabs(r)) return false;
    double f = std::fabs(std::fmod(l / r, 1.0));
    return std::min(f, 1.0 - f) <= std::max(tol, 2e-4);
  }
  return false;
}

const std::regex* regexFor(const std::string& pattern, bool icase) {
  thread_local std::map<std::pair<std::string, bool>, std::unique_ptr<std::regex>> cache;
  auto key = std::make_pair(pattern, icase);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second.get();
  std::unique_ptr<std::regex> re;
  try {
    re = std::make_unique<std::regex>(pattern, icase ? std::regex::ECMAScript | std::regex::icase
                                                     : std::regex::ECMAScript);
  } catch (const std::regex_error&) {
  }
  const std::regex* out = re.get();
  cache.emplace(key, std::move(re));
  return out;
}

bool textOp(const std::string& raw, const std::string& op, const PfAtom& a) {
  std::string v = a.icase ? lower(raw) : raw;
  auto norm = [&](const std::string& x) { return a.icase ? lower(x) : x; };
  if (op == "matches" || op == "finds") {
    for (const std::string& x : a.vals) {
      const std::regex* re = regexFor(x, a.icase);
      if (!re) continue;
      if (op == "matches" ? std::regex_match(raw, *re) : std::regex_search(raw, *re)) return true;
    }
    return false;
  }
  for (const std::string& rx : a.vals) {
    std::string x = norm(rx);
    if (op == "==" || op == "in") {
      if (v == x) return true;
    } else if (op == "!=") {
      return v != x;
    } else if (op == "contains") {
      if (v.find(x) != std::string::npos) return true;
    } else if (op == "starts_with") {
      if (v.compare(0, x.size(), x) == 0) return true;
    } else if (op == "ends_with") {
      if (v.size() >= x.size() && v.compare(v.size() - x.size(), x.size(), x) == 0) return true;
    } else if (op == "occurs_in") {
      if (x.find(v) != std::string::npos) return true;
    } else if (op == "<") {
      return v < x;
    } else if (op == "<=") {
      return v <= x;
    } else if (op == ">") {
      return v > x;
    } else if (op == ">=") {
      return v >= x;
    }
  }
  return false;
}

bool sizeOp(double w, double h, const PfAtom& a) {
  double tw = 0, th = 0;
  const std::string& pw = param(a, "width");
  const std::string& ph = param(a, "height");
  if (!pw.empty() && !ph.empty()) {
    tw = lengthToPt(pw, "pt");
    th = lengthToPt(ph, "pt");
  } else if (a.nums.size() >= 2) {
    tw = a.nums[0];
    th = a.nums[1];
  } else {
    return false;
  }
  const std::string& pt = param(a, "tolerance");
  double tol = pt.empty() ? a.tol : lengthToPt(pt, "pt");
  bool either = paramBool(a, "either_orientation");
  auto test = [&](double x, double y) {
    bool eq = std::fabs(x - tw) <= tol + 1e-6 && std::fabs(y - th) <= tol + 1e-6;
    bool fits = x <= tw + tol && y <= th + tol;
    if (a.op == "==") return eq;
    if (a.op == "!=") return !eq;
    if (a.op == "fits_within") return fits;
    if (a.op == "exceeds") return !fits;
    return false;
  };
  bool r = test(w, h);
  if (either && a.op != "!=" && a.op != "exceeds") r = r || test(h, w);
  if (either && (a.op == "!=" || a.op == "exceeds")) r = r && test(h, w);
  return r;
}

bool scalarMatch(const Value& v, const PfAtom& a) {
  const std::string& op = a.op;
  if (op == "is") {
    bool want = a.vals.empty() || a.vals[0] != "false";
    bool have = v.kind == Value::kBool ? v.b : v.kind == Value::kNum ? v.num != 0 : !v.text.empty();
    return have == want;
  }
  if (v.kind == Value::kSize) return sizeOp(v.num, v.h, a);
  if (v.kind == Value::kNum) {
    if (op == "in" || op == "not_in") {
      bool any = false;
      for (double n : a.nums) any = any || cmpNumbers(v.num, "==", n, a.tol);
      return op == "in" ? any : !any;
    }
    if (numericOp(op) && !a.nums.empty()) return cmpNumbers(v.num, op, a.nums[0], a.tol);
    return textOp(fmtNumber(v.num), op, a);
  }
  std::string t = v.kind == Value::kBool ? (v.b ? "true" : "false") : v.text;
  if (op == "not_in") return !textOp(t, "in", a);
  if (numericOp(op) && a.check >= 0 && numericType(kChecks[a.check].value) && !a.nums.empty()) {
    double n = 0;
    std::string rest;
    if (!readNumber(t, n, rest)) return op == "!=";
    std::string vt = kChecks[a.check].value;
    if (vt == "length") n = lengthToPt(t, param(a, "unit").empty() ? "pt" : param(a, "unit"));
    return cmpNumbers(n, op, a.nums[0], a.tol);
  }
  return textOp(t, op, a);
}
}

int checkIndex(const std::string& id) {
  static const std::unordered_map<std::string, int> idx = [] {
    std::unordered_map<std::string, int> m;
    for (int i = 0; i < countOf(kChecks); ++i) m[kChecks[i].id] = i;
    return m;
  }();
  auto it = idx.find(id);
  return it == idx.end() ? -1 : it->second;
}

int fixIndex(const std::string& id) {
  static const std::unordered_map<std::string, int> idx = [] {
    std::unordered_map<std::string, int> m;
    for (int i = 0; i < countOf(kFixes); ++i) m[kFixes[i].id] = i;
    return m;
  }();
  auto it = idx.find(id);
  return it == idx.end() ? -1 : it->second;
}

const CheckDef& checkDef(int index) { return kChecks[index]; }

const ParamDef* findParam(const ParamDef* params, int count, const std::string& name) {
  for (int i = 0; i < count; ++i) {
    if (name == params[i].name) return &params[i];
  }
  return nullptr;
}

double lengthToPt(const std::string& text, const std::string& defaultUnit, bool* ok) {
  double v = 0;
  std::string rest;
  bool good = readNumber(text, v, rest);
  if (good && rest.empty()) rest = lower(defaultUnit);
  good = good && (rest == "pt" || rest == "mm" || rest == "cm" || rest == "in" || rest == "percent" ||
                  rest == "%" || rest.empty());
  if (ok) *ok = good;
  return good ? v * unitFactor(rest) : 0;
}

double parseQuantity(const std::string& text, const std::string& type, const std::string& unit, bool* ok) {
  if (type == "length") return lengthToPt(text, unit, ok);
  double v = 0;
  std::string rest;
  bool good = readNumber(text, v, rest);
  if (good && type == "bytes") {
    std::string u = rest.empty() ? lower(unit) : rest;
    good = u.empty() || u == "bytes" || u == "b" || u == "kb" || u == "mb";
    v *= byteFactor(u);
  } else if (good && type == "percent") {
    good = rest.empty() || rest == "%";
  } else if (good && type == "resolution") {
    good = rest.empty() || rest == "ppi" || rest == "dpi";
  } else if (good && type == "angle") {
    good = rest.empty() || rest == "deg" || rest == "°";
  } else if (good) {
    good = rest.empty();
  }
  if (ok) *ok = good;
  return good ? v : 0;
}

bool paperSize(const std::string& name, std::vector<double>& out) {
  static const std::map<std::string, std::pair<double, double>> inch = {
      {"letter", {8.5, 11}}, {"legal", {8.5, 14}}, {"tabloid", {11, 17}}, {"ledger", {11, 17}}};
  auto in = inch.find(name);
  if (in != inch.end()) {
    out = {in->second.first * 72, in->second.second * 72};
    return true;
  }
  if (name.size() < 2 || (name[0] != 'a' && name[0] != 'b' && name[0] != 'c')) return false;
  for (size_t k = 1; k < name.size(); ++k) {
    if (!std::isdigit(static_cast<unsigned char>(name[k]))) return false;
  }
  int n = std::atoi(name.c_str() + 1);
  if (n > 10) return false;
  double w = name[0] == 'a' ? 841 : name[0] == 'b' ? 1000 : 917, h = name[0] == 'a' ? 1189 : name[0] == 'b' ? 1414 : 1297;
  for (int i = 0; i < n; ++i) {
    double nh = w;
    w = std::floor(h / 2);
    h = nh;
  }
  out = {w * 72 / 25.4, h * 72 / 25.4};
  return true;
}

std::string checkAtom(PfAtom& a, const std::string& where) {
  a.check = checkIndex(a.token);
  if (a.check < 0) return where + ".check: unknown check " + quote(a.token);
  const CheckDef& d = kChecks[a.check];
  std::string vt = d.value;
  if (a.op.empty() && vt == "bool") a.op = "is";
  if (a.op.empty()) return where + ".op: missing for " + d.id;
  if (!listed(d.operators, a.op)) {
    std::string all = d.operators;
    std::replace(all.begin(), all.end(), '|', ',');
    return where + ".op: " + quote(a.op) + " is not allowed for " + d.id + " (use " + all + ")";
  }
  std::string err = fillParams(d.params, d.paramCount, a.params, d.id, where);
  if (!err.empty()) return err;
  if (a.op == "is") {
    if (a.vals.empty()) a.vals.push_back("true");
    if (a.vals.size() != 1 || (a.vals[0] != "true" && a.vals[0] != "false")) {
      return where + ".value: operator is expects true or false";
    }
    return std::string();
  }
  if (a.op == "count") {
    if (!numericOp(a.countOp) || a.countOp == "multiple_of") {
      return where + ".compare: count needs one of <, <=, ==, !=, >=, >";
    }
  } else if (!a.countOp.empty()) {
    return where + ".compare: only used with operator count";
  }
  if (vt == "size" && a.op != "count" && a.vals.empty() && (param(a, "width").empty() || param(a, "height").empty())) {
    return where + ".value: missing (give a size such as \"A4\" or \"210 x 297 mm\", or width and height)";
  }
  if (a.vals.empty() && vt != "size") {
    return where + ".value: missing";
  }
  bool multi = a.op == "in" || a.op == "not_in" || a.op == "includes" || a.op == "excludes";
  if (!multi && a.vals.size() > 1) {
    return where + ".value: operator " + a.op + " takes a single value";
  }
  const std::string& unit = param(a, "unit");
  std::string qtype = a.op == "count" ? "count" : vt;
  a.nums.clear();
  if (qtype == "size") {
    if (!a.vals.empty()) {
      std::string s = lower(a.vals[0]);
      size_t x = s.find('x');
      bool ok1 = false, ok2 = false;
      if (x != std::string::npos) {
        std::string ws = s.substr(0, x), hs = s.substr(x + 1);
        std::string rest;
        double hv = 0;
        readNumber(hs, hv, rest);
        std::string u = rest.empty() ? "pt" : rest;
        a.nums.push_back(lengthToPt(ws, u, &ok1));
        a.nums.push_back(lengthToPt(hs, u, &ok2));
      }
      if (x == std::string::npos && paperSize(s, a.nums)) ok1 = ok2 = true;
      if (!ok1 || !ok2) return where + ".value: expects a paper name or width x height, e.g. \"A4\" or \"210 x 297 mm\"";
    }
  } else if (numericType(qtype) && (numericOp(a.op) || a.op == "in" || a.op == "not_in" || a.op == "count")) {
    for (const std::string& v : a.vals) {
      bool ok = true;
      double n = parseQuantity(v, qtype, qtype == "length" && unit.empty() ? "pt" : unit, &ok);
      if (!ok) return where + ".value: " + quote(v) + " is not a " + qtype;
      a.nums.push_back(n);
    }
  }
  if (a.op == "matches" || a.op == "finds") {
    for (const std::string& v : a.vals) {
      if (!regexFor(v, a.icase)) return where + ".value: invalid regular expression " + quote(v);
    }
  }
  return std::string();
}

std::string checkFix(PfFix& f, const std::string& where) {
  f.fix = fixIndex(f.id);
  if (f.fix < 0) return std::string();
  const FixDef& d = kFixes[f.fix];
  return fillParams(d.params, d.paramCount, f.args, d.id, where);
}

const std::string& param(const PfAtom& a, const std::string& name) {
  auto it = a.params.find(name);
  return it == a.params.end() ? kEmpty : it->second;
}

double paramNum(const PfAtom& a, const std::string& name) {
  int n = 0;
  const ParamDef* defs = paramsOf(a, n);
  const ParamDef* p = findParam(defs, n, name);
  const std::string& v = param(a, name);
  if (!p || v.empty()) return std::atof(v.c_str());
  return parseQuantity(v, p->type, std::string(p->type) == "length" ? "pt" : param(a, "unit"));
}

bool paramBool(const PfAtom& a, const std::string& name) { return param(a, name) == "true"; }

std::vector<std::string> paramList(const PfAtom& a, const std::string& name) {
  return splitOn(param(a, name), '\n');
}

const std::string& fixParam(const PfFix& f, const std::string& name) {
  auto it = f.args.find(name);
  return it == f.args.end() ? kEmpty : it->second;
}

double fixParamNum(const PfFix& f, const std::string& name) {
  const std::string& v = fixParam(f, name);
  const ParamDef* p = f.fix < 0 ? nullptr : findParam(kFixes[f.fix].params, kFixes[f.fix].paramCount, name);
  if (!p || v.empty()) return std::atof(v.c_str());
  return parseQuantity(v, p->type, std::string(p->type) == "length" ? "pt" : fixParam(f, "unit"));
}

bool fixParamBool(const PfFix& f, const std::string& name) { return fixParam(f, name) == "true"; }

std::vector<std::string> fixParamList(const PfFix& f, const std::string& name) {
  return splitOn(fixParam(f, name), '\n');
}

int compare(const Value& v, const PfAtom& a) {
  if (v.kind == Value::kNone) return 0;
  const std::string& op = a.op;
  if (op == "count") {
    double n = v.kind == Value::kList ? static_cast<double>(v.list.size()) : v.num;
    bool r = !a.nums.empty() && cmpNumbers(n, a.countOp, a.nums[0], a.tol);
    return r != a.negate ? 1 : 0;
  }
  if (v.kind == Value::kList) {
    auto eq = [&](const std::string& x, const std::string& y) {
      return a.icase ? lower(x) == lower(y) : x == y;
    };
    if (op == "includes" || op == "excludes") {
      int present = 0;
      bool all = true, none = true;
      for (const std::string& want : a.vals) {
        bool found = false;
        for (const std::string& e : v.list) found = found || eq(e, want);
        all = all && found;
        none = none && !found;
      }
      for (const std::string& e : v.list) {
        for (const std::string& want : a.vals) {
          if (eq(e, want)) {
            ++present;
            break;
          }
        }
      }
      bool r = op == "includes" ? all : none;
      if (r == a.negate) return 0;
      return op == "includes" && !a.negate ? std::max(present, 1) : 1;
    }
    int hits = 0;
    for (const std::string& e : v.list) {
      if (scalarMatch(Value(e), a) != a.negate) ++hits;
    }
    return hits;
  }
  return scalarMatch(v, a) != a.negate ? 1 : 0;
}

void CheckTable::add(const char* id, CheckFn fn) {
  int i = checkIndex(id ? id : "");
  if (i < 0) throw std::logic_error(std::string("evaluator for unknown check ") + (id ? id : ""));
  if (fns_.empty()) fns_.assign(static_cast<size_t>(countOf(kChecks)), nullptr);
  if (fns_[static_cast<size_t>(i)]) throw std::logic_error(std::string("two evaluators for check ") + id);
  fns_[static_cast<size_t>(i)] = fn;
}

CheckFn CheckTable::find(int check) const {
  if (check < 0 || static_cast<size_t>(check) >= fns_.size()) return nullptr;
  return fns_[static_cast<size_t>(check)];
}

const CheckTable& checkTable() {
  static const CheckTable table = [] {
    CheckTable t;
    registerColorChecks(t);
    registerObjectChecks(t);
    registerExtChecks(t);
    registerSpatialChecks(t);
    return t;
  }();
  return table;
}


Domain domainOf(const PfAtom& a) {
  if (a.check < 0) return Domain::kNone;
  const CheckDef& d = kChecks[a.check];
  if (param(a, "subject") == "annotation") return Domain::kAnnot;
  if (d.domain != Domain::kDoc && param(a, "scope") == "document") return Domain::kDoc;
  if (d.domain == Domain::kDoc && param(a, "scope") == "page") {
    const ParamDef* p = findParam(d.params, d.paramCount, "scope");
    std::string e = p ? p->enums : "";
    if (e == "document|page" || e == "page|document") return Domain::kPage;
  }
  const ParamDef* lv = d.domain == Domain::kDoc ? findParam(d.params, d.paramCount, "level") : nullptr;
  if (lv && std::string(lv->enums) == "document|page|object|annotation") {
    const std::string& l = param(a, "level");
    if (l == "page") return Domain::kPage;
    if (l == "object") return Domain::kAny;
    if (l == "annotation") return Domain::kAnnot;
  }
  return d.domain;
}

Subject subjectFor(Domain kind, const void* object, int page, const Events& ev) {
  Subject s;
  s.kind = kind;
  s.page = page;
  switch (kind) {
    case Domain::kPaint:
      s.paint = static_cast<const PaintEvent*>(object);
      break;
    case Domain::kText:
      s.text = static_cast<const TextEvent*>(object);
      if (s.text) {
        for (const FontFacts& f : ev.fonts) {
          if (f.og == s.text->fontOg) {
            s.font = &f;
            break;
          }
        }
      }
      break;
    case Domain::kImage:
      s.image = static_cast<const ImageEvent*>(object);
      break;
    case Domain::kPage:
      s.pageFacts = static_cast<const PageFacts*>(object);
      break;
    case Domain::kAnnot:
      s.annot = static_cast<const AnnotFacts*>(object);
      break;
    case Domain::kFont:
      s.font = static_cast<const FontFacts*>(object);
      break;
    default:
      break;
  }
  if (!s.pageFacts) s.pageFacts = pageFor(ev, page);
  return s;
}

int evalCheck(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  CheckFn fn = a.check >= 0 ? checkTable().find(a.check) : nullptr;
  if (!fn) {
    supported = false;
    return 0;
  }
  return fn(a, s, ev, supported);
}

}
