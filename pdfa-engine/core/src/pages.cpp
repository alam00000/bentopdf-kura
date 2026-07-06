#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "ctx.hh"
#include "images.hh"
#include "passes.hh"
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

struct TransparencyReport {
  bool real = false;
  std::vector<std::string> where;
  std::vector<bool> pages;
};

bool gsHasTransparency(QPDFObjectHandle gs) {
  if (!gs.isDictionary()) return false;
  QPDFObjectHandle sm = gs.getKey("/SMask");
  if (!sm.isNull() && !nameIs(sm, "/None")) return true;
  std::string bm = nameOf(gs.getKey("/BM"));
  if (gs.getKey("/BM").isArray() && gs.getKey("/BM").getArrayNItems() > 0) {
    bm = nameOf(gs.getKey("/BM").getArrayItem(0));
  }
  if (!bm.empty() && bm != "/Normal" && bm != "/Compatible") return true;
  if (numOf(gs.getKey("/CA"), 1.0) < 1.0) return true;
  if (numOf(gs.getKey("/ca"), 1.0) < 1.0) return true;
  return false;
}

void scanResources(QPDFObjectHandle res, Visited& visited, TransparencyReport& rep) {
  if (!res.isDictionary() || !visited.enter(res)) return;
  QPDFObjectHandle gsd = res.getKey("/ExtGState");
  if (gsd.isDictionary()) {
    for (const std::string& k : gsd.getKeys()) {
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
      if (!xo.isStream() || !visited.enter(xo)) continue;
      QPDFObjectHandle d = xo.getDict();
      std::string subtype = nameOf(d.getKey("/Subtype"));
      if (subtype == "/Form") {
        scanResources(d.getKey("/Resources"), visited, rep);
      }
    }
  }
  QPDFObjectHandle pat = res.getKey("/Pattern");
  if (pat.isDictionary()) {
    for (const std::string& k : pat.getKeys()) {
      QPDFObjectHandle p = pat.getKey(k);
      if (p.isStream() && visited.enter(p)) {
        scanResources(p.getDict().getKey("/Resources"), visited, rep);
      }
    }
  }
}

TransparencyReport scanTransparency(Ctx& ctx, std::vector<QPDFPageObjectHelper>& pages) {
  TransparencyReport rep;
  Visited visited;
  for (auto& page : pages) {
    size_t before = rep.where.size();
    bool hadReal = rep.real;
    rep.real = false;
    scanResources(page.getObjectHandle().getKey("/Resources"), visited, rep);
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
}

namespace {
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
  if (depth > 24) return;
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
  if (!(ctx.isA() && ctx.part >= 2)) return;
  QPDFPageDocumentHelper dh(ctx.pdf);
  Visited visited;
  int fixedEntries = 0;
  for (auto& ph : dh.getAllPages()) {
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
        QPDFObjectHandle ap = a.getKey("/AP");
        if (!ap.isDictionary()) continue;
        QPDFObjectHandle nap = ap.getKey("/N");
        std::vector<QPDFObjectHandle> streams;
        if (nap.isStream()) streams.push_back(nap);
        else if (nap.isDictionary()) {
          for (const std::string& k : nap.getKeys()) {
            if (nap.getKey(k).isStream()) streams.push_back(nap.getKey(k));
          }
        }
        for (QPDFObjectHandle& st : streams) {
          if (visited.enter(st)) {
            completeStreamResources(ctx, st, eff, visited, fixedEntries);
          }
        }
      }
    }
  }
  if (fixedEntries) {
    ctx.issue("RESOURCES_COMPLETED",
              "made content-stream resources explicit: added " +
                  std::to_string(fixedEntries) +
                  " missing resource entr(ies) from the inherited scope (ISO 19005-2 6.2.2)",
              true);
  }
}

bool rasterFlattenPage(Ctx& ctx, QPDFPageObjectHelper& ph, int pageIndex) {
  int w = 0, h = 0;
  std::string rgb;
  if (!ctx.opt.rasterizePage(pageIndex, ctx.opt.rasterDpi, w, h, rgb)) return false;
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
  QPDFObjectHandle img = QPDFObjectHandle::newStream(&ctx.pdf, rgb);
  QPDFObjectHandle id = img.getDict();
  id.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
  id.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
  id.replaceKey("/Width", QPDFObjectHandle::newInteger(w));
  id.replaceKey("/Height", QPDFObjectHandle::newInteger(h));
  id.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
  id.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
  QPDFObjectHandle res = QPDFObjectHandle::newDictionary();
  QPDFObjectHandle xo = QPDFObjectHandle::newDictionary();
  xo.replaceKey("/FlatIm", ctx.pdf.makeIndirectObject(img));
  res.replaceKey("/XObject", xo);
  page.replaceKey("/Resources", res);
  char buf[192];
  std::snprintf(buf, sizeof(buf), "q %.4f 0 0 %.4f %.4f %.4f cm /FlatIm Do Q", pw, phh, mx1,
                my1);
  page.replaceKey("/Contents",
                  ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, buf)));
  if (page.getKey("/Group").isDictionary()) page.removeKey("/Group");
  return true;
}

namespace {
void fixGState(Ctx& ctx, QPDFObjectHandle gs) {
  if (!gs.isDictionary()) return;
  if (gs.hasKey("/TR")) {
    gs.removeKey("/TR");
    ctx.issue("TRANSFER_FUNCTION_REMOVED", "removed /TR from ExtGState", true);
  }
  if (gs.hasKey("/HTO")) {
    gs.removeKey("/HTO");
    ctx.issue("HALFTONE_ORIGIN_REMOVED", "removed /HTO from ExtGState", true);
  }
  QPDFObjectHandle tr2 = gs.getKey("/TR2");
  if (!tr2.isNull() && !nameIs(tr2, "/Default")) {
    gs.replaceKey("/TR2", QPDFObjectHandle::newName("/Default"));
    ctx.issue("TRANSFER_FUNCTION_REMOVED", "replaced non-default /TR2 with /Default", true);
  }
  if (gs.hasKey("/HTP")) {
    gs.removeKey("/HTP");
    ctx.issue("HALFTONE_PHASE_REMOVED", "removed /HTP from ExtGState", true);
  }
  QPDFObjectHandle ht = gs.getKey("/HT");
  if (ht.isDictionary() || ht.isStream()) {
    QPDFObjectHandle htd = ht.isStream() ? ht.getDict() : ht;
    long long type = htd.getKey("/HalftoneType").isInteger()
                         ? htd.getKey("/HalftoneType").getIntValue()
                         : 0;
    if (type != 1 && type != 5) {
      gs.removeKey("/HT");
      ctx.issue("HALFTONE_REMOVED", "removed halftone of type " + std::to_string(type), true);
    } else {
      if (htd.hasKey("/HalftoneName")) {
        htd.removeKey("/HalftoneName");
        ctx.issue("HALFTONE_NAME_REMOVED", "removed /HalftoneName from halftone", true);
      }
      if (htd.hasKey("/TransferFunction")) {
        htd.removeKey("/TransferFunction");
        ctx.issue("HALFTONE_TF_REMOVED", "removed /TransferFunction from halftone", true);
      }
    }
  }
  if (gs.hasKey("/RI") && kValidIntents.count(nameOf(gs.getKey("/RI"))) == 0) {
    gs.removeKey("/RI");
    ctx.issue("RENDERING_INTENT_FIXED", "removed invalid rendering intent in ExtGState", true);
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

void fixImage(Ctx& ctx, QPDFObjectHandle image, int depth = 0);

void fixImage(Ctx& ctx, QPDFObjectHandle image, int depth) {
  QPDFObjectHandle d = image.getDict();
  if (depth < 3) {
    for (const char* key : {"/SMask", "/Mask"}) {
      QPDFObjectHandle m = d.getKey(key);
      if (m.isStream()) fixImage(ctx, m, depth + 1);
    }
  }
  if (d.getKey("/Interpolate").isBool() && d.getKey("/Interpolate").getBoolValue()) {
    d.removeKey("/Interpolate");
    ctx.issue("INTERPOLATE_REMOVED", "removed /Interpolate true from image", true);
  }
  if (d.hasKey("/Alternates")) {
    d.removeKey("/Alternates");
    ctx.issue("ALTERNATES_REMOVED", "removed /Alternates from image", true);
  }
  if (d.hasKey("/OPI")) {
    d.removeKey("/OPI");
    ctx.issue("OPI_REMOVED", "removed /OPI from image", true);
  }
  if (d.hasKey("/Intent") && kValidIntents.count(nameOf(d.getKey("/Intent"))) == 0) {
    d.removeKey("/Intent");
    ctx.issue("RENDERING_INTENT_FIXED", "removed invalid rendering intent on image", true);
  }
  if (ctx.pdf14Target()) {
    QPDFObjectHandle filters = d.getKey("/Filter");
    std::vector<std::string> names;
    if (filters.isName()) names.push_back(filters.getName());
    if (filters.isArray()) {
      for (int i = 0; i < filters.getArrayNItems(); ++i) {
        names.push_back(nameOf(filters.getArrayItem(i)));
      }
    }
    for (const std::string& n : names) {
      if (n == "/JPXDecode") {
        if (!transcodeJpxImage(ctx, image)) return;
        break;
      }
    }
    if (d.getKey("/SMask").isStream()) {
      if (!flattenImageSMask(ctx, image)) return;
    }
  }
}

void fixResources(Ctx& ctx, QPDFObjectHandle res, Visited& visited, bool flatten) {
  if (!res.isDictionary() || !visited.enter(res)) return;
  QPDFObjectHandle gsd = res.getKey("/ExtGState");
  if (gsd.isDictionary()) {
    for (const std::string& k : gsd.getKeys()) {
      fixGState(ctx, gsd.getKey(k));
      if (flatten) flattenGStateTransparency(ctx, gsd.getKey(k));
    }
  }
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (xod.isDictionary()) {
    for (const std::string& k : xod.getKeys()) {
      QPDFObjectHandle xo = xod.getKey(k);
      if (!xo.isStream()) continue;
      if (!visited.enter(xo)) continue;
      QPDFObjectHandle d = xo.getDict();
      std::string subtype = nameOf(d.getKey("/Subtype"));
      if (subtype == "/Image") {
        fixImage(ctx, xo);
        if (ctx.failed()) return;
      } else if (subtype == "/PS") {
        xod.removeKey(k);
        ctx.issue("POSTSCRIPT_XOBJECT_REMOVED", "removed PostScript XObject " + k, true);
      } else if (subtype == "/Form") {
        if (ctx.isA() && ctx.part >= 2 && !d.getKey("/Resources").isDictionary() &&
            res.isDictionary()) {
          d.replaceKey("/Resources", res);
          ctx.issue("FORM_RESOURCES_ADDED",
                    "added explicit /Resources to a form XObject (inherited resources are "
                    "not permitted in PDF/A-2+)",
                    true);
        }
        if (d.hasKey("/Ref")) {
          d.removeKey("/Ref");
          ctx.issue("REFERENCE_XOBJECT_FIXED", "removed /Ref from form XObject", true);
        }
        if (d.hasKey("/PS")) {
          d.removeKey("/PS");
          ctx.issue("POSTSCRIPT_XOBJECT_REMOVED", "removed /PS key from form XObject", true);
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
  QPDFObjectHandle pat = res.getKey("/Pattern");
  if (pat.isDictionary()) {
    for (const std::string& k : pat.getKeys()) {
      QPDFObjectHandle p = pat.getKey(k);
      if (p.isStream() && visited.enter(p)) {
        fixResources(ctx, p.getDict().getKey("/Resources"), visited, flatten);
        if (ctx.failed()) return;
      }
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

bool valid3DContent(Ctx& ctx, QPDFObjectHandle annot) {
  QPDFObjectHandle dd = annot.getKey("/3DD");
  if (dd.isDictionary() && dd.getKey("/3DD").isStream()) dd = dd.getKey("/3DD");
  if (!dd.isStream()) return false;
  std::string st = nameOf(dd.getDict().getKey("/Subtype"));
  if (ctx.isE()) return st == "/U3D";
  return st == "/U3D" || st == "/PRC";
}

const std::set<std::string> kPdf16OnlyExtra = {"/Polygon", "/PolyLine", "/Caret", "/Watermark"};

bool annotForbidden(Ctx& ctx, QPDFObjectHandle annot, const std::string& subtype) {
  if (subtype.empty()) return false;
  if (ctx.isX()) {
    if (ctx.pdf20Print() && subtype == "/TrapNet") return true;
    return subtype != "/TrapNet" && subtype != "/PrinterMark";
  }
  if (subtype == "/Sound" || subtype == "/Movie" || subtype == "/Screen") return true;
  if (ctx.opt.ua && subtype == "/TrapNet") return true;
  if (subtype == "/3D") return !ctx.allow3D() || !valid3DContent(ctx, annot);
  if (subtype == "/RichMedia") return ctx.conf != 'E';
  if (ctx.isE()) {
    if (subtype == "/FileAttachment") return false;
    return kPdf14Annots.count(subtype) == 0 && kPdf16OnlyExtra.count(subtype) == 0;
  }
  if (subtype == "/FileAttachment") return !ctx.allowEmbeddedFiles();
  if (ctx.part == 1) return kPdf14Annots.count(subtype) == 0;
  if (ctx.part >= 4) {
    if (subtype == "/TrapNet") return true;
    return kPdf14Annots.count(subtype) == 0 && kPdf17ExtraAnnots.count(subtype) == 0 &&
           subtype != "/Projection";
  }
  return kPdf14Annots.count(subtype) == 0 && kPdf17ExtraAnnots.count(subtype) == 0;
}

void fixAnnotation(Ctx& ctx, QPDFObjectHandle annot) {
  std::string subtype = nameOf(annot.getKey("/Subtype"));
  if (annot.hasKey("/A")) {
    QPDFObjectHandle a = annot.getKey("/A");
    std::string s = a.isDictionary() ? nameOf(a.getKey("/S")) : std::string();
    if (subtype == "/Widget" || !actionAllowed(ctx, a)) {
      annot.removeKey("/A");
      ctx.issue("ACTION_REMOVED",
                subtype == "/Widget"
                    ? "removed action from Widget annotation (widgets may not have /A)"
                    : "removed forbidden action " + s + " from annotation",
                true);
    } else if (a.hasKey("/Next")) {
      a.removeKey("/Next");
      ctx.issue("ACTION_CHAIN_REMOVED", "removed /Next action chain in annotation", true);
    }
  }
  sanitizeAdditionalActions(ctx, annot, "annotation " + subtype, subtype == "/Widget");
  long long f = annot.getKey("/F").isInteger() ? annot.getKey("/F").getIntValue() : 0;
  long long fixedF = (f | 4LL) & ~(1LL | 2LL | 32LL | 256LL);
  if (fixedF != f || !annot.getKey("/F").isInteger()) {
    annot.replaceKey("/F", QPDFObjectHandle::newInteger(fixedF));
    ctx.issue("ANNOT_FLAGS_FIXED", "normalized annotation flags on " + subtype, true);
  }
  if (ctx.pdf14Target() && numOf(annot.getKey("/CA"), 1.0) < 1.0) {
    annot.replaceKey("/CA", QPDFObjectHandle::newInteger(1));
    ctx.issue("TRANSPARENCY_FLATTENED", "forced annotation /CA to 1 (visual risk)", true);
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
    if (isBtn && n.isStream()) {
      QPDFObjectHandle states = QPDFObjectHandle::newDictionary();
      states.replaceKey("/Off", n);
      ap.replaceKey("/N", states);
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
  } else if (subtype != "/Popup" && subtype != "/Link" && subtype != "/Projection") {
    QPDFObjectHandle n = makeEmptyAppearance(ctx, annot.getKey("/Rect"));
    QPDFObjectHandle newAp = QPDFObjectHandle::newDictionary();
    if (subtype == "/Widget" && fieldTypeIsBtn(annot)) {
      QPDFObjectHandle states = QPDFObjectHandle::newDictionary();
      states.replaceKey("/Off", ctx.pdf.makeIndirectObject(n));
      newAp.replaceKey("/N", states);
      annot.replaceKey("/AS", QPDFObjectHandle::newName("/Off"));
    } else {
      newAp.replaceKey("/N", ctx.pdf.makeIndirectObject(n));
    }
    annot.replaceKey("/AP", newAp);
    ctx.issue("APPEARANCE_ADDED", "synthesized empty appearance stream for " + subtype, true);
  }
}

void fixPageAnnotations(Ctx& ctx, QPDFObjectHandle page) {
  QPDFObjectHandle annots = page.getKey("/Annots");
  if (!annots.isArray()) return;
  QPDFObjectHandle kept = QPDFObjectHandle::newArray();
  bool removedAny = false;
  for (int i = 0; i < annots.getArrayNItems(); ++i) {
    QPDFObjectHandle a = annots.getArrayItem(i);
    if (!a.isDictionary()) {
      removedAny = true;
      continue;
    }
    std::string subtype = nameOf(a.getKey("/Subtype"));
    if (annotForbidden(ctx, a, subtype)) {
      ctx.issue("ANNOTATION_REMOVED", "removed forbidden annotation " + subtype, true);
      removedAny = true;
      continue;
    }
    fixAnnotation(ctx, a);
    kept.appendItem(a);
  }
  if (removedAny) page.replaceKey("/Annots", kept);
}
}

void passPages(Ctx& ctx) {
  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();

  bool flatten = false;
  if (ctx.transparencyBanned()) {
    TransparencyReport rep = scanTransparency(ctx, pages);
    if (rep.real && ctx.opt.rasterizePage) {
      int rastered = 0;
      bool allOk = true;
      for (size_t i = 0; i < pages.size(); ++i) {
        if (i < rep.pages.size() && rep.pages[i]) {
          if (rasterFlattenPage(ctx, pages[i], static_cast<int>(i))) {
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
                      " dpi (renderer-faithful flattening)",
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
  if (ctx.pdf14Target()) {
    for (auto& ph : pages) {
      try {
        ph.externalizeInlineImages(1);
      } catch (...) {
      }
    }
  }

  Visited visited;
  for (auto& ph : pages) {
    QPDFObjectHandle page = ph.getObjectHandle();
    if (page.hasKey("/AA")) {
      page.removeKey("/AA");
      ctx.issue("ADDITIONAL_ACTIONS_REMOVED", "removed /AA from page", true);
    }
    if (page.hasKey("/PresSteps")) page.removeKey("/PresSteps");
    if (ctx.pdf14Target() && page.getKey("/Group").isDictionary() &&
        nameIs(page.getKey("/Group").getKey("/S"), "/Transparency")) {
      page.removeKey("/Group");
      ctx.issue("TRANSPARENCY_GROUP_REMOVED", "removed transparency group from page", true);
    }
    fixResources(ctx, page.getKey("/Resources"), visited, flatten);
    if (ctx.failed()) return;
    fixPageAnnotations(ctx, page);
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (annots.isArray()) {
      for (int i = 0; i < annots.getArrayNItems(); ++i) {
        QPDFObjectHandle a = annots.getArrayItem(i);
        if (!a.isDictionary()) continue;
        QPDFObjectHandle ap = a.getKey("/AP");
        if (!ap.isDictionary()) continue;
        QPDFObjectHandle n = ap.getKey("/N");
        std::vector<QPDFObjectHandle> streams;
        if (n.isStream()) {
          streams.push_back(n);
        } else if (n.isDictionary()) {
          for (const std::string& k : n.getKeys()) {
            if (n.getKey(k).isStream()) streams.push_back(n.getKey(k));
          }
        }
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
