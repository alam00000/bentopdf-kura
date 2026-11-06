#include <lcms2.h>
#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <string>
#include <vector>

#include "assets/cmyk_icc.hh"
#include "assets/srgb_icc.hh"
#include "colorconv.hh"
#include "colorx.hh"
#include "ctx.hh"
#include "fonts_ft.hh"
#include "passes.hh"
#include "limits.hh"
#include "util.hh"

namespace pdfa {
namespace {
struct ColorUsage {
  bool rgb = false;
  bool cmyk = false;
  bool gray = false;
  std::vector<QPDFObjectHandle> cmykScopes;
  std::set<QPDFObjGen> cmykScopeSeen;
  std::vector<QPDFObjectHandle> rgbScopes;
  std::set<QPDFObjGen> rgbScopeSeen;
  std::vector<QPDFObjectHandle> cmykAlternates;
  std::vector<QPDFObjectHandle> processCmyk;
};

void noteScope(std::vector<QPDFObjectHandle>& list, std::set<QPDFObjGen>& seen,
               QPDFObjectHandle res) {
  if (!res.isDictionary()) return;
  if (res.isIndirect()) {
    if (!seen.insert(res.getObjGen()).second) return;
  }
  list.push_back(res);
}

void classifySpace(Ctx& ctx, QPDFObjectHandle cs, QPDFObjectHandle res, ColorUsage& usage,
                   int depth);

thread_local int tBlendFamily = 0;
thread_local bool tPageIntent = false;

int independentFamily(QPDFObjectHandle cs) {
  if (cs.isArray() && cs.getArrayNItems() >= 2) {
    std::string f = nameOf(cs.getArrayItem(0));
    if (f == "/CalRGB") return 3;
    if (f == "/CalGray") return 1;
    if (f == "/ICCBased" && cs.getArrayItem(1).isStream() && cs.getArrayItem(1).getDict().getKey("/N").isInteger()) {
      return static_cast<int>(cs.getArrayItem(1).getDict().getKey("/N").getIntValue());
    }
  }
  return 0;
}

bool defaultCovers(Ctx& ctx, QPDFObjectHandle res, const char* key) {
  if (ctx.opt.verifyOnly && ctx.part >= 4 && tPageIntent && std::string(key) == "/DefaultGray") return true;
  if (ctx.opt.verifyOnly && ctx.part >= 4 && tBlendFamily) {
    std::string k = key;
    if ((k == "/DefaultRGB" && tBlendFamily == 3) || (k == "/DefaultCMYK" && tBlendFamily == 4)) return true;
  }
  if (!ctx.opt.verifyOnly || !res.isDictionary()) return false;
  QPDFObjectHandle csd = res.getKey("/ColorSpace");
  return csd.isDictionary() && !csd.getKey(key).isNull();
}

void classifyByName(Ctx& ctx, const std::string& name, QPDFObjectHandle res, ColorUsage& usage,
                    int depth) {
  if ((name == "/DeviceRGB" || name == "/RGB") && defaultCovers(ctx, res, "/DefaultRGB")) return;
  if ((name == "/DeviceCMYK" || name == "/CMYK") && defaultCovers(ctx, res, "/DefaultCMYK")) return;
  if ((name == "/DeviceGray" || name == "/G") && defaultCovers(ctx, res, "/DefaultGray")) return;
  if (name == "/DeviceRGB" || name == "/RGB" || name == "/CalRGB") {
    if (name != "/CalRGB") {
      usage.rgb = true;
      noteScope(usage.rgbScopes, usage.rgbScopeSeen, res);
    }
    return;
  }
  if (name == "/DeviceCMYK" || name == "/CMYK") {
    usage.cmyk = true;
    noteScope(usage.cmykScopes, usage.cmykScopeSeen, res);
    return;
  }
  if (name == "/DeviceGray" || name == "/G" || name == "/CalGray") {
    if (name != "/CalGray") usage.gray = true;
    return;
  }
  if (name == "/Pattern" || name == "/Indexed" || name == "/I") return;
  if (res.isDictionary() && depth < 8) {
    QPDFObjectHandle csDict = res.getKey("/ColorSpace");
    if (csDict.isDictionary() && csDict.hasKey(name)) {
      classifySpace(ctx, csDict.getKey(name), res, usage, depth + 1);
    }
  }
}

void classifySpace(Ctx& ctx, QPDFObjectHandle cs, QPDFObjectHandle res, ColorUsage& usage,
                   int depth) {
  if (depth > kMaxColorSpaceNest) return;
  if (cs.isName()) {
    classifyByName(ctx, cs.getName(), res, usage, depth);
    return;
  }
  if (!cs.isArray() || cs.getArrayNItems() < 1) return;
  std::string family = nameOf(cs.getArrayItem(0));
  if (family == "/ICCBased" || family == "/CalRGB" || family == "/CalGray" ||
      family == "/Lab") {
    return;
  }
  if (family == "/Indexed" || family == "/I") {
    if (cs.getArrayNItems() >= 2) classifySpace(ctx, cs.getArrayItem(1), res, usage, depth + 1);
    return;
  }
  if (family == "/Separation" && cs.getArrayNItems() >= 3) {
    QPDFObjectHandle alt = cs.getArrayItem(2);
    if (nameIs(alt, "/DeviceCMYK")) {
      if (!defaultCovers(ctx, res, "/DefaultCMYK")) usage.cmykAlternates.push_back(cs);
    } else {
      classifySpace(ctx, alt, res, usage, depth + 1);
    }
    return;
  }
  if (family == "/DeviceN" && cs.getArrayNItems() >= 3) {
    QPDFObjectHandle alt = cs.getArrayItem(2);
    if (nameIs(alt, "/DeviceCMYK")) {
      if (!defaultCovers(ctx, res, "/DefaultCMYK")) usage.cmykAlternates.push_back(cs);
    } else {
      classifySpace(ctx, alt, res, usage, depth + 1);
    }
    if (cs.getArrayNItems() >= 5 && cs.getArrayItem(4).isDictionary()) {
      QPDFObjectHandle attrs = cs.getArrayItem(4);
      QPDFObjectHandle cols = attrs.getKey("/Colorants");
      if (cols.isDictionary()) {
        for (const std::string& ck : cols.getKeys()) {
          classifySpace(ctx, cols.getKey(ck), res, usage, depth + 1);
        }
      }
      QPDFObjectHandle proc = attrs.getKey("/Process");
      if (proc.isDictionary() && nameIs(proc.getKey("/ColorSpace"), "/DeviceCMYK")) {
        usage.processCmyk.push_back(proc);
      }
    }
    return;
  }
  if (family == "/Pattern" && cs.getArrayNItems() >= 2) {
    classifySpace(ctx, cs.getArrayItem(1), res, usage, depth + 1);
    return;
  }
  if (family == "/DeviceRGB" || family == "/DeviceCMYK" || family == "/DeviceGray") {
    classifyByName(ctx, family, res, usage, depth);
  }
}

class OpScanner : public QPDFObjectHandle::ParserCallbacks {
 public:
  OpScanner(Ctx& ctx, QPDFObjectHandle res, ColorUsage& usage, bool fresh = false)
      : ctx(ctx), res(res), usage(usage), fresh(fresh && ctx.opt.verifyOnly) {}

  void handleObject(QPDFObjectHandle obj, size_t, size_t) override {
    if (obj.isOperator() && fresh) {
      std::string op = obj.getOperatorValue();
      static const std::set<std::string> fillOps = {"f", "F", "f*", "B", "B*", "b", "b*"};
      static const std::set<std::string> strokeOps = {"S", "s", "B", "B*", "b", "b*"};
      static const std::set<std::string> textOps = {"Tj", "TJ", "'", "\""};
      if (op == "q") {
        saved.push_back({fillSet, strokeSet, tr});
      } else if (op == "Q" && !saved.empty()) {
        fillSet = std::get<0>(saved.back());
        strokeSet = std::get<1>(saved.back());
        tr = std::get<2>(saved.back());
        saved.pop_back();
      } else if (op == "rg" || op == "k" || op == "g" || op == "cs") {
        fillSet = true;
      } else if (op == "RG" || op == "K" || op == "G" || op == "CS") {
        strokeSet = true;
      } else if (op == "Tr" && !operands.empty() && operands.back().isInteger()) {
        tr = operands.back().getIntValue();
      }
      bool useFill = fillOps.count(op) || (textOps.count(op) && tr != 1 && tr != 3 && tr != 5 && tr != 7);
      bool useStroke = strokeOps.count(op) || (textOps.count(op) && (tr == 1 || tr == 2 || tr == 5 || tr == 6));
      if (((useFill && !fillSet) || (useStroke && !strokeSet)) && !defaultCovers(ctx, res, "/DefaultGray")) {
        usage.gray = true;
      }
    }
    if (obj.isOperator()) {
      std::string op = obj.getOperatorValue();
      if ((op == "rg" || op == "RG") && defaultCovers(ctx, res, "/DefaultRGB")) {
      } else if ((op == "k" || op == "K") && defaultCovers(ctx, res, "/DefaultCMYK")) {
      } else if ((op == "g" || op == "G") && defaultCovers(ctx, res, "/DefaultGray")) {
      } else if (op == "rg" || op == "RG") {
        usage.rgb = true;
        noteScope(usage.rgbScopes, usage.rgbScopeSeen, res);
      } else if (op == "k" || op == "K") {
        usage.cmyk = true;
        noteScope(usage.cmykScopes, usage.cmykScopeSeen, res);
      } else if (op == "g" || op == "G") {
        usage.gray = true;
      } else if ((op == "cs" || op == "CS") && !operands.empty() && operands.back().isName()) {
        classifyByName(ctx, operands.back().getName(), res, usage, 0);
      } else if (op == "sh" && !operands.empty() && operands.back().isName() &&
                 res.isDictionary()) {
        QPDFObjectHandle shd = res.getKey("/Shading");
        if (shd.isDictionary()) {
          QPDFObjectHandle sh = shd.getKey(operands.back().getName());
          QPDFObjectHandle d = sh.isStream() ? sh.getDict() : sh;
          if (d.isDictionary()) classifySpace(ctx, d.getKey("/ColorSpace"), res, usage, 0);
        }
      } else if (op == "BI") {
        inImage = true;
      }
      operands.clear();
      return;
    }
    if (obj.isInlineImage()) {
      inImage = false;
      operands.clear();
      return;
    }
    if (inImage && obj.isName()) {
      if (pendingCs) {
        classifyByName(ctx, obj.getName(), res, usage, 0);
        pendingCs = false;
      } else {
        std::string n = obj.getName();
        pendingCs = (n == "/CS" || n == "/ColorSpace");
      }
      return;
    }
    operands.push_back(obj);
  }

  void handleEOF() override {}

 private:
  Ctx& ctx;
  QPDFObjectHandle res;
  ColorUsage& usage;
  bool fresh;
  bool fillSet = false, strokeSet = false;
  long long tr = 0;
  std::vector<std::tuple<bool, bool, long long>> saved;
  std::vector<QPDFObjectHandle> operands;
  bool inImage = false;
  bool pendingCs = false;
};

void scanContent(Ctx& ctx, QPDFObjectHandle contentHolder, QPDFObjectHandle res,
                 ColorUsage& usage, bool fresh = false) {
  try {
    OpScanner scanner(ctx, res, usage, fresh);
    if (contentHolder.isStream()) {
      QPDFObjectHandle::parseContentStream(contentHolder, &scanner);
    } else if (contentHolder.isArray() || contentHolder.isDictionary()) {
      QPDFObjectHandle::parseContentStream(contentHolder, &scanner);
    }
  } catch (...) {
    ctx.scanIncomplete("page content colour scan");
  }
}

thread_local const std::set<QPDFObjGen>* tColorReached = nullptr;

bool unreached(QPDFObjectHandle o) {
  return tColorReached && o.isIndirect() && !tColorReached->count(o.getObjGen());
}

void scanResources(Ctx& ctx, QPDFObjectHandle res, Visited& visited, ColorUsage& usage,
                   int depth = 0) {
  if (depth > kMaxPageTreeNest) return;
  if (!res.isDictionary() || !visited.enter(res)) return;
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (xod.isDictionary()) {
    for (const std::string& k : xod.getKeys()) {
      QPDFObjectHandle xo = xod.getKey(k);
      if (!xo.isStream() || unreached(xo) || !visited.enter(xo)) continue;
      QPDFObjectHandle d = xo.getDict();
      std::string subtype = nameOf(d.getKey("/Subtype"));
      if (subtype == "/Image") {
        classifySpace(ctx, d.getKey("/ColorSpace"), res, usage, 0);
      } else if (subtype == "/Form") {
        QPDFObjectHandle inner =
            d.getKey("/Resources").isDictionary() ? d.getKey("/Resources") : res;
        QPDFObjectHandle group = d.getKey("/Group");
        if (group.isDictionary()) classifySpace(ctx, group.getKey("/CS"), inner, usage, 0);
        int savedFamily = tBlendFamily;
        if (group.isDictionary() && independentFamily(group.getKey("/CS")) && group.getKey("/I").isBool() &&
            group.getKey("/I").getBoolValue()) {
          tBlendFamily = independentFamily(group.getKey("/CS"));
        }
        scanContent(ctx, xo, inner, usage);
        scanResources(ctx, d.getKey("/Resources"), visited, usage, depth + 1);
        tBlendFamily = savedFamily;
      }
    }
  }
  QPDFObjectHandle shd = res.getKey("/Shading");
  if (shd.isDictionary() && !tColorReached) {
    for (const std::string& k : shd.getKeys()) {
      QPDFObjectHandle sh = shd.getKey(k);
      QPDFObjectHandle d = sh.isStream() ? sh.getDict() : sh;
      if (d.isDictionary()) classifySpace(ctx, d.getKey("/ColorSpace"), res, usage, 0);
    }
  }
  QPDFObjectHandle pat = res.getKey("/Pattern");
  if (pat.isDictionary()) {
    for (const std::string& k : pat.getKeys()) {
      QPDFObjectHandle p = pat.getKey(k);
      if (p.isStream() && unreached(p)) continue;
      QPDFObjectHandle pd = p.isStream() ? p.getDict() : p;
      if (pd.isDictionary() && pd.getKey("/Shading").isInitialized()) {
        QPDFObjectHandle sh = pd.getKey("/Shading");
        QPDFObjectHandle shDict = sh.isStream() ? sh.getDict() : sh;
        if (shDict.isDictionary()) {
          classifySpace(ctx, shDict.getKey("/ColorSpace"), res, usage, 0);
        }
      }
      if (p.isStream() && visited.enter(p)) {
        QPDFObjectHandle inner = p.getDict().getKey("/Resources").isDictionary()
                                     ? p.getDict().getKey("/Resources")
                                     : res;
        scanContent(ctx, p, inner, usage);
        scanResources(ctx, p.getDict().getKey("/Resources"), visited, usage, depth + 1);
      } else if (p.isDictionary()) {
        QPDFObjectHandle sh = p.getKey("/Shading");
        QPDFObjectHandle d = sh.isStream() ? sh.getDict() : sh;
        if (d.isDictionary()) classifySpace(ctx, d.getKey("/ColorSpace"), res, usage, 0);
      }
    }
  }
  QPDFObjectHandle csd = res.getKey("/ColorSpace");
  if (csd.isDictionary() && !tColorReached) {
    for (const std::string& k : csd.getKeys()) {
      if (k == "/DefaultRGB" || k == "/DefaultCMYK" || k == "/DefaultGray") continue;
      QPDFObjectHandle cs = csd.getKey(k);
      if (cs.isArray() && cs.getArrayNItems() >= 1) {
        std::string family = nameOf(cs.getArrayItem(0));
        if (family == "/Separation" || family == "/DeviceN") {
          classifySpace(ctx, cs, res, usage, 0);
        }
      }
    }
  }
  QPDFObjectHandle fonts = res.getKey("/Font");
  if (fonts.isDictionary()) {
    for (const std::string& k : fonts.getKeys()) {
      QPDFObjectHandle fnt = fonts.getKey(k);
      if (!fnt.isDictionary()) continue;
      QPDFObjectHandle cp = fnt.getKey("/CharProcs");
      if (cp.isDictionary() && visited.enter(cp)) {
        QPDFObjectHandle inner =
            fnt.getKey("/Resources").isDictionary() ? fnt.getKey("/Resources") : res;
        for (const std::string& g : cp.getKeys()) {
          QPDFObjectHandle glyph = cp.getKey(g);
          if (glyph.isStream() && visited.enter(glyph)) scanContent(ctx, glyph, inner, usage);
        }
        scanResources(ctx, fnt.getKey("/Resources"), visited, usage, depth + 1);
      }
    }
  }
}

bool resTransparent(QPDFObjectHandle res, Visited& v, int depth) {
  if (depth > 16 || !res.isDictionary() || !v.enter(res)) return false;
  QPDFObjectHandle gsd = res.getKey("/ExtGState");
  for (const std::string& k : gsd.isDictionary() ? gsd.getKeys() : std::set<std::string>()) {
    QPDFObjectHandle gs = gsd.getKey(k);
    if (!gs.isDictionary()) continue;
    QPDFObjectHandle sm = gs.getKey("/SMask");
    if (!sm.isNull() && !nameIs(sm, "/None")) return true;
    std::string bm = nameOf(gs.getKey("/BM"));
    if (gs.getKey("/BM").isArray() && gs.getKey("/BM").getArrayNItems() > 0) bm = nameOf(gs.getKey("/BM").getArrayItem(0));
    if (!bm.empty() && bm != "/Normal" && bm != "/Compatible") return true;
    if (gs.getKey("/CA").isNumber() && gs.getKey("/CA").getNumericValue() < 1.0) return true;
    if (gs.getKey("/ca").isNumber() && gs.getKey("/ca").getNumericValue() < 1.0) return true;
  }
  QPDFObjectHandle xod = res.getKey("/XObject");
  for (const std::string& k : xod.isDictionary() ? xod.getKeys() : std::set<std::string>()) {
    QPDFObjectHandle xo = xod.getKey(k);
    if (!xo.isStream()) continue;
    QPDFObjectHandle d = xo.getDict();
    if (nameIs(d.getKey("/Subtype"), "/Image")) {
      if (d.getKey("/SMask").isStream() || (d.getKey("/SMaskInData").isInteger() && d.getKey("/SMaskInData").getIntValue() > 0)) return true;
    } else if (nameIs(d.getKey("/Subtype"), "/Form")) {
      if (d.getKey("/Group").isDictionary() && nameIs(d.getKey("/Group").getKey("/S"), "/Transparency")) return true;
      if (resTransparent(d.getKey("/Resources"), v, depth + 1)) return true;
    }
  }
  QPDFObjectHandle pat = res.getKey("/Pattern");
  for (const std::string& k : pat.isDictionary() ? pat.getKeys() : std::set<std::string>()) {
    QPDFObjectHandle p = pat.getKey(k);
    if (p.isStream() && resTransparent(p.getDict().getKey("/Resources"), v, depth + 1)) return true;
  }
  QPDFObjectHandle fonts = res.getKey("/Font");
  for (const std::string& k : fonts.isDictionary() ? fonts.getKeys() : std::set<std::string>()) {
    QPDFObjectHandle f = fonts.getKey(k);
    if (f.isDictionary() && resTransparent(f.getKey("/Resources"), v, depth + 1)) return true;
  }
  return false;
}

int pagesNeedingBlendSpace(Ctx& ctx) {
  int n = 0;
  for (auto& ph : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle group = page.getKey("/Group");
    if (group.isDictionary() && !group.getKey("/CS").isNull()) continue;
    QPDFObjectHandle pois = page.getKey("/OutputIntents");
    bool pageIntent = false;
    for (int i = 0; ctx.part >= 4 && pois.isArray() && i < pois.getArrayNItems(); ++i) {
      pageIntent = pageIntent || nameIs(pois.getArrayItem(i).getKey("/S"), "/GTS_PDFA1");
    }
    if (pageIntent) continue;
    Visited v;
    bool transparent = group.isDictionary() && nameIs(group.getKey("/S"), "/Transparency");
    QPDFObjectHandle annots = page.getKey("/Annots");
    for (int i = 0; annots.isArray() && i < annots.getArrayNItems() && !transparent; ++i) {
      QPDFObjectHandle a = annots.getArrayItem(i);
      if (!a.isDictionary()) continue;
      std::string bm = nameOf(a.getKey("/BM"));
      if ((!bm.empty() && bm != "/Normal" && bm != "/Compatible") ||
          (a.getKey("/CA").isNumber() && a.getKey("/CA").getNumericValue() < 1.0) ||
          (a.getKey("/ca").isNumber() && a.getKey("/ca").getNumericValue() < 1.0)) {
        transparent = true;
      }
      for (QPDFObjectHandle st : normalAppearanceStreams(a)) {
        transparent = transparent || resTransparent(st.getDict().getKey("/Resources"), v, 0);
      }
    }
    if (transparent || resTransparent(ph.getAttribute("/Resources", false), v, 0)) ++n;
  }
  return n;
}

ColorUsage scanUsage(Ctx& ctx) {
  ColorUsage usage;
  std::set<QPDFObjGen> reached;
  if (ctx.opt.verifyOnly) {
    collectFontUse(ctx.pdf, &reached);
    tColorReached = &reached;
  }
  struct Reset {
    ~Reset() {
      tColorReached = nullptr;
      tBlendFamily = 0;
      tPageIntent = false;
    }
  } reset;
  Visited visited;
  QPDFPageDocumentHelper dh(ctx.pdf);
  int kuraPage1 = 0;
  for (auto& ph : dh.getAllPages()) {
    PageScope kuraScope1(ctx, ++kuraPage1);
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle res = ph.getAttribute("/Resources", false);
    QPDFObjectHandle group = page.getKey("/Group");
    tBlendFamily = group.isDictionary() ? independentFamily(group.getKey("/CS")) : 0;
    tPageIntent = false;
    QPDFObjectHandle pois = page.getKey("/OutputIntents");
    for (int i = 0; ctx.part >= 4 && pois.isArray() && i < pois.getArrayNItems(); ++i) {
      QPDFObjectHandle prof = pois.getArrayItem(i).getKey("/DestOutputProfile");
      if (nameIs(pois.getArrayItem(i).getKey("/S"), "/GTS_PDFA1") && prof.isStream()) {
        tBlendFamily = prof.getDict().getKey("/N").isInteger() ? static_cast<int>(prof.getDict().getKey("/N").getIntValue()) : 3;
        tPageIntent = true;
      }
    }
    if (group.isDictionary()) classifySpace(ctx, group.getKey("/CS"), res, usage, 0);
    scanContent(ctx, page.getKey("/Contents"), res, usage, true);
    scanResources(ctx, res, visited, usage);
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (annots.isArray()) {
      for (int i = 0; i < annots.getArrayNItems(); ++i) {
        QPDFObjectHandle a = annots.getArrayItem(i);
        if (!a.isDictionary()) continue;
        if (ctx.opt.verifyOnly && a.getKey("/3DD").isStream()) {
          classifySpace(ctx, a.getKey("/3DD").getDict().getKey("/ColorSpace"), res, usage, 0);
        }
        std::vector<QPDFObjectHandle> streams = normalAppearanceStreams(a);
        for (QPDFObjectHandle s : streams) {
          if (!visited.enter(s)) continue;
          QPDFObjectHandle inner = s.getDict().getKey("/Resources");
          scanContent(ctx, s, inner.isDictionary() ? inner : res, usage, true);
          scanResources(ctx, inner, visited, usage);
        }
      }
    }
  }
  return usage;
}
}

const char* iccSpaceTag(const std::string& p) {
  if (p.size() < 20) return nullptr;
  if (std::memcmp(p.data() + 16, "RGB ", 4) == 0) return "RGB ";
  if (std::memcmp(p.data() + 16, "CMYK", 4) == 0) return "CMYK";
  if (std::memcmp(p.data() + 16, "GRAY", 4) == 0) return "GRAY";
  return nullptr;
}

namespace {
}

QPDFObjectHandle buildIccStream(Ctx& ctx, const unsigned char* data, unsigned int len, int n) {
  std::string bytes(reinterpret_cast<const char*>(data), len);
  const std::string& override_ = n == 4   ? ctx.opt.defaultCmykProfile
                                 : n == 1 ? ctx.opt.defaultGrayProfile
                                          : ctx.opt.defaultRgbProfile;
  if (!override_.empty()) {
    const char* want = n == 4 ? "CMYK" : (n == 1 ? "GRAY" : "RGB ");
    const char* got = iccSpaceTag(override_);
    if (!got || std::memcmp(got, want, 4) != 0) {
      ctx.fatal("DEFAULT_PROFILE_MISMATCH",
                std::string("the supplied default profile is not a valid ") + want +
                    " ICC profile");
    } else {
      bytes = override_;
      ctx.issue("DEFAULT_PROFILE_USED",
                std::string("used the caller-supplied ") + want + " ICC profile", true);
    }
  }
  QPDFObjectHandle stream = QPDFObjectHandle::newStream(&ctx.pdf, bytes);
  stream.getDict().replaceKey("/N", QPDFObjectHandle::newInteger(n));
  return ctx.pdf.makeIndirectObject(stream);
}

namespace {
bool validExistingProfile(Ctx& ctx, QPDFObjectHandle profile, std::string& csOut) {
  if (!profile.isStream()) return false;
  std::string header;
  try {
    auto buf = profile.getStreamData(qpdf_dl_all);
    if (buf->getSize() < 132) return false;
    header.assign(reinterpret_cast<const char*>(buf->getBuffer()), 132);
  } catch (...) {
    return false;
  }
  int major = static_cast<unsigned char>(header[8]);
  double version = major + ((static_cast<unsigned char>(header[9]) >> 4) / 10.0);
  double bound = ctx.part == 1 ? 3.0 : 5.0;
  if (version >= bound) return false;
  std::string devClass = header.substr(12, 4);
  if (devClass != "prtr" && devClass != "mntr") return false;
  std::string cs = header.substr(16, 4);
  if (cs != "RGB " && cs != "CMYK" && cs != "GRAY") return false;
  csOut = cs;
  return true;
}

int altComponents(QPDFObjectHandle alt) {
  if (alt.isName()) {
    std::string n = alt.getName();
    if (n == "/DeviceCMYK") return 4;
    if (n == "/DeviceRGB") return 3;
    if (n == "/DeviceGray") return 1;
    return 4;
  }
  if (alt.isArray() && alt.getArrayNItems() >= 2 &&
      nameOf(alt.getArrayItem(0)) == "/ICCBased") {
    QPDFObjectHandle prof = alt.getArrayItem(1);
    if (prof.isStream() && prof.getDict().getKey("/N").isInteger()) {
      return static_cast<int>(prof.getDict().getKey("/N").getIntValue());
    }
  }
  if (alt.isArray() && alt.getArrayNItems() >= 1) {
    std::string fam = nameOf(alt.getArrayItem(0));
    if (fam == "/CalRGB" || fam == "/Lab") return 3;
    if (fam == "/CalGray") return 1;
  }
  return 4;
}

void collectSpecialSpaces(QPDFObjectHandle o, Visited& visited,
                          std::vector<QPDFObjectHandle>& seps,
                          std::vector<QPDFObjectHandle>& dns, int depth = 0) {
  if (depth > kMaxResourceNest) return;
  if (o.isIndirect() && !visited.enter(o)) return;
  if (o.isStream()) {
    collectSpecialSpaces(o.getDict(), visited, seps, dns, depth + 1);
    return;
  }
  if (o.isArray()) {
    std::string fam = o.getArrayNItems() >= 1 ? nameOf(o.getArrayItem(0)) : "";
    if (fam == "/Separation" && o.getArrayNItems() >= 3) seps.push_back(o);
    if (fam == "/DeviceN" && o.getArrayNItems() >= 3) dns.push_back(o);
    for (int i = 0; i < o.getArrayNItems(); ++i) {
      QPDFObjectHandle v = o.getArrayItem(i);
      if (!v.isIndirect()) collectSpecialSpaces(v, visited, seps, dns, depth + 1);
    }
    return;
  }
  if (o.isDictionary()) {
    for (const std::string& k : o.getKeys()) {
      QPDFObjectHandle v = o.getKey(k);
      if (!v.isIndirect()) collectSpecialSpaces(v, visited, seps, dns, depth + 1);
    }
  }
}

bool sameValue(QPDFObjectHandle a, QPDFObjectHandle b, int depth = 0) {
  if (depth > 16) return false;
  if (a.isIndirect() && b.isIndirect() && a.getObjGen() == b.getObjGen()) return true;
  if (a.isStream() && b.isStream()) {
    try {
      auto x = a.getStreamData(qpdf_dl_generalized);
      auto y = b.getStreamData(qpdf_dl_generalized);
      if (x->getSize() != y->getSize() || std::memcmp(x->getBuffer(), y->getBuffer(), x->getSize()) != 0) return false;
    } catch (...) {
      return false;
    }
    QPDFObjectHandle da = a.getDict().shallowCopy(), db = b.getDict().shallowCopy();
    for (const char* k : {"/Length", "/Filter", "/DecodeParms"}) {
      da.removeKey(k);
      db.removeKey(k);
    }
    return sameValue(da, db, depth + 1);
  }
  if (a.isArray() && b.isArray()) {
    if (a.getArrayNItems() != b.getArrayNItems()) return false;
    for (int i = 0; i < a.getArrayNItems(); ++i) {
      if (!sameValue(a.getArrayItem(i), b.getArrayItem(i), depth + 1)) return false;
    }
    return true;
  }
  if (a.isDictionary() && b.isDictionary()) {
    if (a.getKeys() != b.getKeys()) return false;
    for (const std::string& k : a.getKeys()) {
      if (!sameValue(a.getKey(k), b.getKey(k), depth + 1)) return false;
    }
    return true;
  }
  if (a.isNumber() && b.isNumber()) return a.getNumericValue() == b.getNumericValue();
  return a.unparseResolved() == b.unparseResolved();
}

struct OverprintState {
  bool fillIcc = false, strokeIcc = false, OP = false, op = false;
  long long opm = 0, tr = 0;
};

thread_local std::vector<QPDFObjectHandle>* tUsedIcc = nullptr;

bool iccCmykSpace(QPDFObjectHandle cs, QPDFObjectHandle res) {
  if (cs.isName() && res.isDictionary() && res.getKey("/ColorSpace").isDictionary()) {
    cs = res.getKey("/ColorSpace").getKey(cs.getName());
  }
  bool icc = cs.isArray() && cs.getArrayNItems() >= 2 && nameIs(cs.getArrayItem(0), "/ICCBased") &&
             cs.getArrayItem(1).isStream() && cs.getArrayItem(1).getDict().getKey("/N").isInteger() &&
             cs.getArrayItem(1).getDict().getKey("/N").getIntValue() == 4;
  if (icc && tUsedIcc) tUsedIcc->push_back(cs.getArrayItem(1));
  if (!icc && tUsedIcc && cs.isArray() && cs.getArrayNItems() >= 3 &&
      (nameIs(cs.getArrayItem(0), "/DeviceN") || nameIs(cs.getArrayItem(0), "/Separation"))) {
    iccCmykSpace(cs.getArrayItem(2), QPDFObjectHandle::newNull());
  }
  return icc;
}

bool sameProfile(QPDFObjectHandle a, QPDFObjectHandle b) {
  if (!a.isStream() || !b.isStream()) return false;
  if (a.getObjGen() == b.getObjGen()) return true;
  std::string x, y;
  try {
    auto bx = a.getStreamData(qpdf_dl_generalized);
    auto by = b.getStreamData(qpdf_dl_generalized);
    x.assign(reinterpret_cast<const char*>(bx->getBuffer()), bx->getSize());
    y.assign(reinterpret_cast<const char*>(by->getBuffer()), by->getSize());
  } catch (...) {
    return false;
  }
  if (x.size() != y.size() || x.size() < 100) return false;
  std::string idx = x.substr(84, 16), idy = y.substr(84, 16), zero(16, '\0');
  if (idx != zero && idy != zero && idx != idy) return false;
  return x.compare(0, 84, y, 0, 84) == 0 && x.compare(100, std::string::npos, y, 100, std::string::npos) == 0;
}

class OverprintScan : public QPDFObjectHandle::ParserCallbacks {
 public:
  OverprintScan(QPDFObjectHandle res, OverprintState st, int& hits, int depth, std::set<QPDFObjGen>& active)
      : res(res), st(st), hits(hits), depth(depth), active(active) {}

  static void run(QPDFObjectHandle holder, QPDFObjectHandle res, OverprintState st, int& hits, int depth,
                  std::set<QPDFObjGen>& active) {
    if (depth > 16 || (holder.isIndirect() && !active.insert(holder.getObjGen()).second)) return;
    OverprintScan sc(res, st, hits, depth, active);
    try {
      if (holder.isStream()) holder.parseAsContents(&sc);
      else QPDFPageObjectHelper(holder).parseContents(&sc);
    } catch (...) {
    }
    if (holder.isIndirect()) active.erase(holder.getObjGen());
  }

  void handleObject(QPDFObjectHandle obj) override {
    if (!obj.isOperator()) {
      ops.push_back(obj);
      return;
    }
    std::string op = obj.getOperatorValue();
    static const std::set<std::string> fillOps = {"f", "F", "f*", "B", "B*", "b", "b*"};
    static const std::set<std::string> strokeOps = {"S", "s", "B", "B*", "b", "b*"};
    static const std::set<std::string> textOps = {"Tj", "TJ", "'", "\""};
    QPDFObjectHandle last = ops.empty() ? QPDFObjectHandle::newNull() : ops.back();
    if (op == "q") {
      saved.push_back(st);
    } else if (op == "Q" && !saved.empty()) {
      st = saved.back();
      saved.pop_back();
    } else if (op == "cs") {
      st.fillIcc = iccCmykSpace(last, res);
    } else if (op == "CS") {
      st.strokeIcc = iccCmykSpace(last, res);
    } else if (op == "rg" || op == "g" || op == "k") {
      st.fillIcc = false;
    } else if (op == "RG" || op == "G" || op == "K") {
      st.strokeIcc = false;
    } else if (op == "Tr" && last.isInteger()) {
      st.tr = last.getIntValue();
    } else if (op == "gs" && last.isName() && res.isDictionary() && res.getKey("/ExtGState").isDictionary()) {
      QPDFObjectHandle gs = res.getKey("/ExtGState").getKey(last.getName());
      if (gs.isDictionary()) {
        if (gs.getKey("/OPM").isNumber()) st.opm = static_cast<long long>(gs.getKey("/OPM").getNumericValue());
        if (gs.getKey("/OP").isBool()) {
          st.OP = gs.getKey("/OP").getBoolValue();
          if (!gs.getKey("/op").isBool()) st.op = st.OP;
        }
        if (gs.getKey("/op").isBool()) st.op = gs.getKey("/op").getBoolValue();
      }
    } else if (op == "Do" && last.isName() && res.isDictionary() && res.getKey("/XObject").isDictionary()) {
      QPDFObjectHandle xo = res.getKey("/XObject").getKey(last.getName());
      if (xo.isStream() && nameIs(xo.getDict().getKey("/Subtype"), "/Form")) {
        QPDFObjectHandle inner = xo.getDict().getKey("/Resources");
        run(xo, inner.isDictionary() ? inner : res, st, hits, depth + 1, active);
      }
    }
    bool fill = fillOps.count(op) || (textOps.count(op) && st.tr != 1 && st.tr != 3 && st.tr != 5 && st.tr != 7);
    bool stroke = strokeOps.count(op) || (textOps.count(op) && (st.tr == 1 || st.tr == 2 || st.tr == 5 || st.tr == 6));
    if (st.opm == 1 && ((fill && st.fillIcc && st.op) || (stroke && st.strokeIcc && st.OP))) ++hits;
    ops.clear();
  }

  void handleEOF() override {}

 private:
  QPDFObjectHandle res;
  OverprintState st;
  int& hits;
  int depth;
  std::set<QPDFObjGen>& active;
  std::vector<QPDFObjectHandle> ops;
  std::vector<OverprintState> saved;
};

int iccIdenticalViolations(Ctx& ctx, QPDFObjectHandle keepIntent) {
  QPDFObjectHandle oiProf = keepIntent.isDictionary() ? keepIntent.getKey("/DestOutputProfile") : QPDFObjectHandle::newNull();
  int hits = 0;
  for (auto& ph : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    std::vector<QPDFObjectHandle> used;
    tUsedIcc = &used;
    std::set<QPDFObjGen> active;
    int ignored = 0;
    OverprintScan::run(page, ph.getAttribute("/Resources", false), OverprintState(), ignored, 0, active);
    tUsedIcc = nullptr;
    QPDFObjectHandle gcs = page.getKey("/Group").isDictionary() ? page.getKey("/Group").getKey("/CS") : QPDFObjectHandle::newNull();
    QPDFObjectHandle groupProf = gcs.isArray() && gcs.getArrayNItems() >= 2 && nameIs(gcs.getArrayItem(0), "/ICCBased")
                                     ? gcs.getArrayItem(1)
                                     : QPDFObjectHandle::newNull();
    QPDFObjectHandle pageProf = QPDFObjectHandle::newNull();
    QPDFObjectHandle pois = page.getKey("/OutputIntents");
    for (int i = 0; pois.isArray() && i < pois.getArrayNItems(); ++i) {
      if (nameIs(pois.getArrayItem(i).getKey("/S"), "/GTS_PDFA1")) pageProf = pois.getArrayItem(i).getKey("/DestOutputProfile");
    }
    for (QPDFObjectHandle& p : used) {
      if (sameProfile(p, pageProf.isStream() ? pageProf : oiProf) || sameProfile(p, groupProf)) ++hits;
    }
  }
  return hits;
}

int overprintViolations(Ctx& ctx) {
  int hits = 0;
  std::set<QPDFObjGen> active;
  for (auto& ph : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle res = ph.getAttribute("/Resources", false);
    OverprintScan::run(page, res, OverprintState(), hits, 0, active);
    QPDFObjectHandle annots = page.getKey("/Annots");
    for (int i = 0; annots.isArray() && i < annots.getArrayNItems(); ++i) {
      if (!annots.getArrayItem(i).isDictionary()) continue;
      for (QPDFObjectHandle st : normalAppearanceStreams(annots.getArrayItem(i))) {
        QPDFObjectHandle inner = st.getDict().getKey("/Resources");
        OverprintScan::run(st, inner.isDictionary() ? inner : res, OverprintState(), hits, 0, active);
      }
    }
  }
  return hits;
}

bool sameColorRef(QPDFObjectHandle a, QPDFObjectHandle b) {
  if (a.isIndirect() && b.isIndirect()) return a.getObjGen() == b.getObjGen();
  if (a.isName() && b.isName()) return a.getName() == b.getName();
  return false;
}

void fixSpecialColorSpaces(Ctx& ctx) {
  std::vector<QPDFObjectHandle> seps, dns;
  Visited visited;
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    collectSpecialSpaces(obj, visited, seps, dns);
  }
  int renamed = 0, deduped = 0, colorants = 0;
  auto generatedName = [](const std::string& bare) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "/C%08X", fnv1a32(bare));
    return std::string(buf);
  };
  auto fixName = [&](QPDFObjectHandle arr, int idx) {
    QPDFObjectHandle nm = arr.getArrayItem(idx);
    if (!nm.isName()) return;
    std::string bare = nm.getName().substr(1);
    if (validUtf8(bare)) return;
    arr.setArrayItem(idx, QPDFObjectHandle::newName(generatedName(bare)));
    ++renamed;
  };
  std::map<std::string, std::pair<QPDFObjectHandle, QPDFObjectHandle>> canon;
  for (QPDFObjectHandle sep : seps) {
    fixName(sep, 1);
    if (sep.getArrayNItems() < 4) continue;
    std::string name = nameOf(sep.getArrayItem(1));
    if (name.empty() || name == "/All" || name == "/None") continue;
    auto it = canon.find(name);
    if (it == canon.end()) {
      QPDFObjectHandle alt = sep.getArrayItem(2);
      QPDFObjectHandle tint = sep.getArrayItem(3);
      if (!alt.isIndirect() && (alt.isArray() || alt.isDictionary())) {
        alt = ctx.pdf.makeIndirectObject(alt);
        sep.setArrayItem(2, alt);
      }
      if (!tint.isIndirect()) {
        tint = ctx.pdf.makeIndirectObject(tint);
        sep.setArrayItem(3, tint);
      }
      canon[name] = {alt, tint};
    } else {
      QPDFObjectHandle alt = sep.getArrayItem(2);
      QPDFObjectHandle tint = sep.getArrayItem(3);
      bool sameAlt = sameColorRef(alt, it->second.first) || (ctx.opt.verifyOnly && sameValue(alt, it->second.first));
      bool sameTint = sameColorRef(tint, it->second.second) || (ctx.opt.verifyOnly && sameValue(tint, it->second.second));
      if (!sameAlt || !sameTint) {
        if (it->second.first.isIndirect() || it->second.first.isName()) {
          sep.setArrayItem(2, it->second.first);
        }
        sep.setArrayItem(3, it->second.second);
        ++deduped;
      }
    }
  }
  int malformedDn = 0;
  for (QPDFObjectHandle dn : dns) {
    QPDFObjectHandle names = dn.getArrayItem(1);
    std::map<std::string, std::string> dnRenames;
    if (names.isArray()) {
      for (int i = 0; i < names.getArrayNItems(); ++i) {
        QPDFObjectHandle nm = names.getArrayItem(i);
        if (nm.isName() && !validUtf8(nm.getName().substr(1))) {
          std::string fresh = generatedName(nm.getName().substr(1));
          dnRenames[nm.getName()] = fresh;
          names.setArrayItem(i, QPDFObjectHandle::newName(fresh));
          ++renamed;
        }
      }
    }
    if (!ctx.isA() || ctx.part < 2) continue;
    if (!names.isArray()) continue;
    if (dn.getArrayNItems() < 4) {
      ++malformedDn;
      continue;
    }
    QPDFObjectHandle attrs = dn.getArrayNItems() >= 5 ? dn.getArrayItem(4)
                                                      : QPDFObjectHandle::newNull();
    if (!attrs.isDictionary()) {
      attrs = QPDFObjectHandle::newDictionary();
      if (dn.getArrayNItems() >= 5) {
        dn.setArrayItem(4, attrs);
      } else {
        dn.appendItem(attrs);
      }
      attrs = dn.getArrayItem(4);
    }
    if (!attrs.isDictionary()) continue;
    QPDFObjectHandle colDict = attrs.getKey("/Colorants");
    if (!colDict.isDictionary()) {
      attrs.replaceKey("/Colorants", QPDFObjectHandle::newDictionary());
      colDict = attrs.getKey("/Colorants");
    }
    for (const auto& kv : dnRenames) {
      QPDFObjectHandle moved = colDict.getKey(kv.first);
      if (!moved.isNull()) {
        colDict.removeKey(kv.first);
        colDict.replaceKey(kv.second, moved);
      }
    }
    for (const std::string& ck : colDict.getKeys()) {
      QPDFObjectHandle csep = colDict.getKey(ck);
      if (!csep.isArray() || csep.getArrayNItems() < 4) continue;
      auto known = canon.find(ck);
      if (known == canon.end()) continue;
      QPDFObjectHandle alt2 = csep.getArrayItem(2);
      QPDFObjectHandle tint2 = csep.getArrayItem(3);
      bool sameAlt = sameColorRef(alt2, known->second.first) || (ctx.opt.verifyOnly && sameValue(alt2, known->second.first));
      bool sameTint = sameColorRef(tint2, known->second.second) || (ctx.opt.verifyOnly && sameValue(tint2, known->second.second));
      if (!sameAlt || !sameTint) {
        csep.setArrayItem(2, known->second.first);
        csep.setArrayItem(3, known->second.second);
        ++deduped;
      }
    }
    QPDFObjectHandle alt = dn.getArrayItem(2);
    int comps = altComponents(alt);
    bool added = false;
    for (int i = 0; i < names.getArrayNItems(); ++i) {
      QPDFObjectHandle nm = names.getArrayItem(i);
      if (!nm.isName()) continue;
      std::string name = nm.getName();
      if (name == "/None" || colDict.hasKey(name)) continue;
      auto known = canon.find(name);
      if (known != canon.end()) {
        QPDFObjectHandle sep = QPDFObjectHandle::newArray();
        sep.appendItem(QPDFObjectHandle::newName("/Separation"));
        sep.appendItem(nm);
        sep.appendItem(known->second.first);
        sep.appendItem(known->second.second);
        colDict.replaceKey(name, ctx.pdf.makeIndirectObject(sep));
        ++colorants;
        continue;
      }
      QPDFObjectHandle fn = QPDFObjectHandle::newDictionary();
      fn.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(2));
      fn.replaceKey("/Domain", QPDFObjectHandle::parse("[0 1]"));
      fn.replaceKey("/N", QPDFObjectHandle::newInteger(1));
      QPDFObjectHandle c0 = QPDFObjectHandle::newArray();
      QPDFObjectHandle c1 = QPDFObjectHandle::newArray();
      for (int c = 0; c < comps; ++c) {
        c0.appendItem(QPDFObjectHandle::newInteger(0));
        c1.appendItem(QPDFObjectHandle::newInteger(1));
      }
      fn.replaceKey("/C0", c0);
      fn.replaceKey("/C1", c1);
      QPDFObjectHandle sep = QPDFObjectHandle::newArray();
      sep.appendItem(QPDFObjectHandle::newName("/Separation"));
      sep.appendItem(nm);
      sep.appendItem(alt.isIndirect() || alt.isName() ? alt
                                                      : ctx.pdf.makeIndirectObject(alt));
      sep.appendItem(ctx.pdf.makeIndirectObject(fn));
      colDict.replaceKey(name, ctx.pdf.makeIndirectObject(sep));
      added = true;
      ++colorants;
    }
    (void)added;
  }
  if (malformedDn) {
    ctx.issue("DEVICEN_MALFORMED",
              "left " + std::to_string(malformedDn) +
                  " DeviceN space(s) without a tint transform untouched; the array is too "
                  "short to carry attributes",
              false);
  }
  if (renamed) {
    ctx.issue("COLORANT_RENAMED",
              "renamed " + std::to_string(renamed) + " colorant name(s) with invalid UTF-8",
              true);
  }
  if (deduped) {
    ctx.issue("SEPARATION_UNIFIED",
              "unified " + std::to_string(deduped) +
                  " Separation space(s) sharing a colorant name onto one alternate/tint",
              true);
  }
  if (colorants) {
    ctx.issue("COLORANTS_COMPLETED",
              "added " + std::to_string(colorants) +
                  " missing DeviceN /Colorants entr(y/ies)",
              true);
  }
}

void fixIccIdenticalToIntent(Ctx& ctx, QPDFObjectHandle keepIntent) {
  if (!keepIntent.isDictionary()) return;
  QPDFObjectHandle prof = keepIntent.getKey("/DestOutputProfile");
  if (!prof.isStream()) return;
  std::string oiBytes;
  try {
    auto buf = prof.getStreamData(qpdf_dl_all);
    oiBytes.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return;
  }
  bool oiCmyk = oiBytes.size() >= 20 && oiBytes.substr(16, 4) == "CMYK";
  bool anyIccCmyk = false;
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (obj.isStream() && obj.getDict().getKey("/N").isInteger() &&
        obj.getDict().getKey("/N").getIntValue() == 4 &&
        obj.getObjGen() != prof.getObjGen()) {
      anyIccCmyk = true;
      break;
    }
    if (obj.isArray() && obj.getArrayNItems() >= 2 &&
        nameIs(obj.getArrayItem(0), "/ICCBased") && obj.getArrayItem(1).isStream() &&
        obj.getArrayItem(1).getDict().getKey("/N").isInteger() &&
        obj.getArrayItem(1).getDict().getKey("/N").getIntValue() == 4) {
      anyIccCmyk = true;
      break;
    }
  }
  if (anyIccCmyk) {
    int opm = 0;
    int realOpm = ctx.opt.verifyOnly ? overprintViolations(ctx) : 0;
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (obj.isDictionary() && obj.getKey("/OPM").isInteger() &&
          obj.getKey("/OPM").getIntValue() == 1) {
        obj.replaceKey("/OPM", QPDFObjectHandle::newInteger(0));
        ++opm;
      }
    }
    if (ctx.opt.verifyOnly) opm = realOpm;
    if (opm) {
      ctx.issue("OVERPRINT_MODE_RESET",
                "reset " + std::to_string(opm) +
                    " overprint mode(s) to 0 (ICCBased CMYK colour spaces in use)",
                true);
    }
  }
  if (!oiCmyk) return;
  std::set<QPDFObjGen> identical;
  identical.insert(prof.getObjGen());
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (!obj.isStream() || obj.getObjGen() == prof.getObjGen()) continue;
    QPDFObjectHandle n = obj.getDict().getKey("/N");
    if (!n.isInteger() || n.getIntValue() != 4) continue;
    try {
      auto buf = obj.getStreamData(qpdf_dl_all);
      if (buf->getSize() == oiBytes.size() &&
          std::memcmp(buf->getBuffer(), oiBytes.data(), oiBytes.size()) == 0) {
        identical.insert(obj.getObjGen());
      }
    } catch (...) {
    }
  }
  if (identical.empty()) return;
  int replaced = 0;
  Visited visited;
  std::function<void(QPDFObjectHandle, int)> hunt = [&](QPDFObjectHandle o, int depth) {
    if (depth > kMaxObjectWalk) return;
    if (o.isIndirect() && !visited.enter(o)) return;
    if (o.isStream()) {
      hunt(o.getDict(), depth + 1);
      return;
    }
    if (o.isArray()) {
      if (o.getArrayNItems() >= 2 && nameIs(o.getArrayItem(0), "/ICCBased") &&
          o.getArrayItem(1).isIndirect() &&
          identical.count(o.getArrayItem(1).getObjGen())) {
        while (o.getArrayNItems() > 0) o.eraseItem(o.getArrayNItems() - 1);
        o.appendItem(QPDFObjectHandle::newName("/DeviceCMYK"));
        ++replaced;
        return;
      }
      for (int i = 0; i < o.getArrayNItems(); ++i) {
        QPDFObjectHandle v = o.getArrayItem(i);
        if (!v.isIndirect()) hunt(v, depth + 1);
      }
      return;
    }
    if (o.isDictionary()) {
      for (const std::string& k : o.getKeys()) {
        QPDFObjectHandle v = o.getKey(k);
        if (!v.isIndirect()) hunt(v, depth + 1);
      }
    }
  };
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) hunt(obj, 0);
  int opm = 0;
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (obj.isDictionary() && obj.getKey("/OPM").isNumber() &&
        obj.getKey("/OPM").getNumericValue() != 0) {
      obj.replaceKey("/OPM", QPDFObjectHandle::newInteger(0));
      ++opm;
    }
  }
  if ((replaced || opm) && !ctx.opt.verifyOnly) {
    ctx.issue("ICC_INTENT_DEDUPED",
              "replaced " + std::to_string(replaced) +
                  " ICCBased space(s) identical to the output intent with DeviceCMYK and "
                  "reset " +
                  std::to_string(opm) + " overprint mode(s)",
              true);
  }
}

std::string iccStreamBytes(QPDFObjectHandle s) {
  try {
    auto buf = s.getStreamData(qpdf_dl_all);
    return std::string(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return std::string();
  }
}

bool cmykIccStream(QPDFObjectHandle s) {
  return s.isStream() && s.getDict().getKey("/N").isInteger() &&
         s.getDict().getKey("/N").getIntValue() == 4;
}

void collectGroupProfiles(QPDFObjectHandle holder, Visited& seen,
                          std::set<std::string>& profiles, int depth = 0) {
  if (depth > kMaxResourceNest || !holder.isDictionary()) return;
  QPDFObjectHandle grp = holder.getKey("/Group");
  if (grp.isDictionary() && nameIs(grp.getKey("/S"), "/Transparency")) {
    QPDFObjectHandle cs = grp.getKey("/CS");
    if (cs.isArray() && cs.getArrayNItems() >= 2 && nameIs(cs.getArrayItem(0), "/ICCBased") &&
        cmykIccStream(cs.getArrayItem(1))) {
      std::string bytes = iccStreamBytes(cs.getArrayItem(1));
      if (!bytes.empty()) profiles.insert(bytes);
    }
  }
  QPDFObjectHandle res = holder.getKey("/Resources");
  QPDFObjectHandle xod = res.isDictionary() ? res.getKey("/XObject")
                                            : QPDFObjectHandle::newNull();
  if (!xod.isDictionary()) return;
  for (const std::string& k : xod.getKeys()) {
    QPDFObjectHandle xo = xod.getKey(k);
    if (xo.isStream() && nameIs(xo.getDict().getKey("/Subtype"), "/Form") &&
        seen.enter(xo)) {
      collectGroupProfiles(xo.getDict(), seen, profiles, depth + 1);
    }
  }
}

void replaceIccUses(QPDFObjectHandle o, const std::set<QPDFObjGen>& identical,
                    Visited& visited, int& replaced, int depth = 0) {
  if (depth > kMaxObjectWalk) return;
  if (o.isIndirect() && !visited.enter(o)) return;
  if (o.isStream()) {
    replaceIccUses(o.getDict(), identical, visited, replaced, depth + 1);
    return;
  }
  if (o.isArray()) {
    if (o.getArrayNItems() >= 2 && nameIs(o.getArrayItem(0), "/ICCBased") &&
        o.getArrayItem(1).isIndirect() && identical.count(o.getArrayItem(1).getObjGen())) {
      while (o.getArrayNItems() > 0) o.eraseItem(o.getArrayNItems() - 1);
      o.appendItem(QPDFObjectHandle::newName("/DeviceCMYK"));
      ++replaced;
      return;
    }
    for (int i = 0; i < o.getArrayNItems(); ++i) {
      replaceIccUses(o.getArrayItem(i), identical, visited, replaced, depth + 1);
    }
    return;
  }
  if (o.isDictionary()) {
    bool group = nameIs(o.getKey("/S"), "/Transparency");
    for (const std::string& k : o.getKeys()) {
      if (group && k == "/CS") continue;
      replaceIccUses(o.getKey(k), identical, visited, replaced, depth + 1);
    }
  }
}

void fixIccIdenticalToGroups(Ctx& ctx) {
  QPDFPageDocumentHelper dh(ctx.pdf);
  int replaced = 0;
  int kuraPage2 = 0;
  for (auto& ph : dh.getAllPages()) {
    PageScope kuraScope2(ctx, ++kuraPage2);
    QPDFObjectHandle page = ph.getObjectHandle();
    std::set<std::string> profiles;
    Visited gseen;
    collectGroupProfiles(page, gseen, profiles);
    if (profiles.empty()) continue;
    std::set<QPDFObjGen> identical;
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (!cmykIccStream(obj)) continue;
      std::string bytes = iccStreamBytes(obj);
      if (!bytes.empty() && profiles.count(bytes)) identical.insert(obj.getObjGen());
    }
    if (identical.empty()) continue;
    Visited visited;
    replaceIccUses(page.getKey("/Resources"), identical, visited, replaced);
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (annots.isArray()) {
      for (int i = 0; i < annots.getArrayNItems(); ++i) {
        replaceIccUses(annots.getArrayItem(i), identical, visited, replaced);
      }
    }
  }
  if (replaced) {
    ctx.issue("ICC_INTENT_DEDUPED",
              "replaced " + std::to_string(replaced) +
                  " ICCBased space(s) identical to a transparency group blending profile "
                  "with DeviceCMYK",
              true);
  }
}

bool injectDefaultSpace(Ctx&, std::vector<QPDFObjectHandle>& scopes, const char* key,
                        QPDFObjectHandle defRef) {
  bool changed = false;
  for (QPDFObjectHandle res : scopes) {
    QPDFObjectHandle csd = res.getKey("/ColorSpace");
    if (!csd.isDictionary()) {
      csd = QPDFObjectHandle::newDictionary();
      res.replaceKey("/ColorSpace", csd);
    }
    if (!csd.hasKey(key)) {
      csd.replaceKey(key, defRef);
      changed = true;
    }
  }
  return changed;
}

void passColorPrint(Ctx& ctx, ColorUsage& usage) {
  QPDFObjectHandle root = ctx.pdf.getRoot();
  std::string wantSubtype = ctx.isE() ? "/ISO_PDFE1" : "/GTS_PDFX";

  std::string anchor;
  QPDFObjectHandle keepIntent;
  QPDFObjectHandle existing = root.getKey("/OutputIntents");
  bool wantColorants = ctx.opt.level == Level::X5N || ctx.opt.level == Level::X6N;
  bool wantChecksum = ctx.opt.level == Level::X6N || ctx.opt.level == Level::X6P;
  auto wellFormedProfileRef = [&](QPDFObjectHandle oi, std::string& why) {
    QPDFObjectHandle ref = oi.getKey("/DestOutputProfileRef");
    if (!ref.isDictionary()) {
      why = "the output intent has no external press-profile reference";
      return false;
    }
    QPDFObjectHandle urls = ref.getKey("/URLs");
    bool hasUrl = urls.isArray() && urls.getArrayNItems() > 0;
    bool hasFile = ref.getKey("/F").isDictionary() || ref.getKey("/F").isString();
    if (!hasUrl && !hasFile) {
      why = "the press-profile reference names no address to fetch the profile from";
      return false;
    }
    QPDFObjectHandle ver = ref.getKey("/ICCVersion");
    if (!ver.isString() || ver.getStringValue().size() != 4) {
      why = "the press-profile reference must record the profile version as the four "
            "raw header bytes";
      return false;
    }
    QPDFObjectHandle ct = ref.getKey("/ColorantTable");
    if (wantColorants) {
      std::string cs = ref.getKey("/ProfileCS").isString()
                           ? ref.getKey("/ProfileCS").getUTF8Value()
                           : std::string();
      bool xclr = cs.size() == 4 && cs.substr(1) == "CLR";
      if (!xclr) {
        why = "a multi-colorant flavour needs the profile colour space recorded in the "
              "nCLR form";
        return false;
      }
      bool names = ct.isArray() && ct.getArrayNItems() > 0;
      for (int i = 0; names && i < ct.getArrayNItems(); ++i) {
        if (!ct.getArrayItem(i).isName()) names = false;
      }
      if (!names) {
        why = "a multi-colorant flavour needs the colorant names listed in the "
              "press-profile reference";
        return false;
      }
    } else if (!ct.isNull()) {
      why = "a colorant table is only allowed in the multi-colorant flavours";
      return false;
    }
    if (wantChecksum) {
      QPDFObjectHandle ck = ref.getKey("/CheckSum");
      if (!ck.isString() || ck.getStringValue().size() != 16) {
        why = "PDF 2.0 print flavours need a 16-byte checksum of the referenced profile";
        return false;
      }
    }
    return true;
  };
  if (existing.isArray()) {
    for (int i = 0; i < existing.getArrayNItems(); ++i) {
      QPDFObjectHandle oi = existing.getArrayItem(i);
      if (oi.isDictionary() && nameIs(oi.getKey("/S"), wantSubtype)) {
        std::string cs;
        bool embeddedOk = validExistingProfile(ctx, oi.getKey("/DestOutputProfile"), cs) &&
                          oi.getKey("/OutputConditionIdentifier").isString();
        if (ctx.externalIntent()) {
          std::string why;
          bool refOk = wellFormedProfileRef(oi, why) &&
                       oi.getKey("/OutputConditionIdentifier").isString();
          if (refOk) {
            anchor = "CMYK";
            keepIntent = oi;
            if (oi.getKey("/DestOutputProfile").isStream()) {
              ctx.issue("OUTPUT_INTENT_EMBEDDED_TOO",
                        "this flavour identifies its press condition by external "
                        "reference only, but the output intent also embeds the profile",
                        true);
            } else {
              ctx.issue("OUTPUT_INTENT_EXTERNAL_KEPT",
                        "kept output intent with an externally referenced press profile",
                        false);
            }
            break;
          }
          if (oi.getKey("/DestOutputProfileRef").isDictionary()) {
            ctx.issue("OUTPUT_INTENT_REF_INCOMPLETE", why, true);
            anchor = "CMYK";
            keepIntent = oi;
            break;
          }
          if (embeddedOk) {
            ctx.issue("OUTPUT_INTENT_NOT_EXTERNAL",
                      "this flavour identifies its press condition by external reference, "
                      "but the output intent only embeds the profile",
                      true);
            anchor = cs;
            keepIntent = oi;
            break;
          }
          continue;
        }
        if (ctx.opt.level == Level::VT2 && !embeddedOk) {
          std::string why;
          if (wellFormedProfileRef(oi, why) &&
              oi.getKey("/OutputConditionIdentifier").isString()) {
            anchor = "CMYK";
            keepIntent = oi;
            ctx.issue("OUTPUT_INTENT_EXTERNAL_KEPT",
                      "kept output intent with an externally referenced press profile",
                      false);
            break;
          }
        }
        if (embeddedOk) {
          if (ctx.isE() || cs == "CMYK" || (cs == "RGB " && !ctx.cmykIntentOnly())) {
            anchor = cs;
            keepIntent = oi;
            break;
          }
        }
      }
    }
  }
  if (ctx.opt.level == Level::X5N && !keepIntent.isInitialized() && !ctx.opt.verifyOnly &&
      !ctx.opt.externalProfileUrl.empty() && ctx.opt.destProfile.size() >= 132 &&
      ctx.opt.destProfile.substr(17, 3) == "CLR") {
    const std::string& p = ctx.opt.destProfile;
    QPDFObjectHandle ref = QPDFObjectHandle::newDictionary();
    QPDFObjectHandle url = QPDFObjectHandle::newDictionary();
    url.replaceKey("/FS", QPDFObjectHandle::newName("/URL"));
    url.replaceKey("/F", QPDFObjectHandle::newString(ctx.opt.externalProfileUrl));
    QPDFObjectHandle urls = QPDFObjectHandle::newArray();
    urls.appendItem(url);
    ref.replaceKey("/URLs", urls);
    ref.replaceKey("/ICCVersion", QPDFObjectHandle::newString(p.substr(8, 4)));
    ref.replaceKey("/ProfileCS", QPDFObjectHandle::newString(p.substr(16, 4)));
    QPDFObjectHandle table = QPDFObjectHandle::newArray();
    std::string desc;
    cmsHPROFILE h = cmsOpenProfileFromMem(p.data(), static_cast<cmsUInt32Number>(p.size()));
    int channels = std::atoi(p.substr(16, 1).c_str());
    if (std::isalpha(static_cast<unsigned char>(p[16]))) channels = std::toupper(static_cast<unsigned char>(p[16])) - 'A' + 10;
    if (h) {
      char buf[256] = {0};
      cmsGetProfileInfoASCII(h, cmsInfoDescription, "en", "US", buf, sizeof(buf));
      desc = buf;
      auto* list = static_cast<cmsNAMEDCOLORLIST*>(cmsReadTag(h, cmsSigColorantTableTag));
      for (cmsUInt32Number i = 0; list && i < cmsNamedColorCount(list); ++i) {
        char name[256] = {0};
        if (cmsNamedColorInfo(list, i, name, nullptr, nullptr, nullptr, nullptr) && name[0]) {
          table.appendItem(QPDFObjectHandle::newName("/" + std::string(name)));
        }
      }
      cmsCloseProfile(h);
    }
    if (table.getArrayNItems() == 0) {
      static const char* proc[4] = {"/Cyan", "/Magenta", "/Yellow", "/Black"};
      for (int i = 0; i < channels; ++i) {
        table.appendItem(QPDFObjectHandle::newName(i < 4 ? proc[i] : "/Colorant" + std::to_string(i + 1)));
      }
    }
    ref.replaceKey("/ColorantTable", table);
    if (!desc.empty()) ref.replaceKey("/ProfileName", QPDFObjectHandle::newUnicodeString(desc));
    QPDFObjectHandle oi = QPDFObjectHandle::newDictionary();
    oi.replaceKey("/Type", QPDFObjectHandle::newName("/OutputIntent"));
    oi.replaceKey("/S", QPDFObjectHandle::newName("/GTS_PDFX"));
    std::string ident = ctx.opt.outputConditionIdentifier.empty() ? (desc.empty() ? "Custom" : desc)
                                                                     : ctx.opt.outputConditionIdentifier;
    oi.replaceKey("/OutputConditionIdentifier", QPDFObjectHandle::newString(ident));
    oi.replaceKey("/Info", QPDFObjectHandle::newString(ctx.opt.outputConditionInfo.empty() ? ident : ctx.opt.outputConditionInfo));
    if (!ctx.opt.outputConditionRegistry.empty()) {
      oi.replaceKey("/RegistryName", QPDFObjectHandle::newString(ctx.opt.outputConditionRegistry));
    }
    oi.replaceKey("/DestOutputProfileRef", ref);
    keepIntent = ctx.pdf.makeIndirectObject(oi);
    anchor = "CMYK";
    ctx.issue("OUTPUT_INTENT_EXTERNAL_ADDED",
              "added an output intent that references the external " + std::to_string(table.getArrayNItems()) +
                  "-colorant press profile",
              true);
  }
  if (ctx.externalIntent() && !keepIntent.isInitialized()) {
    ctx.issue("OUTPUT_INTENT_EXTERNAL_MISSING",
              "no output intent with a usable external press-profile reference was found",
              true);
  }

  if (!keepIntent.isInitialized()) {
    bool wantCmyk = ctx.isX() || (usage.cmyk && !usage.rgb);
    const unsigned char* data = wantCmyk ? kCmykIcc : kSrgbIcc;
    unsigned int len = wantCmyk ? kCmykIccLen : kSrgbIccLen;
    if (ctx.opt.destProfile.size() >= 132) {
      const std::string& p = ctx.opt.destProfile;
      std::string cs = p.substr(16, 4);
      bool matches = (wantCmyk && cs == "CMYK") || (!wantCmyk && cs == "RGB ");
      if (ctx.isX() && cs == "CMYK") matches = true;
      if (matches) {
        data = reinterpret_cast<const unsigned char*>(p.data());
        len = static_cast<unsigned int>(p.size());
      } else {
        ctx.issue("DEST_PROFILE_IGNORED",
                  "supplied destination profile colour space (" + cs +
                      ") does not match the required intent; using built-in profile",
                  false);
      }
    }
    QPDFObjectHandle icc = buildIccStream(ctx, data, len, wantCmyk ? 4 : 3);
    QPDFObjectHandle oi = QPDFObjectHandle::newDictionary();
    oi.replaceKey("/Type", QPDFObjectHandle::newName("/OutputIntent"));
    oi.replaceKey("/S", QPDFObjectHandle::newName(wantSubtype));
    std::string ident = ctx.opt.outputConditionIdentifier.empty()
                            ? std::string("Custom")
                            : ctx.opt.outputConditionIdentifier;
    std::string info = ctx.opt.outputConditionInfo.empty()
                           ? (wantCmyk ? "Coated GRACoL 2006"
                                       : "sRGB IEC61966-2.1")
                           : ctx.opt.outputConditionInfo;
    oi.replaceKey("/OutputConditionIdentifier", QPDFObjectHandle::newString(ident));
    oi.replaceKey("/OutputCondition", QPDFObjectHandle::newString(info));
    oi.replaceKey("/Info", QPDFObjectHandle::newString(info));
    if (!ctx.opt.outputConditionRegistry.empty()) {
      oi.replaceKey("/RegistryName",
                    QPDFObjectHandle::newString(ctx.opt.outputConditionRegistry));
    }
    oi.replaceKey("/DestOutputProfile", icc);
    keepIntent = ctx.pdf.makeIndirectObject(oi);
    anchor = wantCmyk ? "CMYK" : "RGB ";
    ctx.issue("OUTPUT_INTENT_ADDED",
              std::string("added ") + (ctx.isE() ? "PDF/E" : "PDF/X") + " output intent (" +
                  (wantCmyk ? "CMYK" : "sRGB") + ", condition \"" + ident + "\")",
              true);
  } else {
    ctx.issue("OUTPUT_INTENT_PRESENT", "kept existing print output intent", false);
  }

  QPDFObjectHandle arr = QPDFObjectHandle::newArray();
  arr.appendItem(keepIntent);
  int kept = 1;
  QPDFObjectHandle keepProfile = keepIntent.getKey("/DestOutputProfile");
  for (int i = 0; existing.isArray() && i < existing.getArrayNItems(); ++i) {
    QPDFObjectHandle oi = existing.getArrayItem(i);
    if (!oi.isDictionary() || nameIs(oi.getKey("/S"), wantSubtype)) continue;
    QPDFObjectHandle prof = oi.getKey("/DestOutputProfile");
    if (prof.isIndirect() && keepProfile.isIndirect() && prof.getObjGen() == keepProfile.getObjGen()) {
      arr.appendItem(oi);
      ++kept;
    }
  }
  bool dropped = existing.isArray() && existing.getArrayNItems() > kept;
  root.replaceKey("/OutputIntents", arr);
  if (dropped) {
    ctx.issue("OUTPUT_INTENTS_PRUNED", "reduced output intents to the single print intent",
              true);
  }
  {
    QPDFPageDocumentHelper dh(ctx.pdf);
    std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
    const std::string& sel = ctx.opt.pageOutputIntents;
    int count = static_cast<int>(pages.size()), placed = 0;
    int kuraPage3 = 0;
    for (auto& ph : pages) {
      PageScope kuraScope3(ctx, ++kuraPage3);
      QPDFObjectHandle page = ph.getObjectHandle();
      if (page.hasKey("/OutputIntents")) page.removeKey("/OutputIntents");
      if (sel.empty() || !ctx.pdf20Print()) continue;
      bool want = pageWanted(sel, kuraPage3, count);
      if (!want) continue;
      QPDFObjectHandle pa = QPDFObjectHandle::newArray();
      pa.appendItem(keepIntent);
      page.replaceKey("/OutputIntents", pa);
      ++placed;
    }
    if (placed) {
      ctx.issue("PAGE_OUTPUT_INTENTS_SET",
                "added the output intent at page level on " + std::to_string(placed) + " page(s)",
                true);
    }
  }

  if (ctx.x1a()) {
    convertColorsX1a(ctx);
    return;
  }

  if (!usage.rgbScopes.empty() && anchor != "RGB ") {
    QPDFObjectHandle srgb = buildIccStream(ctx, kSrgbIcc, kSrgbIccLen, 3);
    QPDFObjectHandle defRgb = QPDFObjectHandle::newArray();
    defRgb.appendItem(QPDFObjectHandle::newName("/ICCBased"));
    defRgb.appendItem(srgb);
    if (injectDefaultSpace(ctx, usage.rgbScopes, "/DefaultRGB",
                           ctx.pdf.makeIndirectObject(defRgb))) {
      ctx.issue("DEFAULT_RGB_INJECTED",
                "mapped DeviceRGB usage to sRGB via /DefaultRGB (colour-managed print)", true);
    }
  }
  if (ctx.isX() || ctx.isVT()) {
    fixIccIdenticalToIntent(ctx, keepIntent);
  }
  if (ctx.isE() && anchor != "CMYK" && !usage.cmykScopes.empty()) {
    QPDFObjectHandle cmyk = buildIccStream(ctx, kCmykIcc, kCmykIccLen, 4);
    QPDFObjectHandle defCmyk = QPDFObjectHandle::newArray();
    defCmyk.appendItem(QPDFObjectHandle::newName("/ICCBased"));
    defCmyk.appendItem(cmyk);
    if (injectDefaultSpace(ctx, usage.cmykScopes, "/DefaultCMYK",
                           ctx.pdf.makeIndirectObject(defCmyk))) {
      ctx.issue("DEFAULT_CMYK_INJECTED",
                "mapped DeviceCMYK usage to a calibrated profile via /DefaultCMYK", true);
    }
  }
}
}

void passColor(Ctx& ctx) {
  if (ctx.isA() && ctx.part <= 3) {
    int reduced = 0;
    std::vector<QPDFObjectHandle> wide;
    Visited hunt;
    std::function<void(QPDFObjectHandle, int)> collect = [&](QPDFObjectHandle o, int depth) {
      if (depth > kMaxObjectWalk) return;
      if (o.isIndirect() && !hunt.enter(o)) return;
      if (o.isStream()) {
        collect(o.getDict(), depth + 1);
        return;
      }
      if (o.isArray()) {
        if (o.getArrayNItems() >= 3 && nameIs(o.getArrayItem(0), "/DeviceN")) {
          QPDFObjectHandle names = o.getArrayItem(1);
          if (names.isArray() && names.getArrayNItems() > 8) {
            wide.push_back(o);
            return;
          }
        }
        for (int i = 0; i < o.getArrayNItems(); ++i) {
          QPDFObjectHandle v = o.getArrayItem(i);
          if (!v.isIndirect()) collect(v, depth + 1);
        }
        return;
      }
      if (o.isDictionary()) {
        for (const std::string& k : o.getKeys()) {
          QPDFObjectHandle v = o.getKey(k);
          if (!v.isIndirect()) collect(v, depth + 1);
        }
      }
    };
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) collect(obj, 0);
    QPDFObjectHandle icc = wide.empty() ? QPDFObjectHandle() : buildIccStream(ctx, kCmykIcc, kCmykIccLen, 4);
    if (!wide.empty()) {
      QPDFObjectHandle iccSpace = QPDFObjectHandle::newArray();
      iccSpace.appendItem(QPDFObjectHandle::newName("/ICCBased"));
      iccSpace.appendItem(icc);
      remapContentColors(ctx, [&](const ColorValue& in, unsigned, ColorValue& out) {
        if (in.family != "/DeviceN" || in.colorants.size() <= 8) return false;
        out.family = "/ICCBased";
        out.space = iccSpace;
        return colorValueToProcess(in, 4, out.values);
      });
    }
    for (QPDFObjectHandle obj : wide) {
      while (obj.getArrayNItems() > 0) obj.eraseItem(obj.getArrayNItems() - 1);
      obj.appendItem(QPDFObjectHandle::newName("/ICCBased"));
      obj.appendItem(icc);
      ++reduced;
    }
    if (reduced) {
      ctx.issue("DEVICEN_REDUCED",
                "replaced " + std::to_string(reduced) +
                    " DeviceN space(s) exceeding the limit of 8 colorants with a "
                    "calibrated CMYK space (visual difference possible)",
                true);
    }
  }
  if (ctx.isA() && ctx.part >= 2) fixSpecialColorSpaces(ctx);
  ColorUsage usage = scanUsage(ctx);
  if (ctx.isA() && ctx.part == 1 && usage.rgb && usage.cmyk) {
    convertColorsX1a(ctx);
    if (ctx.failed()) return;
    ctx.issue("DEVICE_COLOR_UNIFIED",
              "document mixed DeviceRGB and DeviceCMYK; converted RGB content to CMYK so a "
              "single PDF/A-1 output intent can anchor it (visual difference possible)",
              true);
    usage = scanUsage(ctx);
  }
  if (!ctx.isA()) {
    passColorPrint(ctx, usage);
    return;
  }
  QPDFObjectHandle root = ctx.pdf.getRoot();

  std::string anchor;
  QPDFObjectHandle keepIntent;
  QPDFObjectHandle existing = root.getKey("/OutputIntents");
  if (existing.isArray()) {
    for (int i = 0; i < existing.getArrayNItems(); ++i) {
      QPDFObjectHandle oi = existing.getArrayItem(i);
      if (oi.isDictionary() && (nameIs(oi.getKey("/S"), "/GTS_PDFA1") ||
                                (ctx.dualX() && nameIs(oi.getKey("/S"), "/GTS_PDFX")))) {
        std::string cs;
        if (validExistingProfile(ctx, oi.getKey("/DestOutputProfile"), cs)) {
          if (ctx.part == 1) {
            bool covers = (cs == "RGB " && !usage.cmyk) || (cs == "CMYK" && !usage.rgb) ||
                          (cs == "GRAY" && !usage.rgb && !usage.cmyk);
            if (!covers) continue;
          }
          anchor = cs;
          keepIntent = oi;
          break;
        }
      }
    }
  }
  if (existing.isArray() && existing.getArrayNItems() > 0 && !keepIntent.isInitialized()) {
    ctx.issue("OUTPUT_INTENT_REPLACED",
              "existing output intent does not cover the document's device colour usage; "
              "replacing it",
              true);
  }

  if (!keepIntent.isInitialized() || anchor.empty()) {
    bool wantCmyk = usage.cmyk && !usage.rgb;
    const unsigned char* data = wantCmyk ? kCmykIcc : kSrgbIcc;
    unsigned int len = wantCmyk ? kCmykIccLen : kSrgbIccLen;
    QPDFObjectHandle icc = buildIccStream(ctx, data, len, wantCmyk ? 4 : 3);
    QPDFObjectHandle oi = QPDFObjectHandle::newDictionary();
    oi.replaceKey("/Type", QPDFObjectHandle::newName("/OutputIntent"));
    oi.replaceKey("/S", QPDFObjectHandle::newName("/GTS_PDFA1"));
    std::string ident = wantCmyk ? "Coated GRACoL 2006" : "sRGB IEC61966-2.1";
    oi.replaceKey("/OutputConditionIdentifier", QPDFObjectHandle::newString(ident));
    oi.replaceKey("/RegistryName", QPDFObjectHandle::newString("http://www.color.org"));
    oi.replaceKey("/Info", QPDFObjectHandle::newString(ident));
    oi.replaceKey("/DestOutputProfile", icc);
    keepIntent = ctx.pdf.makeIndirectObject(oi);
    anchor = wantCmyk ? "CMYK" : "RGB ";
    bool needed = !ctx.opt.verifyOnly || usage.rgb || usage.cmyk || usage.gray ||
                  (existing.isArray() && existing.getArrayNItems() > 0) ||
                  (ctx.part >= 2 && pagesNeedingBlendSpace(ctx) > 0);
    ctx.issue("OUTPUT_INTENT_ADDED",
              std::string("added ") + (wantCmyk ? "CMYK" : "sRGB") + " PDF/A output intent", needed);
  } else {
    ctx.issue("OUTPUT_INTENT_PRESENT", "kept existing PDF/A output intent", false);
  }

  QPDFObjectHandle arr = QPDFObjectHandle::newArray();
  if (ctx.dualX()) {
    QPDFObjectHandle a = keepIntent.shallowCopy();
    a.replaceKey("/S", QPDFObjectHandle::newName("/GTS_PDFA1"));
    QPDFObjectHandle x = keepIntent.shallowCopy();
    x.replaceKey("/S", QPDFObjectHandle::newName("/GTS_PDFX"));
    keepIntent = ctx.pdf.makeIndirectObject(a);
    arr.appendItem(keepIntent);
    arr.appendItem(ctx.pdf.makeIndirectObject(x));
  } else {
    arr.appendItem(keepIntent);
  }
  bool dropped = existing.isArray() && existing.getArrayNItems() > (ctx.dualX() ? 2 : 1);
  if (dropped && ctx.opt.verifyOnly) {
    dropped = false;
    QPDFObjectHandle first;
    for (int i = 0; i < existing.getArrayNItems(); ++i) {
      QPDFObjectHandle p = existing.getArrayItem(i).getKey("/DestOutputProfile");
      if (p.isNull()) continue;
      if (!first.isInitialized()) first = p;
      else if (!(p.isIndirect() && first.isIndirect() && p.getObjGen() == first.getObjGen())) dropped = true;
    }
  }
  root.replaceKey("/OutputIntents", arr);
  if (dropped) {
    ctx.issue("OUTPUT_INTENTS_PRUNED",
              "removed additional output intents (PDF/A requires a single shared profile)", true);
  }

  if (ctx.part >= 4) {
    if (keepIntent.isDictionary() && keepIntent.hasKey("/DestOutputProfileRef")) {
      keepIntent.removeKey("/DestOutputProfileRef");
      ctx.issue("OUTPUT_INTENT_PROFILE_REF_REMOVED",
                "removed external /DestOutputProfileRef from output intent", true);
    }
    QPDFPageDocumentHelper dh(ctx.pdf);
    int pageIntents = 0;
    int kuraPage4 = 0;
    for (auto& ph : dh.getAllPages()) {
      PageScope kuraScope4(ctx, ++kuraPage4);
      QPDFObjectHandle page = ph.getObjectHandle();
      if (page.hasKey("/OutputIntents")) {
        if (!ctx.opt.verifyOnly) page.removeKey("/OutputIntents");
        ++pageIntents;
      }
    }
    if (pageIntents) {
      ctx.issue("PAGE_OUTPUT_INTENTS_REMOVED",
                "removed page-level output intents from " + std::to_string(pageIntents) +
                    " page(s) in favor of the document output intent",
                !ctx.opt.verifyOnly);
    }
  }

  if (ctx.part >= 4 && ctx.opt.verifyOnly) {
    int same = iccIdenticalViolations(ctx, keepIntent);
    int op = overprintViolations(ctx);
    if (op) {
      ctx.issue("OVERPRINT_MODE_RESET", std::to_string(op) + " overprinted ICCBased CMYK paint operation(s) with OPM 1", true);
    }
    if (same) {
      ctx.issue("ICC_INTENT_DEDUPED",
                std::to_string(same) + " ICCBased CMYK use(s) share the output intent or blending profile", true);
    }
  } else if (ctx.part >= 2) {
    fixIccIdenticalToIntent(ctx, keepIntent);
    fixIccIdenticalToGroups(ctx);
  }

  if (ctx.part == 1 && anchor != "RGB ") {
    QPDFPageDocumentHelper dh(ctx.pdf);
    int stripped = 0;
    int kuraPage5 = 0;
    for (auto& ph : dh.getAllPages()) {
      PageScope kuraScope5(ctx, ++kuraPage5);
      QPDFObjectHandle annots = ph.getObjectHandle().getKey("/Annots");
      if (!annots.isArray()) continue;
      for (int i = 0; i < annots.getArrayNItems(); ++i) {
        QPDFObjectHandle a = annots.getArrayItem(i);
        if (!a.isDictionary()) continue;
        for (const char* key : {"/C", "/IC"}) {
          if (a.hasKey(key)) {
            a.removeKey(key);
            ++stripped;
          }
        }
      }
    }
    if (stripped) {
      ctx.issue("ANNOT_COLOR_REMOVED",
                "removed " + std::to_string(stripped) +
                    " annotation colour array(s) (PDF/A-1 requires an RGB output intent "
                    "for annotation /C and /IC)",
                true);
    }
  }

  QPDFObjectHandle cmykIccShared;
  if (anchor != "CMYK" && (!usage.cmykScopes.empty() || !usage.cmykAlternates.empty())) {
    cmykIccShared = buildIccStream(ctx, kCmykIcc, kCmykIccLen, 4);
  }

  if (ctx.part >= 2) {
    if (anchor != "CMYK" && !usage.cmykScopes.empty()) {
      QPDFObjectHandle defCmyk = QPDFObjectHandle::newArray();
      defCmyk.appendItem(QPDFObjectHandle::newName("/ICCBased"));
      defCmyk.appendItem(cmykIccShared);
      if (injectDefaultSpace(ctx, usage.cmykScopes, "/DefaultCMYK",
                             ctx.pdf.makeIndirectObject(defCmyk))) {
        ctx.issue("DEFAULT_CMYK_INJECTED",
                  "mapped DeviceCMYK usage to a calibrated CMYK profile via /DefaultCMYK", true);
      }
    }
    if (anchor != "RGB " && !usage.rgbScopes.empty()) {
      QPDFObjectHandle srgb = buildIccStream(ctx, kSrgbIcc, kSrgbIccLen, 3);
      QPDFObjectHandle defRgb = QPDFObjectHandle::newArray();
      defRgb.appendItem(QPDFObjectHandle::newName("/ICCBased"));
      defRgb.appendItem(srgb);
      QPDFObjectHandle defRgbObj = ctx.pdf.makeIndirectObject(defRgb);
      if (injectDefaultSpace(ctx, usage.rgbScopes, "/DefaultRGB", defRgbObj)) {
        ctx.issue("DEFAULT_RGB_INJECTED",
                  "mapped DeviceRGB usage to sRGB via /DefaultRGB", true);
      }
      int tagged = 0;
      for (QPDFObjectHandle o : ctx.pdf.getAllObjects()) {
        QPDFObjectHandle d = o.isStream() ? o.getDict() : o;
        if (d.isDictionary() && d.getKey("/Shading").isDictionary() && !d.getKey("/Shading").isIndirect()) {
          d = d.getKey("/Shading");
        }
        if (!d.isDictionary() || !d.getKey("/ShadingType").isInteger()) continue;
        QPDFObjectHandle cs = d.getKey("/ColorSpace");
        if (nameIs(cs, "/DeviceRGB") || nameIs(cs, "/RGB")) {
          d.replaceKey("/ColorSpace", defRgbObj);
          ++tagged;
        }
      }
      if (tagged) {
        ctx.issue("SHADING_RGB_TAGGED",
                  "tagged " + std::to_string(tagged) + " DeviceRGB shading(s) with sRGB", true);
      }
    }
  } else {
    if (anchor != "CMYK" && usage.cmyk) {
      ctx.fatal("CMYK_MIXED_P1",
                "document mixes DeviceCMYK with RGB content, which PDF/A-1 cannot anchor with a "
                "single output intent; target PDF/A-2 or 3");
      return;
    }
    if (anchor == "CMYK" && usage.rgb) {
      ctx.fatal("RGB_UNDER_CMYK_P1",
                "document uses DeviceRGB but its output intent is CMYK; target PDF/A-2 or 3");
      return;
    }
  }

  if (ctx.part >= 2 && (usage.cmyk || !usage.cmykScopes.empty())) {
    int opmLate = 0;
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (obj.isDictionary() && obj.getKey("/OPM").isNumber() &&
          obj.getKey("/OPM").getNumericValue() != 0) {
        obj.replaceKey("/OPM", QPDFObjectHandle::newInteger(0));
        ++opmLate;
      }
    }
    if (opmLate) {
      ctx.issue("OVERPRINT_MODE_RESET",
                "reset " + std::to_string(opmLate) +
                    " overprint mode(s) to 0 (DeviceCMYK is colour-managed via DefaultCMYK)",
                true);
    }
  }

  if (anchor != "CMYK" && !usage.processCmyk.empty()) {
    if (!cmykIccShared.isInitialized()) {
      cmykIccShared = buildIccStream(ctx, kCmykIcc, kCmykIccLen, 4);
    }
    QPDFObjectHandle iccArrP = QPDFObjectHandle::newArray();
    iccArrP.appendItem(QPDFObjectHandle::newName("/ICCBased"));
    iccArrP.appendItem(cmykIccShared);
    QPDFObjectHandle iccRefP = ctx.pdf.makeIndirectObject(iccArrP);
    std::set<QPDFObjGen> doneP;
    int fixedP = 0;
    for (QPDFObjectHandle proc : usage.processCmyk) {
      if (proc.isIndirect() && !doneP.insert(proc.getObjGen()).second) continue;
      proc.replaceKey("/ColorSpace", iccRefP);
      ++fixedP;
    }
    if (fixedP) {
      ctx.issue("SEPARATION_ALTERNATE_FIXED",
                "replaced DeviceCMYK with calibrated CMYK in " + std::to_string(fixedP) +
                    " NChannel /Process dictionar(ies)",
                true);
    }
  }
  if (anchor != "CMYK" && !usage.cmykAlternates.empty()) {
    QPDFObjectHandle iccArr = QPDFObjectHandle::newArray();
    iccArr.appendItem(QPDFObjectHandle::newName("/ICCBased"));
    iccArr.appendItem(cmykIccShared);
    QPDFObjectHandle iccRef = ctx.pdf.makeIndirectObject(iccArr);
    std::set<QPDFObjGen> done;
    int fixed = 0;
    for (QPDFObjectHandle cs : usage.cmykAlternates) {
      if (cs.isIndirect() && !done.insert(cs.getObjGen()).second) continue;
      cs.setArrayItem(2, iccRef);
      ++fixed;
    }
    if (fixed) {
      ctx.issue("SEPARATION_ALTERNATE_FIXED",
                "replaced DeviceCMYK alternate space with calibrated CMYK in " +
                    std::to_string(fixed) + " Separation/DeviceN spaces",
                true);
    }
  }
}
}
