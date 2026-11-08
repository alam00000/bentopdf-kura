#include "fontworld.hh"

#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <hb.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <vector>

#include "fonts_ft.hh"
#include "util.hh"

namespace pdfa {
namespace {

uint16_t rd16(const std::string& d, size_t o) {
  return o + 2 <= d.size() ? static_cast<uint16_t>((static_cast<unsigned char>(d[o]) << 8) | static_cast<unsigned char>(d[o + 1])) : 0;
}
uint32_t rd32(const std::string& d, size_t o) { return (static_cast<uint32_t>(rd16(d, o)) << 16) | rd16(d, o + 2); }
void wr16(std::string& d, size_t o, uint16_t v) {
  d[o] = static_cast<char>(v >> 8);
  d[o + 1] = static_cast<char>(v & 0xFF);
}
void wr32(std::string& d, size_t o, uint32_t v) {
  wr16(d, o, static_cast<uint16_t>(v >> 16));
  wr16(d, o + 2, static_cast<uint16_t>(v & 0xFFFF));
}
void put16(std::string& d, uint16_t v) {
  d += static_cast<char>(v >> 8);
  d += static_cast<char>(v & 0xFF);
}
void put32(std::string& d, uint32_t v) {
  put16(d, static_cast<uint16_t>(v >> 16));
  put16(d, static_cast<uint16_t>(v & 0xFFFF));
}

struct WFont {
  std::string name;
  std::string data;
  std::map<std::string, std::string> tables;
  FT_Face face = nullptr;
  hb_blob_t* blob = nullptr;
  hb_face_t* hbFace = nullptr;
  hb_font_t* hbFont = nullptr;
  uint16_t numGlyphs = 0, numMetrics = 0;
  bool longLoca = false;
  ~WFont() {
    if (hbFont) hb_font_destroy(hbFont);
    if (hbFace) hb_face_destroy(hbFace);
    if (blob) hb_blob_destroy(blob);
    if (face) FT_Done_Face(face);
  }
  bool parse() {
    if (data.size() < 12) return false;
    uint16_t n = rd16(data, 4);
    for (uint16_t i = 0; i < n; ++i) {
      size_t rec = 12 + 16u * i;
      uint32_t off = rd32(data, rec + 8), len = rd32(data, rec + 12);
      if (off > data.size() || len > data.size() - off) return false;
      tables[data.substr(rec, 4)] = data.substr(off, len);
    }
    for (const char* t : {"head", "hhea", "maxp", "hmtx", "loca", "glyf"})
      if (!tables.count(t)) return false;
    if (tables["head"].size() < 54 || tables["hhea"].size() < 36 || tables["maxp"].size() < 6) return false;
    if (rd16(tables["head"], 18) != 1000) return false;
    longLoca = rd16(tables["head"], 50) == 1;
    numGlyphs = rd16(tables["maxp"], 4);
    numMetrics = rd16(tables["hhea"], 34);
    return numMetrics > 0;
  }
  std::string glyph(uint32_t g) {
    const std::string& loca = tables["loca"];
    const std::string& glyf = tables["glyf"];
    if (g >= numGlyphs) return std::string();
    uint32_t a, b;
    if (longLoca) {
      a = rd32(loca, g * 4);
      b = rd32(loca, (g + 1) * 4);
    } else {
      a = rd16(loca, g * 2) * 2u;
      b = rd16(loca, (g + 1) * 2) * 2u;
    }
    if (a > b || b > glyf.size()) return std::string();
    return glyf.substr(a, b - a);
  }
  uint16_t advance(uint32_t g) {
    const std::string& h = tables["hmtx"];
    uint32_t i = g < numMetrics ? g : numMetrics - 1u;
    return rd16(h, i * 4);
  }
  int16_t lsb(uint32_t g) {
    const std::string& h = tables["hmtx"];
    size_t o = g < numMetrics ? g * 4u + 2 : numMetrics * 4u + (g - numMetrics) * 2u;
    return static_cast<int16_t>(rd16(h, o));
  }
  uint32_t gidFor(uint32_t cp) { return face ? FT_Get_Char_Index(face, cp) : 0; }
};

const char* const kFamilies[] = {
    "NotoSans", "NotoSerif", "NotoSansArabic", "NotoNaskhArabic", "NotoSansHebrew", "NotoSansArmenian",
    "NotoSansGeorgian", "NotoSansEthiopic", "NotoSansDevanagari", "NotoSansBengali", "NotoSansGurmukhi",
    "NotoSansGujarati", "NotoSansOriya", "NotoSansTamil", "NotoSansTelugu", "NotoSansKannada",
    "NotoSansMalayalam", "NotoSansSinhala", "NotoSansThai", "NotoSansLao", "NotoSansKhmer", "NotoSansMyanmar",
    "NotoSerifTibetan", "NotoSansSyriac", "NotoSansThaana", "NotoSansNKo", "NotoSansMongolian",
    "NotoSansCherokee", "NotoSansCanadianAboriginal", "NotoSansTifinagh", "NotoSansJavanese",
    "NotoSansBalinese", "NotoSansSundanese", "NotoSansOlChiki", "NotoSansAdlam", "NotoSansVai", "NotoSansYi",
    "NotoSansMath", "NotoSansSymbols", "NotoSansSymbols2", "NotoSansCoptic", "NotoSansGothic", "NotoSansRunic",
    "NotoSansOgham", "NotoSansBamum", "NotoSansChakma", "NotoSansLimbu", "NotoSansMeeteiMayek",
    "NotoSansTaiTham", "NotoSansTaiViet", "NotoSansSaurashtra", "NotoSansSamaritan", "NotoSansMandaic",
    "NotoSansBuginese", "NotoSansBatak", "NotoSansTagalog", "NotoSansHanunoo", "NotoSansBuhid",
    "NotoSansTagbanwa", "NotoSansKayahLi", "NotoSansLepcha", "NotoSansNewTaiLue", "NotoSansTaiLe",
    "NotoSansCham", "NotoSansGlagolitic"};

struct Range {
  uint32_t lo, hi;
  const char* family;
};
const Range kRanges[] = {
    {0x0530, 0x058F, "NotoSansArmenian"}, {0xFB13, 0xFB17, "NotoSansArmenian"},
    {0x0590, 0x05FF, "NotoSansHebrew"}, {0xFB1D, 0xFB4F, "NotoSansHebrew"},
    {0x0600, 0x06FF, "NotoSansArabic"}, {0x0750, 0x077F, "NotoSansArabic"}, {0x0870, 0x08FF, "NotoSansArabic"},
    {0xFB50, 0xFDFF, "NotoSansArabic"}, {0xFE70, 0xFEFF, "NotoSansArabic"},
    {0x0700, 0x074F, "NotoSansSyriac"}, {0x0780, 0x07BF, "NotoSansThaana"}, {0x07C0, 0x07FF, "NotoSansNKo"},
    {0x0800, 0x083F, "NotoSansSamaritan"}, {0x0840, 0x085F, "NotoSansMandaic"},
    {0x0900, 0x097F, "NotoSansDevanagari"}, {0xA8E0, 0xA8FF, "NotoSansDevanagari"},
    {0x0980, 0x09FF, "NotoSansBengali"}, {0x0A00, 0x0A7F, "NotoSansGurmukhi"},
    {0x0A80, 0x0AFF, "NotoSansGujarati"}, {0x0B00, 0x0B7F, "NotoSansOriya"}, {0x0B80, 0x0BFF, "NotoSansTamil"},
    {0x0C00, 0x0C7F, "NotoSansTelugu"}, {0x0C80, 0x0CFF, "NotoSansKannada"}, {0x0D00, 0x0D7F, "NotoSansMalayalam"},
    {0x0D80, 0x0DFF, "NotoSansSinhala"}, {0x0E00, 0x0E7F, "NotoSansThai"}, {0x0E80, 0x0EFF, "NotoSansLao"},
    {0x0F00, 0x0FFF, "NotoSerifTibetan"}, {0x1000, 0x109F, "NotoSansMyanmar"}, {0xAA60, 0xAA7F, "NotoSansMyanmar"},
    {0x10A0, 0x10FF, "NotoSansGeorgian"}, {0x2D00, 0x2D2F, "NotoSansGeorgian"}, {0x1C90, 0x1CBF, "NotoSansGeorgian"},
    {0x1200, 0x139F, "NotoSansEthiopic"}, {0x2D80, 0x2DDF, "NotoSansEthiopic"}, {0xAB00, 0xAB2F, "NotoSansEthiopic"},
    {0x13A0, 0x13FF, "NotoSansCherokee"}, {0xAB70, 0xABBF, "NotoSansCherokee"},
    {0x1400, 0x167F, "NotoSansCanadianAboriginal"}, {0x18B0, 0x18FF, "NotoSansCanadianAboriginal"},
    {0x1680, 0x169F, "NotoSansOgham"}, {0x16A0, 0x16FF, "NotoSansRunic"}, {0x1700, 0x171F, "NotoSansTagalog"},
    {0x1720, 0x173F, "NotoSansHanunoo"}, {0x1740, 0x175F, "NotoSansBuhid"}, {0x1760, 0x177F, "NotoSansTagbanwa"},
    {0x1780, 0x17FF, "NotoSansKhmer"}, {0x19E0, 0x19FF, "NotoSansKhmer"}, {0x1800, 0x18AF, "NotoSansMongolian"},
    {0x1900, 0x194F, "NotoSansLimbu"}, {0x1950, 0x197F, "NotoSansTaiLe"}, {0x1980, 0x19DF, "NotoSansNewTaiLue"},
    {0x1A00, 0x1A1F, "NotoSansBuginese"}, {0x1A20, 0x1AAF, "NotoSansTaiTham"}, {0x1B00, 0x1B7F, "NotoSansBalinese"},
    {0x1B80, 0x1BBF, "NotoSansSundanese"}, {0x1BC0, 0x1BFF, "NotoSansBatak"}, {0x1C00, 0x1C4F, "NotoSansLepcha"},
    {0x1C50, 0x1C7F, "NotoSansOlChiki"}, {0x2C00, 0x2C5F, "NotoSansGlagolitic"}, {0x2C80, 0x2CFF, "NotoSansCoptic"},
    {0x2D30, 0x2D7F, "NotoSansTifinagh"}, {0xA000, 0xA4CF, "NotoSansYi"}, {0xA500, 0xA63F, "NotoSansVai"},
    {0xA6A0, 0xA6FF, "NotoSansBamum"}, {0xA880, 0xA8DF, "NotoSansSaurashtra"}, {0xA900, 0xA92F, "NotoSansKayahLi"},
    {0xA980, 0xA9DF, "NotoSansJavanese"}, {0xAA00, 0xAA5F, "NotoSansCham"}, {0xAA80, 0xAADF, "NotoSansTaiViet"},
    {0xABC0, 0xABFF, "NotoSansMeeteiMayek"}, {0x11100, 0x1114F, "NotoSansChakma"}, {0x10330, 0x1034F, "NotoSansGothic"},
    {0x1E900, 0x1E95F, "NotoSansAdlam"}, {0x1D400, 0x1D7FF, "NotoSansMath"}, {0x2200, 0x22FF, "NotoSansMath"},
    {0x27C0, 0x27EF, "NotoSansMath"}, {0x2980, 0x2AFF, "NotoSansMath"}, {0x2190, 0x21FF, "NotoSansSymbols"},
    {0x2300, 0x23FF, "NotoSansSymbols"}, {0x2460, 0x24FF, "NotoSansSymbols"}, {0x2500, 0x27BF, "NotoSansSymbols2"},
    {0x1F000, 0x1FBFF, "NotoSansSymbols2"}};

const char* familyFor(uint32_t cp, bool serif) {
  for (const Range& r : kRanges)
    if (cp >= r.lo && cp <= r.hi) return r.family;
  return serif ? "NotoSerif" : "NotoSans";
}

bool joiningScript(uint32_t cp) {
  return (cp >= 0x0600 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFEFF) || (cp >= 0x0700 && cp <= 0x07FF) ||
         (cp >= 0x0840 && cp <= 0x085F) || (cp >= 0x1800 && cp <= 0x18AF) || (cp >= 0x1E900 && cp <= 0x1E95F);
}

bool blankChar(uint32_t cp) {
  return cp <= 0x20 || (cp >= 0x7F && cp <= 0xA0) || cp == 0xAD || (cp >= 0x2000 && cp <= 0x200F) ||
         (cp >= 0x2028 && cp <= 0x202F) || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

struct WorldCache {
  FtLib lib;
  std::map<std::string, std::shared_ptr<WFont>> fonts;
};

std::shared_ptr<WFont> loadFont(Ctx& ctx, WorldCache& cache, const std::string& name) {
  auto it = cache.fonts.find(name);
  if (it != cache.fonts.end()) return it->second;
  auto f = std::make_shared<WFont>();
  f->name = name;
  if (!ctx.opt.loadWorldFont || !ctx.opt.loadWorldFont(name, f->data) || !f->parse() ||
      FT_New_Memory_Face(cache.lib.lib, reinterpret_cast<const FT_Byte*>(f->data.data()),
                         static_cast<FT_Long>(f->data.size()), 0, &f->face) != 0 ||
      FT_Select_Charmap(f->face, FT_ENCODING_UNICODE) != 0) {
    return nullptr;
  }
  f->blob = hb_blob_create(f->data.data(), static_cast<unsigned>(f->data.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  f->hbFace = hb_face_create(f->blob, 0);
  f->hbFont = hb_font_create(f->hbFace);
  cache.fonts[name] = f;
  return f;
}

std::shared_ptr<WFont> fontFor(Ctx& ctx, WorldCache& cache, uint32_t cp, bool bold, bool serif) {
  std::vector<std::string> order;
  std::string fam = familyFor(cp, serif);
  order.push_back(fam);
  if (fam != "NotoSans") order.push_back("NotoSans");
  for (const char* f : kFamilies) order.push_back(f);
  for (const std::string& fm : order) {
    std::shared_ptr<WFont> f = bold ? loadFont(ctx, cache, fm + "-Bold") : nullptr;
    if (!f) f = loadFont(ctx, cache, fm + "-Regular");
    if (f && f->gidFor(cp)) return f;
  }
  return nullptr;
}

std::vector<uint32_t> utf16HexToCodepoints(const std::string& hex) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i + 4 <= hex.size(); i += 4) {
    uint32_t u = static_cast<uint32_t>(std::strtoul(hex.substr(i, 4).c_str(), nullptr, 16));
    if (u >= 0xD800 && u <= 0xDBFF && i + 8 <= hex.size()) {
      uint32_t lo = static_cast<uint32_t>(std::strtoul(hex.substr(i + 4, 4).c_str(), nullptr, 16));
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        out.push_back(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
        i += 4;
        continue;
      }
    }
    out.push_back(u);
  }
  return out;
}

std::string codepointsToUtf16Hex(const std::vector<uint32_t>& cps) {
  std::string out;
  char buf[16];
  for (uint32_t cp : cps) {
    if (cp > 0xFFFF) {
      uint32_t v = cp - 0x10000;
      std::snprintf(buf, sizeof(buf), "%04X%04X", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
    } else {
      std::snprintf(buf, sizeof(buf), "%04X", cp);
    }
    out += buf;
  }
  return out;
}

class RunScan : public QPDFObjectHandle::ParserCallbacks {
 public:
  RunScan(std::map<QPDFObjGen, std::vector<std::vector<uint32_t>>>& out, QPDFObjectHandle res,
          std::set<QPDFObjGen>& active, int depth, QPDFObjectHandle font)
      : out(out), res(res), active(active), depth(depth), font(font) {}
  void handleObject(QPDFObjectHandle o, size_t, size_t) override {
    if (!o.isOperator()) {
      ops.push_back(o);
      return;
    }
    std::string op = o.getOperatorValue();
    if (op == "q") {
      stack.push_back(font);
    } else if (op == "Q") {
      if (!stack.empty()) {
        font = stack.back();
        stack.pop_back();
      }
    } else if (op == "Tf" && ops.size() >= 2 && ops[ops.size() - 2].isName()) {
      QPDFObjectHandle fd = res.isDictionary() ? res.getKey("/Font") : QPDFObjectHandle::newNull();
      font = fd.isDictionary() ? fd.getKey(ops[ops.size() - 2].getName()) : QPDFObjectHandle::newNull();
    } else if ((op == "Tj" || op == "'" || op == "\"") && !ops.empty() && ops.back().isString()) {
      add(ops.back().getStringValue());
    } else if (op == "TJ" && !ops.empty() && ops.back().isArray()) {
      std::string all;
      QPDFObjectHandle a = ops.back();
      for (int i = 0; i < a.getArrayNItems(); ++i)
        if (a.getArrayItem(i).isString()) all += a.getArrayItem(i).getStringValue();
      add(all);
    } else if (op == "Do" && !ops.empty() && ops.back().isName() && depth < 12) {
      QPDFObjectHandle xod = res.isDictionary() ? res.getKey("/XObject") : QPDFObjectHandle::newNull();
      QPDFObjectHandle x = xod.isDictionary() ? xod.getKey(ops.back().getName()) : QPDFObjectHandle::newNull();
      if (x.isStream() && nameIs(x.getDict().getKey("/Subtype"), "/Form") &&
          (!x.isIndirect() || active.insert(x.getObjGen()).second)) {
        QPDFObjectHandle fr = x.getDict().getKey("/Resources");
        RunScan sub(out, fr.isDictionary() ? fr : res, active, depth + 1, font);
        try {
          QPDFObjectHandle::parseContentStream(x, &sub);
        } catch (...) {
        }
        if (x.isIndirect()) active.erase(x.getObjGen());
      }
    }
    ops.clear();
  }
  void handleEOF() override {}

 private:
  void add(const std::string& bytes) {
    if (!font.isDictionary() || !font.isIndirect() || bytes.empty()) return;
    bool two = nameIs(font.getKey("/Subtype"), "/Type0");
    std::vector<uint32_t> codes;
    if (two) {
      for (size_t i = 0; i + 1 < bytes.size(); i += 2)
        codes.push_back((static_cast<unsigned char>(bytes[i]) << 8) | static_cast<unsigned char>(bytes[i + 1]));
    } else {
      for (unsigned char c : bytes) codes.push_back(c);
    }
    out[font.getObjGen()].push_back(std::move(codes));
  }
  std::map<QPDFObjGen, std::vector<std::vector<uint32_t>>>& out;
  QPDFObjectHandle res;
  std::set<QPDFObjGen>& active;
  int depth;
  QPDFObjectHandle font;
  std::vector<QPDFObjectHandle> ops, stack;
};

void collectRuns(Ctx& ctx) {
  if (ctx.textRunsReady) return;
  ctx.textRunsReady = true;
  std::set<QPDFObjGen> active;
  for (auto& ph : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) {
    QPDFObjectHandle res = ph.getAttribute("/Resources", false);
    RunScan scan(ctx.textRuns, res, active, 0, QPDFObjectHandle::newNull());
    try {
      ph.parseContents(&scan);
    } catch (...) {
    }
    QPDFObjectHandle annots = ph.getObjectHandle().getKey("/Annots");
    for (int i = 0; annots.isArray() && i < annots.getArrayNItems(); ++i) {
      QPDFObjectHandle a = annots.getArrayItem(i);
      if (!a.isDictionary()) continue;
      for (QPDFObjectHandle& st : normalAppearanceStreams(a)) {
        QPDFObjectHandle r = st.getDict().getKey("/Resources");
        RunScan s2(ctx.textRuns, r.isDictionary() ? r : res, active, 1, QPDFObjectHandle::newNull());
        try {
          QPDFObjectHandle::parseContentStream(st, &s2);
        } catch (...) {
        }
      }
    }
  }
}

struct Shaped {
  uint32_t gid;
  int32_t x, y;
};

std::vector<Shaped> shapePlaced(WFont& f, const std::vector<uint32_t>& text) {
  hb_buffer_t* buf = hb_buffer_create();
  hb_buffer_add_utf32(buf, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
  hb_buffer_guess_segment_properties(buf);
  hb_buffer_set_flags(buf, static_cast<hb_buffer_flags_t>(HB_BUFFER_FLAG_DO_NOT_INSERT_DOTTED_CIRCLE |
                                                          HB_BUFFER_FLAG_BOT | HB_BUFFER_FLAG_EOT));
  hb_shape(f.hbFont, buf, nullptr, 0);
  unsigned n = 0;
  hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
  hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
  std::vector<Shaped> out;
  int32_t pen = 0;
  for (unsigned i = 0; i < n; ++i) {
    out.push_back({info[i].codepoint, pen + pos[i].x_offset, pos[i].y_offset});
    pen += pos[i].x_advance;
  }
  hb_buffer_destroy(buf);
  return out;
}

std::vector<hb_codepoint_t> shapeAlone(WFont& f, const std::vector<uint32_t>& text) {
  hb_buffer_t* buf = hb_buffer_create();
  hb_buffer_add_utf32(buf, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
  hb_buffer_guess_segment_properties(buf);
  hb_buffer_set_flags(buf, static_cast<hb_buffer_flags_t>(HB_BUFFER_FLAG_DO_NOT_INSERT_DOTTED_CIRCLE |
                                                          HB_BUFFER_FLAG_BOT | HB_BUFFER_FLAG_EOT));
  hb_shape(f.hbFont, buf, nullptr, 0);
  unsigned n = 0;
  hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
  std::vector<hb_codepoint_t> out;
  for (unsigned i = 0; i < n; ++i) out.push_back(info[i].codepoint);
  hb_buffer_destroy(buf);
  return out;
}

struct Choice {
  std::shared_ptr<WFont> font;
  uint32_t gid = 0;
  std::vector<Shaped> parts;
};

std::string checksumPad(std::string t) {
  while (t.size() % 4) t += '\0';
  return t;
}
uint32_t tableSum(const std::string& t) {
  std::string p = checksumPad(t);
  uint32_t s = 0;
  for (size_t i = 0; i < p.size(); i += 4) s += rd32(p, i);
  return s;
}

std::string writeFont(std::map<std::string, std::string>& tables) {
  std::vector<std::string> order;
  for (const auto& kv : tables) order.push_back(kv.first);
  uint16_t n = static_cast<uint16_t>(order.size());
  uint16_t pow2 = 1, lg = 0;
  while (pow2 * 2 <= n) {
    pow2 *= 2;
    ++lg;
  }
  std::string out;
  put32(out, 0x00010000);
  put16(out, n);
  put16(out, static_cast<uint16_t>(pow2 * 16));
  put16(out, lg);
  put16(out, static_cast<uint16_t>(n * 16 - pow2 * 16));
  out.resize(12 + 16u * n, '\0');
  size_t headAt = 0;
  for (uint16_t i = 0; i < n; ++i) {
    const std::string& t = tables[order[i]];
    size_t rec = 12 + 16u * i;
    out.replace(rec, 4, order[i]);
    wr32(out, rec + 4, tableSum(t));
    wr32(out, rec + 8, static_cast<uint32_t>(out.size()));
    wr32(out, rec + 12, static_cast<uint32_t>(t.size()));
    if (order[i] == "head") headAt = out.size();
    out += checksumPad(t);
  }
  wr32(out, headAt + 8, 0xB1B0AFBAu - tableSum(out));
  return out;
}

std::string stripInstructions(const std::string& g) {
  if (g.size() < 12) return g;
  int16_t contours = static_cast<int16_t>(rd16(g, 0));
  if (contours < 0) {
    size_t p = 10;
    bool more = true, instr = false;
    while (more && p + 4 <= g.size()) {
      uint16_t flags = rd16(g, p);
      p += 4 + ((flags & 1) ? 4 : 2);
      if (flags & 8) p += 2;
      else if (flags & 0x40) p += 4;
      else if (flags & 0x80) p += 8;
      more = (flags & 0x20) != 0;
      instr = instr || (flags & 0x100);
    }
    if (!instr || p > g.size()) return g;
    std::string out = g.substr(0, p);
    size_t q = 10;
    more = true;
    while (more && q + 4 <= out.size()) {
      uint16_t flags = rd16(out, q);
      wr16(out, q, static_cast<uint16_t>(flags & ~0x100));
      q += 4 + ((flags & 1) ? 4 : 2);
      if (flags & 8) q += 2;
      else if (flags & 0x40) q += 4;
      else if (flags & 0x80) q += 8;
      more = (flags & 0x20) != 0;
    }
    return out;
  }
  size_t p = 10 + 2u * static_cast<size_t>(contours);
  if (p + 2 > g.size()) return g;
  uint16_t len = rd16(g, p);
  if (!len || p + 2 + len > g.size()) return g;
  std::string out = g.substr(0, p);
  put16(out, 0);
  out += g.substr(p + 2 + len);
  return out;
}

struct Built {
  std::string program;
  std::map<uint32_t, uint16_t> codeGid;
};

Built buildFont(WFont& base, const std::vector<std::pair<uint32_t, Choice>>& items,
                const std::map<uint32_t, double>& widths, bool symbolicCmap) {
  Built b;
  struct Slot {
    WFont* font;
    uint32_t gid;
    int advance;
  };
  std::vector<Slot> slots{{&base, 0, base.advance(0)}};
  std::map<std::pair<WFont*, uint32_t>, uint16_t> components;
  std::map<size_t, const std::vector<Shaped>*> composed;
  std::map<size_t, double> scaleOf;
  std::vector<std::unique_ptr<std::vector<Shaped>>> single;
  for (const auto& it : items) {
    auto w = widths.find(it.first);
    int own = it.second.parts.empty() ? it.second.font->advance(it.second.gid) : 0;
    if (!it.second.parts.empty()) {
      const Shaped& last = it.second.parts.back();
      own = last.x + it.second.font->advance(last.gid);
    }
    int adv = w != widths.end() ? static_cast<int>(std::lround(std::max(0.0, w->second))) : own;
    size_t slot = slots.size();
    b.codeGid[it.first] = static_cast<uint16_t>(slot);
    if (!it.second.parts.empty()) composed[slot] = &it.second.parts;
    if (w != widths.end() && adv > 0 && own > 0) {
      double ratio = static_cast<double>(adv) / own;
      if ((ratio < 0.97 || (ratio > 1.03 && ratio <= 1.15)) && ratio >= 0.3) {
        scaleOf[slot] = ratio;
        if (!composed.count(slot)) {
          single.push_back(std::make_unique<std::vector<Shaped>>(std::vector<Shaped>{{it.second.gid, 0, 0}}));
          composed[slot] = single.back().get();
        }
      }
    }
    slots.push_back({it.second.font.get(), it.second.gid, adv});
  }
  auto componentSlot = [&](WFont* f, uint32_t g) {
    auto key = std::make_pair(f, g);
    auto c = components.find(key);
    if (c != components.end()) return c->second;
    uint16_t ng = static_cast<uint16_t>(slots.size());
    components[key] = ng;
    slots.push_back({f, g, f->advance(g)});
    return ng;
  };
  std::string glyf, loca;
  uint16_t maxPts = 0, maxCont = 0, maxCompPts = 0, maxCompCont = 0, maxElems = 0, maxDepth = 0;
  for (size_t i = 0; i < slots.size() && slots.size() < 65000; ++i) {
    std::string g;
    auto comp = composed.find(i);
    if (comp != composed.end()) {
      int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
      std::string body;
      const std::vector<Shaped>& parts = *comp->second;
      auto sc = scaleOf.find(i);
      double sx = sc != scaleOf.end() ? sc->second : 1.0;
      for (size_t k = 0; k < parts.size(); ++k) {
        WFont* f = slots[i].font;
        std::string cg = f->glyph(parts[k].gid);
        int32_t px = static_cast<int32_t>(std::lround(parts[k].x * sx));
        if (cg.size() >= 10) {
          x0 = std::min<int32_t>(x0, static_cast<int32_t>(std::lround(static_cast<int16_t>(rd16(cg, 2)) * sx)) + px);
          y0 = std::min<int32_t>(y0, static_cast<int16_t>(rd16(cg, 4)) + parts[k].y);
          x1 = std::max<int32_t>(x1, static_cast<int32_t>(std::lround(static_cast<int16_t>(rd16(cg, 6)) * sx)) + px);
          y1 = std::max<int32_t>(y1, static_cast<int16_t>(rd16(cg, 8)) + parts[k].y);
        }
        uint16_t flags = 0x0001 | 0x0002 | (k + 1 < parts.size() ? 0x0020 : 0) | (sx != 1.0 ? 0x0040 : 0);
        put16(body, flags);
        put16(body, componentSlot(f, parts[k].gid));
        put16(body, static_cast<uint16_t>(static_cast<int16_t>(std::clamp<int32_t>(px, -32768, 32767))));
        put16(body, static_cast<uint16_t>(static_cast<int16_t>(std::clamp<int32_t>(parts[k].y, -32768, 32767))));
        if (sx != 1.0) {
          put16(body, static_cast<uint16_t>(static_cast<int16_t>(std::lround(sx * 16384.0))));
          put16(body, 0x4000);
        }
      }
      if (x0 > x1) x0 = y0 = x1 = y1 = 0;
      put16(g, 0xFFFF);
      for (int32_t v : {x0, y0, x1, y1}) put16(g, static_cast<uint16_t>(static_cast<int16_t>(std::clamp<int32_t>(v, -32768, 32767))));
      g += body;
      maxElems = std::max<uint16_t>(maxElems, static_cast<uint16_t>(parts.size()));
      maxDepth = std::max<uint16_t>(maxDepth, 2);
      put32(loca, static_cast<uint32_t>(glyf.size()));
      glyf += g;
      while (glyf.size() % 4) glyf += '\0';
      continue;
    }
    g = stripInstructions(slots[i].font->glyph(slots[i].gid));
    if (g.size() >= 10 && static_cast<int16_t>(rd16(g, 0)) < 0) {
      size_t p = 10;
      bool more = true;
      uint16_t elems = 0;
      while (more && p + 4 <= g.size()) {
        uint16_t flags = rd16(g, p);
        uint16_t compGid = rd16(g, p + 2);
        wr16(g, p + 2, componentSlot(slots[i].font, compGid));
        ++elems;
        p += 4 + ((flags & 1) ? 4 : 2);
        if (flags & 8) p += 2;
        else if (flags & 0x40) p += 4;
        else if (flags & 0x80) p += 8;
        more = (flags & 0x20) != 0;
      }
      maxElems = std::max(maxElems, elems);
      maxDepth = std::max<uint16_t>(maxDepth, 2);
    }
    put32(loca, static_cast<uint32_t>(glyf.size()));
    glyf += g;
    while (glyf.size() % 4) glyf += '\0';
  }
  put32(loca, static_cast<uint32_t>(glyf.size()));
  std::set<WFont*> used;
  for (const Slot& s : slots) used.insert(s.font);
  for (WFont* f : used) {
    const std::string& m = f->tables["maxp"];
    if (m.size() >= 14) {
      maxPts = std::max(maxPts, rd16(m, 6));
      maxCont = std::max(maxCont, rd16(m, 8));
      maxCompPts = std::max(maxCompPts, rd16(m, 10));
      maxCompCont = std::max(maxCompCont, rd16(m, 12));
    }
  }
  uint16_t n = static_cast<uint16_t>(slots.size());
  std::string hmtx;
  uint16_t maxAdv = 0;
  for (const Slot& s : slots) {
    uint16_t a = static_cast<uint16_t>(std::clamp(s.advance, 0, 65535));
    maxAdv = std::max(maxAdv, a);
    put16(hmtx, a);
    put16(hmtx, static_cast<uint16_t>(s.font->lsb(s.gid)));
  }
  std::map<std::string, std::string> t;
  t["head"] = base.tables["head"];
  wr32(t["head"], 8, 0);
  wr16(t["head"], 50, 1);
  t["hhea"] = base.tables["hhea"];
  wr16(t["hhea"], 10, maxAdv);
  wr16(t["hhea"], 34, n);
  std::string maxp(32, '\0');
  wr32(maxp, 0, 0x00010000);
  wr16(maxp, 4, n);
  wr16(maxp, 6, maxPts);
  wr16(maxp, 8, maxCont);
  wr16(maxp, 10, maxCompPts);
  wr16(maxp, 12, maxCompCont);
  wr16(maxp, 14, 2);
  wr16(maxp, 26, maxElems);
  wr16(maxp, 28, maxDepth);
  t["maxp"] = maxp;
  t["hmtx"] = hmtx;
  t["loca"] = loca;
  t["glyf"] = glyf;
  std::string post(32, '\0');
  wr32(post, 0, 0x00030000);
  t["post"] = post;
  if (base.tables.count("OS/2")) t["OS/2"] = base.tables["OS/2"];
  if (base.tables.count("name")) t["name"] = base.tables["name"];
  if (symbolicCmap) {
    std::vector<std::pair<uint16_t, uint16_t>> map;
    for (const auto& kv : b.codeGid) map.push_back({static_cast<uint16_t>(0xF000 + kv.first), kv.second});
    std::string sub;
    uint16_t segs = static_cast<uint16_t>(map.size() + 1);
    put16(sub, 4);
    put16(sub, 0);
    put16(sub, 0);
    put16(sub, static_cast<uint16_t>(segs * 2));
    uint16_t sr = 1, es = 0;
    while (sr * 2 <= segs) {
      sr = static_cast<uint16_t>(sr * 2);
      ++es;
    }
    put16(sub, static_cast<uint16_t>(sr * 2));
    put16(sub, es);
    put16(sub, static_cast<uint16_t>(segs * 2 - sr * 2));
    for (const auto& m : map) put16(sub, m.first);
    put16(sub, 0xFFFF);
    put16(sub, 0);
    for (const auto& m : map) put16(sub, m.first);
    put16(sub, 0xFFFF);
    for (const auto& m : map) put16(sub, static_cast<uint16_t>(m.second - m.first));
    put16(sub, 1);
    for (size_t i = 0; i < segs; ++i) put16(sub, 0);
    wr16(sub, 2, static_cast<uint16_t>(sub.size()));
    std::string cmap;
    put16(cmap, 0);
    put16(cmap, 1);
    put16(cmap, 3);
    put16(cmap, 0);
    put32(cmap, 12);
    cmap += sub;
    t["cmap"] = cmap;
  }
  b.program = writeFont(t);
  return b;
}

std::string tagFor(const std::string& salt) {
  uint32_t h = 2166136261u;
  for (char c : salt) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
  std::string tag;
  for (int i = 0; i < 6; ++i) {
    tag += static_cast<char>('A' + h % 26);
    h /= 26;
    if (!h) h = 2654435761u;
  }
  return tag;
}
}  // namespace

bool loadWorldFontFromFolder(const std::string& folder, const std::string& name, std::string& bytes) {
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name.find("..") != std::string::npos)
    return false;
  std::ifstream in(std::filesystem::path(folder) / (name + ".ttf"), std::ios::binary);
  if (!in) return false;
  bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return bytes.size() > 64;
}

bool embedWorldSubstitute(Ctx& ctx, QPDFObjectHandle font) {
  if (!ctx.opt.loadWorldFont || !font.isIndirect()) return false;
  static thread_local WorldCache cache;
  bool type0 = nameIs(font.getKey("/Subtype"), "/Type0");
  QPDFObjectHandle target = font;
  if (type0) {
    std::string enc = nameOf(font.getKey("/Encoding"));
    if (enc != "/Identity-H" && enc != "/Identity-V") return false;
    QPDFObjectHandle df = font.getKey("/DescendantFonts");
    if (!df.isArray() || df.getArrayNItems() != 1 || !df.getArrayItem(0).isDictionary()) return false;
    target = df.getArrayItem(0);
  }
  QPDFObjectHandle fd = target.getKey("/FontDescriptor");
  std::string baseName = nameOf(font.getKey("/BaseFont"));
  std::string lower = baseName;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  long long flags = fd.isDictionary() && fd.getKey("/Flags").isInteger() ? fd.getKey("/Flags").getIntValue() : 0;
  bool bold = lower.find("bold") != std::string::npos || lower.find("black") != std::string::npos ||
              (flags & (1LL << 18)) != 0 || (fd.isDictionary() && numOf(fd.getKey("/StemV"), 0) >= 139);
  bool serif = lower.find("times") != std::string::npos || lower.find("serif") != std::string::npos ||
               lower.find("roman") != std::string::npos || lower.find("mincho") != std::string::npos;

  collectRuns(ctx);
  auto runIt = ctx.textRuns.find(font.getObjGen());
  std::set<uint32_t> used;
  if (runIt != ctx.textRuns.end())
    for (const auto& run : runIt->second) used.insert(run.begin(), run.end());

  std::map<uint32_t, std::vector<uint32_t>> text;
  std::map<uint32_t, std::string> tu = worldToUnicodeMap(font.getKey("/ToUnicode"));
  SimpleEncoding enc;
  if (!type0) enc = readEncoding(font, (flags & 4) && !(flags & 32));
  for (uint32_t code : used) {
    auto t = tu.find(code);
    if (t != tu.end()) {
      text[code] = utf16HexToCodepoints(t->second);
      continue;
    }
    if (type0) return false;
    uint32_t u = 0;
    const std::string& dn = enc.diffs[code];
    if (!dn.empty()) {
      std::string bare = dn.substr(1);
      u = aglNameToUnicode(bare);
      if (!u && bare.size() == 7 && bare.compare(0, 3, "uni") == 0) u = static_cast<uint32_t>(std::strtoul(bare.c_str() + 3, nullptr, 16));
      if (!u && bare.size() >= 5 && bare[0] == 'u') u = static_cast<uint32_t>(std::strtoul(bare.c_str() + 1, nullptr, 16));
    } else if (enc.base) {
      u = enc.base(static_cast<int>(code));
    } else {
      u = winAnsiToUnicode(static_cast<int>(code));
    }
    if (!u) return false;
    text[code] = {u};
  }
  if (text.empty()) return false;

  std::shared_ptr<WFont> base = loadFont(ctx, cache, serif ? "NotoSerif-Regular" : "NotoSans-Regular");
  if (!base) base = loadFont(ctx, cache, "NotoSans-Regular");
  if (!base) return false;

  std::map<uint32_t, std::map<std::pair<WFont*, uint32_t>, int>> votes;
  std::map<WFont*, std::shared_ptr<WFont>> owners;
  if (runIt != ctx.textRuns.end()) {
    for (const auto& run : runIt->second) {
      bool joining = false;
      for (uint32_t c : run) {
        auto t = text.find(c);
        if (t != text.end() && !t->second.empty() && joiningScript(t->second[0])) joining = true;
      }
      if (!joining) continue;
      std::vector<uint32_t> logical(run.rbegin(), run.rend());
      std::vector<uint32_t> buf;
      std::vector<unsigned> starts;
      std::shared_ptr<WFont> f;
      for (uint32_t c : logical) {
        starts.push_back(static_cast<unsigned>(buf.size()));
        const auto& cps = text[c];
        if (!f) {
          for (uint32_t cp : cps)
            if (joiningScript(cp)) f = fontFor(ctx, cache, cp, bold, serif);
        }
        buf.insert(buf.end(), cps.begin(), cps.end());
      }
      if (!f || buf.empty()) continue;
      owners[f.get()] = f;
      hb_buffer_t* hb = hb_buffer_create();
      hb_buffer_add_utf32(hb, buf.data(), static_cast<int>(buf.size()), 0, static_cast<int>(buf.size()));
      hb_buffer_guess_segment_properties(hb);
      hb_buffer_set_flags(hb, static_cast<hb_buffer_flags_t>(HB_BUFFER_FLAG_DO_NOT_INSERT_DOTTED_CIRCLE |
                                                             HB_BUFFER_FLAG_BOT | HB_BUFFER_FLAG_EOT));
      hb_shape(f->hbFont, hb, nullptr, 0);
      unsigned n = 0;
      hb_glyph_info_t* info = hb_buffer_get_glyph_infos(hb, &n);
      std::map<unsigned, std::vector<uint32_t>> byCluster;
      for (unsigned i = 0; i < n; ++i) byCluster[info[i].cluster].push_back(info[i].codepoint);
      hb_buffer_destroy(hb);
      for (size_t k = 0; k < logical.size(); ++k) {
        auto bc = byCluster.find(starts[k]);
        if (bc == byCluster.end() || bc->second.size() != 1 || !bc->second[0]) continue;
        unsigned next = k + 1 < starts.size() ? starts[k + 1] : static_cast<unsigned>(buf.size());
        bool merged = false;
        for (const auto& kv : byCluster)
          if (kv.first > starts[k] && kv.first < next) merged = true;
        if (merged) continue;
        ++votes[logical[k]][{f.get(), bc->second[0]}];
      }
    }
  }

  std::vector<std::pair<uint32_t, Choice>> items;
  std::set<std::string> scripts;
  for (const auto& kv : text) {
    uint32_t code = kv.first;
    const std::vector<uint32_t>& cps = kv.second;
    Choice ch;
    auto v = votes.find(code);
    if (v != votes.end() && !v->second.empty()) {
      auto best = std::max_element(v->second.begin(), v->second.end(),
                                   [](const auto& a, const auto& b2) { return a.second < b2.second; });
      ch.font = owners[best->first.first];
      ch.gid = best->first.second;
    } else {
      bool blank = std::all_of(cps.begin(), cps.end(), blankChar);
      if (cps.empty() || blank) {
        ch.font = base;
        ch.gid = base->gidFor(0x20);
      } else {
        uint32_t lead = cps[0];
        ch.font = fontFor(ctx, cache, lead, bold, serif);
        if (!ch.font) return false;
        if (cps.size() == 1) {
          ch.gid = ch.font->gidFor(lead);
          if (joiningScript(lead) || (lead >= 0x0900 && lead <= 0x0DFF)) {
            std::vector<hb_codepoint_t> g = shapeAlone(*ch.font, cps);
            if (g.size() == 1 && g[0]) ch.gid = g[0];
          }
        } else {
          static const std::map<std::vector<uint32_t>, uint32_t> kLig = {
              {{0x66, 0x66}, 0xFB00}, {{0x66, 0x69}, 0xFB01}, {{0x66, 0x6C}, 0xFB02},
              {{0x66, 0x66, 0x69}, 0xFB03}, {{0x66, 0x66, 0x6C}, 0xFB04}};
          auto lg = kLig.find(cps);
          std::vector<Shaped> g = shapePlaced(*ch.font, cps);
          if (lg != kLig.end() && ch.font->gidFor(lg->second)) {
            ch.gid = ch.font->gidFor(lg->second);
          } else if (g.size() == 1 && g[0].gid) {
            ch.gid = g[0].gid;
          } else {
            bool viramaEnd = cps.size() == 2 && lead >= 0x0900 && lead <= 0x0DFF;
            if (viramaEnd) {
              uint32_t blockBase = lead & ~0x7Fu;
              std::vector<uint32_t> ctxText = cps;
              ctxText.push_back(blockBase + 0x15);
              std::vector<Shaped> withBase = shapePlaced(*ch.font, ctxText);
              uint32_t baseGid = ch.font->gidFor(blockBase + 0x15);
              std::vector<Shaped> rest;
              for (const Shaped& sh : withBase)
                if (sh.gid != baseGid) rest.push_back(sh);
              if (rest.size() == 1 && rest[0].gid) {
                ch.gid = rest[0].gid;
                g.clear();
              }
            }
            if (!ch.gid) {
              if (g.empty()) return false;
              for (const Shaped& sh : g)
                if (!sh.gid) return false;
              ch.gid = g[0].gid;
              ch.parts = g;
            }
          }
        }
        if (!ch.gid) return false;
      }
    }
    if (ch.font != base) scripts.insert(ch.font->name.substr(0, ch.font->name.find('-')));
    items.push_back({code, ch});
  }

  std::map<uint32_t, double> widths;
  bool haveWidths = false;
  if (type0) {
    double dw = target.getKey("/DW").isNumber() ? target.getKey("/DW").getNumericValue() : 1000.0;
    std::map<uint32_t, double> w;
    QPDFObjectHandle wa = target.getKey("/W");
    for (int i = 0; wa.isArray() && i + 1 < wa.getArrayNItems();) {
      QPDFObjectHandle a = wa.getArrayItem(i), b2 = wa.getArrayItem(i + 1);
      if (!a.isInteger()) break;
      long long c0 = a.getIntValue();
      if (b2.isArray()) {
        for (int k = 0; k < b2.getArrayNItems(); ++k)
          if (b2.getArrayItem(k).isNumber()) w[static_cast<uint32_t>(c0 + k)] = b2.getArrayItem(k).getNumericValue();
        i += 2;
      } else if (b2.isInteger() && i + 2 < wa.getArrayNItems() && wa.getArrayItem(i + 2).isNumber()) {
        for (long long c = c0; c <= b2.getIntValue() && c - c0 < 65536; ++c)
          w[static_cast<uint32_t>(c)] = wa.getArrayItem(i + 2).getNumericValue();
        i += 3;
      } else {
        break;
      }
    }
    for (const auto& it : items) {
      auto f = w.find(it.first);
      widths[it.first] = f != w.end() ? f->second : dw;
    }
    haveWidths = true;
  } else if (font.getKey("/Widths").isArray() && font.getKey("/FirstChar").isInteger()) {
    QPDFObjectHandle wa = font.getKey("/Widths");
    long long first = font.getKey("/FirstChar").getIntValue();
    double missingW = fd.isDictionary() ? numOf(fd.getKey("/MissingWidth"), 0) : 0;
    for (const auto& it : items) {
      long long i = static_cast<long long>(it.first) - first;
      widths[it.first] = i >= 0 && i < wa.getArrayNItems() && wa.getArrayItem(static_cast<int>(i)).isNumber()
                             ? wa.getArrayItem(static_cast<int>(i)).getNumericValue()
                             : missingW;
    }
    haveWidths = true;
  }

  Built built = buildFont(*base, items, widths, !type0);
  if (built.program.empty()) return false;

  std::string tag = tagFor(baseName + std::to_string(items.size()) + std::to_string(built.program.size()));
  std::string psName = tag + "+Kura" + (serif ? "Serif" : "Sans") + "World";
  const std::string& head = base->tables["head"];
  const std::string& hhea = base->tables["hhea"];
  QPDFObjectHandle nfd = QPDFObjectHandle::newDictionary();
  nfd.replaceKey("/Type", QPDFObjectHandle::newName("/FontDescriptor"));
  nfd.replaceKey("/FontName", QPDFObjectHandle::newName("/" + psName));
  nfd.replaceKey("/Flags", QPDFObjectHandle::newInteger(4));
  QPDFObjectHandle bbox = QPDFObjectHandle::newArray();
  for (size_t o : {36u, 38u, 40u, 42u})
    bbox.appendItem(QPDFObjectHandle::newInteger(static_cast<int16_t>(rd16(head, o))));
  nfd.replaceKey("/FontBBox", bbox);
  nfd.replaceKey("/ItalicAngle", QPDFObjectHandle::newInteger(0));
  nfd.replaceKey("/Ascent", QPDFObjectHandle::newInteger(static_cast<int16_t>(rd16(hhea, 4))));
  nfd.replaceKey("/Descent", QPDFObjectHandle::newInteger(static_cast<int16_t>(rd16(hhea, 6))));
  nfd.replaceKey("/CapHeight", QPDFObjectHandle::newInteger(714));
  nfd.replaceKey("/StemV", QPDFObjectHandle::newInteger(bold ? 150 : 80));
  QPDFObjectHandle prog = QPDFObjectHandle::newStream(&ctx.pdf, built.program);
  prog.getDict().replaceKey("/Length1", QPDFObjectHandle::newInteger(static_cast<long long>(built.program.size())));
  nfd.replaceKey("/FontFile2", ctx.pdf.makeIndirectObject(prog));

  std::map<uint32_t, std::string> toUni;
  for (const auto& kv : text)
    if (!kv.second.empty()) toUni[kv.first] = codepointsToUtf16Hex(kv.second);

  if (type0) {
    uint32_t maxCode = built.codeGid.empty() ? 0 : built.codeGid.rbegin()->first;
    std::string map((static_cast<size_t>(maxCode) + 1) * 2, '\0');
    for (const auto& kv : built.codeGid) {
      map[kv.first * 2] = static_cast<char>(kv.second >> 8);
      map[kv.first * 2 + 1] = static_cast<char>(kv.second & 0xFF);
    }
    target.replaceKey("/Subtype", QPDFObjectHandle::newName("/CIDFontType2"));
    target.replaceKey("/BaseFont", QPDFObjectHandle::newName("/" + psName));
    target.replaceKey("/FontDescriptor", ctx.pdf.makeIndirectObject(nfd));
    QPDFObjectHandle si = QPDFObjectHandle::newDictionary();
    si.replaceKey("/Registry", QPDFObjectHandle::newString("Adobe"));
    si.replaceKey("/Ordering", QPDFObjectHandle::newString("Identity"));
    si.replaceKey("/Supplement", QPDFObjectHandle::newInteger(0));
    target.replaceKey("/CIDSystemInfo", si);
    target.replaceKey("/CIDToGIDMap", ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, map)));
    if (!haveWidths) target.replaceKey("/DW", QPDFObjectHandle::newInteger(1000));
    font.replaceKey("/BaseFont", QPDFObjectHandle::newName("/" + psName));
    if (!font.getKey("/ToUnicode").isStream())
      font.replaceKey("/ToUnicode", ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, worldToUnicodeCMap(toUni, true))));
  } else {
    font.replaceKey("/Subtype", QPDFObjectHandle::newName("/TrueType"));
    font.replaceKey("/BaseFont", QPDFObjectHandle::newName("/" + psName));
    font.removeKey("/Encoding");
    font.replaceKey("/FontDescriptor", ctx.pdf.makeIndirectObject(nfd));
    if (!haveWidths) {
      uint32_t lo = built.codeGid.begin()->first, hi = built.codeGid.rbegin()->first;
      QPDFObjectHandle wa = QPDFObjectHandle::newArray();
      for (uint32_t c = lo; c <= hi; ++c) {
        auto it = std::find_if(items.begin(), items.end(), [&](const auto& p) { return p.first == c; });
        wa.appendItem(QPDFObjectHandle::newInteger(it != items.end() ? it->second.font->advance(it->second.gid) : 0));
      }
      font.replaceKey("/FirstChar", QPDFObjectHandle::newInteger(lo));
      font.replaceKey("/LastChar", QPDFObjectHandle::newInteger(hi));
      font.replaceKey("/Widths", wa);
    }
    font.replaceKey("/ToUnicode", ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, worldToUnicodeCMap(toUni, false))));
  }
  std::string list;
  for (const std::string& s : scripts) list += (list.empty() ? "" : ", ") + s;
  ctx.issue("FONT_SUBSTITUTED",
            "embedded " + std::to_string(items.size()) + " glyph(s) from the world font pack" +
                (list.empty() ? std::string() : " (" + list + ")") + " as substitute for " + baseName +
                ", keeping the original character widths",
            true);
  return true;
}
}  // namespace pdfa
