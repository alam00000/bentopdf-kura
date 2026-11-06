#include "textextract.hh"

#include <qpdf/Pl_Discard.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>

#include "assets/agl_names.hh"
#include "encodings.hh"
#include "limits.hh"
#include "util.hh"

namespace pdfa {
std::string utf8Of(uint32_t cp) {
  std::string o;
  if (cp < 0x80) {
    o += static_cast<char>(cp);
  } else if (cp < 0x800) {
    o += static_cast<char>(0xC0 | (cp >> 6));
    o += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    o += static_cast<char>(0xE0 | (cp >> 12));
    o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    o += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x110000) {
    o += static_cast<char>(0xF0 | (cp >> 18));
    o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    o += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return o;
}

namespace {
struct Mat {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

Mat mul(const Mat& m, const Mat& n) {
  return {m.a * n.a + m.b * n.c,       m.a * n.b + m.b * n.d,       m.c * n.a + m.d * n.c,
          m.c * n.b + m.d * n.d,       m.e * n.a + m.f * n.c + n.e, m.e * n.b + m.f * n.d + n.f};
}

std::string utf16beToUtf8(const std::string& hex) {
  std::string bytes;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    bytes += static_cast<char>(std::strtol(hex.substr(i, 2).c_str(), nullptr, 16));
  }
  std::string out;
  for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
    uint32_t u = (static_cast<unsigned char>(bytes[i]) << 8) | static_cast<unsigned char>(bytes[i + 1]);
    if (u >= 0xD800 && u < 0xDC00 && i + 3 < bytes.size()) {
      uint32_t l = (static_cast<unsigned char>(bytes[i + 2]) << 8) | static_cast<unsigned char>(bytes[i + 3]);
      u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00);
      i += 2;
    }
    out += utf8Of(u);
  }
  return out;
}

struct FontInfo {
  bool twoByte = false;
  bool type3 = false;
  double glyphScale = 0.001;
  double dw = 1000;
  std::map<uint32_t, std::string> uni;
  std::map<uint32_t, double> widths;
  std::string name;

  std::string text(uint32_t code) const {
    auto it = uni.find(code);
    if (it != uni.end()) return it->second;
    return std::string();
  }
  double width(uint32_t code) const {
    auto it = widths.find(code);
    return (it != widths.end() ? it->second : dw) * glyphScale;
  }
};

void parseToUnicode(QPDFObjectHandle tu, FontInfo& fi) {
  if (!tu.isStream()) return;
  std::string t;
  try {
    auto b = tu.getStreamData(qpdf_dl_all);
    t.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
  } catch (...) {
    return;
  }
  auto hexes = [&](size_t from, size_t to) {
    std::vector<std::pair<size_t, std::string>> v;
    size_t p = from;
    while (p < to) {
      size_t a = t.find('<', p);
      if (a == std::string::npos || a >= to) break;
      size_t b = t.find('>', a);
      if (b == std::string::npos || b > to) break;
      std::string h;
      for (size_t i = a + 1; i < b; ++i) {
        if (std::isxdigit(static_cast<unsigned char>(t[i]))) h += t[i];
      }
      v.push_back({a, h});
      p = b + 1;
    }
    return v;
  };
  size_t pos = 0;
  while ((pos = t.find("beginbfchar", pos)) != std::string::npos) {
    size_t end = t.find("endbfchar", pos);
    if (end == std::string::npos) break;
    auto v = hexes(pos, end);
    for (size_t i = 0; i + 1 < v.size(); i += 2) {
      uint32_t code = static_cast<uint32_t>(std::strtoul(v[i].second.c_str(), nullptr, 16));
      fi.uni[code] = utf16beToUtf8(v[i + 1].second);
    }
    pos = end;
  }
  pos = 0;
  while ((pos = t.find("beginbfrange", pos)) != std::string::npos) {
    size_t end = t.find("endbfrange", pos);
    if (end == std::string::npos) break;
    size_t p = pos + 12;
    while (p < end) {
      auto two = hexes(p, end);
      if (two.size() < 2) break;
      uint32_t lo = static_cast<uint32_t>(std::strtoul(two[0].second.c_str(), nullptr, 16));
      uint32_t hi = static_cast<uint32_t>(std::strtoul(two[1].second.c_str(), nullptr, 16));
      size_t after = t.find('>', two[1].first) + 1;
      size_t q = after;
      while (q < end && std::isspace(static_cast<unsigned char>(t[q]))) ++q;
      if (hi < lo || hi - lo > 65535) break;
      if (q < end && t[q] == '[') {
        size_t close = t.find(']', q);
        if (close == std::string::npos || close > end) break;
        auto arr = hexes(q, close);
        for (uint32_t c = lo; c <= hi && c - lo < arr.size(); ++c) fi.uni[c] = utf16beToUtf8(arr[c - lo].second);
        p = close + 1;
      } else {
        auto dst = hexes(q, end);
        if (dst.empty()) break;
        std::string base = dst[0].second;
        for (uint32_t c = lo; c <= hi; ++c) {
          std::string h = base;
          unsigned long last = std::strtoul(h.substr(h.size() >= 4 ? h.size() - 4 : 0).c_str(), nullptr, 16) + (c - lo);
          char buf[8];
          std::snprintf(buf, sizeof(buf), "%04lX", last & 0xFFFF);
          h = h.substr(0, h.size() >= 4 ? h.size() - 4 : 0) + buf;
          fi.uni[c] = utf16beToUtf8(h);
        }
        p = t.find('>', dst[0].first) + 1;
      }
    }
    pos = end;
  }
}

std::shared_ptr<FontInfo> loadFont(QPDFObjectHandle f) {
  auto fi = std::make_shared<FontInfo>();
  if (!f.isDictionary()) return fi;
  std::string sub = nameOf(f.getKey("/Subtype"));
  fi->name = nameOf(f.getKey("/BaseFont"));
  if (sub == "/Type0") {
    fi->twoByte = true;
    QPDFObjectHandle df = f.getKey("/DescendantFonts");
    QPDFObjectHandle cid = df.isArray() && df.getArrayNItems() ? df.getArrayItem(0) : QPDFObjectHandle();
    if (cid.isDictionary()) {
      fi->dw = numOf(cid.getKey("/DW"), 1000);
      QPDFObjectHandle w = cid.getKey("/W");
      for (int i = 0; w.isArray() && i < w.getArrayNItems();) {
        QPDFObjectHandle a = w.getArrayItem(i);
        if (i + 1 < w.getArrayNItems() && w.getArrayItem(i + 1).isArray()) {
          QPDFObjectHandle ws = w.getArrayItem(i + 1);
          uint32_t c0 = static_cast<uint32_t>(numOf(a, 0));
          for (int k = 0; k < ws.getArrayNItems() && k < 65536; ++k) fi->widths[c0 + k] = numOf(ws.getArrayItem(k), fi->dw);
          i += 2;
        } else if (i + 2 < w.getArrayNItems()) {
          uint32_t c0 = static_cast<uint32_t>(numOf(a, 0)), c1 = static_cast<uint32_t>(numOf(w.getArrayItem(i + 1), 0));
          double v = numOf(w.getArrayItem(i + 2), fi->dw);
          for (uint32_t c = c0; c <= c1 && c - c0 < 65536; ++c) fi->widths[c] = v;
          i += 3;
        } else {
          break;
        }
      }
    }
  } else {
    fi->dw = numOf(f.getKey("/FontDescriptor").isDictionary() ? f.getKey("/FontDescriptor").getKey("/MissingWidth") : QPDFObjectHandle(), 500);
    int first = static_cast<int>(numOf(f.getKey("/FirstChar"), 0));
    QPDFObjectHandle w = f.getKey("/Widths");
    for (int i = 0; w.isArray() && i < w.getArrayNItems(); ++i) fi->widths[first + i] = numOf(w.getArrayItem(i), 0);
    if (sub == "/Type3") {
      fi->type3 = true;
      QPDFObjectHandle fm = f.getKey("/FontMatrix");
      fi->glyphScale = fm.isArray() && fm.getArrayNItems() >= 1 ? numOf(fm.getArrayItem(0), 0.001) : 0.001;
    }
    std::string lowerName = lowerAscii(fi->name);
    bool symbol = lowerName.find("symbol") != std::string::npos;
    bool dingbat = lowerName.find("dingbat") != std::string::npos;
    QPDFObjectHandle enc = f.getKey("/Encoding");
    std::string base = enc.isName() ? enc.getName() : enc.isDictionary() ? nameOf(enc.getKey("/BaseEncoding")) : "";
    for (int c = 0; c < 256; ++c) {
      uint16_t u = symbol ? symbolToUnicode(c) : dingbat ? zapfDingbatsToUnicode(c)
                   : base == "/MacRomanEncoding" ? macRomanToUnicode(c)
                   : base == "/StandardEncoding" ? standardToUnicode(c) : winAnsiToUnicode(c);
      if (u) fi->uni[c] = utf8Of(u);
    }
    QPDFObjectHandle diffs = enc.isDictionary() ? enc.getKey("/Differences") : QPDFObjectHandle();
    int code = 0;
    for (int i = 0; diffs.isArray() && i < diffs.getArrayNItems(); ++i) {
      QPDFObjectHandle it = diffs.getArrayItem(i);
      if (it.isNumber()) {
        code = static_cast<int>(it.getNumericValue());
      } else if (it.isName()) {
        uint32_t u = aglNameToUnicode(it.getName().substr(1));
        if (u && code >= 0 && code < 256) fi->uni[code] = utf8Of(u);
        ++code;
      }
    }
  }
  parseToUnicode(f.getKey("/ToUnicode"), *fi);
  return fi;
}

class Walker : public QPDFObjectHandle::TokenFilter {
 public:
  Walker(QPDFObjectHandle res, Mat base, bool inForm, std::vector<TextRun>& out,
         std::map<QPDFObjGen, std::shared_ptr<FontInfo>>& fonts, int depth)
      : res(res), out(out), fonts(fonts), inForm(inForm), depth(depth) {
    gs.ctm = base;
  }

  void handleToken(QPDFTokenizer::Token const& tok) override {
    auto t = tok.getType();
    if (t != QPDFTokenizer::tt_word) {
      if (t == QPDFTokenizer::tt_inline_image) return;
      if (t != QPDFTokenizer::tt_space && t != QPDFTokenizer::tt_comment) operands.push_back(tok);
      return;
    }
    ++opIndex;
    std::string op = tok.getValue();
    if (op == "q") {
      stack.push_back(gs);
    } else if (op == "Q") {
      if (!stack.empty()) {
        gs = stack.back();
        stack.pop_back();
      }
    } else if (op == "cm" && nums(6)) {
      gs.ctm = mul({n[0], n[1], n[2], n[3], n[4], n[5]}, gs.ctm);
    } else if (op == "BT") {
      tm = tlm = Mat();
    } else if (op == "Tf" && operands.size() >= 2) {
      gs.size = std::strtod(operands.back().getValue().c_str(), nullptr);
      std::string nm = operands[operands.size() - 2].getValue();
      QPDFObjectHandle fd = res.isDictionary() ? res.getKey("/Font") : QPDFObjectHandle();
      QPDFObjectHandle f = fd.isDictionary() ? fd.getKey(nm) : QPDFObjectHandle();
      gs.font = nullptr;
      if (f.isDictionary()) {
        if (f.isIndirect()) {
          auto it = fonts.find(f.getObjGen());
          if (it == fonts.end()) it = fonts.emplace(f.getObjGen(), loadFont(f)).first;
          gs.font = it->second;
        } else {
          gs.font = loadFont(f);
        }
      }
    } else if (op == "Tc" && nums(1)) {
      gs.tc = n[0];
    } else if (op == "Tw" && nums(1)) {
      gs.tw = n[0];
    } else if (op == "Tz" && nums(1)) {
      gs.th = n[0] / 100.0;
    } else if (op == "TL" && nums(1)) {
      gs.tl = n[0];
    } else if (op == "Ts" && nums(1)) {
      gs.rise = n[0];
    } else if (op == "Td" && nums(2)) {
      tlm = mul({1, 0, 0, 1, n[0], n[1]}, tlm);
      tm = tlm;
    } else if (op == "TD" && nums(2)) {
      gs.tl = -n[1];
      tlm = mul({1, 0, 0, 1, n[0], n[1]}, tlm);
      tm = tlm;
    } else if (op == "Tm" && nums(6)) {
      tlm = tm = {n[0], n[1], n[2], n[3], n[4], n[5]};
    } else if (op == "T*") {
      nextLine();
    } else if (op == "Tj" || op == "'" || op == "\"" || op == "TJ") {
      if (op == "'" || op == "\"") nextLine();
      if (op == "\"" && operands.size() >= 3) {
        gs.tw = std::strtod(operands[0].getValue().c_str(), nullptr);
        gs.tc = std::strtod(operands[1].getValue().c_str(), nullptr);
      }
      show(op == "TJ");
    } else if (op == "BDC" || op == "BMC") {
      Mark m;
      if (!operands.empty() && operands[0].getType() == QPDFTokenizer::tt_name) {
        m.artifact = operands[0].getValue() == "/Artifact";
      }
      for (size_t i = 0; i + 1 < operands.size(); ++i) {
        if (operands[i].getValue() == "/MCID") m.mcid = std::atoi(operands[i + 1].getValue().c_str());
      }
      if (op == "BDC" && operands.size() >= 2 && operands.back().getType() == QPDFTokenizer::tt_name) {
        QPDFObjectHandle props = res.isDictionary() ? res.getKey("/Properties") : QPDFObjectHandle();
        QPDFObjectHandle pd = props.isDictionary() ? props.getKey(operands.back().getValue()) : QPDFObjectHandle();
        if (pd.isDictionary() && pd.getKey("/MCID").isInteger()) m.mcid = static_cast<int>(pd.getKey("/MCID").getIntValue());
      }
      marks.push_back(m);
    } else if (op == "EMC") {
      if (!marks.empty()) marks.pop_back();
    } else if (op == "Do" && !operands.empty() && depth < kMaxContentNest) {
      QPDFObjectHandle xo = res.isDictionary() ? res.getKey("/XObject") : QPDFObjectHandle();
      QPDFObjectHandle x = xo.isDictionary() ? xo.getKey(operands.back().getValue()) : QPDFObjectHandle();
      if (x.isStream() && nameIs(x.getDict().getKey("/Subtype"), "/Form") && x.isIndirect() &&
          formsSeen.insert(x.getObjGen()).second) {
        Mat fm;
        QPDFObjectHandle m = x.getDict().getKey("/Matrix");
        if (m.isArray() && m.getArrayNItems() == 6) {
          fm = {numOf(m.getArrayItem(0), 1), numOf(m.getArrayItem(1), 0), numOf(m.getArrayItem(2), 0),
                numOf(m.getArrayItem(3), 1), numOf(m.getArrayItem(4), 0), numOf(m.getArrayItem(5), 0)};
        }
        QPDFObjectHandle fr = x.getDict().getKey("/Resources");
        std::vector<TextRun> inner;
        Walker w(fr.isDictionary() ? fr : res, mul(fm, gs.ctm), true, inner, fonts, depth + 1);
        Pl_Discard d;
        try {
          x.filterAsContents(&w, &d);
        } catch (...) {
        }
        for (auto& r : inner) {
          if (r.mcid < 0) r.mcid = currentMcid();
          r.artifact = r.artifact || inArtifact();
          r.op = opIndex;
          out.push_back(r);
        }
        formsSeen.erase(x.getObjGen());
      }
    }
    operands.clear();
  }

  std::set<QPDFObjGen> formsSeen;

 private:
  struct Gs {
    Mat ctm;
    std::shared_ptr<FontInfo> font;
    double size = 0, tc = 0, tw = 0, th = 1, tl = 0, rise = 0;
  };
  struct Mark {
    int mcid = -1;
    bool artifact = false;
  };

  bool nums(size_t k) {
    if (operands.size() < k) return false;
    n.assign(k, 0);
    for (size_t i = 0; i < k; ++i) {
      const auto& tk = operands[operands.size() - k + i];
      if (tk.getType() != QPDFTokenizer::tt_integer && tk.getType() != QPDFTokenizer::tt_real) return false;
      n[i] = std::strtod(tk.getValue().c_str(), nullptr);
    }
    return true;
  }

  void nextLine() {
    tlm = mul({1, 0, 0, 1, 0, -gs.tl}, tlm);
    tm = tlm;
  }

  int currentMcid() const {
    for (auto it = marks.rbegin(); it != marks.rend(); ++it) {
      if (it->mcid >= 0) return it->mcid;
    }
    return -1;
  }

  bool inArtifact() const {
    for (const auto& m : marks) {
      if (m.artifact) return true;
    }
    return false;
  }

  void show(bool array) {
    if (!gs.font) return;
    TextRun run;
    run.mcid = currentMcid();
    run.artifact = inArtifact();
    run.inForm = inForm;
    run.op = opIndex;
    run.font = gs.font->name;
    double ymin = 1e300, ymax = -1e300, xmin = 1e300, xmax = -1e300;
    auto glyphs = [&](const std::string& s) {
      size_t step = gs.font->twoByte ? 2 : 1;
      for (size_t i = 0; i + step - 1 < s.size(); i += step) {
        uint32_t code = static_cast<unsigned char>(s[i]);
        if (step == 2) code = (code << 8) | static_cast<unsigned char>(s[i + 1]);
        Mat trm = mul(mul({gs.size * gs.th, 0, 0, gs.size, 0, gs.rise}, tm), gs.ctm);
        double w0 = gs.font->width(code);
        double tx = (w0 * gs.size + gs.tc + (step == 1 && code == 32 ? gs.tw : 0)) * gs.th;
        Mat after = mul(mul({1, 0, 0, 1, tx, 0}, tm), gs.ctm);
        double eff = std::sqrt(trm.c * trm.c + trm.d * trm.d);
        run.size = std::max(run.size, eff);
        run.charX.push_back(trm.e);
        run.byteAt.push_back(run.text.size());
        run.text += gs.font->text(code);
        double desc = 0.25 * eff, asc = 0.8 * eff;
        xmin = std::min({xmin, trm.e, after.e});
        xmax = std::max({xmax, trm.e, after.e});
        ymin = std::min(ymin, trm.f - desc);
        ymax = std::max(ymax, trm.f + asc);
        tm = mul({1, 0, 0, 1, tx, 0}, tm);
      }
    };
    for (const auto& tk : operands) {
      if (tk.getType() == QPDFTokenizer::tt_string) {
        glyphs(tk.getValue());
      } else if (array && (tk.getType() == QPDFTokenizer::tt_integer || tk.getType() == QPDFTokenizer::tt_real)) {
        double adj = std::strtod(tk.getValue().c_str(), nullptr);
        double tx = -adj / 1000.0 * gs.size * gs.th;
        if (adj < -200 && !run.text.empty() && run.text.back() != ' ') run.text += ' ';
        tm = mul({1, 0, 0, 1, tx, 0}, tm);
      }
    }
    if (run.charX.empty()) return;
    run.x0 = xmin;
    run.x1 = xmax;
    run.y0 = ymin;
    run.y1 = ymax;
    out.push_back(std::move(run));
  }

  QPDFObjectHandle res;
  std::vector<TextRun>& out;
  std::map<QPDFObjGen, std::shared_ptr<FontInfo>>& fonts;
  bool inForm;
  int depth;
  Gs gs;
  std::vector<Gs> stack;
  Mat tm, tlm;
  std::vector<Mark> marks;
  std::vector<QPDFTokenizer::Token> operands;
  std::vector<double> n;
  size_t opIndex = 0;
};
}

std::vector<TextRun> extractPageText(QPDFPageObjectHelper& ph) {
  std::vector<TextRun> out;
  std::map<QPDFObjGen, std::shared_ptr<FontInfo>> fonts;
  QPDFObjectHandle res = ph.getAttribute("/Resources", false);
  Walker w(res, Mat(), false, out, fonts, 0);
  Pl_Discard d;
  try {
    ph.filterContents(&w, &d);
  } catch (...) {
  }
  return out;
}
}
