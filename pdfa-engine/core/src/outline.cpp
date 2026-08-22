#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ctx.hh"
#include "fonts_ft.hh"
#include "passes.hh"
#include "limits.hh"
#include "util.hh"

namespace pdfa {
namespace {
struct OutFont {
  bool loadable = false;
  bool cid = false;
  bool vertical = false;
  bool symbolic = false;
  bool flagged = false;
  std::shared_ptr<FtFace> face;
  double upem = 1000;
  SimpleEncoding enc;
  int firstChar = 0;
  std::vector<double> widths;
  double missingWidth = 0;
  std::map<uint32_t, double> cidWidths;
  double dw = 1000;
  std::string cid2gid;
  std::set<uint32_t> mappedCodes;
  std::set<uint32_t> reverseGids;
};

std::set<uint32_t> parseToUnicodeCodes(QPDFObjectHandle tu) {
  std::set<uint32_t> mapped;
  if (!tu.isStream()) return mapped;
  std::string body;
  try {
    auto buf = tu.getStreamData(qpdf_dl_all);
    body.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return mapped;
  }
  auto hexAt = [&](size_t& i, size_t end, uint32_t& val, std::string& digits) -> bool {
    while (i < end && body[i] != '<') {
      if (body[i] == '[' || body[i] == ']') { ++i; continue; }
      if (!std::isspace(static_cast<unsigned char>(body[i]))) return false;
      ++i;
    }
    if (i >= end) return false;
    size_t close = body.find('>', i);
    if (close == std::string::npos || close > end) return false;
    digits = body.substr(i + 1, close - i - 1);
    i = close + 1;
    if (digits.empty() || digits.size() > 8) return false;
    val = static_cast<uint32_t>(std::strtoul(digits.c_str(), nullptr, 16));
    return true;
  };
  auto realDst = [](const std::string& digits, uint32_t val) {
    if (digits.empty()) return false;
    uint32_t v = val;
    if (digits.size() <= 4) {
      return v != 0 && v != 0xFFFD && v != 0xFFFE && v != 0xFEFF &&
             !(v >= 0xE000 && v <= 0xF8FF);
    }
    return v != 0;
  };
  size_t pos = 0;
  while ((pos = body.find("beginbfchar", pos)) != std::string::npos) {
    size_t end = body.find("endbfchar", pos);
    if (end == std::string::npos) break;
    size_t i = pos + 11;
    while (i < end) {
      uint32_t src = 0, dst = 0;
      std::string d1, d2;
      if (!hexAt(i, end, src, d1)) break;
      if (!hexAt(i, end, dst, d2)) break;
      if (realDst(d2, dst)) mapped.insert(src);
    }
    pos = end + 1;
  }
  pos = 0;
  while ((pos = body.find("beginbfrange", pos)) != std::string::npos) {
    size_t end = body.find("endbfrange", pos);
    if (end == std::string::npos) break;
    size_t i = pos + 12;
    while (i < end) {
      uint32_t lo = 0, hi = 0, dst = 0;
      std::string d1, d2, d3;
      if (!hexAt(i, end, lo, d1)) break;
      if (!hexAt(i, end, hi, d2)) break;
      while (i < end && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
      if (i < end && body[i] == '[') {
        for (uint32_t c = lo; c <= hi && i < end; ++c) {
          if (!hexAt(i, end, dst, d3)) break;
          if (realDst(d3, dst)) mapped.insert(c);
        }
        while (i < end && (body[i] == ']' || std::isspace(static_cast<unsigned char>(body[i])))) ++i;
      } else {
        if (!hexAt(i, end, dst, d3)) break;
        if (hi < lo || hi - lo > 65535) continue;
        for (uint32_t c = lo; c <= hi; ++c) {
          if (realDst(d3, dst + (c - lo))) mapped.insert(c);
        }
      }
    }
    pos = end + 1;
  }
  return mapped;
}

std::set<uint32_t> reverseCmapGids(FT_Face face) {
  std::set<uint32_t> gids;
  if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) return gids;
  FT_UInt gid = 0;
  FT_ULong ch = FT_Get_First_Char(face, &gid);
  while (gid) {
    if (ch < 0xE000 || ch > 0xF8FF) gids.insert(gid);
    ch = FT_Get_Next_Char(face, ch, &gid);
  }
  return gids;
}

void parseCidWidths(QPDFObjectHandle w, std::map<uint32_t, double>& out) {
  if (!w.isArray()) return;
  int i = 0;
  int n = w.getArrayNItems();
  while (i < n) {
    QPDFObjectHandle a = w.getArrayItem(i);
    if (!a.isNumber()) { ++i; continue; }
    long long c1 = static_cast<long long>(a.getNumericValue());
    if (i + 1 >= n) break;
    QPDFObjectHandle b = w.getArrayItem(i + 1);
    if (b.isArray()) {
      for (int j = 0; j < b.getArrayNItems(); ++j) {
        if (b.getArrayItem(j).isNumber() && c1 + j >= 0 && c1 + j < 65536) {
          out[static_cast<uint32_t>(c1 + j)] = b.getArrayItem(j).getNumericValue();
        }
      }
      i += 2;
    } else if (b.isNumber() && i + 2 < n && w.getArrayItem(i + 2).isNumber()) {
      long long c2 = static_cast<long long>(b.getNumericValue());
      double wd = w.getArrayItem(i + 2).getNumericValue();
      if (c2 >= c1 && c2 - c1 < 65536) {
        for (long long c = c1; c <= c2; ++c) {
          if (c >= 0 && c < 65536) out[static_cast<uint32_t>(c)] = wd;
        }
      }
      i += 3;
    } else {
      ++i;
    }
  }
}

std::shared_ptr<OutFont> buildOutFont(FtLib& lib, QPDFObjectHandle font) {
  auto of = std::make_shared<OutFont>();
  std::string subtype = nameOf(font.getKey("/Subtype"));
  if (subtype == "/Type3") return of;
  if (subtype == "/Type0") {
    of->cid = true;
    QPDFObjectHandle enc = font.getKey("/Encoding");
    if (!nameIs(enc, "/Identity-H")) {
      if (nameIs(enc, "/Identity-V")) of->vertical = true;
      return of;
    }
    QPDFObjectHandle df = font.getKey("/DescendantFonts");
    if (!df.isArray() || df.getArrayNItems() != 1 || !df.getArrayItem(0).isDictionary()) {
      return of;
    }
    QPDFObjectHandle cf = df.getArrayItem(0);
    QPDFObjectHandle program = fontFileStream(cf.getKey("/FontDescriptor"));
    if (!program.isStream()) return of;
    of->face = std::make_shared<FtFace>();
    if (!loadFace(lib, program, *of->face)) return of;
    of->loadable = true;
    of->upem = of->face->face->units_per_EM ? of->face->face->units_per_EM : 1000;
    if (cf.getKey("/DW").isNumber()) of->dw = cf.getKey("/DW").getNumericValue();
    parseCidWidths(cf.getKey("/W"), of->cidWidths);
    QPDFObjectHandle c2g = cf.getKey("/CIDToGIDMap");
    if (c2g.isStream()) {
      try {
        auto buf = c2g.getStreamData(qpdf_dl_all);
        of->cid2gid.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
      } catch (...) {
        of->loadable = false;
        return of;
      }
    }
    of->mappedCodes = parseToUnicodeCodes(font.getKey("/ToUnicode"));
    of->reverseGids = reverseCmapGids(of->face->face);
    return of;
  }
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  QPDFObjectHandle program = fontFileStream(fd);
  if (!program.isStream()) return of;
  of->face = std::make_shared<FtFace>();
  if (!loadFace(lib, program, *of->face)) return of;
  of->loadable = true;
  of->upem = of->face->face->units_per_EM ? of->face->face->units_per_EM : 1000;
  long long flags = fd.isDictionary() && fd.getKey("/Flags").isInteger()
                        ? fd.getKey("/Flags").getIntValue()
                        : 0;
  of->symbolic = (flags & 4) && !(flags & 32);
  of->enc = readEncoding(font, of->symbolic);
  of->firstChar = font.getKey("/FirstChar").isInteger()
                      ? static_cast<int>(font.getKey("/FirstChar").getIntValue())
                      : 0;
  QPDFObjectHandle w = font.getKey("/Widths");
  if (w.isArray()) {
    for (int i = 0; i < w.getArrayNItems(); ++i) {
      of->widths.push_back(numOf(w.getArrayItem(i), 0));
    }
  }
  if (fd.isDictionary() && fd.getKey("/MissingWidth").isNumber()) {
    of->missingWidth = fd.getKey("/MissingWidth").getNumericValue();
  }
  of->mappedCodes = parseToUnicodeCodes(font.getKey("/ToUnicode"));
  of->reverseGids = reverseCmapGids(of->face->face);
  return of;
}

uint32_t gidForCode(const OutFont& of, uint32_t code) {
  if (of.cid) {
    if (of.cid2gid.empty()) return code;
    size_t idx = static_cast<size_t>(code) * 2;
    if (idx + 1 >= of.cid2gid.size()) return 0;
    return (static_cast<unsigned char>(of.cid2gid[idx]) << 8) |
           static_cast<unsigned char>(of.cid2gid[idx + 1]);
  }
  return resolveSimpleGid(*of.face, static_cast<int>(code), of.enc, of.symbolic);
}

bool codeMappable(const OutFont& of, uint32_t code) {
  if (of.mappedCodes.count(code)) return true;
  if (of.cid) {
    uint32_t gid = gidForCode(of, code);
    return gid && of.reverseGids.count(gid);
  }
  const std::string& diffName = of.enc.diffs[code];
  if (!diffName.empty()) {
    uint32_t uni = aglNameToUnicode(diffName.substr(1));
    if (!uni) parseUniName(diffName.substr(1), uni);
    if (uni) return true;
  }
  if (!of.symbolic && of.enc.base && of.enc.base(static_cast<int>(code))) return true;
  FT_Face face = of.face->face;
  uint32_t gid = gidForCode(of, code);
  if (gid && FT_HAS_GLYPH_NAMES(face)) {
    char gname[64];
    if (FT_Get_Glyph_Name(face, gid, gname, sizeof(gname)) == 0 && gname[0]) {
      uint32_t uni = aglNameToUnicode(gname);
      if (!uni) parseUniName(gname, uni);
      if (uni) return true;
    }
  }
  if (gid && of.reverseGids.count(gid)) return true;
  if (!of.symbolic && winAnsiToUnicode(static_cast<int>(code))) return true;
  return false;
}

double widthForCode(const OutFont& of, uint32_t code, uint32_t gid) {
  if (of.cid) {
    auto it = of.cidWidths.find(code);
    if (it != of.cidWidths.end()) return it->second;
    return of.dw;
  }
  int idx = static_cast<int>(code) - of.firstChar;
  if (idx >= 0 && idx < static_cast<int>(of.widths.size()) && of.widths[idx] > 0) {
    return of.widths[idx];
  }
  if (of.missingWidth > 0) return of.missingWidth;
  FT_Face face = of.face->face;
  if (gid && FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP) == 0) {
    return face->glyph->advance.x * 1000.0 / of.upem;
  }
  return 0;
}

struct Mat {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

Mat mul(const Mat& m, const Mat& n) {
  Mat r;
  r.a = m.a * n.a + m.b * n.c;
  r.b = m.a * n.b + m.b * n.d;
  r.c = m.c * n.a + m.d * n.c;
  r.d = m.c * n.b + m.d * n.d;
  r.e = m.e * n.a + m.f * n.c + n.e;
  r.f = m.e * n.b + m.f * n.d + n.f;
  return r;
}

struct TextState {
  std::string fontName;
  double size = 0;
  double charSpace = 0;
  double wordSpace = 0;
  double hscale = 100;
  double leading = 0;
  double rise = 0;
  int renderMode = 0;
};

std::string fmtNum(double v) { return fmtFixed(v, 3); }

struct GlyphEmitter {
  std::string& out;
  Mat trm;
  double scale;
  double hscale;

  static int moveTo(const FT_Vector* to, void* user) {
    auto* g = static_cast<GlyphEmitter*>(user);
    g->point(to->x, to->y, "m");
    g->curX = to->x;
    g->curY = to->y;
    return 0;
  }
  static int lineTo(const FT_Vector* to, void* user) {
    auto* g = static_cast<GlyphEmitter*>(user);
    g->point(to->x, to->y, "l");
    g->curX = to->x;
    g->curY = to->y;
    return 0;
  }
  static int conicTo(const FT_Vector* q, const FT_Vector* to, void* user) {
    auto* g = static_cast<GlyphEmitter*>(user);
    double c1x = g->curX + 2.0 / 3.0 * (q->x - g->curX);
    double c1y = g->curY + 2.0 / 3.0 * (q->y - g->curY);
    double c2x = to->x + 2.0 / 3.0 * (q->x - to->x);
    double c2y = to->y + 2.0 / 3.0 * (q->y - to->y);
    g->curve(c1x, c1y, c2x, c2y, to->x, to->y);
    g->curX = to->x;
    g->curY = to->y;
    return 0;
  }
  static int cubicTo(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to,
                     void* user) {
    auto* g = static_cast<GlyphEmitter*>(user);
    g->curve(c1->x, c1->y, c2->x, c2->y, to->x, to->y);
    g->curX = to->x;
    g->curY = to->y;
    return 0;
  }

  void map(double gx, double gy, double& x, double& y) const {
    double u = gx * scale * hscale;
    double v = gy * scale;
    x = trm.a * u + trm.c * v + trm.e;
    y = trm.b * u + trm.d * v + trm.f;
  }
  void point(double gx, double gy, const char* op) {
    double x, y;
    map(gx, gy, x, y);
    out += fmtNum(x) + " " + fmtNum(y) + " " + op + "\n";
  }
  void curve(double x1, double y1, double x2, double y2, double x3, double y3) {
    double a, b, c, d, e, f;
    map(x1, y1, a, b);
    map(x2, y2, c, d);
    map(x3, y3, e, f);
    out += fmtNum(a) + " " + fmtNum(b) + " " + fmtNum(c) + " " + fmtNum(d) + " " +
           fmtNum(e) + " " + fmtNum(f) + " c\n";
  }

  double curX = 0, curY = 0;
};

struct OutlineFilter : public QPDFObjectHandle::TokenFilter {
  OutlineFilter(const std::map<std::string, std::shared_ptr<OutFont>>& fonts, int& runs,
                bool& touched)
      : fonts(fonts), runs(runs), touched(touched) {}

  const std::map<std::string, std::shared_ptr<OutFont>>& fonts;
  int& runs;
  bool& touched;
  TextState ts;
  std::vector<TextState> gsStack;
  bool buffering = false;
  std::vector<QPDFTokenizer::Token> buffer;
  std::vector<QPDFTokenizer::Token> operands;

  const OutFont* fontOf(const std::string& name) const {
    auto it = fonts.find(name);
    return it == fonts.end() ? nullptr : it->second.get();
  }

  static bool isShowOp(const std::string& op) {
    return op == "Tj" || op == "TJ" || op == "'" || op == "\"";
  }

  static std::vector<QPDFTokenizer::Token> sig(
      const std::vector<QPDFTokenizer::Token>& v) {
    std::vector<QPDFTokenizer::Token> out;
    for (const auto& t : v) {
      if (t.getType() == QPDFTokenizer::tt_space ||
          t.getType() == QPDFTokenizer::tt_comment) {
        continue;
      }
      out.push_back(t);
    }
    return out;
  }

  static bool isTextStateOp(const std::string& op) {
    return op == "Tf" || op == "Tc" || op == "Tw" || op == "Tz" || op == "TL" ||
           op == "Ts" || op == "Tr" || op == "Td" || op == "TD" || op == "Tm" ||
           op == "T*";
  }

  void handleToken(QPDFTokenizer::Token const& token) override {
    if (buffering) {
      buffer.push_back(token);
      if (token.getType() == QPDFTokenizer::tt_word && token.getValue() == "ET") {
        finishBlock();
        buffering = false;
        buffer.clear();
      }
      return;
    }
    QPDFTokenizer::token_type_e type = token.getType();
    if (type != QPDFTokenizer::tt_word) {
      operands.push_back(token);
      return;
    }
    std::string op = token.getValue();
    if (op == "BT") {
      flushOperands();
      buffering = true;
      buffer.clear();
      buffer.push_back(token);
      return;
    }
    trackStateOp(op, operands, ts);
    if (op == "q") {
      gsStack.push_back(ts);
    } else if (op == "Q") {
      if (!gsStack.empty()) {
        ts = gsStack.back();
        gsStack.pop_back();
      }
    }
    flushOperands();
    writeToken(token);
  }

  void handleEOF() override {
    if (buffering) {
      for (auto& t : buffer) {
        if (t.getType() == QPDFTokenizer::tt_string) {
          write(QPDFObjectHandle::newString(t.getValue()).unparse());
          write(" ");
        } else {
          writeToken(t);
          write(" ");
        }
      }
      buffering = false;
      buffer.clear();
    }
    flushOperands();
  }

  void flushOperands() {
    for (auto& t : operands) {
      if (t.getType() == QPDFTokenizer::tt_string) {
        write(QPDFObjectHandle::newString(t.getValue()).unparse());
        write(" ");
      } else {
        writeToken(t);
        write(" ");
      }
    }
    operands.clear();
  }

  static double numTok(const QPDFTokenizer::Token& t, double dflt) {
    if (t.getType() != QPDFTokenizer::tt_integer && t.getType() != QPDFTokenizer::tt_real) {
      return dflt;
    }
    return std::atof(t.getValue().c_str());
  }

  static void trackStateOp(const std::string& op,
                           const std::vector<QPDFTokenizer::Token>& rawArgs,
                           TextState& st) {
    std::vector<QPDFTokenizer::Token> args = sig(rawArgs);
    auto lastName = [&]() -> std::string {
      for (auto it = args.rbegin(); it != args.rend(); ++it) {
        if (it->getType() == QPDFTokenizer::tt_name) return it->getValue();
      }
      return std::string();
    };
    size_t n = args.size();
    if (op == "Tf" && n >= 2) {
      st.fontName = lastName();
      st.size = numTok(args[n - 1], st.size);
    } else if (op == "Tc" && n >= 1) {
      st.charSpace = numTok(args[n - 1], st.charSpace);
    } else if (op == "Tw" && n >= 1) {
      st.wordSpace = numTok(args[n - 1], st.wordSpace);
    } else if (op == "Tz" && n >= 1) {
      st.hscale = numTok(args[n - 1], st.hscale);
    } else if (op == "TL" && n >= 1) {
      st.leading = numTok(args[n - 1], st.leading);
    } else if (op == "Ts" && n >= 1) {
      st.rise = numTok(args[n - 1], st.rise);
    } else if (op == "Tr" && n >= 1) {
      st.renderMode = static_cast<int>(numTok(args[n - 1], st.renderMode));
    }
  }

  bool blockNeedsOutline() {
    TextState sim = ts;
    std::vector<QPDFTokenizer::Token> args;
    bool need = false;
    for (const auto& t : buffer) {
      if (t.getType() != QPDFTokenizer::tt_word) {
        args.push_back(t);
        continue;
      }
      std::string op = t.getValue();
      trackStateOp(op, args, sim);
      if (isShowOp(op)) {
        const OutFont* of = fontOf(sim.fontName);
        if (of && of->flagged) need = true;
        if (!of || !of->loadable || of->vertical) {
          bool hasString = false;
          for (const auto& a : args) {
            if (a.getType() == QPDFTokenizer::tt_string) hasString = true;
          }
          if (hasString) return false;
        }
      }
      args.clear();
    }
    return need;
  }

  void emitGlyphRun(std::string& out, std::string& clipOut, const std::string& bytes,
                    const OutFont& of,
                    TextState& st, Mat& tm) {
    FT_Face face = of.face->face;
    size_t step = of.cid ? 2 : 1;
    for (size_t i = 0; i + step <= bytes.size(); i += step) {
      uint32_t code;
      if (of.cid) {
        code = (static_cast<unsigned char>(bytes[i]) << 8) |
               static_cast<unsigned char>(bytes[i + 1]);
      } else {
        code = static_cast<unsigned char>(bytes[i]);
      }
      uint32_t gid = gidForCode(of, code);
      double w1000 = widthForCode(of, code, gid);
      int mode = st.renderMode;
      bool paints = mode == 0 || mode == 1 || mode == 2 || mode == 4 || mode == 5 || mode == 6;
      bool clips = mode >= 4 && mode <= 7;
      if ((paints || clips) && gid &&
          FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP) == 0 &&
          face->glyph->format == FT_GLYPH_FORMAT_OUTLINE &&
          face->glyph->outline.n_contours > 0) {
        Mat withRise = tm;
        withRise.e = tm.c * st.rise + tm.e;
        withRise.f = tm.d * st.rise + tm.f;
        GlyphEmitter em{out, withRise, st.size / of.upem, st.hscale / 100.0};
        FT_Outline_Funcs funcs;
        funcs.move_to = GlyphEmitter::moveTo;
        funcs.line_to = GlyphEmitter::lineTo;
        funcs.conic_to = GlyphEmitter::conicTo;
        funcs.cubic_to = GlyphEmitter::cubicTo;
        funcs.shift = 0;
        funcs.delta = 0;
        size_t mark = out.size();
        if (FT_Outline_Decompose(&face->glyph->outline, &funcs, &em) == 0) {
          if (clips) clipOut.append(out, mark, out.size() - mark);
          if (paints) {
            out += mode == 1 ? "S\n" : (mode == 2 || mode == 5 || mode == 6 ? "B\n" : "f\n");
          } else {
            out.resize(mark);
          }
        } else {
          out.resize(mark);
        }
      }
      double adv = (w1000 / 1000.0 * st.size + st.charSpace +
                    (!of.cid && code == 32 ? st.wordSpace : 0)) *
                   st.hscale / 100.0;
      Mat shift;
      shift.e = adv;
      tm = mul(shift, tm);
      ++runs;
    }
  }

  void finishBlock() {
    if (!blockNeedsOutline()) {
      for (auto& t : buffer) {
        if (t.getType() == QPDFTokenizer::tt_string) {
          write(QPDFObjectHandle::newString(t.getValue()).unparse());
          write(" ");
        } else {
          writeToken(t);
          write(" ");
        }
      }
      TextState sim = ts;
      std::vector<QPDFTokenizer::Token> args;
      for (const auto& t : buffer) {
        if (t.getType() != QPDFTokenizer::tt_word) {
          args.push_back(t);
          continue;
        }
        trackStateOp(t.getValue(), args, sim);
        args.clear();
      }
      ts = sim;
      return;
    }
    touched = true;
    std::string out;
    std::string clipOut;
    Mat tm, tlm;
    std::vector<QPDFTokenizer::Token> rawArgs;
    for (const auto& t : buffer) {
      if (t.getType() != QPDFTokenizer::tt_word) {
        rawArgs.push_back(t);
        continue;
      }
      std::string op = t.getValue();
      std::vector<QPDFTokenizer::Token> args = sig(rawArgs);
      if (op == "BT" || op == "ET") {
        tm = Mat();
        tlm = Mat();
        rawArgs.clear();
        continue;
      }
      if (op == "Tm" && args.size() >= 6) {
        size_t n = args.size();
        Mat m;
        m.a = numTok(args[n - 6], 1);
        m.b = numTok(args[n - 5], 0);
        m.c = numTok(args[n - 4], 0);
        m.d = numTok(args[n - 3], 1);
        m.e = numTok(args[n - 2], 0);
        m.f = numTok(args[n - 1], 0);
        tm = m;
        tlm = m;
        rawArgs.clear();
        continue;
      }
      if ((op == "Td" || op == "TD") && args.size() >= 2) {
        size_t n = args.size();
        double tx = numTok(args[n - 2], 0);
        double ty = numTok(args[n - 1], 0);
        if (op == "TD") ts.leading = -ty;
        Mat shift;
        shift.e = tx;
        shift.f = ty;
        tlm = mul(shift, tlm);
        tm = tlm;
        rawArgs.clear();
        continue;
      }
      if (op == "T*") {
        Mat shift;
        shift.f = -ts.leading;
        tlm = mul(shift, tlm);
        tm = tlm;
        rawArgs.clear();
        continue;
      }
      if (isTextStateOp(op)) {
        trackStateOp(op, args, ts);
        rawArgs.clear();
        continue;
      }
      if (isShowOp(op)) {
        const OutFont* of = fontOf(ts.fontName);
        if (op == "'" || op == "\"") {
          if (op == "\"" && args.size() >= 3) {
            size_t n = args.size();
            ts.wordSpace = numTok(args[n - 3], ts.wordSpace);
            ts.charSpace = numTok(args[n - 2], ts.charSpace);
          }
          Mat shift;
          shift.f = -ts.leading;
          tlm = mul(shift, tlm);
          tm = tlm;
        }
        if (of && of->loadable) {
          if (op == "TJ") {
            bool inArray = false;
            for (const auto& a : args) {
              if (a.getType() == QPDFTokenizer::tt_array_open) inArray = true;
              else if (a.getType() == QPDFTokenizer::tt_array_close) inArray = false;
              else if (a.getType() == QPDFTokenizer::tt_string) {
                emitGlyphRun(out, clipOut, a.getValue(), *of, ts, tm);
              } else if (inArray && (a.getType() == QPDFTokenizer::tt_integer ||
                                     a.getType() == QPDFTokenizer::tt_real)) {
                double adj = std::atof(a.getValue().c_str());
                double tx = -adj / 1000.0 * ts.size * ts.hscale / 100.0;
                Mat shift;
                shift.e = tx;
                tm = mul(shift, tm);
              }
            }
          } else {
            for (const auto& a : args) {
              if (a.getType() == QPDFTokenizer::tt_string) {
                emitGlyphRun(out, clipOut, a.getValue(), *of, ts, tm);
              }
            }
          }
        }
        rawArgs.clear();
        continue;
      }
      for (auto& a : args) {
        if (a.getType() == QPDFTokenizer::tt_string) {
          out += QPDFObjectHandle::newString(a.getValue()).unparse() + " ";
        } else {
          out += a.getRawValue() + " ";
        }
      }
      out += op + "\n";
      rawArgs.clear();
    }
    if (!clipOut.empty()) {
      out += clipOut;
      out += "W n\n";
    }
    write(out);
    std::string sync;
    if (!ts.fontName.empty()) {
      sync += ts.fontName + " " + fmtNum(ts.size) + " Tf\n";
    }
    sync += fmtNum(ts.charSpace) + " Tc " + fmtNum(ts.wordSpace) + " Tw " +
            fmtNum(ts.hscale) + " Tz " + fmtNum(ts.leading) + " TL " + fmtNum(ts.rise) +
            " Ts " + std::to_string(ts.renderMode) + " Tr\n";
    write(sync);
  }
};

struct UsageScanner : public QPDFObjectHandle::ParserCallbacks {
  std::map<std::string, std::set<uint32_t>>& used;
  const std::map<std::string, std::shared_ptr<OutFont>>& fonts;
  std::string curFont;
  std::vector<std::string> fontStack;
  std::vector<QPDFObjectHandle> operands;

  UsageScanner(std::map<std::string, std::set<uint32_t>>& u,
               const std::map<std::string, std::shared_ptr<OutFont>>& f)
      : used(u), fonts(f) {}

  void handleObject(QPDFObjectHandle obj, size_t, size_t) override {
    if (!obj.isOperator()) {
      operands.push_back(obj);
      return;
    }
    std::string op = obj.getOperatorValue();
    if (op == "q") {
      fontStack.push_back(curFont);
    } else if (op == "Q") {
      if (!fontStack.empty()) {
        curFont = fontStack.back();
        fontStack.pop_back();
      }
    } else if (op == "Tf") {
      for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
        if (it->isName()) {
          curFont = it->getName();
          break;
        }
      }
    } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
      auto fit = fonts.find(curFont);
      if (fit != fonts.end()) {
        bool cid = fit->second->cid;
        for (auto& o : operands) {
          std::vector<std::string> strs;
          if (o.isString()) strs.push_back(o.getStringValue());
          if (o.isArray()) {
            for (int i = 0; i < o.getArrayNItems(); ++i) {
              if (o.getArrayItem(i).isString()) {
                strs.push_back(o.getArrayItem(i).getStringValue());
              }
            }
          }
          for (const std::string& s : strs) {
            if (cid) {
              for (size_t i = 0; i + 1 < s.size(); i += 2) {
                used[curFont].insert((static_cast<unsigned char>(s[i]) << 8) |
                                     static_cast<unsigned char>(s[i + 1]));
              }
            } else {
              for (unsigned char c : s) used[curFont].insert(c);
            }
          }
        }
      }
    }
    operands.clear();
  }

  void handleEOF() override {}
};

void outlineHolder(Ctx& ctx, FtLib& lib, QPDFObjectHandle holder, QPDFObjectHandle res,
                   std::map<QPDFObjGen, std::shared_ptr<OutFont>>& cache, Visited& visited,
                   int& runs, int& fontsFlagged, std::set<QPDFObjGen>& flaggedSet,
                   int depth = 0) {
  if (depth > kMaxResourceNest || !res.isDictionary()) return;
  std::map<std::string, std::shared_ptr<OutFont>> fonts;
  QPDFObjectHandle fd = res.getKey("/Font");
  if (fd.isDictionary()) {
    for (const std::string& k : fd.getKeys()) {
      QPDFObjectHandle fnt = fd.getKey(k);
      if (!fnt.isDictionary()) continue;
      std::shared_ptr<OutFont> info;
      if (fnt.isIndirect()) {
        auto it = cache.find(fnt.getObjGen());
        if (it == cache.end()) {
          it = cache.emplace(fnt.getObjGen(), buildOutFont(lib, fnt)).first;
        }
        info = it->second;
      } else {
        info = buildOutFont(lib, fnt);
      }
      fonts[k] = info;
    }
  }
  bool anyLoadable = false;
  for (auto& kv : fonts) {
    if (kv.second->loadable) anyLoadable = true;
  }
  if (anyLoadable) {
    std::map<std::string, std::set<uint32_t>> used;
    UsageScanner scan(used, fonts);
    try {
      if (holder.isStream()) {
        QPDFObjectHandle::parseContentStream(holder, &scan);
      } else {
        QPDFObjectHandle::parseContentStream(holder.getKey("/Contents"), &scan);
      }
    } catch (...) {
      used.clear();
    }
    for (auto& kv : used) {
      auto fit = fonts.find(kv.first);
      if (fit == fonts.end() || !fit->second->loadable || fit->second->flagged) continue;
      for (uint32_t code : kv.second) {
        if (!codeMappable(*fit->second, code)) {
          fit->second->flagged = true;
          QPDFObjectHandle f = fd.getKey(kv.first);
          if (f.isIndirect() && flaggedSet.insert(f.getObjGen()).second) ++fontsFlagged;
          break;
        }
      }
    }
    bool anyFlagged = false;
    for (auto& kv : fonts) {
      if (kv.second->flagged) anyFlagged = true;
    }
    if (anyFlagged) {
      bool touched = false;
      try {
        QPDFPageObjectHelper ph(holder);
        Pl_Buffer sink("outline rewrite");
        OutlineFilter filter(fonts, runs, touched);
        ph.filterContents(&filter, &sink);
        if (touched) {
          auto data = sink.getBufferSharedPointer();
          std::string rewritten(reinterpret_cast<const char*>(data->getBuffer()),
                                data->getSize());
          if (holder.isStream()) {
            holder.replaceStreamData(rewritten, QPDFObjectHandle::newNull(),
                                     QPDFObjectHandle::newNull());
          } else {
            holder.replaceKey("/Contents",
                              ctx.pdf.makeIndirectObject(
                                  QPDFObjectHandle::newStream(&ctx.pdf, rewritten)));
          }
        }
      } catch (...) {
        ctx.scanIncomplete("a content stream being outlined");
      }
    }
  }
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (xod.isDictionary()) {
    for (const std::string& k : xod.getKeys()) {
      QPDFObjectHandle xo = xod.getKey(k);
      if (xo.isStream() && nameIs(xo.getDict().getKey("/Subtype"), "/Form") &&
          visited.enter(xo)) {
        QPDFObjectHandle inner = xo.getDict().getKey("/Resources");
        outlineHolder(ctx, lib, xo, inner.isDictionary() ? inner : res, cache, visited,
                      runs, fontsFlagged, flaggedSet, depth + 1);
      }
    }
  }
}
}

void passOutlineFonts(Ctx& ctx) {
  if (!ctx.opt.outlineFonts || !ctx.needUnicode()) return;
  FtLib lib;
  std::map<QPDFObjGen, std::shared_ptr<OutFont>> cache;
  Visited visited;
  std::set<QPDFObjGen> flaggedSet;
  int runs = 0, fontsFlagged = 0;
  QPDFPageDocumentHelper dh(ctx.pdf);
  for (auto& ph : dh.getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    outlineHolder(ctx, lib, page, ph.getAttribute("/Resources", false), cache, visited,
                  runs, fontsFlagged, flaggedSet);
  }
  if (runs) {
    ctx.issue("TEXT_OUTLINED",
              "converted text to vector outlines in " + std::to_string(fontsFlagged) +
                  " font(s) whose characters have no derivable Unicode mapping (" +
                  std::to_string(runs) + " glyph(s); affected text is no longer "
                  "searchable or selectable)",
              true);
  }
}
}
