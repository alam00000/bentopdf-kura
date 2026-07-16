#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "ctx.hh"
#include "passes.hh"
#include "util.hh"

namespace pdfa {
namespace {
constexpr double kMaxReal = 32767.0;
constexpr double kMaxReal2 = 3.402823e38;
constexpr double kMinDenorm = 1.175494e-38;
constexpr long long kMaxInt = 2147483647LL;
constexpr size_t kMaxString1 = 65535;
constexpr size_t kMaxString23 = 32767;
constexpr size_t kMaxName = 127;
constexpr int kMaxArray = 8191;

struct LimitStats {
  double maxReal = kMaxReal;
  int reals = 0;
  int ints = 0;
  int strings = 0;
  int names = 0;
  int arrays = 0;
  bool clampArrays = false;
  std::set<QPDFObjGen> protectedArrays;
  bool clampReals = true;
  size_t maxString = kMaxString1;
};

const std::set<std::string> kValidRi = {
    "/RelativeColorimetric", "/AbsoluteColorimetric", "/Perceptual", "/Saturation"};

const std::set<std::string> kValidOps = {
    "w", "J", "j", "M", "d", "ri", "i", "gs", "q", "Q", "cm", "m", "l", "c", "v", "y",
    "h", "re", "S", "s", "f", "F", "f*", "B", "B*", "b", "b*", "n", "W", "W*", "BT",
    "ET", "Tc", "Tw", "Tz", "TL", "Tf", "Tr", "Ts", "Td", "TD", "Tm", "T*", "Tj", "TJ",
    "'", "\"", "d0", "d1", "CS", "cs", "SC", "SCN", "sc", "scn", "G", "g", "RG", "rg",
    "K", "k", "sh", "BI", "ID", "EI", "Do", "MP", "DP", "BMC", "BDC", "EMC", "BX", "EX",
    "true", "false", "null"};

std::string shortenName(const std::string& n) {
  unsigned hash = 2166136261u;
  for (unsigned char c : n) hash = (hash ^ c) * 16777619u;
  char suffix[16];
  std::snprintf(suffix, sizeof(suffix), "X%08X", hash);
  return n.substr(0, kMaxName - 9) + suffix;
}

class ContentFixFilter : public QPDFObjectHandle::TokenFilter {
 public:
  ContentFixFilter(bool pdf14, bool limits23)
      : pdf14(pdf14), limits(pdf14 || limits23),
        maxStr(pdf14 ? kMaxString1 : kMaxString23) {}

  void handleToken(QPDFTokenizer::Token const& token) override {
    QPDFTokenizer::token_type_e type = token.getType();
    if (type == QPDFTokenizer::tt_name) {
      flushHeld();
      heldName = token;
      if (limits && token.getValue().size() > kMaxName + 1) {
        heldName = QPDFTokenizer::Token(
            QPDFTokenizer::tt_name, "/" + shortenName(token.getValue().substr(1)));
      }
      haveName = true;
      return;
    }
    if (limits && type == QPDFTokenizer::tt_word && !inImageDict) {
      const std::string& w = token.getValue();
      if (w == "q") {
        if (qDepth >= 28) {
          ++qSuppressed;
          flushHeld();
          return;
        }
        ++qDepth;
      } else if (w == "Q") {
        if (qSuppressed > 0) {
          --qSuppressed;
          flushHeld();
          return;
        }
        if (qDepth > 0) --qDepth;
      }
    }
    if (haveName && (type == QPDFTokenizer::tt_space || type == QPDFTokenizer::tt_comment)) {
      heldTrail.push_back(token);
      return;
    }
    if (type == QPDFTokenizer::tt_word && token.getValue() == "ri" && haveName && pdf14) {
      if (kValidRi.count(heldName.getValue()) == 0) {
        heldName = QPDFTokenizer::Token(QPDFTokenizer::tt_name, "/RelativeColorimetric");
      }
      flushHeld();
      writeToken(token);
      return;
    }
    if (type == QPDFTokenizer::tt_word && token.getValue() == "BI") {
      flushHeld();
      inImageDict = true;
      pendingKey.clear();
      writeToken(token);
      return;
    }
    if (inImageDict) {
      if (type == QPDFTokenizer::tt_inline_image ||
          (type == QPDFTokenizer::tt_word && token.getValue() == "EI")) {
        flushHeld();
        inImageDict = false;
        pendingKey.clear();
        writeToken(token);
        return;
      }
      if (haveName && !pendingKey.empty()) {
        std::string key = pendingKey;
        pendingKey.clear();
        if (key == "/Intent" && kValidRi.count(heldName.getValue()) == 0) {
          heldName = QPDFTokenizer::Token(QPDFTokenizer::tt_name, "/RelativeColorimetric");
        }
        flushHeld();
        return;
      }
      if (haveName) {
        pendingKey = heldName.getValue();
        flushHeld();
      }
      if (!pendingKey.empty()) {
        std::string key = pendingKey;
        pendingKey.clear();
        if ((key == "/I" || key == "/Interpolate") && token.getValue() == "true") {
          writeToken(QPDFTokenizer::Token(QPDFTokenizer::tt_word, "false"));
          return;
        }
      }
      writeToken(token);
      return;
    }
    if (type == QPDFTokenizer::tt_bad) {
      std::string raw = token.getRawValue();
      if (!raw.empty() && raw[0] == '<' && raw.find("<<") != 0) {
        flushHeld();
        std::string hex;
        for (char hc : raw) {
          if (std::isxdigit(static_cast<unsigned char>(hc))) hex += hc;
        }
        if (hex.size() % 2) hex += '0';
        write("<" + hex + ">");
        return;
      }
      flushHeld();
      return;
    }
    if (type == QPDFTokenizer::tt_array_open) {
      ++arrDepth;
    } else if (type == QPDFTokenizer::tt_array_close) {
      if (arrDepth == 0) {
        flushHeld();
        return;
      }
      --arrDepth;
    } else if (type == QPDFTokenizer::tt_dict_open) {
      ++dictDepth;
    } else if (type == QPDFTokenizer::tt_dict_close) {
      if (dictDepth == 0) {
        flushHeld();
        return;
      }
      --dictDepth;
    }
    if (type == QPDFTokenizer::tt_word && !inImageDict &&
        kValidOps.count(token.getValue()) == 0) {
      flushHeld();
      return;
    }
    flushHeld();
    if (limits && type == QPDFTokenizer::tt_real) {
      double v = std::strtod(token.getValue().c_str(), nullptr);
      double bound = pdf14 ? kMaxReal : kMaxReal2;
      if (v > bound || v < -bound) {
        double clamped = v > 0 ? kMaxReal : -kMaxReal;
        writeToken(QPDFTokenizer::Token(
            QPDFTokenizer::tt_real,
            QPDFObjectHandle::newReal(clamped, 1).getRealValue()));
        return;
      }
      if (v != 0 && v > -kMinDenorm && v < kMinDenorm) {
        writeToken(QPDFTokenizer::Token(QPDFTokenizer::tt_integer, "0"));
        return;
      }
    }
    if (limits && type == QPDFTokenizer::tt_integer) {
      long long v = std::strtoll(token.getValue().c_str(), nullptr, 10);
      if (v > kMaxInt || v < -kMaxInt) {
        writeToken(QPDFTokenizer::Token(QPDFTokenizer::tt_integer,
                                        std::to_string(v > 0 ? kMaxInt : -kMaxInt)));
        return;
      }
    }
    if (limits && type == QPDFTokenizer::tt_string) {
      std::string v = token.getValue();
      if (v.size() > maxStr) v.resize(maxStr);
      write(QPDFObjectHandle::newString(v).unparse());
      return;
    }
    writeToken(token);
  }

  void handleEOF() override {
    flushHeld();
    while (arrDepth > 0) {
      write("]");
      --arrDepth;
    }
    while (dictDepth > 0) {
      write(">>");
      --dictDepth;
    }
  }

 private:
  void flushHeld() {
    if (haveName) {
      writeToken(heldName);
      haveName = false;
    }
    for (const auto& t : heldTrail) writeToken(t);
    heldTrail.clear();
  }

  bool pdf14;
  bool limits;
  size_t maxStr;
  bool inImageDict = false;
  bool haveName = false;
  int qDepth = 0;
  int qSuppressed = 0;
  int arrDepth = 0;
  int dictDepth = 0;
  QPDFTokenizer::Token heldName{QPDFTokenizer::tt_bad, ""};
  std::vector<QPDFTokenizer::Token> heldTrail;
  std::string pendingKey;
};

QPDFObjectHandle clampScalar(QPDFObjectHandle v, LimitStats& st, bool& changed) {
  changed = false;
  if (v.isReal()) {
    double d = std::strtod(v.getRealValue().c_str(), nullptr);
    if (d > st.maxReal || d < -st.maxReal) {
      changed = true;
      ++st.reals;
      return QPDFObjectHandle::newReal(d > 0 ? kMaxReal : -kMaxReal, 1);
    }
    if (d != 0 && d > -kMinDenorm && d < kMinDenorm) {
      changed = true;
      ++st.reals;
      return QPDFObjectHandle::newInteger(0);
    }
  } else if (v.isInteger()) {
    long long i = v.getIntValue();
    if (i > kMaxInt || i < -kMaxInt) {
      changed = true;
      ++st.ints;
      return QPDFObjectHandle::newInteger(i > 0 ? kMaxInt : -kMaxInt);
    }
  } else if (v.isString()) {
    std::string s = v.getStringValue();
    if (s.size() > st.maxString) {
      changed = true;
      ++st.strings;
      s.resize(st.maxString);
      return QPDFObjectHandle::newString(s);
    }
  } else if (v.isName()) {
    std::string n = v.getName();
    if (n.size() > kMaxName + 1) {
      changed = true;
      ++st.names;
      return QPDFObjectHandle::newName("/" + shortenName(n.substr(1)));
    }
  }
  return v;
}

void clampObjectLimits(QPDFObjectHandle obj, Visited& visited, LimitStats& st) {
  DepthGuard g_(visited);
  if (g_.over) return;
  if (obj.isIndirect() && !visited.enter(obj)) return;
  if (obj.isStream()) {
    QPDFObjectHandle sd = obj.getDict();
    for (const char* k : {"/F", "/FFilter", "/FDecodeParms"}) {
      if (sd.hasKey(k)) sd.removeKey(k);
    }
    clampObjectLimits(sd, visited, st);
    return;
  }
  if (obj.isDictionary()) {
    for (const std::string& k : obj.getKeys()) {
      QPDFObjectHandle v = obj.getKey(k);
      std::string useKey = k;
      if (k.size() > kMaxName + 1) {
        obj.removeKey(k);
        useKey = "/" + shortenName(k.substr(1));
        obj.replaceKey(useKey, v);
        ++st.names;
      }
      bool changed = false;
      QPDFObjectHandle nv = clampScalar(v, st, changed);
      if (changed) {
        obj.replaceKey(useKey, nv);
      } else if (!v.isIndirect()) {
        clampObjectLimits(v, visited, st);
      }
    }
    return;
  }
  if (obj.isArray()) {
    if (st.clampArrays && obj.getArrayNItems() > kMaxArray &&
        (!obj.isIndirect() || !st.protectedArrays.count(obj.getObjGen()))) {
      ++st.arrays;
      while (obj.getArrayNItems() > kMaxArray) {
        obj.eraseItem(obj.getArrayNItems() - 1);
      }
    }
    for (int i = 0; i < obj.getArrayNItems(); ++i) {
      QPDFObjectHandle v = obj.getArrayItem(i);
      bool changed = false;
      QPDFObjectHandle nv = clampScalar(v, st, changed);
      if (changed) {
        obj.setArrayItem(i, nv);
      } else if (!v.isIndirect()) {
        clampObjectLimits(v, visited, st);
      }
    }
  }
}

bool streamDecodable(QPDFObjectHandle s) {
  try {
    s.getStreamData(qpdf_dl_generalized);
    return true;
  } catch (...) {
    return false;
  }
}

void attachFilter(Ctx& ctx, QPDFObjectHandle s, bool pdf14, bool limits23) {
  if (streamDecodable(s)) {
    s.addTokenFilter(
        std::shared_ptr<QPDFObjectHandle::TokenFilter>(new ContentFixFilter(pdf14, limits23)));
  } else {
    ctx.issue("CONTENT_UNDECODABLE", "content stream could not be decoded; left as-is", false);
  }
}

void attachToContentStreams(Ctx& ctx, QPDFObjectHandle res, Visited& visited) {
  DepthGuard g_(visited);
  if (g_.over) return;
  bool pdf14 = ctx.pdf14Target();
  bool limits23 = ctx.isA() && ctx.part >= 2 && ctx.part <= 3;
  if (!res.isDictionary() || !visited.enter(res)) return;
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (xod.isDictionary()) {
    for (const std::string& k : xod.getKeys()) {
      QPDFObjectHandle xo = xod.getKey(k);
      if (!xo.isStream() || !visited.enter(xo)) continue;
      if (nameIs(xo.getDict().getKey("/Subtype"), "/Form")) {
        attachFilter(ctx, xo, pdf14, limits23);
        attachToContentStreams(ctx, xo.getDict().getKey("/Resources"), visited);
      }
    }
  }
  QPDFObjectHandle pat = res.getKey("/Pattern");
  if (pat.isDictionary()) {
    for (const std::string& k : pat.getKeys()) {
      QPDFObjectHandle p = pat.getKey(k);
      if (p.isStream() && visited.enter(p)) {
        attachFilter(ctx, p, pdf14, limits23);
        attachToContentStreams(ctx, p.getDict().getKey("/Resources"), visited);
      }
    }
  }
  QPDFObjectHandle fonts = res.getKey("/Font");
  if (fonts.isDictionary()) {
    for (const std::string& k : fonts.getKeys()) {
      QPDFObjectHandle fnt = fonts.getKey(k);
      if (!fnt.isDictionary()) continue;
      QPDFObjectHandle cp = fnt.getKey("/CharProcs");
      if (cp.isDictionary() && visited.enter(cp)) {
        for (const std::string& g : cp.getKeys()) {
          QPDFObjectHandle glyph = cp.getKey(g);
          if (glyph.isStream() && visited.enter(glyph)) attachFilter(ctx, glyph, pdf14, limits23);
        }
        attachToContentStreams(ctx, fnt.getKey("/Resources"), visited);
      }
    }
  }
}

void rebalancePageTree(Ctx& ctx) {
  bool oversized = false;
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (obj.isDictionary() && nameIs(obj.getKey("/Type"), "/Pages") &&
        obj.getKey("/Kids").isArray() && obj.getKey("/Kids").getArrayNItems() > kMaxArray) {
      oversized = true;
      break;
    }
  }
  bool inconsistent = false;
  {
    QPDFObjectHandle root = ctx.pdf.getRoot().getKey("/Pages");
    std::set<QPDFObjGen> seen;
    std::function<long long(QPDFObjectHandle, QPDFObjectHandle, int)> walk =
        [&](QPDFObjectHandle node, QPDFObjectHandle parent, int depth) -> long long {
      if (inconsistent || depth > 64) { inconsistent = true; return 0; }
      if (!node.isDictionary()) { inconsistent = true; return 0; }
      if (node.isIndirect() && !seen.insert(node.getObjGen()).second) {
        inconsistent = true;
        return 0;
      }
      if (parent.isInitialized()) {
        QPDFObjectHandle p = node.getKey("/Parent");
        if (!p.isDictionary() || !p.isIndirect() || !parent.isIndirect() ||
            p.getObjGen() != parent.getObjGen()) {
          inconsistent = true;
          return 0;
        }
      }
      if (nameIs(node.getKey("/Type"), "/Page")) return 1;
      QPDFObjectHandle kids = node.getKey("/Kids");
      if (!kids.isArray()) { inconsistent = true; return 0; }
      long long leaves = 0;
      for (int i = 0; i < kids.getArrayNItems() && !inconsistent; ++i) {
        leaves += walk(kids.getArrayItem(i), node, depth + 1);
      }
      QPDFObjectHandle count = node.getKey("/Count");
      if (!count.isInteger() || count.getIntValue() != leaves) inconsistent = true;
      return leaves;
    };
    if (root.isDictionary()) {
      long long n = walk(root, QPDFObjectHandle(), 0);
      if (n == 0) inconsistent = true;
    } else {
      inconsistent = true;
    }
  }
  if (!oversized && !inconsistent) return;
  try {
    ctx.pdf.pushInheritedAttributesToPage();
  } catch (...) {
  }
  std::vector<QPDFObjectHandle> level;
  {
    QPDFPageDocumentHelper dh(ctx.pdf);
    for (auto& ph : dh.getAllPages()) level.push_back(ph.getObjectHandle());
  }
  if (level.empty()) return;
  const size_t kFanout = 1024;
  bool first = true;
  while (first || level.size() > 1) {
    first = false;
    std::vector<QPDFObjectHandle> next;
    for (size_t i = 0; i < level.size(); i += kFanout) {
      size_t end = std::min(level.size(), i + kFanout);
      QPDFObjectHandle node = ctx.pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
      node.replaceKey("/Type", QPDFObjectHandle::newName("/Pages"));
      QPDFObjectHandle kids = QPDFObjectHandle::newArray();
      long long count = 0;
      for (size_t j = i; j < end; ++j) {
        kids.appendItem(level[j]);
        level[j].replaceKey("/Parent", node);
        if (nameIs(level[j].getKey("/Type"), "/Pages")) {
          count += level[j].getKey("/Count").isInteger()
                       ? level[j].getKey("/Count").getIntValue()
                       : 0;
        } else {
          count += 1;
        }
      }
      node.replaceKey("/Kids", kids);
      node.replaceKey("/Count", QPDFObjectHandle::newInteger(count));
      next.push_back(node);
    }
    level = next;
  }
  level[0].removeKey("/Parent");
  ctx.pdf.getRoot().replaceKey("/Pages", level[0]);
  try {
    ctx.pdf.updateAllPagesCache();
  } catch (...) {
  }
  ctx.issue("PAGE_TREE_REBALANCED",
            "rebalanced the page tree into nested nodes (a /Kids array exceeded the PDF 1.4 "
            "limit of 8191 elements)",
            true);
}
}

void passLimits(Ctx& ctx) {
  bool pdf14 = ctx.pdf14Target();
  bool limits23 = ctx.isA() && ctx.part >= 2 && ctx.part <= 3;
  {
    int extRefs = 0;
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (!obj.isStream()) continue;
      QPDFObjectHandle sd = obj.getDict();
      bool filespec = nameIs(sd.getKey("/Type"), "/Filespec");
      for (const char* k : {"/FFilter", "/FDecodeParms"}) {
        if (sd.hasKey(k)) {
          sd.removeKey(k);
          ++extRefs;
        }
      }
      if (!filespec && sd.hasKey("/F") && !sd.getKey("/F").isStream()) {
        sd.removeKey("/F");
        ++extRefs;
      }
    }
    if (extRefs) {
      ctx.issue("STREAM_EXTERNAL_REFS_REMOVED",
                "removed " + std::to_string(extRefs) +
                    " external-file stream key(s) (/F,/FFilter,/FDecodeParms)",
                true);
    }
  }
  rebalancePageTree(ctx);
  if (pdf14 || limits23) {
    LimitStats st;
    st.clampReals = pdf14;
    st.clampArrays = pdf14;
    st.maxReal = pdf14 ? kMaxReal : kMaxReal2;
    st.maxString = pdf14 ? kMaxString1 : kMaxString23;
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (obj.isDictionary() && nameIs(obj.getKey("/Type"), "/Pages") &&
          obj.getKey("/Kids").isArray() && obj.getKey("/Kids").isIndirect()) {
        st.protectedArrays.insert(obj.getKey("/Kids").getObjGen());
      }
    }
    Visited visited;
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (obj.isReal() || obj.isInteger() || obj.isString() || obj.isName()) {
        bool changed = false;
        QPDFObjectHandle nv = clampScalar(obj, st, changed);
        if (changed) ctx.pdf.replaceObject(obj.getObjGen(), nv);
        continue;
      }
      clampObjectLimits(obj, visited, st);
    }
    int total = st.reals + st.ints + st.strings + st.names + st.arrays;
    if (total) {
      ctx.issue("LIMITS_CLAMPED",
                "enforced PDF 1.4 implementation limits: " + std::to_string(st.reals) +
                    " real(s), " + std::to_string(st.ints) + " integer(s), " +
                    std::to_string(st.strings) + " string(s), " + std::to_string(st.names) +
                    " name(s), " + std::to_string(st.arrays) + " array(s)",
                true);
    }
  }

  if (ctx.isA()) {
    static const std::set<std::string> kStdFilters = {
        "/FlateDecode", "/Fl", "/ASCIIHexDecode", "/AHx", "/ASCII85Decode", "/A85",
        "/LZWDecode", "/LZW", "/RunLengthDecode", "/RL", "/CCITTFaxDecode", "/CCF",
        "/JBIG2Decode", "/DCTDecode", "/DCT", "/JPXDecode", "/Crypt"};
    int nsf = 0;
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (!obj.isStream()) continue;
      QPDFObjectHandle sd = obj.getDict();
      QPDFObjectHandle filt = sd.getKey("/Filter");
      std::vector<std::string> names;
      if (filt.isName()) names.push_back(filt.getName());
      if (filt.isArray()) {
        for (int i = 0; i < filt.getArrayNItems(); ++i) {
          names.push_back(nameOf(filt.getArrayItem(i)));
        }
      }
      bool bad = false;
      for (const std::string& n : names) {
        if (!n.empty() && kStdFilters.count(n) == 0) bad = true;
      }
      if (bad) {
        obj.replaceStreamData(std::string(), QPDFObjectHandle::newNull(),
                              QPDFObjectHandle::newNull());
        sd.removeKey("/Filter");
        sd.removeKey("/DecodeParms");
        ++nsf;
      }
    }
    if (nsf) {
      ctx.issue("NONSTANDARD_FILTER_REMOVED",
                "emptied " + std::to_string(nsf) +
                    " stream(s) using non-standard filters (content not recoverable)",
                true);
    }
  }

  QPDFPageDocumentHelper dh(ctx.pdf);
  Visited streamVisited;
  int boxFixed = 0;
  for (auto& ph : dh.getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    if (ctx.isA()) {
      for (const char* bk : {"/MediaBox", "/CropBox", "/BleedBox", "/TrimBox", "/ArtBox"}) {
        QPDFObjectHandle box = bk == std::string("/MediaBox") || bk == std::string("/CropBox")
                                   ? ph.getAttribute(bk, true)
                                   : page.getKey(bk);
        if (!box.isArray() || box.getArrayNItems() != 4) continue;
        double v[4];
        bool nums = true;
        for (int i = 0; i < 4; ++i) {
          if (!box.getArrayItem(i).isNumber()) { nums = false; break; }
          v[i] = box.getArrayItem(i).getNumericValue();
        }
        if (!nums) continue;
        double x1 = std::min(v[0], v[2]), x2 = std::max(v[0], v[2]);
        double y1 = std::min(v[1], v[3]), y2 = std::max(v[1], v[3]);
        bool ch = false;
        if (x2 - x1 < 3) { x2 = x1 + 3; ch = true; }
        if (y2 - y1 < 3) { y2 = y1 + 3; ch = true; }
        if (x2 - x1 > 14400) { x2 = x1 + 14400; ch = true; }
        if (y2 - y1 > 14400) { y2 = y1 + 14400; ch = true; }
        if (ch) {
          QPDFObjectHandle nb = QPDFObjectHandle::newArray();
          nb.appendItem(QPDFObjectHandle::newReal(x1, 2));
          nb.appendItem(QPDFObjectHandle::newReal(y1, 2));
          nb.appendItem(QPDFObjectHandle::newReal(x2, 2));
          nb.appendItem(QPDFObjectHandle::newReal(y2, 2));
          page.replaceKey(bk, nb);
          ++boxFixed;
        }
      }
    }
    QPDFObjectHandle contents = page.getKey("/Contents");
    bool pageOk = true;
    if (contents.isStream()) {
      pageOk = streamDecodable(contents);
    } else if (contents.isArray()) {
      for (int ci = 0; ci < contents.getArrayNItems() && pageOk; ++ci) {
        QPDFObjectHandle part = contents.getArrayItem(ci);
        if (part.isStream()) pageOk = streamDecodable(part);
      }
    }
    if (pageOk) {
      ph.addContentTokenFilter(std::shared_ptr<QPDFObjectHandle::TokenFilter>(
          new ContentFixFilter(pdf14, limits23)));
    } else {
      ctx.issue("CONTENT_UNDECODABLE", "page content stream could not be decoded; left as-is",
                false);
    }
    attachToContentStreams(ctx, page.getKey("/Resources"), streamVisited);
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (annots.isArray()) {
      for (int i = 0; i < annots.getArrayNItems(); ++i) {
        QPDFObjectHandle a = annots.getArrayItem(i);
        if (!a.isDictionary()) continue;
        QPDFObjectHandle ap = a.getKey("/AP");
        if (!ap.isDictionary()) continue;
        QPDFObjectHandle n = ap.getKey("/N");
        std::vector<QPDFObjectHandle> streams;
        if (n.isStream()) streams.push_back(n);
        else if (n.isDictionary()) {
          for (const std::string& k : n.getKeys()) {
            if (n.getKey(k).isStream()) streams.push_back(n.getKey(k));
          }
        }
        for (QPDFObjectHandle s : streams) {
          if (streamVisited.enter(s)) {
            attachFilter(ctx, s, pdf14, limits23);
            attachToContentStreams(ctx, s.getDict().getKey("/Resources"), streamVisited);
          }
        }
      }
    }
  }
  if (boxFixed) {
    ctx.issue("PAGE_BOX_CLAMPED",
              "clamped " + std::to_string(boxFixed) +
                  " page box(es) into the 3..14400 unit range",
              true);
  }
  ctx.issue("CONTENT_FILTERED", "content streams normalized for conformance limits", true);
}
}
