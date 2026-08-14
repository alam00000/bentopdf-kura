#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ctx.hh"
#include "passes.hh"
#include "util.hh"

namespace pdfa {
namespace {
bool isStructElem(QPDFObjectHandle o) {
  return o.isDictionary() &&
         (nameIs(o.getKey("/Type"), "/StructElem") ||
          (o.getKey("/S").isName() && !o.getKey("/Type").isName() &&
           (o.hasKey("/P") || o.hasKey("/K"))));
}

bool isHeadingType(const std::string& s) {
  return s == "/H1" || s == "/H2" || s == "/H3" || s == "/H4" || s == "/H5" ||
         s == "/H6" || s == "/H";
}

void collectElems(QPDFObjectHandle node, Visited& seen, std::vector<QPDFObjectHandle>& out,
                  int depth = 0) {
  if (depth > 200) return;
  if (node.isArray()) {
    for (int i = 0; i < node.getArrayNItems(); ++i) {
      collectElems(node.getArrayItem(i), seen, out, depth + 1);
    }
    return;
  }
  if (!isStructElem(node)) return;
  if (node.isIndirect() && !seen.enter(node)) return;
  out.push_back(node);
  collectElems(node.getKey("/K"), seen, out, depth + 1);
}

void replaceInParentTree(QPDFObjectHandle node, const QPDFObjGen& from,
                         QPDFObjectHandle to, Visited& seen, int depth = 0) {
  if (depth > 64 || !node.isDictionary() || !seen.enter(node)) return;
  QPDFObjectHandle kids = node.getKey("/Kids");
  if (kids.isArray()) {
    for (int i = 0; i < kids.getArrayNItems(); ++i) {
      replaceInParentTree(kids.getArrayItem(i), from, to, seen, depth + 1);
    }
  }
  QPDFObjectHandle nums = node.getKey("/Nums");
  if (!nums.isArray()) return;
  for (int i = 1; i < nums.getArrayNItems(); i += 2) {
    QPDFObjectHandle v = nums.getArrayItem(i);
    if (v.isIndirect() && v.getObjGen() == from) {
      nums.setArrayItem(i, to);
    } else if (v.isArray()) {
      for (int j = 0; j < v.getArrayNItems(); ++j) {
        QPDFObjectHandle e = v.getArrayItem(j);
        if (e.isIndirect() && e.getObjGen() == from) v.setArrayItem(j, to);
      }
    }
  }
}

int mergeAdjacentHeadings(Ctx& ctx, const std::vector<QPDFObjectHandle>& elems) {
  int merged = 0;
  QPDFObjectHandle ptree = QPDFObjectHandle::newNull();
  QPDFObjectHandle str = ctx.pdf.getRoot().getKey("/StructTreeRoot");
  if (str.isDictionary()) ptree = str.getKey("/ParentTree");
  for (QPDFObjectHandle parent : elems) {
    QPDFObjectHandle k = parent.getKey("/K");
    if (!k.isArray()) continue;
    int i = 0;
    while (i + 1 < k.getArrayNItems()) {
      QPDFObjectHandle a = k.getArrayItem(i);
      QPDFObjectHandle b = k.getArrayItem(i + 1);
      if (!a.isIndirect() || !b.isIndirect() || !isStructElem(a) || !isStructElem(b)) {
        ++i;
        continue;
      }
      std::string sa = nameOf(a.getKey("/S"));
      std::string sb = nameOf(b.getKey("/S"));
      if (!isHeadingType(sa) || sa != sb) {
        ++i;
        continue;
      }
      QPDFObjectHandle pgA = a.getKey("/Pg");
      QPDFObjectHandle pgB = b.getKey("/Pg");
      bool samePage = pgA.isIndirect() && pgB.isIndirect() &&
                      pgA.getObjGen() == pgB.getObjGen();
      QPDFObjectHandle bk = b.getKey("/K");
      std::vector<QPDFObjectHandle> moved;
      if (bk.isArray()) {
        for (int j = 0; j < bk.getArrayNItems(); ++j) moved.push_back(bk.getArrayItem(j));
      } else if (!bk.isNull()) {
        moved.push_back(bk);
      }
      QPDFObjectHandle ak = a.getKey("/K");
      if (!ak.isArray()) {
        QPDFObjectHandle arr = QPDFObjectHandle::newArray();
        if (!ak.isNull()) arr.appendItem(ak);
        a.replaceKey("/K", arr);
        ak = a.getKey("/K");
      }
      for (QPDFObjectHandle m : moved) {
        if (m.isInteger() && !samePage && pgB.isIndirect()) {
          QPDFObjectHandle mcr = QPDFObjectHandle::newDictionary();
          mcr.replaceKey("/Type", QPDFObjectHandle::newName("/MCR"));
          mcr.replaceKey("/Pg", pgB);
          mcr.replaceKey("/MCID", m);
          ak.appendItem(mcr);
        } else {
          if (m.isDictionary() && isStructElem(m)) m.replaceKey("/P", a);
          ak.appendItem(m);
        }
      }
      if (ptree.isDictionary()) {
        Visited seen;
        replaceInParentTree(ptree, b.getObjGen(), a, seen);
      }
      k.eraseItem(i + 1);
      ++merged;
    }
  }
  return merged;
}

int ensureNoteIds(Ctx& ctx, const std::vector<QPDFObjectHandle>& elems) {
  std::set<std::string> existing;
  for (QPDFObjectHandle e : elems) {
    if (e.getKey("/ID").isString()) existing.insert(e.getKey("/ID").getStringValue());
  }
  int added = 0;
  int serial = 0;
  for (QPDFObjectHandle e : elems) {
    if (nameOf(e.getKey("/S")) != "/Note") continue;
    if (e.getKey("/ID").isString() && !e.getKey("/ID").getStringValue().empty()) continue;
    std::string id;
    do {
      ++serial;
      char buf[32];
      std::snprintf(buf, sizeof(buf), "note-%06d", serial);
      id = buf;
    } while (existing.count(id));
    existing.insert(id);
    e.replaceKey("/ID", QPDFObjectHandle::newString(id));
    ++added;
  }
  return added;
}

bool rebuildIdTree(Ctx& ctx, const std::vector<QPDFObjectHandle>& elems) {
  std::map<std::string, QPDFObjectHandle> byId;
  for (QPDFObjectHandle e : elems) {
    if (e.isIndirect() && e.getKey("/ID").isString()) {
      std::string id = e.getKey("/ID").getStringValue();
      if (!id.empty() && !byId.count(id)) byId[id] = e;
    }
  }
  QPDFObjectHandle str = ctx.pdf.getRoot().getKey("/StructTreeRoot");
  if (!str.isDictionary()) return false;
  if (byId.empty()) {
    if (str.hasKey("/IDTree")) {
      str.removeKey("/IDTree");
      return true;
    }
    return false;
  }
  QPDFObjectHandle names = QPDFObjectHandle::newArray();
  for (const auto& kv : byId) {
    names.appendItem(QPDFObjectHandle::newString(kv.first));
    names.appendItem(kv.second);
  }
  QPDFObjectHandle tree = QPDFObjectHandle::newDictionary();
  tree.replaceKey("/Names", names);
  QPDFObjectHandle old = str.getKey("/IDTree");
  if (old.isDictionary() && old.getKey("/Names").isArray()) {
    QPDFObjectHandle on = old.getKey("/Names");
    bool same = on.getArrayNItems() == names.getArrayNItems();
    if (same) {
      for (int i = 0; same && i < on.getArrayNItems(); i += 2) {
        if (!on.getArrayItem(i).isString() ||
            on.getArrayItem(i).getStringValue() !=
                names.getArrayItem(i).getStringValue()) {
          same = false;
        }
        if (same && i + 1 < on.getArrayNItems()) {
          QPDFObjectHandle ov = on.getArrayItem(i + 1);
          QPDFObjectHandle nv = names.getArrayItem(i + 1);
          if (!ov.isIndirect() || !nv.isIndirect() || !(ov.getObjGen() == nv.getObjGen())) {
            same = false;
          }
        }
      }
      if (same) return false;
    }
  }
  str.replaceKey("/IDTree", ctx.pdf.makeIndirectObject(tree));
  return true;
}

bool hasLblDescendant(QPDFObjectHandle elem, int depth = 0) {
  if (depth > 8) return false;
  QPDFObjectHandle k = elem.getKey("/K");
  std::vector<QPDFObjectHandle> kids;
  if (k.isArray()) {
    for (int i = 0; i < k.getArrayNItems(); ++i) kids.push_back(k.getArrayItem(i));
  } else if (!k.isNull()) {
    kids.push_back(k);
  }
  for (QPDFObjectHandle kid : kids) {
    if (!kid.isDictionary() || !isStructElem(kid)) continue;
    std::string s = nameOf(kid.getKey("/S"));
    if (s == "/Lbl") return true;
    if (s == "/LI" || s == "/LBody") {
      if (hasLblDescendant(kid, depth + 1)) return true;
    }
  }
  return false;
}

bool listNumberingPresent(QPDFObjectHandle attr) {
  if (attr.isDictionary()) {
    return nameIs(attr.getKey("/O"), "/List") && attr.hasKey("/ListNumbering");
  }
  if (attr.isArray()) {
    for (int i = 0; i < attr.getArrayNItems(); ++i) {
      if (listNumberingPresent(attr.getArrayItem(i))) return true;
    }
  }
  return false;
}

int setListNumbering(Ctx& ctx, const std::vector<QPDFObjectHandle>& elems) {
  int fixed = 0;
  for (QPDFObjectHandle e : elems) {
    if (nameOf(e.getKey("/S")) != "/L") continue;
    if (!hasLblDescendant(e)) continue;
    QPDFObjectHandle attr = e.getKey("/A");
    if (listNumberingPresent(attr)) continue;
    QPDFObjectHandle la = QPDFObjectHandle::newDictionary();
    la.replaceKey("/O", QPDFObjectHandle::newName("/List"));
    la.replaceKey("/ListNumbering", QPDFObjectHandle::newName("/Disc"));
    if (attr.isNull()) {
      e.replaceKey("/A", la);
    } else if (attr.isArray()) {
      attr.appendItem(la);
    } else {
      QPDFObjectHandle arr = QPDFObjectHandle::newArray();
      arr.appendItem(attr);
      arr.appendItem(la);
      e.replaceKey("/A", arr);
    }
    ++fixed;
  }
  return fixed;
}

class ArtifactWrapFilter : public QPDFObjectHandle::TokenFilter {
 public:
  explicit ArtifactWrapFilter(const std::set<std::string>& taggedForms)
      : taggedForms(taggedForms) {}

  int wrapped = 0;

  void handleToken(QPDFTokenizer::Token const& token) override {
    QPDFTokenizer::token_type_e type = token.getType();
    if (type == QPDFTokenizer::tt_inline_image) {
      if (mcDepth == 0) {
        write("/Artifact BMC ");
        flushOperands();
        writeToken(token);
        write(" EMC ");
        ++wrapped;
      } else {
        flushOperands();
        writeToken(token);
      }
      return;
    }
    if (type != QPDFTokenizer::tt_word) {
      operands.push_back(token);
      return;
    }
    std::string op = token.getValue();
    if (op == "BDC" || op == "BMC") {
      flushPath(false);
      ++mcDepth;
      flushOperands();
      writeToken(token);
      return;
    }
    if (op == "EMC") {
      flushPath(false);
      if (mcDepth > 0) --mcDepth;
      flushOperands();
      writeToken(token);
      return;
    }
    if (op == "m" || op == "l" || op == "c" || op == "v" || op == "y" || op == "h" ||
        op == "re") {
      for (const auto& t : operands) pathBuf.push_back(t);
      operands.clear();
      pathBuf.push_back(token);
      inPath = true;
      return;
    }
    if (op == "W" || op == "W*") {
      for (const auto& t : operands) pathBuf.push_back(t);
      operands.clear();
      pathBuf.push_back(token);
      pathClips = true;
      return;
    }
    if (op == "S" || op == "s" || op == "f" || op == "F" || op == "f*" || op == "B" ||
        op == "B*" || op == "b" || op == "b*" || op == "n") {
      for (const auto& t : operands) pathBuf.push_back(t);
      operands.clear();
      pathBuf.push_back(token);
      bool paints = op != "n";
      flushPath(paints && !pathClips && mcDepth == 0);
      return;
    }
    if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
      if (mcDepth == 0) {
        write("/Artifact BMC ");
        flushOperands();
        writeToken(token);
        write(" EMC ");
        ++wrapped;
      } else {
        flushOperands();
        writeToken(token);
      }
      return;
    }
    if (op == "Do" || op == "sh") {
      std::string name;
      for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
        if (it->getType() == QPDFTokenizer::tt_name) {
          name = it->getValue();
          break;
        }
      }
      bool formTagged = op == "Do" && taggedForms.count(name) > 0;
      if (mcDepth == 0 && !formTagged) {
        write("/Artifact BMC ");
        flushOperands();
        writeToken(token);
        write(" EMC ");
        ++wrapped;
      } else {
        flushOperands();
        writeToken(token);
      }
      return;
    }
    flushPath(false);
    flushOperands();
    writeToken(token);
  }

  void handleEOF() override {
    flushPath(false);
    flushOperands();
  }

 private:
  void flushPath(bool wrap) {
    if (pathBuf.empty()) {
      pathClips = false;
      inPath = false;
      return;
    }
    if (wrap) {
      write("/Artifact BMC ");
      ++wrapped;
    }
    for (const auto& t : pathBuf) writeToken(t);
    if (wrap) write(" EMC ");
    pathBuf.clear();
    pathClips = false;
    inPath = false;
  }

  void flushOperands() {
    for (const auto& t : operands) writeToken(t);
    operands.clear();
  }

  const std::set<std::string>& taggedForms;
  std::vector<QPDFTokenizer::Token> operands;
  std::vector<QPDFTokenizer::Token> pathBuf;
  int mcDepth = 0;
  bool pathClips = false;
  bool inPath = false;
};

std::set<std::string> taggedFormNames(QPDFObjectHandle res) {
  std::set<std::string> out;
  if (!res.isDictionary()) return out;
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (!xod.isDictionary()) return out;
  for (const std::string& k : xod.getKeys()) {
    QPDFObjectHandle xo = xod.getKey(k);
    if (!xo.isStream() || !nameIs(xo.getDict().getKey("/Subtype"), "/Form")) continue;
    try {
      auto buf = xo.getStreamData(qpdf_dl_all);
      std::string body(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
      if (body.find("/MCID") != std::string::npos) out.insert(k);
    } catch (...) {
      out.insert(k);
    }
  }
  return out;
}

int artifactMarkUntagged(Ctx& ctx) {
  int wrapped = 0;
  QPDFPageDocumentHelper dh(ctx.pdf);
  for (auto& ph : dh.getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle contents = page.getKey("/Contents");
    bool decodable = true;
    std::vector<QPDFObjectHandle> streams;
    if (contents.isStream()) streams.push_back(contents);
    if (contents.isArray()) {
      for (int i = 0; i < contents.getArrayNItems(); ++i) {
        if (contents.getArrayItem(i).isStream()) {
          streams.push_back(contents.getArrayItem(i));
        }
      }
    }
    for (QPDFObjectHandle s : streams) {
      try {
        s.getStreamData(qpdf_dl_generalized);
      } catch (...) {
        decodable = false;
      }
    }
    if (!decodable || streams.empty()) continue;
    try {
      std::set<std::string> tagged = taggedFormNames(ph.getAttribute("/Resources", false));
      ArtifactWrapFilter filter(tagged);
      Pl_Buffer buf("artifact wrap");
      ph.filterContents(&filter, &buf);
      if (filter.wrapped == 0) continue;
      auto data = buf.getBufferSharedPointer();
      std::string rewritten(reinterpret_cast<const char*>(data->getBuffer()),
                            data->getSize());
      page.replaceKey(
          "/Contents",
          ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, rewritten)));
      wrapped += filter.wrapped;
    } catch (...) {
    }
  }
  return wrapped;
}
}

void passSemanticRepair(Ctx& ctx) {
  QPDFObjectHandle str = ctx.pdf.getRoot().getKey("/StructTreeRoot");
  if (!str.isDictionary()) return;
  std::vector<QPDFObjectHandle> elems;
  Visited seen;
  collectElems(str.getKey("/K"), seen, elems);
  if (elems.empty()) return;

  int merged = mergeAdjacentHeadings(ctx, elems);
  if (merged) {
    ctx.issue("HEADINGS_MERGED",
              "merged " + std::to_string(merged) +
                  " adjacent same-level heading element(s) into single headings",
              true);
  }

  int noteIds = ensureNoteIds(ctx, elems);
  bool idTree = rebuildIdTree(ctx, elems);
  if (noteIds || idTree) {
    std::string detail;
    if (noteIds) {
      detail = "assigned unique IDs to " + std::to_string(noteIds) + " Note element(s)";
    }
    if (idTree) {
      if (!detail.empty()) detail += " and ";
      detail += "rebuilt the structure ID tree from element IDs";
    }
    ctx.issue("STRUCT_IDS_REPAIRED", detail, true);
  }

  int lists = setListNumbering(ctx, elems);
  if (lists) {
    ctx.issue("LIST_NUMBERING_SET",
              "set ListNumbering on " + std::to_string(lists) +
                  " list(s) that contain label elements",
              true);
  }

  int artifacts = artifactMarkUntagged(ctx);
  if (artifacts) {
    ctx.issue("ARTIFACT_MARKED",
              "marked " + std::to_string(artifacts) +
                  " untagged content group(s) as artifacts (content outside the "
                  "structure tree is presentation-only for assistive technology)",
              true);
  }
}
}
