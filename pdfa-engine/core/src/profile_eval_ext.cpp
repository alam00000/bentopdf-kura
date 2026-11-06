#include <qpdf/QPDF.hh>
#include <qpdf/QPDFCryptoProvider.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <sys/stat.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "fonts_ft.hh"
#include FT_OUTLINE_H
#include FT_CID_H
#include FT_TRUETYPE_TABLES_H

#include "einvoice.hh"
#include "pdffunc.hh"
#include "pdfa/pdfa.hh"
#include "profile_ext.hh"
#include "profile_vars.hh"
#include "uacheck.hh"
#include "util.hh"

namespace pdfa {
const std::map<uint32_t, std::u32string>& toUnicodeMap(const FontFacts& f, const Events& ev);

namespace {
std::string bare(QPDFObjectHandle o) {
  if (o.isName()) return o.getName().substr(1);
  if (o.isString()) return o.getUTF8Value();
  return std::string();
}

std::string bytesOf(QPDFObjectHandle s) {
  if (!s.isStream()) return std::string();
  try {
    auto b = s.getStreamData(qpdf_dl_all);
    return std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
  } catch (...) {
    return std::string();
  }
}

bool isDeviceName(const std::string& n) {
  return n == "/DeviceGray" || n == "/DeviceRGB" || n == "/DeviceCMYK" || n == "/G" || n == "/RGB" || n == "/CMYK" ||
         n == "/Pattern";
}

using DPart = DocumentPart;

struct FontText {
  FtFace face;
  bool ok = false;
  std::map<FT_UInt, std::set<uint32_t>> gidUni;
};

struct ExtFacts {
  bool walked = false;
  FtLib ftLib;
  std::map<QPDFObjGen, std::shared_ptr<FontText>> fontText;
  std::map<QPDFObjGen, int> pageIndex;
  std::map<QPDFObjGen, int> doCount;
  std::map<QPDFObjGen, std::set<int>> xobjPages;
  std::set<QPDFObjGen> formsWithMcid;
  std::set<QPDFObjGen> formsSeen;
  std::set<QPDFObjGen> formsDependent;
  std::map<std::string, std::set<std::string>> gsUsed, csUsed, xoUsed;
  std::map<std::string, QPDFObjectHandle> resDicts;
  bool refUnusedBM = false;
  bool refUnusedGray = false, refUnusedRGB = false, refUnusedCMYK = false;
  std::vector<DPart> dparts;
  int recordLevel = -1;
  std::map<int, int> pageRecord;
  std::vector<MarkedState> mcStates;
  std::map<std::vector<size_t>, std::vector<std::pair<size_t, int>>> mcIndex;
  std::map<int, std::map<long long, int>> mcidCount;
  std::map<int, std::set<long long>> mcidText;
  std::set<int> untaggedText;
  std::vector<std::pair<QPDFObjectHandle, QPDFObjGen>> formDicts;
  std::shared_ptr<void> tagging;
};
struct Mark {
  QPDFObjectHandle oc;
  bool artifact = false;
  std::string type, subtype;
  long long mcid = -1;
  bool lang = false;
};

std::vector<std::string> ocgNames(QPDFObjectHandle cond) {
  std::vector<std::string> out;
  for (QPDFObjectHandle g : ocgsOf(cond)) {
    std::string n = bare(g.getKey("/Name"));
    if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
  }
  return out;
}

struct Walker : QPDFObjectHandle::ParserCallbacks {
  ExtFacts& f;
  int page;
  QPDFObjectHandle res;
  std::string resKey;
  QPDFObjGen form;
  bool inForm;
  int depth;
  std::set<QPDFObjGen>& visiting;
  std::vector<QPDFObjectHandle> args;
  std::vector<Mark> marks;
  std::vector<size_t> key;
  size_t offset = 0;
  struct St {
    bool fill = false, stroke = false, lw = false, font = false;
    int tr = 0;
  };
  St st;
  std::vector<St> sstack;

  Walker(ExtFacts& facts, int pg, QPDFObjectHandle r, QPDFObjGen fo, bool inF, int d, std::set<QPDFObjGen>& vis,
         std::vector<Mark> outerMarks, std::vector<size_t> k, const std::string& parentKey = std::string())
      : f(facts), page(pg), res(r), form(fo), inForm(inF), depth(d), visiting(vis), marks(std::move(outerMarks)), key(std::move(k)) {
    if (!parentKey.empty()) {
      resKey = parentKey;
    } else if (res.isDictionary() && res.isIndirect()) {
      resKey = "o" + std::to_string(res.getObjGen().getObj()) + "_" + std::to_string(res.getObjGen().getGen());
    } else {
      resKey = "p" + std::to_string(page) + "f" + std::to_string(form.getObj());
    }
    if (res.isDictionary() && !f.resDicts.count(resKey)) f.resDicts[resKey] = res;
    record(0);
  }

  void record(size_t at) {
    MarkedState s;
    for (size_t i = 0; i < marks.size(); ++i) {
      const Mark& m = marks[i];
      if (m.oc.isDictionary()) {
        s.ocConds.push_back(m.oc);
        for (const std::string& n : ocgNames(m.oc)) {
          if (std::find(s.layers.begin(), s.layers.end(), n) == s.layers.end()) s.layers.push_back(n);
        }
      }
      if (m.artifact) {
        if (s.tagged) s.artifactInsideTagged = true;
        s.artifact = true;
        s.artifactType = m.type;
        s.artifactSubtype = m.subtype;
      }
      if (m.mcid >= 0) {
        if (s.artifact) s.taggedInsideArtifact = true;
        s.tagged = true;
        s.mcid = m.mcid;
      }
    }
    if (f.mcStates.size() > 2000000) return;
    f.mcStates.push_back(std::move(s));
    f.mcIndex[key].push_back({at, static_cast<int>(f.mcStates.size() - 1)});
  }

  QPDFObjectHandle resEntry(const char* cat, QPDFObjectHandle name) {
    if (!name.isName() || !res.isDictionary()) return QPDFObjectHandle::newNull();
    QPDFObjectHandle d = res.getKey(cat);
    return d.isDictionary() ? d.getKey(name.getName()) : QPDFObjectHandle::newNull();
  }

  void painted(bool needFill, bool needStroke, bool text) {
    if (text && !inForm) {
      bool art = false, lang = false;
      long long mcid = -1;
      for (const Mark& m : marks) {
        art = art || m.artifact;
        lang = lang || m.lang;
        if (m.mcid >= 0) mcid = m.mcid;
      }
      if (mcid >= 0 && !lang) f.mcidText[page].insert(mcid);
      else if (mcid < 0 && !art && !lang) f.untaggedText.insert(page);
    }
    if (inForm) {
      bool ok = true;
      if (needFill && !st.fill) ok = false;
      if (needStroke && (!st.stroke || !st.lw)) ok = false;
      if (text && !st.font) ok = false;
      if (!ok) f.formsDependent.insert(form);
    }
  }

  void handleObject(QPDFObjectHandle o, size_t off, size_t) override {
    offset = off;
    if (!o.isOperator()) {
      args.push_back(o);
      return;
    }
    std::string op = o.getOperatorValue();
    QPDFObjectHandle a0 = args.empty() ? QPDFObjectHandle::newNull() : args.front();
    if (op == "q") {
      sstack.push_back(st);
    } else if (op == "Q") {
      if (!sstack.empty()) {
        st = sstack.back();
        sstack.pop_back();
      }
    } else if (op == "cs" || op == "CS") {
      if (a0.isName() && !isDeviceName(a0.getName())) f.csUsed[resKey].insert(a0.getName());
      (op == "cs" ? st.fill : st.stroke) = true;
    } else if (op == "sc" || op == "scn" || op == "g" || op == "rg" || op == "k") {
      st.fill = true;
    } else if (op == "SC" || op == "SCN" || op == "G" || op == "RG" || op == "K") {
      st.stroke = true;
    } else if (op == "w") {
      st.lw = true;
    } else if (op == "Tf") {
      st.font = true;
    } else if (op == "Tr") {
      if (a0.isNumber()) st.tr = static_cast<int>(a0.getNumericValue());
    } else if (op == "gs") {
      if (a0.isName()) f.gsUsed[resKey].insert(a0.getName());
      QPDFObjectHandle g = resEntry("/ExtGState", a0);
      if (g.isDictionary()) {
        if (g.hasKey("/LW")) st.lw = true;
        if (g.hasKey("/Font")) st.font = true;
      }
    } else if (op == "f" || op == "F" || op == "f*") {
      painted(true, false, false);
    } else if (op == "S" || op == "s") {
      painted(false, true, false);
    } else if (op == "B" || op == "B*" || op == "b" || op == "b*") {
      painted(true, true, false);
    } else if (op == "sh") {
      painted(false, false, false);
    } else if (op == "EI") {
      painted(false, false, false);
    } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
      int m = st.tr & 3;
      bool fill = (st.tr < 7) && (m == 0 || m == 2);
      bool stroke = (st.tr < 7) && (m == 1 || m == 2);
      painted(fill, stroke, true);
    } else if (op == "BDC" || op == "BMC") {
      Mark mk;
      mk.artifact = !args.empty() && nameIs(args[0], "/Artifact");
      if (op == "BDC" && args.size() >= 2) {
        QPDFObjectHandle props = args[1];
        if (props.isName()) props = resEntry("/Properties", props);
        if (nameIs(args[0], "/OC") && props.isDictionary()) mk.oc = props;
        if (props.isDictionary()) {
          mk.type = bare(props.getKey("/Type"));
          mk.subtype = bare(props.getKey("/Subtype"));
          mk.lang = props.getKey("/Lang").isString();
        }
        if (props.isDictionary() && props.getKey("/MCID").isInteger()) {
          long long id = props.getKey("/MCID").getIntValue();
          if (!mk.artifact) mk.mcid = id;
          if (inForm) f.formsWithMcid.insert(form);
          else f.mcidCount[page][id]++;
        }
      }
      marks.push_back(mk);
      record(off);
    } else if (op == "EMC") {
      if (!marks.empty()) marks.pop_back();
      record(off);
    } else if (op == "Do") {
      if (a0.isName()) f.xoUsed[resKey].insert(a0.getName());
      QPDFObjectHandle x = resEntry("/XObject", a0);
      if (x.isStream()) {
        QPDFObjectHandle d = x.getDict();
        if (x.isIndirect()) {
          f.doCount[x.getObjGen()]++;
          f.xobjPages[x.getObjGen()].insert(page);
        }
        QPDFObjectHandle xoc = d.getKey("/OC");
        if (nameIs(d.getKey("/Subtype"), "/Form")) {
          QPDFObjGen og = x.isIndirect() ? x.getObjGen() : QPDFObjGen();
          if (x.isIndirect() && f.formsSeen.insert(og).second) f.formDicts.push_back({d, og});
          if (depth < 12 && (!x.isIndirect() || !visiting.count(og))) {
            if (x.isIndirect()) visiting.insert(og);
            QPDFObjectHandle fr = d.getKey("/Resources");
            std::vector<Mark> inner = marks;
            if (xoc.isDictionary()) {
              Mark mk;
              mk.oc = xoc;
              inner.push_back(mk);
            }
            std::vector<size_t> k = key;
            k.push_back(off);
            Walker w(f, page, fr.isDictionary() ? fr : res, og, true, depth + 1, visiting, inner, k,
                     fr.isDictionary() ? std::string() : resKey);
            try {
              QPDFObjectHandle::parseContentStream(x, &w);
            } catch (...) {
            }
            if (x.isIndirect()) visiting.erase(og);
          }
        } else {
          bool mask = d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue();
          painted(mask, false, false);
        }
      }
    }
    args.clear();
  }

  void handleEOF() override {}
};

bool csHasDevice(QPDFObjectHandle cs, QPDFObjectHandle res, const std::string& dev, int depth = 0) {
  if (depth > 8) return false;
  if (cs.isName()) {
    std::string n = cs.getName();
    if (n == dev) return true;
    return false;
  }
  if (!cs.isArray() || cs.getArrayNItems() < 1) return false;
  std::string fam = nameOf(cs.getArrayItem(0));
  if ((fam == "/Separation" || fam == "/DeviceN") && cs.getArrayNItems() >= 3) return csHasDevice(cs.getArrayItem(2), res, dev, depth + 1);
  if ((fam == "/Indexed" || fam == "/I") && cs.getArrayNItems() >= 2) return csHasDevice(cs.getArrayItem(1), res, dev, depth + 1);
  return false;
}

void parseDParts(const Events& ev, ExtFacts& f) {
  QPDFObjectHandle dr = ev.root.isDictionary() ? ev.root.getKey("/DPartRoot") : QPDFObjectHandle();
  if (!dr.isDictionary()) return;
  if (dr.getKey("/RecordLevel").isInteger()) f.recordLevel = static_cast<int>(dr.getKey("/RecordLevel").getIntValue());
  std::set<QPDFObjGen> seen;
  std::function<void(QPDFObjectHandle, int, int)> walk = [&](QPDFObjectHandle d, int depth, int parent) {
    if (!d.isDictionary() || depth > 64 || f.dparts.size() > 100000) return;
    if (d.isIndirect() && !seen.insert(d.getObjGen()).second) return;
    DPart p;
    p.dict = d;
    p.depth = depth;
    p.parent = parent;
    auto pg = [&](const char* k) {
      QPDFObjectHandle o = d.getKey(k);
      if (!o.isDictionary() || !o.isIndirect()) return -1;
      auto it = f.pageIndex.find(o.getObjGen());
      return it == f.pageIndex.end() ? -1 : it->second;
    };
    p.start = pg("/Start");
    p.end = pg("/End");
    QPDFObjectHandle kids = d.getKey("/DParts");
    p.leaf = !kids.isArray();
    int me = static_cast<int>(f.dparts.size());
    f.dparts.push_back(p);
    for (int i = 0; kids.isArray() && i < kids.getArrayNItems(); ++i) {
      QPDFObjectHandle grp = kids.getArrayItem(i);
      for (int j = 0; grp.isArray() && j < grp.getArrayNItems(); ++j) walk(grp.getArrayItem(j), depth + 1, me);
    }
  };
  walk(dr.getKey("/DPartRootNode"), 0, -1);
  int lvl = f.recordLevel >= 0 ? f.recordLevel : 1;
  for (size_t i = 0; i < f.dparts.size(); ++i) {
    const DPart& p = f.dparts[i];
    if (p.start < 0) continue;
    int rec = static_cast<int>(i);
    while (rec >= 0 && f.dparts[rec].depth > lvl) rec = f.dparts[rec].parent;
    int e = p.end >= p.start ? p.end : p.start;
    for (int pg = p.start; pg <= e; ++pg) f.pageRecord[pg] = rec;
  }
}

ExtFacts& cacheOf(const Events& ev) {
  if (!ev.extCache) ev.extCache = std::make_shared<ExtFacts>();
  return *static_cast<ExtFacts*>(ev.extCache.get());
}

ExtFacts& facts(const Events& ev) {
  ExtFacts& f = cacheOf(ev);
  if (f.walked) return f;
  f.walked = true;
  for (size_t i = 0; i < ev.pages.size(); ++i) {
    if (ev.pages[i].obj.isIndirect()) f.pageIndex[ev.pages[i].obj.getObjGen()] = static_cast<int>(i);
  }
  std::set<QPDFObjGen> visiting;
  for (size_t i = 0; i < ev.pages.size(); ++i) {
    QPDFObjectHandle pg = ev.pages[i].obj;
    if (!pg.isDictionary()) continue;
    QPDFObjectHandle res = QPDFPageObjectHelper(pg).getAttribute("/Resources", false);
    Walker w(f, static_cast<int>(i), res, QPDFObjGen(), false, 0, visiting, {}, {static_cast<size_t>(ev.pages[i].page)});
    QPDFObjectHandle c = pg.getKey("/Contents");
    if (!c.isStream() && !c.isArray()) continue;
    try {
      QPDFObjectHandle::parseContentStream(c, &w);
    } catch (...) {
    }
  }
  for (const auto& kvRes : f.resDicts) {
    const std::string& key = kvRes.first;
    QPDFObjectHandle res = kvRes.second;
    QPDFObjectHandle gsd = res.getKey("/ExtGState");
    if (gsd.isDictionary()) {
      for (const std::string& k : gsd.getKeys()) {
        QPDFObjectHandle bm = gsd.getKey(k).isDictionary() ? gsd.getKey(k).getKey("/BM") : QPDFObjectHandle();
        if (bm.isArray() && bm.getArrayNItems() > 0) bm = bm.getArrayItem(0);
        bool special = bm.isName() && bm.getName() != "/Normal" && bm.getName() != "/Compatible";
        if (special && !f.gsUsed[key].count(k)) f.refUnusedBM = true;
      }
    }
    auto mark = [&f, res](QPDFObjectHandle cs) {
      if (csHasDevice(cs, res, "/DeviceGray")) f.refUnusedGray = true;
      if (csHasDevice(cs, res, "/DeviceRGB")) f.refUnusedRGB = true;
      if (csHasDevice(cs, res, "/DeviceCMYK")) f.refUnusedCMYK = true;
    };
    QPDFObjectHandle csd = res.getKey("/ColorSpace");
    if (csd.isDictionary()) {
      for (const std::string& k : csd.getKeys()) {
        if (!f.csUsed[key].count(k)) mark(csd.getKey(k));
      }
    }
    QPDFObjectHandle xd = res.getKey("/XObject");
    if (xd.isDictionary()) {
      for (const std::string& k : xd.getKeys()) {
        QPDFObjectHandle x = xd.getKey(k);
        if (!x.isStream() || !nameIs(x.getDict().getKey("/Subtype"), "/Image") || f.xoUsed[key].count(k)) continue;
        mark(x.getDict().getKey("/ColorSpace"));
      }
    }
  }
  parseDParts(ev, f);
  return f;
}

std::vector<QPDFObjectHandle> outputIntents(const Events& ev, const std::string& sub) {
  std::vector<QPDFObjectHandle> out;
  QPDFObjectHandle oi = ev.root.isDictionary() ? ev.root.getKey("/OutputIntents") : QPDFObjectHandle();
  for (int i = 0; oi.isArray() && i < oi.getArrayNItems(); ++i) {
    QPDFObjectHandle o = oi.getArrayItem(i);
    if (o.isDictionary() && (sub.empty() || nameIs(o.getKey("/S"), sub))) out.push_back(o);
  }
  return out;
}

std::string iccSpace(const std::string& prof) { return prof.size() >= 20 ? prof.substr(16, 4) : std::string(); }

std::vector<std::string> processColorants(const std::string& space) {
  if (space == "CMYK") return {"Cyan", "Magenta", "Yellow", "Black"};
  if (space == "RGB ") return {"Red", "Green", "Blue"};
  if (space == "GRAY") return {"Black"};
  return {};
}

struct EmbFile {
  std::string name;
  QPDFObjectHandle fs;
  QPDFObjectHandle ef;
};

std::vector<EmbFile> embeddedFiles(const Events& ev) {
  std::vector<EmbFile> out;
  std::set<QPDFObjGen> seen;
  auto add = [&](QPDFObjectHandle fs, std::string key) {
    if (!fs.isDictionary()) return;
    if (fs.isIndirect() && !seen.insert(fs.getObjGen()).second) return;
    QPDFObjectHandle ef = fs.getKey("/EF");
    QPDFObjectHandle s = ef.isDictionary() ? (ef.getKey("/UF").isStream() ? ef.getKey("/UF") : ef.getKey("/F")) : QPDFObjectHandle();
    std::string n = fs.getKey("/UF").isString() ? fs.getKey("/UF").getUTF8Value()
                    : fs.getKey("/F").isString() ? fs.getKey("/F").getUTF8Value() : key;
    out.push_back({n, fs, s});
  };
  std::set<QPDFObjGen> nodes;
  std::function<void(QPDFObjectHandle, int)> tree = [&](QPDFObjectHandle n, int depth) {
    if (!n.isDictionary() || depth > 32) return;
    if (n.isIndirect() && !nodes.insert(n.getObjGen()).second) return;
    QPDFObjectHandle names = n.getKey("/Names");
    for (int i = 0; names.isArray() && i + 1 < names.getArrayNItems(); i += 2) {
      add(names.getArrayItem(i + 1), bare(names.getArrayItem(i)));
    }
    QPDFObjectHandle kids = n.getKey("/Kids");
    for (int i = 0; kids.isArray() && i < kids.getArrayNItems(); ++i) tree(kids.getArrayItem(i), depth + 1);
  };
  if (ev.root.isDictionary()) {
    QPDFObjectHandle nm = ev.root.getKey("/Names");
    if (nm.isDictionary()) tree(nm.getKey("/EmbeddedFiles"), 0);
    QPDFObjectHandle af = ev.root.getKey("/AF");
    for (int i = 0; af.isArray() && i < af.getArrayNItems(); ++i) add(af.getArrayItem(i), "");
  }
  for (const AnnotFacts& an : ev.annots) {
    if (an.subtype == "FileAttachment" && an.obj.isDictionary()) add(an.obj.getKey("/FS"), "");
  }
  return out;
}

std::string lowerStr(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool isInvoiceName(const std::string& n) {
  std::string l = lowerStr(n);
  return l == "factur-x.xml" || l == "zugferd-invoice.xml" || l == "xrechnung.xml" || l == "order-x.xml" ||
         l == "zugferd-invoice.xml" || l == "zugferd_invoice.xml";
}

std::string md5Raw(const std::string& data) {
  auto c = QPDFCryptoProvider::getImpl();
  c->MD5_init();
  c->MD5_update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
  c->MD5_finalize();
  QPDFCryptoImpl::MD5_Digest d;
  c->MD5_digest(d);
  return std::string(reinterpret_cast<const char*>(d), sizeof(d));
}

std::string inputDir(const Events& ev) {
  if (ev.inputPath.empty()) return std::string();
  std::filesystem::path p(ev.inputPath);
  return p.has_parent_path() ? p.parent_path().string() : std::string(".");
}

std::string resolveRef(const Events& ev, std::string url) {
  if (url.rfind("file://", 0) == 0) url = url.substr(7);
  if (url.find("://") != std::string::npos || url.empty()) return std::string();
  std::filesystem::path p(url);
  if (p.is_relative() && !ev.inputPath.empty()) p = std::filesystem::path(inputDir(ev)) / p;
  std::error_code ec;
  return std::filesystem::is_regular_file(p, ec) ? p.string() : std::string();
}

std::string readFileBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string fsUrl(QPDFObjectHandle fs) {
  if (fs.isString()) return fs.getUTF8Value();
  if (!fs.isDictionary()) return std::string();
  for (const char* k : {"/UF", "/F", "/Unix", "/DOS", "/Mac"}) {
    if (fs.getKey(k).isString()) return fs.getKey(k).getUTF8Value();
  }
  return std::string();
}

template <class F>
void forEachColor(const Events& ev, int page, F fn) {
  for (const PaintEvent& e : ev.paints) {
    if (page < 0 || e.page == page) fn(e.color);
  }
  for (const TextEvent& e : ev.texts) {
    if (page < 0 || e.page == page) fn(e.color);
  }
  for (const ImageEvent& e : ev.images) {
    if (page < 0 || e.page == page) fn(e.color);
  }
}

std::vector<QPDFObjectHandle> schemaFiles(QPDFObjectHandle md) {
  std::vector<QPDFObjectHandle> out;
  QPDFObjectHandle af = md.isStream() ? md.getDict().getKey("/AF") : QPDFObjectHandle();
  if (af.isDictionary()) {
    QPDFObjectHandle one = af;
    af = QPDFObjectHandle::newArray();
    af.appendItem(one);
  }
  for (int i = 0; af.isArray() && i < af.getArrayNItems(); ++i) {
    QPDFObjectHandle fs = af.getArrayItem(i);
    if (fs.isDictionary() && nameIs(fs.getKey("/AFRelationship"), "/Schema")) out.push_back(fs);
  }
  return out;
}

bool relaxNgValid(QPDFObjectHandle fs) {
  QPDFObjectHandle ef = fs.getKey("/EF");
  QPDFObjectHandle s = ef.isDictionary() ? (ef.getKey("/UF").isStream() ? ef.getKey("/UF") : ef.getKey("/F")) : QPDFObjectHandle();
  std::string ns, local;
  if (!xmlRootOf(bytesOf(s), ns, local)) return false;
  return ns == "http://relaxng.org/ns/structure/1.0" && (local == "grammar" || local == "element" || local == "choice" ||
                                                         local == "interleave" || local == "group" || local == "oneOrMore" ||
                                                         local == "zeroOrMore" || local == "optional");
}

bool findId(const std::string& s, size_t from, std::string& id0, std::string& id1) {
  size_t p = s.find("/ID", from);
  if (p == std::string::npos) return false;
  size_t a = s.find('<', p), b = a == std::string::npos ? a : s.find('>', a);
  size_t c = b == std::string::npos ? b : s.find('<', b), d = c == std::string::npos ? c : s.find('>', c);
  if (d == std::string::npos || d - p > 300) return false;
  id0 = lowerStr(s.substr(a + 1, b - a - 1));
  id1 = lowerStr(s.substr(c + 1, d - c - 1));
  return true;
}

bool linIdMismatch(const Events& ev) {
  if (!ev.inputData || ev.inputSize < 64) return false;
  std::string head(reinterpret_cast<const char*>(ev.inputData), std::min<size_t>(ev.inputSize, 2048));
  size_t lin = head.find("/Linearized");
  if (lin == std::string::npos) return false;
  std::string all(reinterpret_cast<const char*>(ev.inputData), ev.inputSize);
  std::string f0, f1, l0, l1;
  if (!findId(all, lin, f0, f1)) return false;
  size_t last = all.rfind("/ID");
  while (last != std::string::npos && !findId(all, last, l0, l1)) last = last ? all.rfind("/ID", last - 1) : std::string::npos;
  if (last == std::string::npos) return false;
  return f0 != l0 || f1 != l1;
}

bool isRect(QPDFObjectHandle o) {
  if (!o.isArray() || o.getArrayNItems() != 4) return false;
  for (int i = 0; i < 4; ++i) {
    if (!o.getArrayItem(i).isNumber()) return false;
  }
  return true;
}

std::vector<std::pair<std::string, std::string>> dictionaryProblems(const Events& ev) {
  std::vector<std::pair<std::string, std::string>> out;
  QPDF* q = ev.root.isDictionary() ? ev.root.getOwningQPDF() : nullptr;
  if (!q) return out;
  auto add = [&](const char* v, const std::string& type) {
    if (out.size() < 5000) out.push_back({v, type});
  };
  if (!ev.root.getKey("/Pages").isDictionary()) add("required_key_missing", "Catalog");
  static const std::set<std::string> std14 = {"Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic", "Helvetica",
                                              "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique", "Courier",
                                              "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique", "Symbol", "ZapfDingbats"};
  for (QPDFObjectHandle o : q->getAllObjects()) {
    QPDFObjectHandle d = o.isStream() ? o.getDict() : o;
    if (!d.isDictionary()) continue;
    std::string type = nameOf(d.getKey("/Type")), sub = nameOf(d.getKey("/Subtype"));
    std::string t = type.empty() ? (o.isStream() && !sub.empty() ? sub.substr(1) : std::string()) : type.substr(1);
    for (const char* k : {"/MediaBox", "/CropBox", "/BleedBox", "/TrimBox", "/ArtBox", "/Rect", "/BBox", "/FontBBox"}) {
      if (!d.hasKey(k)) continue;
      QPDFObjectHandle r = d.getKey(k);
      if (!r.isArray()) add("wrong_type", t);
      else if (r.getArrayNItems() != 4) add("array_wrong_size", t);
      else if (!isRect(r)) add("array_element_type", t);
    }
    for (const char* k : {"/Matrix", "/FontMatrix"}) {
      if (!d.hasKey(k)) continue;
      QPDFObjectHandle m = d.getKey(k);
      if (!m.isArray()) {
        add("wrong_type", t);
        continue;
      }
      if (m.getArrayNItems() != 6) add("array_wrong_size", t);
      for (int i = 0; i < m.getArrayNItems(); ++i) {
        if (!m.getArrayItem(i).isNumber()) {
          add("array_element_type", t);
          break;
        }
      }
    }
    QPDFObjectHandle w = d.getKey("/Widths");
    for (int i = 0; w.isArray() && i < w.getArrayNItems(); ++i) {
      if (!w.getArrayItem(i).isNumber()) {
        add("array_element_type", t);
        break;
      }
    }
    for (const char* k : {"/Kids", "/Annots"}) {
      QPDFObjectHandle a = d.getKey(k);
      for (int i = 0; a.isArray() && i < a.getArrayNItems(); ++i) {
        if (!a.getArrayItem(i).isDictionary()) {
          add("array_element_type", t);
          break;
        }
      }
    }
    QPDFObjectHandle flt = d.getKey("/Filter");
    for (int i = 0; flt.isArray() && i < flt.getArrayNItems(); ++i) {
      if (!flt.getArrayItem(i).isName()) {
        add("array_element_type", t);
        break;
      }
    }
    auto need = [&](bool ok) {
      if (!ok) add("required_key_missing", t);
    };
    if (type == "/Page") {
      need(d.getKey("/Parent").isDictionary());
    } else if (type == "/Pages") {
      need(d.getKey("/Kids").isArray());
      need(d.getKey("/Count").isInteger());
    } else if (type == "/Font") {
      need(d.getKey("/Subtype").isName());
      std::string bf = nameOf(d.getKey("/BaseFont"));
      bool std = bf.size() > 1 && std14.count(bf.substr(1));
      if ((sub == "/Type1" || sub == "/TrueType" || sub == "/MMType1") && !std) {
        need(d.getKey("/FirstChar").isInteger());
        need(d.getKey("/LastChar").isInteger());
        need(d.getKey("/Widths").isArray());
        need(d.getKey("/FontDescriptor").isDictionary());
      }
      if (sub == "/Type0") {
        need(d.getKey("/DescendantFonts").isArray());
        need(!d.getKey("/Encoding").isNull());
      }
      if (sub == "/Type3") {
        need(isRect(d.getKey("/FontBBox")));
        need(d.getKey("/FontMatrix").isArray());
        need(d.getKey("/CharProcs").isDictionary());
        need(d.getKey("/Encoding").isDictionary());
      }
    } else if (type == "/FontDescriptor") {
      need(d.getKey("/FontName").isName());
      need(d.getKey("/Flags").isInteger());
      need(d.getKey("/ItalicAngle").isNumber());
    } else if (type == "/Annot" || (d.hasKey("/Rect") && d.getKey("/Subtype").isName() && type.empty() && d.hasKey("/P"))) {
      need(d.getKey("/Subtype").isName());
      need(isRect(d.getKey("/Rect")));
    }
    if (o.isStream() && sub == "/Image") {
      bool mask = d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue();
      QPDFObjectHandle fl = d.getKey("/Filter");
      bool jpx = nameIs(fl, "/JPXDecode") || (fl.isArray() && fl.getArrayNItems() > 0 && nameIs(fl.getArrayItem(fl.getArrayNItems() - 1), "/JPXDecode"));
      need(d.getKey("/Width").isInteger());
      need(d.getKey("/Height").isInteger());
      if (!mask && !jpx) {
        need(d.getKey("/BitsPerComponent").isInteger());
        need(!d.getKey("/ColorSpace").isNull());
      }
    }
    if (o.isStream() && sub == "/Form") need(isRect(d.getKey("/BBox")));
  }
  return out;
}


std::vector<QPDFObjectHandle> devicenSpaces(const Events& ev) {
  std::vector<QPDFObjectHandle> out;
  std::set<QPDFObjGen> seen;
  auto add = [&](QPDFObjectHandle cs) {
    if (!cs.isArray() || cs.getArrayNItems() < 4 || !nameIs(cs.getArrayItem(0), "/DeviceN")) return;
    if (cs.isIndirect() && !seen.insert(cs.getObjGen()).second) return;
    out.push_back(cs);
  };
  QPDF* q = ev.root.isDictionary() ? ev.root.getOwningQPDF() : nullptr;
  if (q) {
    for (QPDFObjectHandle o : q->getAllObjects()) add(o);
  }
  forEachColor(ev, -1, [&](const ColorInfo& ci) {
    if (ci.cls == "devicen") add(ci.csObj);
  });
  return out;
}

QPDFObjectHandle mixingHints(QPDFObjectHandle cs) {
  if (!cs.isArray() || cs.getArrayNItems() < 5) return QPDFObjectHandle();
  QPDFObjectHandle at = cs.getArrayItem(4);
  return at.isDictionary() ? at.getKey("/MixingHints") : QPDFObjectHandle();
}

int mixHintsConsistent(const Events& ev, bool solidities, bool nchOnly) {
  std::vector<std::string> sigs;
  for (QPDFObjectHandle cs : devicenSpaces(ev)) {
    QPDFObjectHandle at = cs.getArrayNItems() >= 5 ? cs.getArrayItem(4) : QPDFObjectHandle();
    if (nchOnly && !(at.isDictionary() && nameIs(at.getKey("/Subtype"), "/NChannel"))) continue;
    QPDFObjectHandle mh = mixingHints(cs);
    if (!mh.isDictionary()) continue;
    std::string sig;
    if (solidities) {
      QPDFObjectHandle so = mh.getKey("/Solidities");
      for (const std::string& k : so.isDictionary() ? so.getKeys() : std::set<std::string>()) {
        float v = so.getKey(k).isNumber() ? static_cast<float>(so.getKey(k).getNumericValue()) : 0.f;
        char buf[48];
        std::snprintf(buf, sizeof buf, "=%a|", static_cast<double>(v));
        sig += k + buf;
      }
    } else {
      QPDFObjectHandle po = mh.getKey("/PrintingOrder");
      for (int i = 0; po.isArray() && i < po.getArrayNItems(); ++i) sig += bare(po.getArrayItem(i)) + "|";
    }
    sigs.push_back(sig);
  }
  if (sigs.empty()) return -1;
  for (const std::string& g : sigs) {
    if (g != sigs.front()) return 0;
  }
  return 1;
}

bool faceFor(const FontFacts& f, FtLib& lib, FtFace& face) {
  if (f.fontProgram.empty() || !lib.lib) return false;
  face.data = f.fontProgram;
  return FT_New_Memory_Face(lib.lib, reinterpret_cast<const FT_Byte*>(face.data.data()), static_cast<FT_Long>(face.data.size()), 0,
                            &face.face) == 0 &&
         face.face;
}

std::string programErrors(const FontFacts& f) {
  const std::string& d = f.fontProgram;
  auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= d.size() ? (static_cast<unsigned char>(d[o]) << 8) | static_cast<unsigned char>(d[o + 1]) : 0; };
  auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= d.size() ? (u16(o) << 16) | u16(o + 2) : 0; };
  if (d.size() < 12) return std::string();
  uint32_t ver = u32(0);
  if (ver != 0x00010000 && ver != 0x74727565) return std::string();
  struct Tab {
    uint32_t sum, off, len;
  };
  std::map<std::string, Tab> tabs;
  bool badSum = false, badAlign = false;
  uint32_t n = u16(4);
  for (uint32_t i = 0; i < n && 12 + 16 * (i + 1) <= d.size(); ++i) {
    size_t r = 12 + 16 * i;
    std::string tag = d.substr(r, 4);
    Tab tb{u32(r + 4), u32(r + 8), u32(r + 12)};
    tabs[tag] = tb;
    if (tb.off % 4) badAlign = true;
    if (tag != "head" && static_cast<size_t>(tb.off) + tb.len <= d.size()) {
      uint32_t sum = 0;
      for (size_t o = tb.off; o < static_cast<size_t>(tb.off) + tb.len; o += 4) {
        uint32_t w = 0;
        for (size_t k = 0; k < 4; ++k) w = (w << 8) | (o + k < static_cast<size_t>(tb.off) + tb.len ? static_cast<unsigned char>(d[o + k]) : 0);
        sum += w;
      }
      if (sum != tb.sum) badSum = true;
    }
  }
  std::string out;
  auto add = [&](const char* code, const char* text) { out += std::string("(") + code + ") " + text + "\n"; };
  if (!tabs.count("cmap")) add("FONT-NO-CMAP", "no 'cmap' table");
  if (badSum) add("FONT-CHECKSUM", "table checksum mismatch");
  if (badAlign) add("FONT-ALIGN", "table offset not 4-byte aligned");
  uint32_t glyphs = tabs.count("maxp") ? u16(tabs["maxp"].off + 4) : 0;
  if (tabs.count("hhea") && tabs.count("hmtx") && glyphs) {
    uint32_t hm = u16(tabs["hhea"].off + 34);
    uint64_t need = 4ull * hm + (glyphs > hm ? 2ull * (glyphs - hm) : 0);
    if (hm > glyphs) add("FONT-HMTX-COUNT", "'hmtx' has more entries than glyphs");
    else if (tabs["hmtx"].len < need) add("FONT-HMTX-SIZE", "'hmtx' size wrong");
  }
  if (tabs.count("loca") && tabs.count("glyf") && tabs.count("head") && glyphs) {
    bool longFmt = u16(tabs["head"].off + 50) == 1;
    uint32_t prev = 0;
    bool bad = false;
    for (uint32_t g = 0; g <= glyphs && !bad; ++g) {
      size_t o = tabs["loca"].off + (longFmt ? 4ull * g : 2ull * g);
      if (o + (longFmt ? 4 : 2) > static_cast<size_t>(tabs["loca"].off) + tabs["loca"].len) break;
      uint32_t v = longFmt ? u32(o) : 2 * u16(o);
      if (v < prev || v > tabs["glyf"].len) bad = true;
      prev = v;
    }
    if (bad) add("FONT-LOCA", "wrong 'loca' offset");
  }
  FtLib lib;
  FtFace face;
  if (faceFor(f, lib, face)) {
    for (FT_Long g = 0; g < face.face->num_glyphs && g < 6000; ++g) {
      if (FT_Load_Glyph(face.face, static_cast<FT_UInt>(g), FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) {
        add("FONT-GLYPH-DATA", "glyph data damaged");
        break;
      }
    }
  }
  return out;
}

QPDFObjectHandle descOf(const FontFacts& f) {
  QPDFObjectHandle t = f.dict;
  if (f.cid) {
    QPDFObjectHandle df = f.dict.getKey("/DescendantFonts");
    if (df.isArray() && df.getArrayNItems() > 0) t = df.getArrayItem(0);
  }
  return t.isDictionary() ? t.getKey("/FontDescriptor") : QPDFObjectHandle();
}

QPDFObjectHandle cidFontOf(const FontFacts& f) {
  QPDFObjectHandle df = f.dict.getKey("/DescendantFonts");
  return df.isArray() && df.getArrayNItems() > 0 ? df.getArrayItem(0) : QPDFObjectHandle();
}

std::vector<uint32_t> usedCodes(const FontFacts& f, const Events& ev) {
  std::set<uint32_t> s;
  bool two = f.cid;
  for (const TextEvent& te : ev.texts) {
    if (te.fontOg != f.og) continue;
    size_t step = two ? 2 : 1;
    for (size_t i = 0; i + step <= te.bytes.size(); i += step) {
      s.insert(two ? (static_cast<unsigned char>(te.bytes[i]) << 8) | static_cast<unsigned char>(te.bytes[i + 1])
                   : static_cast<unsigned char>(te.bytes[i]));
    }
    if (s.size() > 70000) break;
  }
  return std::vector<uint32_t>(s.begin(), s.end());
}

bool identityEncoding(const FontFacts& f) {
  std::string e = nameOf(f.dict.getKey("/Encoding"));
  return e == "/Identity-H" || e == "/Identity-V";
}

FT_UInt gidForCode(const FontFacts& f, FT_Face face, uint32_t code, const SimpleEncoding& enc) {
  if (f.cid) {
    if (!identityEncoding(f)) return 0;
    QPDFObjectHandle cf = cidFontOf(f);
    QPDFObjectHandle map = cf.isDictionary() ? cf.getKey("/CIDToGIDMap") : QPDFObjectHandle();
    if (map.isStream()) {
      std::string m = bytesOf(map);
      return 2 * code + 1 < m.size() ? (static_cast<unsigned char>(m[2 * code]) << 8) | static_cast<unsigned char>(m[2 * code + 1]) : 0;
    }
    if (f.cid0 && FT_IS_CID_KEYED(face)) {
      for (FT_Long g = 0; g < face->num_glyphs; ++g) {
        FT_UInt cid = 0;
        if (FT_Get_CID_From_Glyph_Index(face, static_cast<FT_UInt>(g), &cid) == 0 && cid == code) return static_cast<FT_UInt>(g);
      }
      return 0;
    }
    return code;
  }
  return glyphForCode(face, static_cast<int>(code & 0xFF), enc, f.symbolic);
}

struct Pt {
  double x, y;
};

struct OutlineSink {
  std::vector<std::vector<Pt>> contours;
  Pt cur{0, 0};
};

int moveTo(const FT_Vector* to, void* u) {
  auto* s = static_cast<OutlineSink*>(u);
  s->contours.push_back({{static_cast<double>(to->x), static_cast<double>(to->y)}});
  s->cur = {static_cast<double>(to->x), static_cast<double>(to->y)};
  return 0;
}

int lineTo(const FT_Vector* to, void* u) {
  auto* s = static_cast<OutlineSink*>(u);
  if (s->contours.empty()) s->contours.push_back({s->cur});
  s->cur = {static_cast<double>(to->x), static_cast<double>(to->y)};
  s->contours.back().push_back(s->cur);
  return 0;
}

int conicTo(const FT_Vector* c, const FT_Vector* to, void* u) {
  auto* s = static_cast<OutlineSink*>(u);
  if (s->contours.empty()) s->contours.push_back({s->cur});
  Pt p0 = s->cur;
  for (int i = 1; i <= 6; ++i) {
    double t = i / 6.0, mt = 1 - t;
    s->contours.back().push_back({mt * mt * p0.x + 2 * mt * t * c->x + t * t * to->x, mt * mt * p0.y + 2 * mt * t * c->y + t * t * to->y});
  }
  s->cur = {static_cast<double>(to->x), static_cast<double>(to->y)};
  return 0;
}

int cubicTo(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* u) {
  auto* s = static_cast<OutlineSink*>(u);
  if (s->contours.empty()) s->contours.push_back({s->cur});
  Pt p0 = s->cur;
  for (int i = 1; i <= 8; ++i) {
    double t = i / 8.0, mt = 1 - t;
    double a = mt * mt * mt, b = 3 * mt * mt * t, c = 3 * mt * t * t, d = t * t * t;
    s->contours.back().push_back({a * p0.x + b * c1->x + c * c2->x + d * to->x, a * p0.y + b * c1->y + c * c2->y + d * to->y});
  }
  s->cur = {static_cast<double>(to->x), static_cast<double>(to->y)};
  return 0;
}

bool outlineOf(FT_Face face, FT_UInt gid, OutlineSink& out) {
  if (FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) return false;
  if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return false;
  FT_Outline_Funcs fn{};
  fn.move_to = moveTo;
  fn.line_to = lineTo;
  fn.conic_to = conicTo;
  fn.cubic_to = cubicTo;
  return FT_Outline_Decompose(&face->glyph->outline, &fn, &out) == 0;
}

bool segCross(Pt a, Pt b, Pt c, Pt d) {
  auto cr = [](Pt o, Pt p, Pt q) { return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x); };
  double d1 = cr(c, d, a), d2 = cr(c, d, b), d3 = cr(a, b, c), d4 = cr(a, b, d);
  return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

bool glyphOverlaps(FT_Face face, FT_UInt gid) {
  OutlineSink s;
  if (!outlineOf(face, gid, s)) return false;
  std::vector<std::pair<int, std::pair<Pt, Pt>>> segs;
  for (size_t c = 0; c < s.contours.size(); ++c) {
    const auto& pts = s.contours[c];
    for (size_t i = 0; i < pts.size(); ++i) segs.push_back({static_cast<int>(c), {pts[i], pts[(i + 1) % pts.size()]}});
  }
  if (segs.size() > 4000) return false;
  for (size_t i = 0; i < segs.size(); ++i) {
    for (size_t j = i + 1; j < segs.size(); ++j) {
      if (segs[i].first == segs[j].first) {
        size_t n = s.contours[segs[i].first].size();
        if (j == i + 1 || (n > 0 && i + n - 1 == j)) continue;
      }
      if (segCross(segs[i].second.first, segs[i].second.second, segs[j].second.first, segs[j].second.second)) return true;
    }
  }
  return false;
}

std::string outlineHash(FT_Face face, FT_UInt gid) {
  OutlineSink s;
  if (!outlineOf(face, gid, s)) return std::string();
  std::string h;
  for (const auto& c : s.contours) {
    for (const Pt& p : c) h += std::to_string(static_cast<long>(p.x)) + "," + std::to_string(static_cast<long>(p.y)) + ";";
    h += "|";
  }
  return h;
}

uint32_t uniFromName(const std::string& n) {
  uint32_t u = 0;
  if (parseUniName(n, u)) return u;
  return aglNameToUnicode(n);
}

std::u32string ligatureExpansion(const std::string& name) {
  std::u32string out;
  if (name.find('_') != std::string::npos) {
    size_t b = 0;
    while (b <= name.size()) {
      size_t e = name.find('_', b);
      std::string part = name.substr(b, e == std::string::npos ? std::string::npos : e - b);
      uint32_t u = uniFromName(part);
      if (!u) return std::u32string();
      out += static_cast<char32_t>(u);
      if (e == std::string::npos) break;
      b = e + 1;
    }
    return out;
  }
  static const std::map<std::string, std::u32string> ligs = {{"ff", U"ff"}, {"fi", U"fi"}, {"fl", U"fl"}, {"ffi", U"ffi"}, {"ffl", U"ffl"}};
  auto it = ligs.find(name);
  if (it != ligs.end()) return it->second;
  uint32_t u = uniFromName(name);
  if (u) out += static_cast<char32_t>(u);
  return out;
}

std::u32string normalizeLig(const std::u32string& s) {
  std::u32string o;
  for (char32_t c : s) {
    switch (c) {
      case 0xFB00: o += U"ff"; break;
      case 0xFB01: o += U"fi"; break;
      case 0xFB02: o += U"fl"; break;
      case 0xFB03: o += U"ffi"; break;
      case 0xFB04: o += U"ffl"; break;
      case 0xFB05: o += U"ſt"; break;
      case 0xFB06: o += U"st"; break;
      default: o += c;
    }
  }
  return o;
}

std::string pdfGlyphName(const FontFacts& f, uint32_t code, const SimpleEncoding& enc) {
  if (f.cid || code > 255) return std::string();
  if (!enc.diffs[code].empty()) return enc.diffs[code].substr(1);
  return std::string();
}

uint32_t pdfUnicode(const FontFacts& f, uint32_t code, const SimpleEncoding& enc) {
  if (f.cid || code > 255) return 0;
  std::string n = pdfGlyphName(f, code, enc);
  if (!n.empty()) return uniFromName(n);
  if (enc.base) return enc.base(static_cast<int>(code));
  return 0;
}

bool cmapSyntaxOk(const std::string& t, bool requireCodespace) {
  if (t.find("begincmap") == std::string::npos || t.find("endcmap") == std::string::npos) return false;
  if (requireCodespace && t.find("begincodespacerange") == std::string::npos) return false;
  for (const char* blk : {"codespacerange", "bfchar", "bfrange", "cidchar", "cidrange", "notdefchar", "notdefrange"}) {
    std::string b = std::string("begin") + blk, e = std::string("end") + blk;
    size_t pos = 0;
    while ((pos = t.find(b, pos)) != std::string::npos) {
      size_t ns = t.find_last_not_of(" \t\r\n", pos == 0 ? 0 : pos - 1);
      size_t numStart = ns == std::string::npos ? 0 : t.find_last_of(" \t\r\n", ns);
      long declared = ns == std::string::npos ? -1 : std::strtol(t.c_str() + (numStart == std::string::npos ? 0 : numStart + 1), nullptr, 10);
      size_t end = t.find(e, pos + b.size());
      if (end == std::string::npos) return false;
      size_t nextBegin = t.find("begin", pos + b.size());
      if (nextBegin != std::string::npos && nextBegin < end) return false;
      std::string body = t.substr(pos + b.size(), end - pos - b.size());
      int hexes = 0, items = 0;
      for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '<') {
          size_t c = body.find('>', i);
          if (c == std::string::npos) return false;
          int digits = 0;
          for (size_t k = i + 1; k < c; ++k) {
            if (std::isxdigit(static_cast<unsigned char>(body[k]))) ++digits;
            else if (!std::isspace(static_cast<unsigned char>(body[k]))) return false;
          }
          if (digits % 2) return false;
          ++hexes;
          i = c;
        } else if (body[i] == '[') {
          size_t c = body.find(']', i);
          if (c == std::string::npos) return false;
          ++items;
          i = c;
        } else if (std::isdigit(static_cast<unsigned char>(body[i])) || body[i] == '/') {
          ++items;
          while (i + 1 < body.size() && !std::isspace(static_cast<unsigned char>(body[i + 1])) && body[i + 1] != '<') ++i;
        }
      }
      int per = std::string(blk).find("range") != std::string::npos ? 3 : 2;
      int total = hexes + items;
      if (std::string(blk) == "codespacerange") per = 2;
      if (declared >= 0 && declared <= 100 && total != declared * per) return false;
      pos = end;
    }
  }
  return true;
}

const std::set<std::string>& predefinedCMaps() {
  static const std::set<std::string> s = {
      "GB-EUC-H", "GB-EUC-V", "GBpc-EUC-H", "GBpc-EUC-V", "GBK-EUC-H", "GBK-EUC-V", "GBKp-EUC-H", "GBKp-EUC-V", "GBK2K-H",
      "GBK2K-V", "UniGB-UCS2-H", "UniGB-UCS2-V", "UniGB-UTF16-H", "UniGB-UTF16-V", "B5pc-H", "B5pc-V", "HKscs-B5-H",
      "HKscs-B5-V", "ETen-B5-H", "ETen-B5-V", "ETenms-B5-H", "ETenms-B5-V", "CNS-EUC-H", "CNS-EUC-V", "UniCNS-UCS2-H",
      "UniCNS-UCS2-V", "UniCNS-UTF16-H", "UniCNS-UTF16-V", "83pv-RKSJ-H", "90ms-RKSJ-H", "90ms-RKSJ-V", "90msp-RKSJ-H",
      "90msp-RKSJ-V", "90pv-RKSJ-H", "Add-RKSJ-H", "Add-RKSJ-V", "EUC-H", "EUC-V", "Ext-RKSJ-H", "Ext-RKSJ-V", "H", "V",
      "UniJIS-UCS2-H", "UniJIS-UCS2-V", "UniJIS-UCS2-HW-H", "UniJIS-UCS2-HW-V", "UniJIS-UTF16-H", "UniJIS-UTF16-V",
      "KSC-EUC-H", "KSC-EUC-V", "KSCms-UHC-H", "KSCms-UHC-V", "KSCms-UHC-HW-H", "KSCms-UHC-HW-V", "KSCpc-EUC-H",
      "UniKS-UCS2-H", "UniKS-UCS2-V", "UniKS-UTF16-H", "UniKS-UTF16-V", "Identity-H", "Identity-V"};
  return s;
}

void type3Names(QPDFObjectHandle cp, std::map<std::string, std::set<std::string>>& used) {
  struct Cb : QPDFObjectHandle::ParserCallbacks {
    std::map<std::string, std::set<std::string>>& u;
    std::vector<QPDFObjectHandle> args;
    explicit Cb(std::map<std::string, std::set<std::string>>& x) : u(x) {}
    void handleObject(QPDFObjectHandle o, size_t, size_t) override {
      if (!o.isOperator()) {
        args.push_back(o);
        return;
      }
      std::string op = o.getOperatorValue();
      auto nm = [&](size_t i) { return i < args.size() && args[i].isName() ? args[i].getName() : std::string(); };
      if (op == "Tf") u["/Font"].insert(nm(0));
      else if (op == "Do") u["/XObject"].insert(nm(0));
      else if (op == "gs") u["/ExtGState"].insert(nm(0));
      else if (op == "sh") u["/Shading"].insert(nm(0));
      else if ((op == "cs" || op == "CS") && !isDeviceName(nm(0))) u["/ColorSpace"].insert(nm(0));
      else if ((op == "scn" || op == "SCN") && !args.empty() && args.back().isName()) u["/Pattern"].insert(args.back().getName());
      else if (op == "BDC" && args.size() >= 2 && args[1].isName()) u["/Properties"].insert(args[1].getName());
      args.clear();
    }
    void handleEOF() override {}
  } cb(used);
  try {
    QPDFObjectHandle::parseContentStream(cp, &cb);
  } catch (...) {
  }
  for (auto& kv : used) kv.second.erase(std::string());
}

std::string iccProfileId(const std::string& p) {
  if (p.size() < 128) return std::string();
  std::string id = p.substr(84, 16);
  if (id != std::string(16, '\0')) return id;
  std::string z = p;
  for (size_t i : {44, 45, 46, 47, 64, 65, 66, 67}) z[i] = '\0';
  for (size_t i = 84; i < 100; ++i) z[i] = '\0';
  return md5Raw(z);
}

bool blendMatches(const GsExtra& x, const Events& ev, const std::vector<std::string>& subs, bool positive, bool strict) {
  if (!x.groupHasCS) return false;
  std::vector<std::string> ois;
  for (const std::string& sub : subs) {
    for (QPDFObjectHandle oi : outputIntents(ev, sub)) {
      QPDFObjectHandle dp = oi.getKey("/DestOutputProfile");
      if (dp.isStream()) ois.push_back(iccProfileId(bytesOf(dp)));
    }
  }
  if (ois.empty()) return false;
  QPDFObjectHandle cs = x.groupCSObj;
  bool icc = cs.isArray() && cs.getArrayNItems() >= 2 && nameIs(cs.getArrayItem(0), "/ICCBased") && cs.getArrayItem(1).isStream();
  if (!icc) return !positive && !strict;
  std::string id = iccProfileId(bytesOf(cs.getArrayItem(1)));
  bool any = false, miss = false;
  for (const std::string& o : ois) {
    if (!id.empty() && o == id) any = true;
    else miss = true;
  }
  if (positive) return any;
  return strict ? miss : true;
}

std::string csNickName(QPDFObjectHandle cs, const std::vector<std::string>& vals) {
  static const std::set<std::string> known = {"DeviceGray", "DeviceRGB", "DeviceCMYK", "CalGray", "CalRGB", "Lab",
                                              "ICCBased", "Separation", "DeviceN", "Indexed", "Pattern"};
  if (cs.isName()) {
    std::string n = cs.getName().substr(1);
    if (n == "G") n = "DeviceGray";
    if (n == "RGB") n = "DeviceRGB";
    if (n == "CMYK") n = "DeviceCMYK";
    return known.count(n) ? n : std::string();
  }
  if (!cs.isArray() || cs.getArrayNItems() == 0) return std::string();
  std::string fam = bare(cs.getArrayItem(0));
  if (!known.count(fam)) return std::string();
  if (fam != "ICCBased" || std::find(vals.begin(), vals.end(), "ICCBased") != vals.end()) return fam;
  QPDFObjectHandle s = cs.getArrayNItems() > 1 && cs.getArrayItem(1).isStream() ? cs.getArrayItem(1) : QPDFObjectHandle();
  if (!s.isStream()) return fam;
  std::string sp = iccSpace(bytesOf(s));
  if (sp == "GRAY") return fam + "Gray";
  if (sp == "RGB ") return fam + "RGB";
  if (sp == "CMYK") return fam + "CMYK";
  if (sp == "Lab ") return fam + "Lab";
  return fam;
}

QPDFObjectHandle groupIcc(const GsExtra& x, int& n) {
  n = 0;
  QPDFObjectHandle cs = x.groupCSObj;
  if (!x.groupHasCS || !cs.isArray() || cs.getArrayNItems() < 2 || !nameIs(cs.getArrayItem(0), "/ICCBased") ||
      !cs.getArrayItem(1).isStream()) {
    return QPDFObjectHandle();
  }
  QPDFObjectHandle nk = cs.getArrayItem(1).getDict().getKey("/N");
  n = nk.isInteger() ? static_cast<int>(nk.getIntValue()) : 0;
  return cs;
}

const ColorInfo* colorOf(const Subject& s) {
  if (s.paint) return &s.paint->color;
  if (s.text) return &s.text->color;
  if (s.image) return &s.image->color;
  return nullptr;
}

const GsExtra* gsOf(const Subject& s) {
  if (s.paint) return &s.paint->x;
  if (s.text) return &s.text->x;
  if (s.image) return &s.image->x;
  return nullptr;
}

const std::vector<size_t>* srcOf(const Subject& s) {
  if (s.paint) return &s.paint->src;
  if (s.text) return &s.text->src;
  if (s.image) return &s.image->src;
  return nullptr;
}

bool sameStream(QPDFObjectHandle a, QPDFObjectHandle b) {
  if (!a.isStream() || !b.isStream()) return false;
  if (a.isIndirect() && b.isIndirect() && a.getObjGen() == b.getObjGen()) return true;
  std::string x = bytesOf(a), y = bytesOf(b);
  return !x.empty() && x == y;
}

QPDFObjectHandle iccStreamOf(const ColorInfo& ci, int comps) {
  QPDFObjectHandle cs = ci.csObj;
  if (ci.indexed && cs.isArray() && cs.getArrayNItems() > 1 && nameIs(cs.getArrayItem(0), "/Indexed")) cs = cs.getArrayItem(1);
  if (ci.cls != "icc" || (comps && ci.declaredComps != comps) || !cs.isArray() || cs.getArrayNItems() < 2) return QPDFObjectHandle();
  return cs.getArrayItem(1).isStream() ? cs.getArrayItem(1) : QPDFObjectHandle();
}

QPDFObjGen formOf(const Events& ev, const GsExtra& x) {
  if (!x.inForm || !x.formDict.isDictionary()) return QPDFObjGen();
  for (const auto& fd : facts(ev).formDicts) {
    if (fd.first.isSameObjectAs(x.formDict)) return fd.second;
  }
  return QPDFObjGen();
}

std::vector<std::string> unicodeConflicts(const TextEvent& te, const Events& ev) {
  std::vector<std::string> out;
  const FontFacts* ff = nullptr;
  for (const FontFacts& f : ev.fonts) {
    if (f.og == te.fontOg) ff = &f;
  }
  if (!ff || ff->type3 || !ff->dict.isDictionary()) return out;
  const auto& tum = toUnicodeMap(*ff, ev);
  if (tum.empty()) return out;
  SimpleEncoding enc = readEncoding(ff->dict, ff->symbolic);
  ExtFacts& cache = cacheOf(ev);
  std::shared_ptr<FontText>& slot = cache.fontText[ff->og];
  if (!slot) {
    slot = std::make_shared<FontText>();
    slot->ok = ff->embedded && faceFor(*ff, cache.ftLib, slot->face);
    if (slot->ok && FT_Select_Charmap(slot->face.face, FT_ENCODING_UNICODE) == 0) {
      FT_UInt gi = 0;
      FT_ULong c = FT_Get_First_Char(slot->face.face, &gi);
      while (gi != 0) {
        slot->gidUni[gi].insert(static_cast<uint32_t>(c));
        c = FT_Get_Next_Char(slot->face.face, c, &gi);
      }
    }
  }
  FontText& ftx = *slot;
  FtFace& face = ftx.face;
  bool two = ff->cid;
  if (two && !identityEncoding(*ff)) return out;
  bool fontCmap = false, pdfEnc = false, lig = false;
  size_t step = two ? 2 : 1;
  for (size_t i = 0; i + step <= te.bytes.size(); i += step) {
    uint32_t code = two ? (static_cast<unsigned char>(te.bytes[i]) << 8) | static_cast<unsigned char>(te.bytes[i + 1])
                        : static_cast<unsigned char>(te.bytes[i]);
    auto it = tum.find(code);
    if (it == tum.end() || it->second.empty()) continue;
    const std::u32string& tu = it->second;
    if (!lig) {
      std::u32string n = normalizeLig(tu);
      bool letters = n.size() >= 2 && std::all_of(n.begin(), n.end(), [](char32_t c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == 0x17F;
      });
      if (letters) {
        std::string gname = pdfGlyphName(*ff, code, enc);
        if (gname.empty() && ftx.ok && FT_HAS_GLYPH_NAMES(face.face)) {
          FT_UInt gid = gidForCode(*ff, face.face, code, enc);
          char buf[128] = {0};
          if (gid && FT_Get_Glyph_Name(face.face, gid, buf, sizeof buf) == 0) gname = buf;
        }
        if (!gname.empty() && normalizeLig(ligatureExpansion(gname)) != n) lig = true;
      }
    }
    if (tu.size() != 1) continue;
    if (!pdfEnc) {
      uint32_t u = pdfUnicode(*ff, code, enc);
      if (u && u != 0xFFFD && u != static_cast<uint32_t>(tu[0])) pdfEnc = true;
    }
    if (!fontCmap && ftx.ok) {
      FT_UInt gid = gidForCode(*ff, face.face, code, enc);
      if (!gid) continue;
      std::set<uint32_t> expect;
      char buf[128] = {0};
      if (FT_HAS_GLYPH_NAMES(face.face) && FT_Get_Glyph_Name(face.face, gid, buf, sizeof buf) == 0 && buf[0]) {
        if (uint32_t u = uniFromName(buf)) expect.insert(u);
      }
      auto gu = ftx.gidUni.find(gid);
      if (gu != ftx.gidUni.end()) expect.insert(gu->second.begin(), gu->second.end());
      expect.erase(0xFFFD);
      if (!expect.empty() && !expect.count(static_cast<uint32_t>(tu[0]))) fontCmap = true;
    }
  }
  if (fontCmap) out.push_back("font_cmap");
  if (pdfEnc) out.push_back("pdf_encoding");
  if (lig) out.push_back("ligature");
  return out;
}

std::vector<std::string> deviceSpaceRules(const ColorInfo& ci, const GsExtra& x, const Events& ev) {
  std::vector<std::string> out;
  bool device = ci.cls == "gray" || ci.cls == "rgb" || ci.cls == "cmyk";
  if (device && ci.hasDefault) out.push_back("default_space");
  QPDFObjectHandle g = x.groupCSObj;
  std::string fam = x.groupHasCS && g.isArray() && g.getArrayNItems() > 0 ? bare(g.getArrayItem(0)) : std::string();
  if (ev.pdfVersion == "2.0" && ci.cls == "cmyk" && !ci.hasDefault &&
      (fam == "ICCBased" || fam == "CalGray" || fam == "CalRGB" || fam == "Lab")) {
    out.push_back("group_blend_space_pdf2");
  }
  int n = 0;
  groupIcc(x, n);
  if (ci.cls == "cmyk" && !ci.hasDefault && n == 4) out.push_back("cmyk_redefined_by_group");
  if (blendMatches(x, ev, {"/GTS_PDFA1"}, true, false)) out.push_back("matches_pdfa_intent");
  if (blendMatches(x, ev, {"/GTS_PDFX"}, true, false)) out.push_back("matches_pdfx_intent");
  if (blendMatches(x, ev, {"/ISO_PDFE1"}, true, false)) out.push_back("matches_pdfe_intent");
  int want = ci.cls == "gray" ? 1 : ci.cls == "rgb" ? 3 : ci.cls == "cmyk" ? 4 : 0;
  if (device && !ci.hasDefault && n && n == want) out.push_back("matches_blend_space");
  QPDFObjectHandle icc4 = iccStreamOf(ci, 4);
  for (const char* sub : {"/GTS_PDFA1", "/GTS_PDFX"}) {
    bool same = false;
    for (QPDFObjectHandle oi : outputIntents(ev, sub)) same = same || sameStream(icc4, oi.getKey("/DestOutputProfile"));
    if (same) out.push_back(std::string(sub) == "/GTS_PDFA1" ? "cmyk_profile_equals_pdfa_intent" : "cmyk_profile_equals_pdfx_intent");
  }
  if (icc4.isStream() && fam == "ICCBased" && sameStream(icc4, g.getArrayItem(1))) out.push_back("cmyk_icc_equals_blend_space");
  QPDFObjectHandle icc = iccStreamOf(ci, 0);
  if (icc.isStream()) {
    std::string sig = iccSpace(bytesOf(icc));
    QPDFObjectHandle nk = icc.getDict().getKey("/N");
    int nn = nk.isInteger() ? static_cast<int>(nk.getIntValue()) : 0;
    int expect = sig == "GRAY" ? 1 : sig == "RGB " || sig == "Lab " ? 3 : sig == "CMYK" ? 4 : nn;
    if (nn != expect) out.push_back("icc_component_mismatch");
  }
  QPDFObjectHandle attrs = ci.cls == "devicen" && ci.csObj.isArray() && ci.csObj.getArrayNItems() >= 5 ? ci.csObj.getArrayItem(4)
                                                                                                        : QPDFObjectHandle();
  QPDFObjectHandle pr = attrs.isDictionary() && nameIs(attrs.getKey("/Subtype"), "/NChannel") ? attrs.getKey("/Process") : QPDFObjectHandle();
  QPDFObjectHandle pcs = pr.isDictionary() ? pr.getKey("/ColorSpace") : QPDFObjectHandle();
  if (pcs.isArray() && pcs.getArrayNItems() >= 2) {
    std::string pf = bare(pcs.getArrayItem(0));
    QPDFObjectHandle base;
    if (pf == "ICCBased" && pcs.getArrayItem(1).isStream()) {
      QPDFObjectHandle sd = pcs.getArrayItem(1).getDict();
      base = sd.getKey("/Alternate");
      if (base.isNull()) {
        long long nn = sd.getKey("/N").isInteger() ? sd.getKey("/N").getIntValue() : 0;
        base = QPDFObjectHandle::newName(nn == 1 ? "/DeviceGray" : nn == 3 ? "/DeviceRGB" : nn == 4 ? "/DeviceCMYK" : "/");
      }
    } else if ((pf == "Separation" || pf == "DeviceN") && pcs.getArrayNItems() >= 3) {
      base = pcs.getArrayItem(2);
    }
    std::string b = csNickName(base, {});
    if (b == "DeviceGray" || b == "DeviceRGB" || b == "DeviceCMYK") out.push_back("base_space_of_entry");
  }
  return out;
}

std::string loadSettings(const std::string& v) {
  std::error_code ec;
  if (!v.empty() && v.size() < 4096 && v.find('\n') == std::string::npos && std::filesystem::is_regular_file(v, ec)) return readFileBytes(v);
  return v;
}

bool isJobOptions(const EmbFile& e) {
  std::string n = lowerStr(e.name);
  size_t dot = n.rfind('.');
  return dot != std::string::npos && n.substr(dot + 1) == "joboptions";
}

void scanRefs(const unsigned char* d, size_t n, const std::map<QPDFObjGen, QPDFXRefEntry>& xref, std::set<QPDFObjGen>& missing) {
  auto digit = [&](size_t k) { return k < n && d[k] >= '0' && d[k] <= '9'; };
  auto space = [&](size_t k) { return k < n && (d[k] == ' ' || d[k] == '\r' || d[k] == '\n' || d[k] == '\t'); };
  for (size_t i = 1; i < n && missing.size() < 1000; ++i) {
    if (d[i] != 'R' || (i + 1 < n && std::isalnum(d[i + 1])) || !space(i - 1)) continue;
    size_t k = i - 1;
    while (k > 0 && space(k)) --k;
    size_t ge = k + 1;
    while (k > 0 && digit(k)) --k;
    if (!digit(k + 1) || !space(k)) continue;
    size_t gb = k + 1;
    while (k > 0 && space(k)) --k;
    size_t oe = k + 1;
    while (k > 0 && digit(k)) --k;
    size_t ob = digit(k) ? k : k + 1;
    if (ob >= oe || (ob > 0 && std::isalnum(d[ob - 1]))) continue;
    long long obj = std::atoll(std::string(reinterpret_cast<const char*>(d + ob), oe - ob).c_str());
    long long gen = std::atoll(std::string(reinterpret_cast<const char*>(d + gb), ge - gb).c_str());
    if (obj > 0 && obj < INT_MAX && gen < 65536 && !xref.count(QPDFObjGen(static_cast<int>(obj), static_cast<int>(gen)))) {
      missing.insert(QPDFObjGen(static_cast<int>(obj), static_cast<int>(gen)));
    }
  }
}

std::vector<std::string> broken(const Events& ev, bool& supported) {
  std::vector<std::string> out;
  QPDF* q = ev.root.isDictionary() ? ev.root.getOwningQPDF() : nullptr;
  if (!q) return out;
  std::map<QPDFObjGen, QPDFXRefEntry> xref = q->getXRefTable();
  std::set<QPDFObjGen> missing;
  std::set<int> objStreams;
  const unsigned char* d = ev.inputData;
  size_t n = d ? ev.inputSize : 0;
  static const char kStream[] = "stream", kEndObj[] = "endobj";
  for (const auto& kv : xref) {
    if (kv.second.getType() == 2) {
      objStreams.insert(kv.second.getObjStreamNumber());
      continue;
    }
    if (kv.second.getType() != 1 || kv.second.getOffset() < 0 || static_cast<size_t>(kv.second.getOffset()) >= n) continue;
    const unsigned char* b = d + kv.second.getOffset();
    const unsigned char* e = std::search(b, d + n, kEndObj, kEndObj + 6);
    const unsigned char* st = std::search(b, e, kStream, kStream + 6);
    scanRefs(b, static_cast<size_t>(st - b), xref, missing);
  }
  for (int os : objStreams) {
    std::string body = bytesOf(q->getObjectByID(os, 0));
    scanRefs(reinterpret_cast<const unsigned char*>(body.data()), body.size(), xref, missing);
  }
  for (size_t i = 0; i < missing.size(); ++i) out.push_back("object");
  for (QPDFObjectHandle o : q->getAllObjects()) {
    if (!o.isStream() || !nameIs(o.getDict().getKey("/Subtype"), "/Form")) continue;
    QPDFObjectHandle ref = o.getDict().getKey("/Ref");
    if (!ref.isDictionary()) continue;
    QPDFObjectHandle fs = ref.getKey("/F");
    bool found = fs.isDictionary() && fs.getKey("/EF").isDictionary();
    if (!found) {
      if (ev.inputPath.empty()) {
        supported = false;
        return out;
      }
      found = !resolveRef(ev, fsUrl(fs)).empty();
    }
    if (!found) out.push_back("external_file");
  }
  return out;
}

std::vector<std::string> multimediaProblems(const AnnotFacts& an) {
  std::vector<std::string> out;
  QPDFObjectHandle d = an.obj;
  if (!d.isDictionary()) return out;
  auto add = [&](const char* v) {
    if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
  };
  QPDFObjectHandle dd = d.getKey("/3DD");
  if (dd.isDictionary() && dd.getKey("/3DD").isStream()) dd = dd.getKey("/3DD");
  if (dd.isStream()) {
    QPDFObjectHandle sd = dd.getDict();
    QPDFObjectHandle cs = sd.getKey("/ColorSpace");
    if (!cs.isNull()) add("3d_colour_space");
    if (nameIs(cs, "/DeviceRGB") || (cs.isArray() && cs.getArrayNItems() > 0 && nameIs(cs.getArrayItem(0), "/DeviceRGB"))) add("3d_device_rgb");
    if (sd.hasKey("/Resources")) add("3d_stream_resources");
    if (!nameIs(sd.getKey("/Subtype"), "/U3D") && !nameIs(sd.getKey("/Subtype"), "/PRC")) add("3d_stream_subtype");
  }
  std::set<QPDFObjGen> seen;
  std::function<void(QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle o, int depth) {
    if (depth > 12) return;
    if (o.isIndirect() && !seen.insert(o.getObjGen()).second) return;
    if (o.isStream()) return;
    if (o.isArray()) {
      for (int i = 0; i < o.getArrayNItems(); ++i) walk(o.getArrayItem(i), depth + 1);
      return;
    }
    if (!o.isDictionary()) return;
    std::string t = nameOf(o.getKey("/Type"));
    if (nameIs(o.getKey("/S"), "/MCD") || (t == "/MediaClip" && !nameIs(o.getKey("/S"), "/MCS"))) {
      if (!o.hasKey("/Alt")) add("media_clip_missing_alt");
      if (!o.hasKey("/CT")) add("media_clip_missing_type");
    }
    if (o.getKey("/MH").isDictionary() && (t == "/Rendition" || t == "/MediaClip" || t == "/MediaPlayParams" ||
                                           t == "/MediaScreenParams" || nameIs(o.getKey("/S"), "/MR") ||
                                           nameIs(o.getKey("/S"), "/SR") || nameIs(o.getKey("/S"), "/MCD") ||
                                           nameIs(o.getKey("/S"), "/MCS"))) {
      add("media_has_mh");
    }
    for (const std::string& k : o.getKeys()) {
      if (k != "/P" && k != "/Parent" && k != "/Pg") walk(o.getKey(k), depth + 1);
    }
  };
  walk(d.getKey("/A"), 0);
  walk(d.getKey("/AA"), 0);
  return out;
}

std::string standardLevel(const std::string& s) {
  static const std::map<std::string, std::string> m = {
      {"pdfa-1a", "1a"},  {"pdfa-1b", "1b"},   {"pdfa-2a", "2a"},  {"pdfa-2b", "2b"},  {"pdfa-2u", "2u"},
      {"pdfa-3a", "3a"},  {"pdfa-3b", "3b"},   {"pdfa-3u", "3u"},  {"pdfa-4", "4"},    {"pdfa-4e", "4e"},
      {"pdfa-4f", "4f"},  {"pdfx-1a", "x1a"},  {"pdfx-3", "x3"},   {"pdfx-4", "x4"},   {"pdfx-4p", "x4p"},
      {"pdfx-5g", "x5g"}, {"pdfx-5n", "x5n"},  {"pdfx-5pg", "x5pg"}, {"pdfx-6", "x6"}, {"pdfx-6n", "x6n"},
      {"pdfx-6p", "x6p"}, {"pdfe-1", "e1"},    {"pdfvt-1", "vt1"}, {"pdfvt-2", "vt2"}, {"pdfvt-3", "vt3"}};
  auto it = m.find(s);
  return it == m.end() ? std::string() : it->second;
}

long long violations(const unsigned char* data, size_t size, const std::string& standard, bool& ok) {
  ok = false;
  if (standard == "pdfua-1") {
    try {
      long long n = static_cast<long long>(uaCheck(data, size, "").size());
      ok = true;
      return n;
    } catch (...) {
      return 1;
    }
  }
  Level lv;
  if (!levelFromString(standardLevel(standard), lv)) return 1;
  Options o;
  o.level = lv;
  o.verifyOnly = true;
  Result r = convert(data, size, o);
  if (!r.ok) return 1;
  ok = true;
  if (r.compliant) return 0;
  long long n = 0;
  for (const Issue& i : r.issues) n += i.fixed && !issueIsNormalization(i.code);
  return std::max<long long>(n, 1);
}

std::vector<std::string> embeddedPdfs(const Events& ev) {
  std::vector<std::string> out;
  for (const EmbFile& e : embeddedFiles(ev)) {
    if (!e.ef.isStream() || out.size() >= 16) continue;
    std::string b = bytesOf(e.ef);
    if (b.compare(0, 4, "%PDF") == 0) out.push_back(std::move(b));
  }
  return out;
}

int evalTrigger(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string& on = param(a, "on");
  std::vector<std::string> names;
  auto add = [&](QPDFObjectHandle d) {
    QPDFObjectHandle aa = d.isDictionary() ? d.getKey("/AA") : QPDFObjectHandle();
    if (!aa.isDictionary()) return;
    for (const std::string& k : aa.getKeys()) names.push_back(k.substr(1));
  };
  if (on == "any" || on == "document") add(ev.root);
  if (on == "any" || on == "page") {
    for (const PageFacts& p : ev.pages) add(p.obj);
  }
  if (on == "any" || on == "annotation") {
    for (const AnnotFacts& an : ev.annots) {
      if (an.subtype != "Widget") add(an.obj);
    }
  }
  if (on == "any" || on == "form_field") {
    QPDFObjectHandle af = ev.root.isDictionary() ? ev.root.getKey("/AcroForm") : QPDFObjectHandle();
    std::set<QPDFObjGen> seen;
    std::function<void(QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle fl, int depth) {
      if (!fl.isDictionary() || depth > 32) return;
      if (fl.isIndirect() && !seen.insert(fl.getObjGen()).second) return;
      add(fl);
      QPDFObjectHandle kids = fl.getKey("/Kids");
      for (int i = 0; kids.isArray() && i < kids.getArrayNItems(); ++i) walk(kids.getArrayItem(i), depth + 1);
    };
    QPDFObjectHandle fields = af.isDictionary() ? af.getKey("/Fields") : QPDFObjectHandle();
    for (int i = 0; fields.isArray() && i < fields.getArrayNItems(); ++i) walk(fields.getArrayItem(i), 0);
  }
  return compare(Value(names), a);
}

int evalDeviceSpaceRule(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  const ColorInfo* ci = colorOf(s);
  const GsExtra* x = gsOf(s);
  if (!ci || !x) return 0;
  return compare(Value(deviceSpaceRules(*ci, *x, ev)), a);
}

int evalUnusedResource(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  ExtFacts& f = facts(ev);
  std::vector<std::string> v;
  if (f.refUnusedGray) v.push_back("DeviceGray");
  if (f.refUnusedRGB) v.push_back("DeviceRGB");
  if (f.refUnusedCMYK) v.push_back("DeviceCMYK");
  if (f.refUnusedBM) v.push_back("blend_mode");
  return compare(Value(v), a);
}

int evalJobOptions(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string& cmp = param(a, "compare_to");
  std::string want = cmp.empty() ? std::string() : loadSettings(cmp);
  std::vector<std::string> v;
  for (const EmbFile& e : embeddedFiles(ev)) {
    if (!isJobOptions(e)) continue;
    std::string body = e.ef.isStream() ? bytesOf(e.ef) : std::string();
    v.push_back(cmp.empty() ? body : body == want ? "same" : "different");
  }
  if (v.empty()) return 0;
  return compare(Value(v), a);
}

std::string wrapConditions(const std::string& json, size_t b) {
  if (json[b] == '[') return "{\"kura-profile\":1,\"checks\":[{\"name\":\"script\",\"all\":" + json + "}]}";
  if (json.find("\"checks\"") != std::string::npos) return json;
  if (json.find("\"check\"") != std::string::npos && json.find("\"all\"") == std::string::npos &&
      json.find("\"any\"") == std::string::npos) {
    return "{\"kura-profile\":1,\"checks\":[{\"name\":\"script\",\"all\":[" + json + "]}]}";
  }
  return "{\"kura-profile\":1,\"checks\":[" + json.substr(0, json.rfind('}')) + ",\"name\":\"script\"}]}";
}

int evalScriptCheck(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  const std::string& src = param(a, "script");
  size_t b = src.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return 0;
  std::string json = src;
  if (src[b] != '{' && src[b] != '[') {
    QPDF* q = ev.root.isDictionary() ? ev.root.getOwningQPDF() : nullptr;
    std::map<std::string, std::string> vars;
    std::string err;
    if (!q || !evalScript(src, docFactsJson(*q), vars, json, err)) {
      supported = false;
      return 0;
    }
    size_t jb = json.find_first_not_of(" \t\r\n");
    if (jb == std::string::npos) return compare(Value(false), a);
    if (json[jb] != '{' && json[jb] != '[') {
      std::string l = lowerStr(json.substr(jb, json.find_last_not_of(" \t\r\n") + 1 - jb));
      return compare(Value(l == "true" || l == "1"), a);
    }
    b = jb;
  }
  PfProfile prof;
  static thread_local int nesting = 0;
  if (nesting > 4 || !parseKuraJson(wrapConditions(json, b), prof) || prof.rules.empty()) {
    supported = false;
    return 0;
  }
  struct Nest {
    Nest() { ++nesting; }
    ~Nest() { --nesting; }
  } nest;
  Subject doc = subjectFor(Domain::kDoc, nullptr, 0, ev);
  bool hit = false;
  for (const PfRule& r : prof.rules) {
    bool combined = r.logic != 1;
    for (const std::string& cid : r.condIds) {
      auto it = prof.conds.find(cid);
      if (it == prof.conds.end()) continue;
      bool c = true;
      for (const PfAtom& at : it->second.atoms) {
        if (domainOf(at) != Domain::kDoc) {
          supported = false;
          return 0;
        }
        bool sup = true;
        bool v = evalCheck(at, doc, ev, sup) > 0;
        if (!sup) {
          supported = false;
          return 0;
        }
        c = c && v;
      }
      combined = r.logic == 1 ? (combined || c) : (combined && c);
    }
    hit = hit || combined;
  }
  return compare(Value(hit), a);
}

std::vector<EmbFile> invoiceFiles(const Events& ev) {
  std::vector<EmbFile> out;
  for (const EmbFile& e : embeddedFiles(ev)) {
    if (isInvoiceName(e.name)) out.push_back(e);
  }
  return out;
}

int evalInvoiceRelationship(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::vector<EmbFile> inv = invoiceFiles(ev);
  if (inv.empty()) return 0;
  bool ok = true;
  for (const EmbFile& e : inv) {
    std::string rel = bare(e.fs.getKey("/AFRelationship"));
    ok = ok && (rel == "Alternative" || rel == "Data" || rel == "Source");
  }
  return compare(Value(ok), a);
}

int evalInvoiceValid(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  int hits = 0;
  for (const EmbFile& e : invoiceFiles(ev)) {
    std::string xml = e.ef.isStream() ? bytesOf(e.ef) : std::string();
    std::string ns, local;
    bool valid = xmlRootOf(xml, ns, local) && detectInvoice(xml, "", e.name).rootKnown;
    hits += compare(Value(valid), a);
  }
  return hits;
}

int evalLayerMembership(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  bool known = true;
  const MarkedState* st = markedStateOf(s, ev, known);
  if (!known) {
    supported = false;
    return 0;
  }
  std::vector<std::string> layers = st ? st->layers : std::vector<std::string>();
  if (s.image && s.image->obj.isStream()) {
    for (const std::string& n : ocgNames(s.image->obj.getDict().getKey("/OC"))) {
      if (std::find(layers.begin(), layers.end(), n) == layers.end()) layers.push_back(n);
    }
  }
  return compare(Value(layers), a);
}

int evalColourExchange(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string& prop = param(a, "property");
  std::vector<QPDFObjectHandle> xois = outputIntents(ev, "/GTS_PDFX");
  QPDFObjectHandle oi = xois.empty() ? QPDFObjectHandle() : xois.front();
  if (prop == "conformance") {
    std::vector<std::string> v;
    bool specific = false;
    for (const std::string& c : ev.cxfLevels) specific = specific || c != "CxF/X-4";
    for (const std::string& c : ev.cxfLevels) {
      if (specific && c == "CxF/X-4") continue;
      std::string k = c == "CxF/X-4a" ? "cxf-x-4a" : c == "CxF/X-4b" ? "cxf-x-4b" : "cxf-x-4";
      if (std::find(v.begin(), v.end(), k) == v.end()) v.push_back(k);
    }
    return v.empty() ? 0 : compare(Value(v), a);
  }
  if (prop == "mixing_hints_present") {
    return compare(Value(std::string(oi.isDictionary() && oi.getKey("/MixingHints").isDictionary() ? "true" : "false")), a);
  }
  QPDFObjectHandle sd = oi.isDictionary() ? oi.getKey("/SpectralData") : QPDFObjectHandle();
  std::set<std::string> cxf;
  if (sd.isDictionary()) {
    for (const std::string& k : sd.getKeys()) cxf.insert(k.substr(1));
  }
  std::string sp = iccSpace(oi.isDictionary() ? bytesOf(oi.getKey("/DestOutputProfile")) : "");
  std::vector<std::string> proc = processColorants(sp);
  if (prop == "has_entry_for_colour") return compare(Value(std::vector<std::string>(cxf.begin(), cxf.end())), a);
  if (prop == "valid") {
    bool any = false, ok = true;
    for (const std::string& k : sd.isDictionary() ? sd.getKeys() : std::set<std::string>()) {
      QPDFObjectHandle e = sd.getKey(k);
      if (!e.isStream()) continue;
      any = true;
      std::string ns, local;
      ok = ok && xmlRootOf(bytesOf(e), ns, local);
    }
    return any ? compare(Value(std::string(ok ? "true" : "false")), a) : 0;
  }
  if (prop == "stray_entries") {
    std::set<std::string> spots(ev.spotPlates.begin(), ev.spotPlates.end());
    int n = 0;
    for (const std::string& c : cxf) n += !spots.count(c) && std::find(proc.begin(), proc.end(), c) == proc.end();
    return compare(Value(std::to_string(n)), a);
  }
  if (prop == "in_data_and_mixing_hints") {
    QPDFObjectHandle mh = oi.isDictionary() ? oi.getKey("/MixingHints") : QPDFObjectHandle();
    QPDFObjectHandle sol = mh.isDictionary() ? mh.getKey("/Solidities") : QPDFObjectHandle();
    bool both = false;
    for (const std::string& c : cxf) both = both || (sol.isDictionary() && sol.hasKey("/" + c));
    return compare(Value(std::string(both ? "true" : "false")), a);
  }
  if (!sd.isDictionary()) return 0;
  bool procOnly = prop == "process_colours_without_entry";
  auto count = [&](const ColorInfo& ci, const std::vector<double>& comps) {
    int n = 0;
    if (!procOnly && (ci.cls == "separation" || ci.cls == "devicen")) {
      for (const std::string& c : ci.colorants) n += c != "All" && c != "None" && !cxf.count(c);
    }
    static const char* names[4] = {"Cyan", "Magenta", "Yellow", "Black"};
    bool used[4] = {false, false, false, false};
    if (ci.cls == "cmyk") {
      for (int i = 0; i < 4; ++i) used[i] = comps.size() == 4 ? comps[i] > 0 : true;
    } else if (ci.cls == "gray") {
      used[3] = true;
    }
    if (procOnly && !used[0] && !used[1] && !used[2] && !used[3]) return -2;
    if (sp == "CMYK") {
      for (int i = 0; i < 4; ++i) n += used[i] && !cxf.count(names[i]);
    } else if (sp == "GRAY") {
      n += used[3] && !cxf.count(names[3]);
    } else {
      return -1;
    }
    return n;
  };
  std::vector<std::string> v;
  auto put = [&](int n) {
    if (n >= 0) v.push_back(std::to_string(n));
  };
  for (const PaintEvent& e : ev.paints) put(count(e.color, e.comps));
  for (const TextEvent& e : ev.texts) put(count(e.color, e.comps));
  for (const ImageEvent& e : ev.images) put(count(e.color, {}));
  return compare(Value(v), a);
}

int evalMixingHints(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::vector<std::string> v;
  if (mixHintsConsistent(ev, false, false) == 0) v.push_back("printing_order_inconsistent");
  if (mixHintsConsistent(ev, true, false) == 0) v.push_back("solidities_inconsistent");
  bool noOrder = false, noSol = false;
  for (QPDFObjectHandle cs : devicenSpaces(ev)) {
    QPDFObjectHandle mh = mixingHints(cs);
    if (!mh.isDictionary()) continue;
    noOrder = noOrder || !mh.getKey("/PrintingOrder").isArray();
    noSol = noSol || !mh.getKey("/Solidities").isDictionary();
  }
  if (noOrder) v.push_back("printing_order_missing");
  if (noSol) v.push_back("solidities_missing");
  return compare(Value(v), a);
}

int evalEvery(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  const std::string& level = param(a, "level");
  size_t n = 1;
  bool onItem = (level == "page" && s.kind == Domain::kPage) || (level == "annotation" && s.kind == Domain::kAnnot) ||
                (level == "object" && (s.paint || s.text || s.image));
  if (!onItem) {
    if (level == "page") n = ev.pages.size();
    else if (level == "object") n = ev.paints.size() + ev.texts.size() + ev.images.size();
    else if (level == "annotation") n = ev.annots.size();
  }
  return compare(Value(true), a) ? static_cast<int>(std::min<size_t>(n, 1000000)) : 0;
}

int evalBrokenReference(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  std::vector<std::string> v = broken(ev, supported);
  if (!supported) return 0;
  return compare(Value(v), a);
}

int evalDictionaryError(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string& only = param(a, "object_type");
  std::vector<std::string> v;
  for (const auto& p : dictionaryProblems(ev)) {
    if (only.empty() || p.second == only) v.push_back(p.first);
  }
  return compare(Value(v), a);
}

int evalLinearizationId(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  if (!ev.inputData) {
    supported = false;
    return 0;
  }
  return compare(Value(linIdMismatch(ev)), a);
}

int evalUnicodeConflict(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.text) return 0;
  return compare(Value(unicodeConflicts(*s.text, ev)), a);
}

int evalEncapsulated(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  const GsExtra* x = gsOf(s);
  if (!x) return 0;
  ExtFacts& f = facts(ev);
  std::vector<std::string> v;
  QPDFObjectHandle enc = x->inForm && x->formDict.isDictionary() ? x->formDict.getKey("/GTS_Encapsulated") : QPDFObjectHandle();
  QPDFObjectHandle ienc;
  if (s.image && !s.image->inlineImg && s.image->obj.isStream()) ienc = s.image->obj.getDict().getKey("/GTS_Encapsulated");
  bool formTrue = enc.isBool() && enc.getBoolValue();
  if (enc.isBool() || ienc.isBool()) v.push_back("marked_encapsulated");
  if (formTrue || (ienc.isBool() && ienc.getBoolValue())) v.push_back("marked_true");
  if (ienc.isBool()) v.push_back("image_encapsulated");
  if (formTrue) v.push_back("inside_encapsulated");
  QPDFObjGen form = formOf(ev, *x);
  bool independent = x->inForm && form.getObj() && !f.formsDependent.count(form);
  if (formTrue && independent && x->formDict.getKey("/Resources").isDictionary()) v.push_back("all_independent");
  QPDFObjGen xo = s.image && s.image->obj.isIndirect() ? s.image->obj.getObjGen() : form;
  auto it = f.xobjPages.find(xo);
  if (xo.getObj() && it != f.xobjPages.end()) {
    std::set<int> recs;
    for (int pg : it->second) {
      auto r = f.pageRecord.find(pg);
      if (r != f.pageRecord.end()) recs.insert(r->second);
    }
    if (recs.size() > 1) v.push_back("used_by_several_records");
  }
  if (independent) v.push_back("graphics_state_set_before_use");
  return compare(Value(v), a);
}

int evalXObjectUses(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  const GsExtra* x = gsOf(s);
  if (!x) return 0;
  ExtFacts& f = facts(ev);
  QPDFObjGen xo = s.image && s.image->obj.isIndirect() ? s.image->obj.getObjGen() : formOf(ev, *x);
  auto it = f.doCount.find(xo);
  if (!xo.getObj() || it == f.doCount.end()) return 0;
  return compare(Value(it->second), a);
}

int evalMultimedia(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.annot) return 0;
  return compare(Value(multimediaProblems(*s.annot)), a);
}

int evalConformance(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  const std::string& standard = param(a, "standard");
  if (paramBool(a, "embedded_files")) {
    int hits = 0;
    for (const std::string& pdf : embeddedPdfs(ev)) {
      bool ok = false;
      long long n = violations(reinterpret_cast<const unsigned char*>(pdf.data()), pdf.size(), standard, ok);
      hits += compare(Value(ok && n == 0), a);
    }
    return hits;
  }
  if (!ev.inputData || !ev.inputSize) {
    supported = false;
    return 0;
  }
  bool ok = false;
  long long n = violations(ev.inputData, ev.inputSize, standard, ok);
  bool meets = ok && n == 0;
  int r = compare(Value(meets), a);
  return r && !meets ? static_cast<int>(std::min<long long>(std::max<long long>(n, 1), 1000000)) : r;
}

int evalEncryption(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDF probe;
  QPDF* q = ev.root.isDictionary() ? ev.root.getOwningQPDF() : nullptr;
  std::string v = "none";
  if (ev.inputData && ev.inputSize) {
    try {
      probe.setSuppressWarnings(true);
      probe.processMemoryFile("probe", reinterpret_cast<const char*>(ev.inputData), ev.inputSize);
      q = &probe;
    } catch (...) {
      q = nullptr;
      std::string raw(reinterpret_cast<const char*>(ev.inputData), ev.inputSize);
      if (raw.find("/Encrypt") != std::string::npos) v = "other";
    }
  }
  if (q) {
    int R = 0, P = 0, V = 0;
    QPDF::encryption_method_e sm = QPDF::e_none, tm = QPDF::e_none, fm = QPDF::e_none;
    if (q->isEncrypted(R, P, V, sm, tm, fm)) {
      QPDF::encryption_method_e m = sm != QPDF::e_none ? sm : tm;
      v = m == QPDF::e_rc4 ? "rc4" : m == QPDF::e_aes ? "aes128" : m == QPDF::e_aesv3 ? "aes256" : "other";
    }
  }
  return compare(Value(v), a);
}

int evalSchemaFile(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::vector<std::string> v;
  for (const auto& m : metadataPackets(ev, param(a, "scope"))) {
    std::vector<QPDFObjectHandle> sf = schemaFiles(m.first);
    bool bad = false;
    for (QPDFObjectHandle fs : sf) bad = bad || !relaxNgValid(fs);
    v.push_back(sf.empty() ? "absent" : bad ? "invalid" : "valid");
  }
  return compare(Value(v), a);
}
}

bool xmlRootOf(const std::string& xml, std::string& ns, std::string& local);

double barWidthReduction(const std::vector<int>& runs, bool firstDark, double& modulePx) {
  modulePx = 0;
  std::vector<int> r = runs;
  if (!firstDark && !r.empty()) r.erase(r.begin());
  if (r.size() % 2 == 0 && !r.empty()) r.pop_back();
  if (r.size() < 5) return 0;
  std::vector<double> pairs;
  for (size_t i = 0; i + 1 < r.size(); i += 2) pairs.push_back(r[i] + r[i + 1]);
  int minRun = r[1];
  for (size_t i = 1; i + 1 < r.size(); ++i) minRun = std::min(minRun, r[i]);
  double m = 0, bestErr = 1e18;
  for (double c = 0.8 * minRun; c <= 1.7 * minRun; c += 0.01 * minRun) {
    double err = 0;
    for (double p : pairs) {
      double q = p / c;
      double k = std::max(2.0, std::round(q));
      err += (q - k) * (q - k);
    }
    if (err < bestErr - 1e-9) {
      bestErr = err;
      m = c;
    }
  }
  for (int it = 0; it < 4 && m > 0; ++it) {
    double num = 0, den = 0;
    for (double p : pairs) {
      num += p;
      den += std::max(2.0, std::round(p / m));
    }
    if (den > 0) m = num / den;
  }
  if (m <= 0) return 0;
  double red = 0;
  for (int it = 0; it < 6; ++it) {
    double sum = 0;
    int n = 0;
    for (size_t i = 0; i < r.size(); ++i) {
      bool dark = i % 2 == 0;
      double k = std::max(1.0, std::round((r[i] + (dark ? red : -red)) / m));
      sum += dark ? (k * m - r[i]) : (r[i] - k * m);
      ++n;
    }
    red = n ? sum / n : 0;
  }
  modulePx = m;
  return red;
}

std::vector<std::pair<QPDFObjectHandle, std::string>> metadataPackets(const Events& ev, const std::string& scope) {
  std::vector<std::pair<QPDFObjectHandle, std::string>> out;
  if (!ev.root.isDictionary()) return out;
  QPDFObjectHandle md = ev.root.getKey("/Metadata");
  if ((scope == "document" || scope == "any") && md.isStream()) out.push_back({md, bytesOf(md)});
  if (scope == "document") return out;
  if (scope == "page") {
    for (const PageFacts& p : ev.pages) {
      QPDFObjectHandle pm = p.obj.isDictionary() ? p.obj.getKey("/Metadata") : QPDFObjectHandle();
      if (pm.isStream()) out.push_back({pm, bytesOf(pm)});
    }
    return out;
  }
  QPDF* q = ev.root.getOwningQPDF();
  if (!q) return out;
  for (QPDFObjectHandle o : q->getAllObjects()) {
    QPDFObjectHandle d = o.isStream() ? o.getDict() : o;
    if (!d.isDictionary() || nameIs(d.getKey("/Type"), "/Catalog") || !d.getKey("/Metadata").isStream()) continue;
    out.push_back({d.getKey("/Metadata"), bytesOf(d.getKey("/Metadata"))});
  }
  return out;
}

std::vector<QPDFObjectHandle> ocgsOf(QPDFObjectHandle cond) {
  std::vector<QPDFObjectHandle> out;
  std::function<void(QPDFObjectHandle, int)> add = [&](QPDFObjectHandle c, int d) {
    if (!c.isDictionary() || d > 16) return;
    if (nameIs(c.getKey("/Type"), "/OCG")) {
      for (QPDFObjectHandle o : out) {
        if (o.isSameObjectAs(c)) return;
      }
      out.push_back(c);
      return;
    }
    QPDFObjectHandle g = c.getKey("/OCGs");
    if (g.isDictionary()) add(g, d + 1);
    for (int i = 0; g.isArray() && i < g.getArrayNItems(); ++i) add(g.getArrayItem(i), d + 1);
  };
  add(cond, 0);
  return out;
}

bool objectOcConds(const Subject& s, const Events& ev, std::vector<QPDFObjectHandle>& out) {
  bool known = true;
  const MarkedState* st = markedStateOf(s, ev, known);
  if (!known) return false;
  out = st ? st->ocConds : std::vector<QPDFObjectHandle>();
  if (s.image && s.image->obj.isStream() && s.image->obj.getDict().getKey("/OC").isDictionary()) {
    out.push_back(s.image->obj.getDict().getKey("/OC"));
  }
  return true;
}

const MarkedState* markedStateOf(const Subject& s, const Events& ev, bool& known) {
  const std::vector<size_t>* src = srcOf(s);
  known = src && !src->empty();
  if (!known) return nullptr;
  if (std::find(src->begin(), src->end(), SIZE_MAX) != src->end()) return nullptr;
  ExtFacts& f = facts(ev);
  std::vector<size_t> key{static_cast<size_t>(s.page)};
  key.insert(key.end(), src->begin(), src->end() - 1);
  auto it = f.mcIndex.find(key);
  if (it == f.mcIndex.end() || it->second.empty()) return nullptr;
  size_t off = src->back();
  auto pos = std::upper_bound(it->second.begin(), it->second.end(), std::make_pair(off, INT_MAX));
  if (pos == it->second.begin()) return nullptr;
  return &f.mcStates[static_cast<size_t>(std::prev(pos)->second)];
}

std::shared_ptr<void>& extTaggingSlot(const Events& ev) { return cacheOf(ev).tagging; }

const std::map<int, std::map<long long, int>>& pageMarkedContentIds(const Events& ev) { return facts(ev).mcidCount; }

const std::map<int, std::set<long long>>& pageTextMarkedContentIds(const Events& ev) { return facts(ev).mcidText; }

const std::set<int>& pagesWithUntaggedText(const Events& ev) { return facts(ev).untaggedText; }

std::vector<std::string> fontProgramProblems(const FontFacts& f, const Events& ev, bool strict) {
  std::vector<std::string> out;
  QPDFObjectHandle desc = descOf(f);
  QPDFObjectHandle enc = f.dict.getKey("/Encoding");
  QPDFObjectHandle diffs = enc.isDictionary() ? enc.getKey("/Differences") : QPDFObjectHandle();
  if (!f.cid && diffs.isArray()) {
    for (int i = 0; i < diffs.getArrayNItems(); ++i) {
      QPDFObjectHandle it = diffs.getArrayItem(i);
      if (it.isName() && it.getName() != "/.notdef" && !aglNameToUnicode(it.getName().substr(1))) {
        out.push_back("glyph_names_not_in_standard_list");
        break;
      }
    }
  }
  bool t1 = !f.cid && (f.subtype == "Type1" || f.subtype == "MMType1");
  if (f.cid && f.subsetName && f.embedded && !(desc.isDictionary() && desc.getKey("/CIDSet").isStream())) out.push_back("cidset_missing");
  if (t1 && f.subsetName && f.embedded && !(desc.isDictionary() && desc.getKey("/CharSet").isString())) out.push_back("charset_missing");
  QPDFObjectHandle use = enc.isStream() ? enc.getDict().getKey("/UseCMap") : QPDFObjectHandle();
  if (use.isName() && !predefinedCMaps().count(use.getName().substr(1))) out.push_back("cmap_references_unknown");
  if (enc.isStream()) {
    QPDFObjectHandle d = enc.getDict();
    bool ok = d.getKey("/CMapName").isName() && d.getKey("/CIDSystemInfo").isDictionary() &&
              (!d.hasKey("/Type") || nameIs(d.getKey("/Type"), "/CMap")) && cmapSyntaxOk(bytesOf(enc), true) &&
              (!d.hasKey("/WMode") || d.getKey("/WMode").isInteger());
    if (!ok) out.push_back("cmap_bad_syntax");
  }
  QPDFObjectHandle tu = f.dict.getKey("/ToUnicode");
  if (tu.isStream() && !cmapSyntaxOk(bytesOf(tu), false)) out.push_back("to_unicode_bad_syntax");
  if (f.type3) {
    std::map<std::string, std::set<std::string>> used;
    QPDFObjectHandle cps = f.dict.getKey("/CharProcs");
    for (const std::string& k : cps.isDictionary() ? cps.getKeys() : std::set<std::string>()) {
      if (cps.getKey(k).isStream()) type3Names(cps.getKey(k), used);
    }
    QPDFObjectHandle res = f.dict.getKey("/Resources");
    bool ok = true;
    for (const auto& kv : used) {
      for (const std::string& n : kv.second) {
        QPDFObjectHandle cat = res.isDictionary() ? res.getKey(kv.first) : QPDFObjectHandle();
        if (!cat.isDictionary() || !cat.hasKey(n)) ok = false;
      }
    }
    if (!ok) out.push_back("type3_missing_resources");
  }
  if (f.embedded && f.anyUndefinedGlyph) out.push_back("missing_glyph");
  if (f.embedded && !f.type3 && !programErrors(f).empty()) out.push_back("truetype_errors");
  FtLib lib;
  FtFace face;
  bool loaded = f.embedded && !f.type3 && faceFor(f, lib, face);
  if (strict && f.embedded && !f.type3) {
    bool bad = !loaded;
    if (loaded && FT_IS_SFNT(face.face) && !f.openType && f.trueType) {
      for (const char* tag : {"head", "hhea", "hmtx", "loca", "glyf", "maxp"}) {
        FT_ULong len = 0;
        FT_ULong tg = FT_MAKE_TAG(tag[0], tag[1], tag[2], tag[3]);
        if (FT_Load_Sfnt_Table(face.face, tg, 0, nullptr, &len) != 0 || len == 0) bad = true;
      }
    }
    for (FT_Long g = 0; loaded && g < face.face->num_glyphs && g < 6000 && !bad; ++g) {
      if (FT_Load_Glyph(face.face, static_cast<FT_UInt>(g), FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) bad = true;
    }
    if (bad) out.push_back("invalid_program");
  }
  if (!loaded) return out;
  QPDFObjectHandle cidset = desc.isDictionary() ? desc.getKey("/CIDSet") : QPDFObjectHandle();
  if (f.cid && f.subsetName && cidset.isStream()) {
    std::string bits = bytesOf(cidset);
    auto has = [&](uint32_t cid) { return cid / 8 < bits.size() && (static_cast<unsigned char>(bits[cid / 8]) & (0x80 >> (cid % 8))); };
    bool ok = true;
    QPDFObjectHandle cf = cidFontOf(f);
    QPDFObjectHandle map = cf.isDictionary() ? cf.getKey("/CIDToGIDMap") : QPDFObjectHandle();
    if (f.cid0 && FT_IS_CID_KEYED(face.face)) {
      for (FT_Long g = 0; g < face.face->num_glyphs && ok; ++g) {
        FT_UInt cid = 0;
        if (FT_Get_CID_From_Glyph_Index(face.face, static_cast<FT_UInt>(g), &cid) == 0 && !has(cid)) ok = false;
      }
    } else if (map.isStream()) {
      std::string m = bytesOf(map);
      for (size_t c = 0; 2 * c + 1 < m.size() && ok; ++c) {
        unsigned gid = (static_cast<unsigned char>(m[2 * c]) << 8) | static_cast<unsigned char>(m[2 * c + 1]);
        if ((gid != 0 || c == 0) && gid < static_cast<unsigned>(face.face->num_glyphs) && !has(static_cast<uint32_t>(c))) ok = false;
      }
    } else {
      for (FT_Long g = 0; g < face.face->num_glyphs && ok; ++g) ok = has(static_cast<uint32_t>(g));
    }
    if (!ok) out.push_back("cidset_incomplete");
  }
  QPDFObjectHandle cs = desc.isDictionary() ? desc.getKey("/CharSet") : QPDFObjectHandle();
  if (t1 && f.subsetName && cs.isString() && FT_HAS_GLYPH_NAMES(face.face)) {
    std::set<std::string> listed, present;
    std::string s = cs.getStringValue();
    for (size_t i = 0; i < s.size();) {
      if (s[i] != '/') {
        ++i;
        continue;
      }
      size_t e = s.find_first_of("/ \t\r\n", i + 1);
      listed.insert(s.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1));
      i = e == std::string::npos ? s.size() : e;
    }
    for (FT_Long g = 0; g < face.face->num_glyphs; ++g) {
      char buf[128] = {0};
      if (FT_Get_Glyph_Name(face.face, static_cast<FT_UInt>(g), buf, sizeof buf) == 0 && buf[0]) present.insert(buf);
    }
    listed.erase(".notdef");
    present.erase(".notdef");
    if (listed != present) out.push_back("charset_incomplete");
  }
  SimpleEncoding se = readEncoding(f.dict, f.symbolic);
  std::vector<uint32_t> codes = usedCodes(f, ev);
  std::set<FT_UInt> gids;
  for (uint32_t c : codes) gids.insert(gidForCode(f, face.face, c, se));
  gids.erase(0);
  for (FT_UInt g : gids) {
    if (glyphOverlaps(face.face, g)) {
      out.push_back("glyph_overlapping_paths");
      break;
    }
  }
  const auto& tum = toUnicodeMap(f, ev);
  std::map<std::string, std::set<std::u32string>> byOutline;
  for (uint32_t c : codes) {
    FT_UInt g = gidForCode(f, face.face, c, se);
    if (!g) continue;
    std::string h = outlineHash(face.face, g);
    if (h.empty() || h == "|") continue;
    std::u32string meaning;
    auto it = tum.find(c);
    if (it != tum.end()) meaning = it->second;
    else if (uint32_t u = pdfUnicode(f, c, se)) meaning = std::u32string(1, static_cast<char32_t>(u));
    else meaning = U"#" + std::u32string(1, static_cast<char32_t>(c));
    byOutline[h].insert(meaning);
  }
  for (const auto& kv : byOutline) {
    if (kv.second.size() > 1) {
      out.push_back("identical_glyphs_for_different_chars");
      break;
    }
  }
  return out;
}

const std::vector<DocumentPart>& documentParts(const Events& ev) { return facts(ev).dparts; }

void registerExtChecks(CheckTable& t) {
  t.add("action.trigger", evalTrigger);
  t.add("color.device_space_rule", evalDeviceSpaceRule);
  t.add("color.unused_resource", evalUnusedResource);
  t.add("document.job_options", evalJobOptions);
  t.add("document.script", evalScriptCheck);
  t.add("einvoice.relationship_valid", evalInvoiceRelationship);
  t.add("einvoice.valid", evalInvoiceValid);
  t.add("layer.membership", evalLayerMembership);
  t.add("output_intent.colour_exchange", evalColourExchange);
  t.add("print.mixing_hints", evalMixingHints);
  t.add("scope.every", evalEvery);
  t.add("syntax.broken_reference", evalBrokenReference);
  t.add("syntax.dictionary_error", evalDictionaryError);
  t.add("syntax.linearization_id_mismatch", evalLinearizationId);
  t.add("text.unicode_conflict", evalUnicodeConflict);
  t.add("variable_data.encapsulated", evalEncapsulated);
  t.add("variable_data.xobject_uses", evalXObjectUses);
  t.add("annotation.multimedia", evalMultimedia);
  t.add("conformance.meets", evalConformance);
  t.add("security.encryption", evalEncryption);
  t.add("metadata.xmp_schema_file", evalSchemaFile);
  registerXmpChecks(t);
  registerStructureChecks(t);
}
}

