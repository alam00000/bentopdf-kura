#include <qpdf/QPDF.hh>
#include <qpdf/QPDFAnnotationObjectHelper.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>
#include <qpdf/Pl_Buffer.hh>
#include <qpdf/Pl_Discard.hh>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <sstream>
#include <set>
#include <string>
#include <vector>

#include <lcms2.h>

#include "assets/fonts_data.hh"
#include "barcode.hh"
#include "fonts_ft.hh"
#include "check_registry.hh"
#include "colorconv.hh"
#include "ctx.hh"
#include "images.hh"
#include "limits.hh"
#include "htmlshim.hh"
#include "objlayer.hh"
#include "passes.hh"
#include "profile_types.hh"
#include "textextract.hh"
#include "util.hh"

namespace pdfa {
extern const unsigned char kSrgbIcc[];
extern const unsigned int kSrgbIccLen;
extern const unsigned char kCmykIcc[];
extern const unsigned int kCmykIccLen;
std::vector<int> nearestSeed(const std::vector<unsigned char>& on, int w, int h);
namespace {
class LayerFlattenFilter : public QPDFObjectHandle::TokenFilter {
 public:
  LayerFlattenFilter(const std::set<std::string>& hiddenNames, long long& dropped)
      : hidden_(hiddenNames), dropped_(dropped) {}

  void handleToken(QPDFTokenizer::Token const& tok) override {
    using TT = QPDFTokenizer;
    if (tok.getType() == TT::tt_inline_image) {
      if (!suppress_) writeToken(tok);
      pending_.clear();
      return;
    }
    if (tok.getType() != TT::tt_word) {
      pending_.push_back(tok);
      if (pending_.size() > 64) {
        if (!suppress_) flush();
        else pending_.clear();
      }
      return;
    }
    const std::string op = tok.getValue();
    if (op == "BDC" || op == "BMC") {
      bool isOc = false;
      std::string tag;
      for (const QPDFTokenizer::Token& t : pending_) {
        if (t.getType() == TT::tt_name) {
          if (t.getValue() == "/OC") isOc = true;
          else if (isOc) tag = t.getValue();
        }
      }
      if (isOc) {
        bool hide = suppress_ || (!tag.empty() && hidden_.count(tag) > 0);
        marks_.push_back({true, hide && !suppress_});
        if (!suppress_ && hide) {
          suppress_ = true;
          ++dropped_;
        }
        pending_.clear();
        return;
      }
      marks_.push_back({false, false});
      if (!suppress_) flush(), writeToken(tok);
      else pending_.clear();
      return;
    }
    if (op == "EMC") {
      if (marks_.empty()) {
        if (!suppress_) flush(), writeToken(tok);
        else pending_.clear();
        return;
      }
      Mark m = marks_.back();
      marks_.pop_back();
      if (m.startedSuppress) suppress_ = false;
      pending_.clear();
      if (!m.isOc && !suppress_) writeToken(tok);
      return;
    }
    if (suppress_) {
      pending_.clear();
      return;
    }
    flush();
    writeToken(tok);
  }

  void handleEOF() override {
    suppress_ = false;
    flush();
  }

 private:
  struct Mark {
    bool isOc;
    bool startedSuppress;
  };
  void flush() {
    for (const QPDFTokenizer::Token& t : pending_) writeToken(t);
    pending_.clear();
  }
  const std::set<std::string>& hidden_;
  long long& dropped_;
  std::vector<QPDFTokenizer::Token> pending_;
  std::vector<Mark> marks_;
  bool suppress_ = false;
};

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
    if (tok.getType() == TT::tt_inline_image) {
      flushPending();
      writeToken(tok);
      return;
    }
    if (tok.getType() != TT::tt_word) {
      pending_.push_back(tok);
      if (pending_.size() > 4096) flushPending();
      return;
    }
    std::string op = tok.getValue();
    static const std::set<std::string> kPathOps = {"m", "l", "c", "v", "y", "h", "re"};
    if (kPathOps.count(op)) {
      pending_.push_back(tok);
      nums_.clear();
      if (pending_.size() > 4096) flushPending();
      return;
    }
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
      writeToken(QPDFTokenizer::Token(TT::tt_word, fmtFixed(minWidth_, 4) + " w"));
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
    const char* gsName = nullptr;
    if (actOn && (isFillOp || isStrokeOp || isTextOp)) {
      const std::vector<double>& c = (isStrokeOp && !isFillOp) ? stroke_ : fill_;
      if (knockWhite_ && isWhiteVec(c)) gsName = "/KuraKO gs\n";
      else if (opBlack_ && is100kVec(c)) gsName = "/KuraOB gs\n";
    }
    if (gsName && (isFillOp || isStrokeOp)) {
      write("q\n");
      write(gsName);
      pending_.push_back(tok);
      flushPending();
      write("\nQ\n");
      nums_.clear();
      return;
    }
    if (gsName) write(gsName);
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

namespace {
bool placeEdgeBleed(Ctx& ctx, QPDFPageObjectHelper& ph, int pageIndex, const double* t,
                    const double* amt, double dpi, bool mirror) {
  int w = 0, h = 0;
  std::string rgb;
  if (!ctx.opt.rasterizePage(pageIndex, dpi, w, h, rgb) || w <= 0 || h <= 0 ||
      rgb.size() < static_cast<size_t>(w) * h * 3) {
    return false;
  }
  QPDFObjectHandle cb = ph.getAttribute("/CropBox", false);
  if (!cb.isArray()) cb = ph.getAttribute("/MediaBox", true);
  if (!cb.isArray() || cb.getArrayNItems() != 4) return false;
  double c0 = numOf(cb.getArrayItem(0), 0), c1 = numOf(cb.getArrayItem(1), 0);
  double c2 = numOf(cb.getArrayItem(2), 0), c3 = numOf(cb.getArrayItem(3), 0);
  double sx = w / (c2 - c0), sy = h / (c3 - c1);
  auto px = [&](double x) { return static_cast<int>(std::lround((x - c0) * sx)); };
  auto py = [&](double y) { return static_cast<int>(std::lround((c3 - y) * sy)); };
  int tx1 = std::max(0, px(t[0])), tx2 = std::min(w, px(t[2]));
  int ty1 = std::max(0, py(t[3])), ty2 = std::min(h, py(t[1]));
  if (tx2 - tx1 < 2 || ty2 - ty1 < 2) return false;
  int bl = static_cast<int>(std::ceil(amt[2] * sx)), br = static_cast<int>(std::ceil(amt[3] * sx));
  int bt = static_cast<int>(std::ceil(amt[0] * sy)), bb = static_cast<int>(std::ceil(amt[1] * sy));
  int ow = (tx2 - tx1) + bl + br, oh = (ty2 - ty1) + bt + bb;
  if (ow <= 0 || oh <= 0 || static_cast<long long>(ow) * oh > 60000000LL) return false;
  std::string out(static_cast<size_t>(ow) * oh * 3, '\xff');
  auto src = [&](int x, int y) {
    int iw = tx2 - tx1, ih = ty2 - ty1;
    auto fold = [&](int v, int n) {
      if (mirror) {
        if (v < 0) v = -v - 1;
        if (v >= n) v = 2 * n - v - 1;
      }
      return std::min(std::max(v, 0), n - 1);
    };
    int sxp = tx1 + fold(x, iw), syp = ty1 + fold(y, ih);
    return rgb.data() + (static_cast<size_t>(syp) * w + sxp) * 3;
  };
  for (int y = 0; y < oh; ++y) {
    for (int x = 0; x < ow; ++x) {
      std::memcpy(&out[(static_cast<size_t>(y) * ow + x) * 3], src(x - bl, y - bt), 3);
    }
  }
  std::string z = flateCompress(out);
  if (z.empty()) return false;
  QPDFObjectHandle img = QPDFObjectHandle::newStream(&ctx.pdf, z);
  QPDFObjectHandle d = img.getDict();
  d.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
  d.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
  d.replaceKey("/Width", QPDFObjectHandle::newInteger(ow));
  d.replaceKey("/Height", QPDFObjectHandle::newInteger(oh));
  d.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
  d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
  d.replaceKey("/Filter", QPDFObjectHandle::newName("/FlateDecode"));
  img = ctx.pdf.makeIndirectObject(img);
  QPDFObjectHandle res = ph.getAttribute("/Resources", true);
  QPDFObjectHandle xo = res.getKey("/XObject");
  if (!xo.isDictionary()) {
    xo = QPDFObjectHandle::newDictionary();
    res.replaceKey("/XObject", xo);
  }
  std::string name = "/KuraBleed";
  xo.replaceKey(name, img);
  double x1 = t[0] - amt[2], y1 = t[1] - amt[1];
  double wpt = ow / sx, hpt = oh / sy;
  std::string clip = fmtFixed(x1, 3) + " " + fmtFixed(y1, 3) + " " + fmtFixed(t[2] + amt[3] - x1, 3) + " " +
                     fmtFixed(t[3] + amt[0] - y1, 3) + " re " + fmtFixed(t[0], 3) + " " + fmtFixed(t[1], 3) + " " +
                     fmtFixed(t[2] - t[0], 3) + " " + fmtFixed(t[3] - t[1], 3) + " re W* n ";
  std::string c = "q " + clip + fmtFixed(wpt, 4) + " 0 0 " + fmtFixed(hpt, 4) + " " + fmtFixed(x1, 4) + " " +
                  fmtFixed(t[3] + amt[0] - hpt, 4) + " cm " + name + " Do Q\n";
  ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, c), true);
  return true;
}
}

namespace {
struct FixupCtx {
  Ctx& ctx;
  std::vector<QPDFPageObjectHelper>& pages;
  const PfFix& fix;
  std::vector<int> pageIdx{};
  int pageCount = 0;
  int indexOf(size_t k) const { return k < pageIdx.size() ? pageIdx[k] : static_cast<int>(k); }
  const std::string& str(const char* name) const { return fixParam(fix, name); }
  double num(const char* name) const { return fixParamNum(fix, name); }
  bool flag(const char* name) const { return fixParamBool(fix, name); }
  bool has(const char* name) const { return !fixParam(fix, name).empty(); }
  void note(const std::string& what) const { ctx.issue("FIX_DONE", fix.id + ": " + what, true); }
  void fail(const std::string& what) const { ctx.issue("FIX_NOT_NEEDED", fix.id + ": " + what, false); }
};

std::string boxKey(const std::string& v) {
  if (v == "crop") return "CropBox";
  if (v == "bleed") return "BleedBox";
  if (v == "trim") return "TrimBox";
  if (v == "art") return "ArtBox";
  return "MediaBox";
}

bool parseColorText(const std::string& text, ColorValue& out) {
  std::istringstream in(text);
  std::string space;
  in >> space;
  space = lowerAscii(space);
  std::vector<double> v;
  double x = 0;
  while (in >> x) v.push_back(x);
  out.values.clear();
  if (space == "cmyk" && v.size() == 4) {
    out.family = "/DeviceCMYK";
    for (double c : v) out.values.push_back(c / 100.0);
  } else if (space == "rgb" && v.size() == 3) {
    bool bytes = std::max({v[0], v[1], v[2]}) > 1;
    out.family = "/DeviceRGB";
    for (double c : v) out.values.push_back(bytes ? c / 255.0 : c);
  } else if ((space == "gray" || space == "grey") && v.size() == 1) {
    out.family = "/DeviceGray";
    out.values = {v[0] / 100.0};
  } else if (space == "lab" && v.size() == 3) {
    out.family = "/Lab";
    out.values = v;
  } else {
    return false;
  }
  return true;
}

bool renderedContentBox(Ctx& ctx, QPDFPageObjectHelper& ph, int pageIndex, double dpi, double out[4]) {
  int w = 0, h = 0;
  std::string rgb;
  if (!ctx.opt.rasterizePage || !ctx.opt.rasterizePage(pageIndex, dpi, w, h, rgb) || w <= 0 || h <= 0 ||
      rgb.size() < static_cast<size_t>(w) * h * 3) {
    return false;
  }
  int minx = w, miny = h, maxx = -1, maxy = -1;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(rgb.data()) + (static_cast<size_t>(y) * w + x) * 3;
      if (p[0] < 250 || p[1] < 250 || p[2] < 250) {
        minx = std::min(minx, x);
        maxx = std::max(maxx, x);
        miny = std::min(miny, y);
        maxy = std::max(maxy, y);
      }
    }
  }
  if (maxx < 0) return false;
  QPDFObjectHandle cb = ph.getAttribute("/CropBox", false);
  if (!cb.isArray()) cb = ph.getAttribute("/MediaBox", true);
  if (!cb.isArray() || cb.getArrayNItems() != 4) return false;
  double x1 = numOf(cb.getArrayItem(0), 0), y2 = numOf(cb.getArrayItem(3), 0);
  double sx = (numOf(cb.getArrayItem(2), 0) - x1) / w, sy = (y2 - numOf(cb.getArrayItem(1), 0)) / h;
  out[0] = x1 + minx * sx;
  out[1] = y2 - (maxy + 1) * sy;
  out[2] = x1 + (maxx + 1) * sx;
  out[3] = y2 - miny * sy;
  return true;
}

QPDFObjectHandle newBox(double a, double b, double c, double d) {
  QPDFObjectHandle n = QPDFObjectHandle::newArray();
  for (double v : {a, b, c, d}) n.appendItem(QPDFObjectHandle::newReal(v, 2));
  return n;
}
}





namespace {
QPDFObjectHandle familySpace(const ColorValue& c) {
  if (c.family == "/Lab") {
    return QPDFObjectHandle::parse("[/Lab << /WhitePoint [0.9642 1 0.8249] /Range [-128 127 -128 127] >>]");
  }
  return QPDFObjectHandle::newName(c.family);
}

QPDFObjectHandle tintFunction(const ColorValue& c) {
  QPDFObjectHandle fn = QPDFObjectHandle::newDictionary();
  QPDFObjectHandle c0 = QPDFObjectHandle::newArray(), c1 = QPDFObjectHandle::newArray();
  for (size_t i = 0; i < c.values.size(); ++i) {
    double white = c.family == "/DeviceCMYK" ? 0 : c.family == "/Lab" ? (i == 0 ? 100 : 0) : 1;
    c0.appendItem(QPDFObjectHandle::newReal(white, 4));
    c1.appendItem(QPDFObjectHandle::newReal(c.values[i], 4));
  }
  fn.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(2));
  fn.replaceKey("/Domain", QPDFObjectHandle::parse("[0 1]"));
  fn.replaceKey("/C0", c0);
  fn.replaceKey("/C1", c1);
  fn.replaceKey("/N", QPDFObjectHandle::newInteger(1));
  return fn;
}

QPDFObjectHandle separationSpace(Ctx& ctx, const std::string& name, const ColorValue& look) {
  QPDFObjectHandle arr = QPDFObjectHandle::newArray();
  arr.appendItem(QPDFObjectHandle::newName("/Separation"));
  arr.appendItem(QPDFObjectHandle::newName("/" + name));
  arr.appendItem(familySpace(look));
  arr.appendItem(tintFunction(look));
  return ctx.pdf.makeIndirectObject(arr);
}

bool sameColor(const ColorValue& a, const ColorValue& b, double tol) {
  if (a.family != b.family || a.values.size() != b.values.size()) return false;
  for (size_t i = 0; i < a.values.size(); ++i) {
    if (std::fabs(a.values[i] - b.values[i]) > tol + 1e-6) return false;
  }
  return true;
}

std::string cp1252ToUtf8(const std::string& s) {
  static const unsigned short hi[32] = {
      0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
      0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
      0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
  std::string out;
  for (unsigned char c : s) {
    unsigned cp = c < 0x80 ? c : c < 0xA0 ? hi[c - 0x80] : c;
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

std::string collapseSpaces(const std::string& s) {
  std::string out;
  bool sp = false;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
      sp = !out.empty();
    } else {
      if (sp) out += ' ';
      sp = false;
      out += c;
    }
  }
  return out;
}

std::string normalizePantone(const std::string& name) {
  std::string l = lowerAscii(name);
  size_t at = l.find("pantone");
  if (at == std::string::npos) return name;
  std::string rest = name.substr(at + 7);
  if (!rest.empty() && static_cast<unsigned char>(rest[0]) == 0xC2 && rest.size() > 1) rest = rest.substr(2);
  if (!rest.empty() && rest[0] == '\xAE') rest = rest.substr(1);
  rest = collapseSpaces(rest);
  static const char* suffixes[] = {"CVC", "CVU", "CVP", "TPX", "TCX", "TPG", "XGC", "CV", "EC",
                                   "UP", "HC", "C", "U", "M"};
  std::string up = rest;
  for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  std::string core = rest, suffix;
  for (const char* sfx : suffixes) {
    size_t n = std::strlen(sfx);
    if (up.size() > n && up.compare(up.size() - n, n, sfx) == 0) {
      char before = up[up.size() - n - 1];
      if (before == ' ' || std::isdigit(static_cast<unsigned char>(before))) {
        core = rest.substr(0, rest.size() - n);
        while (!core.empty() && core.back() == ' ') core.pop_back();
        suffix = sfx;
        break;
      }
    }
  }
  if (core.empty()) return name;
  return "PANTONE " + core + (suffix.empty() ? "" : " " + suffix);
}

std::string normalizeHks(const std::string& name) {
  std::string l = lowerAscii(name);
  size_t at = l.find("hks");
  if (at == std::string::npos) return name;
  std::string rest = collapseSpaces(name.substr(at + 3));
  std::string digits, suffix;
  size_t i = 0;
  while (i < rest.size() && rest[i] == ' ') ++i;
  while (i < rest.size() && std::isdigit(static_cast<unsigned char>(rest[i]))) digits += rest[i++];
  while (i < rest.size() && rest[i] == ' ') ++i;
  if (i < rest.size()) suffix = rest.substr(i);
  for (char& c : suffix) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (digits.empty() || (suffix != "K" && suffix != "N" && suffix != "E" && suffix != "Z")) {
    return name;
  }
  return "HKS " + digits + " " + suffix;
}

QPDFObjectHandle deviceFor(QPDFObjectHandle cs) {
  if (!cs.isArray() || cs.getArrayNItems() < 2 || !nameIs(cs.getArrayItem(0), "/ICCBased")) {
    return QPDFObjectHandle();
  }
  QPDFObjectHandle st = cs.getArrayItem(1);
  long long n = st.isStream() && st.getDict().getKey("/N").isInteger()
                    ? st.getDict().getKey("/N").getIntValue()
                    : 0;
  if (n == 1) return QPDFObjectHandle::newName("/DeviceGray");
  if (n == 3) return QPDFObjectHandle::newName("/DeviceRGB");
  if (n == 4) return QPDFObjectHandle::newName("/DeviceCMYK");
  return QPDFObjectHandle();
}

void replaceIccInTree(QPDFObjectHandle o, std::set<QPDFObjGen>& seen, int depth, int& n) {
  if (depth > kMaxObjectWalk) return;
  if (o.isIndirect() && !seen.insert(o.getObjGen()).second) return;
  if (o.isStream()) o = o.getDict();
  if (o.isDictionary()) {
    for (const std::string& k : o.getKeys()) {
      QPDFObjectHandle v = o.getKey(k);
      if (k == "/ColorSpace" && v.isDictionary()) {
        for (const std::string& ck : v.getKeys()) {
          if (ck == "/DefaultRGB" || ck == "/DefaultCMYK" || ck == "/DefaultGray") {
            if (deviceFor(v.getKey(ck)).isInitialized()) {
              v.removeKey(ck);
              ++n;
            }
            continue;
          }
          QPDFObjectHandle d = deviceFor(v.getKey(ck));
          if (d.isInitialized()) {
            v.replaceKey(ck, d);
            ++n;
          } else {
            replaceIccInTree(v.getKey(ck), seen, depth + 1, n);
          }
        }
        continue;
      }
      if (k == "/ColorSpace" || k == "/CS") {
        QPDFObjectHandle d = deviceFor(v);
        if (d.isInitialized()) {
          o.replaceKey(k, d);
          ++n;
          continue;
        }
      }
      if (k == "/Parent" || k == "/P" || k == "/DestOutputProfile") continue;
      if (v.isDictionary() || v.isArray() || v.isStream()) replaceIccInTree(v, seen, depth + 1, n);
    }
  } else if (o.isArray()) {
    std::string fam = o.getArrayNItems() > 0 ? nameOf(o.getArrayItem(0)) : std::string();
    int slot = (fam == "/Indexed" || fam == "/I" || fam == "/Pattern") ? 1
               : (fam == "/Separation" || fam == "/DeviceN") ? 2 : -1;
    if (slot > 0 && o.getArrayNItems() > slot) {
      QPDFObjectHandle d = deviceFor(o.getArrayItem(slot));
      if (d.isInitialized()) {
        o.setArrayItem(slot, d);
        ++n;
      }
    }
    for (int i = 0; i < o.getArrayNItems(); ++i) {
      QPDFObjectHandle v = o.getArrayItem(i);
      if (v.isDictionary() || v.isArray() || v.isStream()) replaceIccInTree(v, seen, depth + 1, n);
    }
  }
}

bool downgradeIcc(QPDFObjectHandle st) {
  std::string b;
  try {
    auto buf = st.getStreamData(qpdf_dl_all);
    b.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return false;
  }
  if (b.size() < 132 || static_cast<unsigned char>(b[8]) < 4) return false;
  cmsHPROFILE src = cmsOpenProfileFromMem(b.data(), static_cast<cmsUInt32Number>(b.size()));
  if (!src) return false;
  cmsHPROFILE dst = cmsCreateProfilePlaceholder(nullptr);
  bool ok = dst != nullptr;
  if (ok) {
    cmsSetProfileVersion(dst, 2.4);
    cmsSetDeviceClass(dst, cmsGetDeviceClass(src));
    cmsSetColorSpace(dst, cmsGetColorSpace(src));
    cmsSetPCS(dst, cmsGetPCS(src));
    cmsSetHeaderRenderingIntent(dst, cmsGetHeaderRenderingIntent(src));
    cmsInt32Number tags = cmsGetTagCount(src);
    for (cmsInt32Number i = 0; i < tags; ++i) {
      cmsTagSignature sig = cmsGetTagSignature(src, static_cast<cmsUInt32Number>(i));
      void* data = cmsReadTag(src, sig);
      if (data) cmsWriteTag(dst, sig, data);
    }
    cmsUInt32Number n = 0;
    std::string out;
    if (cmsSaveProfileToMem(dst, nullptr, &n) && n > 132) {
      out.resize(n);
      if (!cmsSaveProfileToMem(dst, &out[0], &n)) out.clear();
    }
    cmsHPROFILE check = out.empty() ? nullptr
                                    : cmsOpenProfileFromMem(out.data(), static_cast<cmsUInt32Number>(out.size()));
    ok = check != nullptr;
    if (check) cmsCloseProfile(check);
    if (ok) st.replaceStreamData(out, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    cmsCloseProfile(dst);
  }
  cmsCloseProfile(src);
  return ok;
}
}

namespace {
std::vector<std::vector<std::pair<double, double>>> traceMask(const std::vector<unsigned char>& on, int w, int h,
                                                               double tol, size_t maxPoints) {
  std::vector<std::vector<std::pair<double, double>>> loops;
  const int vw = w + 1;
  std::vector<std::array<int, 2>> out(static_cast<size_t>(vw) * (h + 1), {-1, -1});
  auto lit = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && on[static_cast<size_t>(y) * w + x]; };
  auto addEdge = [&](int ax, int ay, int bx, int by) {
    auto& o = out[static_cast<size_t>(ay) * vw + ax];
    o[o[0] < 0 ? 0 : 1] = by * vw + bx;
  };
  size_t edges = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (!lit(x, y)) continue;
      if (!lit(x, y - 1)) addEdge(x, y, x + 1, y), ++edges;
      if (!lit(x + 1, y)) addEdge(x + 1, y, x + 1, y + 1), ++edges;
      if (!lit(x, y + 1)) addEdge(x + 1, y + 1, x, y + 1), ++edges;
      if (!lit(x - 1, y)) addEdge(x, y + 1, x, y), ++edges;
    }
  }
  if (edges > maxPoints * 8) return {};
  std::function<void(const std::vector<std::pair<double, double>>&, size_t, size_t, std::vector<char>&)> dp;
  dp = [&](const std::vector<std::pair<double, double>>& pts, size_t a, size_t b, std::vector<char>& keep) {
    if (b <= a + 1) return;
    double ax = pts[a].first, ay = pts[a].second, bx = pts[b].first, by = pts[b].second;
    double dx = bx - ax, dy = by - ay, len = std::hypot(dx, dy);
    double best = -1;
    size_t at = a;
    for (size_t i = a + 1; i < b; ++i) {
      double d = len > 0 ? std::fabs(dy * (pts[i].first - ax) - dx * (pts[i].second - ay)) / len
                         : std::hypot(pts[i].first - ax, pts[i].second - ay);
      if (d > best) {
        best = d;
        at = i;
      }
    }
    if (best > tol) {
      keep[at] = 1;
      dp(pts, a, at, keep);
      dp(pts, at, b, keep);
    }
  };
  size_t total = 0;
  for (size_t start = 0; start < out.size(); ++start) {
    while (out[start][0] >= 0) {
      std::vector<std::pair<double, double>> pts;
      size_t v = start;
      do {
        auto& o = out[v];
        int nx = o[1] >= 0 ? o[1] : o[0];
        if (o[1] >= 0) o[1] = -1;
        else o[0] = -1;
        double px = static_cast<double>(v % vw), py = static_cast<double>(v / vw);
        size_t nv = static_cast<size_t>(nx);
        double qx = static_cast<double>(nv % vw), qy = static_cast<double>(nv / vw);
        pts.push_back({(px + qx) / 2, (py + qy) / 2});
        v = nv;
      } while (v != start && out[v][0] >= 0);
      if (pts.size() < 3) continue;
      std::vector<char> keep(pts.size() + 1, 0);
      std::vector<std::pair<double, double>> ring(pts);
      ring.push_back(pts.front());
      size_t far = 0;
      double fd = -1;
      for (size_t i = 1; i < pts.size(); ++i) {
        double d = std::hypot(pts[i].first - pts[0].first, pts[i].second - pts[0].second);
        if (d > fd) {
          fd = d;
          far = i;
        }
      }
      keep[0] = keep[far] = keep[pts.size()] = 1;
      dp(ring, 0, far, keep);
      dp(ring, far, pts.size(), keep);
      std::vector<std::pair<double, double>> simple;
      for (size_t i = 0; i < pts.size(); ++i) {
        if (keep[i]) simple.push_back(pts[i]);
      }
      if (simple.size() < 3) continue;
      total += simple.size();
      if (total > maxPoints) return {};
      loops.push_back(std::move(simple));
    }
  }
  return loops;
}

std::vector<double> squaredDistance(const std::vector<unsigned char>& on, int w, int h) {
  const double inf = 1e20;
  std::vector<double> d(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < d.size(); ++i) d[i] = on[i] ? 0 : inf;
  auto pass = [&](double* f, int n, int stride) {
    std::vector<double> g(n), z(n + 1);
    std::vector<int> v(n);
    for (int i = 0; i < n; ++i) g[i] = f[static_cast<size_t>(i) * stride];
    int k = 0;
    v[0] = 0;
    z[0] = -inf;
    z[1] = inf;
    for (int q = 1; q < n; ++q) {
      double sq = ((g[q] + double(q) * q) - (g[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
      while (sq <= z[k]) {
        --k;
        sq = ((g[q] + double(q) * q) - (g[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
      }
      ++k;
      v[k] = q;
      z[k] = sq;
      z[k + 1] = inf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
      while (z[k + 1] < q) ++k;
      f[static_cast<size_t>(q) * stride] = double(q - v[k]) * (q - v[k]) + g[v[k]];
    }
  };
  for (int x = 0; x < w; ++x) pass(&d[static_cast<size_t>(x)], h, w);
  for (int y = 0; y < h; ++y) pass(&d[static_cast<size_t>(y) * w], w, 1);
  return d;
}

void keepInside(std::vector<unsigned char>& on, int w, int h, QPDFPageObjectHelper& ph, const std::vector<Box>& boxes,
                double growPt) {
  QPDFObjectHandle cb = ph.getAttribute("/CropBox", false);
  if (!cb.isArray()) cb = ph.getAttribute("/MediaBox", true);
  if (!cb.isArray() || cb.getArrayNItems() != 4) return;
  double x1 = numOf(cb.getArrayItem(0), 0), y1 = numOf(cb.getArrayItem(1), 0);
  double x2 = numOf(cb.getArrayItem(2), 0), y2 = numOf(cb.getArrayItem(3), 0);
  double sx = (x2 - x1) / w, sy = (y2 - y1) / h;
  std::vector<unsigned char> keep(on.size(), 0);
  for (const Box& b : boxes) {
    int px0 = std::max(0, static_cast<int>(std::floor((b.x0 - growPt - x1) / sx)));
    int px1 = std::min(w, static_cast<int>(std::ceil((b.x1 + growPt - x1) / sx)));
    int py0 = std::max(0, static_cast<int>(std::floor((y2 - b.y1 - growPt) / sy)));
    int py1 = std::min(h, static_cast<int>(std::ceil((y2 - b.y0 + growPt) / sy)));
    for (int y = py0; y < py1; ++y) {
      for (int x = px0; x < px1; ++x) keep[static_cast<size_t>(y) * w + x] = 1;
    }
  }
  for (size_t i = 0; i < on.size(); ++i) on[i] = on[i] && keep[i];
}

bool placeShapeBleed(Ctx& ctx, QPDFPageObjectHelper& ph, int pageIndex, double dpi, double widthPt,
                     const std::string& setBox, bool mirror, const std::vector<Box>* contour = nullptr) {
  int w = 0, h = 0;
  std::string rgb;
  if (!ctx.opt.rasterizePage || !ctx.opt.rasterizePage(pageIndex, dpi, w, h, rgb) || w <= 0 || h <= 0 ||
      rgb.size() < static_cast<size_t>(w) * h * 3) {
    return false;
  }
  QPDFObjectHandle cb = ph.getAttribute("/CropBox", false);
  if (!cb.isArray()) cb = ph.getAttribute("/MediaBox", true);
  if (!cb.isArray() || cb.getArrayNItems() != 4) return false;
  double x1 = numOf(cb.getArrayItem(0), 0), y1 = numOf(cb.getArrayItem(1), 0);
  double x2 = numOf(cb.getArrayItem(2), 0), y2 = numOf(cb.getArrayItem(3), 0);
  const unsigned char* src = reinterpret_cast<const unsigned char*>(rgb.data());
  size_t n = static_cast<size_t>(w) * h;
  std::vector<unsigned char> on(n, 0);
  for (size_t i = 0; i < n; ++i) {
    const unsigned char* q = src + i * 3;
    on[i] = (q[0] < 250 || q[1] < 250 || q[2] < 250) ? 1 : 0;
  }
  int radius = std::max(1, static_cast<int>(std::lround(widthPt * dpi / 72.0)));
  const int nb[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
  auto spread = [&](const std::vector<unsigned char>& seed, int limit, std::vector<int>& dist, std::vector<int>* from) {
    dist.assign(n, -1);
    std::vector<size_t> queue;
    for (size_t i = 0; i < n; ++i) {
      if (!seed[i]) continue;
      dist[i] = 0;
      if (from) (*from)[i] = static_cast<int>(i);
      queue.push_back(i);
    }
    for (size_t qi = 0; qi < queue.size(); ++qi) {
      size_t i = queue[qi];
      if (dist[i] >= limit) continue;
      int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
      for (const auto& d : nb) {
        int nx = x + d[0], ny = y + d[1];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
        size_t j = static_cast<size_t>(ny) * w + nx;
        if (dist[j] >= 0) continue;
        dist[j] = dist[i] + 1;
        if (from) {
          (*from)[j] = (*from)[i];
          if (mirror) {
            int sxp = (*from)[i] % w, syp = (*from)[i] / w;
            int mx = 2 * sxp - nx, my = 2 * syp - ny;
            if (mx >= 0 && my >= 0 && mx < w && my < h && seed[static_cast<size_t>(my) * w + mx]) (*from)[j] = my * w + mx;
          }
        }
        queue.push_back(j);
      }
    }
  };
  std::vector<int> cdist, from(n, -1);
  std::vector<double> dist2 = squaredDistance(on, w, h);
  std::vector<unsigned char> core(on);
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<unsigned char> next(core);
    for (size_t i = 0; i < n; ++i) {
      if (!core[i]) continue;
      int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
      if (x == 0 || y == 0 || x == w - 1 || y == h - 1 || !core[i - 1] || !core[i + 1] || !core[i - w] || !core[i + w]) next[i] = 0;
    }
    if (std::find(next.begin(), next.end(), 1) == next.end()) break;
    core.swap(next);
  }
  if (std::find(on.begin(), on.end(), 1) == on.end()) return false;
  if (mirror) spread(core, radius + 6, cdist, &from);
  else from = nearestSeed(core, w, h);
  std::string img(n * 3, '\xff');
  std::vector<unsigned char> band(n, 0);
  int bx0 = w, by0 = h, bx1 = -1, by1 = -1;
  std::vector<unsigned char> allowed(n, 1);
  if (contour) keepInside(allowed, w, h, ph, *contour, widthPt);
  for (size_t i = 0; i < n; ++i) {
    if (from[i] < 0 || !allowed[i] || dist2[i] > static_cast<double>(radius) * radius) continue;
    band[i] = 1;
    std::memcpy(&img[i * 3], src + static_cast<size_t>(from[i]) * 3, 3);
    int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
    bx0 = std::min(bx0, x);
    by0 = std::min(by0, y);
    bx1 = std::max(bx1, x + 1);
    by1 = std::max(by1, y + 1);
  }
  auto loops = traceMask(band, w, h, 0.6, 400000);
  if (loops.empty()) return false;
  std::string z = flateCompress(img);
  if (z.empty()) return false;
  QPDFObjectHandle im = QPDFObjectHandle::newStream(&ctx.pdf, z);
  QPDFObjectHandle d = im.getDict();
  d.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
  d.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
  d.replaceKey("/Width", QPDFObjectHandle::newInteger(w));
  d.replaceKey("/Height", QPDFObjectHandle::newInteger(h));
  d.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
  d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
  d.replaceKey("/Filter", QPDFObjectHandle::newName("/FlateDecode"));
  QPDFObjectHandle res = ph.getAttribute("/Resources", true);
  QPDFObjectHandle xo = res.getKey("/XObject");
  if (!xo.isDictionary()) {
    xo = QPDFObjectHandle::newDictionary();
    res.replaceKey("/XObject", xo);
  }
  xo.replaceKey("/KuraShapeBleed", ctx.pdf.makeIndirectObject(im));
  double sx = (x2 - x1) / w, sy = (y2 - y1) / h;
  std::string c = "q\n";
  for (const auto& lp : loops) {
    for (size_t i = 0; i < lp.size(); ++i) {
      c += fmtFixed(x1 + lp[i].first * sx, 2) + " " + fmtFixed(y2 - lp[i].second * sy, 2) + (i ? " l\n" : " m\n");
    }
    c += "h\n";
  }
  c += "W* n " + fmtFixed(x2 - x1, 4) + " 0 0 " + fmtFixed(y2 - y1, 4) + " " + fmtFixed(x1, 4) + " " + fmtFixed(y1, 4) +
       " cm /KuraShapeBleed Do Q\n";
  ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, c), true);
  std::string mode = lowerAscii(setBox);
  bool hasBleed = ph.getObjectHandle().getKey("/BleedBox").isArray();
  if (mode.find("never") == std::string::npos && (mode.find("missing") == std::string::npos || !hasBleed)) {
    QPDFObjectHandle tb = ph.getAttribute("/TrimBox", false);
    double t0 = x1 + bx0 * sx, t1 = y2 - by1 * sy, t2 = x1 + bx1 * sx, t3 = y2 - by0 * sy;
    if (tb.isArray() && tb.getArrayNItems() == 4) {
      t0 = std::min(t0, numOf(tb.getArrayItem(0), t0));
      t1 = std::min(t1, numOf(tb.getArrayItem(1), t1));
      t2 = std::max(t2, numOf(tb.getArrayItem(2), t2));
      t3 = std::max(t3, numOf(tb.getArrayItem(3), t3));
    }
    ph.getObjectHandle().replaceKey("/BleedBox", QPDFObjectHandle::parse("[" + fmtFixed(std::max(t0, x1), 3) + " " +
                                                                         fmtFixed(std::max(t1, y1), 3) + " " +
                                                                         fmtFixed(std::min(t2, x2), 3) + " " +
                                                                         fmtFixed(std::min(t3, y2), 3) + "]"));
  }
  return true;
}

double tileExactDpi(QPDFPageObjectHelper& ph, double dpi) {
  QPDFObjectHandle res = ph.getAttribute("/Resources", false);
  QPDFObjectHandle pats = res.isDictionary() ? res.getKey("/Pattern") : QPDFObjectHandle();
  if (!pats.isInitialized() || !pats.isDictionary()) return dpi;
  for (const std::string& k : pats.getKeys()) {
    QPDFObjectHandle p = pats.getKey(k);
    if (!p.isStream() || numOf(p.getDict().getKey("/PatternType"), 0) != 1) continue;
    QPDFObjectHandle m = p.getDict().getKey("/Matrix");
    double a = m.isArray() && m.getArrayNItems() == 6 ? numOf(m.getArrayItem(0), 1) : 1;
    double b = m.isArray() && m.getArrayNItems() == 6 ? numOf(m.getArrayItem(1), 0) : 0;
    double step = std::fabs(numOf(p.getDict().getKey("/XStep"), 0)) * std::hypot(a, b);
    if (step <= 0) continue;
    double cells = std::max(1.0, std::round(step * dpi / 72));
    return cells * 72 / step;
  }
  return dpi;
}

bool placeWhitePlate(Ctx& ctx, QPDFPageObjectHelper& ph, int pageIndex, double dpi,
                     const std::string& spot, const ColorValue& look, int choke, bool whole,
                     const std::vector<Box>* objects = nullptr) {
  dpi = tileExactDpi(ph, dpi);
  int w = 0, h = 0;
  std::string rgb;
  if (whole) {
    w = h = 4;
    rgb.assign(static_cast<size_t>(w) * h * 3, '\0');
  } else if (!ctx.opt.rasterizePage || !ctx.opt.rasterizePage(pageIndex, dpi, w, h, rgb) || w <= 0 ||
             h <= 0 || rgb.size() < static_cast<size_t>(w) * h * 3) {
    return false;
  }
  std::vector<unsigned char> on(static_cast<size_t>(w) * h, 0);
  bool any = false;
  for (size_t i = 0; i < on.size(); ++i) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(rgb.data()) + i * 3;
    on[i] = (p[0] < 250 || p[1] < 250 || p[2] < 250) ? 1 : 0;
  }
  if (objects && !whole) keepInside(on, w, h, ph, *objects, 0);
  any = std::find(on.begin(), on.end(), 1) != on.end();
  if (!any) return false;
  for (int pass = 0; pass < choke; ++pass) {
    std::vector<unsigned char> next(on);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        size_t i = static_cast<size_t>(y) * w + x;
        if (!on[i]) continue;
        if (x == 0 || y == 0 || x == w - 1 || y == h - 1 || !on[i - 1] || !on[i + 1] || !on[i - w] || !on[i + w]) next[i] = 0;
      }
    }
    on.swap(next);
  }
  auto loops = traceMask(on, w, h, 0.6, 400000);
  if (whole) loops = {{{0.0, 0.0}, {static_cast<double>(w), 0.0}, {static_cast<double>(w), static_cast<double>(h)}, {0.0, static_cast<double>(h)}}};
  size_t row = (static_cast<size_t>(w) + 7) / 8;
  std::string bits(row * h, '\xff');
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (on[static_cast<size_t>(y) * w + x]) bits[y * row + x / 8] &= static_cast<char>(~(0x80 >> (x % 8)));
    }
  }
  std::string z = flateCompress(bits);
  if (z.empty()) return false;
  QPDFObjectHandle mask = QPDFObjectHandle::newStream(&ctx.pdf, z);
  QPDFObjectHandle md = mask.getDict();
  md.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
  md.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
  md.replaceKey("/Width", QPDFObjectHandle::newInteger(w));
  md.replaceKey("/Height", QPDFObjectHandle::newInteger(h));
  md.replaceKey("/ImageMask", QPDFObjectHandle::newBool(true));
  md.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(1));
  md.replaceKey("/Filter", QPDFObjectHandle::newName("/FlateDecode"));
  mask = ctx.pdf.makeIndirectObject(mask);
  QPDFObjectHandle sep = separationSpace(ctx, spot, look);
  QPDFObjectHandle gs = QPDFObjectHandle::parse("<< /Type /ExtGState /OP true /op true /OPM 1 >>");
  QPDFObjectHandle res = ph.getAttribute("/Resources", true);
  auto sub = [&](const char* key) {
    QPDFObjectHandle d = res.getKey(key);
    if (!d.isDictionary()) {
      d = QPDFObjectHandle::newDictionary();
      res.replaceKey(key, d);
    }
    return d;
  };
  sub("/XObject").replaceKey("/KuraWhiteMask", mask);
  sub("/ColorSpace").replaceKey("/KuraWhite", sep);
  sub("/ExtGState").replaceKey("/KuraWhiteGS", ctx.pdf.makeIndirectObject(gs));
  QPDFObjectHandle root = ctx.pdf.getRoot();
  QPDFObjectHandle ocp = root.getKey("/OCProperties");
  if (!ocp.isDictionary()) {
    ocp = QPDFObjectHandle::parse("<< /OCGs [] /D << /Order [] >> >>");
    root.replaceKey("/OCProperties", ocp);
  }
  QPDFObjectHandle ocg;
  QPDFObjectHandle ocgs = ocp.getKey("/OCGs");
  for (int i = 0; ocgs.isArray() && i < ocgs.getArrayNItems(); ++i) {
    QPDFObjectHandle g = ocgs.getArrayItem(i);
    if (g.isDictionary() && g.getKey("/Name").isString() && g.getKey("/Name").getUTF8Value() == spot) ocg = g;
  }
  if (!ocg.isInitialized()) {
    ocg = ctx.pdf.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /OCG >>"));
    ocg.replaceKey("/Name", QPDFObjectHandle::newUnicodeString(spot));
    if (!ocgs.isArray()) {
      ocgs = QPDFObjectHandle::newArray();
      ocp.replaceKey("/OCGs", ocgs);
    }
    ocgs.appendItem(ocg);
    QPDFObjectHandle d = ocp.getKey("/D");
    if (!d.isDictionary()) {
      d = QPDFObjectHandle::newDictionary();
      ocp.replaceKey("/D", d);
    }
    QPDFObjectHandle order = d.getKey("/Order");
    if (!order.isArray()) {
      order = QPDFObjectHandle::newArray();
      d.replaceKey("/Order", order);
    }
    order.appendItem(ocg);
  }
  sub("/Properties").replaceKey("/KuraWhiteOC", ocg);
  QPDFObjectHandle cb = ph.getAttribute("/CropBox", false);
  if (!cb.isArray()) cb = ph.getAttribute("/MediaBox", true);
  double x1 = numOf(cb.getArrayItem(0), 0), y1 = numOf(cb.getArrayItem(1), 0);
  double x2 = numOf(cb.getArrayItem(2), 0), y2 = numOf(cb.getArrayItem(3), 0);
  std::string c;
  if (!loops.empty()) {
    double sx = (x2 - x1) / w, sy = (y2 - y1) / h;
    c = "\nq /OC /KuraWhiteOC BDC /KuraWhiteGS gs /KuraWhite cs 1 scn\n";
    for (const auto& lp : loops) {
      for (size_t i = 0; i < lp.size(); ++i) {
        c += fmtFixed(x1 + lp[i].first * sx, 2) + " " + fmtFixed(y2 - lp[i].second * sy, 2) + (i ? " l\n" : " m\n");
      }
      c += "h\n";
    }
    c += "f* EMC Q\n";
    res.getKey("/XObject").removeKey("/KuraWhiteMask");
  } else {
    c = "\nq /OC /KuraWhiteOC BDC /KuraWhiteGS gs /KuraWhite cs 1 scn " + fmtFixed(x2 - x1, 4) +
        " 0 0 " + fmtFixed(y2 - y1, 4) + " " + fmtFixed(x1, 4) + " " + fmtFixed(y1, 4) +
        " cm /KuraWhiteMask Do EMC Q\n";
  }
  ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, std::string("q\n")), true);
  ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, "\nQ" + c), false);
  return true;
}
}

namespace {
struct Line {
  int page = 0;
  std::string text;
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0, size = 0, baseline = 0;
  std::vector<size_t> ops;
  bool tagged = false;
  bool artifact = false;
  bool form = false;
};

std::vector<Line> pageLines(int page, const std::vector<TextRun>& runs) {
  std::vector<Line> lines;
  for (const TextRun& r : runs) {
    double base = r.y0 + 0.25 * r.size;
    Line* hit = nullptr;
    for (auto& l : lines) {
      if (std::fabs(l.baseline - base) < 0.4 * std::max(1.0, std::min(l.size, r.size)) &&
          std::fabs(l.size - r.size) < 0.25 * std::max(l.size, r.size) && r.x0 >= l.x1 - r.size * 0.5 &&
          r.x0 - l.x1 < r.size * 3) {
        hit = &l;
      }
    }
    if (!hit) {
      lines.push_back(Line());
      hit = &lines.back();
      hit->page = page;
      hit->x0 = r.x0;
      hit->y0 = r.y0;
      hit->x1 = r.x1;
      hit->y1 = r.y1;
      hit->size = r.size;
      hit->baseline = base;
    } else if (!hit->text.empty() && hit->text.back() != ' ' && r.x0 - hit->x1 > 0.15 * r.size) {
      hit->text += ' ';
    }
    hit->text += r.text;
    hit->x0 = std::min(hit->x0, r.x0);
    hit->x1 = std::max(hit->x1, r.x1);
    hit->y0 = std::min(hit->y0, r.y0);
    hit->y1 = std::max(hit->y1, r.y1);
    hit->ops.push_back(r.op);
    hit->tagged = hit->tagged || r.mcid >= 0;
    hit->artifact = hit->artifact || r.artifact;
    hit->form = hit->form || r.inForm;
  }
  return lines;
}

std::string trimmed(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return std::string();
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

class WrapOpsFilter : public QPDFObjectHandle::TokenFilter {
 public:
  WrapOpsFilter(const std::map<size_t, std::string>& wrap) : wrap(wrap) {}
  void handleToken(QPDFTokenizer::Token const& tok) override {
    if (tok.getType() != QPDFTokenizer::tt_word) {
      pending.push_back(tok);
      return;
    }
    ++index;
    auto it = wrap.find(index);
    if (it != wrap.end()) {
      write(" " + it->second + " BDC ");
      for (auto& t : pending) writeToken(t);
      pending.clear();
      writeToken(tok);
      write(" EMC ");
      return;
    }
    for (auto& t : pending) writeToken(t);
    pending.clear();
    writeToken(tok);
  }
  void handleEOF() override {
    for (auto& t : pending) writeToken(t);
    pending.clear();
  }

 private:
  const std::map<size_t, std::string>& wrap;
  std::vector<QPDFTokenizer::Token> pending;
  size_t index = 0;
};

QPDFObjectHandle outlineItem(Ctx& ctx, const std::string& title, QPDFObjectHandle page, double x,
                             double y) {
  QPDFObjectHandle it = QPDFObjectHandle::newDictionary();
  it.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(title));
  QPDFObjectHandle dest = QPDFObjectHandle::newArray();
  dest.appendItem(page);
  dest.appendItem(QPDFObjectHandle::newName("/XYZ"));
  dest.appendItem(QPDFObjectHandle::newReal(x, 2));
  dest.appendItem(QPDFObjectHandle::newReal(y, 2));
  dest.appendItem(QPDFObjectHandle::newNull());
  it.replaceKey("/Dest", dest);
  return ctx.pdf.makeIndirectObject(it);
}

void linkChildren(QPDFObjectHandle parent, std::vector<QPDFObjectHandle>& kids) {
  for (size_t i = 0; i < kids.size(); ++i) {
    kids[i].replaceKey("/Parent", parent);
    if (i) kids[i].replaceKey("/Prev", kids[i - 1]);
    if (i + 1 < kids.size()) kids[i].replaceKey("/Next", kids[i + 1]);
  }
  if (!kids.empty()) {
    parent.replaceKey("/First", kids.front());
    parent.replaceKey("/Last", kids.back());
    parent.replaceKey("/Count", QPDFObjectHandle::newInteger(static_cast<long long>(kids.size())));
  }
}

int headingLevel(const std::string& type) {
  if (type == "/H" || type == "/H1") return 1;
  if (type.size() == 3 && type[1] == 'H' && type[2] >= '2' && type[2] <= '6') return type[2] - '0';
  return 0;
}

void collectHeadings(QPDFObjectHandle node, QPDFObjectHandle roleMap, QPDFObjectHandle inheritedPage,
                     std::vector<std::tuple<int, QPDFObjectHandle, std::vector<int>>>& out,
                     std::set<QPDFObjGen>& seen, int depth) {
  if (depth > kMaxStructDepth || !node.isDictionary()) return;
  if (node.isIndirect() && !seen.insert(node.getObjGen()).second) return;
  QPDFObjectHandle pg = node.getKey("/Pg").isDictionary() ? node.getKey("/Pg") : inheritedPage;
  std::string type = nameOf(node.getKey("/S"));
  if (roleMap.isDictionary() && roleMap.hasKey(type)) type = nameOf(roleMap.getKey(type));
  int level = headingLevel(type);
  QPDFObjectHandle k = node.getKey("/K");
  std::vector<QPDFObjectHandle> kids;
  if (k.isArray()) {
    for (int i = 0; i < k.getArrayNItems(); ++i) kids.push_back(k.getArrayItem(i));
  } else if (!k.isNull()) {
    kids.push_back(k);
  }
  if (level) {
    std::vector<int> mcids;
    std::function<void(QPDFObjectHandle, int)> gather = [&](QPDFObjectHandle n, int d) {
      if (d > 32) return;
      if (n.isInteger()) {
        mcids.push_back(static_cast<int>(n.getIntValue()));
      } else if (n.isDictionary() && n.getKey("/MCID").isInteger()) {
        mcids.push_back(static_cast<int>(n.getKey("/MCID").getIntValue()));
      } else if (n.isDictionary()) {
        QPDFObjectHandle kk = n.getKey("/K");
        if (kk.isArray()) {
          for (int i = 0; i < kk.getArrayNItems(); ++i) gather(kk.getArrayItem(i), d + 1);
        } else {
          gather(kk, d + 1);
        }
      }
    };
    for (auto& kid : kids) gather(kid, 0);
    out.emplace_back(level, pg, mcids);
    return;
  }
  for (auto& kid : kids) {
    if (kid.isDictionary() && !kid.hasKey("/MCID")) collectHeadings(kid, roleMap, pg, out, seen, depth + 1);
  }
}
}

namespace {
const std::set<std::string>& standardTypes() {
  static const std::set<std::string> s = {
      "/Document", "/Part", "/Art", "/Sect", "/Div", "/BlockQuote", "/Caption", "/TOC", "/TOCI",
      "/Index", "/NonStruct", "/Private", "/P", "/H", "/H1", "/H2", "/H3", "/H4", "/H5", "/H6",
      "/L", "/LI", "/Lbl", "/LBody", "/Table", "/TR", "/TH", "/TD", "/THead", "/TBody", "/TFoot",
      "/Span", "/Quote", "/Note", "/Reference", "/BibEntry", "/Code", "/Link", "/Annot", "/Ruby",
      "/RB", "/RT", "/RP", "/Warichu", "/WT", "/WP", "/Figure", "/Formula", "/Form"};
  return s;
}

std::string guessStandard(const std::string& type, bool hasContent) {
  std::string l = lowerAscii(type);
  auto has = [&](const char* k) { return l.find(k) != std::string::npos; };
  if (has("heading") || has("title")) return "/H1";
  if (has("caption")) return "/Caption";
  if (has("footnote") || has("endnote") || has("note")) return "/Note";
  if (has("table")) return "/Table";
  if (has("row")) return "/TR";
  if (has("cell")) return "/TD";
  if (has("listitem") || has("item")) return "/LI";
  if (has("list")) return "/L";
  if (has("figure") || has("image") || has("picture") || has("graphic")) return "/Figure";
  if (has("link")) return "/Link";
  if (has("quote")) return "/Quote";
  if (has("code")) return "/Code";
  if (has("section")) return "/Sect";
  if (has("article")) return "/Art";
  if (has("para") || has("body") || has("text")) return "/P";
  return hasContent ? "/P" : "/Div";
}

void walkStruct(QPDFObjectHandle node, std::set<QPDFObjGen>& seen, int depth,
                const std::function<void(QPDFObjectHandle)>& fn) {
  if (depth > kMaxStructDepth || !node.isDictionary()) return;
  if (node.isIndirect() && !seen.insert(node.getObjGen()).second) return;
  if (node.hasKey("/S")) fn(node);
  QPDFObjectHandle k = node.getKey("/K");
  if (k.isArray()) {
    for (int i = 0; i < k.getArrayNItems(); ++i) walkStruct(k.getArrayItem(i), seen, depth + 1, fn);
  } else if (k.isDictionary()) {
    walkStruct(k, seen, depth + 1, fn);
  }
}

bool hasContentKids(QPDFObjectHandle e) {
  QPDFObjectHandle k = e.getKey("/K");
  auto isContent = [](QPDFObjectHandle x) { return x.isInteger() || (x.isDictionary() && (x.hasKey("/MCID") || nameIs(x.getKey("/Type"), "/OBJR"))); };
  if (k.isArray()) {
    for (int i = 0; i < k.getArrayNItems(); ++i) {
      if (isContent(k.getArrayItem(i))) return true;
    }
    return false;
  }
  return isContent(k);
}

void appearanceStream(Ctx& ctx, QPDFObjectHandle w, QPDFObjectHandle acro, int& made) {
  QPDFObjectHandle field = w;
  std::string ft;
  QPDFObjectHandle v, da;
  for (int d = 0; field.isDictionary() && d < 16; ++d) {
    if (ft.empty() && field.getKey("/FT").isName()) ft = field.getKey("/FT").getName();
    if (v.isNull() && field.hasKey("/V")) v = field.getKey("/V");
    if (da.isNull() && field.hasKey("/DA")) da = field.getKey("/DA");
    field = field.getKey("/Parent");
  }
  if (da.isNull() && acro.isDictionary()) da = acro.getKey("/DA");
  QPDFObjectHandle rect = w.getKey("/Rect");
  if (!rect.isArray() || rect.getArrayNItems() != 4) return;
  double wdt = std::fabs(numOf(rect.getArrayItem(2), 0) - numOf(rect.getArrayItem(0), 0));
  double hgt = std::fabs(numOf(rect.getArrayItem(3), 0) - numOf(rect.getArrayItem(1), 0));
  if (wdt <= 0 || hgt <= 0) return;
  QPDFObjectHandle dr = acro.isDictionary() ? acro.getKey("/DR") : QPDFObjectHandle();
  auto mkStream = [&](const std::string& body, QPDFObjectHandle res) {
    QPDFObjectHandle st = QPDFObjectHandle::newStream(&ctx.pdf, body);
    QPDFObjectHandle d = st.getDict();
    d.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    d.replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
    d.replaceKey("/BBox", QPDFObjectHandle::parse("[0 0 " + fmtFixed(wdt, 3) + " " + fmtFixed(hgt, 3) + "]"));
    if (res.isDictionary()) d.replaceKey("/Resources", res);
    return ctx.pdf.makeIndirectObject(st);
  };
  std::string daStr = da.isString() ? da.getStringValue() : std::string("/Helv 0 Tf 0 g");
  QPDFObjectHandle ap = QPDFObjectHandle::newDictionary();
  if (ft == "/Btn") {
    QPDFObjectHandle mk = w.getKey("/AP");
    std::string on = "/Yes";
    QPDFObjectHandle ns = mk.isDictionary() ? mk.getKey("/N") : QPDFObjectHandle();
    if (ns.isDictionary()) {
      for (const std::string& k : ns.getKeys()) {
        if (k != "/Off") on = k;
      }
    }
    double sz = std::min(wdt, hgt) * 0.8;
    QPDFObjectHandle res = QPDFObjectHandle::parse("<< /Font << /ZaDb << /Type /Font /Subtype /Type1 /BaseFont /ZapfDingbats >> >> >>");
    QPDFObjectHandle nd = QPDFObjectHandle::newDictionary();
    nd.replaceKey(on, mkStream("q 0 g BT /ZaDb " + fmtFixed(sz, 2) + " Tf " + fmtFixed((wdt - sz * 0.75) / 2, 2) + " " +
                                   fmtFixed((hgt - sz * 0.7) / 2, 2) + " Td (4) Tj ET Q",
                               res));
    nd.replaceKey("/Off", mkStream("", QPDFObjectHandle()));
    ap.replaceKey("/N", nd);
    if (!w.hasKey("/AS")) w.replaceKey("/AS", QPDFObjectHandle::newName(nameIs(v, on) ? on : "/Off"));
  } else if (ft == "/Tx" || ft == "/Ch") {
    std::string text = v.isString() ? v.getUTF8Value() : v.isArray() && v.getArrayNItems() && v.getArrayItem(0).isString() ? v.getArrayItem(0).getUTF8Value() : "";
    double size = 0;
    std::string fontName = "/Helv";
    {
      std::istringstream is(daStr);
      std::vector<std::string> toks;
      std::string t;
      while (is >> t) toks.push_back(t);
      for (size_t i = 2; i < toks.size(); ++i) {
        if (toks[i] == "Tf") {
          fontName = toks[i - 2];
          size = std::atof(toks[i - 1].c_str());
        }
      }
    }
    if (size <= 0) size = std::min(12.0, hgt * 0.7);
    std::string latin;
    for (size_t i = 0; i < text.size(); ++i) {
      unsigned char c = static_cast<unsigned char>(text[i]);
      if (c < 0x80) {
        if (c == '(' || c == ')' || c == '\\') latin += '\\';
        latin += static_cast<char>(c);
      } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
        unsigned cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
        latin += cp < 256 ? static_cast<char>(cp) : '?';
        ++i;
      } else {
        latin += '?';
      }
    }
    QPDFObjectHandle res = QPDFObjectHandle::newDictionary();
    QPDFObjectHandle fonts = dr.isDictionary() && dr.getKey("/Font").isDictionary() ? dr.getKey("/Font") : QPDFObjectHandle();
    QPDFObjectHandle fobj = fonts.isDictionary() ? fonts.getKey(fontName) : QPDFObjectHandle();
    if (!fobj.isDictionary()) {
      fontName = "/Helv";
      fobj = QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
    }
    QPDFObjectHandle fd = QPDFObjectHandle::newDictionary();
    fd.replaceKey(fontName, fobj);
    res.replaceKey("/Font", fd);
    std::string colour = "0 g";
    size_t tfPos = daStr.find("Tf");
    if (tfPos != std::string::npos) colour = daStr.substr(tfPos + 2);
    std::string body = "/Tx BMC q BT " + fontName + " " + fmtFixed(size, 2) + " Tf " + colour + " 2 " +
                       fmtFixed(std::max(1.0, (hgt - size * 0.7) / 2), 2) + " Td (" + latin + ") Tj ET Q EMC";
    ap.replaceKey("/N", mkStream(body, res));
  } else {
    return;
  }
  w.replaceKey("/AP", ap);
  ++made;
}
}

namespace {
bool anchorBox(QPDFPageObjectHelper& ph, const std::string& boxName, double out[4]) {
  QPDFObjectHandle b = boxOnPage(ph, boxName.empty() ? "CropBox" : boxName);
  if (!b.isArray() || b.getArrayNItems() != 4) return false;
  for (int i = 0; i < 4; ++i) out[i] = numOf(b.getArrayItem(i), 0);
  return true;
}

void anchoredOrigin(const double b[4], const std::string& anchor, double x, double y, double w, double h,
                    double& ox, double& oy) {
  std::string a = lowerAscii(anchor);
  bool top = a.find("upper") != std::string::npos || a.find("top") != std::string::npos;
  bool bottom = a.find("lower") != std::string::npos || a.find("bottom") != std::string::npos;
  bool right = a.find("right") != std::string::npos, left = a.find("left") != std::string::npos;
  ox = (right ? b[2] - w : left ? b[0] : (b[0] + b[2] - w) / 2) + x;
  oy = (top ? b[3] - h : bottom ? b[1] : (b[1] + b[3] - h) / 2) + y;
  if (a.empty()) {
    ox = b[0] + x;
    oy = b[1] + y;
  }
}

int placeFormOnPages(Ctx& ctx, std::vector<QPDFPageObjectHelper>& pages, QPDFObjectHandle form, double w, double h,
                     double x, double y, const std::string& anchor, const std::string& box, double scale,
                     double rot, bool behind, const std::string& resName, QPDFObjectHandle ocg = QPDFObjectHandle()) {
  int n = 0;
  for (auto& ph : pages) {
    double b[4];
    if (!anchorBox(ph, box, b)) continue;
    double sw = w * scale, sh = h * scale;
    bool quarter = std::fmod(std::fabs(rot), 180.0) == 90.0;
    double ox, oy;
    anchoredOrigin(b, anchor, x, y, quarter ? sh : sw, quarter ? sw : sh, ox, oy);
    QPDFObjectHandle res = ph.getAttribute("/Resources", true);
    QPDFObjectHandle xo = res.getKey("/XObject");
    if (!xo.isDictionary()) {
      xo = QPDFObjectHandle::newDictionary();
      res.replaceKey("/XObject", xo);
    }
    xo.replaceKey(resName, form);
    double rad = rot * 3.14159265358979 / 180.0, c = std::cos(rad) * scale, sn = std::sin(rad) * scale;
    double tx = ox, ty = oy;
    int q = static_cast<int>(std::lround(std::fmod(rot + 360.0, 360.0) / 90.0)) % 4;
    if (q == 1) tx += sh;
    if (q == 2) {
      tx += sw;
      ty += sh;
    }
    if (q == 3) ty += sw;
    std::string cm = "\nq " + fmtFixed(c, 5) + " " + fmtFixed(sn, 5) + " " + fmtFixed(-sn, 5) + " " + fmtFixed(c, 5) + " " +
                     fmtFixed(tx, 3) + " " + fmtFixed(ty, 3) + " cm " + resName + " Do Q\n";
    if (ocg.isInitialized()) {
      QPDFObjectHandle props = res.getKey("/Properties");
      if (!props.isDictionary()) {
        props = QPDFObjectHandle::newDictionary();
        res.replaceKey("/Properties", props);
      }
      props.replaceKey(resName + "OC", ocg);
      cm = "\n/OC " + resName + "OC BDC" + cm + "EMC\n";
    }
    if (behind) {
      ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, cm), true);
    } else {
      ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, std::string("q\n")), true);
      ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, "\nQ" + cm), false);
    }
    ++n;
  }
  return n;
}

std::string fillOp(const ColorValue& c, bool stroke) {
  std::string v;
  for (double x : c.values) v += fmtFixed(x, 4) + " ";
  if (c.family == "/DeviceCMYK") return v + (stroke ? "K" : "k");
  if (c.family == "/DeviceRGB") return v + (stroke ? "RG" : "rg");
  return v + (stroke ? "G" : "g");
}
}

namespace {
struct M6 {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

M6 mul6(const M6& m, const M6& n) {
  return {m.a * n.a + m.b * n.c, m.a * n.b + m.b * n.d, m.c * n.a + m.d * n.c,
          m.c * n.b + m.d * n.d, m.e * n.a + m.f * n.c + n.e, m.e * n.b + m.f * n.d + n.f};
}

bool inv6(const M6& m, M6& out) {
  double det = m.a * m.d - m.b * m.c;
  if (std::fabs(det) < 1e-12) return false;
  out.a = m.d / det;
  out.b = -m.b / det;
  out.c = -m.c / det;
  out.d = m.a / det;
  out.e = (m.c * m.f - m.d * m.e) / det;
  out.f = (m.b * m.e - m.a * m.f) / det;
  return true;
}

std::string cm6(const M6& m) {
  return fmtFixed(m.a, 6) + " " + fmtFixed(m.b, 6) + " " + fmtFixed(m.c, 6) + " " + fmtFixed(m.d, 6) + " " +
         fmtFixed(m.e, 4) + " " + fmtFixed(m.f, 4) + " cm";
}

std::string tokensText(const std::vector<QPDFTokenizer::Token>& v) {
  std::string out;
  for (const auto& t : v) {
    out += t.getRawValue();
    if (t.getType() != QPDFTokenizer::tt_space) out += ' ';
  }
  return out;
}

std::string toStrokeOps(std::string s) {
  static const std::pair<const char*, const char*> m[] = {{" scn", " SCN"}, {" sc", " SC"}, {" cs", " CS"},
                                                          {" rg", " RG"}, {" g", " G"}, {" k", " K"}};
  std::string out;
  std::istringstream is(s);
  std::string tok;
  while (is >> tok) {
    if (tok == "scn") tok = "SCN";
    else if (tok == "sc") tok = "SC";
    else if (tok == "cs") tok = "CS";
    else if (tok == "rg") tok = "RG";
    else if (tok == "g") tok = "G";
    else if (tok == "k") tok = "K";
    out += tok + " ";
  }
  (void)m;
  return out;
}

class StateTrackFilter : public QPDFObjectHandle::TokenFilter {
 public:
  enum Mode { kBringTextFront, kFillToStroke, kSmoothLines, kDashRepair };
  StateTrackFilter(Mode mode, double tol, bool text = true, bool vector = false)
      : mode(mode), tol(tol), moveText(text), moveVector(vector) {}

  void handleToken(QPDFTokenizer::Token const& tok) override {
    auto t = tok.getType();
    if (t == QPDFTokenizer::tt_inline_image) {
      flushPath();
      emit(tokensText(operands) + tok.getRawValue() + " ");
      operands.clear();
      return;
    }
    if (t != QPDFTokenizer::tt_word) {
      if (t != QPDFTokenizer::tt_space && t != QPDFTokenizer::tt_comment) operands.push_back(tok);
      return;
    }
    std::string op = tok.getValue();
    std::string raw = tokensText(operands) + op;
    static const std::set<std::string> pathOps = {"m", "l", "c", "v", "y", "h", "re"};
    if (pathOps.count(op)) {
      std::vector<double> v = nums();
      path.push_back({op, v, raw});
      operands.clear();
      return;
    }
    static const std::set<std::string> paintOps = {"S", "s", "f", "F", "f*", "B", "B*", "b", "b*", "n", "W", "W*"};
    if (paintOps.count(op)) {
      if (op != "W" && op != "W*") finishPath(op);
      else pendingClip += raw + " ";
      operands.clear();
      return;
    }
    flushPath();
    if (op == "q") {
      st.push_back(cur);
    } else if (op == "Q") {
      if (!st.empty()) {
        cur = st.back();
        st.pop_back();
      }
    } else if (op == "cm") {
      std::vector<double> v = nums();
      if (v.size() == 6) cur.ctm = mul6({v[0], v[1], v[2], v[3], v[4], v[5]}, cur.ctm);
    } else if (op == "g" || op == "rg" || op == "k" || op == "sc" || op == "scn") {
      cur.fill = (op == "sc" || op == "scn") ? cur.fillCs + raw : raw;
    } else if (op == "cs") {
      cur.fillCs = raw + " ";
      cur.fill = raw;
    } else if (op == "G" || op == "RG" || op == "K" || op == "SC" || op == "SCN") {
      cur.stroke = (op == "SC" || op == "SCN") ? cur.strokeCs + raw : raw;
    } else if (op == "CS") {
      cur.strokeCs = raw + " ";
      cur.stroke = raw;
    } else if (op == "gs") {
      cur.gs += raw + " ";
    } else if (op == "Tf" || op == "Tc" || op == "Tw" || op == "Tz" || op == "TL" || op == "Ts" || op == "Tr") {
      cur.text[op] = raw;
    } else if (op == "d" && mode == kDashRepair && badDash()) {
      emit("[] 0 d ");
      cur.line[op] = "[] 0 d";
      ++changed;
      operands.clear();
      return;
    } else if (op == "w" || op == "J" || op == "j" || op == "M" || op == "d" || op == "ri") {
      cur.line[op] = raw;
    } else if (op == "BDC" || op == "BMC") {
      ++marks;
    } else if (op == "EMC") {
      if (marks > 0) --marks;
    }
    if (mode == kBringTextFront) {
      if (op == "BT" && marks == 0 && !inText && moveText) {
        inText = true;
        snapshot = cur;
        textBlock = "BT ";
        operands.clear();
        return;
      }
      if (inText) {
        textBlock += raw + " ";
        if (op == "ET") {
          inText = false;
          std::string pre = "q " + cm6(snapshot.ctm) + " " + snapshot.gs + snapshot.fill + " " + snapshot.stroke + " ";
          std::string ts;
          for (auto& kv : snapshot.line) pre += kv.second + " ";
          for (auto& kv : snapshot.text) ts += kv.second + " ";
          front += pre + textBlock.substr(0, 3) + ts + textBlock.substr(3) + " Q\n";
          ++changed;
        }
        operands.clear();
        return;
      }
    }
    emit(raw + " ");
    operands.clear();
  }

  void handleEOF() override {
    flushPath();
    if (!operands.empty()) emit(tokensText(operands));
    if (!front.empty()) write("\n" + front);
  }

  int changed = 0;

 private:
  struct Seg {
    std::string op;
    std::vector<double> v;
    std::string raw;
  };
  struct State {
    M6 ctm;
    std::string fill = "0 g", stroke = "0 G", fillCs, strokeCs, gs;
    std::map<std::string, std::string> text;
    std::map<std::string, std::string> line;
  };

  void emit(const std::string& s) {
    if (inText) textBlock += s;
    else write(s);
  }

  std::vector<double> nums() const {
    std::vector<double> v;
    for (const auto& t : operands) {
      if (t.getType() == QPDFTokenizer::tt_integer || t.getType() == QPDFTokenizer::tt_real) {
        v.push_back(std::strtod(t.getValue().c_str(), nullptr));
      }
    }
    return v;
  }

  bool badDash() const {
    std::vector<double> v = nums();
    double total = 0;
    for (size_t i = 0; i + 1 < v.size(); ++i) total += v[i];
    for (double x : v)
      if (x < 0) return true;
    return v.size() > 1 && total <= 0;
  }

  void flushPath() {
    for (auto& sgm : path) emit(sgm.raw + " ");
    path.clear();
    if (!pendingClip.empty()) emit(pendingClip);
    pendingClip.clear();
  }

  void finishPath(const std::string& paint) {
    bool fills = paint == "f" || paint == "F" || paint == "f*";
    if (mode == kFillToStroke && fills && pendingClip.empty() && path.size() == 1 && path[0].op == "re" &&
        path[0].v.size() == 4) {
      double x = path[0].v[0], y = path[0].v[1], w = path[0].v[2], h = path[0].v[3];
      double thin = std::min(std::fabs(w), std::fabs(h));
      if (thin > 0 && thin <= tol) {
        std::string line;
        if (std::fabs(w) >= std::fabs(h)) {
          double cy = y + h / 2;
          line = fmtFixed(x, 4) + " " + fmtFixed(cy, 4) + " m " + fmtFixed(x + w, 4) + " " + fmtFixed(cy, 4) + " l";
        } else {
          double cx = x + w / 2;
          line = fmtFixed(cx, 4) + " " + fmtFixed(y, 4) + " m " + fmtFixed(cx, 4) + " " + fmtFixed(y + h, 4) + " l";
        }
        emit("q " + toStrokeOps(cur.fill) + fmtFixed(thin, 4) + " w 0 J " + line + " S Q ");
        path.clear();
        ++changed;
        return;
      }
    }
    if (mode == kSmoothLines && paint != "n") smooth();
    if (mode == kBringTextFront && moveVector && marks == 0 && !inText && pendingClip.empty() && paint != "n") {
      std::string ls;
      for (auto& kv : cur.line) ls += kv.second + " ";
      std::string body;
      for (const Seg& sg : path) body += sg.raw + " ";
      front += "q " + cm6(cur.ctm) + " " + cur.gs + cur.fill + " " + cur.stroke + " " + ls + body + paint + " Q\n";
      path.clear();
      ++changed;
      return;
    }
    flushPath();
    emit(paint + " ");
  }

  void smooth() {
    std::vector<Seg> out;
    size_t i = 0;
    while (i < path.size()) {
      if (path[i].op != "m" && path[i].op != "l") {
        out.push_back(path[i++]);
        continue;
      }
      size_t j = i;
      std::vector<std::pair<double, double>> pts;
      while (j < path.size() && (j == i || path[j].op == "l") && path[j].v.size() == 2) {
        if (j > i && path[j].op == "m") break;
        pts.push_back({path[j].v[0], path[j].v[1]});
        ++j;
      }
      bool smoothRun = pts.size() >= 5;
      for (size_t k = 1; smoothRun && k + 1 < pts.size(); ++k) {
        double ax = pts[k].first - pts[k - 1].first, ay = pts[k].second - pts[k - 1].second;
        double bx = pts[k + 1].first - pts[k].first, by = pts[k + 1].second - pts[k].second;
        double la = std::hypot(ax, ay), lb = std::hypot(bx, by);
        if (la < 1e-9 || lb < 1e-9) continue;
        double cosang = (ax * bx + ay * by) / (la * lb);
        if (cosang < std::cos(25.0 * 3.14159265 / 180.0)) smoothRun = false;
      }
      if (!smoothRun || j == i) {
        for (size_t k = i; k < std::max(j, i + 1); ++k) out.push_back(path[k]);
        i = std::max(j, i + 1);
        continue;
      }
      out.push_back(path[i]);
      for (size_t k = 0; k + 1 < pts.size(); ++k) {
        auto p0 = pts[k == 0 ? 0 : k - 1], p1 = pts[k], p2 = pts[k + 1], p3 = pts[std::min(k + 2, pts.size() - 1)];
        double c1x = p1.first + (p2.first - p0.first) / 6, c1y = p1.second + (p2.second - p0.second) / 6;
        double c2x = p2.first - (p3.first - p1.first) / 6, c2y = p2.second - (p3.second - p1.second) / 6;
        Seg c;
        c.op = "c";
        c.raw = fmtFixed(c1x, 4) + " " + fmtFixed(c1y, 4) + " " + fmtFixed(c2x, 4) + " " + fmtFixed(c2y, 4) + " " +
                fmtFixed(p2.first, 4) + " " + fmtFixed(p2.second, 4) + " c";
        out.push_back(c);
      }
      ++changed;
      i = j;
    }
    path.swap(out);
  }

  Mode mode;
  double tol;
  bool moveText, moveVector;
  std::vector<QPDFTokenizer::Token> operands;
  std::vector<Seg> path;
  std::string pendingClip;
  State cur, snapshot;
  std::vector<State> st;
  int marks = 0;
  bool inText = false;
  std::string textBlock, front;
};

int rewritePages(Ctx& ctx, std::vector<QPDFPageObjectHelper>& pages, StateTrackFilter::Mode mode, double tol,
                 bool text = true, bool vector = false) {
  int total = 0;
  for (auto& ph : pages) {
    StateTrackFilter flt(mode, tol, text, vector);
    Pl_Buffer buf("state rewrite");
    try {
      ph.filterContents(&flt, &buf);
    } catch (...) {
      continue;
    }
    if (!flt.changed) continue;
    auto b = buf.getBufferSharedPointer();
    ph.getObjectHandle().replaceKey("/Contents", ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(
                                                     &ctx.pdf, std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize()))));
    total += flt.changed;
  }
  return total;
}

void relinkOutline(QPDFObjectHandle parent, std::set<QPDFObjGen>& seen, int depth, int& fixedN) {
  std::vector<QPDFObjectHandle> kids;
  QPDFObjectHandle k = parent.getKey("/First");
  while (k.isDictionary() && kids.size() < 100000) {
    if (k.isIndirect() && !seen.insert(k.getObjGen()).second) {
      ++fixedN;
      break;
    }
    kids.push_back(k);
    k = k.getKey("/Next");
  }
  int open = 0;
  for (size_t i = 0; i < kids.size(); ++i) {
    QPDFObjectHandle it = kids[i];
    if (!it.getKey("/Title").isString()) {
      it.replaceKey("/Title", QPDFObjectHandle::newUnicodeString("Untitled"));
      ++fixedN;
    }
    if (!it.getKey("/Parent").isIndirect() || it.getKey("/Parent").getObjGen() != parent.getObjGen()) {
      if (parent.isIndirect()) {
        it.replaceKey("/Parent", parent);
        ++fixedN;
      }
    }
    if (i) it.replaceKey("/Prev", kids[i - 1]);
    else if (it.hasKey("/Prev")) it.removeKey("/Prev");
    if (i + 1 < kids.size()) it.replaceKey("/Next", kids[i + 1]);
    else if (it.hasKey("/Next")) it.removeKey("/Next");
    if (depth < 64) relinkOutline(it, seen, depth + 1, fixedN);
    ++open;
  }
  if (kids.empty()) {
    for (const char* key : {"/First", "/Last", "/Count"}) {
      if (parent.hasKey(key) && !nameIs(parent.getKey("/Type"), "/Outlines")) parent.removeKey(key);
    }
    return;
  }
  parent.replaceKey("/First", kids.front());
  parent.replaceKey("/Last", kids.back());
  long long count = parent.getKey("/Count").isInteger() ? parent.getKey("/Count").getIntValue() : open;
  if (count == 0 || std::llabs(count) != open) {
    parent.replaceKey("/Count", QPDFObjectHandle::newInteger(count < 0 ? -open : open));
  }
}
}

namespace {
struct Placement {
  size_t op;
  std::string name;
  M6 ctm;
};

class PlacementScan : public QPDFObjectHandle::TokenFilter {
 public:
  void handleToken(QPDFTokenizer::Token const& tok) override {
    auto t = tok.getType();
    if (t != QPDFTokenizer::tt_word) {
      if (t != QPDFTokenizer::tt_space && t != QPDFTokenizer::tt_comment && t != QPDFTokenizer::tt_inline_image) operands.push_back(tok);
      return;
    }
    ++index;
    std::string op = tok.getValue();
    if (op == "q") st.push_back(ctm);
    else if (op == "Q" && !st.empty()) { ctm = st.back(); st.pop_back(); }
    else if (op == "cm" && operands.size() >= 6) {
      double v[6];
      for (int i = 0; i < 6; ++i) v[i] = std::strtod(operands[operands.size() - 6 + i].getValue().c_str(), nullptr);
      ctm = mul6({v[0], v[1], v[2], v[3], v[4], v[5]}, ctm);
    } else if (op == "Do" && !operands.empty() && operands.back().getType() == QPDFTokenizer::tt_name) {
      out.push_back({index, operands.back().getValue(), ctm});
    }
    operands.clear();
  }
  std::vector<Placement> out;

 private:
  std::vector<QPDFTokenizer::Token> operands;
  M6 ctm;
  std::vector<M6> st;
  size_t index = 0;
};

class ReplaceOpsFilter : public QPDFObjectHandle::TokenFilter {
 public:
  explicit ReplaceOpsFilter(const std::map<size_t, std::string>& rep) : rep(rep) {}
  void handleToken(QPDFTokenizer::Token const& tok) override {
    if (tok.getType() != QPDFTokenizer::tt_word) {
      pending.push_back(tok);
      return;
    }
    ++index;
    auto it = rep.find(index);
    if (it != rep.end()) {
      pending.clear();
      write(" " + it->second + " ");
      return;
    }
    for (auto& t : pending) writeToken(t);
    pending.clear();
    writeToken(tok);
  }
  void handleEOF() override {
    for (auto& t : pending) writeToken(t);
  }

 private:
  const std::map<size_t, std::string>& rep;
  std::vector<QPDFTokenizer::Token> pending;
  size_t index = 0;
};

int mergeImageStrips(Ctx& ctx, std::vector<QPDFPageObjectHelper>& pages) {
  int merged = 0;
  int serial = 0;
  for (auto& ph : pages) {
    PlacementScan scan;
    Pl_Discard discard;
    try {
      ph.filterContents(&scan, &discard);
    } catch (...) {
      continue;
    }
    QPDFObjectHandle res = ph.getAttribute("/Resources", true);
    QPDFObjectHandle xo = res.getKey("/XObject");
    if (!xo.isDictionary()) continue;
    struct Strip {
      Placement p;
      QPDFObjectHandle img;
      int w, h;
    };
    std::vector<Strip> strips;
    for (const Placement& p : scan.out) {
      QPDFObjectHandle img = xo.getKey(p.name);
      if (!img.isStream() || !nameIs(img.getDict().getKey("/Subtype"), "/Image")) continue;
      QPDFObjectHandle d = img.getDict();
      if (d.hasKey("/SMask") || d.hasKey("/Mask") || (d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue())) continue;
      if (std::fabs(p.ctm.b) > 1e-6 || std::fabs(p.ctm.c) > 1e-6 || p.ctm.a <= 0 || p.ctm.d <= 0) continue;
      strips.push_back({p, img, static_cast<int>(numOf(d.getKey("/Width"), 0)), static_cast<int>(numOf(d.getKey("/Height"), 0))});
    }
    std::map<size_t, std::string> rep;
    size_t i = 0;
    while (i < strips.size()) {
      size_t j = i + 1;
      while (j < strips.size()) {
        const Strip& a = strips[j - 1];
        const Strip& b = strips[j];
        bool same = b.w == a.w && std::fabs(b.p.ctm.a - a.p.ctm.a) < 0.01 && std::fabs(b.p.ctm.e - a.p.ctm.e) < 0.01 &&
                    std::fabs(b.p.ctm.d / std::max(1, b.h) - a.p.ctm.d / std::max(1, a.h)) < 1e-4 &&
                    b.img.getDict().getKey("/ColorSpace").unparse() == a.img.getDict().getKey("/ColorSpace").unparse() &&
                    std::fabs((b.p.ctm.f + b.p.ctm.d) - a.p.ctm.f) < 0.05;
        if (!same) break;
        ++j;
      }
      if (j - i >= 2) {
        std::string rows;
        int width = strips[i].w, height = 0, comps = 0;
        bool ok = true;
        for (size_t k = i; k < j && ok; ++k) {
          RawImage r = decodeImage(strips[k].img);
          if (!r.ok || r.width != width || (comps && r.comps != comps)) ok = false;
          comps = r.comps;
          rows += r.samples;
          height += r.height;
        }
        if (ok && comps >= 1 && comps <= 4) {
          QPDFObjectHandle cs = strips[i].img.getDict().getKey("/ColorSpace");
          bool indexed = cs.isArray() && cs.getArrayNItems() && (nameIs(cs.getArrayItem(0), "/Indexed") || nameIs(cs.getArrayItem(0), "/I"));
          QPDFObjectHandle ni = QPDFObjectHandle::newStream(&ctx.pdf, flateCompress(rows));
          QPDFObjectHandle nd = ni.getDict();
          nd.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
          nd.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
          nd.replaceKey("/Width", QPDFObjectHandle::newInteger(width));
          nd.replaceKey("/Height", QPDFObjectHandle::newInteger(height));
          nd.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
          nd.replaceKey("/Filter", QPDFObjectHandle::newName("/FlateDecode"));
          nd.replaceKey("/ColorSpace", indexed || cs.isNull() ? QPDFObjectHandle::newName(comps == 4 ? "/DeviceCMYK" : comps == 3 ? "/DeviceRGB" : "/DeviceGray") : cs);
          std::string name = "/KuraMerged" + std::to_string(++serial);
          xo.replaceKey(name, ctx.pdf.makeIndirectObject(ni));
          M6 top = strips[i].p.ctm, bottom = strips[j - 1].p.ctm;
          M6 full{top.a, 0, 0, top.f + top.d - bottom.f, top.e, bottom.f};
          M6 inv;
          if (inv6(top, inv)) {
            rep[strips[i].p.op] = cm6(mul6(full, inv)) + " " + name + " Do";
            for (size_t k = i + 1; k < j; ++k) rep[strips[k].p.op] = "";
            merged += static_cast<int>(j - i);
          }
        }
      }
      i = j;
    }
    if (rep.empty()) continue;
    ReplaceOpsFilter rf(rep);
    Pl_Buffer buf("merge strips");
    try {
      ph.filterContents(&rf, &buf);
      auto b = buf.getBufferSharedPointer();
      ph.getObjectHandle().replaceKey("/Contents", ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(
                                                       &ctx.pdf, std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize()))));
    } catch (...) {
    }
  }
  return merged;
}
}

namespace {
std::vector<std::pair<double, double>> toneCurve(const std::string& spec) {
  std::vector<std::pair<double, double>> pts;
  std::vector<double> nums;
  std::string cur;
  for (size_t i = 0; i <= spec.size(); ++i) {
    char c = i < spec.size() ? spec[i] : ' ';
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || (c == '-' && cur.empty())) {
      cur += c;
    } else {
      if (!cur.empty() && cur != "-") nums.push_back(std::atof(cur.c_str()));
      cur.clear();
    }
  }
  for (size_t i = 0; i + 1 < nums.size(); i += 2) pts.push_back({nums[i], nums[i + 1]});
  std::sort(pts.begin(), pts.end());
  if (!pts.empty() && pts.front().first > 0) pts.insert(pts.begin(), {0, 0});
  if (!pts.empty() && pts.back().first < 100) pts.push_back({100, 100});
  return pts;
}

double applyTone(const std::vector<std::pair<double, double>>& c, double coverage) {
  double x = coverage * 100;
  if (c.size() < 2) return coverage;
  if (x <= c.front().first) return c.front().second / 100;
  for (size_t i = 1; i < c.size(); ++i) {
    if (x <= c[i].first) {
      double x0 = c[i - 1].first, y0 = c[i - 1].second, x1 = c[i].first, y1 = c[i].second;
      double y = x1 == x0 ? y1 : y0 + (x - x0) * (y1 - y0) / (x1 - x0);
      return std::min(1.0, std::max(0.0, y / 100));
    }
  }
  return c.back().second / 100;
}
}

namespace {
bool reencodeImage(QPDFObjectHandle img, const std::string& px, int w, int h, int comps, bool lossy, int quality = 90);
std::string lastFilter(QPDFObjectHandle d);
}

namespace {
struct LibColor {
  std::string name;
  double lab[3];
};

std::vector<LibColor> loadColorLibrary(const std::string& path) {
  std::vector<LibColor> out;
  std::ifstream in(path, std::ios::binary);
  if (!in) return out;
  std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (t.find("<") != std::string::npos && (t.find("CxF") != std::string::npos || t.find("ColorCIELab") != std::string::npos)) {
    static const std::regex obj(R"re(<(?:\w+:)?Object\b[^>]*\bName="([^"]+)"[\s\S]*?</(?:\w+:)?Object>)re");
    static const std::regex lab(R"re(<(?:\w+:)?L>\s*([-0-9.]+)\s*</(?:\w+:)?L>\s*<(?:\w+:)?A>\s*([-0-9.]+)\s*</(?:\w+:)?A>\s*<(?:\w+:)?B>\s*([-0-9.]+)\s*</(?:\w+:)?B>)re");
    for (std::sregex_iterator it(t.begin(), t.end(), obj), end; it != end; ++it) {
      std::string body = it->str();
      std::smatch m;
      if (std::regex_search(body, m, lab)) {
        out.push_back({(*it)[1].str(), {std::atof(m[1].str().c_str()), std::atof(m[2].str().c_str()), std::atof(m[3].str().c_str())}});
      }
    }
    return out;
  }
  std::istringstream is(t);
  std::string line;
  while (std::getline(is, line)) {
    for (char& c : line) {
      if (c == ';' || c == '\t') c = ',';
    }
    std::vector<std::string> f;
    std::string cur;
    for (char c : line) {
      if (c == ',') {
        f.push_back(cur);
        cur.clear();
      } else if (c != '\r') {
        cur += c;
      }
    }
    f.push_back(cur);
    if (f.size() < 4) continue;
    char* e = nullptr;
    double L = std::strtod(f[f.size() - 3].c_str(), &e);
    if (!e || *e) continue;
    std::string name;
    for (size_t i = 0; i + 3 < f.size(); ++i) name += (i ? "," : "") + f[i];
    out.push_back({trimmed(name), {L, std::atof(f[f.size() - 2].c_str()), std::atof(f[f.size() - 1].c_str())}});
  }
  return out;
}

std::string xmlEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}
}


namespace {
std::string pdfNow(Ctx& ctx) {
  if (!ctx.opt.nowOverride.empty()) return ctx.opt.nowOverride;
  std::time_t t = std::time(nullptr);
  std::tm g{};
#ifdef _WIN32
  gmtime_s(&g, &t);
#else
  gmtime_r(&t, &g);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "D:%04d%02d%02d%02d%02d%02dZ", g.tm_year + 1900, g.tm_mon + 1,
                g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
  return buf;
}

bool regexLikeMatch(const std::string& pattern, const std::string& value) {
  if (pattern.empty() || pattern == ".*" || pattern == "*") return true;
  try {
    return std::regex_search(value, std::regex(pattern, std::regex::icase));
  } catch (...) {
    return value == pattern;
  }
}

std::vector<QPDFObjectHandle> matchingLayers(Ctx& ctx, const std::string& pattern) {
  std::vector<QPDFObjectHandle> out;
  QPDFObjectHandle ocp = ctx.pdf.getRoot().getKey("/OCProperties");
  QPDFObjectHandle ocgs = ocp.isDictionary() ? ocp.getKey("/OCGs") : QPDFObjectHandle();
  for (int i = 0; ocgs.isArray() && i < ocgs.getArrayNItems(); ++i) {
    QPDFObjectHandle g = ocgs.getArrayItem(i);
    if (!g.isDictionary()) continue;
    std::string name = g.getKey("/Name").isString() ? g.getKey("/Name").getUTF8Value() : std::string();
    if (regexLikeMatch(pattern, name)) out.push_back(g);
  }
  return out;
}

void setMembership(QPDFObjectHandle d, const std::string& key, QPDFObjectHandle g, bool present) {
  QPDFObjectHandle arr = d.getKey(key);
  if (!arr.isArray()) {
    if (!present) return;
    arr = QPDFObjectHandle::newArray();
    d.replaceKey(key, arr);
  }
  for (int i = arr.getArrayNItems() - 1; i >= 0; --i) {
    QPDFObjectHandle it = arr.getArrayItem(i);
    if (it.isIndirect() && g.isIndirect() && it.getObjGen() == g.getObjGen()) arr.eraseItem(i);
  }
  if (present) arr.appendItem(g);
}

QPDFObjectHandle usageDict(QPDFObjectHandle g, const std::string& category) {
  QPDFObjectHandle u = g.getKey("/Usage");
  if (!u.isDictionary()) {
    u = QPDFObjectHandle::newDictionary();
    g.replaceKey("/Usage", u);
  }
  QPDFObjectHandle c = u.getKey(category);
  if (!c.isDictionary()) {
    c = QPDFObjectHandle::newDictionary();
    u.replaceKey(category, c);
  }
  return c;
}

void addAutoState(Ctx& ctx, const std::string& event, const std::string& category,
                  QPDFObjectHandle g) {
  QPDFObjectHandle ocp = ctx.pdf.getRoot().getKey("/OCProperties");
  QPDFObjectHandle d = ocp.getKey("/D");
  if (!d.isDictionary()) return;
  QPDFObjectHandle as = d.getKey("/AS");
  if (!as.isArray()) {
    as = QPDFObjectHandle::newArray();
    d.replaceKey("/AS", as);
  }
  for (int i = 0; i < as.getArrayNItems(); ++i) {
    QPDFObjectHandle e = as.getArrayItem(i);
    if (e.isDictionary() && nameIs(e.getKey("/Event"), event)) {
      QPDFObjectHandle ocgs = e.getKey("/OCGs");
      if (!ocgs.isArray()) {
        ocgs = QPDFObjectHandle::newArray();
        e.replaceKey("/OCGs", ocgs);
      }
      for (int j = 0; j < ocgs.getArrayNItems(); ++j) {
        if (ocgs.getArrayItem(j).isIndirect() && ocgs.getArrayItem(j).getObjGen() == g.getObjGen()) return;
      }
      ocgs.appendItem(g);
      return;
    }
  }
  QPDFObjectHandle e = QPDFObjectHandle::newDictionary();
  e.replaceKey("/Event", QPDFObjectHandle::newName(event));
  QPDFObjectHandle cat = QPDFObjectHandle::newArray();
  cat.appendItem(QPDFObjectHandle::newName(category));
  e.replaceKey("/Category", cat);
  QPDFObjectHandle ocgs = QPDFObjectHandle::newArray();
  ocgs.appendItem(g);
  e.replaceKey("/OCGs", ocgs);
  as.appendItem(e);
}

std::string fmtCm(double a, double b, double c, double d, double e, double f);

void transformPageContent(Ctx& ctx, QPDFPageObjectHelper& ph, double a, double d, double e, double f) {
  ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, "q " + fmtCm(a, 0, 0, d, e, f) + "\n"), true);
  ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, std::string("\nQ")), false);
  QPDFObjectHandle page = ph.getObjectHandle();
  QPDFObjectHandle res = ph.getAttribute("/Resources", false);
  QPDFObjectHandle pats = res.isDictionary() ? res.getKey("/Pattern") : QPDFObjectHandle();
  if (!pats.isInitialized() || !pats.isDictionary()) return;
  QPDFObjectHandle newPats = QPDFObjectHandle::newDictionary();
  for (const std::string& k : pats.getKeys()) {
    QPDFObjectHandle p = pats.getKey(k);
    QPDFObjectHandle copy;
    if (p.isStream()) copy = p.copyStream();
    else if (p.isDictionary()) copy = ctx.pdf.makeIndirectObject(p.shallowCopy());
    else continue;
    QPDFObjectHandle cd = copy.isStream() ? copy.getDict() : copy;
    QPDFObjectHandle m = cd.getKey("/Matrix");
    double v[6] = {1, 0, 0, 1, 0, 0};
    for (int i = 0; m.isArray() && i < 6 && i < m.getArrayNItems(); ++i) v[i] = numOf(m.getArrayItem(i), v[i]);
    double t[6] = {v[0] * a, v[1] * d, v[2] * a, v[3] * d, v[4] * a + e, v[5] * d + f};
    QPDFObjectHandle nm = QPDFObjectHandle::newArray();
    for (double x : t) nm.appendItem(QPDFObjectHandle::newReal(x, 6));
    cd.replaceKey("/Matrix", nm);
    newPats.replaceKey(k, copy);
  }
  QPDFObjectHandle newRes = res.shallowCopy();
  newRes.replaceKey("/Pattern", newPats);
  page.replaceKey("/Resources", newRes);
}

void shiftBox(QPDFObjectHandle page, const std::string& key, double dx, double dy) {
  QPDFObjectHandle b = page.getKey(key);
  if (!b.isArray() || b.getArrayNItems() != 4) return;
  QPDFObjectHandle n = QPDFObjectHandle::newArray();
  for (int i = 0; i < 4; ++i) n.appendItem(QPDFObjectHandle::newReal(numOf(b.getArrayItem(i), 0) + (i % 2 ? dy : dx), 4));
  page.replaceKey(key, n);
}

void scaleBox(QPDFObjectHandle page, const std::string& key, double a, double d, double e,
              double f) {
  QPDFObjectHandle b = page.getKey(key);
  if (!b.isArray() || b.getArrayNItems() != 4) return;
  double x1 = numOf(b.getArrayItem(0), 0) * a + e, y1 = numOf(b.getArrayItem(1), 0) * d + f;
  double x2 = numOf(b.getArrayItem(2), 0) * a + e, y2 = numOf(b.getArrayItem(3), 0) * d + f;
  QPDFObjectHandle n = QPDFObjectHandle::newArray();
  n.appendItem(QPDFObjectHandle::newReal(std::min(x1, x2), 4));
  n.appendItem(QPDFObjectHandle::newReal(std::min(y1, y2), 4));
  n.appendItem(QPDFObjectHandle::newReal(std::max(x1, x2), 4));
  n.appendItem(QPDFObjectHandle::newReal(std::max(y1, y2), 4));
  page.replaceKey(key, n);
}

void transformAnnots(QPDFObjectHandle page, double a, double d, double e, double f) {
  QPDFObjectHandle annots = page.getKey("/Annots");
  for (int i = 0; annots.isArray() && i < annots.getArrayNItems(); ++i) {
    QPDFObjectHandle an = annots.getArrayItem(i);
    if (an.isDictionary()) scaleBox(an, "/Rect", a, d, e, f);
  }
}

std::string fmtCm(double a, double b, double c, double d, double e, double f) {
  return fmtFixed(a, 6) + " " + fmtFixed(b, 6) + " " + fmtFixed(c, 6) + " " + fmtFixed(d, 6) +
         " " + fmtFixed(e, 4) + " " + fmtFixed(f, 4) + " cm";
}

void unsharp(std::string& px, int w, int h, int comps, int radius, double amount) {
  if (radius < 1) radius = 1;
  std::vector<float> blur(px.size());
  std::vector<float> tmp(px.size());
  auto at = [&](int x, int y, int c) { return static_cast<unsigned char>(px[(static_cast<size_t>(y) * w + x) * comps + c]); };
  for (int y = 0; y < h; ++y) {
    for (int c = 0; c < comps; ++c) {
      float acc = 0;
      int n = 0;
      for (int x = -radius; x <= radius; ++x) {
        if (x >= 0 && x < w) { acc += at(x, y, c); ++n; }
      }
      for (int x = 0; x < w; ++x) {
        tmp[(static_cast<size_t>(y) * w + x) * comps + c] = acc / std::max(1, n);
        int out = x - radius, in = x + radius + 1;
        if (out >= 0) { acc -= at(out, y, c); --n; }
        if (in < w) { acc += at(in, y, c); ++n; }
      }
    }
  }
  for (int x = 0; x < w; ++x) {
    for (int c = 0; c < comps; ++c) {
      float acc = 0;
      int n = 0;
      for (int y = -radius; y <= radius; ++y) {
        if (y >= 0 && y < h) { acc += tmp[(static_cast<size_t>(y) * w + x) * comps + c]; ++n; }
      }
      for (int y = 0; y < h; ++y) {
        blur[(static_cast<size_t>(y) * w + x) * comps + c] = acc / std::max(1, n);
        int out = y - radius, in = y + radius + 1;
        if (out >= 0) { acc -= tmp[(static_cast<size_t>(out) * w + x) * comps + c]; --n; }
        if (in < h) { acc += tmp[(static_cast<size_t>(in) * w + x) * comps + c]; ++n; }
      }
    }
  }
  for (size_t i = 0; i < px.size(); ++i) {
    float v = static_cast<unsigned char>(px[i]);
    float o = v + static_cast<float>(amount) * (v - blur[i]);
    px[i] = static_cast<char>(std::lround(std::min(255.0f, std::max(0.0f, o))));
  }
}

bool reencodeImage(QPDFObjectHandle img, const std::string& px, int w, int h, int comps,
                   bool lossy, int quality) {
  std::string enc;
  const char* filter = nullptr;
  if (lossy) {
    enc = comps == 4 ? encodeCmykJpeg(px, w, h, quality) : encodeJpeg(px, w, h, comps, quality);
    if (!enc.empty()) filter = "/DCTDecode";
  }
  if (!filter) {
    enc = flateCompress(px);
    if (enc.empty()) return false;
    filter = "/FlateDecode";
  }
  img.replaceStreamData(enc, QPDFObjectHandle::newName(filter), QPDFObjectHandle::newNull());
  QPDFObjectHandle d = img.getDict();
  d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
  d.removeKey("/Decode");
  d.removeKey("/DecodeParms");
  return true;
}

std::string lastFilter(QPDFObjectHandle d) {
  QPDFObjectHandle f = d.getKey("/Filter");
  if (f.isArray() && f.getArrayNItems() > 0) return nameOf(f.getArrayItem(f.getArrayNItems() - 1));
  return nameOf(f);
}
}



namespace {
QPDFObjectHandle findOrAddLayer(Ctx& ctx, const std::string& label, const std::string& procStep) {
  QPDFObjectHandle root = ctx.pdf.getRoot();
  QPDFObjectHandle ocp = root.getKey("/OCProperties");
  if (!ocp.isDictionary()) {
    ocp = QPDFObjectHandle::parse("<< /OCGs [] /D << /Order [] >> >>");
    root.replaceKey("/OCProperties", ocp);
  }
  if (!ocp.getKey("/OCGs").isArray()) ocp.replaceKey("/OCGs", QPDFObjectHandle::newArray());
  for (auto& g : ocp.getKey("/OCGs").aitems()) {
    if (g.isDictionary() && g.isIndirect() && g.getKey("/Name").isString() && g.getKey("/Name").getUTF8Value() == label &&
        g.getKey("/GTS_Metadata").isDictionary() == !procStep.empty()) {
      return g;
    }
  }
  QPDFObjectHandle ocg = QPDFObjectHandle::newDictionary();
  ocg.replaceKey("/Type", QPDFObjectHandle::newName("/OCG"));
  ocg.replaceKey("/Name", QPDFObjectHandle::newUnicodeString(label));
  if (!procStep.empty()) {
    QPDFObjectHandle md = QPDFObjectHandle::newDictionary();
    md.replaceKey("/Type", QPDFObjectHandle::newName("/GTS_ProcSteps"));
    md.replaceKey("/GTS_ProcStepsGroup", QPDFObjectHandle::newName("/" + procStep));
    ocg.replaceKey("/GTS_Metadata", md);
  }
  QPDFObjectHandle ref = ctx.pdf.makeIndirectObject(ocg);
  ocp.getKey("/OCGs").appendItem(ref);
  QPDFObjectHandle d = ocp.getKey("/D");
  if (!d.isDictionary()) {
    d = QPDFObjectHandle::newDictionary();
    ocp.replaceKey("/D", d);
  }
  if (!d.getKey("/Order").isArray()) d.replaceKey("/Order", QPDFObjectHandle::newArray());
  d.getKey("/Order").appendItem(ref);
  return ref;
}

std::string resolvePath(const FixupCtx& f, const std::string& path) {
  if (path.empty() || path[0] == '/' || path.find(":\\") == 1 || f.ctx.opt.profilePath.empty()) return path;
  size_t slash = f.ctx.opt.profilePath.find_last_of("/\\");
  if (slash == std::string::npos) return path;
  std::string near = f.ctx.opt.profilePath.substr(0, slash + 1) + path;
  std::ifstream probe(near, std::ios::binary);
  return probe ? near : path;
}

std::string readFileText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> ruleIdsFor(Ctx& ctx, const std::string& checkName) {
  PfProfile prof;
  if (checkName.empty() || !parseKuraJson(ctx.opt.preflightProfile, prof)) return {};
  auto it = prof.ruleIndex.find(checkName);
  if (it == prof.ruleIndex.end()) return {};
  return {it->second.id};
}

int placeStamp(const FixupCtx& f, const std::function<QPDFObjectHandle(int, double&, double&)>& make,
               const std::string& resName, double scale) {
  QPDFObjectHandle ocg = f.has("layer") ? findOrAddLayer(f.ctx, f.str("layer"), "") : QPDFObjectHandle();
  int n = 0;
  for (size_t slot = 0; slot < f.pages.size(); ++slot) {
    int pageNo = f.indexOf(slot) + 1;
    if (!pageWanted(f.str("pages"), pageNo, f.pageCount)) continue;
    double w = 0, h = 0;
    QPDFObjectHandle form = make(pageNo, w, h);
    if (!form.isInitialized()) continue;
    std::vector<QPDFPageObjectHelper> one{f.pages[slot]};
    n += placeFormOnPages(f.ctx, one, form, w, h, f.num("x"), f.num("y"), f.str("anchor"), boxKey(f.str("relative_to")),
                          scale, f.num("rotation"), f.str("position") == "back", resName, ocg);
  }
  return n;
}

QPDFObjectHandle formOf(Ctx& ctx, const std::string& body, double w, double h, QPDFObjectHandle res) {
  QPDFObjectHandle form = QPDFObjectHandle::newStream(&ctx.pdf, body);
  QPDFObjectHandle fd = form.getDict();
  fd.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
  fd.replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
  fd.replaceKey("/BBox", QPDFObjectHandle::parse("[0 0 " + fmtFixed(w, 3) + " " + fmtFixed(h, 3) + "]"));
  fd.replaceKey("/Resources", res.isInitialized() ? res : QPDFObjectHandle::newDictionary());
  return ctx.pdf.makeIndirectObject(form);
}

ColorValue stampColor(const FixupCtx& f) {
  ColorValue col;
  if (!parseColorText(f.str("color"), col)) {
    col.family = "/DeviceCMYK";
    col.values = {0, 0, 0, 1};
  }
  return col;
}

bool fixPageRotate(const FixupCtx& f) {
  int ang = static_cast<int>(std::lround(f.num("angle")));
  if (ang % 90 != 0 || ang % 360 == 0) return true;
  const std::string& only = f.str("only");
  int n = 0;
  for (auto& ph : f.pages) {
    QPDFObjectHandle page = ph.getObjectHandle();
    int cur = page.getKey("/Rotate").isInteger() ? static_cast<int>(page.getKey("/Rotate").getIntValue()) : 0;
    if (only == "portrait" || only == "landscape") {
      QPDFObjectHandle b = boxOnPage(ph, "CropBox");
      if (!b.isArray() || b.getArrayNItems() != 4) continue;
      double w = std::fabs(numOf(b.getArrayItem(2), 0) - numOf(b.getArrayItem(0), 0));
      double h = std::fabs(numOf(b.getArrayItem(3), 0) - numOf(b.getArrayItem(1), 0));
      if ((cur % 180 + 180) % 180 == 90) std::swap(w, h);
      if ((only == "portrait") != (h > w)) continue;
    }
    page.replaceKey("/Rotate", QPDFObjectHandle::newInteger(((cur + ang) % 360 + 360) % 360));
    ++n;
  }
  if (n) f.note("rotated " + std::to_string(n) + " page(s) by " + std::to_string(ang) + " degrees");
  return true;
}

bool fixPageRemoveUserUnit(const FixupCtx& f) {
  int n = 0;
  for (auto& ph : f.pages) {
    if (ph.getObjectHandle().hasKey("/UserUnit")) {
      ph.getObjectHandle().removeKey("/UserUnit");
      ++n;
    }
  }
  if (n) f.note("removed page scaling (/UserUnit) from " + std::to_string(n) + " page(s)");
  return true;
}

bool fixPageScale(const FixupCtx& f) {
  double tw = f.num("width"), th = f.num("height");
  if (tw <= 1 || th <= 1) return true;
  bool contentOnly = f.str("mode") == "content_only";
  bool onlyLarger = f.flag("only_if_larger");
  int n = 0;
  for (auto& ph : f.pages) {
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle mb = contentOnly ? boxOnPage(ph, "TrimBox") : ph.getAttribute("/MediaBox", true);
    if (!mb.isArray() || mb.getArrayNItems() != 4) continue;
    double x1 = numOf(mb.getArrayItem(0), 0), y1 = numOf(mb.getArrayItem(1), 0);
    double w = numOf(mb.getArrayItem(2), 0) - x1, h = numOf(mb.getArrayItem(3), 0) - y1;
    if (w < 1 || h < 1) continue;
    double sc = std::min(tw / w, th / h);
    if (std::abs(sc - 1.0) < 0.001 || (onlyLarger && sc > 1)) continue;
    if (contentOnly) {
      double ax = x1 + w / 2, ay = y1 + h / 2;
      transformPageContent(f.ctx, ph, sc, sc, ax - sc * ax, ay - sc * ay);
      transformAnnots(page, sc, sc, ax - sc * ax, ay - sc * ay);
      ++n;
      continue;
    }
    transformPageContent(f.ctx, ph, sc, sc, -sc * x1, -sc * y1);
    transformAnnots(page, sc, sc, -sc * x1, -sc * y1);
    page.replaceKey("/MediaBox", newBox(0, 0, w * sc, h * sc));
    for (const char* bx : {"/CropBox", "/TrimBox", "/BleedBox", "/ArtBox"}) {
      if (page.hasKey(bx)) page.removeKey(bx);
    }
    ++n;
  }
  if (n) f.note(std::string("scaled ") + (contentOnly ? "the content of " : "") + std::to_string(n) + " page(s)");
  return true;
}

bool fixPageSetBox(const FixupCtx& f) {
  std::string target = boxKey(f.str("box"));
  std::string from = f.str("from");
  bool onlyMissing = f.str("when") == "missing";
  double left = f.num("left"), bottom = f.num("bottom"), right = f.num("right"), top = f.num("top");
  int n = 0;
  for (size_t slot = 0; slot < f.pages.size(); ++slot) {
    QPDFPageObjectHelper& ph = f.pages[slot];
    QPDFObjectHandle page = ph.getObjectHandle();
    if (onlyMissing && page.hasKey("/" + target)) continue;
    double r[4];
    if (from == "content") {
      if (!renderedContentBox(f.ctx, ph, f.indexOf(slot), 72, r)) continue;
    } else {
      QPDFObjectHandle ref = boxOnPage(ph, from == "marks" ? "CropBox" : boxKey(from));
      if (!ref.isArray() || ref.getArrayNItems() != 4) continue;
      for (int i = 0; i < 4; ++i) r[i] = numOf(ref.getArrayItem(i), 0);
    }
    page.replaceKey("/" + target, newBox(r[0] + left, r[1] + bottom, r[2] - right, r[3] - top));
    ++n;
  }
  if (n) f.note("set the " + target + " on " + std::to_string(n) + " page(s)");
  return true;
}

bool fixBleedGenerate(const FixupCtx& f) {
  double a = f.num("amount");
  if (a <= 0) return true;
  double amt[4] = {a, a, a, a};
  bool mirror = f.str("method") != "stretch";
  int done = 0;
  for (size_t slot = 0; slot < f.pages.size(); ++slot) {
    QPDFPageObjectHelper& ph = f.pages[slot];
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle tb = boxOnPage(ph, "TrimBox");
    QPDFObjectHandle mb = ph.getAttribute("/MediaBox", true);
    if (!tb.isArray() || !mb.isArray() || tb.getArrayNItems() != 4) continue;
    double t[4], m[4];
    for (int i = 0; i < 4; ++i) {
      t[i] = numOf(tb.getArrayItem(i), 0);
      m[i] = numOf(mb.getArrayItem(i), 0);
    }
    double bx1 = t[0] - a, by1 = t[1] - a, bx2 = t[2] + a, by2 = t[3] + a;
    if (f.ctx.opt.rasterizePage && placeEdgeBleed(f.ctx, ph, f.indexOf(slot), t, amt, 300, mirror)) {
      if (bx1 < m[0] || by1 < m[1] || bx2 > m[2] || by2 > m[3]) {
        page.replaceKey("/MediaBox", newBox(std::min(m[0], bx1), std::min(m[1], by1), std::max(m[2], bx2), std::max(m[3], by2)));
        if (page.hasKey("/CropBox")) page.removeKey("/CropBox");
      }
      page.replaceKey("/BleedBox", newBox(bx1, by1, bx2, by2));
      if (!page.hasKey("/TrimBox")) page.replaceKey("/TrimBox", newBox(t[0], t[1], t[2], t[3]));
      ++done;
      continue;
    }
    page.replaceKey("/BleedBox", newBox(std::max(bx1, m[0]), std::max(by1, m[1]), std::min(bx2, m[2]), std::min(by2, m[3])));
  }
  if (done) {
    f.note(std::string("generated ") + (mirror ? "mirrored" : "stretched") + " image bleed on " + std::to_string(done) + " page(s)");
  } else {
    f.note("set the bleed box from the trim box");
  }
  return true;
}

bool fixPageCropTo(const FixupCtx& f) {
  int n = 0;
  for (size_t slot = 0; slot < f.pages.size(); ++slot) {
    double r[4];
    if (!renderedContentBox(f.ctx, f.pages[slot], f.indexOf(slot), 72, r)) continue;
    f.pages[slot].getObjectHandle().replaceKey("/CropBox", newBox(r[0], r[1], r[2], r[3]));
    ++n;
  }
  if (n) f.note("cropped " + std::to_string(n) + " page(s) to the visible content");
  return true;
}

bool fixPageAddMarks(const FixupCtx& f) {
  bool enlarge = f.flag("enlarge_media");
  double off = f.num("offset"), len = f.num("length"), room = 36;
  std::string lw = fmtFixed(f.num("line_width") > 0 ? f.num("line_width") : 0.25, 3);
  int n = 0;
  QPDFObjectHandle reg = QPDFObjectHandle::parse(
      "[/Separation /All /DeviceCMYK << /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [1 1 1 1] /N 1 >>]");
  reg = f.ctx.pdf.makeIndirectObject(reg);
  for (auto& ph : f.pages) {
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle tb = boxOnPage(ph, "TrimBox");
    QPDFObjectHandle bb = ph.getAttribute("/BleedBox", false);
    if (!tb.isArray() || tb.getArrayNItems() != 4) continue;
    double t[4], b[4];
    for (int i = 0; i < 4; ++i) t[i] = numOf(tb.getArrayItem(i), 0);
    for (int i = 0; i < 4; ++i) b[i] = bb.isArray() && bb.getArrayNItems() == 4 ? numOf(bb.getArrayItem(i), t[i]) : t[i];
    if (!page.hasKey("/TrimBox")) page.replaceKey("/TrimBox", tb.shallowCopy());
    QPDFObjectHandle mb = ph.getAttribute("/MediaBox", true);
    double m[4];
    for (int i = 0; i < 4; ++i) m[i] = numOf(mb.getArrayItem(i), 0);
    if (enlarge) {
      double need[4] = {b[0] - off - len - room / 2, b[1] - off - len - room / 2, b[2] + off + len + room / 2, b[3] + off + len + room / 2};
      QPDFObjectHandle nm = QPDFObjectHandle::newArray();
      nm.appendItem(QPDFObjectHandle::newReal(std::min(m[0], need[0]), 2));
      nm.appendItem(QPDFObjectHandle::newReal(std::min(m[1], need[1]), 2));
      nm.appendItem(QPDFObjectHandle::newReal(std::max(m[2], need[2]), 2));
      nm.appendItem(QPDFObjectHandle::newReal(std::max(m[3], need[3]), 2));
      page.replaceKey("/MediaBox", nm);
      if (page.hasKey("/CropBox")) page.removeKey("/CropBox");
    }
    std::string c = "q /KuraReg CS 1 SCN " + lw + " w\n";
    auto line = [&](double x1, double y1, double x2, double y2) {
      c += fmtFixed(x1, 2) + " " + fmtFixed(y1, 2) + " m " + fmtFixed(x2, 2) + " " + fmtFixed(y2, 2) + " l S\n";
    };
    double xs[2] = {t[0], t[2]}, ys[2] = {t[1], t[3]};
    for (int i = 0; i < 2; ++i) {
      double x = xs[i], dirx = i == 0 ? -1 : 1;
      double bxEdge = i == 0 ? b[0] : b[2];
      for (int j = 0; j < 2; ++j) {
        double y = ys[j], diry = j == 0 ? -1 : 1;
        double byEdge = j == 0 ? b[1] : b[3];
        line(bxEdge + dirx * off, y, bxEdge + dirx * (off + len), y);
        line(x, byEdge + diry * off, x, byEdge + diry * (off + len));
      }
    }
    auto target = [&](double cx, double cy) {
      double r = 4;
      c += fmtFixed(cx - r, 2) + " " + fmtFixed(cy, 2) + " m " + fmtFixed(cx + r, 2) + " " + fmtFixed(cy, 2) + " l S\n";
      c += fmtFixed(cx, 2) + " " + fmtFixed(cy - r, 2) + " m " + fmtFixed(cx, 2) + " " + fmtFixed(cy + r, 2) + " l S\n";
      double k = r * 0.5523 * 0.6, rr = r * 0.6;
      c += fmtFixed(cx + rr, 2) + " " + fmtFixed(cy, 2) + " m ";
      c += fmtFixed(cx + rr, 2) + " " + fmtFixed(cy + k, 2) + " " + fmtFixed(cx + k, 2) + " " + fmtFixed(cy + rr, 2) + " " + fmtFixed(cx, 2) + " " + fmtFixed(cy + rr, 2) + " c ";
      c += fmtFixed(cx - k, 2) + " " + fmtFixed(cy + rr, 2) + " " + fmtFixed(cx - rr, 2) + " " + fmtFixed(cy + k, 2) + " " + fmtFixed(cx - rr, 2) + " " + fmtFixed(cy, 2) + " c ";
      c += fmtFixed(cx - rr, 2) + " " + fmtFixed(cy - k, 2) + " " + fmtFixed(cx - k, 2) + " " + fmtFixed(cy - rr, 2) + " " + fmtFixed(cx, 2) + " " + fmtFixed(cy - rr, 2) + " c ";
      c += fmtFixed(cx + k, 2) + " " + fmtFixed(cy - rr, 2) + " " + fmtFixed(cx + rr, 2) + " " + fmtFixed(cy - k, 2) + " " + fmtFixed(cx + rr, 2) + " " + fmtFixed(cy, 2) + " c S\n";
    };
    double mx = (t[0] + t[2]) / 2, my = (t[1] + t[3]) / 2;
    target(mx, b[1] - off - len / 2);
    target(mx, b[3] + off + len / 2);
    target(b[0] - off - len / 2, my);
    target(b[2] + off + len / 2, my);
    c += "Q\n";
    QPDFObjectHandle res = ph.getAttribute("/Resources", true);
    QPDFObjectHandle csd = res.getKey("/ColorSpace");
    if (!csd.isDictionary()) {
      csd = QPDFObjectHandle::newDictionary();
      res.replaceKey("/ColorSpace", csd);
    }
    csd.replaceKey("/KuraReg", reg);
    ph.addPageContents(QPDFObjectHandle::newStream(&f.ctx.pdf, std::string("q\n")), true);
    ph.addPageContents(QPDFObjectHandle::newStream(&f.ctx.pdf, "\nQ\n" + c), false);
    ++n;
  }
  if (n) f.note("added crop and registration marks on " + std::to_string(n) + " page(s)");
  return true;
}

bool fixContentRemoveOutside(const FixupCtx& f) {
  std::string bx = boxKey(f.str("box"));
  for (auto& ph : f.pages) {
    QPDFObjectHandle b = boxOnPage(ph, bx);
    if (!b.isArray() || b.getArrayNItems() != 4) continue;
    double x0 = numOf(b.getArrayItem(0), 0), y0 = numOf(b.getArrayItem(1), 0);
    std::string clipOp = "q " + fmtFixed(x0, 4) + " " + fmtFixed(y0, 4) + " " +
                         fmtFixed(numOf(b.getArrayItem(2), 0) - x0, 4) + " " +
                         fmtFixed(numOf(b.getArrayItem(3), 0) - y0, 4) + " re W n\n";
    ph.addPageContents(QPDFObjectHandle::newStream(&f.ctx.pdf, clipOp), true);
    ph.addPageContents(QPDFObjectHandle::newStream(&f.ctx.pdf, std::string("\nQ")), false);
  }
  f.note("clipped page content to the " + bx);
  return true;
}

bool fixDocumentRemoveConformanceClaim(const FixupCtx& f) {
  static const std::map<std::string, std::string> prefixes = {
      {"pdfa", "pdfaid:"}, {"pdfx", "pdfxid:"}, {"pdfe", "pdfe:"}, {"pdfua", "pdfuaid:"}, {"pdfvt", "pdfvtid:"}};
  auto pf = prefixes.find(f.str("family"));
  if (pf == prefixes.end()) return true;
  const std::string& prefix = pf->second;
  if (f.str("family") == "pdfx") {
    QPDFObjectHandle info = f.ctx.pdf.getTrailer().getKey("/Info");
    for (const char* k : {"/GTS_PDFXVersion", "/GTS_PDFXConformance"}) {
      if (info.isDictionary() && info.hasKey(k)) info.removeKey(k);
    }
  }
  QPDFObjectHandle meta = f.ctx.pdf.getRoot().getKey("/Metadata");
  if (meta.isStream()) {
    std::string xmp;
    try {
      auto buf = meta.getStreamData(qpdf_dl_all);
      xmp.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    } catch (...) {
      f.ctx.scanIncomplete("the XMP metadata packet");
    }
    std::string before = xmp;
    size_t pos = 0;
    while ((pos = xmp.find("<" + prefix, pos)) != std::string::npos) {
      size_t tagEnd = xmp.find(':', pos + 1);
      size_t nameEnd = xmp.find_first_of(" >", tagEnd + 1);
      std::string tag = xmp.substr(pos + 1, nameEnd - pos - 1);
      size_t close = xmp.find("</" + tag + ">", pos);
      if (close != std::string::npos) {
        xmp.erase(pos, close + tag.size() + 3 - pos);
      } else {
        size_t gt = xmp.find('>', pos);
        if (gt == std::string::npos) break;
        xmp.erase(pos, gt + 1 - pos);
      }
    }
    pos = 0;
    while ((pos = xmp.find(prefix, pos)) != std::string::npos) {
      size_t eq = xmp.find('=', pos);
      size_t lineStart = xmp.rfind('\n', pos);
      if (eq != std::string::npos && eq < pos + 40 && eq + 1 < xmp.size() &&
          (xmp[eq + 1] == '"' || xmp[eq + 1] == '\'')) {
        char q = xmp[eq + 1];
        size_t end = xmp.find(q, eq + 2);
        if (end == std::string::npos) break;
        size_t start = pos;
        if (start >= 6 && xmp.compare(start - 6, 6, "xmlns:") == 0) start -= 6;
        while (start > 0 && (xmp[start - 1] == ' ' || xmp[start - 1] == '\n' ||
                             xmp[start - 1] == '\t')) {
          --start;
          if (lineStart != std::string::npos && start <= lineStart) break;
        }
        xmp.erase(start, end + 1 - start);
        pos = start;
      } else {
        pos += prefix.size();
      }
    }
    if (xmp != before) {
      meta.replaceStreamData(xmp, QPDFObjectHandle::newNull(),
                             QPDFObjectHandle::newNull());
      f.note("removed the " + f.str("family") + " identification from the document metadata");
    }
  }
  return true;
}

bool fixDocumentSetView(const FixupCtx& f) {
  QPDFObjectHandle root = f.ctx.pdf.getRoot();
  if (f.has("page_layout")) root.replaceKey("/PageLayout", QPDFObjectHandle::newName("/" + f.str("page_layout")));
  if (f.has("page_mode")) root.replaceKey("/PageMode", QPDFObjectHandle::newName("/" + f.str("page_mode")));
  static const std::pair<const char*, const char*> prefs[] = {{"show_title", "/DisplayDocTitle"},
                                                               {"fit_window", "/FitWindow"},
                                                               {"center_window", "/CenterWindow"},
                                                               {"hide_toolbar", "/HideToolbar"},
                                                               {"hide_menubar", "/HideMenubar"}};
  for (const auto& [param, key] : prefs) {
    if (!f.has(param)) continue;
    QPDFObjectHandle vp = root.getKey("/ViewerPreferences");
    if (!vp.isDictionary()) {
      vp = QPDFObjectHandle::newDictionary();
      root.replaceKey("/ViewerPreferences", vp);
    }
    vp.replaceKey(key, QPDFObjectHandle::newBool(f.flag(param)));
  }
  f.note("set how the document opens");
  return true;
}

bool fixTransparencySetBlendSpace(const FixupCtx& f) {
  std::string space = f.str("space");
  QPDFObjectHandle icc;
  if (space == "output_intent") {
    QPDFObjectHandle ois = f.ctx.pdf.getRoot().getKey("/OutputIntents");
    for (int i = 0; ois.isArray() && i < ois.getArrayNItems() && !icc.isInitialized(); ++i) {
      QPDFObjectHandle oi = ois.getArrayItem(i);
      if (oi.isDictionary() && oi.getKey("/DestOutputProfile").isStream()) icc = oi.getKey("/DestOutputProfile");
    }
    if (!icc.isInitialized()) space = "CMYK";
  }
  bool cmyk = space == "CMYK";
  if (space == "CMYK" || space == "sRGB") {
    icc = buildIccStream(f.ctx, cmyk ? kCmykIcc : kSrgbIcc, cmyk ? kCmykIccLen : kSrgbIccLen, cmyk ? 4 : 3);
  }
  QPDFObjectHandle csRef = QPDFObjectHandle::newName("/DeviceGray");
  if (icc.isInitialized()) {
    QPDFObjectHandle cs = QPDFObjectHandle::newArray();
    cs.appendItem(QPDFObjectHandle::newName("/ICCBased"));
    cs.appendItem(icc);
    csRef = f.ctx.pdf.makeIndirectObject(cs);
  }
  int touched = 0;
  for (auto& ph : f.pages) {
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle grp = page.getKey("/Group");
    if (!grp.isDictionary()) {
      grp = QPDFObjectHandle::newDictionary();
      grp.replaceKey("/S", QPDFObjectHandle::newName("/Transparency"));
      grp.replaceKey("/I", QPDFObjectHandle::newBool(true));
      page.replaceKey("/Group", grp);
    }
    if (nameIs(grp.getKey("/S"), "/Transparency")) {
      grp.replaceKey("/CS", csRef);
      ++touched;
    }
  }
  if (touched) {
    f.note("set the transparency blending colour space on " + std::to_string(touched) +
         " page(s) to " + space);
  }
  return true;
}

bool fixImageSetInterpolation(const FixupCtx& f) {
  bool remove = f.str("value") == "remove";
  bool on = f.str("value") == "on";
  int n = 0;
  for (QPDFObjectHandle obj : f.ctx.pdf.getAllObjects()) {
    if (obj.isStream() && nameIs(obj.getDict().getKey("/Subtype"), "/Image")) {
      QPDFObjectHandle d = obj.getDict();
      if (remove && d.hasKey("/Interpolate")) {
        d.removeKey("/Interpolate");
        ++n;
      } else if (!remove) {
        d.replaceKey("/Interpolate", QPDFObjectHandle::newBool(on));
        ++n;
      }
    }
  }
  if (n) f.note((remove ? "removed" : "set") + std::string(" interpolation on ") +
              std::to_string(n) + " image(s)");
  return true;
}

bool fixTransparencyRemoveUnneededGroups(const FixupCtx& f) {
  int n = 0;
  for (auto& ph : f.pages) {
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
  if (n) f.note("removed " + std::to_string(n) + " unnecessary transparency group(s)");
  return true;
}

std::vector<QPDFObjectHandle> colorantArrays(QPDF& pdf) {
  std::vector<QPDFObjectHandle> out;
  std::function<void(QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle o, int depth) {
    if (depth > 12) return;
    if (o.isStream()) o = o.getDict();
    if (o.isArray()) {
      std::string fam = o.getArrayNItems() >= 2 ? nameOf(o.getArrayItem(0)) : std::string();
      if (fam == "/Separation" || fam == "/DeviceN") out.push_back(o);
      for (int i = 0; i < o.getArrayNItems(); ++i) {
        if (!o.getArrayItem(i).isIndirect()) walk(o.getArrayItem(i), depth + 1);
      }
    } else if (o.isDictionary()) {
      for (const std::string& k : o.getKeys()) {
        if (!o.getKey(k).isIndirect()) walk(o.getKey(k), depth + 1);
      }
    }
  };
  for (QPDFObjectHandle obj : pdf.getAllObjects()) walk(obj, 0);
  return out;
}

bool fixSpotRenameByMap(const FixupCtx& f) {
  std::map<std::string, std::string> canon;
  std::string mapFrom, mapTo;
  bool registration = f.fix.id == "spot.registration_to_black";
  if (f.fix.id == "spot.rename") {
    mapFrom = f.str("from");
    mapTo = f.str("to");
    if (mapFrom.empty() || mapTo.empty()) return true;
  }
  auto fromMatches = [&](const std::string& name) {
    if (name == mapFrom) return true;
    try {
      return std::regex_match(name, std::regex(mapFrom));
    } catch (...) {
      return false;
    }
  };
  int renamed = 0;
  for (QPDFObjectHandle obj : colorantArrays(f.ctx.pdf)) {
    std::string fam = nameOf(obj.getArrayItem(0));
    auto renameName = [&](QPDFObjectHandle holder, int idx) {
      QPDFObjectHandle nm = holder.getArrayItem(idx);
      if (!nm.isName() || nm.getName().size() < 2) return;
      std::string name = nm.getName().substr(1);
      std::string newName = name;
      if (registration) {
        if (name == "All" || name == "Registration") newName = "Black";
      } else if (!mapFrom.empty()) {
        if (fromMatches(name)) newName = mapTo;
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
  if (renamed) f.note("unified " + std::to_string(renamed) + " spot colourant name(s)");
  return true;
}

bool fixColorNchannelToDevicen(const FixupCtx& f) {
  int n = 0;
  for (QPDFObjectHandle obj : f.ctx.pdf.getAllObjects()) {
    if (obj.isArray() && obj.getArrayNItems() >= 5 &&
        nameIs(obj.getArrayItem(0), "/DeviceN") &&
        obj.getArrayItem(4).isDictionary() &&
        nameIs(obj.getArrayItem(4).getKey("/Subtype"), "/NChannel")) {
      obj.getArrayItem(4).removeKey("/Subtype");
      ++n;
    }
  }
  if (n) f.note("converted " + std::to_string(n) + " NChannel space(s) to plain DeviceN");
  return true;
}

bool fixSpotNormaliseNames(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  int renamed = 0;
  auto fix = [&](const std::string& name) {
    std::string fixed = validUtf8(name) ? name : cp1252ToUtf8(name);
    if (f.str("system") == "pantone") return normalizePantone(fixed);
    if (f.str("system") == "hks") return normalizeHks(fixed);
    return fixed;
  };
  auto renameAt = [&](QPDFObjectHandle holder, int idx) {
    QPDFObjectHandle nm = holder.getArrayItem(idx);
    if (!nm.isName() || nm.getName().size() < 2) return;
    std::string name = nm.getName().substr(1);
    std::string nn = fix(name);
    if (nn != name) {
      holder.setArrayItem(idx, QPDFObjectHandle::newName("/" + nn));
      ++renamed;
    }
  };
  for (QPDFObjectHandle obj : colorantArrays(ctx.pdf)) {
    std::string fam = nameOf(obj.getArrayItem(0));
    if (fam == "/Separation") {
      renameAt(obj, 1);
    } else if (fam == "/DeviceN" && obj.getArrayItem(1).isArray()) {
      QPDFObjectHandle names = obj.getArrayItem(1);
      for (int i = 0; i < names.getArrayNItems(); ++i) renameAt(names, i);
      QPDFObjectHandle attrs = obj.getArrayNItems() > 4 ? obj.getArrayItem(4) : QPDFObjectHandle();
      QPDFObjectHandle cols = attrs.isDictionary() ? attrs.getKey("/Colorants") : QPDFObjectHandle();
      if (cols.isDictionary()) {
        for (const std::string& k : cols.getKeys()) {
          std::string nn = "/" + fix(k.substr(1));
          if (nn != k && !cols.hasKey(nn)) {
            cols.replaceKey(nn, cols.getKey(k));
            cols.removeKey(k);
          }
        }
      }
    }
  }
  if (renamed) f.note("normalized " + std::to_string(renamed) + " spot colour name(s)");
  return true;
}

bool fixColorIccToDevice(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  std::set<QPDFObjGen> seen;
  int n = 0;
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) replaceIccInTree(obj, seen, 0, n);
  if (n) f.note("replaced " + std::to_string(n) + " ICC-based colour space(s) with device colour");
  return true;
}

bool fixColorIccToV2(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  int n = 0;
  std::set<QPDFObjGen> done;
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    QPDFObjectHandle st;
    if (obj.isArray() && obj.getArrayNItems() >= 2 && nameIs(obj.getArrayItem(0), "/ICCBased")) {
      st = obj.getArrayItem(1);
    } else if (obj.isDictionary() && obj.getKey("/DestOutputProfile").isStream()) {
      st = obj.getKey("/DestOutputProfile");
    } else if (obj.isStream() && obj.getDict().getKey("/N").isInteger() &&
               obj.getDict().hasKey("/Alternate")) {
      st = obj;
    }
    if (!st.isStream() || !done.insert(st.getObjGen()).second) continue;
    if (downgradeIcc(st)) ++n;
  }
  if (n) f.note("rewrote " + std::to_string(n) + " ICC v4 profile(s) as ICC v2");
  return true;
}

bool fixOutputIntentRemoveProfile(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  int n = 0;
  QPDFObjectHandle ois = ctx.pdf.getRoot().getKey("/OutputIntents");
  for (int i = 0; ois.isArray() && i < ois.getArrayNItems(); ++i) {
    QPDFObjectHandle oi = ois.getArrayItem(i);
    if (oi.isDictionary() && oi.hasKey("/DestOutputProfile")) {
      oi.removeKey("/DestOutputProfile");
      ++n;
    }
  }
  if (n) f.note("removed the embedded ICC profile from " + std::to_string(n) + " output intent(s)");
  return true;
}

bool fixOutputIntentSet(const FixupCtx& f) {
  QPDFObjectHandle root = f.ctx.pdf.getRoot();
  if (f.has("profile")) {
    if (!f.flag("keep_existing") && root.hasKey("/OutputIntents")) {
      root.removeKey("/OutputIntents");
      f.note("replaced the document output intent");
    }
    return true;
  }
  QPDFObjectHandle ois = root.getKey("/OutputIntents");
  int n = 0;
  for (int i = 0; ois.isArray() && i < ois.getArrayNItems(); ++i) {
    QPDFObjectHandle oi = ois.getArrayItem(i);
    if (!oi.isDictionary()) continue;
    if (f.has("identifier")) oi.replaceKey("/OutputConditionIdentifier", QPDFObjectHandle::newUnicodeString(f.str("identifier")));
    if (f.has("condition")) oi.replaceKey("/Info", QPDFObjectHandle::newUnicodeString(f.str("condition")));
    if (f.has("identifier") || f.has("condition")) ++n;
  }
  if (n) f.note("updated the identification of " + std::to_string(n) + " output intent(s)");
  return true;
}

bool fixSpotUnifyDefinitions(const FixupCtx& f) {
  std::map<std::string, std::vector<QPDFObjectHandle>> uses;
  for (QPDFObjectHandle obj : f.ctx.pdf.getAllObjects()) {
    if (!obj.isArray() || obj.getArrayNItems() < 4 || !nameIs(obj.getArrayItem(0), "/Separation")) continue;
    std::string nm = nameOf(obj.getArrayItem(1));
    if (nm.size() < 2 || nm == "/All" || nm == "/None") continue;
    uses[nm].push_back(obj);
  }
  bool first = f.str("prefer") == "first";
  int n = 0;
  for (auto& [name, list] : uses) {
    if (list.size() < 2) continue;
    auto key = [](QPDFObjectHandle o) { return o.getArrayItem(2).unparseResolved() + o.getArrayItem(3).unparseResolved(); };
    std::map<std::string, int> count;
    for (QPDFObjectHandle o : list) ++count[key(o)];
    if (count.size() < 2) continue;
    QPDFObjectHandle ref = list.front();
    if (!first) {
      for (QPDFObjectHandle o : list) {
        if (count[key(o)] > count[key(ref)]) ref = o;
      }
    }
    std::string want = key(ref);
    for (QPDFObjectHandle o : list) {
      if (key(o) == want) continue;
      o.setArrayItem(2, ref.getArrayItem(2));
      o.setArrayItem(3, ref.getArrayItem(3));
      ++n;
    }
  }
  if (n) f.note("gave " + std::to_string(n) + " spot colour definition(s) the same appearance");
  return true;
}

bool fixColorMap(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  ColorValue src;
  if (!parseColorText(f.str("from"), src)) {
    f.fail("colour \"" + f.str("from") + "\" not understood");
    return true;
  }
  double tol = f.num("tolerance") / 100.0;
  bool toSpot = f.fix.id == "color.process_to_spot";
  unsigned objects = ColorRule::kAllObjects;
  const std::string& which = f.str("objects");
  if (which == "text") objects = ColorRule::kText;
  if (which == "vector") objects = ColorRule::kVector | ColorRule::kShading;
  if (which == "images") objects = ColorRule::kImageLossy | ColorRule::kImageLossless;
  ColorValue dst = src;
  QPDFObjectHandle spot;
  if (toSpot) {
    if (!f.has("spot")) return true;
    spot = separationSpace(ctx, f.str("spot"), src);
  } else if (!parseColorText(f.str("to"), dst)) {
    f.fail("colour \"" + f.str("to") + "\" not understood");
    return true;
  }
  int n = remapContentColors(ctx, [&](const ColorValue& in, unsigned cls, ColorValue& out) {
    if (!(cls & objects) || !sameColor(in, src, tol)) return false;
    if (spot.isInitialized()) {
      out.space = spot;
      out.values = {1};
    } else {
      out = dst;
    }
    return true;
  });
  if (n) f.note("remapped " + std::to_string(n) + " colour value(s)");
  return true;
}

bool fixInkWhitePlate(const FixupCtx& f) {
  std::string spot = f.has("name") ? f.str("name") : std::string("White");
  ColorValue look;
  if (!parseColorText(f.str("look"), look)) {
    look.family = "/DeviceCMYK";
    look.values = {0, 0, 0, 0.2};
  }
  bool whole = f.str("mode") == "background";
  std::map<int, std::vector<Box>> boxes;
  bool select = !whole && f.has("objects");
  if (select && !objectBoxesByRules(f.ctx, ruleIdsFor(f.ctx, f.str("objects")), boxes)) {
    f.fail("the check \"" + f.str("objects") + "\" cannot select objects");
    return true;
  }
  int n = 0;
  for (size_t slot = 0; slot < f.pages.size(); ++slot) {
    const std::vector<Box> none;
    auto it = boxes.find(f.indexOf(slot) + 1);
    const std::vector<Box>* keep = select ? (it == boxes.end() ? &none : &it->second) : nullptr;
    if (placeWhitePlate(f.ctx, f.pages[slot], f.indexOf(slot), 300, spot, look, 0, whole, keep)) ++n;
  }
  if (n) f.note("created a \"" + spot + "\" underprint plate on " + std::to_string(n) + " page(s)");
  return true;
}

bool fixBleedAroundShape(const FixupCtx& f) {
  double width = f.num("amount");
  double dpi = f.num("resolution") >= 72 ? f.num("resolution") : 300;
  if (width <= 0) return true;
  std::map<int, std::vector<Box>> boxes;
  bool select = f.has("die_line_check");
  if (select && !objectBoxesByRules(f.ctx, ruleIdsFor(f.ctx, f.str("die_line_check")), boxes)) {
    f.fail("the check \"" + f.str("die_line_check") + "\" cannot select the cutting contour");
    return true;
  }
  int n = 0;
  for (size_t slot = 0; slot < f.pages.size(); ++slot) {
    auto it = boxes.find(f.indexOf(slot) + 1);
    if (select && it == boxes.end()) continue;
    if (placeShapeBleed(f.ctx, f.pages[slot], f.indexOf(slot), dpi, width, "missing", false, select ? &it->second : nullptr)) ++n;
  }
  if (n) f.note("generated " + fmtFixed(width, 2) + " pt bleed around the artwork shape on " + std::to_string(n) + " page(s)");
  return true;
}

bool fixOutlineFromHeadings(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  QPDFObjectHandle root = ctx.pdf.getRoot();
  QPDFObjectHandle existing = root.getKey("/Outlines");
  if (existing.isDictionary() && existing.getKey("/First").isDictionary() &&
      true) {
    return true;
  }
  std::vector<std::vector<TextRun>> runs;
  std::map<QPDFObjGen, int> pageNo;
  for (size_t i = 0; i < f.pages.size(); ++i) {
    runs.push_back(extractPageText(f.pages[i]));
    pageNo[f.pages[i].getObjectHandle().getObjGen()] = static_cast<int>(i);
  }
  struct Head {
    int level;
    int page;
    std::string title;
    double x, y;
  };
  std::vector<Head> heads;
  QPDFObjectHandle tree = root.getKey("/StructTreeRoot");
  if (tree.isDictionary() && f.str("source") != "text") {
    std::vector<std::tuple<int, QPDFObjectHandle, std::vector<int>>> found;
    std::set<QPDFObjGen> seen;
    collectHeadings(tree, tree.getKey("/RoleMap"), QPDFObjectHandle(), found, seen, 0);
    for (auto& [level, pg, mcids] : found) {
      if (!pg.isDictionary() || !pageNo.count(pg.getObjGen())) continue;
      int p = pageNo[pg.getObjGen()];
      std::string title;
      double x = 0, y = 0;
      bool first = true;
      for (const TextRun& r : runs[p]) {
        if (std::find(mcids.begin(), mcids.end(), r.mcid) == mcids.end()) continue;
        title += r.text;
        if (first) {
          x = r.x0;
          y = r.y1;
          first = false;
        }
      }
      title = trimmed(title);
      if (!title.empty()) heads.push_back({level, p, title, x, y});
    }
  }
  if (heads.empty()) {
    std::map<long, size_t> sizeChars;
    std::vector<Line> all;
    for (size_t p = 0; p < runs.size(); ++p) {
      for (Line& l : pageLines(static_cast<int>(p), runs[p])) {
        sizeChars[std::lround(l.size * 2)] += l.text.size();
        all.push_back(l);
      }
    }
    long body = 0;
    size_t best = 0;
    for (auto& kv : sizeChars) {
      if (kv.second > best) {
        best = kv.second;
        body = kv.first;
      }
    }
    std::set<long, std::greater<long>> headSizes;
    for (Line& l : all) {
      std::string t = trimmed(l.text);
      if (l.size * 2 >= body * 1.25 && t.size() >= 2 && t.size() <= 160 && !l.artifact) headSizes.insert(std::lround(l.size * 2));
    }
    std::map<long, int> levelOf;
    int lv = 0;
    for (long hs : headSizes) {
      if (lv < 3) levelOf[hs] = ++lv;
    }
    auto wordy = [](const std::string& t) {
      size_t letters = 0, total = 0;
      for (unsigned char c : t) {
        if (c == ' ') continue;
        ++total;
        if (std::isalpha(c) || c >= 0x80) ++letters;
      }
      int run = 0, worst = 0;
      for (unsigned char c : t) {
        bool cons = std::isalpha(c) && !std::strchr("aeiouyAEIOUY", c);
        run = cons ? run + 1 : 0;
        worst = std::max(worst, run);
      }
      bool word = false;
      std::string cur;
      for (size_t i = 0; i <= t.size(); ++i) {
        unsigned char c = i < t.size() ? static_cast<unsigned char>(t[i]) : ' ';
        if (std::isalpha(c) || c >= 0x80) {
          cur += static_cast<char>(c);
          continue;
        }
        if (cur.size() >= 3 && (cur.find_first_of("aeiouyAEIOUY") != std::string::npos ||
                                static_cast<unsigned char>(cur[0]) >= 0x80)) {
          word = true;
        }
        cur.clear();
      }
      return total >= 3 && letters * 10 >= total * 7 && worst <= 6 && word;
    };
    std::map<std::string, int> repeats;
    for (Line& l : all) {
      if (levelOf.count(std::lround(l.size * 2))) ++repeats[trimmed(l.text)];
    }
    const Line* prev = nullptr;
    for (Line& l : all) {
      auto it = levelOf.find(std::lround(l.size * 2));
      std::string t = trimmed(l.text);
      if (it == levelOf.end() || t.size() > 160 || l.artifact || !wordy(t) || repeats[t] > 3) {
        prev = nullptr;
        continue;
      }
      if (prev && !heads.empty() && prev->page == l.page && heads.back().level == it->second &&
          std::fabs(prev->size - l.size) < 0.5 && prev->baseline - l.baseline > 0 &&
          prev->baseline - l.baseline < l.size * 1.8 && heads.back().title.size() + t.size() < 200) {
        heads.back().title += " " + t;
        prev = &l;
        continue;
      }
      heads.push_back({it->second, l.page, t, l.x0, l.y1});
      prev = &l;
    }
  }
  if (heads.empty()) return true;
  QPDFObjectHandle outlines = ctx.pdf.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Outlines >>"));
  std::vector<std::pair<int, QPDFObjectHandle>> stackItems;
  std::map<QPDFObjGen, std::vector<QPDFObjectHandle>> children;
  std::vector<QPDFObjectHandle> topLevel;
  for (const Head& h : heads) {
    QPDFObjectHandle item = outlineItem(ctx, h.title, f.pages[h.page].getObjectHandle(), h.x, h.y);
    while (!stackItems.empty() && stackItems.back().first >= h.level) stackItems.pop_back();
    if (stackItems.empty()) topLevel.push_back(item);
    else children[stackItems.back().second.getObjGen()].push_back(item);
    stackItems.push_back({h.level, item});
  }
  std::function<void(QPDFObjectHandle, std::vector<QPDFObjectHandle>&)> attach =
      [&](QPDFObjectHandle parent, std::vector<QPDFObjectHandle>& kids) {
        linkChildren(parent, kids);
        for (auto& k : kids) {
          auto it = children.find(k.getObjGen());
          if (it != children.end()) attach(k, it->second);
        }
      };
  attach(outlines, topLevel);
  root.replaceKey("/Outlines", outlines);
  f.note("created " + std::to_string(heads.size()) + " bookmark(s) from headings");
  return true;
}

bool fixStructureMarkHeaderFooter(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  std::vector<std::vector<Line>> lines;
  std::map<std::string, std::set<int>> seenOn;
  auto key = [](const std::string& t) {
    std::string k;
    for (char c : t) k += std::isdigit(static_cast<unsigned char>(c)) ? '#' : c;
    return trimmed(k);
  };
  std::vector<std::pair<double, double>> heights;
  for (size_t p = 0; p < f.pages.size(); ++p) {
    lines.push_back(pageLines(static_cast<int>(p), extractPageText(f.pages[p])));
    QPDFObjectHandle cb = f.pages[p].getAttribute("/CropBox", false);
    if (!cb.isArray()) cb = f.pages[p].getAttribute("/MediaBox", true);
    double y1 = cb.isArray() ? numOf(cb.getArrayItem(1), 0) : 0, y2 = cb.isArray() ? numOf(cb.getArrayItem(3), 792) : 792;
    heights.push_back({y1, y2});
    for (Line& l : lines.back()) {
      double band = (y2 - y1) * 0.12;
      bool top = l.y0 > y2 - band, bottom = l.y1 < y1 + band;
      if ((top || bottom) && !l.tagged && !l.artifact && !l.form) {
        seenOn[std::string(top ? "T" : "B") + key(l.text)].insert(static_cast<int>(p));
      }
    }
  }
  int pagesN = static_cast<int>(f.pages.size());
  int marked = 0;
  for (size_t p = 0; p < f.pages.size(); ++p) {
    std::map<size_t, std::string> wrap;
    double y1 = heights[p].first, y2 = heights[p].second;
    for (Line& l : lines[p]) {
      double band = (y2 - y1) * 0.12;
      bool top = l.y0 > y2 - band, bottom = l.y1 < y1 + band;
      if (!(top || bottom) || l.tagged || l.artifact || l.form) continue;
      std::string k = std::string(top ? "T" : "B") + key(l.text);
      bool pageNum = trimmed(l.text).find_first_not_of("0123456789 -/.|ivxlcdmIVXLCDMPage") == std::string::npos;
      int repeats = static_cast<int>(seenOn[k].size());
      if (!(pageNum || (pagesN >= 2 && repeats >= std::max(2, pagesN * 2 / 5)))) continue;
      for (size_t op : l.ops) {
        wrap[op] = std::string("/Artifact << /Type /Pagination /Subtype /") + (top ? "Header" : "Footer") + " >>";
      }
    }
    if (wrap.empty()) continue;
    try {
      Pl_Buffer buf("artifact wrap");
      WrapOpsFilter wf(wrap);
      f.pages[p].filterContents(&wf, &buf);
      auto b = buf.getBufferSharedPointer();
      f.pages[p].getObjectHandle().replaceKey(
          "/Contents", ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(
                           &ctx.pdf, std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize()))));
      marked += static_cast<int>(wrap.size());
    } catch (...) {
    }
  }
  if (marked) f.note("marked " + std::to_string(marked) + " header/footer text object(s) as pagination artifacts");
  return true;
}

bool fixLinkFromText(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  static const std::regex url(R"re((https?://[^\s<>()"]+|www\.[^\s<>()"]+|[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}))re");
  int made = 0;
  for (auto& ph : f.pages) {
    std::vector<TextRun> runs = extractPageText(ph);
    QPDFObjectHandle page = ph.getObjectHandle();
    for (const TextRun& r : runs) {
      if (r.inForm || r.charX.empty()) continue;
      for (std::sregex_iterator it(r.text.begin(), r.text.end(), url), end; it != end; ++it) {
        std::string u = it->str();
        while (!u.empty() && (u.back() == '.' || u.back() == ',' || u.back() == ';')) u.pop_back();
        size_t b0 = static_cast<size_t>(it->position()), b1 = b0 + u.size();
        size_t g0 = static_cast<size_t>(std::upper_bound(r.byteAt.begin(), r.byteAt.end(), b0) - r.byteAt.begin()) - 1;
        size_t g1 = static_cast<size_t>(std::lower_bound(r.byteAt.begin(), r.byteAt.end(), b1) - r.byteAt.begin());
        double x0 = r.charX[std::min(g0, r.charX.size() - 1)];
        double x1 = g1 < r.charX.size() ? r.charX[g1] : r.x1;
        std::string target = u.find('@') != std::string::npos && u.find("://") == std::string::npos ? "mailto:" + u
                             : u.rfind("www.", 0) == 0 ? "http://" + u : u;
        QPDFObjectHandle an = QPDFObjectHandle::parse("<< /Type /Annot /Subtype /Link /Border [0 0 0] /F 4 >>");
        QPDFObjectHandle rect = QPDFObjectHandle::newArray();
        for (double v : {std::min(x0, x1), r.y0, std::max(x0, x1), r.y1}) rect.appendItem(QPDFObjectHandle::newReal(v, 2));
        an.replaceKey("/Rect", rect);
        QPDFObjectHandle act = QPDFObjectHandle::newDictionary();
        act.replaceKey("/S", QPDFObjectHandle::newName("/URI"));
        act.replaceKey("/URI", QPDFObjectHandle::newString(target));
        an.replaceKey("/A", act);
        an.replaceKey("/P", page);
        QPDFObjectHandle annots = page.getKey("/Annots");
        if (!annots.isArray()) {
          annots = QPDFObjectHandle::newArray();
          page.replaceKey("/Annots", annots);
        }
        annots.appendItem(ctx.pdf.makeIndirectObject(an));
        ++made;
      }
    }
  }
  if (made) f.note("created " + std::to_string(made) + " link(s) from URLs in the text");
  return true;
}

bool fixStructureResolveRoleMap(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  QPDFObjectHandle tree = ctx.pdf.getRoot().getKey("/StructTreeRoot");
  if (!tree.isDictionary()) return true;
  QPDFObjectHandle rm = tree.getKey("/RoleMap");
  auto resolveRole = [&](std::string t) {
    for (int i = 0; i < 16 && rm.isDictionary() && rm.hasKey(t) && !standardTypes().count(t); ++i) {
      t = nameOf(rm.getKey(t));
    }
    return t;
  };
  int n = 0;
  std::set<QPDFObjGen> seen;
  walkStruct(tree, seen, 0, [&](QPDFObjectHandle e) {
    std::string t = nameOf(e.getKey("/S"));
    if (t.empty() || standardTypes().count(t)) return;
    std::string target = resolveRole(t);
    if (!standardTypes().count(target)) {
      return;
      target = guessStandard(t, hasContentKids(e));
    }
    e.replaceKey("/S", QPDFObjectHandle::newName(target));
    ++n;
  });
  if (n && rm.isDictionary()) tree.removeKey("/RoleMap");
  if (n) f.note("set " + std::to_string(n) + " structure element(s) to standard types");
  return true;
}

bool fixStructureActualTextForSymbols(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  int n = 0;
  for (auto& ph : f.pages) {
    std::map<size_t, std::string> wrap;
    for (const TextRun& r : extractPageText(ph)) {
      std::string fl = lowerAscii(r.font);
      if (r.inForm || r.text.empty() || r.artifact) continue;
      if (fl.find("symbol") == std::string::npos && fl.find("dingbat") == std::string::npos) continue;
      QPDFObjectHandle str = QPDFObjectHandle::newUnicodeString(r.text);
      wrap[r.op] = "/Span << /ActualText " + str.unparse() + " >>";
    }
    if (wrap.empty()) continue;
    try {
      Pl_Buffer buf("actualtext");
      WrapOpsFilter wf(wrap);
      ph.filterContents(&wf, &buf);
      auto b = buf.getBufferSharedPointer();
      ph.getObjectHandle().replaceKey("/Contents", ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(
                                                       &ctx.pdf, std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize()))));
      n += static_cast<int>(wrap.size());
    } catch (...) {
    }
  }
  if (n) f.note("added ActualText to " + std::to_string(n) + " symbol-font text object(s)");
  return true;
}

bool fixFormGenerateAppearances(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  QPDFObjectHandle acro = ctx.pdf.getRoot().getKey("/AcroForm");
  int made = 0;
  for (auto& ph : f.pages) {
    QPDFObjectHandle annots = ph.getObjectHandle().getKey("/Annots");
    for (int i = 0; annots.isArray() && i < annots.getArrayNItems(); ++i) {
      QPDFObjectHandle w = annots.getArrayItem(i);
      if (!w.isDictionary() || !nameIs(w.getKey("/Subtype"), "/Widget")) continue;
      QPDFObjectHandle ap = w.getKey("/AP");
      bool missing = !ap.isDictionary() || ap.getKey("/N").isNull();
      if (!missing) continue;
      appearanceStream(ctx, w, acro, made);
    }
  }
  if (acro.isDictionary() && made) acro.removeKey("/NeedAppearances");
  if (made) f.note("generated appearances for " + std::to_string(made) + " form field(s)");
  return true;
}

bool fixPlaceShape(const FixupCtx& f) {
  const std::string& id = f.fix.id;
  bool bc = id == "place.barcode", circle = id == "place.circle";
  double w = circle ? f.num("diameter") : f.num("width"), h = circle ? f.num("diameter") : f.num("height");
  ColorValue col = stampColor(f);
  bool stroke = !bc && f.str("paint") == "stroke";
  std::string body = "q " + fillOp(col, stroke) + " 1 w ";
  if (bc) {
    BarcodeMatrix m = makeBarcode(f.str("symbology"), f.str("data"));
    if (!m.error.empty() || m.width <= 0) {
      f.fail("barcode not placed: " + (m.error.empty() ? std::string("empty symbol") : m.error));
      return true;
    }
    if (w <= 0) w = m.width * (m.linear ? 1.0 : 2.0);
    if (h <= 0) h = m.linear ? 50 : w * m.height / m.width;
    double mw = w / m.width, mh = m.linear ? h : h / m.height;
    for (int row = 0; row < (m.linear ? 1 : m.height); ++row) {
      int col0 = 0;
      while (col0 < m.width) {
        if (!m.dark[static_cast<size_t>(row) * m.width + col0]) {
          ++col0;
          continue;
        }
        int run = col0;
        while (run < m.width && m.dark[static_cast<size_t>(row) * m.width + run]) ++run;
        double top = m.linear ? 0 : h - (row + 1) * mh;
        body += fmtFixed(col0 * mw, 4) + " " + fmtFixed(top, 4) + " " + fmtFixed((run - col0) * mw, 4) + " " + fmtFixed(mh, 4) + " re ";
        col0 = run;
      }
    }
    body += "f";
  } else if (w <= 0 || h <= 0) {
    f.fail("no size given");
    return true;
  } else if (circle) {
    double k = 0.5523, rx = w / 2, ry = h / 2, cx = rx, cy = ry;
    body += fmtFixed(cx + rx, 3) + " " + fmtFixed(cy, 3) + " m " +
            fmtFixed(cx + rx, 3) + " " + fmtFixed(cy + k * ry, 3) + " " + fmtFixed(cx + k * rx, 3) + " " + fmtFixed(cy + ry, 3) + " " + fmtFixed(cx, 3) + " " + fmtFixed(cy + ry, 3) + " c " +
            fmtFixed(cx - k * rx, 3) + " " + fmtFixed(cy + ry, 3) + " " + fmtFixed(cx - rx, 3) + " " + fmtFixed(cy + k * ry, 3) + " " + fmtFixed(cx - rx, 3) + " " + fmtFixed(cy, 3) + " c " +
            fmtFixed(cx - rx, 3) + " " + fmtFixed(cy - k * ry, 3) + " " + fmtFixed(cx - k * rx, 3) + " " + fmtFixed(cy - ry, 3) + " " + fmtFixed(cx, 3) + " " + fmtFixed(cy - ry, 3) + " c " +
            fmtFixed(cx + k * rx, 3) + " " + fmtFixed(cy - ry, 3) + " " + fmtFixed(cx + rx, 3) + " " + fmtFixed(cy - k * ry, 3) + " " + fmtFixed(cx + rx, 3) + " " + fmtFixed(cy, 3) + " c h " + (stroke ? "S" : "f");
  } else {
    body += "0 0 " + fmtFixed(w, 3) + " " + fmtFixed(h, 3) + " re " + (stroke ? "S" : "f");
  }
  body += " Q";
  QPDFObjectHandle form = formOf(f.ctx, body, w, h, QPDFObjectHandle());
  int n = placeStamp(f, [&](int, double& fw, double& fh) {
    fw = w;
    fh = h;
    return form;
  }, bc ? "/KuraBarcode" : circle ? "/KuraCircle" : "/KuraRect", 1.0);
  if (n) f.note("placed on " + std::to_string(n) + " page(s)");
  return true;
}

bool fixPlaceHtml(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  const std::string& name = f.str("template");
  std::string html = name.find('<') != std::string::npos ? name : readFileText(resolvePath(f, name));
  std::string pdfBytes;
  if (html.empty()) {
    f.fail("HTML template \"" + name + "\" not found");
    return true;
  }
  if (!ctx.opt.renderHtml || !ctx.opt.renderHtml(html, htmlStampScript(ctx), pdfBytes) || pdfBytes.empty()) {
    f.fail("HTML could not be rendered (no HTML renderer available)");
    return true;
  }
  auto other = std::make_shared<QPDF>();
  try {
    ctx.foreignBuffers.push_back(std::make_shared<std::string>(std::move(pdfBytes)));
    other->processMemoryFile("html", ctx.foreignBuffers.back()->data(), ctx.foreignBuffers.back()->size());
  } catch (...) {
    return true;
  }
  QPDFPageDocumentHelper odh(*other);
  std::vector<QPDFPageObjectHelper> opages = odh.getAllPages();
  if (opages.empty()) return true;
  ctx.foreignDocs.push_back(other);
  bool perPage = f.flag("per_page");
  std::map<size_t, QPDFObjectHandle> forms;
  int n = placeStamp(f, [&](int pageNo, double& w, double& h) {
    size_t k = perPage ? std::min(static_cast<size_t>(pageNo - 1), opages.size() - 1) : 0;
    QPDFObjectHandle mb = opages[k].getAttribute("/MediaBox", true);
    w = mb.isArray() ? numOf(mb.getArrayItem(2), 0) - numOf(mb.getArrayItem(0), 0) : 0;
    h = mb.isArray() ? numOf(mb.getArrayItem(3), 0) - numOf(mb.getArrayItem(1), 0) : 0;
    if (!forms.count(k)) forms[k] = ctx.pdf.copyForeignObject(opages[k].getFormXObjectForPage());
    return forms[k];
  }, "/KuraHtml", 1.0);
  if (n) f.note("placed the HTML stamp on " + std::to_string(n) + " page(s)");
  return true;
}

bool fixPlacePdf(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  std::string bytes = readFileText(resolvePath(f, f.str("file")));
  if (bytes.empty()) {
    f.fail("file to place could not be read: " + f.str("file"));
    return true;
  }
  auto other = std::make_shared<QPDF>();
  try {
    ctx.foreignBuffers.push_back(std::make_shared<std::string>(std::move(bytes)));
    other->processMemoryFile("placed", ctx.foreignBuffers.back()->data(), ctx.foreignBuffers.back()->size());
  } catch (...) {
    f.fail("file to place is not a readable PDF");
    return true;
  }
  QPDFPageDocumentHelper odh(*other);
  std::vector<QPDFPageObjectHelper> opages = odh.getAllPages();
  int pn = std::max(1, static_cast<int>(f.num("page")));
  if (opages.empty() || pn > static_cast<int>(opages.size())) return true;
  QPDFObjectHandle mb = opages[pn - 1].getAttribute("/MediaBox", true);
  double w = mb.isArray() ? numOf(mb.getArrayItem(2), 0) - numOf(mb.getArrayItem(0), 0) : 0;
  double h = mb.isArray() ? numOf(mb.getArrayItem(3), 0) - numOf(mb.getArrayItem(1), 0) : 0;
  QPDFObjectHandle form = ctx.pdf.copyForeignObject(opages[pn - 1].getFormXObjectForPage());
  ctx.foreignDocs.push_back(other);
  double sc = f.num("scale") > 0 ? f.num("scale") : 1;
  int n = placeStamp(f, [&](int, double& fw, double& fh) {
    fw = w;
    fh = h;
    return form;
  }, "/KuraPlaced", sc);
  if (n) f.note("placed page " + std::to_string(pn) + " of " + f.str("file") + " on " + std::to_string(n) + " page(s)");
  return true;
}

bool fixTextAddInvisibleCopy(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  int n = 0;
  for (auto& ph : f.pages) {
    int runs = 0;
    std::string t = invisibleTextLayer(ph, runs);
    if (!runs) continue;
    ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, std::string("q\n")), true);
    ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, "\nQ\n" + t), false);
    n += runs;
  }
  if (n) f.note("added an invisible copy of " + std::to_string(n) + " text object(s)");
  return true;
}

bool fixPageRasterize(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  double dpi = f.num("resolution");
  if (dpi < 36) dpi = 300;
  int n = 0, idx = -1, slot = -1;
  for (auto& ph : f.pages) {
    idx = f.indexOf(static_cast<size_t>(++slot));
    if (!ctx.opt.rasterizePage) break;
    if (rasterFlattenPage(ctx, ph, idx, true, dpi)) ++n;
  }
  if (n) f.note("replaced the content of " + std::to_string(n) + " page(s) with a " + fmtFixed(dpi, 0) + " ppi image");
  return true;
}

bool fixContentBringToFront(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  const std::string& which = f.str("objects");
  bool text = which.empty() || which == "text" || which == "all";
  bool vector = which == "vector" || which == "all";
  if (!text && !vector) {
    f.fail("objects must be text, vector or all");
    return true;
  }
  int n = rewritePages(ctx, f.pages, StateTrackFilter::kBringTextFront, 0, text, vector);
  if (n) f.note("brought " + std::to_string(n) + " " + (vector && !text ? "vector" : text && !vector ? "text" : "text and vector") + " object(s) to the front");
  return true;
}

bool fixContentFillAsStroke(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  double tol = f.num("max_width");
  int n = rewritePages(ctx, f.pages, StateTrackFilter::kFillToStroke, tol > 0 ? tol : 0.5);
  if (n) f.note("converted " + std::to_string(n) + " hairline fill(s) to strokes");
  return true;
}

bool fixContentSmoothLinesToCurves(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  int n = rewritePages(ctx, f.pages, StateTrackFilter::kSmoothLines, 0);
  if (n) f.note("converted " + std::to_string(n) + " smooth polyline(s) to curves");
  return true;
}

bool fixContentFixNegativeDash(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  int n = rewritePages(ctx, f.pages, StateTrackFilter::kDashRepair, 0);
  for (QPDFObjectHandle o : ctx.pdf.getAllObjects()) {
    if (!o.isDictionary() || !nameIs(o.getKey("/Type"), "/ExtGState")) continue;
    QPDFObjectHandle d = o.getKey("/D");
    if (!d.isArray() || d.getArrayNItems() < 1 || !d.getArrayItem(0).isArray()) continue;
    QPDFObjectHandle arr = d.getArrayItem(0);
    bool bad = false;
    for (int i = 0; i < arr.getArrayNItems(); ++i) bad = bad || numOf(arr.getArrayItem(i), 0) < 0;
    if (bad) {
      o.replaceKey("/D", QPDFObjectHandle::parse("[[] 0]"));
      ++n;
    }
  }
  if (n) f.note("repaired " + std::to_string(n) + " invalid dash pattern(s)");
  return true;
}

bool fixOutlineRepair(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  QPDFObjectHandle ol = ctx.pdf.getRoot().getKey("/Outlines");
  if (ol.isDictionary()) {
    int fixedN = 0;
    std::set<QPDFObjGen> seen;
    relinkOutline(ol, seen, 0, fixedN);
    f.note("rebuilt the bookmark tree links" + (fixedN ? " (" + std::to_string(fixedN) + " repair(s))" : std::string()));
  }
  return true;
}

bool fixFontRename(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  int n = 0;
  if (f.has("from") && f.has("to")) {
    for (QPDFObjectHandle o : ctx.pdf.getAllObjects()) {
      if (!o.isDictionary() || !nameIs(o.getKey("/Type"), "/Font") || !o.getKey("/BaseFont").isName()) continue;
      std::string name = o.getKey("/BaseFont").getName().substr(1);
      std::string prefix = name.size() > 7 && name[6] == '+' ? name.substr(0, 7) : std::string();
      std::string bare = name.substr(prefix.size());
      std::string tail;
      if (nameIs(o.getKey("/Subtype"), "/Type0") && bare.compare(0, f.str("from").size(), f.str("from")) == 0 &&
          bare.size() > f.str("from").size() && bare[f.str("from").size()] == '-') {
        tail = bare.substr(f.str("from").size());
        bare = f.str("from");
      }
      if (bare != f.str("from")) continue;
      o.replaceKey("/BaseFont", QPDFObjectHandle::newName("/" + prefix + f.str("to") + tail));
      ++n;
    }
  }
  for (QPDFObjectHandle o : ctx.pdf.getAllObjects()) {
    if (!o.isDictionary() || !nameIs(o.getKey("/Type"), "/Font")) continue;
    QPDFObjectHandle bf = o.getKey("/BaseFont");
    QPDFObjectHandle target = o;
    if (nameIs(o.getKey("/Subtype"), "/Type0")) {
      QPDFObjectHandle df = o.getKey("/DescendantFonts");
      if (df.isArray() && df.getArrayNItems()) target = df.getArrayItem(0);
    }
    QPDFObjectHandle fd = target.getKey("/FontDescriptor");
    if (!bf.isName() || !fd.isDictionary()) continue;
    std::string want = bf.getName();
    if (nameIs(o.getKey("/Subtype"), "/Type0")) {
      size_t dash = want.find('-', want.size() > 7 && want[7] == '+' ? 8 : 1);
      QPDFObjectHandle dbf = target.getKey("/BaseFont");
      if (dbf.isName()) want = dbf.getName();
      else if (dash != std::string::npos && want.find("Identity") != std::string::npos) want = want.substr(0, dash);
    }
    if (!nameIs(fd.getKey("/FontName"), want)) {
      fd.replaceKey("/FontName", QPDFObjectHandle::newName(want));
      ++n;
    }
  }
  if (n) f.note("renamed " + std::to_string(n) + " font name(s)");
  return true;
}

bool fixPageRepairResources(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  completeResources(ctx);
  return true;
}

bool fixImageMergeFragments(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  int merged = mergeImageStrips(ctx, f.pages);
  if (merged) f.note("merged " + std::to_string(merged) + " image fragment(s)");
  return true;
}

bool fixInkDotGain(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  auto curve = toneCurve(f.str("curve"));
  if (curve.size() < 2) {
    f.fail("tone curve \"" + f.str("curve") + "\" not understood");
    return true;
  }
  bool dev = true, rgb = false;
  int n = remapContentColors(ctx, [&](const ColorValue& in, unsigned, ColorValue& out) {
    bool ink = in.family == "/DeviceCMYK" || in.family == "/Separation" || in.family == "/DeviceN";
    bool gray = in.family == "/DeviceGray";
    bool isRgb = in.family == "/DeviceRGB";
    if (!((dev && (ink || gray)) || (rgb && isRgb))) return false;
    out = in;
    for (double& v : out.values) v = (ink ? applyTone(curve, v) : 1 - applyTone(curve, 1 - v));
    if (in.family == "/DeviceCMYK" || gray || isRgb) out.space = QPDFObjectHandle();
    return true;
  });
  int imgs = 0;
  for (QPDFObjectHandle o : ctx.pdf.getAllObjects()) {
    if (!o.isStream() || !nameIs(o.getDict().getKey("/Subtype"), "/Image")) continue;
    QPDFObjectHandle d = o.getDict();
    if (d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue()) continue;
    QPDFObjectHandle cs = d.getKey("/ColorSpace");
    std::string fam = cs.isName() ? cs.getName() : cs.isArray() && cs.getArrayNItems() ? nameOf(cs.getArrayItem(0)) : "";
    bool ink = fam == "/DeviceCMYK" || fam == "/Separation" || fam == "/DeviceN";
    bool gray = fam == "/DeviceGray", isRgb = fam == "/DeviceRGB";
    if (!((dev && (ink || gray)) || (rgb && isRgb))) continue;
    RawImage img = decodeImage(o);
    if (!img.ok) continue;
    unsigned char lut[256];
    for (int v = 0; v < 256; ++v) {
      double t = ink ? applyTone(curve, v / 255.0) : 1 - applyTone(curve, 1 - v / 255.0);
      lut[v] = static_cast<unsigned char>(std::lround(t * 255));
    }
    for (char& c : img.samples) c = static_cast<char>(lut[static_cast<unsigned char>(c)]);
    std::string lf = lastFilter(d);
    if (reencodeImage(o, img.samples, img.width, img.height, img.comps, lf == "/DCTDecode" && img.comps != 2)) ++imgs;
  }
  if (n || imgs) f.note("adjusted tone values of " + std::to_string(n) + " colour value(s) and " + std::to_string(imgs) + " image(s)");
  return true;
}

bool fixColorToLibrarySpot(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  std::vector<LibColor> lib = loadColorLibrary(resolvePath(f, f.str("library")));
  if (lib.empty()) {
    f.fail("no spot colour library could be read from \"" + f.str("library") + "\"");
    return true;
  }
  double maxDe = f.has("max_delta_e") ? f.num("max_delta_e") : 3.0;
  unsigned objects = ColorRule::kAllObjects;
  std::map<std::string, QPDFObjectHandle> spaces;
  int n = remapContentColors(ctx, [&](const ColorValue& in, unsigned cls, ColorValue& out) {
    if (!(cls & objects)) return false;
    if (in.family != "/DeviceCMYK" && in.family != "/DeviceRGB" && in.family != "/DeviceGray" && in.family != "/Lab") return false;
    double lab[3];
    if (!colorValueToLab(in, lab)) return false;
    const LibColor* best = nullptr;
    double bestDe = 1e9;
    for (const LibColor& c : lib) {
      double de = deltaE2000(lab, c.lab);
      if (de < bestDe) {
        bestDe = de;
        best = &c;
      }
    }
    if (!best || bestDe > maxDe) return false;
    auto it = spaces.find(best->name);
    if (it == spaces.end()) {
      ColorValue look;
      look.family = "/Lab";
      look.values = {best->lab[0], best->lab[1], best->lab[2]};
      it = spaces.emplace(best->name, separationSpace(ctx, best->name, look)).first;
    }
    out.space = it->second;
    out.values = {1};
    return true;
  });
  if (n) f.note("replaced " + std::to_string(n) + " colour value(s) with " + std::to_string(spaces.size()) + " best-matching spot colour(s)");
  return true;
}

bool fixColorEmbedMeasurements(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  std::map<std::string, std::array<double, 3>> found;
  for (QPDFObjectHandle o : ctx.pdf.getAllObjects()) {
    if (!o.isArray() || o.getArrayNItems() < 4 || !nameIs(o.getArrayItem(0), "/Separation")) continue;
    std::string nm = nameOf(o.getArrayItem(1));
    if (nm.size() < 2 || nm == "/All" || nm == "/None" || found.count(nm)) continue;
    ColorValue c;
    c.family = "/Separation";
    c.space = o;
    c.values = {1};
    double lab[3];
    if (colorValueToLab(c, lab)) found[nm] = {lab[0], lab[1], lab[2]};
  }
  std::string x = readFileText(resolvePath(f, f.str("source")));
  if (x.empty() && found.empty()) return true;
  bool given = !x.empty();
  if (!given) x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<cc:CxF xmlns:cc=\"http://colorexchangeformat.com/CxF3-core\">\n"
                  "<cc:Resources><cc:ObjectCollection>\n";
  int id = 0;
  for (auto& kv : given ? std::map<std::string, std::array<double, 3>>() : found) {
    x += "<cc:Object ObjectType=\"Standard\" Name=\"" + xmlEscape(kv.first.substr(1)) + "\" Id=\"o" + std::to_string(++id) +
         "\"><cc:ColorValues><cc:ColorCIELab ColorSpecification=\"D50_2\"><cc:L>" + fmtFixed(kv.second[0], 2) +
         "</cc:L><cc:A>" + fmtFixed(kv.second[1], 2) + "</cc:A><cc:B>" + fmtFixed(kv.second[2], 2) +
         "</cc:B></cc:ColorCIELab></cc:ColorValues></cc:Object>\n";
  }
  if (!given) x += "</cc:ObjectCollection><cc:ColorSpecificationCollection><cc:ColorSpecification Id=\"D50_2\">"
       "<cc:TristimulusSpec><cc:Illuminant>D50</cc:Illuminant><cc:Observer>2_Degree</cc:Observer>"
       "</cc:TristimulusSpec></cc:ColorSpecification></cc:ColorSpecificationCollection></cc:Resources>\n</cc:CxF>\n";
  QPDFObjectHandle ef = QPDFObjectHandle::newStream(&ctx.pdf, x);
  ef.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/EmbeddedFile"));
  ef.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/application#2Fvnd.cxf+xml"));
  QPDFObjectHandle spec = QPDFObjectHandle::parse("<< /Type /Filespec /F (spotcolors.cxf) /UF (spotcolors.cxf) /AFRelationship /Supplement /Desc (Spot colour definitions \\(CxF\\)) >>");
  QPDFObjectHandle efd = QPDFObjectHandle::newDictionary();
  efd.replaceKey("/F", ctx.pdf.makeIndirectObject(ef));
  efd.replaceKey("/UF", efd.getKey("/F"));
  spec.replaceKey("/EF", efd);
  spec = ctx.pdf.makeIndirectObject(spec);
  QPDFObjectHandle root = ctx.pdf.getRoot();
  QPDFObjectHandle names = root.getKey("/Names");
  if (!names.isDictionary()) {
    names = QPDFObjectHandle::newDictionary();
    root.replaceKey("/Names", names);
  }
  QPDFObjectHandle efTree = names.getKey("/EmbeddedFiles");
  if (!efTree.isDictionary()) {
    efTree = QPDFObjectHandle::parse("<< /Names [] >>");
    names.replaceKey("/EmbeddedFiles", efTree);
  }
  QPDFObjectHandle arr = efTree.getKey("/Names");
  if (arr.isArray()) {
    arr.appendItem(QPDFObjectHandle::newUnicodeString("spotcolors.cxf"));
    arr.appendItem(spec);
  }
  QPDFObjectHandle af = root.getKey("/AF");
  if (!af.isArray()) {
    af = QPDFObjectHandle::newArray();
    root.replaceKey("/AF", af);
  }
  af.appendItem(spec);
  f.note("embedded CxF definitions for " + std::to_string(found.size()) + " spot colour(s)");
  return true;
}

bool fixColorSplitDevicen(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  bool keepProcess = f.str("process_to") == "keep";
  int n = remapContentColors(ctx, [&](const ColorValue& in, unsigned, ColorValue& out) {
    if (in.family != "/DeviceN" || in.colorants.empty() || in.values.size() != in.colorants.size()) {
      return false;
    }
    bool process = true;
    for (const std::string& c : in.colorants) {
      if (c != "/Cyan" && c != "/Magenta" && c != "/Yellow" && c != "/Black") process = false;
    }
    if (process && !keepProcess) {
      out.family = "/DeviceCMYK";
      out.values = {0, 0, 0, 0};
      for (size_t i = 0; i < in.colorants.size(); ++i) {
        int slot = in.colorants[i] == "/Cyan" ? 0 : in.colorants[i] == "/Magenta" ? 1
                   : in.colorants[i] == "/Yellow" ? 2 : 3;
        out.values[slot] = in.values[i];
      }
      return true;
    }
    if (in.colorants.size() == 1 && in.space.isArray() && in.space.getArrayNItems() >= 4) {
      QPDFObjectHandle sep = QPDFObjectHandle::newArray();
      sep.appendItem(QPDFObjectHandle::newName("/Separation"));
      sep.appendItem(QPDFObjectHandle::newName(in.colorants[0]));
      sep.appendItem(in.space.getArrayItem(2));
      sep.appendItem(in.space.getArrayItem(3));
      out.space = ctx.pdf.makeIndirectObject(sep);
      out.values = in.values;
      return true;
    }
    return false;
  });
  if (n) f.note("split " + std::to_string(n) + " DeviceN colour value(s)");
  return true;
}

bool fixDocumentSetInfo(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  std::string key = f.str("key");
  std::string value = f.str("value");
  if (key == "Lang") {
    QPDFObjectHandle root = ctx.pdf.getRoot();
    bool has = root.getKey("/Lang").isString() && !root.getKey("/Lang").getUTF8Value().empty();
    if (value.empty() || (f.flag("only_if_empty") && has)) return true;
    root.replaceKey("/Lang", QPDFObjectHandle::newUnicodeString(value));
    f.note("set the document language to " + value);
    return true;
  }
  if (key == "custom") {
    key = f.str("name");
    if (key.empty() || key.find_first_of(" /()<>[]{}%#") != std::string::npos) return true;
  }
  QPDFObjectHandle tr = ctx.pdf.getTrailer();
  QPDFObjectHandle info = tr.getKey("/Info");
  if (!info.isDictionary()) {
    info = ctx.pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
    tr.replaceKey("/Info", info);
  }
  QPDFObjectHandle cur = info.getKey("/" + key);
  bool has = (cur.isString() && !cur.getUTF8Value().empty()) || cur.isName();
  if (f.flag("only_if_empty") && has) return true;
  if (key == "CreationDate" || key == "ModDate") {
    value = value.rfind("D:", 0) == 0 ? value : pdfNow(ctx);
    info.replaceKey("/" + key, QPDFObjectHandle::newString(value));
  } else if (key == "Trapped") {
    std::string v = lowerAscii(value);
    info.replaceKey("/Trapped", QPDFObjectHandle::newName(v == "true" ? "/True" : v == "false" ? "/False" : "/Unknown"));
  } else {
    if (key == "Title" && value == "file_name") {
      const std::string& path = ctx.opt.inputName.empty() ? ctx.opt.inputPath : ctx.opt.inputName;
      size_t slash = path.find_last_of("/\\");
      value = slash == std::string::npos ? path : path.substr(slash + 1);
    }
    if (value.empty()) return true;
    info.replaceKey("/" + key, QPDFObjectHandle::newUnicodeString(value));
  }
  f.note("set " + key);
  return true;
}

bool fixMetadataUncompressXmp(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  int n = 0;
  for (QPDFObjectHandle o : ctx.pdf.getAllObjects()) {
    if (!o.isStream() || !nameIs(o.getDict().getKey("/Subtype"), "/XML")) continue;
    if (o.getDict().getKey("/Filter").isNull()) continue;
    try {
      auto b = o.getStreamData(qpdf_dl_all);
      o.replaceStreamData(std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize()),
                          QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
      ++n;
    } catch (...) {
    }
  }
  if (n) f.note("uncompressed " + std::to_string(n) + " XMP metadata stream(s)");
  return true;
}

bool fixDocumentAssociateFiles(const FixupCtx& f) {
  QPDFObjectHandle root = f.ctx.pdf.getRoot();
  QPDFObjectHandle names = root.getKey("/Names");
  QPDFObjectHandle ef = names.isDictionary() ? names.getKey("/EmbeddedFiles") : QPDFObjectHandle();
  std::vector<QPDFObjectHandle> specs;
  std::function<void(QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle node, int depth) {
    if (!node.isDictionary() || depth > 32) return;
    QPDFObjectHandle arr = node.getKey("/Names");
    for (int i = 1; arr.isArray() && i < arr.getArrayNItems(); i += 2) specs.push_back(arr.getArrayItem(i));
    QPDFObjectHandle kids = node.getKey("/Kids");
    for (int i = 0; kids.isArray() && i < kids.getArrayNItems(); ++i) walk(kids.getArrayItem(i), depth + 1);
  };
  walk(ef, 0);
  QPDFObjectHandle af = root.getKey("/AF");
  if (!af.isArray()) {
    af = QPDFObjectHandle::newArray();
    root.replaceKey("/AF", af);
  }
  int n = 0;
  for (QPDFObjectHandle sp : specs) {
    if (!sp.isDictionary()) continue;
    if (!sp.hasKey("/AFRelationship")) sp.replaceKey("/AFRelationship", QPDFObjectHandle::newName("/" + f.str("relationship")));
    bool listed = false;
    for (int i = 0; i < af.getArrayNItems(); ++i) {
      if (af.getArrayItem(i).isIndirect() && sp.isIndirect() && af.getArrayItem(i).getObjGen() == sp.getObjGen()) listed = true;
    }
    if (!listed && sp.isIndirect()) {
      af.appendItem(sp);
      ++n;
    }
  }
  if (n) f.note("linked " + std::to_string(n) + " embedded file(s) through /AF");
  return true;
}

bool fixLayerSet(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  QPDFObjectHandle ocp = ctx.pdf.getRoot().getKey("/OCProperties");
  if (!ocp.isDictionary()) return true;
  QPDFObjectHandle d = ocp.getKey("/D");
  if (!d.isDictionary()) {
    d = QPDFObjectHandle::newDictionary();
    ocp.replaceKey("/D", d);
  }
  int n = 0;
  if (f.flag("name_unnamed")) {
    QPDFObjectHandle ocgs = ocp.getKey("/OCGs");
    for (int i = 0; ocgs.isArray() && i < ocgs.getArrayNItems(); ++i) {
      QPDFObjectHandle g = ocgs.getArrayItem(i);
      if (!g.isDictionary()) continue;
      if (!g.getKey("/Name").isString() || g.getKey("/Name").getUTF8Value().empty()) {
        g.replaceKey("/Name", QPDFObjectHandle::newUnicodeString("Layer " + std::to_string(i + 1)));
        ++n;
      }
    }
  }
  for (QPDFObjectHandle g : matchingLayers(ctx, f.str("match"))) {
    if (f.has("name")) g.replaceKey("/Name", QPDFObjectHandle::newUnicodeString(f.str("name")));
    if (f.has("default_state")) {
      bool on = f.str("default_state") == "on";
      setMembership(d, "/ON", g, on);
      setMembership(d, "/OFF", g, !on);
    }
    if (f.has("locked")) setMembership(d, "/Locked", g, f.flag("locked"));
    if (f.has("zoom_min")) usageDict(g, "/Zoom").replaceKey("/min", QPDFObjectHandle::newReal(f.num("zoom_min") / 100.0, 4));
    if (f.has("zoom_max")) usageDict(g, "/Zoom").replaceKey("/max", QPDFObjectHandle::newReal(f.num("zoom_max") / 100.0, 4));
    for (const auto& [param, cat] : {std::pair<const char*, std::string>{"print", "/Print"}, {"export", "/Export"}, {"view", "/View"}}) {
      if (!f.has(param)) continue;
      std::string key = cat + "State";
      if (f.str(param) == "when_visible") {
        QPDFObjectHandle u = g.getKey("/Usage");
        if (u.isDictionary() && u.hasKey(cat)) u.removeKey(cat);
        continue;
      }
      usageDict(g, cat).replaceKey(key, QPDFObjectHandle::newName(f.str(param) == "always" ? "/ON" : "/OFF"));
      addAutoState(ctx, cat, cat, g);
    }
    ++n;
  }
  if (n) f.note("updated " + std::to_string(n) + " layer(s)");
  return true;
}

bool fixGraphicsRemove(const FixupCtx& f) {
  static const std::map<std::string, std::vector<std::string>> keys = {
      {"flatness", {"/FL"}}, {"smoothness", {"/SM"}}, {"transfer", {"/TR", "/TR2"}},
      {"black_generation", {"/BG", "/BG2"}}, {"undercolour_removal", {"/UCR", "/UCR2"}}, {"rendering_intent", {"/RI"}}};
  std::vector<std::string> wanted = fixParamList(f.fix, "settings");
  if (wanted.empty()) {
    for (const auto& kv : keys) wanted.push_back(kv.first);
  }
  std::set<std::string> scrub;
  for (const std::string& w : wanted) {
    auto it = keys.find(w);
    if (it != keys.end()) scrub.insert(it->second.begin(), it->second.end());
  }
  scrubExtGStates(f.ctx, scrub);
  if (scrub.count("/RI")) {
    for (QPDFObjectHandle obj : f.ctx.pdf.getAllObjects()) {
      if (obj.isStream() && nameIs(obj.getDict().getKey("/Subtype"), "/Image") && obj.getDict().hasKey("/Intent")) {
        obj.getDict().removeKey("/Intent");
      }
    }
  }
  f.note("removed device-dependent graphics state settings");
  return true;
}

bool fixGraphicsSet(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  std::vector<QPDFObjectHandle> states;
  for (QPDFObjectHandle o : ctx.pdf.getAllObjects()) {
    if (o.isDictionary() && nameIs(o.getKey("/Type"), "/ExtGState")) states.push_back(o);
  }
  int n = 0;
  if (f.has("flatness")) {
    double nv = f.num("flatness");
    for (QPDFObjectHandle o : states) {
      if (o.hasKey("/FL")) {
        o.replaceKey("/FL", QPDFObjectHandle::newReal(nv, 2));
        ++n;
      }
    }
    for (auto& ph : f.pages) ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, fmtFixed(nv, 2) + " i\n"), true);
  }
  if (f.has("rendering_intent")) {
    for (QPDFObjectHandle o : states) o.replaceKey("/RI", QPDFObjectHandle::newName("/" + f.str("rendering_intent")));
    n += static_cast<int>(states.size());
  }
  if (f.has("overprint_mode")) {
    int opm = f.str("overprint_mode") == "1" ? 1 : 0;
    for (QPDFObjectHandle o : states) {
      bool op = (o.getKey("/OP").isBool() && o.getKey("/OP").getBoolValue()) ||
                (o.getKey("/op").isBool() && o.getKey("/op").getBoolValue());
      if ((op || o.hasKey("/OPM")) && numOf(o.getKey("/OPM"), 0) != opm) {
        o.replaceKey("/OPM", QPDFObjectHandle::newInteger(opm));
        ++n;
      }
    }
  }
  for (const auto& [param, key] : {std::pair<const char*, const char*>{"fill_blend_mode", "/op"}, {"stroke_blend_mode", "/OP"}}) {
    if (!f.has(param)) continue;
    std::string bm = f.str(param);
    for (QPDFObjectHandle o : states) {
      QPDFObjectHandle v = o.hasKey(key) ? o.getKey(key) : (std::string(key) == "/op" ? o.getKey("/OP") : QPDFObjectHandle());
      if (!v.isBool() || !v.getBoolValue()) continue;
      o.replaceKey("/BM", QPDFObjectHandle::newName("/" + bm));
      o.replaceKey(key, QPDFObjectHandle::newBool(false));
      ++n;
    }
  }
  for (const auto& [param, key] : {std::pair<const char*, const char*>{"fill_alpha", "/ca"}, {"stroke_alpha", "/CA"}}) {
    if (!f.has(param)) continue;
    for (QPDFObjectHandle o : states) o.replaceKey(key, QPDFObjectHandle::newReal(f.num(param), 3));
    n += static_cast<int>(states.size());
  }
  if (f.has("black_point_compensation")) {
    for (QPDFObjectHandle o : states) o.replaceKey("/UseBlackPtComp", QPDFObjectHandle::newName("/" + f.str("black_point_compensation")));
    n += static_cast<int>(states.size());
  }
  if (n) f.note("updated " + std::to_string(n) + " graphics state setting(s)");
  return true;
}

bool fixPageMirror(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  bool horiz = f.str("axis") != "vertical";
  for (auto& ph : f.pages) {
    QPDFObjectHandle mb = ph.getAttribute("/MediaBox", true);
    if (!mb.isArray() || mb.getArrayNItems() != 4) continue;
    double x1 = numOf(mb.getArrayItem(0), 0), y1 = numOf(mb.getArrayItem(1), 0);
    double x2 = numOf(mb.getArrayItem(2), 0), y2 = numOf(mb.getArrayItem(3), 0);
    double a = horiz ? -1 : 1, d = horiz ? 1 : -1;
    double e = horiz ? x1 + x2 : 0, fy = horiz ? 0 : y1 + y2;
    transformPageContent(ctx, ph, a, d, e, fy);
    QPDFObjectHandle page = ph.getObjectHandle();
    for (const char* k : {"/CropBox", "/TrimBox", "/BleedBox", "/ArtBox"}) scaleBox(page, k, a, d, e, fy);
    transformAnnots(page, a, d, e, fy);
  }
  f.note(std::string("mirrored pages ") + (horiz ? "horizontally" : "vertically"));
  return true;
}

bool fixPageMoveOrigin(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  int n = 0;
  for (auto& ph : f.pages) {
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle mb = ph.getAttribute("/MediaBox", true);
    if (!mb.isArray() || mb.getArrayNItems() != 4) continue;
    double dx = -numOf(mb.getArrayItem(0), 0), dy = -numOf(mb.getArrayItem(1), 0);
    if (dx == 0 && dy == 0) continue;
    if (!page.hasKey("/MediaBox")) page.replaceKey("/MediaBox", mb.shallowCopy());
    QPDFObjectHandle cb = ph.getAttribute("/CropBox", false);
    if (cb.isArray() && !page.hasKey("/CropBox")) page.replaceKey("/CropBox", cb.shallowCopy());
    transformPageContent(ctx, ph, 1, 1, dx, dy);
    for (const char* k : {"/MediaBox", "/CropBox", "/TrimBox", "/BleedBox", "/ArtBox"}) shiftBox(page, k, dx, dy);
    transformAnnots(page, 1, 1, dx, dy);
    ++n;
  }
  if (n) f.note("moved the MediaBox origin to 0,0 on " + std::to_string(n) + " page(s)");
  return true;
}

bool fixImageExpandFilterNames(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  static const std::map<std::string, std::string> full = {
      {"/AHx", "/ASCIIHexDecode"}, {"/A85", "/ASCII85Decode"}, {"/LZW", "/LZWDecode"},
      {"/Fl", "/FlateDecode"}, {"/RL", "/RunLengthDecode"}, {"/CCF", "/CCITTFaxDecode"},
      {"/DCT", "/DCTDecode"}};
  int n = 0;
  for (QPDFObjectHandle o : ctx.pdf.getAllObjects()) {
    if (!o.isStream()) continue;
    QPDFObjectHandle d = o.getDict();
    QPDFObjectHandle flt = d.getKey("/Filter");
    if (flt.isName() && full.count(flt.getName())) {
      d.replaceKey("/Filter", QPDFObjectHandle::newName(full.at(flt.getName())));
      ++n;
    } else if (flt.isArray()) {
      for (int i = 0; i < flt.getArrayNItems(); ++i) {
        QPDFObjectHandle it = flt.getArrayItem(i);
        if (it.isName() && full.count(it.getName())) {
          flt.setArrayItem(i, QPDFObjectHandle::newName(full.at(it.getName())));
          ++n;
        }
      }
    }
  }
  if (n) f.note("expanded " + std::to_string(n) + " abbreviated filter name(s)");
  return true;
}

bool fixImageRecode(const FixupCtx& f) {
  Ctx& ctx = f.ctx;
  bool sharpen = f.fix.id == "image.sharpen";
  int radius = sharpen ? std::max(1, static_cast<int>(std::lround(f.num("radius")))) : 1;
  double amount = sharpen ? f.num("amount") : 0;
  int quality = sharpen ? 90 : std::min(100, std::max(1, static_cast<int>(std::lround(f.num("quality")))));
  int n = 0;
  for (QPDFObjectHandle o : ctx.pdf.getAllObjects()) {
    if (!o.isStream()) continue;
    QPDFObjectHandle d = o.getDict();
    if (!nameIs(d.getKey("/Subtype"), "/Image")) continue;
    if (d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue()) continue;
    std::string lf = lastFilter(d);
    bool jpx = lf == "/JPXDecode";
    if (!sharpen && !jpx) continue;
    RawImage img = decodeImage(o);
    if (!img.ok || (img.comps != 1 && img.comps != 3 && img.comps != 4)) continue;
    QPDFObjectHandle cs = d.getKey("/ColorSpace");
    bool indexed = cs.isArray() && cs.getArrayNItems() > 0 && (nameIs(cs.getArrayItem(0), "/Indexed") || nameIs(cs.getArrayItem(0), "/I"));
    if (sharpen) {
      if (numOf(d.getKey("/BitsPerComponent"), 8) == 1 || indexed) continue;
      unsharp(img.samples, img.width, img.height, img.comps, radius, amount);
    }
    if (cs.isNull() || indexed) {
      d.replaceKey("/ColorSpace", QPDFObjectHandle::newName(img.comps == 4 ? "/DeviceCMYK" : img.comps == 3 ? "/DeviceRGB" : "/DeviceGray"));
    }
    bool lossy = jpx || lf == "/DCTDecode" || lf == "/DCT";
    if (reencodeImage(o, img.samples, img.width, img.height, img.comps, lossy, quality)) {
      d.removeKey("/SMaskInData");
      ++n;
    }
  }
  if (n) {
    f.note(sharpen ? "sharpened " + std::to_string(n) + " image(s)"
                                : "re-encoded " + std::to_string(n) + " JPEG 2000 image(s) as JPEG");
  }
  return true;
}

double stdFontTextWidth(const std::string& base, const std::string& text, double size) {
  std::string lower = lowerAscii(base);
  std::string key = lower.find("courier") != std::string::npos ? "mono" : lower.find("times") != std::string::npos ? "serif" : "sans";
  bool bold = lower.find("bold") != std::string::npos;
  bool italic = lower.find("italic") != std::string::npos || lower.find("oblique") != std::string::npos;
  key += bold && italic ? "_bi" : bold ? "_b" : italic ? "_i" : "";
  const FontAsset* asset = nullptr;
  for (const std::string& k : {key + ":ttf", key, std::string("sans:ttf")}) {
    for (unsigned int i = 0; i < kFontAssetCount && !asset; ++i) {
      if (k == kFontAssets[i].key) asset = &kFontAssets[i];
    }
    if (asset) break;
  }
  FtLib lib;
  FT_Face face = nullptr;
  if (!asset || !lib.lib || FT_New_Memory_Face(lib.lib, asset->data, static_cast<FT_Long>(asset->len), 0, &face) || !face) return -1;
  double units = 0;
  for (unsigned char c : text) {
    FT_UInt g = FT_Get_Char_Index(face, winAnsiToUnicode(c));
    if (FT_Load_Glyph(face, g, FT_LOAD_NO_SCALE) == 0) units += face->glyph->metrics.horiAdvance;
  }
  double em = face->units_per_EM ? face->units_per_EM : 1000;
  FT_Done_Face(face);
  return units / em * size;
}

bool fixPlaceText(const FixupCtx& f) {
  double size = f.num("size") > 0 ? f.num("size") : 10;
  static const std::set<std::string> std14 = {"Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic", "Helvetica",
                                              "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique", "Courier",
                                              "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique"};
  std::string font = f.str("font");
  bool mono = lowerAscii(font).find("courier") != std::string::npos || lowerAscii(font).find("mono") != std::string::npos;
  std::string base = std14.count(font) ? font : (mono ? "Courier" : "Helvetica");
  mono = base.rfind("Courier", 0) == 0;
  ColorValue col = stampColor(f);
  QPDFObjectHandle fontDict = QPDFObjectHandle::newDictionary();
  fontDict.replaceKey("/Type", QPDFObjectHandle::newName("/Font"));
  fontDict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Type1"));
  fontDict.replaceKey("/BaseFont", QPDFObjectHandle::newName("/" + base));
  fontDict.replaceKey("/Encoding", QPDFObjectHandle::newName("/WinAnsiEncoding"));
  QPDFObjectHandle fontRef = f.ctx.pdf.makeIndirectObject(fontDict);
  std::string date = pdfNow(f.ctx);
  if (date.rfind("D:", 0) == 0 && date.size() >= 10) date = date.substr(2, 4) + "-" + date.substr(6, 2) + "-" + date.substr(8, 2);
  int n = placeStamp(f, [&](int pageNo, double& w, double& h) {
    std::string text = f.str("text");
    for (const auto& [key, val] : {std::pair<std::string, std::string>{"{page}", std::to_string(pageNo)},
                                  {"{pages}", std::to_string(f.pageCount)}, {"{date}", date}}) {
      for (size_t p = text.find(key); p != std::string::npos; p = text.find(key, p + val.size())) text.replace(p, key.size(), val);
    }
    std::string esc;
    for (char c : text) {
      if (c == '(' || c == ')' || c == '\\') esc += '\\';
      esc += c;
    }
    w = stdFontTextWidth(base, text, size);
    if (w < 0) w = text.size() * size * (mono ? 0.6 : 0.52);
    h = size * 1.2;
    QPDFObjectHandle fres = QPDFObjectHandle::newDictionary();
    fres.replaceKey("/KuraStampF", fontRef);
    QPDFObjectHandle res = QPDFObjectHandle::newDictionary();
    res.replaceKey("/Font", fres);
    return formOf(f.ctx, "BT " + fillOp(col, false) + " /KuraStampF " + fmtFixed(size, 2) + " Tf 0 " + fmtFixed(size * 0.22, 3) +
                             " Td (" + esc + ") Tj ET", w, h, res);
  }, "/KuraText", 1.0);
  if (n) f.note("placed text on " + std::to_string(n) + " page(s)");
  return true;
}

bool fixAnnotationHandle(const FixupCtx& f) {
  std::set<std::string> sel;
  for (const std::string& t : fixParamList(f.fix, "types")) sel.insert(t);
  const std::string& act = f.str("action");
  int n = 0;
  static const std::set<std::string> multimedia = {"Screen", "Movie", "Sound", "RichMedia", "3D"};
  static const std::set<std::string> known = {
      "Text", "Link", "FreeText", "Line", "Square", "Circle", "Polygon", "PolyLine",
      "Highlight", "Underline", "Squiggly", "StrikeOut", "Stamp", "Caret", "Ink",
      "Popup", "FileAttachment", "Sound", "Movie", "Widget", "Screen", "PrinterMark",
      "TrapNet", "Watermark", "3D", "Redact", "Projection", "RichMedia"};
  for (auto& ph : f.pages) {
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (!annots.isArray()) continue;
    std::string flat;
    for (int i = annots.getArrayNItems() - 1; i >= 0; --i) {
      QPDFObjectHandle an = annots.getArrayItem(i);
      if (!an.isDictionary()) continue;
      std::string st = nameOf(an.getKey("/Subtype"));
      if (st.size() > 1) st = st.substr(1);
      bool match = sel.count("all") || sel.count(st) || (sel.count("multimedia") && multimedia.count(st)) ||
                   (sel.count("unknown") && !known.count(st));
      if (!match) continue;
      long long fl = an.getKey("/F").isInteger() ? an.getKey("/F").getIntValue() : 0;
      if (act == "remove") {
        annots.eraseItem(i);
      } else if (act == "hide") {
        an.replaceKey("/F", QPDFObjectHandle::newInteger((fl | 2) & ~4));
      } else if (act == "print") {
        an.replaceKey("/F", QPDFObjectHandle::newInteger((fl | 4) & ~(1 | 2 | 32)));
      } else {
        QPDFAnnotationObjectHelper ah(an);
        QPDFObjectHandle ap = ah.getAppearanceStream("/N");
        if (ap.isStream()) {
          std::string name = "/KuraFlat" + std::to_string(i);
          QPDFObjectHandle res = ph.getAttribute("/Resources", true);
          QPDFObjectHandle xo = res.getKey("/XObject");
          if (!xo.isDictionary()) {
            xo = QPDFObjectHandle::newDictionary();
            res.replaceKey("/XObject", xo);
          }
          xo.replaceKey(name, ap);
          flat += ah.getPageContentForAppearance(name, 0, 0, an_invisible | an_hidden);
        }
        annots.eraseItem(i);
      }
      ++n;
    }
    if (!flat.empty()) {
      ph.addPageContents(QPDFObjectHandle::newStream(&f.ctx.pdf, std::string("q\n")), true);
      ph.addPageContents(QPDFObjectHandle::newStream(&f.ctx.pdf, "\nQ\n" + flat), false);
    }
  }
  if (n) f.note(act + " applied to " + std::to_string(n) + " annotation(s)");
  return true;
}

bool fixLayerMoveObjects(const FixupCtx& f) {
  std::string step = f.str("processing_step");
  std::string label = f.has("layer") ? f.str("layer") : (step.empty() ? std::string("Layer") : step);
  QPDFObjectHandle ocgRef = findOrAddLayer(f.ctx, label, step);
  if (f.has("objects")) {
    std::vector<std::string> rules = ruleIdsFor(f.ctx, f.str("objects"));
    int stuck = 0;
    int objs = rules.empty() ? -1 : layerObjectsByRules(f.ctx, f.pages, rules, ocgRef, stuck);
    if (objs >= 0) {
      if (stuck) f.fail(std::to_string(stuck) + " matching object(s) inside patterns, Type 3 glyphs or annotation appearances were left in place");
      f.note("moved " + std::to_string(objs) + " object(s) selected by the check onto layer " + label);
      return true;
    }
    f.fail("the check \"" + f.str("objects") + "\" cannot select single objects; the whole page was layered");
  }
  for (auto& ph : f.pages) {
    QPDFObjectHandle res = ph.getAttribute("/Resources", true);
    if (!res.isDictionary()) continue;
    QPDFObjectHandle props = res.getKey("/Properties");
    if (!props.isDictionary()) {
      props = QPDFObjectHandle::newDictionary();
      res.replaceKey("/Properties", props);
    }
    props.replaceKey("/KuraOC1", ocgRef);
    ph.addPageContents(QPDFObjectHandle::newStream(&f.ctx.pdf, std::string("/OC /KuraOC1 BDC\n")), true);
    ph.addPageContents(QPDFObjectHandle::newStream(&f.ctx.pdf, std::string("\nEMC")), false);
  }
  f.note("placed page content on layer " + label);
  return true;
}

bool fixLayerFlatten(const FixupCtx& f) {
  QPDFObjectHandle root = f.ctx.pdf.getRoot();
  QPDFObjectHandle ocp = root.getKey("/OCProperties");
  if (ocp.isDictionary()) {
    std::set<QPDFObjGen> hiddenOcgs;
    QPDFObjectHandle dflt = ocp.getKey("/D");
    if (dflt.isDictionary() && !f.flag("keep_hidden")) {
      if (nameOf(dflt.getKey("/BaseState")) == "/OFF") {
        QPDFObjectHandle all = ocp.getKey("/OCGs");
        std::set<QPDFObjGen> on;
        QPDFObjectHandle onArr = dflt.getKey("/ON");
        if (onArr.isArray()) {
          for (int i = 0; i < onArr.getArrayNItems(); ++i) {
            if (onArr.getArrayItem(i).isIndirect()) on.insert(onArr.getArrayItem(i).getObjGen());
          }
        }
        if (all.isArray()) {
          for (int i = 0; i < all.getArrayNItems(); ++i) {
            QPDFObjectHandle g = all.getArrayItem(i);
            if (g.isIndirect() && !on.count(g.getObjGen())) hiddenOcgs.insert(g.getObjGen());
          }
        }
      }
      QPDFObjectHandle offArr = dflt.getKey("/OFF");
      if (offArr.isArray()) {
        for (int i = 0; i < offArr.getArrayNItems(); ++i) {
          QPDFObjectHandle g = offArr.getArrayItem(i);
          if (g.isIndirect()) hiddenOcgs.insert(g.getObjGen());
        }
      }
    }
    auto ocIsHidden = [&](QPDFObjectHandle oc) {
      if (!oc.isDictionary()) return false;
      if (oc.isIndirect() && hiddenOcgs.count(oc.getObjGen())) return true;
      if (nameOf(oc.getKey("/Type")) != "/OCMD") return false;
      QPDFObjectHandle gs = oc.getKey("/OCGs");
      if (gs.isIndirect() && gs.isDictionary()) return hiddenOcgs.count(gs.getObjGen()) > 0;
      if (!gs.isArray() || gs.getArrayNItems() == 0) return false;
      std::string policy = nameOf(oc.getKey("/P"));
      int hiddenCount = 0, total = 0;
      for (int i = 0; i < gs.getArrayNItems(); ++i) {
        QPDFObjectHandle g = gs.getArrayItem(i);
        if (!g.isIndirect()) continue;
        ++total;
        if (hiddenOcgs.count(g.getObjGen())) ++hiddenCount;
      }
      if (total == 0) return false;
      if (policy == "/AllOn") return hiddenCount > 0;
      if (policy == "/AnyOff") return hiddenCount == 0;
      if (policy == "/AllOff") return hiddenCount < total;
      return hiddenCount == total;
    };
    long long droppedMarks = 0, droppedObjects = 0;
    for (auto& ph : f.pages) {
      QPDFObjectHandle pg = ph.getObjectHandle();
      std::set<std::string> hiddenTags;
      QPDFObjectHandle res = ph.getAttribute("/Resources", true);
      if (res.isDictionary()) {
        QPDFObjectHandle props = res.getKey("/Properties");
        if (props.isDictionary()) {
          for (const std::string& k : props.getKeys()) {
            if (ocIsHidden(props.getKey(k))) hiddenTags.insert(k);
          }
        }
        QPDFObjectHandle xod = res.getKey("/XObject");
        if (xod.isDictionary()) {
          for (const std::string& k : xod.getKeys()) {
            QPDFObjectHandle xo = xod.getKey(k);
            if (!xo.isStream()) continue;
            QPDFObjectHandle oc = xo.getDict().getKey("/OC");
            if (oc.isDictionary() && ocIsHidden(oc)) {
              xod.removeKey(k);
              ++droppedObjects;
            } else if (!oc.isNull()) {
              xo.getDict().removeKey("/OC");
            }
          }
        }
      }
      QPDFObjectHandle annots = pg.getKey("/Annots");
      if (annots.isArray()) {
        QPDFObjectHandle keptAnnots = QPDFObjectHandle::newArray();
        for (int i = 0; i < annots.getArrayNItems(); ++i) {
          QPDFObjectHandle a = annots.getArrayItem(i);
          QPDFObjectHandle oc = a.isDictionary() ? a.getKey("/OC") : QPDFObjectHandle::newNull();
          if (oc.isDictionary() && ocIsHidden(oc)) {
            ++droppedObjects;
            continue;
          }
          if (a.isDictionary() && !oc.isNull()) a.removeKey("/OC");
          keptAnnots.appendItem(a);
        }
        pg.replaceKey("/Annots", keptAnnots);
      }
      QPDFObjectHandle contents = pg.getKey("/Contents");
      if (!contents.isStream() && !contents.isArray()) continue;
      try {
        LayerFlattenFilter filter(hiddenTags, droppedMarks);
        Pl_Buffer buf("layer flatten");
        ph.filterContents(&filter, &buf);
        auto out = buf.getBufferSharedPointer();
        if (out) {
          pg.replaceKey(
              "/Contents",
              QPDFObjectHandle::newStream(
                  &f.ctx.pdf, std::string(reinterpret_cast<const char*>(out->getBuffer()),
                                        out->getSize())));
        }
      } catch (const std::exception&) {
      }
    }
    root.removeKey("/OCProperties");
    f.note("flattened layers (removed " + std::to_string(droppedMarks) +
         " hidden content region(s) and " + std::to_string(droppedObjects) +
         " hidden object(s); layer switching removed)");
  }
  return true;
}
}

namespace {
bool fixInkFilter(const FixupCtx& f) {
  const std::string& id = f.fix.id;
  bool knockWhite = id == "ink.knockout_white", opBlack = id == "ink.overprint_black";
  const std::string& objects = f.str("objects");
  bool textOnly = (knockWhite || opBlack) && objects == "text";
  bool vectorOnly = (knockWhite || opBlack) && objects == "vector";
  double minWidth = id == "ink.min_line_width" ? f.num("width") : 0;
  int forceTr = -1;
  if (id == "text.set_render_mode") {
    const std::string& m = f.str("mode");
    forceTr = m == "stroke" ? 1 : m == "fill_and_stroke" ? 2 : m == "invisible" ? 3 : 0;
  }
  if (id == "ink.min_line_width" && minWidth <= 0) return true;
  for (auto& ph : f.pages) {
    QPDFObjectHandle res = ph.getAttribute("/Resources", true);
    if (res.isDictionary() && (knockWhite || opBlack)) {
      QPDFObjectHandle egs = res.getKey("/ExtGState");
      if (!egs.isDictionary()) {
        egs = QPDFObjectHandle::newDictionary();
        res.replaceKey("/ExtGState", egs);
      }
      QPDFObjectHandle g = QPDFObjectHandle::newDictionary();
      g.replaceKey("/Type", QPDFObjectHandle::newName("/ExtGState"));
      g.replaceKey("/OP", QPDFObjectHandle::newBool(opBlack));
      g.replaceKey("/op", QPDFObjectHandle::newBool(opBlack));
      if (opBlack) g.replaceKey("/OPM", QPDFObjectHandle::newInteger(1));
      egs.replaceKey(knockWhite ? "/KuraKO" : "/KuraOB", f.ctx.pdf.makeIndirectObject(g));
    }
    QPDFObjectHandle pageContents = ph.getObjectHandle().getKey("/Contents");
    if (!pageContents.isStream() && !pageContents.isArray()) continue;
    ph.addContentTokenFilter(std::make_shared<OverprintFilter>(knockWhite, opBlack, textOnly, vectorOnly, minWidth, forceTr));
  }
  f.note(knockWhite ? "white objects set to knock out"
         : opBlack ? "solid black set to overprint"
         : minWidth > 0 ? "thin strokes raised to the minimum width" : "text render mode set");
  return true;
}

bool fixTransparencyFlatten(const FixupCtx& f) {
  double dpi = f.num("raster_resolution") >= 36 ? f.num("raster_resolution") : 300;
  int vec = 0, ras = 0;
  for (size_t slot = 0; slot < f.pages.size(); ++slot) {
    QPDFPageObjectHelper& ph = f.pages[slot];
    if (!pageHasTransparency(f.ctx, ph)) continue;
    if (vectorFlattenPage(f.ctx, ph, f.indexOf(slot), nullptr, dpi)) {
      ++vec;
    } else if (f.ctx.opt.rasterizePage && rasterFlattenPage(f.ctx, ph, f.indexOf(slot), true, dpi)) {
      ++ras;
    }
  }
  if (vec || ras) f.note("flattened transparency on " + std::to_string(vec + ras) + " page(s) (" + std::to_string(ras) + " as images)");
  return true;
}

bool fixSpotNormalise(const FixupCtx& f) {
  fixSpotNormaliseNames(f);
  if (f.str("system") == "any") fixSpotRenameByMap(f);
  return true;
}

using FixFn = bool (*)(const FixupCtx&);

const std::map<std::string, FixFn>& fixTable() {
  static const std::map<std::string, FixFn> table = {
      {"annotation.handle", fixAnnotationHandle},
      {"bleed.around_shape", fixBleedAroundShape},
      {"bleed.generate", fixBleedGenerate},
      {"color.embed_measurements", fixColorEmbedMeasurements},
      {"color.icc_to_device", fixColorIccToDevice},
      {"color.icc_to_v2", fixColorIccToV2},
      {"color.map", fixColorMap},
      {"color.nchannel_to_devicen", fixColorNchannelToDevicen},
      {"color.process_to_spot", fixColorMap},
      {"color.split_devicen", fixColorSplitDevicen},
      {"color.to_library_spot", fixColorToLibrarySpot},
      {"content.bring_to_front", fixContentBringToFront},
      {"content.fill_as_stroke", fixContentFillAsStroke},
      {"content.fix_negative_dash", fixContentFixNegativeDash},
      {"content.remove_outside", fixContentRemoveOutside},
      {"content.smooth_lines_to_curves", fixContentSmoothLinesToCurves},
      {"document.associate_files", fixDocumentAssociateFiles},
      {"document.remove_conformance_claim", fixDocumentRemoveConformanceClaim},
      {"document.set_info", fixDocumentSetInfo},
      {"document.set_view", fixDocumentSetView},
      {"font.rename", fixFontRename},
      {"form.generate_appearances", fixFormGenerateAppearances},
      {"graphics.remove", fixGraphicsRemove},
      {"graphics.set", fixGraphicsSet},
      {"image.expand_filter_names", fixImageExpandFilterNames},
      {"image.merge_fragments", fixImageMergeFragments},
      {"image.recompress", fixImageRecode},
      {"image.set_interpolation", fixImageSetInterpolation},
      {"image.sharpen", fixImageRecode},
      {"ink.dot_gain", fixInkDotGain},
      {"ink.knockout_white", fixInkFilter},
      {"ink.min_line_width", fixInkFilter},
      {"ink.overprint_black", fixInkFilter},
      {"ink.white_plate", fixInkWhitePlate},
      {"layer.flatten", fixLayerFlatten},
      {"layer.move_objects", fixLayerMoveObjects},
      {"layer.set", fixLayerSet},
      {"link.from_text", fixLinkFromText},
      {"metadata.uncompress_xmp", fixMetadataUncompressXmp},
      {"outline.from_headings", fixOutlineFromHeadings},
      {"outline.repair", fixOutlineRepair},
      {"output_intent.remove_profile", fixOutputIntentRemoveProfile},
      {"output_intent.set", fixOutputIntentSet},
      {"page.add_marks", fixPageAddMarks},
      {"page.crop_to", fixPageCropTo},
      {"page.mirror", fixPageMirror},
      {"page.move_origin", fixPageMoveOrigin},
      {"page.rasterize", fixPageRasterize},
      {"page.remove_user_unit", fixPageRemoveUserUnit},
      {"page.repair_resources", fixPageRepairResources},
      {"page.rotate", fixPageRotate},
      {"page.scale", fixPageScale},
      {"page.set_box", fixPageSetBox},
      {"place.barcode", fixPlaceShape},
      {"place.circle", fixPlaceShape},
      {"place.html", fixPlaceHtml},
      {"place.pdf", fixPlacePdf},
      {"place.rectangle", fixPlaceShape},
      {"place.text", fixPlaceText},
      {"spot.normalise_names", fixSpotNormalise},
      {"spot.registration_to_black", fixSpotRenameByMap},
      {"spot.rename", fixSpotRenameByMap},
      {"spot.unify_definitions", fixSpotUnifyDefinitions},
      {"structure.actual_text_for_symbols", fixStructureActualTextForSymbols},
      {"structure.mark_header_footer", fixStructureMarkHeaderFooter},
      {"structure.resolve_role_map", fixStructureResolveRoleMap},
      {"text.add_invisible_copy", fixTextAddInvisibleCopy},
      {"text.set_render_mode", fixInkFilter},
      {"transparency.flatten", fixTransparencyFlatten},
      {"transparency.remove_unneeded_groups", fixTransparencyRemoveUnneededGroups},
      {"transparency.set_blend_space", fixTransparencySetBlendSpace},
  };
  return table;
}
}

bool isContentFix(const std::string& id) { return fixTable().count(id) > 0; }

void passProfileFixups(Ctx& ctx) {
  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
  for (size_t fi = 0; fi < ctx.opt.profileFixOps.size(); ++fi) {
    PfFix fix;
    fix.id = ctx.opt.profileFixOps[fi].first;
    fix.fix = fixIndex(fix.id);
    fix.args = ctx.opt.profileFixOps[fi].second;
    auto fn = fixTable().find(fix.id);
    if (fn == fixTable().end()) continue;
    std::vector<QPDFPageObjectHelper> selected;
    std::vector<int> selectedIdx;
    auto rulesIt = ctx.opt.profileFixRules.find(fi);
    bool restricted = rulesIt != ctx.opt.profileFixRules.end() && !rulesIt->second.empty();
    if (restricted) {
      std::set<int> hitPages;
      for (const std::string& rid : rulesIt->second) {
        auto hp = ctx.ruleHitPages.find(rid);
        if (hp != ctx.ruleHitPages.end()) hitPages.insert(hp->second.begin(), hp->second.end());
      }
      for (size_t k = 0; k < pages.size(); ++k) {
        if (hitPages.count(0) || hitPages.count(static_cast<int>(k) + 1)) {
          selected.push_back(pages[k]);
          selectedIdx.push_back(static_cast<int>(k));
        }
      }
      if (selected.empty()) {
        ctx.issue("FIX_NOT_NEEDED", fix.id + ": its only_where checks found nothing", false);
        continue;
      }
    }
    FixupCtx f{ctx, restricted ? selected : pages, fix, selectedIdx, static_cast<int>(pages.size())};
    fn->second(f);
  }
}
}
