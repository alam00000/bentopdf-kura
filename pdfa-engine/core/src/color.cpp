#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "assets/cmyk_icc.hh"
#include "assets/srgb_icc.hh"
#include "colorx.hh"
#include "ctx.hh"
#include "passes.hh"
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

void classifyByName(Ctx& ctx, const std::string& name, QPDFObjectHandle res, ColorUsage& usage,
                    int depth) {
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
  if (depth > 8) return;
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
      usage.cmykAlternates.push_back(cs);
    } else {
      classifySpace(ctx, alt, res, usage, depth + 1);
    }
    return;
  }
  if (family == "/DeviceN" && cs.getArrayNItems() >= 3) {
    QPDFObjectHandle alt = cs.getArrayItem(2);
    if (nameIs(alt, "/DeviceCMYK")) {
      usage.cmykAlternates.push_back(cs);
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
  OpScanner(Ctx& ctx, QPDFObjectHandle res, ColorUsage& usage)
      : ctx(ctx), res(res), usage(usage) {}

  void handleObject(QPDFObjectHandle obj, size_t, size_t) override {
    if (obj.isOperator()) {
      std::string op = obj.getOperatorValue();
      if (op == "rg" || op == "RG") {
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
  std::vector<QPDFObjectHandle> operands;
  bool inImage = false;
  bool pendingCs = false;
};

void scanContent(Ctx& ctx, QPDFObjectHandle contentHolder, QPDFObjectHandle res,
                 ColorUsage& usage) {
  try {
    OpScanner scanner(ctx, res, usage);
    if (contentHolder.isStream()) {
      QPDFObjectHandle::parseContentStream(contentHolder, &scanner);
    } else if (contentHolder.isArray() || contentHolder.isDictionary()) {
      QPDFObjectHandle::parseContentStream(contentHolder, &scanner);
    }
  } catch (...) {
  }
}

void scanResources(Ctx& ctx, QPDFObjectHandle res, Visited& visited, ColorUsage& usage,
                   int depth = 0) {
  if (depth > 96) return;
  if (!res.isDictionary() || !visited.enter(res)) return;
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (xod.isDictionary()) {
    for (const std::string& k : xod.getKeys()) {
      QPDFObjectHandle xo = xod.getKey(k);
      if (!xo.isStream() || !visited.enter(xo)) continue;
      QPDFObjectHandle d = xo.getDict();
      std::string subtype = nameOf(d.getKey("/Subtype"));
      if (subtype == "/Image") {
        classifySpace(ctx, d.getKey("/ColorSpace"), res, usage, 0);
      } else if (subtype == "/Form") {
        QPDFObjectHandle inner =
            d.getKey("/Resources").isDictionary() ? d.getKey("/Resources") : res;
        QPDFObjectHandle group = d.getKey("/Group");
        if (group.isDictionary()) classifySpace(ctx, group.getKey("/CS"), inner, usage, 0);
        scanContent(ctx, xo, inner, usage);
        scanResources(ctx, d.getKey("/Resources"), visited, usage, depth + 1);
      }
    }
  }
  QPDFObjectHandle shd = res.getKey("/Shading");
  if (shd.isDictionary()) {
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
  if (csd.isDictionary()) {
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

ColorUsage scanUsage(Ctx& ctx) {
  ColorUsage usage;
  Visited visited;
  QPDFPageDocumentHelper dh(ctx.pdf);
  for (auto& ph : dh.getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle res = ph.getAttribute("/Resources", false);
    QPDFObjectHandle group = page.getKey("/Group");
    if (group.isDictionary()) classifySpace(ctx, group.getKey("/CS"), res, usage, 0);
    scanContent(ctx, page.getKey("/Contents"), res, usage);
    scanResources(ctx, res, visited, usage);
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (annots.isArray()) {
      for (int i = 0; i < annots.getArrayNItems(); ++i) {
        QPDFObjectHandle a = annots.getArrayItem(i);
        if (!a.isDictionary()) continue;
        QPDFObjectHandle ap = a.getKey("/AP");
        if (!ap.isDictionary()) continue;
        QPDFObjectHandle n = ap.getKey("/N");
        std::vector<QPDFObjectHandle> streams;
        if (n.isStream()) streams.push_back(n);
        else if (n.isDictionary()) {
          for (const std::string& k : n.getKeys()) {
            if (n.getKey(k).isStream()) streams.push_back(n.getKey(k));
          }
        }
        for (QPDFObjectHandle s : streams) {
          if (!visited.enter(s)) continue;
          QPDFObjectHandle inner = s.getDict().getKey("/Resources");
          scanContent(ctx, s, inner.isDictionary() ? inner : res, usage);
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

bool validUtf8Bare(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = s[i];
    int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (extra < 0) return false;
    for (int k = 1; k <= extra; ++k) {
      if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
    }
    i += extra + 1;
  }
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
  if (depth > 24) return;
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
    unsigned hash = 2166136261u;
    for (unsigned char c : bare) hash = (hash ^ c) * 16777619u;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "/C%08X", hash);
    return std::string(buf);
  };
  auto fixName = [&](QPDFObjectHandle arr, int idx) {
    QPDFObjectHandle nm = arr.getArrayItem(idx);
    if (!nm.isName()) return;
    std::string bare = nm.getName().substr(1);
    if (validUtf8Bare(bare)) return;
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
      bool sameAlt = sameColorRef(alt, it->second.first);
      bool sameTint = sameColorRef(tint, it->second.second);
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
        if (nm.isName() && !validUtf8Bare(nm.getName().substr(1))) {
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
      bool sameAlt = sameColorRef(alt2, known->second.first);
      bool sameTint = sameColorRef(tint2, known->second.second);
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
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (obj.isDictionary() && obj.getKey("/OPM").isInteger() &&
          obj.getKey("/OPM").getIntValue() == 1) {
        obj.replaceKey("/OPM", QPDFObjectHandle::newInteger(0));
        ++opm;
      }
    }
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
    if (depth > 64) return;
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
  if (replaced || opm) {
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
  if (depth > 24 || !holder.isDictionary()) return;
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
  if (depth > 64) return;
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
  for (auto& ph : dh.getAllPages()) {
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
                           ? (wantCmyk ? "In-house CMYK output condition"
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
  bool dropped = existing.isArray() && existing.getArrayNItems() > 1;
  root.replaceKey("/OutputIntents", arr);
  if (dropped) {
    ctx.issue("OUTPUT_INTENTS_PRUNED", "reduced output intents to the single print intent",
              true);
  }
  {
    QPDFPageDocumentHelper dh(ctx.pdf);
    for (auto& ph : dh.getAllPages()) {
      QPDFObjectHandle page = ph.getObjectHandle();
      if (page.hasKey("/OutputIntents")) page.removeKey("/OutputIntents");
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
      if (depth > 64) return;
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
    for (QPDFObjectHandle obj : wide) {
      QPDFObjectHandle icc = buildIccStream(ctx, kCmykIcc, kCmykIccLen, 4);
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
      if (oi.isDictionary() && nameIs(oi.getKey("/S"), "/GTS_PDFA1")) {
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
    std::string ident = wantCmyk ? "Naive CMYK (composite over sRGB)" : "sRGB IEC61966-2.1";
    oi.replaceKey("/OutputConditionIdentifier", QPDFObjectHandle::newString(ident));
    oi.replaceKey("/RegistryName", QPDFObjectHandle::newString("http://www.color.org"));
    oi.replaceKey("/Info", QPDFObjectHandle::newString(ident));
    oi.replaceKey("/DestOutputProfile", icc);
    keepIntent = ctx.pdf.makeIndirectObject(oi);
    anchor = wantCmyk ? "CMYK" : "RGB ";
    ctx.issue("OUTPUT_INTENT_ADDED",
              std::string("added ") + (wantCmyk ? "CMYK" : "sRGB") + " PDF/A output intent", true);
  } else {
    ctx.issue("OUTPUT_INTENT_PRESENT", "kept existing PDF/A output intent", false);
  }

  QPDFObjectHandle arr = QPDFObjectHandle::newArray();
  arr.appendItem(keepIntent);
  bool dropped = existing.isArray() && existing.getArrayNItems() > 1;
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
    for (auto& ph : dh.getAllPages()) {
      QPDFObjectHandle page = ph.getObjectHandle();
      if (page.hasKey("/OutputIntents")) {
        page.removeKey("/OutputIntents");
        ++pageIntents;
      }
    }
    if (pageIntents) {
      ctx.issue("PAGE_OUTPUT_INTENTS_REMOVED",
                "removed page-level output intents from " + std::to_string(pageIntents) +
                    " page(s) in favor of the document output intent",
                true);
    }
  }

  if (ctx.part >= 2) {
    fixIccIdenticalToIntent(ctx, keepIntent);
    fixIccIdenticalToGroups(ctx);
  }

  if (ctx.part == 1 && anchor != "RGB ") {
    QPDFPageDocumentHelper dh(ctx.pdf);
    int stripped = 0;
    for (auto& ph : dh.getAllPages()) {
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
      if (injectDefaultSpace(ctx, usage.rgbScopes, "/DefaultRGB",
                             ctx.pdf.makeIndirectObject(defRgb))) {
        ctx.issue("DEFAULT_RGB_INJECTED",
                  "mapped DeviceRGB usage to sRGB via /DefaultRGB", true);
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
