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

#include "fonts_ft.hh"
#include "images.hh"
#include "passes.hh"
#include "limits.hh"
#include "profile_types.hh"
#include "profile_eval.hh"
#include "profile_events.hh"
#include "util.hh"

namespace pdfa {
extern const unsigned char kSrgbIcc[];
extern const unsigned int kSrgbIccLen;
extern const unsigned char kCmykIcc[];
extern const unsigned int kCmykIccLen;


namespace {
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}

const char* const kSevName[] = {"", "Info", "Warning", "Error"};

void gatherFileFacts(Ctx& ctx, const unsigned char* inputData, std::size_t inputSize, Events& ev) {
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
}

void gatherDocumentFacts(Ctx& ctx, Events& ev) {
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
      QPDFObjectHandle oiProf = oi.getArrayItem(0).getKey("/DestOutputProfile");
      if (oiProf.isStream()) {
        try {
          auto buf = oiProf.getStreamData(qpdf_dl_all);
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
}

void gatherPageFacts(Ctx& ctx, Events& ev) {
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
            if (af.subtype == "Widget" || af.subtype == "Popup") continue;
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
      if (pf.hasTransGroup) initial.x.inTransGroup = true;
      scanEvents(page.getKey("/Contents"), res, initial, pageNum, 0, seen, ev);
      QPDFObjectHandle pannots = page.getKey("/Annots");
      if (pannots.isArray()) {
        for (int ai = 0; ai < pannots.getArrayNItems(); ++ai) {
          QPDFObjectHandle an = pannots.getArrayItem(ai);
          if (!an.isDictionary()) continue;
          std::vector<QPDFObjectHandle> streams = normalAppearanceStreams(an);
          QPDFObjectHandle rect = an.getKey("/Rect");
          long long afl = an.getKey("/F").isInteger() ? an.getKey("/F").getIntValue() : 0;
          if (afl & 2) continue;
          for (QPDFObjectHandle& st : streams) {
            if (!seen.enter(st)) continue;
            QPDFObjectHandle sres = st.getDict().getKey("/Resources");
            Gs apgs;
            QPDFObjectHandle bb = st.getDict().getKey("/BBox");
            QPDFObjectHandle mx = st.getDict().getKey("/Matrix");
            if (bb.isArray() && bb.getArrayNItems() == 4 && rect.isArray() &&
                rect.getArrayNItems() == 4) {
              Mat m;
              if (mx.isArray() && mx.getArrayNItems() == 6) {
                m = {numOf(mx.getArrayItem(0), 1), numOf(mx.getArrayItem(1), 0),
                     numOf(mx.getArrayItem(2), 0), numOf(mx.getArrayItem(3), 1),
                     numOf(mx.getArrayItem(4), 0), numOf(mx.getArrayItem(5), 0)};
              }
              double bx0 = numOf(bb.getArrayItem(0), 0), by0 = numOf(bb.getArrayItem(1), 0);
              double bx1 = numOf(bb.getArrayItem(2), 0), by1 = numOf(bb.getArrayItem(3), 0);
              double cx[4], cy[4];
              const double xs[4] = {bx0, bx1, bx0, bx1};
              const double ys[4] = {by0, by0, by1, by1};
              for (int c = 0; c < 4; ++c) {
                cx[c] = m.a * xs[c] + m.c * ys[c] + m.e;
                cy[c] = m.b * xs[c] + m.d * ys[c] + m.f;
              }
              double tx0 = *std::min_element(cx, cx + 4), tx1 = *std::max_element(cx, cx + 4);
              double ty0 = *std::min_element(cy, cy + 4), ty1 = *std::max_element(cy, cy + 4);
              double rx0 = std::min(numOf(rect.getArrayItem(0), 0), numOf(rect.getArrayItem(2), 0));
              double ry0 = std::min(numOf(rect.getArrayItem(1), 0), numOf(rect.getArrayItem(3), 0));
              double rx1 = std::max(numOf(rect.getArrayItem(0), 0), numOf(rect.getArrayItem(2), 0));
              double ry1 = std::max(numOf(rect.getArrayItem(1), 0), numOf(rect.getArrayItem(3), 0));
              double sx = tx1 - tx0 > 1e-6 ? (rx1 - rx0) / (tx1 - tx0) : 1.0;
              double sy = ty1 - ty0 > 1e-6 ? (ry1 - ry0) / (ty1 - ty0) : 1.0;
              Mat fit{sx, 0, 0, sy, rx0 - tx0 * sx, ry0 - ty0 * sy};
              apgs.ctm = mul(m, fit);
            }
            scanEvents(st, sres.isDictionary() ? sres : res, apgs, pageNum, 1, seen, ev);
          }
        }
      }
      pf.imageCount = static_cast<int>(ev.images.size() - imgBefore);
      pf.empty = ev.images.size() == imgBefore && ev.paints.size() == paintBefore &&
                 ev.texts.size() == textBefore;
      auto addPlates = [&pf](const std::vector<double>& comps, const ColorInfo& ci) {
        std::set<std::string>& plout = pf.plates;
        auto addCMYK = [&]() {
          plout.insert("Cyan");
          plout.insert("Magenta");
          plout.insert("Yellow");
          plout.insert("Black");
        };
        if (ci.cls == "cmyk" && comps.size() == 4) {
          const char* names[4] = {"Cyan", "Magenta", "Yellow", "Black"};
          for (int ch = 0; ch < 4; ++ch) {
            if (comps[ch] > 0.001) plout.insert(names[ch]);
          }
        } else if (ci.cls == "icc" || ci.cls == "cal" || ci.cls == "lab") {
          bool white = !comps.empty();
          for (double v : comps) {
            if (v < 0.999) white = false;
          }
          if (!white) addCMYK();
        } else if (ci.cls == "gray") {
          if (!comps.empty() && comps[0] < 0.999) plout.insert("Black");
        } else if (ci.cls == "separation" || ci.cls == "devicen") {
          for (size_t i = 0; i < ci.colorants.size(); ++i) {
            double tint = i < comps.size() ? comps[i] : (comps.empty() ? 1.0 : comps[0]);
            if (tint <= 0.001) continue;
            const std::string& c = ci.colorants[i];
            if (c == "All" || c == "Registration") {
              plout.insert("Cyan");
              plout.insert("Magenta");
              plout.insert("Yellow");
              plout.insert("Black");
            } else {
              plout.insert(c);
            }
          }
        } else if (!comps.empty()) {
          bool white = true;
          for (double v : comps) {
            if (v < 0.999) white = false;
          }
          if (!white) addCMYK();
        }
      };
      auto bigEnough = [](const Box& b) {
        if (!b.valid) return false;
        return (b.x1 - b.x0) > 0.1 || (b.y1 - b.y0) > 0.1;
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
          addPlates({0.0}, ColorInfo{"gray", "", "", {}, 1, false});
        } else if (ci.cls == "gray" || (ci.cls == "icc" && ci.declaredComps == 1)) {
          pf.plates.insert("Black");
        } else if (ci.cls == "separation" || ci.cls == "devicen") {
          for (const std::string& c : ci.colorants) {
            if (c == "All" || c == "Registration") continue;
            pf.plates.insert(c);
          }
        } else {
          for (const char* c : {"Cyan", "Magenta", "Yellow", "Black"}) {
            pf.plates.insert(c);
          }
        }
      }
      ev.pages.push_back(pf);
    }
  } catch (...) {
    return;
  }

}

void gatherInkFacts(Ctx& ctx, const PfProfile& prof, Events& ev) {
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

}

void gatherResourceFacts(Ctx& ctx, const PfProfile& prof, Events& ev) {
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
        std::map<std::string, std::set<int>> tuValid;
        std::set<std::string> tuPresent;
        for (FontFacts& f : ev.fonts) {
          if (!f.hasToUnicode || !f.dict.isInitialized()) continue;
          QPDFObjectHandle tu = f.dict.getKey("/ToUnicode");
          if (!tu.isStream()) continue;
          std::string txt;
          try {
            auto buf = tu.getStreamData(qpdf_dl_all);
            txt.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
          } catch (...) {
            continue;
          }
          char key[32];
          std::snprintf(key, sizeof(key), "%d,%d", f.og.getObj(), f.og.getGen());
          tuPresent.insert(key);
          std::set<int>& valid = tuValid[key];
          auto goodDst = [](std::string h) {
            for (char& c : h) c = std::tolower(static_cast<unsigned char>(c));
            return !h.empty() && h.find_first_not_of('0') != std::string::npos &&
                   h.substr(0, 4) != "fffd";
          };
          size_t p = 0;
          while ((p = txt.find("beginbfchar", p)) != std::string::npos) {
            size_t end = txt.find("endbfchar", p);
            if (end == std::string::npos) break;
            size_t q = p;
            while (true) {
              size_t a = txt.find('<', q);
              if (a == std::string::npos || a > end) break;
              size_t ac = txt.find('>', a);
              if (ac == std::string::npos || ac > end) break;
              size_t b = txt.find('<', ac);
              if (b == std::string::npos || b > end) break;
              size_t bc = txt.find('>', b);
              if (bc == std::string::npos || bc > end) break;
              int src = static_cast<int>(std::strtol(txt.substr(a + 1, ac - a - 1).c_str(),
                                                     nullptr, 16));
              if (src < 256 && goodDst(txt.substr(b + 1, bc - b - 1))) valid.insert(src);
              q = bc + 1;
            }
            p = end + 1;
          }
          p = 0;
          while ((p = txt.find("beginbfrange", p)) != std::string::npos) {
            size_t end = txt.find("endbfrange", p);
            if (end == std::string::npos) break;
            size_t q = p;
            while (true) {
              size_t a = txt.find('<', q);
              if (a == std::string::npos || a > end) break;
              size_t ac = txt.find('>', a);
              if (ac == std::string::npos || ac > end) break;
              size_t b = txt.find('<', ac);
              if (b == std::string::npos || b > end) break;
              size_t bc = txt.find('>', b);
              if (bc == std::string::npos || bc > end) break;
              long lo = std::strtol(txt.substr(a + 1, ac - a - 1).c_str(), nullptr, 16);
              long hi = std::strtol(txt.substr(b + 1, bc - b - 1).c_str(), nullptr, 16);
              size_t dststart = txt.find_first_not_of(" \r\n\t", bc + 1);
              bool ok = false;
              if (dststart != std::string::npos && txt[dststart] == '<') {
                size_t dc = txt.find('>', dststart);
                if (dc == std::string::npos) break;
                ok = goodDst(txt.substr(dststart + 1, dc - dststart - 1));
                q = dc + 1;
              } else if (dststart != std::string::npos && txt[dststart] == '[') {
                size_t dc = txt.find(']', dststart);
                ok = dc != std::string::npos;
                q = dc == std::string::npos ? bc + 1 : dc + 1;
              } else {
                q = bc + 1;
              }
              if (ok && lo >= 0 && hi < 256 && hi >= lo) {
                for (long c = lo; c <= hi; ++c) valid.insert(static_cast<int>(c));
              }
            }
            p = end + 1;
          }
        }
        std::map<QPDFObjGen, std::map<std::string, bool>> charProcCache;
        for (TextEvent& e : ev.texts) {
          char key[32];
          std::snprintf(key, sizeof(key), "%d,%d", e.fontOg.getObj(), e.fontOg.getGen());
          auto it = faces.find(key);
          FontFacts* ff = nullptr;
          for (FontFacts& f : ev.fonts) {
            if (f.og == e.fontOg) ff = &f;
          }
          if (ff && ff->cid && !ff->hasToUnicode) {
            e.mappedToUnicode = false;
            ff->allUsedMapped = false;
          }
          if (ff && ff->type3 && ff->dict.isInitialized()) {
            QPDFObjectHandle cp = ff->dict.getKey("/CharProcs");
            SimpleEncoding t3enc = readEncoding(ff->dict, false);
            std::map<std::string, bool>& charProcPaints = charProcCache[e.fontOg];
            for (unsigned char c : e.bytes) {
              if (c == ' ') e.glyphWhitespace = true;
              const std::string& dn = t3enc.diffs[c];
              bool has = false, paints = false;
              if (!dn.empty() && cp.isDictionary()) {
                QPDFObjectHandle pr = cp.getKey(dn);
                if (pr.isStream()) {
                  has = true;
                  auto cached = charProcPaints.find(dn);
                  if (cached != charProcPaints.end()) {
                    paints = cached->second;
                  } else {
                    try {
                      auto buf = pr.getStreamData(qpdf_dl_all);
                      std::string t(reinterpret_cast<const char*>(buf->getBuffer()),
                                    std::min<size_t>(buf->getSize(), 8192));
                      for (const char* op :
                           {" re", " f", "\nf", " S", "\nS", " c", " l", " Do", " sh"}) {
                        if (t.find(op) != std::string::npos) {
                          paints = true;
                          break;
                        }
                      }
                    } catch (...) {
                      paints = true;
                    }
                    charProcPaints[dn] = paints;
                  }
                }
              }
              if ((!has || !paints) && c != ' ') e.glyphHasContour = false;
            }
            continue;
          }
          if (it == faces.end() || !ff || ff->cid) continue;
          FT_Face face = it->second;
          SimpleEncoding enc = readEncoding(ff->dict, ff->symbolic);
          bool hasTu = tuPresent.count(key) != 0;
          const std::set<int>* tuSet = hasTu ? &tuValid[key] : nullptr;
          auto codeMaps = [&](int c, FT_UInt gid) {
            if (hasTu) return tuSet->count(c) != 0;
            const std::string& dn = enc.diffs[c];
            if (!dn.empty()) {
              uint32_t uni = aglNameToUnicode(dn.substr(1));
              if (!uni) parseUniName(dn.substr(1), uni);
              if (uni) return true;
            }
            if (enc.base && enc.base(c)) return true;
            if (gid && FT_HAS_GLYPH_NAMES(face)) {
              char gname[64];
              if (FT_Get_Glyph_Name(face, gid, gname, sizeof(gname)) == 0 && gname[0]) {
                uint32_t uni = aglNameToUnicode(gname);
                if (!uni) parseUniName(gname, uni);
                if (uni) return true;
              }
            }
            return false;
          };
          auto resolveGid = [&](int code) -> FT_UInt {
            if (ff->symbolic && enc.diffs[code].empty()) {
              const char* fmt = FT_Get_Font_Format(face);
              if (fmt && std::strcmp(fmt, "CFF") == 0) {
                int g = cffCustomEncodingGid(ff->fontProgram, code);
                if (g >= 0) {
                  return g > 0 && g < static_cast<int>(face->num_glyphs)
                             ? static_cast<FT_UInt>(g)
                             : 0;
                }
              } else if (fmt && std::strcmp(fmt, "Type 1") == 0) {
                FT_CharMap saved = face->charmap;
                for (int i = 0; i < face->num_charmaps; ++i) {
                  FT_CharMap cm = face->charmaps[i];
                  if (cm->encoding == FT_ENCODING_ADOBE_CUSTOM ||
                      cm->encoding == FT_ENCODING_ADOBE_STANDARD) {
                    FT_Set_Charmap(face, cm);
                    FT_UInt gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(code));
                    if (saved) FT_Set_Charmap(face, saved);
                    return gid;
                  }
                }
              }
            }
            return glyphForCode(face, code, enc, ff->symbolic);
          };
          for (unsigned char c : e.bytes) {
            if (c == ' ') e.glyphWhitespace = true;
            FT_UInt gid = resolveGid(c);
            if (gid == 0 && ff->symbolic && ff->trueType &&
                static_cast<long>(c) < face->num_glyphs) {
              gid = c;
            }
            if (c != ' ' && !codeMaps(c, gid)) {
              e.mappedToUnicode = false;
              ff->allUsedMapped = false;
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
          size_t n = std::min<size_t>(buf->getSize(), kMaxXmpBytes);
          ev.xmpRaw.assign(reinterpret_cast<const char*>(buf->getBuffer()), n);
        } catch (...) {
          ctx.scanIncomplete("the XMP metadata packet");
        }
      }
    }
  }

}

void reportBuiltinHits(Ctx& ctx, const PfProfile& prof, const unsigned char* inputData, std::size_t inputSize, Events& ev) {
  {
    auto sevLabel = [](int sev) {
      if (sev >= 3) return "Error";
      return sev == 2 ? "Warning" : "Info";
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
    std::set<std::string> notedWizard;
    auto xcompLevels = [](const PfBuiltin& b) {
      static const std::vector<std::pair<const char*, const char*>> flags = {
          {"PDFA1b2005", "1b"}, {"PDFA1a2005", "1a"}, {"PDFA2b", "2b"},
          {"PDFA2u", "2u"},     {"PDFA2a", "2a"},     {"PDFA3ZFeRD", "3b"},
          {"PDFA3b", "3b"},     {"PDFA3u", "3u"},     {"PDFA3a", "3a"},
          {"PDFA4", "4"},       {"PDFA4f", "4f"},     {"PDFA4e", "4e"},
          {"PDFX1A2001", "x1a"}, {"PDFX1A2003", "x1a"}, {"PDFX32002", "x3"},
          {"PDFX32003", "x3"},  {"PDFX4", "x4"},      {"PDFX4p", "x4p"},
          {"PDFX5g", "x5g"},    {"PDFX5n", "x5n"},    {"PDFX5pg", "x5pg"},
          {"PDFX6", "x6"},      {"PDFX6n", "x6n"},    {"PDFX6p", "x6p"},
          {"PDFE12008", "e1"},  {"PDFVT1", "vt1"},    {"PDFVT2", "vt2"},
          {"PDFVT3", "vt3"},
      };
      std::vector<std::string> out;
      for (const auto& [flag, lvl] : flags) {
        auto it = b.params.find(flag);
        if (it != b.params.end() && it->second > 0.5 &&
            std::find(out.begin(), out.end(), lvl) == out.end()) {
          out.push_back(lvl);
        }
      }
      return out;
    };
    auto prettyLevel = [](const std::string& ls) {
      if (ls == "e1") return std::string("PDF/E-1");
      if (ls.rfind("vt", 0) == 0) return "PDF/VT-" + ls.substr(2);
      if (ls.rfind("x", 0) == 0) {
        std::string tail = ls.substr(1);
        if (tail == "1a") return std::string("PDF/X-1a");
        return "PDF/X-" + tail;
      }
      return "PDF/A-" + ls;
    };
    auto prettyLevelList = [&](const std::vector<std::string>& lvls) {
      std::string out;
      for (size_t i = 0; i < lvls.size(); ++i) {
        if (i) out += lvls.size() == 2 ? " or " : (i + 1 == lvls.size() ? ", or " : ", ");
        out += prettyLevel(lvls[i]);
      }
      return out;
    };
    struct VerifyResult {
      bool pass = false;
      long long count = 0;
      std::string detail;
    };
    auto verifyCache = std::make_shared<std::map<std::pair<const void*, std::string>,
                                                 VerifyResult>>();
    auto verifyBytes = [&](const unsigned char* data, std::size_t size,
                           const std::vector<std::string>& lvls, std::string& failDetail,
                           long long& worst) {
      worst = -1;
      for (const std::string& ls : lvls) {
        Level lv;
        if (!levelFromString(ls, lv)) continue;
        auto ckey = std::make_pair(static_cast<const void*>(data), ls);
        auto cached = verifyCache->find(ckey);
        if (cached != verifyCache->end()) {
          if (cached->second.pass) {
            worst = 0;
            failDetail.clear();
            return true;
          }
          if (worst < 0 || cached->second.count < worst) {
            worst = cached->second.count;
            failDetail = cached->second.detail;
          }
          continue;
        }
        Options o;
        o.level = lv;
        o.verifyOnly = true;
        o.password = ctx.opt.password;
        Result r = convert(data, size, o);
        if (r.ok && r.compliant) {
          (*verifyCache)[ckey] = {true, 0, std::string()};
          worst = 0;
          failDetail.clear();
          return true;
        }
        long long cnt = 0;
        std::string first;
        if (!r.ok) {
          cnt = 1;
          first = r.error;
        } else {
          for (const Issue& i : r.issues) {
            if (i.fixed && !issueIsNormalization(i.code)) {
              ++cnt;
              if (first.empty()) first = i.detail;
            }
          }
          if (cnt == 0) cnt = 1;
        }
        (*verifyCache)[ckey] = {false, cnt, first};
        if (worst < 0 || cnt < worst) {
          worst = cnt;
          failDetail = first;
        }
      }
      if (worst < 0) worst = 1;
      return false;
    };
    auto verifyAgainst = [&](const std::vector<std::string>& lvls, std::string& failDetail,
                             long long& worst) {
      if (!inputData || !inputSize) {
        worst = 0;
        return true;
      }
      return verifyBytes(inputData, inputSize, lvls, failDetail, worst);
    };
    auto embeddedPdfPayloads = [&]() {
      std::vector<std::string> out;
      Visited walkSeen;
      std::function<void(QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle node,
                                                            int depth) {
        if (depth > 8 || out.size() >= 16 || !node.isDictionary()) return;
        if (node.isIndirect() && !walkSeen.enter(node)) return;
        QPDFObjectHandle kids = node.getKey("/Kids");
        if (kids.isArray()) {
          for (int i = 0; i < kids.getArrayNItems(); ++i) walk(kids.getArrayItem(i), depth + 1);
        }
        QPDFObjectHandle names = node.getKey("/Names");
        if (!names.isArray()) return;
        for (int i = 0; i + 1 < names.getArrayNItems(); i += 2) {
          QPDFObjectHandle fs = names.getArrayItem(i + 1);
          if (!fs.isDictionary()) continue;
          QPDFObjectHandle ef = fs.getKey("/EF");
          if (!ef.isDictionary()) continue;
          QPDFObjectHandle f = ef.getKey("/F");
          if (!f.isStream()) f = ef.getKey("/UF");
          if (!f.isStream()) continue;
          try {
            auto buf = f.getStreamData(qpdf_dl_all);
            std::string bytes(reinterpret_cast<const char*>(buf->getBuffer()),
                              buf->getSize());
            if (bytes.compare(0, 4, "%PDF") == 0) out.push_back(std::move(bytes));
          } catch (...) {
            ctx.scanIncomplete("an embedded PDF attachment");
          }
          if (out.size() >= 16) return;
        }
      };
      QPDFObjectHandle namesDict = ctx.pdf.getRoot().getKey("/Names");
      if (namesDict.isDictionary()) walk(namesDict.getKey("/EmbeddedFiles"), 0);
      return out;
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
      } else if (nm == "PRCWzXComp_PDFDocument" || nm == "PRCWzXComp_PDFDocumentA" ||
                 nm == "PRCWzXComp_PDFDocumentE" || nm == "PRCWzXComp_PDFDocumentVT") {
        std::vector<std::string> lvls = xcompLevels(b);
        if (lvls.empty()) {
          if (notedWizard.insert(nm).second) {
            ctx.res.analysis.push_back(
                {"PROFILE_CHECK_UNSUPPORTED",
                 nm + ": no recognizable standard flavour is switched on", false});
          }
        } else {
          std::string failDetail;
          long long worst = 0;
          if (!verifyAgainst(lvls, failDetail, worst)) {
            emitB(b, worst, pg,
                  "Document does not conform to " + prettyLevelList(lvls) +
                      (failDetail.empty() ? "" : " - first deviation: " + failDetail));
          }
        }
      } else if (nm == "PRCWzXCompEmb_PDFDocumentA") {
        std::vector<std::string> lvls = xcompLevels(b);
        long long bad = 0;
        for (const std::string& att : embeddedPdfPayloads()) {
          std::string failDetail;
          long long worst = 0;
          if (!verifyBytes(reinterpret_cast<const unsigned char*>(att.data()), att.size(),
                           lvls.empty() ? std::vector<std::string>{"2b"} : lvls,
                           failDetail, worst)) {
            ++bad;
          }
        }
        emitB(b, bad, pg, "Embedded document does not conform to the archival standard");
      } else {
        if (notedWizard.insert(nm).second) {
          ctx.res.analysis.push_back(
              {"PROFILE_CHECK_UNSUPPORTED",
               nm + ": uses a built-in check Kura cannot evaluate yet", false});
        }
      }
    }
    if (ctx.opt.preflightProfile.find("PRC_PDFUA1_CHECK") != std::string::npos) {
      ctx.res.analysis.push_back(
          {"PROFILE_CHECK_UNSUPPORTED",
           "this profile requests the PDF/UA-1 accessibility check, which Kura does not "
           "run in analysis mode yet",
           false});
    }
  }

}

void gatherEvents(Ctx& ctx, const unsigned char* inputData, std::size_t inputSize,
                  const PfProfile& prof, Events& ev) {
  gatherFileFacts(ctx, inputData, inputSize, ev);
  gatherDocumentFacts(ctx, ev);
  gatherPageFacts(ctx, ev);
  gatherInkFacts(ctx, prof, ev);
  gatherResourceFacts(ctx, prof, ev);
  reportBuiltinHits(ctx, prof, inputData, inputSize, ev);
}


void reportRuleHits(Ctx& ctx, const PfProfile& prof, const Events& ev) {
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
      sweep(ev.paints, nullptr);
      if (supported) {
        dom = Domain::kText;
        sweep(ev.texts, nullptr);
      }
      if (supported) {
        dom = Domain::kImage;
        sweep(ev.images, nullptr);
      }
      dom = Domain::kPaint;
    } else if (supported && dom == Domain::kPaint) {
      sweep(ev.paints, nullptr);
    } else if (supported && dom == Domain::kText) sweep(ev.texts, nullptr);
    else if (supported && dom == Domain::kImage) sweep(ev.images, nullptr);
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
      std::string detail = std::string(kSevName[rule.severity < 4 && rule.severity > 0
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

void passProfile(Ctx& ctx, const unsigned char* inputData, std::size_t inputSize) {
  if (ctx.opt.preflightProfile.empty()) return;
  PfProfile prof;
  size_t firstCh = ctx.opt.preflightProfile.find_first_not_of(" \t\r\n");
  bool isJson = firstCh != std::string::npos && ctx.opt.preflightProfile[firstCh] == '{';
  bool parsed = isJson ? parseKuraJson(ctx.opt.preflightProfile, prof)
                       : parsePreflightXml(ctx.opt.preflightProfile, prof);
  if (!parsed) {
    bool looksLikeXml = ctx.opt.preflightProfile.find("<pdfpreflight") != std::string::npos;
    bool hasFixes = ctx.opt.preflightProfile.find("<fcfg>") != std::string::npos;
    if (looksLikeXml && hasFixes) {
      ctx.res.analysis.push_back(
          {"PROFILE_NO_CHECKS",
           "this profile only converts or repairs documents; it defines no checks to "
           "run in analysis mode",
           false});
    } else if (looksLikeXml) {
      ctx.res.analysis.push_back(
          {"PROFILE_NO_CHECKS", "this profile defines no checks Kura can run", false});
    } else {
      ctx.res.analysis.push_back(
          {"PROFILE_UNREADABLE", "the preflight profile could not be parsed", false});
    }
    return;
  }
  Events ev;
  gatherEvents(ctx, inputData, inputSize, prof, ev);
  reportRuleHits(ctx, prof, ev);
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
      "removepdfuakeys", "settransparencyblendcs",
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
}
