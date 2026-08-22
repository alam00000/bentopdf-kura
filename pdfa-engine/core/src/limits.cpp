#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "ctx.hh"
#include "images.hh"
#include "passes.hh"
#include "limits.hh"
#include "util.hh"

namespace pdfa {
namespace {
bool containsInlineTerminator(const std::string& data) {
  auto isWs = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0';
  };
  for (size_t i = 0; i + 1 < data.size(); ++i) {
    if (data[i] != 'E' || data[i + 1] != 'I') continue;
    bool beforeOk = i == 0 || isWs(static_cast<unsigned char>(data[i - 1]));
    bool afterOk = i + 2 >= data.size() || isWs(static_cast<unsigned char>(data[i + 2]));
    if (beforeOk && afterOk) return true;
  }
  return false;
}

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
  char suffix[16];
  std::snprintf(suffix, sizeof(suffix), "X%08X", fnv1a32(n));
  return n.substr(0, kMaxName - 9) + suffix;
}

std::string lzwDecode(const std::string& in, bool& ok) {
  ok = false;
  std::vector<std::string> table;
  auto reset = [&]() {
    table.clear();
    for (int i = 0; i < 258; ++i) {
      table.push_back(i < 256 ? std::string(1, static_cast<char>(i)) : std::string());
    }
  };
  reset();
  int width = 9;
  std::string out, prev;
  size_t bitpos = 0;
  auto next = [&](int& code) -> bool {
    if (bitpos + width > in.size() * 8) return false;
    code = 0;
    for (int b = 0; b < width; ++b) {
      size_t p = bitpos + b;
      code = (code << 1) |
             ((static_cast<unsigned char>(in[p / 8]) >> (7 - p % 8)) & 1);
    }
    bitpos += width;
    return true;
  };
  int code = 0;
  while (next(code)) {
    if (code == 256) {
      reset();
      width = 9;
      prev.clear();
      continue;
    }
    if (code == 257) {
      ok = true;
      return out;
    }
    std::string entry;
    if (code >= 0 && code < static_cast<int>(table.size()) && code < 256) {
      entry = table[code];
    } else if (code >= 258 && code < static_cast<int>(table.size())) {
      entry = table[code];
    } else if (code == static_cast<int>(table.size()) && !prev.empty()) {
      entry = prev + prev[0];
    } else {
      return out;
    }
    if (out.size() + entry.size() > 100000000) return out;
    out += entry;
    if (!prev.empty()) table.push_back(prev + entry[0]);
    prev = entry;
    if (table.size() >= (1u << width) - 1 && width < 12) ++width;
  }
  ok = true;
  return out;
}

class ContentFixFilter : public QPDFObjectHandle::TokenFilter {
 public:
  ContentFixFilter(bool pdf14, bool limits23, bool scrubActualText = false,
                   int* iiFixed = nullptr, int* puaFixed = nullptr)
      : pdf14(pdf14), limits(pdf14 || limits23),
        maxStr(pdf14 ? kMaxString1 : kMaxString23), scrubActualText(scrubActualText),
        iiFixed(iiFixed), puaFixed(puaFixed) {}

  void handleToken(QPDFTokenizer::Token const& token) override {
    QPDFTokenizer::token_type_e type = token.getType();
    if (inImageDict) {
      if (type == QPDFTokenizer::tt_word && token.getValue() == "ID") return;
      if (type == QPDFTokenizer::tt_inline_image) {
        processInlineImage(token.getRawValue());
        inImageDict = false;
        swallowEI = true;
        return;
      }
      if (type == QPDFTokenizer::tt_word && token.getValue() == "EI") {
        write("BI " + iiText + " EI\n");
        iiText.clear();
        inImageDict = false;
        return;
      }
      iiText += token.getRawValue();
      return;
    }
    if (swallowEI) {
      if (type == QPDFTokenizer::tt_space) return;
      swallowEI = false;
      if (type == QPDFTokenizer::tt_word && token.getValue() == "EI") return;
    }
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
    if (type == QPDFTokenizer::tt_word && token.getValue() == "ri" && haveName) {
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
      iiText.clear();
      return;
    }
    if (type == QPDFTokenizer::tt_string && scrubActualText && haveName &&
        heldName.getValue() == "/ActualText") {
      bool changed = false;
      std::string cleaned = stripPuaUtf8(
          QPDFObjectHandle::newString(token.getValue()).getUTF8Value(), changed);
      if (changed) {
        flushHeld();
        write(QPDFObjectHandle::newUnicodeString(cleaned).unparse());
        if (puaFixed) ++(*puaFixed);
        return;
      }
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
        double clamped = v > 0 ? bound : -bound;
        writeToken(QPDFTokenizer::Token(
            QPDFTokenizer::tt_real,
            QPDFObjectHandle::newReal(clamped, pdf14 ? 1 : 0).getRealValue()));
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

  void processInlineImage(const std::string& data) {
    std::string dictText = iiText;
    iiText.clear();
    auto passthrough = [&]() { write("BI " + dictText + " ID\n" + data + "\nEI\n"); };
    QPDFObjectHandle dict;
    try {
      dict = QPDFObjectHandle::parse("<<" + dictText + ">>");
    } catch (...) {
      passthrough();
      return;
    }
    if (!dict.isDictionary()) {
      passthrough();
      return;
    }
    bool changed = false;
    for (const char* ik : {"/Intent"}) {
      if (dict.getKey(ik).isName() && kValidRi.count(dict.getKey(ik).getName()) == 0) {
        dict.replaceKey(ik, QPDFObjectHandle::newName("/RelativeColorimetric"));
        changed = true;
      }
    }
    for (const char* bk : {"/I", "/Interpolate"}) {
      if (dict.getKey(bk).isBool() && dict.getKey(bk).getBoolValue()) {
        dict.replaceKey(bk, QPDFObjectHandle::newBool(false));
        changed = true;
      }
    }
    static const std::map<std::string, std::string> kToAbbrev = {
        {"/AHx", "/AHx"}, {"/ASCIIHexDecode", "/AHx"},
        {"/A85", "/A85"}, {"/ASCII85Decode", "/A85"},
        {"/Fl", "/Fl"}, {"/FlateDecode", "/Fl"},
        {"/RL", "/RL"}, {"/RunLengthDecode", "/RL"},
        {"/CCF", "/CCF"}, {"/CCITTFaxDecode", "/CCF"},
        {"/DCT", "/DCT"}, {"/DCTDecode", "/DCT"}};
    QPDFObjectHandle f = dict.getKey("/F");
    if (f.isNull()) f = dict.getKey("/Filter");
    std::vector<std::string> names;
    if (f.isName()) names.push_back(f.getName());
    if (f.isArray()) {
      for (int i = 0; i < f.getArrayNItems(); ++i) names.push_back(nameOf(f.getArrayItem(i)));
    }
    std::string newData = data;
    std::vector<std::string> outNames;
    bool filterChanged = false;
    bool lzwSeen = false;
    for (const std::string& n : names) {
      std::string lower;
      for (char c : n) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      auto it = kToAbbrev.find(n);
      if (it != kToAbbrev.end()) {
        outNames.push_back(it->second);
        if (it->second != n) filterChanged = true;
      } else if (lower.find("lzw") != std::string::npos) {
        lzwSeen = true;
        outNames.push_back("/LZW?");
        filterChanged = true;
      } else {
        passthrough();
        return;
      }
    }
    if (lzwSeen) {
      if (names.size() != 1) {
        passthrough();
        return;
      }
      bool ok = false;
      std::string raw = lzwDecode(data, ok);
      if (!ok || raw.empty()) {
        passthrough();
        return;
      }
      QPDFObjectHandle dp = dict.getKey("/DP");
      if (dp.isNull()) dp = dict.getKey("/DecodeParms");
      long long predictor = 1;
      if (dp.isDictionary() && dp.getKey("/Predictor").isInteger()) {
        predictor = dp.getKey("/Predictor").getIntValue();
      }
      bool mustStayFiltered = predictor > 1;
      std::string packed = flateCompress(raw);
      if (!packed.empty() && (mustStayFiltered || packed.size() < raw.size())) {
        newData = packed;
        outNames.assign(1, "/Fl");
      } else if (!mustStayFiltered) {
        newData = raw;
        outNames.clear();
      } else {
        passthrough();
        return;
      }
      if (!mustStayFiltered) {
        dict.removeKey("/DP");
        dict.removeKey("/DecodeParms");
      }
      changed = true;
    }
    if (filterChanged || changed) {
      dict.removeKey("/F");
      dict.removeKey("/Filter");
      dict.removeKey("/L");
      dict.removeKey("/Length");
      if (outNames.size() == 1) {
        dict.replaceKey("/F", QPDFObjectHandle::newName(outNames[0]));
      } else if (outNames.size() > 1) {
        QPDFObjectHandle arr = QPDFObjectHandle::newArray();
        for (const std::string& n : outNames) arr.appendItem(QPDFObjectHandle::newName(n));
        dict.replaceKey("/F", arr);
      }
      if (containsInlineTerminator(newData)) {
        static const char* kHex = "0123456789ABCDEF";
        std::string hex;
        hex.reserve(newData.size() * 2 + 1);
        for (unsigned char c : newData) {
          hex += kHex[c >> 4];
          hex += kHex[c & 0x0F];
        }
        hex += '>';
        newData = hex;
        outNames.insert(outNames.begin(), "/AHx");
        dict.removeKey("/F");
        dict.removeKey("/Filter");
        if (outNames.size() == 1) {
          dict.replaceKey("/F", QPDFObjectHandle::newName(outNames[0]));
        } else {
          QPDFObjectHandle arr = QPDFObjectHandle::newArray();
          for (const std::string& n : outNames) arr.appendItem(QPDFObjectHandle::newName(n));
          dict.replaceKey("/F", arr);
        }
      }
      std::string out = "BI";
      for (const std::string& k : dict.getKeys()) {
        out += " " + k + " " + dict.getKey(k).unparse();
      }
      out += " ID\n" + newData + "\nEI\n";
      write(out);
      if (iiFixed) ++(*iiFixed);
      return;
    }
    passthrough();
  }

  bool pdf14;
  bool limits;
  size_t maxStr;
  bool scrubActualText;
  int* iiFixed;
  int* puaFixed;
  bool inImageDict = false;
  bool swallowEI = false;
  bool haveName = false;
  int qDepth = 0;
  int qSuppressed = 0;
  int arrDepth = 0;
  int dictDepth = 0;
  QPDFTokenizer::Token heldName{QPDFTokenizer::tt_bad, ""};
  std::vector<QPDFTokenizer::Token> heldTrail;
  std::string iiText;
};

QPDFObjectHandle clampScalar(QPDFObjectHandle v, LimitStats& st, bool& changed) {
  changed = false;
  if (v.isReal()) {
    double d = std::strtod(v.getRealValue().c_str(), nullptr);
    if (d > st.maxReal || d < -st.maxReal) {
      changed = true;
      ++st.reals;
      return QPDFObjectHandle::newReal(d > 0 ? st.maxReal : -st.maxReal,
                                       st.maxReal > kMaxReal ? 0 : 1);
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

bool wantPuaScrub(Ctx& ctx) {
  return (ctx.isA() && ctx.part >= 4) || ctx.opt.ua;
}

void attachFilter(Ctx& ctx, QPDFObjectHandle s, bool pdf14, bool limits23) {
  if (streamDecodable(s)) {
    s.addTokenFilter(std::shared_ptr<QPDFObjectHandle::TokenFilter>(
        new ContentFixFilter(pdf14, limits23, wantPuaScrub(ctx), &ctx.inlineImagesFixed,
                             &ctx.contentPuaFixed)));
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
      if (inconsistent || depth > kMaxObjectWalk) { inconsistent = true; return 0; }
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
      QPDFObjectHandle kept = QPDFObjectHandle::newArray();
      bool dropped = false;
      for (int ci = 0; ci < contents.getArrayNItems(); ++ci) {
        QPDFObjectHandle part = contents.getArrayItem(ci);
        if (part.isStream()) {
          if (pageOk) pageOk = streamDecodable(part);
          kept.appendItem(part);
        } else {
          dropped = true;
        }
      }
      if (dropped) {
        page.replaceKey("/Contents", kept);
        ctx.issue("CONTENT_MISSING_REPLACED",
                  "removed unresolvable entries from the page /Contents array", true);
      }
      if (kept.getArrayNItems() == 0) pageOk = false;
    } else {
      pageOk = false;
      if (page.hasKey("/Contents")) {
        page.replaceKey("/Contents",
                        ctx.pdf.makeIndirectObject(
                            QPDFObjectHandle::newStream(&ctx.pdf, std::string())));
        ctx.issue("CONTENT_MISSING_REPLACED",
                  "page /Contents did not resolve to a usable stream; replaced with an "
                  "empty content stream",
                  true);
      }
    }
    if (pageOk) {
      ph.addContentTokenFilter(std::shared_ptr<QPDFObjectHandle::TokenFilter>(
          new ContentFixFilter(pdf14, limits23, wantPuaScrub(ctx), &ctx.inlineImagesFixed,
                               &ctx.contentPuaFixed)));
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
        std::vector<QPDFObjectHandle> streams = normalAppearanceStreams(a);
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
