#include <lcms2.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
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

#include "assets/cmyk_icc.hh"
#include "assets/srgb_icc.hh"
#include "colorconv.hh"
#include "ctx.hh"
#include "images.hh"
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
using Pt = std::array<double, 2>;
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
  QPDFObjectHandle smaskObj;
  Mat smaskCtm = kId;
  bool op = false, OP = false;
  int opm = 0;
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
  int image = -1;
  bool stencil = false;
  std::string xobj;
  Mat xm = kId;
  Colour colour;
  double alpha = 1;
  bool flat = false;
  Rect box;
};

struct Layer {
  Colour src;
  double a = 1;
  int bm = 0;
  bool ovp = false;
  int opm = 0;
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

bool inksUnder(const Piece& back, Colour& src, Colour& bc, Env& env) {
  if (src.kind >= 0 || src.names.size() != src.v.size()) return false;
  bc = back.colour;
  for (double& x : bc.v) x = bc.kind > 0 ? back.alpha * x + (1 - back.alpha) : back.alpha * x;
  if (bc.kind == 0 && bc.fam == "/Separation" && bc.space.isArray() && nameOf(bc.space.getArrayItem(1)) == "/All") {
    bc = device(std::vector<double>(4, bc.v.empty() ? 1 : bc.v[0]));
  }
  if ((bc.kind >= 0 || bc.names.empty()) && !toProcess(bc, 4)) return false;
  return mergeInks(bc, src, env) && bc.v.size() == src.v.size();
}

QPDFObjectHandle sampledFunction(Ctx& ctx, const std::string& data, double d0, double d1, size_t on) {
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
  return ctx.pdf.makeIndirectObject(f);
}

bool hasForeignInk(const Colour& under, const std::vector<std::string>& over) {
  for (size_t k = 0; k < under.names.size() && k < under.v.size(); ++k) {
    if (under.v[k] > 0 && std::find(over.begin(), over.end(), under.names[k]) == over.end()) return true;
  }
  return false;
}

struct ShadeGeo {
  int type = 0;
  std::array<double, 6> c{};
  bool e0 = false, e1 = false;
  double d0 = 0, d1 = 1;
  std::unique_ptr<PdfFunction> fn;
  Mat inv = kId;
};

bool loadGeo(QPDFObjectHandle sd, const Mat& ctm, ShadeGeo& g) {
  g.type = static_cast<int>(numOf(sd.getKey("/ShadingType"), 0));
  QPDFObjectHandle co = sd.getKey("/Coords"), ex = sd.getKey("/Extend"), dom = sd.getKey("/Domain");
  size_t want = g.type == 2 ? 4 : 6;
  if ((g.type != 2 && g.type != 3) || !co.isArray() || static_cast<size_t>(co.getArrayNItems()) != want || !invert(ctm, g.inv)) {
    return false;
  }
  for (size_t k = 0; k < want; ++k) g.c[k] = num(co.getArrayItem(static_cast<int>(k)));
  if (ex.isArray() && ex.getArrayNItems() == 2) {
    g.e0 = ex.getArrayItem(0).isBool() && ex.getArrayItem(0).getBoolValue();
    g.e1 = ex.getArrayItem(1).isBool() && ex.getArrayItem(1).getBoolValue();
  }
  if (dom.isArray() && dom.getArrayNItems() == 2) {
    g.d0 = num(dom.getArrayItem(0));
    g.d1 = num(dom.getArrayItem(1));
  }
  g.fn = PdfFunction::loadArray(sd.getKey("/Function"));
  return g.fn && g.fn->inputs() == 1;
}

bool shadeAt(const ShadeGeo& g, double x, double y, double* out) {
  auto p = apply(g.inv, x, y);
  double s = 0;
  auto fits = [&](double v) { return (v >= 0 || g.e0) && (v <= 1 || g.e1); };
  if (g.type == 2) {
    double dx = g.c[2] - g.c[0], dy = g.c[3] - g.c[1], den = dx * dx + dy * dy;
    s = den > 0 ? ((p[0] - g.c[0]) * dx + (p[1] - g.c[1]) * dy) / den : 0;
    if (!fits(s)) return false;
  } else {
    double cx = g.c[3] - g.c[0], cy = g.c[4] - g.c[1], dr = g.c[5] - g.c[2];
    double px = p[0] - g.c[0], py = p[1] - g.c[1];
    double a = cx * cx + cy * cy - dr * dr, b = px * cx + py * cy + g.c[2] * dr, c = px * px + py * py - g.c[2] * g.c[2];
    std::vector<double> cand;
    if (std::fabs(a) < 1e-12) {
      if (std::fabs(b) < 1e-12) return false;
      cand.push_back(c / (2 * b));
    } else {
      double disc = b * b - a * c;
      if (disc < 0) return false;
      double r = std::sqrt(disc);
      cand = {std::max((b + r) / a, (b - r) / a), std::min((b + r) / a, (b - r) / a)};
    }
    bool found = false;
    for (double v : cand) {
      if (fits(v) && g.c[2] + v * dr >= 0) {
        s = v;
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  double t = g.d0 + std::clamp(s, 0.0, 1.0) * (g.d1 - g.d0);
  return g.fn->compute(&t, out);
}

struct CidWidths {
  double dw = 1000;
  std::map<unsigned, double> w;
};

std::shared_ptr<CidWidths> cidWidths(QPDFObjectHandle font) {
  if (!nameIs(font.getKey("/Encoding"), "/Identity-H")) return nullptr;
  QPDFObjectHandle df = font.getKey("/DescendantFonts");
  QPDFObjectHandle cf = df.isArray() && df.getArrayNItems() > 0 ? df.getArrayItem(0) : QPDFObjectHandle::newNull();
  if (!cf.isDictionary()) return nullptr;
  auto r = std::make_shared<CidWidths>();
  r->dw = numOf(cf.getKey("/DW"), 1000);
  QPDFObjectHandle wa = cf.getKey("/W");
  if (!wa.isArray()) return r;
  std::vector<QPDFObjectHandle> v = wa.getArrayAsVector();
  for (size_t i = 0; i + 1 < v.size();) {
    if (!v[i].isNumber()) return nullptr;
    unsigned c0 = static_cast<unsigned>(num(v[i]));
    if (v[i + 1].isArray()) {
      unsigned k = 0;
      for (const auto& x : v[i + 1].aitems()) r->w[c0 + k++] = num(x);
      i += 2;
    } else if (i + 2 < v.size() && v[i + 1].isNumber()) {
      unsigned c1 = static_cast<unsigned>(num(v[i + 1]));
      if (c1 < c0 || c1 - c0 > 65535) return nullptr;
      for (unsigned c = c0; c <= c1; ++c) r->w[c] = num(v[i + 2]);
      i += 3;
    } else {
      return nullptr;
    }
  }
  return r;
}

bool type3Outline(QPDFObjectHandle font, const std::vector<std::pair<unsigned char, double>>& codes, const Mat& text,
                  std::vector<Op>& out) {
  QPDFObjectHandle procs = font.getKey("/CharProcs"), fm = font.getKey("/FontMatrix"), enc = font.getKey("/Encoding");
  if (!procs.isDictionary() || !fm.isArray() || fm.getArrayNItems() != 6) return false;
  Mat fmat;
  for (int k = 0; k < 6; ++k) fmat[k] = num(fm.getArrayItem(k));
  std::map<int, std::string> names;
  QPDFObjectHandle diff = enc.isDictionary() ? enc.getKey("/Differences") : QPDFObjectHandle::newNull();
  if (diff.isArray()) {
    int code = 0;
    for (const auto& x : diff.aitems()) {
      if (x.isNumber()) code = static_cast<int>(num(x));
      else if (x.isName()) names[code++] = x.getName();
    }
  }
  for (const auto& [ch, gx0] : codes) {
    auto nm = names.find(ch);
    if (nm == names.end()) return false;
    QPDFObjectHandle proc = procs.getKey(nm->second);
    if (!proc.isStream()) return false;
    Collect c;
    try {
      QPDFObjectHandle::parseContentStream(proc, &c);
    } catch (...) {
      return false;
    }
    Mat base = mul(fmat, mul({1, 0, 0, 1, gx0, 0}, text));
    std::vector<Mat> stack;
    Mat m = base;
    std::vector<Op> path;
    std::map<std::string, QPDFObjectHandle> inl;
    Pt cur{0, 0};
    auto pt = [&](const Op& o, size_t k) { return Pt{num(o.args[k]), num(o.args[k + 1])}; };
    auto put = [&](const char* name, std::initializer_list<Pt> pts) {
      std::vector<double> v;
      for (const Pt& q : pts) {
        auto t = apply(m, q[0], q[1]);
        v.push_back(t[0]);
        v.push_back(t[1]);
      }
      Op o;
      o.op = name;
      for (double x : v) o.args.push_back(QPDFObjectHandle::newReal(x, 4));
      path.push_back(o);
    };
    for (const Op& o : c.ops) {
      const std::string& op = o.op;
      size_t na = o.args.size();
      if (op == "q") {
        stack.push_back(m);
      } else if (op == "Q") {
        if (!stack.empty()) {
          m = stack.back();
          stack.pop_back();
        }
      } else if (op == "cm" && na == 6) {
        m = mul({num(o.args[0]), num(o.args[1]), num(o.args[2]), num(o.args[3]), num(o.args[4]), num(o.args[5])}, m);
      } else if (op == "m" && na == 2) {
        cur = pt(o, 0);
        put("m", {cur});
      } else if (op == "l" && na == 2) {
        cur = pt(o, 0);
        put("l", {cur});
      } else if (op == "c" && na == 6) {
        put("c", {pt(o, 0), pt(o, 2), pt(o, 4)});
        cur = pt(o, 4);
      } else if (op == "v" && na == 4) {
        put("c", {cur, pt(o, 0), pt(o, 2)});
        cur = pt(o, 2);
      } else if (op == "y" && na == 4) {
        put("c", {pt(o, 0), pt(o, 2), pt(o, 2)});
        cur = pt(o, 2);
      } else if (op == "h") {
        path.push_back(Op{{}, "h"});
      } else if (op == "re" && na == 4) {
        double x = num(o.args[0]), y = num(o.args[1]), w = num(o.args[2]), hh = num(o.args[3]);
        put("m", {{x, y}});
        put("l", {{x + w, y}});
        put("l", {{x + w, y + hh}});
        put("l", {{x, y + hh}});
        path.push_back(Op{{}, "h"});
        cur = {x, y};
      } else if (op == "f" || op == "F" || op == "f*" || op == "b" || op == "b*" || op == "B" || op == "B*") {
        out.insert(out.end(), path.begin(), path.end());
        path.clear();
      } else if (op == "n" || op == "W" || op == "W*") {
        if (op == "n") path.clear();
      } else if (op == "ID") {
        inl.clear();
        for (size_t k = 0; k + 1 < na; k += 2) {
          if (o.args[k].isName()) inl[o.args[k].getName()] = o.args[k + 1];
        }
      } else if (op == "EI" && na == 1 && o.args[0].isInlineImage()) {
        auto key = [&](const char* a, const char* b) {
          return inl.count(a) ? inl[a] : inl.count(b) ? inl[b] : QPDFObjectHandle::newNull();
        };
        QPDFObjectHandle im = key("/IM", "/ImageMask"), dec = key("/D", "/Decode");
        int w = static_cast<int>(numOf(key("/W", "/Width"), 0)), h = static_cast<int>(numOf(key("/H", "/Height"), 0));
        if (!im.isBool() || !im.getBoolValue() || !key("/F", "/Filter").isNull() || w <= 0 || h <= 0 || w * h > 1 << 20) {
          return false;
        }
        bool paintOne = dec.isArray() && dec.getArrayNItems() == 2 && num(dec.getArrayItem(0)) == 1;
        std::string bits = o.args[0].getInlineImageValue();
        size_t row = (static_cast<size_t>(w) + 7) / 8;
        if (bits.size() < row * h) return false;
        std::map<std::pair<int, int>, int> open;
        auto flush = [&](int x0, int x1, int top, int bottom) {
          double fx0 = static_cast<double>(x0) / w, fx1 = static_cast<double>(x1) / w;
          double fy0 = 1 - static_cast<double>(bottom) / h, fy1 = 1 - static_cast<double>(top) / h;
          put("m", {{fx0, fy0}});
          put("l", {{fx1, fy0}});
          put("l", {{fx1, fy1}});
          put("l", {{fx0, fy1}});
          out.insert(out.end(), path.begin(), path.end());
          out.push_back(Op{{}, "h"});
          path.clear();
        };
        for (int y = 0; y <= h; ++y) {
          std::map<std::pair<int, int>, int> next;
          for (int x = 0; y < h && x < w;) {
            auto on = [&](int xx) {
              bool b = (static_cast<unsigned char>(bits[y * row + xx / 8]) >> (7 - xx % 8)) & 1;
              return b == paintOne;
            };
            if (!on(x)) {
              ++x;
              continue;
            }
            int x0 = x;
            while (x < w && on(x)) ++x;
            auto it = open.find({x0, x});
            next[{x0, x}] = it != open.end() ? it->second : y;
          }
          for (const auto& [run, top] : open) {
            if (!next.count(run) || next[run] != top) flush(run.first, run.second, top, y);
          }
          open.swap(next);
        }
      } else if (op == "S" || op == "s" || op == "Do" || op == "EI" || op == "sh" || op == "BT" || op == "Tj" ||
                 op == "TJ") {
        return false;
      }
    }
  }
  return true;
}

bool coveredBy(const Colour& c, const std::vector<std::string>& inks) {
  auto has = [&](const std::string& n) { return std::find(inks.begin(), inks.end(), n) != inks.end(); };
  if (c.kind < 0) return std::all_of(c.names.begin(), c.names.end(), has);
  if (c.kind == 1 && c.v.size() == 1) return has("/Black");
  return c.kind == 1 && std::all_of(kProc.begin(), kProc.end(), has);
}

bool paintedInks(Colour& src, int opm, std::set<std::string>& painted) {
  if (src.kind == 1) {
    if (!toProcess(src, 4)) return false;
    opm = 0;
  }
  if (src.kind >= 0) return false;
  bool cmyk = src.names == kProc && src.fam != "/DeviceN";
  for (size_t i = 0; i < src.names.size() && i < src.v.size(); ++i) {
    if (!(opm == 1 && cmyk && src.v[i] == 0)) painted.insert(src.names[i]);
  }
  return true;
}

bool imageSamples(QPDFObjectHandle x, QPDFObjectHandle res, Colour& cs, int& w, int& h, std::string& data, bool soft = false,
                  std::string* alphaOut = nullptr);

bool softAlpha(QPDFObjectHandle sm, int w, int h, std::string& alpha) {
  if (!sm.isStream() || sm.getDict().hasKey("/Matte")) return false;
  Colour cs;
  int sw, sh;
  std::string g;
  if (!imageSamples(sm, QPDFObjectHandle::newDictionary(), cs, sw, sh, g) || cs.names != kProc) return false;
  alpha.resize(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      size_t q = static_cast<size_t>(y * sh / h) * sw + static_cast<size_t>(x) * sw / w;
      alpha[static_cast<size_t>(y) * w + x] = static_cast<char>(255 - static_cast<unsigned char>(g[q * 4 + 3]));
    }
  }
  return true;
}

bool imageSamplesRaw(QPDFObjectHandle x, QPDFObjectHandle res, Colour& cs, int& w, int& h, std::string& data, bool soft);

bool imageSamples(QPDFObjectHandle x, QPDFObjectHandle res, Colour& cs, int& w, int& h, std::string& data, bool soft,
                  std::string* alphaOut) {
  if (!imageSamplesRaw(x, res, cs, w, h, data, soft)) return false;
  QPDFObjectHandle sm = x.getDict().getKey("/SMask");
  if (!soft || !sm.isStream()) return true;
  std::string alpha;
  if (!softAlpha(sm, w, h, alpha)) return false;
  size_t n = cs.names.size(), px = static_cast<size_t>(w) * h;
  for (size_t q = 0; q < px; ++q) {
    unsigned a = static_cast<unsigned char>(alpha[q]);
    for (size_t k = 0; k < n; ++k) {
      data[q * n + k] = static_cast<char>(static_cast<unsigned char>(data[q * n + k]) * a / 255);
    }
  }
  if (alphaOut) alphaOut->swap(alpha);
  return true;
}

bool imageSamplesRaw(QPDFObjectHandle x, QPDFObjectHandle res, Colour& cs, int& w, int& h, std::string& data, bool soft) {
  QPDFObjectHandle d = x.getDict();
  cs = colourOf(d.getKey("/ColorSpace"), res);
  w = static_cast<int>(numOf(d.getKey("/Width"), 0));
  h = static_cast<int>(numOf(d.getKey("/Height"), 0));
  int bpc = static_cast<int>(numOf(d.getKey("/BitsPerComponent"), 0));
  bool plainDecode = true;
  QPDFObjectHandle dec = d.getKey("/Decode");
  if (dec.isArray()) {
    Colour probe = colourOf(d.getKey("/ColorSpace"), res);
    for (int k = 0; k < dec.getArrayNItems(); ++k) {
      double want = k % 2 == 0 ? 0 : probe.kind == 2 ? (1 << bpc) - 1 : 1;
      if (std::fabs(num(dec.getArrayItem(k)) - want) > 1e-6) plainDecode = false;
    }
  } else if (!dec.isNull()) {
    plainDecode = false;
  }
  if ((d.hasKey("/SMask") && !soft) || d.hasKey("/Mask") || !plainDecode || (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8) ||
      w <= 0 || h <= 0 || static_cast<double>(w) * h > 4e7) {
    return false;
  }
  std::string lookup;
  if (cs.kind == 2 && cs.space.isArray()) {
    QPDFObjectHandle lk = cs.space.getArrayItem(3);
    try {
      if (lk.isString()) {
        lookup = lk.getStringValue();
      } else if (lk.isStream()) {
        auto b = lk.getStreamData(qpdf_dl_generalized);
        lookup.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
      }
    } catch (...) {
      return false;
    }
    cs = colourOf(cs.space.getArrayItem(1), res);
    if (lookup.empty()) return false;
  }
  bool gray = cs.kind == 1 && cs.v.size() == 1 && cs.cs.empty() && cs.fam == "/DeviceGray";
  bool rgb = cs.kind == 1 && cs.v.size() == 3;
  if (cs.kind == 0 && cs.fam == "/DeviceN" && !cs.names.empty() &&
      std::find(cs.names.begin(), cs.names.end(), "/All") == cs.names.end()) {
    cs.kind = -1;
  }
  size_t n = gray ? 1 : rgb ? 3 : cs.names.size();
  if ((cs.kind >= 0 && !gray && !rgb) || n == 0 || (!lookup.empty() && lookup.size() < n)) return false;
  size_t px = static_cast<size_t>(w) * h, per = lookup.empty() ? n : 1;
  std::string unpacked;
  const unsigned char* sp = nullptr;
  std::shared_ptr<Buffer> buf;
  try {
    buf = x.getStreamData(qpdf_dl_all);
    sp = buf->getBuffer();
  } catch (...) {
    RawImage ri = decodeImage(x);
    if (!ri.ok || ri.width != w || ri.height != h || static_cast<size_t>(ri.comps) != per || ri.samples.size() < px * per) {
      return false;
    }
    unpacked.swap(ri.samples);
    sp = reinterpret_cast<const unsigned char*>(unpacked.data());
    bpc = 8;
  }
  if (!buf) {
  } else if (bpc != 8) {
    size_t row = (static_cast<size_t>(w) * per * bpc + 7) / 8;
    if (buf->getSize() < row * h) return false;
    unpacked.resize(px * per);
    int mx = (1 << bpc) - 1;
    for (size_t y = 0; y < static_cast<size_t>(h); ++y) {
      for (size_t k = 0; k < static_cast<size_t>(w) * per; ++k) {
        size_t bit = k * bpc;
        int v = (sp[y * row + bit / 8] >> (8 - bpc - bit % 8)) & mx;
        unpacked[y * w * per + k] = static_cast<char>(lookup.empty() ? v * 255 / mx : v);
      }
    }
    sp = reinterpret_cast<const unsigned char*>(unpacked.data());
  } else if (buf->getSize() < px * per) {
    return false;
  }
  if (gray) {
    data.assign(px * 4, '\0');
    for (size_t q = 0; q < px; ++q) {
      unsigned char g = lookup.empty() ? sp[q] : static_cast<unsigned char>(lookup[std::min<size_t>(sp[q], lookup.size() - 1)]);
      data[q * 4 + 3] = static_cast<char>(255 - g);
    }
    cs = device({0, 0, 0, 0});
    return true;
  }
  if (lookup.empty()) {
    data.assign(reinterpret_cast<const char*>(sp), px * n);
  } else {
    size_t entries = lookup.size() / n;
    data.resize(px * n);
    for (size_t q = 0; q < px; ++q) std::copy_n(lookup.data() + std::min<size_t>(sp[q], entries - 1) * n, n, &data[q * n]);
  }
  if (rgb) {
    std::string icc;
    if (cs.fam == "/ICCBased" && cs.space.isArray() && cs.space.getArrayItem(1).isStream()) {
      try {
        auto b = cs.space.getArrayItem(1).getStreamData(qpdf_dl_generalized);
        icc.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
      } catch (...) {
        icc.clear();
      }
    }
    cmsHPROFILE in = icc.empty() ? cmsOpenProfileFromMem(kSrgbIcc, kSrgbIccLen)
                                 : cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size()));
    if (!in) in = cmsOpenProfileFromMem(kSrgbIcc, kSrgbIccLen);
    cmsHPROFILE out = cmsOpenProfileFromMem(kCmykIcc, kCmykIccLen);
    cmsHTRANSFORM xf = in && out ? cmsCreateTransform(in, TYPE_RGB_8, out, TYPE_CMYK_8, INTENT_RELATIVE_COLORIMETRIC, 0) : nullptr;
    if (in) cmsCloseProfile(in);
    if (out) cmsCloseProfile(out);
    if (!xf) return false;
    std::string cmyk(px * 4, '\0');
    cmsDoTransform(xf, data.data(), cmyk.data(), static_cast<cmsUInt32Number>(px));
    cmsDeleteTransform(xf);
    data.swap(cmyk);
    cs = device({0, 0, 0, 0});
    return true;
  }
  cs.v.assign(n, 0);
  return true;
}

std::optional<Piece> overprintOn(const Piece& back, Colour src, int opm, Env& env, double a = 1, int bm = 0) {
  if (bm < 0 || bm >= 12) return std::nullopt;
  std::set<std::string> painted;
  if (!paintedInks(src, opm, painted)) return std::nullopt;
  Colour bc;
  if (!inksUnder(back, src, bc, env)) return std::nullopt;
  Piece r;
  r.colour = src;
  for (size_t i = 0; i < src.names.size(); ++i) {
    if (!painted.count(src.names[i])) r.colour.v[i] = bc.v[i];
    else if (bm) r.colour.v[i] = 1 - blendF(bm, 1 - bc.v[i], 1 - r.colour.v[i]);
    r.colour.v[i] = a * r.colour.v[i] + (1 - a) * bc.v[i];
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

std::vector<Op> strokeOutline(const std::vector<Op>& ops, const Mat& ctm, double lw, int cap, int join, double ml,
                              double minWidth = 1) {
  double scale = std::sqrt(std::fabs(ctm[0] * ctm[3] - ctm[1] * ctm[2]));
  std::vector<Op> out;
  if (lw * scale < minWidth || lw <= 0) return out;
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
    if (x.isIndirect() && env.ctx.keepWholeForms.count(x.getObjGen())) return false;
    QPDFObjectHandle d = x.getDict();
    if (!nameIs(d.getKey("/Subtype"), "/Form")) return false;
    for (const char* k : {"/StructParents", "/StructParent", "/OC", "/Ref", "/OPI"}) {
      if (d.hasKey(k)) return false;
    }
    QPDFObjectHandle bb = d.getKey("/BBox");
    if (!bb.isArray() || bb.getArrayNItems() != 4) return false;
    QPDFObjectHandle g = d.getKey("/Group");
    if (env.ctx.flattenOverprint && !g.isNull()) {
      std::set<QPDFObjGen> seen;
      bool isolated = g.isDictionary() && g.getKey("/I").isBool() && g.getKey("/I").getBoolValue();
      if (isolated || !resourcesOverprint(d.getKey("/Resources"), seen)) {
        return false;
      }
    }
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
        std::set<QPDFObjGen> seen;
        if (env.ctx.flattenOverprint && depth < 6 && x.isStream() && nameIs(x.getDict().getKey("/Subtype"), "/Form") &&
            x.getDict().getKey("/BBox").isArray() && x.getDict().getKey("/BBox").getArrayNItems() == 4 &&
            !resourcesOverprint(x.getDict().getKey("/Resources"), seen)) {
          size_t mark = out.size();
          out.push_back(Op{{o.args[0]}, "KuraFB"});
          if (expand(x, ren, depth, out)) {
            out.push_back(Op{{}, "KuraFE"});
            continue;
          }
          out.resize(mark);
        }
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

bool renderSoftMask(Ctx& ctx, QPDFObjectHandle sm, const Mat& ctm, const Rect& bb, int& w, int& h, std::string& alpha) {
  if (!ctx.opt.renderPdf || !sm.isDictionary() || !bb.valid()) return false;
  QPDFObjectHandle g = sm.getKey("/G"), tr = sm.getKey("/TR");
  bool lum = nameIs(sm.getKey("/S"), "/Luminosity");
  if (!g.isStream() || (!lum && !nameIs(sm.getKey("/S"), "/Alpha")) || (!tr.isNull() && !nameIs(tr, "/Identity"))) return false;
  double area = (bb.x1 - bb.x0) * (bb.y1 - bb.y0);
  if (area <= 0) return false;
  double dpi = std::min(300.0, 72.0 * std::sqrt(4e6 / area));
  auto render = [&](const std::string& backdrop, std::string& rgb) {
    try {
      QPDF one;
      one.emptyPDF();
      QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
      page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
      QPDFObjectHandle mb = QPDFObjectHandle::newArray();
      for (double v : {bb.x0, bb.y0, bb.x1, bb.y1}) mb.appendItem(QPDFObjectHandle::newReal(v, 4));
      page.replaceKey("/MediaBox", mb);
      QPDFObjectHandle res = QPDFObjectHandle::newDictionary(), xo = QPDFObjectHandle::newDictionary();
      xo.replaceKey("/M", one.copyForeignObject(g));
      res.replaceKey("/XObject", xo);
      QPDFObjectHandle grp = g.getDict().getKey("/Group");
      QPDFObjectHandle gcs = grp.isDictionary() ? grp.getKey("/CS") : QPDFObjectHandle();
      if (!gcs.isNull() && gcs.isInitialized()) {
        QPDFObjectHandle csd = QPDFObjectHandle::newDictionary();
        csd.replaceKey("/B", gcs.isIndirect() ? one.copyForeignObject(gcs) : gcs.unsafeShallowCopy());
        res.replaceKey("/ColorSpace", csd);
      }
      page.replaceKey("/Resources", res);
      std::string c = "q " + backdrop + f4(bb.x0) + " " + f4(bb.y0) + " " + f4(bb.x1 - bb.x0) + " " + f4(bb.y1 - bb.y0) +
                      " re f Q q " + matStr(ctm) + "cm /M Do Q\n";
      page.replaceKey("/Contents", one.makeIndirectObject(QPDFObjectHandle::newStream(&one, c)));
      QPDFPageDocumentHelper(one).addPage(QPDFPageObjectHelper(one.makeIndirectObject(page)), false);
      QPDFWriter wr(one);
      wr.setOutputMemory();
      wr.write();
      auto buf = wr.getBufferSharedPointer();
      std::string bytes(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
      return ctx.opt.renderPdf(bytes, dpi, w, h, rgb);
    } catch (...) {
      return false;
    }
  };
  std::string a, b;
  if (lum) {
    QPDFObjectHandle bc = sm.getKey("/BC");
    std::string back = "0 g ";
    if (bc.isArray() && bc.getArrayNItems() > 0 && sm.getKey("/G").getDict().getKey("/Group").isDictionary()) {
      back = "/B cs ";
      for (const auto& v : bc.aitems()) back += f4(num(v)) + " ";
      back += "sc ";
    }
    if (!render(back, a)) return false;
    alpha.resize(static_cast<size_t>(w) * h);
    for (size_t q = 0; q < alpha.size(); ++q) {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(a.data()) + q * 3;
      alpha[q] = static_cast<char>(std::clamp(static_cast<int>(std::lround(0.3 * p[0] + 0.59 * p[1] + 0.11 * p[2])), 0, 255));
    }
    return true;
  }
  int w2 = 0, h2 = 0;
  if (!render("1 g ", a)) return false;
  w2 = w;
  h2 = h;
  if (!render("0 g ", b) || w != w2 || h != h2) return false;
  alpha.resize(static_cast<size_t>(w) * h);
  for (size_t q = 0; q < alpha.size(); ++q) {
    int d = 0;
    for (int k = 0; k < 3; ++k) d += static_cast<unsigned char>(a[q * 3 + k]) - static_cast<unsigned char>(b[q * 3 + k]);
    alpha[q] = static_cast<char>(std::clamp(255 - d / 3, 0, 255));
  }
  return true;
}

bool renderCurrentPage(Ctx& ctx, QPDFPageObjectHelper& ph, double dpi, int& w, int& h, std::string& rgb) {
  try {
    QPDFObjectHandle copy = ph.getObjectHandle().shallowCopy();
    for (const char* k : {"/MediaBox", "/CropBox", "/Rotate", "/Resources"}) {
      QPDFObjectHandle v = ph.getAttribute(k, false);
      if (!v.isNull()) copy.replaceKey(k, v);
    }
    copy.removeKey("/Annots");
    copy.removeKey("/Parent");
    copy.removeKey("/StructParents");
    QPDF one;
    one.emptyPDF();
    QPDFPageDocumentHelper(one).addPage(QPDFPageObjectHelper(one.copyForeignObject(ctx.pdf.makeIndirectObject(copy))), false);
    QPDFWriter wr(one);
    wr.setOutputMemory();
    wr.write();
    auto buf = wr.getBufferSharedPointer();
    std::string bytes(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    return ctx.opt.renderPdf(bytes, dpi, w, h, rgb);
  } catch (...) {
    return false;
  }
}

bool resourcesOverprint(QPDFObjectHandle res, std::set<QPDFObjGen>& seen, int depth) {
  if (!res.isDictionary() || depth > 12) return false;
  if (res.isIndirect() && !seen.insert(res.getObjGen()).second) return false;
  QPDFObjectHandle egs = res.getKey("/ExtGState");
  if (egs.isDictionary()) {
    for (const std::string& k : egs.getKeys()) {
      QPDFObjectHandle g = egs.getKey(k);
      if (!g.isDictionary()) continue;
      for (const char* key : {"/OP", "/op"}) {
        if (g.getKey(key).isBool() && g.getKey(key).getBoolValue()) return true;
      }
    }
  }
  for (const char* cat : {"/XObject", "/Pattern"}) {
    QPDFObjectHandle d = res.getKey(cat);
    if (!d.isDictionary()) continue;
    for (const std::string& k : d.getKeys()) {
      QPDFObjectHandle x = d.getKey(k);
      if (x.isStream() && resourcesOverprint(x.getDict().getKey("/Resources"), seen, depth + 1)) return true;
    }
  }
  return false;
}

bool flattenFormOverprint(Ctx& ctx, QPDFObjectHandle form) {
  static thread_local int depth = 0;
  QPDFObjectHandle d = form.getDict();
  QPDFObjectHandle bb = d.getKey("/BBox");
  if (depth >= 12 || !bb.isArray()) return false;
  QPDFObjectHandle res = d.getKey("/Resources");
  QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
  page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
  page.replaceKey("/MediaBox", bb);
  page.replaceKey("/Contents", form);
  page.replaceKey("/Resources", res.isDictionary() ? res : QPDFObjectHandle::newDictionary());
  QPDFPageObjectHelper ph(page);
  ++depth;
  bool ok = false;
  try {
    ok = vectorFlattenPage(ctx, ph, -1, nullptr, 0);
  } catch (...) {
    ok = false;
  }
  --depth;
  if (!ok) return false;
  QPDFObjectHandle nc = page.getKey("/Contents");
  if (!nc.isStream()) return false;
  auto buf = nc.getStreamData(qpdf_dl_generalized);
  form.replaceStreamData(std::string(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize()), QPDFObjectHandle::newNull(),
                         QPDFObjectHandle::newNull());
  d.replaceKey("/Resources", page.getKey("/Resources"));
  std::set<QPDFObjGen> seen;
  return !resourcesOverprint(d.getKey("/Resources"), seen);
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
  Inliner inl{env, ctx.flattenOverprint ? size_t{3000000} : size_t{300000}};
  inl.run(col.ops, Ren(), 0, ops);

  std::vector<State> stack;
  std::string opqName;
  int silent = 0;
  std::vector<char> mute(ops.size());
  State st;
  std::vector<Shape> shapes;
  std::vector<Piece> pieces;
  std::vector<Shade> shades;
  Rect path;
  std::vector<Rect> pendingVec;
  size_t pathStart = 0;
  bool inPath = false, inText = false, pendingW = false, pendingEo = false, tmExact = true;
  std::string pendingText, pendingKey;
  size_t emitted = 0;
  int regions = 0;
  std::map<size_t, std::string> before, after, replace;
  std::vector<int> textClips;
  std::map<std::string, std::shared_ptr<std::vector<double>>> advCache;
  std::map<std::string, std::shared_ptr<CidWidths>> cidCache;
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
    double dpi = dpiWanted > 0 ? dpiWanted : ctx.opt.rasterDpi > 0 ? ctx.opt.rasterDpi : 300;
    if (ctx.redrawnPages.count(page.getObjGen()) && ctx.opt.renderPdf) {
      if (!renderCurrentPage(ctx, ph, dpi, rnd.w, rnd.h, rnd.rgb)) return false;
    } else {
      if (!ctx.opt.rasterizePage) return false;
      if (!ctx.opt.rasterizePage(pageIndex, dpi, rnd.w, rnd.h, rnd.rgb)) return false;
    }
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
    if (ctx.flattenOverprint) return false;
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
        auto r = l.ovp ? overprintOn(q, l.src, l.opm, env, l.a, l.bm) : over(q, l.src, l.a, l.bm, env);
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
    QPDFObjectHandle nd = sh.dict.shallowCopy();
    nd.replaceKey("/Function", sampledFunction(ctx, data, d0, d1, on));
    nd.replaceKey("/ColorSpace", outSpace);
    return env.add("/Shading", "KuraSh", ctx.pdf.makeIndirectObject(nd));
  };
  struct Samples {
    Colour cs;
    int w = 0, h = 0;
    std::string data;
    QPDFObjectHandle x;
    bool soft = false;
    std::string alpha;
  };
  auto mergedImage = [&](const Samples& sm, Colour other, const std::set<std::string>& painted, bool imageOnTop,
                         Samples* out = nullptr) -> std::string {
    const Colour& ic = sm.cs;
    int w = sm.w, h = sm.h;
    const std::string& smp = sm.data;
    size_t n = ic.names.size(), px = static_cast<size_t>(w) * h;
    Colour frame = ic;
    if (!mergeInks(frame, other, env) || frame.v.size() != other.v.size()) return "";
    size_t u = other.names.size();
    if (px * u > (size_t{96} << 20)) return "";
    std::vector<int> from(u, -1);
    std::vector<char> useConst(u);
    std::vector<unsigned char> cv(u);
    for (size_t k = 0; k < u; ++k) {
      auto it = std::find(ic.names.begin(), ic.names.end(), other.names[k]);
      if (it != ic.names.end()) from[k] = static_cast<int>(it - ic.names.begin());
      useConst[k] = imageOnTop ? from[k] < 0 : painted.count(other.names[k]) > 0;
      cv[k] = static_cast<unsigned char>(std::clamp(static_cast<int>(std::lround(other.v[k] * 255)), 0, 255));
    }
    std::string data(px * u, '\0');
    bool mix = imageOnTop && !sm.alpha.empty();
    for (size_t q = 0; q < px; ++q) {
      unsigned a = mix ? static_cast<unsigned char>(sm.alpha[q]) : 255;
      for (size_t k = 0; k < u; ++k) {
        unsigned img = from[k] >= 0 ? static_cast<unsigned char>(smp[q * n + from[k]]) : 0;
        data[q * u + k] = static_cast<char>(mix ? img + (255 - a) * cv[k] / 255 : useConst[k] ? cv[k] : img);
      }
    }
    QPDFObjectHandle d = sm.x.getDict();
    QPDFObjectHandle img = QPDFObjectHandle::newStream(&ctx.pdf, data);
    QPDFObjectHandle id = img.getDict();
    id.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    id.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
    id.replaceKey("/Width", QPDFObjectHandle::newInteger(w));
    id.replaceKey("/Height", QPDFObjectHandle::newInteger(h));
    id.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    id.replaceKey("/ColorSpace", other.space.isInitialized() && !other.space.isNull() ? other.space : QPDFObjectHandle::newName(other.fam));
    if (d.hasKey("/Interpolate")) id.replaceKey("/Interpolate", d.getKey("/Interpolate"));
    img = ctx.pdf.makeIndirectObject(img);
    if (out) {
      out->cs = other;
      out->w = w;
      out->h = h;
      out->data = data;
      out->x = img;
    }
    return env.add("/XObject", "KuraOpI", img);
  };
  std::vector<Samples> imgSm;
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
    bool ovp = ctx.flattenOverprint && (strokeSide ? st.OP : st.op) && (c.kind == 1 || c.kind == -1);
    if (ovp && (st.bm < 0 || st.bm >= 12)) return 0;
    if (ovp && st.smask) {
      Mat ci;
      if (!invert(st.ctm, ci) || reemit || s.clipCost() >= 9) return 0;
      int mw = 0, mh = 0;
      std::string ma;
      if (!renderSoftMask(ctx, st.smaskObj, st.smaskCtm, base.box, mw, mh, ma)) return 0;
      if (opqName.empty()) {
        QPDFObjectHandle g = QPDFObjectHandle::parse("<< /Type /ExtGState /ca 1 /CA 1 /BM /Normal /SMask /None >>");
        opqName = env.add("/ExtGState", "KuraOpq", ctx.pdf.makeIndirectObject(g));
      }
      std::set<int> active(st.clips.begin(), st.clips.end());
      std::string myExtra;
      const Rect& bb = base.box;
      for (size_t j = 0; j < limit; ++j) {
        const Piece& p = pieces[j];
        if (!p.box.hits(bb)) continue;
        if (!p.flat || p.painter < 0 || shapes[p.painter].clipCost() >= 9) return 0;
        auto full = overprintOn(p, c, st.opm, env, alpha, st.bm), none = overprintOn(p, c, st.opm, env, 0, 0);
        if (!full || !none || full->colour.v.size() != none->colour.v.size()) return 0;
        size_t u = full->colour.v.size(), px = static_cast<size_t>(mw) * mh;
        if (px * u > (size_t{96} << 20)) return 0;
        std::string data(px * u, '\0');
        for (size_t q = 0; q < px; ++q) {
          double a = static_cast<unsigned char>(ma[q]) / 255.0;
          for (size_t k = 0; k < u; ++k) {
            double v = none->colour.v[k] + a * (full->colour.v[k] - none->colour.v[k]);
            data[q * u + k] = static_cast<char>(std::clamp(static_cast<int>(std::lround(v * 255)), 0, 255));
          }
        }
        const Colour& fr = full->colour;
        QPDFObjectHandle img = QPDFObjectHandle::newStream(&ctx.pdf, data);
        QPDFObjectHandle id = img.getDict();
        id.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
        id.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
        id.replaceKey("/Width", QPDFObjectHandle::newInteger(mw));
        id.replaceKey("/Height", QPDFObjectHandle::newInteger(mh));
        id.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
        id.replaceKey("/ColorSpace", fr.space.isInitialized() && !fr.space.isNull() ? fr.space : QPDFObjectHandle::newName(fr.fam));
        std::string nm = env.add("/XObject", "KuraOpI", ctx.pdf.makeIndirectObject(img));
        myExtra += "q\n" + opqName + " gs\n";
        for (int k : p.clips) {
          if (!active.count(k)) myExtra += emitClip(shapes, shapes[k], ci);
        }
        myExtra += emitClip(shapes, shapes[p.painter], ci) + emitClip(shapes, s, ci) +
                   matStr(mul({bb.x1 - bb.x0, 0, 0, bb.y1 - bb.y0, bb.x0, bb.y0}, ci)) + "cm " + nm + " Do\nQ\n";
      }
      if (emitted + myExtra.size() > (size_t{16} << 20)) return 0;
      emitted += myExtra.size();
      extra += myExtra;
      base.flat = false;
      base.painter = -1;
      pieces.push_back(base);
      return 1;
    }
    bool transparent = ovp || alpha < 1 || st.bm != 0 || st.smask;
    Mat ci;
    bool inv = invert(st.ctm, ci);
    if (ctx.flattenOverprint && transparent && !ovp && (s.kind > 1 || pendingW)) {
      if (reemit) return 0;
      base.flat = false;
      base.painter = -1;
      pieces.push_back(base);
      return 1;
    }
    if (!transparent) {
      if (reemit) {
        if (!inv || !base.flat) return 0;
        extra += "q\n" + emitPaint(s, c, ci) + "Q\n";
      }
      pieces.push_back(base);
      return 1;
    }
    if (st.bm < 0 || st.smask || !base.flat || !inv) return 0;
    if (!ovp && env.x && !env.groupN && base.colour.kind > 0 && !toProcess(base.colour, 4)) return 0;
    base.alpha = alpha;
    bool vecT = ctx.flattenOverprint && !ovp && s.kind <= 1 && !pendingW;
    std::string opq;
    if ((ovp && (alpha < 1 || st.bm != 0)) || vecT) {
      if (opqName.empty()) {
        QPDFObjectHandle g = QPDFObjectHandle::parse("<< /Type /ExtGState /ca 1 /CA 1 /BM /Normal /SMask /None >>");
        opqName = env.add("/ExtGState", "KuraOpq", ctx.pdf.makeIndirectObject(g));
      }
      opq = opqName + " gs\n";
    }
    Piece paper;
    paper.colour = device({0, 0, 0, 0});
    std::optional<Piece> onWhite = ovp ? overprintOn(paper, c, st.opm, env, alpha, st.bm) : std::nullopt;
    if (ovp && !onWhite) return 0;
    std::string myPre = ovp ? "" : vecT ? "q\n" + opq + setColour(onPaper(base), strokeSide) : setColour(onPaper(base), strokeSide), myExtra;
    if (reemit) myExtra += "q\n" + opq + emitPaint(s, ovp ? onWhite->colour : onPaper(base), ci) + "Q\n";
    std::vector<Piece> add{base};
    if (ovp) {
      add[0].colour = onWhite->colour;
      add[0].alpha = 1;
    }
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
        if (!ovp && !sameSpace(tmpl, c)) return 0;
        Shade ns = shades[p.shade];
        ns.layers.push_back({c, alpha, st.bm, ovp, st.opm});
        ns.name = buildShading(ns);
        if (ns.name.empty()) return 0;
        shades.push_back(ns);
        n.clips.push_back(si);
        n.shade = static_cast<int>(shades.size()) - 1;
        add.push_back(n);
        continue;
      }
      if (ovp && p.stencil) {
        auto r = overprintOn(p, c, st.opm, env, alpha, st.bm);
        if (!r || s.clipCost() >= 9) return 0;
        n.colour = r->colour;
        n.clips.push_back(si);
        n.xobj = p.xobj;
        n.xm = p.xm;
        n.stencil = true;
        add.push_back(n);
        continue;
      }
      if (ovp && p.image >= 0) {
        Samples& sm = imgSm[p.image];
        if (alpha < 1 || st.bm != 0 || s.clipCost() >= 9 || (sm.w == 0 && !imageSamples(sm.x, res, sm.cs, sm.w, sm.h, sm.data, sm.soft))) return 0;
        Colour pc = c, frame = sm.cs;
        std::set<std::string> painted;
        if (!paintedInks(pc, st.opm, painted) || !mergeInks(frame, pc, env)) return 0;
        Samples mixed;
        n.xobj = mergedImage(sm, pc, painted, false, &mixed);
        if (n.xobj.empty()) return 0;
        n.clips.push_back(si);
        n.xm = p.xm;
        n.image = static_cast<int>(imgSm.size());
        imgSm.push_back(std::move(mixed));
        add.push_back(n);
        continue;
      }
      bool sepAll = ovp && p.painter >= 0 && shapes[p.painter].exact && p.colour.kind == 0 && p.colour.fam == "/Separation" &&
                    p.colour.space.isArray();
      if (sepAll && nameOf(p.colour.space.getArrayItem(1)) == "/None") continue;
      if (!p.flat && !sepAll) return 0;
      std::optional<Piece> r = ovp ? overprintOn(p, c, st.opm, env, alpha, st.bm) : over(p, c, alpha, st.bm, env);
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
    if (pieces.size() + add.size() > (ctx.flattenOverprint ? 40000u : 4000u)) return 0;
    std::set<int> active(st.clips.begin(), st.clips.end());
    bool group = s.kind == 0 && s.ops.size() > 200 && add.size() > 2 &&
                 std::all_of(add.begin() + 1, add.end(), [&](const Piece& q) {
                   return q.painter == si || std::find(q.clips.begin(), q.clips.end(), si) != q.clips.end();
                 });
    if (group) {
      myExtra += "q\n" + emitClip(shapes, s, ci);
      active.insert(si);
    }
    for (size_t k = 1; k < add.size(); ++k) {
      myExtra += "q\n" + opq;
      for (int cidx : add[k].clips) {
        if (!active.count(cidx)) myExtra += emitClip(shapes, shapes[cidx], ci);
      }
      if (group && add[k].painter == si && add[k].xobj.empty() && add[k].shade < 0) {
        const Rect& b = add[k].box;
        myExtra += setColour(onPaper(add[k]), false) + matStr(ci) + "cm " + f4(b.x0) + " " + f4(b.y0) + " " +
                   f4(b.x1 - b.x0) + " " + f4(b.y1 - b.y0) + " re f\n";
      } else if (!add[k].xobj.empty()) {
        myExtra += "q " + matStr(mul(add[k].xm, ci)) + "cm " + (add[k].stencil ? setColour(add[k].colour, false) : "") +
                   add[k].xobj + " Do Q\n";
      } else if (add[k].shade >= 0) {
        const Shade& sh = shades[add[k].shade];
        myExtra += "q " + matStr(mul(sh.ctm, ci)) + "cm " + sh.name + " sh Q\n";
      } else {
        myExtra += emitPaint(shapes[add[k].painter], onPaper(add[k]), ci);
      }
      myExtra += "Q\n";
    }
    if (group) myExtra += "Q\n";
    if (emitted + myExtra.size() > (ctx.flattenOverprint ? size_t{64} << 20 : size_t{16} << 20)) return 0;
    emitted += myExtra.size();
    pre += myPre;
    post += vecT ? "Q\n" : c.restore;
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
  auto backdropUsable = [&](const Piece& p, bool& skip, const std::vector<std::string>& inks) {
    skip = (p.shade >= 0 && coveredBy(shades[p.shade].frame, inks)) ||
           (p.image >= 0 && coveredBy(colourOf(imgSm[p.image].x.getDict().getKey("/ColorSpace"), res), inks));
    if (skip) return true;
    if (p.shade >= 0 || p.painter < 0 || shapes[p.painter].clipCost() >= 9) return false;
    if (p.flat) return true;
    if (!shapes[p.painter].exact || p.colour.kind != 0 || p.colour.fam != "/Separation" || !p.colour.space.isArray()) return false;
    skip = nameOf(p.colour.space.getArrayItem(1)) == "/None";
    return true;
  };
  auto overprintImage = [&](QPDFObjectHandle x, const Rect& box, size_t at, std::vector<Piece>& made, bool soft = false) -> bool {
    Mat ci;
    if (!invert(st.ctm, ci)) return false;
    Samples sm;
    sm.x = x;
    bool decoded = imageSamples(x, res, sm.cs, sm.w, sm.h, sm.data, soft, &sm.alpha);
    if (soft && !decoded) return false;
    std::set<int> active(st.clips.begin(), st.clips.end());
    std::string extra;
    for (size_t pi = 0, pn = pieces.size(); pi < pn; ++pi) {
      const Piece p = pieces[pi];
      if (!p.box.hits(box)) continue;
      bool skip;
      if (p.shade >= 0 || p.image >= 0) {
        const std::vector<std::string>& mine = decoded ? sm.cs.names : kProc;
        Colour bf;
        ShadeGeo bg;
        Mat binv = kId;
        bool bgray = false;
        if (p.shade >= 0) {
          const Shade& bs = shades[p.shade];
          if (!soft && coveredBy(bs.frame, mine)) continue;
          bf = bs.frame;
          bgray = bf.kind == 1 && bf.v.size() == 1;
          if (bgray) bf = device({0, 0, 0, 0});
          if (!bs.layers.empty() || !loadGeo(bs.dict, bs.ctm, bg)) return false;
        } else {
          Samples& bsm = imgSm[p.image];
          if (bsm.w == 0 && !imageSamples(bsm.x, res, bsm.cs, bsm.w, bsm.h, bsm.data, bsm.soft, &bsm.alpha)) return false;
          if (!soft && coveredBy(bsm.cs, mine)) continue;
          if (!invert(p.xm, binv)) return false;
          bf = bsm.cs;
        }
        if (!decoded || bf.kind >= 0) return false;
        std::vector<std::string> bnames = p.shade >= 0 && bgray ? std::vector<std::string>{"/Black"} : bf.names;
        Colour sf = sm.cs;
        if (!mergeInks(bf, sf, env)) return false;
        size_t u = sf.names.size(), n = sm.cs.names.size(), px = static_cast<size_t>(sm.w) * sm.h;
        if (px * u > (size_t{96} << 20)) return false;
        std::vector<int> fromI(u, -1), fromB(u, -1);
        for (size_t k = 0; k < u; ++k) {
          auto a = std::find(sm.cs.names.begin(), sm.cs.names.end(), sf.names[k]);
          if (a != sm.cs.names.end()) fromI[k] = static_cast<int>(a - sm.cs.names.begin());
          auto b = std::find(bnames.begin(), bnames.end(), sf.names[k]);
          if (b != bnames.end()) fromB[k] = static_cast<int>(b - bnames.begin());
        }
        Samples mixed;
        mixed.cs = sf;
        mixed.w = sm.w;
        mixed.h = sm.h;
        mixed.data.assign(px * u, '\0');
        double bo[32];
        for (int y = 0; y < sm.h; ++y) {
          for (int xx = 0; xx < sm.w; ++xx) {
            auto pg = apply(st.ctm, (xx + 0.5) / sm.w, 1 - (y + 0.5) / sm.h);
            bool have = false;
            const unsigned char* bpx = nullptr;
            if (p.shade >= 0) {
              have = shadeAt(bg, pg[0], pg[1], bo);
            } else {
              const Samples& bsm = imgSm[p.image];
              auto uv = apply(binv, pg[0], pg[1]);
              if (uv[0] >= 0 && uv[0] < 1 && uv[1] > 0 && uv[1] <= 1) {
                size_t bx = static_cast<size_t>(uv[0] * bsm.w), by = static_cast<size_t>((1 - uv[1]) * bsm.h);
                bpx = reinterpret_cast<const unsigned char*>(bsm.data.data()) + (by * bsm.w + bx) * bsm.cs.names.size();
              }
            }
            size_t q = static_cast<size_t>(y) * sm.w + xx;
            unsigned a = sm.alpha.empty() ? 255 : static_cast<unsigned char>(sm.alpha[q]);
            for (size_t k = 0; k < u; ++k) {
              unsigned bv = 0;
              if (fromB[k] >= 0) {
                if (p.shade >= 0 && have) bv = static_cast<unsigned>(std::clamp(static_cast<int>(std::lround((bgray ? 1 - bo[0] : bo[fromB[k]]) * 255)), 0, 255));
                if (p.image >= 0 && bpx) bv = bpx[fromB[k]];
              }
              unsigned iv = fromI[k] >= 0 ? static_cast<unsigned char>(sm.data[q * n + fromI[k]]) : 0;
              mixed.data[q * u + k] = static_cast<char>(soft ? iv + (255 - a) * bv / 255 : fromI[k] >= 0 ? iv : bv);
            }
          }
        }
        QPDFObjectHandle img = QPDFObjectHandle::newStream(&ctx.pdf, mixed.data);
        QPDFObjectHandle id = img.getDict();
        id.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
        id.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
        id.replaceKey("/Width", QPDFObjectHandle::newInteger(sm.w));
        id.replaceKey("/Height", QPDFObjectHandle::newInteger(sm.h));
        id.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
        id.replaceKey("/ColorSpace", sf.space.isInitialized() && !sf.space.isNull() ? sf.space : QPDFObjectHandle::newName(sf.fam));
        mixed.x = ctx.pdf.makeIndirectObject(img);
        std::string nm = env.add("/XObject", "KuraOpI", mixed.x);
        extra += "q\n";
        for (int k : p.clips) {
          if (!active.count(k)) extra += emitClip(shapes, shapes[k], ci);
        }
        if (p.image >= 0) {
          Mat bm = mul(p.xm, ci);
          auto c0 = apply(bm, 0, 0), c1 = apply(bm, 1, 0), c2 = apply(bm, 1, 1), c3 = apply(bm, 0, 1);
          extra += f4(c0[0]) + " " + f4(c0[1]) + " m " + f4(c1[0]) + " " + f4(c1[1]) + " l " + f4(c2[0]) + " " + f4(c2[1]) +
                   " l " + f4(c3[0]) + " " + f4(c3[1]) + " l h W n\n";
        }
        extra += nm + " Do\nQ\n";
        Piece m;
        m.clips = st.clips;
        for (int k : p.clips) m.clips.push_back(k);
        m.box = meet(p.box, box);
        m.image = static_cast<int>(imgSm.size());
        m.xm = st.ctm;
        imgSm.push_back(std::move(mixed));
        made.push_back(m);
        continue;
      }
      if (!backdropUsable(p, skip, decoded ? sm.cs.names : kProc)) return false;
      if (skip) continue;
      Colour probe = decoded ? sm.cs : device({0, 0, 0, 0}), bc;
      if (!inksUnder(p, probe, bc, env)) return false;
      if (!soft && !hasForeignInk(bc, decoded ? sm.cs.names : kProc)) continue;
      if (!decoded) return false;
      Samples mixed;
      std::string nm = mergedImage(sm, bc, {}, true, &mixed);
      if (nm.empty()) return false;
      extra += "q\n";
      for (int k : p.clips) {
        if (!active.count(k)) extra += emitClip(shapes, shapes[k], ci);
      }
      extra += emitClip(shapes, shapes[p.painter], ci) + nm + " Do\nQ\n";
      Piece m;
      m.clips = st.clips;
      for (int k : p.clips) m.clips.push_back(k);
      m.clips.push_back(p.painter);
      m.box = meet(p.box, box);
      m.image = static_cast<int>(imgSm.size());
      m.xm = st.ctm;
      imgSm.push_back(std::move(mixed));
      made.push_back(m);
    }
    after[at] += extra;
    return true;
  };
  auto overprintShading = [&](QPDFObjectHandle sh, size_t at) -> bool {
    QPDFObjectHandle sd = sh.isStream() ? sh.getDict() : sh;
    if (!sd.isDictionary()) return false;
    Colour sc = colourOf(sd.getKey("/ColorSpace"), res);
    bool gray = sc.kind == 1 && sc.v.size() == 1 && sc.cs.empty() && sc.fam == "/DeviceGray";
    if (sc.kind == 0 && sc.fam == "/DeviceN" && !sc.names.empty() &&
        std::find(sc.names.begin(), sc.names.end(), "/All") == sc.names.end()) {
      sc.kind = -1;
    }
    std::vector<std::string> inks = gray ? std::vector<std::string>{"/Black"} : sc.names;
    if (gray) sc = device({0, 0, 0, 0});
    int type = static_cast<int>(numOf(sd.getKey("/ShadingType"), 0));
    auto fn = PdfFunction::loadArray(sd.getKey("/Function"));
    bool usable = sc.kind < 0 && (type == 2 || type == 3) && fn && fn->inputs() == 1 &&
                  static_cast<size_t>(fn->outputs()) == (gray ? 1 : inks.size()) && !sh.isStream();
    Mat ci;
    if (!invert(st.ctm, ci)) return false;
    double d0 = 0, d1 = 1;
    QPDFObjectHandle dom = sd.getKey("/Domain");
    if (dom.isArray() && dom.getArrayNItems() == 2) {
      d0 = num(dom.getArrayItem(0));
      d1 = num(dom.getArrayItem(1));
    }
    std::set<int> active(st.clips.begin(), st.clips.end());
    std::string extra;
    for (const Piece& p : pieces) {
      if (!p.box.hits(st.clipBox)) continue;
      bool skip;
      const std::vector<std::string>& cover = sc.kind < 0 ? inks : kProc;
      if (p.shade >= 0 && !coveredBy(shades[p.shade].frame, cover)) {
        const Shade& bs = shades[p.shade];
        ShadeGeo sg, bg;
        Colour bf = bs.frame, sf = sc;
        bool bgray = bf.kind == 1 && bf.v.size() == 1;
        std::vector<std::string> bnames = bgray ? std::vector<std::string>{"/Black"} : bf.names;
        if (bgray) bf = device({0, 0, 0, 0});
        if (!usable || !bs.layers.empty() || bf.kind >= 0 || !loadGeo(sd, st.ctm, sg) || !loadGeo(bs.dict, bs.ctm, bg) ||
            !mergeInks(bf, sf, env)) {
          return false;
        }
        Rect b = meet(p.box, st.clipBox);
        if (!b.valid() || b.x1 <= b.x0 || b.y1 <= b.y0) continue;
        double scale = 300.0 / 72;
        double area = (b.x1 - b.x0) * (b.y1 - b.y0) * scale * scale;
        if (area > 4e6) scale *= std::sqrt(4e6 / area);
        int W = std::max(1, static_cast<int>(std::ceil((b.x1 - b.x0) * scale)));
        int H = std::max(1, static_cast<int>(std::ceil((b.y1 - b.y0) * scale)));
        size_t u = sf.names.size();
        std::vector<int> fromS(u, -1), fromB(u, -1);
        for (size_t k = 0; k < u; ++k) {
          auto i1 = std::find(inks.begin(), inks.end(), sf.names[k]);
          if (i1 != inks.end()) fromS[k] = static_cast<int>(i1 - inks.begin());
          auto i2 = std::find(bnames.begin(), bnames.end(), sf.names[k]);
          if (i2 != bnames.end()) fromB[k] = static_cast<int>(i2 - bnames.begin());
        }
        std::string data(static_cast<size_t>(W) * H * u, '\0');
        double so[32], bo[32];
        for (int y = 0; y < H; ++y) {
          for (int x = 0; x < W; ++x) {
            double px = b.x0 + (x + 0.5) / W * (b.x1 - b.x0), py = b.y1 - (y + 0.5) / H * (b.y1 - b.y0);
            if (!shadeAt(bg, px, py, bo)) return false;
            bool painted = shadeAt(sg, px, py, so);
            for (size_t k = 0; k < u; ++k) {
              double v = painted && fromS[k] >= 0 ? (gray ? 1 - so[0] : so[fromS[k]])
                         : fromB[k] >= 0         ? (bgray ? 1 - bo[0] : bo[fromB[k]])
                                                 : 0;
              data[(static_cast<size_t>(y) * W + x) * u + k] = static_cast<char>(std::clamp(static_cast<int>(std::lround(v * 255)), 0, 255));
            }
          }
        }
        QPDFObjectHandle img = QPDFObjectHandle::newStream(&ctx.pdf, data);
        QPDFObjectHandle id = img.getDict();
        id.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
        id.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
        id.replaceKey("/Width", QPDFObjectHandle::newInteger(W));
        id.replaceKey("/Height", QPDFObjectHandle::newInteger(H));
        id.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
        id.replaceKey("/ColorSpace", sf.space.isInitialized() && !sf.space.isNull() ? sf.space : QPDFObjectHandle::newName(sf.fam));
        std::string nm = env.add("/XObject", "KuraOpI", ctx.pdf.makeIndirectObject(img));
        extra += "q\n";
        for (int k : p.clips) {
          if (!active.count(k)) extra += emitClip(shapes, shapes[k], ci);
        }
        extra += matStr(mul({b.x1 - b.x0, 0, 0, b.y1 - b.y0, b.x0, b.y0}, ci)) + "cm " + nm + " Do\nQ\n";
        continue;
      }
      if (!backdropUsable(p, skip, cover)) return false;
      if (skip) continue;
      Colour src = usable ? sc : device({0, 0, 0, 0}), bc;
      if (!inksUnder(p, src, bc, env)) return false;
      if (!hasForeignInk(bc, cover)) continue;
      if (!usable) return false;
      size_t u = src.names.size();
      std::string data;
      for (int k = 0; k < 256; ++k) {
        double t = d0 + (d1 - d0) * k / 255.0, out[32] = {0};
        if (!fn->compute(&t, out)) return false;
        for (size_t c = 0; c < u; ++c) {
          auto it = std::find(inks.begin(), inks.end(), src.names[c]);
          double x = it == inks.end() ? bc.v[c] : gray ? 1 - out[0] : out[it - inks.begin()];
          data += static_cast<char>(std::clamp(static_cast<int>(std::lround(x * 255)), 0, 255));
        }
      }
      QPDFObjectHandle nd = sd.shallowCopy();
      nd.replaceKey("/Function", sampledFunction(ctx, data, d0, d1, u));
      nd.replaceKey("/ColorSpace", src.space.isInitialized() && !src.space.isNull() ? src.space : QPDFObjectHandle::newName(src.fam));
      std::string nm = env.add("/Shading", "KuraSh", ctx.pdf.makeIndirectObject(nd));
      extra += "q\n";
      for (int k : p.clips) {
        if (!active.count(k)) extra += emitClip(shapes, shapes[k], ci);
      }
      extra += emitClip(shapes, shapes[p.painter], ci) + nm + " sh\nQ\n";
    }
    after[at] += extra;
    return true;
  };
  auto overprintStencil = [&](const std::string& name, const Rect& box, size_t at, std::vector<Piece>& made) -> bool {
    Mat ci;
    if (!invert(st.ctm, ci)) return false;
    Colour fc = st.fill;
    std::set<std::string> painted;
    if (!paintedInks(fc, st.opm, painted)) return false;
    std::vector<std::string> inks(painted.begin(), painted.end());
    std::set<int> active(st.clips.begin(), st.clips.end());
    std::string extra;
    for (const Piece& p : pieces) {
      if (!p.box.hits(box)) continue;
      bool skip;
      if (!backdropUsable(p, skip, inks)) return false;
      if (skip) continue;
      auto r = overprintOn(p, st.fill, st.opm, env);
      if (!r) return false;
      extra += "q\n";
      for (int k : p.clips) {
        if (!active.count(k)) extra += emitClip(shapes, shapes[k], ci);
      }
      extra += emitClip(shapes, shapes[p.painter], ci) + setColour(r->colour, false) + name + " Do\nQ\n";
      Piece m;
      m.clips = st.clips;
      for (int k : p.clips) m.clips.push_back(k);
      m.clips.push_back(p.painter);
      m.box = meet(p.box, box);
      m.stencil = true;
      m.colour = r->colour;
      m.xobj = name;
      m.xm = st.ctm;
      made.push_back(m);
    }
    after[at] += extra;
    return true;
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
    if (op == "KuraFB") {
      if (silent++ == 0) replace[i] = a[0].getName() + " Do\n";
      continue;
    }
    if (op == "KuraFE") {
      --silent;
      continue;
    }
    if (silent) mute[i] = 1;
    if (op == "q") {
      stack.push_back(st);
    } else if (op == "Q") {
      if (!stack.empty()) {
        st = stack.back();
        stack.pop_back();
      }
    } else if (op == "KuraGB" && silent) {
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
      st.dash = (a[0].isArray() && a[0].getArrayNItems() == 0 ? "[]" : a[0].unparseResolved()) + " " + a[1].unparseResolved();
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
        QPDFObjectHandle da = dd.getArrayItem(0);
        st.dash = (da.isArray() && da.getArrayNItems() == 0 ? "[]" : da.unparseResolved()) + " " + dd.getArrayItem(1).unparseResolved();
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
      if (g.hasKey("/SMask")) {
        st.smask = !nameIs(g.getKey("/SMask"), "/None");
        st.smaskObj = st.smask ? g.getKey("/SMask") : QPDFObjectHandle();
        st.smaskCtm = st.ctm;
      }
      if (g.hasKey("/OP")) st.OP = st.op = g.getKey("/OP").isBool() && g.getKey("/OP").getBoolValue();
      if (g.hasKey("/op")) st.op = g.getKey("/op").isBool() && g.getKey("/op").getBoolValue();
      if (g.hasKey("/OPM")) st.opm = static_cast<int>(numOf(g.getKey("/OPM"), 0));
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
        if (st.dash.rfind("[]", 0) == 0) ss.outline = strokeOutline(pops, st.ctm, st.lw, st.cap, st.join, st.ml, ctx.flattenOverprint ? 0.01 : 1);
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
        if (!ok && ctx.flattenOverprint && !(fill && st.op) && !(stroke && st.OP)) {
          pieces.resize(base);
          Piece blk;
          blk.box = meet(stroke ? ss.box : fs.box, st.clipBox);
          pieces.push_back(blk);
        } else if (!ok) {
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
      pendingKey.clear();
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
      Mat tlmBefore = st.tlm;
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
      std::shared_ptr<CidWidths> cw;
      if (fontOk && !simple) {
        std::string ck = "cid" + (st.font.isIndirect() ? spaceKey(st.font) : st.font.unparse());
        auto it = cidCache.find(ck);
        cw = it != cidCache.end() ? it->second : (cidCache[ck] = cidWidths(st.font));
      }
      bool known = fontOk && ((simple && (widths.isArray() || adv)) || cw);
      int firstChar = fontOk ? static_cast<int>(numOf(st.font.getKey("/FirstChar"), 0)) : 0;
      QPDFObjectHandle desc = fontOk ? st.font.getKey("/FontDescriptor") : QPDFObjectHandle::newNull();
      double missing = desc.isDictionary() && !type3 ? numOf(desc.getKey("/MissingWidth"), 0) : 0;
      double fs = st.fontSize, tx = 0;
      std::vector<std::pair<double, double>> gx;
      std::vector<std::pair<unsigned char, double>> t3codes;
      auto glyphs = [&](const std::string& str) {
        if (cw) {
          for (size_t q = 0; q + 1 < str.size(); q += 2) {
            unsigned cid = static_cast<unsigned char>(str[q]) << 8 | static_cast<unsigned char>(str[q + 1]);
            auto f = cw->w.find(cid);
            double w = (f != cw->w.end() ? f->second : cw->dw) / 1000.0, gx0 = tx;
            tx += (w * fs + st.tc) * st.tz;
            gx.push_back({std::min(gx0, gx0 + w * fs * st.tz), std::max(gx0, gx0 + w * fs * st.tz)});
          }
          return;
        }
        for (unsigned char ch : str) {
          double gx0 = tx;
          if (type3) t3codes.push_back({ch, gx0});
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
      std::vector<Op> t3path;
      if (type3 && ctx.flattenOverprint && mode == 0 && !clipMode && tmExact && known &&
          type3Outline(st.font, t3codes, mul({fs * st.tz, 0, 0, fs, 0, st.rise}, trm), t3path)) {
        s.kind = 0;
        s.ops = t3path;
        s.ctm = kId;
        s.noClip = false;
        s.exact = true;
      }
      if (clipMode) {
        shapes.push_back(s);
        textClips.push_back(static_cast<int>(shapes.size()) - 1);
      }
      std::string paintKey = ctx.flattenOverprint ? setColour(st.fill, false) + setColour(st.stroke, true) +
                                                        std::to_string(st.op) + std::to_string(st.OP) +
                                                        std::to_string(st.opm) + std::to_string(mode)
                                                  : "";
      if (mode != 3) {
        if (pendingHits(gboxes) && (paintKey.empty() || paintKey != pendingKey)) {
          if (!ctx.flattenOverprint || st.tr >= 4 || !tmExact) return false;
          std::string restore;
          if (op == "'" || op == "\"") {
            restore = matStr(tlmBefore) + "Tm\n";
          } else {
            Mat il;
            if (!invert(st.tlm, il)) return false;
            Mat dm = mul(st.tm, il);
            if (std::fabs(dm[0] - 1) > 1e-6 || std::fabs(dm[1]) > 1e-6 || std::fabs(dm[2]) > 1e-6 ||
                std::fabs(dm[3] - 1) > 1e-6 || std::fabs(dm[5]) > 1e-6) {
              return false;
            }
            restore = matStr(st.tlm) + "Tm\n";
            if (std::fabs(dm[4]) > 1e-9) {
              if (st.fontSize == 0 || st.tz == 0) return false;
              restore += "[" + f4(-dm[4] * 1000 / (st.fontSize * st.tz)) + "] TJ\n";
            }
          }
          before[i] += "ET\n" + pendingText + "BT\n" + restore;
          pendingText.clear();
          pendingVec.clear();
          pendingKey.clear();
        }
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
            pendingKey = pendingVec.empty() || pendingKey == paintKey ? paintKey : "#";
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
      bool stencil = d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue();
      bool opImage = ctx.flattenOverprint && st.op && !form && !stencil;
      if (ctx.flattenOverprint && form) {
        std::set<QPDFObjGen> seen;
        if (st.op || st.OP || inText) return false;
        if (resourcesOverprint(d.getKey("/Resources"), seen)) {
          QPDFObjectHandle g = d.getKey("/Group");
          bool isolated = g.isDictionary() && g.getKey("/I").isBool() && g.getKey("/I").getBoolValue();
          Rect under = st.clipBox;
          bool covered = std::any_of(pieces.begin(), pieces.end(), [&](const Piece& q) { return q.box.hits(under); });
          if (!isolated && covered) return false;
          QPDFObjectHandle copy = QPDFObjectHandle::newStream(&ctx.pdf);
          copy.replaceStreamData(x.getRawStreamData(), d.getKey("/Filter"), d.getKey("/DecodeParms"));
          for (const std::string& k : d.getKeys()) {
            if (k != "/Length" && k != "/Filter" && k != "/DecodeParms") copy.getDict().replaceKey(k, d.getKey(k));
          }
          if (!flattenFormOverprint(ctx, copy)) return false;
          replace[i] = env.add("/XObject", "KuraOpF", ctx.pdf.makeIndirectObject(copy)) + " Do\n";
        }
        Piece p;
        p.box = meet(everywhere(), st.clipBox);
        QPDFObjectHandle bb = d.getKey("/BBox");
        if (bb.isArray() && bb.getArrayNItems() == 4) {
          Mat fm = kId;
          QPDFObjectHandle mm = d.getKey("/Matrix");
          if (mm.isArray() && mm.getArrayNItems() == 6) {
            for (int k = 0; k < 6; ++k) fm[k] = num(mm.getArrayItem(k));
          }
          Rect fb;
          for (int cx : {0, 2}) {
            for (int cy : {1, 3}) fb.add(mul(fm, st.ctm), num(bb.getArrayItem(cx)), num(bb.getArrayItem(cy)));
          }
          p.box = meet(fb, st.clipBox);
        }
        pieces.push_back(p);
        continue;
      }
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
      if ((selfT || st.ca < 1 || st.bm != 0 || st.smask) && ctx.flattenOverprint) {
        if (st.op || st.OP || inText) return false;
        Rect vis = meet(box, st.clipBox);
        bool softImage = !form && d.getKey("/SMask").isStream() && st.ca >= 1 && st.bm == 0 && !st.smask;
        std::vector<Piece> made;
        std::string keep = after[i];
        bool clear = softImage && overprintImage(x, vis, i, made, true);
        if (!clear) after[i] = keep;
        if (!addBlock(box)) return false;
        if (clear) {
          pieces.back().clips = st.clips;
          pieces.back().image = static_cast<int>(imgSm.size());
          pieces.back().xm = st.ctm;
          imgSm.emplace_back();
          imgSm.back().x = x;
          imgSm.back().soft = true;
          pieces.insert(pieces.end(), made.begin(), made.end());
        }
        continue;
      }
      if (selfT || st.ca < 1 || st.bm != 0 || st.smask) {
        if (inText || !region(clip, box, i, !form && axis)) return false;
        QPDFObjectHandle fr = d.getKey("/Resources");
        if (!form || !(fr.isDictionary() && fr.getKey("/Font").isDictionary())) replace[i] = "";
        continue;
      }
      std::vector<Piece> made;
      if (opImage && (inText || !overprintImage(x, meet(box, st.clipBox), i, made))) return false;
      if (ctx.flattenOverprint && st.op && stencil &&
          (inText || !overprintStencil(a[0].getName(), meet(box, st.clipBox), i, made))) {
        return false;
      }
      if (!addBlock(box)) return false;
      if (ctx.flattenOverprint && stencil && (st.fill.kind == 1 || st.fill.kind == -1)) {
        pieces.back().clips = st.clips;
        pieces.back().stencil = true;
        pieces.back().colour = st.fill;
        pieces.back().xobj = a[0].getName();
        pieces.back().xm = st.ctm;
        pieces.insert(pieces.end(), made.begin(), made.end());
      } else if (ctx.flattenOverprint && !form && !stencil) {
        pieces.back().clips = st.clips;
        pieces.back().image = static_cast<int>(imgSm.size());
        pieces.back().xm = st.ctm;
        imgSm.emplace_back();
        imgSm.back().x = x;
        pieces.insert(pieces.end(), made.begin(), made.end());
      }
      if (!form) opaqueImages.push_back({i, meet(box, st.clipBox), st.clips});
    } else if (op == "EI") {
      if (ctx.flattenOverprint && st.op) return false;
      Rect box;
      for (double cx : {0.0, 1.0}) {
        for (double cy : {0.0, 1.0}) box.add(st.ctm, cx, cy);
      }
      if (st.ca < 1 || st.bm != 0 || st.smask) {
        if (ctx.flattenOverprint) {
          if (!addBlock(box)) return false;
          continue;
        }
        if (inText || !region("0 0 m 1 0 l 1 1 l 0 1 l h W n\n", box, i)) return false;
        continue;
      }
      if (!addBlock(box)) return false;
    } else if (op == "sh" && !a.empty() && a[0].isName()) {
      if (inText) return false;
      if (ctx.flattenOverprint && st.op) {
        QPDFObjectHandle shd = res.getKey("/Shading");
        if (!shd.isDictionary() || !overprintShading(shd.getKey(a[0].getName()), i)) return false;
      }
      if (st.ca < 1 || st.bm != 0 || st.smask) {
        if (ctx.flattenOverprint && !st.op) {
          Piece p;
          p.box = st.clipBox;
          pieces.push_back(p);
          continue;
        }
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
      if (ops[i].op == "KuraFB") used.insert(ops[i].args[0].getName());
      if (ops[i].op != "Do" || ops[i].args.empty() || !ops[i].args[0].isName()) continue;
      auto r = replace.find(i);
      if (r == replace.end() || !r->second.empty()) used.insert(ops[i].args[0].getName());
    }
    QPDFObjectHandle xd = res.getKey("/XObject");
    if (xd.isDictionary()) {
      for (const std::string& nm : xd.getKeys()) {
        if (!used.count(nm) && nm.rfind("/Kura", 0) != 0) xd.removeKey(nm);
      }
    }
  }
  if (!ctx.flattenOverprint) neutraliseTransparency(ctx, res);
  QPDFObjectHandle egs = res.getKey("/ExtGState");
  if (ctx.flattenOverprint && egs.isDictionary()) {
    for (const std::string& k : egs.getKeys()) {
      QPDFObjectHandle g = egs.getKey(k);
      if (!g.isDictionary() || !(g.hasKey("/OP") || g.hasKey("/op") || g.hasKey("/OPM"))) continue;
      g = g.shallowCopy();
      g.removeKey("/OP");
      g.removeKey("/op");
      g.removeKey("/OPM");
      egs.replaceKey(k, ctx.pdf.makeIndirectObject(g));
    }
  }
  QPDFObjectHandle origRes = page.getKey("/Resources");
  QPDFObjectHandle origContents = page.getKey("/Contents");
  std::string out;
  for (size_t i = 0; i < ops.size(); ++i) {
    if (mute[i]) continue;
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
  if (!ctx.flattenOverprint && resourcesHaveTransparency(page.getKey("/Resources"))) {
    page.replaceKey("/Resources", origRes);
    page.replaceKey("/Contents", origContents);
    return false;
  }
  if (regionsOut) *regionsOut = regions;
  return true;
}
}
