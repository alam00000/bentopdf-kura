#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "passes.hh"
#include "util.hh"

namespace pdfa {
namespace {
constexpr double kHairlinePt = 0.125;
constexpr double kRichBlackK = 0.9;
constexpr double kRichBlackInk = 0.05;
constexpr double kContoneMinPpi = 250.0;
constexpr double kContoneMaxPpi = 450.0;
constexpr double kBitonalMinPpi = 550.0;
constexpr double kBitonalMaxPpi = 3600.0;

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

struct Gs {
  Mat ctm;
  double lineWidth = 1.0;
  int renderMode = 0;
  double fill[4] = {0, 0, 0, 0};
  double stroke[4] = {0, 0, 0, 0};
  bool fillCmyk = false;
  bool strokeCmyk = false;
};

struct Tally {
  long long hairlines = 0;
  long long richBlack = 0;
  long long invisibleText = 0;
  long long lowRes = 0;
  long long highRes = 0;
  double minPpi = 0;
  double maxPpi = 0;
  std::set<int> hairlinePages, richPages, invisPages, lowPages, highPages;
};

bool richBlack(const double c[4]) {
  return c[3] > kRichBlackK &&
         (c[0] > kRichBlackInk || c[1] > kRichBlackInk || c[2] > kRichBlackInk);
}

double strokeScale(const Mat& m) {
  return (std::hypot(m.a, m.b) + std::hypot(m.c, m.d)) / 2.0;
}

struct Scanner : QPDFObjectHandle::ParserCallbacks {
  Gs gs;
  std::vector<Gs> stack;
  std::vector<double> nums;
  std::string lastName;
  QPDFObjectHandle res;
  Tally& tally;
  int page;
  std::vector<std::pair<std::string, Gs>> draws;

  Scanner(QPDFObjectHandle resources, Tally& t, int pageNum, const Gs& initial)
      : gs(initial), res(resources), tally(t), page(pageNum) {}

  void markHairline() {
    double eff = gs.lineWidth * strokeScale(gs.ctm);
    if (eff < kHairlinePt) {
      ++tally.hairlines;
      tally.hairlinePages.insert(page);
    }
  }

  void markRich(const double c[4], bool isCmyk) {
    if (isCmyk && richBlack(c)) {
      ++tally.richBlack;
      tally.richPages.insert(page);
    }
  }

  void applyExtGState() {
    if (lastName.empty() || !res.isDictionary()) return;
    QPDFObjectHandle egs = res.getKey("/ExtGState");
    if (!egs.isDictionary()) return;
    QPDFObjectHandle g = egs.getKey(lastName);
    if (!g.isDictionary()) return;
    if (g.getKey("/LW").isNumber()) gs.lineWidth = g.getKey("/LW").getNumericValue();
  }

  void handleObject(QPDFObjectHandle obj, size_t, size_t) override {
    if (!obj.isOperator()) {
      if (obj.isName()) {
        lastName = obj.getName();
      } else if (obj.isNumber()) {
        nums.push_back(obj.getNumericValue());
      }
      return;
    }
    std::string op = obj.getOperatorValue();
    if (op == "q") {
      stack.push_back(gs);
    } else if (op == "Q") {
      if (!stack.empty()) {
        gs = stack.back();
        stack.pop_back();
      }
    } else if (op == "cm" && nums.size() >= 6) {
      size_t n = nums.size();
      Mat m;
      m.a = nums[n - 6]; m.b = nums[n - 5]; m.c = nums[n - 4];
      m.d = nums[n - 3]; m.e = nums[n - 2]; m.f = nums[n - 1];
      gs.ctm = mul(m, gs.ctm);
    } else if (op == "w" && !nums.empty()) {
      gs.lineWidth = nums.back();
    } else if (op == "gs") {
      applyExtGState();
    } else if (op == "Tr" && !nums.empty()) {
      gs.renderMode = static_cast<int>(nums.back());
    } else if (op == "k" && nums.size() >= 4) {
      size_t n = nums.size();
      gs.fill[0] = nums[n - 4]; gs.fill[1] = nums[n - 3];
      gs.fill[2] = nums[n - 2]; gs.fill[3] = nums[n - 1];
      gs.fillCmyk = true;
    } else if (op == "K" && nums.size() >= 4) {
      size_t n = nums.size();
      gs.stroke[0] = nums[n - 4]; gs.stroke[1] = nums[n - 3];
      gs.stroke[2] = nums[n - 2]; gs.stroke[3] = nums[n - 1];
      gs.strokeCmyk = true;
    } else if (op == "rg" || op == "g" || op == "cs" || op == "sc" || op == "scn") {
      gs.fillCmyk = false;
    } else if (op == "RG" || op == "G" || op == "CS" || op == "SC" || op == "SCN") {
      gs.strokeCmyk = false;
    } else if (op == "S" || op == "s") {
      markHairline();
      markRich(gs.stroke, gs.strokeCmyk);
    } else if (op == "B" || op == "B*" || op == "b" || op == "b*") {
      markHairline();
      markRich(gs.stroke, gs.strokeCmyk);
      markRich(gs.fill, gs.fillCmyk);
    } else if (op == "f" || op == "F" || op == "f*") {
      markRich(gs.fill, gs.fillCmyk);
    } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
      int m = gs.renderMode;
      if (m == 3) {
        ++tally.invisibleText;
        tally.invisPages.insert(page);
      } else {
        if (m == 0 || m == 2 || m == 4 || m == 6) markRich(gs.fill, gs.fillCmyk);
        if (m == 1 || m == 2 || m == 5 || m == 6) {
          markRich(gs.stroke, gs.strokeCmyk);
          markHairline();
        }
      }
    } else if (op == "Do" && !lastName.empty()) {
      draws.push_back({lastName, gs});
    }
    nums.clear();
    lastName.clear();
  }

  void handleEOF() override {}
};

void scanContent(QPDFObjectHandle contents, QPDFObjectHandle res, const Gs& initial,
                 int page, int depth, Visited& seen, Tally& tally) {
  if (depth > 12) return;
  Scanner scan(res, tally, page, initial);
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
      int w = dict.getKey("/Width").isInteger()
                  ? static_cast<int>(dict.getKey("/Width").getIntValue()) : 0;
      int h = dict.getKey("/Height").isInteger()
                  ? static_cast<int>(dict.getKey("/Height").getIntValue()) : 0;
      if (w <= 0 || h <= 0) continue;
      double wpt = std::hypot(d.second.ctm.a, d.second.ctm.b);
      double hpt = std::hypot(d.second.ctm.c, d.second.ctm.d);
      if (wpt <= 0.01 || hpt <= 0.01) continue;
      double ppiX = w * 72.0 / wpt;
      double ppiY = h * 72.0 / hpt;
      double ppi = ppiX > ppiY ? ppiX : ppiY;
      int bpc = dict.getKey("/BitsPerComponent").isInteger()
                    ? static_cast<int>(dict.getKey("/BitsPerComponent").getIntValue()) : 8;
      bool mask = dict.getKey("/ImageMask").isBool() &&
                  dict.getKey("/ImageMask").getBoolValue();
      bool bitonal = mask || bpc == 1;
      double lo = bitonal ? kBitonalMinPpi : kContoneMinPpi;
      double hi = bitonal ? kBitonalMaxPpi : kContoneMaxPpi;
      if (ppi < lo) {
        ++tally.lowRes;
        tally.lowPages.insert(page);
        if (tally.minPpi == 0 || ppi < tally.minPpi) tally.minPpi = ppi;
      } else if (ppi > hi) {
        ++tally.highRes;
        tally.highPages.insert(page);
        if (ppi > tally.maxPpi) tally.maxPpi = ppi;
      }
    } else if (sub == "/Form") {
      if (!seen.enter(xo)) continue;
      Gs inner = d.second;
      QPDFObjectHandle mtx = dict.getKey("/Matrix");
      if (mtx.isArray() && mtx.getArrayNItems() == 6) {
        Mat m;
        m.a = numOf(mtx.getArrayItem(0), 1); m.b = numOf(mtx.getArrayItem(1), 0);
        m.c = numOf(mtx.getArrayItem(2), 0); m.d = numOf(mtx.getArrayItem(3), 1);
        m.e = numOf(mtx.getArrayItem(4), 0); m.f = numOf(mtx.getArrayItem(5), 0);
        inner.ctm = mul(m, inner.ctm);
      }
      QPDFObjectHandle sres = dict.getKey("/Resources");
      scanContent(xo, sres.isDictionary() ? sres : res, inner, page, depth + 1, seen,
                  tally);
    }
  }
}

std::string pageList(const std::set<int>& pages) {
  std::string out;
  int shown = 0;
  for (int p : pages) {
    if (shown == 8) {
      out += ", …";
      break;
    }
    if (shown) out += ", ";
    out += std::to_string(p);
    ++shown;
  }
  std::string label = pages.size() == 1 ? "page " : "pages ";
  return label + out;
}
}

void passAnalyze(Ctx& ctx) {
  Tally tally;
  try {
    QPDFPageDocumentHelper dh(ctx.pdf);
    std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
    int pageNum = 0;
    for (auto& ph : pages) {
      ++pageNum;
      QPDFObjectHandle res = ph.getAttribute("/Resources", true);
      Visited seen;
      Gs initial;
      scanContent(ph.getObjectHandle().getKey("/Contents"), res, initial, pageNum, 0,
                  seen, tally);
    }
  } catch (...) {
    return;
  }
  auto finding = [&](const std::string& code, const std::string& detail) {
    ctx.res.analysis.push_back({code, detail, false});
  };
  if (tally.hairlines) {
    finding("ANALYZE_HAIRLINE",
            std::to_string(tally.hairlines) + " stroke(s) thinner than 0.125 pt at their "
            "rendered size (" + pageList(tally.hairlinePages) + ")");
  }
  if (tally.richBlack) {
    finding("ANALYZE_RICH_BLACK",
            std::to_string(tally.richBlack) + " object(s) painted in rich black (CMYK "
            "black over 90% with additional C/M/Y ink) (" + pageList(tally.richPages) +
            ")");
  }
  if (tally.invisibleText) {
    finding("ANALYZE_INVISIBLE_TEXT",
            std::to_string(tally.invisibleText) + " text run(s) in rendering mode 3, "
            "invisible and not used for clipping (" + pageList(tally.invisPages) + ")");
  }
  if (tally.lowRes) {
    finding("ANALYZE_IMAGE_LOWRES",
            std::to_string(tally.lowRes) + " image(s) below the minimum analysis "
            "resolution at their placed size, 250 ppi contone or 550 ppi bitonal; "
            "lowest is " + std::to_string(static_cast<int>(tally.minPpi)) + " ppi (" +
            pageList(tally.lowPages) + ")");
  }
  if (tally.highRes) {
    finding("ANALYZE_IMAGE_HIGHRES",
            std::to_string(tally.highRes) + " image(s) above the maximum analysis "
            "resolution at their placed size, 450 ppi contone or 3600 ppi bitonal; "
            "highest is " + std::to_string(static_cast<int>(tally.maxPpi)) + " ppi (" +
            pageList(tally.highPages) + ")");
  }
}
}
