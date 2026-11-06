#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "colorconv.hh"
#include "ctx.hh"
#include "passes.hh"
#include "pdffunc.hh"
#include "util.hh"

namespace pdfa {
namespace {
struct Op {
  std::vector<QPDFObjectHandle> args;
  std::string op;
};

class Collect : public QPDFObjectHandle::ParserCallbacks {
 public:
  std::vector<Op> ops;
  void handleObject(QPDFObjectHandle obj) override {
    if (obj.isOperator()) {
      cur_.op = obj.getOperatorValue();
      ops.push_back(cur_);
      cur_ = Op();
    } else {
      cur_.args.push_back(obj);
    }
  }
  void handleEOF() override {}

 private:
  Op cur_;
};

using Mat = std::array<double, 6>;
const Mat kId{1, 0, 0, 1, 0, 0};

Mat mul(const Mat& a, const Mat& b) {
  return {a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3], a[2] * b[0] + a[3] * b[2],
          a[2] * b[1] + a[3] * b[3], a[4] * b[0] + a[5] * b[2] + b[4], a[4] * b[1] + a[5] * b[3] + b[5]};
}

bool invert(const Mat& m, Mat& out) {
  double d = m[0] * m[3] - m[1] * m[2];
  if (std::fabs(d) < 1e-12) return false;
  out = {m[3] / d, -m[1] / d, -m[2] / d, m[0] / d, (m[2] * m[5] - m[3] * m[4]) / d, (m[1] * m[4] - m[0] * m[5]) / d};
  return true;
}

std::array<double, 2> apply(const Mat& m, double x, double y) {
  return {m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]};
}

struct Rect {
  double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
  void add(double x, double y) {
    x0 = std::min(x0, x);
    y0 = std::min(y0, y);
    x1 = std::max(x1, x);
    y1 = std::max(y1, y);
  }
  void add(const Mat& m, double x, double y) { add(m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]); }
  void add(const Rect& r) {
    if (!r.valid()) return;
    add(r.x0, r.y0);
    add(r.x1, r.y1);
  }
  bool valid() const { return x1 >= x0 && y1 >= y0; }
  bool hits(const Rect& o) const { return valid() && o.valid() && x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1; }
  void grow(double d) {
    x0 -= d;
    y0 -= d;
    x1 += d;
    y1 += d;
  }
};

Rect meet(const Rect& a, const Rect& b) {
  Rect r;
  r.x0 = std::max(a.x0, b.x0);
  r.y0 = std::max(a.y0, b.y0);
  r.x1 = std::min(a.x1, b.x1);
  r.y1 = std::min(a.y1, b.y1);
  return r;
}

Rect everywhere() {
  Rect r;
  r.add(-1e9, -1e9);
  r.add(1e9, 1e9);
  return r;
}

const std::vector<std::string> kProc{"/Cyan", "/Magenta", "/Yellow", "/Black"};

struct Colour {
  int kind = 1;
  std::string cs;
  std::string fam = "/DeviceGray";
  QPDFObjectHandle space;
  std::vector<std::string> names;
  std::vector<double> v{0};
  std::string csOp;
  std::string csName;
  std::string restore;
  std::string pattern;
};

struct State {
  Mat ctm = kId;
  Colour fill, stroke;
  double ca = 1, CA = 1, gA = 1;
  int bm = 0;
  bool smask = false;
  double lw = 1, ml = 10;
  int cap = 0, join = 0;
  std::string dash = "[] 0";
  Mat tm = kId, tlm = kId;
  double fontSize = 0, tz = 1, tl = 0, rise = 0, tc = 0, tw = 0;
  std::string fontName;
  QPDFObjectHandle font;
  int tr = 0;
  std::vector<int> clips;
  Rect clipBox = everywhere();
  State() {
    fill.restore = "0 g\n";
    stroke.restore = "0 G\n";
  }
};

struct Shape {
  int kind = 0;
  bool eo = false, exact = true, noClip = false;
  std::vector<Op> ops;
  Mat ctm = kId, tm = kId;
  std::string setup;
  Rect box;
  std::vector<Op> outline;
  std::vector<int> parts;
  int clipCost() const {
    if (!exact || noClip) return 9;
    if (kind == 0) return 0;
    if (kind == 2 || kind == 4) return 1;
    if (kind == 1 && !outline.empty()) return 2;
    return 9;
  }
};

struct Piece {
  std::vector<int> clips;
  int painter = -1;
  int shade = -1;
  Colour colour;
  double alpha = 1;
  bool flat = false;
  Rect box;
};

struct Layer {
  Colour src;
  double a = 1;
  int bm = 0;
};

struct Shade {
  mutable QPDFObjectHandle dict;
  Mat ctm = kId;
  Colour frame;
  std::vector<Layer> layers;
  std::string name;
};

double num(const QPDFObjectHandle& o) { return o.isNumber() ? o.getNumericValue() : 0; }

std::string f4(double x) { return fmtFixed(x, 4); }

bool isDevice(const std::string& fam) {
  return fam == "/DeviceGray" || fam == "/DeviceRGB" || fam == "/DeviceCMYK";
}

std::string spaceKey(QPDFObjectHandle s) {
  if (!s.isInitialized() || s.isNull()) return "";
  if (s.isIndirect()) return "o" + std::to_string(s.getObjectID()) + "_" + std::to_string(s.getGeneration());
  return s.unparse();
}

struct Env {
  Ctx& ctx;
  QPDFObjectHandle res;
  int groupN = 0;
  bool x = false;
  int counter = 0;
  std::map<std::string, std::string> spaces;
  std::map<std::string, std::vector<double>> alt;

  QPDFObjectHandle cat(const std::string& c) {
    QPDFObjectHandle d = res.getKey(c);
    if (!d.isDictionary()) {
      d = QPDFObjectHandle::newDictionary();
      res.replaceKey(c, d);
    }
    return d;
  }
  std::string add(const std::string& c, const std::string& prefix, QPDFObjectHandle obj) {
    QPDFObjectHandle d = cat(c);
    std::string n;
    do {
      n = "/" + prefix + std::to_string(++counter);
    } while (d.hasKey(n));
    d.replaceKey(n, obj);
    return n;
  }
  std::string spaceName(QPDFObjectHandle space, const std::string& key) {
    auto it = spaces.find(key);
    if (it != spaces.end()) return it->second;
    std::string n = add("/ColorSpace", "KuraCS", space);
    spaces[key] = n;
    return n;
  }
};

Colour colourOf(QPDFObjectHandle cs, QPDFObjectHandle res, int depth = 0) {
  Colour c;
  if (cs.isName()) {
    std::string n = cs.getName();
    if (n == "/DeviceGray" || n == "/G") return c;
    if (n == "/DeviceRGB" || n == "/RGB") {
      c.fam = "/DeviceRGB";
      c.v = {0, 0, 0};
      return c;
    }
    if (n == "/DeviceCMYK" || n == "/CMYK") {
      c.kind = -1;
      c.fam = "/DeviceCMYK";
      c.names = kProc;
      c.v = {0, 0, 0, 1};
      return c;
    }
    c.kind = 0;
    c.fam = n == "/Pattern" ? "/Pattern" : "";
    if (n == "/Pattern" || depth > 3) return c;
    QPDFObjectHandle d = res.isDictionary() ? res.getKey("/ColorSpace") : QPDFObjectHandle::newNull();
    QPDFObjectHandle r = d.isDictionary() ? d.getKey(n) : QPDFObjectHandle::newNull();
    if (r.isNull()) return c;
    return colourOf(r, res, depth + 1);
  }
  c.kind = 0;
  c.fam = "";
  if (!cs.isArray() || cs.getArrayNItems() < 1) return c;
  int items = cs.getArrayNItems();
  std::string fam = nameOf(cs.getArrayItem(0));
  c.fam = fam;
  c.space = cs;
  if (fam == "/ICCBased" && items >= 2 && cs.getArrayItem(1).isStream()) {
    int n = static_cast<int>(numOf(cs.getArrayItem(1).getDict().getKey("/N"), 0));
    c.v.assign(n > 0 && n <= 4 ? n : 1, 0);
    if (n == 1 || n == 3) c.kind = 1;
    if (n == 4) {
      c.kind = -1;
      c.names = kProc;
    }
  } else if (fam == "/CalRGB" || fam == "/CalGray") {
    c.kind = 1;
    c.v.assign(fam == "/CalRGB" ? 3 : 1, 0);
  } else if (fam == "/Lab") {
    c.kind = 3;
    c.v = {0, 0, 0};
  } else if (fam == "/Separation" && items >= 4) {
    std::string nm = nameOf(cs.getArrayItem(1));
    if (!nm.empty() && nm != "/All" && nm != "/None") {
      c.kind = -1;
      c.names = {nm};
    }
    c.v = {1};
  } else if (fam == "/DeviceN" && items >= 4 && cs.getArrayItem(1).isArray()) {
    c.kind = -1;
    for (const auto& x : cs.getArrayItem(1).aitems()) {
      std::string nm = nameOf(x);
      if (nm.empty() || nm == "/None" || nm == "/All") c.kind = 0;
      c.names.push_back(nm);
    }
    if (c.names.empty()) c.kind = 0;
    c.v.assign(std::max<size_t>(1, c.names.size()), 1);
  } else if (fam == "/Indexed" && items >= 4) {
    c.kind = 2;
    c.v = {0};
  }
  return c;
}

Colour device(const std::vector<double>& v) {
  Colour c;
  c.v = v;
  if (v.size() == 3) c.fam = "/DeviceRGB";
  if (v.size() == 4) {
    c.kind = -1;
    c.fam = "/DeviceCMYK";
    c.names = kProc;
  }
  return c;
}

bool indexedColour(const Colour& ix, double idx, QPDFObjectHandle res, Env& env, Colour& out) {
  QPDFObjectHandle cs = ix.space;
  QPDFObjectHandle base = cs.getArrayItem(1);
  int hival = static_cast<int>(num(cs.getArrayItem(2)));
  QPDFObjectHandle lookup = cs.getArrayItem(3);
  Colour b = colourOf(base, res);
  if (b.kind != 1 && b.kind != -1) return false;
  std::string lk;
  try {
    if (lookup.isString()) {
      lk = lookup.getStringValue();
    } else if (lookup.isStream()) {
      auto buf = lookup.getStreamData(qpdf_dl_all);
      lk.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    }
  } catch (...) {
    return false;
  }
  size_t n = b.v.size();
  int i = std::clamp(static_cast<int>(std::lround(idx)), 0, std::max(0, hival));
  if ((static_cast<size_t>(i) + 1) * n > lk.size()) return false;
  for (size_t k = 0; k < n; ++k) b.v[k] = static_cast<unsigned char>(lk[i * n + k]) / 255.0;
  if (!isDevice(b.fam)) {
    QPDFObjectHandle sp = b.space;
    b.cs = env.spaceName(sp, spaceKey(sp));
  }
  out = b;
  return true;
}

bool sameSpace(const Colour& a, const Colour& b) {
  if (a.kind != b.kind || a.kind == 0 || a.kind > 1 || a.v.size() != b.v.size() || a.fam != b.fam) return false;
  if (a.cs == b.cs) return true;
  if (isDevice(a.fam)) return a.cs.empty() == b.cs.empty() && a.cs.empty();
  return spaceKey(a.space) == spaceKey(b.space) && !spaceKey(a.space).empty();
}

bool toProcess(Colour& c, int n) {
  if (c.cs.empty() && isDevice(c.fam) && static_cast<int>(c.v.size()) == n) return true;
  std::vector<double> out;
  if (c.cs.empty() && c.fam == "/DeviceGray" && !c.v.empty()) {
    double x = c.v[0];
    if (n == 3) out = {x, x, x};
    else if (n == 4) out = {0, 0, 0, 1 - x};
    else return false;
  } else {
    ColorValue cv;
    cv.family = c.fam;
    cv.colorants = c.names;
    cv.values = c.v;
    cv.space = c.space;
    if (!colorValueToProcess(cv, n, out)) return false;
  }
  std::string r = c.restore;
  c = device(out);
  c.restore = r;
  return true;
}

bool learnAlt(const Colour& c, Env& env) {
  for (size_t k = 0; k < c.names.size(); ++k) {
    const std::string& nm = c.names[k];
    if (env.alt.count(nm)) continue;
    auto p = std::find(kProc.begin(), kProc.end(), nm);
    if (p != kProc.end()) {
      std::vector<double> e(4, 0);
      e[p - kProc.begin()] = 1;
      env.alt[nm] = e;
      continue;
    }
    ColorValue cv;
    cv.family = c.fam;
    cv.colorants = c.names;
    cv.values.assign(c.names.size(), 0);
    cv.values[k] = 1;
    cv.space = c.space;
    std::vector<double> out;
    if (!colorValueToProcess(cv, 4, out)) return false;
    env.alt[nm] = out;
  }
  return true;
}

std::string tintProgram(const std::vector<std::vector<double>>& alt) {
  size_t n = alt.size();
  std::string s = "{ ";
  for (size_t j = 0; j < 4; ++j) {
    s += "0 ";
    for (size_t i = 0; i < n; ++i) {
      if (std::fabs(alt[i][j]) < 1e-5) continue;
      s += std::to_string(n + j - i) + " index " + f4(alt[i][j]) + " mul add ";
    }
    s += "dup 1 gt { pop 1 } if ";
  }
  s += std::to_string(n + 4) + " 4 roll";
  for (size_t i = 0; i < n; ++i) s += " pop";
  return s + " }";
}

bool mergeInks(Colour& a, Colour& b, Env& env) {
  if (a.names == b.names && sameSpace(a, b)) return true;
  auto has = [](const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
  };
  std::vector<std::string> u;
  for (const auto& p : kProc) {
    if (has(a.names, p) || has(b.names, p)) u.push_back(p);
  }
  for (const Colour* c : {&a, &b}) {
    for (const auto& nm : c->names) {
      if (!has(u, nm)) u.push_back(nm);
    }
  }
  if (u.size() > 8) return false;
  if (!learnAlt(a, env) || !learnAlt(b, env)) return false;
  Colour frame;
  if (u == kProc) {
    frame = device({0, 0, 0, 0});
  } else if (u == a.names) {
    frame = a;
  } else if (u == b.names) {
    frame = b;
  } else {
    std::string key = "DN";
    QPDFObjectHandle names = QPDFObjectHandle::newArray();
    std::vector<std::vector<double>> alts;
    for (const auto& nm : u) {
      key += nm;
      names.appendItem(QPDFObjectHandle::newName(nm));
      alts.push_back(env.alt[nm]);
    }
    auto it = env.spaces.find(key);
    QPDFObjectHandle arr;
    if (it == env.spaces.end()) {
      QPDFObjectHandle fn = QPDFObjectHandle::newStream(&env.ctx.pdf, tintProgram(alts));
      QPDFObjectHandle fd = fn.getDict();
      fd.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(4));
      QPDFObjectHandle dom = QPDFObjectHandle::newArray(), rng = QPDFObjectHandle::newArray();
      for (size_t i = 0; i < u.size(); ++i) {
        dom.appendItem(QPDFObjectHandle::newInteger(0));
        dom.appendItem(QPDFObjectHandle::newInteger(1));
      }
      for (int i = 0; i < 4; ++i) {
        rng.appendItem(QPDFObjectHandle::newInteger(0));
        rng.appendItem(QPDFObjectHandle::newInteger(1));
      }
      fd.replaceKey("/Domain", dom);
      fd.replaceKey("/Range", rng);
      arr = QPDFObjectHandle::newArray();
      arr.appendItem(QPDFObjectHandle::newName("/DeviceN"));
      arr.appendItem(names);
      arr.appendItem(QPDFObjectHandle::newName("/DeviceCMYK"));
      arr.appendItem(env.ctx.pdf.makeIndirectObject(fn));
      arr = env.ctx.pdf.makeIndirectObject(arr);
    }
    frame.kind = -1;
    frame.fam = "/DeviceN";
    frame.names = u;
    if (it == env.spaces.end()) {
      frame.space = arr;
      frame.cs = env.spaceName(arr, key);
    } else {
      frame.cs = it->second;
      frame.space = env.cat("/ColorSpace").getKey(it->second);
    }
  }
  for (Colour* c : {&a, &b}) {
    std::vector<double> v(u.size(), 0);
    for (size_t k = 0; k < c->names.size() && k < c->v.size(); ++k) {
      v[std::find(u.begin(), u.end(), c->names[k]) - u.begin()] = c->v[k];
    }
    std::string r = c->restore;
    *c = frame;
    c->v = v;
    c->restore = r;
  }
  return true;
}

bool unify(Colour& a, Colour& b, Env& env) {
  if (a.kind == 0 || b.kind == 0 || a.kind > 1 || b.kind > 1 || a.v.empty() || b.v.empty()) return false;
  int tn = env.groupN ? env.groupN : env.x ? 4 : 0;
  if (sameSpace(a, b) && (tn == 0 || tn == static_cast<int>(a.v.size()) || (tn == 4 && a.kind < 0))) return true;
  if (a.cs.empty() && b.cs.empty() && isDevice(a.fam) && isDevice(b.fam) && (a.v.size() == 1 || b.v.size() == 1)) {
    size_t n = std::max(a.v.size(), b.v.size());
    if (tn == 0 || tn == static_cast<int>(n)) return toProcess(a, static_cast<int>(n)) && toProcess(b, static_cast<int>(n));
  }
  if (tn == 0) tn = env.x ? 4 : (a.kind > 0 || b.kind > 0 ? 3 : 4);
  if (tn == 4 && env.x) {
    if (!(a.kind < 0 && !a.names.empty()) && !toProcess(a, 4)) return false;
    if (!(b.kind < 0 && !b.names.empty()) && !toProcess(b, 4)) return false;
    return mergeInks(a, b, env);
  }
  return toProcess(a, tn) && toProcess(b, tn);
}

std::string matStr(const Mat& m) {
  std::string s;
  for (double x : m) s += fmtFixed(x, 6) + " ";
  return s;
}

std::string setColour(const Colour& c, bool stroke) {
  std::string s;
  for (double x : c.v) s += f4(x) + " ";
  if (!c.cs.empty()) return c.cs + (stroke ? " CS " : " cs ") + s + (stroke ? "SCN\n" : "scn\n");
  const char* op = c.v.size() == 1 ? "g" : c.v.size() == 3 ? "rg" : "k";
  std::string o = op;
  if (stroke) std::transform(o.begin(), o.end(), o.begin(), ::toupper);
  return s + o + "\n";
}

int blendMode(const std::string& b) {
  static const char* names[] = {"/Normal",    "/Multiply",  "/Screen",     "/Overlay",    "/Darken",     "/Lighten",
                                "/ColorDodge", "/ColorBurn", "/HardLight",  "/SoftLight",  "/Difference", "/Exclusion",
                                "/Hue",       "/Saturation", "/Color",      "/Luminosity"};
  if (b.empty() || b == "/Compatible") return 0;
  for (int i = 0; i < 16; ++i) {
    if (b == names[i]) return i;
  }
  return -1;
}

double screenF(double b, double s) { return b + s - b * s; }
double hardLight(double b, double s) { return s <= 0.5 ? b * 2 * s : screenF(b, 2 * s - 1); }

double blendF(int m, double b, double s) {
  switch (m) {
    case 1: return b * s;
    case 2: return screenF(b, s);
    case 3: return hardLight(s, b);
    case 4: return std::min(b, s);
    case 5: return std::max(b, s);
    case 6: return b <= 0 ? 0 : s >= 1 ? 1 : std::min(1.0, b / (1 - s));
    case 7: return b >= 1 ? 1 : s <= 0 ? 0 : 1 - std::min(1.0, (1 - b) / s);
    case 8: return hardLight(b, s);
    case 9: {
      if (s <= 0.5) return b - (1 - 2 * s) * b * (1 - b);
      double d = b <= 0.25 ? ((16 * b - 12) * b + 4) * b : std::sqrt(b);
      return b + (2 * s - 1) * (d - b);
    }
    case 10: return std::fabs(b - s);
    case 11: return b + s - 2 * b * s;
    default: return s;
  }
}

using V3 = std::array<double, 3>;

double lum(const V3& c) { return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]; }

V3 clipColor(V3 c) {
  double l = lum(c);
  double n = std::min({c[0], c[1], c[2]}), x = std::max({c[0], c[1], c[2]});
  if (n < 0 && l - n > 1e-12) {
    for (double& k : c) k = l + (k - l) * l / (l - n);
  }
  if (x > 1 && x - l > 1e-12) {
    for (double& k : c) k = l + (k - l) * (1 - l) / (x - l);
  }
  return c;
}

V3 setLum(V3 c, double l) {
  double d = l - lum(c);
  for (double& k : c) k += d;
  return clipColor(c);
}

double sat(const V3& c) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }

V3 setSat(const V3& c, double s) {
  int mx = 0;
  for (int k = 1; k < 3; ++k) {
    if (c[k] > c[mx]) mx = k;
  }
  int mn = mx == 0 ? 1 : 0;
  for (int k = 0; k < 3; ++k) {
    if (k != mx && c[k] < c[mn]) mn = k;
  }
  int md = 3 - mx - mn;
  V3 r{0, 0, 0};
  if (c[mx] > c[mn]) {
    r[md] = (c[md] - c[mn]) * s / (c[mx] - c[mn]);
    r[mx] = s;
  }
  return r;
}

V3 nonSeparable(int m, const V3& b, const V3& s) {
  switch (m) {
    case 12: return setLum(setSat(s, sat(b)), lum(b));
    case 13: return setLum(setSat(b, sat(s)), lum(b));
    case 14: return setLum(s, lum(b));
    default: return setLum(b, lum(s));
  }
}

void blendVec(int bm, const Colour& frame, const std::vector<double>& cb, const std::vector<double>& cs,
              std::vector<double>& out) {
  size_t n = cs.size();
  out.assign(n, 0);
  if (bm < 12) {
    for (size_t i = 0; i < n; ++i) out[i] = blendF(bm, cb[i], cs[i]);
    return;
  }
  out = cs;
  if (frame.kind > 0) {
    if (n == 3) {
      V3 r = nonSeparable(bm, {cb[0], cb[1], cb[2]}, {cs[0], cs[1], cs[2]});
      out.assign(r.begin(), r.end());
    } else if (n == 1) {
      out[0] = nonSeparable(bm, {cb[0], cb[0], cb[0]}, {cs[0], cs[0], cs[0]})[0];
    }
    return;
  }
  int ix[4];
  for (int p = 0; p < 4; ++p) {
    auto it = std::find(frame.names.begin(), frame.names.end(), kProc[p]);
    ix[p] = it == frame.names.end() || static_cast<size_t>(it - frame.names.begin()) >= n
                ? -1
                : static_cast<int>(it - frame.names.begin());
  }
  if (ix[0] >= 0 && ix[1] >= 0 && ix[2] >= 0) {
    V3 r = nonSeparable(bm, {cb[ix[0]], cb[ix[1]], cb[ix[2]]}, {cs[ix[0]], cs[ix[1]], cs[ix[2]]});
    for (int p = 0; p < 3; ++p) out[ix[p]] = r[p];
  }
  if (ix[3] >= 0) out[ix[3]] = bm == 15 ? cs[ix[3]] : cb[ix[3]];
}

std::optional<Piece> over(const Piece& back, Colour src, double a, int bm, Env& env) {
  Colour bc = back.colour;
  if (!unify(bc, src, env)) return std::nullopt;
  Piece r;
  r.colour = src;
  r.alpha = back.alpha + a - back.alpha * a;
  bool add = src.kind > 0;
  size_t n = src.v.size();
  std::vector<double> cb(n), cs(n), bl;
  for (size_t i = 0; i < n; ++i) {
    cb[i] = add ? bc.v[i] : 1 - bc.v[i];
    cs[i] = add ? src.v[i] : 1 - src.v[i];
  }
  blendVec(bm, src, cb, cs, bl);
  for (size_t i = 0; i < n; ++i) {
    double mixed = (1 - back.alpha) * cs[i] + back.alpha * bl[i];
    double cr = r.alpha > 0 ? (1 - a / r.alpha) * cb[i] + (a / r.alpha) * mixed : cs[i];
    r.colour.v[i] = add ? cr : 1 - cr;
  }
  return r;
}

Colour onPaper(const Piece& p) {
  Colour c = p.colour;
  for (double& x : c.v) x = c.kind > 0 ? p.alpha * x + (1 - p.alpha) : p.alpha * x;
  return c;
}

std::string unparseOp(const Op& o) {
  std::string s;
  for (const auto& a : o.args) s += a.unparseResolved() + " ";
  return s + o.op + "\n";
}

std::string pathStr(const std::vector<Op>& ops, const Mat& m) {
  std::string out;
  auto pt = [&](double x, double y) { return f4(m[0] * x + m[2] * y + m[4]) + " " + f4(m[1] * x + m[3] * y + m[5]) + " "; };
  for (const Op& o : ops) {
    auto n = [&](size_t k) { return k < o.args.size() ? num(o.args[k]) : 0.0; };
    if (o.op == "re") {
      double x = n(0), y = n(1), w = n(2), h = n(3);
      out += pt(x, y) + "m " + pt(x + w, y) + "l " + pt(x + w, y + h) + "l " + pt(x, y + h) + "l h\n";
    } else if (o.op == "h") {
      out += "h\n";
    } else {
      for (size_t k = 0; k + 1 < o.args.size(); k += 2) out += pt(n(k), n(k + 1));
      out += o.op + "\n";
    }
  }
  return out;
}

std::string textStr(const Shape& s, const Mat& tm, int tr) {
  return "BT " + s.setup + matStr(tm) + "Tm " + std::to_string(tr) + " Tr " + unparseOp(s.ops[0]) + "ET\n";
}

std::string emitClip(const std::vector<Shape>& shapes, const Shape& s, const Mat& ci) {
  if (s.kind == 1) return pathStr(s.outline, mul(s.ctm, ci)) + "W n\n";
  if (s.kind == 0) return pathStr(s.ops, mul(s.ctm, ci)) + (s.eo ? "W* n\n" : "W n\n");
  if (s.kind == 4) {
    std::string out = "BT ";
    for (int p : s.parts) {
      const Shape& t = shapes[p];
      out += t.setup + matStr(mul(t.tm, mul(t.ctm, ci))) + "Tm 7 Tr " + unparseOp(t.ops[0]);
    }
    return out + "ET\n";
  }
  return textStr(s, mul(s.tm, mul(s.ctm, ci)), 7);
}

std::string emitPaint(const Shape& s, const Colour& c, const Mat& ci) {
  Mat m = mul(s.ctm, ci);
  if (s.kind == 0) return setColour(c, false) + pathStr(s.ops, m) + (s.eo ? "f*\n" : "f\n");
  if (s.kind == 1) return "q " + matStr(m) + "cm " + s.setup + setColour(c, true) + pathStr(s.ops, kId) + "S Q\n";
  return "q " + matStr(m) + "cm " + setColour(c, s.kind == 3) + textStr(s, s.tm, s.kind == 2 ? 0 : 1) + "Q\n";
}

using Pt = std::array<double, 2>;

Op mkOp(const std::string& name, std::initializer_list<double> v) {
  Op o;
  o.op = name;
  for (double x : v) o.args.push_back(QPDFObjectHandle::newReal(x, 6));
  return o;
}

void addPoly(std::vector<Op>& out, std::vector<Pt> p) {
  double area = 0;
  for (size_t i = 0; i < p.size(); ++i) {
    const Pt& a = p[i];
    const Pt& b = p[(i + 1) % p.size()];
    area += a[0] * b[1] - b[0] * a[1];
  }
  if (std::fabs(area) < 1e-12) return;
  if (area < 0) std::reverse(p.begin(), p.end());
  out.push_back(mkOp("m", {p[0][0], p[0][1]}));
  for (size_t i = 1; i < p.size(); ++i) out.push_back(mkOp("l", {p[i][0], p[i][1]}));
  out.push_back(mkOp("h", {}));
}

void addCircle(std::vector<Op>& out, Pt c, double r) {
  double k = 0.5523 * r, x = c[0], y = c[1];
  out.push_back(mkOp("m", {x + r, y}));
  out.push_back(mkOp("c", {x + r, y + k, x + k, y + r, x, y + r}));
  out.push_back(mkOp("c", {x - k, y + r, x - r, y + k, x - r, y}));
  out.push_back(mkOp("c", {x - r, y - k, x - k, y - r, x, y - r}));
  out.push_back(mkOp("c", {x + k, y - r, x + r, y - k, x + r, y}));
  out.push_back(mkOp("h", {}));
}

std::vector<Op> strokeOutline(const std::vector<Op>& ops, const Mat& ctm, double lw, int cap, int join, double ml) {
  double scale = std::sqrt(std::fabs(ctm[0] * ctm[3] - ctm[1] * ctm[2]));
  std::vector<Op> out;
  if (lw * scale < 1) return out;
  double hw = lw / 2;
  std::vector<std::pair<std::vector<Pt>, bool>> subs;
  Pt cur{0, 0}, start{0, 0};
  auto pt = [](const Op& o, size_t k) { return Pt{num(o.args[k]), num(o.args[k + 1])}; };
  auto bez = [&](Pt p0, Pt p1, Pt p2, Pt p3) {
    double len = 0;
    Pt q[4] = {p0, p1, p2, p3};
    for (int i = 0; i < 3; ++i) len += std::hypot(q[i + 1][0] - q[i][0], q[i + 1][1] - q[i][1]);
    int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(len * scale * 8))), 4, 512);
    for (int i = 1; i <= n; ++i) {
      double t = static_cast<double>(i) / n, u = 1 - t;
      double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
      subs.back().first.push_back({a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0], a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1]});
    }
  };
  auto ensure = [&]() {
    if (subs.empty() || subs.back().second) subs.push_back({{cur}, false});
  };
  for (const Op& o : ops) {
    size_t na = o.args.size();
    if (o.op == "m" && na >= 2) {
      cur = start = pt(o, 0);
      subs.push_back({{cur}, false});
    } else if (o.op == "l" && na >= 2) {
      ensure();
      cur = pt(o, 0);
      subs.back().first.push_back(cur);
    } else if (o.op == "c" && na >= 6) {
      ensure();
      bez(cur, pt(o, 0), pt(o, 2), pt(o, 4));
      cur = pt(o, 4);
    } else if (o.op == "v" && na >= 4) {
      ensure();
      bez(cur, cur, pt(o, 0), pt(o, 2));
      cur = pt(o, 2);
    } else if (o.op == "y" && na >= 4) {
      ensure();
      bez(cur, pt(o, 0), pt(o, 2), pt(o, 2));
      cur = pt(o, 2);
    } else if (o.op == "re" && na >= 4) {
      double x = num(o.args[0]), y = num(o.args[1]), w = num(o.args[2]), h = num(o.args[3]);
      subs.push_back({{{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}, true});
      cur = start = {x, y};
    } else if (o.op == "h") {
      if (!subs.empty() && !subs.back().second) subs.back().second = true;
      cur = start;
    } else {
      return {};
    }
  }
  for (auto& [raw, closed] : subs) {
    std::vector<Pt> p;
    for (const Pt& q : raw) {
      if (p.empty() || std::hypot(q[0] - p.back()[0], q[1] - p.back()[1]) > 1e-9) p.push_back(q);
    }
    if (closed && p.size() > 1 && std::hypot(p[0][0] - p.back()[0], p[0][1] - p.back()[1]) <= 1e-9) p.pop_back();
    if (p.size() == 1) {
      if (cap == 1) addCircle(out, p[0], hw);
      if (cap == 2) addPoly(out, {{p[0][0] - hw, p[0][1] - hw}, {p[0][0] + hw, p[0][1] - hw}, {p[0][0] + hw, p[0][1] + hw}, {p[0][0] - hw, p[0][1] + hw}});
      continue;
    }
    size_t m = p.size(), segs = closed ? m : m - 1;
    auto dir = [&](size_t i) {
      const Pt& a = p[i % m];
      const Pt& b = p[(i + 1) % m];
      double l = std::hypot(b[0] - a[0], b[1] - a[1]);
      return Pt{(b[0] - a[0]) / l, (b[1] - a[1]) / l};
    };
    for (size_t i = 0; i < segs; ++i) {
      const Pt& a = p[i];
      const Pt& b = p[(i + 1) % m];
      Pt d = dir(i), nn{-d[1] * hw, d[0] * hw};
      Pt a0 = a, b0 = b;
      if (!closed && cap == 2) {
        if (i == 0) a0 = {a[0] - d[0] * hw, a[1] - d[1] * hw};
        if (i + 1 == segs) b0 = {b[0] + d[0] * hw, b[1] + d[1] * hw};
      }
      addPoly(out, {{a0[0] + nn[0], a0[1] + nn[1]}, {b0[0] + nn[0], b0[1] + nn[1]}, {b0[0] - nn[0], b0[1] - nn[1]}, {a0[0] - nn[0], a0[1] - nn[1]}});
    }
    if (!closed && cap == 1) {
      addCircle(out, p[0], hw);
      addCircle(out, p[m - 1], hw);
    }
    for (size_t v = closed ? 0 : 1; v < (closed ? m : m - 1); ++v) {
      Pt d1 = dir((v + m - 1) % m), d2 = dir(v);
      const Pt& c = p[v];
      if (join == 1) {
        addCircle(out, c, hw);
        continue;
      }
      double cr = d1[0] * d2[1] - d1[1] * d2[0];
      if (std::fabs(cr) < 1e-12) continue;
      double side = cr > 0 ? -1 : 1;
      Pt o1{-d1[1] * side, d1[0] * side}, o2{-d2[1] * side, d2[0] * side};
      Pt e1{c[0] + o1[0] * hw, c[1] + o1[1] * hw}, e2{c[0] + o2[0] * hw, c[1] + o2[1] * hw};
      double cosHalf = std::sqrt(std::max(0.0, (1 + o1[0] * o2[0] + o1[1] * o2[1]) / 2));
      if (join == 0 && cosHalf > 1e-9 && 1 / cosHalf <= ml) {
        Pt bis{o1[0] + o2[0], o1[1] + o2[1]};
        double bl = std::hypot(bis[0], bis[1]);
        double r = hw / cosHalf;
        addPoly(out, {c, e1, {c[0] + bis[0] / bl * r, c[1] + bis[1] / bl * r}, e2});
      } else {
        addPoly(out, {c, e1, e2});
      }
    }
  }
  return out;
}

std::string strokeSetup(const State& st) {
  return f4(st.lw) + " w " + std::to_string(st.cap) + " J " + std::to_string(st.join) + " j " + f4(st.ml) + " M " +
         st.dash + " d ";
}

std::string textSetup(const State& st) {
  return st.fontName + " " + f4(st.fontSize) + " Tf " + f4(st.tc) + " Tc " + f4(st.tw) + " Tw " + f4(st.tz * 100) +
         " Tz " + f4(st.rise) + " Ts ";
}

bool isPathOp(const std::string& op) {
  return op == "m" || op == "l" || op == "c" || op == "v" || op == "y" || op == "re" || op == "h";
}

int groupComponents(QPDFObjectHandle page) {
  QPDFObjectHandle g = page.getKey("/Group");
  if (!g.isDictionary()) return 0;
  QPDFObjectHandle cs = g.getKey("/CS");
  if (cs.isArray() && cs.getArrayNItems() == 1) cs = cs.getArrayItem(0);
  std::string n = nameOf(cs);
  if (n == "/DeviceGray" || n == "/CalGray") return 1;
  if (n == "/DeviceRGB" || n == "/CalRGB") return 3;
  if (n == "/DeviceCMYK") return 4;
  if (cs.isArray() && cs.getArrayNItems() >= 2 && cs.getArrayItem(1).isStream()) {
    return static_cast<int>(numOf(cs.getArrayItem(1).getDict().getKey("/N"), 0));
  }
  if (cs.isArray() && cs.getArrayNItems() >= 1) {
    std::string f = nameOf(cs.getArrayItem(0));
    if (f == "/CalRGB") return 3;
    if (f == "/CalGray") return 1;
  }
  return cs.isNull() ? 0 : -1;
}

bool usesBlend(QPDFObjectHandle res) {
  QPDFObjectHandle g = res.isDictionary() ? res.getKey("/ExtGState") : QPDFObjectHandle::newNull();
  if (!g.isDictionary()) return false;
  for (const std::string& k : g.getKeys()) {
    QPDFObjectHandle e = g.getKey(k);
    if (!e.isDictionary()) continue;
    QPDFObjectHandle bm = e.getKey("/BM");
    std::string b = nameOf(bm.isArray() && bm.getArrayNItems() ? bm.getArrayItem(0) : bm);
    if (blendMode(b) != 0) return true;
  }
  return false;
}

using Ren = std::map<std::string, std::map<std::string, std::string>>;

struct Inliner {
  Env& env;
  size_t budget = 300000;

  Op rename(const Op& o, const Ren& ren) {
    if (ren.empty()) return o;
    Op r = o;
    auto sw = [&](size_t k, const char* cat) {
      if (k >= r.args.size() || !r.args[k].isName()) return;
      auto c = ren.find(cat);
      if (c == ren.end()) return;
      auto f = c->second.find(r.args[k].getName());
      if (f != c->second.end()) r.args[k] = QPDFObjectHandle::newName(f->second);
    };
    const std::string& op = o.op;
    if (op == "Tf") sw(0, "/Font");
    else if (op == "Do") sw(0, "/XObject");
    else if (op == "gs") sw(0, "/ExtGState");
    else if (op == "sh") sw(0, "/Shading");
    else if (op == "cs" || op == "CS") sw(0, "/ColorSpace");
    else if ((op == "scn" || op == "SCN") && !r.args.empty()) sw(r.args.size() - 1, "/Pattern");
    else if (op == "BDC" || op == "DP") sw(1, "/Properties");
    else if (op == "ID") {
      for (size_t k = 0; k + 1 < r.args.size(); k += 2) {
        std::string key = nameOf(r.args[k]);
        if (key == "/CS" || key == "/ColorSpace") sw(k + 1, "/ColorSpace");
      }
    }
    return r;
  }

  bool inlinable(QPDFObjectHandle x, int depth) {
    if (depth >= 6 || !x.isStream()) return false;
    QPDFObjectHandle d = x.getDict();
    if (!nameIs(d.getKey("/Subtype"), "/Form")) return false;
    for (const char* k : {"/StructParents", "/StructParent", "/OC", "/Ref", "/OPI"}) {
      if (d.hasKey(k)) return false;
    }
    QPDFObjectHandle bb = d.getKey("/BBox");
    if (!bb.isArray() || bb.getArrayNItems() != 4) return false;
    QPDFObjectHandle g = d.getKey("/Group");
    if (g.isDictionary() && nameIs(g.getKey("/S"), "/Transparency")) {
      if (g.getKey("/K").isBool() && g.getKey("/K").getBoolValue()) return false;
      if (g.getKey("/I").isBool() && g.getKey("/I").getBoolValue() && usesBlend(d.getKey("/Resources"))) return false;
    }
    QPDFObjectHandle r = d.getKey("/Resources");
    if (r.isDictionary() && r.getKey("/Pattern").isDictionary() && !r.getKey("/Pattern").getKeys().empty()) return false;
    return true;
  }

  void run(const std::vector<Op>& in, const Ren& ren, int depth, std::vector<Op>& out) {
    for (const Op& o0 : in) {
      Op o = rename(o0, ren);
      if (o.op == "Do" && !o.args.empty() && o.args[0].isName() && out.size() < budget) {
        QPDFObjectHandle xod = env.res.getKey("/XObject");
        QPDFObjectHandle x = xod.isDictionary() ? xod.getKey(o.args[0].getName()) : QPDFObjectHandle::newNull();
        if (inlinable(x, depth) && expand(x, ren, depth, out)) continue;
      }
      out.push_back(o);
    }
  }

  bool expand(QPDFObjectHandle x, const Ren& parent, int depth, std::vector<Op>& out) {
    Collect c;
    try {
      QPDFObjectHandle::parseContentStream(x, &c);
    } catch (...) {
      return false;
    }
    if (out.size() + c.ops.size() > budget) return false;
    int bal = 0;
    for (const Op& o : c.ops) {
      if (o.op == "q") ++bal;
      if (o.op == "Q" && --bal < 0) return false;
      if (o.op == "BT") ++bal;
      if (o.op == "ET" && --bal < 0) return false;
    }
    if (bal != 0) return false;
    QPDFObjectHandle d = x.getDict();
    Ren ren;
    QPDFObjectHandle r = d.getKey("/Resources");
    if (r.isDictionary()) {
      for (const char* cat : {"/Font", "/XObject", "/ExtGState", "/ColorSpace", "/Pattern", "/Shading", "/Properties"}) {
        QPDFObjectHandle cd = r.getKey(cat);
        if (!cd.isDictionary()) continue;
        QPDFObjectHandle pd = env.cat(cat);
        for (const std::string& k : cd.getKeys()) {
          QPDFObjectHandle v = cd.getKey(k);
          QPDFObjectHandle have = pd.getKey(k);
          if (v.isIndirect() && have.isIndirect() && have.getObjGen() == v.getObjGen()) continue;
          ren[cat][k] = env.add(cat, k.substr(1) + "_K", v);
        }
      }
    } else {
      ren = parent;
    }
    QPDFObjectHandle bb = d.getKey("/BBox");
    double x0 = num(bb.getArrayItem(0)), y0 = num(bb.getArrayItem(1)), x1 = num(bb.getArrayItem(2)),
           y1 = num(bb.getArrayItem(3));
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    out.push_back(Op{{}, "q"});
    QPDFObjectHandle mm = d.getKey("/Matrix");
    if (mm.isArray() && mm.getArrayNItems() == 6) {
      Op cm;
      cm.op = "cm";
      for (int k = 0; k < 6; ++k) cm.args.push_back(QPDFObjectHandle::newReal(num(mm.getArrayItem(k)), 6));
      out.push_back(cm);
    }
    out.push_back(mkOp("re", {x0, y0, x1 - x0, y1 - y0}));
    out.push_back(Op{{}, "W"});
    out.push_back(Op{{}, "n"});
    QPDFObjectHandle g = d.getKey("/Group");
    bool grp = g.isDictionary() && nameIs(g.getKey("/S"), "/Transparency");
    if (grp) out.push_back(mkOp("KuraGB", {x0, y0, x1 - x0, y1 - y0}));
    run(c.ops, ren, depth + 1, out);
    if (grp) out.push_back(Op{{}, "KuraGE"});
    out.push_back(Op{{}, "Q"});
    return true;
  }
};

bool type3Coloured(QPDFObjectHandle font) {
  QPDFObjectHandle cp = font.getKey("/CharProcs");
  if (!cp.isDictionary()) return true;
  for (const std::string& k : cp.getKeys()) {
    QPDFObjectHandle s = cp.getKey(k);
    if (!s.isStream()) continue;
    Collect c;
    try {
      QPDFObjectHandle::parseContentStream(s, &c);
    } catch (...) {
      return true;
    }
    for (const Op& o : c.ops) {
      if (o.op == "d0") return true;
      if (o.op == "d1") break;
    }
  }
  return false;
}
}

bool vectorFlattenPage(Ctx& ctx, QPDFPageObjectHelper& ph, int pageIndex, int* regionsOut, double dpiWanted) {
  QPDFObjectHandle page = ph.getObjectHandle();
  QPDFObjectHandle res0 = ph.getAttribute("/Resources", true);
  if (!res0.isDictionary()) return false;
  int groupN = groupComponents(page);
  if (groupN < 0) return false;
  Collect col;
  try {
    QPDFObjectHandle::parseContentStream(page.getKey("/Contents"), &col);
  } catch (...) {
    return false;
  }
  QPDFObjectHandle res = res0.shallowCopy();
  for (const std::string& k : res.getKeys()) {
    if (res.getKey(k).isDictionary()) res.replaceKey(k, res.getKey(k).shallowCopy());
  }
  Env env{ctx, res, groupN, ctx.isX(), 0, {}, {}};
  std::vector<Op> ops;
  Inliner inl{env, 300000};
  inl.run(col.ops, Ren(), 0, ops);

  std::vector<State> stack;
  State st;
  std::vector<Shape> shapes;
  std::vector<Piece> pieces;
  std::vector<Shade> shades;
  Rect path;
  std::vector<Rect> pendingVec;
  size_t pathStart = 0;
  bool inPath = false, inText = false, pendingW = false, pendingEo = false, tmExact = true;
  std::string pendingText;
  size_t emitted = 0;
  int regions = 0;
  std::map<size_t, std::string> before, after, replace;
  std::vector<int> textClips;
  std::map<std::string, std::shared_ptr<std::vector<double>>> advCache;
  std::map<std::string, bool> t3Cache;

  struct Render {
    bool tried = false, ok = false;
    int w = 0, h = 0;
    std::string rgb;
    Mat d = kId, dinv = kId;
    Rect page;
  } rnd;
  auto ensureRender = [&]() -> bool {
    if (rnd.tried) return rnd.ok;
    rnd.tried = true;
    if (!ctx.opt.rasterizePage) return false;
    double dpi = dpiWanted > 0 ? dpiWanted : ctx.opt.rasterDpi > 0 ? ctx.opt.rasterDpi : 300;
    if (!ctx.opt.rasterizePage(pageIndex, dpi, rnd.w, rnd.h, rnd.rgb)) return false;
    if (rnd.w <= 0 || rnd.h <= 0 || rnd.rgb.size() < static_cast<size_t>(rnd.w) * rnd.h * 3) return false;
    auto boxOf = [&](const char* key, Rect& r) {
      QPDFObjectHandle b = ph.getAttribute(key, false);
      if (!b.isArray() || b.getArrayNItems() != 4) return false;
      r = Rect();
      r.add(num(b.getArrayItem(0)), num(b.getArrayItem(1)));
      r.add(num(b.getArrayItem(2)), num(b.getArrayItem(3)));
      return r.valid() && r.x1 > r.x0 && r.y1 > r.y0;
    };
    Rect media, crop;
    if (!boxOf("/MediaBox", media)) {
      media = Rect();
      media.add(0, 0);
      media.add(612, 792);
    }
    rnd.page = media;
    if (boxOf("/CropBox", crop)) {
      Rect m = meet(crop, media);
      if (m.valid() && m.x1 > m.x0 && m.y1 > m.y0) rnd.page = m;
    }
    QPDFObjectHandle ro = ph.getAttribute("/Rotate", false);
    long long rot = ro.isInteger() ? ro.getIntValue() % 360 : 0;
    if (rot < 0) rot += 360;
    const Rect& b = rnd.page;
    double bw = b.x1 - b.x0, bh = b.y1 - b.y0;
    if (rot == 90 || rot == 270) {
      double sx = rnd.w / bh, sy = rnd.h / bw;
      rnd.d = rot == 90 ? Mat{0, sy, sx, 0, -b.y0 * sx, -b.x0 * sy} : Mat{0, -sy, -sx, 0, b.y1 * sx, b.x1 * sy};
    } else {
      double sx = rnd.w / bw, sy = rnd.h / bh;
      rnd.d = rot == 180 ? Mat{-sx, 0, 0, sy, b.x1 * sx, -b.y0 * sy} : Mat{sx, 0, 0, -sy, -b.x0 * sx, b.y1 * sy};
    }
    if (!invert(rnd.d, rnd.dinv)) return false;
    rnd.ok = true;
    return true;
  };
  std::vector<std::array<int, 4>> crops;
  std::map<QPDFObjGen, bool> tmemo;
  struct Covered {
    size_t i;
    Rect vis;
    std::vector<int> clips;
  };
  std::vector<Covered> opaqueImages;
  auto region = [&](const std::string& clip, Rect box, size_t at, bool rect = false) -> bool {
    box = meet(box, st.clipBox);
    if (rect) {
      for (const Covered& c : opaqueImages) {
        if (replace.count(c.i)) continue;
        bool inside = c.vis.x0 >= box.x0 - 0.5 && c.vis.y0 >= box.y0 - 0.5 && c.vis.x1 <= box.x1 + 0.5 &&
                      c.vis.y1 <= box.y1 + 0.5;
        bool sub = std::all_of(st.clips.begin(), st.clips.end(), [&](int k) {
          return std::find(c.clips.begin(), c.clips.end(), k) != c.clips.end();
        });
        if (inside && sub) replace[c.i] = "";
      }
    }
    if (!ensureRender()) return false;
    Mat ci;
    if (!invert(st.ctm, ci)) return false;
    box = meet(box, rnd.page);
    if (!box.valid() || box.x1 - box.x0 <= 0 || box.y1 - box.y0 <= 0) return true;
    Rect pr;
    for (double x : {box.x0, box.x1}) {
      for (double y : {box.y0, box.y1}) pr.add(rnd.d, x, y);
    }
    int u0 = std::max(0, static_cast<int>(std::floor(pr.x0)) - 1);
    int v0 = std::max(0, static_cast<int>(std::floor(pr.y0)) - 1);
    int u1 = std::min(rnd.w, static_cast<int>(std::ceil(pr.x1)) + 1);
    int v1 = std::min(rnd.h, static_cast<int>(std::ceil(pr.y1)) + 1);
    if (u1 <= u0 || v1 <= v0) return true;
    std::string token = "\x01KuraRgnTok" + std::to_string(crops.size()) + "\x01";
    crops.push_back({u0, v0, u1, v1});
    std::string s = "q\n" + clip + matStr(ci) + "cm " + f4(box.x0) + " " + f4(box.y0) + " " + f4(box.x1 - box.x0) +
                    " " + f4(box.y1 - box.y0) + " re W n " + token + " Q\n";
    if (inText) pendingText += s;
    else after[at] += s;
    Piece p;
    p.box = box;
    pieces.push_back(p);
    ++regions;
    return true;
  };
  auto patternTransparent = [&](const std::string& name) {
    QPDFObjectHandle pd = res.getKey("/Pattern");
    QPDFObjectHandle p = pd.isDictionary() ? pd.getKey(name) : QPDFObjectHandle::newNull();
    QPDFObjectHandle d = p.isStream() ? p.getDict() : p;
    if (!d.isDictionary()) return false;
    QPDFObjectHandle probe = QPDFObjectHandle::newDictionary();
    QPDFObjectHandle g = QPDFObjectHandle::newDictionary();
    if (d.getKey("/ExtGState").isDictionary()) g.replaceKey("/P", d.getKey("/ExtGState"));
    probe.replaceKey("/ExtGState", g);
    return resourcesHaveTransparency(probe, tmemo) || (p.isStream() && resourcesHaveTransparency(d.getKey("/Resources"), tmemo));
  };
  auto buildShading = [&](const Shade& sh) -> std::string {
    auto fn = PdfFunction::loadArray(sh.dict.getKey("/Function"));
    size_t n = sh.frame.v.size();
    if (!fn || fn->inputs() != 1 || static_cast<size_t>(fn->outputs()) != n || n > 32) return "";
    QPDFObjectHandle dom = sh.dict.getKey("/Domain");
    double d0 = 0, d1 = 1;
    if (dom.isArray() && dom.getArrayNItems() == 2) {
      d0 = num(dom.getArrayItem(0));
      d1 = num(dom.getArrayItem(1));
    }
    std::string data;
    Colour outFrame;
    for (int k = 0; k < 256; ++k) {
      double t = d0 + (d1 - d0) * k / 255.0, out[32] = {0};
      if (!fn->compute(&t, out)) return "";
      Piece q;
      q.colour = sh.frame;
      q.colour.v.assign(out, out + n);
      q.alpha = 1;
      for (const Layer& l : sh.layers) {
        auto r = over(q, l.src, l.a, l.bm, env);
        if (!r) return "";
        q = *r;
      }
      if (k == 0) outFrame = q.colour;
      if (q.colour.v.size() != outFrame.v.size() || q.colour.fam != outFrame.fam) return "";
      for (double x : q.colour.v) data += static_cast<char>(std::clamp(static_cast<int>(std::lround(x * 255)), 0, 255));
    }
    size_t on = outFrame.v.size();
    QPDFObjectHandle outSpace = isDevice(outFrame.fam) && outFrame.cs.empty() ? QPDFObjectHandle::newName(outFrame.fam)
                                : outFrame.cs == sh.frame.cs ? sh.dict.getKey("/ColorSpace")
                                                             : outFrame.space;
    if (!outSpace.isInitialized() || outSpace.isNull()) return "";
    QPDFObjectHandle f = QPDFObjectHandle::newStream(&ctx.pdf, data);
    QPDFObjectHandle fd = f.getDict();
    fd.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(0));
    QPDFObjectHandle da = QPDFObjectHandle::newArray(), ra = QPDFObjectHandle::newArray(), sz = QPDFObjectHandle::newArray();
    da.appendItem(QPDFObjectHandle::newReal(d0, 6));
    da.appendItem(QPDFObjectHandle::newReal(d1, 6));
    for (size_t i = 0; i < on; ++i) {
      ra.appendItem(QPDFObjectHandle::newInteger(0));
      ra.appendItem(QPDFObjectHandle::newInteger(1));
    }
    sz.appendItem(QPDFObjectHandle::newInteger(256));
    fd.replaceKey("/Domain", da);
    fd.replaceKey("/Range", ra);
    fd.replaceKey("/Size", sz);
    fd.replaceKey("/BitsPerSample", QPDFObjectHandle::newInteger(8));
    QPDFObjectHandle nd = sh.dict.shallowCopy();
    nd.replaceKey("/Function", ctx.pdf.makeIndirectObject(f));
    nd.replaceKey("/ColorSpace", outSpace);
    return env.add("/Shading", "KuraSh", ctx.pdf.makeIndirectObject(nd));
  };
  auto paintShape = [&](int si, const Colour& c, double alpha, bool strokeSide, size_t limit, std::string& pre,
                        std::string& post, std::string& extra, bool reemit) -> int {
    const Shape s = shapes[si];
    Piece base;
    base.clips = st.clips;
    base.painter = si;
    base.colour = c;
    base.flat = (c.kind == 1 || c.kind == -1) && s.exact;
    base.box = meet(s.box, st.clipBox);
    if (c.kind == 0 && !c.pattern.empty() && patternTransparent(c.pattern)) return 0;
    bool transparent = alpha < 1 || st.bm != 0 || st.smask;
    Mat ci;
    bool inv = invert(st.ctm, ci);
    if (!transparent) {
      if (reemit) {
        if (!inv || !base.flat) return 0;
        extra += "q\n" + emitPaint(s, c, ci) + "Q\n";
      }
      pieces.push_back(base);
      return 1;
    }
    if (st.bm < 0 || st.smask || !base.flat || !inv) return 0;
    if (env.x && !env.groupN && base.colour.kind > 0 && !toProcess(base.colour, 4)) return 0;
    base.alpha = alpha;
    std::string myPre = setColour(onPaper(base), strokeSide), myExtra;
    if (reemit) myExtra += "q\n" + emitPaint(s, onPaper(base), ci) + "Q\n";
    std::vector<Piece> add{base};
    for (size_t j = 0; j < limit; ++j) {
      const Piece& p = pieces[j];
      if (!p.box.hits(base.box)) continue;
      Piece n;
      n.clips = base.clips;
      for (int k : p.clips) {
        if (std::find(n.clips.begin(), n.clips.end(), k) == n.clips.end()) n.clips.push_back(k);
      }
      n.box = meet(p.box, base.box);
      if (p.shade >= 0) {
        if (s.clipCost() >= 9) return 0;
        Colour tmpl = shades[p.shade].frame;
        if (!sameSpace(tmpl, c)) return 0;
        Shade ns = shades[p.shade];
        ns.layers.push_back({c, alpha, st.bm});
        ns.name = buildShading(ns);
        if (ns.name.empty()) return 0;
        shades.push_back(ns);
        n.clips.push_back(si);
        n.shade = static_cast<int>(shades.size()) - 1;
        add.push_back(n);
        continue;
      }
      if (!p.flat) return 0;
      std::optional<Piece> r = over(p, c, alpha, st.bm, env);
      if (!r) return 0;
      int pc = shapes[p.painter].clipCost(), tc = s.clipCost();
      if (std::min(pc, tc) >= 9) return 0;
      n.colour = r->colour;
      n.alpha = r->alpha;
      if (pc <= tc) {
        n.clips.push_back(p.painter);
        n.painter = si;
      } else {
        n.clips.push_back(si);
        n.painter = p.painter;
      }
      n.flat = true;
      add.push_back(n);
    }
    if (pieces.size() + add.size() > 4000) return 0;
    std::set<int> active(st.clips.begin(), st.clips.end());
    for (size_t k = 1; k < add.size(); ++k) {
      myExtra += "q\n";
      for (int cidx : add[k].clips) {
        if (!active.count(cidx)) myExtra += emitClip(shapes, shapes[cidx], ci);
      }
      if (add[k].shade >= 0) {
        const Shade& sh = shades[add[k].shade];
        myExtra += "q " + matStr(mul(sh.ctm, ci)) + "cm " + sh.name + " sh Q\n";
      } else {
        myExtra += emitPaint(shapes[add[k].painter], onPaper(add[k]), ci);
      }
      myExtra += "Q\n";
    }
    if (emitted + myExtra.size() > (size_t{16} << 20)) return 0;
    emitted += myExtra.size();
    pre += myPre;
    post += c.restore;
    extra += myExtra;
    pieces.insert(pieces.end(), add.begin(), add.end());
    return 1;
  };
  auto pendingHits = [&](const std::vector<Rect>& boxes) {
    for (const Rect& b : boxes) {
      for (const Rect& p : pendingVec) {
        if (p.hits(b)) return true;
      }
    }
    return false;
  };
  auto addBlock = [&](const Rect& box) {
    Piece p;
    p.box = meet(box, st.clipBox);
    if (inText && pendingHits({p.box})) return false;
    pieces.push_back(p);
    return true;
  };
  auto rectClip = [](double x, double y, double w, double h) {
    return f4(x) + " " + f4(y) + " " + f4(w) + " " + f4(h) + " re W n\n";
  };
  auto setSc = [&](Colour& c, const Op& o) {
    const auto& a = o.args;
    std::string csOp = c.csOp, csName = c.csName;
    Colour sp = c;
    if (!csName.empty()) {
      sp = colourOf(QPDFObjectHandle::newName(csName), res);
      if (!isDevice(sp.fam)) sp.cs = csName;
    }
    sp.csOp = csOp;
    sp.csName = csName;
    sp.restore = csOp + unparseOp(o);
    if (!a.empty() && a.back().isName()) {
      sp.kind = 0;
      sp.pattern = a.back().getName();
      c = sp;
      return;
    }
    std::vector<double> v;
    for (const auto& x : a) v.push_back(num(x));
    if (sp.kind == 2) {
      Colour b;
      if (v.size() == 1 && indexedColour(sp, v[0], res, env, b)) {
        b.csOp = sp.csOp;
        b.csName = sp.csName;
        b.restore = sp.restore;
        c = b;
      } else {
        sp.kind = 0;
        c = sp;
      }
      return;
    }
    if (sp.kind == 3) {
      ColorValue cv;
      cv.family = "/Lab";
      cv.values = v;
      std::vector<double> out;
      int tn = env.groupN ? env.groupN : (env.x ? 4 : 3);
      if (v.size() == 3 && colorValueToProcess(cv, tn, out)) {
        Colour d = device(out);
        d.csOp = sp.csOp;
        d.csName = sp.csName;
        d.restore = sp.restore;
        c = d;
      } else {
        sp.kind = 0;
        c = sp;
      }
      return;
    }
    if (sp.v.size() == v.size() && (sp.kind == 1 || sp.kind == -1)) sp.v = v;
    else sp.kind = 0;
    c = sp;
  };
  auto advances = [&](QPDFObjectHandle font) -> std::shared_ptr<std::vector<double>> {
    std::string key = font.isIndirect() ? spaceKey(font) : font.unparse();
    auto it = advCache.find(key);
    if (it != advCache.end()) return it->second;
    auto v = std::make_shared<std::vector<double>>();
    if (!simpleFontAdvances(font, *v)) v.reset();
    advCache[key] = v;
    return v;
  };
  for (size_t i = 0; i < ops.size(); ++i) {
    const Op& o = ops[i];
    const std::string& op = o.op;
    const auto& a = o.args;
    auto n = [&](size_t k) { return k < a.size() ? num(a[k]) : 0.0; };
    QPDFObjectHandle gsd = res.getKey("/ExtGState");
    QPDFObjectHandle xod = res.getKey("/XObject");
    if (op == "q") {
      stack.push_back(st);
    } else if (op == "Q") {
      if (!stack.empty()) {
        st = stack.back();
        stack.pop_back();
      }
    } else if (op == "KuraGB") {
      int depth = 0, paints = 0;
      size_t j = i;
      for (; j < ops.size(); ++j) {
        const std::string& q = ops[j].op;
        if (q == "KuraGB") ++depth;
        if (q == "KuraGE" && --depth == 0) break;
        static const std::set<std::string> kPaint = {"f", "F", "f*", "B", "B*", "b", "b*", "S", "s", "Tj",
                                                     "TJ", "'", "\"", "Do", "sh", "EI"};
        if (kPaint.count(q)) ++paints;
      }
      if (j >= ops.size()) return false;
      if (paints <= 1 && st.bm == 0 && !st.smask) {
        st.gA = st.ca;
        st.CA = st.ca;
        continue;
      }
      if (st.ca < 1 || st.bm != 0 || st.smask) {
        Rect box;
        box.add(st.ctm, n(0), n(1));
        box.add(st.ctm, n(0) + n(2), n(1));
        box.add(st.ctm, n(0), n(1) + n(3));
        box.add(st.ctm, n(0) + n(2), n(1) + n(3));
        if (inText || !region(rectClip(n(0), n(1), n(2), n(3)), box, j)) return false;
        i = j;
        continue;
      }
      st.CA = st.ca;
      st.gA = st.ca;
    } else if (op == "KuraGE") {
    } else if (op == "cm" && a.size() == 6) {
      if (inText && !pendingText.empty()) return false;
      st.ctm = mul({n(0), n(1), n(2), n(3), n(4), n(5)}, st.ctm);
    } else if (op == "w") {
      st.lw = n(0);
    } else if (op == "J") {
      st.cap = static_cast<int>(n(0));
    } else if (op == "j") {
      st.join = static_cast<int>(n(0));
    } else if (op == "M") {
      st.ml = n(0);
    } else if (op == "d" && a.size() == 2) {
      st.dash = a[0].unparseResolved() + " " + a[1].unparseResolved();
    } else if (op == "gs" && !a.empty() && a[0].isName()) {
      QPDFObjectHandle g = gsd.isDictionary() ? gsd.getKey(a[0].getName()) : QPDFObjectHandle::newNull();
      if (!g.isDictionary()) continue;
      if (g.hasKey("/ca")) st.ca = numOf(g.getKey("/ca"), 1) * st.gA;
      if (g.hasKey("/CA")) st.CA = numOf(g.getKey("/CA"), 1) * st.gA;
      if (g.hasKey("/LW")) st.lw = numOf(g.getKey("/LW"), st.lw);
      if (g.hasKey("/LC")) st.cap = static_cast<int>(numOf(g.getKey("/LC"), st.cap));
      if (g.hasKey("/LJ")) st.join = static_cast<int>(numOf(g.getKey("/LJ"), st.join));
      if (g.hasKey("/ML")) st.ml = numOf(g.getKey("/ML"), st.ml);
      QPDFObjectHandle dd = g.getKey("/D");
      if (dd.isArray() && dd.getArrayNItems() == 2) {
        st.dash = dd.getArrayItem(0).unparseResolved() + " " + dd.getArrayItem(1).unparseResolved();
      }
      if (g.hasKey("/BM")) {
        QPDFObjectHandle bm = g.getKey("/BM");
        st.bm = -1;
        if (bm.isArray()) {
          for (const auto& x : bm.aitems()) {
            int m = blendMode(nameOf(x));
            if (m >= 0) {
              st.bm = m;
              break;
            }
          }
        } else {
          st.bm = blendMode(nameOf(bm));
        }
      }
      if (g.hasKey("/SMask")) st.smask = !nameIs(g.getKey("/SMask"), "/None");
    } else if (op == "g" || op == "G") {
      Colour& c = op == "g" ? st.fill : st.stroke;
      c = Colour();
      c.v = {n(0)};
      c.restore = unparseOp(o);
    } else if (op == "rg" || op == "RG") {
      Colour& c = op == "rg" ? st.fill : st.stroke;
      c = device({n(0), n(1), n(2)});
      c.restore = unparseOp(o);
    } else if (op == "k" || op == "K") {
      Colour& c = op == "k" ? st.fill : st.stroke;
      c = device({n(0), n(1), n(2), n(3)});
      c.restore = unparseOp(o);
    } else if ((op == "cs" || op == "CS") && !a.empty() && a[0].isName()) {
      Colour& c = op == "cs" ? st.fill : st.stroke;
      c = colourOf(a[0], res);
      if (!isDevice(c.fam)) c.cs = a[0].getName();
      c.csName = a[0].getName();
      c.csOp = unparseOp(o);
      c.restore = c.csOp;
      if (c.kind == 2 || c.kind == 3) c.kind = 0;
    } else if (op == "sc" || op == "scn" || op == "SC" || op == "SCN") {
      setSc(op[0] == 's' ? st.fill : st.stroke, o);
    } else if (op == "m" || op == "l" || op == "c" || op == "v" || op == "y" || op == "re") {
      if (!inPath) {
        inPath = true;
        pathStart = i;
        path = Rect();
      }
      if (op == "re") {
        path.add(st.ctm, n(0), n(1));
        path.add(st.ctm, n(0) + n(2), n(1) + n(3));
        path.add(st.ctm, n(0) + n(2), n(1));
        path.add(st.ctm, n(0), n(1) + n(3));
      } else {
        for (size_t k = 0; k + 1 < a.size(); k += 2) path.add(st.ctm, n(k), n(k + 1));
      }
    } else if (op == "h") {
    } else if (op == "W" || op == "W*") {
      pendingW = true;
      pendingEo = op == "W*";
    } else if (op == "n" || op == "S" || op == "s" || op == "f" || op == "F" || op == "f*" || op == "B" ||
               op == "B*" || op == "b" || op == "b*") {
      std::vector<Op> pops;
      if (inPath) {
        for (size_t k = pathStart; k < i; ++k) {
          if (isPathOp(ops[k].op)) pops.push_back(ops[k]);
        }
      }
      if (op != "n") {
        bool fill = op != "S" && op != "s";
        bool stroke = op == "S" || op == "s" || op[0] == 'B' || op[0] == 'b';
        if (op == "s" || op == "b" || op == "b*") pops.push_back(Op{{}, "h"});
        size_t base = pieces.size();
        std::string pre, post, extra;
        if (inText && pendingHits({meet(path, st.clipBox)})) return false;
        int ok = 1;
        Shape fs;
        fs.eo = op.back() == '*';
        fs.ops = pops;
        fs.ctm = st.ctm;
        fs.box = path;
        Shape ss;
        ss.kind = 1;
        ss.ops = pops;
        ss.ctm = st.ctm;
        ss.setup = strokeSetup(st);
        if (st.dash.rfind("[]", 0) == 0) ss.outline = strokeOutline(pops, st.ctm, st.lw, st.cap, st.join, st.ml);
        ss.box = path;
        double scale = std::sqrt(std::fabs(st.ctm[0] * st.ctm[3] - st.ctm[1] * st.ctm[2]));
        ss.box.grow(st.lw * scale * (st.join == 0 ? std::max(1.0, st.ml / 2) : 1.0) + 1);
        if (fill) {
          shapes.push_back(fs);
          ok = paintShape(static_cast<int>(shapes.size()) - 1, st.fill, st.ca, false, base, pre, post, extra, false);
        }
        if (ok && stroke) {
          shapes.push_back(ss);
          ok = paintShape(static_cast<int>(shapes.size()) - 1, st.stroke, st.CA, true, base, pre, post, extra,
                          !extra.empty());
        }
        if (ok && !extra.empty() && (pendingW || inText)) ok = 0;
        if (!ok) {
          pieces.resize(base);
          if (pendingW && stroke) return false;
          std::string clip;
          Rect box = stroke ? ss.box : fs.box;
          if (fill && !stroke) clip = pathStr(fs.ops, kId) + (fs.eo ? "W* n\n" : "W n\n");
          else if (stroke && !fill && !ss.outline.empty()) clip = pathStr(ss.outline, kId) + "W n\n";
          if (!region(clip, box, i)) return false;
          replace[i] = "n\n";
        } else {
          before[inPath ? pathStart : i] += pre;
          after[i] += post + extra;
        }
      }
      if (pendingW) {
        Shape s;
        s.eo = pendingEo;
        s.ops = pops;
        s.ctm = st.ctm;
        s.box = path;
        shapes.push_back(s);
        st.clips.push_back(static_cast<int>(shapes.size()) - 1);
        st.clipBox = meet(st.clipBox, path);
      }
      pendingW = false;
      inPath = false;
    } else if (op == "BT") {
      inText = true;
      tmExact = true;
      st.tm = st.tlm = kId;
      textClips.clear();
    } else if (op == "ET") {
      inText = false;
      after[i] += pendingText;
      pendingText.clear();
      pendingVec.clear();
      if (!textClips.empty()) {
        Shape u;
        u.kind = 4;
        u.parts = textClips;
        u.ctm = st.ctm;
        for (int p : textClips) {
          u.box.add(shapes[p].box);
          u.exact = u.exact && shapes[p].exact;
          u.noClip = u.noClip || shapes[p].noClip;
        }
        shapes.push_back(u);
        st.clips.push_back(static_cast<int>(shapes.size()) - 1);
        st.clipBox = meet(st.clipBox, u.box);
        textClips.clear();
      }
    } else if (op == "Tf" && a.size() >= 2 && a[0].isName()) {
      st.fontSize = n(1);
      st.fontName = a[0].getName();
      QPDFObjectHandle fd = res.getKey("/Font");
      st.font = fd.isDictionary() ? fd.getKey(st.fontName) : QPDFObjectHandle::newNull();
    } else if (op == "Tc") {
      st.tc = n(0);
    } else if (op == "Tw") {
      st.tw = n(0);
    } else if (op == "Tz") {
      st.tz = n(0) / 100.0;
    } else if (op == "TL") {
      st.tl = n(0);
    } else if (op == "Ts") {
      st.rise = n(0);
    } else if (op == "Tr") {
      st.tr = static_cast<int>(n(0));
    } else if (op == "Tm" && a.size() == 6) {
      st.tm = st.tlm = {n(0), n(1), n(2), n(3), n(4), n(5)};
      tmExact = true;
    } else if (op == "Td" || op == "TD") {
      if (op == "TD") st.tl = -n(1);
      st.tlm = mul({1, 0, 0, 1, n(0), n(1)}, st.tlm);
      st.tm = st.tlm;
      tmExact = true;
    } else if (op == "T*") {
      st.tlm = mul({1, 0, 0, 1, 0, -st.tl}, st.tlm);
      st.tm = st.tlm;
      tmExact = true;
    } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
      if (!inText || a.empty() || st.tr < 0 || st.tr > 7) return false;
      Op show = o;
      if (op == "'" || op == "\"") {
        if (op == "\"") {
          if (a.size() != 3) return false;
          st.tw = n(0);
          st.tc = n(1);
        }
        show = Op{{a.back()}, "Tj"};
        st.tlm = mul({1, 0, 0, 1, 0, -st.tl}, st.tlm);
        st.tm = st.tlm;
        tmExact = true;
      }
      bool fontOk = st.font.isDictionary();
      QPDFObjectHandle widths = fontOk ? st.font.getKey("/Widths") : QPDFObjectHandle::newNull();
      std::string sub = fontOk ? nameOf(st.font.getKey("/Subtype")) : "";
      bool simple = sub != "/Type0";
      bool type3 = sub == "/Type3";
      double wscale = 0.001;
      if (type3) {
        QPDFObjectHandle fm = st.font.getKey("/FontMatrix");
        wscale = fm.isArray() && fm.getArrayNItems() == 6 ? num(fm.getArrayItem(0)) : 0.001;
      }
      std::shared_ptr<std::vector<double>> adv;
      if (fontOk && simple && !type3 && !widths.isArray()) adv = advances(st.font);
      bool known = fontOk && simple && (widths.isArray() || adv);
      int firstChar = fontOk ? static_cast<int>(numOf(st.font.getKey("/FirstChar"), 0)) : 0;
      QPDFObjectHandle desc = fontOk ? st.font.getKey("/FontDescriptor") : QPDFObjectHandle::newNull();
      double missing = desc.isDictionary() && !type3 ? numOf(desc.getKey("/MissingWidth"), 0) : 0;
      double fs = st.fontSize, tx = 0;
      std::vector<std::pair<double, double>> gx;
      auto glyphs = [&](const std::string& str) {
        for (unsigned char ch : str) {
          double gx0 = tx;
          int k = ch - firstChar;
          double w;
          if (simple && widths.isArray()) {
            w = (k >= 0 && k < widths.getArrayNItems() ? num(widths.getArrayItem(k)) : missing) * wscale;
          } else if (adv) {
            w = (*adv)[ch] / 1000.0;
          } else {
            w = simple ? 0.75 : 0.55;
          }
          tx += (w * fs + st.tc + (simple && ch == 32 ? st.tw : 0)) * st.tz;
          if (!(simple && ch == 32)) gx.push_back({std::min(gx0, gx0 + w * fs * st.tz), std::max(gx0, gx0 + w * fs * st.tz)});
        }
      };
      for (const auto& x : show.args) {
        if (x.isString()) glyphs(x.getStringValue());
        if (x.isArray()) {
          for (const auto& y : QPDFObjectHandle(x).aitems()) {
            if (y.isString()) glyphs(y.getStringValue());
            else if (y.isNumber()) tx -= num(y) / 1000.0 * fs * st.tz;
          }
        }
      }
      double afs = std::fabs(fs);
      Mat trm = mul(st.tm, st.ctm);
      Rect box;
      for (double x : {std::min(0.0, tx) - afs * 0.3, std::max(0.0, tx) + afs * 0.5}) {
        for (double y : {-afs * 0.4, afs * 1.2}) box.add(trm, x, st.rise + y);
      }
      if (type3) {
        Rect b3;
        for (double x : {std::min(0.0, tx) - afs, std::max(0.0, tx) + afs}) {
          for (double y : {-afs, afs * 2}) b3.add(trm, x, st.rise + y);
        }
        box = b3;
      }
      box.grow(1);
      std::vector<Rect> gboxes;
      if (known && tmExact && !type3 && gx.size() <= 4000) {
        for (const auto& [g0, g1] : gx) {
          Rect r;
          for (double x : {g0, g1}) {
            for (double y : {-afs * 0.3, afs * 1.1}) r.add(trm, x, st.rise + y);
          }
          gboxes.push_back(meet(r, st.clipBox));
        }
      } else {
        gboxes.push_back(meet(box, st.clipBox));
      }
      int mode = st.tr & 3;
      bool clipMode = st.tr >= 4;
      Shape s;
      s.kind = 2;
      s.exact = tmExact && !st.fontName.empty();
      s.noClip = type3 || !fontOk;
      s.ops = {show};
      s.ctm = st.ctm;
      s.tm = st.tm;
      s.setup = textSetup(st);
      s.box = box;
      if (clipMode) {
        shapes.push_back(s);
        textClips.push_back(static_cast<int>(shapes.size()) - 1);
      }
      if (mode != 3) {
        if (pendingHits(gboxes)) return false;
        size_t base = pieces.size();
        std::string pre, post, extra;
        Colour fc = st.fill, sc = st.stroke;
        if (type3) {
          std::string key = st.font.isIndirect() ? spaceKey(st.font) : st.font.unparse();
          auto it = t3Cache.find(key);
          bool coloured = it != t3Cache.end() ? it->second : (t3Cache[key] = type3Coloured(st.font));
          if (coloured) {
            fc.kind = 0;
            sc.kind = 0;
          }
        }
        int ok = 1;
        if (mode == 0 || mode == 2) {
          shapes.push_back(s);
          ok = paintShape(static_cast<int>(shapes.size()) - 1, fc, st.ca, false, base, pre, post, extra, false);
        }
        if (ok && (mode == 1 || mode == 2)) {
          Shape t = s;
          t.kind = 3;
          t.setup = strokeSetup(st) + textSetup(st);
          double scale = std::sqrt(std::fabs(st.ctm[0] * st.ctm[3] - st.ctm[1] * st.ctm[2]));
          t.box.grow(st.lw * scale * (st.join == 0 ? std::max(1.0, st.ml / 2) : 1.0));
          box = t.box;
          shapes.push_back(t);
          ok = paintShape(static_cast<int>(shapes.size()) - 1, sc, st.CA, true, base, pre, post, extra, !extra.empty());
        }
        if (!ok) {
          pieces.resize(base);
          Mat ci;
          std::string clip;
          if (mode == 0 && s.clipCost() < 9 && invert(st.ctm, ci)) clip = emitClip(shapes, s, ci);
          if (!region(clip, box, i)) return false;
          before[i] += std::string(clipMode ? "7" : "3") + " Tr\n";
          after[i] += std::to_string(st.tr) + " Tr\n";
        } else {
          before[i] += pre;
          after[i] += post;
          if (!extra.empty()) {
            pendingText += extra;
            if (pendingVec.size() + gboxes.size() > 20000) return false;
            pendingVec.insert(pendingVec.end(), gboxes.begin(), gboxes.end());
          }
        }
      }
      st.tm = mul({1, 0, 0, 1, tx, 0}, st.tm);
      tmExact = tmExact && known;
    } else if (op == "Do" && !a.empty() && a[0].isName()) {
      QPDFObjectHandle x = xod.isDictionary() ? xod.getKey(a[0].getName()) : QPDFObjectHandle::newNull();
      if (!x.isStream()) continue;
      QPDFObjectHandle d = x.getDict();
      bool form = nameIs(d.getKey("/Subtype"), "/Form");
      Rect box;
      std::string clip;
      bool selfT;
      if (form) {
        QPDFObjectHandle bb = d.getKey("/BBox");
        Mat fm = kId;
        QPDFObjectHandle mm = d.getKey("/Matrix");
        if (mm.isArray() && mm.getArrayNItems() == 6) {
          for (int k = 0; k < 6; ++k) fm[k] = num(mm.getArrayItem(k));
        }
        Mat full = mul(fm, st.ctm);
        if (bb.isArray() && bb.getArrayNItems() == 4) {
          double bx[2] = {num(bb.getArrayItem(0)), num(bb.getArrayItem(2))};
          double by[2] = {num(bb.getArrayItem(1)), num(bb.getArrayItem(3))};
          for (double cx : bx) {
            for (double cy : by) box.add(full, cx, cy);
          }
          auto p0 = apply(fm, bx[0], by[0]), p1 = apply(fm, bx[1], by[0]), p2 = apply(fm, bx[1], by[1]),
               p3 = apply(fm, bx[0], by[1]);
          clip = f4(p0[0]) + " " + f4(p0[1]) + " m " + f4(p1[0]) + " " + f4(p1[1]) + " l " + f4(p2[0]) + " " +
                 f4(p2[1]) + " l " + f4(p3[0]) + " " + f4(p3[1]) + " l h W n\n";
        } else {
          box = everywhere();
        }
        QPDFObjectHandle grp = d.getKey("/Group");
        selfT = (grp.isDictionary() && nameIs(grp.getKey("/S"), "/Transparency")) ||
                resourcesHaveTransparency(d.getKey("/Resources"), tmemo);
      } else {
        for (double cx : {0.0, 1.0}) {
          for (double cy : {0.0, 1.0}) box.add(st.ctm, cx, cy);
        }
        clip = "0 0 m 1 0 l 1 1 l 0 1 l h W n\n";
        QPDFObjectHandle sm = d.getKey("/SMask");
        selfT = (!sm.isNull() && !nameIs(sm, "/None")) || numOf(d.getKey("/SMaskInData"), 0) > 0;
      }
      bool axis = std::fabs(st.ctm[1]) < 1e-9 && std::fabs(st.ctm[2]) < 1e-9;
      if (selfT || st.ca < 1 || st.bm != 0 || st.smask) {
        if (inText || !region(clip, box, i, !form && axis)) return false;
        QPDFObjectHandle fr = d.getKey("/Resources");
        if (!form || !(fr.isDictionary() && fr.getKey("/Font").isDictionary())) replace[i] = "";
        continue;
      }
      if (!addBlock(box)) return false;
      if (!form) opaqueImages.push_back({i, meet(box, st.clipBox), st.clips});
    } else if (op == "EI") {
      Rect box;
      for (double cx : {0.0, 1.0}) {
        for (double cy : {0.0, 1.0}) box.add(st.ctm, cx, cy);
      }
      if (st.ca < 1 || st.bm != 0 || st.smask) {
        if (inText || !region("0 0 m 1 0 l 1 1 l 0 1 l h W n\n", box, i)) return false;
        continue;
      }
      if (!addBlock(box)) return false;
    } else if (op == "sh" && !a.empty() && a[0].isName()) {
      if (inText) return false;
      if (st.ca < 1 || st.bm != 0 || st.smask) {
        if (!region("", everywhere(), i)) return false;
        replace[i] = "";
        continue;
      }
      Piece p;
      p.box = st.clipBox;
      QPDFObjectHandle shd = res.getKey("/Shading");
      QPDFObjectHandle sh = shd.isDictionary() ? shd.getKey(a[0].getName()) : QPDFObjectHandle::newNull();
      if (sh.isDictionary()) {
        int type = static_cast<int>(numOf(sh.getKey("/ShadingType"), 0));
        Colour t = colourOf(sh.getKey("/ColorSpace"), res);
        if ((type == 2 || type == 3) && sh.hasKey("/Function") && (t.kind == 1 || t.kind == -1)) {
          if (!isDevice(t.fam)) t.cs = "#shading";
          Shade s;
          s.dict = sh;
          s.ctm = st.ctm;
          s.frame = t;
          shades.push_back(s);
          p.shade = static_cast<int>(shades.size()) - 1;
          p.clips = st.clips;
        }
      }
      pieces.push_back(p);
    } else if (op == "d0" || op == "d1") {
      return false;
    }
  }
  if (!pendingText.empty() || inText) return false;
  {
    std::set<std::string> used;
    for (size_t i = 0; i < ops.size(); ++i) {
      if (ops[i].op != "Do" || ops[i].args.empty() || !ops[i].args[0].isName()) continue;
      auto r = replace.find(i);
      if (r == replace.end() || !r->second.empty()) used.insert(ops[i].args[0].getName());
    }
    QPDFObjectHandle xd = res.getKey("/XObject");
    if (xd.isDictionary()) {
      for (const std::string& nm : xd.getKeys()) {
        if (!used.count(nm)) xd.removeKey(nm);
      }
    }
  }
  neutraliseTransparency(ctx, res);
  QPDFObjectHandle origRes = page.getKey("/Resources");
  QPDFObjectHandle origContents = page.getKey("/Contents");
  std::string out;
  for (size_t i = 0; i < ops.size(); ++i) {
    auto b = before.find(i);
    if (b != before.end()) out += b->second;
    auto r = replace.find(i);
    if (r != replace.end()) out += r->second;
    else if (ops[i].op.rfind("Kura", 0) != 0) out += unparseOp(ops[i]);
    auto af = after.find(i);
    if (af != after.end()) out += af->second;
  }
  if (!crops.empty()) {
    double sum = 0;
    std::array<int, 4> uni = crops[0];
    for (const auto& c : crops) {
      sum += static_cast<double>(c[2] - c[0]) * (c[3] - c[1]);
      uni = {std::min(uni[0], c[0]), std::min(uni[1], c[1]), std::max(uni[2], c[2]), std::max(uni[3], c[3])};
    }
    bool shared = sum > 1.5 * static_cast<double>(uni[2] - uni[0]) * (uni[3] - uni[1]);
    std::vector<std::string> place(crops.size());
    std::string sharedName;
    for (size_t k = 0; k < crops.size(); ++k) {
      std::array<int, 4> c = shared ? uni : crops[k];
      if (!shared || sharedName.empty()) {
        int cw = c[2] - c[0], chh = c[3] - c[1];
        std::string data(static_cast<size_t>(cw) * chh * 3, '\0');
        for (int y = 0; y < chh; ++y) {
          std::copy_n(rnd.rgb.data() + (static_cast<size_t>(c[1] + y) * rnd.w + c[0]) * 3, static_cast<size_t>(cw) * 3,
                      &data[static_cast<size_t>(y) * cw * 3]);
        }
        QPDFObjectHandle img = QPDFObjectHandle::newStream(&ctx.pdf, data);
        QPDFObjectHandle id = img.getDict();
        id.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
        id.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
        id.replaceKey("/Width", QPDFObjectHandle::newInteger(cw));
        id.replaceKey("/Height", QPDFObjectHandle::newInteger(chh));
        id.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
        id.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
        sharedName = env.add("/XObject", "KuraRgn", ctx.pdf.makeIndirectObject(img));
      }
      auto p00 = apply(rnd.dinv, c[0], c[3]), p10 = apply(rnd.dinv, c[2], c[3]), p01 = apply(rnd.dinv, c[0], c[1]);
      Mat m{p10[0] - p00[0], p10[1] - p00[1], p01[0] - p00[0], p01[1] - p00[1], p00[0], p00[1]};
      place[k] = matStr(m) + "cm " + sharedName + " Do";
    }
    std::string fixed;
    fixed.reserve(out.size());
    size_t p = 0;
    for (size_t k = 0; k < place.size(); ++k) {
      std::string token = "\x01KuraRgnTok" + std::to_string(k) + "\x01";
      size_t at = out.find(token, p);
      if (at == std::string::npos) return false;
      fixed.append(out, p, at - p);
      fixed += place[k];
      p = at + token.size();
    }
    fixed.append(out, p, std::string::npos);
    out.swap(fixed);
  }
  page.replaceKey("/Resources", res);
  page.replaceKey("/Contents", ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, out)));
  if (resourcesHaveTransparency(page.getKey("/Resources"))) {
    page.replaceKey("/Resources", origRes);
    page.replaceKey("/Contents", origContents);
    return false;
  }
  if (regionsOut) *regionsOut = regions;
  return true;
}
}
