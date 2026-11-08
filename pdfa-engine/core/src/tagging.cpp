#include <cstdio>
#include <cstdlib>
#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFNameTreeObjectHelper.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "ctx.hh"
#include "uacheck.hh"
#include "passes.hh"
#include "util.hh"

namespace pdfa {
bool pdfaLangTag(const std::string& s, int part) {
  if (s.empty()) return true;
  size_t i = 0;
  for (int seg = 0;; ++seg) {
    size_t start = i;
    while (i < s.size() && s[i] != '-') {
      unsigned char c = static_cast<unsigned char>(s[i]);
      if (c >= 0x80 || !(std::isalpha(c) || (seg > 0 && part >= 2 && std::isdigit(c)))) return false;
      ++i;
    }
    if (i - start < 1 || i - start > 8) return false;
    if (i == s.size()) return true;
    ++i;
  }
}

namespace {
bool validLangTag(const std::string& s) {
  if (s.empty()) return false;
  size_t i = 0;
  int seg = 0;
  while (i < s.size()) {
    size_t start = i;
    while (i < s.size() && s[i] != '-') ++i;
    size_t len = i - start;
    if (len < 1 || len > 8) return false;
    int alphas = 0, digits = 0;
    for (size_t j = start; j < start + len; ++j) {
      unsigned char c = static_cast<unsigned char>(s[j]);
      if (c >= 0x80) return false;
      if (std::isalpha(c)) ++alphas;
      else if (std::isdigit(c)) ++digits;
      else return false;
    }
    if (seg == 0) {
      if (digits || len > 8) return false;
    } else if (len == 1) {
      if (!alphas) return false;
    } else if (len == 2) {
      if (digits) return false;
    } else if (len == 3) {
      if (alphas && digits) return false;
    } else if (len == 4) {
      if (digits && alphas) {
        if (!std::isdigit(static_cast<unsigned char>(s[start]))) return false;
      }
    }
    ++seg;
    if (i < s.size()) ++i;
  }
  return seg >= 1;
}

const std::set<std::string> kStandardStructTypes = {
    "/Document", "/Part", "/Art", "/Sect", "/Div", "/BlockQuote", "/Caption", "/TOC",
    "/TOCI", "/Index", "/NonStruct", "/Private", "/P", "/H", "/H1", "/H2", "/H3", "/H4",
    "/H5", "/H6", "/L", "/LI", "/Lbl", "/LBody", "/Table", "/TR", "/TH", "/TD", "/THead",
    "/TBody", "/TFoot", "/Span", "/Quote", "/Note", "/Reference", "/BibEntry", "/Code",
    "/Link", "/Annot", "/Ruby", "/RB", "/RT", "/RP", "/Warichu", "/WT", "/WP", "/Figure",
    "/Formula", "/Form"};

void fixRoleMap(Ctx& ctx) {
  QPDFObjectHandle root = ctx.pdf.getRoot();
  QPDFObjectHandle str = root.getKey("/StructTreeRoot");
  if (!str.isDictionary()) return;
  std::set<std::string> standard = kStandardStructTypes;
  if (ctx.part == 1) {
    for (const char* t14 : {"/THead", "/TBody", "/TFoot", "/Ruby", "/RB", "/RT", "/RP",
                            "/Warichu", "/WT", "/WP"}) {
      standard.erase(t14);
    }
  }
  std::set<std::string> usedNonStandard;
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (!obj.isDictionary()) continue;
    bool isElem = nameIs(obj.getKey("/Type"), "/StructElem") ||
                  (obj.getKey("/S").isName() && !obj.getKey("/Type").isName() &&
                   (obj.hasKey("/P") || obj.hasKey("/K")));
    if (isElem) {
      std::string s = nameOf(obj.getKey("/S"));
      if (!s.empty() && standard.count(s) == 0) usedNonStandard.insert(s);
    }
  }
  std::set<std::string> reachedNonStandard;
  std::set<QPDFObjGen> seen;
  std::function<void(QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle k, int depth) {
    if (depth > 512) return;
    if (k.isArray()) {
      for (int i = 0; i < k.getArrayNItems(); ++i) walk(k.getArrayItem(i), depth + 1);
      return;
    }
    if (!k.isDictionary() || (k.isIndirect() && !seen.insert(k.getObjGen()).second)) return;
    std::string s = nameOf(k.getKey("/S"));
    if (s.empty()) return;
    if (standard.count(s) == 0) reachedNonStandard.insert(s);
    walk(k.getKey("/K"), depth + 1);
  };
  walk(str.getKey("/K"), 0);
  QPDFObjectHandle rm = str.getKey("/RoleMap");
  bool violation = false;
  for (const std::string& t : reachedNonStandard) {
    std::set<std::string> chain{t};
    std::string cur = t;
    bool ok = false;
    while (rm.isDictionary() && rm.getKey(cur).isName()) {
      cur = rm.getKey(cur).getName();
      if (standard.count(cur)) {
        ok = true;
        break;
      }
      if (!chain.insert(cur).second) break;
    }
    if (!ok) violation = true;
  }
  if (!rm.isDictionary()) {
    if (usedNonStandard.empty()) return;
    str.replaceKey("/RoleMap", QPDFObjectHandle::newDictionary());
    rm = str.getKey("/RoleMap");
  }
  bool changed = false;
  for (const std::string& k : rm.getKeys()) {
    if (standard.count(k)) {
      rm.removeKey(k);
      changed = true;
    }
  }
  for (const std::string& k : rm.getKeys()) {
    std::set<std::string> chain{k};
    std::string cur = k;
    bool terminated = false;
    for (int hop = 0; hop < 16; ++hop) {
      QPDFObjectHandle v = rm.getKey(cur);
      if (!v.isName()) break;
      std::string next = v.getName();
      if (standard.count(next)) {
        terminated = true;
        break;
      }
      if (chain.count(next) || !rm.hasKey(next)) break;
      chain.insert(next);
      cur = next;
    }
    if (!terminated) {
      rm.replaceKey(k, QPDFObjectHandle::newName("/P"));
      changed = true;
    }
  }
  for (const std::string& t2 : usedNonStandard) {
    if (!rm.hasKey(t2)) {
      rm.replaceKey(t2, QPDFObjectHandle::newName("/P"));
      changed = true;
    }
  }
  if (changed && (violation || !ctx.opt.verifyOnly || ctx.opt.ua)) {
    ctx.issue("ROLEMAP_NORMALIZED",
              "normalized structure role map (standard types unmapped, cycles broken, "
              "non-standard types mapped)",
              true);
  }
}

void fixLang(Ctx& ctx, QPDFObjectHandle dict, const std::string& where) {
  QPDFObjectHandle lang = dict.getKey("/Lang");
  if (lang.isNull()) return;
  bool pdfaRule = !ctx.opt.ua && ctx.isA() && ctx.conf == 'A';
  if (pdfaRule && ctx.opt.verifyOnly && !lang.isString()) return;
  bool bad = pdfaRule ? !lang.isString() || !pdfaLangTag(lang.getUTF8Value(), ctx.part)
                      : !lang.isString() || (!validLangTag(lang.getUTF8Value()) &&
                                             !(ctx.opt.verifyOnly && lang.getUTF8Value().empty()));
  if (bad) {
    dict.removeKey("/Lang");
    ctx.issue("LANG_REMOVED", "removed invalid language tag from " + where, true);
    return;
  }
  std::string tag = lang.getUTF8Value(), canon;
  for (size_t i = 0, seg = 0; i <= tag.size(); ++seg) {
    size_t j = std::min(tag.find('-', i), tag.size());
    std::string part = tag.substr(i, j - i);
    for (size_t k = 0; k < part.size(); ++k) {
      unsigned char c = static_cast<unsigned char>(part[k]);
      bool upper = seg > 0 && (part.size() == 2 || (part.size() == 4 && k == 0));
      part[k] = static_cast<char>(upper ? std::toupper(c) : std::tolower(c));
    }
    canon += (seg ? "-" : "") + part;
    i = j + 1;
  }
  if (canon != tag && !ctx.opt.verifyOnly) {
    dict.replaceKey("/Lang", QPDFObjectHandle::newUnicodeString(canon));
    ctx.issue("LANG_NORMALIZED", "wrote the language tag of " + where + " in standard letter case", true);
  }
}

struct Segment {
  bool figure;
  int mcid;
};

class StripMcFilter : public QPDFObjectHandle::TokenFilter {
 public:
  void handleToken(QPDFTokenizer::Token const& token) override {
    QPDFTokenizer::token_type_e type = token.getType();
    if (type != QPDFTokenizer::tt_word) {
      operands.push_back(token);
      return;
    }
    std::string op = token.getValue();
    if (op == "BMC" || op == "BDC" || op == "EMC") {
      operands.clear();
      return;
    }
    flush();
    writeToken(token);
  }
  void handleEOF() override { flush(); }

 private:
  void flush() {
    for (const auto& t : operands) writeToken(t);
    operands.clear();
  }
  std::vector<QPDFTokenizer::Token> operands;
};

void stripFormMarkedContent(Ctx& ctx, QPDFObjectHandle res, Visited& visited) {
  DepthGuard g_(visited);
  if (g_.over) return;
  if (!res.isDictionary() || !visited.enter(res)) return;
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (!xod.isDictionary()) return;
  for (const std::string& k : xod.getKeys()) {
    QPDFObjectHandle xo = xod.getKey(k);
    if (!xo.isStream() || !visited.enter(xo)) continue;
    if (!nameIs(xo.getDict().getKey("/Subtype"), "/Form")) continue;
    xo.getDict().removeKey("/StructParents");
    xo.getDict().removeKey("/StructParent");
    try {
      QPDFPageObjectHelper ph(xo);
      StripMcFilter filter;
      Pl_Buffer buf("ua form strip");
      ph.filterContents(&filter, &buf);
      auto data = buf.getBufferSharedPointer();
      std::string rewritten(reinterpret_cast<const char*>(data->getBuffer()),
                            data->getSize());
      xo.replaceStreamData(rewritten, QPDFObjectHandle::newNull(),
                           QPDFObjectHandle::newNull());
    } catch (...) {
      ctx.scanIncomplete("a form XObject being retagged");
    }
    stripFormMarkedContent(ctx, xo.getDict().getKey("/Resources"), visited);
  }
}

class TagWrapFilter : public QPDFObjectHandle::TokenFilter {
 public:
  TagWrapFilter(std::set<std::string> imageNames) : imageNames(std::move(imageNames)) {}

  std::vector<Segment> segments;
  bool sawContent = false;

  void handleToken(QPDFTokenizer::Token const& token) override {
    QPDFTokenizer::token_type_e type = token.getType();
    if (type == QPDFTokenizer::tt_inline_image) {
      flushOperands();
      writeToken(token);
      return;
    }
    if (type != QPDFTokenizer::tt_word) {
      operands.push_back(token);
      return;
    }
    std::string op = token.getValue();
    if (op == "BT") {
      ++btDepth;
    } else if (op == "ET") {
      if (btDepth > 0) --btDepth;
    }
    if (op == "BMC" || op == "BDC") {
      std::string tag;
      for (const auto& t : operands) {
        if (t.getType() == QPDFTokenizer::tt_name) {
          tag = t.getValue();
          break;
        }
      }
      if (tag == "/Artifact") {
        closeP();
        flushOperands();
        writeToken(token);
        mcStack.push_back(true);
      } else {
        operands.clear();
        mcStack.push_back(false);
      }
      return;
    }
    if (op == "EMC") {
      if (!mcStack.empty()) {
        bool kept = mcStack.back();
        mcStack.pop_back();
        if (kept) {
          flushOperands();
          writeToken(token);
        } else {
          operands.clear();
        }
      } else {
        operands.clear();
      }
      return;
    }
    bool inArtifact = false;
    for (bool kept : mcStack) {
      if (kept) inArtifact = true;
    }
    if (op == "Do" && !inArtifact && btDepth == 0) {
      std::string name;
      for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
        if (it->getType() == QPDFTokenizer::tt_name) {
          name = it->getValue();
          break;
        }
      }
      if (imageNames.count(name)) {
        closeP();
        write("/Figure << /MCID " + std::to_string(nextMcid) + " >> BDC ");
        segments.push_back({true, nextMcid});
        ++nextMcid;
        flushOperands();
        writeToken(token);
        write(" EMC ");
        sawContent = true;
        return;
      }
    }
    if (!inArtifact) openP();
    flushOperands();
    writeToken(token);
    sawContent = true;
  }

  void handleEOF() override {
    flushOperands();
    closeP();
  }

 private:
  void openP() {
    bool inArtifact = false;
    for (bool kept : mcStack) {
      if (kept) inArtifact = true;
    }
    if (pOpen || inArtifact) return;
    write("/P << /MCID " + std::to_string(nextMcid) + " >> BDC\n");
    segments.push_back({false, nextMcid});
    ++nextMcid;
    pOpen = true;
  }

  void closeP() {
    if (!pOpen) return;
    write("\nEMC\n");
    pOpen = false;
  }

  void flushOperands() {
    for (const auto& t : operands) writeToken(t);
    operands.clear();
  }

  std::set<std::string> imageNames;
  std::vector<QPDFTokenizer::Token> operands;
  std::vector<bool> mcStack;
  int btDepth = 0;
  int nextMcid = 0;
  bool pOpen = false;
};

QPDFObjectHandle makeElem(Ctx& ctx, const std::string& type, QPDFObjectHandle parent,
                          QPDFObjectHandle page, QPDFObjectHandle nsRef) {
  QPDFObjectHandle e = QPDFObjectHandle::newDictionary();
  e.replaceKey("/Type", QPDFObjectHandle::newName("/StructElem"));
  e.replaceKey("/S", QPDFObjectHandle::newName(type));
  e.replaceKey("/P", parent);
  if (page.isInitialized()) e.replaceKey("/Pg", page);
  if (nsRef.isInitialized()) e.replaceKey("/NS", nsRef);
  return ctx.pdf.makeIndirectObject(e);
}

void appendKid(QPDFObjectHandle elem, QPDFObjectHandle kid) {
  QPDFObjectHandle k = elem.getKey("/K");
  if (!k.isArray()) {
    QPDFObjectHandle arr = QPDFObjectHandle::newArray();
    if (!k.isNull()) arr.appendItem(k);
    elem.replaceKey("/K", arr);
    k = elem.getKey("/K");
  }
  k.appendItem(kid);
}

std::set<std::string> pageImageNames(QPDFPageObjectHelper& ph) {
  std::set<std::string> names;
  QPDFObjectHandle res = ph.getAttribute("/Resources", false);
  if (!res.isDictionary()) return names;
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (!xod.isDictionary()) return names;
  for (const std::string& k : xod.getKeys()) {
    QPDFObjectHandle xo = xod.getKey(k);
    if (xo.isStream() && nameIs(xo.getDict().getKey("/Subtype"), "/Image")) {
      names.insert(k);
    }
  }
  return names;
}

bool contentsDecodable(QPDFObjectHandle page) {
  QPDFObjectHandle contents = page.getKey("/Contents");
  std::vector<QPDFObjectHandle> streams;
  if (contents.isStream()) streams.push_back(contents);
  if (contents.isArray()) {
    for (int i = 0; i < contents.getArrayNItems(); ++i) {
      if (contents.getArrayItem(i).isStream()) streams.push_back(contents.getArrayItem(i));
    }
  }
  for (QPDFObjectHandle s : streams) {
    try {
      s.getStreamData(qpdf_dl_generalized);
    } catch (...) {
      return false;
    }
  }
  return true;
}

bool artifactWrapped(const std::string& s) {
  size_t i = s.find_first_not_of(" \t\r\n");
  return i != std::string::npos && s.compare(i, 9, "/Artifact") == 0;
}

int wrapAppearanceAsArtifact(QPDFObjectHandle a) {
  QPDFObjectHandle ap = a.getKey("/AP");
  if (!ap.isDictionary()) return 0;
  int n = 0;
  auto wrap = [&](QPDFObjectHandle st) {
    if (!st.isStream()) return;
    std::string data;
    try {
      auto buf = st.getStreamData(qpdf_dl_generalized);
      data.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    } catch (...) {
      return;
    }
    if (artifactWrapped(data)) return;
    st.replaceStreamData("/Artifact BMC\n" + data + "\nEMC\n", QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    ++n;
  };
  for (const char* k : {"/N", "/D", "/R"}) {
    QPDFObjectHandle v = ap.getKey(k);
    if (v.isStream()) {
      wrap(v);
    } else if (v.isDictionary()) {
      for (const std::string& state : v.getKeys()) wrap(v.getKey(state));
    }
  }
  return n;
}

void fillAnnotText(QPDFObjectHandle a) {
  std::string subtype = nameOf(a.getKey("/Subtype"));
  if (subtype == "/Widget") {
    std::string tu = a.getKey("/T").isString() ? a.getKey("/T").getUTF8Value() : std::string("Form field");
    QPDFObjectHandle node = a;
    for (int hop = 0; hop < 16 && node.isDictionary(); ++hop) {
      if (!node.getKey("/TU").isString() || node.getKey("/TU").getUTF8Value().empty()) {
        node.replaceKey("/TU", QPDFObjectHandle::newUnicodeString(tu));
      }
      node = node.getKey("/Parent");
    }
    if (!a.getKey("/Contents").isString() || a.getKey("/Contents").getUTF8Value().empty()) {
      a.replaceKey("/Contents", QPDFObjectHandle::newUnicodeString(tu));
    }
    return;
  }
  if (a.getKey("/Contents").isString() && !a.getKey("/Contents").getUTF8Value().empty()) return;
  std::string alt = subtype == "/Link" ? "Link"
                    : (subtype.size() > 1 ? subtype.substr(1) : std::string("Annotation")) + " annotation";
  QPDFObjectHandle parent = a.getKey("/Parent");
  if (subtype == "/Popup" && parent.isDictionary() && parent.getKey("/Contents").isString() &&
      !parent.getKey("/Contents").getUTF8Value().empty()) {
    alt = parent.getKey("/Contents").getUTF8Value();
  }
  if (subtype == "/Link") {
    QPDFObjectHandle act = a.getKey("/A");
    if (act.isDictionary() && act.getKey("/URI").isString()) alt = act.getKey("/URI").getUTF8Value();
  }
  a.replaceKey("/Contents", QPDFObjectHandle::newUnicodeString(alt));
}

QPDFObjectHandle annotElem(Ctx& ctx, QPDFObjectHandle a, QPDFObjectHandle page, QPDFObjectHandle docElem,
                           QPDFObjectHandle nsRef) {
  std::string subtype = nameOf(a.getKey("/Subtype"));
  std::string elemType = subtype == "/Link" ? "/Link" : subtype == "/Widget" ? "/Form" : "/Annot";
  QPDFObjectHandle elem = makeElem(ctx, elemType, docElem, page, nsRef);
  QPDFObjectHandle objr = QPDFObjectHandle::newDictionary();
  objr.replaceKey("/Type", QPDFObjectHandle::newName("/OBJR"));
  objr.replaceKey("/Pg", page);
  objr.replaceKey("/Obj", a);
  elem.replaceKey("/K", objr);
  appendKid(docElem, elem);
  fillAnnotText(a);
  return elem;
}

struct TreeIndex {
  std::map<std::pair<QPDFObjGen, int>, QPDFObjectHandle> mc;
  std::map<QPDFObjGen, QPDFObjectHandle> stms;
  std::map<QPDFObjGen, std::pair<QPDFObjectHandle, QPDFObjectHandle>> objr;
  std::vector<QPDFObjectHandle> figures;
  std::vector<QPDFObjectHandle> rows;
  std::vector<QPDFObjectHandle> formulas;
  std::vector<QPDFObjectHandle> notes;
  std::vector<QPDFObjectHandle> tables;
  int elems = 0;
  int madeIndirect = 0;
};

std::string mappedType(QPDFObjectHandle rm, std::string s) {
  for (int hop = 0; hop < 16 && rm.isDictionary() && !kStandardStructTypes.count(s); ++hop) {
    QPDFObjectHandle v = rm.getKey(s);
    if (!v.isName()) break;
    s = v.getName();
  }
  return s;
}

void indexKids(Ctx& ctx, QPDFObjectHandle parent, QPDFObjectHandle pg, QPDFObjectHandle rm, TreeIndex& ix,
               std::set<QPDFObjGen>& seen, int depth) {
  if (depth > 256) return;
  QPDFObjectHandle k = parent.getKey("/K");
  bool arr = k.isArray();
  int n = arr ? k.getArrayNItems() : (k.isNull() ? 0 : 1);
  for (int i = 0; i < n; ++i) {
    QPDFObjectHandle kid = arr ? k.getArrayItem(i) : k;
    if (kid.isInteger()) {
      if (pg.isIndirect()) ix.mc[{pg.getObjGen(), static_cast<int>(kid.getIntValue())}] = parent;
      continue;
    }
    if (!kid.isDictionary()) continue;
    std::string type = nameOf(kid.getKey("/Type"));
    if (type == "/MCR" || (kid.getKey("/MCID").isInteger() && !kid.hasKey("/S"))) {
      if (!kid.getKey("/MCID").isInteger()) continue;
      int mcid = static_cast<int>(kid.getKey("/MCID").getIntValue());
      QPDFObjectHandle stm = kid.getKey("/Stm");
      QPDFObjectHandle kpg = kid.getKey("/Pg").isDictionary() ? kid.getKey("/Pg") : pg;
      if (stm.isStream() && stm.isIndirect()) {
        ix.mc[{stm.getObjGen(), mcid}] = parent;
        ix.stms[stm.getObjGen()] = stm;
      } else if (kpg.isIndirect()) {
        ix.mc[{kpg.getObjGen(), mcid}] = parent;
      }
      continue;
    }
    if (type == "/OBJR") {
      QPDFObjectHandle o = kid.getKey("/Obj");
      if (o.isIndirect() && (o.isDictionary() || o.isStream())) ix.objr[o.getObjGen()] = {o, parent};
      continue;
    }
    if (!kid.getKey("/S").isName()) continue;
    if (!kid.isIndirect()) {
      kid = ctx.pdf.makeIndirectObject(kid);
      if (arr) k.setArrayItem(i, kid);
      else parent.replaceKey("/K", kid);
      ++ix.madeIndirect;
    }
    if (!seen.insert(kid.getObjGen()).second) continue;
    kid.replaceKey("/P", parent);
    ++ix.elems;
    std::string mapped = mappedType(rm, kid.getKey("/S").getName());
    if (mapped == "/Figure") ix.figures.push_back(kid);
    if (mapped == "/TR") ix.rows.push_back(kid);
    if (mapped == "/Formula") ix.formulas.push_back(kid);
    if (mapped == "/Note") ix.notes.push_back(kid);
    if (mapped == "/Table") ix.tables.push_back(kid);
    indexKids(ctx, kid, kid.getKey("/Pg").isDictionary() ? kid.getKey("/Pg") : pg, rm, ix, seen, depth + 1);
  }
}

void readNumTree(QPDFObjectHandle node, std::map<long long, QPDFObjectHandle>& out, std::set<QPDFObjGen>& seen,
                 int depth) {
  if (!node.isDictionary() || depth > 32) return;
  if (node.isIndirect() && !seen.insert(node.getObjGen()).second) return;
  QPDFObjectHandle nums = node.getKey("/Nums");
  for (int i = 0; nums.isArray() && i + 1 < nums.getArrayNItems(); i += 2) {
    if (nums.getArrayItem(i).isInteger()) out[nums.getArrayItem(i).getIntValue()] = nums.getArrayItem(i + 1);
  }
  QPDFObjectHandle kids = node.getKey("/Kids");
  for (int i = 0; kids.isArray() && i < kids.getArrayNItems(); ++i) readNumTree(kids.getArrayItem(i), out, seen, depth + 1);
}

void keepOldParentTree(Ctx& ctx, QPDFObjectHandle tree, TreeIndex& ix) {
  std::map<long long, QPDFObjectHandle> old;
  std::set<QPDFObjGen> seen;
  readNumTree(tree.getKey("/ParentTree"), old, seen, 0);
  auto isElem = [](QPDFObjectHandle e) { return e.isDictionary() && e.isIndirect() && e.getKey("/S").isName(); };
  auto content = [&](QPDFObjectHandle owner, QPDFObjectHandle dict, bool stm) {
    QPDFObjectHandle key = dict.getKey("/StructParents");
    if (!key.isInteger() || !old.count(key.getIntValue())) return;
    QPDFObjectHandle arr = old[key.getIntValue()];
    for (int i = 0; arr.isArray() && i < arr.getArrayNItems(); ++i) {
      if (!isElem(arr.getArrayItem(i)) || ix.mc.count({owner.getObjGen(), i})) continue;
      ix.mc[{owner.getObjGen(), i}] = arr.getArrayItem(i);
      if (stm) ix.stms[owner.getObjGen()] = owner;
    }
  };
  for (auto& ph : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    content(page, page, false);
    QPDFObjectHandle an = page.getKey("/Annots");
    for (int i = 0; an.isArray() && i < an.getArrayNItems(); ++i) {
      QPDFObjectHandle a = an.getArrayItem(i);
      QPDFObjectHandle key = a.isDictionary() ? a.getKey("/StructParent") : QPDFObjectHandle::newNull();
      if (!a.isIndirect() || !key.isInteger() || ix.objr.count(a.getObjGen())) continue;
      auto it = old.find(key.getIntValue());
      if (it != old.end() && isElem(it->second)) ix.objr[a.getObjGen()] = {a, it->second};
    }
  }
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (obj.isStream() && nameIs(obj.getDict().getKey("/Subtype"), "/Form")) content(obj, obj.getDict(), true);
  }
}

int fixCornerCells(QPDFObjectHandle rm, const std::vector<QPDFObjectHandle>& rows) {
  int n = 0;
  for (QPDFObjectHandle tr : rows) {
    QPDFObjectHandle k = tr.getKey("/K");
    if (!k.isArray() || k.getArrayNItems() < 2) continue;
    QPDFObjectHandle first = k.getArrayItem(0);
    if (!first.isDictionary() || !first.getKey("/S").isName() || mappedType(rm, first.getKey("/S").getName()) != "/TD") continue;
    QPDFObjectHandle attr = first.getKey("/A");
    if (attr.unparse().find("/Headers") != std::string::npos) continue;
    bool allTh = true;
    for (int i = 1; i < k.getArrayNItems() && allTh; ++i) {
      QPDFObjectHandle c = k.getArrayItem(i);
      allTh = c.isDictionary() && c.getKey("/S").isName() && mappedType(rm, c.getKey("/S").getName()) == "/TH";
    }
    if (!allTh) continue;
    first.replaceKey("/S", QPDFObjectHandle::newName("/TH"));
    ++n;
  }
  return n;
}

std::string uaStructureProblem(QPDFObjectHandle tree) {
  QPDFObjectHandle rm = tree.getKey("/RoleMap");
  static const std::map<std::string, std::set<std::string>> parents = {
      {"/TR", {"/Table", "/THead", "/TBody", "/TFoot"}}, {"/THead", {"/Table"}}, {"/TBody", {"/Table"}},
      {"/TFoot", {"/Table"}}, {"/TH", {"/TR"}}, {"/TD", {"/TR"}}, {"/LI", {"/L"}}, {"/Lbl", {"/LI"}},
      {"/LBody", {"/LI"}}, {"/TOCI", {"/TOC"}}};
  static const std::map<std::string, std::set<std::string>> children = {
      {"/Table", {"/TR", "/THead", "/TBody", "/TFoot", "/Caption"}}, {"/THead", {"/TR"}}, {"/TBody", {"/TR"}},
      {"/TFoot", {"/TR"}}, {"/TR", {"/TH", "/TD"}}, {"/L", {"/L", "/LI", "/Caption"}}, {"/LI", {"/Lbl", "/LBody"}},
      {"/TOC", {"/TOC", "/TOCI", "/Caption"}}};
  std::string problem;
  bool plainH = false, numberedH = false;
  std::set<QPDFObjGen> seen;
  auto typeOf = [&](QPDFObjectHandle e) { return mappedType(rm, nameOf(e.getKey("/S"))); };
  auto kidsOf = [&](QPDFObjectHandle e) {
    std::vector<QPDFObjectHandle> out;
    QPDFObjectHandle k = e.getKey("/K");
    int n = k.isArray() ? k.getArrayNItems() : (k.isNull() ? 0 : 1);
    for (int i = 0; i < n; ++i) {
      QPDFObjectHandle kid = k.isArray() ? k.getArrayItem(i) : k;
      if (kid.isDictionary() && kid.getKey("/S").isName()) out.push_back(kid);
    }
    return out;
  };
  auto intAttr = [](QPDFObjectHandle e, const char* key) {
    QPDFObjectHandle a = e.getKey("/A");
    std::vector<QPDFObjectHandle> list;
    if (a.isDictionary()) list.push_back(a);
    for (int i = 0; a.isArray() && i < a.getArrayNItems(); ++i) list.push_back(a.getArrayItem(i));
    for (QPDFObjectHandle d : list) {
      if (d.isDictionary() && d.getKey(key).isInteger()) return std::max(1, static_cast<int>(d.getKey(key).getIntValue()));
    }
    return 1;
  };
  std::function<void(QPDFObjectHandle, const std::string&, int)> walk = [&](QPDFObjectHandle e, const std::string& parent,
                                                                           int depth) {
    if (!problem.empty() || depth > 256) return;
    if (e.isIndirect() && !seen.insert(e.getObjGen()).second) return;
    std::string type = typeOf(e);
    auto pr = parents.find(type);
    if (pr != parents.end() && !pr->second.count(parent)) problem = type.substr(1) + " outside " + *pr->second.begin();
    if (type == "/H") plainH = true;
    if (type.size() == 3 && type[1] == 'H' && type[2] >= '1' && type[2] <= '6') numberedH = true;
    std::vector<QPDFObjectHandle> kids = kidsOf(e);
    std::vector<std::string> kt;
    for (QPDFObjectHandle k : kids) kt.push_back(typeOf(k));
    auto ch = children.find(type);
    for (size_t i = 0; ch != children.end() && i < kt.size() && problem.empty(); ++i) {
      if (!ch->second.count(kt[i])) problem = type.substr(1) + " containing " + kt[i].substr(1);
      if (kt[i] == "/Caption" && i != 0 && (type != "/Table" || i + 1 != kt.size())) problem = "misplaced Caption in " + type.substr(1);
    }
    auto count = [&](const char* t) { return std::count(kt.begin(), kt.end(), std::string(t)); };
    if (count("/H") > 1) problem = "more than one H in one element";
    QPDFObjectHandle fk = e.getKey("/K");
    bool widget = false;
    for (int i = 0; type == "/Form" && fk.isArray() && i < fk.getArrayNItems(); ++i) {
      widget = widget || (fk.getArrayItem(i).isDictionary() && nameIs(fk.getArrayItem(i).getKey("/Type"), "/OBJR"));
    }
    if (widget && fk.getArrayNItems() > 1 && e.getKey("/A").unparseResolved().find("/Role") == std::string::npos) {
      problem = "Form element mixing a widget with other content";
    }
    if (type == "/Table" && (count("/Caption") > 1 || count("/THead") > 1 || count("/TFoot") > 1 ||
                             ((count("/THead") || count("/TFoot")) && !count("/TBody")))) {
      problem = "Table section layout";
    }
    if (type == "/Table" && problem.empty()) {
      std::vector<QPDFObjectHandle> rows;
      for (size_t i = 0; i < kids.size(); ++i) {
        if (kt[i] == "/TR") rows.push_back(kids[i]);
        if (kt[i] == "/THead" || kt[i] == "/TBody" || kt[i] == "/TFoot") {
          for (QPDFObjectHandle r : kidsOf(kids[i])) rows.push_back(r);
        }
      }
      std::vector<int> busy;
      int width = -1;
      for (size_t r = 0; r < rows.size() && problem.empty(); ++r) {
        int col = 0;
        std::vector<int> carried = busy;
        for (QPDFObjectHandle c : kidsOf(rows[r])) {
          while (col < static_cast<int>(busy.size()) && busy[col] > 0) ++col;
          int cs = intAttr(c, "/ColSpan"), rs = intAttr(c, "/RowSpan");
          if (static_cast<int>(busy.size()) < col + cs) busy.resize(col + cs, 0);
          for (int j = col; j < col + cs; ++j) busy[j] = std::max(busy[j], rs);
          col += cs;
        }
        int w = col;
        for (int j = 0; j < static_cast<int>(carried.size()); ++j) {
          if (carried[j] > 0) w = std::max(w, j + 1);
        }
        if (width >= 0 && w != width) problem = "table rows with different column counts";
        width = w;
        for (int& b : busy) b = b > 0 ? b - 1 : 0;
      }
    }
    for (QPDFObjectHandle k : kids) walk(k, type, depth + 1);
  };
  for (QPDFObjectHandle k : kidsOf(tree)) walk(k, "", 0);
  if (problem.empty() && plainH && numberedH) problem = "both H and H1-H6 headings";
  return problem;
}

bool uaRepairTree(Ctx& ctx, QPDFObjectHandle tree, std::string& reason) {
  QPDFObjectHandle root = ctx.pdf.getRoot();
  if (!tree.isIndirect()) {
    tree = ctx.pdf.makeIndirectObject(tree);
    root.replaceKey("/StructTreeRoot", tree);
  }
  TreeIndex ix;
  std::set<QPDFObjGen> seen;
  indexKids(ctx, tree, QPDFObjectHandle::newNull(), tree.getKey("/RoleMap"), ix, seen, 0);
  keepOldParentTree(ctx, tree, ix);
  if (ix.mc.empty()) {
    reason = "it referenced no page content";
    return false;
  }
  reason = uaStructureProblem(tree);
  std::map<QPDFObjGen, int> draws;
  for (auto& ph : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) {
    QPDFObjectHandle res = ph.getAttribute("/Resources", false);
    QPDFObjectHandle xo = res.isDictionary() ? res.getKey("/XObject") : QPDFObjectHandle::newNull();
    if (!xo.isDictionary()) continue;
    std::string body;
    for (const std::string& k : xo.getKeys()) {
      QPDFObjectHandle f = xo.getKey(k);
      if (!f.isStream() || !f.isIndirect() || !ix.stms.count(f.getObjGen())) continue;
      if (body.empty()) {
        try {
          for (QPDFObjectHandle c : ph.getPageContents()) {
            auto b = c.getStreamData(qpdf_dl_generalized);
            body.append(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
            body += "\n";
          }
        } catch (...) {
        }
      }
      for (size_t at = body.find(k); at != std::string::npos; at = body.find(k, at + k.size())) {
        size_t e = at + k.size();
        if (e < body.size() && (std::isalnum(static_cast<unsigned char>(body[e])) || body[e] == '_')) continue;
        while (e < body.size() && std::isspace(static_cast<unsigned char>(body[e]))) ++e;
        if (body.compare(e, 2, "Do") == 0) ++draws[f.getObjGen()];
      }
    }
  }
  for (const auto& [og, n] : draws) {
    if (reason.empty() && n > 1) reason = "a tagged form XObject is drawn more than once";
  }
  for (const UaFinding& f : uaMachineCheck(ctx.pdf)) {
    if (reason.empty() && f.checkpoint == "01-004") reason = f.detail;
  }
  if (!reason.empty()) return false;
  std::vector<std::string> changes;
  int corners = fixCornerCells(tree.getKey("/RoleMap"), ix.rows);
  if (corners) changes.push_back("retagged " + std::to_string(corners) + " table corner cell(s) in header rows from TD to TH");
  QPDFObjectHandle k = tree.getKey("/K");
  QPDFObjectHandle docElem;
  if (k.isDictionary() && k.isIndirect()) docElem = k;
  if (k.isArray() && k.getArrayNItems() == 1 && k.getArrayItem(0).isDictionary()) docElem = k.getArrayItem(0);
  if (!docElem.isInitialized()) {
    docElem = makeElem(ctx, "/Document", tree, QPDFObjectHandle(), QPDFObjectHandle());
    for (int i = 0; k.isArray() && i < k.getArrayNItems(); ++i) {
      if (k.getArrayItem(i).isDictionary()) k.getArrayItem(i).replaceKey("/P", docElem);
    }
    if (!k.isNull()) docElem.replaceKey("/K", k);
    tree.replaceKey("/K", docElem);
    changes.push_back("grouped the top-level elements under a Document element");
  }
  int alts = 0;
  for (QPDFObjectHandle fig : ix.figures) {
    bool hasAlt = fig.getKey("/Alt").isString() && !fig.getKey("/Alt").getUTF8Value().empty();
    bool hasActual = fig.getKey("/ActualText").isString() && !fig.getKey("/ActualText").getUTF8Value().empty();
    if (hasAlt || hasActual) continue;
    fig.replaceKey("/Alt", QPDFObjectHandle::newUnicodeString("Image"));
    ++alts;
  }
  for (QPDFObjectHandle f : ix.formulas) {
    if ((f.getKey("/Alt").isString() && !f.getKey("/Alt").getUTF8Value().empty()) ||
        (f.getKey("/ActualText").isString() && !f.getKey("/ActualText").getUTF8Value().empty())) {
      continue;
    }
    f.replaceKey("/Alt", QPDFObjectHandle::newUnicodeString("Formula"));
    ++alts;
  }
  if (alts) changes.push_back("added placeholder alternate text to " + std::to_string(alts) + " figure(s) or formula(s) without any");
  int levels = 0;
  {
    std::vector<QPDFObjectHandle> heads;
    std::set<QPDFObjGen> hseen;
    std::function<void(QPDFObjectHandle, int)> collect = [&](QPDFObjectHandle e, int depth) {
      if (depth > 256 || !e.isDictionary() || (e.isIndirect() && !hseen.insert(e.getObjGen()).second)) return;
      std::string t = mappedType(tree.getKey("/RoleMap"), nameOf(e.getKey("/S")));
      if (t.size() == 3 && t[1] == 'H' && t[2] >= '1' && t[2] <= '6') heads.push_back(e);
      QPDFObjectHandle k = e.getKey("/K");
      for (int i = 0; k.isArray() && i < k.getArrayNItems(); ++i) collect(k.getArrayItem(i), depth + 1);
      if (k.isDictionary()) collect(k, depth + 1);
    };
    collect(docElem, 0);
    int prev = 0;
    for (QPDFObjectHandle h : heads) {
      int lvl = mappedType(tree.getKey("/RoleMap"), nameOf(h.getKey("/S")))[2] - '0';
      int fixed = std::min(lvl, prev + 1);
      if (fixed != lvl) {
        h.replaceKey("/S", QPDFObjectHandle::newName("/H" + std::to_string(fixed)));
        ++levels;
      }
      prev = fixed;
    }
  }
  if (levels) changes.push_back("renumbered " + std::to_string(levels) + " heading(s) that skipped a level");
  std::set<std::string> noteIds;
  int ids = 0;
  for (QPDFObjectHandle note : ix.notes) {
    std::string id = note.getKey("/ID").isString() ? note.getKey("/ID").getStringValue() : std::string();
    if (!id.empty() && noteIds.insert(id).second) continue;
    std::string fresh;
    for (int n = 1; fresh.empty() || noteIds.count(fresh); ++n) fresh = "KuraNote" + std::to_string(n);
    noteIds.insert(fresh);
    note.replaceKey("/ID", QPDFObjectHandle::newString(fresh));
    ++ids;
  }
  if (ids) changes.push_back("gave " + std::to_string(ids) + " Note element(s) a unique ID");
  int scopes = 0;
  for (QPDFObjectHandle table : ix.tables) {
    if (table.unparseResolved().find("/Headers") != std::string::npos) continue;
    for (QPDFObjectHandle tr : ix.rows) {
      QPDFObjectHandle p = tr.getKey("/P");
      if (!(p.isIndirect() && (p.getObjGen() == table.getObjGen() || (p.getKey("/P").isIndirect() && p.getKey("/P").getObjGen() == table.getObjGen())))) continue;
      QPDFObjectHandle rowKids = tr.getKey("/K");
      int n = rowKids.isArray() ? rowKids.getArrayNItems() : 1;
      bool allTh = true;
      for (int i = 0; i < n; ++i) {
        QPDFObjectHandle c = rowKids.isArray() ? rowKids.getArrayItem(i) : rowKids;
        allTh = allTh && c.isDictionary() && mappedType(tree.getKey("/RoleMap"), nameOf(c.getKey("/S"))) == "/TH";
      }
      for (int i = 0; i < n; ++i) {
        QPDFObjectHandle c = rowKids.isArray() ? rowKids.getArrayItem(i) : rowKids;
        if (!c.isDictionary() || mappedType(tree.getKey("/RoleMap"), nameOf(c.getKey("/S"))) != "/TH") continue;
        if (c.getKey("/A").unparseResolved().find("/Scope") != std::string::npos) continue;
        QPDFObjectHandle attr = QPDFObjectHandle::parse("<< /O /Table >>");
        attr.replaceKey("/Scope", QPDFObjectHandle::newName(allTh ? "/Column" : i == 0 ? "/Row" : "/Both"));
        QPDFObjectHandle a = c.getKey("/A");
        if (a.isArray()) {
          a.appendItem(attr);
        } else if (a.isDictionary() || a.isStream()) {
          QPDFObjectHandle arr = QPDFObjectHandle::newArray();
          arr.appendItem(a);
          arr.appendItem(attr);
          c.replaceKey("/A", arr);
        } else {
          c.replaceKey("/A", attr);
        }
        ++scopes;
      }
    }
  }
  if (scopes) changes.push_back("set Scope on " + std::to_string(scopes) + " table header cell(s) of tables without Headers");
  int annots = 0;
  std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(ctx.pdf).getAllPages();
  for (auto& ph : pages) {
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle an = page.getKey("/Annots");
    if (!an.isArray() || an.getArrayNItems() == 0) continue;
    if (!nameIs(page.getKey("/Tabs"), "/S")) page.replaceKey("/Tabs", QPDFObjectHandle::newName("/S"));
    for (int i = 0; i < an.getArrayNItems(); ++i) {
      QPDFObjectHandle a = an.getArrayItem(i);
      if (!a.isDictionary()) continue;
      if (nameIs(a.getKey("/Subtype"), "/PrinterMark")) {
        fillAnnotText(a);
        wrapAppearanceAsArtifact(a);
        auto it = a.isIndirect() ? ix.objr.find(a.getObjGen()) : ix.objr.end();
        if (it != ix.objr.end()) {
          QPDFObjectHandle e = it->second.second;
          QPDFObjectHandle kids = e.getKey("/K");
          if (kids.isDictionary() && kids.getKey("/Obj").isIndirect() && kids.getKey("/Obj").getObjGen() == a.getObjGen()) e.removeKey("/K");
          for (int j = kids.isArray() ? kids.getArrayNItems() - 1 : -1; j >= 0; --j) {
            QPDFObjectHandle o = kids.getArrayItem(j).isDictionary() ? kids.getArrayItem(j).getKey("/Obj") : QPDFObjectHandle::newNull();
            if (o.isIndirect() && o.getObjGen() == a.getObjGen()) kids.eraseItem(j);
          }
          ix.objr.erase(it);
          ++annots;
        }
        continue;
      }
      if (!a.isIndirect()) {
        a = ctx.pdf.makeIndirectObject(a);
        an.setArrayItem(i, a);
      }
      if (ix.objr.count(a.getObjGen())) {
        fillAnnotText(a);
        std::string sub = nameOf(a.getKey("/Subtype"));
        std::string want = sub == "/Link" ? "/Link" : sub == "/Widget" ? "/Form" : "/Annot";
        QPDFObjectHandle e = ix.objr[a.getObjGen()].second;
        if (mappedType(tree.getKey("/RoleMap"), nameOf(e.getKey("/S"))) != want) {
          QPDFObjectHandle kids = e.getKey("/K");
          if (!kids.isArray() || kids.getArrayNItems() == 1) {
            e.replaceKey("/S", QPDFObjectHandle::newName(want));
          } else {
            for (int j = 0; j < kids.getArrayNItems(); ++j) {
              QPDFObjectHandle o = kids.getArrayItem(j);
              if (!o.isDictionary() || !o.getKey("/Obj").isIndirect() || o.getKey("/Obj").getObjGen() != a.getObjGen()) continue;
              QPDFObjectHandle wrap = makeElem(ctx, want, e, page, QPDFObjectHandle());
              wrap.replaceKey("/K", o);
              kids.setArrayItem(j, wrap);
              ix.objr[a.getObjGen()].second = wrap;
              break;
            }
          }
          ++annots;
        }
        continue;
      }
      ix.objr[a.getObjGen()] = {a, annotElem(ctx, a, page, docElem, QPDFObjectHandle())};
      ++annots;
    }
  }
  if (annots) changes.push_back("tagged or retyped " + std::to_string(annots) + " annotation structure element(s)");
  int adopted = 0;
  for (auto& ph : pages) {
    QPDFObjectHandle page = ph.getObjectHandle();
    std::string body;
    try {
      for (QPDFObjectHandle c : ph.getPageContents()) {
        auto b = c.getStreamData(qpdf_dl_generalized);
        body.append(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
        body += "\n";
      }
    } catch (...) {
      continue;
    }
    std::set<int> found;
    for (size_t at = body.find("/MCID"); at != std::string::npos; at = body.find("/MCID", at + 5)) {
      size_t p = at + 5;
      while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p]))) ++p;
      size_t q = p;
      while (q < body.size() && std::isdigit(static_cast<unsigned char>(body[q]))) ++q;
      if (q > p && q - p < 7) found.insert(std::stoi(body.substr(p, q - p)));
    }
    QPDFObjectHandle holder;
    for (int mcid : found) {
      if (ix.mc.count({page.getObjGen(), mcid})) continue;
      if (!holder.isInitialized()) {
        holder = makeElem(ctx, "/P", docElem, page, QPDFObjectHandle());
        appendKid(docElem, holder);
      }
      appendKid(holder, QPDFObjectHandle::newInteger(mcid));
      ix.mc[{page.getObjGen(), mcid}] = holder;
      ++adopted;
    }
  }
  if (adopted) changes.push_back("attached " + std::to_string(adopted) + " tagged content item(s) missing from the structure tree");
  std::map<QPDFObjGen, std::map<int, QPDFObjectHandle>> owners;
  for (const auto& [key, elem] : ix.mc) owners[key.first][key.second] = elem;
  QPDFObjectHandle nums = QPDFObjectHandle::newArray();
  int next = 0;
  auto add = [&](QPDFObjectHandle holder, const char* key, QPDFObjectHandle value) {
    holder.replaceKey(key, QPDFObjectHandle::newInteger(next));
    nums.appendItem(QPDFObjectHandle::newInteger(next++));
    nums.appendItem(value);
  };
  auto mcArray = [&](const std::map<int, QPDFObjectHandle>& m) {
    QPDFObjectHandle arr = QPDFObjectHandle::newArray();
    for (const auto& [mcid, elem] : m) {
      if (mcid < 0 || mcid > 100000) continue;
      while (arr.getArrayNItems() < mcid) arr.appendItem(QPDFObjectHandle::newNull());
      arr.appendItem(elem);
    }
    return ctx.pdf.makeIndirectObject(arr);
  };
  for (auto& ph : pages) {
    QPDFObjectHandle page = ph.getObjectHandle();
    page.removeKey("/StructParents");
    auto it = owners.find(page.getObjGen());
    if (it != owners.end()) add(page, "/StructParents", mcArray(it->second));
  }
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (!obj.isStream() || !nameIs(obj.getDict().getKey("/Subtype"), "/Form")) continue;
    obj.getDict().removeKey("/StructParents");
    if (!ix.objr.count(obj.getObjGen())) obj.getDict().removeKey("/StructParent");
  }
  for (const auto& [og, stm] : ix.stms) add(stm.getDict(), "/StructParents", mcArray(owners[og]));
  for (auto& ph : pages) {
    QPDFObjectHandle an = ph.getObjectHandle().getKey("/Annots");
    for (int i = 0; an.isArray() && i < an.getArrayNItems(); ++i) {
      QPDFObjectHandle a = an.getArrayItem(i);
      if (a.isDictionary() && !ix.objr.count(a.getObjGen())) a.removeKey("/StructParent");
    }
  }
  for (const auto& [og, target] : ix.objr) {
    QPDFObjectHandle holder = target.first.isStream() ? target.first.getDict() : target.first;
    add(holder, "/StructParent", target.second);
  }
  QPDFObjectHandle parentTree = QPDFObjectHandle::newDictionary();
  parentTree.replaceKey("/Nums", nums);
  tree.replaceKey("/ParentTree", ctx.pdf.makeIndirectObject(parentTree));
  tree.replaceKey("/ParentTreeNextKey", QPDFObjectHandle::newInteger(next));
  for (const std::string& c : changes) {
    ctx.issue("UA_STRUCTURE_REPAIRED", "kept the existing structure tree (" + std::to_string(ix.elems) +
                                           " element(s)) and " + c, true);
  }
  fixRoleMap(ctx);
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (obj.isDictionary() && (nameIs(obj.getKey("/Type"), "/StructElem") || (obj.getKey("/S").isName() && obj.hasKey("/P") && !obj.hasKey("/Type")))) fixLang(ctx, obj, "structure element");
  }
  passSemanticRepair(ctx, false);
  return true;
}

void uaTagging(Ctx& ctx) {
  QPDFObjectHandle root = ctx.pdf.getRoot();

  QPDFObjectHandle markInfo = root.getKey("/MarkInfo");
  if (!markInfo.isDictionary()) {
    markInfo = QPDFObjectHandle::newDictionary();
    root.replaceKey("/MarkInfo", markInfo);
  }
  markInfo.replaceKey("/Marked", QPDFObjectHandle::newBool(true));
  if (markInfo.hasKey("/Suspects")) {
    markInfo.replaceKey("/Suspects", QPDFObjectHandle::newBool(false));
  }

  QPDFObjectHandle lang = root.getKey("/Lang");
  if (!lang.isString() || !validLangTag(lang.getUTF8Value())) {
    root.replaceKey("/Lang", QPDFObjectHandle::newUnicodeString(ctx.docLang()));
    ctx.issue("LANG_SET", "set document language to \"" + ctx.docLang() +
                              "\" (pass docLang to override)",
              true);
  }

  QPDFObjectHandle vp = root.getKey("/ViewerPreferences");
  if (!vp.isDictionary()) {
    vp = QPDFObjectHandle::newDictionary();
    root.replaceKey("/ViewerPreferences", vp);
  }
  vp.replaceKey("/DisplayDocTitle", QPDFObjectHandle::newBool(true));

  QPDFObjectHandle oldTree = root.getKey("/StructTreeRoot");
  std::string reason = "PDF/UA-2 output needs a namespaced tree";
  if (oldTree.isDictionary() && !ctx.ua2() && uaRepairTree(ctx, oldTree, reason)) return;
  if (root.hasKey("/StructTreeRoot")) {
    root.removeKey("/StructTreeRoot");
    ctx.issue("STRUCT_TREE_REBUILT",
              "replaced the existing structure tree with a generated one because " + reason, true);
  }

  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    QPDFObjectHandle dict = obj.isStream() ? obj.getDict() : obj;
    if (!dict.isDictionary()) continue;
    if (dict.hasKey("/StructParent")) dict.removeKey("/StructParent");
    if (dict.hasKey("/StructParents")) dict.removeKey("/StructParents");
  }

  QPDFObjectHandle treeRoot = QPDFObjectHandle::newDictionary();
  treeRoot.replaceKey("/Type", QPDFObjectHandle::newName("/StructTreeRoot"));
  QPDFObjectHandle treeRef = ctx.pdf.makeIndirectObject(treeRoot);
  QPDFObjectHandle gNsRef;
  if (ctx.ua2()) {
    QPDFObjectHandle ns = QPDFObjectHandle::newDictionary();
    ns.replaceKey("/Type", QPDFObjectHandle::newName("/Namespace"));
    ns.replaceKey("/NS", QPDFObjectHandle::newUnicodeString("http://iso.org/pdf2/ssn"));
    gNsRef = ctx.pdf.makeIndirectObject(ns);
    QPDFObjectHandle nsArr = QPDFObjectHandle::newArray();
    nsArr.appendItem(gNsRef);
    treeRoot.replaceKey("/Namespaces", nsArr);
  }
  QPDFObjectHandle docElem = makeElem(ctx, "/Document", treeRef, QPDFObjectHandle(), gNsRef);
  treeRoot.replaceKey("/K", docElem);
  std::map<QPDFObjGen, QPDFObjectHandle> pageFirstElem;
  root.replaceKey("/StructTreeRoot", treeRef);

  QPDFObjectHandle nums = QPDFObjectHandle::newArray();
  int nextKey = 0;
  int figures = 0;
  int wrapped = 0;
  int annotElems = 0;

  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
  int kuraPageNo = 0;
  for (auto& ph : pages) {
    PageScope kuraScope(ctx, ++kuraPageNo);
    QPDFObjectHandle page = ph.getObjectHandle();
    std::vector<Segment> segments;
    {
      Visited formVisited;
      stripFormMarkedContent(ctx, ph.getAttribute("/Resources", false), formVisited);
      QPDFObjectHandle pageAnnots = page.getKey("/Annots");
      if (pageAnnots.isArray()) {
        for (int ai = 0; ai < pageAnnots.getArrayNItems(); ++ai) {
          QPDFObjectHandle a = pageAnnots.getArrayItem(ai);
          if (!a.isDictionary()) continue;
          QPDFObjectHandle apd = a.getKey("/AP");
          if (!apd.isDictionary()) continue;
          QPDFObjectHandle nap = apd.getKey("/N");
          std::vector<QPDFObjectHandle> streams;
          if (nap.isStream()) streams.push_back(nap);
          else if (nap.isDictionary()) {
            for (const std::string& kk : nap.getKeys()) {
              if (nap.getKey(kk).isStream()) streams.push_back(nap.getKey(kk));
            }
          }
          for (QPDFObjectHandle s : streams) {
            stripFormMarkedContent(ctx, s.getDict().getKey("/Resources"), formVisited);
          }
        }
      }
    }
    if (contentsDecodable(page)) {
      TagWrapFilter filter(pageImageNames(ph));
      Pl_Buffer buf("ua tag wrap");
      ph.filterContents(&filter, &buf);
      auto data = buf.getBufferSharedPointer();
      std::string rewritten(reinterpret_cast<const char*>(data->getBuffer()), data->getSize());
      page.replaceKey(
          "/Contents",
          ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, rewritten)));
      segments = filter.segments;
      if (filter.sawContent) ++wrapped;
    } else {
      ctx.issueMerged("CONTENT_UNDECODABLE",
                "page content could not be decoded for tagging; left untagged", false);
    }

    QPDFObjectHandle pElem;
    QPDFObjectHandle pageParents = QPDFObjectHandle::newArray();
    bool anyMcid = false;
    for (const Segment& seg : segments) {
      anyMcid = true;
      while (pageParents.getArrayNItems() < seg.mcid) {
        pageParents.appendItem(QPDFObjectHandle::newNull());
      }
      if (seg.figure) {
        QPDFObjectHandle fig = makeElem(ctx, "/Figure", docElem, page, gNsRef);
        fig.replaceKey("/Alt", QPDFObjectHandle::newUnicodeString("Image"));
        fig.replaceKey("/K", QPDFObjectHandle::newInteger(seg.mcid));
        appendKid(docElem, fig);
        pageParents.appendItem(fig);
        if (page.isIndirect() && !pageFirstElem.count(page.getObjGen())) {
          pageFirstElem[page.getObjGen()] = fig;
        }
        ++figures;
      } else {
        if (!pElem.isInitialized()) {
          pElem = makeElem(ctx, "/P", docElem, page, gNsRef);
          appendKid(docElem, pElem);
          if (page.isIndirect() && !pageFirstElem.count(page.getObjGen())) {
            pageFirstElem[page.getObjGen()] = pElem;
          }
        }
        appendKid(pElem, QPDFObjectHandle::newInteger(seg.mcid));
        pageParents.appendItem(pElem);
      }
    }
    if (anyMcid) {
      page.replaceKey("/StructParents", QPDFObjectHandle::newInteger(nextKey));
      nums.appendItem(QPDFObjectHandle::newInteger(nextKey));
      nums.appendItem(ctx.pdf.makeIndirectObject(pageParents));
      ++nextKey;
    }

    QPDFObjectHandle annots = page.getKey("/Annots");
    if (annots.isArray() && annots.getArrayNItems() > 0) {
      page.replaceKey("/Tabs", QPDFObjectHandle::newName("/S"));
      for (int i = 0; i < annots.getArrayNItems(); ++i) {
        QPDFObjectHandle a = annots.getArrayItem(i);
        if (!a.isDictionary()) continue;
        if (!a.isIndirect()) a = ctx.pdf.makeIndirectObject(a);
        if (nameIs(a.getKey("/Subtype"), "/PrinterMark")) {
          fillAnnotText(a);
          wrapAppearanceAsArtifact(a);
          continue;
        }
        if (ctx.ua2() && nameIs(a.getKey("/Subtype"), "/Popup")) {
          a.removeKey("/StructParent");
          continue;
        }
        QPDFObjectHandle elem = annotElem(ctx, a, page, docElem, gNsRef);
        a.replaceKey("/StructParent", QPDFObjectHandle::newInteger(nextKey));
        nums.appendItem(QPDFObjectHandle::newInteger(nextKey));
        nums.appendItem(elem);
        ++nextKey;
        ++annotElems;
      }
    }
  }

  if (ctx.ua2()) {
    QPDFObjectHandle sdElem = docElem.isIndirect() ? docElem
                                                   : ctx.pdf.makeIndirectObject(docElem);
    int fixedDest = 0;
    int keptDest = 0;
    auto elemForDest = [&](QPDFObjectHandle dest) {
      if (dest.isString()) {
        QPDFObjectHandle nm = root.getKey("/Names");
        QPDFObjectHandle tree = nm.isDictionary() ? nm.getKey("/Dests") : QPDFObjectHandle::newNull();
        QPDFObjectHandle found;
        if (tree.isDictionary()) {
          try {
            QPDFNameTreeObjectHelper(tree, ctx.pdf).findObject(dest.getUTF8Value(), found);
          } catch (...) {
          }
        }
        dest = found.isInitialized() ? found : QPDFObjectHandle::newNull();
      } else if (dest.isName()) {
        QPDFObjectHandle dd = root.getKey("/Dests");
        dest = dd.isDictionary() ? dd.getKey(dest.getName()) : QPDFObjectHandle::newNull();
      }
      if (dest.isDictionary()) dest = dest.getKey("/D");
      if (dest.isArray() && dest.getArrayNItems() >= 1) {
        QPDFObjectHandle target = dest.getArrayItem(0);
        if (target.isIndirect()) {
          if (target.isDictionary() && target.hasKey("/S") && target.hasKey("/P")) return target;
          auto it = pageFirstElem.find(target.getObjGen());
          if (it != pageFirstElem.end()) return it->second;
          if (nameIs(target.getKey("/Type"), "/Page")) {
            QPDFObjectHandle part = makeElem(ctx, "/Part", docElem, target, gNsRef);
            appendKid(docElem, part);
            pageFirstElem[target.getObjGen()] = part;
            return part;
          }
        }
      }
      return QPDFObjectHandle();
    };
    auto structDestArray = [&](QPDFObjectHandle elem) {
      QPDFObjectHandle sd = QPDFObjectHandle::newArray();
      sd.appendItem(elem);
      sd.appendItem(QPDFObjectHandle::newName("/Fit"));
      return sd;
    };
    int fixedAction = 0;
    auto toStructActionDest = [&](QPDFObjectHandle action) {
      QPDFObjectHandle d = action.getKey("/D");
      bool inDoc = d.isString() || d.isName() ||
                   (d.isArray() && d.getArrayNItems() >= 1 && !d.getArrayItem(0).isString());
      if (!inDoc) return;
      QPDFObjectHandle elem = elemForDest(d);
      if (!elem.isInitialized()) {
        ++keptDest;
        return;
      }
      action.replaceKey("/SD", structDestArray(elem));
      ++fixedAction;
    };
    auto toStructDest = [&](QPDFObjectHandle owner, const std::string& key) {
      QPDFObjectHandle dest = owner.getKey(key);
      if (dest.isNull()) return;

      bool inDoc = dest.isString() || dest.isName() ||
                   (dest.isArray() && dest.getArrayNItems() >= 1 &&
                    !dest.getArrayItem(0).isString());
      if (!inDoc) return;
      QPDFObjectHandle elem = elemForDest(dest);
      if (!elem.isInitialized()) {
        ++keptDest;
        return;
      }
      owner.replaceKey(key, structDestArray(elem));
      ++fixedDest;
    };
    QPDFObjectHandle oa = root.getKey("/OpenAction");
    if (oa.isDictionary() && nameIs(oa.getKey("/S"), "/GoTo")) {
      toStructActionDest(oa);
    } else if (oa.isArray() || oa.isName() || oa.isString()) {
      QPDFObjectHandle elem = elemForDest(oa);
      if (elem.isInitialized()) {
        root.replaceKey("/OpenAction", structDestArray(elem));
        ++fixedDest;
      } else {
        ++keptDest;
      }
    }
    for (auto& ph2 : pages) {
      QPDFObjectHandle an = ph2.getObjectHandle().getKey("/Annots");
      if (!an.isArray()) continue;
      for (int i = 0; i < an.getArrayNItems(); ++i) {
        QPDFObjectHandle a = an.getArrayItem(i);
        if (!a.isDictionary()) continue;
        toStructDest(a, "/Dest");
        QPDFObjectHandle act = a.getKey("/A");
        if (act.isDictionary() && nameIs(act.getKey("/S"), "/GoTo")) {
          toStructActionDest(act);
        }
      }
    }
    QPDFObjectHandle names = root.getKey("/Names");
    QPDFObjectHandle destTree = names.isDictionary() ? names.getKey("/Dests")
                                                     : QPDFObjectHandle::newNull();
    std::vector<QPDFObjectHandle> nstack;
    if (destTree.isDictionary()) nstack.push_back(destTree);
    Visited nseen;
    while (!nstack.empty()) {
      QPDFObjectHandle node = nstack.back();
      nstack.pop_back();
      if (!node.isDictionary() || !nseen.enter(node)) continue;
      QPDFObjectHandle kids = node.getKey("/Kids");
      if (kids.isArray()) {
        for (int i = 0; i < kids.getArrayNItems(); ++i) nstack.push_back(kids.getArrayItem(i));
      }
      QPDFObjectHandle nm = node.getKey("/Names");
      if (nm.isArray()) {
        for (int i = 1; i < nm.getArrayNItems(); i += 2) {
          QPDFObjectHandle entry = nm.getArrayItem(i);
          if (entry.isDictionary() && entry.getKey("/D").isArray()) {
            toStructDest(entry, "/D");
          } else if (entry.isArray() && entry.getArrayNItems() >= 1 &&
                     !entry.getArrayItem(0).isString()) {
            QPDFObjectHandle elem = elemForDest(entry);
            if (elem.isInitialized()) {
              nm.setArrayItem(i, structDestArray(elem));
              ++fixedDest;
            } else {
              ++keptDest;
            }
          }
        }
      }
    }
    QPDFObjectHandle outlines = root.getKey("/Outlines");
    if (outlines.isDictionary()) {
      std::vector<QPDFObjectHandle> ostack;
      ostack.push_back(outlines.getKey("/First"));
      Visited oseen;
      int guard = 0;
      while (!ostack.empty() && ++guard < 100000) {
        QPDFObjectHandle item = ostack.back();
        ostack.pop_back();
        if (!item.isDictionary() || !oseen.enter(item)) continue;
        toStructDest(item, "/Dest");
        QPDFObjectHandle act = item.getKey("/A");
        if (act.isDictionary() && nameIs(act.getKey("/S"), "/GoTo")) toStructActionDest(act);
        if (item.getKey("/Next").isDictionary()) ostack.push_back(item.getKey("/Next"));
        if (item.getKey("/First").isDictionary()) ostack.push_back(item.getKey("/First"));
      }
    }
    if (fixedDest || fixedAction) {
      ctx.issue("UA_STRUCT_DEST",
                "converted " + std::to_string(fixedDest + fixedAction) +
                    " in-document destination(s) to structure destinations (PDF/UA-2 8.8)",
                true);
    }
    if (keptDest) {
      ctx.issue("UA_STRUCT_DEST_KEPT",
                "kept " + std::to_string(keptDest) +
                    " destination(s) unchanged; no structure element was tagged on the "
                    "target page",
                false);
    }
  }

  QPDFObjectHandle parentTree = QPDFObjectHandle::newDictionary();
  parentTree.replaceKey("/Nums", nums);
  treeRoot.replaceKey("/ParentTree", ctx.pdf.makeIndirectObject(parentTree));
  treeRoot.replaceKey("/ParentTreeNextKey", QPDFObjectHandle::newInteger(nextKey));

  ctx.issue("UA_STRUCTURE_BUILT",
            "tagged " + std::to_string(wrapped) + " page(s) as paragraphs with " +
                std::to_string(figures) + " figure(s) and " + std::to_string(annotElems) +
                " annotation element(s); semantic depth (headings, tables, reading order) "
                "requires assisted remediation",
            true);
}
}

void passTagging(Ctx& ctx) {
  if (ctx.opt.ua) {
    uaTagging(ctx);
    fixLang(ctx, ctx.pdf.getRoot(), "document catalog");
    return;
  }
  if (ctx.conf != 'A') return;
  QPDFObjectHandle root = ctx.pdf.getRoot();
  QPDFObjectHandle markInfo = root.getKey("/MarkInfo");
  if (!markInfo.isDictionary()) {
    markInfo = QPDFObjectHandle::newDictionary();
    root.replaceKey("/MarkInfo", markInfo);
  }
  QPDFObjectHandle marked = markInfo.getKey("/Marked");
  if (!marked.isBool() || !marked.getBoolValue()) {
    markInfo.replaceKey("/Marked", QPDFObjectHandle::newBool(true));
    ctx.issue("MARKINFO_SET", "set /MarkInfo /Marked true", true);
  }

  QPDFObjectHandle str = root.getKey("/StructTreeRoot");
  if (!str.isDictionary()) {
    QPDFObjectHandle docElem = QPDFObjectHandle::newDictionary();
    docElem.replaceKey("/Type", QPDFObjectHandle::newName("/StructElem"));
    docElem.replaceKey("/S", QPDFObjectHandle::newName("/Document"));
    QPDFObjectHandle docRef = ctx.pdf.makeIndirectObject(docElem);
    QPDFObjectHandle treeRoot = QPDFObjectHandle::newDictionary();
    treeRoot.replaceKey("/Type", QPDFObjectHandle::newName("/StructTreeRoot"));
    treeRoot.replaceKey("/K", docRef);
    QPDFObjectHandle treeRef = ctx.pdf.makeIndirectObject(treeRoot);
    docElem.replaceKey("/P", treeRef);
    root.replaceKey("/StructTreeRoot", treeRef);
    ctx.issue("STRUCT_TREE_SYNTHESIZED",
              "synthesized minimal structure tree; semantic tagging quality is limited for "
              "auto-converted documents",
              true);
  }

  fixLang(ctx, root, "document catalog");
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (obj.isDictionary() && (nameIs(obj.getKey("/Type"), "/StructElem") || (obj.getKey("/S").isName() && obj.hasKey("/P") && !obj.hasKey("/Type")))) {
      fixLang(ctx, obj, "structure element");
    }
  }
  fixRoleMap(ctx);
  if (root.getKey("/StructTreeRoot").isDictionary()) passSemanticRepair(ctx);
}
}
