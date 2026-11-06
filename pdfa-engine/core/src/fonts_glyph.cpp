#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <ft2build.h>
#include FT_CID_H
#include FT_FREETYPE_H

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "ctx.hh"
#include "encodings.hh"
#include "fonts_ft.hh"
#include "passes.hh"
#include "limits.hh"
#include "util.hh"

namespace pdfa {
namespace {
struct CodespaceRange {
  int bytes;
  uint32_t lo;
  uint32_t hi;
};

struct FontGlyphInfo {
  bool simple = false;
  bool cid2byte = false;
  bool cmapCoded = false;
  std::vector<CodespaceRange> spaces;
  std::set<uint32_t> badCodes;
  std::set<int> bad;
};

bool parseCmapBody(const std::string& text, std::vector<CodespaceRange>& spaces,
                   std::map<uint32_t, int>& code2cid) {
  auto hexAt = [&](size_t& i, uint32_t& val, int& nbytes) -> bool {
    while (i < text.size() && text[i] != '<') {
      if (text[i] == 'e' && text.compare(i, 3, "end") == 0) return false;
      ++i;
    }
    if (i >= text.size()) return false;
    size_t close = text.find('>', i);
    if (close == std::string::npos) return false;
    std::string hex = text.substr(i + 1, close - i - 1);
    i = close + 1;
    if (hex.empty() || hex.size() > 8) return false;
    val = static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
    nbytes = static_cast<int>((hex.size() + 1) / 2);
    return true;
  };
  size_t pos = 0;
  while ((pos = text.find("begincodespacerange", pos)) != std::string::npos) {
    size_t end = text.find("endcodespacerange", pos);
    if (end == std::string::npos) break;
    size_t i = pos + 19;
    while (i < end) {
      uint32_t lo = 0, hi = 0;
      int nb1 = 0, nb2 = 0;
      size_t save = i;
      if (!hexAt(i, lo, nb1) || i > end) { i = save; break; }
      if (!hexAt(i, hi, nb2) || i > end) break;
      spaces.push_back({std::max(nb1, nb2), lo, hi});
    }
    pos = end + 1;
  }
  pos = 0;
  while ((pos = text.find("begincidrange", pos)) != std::string::npos) {
    size_t end = text.find("endcidrange", pos);
    if (end == std::string::npos) break;
    size_t i = pos + 13;
    while (i < end) {
      uint32_t lo = 0, hi = 0;
      int nb = 0, nb2 = 0;
      size_t save = i;
      if (!hexAt(i, lo, nb) || i > end) { i = save; break; }
      if (!hexAt(i, hi, nb2) || i > end) break;
      while (i < end && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
      size_t j = i;
      while (j < end && (std::isdigit(static_cast<unsigned char>(text[j])))) ++j;
      if (j == i) break;
      int cid = std::atoi(text.substr(i, j - i).c_str());
      i = j;
      if (hi < lo || hi - lo > 65535) continue;
      for (uint32_t c = lo; c <= hi; ++c) code2cid[c] = cid + static_cast<int>(c - lo);
    }
    pos = end + 1;
  }
  pos = 0;
  while ((pos = text.find("begincidchar", pos)) != std::string::npos) {
    size_t end = text.find("endcidchar", pos);
    if (end == std::string::npos) break;
    size_t i = pos + 12;
    while (i < end) {
      uint32_t c = 0;
      int nb = 0;
      size_t save = i;
      if (!hexAt(i, c, nb) || i > end) { i = save; break; }
      while (i < end && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
      size_t j = i;
      while (j < end && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
      if (j == i) break;
      code2cid[c] = std::atoi(text.substr(i, j - i).c_str());
      i = j;
    }
    pos = end + 1;
  }
  return !spaces.empty() && !code2cid.empty();
}

const std::set<QPDFObjGen>* ctxIdentityCmaps = nullptr;
bool gStrictNotdef = false;
bool gAllCodes = false;

FT_UInt strictSimpleGid(const FtFace& ftFace, int code, const SimpleEncoding& enc,
                        bool symbolic) {
  FT_Face face = ftFace.face;
  if (symbolic) return resolveSimpleGid(ftFace, code, enc, symbolic);
  const std::string& diffName = enc.diffs[code];
  if (!diffName.empty()) {
    std::string bare = diffName.substr(1);
    FT_UInt gid = FT_HAS_GLYPH_NAMES(face) ? FT_Get_Name_Index(face, bare.c_str()) : 0;
    if (gid) return gid;
    uint32_t uni = aglNameToUnicode(bare);
    if (!uni) parseUniName(bare, uni);
    if (uni && FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0) {
      return FT_Get_Char_Index(face, uni);
    }
    return 0;
  }
  uint16_t uni = enc.base ? enc.base(code) : 0;
  if (!uni) uni = winAnsiToUnicode(code);
  if (uni && FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0) {
    return FT_Get_Char_Index(face, uni);
  }
  return 0;
}

FontGlyphInfo analyzeFontGlyphs(FtLib& lib, QPDFObjectHandle font) {
  FontGlyphInfo info;
  std::string subtype = nameOf(font.getKey("/Subtype"));
  if (subtype == "/Type3") return info;
  if (subtype == "/Type0") {
    QPDFObjectHandle enc = font.getKey("/Encoding");
    bool identity = enc.isName() && (enc.getName() == "/Identity-H" ||
                                     enc.getName() == "/Identity-V");
    if (!identity && enc.isIndirect() && ctxIdentityCmaps &&
        ctxIdentityCmaps->count(enc.getObjGen())) {
      identity = true;
    }
    std::map<uint32_t, int> code2cid;
    if (!identity && enc.isStream()) {
      std::string body;
      try {
        auto buf = enc.getStreamData(qpdf_dl_all);
        body.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
      } catch (...) {
        return info;
      }
      if (!parseCmapBody(body, info.spaces, code2cid)) return info;
      info.cmapCoded = true;
    } else if (!identity) {
      return info;
    }
    QPDFObjectHandle df = font.getKey("/DescendantFonts");
    if (!df.isArray() || df.getArrayNItems() != 1) return info;
    QPDFObjectHandle fd = df.getArrayItem(0).getKey("/FontDescriptor");
    QPDFObjectHandle program = fontFileStream(fd);
    if (!program.isStream()) return info;
    FtFace face;
    if (!loadFace(lib, program, face)) return info;
    long n = face.face->num_glyphs;
    QPDFObjectHandle c2g = df.getArrayItem(0).getKey("/CIDToGIDMap");
    std::string mapData;
    if (c2g.isStream()) {
      try {
        auto buf = c2g.getStreamData(qpdf_dl_all);
        mapData.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
      } catch (...) {
        return info;
      }
    }
    info.cid2byte = true;
    FT_Bool cidKeyed = 0;
    bool exact = !mapData.empty();
    if (mapData.empty() && !FT_Get_CID_Is_Internally_CID_Keyed(face.face, &cidKeyed) &&
        cidKeyed) {
      std::set<int> valid;
      for (long g = 0; g < n; ++g) {
        FT_UInt cid = 0;
        if (!FT_Get_CID_From_Glyph_Index(face.face, static_cast<FT_UInt>(g), &cid)) {
          valid.insert(static_cast<int>(cid));
        }
      }
      if (!valid.empty()) {
        exact = true;
        info.bad.insert(0);
        for (int cid = 1; cid < 65536; ++cid) {
          if (!valid.count(cid)) info.bad.insert(cid);
        }
        if (info.cmapCoded) {
          for (const auto& kv : code2cid) {
            if (info.bad.count(kv.second)) info.badCodes.insert(kv.first);
          }
          info.cid2byte = false;
        }
        return info;
      }
    }
    for (int cid = 0; cid < 65536; ++cid) {
      long gid;
      if (!mapData.empty()) {
        gid = static_cast<size_t>(cid) * 2 + 1 < mapData.size()
                  ? ((static_cast<unsigned char>(mapData[cid * 2]) << 8) |
                     static_cast<unsigned char>(mapData[cid * 2 + 1]))
                  : 0;
      } else {
        gid = cid;
      }

      if (cid == 0 || gid <= 0 || gid >= n) info.bad.insert(cid);
    }
    (void)exact;
    if (info.cmapCoded) {
      for (const auto& kv : code2cid) {
        if (info.bad.count(kv.second)) info.badCodes.insert(kv.first);
      }
      info.cid2byte = false;
    }
    return info;
  }
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  QPDFObjectHandle program = fontFileStream(fd);
  if (!program.isStream()) return info;
  FtFace face;
  if (!loadFace(lib, program, face)) return info;
  long long flags = fd.isDictionary() && fd.getKey("/Flags").isInteger()
                        ? fd.getKey("/Flags").getIntValue()
                        : 0;
  bool symbolic = (flags & 4) && !(flags & 32);
  SimpleEncoding enc = readEncoding(font, symbolic);
  int first = font.getKey("/FirstChar").isInteger()
                  ? static_cast<int>(font.getKey("/FirstChar").getIntValue())
                  : 0;
  int last = font.getKey("/LastChar").isInteger()
                 ? static_cast<int>(font.getKey("/LastChar").getIntValue())
                 : 255;
  info.simple = true;
  bool nameBased = fd.isDictionary() && (fd.hasKey("/FontFile") || fd.hasKey("/FontFile3"));
  for (int code = 0; code < 256; ++code) {
    if (!gAllCodes && (code < first || code > last)) continue;
    FT_UInt gid;
    if (nameBased && !enc.diffs[code].empty()) {
      gid = FT_Get_Name_Index(face.face, enc.diffs[code].substr(1).c_str());
    } else {
      gid = resolveSimpleGid(face, code, enc, symbolic);
    }
    if (gid == 0) {
      info.bad.insert(code);
    } else if (gStrictNotdef && !(nameBased && !enc.diffs[code].empty()) &&
               strictSimpleGid(face, code, enc, symbolic) == 0) {
      info.bad.insert(code);
    }
  }
  return info;
}

class GlyphCleanFilter : public QPDFObjectHandle::TokenFilter {
 public:
  GlyphCleanFilter(const std::map<std::string, const FontGlyphInfo*>& fonts,
                   const std::set<std::string>& xobjects,
                   const std::set<std::string>& gstates,
                   const std::set<std::string>& shadings,
                   const std::set<std::string>& colorSpaces, int& dropped, int& refDropped,
                   int& langFixed)
      : fonts(fonts), xobjects(xobjects), gstates(gstates), shadings(shadings),
        colorSpaces(colorSpaces), dropped(dropped), refDropped(refDropped),
        langFixed(langFixed) {}

  static bool langOk(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    int seg = 0;
    while (i < s.size()) {
      size_t start = i;
      while (i < s.size() && s[i] != '-') ++i;
      size_t len = i - start;
      if (len < 1 || len > 8) return false;
      int alphas = 0, digits = 0;
      for (size_t j = start; j < start + len; ++j) {
        unsigned char c = static_cast<unsigned char>(s[j]);
        if (c >= 0x80) return false;
        if (std::isalpha(c)) ++alphas;
        else if (std::isdigit(c)) ++digits;
        else return false;
      }
      if (seg == 0 && digits) return false;
      if (seg > 0 && len == 2 && digits) return false;
      if (seg > 0 && len == 3 && alphas && digits) return false;
      ++seg;
      if (i < s.size()) ++i;
    }
    return true;
  }

  void handleToken(QPDFTokenizer::Token const& token) override {
    QPDFTokenizer::token_type_e type = token.getType();
    if (type != QPDFTokenizer::tt_word) {
      operands.push_back(token);
      return;
    }
    std::string op = token.getValue();
    if (op == "BDC" || op == "BMC" || op == "DP") {
      for (size_t oi = 0; oi + 1 < operands.size(); ++oi) {
        if (operands[oi].getType() == QPDFTokenizer::tt_name &&
            operands[oi].getValue() == "/Lang") {
          for (size_t oj = oi + 1; oj < operands.size(); ++oj) {
            if (operands[oj].getType() == QPDFTokenizer::tt_string) {
              if (!langOk(operands[oj].getValue())) {
                operands[oj] = QPDFTokenizer::Token(QPDFTokenizer::tt_string, "en");
                ++langFixed;
              }
              break;
            }
            if (operands[oj].getType() == QPDFTokenizer::tt_name) break;
          }
        }
      }
      flush();
      writeToken(token);
      return;
    }
    if (op == "q") {
      saved.emplace_back(cur, fontSet);
    } else if (op == "Q" && !saved.empty()) {
      std::tie(cur, fontSet) = saved.back();
      saved.pop_back();
    }
    if (op == "Tf") {
      std::string name = lastName();
      if (!name.empty() && fonts.count(name) == 0) {
        operands.clear();
        cur = nullptr;
        fontSet = false;
        ++refDropped;
        return;
      }
      cur = fonts.count(name) ? fonts.at(name) : nullptr;
      fontSet = !name.empty();
      flush();
      writeToken(token);
      return;
    }
    if (op == "cs" || op == "CS") {
      std::string name = lastName();
      static const std::set<std::string> kDevice = {"/DeviceRGB", "/DeviceCMYK",
                                                    "/DeviceGray", "/Pattern"};
      if (!name.empty() && kDevice.count(name) == 0 && colorSpaces.count(name) == 0) {
        operands.clear();
        ++refDropped;
        return;
      }
      flush();
      writeToken(token);
      return;
    }
    if (op == "Do" || op == "gs" || op == "sh") {
      std::string name = lastName();
      const std::set<std::string>* domain =
          op == "Do" ? &xobjects : (op == "gs" ? &gstates : &shadings);
      if (!name.empty() && domain->count(name) == 0) {
        operands.clear();
        ++refDropped;
        return;
      }
      flush();
      writeToken(token);
      return;
    }
    if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
      if (!fontSet) {
        operands.clear();
        ++refDropped;
        return;
      }
      if (cur && cur->cmapCoded && !cur->badCodes.empty()) {
        for (auto& t : operands) {
          if (t.getType() != QPDFTokenizer::tt_string) continue;
          std::string v = t.getValue();
          std::string outV;
          size_t i = 0;
          while (i < v.size()) {
            int len = 1;
            uint32_t code = static_cast<unsigned char>(v[i]);
            bool matched = false;
            for (int nb = 1; nb <= 4 && i + nb <= v.size(); ++nb) {
              uint32_t c = 0;
              for (int b = 0; b < nb; ++b) {
                c = (c << 8) | static_cast<unsigned char>(v[i + b]);
              }
              for (const auto& sp : cur->spaces) {
                if (sp.bytes == nb && c >= sp.lo && c <= sp.hi) {
                  len = nb;
                  code = c;
                  matched = true;
                  break;
                }
              }
              if (matched) break;
            }
            if (!matched && !cur->spaces.empty()) {
              len = cur->spaces.front().bytes;
              code = 0;
              for (int b = 0; b < len && i + b < v.size(); ++b) {
                code = (code << 8) | static_cast<unsigned char>(v[i + b]);
              }
            }
            if (i + len > v.size()) len = static_cast<int>(v.size() - i);
            if (!cur->badCodes.count(code)) {
              outV.append(v, i, len);
            } else {
              ++dropped;
            }
            i += len;
          }
          if (outV != v) t = QPDFTokenizer::Token(QPDFTokenizer::tt_string, outV);
        }
      } else if (cur && !cur->bad.empty()) {
        for (auto& t : operands) {
          if (t.getType() == QPDFTokenizer::tt_string) {
            std::string v = t.getValue();
            std::string outV;
            if (cur->cid2byte) {
              for (size_t i = 0; i + 1 < v.size(); i += 2) {
                int cid = (static_cast<unsigned char>(v[i]) << 8) |
                          static_cast<unsigned char>(v[i + 1]);
                if (!cur->bad.count(cid)) {
                  outV += v[i];
                  outV += v[i + 1];
                } else {
                  ++dropped;
                }
              }
            } else {
              for (unsigned char c : v) {
                if (!cur->bad.count(c)) outV += static_cast<char>(c);
                else ++dropped;
              }
            }
            if (outV != v) {
              t = QPDFTokenizer::Token(QPDFTokenizer::tt_string, outV);
            }
          }
        }
      }
      flush();
      writeToken(token);
      return;
    }
    flush();
    writeToken(token);
  }

  void handleEOF() override { flush(); }

 private:
  std::string lastName() {
    for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
      if (it->getType() == QPDFTokenizer::tt_name) return it->getValue();
    }
    return std::string();
  }

  void flush() {
    for (auto& t : operands) {
      if (t.getType() == QPDFTokenizer::tt_string) {
        write(QPDFObjectHandle::newString(t.getValue()).unparse());
      } else {
        writeToken(t);
      }
    }
    operands.clear();
  }

  const std::map<std::string, const FontGlyphInfo*>& fonts;
  const std::set<std::string>& xobjects;
  const std::set<std::string>& gstates;
  const std::set<std::string>& shadings;
  const std::set<std::string>& colorSpaces;
  int& dropped;
  int& refDropped;
  int& langFixed;
  const FontGlyphInfo* cur = nullptr;
  bool fontSet = false;
  std::vector<std::pair<const FontGlyphInfo*, bool>> saved;
  std::vector<QPDFTokenizer::Token> operands;
};

void glyphCleanHolder(Ctx& ctx, FtLib& lib, QPDFObjectHandle holder, QPDFObjectHandle res,
                      Visited& visited, std::map<QPDFObjGen, FontGlyphInfo>& cache,
                      int& dropped, int& refDropped, int& langFixed, int depth = 0);

void glyphCleanResources(Ctx& ctx, FtLib& lib, QPDFObjectHandle res, Visited& visited,
                         std::map<QPDFObjGen, FontGlyphInfo>& cache, int& dropped,
                         int& refDropped, int& langFixed, int depth = 0) {
  if (depth > kMaxResourceNest) return;
  if (!res.isDictionary() || !visited.enter(res)) return;
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (xod.isDictionary()) {
    for (const std::string& k : xod.getKeys()) {
      QPDFObjectHandle xo = xod.getKey(k);
      if (xo.isStream() && nameIs(xo.getDict().getKey("/Subtype"), "/Form") &&
          visited.enter(xo)) {
        QPDFObjectHandle inner = xo.getDict().getKey("/Resources");
        glyphCleanHolder(ctx, lib, xo, inner.isDictionary() ? inner : res, visited, cache,
                         dropped, refDropped, langFixed, depth + 1);
      }
    }
  }
}

double gTimeFonts = 0, gTimePre = 0, gTimeProbe = 0;
int gHolders = 0, gProbes = 0;

void glyphCleanHolder(Ctx& ctx, FtLib& lib, QPDFObjectHandle holder, QPDFObjectHandle res,
                      Visited& visited, std::map<QPDFObjGen, FontGlyphInfo>& cache,
                      int& dropped, int& refDropped, int& langFixed, int depth) {
  ++gHolders;
  auto tA = std::chrono::steady_clock::now();
  std::map<std::string, const FontGlyphInfo*> fonts;
  std::deque<FontGlyphInfo> ownedInfos;
  std::set<std::string> xobjects, gstates, shadings, colorSpaces;
  if (res.isDictionary()) {
    QPDFObjectHandle fd = res.getKey("/Font");
    if (fd.isDictionary()) {
      for (const std::string& k : fd.getKeys()) {
        QPDFObjectHandle fnt = fd.getKey(k);
        if (!fnt.isDictionary()) continue;
        const FontGlyphInfo* info = nullptr;
        if (fnt.isIndirect()) {
          auto it = cache.find(fnt.getObjGen());
          if (it == cache.end()) {
            it = cache.emplace(fnt.getObjGen(), analyzeFontGlyphs(lib, fnt)).first;
          }
          info = &it->second;
        } else {
          ownedInfos.push_back(analyzeFontGlyphs(lib, fnt));
          info = &ownedInfos.back();
        }
        fonts[k] = info;
      }
    }
    QPDFObjectHandle xod = res.getKey("/XObject");
    if (xod.isDictionary()) {
      for (const std::string& k : xod.getKeys()) xobjects.insert(k);
    }
    QPDFObjectHandle gsd = res.getKey("/ExtGState");
    if (gsd.isDictionary()) {
      for (const std::string& k : gsd.getKeys()) gstates.insert(k);
    }
    QPDFObjectHandle shd = res.getKey("/Shading");
    if (shd.isDictionary()) {
      for (const std::string& k : shd.getKeys()) shadings.insert(k);
    }
    QPDFObjectHandle csd = res.getKey("/ColorSpace");
    if (csd.isDictionary()) {
      for (const std::string& k : csd.getKeys()) colorSpaces.insert(k);
    }
  }
  bool needClean = false;
  for (auto& kv : fonts) {
    if (!kv.second->bad.empty()) needClean = true;
  }
  auto tB = std::chrono::steady_clock::now();
  gTimeFonts += std::chrono::duration<double>(tB - tA).count();

  if (holder.isStream()) {
    std::string rawBytes;
    try {
      auto rawBuf = holder.getStreamData(qpdf_dl_all);
      rawBytes.assign(reinterpret_cast<const char*>(rawBuf->getBuffer()), rawBuf->getSize());
    } catch (...) {
      rawBytes.clear();
    }
    if (rawBytes.size() > 65536) {
      bool mentions = rawBytes.find("/Lang") != std::string::npos;
      if (!mentions) {
        for (auto& kv : fonts) {
          if (!kv.second->bad.empty() && rawBytes.find(kv.first) != std::string::npos) {
            mentions = true;
            break;
          }
        }
      }
      if (!mentions) {
        glyphCleanResources(ctx, lib, res, visited, cache, dropped, refDropped, langFixed,
                            depth);
        return;
      }
    }
  }
  auto tC = std::chrono::steady_clock::now();
  gTimePre += std::chrono::duration<double>(tC - tB).count();
  ++gProbes;
  struct ProbeTimer {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~ProbeTimer() { gTimeProbe += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count(); }
  } probeTimer;
  try {
    QPDFPageObjectHelper ph(holder);
    Pl_Buffer probe("glyphclean probe");
    GlyphCleanFilter probeFilter(fonts, xobjects, gstates, shadings, colorSpaces, dropped,
                                 refDropped, langFixed);
    int before = dropped + refDropped + langFixed;
    ph.filterContents(&probeFilter, &probe);
    if (dropped + refDropped + langFixed == before && !needClean) {
      glyphCleanResources(ctx, lib, res, visited, cache, dropped, refDropped, langFixed,
                          depth);
      return;
    }
    auto data = probe.getBufferSharedPointer();
    std::string rewritten(reinterpret_cast<const char*>(data->getBuffer()), data->getSize());
    if (holder.isStream()) {
      holder.replaceStreamData(rewritten, QPDFObjectHandle::newNull(),
                               QPDFObjectHandle::newNull());
    } else {
      holder.replaceKey(
          "/Contents",
          ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, rewritten)));
    }
  } catch (...) {
    ctx.scanIncomplete("a content stream being rewritten for fonts");
  }
  glyphCleanResources(ctx, lib, res, visited, cache, dropped, refDropped, langFixed, depth);
}
}

void passGlyphClean(Ctx& ctx) {
  if (!ctx.isA()) return;
  ctxIdentityCmaps = &ctx.identityCmaps;
  gStrictNotdef = ctx.part >= 2;
  gAllCodes = ctx.opt.verifyOnly;
  FtLib lib;
  Visited visited;
  std::map<QPDFObjGen, FontGlyphInfo> cache;
  int dropped = 0, refDropped = 0, langFixed = 0;
  QPDFPageDocumentHelper dh(ctx.pdf);
  int kuraPage1 = 0;
  for (auto& ph : dh.getAllPages()) {
    PageScope kuraScope1(ctx, ++kuraPage1);
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle res = ph.getAttribute("/Resources", false);
    glyphCleanHolder(ctx, lib, page, res, visited, cache, dropped, refDropped, langFixed);
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (!annots.isArray()) continue;
    for (int i = 0; i < annots.getArrayNItems(); ++i) {
      QPDFObjectHandle a = annots.getArrayItem(i);
      if (!a.isDictionary()) continue;
      std::vector<QPDFObjectHandle> streams = normalAppearanceStreams(a);
      for (QPDFObjectHandle& st : streams) {
        if (!visited.enter(st)) continue;
        glyphCleanHolder(ctx, lib, st, st.getDict().getKey("/Resources"), visited, cache,
                         dropped, refDropped, langFixed);
      }
    }
  }
  if (langFixed) {
    ctx.issue("CONTENT_LANG_FIXED",
              "replaced " + std::to_string(langFixed) +
                  " invalid /Lang value(s) in marked-content properties",
              true);
  }
  if (std::getenv("PDFA_TIMING")) {
    std::fprintf(stderr,
                 "[timing] glyphclean detail: holders=%d probes=%d fonts=%.1fs pre=%.1fs "
                 "probe=%.1fs\n",
                 gHolders, gProbes, gTimeFonts, gTimePre, gTimeProbe);
  }
  if (dropped) {
    ctx.issue("MISSING_GLYPHS_REMOVED",
              "removed " + std::to_string(dropped) +
                  " character reference(s) whose glyphs are absent from the embedded font "
                  "(rendered as .notdef before conversion)",
              true);
  }
  if (refDropped) {
    ctx.issue("DANGLING_REFS_REMOVED",
              "removed " + std::to_string(refDropped) +
                  " operator(s) referencing resources missing from the resource dictionary",
              !ctx.opt.verifyOnly || ctx.part >= 4);
  }
}
}

namespace pdfa {
namespace {
struct RawUse {
  QPDFObjectHandle font;
  std::set<std::string> strings[2];
  std::set<std::tuple<std::string, QPDFObjGen, long long>> bare;
};

thread_local QPDFObjGen tUsePage;
thread_local bool tBaseAt = false;
thread_local long long tBaseMcid = -1;

class UseCollector : public QPDFObjectHandle::ParserCallbacks {
 public:
  UseCollector(QPDFObjectHandle res, std::map<QPDFObjGen, RawUse>& out, std::set<QPDFObjGen>& active, int depth,
               QPDFObjectHandle font, long long tr, std::set<QPDFObjGen>* reached)
      : res_(res), out_(out), active_(active), depth_(depth), font_(font), tr_(tr), reached_(reached),
        baseAt_(tBaseAt), baseMcid_(tBaseMcid) {}

  void handleObject(QPDFObjectHandle obj) override {
    if (!obj.isOperator()) {
      ops_.push_back(obj);
      return;
    }
    std::string op = obj.getOperatorValue();
    if (op == "BDC" && ops_.size() >= 2) {
      QPDFObjectHandle props = ops_[ops_.size() - 1];
      if (props.isName() && res_.isDictionary() && res_.getKey("/Properties").isDictionary()) {
        props = res_.getKey("/Properties").getKey(props.getName());
      }
      bool at = props.isDictionary() && props.hasKey("/ActualText");
      long long mcid = props.isDictionary() && props.getKey("/MCID").isInteger() ? props.getKey("/MCID").getIntValue() : -1;
      mc_.push_back({at, mcid});
    } else if (op == "BMC" || op == "BDC") {
      mc_.push_back({false, -1});
    } else if (op == "EMC" && !mc_.empty()) {
      mc_.pop_back();
    }
    if (op == "q") {
      stack_.push_back({font_, tr_});
    } else if (op == "Q") {
      if (!stack_.empty()) {
        font_ = stack_.back().first;
        tr_ = stack_.back().second;
        stack_.pop_back();
      }
    } else if (op == "Tf" && ops_.size() >= 2 && ops_[ops_.size() - 2].isName()) {
      QPDFObjectHandle fd = res_.isDictionary() ? res_.getKey("/Font") : QPDFObjectHandle::newNull();
      font_ = fd.isDictionary() ? fd.getKey(ops_[ops_.size() - 2].getName()) : QPDFObjectHandle::newNull();
    } else if (op == "Tr" && !ops_.empty() && ops_.back().isInteger()) {
      tr_ = ops_.back().getIntValue();
    } else if (op == "Tj" || op == "'" || op == "\"" || op == "TJ") {
      for (QPDFObjectHandle& o : ops_) {
        if (o.isString()) note(o.getStringValue());
        if (o.isArray()) {
          for (int i = 0; i < o.getArrayNItems(); ++i) {
            if (o.getArrayItem(i).isString()) note(o.getArrayItem(i).getStringValue());
          }
        }
      }
    } else if (op == "Do" && !ops_.empty() && ops_.back().isName() && res_.isDictionary()) {
      QPDFObjectHandle xd = res_.getKey("/XObject");
      QPDFObjectHandle xo = xd.isDictionary() ? xd.getKey(ops_.back().getName()) : QPDFObjectHandle::newNull();
      if (reached_ && xo.isStream() && xo.isIndirect()) reached_->insert(xo.getObjGen());
      if (xo.isStream() && nameIs(xo.getDict().getKey("/Subtype"), "/Form")) {
        QPDFObjectHandle inner = xo.getDict().getKey("/Resources");
        bool savedAt = tBaseAt;
        long long savedMcid = tBaseMcid;
        tBaseAt = actualText();
        tBaseMcid = mcid();
        collectHolder(xo, inner.isDictionary() ? inner : res_, out_, active_, depth_ + 1, font_, tr_, reached_);
        tBaseAt = savedAt;
        tBaseMcid = savedMcid;
      }
    } else if ((op == "scn" || op == "SCN") && !ops_.empty() && ops_.back().isName() && res_.isDictionary() && reached_) {
      QPDFObjectHandle pd = res_.getKey("/Pattern");
      QPDFObjectHandle pat = pd.isDictionary() ? pd.getKey(ops_.back().getName()) : QPDFObjectHandle::newNull();
      if (pat.isStream()) {
        QPDFObjectHandle inner = pat.getDict().getKey("/Resources");
        collectHolder(pat, inner.isDictionary() ? inner : res_, out_, active_, depth_ + 1, font_, tr_, reached_);
      }
    }
    ops_.clear();
  }

  void handleEOF() override {}

  static void collectHolder(QPDFObjectHandle stream, QPDFObjectHandle res, std::map<QPDFObjGen, RawUse>& out,
                            std::set<QPDFObjGen>& active, int depth, QPDFObjectHandle font, long long tr,
                            std::set<QPDFObjGen>* reached) {
    if (depth > kMaxResourceNest) return;
    if (stream.isIndirect() && !active.insert(stream.getObjGen()).second) return;
    if (reached && stream.isIndirect()) reached->insert(stream.getObjGen());
    UseCollector c(res, out, active, depth, font, tr, reached);
    try {
      if (stream.isStream()) {
        stream.parseAsContents(&c);
      } else {
        QPDFPageObjectHelper(stream).parseContents(&c);
      }
    } catch (...) {
    }
    if (stream.isIndirect()) active.erase(stream.getObjGen());
  }

 private:
  void note(const std::string& s) {
    if (!font_.isDictionary() || !font_.isIndirect()) return;
    RawUse& r = out_[font_.getObjGen()];
    r.font = font_;
    r.strings[tr_ == 3 ? 0 : 1].insert(s);
    if (!actualText()) r.bare.insert({s, tUsePage, mcid()});
  }

  bool actualText() const {
    if (baseAt_) return true;
    for (const auto& m : mc_) {
      if (m.first) return true;
    }
    return false;
  }

  long long mcid() const {
    for (auto it = mc_.rbegin(); it != mc_.rend(); ++it) {
      if (it->second >= 0) return it->second;
    }
    return baseMcid_;
  }

  QPDFObjectHandle res_;
  std::map<QPDFObjGen, RawUse>& out_;
  std::set<QPDFObjGen>& active_;
  int depth_;
  QPDFObjectHandle font_;
  long long tr_;
  std::vector<QPDFObjectHandle> ops_;
  std::vector<std::pair<QPDFObjectHandle, long long>> stack_;
  std::set<QPDFObjGen>* reached_;
  bool baseAt_;
  long long baseMcid_;
  std::vector<std::pair<bool, long long>> mc_;
};
}

std::map<QPDFObjGen, FontUse> collectFontUse(QPDF& pdf, std::set<QPDFObjGen>* reached) {
  std::map<QPDFObjGen, RawUse> raw;
  std::set<QPDFObjGen> active;
  for (auto& ph : QPDFPageDocumentHelper(pdf).getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    QPDFObjectHandle res = ph.getAttribute("/Resources", false);
    tUsePage = page.getObjGen();
    tBaseAt = false;
    tBaseMcid = -1;
    UseCollector::collectHolder(page, res, raw, active, 0, QPDFObjectHandle::newNull(), 0, reached);
    QPDFObjectHandle annots = page.getKey("/Annots");
    for (int i = 0; annots.isArray() && i < annots.getArrayNItems(); ++i) {
      QPDFObjectHandle a = annots.getArrayItem(i);
      if (!a.isDictionary()) continue;
      for (QPDFObjectHandle& st : normalAppearanceStreams(a)) {
        QPDFObjectHandle inner = st.getDict().getKey("/Resources");
        UseCollector::collectHolder(st, inner.isDictionary() ? inner : res, raw, active, 0,
                                    QPDFObjectHandle::newNull(), 0, reached);
      }
    }
  }
  std::map<QPDFObjGen, FontUse> out;
  for (auto& kv : raw) {
    FontUse& u = out[kv.first];
    QPDFObjectHandle font = kv.second.font;
    QPDFObjectHandle procs = font.getKey("/CharProcs");
    for (const std::string& k : procs.isDictionary() && reached ? procs.getKeys() : std::set<std::string>()) {
      if (procs.getKey(k).isIndirect()) reached->insert(procs.getKey(k).getObjGen());
    }
    bool type0 = nameIs(font.getKey("/Subtype"), "/Type0");
    std::vector<CodespaceRange> spaces;
    std::map<uint32_t, int> code2cid;
    bool cmapCoded = false;
    QPDFObjectHandle enc = font.getKey("/Encoding");
    if (type0 && enc.isStream()) {
      try {
        auto buf = enc.getStreamData(qpdf_dl_all);
        cmapCoded = parseCmapBody(std::string(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize()), spaces, code2cid);
      } catch (...) {
      }
    }
    bool identity = type0 && enc.isName() && (enc.getName() == "/Identity-H" || enc.getName() == "/Identity-V");
    auto split = [&](const std::string& v, bool& partial) {
      std::vector<uint32_t> codes;
      size_t i = 0;
      while (i < v.size()) {
        int len = type0 ? 2 : 1;
        if (cmapCoded) {
          bool matched = false;
          for (int nb = 1; nb <= 4 && i + nb <= v.size() && !matched; ++nb) {
            uint32_t c = 0;
            for (int b = 0; b < nb; ++b) c = (c << 8) | static_cast<unsigned char>(v[i + b]);
            for (const auto& sp : spaces) {
              if (sp.bytes == nb && c >= sp.lo && c <= sp.hi) {
                len = nb;
                matched = true;
                break;
              }
            }
          }
          if (!matched) len = spaces.front().bytes;
        }
        if (i + len > v.size()) {
          partial = true;
          break;
        }
        uint32_t code = 0;
        for (int b = 0; b < len; ++b) code = (code << 8) | static_cast<unsigned char>(v[i + b]);
        i += len;
        codes.push_back(code);
      }
      return codes;
    };
    bool haveCid = !type0 || identity || cmapCoded;
    for (int vis = 0; vis < 2; ++vis) {
      for (const std::string& v : kv.second.strings[vis]) {
        for (uint32_t code : split(v, u.partialCode)) {
          u.codes.insert(code);
          if (vis) u.visibleCodes.insert(code);
          uint32_t cid = code;
          if (cmapCoded) {
            auto it = code2cid.find(code);
            cid = it == code2cid.end() ? 0 : static_cast<uint32_t>(it->second);
          }
          if (haveCid) {
            u.cids.insert(cid);
            if (vis) u.visibleCids.insert(cid);
          }
        }
      }
    }
    for (const auto& b : kv.second.bare) {
      bool ignored = false;
      for (uint32_t code : split(std::get<0>(b), ignored)) u.bareCodes.insert({code, std::get<1>(b), std::get<2>(b)});
    }
  }
  return out;
}
}
