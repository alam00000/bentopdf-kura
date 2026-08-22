#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "ctx.hh"
#include "passes.hh"
#include "limits.hh"
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
  if (depth > kMaxOutlineDepth) return;
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
  try {
    QPDFPageDocumentHelper dh(ctx.pdf);
    for (auto& ph : dh.getAllPages()) {
      QPDFObjectHandle annots = ph.getObjectHandle().getKey("/Annots");
      if (!annots.isArray()) continue;
      for (int i = 0; i < annots.getArrayNItems(); ++i) {
        QPDFObjectHandle an = annots.getArrayItem(i);
        if (an.isDictionary() && nameIs(an.getKey("/FT"), "/Sig") &&
            visited.enter(an) && an.hasKey("/V")) {
          an.removeKey("/V");
          an.removeKey("/Lock");
          an.removeKey("/SV");
          ++stripped;
        }
      }
    }
  } catch (...) {
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
  std::vector<std::pair<std::string, QPDFObjectHandle>> treePairs;
  {
    std::vector<QPDFObjectHandle> pstack;
    if (tree.isDictionary()) pstack.push_back(tree);
    Visited pseen;
    while (!pstack.empty()) {
      QPDFObjectHandle node = pstack.back();
      pstack.pop_back();
      if (!node.isDictionary() || !pseen.enter(node)) continue;
      QPDFObjectHandle kids = node.getKey("/Kids");
      if (kids.isArray()) {
        for (int i = 0; i < kids.getArrayNItems(); ++i) pstack.push_back(kids.getArrayItem(i));
      }
      QPDFObjectHandle vals = node.getKey("/Names");
      if (vals.isArray()) {
        for (int i = 0; i + 1 < vals.getArrayNItems(); i += 2) {
          if (vals.getArrayItem(i).isString()) {
            treePairs.push_back({vals.getArrayItem(i).getUTF8Value(), vals.getArrayItem(i + 1)});
          }
        }
      }
    }
  }
  for (QPDFObjectHandle fs : specs) normalizeFilespecPart3(ctx, fs);
  std::vector<QPDFObjectHandle> orphans;
  {
    std::set<QPDFObjGen> inTree;
    for (QPDFObjectHandle fs : specs) {
      if (fs.isIndirect()) inTree.insert(fs.getObjGen());
    }
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (obj.isDictionary() && nameIs(obj.getKey("/Type"), "/Filespec") && obj.hasKey("/EF")) {
        normalizeFilespecPart3(ctx, obj);
        if (!inTree.count(obj.getObjGen())) orphans.push_back(obj);
      }
    }
  }
  if (!orphans.empty()) {
    std::vector<std::pair<std::string, QPDFObjectHandle>> pairs;
    std::set<std::string> used;
    auto keyFor = [&](QPDFObjectHandle fs, int i) {
      std::string k;
      if (fs.getKey("/UF").isString()) k = fs.getKey("/UF").getUTF8Value();
      else if (fs.getKey("/F").isString()) k = fs.getKey("/F").getUTF8Value();
      if (k.empty()) k = "attachment-" + std::to_string(i);
      std::string base = k;
      int n = 2;
      while (used.count(k)) k = base + "-" + std::to_string(n++);
      used.insert(k);
      return k;
    };
    for (const auto& [k, v] : treePairs) {
      used.insert(k);
      pairs.push_back({k, v});
    }
    int idx = 0;
    for (QPDFObjectHandle fs : orphans) {
      pairs.push_back({keyFor(fs, ++idx),
                       fs.isIndirect() ? fs : ctx.pdf.makeIndirectObject(fs)});
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    QPDFObjectHandle flat = QPDFObjectHandle::newArray();
    for (const auto& [k, v] : pairs) {
      flat.appendItem(QPDFObjectHandle::newUnicodeString(k));
      flat.appendItem(v);
    }
    QPDFObjectHandle newTree = QPDFObjectHandle::newDictionary();
    newTree.replaceKey("/Names", flat);
    if (!names.isDictionary()) {
      names = QPDFObjectHandle::newDictionary();
      root.replaceKey("/Names", names);
    }
    names.replaceKey("/EmbeddedFiles", ctx.pdf.makeIndirectObject(newTree));
    tree = names.getKey("/EmbeddedFiles");
    ctx.issue("EMBEDDED_FILES_ENROLLED",
              "listed " + std::to_string(orphans.size()) +
                  " embedded file(s) in the catalog /EmbeddedFiles name tree",
              true);
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

void attachAssociatedFile(Ctx& ctx, QPDFObjectHandle root, const std::string& name,
                          const std::string& mime, const std::string& desc,
                          const std::string& relationship, const std::string& bytes) {
  QPDFObjectHandle ef = QPDFObjectHandle::newStream(&ctx.pdf, bytes);
  QPDFObjectHandle efd = ef.getDict();
  efd.replaceKey("/Type", QPDFObjectHandle::newName("/EmbeddedFile"));
  efd.replaceKey("/Subtype", QPDFObjectHandle::newName("/" + mime));
  QPDFObjectHandle efRef = ctx.pdf.makeIndirectObject(ef);

  QPDFObjectHandle fs = QPDFObjectHandle::newDictionary();
  fs.replaceKey("/Type", QPDFObjectHandle::newName("/Filespec"));
  fs.replaceKey("/F", QPDFObjectHandle::newString(name));
  fs.replaceKey("/UF", QPDFObjectHandle::newUnicodeString(name));
  fs.replaceKey("/Desc", QPDFObjectHandle::newUnicodeString(desc));
  fs.replaceKey("/AFRelationship", QPDFObjectHandle::newName(relationship));
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
}

void attachInvoiceXml(Ctx& ctx, QPDFObjectHandle root) {
  attachAssociatedFile(ctx, root, ctx.inv.filename, "text/xml",
                       ctx.inv.standard + " invoice data", ctx.inv.relationship,
                       ctx.opt.attachXml);
  ctx.issue("FACTURX_ATTACHED",
            "embedded " + ctx.inv.filename + " as " + ctx.inv.standard + " " + ctx.inv.profile +
                (ctx.inv.guidelineId.empty()
                     ? " (profile supplied by the caller; the XML declares no guideline)"
                     : " (detected from " + ctx.inv.guidelineId + ")") +
                ", AFRelationship " + ctx.inv.relationship,
            true);
}

void embedSourceFile(Ctx& ctx, QPDFObjectHandle root) {
  std::string name = ctx.opt.embedSourceName.empty() ? "source.pdf" : ctx.opt.embedSourceName;
  std::string mime = ctx.opt.embedSourceMime.empty() ? "application/octet-stream"
                                                     : ctx.opt.embedSourceMime;
  attachAssociatedFile(ctx, root, name, mime, "Original source document before conversion",
                       "/Source", ctx.opt.embedSource);
  ctx.issue("SOURCE_EMBEDDED",
            "embedded the original document as " + name + " (" + mime +
                ", AFRelationship /Source)",
            true);
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
}

namespace {
void fixStructNameText(Ctx& ctx) {
  QPDFObjectHandle root = ctx.pdf.getRoot();
  bool wantUtf8 = ctx.isA() && ctx.part >= 4;
  bool wantPua = (ctx.isA() && ctx.part >= 4) || ctx.opt.ua;
  if (!wantUtf8 && !wantPua) return;
  QPDFObjectHandle rm = QPDFObjectHandle::newNull();
  QPDFObjectHandle str = root.getKey("/StructTreeRoot");
  if (str.isDictionary()) rm = str.getKey("/RoleMap");
  int renamed = 0, rmFixed = 0, puaFixed = 0;
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (!obj.isDictionary()) continue;
    if (wantUtf8 && obj.getKey("/S").isName() &&
        (nameIs(obj.getKey("/Type"), "/StructElem") ||
         (!obj.getKey("/Type").isName() && (obj.hasKey("/P") || obj.hasKey("/K"))))) {
      std::string s = obj.getKey("/S").getName();
      if (!validUtf8(s.substr(1))) {
        std::string target = "/P";
        if (rm.isDictionary() && rm.getKey(s).isName()) {
          std::string mapped = rm.getKey(s).getName();
          if (validUtf8(mapped.substr(1))) target = mapped;
        }
        obj.replaceKey("/S", QPDFObjectHandle::newName(target));
        ++renamed;
      }
    }
    if (wantPua && obj.getKey("/ActualText").isString()) {
      bool changed = false;
      std::string cleaned = stripPuaUtf8(obj.getKey("/ActualText").getUTF8Value(), changed);
      if (changed) {
        if (cleaned.empty()) {
          obj.removeKey("/ActualText");
        } else {
          obj.replaceKey("/ActualText", QPDFObjectHandle::newUnicodeString(cleaned));
        }
        ++puaFixed;
      }
    }
  }
  if (wantUtf8 && rm.isDictionary()) {
    for (const std::string& k : rm.getKeys()) {
      QPDFObjectHandle v = rm.getKey(k);
      if (!validUtf8(k.substr(1)) || !v.isName() || !validUtf8(v.getName().substr(1))) {
        rm.removeKey(k);
        ++rmFixed;
      }
    }
  }
  if (renamed || rmFixed) {
    ctx.issue("STRUCT_NAMES_SANITIZED",
              "replaced " + std::to_string(renamed) + " structure type name(s) and removed " +
                  std::to_string(rmFixed) + " role map entrie(s) that are not valid UTF-8",
              true);
  }
  if (puaFixed) {
    ctx.issue("ACTUALTEXT_PUA_REMOVED",
              "removed private-use-area characters from " + std::to_string(puaFixed) +
                  " ActualText value(s)",
              true);
  }
}
}

void passStructure(Ctx& ctx) {
  QPDFObjectHandle root = ctx.pdf.getRoot();
  fixStructNameText(ctx);

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
  if (!ctx.opt.embedSource.empty()) {
    if (ctx.allowEmbeddedFiles()) {
      embedSourceFile(ctx, root);
    } else {
      ctx.fatal("EMBED_SOURCE_UNSUPPORTED_LEVEL",
                "embedding the source document needs a level that allows arbitrary "
                "attachments: PDF/A-3 (3b, 3u, 3a), PDF/A-4f or PDF/E-1");
    }
  }
  if (!ctx.opt.attachXml.empty()) {
    if (!ctx.inv.rootKnown) {
      ctx.fatal("EINVOICE_NOT_A_DOCUMENT",
                "the attached XML has root element \"" +
                    (ctx.inv.rootName.empty() ? std::string("(none)") : ctx.inv.rootName) +
                    "\"; a hybrid document needs CrossIndustryInvoice, CrossIndustryDocument, "
                    "SCRDMCCBDACIOMessageStructure (Order-X) or a UBL Invoice/CreditNote");
    } else if (!ctx.inv.detected) {
      ctx.fatal("EINVOICE_PROFILE_UNKNOWN",
                "the attached XML declares no recognised Factur-X/ZUGFeRD/XRechnung guideline "
                "in GuidelineSpecifiedDocumentContextParameter/ID; supply --facturx-profile "
                "to state the profile explicitly");
    } else if (!ctx.inv.profileValid) {
      ctx.fatal("EINVOICE_PROFILE_INVALID",
                "conformance level \"" + ctx.inv.profile +
                    "\" is not in the Factur-X HybridConformanceType code list (MINIMUM, "
                    "BASIC WL, BASIC, EN 16931, EXTENDED, XRECHNUNG)");
    } else if (ctx.isA() && (ctx.part == 3 || ctx.conf == 'F')) {
      attachInvoiceXml(ctx, root);
      if (ctx.part == 4) {
        ctx.issue("EINVOICE_PDFA4_CONTAINER",
                  "PDF/A-4f container is permitted by Factur-X 1.07 BR-HYBRID-02, but "
                  "validators and recipients still commonly require PDF/A-3; use 3b unless "
                  "the recipient has confirmed PDF/A-4f support",
                  false);
      }
    } else {
      ctx.fatal("FACTURX_REQUIRES_PDFA3",
                "Factur-X/ZUGFeRD invoices must be PDF/A-3 (3b, 3u, 3a) or, per Factur-X "
                "1.07 BR-HYBRID-02, PDF/A-4f");
    }
  }
}
}
