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
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "barcode.hh"
#include "colorconv.hh"
#include "uacheck.hh"
#include "fonts_ft.hh"
#include "images.hh"
#include "passes.hh"
#include "limits.hh"
#include "profile_types.hh"
#include "profile_eval.hh"
#include "profile_events.hh"
#include "profile_ext.hh"
#include "profile_spatial.hh"
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

void scanSyntax(Ctx& ctx, const unsigned char* data, std::size_t size, Events& ev) {
  auto isEol = [](unsigned char c) { return c == '\r' || c == '\n'; };
  auto isDigit = [](unsigned char c) { return c >= '0' && c <= '9'; };
  std::string_view f(reinterpret_cast<const char*>(data), size);
  std::map<QPDFObjGen, QPDFXRefEntry> xref = ctx.pdf.getXRefTable();
  std::vector<std::pair<qpdf_offset_t, QPDFObjGen>> offs;
  for (const auto& [og, e] : xref) {
    if (e.getType() == 1 && e.getOffset() > 0 && static_cast<std::size_t>(e.getOffset()) < size) offs.push_back({e.getOffset(), og});
  }
  std::sort(offs.begin(), offs.end());
  if (offs.size() > 500000) return;
  ev.syntaxScanned = true;
  for (size_t i = 0; i < offs.size(); ++i) {
    std::size_t o = static_cast<std::size_t>(offs[i].first);
    std::size_t end = i + 1 < offs.size() ? static_cast<std::size_t>(offs[i + 1].first) : size;
    std::size_t p = o;
    while (p < end && (data[p] == ' ' || data[p] == '\t')) ++p;
    if (p > 0 && !isEol(data[p - 1])) ev.syntaxIssues.insert("objNotPrecededByEol");
    while (p < end && isDigit(data[p])) ++p;
    std::size_t s1 = p;
    while (p < end && (data[p] == ' ' || data[p] == '\t' || isEol(data[p]))) ++p;
    if (p - s1 != 1 || data[s1] != ' ') ev.syntaxIssues.insert("objNumSep");
    while (p < end && isDigit(data[p])) ++p;
    std::size_t s2 = p;
    while (p < end && (data[p] == ' ' || data[p] == '\t' || isEol(data[p]))) ++p;
    if (p - s2 != 1 || data[s2] != ' ') ev.syntaxIssues.insert("objKeySep");
    if (f.compare(p, 3, "obj") != 0) continue;
    p += 3;
    if (p < end && !isEol(data[p])) ev.syntaxIssues.insert("objNotFollowedByEol");
    QPDFObjectHandle obj = ctx.pdf.getObjectByObjGen(offs[i].second);
    std::size_t from = p;
    if (obj.isStream()) {
      std::size_t sk = f.find("stream", p);
      while (sk != std::string_view::npos && sk < end) {
        std::size_t b = sk;
        while (b > p && (data[b - 1] == ' ' || isEol(data[b - 1]) || data[b - 1] == '\t')) --b;
        if (b >= p + 2 && data[b - 1] == '>' && data[b - 2] == '>') break;
        sk = f.find("stream", sk + 6);
      }
      if (sk == std::string_view::npos || sk >= end) continue;
      std::size_t ds = sk + 6;
      bool kwOk = true;
      if (ds < size && data[ds] == '\r' && ds + 1 < size && data[ds + 1] == '\n') ds += 2;
      else if (ds < size && data[ds] == '\n') ds += 1;
      else kwOk = false;
      if (!kwOk) ev.syntaxIssues.insert("streamKeyword");
      QPDFObjectHandle len = obj.getDict().getKey("/Length");
      std::size_t hint = ds;
      if (len.isInteger() && len.getIntValue() > 0 && ds + static_cast<std::size_t>(len.getIntValue()) <= size) {
        hint = ds + static_cast<std::size_t>(len.getIntValue()) - std::min<std::size_t>(static_cast<std::size_t>(len.getIntValue()), 16);
        if (f.find("endstream", hint) == std::string_view::npos) hint = ds;
      }
      std::size_t es = f.find("endstream", hint);
      if (es == std::string_view::npos) continue;
      if (!isEol(data[es - 1])) ev.syntaxIssues.insert("streamKeyword");
      if (kwOk && len.isInteger()) {
        std::size_t actual = es - ds;
        if (actual >= 2 && data[es - 2] == '\r' && data[es - 1] == '\n') actual -= 2;
        else if (actual >= 1 && isEol(data[es - 1])) actual -= 1;
        if (static_cast<long long>(actual) != len.getIntValue()) ev.syntaxIssues.insert("streamLength");
      }
      from = es + 9;
    }
    std::size_t eo = f.find("endobj", from);
    if (eo == std::string_view::npos) continue;
    if (!isEol(data[eo - 1])) ev.syntaxIssues.insert("endobjNotPrecededByEol");
    if (eo + 6 < size && !isEol(data[eo + 6])) ev.syntaxIssues.insert("endobjNotFollowedByEol");
  }
  std::size_t sx = f.rfind("startxref");
  if (sx != std::string_view::npos) {
    std::size_t p = sx + 9;
    while (p < size && (data[p] == ' ' || isEol(data[p]))) ++p;
    std::size_t xo = 0;
    while (p < size && isDigit(data[p])) xo = xo * 10 + (data[p++] - '0');
    if (xo < size && f.compare(xo, 4, "xref") == 0) {
      std::size_t q = xo + 4;
      if (q < size && data[q] == '\r' && q + 1 < size && data[q + 1] == '\n') q += 2;
      else if (q < size && isEol(data[q])) q += 1;
      else ev.syntaxIssues.insert("xrefEol");
      for (int guard = 0; guard < 100000 && q < size; ++guard) {
        while (q < size && isEol(data[q])) ++q;
        if (q >= size || f.compare(q, 7, "trailer") == 0) break;
        std::size_t a = q;
        while (q < size && isDigit(data[q])) ++q;
        if (q == a || q >= size || data[q] != ' ') {
          ev.syntaxIssues.insert("xrefHeader");
          break;
        }
        ++q;
        std::size_t b = q, cnt = 0;
        while (q < size && isDigit(data[q])) cnt = cnt * 10 + (data[q++] - '0');
        if (q == b) {
          ev.syntaxIssues.insert("xrefHeader");
          break;
        }
        if (q < size && data[q] == '\r' && q + 1 < size && data[q + 1] == '\n') q += 2;
        else if (q < size && isEol(data[q])) q += 1;
        else {
          ev.syntaxIssues.insert("xrefHeader");
          break;
        }
        q += cnt * 20;
      }
    }
  }
}

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
  ev.filesize = static_cast<double>(inputSize);
  try {
    for (QPDFObjectHandle o : ctx.pdf.getAllObjects()) o.getTypeCode();
    for (const QPDFExc& w : ctx.pdf.getWarnings()) {
      ev.qpdfWarningText.push_back(w.what());
      std::string m = w.getMessageDetail();
      ev.damaged = ev.damaged || m.find("reconstruct") != std::string::npos;
    }
    ev.qpdfWarnings = static_cast<int>(ev.qpdfWarningText.size());
  } catch (...) {
  }
  if (inputData && inputSize > 8) {
    try {
      scanSyntax(ctx, inputData, inputSize, ev);
    } catch (...) {
    }
  }
}

void gatherDocumentFacts(Ctx& ctx, Events& ev) {
  ev.root = ctx.pdf.getRoot();
  ev.raster = ctx.opt.rasterizePage;
  ev.trailer = ctx.pdf.getTrailer();
  ev.header = ctx.pdf.getPDFVersion();
  for (QPDFObjectHandle o : ctx.pdf.getAllObjects()) {
    if (!o.isStream()) continue;
    QPDFObjectHandle d = o.getDict();
    std::string st = nameOf(d.getKey("/Subtype"));
    if (!nameIs(d.getKey("/Type"), "/EmbeddedFile") && st.find("cxf") == std::string::npos) continue;
    std::string data;
    try {
      auto b = o.getStreamData(qpdf_dl_all);
      data.assign(reinterpret_cast<const char*>(b->getBuffer()), std::min<size_t>(b->getSize(), 1 << 20));
    } catch (...) {
      continue;
    }
    if (data.find("CxF") == std::string::npos) continue;
    for (const char* lvl : {"CxF/X-4a", "CxF/X-4b"}) {
      if (data.find(lvl) != std::string::npos) ev.cxfLevels.push_back(lvl);
    }
    if (data.find("CxF/X-4") != std::string::npos) ev.cxfLevels.push_back("CxF/X-4");
  }
  {
    QPDFObjectHandle root = ctx.pdf.getRoot();
    QPDFObjectHandle oi = root.getKey("/OutputIntents");
    ev.hasOutputIntent = oi.isArray() && oi.getArrayNItems() > 0;
    ev.outputIntentCount = oi.isArray() ? oi.getArrayNItems() : 0;
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
      pf.obj = ph.getObjectHandle();
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
      pf.crop = readBox(ph.getAttribute("/CropBox", false));
      if (!pf.crop.valid) pf.crop = pf.media;
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
            af.obj = an;
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
            if (af.subtype == "Popup") continue;
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
      enterGroup(initial.x, grp, res);
      initial.x.resInherited = !page.getKey("/Resources").isDictionary();
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
          addPlates({0.0}, ColorInfo{"gray", "", "", {}, 1, false, {}, QPDFObjectHandle(), false});
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

void gatherResourceFacts(Ctx& ctx, const PfProfile& prof, Events& ev) {
  {
    static const std::set<std::string> glyphChecks = {"text.glyph", "text.unicode", "font.license", "font.problem",
                                                      "page.unmappable_text"};
    bool needGlyphs = false;
    for (const auto& [cid, cond] : prof.conds) {
      for (const PfAtom& a : cond.atoms) needGlyphs = needGlyphs || glyphChecks.count(a.token) > 0;
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
    {
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

void gatherBarcodeFacts(Ctx& ctx, const PfProfile& prof, Events& ev) {
  bool wanted = false;
  for (const auto& kv : prof.conds) {
    for (const PfAtom& a : kv.second.atoms) wanted = wanted || (a.check >= 0 && std::string(checkDef(a.check).group) == "barcode");
  }
  if (!wanted || !ctx.opt.rasterizePage || !barcodeReaderAvailable()) return;
  ev.barcodesScanned = true;
  const double dpi = 300;
  QPDFPageDocumentHelper dh(ctx.pdf);
  int idx = 0;
  for (auto& ph : dh.getAllPages()) {
    int w = 0, h = 0;
    std::string rgb;
    if (!ctx.opt.rasterizePage(idx, dpi, w, h, rgb) || w <= 0 || h <= 0) {
      ++idx;
      continue;
    }
    QPDFObjectHandle cb = ph.getAttribute("/CropBox", false);
    if (!cb.isArray()) cb = ph.getAttribute("/MediaBox", true);
    double cx = cb.isArray() ? numOf(cb.getArrayItem(0), 0) : 0, cy2 = cb.isArray() ? numOf(cb.getArrayItem(3), 792) : 792;
    double s = 72.0 / dpi;
    auto lum = [&](int x, int y) {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(rgb.data()) + (static_cast<size_t>(y) * w + x) * 3;
      return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
    };
    for (const BarcodeHit& b : readBarcodes(rgb, w, h)) {
      Events::BarcodeFact f;
      f.page = idx + 1;
      f.format = b.format;
      f.text = b.text;
      f.rotation = b.rotation;
      f.linear = b.linear;
      f.box = {cx + b.x0 * s, cy2 - b.y1 * s, cx + b.x1 * s, cy2 - b.y0 * s, true};
      int x0 = std::max(0, static_cast<int>(b.x0)), x1 = std::min(w - 1, static_cast<int>(b.x1));
      int y0 = std::max(0, static_cast<int>(b.y0)), y1 = std::min(h - 1, static_cast<int>(b.y1));
      double darkSum = 0, lightSum = 0;
      int darkN = 0, lightN = 0, minRun = 1 << 30;
      for (int y = y0; y <= y1; ++y) {
        int run = 0;
        for (int x = x0; x <= x1; ++x) {
          double l = lum(x, y);
          if (l < 128) {
            darkSum += l;
            ++darkN;
            ++run;
          } else {
            lightSum += l;
            ++lightN;
            if (run > 0) minRun = std::min(minRun, run);
            run = 0;
          }
        }
      }
      if (darkN && lightN) f.contrast = (lightSum / lightN - darkSum / darkN) / 255.0 * 100.0;
      if (minRun < (1 << 30)) f.moduleWidthPt = minRun * s;
      int quiet = 0;
      for (int q = 1; q < 400; ++q) {
        bool clear = true;
        for (int y = y0; y <= y1 && clear; y += 2) {
          if (x0 - q < 0 || x1 + q >= w || lum(x0 - q, y) < 128 || lum(x1 + q, y) < 128) clear = false;
        }
        if (!b.linear) {
          for (int x = x0; x <= x1 && clear; x += 2) {
            if (y0 - q < 0 || y1 + q >= h || lum(x, y0 - q) < 128 || lum(x, y1 + q) < 128) clear = false;
          }
        }
        if (!clear) break;
        quiet = q;
      }
      f.quietZonePt = quiet * s;
      {
        double redSum = 0, modSum = 0;
        int rows = 0;
        for (int y = y0 + (y1 - y0) / 4; y <= y1 - (y1 - y0) / 4; y += std::max(1, (y1 - y0) / 16)) {
          std::vector<int> runs;
          bool firstDark = lum(x0, y) < 128;
          bool cur = firstDark;
          int len = 0;
          for (int x = x0; x <= x1; ++x) {
            bool dk = lum(x, y) < 128;
            if (dk == cur) {
              ++len;
            } else {
              runs.push_back(len);
              cur = dk;
              len = 1;
            }
          }
          runs.push_back(len);
          if (!cur && !runs.empty()) runs.pop_back();
          double mod = 0;
          double red = barWidthReduction(runs, firstDark, mod);
          if (mod > 0) {
            redSum += red;
            modSum += mod;
            ++rows;
          }
        }
        if (rows) {
          f.barReductionKnown = true;
          f.barReductionPt = redSum / rows * s;
          f.barReductionPct = redSum / modSum * 100.0;
        }
      }
      ev.barcodes.push_back(f);
    }
    ++idx;
  }
}

void gatherEvents(Ctx& ctx, const unsigned char* inputData, std::size_t inputSize,
                  const PfProfile& prof, Events& ev) {
  ev.inputPath = ctx.opt.inputPath;
  ev.inputData = inputData;
  ev.inputSize = inputSize;
  static const std::set<std::string> sourceChecks = {"layer.membership", "layer.property", "layer.visibility_policy", "layer.processing_step", "structure.artifact", "structure.content_tagging", "content.contrast_sufficient"};
  for (const auto& kv : prof.conds) {
    for (const PfAtom& a : kv.second.atoms) ev.trackSrc = ev.trackSrc || sourceChecks.count(a.token) > 0;
  }
  gatherBarcodeFacts(ctx, prof, ev);
  gatherFileFacts(ctx, inputData, inputSize, ev);
  gatherDocumentFacts(ctx, ev);
  gatherPageFacts(ctx, ev);
  auto clipFlags = [](auto& vec) {
    for (auto& e : vec) {
      if (!e.bbox.valid || !e.clip.valid) continue;
      e.clippedFull = !boxesIntersect(e.bbox, e.clip);
      const Box& b = e.bbox;
      const Box& c = e.clip;
      bool inside = b.x0 >= c.x0 - 0.01 && b.y0 >= c.y0 - 0.01 && b.x1 <= c.x1 + 0.01 && b.y1 <= c.y1 + 0.01;
      e.clippedPart = !e.clippedFull && !inside;
    }
  };
  clipFlags(ev.paints);
  clipFlags(ev.texts);
  clipFlags(ev.images);
  gatherResourceFacts(ctx, prof, ev);
}


bool hitsSatisfy(long long n, const std::string& rule) {
  std::string op;
  size_t k = 0;
  while (k < rule.size() && std::strchr("<>=!", rule[k])) op += rule[k++];
  double v = std::atof(rule.c_str() + k);
  double x = static_cast<double>(n);
  if (op == "<") return x < v;
  if (op == "<=") return x <= v;
  if (op == ">") return x > v;
  if (op == ">=") return x >= v;
  if (op == "!=") return x != v;
  return x == v;
}

bool kindIs(const PfAtom& a, const char* kind) {
  if (a.token != "content.kind" || a.negate) return false;
  if (a.op == "==") return !a.vals.empty() && a.vals[0] == kind;
  if (a.op == "in") return a.vals.size() == 1 && a.vals[0] == kind;
  return false;
}

void reportRuleHits(Ctx& ctx, const PfProfile& prof, const Events& ev) {
  for (const PfRule& rule : prof.rules) {
    struct HitCapture {
      Ctx& c;
      const PfRule& r;
      size_t start;
      ~HitCapture() {
        auto& a = c.res.analysis;
        for (size_t i = start; i < a.size(); ++i) {
          if (a[i].code != "CHECK_HIT") continue;
          if (a[i].pages.empty()) c.ruleHitPages[r.id].insert(0);
          c.ruleHitPages[r.id].insert(a[i].pages.begin(), a[i].pages.end());
        }
        if (r.silent && start < a.size()) a.erase(a.begin() + static_cast<long>(start), a.end());
      }
    } capture{ctx, rule, ctx.res.analysis.size()};
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
      Domain d = domainOf(*a);
      if (d == Domain::kNone) mixed = true;
      else if (d == Domain::kDoc) continue;
      else if (d == Domain::kAny) sawAny = true;
      else if (d == Domain::kFont) sawFont = true;
      else if (dom == Domain::kNone) dom = d;
      else if (dom != d && d == Domain::kPage) continue;
      else if (dom != d && dom == Domain::kPage) dom = d;
      else if (dom != d) mixed = true;
    }
    if (dom == Domain::kPage && sawAny) dom = Domain::kNone;
    if (sawFont) {
      if (dom == Domain::kNone) dom = sawAny ? Domain::kText : Domain::kFont;
      else if (dom != Domain::kText) mixed = true;
    }
    bool anyFallback = false;
    if (dom == Domain::kNone && sawAny) {
      dom = Domain::kPaint;
      anyFallback = true;
      for (const PfAtom* a : atoms) {
        if (kindIs(*a, "text")) {
          dom = Domain::kText;
          anyFallback = false;
        }
        if (kindIs(*a, "image") || kindIs(*a, "image_mask")) {
          dom = Domain::kImage;
          anyFallback = false;
        }
      }
    }
    long long pageCap = rule.maxHitsPerPage;
    if (ctx.opt.hitsPerPage > 0 && (pageCap <= 0 || ctx.opt.hitsPerPage < pageCap)) pageCap = ctx.opt.hitsPerPage;
    bool supported = !mixed;
    long long hits = 0;
    std::set<int> pages;

    int curPage = 0;
    std::string unsupTok;
    auto evalAtom = [&](const PfAtom& a, const void* e) -> bool {
      bool wasSupported = supported;
      bool r = evalCheck(a, subjectFor(dom, e, curPage, ev), ev, supported) > 0;
      if (wasSupported && !supported) unsupTok = a.token;
      return r;
    };
    auto ruleMatches = [&](const void* e) -> bool {
      bool combined = rule.logic != 1;
      XmpNodeScope ruleNodes;
      for (const PfCondition* c : conds) {
        std::optional<XmpNodeScope> condNodes;
        if (rule.logic == 1) condNodes.emplace();
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
    std::map<int, long long> perPage;
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
        if (ctx.opt.visibleLayersOnly && e.layerHidden) continue;
        if (ruleMatches(&e)) {
          if (used) (*used)[i] = true;
          if (ctx.opt.hitsPerDoc > 0 && hits >= ctx.opt.hitsPerDoc) continue;
          if (pageCap > 0 && perPage[e.page] >= pageCap) continue;
          ++perPage[e.page];
          ++hits;
          pages.insert(e.page);
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
        curPage = e.page;
        if (ruleMatches(&e)) {
          if (pageCap > 0 && perPage[e.page] >= pageCap) continue;
          ++perPage[e.page];
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
    if (supported) {
      bool formRule = false;
      for (const PfAtom* a : atoms) formRule = formRule || kindIs(*a, "form_xobject");
      if (formRule) {
        std::map<int, long long> fp;
        hits += sweepFormObjects(conds, rule.logic, ev, fp, supported);
        for (const auto& [pg, n] : fp) {
          perPage[pg] += n;
          pages.insert(pg);
        }
      }
    }
    if (!supported) {
      ctx.res.analysis.push_back(
          {"CHECK_NOT_EVALUATED",
           rule.name + ": uses checks Kura cannot evaluate for this file" + (unsupTok.empty() ? "" : " (" + unsupTok + ")"),
           false});
      continue;
    }
    if (!rule.reportPageHits.empty()) {
      hits = 0;
      pages.clear();
      for (const auto& [pg, n] : perPage) {
        if (n > 0 && hitsSatisfy(n, rule.reportPageHits)) {
          hits += n;
          pages.insert(pg);
        }
      }
    }
    if (!rule.reportDocHits.empty() && !hitsSatisfy(hits, rule.reportDocHits)) {
      hits = 0;
      pages.clear();
    }
    if (hits) {
      int sev = rule.severity < 4 && rule.severity > 0 ? rule.severity : 1;
      std::string detail = std::string(kSevName[sev]) + ": " + rule.name + " (" + std::to_string(hits) + " hit(s)";
      if (!pages.empty()) detail += ", " + pageRangeList(pages);
      detail += ")";
      if (!rule.description.empty()) detail += " - " + rule.description;
      Issue hit;
      hit.code = "CHECK_HIT";
      hit.detail = detail;
      hit.severity = sev;
      hit.pages.assign(pages.begin(), pages.end());
      ctx.res.analysis.push_back(std::move(hit));
    }
  }
}

void passProfile(Ctx& ctx, const unsigned char* inputData, std::size_t inputSize) {
  if (ctx.opt.preflightProfile.empty()) return;
  PfProfile prof;
  if (!parseKuraJson(ctx.opt.preflightProfile, prof)) {
    ctx.res.analysis.push_back({"PROFILE_INVALID", prof.error, false});
    return;
  }
  if (prof.rules.empty()) {
    if (ctx.opt.verifyOnly) ctx.res.analysis.push_back({"PROFILE_EMPTY", "this profile defines no checks to run", false});
    return;
  }
  Events ev;
  gatherEvents(ctx, inputData, inputSize, prof, ev);
  ev.prof = &prof;
  loadProfileShapes(prof, ev);
  reportRuleHits(ctx, prof, ev);
}

namespace {
void addColorRule(ColorJob& j, const std::string& text) {
  static const std::map<std::string, unsigned> objects = {
      {"all", ColorRule::kAllObjects}, {"images", ColorRule::kImageLossy | ColorRule::kImageLossless},
      {"lossy_images", ColorRule::kImageLossy}, {"lossless_images", ColorRule::kImageLossless},
      {"vector", ColorRule::kVector}, {"text", ColorRule::kText}, {"shadings", ColorRule::kShading}};
  static const std::map<std::string, unsigned> sources = {
      {"device_rgb", ColorRule::kUncRgb}, {"calibrated_rgb", ColorRule::kCalRgb}, {"device_cmyk", ColorRule::kUncCmyk},
      {"calibrated_cmyk", ColorRule::kCalCmyk}, {"device_gray", ColorRule::kUncGray}, {"calibrated_gray", ColorRule::kCalGray},
      {"lab", ColorRule::kLab}, {"spot", ColorRule::kSpot}, {"registration", ColorRule::kRegistration}};
  static const std::map<std::string, ColorRule::Action> actions = {
      {"convert", ColorRule::kConvert}, {"keep", ColorRule::kNone}, {"decalibrate", ColorRule::kDecalibrate},
      {"convert_alternate", ColorRule::kConvertAlternate}, {"decalibrate_and_convert", ColorRule::kDecalibrateConvert},
      {"tag", ColorRule::kTag}};
  static const std::map<std::string, int> intents = {
      {"perceptual", 0}, {"relativecolorimetric", 1}, {"relative_colorimetric", 1}, {"saturation", 2},
      {"absolutecolorimetric", 3}, {"absolute_colorimetric", 3}};
  ColorRule r;
  unsigned obj = 0, src = 0;
  std::string tok;
  for (size_t k = 0; k <= text.size(); ++k) {
    char c = k < text.size() ? text[k] : ' ';
    if (c != ' ' && c != ',' && c != ';' && c != '\t') {
      tok += c;
      continue;
    }
    size_t eq = tok.find_first_of("=:");
    std::string v = lower(eq == std::string::npos ? tok : tok.substr(eq + 1));
    tok.clear();
    if (v.empty()) continue;
    if (objects.count(v)) obj |= objects.at(v);
    else if (sources.count(v)) src |= sources.at(v);
    else if (actions.count(v)) r.action = actions.at(v);
    else if (intents.count(v)) r.intent = intents.at(v);
  }
  if (obj) r.objects = obj;
  r.sources = src ? src : ColorRule::kAnySource & ~(ColorRule::kSpot | ColorRule::kRegistration);
  j.rules.push_back(r);
}

void scheduleColorConvert(Options& opt, const PfFix& f) {
  ColorJob& j = opt.colorJob;
  j.enabled = true;
  const std::string& dest = fixParam(f, "destination");
  if (!dest.empty() && dest != "output_intent") j.destName = dest;
  const std::string& embed = fixParam(f, "embed");
  j.embed = embed == "none" ? ColorJob::kEmbedNone : embed == "source" ? ColorJob::kEmbedSource : ColorJob::kEmbedOutputIntent;
  j.preserveBlack = fixParamBool(f, "preserve_black");
  j.blackPointCompensation = fixParamBool(f, "black_point_compensation");
  j.processSeparationsAsCmyk = fixParamBool(f, "process_separations_as_cmyk");
  j.grayToBlackOnly = fixParamBool(f, "grey_to_black");
  if (!fixParam(f, "rgb_grey_to_black_tolerance").empty()) {
    j.rgbGrayToBlackOnly = true;
    for (double& t : j.rgbGrayTolerance) t = fixParamNum(f, "rgb_grey_to_black_tolerance") / 100.0;
  }
  for (const std::string& rule : fixParamList(f, "rules")) addColorRule(j, rule);
  if (!fixParam(f, "device_link").empty()) {
    std::string bytes;
    if (resolveIccByName(opt, fixParam(f, "device_link"), bytes)) j.deviceLink = bytes;
    j.embed = ColorJob::kEmbedNone;
  }
  if (fixParamBool(f, "quick")) {
    j.quick = true;
    if (!fixParam(f, "black_generation").empty()) j.quickBlackGeneration = fixParamNum(f, "black_generation") / 100.0;
    if (!fixParam(f, "undercolour_removal").empty()) j.quickUnderColorRemoval = fixParamNum(f, "undercolour_removal") / 100.0;
    if (j.rules.empty()) {
      ColorRule r;
      r.sources = ColorRule::kUncRgb | ColorRule::kCalRgb;
      j.rules.push_back(r);
    }
    j.embed = ColorJob::kEmbedNone;
  }
}

bool scheduleOutputIntent(Options& opt, const PfFix& f, std::vector<Issue>& notes) {
  if (!fixParam(f, "pages").empty()) {
    const std::string& pages = fixParam(f, "pages");
    opt.pageOutputIntents = pages;
  }
  const std::string& name = fixParam(f, "profile");
  if (!name.empty()) {
    std::string bytes;
    if (!resolveIccByName(opt, name, bytes)) {
      notes.push_back({"ICC_PROFILE_MISSING",
                       "output intent profile \"" + name + "\" was not found; the output intent was left to the standard conversion",
                       false});
      return false;
    }
    opt.destProfile = bytes;
    std::string desc = iccDescription(bytes);
    opt.outputConditionInfo = desc.empty() ? name : desc;
  }
  if (!fixParam(f, "identifier").empty()) opt.outputConditionIdentifier = fixParam(f, "identifier");
  else if (opt.outputConditionIdentifier.empty() && !opt.outputConditionInfo.empty()) {
    opt.outputConditionIdentifier = opt.outputConditionInfo;
  }
  if (!fixParam(f, "condition").empty()) opt.outputConditionInfo = fixParam(f, "condition");
  if (!fixParam(f, "registry").empty()) opt.outputConditionRegistry = fixParam(f, "registry");
  return !name.empty() || !fixParam(f, "identifier").empty() || !fixParam(f, "condition").empty();
}
}

void applyProfileFixes(Options& opt, std::vector<Issue>& notes) {
  PfProfile prof;
  if (!parseKuraJson(opt.preflightProfile, prof) || prof.fixes.empty()) return;
  std::set<std::string> noted;
  auto scheduled = [&](const PfFix& f, const std::string& what) {
    if (noted.insert(f.id).second) notes.push_back({"FIX_SCHEDULED", f.id + ": " + what, true});
  };
  for (const PfFix& f : prof.fixes) {
    if (f.fix < 0) {
      notes.push_back({"FIX_UNKNOWN", f.id + ": not a Kura fix", false});
      continue;
    }
    if (f.id == "color.convert") {
      scheduleColorConvert(opt, f);
      scheduled(f, "colour conversion scheduled");
    } else if (f.id == "image.downsample") {
      double target = fixParamNum(f, "target");
      double above = fixParamNum(f, "above");
      if (target > 0 && (opt.imageMaxPpi <= 0 || target < opt.imageMaxPpi)) opt.imageMaxPpi = target;
      opt.imageDownsampleAbovePpi = std::max(above, opt.imageMaxPpi);
      opt.imageDownsampleKind = fixParam(f, "kind");
      scheduled(f, "images above " + fmtFixed(opt.imageDownsampleAbovePpi, 0) + " ppi are reduced to " + fmtFixed(target, 0) + " ppi");
    } else if (f.id == "image.upsample") {
      double target = fixParamNum(f, "target"), below = fixParamNum(f, "below");
      if (target > 0) {
        opt.imageUpsampleToPpi = target;
        opt.imageUpsampleBelowPpi = below > 0 ? below : target;
      }
      scheduled(f, "images below " + fmtFixed(opt.imageUpsampleBelowPpi, 0) + " ppi are raised");
    } else if (f.id == "font.to_outlines") {
      opt.outlineFonts = true;
      scheduled(f, "text will be drawn as outlines");
    } else if (f.id == "document.linearize") {
      opt.linearize = true;
      scheduled(f, "output will be linearized for fast web view");
    } else if (isContentFix(f.id)) {
      if (f.id == "output_intent.set" && !scheduleOutputIntent(opt, f, notes)) continue;
      std::vector<std::string> ruleIds;
      for (const std::string& name : f.onlyWhere) {
        auto it = prof.ruleIndex.find(name);
        if (it != prof.ruleIndex.end()) ruleIds.push_back(it->second.id);
      }
      if (!ruleIds.empty()) opt.profileFixRules[opt.profileFixOps.size()] = ruleIds;
      opt.profileFixOps.push_back({f.id, f.args});
      scheduled(f, "scheduled");
    } else {
      notes.push_back({"FIX_UNKNOWN", f.id + ": Kura cannot run this fix yet", false});
    }
  }
}

}
