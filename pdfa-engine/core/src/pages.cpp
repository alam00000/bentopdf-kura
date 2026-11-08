#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFAnnotationObjectHelper.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ctx.hh"
#include "images.hh"
#include "passes.hh"
#include "limits.hh"
#include "util.hh"

namespace pdfa {
namespace {
const std::set<std::string> kPdf14Annots = {
    "/Text", "/Link", "/FreeText", "/Line", "/Square", "/Circle", "/Highlight",
    "/Underline", "/Squiggly", "/StrikeOut", "/Stamp", "/Ink", "/Popup",
    "/Widget", "/PrinterMark", "/TrapNet"};

const std::set<std::string> kPdf17ExtraAnnots = {
    "/Polygon", "/PolyLine", "/Caret", "/Watermark", "/Redact", "/FileAttachment"};

const std::set<std::string> kValidIntents = {
    "/RelativeColorimetric", "/AbsoluteColorimetric", "/Perceptual", "/Saturation"};

thread_local const GraphicsUse* tPageUse = nullptr;

bool unusedHere(QPDFObjectHandle o) {
  return tPageUse && !tPageUse->has(o);
}

struct TransparencyReport {
  bool real = false;
  std::vector<std::string> where;
  std::vector<bool> pages;
};

std::set<std::string> filterNames(QPDFObjectHandle d) {
  std::set<std::string> out;
  QPDFObjectHandle f = d.getKey("/Filter");
  if (f.isName()) out.insert(f.getName());
  for (int i = 0; f.isArray() && i < f.getArrayNItems(); ++i) out.insert(nameOf(f.getArrayItem(i)));
  return out;
}

bool gsHasTransparency(QPDFObjectHandle gs) {
  if (!gs.isDictionary()) return false;
  QPDFObjectHandle sm = gs.getKey("/SMask");
  if (!sm.isNull() && !nameIs(sm, "/None")) return true;
  std::string bm = nameOf(gs.getKey("/BM"));
  if (gs.getKey("/BM").isArray() && gs.getKey("/BM").getArrayNItems() > 0) {
    bm = nameOf(gs.getKey("/BM").getArrayItem(0));
  }
  if (!bm.empty() && bm != "/Normal" && bm != "/Compatible") return true;
  if (numOf(gs.getKey("/CA"), 1.0) < 1.0 - 1e-6) return true;
  if (numOf(gs.getKey("/ca"), 1.0) < 1.0 - 1e-6) return true;
  return false;
}

void scanResources(QPDFObjectHandle res, Visited& visited, TransparencyReport& rep) {
  DepthGuard g_(visited);
  if (g_.over) return;
  if (!res.isDictionary() || !visited.enter(res)) return;
  QPDFObjectHandle gsd = res.getKey("/ExtGState");
  if (gsd.isDictionary()) {
    for (const std::string& k : gsd.getKeys()) {
      if (unusedHere(gsd.getKey(k))) continue;
      if (gsHasTransparency(gsd.getKey(k))) {
        rep.real = true;
        rep.where.push_back("ExtGState" + k);
      }
    }
  }
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (xod.isDictionary()) {
    for (const std::string& k : xod.getKeys()) {
      QPDFObjectHandle xo = xod.getKey(k);
      if (!xo.isStream() || unusedHere(xo) || !visited.enter(xo)) continue;
      QPDFObjectHandle d = xo.getDict();
      std::string subtype = nameOf(d.getKey("/Subtype"));
      if (subtype == "/Form") {
        QPDFObjectHandle grp = d.getKey("/Group");
        if (grp.isDictionary() && nameIs(grp.getKey("/S"), "/Transparency")) {
          rep.real = true;
          rep.where.push_back("XObject" + k + " transparency group");
        }
        scanResources(d.getKey("/Resources"), visited, rep);
      } else if (subtype == "/Image") {
        QPDFObjectHandle sm = d.getKey("/SMask");
        if (!sm.isNull() && !nameIs(sm, "/None")) {
          rep.real = true;
          rep.where.push_back("XObject" + k + " /SMask");
        }
        if (numOf(d.getKey("/SMaskInData"), 0) > 0 && filterNames(d).count("/JPXDecode")) {
          rep.real = true;
          rep.where.push_back("XObject" + k + " /SMaskInData");
        }
      }
    }
  }
  QPDFObjectHandle pat = res.getKey("/Pattern");
  if (pat.isDictionary()) {
    for (const std::string& k : pat.getKeys()) {
      QPDFObjectHandle p = pat.getKey(k);
      if (unusedHere(p) || !visited.enter(p)) continue;
      QPDFObjectHandle pd = p.isStream() ? p.getDict() : p;
      if (!pd.isDictionary()) continue;
      if (!tPageUse && gsHasTransparency(pd.getKey("/ExtGState"))) {
        rep.real = true;
        rep.where.push_back("Pattern" + k + " /ExtGState");
      }
      scanResources(pd.getKey("/Resources"), visited, rep);
    }
  }
  QPDFObjectHandle fontd = res.getKey("/Font");
  for (const std::string& k : fontd.isDictionary() ? fontd.getKeys() : std::set<std::string>()) {
    QPDFObjectHandle f = fontd.getKey(k);
    if (!f.isDictionary() || unusedHere(f) || !nameIs(f.getKey("/Subtype"), "/Type3") || !visited.enter(f)) continue;
    scanResources(f.getKey("/Resources"), visited, rep);
    QPDFObjectHandle procs = f.getKey("/CharProcs");
    for (const std::string& g : procs.isDictionary() ? procs.getKeys() : std::set<std::string>()) {
      if (procs.getKey(g).isStream()) scanResources(procs.getKey(g).getDict().getKey("/Resources"), visited, rep);
    }
  }
}

TransparencyReport scanTransparency(Ctx&, std::vector<QPDFPageObjectHelper>& pages);

}

bool pageHasTransparency(Ctx& ctx, QPDFPageObjectHelper& ph) {
  std::vector<QPDFPageObjectHelper> one{ph};
  return scanTransparency(ctx, one).real;
}

namespace {
TransparencyReport scanTransparency(Ctx&, std::vector<QPDFPageObjectHelper>& pages) {
  TransparencyReport rep;
  for (auto& page : pages) {
    Visited visited;
    size_t before = rep.where.size();
    bool hadReal = rep.real;
    rep.real = false;
    scanResources(page.getAttribute("/Resources", false), visited, rep);
    QPDFObjectHandle annots = page.getObjectHandle().getKey("/Annots");
    if (annots.isArray()) {
      for (int i = 0; i < annots.getArrayNItems(); ++i) {
        QPDFObjectHandle a = annots.getArrayItem(i);
        if (!a.isDictionary()) continue;
        QPDFObjectHandle ap = a.getKey("/AP");
        if (ap.isDictionary()) {
          for (const std::string& k : ap.getKeys()) {
            QPDFObjectHandle s = ap.getKey(k);
            if (s.isStream() && visited.enter(s)) {
              scanResources(s.getDict().getKey("/Resources"), visited, rep);
            }
          }
        }
      }
    }
    rep.pages.push_back(rep.real || rep.where.size() > before);
    rep.real = rep.real || hadReal;
  }
  return rep;
}

bool bakeTransparentAppearances(Ctx& ctx, QPDFPageObjectHelper& ph) {
  QPDFObjectHandle page = ph.getObjectHandle();
  QPDFObjectHandle annots = page.getKey("/Annots");
  if (!annots.isArray()) return false;
  int rotate = page.getKey("/Rotate").isInteger() ? static_cast<int>(page.getKey("/Rotate").getIntValue()) : 0;
  std::string content;
  int baked = 0;
  std::set<QPDFObjGen> drop;
  for (int i = 0; i < annots.getArrayNItems(); ++i) {
    QPDFObjectHandle a = annots.getArrayItem(i);
    if (!a.isDictionary() || !a.isIndirect() || nameIs(a.getKey("/Subtype"), "/Widget") || nameIs(a.getKey("/Subtype"), "/Popup")) continue;
    QPDFAnnotationObjectHelper aoh(a);
    QPDFObjectHandle as = aoh.getAppearanceStream("/N");
    if (!as.isStream()) continue;
    Visited v;
    TransparencyReport r;
    QPDFObjectHandle grp = as.getDict().getKey("/Group");
    r.real = grp.isDictionary() && nameIs(grp.getKey("/S"), "/Transparency");
    scanResources(as.getDict().getKey("/Resources"), v, r);
    if (!r.real && numOf(a.getKey("/CA"), 1.0) >= 1.0 - 1e-6) continue;
    QPDFObjectHandle res = ph.getAttribute("/Resources", true);
    if (!res.isDictionary()) continue;
    if (!res.getKey("/XObject").isDictionary()) res.replaceKey("/XObject", QPDFObjectHandle::newDictionary());
    std::string name = res.getUniqueResourceName("/KuraAp", baked);
    std::string part = aoh.getPageContentForAppearance(name, rotate, 0, an_invisible | an_hidden | an_no_view);
    if (part.empty()) continue;
    if (!as.isIndirect()) as = ctx.pdf.makeIndirectObject(as);
    res.getKey("/XObject").replaceKey(name, as);
    ctx.keepWholeForms.insert(as.getObjGen());
    double ca = numOf(a.getKey("/CA"), 1.0);
    if (ca < 1.0 - 1e-6) {
      if (!res.getKey("/ExtGState").isDictionary()) res.replaceKey("/ExtGState", QPDFObjectHandle::newDictionary());
      std::string gsName = res.getUniqueResourceName("/KuraCa", baked);
      QPDFObjectHandle gs = QPDFObjectHandle::newDictionary();
      gs.replaceKey("/CA", QPDFObjectHandle::newReal(ca, 4));
      gs.replaceKey("/ca", QPDFObjectHandle::newReal(ca, 4));
      res.getKey("/ExtGState").replaceKey(gsName, gs);
      part = "q " + gsName + " gs\n" + part + "Q\n";
    }
    content += part;
    drop.insert(a.getObjGen());
    if (a.getKey("/Popup").isIndirect()) drop.insert(a.getKey("/Popup").getObjGen());
    ++baked;
  }
  if (content.empty()) return false;
  QPDFObjectHandle kept = QPDFObjectHandle::newArray();
  for (int i = 0; i < annots.getArrayNItems(); ++i) {
    QPDFObjectHandle a = annots.getArrayItem(i);
    if (!(a.isIndirect() && drop.count(a.getObjGen()))) kept.appendItem(a);
  }
  page.replaceKey("/Annots", kept);
  ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, "q\n"), true);
  ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, "\nQ\n" + content), false);
  ctx.redrawnPages.insert(page.getObjGen());
  ctx.issue("ANNOTATION_APPEARANCE_BAKED",
            "drew " + std::to_string(baked) + " transparent annotation appearance(s) into the page so the "
            "transparency could be flattened, and removed those annotations",
            true);
  return true;
}
}

namespace {
class PointPropertyScanner : public QPDFObjectHandle::ParserCallbacks {
 public:
  void handleObject(QPDFObjectHandle obj, size_t, size_t) override {
    if (obj.isOperator()) {
      if (obj.getOperatorValue() == "DP" && !lastName.empty()) names.insert(lastName);
      lastName.clear();
      return;
    }
    lastName = obj.isName() ? obj.getName() : std::string();
  }
  void handleEOF() override {}
  std::set<std::string> names;

 private:
  std::string lastName;
};

void moveMarkedContentAf(Ctx& ctx, QPDFPageObjectHelper& ph) {
  QPDFObjectHandle page = ph.getObjectHandle();
  QPDFObjectHandle props = page.getKey("/Resources").isDictionary()
                               ? page.getKey("/Resources").getKey("/Properties")
                               : QPDFObjectHandle::newNull();
  if (!props.isDictionary()) return;
  PointPropertyScanner scan;
  try {
    ph.parseContents(&scan);
  } catch (...) {
    return;
  }
  int moved = 0;
  for (const std::string& n : scan.names) {
    QPDFObjectHandle pl = props.getKey(n);
    if (!pl.isDictionary() || !pl.hasKey("/AF")) continue;
    QPDFObjectHandle af = pl.getKey("/AF");
    QPDFObjectHandle pageAf = page.getKey("/AF");
    if (!pageAf.isArray()) {
      pageAf = QPDFObjectHandle::newArray();
      page.replaceKey("/AF", pageAf);
    }
    if (af.isArray()) {
      for (int i = 0; i < af.getArrayNItems(); ++i) pageAf.appendItem(af.getArrayItem(i));
    } else if (af.isDictionary()) {
      pageAf.appendItem(af);
    }
    pl.removeKey("/AF");
    ++moved;
  }
  if (moved) {
    ctx.issue("MARKED_CONTENT_AF_MOVED",
              "moved associated files from " + std::to_string(moved) +
                  " marked-content point(s) to the page, since only marked-content sequences may carry them",
              true);
  }
}

void applyUserUnit(Ctx& ctx, QPDFPageObjectHelper& ph) {
  QPDFObjectHandle page = ph.getObjectHandle();
  double u = page.getKey("/UserUnit").getNumericValue();
  page.removeKey("/UserUnit");
  if (!(u > 0) || std::fabs(u - 1.0) < 1e-9) return;
  auto scale = [u](QPDFObjectHandle b) {
    if (!b.isArray()) return;
    for (int i = 0; i < b.getArrayNItems(); ++i) {
      if (b.getArrayItem(i).isNumber()) b.setArrayItem(i, QPDFObjectHandle::newReal(b.getArrayItem(i).getNumericValue() * u, 6));
    }
  };
  for (const char* k : {"/MediaBox", "/CropBox"}) scale(ph.getAttribute(k, true));
  for (const char* k : {"/TrimBox", "/BleedBox", "/ArtBox"}) scale(page.getKey(k));
  QPDFObjectHandle annots = page.getKey("/Annots");
  for (int i = 0; annots.isArray() && i < annots.getArrayNItems(); ++i) {
    if (annots.getArrayItem(i).isDictionary()) scale(annots.getArrayItem(i).getKey("/Rect"));
  }
  std::string cm = std::to_string(u) + " 0 0 " + std::to_string(u) + " 0 0 cm\n";
  ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, cm), true);
  ctx.issue("USER_UNIT_APPLIED", "applied a page scale factor to the page geometry and content, since PDF 1.4 has no user unit",
            !(ctx.opt.verifyOnly && ctx.isA()));
}
}

namespace {
thread_local int gPlaceholderForms = 0;
thread_local int gPlaceholderPatterns = 0;
thread_local std::map<std::string, QPDFObjectHandle>* gPlaceholders = nullptr;
class ResourceNeedScanner : public QPDFObjectHandle::ParserCallbacks {
 public:
  void handleObject(QPDFObjectHandle obj, size_t, size_t) override {
    if (obj.isOperator()) {
      std::string op = obj.getOperatorValue();
      if (!lastName.empty()) {
        if (op == "Tf") need["/Font"].insert(lastName);
        else if (op == "Do") need["/XObject"].insert(lastName);
        else if (op == "gs") need["/ExtGState"].insert(lastName);
        else if (op == "sh") need["/Shading"].insert(lastName);
        else if (op == "cs" || op == "CS") need["/ColorSpace"].insert(lastName);
        else if (op == "scn" || op == "SCN") need["/Pattern"].insert(lastName);
        else if (op == "BDC" || op == "DP") need["/Properties"].insert(lastName);
      }
      lastName.clear();
      return;
    }
    if (obj.isName()) lastName = obj.getName();
  }
  void handleEOF() override {}
  std::map<std::string, std::set<std::string>> need;

 private:
  std::string lastName;
};

void completeStreamResources(Ctx& ctx, QPDFObjectHandle holder, QPDFObjectHandle parentRes,
                             Visited& visited, int& fixedEntries, int depth = 0) {
  if (depth > kMaxResourceNest) return;
  QPDFObjectHandle d = holder.isStream() ? holder.getDict() : holder;
  bool isPage = !holder.isStream();
  QPDFObjectHandle own = d.getKey("/Resources");
  QPDFObjectHandle eff = own.isDictionary() ? own : parentRes;

  ResourceNeedScanner scan;
  try {
    QPDFObjectHandle::parseContentStream(holder, &scan);
  } catch (...) {
    return;
  }
  static const std::set<std::string> kStdCs = {"/DeviceRGB", "/DeviceCMYK", "/DeviceGray",
                                               "/Pattern"};
  bool anyNeeded = false;
  for (auto& kv : scan.need) {
    for (const std::string& n : kv.second) {
      if (kv.first == "/ColorSpace" && kStdCs.count(n)) continue;
      anyNeeded = true;
    }
  }
  if (anyNeeded && !own.isDictionary()) {
    own = QPDFObjectHandle::newDictionary();
    d.replaceKey("/Resources", own);
  }
  if (own.isDictionary()) {
    for (auto& kv : scan.need) {
      const std::string& cat = kv.first;
      for (const std::string& n : kv.second) {
        if (cat == "/ColorSpace" && kStdCs.count(n)) continue;
        QPDFObjectHandle ownCat = own.getKey(cat);
        if (ownCat.isDictionary() && ownCat.hasKey(n)) continue;
        QPDFObjectHandle src = parentRes;
        QPDFObjectHandle val;
        if (src.isDictionary() && src.getKey(cat).isDictionary() &&
            src.getKey(cat).hasKey(n)) {
          val = src.getKey(cat).getKey(n);
        }
        if (!val.isInitialized() && isPage && eff.isDictionary() &&
            eff.getKey(cat).isDictionary() && eff.getKey(cat).hasKey(n)) {
          val = eff.getKey(cat).getKey(n);
        }
        if (!val.isInitialized() && gPlaceholders && gPlaceholders->count(cat + n)) {
          val = (*gPlaceholders)[cat + n];
        }
        if (!val.isInitialized() && cat == "/XObject" && (ctx.isX() || ctx.isE())) {
          val = ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, ""));
          val.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
          val.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
          val.getDict().replaceKey("/BBox", QPDFObjectHandle::parse("[0 0 0 0]"));
          ++gPlaceholderForms;
          if (gPlaceholders) (*gPlaceholders)[cat + n] = val;
        }
        if (!val.isInitialized() && cat == "/Pattern" && (ctx.isX() || ctx.isE())) {
          val = ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, ""));
          val.getDict().replaceKey("/PatternType", QPDFObjectHandle::newInteger(1));
          val.getDict().replaceKey("/PaintType", QPDFObjectHandle::newInteger(1));
          val.getDict().replaceKey("/TilingType", QPDFObjectHandle::newInteger(1));
          val.getDict().replaceKey("/BBox", QPDFObjectHandle::parse("[0 0 1 1]"));
          val.getDict().replaceKey("/XStep", QPDFObjectHandle::newInteger(1));
          val.getDict().replaceKey("/YStep", QPDFObjectHandle::newInteger(1));
          val.getDict().replaceKey("/Resources", QPDFObjectHandle::newDictionary());
          ++gPlaceholderPatterns;
          if (gPlaceholders) (*gPlaceholders)[cat + n] = val;
        }
        if (!val.isInitialized()) continue;
        if (!ownCat.isDictionary()) {
          ownCat = QPDFObjectHandle::newDictionary();
          own.replaceKey(cat, ownCat);
        }
        ownCat.replaceKey(n, val);
        ++fixedEntries;
      }
    }
  }
  QPDFObjectHandle effNow = d.getKey("/Resources").isDictionary() ? d.getKey("/Resources")
                                                                  : parentRes;
  if (effNow.isDictionary()) {
    QPDFObjectHandle fonts = effNow.getKey("/Font");
    if (fonts.isDictionary()) {
      for (const std::string& fk : fonts.getKeys()) {
        QPDFObjectHandle fnt = fonts.getKey(fk);
        if (!fnt.isDictionary() || !nameIs(fnt.getKey("/Subtype"), "/Type3")) continue;
        QPDFObjectHandle cp = fnt.getKey("/CharProcs");
        if (!cp.isDictionary() || !visited.enter(fnt)) continue;
        ResourceNeedScanner t3scan;
        for (const std::string& g : cp.getKeys()) {
          if (!cp.getKey(g).isStream()) continue;
          try {
            QPDFObjectHandle::parseContentStream(cp.getKey(g), &t3scan);
          } catch (...) {
            ctx.scanIncomplete("a Type 3 glyph procedure");
          }
        }
        QPDFObjectHandle fres = fnt.getKey("/Resources");
        for (auto& kv : t3scan.need) {
          const std::string& cat = kv.first;
          for (const std::string& n : kv.second) {
            if (cat == "/ColorSpace" && kStdCs.count(n)) continue;
            if (fres.isDictionary() && fres.getKey(cat).isDictionary() &&
                fres.getKey(cat).hasKey(n)) {
              continue;
            }
            if (!effNow.getKey(cat).isDictionary() || !effNow.getKey(cat).hasKey(n)) {
              continue;
            }
            if (!fres.isDictionary()) {
              fnt.replaceKey("/Resources", QPDFObjectHandle::newDictionary());
              fres = fnt.getKey("/Resources");
            }
            QPDFObjectHandle fcat = fres.getKey(cat);
            if (!fcat.isDictionary()) {
              fres.replaceKey(cat, QPDFObjectHandle::newDictionary());
              fcat = fres.getKey(cat);
            }
            fcat.replaceKey(n, effNow.getKey(cat).getKey(n));
            ++fixedEntries;
          }
        }
      }
    }
    QPDFObjectHandle xod = effNow.getKey("/XObject");
    if (xod.isDictionary()) {
      for (const std::string& k : xod.getKeys()) {
        QPDFObjectHandle xo = xod.getKey(k);
        if (xo.isStream() && nameIs(xo.getDict().getKey("/Subtype"), "/Form") &&
            visited.enter(xo)) {
          completeStreamResources(ctx, xo, effNow, visited, fixedEntries, depth + 1);
        }
      }
    }
    QPDFObjectHandle pat = effNow.getKey("/Pattern");
    if (pat.isDictionary()) {
      for (const std::string& k : pat.getKeys()) {
        QPDFObjectHandle po = pat.getKey(k);
        if (po.isStream() && visited.enter(po)) {
          completeStreamResources(ctx, po, effNow, visited, fixedEntries, depth + 1);
        }
      }
    }
  }
}
}

void passCompleteResources(Ctx& ctx) {
  if (!(ctx.isA() && ctx.part >= 2) && !ctx.pdf20Print() && !ctx.isX() && !ctx.isE()) return;
  completeResources(ctx);
}

void completeResources(Ctx& ctx) {
  gPlaceholderForms = 0;
  gPlaceholderPatterns = 0;
  std::map<std::string, QPDFObjectHandle> placeholders;
  gPlaceholders = &placeholders;
  QPDFPageDocumentHelper dh(ctx.pdf);
  Visited visited;
  int fixedEntries = 0;
  int kuraPage1 = 0;
  for (auto& ph : dh.getAllPages()) {
    PageScope kuraScope1(ctx, ++kuraPage1);
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle eff = ph.getAttribute("/Resources", true);
    if (!page.getKey("/Resources").isDictionary() && eff.isDictionary()) {
      page.replaceKey("/Resources", eff);
      ++fixedEntries;
    }
    completeStreamResources(ctx, page, eff, visited, fixedEntries);
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (annots.isArray()) {
      for (int i = 0; i < annots.getArrayNItems(); ++i) {
        QPDFObjectHandle a = annots.getArrayItem(i);
        if (!a.isDictionary()) continue;
        std::vector<QPDFObjectHandle> streams = normalAppearanceStreams(a);
        for (QPDFObjectHandle& st : streams) {
          if (visited.enter(st)) {
            completeStreamResources(ctx, st, eff, visited, fixedEntries);
          }
        }
      }
    }
  }
  gPlaceholders = nullptr;
  if (gPlaceholderForms) {
    ctx.issue("XOBJECT_PLACEHOLDER_ADDED",
              std::to_string(gPlaceholderForms) + " drawing reference(s) named an XObject that "
              "exists nowhere in the file; added empty forms in their place", true);
  }
  if (gPlaceholderPatterns) {
    ctx.issue("PATTERN_PLACEHOLDER_ADDED",
              std::to_string(gPlaceholderPatterns) + " paint reference(s) named a pattern that "
              "exists nowhere in the file; added empty patterns in their place", true);
  }
  if (fixedEntries) {
    ctx.issue("RESOURCES_COMPLETED",
              "made content-stream resources explicit: added " +
                  std::to_string(fixedEntries) +
                  " missing resource entr(ies) from the inherited scope (ISO 19005-2 6.2.2)",
              true);
  }
}

class InvisibleTextFilter : public QPDFObjectHandle::TokenFilter {
 public:
  void handleToken(QPDFTokenizer::Token const& token) override {
    QPDFTokenizer::token_type_e type = token.getType();
    if (type == QPDFTokenizer::tt_inline_image) {
      operands.clear();
      return;
    }
    if (type != QPDFTokenizer::tt_word) {
      operands.push_back(token);
      return;
    }
    std::string op = token.getValue();
    if (op == "BT") {
      emit(op);
      write(" 3 Tr");
      inText = true;
      ++kept;
    } else if (op == "ET") {
      emit(op);
      inText = false;
    } else if (inText) {
      if (op == "Tr" || op == "Do" || op == "sh" || op == "gs") {
        operands.clear();
        return;
      }
      emit(op);
    } else if (op == "q" || op == "Q" || op == "cm") {
      emit(op);
    } else {
      operands.clear();
    }
  }

  void handleEOF() override { operands.clear(); }

  int textRuns() const { return kept; }

 private:
  void emit(const std::string& op) {
    for (const auto& t : operands) writeToken(t);
    operands.clear();
    write(" ");
    write(op);
    write("\n");
  }

  std::vector<QPDFTokenizer::Token> operands;
  bool inText = false;
  int kept = 0;
};

std::string invisibleTextLayer(QPDFPageObjectHelper& ph, int& runs) {
  InvisibleTextFilter filter;
  Pl_Buffer buf("invisible-text");
  try {
    ph.filterContents(&filter, &buf);
  } catch (const std::exception&) {
    runs = 0;
    return std::string();
  }
  runs = filter.textRuns();
  if (!runs) return std::string();
  std::shared_ptr<Buffer> b(buf.getBuffer());
  return std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
}

namespace {
struct TMat {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};
TMat tmul(const TMat& m, const TMat& n) {
  return {m.a * n.a + m.b * n.c,       m.a * n.b + m.b * n.d,
          m.c * n.a + m.d * n.c,       m.c * n.b + m.d * n.d,
          m.e * n.a + m.f * n.c + n.e, m.e * n.b + m.f * n.d + n.f};
}

struct TransBBox : QPDFObjectHandle::ParserCallbacks {
  QPDFObjectHandle res;
  bool found = false;
  double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  TMat ctm;
  std::vector<TMat> stack;
  std::vector<double> nums;
  std::string lastName;
  bool transp = false;
  std::vector<bool> transpStack;
  double px0 = 1e9, py0 = 1e9, px1 = -1e9, py1 = -1e9;

  explicit TransBBox(QPDFObjectHandle r) : res(r) {}

  void addPt(double x, double y) {
    double tx = ctm.a * x + ctm.c * y + ctm.e;
    double ty = ctm.b * x + ctm.d * y + ctm.f;
    px0 = std::min(px0, tx); py0 = std::min(py0, ty);
    px1 = std::max(px1, tx); py1 = std::max(py1, ty);
  }
  void grow() {
    if (px1 < px0) return;
    x0 = std::min(x0, px0); y0 = std::min(y0, py0);
    x1 = std::max(x1, px1); y1 = std::max(y1, py1);
    found = true;
  }
  void resetPath() { px0 = py0 = 1e9; px1 = py1 = -1e9; }

  void handleObject(QPDFObjectHandle obj, size_t, size_t) override {
    if (!obj.isOperator()) {
      if (obj.isName()) lastName = obj.getName();
      else if (obj.isNumber()) nums.push_back(obj.getNumericValue());
      return;
    }
    std::string op = obj.getOperatorValue();
    size_t n = nums.size();
    if (op == "q") { stack.push_back(ctm); transpStack.push_back(transp); }
    else if (op == "Q") {
      if (!stack.empty()) { ctm = stack.back(); stack.pop_back(); }
      if (!transpStack.empty()) { transp = transpStack.back(); transpStack.pop_back(); }
    } else if (op == "cm" && n >= 6) {
      ctm = tmul(TMat{nums[n-6], nums[n-5], nums[n-4], nums[n-3], nums[n-2], nums[n-1]}, ctm);
    } else if (op == "gs" && res.isDictionary()) {
      QPDFObjectHandle egs = res.getKey("/ExtGState");
      QPDFObjectHandle g = egs.isDictionary() ? egs.getKey(lastName) : QPDFObjectHandle::newNull();
      if (g.isDictionary()) {
        bool t = false;
        if (g.getKey("/ca").isNumber() && g.getKey("/ca").getNumericValue() < 1.0) t = true;
        if (g.getKey("/CA").isNumber() && g.getKey("/CA").getNumericValue() < 1.0) t = true;
        if (!g.getKey("/SMask").isNull() && !nameIs(g.getKey("/SMask"), "/None")) t = true;
        QPDFObjectHandle bm = g.getKey("/BM");
        std::string bmn = nameOf(bm.isArray() && bm.getArrayNItems() ? bm.getArrayItem(0) : bm);
        if (!bmn.empty() && bmn != "/Normal" && bmn != "/Compatible") t = true;
        if (t) transp = true;
      }
    } else if (op == "m" || op == "l") { if (n >= 2) addPt(nums[n-2], nums[n-1]); }
    else if (op == "c" && n >= 6) { addPt(nums[n-6], nums[n-5]); addPt(nums[n-4], nums[n-3]); addPt(nums[n-2], nums[n-1]); }
    else if (op == "v" || op == "y") { if (n >= 4) { addPt(nums[n-4], nums[n-3]); addPt(nums[n-2], nums[n-1]); } }
    else if (op == "re" && n >= 4) { addPt(nums[n-4], nums[n-3]); addPt(nums[n-4]+nums[n-2], nums[n-3]+nums[n-1]); }
    else if (op == "S" || op == "s" || op == "f" || op == "F" || op == "f*" ||
             op == "B" || op == "B*" || op == "b" || op == "b*" || op == "n") {
      if (transp) grow();
      resetPath();
    } else if (op == "Do") {
      if (transp) { addPt(0, 0); addPt(1, 1); grow(); resetPath(); }
    }
    nums.clear();
    lastName.clear();
  }
  void handleEOF() override {}
};
}

int pageRotation(QPDFPageObjectHelper& ph) {
  QPDFObjectHandle r = ph.getAttribute("/Rotate", false);
  if (!r.isInteger()) return 0;
  long long v = r.getIntValue() % 360;
  if (v < 0) v += 360;
  if (v != 90 && v != 180 && v != 270) return 0;
  return static_cast<int>(v);
}

void rotatePageBoxes(QPDFObjectHandle page, int rot, double pw, double phh) {
  if (rot == 0) return;
  static const char* kBoxes[] = {"/MediaBox", "/CropBox", "/TrimBox", "/BleedBox", "/ArtBox"};
  for (const char* key : kBoxes) {
    QPDFObjectHandle b = page.getKey(key);
    if (!b.isArray() || b.getArrayNItems() != 4) continue;
    double x1 = numOf(b.getArrayItem(0), 0), y1 = numOf(b.getArrayItem(1), 0);
    double x2 = numOf(b.getArrayItem(2), 0), y2 = numOf(b.getArrayItem(3), 0);
    double n[4];
    if (rot == 90) {
      n[0] = y1; n[1] = pw - x2; n[2] = y2; n[3] = pw - x1;
    } else if (rot == 180) {
      n[0] = pw - x2; n[1] = phh - y2; n[2] = pw - x1; n[3] = phh - y1;
    } else {
      n[0] = phh - y2; n[1] = x1; n[2] = phh - y1; n[3] = x2;
    }
    QPDFObjectHandle out = QPDFObjectHandle::newArray();
    for (int i = 0; i < 4; ++i) out.appendItem(QPDFObjectHandle::newReal(n[i], 4));
    page.replaceKey(key, out);
  }
  page.removeKey("/Rotate");
}

bool rasterFlattenPage(Ctx& ctx, QPDFPageObjectHelper& ph, int pageIndex, bool preserveText,
                       double dpi) {
  int w = 0, h = 0;
  std::string rgb;
  if (!ctx.opt.rasterizePage(pageIndex, dpi > 0 ? dpi : ctx.opt.rasterDpi, w, h, rgb)) return false;
  if (w <= 0 || h <= 0 || rgb.size() < static_cast<size_t>(w) * h * 3) return false;
  QPDFObjectHandle page = ph.getObjectHandle();
  QPDFObjectHandle media = ph.getAttribute("/MediaBox", true);
  double mx1 = 0, my1 = 0, mx2 = 612, my2 = 792;
  if (media.isArray() && media.getArrayNItems() == 4) {
    mx1 = numOf(media.getArrayItem(0), 0);
    my1 = numOf(media.getArrayItem(1), 0);
    mx2 = numOf(media.getArrayItem(2), 612);
    my2 = numOf(media.getArrayItem(3), 792);
  }
  double pw = mx2 - mx1, phh = my2 - my1;
  int rot = pageRotation(ph);
  if (rot) {
    rotatePageBoxes(page, rot, pw, phh);
    if (rot == 90 || rot == 270) {
      std::swap(pw, phh);
      std::swap(mx1, my1);
    }
    QPDFObjectHandle nm = page.getKey("/MediaBox");
    if (nm.isArray() && nm.getArrayNItems() == 4) {
      mx1 = numOf(nm.getArrayItem(0), 0);
      my1 = numOf(nm.getArrayItem(1), 0);
      pw = numOf(nm.getArrayItem(2), 612) - mx1;
      phh = numOf(nm.getArrayItem(3), 792) - my1;
    }
  }
  QPDFObjectHandle img = QPDFObjectHandle::newStream(&ctx.pdf, rgb);
  QPDFObjectHandle id = img.getDict();
  id.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
  id.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
  id.replaceKey("/Width", QPDFObjectHandle::newInteger(w));
  id.replaceKey("/Height", QPDFObjectHandle::newInteger(h));
  id.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
  id.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
  int runs = 0;
  std::string text;
  QPDFObjectHandle fonts = QPDFObjectHandle::newNull();
  if (preserveText) {
    QPDFObjectHandle oldRes = ph.getAttribute("/Resources", true);
    if (oldRes.isDictionary() && oldRes.getKey("/Font").isDictionary()) {
      text = invisibleTextLayer(ph, runs);
      if (runs) fonts = oldRes.getKey("/Font");
    }
  }

  QPDFObjectHandle res = QPDFObjectHandle::newDictionary();
  QPDFObjectHandle xo = QPDFObjectHandle::newDictionary();
  xo.replaceKey("/FlatIm", ctx.pdf.makeIndirectObject(img));
  res.replaceKey("/XObject", xo);
  if (!fonts.isNull()) res.replaceKey("/Font", fonts);
  page.replaceKey("/Resources", res);
  std::string content = "q " + fmtFixed(pw, 4) + " 0 0 " + fmtFixed(phh, 4) + " " +
                        fmtFixed(mx1, 4) + " " + fmtFixed(my1, 4) + " cm /FlatIm Do Q\n";
  if (!text.empty()) content += text;
  page.replaceKey("/Contents",
                  ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, content)));
  if (page.getKey("/Group").isDictionary()) page.removeKey("/Group");
  return true;
}

void neutraliseTransparency(Ctx& ctx, QPDFObjectHandle res) {
  std::map<QPDFObjGen, QPDFObjectHandle> done;
  std::map<QPDFObjGen, bool> memo;
  std::set<QPDFObjGen> clean;
  auto copyOf = [&ctx](QPDFObjectHandle src) {
    QPDFObjectHandle n = QPDFObjectHandle::newStream(&ctx.pdf);
    QPDFObjectHandle d = src.getDict();
    n.replaceStreamData(src.getRawStreamData(), d.getKey("/Filter"), d.getKey("/DecodeParms"));
    for (const std::string& k : d.getKeys()) {
      if (k != "/Length" && k != "/Filter" && k != "/DecodeParms") n.getDict().replaceKey(k, d.getKey(k));
    }
    return n;
  };
  auto privatise = [&ctx](QPDFObjectHandle owner, const std::string& key) {
    QPDFObjectHandle child = owner.getKey(key);
    if (!child.isIndirect()) return child;
    QPDFObjectHandle copy = ctx.pdf.makeIndirectObject(child.shallowCopy());
    owner.replaceKey(key, copy);
    return copy;
  };
  std::function<void(QPDFObjectHandle, int)> neutralize = [&](QPDFObjectHandle r, int depth) {
    if (depth > kMaxContentNest || !r.isDictionary()) return;
    if (r.getKey("/ExtGState").isDictionary()) {
      QPDFObjectHandle egs = privatise(r, "/ExtGState");
      for (const std::string& k : egs.getKeys()) {
        if (!gsHasTransparency(egs.getKey(k))) continue;
        QPDFObjectHandle g = privatise(egs, k);
        g.replaceKey("/ca", QPDFObjectHandle::newReal(1.0, 3));
        g.replaceKey("/CA", QPDFObjectHandle::newReal(1.0, 3));
        g.replaceKey("/BM", QPDFObjectHandle::newName("/Normal"));
        if (!g.getKey("/SMask").isNull()) g.replaceKey("/SMask", QPDFObjectHandle::newName("/None"));
      }
    }
    if (r.getKey("/Pattern").isDictionary()) {
      QPDFObjectHandle pd = privatise(r, "/Pattern");
      for (const std::string& k : pd.getKeys()) {
        QPDFObjectHandle orig = pd.getKey(k);
        QPDFObjectHandle od = orig.isStream() ? orig.getDict() : orig;
        if (!od.isDictionary()) continue;
        bool gsT = gsHasTransparency(od.getKey("/ExtGState"));
        bool resT = orig.isStream() && resourcesHaveTransparency(od.getKey("/Resources"), memo, 0);
        if (!gsT && !resT) continue;
        QPDFObjectHandle cp = orig.isStream() ? copyOf(orig) : orig.shallowCopy();
        pd.replaceKey(k, ctx.pdf.makeIndirectObject(cp));
        QPDFObjectHandle cd = cp.isStream() ? cp.getDict() : cp;
        if (gsT) {
          QPDFObjectHandle g = cd.getKey("/ExtGState").shallowCopy();
          g.replaceKey("/ca", QPDFObjectHandle::newReal(1.0, 3));
          g.replaceKey("/CA", QPDFObjectHandle::newReal(1.0, 3));
          g.replaceKey("/BM", QPDFObjectHandle::newName("/Normal"));
          if (!g.getKey("/SMask").isNull()) g.replaceKey("/SMask", QPDFObjectHandle::newName("/None"));
          cd.replaceKey("/ExtGState", g);
        }
        if (resT && cd.getKey("/Resources").isDictionary()) neutralize(privatise(cd, "/Resources"), depth + 1);
      }
    }
    if (r.getKey("/XObject").isDictionary()) {
      QPDFObjectHandle xod = privatise(r, "/XObject");
      for (const std::string& k : xod.getKeys()) {
        QPDFObjectHandle orig = xod.getKey(k);
        if (!orig.isStream()) continue;
        if (orig.isIndirect() && done.count(orig.getObjGen())) {
          xod.replaceKey(k, done[orig.getObjGen()]);
          continue;
        }
        if (orig.isIndirect() && clean.count(orig.getObjGen())) continue;
        QPDFObjectHandle od = orig.getDict();
        bool form = nameIs(od.getKey("/Subtype"), "/Form");
        bool needsCopy = (!od.getKey("/SMask").isNull() && !nameIs(od.getKey("/SMask"), "/None")) ||
                         numOf(od.getKey("/SMaskInData"), 0) > 0 ||
                         (form && (od.getKey("/Group").isDictionary() ||
                                   resourcesHaveTransparency(od.getKey("/Resources"), memo, 0)));
        if (!needsCopy) {
          if (orig.isIndirect()) clean.insert(orig.getObjGen());
          continue;
        }
        QPDFObjectHandle xo = orig.isIndirect() ? copyOf(orig) : orig;
        if (orig.isIndirect()) {
          xo = ctx.pdf.makeIndirectObject(xo);
          xod.replaceKey(k, xo);
          done[orig.getObjGen()] = xo;
          done[xo.getObjGen()] = xo;
        }
        QPDFObjectHandle d = xo.getDict();
        if (d.hasKey("/SMask")) d.removeKey("/SMask");
        if (d.hasKey("/SMaskInData")) d.removeKey("/SMaskInData");
        if (form) {
          if (d.getKey("/Group").isDictionary() && nameIs(d.getKey("/Group").getKey("/S"), "/Transparency")) {
            d.removeKey("/Group");
          }
          if (d.getKey("/Resources").isDictionary()) neutralize(privatise(d, "/Resources"), depth + 1);
        }
      }
    }
  };
  neutralize(res, 0);
}

bool resourcesHaveTransparency(QPDFObjectHandle res, std::map<QPDFObjGen, bool>& memo, int depth) {
  if (!res.isDictionary() || depth > kMaxContentNest) return false;
  if (res.isIndirect()) {
    auto it = memo.find(res.getObjGen());
    if (it != memo.end()) return it->second;
    memo[res.getObjGen()] = false;
  }
  bool t = false;
  QPDFObjectHandle gsd = res.getKey("/ExtGState");
  if (gsd.isDictionary()) {
    for (const std::string& k : gsd.getKeys()) t = t || gsHasTransparency(gsd.getKey(k));
  }
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (!t && xod.isDictionary()) {
    for (const std::string& k : xod.getKeys()) {
      QPDFObjectHandle xo = xod.getKey(k);
      if (!xo.isStream() || t) continue;
      QPDFObjectHandle d = xo.getDict();
      if (nameIs(d.getKey("/Subtype"), "/Image")) {
        QPDFObjectHandle sm = d.getKey("/SMask");
        t = (!sm.isNull() && !nameIs(sm, "/None")) || numOf(d.getKey("/SMaskInData"), 0) > 0;
        continue;
      }
      if (!nameIs(d.getKey("/Subtype"), "/Form")) continue;
      QPDFObjectHandle grp = d.getKey("/Group");
      if (grp.isDictionary() && nameIs(grp.getKey("/S"), "/Transparency")) {
        t = true;
        continue;
      }
      if (xo.isIndirect()) {
        auto it = memo.find(xo.getObjGen());
        if (it != memo.end()) {
          t = it->second;
          continue;
        }
        memo[xo.getObjGen()] = false;
      }
      bool ft = resourcesHaveTransparency(d.getKey("/Resources"), memo, depth + 1);
      if (xo.isIndirect()) memo[xo.getObjGen()] = ft;
      t = ft;
    }
  }
  QPDFObjectHandle pat = res.getKey("/Pattern");
  if (!t && pat.isDictionary()) {
    for (const std::string& k : pat.getKeys()) {
      QPDFObjectHandle pp = pat.getKey(k);
      QPDFObjectHandle pd = pp.isStream() ? pp.getDict() : pp;
      if (!pd.isDictionary() || t) continue;
      t = gsHasTransparency(pd.getKey("/ExtGState")) ||
          (pp.isStream() && resourcesHaveTransparency(pd.getKey("/Resources"), memo, depth + 1));
    }
  }
  if (res.isIndirect()) memo[res.getObjGen()] = t;
  return t;
}

bool resourcesHaveTransparency(QPDFObjectHandle res) {
  Visited visited;
  TransparencyReport rep;
  scanResources(res, visited, rep);
  return rep.real || !rep.where.empty();
}

bool regionFlattenPage(Ctx& ctx, QPDFPageObjectHelper& ph, int pageIndex) {
  QPDFObjectHandle page = ph.getObjectHandle();
  QPDFObjectHandle res = ph.getAttribute("/Resources", true);
  if (!res.isDictionary()) return false;
  QPDFObjectHandle media = ph.getAttribute("/MediaBox", true);
  double mx1 = 0, my1 = 0, mx2 = 612, my2 = 792;
  if (media.isArray() && media.getArrayNItems() == 4) {
    mx1 = numOf(media.getArrayItem(0), 0); my1 = numOf(media.getArrayItem(1), 0);
    mx2 = numOf(media.getArrayItem(2), 612); my2 = numOf(media.getArrayItem(3), 792);
  }
  double pw = mx2 - mx1, phh = my2 - my1;
  if (pw <= 1 || phh <= 1) return false;

  TransBBox scan(res);
  try {
    QPDFObjectHandle::parseContentStream(page.getKey("/Contents"), &scan);
  } catch (...) {
    return false;
  }
  if (!scan.found || scan.x1 <= scan.x0 || scan.y1 <= scan.y0) return false;
  double margin = std::min(pw, phh) * 0.02;
  double tx0 = std::max(mx1, scan.x0 - margin), ty0 = std::max(my1, scan.y0 - margin);
  double tx1 = std::min(mx2, scan.x1 + margin), ty1 = std::min(my2, scan.y1 + margin);
  double regionArea = (tx1 - tx0) * (ty1 - ty0);
  if (regionArea >= pw * phh * 0.55) return false;
  if (pageRotation(ph) != 0) return false;

  int w = 0, h = 0;
  std::string rgb;
  if (!ctx.opt.rasterizePage(pageIndex, ctx.opt.rasterDpi, w, h, rgb)) return false;
  if (w <= 0 || h <= 0 || rgb.size() < static_cast<size_t>(w) * h * 3) return false;

  QPDFObjectHandle privRes = res;
  if (res.isIndirect()) {
    privRes = ctx.pdf.makeIndirectObject(res.shallowCopy());
    page.replaceKey("/Resources", privRes);
  } else {
    page.replaceKey("/Resources", res);
    privRes = page.getKey("/Resources");
  }
  neutraliseTransparency(ctx, privRes);

  QPDFObjectHandle img = QPDFObjectHandle::newStream(&ctx.pdf, rgb);
  QPDFObjectHandle id = img.getDict();
  id.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
  id.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
  id.replaceKey("/Width", QPDFObjectHandle::newInteger(w));
  id.replaceKey("/Height", QPDFObjectHandle::newInteger(h));
  id.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
  id.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));

  std::string baseContent;
  {
    QPDFObjectHandle contents = page.getKey("/Contents");
    std::vector<QPDFObjectHandle> streams;
    if (contents.isStream()) streams.push_back(contents);
    else if (contents.isArray()) {
      for (int i = 0; i < contents.getArrayNItems(); ++i) {
        if (contents.getArrayItem(i).isStream()) streams.push_back(contents.getArrayItem(i));
      }
    }
    if (streams.empty()) return false;
    for (QPDFObjectHandle s : streams) {
      try {
        auto b = s.getStreamData(qpdf_dl_all);
        baseContent.append(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
        baseContent += "\n";
      } catch (...) {
        return false;
      }
    }
  }
  QPDFObjectHandle base = QPDFObjectHandle::newStream(&ctx.pdf, baseContent);
  QPDFObjectHandle bd = base.getDict();
  bd.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
  bd.replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
  bd.replaceKey("/FormType", QPDFObjectHandle::newInteger(1));
  QPDFObjectHandle bbox = QPDFObjectHandle::newArray();
  for (double v : {mx1, my1, mx2, my2}) bbox.appendItem(QPDFObjectHandle::newReal(v, 3));
  bd.replaceKey("/BBox", bbox);
  bd.replaceKey("/Resources", res);

  QPDFObjectHandle newRes = QPDFObjectHandle::newDictionary();
  QPDFObjectHandle xo = QPDFObjectHandle::newDictionary();
  xo.replaceKey("/KuraBase", ctx.pdf.makeIndirectObject(base));
  xo.replaceKey("/FlatIm", ctx.pdf.makeIndirectObject(img));
  newRes.replaceKey("/XObject", xo);

  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "q %.3f %.3f m %.3f %.3f l %.3f %.3f l %.3f %.3f l h\n"
                "%.3f %.3f m %.3f %.3f l %.3f %.3f l %.3f %.3f l h W n /KuraBase Do Q\n"
                "q %.3f %.3f %.3f %.3f re W n %.4f 0 0 %.4f %.4f %.4f cm /FlatIm Do Q\n",
                mx1, my1, mx2, my1, mx2, my2, mx1, my2,
                tx0, ty0, tx1, ty0, tx1, ty1, tx0, ty1,
                tx0, ty0, tx1 - tx0, ty1 - ty0, pw, phh, mx1, my1);
  page.replaceKey("/Resources", newRes);
  page.replaceKey("/Contents",
                  ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, std::string(buf))));
  if (page.getKey("/Group").isDictionary()) page.removeKey("/Group");
  ctx.issue("TRANSPARENCY_REGION_FLATTENED",
            "flattened transparency in a confined region and kept the rest of the page as "
            "vector (sharp text and line art preserved)",
            true);
  return true;
}

namespace {
void fixGState(Ctx& ctx, QPDFObjectHandle gs) {
  if (!gs.isDictionary()) return;
  for (const char* k : {"/CA", "/ca"}) {
    if (ctx.transparencyBanned() && gs.getKey(k).isNumber() && gs.getKey(k).getNumericValue() > 1.0 + 1e-6) {
      gs.replaceKey(k, QPDFObjectHandle::newInteger(1));
      ctx.issue("ALPHA_CLAMPED", std::string("clamped out-of-range ") + k + " to 1", true);
    } else if (ctx.transparencyBanned() && gs.hasKey(k) && !gs.getKey(k).isNumber() && !gs.getKey(k).isNull()) {
      gs.replaceKey(k, QPDFObjectHandle::newInteger(1));
      ctx.issue("ALPHA_CLAMPED", std::string("replaced non-numeric ") + k + " with 1", true);
    }
  }
  bool halftoneRules = !ctx.opt.verifyOnly || !ctx.isA() || ctx.part >= 2;
  if (gs.hasKey("/TR") && !(ctx.opt.verifyOnly && ctx.isE())) {
    gs.removeKey("/TR");
    ctx.issue("TRANSFER_FUNCTION_REMOVED", "removed /TR from ExtGState", true);
  }
  if (halftoneRules && gs.hasKey("/HTO")) {
    gs.removeKey("/HTO");
    ctx.issue("HALFTONE_ORIGIN_REMOVED", "removed /HTO from ExtGState", true);
  }
  QPDFObjectHandle tr2 = gs.getKey("/TR2");
  if (!tr2.isNull() && !nameIs(tr2, "/Default")) {
    gs.replaceKey("/TR2", QPDFObjectHandle::newName("/Default"));
    ctx.issue("TRANSFER_FUNCTION_REMOVED", "replaced non-default /TR2 with /Default", true);
  }
  if (halftoneRules && gs.hasKey("/HTP") && !(ctx.opt.verifyOnly && ctx.isA() && ctx.part >= 4)) {
    gs.removeKey("/HTP");
    ctx.issue("HALFTONE_PHASE_REMOVED", "removed /HTP from ExtGState", true);
  }
  QPDFObjectHandle ht = gs.getKey("/HT");
  if (halftoneRules && (ht.isDictionary() || ht.isStream())) {
    QPDFObjectHandle htd = ht.isStream() ? ht.getDict() : ht;
    long long type = htd.getKey("/HalftoneType").isInteger()
                         ? htd.getKey("/HalftoneType").getIntValue()
                         : 0;
    bool subOk = true;
    for (const std::string& ck : type == 5 ? htd.getKeys() : std::set<std::string>()) {
      QPDFObjectHandle sub = htd.getKey(ck);
      QPDFObjectHandle subd = sub.isStream() ? sub.getDict() : sub;
      if (!subd.isDictionary() || !subd.hasKey("/HalftoneType")) continue;
      long long st = subd.getKey("/HalftoneType").isInteger() ? subd.getKey("/HalftoneType").getIntValue() : 0;
      if (st != 1 && st != 5) subOk = false;
    }
    if (type != 1 && type != 5) {
      gs.removeKey("/HT");
      ctx.issue("HALFTONE_REMOVED", "removed halftone of type " + std::to_string(type), true);
    } else if (!subOk) {
      gs.removeKey("/HT");
      ctx.issue("HALFTONE_REMOVED", "removed a type 5 halftone holding a component halftone of a type other than 1 or 5", true);
    } else {
      if (htd.hasKey("/HalftoneName")) {
        htd.removeKey("/HalftoneName");
        ctx.issue("HALFTONE_NAME_REMOVED", "removed /HalftoneName from halftone", true);
      }
      if (htd.hasKey("/TransferFunction")) {
        htd.removeKey("/TransferFunction");
        ctx.issue("HALFTONE_TF_REMOVED", "removed /TransferFunction from halftone", true);
      }
      if (type == 5) {
        static const std::set<std::string> kPrimary = {"/Cyan", "/Magenta", "/Yellow",
                                                       "/Black", "/Default"};
        std::set<QPDFObjGen> seenSub;
        for (const std::string& ck : htd.getKeys()) {
          if (ck == "/Type" || ck == "/HalftoneType") continue;
          QPDFObjectHandle sub = htd.getKey(ck);
          if (sub.isIndirect() && !seenSub.insert(sub.getObjGen()).second && !ctx.opt.verifyOnly) {
            sub = sub.isStream() ? sub.copyStream() : ctx.pdf.makeIndirectObject(sub.shallowCopy());
            htd.replaceKey(ck, sub);
          }
          QPDFObjectHandle subd = sub.isStream() ? sub.getDict()
                                                 : (sub.isDictionary() ? sub
                                                                       : QPDFObjectHandle::newNull());
          if (!subd.isDictionary()) continue;
          if (subd.hasKey("/HalftoneName")) {
            subd.removeKey("/HalftoneName");
            ctx.issue("HALFTONE_NAME_REMOVED", "removed /HalftoneName from halftone", true);
          }
          if (kPrimary.count(ck) && subd.hasKey("/TransferFunction")) {
            subd.removeKey("/TransferFunction");
            ctx.issue("HALFTONE_TF_REMOVED",
                      "removed /TransferFunction from primary colourant halftone " + ck,
                      !ctx.opt.verifyOnly || ck != "/Default");
          } else if (!kPrimary.count(ck) && !subd.hasKey("/TransferFunction")) {
            subd.replaceKey("/TransferFunction", QPDFObjectHandle::newName("/Identity"));
            ctx.issue("HALFTONE_TF_ADDED",
                      "added identity /TransferFunction to nonprimary colourant halftone " +
                          ck,
                      true);
          }
        }
      }
    }
  }
  if (gs.hasKey("/RI") && kValidIntents.count(nameOf(gs.getKey("/RI"))) == 0 &&
      !(ctx.opt.verifyOnly && ((ctx.isA() && ctx.part >= 4) || ctx.isE()))) {
    gs.removeKey("/RI");
    ctx.issue("RENDERING_INTENT_FIXED", "removed invalid rendering intent in ExtGState", true);
  }
  QPDFObjectHandle bm = gs.getKey("/BM");
  if (!bm.isNull()) {
    static const std::set<std::string> kBlendModes = {
        "/Normal", "/Multiply", "/Screen", "/Overlay", "/Darken", "/Lighten",
        "/ColorDodge", "/ColorBurn", "/HardLight", "/SoftLight", "/Difference",
        "/Exclusion", "/Hue", "/Saturation", "/Color", "/Luminosity"};
    std::string pick;
    if (bm.isName() && kBlendModes.count(bm.getName())) pick = bm.getName();
    if (bm.isArray()) {
      for (int i = 0; i < bm.getArrayNItems() && pick.empty(); ++i) {
        std::string n = nameOf(bm.getArrayItem(i));
        if (kBlendModes.count(n)) pick = n;
      }
    }
    bool isPlainStandard = (bm.isName() && (!pick.empty() || bm.getName() == "/Compatible")) ||
                           (ctx.opt.verifyOnly && ctx.isA() && ctx.part >= 4 && bm.isArray() && !pick.empty()) ||
                           (ctx.opt.verifyOnly && ctx.isE());
    if (!isPlainStandard) {
      gs.replaceKey("/BM", QPDFObjectHandle::newName(pick.empty() ? "/Normal" : pick));
      ctx.issue("BLEND_MODE_FIXED",
                "replaced non-standard blend mode with " +
                    (pick.empty() ? std::string("/Normal") : pick),
                true);
    }
  }
}

void flattenGStateTransparency(Ctx& ctx, QPDFObjectHandle gs) {
  if (!gs.isDictionary()) return;
  QPDFObjectHandle sm = gs.getKey("/SMask");
  if (!sm.isNull() && !nameIs(sm, "/None")) {
    gs.replaceKey("/SMask", QPDFObjectHandle::newName("/None"));
    ctx.issue("TRANSPARENCY_FLATTENED", "forced ExtGState /SMask to /None (visual risk)", true);
  }
  QPDFObjectHandle bm = gs.getKey("/BM");
  std::string bmName = nameOf(bm);
  if (bm.isArray() && bm.getArrayNItems() > 0) bmName = nameOf(bm.getArrayItem(0));
  if (!bmName.empty() && bmName != "/Normal" && bmName != "/Compatible") {
    gs.replaceKey("/BM", QPDFObjectHandle::newName("/Normal"));
    ctx.issue("TRANSPARENCY_FLATTENED", "forced blend mode " + bmName + " to /Normal (visual risk)", true);
  }
  if (numOf(gs.getKey("/CA"), 1.0) < 1.0) {
    gs.replaceKey("/CA", QPDFObjectHandle::newInteger(1));
    ctx.issue("TRANSPARENCY_FLATTENED", "forced /CA to 1 (visual risk)", true);
  }
  if (numOf(gs.getKey("/ca"), 1.0) < 1.0) {
    gs.replaceKey("/ca", QPDFObjectHandle::newInteger(1));
    ctx.issue("TRANSPARENCY_FLATTENED", "forced /ca to 1 (visual risk)", true);
  }
}

bool imageBitsAllowed(QPDFObjectHandle dict, bool asMask) {
  QPDFObjectHandle b = dict.getKey("/BitsPerComponent");
  if (!b.isInteger()) b = dict.getKey("/BPC");
  if (!b.isInteger()) return true;
  long long v = b.getIntValue();
  if (asMask) return v == 1;
  for (const char* k : {"/ImageMask", "/IM"}) {
    if (dict.getKey(k).isBool() && dict.getKey(k).getBoolValue()) return true;
  }
  return v == 1 || v == 2 || v == 4 || v == 8 || v == 16;
}

void fixImage(Ctx& ctx, QPDFObjectHandle image, int depth = 0, bool asMask = false);

void fixImage(Ctx& ctx, QPDFObjectHandle image, int depth, bool asMask) {
  QPDFObjectHandle d = image.getDict();
  if (depth < 3) {
    for (const char* key : {"/SMask", "/Mask"}) {
      QPDFObjectHandle m = d.getKey(key);
      if (m.isStream()) fixImage(ctx, m, depth + 1, std::string(key) == "/Mask");
    }
  }
  if (ctx.isA() && !imageBitsAllowed(d, asMask)) {
    if (ctx.opt.verifyOnly || repackImageBits(image, asMask ? 1 : 8)) {
      ctx.issue("IMAGE_DEPTH_NORMALIZED",
                asMask ? "re-encoded an image used as a stencil mask at 1 bit per component"
                       : "re-encoded an image with an unsupported bit depth at 8 bits per component",
                true);
    }
  }
  if (d.getKey("/Interpolate").isBool() && d.getKey("/Interpolate").getBoolValue() && !ctx.isX()) {
    d.removeKey("/Interpolate");
    ctx.issue("INTERPOLATE_REMOVED", "removed /Interpolate true from image", true);
  }
  bool printAlternate = !ctx.isX() || !ctx.opt.verifyOnly;
  QPDFObjectHandle alts = d.getKey("/Alternates");
  for (int i = 0; !printAlternate && alts.isArray() && i < alts.getArrayNItems(); ++i) {
    QPDFObjectHandle dp = alts.getArrayItem(i).getKey("/DefaultForPrinting");
    printAlternate = dp.isBool() && dp.getBoolValue();
  }
  if (d.hasKey("/Alternates") && printAlternate) {
    d.removeKey("/Alternates");
    ctx.issue("ALTERNATES_REMOVED", "removed /Alternates from image", true);
  }
  if (d.hasKey("/OPI")) {
    d.removeKey("/OPI");
    ctx.issue("OPI_REMOVED", "removed /OPI from image", true);
  }
  retagDctColorSpace(ctx, image);
  if (d.hasKey("/Intent") && kValidIntents.count(nameOf(d.getKey("/Intent"))) == 0 &&
      !(ctx.opt.verifyOnly && ((ctx.isA() && ctx.part >= 4) || ctx.isE()))) {
    d.removeKey("/Intent");
    ctx.issue("RENDERING_INTENT_FIXED", "removed invalid rendering intent on image", true);
  }
  {
    QPDFObjectHandle filters = d.getKey("/Filter");
    std::vector<std::string> names;
    if (filters.isName()) names.push_back(filters.getName());
    if (filters.isArray()) {
      for (int i = 0; i < filters.getArrayNItems(); ++i) {
        names.push_back(nameOf(filters.getArrayItem(i)));
      }
    }
    bool jpx = false;
    for (const std::string& n : names) {
      if (n == "/JPXDecode") jpx = true;
    }
    if (jpx && ctx.pdf14Target()) {
      if (!transcodeJpxImage(ctx, image)) return;
    } else if (jpx && names.size() == 1 && ((ctx.isA() && ctx.part >= 2) || ctx.isX())) {
      std::string raw;
      try {
        auto buf = image.getRawStreamData();
        raw.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
      } catch (...) {
      }
      if (!raw.empty() && (!jpxPdfaConformant(raw, d.hasKey("/ColorSpace"), ctx.isA()) ||
                           (ctx.isX() && !ctx.pdf20Print() && !jpxPrintDepthOk(raw)))) {
        if (!transcodeJpxImage(ctx, image, false)) return;
        if (ctx.opt.verifyOnly && nameIs(d.getKey("/Filter"), "/JPXDecode"))
          ctx.issueMerged("JPX_NONCONFORMING", "JPEG2000 image data breaks the archive image rules and cannot be re-encoded", true);
      }
    }
  }
  if (ctx.isX()) {
    QPDFObjectHandle filters = d.getKey("/Filter");
    bool jbig2 = nameIs(filters, "/JBIG2Decode");
    for (int i = 0; filters.isArray() && i < filters.getArrayNItems(); ++i) {
      if (nameIs(filters.getArrayItem(i), "/JBIG2Decode")) jbig2 = true;
    }
    if (jbig2 && (ctx.pdf14Target() || ctx.opt.renderPdf) && !reencodeJbig2Image(ctx, image, ctx.pdf14Target())) return;
  }
  if (ctx.pdf14Target()) {
    QPDFObjectHandle bpc = d.getKey("/BitsPerComponent");
    if (bpc.isInteger() && bpc.getIntValue() == 16 && !reduceImageDepth(ctx, image)) return;
    if (d.getKey("/SMask").isStream()) {
      if (!flattenImageSMask(ctx, image)) return;
    }
  }
}

void fixResources(Ctx& ctx, QPDFObjectHandle res, Visited& visited, bool flatten) {
  DepthGuard g_(visited);
  if (g_.over) return;
  if (!res.isDictionary() || !visited.enter(res)) return;
  QPDFObjectHandle fontd = res.getKey("/Font");
  if (fontd.isDictionary()) {
    for (const std::string& k : fontd.getKeys()) {
      QPDFObjectHandle f = fontd.getKey(k);
      bool isFont = f.isDictionary() && f.getKey("/Subtype").isName() &&
                    (!f.hasKey("/Type") || nameIs(f.getKey("/Type"), "/Font"));
      if (isFont && nameIs(f.getKey("/Subtype"), "/Type3") && !unusedHere(f) && visited.enter(f)) {
        fixResources(ctx, f.getKey("/Resources"), visited, flatten);
        QPDFObjectHandle procs = f.getKey("/CharProcs");
        for (const std::string& g : procs.isDictionary() ? procs.getKeys() : std::set<std::string>()) {
          if (procs.getKey(g).isStream()) fixResources(ctx, procs.getKey(g).getDict().getKey("/Resources"), visited, flatten);
        }
        if (ctx.failed()) return;
      }
      if (isFont) continue;
      fontd.replaceKey(k, QPDFObjectHandle::parse(
                              "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                              "/Encoding /WinAnsiEncoding >>"));
      ctx.issue("FONT_RESOURCE_REPLACED",
                "font resource " + k + " did not point at a font; replaced it with a standard font",
                true);
    }
  }
  QPDFObjectHandle gsd = res.getKey("/ExtGState");
  if (gsd.isDictionary()) {
    for (const std::string& k : gsd.getKeys()) {
      if (unusedHere(gsd.getKey(k))) continue;
      fixGState(ctx, gsd.getKey(k));
      if (flatten) flattenGStateTransparency(ctx, gsd.getKey(k));
    }
  }
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (xod.isDictionary()) {
    for (const std::string& k : xod.getKeys()) {
      QPDFObjectHandle xo = xod.getKey(k);
      if (!xo.isStream() || unusedHere(xo)) continue;
      if (!visited.enter(xo)) continue;
      QPDFObjectHandle d = xo.getDict();
      std::string subtype = nameOf(d.getKey("/Subtype"));
      if (ctx.isA() && ctx.part == 1 && (subtype == "/Image" || subtype == "/Form") && d.hasKey("/SMask") &&
          !(subtype == "/Image" && d.getKey("/SMask").isStream())) {
        d.removeKey("/SMask");
        ctx.issue("TRANSPARENCY_FLATTENED", "removed /SMask from an XObject, which PDF/A-1 does not allow", true);
      }
      if (subtype == "/Image") {
        fixImage(ctx, xo);
        if (ctx.failed()) return;
      } else if (subtype == "/PS" && !(ctx.opt.verifyOnly && ctx.isA() && ctx.part >= 4)) {
        xod.removeKey(k);
        ctx.issue("POSTSCRIPT_XOBJECT_REMOVED", "removed PostScript XObject " + k, true);
      } else if (subtype == "/Form") {
        QPDFObjectHandle bb = d.getKey("/BBox");
        bool bbOk = bb.isArray() && bb.getArrayNItems() == 4;
        for (int i = 0; bbOk && i < 4; ++i) bbOk = bb.getArrayItem(i).isNumber();
        if (!bbOk) {
          d.replaceKey("/BBox", QPDFObjectHandle::parse("[-32767 -32767 32767 32767]"));
          ctx.issue("FORM_BBOX_ADDED", "gave form XObject " + k + " the bounding box it lacked", true);
        }
        if (((ctx.isA() && ctx.part >= 2) || ctx.pdf20Print() || (ctx.isX() && !ctx.opt.verifyOnly)) &&
            !d.getKey("/Resources").isDictionary() &&
            res.isDictionary()) {
          if (!ctx.opt.verifyOnly) d.replaceKey("/Resources", res);
          ctx.issue("FORM_RESOURCES_ADDED",
                    "added explicit /Resources to a form XObject (inherited resources are "
                    "not permitted at this level)",
                    !ctx.opt.verifyOnly);
        }
        if (d.hasKey("/Ref")) {
          QPDFObjectHandle ref = d.getKey("/Ref");
          bool wellFormed = ref.isDictionary() && (ref.getKey("/F").isDictionary() ||
                                                   ref.getKey("/F").isString());
          if (ctx.allowRefXObjects() && wellFormed) {
            ctx.issue("REFERENCE_XOBJECT_KEPT",
                      "kept reference XObject pointing at an external document", false);
            if (!ref.getKey("/ID").isArray()) {
              ctx.issue("REFERENCE_XOBJECT_INCOMPLETE",
                        "reference XObject does not carry the file ID pair of the "
                        "document it points at",
                        true);
            }
            if (!d.getKey("/Metadata").isStream()) {
              ctx.issue("REFERENCE_XOBJECT_INCOMPLETE",
                        "reference XObject has no metadata describing the referenced "
                        "rendition",
                        true);
            } else {
              std::string xmp;
              try {
                auto buf = d.getKey("/Metadata").getStreamData(qpdf_dl_all);
                xmp.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
              } catch (...) {
              }
              size_t at = xmp.find(":RenditionOf");
              bool complete = at != std::string::npos;
              for (const char* field : {":documentID", ":versionID", ":renditionClass"}) {
                if (complete && xmp.find(field, at) == std::string::npos) complete = false;
              }
              if (!complete) {
                ctx.issue("REFERENCE_XOBJECT_INCOMPLETE",
                          "reference XObject metadata does not identify the referenced document, "
                          "version and rendition",
                          true);
              }
            }
            QPDFObjectHandle fs = ref.getKey("/F");
            if (!ctx.opt.inputPath.empty() && !(fs.isDictionary() && fs.getKey("/EF").isDictionary())) {
              std::string url;
              if (fs.isString()) url = fs.getUTF8Value();
              for (const char* fk : {"/UF", "/F", "/Unix", "/DOS", "/Mac"}) {
                if (url.empty() && fs.isDictionary() && fs.getKey(fk).isString()) url = fs.getKey(fk).getUTF8Value();
              }
              if (url.rfind("file://", 0) == 0) url = url.substr(7);
              if (url.find("://") == std::string::npos) {
                std::filesystem::path p(url);
                if (p.is_relative()) p = std::filesystem::path(ctx.opt.inputPath).parent_path() / p;
                std::error_code ec;
                if (url.empty() || !std::filesystem::is_regular_file(p, ec)) {
                  ctx.issue("REFERENCE_XOBJECT_INCOMPLETE",
                            "the file a reference XObject points at cannot be found next to the document",
                            true);
                }
              }
            }
          } else if (ctx.allowRefXObjects()) {
            d.removeKey("/Ref");
            ctx.issue("REFERENCE_XOBJECT_FIXED",
                      "removed /Ref without a usable file reference from form XObject", true);
          } else {
            d.removeKey("/Ref");
            ctx.issue("REFERENCE_XOBJECT_FIXED", "removed /Ref from form XObject", true);
          }
        }
        if (d.hasKey("/PS")) {
          d.removeKey("/PS");
          ctx.issue("POSTSCRIPT_XOBJECT_REMOVED", "removed /PS key from form XObject", !ctx.opt.verifyOnly);
        }
        if (d.hasKey("/Subtype2")) d.removeKey("/Subtype2");
        if (d.hasKey("/OPI")) {
          d.removeKey("/OPI");
          ctx.issue("OPI_REMOVED", "removed /OPI from form XObject", true);
        }
        if (ctx.pdf14Target() && d.getKey("/Group").isDictionary() &&
            nameIs(d.getKey("/Group").getKey("/S"), "/Transparency")) {
          d.removeKey("/Group");
          ctx.issue("TRANSPARENCY_GROUP_REMOVED", "removed transparency group from form XObject", true);
        }
        fixResources(ctx, d.getKey("/Resources"), visited, flatten);
        if (ctx.failed()) return;
      }
    }
  }
  auto fixShadingBox = [&ctx](QPDFObjectHandle sh) {
    QPDFObjectHandle sd = sh.isStream() ? sh.getDict() : sh;
    if (!sd.isDictionary() || !sd.hasKey("/BBox")) return;
    QPDFObjectHandle b = sd.getKey("/BBox");
    bool ok = b.isArray() && b.getArrayNItems() == 4;
    for (int i = 0; ok && i < 4; ++i) ok = b.getArrayItem(i).isNumber();
    if (ok) return;
    sd.removeKey("/BBox");
    ctx.issue("SHADING_BBOX_REMOVED", "removed a malformed bounding box from a shading", true);
  };
  QPDFObjectHandle shd = res.getKey("/Shading");
  if (shd.isDictionary()) {
    for (const std::string& k : shd.getKeys()) fixShadingBox(shd.getKey(k));
  }
  QPDFObjectHandle pat = res.getKey("/Pattern");
  if (pat.isDictionary()) {
    for (const std::string& k : pat.getKeys()) {
      QPDFObjectHandle p = pat.getKey(k);
      if (unusedHere(p) || !visited.enter(p)) continue;
      QPDFObjectHandle pd = p.isStream() ? p.getDict() : p;
      if (!pd.isDictionary()) continue;
      fixShadingBox(pd.getKey("/Shading"));
      if (p.isStream() && pd.getKey("/PatternType").isInteger() && pd.getKey("/PatternType").getIntValue() == 1) {
        QPDFObjectHandle bb = pd.getKey("/BBox");
        bool bbOk = bb.isArray() && bb.getArrayNItems() == 4;
        for (int i = 0; bbOk && i < 4; ++i) bbOk = bb.getArrayItem(i).isNumber();
        bool fixedPattern = false;
        if (!bbOk) {
          pd.replaceKey("/BBox", QPDFObjectHandle::parse("[0 0 1 1]"));
          bb = pd.getKey("/BBox");
          fixedPattern = true;
        }
        double w = std::fabs(numOf(bb.getArrayItem(2), 1) - numOf(bb.getArrayItem(0), 0));
        double h = std::fabs(numOf(bb.getArrayItem(3), 1) - numOf(bb.getArrayItem(1), 0));
        auto need = [&](const char* key, QPDFObjectHandle v) {
          QPDFObjectHandle cur = pd.getKey(key);
          bool ok = std::string(key) == "/PaintType" || std::string(key) == "/TilingType"
                        ? cur.isInteger() && cur.getIntValue() >= 1 && cur.getIntValue() <= 3
                        : cur.isNumber() && numOf(cur, 0) != 0;
          if (ok) return;
          pd.replaceKey(key, v);
          fixedPattern = true;
        };
        need("/PaintType", QPDFObjectHandle::newInteger(1));
        need("/TilingType", QPDFObjectHandle::newInteger(1));
        need("/XStep", QPDFObjectHandle::newReal(w > 0 ? w : 1, 4));
        need("/YStep", QPDFObjectHandle::newReal(h > 0 ? h : 1, 4));
        if (fixedPattern) {
          ctx.issue("TILING_PATTERN_COMPLETED", "filled in required tiling-pattern entries for " + k, true);
        }
      }
      QPDFObjectHandle pgs = pd.getKey("/ExtGState");
      if (pgs.isDictionary() && !tPageUse) {
        fixGState(ctx, pgs);
        if (flatten) flattenGStateTransparency(ctx, pgs);
      }
      fixResources(ctx, pd.getKey("/Resources"), visited, flatten);
      if (ctx.failed()) return;
    }
  }
}

QPDFObjectHandle makeEmptyAppearance(Ctx& ctx, QPDFObjectHandle rect) {
  double w = 0, h = 0;
  if (rect.isArray() && rect.getArrayNItems() == 4) {
    double x1 = numOf(rect.getArrayItem(0), 0);
    double y1 = numOf(rect.getArrayItem(1), 0);
    double x2 = numOf(rect.getArrayItem(2), 0);
    double y2 = numOf(rect.getArrayItem(3), 0);
    w = std::fabs(x2 - x1);
    h = std::fabs(y2 - y1);
  }
  QPDFObjectHandle stream = QPDFObjectHandle::newStream(&ctx.pdf, "");
  QPDFObjectHandle d = stream.getDict();
  d.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
  d.replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
  QPDFObjectHandle bbox = QPDFObjectHandle::newArray();
  bbox.appendItem(QPDFObjectHandle::newInteger(0));
  bbox.appendItem(QPDFObjectHandle::newInteger(0));
  bbox.appendItem(QPDFObjectHandle::newReal(w, 2));
  bbox.appendItem(QPDFObjectHandle::newReal(h, 2));
  d.replaceKey("/BBox", bbox);
  return stream;
}

bool fieldTypeIsBtn(QPDFObjectHandle annot) {
  QPDFObjectHandle node = annot;
  for (int i = 0; i < 32 && node.isDictionary(); ++i) {
    QPDFObjectHandle ft = node.getKey("/FT");
    if (ft.isName()) return ft.getName() == "/Btn";
    node = node.getKey("/Parent");
  }
  return false;
}

std::string buttonStateName(QPDFObjectHandle annot) {
  std::string as = nameOf(annot.getKey("/AS"));
  if (!as.empty()) return as;
  QPDFObjectHandle node = annot;
  for (int i = 0; i < 32 && node.isDictionary(); ++i) {
    QPDFObjectHandle v = node.getKey("/V");
    if (v.isName()) return v.getName();
    node = node.getKey("/Parent");
  }
  return "/Off";
}

bool valid3DContent(Ctx& ctx, QPDFObjectHandle annot) {
  QPDFObjectHandle dd = annot.getKey("/3DD");
  if (dd.isDictionary() && dd.getKey("/3DD").isStream()) dd = dd.getKey("/3DD");
  if (!dd.isStream()) return ctx.opt.verifyOnly && dd.isNull() && !ctx.isE();
  std::string st = nameOf(dd.getDict().getKey("/Subtype"));
  if (ctx.isE()) return st == "/U3D";
  return st == "/U3D" || st == "/PRC";
}

const std::set<std::string> kPdf16OnlyExtra = {"/Polygon", "/PolyLine", "/Caret", "/Watermark"};

bool annotForbidden(Ctx& ctx, QPDFObjectHandle annot, const std::string& subtype) {
  if (subtype.empty()) return ctx.isA();
  if (ctx.isX()) {
    if (ctx.pdf20Print() && (subtype == "/TrapNet" || subtype == "/PrinterMark")) return true;
    return subtype != "/TrapNet" && subtype != "/PrinterMark";
  }
  if (subtype == "/Sound" || subtype == "/Movie" || subtype == "/Screen") return !(ctx.opt.verifyOnly && ctx.isE());
  if (ctx.opt.ua && subtype == "/TrapNet") return true;
  if (subtype == "/3D") return !ctx.allow3D() || !valid3DContent(ctx, annot);
  if (subtype == "/RichMedia") return ctx.conf != 'E';
  if (ctx.isE()) {
    if (subtype == "/FileAttachment") return false;
    return kPdf14Annots.count(subtype) == 0 && kPdf16OnlyExtra.count(subtype) == 0;
  }
  if (subtype == "/FileAttachment") return ctx.part >= 4 ? ctx.conf != 'F' : !ctx.allowEmbeddedFiles();
  if (ctx.part == 1) return kPdf14Annots.count(subtype) == 0;
  if (ctx.part >= 4) {
    if (subtype == "/TrapNet") return !ctx.opt.verifyOnly;
    return kPdf14Annots.count(subtype) == 0 && kPdf17ExtraAnnots.count(subtype) == 0 &&
           subtype != "/Projection";
  }
  return kPdf14Annots.count(subtype) == 0 && kPdf17ExtraAnnots.count(subtype) == 0;
}

bool zeroRect(QPDFObjectHandle r) {
  if (!r.isArray() || r.getArrayNItems() != 4) return false;
  for (int i = 0; i < 4; ++i) {
    if (!r.getArrayItem(i).isNumber()) return false;
  }
  return r.getArrayItem(0).getNumericValue() == r.getArrayItem(2).getNumericValue() &&
         r.getArrayItem(1).getNumericValue() == r.getArrayItem(3).getNumericValue();
}

void fixAnnotation(Ctx& ctx, QPDFObjectHandle annot) {
  std::string subtype = nameOf(annot.getKey("/Subtype"));
  if (annot.hasKey("/A")) {
    QPDFObjectHandle a = annot.getKey("/A");
    std::string s = a.isDictionary() ? nameOf(a.getKey("/S")) : std::string();
    if ((subtype == "/Widget" && a.isDictionary()) || !actionAllowed(ctx, a)) {
      annot.removeKey("/A");
      ctx.issue("ACTION_REMOVED",
                subtype == "/Widget"
                    ? "removed action from Widget annotation (widgets may not have /A)"
                    : "removed forbidden action " + s + " from annotation",
                true);
    } else if (a.hasKey("/Next") && !nextChainAllowed(ctx, a, 0)) {
      a.removeKey("/Next");
      ctx.issue("ACTION_CHAIN_REMOVED", "removed /Next action chain in annotation", true);
    }
  }
  sanitizeAdditionalActions(ctx, annot, "annotation " + subtype, subtype == "/Widget");
  long long f = annot.getKey("/F").isInteger() ? annot.getKey("/F").getIntValue() : 0;
  long long fixedF = (f | 4LL) & ~(1LL | 2LL | 32LL | 256LL);
  if (ctx.isE() && subtype == "/Text") fixedF |= 8LL | 16LL;
  if (fixedF != f || !annot.getKey("/F").isInteger()) {
    bool violation = !ctx.opt.verifyOnly || subtype != "/Popup";
    if (ctx.opt.verifyOnly && ctx.isA()) {
      QPDFObjectHandle fo = annot.getKey("/F");
      long long fv = fo.isNumber() ? static_cast<long long>(fo.getNumericValue()) : 0;
      long long badBits = 1LL | 2LL | 32LL | (ctx.part >= 2 ? 256LL : 0LL);
      if (fo.isNumber()) {
        violation = !(fv & 4LL) || (fv & badBits);
      } else {
        violation = ctx.part == 1 || subtype != "/Popup";
      }
    }
    annot.replaceKey("/F", QPDFObjectHandle::newInteger(fixedF));
    ctx.issue("ANNOT_FLAGS_FIXED", "normalized annotation flags on " + subtype, violation);
  }
  if (ctx.isE() && subtype == "/FileAttachment") {
    QPDFObjectHandle fs = annot.getKey("/FS");
    if (fs.isDictionary() && fs.hasKey("/EF")) {
      fs.removeKey("/EF");
      ctx.issue("EMBEDDED_FILE_REMOVED", "removed the embedded file from a file attachment annotation", true);
    }
  }
  if (ctx.isE() && subtype == "/3D") {
    QPDFObjectHandle dd = annot.getKey("/3DD");
    if (dd.isDictionary() && !dd.isStream() && dd.getKey("/3DD").isStream()) dd = dd.getKey("/3DD");
    if (dd.isStream() && dd.getDict().hasKey("/Resources")) {
      dd.getDict().removeKey("/Resources");
      ctx.issue("MODEL_RESOURCES_REMOVED", "removed the resource dictionary from a 3D stream", true);
    }
  }
  if (ctx.pdf14Target() && (numOf(annot.getKey("/CA"), 1.0) < 1.0 || (ctx.isA() && numOf(annot.getKey("/CA"), 1.0) > 1.0))) {
    annot.replaceKey("/CA", QPDFObjectHandle::newInteger(1));
    ctx.issue("TRANSPARENCY_FLATTENED", "forced annotation /CA to 1 (visual risk)", true);
  }
  if (ctx.pdf14Target() && annot.getKey("/AP").isDictionary()) {
    std::vector<QPDFObjectHandle> streams;
    for (const char* k : {"/N", "/D", "/R"}) {
      QPDFObjectHandle v = annot.getKey("/AP").getKey(k);
      if (v.isStream()) streams.push_back(v);
      for (const std::string& s : v.isDictionary() && !v.isStream() ? v.getKeys() : std::set<std::string>()) {
        if (v.getKey(s).isStream()) streams.push_back(v.getKey(s));
      }
    }
    std::set<QPDFObjGen> seenAp;
    for (size_t i = 0; i < streams.size() && i < 256; ++i) {
      if (streams[i].isIndirect() && !seenAp.insert(streams[i].getObjGen()).second) continue;
      QPDFObjectHandle res = streams[i].getDict().getKey("/Resources");
      if (!res.isDictionary()) continue;
      QPDFObjectHandle gsd = res.getKey("/ExtGState");
      for (const std::string& g : gsd.isDictionary() ? gsd.getKeys() : std::set<std::string>()) {
        flattenGStateTransparency(ctx, gsd.getKey(g));
      }
      QPDFObjectHandle xo = res.getKey("/XObject");
      for (const std::string& x : xo.isDictionary() ? xo.getKeys() : std::set<std::string>()) {
        if (xo.getKey(x).isStream() && nameIs(xo.getKey(x).getDict().getKey("/Subtype"), "/Form")) streams.push_back(xo.getKey(x));
      }
    }
  }
  if (ctx.pdf14Target() && annot.hasKey("/OC")) {
    annot.removeKey("/OC");
    ctx.issue("OPTIONAL_CONTENT_REMOVED", "removed /OC from annotation", true);
  }
  QPDFObjectHandle ap = annot.getKey("/AP");
  if (ap.isDictionary()) {
    if (!ap.hasKey("/N")) {
      for (const char* alt : {"/D", "/R"}) {
        QPDFObjectHandle cand = ap.getKey(alt);
        if (cand.isStream() || cand.isDictionary()) {
          ap.replaceKey("/N", cand);
          break;
        }
      }
    }
    bool removedExtra = false;
    for (const std::string& key : ap.getKeys()) {
      if (key != "/N") {
        ap.removeKey(key);
        removedExtra = true;
      }
    }
    if (removedExtra) {
      ctx.issue("APPEARANCE_EXTRA_REMOVED",
                "reduced appearance dictionary to /N only on " + subtype, true);
    }
    if (!ap.hasKey("/N")) {
      ap.replaceKey("/N",
                    ctx.pdf.makeIndirectObject(makeEmptyAppearance(ctx, annot.getKey("/Rect"))));
      ctx.issue("APPEARANCE_ADDED", "synthesized empty appearance stream for " + subtype, true);
    }
    bool isBtn = subtype == "/Widget" && fieldTypeIsBtn(annot);
    QPDFObjectHandle n = ap.getKey("/N");
    bool anyState = false;
    for (const std::string& k : n.isDictionary() && !n.isStream() ? n.getKeys() : std::set<std::string>()) {
      if (n.getKey(k).isStream()) anyState = true;
    }
    if (isBtn && !n.isStream() && !anyState) {
      std::string state = buttonStateName(annot);
      QPDFObjectHandle states = QPDFObjectHandle::newDictionary();
      states.replaceKey(state, ctx.pdf.makeIndirectObject(makeEmptyAppearance(ctx, annot.getKey("/Rect"))));
      ap.replaceKey("/N", states);
      annot.replaceKey("/AS", QPDFObjectHandle::newName(state));
      ctx.issue("APPEARANCE_STATES_WRAPPED", "gave a button widget the appearance state it lacked", true);
    } else if (!isBtn && !n.isStream() && !n.isDictionary()) {
      ap.replaceKey("/N", ctx.pdf.makeIndirectObject(makeEmptyAppearance(ctx, annot.getKey("/Rect"))));
      ctx.issue("APPEARANCE_ADDED", "replaced an unusable normal appearance on " + subtype, true);
    } else if (isBtn && n.isStream()) {
      std::string state = buttonStateName(annot);
      QPDFObjectHandle states = QPDFObjectHandle::newDictionary();
      states.replaceKey(state, n);
      if (state != "/Off") {
        states.replaceKey("/Off", ctx.pdf.makeIndirectObject(
                                      makeEmptyAppearance(ctx, annot.getKey("/Rect"))));
      }
      ap.replaceKey("/N", states);
      annot.replaceKey("/AS", QPDFObjectHandle::newName(state));
      ctx.issue("APPEARANCE_STATES_WRAPPED",
                "wrapped button widget appearance stream into a state subdictionary", true);
    } else if (!isBtn && n.isDictionary() && !n.isStream()) {
      QPDFObjectHandle pick;
      std::string as = nameOf(annot.getKey("/AS"));
      if (!as.empty() && n.getKey(as).isStream()) {
        pick = n.getKey(as);
      } else {
        for (const std::string& k : n.getKeys()) {
          if (n.getKey(k).isStream()) {
            pick = n.getKey(k);
            break;
          }
        }
      }
      if (!pick.isInitialized()) {
        pick = ctx.pdf.makeIndirectObject(makeEmptyAppearance(ctx, annot.getKey("/Rect")));
      }
      ap.replaceKey("/N", pick);
      if (annot.hasKey("/AS")) annot.removeKey("/AS");
      ctx.issue("APPEARANCE_FLATTENED",
                "reduced appearance state subdictionary to a single stream on " + subtype, true);
    }
    n = ap.getKey("/N");
    if (n.isDictionary() && !n.isStream()) {
      std::string as = nameOf(annot.getKey("/AS"));
      if (as.empty() || !n.hasKey(as)) {
        auto keys = n.getKeys();
        if (!keys.empty()) {
          annot.replaceKey("/AS", QPDFObjectHandle::newName(*keys.begin()));
          ctx.issue("APPEARANCE_STATE_ADDED", "added missing /AS for " + subtype, true);
        }
      }
    }
  } else if (ctx.opt.verifyOnly ? (ctx.isA() && ctx.part == 1 ? subtype == "/Widget"
                                                              : subtype != "/Popup" && subtype != "/Link" &&
                                                                    subtype != "/Projection" && !zeroRect(annot.getKey("/Rect")) &&
                                                                    !(ctx.isE() && (subtype == "/Sound" || subtype == "/Movie" || subtype == "/Screen")))
                                 : subtype != "/Popup" && subtype != "/Link" && subtype != "/Projection") {
    QPDFObjectHandle n = makeEmptyAppearance(ctx, annot.getKey("/Rect"));
    QPDFObjectHandle newAp = QPDFObjectHandle::newDictionary();
    if (subtype == "/Widget" && fieldTypeIsBtn(annot)) {
      std::string state = buttonStateName(annot);
      QPDFObjectHandle states = QPDFObjectHandle::newDictionary();
      states.replaceKey(state, ctx.pdf.makeIndirectObject(n));
      if (state != "/Off") {
        states.replaceKey("/Off", ctx.pdf.makeIndirectObject(
                                      makeEmptyAppearance(ctx, annot.getKey("/Rect"))));
      }
      newAp.replaceKey("/N", states);
      annot.replaceKey("/AS", QPDFObjectHandle::newName(state));
    } else {
      newAp.replaceKey("/N", ctx.pdf.makeIndirectObject(n));
    }
    annot.replaceKey("/AP", newAp);
    ctx.issue("APPEARANCE_ADDED", "synthesized empty appearance stream for " + subtype, true);
  }
}

bool numberBox(QPDFObjectHandle b, double out[4]) {
  if (!b.isArray() || b.getArrayNItems() != 4) return false;
  for (int i = 0; i < 4; ++i) {
    if (!b.getArrayItem(i).isNumber()) return false;
    out[i] = b.getArrayItem(i).getNumericValue();
  }
  if (out[0] > out[2]) std::swap(out[0], out[2]);
  if (out[1] > out[3]) std::swap(out[1], out[3]);
  return true;
}

bool overlapsFinishedArea(QPDFPageObjectHelper ph, QPDFObjectHandle annot) {
  double r[4], a[4];
  if (!numberBox(annot.getKey("/Rect"), r)) return false;
  QPDFObjectHandle page = ph.getObjectHandle();
  bool found = false;
  for (const char* k : {"/BleedBox", "/TrimBox", "/ArtBox"}) {
    if (numberBox(page.getKey(k), a)) {
      found = true;
      if (r[0] < a[2] && r[2] > a[0] && r[1] < a[3] && r[3] > a[1]) return true;
    }
  }
  if (found) return false;
  if (!numberBox(ph.getAttribute("/CropBox", false), a) && !numberBox(ph.getAttribute("/MediaBox", false), a)) return false;
  return r[0] < a[2] && r[2] > a[0] && r[1] < a[3] && r[3] > a[1];
}

bool trapNetUsable(QPDFObjectHandle annot) {
  bool modified = annot.hasKey("/LastModified");
  bool versioned = annot.getKey("/Version").isArray() && annot.getKey("/AnnotStates").isArray();
  return modified != versioned;
}

bool trapNetModelMismatch(Ctx& ctx, QPDFObjectHandle annot) {
  QPDFObjectHandle n = QPDFAnnotationObjectHelper(annot).getAppearanceStream("/N");
  if (!n.isStream() || !n.getDict().getKey("/PCM").isName()) return false;
  std::string pcm = n.getDict().getKey("/PCM").getName();
  if (ctx.opt.level == Level::X5N || ctx.opt.level == Level::X6N) return pcm != "/DeviceN";
  QPDFObjectHandle ois = ctx.pdf.getRoot().getKey("/OutputIntents");
  std::string space;
  for (int i = 0; ois.isArray() && i < ois.getArrayNItems() && space.empty(); ++i) {
    QPDFObjectHandle oi = ois.getArrayItem(i);
    if (!nameIs(oi.getKey("/S"), "/GTS_PDFX")) continue;
    QPDFObjectHandle prof = oi.getKey("/DestOutputProfile");
    if (prof.isStream()) {
      try {
        auto buf = prof.getStreamData(qpdf_dl_all);
        if (buf->getSize() >= 20) space.assign(reinterpret_cast<const char*>(buf->getBuffer()) + 16, 4);
      } catch (...) {
      }
    } else if (oi.getKey("/DestOutputProfileRef").isDictionary() &&
               oi.getKey("/DestOutputProfileRef").getKey("/ProfileCS").isString()) {
      space = oi.getKey("/DestOutputProfileRef").getKey("/ProfileCS").getStringValue();
    } else {
      space = "CMYK";
    }
  }
  if (space.empty()) return false;
  if (pcm == "/DeviceCMYK") return space != "CMYK";
  if (pcm == "/DeviceRGB") return space != "RGB ";
  if (pcm == "/DeviceGray") return space != "GRAY";
  return pcm != "/DeviceN" && pcm != "/DeviceRGBK" && pcm != "/DeviceCMY";
}

bool bakeAnnotationIntoPage(Ctx& ctx, QPDFPageObjectHelper& ph, QPDFObjectHandle a, const std::string& subtype) {
  long long flags = a.getKey("/F").isInteger() ? a.getKey("/F").getIntValue() : 0;
  if (flags & (an_invisible | an_hidden | an_no_view)) return false;
  QPDFObjectHandle page = ph.getObjectHandle();
  QPDFObjectHandle res = ph.getAttribute("/Resources", true);
  if (!res.isDictionary()) return false;
  std::string content;
  QPDFAnnotationObjectHelper aoh(a);
  if (aoh.getAppearanceStream("/N").isStream()) {
    if (!res.getKey("/XObject").isDictionary()) res.replaceKey("/XObject", QPDFObjectHandle::newDictionary());
    int n = 0;
    std::string name = res.getUniqueResourceName("/KuraAn", n);
    int rotate = page.getKey("/Rotate").isInteger() ? static_cast<int>(page.getKey("/Rotate").getIntValue()) : 0;
    content = aoh.getPageContentForAppearance(name, rotate, 0, an_invisible | an_hidden | an_no_view);
    if (!content.empty()) {
      QPDFObjectHandle apn = aoh.getAppearanceStream("/N");
      if (!apn.getDict().getKey("/Resources").isDictionary()) apn.getDict().replaceKey("/Resources", QPDFObjectHandle::newDictionary());
      res.getKey("/XObject").replaceKey(name, apn);
    }
  } else if (subtype == "/Polygon" || subtype == "/PolyLine") {
    QPDFObjectHandle v = a.getKey("/Vertices");
    QPDFObjectHandle c = a.getKey("/C");
    if (!v.isArray() || v.getArrayNItems() < 4 || !c.isArray()) return false;
    auto colour = [](QPDFObjectHandle arr, bool stroke) {
      std::string s;
      for (int i = 0; i < arr.getArrayNItems(); ++i) s += std::to_string(arr.getArrayItem(i).getNumericValue()) + " ";
      int n = arr.getArrayNItems();
      if (n == 1) return s + (stroke ? "G\n" : "g\n");
      if (n == 3) return s + (stroke ? "RG\n" : "rg\n");
      if (n == 4) return s + (stroke ? "K\n" : "k\n");
      return std::string();
    };
    std::string sc = colour(c, true);
    if (sc.empty()) return false;
    double w = 1;
    QPDFObjectHandle bs = a.getKey("/BS");
    if (bs.isDictionary() && bs.getKey("/W").isNumber()) w = bs.getKey("/W").getNumericValue();
    else if (a.getKey("/Border").isArray() && a.getKey("/Border").getArrayNItems() >= 3 && a.getKey("/Border").getArrayItem(2).isNumber())
      w = a.getKey("/Border").getArrayItem(2).getNumericValue();
    if (w <= 0) return false;
    std::string path;
    for (int i = 0; i + 1 < v.getArrayNItems(); i += 2) {
      if (!v.getArrayItem(i).isNumber() || !v.getArrayItem(i + 1).isNumber()) return false;
      path += std::to_string(v.getArrayItem(i).getNumericValue()) + " " + std::to_string(v.getArrayItem(i + 1).getNumericValue()) + (i ? " l\n" : " m\n");
    }
    bool closed = subtype == "/Polygon";
    std::string fc = closed && a.getKey("/IC").isArray() ? colour(a.getKey("/IC"), false) : std::string();
    content = "q\n" + sc + fc + std::to_string(w) + " w 1 j\n" + path + (closed ? (fc.empty() ? "s\n" : "b\n") : "S\n") + "Q\n";
  }
  if (content.empty()) return false;
  ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, "q\n"), true);
  ph.addPageContents(QPDFObjectHandle::newStream(&ctx.pdf, "\nQ\n" + content), false);
  return true;
}

void fixPageAnnotations(Ctx& ctx, QPDFObjectHandle page) {
  QPDFObjectHandle annots = page.getKey("/Annots");
  if (!annots.isArray()) return;
  QPDFPageObjectHelper ph(page);
  QPDFObjectHandle kept = QPDFObjectHandle::newArray();
  bool removedAny = false;
  for (int i = 0; i < annots.getArrayNItems(); ++i) {
    QPDFObjectHandle a = annots.getArrayItem(i);
    if (!a.isDictionary()) {
      removedAny = true;
      continue;
    }
    std::string subtype = nameOf(a.getKey("/Subtype"));
    if (ctx.isX() && ctx.opt.verifyOnly && subtype != "/TrapNet" && subtype != "/PrinterMark") {
      bool acceptable = false;
      if (ctx.pdf20Print()) {
        long long fl = a.getKey("/F").isInteger() ? a.getKey("/F").getIntValue() : 0;
        bool printable = (fl & an_print) && !(fl & (an_invisible | an_hidden | an_no_view));
        bool known = kPdf14Annots.count(subtype) || kPdf17ExtraAnnots.count(subtype) || subtype == "/Projection" ||
                     (subtype == "/Screen" && !a.hasKey("/A") && !a.hasKey("/AA"));
        acceptable = printable && known && QPDFAnnotationObjectHelper(a).getAppearanceStream("/N").isStream();
      } else {
        static const std::set<std::string> media = {"/3D", "/Movie", "/Sound", "/Screen", "/RichMedia"};
        acceptable = !media.count(subtype) && !overlapsFinishedArea(ph, a);
        if (acceptable) {
          QPDFObjectHandle act = a.getKey("/A");
          if (act.isDictionary() && !actionAllowed(ctx, act)) {
            ctx.issue("ACTION_REMOVED", "removed forbidden action " + nameOf(act.getKey("/S")) + " from annotation", true);
          }
          sanitizeAdditionalActions(ctx, a, "annotation " + subtype, subtype == "/Widget");
          kept.appendItem(a);
          continue;
        }
      }
      if (acceptable) {
        fixAnnotation(ctx, a);
        kept.appendItem(a);
        continue;
      }
    }
    if (annotForbidden(ctx, a, subtype)) {
      if (!ctx.opt.verifyOnly && bakeAnnotationIntoPage(ctx, ph, a, subtype)) {
        ctx.issue("ANNOTATION_DRAWN_INTO_PAGE", "drew the visible appearance of a " + subtype +
                  " annotation into the page content before removing the annotation", true);
      }
      ctx.issue("ANNOTATION_REMOVED", "removed forbidden annotation " + subtype, true);
      removedAny = true;
      continue;
    }
    if (ctx.isX() && subtype == "/PrinterMark" && overlapsFinishedArea(ph, a)) {
      ctx.issue("ANNOTATION_REMOVED", "removed a printer's mark annotation that lies on the finished page area", true);
      removedAny = true;
      continue;
    }
    if (ctx.isX() && subtype == "/TrapNet") {
      if (!trapNetUsable(a) || (ctx.opt.verifyOnly && trapNetModelMismatch(ctx, a))) {
        ctx.issue("ANNOTATION_REMOVED", "removed a trap network annotation without valid modification information", true);
        removedAny = true;
        continue;
      }
      if (a.hasKey("/FontFauxing")) {
        a.removeKey("/FontFauxing");
        ctx.issue("TRAPNET_FONT_SUBSTITUTION_REMOVED", "removed the font substitution list from a trap network annotation", true);
      }
    }
    fixAnnotation(ctx, a);
    kept.appendItem(a);
  }
  if (removedAny) page.replaceKey("/Annots", kept);
}
}

namespace {
QPDFObjectHandle emptyFormLike(Ctx& ctx) {
  QPDFObjectHandle f = ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, std::string()));
  QPDFObjectHandle d = f.getDict();
  d.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
  d.replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
  d.replaceKey("/BBox", QPDFObjectHandle::parse("[0 0 0 0]"));
  d.replaceKey("/Resources", QPDFObjectHandle::newDictionary());
  return f;
}

QPDFObjectHandle emptyPatternLike(Ctx& ctx) {
  QPDFObjectHandle p = ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, std::string()));
  p.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/Pattern"));
  p.getDict().replaceKey("/PatternType", QPDFObjectHandle::newInteger(1));
  p.getDict().replaceKey("/PaintType", QPDFObjectHandle::newInteger(1));
  p.getDict().replaceKey("/TilingType", QPDFObjectHandle::newInteger(1));
  p.getDict().replaceKey("/BBox", QPDFObjectHandle::parse("[0 0 1 1]"));
  p.getDict().replaceKey("/XStep", QPDFObjectHandle::newInteger(1));
  p.getDict().replaceKey("/YStep", QPDFObjectHandle::newInteger(1));
  p.getDict().replaceKey("/Resources", QPDFObjectHandle::newDictionary());
  return p;
}

std::set<std::string> drawnNames(QPDFObjectHandle st) {
  std::set<std::string> names;
  std::string data;
  try {
    auto buf = st.getStreamData(qpdf_dl_generalized);
    data.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return names;
  }
  std::string last;
  size_t i = 0, n = data.size();
  auto delim = [](char c) { return std::isspace(static_cast<unsigned char>(c)) || std::strchr("()<>[]{}/%", c); };
  while (i < n) {
    char c = data[i];
    if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
    if (c == '%') { while (i < n && data[i] != '\n' && data[i] != '\r') ++i; continue; }
    if (c == '/') {
      size_t j = i + 1;
      while (j < n && !delim(data[j])) ++j;
      last = "/" + data.substr(i + 1, j - i - 1);
      i = j;
      continue;
    }
    size_t j = i;
    while (j < n && !delim(data[j])) ++j;
    if (j == i) { ++i; continue; }
    std::string tok = data.substr(i, j - i);
    if ((tok == "Do" || tok == "scn" || tok == "SCN") && !last.empty()) names.insert(last);
    if (tok != "Do" && tok != "scn" && tok != "SCN") last.clear();
    i = j;
  }
  return names;
}

void cutResourceCycles(Ctx& ctx, QPDFObjectHandle res, const std::set<std::string>* drawn,
                       std::set<QPDFObjGen>& onPath, std::set<QPDFObjGen>& done, int& cut, int depth,
                       QPDFObjectHandle owner = QPDFObjectHandle()) {
  if (!res.isDictionary() || depth > kMaxResourceNest) return;
  bool privateRes = !owner.isInitialized();
  for (const char* cat : {"/XObject", "/Pattern"}) {
    QPDFObjectHandle dict = res.getKey(cat);
    if (!dict.isDictionary()) continue;
    bool privateDict = privateRes && !dict.isIndirect();
    for (const std::string& k : dict.getKeys()) {
      if (drawn && !drawn->count(k)) continue;
      QPDFObjectHandle child = dict.getKey(k);
      if (!child.isStream() || !child.isIndirect()) continue;
      QPDFObjGen og = child.getObjGen();
      if (onPath.count(og)) {
        if (!privateRes) {
          res = res.shallowCopy();
          owner.getDict().replaceKey("/Resources", res);
          privateRes = true;
        }
        if (!privateDict) {
          dict = dict.shallowCopy();
          res.replaceKey(cat, dict);
          privateDict = true;
        }
        dict.replaceKey(k, std::string(cat) == "/XObject" ? emptyFormLike(ctx) : emptyPatternLike(ctx));
        ++cut;
        continue;
      }
      if (done.count(og)) continue;
      onPath.insert(og);
      std::set<std::string> inner = drawnNames(child);
      cutResourceCycles(ctx, child.getDict().getKey("/Resources"), &inner, onPath, done, cut, depth + 1, child);
      onPath.erase(og);
      done.insert(og);
    }
  }
}
}

void passPages(Ctx& ctx) {
  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
  GraphicsUse use;
  if (ctx.opt.verifyOnly) {
    use = collectGraphicsUse(ctx.pdf);
    tPageUse = &use;
  }
  struct UseReset {
    ~UseReset() { tPageUse = nullptr; }
  } useReset;
  {
    std::set<QPDFObjGen> onPath, done;
    int cut = 0;
    for (auto& ph : pages) cutResourceCycles(ctx, ph.getAttribute("/Resources", false), nullptr, onPath, done, cut, 0);
    if (cut) {
      ctx.issue("RESOURCE_CYCLE_CUT",
                "broke " + std::to_string(cut) + " resource loop(s) in which a form or pattern draws itself; "
                "the repeated drawing was replaced by an empty one", true);
    }
  }

  if (ctx.opt.rasterizeAllPages || !ctx.opt.rasterPages.empty()) {
    if (!ctx.opt.rasterizePage) {
      ctx.fatal("RASTERIZER_UNAVAILABLE",
                "--rasterize-pages needs a renderer; this build has none linked in");
      return;
    }
    int rastered = 0, wanted = 0;
    for (size_t i = 0; i < pages.size(); ++i) {
      if (!ctx.opt.rasterizeAllPages && !ctx.opt.rasterPages.count(static_cast<int>(i) + 1)) continue;
      ++wanted;
      PageScope kuraScope(ctx, static_cast<int>(i) + 1);
      if (rasterFlattenPage(ctx, pages[i], static_cast<int>(i), ctx.opt.rasterKeepText)) ++rastered;
    }
    if (rastered != wanted) {
      ctx.fatal("RASTERIZE_FAILED",
                "could not render " + std::to_string(wanted - rastered) + " of " +
                    std::to_string(wanted) + " page(s)");
      return;
    }
    ctx.issue("PAGES_RASTERIZED",
              "rasterized " + std::string(ctx.opt.rasterizeAllPages ? "all " : "") + std::to_string(rastered) + " page(s) at " +
                  std::to_string(static_cast<int>(ctx.opt.rasterDpi)) +
                  (ctx.opt.rasterKeepText ? " dpi on request; original text kept as invisible text"
                                          : " dpi on request; vector text and its searchability are gone"),
              true);
  }

  bool flatten = false;
  if (ctx.transparencyBanned()) {
    TransparencyReport rep = scanTransparency(ctx, pages);
    if (rep.real) {
      int vec = 0, rgnPages = 0, rgnCount = 0;
      bool allVec = true;
      for (size_t i = 0; i < pages.size() && i < rep.pages.size(); ++i) {
        if (!rep.pages[i]) continue;
        PageScope kuraScope(ctx, static_cast<int>(i) + 1);
        int rg = 0;
        bakeTransparentAppearances(ctx, pages[i]);
        if (vectorFlattenPage(ctx, pages[i], static_cast<int>(i), &rg)) {
          rep.pages[i] = false;
          ++vec;
          if (rg) {
            ++rgnPages;
            rgnCount += rg;
          }
        } else {
          allVec = false;
        }
      }
      if (rgnPages) {
        ctx.issue("TRANSPARENCY_REGIONS_RASTERIZED",
                  "rasterized " + std::to_string(rgnCount) + " transparent region(s) on " + std::to_string(rgnPages) +
                      " page(s) at " + std::to_string(static_cast<int>(ctx.opt.rasterDpi)) +
                      " dpi, each clipped to its object outline; everything else on those pages stays vector",
                  true);
      }
      if (vec) {
        ctx.issue("TRANSPARENCY_FLATTENED_VECTOR",
                  "flattened transparency on " + std::to_string(vec) +
                      " page(s) by pre-blending colours and splitting overlaps into clipped vector pieces; text and vector art stay vector",
                  true);
      }
      if (allVec) rep.real = false;
    }
    if (rep.real && ctx.opt.rasterizePage) {
      int rastered = 0;
      bool allOk = true;
      for (size_t i = 0; i < pages.size(); ++i) {
        PageScope kuraScope(ctx, static_cast<int>(i) + 1);
        if (i < rep.pages.size() && rep.pages[i]) {
          if (regionFlattenPage(ctx, pages[i], static_cast<int>(i)) ||
              rasterFlattenPage(ctx, pages[i], static_cast<int>(i), true)) {
            ++rastered;
          } else {
            allOk = false;
          }
        }
      }
      if (rastered) {
        ctx.issue("TRANSPARENCY_RASTERIZED",
                  "rasterized " + std::to_string(rastered) +
                      " page(s) containing transparency at " +
                      std::to_string(static_cast<int>(ctx.opt.rasterDpi)) +
                      " dpi (renderer-faithful flattening; the original text is kept as an invisible layer so the page stays searchable)",
                  true);
      }
      if (allOk) rep.real = false;
    }
    if (rep.real) {
      if (!ctx.opt.allowVisualRisk) {
        ctx.fatal("TRANSPARENCY_P1",
                  "document uses real transparency (" + rep.where.front() + ") which " +
                      (ctx.isX() ? "PDF/X-1a/X-3 forbid; target PDF/X-4"
                                 : "PDF/A-1 forbids; target PDF/A-2 or 3") +
                      ", or enable allowVisualRisk");
        return;
      }
      flatten = true;
    }
  }
  bool badInline = false;
  if (ctx.isA() && !ctx.opt.verifyOnly && !ctx.pdf14Target()) {
    for (QPDFObjectHandle ii : collectGraphicsUse(ctx.pdf).inlineImages) badInline = badInline || !imageBitsAllowed(ii, false);
  }
  if (ctx.pdf14Target() || badInline) {
    externalizeInlineImagesOnce(pages, 1);
    if (ctx.opt.verifyOnly) use = collectGraphicsUse(ctx.pdf);
  }

  Visited visited;
  int kuraPageNoA = 0;
  for (auto& ph : pages) {
    PageScope kuraScopeA(ctx, ++kuraPageNoA);
    QPDFObjectHandle page = ph.getObjectHandle();
    if (page.hasKey("/AA")) {
      bool bad = !ctx.opt.verifyOnly || aaForbidden(ctx, page.getKey("/AA"), "page");
      page.removeKey("/AA");
      ctx.issue("ADDITIONAL_ACTIONS_REMOVED", "removed /AA from page", bad);
    }
    if (page.hasKey("/PresSteps")) {
      page.removeKey("/PresSteps");
      if (ctx.isX() || (ctx.isA() && ctx.part >= 2)) ctx.issue("PRESENTATION_STEPS_REMOVED", "removed presentation steps from page", true);
    }
    if (ctx.isX() && page.hasKey("/SeparationInfo")) {
      page.removeKey("/SeparationInfo");
      ctx.issue("SEPARATION_INFO_REMOVED", "removed separation information: a print exchange page is a composite, not one colour plate", true);
    }
    if (ctx.pdf14Target() && page.getKey("/UserUnit").isNumber()) applyUserUnit(ctx, ph);
    if (ctx.isA() && ctx.part == 3) moveMarkedContentAf(ctx, ph);
    if (ctx.pdf14Target() && page.getKey("/Group").isDictionary() &&
        nameIs(page.getKey("/Group").getKey("/S"), "/Transparency")) {
      page.removeKey("/Group");
      ctx.issue("TRANSPARENCY_GROUP_REMOVED", "removed transparency group from page", true);
    }
    fixResources(ctx, page.getKey("/Resources"), visited, flatten);
    if (ctx.failed()) return;
    fixPageAnnotations(ctx, page);
    if (tPageUse && ctx.isA() && ctx.part >= 4 && kuraPageNoA == 1 && use.glyphUndefined) {
      ctx.issue("DANGLING_REFS_REMOVED", "a Type 3 glyph description names a resource that no resource dictionary defines", true);
    }
    if (tPageUse && ctx.isA() && kuraPageNoA == 1) {
      for (QPDFObjectHandle ii : use.inlineImages) {
        if (!imageBitsAllowed(ii, false)) {
          ctx.issue("IMAGE_DEPTH_NORMALIZED", "an inline image uses an unsupported bit depth", true);
        }
      }
    }
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (annots.isArray()) {
      for (int i = 0; i < annots.getArrayNItems(); ++i) {
        QPDFObjectHandle a = annots.getArrayItem(i);
        if (!a.isDictionary()) continue;
        std::vector<QPDFObjectHandle> streams = normalAppearanceStreams(a);
        for (QPDFObjectHandle s : streams) {
          QPDFObjectHandle d = s.getDict();
          if (ctx.pdf14Target() && d.getKey("/Group").isDictionary() &&
              nameIs(d.getKey("/Group").getKey("/S"), "/Transparency")) {
            d.removeKey("/Group");
            ctx.issue("TRANSPARENCY_GROUP_REMOVED", "removed transparency group from appearance", true);
          }
          fixResources(ctx, d.getKey("/Resources"), visited, flatten);
          if (ctx.failed()) return;
        }
      }
    }
  }
}
}
