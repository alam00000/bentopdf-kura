#include <qpdf/Pl_Buffer.hh>
#include <qpdf/Pl_Discard.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
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
constexpr double kMaxReal2 = 3.403e38 * (1 + 1e-9);
constexpr double kSafeReal2 = 3.402823e38;

std::string realText(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.1f", v);
  return buf;
}
constexpr double kMinDenorm = 1.175e-38 * (1 - 1e-9);
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
  bool tinyReals = true;
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

const std::map<std::string, int> kOpArity = {
    {"w", 1}, {"J", 1}, {"j", 1}, {"M", 1}, {"d", 2}, {"ri", 1}, {"i", 1}, {"gs", 1},
    {"q", 0}, {"Q", 0}, {"cm", 6}, {"m", 2}, {"l", 2}, {"c", 6}, {"v", 4}, {"y", 4},
    {"h", 0}, {"re", 4}, {"S", 0}, {"s", 0}, {"f", 0}, {"F", 0}, {"f*", 0}, {"B", 0},
    {"B*", 0}, {"b", 0}, {"b*", 0}, {"n", 0}, {"W", 0}, {"W*", 0}, {"BT", 0}, {"ET", 0},
    {"Tc", 1}, {"Tw", 1}, {"Tz", 1}, {"TL", 1}, {"Tf", 2}, {"Tr", 1}, {"Ts", 1},
    {"Td", 2}, {"TD", 2}, {"Tm", 6}, {"T*", 0}, {"Tj", 1}, {"TJ", 1}, {"'", 1}, {"\"", 3},
    {"d0", 2}, {"d1", 6}, {"CS", 1}, {"cs", 1}, {"G", 1}, {"g", 1}, {"RG", 3}, {"rg", 3},
    {"K", 4}, {"k", 4}, {"sh", 1}, {"Do", 1}, {"MP", 1}, {"DP", 2},
    {"BMC", 1}, {"BDC", 2}, {"EMC", 0}};

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
thread_local int tColorFixed = 0;
thread_local const std::set<QPDFObjGen>* tReached = nullptr;
thread_local bool tWholeLargeReals = false;
thread_local int tPart = 0;
thread_local bool tStrictX = false;
thread_local bool tLaxIntent = false;
void violation() {
  if (tViolations) ++*tViolations;
}
void repairOnly() {
  if (tPart == 0) violation();
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
        flushPending();
        inImageDict = false;
        swallowEI = true;
        return;
      }
      if (type == QPDFTokenizer::tt_word && token.getValue() == "EI") {
        write("BI " + iiText + " EI\n");
        flushPending();
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
    if (!boxes.empty() && type != QPDFTokenizer::tt_space && type != QPDFTokenizer::tt_comment &&
        type != QPDFTokenizer::tt_array_close && type != QPDFTokenizer::tt_dict_close)
      ++boxes.back().second;
    if (type == QPDFTokenizer::tt_word) {
      static const std::set<std::string> named = {"Tf", "Do", "gs", "sh", "scn", "SCN"};
      static const std::set<std::string> device = {"/DeviceRGB", "/DeviceCMYK", "/DeviceGray", "/Pattern"};
      const std::string& w = token.getValue();
      if (nameSinceOp && (named.count(w) || ((w == "cs" || w == "CS") && !device.count(lastNameVal)))) usedNamed = true;
      nameSinceOp = false;
    }
    if (arrDepth == 0 && dictDepth == 0 &&
        (type == QPDFTokenizer::tt_name || type == QPDFTokenizer::tt_string ||
         type == QPDFTokenizer::tt_integer || type == QPDFTokenizer::tt_real ||
         type == QPDFTokenizer::tt_bool || type == QPDFTokenizer::tt_null ||
         (type == QPDFTokenizer::tt_word &&
          (token.getValue() == "true" || token.getValue() == "false" || token.getValue() == "null")) ||
         type == QPDFTokenizer::tt_array_open || type == QPDFTokenizer::tt_dict_open))
    {
      ++operands;
      opTypes.push_back(type == QPDFTokenizer::tt_word ? QPDFTokenizer::tt_bool : type);
      nameOperand = type == QPDFTokenizer::tt_name;
      if (operands == 1) firstOperand = type;
      if (operands == 2) secondOperand = type;
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
          operands = 0;
          return;
        }
        ++qDepth;
      } else if (w == "Q") {
        if (qSuppressed > 0) {
          --qSuppressed;
          flushHeld();
          operands = 0;
          return;
        }
        if (qDepth > 0) --qDepth;
      }
    }
    if (haveName && (type == QPDFTokenizer::tt_space || type == QPDFTokenizer::tt_comment)) {
      heldTrail.push_back(token);
      return;
    }
    if (type == QPDFTokenizer::tt_word && token.getValue() == "ri" && haveName && operands == 1) {
      operands = 0;
      if (kValidRi.count(heldName.getValue()) == 0) {
        if (tPart != 4 && !tLaxIntent) violation();
        heldName = QPDFTokenizer::Token(QPDFTokenizer::tt_name, "/RelativeColorimetric");
      }
      flushHeld();
      writeToken(token);
      return;
    }
    if (type == QPDFTokenizer::tt_word && token.getValue() == "BI") {
      operands = 0;
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
      std::string hex;
      bool badHex = false;
      for (char hc : token.getRawValue().substr(1)) {
        if (std::isxdigit(static_cast<unsigned char>(hc))) hex += hc;
        else if (hc != '>' && !std::isspace(static_cast<unsigned char>(hc)) && hc != 0 && hc != 12) badHex = true;
      }
      if (badHex || hex.size() % 2) {
        violation();
        flushHeld();
        if (hex.size() % 2) hex += '0';
        write("<" + hex + ">");
        return;
      }
    }
    bool prevBad = afterBad;
    if (type != QPDFTokenizer::tt_space && type != QPDFTokenizer::tt_comment) afterBad = type == QPDFTokenizer::tt_bad;
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
      boxes.push_back({'a', 0});
    } else if (type == QPDFTokenizer::tt_array_close) {
      if (arrDepth == 0) {
        repairOnly();
        flushHeld();
        return;
      }
      --arrDepth;
      closeBox();
    } else if (type == QPDFTokenizer::tt_dict_open) {
      ++dictDepth;
      boxes.push_back({'d', 0});
    } else if (type == QPDFTokenizer::tt_dict_close) {
      if (dictDepth == 0) {
        violation();
        flushHeld();
        return;
      }
      --dictDepth;
      closeBox();
    }
    if (type == QPDFTokenizer::tt_word && !inImageDict &&
        kValidOps.count(token.getValue()) == 0) {
      violation();
      if (arrDepth == 0 && dictDepth == 0 && !prevBad) {
        haveName = false;
        heldTrail.clear();
        pending.clear();
        operands = 0;
      } else {
        flushHeld();
      }
      return;
    }
    if (type == QPDFTokenizer::tt_word && arrDepth == 0 && dictDepth == 0 &&
        token.getValue() != "true" && token.getValue() != "false" && token.getValue() != "null") {
      auto ar = kOpArity.find(token.getValue());
      int have = operands;
      bool endedWithName = nameOperand;
      operands = 0;
      nameOperand = false;
      if (knowPatterns && endedWithName &&
          (((token.getValue() == "scn" || token.getValue() == "SCN") && !patterns.count(lastNameVal)) ||
           (token.getValue() == "gs" && !gstates.count(lastNameVal)))) {
        if (tPart == 0 || tPart >= 4) violation();
        haveName = false;
        heldTrail.clear();
        pending.clear();
        return;
      }
      const std::string& op = token.getValue();
      std::vector<QPDFTokenizer::token_type_e> types;
      types.swap(opTypes);
      if (types.size() > static_cast<size_t>(have)) types.erase(types.begin(), types.end() - have);
      if (tStrictX && strictOperandProblem(op, types, endedWithName)) violation();
      bool bad = ar != kOpArity.end() && have != ar->second;
      bool tagged = op == "BMC" || op == "BDC" || op == "MP" || op == "DP";
      if (!bad && tagged && firstOperand != QPDFTokenizer::tt_name) bad = true;
      if (!bad && (op == "BDC" || op == "DP") && secondOperand != QPDFTokenizer::tt_name &&
          secondOperand != QPDFTokenizer::tt_dict_open)
        bad = true;
      if (op == "BMC" || op == "BDC") markDropped.push_back(bad);
      if (op == "EMC") {
        bool dropEmc = markDropped.empty() || markDropped.back();
        if (markDropped.empty()) repairOnly();
        if (!markDropped.empty()) markDropped.pop_back();
        if (dropEmc) {
          haveName = false;
          heldTrail.clear();
          pending.clear();
          return;
        }
      }
      if (bad) {
        repairOnly();
        haveName = false;
        heldTrail.clear();
        pending.clear();
        if (ar->second == 0) writeToken(token);
        return;
      }
    }
    flushHeld();
    if (limits && type == QPDFTokenizer::tt_real) {
      double v = std::strtod(token.getValue().c_str(), nullptr);
      if (!pdf14 && tWholeLargeReals && (v > kMaxReal || v < -kMaxReal) && v <= kMaxInt && v >= -kMaxInt) {
        violation();
        writeToken(QPDFTokenizer::Token(QPDFTokenizer::tt_integer, std::to_string(std::llround(v))));
        return;
      }
      double bound = pdf14 ? kMaxReal : kMaxReal2;
      if (v > bound || v < -bound) {
        violation();
        double safe = pdf14 ? kMaxReal : kSafeReal2;
        double clamped = v > 0 ? safe : -safe;
        writeToken(QPDFTokenizer::Token(QPDFTokenizer::tt_real, realText(clamped)));
        return;
      }
      if (v != 0 && v > -kMinDenorm && v < kMinDenorm) {
        if (tPart == 2 || tPart == 3 || tPart == 0) violation();
        writeToken(QPDFTokenizer::Token(QPDFTokenizer::tt_integer, "0"));
        return;
      }
    }
    if (limits && type == QPDFTokenizer::tt_integer) {
      long long v = std::strtoll(token.getValue().c_str(), nullptr, 10);
      if (v > kMaxInt || v < -kMaxInt - 1) {
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
    if (tStrictX && (arrDepth > 0 || dictDepth > 0)) violation();
    while (arrDepth > 0) {
      write("]");
      --arrDepth;
    }
    while (dictDepth > 0) {
      write(">>");
      --dictDepth;
    }
    for (bool dropped : markDropped) {
      if (dropped) continue;
      repairOnly();
      write("\nEMC\n");
    }
    markDropped.clear();
    flushPending();
  }

  void setPatterns(QPDFObjectHandle res, bool prune = false) {
    if (!res.isDictionary()) return;
    knowPatterns = true;
    for (const char* cat : {"/Font", "/XObject", "/Shading", "/Properties", "/ColorSpace"}) {
      QPDFObjectHandle d = res.getKey(cat);
      if (d.isDictionary()) {
        for (const std::string& k : d.getKeys()) known[cat].insert(k);
      }
    }
    QPDFObjectHandle pat = res.getKey("/Pattern");
    if (pat.isDictionary()) {
      for (const std::string& k : pat.getKeys()) patterns.insert(k);
    }
    QPDFObjectHandle gsd = res.getKey("/ExtGState");
    if (gsd.isDictionary()) {
      for (const std::string& k : gsd.getKeys()) {
        QPDFObjectHandle g = gsd.getKey(k);
        bool valid = g.isDictionary() && !g.isStream() &&
                     (!g.hasKey("/Type") || nameIs(g.getKey("/Type"), "/ExtGState"));
        if (valid) gstates.insert(k);
        else if (prune) gsd.removeKey(k);
      }
    }
  }

 private:
  std::string pending;
  bool afterBad = false;

  void write(const std::string& text) {
    pending += text;
    if (pending.size() > (1u << 20)) flushPending();
  }
  void writeToken(const QPDFTokenizer::Token& t) {
    write(t.getRawValue());
    if (t.getType() == QPDFTokenizer::tt_word && arrDepth == 0 && dictDepth == 0 && kValidOps.count(t.getValue()) &&
        t.getValue() != "true" && t.getValue() != "false" && t.getValue() != "null")
      flushPending();
  }
  void flushPending() {
    if (pending.empty()) return;
    QPDFObjectHandle::TokenFilter::write(pending);
    pending.clear();
  }

  void closeBox() {
    if (boxes.empty()) return;
    int limit = boxes.back().first == 'a' ? kMaxArray : 2 * 4095;
    if (pdf14 && limits && boxes.back().second > limit) violation();
    boxes.pop_back();
  }

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
    if (pdf14 && limits && dict.getKeys().size() > 4095) violation();
    if (tStrictX && inlineDataProblem(dict, data)) violation();
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
  std::vector<QPDFTokenizer::token_type_e> opTypes;
  std::map<std::string, std::set<std::string>> known;

  bool strictOperandProblem(const std::string& op, const std::vector<QPDFTokenizer::token_type_e>& t, bool endedWithName) {
    using T = QPDFTokenizer;
    auto num = [&](size_t i) { return i < t.size() && (t[i] == T::tt_integer || t[i] == T::tt_real); };
    auto is = [&](size_t i, QPDFTokenizer::token_type_e want) { return i < t.size() && t[i] == want; };
    static const std::set<std::string> numeric = {"w", "J", "j", "M", "i", "cm", "m", "l", "c", "v", "y", "re",
                                                  "Tc", "Tw", "Tz", "TL", "Tr", "Ts", "Td", "TD", "Tm", "d0",
                                                  "d1", "G", "g", "RG", "rg", "K", "k"};
    static const std::set<std::string> named = {"gs", "sh", "Do", "CS", "cs", "ri", "MP", "BMC", "BDC", "DP"};
    if (numeric.count(op)) {
      for (size_t i = 0; i < t.size(); ++i) {
        if (!num(i)) return true;
      }
    } else if (named.count(op)) {
      if (!t.empty() && !is(0, T::tt_name)) return true;
    } else if (op == "Tf") {
      if (t.size() == 2 && (!is(0, T::tt_name) || !num(1))) return true;
    } else if (op == "Tj" || op == "'") {
      if (t.size() == 1 && !is(0, T::tt_string)) return true;
    } else if (op == "TJ") {
      if (t.size() == 1 && !is(0, T::tt_array_open)) return true;
    } else if (op == "\"") {
      if (t.size() == 3 && (!num(0) || !num(1) || !is(2, T::tt_string))) return true;
    } else if (op == "d") {
      if (t.size() == 2 && (!is(0, T::tt_array_open) || !num(1))) return true;
    }
    if (!knowPatterns) return false;
    if (op == "Tf") return t.size() == 2 && is(0, T::tt_name) && !known["/Font"].count(lastNameVal);
    if (!endedWithName) return false;
    static const std::set<std::string> device = {"/DeviceRGB", "/DeviceCMYK", "/DeviceGray", "/Pattern"};
    if (op == "Do") return !known["/XObject"].count(lastNameVal);
    if (op == "sh") return !known["/Shading"].count(lastNameVal);
    if ((op == "cs" || op == "CS") && !device.count(lastNameVal)) return !known["/ColorSpace"].count(lastNameVal);
    if ((op == "BDC" || op == "DP") && t.size() == 2 && is(1, T::tt_name)) return !known["/Properties"].count(lastNameVal);
    return false;
  }

  static bool inlineDataProblem(QPDFObjectHandle dict, const std::string& data) {
    QPDFObjectHandle f = dict.getKey("/F");
    if (f.isNull()) f = dict.getKey("/Filter");
    if (f.isArray() && f.getArrayNItems() == 1) f = f.getArrayItem(0);
    if (nameIs(f, "/AHx") || nameIs(f, "/ASCIIHexDecode")) return data.find('>') == std::string::npos;
    if (nameIs(f, "/A85") || nameIs(f, "/ASCII85Decode")) return data.find("~>") == std::string::npos;
    if (!f.isNull()) return false;
    auto num = [&](const char* a, const char* b) {
      QPDFObjectHandle v = dict.getKey(a);
      if (v.isNull()) v = dict.getKey(b);
      return v.isInteger() ? v.getIntValue() : -1LL;
    };
    long long w = num("/W", "/Width"), h = num("/H", "/Height"), bpc = num("/BPC", "/BitsPerComponent");
    QPDFObjectHandle im = dict.getKey("/IM");
    if (im.isNull()) im = dict.getKey("/ImageMask");
    bool mask = im.isBool() && im.getBoolValue();
    if (mask) bpc = 1;
    QPDFObjectHandle cs = dict.getKey("/CS");
    if (cs.isNull()) cs = dict.getKey("/ColorSpace");
    long long comps = mask ? 1 : 0;
    std::string csn = cs.isName() ? cs.getName() : cs.isArray() && cs.getArrayNItems() > 0 ? nameOf(cs.getArrayItem(0)) : "";
    if (csn == "/G" || csn == "/DeviceGray" || csn == "/I" || csn == "/Indexed") comps = 1;
    if (csn == "/RGB" || csn == "/DeviceRGB") comps = 3;
    if (csn == "/CMYK" || csn == "/DeviceCMYK") comps = 4;
    if (w <= 0 || h <= 0 || bpc <= 0 || comps <= 0 || w > 100000 || h > 100000) return false;
    long long need = (w * comps * bpc + 7) / 8 * h;
    return static_cast<long long>(data.size()) < need;
  }

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
  int operands = 0;
  bool nameOperand = false;
  QPDFTokenizer::token_type_e firstOperand = QPDFTokenizer::tt_bad;
  QPDFTokenizer::token_type_e secondOperand = QPDFTokenizer::tt_bad;
  std::vector<bool> markDropped;
  bool knowPatterns = false;
  std::set<std::string> patterns;
  std::set<std::string> gstates;
  int qDepth = 0;
  int qSuppressed = 0;
  int arrDepth = 0;
  int dictDepth = 0;
  std::vector<std::pair<char, int>> boxes;
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
      double safe = st.maxReal == kMaxReal2 ? kSafeReal2 : st.maxReal;
      return QPDFObjectHandle::newReal(realText(d > 0 ? safe : -safe));
    }
    if (st.tinyReals && d != 0 && d > -kMinDenorm && d < kMinDenorm) {
      changed = true;
      ++st.reals;
      return QPDFObjectHandle::newInteger(0);
    }
  } else if (v.isInteger()) {
    long long i = v.getIntValue();
    if (i > kMaxInt || i < -kMaxInt - 1) {
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

bool wantLimits23(const Ctx& ctx) {
  return (ctx.isA() && ctx.part >= 2 && ctx.part <= 3) || ctx.opt.level == Level::X4 ||
         ctx.opt.level == Level::VT1 || ctx.isE();
}

bool wantPuaScrub(Ctx& ctx) {
  return (ctx.isA() && ctx.part >= 4) || ctx.opt.ua;
}

void attachFilter(Ctx& ctx, QPDFObjectHandle s, bool pdf14, bool limits23,
                  QPDFObjectHandle res = QPDFObjectHandle()) {
  if (ctx.opt.verifyOnly && tReached && s.isIndirect() && !tReached->count(s.getObjGen())) return;
  if (streamDecodable(s) && ctx.opt.verifyOnly) {
    ContentFixFilter f(pdf14, limits23, wantPuaScrub(ctx));
    f.setPatterns(res);
    Pl_Discard discard;
    try {
      s.filterAsContents(&f, &discard);
    } catch (...) {
    }
    if (((ctx.isA() && ctx.part >= 2) || ctx.pdf20Print()) && f.usedNamed && nameIs(s.getDict().getKey("/Subtype"), "/Form") &&
        !s.getDict().getKey("/Resources").isDictionary()) {
      violation();
    }
  } else if (streamDecodable(s)) {
    auto filter = std::make_shared<ContentFixFilter>(pdf14, limits23, wantPuaScrub(ctx),
                                                     &ctx.inlineImagesFixed, &ctx.contentPuaFixed);
    filter->setPatterns(res, true);
    s.addTokenFilter(filter);
  } else {
    ctx.issueMerged("CONTENT_UNDECODABLE", "content stream could not be decoded; left as-is", false);
  }
}

void attachToContentStreams(Ctx& ctx, QPDFObjectHandle res, Visited& visited) {
  DepthGuard g_(visited);
  if (g_.over) return;
  bool pdf14 = ctx.pdf14Target();
  bool limits23 = wantLimits23(ctx);
  if (!res.isDictionary() || !visited.enter(res)) return;
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (xod.isDictionary()) {
    for (const std::string& k : xod.getKeys()) {
      QPDFObjectHandle xo = xod.getKey(k);
      if (!xo.isStream() || !visited.enter(xo)) continue;
      if (nameIs(xo.getDict().getKey("/Subtype"), "/Form")) {
        attachFilter(ctx, xo, pdf14, limits23, xo.getDict().getKey("/Resources"));
        attachToContentStreams(ctx, xo.getDict().getKey("/Resources"), visited);
      }
    }
  }
  QPDFObjectHandle pat = res.getKey("/Pattern");
  if (pat.isDictionary()) {
    for (const std::string& k : pat.getKeys()) {
      QPDFObjectHandle p = pat.getKey(k);
      if (p.isStream() && visited.enter(p)) {
        attachFilter(ctx, p, pdf14, limits23, p.getDict().getKey("/Resources"));
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
          if (glyph.isStream() && visited.enter(glyph))
            attachFilter(ctx, glyph, pdf14, limits23, fnt.getKey("/Resources"));
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
  bool inlineImage = false;
  std::vector<std::pair<size_t, size_t>> badRanges;

  void handleObject(QPDFObjectHandle obj, size_t offset, size_t length) override {
    if (!obj.isOperator()) {
      if (ops.empty()) opStart = offset;
      if (obj.isInlineImage()) inlineImage = true;
      ops.push_back(obj);
      return;
    }
    if (ops.empty()) opStart = offset;
    int before = bad;
    handleOperator(obj);
    if (bad != before) badRanges.push_back({opStart, offset + length});
  }

  void handleOperator(QPDFObjectHandle obj) {
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
  size_t opStart = 0;
};

std::string withoutRanges(const std::string& data, const std::vector<std::pair<size_t, size_t>>& ranges) {
  std::string out;
  size_t pos = 0;
  for (const auto& r : ranges) {
    if (r.first < pos || r.second > data.size()) continue;
    out.append(data, pos, r.first - pos);
    pos = r.second;
  }
  out.append(data, pos, std::string::npos);
  return out;
}

int checkColorOperandsIn(Ctx& ctx, QPDFObjectHandle holder, QPDFObjectHandle res, bool page, Visited& seen, int depth) {
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
    if (cb.bad && !ctx.opt.verifyOnly && !cb.inlineImage) {
      std::string data;
      if (page) {
        Pl_Buffer buf("contents");
        QPDFPageObjectHelper(holder).pipeContents(&buf);
        auto b = buf.getBufferSharedPointer();
        data.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
      } else {
        auto b = holder.getStreamData(qpdf_dl_all);
        data.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
      }
      std::string fixed = withoutRanges(data, cb.badRanges);
      if (page) {
        holder.replaceKey("/Contents", ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, fixed)));
      } else {
        holder.replaceStreamData(fixed, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
      }
      tColorFixed += cb.bad;
    }
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
      bad += checkColorOperandsIn(ctx, x, r.isDictionary() ? r : res, false, seen, depth + 1);
    }
  }
  return bad;
}

void checkColorOperands(Ctx& ctx) {
  tColorFixed = 0;
  Visited seen;
  int bad = 0;
  for (auto& ph : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    bad += checkColorOperandsIn(ctx, page, ph.getAttribute("/Resources", false), true, seen, 0);
    QPDFObjectHandle annots = page.getKey("/Annots");
    for (int i = 0; annots.isArray() && i < annots.getArrayNItems(); ++i) {
      if (!annots.getArrayItem(i).isDictionary()) continue;
      for (QPDFObjectHandle s : normalAppearanceStreams(annots.getArrayItem(i))) {
        if (seen.enter(s)) bad += checkColorOperandsIn(ctx, s, s.getDict().getKey("/Resources"), false, seen, 1);
      }
    }
  }
  if (bad) {
    bool fixed = ctx.opt.verifyOnly || tColorFixed == bad;
    ctx.issue("CONTENT_COLOR_OPERANDS_INVALID",
              std::to_string(bad) +
                  (fixed ? " colour operator(s) whose operand count does not match the current colour space removed"
                         : " colour operator(s) whose operand count does not match the current colour space"),
              ctx.opt.verifyOnly ? !ctx.isA() : fixed);
  }
}

void fixIccChannelCounts(Ctx& ctx) {
  int fixed = 0;
  std::set<QPDFObjGen> seen;
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (!obj.isArray() || obj.getArrayNItems() < 2 || !nameIs(obj.getArrayItem(0), "/ICCBased")) continue;
    QPDFObjectHandle st = obj.getArrayItem(1);
    if (!st.isStream() || !seen.insert(st.getObjGen()).second) continue;
    std::string head;
    try {
      auto buf = st.getStreamData(qpdf_dl_all);
      if (buf->getSize() >= 20) head.assign(reinterpret_cast<const char*>(buf->getBuffer()), 20);
    } catch (...) {
    }
    if (head.size() < 20) continue;
    std::string cs = head.substr(16, 4);
    int n = cs == "GRAY" ? 1 : (cs == "RGB " || cs == "Lab " || cs == "XYZ ") ? 3 : cs == "CMYK" ? 4 : 0;
    QPDFObjectHandle cur = st.getDict().getKey("/N");
    if (n == 0 || (cur.isInteger() && cur.getIntValue() == n)) continue;
    if (!ctx.opt.verifyOnly) st.getDict().replaceKey("/N", QPDFObjectHandle::newInteger(n));
    ++fixed;
  }
  if (fixed) {
    ctx.issue("ICC_COMPONENTS_SET",
              "set the component count of " + std::to_string(fixed) +
                  " ICC-based colour space(s) from the profile header",
              true);
  }
}

void passLimits(Ctx& ctx) {
  tWholeLargeReals = ctx.isX() || ctx.isE();
  checkColorOperands(ctx);
  bool pdf14 = ctx.pdf14Target();
  bool limits23 = wantLimits23(ctx);
  {
    int extRefs = 0;
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (!obj.isStream()) continue;
      QPDFObjectHandle sd = obj.getDict();
      for (const char* k : {"/F", "/FFilter", "/FDecodeParms"}) {
        if (sd.hasKey(k)) {
          sd.removeKey(k);
          ++extRefs;
        }
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
    st.tinyReals = !(ctx.opt.verifyOnly && ctx.isA() && ctx.part == 1);
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

  if ((ctx.isA() || ctx.isX()) && !ctx.opt.verifyOnly) {
    int lzwChains = 0;
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
      if (!obj.isStream()) continue;
      QPDFObjectHandle sd = obj.getDict();
      QPDFObjectHandle filt = sd.getKey("/Filter");
      if (filt.isName()) {
        QPDFObjectHandle one = QPDFObjectHandle::newArray();
        one.appendItem(filt);
        filt = one;
      }
      if (!filt.isArray() || filt.getArrayNItems() < 1) continue;
      std::string first = nameOf(filt.getArrayItem(0));
      if (first != "/LZWDecode" && first != "/LZW") continue;
      QPDFObjectHandle dp0 = sd.getKey("/DecodeParms");
      if (dp0.isArray() && dp0.getArrayNItems() > 0) dp0 = dp0.getArrayItem(0);
      if (dp0.isDictionary() && dp0.getKey("/EarlyChange").isInteger() &&
          dp0.getKey("/EarlyChange").getIntValue() == 0)
        continue;
      std::string raw;
      try {
        auto buf = obj.getRawStreamData();
        raw.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
      } catch (...) {
        continue;
      }
      bool ok = false;
      std::string plain = lzwDecode(raw, ok);
      if (!ok) continue;
      QPDFObjectHandle nf = QPDFObjectHandle::newArray();
      nf.appendItem(QPDFObjectHandle::newName("/FlateDecode"));
      for (int i = 1; i < filt.getArrayNItems(); ++i) nf.appendItem(filt.getArrayItem(i));
      QPDFObjectHandle dp = sd.getKey("/DecodeParms");
      if (dp.isDictionary()) {
        QPDFObjectHandle wrap = QPDFObjectHandle::newArray();
        wrap.appendItem(dp);
        dp = wrap;
      }
      if (dp.isArray() && dp.getArrayNItems() > 0 && dp.getArrayItem(0).isDictionary()) {
        dp.getArrayItem(0).removeKey("/EarlyChange");
      }
      obj.replaceStreamData(flateCompress(plain), nf, dp.isArray() ? dp : QPDFObjectHandle::newNull());
      ++lzwChains;
    }
    if (lzwChains) {
      ctx.issue("LZW_REENCODED",
                "re-encoded the LZW stage of " + std::to_string(lzwChains) + " stream(s) as Flate",
                true);
    }
  }

  if (ctx.isA() || ctx.isX()) {
    static const std::set<std::string> kAllFilters = {
        "/FlateDecode", "/Fl", "/ASCIIHexDecode", "/AHx", "/ASCII85Decode", "/A85",
        "/LZWDecode", "/LZW", "/RunLengthDecode", "/RL", "/CCITTFaxDecode", "/CCF",
        "/JBIG2Decode", "/DCTDecode", "/DCT", "/JPXDecode", "/Crypt"};
    static const std::set<std::string> kPrintFilters = {
        "/FlateDecode", "/Fl", "/ASCIIHexDecode", "/AHx", "/ASCII85Decode", "/A85",
        "/RunLengthDecode", "/RL", "/CCITTFaxDecode", "/CCF",
        "/JBIG2Decode", "/DCTDecode", "/DCT", "/JPXDecode", "/Crypt"};
    static const std::set<std::string> kFullFilters = {
        "/FlateDecode", "/ASCIIHexDecode", "/ASCII85Decode", "/RunLengthDecode", "/CCITTFaxDecode",
        "/JBIG2Decode", "/DCTDecode", "/JPXDecode", "/Crypt"};
    const bool fullNames = ctx.isA() && ctx.part >= 2;
    const std::set<std::string>& kStdFilters = ctx.isX() ? kPrintFilters : fullNames ? kFullFilters : kAllFilters;
    int nsf = 0, recoded = 0;
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
      QPDFObjectHandle parms = sd.getKey("/DecodeParms");
      for (size_t fi = 0; fi < names.size(); ++fi) {
        const std::string& n = names[fi];
        if (!n.empty() && kStdFilters.count(n) == 0) bad = true;
        if (n == "/Crypt" && ctx.isX() && !ctx.pdf20Print()) bad = true;
        if (n == "/Crypt" && (ctx.isX() || ctx.part >= 2)) {
          QPDFObjectHandle dp = parms.isArray() ? parms.getArrayItem(static_cast<int>(fi)) : parms;
          if (dp.isDictionary() && dp.getKey("/Name").isName() && dp.getKey("/Name").getName() != "/Identity") bad = true;
          if (fullNames && dp.isDictionary() && !nameIs(dp.getKey("/Name"), "/Identity")) bad = true;
        }
      }
      if (bad && ctx.opt.verifyOnly && ctx.isA() && ctx.part == 1) continue;
      if (bad && ctx.opt.verifyOnly) {
        ++nsf;
        continue;
      }
      if (bad) {
        try {
          auto plain = obj.getStreamData(qpdf_dl_generalized);
          obj.replaceStreamData(std::string(reinterpret_cast<const char*>(plain->getBuffer()), plain->getSize()),
                                QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
          ++recoded;
          continue;
        } catch (...) {
        }
        obj.replaceStreamData(std::string(), QPDFObjectHandle::newNull(),
                              QPDFObjectHandle::newNull());
        sd.removeKey("/Filter");
        sd.removeKey("/DecodeParms");
        ++nsf;
      }
    }
    if (recoded) {
      ctx.issue("STREAM_FILTER_REPLACED",
                "decoded " + std::to_string(recoded) + " stream(s) whose compression the target does not allow", true);
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
    auto imageCodec = [](QPDFObjectHandle st) {
      QPDFObjectHandle f = st.getDict().getKey("/Filter");
      std::vector<QPDFObjectHandle> fs;
      if (f.isArray()) fs = f.getArrayAsVector();
      else fs.push_back(f);
      for (QPDFObjectHandle& x : fs) {
        if (nameIs(x, "/JBIG2Decode") || nameIs(x, "/JPXDecode") || nameIs(x, "/DCTDecode") ||
            nameIs(x, "/CCITTFaxDecode"))
          return true;
      }
      return false;
    };
    QPDFObjectHandle thumb = page.getKey("/Thumb");
    if (ctx.pdf14Target() && thumb.isStream() && imageCodec(thumb) &&
        !nameIs(thumb.getDict().getKey("/Filter"), "/DCTDecode")) {
      page.removeKey("/Thumb");
      ctx.issue("THUMBNAIL_REMOVED", "removed a page thumbnail stored with a compression this level does not allow",
                !ctx.opt.verifyOnly || !ctx.isA());
    }
    QPDFObjectHandle contents = page.getKey("/Contents");
    if (contents.isStream() && imageCodec(contents)) {
      page.replaceKey("/Contents", ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, "")));
      contents = page.getKey("/Contents");
      ctx.issue("CONTENT_MISSING_REPLACED", "page content was stored with an image compression and cannot be drawn; replaced with empty content", true);
    }
    bool pageOk = true;
    if (contents.isStream()) {
      pageOk = streamDecodable(contents);
    } else if (contents.isArray()) {
      QPDFObjectHandle kept = QPDFObjectHandle::newArray();
      bool dropped = false;
      for (int ci = 0; ci < contents.getArrayNItems(); ++ci) {
        QPDFObjectHandle part = contents.getArrayItem(ci);
        if (part.isStream() && imageCodec(part)) {
          dropped = true;
        } else if (part.isStream()) {
          if (pageOk) pageOk = streamDecodable(part);
          kept.appendItem(part);
        } else {
          dropped = true;
        }
      }
      if (dropped) {
        page.replaceKey("/Contents", kept);
        ctx.issue("CONTENT_MISSING_REPLACED",
                  "removed unresolvable entries from the page /Contents array", !ctx.opt.verifyOnly || !ctx.isA());
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
                  !ctx.opt.verifyOnly || !ctx.isA());
      }
    }
    if (ctx.opt.verifyOnly) {
      continue;
    } else if (pageOk) {
      auto filter = std::make_shared<ContentFixFilter>(pdf14, limits23, wantPuaScrub(ctx),
                                                       &ctx.inlineImagesFixed, &ctx.contentPuaFixed);
      filter->setPatterns(ph.getAttribute("/Resources", false), true);
      ph.addContentTokenFilter(filter);
    } else {
      ctx.issueMerged("CONTENT_UNDECODABLE", "page content stream could not be decoded; left as-is",
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
            attachFilter(ctx, s, pdf14, limits23, s.getDict().getKey("/Resources"));
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
              !ctx.opt.verifyOnly || !ctx.isA() || ctx.part == 2 || ctx.part == 3);
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

const unsigned char* findBytes(const unsigned char* d, size_t n, const char* pat) {
  const unsigned char* e = std::search(d, d + n, pat, pat + std::strlen(pat));
  return e == d + n ? nullptr : e;
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

size_t skipLiteral(const unsigned char* d, size_t n, size_t i) {
  int depth = 0;
  while (i < n) {
    if (d[i] == '\\') {
      i += 2;
      continue;
    }
    if (d[i] == '(') ++depth;
    if (d[i] == ')' && --depth == 0) return i + 1;
    ++i;
  }
  return n;
}

bool hexStringsBad(const unsigned char* d, size_t b, size_t e) {
  for (size_t i = b; i < e;) {
    unsigned char c = d[i];
    if (c == '%') {
      while (i < e && d[i] != '\r' && d[i] != '\n') ++i;
    } else if (c == '(') {
      i = skipLiteral(d, e, i);
    } else if (c == '<' && i + 1 < e && d[i + 1] == '<') {
      i += 2;
    } else if (c == '<') {
      int digits = 0;
      for (++i; i < e && d[i] != '>'; ++i) {
        if (std::isxdigit(d[i])) ++digits;
        else if (!pdfWs(d[i])) return true;
      }
      if (digits % 2) return true;
      ++i;
    } else {
      ++i;
    }
  }
  return false;
}

size_t dictSpanEnd(const unsigned char* d, size_t n, size_t i) {
  int depth = 0;
  while (i < n) {
    if (d[i] == '(') {
      i = skipLiteral(d, n, i);
    } else if (d[i] == '<' && i + 1 < n && d[i + 1] == '<') {
      ++depth;
      i += 2;
    } else if (d[i] == '>' && i + 1 < n && d[i + 1] == '>') {
      i += 2;
      if (--depth == 0) return i;
    } else {
      ++i;
    }
  }
  return 0;
}

std::vector<std::pair<size_t, size_t>> trailerSpans(const unsigned char* d, size_t n) {
  std::vector<std::pair<size_t, size_t>> out;
  for (const unsigned char* p = findBytes(d, n, "trailer"); p; p = findBytes(p + 7, n - (p + 7 - d), "trailer")) {
    size_t i = static_cast<size_t>(p - d) + 7;
    while (i < n && pdfWs(d[i])) ++i;
    if (i + 1 >= n || d[i] != '<' || d[i + 1] != '<') continue;
    size_t e = dictSpanEnd(d, n, i);
    if (e) out.push_back({i, e});
  }
  return out;
}

QPDFObjectHandle lastXRefStream(QPDF& pdf, const unsigned char* d, size_t n) {
  const unsigned char* last = nullptr;
  for (const unsigned char* p = findBytes(d, n, "startxref"); p; p = findBytes(p + 9, n - (p + 9 - d), "startxref")) last = p;
  if (!last) return QPDFObjectHandle::newNull();
  size_t rest = n - static_cast<size_t>(last + 9 - d);
  long long off = std::strtoll(std::string(reinterpret_cast<const char*>(last) + 9, std::min<size_t>(32, rest)).c_str(),
                               nullptr, 10);
  if (off <= 0 || static_cast<size_t>(off) >= n) return QPDFObjectHandle::newNull();
  size_t at = static_cast<size_t>(off);
  const unsigned char* obj = findBytes(d + at, std::min<size_t>(64, n - at), "obj");
  if (!obj) return QPDFObjectHandle::newNull();
  size_t i = static_cast<size_t>(obj - d) + 3;
  while (i < n && pdfWs(d[i])) ++i;
  if (i + 1 >= n || d[i] != '<' || d[i + 1] != '<') return QPDFObjectHandle::newNull();
  size_t e = dictSpanEnd(d, n, i);
  if (!e) return QPDFObjectHandle::newNull();
  try {
    QPDFObjectHandle dict = QPDFObjectHandle::parse(&pdf, std::string(reinterpret_cast<const char*>(d) + i, e - i));
    if (!dict.isDictionary() || !nameIs(dict.getKey("/Type"), "/XRef")) return QPDFObjectHandle::newNull();
    size_t s = e;
    while (s < n && pdfWs(d[s])) ++s;
    std::string body;
    if (s + 6 <= n && std::memcmp(d + s, "stream", 6) == 0 && dict.getKey("/Length").isInteger()) {
      s += 6;
      if (s < n && d[s] == '\r') ++s;
      if (s < n && d[s] == '\n') ++s;
      size_t len = static_cast<size_t>(std::max<long long>(0, dict.getKey("/Length").getIntValue()));
      body.assign(reinterpret_cast<const char*>(d) + s, std::min(len, n - s));
    }
    QPDFObjectHandle st = QPDFObjectHandle::newStream(&pdf, body);
    st.replaceDict(dict);
    return st;
  } catch (...) {
  }
  return QPDFObjectHandle::newNull();
}

long long xrefStreamInUse(QPDFObjectHandle xs) {
  QPDFObjectHandle dict = xs.getDict();
  QPDFObjectHandle w = dict.getKey("/W");
  if (!w.isArray() || w.getArrayNItems() < 3) return 0;
  int wid[3];
  for (int k = 0; k < 3; ++k) {
    wid[k] = w.getArrayItem(k).isInteger() ? static_cast<int>(w.getArrayItem(k).getIntValue()) : -1;
    if (wid[k] < 0 || wid[k] > 8) return 0;
  }
  size_t row = static_cast<size_t>(wid[0] + wid[1] + wid[2]);
  if (!row) return 0;
  std::string data;
  try {
    auto buf = xs.getStreamData(qpdf_dl_generalized);
    data.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return 0;
  }
  long long used = 0;
  for (size_t at = 0; at + row <= data.size(); at += row) {
    unsigned type = wid[0] ? 0 : 1;
    for (int k = 0; k < wid[0]; ++k) type = (type << 8) | static_cast<unsigned char>(data[at + k]);
    if (type != 0) ++used;
  }
  return used;
}

bool trailerId(QPDFObjectHandle trailer, std::string& id) {
  QPDFObjectHandle arr = trailer.isDictionary() ? trailer.getKey("/ID") : QPDFObjectHandle::newNull();
  if (!arr.isArray()) return false;
  id.clear();
  for (int i = 0; i < arr.getArrayNItems(); ++i) {
    if (arr.getArrayItem(i).isString()) id += arr.getArrayItem(i).getStringValue();
  }
  return true;
}

int postEofSize(const unsigned char* d, size_t n) {
  const char* marker = "%%EOF";
  int look = n < 1024 ? static_cast<int>(n) : 1024;
  const unsigned char* buf = d + n - look;
  int8_t size = -1;
  int m = 4;
  for (int at = look - 1; at >= 0; --at) {
    if (buf[at] == static_cast<unsigned char>(marker[m])) {
      if (m == 0) {
        size = static_cast<int8_t>(look - at);
        size = static_cast<int8_t>(size - 5);
        if (size > 0) {
          if (buf[at + 5] == 0x0D) {
            size = static_cast<int8_t>(size - (at + 6 < look && buf[at + 6] == 0x0A ? 2 : 1));
          } else if (buf[at + 5] == 0x0A) {
            size = static_cast<int8_t>(size - 1);
          }
        }
        return size;
      }
      --m;
    } else if (m < 4) {
      m = 4;
    }
  }
  return size;
}

int printSpaceComponents(QPDFObjectHandle cs, int depth, bool& bad);

bool printFunctionBad(QPDFObjectHandle fn, int inputs, int outputs, int depth) {
  if (depth > 8) return false;
  QPDFObjectHandle d = fn.isStream() ? fn.getDict() : fn;
  if (!d.isDictionary()) return true;
  QPDFObjectHandle type = d.getKey("/FunctionType");
  if (!type.isInteger()) return true;
  long long t = type.getIntValue();
  if (t != 0 && t != 2 && t != 3 && t != 4) return true;
  if ((t == 0 || t == 4) && !fn.isStream()) return true;
  QPDFObjectHandle dom = d.getKey("/Domain");
  if (!dom.isArray() || dom.getArrayNItems() < 2 || dom.getArrayNItems() % 2) return true;
  for (int i = 0; i + 1 < dom.getArrayNItems(); i += 2) {
    if (!dom.getArrayItem(i).isNumber() || !dom.getArrayItem(i + 1).isNumber()) return true;
    if (dom.getArrayItem(i).getNumericValue() > dom.getArrayItem(i + 1).getNumericValue()) return true;
  }
  int m = dom.getArrayNItems() / 2;
  if (inputs > 0 && m != inputs) return true;
  QPDFObjectHandle range = d.getKey("/Range");
  int n = -1;
  if (range.isArray()) {
    if (range.getArrayNItems() < 2 || range.getArrayNItems() % 2) return true;
    n = range.getArrayNItems() / 2;
  } else if (!range.isNull() || t == 0 || t == 4) {
    return true;
  }
  if (t == 0) {
    QPDFObjectHandle size = d.getKey("/Size");
    if (!size.isArray() || size.getArrayNItems() != m || !d.getKey("/BitsPerSample").isInteger()) return true;
  } else if (t == 2) {
    QPDFObjectHandle c0 = d.getKey("/C0"), c1 = d.getKey("/C1");
    int l0 = c0.isArray() ? c0.getArrayNItems() : c0.isNull() ? 1 : -1;
    int l1 = c1.isArray() ? c1.getArrayNItems() : c1.isNull() ? 1 : -1;
    if (l0 < 0 || l1 < 0 || l0 != l1 || !d.getKey("/N").isNumber() || m != 1) return true;
    if (n < 0) n = l0;
    else if (n != l0) return true;
  } else if (t == 3) {
    QPDFObjectHandle fs = d.getKey("/Functions"), bounds = d.getKey("/Bounds"), enc = d.getKey("/Encode");
    if (!fs.isArray() || fs.getArrayNItems() < 1 || m != 1) return true;
    int k = fs.getArrayNItems();
    if (!bounds.isArray() || bounds.getArrayNItems() != k - 1) return true;
    if (!enc.isArray() || enc.getArrayNItems() != 2 * k) return true;
    for (int i = 0; i < k; ++i) {
      if (printFunctionBad(fs.getArrayItem(i), 1, n, depth + 1)) return true;
    }
  }
  return outputs > 0 && n > 0 && n != outputs;
}

int printSpaceComponents(QPDFObjectHandle cs, int depth, bool& bad) {
  if (depth > kMaxColorSpaceNest) return 0;
  if (cs.isName()) {
    std::string n = cs.getName();
    if (n == "/DeviceGray" || n == "/G" || n == "/CalGray") return 1;
    if (n == "/DeviceRGB" || n == "/RGB") return 3;
    if (n == "/DeviceCMYK" || n == "/CMYK") return 4;
    if (n == "/Pattern") return 0;
    bad = true;
    return 0;
  }
  if (!cs.isArray() || cs.getArrayNItems() < 1) {
    bad = true;
    return 0;
  }
  std::string f = nameOf(cs.getArrayItem(0));
  int items = cs.getArrayNItems();
  if (f == "/DeviceGray" || f == "/DeviceRGB" || f == "/DeviceCMYK" || f == "/Pattern") {
    if (f == "/Pattern" && items >= 2) printSpaceComponents(cs.getArrayItem(1), depth + 1, bad);
    return f == "/DeviceGray" ? 1 : f == "/DeviceRGB" ? 3 : f == "/DeviceCMYK" ? 4 : 0;
  }
  if (f == "/CalGray" || f == "/CalRGB" || f == "/Lab") {
    if (items < 2 || !cs.getArrayItem(1).isDictionary()) bad = true;
    return f == "/CalGray" ? 1 : 3;
  }
  if (f == "/ICCBased") {
    if (items < 2 || !cs.getArrayItem(1).isStream()) {
      bad = true;
      return 0;
    }
    QPDFObjectHandle n = cs.getArrayItem(1).getDict().getKey("/N");
    if (!n.isInteger() || (n.getIntValue() != 1 && n.getIntValue() != 3 && n.getIntValue() != 4)) bad = true;
    return n.isInteger() ? static_cast<int>(n.getIntValue()) : 0;
  }
  auto special = [](QPDFObjectHandle a) {
    std::string af = a.isArray() && a.getArrayNItems() > 0 ? nameOf(a.getArrayItem(0)) : nameOf(a);
    return af == "/Separation" || af == "/DeviceN" || af == "/Indexed" || af == "/Pattern" || af == "/I";
  };
  if (f == "/Indexed" || f == "/I") {
    if (items != 4 || special(cs.getArrayItem(1)) || !cs.getArrayItem(2).isInteger()) {
      bad = true;
      return 1;
    }
    int base = printSpaceComponents(cs.getArrayItem(1), depth + 1, bad);
    long long hival = cs.getArrayItem(2).getIntValue();
    QPDFObjectHandle lookup = cs.getArrayItem(3);
    size_t size = 0;
    if (lookup.isString()) {
      size = lookup.getStringValue().size();
    } else if (!lookup.isStream()) {
      bad = true;
    }
    if (hival < 0 || hival > 255 || (base > 0 && size > static_cast<size_t>((hival + 1) * base))) bad = true;
    return 1;
  }
  if (f == "/Separation" || f == "/DeviceN") {
    bool sep = f == "/Separation";
    if (items < 4 || (sep && !cs.getArrayItem(1).isName()) || (!sep && !cs.getArrayItem(1).isArray()) ||
        special(cs.getArrayItem(2))) {
      bad = true;
      return sep ? 1 : 0;
    }
    int inputs = sep ? 1 : cs.getArrayItem(1).getArrayNItems();
    int outs = printSpaceComponents(cs.getArrayItem(2), depth + 1, bad);
    if (printFunctionBad(cs.getArrayItem(3), inputs, outs, 0)) bad = true;
    return inputs;
  }
  bad = true;
  return 0;
}

std::vector<std::string> printStructureProblems(Ctx& ctx) {
  std::vector<std::string> out;
  int badSpaces = 0, badImages = 0, badAlternates = 0, badXObjects = 0, badForms = 0, badNames = 0,
      badStruct = 0, solidity = 0;
  std::map<std::string, double> solid;
  std::set<std::string> checkedSpaces;
  auto checkSpace = [&](QPDFObjectHandle cs) {
    std::string key = cs.isIndirect() ? cs.getObjGen().unparse() : cs.unparse();
    if (key.size() > 4096 || !checkedSpaces.insert(key).second) return;
    bool bad = false;
    printSpaceComponents(cs, 0, bad);
    if (bad) ++badSpaces;
    if (cs.isArray() && cs.getArrayNItems() >= 5 && nameIs(cs.getArrayItem(0), "/DeviceN") &&
        cs.getArrayItem(4).isDictionary()) {
      QPDFObjectHandle mh = cs.getArrayItem(4).getKey("/MixingHints");
      QPDFObjectHandle so = mh.isDictionary() ? mh.getKey("/Solidities") : QPDFObjectHandle::newNull();
      for (const std::string& k : so.isDictionary() ? so.getKeys() : std::set<std::string>()) {
        if (!so.getKey(k).isNumber()) continue;
        double v = so.getKey(k).getNumericValue();
        auto it = solid.find(k);
        if (it == solid.end()) solid[k] = v;
        else if (std::fabs(it->second - v) > 1e-6) ++solidity;
      }
    }
  };
  const bool x6 = ctx.pdf20Print();
  Visited seen;
  std::function<void(QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle o, int depth) {
    if (depth > kMaxObjectWalk || !seen.enter(o)) return;
    if (x6 && o.isName() && !validUtf8(o.getName().substr(1))) ++badNames;
    if (o.isArray()) {
      for (int i = 0; i < o.getArrayNItems(); ++i) walk(o.getArrayItem(i), depth + 1);
      return;
    }
    if (!o.isDictionary() && !o.isStream()) return;
    QPDFObjectHandle d = o.isStream() ? o.getDict() : o;
    std::string subtype = nameOf(d.getKey("/Subtype"));
    if (o.isStream() && subtype == "/Image") {
      bool mask = d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue();
      QPDFObjectHandle filt = d.getKey("/Filter");
      std::vector<std::string> filters;
      if (filt.isName()) filters.push_back(filt.getName());
      for (int i = 0; filt.isArray() && i < filt.getArrayNItems(); ++i) filters.push_back(nameOf(filt.getArrayItem(i)));
      bool jpx = std::find(filters.begin(), filters.end(), "/JPXDecode") != filters.end();
      QPDFObjectHandle cs = d.getKey("/ColorSpace");
      int comps = 0;
      bool bad = false;
      if (mask && !cs.isNull()) bad = true;
      if (!mask && cs.isNull() && !jpx) bad = true;
      if (!cs.isNull()) {
        bool csBad = false;
        comps = printSpaceComponents(cs, 0, csBad);
        bad = bad || csBad;
      }
      if (mask) comps = 1;
      bool plain = true;
      for (const std::string& f : filters) {
        static const std::set<std::string> generic = {"/FlateDecode", "/Fl", "/LZWDecode", "/LZW", "/ASCIIHexDecode",
                                                      "/AHx", "/ASCII85Decode", "/A85", "/RunLengthDecode", "/RL"};
        if (!generic.count(f)) plain = false;
      }
      QPDFObjectHandle w = d.getKey("/Width"), h = d.getKey("/Height"), bpc = d.getKey("/BitsPerComponent");
      long long bits = mask ? 1 : bpc.isInteger() ? bpc.getIntValue() : 0;
      if (!bad && plain && comps > 0 && bits > 0 && w.isInteger() && h.isInteger() && w.getIntValue() > 0 &&
          h.getIntValue() > 0 && w.getIntValue() * h.getIntValue() <= kMaxImagePixels) {
        long long need = (w.getIntValue() * comps * bits + 7) / 8 * h.getIntValue();
        try {
          if (static_cast<long long>(o.getStreamData(qpdf_dl_generalized)->getSize()) < need) bad = true;
        } catch (...) {
          bad = true;
        }
      }
      if (bad) ++badImages;
      QPDFObjectHandle alts = d.getKey("/Alternates");
      for (int i = 0; alts.isArray() && i < alts.getArrayNItems(); ++i) {
        QPDFObjectHandle ad = alts.getArrayItem(i);
        QPDFObjectHandle ai = ad.isDictionary() ? ad.getKey("/Image") : QPDFObjectHandle::newNull();
        if (!ai.isStream()) {
          ++badAlternates;
          continue;
        }
        QPDFObjectHandle aid = ai.getDict();
        QPDFObjectHandle aw = aid.getKey("/Width"), ah = aid.getKey("/Height");
        if (w.isInteger() && h.isInteger() && aw.isInteger() && ah.isInteger() &&
            w.getIntValue() * ah.getIntValue() != aw.getIntValue() * h.getIntValue())
          ++badAlternates;
        else if (x6 && aid.hasKey("/OC"))
          ++badAlternates;
        else if (ctx.pdf14Target() &&
                 (aid.getKey("/Filter").unparse().find("/JBIG2Decode") != std::string::npos ||
                  aid.getKey("/Filter").unparse().find("/JPXDecode") != std::string::npos))
          ++badAlternates;
      }
    }
    if (x6 && nameIs(d.getKey("/Type"), "/StructElem") &&
        (!d.getKey("/S").isName() || (d.hasKey("/Pg") && !d.getKey("/Pg").isDictionary())))
      ++badStruct;
    QPDFObjectHandle csd = d.getKey("/ColorSpace");
    if (csd.isDictionary() && !o.isStream() && !d.hasKey("/ShadingType")) {
      for (const std::string& k : csd.getKeys()) checkSpace(csd.getKey(k));
    } else if (!csd.isNull() && (d.hasKey("/ShadingType"))) {
      checkSpace(csd);
    }
    QPDFObjectHandle xod = d.getKey("/XObject");
    if (xod.isDictionary() && !o.isStream()) {
      for (const std::string& k : xod.getKeys()) {
        QPDFObjectHandle x = xod.getKey(k);
        std::string st = x.isStream() ? nameOf(x.getDict().getKey("/Subtype")) : std::string();
        if (st != "/Form" && st != "/Image" && st != "/PS") ++badXObjects;
      }
    }
    for (const std::string& k : d.getKeys()) {
      if (x6 && !validUtf8(k.substr(1))) ++badNames;
      if (k == "/Parent" || k == "/P" || k == "/Pg") continue;
      walk(d.getKey(k), depth + 1);
    }
  };
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) walk(obj, 0);
  int badBoxes = 0;
  for (auto& ph : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) {
    QPDFObjectHandle mb = ph.getAttribute("/MediaBox", false);
    bool ok = mb.isArray() && mb.getArrayNItems() == 4;
    for (int i = 0; ok && i < 4; ++i) ok = mb.getArrayItem(i).isNumber();
    if (!ok) ++badBoxes;
    QPDFObjectHandle annots = ph.getObjectHandle().getKey("/Annots");
    for (int i = 0; x6 && annots.isArray() && i < annots.getArrayNItems(); ++i) {
      if (!annots.getArrayItem(i).isDictionary()) continue;
      for (QPDFObjectHandle st : normalAppearanceStreams(annots.getArrayItem(i))) {
        if (!st.getDict().getKey("/Resources").isDictionary()) ++badForms;
      }
    }
  }
  int badOutlines = 0;
  QPDFObjectHandle outlines = ctx.pdf.getRoot().getKey("/Outlines");
  std::set<QPDFObjGen> visitedItems;
  std::vector<QPDFObjectHandle> stack;
  if (outlines.isDictionary()) stack.push_back(outlines.getKey("/First"));
  while (!stack.empty() && visitedItems.size() < 1000000) {
    QPDFObjectHandle item = stack.back();
    stack.pop_back();
    while (item.isDictionary()) {
      if (item.isIndirect() && !visitedItems.insert(item.getObjGen()).second) {
        ++badOutlines;
        break;
      }
      if (!item.getKey("/Title").isString()) ++badOutlines;
      if (item.getKey("/First").isDictionary()) stack.push_back(item.getKey("/First"));
      item = item.getKey("/Next");
    }
  }
  if (badSpaces) out.push_back(std::to_string(badSpaces) + " colour space(s) or tint transform(s) are malformed");
  if (solidity) out.push_back("DeviceN mixing hints give the same colorant different solidity values");
  if (badImages) out.push_back(std::to_string(badImages) + " image(s) have a missing or wrong colour space or too little sample data");
  if (badAlternates) out.push_back(std::to_string(badAlternates) + " alternate image entr(y/ies) are unusable for print");
  if (badXObjects) out.push_back(std::to_string(badXObjects) + " XObject resource(s) are not form or image streams");
  if (badForms) out.push_back(std::to_string(badForms) + " annotation appearance(s) rely on resources inherited from the page");
  if (badNames) out.push_back(std::to_string(badNames) + " name object(s) are not valid UTF-8");
  if (badStruct) out.push_back(std::to_string(badStruct) + " structure element(s) are malformed");
  if (badBoxes) out.push_back(std::to_string(badBoxes) + " page(s) have no usable MediaBox");
  if (badOutlines) out.push_back("the outline tree has items without a title or loops back on itself");
  return out;
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
  tWholeLargeReals = ctx.isX() || ctx.isE();
  bool pdf14 = ctx.pdf14Target();
  bool limits23 = wantLimits23(ctx);
  int contentViolations = 0;
  std::set<QPDFObjGen> reached;
  collectFontUse(ctx.pdf, &reached);
  tViolations = &contentViolations;
  tReached = &reached;
  tPart = ctx.isA() ? ctx.part : 0;
  tStrictX = ctx.isX();
  tLaxIntent = ctx.isE();
  Visited streamVisited;
  for (auto& ph : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    ContentFixFilter f(pdf14, limits23, wantPuaScrub(ctx));
    f.setPatterns(ph.getAttribute("/Resources", false));
    Pl_Discard discard;
    try {
      ph.filterContents(&f, &discard);
    } catch (...) {
    }
    if (ctx.part >= 2 && f.usedNamed && !page.getKey("/Resources").isDictionary()) violation();
    if (ctx.isX() && f.usedNamed && !ph.getAttribute("/Resources", false).isDictionary()) violation();
    if (ctx.pdf20Print() && !page.getKey("/Resources").isDictionary()) violation();
    attachToContentStreams(ctx, page.getKey("/Resources"), streamVisited);
    QPDFObjectHandle annots = page.getKey("/Annots");
    for (int i = 0; annots.isArray() && i < annots.getArrayNItems(); ++i) {
      QPDFObjectHandle a = annots.getArrayItem(i);
      if (!a.isDictionary()) continue;
      for (QPDFObjectHandle st : normalAppearanceStreams(a)) {
        if (streamVisited.enter(st)) {
          attachFilter(ctx, st, pdf14, limits23, st.getDict().getKey("/Resources"));
          attachToContentStreams(ctx, st.getDict().getKey("/Resources"), streamVisited);
        }
      }
    }
  }
  tViolations = nullptr;
  tReached = nullptr;
  tPart = 0;
  tStrictX = false;
  tLaxIntent = false;
  if (contentViolations) {
    ctx.issue("CONTENT_NONCONFORMING",
              std::to_string(contentViolations) +
                  " content stream token(s) break content syntax rules or implementation limits",
              true);
  }
}

void checkFileSyntax(Ctx& ctx, const unsigned char* d, size_t n, bool encrypted) {
  verifyContentStreams(ctx);
  std::vector<std::string> problems;
  auto add = [&](const std::string& s) {
    if (!s.empty()) problems.push_back(s);
  };
  const bool archive = ctx.isA();
  if (archive) add(headerProblem(d, n, ctx.part));
  if (!archive && n > 8 && std::memcmp(d, "%PDF-", 5) == 0) {
    if (ctx.pdf20Print() && d[5] != '2') add("the file header does not declare PDF 2.0");
    if (ctx.isE() && d[5] != '1') add("the file header declares a PDF version newer than PDF/E-1 allows");
  }
  if (encrypted) add("the file is encrypted");
  QPDFObjectHandle id = ctx.pdf.getTrailer().getKey("/ID");
  std::vector<std::pair<size_t, size_t>> trailers = trailerSpans(d, n);
  if (archive) {
    std::vector<QPDFObjectHandle> raw;
    for (const auto& t : trailers) {
      try {
        raw.push_back(QPDFObjectHandle::parse(&ctx.pdf, std::string(reinterpret_cast<const char*>(d) + t.first, t.second - t.first)));
      } catch (...) {
        raw.push_back(QPDFObjectHandle::newNull());
      }
    }
    bool linearized = false;
    try {
      linearized = raw.size() >= 2 && ctx.pdf.isLinearized();
    } catch (...) {
    }
    std::string firstId, lastId;
    bool haveFirst = linearized && trailerId(raw.front(), firstId);
    bool haveLast = linearized ? trailerId(raw.back(), lastId) : trailerId(ctx.pdf.getTrailer(), lastId);
    long long indirects = 0;
    try {
      for (const auto& kv : ctx.pdf.getXRefTable()) indirects += kv.second.getType() != 0;
    } catch (...) {
    }
    if (!linearized && !haveLast) {
      QPDFObjectHandle xs = lastXRefStream(ctx.pdf, d, n);
      if (xs.isStream()) {
        haveLast = trailerId(xs.getDict(), lastId);
        indirects = std::max(indirects, xrefStreamInUse(xs));
      }
    }
    if (ctx.part <= 3 && indirects > 8388607) add("the file holds more than 8388607 indirect objects");
    if (ctx.part == 1) {
      if (!(linearized ? haveFirst : haveLast)) add("the trailer has no /ID file identifier");
      else if (linearized && haveLast && firstId != lastId)
        add("the first-page trailer /ID differs from the last trailer /ID");
    } else {
      std::string& used = linearized ? firstId : lastId;
      if (!(linearized ? haveFirst : haveLast) || used.empty()) add("the trailer has no valid /ID file identifier");
    }
    if (postEofSize(d, n) != 0) add("data follows the last end-of-file marker");
  } else if (!id.isArray() || id.getArrayNItems() != 2 || !id.getArrayItem(0).isString() || !id.getArrayItem(1).isString() ||
             id.getArrayItem(0).getStringValue().empty() || id.getArrayItem(1).getStringValue().empty()) {
    add("the trailer has no valid /ID file identifier");
  }
  if (archive || ctx.pdf20Print()) add(xrefProblem(d, n, archive ? ctx.part : 4));
  if (ctx.isX()) {
    for (const std::string& p : printStructureProblems(ctx)) add(p);
  }
  if (archive && !encrypted) {
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
          bool kwOk = (archive && ctx.part >= 4) || (s < n && (d[s] == '\n' || (d[s] == '\r' && s + 1 < n && d[s + 1] == '\n')));
          s += s < n && d[s] == '\r' ? (s + 1 < n && d[s + 1] == '\n' ? 2 : 1) : (s < n && d[s] == '\n' ? 1 : 0);
          QPDFObjectHandle obj = ctx.pdf.getObjectByObjGen(kv.first);
          QPDFObjectHandle len = obj.isStream() ? obj.getDict().getKey("/Length") : QPDFObjectHandle::newNull();
          bool lenOk = false;
          if (len.isInteger() && len.getIntValue() >= 0 && s + static_cast<size_t>(len.getIntValue()) <= n) {
            size_t e = s + static_cast<size_t>(len.getIntValue());
            size_t el = eolLen(d, n, e);
            lenOk = e + el + 9 <= n && std::memcmp(d + e + el, "endstream", 9) == 0 && (el > 0 || ctx.part >= 4);
          }
          if (!kwOk || !lenOk) ++badStream;
          const unsigned char* es = findBytes(d + s, n - s, "endstream");
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
    for (const auto& t : trailers) {
      if (hexStringsBad(d, t.first, t.second)) ++badHex;
    }
    std::set<int> objStreams;
    for (const auto& kv : xref) {
      if (kv.second.getType() == 2) objStreams.insert(kv.second.getObjStreamNumber());
    }
    for (int os : objStreams) {
      try {
        auto buf = ctx.pdf.getObjectByID(os, 0).getStreamData(qpdf_dl_generalized);
        if (hexStringsBad(buf->getBuffer(), 0, buf->getSize())) ++badHex;
      } catch (...) {
      }
    }
    if (badObj) add(std::to_string(badObj) + " indirect object header(s) or obj/endobj keywords are not delimited as required");
    if (badStream) add(std::to_string(badStream) + " stream(s) with a wrong /Length or stream keyword line ending");
    if (badHex) add(std::to_string(badHex) + " object(s) containing malformed hexadecimal strings");
  }
  int big = 0;
  std::vector<QPDFObjectHandle> todo;
  for (QPDFObjectHandle obj : ctx.pdf14Target() ? ctx.pdf.getAllObjects() : std::vector<QPDFObjectHandle>()) {
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
  if (big) add(std::to_string(big) + " dictionary or array object(s) exceed the PDF 1.4 size limits");
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
          (((dict.hasKey("/PS") || nameIs(dict.getKey("/Subtype2"), "/PS")) && !(archive && ctx.part >= 4)) ||
           ((ctx.part >= 2 || !archive) && dict.hasKey("/OPI"))))
        ++ps;
    }
    if ((ctx.part >= 4 || (!archive && !ctx.pdf14Target())) && nameIs(dict.getKey("/Type"), "/Page") &&
        dict.hasKey("/PresSteps"))
      ++pres;
  }
  bool xrefStream = false;
  try {
    for (const auto& kv : ctx.pdf.getXRefTable()) xrefStream = xrefStream || kv.second.getType() == 2;
  } catch (...) {
  }
  xrefStream = xrefStream || ctx.pdf.getTrailer().hasKey("/XRefStm");
  for (const unsigned char* p = findBytes(d, n, "startxref"); p && !xrefStream;
       p = findBytes(p + 9, n - static_cast<size_t>(p + 9 - d), "startxref")) {
    long long off = std::strtoll(std::string(reinterpret_cast<const char*>(p) + 9,
                                             std::min<size_t>(32, n - static_cast<size_t>(p + 9 - d))).c_str(),
                                 nullptr, 10);
    size_t at = static_cast<size_t>(off);
    while (off > 0 && at < n && pdfWs(d[at])) ++at;
    if (off > 0 && at + 4 <= n && std::memcmp(d + at, "xref", 4) != 0 && std::isdigit(d[at])) xrefStream = true;
  }
  if (xrefStream && ctx.pdf14Target()) add("cross-reference streams or object streams are used");
  if (lzw) add("LZWDecode filter used by " + std::to_string(lzw) + " stream(s)");
  if (ps) add(std::to_string(ps) + " form XObject(s) carry PostScript or OPI keys");
  if (pres) add(std::to_string(pres) + " page(s) carry a /PresSteps entry");
  for (const std::string& p : problems) ctx.issue("FILE_STRUCTURE_NONCONFORMING", p, true);
}
}
