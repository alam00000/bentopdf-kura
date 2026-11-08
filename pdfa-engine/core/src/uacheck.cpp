#include "uacheck.hh"

#include <qpdf/Pl_Discard.hh>
#include <qpdf/QPDFNameTreeObjectHelper.hh>
#include <qpdf/QPDFNumberTreeObjectHelper.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <algorithm>
#include <functional>
#include <map>
#include <regex>

#include "limits.hh"
#include "pdfa/pdfa.hh"
#include "util.hh"

namespace pdfa {
std::set<uint32_t> puaGlyphCodes(QPDFObjectHandle font);
namespace {
const char* const kNs17 = "http://iso.org/pdf/ssn";
const char* const kNs20 = "http://iso.org/pdf2/ssn";
const char* const kNsMath = "http://www.w3.org/1998/Math/MathML";

const std::set<std::string>& uaStandardTypes() {
  static const std::set<std::string> s = {
      "/Document", "/Part", "/Art", "/Sect", "/Div", "/BlockQuote", "/Caption", "/TOC", "/TOCI",
      "/Index", "/NonStruct", "/Private", "/P", "/H", "/H1", "/H2", "/H3", "/H4", "/H5", "/H6",
      "/L", "/LI", "/Lbl", "/LBody", "/Table", "/TR", "/TH", "/TD", "/THead", "/TBody", "/TFoot",
      "/Span", "/Quote", "/Note", "/Reference", "/BibEntry", "/Code", "/Link", "/Annot", "/Ruby",
      "/RB", "/RT", "/RP", "/Warichu", "/WT", "/WP", "/Figure", "/Formula", "/Form"};
  return s;
}

const std::set<std::string>& pdf2Types() {
  static const std::set<std::string> s = {
      "/Document", "/DocumentFragment", "/Part", "/Sect", "/Div", "/Aside", "/NonStruct", "/P", "/H",
      "/Title", "/FENote", "/Sub", "/Lbl", "/Span", "/Em", "/Strong", "/Link", "/Annot", "/Form",
      "/Ruby", "/RB", "/RT", "/RP", "/Warichu", "/WT", "/WP", "/L", "/LI", "/LBody", "/Table", "/TR",
      "/TH", "/TD", "/THead", "/TBody", "/TFoot", "/Caption", "/Figure", "/Formula", "/Artifact"};
  return s;
}

bool isHn(const std::string& t) {
  if (t.size() < 3 || t[0] != '/' || t[1] != 'H' || t[2] < '1' || t[2] > '9') return false;
  return t.find_first_not_of("0123456789", 3) == std::string::npos;
}

bool passThrough(const std::string& t) {
  return t == "/Div" || t == "/Part" || t == "/NonStruct";
}

bool langValid(const std::string& s) {
  static const std::regex re("^[a-zA-Z]{1,8}(-[a-zA-Z0-9]{1,8})*$");
  return std::regex_match(s, re);
}

std::string textOf(QPDFObjectHandle o) {
  return o.isString() ? o.getUTF8Value() : std::string();
}

bool hasPua(QPDFObjectHandle o) {
  bool changed = false;
  if (o.isString()) stripPuaUtf8(o.getUTF8Value(), changed);
  return changed;
}

bool isStructElem(QPDFObjectHandle k) {
  if (!k.isDictionary()) return false;
  QPDFObjectHandle t = k.getKey("/Type");
  return !t.isName() || t.getName() == "/StructElem";
}

bool isContentItem(QPDFObjectHandle k) {
  return k.isInteger() || (k.isDictionary() && (nameIs(k.getKey("/Type"), "/MCR") || nameIs(k.getKey("/Type"), "/OBJR")));
}

std::vector<QPDFObjectHandle> kidList(QPDFObjectHandle e) {
  std::vector<QPDFObjectHandle> out;
  QPDFObjectHandle k = e.isDictionary() ? e.getKey("/K") : QPDFObjectHandle::newNull();
  if (k.isArray()) {
    for (int i = 0; i < k.getArrayNItems(); ++i) out.push_back(k.getArrayItem(i));
  } else if (!k.isNull()) {
    out.push_back(k);
  }
  return out;
}

std::vector<QPDFObjectHandle> kidsOf(QPDFObjectHandle e) {
  std::vector<QPDFObjectHandle> out;
  for (QPDFObjectHandle k : kidList(e)) {
    if (isStructElem(k)) out.push_back(k);
  }
  return out;
}

std::vector<QPDFObjectHandle> allKids(QPDFObjectHandle e) {
  std::vector<QPDFObjectHandle> out;
  for (QPDFObjectHandle k : kidList(e)) {
    if (isStructElem(k) || isContentItem(k)) out.push_back(k);
  }
  return out;
}

QPDFObjectHandle attrFrom(QPDFObjectHandle a, const std::string& key, const std::string& owner, char kind) {
  auto pick = [&](QPDFObjectHandle d) {
    if (!d.isDictionary() || !nameIs(d.getKey("/O"), owner)) return QPDFObjectHandle::newNull();
    QPDFObjectHandle v = d.getKey(key);
    bool good = kind == 'n' ? v.isName() : kind == 'i' ? v.isInteger() : v.isArray();
    return good ? v : QPDFObjectHandle::newNull();
  };
  for (int i = 0; a.isArray() && i < a.getArrayNItems(); ++i) {
    QPDFObjectHandle v = pick(a.getArrayItem(i));
    if (!v.isNull()) return v;
  }
  return pick(a);
}

QPDFObjectHandle attr(QPDFObjectHandle e, const std::string& key, const std::string& owner, char kind,
                      QPDFObjectHandle classMap) {
  QPDFObjectHandle v = attrFrom(e.getKey("/A"), key, owner, kind);
  if (!v.isNull() || !classMap.isDictionary()) return v;
  QPDFObjectHandle c = e.getKey("/C");
  if (c.isName()) return attrFrom(classMap.getKey(c.getName()), key, owner, kind);
  for (int i = 0; c.isArray() && i < c.getArrayNItems(); ++i) {
    if (!c.getArrayItem(i).isName()) continue;
    v = attrFrom(classMap.getKey(c.getArrayItem(i).getName()), key, owner, kind);
    if (!v.isNull()) return v;
  }
  return QPDFObjectHandle::newNull();
}

class Types {
 public:
  Types(QPDFObjectHandle tree, int part)
      : part_(part),
        rm_(tree.isDictionary() ? tree.getKey("/RoleMap") : QPDFObjectHandle::newNull()) {}

  std::string of(QPDFObjectHandle e) { return full(e).first; }

  std::pair<std::string, std::string> full(QPDFObjectHandle e) {
    if (!e.isDictionary()) return {};
    if (e.isIndirect()) {
      auto it = cache_.find(e.getObjGen());
      if (it != cache_.end()) return it->second;
    }
    std::pair<std::string, std::string> r =
        part_ == 2 ? resolve2(e) : std::make_pair(chain17(nameOf(e.getKey("/S"))), std::string(kNs17));
    if (part_ == 2 && r.second == kNsMath) r.first = "/MathML";
    if (e.isIndirect()) cache_[e.getObjGen()] = r;
    return r;
  }

  bool ownStandard(QPDFObjectHandle e) {
    QPDFObjectHandle s = e.getKey("/S");
    if (!s.isName()) return false;
    if (part_ != 2) return uaStandardTypes().count(s.getName()) > 0;
    QPDFObjectHandle ns = e.getKey("/NS");
    return std2({s.getName(), ns.isDictionary() ? ns : QPDFObjectHandle::newNull()});
  }

  bool remapped17(QPDFObjectHandle e) {
    std::string raw = nameOf(e.getKey("/S"));
    QPDFObjectHandle m = mapped(raw);
    return uaStandardTypes().count(raw) && m.isName() && m.getName() != raw;
  }

  bool circular(QPDFObjectHandle e) {
    std::string raw = nameOf(e.getKey("/S"));
    std::set<std::string> seen{raw};
    for (QPDFObjectHandle res = mapped(raw); res.isName(); res = mapped(res.getName())) {
      if (!seen.insert(res.getName()).second) return true;
    }
    if (part_ != 2 || raw.empty()) return false;
    return nsChain(e) == 1;
  }

  bool sameNamespaceMapping(QPDFObjectHandle e) {
    return part_ == 2 && e.getKey("/S").isName() && nsChain(e) == 2;
  }

  std::string parentStd(QPDFObjectHandle e) {
    QPDFObjectHandle p = e.getKey("/P");
    std::set<QPDFObjGen> seen;
    for (int i = 0; i < kMaxStructDepth; ++i) {
      if (!p.isDictionary()) return "";
      std::string t = of(p);
      if (!passThrough(t)) return nameIs(p.getKey("/Type"), "/StructTreeRoot") ? "/StructTreeRoot" : t;
      if (p.isIndirect() && !seen.insert(p.getObjGen()).second) return "";
      p = p.getKey("/P");
    }
    return "";
  }

  void sigKids(QPDFObjectHandle e, std::vector<QPDFObjectHandle>& out, int depth = 0) {
    if (depth > kMaxStructDepth) return;
    for (QPDFObjectHandle k : kidsOf(e)) {
      if (passThrough(of(k))) {
        sigKids(k, out, depth + 1);
      } else {
        out.push_back(k);
      }
    }
  }

  std::vector<QPDFObjectHandle> sigKids(QPDFObjectHandle e) {
    std::vector<QPDFObjectHandle> out;
    sigKids(e, out);
    return out;
  }

  std::vector<std::string> kidTypes(QPDFObjectHandle e) {
    std::vector<std::string> out;
    for (QPDFObjectHandle k : sigKids(e)) {
      std::string t = of(k);
      if (!t.empty()) out.push_back(t);
    }
    return out;
  }

  bool hasContent(QPDFObjectHandle e, int depth = 0) {
    if (depth > kMaxStructDepth) return false;
    for (QPDFObjectHandle k : kidList(e)) {
      if (isContentItem(k)) return true;
    }
    for (QPDFObjectHandle k : kidsOf(e)) {
      if (passThrough(of(k)) && hasContent(k, depth + 1)) return true;
    }
    return false;
  }

 private:
  using SType = std::pair<std::string, QPDFObjectHandle>;

  QPDFObjectHandle mapped(const std::string& t) {
    return rm_.isDictionary() && !t.empty() ? rm_.getKey(t) : QPDFObjectHandle::newNull();
  }

  std::string chain17(const std::string& t) {
    if (t.empty()) return "";
    std::set<std::string> seen{t};
    QPDFObjectHandle res = mapped(t);
    if ((!res.isName() || res.getName() == t) && uaStandardTypes().count(t)) return t;
    while (res.isName() && seen.insert(res.getName()).second) {
      if (uaStandardTypes().count(res.getName())) return res.getName();
      res = mapped(res.getName());
    }
    return "";
  }

  static std::string uriOf(QPDFObjectHandle ns) {
    return ns.isDictionary() ? textOf(ns.getKey("/NS")) : std::string();
  }

  static std::string keyOf(const SType& s) {
    if (!s.second.isDictionary()) return s.first + "|";
    return s.first + "|" + (s.second.isIndirect() ? s.second.getObjGen().unparse(' ') : std::string("d"));
  }

  static bool std2(const SType& s) {
    if (s.second.isDictionary() && s.second.getKey("/NS").isString()) {
      std::string u = uriOf(s.second);
      if (u == kNs17) return uaStandardTypes().count(s.first) > 0;
      if (u == kNs20) return pdf2Types().count(s.first) > 0 || isHn(s.first);
      return u == kNsMath;
    }
    return uaStandardTypes().count(s.first) > 0;
  }

  int nsChain(QPDFObjectHandle e) {
    QPDFObjectHandle ns = e.getKey("/NS");
    SType prev{nameOf(e.getKey("/S")), ns.isDictionary() ? ns : QPDFObjectHandle::newNull()};
    std::set<std::string> seen{keyOf(prev)};
    std::map<std::string, std::string> done;
    SType cur;
    for (bool has = equiv(prev, cur, false); has && seen.size() < 256; has = equiv(prev, cur, false)) {
      if (!prev.second.isDictionary() || prev.second.getKey("/NS").isString()) done[made(prev).second] = prev.first;
      bool known = !cur.second.isDictionary() || cur.second.getKey("/NS").isString();
      auto it = known ? done.find(made(cur).second) : done.end();
      if (it != done.end()) return it->second == cur.first ? 1 : 2;
      if (!seen.insert(keyOf(cur)).second) return 0;
      prev = cur;
    }
    return 0;
  }

  bool equiv(const SType& s, SType& out, bool roleMap = true) {
    if (s.second.isDictionary()) {
      QPDFObjectHandle m = s.second.getKey("/RoleMapNS");
      if (m.isDictionary()) {
        QPDFObjectHandle v = m.getKey(s.first);
        if (v.isName()) {
          out = {v.getName(), QPDFObjectHandle::newNull()};
          return true;
        }
        if (v.isArray() && v.getArrayNItems() >= 2 && v.getArrayItem(0).isName()) {
          QPDFObjectHandle n = v.getArrayItem(1);
          out = {v.getArrayItem(0).getName(), n.isDictionary() ? n : QPDFObjectHandle::newNull()};
          return true;
        }
        return false;
      }
      if (uriOf(s.second) != kNs17) return false;
    }
    if (!roleMap) return false;
    QPDFObjectHandle v = mapped(s.first);
    if (!v.isName()) return false;
    out = {v.getName(), QPDFObjectHandle::newNull()};
    return true;
  }

  static std::pair<std::string, std::string> made(const SType& s) {
    return {s.first, s.second.isDictionary() ? uriOf(s.second) : std::string(kNs17)};
  }

  std::pair<std::string, std::string> resolve2(QPDFObjectHandle e) {
    QPDFObjectHandle s = e.getKey("/S");
    if (!s.isName()) return {};
    QPDFObjectHandle ns = e.getKey("/NS");
    SType start{s.getName(), ns.isDictionary() ? ns : QPDFObjectHandle::newNull()};
    std::set<std::string> seen{keyOf(start)};
    SType cur;
    bool has = equiv(start, cur);
    if (!has || seen.count(keyOf(cur))) return std2(start) ? made(start) : std::pair<std::string, std::string>();
    for (int i = 0; has && !seen.count(keyOf(cur)) && i < 256; ++i) {
      if (std2(cur)) return made(cur);
      seen.insert(keyOf(cur));
      SType next;
      has = equiv(cur, next);
      cur = next;
    }
    return {};
  }

  int part_;
  QPDFObjectHandle rm_;
  std::map<QPDFObjGen, std::pair<std::string, std::string>> cache_;
};

struct UaScanState {
  int part = 1;
  bool catalogLang = false;
  const std::map<long long, QPDFObjectHandle>* parentTree = nullptr;
  std::function<bool(QPDFObjectHandle)> reaches;
  std::map<std::pair<QPDFObjGen, int>, bool> mcidLang;
  std::map<QPDFObjGen, int> taggedFormDraws;
  long long untagged = 0;
  long long nested = 0;
  long long textNoLang = 0;
  long long spanNoLang = 0;
  long long badLang = 0;
  long long puaAlt = 0;
  long long puaActual = 0;
  long long puaGlyph = 0;
  std::map<QPDFObjGen, std::set<uint32_t>> puaCodes;

  QPDFObjectHandle tree(long long key) const {
    auto it = parentTree->find(key);
    return it == parentTree->end() ? QPDFObjectHandle::newNull() : it->second;
  }

  QPDFObjectHandle lookup(QPDFObjectHandle owner, long long mcid) const {
    if (!owner.isDictionary()) return QPDFObjectHandle::newNull();
    QPDFObjectHandle sp = owner.getKey("/StructParent");
    if (sp.isInteger()) return tree(sp.getIntValue());
    QPDFObjectHandle sps = owner.getKey("/StructParents");
    if (mcid < 0 || !sps.isInteger()) return QPDFObjectHandle::newNull();
    QPDFObjectHandle arr = tree(sps.getIntValue());
    if (!arr.isArray() || arr.getArrayNItems() <= mcid) return QPDFObjectHandle::newNull();
    return arr.getArrayItem(static_cast<int>(mcid));
  }
};

class UntaggedScan : public QPDFObjectHandle::TokenFilter {
 public:
  UntaggedScan(UaScanState& st, QPDFObjectHandle res, QPDFObjectHandle ownerDict, QPDFObjGen owner, QPDFObjGen page,
               QPDFObjectHandle elem, bool outerArtifact, bool inheritedLang, int depth)
      : st(st),
        res(res),
        ownerDict(ownerDict),
        owner(owner),
        page(page),
        elem(elem),
        elemTagged(st.reaches(elem)),
        outerArtifact(outerArtifact),
        inheritedLang(inheritedLang),
        depth(depth) {}

  void handleToken(QPDFTokenizer::Token const& tok) override {
    auto t = tok.getType();
    if (t == QPDFTokenizer::tt_inline_image) {
      if (!covered()) ++st.untagged;
      return;
    }
    if (t != QPDFTokenizer::tt_word) {
      if (t != QPDFTokenizer::tt_space && t != QPDFTokenizer::tt_comment) ops.push_back(tok);
      return;
    }
    std::string op = tok.getValue();
    if (op == "BDC" || op == "BMC") {
      openMark(op == "BDC");
    } else if (op == "EMC") {
      if (!marks.empty()) marks.pop_back();
    } else if (op == "Tf" && ops.size() >= 2 && ops[ops.size() - 2].getType() == QPDFTokenizer::tt_name) {
      QPDFObjectHandle fd = res.isDictionary() ? res.getKey("/Font") : QPDFObjectHandle();
      font = fd.isDictionary() ? fd.getKey(ops[ops.size() - 2].getValue()) : QPDFObjectHandle();
    } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
      if (!covered()) {
        ++st.untagged;
      } else if (!st.catalogLang && !inArtifact() && !langKnown()) {
        ++st.textNoLang;
      }
      if (st.part == 2 && covered() && !inArtifact() && showsPua() && !altCovered()) ++st.puaGlyph;
    } else if (op == "f" || op == "F" || op == "f*" || op == "B" || op == "B*" || op == "b" || op == "b*" ||
               op == "S" || op == "s" || op == "sh") {
      if (!covered()) ++st.untagged;
    } else if (op == "Do" && !ops.empty()) {
      QPDFObjectHandle xo = res.isDictionary() ? res.getKey("/XObject") : QPDFObjectHandle();
      QPDFObjectHandle x = xo.isDictionary() ? xo.getKey(ops.back().getValue()) : QPDFObjectHandle();
      if (x.isStream() && nameIs(x.getDict().getKey("/Subtype"), "/Form") && depth < kMaxContentNest) {
        if (x.getDict().hasKey("/StructParents") && x.isIndirect()) ++st.taggedFormDraws[x.getObjGen()];
        QPDFObjectHandle fr = x.getDict().getKey("/Resources");
        QPDFObjectHandle own = st.lookup(x.getDict(), -1);
        UntaggedScan inner(st, fr.isDictionary() ? fr : res, x.getDict(), x.isIndirect() ? x.getObjGen() : owner, page,
                           own.isNull() ? current() : own, inArtifact(), inheritedLang || langKnown(), depth + 1);
        Pl_Discard d;
        try {
          x.filterAsContents(&inner, &d);
        } catch (...) {
        }
      } else if (x.isStream() && !covered()) {
        ++st.untagged;
      }
    }
    ops.clear();
  }

 private:
  struct Mark {
    QPDFObjectHandle elem;
    bool alt = false;
    bool tagged = false;
    bool hasMcid = false;
    bool artifact = false;
    bool lang = false;
  };

  void openMark(bool bdc) {
    Mark m;
    std::string tag = !ops.empty() && ops[0].getType() == QPDFTokenizer::tt_name ? ops[0].getValue() : "";
    m.artifact = tag == "/Artifact";
    QPDFObjectHandle props;
    if (bdc && ops.size() >= 2 && ops.back().getType() == QPDFTokenizer::tt_name) {
      QPDFObjectHandle pr = res.isDictionary() ? res.getKey("/Properties") : QPDFObjectHandle();
      props = pr.isDictionary() ? pr.getKey(ops.back().getValue()) : QPDFObjectHandle();
    }
    long long mcid = -1;
    bool hasLang = false, hasAlt = false, hasAltText = false;
    bool real = !outerArtifact && st.part == 2;
    if (props.isDictionary()) {
      if (props.getKey("/MCID").isInteger()) mcid = props.getKey("/MCID").getIntValue();
      QPDFObjectHandle l = props.getKey("/Lang");
      if (l.isString()) {
        hasLang = true;
        if (!langValid(l.getUTF8Value())) ++st.badLang;
      }
      hasAlt = props.hasKey("/ActualText") || props.hasKey("/Alt") || props.hasKey("/E");
      hasAltText = props.hasKey("/ActualText") || props.hasKey("/Alt");
      if (real && hasPua(props.getKey("/Alt"))) ++st.puaAlt;
      if (real && hasPua(props.getKey("/ActualText"))) ++st.puaActual;
    } else if (bdc) {
      int nest = 0;
      for (size_t i = 1; i < ops.size(); ++i) {
        auto ty = ops[i].getType();
        if (ty == QPDFTokenizer::tt_dict_open || ty == QPDFTokenizer::tt_array_open) ++nest;
        if (ty == QPDFTokenizer::tt_dict_close || ty == QPDFTokenizer::tt_array_close) --nest;
        if (nest != 1 || ty != QPDFTokenizer::tt_name || i + 1 >= ops.size()) continue;
        const std::string& k = ops[i].getValue();
        const auto& v = ops[i + 1];
        if (k == "/MCID" && v.getType() == QPDFTokenizer::tt_integer) mcid = std::stoll(v.getValue());
        if (k == "/Lang" && v.getType() == QPDFTokenizer::tt_string) {
          hasLang = true;
          if (!langValid(QPDFObjectHandle::newString(v.getValue()).getUTF8Value())) ++st.badLang;
        }
        if (k == "/ActualText" || k == "/Alt" || k == "/E") hasAlt = true;
        if (k == "/ActualText" || k == "/Alt") hasAltText = true;
        if (real && v.getType() == QPDFTokenizer::tt_string && (k == "/Alt" || k == "/ActualText") &&
            hasPua(QPDFObjectHandle::newString(v.getValue()))) {
          ++(k == "/Alt" ? st.puaAlt : st.puaActual);
        }
      }
    }
    m.alt = hasAltText;
    m.hasMcid = mcid >= 0;
    QPDFObjectHandle own = m.hasMcid ? st.lookup(ownerDict, mcid) : QPDFObjectHandle::newNull();
    m.elem = own.isNull() ? current() : own;
    m.tagged = own.isNull() ? currentTagged() : st.reaches(own);
    bool outerLang = inheritedLang || langKnown();
    bool treeLang = false;
    if (m.hasMcid) {
      auto it = st.mcidLang.find({owner, static_cast<int>(mcid)});
      if (it == st.mcidLang.end()) it = st.mcidLang.find({page, static_cast<int>(mcid)});
      treeLang = it == st.mcidLang.end() || it->second;
    }
    m.lang = hasLang || treeLang || (!m.hasMcid && outerLang);
    if (tag == "/Span" && hasAlt && !st.catalogLang && !hasLang && !outerLang && !treeLang) ++st.spanNoLang;
    if (m.tagged && (m.artifact || inArtifact())) ++st.nested;
    marks.push_back(m);
  }
  QPDFObjectHandle current() const {
    return marks.empty() ? elem : marks.back().elem;
  }
  bool currentTagged() const {
    return marks.empty() ? elemTagged : marks.back().tagged;
  }
  bool covered() const {
    return currentTagged() || inArtifact();
  }
  bool showsPua() {
    if (!font.isDictionary()) return false;
    std::set<uint32_t> empty;
    const std::set<uint32_t>* codes = &empty;
    if (font.isIndirect()) {
      auto it = st.puaCodes.find(font.getObjGen());
      if (it == st.puaCodes.end()) it = st.puaCodes.emplace(font.getObjGen(), puaGlyphCodes(font)).first;
      codes = &it->second;
    } else {
      empty = puaGlyphCodes(font);
    }
    if (codes->empty()) return false;
    size_t width = nameIs(font.getKey("/Subtype"), "/Type0") ? 2 : 1;
    for (const auto& t : ops) {
      if (t.getType() != QPDFTokenizer::tt_string) continue;
      const std::string& b = t.getValue();
      for (size_t i = 0; i + width <= b.size(); i += width) {
        uint32_t c = static_cast<unsigned char>(b[i]);
        if (width == 2) c = (c << 8) | static_cast<unsigned char>(b[i + 1]);
        if (codes->count(c)) return true;
      }
    }
    return false;
  }
  bool altCovered() const {
    for (const Mark& m : marks) {
      if (m.alt) return true;
    }
    Visited seen;
    for (QPDFObjectHandle e = current(); e.isDictionary() && seen.enter(e); e = e.getKey("/P")) {
      if (e.hasKey("/ActualText") || e.hasKey("/Alt")) return true;
    }
    return false;
  }
  bool inArtifact() const {
    if (outerArtifact) return true;
    for (const Mark& m : marks) {
      if (m.artifact) return true;
    }
    return false;
  }
  bool langKnown() const {
    if (inheritedLang) return true;
    for (const Mark& m : marks) {
      if (m.lang) return true;
    }
    return false;
  }
  UaScanState& st;
  QPDFObjectHandle res;
  QPDFObjectHandle ownerDict;
  QPDFObjGen owner;
  QPDFObjGen page;
  QPDFObjectHandle elem;
  bool elemTagged;
  bool outerArtifact;
  bool inheritedLang;
  int depth;
  std::vector<Mark> marks;
  std::vector<QPDFTokenizer::Token> ops;
  QPDFObjectHandle font;
};

struct Cell {
  bool th = false;
  QPDFObjectHandle e;
  long long rs = 1, cs = 1;
  long long row = 0, col = 0;
};

void checkTable(QPDFObjectHandle table, Types& types, QPDFObjectHandle classMap, int part, std::set<std::string>& rules) {
  auto rule = [&](const char* a, const char* b) { rules.insert(part == 2 ? b : a); };
  std::vector<std::vector<Cell>> rows;
  std::vector<size_t> groups;
  auto addRow = [&](QPDFObjectHandle tr) {
    rows.emplace_back();
    for (QPDFObjectHandle c : types.sigKids(tr)) {
      std::string t = types.of(c);
      if (t != "/TH" && t != "/TD") continue;
      Cell cell;
      cell.th = t == "/TH";
      cell.e = c;
      QPDFObjectHandle rs = attr(c, "/RowSpan", "/Table", 'i', classMap);
      QPDFObjectHandle cs = attr(c, "/ColSpan", "/Table", 'i', classMap);
      if (rs.isInteger()) cell.rs = std::clamp<long long>(rs.getIntValue(), 1, 1000);
      if (cs.isInteger()) cell.cs = std::clamp<long long>(cs.getIntValue(), 1, 1000);
      rows.back().push_back(cell);
    }
  };
  for (QPDFObjectHandle k : types.sigKids(table)) {
    std::string t = types.of(k);
    if (t == "/TR") {
      addRow(k);
    } else if (t == "/THead" || t == "/TBody" || t == "/TFoot") {
      groups.push_back(rows.size());
      for (QPDFObjectHandle r : types.sigKids(k)) {
        if (types.of(r) == "/TR") addRow(r);
      }
      groups.push_back(rows.size());
    }
  }
  long long nRows = 0;
  for (size_t r = 0; r < rows.size(); ++r) {
    if (rows[r].empty()) continue;
    nRows += rows[r][0].rs;
    r += static_cast<size_t>(rows[r][0].rs - 1);
  }
  if (nRows == 0) return;
  long long nCols = 0;
  for (const Cell& c : rows[0]) nCols += c.cs;
  if (nRows * std::max<long long>(nCols, 1) > 4000000) return;
  std::vector<Cell*> grid(static_cast<size_t>(nRows * nCols), nullptr);
  auto at = [&](long long r, long long c) -> Cell*& { return grid[static_cast<size_t>(r * nCols + c)]; };
  for (size_t ri = 0; ri < rows.size(); ++ri) {
    long long r = static_cast<long long>(ri);
    long long col = 0;
    for (Cell& cell : rows[ri]) {
      if (r >= nRows) return rule("7.2-41", "8.2.5.26-2");
      while (col < nCols && at(r, col)) ++col;
      if (col + cell.cs > nCols) return rule("7.2-42", "8.2.5.26-3");
      if (r + cell.rs > nRows) return rule("7.2-41", "8.2.5.26-2");
      if (part == 2) {
        for (size_t g : groups) {
          long long gl = static_cast<long long>(g);
          if (r + cell.rs > gl && r < gl) return rule("7.2-41", "8.2.5.26-2");
        }
      }
      cell.row = r;
      cell.col = col;
      for (long long dr = 0; dr < cell.rs; ++dr) {
        for (long long dc = 0; dc < cell.cs; ++dc) {
          if (at(r + dr, col + dc)) return rule("7.2-15", "8.2.5.26-1");
          at(r + dr, col + dc) = &cell;
        }
      }
      col += cell.cs;
    }
  }
  for (Cell* c : grid) {
    if (!c) return rule("7.2-43", "8.2.5.26-4");
  }
  std::set<std::string> ids;
  bool allScoped = true;
  for (Cell* c : grid) {
    if (!c->th) continue;
    std::string id = textOf(c->e.getKey("/ID"));
    if (!id.empty()) ids.insert(id);
    if (attr(c->e, "/Scope", "/Table", 'n', classMap).isNull()) allScoped = false;
  }
  if (allScoped) return;
  auto scoped = [&](long long r, long long c, const std::string& want) {
    Cell* x = at(r, c);
    if (!x->th) return false;
    std::string s = nameOf(attr(x->e, "/Scope", "/Table", 'n', classMap));
    if (s.empty() && part == 2) s = r == 0 && c == 0 ? "/Both" : r == 0 ? "/Column" : c == 0 ? "/Row" : "/Both";
    return s == "/Both" || s == want;
  };
  auto positional = [&](const Cell& x) {
    for (long long c = x.col; x.row > 0 && c < x.col + x.cs; ++c) {
      bool found = false;
      for (long long r = x.row - 1; r >= 0; --r) {
        if (scoped(r, c, "/Column")) return true;
        if (at(r, c)->th) {
          found = true;
        } else if (found) {
          break;
        }
      }
    }
    for (long long r = x.row; x.col > 0 && r < x.row + x.rs; ++r) {
      bool found = false;
      for (long long c = x.col - 1; c >= 0; --c) {
        if (scoped(r, c, "/Row")) return true;
        if (at(r, c)->th) {
          found = true;
        } else if (found) {
          break;
        }
      }
    }
    return false;
  };
  for (long long r = 0; r < nRows; ++r) {
    for (long long c = 0; c < nCols; ++c) {
      Cell* x = at(r, c);
      if (x->th || x->row != r || x->col != c) continue;
      QPDFObjectHandle hd = attr(x->e, "/Headers", "/Table", 'a', classMap);
      std::vector<std::string> list;
      for (int j = 0; hd.isArray() && j < hd.getArrayNItems(); ++j) {
        if (hd.getArrayItem(j).isString()) list.push_back(textOf(hd.getArrayItem(j)));
      }
      bool unknown = false;
      for (const std::string& h : list) unknown = unknown || !ids.count(h);
      if ((!list.empty() && !unknown) || positional(*x)) continue;
      return unknown ? rule("7.5-2", "8.2.5.26-6") : rule("7.5-1", "8.2.5.26-5");
    }
  }
}

bool outsideCrop(QPDFObjectHandle rect, QPDFObjectHandle box) {
  if (!rect.isArray() || rect.getArrayNItems() < 4 || !box.isArray() || box.getArrayNItems() != 4) return false;
  auto v = [](QPDFObjectHandle a, int i) { return numOf(a.getArrayItem(i), 0); };
  double bx0 = std::min(v(box, 0), v(box, 2)), bx1 = std::max(v(box, 0), v(box, 2));
  double by0 = std::min(v(box, 1), v(box, 3)), by1 = std::max(v(box, 1), v(box, 3));
  return by0 >= v(rect, 3) || bx0 >= v(rect, 2) || by1 <= v(rect, 1) || bx1 <= v(rect, 0);
}

void forEachDirect(QPDFObjectHandle o, const std::function<void(QPDFObjectHandle)>& fn, int depth) {
  if (depth > kMaxObjectWalk) return;
  if (o.isStream()) {
    fn(o);
    o = o.getDict();
  } else if (o.isDictionary()) {
    fn(o);
  }
  if (o.isDictionary()) {
    for (const std::string& k : o.getKeys()) {
      QPDFObjectHandle v = o.getKey(k);
      if (!v.isIndirect() && (v.isDictionary() || v.isArray())) forEachDirect(v, fn, depth + 1);
    }
  } else if (o.isArray()) {
    for (int i = 0; i < o.getArrayNItems(); ++i) {
      QPDFObjectHandle v = o.getArrayItem(i);
      if (!v.isIndirect() && (v.isDictionary() || v.isArray())) forEachDirect(v, fn, depth + 1);
    }
  }
}

struct PrefixUse {
  std::string prefix;
  std::string prop;
  std::string value;
};

std::vector<PrefixUse> uaIdProps(const std::string& xmp) {
  std::vector<PrefixUse> out;
  static const std::string ns = "\"http://www.aiim.org/pdfua/ns/id/\"";
  std::set<std::string> prefixes;
  for (size_t at = xmp.find(ns); at != std::string::npos; at = xmp.find(ns, at + 1)) {
    size_t eq = xmp.rfind('=', at);
    size_t colon = xmp.rfind("xmlns:", eq);
    if (eq == std::string::npos || colon == std::string::npos || eq - colon > 64) continue;
    std::string p = xmp.substr(colon + 6, eq - colon - 6);
    while (!p.empty() && std::isspace(static_cast<unsigned char>(p.back()))) p.pop_back();
    prefixes.insert(p);
  }
  for (const std::string& p : prefixes) {
    for (const char* prop : {"part", "rev", "amd", "corr"}) {
      std::string el = "<" + p + ":" + prop + ">";
      size_t at = xmp.find(el);
      if (at != std::string::npos) {
        size_t e = xmp.find('<', at + el.size());
        out.push_back({p, prop, xmp.substr(at + el.size(), e == std::string::npos ? 0 : e - at - el.size())});
        continue;
      }
      std::string attr = p + ":" + prop + "=";
      at = xmp.find(attr);
      if (at != std::string::npos && at + attr.size() < xmp.size()) {
        char q = xmp[at + attr.size()];
        size_t e = xmp.find(q, at + attr.size() + 1);
        out.push_back({p, prop, e == std::string::npos ? "" : xmp.substr(at + attr.size() + 1, e - at - attr.size() - 1)});
      }
    }
  }
  return out;
}

bool onlyDefaultLangAlt(const std::string& xmp) {
  for (size_t at = xmp.find("<rdf:Alt"); at != std::string::npos; at = xmp.find("<rdf:Alt", at + 1)) {
    size_t end = xmp.find("</rdf:Alt>", at);
    if (end == std::string::npos) break;
    std::string body = xmp.substr(at, end - at);
    bool def = false, other = false;
    for (size_t p = body.find("xml:lang="); p != std::string::npos; p = body.find("xml:lang=", p + 1)) {
      if (body.compare(p + 10, 9, "x-default") == 0) def = true;
      else other = true;
    }
    if (def && !other) return true;
  }
  return false;
}

bool titleHasValue(const std::string& xmp) {
  size_t at = xmp.find("<dc:title");
  if (at == std::string::npos) return false;
  size_t end = xmp.find("</dc:title>", at);
  return xmp.substr(at, end == std::string::npos ? std::string::npos : end - at).find("<rdf:li") != std::string::npos;
}

std::string streamText(QPDFObjectHandle s) {
  try {
    auto b = s.getStreamData(qpdf_dl_generalized);
    return std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
  } catch (...) {
    return std::string();
  }
}

bool xfaDynamic(QPDFObjectHandle acro) {
  QPDFObjectHandle x = acro.getKey("/XFA");
  if (x.isArray()) {
    QPDFObjectHandle found;
    for (int i = 0; i + 1 < x.getArrayNItems(); ++i) {
      if (textOf(x.getArrayItem(i)) == "config") {
        found = x.getArrayItem(i + 1);
        break;
      }
    }
    x = found.isInitialized() ? found : QPDFObjectHandle::newNull();
  }
  if (!x.isStream()) return false;
  std::string s = streamText(x);
  size_t at = s.find("<config");
  at = at == std::string::npos ? at : s.find("<acrobat7", at);
  at = at == std::string::npos ? at : s.find("<dynamicRender", at);
  at = at == std::string::npos ? at : s.find('>', at);
  if (at == std::string::npos) return false;
  size_t end = s.find('<', at);
  return s.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1) == "required";
}

bool richText(const std::string& s, std::string& out) {
  size_t i = s.find_first_not_of(" \t\r\n");
  if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) i = s.find_first_not_of(" \t\r\n", 3);
  if (i == std::string::npos || s[i] != '<') return false;
  int depth = 0;
  std::string raw;
  while (i < s.size()) {
    if (s[i] == '<') {
      size_t e = s.find('>', i);
      if (e == std::string::npos) return false;
      if (s.compare(i, 9, "<![CDATA[") == 0) {
        size_t ce = s.find("]]>", i);
        if (ce == std::string::npos) return false;
        if (depth > 0) raw += "\x01" + s.substr(i + 9, ce - i - 9) + "\x02";
        i = ce + 3;
        continue;
      }
      if (s[i + 1] == '/') {
        --depth;
      } else if (s[i + 1] != '?' && s[i + 1] != '!' && s[e - 1] != '/') {
        ++depth;
      }
      i = e + 1;
      continue;
    }
    if (depth > 0) raw += s[i];
    ++i;
  }
  static const std::map<std::string, std::string> ents = {
      {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}};
  out.clear();
  bool cdata = false;
  for (size_t j = 0; j < raw.size(); ++j) {
    if (raw[j] == '\x01' || raw[j] == '\x02') {
      cdata = raw[j] == '\x01';
      continue;
    }
    size_t semi = raw[j] == '&' && !cdata ? raw.find(';', j) : std::string::npos;
    if (semi == std::string::npos) {
      out += raw[j];
      continue;
    }
    std::string name = raw.substr(j + 1, semi - j - 1);
    auto it = ents.find(name);
    if (it != ents.end()) {
      out += it->second;
    } else if (name.size() > 1 && name[0] == '#') {
      unsigned long cp = name[1] == 'x' ? std::strtoul(name.c_str() + 2, nullptr, 16) : std::strtoul(name.c_str() + 1, nullptr, 10);
      if (cp < 0x80) {
        out += static_cast<char>(cp);
      } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      }
    } else {
      return false;
    }
    j = semi;
  }
  return true;
}

bool richEquals(QPDFObjectHandle rich, QPDFObjectHandle plain) {
  if (!plain.isString()) return false;
  std::string src = rich.isStream() ? streamText(rich) : rich.isString() ? rich.getUTF8Value() : std::string();
  std::string text;
  return (rich.isStream() || rich.isString()) && richText(src, text) && text == plain.getUTF8Value();
}

QPDFObjectHandle valueText(QPDFObjectHandle v) {
  if (v.isStream()) return QPDFObjectHandle::newUnicodeString(streamText(v));
  return v.isString() ? v : QPDFObjectHandle::newNull();
}
}

std::vector<UaFinding> uaMachineCheck(QPDF& pdf, int part) {
  std::vector<UaFinding> out;
  auto add = [&](const std::string& cp, const std::string& detail, std::set<int> pages = {}) {
    out.push_back({cp, detail, pages});
  };
  auto cp = [&](const char* ua1, const char* ua2) { return std::string(part == 2 ? ua2 : ua1); };
  QPDFObjectHandle root = pdf.getRoot();
  std::string xmp;
  QPDFObjectHandle md = root.getKey("/Metadata");
  if (md.isStream()) {
    try {
      auto b = md.getStreamData(qpdf_dl_all);
      xmp.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
    } catch (...) {
    }
  }
  bool uaId = false;
  bool uaRev = false;
  const std::string want = std::to_string(part);
  for (const PrefixUse& u : uaIdProps(xmp)) {
    if (u.prop == "part") {
      uaId = true;
      if (u.value != want) add("5-2", "pdfuaid:part is " + u.value + ", not " + want);
    }
    if (u.prop == "rev") {
      uaRev = true;
      if (part == 2 && (u.value.size() != 4 || u.value.find_first_not_of("0123456789") != std::string::npos)) {
        add("5-5", "pdfuaid:rev is not a four-digit year");
      }
    }
    if (u.prefix == "pdfuaid") continue;
    static const std::map<std::string, std::string> ua1Prefix = {{"part", "5-3"}, {"amd", "5-4"}, {"corr", "5-5"}};
    static const std::map<std::string, std::string> ua2Prefix = {{"part", "5-3"}, {"rev", "5-4"}};
    const auto& m = part == 2 ? ua2Prefix : ua1Prefix;
    auto it = m.find(u.prop);
    if (it != m.end()) {
      add(it->second, "PDF/UA identification property " + u.prop + " uses prefix " + u.prefix + " instead of pdfuaid");
    }
  }
  if (!uaId && md.isStream()) {
    add(cp("06-002", "5-1"), "the XMP metadata does not identify the file as PDF/UA-" + want + " (pdfuaid:part)");
  }
  if (part == 2 && uaId && !uaRev) add("5-5", "the XMP metadata has no pdfuaid:rev");
  if (!md.isStream()) add(cp("7.1-8", "8.11.1-2"), "the catalog has no metadata stream");
  if (md.isStream() && !titleHasValue(xmp)) add(cp("06-003", "8.11.1-1"), "the XMP metadata has no dc:title value");
  QPDFObjectHandle vp = root.getKey("/ViewerPreferences");
  if (!vp.isDictionary() || !vp.getKey("/DisplayDocTitle").isBool() || !vp.getKey("/DisplayDocTitle").getBoolValue()) {
    add(cp("07-001", "8.11.2-1"), "ViewerPreferences does not set DisplayDocTitle to true");
  }
  QPDFObjectHandle mi = root.getKey("/MarkInfo");
  if (!mi.isDictionary() || !mi.getKey("/Marked").isBool() || !mi.getKey("/Marked").getBoolValue()) {
    add(cp("01-002", "6.2-1"), "the document is not marked as tagged (MarkInfo /Marked)");
  }
  if (part == 1 && mi.isDictionary() && mi.getKey("/Suspects").isBool() && mi.getKey("/Suspects").getBoolValue()) {
    add("01-003", "MarkInfo /Suspects is true");
  }
  QPDFObjectHandle tree = root.getKey("/StructTreeRoot");
  std::map<long long, QPDFObjectHandle> parentTree;
  if (tree.isDictionary() && tree.getKey("/ParentTree").isDictionary()) {
    try {
      for (auto& kv : QPDFNumberTreeObjectHelper(tree.getKey("/ParentTree"), pdf)) {
        if (!kv.second.isNull()) parentTree[kv.first] = kv.second;
      }
    } catch (...) {
    }
  }
  std::map<QPDFObjGen, bool> reachCache;
  UaScanState st;
  st.part = part;
  st.parentTree = &parentTree;
  st.reaches = [&](QPDFObjectHandle e) {
    std::vector<QPDFObjGen> path;
    bool result = false;
    std::set<QPDFObjGen> seen;
    for (int i = 0; e.isDictionary() && i < 100000; ++i) {
      if (nameIs(e.getKey("/Type"), "/StructTreeRoot")) {
        result = true;
        break;
      }
      if (e.isIndirect()) {
        auto it = reachCache.find(e.getObjGen());
        if (it != reachCache.end()) {
          result = it->second;
          break;
        }
        if (!seen.insert(e.getObjGen()).second) break;
        path.push_back(e.getObjGen());
      }
      e = e.getKey("/P");
    }
    for (const QPDFObjGen& og : path) reachCache[og] = result;
    return result;
  };
  QPDFObjectHandle lang = root.getKey("/Lang");
  st.catalogLang = lang.isString() && !lang.getUTF8Value().empty();
  if (lang.isString() && !langValid(lang.getUTF8Value())) ++st.badLang;
  if (part == 2 && !lang.isString()) add("8.4.4-1", "the catalog has no /Lang entry");
  if (part == 1 && !st.catalogLang) {
    QPDFObjectHandle ol = root.getKey("/Outlines");
    if (ol.isDictionary() && ol.getKey("/First").isDictionary()) add("7.2-2", "outline entries have no natural language (no catalog /Lang)");
    if (onlyDefaultLangAlt(xmp)) add("7.2-33", "metadata text has only an x-default language and the catalog has no /Lang");
  }
  QPDFObjectHandle acro = root.getKey("/AcroForm");
  if (part == 1 && acro.isDictionary() && xfaDynamic(acro)) add("25-001", "the file contains a dynamic XFA form");
  if (part == 2 && acro.isDictionary() && acro.hasKey("/XFA")) add("8.10.1-3", "the file contains an XFA form");
  QPDFObjectHandle enc = pdf.getTrailer().getKey("/Encrypt");
  if (part == 1 && enc.isDictionary()) {
    if (!enc.getKey("/P").isInteger()) {
      add("26-002", "encryption dictionary has no integer /P permission flags");
    } else if (!(enc.getKey("/P").getIntValue() & 512)) {
      add("26-002", "encryption does not permit assistive technology access (P bit 10)");
    }
  }
  std::function<bool(QPDFObjectHandle)> structDest = [&](QPDFObjectHandle d) {
    if (d.isString()) {
      QPDFObjectHandle nm = root.getKey("/Names");
      if (!nm.isDictionary()) return false;
      QPDFObjectHandle t = nm.getKey("/Dests");
      if (t.isDictionary()) {
        QPDFObjectHandle found;
        try {
          QPDFNameTreeObjectHelper(t, pdf).findObject(d.getUTF8Value(), found);
        } catch (...) {
        }
        if (!found.isInitialized() || found.isNull()) return false;
        d = found;
      }
    } else if (d.isName()) {
      QPDFObjectHandle dd = root.getKey("/Dests");
      if (dd.isDictionary()) {
        if (dd.getKey(d.getName()).isNull()) return false;
        d = dd.getKey(d.getName());
      }
    }
    if (d.isDictionary()) return d.hasKey("/SD");
    return d.isArray() && d.getArrayNItems() > 0 && d.getArrayItem(0).isDictionary() && d.getArrayItem(0).hasKey("/S");
  };
  bool badGoTo = false;
  std::set<QPDFObjGen> actSeen;
  std::function<void(QPDFObjectHandle, int)> action = [&](QPDFObjectHandle a, int depth) {
    if (depth > kMaxObjectWalk) return;
    if (a.isArray()) {
      for (int i = 0; i < a.getArrayNItems(); ++i) action(a.getArrayItem(i), depth + 1);
      return;
    }
    if (!a.isDictionary() || (a.isIndirect() && !actSeen.insert(a.getObjGen()).second)) return;
    if (nameIs(a.getKey("/S"), "/GoTo") && !a.hasKey("/SD")) {
      QPDFObjectHandle d = a.getKey("/D");
      if (!((d.isName() || d.isString()) && structDest(d))) badGoTo = true;
    }
    action(a.getKey("/Next"), depth + 1);
  };
  auto actions = [&](QPDFObjectHandle aa) {
    if (!aa.isDictionary()) return;
    for (const std::string& k : aa.getKeys()) action(aa.getKey(k), 0);
  };
  bool plainDest = false;
  if (part == 2) {
    QPDFObjectHandle oa = root.getKey("/OpenAction");
    if (oa.isArray() && !structDest(oa)) plainDest = true;
    if (oa.isDictionary()) action(oa, 0);
    actions(root.getKey("/AA"));
    QPDFObjectHandle ol = root.getKey("/Outlines");
    std::vector<QPDFObjectHandle> stack;
    if (ol.isDictionary()) stack.push_back(ol.getKey("/First"));
    std::set<QPDFObjGen> visited;
    bool puaTitle = false;
    while (!stack.empty() && visited.size() < 100000) {
      QPDFObjectHandle it = stack.back();
      stack.pop_back();
      if (!it.isDictionary() || (it.isIndirect() && !visited.insert(it.getObjGen()).second)) continue;
      if (hasPua(it.getKey("/Title"))) puaTitle = true;
      if (!it.getKey("/Dest").isNull() && !structDest(it.getKey("/Dest"))) plainDest = true;
      action(it.getKey("/A"), 0);
      stack.push_back(it.getKey("/Next"));
      stack.push_back(it.getKey("/First"));
    }
    if (puaTitle) add("8.6-1", "an outline title uses Unicode private use area characters");
    QPDFObjectHandle nm = root.getKey("/Names");
    QPDFObjectHandle ef = nm.isDictionary() ? nm.getKey("/EmbeddedFiles") : QPDFObjectHandle::newNull();
    if (ef.isDictionary()) {
      try {
        for (auto& kv : QPDFNameTreeObjectHelper(ef, pdf)) {
          if (kv.second.isDictionary() && !kv.second.hasKey("/Desc")) {
            add("8.14.1-1", "an embedded file specification has no Desc entry");
            break;
          }
        }
      } catch (...) {
      }
    }
  }
  QPDFObjectHandle oc = root.getKey("/OCProperties");
  if (oc.isDictionary()) {
    std::vector<QPDFObjectHandle> cfgs{oc.getKey("/D")};
    QPDFObjectHandle more = oc.getKey("/Configs");
    bool hasConfigs = false;
    for (int i = 0; more.isArray() && i < more.getArrayNItems(); ++i) {
      cfgs.push_back(more.getArrayItem(i));
      hasConfigs = hasConfigs || more.getArrayItem(i).isDictionary();
    }
    for (QPDFObjectHandle c : cfgs) {
      if (!c.isDictionary()) continue;
      if ((part == 1 || hasConfigs) && textOf(c.getKey("/Name")).empty()) {
        add(cp("7.10-1", "8.7-1"), "an optional content configuration has no Name");
      }
      if (c.hasKey("/AS")) add(cp("7.10-2", "8.7-2"), "an optional content configuration contains an AS entry");
    }
  }
  if (part == 1) {
    std::set<std::string> objRules;
    auto objCheck = [&](QPDFObjectHandle o) {
      QPDFObjectHandle d = o.isStream() ? o.getDict() : o;
      if (d.getKey("/EF").isDictionary() && (textOf(d.getKey("/F")).empty() || textOf(d.getKey("/UF")).empty())) {
        objRules.insert("7.11-1");
      }
      if (nameIs(d.getKey("/Type"), "/MediaClip") && nameIs(d.getKey("/S"), "/MCD")) {
        if (!d.hasKey("/CT")) objRules.insert("7.18.6.2-1");
        QPDFObjectHandle alt = d.getKey("/Alt");
        bool good = false;
        for (int i = 1; alt.isArray() && alt.getArrayNItems() % 2 == 0 && i < alt.getArrayNItems(); i += 2) {
          if (!textOf(alt.getArrayItem(i)).empty()) good = true;
        }
        if (!good) objRules.insert("7.18.6.2-2");
      }
      if (o.isStream() && nameIs(d.getKey("/Subtype"), "/Form") && d.hasKey("/Ref")) objRules.insert("7.20-1");
    };
    for (QPDFObjectHandle o : pdf.getAllObjects()) forEachDirect(o, objCheck, 0);
    forEachDirect(pdf.getTrailer(), objCheck, 0);
    for (const std::string& r : objRules) {
      if (r == "7.11-1") add(r, "an embedded file specification lacks a non-empty F or UF entry");
      if (r == "7.18.6.2-1") add(r, "a media clip data dictionary has no CT entry");
      if (r == "7.18.6.2-2") add(r, "a media clip data dictionary has no usable Alt entry");
      if (r == "7.20-1") add(r, "the file contains a reference XObject");
    }
  }

  Types types(tree, part);
  QPDFObjectHandle classMap = tree.isDictionary() ? tree.getKey("/ClassMap") : QPDFObjectHandle::newNull();
  const std::string iso = part == 2 ? "ISO 14289-2" : "ISO 14289-1";
  std::map<QPDFObjGen, bool> langCache;
  auto chainLang = [&](QPDFObjectHandle e) {
    std::set<QPDFObjGen> seen;
    for (int i = 0; e.isDictionary() && i < kMaxStructDepth; ++i) {
      if (e.getKey("/Lang").isString()) return true;
      if (e.isIndirect() && !seen.insert(e.getObjGen()).second) return false;
      e = e.getKey("/P");
    }
    return false;
  };
  auto isArtifactElem = [&](QPDFObjectHandle e) {
    std::set<QPDFObjGen> seen;
    for (int i = 0; e.isDictionary() && i < kMaxStructDepth; ++i) {
      if (types.of(e) == "/Artifact") return true;
      if (e.isIndirect() && !seen.insert(e.getObjGen()).second) return false;
      e = e.getKey("/P");
    }
    return false;
  };
  if (!tree.isDictionary()) {
    add(cp("01-001", "8.2.1-1"), "the document has no structure tree");
  } else {
    std::map<std::string, std::set<int>> nonStd;
    int figures = 0, formulas = 0;
    std::vector<int> headings;
    bool numbered = false, plainH = false;
    std::set<std::string> rules;
    std::set<std::string> nesting;
    std::set<std::string> noteIds;
    std::set<QPDFObjGen> seen;
    std::map<QPDFObjGen, std::set<QPDFObjGen>> refsTo;
    std::vector<QPDFObjectHandle> notes;
    auto refKeys = [](QPDFObjectHandle e) {
      std::set<QPDFObjGen> out;
      QPDFObjectHandle ref = e.getKey("/Ref");
      for (int i = 0; ref.isArray() && i < ref.getArrayNItems(); ++i) {
        if (ref.getArrayItem(i).isIndirect()) out.insert(ref.getArrayItem(i).getObjGen());
      }
      return out;
    };
    auto joined = [](const std::vector<std::string>& ks) {
      std::string s;
      for (const std::string& t : ks) s += (s.empty() ? "" : "&") + t.substr(1);
      return s;
    };
    auto checkNesting = [&](const std::string& type, const std::string& ptype, const std::vector<std::string>& ks,
                            QPDFObjectHandle e) {
      const NestingTable& t5 = nestingTable();
      std::string ck = isHn(type) ? "/Hn" : type;
      std::string pk = isHn(ptype) ? "/Hn" : ptype;
      auto fp = t5.forbiddenParents.find(ck);
      if (fp != t5.forbiddenParents.end() && fp->second.count(pk)) {
        nesting.insert("8.2.5.1-1|" + pk.substr(1) + " contains " + type.substr(1));
      }
      auto kl = t5.kidLimits.find(ck);
      if (kl != t5.kidLimits.end()) {
        for (const auto& [kid, exact] : kl->second) {
          long long n = std::count(ks.begin(), ks.end(), kid);
          if (exact ? n != 1 : n > 1) {
            std::string how = exact ? " does not contain exactly one " : " contains more than one ";
            nesting.insert("8.2.5.1-2|" + type.substr(1) + how + kid.substr(1));
          }
        }
      }
      if (t5.noContent.count(ck) && types.hasContent(e)) nesting.insert("8.2.5.1-3|" + type.substr(1) + " directly contains content");
    };
    std::function<bool(QPDFObjectHandle, int)> containsRef = [&](QPDFObjectHandle e, int depth) {
      if (depth > kMaxStructDepth) return false;
      if (e.hasKey("/Ref")) return true;
      for (QPDFObjectHandle k : kidsOf(e)) {
        if (containsRef(k, depth + 1)) return true;
      }
      return false;
    };
    std::function<void(QPDFObjectHandle, QPDFObjGen, int)> walk = [&](QPDFObjectHandle e, QPDFObjGen pg, int depth) {
      if (depth > kMaxStructDepth || !e.isDictionary()) return;
      if (e.isIndirect() && !seen.insert(e.getObjGen()).second) return;
      std::string raw = nameOf(e.getKey("/S"));
      std::string type = types.of(e);
      std::string ptype = types.parentStd(e);
      std::vector<std::string> ks = types.kidTypes(e);
      QPDFObjectHandle el = e.getKey("/Lang");
      if (el.isString() && !langValid(el.getUTF8Value())) ++st.badLang;
      bool hasLang = chainLang(e);
      auto count = [&](const std::string& t) { return std::count(ks.begin(), ks.end(), t); };
      auto only = [&](std::set<std::string> allowed) {
        for (const std::string& t : ks) {
          if (!allowed.count(t)) return false;
        }
        return true;
      };
      auto captionNotFirst = [&]() {
        for (size_t i = 1; i < ks.size(); ++i) {
          if (ks[i] == "/Caption") return true;
        }
        return false;
      };
      if (types.circular(e)) rules.insert(cp("02-003", "8.2.4-2"));
      if (types.sameNamespaceMapping(e)) rules.insert("8.2.4-3");
      if (part == 1) {
        if (types.remapped17(e)) {
          add("02-004", "standard structure type " + raw + " is remapped in the role map");
        } else if (type.empty()) {
          nonStd[raw.empty() ? "(none)" : raw];
        }
        if (!st.catalogLang && !hasLang) {
          if (e.getKey("/ActualText").isString()) rules.insert("7.2-21");
          if (e.getKey("/Alt").isString()) rules.insert("7.2-22");
          if (e.getKey("/E").isString()) rules.insert("7.2-23");
        }
        bool hasAlt = !textOf(e.getKey("/Alt")).empty() || e.getKey("/ActualText").isString();
        if (type == "/Figure" && !hasAlt) ++figures;
        if (type == "/Formula" && !hasAlt) ++formulas;
        if (type == "/Note") {
          std::string id = textOf(e.getKey("/ID"));
          if (id.empty()) rules.insert("7.9-1");
          else if (!noteIds.insert(id).second) rules.insert("7.9-2");
        }
        if (type == "/H") plainH = true;
        if (isHn(type)) {
          numbered = true;
          headings.push_back(std::atoi(type.c_str() + 2));
        }
        if (count("/H") > 1) rules.insert("7.4.4-1");
        if (type == "/Form" && attr(e, "/Role", "/PrintField", 'n', classMap).isNull()) {
          std::vector<QPDFObjectHandle> all = allKids(e);
          bool one = all.size() == 1 && all[0].isDictionary() && nameIs(all[0].getKey("/Type"), "/OBJR") &&
                     nameIs(all[0].getKey("/Obj").getKey("/Subtype"), "/Widget");
          if (!one) rules.insert("7.18.4-2");
        }
        if (type == "/Table") {
          if (!only({"/TR", "/THead", "/TBody", "/TFoot", "/Caption"})) rules.insert("7.2-3");
          if (count("/THead") > 1) rules.insert("7.2-11");
          if (count("/TFoot") > 1) rules.insert("7.2-12");
          if (count("/TFoot") && !count("/TBody")) rules.insert("7.2-13");
          if (count("/THead") && !count("/TBody")) rules.insert("7.2-14");
          if (joined(ks).find("&Caption&") != std::string::npos) rules.insert("7.2-16");
          if (count("/Caption") > 1) rules.insert("7.2-39");
          checkTable(e, types, classMap, part, rules);
        }
        if (type == "/TR" && ptype != "/Table" && ptype != "/THead" && ptype != "/TBody" && ptype != "/TFoot") rules.insert("7.2-4");
        if ((type == "/THead" || type == "/TBody" || type == "/TFoot") && ptype != "/Table") rules.insert("7.2-5");
        if ((type == "/TH" || type == "/TD") && ptype != "/TR") rules.insert("7.2-8");
        if (type == "/TR" && !only({"/TH", "/TD"})) rules.insert("7.2-10");
        if ((type == "/THead" || type == "/TBody" || type == "/TFoot") && !only({"/TR"})) rules.insert("7.2-36");
        if (type == "/LI" && ptype != "/L") rules.insert("7.2-17");
        if (type == "/LBody" && ptype != "/LI") rules.insert("7.2-18");
        if (type == "/L" && !only({"/L", "/LI", "/Caption"})) rules.insert("7.2-19");
        if (type == "/L" && captionNotFirst()) rules.insert("7.2-40");
        if (type == "/LI" && !only({"/Lbl", "/LBody"})) rules.insert("7.2-20");
        if (type == "/TOCI" && ptype != "/TOC") rules.insert("7.2-26");
        if (type == "/TOC" && !only({"/TOC", "/TOCI", "/Caption"})) rules.insert("7.2-27");
        if (type == "/TOC" && captionNotFirst()) rules.insert("7.2-28");
      } else {
        if (e.isIndirect()) {
          for (const QPDFObjGen& og : refKeys(e)) refsTo[og].insert(e.getObjGen());
        }
        if (type == "/FENote") {
          if (e.isIndirect()) notes.push_back(e);
          std::string nt = nameOf(attr(e, "/NoteType", "/FENote", 'n', classMap));
          if (!nt.empty() && nt != "/Footnote" && nt != "/Endnote" && nt != "/None") rules.insert("8.2.5.14-4");
        }
        if (type.empty()) {
          if (types.ownStandard(e)) {
            add("8.2.4-4", "standard structure type " + raw + " is role-mapped away from its namespace");
          } else {
            nonStd[raw.empty() ? "(none)" : raw];
          }
        } else if (type == "/MathML") {
          if (ptype != "/Formula" && ptype != "/MathML") rules.insert("8.2.5.29-1");
        } else {
          checkNesting(type, ptype, ks, e);
          if (type == "/TOCI" && !containsRef(e, 0)) rules.insert("8.2.5.8-1");
          if (type == "/H") rules.insert("8.2.5.12-1");
          if (type == "/Note") rules.insert("8.2.5.14-1");
          if (type == "/Ruby" && joined(ks) != "RB&RT" && joined(ks) != "RB&RP&RT&RP") rules.insert("8.2.5.23-1");
          if (type == "/Warichu" && joined(ks) != "WP&WT&WP") rules.insert("8.2.5.24-1");
          if (type == "/L") {
            bool labels = false;
            for (QPDFObjectHandle li : types.sigKids(e)) {
              std::vector<std::string> lk = types.kidTypes(li);
              if (types.of(li) == "/LI" && std::count(lk.begin(), lk.end(), "/Lbl")) labels = true;
            }
            std::string numbering = "/None";
            QPDFObjectHandle cur = e;
            for (int i = 0; cur.isDictionary() && i < kMaxStructDepth; ++i) {
              QPDFObjectHandle v = attr(cur, "/ListNumbering", "/List", 'n', classMap);
              if (!v.isNull()) {
                numbering = v.getName();
                break;
              }
              cur = cur.getKey("/P");
            }
            if (labels && numbering == "/None") rules.insert("8.2.5.25-1");
          }
          if (type == "/LI" && types.hasContent(e)) rules.insert("8.2.5.25-2");
          if (type == "/Table") checkTable(e, types, classMap, part, rules);
          if (type == "/Figure" && !e.getKey("/Alt").isString() && !e.getKey("/ActualText").isString()) ++figures;
          if (type == "/Form") {
            int widgets = 0;
            for (QPDFObjectHandle kid : kidList(e)) {
              if (nameIs(kid.getKey("/Type"), "/OBJR") && nameIs(kid.getKey("/Obj").getKey("/Subtype"), "/Widget")) ++widgets;
            }
            if (widgets > 1) rules.insert("8.10.1-2");
          }
        }
        if (joined(ks).find("&Caption&") != std::string::npos) rules.insert("8.2.5.27-1");
        if (hasPua(e.getKey("/Alt"))) rules.insert("8.4.3-3");
        if (hasPua(e.getKey("/ActualText"))) rules.insert("8.4.3-2");
      }
      QPDFObjectHandle epg = e.getKey("/Pg");
      if (epg.isDictionary() && epg.isIndirect()) pg = epg.getObjGen();
      for (QPDFObjectHandle kid : kidList(e)) {
        if (kid.isInteger()) {
          st.mcidLang[{pg, static_cast<int>(kid.getIntValue())}] = hasLang;
        } else if (nameIs(kid.getKey("/Type"), "/MCR") && kid.getKey("/MCID").isInteger()) {
          QPDFObjGen ow = pg;
          if (kid.getKey("/Stm").isStream() && kid.getKey("/Stm").isIndirect()) ow = kid.getKey("/Stm").getObjGen();
          else if (kid.getKey("/Pg").isDictionary() && kid.getKey("/Pg").isIndirect()) ow = kid.getKey("/Pg").getObjGen();
          st.mcidLang[{ow, static_cast<int>(kid.getKey("/MCID").getIntValue())}] = hasLang;
        }
      }
      for (QPDFObjectHandle kid : kidsOf(e)) walk(kid, pg, depth + 1);
    };
    for (QPDFObjectHandle top : kidsOf(tree)) walk(top, QPDFObjGen(), 0);
    for (auto& kv : nonStd) {
      add(cp("02-001", "8.2.4-1"), "non-standard structure type " + kv.first + " is not role-mapped to a standard type");
    }
    if (figures) add(cp("13-004", "8.2.5.28.2-1"), std::to_string(figures) + " Figure element(s) without alternate text");
    if (formulas && part == 1) add("17-002", std::to_string(formulas) + " Formula element(s) without alternate text");
    if (part == 2) {
      std::vector<std::string> top = types.kidTypes(tree);
      std::vector<QPDFObjectHandle> topElems = types.sigKids(tree);
      if (joined(top) != "Document") {
        rules.insert("8.2.5.2-1");
      } else if (topElems.empty() || types.full(topElems[0]).second != kNs20) {
        rules.insert("8.2.5.2-2");
      }
      checkNesting("/StructTreeRoot", "", top, tree);
      for (QPDFObjectHandle n : notes) {
        std::set<QPDFObjGen> own = refKeys(n);
        const std::set<QPDFObjGen>& in = refsTo[n.getObjGen()];
        for (const QPDFObjGen& og : in) {
          if (!own.count(og)) rules.insert("8.2.5.14-2");
        }
        for (const QPDFObjGen& og : own) {
          if (!in.count(og)) rules.insert("8.2.5.14-3");
        }
      }
      for (const std::string& n : nesting) {
        size_t bar = n.find('|');
        add(n.substr(0, bar), "structure nesting (ISO/TS 32005 Table 5): " + n.substr(bar + 1));
      }
    } else {
      if (numbered && plainH) add("14-001", "the document mixes numbered and unnumbered headings");
      if (!headings.empty() && headings[0] != 1) add("14-002", "the first numbered heading is not H1");
      for (size_t i = 1; i < headings.size(); ++i) {
        if (headings[i] > headings[i - 1] + 1) {
          add("14-003", "heading level skipped (H" + std::to_string(headings[i - 1]) + " followed by H" + std::to_string(headings[i]) + ")");
          break;
        }
      }
    }
    for (const std::string& r : rules) add(r, "structure element rule " + r + " of " + iso + " is violated");
  }
  auto parentOf = [&](QPDFObjectHandle a) {
    QPDFObjectHandle sp = a.getKey("/StructParent");
    QPDFObjectHandle p = sp.isInteger() ? st.tree(sp.getIntValue()) : QPDFObjectHandle::newNull();
    return p.isDictionary() ? p : QPDFObjectHandle::newNull();
  };
  std::map<std::string, std::set<int>> annotRules;
  std::function<void(QPDFObjectHandle, bool, int)> fields = [&](QPDFObjectHandle f, bool top, int depth) {
    if (!f.isDictionary() || depth > 32 || (!top && !f.hasKey("/T"))) return;
    QPDFObjectHandle p = parentOf(f);
    if (part == 1 && f.getKey("/TU").isString() && !st.catalogLang && !(p.isDictionary() && p.getKey("/Lang").isString())) {
      annotRules["7.2-25"];
    }
    if (part == 2) {
      actions(f.getKey("/AA"));
      QPDFObjectHandle ft = f;
      for (int up = 0; !ft.hasKey("/FT") && ft.getKey("/Parent").isDictionary() && up < 32; ++up) ft = ft.getKey("/Parent");
      if (nameIs(ft.getKey("/FT"), "/Tx") && f.hasKey("/RV") && !richEquals(f.getKey("/RV"), valueText(f.getKey("/V")))) {
        annotRules["8.10.3.3-1"];
      }
    }
    QPDFObjectHandle kids = f.getKey("/Kids");
    for (int i = 0; kids.isArray() && i < kids.getArrayNItems(); ++i) fields(kids.getArrayItem(i), false, depth + 1);
  };
  QPDFObjectHandle allFields = acro.isDictionary() ? acro.getKey("/Fields") : QPDFObjectHandle::newNull();
  for (int i = 0; allFields.isArray() && i < allFields.getArrayNItems(); ++i) fields(allFields.getArrayItem(i), true, 0);
  QPDFPageDocumentHelper dh(pdf);
  std::set<int> untaggedPages, textLangPages;
  std::map<std::string, std::set<QPDFObjGen>> linkTargets;
  static const std::set<std::string> markup = {"/Caret", "/Circle", "/FreeText", "/Highlight", "/Line", "/Polygon",
                                               "/PolyLine", "/Redact", "/StrikeOut", "/Square", "/Squiggly", "/Text",
                                               "/Underline", "/Ink", "/FileAttachment", "/Stamp"};
  auto linkTarget = [&](QPDFObjectHandle a) {
    QPDFObjectHandle d = a;
    if (a.hasKey("/A")) {
      QPDFObjectHandle act = a.getKey("/A");
      if (act.isDictionary() && nameIs(act.getKey("/S"), "/GoTo")) d = act.hasKey("/SD") ? act.getKey("/SD") : act.getKey("/D");
    } else if (a.hasKey("/Dest")) {
      d = a.getKey("/Dest");
    }
    if (d.isString()) {
      QPDFObjectHandle nm = root.getKey("/Names");
      if (!nm.isDictionary()) return QPDFObjectHandle::newNull();
      QPDFObjectHandle t = nm.getKey("/Dests");
      if (t.isDictionary()) {
        QPDFObjectHandle found;
        try {
          QPDFNameTreeObjectHelper(t, pdf).findObject(d.getUTF8Value(), found);
        } catch (...) {
        }
        if (!found.isInitialized() || found.isNull()) return QPDFObjectHandle::newNull();
        d = found;
      }
    } else if (d.isName()) {
      QPDFObjectHandle dd = root.getKey("/Dests");
      if (dd.isDictionary()) {
        if (dd.getKey(d.getName()).isNull()) return QPDFObjectHandle::newNull();
        d = dd.getKey(d.getName());
      }
    }
    if (d.isDictionary()) return d.getKey("/SD");
    if (d.isArray() && d.getArrayNItems() > 0 && d.getArrayItem(0).isDictionary() && d.getArrayItem(0).hasKey("/S")) {
      return d.getArrayItem(0);
    }
    return QPDFObjectHandle::newNull();
  };
  int pageNo = 0;
  for (auto& ph : dh.getAllPages()) {
    ++pageNo;
    QPDFObjectHandle pobj = ph.getObjectHandle();
    QPDFObjectHandle res = ph.getAttribute("/Resources", false);
    long long before = st.untagged, beforeLang = st.textNoLang;
    QPDFObjGen pog = pobj.getObjGen();
    UntaggedScan scan(st, res, pobj, pog, pog, st.lookup(pobj, -1), false, false, 0);
    Pl_Discard d;
    try {
      ph.filterContents(&scan, &d);
    } catch (...) {
    }
    if (st.untagged > before) untaggedPages.insert(pageNo);
    if (st.textNoLang > beforeLang) textLangPages.insert(pageNo);
    if (part == 2) actions(pobj.getKey("/AA"));
    QPDFObjectHandle annots = pobj.getKey("/Annots");
    bool anyAnnots = annots.isArray() && annots.getArrayNItems();
    std::string tabs = nameOf(pobj.getKey("/Tabs"));
    if (anyAnnots && part == 1 && tabs != "/S") annotRules["7.18.3-1"].insert(pageNo);
    if (anyAnnots && part == 2 && tabs != "/S" && tabs != "/A" && tabs != "/W") annotRules["8.9.3.3-1"].insert(pageNo);
    QPDFObjectHandle box = ph.getAttribute("/CropBox", false);
    if (!box.isArray()) box = ph.getAttribute("/MediaBox", false);
    for (int i = 0; annots.isArray() && i < annots.getArrayNItems(); ++i) {
      QPDFObjectHandle a = annots.getArrayItem(i);
      if (!a.isDictionary()) continue;
      std::string sub = nameOf(a.getKey("/Subtype"));
      long long fl = a.getKey("/F").isInteger() ? a.getKey("/F").getIntValue() : 0;
      QPDFObjectHandle parent = parentOf(a);
      std::string pstd = parent.isDictionary() ? types.of(parent) : "";
      bool hasParentType = parent.isDictionary() && parent.getKey("/S").isName();
      QPDFObjectHandle altObj = parent.isDictionary() ? parent.getKey("/Alt") : QPDFObjectHandle::newNull();
      std::string alt = textOf(altObj);
      QPDFObjectHandle contentsObj = a.getKey("/Contents");
      bool hasContents = contentsObj.isString();
      std::string contents = textOf(contentsObj);
      auto mark = [&](const char* r) { annotRules[r].insert(pageNo); };
      if (part == 1) {
        if (hasContents && !st.catalogLang && !(parent.isDictionary() && parent.getKey("/Lang").isString())) mark("7.2-24");
        if ((fl & 2) || outsideCrop(a.getKey("/Rect"), box)) continue;
        if (sub == "/TrapNet") mark("7.18.2-1");
        if (sub == "/PrinterMark" && hasParentType) mark("7.18.8-1");
        if (sub != "/Widget" && contents.empty() && alt.empty()) mark("7.18.1-2");
        if (sub != "/Widget" && sub != "/PrinterMark" && sub != "/Link" && pstd != "/Annot") mark("7.18.1-1");
        if (sub == "/Widget") {
          if (pstd != "/Form") mark("7.18.4-1");
          std::string tu = textOf(a.hasKey("/T") ? a.getKey("/TU") : a.getKey("/Parent").getKey("/TU"));
          if (tu.empty() && alt.empty()) mark("7.18.1-3");
        }
        if (sub == "/Link") {
          if (pstd != "/Link") mark("7.18.5-1");
          if (contents.empty()) mark("7.18.5-2");
        }
        continue;
      }
      bool artifact = parent.isDictionary() && isArtifactElem(parent);
      bool real = hasParentType && !artifact;
      action(a.getKey("/A"), 0);
      actions(a.getKey("/AA"));
      if (real && (fl & 1)) mark("8.9.2.2-1");
      if (real && (fl & 32) && !(fl & 256)) mark("8.9.2.2-2");
      if (hasContents && altObj.isString() && contents != alt) mark("8.9.4.2-1");
      if (markup.count(sub)) {
        if (pstd != "/Annot") mark("8.9.2.3-1");
        if (a.hasKey("/RC") && hasContents && !richEquals(a.getKey("/RC"), contentsObj)) mark("8.9.2.3-2");
      }
      if (sub == "/Stamp" && !a.getKey("/Name").isName() && !hasContents) mark("8.9.2.4.7-1");
      if (sub == "/Ink" && !hasContents) mark("8.9.2.4.8-1");
      if (sub == "/Popup" && hasParentType) mark("8.9.2.4.9-1");
      if (sub == "/FileAttachment" && a.hasKey("/FS") && !a.getKey("/FS").getKey("/AFRelationship").isName()) mark("8.9.2.4.10-1");
      if (sub == "/Sound") mark("8.9.2.4.11-1");
      if (sub == "/Movie") mark("8.9.2.4.11-2");
      if (sub == "/Screen" && !hasContents) mark("8.9.2.4.12-1");
      if (sub == "/PrinterMark" && real) mark("8.9.2.4.14-1");
      if (sub == "/TrapNet") mark("8.9.2.4.15-1");
      if (sub == "/Watermark" && real && pstd != "/Annot") mark("8.9.2.4.16-1");
      if (sub == "/3D" && !hasContents) mark("8.9.2.4.19-1");
      if (sub == "/RichMedia" && !hasContents) mark("8.9.2.4.19-2");
      if (sub == "/Widget") {
        QPDFObjectHandle r = a.getKey("/Rect");
        auto v = [&](int k) { return r.isArray() && r.getArrayNItems() >= 4 ? numOf(r.getArrayItem(k), 0) : 1.0; };
        if (r.isArray() && r.getArrayNItems() >= 4 && v(2) - v(0) == 0 && v(3) - v(1) == 0 && real) mark("8.9.2.4.13-1");
        if (real && pstd != "/Form") mark("8.10.1-1");
        bool label = false;
        for (QPDFObjectHandle c : kidsOf(parent)) {
          if (types.of(c) == "/Lbl" && !allKids(c).empty()) label = true;
        }
        if (!label && !hasContents) mark("8.10.2.3-1");
        if (a.hasKey("/AA") && !hasContents) mark("8.10.2.3-2");
      }
      if (sub == "/Link") {
        if (pstd != "/Link" && pstd != "/Reference") mark("8.2.5.20-1");
        if (!a.hasKey("/A") && !a.getKey("/Dest").isNull() && !structDest(a.getKey("/Dest"))) plainDest = true;
        QPDFObjectHandle target = linkTarget(a);
        if (parent.isIndirect() && target.isIndirect()) {
          std::string key = target.getObjGen().unparse(' ');
          for (const auto& [k, owners] : linkTargets) {
            if (k != key && owners.count(parent.getObjGen())) mark("8.2.5.20-2");
          }
          linkTargets[key].insert(parent.getObjGen());
        }
      }
    }
  }
  if (part == 2 && plainDest) add("8.8-1", "a destination inside the document is not a structure destination");
  if (part == 2 && badGoTo) add("8.8-2", "a GoTo action points into the document without a structure destination");
  if (part == 1) {
    for (const auto& [og, n] : st.taggedFormDraws) {
      if (n > 1) {
        add("7.20-2", "a form XObject with tagged content is drawn more than once");
        break;
      }
    }
  }
  if (!untaggedPages.empty()) add(cp("01-005", "8.2.2-1"), "content is neither tagged nor marked as an artifact", untaggedPages);
  if (part == 1 && !textLangPages.empty()) add("7.2-34", "text has no natural language (no /Lang on it, its structure or the catalog)", textLangPages);
  if (part == 1 && st.spanNoLang) add("7.2-30", "a Span with alternate text has no natural language");
  if (st.badLang) add(cp("7.2-29", "8.4.4-2"), std::to_string(st.badLang) + " /Lang value(s) are not valid language tags");
  if (st.puaAlt) add("8.4.3-3", "an Alt entry uses Unicode private use area characters");
  if (st.puaGlyph) add("8.4.3-1", "text whose characters map to Unicode private use area values has no ActualText or Alt");
  if (st.puaActual) add("8.4.3-2", "an ActualText entry uses Unicode private use area characters");
  for (const auto& [rule, pages] : annotRules) add(rule, "annotation or form rule " + rule + " of " + iso + " is violated", pages);
  if (part == 1 && st.nested) add("01-004", std::to_string(st.nested) + " tagged content sequence(s) nested inside an artifact, or the reverse");
  return out;
}

std::vector<UaFinding> uaCheck(const unsigned char* data, size_t size, const std::string& password, int part) {
  QPDF q;
  q.setSuppressWarnings(true);
  q.processMemoryFile("ua", reinterpret_cast<const char*>(data), size, password.c_str());
  std::vector<UaFinding> out = uaMachineCheck(q, part);
  if (part == 1) {
    std::string head(reinterpret_cast<const char*>(data), std::min<size_t>(size, 1024));
    size_t at = head.find("%PDF-");
    size_t eol = at == std::string::npos ? at : head.find_first_of("\r\n", at);
    std::string line =
        at == std::string::npos ? std::string() : head.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
    if (line.size() != 8 || line.compare(0, 7, "%PDF-1.") != 0 || line[7] < '0' || line[7] > '7') {
      out.push_back({"6.1-1", "the file header is not %PDF-1.0 to %PDF-1.7", {}});
    }
  }
  static const std::map<std::string, std::pair<std::string, std::string>> fontRules = {
      {"CMAP_ROS_SYNCED", {"7.21.3.1", "8.4.5.3.1"}},        {"CIDSYSTEMINFO_SYNCED", {"7.21.3.1", "8.4.5.3.1"}},
      {"CIDTOGIDMAP_SET", {"7.21.3.2", "8.4.5.3.2"}},        {"CMAP_FALLBACK_EMBEDDED", {"7.21.3.3", "8.4.5.4"}},
      {"CMAP_WMODE_SYNCED", {"7.21.3.3", "8.4.5.4"}},        {"CMAP_USECMAP_REMOVED", {"7.21.3.3", "8.4.5.4"}},
      {"FONT_SUBSTITUTED", {"7.21.4.1", "8.4.5.5.1"}},       {"FONT_NOT_EMBEDDED", {"7.21.4.1", "8.4.5.5.1"}},
      {"FONT_FSTYPE_RESTRICTED", {"7.21.4.1", "8.4.5.5.1"}},
      {"CHARSET_ADDED", {"7.21.4.2", ""}},                   {"CIDSET_ADDED", {"7.21.4.2", ""}},
      {"FONT_WIDTHS_SYNCED", {"7.21.5", "8.4.5.6"}},         {"TYPE3_WIDTHS_SYNCED", {"7.21.5", "8.4.5.6"}},
      {"TTF_ENCODING_SET", {"7.21.6", "8.4.5.7"}},           {"ENCODING_DIFFS_CLEANED", {"7.21.6", "8.4.5.7"}},
      {"SYMBOLIC_ENCODING_REMOVED", {"7.21.6", "8.4.5.7"}},  {"FONT_CMAP_ADDED", {"", "8.4.5.7"}},
      {"SYMBOLIC_FLAG_CLEARED", {"7.21.6", "8.4.5.7"}},      {"TT_DIFFERENCES_REMOVED", {"7.21.6", "8.4.5.7"}},
      {"FONT_PROGRAM_UNUSABLE", {"7.21.6", "8.4.5.7"}},      {"FONT_PROGRAM_UNREADABLE", {"7.21.6", "8.4.5.7"}},
      {"TOUNICODE_BUILT", {"7.21.7", "8.4.5.8"}},            {"TOUNICODE_SCRUBBED", {"7.21.7", "8.4.5.8"}},
      {"MISSING_GLYPHS_REMOVED", {"7.21.8", "8.4.5.9"}}};
  Options o;
  o.level = part == 2 ? Level::A4 : Level::A2U;
  o.ua = true;
  o.verifyOnly = true;
  o.password = password;
  Result r = convert(data, size, o);
  for (const Issue& i : r.issues) {
    auto it = fontRules.find(i.code);
    if (it == fontRules.end() || (!i.fixed && i.code != "FONT_NOT_EMBEDDED")) continue;
    const std::string& rule = part == 2 ? it->second.second : it->second.first;
    if (!rule.empty()) out.push_back({rule, i.detail, std::set<int>(i.pages.begin(), i.pages.end())});
  }
  return out;
}
}
