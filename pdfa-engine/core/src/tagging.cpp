#include <cstdio>
#include <cstdlib>
#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <cctype>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "ctx.hh"
#include "passes.hh"
#include "util.hh"

namespace pdfa {
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
  QPDFObjectHandle rm = str.getKey("/RoleMap");
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
  if (changed) {
    ctx.issue("ROLEMAP_NORMALIZED",
              "normalized structure role map (standard types unmapped, cycles broken, "
              "non-standard types mapped)",
              true);
  }
}

void fixLang(Ctx& ctx, QPDFObjectHandle dict, const std::string& where) {
  QPDFObjectHandle lang = dict.getKey("/Lang");
  if (lang.isNull()) return;
  if (!lang.isString() || !validLangTag(lang.getUTF8Value())) {
    dict.removeKey("/Lang");
    ctx.issue("LANG_REMOVED", "removed invalid language tag from " + where, true);
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
    if (op == "BMC" || op == "BDC") {
      std::string tag;
      for (const auto& t : operands) {
        if (t.getType() == QPDFTokenizer::tt_name) {
          tag = t.getValue();
          break;
        }
      }
      if (tag == "/Artifact") {
        flush();
        writeToken(token);
        stack.push_back(true);
      } else {
        operands.clear();
        stack.push_back(false);
      }
      return;
    }
    if (op == "EMC") {
      if (!stack.empty()) {
        bool kept = stack.back();
        stack.pop_back();
        if (kept) {
          flush();
          writeToken(token);
        } else {
          operands.clear();
        }
      } else {
        operands.clear();
      }
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
  std::vector<bool> stack;
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
      openP();
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

void uaTagging(Ctx& ctx) {
  QPDFObjectHandle root = ctx.pdf.getRoot();

  if (root.hasKey("/StructTreeRoot")) {
    root.removeKey("/StructTreeRoot");
    ctx.issue("STRUCT_TREE_REBUILT",
              "replaced existing structure tree with machine-conformant re-tagged structure "
              "(semantic re-tagging of arbitrary input trees is roadmap)",
              true);
  }

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
  for (auto& ph : pages) {
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
      ctx.issue("CONTENT_UNDECODABLE",
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
        std::string subtype = nameOf(a.getKey("/Subtype"));
        if (subtype == "/PrinterMark" || subtype == "/Popup") continue;
        std::string elemType = "/Annot";
        if (subtype == "/Link") elemType = "/Link";
        if (subtype == "/Widget") elemType = "/Form";
        QPDFObjectHandle elem = makeElem(ctx, elemType, docElem, page, gNsRef);
        QPDFObjectHandle objr = QPDFObjectHandle::newDictionary();
        objr.replaceKey("/Type", QPDFObjectHandle::newName("/OBJR"));
        objr.replaceKey("/Pg", page);
        objr.replaceKey("/Obj", a);
        elem.replaceKey("/K", objr);
        appendKid(docElem, elem);
        a.replaceKey("/StructParent", QPDFObjectHandle::newInteger(nextKey));
        nums.appendItem(QPDFObjectHandle::newInteger(nextKey));
        nums.appendItem(elem);
        ++nextKey;
        ++annotElems;
        if (subtype == "/Widget") {
          std::string tu = a.getKey("/T").isString() ? a.getKey("/T").getUTF8Value()
                                                     : std::string("Form field");
          QPDFObjectHandle node = a;
          for (int hop = 0; hop < 16 && node.isDictionary(); ++hop) {
            if (!node.getKey("/TU").isString() ||
                node.getKey("/TU").getUTF8Value().empty()) {
              node.replaceKey("/TU", QPDFObjectHandle::newUnicodeString(tu));
            }
            node = node.getKey("/Parent");
          }
          if (!a.getKey("/Contents").isString() ||
              a.getKey("/Contents").getUTF8Value().empty()) {
            a.replaceKey("/Contents", QPDFObjectHandle::newUnicodeString(tu));
          }
        } else if (!a.getKey("/Contents").isString() ||
                   a.getKey("/Contents").getUTF8Value().empty()) {
          std::string alt = subtype == "/Link" ? "Link"
                            : (subtype.size() > 1 ? subtype.substr(1) : std::string("Annotation")) +
                                  " annotation";
          if (subtype == "/Link") {
            QPDFObjectHandle act = a.getKey("/A");
            if (act.isDictionary() && act.getKey("/URI").isString()) {
              alt = act.getKey("/URI").getUTF8Value();
            }
          }
          a.replaceKey("/Contents", QPDFObjectHandle::newUnicodeString(alt));
        }
      }
    }
  }

  if (ctx.ua2()) {
    QPDFObjectHandle sdElem = docElem.isIndirect() ? docElem
                                                   : ctx.pdf.makeIndirectObject(docElem);
    int fixedDest = 0;
    int keptDest = 0;
    auto elemForDest = [&](QPDFObjectHandle dest) {
      if (dest.isArray() && dest.getArrayNItems() >= 1) {
        QPDFObjectHandle target = dest.getArrayItem(0);
        if (target.isIndirect()) {
          auto it = pageFirstElem.find(target.getObjGen());
          if (it != pageFirstElem.end()) return it->second;
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
  bool hadTree = root.getKey("/StructTreeRoot").isDictionary();

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
    if (obj.isDictionary() && nameIs(obj.getKey("/Type"), "/StructElem")) {
      fixLang(ctx, obj, "structure element");
    }
  }
  fixRoleMap(ctx);
  if (hadTree) passSemanticRepair(ctx);
}
}
