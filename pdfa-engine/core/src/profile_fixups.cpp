#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>
#include <qpdf/Pl_Buffer.hh>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ctx.hh"
#include "limits.hh"
#include "passes.hh"
#include "profile_types.hh"
#include "util.hh"

namespace pdfa {
extern const unsigned char kSrgbIcc[];
extern const unsigned int kSrgbIccLen;
extern const unsigned char kCmykIcc[];
extern const unsigned int kCmykIccLen;
namespace {
double unitPt(const std::string& u) {
  if (u == "mm") return 72.0 / 25.4;
  if (u == "cm") return 72.0 / 2.54;
  if (u == "in" || u == "inch") return 72.0;
  return 1.0;
}

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

struct FixupCtx {
  Ctx& ctx;
  std::vector<QPDFPageObjectHelper>& pages;
  const std::string& op;
  const std::vector<std::string>& params;
  std::string param(size_t i) const {
    return i < params.size() ? params[i] : std::string();
  }
  void note(const std::string& what) const { ctx.issue("PROFILE_FIX_DONE", what, true); }
};

bool applyPageGeometryFix(const FixupCtx& f) {
  if (f.op == "rotatepages") {
    int ang = std::atoi(f.param(0).c_str());
    if (ang % 90 == 0 && ang % 360 != 0) {
      for (auto& ph : f.pages) {
        QPDFObjectHandle page = ph.getObjectHandle();
        int cur = page.getKey("/Rotate").isInteger()
                      ? static_cast<int>(page.getKey("/Rotate").getIntValue()) : 0;
        page.replaceKey("/Rotate", QPDFObjectHandle::newInteger(((cur + ang) % 360 + 360) % 360));
      }
      f.note("rotated pages by " + std::to_string(ang) + " degrees");
    }
  } else if (f.op == "removepagescaling") {
    int n = 0;
    for (auto& ph : f.pages) {
      if (ph.getObjectHandle().hasKey("/UserUnit")) {
        ph.getObjectHandle().removeKey("/UserUnit");
        ++n;
      }
    }
    if (n) f.note("removed page scaling (/UserUnit) from " + std::to_string(n) + " page(s)");
  } else if (f.op == "scalepagesex") {
    double tw = std::atof(f.param(0).c_str()) * unitPt(f.param(2));
    double th = std::atof(f.param(1).c_str()) * unitPt(f.param(2));
    if (tw > 1 && th > 1) {
      for (auto& ph : f.pages) {
        QPDFObjectHandle mb = ph.getAttribute("/MediaBox", true);
        if (!mb.isArray() || mb.getArrayNItems() != 4) continue;
        double w = numOf(mb.getArrayItem(2), 0) - numOf(mb.getArrayItem(0), 0);
        double h = numOf(mb.getArrayItem(3), 0) - numOf(mb.getArrayItem(1), 0);
        if (w < 1 || h < 1) continue;
        double sc = std::min(tw / w, th / h);
        if (std::abs(sc - 1.0) < 0.001) continue;
        std::string scaleOp =
            "q " + fmtFixed(sc, 6) + " 0 0 " + fmtFixed(sc, 6) + " 0 0 cm\n";
        ph.addPageContents(QPDFObjectHandle::newStream(&f.ctx.pdf, scaleOp), true);
        ph.addPageContents(QPDFObjectHandle::newStream(&f.ctx.pdf, std::string("\nQ")), false);
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
      f.note("scaled pages to fit the requested size");
    }
  } else if (f.op == "setpagebox" || f.op == "setpageboxesbasedonmarks") {
    static const std::set<std::string> kPageBoxes = {"MediaBox", "CropBox", "TrimBox",
                                                     "BleedBox", "ArtBox"};
    std::string target = f.op == "setpagebox" && !f.param(0).empty() ? f.param(0) : "TrimBox";
    if (!kPageBoxes.count(target)) return true;
    bool onlyMissing = true;
    for (const std::string& prm : f.params) {
      if (prm == "Always") onlyMissing = false;
    }
    double u = unitPt(f.param(6));
    double o0 = std::atof(f.param(2).c_str()) * u, o1 = std::atof(f.param(3).c_str()) * u;
    double o2 = std::atof(f.param(4).c_str()) * u, o3 = std::atof(f.param(5).c_str()) * u;
    for (auto& ph : f.pages) {
      QPDFObjectHandle page = ph.getObjectHandle();
      if (onlyMissing && page.hasKey("/" + target)) continue;
      std::string refName = f.param(1).rfind("RelativeTo", 0) == 0 ? f.param(1).substr(10) : "CropBox";
      QPDFObjectHandle ref = boxOnPage(ph, refName);
      if (!ref.isArray() || ref.getArrayNItems() != 4) continue;
      QPDFObjectHandle nb = QPDFObjectHandle::newArray();
      nb.appendItem(QPDFObjectHandle::newReal(numOf(ref.getArrayItem(0), 0) + o0, 2));
      nb.appendItem(QPDFObjectHandle::newReal(numOf(ref.getArrayItem(1), 0) + o1, 2));
      nb.appendItem(QPDFObjectHandle::newReal(numOf(ref.getArrayItem(2), 0) - o2, 2));
      nb.appendItem(QPDFObjectHandle::newReal(numOf(ref.getArrayItem(3), 0) - o3, 2));
      page.replaceKey("/" + target, nb);
    }
    f.note("set " + target + " on pages");
  } else if (f.op == "generatebleed") {
    double amt = f.param(0) == "Auto" ? 9.0 : std::atof(f.param(1).c_str()) * unitPt(f.param(2));
    if (amt <= 0) amt = 9.0;
    for (auto& ph : f.pages) {
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
    f.note("generated bleed box from the trim box");
  } else if (f.op == "removeobjectsoutofbox") {
    std::string bx = f.param(0).empty() ? "MediaBox" : f.param(0);
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
  } else {
    return false;
  }
  return true;
}

bool applyDocumentInfoFix(const FixupCtx& f) {
  if (f.op == "removepdfuakeys") {
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
      while ((pos = xmp.find("<pdfuaid:", pos)) != std::string::npos) {
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
      while ((pos = xmp.find("pdfuaid:", pos)) != std::string::npos) {
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
          pos += 8;
        }
      }
      if (xmp != before) {
        meta.replaceStreamData(xmp, QPDFObjectHandle::newNull(),
                               QPDFObjectHandle::newNull());
        f.note("removed the PDF/UA marker from the document metadata");
      }
    }
  } else if (f.op == "settitle") {
    QPDFObjectHandle info = f.ctx.pdf.getTrailer().getKey("/Info");
    if (!info.isDictionary()) {
      info = QPDFObjectHandle::newDictionary();
      f.ctx.pdf.getTrailer().replaceKey("/Info", f.ctx.pdf.makeIndirectObject(info));
    }
    bool ifEmpty = f.param(0) == "IfEmpty";
    std::string title = f.param(1);
    if (!(ifEmpty && info.getKey("/Title").isString() &&
          !info.getKey("/Title").getUTF8Value().empty()) && !title.empty()) {
      info.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(title));
      f.note("set the document title");
    }
  } else if (f.op == "trappedkey") {
    QPDFObjectHandle info = f.ctx.pdf.getTrailer().getKey("/Info");
    if (info.isDictionary()) {
      info.replaceKey("/Trapped", QPDFObjectHandle::newName(
          lowerAscii(f.param(0)) == "true" ? "/True" : "/False"));
      f.note("set the trapped flag");
    }
  } else if (f.op == "setinitialviewdocumentoptions") {
    QPDFObjectHandle root = f.ctx.pdf.getRoot();
    static const std::set<std::string> modes = {"UseNone", "UseOutlines", "UseThumbs",
                                                "FullScreen", "UseOC", "UseAttachments"};
    static const std::set<std::string> layouts = {"SinglePage", "OneColumn",
                                                  "TwoColumnLeft", "TwoColumnRight",
                                                  "TwoPageLeft", "TwoPageRight"};
    for (const std::string& prm : f.params) {
      if (modes.count(prm)) root.replaceKey("/PageMode", QPDFObjectHandle::newName("/" + prm));
      if (layouts.count(prm)) root.replaceKey("/PageLayout", QPDFObjectHandle::newName("/" + prm));
    }
    f.note("set initial view document options");
  } else if (f.op == "setinitialviewuioptions" || f.op == "setinitialviewwindowoptions") {
    QPDFObjectHandle root = f.ctx.pdf.getRoot();
    QPDFObjectHandle vp = root.getKey("/ViewerPreferences");
    if (!vp.isDictionary()) {
      vp = QPDFObjectHandle::newDictionary();
      root.replaceKey("/ViewerPreferences", vp);
    }
    const char* uiKeys[] = {"/HideToolbar", "/HideMenubar", "/HideWindowUI"};
    const char* winKeys[] = {"/FitWindow", "/CenterWindow", "/DisplayDocTitle"};
    const char** keys = f.op == "setinitialviewuioptions" ? uiKeys : winKeys;
    for (size_t i = 0; i < 3 && i < f.params.size(); ++i) {
      std::string v = lowerAscii(f.params[i]);
      if (v == "true" || v == "false") {
        vp.replaceKey(keys[i], QPDFObjectHandle::newBool(v == "true"));
      }
    }
    f.note("set initial view preferences");
  } else {
    return false;
  }
  return true;
}

bool applyGraphicsStateFix(const FixupCtx& f) {
  if (f.op == "settransparencyblendcs") {
    std::string profName = f.param(0);
    bool cmyk = profName.find("CMYK") != std::string::npos ||
                profName.find("Coated") != std::string::npos ||
                profName.find("SWOP") != std::string::npos ||
                profName.find("FOGRA") != std::string::npos;
    QPDFObjectHandle icc =
        buildIccStream(f.ctx, cmyk ? kCmykIcc : kSrgbIcc,
                       cmyk ? kCmykIccLen : kSrgbIccLen, cmyk ? 4 : 3);
    QPDFObjectHandle cs = QPDFObjectHandle::newArray();
    cs.appendItem(QPDFObjectHandle::newName("/ICCBased"));
    cs.appendItem(icc);
    QPDFObjectHandle csRef = f.ctx.pdf.makeIndirectObject(cs);
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
           " page(s) to " + (cmyk ? "a CMYK press profile" : "sRGB"));
    }
  } else if (f.op == "modifyinterpolateentry") {
    bool remove = f.param(0) == "Remove";
    int n = 0;
    for (QPDFObjectHandle obj : f.ctx.pdf.getAllObjects()) {
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
    if (n) f.note((remove ? "removed" : "set") + std::string(" interpolation on ") +
                std::to_string(n) + " image(s)");
  } else if (f.op == "removeflatness") {
    scrubExtGStates(f.ctx, {"/FL"});
    f.note("removed flatness overrides");
  } else if (f.op == "removesmoothness") {
    scrubExtGStates(f.ctx, {"/SM"});
    f.note("removed smoothness overrides");
  } else if (f.op == "transfercurves") {
    scrubExtGStates(f.ctx, {"/TR", "/TR2"});
    f.note("removed transfer curves");
  } else if (f.op == "removebg") {
    scrubExtGStates(f.ctx, {"/BG", "/BG2"});
    f.note("removed black generation overrides");
  } else if (f.op == "removeucr") {
    scrubExtGStates(f.ctx, {"/UCR", "/UCR2"});
    f.note("removed undercolour removal overrides");
  } else if (f.op == "removerenderingintents") {
    scrubExtGStates(f.ctx, {"/RI"});
    for (QPDFObjectHandle obj : f.ctx.pdf.getAllObjects()) {
      if (obj.isStream() && nameIs(obj.getDict().getKey("/Subtype"), "/Image") &&
          obj.getDict().hasKey("/Intent")) {
        obj.getDict().removeKey("/Intent");
      }
    }
    f.note("removed rendering intents");
  } else if (f.op == "setrenderingintent") {
    std::string in = f.param(0).empty() ? "RelativeColorimetric" : f.param(0);
    for (QPDFObjectHandle obj : f.ctx.pdf.getAllObjects()) {
      if (obj.isDictionary() && nameIs(obj.getKey("/Type"), "/ExtGState")) {
        obj.replaceKey("/RI", QPDFObjectHandle::newName("/" + in));
      }
    }
    f.note("set rendering intent to " + in);
  } else if (f.op == "removeunnecessarytransparencygroups") {
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
  } else {
    return false;
  }
  return true;
}

bool applyColorantFix(const FixupCtx& f) {
  if (f.op == "mergespotcolornames" || f.op == "makecustomspotcolornamesconsistent" ||
             f.op == "mksptclrappcnsistent" || f.op == "mapspotcolors" ||
             f.op == "convertregistrationcolortoblack") {
    std::map<std::string, std::string> canon;
    std::string mapFrom, mapTo;
    if (f.op == "mapspotcolors" && f.params.size() >= 3) {
      mapFrom = f.param(2);
      mapTo = f.param(0);
    }
    int renamed = 0;
    for (QPDFObjectHandle obj : f.ctx.pdf.getAllObjects()) {
      if (!obj.isArray() || obj.getArrayNItems() < 2) continue;
      std::string fam = nameOf(obj.getArrayItem(0));
      if (fam != "/Separation" && fam != "/DeviceN") continue;
      auto renameName = [&](QPDFObjectHandle holder, int idx) {
        QPDFObjectHandle nm = holder.getArrayItem(idx);
        if (!nm.isName() || nm.getName().size() < 2) return;
        std::string name = nm.getName().substr(1);
        std::string newName = name;
        if (f.op == "convertregistrationcolortoblack") {
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
    if (renamed) f.note("unified " + std::to_string(renamed) + " spot colourant name(s)");
  } else if (f.op == "convertnchtodevn") {
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
  } else {
    return false;
  }
  return true;
}

bool applyContentFix(const FixupCtx& f) {
  if (f.op == "placetext") {
    std::string text = f.param(0) == "Date" ? (f.ctx.opt.nowOverride.empty() ? "D:converted"
                                                                      : f.ctx.opt.nowOverride)
                                      : f.param(0);
    double size = std::atof(f.param(2).c_str());
    if (size <= 0) size = 12;
    for (auto& ph : f.pages) {
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
      fonts.replaceKey("/KuraStampF", f.ctx.pdf.makeIndirectObject(helv));
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
      ph.addPageContents(QPDFObjectHandle::newStream(&f.ctx.pdf, std::string(buf)), false);
    }
    f.note("placed text on pages");
  } else if (f.op == "annotation") {
    std::string sel = f.param(0), act = f.param(1);
    int n = 0;
    static const std::set<std::string> multimedia = {"Screen", "Movie", "Sound",
                                                     "RichMedia", "3D"};
    static const std::set<std::string> known = {
        "Text", "Link", "FreeText", "Line", "Square", "Circle", "Polygon", "PolyLine",
        "Highlight", "Underline", "Squiggly", "StrikeOut", "Stamp", "Caret", "Ink",
        "Popup", "FileAttachment", "Sound", "Movie", "Widget", "Screen", "PrinterMark",
        "TrapNet", "Watermark", "3D", "Redact", "Projection", "RichMedia"};
    for (auto& ph : f.pages) {
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
    if (n) f.note("annotation fix (" + act + "): adjusted " + std::to_string(n) +
                " annotation(s)");
  } else if (f.op == "putobjectsonlayer" || f.op == "putobjpsteps") {
    std::string label = f.param(1).empty() ? (f.param(0).empty() ? "Layer" : f.param(0)) : f.param(1);
    QPDFObjectHandle ocg = QPDFObjectHandle::newDictionary();
    ocg.replaceKey("/Type", QPDFObjectHandle::newName("/OCG"));
    ocg.replaceKey("/Name", QPDFObjectHandle::newUnicodeString(label));
    if (f.op == "putobjpsteps") {
      QPDFObjectHandle md = QPDFObjectHandle::newDictionary();
      md.replaceKey("/Type", QPDFObjectHandle::newName("/GTS_ProcSteps"));
      md.replaceKey("/GTS_ProcStepsGroup", QPDFObjectHandle::newName("/" + label));
      ocg.replaceKey("/GTS_Metadata", md);
    }
    QPDFObjectHandle ocgRef = f.ctx.pdf.makeIndirectObject(ocg);
    QPDFObjectHandle root = f.ctx.pdf.getRoot();
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
    for (auto& ph : f.pages) {
      QPDFObjectHandle res = ph.getAttribute("/Resources", true);
      if (!res.isDictionary()) continue;
      QPDFObjectHandle props = res.getKey("/Properties");
      if (!props.isDictionary()) {
        props = QPDFObjectHandle::newDictionary();
        res.replaceKey("/Properties", props);
      }
      props.replaceKey("/KuraOC1", ocgRef);
      ph.addPageContents(
          QPDFObjectHandle::newStream(&f.ctx.pdf, std::string("/OC /KuraOC1 BDC\n")), true);
      ph.addPageContents(QPDFObjectHandle::newStream(&f.ctx.pdf, std::string("\nEMC")),
                         false);
    }
    f.note("placed page content on layer " + label);
  } else if (f.op == "dscdhdnlycntfltnvsblyrs") {
    QPDFObjectHandle root = f.ctx.pdf.getRoot();
    QPDFObjectHandle ocp = root.getKey("/OCProperties");
    if (ocp.isDictionary()) {
      std::set<QPDFObjGen> hiddenOcgs;
      QPDFObjectHandle dflt = ocp.getKey("/D");
      if (dflt.isDictionary()) {
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
  } else {
    return false;
  }
  return true;
}

void passProfileFixups(Ctx& ctx) {
  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
  bool knockWhite = false, opBlack = false, textOnly = false, vectorOnly = false;
  double minWidth = 0;
  int forceTr = -1;
  for (const auto& fixOp : ctx.opt.profileFixOps) {
    const std::string& op = fixOp.first;
    const std::vector<std::string>& params = fixOp.second;
    auto p = [&params](size_t i) { return i < params.size() ? params[i] : std::string(); };
    FixupCtx f{ctx, pages, op, params};
    if (applyPageGeometryFix(f) || applyDocumentInfoFix(f) || applyGraphicsStateFix(f) ||
        applyColorantFix(f) || applyContentFix(f)) {
      continue;
    }
    if (op == "knockoutwhite" || op == "overprintblack" ||
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
      QPDFObjectHandle pageContents = ph.getObjectHandle().getKey("/Contents");
      if (!pageContents.isStream() && !pageContents.isArray()) continue;
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

