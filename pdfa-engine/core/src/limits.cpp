#include <qpdf/Pl_Discard.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "ctx.hh"
#include "fonts_ft.hh"
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

std::string realText(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.1f", v);
  return buf;
}
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

thread_local int* tViolations = nullptr;
thread_local const std::set<QPDFObjGen>* tReached = nullptr;
void violation() {
  if (tViolations) ++*tViolations;
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
    if (type == QPDFTokenizer::tt_word) {
      static const std::set<std::string> named = {"Tf", "Do", "gs", "sh", "scn", "SCN"};
      static const std::set<std::string> device = {"/DeviceRGB", "/DeviceCMYK", "/DeviceGray", "/Pattern"};
      const std::string& w = token.getValue();
      if (nameSinceOp && (named.count(w) || ((w == "cs" || w == "CS") && !device.count(lastNameVal)))) usedNamed = true;
      nameSinceOp = false;
    }
    if (type == QPDFTokenizer::tt_name) {
      nameSinceOp = true;
      lastNameVal = token.getValue();
      flushHeld();
      heldName = token;
      if (limits && token.getValue().size() > kMaxName + 1) {
        violation();
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
          violation();
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
        violation();
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
        violation();
        if (puaFixed) ++(*puaFixed);
        return;
      }
    }
    if (type == QPDFTokenizer::tt_string && !token.getRawValue().empty() && token.getRawValue()[0] == '<') {
      int digits = 0;
      for (char hc : token.getRawValue().substr(1)) {
        if (std::isxdigit(static_cast<unsigned char>(hc))) ++digits;
        else if (hc != '>' && !std::isspace(static_cast<unsigned char>(hc)) && hc != 0 && hc != 12) violation();
      }
      if (digits % 2) violation();
    }
    if (type == QPDFTokenizer::tt_bad) {
      violation();
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
        violation();
        flushHeld();
        return;
      }
      --arrDepth;
    } else if (type == QPDFTokenizer::tt_dict_open) {
      ++dictDepth;
    } else if (type == QPDFTokenizer::tt_dict_close) {
      if (dictDepth == 0) {
        violation();
        flushHeld();
        return;
      }
      --dictDepth;
    }
    if (type == QPDFTokenizer::tt_word && !inImageDict &&
        kValidOps.count(token.getValue()) == 0) {
      violation();
      flushHeld();
      return;
    }
    flushHeld();
    if (limits && type == QPDFTokenizer::tt_real) {
      double v = std::strtod(token.getValue().c_str(), nullptr);
      double bound = pdf14 ? kMaxReal : kMaxReal2;
      if (v > bound || v < -bound) {
        violation();
        double clamped = v > 0 ? bound : -bound;
        writeToken(QPDFTokenizer::Token(QPDFTokenizer::tt_real, realText(clamped)));
        return;
      }
      if (v != 0 && v > -kMinDenorm && v < kMinDenorm) {
        if (pdf14) violation();
        writeToken(QPDFTokenizer::Token(QPDFTokenizer::tt_integer, "0"));
        return;
      }
    }
    if (limits && type == QPDFTokenizer::tt_integer) {
      long long v = std::strtoll(token.getValue().c_str(), nullptr, 10);
      if (v > kMaxInt || v < -kMaxInt) {
        violation();
        writeToken(QPDFTokenizer::Token(QPDFTokenizer::tt_integer,
                                        std::to_string(v > 0 ? kMaxInt : -kMaxInt)));
        return;
      }
    }
    if (limits && type == QPDFTokenizer::tt_string) {
      std::string v = token.getValue();
      if (v.size() > maxStr) {
        violation();
        v.resize(maxStr);
      }
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
        violation();
        dict.replaceKey(ik, QPDFObjectHandle::newName("/RelativeColorimetric"));
        changed = true;
      }
    }
    for (const char* bk : {"/I", "/Interpolate"}) {
      if (dict.getKey(bk).isBool() && dict.getKey(bk).getBoolValue()) {
        violation();
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
        violation();
        lzwSeen = true;
        outNames.push_back("/LZW?");
        filterChanged = true;
      } else {
        violation();
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

  bool nameSinceOp = false;
  std::string lastNameVal;

 public:
  bool usedNamed = false;

 private:
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
      return QPDFObjectHandle::newReal(realText(d > 0 ? st.maxReal : -st.maxReal));
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

class NameCollector : public QPDFObjectHandle::TokenFilter {
 public:
  explicit NameCollector(std::set<std::string>& names) : names(names) {}
  void handleToken(QPDFTokenizer::Token const& t) override {
    if (t.getType() == QPDFTokenizer::tt_name) names.insert(t.getValue());
  }

 private:
  std::set<std::string>& names;
};

int pruneOversizedResources(QPDF& pdf) {
  static const char* kCats[] = {"/ExtGState", "/Font", "/XObject", "/ColorSpace", "/Pattern", "/Shading", "/Properties"};
  std::vector<QPDFObjectHandle> big;
  std::vector<QPDFObjectHandle> todo;
  for (QPDFObjectHandle obj : pdf.getAllObjects()) {
    todo.push_back(obj.isStream() ? obj.getDict() : obj);
    while (!todo.empty()) {
      QPDFObjectHandle d = todo.back();
      todo.pop_back();
      if (!d.isDictionary()) continue;
      for (const char* c : kCats) {
        QPDFObjectHandle sub = d.getKey(c);
        if (sub.isDictionary() && sub.getKeys().size() > 4095) big.push_back(sub);
      }
      for (const std::string& k : d.getKeys()) {
        QPDFObjectHandle v = d.getKey(k);
        if (!v.isIndirect() && v.isDictionary()) todo.push_back(v);
      }
    }
  }
  if (big.empty()) return 0;
  std::set<std::string> used;
  NameCollector nc(used);
  for (QPDFObjectHandle obj : pdf.getAllObjects()) {
    if (!obj.isStream()) continue;
    QPDFObjectHandle d = obj.getDict();
    bool content = nameIs(d.getKey("/Subtype"), "/Form") || d.getKey("/PatternType").isInteger();
    if (!content) continue;
    Pl_Discard discard;
    try {
      obj.filterAsContents(&nc, &discard);
    } catch (...) {
    }
  }
  for (auto& ph : QPDFPageDocumentHelper(pdf).getAllPages()) {
    Pl_Discard discard;
    try {
      ph.filterContents(&nc, &discard);
    } catch (...) {
    }
  }
  for (QPDFObjectHandle obj : pdf.getAllObjects()) {
    QPDFObjectHandle procs = obj.isDictionary() && nameIs(obj.getKey("/Subtype"), "/Type3") ? obj.getKey("/CharProcs") : QPDFObjectHandle();
    for (const std::string& k : procs.isDictionary() ? procs.getKeys() : std::set<std::string>()) {
      Pl_Discard discard;
      try {
        if (procs.getKey(k).isStream()) procs.getKey(k).filterAsContents(&nc, &discard);
      } catch (...) {
      }
    }
  }
  int removed = 0;
  for (QPDFObjectHandle sub : big) {
    for (const std::string& k : sub.getKeys()) {
      if (!used.count(k)) {
        sub.removeKey(k);
        ++removed;
      }
    }
  }
  return removed;
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
  if (ctx.opt.verifyOnly && tReached && s.isIndirect() && !tReached->count(s.getObjGen())) return;
  if (streamDecodable(s) && ctx.opt.verifyOnly) {
    ContentFixFilter f(pdf14, limits23, wantPuaScrub(ctx));
    Pl_Discard discard;
    try {
      s.filterAsContents(&f, &discard);
    } catch (...) {
    }
    if (ctx.isA() && ctx.part >= 2 && f.usedNamed && nameIs(s.getDict().getKey("/Subtype"), "/Form") &&
        !s.getDict().getKey("/Resources").isDictionary()) {
      violation();
    }
  } else if (streamDecodable(s)) {
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

int csComponents(QPDFObjectHandle cs, QPDFObjectHandle res, int depth, bool& pattern) {
  pattern = false;
  if (depth > 8) return -1;
  if (cs.isName()) {
    std::string n = cs.getName();
    if (n == "/DeviceGray" || n == "/G" || n == "/CalGray") return 1;
    if (n == "/DeviceRGB" || n == "/RGB" || n == "/CalRGB" || n == "/Lab") return 3;
    if (n == "/DeviceCMYK" || n == "/CMYK") return 4;
    if (n == "/Pattern") {
      pattern = true;
      return 0;
    }
    QPDFObjectHandle d = res.isDictionary() ? res.getKey("/ColorSpace") : QPDFObjectHandle();
    if (d.isInitialized() && d.isDictionary() && d.hasKey(n)) {
      return csComponents(d.getKey(n), QPDFObjectHandle(), depth + 1, pattern);
    }
    return -1;
  }
  if (!cs.isArray() || cs.getArrayNItems() < 1) return -1;
  std::string f = nameOf(cs.getArrayItem(0));
  if (cs.getArrayNItems() == 1) return csComponents(cs.getArrayItem(0), res, depth + 1, pattern);
  if (f == "/ICCBased") {
    QPDFObjectHandle st = cs.getArrayItem(1);
    return st.isStream() && st.getDict().getKey("/N").isInteger()
               ? static_cast<int>(st.getDict().getKey("/N").getIntValue())
               : -1;
  }
  if (f == "/CalRGB" || f == "/Lab") return 3;
  if (f == "/CalGray" || f == "/Indexed" || f == "/I" || f == "/Separation") return 1;
  if (f == "/DeviceN") return cs.getArrayItem(1).isArray() ? cs.getArrayItem(1).getArrayNItems() : -1;
  if (f == "/Pattern") {
    bool ignored;
    int n = csComponents(cs.getArrayItem(1), res, depth + 1, ignored);
    pattern = true;
    return n;
  }
  return -1;
}

class ColorOperandCheck : public QPDFObjectHandle::ParserCallbacks {
 public:
  ColorOperandCheck(QPDFObjectHandle res, bool page) : res(res) {
    if (page) cur = {{1, false}, {1, false}};
  }
  int bad = 0;

  void handleObject(QPDFObjectHandle obj) override {
    if (!obj.isOperator()) {
      ops.push_back(obj);
      return;
    }
    std::string op = obj.getOperatorValue();
    if (op == "q") {
      stack.push_back(cur);
    } else if (op == "Q") {
      if (!stack.empty()) {
        cur = stack.back();
        stack.pop_back();
      }
    } else if (op == "cs" || op == "CS") {
      Side& sd = op == "cs" ? cur.fill : cur.stroke;
      sd.n = -1;
      if (!ops.empty() && ops.back().isName()) sd.n = csComponents(ops.back(), res, 0, sd.pattern);
    } else if (op == "g" || op == "G" || op == "rg" || op == "RG" || op == "k" || op == "K") {
      int want = (op == "g" || op == "G") ? 1 : (op == "rg" || op == "RG") ? 3 : 4;
      if (numbers() != want || ops.size() != static_cast<size_t>(want)) ++bad;
      (op[0] >= 'a' ? cur.fill : cur.stroke) = {want, false};
    } else if (op == "sc" || op == "SC" || op == "scn" || op == "SCN") {
      const Side& sd = op[0] == 's' ? cur.fill : cur.stroke;
      bool named = !ops.empty() && ops.back().isName();
      bool n2 = op.size() == 3;
      if (sd.n >= 0) {
        if (sd.pattern) {
          if (!n2 || !named || numbers() != sd.n) ++bad;
        } else if (named || numbers() != sd.n || ops.size() != static_cast<size_t>(sd.n)) {
          ++bad;
        }
      }
    }
    ops.clear();
  }
  void handleEOF() override {}

 private:
  struct Side {
    int n = -1;
    bool pattern = false;
  };
  struct State {
    Side fill, stroke;
  };
  int numbers() const {
    int c = 0;
    for (const auto& o : ops) c += o.isNumber() ? 1 : 0;
    return c;
  }
  QPDFObjectHandle res;
  State cur;
  std::vector<State> stack;
  std::vector<QPDFObjectHandle> ops;
};

int checkColorOperandsIn(QPDFObjectHandle holder, QPDFObjectHandle res, bool page, Visited& seen, int depth) {
  if (depth > 32) return 0;
  int bad = 0;
  try {
    ColorOperandCheck cb(res, page);
    if (page) {
      QPDFPageObjectHelper(holder).parseContents(&cb);
    } else {
      holder.parseAsContents(&cb);
    }
    bad += cb.bad;
  } catch (...) {
  }
  if (!res.isDictionary()) return bad;
  for (const char* k : {"/XObject", "/Pattern"}) {
    QPDFObjectHandle d = res.getKey(k);
    if (!d.isDictionary()) continue;
    for (const std::string& n : d.getKeys()) {
      QPDFObjectHandle x = d.getKey(n);
      if (!x.isStream() || !seen.enter(x)) continue;
      QPDFObjectHandle xd = x.getDict();
      if (std::string(k) == "/XObject" && !nameIs(xd.getKey("/Subtype"), "/Form")) continue;
      QPDFObjectHandle r = xd.getKey("/Resources");
      bad += checkColorOperandsIn(x, r.isDictionary() ? r : res, false, seen, depth + 1);
    }
  }
  return bad;
}

void checkColorOperands(Ctx& ctx) {
  Visited seen;
  int bad = 0;
  for (auto& ph : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    bad += checkColorOperandsIn(page, ph.getAttribute("/Resources", false), true, seen, 0);
    QPDFObjectHandle annots = page.getKey("/Annots");
    for (int i = 0; annots.isArray() && i < annots.getArrayNItems(); ++i) {
      if (!annots.getArrayItem(i).isDictionary()) continue;
      for (QPDFObjectHandle s : normalAppearanceStreams(annots.getArrayItem(i))) {
        if (seen.enter(s)) bad += checkColorOperandsIn(s, s.getDict().getKey("/Resources"), false, seen, 1);
      }
    }
  }
  if (bad) {
    ctx.issue("CONTENT_COLOR_OPERANDS_INVALID",
              std::to_string(bad) +
                  " colour operator(s) whose operand count does not match the current colour space",
              ctx.opt.verifyOnly);
  }
}

void passLimits(Ctx& ctx) {
  checkColorOperands(ctx);
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
    if (pdf14) {
      int pruned = pruneOversizedResources(ctx.pdf);
      if (pruned) {
        ctx.issue("RESOURCES_PRUNED",
                  "removed " + std::to_string(pruned) +
                      " unused resource entries from dictionaries over the PDF/A-1 limit of 4095 keys",
                  true);
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

  int kuraPage1 = 0;
  for (auto& ph : dh.getAllPages()) {
    PageScope kuraScope1(ctx, ++kuraPage1);
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
    if (ctx.opt.verifyOnly) {
      continue;
    } else if (pageOk) {
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

namespace pdfa {
namespace {
size_t eolLen(const unsigned char* d, size_t n, size_t i) {
  if (i < n && d[i] == '\r') return i + 1 < n && d[i + 1] == '\n' ? 2 : 1;
  if (i < n && d[i] == '\n') return 1;
  return 0;
}

bool pdfWs(unsigned char c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
bool pdfDelim(unsigned char c) { return pdfWs(c) || std::strchr("()<>[]{}/%", c) != nullptr; }

std::string headerProblem(const unsigned char* d, size_t n, int part) {
  auto digit = [&](size_t i) { return i < n && std::isdigit(d[i]); };
  if (n < 8 || std::memcmp(d, "%PDF-", 5) != 0 || !digit(5) || d[6] != '.' || !digit(7))
    return "file header does not start with %PDF-n.n at byte 0";
  if (part >= 2) {
    bool versionOk = part >= 4 ? d[5] == '2' : d[5] == '1' && d[7] <= '7';
    if (!versionOk || !eolLen(d, n, 8)) return "file header version or line ending does not conform";
  }
  size_t i = 8;
  while (i < n && d[i] != '\r' && d[i] != '\n') ++i;
  i += eolLen(d, n, i);
  if (i + 5 > n || d[i] != '%') return "file header is not followed by a binary comment line";
  for (size_t k = 1; k <= 4; ++k) {
    if (d[i + k] < 128) return "binary comment after the file header has fewer than four high bytes";
  }
  return std::string();
}

std::string xrefProblem(const unsigned char* d, size_t n, int part) {
  for (size_t i = 0; i + 4 <= n; ++i) {
    if (std::memcmp(d + i, "xref", 4) != 0) continue;
    if (i > 0 && d[i - 1] != '\n' && d[i - 1] != '\r') continue;
    if (i >= 9 && std::memcmp(d + i - 9, "startxref", 9) == 0) continue;
    size_t j = i + 4;
    size_t e = eolLen(d, n, j);
    if (j < n && !std::isdigit(d[j]) && !pdfWs(d[j])) continue;
    if (!e || j + e >= n || !std::isdigit(d[j + e])) {
      return "the xref keyword is not followed by a single EOL marker";
    }
    j += e;
    while (j < n && std::memcmp(d + j, "trailer", 7) != 0) {
      size_t le = j;
      while (le < n && d[le] != '\r' && d[le] != '\n') ++le;
      std::string line(reinterpret_cast<const char*>(d + j), le - j);
      std::vector<std::string> f;
      size_t p = 0;
      while (p < line.size()) {
        while (p < line.size() && pdfWs(static_cast<unsigned char>(line[p]))) ++p;
        size_t b = p;
        while (p < line.size() && !pdfWs(static_cast<unsigned char>(line[p]))) ++p;
        if (p > b) f.push_back(line.substr(b, p - b));
      }
      if (f.size() == 2 && line.find(f[0] + " " + f[1]) != 0 && part <= 3) {
        return "a cross-reference subsection header does not separate its numbers with a single space";
      }
      if (f.empty() && le == j) {
        ++j;
        continue;
      }
      j = le + eolLen(d, n, le);
      if (le == n) break;
    }
  }
  return std::string();
}
}

void verifyContentStreams(Ctx& ctx) {
  bool pdf14 = ctx.pdf14Target();
  bool limits23 = ctx.isA() && ctx.part >= 2 && ctx.part <= 3;
  int contentViolations = 0;
  std::set<QPDFObjGen> reached;
  collectFontUse(ctx.pdf, &reached);
  tViolations = &contentViolations;
  tReached = &reached;
  Visited streamVisited;
  for (auto& ph : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    ContentFixFilter f(pdf14, limits23, wantPuaScrub(ctx));
    Pl_Discard discard;
    try {
      ph.filterContents(&f, &discard);
    } catch (...) {
    }
    if (ctx.part >= 2 && f.usedNamed && !page.getKey("/Resources").isDictionary()) violation();
    attachToContentStreams(ctx, page.getKey("/Resources"), streamVisited);
    QPDFObjectHandle annots = page.getKey("/Annots");
    for (int i = 0; annots.isArray() && i < annots.getArrayNItems(); ++i) {
      QPDFObjectHandle a = annots.getArrayItem(i);
      if (!a.isDictionary()) continue;
      for (QPDFObjectHandle st : normalAppearanceStreams(a)) {
        if (streamVisited.enter(st)) {
          attachFilter(ctx, st, pdf14, limits23);
          attachToContentStreams(ctx, st.getDict().getKey("/Resources"), streamVisited);
        }
      }
    }
  }
  tViolations = nullptr;
  tReached = nullptr;
  if (contentViolations) {
    ctx.issue("CONTENT_NONCONFORMING",
              std::to_string(contentViolations) +
                  " content stream token(s) violate PDF/A content rules or implementation limits",
              true);
  }
}

void checkFileSyntax(Ctx& ctx, const unsigned char* d, size_t n, bool encrypted) {
  verifyContentStreams(ctx);
  std::vector<std::string> problems;
  auto add = [&](const std::string& s) {
    if (!s.empty()) problems.push_back(s);
  };
  add(headerProblem(d, n, ctx.part));
  if (encrypted) add("the file is encrypted");
  QPDFObjectHandle id = ctx.pdf.getTrailer().getKey("/ID");
  if (!id.isArray() || id.getArrayNItems() != 2 || !id.getArrayItem(0).isString() || !id.getArrayItem(1).isString() ||
      id.getArrayItem(0).getStringValue().empty() || id.getArrayItem(1).getStringValue().empty())
    add("the trailer has no valid /ID file identifier");
  {
    std::string tail(reinterpret_cast<const char*>(d) + (n > 2048 ? n - 2048 : 0), n > 2048 ? 2048 : n);
    size_t eof = tail.rfind("%%EOF");
    if (eof == std::string::npos) {
      add("the file does not end with an end-of-file marker");
    } else {
      std::string rest = tail.substr(eof + 5);
      if (!rest.empty() && rest != "\n" && rest != "\r" && rest != "\r\n") add("data follows the last end-of-file marker");
    }
  }
  add(xrefProblem(d, n, ctx.part));
  if (!encrypted) {
    std::map<QPDFObjGen, QPDFXRefEntry> xref;
    try {
      xref = ctx.pdf.getXRefTable();
    } catch (...) {
    }
    int badObj = 0, badStream = 0, badHex = 0;
    for (const auto& kv : xref) {
      if (kv.second.getType() != 1) continue;
      long long off = kv.second.getOffset();
      if (off < 0 || static_cast<size_t>(off) >= n) continue;
      size_t i = static_cast<size_t>(off);
      std::string num = std::to_string(kv.first.getObj());
      auto headerAt = [&](size_t k) {
        if (k + num.size() >= n || std::memcmp(d + k, num.data(), num.size()) != 0) return false;
        if (k > 0 && std::isdigit(d[k - 1])) return false;
        size_t m = k + num.size();
        if (m >= n || !pdfWs(d[m])) return false;
        while (m < n && pdfWs(d[m])) ++m;
        while (m < n && std::isdigit(d[m])) ++m;
        while (m < n && pdfWs(d[m])) ++m;
        return m + 3 <= n && std::memcmp(d + m, "obj", 3) == 0;
      };
      bool at = headerAt(i);
      for (size_t delta = 1; !at && delta <= 32; ++delta) {
        if (i >= delta && headerAt(i - delta)) {
          i -= delta;
          at = true;
        } else if (headerAt(i + delta)) {
          i += delta;
          at = true;
        }
      }
      if (!at) continue;
      size_t p = i;
      while (p < n && std::isdigit(d[p])) ++p;
      bool headOk = p > i && p < n && pdfWs(d[p]) && !pdfWs(d[p + 1]) && (i == 0 || d[i - 1] == '\n' || d[i - 1] == '\r');
      size_t q = p + 1;
      while (q < n && std::isdigit(d[q])) ++q;
      headOk = headOk && q > p + 1 && q + 4 <= n && pdfWs(d[q]) && std::memcmp(d + q + 1, "obj", 3) == 0;
      if (!headOk) ++badObj;
      size_t body = q + 4;
      size_t endObj = body;
      bool hexBad = false;
      while (endObj < n) {
        unsigned char c = d[endObj];
        if (c == '%') {
          while (endObj < n && d[endObj] != '\r' && d[endObj] != '\n') ++endObj;
          continue;
        }
        if (c == '(') {
          int depth = 0;
          while (endObj < n) {
            if (d[endObj] == '\\') {
              endObj += 2;
              continue;
            }
            if (d[endObj] == '(') ++depth;
            if (d[endObj] == ')' && --depth == 0) break;
            ++endObj;
          }
          ++endObj;
          continue;
        }
        if (c == '<' && endObj + 1 < n && d[endObj + 1] == '<') {
          endObj += 2;
          continue;
        }
        if (c == '<') {
          size_t h = endObj + 1;
          int digits = 0;
          while (h < n && d[h] != '>') {
            if (std::isxdigit(d[h])) ++digits;
            else if (!pdfWs(d[h])) hexBad = true;
            ++h;
          }
          if (digits % 2) hexBad = true;
          endObj = h + 1;
          continue;
        }
        if (c == 's' && endObj + 6 <= n && std::memcmp(d + endObj, "stream", 6) == 0 &&
            (endObj == 0 || pdfDelim(d[endObj - 1]))) {
          size_t s = endObj + 6;
          bool kwOk = s < n && (d[s] == '\n' || (d[s] == '\r' && s + 1 < n && d[s + 1] == '\n'));
          s += s < n && d[s] == '\r' ? (s + 1 < n && d[s + 1] == '\n' ? 2 : 1) : (s < n && d[s] == '\n' ? 1 : 0);
          QPDFObjectHandle obj = ctx.pdf.getObjectByObjGen(kv.first);
          QPDFObjectHandle len = obj.isStream() ? obj.getDict().getKey("/Length") : QPDFObjectHandle::newNull();
          bool lenOk = false;
          if (len.isInteger() && len.getIntValue() >= 0 && s + static_cast<size_t>(len.getIntValue()) <= n) {
            size_t e = s + static_cast<size_t>(len.getIntValue());
            size_t el = eolLen(d, n, e);
            lenOk = e + el + 9 <= n && std::memcmp(d + e + el, "endstream", 9) == 0;
          }
          if (!kwOk || !lenOk) ++badStream;
          const unsigned char* es = static_cast<const unsigned char*>(
              memmem(d + s, n - s, "endstream", 9));
          if (!es) break;
          endObj = static_cast<size_t>(es - d) + 9;
          continue;
        }
        if (c == 'e' && endObj + 6 <= n && std::memcmp(d + endObj, "endobj", 6) == 0) break;
        if (c == 'o' && endObj + 3 <= n && std::memcmp(d + endObj, "obj", 3) == 0 && pdfDelim(d[endObj - 1])) break;
        ++endObj;
      }
      if (hexBad) ++badHex;
      if (endObj + 6 <= n && std::memcmp(d + endObj, "endobj", 6) == 0) {
        bool before = endObj > 0 && (d[endObj - 1] == '\n' || d[endObj - 1] == '\r');
        bool after = endObj + 6 == n || eolLen(d, n, endObj + 6) || false;
        bool objEol = eolLen(d, n, q + 4) != 0;
        if (!before || !after || !objEol) ++badObj;
      }
    }
    if (badObj) add(std::to_string(badObj) + " indirect object header(s) or obj/endobj keywords are not delimited as required");
    if (badStream) add(std::to_string(badStream) + " stream(s) with a wrong /Length or stream keyword line ending");
    if (badHex) add(std::to_string(badHex) + " object(s) containing malformed hexadecimal strings");
  }
  int big = 0;
  std::vector<QPDFObjectHandle> todo;
  for (QPDFObjectHandle obj : ctx.part == 1 ? ctx.pdf.getAllObjects() : std::vector<QPDFObjectHandle>()) {
    todo.push_back(obj.isStream() ? obj.getDict() : obj);
    while (!todo.empty()) {
      QPDFObjectHandle dd = todo.back();
      todo.pop_back();
      if (dd.isDictionary()) {
        if (dd.getKeys().size() > 4095) ++big;
        for (const std::string& k : dd.getKeys()) {
          QPDFObjectHandle v = dd.getKey(k);
          if (!v.isIndirect() && (v.isDictionary() || v.isArray())) todo.push_back(v);
        }
      } else if (dd.isArray()) {
        if (dd.getArrayNItems() > 8191) ++big;
        for (int i = 0; i < dd.getArrayNItems(); ++i) {
          QPDFObjectHandle v = dd.getArrayItem(i);
          if (!v.isIndirect() && (v.isDictionary() || v.isArray())) todo.push_back(v);
        }
      }
    }
  }
  if (big) add(std::to_string(big) + " dictionary or array object(s) exceed the PDF/A-1 size limits");
  int lzw = 0, ps = 0, pres = 0;
  std::set<QPDFObjGen> reached;
  collectFontUse(ctx.pdf, &reached);
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (!obj.isStream() && !obj.isDictionary()) continue;
    QPDFObjectHandle dict = obj.isStream() ? obj.getDict() : obj;
    if (obj.isStream()) {
      QPDFObjectHandle f = dict.getKey("/Filter");
      std::vector<QPDFObjectHandle> fs;
      if (f.isArray()) fs = f.getArrayAsVector();
      else fs.push_back(f);
      for (QPDFObjectHandle& x : fs) {
        if (nameIs(x, "/LZWDecode") || nameIs(x, "/LZW")) ++lzw;
      }
      if (nameIs(dict.getKey("/Subtype"), "/Form") && obj.isIndirect() && reached.count(obj.getObjGen()) &&
          (dict.hasKey("/PS") || nameIs(dict.getKey("/Subtype2"), "/PS") || (ctx.part >= 2 && dict.hasKey("/OPI"))))
        ++ps;
    }
    if (ctx.part >= 4 && nameIs(dict.getKey("/Type"), "/Page") && dict.hasKey("/PresSteps")) ++pres;
  }
  bool xrefStream = false;
  try {
    for (const auto& kv : ctx.pdf.getXRefTable()) xrefStream = xrefStream || kv.second.getType() == 2;
  } catch (...) {
  }
  for (const char* pat : {"/Type/XRef", "/Type /XRef"}) {
    xrefStream = xrefStream || memmem(d, n, pat, std::strlen(pat)) != nullptr;
  }
  if (xrefStream && ctx.part == 1) add("cross-reference streams or object streams are used");
  if (lzw) add("LZWDecode filter used by " + std::to_string(lzw) + " stream(s)");
  if (ps) add(std::to_string(ps) + " form XObject(s) carry PostScript or OPI keys");
  if (pres) add(std::to_string(pres) + " page(s) carry a /PresSteps entry");
  for (const std::string& p : problems) ctx.issue("FILE_STRUCTURE_NONCONFORMING", p, true);
}
}
