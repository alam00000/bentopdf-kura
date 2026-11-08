#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
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
  if (!action.isDictionary()) return ctx.opt.verifyOnly && ctx.isA();
  std::string s = nameOf(action.getKey("/S"));
  if (ctx.isX()) {
    if (!ctx.opt.verifyOnly) return false;
    if (!ctx.pdf20Print()) return s == "/GoTo";
    return s == "/GoTo" || s == "/GoToR" || s == "/GoToE" || s == "/GoToDp" || s == "/URI" ||
           s == "/SubmitForm" || s == "/Thread" || s == "/JavaScript" ||
           (s == "/Named" && kAllowedNamedActions.count(nameOf(action.getKey("/N"))) > 0);
  }
  if (ctx.isE()) {
    if (ctx.opt.verifyOnly && (s == "/SubmitForm" || s == "/ResetForm" || s == "/ImportData" || s == "/Named" ||
                               s == "/Rendition" || s == "/Thread"))
      return true;
    return s == "/GoTo" || s == "/GoToR" || s == "/URI" || s == "/GoTo3DView" ||
           s == "/SetOCGState" ||
           (s == "/Named" && kAllowedNamedActions.count(nameOf(action.getKey("/N"))) > 0);
  }
  if (s == "/GoTo" || s == "/GoToR" || s == "/URI" || s == "/SubmitForm" ||
      s == "/Thread") {
    return true;
  }
  if (s == "/GoToE") return ctx.allowEmbeddedFiles() || (ctx.opt.verifyOnly && ctx.isA() && ctx.part >= 2);
  if (s == "/JavaScript" || s == "/GoToDp") return ctx.isA() && ctx.part >= 4;
  if (s == "/RichMediaExecute" && ctx.isA() && ctx.part >= 4) return true;
  if (s == "/RichMediaExecute" || s == "/SetOCGState" || s == "/GoTo3DView") {
    return ctx.allow3D();
  }
  if (s == "/Named") {
    return kAllowedNamedActions.count(nameOf(action.getKey("/N"))) > 0;
  }
  return false;
}

bool nextChainAllowed(Ctx& ctx, QPDFObjectHandle action, int depth) {
  if (!ctx.isA() || depth > 32) return false;
  QPDFObjectHandle next = action.getKey("/Next");
  std::vector<QPDFObjectHandle> list;
  if (next.isDictionary()) list.push_back(next);
  for (int i = 0; next.isArray() && i < next.getArrayNItems(); ++i) list.push_back(next.getArrayItem(i));
  for (QPDFObjectHandle a : list) {
    if (!actionAllowed(ctx, a)) return false;
    if (a.hasKey("/Next") && !nextChainAllowed(ctx, a, depth + 1)) return false;
  }
  return true;
}

bool triggerDefined(const std::string& where, const std::string& key) {
  if (where == "form field" || where == "annotation /Widget") return true;
  static const std::set<std::string> allowed = {"/E", "/X", "/D", "/U", "/Fo", "/Bl"};
  return allowed.count(key) > 0;
}

bool aaForbidden(Ctx& ctx, QPDFObjectHandle aa, const std::string& where) {
  if (!ctx.isA() || !aa.isDictionary()) return true;
  if (ctx.part < 4) {
    if (where == "catalog" || where == "form field" || where == "annotation /Widget" ||
        (ctx.part >= 2 && where == "page"))
      return true;
    for (const std::string& k : aa.getKeys()) {
      QPDFObjectHandle a = aa.getKey(k);
      if (!actionAllowed(ctx, a) || (a.isDictionary() && a.hasKey("/Next") && !nextChainAllowed(ctx, a, 0))) return true;
    }
    return false;
  }
  for (const std::string& k : aa.getKeys()) {
    if (!triggerDefined(where, k)) return true;
  }
  static const std::set<std::string> catalogKeys = {"/WC", "/WS", "/DS", "/WP", "/DP"};
  static const std::set<std::string> pageKeys = {"/O", "/C"};
  static const std::set<std::string> annotKeys = {"/PO", "/PC", "/PV", "/PI"};
  const std::set<std::string>& banned =
      where.rfind("annotation", 0) == 0 ? annotKeys : where == "page" ? pageKeys : catalogKeys;
  for (const std::string& k : aa.getKeys()) {
    if (banned.count(k) || !actionAllowed(ctx, aa.getKey(k))) return true;
  }
  return false;
}

void sanitizeAdditionalActions(Ctx& ctx, QPDFObjectHandle dict, const std::string& where,
                               bool aaAllowed) {
  if (!dict.hasKey("/AA")) return;
  QPDFObjectHandle aa = dict.getKey("/AA");
  if (ctx.opt.verifyOnly && ctx.isA()) {
    bool bad = aaForbidden(ctx, aa, where);
    dict.removeKey("/AA");
    ctx.issue("ADDITIONAL_ACTIONS_REMOVED", "removed /AA from " + where, bad);
    return;
  }
  if (aaAllowed && ctx.isA() && ctx.part >= 4 && aa.isDictionary()) {
    for (const std::string& k : aa.getKeys()) {
      if (!triggerDefined(where, k)) {
        aa.removeKey(k);
        ctx.issue("ACTION_REMOVED", "removed additional action under an undefined trigger " + k + " in " + where, true);
      } else if (!actionAllowed(ctx, aa.getKey(k))) {
        aa.removeKey(k);
        ctx.issue("ACTION_REMOVED",
                  "removed forbidden additional action " + k + " in " + where, true);
      } else if (aa.getKey(k).isDictionary() && aa.getKey(k).hasKey("/Next") && !nextChainAllowed(ctx, aa.getKey(k), 0)) {
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
                       bool aaAllowed = false, bool checkA = true) {
  if (!dict.isDictionary()) return;
  sanitizeAdditionalActions(ctx, dict, where, aaAllowed);
  if (checkA && dict.hasKey("/A")) {
    QPDFObjectHandle a = dict.getKey("/A");
    if (!actionAllowed(ctx, a)) {
      std::string s = a.isDictionary() ? nameOf(a.getKey("/S")) : std::string("invalid");
      dict.removeKey("/A");
      ctx.issue("ACTION_REMOVED", "removed forbidden action " + s + " from " + where, true);
    } else if (a.isDictionary() && a.hasKey("/Next") && !nextChainAllowed(ctx, a, 0)) {
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

bool byteRangeUsable(QPDFObjectHandle sig) {
  QPDFObjectHandle br = sig.isDictionary() ? sig.getKey("/ByteRange") : QPDFObjectHandle::newNull();
  if (!br.isArray() || br.getArrayNItems() != 4) return false;
  long long v[4];
  for (int i = 0; i < 4; ++i) {
    if (!br.getArrayItem(i).isInteger()) return false;
    v[i] = br.getArrayItem(i).getIntValue();
    if (v[i] < 0) return false;
  }
  return v[0] == 0 && v[1] > 0 && v[2] > v[1] && v[3] > 0;
}

bool derItem(const unsigned char* d, size_t n, size_t& at, int& tag, size_t& body, size_t& len) {
  if (at + 2 > n) return false;
  tag = d[at];
  size_t i = at + 1;
  size_t l = d[i++];
  if (l & 0x80) {
    size_t k = l & 0x7F;
    if (k == 0 || k > 4 || i + k > n) return false;
    l = 0;
    while (k--) l = (l << 8) | d[i++];
  }
  if (l > n - i) return false;
  body = i;
  len = l;
  at = i + l;
  return true;
}

size_t derCount(const unsigned char* d, size_t n) {
  size_t at = 0, count = 0, b = 0, l = 0;
  int tag = 0;
  while (at < n && derItem(d, n, at, tag, b, l)) ++count;
  return count;
}

void pkcs7Counts(const std::string& der, size_t& certs, size_t& signers) {
  certs = signers = 0;
  const unsigned char* d = reinterpret_cast<const unsigned char*>(der.data());
  size_t n = der.size(), at = 0, b = 0, l = 0;
  int tag = 0;
  for (int want : {0x30, -1, 0xA0, 0x30}) {
    if (want == -1) {
      if (!derItem(d, n, at, tag, b, l) || tag != 0x06) return;
      continue;
    }
    if (!derItem(d, n, at, tag, b, l) || tag != want) return;
    d += b;
    n = l;
    at = 0;
  }
  while (at < n && derItem(d, n, at, tag, b, l)) {
    if (tag == 0xA0) certs = derCount(d + b, l);
    if (tag == 0x31) signers = derCount(d + b, l);
  }
}

bool byteRangeCoversFile(Ctx& ctx, QPDFObjectHandle sig) {
  const unsigned char* d = ctx.src;
  size_t n = ctx.srcSize;
  if (!d || !sig.isIndirect()) return true;
  size_t off = 0;
  try {
    auto xref = ctx.pdf.getXRefTable();
    auto it = xref.find(sig.getObjGen());
    if (it == xref.end() || it->second.getType() != 1) return true;
    off = static_cast<size_t>(it->second.getOffset());
  } catch (...) {
    return true;
  }
  std::string text(reinterpret_cast<const char*>(d), n);
  size_t end = text.find("endobj", off);
  size_t key = text.find("/Contents", off);
  while (key != std::string::npos && key < end && key + 9 < n && std::isalnum(d[key + 9])) key = text.find("/Contents", key + 9);
  if (key == std::string::npos || key >= end) return true;
  size_t vs = key + 9;
  while (vs < n && std::isspace(d[vs])) ++vs;
  if (vs >= n || d[vs] != '<') return true;
  size_t ve = text.find('>', vs);
  if (ve == std::string::npos) return true;
  ++ve;
  QPDFObjectHandle br = sig.getKey("/ByteRange");
  if (!br.isArray() || br.getArrayNItems() < 4) return false;
  long long v[4];
  for (int i = 0; i < 4; ++i) {
    if (!br.getArrayItem(i).isInteger()) return false;
    v[i] = br.getArrayItem(i).getIntValue();
  }
  if (v[0] != 0 || v[1] != static_cast<long long>(vs) || v[2] != static_cast<long long>(ve)) return false;
  size_t from = ve, best = std::string::npos;
  for (size_t p = text.find("startxref"); p != std::string::npos; p = text.find("startxref", p + 9)) {
    size_t x = static_cast<size_t>(std::strtoull(text.c_str() + p + 9, nullptr, 10));
    if (x > ve && x < best) best = x;
  }
  if (best != std::string::npos) from = best;
  size_t eof = text.find("%%EOF", from);
  while (eof != std::string::npos) {
    size_t q = eof;
    while (q > 0 && d[q - 1] == ' ') --q;
    if (q == 0 || d[q - 1] == '\n' || d[q - 1] == '\r') break;
    eof = text.find("%%EOF", eof + 5);
  }
  if (eof == std::string::npos) return v[3] == static_cast<long long>(n - ve);
  size_t res = eof + 5, floating = 0;
  if (res < n && d[res] == '\n') {
    ++res;
    ++floating;
  } else if (res < n && d[res] == '\r') {
    ++res;
    ++floating;
    if (res < n && d[res] == '\n') {
      ++res;
      ++floating;
    }
  }
  long long want = static_cast<long long>(res - ve);
  if (res >= n) return v[3] == want;
  return v[3] >= want - static_cast<long long>(floating) && v[3] <= want;
}

bool signatureBreaksArchiveRules(Ctx& ctx, QPDFObjectHandle sig) {
  if (!ctx.opt.verifyOnly || !ctx.isA() || ctx.part < 2 || ctx.part > 3 || !sig.isDictionary()) return false;
  if (!byteRangeCoversFile(ctx, sig)) return true;
  QPDFObjectHandle contents = sig.getKey("/Contents");
  if (!contents.isString()) return false;
  size_t certs = 0, signers = 0;
  pkcs7Counts(contents.getStringValue(), certs, signers);
  return certs == 0 || signers != 1;
}

void stripSignatures(Ctx& ctx, QPDFObjectHandle root) {
  int stripped = 0;
  bool unusable = false;
  bool badSig = false;
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
        if (!byteRangeUsable(f.getKey("/V"))) unusable = true;
        if (signatureBreaksArchiveRules(ctx, f.getKey("/V"))) badSig = true;
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
    int kuraPage1 = 0;
    for (auto& ph : dh.getAllPages()) {
      PageScope kuraScope1(ctx, ++kuraPage1);
      QPDFObjectHandle annots = ph.getObjectHandle().getKey("/Annots");
      if (!annots.isArray()) continue;
      for (int i = 0; i < annots.getArrayNItems(); ++i) {
        QPDFObjectHandle an = annots.getArrayItem(i);
        if (an.isDictionary() && nameIs(an.getKey("/FT"), "/Sig") &&
            visited.enter(an) && an.hasKey("/V")) {
          if (!byteRangeUsable(an.getKey("/V"))) unusable = true;
          an.removeKey("/V");
          an.removeKey("/Lock");
          an.removeKey("/SV");
          ++stripped;
        }
      }
    }
  } catch (...) {
  }
  if (ctx.isE() && acro.isDictionary()) {
    std::set<QPDFObjGen> sigs;
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (obj.isDictionary() && nameIs(obj.getKey("/FT"), "/Sig") && !obj.hasKey("/V")) {
        sigs.insert(obj.getObjGen());
        QPDFObjectHandle kids = obj.getKey("/Kids");
        for (int i = 0; kids.isArray() && i < kids.getArrayNItems(); ++i) {
          if (kids.getArrayItem(i).isIndirect()) sigs.insert(kids.getArrayItem(i).getObjGen());
        }
      }
    }
    auto prune = [&](QPDFObjectHandle arr) {
      int n = 0;
      for (int i = arr.isArray() ? arr.getArrayNItems() - 1 : -1; i >= 0; --i) {
        QPDFObjectHandle o = arr.getArrayItem(i);
        if (o.isIndirect() && sigs.count(o.getObjGen())) {
          arr.eraseItem(i);
          ++n;
        }
      }
      return n;
    };
    int removed = 0;
    if (!sigs.empty()) {
      for (auto& ph : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) removed += prune(ph.getObjectHandle().getKey("/Annots"));
      for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
        if (obj.isDictionary() && !sigs.count(obj.getObjGen())) prune(obj.getKey("/Kids"));
      }
      prune(acro.getKey("/Fields"));
    }
    if (removed) {
      ctx.issue("SIGNATURE_FIELDS_REMOVED",
                "removed " + std::to_string(removed) +
                    " unsigned signature field widget(s); PDF/E-1 accepts signature fields only when signed",
                true);
    }
    acro.replaceKey("/SigFlags", QPDFObjectHandle::newInteger(0));
  } else if (acro.isDictionary() && acro.getKey("/SigFlags").isInteger() &&
      acro.getKey("/SigFlags").getIntValue() != 0) {
    acro.replaceKey("/SigFlags", QPDFObjectHandle::newInteger(0));
  }
  QPDFObjectHandle fields = acro.isDictionary() ? acro.getKey("/Fields") : QPDFObjectHandle();
  if (!ctx.opt.verifyOnly && acro.isDictionary() && (fields.isNull() || (fields.isArray() && fields.getArrayNItems() == 0)) &&
      acro.getKey("/XFA").isNull()) {
    root.removeKey("/AcroForm");
  }
  if (root.getKey("/Perms").isDictionary()) {
    root.removeKey("/Perms");
    ++stripped;
  }
  if (stripped) {
    ctx.issue("SIGNATURE_REMOVED",
              "removed " + std::to_string(stripped) +
                  " digital signature value(s)/permission dictionaries (signatures are "
                  "invalidated by conversion; re-sign the converted file)" +
                  std::string(unusable ? "; a signature did not state which bytes it covers" : "") +
                  std::string(badSig ? "; a signature does not cover the whole file or lacks a single signer with its certificate" : ""),
              !ctx.opt.verifyOnly || unusable || badSig);
  }
}

void walkFields(Ctx& ctx, QPDFObjectHandle fields, Visited& visited) {
  DepthGuard g_(visited);
  if (g_.over) return;
  if (!fields.isArray()) return;
  for (int i = 0; i < fields.getArrayNItems(); ++i) {
    QPDFObjectHandle f = fields.getArrayItem(i);
    if (!f.isDictionary() || !visited.enter(f)) continue;
    sanitizeActionsIn(ctx, f, "form field", true, !ctx.opt.verifyOnly);
    walkFields(ctx, f.getKey("/Kids"), visited);
  }
}

void removeNamesTreeEntry(Ctx& ctx, QPDFObjectHandle root, const std::string& key,
                          const std::string& code, bool real = true) {
  QPDFObjectHandle names = root.getKey("/Names");
  if (names.isDictionary() && names.hasKey(key)) {
    names.removeKey(key);
    ctx.issue(code, "removed catalog /Names" + key, real);
  }
}

void stripEmbeddedFilespec(Ctx& ctx, QPDFObjectHandle fs) {
  if (!fs.isDictionary()) return;
  if (fs.hasKey("/EF")) {
    fs.removeKey("/EF");
    ctx.issue("EMBEDDED_FILE_REMOVED", "removed embedded file stream from filespec", true);
  }
}

bool validMimeName(QPDFObjectHandle st) {
  static const std::regex mime("[-A-Za-z0-9_+.]+/[-A-Za-z0-9_+.]+");
  return st.isName() && std::regex_match(st.getName().substr(1), mime);
}

void normalizeFilespecPart3(Ctx& ctx, QPDFObjectHandle fs) {
  if (!fs.isDictionary()) return;
  if (!ctx.isE() && !fs.getKey("/AFRelationship").isName()) {
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
    for (const char* k : {"/F", "/UF", "/DOS", "/Mac", "/Unix"}) {
      QPDFObjectHandle stream = ef.getKey(k);
      if (!stream.isStream() || validMimeName(stream.getDict().getKey("/Subtype"))) continue;
      stream.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/application/octet-stream"));
      ctx.issue("EMBEDDED_FILE_MIME_ADDED", "added default MIME subtype to embedded file", true);
    }
  }
  if (!fs.getKey("/UF").isString() && fs.getKey("/F").isString()) {
    fs.replaceKey("/UF", fs.getKey("/F"));
  }
  if (!fs.getKey("/F").isString() && fs.getKey("/UF").isString()) {
    fs.replaceKey("/F", QPDFObjectHandle::newString(fs.getKey("/UF").getUTF8Value()));
  }
}

bool embeddedStreamIsPdfa(QPDFObjectHandle st, bool allow4) {
  static thread_local int nesting = 0;
  static const std::regex partRe("pdfaid:part(?:\\s*=\\s*[\"']|>)\\s*([0-9]+)");
  static const std::regex confRe("pdfaid:conformance(?:\\s*=\\s*[\"']|>)\\s*([A-Za-z]*)");
  if (nesting > 2) return false;
  try {
    auto buf = st.getStreamData(qpdf_dl_generalized);
    QPDF inner;
    inner.setSuppressWarnings(true);
    inner.processMemoryFile("embedded", reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    QPDFObjectHandle md = inner.getRoot().getKey("/Metadata");
    if (!md.isStream()) return false;
    auto mb = md.getStreamData(qpdf_dl_generalized);
    std::string xmp(reinterpret_cast<const char*>(mb->getBuffer()), mb->getSize());
    std::smatch m, c;
    if (!std::regex_search(xmp, m, partRe)) return false;
    std::string lv = m[1].str();
    if (std::regex_search(xmp, c, confRe)) {
      for (char ch : c[1].str()) lv += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (lv != "1b" && lv != "2b" && !(allow4 && lv == "4")) return false;
    Options o;
    if (!levelFromString(lv, o.level)) return false;
    o.verifyOnly = true;
    ++nesting;
    Result r = convertOnce(buf->getBuffer(), buf->getSize(), o);
    --nesting;
    return r.ok && r.compliant;
  } catch (...) {
    return false;
  }
}

std::vector<QPDFObjectHandle> allFilespecs(QPDF& pdf, QPDFObjectHandle tree) {
  std::vector<QPDFObjectHandle> out;
  std::set<QPDFObjGen> have;
  std::vector<QPDFObjectHandle> stack;
  for (QPDFObjectHandle obj : pdf.getAllObjects()) {
    stack.push_back(obj.isStream() ? obj.getDict() : obj);
    while (!stack.empty()) {
      QPDFObjectHandle o = stack.back();
      stack.pop_back();
      if (o.isDictionary()) {
        if (nameIs(o.getKey("/Type"), "/Filespec") && (!o.isIndirect() || have.insert(o.getObjGen()).second))
          out.push_back(o);
        for (const std::string& k : o.getKeys()) {
          QPDFObjectHandle v = o.getKey(k);
          if (!v.isIndirect() && (v.isDictionary() || v.isArray())) stack.push_back(v);
        }
      } else if (o.isArray()) {
        for (int i = 0; i < o.getArrayNItems(); ++i) {
          QPDFObjectHandle v = o.getArrayItem(i);
          if (!v.isIndirect() && (v.isDictionary() || v.isArray())) stack.push_back(v);
        }
      }
    }
  }
  Visited visited;
  std::vector<QPDFObjectHandle> nodes;
  if (tree.isDictionary()) nodes.push_back(tree);
  while (!nodes.empty()) {
    QPDFObjectHandle node = nodes.back();
    nodes.pop_back();
    if (!node.isDictionary() || !visited.enter(node)) continue;
    QPDFObjectHandle kids = node.getKey("/Kids");
    for (int i = 0; kids.isArray() && i < kids.getArrayNItems(); ++i) nodes.push_back(kids.getArrayItem(i));
    QPDFObjectHandle vals = node.getKey("/Names");
    for (int i = 1; vals.isArray() && i < vals.getArrayNItems(); i += 2) {
      QPDFObjectHandle v = vals.getArrayItem(i);
      if (!v.isDictionary() || nameIs(v.getKey("/Type"), "/Filespec")) continue;
      if (!v.isIndirect() || have.insert(v.getObjGen()).second) out.push_back(v);
    }
  }
  return out;
}

struct AfKeys {
  std::set<QPDFObjGen> keys;
  std::set<QPDFObjGen> seen;

  bool first(QPDFObjectHandle o) { return !o.isIndirect() || seen.insert(o.getObjGen()).second; }
  void add(QPDFObjectHandle o) {
    QPDFObjectHandle d = o.isStream() ? o.getDict() : o;
    QPDFObjectHandle af = d.isDictionary() ? d.getKey("/AF") : QPDFObjectHandle::newNull();
    for (int i = 0; af.isArray() && i < af.getArrayNItems(); ++i) {
      if (af.getArrayItem(i).isIndirect()) keys.insert(af.getArrayItem(i).getObjGen());
    }
  }
  void xobject(QPDFObjectHandle x, int depth) {
    if (!x.isStream() || !first(x)) return;
    add(x);
    if (nameIs(x.getDict().getKey("/Subtype"), "/Form")) resources(x.getDict().getKey("/Resources"), depth + 1);
  }
  void font(QPDFObjectHandle f, int depth) {
    if (f.isDictionary() && first(f) && nameIs(f.getKey("/Subtype"), "/Type3")) resources(f.getKey("/Resources"), depth + 1);
  }
  void gstate(QPDFObjectHandle g, int depth) {
    if (!g.isDictionary() || !first(g)) return;
    QPDFObjectHandle f = g.getKey("/Font");
    if (f.isArray() && f.getArrayNItems() > 0) font(f.getArrayItem(0), depth);
  }
  void pattern(QPDFObjectHandle p, int depth) {
    QPDFObjectHandle d = p.isStream() ? p.getDict() : p;
    if (!d.isDictionary() || !first(p)) return;
    QPDFObjectHandle type = d.getKey("/PatternType");
    if (type.isInteger() && type.getIntValue() == 1) resources(d.getKey("/Resources"), depth + 1);
    if (type.isInteger() && type.getIntValue() == 2) gstate(d.getKey("/ExtGState"), depth);
  }
  void resources(QPDFObjectHandle res, int depth) {
    if (!res.isDictionary() || depth > kMaxResourceNest) return;
    auto each = [&](const char* cat, auto fn) {
      QPDFObjectHandle d = res.getKey(cat);
      for (const std::string& k : d.isDictionary() ? d.getKeys() : std::set<std::string>()) fn(d.getKey(k));
    };
    each("/XObject", [&](QPDFObjectHandle v) { xobject(v, depth); });
    each("/ExtGState", [&](QPDFObjectHandle v) { gstate(v, depth); });
    each("/Pattern", [&](QPDFObjectHandle v) { pattern(v, depth); });
    each("/Font", [&](QPDFObjectHandle v) { font(v, depth); });
  }
  void structKids(QPDFObjectHandle node, Visited& visited) {
    DepthGuard g_(visited);
    if (g_.over) return;
    QPDFObjectHandle k = node.getKey("/K");
    std::vector<QPDFObjectHandle> kids;
    if (k.isArray()) kids = k.getArrayAsVector();
    else kids.push_back(k);
    for (QPDFObjectHandle e : kids) {
      if (!e.isDictionary() || nameIs(e.getKey("/Type"), "/MCR") || nameIs(e.getKey("/Type"), "/OBJR")) continue;
      if (!visited.enter(e)) continue;
      add(e);
      structKids(e, visited);
    }
  }
};

void collectAfKeys(Ctx& ctx, QPDFObjectHandle root, AfKeys& af) {
  af.add(root);
  Visited sv;
  if (root.getKey("/StructTreeRoot").isDictionary()) af.structKids(root.getKey("/StructTreeRoot"), sv);
  for (auto& ph : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    af.add(page);
    QPDFObjectHandle annots = page.getKey("/Annots");
    for (int i = 0; annots.isArray() && i < annots.getArrayNItems(); ++i) {
      QPDFObjectHandle a = annots.getArrayItem(i);
      if (!a.isDictionary()) continue;
      af.add(a);
      QPDFObjectHandle ap = a.getKey("/AP");
      for (const char* k : {"/N", "/D", "/R"}) {
        QPDFObjectHandle e = ap.isDictionary() ? ap.getKey(k) : QPDFObjectHandle::newNull();
        if (e.isStream()) af.xobject(e, 0);
        else if (e.isDictionary())
          for (const std::string& sk : e.getKeys()) af.xobject(e.getKey(sk), 0);
      }
    }
    af.resources(ph.getAttribute("/Resources", false), 0);
  }
}

void associateFilespecs(Ctx& ctx, QPDFObjectHandle root, QPDFObjectHandle tree) {
  std::vector<QPDFObjectHandle> stack;
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) stack.push_back(obj.isStream() ? obj.getDict() : obj);
  auto lift = [&](QPDFObjectHandle v, bool inTree) {
    return !v.isIndirect() && v.isDictionary() && (inTree || nameIs(v.getKey("/Type"), "/Filespec"));
  };
  while (!stack.empty()) {
    QPDFObjectHandle o = stack.back();
    stack.pop_back();
    if (o.isDictionary()) {
      for (const std::string& k : o.getKeys()) {
        QPDFObjectHandle v = o.getKey(k);
        if (lift(v, false)) o.replaceKey(k, ctx.pdf.makeIndirectObject(v));
        else if (!v.isIndirect() && (v.isDictionary() || v.isArray())) stack.push_back(v);
      }
    } else if (o.isArray()) {
      for (int i = 0; i < o.getArrayNItems(); ++i) {
        QPDFObjectHandle v = o.getArrayItem(i);
        if (lift(v, false)) o.setArrayItem(i, ctx.pdf.makeIndirectObject(v));
        else if (!v.isIndirect() && (v.isDictionary() || v.isArray())) stack.push_back(v);
      }
    }
  }
  Visited seen;
  std::vector<QPDFObjectHandle> nodes;
  if (tree.isDictionary()) nodes.push_back(tree);
  while (!nodes.empty()) {
    QPDFObjectHandle node = nodes.back();
    nodes.pop_back();
    if (!node.isDictionary() || !seen.enter(node)) continue;
    QPDFObjectHandle kids = node.getKey("/Kids");
    for (int i = 0; kids.isArray() && i < kids.getArrayNItems(); ++i) nodes.push_back(kids.getArrayItem(i));
    QPDFObjectHandle vals = node.getKey("/Names");
    for (int i = 1; vals.isArray() && i < vals.getArrayNItems(); i += 2) {
      if (lift(vals.getArrayItem(i), true)) vals.setArrayItem(i, ctx.pdf.makeIndirectObject(vals.getArrayItem(i)));
    }
  }
  AfKeys af;
  collectAfKeys(ctx, root, af);
  QPDFObjectHandle catAf = root.getKey("/AF");
  if (!catAf.isArray()) catAf = QPDFObjectHandle::newArray();
  int added = 0;
  for (QPDFObjectHandle fs : allFilespecs(ctx.pdf, tree)) {
    normalizeFilespecPart3(ctx, fs);
    if (!fs.isIndirect() || af.keys.count(fs.getObjGen())) continue;
    catAf.appendItem(fs);
    af.keys.insert(fs.getObjGen());
    ++added;
  }
  if (added) {
    root.replaceKey("/AF", catAf);
    ctx.issue("AF_ARRAY_ADDED", "associated " + std::to_string(added) + " file specification(s) with the document via catalog /AF",
              true);
  }
}

void verifyEmbeddedFiles(Ctx& ctx, QPDFObjectHandle root) {
  QPDFObjectHandle names = root.getKey("/Names");
  bool treeKey = names.isDictionary() && names.hasKey("/EmbeddedFiles");
  std::vector<QPDFObjectHandle> specs =
      allFilespecs(ctx.pdf, treeKey ? names.getKey("/EmbeddedFiles") : QPDFObjectHandle::newNull());
  if (ctx.part == 1) {
    int ef = 0;
    for (QPDFObjectHandle fs : specs) ef += fs.hasKey("/EF") ? 1 : 0;
    if (treeKey) ctx.issue("EMBEDDED_FILES_REMOVED", "the catalog name dictionary has an /EmbeddedFiles entry", true);
    if (ef) ctx.issue("EMBEDDED_FILE_REMOVED", std::to_string(ef) + " file specification(s) embed a file", true);
    return;
  }
  AfKeys af;
  if (ctx.part == 3) collectAfKeys(ctx, root, af);
  const bool checkPdfa = ctx.part == 2 || (ctx.part >= 4 && ctx.conf != 'E' && ctx.conf != 'F');
  std::map<QPDFObjGen, bool> pdfaMemo;
  int noRel = 0, notAssoc = 0, noNames = 0, badMime = 0, notPdfa = 0;
  for (QPDFObjectHandle fs : specs) {
    bool ef = fs.hasKey("/EF");
    if ((ctx.part == 3 || (ef && ctx.part >= 4)) && !fs.getKey("/AFRelationship").isName()) ++noRel;
    if (ctx.part == 3 && !(fs.isIndirect() && af.keys.count(fs.getObjGen()))) ++notAssoc;
    if (ef && !(fs.getKey("/F").isString() && fs.getKey("/UF").isString())) ++noNames;
    QPDFObjectHandle efd = fs.getKey("/EF");
    for (const char* k : {"/F", "/UF", "/DOS", "/Mac", "/Unix"}) {
      QPDFObjectHandle st = efd.isDictionary() ? efd.getKey(k) : QPDFObjectHandle::newNull();
      if (!st.isStream()) continue;
      if (ctx.part >= 3 && !validMimeName(st.getDict().getKey("/Subtype"))) ++badMime;
      if (!checkPdfa) continue;
      auto memo = pdfaMemo.find(st.getObjGen());
      bool ok = memo != pdfaMemo.end() ? memo->second : embeddedStreamIsPdfa(st, ctx.part >= 4);
      if (st.isIndirect()) pdfaMemo[st.getObjGen()] = ok;
      if (!ok) ++notPdfa;
    }
  }
  if (noRel) ctx.issue("AF_RELATIONSHIP_ADDED", std::to_string(noRel) + " file specification(s) lack /AFRelationship", true);
  if (notAssoc)
    ctx.issue("AF_ARRAY_ADDED", std::to_string(notAssoc) + " file specification(s) are not listed in an /AF array", true);
  if (noNames) ctx.issue("EMBEDDED_FILE_KEYS_MISSING", "embedded file specification lacks the /F or /UF key", true);
  if (badMime) ctx.issue("EMBEDDED_FILE_MIME_ADDED", std::to_string(badMime) + " embedded file(s) lack a valid MIME /Subtype", true);
  if (notPdfa)
    ctx.issue("EMBEDDED_FILE_REMOVED", std::to_string(notPdfa) + " embedded file(s) are not valid PDF/A documents", true);
  if (ctx.conf == 'F' && !treeKey)
    ctx.issue("EMBEDDED_FILES_TREE_ADDED", "PDF/A-4f requires an /EmbeddedFiles name tree", true);
}

void handleEmbeddedFiles(Ctx& ctx, QPDFObjectHandle root) {
  QPDFObjectHandle names = root.getKey("/Names");
  QPDFObjectHandle tree =
      names.isDictionary() ? names.getKey("/EmbeddedFiles") : QPDFObjectHandle::newNull();
  bool hadTree = tree.isDictionary();
  if (ctx.opt.verifyOnly && ctx.isA()) {
    verifyEmbeddedFiles(ctx, root);
    return;
  }
  if (ctx.opt.verifyOnly && ctx.isX() && !ctx.pdf14Target()) {
    int bad = 0;
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      QPDFObjectHandle d = obj.isStream() ? obj.getDict() : obj;
      if (!d.isDictionary() || !d.hasKey("/AF")) continue;
      QPDFObjectHandle af = d.getKey("/AF");
      bool ok = af.isArray();
      for (int i = 0; ok && i < af.getArrayNItems(); ++i) ok = af.getArrayItem(i).isDictionary();
      if (!ok) ++bad;
    }
    if (bad) {
      ctx.issue("ASSOCIATED_FILES_INVALID",
                std::to_string(bad) + " associated-file entr" + (bad == 1 ? "y is" : "ies are") +
                    " not an array of file specifications",
                true);
    }
    return;
  }
  bool allowed = ctx.allowEmbeddedFiles();
  if (!allowed) {
    if (names.isDictionary() && names.hasKey("/EmbeddedFiles")) {
      names.removeKey("/EmbeddedFiles");
      ctx.issue("EMBEDDED_FILES_REMOVED",
                ctx.part >= 4
                    ? "removed /EmbeddedFiles name tree (PDF/A-4 requires attachments to be "
                      "PDF/A themselves; target PDF/A-4f to keep arbitrary attachments)"
                    : "removed /EmbeddedFiles name tree (forbidden before PDF/A-3)",
                true);
    }
    for (QPDFObjectHandle fs : allFilespecs(ctx.pdf, QPDFObjectHandle::newNull())) stripEmbeddedFilespec(ctx, fs);
    if (root.hasKey("/AF")) root.removeKey("/AF");
    if (ctx.isX()) {
      int dropped = 0;
      for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
        QPDFObjectHandle d = obj.isStream() ? obj.getDict() : obj;
        if (d.isDictionary() && d.hasKey("/AF")) {
          d.removeKey("/AF");
          ++dropped;
        }
      }
      if (dropped) {
        ctx.issue("ASSOCIATED_FILES_REMOVED",
                  "removed associated-file references from " + std::to_string(dropped) +
                      " object(s); print exchange files may not point at other files",
                  true);
      }
    }
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
              !ctx.opt.verifyOnly || (ctx.conf == 'F' && !hadTree));
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
  std::set<QPDFObjGen> associated;
  if (ctx.opt.verifyOnly) {
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      QPDFObjectHandle d = obj.isStream() ? obj.getDict() : obj;
      QPDFObjectHandle afs = d.isDictionary() ? d.getKey("/AF") : QPDFObjectHandle::newNull();
      for (int i = 0; afs.isArray() && i < afs.getArrayNItems(); ++i) {
        if (afs.getArrayItem(i).isIndirect()) associated.insert(afs.getArrayItem(i).getObjGen());
      }
    }
  }
  bool unassociated = !ctx.opt.verifyOnly;
  for (QPDFObjectHandle fs : specs) {
    unassociated = unassociated || (ctx.part == 3 && (!fs.isIndirect() || !associated.count(fs.getObjGen())));
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
                unassociated);
    }
  }
  if (ctx.isA() && ctx.part == 3) associateFilespecs(ctx, root, tree);
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
    long long marks = 0, objects = 0;
    if (!ctx.opt.verifyOnly) {
      std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(ctx.pdf).getAllPages();
      flattenOptionalContent(ctx, pages, false, marks, objects);
    }
    root.removeKey("/OCProperties");
    ctx.issue("OPTIONAL_CONTENT_REMOVED",
              "removed optional content (unavailable before PDF 1.5)" +
                  (marks + objects ? "; dropped " + std::to_string(marks + objects) +
                                         " piece(s) of content that were hidden by default"
                                   : std::string()),
              true);
    return;
  }
  QPDFObjectHandle ocgs = oc.getKey("/OCGs");
  {
    std::set<QPDFObjGen> listed;
    for (int i = 0; ocgs.isArray() && i < ocgs.getArrayNItems(); ++i) {
      if (ocgs.getArrayItem(i).isIndirect()) listed.insert(ocgs.getArrayItem(i).getObjGen());
    }
    int added = 0;
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (!obj.isDictionary() || !nameIs(obj.getKey("/Type"), "/OCG") || listed.count(obj.getObjGen())) continue;
      if (!ocgs.isArray()) {
        ocgs = QPDFObjectHandle::newArray();
        oc.replaceKey("/OCGs", ocgs);
      }
      ocgs.appendItem(obj);
      listed.insert(obj.getObjGen());
      ++added;
    }
    if (added) {
      ctx.issue("OCG_LISTED", "listed " + std::to_string(added) + " optional content group(s) missing from the document's group list", true);
    }
  }
  if (!oc.getKey("/D").isDictionary()) {
    QPDFObjectHandle d = QPDFObjectHandle::newDictionary();
    d.replaceKey("/Name", QPDFObjectHandle::newUnicodeString("Configuration"));
    d.replaceKey("/Order", QPDFObjectHandle::newArray());
    oc.replaceKey("/D", d);
    ctx.issue("OC_DEFAULT_CONFIG_ADDED", "added the required default optional content configuration", true);
  }
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
      ctx.issue("OC_AS_REMOVED", "removed /AS from optional content configuration",
                !ctx.opt.verifyOnly || !ctx.isA() || ctx.part <= 3);
    }
    if (!cfg.getKey("/Name").isString() || cfg.getKey("/Name").getUTF8Value().empty()) {
      bool nameMissing = !cfg.getKey("/Name").isString() || (ctx.isA() && ctx.part <= 3);
      std::string nm;
      do {
        ++nameCounter;
        nm = nameCounter == 1 ? "Configuration" : "Configuration " + std::to_string(nameCounter);
      } while (!usedNames.insert(nm).second);
      cfg.replaceKey("/Name", QPDFObjectHandle::newUnicodeString(nm));
      ctx.issue("OC_CONFIG_NAME_ADDED", "added missing /Name to optional content config",
                !ctx.opt.verifyOnly || (nameMissing && !ctx.isE()));
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
  bool wantUtf8 = (ctx.isA() && ctx.part >= 4) || ctx.pdf20Print();
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
      bool real = !ctx.opt.verifyOnly;
      if (!real && std::string(key) == "/Perms") {
        QPDFObjectHandle perms = root.getKey(key);
        for (const std::string& pk : perms.isDictionary() && (ctx.part >= 2 || ctx.isX()) ? perms.getKeys() : std::set<std::string>()) {
          real = real || (pk != "/UR3" && pk != "/DocMDP");
        }
        bool docMdp = perms.isDictionary() && perms.hasKey("/DocMDP") && (ctx.part == 2 || ctx.part == 3);
        for (QPDFObjectHandle sf : docMdp ? ctx.pdf.getAllObjects() : std::vector<QPDFObjectHandle>()) {
          if (!sf.isDictionary() || !nameIs(sf.getKey("/FT"), "/Sig") || !sf.getKey("/V").isDictionary()) continue;
          QPDFObjectHandle refs = sf.getKey("/V").getKey("/Reference");
          for (int ri = 0; refs.isArray() && ri < refs.getArrayNItems(); ++ri) {
            QPDFObjectHandle rd = refs.getArrayItem(ri);
            for (const char* dk : {"/DigestLocation", "/DigestMethod", "/DigestValue"}) {
              real = real || (rd.isDictionary() && rd.hasKey(dk));
            }
          }
        }
      } else if (!real && std::string(key) == "/AA") {
        real = aaForbidden(ctx, root.getKey(key), "catalog");
      } else if (!real && std::string(key) == "/Version") {
        QPDFObjectHandle v = root.getKey(key);
        std::string vs = v.isName() ? v.getName().substr(1) : std::string();
        real = (ctx.part >= 4 || ctx.pdf20Print()) && (vs.size() != 3 || vs[0] != '2' || vs[1] != '.' || !std::isdigit(static_cast<unsigned char>(vs[2])));
      } else if (!real && std::string(key) == "/NeedsRendering") {
        QPDFObjectHandle v = root.getKey(key);
        real = !ctx.isA() || (ctx.part >= 2 && !(v.isBool() && !v.getBoolValue()));
      } else if (!real && std::string(key) == "/Requirements") {
        real = !ctx.isA() || ctx.part >= 2;
      } else if (!real) {
        real = true;
      }
      root.removeKey(key);
      ctx.issue("CATALOG_KEY_REMOVED", std::string("removed catalog ") + key, real);
    }
  }

  stripSignatures(ctx, root);

  QPDFObjectHandle openAction = root.getKey("/OpenAction");
  if (openAction.isDictionary() && !actionAllowed(ctx, openAction)) {
    root.removeKey("/OpenAction");
    ctx.issue("ACTION_REMOVED", "removed forbidden /OpenAction", true);
  } else if (openAction.isDictionary() && openAction.hasKey("/Next") && !nextChainAllowed(ctx, openAction, 0)) {
    openAction.removeKey("/Next");
    ctx.issue("ACTION_CHAIN_REMOVED", "removed /Next action chain in /OpenAction", true);
  }

  if (!(ctx.isA() && ctx.part >= 4) && !(ctx.opt.verifyOnly && ctx.pdf20Print())) {
    removeNamesTreeEntry(ctx, root, "/JavaScript", "JAVASCRIPT_REMOVED", !ctx.opt.verifyOnly || !ctx.isA());
  }
  removeNamesTreeEntry(ctx, root, "/AlternatePresentations", "ALTERNATE_PRESENTATIONS_REMOVED",
                       !ctx.opt.verifyOnly || !ctx.isA() || ctx.part >= 2);

  QPDFObjectHandle outlines = root.getKey("/Outlines");
  if (outlines.isDictionary()) {
    Visited visited;
    walkOutlines(ctx, outlines, visited);
  }
  for (QPDFObjectHandle obj : ctx.opt.verifyOnly && ctx.isA() ? std::vector<QPDFObjectHandle>() : ctx.pdf.getAllObjects()) {
    if (obj.isDictionary() && (nameIs(obj.getKey("/Type"), "/Annot") || nameIs(obj.getKey("/Subtype"), "/Link")) &&
        obj.hasKey("/A")) {
      sanitizeActionsIn(ctx, obj, "annotation " + nameOf(obj.getKey("/Subtype")), ctx.isA() && ctx.part >= 4);
    }
  }

  if (ctx.isX()) {
    if (root.hasKey("/Dests")) root.removeKey("/Dests");
    removeNamesTreeEntry(ctx, root, "/Dests", "DESTS_REMOVED", !ctx.opt.verifyOnly);
    if (root.hasKey("/OpenAction") && !ctx.opt.verifyOnly) {
      root.removeKey("/OpenAction");
      ctx.issue("ACTION_REMOVED", "removed /OpenAction (navigation not used in print exchange)",
                true);
    }
  }
  QPDFObjectHandle vp = root.getKey("/ViewerPreferences");
  if (ctx.isX() && vp.isDictionary()) {
    for (const char* k : {"/ViewArea", "/ViewClip", "/PrintArea", "/PrintClip"}) {
      if (!vp.hasKey(k)) continue;
      std::string v = nameOf(vp.getKey(k));
      if (!ctx.pdf20Print() && (v == "/MediaBox" || v == "/BleedBox")) continue;
      vp.removeKey(k);
      ctx.issue("VIEWER_PREFERENCES_FIXED", std::string("removed viewer preference ") + k + " that would hide part of the print area", true);
    }
  }
  if (ctx.isX() && root.hasKey("/AcroForm") && !ctx.opt.verifyOnly) {
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
      bool on = !(acro.getKey("/NeedAppearances").isBool() && !acro.getKey("/NeedAppearances").getBoolValue());
      if (ctx.opt.verifyOnly && ctx.pdf20Print())
        on = on && acro.getKey("/Fields").isArray() && acro.getKey("/Fields").getArrayNItems() > 0;
      acro.removeKey("/NeedAppearances");
      ctx.issue("NEED_APPEARANCES_REMOVED", "removed /NeedAppearances from AcroForm", !ctx.opt.verifyOnly || on);
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
      if (ctx.inv.profile == "MINIMUM" || ctx.inv.profile == "BASIC WL") {
        ctx.issue("EINVOICE_PROFILE_LIMITED",
                  ctx.inv.standard + " " + ctx.inv.profile +
                      " is a valid hybrid profile, but its structured part does not carry "
                      "the full invoice, so the German mandate does not accept it as an "
                      "e-invoice; use BASIC, EN 16931 or EXTENDED for that",
                  false);
      }
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
