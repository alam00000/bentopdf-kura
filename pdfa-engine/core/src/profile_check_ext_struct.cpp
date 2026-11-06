#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "colorconv.hh"
#include "profile_ext.hh"
#include "textextract.hh"
#include "util.hh"

namespace pdfa {
const std::map<uint32_t, std::u32string>& toUnicodeMap(const FontFacts& f, const Events& ev);

namespace {
struct Shade {
  int page = -1;
  double dpi = 0, scale = 1;
  bool ok = false;
  int w = 0, h = 0, rot = 0;
  Box crop;
  std::string rgb;
};

struct Kid {
  int elem = -1;
  long long mcid = -1;
  int page = -1;
  bool pageMissing = false;
  QPDFObjectHandle obj;
  bool objr = false;
};

struct Elem {
  QPDFObjectHandle d;
  std::string raw, type;
  int parent = -1;
  std::vector<Kid> kids;
  std::string lang;
};

struct Tagging {
  bool root = false;
  std::vector<Elem> elems;
  std::vector<int> order;
  std::vector<std::string> tree;
  std::vector<std::string> roleProblems;
  std::map<QPDFObjGen, int> objrParent;
  std::set<std::pair<int, long long>> referenced;
  std::map<std::pair<int, long long>, int> mcidOwner;
  std::string docLang;
  mutable Shade shade;
  mutable std::map<int, std::vector<TextRun>> runs;
};

std::string nm(QPDFObjectHandle o) { return o.isName() ? o.getName().substr(1) : std::string(); }

std::string text(QPDFObjectHandle o) { return o.isString() ? o.getUTF8Value() : std::string(); }

bool standardType(const std::string& t, bool pdf2) {
  static const std::set<std::string> v17 = {
      "Document", "Part", "Art", "Sect", "Div", "BlockQuote", "Caption", "TOC", "TOCI", "Index", "NonStruct",
      "Private", "P", "H", "H1", "H2", "H3", "H4", "H5", "H6", "L", "LI", "Lbl", "LBody", "Table", "TR", "TH",
      "TD", "THead", "TBody", "TFoot", "Span", "Quote", "Note", "Reference", "BibEntry", "Code", "Link", "Annot",
      "Ruby", "RB", "RT", "RP", "Warichu", "WT", "WP", "Figure", "Formula", "Form"};
  static const std::set<std::string> v20 = {
      "Document", "DocumentFragment", "Part", "Sect", "Div", "Aside", "NonStruct", "P", "H", "Title", "FENote",
      "Sub", "Lbl", "Span", "Em", "Strong", "Link", "Annot", "Form", "Ruby", "RB", "RT", "RP", "Warichu", "WT",
      "WP", "L", "LI", "LBody", "Table", "TR", "TH", "TD", "THead", "TBody", "TFoot", "Caption", "Figure",
      "Formula", "Artifact"};
  if (pdf2) {
    static const std::regex hn("H[1-9][0-9]*");
    return v20.count(t) || std::regex_match(t, hn);
  }
  return v17.count(t) > 0;
}

int headingLevel(const std::string& t) {
  if (t.size() < 2 || t[0] != 'H' || t.find_first_not_of("0123456789", 1) != std::string::npos) return 0;
  return std::atoi(t.c_str() + 1);
}

bool blockType(const std::string& t) {
  static const std::set<std::string> b = {"P", "H", "L", "LI", "Lbl", "LBody", "Table", "TR", "TH", "TD", "THead",
                                          "TBody", "TFoot", "BlockQuote", "Caption", "TOC", "TOCI", "Index"};
  return b.count(t) || headingLevel(t) > 0;
}

std::string resolveRole(const std::string& raw, QPDFObjectHandle roleMap, bool& circular) {
  std::string t = raw;
  std::set<std::string> seen;
  while (!standardType(t, false) && roleMap.isDictionary()) {
    if (!seen.insert(t).second) {
      circular = true;
      return std::string();
    }
    QPDFObjectHandle m = roleMap.getKey("/" + t);
    if (!m.isName()) break;
    t = m.getName().substr(1);
  }
  return t;
}

Tagging build(const Events& ev) {
  Tagging tg;
  QPDFObjectHandle root = ev.root.isDictionary() ? ev.root.getKey("/StructTreeRoot") : QPDFObjectHandle();
  tg.docLang = ev.root.isDictionary() ? text(ev.root.getKey("/Lang")) : std::string();
  QPDFObjectHandle mi = ev.root.isDictionary() ? ev.root.getKey("/MarkInfo") : QPDFObjectHandle();
  QPDFObjectHandle sus = mi.isDictionary() ? mi.getKey("/Suspects") : QPDFObjectHandle();
  if (sus.isBool() && sus.getBoolValue()) tg.tree.push_back("suspects_true");
  if (!root.isDictionary()) {
    tg.tree.push_back("missing_root");
    return tg;
  }
  tg.root = true;
  std::map<QPDFObjGen, int> pageIdx;
  for (size_t i = 0; i < ev.pages.size(); ++i) {
    if (ev.pages[i].obj.isIndirect()) pageIdx[ev.pages[i].obj.getObjGen()] = static_cast<int>(i);
  }
  QPDFObjectHandle roleMap = root.getKey("/RoleMap");
  QPDFObjectHandle classMap = root.getKey("/ClassMap");
  auto problem = [&](const char* v) {
    if (tg.tree.size() < 10000) tg.tree.push_back(v);
  };
  std::set<QPDFObjGen> seen;
  std::function<void(QPDFObjectHandle, int, QPDFObjectHandle, int)> walkK;
  std::function<int(QPDFObjectHandle, int, QPDFObjectHandle, int)> addElem = [&](QPDFObjectHandle d, int parent,
                                                                                 QPDFObjectHandle pg, int depth) {
    if (d.isIndirect() && !seen.insert(d.getObjGen()).second) return -1;
    if (tg.elems.size() > 200000 || depth > 256) return -1;
    Elem e;
    e.d = d;
    e.parent = parent;
    e.raw = nm(d.getKey("/S"));
    if (!d.getKey("/S").isName()) problem("element_without_type");
    bool circ = false;
    e.type = resolveRole(e.raw, roleMap, circ);
    e.lang = text(d.getKey("/Lang"));
    QPDFObjectHandle c = d.getKey("/C");
    if (d.hasKey("/C")) {
      std::vector<QPDFObjectHandle> cls;
      if (c.isName()) {
        cls.push_back(c);
      } else if (c.isArray()) {
        for (int i = 0; i < c.getArrayNItems(); ++i) {
          QPDFObjectHandle it = c.getArrayItem(i);
          if (it.isName()) cls.push_back(it);
          else if (!it.isInteger()) problem("malformed_class_array");
        }
      } else {
        problem("malformed_class_array");
      }
      for (QPDFObjectHandle n : cls) {
        if (!classMap.isDictionary() || !classMap.hasKey(n.getName())) problem("invalid_class");
      }
    }
    QPDFObjectHandle own = d.getKey("/Pg");
    if (d.hasKey("/Pg") && !own.isDictionary()) problem("page_key_wrong_type");
    if (own.isDictionary()) pg = own;
    int me = static_cast<int>(tg.elems.size());
    tg.elems.push_back(e);
    tg.order.push_back(me);
    walkK(d.getKey("/K"), me, pg, depth + 1);
    return me;
  };
  auto pageOf = [&](QPDFObjectHandle pg) {
    if (!pg.isDictionary() || !pg.isIndirect()) return -1;
    auto it = pageIdx.find(pg.getObjGen());
    return it == pageIdx.end() ? -1 : it->second;
  };
  walkK = [&](QPDFObjectHandle k, int parent, QPDFObjectHandle pg, int depth) {
    if (depth > 256) return;
    if (k.isArray()) {
      for (int i = 0; i < k.getArrayNItems(); ++i) walkK(k.getArrayItem(i), parent, pg, depth + 1);
      return;
    }
    Kid kid;
    if (k.isInteger()) {
      kid.mcid = k.getIntValue();
      kid.page = pageOf(pg);
      kid.pageMissing = !pg.isDictionary();
    } else if (k.isDictionary()) {
      std::string t = nm(k.getKey("/Type"));
      QPDFObjectHandle kpg = k.getKey("/Pg");
      if (k.hasKey("/Pg") && !kpg.isDictionary()) problem("page_key_wrong_type");
      if (kpg.isDictionary()) pg = kpg;
      if (t == "MCR" || (!k.hasKey("/S") && k.getKey("/MCID").isInteger())) {
        kid.mcid = k.getKey("/MCID").isInteger() ? k.getKey("/MCID").getIntValue() : -1;
        kid.page = pageOf(pg);
        kid.pageMissing = !pg.isDictionary() && !k.getKey("/Stm").isStream();
        if (k.getKey("/Stm").isStream()) kid.page = -2;
      } else if (t == "OBJR") {
        kid.objr = true;
        kid.obj = k.getKey("/Obj");
      } else if (parent >= 0 || k.hasKey("/S")) {
        kid.elem = addElem(k, parent, pg, depth);
        if (kid.elem < 0) return;
      } else {
        return;
      }
    } else {
      return;
    }
    if (parent >= 0) tg.elems[static_cast<size_t>(parent)].kids.push_back(kid);
  };
  walkK(root.getKey("/K"), -1, QPDFObjectHandle(), 0);
  const auto& mcids = pageMarkedContentIds(ev);
  for (size_t i = 0; i < tg.elems.size(); ++i) {
    for (const Kid& k : tg.elems[i].kids) {
      if (k.objr) {
        if (!k.obj.isDictionary() && !k.obj.isStream()) {
          problem("reference_to_object_without_marked_content");
          continue;
        }
        QPDFObjectHandle od = k.obj.isStream() ? k.obj.getDict() : k.obj;
        if (k.obj.isIndirect()) tg.objrParent[k.obj.getObjGen()] = static_cast<int>(i);
        if (!od.getKey("/StructParent").isInteger()) problem("reference_to_object_without_parent_key");
        continue;
      }
      if (k.mcid < 0) continue;
      if (k.pageMissing) {
        problem("marked_content_reference_without_page");
        continue;
      }
      if (k.page < 0) continue;
      tg.referenced.insert({k.page, k.mcid});
      tg.mcidOwner[{k.page, k.mcid}] = static_cast<int>(i);
      auto pm = mcids.find(k.page);
      if (pm == mcids.end() || !pm->second.count(k.mcid)) problem("dangling_marked_content_reference");
    }
  }
  std::map<long long, QPDFObjectHandle> parents;
  std::function<void(QPDFObjectHandle, int)> nums = [&](QPDFObjectHandle n, int depth) {
    if (!n.isDictionary() || depth > 32) return;
    QPDFObjectHandle a = n.getKey("/Nums");
    for (int i = 0; a.isArray() && i + 1 < a.getArrayNItems(); i += 2) {
      if (a.getArrayItem(i).isInteger()) parents[a.getArrayItem(i).getIntValue()] = a.getArrayItem(i + 1);
    }
    QPDFObjectHandle kids = n.getKey("/Kids");
    for (int i = 0; kids.isArray() && i < kids.getArrayNItems(); ++i) nums(kids.getArrayItem(i), depth + 1);
  };
  nums(root.getKey("/ParentTree"), 0);
  auto elemIndex = [&](QPDFObjectHandle d) {
    for (size_t i = 0; d.isDictionary() && i < tg.elems.size(); ++i) {
      if (tg.elems[i].d.isSameObjectAs(d)) return static_cast<int>(i);
    }
    return -1;
  };
  for (size_t p = 0; p < ev.pages.size(); ++p) {
    auto pm = mcids.find(static_cast<int>(p));
    if (pm == mcids.end()) continue;
    QPDFObjectHandle sp = ev.pages[p].obj.getKey("/StructParents");
    QPDFObjectHandle arr;
    if (sp.isInteger() && parents.count(sp.getIntValue())) arr = parents[sp.getIntValue()];
    else problem("missing_parent_tree_entry");
    for (const auto& kv : pm->second) {
      if (kv.second > 1) problem("duplicate_mcid");
      if (!tg.referenced.count({static_cast<int>(p), kv.first})) problem("unreferenced_marked_content");
      if (!arr.isArray()) continue;
      QPDFObjectHandle e = kv.first < arr.getArrayNItems() ? arr.getArrayItem(static_cast<int>(kv.first)) : QPDFObjectHandle();
      if (!e.isDictionary()) {
        problem("marked_content_without_parent");
        continue;
      }
      auto own = tg.mcidOwner.find({static_cast<int>(p), kv.first});
      if (own == tg.mcidOwner.end() || !tg.elems[static_cast<size_t>(own->second)].d.isSameObjectAs(e)) problem("inconsistent_parent_tree");
    }
  }
  for (const auto& kv : tg.objrParent) {
    QPDFObjectHandle o = ev.root.getOwningQPDF()->getObject(kv.first);
    QPDFObjectHandle od = o.isStream() ? o.getDict() : o;
    QPDFObjectHandle spk = od.isDictionary() ? od.getKey("/StructParent") : QPDFObjectHandle();
    if (!spk.isInteger()) continue;
    auto it = parents.find(spk.getIntValue());
    if (it == parents.end()) problem("missing_parent_tree_entry");
    else if (elemIndex(it->second) != kv.second) problem("inconsistent_parent_tree");
  }
  for (const AnnotFacts& an : ev.annots) {
    if (an.obj.isDictionary() && an.obj.getKey("/StructParent").isInteger() && an.obj.isIndirect() &&
        !tg.objrParent.count(an.obj.getObjGen())) {
      problem("object_reference_without_struct_parent");
    }
  }
  if (QPDF* q = ev.root.getOwningQPDF()) {
    for (QPDFObjectHandle o : q->getAllObjects()) {
      if (!o.isDictionary() || !o.isIndirect() || seen.count(o.getObjGen())) continue;
      std::string t = nm(o.getKey("/Type"));
      if (t == "StructElem" || (t.empty() && o.getKey("/S").isName() && o.getKey("/P").isDictionary())) problem("unreferenced_element");
    }
  }
  for (const std::string& k : roleMap.isDictionary() ? roleMap.getKeys() : std::set<std::string>()) {
    std::string t = k.substr(1);
    std::set<std::string> s;
    while (roleMap.getKey("/" + t).isName() && s.insert(t).second) t = roleMap.getKey("/" + t).getName().substr(1);
    if (s.count(t)) tg.roleProblems.push_back("circular_role_map");
  }
  return tg;
}

const Tagging& tagging(const Events& ev) {
  std::shared_ptr<void>& slot = extTaggingSlot(ev);
  if (!slot) slot = std::make_shared<Tagging>(build(ev));
  return *static_cast<Tagging*>(slot.get());
}

std::string langOf(const Tagging& tg, int i) {
  while (i >= 0) {
    if (!tg.elems[static_cast<size_t>(i)].lang.empty()) return tg.elems[static_cast<size_t>(i)].lang;
    i = tg.elems[static_cast<size_t>(i)].parent;
  }
  return tg.docLang;
}

std::vector<int> kidsOf(const Tagging& tg, int i) {
  std::vector<int> out;
  for (const Kid& k : tg.elems[static_cast<size_t>(i)].kids) {
    if (k.elem >= 0) out.push_back(k.elem);
  }
  return out;
}

bool hasDescendant(const Tagging& tg, int i, const std::string& type) {
  for (int k : kidsOf(tg, i)) {
    if (tg.elems[static_cast<size_t>(k)].type == type || hasDescendant(tg, k, type)) return true;
  }
  return false;
}

bool onlyStandard(const Tagging& tg, int i) {
  for (int k : kidsOf(tg, i)) {
    if (!standardType(tg.elems[static_cast<size_t>(k)].raw, false) || !onlyStandard(tg, k)) return false;
  }
  return true;
}

bool hasContent(const Tagging& tg, int i) {
  for (const Kid& k : tg.elems[static_cast<size_t>(i)].kids) {
    if (k.mcid >= 0 || k.objr || (k.elem >= 0 && hasContent(tg, k.elem))) return true;
  }
  return false;
}

QPDFObjectHandle attribute(QPDFObjectHandle d, const char* owner, const char* key) {
  QPDFObjectHandle a = d.getKey("/A");
  std::vector<QPDFObjectHandle> list;
  if (a.isDictionary()) list.push_back(a);
  for (int i = 0; a.isArray() && i < a.getArrayNItems(); ++i) {
    if (a.getArrayItem(i).isDictionary()) list.push_back(a.getArrayItem(i));
  }
  for (QPDFObjectHandle x : list) {
    if ((!owner || nameIs(x.getKey("/O"), owner)) && x.hasKey(key)) return x.getKey(key);
  }
  return QPDFObjectHandle();
}

std::string tf(bool b) { return b ? "true" : "false"; }

bool filled(QPDFObjectHandle o) { return o.isString() && !o.getUTF8Value().empty(); }

bool shown(const AnnotFacts& an, const Events& ev) {
  QPDFObjectHandle f = an.obj.getKey("/F");
  if (f.isInteger() && (f.getIntValue() & 2)) return false;
  const PageFacts* p = pageFor(ev, an.page);
  if (!p || !an.rect.valid) return true;
  const Box& area = p->crop.valid ? p->crop : p->media;
  return !area.valid || boxesIntersect(area, an.rect);
}

int evalTree(const PfAtom& a, const Subject&, const Events& ev, bool&) { return compare(Value(tagging(ev).tree), a); }

int evalHeadings(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const Tagging& tg = tagging(ev);
  if (!tg.root) return 0;
  std::vector<std::string> v;
  std::vector<int> levels;
  bool plain = false;
  for (int i : tg.order) {
    const std::string& t = tg.elems[static_cast<size_t>(i)].type;
    if (t == "H") plain = true;
    if (int n = headingLevel(t)) levels.push_back(n);
  }
  if (!levels.empty()) v.push_back("numbered");
  if (!levels.empty() && plain) v.push_back("both_h_and_numbered");
  if (!levels.empty() && levels.front() != 1) v.push_back("first_not_h1");
  for (size_t i = 1; i < levels.size(); ++i) {
    if (levels[i] > levels[i - 1] + 1) v.push_back("skipped_levels");
  }
  for (size_t i = 0; i < tg.elems.size(); ++i) {
    int h = 0;
    int prev = 0;
    for (int k : kidsOf(tg, static_cast<int>(i))) {
      const std::string& t = tg.elems[static_cast<size_t>(k)].type;
      h += t == "H";
      int n = headingLevel(t);
      if (n && n == prev) v.push_back("adjacent_same_level");
      prev = n;
    }
    if (h > 1) v.push_back("several_h_in_node");
  }
  return compare(Value(v), a);
}

int tocDepth(const Tagging& tg, int i) {
  int best = 0;
  for (int k : kidsOf(tg, i)) {
    int d = tocDepth(tg, k);
    if (tg.elems[static_cast<size_t>(k)].type == "TOC") d += 1;
    best = std::max(best, d);
  }
  return best;
}

int evalHeadingLevel(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const Tagging& tg = tagging(ev);
  if (!tg.root) return 0;
  int highest = 0;
  bool toc = false;
  int depth = 0;
  for (size_t i = 0; i < tg.elems.size(); ++i) {
    highest = std::max(highest, headingLevel(tg.elems[i].type));
    if (tg.elems[i].type == "TOC") {
      int p = tg.elems[i].parent;
      bool top = true;
      while (p >= 0) {
        if (tg.elems[static_cast<size_t>(p)].type == "TOC") top = false;
        p = tg.elems[static_cast<size_t>(p)].parent;
      }
      if (top) {
        toc = true;
        depth = std::max(depth, 1 + tocDepth(tg, static_cast<int>(i)));
      }
    }
  }
  if (param(a, "measure") == "toc_difference") return toc ? compare(Value(std::abs(highest - depth)), a) : 0;
  return compare(Value(highest), a);
}

bool privateUse(const std::string& s) {
  for (size_t i = 0; i + 2 < s.size(); ++i) {
    unsigned char c0 = static_cast<unsigned char>(s[i]), c1 = static_cast<unsigned char>(s[i + 1]);
    if (c0 == 0xEE || (c0 == 0xEF && c1 < 0xA4)) return true;
  }
  return false;
}

std::string lowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string trimmed(const std::string& s, const char* strip) {
  size_t b = s.find_first_not_of(strip), e = s.find_last_not_of(strip);
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

const char* kSpacePunct = " \t\r\n.,:;!?()[]{}\"'*-_/|<>";

bool placeholderAlt(const std::string& alt, const std::vector<std::string>& words) {
  std::string s = trimmed(lowerAscii(alt), kSpacePunct);
  if (s.empty()) return true;
  static const std::regex file(R"([^\s/\\]+\.(jpe?g|png|gif|bmp|tiff?|svg|webp|emf|wmf|eps|heic))");
  if (std::regex_match(s, file)) return true;
  s = trimmed(s.substr(0, s.find_last_not_of("0123456789 _-#") + 1), kSpacePunct);
  for (const std::string& w : words) {
    if (!w.empty() && trimmed(lowerAscii(w), kSpacePunct) == s) return true;
  }
  return false;
}

int evalAlternateText(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const Tagging& tg = tagging(ev);
  std::vector<std::string> v;
  std::vector<std::string> words = paramList(a, "placeholders");
  for (const Elem& e : tg.elems) {
    bool alt = filled(e.d.getKey("/Alt")), actual = filled(e.d.getKey("/ActualText"));
    if (alt && placeholderAlt(text(e.d.getKey("/Alt")), words)) v.push_back("placeholder_alt");
    if (e.type == "Figure" && !alt && !actual) v.push_back("figure_without_alt");
    if (e.type == "Figure" && actual) v.push_back("figure_with_actual_text");
    if (e.type == "Formula" && !alt) v.push_back("formula_without_alt");
    if (actual && privateUse(text(e.d.getKey("/ActualText")))) v.push_back("actual_text_private_use");
  }
  for (const AnnotFacts& an : ev.annots) {
    if (!an.obj.isDictionary()) continue;
    int parent = -1;
    if (an.obj.isIndirect()) {
      auto it = tg.objrParent.find(an.obj.getObjGen());
      if (it != tg.objrParent.end()) parent = it->second;
    }
    if (!shown(an, ev)) continue;
    bool elemAlt = parent >= 0 && filled(tg.elems[static_cast<size_t>(parent)].d.getKey("/Alt"));
    bool contents = filled(an.obj.getKey("/Contents"));
    if (an.subtype == "Link") {
      if (!contents) v.push_back("link_without_contents");
    } else if (an.subtype == "Widget") {
      QPDFObjectHandle f = an.obj;
      bool tu = false;
      for (int d = 0; f.isDictionary() && d < 32 && !tu; ++d, f = f.getKey("/Parent")) tu = filled(f.getKey("/TU"));
      if (!tu && !elemAlt) v.push_back("form_field_without_alt");
    } else if (an.subtype != "Popup" && an.subtype != "PrinterMark" && an.subtype != "TrapNet") {
      if (!contents && !elemAlt) v.push_back("annotation_without_alt");
    }
  }
  return compare(Value(v), a);
}

int evalNesting(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const Tagging& tg = tagging(ev);
  if (!tg.root) return 0;
  std::vector<std::string> v;
  for (const AnnotFacts& an : ev.annots) {
    if (!an.obj.isDictionary() || an.subtype == "Popup" || !shown(an, ev)) continue;
    int parent = -1;
    if (an.obj.isIndirect()) {
      auto it = tg.objrParent.find(an.obj.getObjGen());
      if (it != tg.objrParent.end()) parent = it->second;
    }
    std::string pt = parent >= 0 ? tg.elems[static_cast<size_t>(parent)].type : std::string();
    if (an.subtype == "PrinterMark") {
      if (parent >= 0) v.push_back("printer_mark_in_structure");
      if (parent >= 0 || an.obj.hasKey("/StructParent")) v.push_back("printer_mark_not_artifact");
      continue;
    }
    if (an.subtype == "TrapNet") continue;
    if (an.subtype == "Link") {
      if (pt != "Link") v.push_back("link_not_in_link");
      if (!an.obj.getKey("/StructParent").isInteger()) v.push_back("link_without_struct_parent");
    } else if (an.subtype == "Widget") {
      if (pt != "Form") v.push_back("field_not_in_form");
    } else if (pt != "Annot") {
      v.push_back("annotation_not_in_annot");
    }
  }
  return compare(Value(v), a);
}

int evalNotes(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const Tagging& tg = tagging(ev);
  if (!tg.root) return 0;
  std::vector<std::string> v;
  std::map<std::string, int> ids;
  for (const Elem& e : tg.elems) {
    if (e.type != "Note" && e.type != "FENote") continue;
    QPDFObjectHandle id = e.d.getKey("/ID");
    if (!id.isString() || id.getStringValue().empty()) v.push_back("missing_id");
    else if (ids[id.getStringValue()]++) v.push_back("duplicate_id");
  }
  return compare(Value(v), a);
}

int evalStandardType(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const Tagging& tg = tagging(ev);
  if (!tg.root) return 0;
  bool pdf2 = param(a, "standard") == "pdf-2.0";
  QPDFObjectHandle root = ev.root.getKey("/StructTreeRoot");
  QPDFObjectHandle roleMap = root.getKey("/RoleMap");
  std::vector<std::string> v = tg.roleProblems;
  for (const Elem& e : tg.elems) {
    if (e.raw.empty()) continue;
    QPDFObjectHandle m = roleMap.isDictionary() ? roleMap.getKey("/" + e.raw) : QPDFObjectHandle();
    if (standardType(e.raw, pdf2)) {
      if (m.isName()) {
        std::string t = m.getName().substr(1);
        bool c = false;
        std::string r = standardType(t, pdf2) ? t : resolveRole(t, roleMap, c);
        v.push_back(standardType(r, pdf2) ? "standard_remapped_to_standard" : "standard_remapped_to_non_standard");
      }
      continue;
    }
    std::string t = e.raw;
    std::set<std::string> seen;
    while (!standardType(t, pdf2) && roleMap.isDictionary() && roleMap.getKey("/" + t).isName() && seen.insert(t).second) {
      t = roleMap.getKey("/" + t).getName().substr(1);
    }
    if (!standardType(t, pdf2)) v.push_back("non_standard_type");
  }
  return compare(Value(v), a);
}

int evalToc(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const Tagging& tg = tagging(ev);
  if (!tg.root) return 0;
  std::vector<std::string> v;
  static const std::set<std::string> ok = {"Lbl", "Reference", "NonStruct", "P", "TOC"};
  for (size_t i = 0; i < tg.elems.size(); ++i) {
    const Elem& e = tg.elems[i];
    if (e.type == "TOC" && e.parent >= 0 && tg.elems[static_cast<size_t>(e.parent)].type == "TOC") v.push_back("nested_toc");
    if (e.type != "TOCI") continue;
    bool direct = false;
    for (const Kid& k : e.kids) {
      if (k.mcid >= 0 || k.objr) direct = true;
      if (k.elem >= 0 && !ok.count(tg.elems[static_cast<size_t>(k.elem)].type)) v.push_back("toci_child_not_label_or_reference");
    }
    if (direct) v.push_back("toci_direct_content");
  }
  return compare(Value(v), a);
}

int evalLanguageUnknown(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const Tagging& tg = tagging(ev);
  std::vector<std::string> v;
  const auto& textIds = pageTextMarkedContentIds(ev);
  for (size_t i = 0; i < tg.elems.size(); ++i) {
    const Elem& e = tg.elems[i];
    bool known = !langOf(tg, static_cast<int>(i)).empty();
    if (known) continue;
    bool content = false;
    for (const Kid& k : e.kids) {
      if (k.mcid < 0 || k.page < 0) continue;
      auto t = textIds.find(k.page);
      if (t != textIds.end() && t->second.count(k.mcid)) content = true;
    }
    if (content) v.push_back("page_content");
    if (e.d.getKey("/Alt").isString()) v.push_back("alt_text");
    if (e.d.getKey("/ActualText").isString()) v.push_back("actual_text");
    if (e.d.getKey("/E").isString()) v.push_back("expansion_text");
  }
  if (tg.docLang.empty()) {
    for (size_t i = 0; i < pagesWithUntaggedText(ev).size(); ++i) v.push_back("page_content");
    QPDFObjectHandle ol = ev.root.getKey("/Outlines");
    if (ol.isDictionary() && ol.getKey("/First").isDictionary()) v.push_back("outline");
    QPDFObjectHandle md = ev.root.getKey("/Metadata");
    if (md.isStream()) {
      std::string x;
      try {
        auto b = md.getStreamData(qpdf_dl_all);
        x.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
      } catch (...) {
      }
      bool tagged = x.find("dc:language") != std::string::npos;
      for (size_t p = x.find("xml:lang="); p != std::string::npos && !tagged; p = x.find("xml:lang=", p + 9)) {
        tagged = x.compare(p + 10, 9, "x-default") != 0;
      }
      if (!tagged) v.push_back("metadata");
    }
  }
  for (const AnnotFacts& an : ev.annots) {
    if (!an.obj.isDictionary()) continue;
    int parent = -1;
    if (an.obj.isIndirect()) {
      auto it = tg.objrParent.find(an.obj.getObjGen());
      if (it != tg.objrParent.end()) parent = it->second;
    }
    if (!langOf(tg, parent).empty()) continue;
    if (an.subtype != "Widget" && an.obj.getKey("/Contents").isString()) v.push_back("annotation_contents");
    if (an.subtype == "Widget") {
      QPDFObjectHandle f = an.obj;
      bool tu = false;
      for (int d = 0; f.isDictionary() && d < 32 && !tu; ++d, f = f.getKey("/Parent")) tu = f.getKey("/TU").isString();
      if (tu) v.push_back("form_field_tooltip");
    }
  }
  return compare(Value(v), a);
}

int evalList(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const Tagging& tg = tagging(ev);
  if (!tg.root) return 0;
  const std::string& prop = param(a, "property");
  std::vector<std::string> v;
  for (size_t i = 0; i < tg.elems.size(); ++i) {
    if (tg.elems[i].type != "L") continue;
    std::vector<int> kids = kidsOf(tg, static_cast<int>(i));
    std::vector<int> items, lbls;
    bool caption = false, first = false;
    for (size_t j = 0; j < kids.size(); ++j) {
      const std::string& t = tg.elems[static_cast<size_t>(kids[j])].type;
      if (t == "LI") items.push_back(kids[j]);
      if (t == "Caption") {
        caption = true;
        first = first || j == 0;
      }
    }
    for (int li : items) {
      for (int k : kidsOf(tg, li)) {
        if (tg.elems[static_cast<size_t>(k)].type == "Lbl") lbls.push_back(k);
      }
    }
    if (prop == "items") v.push_back(std::to_string(items.size()));
    else if (prop == "has_caption") v.push_back(tf(caption));
    else if (prop == "caption_first" && caption) v.push_back(tf(first));
    else if (prop == "nested") v.push_back(tf(hasDescendant(tg, static_cast<int>(i), "L")));
    else if (prop == "standard_types_only") v.push_back(tf(onlyStandard(tg, static_cast<int>(i))));
    else if (prop == "label_language_set") {
      bool any = false;
      for (int l : lbls) any = any || !tg.elems[static_cast<size_t>(l)].lang.empty();
      v.push_back(tf(any));
    } else if (prop == "labels_per_item" || prop == "bodies_per_item") {
      const char* want = prop == "labels_per_item" ? "Lbl" : "LBody";
      for (int li : items) {
        int n = 0;
        for (int k : kidsOf(tg, li)) n += tg.elems[static_cast<size_t>(k)].type == want;
        v.push_back(std::to_string(n));
      }
    } else {
      for (int l : lbls) {
        const Elem& e = tg.elems[static_cast<size_t>(l)];
        if (prop == "label_language") v.push_back(e.lang);
        else if (prop == "label_has_language") v.push_back(tf(!e.lang.empty()));
        else if (prop == "label_language_differs" && !e.lang.empty()) v.push_back(tf(e.lang != langOf(tg, e.parent)));
      }
    }
  }
  return compare(Value(v), a);
}

int evalTable(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const Tagging& tg = tagging(ev);
  if (!tg.root) return 0;
  const std::string& prop = param(a, "property");
  std::vector<std::string> v;
  for (size_t i = 0; i < tg.elems.size(); ++i) {
    if (tg.elems[i].type != "Table") continue;
    std::vector<int> kids = kidsOf(tg, static_cast<int>(i));
    std::vector<int> rows;
    int thead = 0, tbody = 0, tfoot = 0, th = 0, td = 0, noScope = 0, emptyTh = 0;
    bool usesHeaders = false;
    int capPos = -1;
    for (size_t j = 0; j < kids.size(); ++j) {
      const std::string& t = tg.elems[static_cast<size_t>(kids[j])].type;
      if (t == "Caption") capPos = static_cast<int>(j);
      thead += t == "THead";
      tbody += t == "TBody";
      tfoot += t == "TFoot";
      if (t == "TR") rows.push_back(kids[j]);
      if (t == "THead" || t == "TBody" || t == "TFoot") {
        for (int r : kidsOf(tg, kids[j])) {
          if (tg.elems[static_cast<size_t>(r)].type == "TR") rows.push_back(r);
        }
      }
    }
    std::map<int, int> carry;
    std::set<int> widths;
    for (size_t r = 0; r < rows.size(); ++r) {
      std::map<int, int> next;
      int col = 0;
      auto skip = [&]() {
        while (carry.count(col)) {
          if (carry[col] > 1) next[col] = carry[col] - 1;
          ++col;
        }
      };
      for (int c : kidsOf(tg, rows[r])) {
        const Elem& e = tg.elems[static_cast<size_t>(c)];
        if (e.type != "TH" && e.type != "TD") continue;
        (e.type == "TH" ? th : td) += 1;
        if (e.type == "TH" && !attribute(e.d, "/Table", "/Scope").isName()) ++noScope;
        if (e.type == "TH" && !hasContent(tg, c) && !filled(e.d.getKey("/ActualText")) && !filled(e.d.getKey("/Alt"))) ++emptyTh;
        if (attribute(e.d, "/Table", "/Headers").isArray()) usesHeaders = true;
        QPDFObjectHandle cs = attribute(e.d, "/Table", "/ColSpan"), rs = attribute(e.d, "/Table", "/RowSpan");
        int cspan = cs.isInteger() ? std::max(1, static_cast<int>(cs.getIntValue())) : 1;
        int rspan = rs.isInteger() ? std::max(1, static_cast<int>(rs.getIntValue())) : 1;
        skip();
        for (int k = 0; k < cspan; ++k) {
          if (rspan > 1) next[col] = rspan - 1;
          ++col;
        }
      }
      skip();
      for (const auto& kv : carry) {
        if (kv.first >= col && kv.second > 1) next[kv.first] = kv.second - 1;
        col = std::max(col, kv.first + 1);
      }
      widths.insert(col);
      carry = next;
    }
    int n = static_cast<int>(kids.size());
    if (prop == "regular") v.push_back(tf(widths.size() <= 1));
    else if (prop == "th_cells") v.push_back(std::to_string(th));
    else if (prop == "td_cells") v.push_back(std::to_string(td));
    else if (prop == "thead") v.push_back(std::to_string(thead));
    else if (prop == "tbody") v.push_back(std::to_string(tbody));
    else if (prop == "tfoot") v.push_back(std::to_string(tfoot));
    else if (prop == "has_caption") v.push_back(tf(capPos >= 0));
    else if (prop == "caption_misplaced" && capPos >= 0) v.push_back(tf(capPos != 0 && capPos != n - 1));
    else if (prop == "th_without_scope") v.push_back(std::to_string(noScope));
    else if (prop == "th_without_scope_or_headers") v.push_back(std::to_string(usesHeaders ? 0 : noScope));
    else if (prop == "empty_header_cells") v.push_back(std::to_string(emptyTh));
    else if (prop == "has_summary") v.push_back(tf(capPos >= 0 || filled(attribute(tg.elems[i].d, "/Table", "/Summary"))));
    else if (prop == "nested") v.push_back(tf(hasDescendant(tg, static_cast<int>(i), "Table")));
    else if (prop == "standard_types_only") v.push_back(tf(onlyStandard(tg, static_cast<int>(i))));
    else if (prop == "thead_without_tbody") v.push_back(tf(thead > 0 && tbody == 0));
    else if (prop == "tfoot_without_tbody") v.push_back(tf(tfoot > 0 && tbody == 0));
  }
  return compare(Value(v), a);
}

int evalEmptyBlock(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const Tagging& tg = tagging(ev);
  if (!tg.root) return 0;
  int hits = 0;
  for (size_t i = 0; i < tg.elems.size(); ++i) {
    if (blockType(tg.elems[i].type)) hits += compare(Value(!hasContent(tg, static_cast<int>(i))), a);
  }
  return hits;
}

int evalArtifact(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  bool known = true;
  const MarkedState* st = markedStateOf(s, ev, known);
  if (!known) {
    supported = false;
    return 0;
  }
  if (!st || !st->artifact) return 0;
  const std::string& part = param(a, "part");
  if (part == "subtype") return compare(Value(st->artifactSubtype), a);
  if (part == "header_footer") return compare(Value(tf(st->artifactSubtype == "Header" || st->artifactSubtype == "Footer")), a);
  return compare(Value(st->artifactType), a);
}

int evalContentTagging(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  bool known = true;
  const MarkedState* st = markedStateOf(s, ev, known);
  if (!known) {
    supported = false;
    return 0;
  }
  if (!st) return 0;
  std::string v = st->artifactInsideTagged ? "artifact_inside_structure"
                  : st->taggedInsideArtifact ? "structure_inside_artifact"
                  : st->artifact ? "artifact"
                  : st->tagged ? "structure"
                  : "neither";
  return compare(Value(v), a);
}

double labToY(double l) {
  double f = (l + 16) / 116;
  return l > 8 ? f * f * f : l / 903.3;
}

bool colourY(const ColorInfo& ci, const std::vector<double>& comps, double& y) {
  if (ci.indexed) return false;
  ColorValue c;
  c.values = comps;
  c.space = ci.csObj;
  static const std::map<std::string, std::string> fam = {{"gray", "/DeviceGray"}, {"rgb", "/DeviceRGB"}, {"cmyk", "/DeviceCMYK"},
                                                         {"icc", "/ICCBased"}, {"lab", "/Lab"}, {"separation", "/Separation"},
                                                         {"devicen", "/DeviceN"}};
  auto it = fam.find(ci.cls);
  if (ci.cls == "cal") c.family = ci.declaredComps == 3 ? "/CalRGB" : "/CalGray";
  else if (it != fam.end()) c.family = it->second;
  else return false;
  double lab[3];
  if (!colorValueToLab(c, lab)) return false;
  y = labToY(lab[0]);
  return true;
}

double ratioOf(double y1, double y2) { return (std::max(y1, y2) + 0.05) / (std::min(y1, y2) + 0.05); }

double srgbY(const unsigned char* px) {
  static const std::array<double, 256> lin = [] {
    std::array<double, 256> t{};
    for (int i = 0; i < 256; ++i) {
      double c = i / 255.0;
      t[static_cast<size_t>(i)] = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    }
    return t;
  }();
  return 0.2126 * lin[px[0]] + 0.7152 * lin[px[1]] + 0.0722 * lin[px[2]];
}

int pageRotate(QPDFObjectHandle pg) {
  for (int d = 0; pg.isDictionary() && d < 32; ++d, pg = pg.getKey("/Parent")) {
    QPDFObjectHandle r = pg.getKey("/Rotate");
    if (r.isInteger()) return static_cast<int>(((r.getIntValue() % 360) + 360) % 360);
  }
  return 0;
}

const Shade* shadeOf(const Events& ev, int page, double dpi) {
  Shade& r = tagging(ev).shade;
  if (r.page == page && r.dpi == dpi) return r.ok ? &r : nullptr;
  r = Shade();
  r.page = page;
  r.dpi = dpi;
  const PageFacts* p = pageFor(ev, page);
  if (!p || !ev.raster) return nullptr;
  r.crop = p->crop.valid ? p->crop : p->media;
  if (!r.crop.valid) return nullptr;
  double area = (r.crop.x1 - r.crop.x0) * (r.crop.y1 - r.crop.y0) / (72.0 * 72.0);
  double use = std::max(1.0, dpi);
  if (area > 0 && area * use * use > 8e6) use = std::max(1.0, std::sqrt(8e6 / area));
  if (!ev.raster(page - 1, use, r.w, r.h, r.rgb) || r.w <= 0 || r.h <= 0 ||
      r.rgb.size() < static_cast<size_t>(r.w) * static_cast<size_t>(r.h) * 3) {
    return nullptr;
  }
  r.rot = pageRotate(p->obj);
  bool turned = r.rot == 90 || r.rot == 270;
  double side = turned ? r.crop.y1 - r.crop.y0 : r.crop.x1 - r.crop.x0;
  r.scale = side > 0 ? r.w / side : use / 72.0;
  r.ok = true;
  return &r;
}

void toPixel(const Shade& r, double x, double y, double& px, double& py) {
  const Box& c = r.crop;
  double s = r.scale;
  switch (r.rot) {
    case 90: px = (y - c.y0) * s; py = (x - c.x0) * s; break;
    case 180: px = (c.x1 - x) * s; py = (y - c.y0) * s; break;
    case 270: px = (c.y1 - y) * s; py = (c.x1 - x) * s; break;
    default: px = (x - c.x0) * s; py = (c.y1 - y) * s; break;
  }
}

bool backgroundY(const Shade& r, const Box& b, double fillY, bool ring, double& out) {
  double ax = 0, ay = 0, bx = 0, by = 0;
  toPixel(r, b.x0, b.y0, ax, ay);
  toPixel(r, b.x1, b.y1, bx, by);
  int x0 = static_cast<int>(std::floor(std::min(ax, bx))), x1 = static_cast<int>(std::ceil(std::max(ax, bx)));
  int y0 = static_cast<int>(std::floor(std::min(ay, by))), y1 = static_cast<int>(std::ceil(std::max(ay, by)));
  int pad = ring ? 3 : 0;
  int X0 = std::max(0, x0 - pad), X1 = std::min(r.w, x1 + pad), Y0 = std::max(0, y0 - pad), Y1 = std::min(r.h, y1 + pad);
  if (X0 >= X1 || Y0 >= Y1) return false;
  std::map<int, std::pair<int, double>> far, all;
  int n = 0;
  const unsigned char* px = reinterpret_cast<const unsigned char*>(r.rgb.data());
  for (int y = Y0; y < Y1; ++y) {
    for (int x = X0; x < X1; ++x) {
      if (ring && x >= x0 - 1 && x <= x1 && y >= y0 - 1 && y <= y1) continue;
      const unsigned char* p = px + (static_cast<size_t>(y) * r.w + x) * 3;
      int key = (p[0] >> 4) << 8 | (p[1] >> 4) << 4 | (p[2] >> 4);
      double yy = srgbY(p);
      auto& cell = all[key];
      ++cell.first;
      cell.second += yy;
      if (ratioOf(yy, fillY) >= 1.1) {
        auto& f = far[key];
        ++f.first;
        f.second += yy;
      }
      ++n;
    }
  }
  if (n < 4) return false;
  int farCount = 0;
  for (const auto& kv : far) farCount += kv.second.first;
  const auto& use = farCount * 10 >= n ? far : all;
  auto best = use.begin();
  for (auto it = use.begin(); it != use.end(); ++it) {
    if (it->second.first > best->second.first) best = it;
  }
  if (best == use.end()) return false;
  out = best->second.second / best->second.first;
  return true;
}

bool boldFont(const Subject& s) {
  if (!s.font) return false;
  std::string n = lowerAscii(s.font->baseFont);
  for (const char* w : {"bold", "black", "heavy", "demi", "semibold"}) {
    if (n.find(w) != std::string::npos) return true;
  }
  QPDFObjectHandle fd = s.font->dict.isDictionary() ? s.font->dict.getKey("/FontDescriptor") : QPDFObjectHandle();
  QPDFObjectHandle dfs = s.font->dict.isDictionary() ? s.font->dict.getKey("/DescendantFonts") : QPDFObjectHandle();
  if (!fd.isDictionary() && dfs.isArray() && dfs.getArrayNItems() > 0 && dfs.getArrayItem(0).isDictionary()) {
    fd = dfs.getArrayItem(0).getKey("/FontDescriptor");
  }
  if (!fd.isDictionary()) return false;
  QPDFObjectHandle wt = fd.getKey("/FontWeight"), fl = fd.getKey("/Flags");
  return (wt.isNumber() && wt.getNumericValue() >= 600) || (fl.isInteger() && (fl.getIntValue() & (1 << 18)));
}

bool blankText(const TextEvent& t, const Subject& s, const Events& ev) {
  if (t.bytes.empty()) return true;
  if (!s.font) return t.bytes.find_first_not_of(' ') == std::string::npos;
  const auto& tum = toUnicodeMap(*s.font, ev);
  size_t step = s.font->cid ? 2 : 1;
  for (size_t i = 0; i + step <= t.bytes.size(); i += step) {
    uint32_t code = static_cast<unsigned char>(t.bytes[i]);
    if (step == 2) code = code << 8 | static_cast<unsigned char>(t.bytes[i + 1]);
    auto it = tum.find(code);
    if (it == tum.end()) {
      if (step == 1 && code == 32) continue;
      return false;
    }
    for (char32_t c : it->second) {
      if (c != U' ' && c != U'\t' && c != 0xA0 && c != 0x3000 && !(c >= 0x2000 && c <= 0x200B)) return false;
    }
  }
  return true;
}

const std::vector<TextRun>& pageRuns(const Events& ev, int idx);
int pageIndexOf(const Events& ev, int page);

bool artifactText(const Events& ev, int page, const Box& b) {
  double cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
  for (const TextRun& r : pageRuns(ev, pageIndexOf(ev, page))) {
    if (r.artifact && cx >= r.x0 - 1 && cx <= r.x1 + 1 && cy >= r.y0 - 1 && cy <= r.y1 + 1) return true;
  }
  return false;
}

int evalContrast(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  const std::string& kind = param(a, "kind");
  const TextEvent* t = kind != "graphics" ? s.text : nullptr;
  const PaintEvent* p = kind != "text" ? s.paint : nullptr;
  if (t) {
    if (!t->visible || t->clippedFull || t->layerHidden || (t->renderMode & 1) || t->renderMode > 6 || !t->bbox.valid ||
        blankText(*t, s, ev)) {
      return 0;
    }
  } else if (p) {
    if (!p->visible || p->clippedFull || p->layerHidden || p->noPaint || p->shade || !p->bbox.valid) return 0;
    double w = p->bbox.x1 - p->bbox.x0, h = p->bbox.y1 - p->bbox.y0;
    const PageFacts* pf = pageFor(ev, s.page);
    const Box* pb = pf ? (pf->crop.valid ? &pf->crop : &pf->media) : nullptr;
    if ((w < 1 && h < 1) || (pb && pb->valid && w * h > 0.25 * (pb->x1 - pb->x0) * (pb->y1 - pb->y0))) return 0;
  } else {
    return 0;
  }
  bool known = true;
  const MarkedState* st = markedStateOf(s, ev, known);
  if (st && st->artifact) return 0;
  if (!known && t && artifactText(ev, s.page, t->bbox)) return 0;
  double fy = 0;
  if (!colourY(t ? t->color : p->color, t ? t->comps : p->comps, fy)) return 0;
  if (!ev.raster) {
    supported = false;
    return 0;
  }
  const Shade* r = shadeOf(ev, s.page, paramNum(a, "resolution"));
  double by = 0;
  if (!r || !backgroundY(*r, t ? t->bbox : p->bbox, fy, !t, by)) return 0;
  double ratio = ratioOf(fy, by);
  double need = paramNum(a, "graphics_ratio");
  if (t) {
    bool large = t->sizePt >= paramNum(a, "large_size") - 0.01 || (boldFont(s) && t->sizePt >= paramNum(a, "large_bold_size") - 0.01);
    need = paramNum(a, large ? "large_text_ratio" : "normal_text_ratio");
  } else if (ratio < 1.05) {
    return 0;
  }
  return compare(Value(ratio + 1e-9 >= need), a);
}

const std::vector<TextRun>& pageRuns(const Events& ev, int idx) {
  const Tagging& tg = tagging(ev);
  auto it = tg.runs.find(idx);
  if (it != tg.runs.end()) return it->second;
  std::vector<TextRun>& out = tg.runs[idx];
  if (idx >= 0 && static_cast<size_t>(idx) < ev.pages.size() && ev.pages[static_cast<size_t>(idx)].obj.isDictionary()) {
    try {
      QPDFPageObjectHelper ph(ev.pages[static_cast<size_t>(idx)].obj);
      out = extractPageText(ph);
    } catch (...) {
      out.clear();
    }
  }
  return out;
}

int pageIndexOf(const Events& ev, int page) {
  for (size_t i = 0; i < ev.pages.size(); ++i) {
    if (ev.pages[i].page == page) return static_cast<int>(i);
  }
  return -1;
}

std::vector<uint32_t> codepoints(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < n && i + k < s.size(); ++k) cp = cp << 6 | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    out.push_back(cp);
    i += static_cast<size_t>(n);
  }
  return out;
}

const char* scriptOf(uint32_t c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
      (c >= 0x1E00 && c <= 0x1EFF)) {
    return "latin";
  }
  if ((c >= 0x370 && c <= 0x3FF && c != 0x37E && c != 0x387) || (c >= 0x1F00 && c <= 0x1FFF)) return "greek";
  if (c >= 0x400 && c <= 0x52F) return "cyrillic";
  if (c >= 0x5D0 && c <= 0x5EA) return "hebrew";
  if ((c >= 0x620 && c <= 0x64A) || (c >= 0x671 && c <= 0x6D3) || (c >= 0x750 && c <= 0x77F) || (c >= 0xFB50 && c <= 0xFDFF) ||
      (c >= 0xFE70 && c <= 0xFEFC)) {
    return "arabic";
  }
  if (c >= 0x904 && c <= 0x939) return "devanagari";
  if (c >= 0xE01 && c <= 0xE30) return "thai";
  if ((c >= 0x1100 && c <= 0x11FF) || (c >= 0x3131 && c <= 0x318E) || (c >= 0xAC00 && c <= 0xD7A3)) return "hangul";
  if ((c >= 0x3041 && c <= 0x30FA) || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0xFF66 && c <= 0xFF9D)) return "kana";
  if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x20000 && c <= 0x2FFFF)) {
    return "han";
  }
  return nullptr;
}

std::set<std::string> scriptsFor(const std::string& lang) {
  std::string l = lowerAscii(lang);
  if (l.empty() || l == "und" || l == "x-default" || l == "zxx") return {};
  if (l.find("-latn") != std::string::npos) return {"latin"};
  if (l.find("-cyrl") != std::string::npos) return {"latin", "cyrillic"};
  if (l.find("-arab") != std::string::npos) return {"latin", "arabic"};
  std::string p = l.substr(0, l.find_first_of("-_"));
  static const std::map<std::string, std::set<std::string>> m = {
      {"ru", {"cyrillic"}}, {"uk", {"cyrillic"}}, {"be", {"cyrillic"}}, {"bg", {"cyrillic"}}, {"mk", {"cyrillic"}},
      {"sr", {"cyrillic"}}, {"kk", {"cyrillic"}}, {"ky", {"cyrillic"}}, {"mn", {"cyrillic"}}, {"tg", {"cyrillic"}},
      {"el", {"greek"}},    {"ar", {"arabic"}},   {"fa", {"arabic"}},   {"ur", {"arabic"}},   {"ps", {"arabic"}},
      {"sd", {"arabic"}},   {"ug", {"arabic"}},   {"he", {"hebrew"}},   {"iw", {"hebrew"}},   {"yi", {"hebrew"}},
      {"zh", {"han"}},      {"ja", {"han", "kana"}}, {"ko", {"hangul", "han"}}, {"hi", {"devanagari"}},
      {"mr", {"devanagari"}}, {"ne", {"devanagari"}}, {"sa", {"devanagari"}}, {"th", {"thai"}}};
  auto it = m.find(p);
  std::set<std::string> out = it == m.end() ? std::set<std::string>() : it->second;
  out.insert("latin");
  return out;
}

int evalLanguageChange(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const Tagging& tg = tagging(ev);
  double minLetters = std::max(1.0, paramNum(a, "min_letters"));
  std::vector<std::string> v;
  for (size_t i = 0; i < ev.pages.size(); ++i) {
    std::map<long long, std::string> tagged;
    std::vector<std::string> loose;
    for (const TextRun& r : pageRuns(ev, static_cast<int>(i))) {
      if (r.artifact) continue;
      if (r.mcid >= 0 && tg.mcidOwner.count({static_cast<int>(i), r.mcid})) tagged[r.mcid] += r.text;
      else loose.push_back(r.text);
    }
    auto judge = [&](const std::string& txt, const std::string& lang) {
      std::set<std::string> ok = scriptsFor(lang);
      if (ok.empty()) return;
      std::map<std::string, int> count;
      std::string cur;
      int streak = 0;
      for (uint32_t c : codepoints(txt)) {
        const char* sc = scriptOf(c);
        if (!sc) continue;
        if (cur != sc) {
          cur = sc;
          streak = 0;
        }
        count[cur] = std::max(count[cur], ++streak);
      }
      std::string best;
      int n = 0;
      for (const auto& kv : count) {
        if (!ok.count(kv.first) && kv.second > n) {
          best = kv.first;
          n = kv.second;
        }
      }
      if (n >= minLetters) v.push_back(best);
    };
    for (const auto& kv : tagged) judge(kv.second, langOf(tg, tg.mcidOwner.at({static_cast<int>(i), kv.first})));
    for (const std::string& t : loose) judge(t, tg.docLang);
  }
  return compare(Value(v), a);
}

std::string linkText(const Events& ev, const AnnotFacts& an) {
  int idx = pageIndexOf(ev, an.page);
  if (idx < 0 || !an.rect.valid) return std::string();
  const Box& R = an.rect;
  std::string out;
  for (const TextRun& r : pageRuns(ev, idx)) {
    double cy = (r.y0 + r.y1) / 2;
    if (cy < R.y0 - 1 || cy > R.y1 + 1 || r.x1 < R.x0 || r.x0 > R.x1) continue;
    std::string part;
    for (size_t j = 0; j < r.charX.size(); ++j) {
      if (r.charX[j] < R.x0 - 1 || r.charX[j] >= R.x1) continue;
      size_t b = r.byteAt[j], e = j + 1 < r.byteAt.size() ? r.byteAt[j + 1] : r.text.size();
      part += r.text.substr(b, e - b);
    }
    if (!part.empty()) out += (out.empty() ? "" : " ") + part;
  }
  std::string norm;
  for (char c : lowerAscii(out)) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!norm.empty() && norm.back() != ' ') norm += ' ';
    } else {
      norm += c;
    }
  }
  return trimmed(norm, kSpacePunct);
}

int evalLinkText(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.annot || s.annot->subtype != "Link" || !shown(*s.annot, ev)) return 0;
  std::string t = linkText(ev, *s.annot);
  return t.empty() ? 0 : compare(Value(t), a);
}

bool sameColour(const TextEvent& x, const TextEvent& y) {
  if (x.color.cls != y.color.cls || x.color.spot != y.color.spot || x.comps.size() != y.comps.size()) return false;
  for (size_t i = 0; i < x.comps.size(); ++i) {
    if (std::fabs(x.comps[i] - y.comps[i]) > 0.01) return false;
  }
  return true;
}

int evalLinkColourOnly(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.annot || s.annot->subtype != "Link" || !s.annot->rect.valid || !shown(*s.annot, ev)) return 0;
  QPDFObjectHandle o = s.annot->obj;
  QPDFObjectHandle bs = o.getKey("/BS"), border = o.getKey("/Border");
  double bw = 1;
  if (bs.isDictionary()) {
    if (nameIs(bs.getKey("/S"), "/U")) return compare(Value(false), a);
    if (bs.getKey("/W").isNumber()) bw = bs.getKey("/W").getNumericValue();
  } else if (border.isArray() && border.getArrayNItems() >= 3 && border.getArrayItem(2).isNumber()) {
    bw = border.getArrayItem(2).getNumericValue();
  }
  if (bw > 0 && !o.getKey("/AP").isDictionary()) return compare(Value(false), a);
  const Box& R = s.annot->rect;
  const TextEvent* link = nullptr;
  const TextEvent* near = nullptr;
  Box lb;
  double best = 1e300;
  for (const TextEvent& t : ev.texts) {
    if (t.page != s.annot->page || !t.bbox.valid || !t.visible || t.renderMode == 3 || t.renderMode == 7) continue;
    double cx = (t.bbox.x0 + t.bbox.x1) / 2, cy = (t.bbox.y0 + t.bbox.y1) / 2;
    if (cx >= R.x0 && cx <= R.x1 && cy >= R.y0 && cy <= R.y1) {
      if (!link) {
        link = &t;
        lb = t.bbox;
      }
      lb.x0 = std::min(lb.x0, t.bbox.x0);
      lb.x1 = std::max(lb.x1, t.bbox.x1);
    }
  }
  if (!link) return 0;
  double h = lb.y1 - lb.y0;
  for (const TextEvent& t : ev.texts) {
    if (t.page != s.annot->page || !t.bbox.valid || !t.visible || &t == link) continue;
    double cx = (t.bbox.x0 + t.bbox.x1) / 2, cy = (t.bbox.y0 + t.bbox.y1) / 2;
    if (cx >= R.x0 && cx <= R.x1 && cy >= R.y0 && cy <= R.y1) continue;
    double overlap = std::min(t.bbox.y1, lb.y1) - std::max(t.bbox.y0, lb.y0);
    if (overlap < 0.5 * h) continue;
    double gap = t.bbox.x0 >= lb.x1 ? t.bbox.x0 - lb.x1 : lb.x0 - t.bbox.x1;
    gap = std::max(0.0, gap);
    if (gap <= 2 * h && gap < best) {
      best = gap;
      near = &t;
    }
  }
  if (!near || sameColour(*link, *near)) return 0;
  if (link->fontName != near->fontName) return compare(Value(false), a);
  double size = link->sizePt > 0 ? link->sizePt : h;
  for (const PaintEvent& p : ev.paints) {
    if (p.page != s.annot->page || !p.bbox.valid || p.noPaint || !p.visible) continue;
    double ph = p.bbox.y1 - p.bbox.y0;
    double ov = std::min(p.bbox.x1, lb.x1) - std::max(p.bbox.x0, lb.x0);
    if (ph <= std::max(1.5, 0.15 * size) && ov >= 0.5 * (lb.x1 - lb.x0) && p.bbox.y1 >= lb.y0 - 0.25 * size &&
        p.bbox.y0 <= lb.y0 + 0.35 * size) {
      return compare(Value(false), a);
    }
  }
  double y1 = 0, y2 = 0;
  if (colourY(link->color, link->comps, y1) && colourY(near->color, near->comps, y2) && ratioOf(y1, y2) >= 3) {
    return compare(Value(false), a);
  }
  return compare(Value(true), a);
}

bool nonBlank(const std::string& s) { return s.find_first_not_of(" \t\r\n") != std::string::npos; }

int evalTitle(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::vector<std::string> v;
  QPDFObjectHandle info = ev.trailer.isDictionary() ? ev.trailer.getKey("/Info") : QPDFObjectHandle();
  bool infoTitle = info.isDictionary() && nonBlank(text(info.getKey("/Title")));
  std::string x;
  QPDFObjectHandle md = ev.root.isDictionary() ? ev.root.getKey("/Metadata") : QPDFObjectHandle();
  if (md.isStream()) {
    try {
      auto b = md.getStreamData(qpdf_dl_all);
      x.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
    } catch (...) {
      x.clear();
    }
  }
  bool xmpTitle = false;
  size_t p = x.find("<dc:title");
  if (p != std::string::npos) {
    size_t open = x.find('>', p), end = x.find("</dc:title>", p);
    if (open != std::string::npos && end != std::string::npos && open < end && x[open - 1] != '/') {
      std::string inner;
      bool tag = false;
      for (size_t i = open + 1; i < end; ++i) {
        if (x[i] == '<') tag = true;
        else if (x[i] == '>') tag = false;
        else if (!tag) inner += x[i];
      }
      xmpTitle = nonBlank(inner);
    }
  }
  if (!infoTitle && !xmpTitle) v.push_back("missing");
  if (!xmpTitle) v.push_back("not_in_metadata");
  QPDFObjectHandle vp = ev.root.isDictionary() ? ev.root.getKey("/ViewerPreferences") : QPDFObjectHandle();
  QPDFObjectHandle ddt = vp.isDictionary() ? vp.getKey("/DisplayDocTitle") : QPDFObjectHandle();
  if (!ddt.isBool() || !ddt.getBoolValue()) v.push_back("not_displayed");
  return compare(Value(v), a);
}
}

void registerStructureChecks(CheckTable& t) {
  t.add("annotation.link_colour_only", evalLinkColourOnly);
  t.add("annotation.link_text", evalLinkText);
  t.add("content.contrast_sufficient", evalContrast);
  t.add("document.title", evalTitle);
  t.add("structure.language_change", evalLanguageChange);
  t.add("structure.alternate_text", evalAlternateText);
  t.add("structure.artifact", evalArtifact);
  t.add("structure.content_tagging", evalContentTagging);
  t.add("structure.empty_block", evalEmptyBlock);
  t.add("structure.heading_level", evalHeadingLevel);
  t.add("structure.headings", evalHeadings);
  t.add("structure.language_unknown", evalLanguageUnknown);
  t.add("structure.list", evalList);
  t.add("structure.nesting", evalNesting);
  t.add("structure.notes", evalNotes);
  t.add("structure.standard_type", evalStandardType);
  t.add("structure.table", evalTable);
  t.add("structure.toc", evalToc);
  t.add("structure.tree", evalTree);
}
}
