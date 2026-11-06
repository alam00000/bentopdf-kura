#include "uacheck.hh"

#include <qpdf/Pl_Discard.hh>
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
namespace {
const std::set<std::string>& uaStandardTypes() {
  static const std::set<std::string> s = {
      "/Document", "/Part", "/Art", "/Sect", "/Div", "/BlockQuote", "/Caption", "/TOC", "/TOCI",
      "/Index", "/NonStruct", "/Private", "/P", "/H", "/H1", "/H2", "/H3", "/H4", "/H5", "/H6",
      "/L", "/LI", "/Lbl", "/LBody", "/Table", "/TR", "/TH", "/TD", "/THead", "/TBody", "/TFoot",
      "/Span", "/Quote", "/Note", "/Reference", "/BibEntry", "/Code", "/Link", "/Annot", "/Ruby",
      "/RB", "/RT", "/RP", "/Warichu", "/WT", "/WP", "/Figure", "/Formula", "/Form"};
  return s;
}

bool langValid(const std::string& s) {
  static const std::regex re("^[a-zA-Z]{1,8}(-[a-zA-Z0-9]{1,8})*$");
  return std::regex_match(s, re);
}

std::string textOf(QPDFObjectHandle o) {
  return o.isString() ? o.getUTF8Value() : std::string();
}

struct UaScanState {
  bool catalogLang = false;
  std::map<std::pair<QPDFObjGen, int>, bool> mcidLang;
  std::map<QPDFObjGen, int> taggedFormDraws;
  long long untagged = 0;
  long long nested = 0;
  long long textNoLang = 0;
  long long spanNoLang = 0;
  long long badLang = 0;
};

class UntaggedScan : public QPDFObjectHandle::TokenFilter {
 public:
  UntaggedScan(UaScanState& st, QPDFObjectHandle res, QPDFObjGen owner, QPDFObjGen page, bool inherited,
               bool inheritedLang, int depth)
      : st(st), res(res), owner(owner), page(page), inherited(inherited), inheritedLang(inheritedLang), depth(depth) {}

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
    } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
      if (!covered()) {
        ++st.untagged;
      } else if (!st.catalogLang && !inArtifact() && !langKnown()) {
        ++st.textNoLang;
      }
    } else if (op == "f" || op == "F" || op == "f*" || op == "B" || op == "B*" || op == "b" || op == "b*" ||
               op == "S" || op == "s" || op == "sh") {
      if (!covered()) ++st.untagged;
    } else if (op == "Do" && !ops.empty()) {
      QPDFObjectHandle xo = res.isDictionary() ? res.getKey("/XObject") : QPDFObjectHandle();
      QPDFObjectHandle x = xo.isDictionary() ? xo.getKey(ops.back().getValue()) : QPDFObjectHandle();
      if (x.isStream() && nameIs(x.getDict().getKey("/Subtype"), "/Form") && depth < kMaxContentNest) {
        if (x.getDict().hasKey("/StructParents") && x.isIndirect()) ++st.taggedFormDraws[x.getObjGen()];
        QPDFObjectHandle fr = x.getDict().getKey("/Resources");
        UntaggedScan inner(st, fr.isDictionary() ? fr : res, x.isIndirect() ? x.getObjGen() : owner, page, covered(),
                           inheritedLang || langKnown(), depth + 1);
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
    bool tagged = false;
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
    bool hasLang = false, hasAlt = false;
    if (props.isDictionary()) {
      if (props.getKey("/MCID").isInteger()) mcid = props.getKey("/MCID").getIntValue();
      QPDFObjectHandle l = props.getKey("/Lang");
      if (l.isString()) {
        hasLang = true;
        if (!langValid(l.getUTF8Value())) ++st.badLang;
      }
      hasAlt = props.hasKey("/ActualText") || props.hasKey("/Alt") || props.hasKey("/E");
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
      }
    }
    m.tagged = mcid >= 0;
    bool outerLang = inheritedLang || langKnown();
    bool treeLang = false;
    if (m.tagged) {
      auto it = st.mcidLang.find({owner, static_cast<int>(mcid)});
      if (it == st.mcidLang.end()) it = st.mcidLang.find({page, static_cast<int>(mcid)});
      treeLang = it == st.mcidLang.end() || it->second;
    }
    m.lang = hasLang || treeLang || (!m.tagged && outerLang);
    if (tag == "/Span" && hasAlt && !st.catalogLang && !hasLang && !outerLang && !treeLang) ++st.spanNoLang;
    bool insideArtifact = inArtifact(), insideTagged = false;
    for (const Mark& o : marks) insideTagged = insideTagged || o.tagged;
    if ((m.tagged && insideArtifact) || (m.artifact && insideTagged)) ++st.nested;
    marks.push_back(m);
  }
  bool covered() const {
    if (inherited) return true;
    for (const Mark& m : marks) {
      if (m.tagged || m.artifact) return true;
    }
    return false;
  }
  bool inArtifact() const {
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
  QPDFObjGen owner;
  QPDFObjGen page;
  bool inherited;
  bool inheritedLang;
  int depth;
  std::vector<Mark> marks;
  std::vector<QPDFTokenizer::Token> ops;
};

QPDFObjectHandle tableAttr(QPDFObjectHandle e, const std::string& key) {
  QPDFObjectHandle a = e.getKey("/A");
  std::vector<QPDFObjectHandle> list;
  if (a.isDictionary()) list.push_back(a);
  for (int i = 0; a.isArray() && i < a.getArrayNItems(); ++i) list.push_back(a.getArrayItem(i));
  for (QPDFObjectHandle d : list) {
    if (d.isDictionary() && nameIs(d.getKey("/O"), "/Table") && d.hasKey(key)) return d.getKey(key);
  }
  return QPDFObjectHandle::newNull();
}

struct Cell {
  bool th = false;
  bool empty = false;
  int row = 0, col = 0, rs = 1, cs = 1;
  std::string scope;
  std::vector<std::string> headers;
  std::string id;
};

void checkTable(const std::vector<std::vector<Cell>>& rowsIn, std::set<std::string>& rules) {
  std::vector<std::vector<Cell>> rows = rowsIn;
  std::map<std::pair<int, int>, bool> occ;
  int n = static_cast<int>(rows.size());
  bool overflow = false;
  for (int r = 0; r < n; ++r) {
    int c = 0;
    for (Cell& cell : rows[r]) {
      while (occ.count({r, c})) ++c;
      cell.row = r;
      cell.col = c;
      for (int dr = 0; dr < cell.rs; ++dr) {
        for (int dc = 0; dc < cell.cs; ++dc) occ[{r + dr, c + dc}] = true;
      }
      if (r + cell.rs > n) overflow = true;
      c += cell.cs;
    }
  }
  std::map<int, int> width;
  for (const auto& kv : occ) {
    if (kv.first.first < n) ++width[kv.first.first];
  }
  std::set<int> widths;
  for (int r = 0; r < n; ++r) widths.insert(width[r]);
  if (overflow || widths.size() > 1) rules.insert("7.2-42");
  std::set<std::string> ids;
  bool anyTh = false;
  for (const auto& row : rows) {
    for (const Cell& cell : row) {
      if (cell.th) {
        anyTh = true;
        if (!cell.id.empty()) ids.insert(cell.id);
      }
    }
  }
  if (!anyTh) return;
  for (const auto& row : rows) {
    for (const Cell& td : row) {
      if (td.th || td.empty) continue;
      if (!td.headers.empty()) {
        for (const std::string& h : td.headers) {
          if (!ids.count(h)) rules.insert("7.5-2");
        }
        continue;
      }
      bool connected = false;
      for (const auto& row2 : rows) {
        for (const Cell& th : row2) {
          if (!th.th) continue;
          bool sameCol = th.col < td.col + td.cs && td.col < th.col + th.cs;
          bool sameRow = th.row < td.row + td.rs && td.row < th.row + th.rs;
          if (sameCol && (th.scope == "/Column" || th.scope == "/Both")) connected = true;
          if (sameRow && (th.scope == "/Row" || th.scope == "/Both")) connected = true;
        }
      }
      if (!connected) rules.insert("7.5-1");
    }
  }
}

bool rectOutside(QPDFObjectHandle rect, QPDFObjectHandle box) {
  if (!rect.isArray() || rect.getArrayNItems() != 4 || !box.isArray() || box.getArrayNItems() != 4) return false;
  auto v = [](QPDFObjectHandle a, int i) { return numOf(a.getArrayItem(i), 0); };
  double ax0 = std::min(v(rect, 0), v(rect, 2)), ax1 = std::max(v(rect, 0), v(rect, 2));
  double ay0 = std::min(v(rect, 1), v(rect, 3)), ay1 = std::max(v(rect, 1), v(rect, 3));
  double bx0 = std::min(v(box, 0), v(box, 2)), bx1 = std::max(v(box, 0), v(box, 2));
  double by0 = std::min(v(box, 1), v(box, 3)), by1 = std::max(v(box, 1), v(box, 3));
  return ax1 < bx0 || ax0 > bx1 || ay1 < by0 || ay0 > by1;
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
    for (const char* prop : {"part", "amd", "corr"}) {
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
}

std::vector<UaFinding> uaMachineCheck(QPDF& pdf) {
  std::vector<UaFinding> out;
  auto add = [&](const std::string& cp, const std::string& detail, std::set<int> pages = {}) {
    out.push_back({cp, detail, pages});
  };
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
  for (const PrefixUse& u : uaIdProps(xmp)) {
    if (u.prop == "part") {
      uaId = true;
      if (u.value != "1") add("5-2", "pdfuaid:part is " + u.value + ", not 1");
    }
    if (u.prefix != "pdfuaid") add("5-3", "PDF/UA identification property " + u.prop + " uses prefix " + u.prefix + " instead of pdfuaid");
  }
  if (!uaId) add("06-002", "the XMP metadata does not identify the file as PDF/UA-1 (pdfuaid:part)");
  if (!md.isStream()) add("7.1-8", "the catalog has no metadata stream");
  if (xmp.find("<dc:title") == std::string::npos) add("06-003", "the XMP metadata has no dc:title");
  QPDFObjectHandle vp = root.getKey("/ViewerPreferences");
  if (!vp.isDictionary() || !vp.getKey("/DisplayDocTitle").isBool() || !vp.getKey("/DisplayDocTitle").getBoolValue()) {
    add("07-001", "ViewerPreferences does not set DisplayDocTitle to true");
  }
  QPDFObjectHandle mi = root.getKey("/MarkInfo");
  if (!mi.isDictionary() || !mi.getKey("/Marked").isBool() || !mi.getKey("/Marked").getBoolValue()) {
    add("01-002", "the document is not marked as tagged (MarkInfo /Marked)");
  }
  if (mi.isDictionary() && mi.getKey("/Suspects").isBool() && mi.getKey("/Suspects").getBoolValue()) {
    add("01-003", "MarkInfo /Suspects is true");
  }
  UaScanState st;
  QPDFObjectHandle lang = root.getKey("/Lang");
  st.catalogLang = lang.isString() && !lang.getUTF8Value().empty();
  if (lang.isString() && !langValid(lang.getUTF8Value())) ++st.badLang;
  if (!st.catalogLang) {
    QPDFObjectHandle ol = root.getKey("/Outlines");
    if (ol.isDictionary() && ol.getKey("/First").isDictionary()) add("7.2-2", "outline entries have no natural language (no catalog /Lang)");
    if (onlyDefaultLangAlt(xmp)) add("7.2-33", "metadata text has only an x-default language and the catalog has no /Lang");
  }
  QPDFObjectHandle acro = root.getKey("/AcroForm");
  if (acro.isDictionary() && acro.hasKey("/XFA")) add("25-001", "the file contains a dynamic XFA form");
  QPDFObjectHandle enc = pdf.getTrailer().getKey("/Encrypt");
  if (enc.isDictionary()) {
    long long p = enc.getKey("/P").isInteger() ? enc.getKey("/P").getIntValue() : -1;
    if (!(p & 512)) add("26-002", "encryption does not permit assistive technology access (P bit 10)");
  }
  QPDFObjectHandle oc = root.getKey("/OCProperties");
  if (oc.isDictionary()) {
    std::vector<QPDFObjectHandle> cfgs{oc.getKey("/D")};
    QPDFObjectHandle more = oc.getKey("/Configs");
    for (int i = 0; more.isArray() && i < more.getArrayNItems(); ++i) cfgs.push_back(more.getArrayItem(i));
    for (QPDFObjectHandle c : cfgs) {
      if (!c.isDictionary()) continue;
      if (textOf(c.getKey("/Name")).empty()) add("7.10-1", "an optional content configuration has no Name");
      if (c.hasKey("/AS")) add("7.10-2", "an optional content configuration contains an AS entry");
    }
  }
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

  std::map<QPDFObjGen, std::pair<QPDFObjectHandle, bool>> annotParent;
  std::map<QPDFObjGen, bool> elemLang;
  QPDFObjectHandle tree = root.getKey("/StructTreeRoot");
  if (!tree.isDictionary()) {
    add("01-001", "the document has no structure tree");
  } else {
    QPDFObjectHandle rm = tree.getKey("/RoleMap");
    auto stdType = [&](const std::string& raw) {
      std::string type = raw;
      std::set<std::string> chain;
      while (rm.isDictionary() && rm.hasKey(type) && !uaStandardTypes().count(type) && chain.insert(type).second) {
        type = nameOf(rm.getKey(type));
      }
      return type;
    };
    std::map<std::string, std::set<int>> nonStd;
    int figures = 0, formulas = 0;
    std::vector<int> headings;
    bool numbered = false, plainH = false;
    std::set<std::string> rules;
    std::set<std::string> noteIds;
    std::set<QPDFObjGen> seen;
    auto isElem = [](QPDFObjectHandle k) {
      return k.isDictionary() && !nameIs(k.getKey("/Type"), "/MCR") && !nameIs(k.getKey("/Type"), "/OBJR");
    };
    std::function<void(QPDFObjectHandle, std::string, bool, QPDFObjGen, int)> walk =
        [&](QPDFObjectHandle e, std::string parentType, bool inLang, QPDFObjGen pg, int depth) {
      if (depth > kMaxStructDepth || !e.isDictionary()) return;
      if (e.isIndirect() && !seen.insert(e.getObjGen()).second) return;
      std::string raw = nameOf(e.getKey("/S"));
      std::string type = stdType(raw);
      if (!raw.empty() && !uaStandardTypes().count(type)) nonStd[raw];
      if (uaStandardTypes().count(raw) && rm.isDictionary() && rm.hasKey(raw)) {
        add("02-004", "standard structure type " + raw + " is remapped in the role map");
      }
      QPDFObjectHandle el = e.getKey("/Lang");
      if (el.isString() && !langValid(el.getUTF8Value())) ++st.badLang;
      bool hasLang = inLang || el.isString();
      if (e.isIndirect()) elemLang[e.getObjGen()] = hasLang;
      if (!st.catalogLang && !hasLang) {
        if (e.hasKey("/ActualText")) rules.insert("7.2-21");
        if (e.hasKey("/Alt")) rules.insert("7.2-22");
        if (e.hasKey("/E")) rules.insert("7.2-23");
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
      if (type.size() == 3 && type[1] == 'H' && type[2] >= '1' && type[2] <= '6') {
        numbered = true;
        headings.push_back(type[2] - '0');
      }
      QPDFObjectHandle epg = e.getKey("/Pg");
      if (epg.isDictionary() && epg.isIndirect()) pg = epg.getObjGen();
      QPDFObjectHandle k = e.getKey("/K");
      std::vector<QPDFObjectHandle> kids;
      if (k.isArray()) {
        for (int i = 0; i < k.getArrayNItems(); ++i) kids.push_back(k.getArrayItem(i));
      } else if (!k.isNull()) {
        kids.push_back(k);
      }
      std::vector<QPDFObjectHandle> elems;
      std::vector<std::string> ks;
      for (QPDFObjectHandle kid : kids) {
        if (kid.isInteger()) {
          st.mcidLang[{pg, static_cast<int>(kid.getIntValue())}] = hasLang;
        } else if (nameIs(kid.getKey("/Type"), "/MCR") && kid.getKey("/MCID").isInteger()) {
          QPDFObjGen ow = pg;
          if (kid.getKey("/Stm").isStream() && kid.getKey("/Stm").isIndirect()) ow = kid.getKey("/Stm").getObjGen();
          else if (kid.getKey("/Pg").isDictionary() && kid.getKey("/Pg").isIndirect()) ow = kid.getKey("/Pg").getObjGen();
          st.mcidLang[{ow, static_cast<int>(kid.getKey("/MCID").getIntValue())}] = hasLang;
        } else if (nameIs(kid.getKey("/Type"), "/OBJR")) {
          QPDFObjectHandle obj = kid.getKey("/Obj");
          if (obj.isIndirect()) annotParent[obj.getObjGen()] = {e, hasLang};
        } else if (isElem(kid)) {
          elems.push_back(kid);
          ks.push_back(stdType(nameOf(kid.getKey("/S"))));
        }
      }
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
      if (count("/H") > 1) rules.insert("7.4.4-1");
      if (type == "/Table") {
        if (!only({"/TR", "/THead", "/TBody", "/TFoot", "/Caption"})) rules.insert("7.2-3");
        if (count("/THead") > 1) rules.insert("7.2-11");
        if (count("/TFoot") > 1) rules.insert("7.2-12");
        if (count("/TFoot") && !count("/TBody")) rules.insert("7.2-13");
        if (count("/THead") && !count("/TBody")) rules.insert("7.2-14");
        for (size_t i = 1; i + 1 < ks.size(); ++i) {
          if (ks[i] == "/Caption") rules.insert("7.2-16");
        }
        if (count("/Caption") > 1) rules.insert("7.2-39");
        std::vector<QPDFObjectHandle> trs;
        for (size_t i = 0; i < elems.size(); ++i) {
          if (ks[i] == "/TR") trs.push_back(elems[i]);
          if (ks[i] != "/THead" && ks[i] != "/TBody" && ks[i] != "/TFoot") continue;
          QPDFObjectHandle gk = elems[i].getKey("/K");
          std::vector<QPDFObjectHandle> g;
          if (gk.isArray()) {
            for (int j = 0; j < gk.getArrayNItems(); ++j) g.push_back(gk.getArrayItem(j));
          } else {
            g.push_back(gk);
          }
          for (QPDFObjectHandle r : g) {
            if (isElem(r) && stdType(nameOf(r.getKey("/S"))) == "/TR") trs.push_back(r);
          }
        }
        std::vector<std::vector<Cell>> rows;
        for (QPDFObjectHandle tr : trs) {
          rows.emplace_back();
          QPDFObjectHandle ck = tr.getKey("/K");
          std::vector<QPDFObjectHandle> cells;
          if (ck.isArray()) {
            for (int j = 0; j < ck.getArrayNItems(); ++j) cells.push_back(ck.getArrayItem(j));
          } else {
            cells.push_back(ck);
          }
          for (QPDFObjectHandle c : cells) {
            if (!isElem(c)) continue;
            std::string ct = stdType(nameOf(c.getKey("/S")));
            if (ct != "/TH" && ct != "/TD") continue;
            Cell cell;
            cell.th = ct == "/TH";
            cell.empty = c.getKey("/K").isNull() || (c.getKey("/K").isArray() && !c.getKey("/K").getArrayNItems());
            QPDFObjectHandle rs = tableAttr(c, "/RowSpan"), cs = tableAttr(c, "/ColSpan");
            if (rs.isInteger() && rs.getIntValue() > 1) cell.rs = static_cast<int>(std::min<long long>(rs.getIntValue(), 1000));
            if (cs.isInteger() && cs.getIntValue() > 1) cell.cs = static_cast<int>(std::min<long long>(cs.getIntValue(), 1000));
            cell.scope = nameOf(tableAttr(c, "/Scope"));
            QPDFObjectHandle hd = tableAttr(c, "/Headers");
            for (int j = 0; hd.isArray() && j < hd.getArrayNItems(); ++j) cell.headers.push_back(textOf(hd.getArrayItem(j)));
            cell.id = textOf(c.getKey("/ID"));
            rows.back().push_back(cell);
          }
        }
        checkTable(rows, rules);
      }
      if (type == "/TR" && parentType != "/Table" && parentType != "/THead" && parentType != "/TBody" && parentType != "/TFoot") {
        rules.insert("7.2-4");
      }
      if ((type == "/THead" || type == "/TBody" || type == "/TFoot") && parentType != "/Table") rules.insert("7.2-5");
      if ((type == "/TH" || type == "/TD") && parentType != "/TR") rules.insert("7.2-8");
      if (type == "/TR" && !only({"/TH", "/TD"})) rules.insert("7.2-10");
      if ((type == "/THead" || type == "/TBody" || type == "/TFoot") && !only({"/TR"})) rules.insert("7.2-36");
      if (type == "/LI" && parentType != "/L") rules.insert("7.2-17");
      if (type == "/LBody" && parentType != "/LI") rules.insert("7.2-18");
      if (type == "/L" && !only({"/L", "/LI", "/Caption"})) rules.insert("7.2-19");
      if (type == "/L" && captionNotFirst()) rules.insert("7.2-40");
      if (type == "/LI" && !only({"/Lbl", "/LBody"})) rules.insert("7.2-20");
      if (type == "/TOCI" && parentType != "/TOC") rules.insert("7.2-26");
      if (type == "/TOC" && !only({"/TOC", "/TOCI", "/Caption"})) rules.insert("7.2-27");
      if (type == "/TOC" && captionNotFirst()) rules.insert("7.2-28");
      for (QPDFObjectHandle kid : elems) walk(kid, type, hasLang, pg, depth + 1);
    };
    QPDFObjectHandle k = tree.getKey("/K");
    if (k.isArray()) {
      for (int i = 0; i < k.getArrayNItems(); ++i) walk(k.getArrayItem(i), "", false, QPDFObjGen(), 0);
    } else {
      walk(k, "", false, QPDFObjGen(), 0);
    }
    for (auto& kv : nonStd) add("02-001", "non-standard structure type " + kv.first + " is not role-mapped to a standard type");
    if (figures) add("13-004", std::to_string(figures) + " Figure element(s) without alternate text");
    if (formulas) add("17-002", std::to_string(formulas) + " Formula element(s) without alternate text");
    if (numbered && plainH) add("14-001", "the document mixes numbered and unnumbered headings");
    if (!headings.empty() && headings[0] != 1) add("14-002", "the first numbered heading is not H1");
    for (size_t i = 1; i < headings.size(); ++i) {
      if (headings[i] > headings[i - 1] + 1) {
        add("14-003", "heading level skipped (H" + std::to_string(headings[i - 1]) + " followed by H" + std::to_string(headings[i]) + ")");
        break;
      }
    }
    for (const std::string& r : rules) add(r, "structure element rule " + r + " of ISO 14289-1 is violated");
  }
  QPDFObjectHandle parentTree = tree.isDictionary() ? tree.getKey("/ParentTree") : QPDFObjectHandle();
  auto structParentOf = [&](QPDFObjectHandle a) -> std::pair<QPDFObjectHandle, bool> {
    if (a.isIndirect()) {
      auto it = annotParent.find(a.getObjGen());
      if (it != annotParent.end()) return it->second;
    }
    if (parentTree.isDictionary() && a.getKey("/StructParent").isInteger()) {
      try {
        QPDFNumberTreeObjectHelper nt(parentTree, pdf);
        QPDFObjectHandle p;
        if (nt.findObject(a.getKey("/StructParent").getIntValue(), p) && p.isDictionary()) {
          auto it = elemLang.find(p.getObjGen());
          return {p, it != elemLang.end() && it->second};
        }
      } catch (...) {
      }
    }
    return {QPDFObjectHandle(), false};
  };
  QPDFObjectHandle rm = tree.isDictionary() ? tree.getKey("/RoleMap") : QPDFObjectHandle();
  auto stdOf = [&](QPDFObjectHandle e) {
    std::string type = e.isDictionary() ? nameOf(e.getKey("/S")) : "";
    std::set<std::string> chain;
    while (rm.isDictionary() && rm.hasKey(type) && !uaStandardTypes().count(type) && chain.insert(type).second) {
      type = nameOf(rm.getKey(type));
    }
    return type;
  };
  QPDFPageDocumentHelper dh(pdf);
  std::set<int> untaggedPages, textLangPages;
  std::map<std::string, std::set<int>> annotRules;
  int pageNo = 0;
  for (auto& ph : dh.getAllPages()) {
    ++pageNo;
    QPDFObjectHandle res = ph.getAttribute("/Resources", false);
    long long before = st.untagged, beforeLang = st.textNoLang;
    QPDFObjGen pog = ph.getObjectHandle().getObjGen();
    UntaggedScan scan(st, res, pog, pog, false, false, 0);
    Pl_Discard d;
    try {
      ph.filterContents(&scan, &d);
    } catch (...) {
    }
    if (st.untagged > before) untaggedPages.insert(pageNo);
    if (st.textNoLang > beforeLang) textLangPages.insert(pageNo);
    QPDFObjectHandle annots = ph.getObjectHandle().getKey("/Annots");
    if (annots.isArray() && annots.getArrayNItems() && !nameIs(ph.getObjectHandle().getKey("/Tabs"), "/S")) {
      annotRules["7.18.3-1"].insert(pageNo);
    }
    QPDFObjectHandle box = ph.getAttribute("/CropBox", false);
    if (!box.isArray()) box = ph.getAttribute("/MediaBox", false);
    for (int i = 0; annots.isArray() && i < annots.getArrayNItems(); ++i) {
      QPDFObjectHandle a = annots.getArrayItem(i);
      if (!a.isDictionary()) continue;
      std::string sub = nameOf(a.getKey("/Subtype"));
      long long fl = a.getKey("/F").isInteger() ? a.getKey("/F").getIntValue() : 0;
      if ((fl & 2) || rectOutside(a.getKey("/Rect"), box)) continue;
      auto [parent, parentLang] = structParentOf(a);
      std::string pstd = stdOf(parent);
      std::string alt = parent.isDictionary() ? textOf(parent.getKey("/Alt")) : "";
      std::string contents = textOf(a.getKey("/Contents"));
      bool annotLang = st.catalogLang || a.getKey("/Lang").isString() || parentLang;
      if (sub == "/TrapNet") annotRules["7.18.2-1"].insert(pageNo);
      if (sub == "/PrinterMark") {
        if (parent.isDictionary()) annotRules["7.18.8-1"].insert(pageNo);
        continue;
      }
      if (sub == "/Popup") continue;
      if (a.hasKey("/Contents") && !annotLang) annotRules["7.2-24"].insert(pageNo);
      if (sub == "/Widget") {
        QPDFObjectHandle f = a;
        for (int up = 0; !f.hasKey("/T") && f.getKey("/Parent").isDictionary() && up < 32; ++up) f = f.getKey("/Parent");
        std::string tu = textOf(f.getKey("/TU"));
        if (f.hasKey("/TU") && !st.catalogLang && !f.getKey("/Lang").isString() && !parentLang) {
          annotRules["7.2-25"].insert(pageNo);
        }
        if (pstd != "/Form") annotRules["7.18.4-1"].insert(pageNo);
        if (tu.empty() && alt.empty()) annotRules["7.18.1-3"].insert(pageNo);
        continue;
      }
      if (sub == "/Link") {
        if (pstd != "/Link") annotRules["7.18.5-1"].insert(pageNo);
        if (contents.empty()) annotRules["7.18.5-2"].insert(pageNo);
      } else if (pstd != "/Annot") {
        annotRules["7.18.1-1"].insert(pageNo);
      }
      if (contents.empty() && alt.empty()) annotRules["7.18.1-2"].insert(pageNo);
    }
  }
  for (const auto& [og, n] : st.taggedFormDraws) {
    if (n > 1) {
      add("7.20-2", "a form XObject with tagged content is drawn more than once");
      break;
    }
  }
  if (!untaggedPages.empty()) add("01-005", "content is neither tagged nor marked as an artifact", untaggedPages);
  if (!textLangPages.empty()) add("7.2-34", "text has no natural language (no /Lang on it, its structure or the catalog)", textLangPages);
  if (st.spanNoLang) add("7.2-30", "a Span with alternate text has no natural language");
  if (st.badLang) add("7.2-29", std::to_string(st.badLang) + " /Lang value(s) are not valid language tags");
  for (const auto& [rule, pages] : annotRules) add(rule, "annotation rule " + rule + " of ISO 14289-1 is violated", pages);
  if (st.nested) add("01-004", std::to_string(st.nested) + " tagged content sequence(s) nested inside an artifact, or the reverse");
  return out;
}

std::vector<UaFinding> uaCheck(const unsigned char* data, size_t size, const std::string& password) {
  QPDF q;
  q.setSuppressWarnings(true);
  q.processMemoryFile("ua", reinterpret_cast<const char*>(data), size, password.c_str());
  std::vector<UaFinding> out = uaMachineCheck(q);
  static const std::map<std::string, std::string> fontRules = {
      {"CMAP_ROS_SYNCED", "7.21.3.1"},        {"CIDSYSTEMINFO_SYNCED", "7.21.3.1"},
      {"CIDTOGIDMAP_SET", "7.21.3.2"},        {"CMAP_FALLBACK_EMBEDDED", "7.21.3.3"},
      {"CMAP_WMODE_SYNCED", "7.21.3.3"},      {"CMAP_USECMAP_REMOVED", "7.21.3.3"},
      {"FONT_SUBSTITUTED", "7.21.4.1"},       {"FONT_NOT_EMBEDDED", "7.21.4.1"},
      {"CHARSET_ADDED", "7.21.4.2"},          {"CIDSET_ADDED", "7.21.4.2"},
      {"FONT_WIDTHS_SYNCED", "7.21.5"},       {"TTF_ENCODING_SET", "7.21.6"},
      {"ENCODING_DIFFS_CLEANED", "7.21.6"},   {"SYMBOLIC_ENCODING_REMOVED", "7.21.6"},
      {"TOUNICODE_BUILT", "7.21.7"},          {"TOUNICODE_SCRUBBED", "7.21.7"},
      {"MISSING_GLYPHS_REMOVED", "7.21.8"}};
  Options o;
  o.level = Level::A2U;
  o.verifyOnly = true;
  o.password = password;
  Result r = convert(data, size, o);
  for (const Issue& i : r.issues) {
    auto it = fontRules.find(i.code);
    if (it != fontRules.end() && i.fixed) out.push_back({it->second, i.detail, std::set<int>(i.pages.begin(), i.pages.end())});
  }
  return out;
}
}
