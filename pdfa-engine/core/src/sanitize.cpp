#include <qpdf/QPDF.hh>

#include <set>
#include <string>
#include <vector>

#include "ctx.hh"
#include "passes.hh"
#include "util.hh"

namespace pdfa {
namespace {
const std::set<std::string> kAllowedNamedActions = {
    "/NextPage", "/PrevPage", "/FirstPage", "/LastPage"};
}

bool actionAllowed(Ctx& ctx, QPDFObjectHandle action) {
  if (!action.isDictionary()) return false;
  if (ctx.isX()) return false;
  std::string s = nameOf(action.getKey("/S"));
  if (ctx.isE()) {
    return s == "/GoTo" || s == "/GoToR" || s == "/URI" || s == "/GoTo3DView" ||
           s == "/SetOCGState" ||
           (s == "/Named" && kAllowedNamedActions.count(nameOf(action.getKey("/N"))) > 0);
  }
  if (s == "/GoTo" || s == "/GoToR" || s == "/URI" || s == "/SubmitForm" ||
      s == "/Thread") {
    return true;
  }
  if (s == "/GoToE") return ctx.allowEmbeddedFiles();
  if (s == "/JavaScript" || s == "/GoToDp") return ctx.isA() && ctx.part >= 4;
  if (s == "/RichMediaExecute" || s == "/SetOCGState" || s == "/GoTo3DView") {
    return ctx.allow3D();
  }
  if (s == "/Named") {
    return kAllowedNamedActions.count(nameOf(action.getKey("/N"))) > 0;
  }
  return false;
}

void sanitizeAdditionalActions(Ctx& ctx, QPDFObjectHandle dict, const std::string& where,
                               bool aaAllowed) {
  if (!dict.hasKey("/AA")) return;
  QPDFObjectHandle aa = dict.getKey("/AA");
  if (aaAllowed && ctx.isA() && ctx.part >= 4 && aa.isDictionary()) {
    for (const std::string& k : aa.getKeys()) {
      if (!actionAllowed(ctx, aa.getKey(k))) {
        aa.removeKey(k);
        ctx.issue("ACTION_REMOVED",
                  "removed forbidden additional action " + k + " in " + where, true);
      } else if (aa.getKey(k).isDictionary() && aa.getKey(k).hasKey("/Next")) {
        aa.getKey(k).removeKey("/Next");
        ctx.issue("ACTION_CHAIN_REMOVED", "removed /Next action chain in " + where, true);
      }
    }
    if (aa.getKeys().empty()) dict.removeKey("/AA");
    return;
  }
  dict.removeKey("/AA");
  ctx.issue("ADDITIONAL_ACTIONS_REMOVED", "removed /AA from " + where, true);
}

namespace {
void sanitizeActionsIn(Ctx& ctx, QPDFObjectHandle dict, const std::string& where,
                       bool aaAllowed = false) {
  if (!dict.isDictionary()) return;
  sanitizeAdditionalActions(ctx, dict, where, aaAllowed);
  if (dict.hasKey("/A")) {
    QPDFObjectHandle a = dict.getKey("/A");
    if (!actionAllowed(ctx, a)) {
      std::string s = a.isDictionary() ? nameOf(a.getKey("/S")) : std::string("invalid");
      dict.removeKey("/A");
      ctx.issue("ACTION_REMOVED", "removed forbidden action " + s + " from " + where, true);
    } else if (a.isDictionary() && a.hasKey("/Next")) {
      a.removeKey("/Next");
      ctx.issue("ACTION_CHAIN_REMOVED", "removed /Next action chain in " + where, true);
    }
  }
}

void walkOutlines(Ctx& ctx, QPDFObjectHandle node, Visited& visited, int depth = 0) {
  if (depth > 128) return;
  for (QPDFObjectHandle item = node; item.isDictionary() && visited.enter(item);
       item = item.getKey("/Next")) {
    sanitizeActionsIn(ctx, item, "outline item");
    if (ctx.isX() && item.hasKey("/Dest")) item.removeKey("/Dest");
    walkOutlines(ctx, item.getKey("/First"), visited, depth + 1);
  }
}

void stripSignatures(Ctx& ctx, QPDFObjectHandle root) {
  int stripped = 0;
  QPDFObjectHandle acro = root.getKey("/AcroForm");
  std::vector<QPDFObjectHandle> stack;
  if (acro.isDictionary() && acro.getKey("/Fields").isArray()) {
    stack.push_back(acro.getKey("/Fields"));
  }
  Visited visited;
  while (!stack.empty()) {
    QPDFObjectHandle arr = stack.back();
    stack.pop_back();
    if (!arr.isArray()) continue;
    for (int i = 0; i < arr.getArrayNItems(); ++i) {
      QPDFObjectHandle f = arr.getArrayItem(i);
      if (!f.isDictionary() || !visited.enter(f)) continue;
      if (nameIs(f.getKey("/FT"), "/Sig") && f.hasKey("/V")) {
        f.removeKey("/V");
        f.removeKey("/Lock");
        f.removeKey("/SV");
        ++stripped;
      }
      if (f.getKey("/Kids").isArray()) stack.push_back(f.getKey("/Kids"));
    }
  }
  if (acro.isDictionary() && acro.getKey("/SigFlags").isInteger() &&
      acro.getKey("/SigFlags").getIntValue() != 0) {
    acro.replaceKey("/SigFlags", QPDFObjectHandle::newInteger(0));
  }
  if (root.getKey("/Perms").isDictionary()) {
    root.removeKey("/Perms");
    ++stripped;
  }
  if (stripped) {
    ctx.issue("SIGNATURE_REMOVED",
              "removed " + std::to_string(stripped) +
                  " digital signature value(s)/permission dictionaries (signatures are "
                  "invalidated by conversion; re-sign the converted file)",
              true);
  }
}

void walkFields(Ctx& ctx, QPDFObjectHandle fields, Visited& visited) {
  DepthGuard g_(visited);
  if (g_.over) return;
  if (!fields.isArray()) return;
  for (int i = 0; i < fields.getArrayNItems(); ++i) {
    QPDFObjectHandle f = fields.getArrayItem(i);
    if (!f.isDictionary() || !visited.enter(f)) continue;
    sanitizeActionsIn(ctx, f, "form field", true);
    walkFields(ctx, f.getKey("/Kids"), visited);
  }
}

void removeNamesTreeEntry(Ctx& ctx, QPDFObjectHandle root, const std::string& key,
                          const std::string& code) {
  QPDFObjectHandle names = root.getKey("/Names");
  if (names.isDictionary() && names.hasKey(key)) {
    names.removeKey(key);
    ctx.issue(code, "removed catalog /Names" + key, true);
  }
}

void stripEmbeddedFilespec(Ctx& ctx, QPDFObjectHandle fs) {
  if (!fs.isDictionary()) return;
  if (fs.hasKey("/EF")) {
    fs.removeKey("/EF");
    ctx.issue("EMBEDDED_FILE_REMOVED", "removed embedded file stream from filespec", true);
  }
}

void normalizeFilespecPart3(Ctx& ctx, QPDFObjectHandle fs) {
  if (!fs.isDictionary()) return;
  if (!ctx.isE() && !fs.hasKey("/AFRelationship")) {
    fs.replaceKey("/AFRelationship", QPDFObjectHandle::newName("/Unspecified"));
    ctx.issue("AF_RELATIONSHIP_ADDED", "added /AFRelationship /Unspecified to filespec", true);
  }
  QPDFObjectHandle ef = fs.getKey("/EF");
  if (ef.isDictionary()) {
    QPDFObjectHandle f = ef.getKey("/F");
    QPDFObjectHandle uf = ef.getKey("/UF");
    if (!f.isStream() && uf.isStream()) ef.replaceKey("/F", uf);
    if (!ef.getKey("/UF").isStream() && ef.getKey("/F").isStream()) {
      ef.replaceKey("/UF", ef.getKey("/F"));
    }
    QPDFObjectHandle stream = ef.getKey("/F");
    if (stream.isStream()) {
      QPDFObjectHandle sd = stream.getDict();
      QPDFObjectHandle st = sd.getKey("/Subtype");
      bool validMime = st.isName() && st.getName().find('/', 1) != std::string::npos;
      if (!validMime) {
        sd.replaceKey("/Subtype", QPDFObjectHandle::newName("/application/octet-stream"));
        ctx.issue("EMBEDDED_FILE_MIME_ADDED", "added default MIME subtype to embedded file", true);
      }
    }
  }
  if (!fs.hasKey("/UF") && fs.hasKey("/F")) {
    fs.replaceKey("/UF", fs.getKey("/F"));
  }
}

void handleEmbeddedFiles(Ctx& ctx, QPDFObjectHandle root) {
  QPDFObjectHandle names = root.getKey("/Names");
  QPDFObjectHandle tree =
      names.isDictionary() ? names.getKey("/EmbeddedFiles") : QPDFObjectHandle::newNull();
  if (!ctx.allowEmbeddedFiles()) {
    if (tree.isDictionary()) {
      names.removeKey("/EmbeddedFiles");
      ctx.issue("EMBEDDED_FILES_REMOVED",
                ctx.part >= 4
                    ? "removed /EmbeddedFiles name tree (PDF/A-4 requires attachments to be "
                      "PDF/A themselves; target PDF/A-4f to keep arbitrary attachments)"
                    : "removed /EmbeddedFiles name tree (forbidden before PDF/A-3)",
                true);
    }
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (obj.isDictionary() && nameIs(obj.getKey("/Type"), "/Filespec")) {
        stripEmbeddedFilespec(ctx, obj);
      }
    }
    if (root.hasKey("/AF")) root.removeKey("/AF");
    return;
  }
  std::vector<QPDFObjectHandle> specs;
  std::vector<QPDFObjectHandle> stack;
  if (tree.isDictionary()) stack.push_back(tree);
  Visited visited;
  while (!stack.empty()) {
    QPDFObjectHandle node = stack.back();
    stack.pop_back();
    if (!node.isDictionary() || !visited.enter(node)) continue;
    QPDFObjectHandle kids = node.getKey("/Kids");
    if (kids.isArray()) {
      for (int i = 0; i < kids.getArrayNItems(); ++i) stack.push_back(kids.getArrayItem(i));
    }
    QPDFObjectHandle vals = node.getKey("/Names");
    if (vals.isArray()) {
      for (int i = 1; i < vals.getArrayNItems(); i += 2) specs.push_back(vals.getArrayItem(i));
    }
  }
  for (QPDFObjectHandle fs : specs) normalizeFilespecPart3(ctx, fs);
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (obj.isDictionary() && nameIs(obj.getKey("/Type"), "/Filespec") && obj.hasKey("/EF")) {
      normalizeFilespecPart3(ctx, obj);
    }
  }
  if (ctx.conf == 'F' && !tree.isDictionary()) {
    if (!names.isDictionary()) {
      names = QPDFObjectHandle::newDictionary();
      root.replaceKey("/Names", names);
    }
    QPDFObjectHandle newTree = QPDFObjectHandle::newDictionary();
    newTree.replaceKey("/Names", QPDFObjectHandle::newArray());
    names.replaceKey("/EmbeddedFiles", ctx.pdf.makeIndirectObject(newTree));
    ctx.issue("EMBEDDED_FILES_TREE_ADDED",
              "added empty /EmbeddedFiles name tree (PDF/A-4f requires the key; document has "
              "no attachments)",
              true);
  }
  if (!specs.empty() && !ctx.isE()) {
    QPDFObjectHandle af = root.getKey("/AF");
    if (!af.isArray()) af = QPDFObjectHandle::newArray();
    std::set<QPDFObjGen> present;
    for (int i = 0; i < af.getArrayNItems(); ++i) {
      QPDFObjectHandle item = af.getArrayItem(i);
      if (item.isIndirect()) present.insert(item.getObjGen());
    }
    bool added = false;
    for (QPDFObjectHandle fs : specs) {
      QPDFObjectHandle ref = fs.isIndirect() ? fs : ctx.pdf.makeIndirectObject(fs);
      if (!present.count(ref.getObjGen())) {
        af.appendItem(ref);
        present.insert(ref.getObjGen());
        added = true;
      }
    }
    if (added) {
      root.replaceKey("/AF", af);
      ctx.issue("AF_ARRAY_ADDED", "associated embedded files with the document via catalog /AF",
                true);
    }
  }
}

void attachInvoiceXml(Ctx& ctx, QPDFObjectHandle root) {
  std::string name = ctx.opt.attachXmlName.empty() ? "factur-x.xml" : ctx.opt.attachXmlName;
  QPDFObjectHandle ef = QPDFObjectHandle::newStream(&ctx.pdf, ctx.opt.attachXml);
  QPDFObjectHandle efd = ef.getDict();
  efd.replaceKey("/Type", QPDFObjectHandle::newName("/EmbeddedFile"));
  efd.replaceKey("/Subtype", QPDFObjectHandle::newName("/text/xml"));
  QPDFObjectHandle efRef = ctx.pdf.makeIndirectObject(ef);

  QPDFObjectHandle fs = QPDFObjectHandle::newDictionary();
  fs.replaceKey("/Type", QPDFObjectHandle::newName("/Filespec"));
  fs.replaceKey("/F", QPDFObjectHandle::newString(name));
  fs.replaceKey("/UF", QPDFObjectHandle::newUnicodeString(name));
  fs.replaceKey("/Desc", QPDFObjectHandle::newUnicodeString("Factur-X/ZUGFeRD invoice data"));
  std::string profile = ctx.opt.facturxProfile.empty() ? "EN 16931" : ctx.opt.facturxProfile;
  bool dataRel = profile == "MINIMUM" || profile == "BASIC WL";
  fs.replaceKey("/AFRelationship",
                QPDFObjectHandle::newName(dataRel ? "/Data" : "/Alternative"));
  QPDFObjectHandle efMap = QPDFObjectHandle::newDictionary();
  efMap.replaceKey("/F", efRef);
  efMap.replaceKey("/UF", efRef);
  fs.replaceKey("/EF", efMap);
  QPDFObjectHandle fsRef = ctx.pdf.makeIndirectObject(fs);

  QPDFObjectHandle names = root.getKey("/Names");
  if (!names.isDictionary()) {
    names = QPDFObjectHandle::newDictionary();
    root.replaceKey("/Names", names);
  }
  QPDFObjectHandle tree = names.getKey("/EmbeddedFiles");
  if (!tree.isDictionary()) {
    tree = QPDFObjectHandle::newDictionary();
    tree.replaceKey("/Names", QPDFObjectHandle::newArray());
    names.replaceKey("/EmbeddedFiles", ctx.pdf.makeIndirectObject(tree));
    tree = names.getKey("/EmbeddedFiles");
  }
  QPDFObjectHandle vals = tree.getKey("/Names");
  if (!vals.isArray()) {
    vals = QPDFObjectHandle::newArray();
    tree.replaceKey("/Names", vals);
  }
  int insertAt = vals.getArrayNItems();
  for (int i = 0; i + 1 < vals.getArrayNItems(); i += 2) {
    if (vals.getArrayItem(i).isString() && vals.getArrayItem(i).getUTF8Value() > name) {
      insertAt = i;
      break;
    }
  }
  vals.insertItem(insertAt, QPDFObjectHandle::newString(name));
  vals.insertItem(insertAt + 1, fsRef);

  QPDFObjectHandle af = root.getKey("/AF");
  if (!af.isArray()) {
    af = QPDFObjectHandle::newArray();
    root.replaceKey("/AF", af);
  }
  af.appendItem(fsRef);
  ctx.issue("FACTURX_ATTACHED",
            "embedded " + name + " (" + profile + ") as an associated invoice attachment", true);
}

void handleOptionalContent(Ctx& ctx, QPDFObjectHandle root) {
  QPDFObjectHandle oc = root.getKey("/OCProperties");
  if (!oc.isDictionary()) return;
  if (ctx.pdf14Target()) {
    root.removeKey("/OCProperties");
    ctx.issue("OPTIONAL_CONTENT_REMOVED",
              "removed /OCProperties (optional content unavailable before PDF 1.5)", true);
    return;
  }
  QPDFObjectHandle ocgs = oc.getKey("/OCGs");
  if (ocgs.isArray()) {
    for (int i = 0; i < ocgs.getArrayNItems(); ++i) {
      QPDFObjectHandle g = ocgs.getArrayItem(i);
      if (g.isDictionary() && !g.getKey("/Name").isString()) {
        g.replaceKey("/Name", QPDFObjectHandle::newUnicodeString("Layer " + std::to_string(i + 1)));
        ctx.issue("OCG_NAME_ADDED", "added missing /Name to optional content group", true);
      }
    }
  }
  std::vector<QPDFObjectHandle> configs;
  if (oc.getKey("/D").isDictionary()) configs.push_back(oc.getKey("/D"));
  QPDFObjectHandle alt = oc.getKey("/Configs");
  if (alt.isArray()) {
    for (int i = 0; i < alt.getArrayNItems(); ++i) configs.push_back(alt.getArrayItem(i));
  }
  std::set<std::string> usedNames;
  int nameCounter = 0;
  for (QPDFObjectHandle cfg : configs) {
    if (!cfg.isDictionary()) continue;
    if (cfg.hasKey("/AS")) {
      cfg.removeKey("/AS");
      ctx.issue("OC_AS_REMOVED", "removed /AS from optional content configuration", true);
    }
    if (!cfg.getKey("/Name").isString() || cfg.getKey("/Name").getUTF8Value().empty()) {
      std::string nm;
      do {
        ++nameCounter;
        nm = nameCounter == 1 ? "Configuration" : "Configuration " + std::to_string(nameCounter);
      } while (!usedNames.insert(nm).second);
      cfg.replaceKey("/Name", QPDFObjectHandle::newUnicodeString(nm));
      ctx.issue("OC_CONFIG_NAME_ADDED", "added missing /Name to optional content config", true);
    } else if (!usedNames.insert(cfg.getKey("/Name").getUTF8Value()).second) {
      std::string nm = cfg.getKey("/Name").getUTF8Value();
      std::string uniq;
      int i = 2;
      do {
        uniq = nm + " " + std::to_string(i++);
      } while (!usedNames.insert(uniq).second);
      cfg.replaceKey("/Name", QPDFObjectHandle::newUnicodeString(uniq));
      ctx.issue("OC_CONFIG_NAME_DEDUPED",
                "renamed duplicate optional content config name to \"" + uniq + "\"", true);
    }
    QPDFObjectHandle order = cfg.getKey("/Order");
    if (order.isArray() && ocgs.isArray()) {
      std::set<QPDFObjGen> present;
      std::vector<QPDFObjectHandle> stack{order};
      Visited seen;
      while (!stack.empty()) {
        QPDFObjectHandle node = stack.back();
        stack.pop_back();
        if (node.isArray()) {
          if (!seen.enter(node)) continue;
          for (int i = 0; i < node.getArrayNItems(); ++i) stack.push_back(node.getArrayItem(i));
        } else if (node.isDictionary() && node.isIndirect()) {
          present.insert(node.getObjGen());
        }
      }
      int added = 0;
      for (int i = 0; i < ocgs.getArrayNItems(); ++i) {
        QPDFObjectHandle g = ocgs.getArrayItem(i);
        if (g.isIndirect() && !present.count(g.getObjGen())) {
          order.appendItem(g);
          present.insert(g.getObjGen());
          ++added;
        }
      }
      if (added) {
        ctx.issue("OC_ORDER_COMPLETED",
                  "added " + std::to_string(added) + " missing layer(s) to optional content /Order",
                  true);
      }
    }
  }
}
}

namespace {
void collectTreePairs(QPDFObjectHandle node, bool numberTree, Visited& visited, int depth,
                      std::vector<std::pair<std::string, QPDFObjectHandle>>& strPairs,
                      std::vector<std::pair<long long, QPDFObjectHandle>>& numPairs,
                      bool& malformed) {
  if (depth > 64 || !node.isDictionary() || !visited.enter(node)) {
    malformed = malformed || depth > 64 || !node.isDictionary();
    return;
  }
  QPDFObjectHandle kids = node.getKey("/Kids");
  if (kids.isArray()) {
    for (int i = 0; i < kids.getArrayNItems(); ++i) {
      collectTreePairs(kids.getArrayItem(i), numberTree, visited, depth + 1, strPairs,
                       numPairs, malformed);
    }
  }
  QPDFObjectHandle entries = node.getKey(numberTree ? "/Nums" : "/Names");
  if (entries.isArray()) {
    for (int i = 0; i + 1 < entries.getArrayNItems(); i += 2) {
      QPDFObjectHandle k = entries.getArrayItem(i);
      QPDFObjectHandle v = entries.getArrayItem(i + 1);
      if (numberTree) {
        if (!k.isInteger()) { malformed = true; continue; }
        numPairs.emplace_back(k.getIntValue(), v);
      } else {
        if (!k.isString()) { malformed = true; continue; }
        strPairs.emplace_back(k.getStringValue(), v);
      }
    }
    if (entries.getArrayNItems() % 2 != 0) malformed = true;
  }
  if (!kids.isArray() && !entries.isArray()) malformed = true;
}

bool normalizeTree(Ctx& ctx, QPDFObjectHandle holder, const std::string& key,
                   bool numberTree) {
  QPDFObjectHandle root = holder.getKey(key);
  if (!root.isDictionary()) return false;
  std::vector<std::pair<std::string, QPDFObjectHandle>> strPairs;
  std::vector<std::pair<long long, QPDFObjectHandle>> numPairs;
  bool malformed = false;
  {
    Visited visited;
    collectTreePairs(root, numberTree, visited, 0, strPairs, numPairs, malformed);
  }
  bool unsorted = false;
  if (numberTree) {
    for (size_t i = 1; i < numPairs.size(); ++i) {
      if (numPairs[i].first <= numPairs[i - 1].first) { unsorted = true; break; }
    }
  } else {
    for (size_t i = 1; i < strPairs.size(); ++i) {
      if (strPairs[i].first < strPairs[i - 1].first) { unsorted = true; break; }
    }
  }
  if (!malformed && !unsorted) return false;
  if (numberTree) {
    std::stable_sort(numPairs.begin(), numPairs.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    numPairs.erase(std::unique(numPairs.begin(), numPairs.end(),
                               [](const auto& a, const auto& b) { return a.first == b.first; }),
                   numPairs.end());
  } else {
    std::stable_sort(strPairs.begin(), strPairs.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    strPairs.erase(std::unique(strPairs.begin(), strPairs.end(),
                               [](const auto& a, const auto& b) { return a.first == b.first; }),
                   strPairs.end());
  }
  size_t total = numberTree ? numPairs.size() : strPairs.size();
  const size_t kChunk = 800;
  std::vector<QPDFObjectHandle> leaves;
  for (size_t i = 0; i < total; i += kChunk) {
    size_t end = std::min(total, i + kChunk);
    QPDFObjectHandle leaf = QPDFObjectHandle::newDictionary();
    QPDFObjectHandle arr = QPDFObjectHandle::newArray();
    for (size_t j = i; j < end; ++j) {
      if (numberTree) {
        arr.appendItem(QPDFObjectHandle::newInteger(numPairs[j].first));
        arr.appendItem(numPairs[j].second);
      } else {
        arr.appendItem(QPDFObjectHandle::newString(strPairs[j].first));
        arr.appendItem(strPairs[j].second);
      }
    }
    leaf.replaceKey(numberTree ? "/Nums" : "/Names", arr);
    if (total > kChunk) {
      QPDFObjectHandle lim = QPDFObjectHandle::newArray();
      if (numberTree) {
        lim.appendItem(QPDFObjectHandle::newInteger(numPairs[i].first));
        lim.appendItem(QPDFObjectHandle::newInteger(numPairs[end - 1].first));
      } else {
        lim.appendItem(QPDFObjectHandle::newString(strPairs[i].first));
        lim.appendItem(QPDFObjectHandle::newString(strPairs[end - 1].first));
      }
      leaf.replaceKey("/Limits", lim);
    }
    leaves.push_back(ctx.pdf.makeIndirectObject(leaf));
  }
  QPDFObjectHandle newRoot;
  if (leaves.size() == 1) {
    newRoot = leaves[0];
  } else {
    newRoot = QPDFObjectHandle::newDictionary();
    QPDFObjectHandle kids = QPDFObjectHandle::newArray();
    for (QPDFObjectHandle& l : leaves) kids.appendItem(l);
    newRoot.replaceKey("/Kids", kids);
    newRoot = ctx.pdf.makeIndirectObject(newRoot);
  }
  holder.replaceKey(key, newRoot);
  return true;
}

void normalizeNameTrees(Ctx& ctx) {
  QPDFObjectHandle root = ctx.pdf.getRoot();
  int fixed = 0;
  QPDFObjectHandle names = root.getKey("/Names");
  if (names.isDictionary()) {
    for (const std::string& k : names.getKeys()) {
      if (normalizeTree(ctx, names, k, false)) ++fixed;
    }
  }
  QPDFObjectHandle str = root.getKey("/StructTreeRoot");
  if (str.isDictionary()) {
    if (normalizeTree(ctx, str, "/ParentTree", true)) ++fixed;
    if (normalizeTree(ctx, str, "/IDTree", false)) ++fixed;
  }
  if (root.getKey("/PageLabels").isDictionary()) {
    if (normalizeTree(ctx, root, "/PageLabels", true)) ++fixed;
  }
  if (fixed) {
    ctx.issue("NAME_TREE_NORMALIZED",
              "rebuilt " + std::to_string(fixed) +
                  " name/number tree(s) with unsorted or malformed nodes",
              true);
  }
}
}

void passStructure(Ctx& ctx) {
  QPDFObjectHandle root = ctx.pdf.getRoot();

  for (const char* key : {"/AA", "/Requirements", "/NeedsRendering", "/Perms", "/Version"}) {
    if (root.hasKey(key)) {
      root.removeKey(key);
      ctx.issue("CATALOG_KEY_REMOVED", std::string("removed catalog ") + key, true);
    }
  }

  stripSignatures(ctx, root);

  QPDFObjectHandle openAction = root.getKey("/OpenAction");
  if (openAction.isDictionary() && !actionAllowed(ctx, openAction)) {
    root.removeKey("/OpenAction");
    ctx.issue("ACTION_REMOVED", "removed forbidden /OpenAction", true);
  } else if (openAction.isDictionary() && openAction.hasKey("/Next")) {
    openAction.removeKey("/Next");
    ctx.issue("ACTION_CHAIN_REMOVED", "removed /Next action chain in /OpenAction", true);
  }

  if (!(ctx.isA() && ctx.part >= 4)) {
    removeNamesTreeEntry(ctx, root, "/JavaScript", "JAVASCRIPT_REMOVED");
  }
  removeNamesTreeEntry(ctx, root, "/AlternatePresentations", "ALTERNATE_PRESENTATIONS_REMOVED");

  QPDFObjectHandle outlines = root.getKey("/Outlines");
  if (outlines.isDictionary()) {
    Visited visited;
    walkOutlines(ctx, outlines, visited);
  }

  if (ctx.isX()) {
    if (root.hasKey("/Dests")) root.removeKey("/Dests");
    removeNamesTreeEntry(ctx, root, "/Dests", "DESTS_REMOVED");
    if (root.hasKey("/OpenAction")) {
      root.removeKey("/OpenAction");
      ctx.issue("ACTION_REMOVED", "removed /OpenAction (navigation not used in print exchange)",
                true);
    }
  }
  if (ctx.isX() && root.hasKey("/AcroForm")) {
    root.removeKey("/AcroForm");
    ctx.issue("ACROFORM_REMOVED", "removed interactive form (not usable in PDF/X print files)",
              true);
  }
  QPDFObjectHandle acro = root.getKey("/AcroForm");
  if (acro.isDictionary()) {
    if (acro.hasKey("/XFA")) {
      acro.removeKey("/XFA");
      ctx.issue("XFA_REMOVED", "removed /XFA form data", true);
    }
    if (acro.hasKey("/NeedAppearances")) {
      acro.removeKey("/NeedAppearances");
      ctx.issue("NEED_APPEARANCES_REMOVED", "removed /NeedAppearances from AcroForm", true);
    }
    Visited visited;
    walkFields(ctx, acro.getKey("/Fields"), visited);
  }

  handleOptionalContent(ctx, root);
  handleEmbeddedFiles(ctx, root);
  if (!ctx.opt.attachXml.empty()) {
    if (ctx.isA() && ctx.part == 3) {
      attachInvoiceXml(ctx, root);
    } else {
      ctx.fatal("FACTURX_REQUIRES_PDFA3",
                "Factur-X/ZUGFeRD invoices must be PDF/A-3; use level 3b, 3u or 3a");
    }
  }
}
}
