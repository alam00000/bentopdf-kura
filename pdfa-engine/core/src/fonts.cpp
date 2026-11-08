#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFNumberTreeObjectHelper.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>

#include <ft2build.h>
#include FT_ADVANCES_H
#include FT_CID_H
#include FT_FREETYPE_H
#include FT_TRUETYPE_IDS_H

#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include FT_TRUETYPE_TABLES_H

#include <algorithm>

#include "assets/agl_names.hh"
#include "assets/fonts_data.hh"
#include "assets/srgb_icc.hh"
#include "colorx.hh"
#include "ctx.hh"
#include "encodings.hh"
#include "fonts_ft.hh"
#include "fontworld.hh"
#include "passes.hh"
#include "util.hh"

namespace pdfa {
namespace {
struct HmtxTable {
  bool ok = false;
  double upem = 1000.0;
  uint32_t numH = 0;
  uint32_t numGlyphs = 0;
  const unsigned char* hmtx = nullptr;
  size_t hmtxLen = 0;
};

uint32_t rdU32(const unsigned char* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

uint16_t rdU16(const unsigned char* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

HmtxTable parseHmtx(const std::string& data) {
  HmtxTable t;
  const unsigned char* d = reinterpret_cast<const unsigned char*>(data.data());
  size_t n = data.size();
  if (n < 12) return t;
  uint32_t tag = rdU32(d);
  if (tag != 0x00010000 && tag != 0x74727565) return t;
  uint16_t ntables = rdU16(d + 4);
  size_t headOff = 0, hheaOff = 0, hmtxOff = 0, hmtxLen = 0, maxpOff = 0;
  for (uint16_t i = 0; i < ntables; ++i) {
    size_t rec = 12 + static_cast<size_t>(i) * 16;
    if (rec + 16 > n) return t;
    uint32_t tg = rdU32(d + rec);
    uint32_t off = rdU32(d + rec + 8);
    uint32_t len = rdU32(d + rec + 12);
    if (static_cast<size_t>(off) + len > n) continue;
    if (tg == 0x68656164) headOff = off;
    else if (tg == 0x68686561) hheaOff = off;
    else if (tg == 0x686D7478) { hmtxOff = off; hmtxLen = len; }
    else if (tg == 0x6D617870) maxpOff = off;
  }
  if (!headOff || !hheaOff || !hmtxOff || !maxpOff) return t;
  if (headOff + 20 > n || hheaOff + 36 > n || maxpOff + 6 > n) return t;
  t.upem = rdU16(d + headOff + 18);
  if (t.upem <= 0) t.upem = 1000.0;
  t.numH = rdU16(d + hheaOff + 34);
  t.numGlyphs = rdU16(d + maxpOff + 4);
  t.hmtx = d + hmtxOff;
  t.hmtxLen = hmtxLen;
  t.ok = t.numH > 0 && t.hmtxLen >= 4;
  return t;
}

double hmtxAdvance(const HmtxTable& t, uint32_t gid) {
  uint32_t idx = gid < t.numH ? gid : t.numH - 1;
  if ((static_cast<size_t>(idx) + 1) * 4 > t.hmtxLen) return -1;
  return rdU16(t.hmtx + static_cast<size_t>(idx) * 4) * 1000.0 / t.upem;
}

double glyphAdvance(FT_Face face, FT_UInt gid, double upem) {
  if (FT_Load_Glyph(face, gid,
                    FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_IGNORE_TRANSFORM) == 0) {
    return face->glyph->metrics.horiAdvance * 1000.0 / upem;
  }

  FT_Fixed adv = 0;
  if (FT_Get_Advance(face, gid, FT_LOAD_NO_SCALE | FT_LOAD_IGNORE_TRANSFORM, &adv) == 0 &&
      adv != 0) {
    return static_cast<double>(adv) * 1000.0 / upem;
  }
  return -1;
}

class CffWidths {
 public:
  explicit CffWidths(const std::string& file, bool applyMatrix = false) {
    const unsigned char* d = reinterpret_cast<const unsigned char*>(file.data());
    size_t n = file.size();
    size_t base = 0;
    if (n > 12 && std::memcmp(d, "OTTO", 4) == 0) {
      uint16_t numTables = rd16(d + 4);
      for (uint16_t i = 0; i < numTables && 12 + 16 * (i + 1) <= n; ++i) {
        const unsigned char* rec = d + 12 + 16 * i;
        if (std::memcmp(rec, "CFF ", 4) == 0) {
          uint32_t off = rd32(rec + 8), len = rd32(rec + 12);
          if (off + len <= n) { base = off; n = off + len; }
          break;
        }
      }
      if (base == 0) return;
    }
    data = d; end = d + n; cur = base;
    if (base + 4 > n) return;
    uint8_t hdrSize = data[base + 2];
    cur = base + hdrSize;
    if (!skipIndex()) return;
    std::vector<std::pair<size_t, size_t>> top;
    if (!readIndex(top) || top.empty()) return;
    if (!skipIndex()) return;
    if (!skipIndex()) return;
    std::map<int, std::vector<double>> topDict;
    parseDict(top[0].first, top[0].second, topDict);
    topM = applyMatrix ? matrixA(topDict) : 0;
    size_t fileLen = static_cast<size_t>(end - data);
    auto getOff = [&](int op) -> size_t {
      auto it = topDict.find(op);
      if (it == topDict.end() || it->second.empty()) return 0;
      double v = it->second.back();
      if (v < 0 || v > static_cast<double>(fileLen)) return 0;
      size_t off = base + static_cast<size_t>(v);
      return off <= fileLen ? off : 0;
    };
    size_t csOff = getOff(17);
    if (!csOff) return;
    cur = csOff;
    if (!readIndex(charstrings)) return;
    size_t fdaOff = getOff(0x0c24), fdsOff = getOff(0x0c25);
    if (fdaOff && fdsOff) {
      cur = fdaOff;
      std::vector<std::pair<size_t, size_t>> fds;
      if (!readIndex(fds)) return;
      for (auto& fd : fds) {
        std::map<int, std::vector<double>> fdd;
        parseDict(fd.first, fd.second, fdd);
        fdPriv.push_back(readPrivate(fdd, base));
        fdScale.push_back(applyMatrix ? (topM > 0 ? topM : 1.0) * (matrixA(fdd) > 0 ? matrixA(fdd) : 0.001) * 1000.0 : 1.0);
      }
      parseFdSelect(fdsOff);
    } else {
      fdPriv.push_back(readPrivate(topDict, base));
      fdScale.push_back((topM > 0 ? topM : 0.001) * 1000.0);
      fdSelectAll0 = true;
    }
    ok = !charstrings.empty() && !fdPriv.empty();
  }

  bool ok = false;

  double widthForGid(size_t gid) const {
    if (!ok || gid >= charstrings.size()) return -1;
    int fd = 0;
    if (!fdSelectAll0) {
      fd = gid < fdSel.size() ? fdSel[gid] : 0;
      if (fd < 0 || static_cast<size_t>(fd) >= fdPriv.size()) fd = 0;
    }
    double scale = static_cast<size_t>(fd) < fdScale.size() ? fdScale[fd] : 1.0;
    double defaultW = fdPriv[fd].first * scale, nominalW = fdPriv[fd].second * scale;
    const unsigned char* p = data + charstrings[gid].first;
    const unsigned char* e = p + charstrings[gid].second;
    int stack = 0;
    double first = 0;
    while (p < e) {
      uint8_t b = *p;
      if (b >= 32 || b == 28) {
        double v;
        if (b == 28) { if (p + 3 > e) break; v = static_cast<int16_t>((p[1] << 8) | p[2]); p += 3; }
        else if (b <= 246) { v = static_cast<int>(b) - 139; p += 1; }
        else if (b <= 250) { if (p + 2 > e) break; v = (b - 247) * 256 + p[1] + 108; p += 2; }
        else if (b <= 254) { if (p + 2 > e) break; v = -((static_cast<int>(b) - 251) * 256) - p[1] - 108; p += 2; }
        else { if (p + 5 > e) break; v = static_cast<double>(static_cast<int32_t>((p[1] << 24) | (p[2] << 16) | (p[3] << 8) | p[4])) / 65536.0; p += 5; }
        if (stack == 0) first = v;
        ++stack;
        continue;
      }

      int expected = -1;
      switch (b) {
        case 1: case 3: case 18: case 23: expected = -2; break;
        case 19: case 20: expected = -2; break;
        case 21: expected = 2; break;
        case 4: case 22: expected = 1; break;
        case 14: expected = 0; break;
        case 10: case 29: return -1;
        default: return defaultW;
      }
      bool hasWidth;
      if (expected == -2) hasWidth = (stack % 2) == 1;
      else if (b == 14) hasWidth = stack == 1 || stack == 5;
      else hasWidth = stack == expected + 1;
      return hasWidth ? nominalW + first * scale : defaultW;
    }
    return defaultW;
  }

 private:
  double topM = 0;
  std::vector<double> fdScale;
  static double matrixA(const std::map<int, std::vector<double>>& dict) {
    auto it = dict.find(0x0c07);
    if (it == dict.end() || it->second.size() < 6 || it->second[0] <= 0) return 0;
    return it->second[0];
  }
  static uint16_t rd16(const unsigned char* p) { return (p[0] << 8) | p[1]; }
  static uint32_t rd32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
  }
  bool readIndex(std::vector<std::pair<size_t, size_t>>& out) {
    if (data + cur + 2 > end) return false;
    uint16_t count = rd16(data + cur);
    cur += 2;
    if (count == 0) return true;
    if (data + cur + 1 > end) return false;
    uint8_t osz = data[cur++];
    if (osz < 1 || osz > 4) return false;
    auto offAt = [&](size_t i) -> size_t {
      const unsigned char* p = data + cur + i * osz;
      size_t v = 0;
      for (int b = 0; b < osz; ++b) v = (v << 8) | p[b];
      return v;
    };
    size_t offArr = cur;
    size_t dataStart = offArr + (static_cast<size_t>(count) + 1) * osz - 1;
    if (data + dataStart > end) return false;
    for (uint16_t i = 0; i < count; ++i) {
      size_t o1 = offAt(i), o2 = offAt(i + 1);
      if (o2 < o1 || data + dataStart + o2 > end) return false;
      out.emplace_back(dataStart + o1, o2 - o1);
    }
    cur = dataStart + offAt(count);
    return true;
  }
  bool skipIndex() {
    std::vector<std::pair<size_t, size_t>> tmp;
    return readIndex(tmp);
  }
  void parseDict(size_t off, size_t len, std::map<int, std::vector<double>>& out) {
    const unsigned char* p = data + off;
    const unsigned char* e = p + len;
    std::vector<double> operands;
    while (p < e) {
      uint8_t b = *p;
      if (b <= 21) {
        int op = b;
        ++p;
        if (b == 12 && p < e) { op = 0x0c00 | *p; ++p; }
        out[op] = operands;
        operands.clear();
      } else if (b == 28) { if (p + 3 > e) break; operands.push_back(static_cast<int16_t>((p[1] << 8) | p[2])); p += 3; }
      else if (b == 29) { if (p + 5 > e) break; operands.push_back(static_cast<int32_t>(rd32(p + 1))); p += 5; }
      else if (b == 30) {
        ++p;
        std::string numTxt;
        bool done = false;
        while (p < e && !done) {
          for (int half = 0; half < 2; ++half) {
            int nib = half == 0 ? (*p >> 4) : (*p & 0xF);
            if (nib <= 9) numTxt += static_cast<char>('0' + nib);
            else if (nib == 0xa) numTxt += '.';
            else if (nib == 0xb) numTxt += 'E';
            else if (nib == 0xc) numTxt += "E-";
            else if (nib == 0xe) numTxt += '-';
            else if (nib == 0xf) { done = true; break; }
          }
          ++p;
        }
        operands.push_back(std::strtod(numTxt.c_str(), nullptr));
      }
      else if (b >= 32 && b <= 246) { operands.push_back(static_cast<int>(b) - 139); ++p; }
      else if (b >= 247 && b <= 250) { if (p + 2 > e) break; operands.push_back((b - 247) * 256 + p[1] + 108); p += 2; }
      else if (b >= 251 && b <= 254) { if (p + 2 > e) break; operands.push_back(-((static_cast<int>(b) - 251) * 256) - p[1] - 108); p += 2; }
      else ++p;
    }
  }
  std::pair<double, double> readPrivate(std::map<int, std::vector<double>>& dict, size_t base) {
    auto it = dict.find(18);
    if (it == dict.end() || it->second.size() < 2) return {0.0, 0.0};
    double dsz = it->second[0], doff = it->second[1];
    size_t fileLen = static_cast<size_t>(end - data);
    if (dsz < 0 || doff < 0 || dsz > static_cast<double>(fileLen) ||
        doff > static_cast<double>(fileLen)) {
      return {0.0, 0.0};
    }
    size_t psz = static_cast<size_t>(dsz);
    size_t poff = base + static_cast<size_t>(doff);
    if (poff > fileLen || psz > fileLen - poff) return {0.0, 0.0};
    std::map<int, std::vector<double>> pd;
    parseDict(poff, psz, pd);
    double dw = pd.count(20) && !pd[20].empty() ? pd[20].back() : 0.0;
    double nw = pd.count(21) && !pd[21].empty() ? pd[21].back() : 0.0;
    return {dw, nw};
  }
  void parseFdSelect(size_t off) {
    if (data + off + 1 > end) return;
    uint8_t fmt = data[off];
    size_t ng = charstrings.size();
    fdSel.assign(ng, 0);
    if (fmt == 0) {
      for (size_t g = 0; g < ng && data + off + 1 + g < end; ++g) fdSel[g] = data[off + 1 + g];
    } else if (fmt == 3) {
      if (data + off + 5 > end) return;
      uint16_t nR = rd16(data + off + 1);
      size_t p = off + 3;
      for (uint16_t r = 0; r < nR && data + p + 5 <= end; ++r, p += 3) {
        uint16_t firstG = rd16(data + p);
        uint8_t fd = data[p + 2];
        uint16_t nextG = rd16(data + p + 3);
        for (uint16_t g = firstG; g < nextG && g < ng; ++g) fdSel[g] = fd;
      }
    }
  }

  const unsigned char* data = nullptr;
  const unsigned char* end = nullptr;
  size_t cur = 0;
  std::vector<std::pair<size_t, size_t>> charstrings;
  std::vector<std::pair<double, double>> fdPriv;
  std::vector<uint8_t> fdSel;
  bool fdSelectAll0 = false;
};

double programAdvance(const FtFace& face, const HmtxTable& hmtx, FT_UInt gid, double upem) {
  if (hmtx.ok) {
    double w = hmtxAdvance(hmtx, gid);
    if (w >= 0) return w;
  }
  return glyphAdvance(face.face, gid, upem);
}

void syncSimpleFontWidths(Ctx& ctx, FtLib& lib, QPDFObjectHandle font, const FontUse* use) {
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  QPDFObjectHandle program = fontFileStream(fd);
  if (!program.isStream()) return;
  QPDFObjectHandle first = font.getKey("/FirstChar");
  QPDFObjectHandle last = font.getKey("/LastChar");
  QPDFObjectHandle widths = font.getKey("/Widths");
  bool keysOk = first.isNumber() && last.isNumber() && widths.isArray();
  if (!keysOk && !use) widths = QPDFObjectHandle::newArray();
  FtFace face;
  if (!loadFace(lib, program, face)) {
    ctx.issue("FONT_PROGRAM_UNREADABLE",
              "embedded font program unreadable: " + nameOf(font.getKey("/BaseFont")), false);
    return;
  }
  double upem = face.face->units_per_EM ? face.face->units_per_EM : 1000.0;
  HmtxTable hmtx = parseHmtx(face.data);
  CffWidths cff(face.data, !ctx.isA());
  long long flags = fd.getKey("/Flags").isInteger() ? fd.getKey("/Flags").getIntValue() : 0;
  bool symbolic = (flags & 4) != 0;
  SimpleEncoding enc = readEncoding(font, symbolic);
  int firstChar = keysOk ? static_cast<int>(first.getNumericValue()) : 0;
  int lastChar = keysOk ? static_cast<int>(last.getNumericValue()) : -1;
  int n = widths.isArray() ? widths.getArrayNItems() : 0;
  bool tt = nameIs(font.getKey("/Subtype"), "/TrueType");
  if (use) {
    double missing = fd.getKey("/MissingWidth").isNumber() ? fd.getKey("/MissingWidth").getNumericValue() : 0.0;
    int bad = 0;
    for (uint32_t code : use->visibleCodes) {
      if (code > 255) continue;
      int c = static_cast<int>(code);
      bool inRange = keysOk && n > 0 && c >= firstChar && c <= lastChar;
      QPDFObjectHandle w = inRange && c - firstChar < n ? widths.getArrayItem(c - firstChar) : QPDFObjectHandle::newNull();
      double dictW = inRange ? (w.isNumber() ? w.getNumericValue() : 0.0) : missing;
      bool present = false;
      FT_UInt gid = tt ? strictTrueTypeGid(face.face, c, font, symbolic, present) : resolveSimpleGid(face, c, enc, symbolic);
      double progW = programAdvance(face, hmtx, gid, upem);
      if (progW < 0 && cff.ok) {
        double cffW = cff.widthForGid(gid);
        if (cffW > 0) progW = cffW * 1000.0 / upem;
      }
      if (progW >= 0 && std::fabs(dictW - progW) > 1.0) ++bad;
    }
    if (bad) {
      ctx.issue("FONT_WIDTHS_SYNCED",
                std::to_string(bad) + " glyph width(s) of " + nameOf(font.getKey("/BaseFont")) +
                    " differ from the embedded font program",
                true);
    }
    return;
  }
  int changed = 0;
  bool extended = false;
  std::vector<QPDFObjectHandle> full(256, QPDFObjectHandle::newInteger(0));
  for (int code = 0; code < 256; ++code) {
    bool inRange = code >= firstChar && code <= lastChar && code - firstChar < n;
    QPDFObjectHandle w =
        inRange ? widths.getArrayItem(code - firstChar) : QPDFObjectHandle::newNull();
    double dictW = w.isNumber() ? w.getNumericValue() : 0.0;
    bool present = false;
    FT_UInt gid = tt ? strictTrueTypeGid(face.face, code, font, symbolic, present) : resolveSimpleGid(face, code, enc, symbolic);
    double progW = programAdvance(face, hmtx, gid, upem);
    if (progW < 0 && cff.ok) {
      double cffW = cff.widthForGid(gid);
      if (cffW > 0) progW = cffW * 1000.0 / upem;
    }
    if (inRange && w.isNumber()) full[code] = w;
    if (gid == 0 && inRange && w.isNumber() && dictW != 0) continue;
    if (progW < 0) continue;
    if (!inRange || !w.isNumber()) {
      if (llround(progW) != 0) {
        full[code] = QPDFObjectHandle::newInteger(llround(progW));
        extended = true;
      }
    } else if (std::fabs(dictW - progW) > 0.9) {
      full[code] = QPDFObjectHandle::newInteger(llround(progW));
      ++changed;
    }
  }
  if (changed || extended || !keysOk) {
    QPDFObjectHandle arr = QPDFObjectHandle::newArray();
    for (int code = 0; code < 256; ++code) arr.appendItem(full[code]);
    font.replaceKey("/FirstChar", QPDFObjectHandle::newInteger(0));
    font.replaceKey("/LastChar", QPDFObjectHandle::newInteger(255));
    font.replaceKey("/Widths", ctx.pdf.makeIndirectObject(arr));
    ctx.issue("FONT_WIDTHS_SYNCED",
              "resynced " + std::to_string(changed) + " width(s) for " +
                  nameOf(font.getKey("/BaseFont")) +
                  " from the embedded font program and extended coverage to all codes",
              true);
  }
}

void syncCidFontWidths(Ctx& ctx, FtLib& lib, QPDFObjectHandle type0, const FontUse* use) {
  QPDFObjectHandle df = type0.getKey("/DescendantFonts");
  if (!df.isArray() || df.getArrayNItems() != 1) return;
  QPDFObjectHandle cidFont = df.getArrayItem(0);
  if (!cidFont.isDictionary()) return;
  bool isT2 = nameIs(cidFont.getKey("/Subtype"), "/CIDFontType2");
  bool isT0cff = nameIs(cidFont.getKey("/Subtype"), "/CIDFontType0");
  if (!isT2 && !isT0cff) return;
  QPDFObjectHandle program = fontFileStream(cidFont.getKey("/FontDescriptor"));
  bool trace = std::getenv("PDFA_TRACE") != nullptr;
  if (!program.isStream()) {
    if (trace) std::fprintf(stderr, "[cidw] %s: no program\n",
                            nameOf(type0.getKey("/BaseFont")).c_str());
    return;
  }
  FtFace face;
  if (!loadFace(lib, program, face)) {
    if (trace) std::fprintf(stderr, "[cidw] %s: loadFace failed\n",
                            nameOf(type0.getKey("/BaseFont")).c_str());
    return;
  }
  double upem = face.face->units_per_EM ? face.face->units_per_EM : 1000.0;
  HmtxTable hmtx = parseHmtx(face.data);
  CffWidths cff(face.data, !ctx.isA());

  QPDFObjectHandle c2g = cidFont.getKey("/CIDToGIDMap");
  std::string mapData;
  std::map<int, FT_UInt> cffCidToGid;
  size_t numCids = static_cast<size_t>(face.face->num_glyphs);
  if (isT2 && c2g.isStream()) {
    auto buf = c2g.getStreamData(qpdf_dl_all);
    mapData.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    numCids = mapData.size() / 2;
  } else if (isT2 && !c2g.isNull() && !nameIs(c2g, "/Identity")) {
    return;
  }
  if (isT0cff) {
    FT_Bool cidKeyed = 0;
    if (!FT_Get_CID_Is_Internally_CID_Keyed(face.face, &cidKeyed) && cidKeyed) {
      int maxCid = 0;
      for (long g = 0; g < face.face->num_glyphs; ++g) {
        FT_UInt cid = 0;
        if (!FT_Get_CID_From_Glyph_Index(face.face, static_cast<FT_UInt>(g), &cid)) {
          cffCidToGid[static_cast<int>(cid)] = static_cast<FT_UInt>(g);
          if (static_cast<int>(cid) > maxCid) maxCid = static_cast<int>(cid);
        }
      }
      numCids = static_cast<size_t>(maxCid) + 1;
    }
  }
  if (numCids == 0 || numCids > 65536) return;

  double dw = 1000.0;
  QPDFObjectHandle dwObj = cidFont.getKey("/DW");
  if (dwObj.isNumber()) dw = dwObj.getNumericValue();
  std::vector<double> declared(numCids, dw);
  QPDFObjectHandle oldW = cidFont.getKey("/W");
  if (oldW.isArray()) {
    int nw = oldW.getArrayNItems();
    int idx = 0;
    while (idx < nw) {
      QPDFObjectHandle a = oldW.getArrayItem(idx);
      if (!a.isNumber()) break;
      long long start = static_cast<long long>(a.getNumericValue());
      if (idx + 1 >= nw) break;
      QPDFObjectHandle b = oldW.getArrayItem(idx + 1);
      if (b.isArray()) {
        for (int m = 0; m < b.getArrayNItems(); ++m) {
          long long cid = start + m;
          if (cid >= 0 && static_cast<size_t>(cid) < numCids && b.getArrayItem(m).isNumber()) {
            declared[cid] = b.getArrayItem(m).getNumericValue();
          }
        }
        idx += 2;
      } else if (b.isNumber() && idx + 2 < nw && oldW.getArrayItem(idx + 2).isNumber()) {
        double endRaw = b.getNumericValue();
        long long end = std::isfinite(endRaw)
                            ? static_cast<long long>(std::min(endRaw, 1e9))
                            : -1;
        double val = oldW.getArrayItem(idx + 2).getNumericValue();
        long long lo = std::max<long long>(start, 0);
        long long hi = std::min<long long>(end, static_cast<long long>(numCids) - 1);
        for (long long cid = lo; cid <= hi; ++cid) declared[cid] = val;
        idx += 3;
      } else {
        break;
      }
    }
  }
  std::vector<long long> widths(numCids, 0);
  std::vector<double> exact(numCids, 0);
  for (size_t cid = 0; cid < numCids; ++cid) {
    FT_UInt gid = static_cast<FT_UInt>(cid);
    bool unmapped = false;
    if (!mapData.empty()) {
      gid = static_cast<FT_UInt>((static_cast<unsigned char>(mapData[cid * 2]) << 8) |
                                 static_cast<unsigned char>(mapData[cid * 2 + 1]));
    } else if (!cffCidToGid.empty()) {
      auto it = cffCidToGid.find(static_cast<int>(cid));
      if (it == cffCidToGid.end()) unmapped = true;
      else gid = it->second;
    }
    double adv = -1;
    if (!unmapped) {
      if (hmtx.ok) adv = programAdvance(face, hmtx, gid, upem);
      if (adv < 0 && cff.ok) {
        double w = cff.widthForGid(gid);
        if (w >= 0) adv = w * 1000.0 / upem;
      }
      if (adv < 0) adv = programAdvance(face, hmtx, gid, upem);
    }

    widths[cid] = adv < 0 ? llround(declared[cid]) : llround(adv);
    exact[cid] = adv < 0 || unmapped ? declared[cid] : adv;
  }
  if (use) {
    int bad = 0;
    for (uint32_t cid : use->visibleCids) {
      if (cid < numCids && std::fabs(declared[cid] - exact[cid]) > 1.0) ++bad;
    }
    if (bad) {
      ctx.issue("FONT_WIDTHS_SYNCED",
                std::to_string(bad) + " glyph width(s) of " + nameOf(cidFont.getKey("/BaseFont")) +
                    " differ from the embedded font program",
                true);
    }
    return;
  }
  bool mismatch = false;
  for (size_t cid = 0; cid < numCids && !mismatch; ++cid) {
    if (std::fabs(declared[cid] - static_cast<double>(widths[cid])) > 0.9) mismatch = true;
  }
  if (!mismatch) return;

  QPDFObjectHandle w = QPDFObjectHandle::newArray();
  size_t i = 0;
  while (i < numCids) {
    size_t j = i;
    while (j + 1 < numCids && widths[j + 1] == widths[i]) ++j;
    if (j - i >= 3) {
      w.appendItem(QPDFObjectHandle::newInteger(static_cast<long long>(i)));
      w.appendItem(QPDFObjectHandle::newInteger(static_cast<long long>(j)));
      w.appendItem(QPDFObjectHandle::newInteger(widths[i]));
      i = j + 1;
    } else {
      size_t runStart = i;
      QPDFObjectHandle list = QPDFObjectHandle::newArray();
      while (i < numCids) {
        size_t k = i;
        while (k + 1 < numCids && widths[k + 1] == widths[i]) ++k;
        if (k - i >= 3) break;
        for (size_t m = i; m <= k; ++m) list.appendItem(QPDFObjectHandle::newInteger(widths[m]));
        i = k + 1;
      }
      w.appendItem(QPDFObjectHandle::newInteger(static_cast<long long>(runStart)));
      w.appendItem(list);
    }
  }
  cidFont.replaceKey("/W", w);
  double notdef = programAdvance(face, hmtx, 0, upem);
  cidFont.replaceKey("/DW", QPDFObjectHandle::newInteger(notdef < 0 ? 1000 : llround(notdef)));
  ctx.issue("FONT_WIDTHS_SYNCED",
            "rebuilt /W array for " + nameOf(cidFont.getKey("/BaseFont")) +
                " from embedded font program",
            true);
}

bool isSubsetName(const std::string& base) {
  if (base.size() < 8 || base[0] != '/') return false;
  for (int i = 1; i <= 6; ++i) {
    if (base[i] < 'A' || base[i] > 'Z') return false;
  }
  return base[7] == '+';
}

void ensureCidSet(Ctx& ctx, FtLib& lib, QPDFObjectHandle type0) {
  QPDFObjectHandle df = type0.getKey("/DescendantFonts");
  if (!df.isArray() || df.getArrayNItems() != 1) return;
  QPDFObjectHandle cidFont = df.getArrayItem(0);
  if (!cidFont.isDictionary()) return;
  QPDFObjectHandle fd = cidFont.getKey("/FontDescriptor");
  QPDFObjectHandle program = fontFileStream(fd);
  if (!fd.isDictionary() || !program.isStream()) return;
  if (!isSubsetName(nameOf(type0.getKey("/BaseFont"))) &&
      !isSubsetName(nameOf(cidFont.getKey("/BaseFont")))) {
    return;
  }
  size_t numCids = 0;
  QPDFObjectHandle c2g = cidFont.getKey("/CIDToGIDMap");
  std::string mapData;
  if (c2g.isStream()) {
    auto buf = c2g.getStreamData(qpdf_dl_all);
    mapData.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    numCids = mapData.size() / 2;
  } else {
    FtFace face;
    if (loadFace(lib, program, face)) numCids = static_cast<size_t>(face.face->num_glyphs);
  }
  if (numCids == 0 || numCids > 65536) return;

  std::string bits((numCids + 7) / 8, '\0');
  if (!mapData.empty()) {
    bits[0] = static_cast<char>(0x80);
    for (size_t cid = 0; cid < numCids; ++cid) {
      uint16_t gid = static_cast<uint16_t>(
          (static_cast<unsigned char>(mapData[cid * 2]) << 8) |
          static_cast<unsigned char>(mapData[cid * 2 + 1]));
      if (gid != 0) bits[cid / 8] |= static_cast<char>(0x80 >> (cid % 8));
    }
  } else {
    for (size_t cid = 0; cid < numCids; ++cid) {
      bits[cid / 8] |= static_cast<char>(0x80 >> (cid % 8));
    }
  }
  QPDFObjectHandle cidSet = QPDFObjectHandle::newStream(&ctx.pdf, bits);
  QPDFObjectHandle prior = fd.getKey("/CIDSet");
  bool had = prior.isStream();
  bool same = false;
  if (had) {
    try {
      std::shared_ptr<Buffer> pb = prior.getStreamData(qpdf_dl_all);
      same = pb->getSize() == bits.size() &&
             std::memcmp(pb->getBuffer(), bits.data(), bits.size()) == 0;
    } catch (...) {
    }
  }
  fd.replaceKey("/CIDSet", ctx.pdf.makeIndirectObject(cidSet));
  if (!same) {
    ctx.issue("CIDSET_ADDED",
              std::string(had ? "regenerated" : "synthesized") + " /CIDSet for subset " +
                  nameOf(cidFont.getKey("/BaseFont")),
              true);
  }
}

void ensureCharSet(Ctx& ctx, FtLib& lib, QPDFObjectHandle font) {
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  QPDFObjectHandle program = fontFileStream(fd);
  if (!fd.isDictionary() || !program.isStream()) return;
  if (!isSubsetName(nameOf(font.getKey("/BaseFont")))) return;
  FtFace face;
  if (!loadFace(lib, program, face) || !FT_HAS_GLYPH_NAMES(face.face)) return;
  std::string charset;
  long n = face.face->num_glyphs;
  if (n <= 0 || n > 65536) return;
  char name[128];
  for (long gid = 1; gid < n; ++gid) {
    if (FT_Get_Glyph_Name(face.face, static_cast<FT_UInt>(gid), name, sizeof(name)) == 0 &&
        name[0] != '\0') {
      charset += "/";
      charset += name;
    }
  }
  if (charset.empty()) return;
  QPDFObjectHandle prior = fd.getKey("/CharSet");
  bool had = prior.isString();
  bool same = had && prior.getUTF8Value() == charset;
  fd.replaceKey("/CharSet", QPDFObjectHandle::newString(charset));
  if (!same) {
    ctx.issue("CHARSET_ADDED",
              std::string(had ? "regenerated" : "synthesized") + " /CharSet for subset " +
                  nameOf(font.getKey("/BaseFont")),
              true);
  }
}

const struct {
  const char* prefix;
  const char* registry;
  const char* ordering;
} kPredefRos[] = {
    {"/UniJIS", "Adobe", "Japan1"},   {"/UniGB", "Adobe", "GB1"},
    {"/UniCNS", "Adobe", "CNS1"},     {"/UniKS", "Adobe", "Korea1"},
    {"/90ms", "Adobe", "Japan1"},     {"/90pv", "Adobe", "Japan1"},
    {"/GBK", "Adobe", "GB1"},         {"/ETen", "Adobe", "CNS1"},
    {"/KSC", "Adobe", "Korea1"},      {"/B5pc", "Adobe", "CNS1"},
};

void syncRosWithPredefined(Ctx& ctx, QPDFObjectHandle type0) {
  QPDFObjectHandle enc = type0.getKey("/Encoding");
  if (!enc.isName()) return;
  std::string name = enc.getName();
  if (name == "/Identity-H" || name == "/Identity-V") return;
  QPDFObjectHandle df = type0.getKey("/DescendantFonts");
  if (!df.isArray() || df.getArrayNItems() != 1) return;
  QPDFObjectHandle cid = df.getArrayItem(0);
  for (const auto& entry : kPredefRos) {
    if (name.rfind(entry.prefix, 0) == 0) {
      QPDFObjectHandle ros = cid.getKey("/CIDSystemInfo");
      std::string reg = ros.isDictionary() && ros.getKey("/Registry").isString()
                            ? ros.getKey("/Registry").getStringValue()
                            : "";
      std::string ord = ros.isDictionary() && ros.getKey("/Ordering").isString()
                            ? ros.getKey("/Ordering").getStringValue()
                            : "";
      QPDFObjectHandle rosSup = ros.isDictionary() ? ros.getKey("/Supplement")
                                                   : QPDFObjectHandle::newNull();
      long long haveSupp = rosSup.isInteger() ? rosSup.getIntValue() : -1;
      long long wantSupp = std::string(entry.ordering) == "Japan1"   ? 7
                           : std::string(entry.ordering) == "GB1"    ? 5
                           : std::string(entry.ordering) == "CNS1"   ? 7
                           : std::string(entry.ordering) == "Korea1" ? 2
                                                                     : 0;
      if (reg != entry.registry || ord != entry.ordering || haveSupp < wantSupp) {
        QPDFObjectHandle nr = QPDFObjectHandle::newDictionary();
        nr.replaceKey("/Registry", QPDFObjectHandle::newString(entry.registry));
        nr.replaceKey("/Ordering", QPDFObjectHandle::newString(entry.ordering));
        long long supp = ros.isDictionary() && ros.getKey("/Supplement").isInteger()
                             ? ros.getKey("/Supplement").getIntValue()
                             : 0;
        long long maxSupp = std::string(entry.ordering) == "Japan1"   ? 7
                            : std::string(entry.ordering) == "GB1"    ? 5
                            : std::string(entry.ordering) == "CNS1"   ? 7
                            : std::string(entry.ordering) == "Korea1" ? 2
                                                                      : supp;
        nr.replaceKey("/Supplement",
                      QPDFObjectHandle::newInteger(supp > maxSupp ? supp : maxSupp));
        cid.replaceKey("/CIDSystemInfo", ctx.pdf.makeIndirectObject(nr));
        ctx.issue("CIDSYSTEMINFO_SYNCED",
                  "aligned CIDSystemInfo of " + nameOf(type0.getKey("/BaseFont")) +
                      " with its predefined CMap " + name,
                  true);
      }
      return;
    }
  }
}

void ensureCidToGidMap(Ctx& ctx, QPDFObjectHandle type0) {
  QPDFObjectHandle df = type0.getKey("/DescendantFonts");
  if (!df.isArray() || df.getArrayNItems() != 1) return;
  QPDFObjectHandle cid = df.getArrayItem(0);
  if (!nameIs(cid.getKey("/Subtype"), "/CIDFontType2")) return;
  QPDFObjectHandle map = cid.getKey("/CIDToGIDMap");
  if (map.isStream() || nameIs(map, "/Identity")) return;
  cid.replaceKey("/CIDToGIDMap", QPDFObjectHandle::newName("/Identity"));
  ctx.issue("CIDTOGIDMAP_SET",
            "set /CIDToGIDMap /Identity on " + nameOf(type0.getKey("/BaseFont")), true);
}

void scrubToUnicode(Ctx& ctx, QPDFObjectHandle font) {
  bool puaBanned = ctx.conf == 'A' || ctx.opt.ua;
  QPDFObjectHandle tu = font.getKey("/ToUnicode");
  if (!tu.isStream()) return;
  std::string text;
  try {
    auto buf = tu.getStreamData(qpdf_dl_all);
    text.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return;
  }
  bool changed = false;
  std::string out;
  out.reserve(text.size());
  size_t copied = 0;

  auto scrubDst = [&](size_t a, size_t b) {
    std::string hex = text.substr(a + 1, b - a - 2);
    bool bad = false;
    for (size_t i = 0; i + 4 <= hex.size(); i += 4) {
      std::string quad = hex.substr(i, 4);
      unsigned v = static_cast<unsigned>(std::strtoul(quad.c_str(), nullptr, 16));
      if (puaBanned && v >= 0xD800 && v <= 0xDBFF && i + 8 <= hex.size()) {
        unsigned lo = static_cast<unsigned>(
            std::strtoul(hex.substr(i + 4, 4).c_str(), nullptr, 16));
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          unsigned cp = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
          if ((cp >= 0xF0000 && cp <= 0xFFFFD) || (cp >= 0x100000 && cp <= 0x10FFFD)) {
            hex.replace(i, 8, "FFFD");
            bad = true;
            continue;
          }
          i += 4;
          continue;
        }
      }
      if (v == 0 || v == 0xFEFF || v == 0xFFFE) {
        hex.replace(i, 4, "FFFD");
        bad = true;
      } else if (puaBanned && v >= 0xE000 && v <= 0xF8FF) {
        uint16_t win =
            (v & 0xFF00) == 0xF000 ? winAnsiToUnicode(static_cast<int>(v & 0xFF)) : 0;
        char rep[8];
        std::snprintf(rep, sizeof(rep), "%04X", win ? win : 0xFFFD);
        hex.replace(i, 4, rep);
        bad = true;
      }
    }
    if (hex.size() < 4 && !hex.empty()) {
      unsigned v = static_cast<unsigned>(std::strtoul(hex.c_str(), nullptr, 16));
      if (v == 0 || v == 0xFEFF || v == 0xFFFE) {
        char rep[8];
        std::snprintf(rep, sizeof(rep), "%0*X", static_cast<int>(hex.size()), 0xFFFD & ((1u << (hex.size() * 4)) - 1));
        hex = rep;
        bad = true;
      }
    }
    if (bad) {
      out += '<';
      out += hex;
      out += '>';
      changed = true;
      return true;
    }
    return false;
  };

  size_t pos = 0;
  while (pos < text.size()) {
    size_t c1 = text.find("beginbfchar", pos);
    size_t c2 = text.find("beginbfrange", pos);
    size_t sec;
    bool range;
    if (c2 != std::string::npos && (c1 == std::string::npos || c2 < c1)) {
      sec = c2;
      range = true;
    } else if (c1 != std::string::npos) {
      sec = c1;
      range = false;
    } else {
      break;
    }
    const char* endMark = range ? "endbfrange" : "endbfchar";
    size_t e = text.find(endMark, sec);
    if (e == std::string::npos) break;
    out.append(text, copied, sec - copied);

    size_t p = sec;
    int phase = 0;
    bool inArray = false;
    while (p < e) {
      size_t a = text.find('<', p);
      if (a == std::string::npos || a >= e) break;
      size_t b = text.find('>', a + 1);
      if (b == std::string::npos || b >= e) break;
      ++b;
      for (size_t g = p; g < a; ++g) {
        if (text[g] == '[') inArray = true;
        if (text[g] == ']') {
          inArray = false;
          phase = 0;
        }
      }
      out.append(text, copied, a - copied);
      bool isDst = range ? (phase == 2) : (phase == 1);
      if (isDst) {
        if (!scrubDst(a, b)) out.append(text, a, b - a);
      } else {
        out.append(text, a, b - a);
      }
      copied = b;
      p = b;
      if (range) {
        if (phase < 2) {
          ++phase;
        } else if (!inArray) {
          phase = 0;
        }
      } else {
        phase ^= 1;
      }
    }
    pos = e;
  }
  out.append(text, copied, text.size() - copied);
  if (changed) {
    text = out;
    tu.replaceStreamData(text, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    tu.getDict().removeKey("/Filter");
    tu.getDict().removeKey("/DecodeParms");
    ctx.issue("TOUNICODE_SCRUBBED",
              "replaced forbidden Unicode values (U+0000/U+FEFF/U+FFFE) in ToUnicode of " +
                  nameOf(font.getKey("/BaseFont")),
              true);
  }
}

void fixNonsymbolicTtfEncoding(Ctx& ctx, QPDFObjectHandle font) {
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  long long flags = fd.isDictionary() && fd.getKey("/Flags").isInteger()
                        ? fd.getKey("/Flags").getIntValue()
                        : 0;
  if (flags & 4) return;
  QPDFObjectHandle enc = font.getKey("/Encoding");
  if (enc.isName() &&
      (enc.getName() == "/WinAnsiEncoding" || enc.getName() == "/MacRomanEncoding")) {
    return;
  }
  if (enc.isDictionary()) {
    QPDFObjectHandle base = enc.getKey("/BaseEncoding");
    if (!nameIs(base, "/WinAnsiEncoding") && !nameIs(base, "/MacRomanEncoding")) {
      enc.replaceKey("/BaseEncoding", QPDFObjectHandle::newName("/WinAnsiEncoding"));
      ctx.issue("TTF_ENCODING_SET",
                "set /BaseEncoding /WinAnsiEncoding on non-symbolic TrueType font " +
                    nameOf(font.getKey("/BaseFont")),
                true);
    }
    QPDFObjectHandle diffs = enc.getKey("/Differences");
    if (diffs.isArray()) {
      QPDFObjectHandle nd = QPDFObjectHandle::newArray();
      bool removedAny = false;
      int pendingCode = -1;
      bool codeWritten = false;
      for (int i = 0; i < diffs.getArrayNItems(); ++i) {
        QPDFObjectHandle item = diffs.getArrayItem(i);
        if (item.isInteger()) {
          pendingCode = static_cast<int>(item.getIntValue());
          codeWritten = false;
          continue;
        }
        if (!item.isName()) continue;
        std::string bare = item.getName().substr(1);
        uint32_t parsed = 0;
        if (parseUniName(bare, parsed)) {
          std::string named = unicodeToAglName(parsed);
          if (!named.empty()) {
            bare = named;
            item = QPDFObjectHandle::newName("/" + named);
            removedAny = true;
          }
        }
        uint32_t uni = aglNameToUnicode(bare);
        bool emptySlot = bare == ".notdef" && pendingCode >= 0 && pendingCode < 256 && !winAnsiToUnicode(pendingCode);
        bool ok = !emptySlot && (uni != 0 || parseUniName(bare, parsed) || bare == ".notdef");
        if (ok) {
          if (!codeWritten) {
            nd.appendItem(QPDFObjectHandle::newInteger(pendingCode));
            codeWritten = true;
          }
          nd.appendItem(item);
          ++pendingCode;
        } else {
          removedAny = true;
          ++pendingCode;
          codeWritten = false;
        }
      }
      if (removedAny) {
        if (nd.getArrayNItems() == 0) {
          enc.removeKey("/Differences");
        } else {
          enc.replaceKey("/Differences", nd);
        }
        ctx.issue("ENCODING_DIFFS_CLEANED",
                  "replaced or removed non-standard glyph names in /Differences of " +
                      nameOf(font.getKey("/BaseFont")),
                  true);
      }
    }
    return;
  }
  font.replaceKey("/Encoding", QPDFObjectHandle::newName("/WinAnsiEncoding"));
  ctx.issue("TTF_ENCODING_SET",
            "set /Encoding /WinAnsiEncoding on non-symbolic TrueType font " +
                nameOf(font.getKey("/BaseFont")),
            true);
}

void embedFallbackCMap(Ctx& ctx, QPDFObjectHandle type0) {
  QPDFObjectHandle enc = type0.getKey("/Encoding");
  if (!enc.isName()) return;
  std::string name = enc.getName();
  if (name == "/Identity-H" || name == "/Identity-V") return;
  for (const auto& entry : kPredefRos) {
    if (name.rfind(entry.prefix, 0) == 0) return;
  }
  QPDFObjectHandle df = type0.getKey("/DescendantFonts");
  if (!df.isArray() || df.getArrayNItems() != 1) return;
  QPDFObjectHandle ros = df.getArrayItem(0).getKey("/CIDSystemInfo");
  std::string reg = ros.isDictionary() && ros.getKey("/Registry").isString()
                        ? ros.getKey("/Registry").getStringValue()
                        : "Adobe";
  std::string ord = ros.isDictionary() && ros.getKey("/Ordering").isString()
                        ? ros.getKey("/Ordering").getStringValue()
                        : "Identity";
  long long supp = ros.isDictionary() && ros.getKey("/Supplement").isInteger()
                       ? ros.getKey("/Supplement").getIntValue()
                       : 0;
  std::string bare = name.substr(1);
  std::string body;
  body += "%!PS-Adobe-3.0 Resource-CMap\n/CIDInit /ProcSet findresource begin\n";
  body += "12 dict begin\nbegincmap\n";
  body += "/CIDSystemInfo 3 dict dup begin\n  /Registry (" + reg + ") def\n  /Ordering (" +
          ord + ") def\n  /Supplement " + std::to_string(supp) + " def\nend def\n";
  body += "/CMapName /" + bare + " def\n/CMapType 1 def\n/WMode 0 def\n";
  body += "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
  body += "1 begincidrange\n<0000> <FFFF> 0\nendcidrange\n";
  body += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
  QPDFObjectHandle stream = QPDFObjectHandle::newStream(&ctx.pdf, body);
  QPDFObjectHandle sd = stream.getDict();
  sd.replaceKey("/Type", QPDFObjectHandle::newName("/CMap"));
  sd.replaceKey("/CMapName", QPDFObjectHandle::newName("/" + bare));
  QPDFObjectHandle nr = QPDFObjectHandle::newDictionary();
  nr.replaceKey("/Registry", QPDFObjectHandle::newString(reg));
  nr.replaceKey("/Ordering", QPDFObjectHandle::newString(ord));
  nr.replaceKey("/Supplement", QPDFObjectHandle::newInteger(supp));
  sd.replaceKey("/CIDSystemInfo", nr);
  sd.replaceKey("/WMode", QPDFObjectHandle::newInteger(0));
  QPDFObjectHandle ref = ctx.pdf.makeIndirectObject(stream);
  type0.replaceKey("/Encoding", ref);
  ctx.identityCmaps.insert(ref.getObjGen());
  ctx.issue("CMAP_FALLBACK_EMBEDDED",
            "embedded an identity fallback for unresolvable CMap " + name +
                " (character-to-glyph mapping approximated; visual difference possible)",
            true);
}

void fixEmbeddedCMap(Ctx& ctx, QPDFObjectHandle type0) {
  QPDFObjectHandle enc = type0.getKey("/Encoding");
  if (!enc.isStream()) return;
  QPDFObjectHandle df = type0.getKey("/DescendantFonts");
  if (!df.isArray() || df.getArrayNItems() != 1) return;
  QPDFObjectHandle ros = df.getArrayItem(0).getKey("/CIDSystemInfo");
  std::string reg, ord;
  long long rosSupp = 0;
  if (ros.isDictionary()) {
    if (ros.getKey("/Registry").isString()) reg = ros.getKey("/Registry").getStringValue();
    if (ros.getKey("/Ordering").isString()) ord = ros.getKey("/Ordering").getStringValue();
    if (ros.getKey("/Supplement").isInteger()) {
      rosSupp = ros.getKey("/Supplement").getIntValue();
    }
  }
  QPDFObjectHandle encName = enc.getDict().getKey("/CMapName");
  bool identityName = nameIs(encName, "/Identity-H") || nameIs(encName, "/Identity-V");
  if (!reg.empty() && !ord.empty()) {
    QPDFObjectHandle cd = enc.getDict().getKey("/CIDSystemInfo");
    std::string creg = cd.isDictionary() && cd.getKey("/Registry").isString()
                           ? cd.getKey("/Registry").getStringValue()
                           : "";
    std::string cord = cd.isDictionary() && cd.getKey("/Ordering").isString()
                           ? cd.getKey("/Ordering").getStringValue()
                           : "";
    long long csupp = cd.isDictionary() && cd.getKey("/Supplement").isInteger()
                          ? cd.getKey("/Supplement").getIntValue()
                          : -1;
    if (creg != reg || cord != ord || csupp < rosSupp) {
      QPDFObjectHandle nr = QPDFObjectHandle::newDictionary();
      nr.replaceKey("/Registry", QPDFObjectHandle::newString(reg));
      nr.replaceKey("/Ordering", QPDFObjectHandle::newString(ord));
      nr.replaceKey("/Supplement",
                    QPDFObjectHandle::newInteger(csupp > rosSupp ? csupp : rosSupp));
      enc.getDict().replaceKey("/CIDSystemInfo", nr);
      ctx.issue("CMAP_ROS_SYNCED",
                "aligned embedded CMap /CIDSystemInfo dictionary with the CIDFont",
                !ctx.opt.verifyOnly || (!identityName && (creg != reg || cord != ord || (ctx.part >= 2 && csupp < rosSupp))));
    }
  }
  std::string text;
  try {
    auto buf = enc.getStreamData(qpdf_dl_all);
    text.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return;
  }
  bool changed = false;
  auto patchLiteral = [&](const std::string& key, const std::string& val) {
    size_t p = text.find("/" + key);
    while (p != std::string::npos) {
      size_t o = text.find('(', p);
      if (o != std::string::npos && o - p < 40) {
        size_t c = text.find(')', o);
        if (c != std::string::npos) {
          if (text.substr(o + 1, c - o - 1) != val) {
            text = text.substr(0, o + 1) + val + text.substr(c);
            changed = true;
          }
          return;
        }
      }
      p = text.find("/" + key, p + 1);
    }
  };
  if (!reg.empty() && !ctx.opt.verifyOnly) patchLiteral("Registry", reg);
  if (!ord.empty() && !ctx.opt.verifyOnly) patchLiteral("Ordering", ord);
  static const std::set<std::string> kPredefinedCMaps = {
      "/GB-EUC-H", "/GB-EUC-V", "/GBpc-EUC-H", "/GBpc-EUC-V", "/GBK-EUC-H", "/GBK-EUC-V", "/GBKp-EUC-H",
      "/GBKp-EUC-V", "/GBK2K-H", "/GBK2K-V", "/UniGB-UCS2-H", "/UniGB-UCS2-V", "/UniGB-UTF16-H",
      "/UniGB-UTF16-V", "/B5pc-H", "/B5pc-V", "/HKscs-B5-H", "/HKscs-B5-V", "/ETen-B5-H", "/ETen-B5-V",
      "/ETenms-B5-H", "/ETenms-B5-V", "/CNS-EUC-H", "/CNS-EUC-V", "/UniCNS-UCS2-H", "/UniCNS-UCS2-V",
      "/UniCNS-UTF16-H", "/UniCNS-UTF16-V", "/83pv-RKSJ-H", "/90ms-RKSJ-H", "/90ms-RKSJ-V",
      "/90msp-RKSJ-H", "/90msp-RKSJ-V", "/90pv-RKSJ-H", "/Add-RKSJ-H", "/Add-RKSJ-V", "/EUC-H", "/EUC-V",
      "/Ext-RKSJ-H", "/Ext-RKSJ-V", "/H", "/V", "/UniJIS-UCS2-H", "/UniJIS-UCS2-V", "/UniJIS-UCS2-HW-H",
      "/UniJIS-UCS2-HW-V", "/UniJIS-UTF16-H", "/UniJIS-UTF16-V", "/KSC-EUC-H", "/KSC-EUC-V",
      "/KSCms-UHC-H", "/KSCms-UHC-V", "/KSCms-UHC-HW-H", "/KSCms-UHC-HW-V", "/KSCpc-EUC-H",
      "/UniKS-UCS2-H", "/UniKS-UCS2-V", "/UniKS-UTF16-H", "/UniKS-UTF16-V"};
  {
    size_t uc = text.find("usecmap");
    if (uc != std::string::npos) {
      size_t lineStart = text.rfind('\n', uc);
      lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
      size_t nameStart = text.rfind('/', uc);
      bool standardRef = false;
      if (nameStart != std::string::npos && nameStart >= lineStart) {
        std::string refName = text.substr(nameStart, uc - nameStart);
        if (refName.find("Identity") != std::string::npos) standardRef = true;
        while (!refName.empty() && std::isspace(static_cast<unsigned char>(refName.back()))) refName.pop_back();
        if (ctx.part >= 2 && kPredefinedCMaps.count(refName)) standardRef = true;
      }
      if (!standardRef) {
        text.erase(lineStart, uc + 7 - lineStart);
        changed = true;
        if (enc.getDict().hasKey("/UseCMap")) enc.getDict().removeKey("/UseCMap");
      }
    } else if (enc.getDict().hasKey("/UseCMap")) {
      QPDFObjectHandle ref = enc.getDict().getKey("/UseCMap");
      std::string refName = ref.isName() ? ref.getName() : std::string();
      if (ref.isStream() && ref.getDict().getKey("/CMapName").isName()) {
        refName = ref.getDict().getKey("/CMapName").getName();
      }
      if (refName.find("Identity") == std::string::npos) {
        bool real = !ctx.opt.verifyOnly || ctx.part == 1 || !kPredefinedCMaps.count(refName);
        if (real) enc.getDict().removeKey("/UseCMap");
        ctx.issue("CMAP_USECMAP_REMOVED",
                  "removed /UseCMap reference not used by the embedded CMap program", real);
      }
    }
    size_t wm = text.find("/WMode");
    {
      int v = wm == std::string::npos ? 0 : std::atoi(text.c_str() + wm + 6);
      QPDFObjectHandle cur = enc.getDict().getKey("/WMode");
      long long dictW = cur.isInteger() ? cur.getIntValue() : 0;
      if ((v == 0 || v == 1) && dictW != v) {
        enc.getDict().replaceKey("/WMode", QPDFObjectHandle::newInteger(v));
        ctx.issue("CMAP_WMODE_SYNCED",
                  "synchronized CMap dictionary /WMode with embedded program", true);
      }
    }
  }
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  bool inCidRange = false;
  std::vector<std::string> pend;
  auto hexVal = [](const std::string& h) {
    return std::strtoll(h.c_str(), nullptr, 16);
  };
  while (i < text.size()) {
    char c = text[i];
    if (std::isalpha(static_cast<unsigned char>(c))) {
      size_t j = i;
      while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])))) ++j;
      std::string word = text.substr(i, j - i);
      if (word == "begincidrange") inCidRange = true;
      if (word == "endcidrange") inCidRange = false;
      out.append(word);
      i = j;
      continue;
    }
    if (inCidRange && c == '<') {
      size_t j = text.find('>', i);
      if (j != std::string::npos && j - i <= 10) {
        pend.push_back(text.substr(i + 1, j - i - 1));
        if (pend.size() > 2) pend.erase(pend.begin());
        out.append(text, i, j - i + 1);
        i = j + 1;
        continue;
      }
    }
    if (std::isdigit(static_cast<unsigned char>(c)) &&
        (i == 0 || (!std::isdigit(static_cast<unsigned char>(text[i - 1])) &&
                    text[i - 1] != '.' && text[i - 1] != '#'))) {
      size_t j = i;
      while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
      if (j < text.size() && text[j] == '.') {
        out.append(text, i, j - i);
        i = j;
        continue;
      }
      std::string num = text.substr(i, j - i);
      if (num.size() <= 10) {
        long long v = std::strtoll(num.c_str(), nullptr, 10);
        long long maxStart = 65535;
        if (inCidRange && pend.size() == 2) {
          long long span = hexVal(pend[1]) - hexVal(pend[0]);
          if (span > 0 && span < 65536) maxStart = 65535 - span;
          if (maxStart < 0) maxStart = 0;
        }
        if (v > maxStart) {
          out += std::to_string(maxStart);
          changed = true;
          i = j;
          continue;
        }
      }
      out.append(text, i, j - i);
      i = j;
      continue;
    }
    out += c;
    ++i;
  }
  if (changed) {
    if (ros.isDictionary()) {
      enc.getDict().replaceKey("/CIDSystemInfo", ros);
    }
    enc.replaceStreamData(out, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    enc.getDict().removeKey("/Filter");
    enc.getDict().removeKey("/DecodeParms");
    ctx.issue("CMAP_REPAIRED",
              "repaired embedded CMap of " + nameOf(type0.getKey("/BaseFont")) +
                  " (CIDSystemInfo sync, CID clamp)",
              true);
  }
}

double type3ProcWidth(QPDFObjectHandle glyph) {
  if (!glyph.isStream()) return -1;
  std::string data;
  try {
    auto buf = glyph.getStreamData(qpdf_dl_all);
    data.assign(reinterpret_cast<const char*>(buf->getBuffer()), std::min<size_t>(buf->getSize(), 512));
  } catch (...) {
    return -1;
  }
  std::vector<double> nums;
  size_t i = 0;
  while (i < data.size() && nums.size() < 8) {
    while (i < data.size() && std::isspace(static_cast<unsigned char>(data[i]))) ++i;
    size_t j = i;
    while (j < data.size() && !std::isspace(static_cast<unsigned char>(data[j]))) ++j;
    if (j == i) break;
    std::string tok = data.substr(i, j - i);
    if (tok == "d0" || tok == "d1") return nums.empty() ? -1 : nums[0];
    char* end = nullptr;
    double v = std::strtod(tok.c_str(), &end);
    if (!end || *end != '\0') break;
    nums.push_back(v);
    i = j;
  }
  return -1;
}

void syncType3Widths(Ctx& ctx, QPDFObjectHandle font, const FontUse* use) {
  QPDFObjectHandle cp = font.getKey("/CharProcs");
  QPDFObjectHandle widths = font.getKey("/Widths");
  QPDFObjectHandle firstObj = font.getKey("/FirstChar");
  QPDFObjectHandle lastObj = font.getKey("/LastChar");
  bool keysOk = firstObj.isNumber() && lastObj.isNumber() && widths.isArray();
  SimpleEncoding enc = readEncoding(font, false);
  if (use) {
    QPDFObjectHandle fd = font.getKey("/FontDescriptor");
    double missing = fd.isDictionary() && fd.getKey("/MissingWidth").isNumber() ? fd.getKey("/MissingWidth").getNumericValue() : 0.0;
    int first = keysOk ? static_cast<int>(firstObj.getNumericValue()) : 0;
    int last = keysOk ? static_cast<int>(lastObj.getNumericValue()) : -1;
    int n = keysOk ? widths.getArrayNItems() : 0;
    int bad = 0;
    for (uint32_t code : use->visibleCodes) {
      if (code > 255) continue;
      int c = static_cast<int>(code);
      bool inRange = n > 0 && c >= first && c <= last;
      QPDFObjectHandle w = inRange && c - first < n ? widths.getArrayItem(c - first) : QPDFObjectHandle::newNull();
      double dictW = inRange ? (w.isNumber() ? w.getNumericValue() : 0.0) : missing;
      const std::string& gname = enc.diffs[code];
      double progW = cp.isDictionary() && !gname.empty() ? type3ProcWidth(cp.getKey(gname)) : -1;
      if (progW < 0) progW = missing;
      if (std::fabs(dictW - progW) > 1.0) ++bad;
    }
    if (bad) {
      ctx.issue("TYPE3_WIDTHS_SYNCED", std::to_string(bad) + " glyph width(s) of a Type 3 font differ from its glyph procedures", true);
    }
    return;
  }
  if (!cp.isDictionary()) return;
  bool outside = false;
  for (int c = 0; keysOk && c < 256; ++c) {
    int lo = static_cast<int>(firstObj.getNumericValue());
    if (!enc.diffs[c].empty() && cp.getKey(enc.diffs[c]).isStream() && (c < lo || c - lo >= widths.getArrayNItems())) outside = true;
  }
  if (!keysOk || outside) {
    QPDFObjectHandle full = QPDFObjectHandle::newArray();
    int lo = keysOk ? static_cast<int>(firstObj.getNumericValue()) : 0;
    for (int c = 0; c < 256; ++c) {
      QPDFObjectHandle old = keysOk && c >= lo && c - lo < widths.getArrayNItems() ? widths.getArrayItem(c - lo) : QPDFObjectHandle::newNull();
      full.appendItem(old.isNumber() ? old : QPDFObjectHandle::newInteger(0));
    }
    widths = full;
    font.replaceKey("/FirstChar", QPDFObjectHandle::newInteger(0));
    font.replaceKey("/LastChar", QPDFObjectHandle::newInteger(255));
    font.replaceKey("/Widths", widths);
  }
  int first = static_cast<int>(font.getKey("/FirstChar").getNumericValue());
  bool changed = !keysOk || outside;
  for (int idx = 0; idx < widths.getArrayNItems(); ++idx) {
    int code = first + idx;
    if (code < 0 || code > 255) break;
    const std::string& gname = enc.diffs[code];
    if (gname.empty()) continue;
    double w = type3ProcWidth(cp.getKey(gname));
    if (w < 0) continue;
    double cur = numOf(widths.getArrayItem(idx), -1);
    if (std::fabs(cur - w) > 0.1) {
      widths.setArrayItem(idx, QPDFObjectHandle::newReal(w, 2));
      changed = true;
    }
  }
  if (changed) {
    ctx.issue("TYPE3_WIDTHS_SYNCED",
              "synchronized Type 3 font /Widths with glyph procedure metrics", true);
  }
}

int symbolicCmapAction(FtLib& lib, QPDFObjectHandle font, bool verifyOnly) {
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  if (!fd.isDictionary() || !fd.getKey("/Flags").isInteger()) return 0;
  long long flags = fd.getKey("/Flags").getIntValue();
  if (!(flags & 4)) return 0;
  QPDFObjectHandle program = fontFileStream(fd);
  if (!program.isStream()) return 0;
  FtFace face;
  if (!loadFace(lib, program, face)) return 0;
  int n = face.face->num_charmaps;
  if (n == 1) return findCmap(face.face, 3, 1) && !verifyOnly ? 1 : 0;
  for (int i = 0; i < n; ++i) {
    FT_CharMap cm = face.face->charmaps[i];
    if (cm->platform_id == 3 && cm->encoding_id == 1) return 1;
  }
  return 2;
}

bool programHasCmap(FtLib& lib, QPDFObjectHandle font, int platform, int encoding) {
  QPDFObjectHandle program = fontFileStream(font.getKey("/FontDescriptor"));
  if (!program.isStream()) return true;
  FtFace face;
  if (!loadFace(lib, program, face)) return true;
  for (int i = 0; i < face.face->num_charmaps; ++i) {
    FT_CharMap cm = face.face->charmaps[i];
    if (cm->platform_id == platform && cm->encoding_id == encoding) return true;
  }
  return false;
}

bool addMsUnicodeCmap(QPDFObjectHandle font) {
  QPDFObjectHandle program = fontFileStream(font.getKey("/FontDescriptor"));
  if (!program.isStream()) return false;
  std::string f;
  try {
    auto buf = program.getStreamData(qpdf_dl_all);
    f.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return false;
  }
  auto u16 = [&](size_t o) { return o + 2 <= f.size() ? (static_cast<unsigned char>(f[o]) << 8) | static_cast<unsigned char>(f[o + 1]) : 0; };
  auto u32 = [&](size_t o) { return o + 4 <= f.size() ? (static_cast<uint32_t>(u16(o)) << 16) | u16(o + 2) : 0u; };
  auto p16 = [](std::string& s, size_t o, uint32_t v) { s[o] = static_cast<char>(v >> 8); s[o + 1] = static_cast<char>(v); };
  auto p32 = [&](std::string& s, size_t o, uint32_t v) { p16(s, o, v >> 16); p16(s, o + 2, v & 0xFFFF); };
  if (f.size() < 12) return false;
  int numTables = u16(4);
  size_t dir = 0;
  for (int i = 0; i < numTables; ++i) {
    if (f.compare(12 + 16 * i, 4, "cmap") == 0) dir = 12 + 16 * i;
  }
  if (!dir) return false;
  size_t off = u32(dir + 8), len = u32(dir + 12);
  if (off + len > f.size() || len < 4) return false;
  std::string cmap = f.substr(off, len);
  int n = (static_cast<unsigned char>(cmap[2]) << 8) | static_cast<unsigned char>(cmap[3]);
  if (4 + 8 * static_cast<size_t>(n) > cmap.size()) return false;
  uint32_t target = 0;
  for (int i = 0; i < n; ++i) {
    size_t r = 4 + 8 * i;
    int plat = (static_cast<unsigned char>(cmap[r]) << 8) | static_cast<unsigned char>(cmap[r + 1]);
    int enc = (static_cast<unsigned char>(cmap[r + 2]) << 8) | static_cast<unsigned char>(cmap[r + 3]);
    uint32_t so = u32(off + r + 4);
    if (plat == 3 && (enc == 1 || enc == 0)) return false;
    if (plat == 0 && so + 2 <= len && u16(off + so) == 4 && !target) target = so;
  }
  if (!target) return false;
  std::string out = cmap.substr(0, 4 + 8 * n);
  for (int i = 0; i < n; ++i) p32(out, 4 + 8 * i + 4, u32(off + 4 + 8 * i + 4) + 8);
  std::string rec(8, '\0');
  p16(rec, 0, 3);
  p16(rec, 2, 1);
  p32(rec, 4, target + 8);
  out += rec + cmap.substr(4 + 8 * n);
  p16(out, 2, n + 1);
  while (f.size() % 4) f.push_back('\0');
  uint32_t sum = 0;
  std::string padded = out;
  while (padded.size() % 4) padded.push_back('\0');
  for (size_t i = 0; i < padded.size(); ++i) sum += static_cast<uint32_t>(static_cast<unsigned char>(padded[i])) << (24 - 8 * (i % 4));
  p32(f, dir + 4, sum);
  p32(f, dir + 8, static_cast<uint32_t>(f.size()));
  p32(f, dir + 12, static_cast<uint32_t>(out.size()));
  f += padded;
  program.replaceStreamData(f, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
  program.getDict().replaceKey("/Length1", QPDFObjectHandle::newInteger(static_cast<long long>(f.size())));
  return true;
}

bool embeddingRestricted(FtLib& lib, QPDFObjectHandle font, const std::string& subtype) {
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  if (subtype == "/Type0") {
    QPDFObjectHandle df = font.getKey("/DescendantFonts");
    if (!df.isArray() || df.getArrayNItems() != 1) return false;
    fd = df.getArrayItem(0).getKey("/FontDescriptor");
  }
  QPDFObjectHandle program = fontFileStream(fd);
  if (!program.isStream()) return false;
  FtFace face;
  if (!loadFace(lib, program, face)) return false;
  FT_UShort fs = FT_Get_FSType_Flags(face.face);
  return (fs & FT_FSTYPE_RESTRICTED_LICENSE_EMBEDDING) &&
         !(fs & (FT_FSTYPE_PREVIEW_AND_PRINT_EMBEDDING | FT_FSTYPE_EDITABLE_EMBEDDING |
                 FT_FSTYPE_BITMAP_EMBEDDING_ONLY));
}

bool programUsable(FtLib& lib, QPDFObjectHandle font, const std::string& subtype) {
  QPDFObjectHandle fd;
  if (subtype == "/Type0") {
    QPDFObjectHandle df = font.getKey("/DescendantFonts");
    if (!df.isArray() || df.getArrayNItems() != 1) return true;
    fd = df.getArrayItem(0).getKey("/FontDescriptor");
  } else {
    fd = font.getKey("/FontDescriptor");
  }
  QPDFObjectHandle program = fontFileStream(fd);
  if (!program.isStream()) return false;
  FtFace face;
  if (!loadFace(lib, program, face)) return false;
  return face.face->num_glyphs > 0;
}

std::string utf16Hex(uint32_t uni) {
  char buf[16];
  if (uni > 0xFFFF) {
    uint32_t v = uni - 0x10000;
    std::snprintf(buf, sizeof(buf), "%04X%04X", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
  } else {
    std::snprintf(buf, sizeof(buf), "%04X", uni);
  }
  return buf;
}

std::map<uint32_t, uint32_t> reverseCmap(FT_Face face) {
  std::map<uint32_t, uint32_t> gidToUni;
  if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) return gidToUni;
  FT_UInt gid = 0;
  FT_ULong ch = FT_Get_First_Char(face, &gid);
  while (gid != 0) {
    gidToUni.emplace(gid, static_cast<uint32_t>(ch));
    ch = FT_Get_Next_Char(face, ch, &gid);
  }
  return gidToUni;
}

bool parseHexToken(const std::string& text, size_t& pos, std::string& hex) {
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
  if (pos >= text.size() || text[pos] != '<') return false;
  size_t end = text.find('>', pos);
  if (end == std::string::npos) return false;
  hex = text.substr(pos + 1, end - pos - 1);
  pos = end + 1;
  return true;
}

std::map<uint32_t, std::string> parseExistingToUnicode(QPDFObjectHandle tu) {
  std::map<uint32_t, std::string> out;
  if (!tu.isStream()) return out;
  std::string text;
  try {
    auto buf = tu.getStreamData(qpdf_dl_all);
    text.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return out;
  }
  size_t pos = 0;
  while ((pos = text.find("beginbfchar", pos)) != std::string::npos) {
    pos += 11;
    size_t blockEnd = text.find("endbfchar", pos);
    if (blockEnd == std::string::npos) break;
    size_t p = pos;
    std::string src, dst;
    while (p < blockEnd && parseHexToken(text, p, src) && p < blockEnd &&
           parseHexToken(text, p, dst)) {
      if (src.size() <= 8) {
        out[static_cast<uint32_t>(std::strtoul(src.c_str(), nullptr, 16))] = dst;
      }
    }
    pos = blockEnd + 9;
  }
  pos = 0;
  while ((pos = text.find("beginbfrange", pos)) != std::string::npos) {
    pos += 12;
    size_t blockEnd = text.find("endbfrange", pos);
    if (blockEnd == std::string::npos) break;
    size_t p = pos;
    while (p < blockEnd) {
      std::string lo, hi;
      if (!parseHexToken(text, p, lo) || !parseHexToken(text, p, hi)) break;
      while (p < blockEnd && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
      uint32_t loV = static_cast<uint32_t>(std::strtoul(lo.c_str(), nullptr, 16));
      uint32_t hiV = static_cast<uint32_t>(std::strtoul(hi.c_str(), nullptr, 16));
      if (hiV < loV || hiV - loV > 65535) break;
      if (p < blockEnd && text[p] == '[') {
        ++p;
        for (uint32_t c = loV; c <= hiV && p < blockEnd; ++c) {
          std::string dst;
          if (!parseHexToken(text, p, dst)) break;
          out[c] = dst;
          while (p < blockEnd && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
        }
        if (p < blockEnd && text[p] == ']') ++p;
      } else {
        std::string dst;
        if (!parseHexToken(text, p, dst)) break;
        uint32_t base = static_cast<uint32_t>(std::strtoul(dst.c_str(), nullptr, 16));
        bool simpleInc = dst.size() <= 4;
        for (uint32_t c = loV; c <= hiV; ++c) {
          if (simpleInc) {
            out[c] = utf16Hex(base + (c - loV));
          } else {
            out[c] = dst;
          }
        }
      }
      while (p < blockEnd && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    }
    pos = blockEnd + 10;
  }
  return out;
}

std::string buildCMapStream(const std::map<uint32_t, std::string>& entries, bool twoByte) {
  std::string cs = twoByte ? "<0000> <FFFF>" : "<00> <FF>";
  std::string s;
  s += "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n";
  s += "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n";
  s += "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n";
  s += "1 begincodespacerange\n" + cs + "\nendcodespacerange\n";
  std::vector<std::pair<uint32_t, std::string>> flat(entries.begin(), entries.end());
  size_t i = 0;
  char buf[32];
  while (i < flat.size()) {
    size_t n = std::min<size_t>(100, flat.size() - i);
    s += std::to_string(n) + " beginbfchar\n";
    for (size_t j = i; j < i + n; ++j) {
      if (twoByte) {
        std::snprintf(buf, sizeof(buf), "<%04X> <", flat[j].first);
      } else {
        std::snprintf(buf, sizeof(buf), "<%02X> <", flat[j].first);
      }
      s += buf;
      s += flat[j].second;
      s += ">\n";
    }
    s += "endbfchar\n";
    i += n;
  }
  s += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
  return s;
}

uint32_t deriveSimpleUnicode(FT_Face face, const std::map<uint32_t, uint32_t>& gidToUni,
                             int code, const SimpleEncoding& enc, bool symbolic) {
  {
    const std::string& diffName = enc.diffs[code];
    if (!diffName.empty()) {
      uint32_t uni = aglNameToUnicode(diffName.substr(1));
      if (uni) return uni;
    }
  }
  if (enc.base) {
    uint16_t uni = enc.base(code);
    if (uni) return uni;
  }
  if (face && FT_HAS_GLYPH_NAMES(face)) {
    FT_UInt gid0 = FT_Get_Char_Index(face, static_cast<FT_ULong>(code));
    char gname[64];
    if (gid0 && FT_Get_Glyph_Name(face, gid0, gname, sizeof(gname)) == 0 && gname[0]) {
      uint32_t uni = aglNameToUnicode(gname);
      if (!uni) parseUniName(gname, uni);
      if (uni) return uni;
    }
  }
  if (face) {
    FT_UInt gid = glyphForCode(face, code, enc, symbolic);
    auto it = gidToUni.find(gid);
    if (gid && it != gidToUni.end()) return it->second;
  }
  {
    uint16_t win = winAnsiToUnicode(code);
    if (win) return win;
  }
  return 0xFFFD;
}

void ensureSimpleToUnicode(Ctx& ctx, FtLib& lib, QPDFObjectHandle font) {
  std::map<uint32_t, std::string> entries = parseExistingToUnicode(font.getKey("/ToUnicode"));
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  QPDFObjectHandle program = fontFileStream(fd);
  FtFace face;
  bool haveFace = program.isStream() && loadFace(lib, program, face);
  std::map<uint32_t, uint32_t> gidToUni;
  if (haveFace) gidToUni = reverseCmap(face.face);
  long long flags = fd.isDictionary() && fd.getKey("/Flags").isInteger()
                        ? fd.getKey("/Flags").getIntValue()
                        : 0;
  bool symbolic = (flags & 4) != 0;
  SimpleEncoding enc = readEncoding(font, symbolic);
  int added = 0;
  for (int code = 0; code < 256; ++code) {
    auto it = entries.find(static_cast<uint32_t>(code));
    if (it != entries.end() && !it->second.empty() && it->second != "0000") continue;
    uint32_t uni = deriveSimpleUnicode(haveFace ? face.face : nullptr, gidToUni, code, enc,
                                       symbolic);
    if (uni >= 0xE000 && uni <= 0xF8FF) {
      uint32_t low = uni & 0xFF;
      uint16_t win = winAnsiToUnicode(static_cast<int>(low));
      if ((uni & 0xFF00) == 0xF000 && win) {
        uni = win;
      } else {
        uint16_t byCode = winAnsiToUnicode(code);
        if (byCode) uni = byCode;
      }
    }
    entries[static_cast<uint32_t>(code)] = utf16Hex(uni);
    ++added;
  }
  if (!added) return;
  QPDFObjectHandle stream = QPDFObjectHandle::newStream(&ctx.pdf, buildCMapStream(entries, false));
  font.replaceKey("/ToUnicode", ctx.pdf.makeIndirectObject(stream));
  ctx.issue("TOUNICODE_BUILT",
            "completed ToUnicode mapping for " + nameOf(font.getKey("/BaseFont")), true);
}

void ensureCidToUnicode(Ctx& ctx, FtLib& lib, QPDFObjectHandle type0) {
  QPDFObjectHandle df = type0.getKey("/DescendantFonts");
  if (!df.isArray() || df.getArrayNItems() != 1 || !df.getArrayItem(0).isDictionary()) return;
  QPDFObjectHandle cidFont = df.getArrayItem(0);
  std::map<uint32_t, std::string> entries = parseExistingToUnicode(type0.getKey("/ToUnicode"));
  QPDFObjectHandle program = fontFileStream(cidFont.getKey("/FontDescriptor"));
  FtFace face;
  bool haveFace = program.isStream() && loadFace(lib, program, face);
  std::map<uint32_t, uint32_t> gidToUni;
  if (haveFace) gidToUni = reverseCmap(face.face);

  QPDFObjectHandle c2g = cidFont.getKey("/CIDToGIDMap");
  std::string mapData;
  if (c2g.isStream()) {
    try {
      auto buf = c2g.getStreamData(qpdf_dl_all);
      mapData.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    } catch (...) {
    }
  }
  size_t numCids = 65536;
  if (!mapData.empty()) {
    numCids = mapData.size() / 2;
  } else if (haveFace) {
    numCids = static_cast<size_t>(face.face->num_glyphs);
  }
  if (numCids > 65536) numCids = 65536;

  QPDFObjectHandle csi = cidFont.getKey("/CIDSystemInfo");
  std::string ordering = csi.isDictionary() && csi.getKey("/Ordering").isString() ? csi.getKey("/Ordering").getUTF8Value() : "";
  std::vector<CmapSpace> spaces;
  std::map<uint32_t, int> code2cid;
  std::string cmapOrdering;
  bool coded = loadEncodingCmap(type0.getKey("/Encoding"), spaces, code2cid, &cmapOrdering);
  if (coded && orderingCidCount(ordering) == 0) ordering = cmapOrdering;
  std::vector<std::pair<uint32_t, size_t>> pairs;
  if (coded) {
    for (const auto& kv : code2cid) {
      if (kv.second >= 0 && static_cast<size_t>(kv.second) < numCids) pairs.push_back({kv.first, static_cast<size_t>(kv.second)});
    }
  } else {
    for (size_t cid = 0; cid < numCids; ++cid) pairs.push_back({static_cast<uint32_t>(cid), cid});
  }

  int added = 0;
  for (const auto& pc : pairs) {
    size_t cid = pc.second;
    auto it = entries.find(pc.first);
    if (it != entries.end() && !it->second.empty() && it->second != "0000") continue;
    uint32_t gid = static_cast<uint32_t>(cid);
    if (!mapData.empty()) {
      gid = static_cast<uint32_t>((static_cast<unsigned char>(mapData[cid * 2]) << 8) |
                                  static_cast<unsigned char>(mapData[cid * 2 + 1]));
    }
    if (gid == 0 && cid != 0) continue;
    if (haveFace && gid >= static_cast<uint32_t>(face.face->num_glyphs)) continue;
    uint32_t uni = 0;
    auto rit = gidToUni.find(gid);
    if (gid && rit != gidToUni.end()) {
      uni = rit->second;
      if (uni >= 0xE000 && uni <= 0xF8FF) {
        uint16_t win = (uni & 0xFF00) == 0xF000
                           ? winAnsiToUnicode(static_cast<int>(uni & 0xFF))
                           : 0;
        uni = win ? win : 0xFFFD;
      }
    } else if (uint32_t mapped = orderingCidToUnicode(ordering, static_cast<uint32_t>(cid))) {
      uni = mapped;
    } else if (!coded && cid >= 0x20 && cid <= 0x10FFFF && !(cid >= 0xD800 && cid <= 0xDFFF)) {
      uni = static_cast<uint32_t>(cid);
    } else {
      uni = 0xFFFD;
    }
    entries[pc.first] = utf16Hex(uni);
    ++added;
  }
  if (!added) return;
  QPDFObjectHandle stream = QPDFObjectHandle::newStream(&ctx.pdf, buildCMapStream(entries, true));
  type0.replaceKey("/ToUnicode", ctx.pdf.makeIndirectObject(stream));
  ctx.issue("TOUNICODE_BUILT",
            "completed ToUnicode mapping for " + nameOf(type0.getKey("/BaseFont")), true);
}

bool charSetConsistent(FtLib& lib, QPDFObjectHandle font, const FontUse* usedOnly) {
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  QPDFObjectHandle cs = fd.isDictionary() ? fd.getKey("/CharSet") : QPDFObjectHandle::newNull();
  if (!cs.isString()) return false;
  FtFace face;
  if (!loadFace(lib, fontFileStream(fd), face) || !FT_HAS_GLYPH_NAMES(face.face)) return true;
  std::set<std::string> listed;
  std::string text = cs.getStringValue();
  for (size_t i = 0; i < text.size();) {
    if (text[i] != '/') {
      ++i;
      continue;
    }
    size_t e = text.find('/', i + 1);
    if (e == std::string::npos) e = text.size();
    std::string nm = text.substr(i + 1, e - i - 1);
    while (!nm.empty() && std::isspace(static_cast<unsigned char>(nm.back()))) nm.pop_back();
    if (!nm.empty()) listed.insert(nm);
    i = e;
  }
  std::set<std::string> present;
  char name[128];
  if (usedOnly) {
    long long flags = fd.getKey("/Flags").isInteger() ? fd.getKey("/Flags").getIntValue() : 0;
    bool symbolic = (flags & 4) != 0;
    SimpleEncoding enc = readEncoding(font, symbolic);
    for (uint32_t code : usedOnly->codes) {
      if (code > 255) continue;
      FT_UInt gid = glyphForCode(face.face, static_cast<int>(code), enc, symbolic);
      if (gid && FT_Get_Glyph_Name(face.face, gid, name, sizeof(name)) == 0 && name[0]) present.insert(name);
    }
  }
  for (long gid = 0; !usedOnly && gid < face.face->num_glyphs; ++gid) {
    if (FT_Get_Glyph_Name(face.face, static_cast<FT_UInt>(gid), name, sizeof(name)) == 0 && name[0] &&
        std::strcmp(name, ".notdef") != 0) {
      present.insert(name);
    }
  }
  for (const std::string& g : present) {
    if (!listed.count(g)) return false;
  }
  for (const std::string& g : listed) {
    if (!usedOnly && g != ".notdef" && !present.count(g)) return false;
  }
  return true;
}

bool cidSetConsistent(FtLib& lib, QPDFObjectHandle type0, const std::set<uint32_t>* usedOnly) {
  QPDFObjectHandle df = type0.getKey("/DescendantFonts");
  if (!df.isArray() || df.getArrayNItems() != 1 || !df.getArrayItem(0).isDictionary()) return true;
  QPDFObjectHandle cidFont = df.getArrayItem(0);
  QPDFObjectHandle fd = cidFont.getKey("/FontDescriptor");
  QPDFObjectHandle set = fd.isDictionary() ? fd.getKey("/CIDSet") : QPDFObjectHandle::newNull();
  if (!set.isStream()) return false;
  FtFace face;
  if (!loadFace(lib, fontFileStream(fd), face)) return true;
  std::string bits;
  try {
    auto buf = set.getStreamData(qpdf_dl_all);
    bits.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return false;
  }
  std::set<uint32_t> listed;
  for (size_t i = 0; i < bits.size() * 8; ++i) {
    if (static_cast<unsigned char>(bits[i / 8]) & (0x80 >> (i % 8))) listed.insert(static_cast<uint32_t>(i));
  }
  long n = face.face->num_glyphs;
  std::vector<uint32_t> fontCids;
  std::map<uint32_t, long> cidGid;
  bool identity = false;
  FT_Bool cidKeyed = 0;
  QPDFObjectHandle c2g = cidFont.getKey("/CIDToGIDMap");
  if (nameIs(cidFont.getKey("/Subtype"), "/CIDFontType0")) {
    if (!FT_Get_CID_Is_Internally_CID_Keyed(face.face, &cidKeyed) && cidKeyed) {
      for (long g = 0; g < n; ++g) {
        FT_UInt cid = 0;
        if (FT_Get_CID_From_Glyph_Index(face.face, static_cast<FT_UInt>(g), &cid)) continue;
        fontCids.push_back(cid);
        cidGid[cid] = g;
      }
    } else {
      identity = true;
    }
  } else {
    std::string map;
    try {
      if (c2g.isStream()) {
        auto buf = c2g.getStreamData(qpdf_dl_all);
        map.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
      }
    } catch (...) {
      return true;
    }
    identity = !c2g.isStream();
    if (identity) {
      HmtxTable hm = parseHmtx(face.data);
      long metrics = hm.ok ? static_cast<long>(hm.numH) : n;
      for (long c = 0; c < metrics; ++c) fontCids.push_back(static_cast<uint32_t>(c));
    } else {
      for (size_t c = 0; c < map.size() / 2; ++c) {
        long gid = (static_cast<unsigned char>(map[c * 2]) << 8) | static_cast<unsigned char>(map[c * 2 + 1]);
        cidGid[static_cast<uint32_t>(c)] = gid;
        if (c && gid < n) fontCids.push_back(static_cast<uint32_t>(c));
      }
    }
  }
  auto inFont = [&](uint32_t cid) {
    if (cid == 0) return false;
    if (identity) return static_cast<long>(cid) < n;
    auto it = cidGid.find(cid);
    if (it == cidGid.end()) return false;
    return nameIs(cidFont.getKey("/Subtype"), "/CIDFontType0") ? it->second != 0 : it->second < n;
  };
  if (usedOnly) {
    for (uint32_t c : *usedOnly) {
      if (inFont(c) && !listed.count(c)) return false;
    }
    return true;
  }
  for (uint32_t c : fontCids) {
    if (c != 0 && !listed.count(c)) return false;
  }
  for (uint32_t c : listed) {
    if (c != 0 && !inFont(c)) return false;
  }
  return true;
}

bool structActualText(QPDF& pdf, QPDFObjGen pageOg, long long mcid) {
  if (mcid < 0) return false;
  try {
    QPDFObjectHandle page = pdf.getObjectByObjGen(pageOg);
    QPDFObjectHandle sp = page.getKey("/StructParents");
    QPDFObjectHandle tree = pdf.getRoot().getKey("/StructTreeRoot").getKey("/ParentTree");
    if (!sp.isInteger() || !tree.isDictionary()) return false;
    QPDFObjectHandle arr;
    if (!QPDFNumberTreeObjectHelper(tree, pdf).findObject(sp.getIntValue(), arr) || !arr.isArray() ||
        mcid >= arr.getArrayNItems()) {
      return false;
    }
    QPDFObjectHandle e = arr.getArrayItem(static_cast<int>(mcid));
    for (int i = 0; i < 64 && e.isDictionary() && !nameIs(e.getKey("/Type"), "/StructTreeRoot"); ++i) {
      if (e.hasKey("/ActualText")) return true;
      e = e.getKey("/P");
    }
  } catch (...) {
  }
  return false;
}

bool puaValue(const std::string& hex) {
  if (hex.size() < 4) return false;
  unsigned v = static_cast<unsigned>(std::strtoul(hex.substr(0, 4).c_str(), nullptr, 16));
  if (v >= 0xE000 && v <= 0xF8FF) return true;
  if (v >= 0xD800 && v <= 0xDBFF && hex.size() >= 8) {
    unsigned lo = static_cast<unsigned>(std::strtoul(hex.substr(4, 4).c_str(), nullptr, 16));
    unsigned cp = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
    return cp >= 0xF0000;
  }
  return false;
}

int puaWithoutActualText(Ctx& ctx, QPDFObjectHandle font, const FontUse& use) {
  std::map<uint32_t, std::string> entries = parseExistingToUnicode(font.getKey("/ToUnicode"));
  int hits = 0;
  for (const auto& b : use.bareCodes) {
    auto it = entries.find(std::get<0>(b));
    if (it != entries.end() && puaValue(it->second) && !structActualText(ctx.pdf, std::get<1>(b), std::get<2>(b))) ++hits;
  }
  return hits;
}

int verifyUnicode(Ctx& ctx, FtLib& lib, QPDFObjectHandle font, const FontUse& use, int& forbidden) {
  std::string subtype = nameOf(font.getKey("/Subtype"));
  std::map<uint32_t, std::string> entries = parseExistingToUnicode(font.getKey("/ToUnicode"));
  bool type0 = subtype == "/Type0";
  QPDFObjectHandle enc0 = font.getKey("/Encoding");
  bool identityEnc = nameIs(enc0, "/Identity-H") || nameIs(enc0, "/Identity-V");
  bool predefined = type0 && enc0.isName() && !identityEnc;
  std::string ucsOrdering;
  std::vector<CmapSpace> ucsSpaces;
  std::map<uint32_t, int> ucsCids;
  if (type0) {
    QPDFObjectHandle df = font.getKey("/DescendantFonts");
    QPDFObjectHandle csi = df.isArray() && df.getArrayNItems() > 0 ? df.getArrayItem(0).getKey("/CIDSystemInfo")
                                                                   : QPDFObjectHandle::newNull();
    if (csi.isDictionary() && csi.getKey("/Registry").isString() && csi.getKey("/Ordering").isString() &&
        csi.getKey("/Registry").getUTF8Value() == "Adobe") {
      std::string ord = csi.getKey("/Ordering").getUTF8Value();
      if (ord == "GB1" || ord == "CNS1" || ord == "Japan1" || ord == "Korea1") {
        predefined = true;
        if (identityEnc) ucsOrdering = ord;
      }
    }
    if (predefined && !identityEnc && !loadEncodingCmap(enc0, ucsSpaces, ucsCids, &ucsOrdering)) ucsOrdering.clear();
    if (ucsOrdering != "GB1" && ucsOrdering != "CNS1" && ucsOrdering != "Japan1" && ucsOrdering != "Korea1") {
      ucsOrdering.clear();
    }
  }
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  long long flags = fd.isDictionary() && fd.getKey("/Flags").isInteger() ? fd.getKey("/Flags").getIntValue() : 0;
  bool symbolic = (flags & 4) != 0;
  SimpleEncoding enc = readEncoding(font, symbolic);
  FtFace face;
  bool haveFace = false;
  int missing = 0;
  bool type1 = subtype == "/Type1" || subtype == "/MMType1";
  auto nameUnicode = [&](const std::string& n) -> uint32_t {
    uint32_t tmp = 0;
    if (n.empty() || parseUniName(n, tmp)) return 0;
    if (uint32_t u = aglNameToUnicode(n)) return u;
    for (int sc = 0; sc < 256; ++sc) {
      const char* sn = symbolCodeToName(sc);
      if (sn && n == sn) return symbolToUnicode(sc) ? symbolToUnicode(sc) : 0x20;
      if (!type1 && zapfCodeToName(sc) == n) return 0x20;
    }
    return 0;
  };
  for (uint32_t code : use.codes) {
    auto it = entries.find(code);
    if (it != entries.end()) {
      bool bad = false;
      for (size_t q = 0; q + 4 <= it->second.size(); q += 4) {
        unsigned v = static_cast<unsigned>(std::strtoul(it->second.substr(q, 4).c_str(), nullptr, 16));
        if (v == 0 || v == 0xFEFF || v == 0xFFFE) bad = true;
      }
      if (ctx.part >= 2 && bad) ++forbidden;
      continue;
    }
    if (subtype == "/Type3" && ctx.part == 1) {
      ++missing;
      continue;
    }
    if (type0) {
      if (!predefined) {
        ++missing;
      } else if (!ucsOrdering.empty()) {
        auto cit = ucsCids.find(code);
        uint32_t cid = cit == ucsCids.end() ? 0 : static_cast<uint32_t>(cit->second);
        bool firstLookup = !identityEnc && use.hasFirstCode && use.firstCode == code;
        uint32_t count = orderingCidCount(ucsOrdering);
        if (code >= count && !(firstLookup && cid < count)) {
          ++missing;
        }
      }
      continue;
    }
    if (code > 255) {
      ++missing;
      continue;
    }
    const std::string& dn = enc.diffs[code];
    uint32_t uni = 0;
    if (!dn.empty()) {
      uni = nameUnicode(dn.substr(1));
    } else if (enc.base) {
      uni = enc.base(static_cast<int>(code));
    } else if (subtype != "/Type3") {
      if (!haveFace) {
        QPDFObjectHandle program = fontFileStream(fd);
        haveFace = program.isStream() && loadFace(lib, program, face);
      }
      if (haveFace && FT_HAS_GLYPH_NAMES(face.face) && !nameIs(font.getKey("/Subtype"), "/TrueType")) {
        FT_UInt gid = glyphForCode(face.face, static_cast<int>(code), enc, symbolic);
        for (FT_Encoding e : {FT_ENCODING_ADOBE_CUSTOM, FT_ENCODING_ADOBE_STANDARD}) {
          if (!gid && FT_Select_Charmap(face.face, e) == 0) gid = FT_Get_Char_Index(face.face, code);
        }
        char gname[64];
        if (gid && FT_Get_Glyph_Name(face.face, gid, gname, sizeof(gname)) == 0 && gname[0]) {
          uni = nameUnicode(gname);
        }
      }
    }
    if (!uni) ++missing;
  }
  return missing;
}

struct EmbedCache {
  std::map<std::string, QPDFObjectHandle> streams;
  std::map<std::string, std::shared_ptr<FtFace>> faces;
};

const FontAsset* findAsset(const std::string& key) {
  for (unsigned int i = 0; i < kFontAssetCount; ++i) {
    if (key == kFontAssets[i].key) return &kFontAssets[i];
  }
  return nullptr;
}

struct UserFont {
  std::string wanted;
  std::string psName;
  std::string key;
  std::string bytes;
  FontAsset asset{nullptr, nullptr, nullptr, 0};
};

struct SubstituteChoice {
  const FontAsset* asset = nullptr;
  bool serif = false;
  bool mono = false;
  bool bold = false;
  bool italic = false;
  uint16_t (*symbolTable)(int) = nullptr;
};

bool nameHas(const std::string& lower, const char* needle) {
  return lower.find(needle) != std::string::npos;
}

std::vector<std::unique_ptr<UserFont>>& userFonts() {
  static thread_local std::vector<std::unique_ptr<UserFont>> fonts;
  return fonts;
}

bool isTrueTypeSfnt(const std::string& b) {
  if (b.size() < 4) return false;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(b.data());
  return (p[0] == 0x00 && p[1] == 0x01 && p[2] == 0x00 && p[3] == 0x00) ||
         std::memcmp(p, "true", 4) == 0 || std::memcmp(p, "ttcf", 4) == 0;
}

const FontAsset* hostFont(Ctx& ctx, const std::string& base) {
  if (!ctx.opt.loadFont) return nullptr;
  std::string bare = base.empty() || base[0] != '/' ? base : base.substr(1);
  std::string wanted = bare;
  for (const auto& sub : ctx.opt.fontSubstitutions) {
    if (sub.first == bare) {
      wanted = sub.second;
      break;
    }
  }
  for (const auto& f : userFonts()) {
    if (f->wanted == wanted) return f->asset.data ? &f->asset : nullptr;
  }
  auto uf = std::make_unique<UserFont>();
  uf->wanted = wanted;
  if (!ctx.opt.loadFont(wanted, uf->psName, uf->bytes) || uf->bytes.size() < 64 ||
      !isTrueTypeSfnt(uf->bytes)) {
    uf->bytes.clear();
    userFonts().push_back(std::move(uf));
    return nullptr;
  }
  if (uf->psName.empty()) uf->psName = wanted;
  uf->key = "user:" + wanted;
  uf->asset.key = uf->key.c_str();
  uf->asset.psName = uf->psName.c_str();
  uf->asset.data = reinterpret_cast<const unsigned char*>(uf->bytes.data());
  uf->asset.len = static_cast<unsigned int>(uf->bytes.size());
  const FontAsset* out = &uf->asset;
  userFonts().push_back(std::move(uf));
  return out;
}

SubstituteChoice chooseSubstitute(QPDFObjectHandle font, QPDFObjectHandle fd) {
  SubstituteChoice c;
  std::string base = nameOf(font.getKey("/BaseFont"));
  if (isSubsetName(base)) base = "/" + base.substr(8);
  std::string lower = base;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char ch) { return std::tolower(ch); });
  long long flags = fd.isDictionary() && fd.getKey("/Flags").isInteger()
                        ? fd.getKey("/Flags").getIntValue()
                        : 0;
  double italicAngle = fd.isDictionary() ? numOf(fd.getKey("/ItalicAngle"), 0) : 0;
  double stemV = fd.isDictionary() ? numOf(fd.getKey("/StemV"), 0) : 0;

  c.bold = nameHas(lower, "bold") || nameHas(lower, "black") || nameHas(lower, "heavy") ||
           (flags & (1LL << 18)) != 0 || stemV >= 139;
  c.italic = nameHas(lower, "italic") || nameHas(lower, "oblique") || italicAngle != 0 ||
             (flags & 64) != 0;

  if (nameHas(lower, "zapf") || nameHas(lower, "dingbat")) {
    c.symbolTable = zapfDingbatsToUnicode;
  } else if (nameHas(lower, "symbol")) {
    c.symbolTable = symbolToUnicode;
  }
  if (c.symbolTable) {
    c.asset = findAsset("symbol");
    c.bold = false;
    c.italic = false;
    return c;
  }

  if (nameHas(lower, "courier") || nameHas(lower, "mono") || nameHas(lower, "consol") ||
      (flags & 1) != 0) {
    c.mono = true;
  } else if (nameHas(lower, "times") || nameHas(lower, "serif") || nameHas(lower, "roman") ||
             nameHas(lower, "georgia") || nameHas(lower, "garamond") ||
             nameHas(lower, "palatino") || nameHas(lower, "book") || nameHas(lower, "cambria") ||
             nameHas(lower, "constantia") || nameHas(lower, "minion") ||
             nameHas(lower, "century")) {
    c.serif = true;
  } else if ((flags & 2) != 0 && !nameHas(lower, "arial") && !nameHas(lower, "helvetica") &&
             !nameHas(lower, "verdana") && !nameHas(lower, "tahoma") &&
             !nameHas(lower, "calibri") && !nameHas(lower, "segoe")) {
    c.serif = true;
  }

  std::string key = c.mono ? "mono" : (c.serif ? "serif" : "sans");
  if (c.bold && c.italic) key += "_bi";
  else if (c.bold) key += "_b";
  else if (c.italic) key += "_i";
  c.asset = findAsset(key);
  return c;
}

QPDFObjectHandle assetStream(Ctx& ctx, EmbedCache& cache, const FontAsset* asset, bool ttf) {
  auto it = cache.streams.find(asset->key);
  if (it != cache.streams.end()) return it->second;
  std::string data(reinterpret_cast<const char*>(asset->data), asset->len);
  QPDFObjectHandle stream = QPDFObjectHandle::newStream(&ctx.pdf, data);
  if (ttf) {
    stream.getDict().replaceKey("/Length1",
                                QPDFObjectHandle::newInteger(static_cast<long long>(asset->len)));
  } else {
    stream.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/Type1C"));
  }
  QPDFObjectHandle ref = ctx.pdf.makeIndirectObject(stream);
  cache.streams[asset->key] = ref;
  return ref;
}

std::shared_ptr<FtFace> assetFace(FtLib& lib, EmbedCache& cache, const FontAsset* asset) {
  auto it = cache.faces.find(asset->key);
  if (it != cache.faces.end()) return it->second;
  auto face = std::make_shared<FtFace>();
  face->data.assign(reinterpret_cast<const char*>(asset->data), asset->len);
  FT_New_Memory_Face(lib.lib, reinterpret_cast<const FT_Byte*>(face->data.data()),
                     static_cast<FT_Long>(face->data.size()), 0, &face->face);
  cache.faces[asset->key] = face;
  return face;
}

QPDFObjectHandle buildDescriptor(Ctx&, const SubstituteChoice& c, FT_Face face,
                                 const std::string& psName) {
  double upem = face->units_per_EM ? face->units_per_EM : 1000.0;
  double scale = 1000.0 / upem;
  QPDFObjectHandle fd = QPDFObjectHandle::newDictionary();
  fd.replaceKey("/Type", QPDFObjectHandle::newName("/FontDescriptor"));
  fd.replaceKey("/FontName", QPDFObjectHandle::newName("/" + psName));
  long long flags = 32;
  if (c.mono) flags |= 1;
  if (c.serif) flags |= 2;
  if (c.italic) flags |= 64;
  fd.replaceKey("/Flags", QPDFObjectHandle::newInteger(flags));
  QPDFObjectHandle bbox = QPDFObjectHandle::newArray();
  bbox.appendItem(QPDFObjectHandle::newInteger(llround(face->bbox.xMin * scale)));
  bbox.appendItem(QPDFObjectHandle::newInteger(llround(face->bbox.yMin * scale)));
  bbox.appendItem(QPDFObjectHandle::newInteger(llround(face->bbox.xMax * scale)));
  bbox.appendItem(QPDFObjectHandle::newInteger(llround(face->bbox.yMax * scale)));
  fd.replaceKey("/FontBBox", bbox);
  fd.replaceKey("/ItalicAngle", QPDFObjectHandle::newInteger(c.italic ? -12 : 0));
  fd.replaceKey("/Ascent", QPDFObjectHandle::newInteger(llround(face->ascender * scale)));
  fd.replaceKey("/Descent", QPDFObjectHandle::newInteger(llround(face->descender * scale)));
  double capHeight = face->ascender * scale * 0.72;
  TT_OS2* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
  if (os2 && os2->version >= 2 && os2->sCapHeight) capHeight = os2->sCapHeight * scale;
  fd.replaceKey("/CapHeight", QPDFObjectHandle::newInteger(llround(capHeight)));
  fd.replaceKey("/StemV", QPDFObjectHandle::newInteger(c.bold ? 150 : 80));
  return fd;
}

void buildSymbolDifferences(Ctx&, QPDFObjectHandle font, uint16_t (*table)(int)) {
  bool zapf = table == zapfDingbatsToUnicode;
  QPDFObjectHandle diffs = QPDFObjectHandle::newArray();
  int last = -2;
  for (int code = 0; code < 256; ++code) {
    if (!table(code)) continue;
    std::string name;
    if (zapf) {
      name = zapfCodeToName(code);
    } else {
      const char* n = symbolCodeToName(code);
      if (n) name = n;
    }
    if (name.empty()) continue;
    if (code != last + 1) diffs.appendItem(QPDFObjectHandle::newInteger(code));
    diffs.appendItem(QPDFObjectHandle::newName("/" + name));
    last = code;
  }
  QPDFObjectHandle enc = QPDFObjectHandle::newDictionary();
  enc.replaceKey("/Type", QPDFObjectHandle::newName("/Encoding"));
  enc.replaceKey("/BaseEncoding", QPDFObjectHandle::newName("/WinAnsiEncoding"));
  enc.replaceKey("/Differences", diffs);
  font.replaceKey("/Encoding", enc);
}

namespace {
std::string subsetTrueType(const std::string& font, std::set<uint32_t> keep,
                           const std::map<uint32_t, uint16_t>* advances, bool keepCmap);
}

void rebuildSimpleWidths(Ctx&, QPDFObjectHandle font, const FtFace& sub) {
  FT_Face face = sub.face;
  HmtxTable hmtx = parseHmtx(sub.data);
  double upem = face->units_per_EM ? face->units_per_EM : 1000.0;
  int firstChar = 0, lastChar = 255;
  if (font.getKey("/FirstChar").isInteger() && font.getKey("/LastChar").isInteger()) {
    firstChar = static_cast<int>(font.getKey("/FirstChar").getIntValue());
    lastChar = static_cast<int>(font.getKey("/LastChar").getIntValue());
    if (firstChar < 0 || lastChar > 255 || firstChar > lastChar) {
      firstChar = 0;
      lastChar = 255;
    }
  }
  SimpleEncoding enc = readEncoding(font, false);
  QPDFObjectHandle widths = QPDFObjectHandle::newArray();
  for (int code = firstChar; code <= lastChar; ++code) {
    FT_UInt gid = glyphForCode(face, code, enc, false);
    double adv = programAdvance(sub, hmtx, gid, upem);
    widths.appendItem(QPDFObjectHandle::newInteger(adv < 0 ? 0 : llround(adv)));
  }
  font.replaceKey("/FirstChar", QPDFObjectHandle::newInteger(firstChar));
  font.replaceKey("/LastChar", QPDFObjectHandle::newInteger(lastChar));
  font.replaceKey("/Widths", widths);
}

bool embedSimpleSubstitute(Ctx& ctx, FtLib& lib, EmbedCache& cache, QPDFObjectHandle font) {
  QPDFObjectHandle oldFd = font.getKey("/FontDescriptor");
  SubstituteChoice c = chooseSubstitute(font, oldFd);
  if (!c.asset) return false;
  const FontAsset* host = hostFont(ctx, nameOf(font.getKey("/BaseFont")));
  if (host) c.asset = host;
  auto face = assetFace(lib, cache, c.asset);
  if (!face->face) return false;
  std::string original = nameOf(font.getKey("/BaseFont"));

  QPDFObjectHandle oldWidths = font.getKey("/Widths");
  QPDFObjectHandle encIn = font.getKey("/Encoding");
  bool plainEncoding = nameIs(encIn, "/WinAnsiEncoding") || nameIs(encIn, "/MacRomanEncoding");
  bool keepWidths = !c.symbolTable && plainEncoding && oldWidths.isArray() && font.getKey("/FirstChar").isInteger();
  if (keepWidths) {
    bool any = false;
    for (int i = 0; i < oldWidths.getArrayNItems(); ++i)
      if (oldWidths.getArrayItem(i).isNumber() && oldWidths.getArrayItem(i).getNumericValue() > 0) any = true;
    keepWidths = any;
  }
  std::string fitted;
  const FontAsset* ttfAsset = host ? host : findAsset(std::string(c.asset->key) + ":ttf");
  if (keepWidths && ttfAsset) {
    QPDFObjectHandle enc0 = font.getKey("/Encoding");
    if (!enc0.isName() && !enc0.isDictionary()) font.replaceKey("/Encoding", QPDFObjectHandle::newName("/WinAnsiEncoding"));
    auto tf = assetFace(lib, cache, ttfAsset);
    if (tf->face) {
      SimpleEncoding enc = readEncoding(font, false);
      HmtxTable hm = parseHmtx(tf->data);
      double upem = tf->face->units_per_EM ? tf->face->units_per_EM : 1000.0;
      int first = static_cast<int>(font.getKey("/FirstChar").getIntValue());
      std::set<uint32_t> keep;
      std::map<uint32_t, uint16_t> adv;
      bool differs = false;
      for (int i = 0; i < oldWidths.getArrayNItems() && first + i < 256; ++i) {
        if (first + i < 0 || !oldWidths.getArrayItem(i).isNumber()) continue;
        double w = oldWidths.getArrayItem(i).getNumericValue();
        FT_UInt gid = glyphForCode(tf->face, first + i, enc, false);
        if (!gid || w <= 0 || adv.count(gid)) continue;
        keep.insert(gid);
        double own = programAdvance(*tf, hm, gid, upem);
        if (std::fabs(own - w) > 1.0) differs = true;
        adv[gid] = static_cast<uint16_t>(std::clamp<long long>(llround(w * upem / 1000.0), 0, 65535));
      }
      if (differs && !keep.empty()) fitted = subsetTrueType(tf->data, keep, &adv, true);
    }
  }
  QPDFObjectHandle fd = buildDescriptor(ctx, c, face->face, c.asset->psName);
  if (!fitted.empty()) {
    QPDFObjectHandle prog = QPDFObjectHandle::newStream(&ctx.pdf, fitted);
    prog.getDict().replaceKey("/Length1", QPDFObjectHandle::newInteger(static_cast<long long>(fitted.size())));
    fd.replaceKey("/FontFile2", ctx.pdf.makeIndirectObject(prog));
  } else {
    fd.replaceKey(host ? "/FontFile2" : "/FontFile3", assetStream(ctx, cache, c.asset, host != nullptr));
  }
  font.replaceKey("/FontDescriptor", ctx.pdf.makeIndirectObject(fd));
  font.replaceKey("/Subtype", QPDFObjectHandle::newName(host || !fitted.empty() ? "/TrueType" : "/Type1"));
  font.replaceKey("/BaseFont", QPDFObjectHandle::newName("/" + std::string(c.asset->psName)));

  if (c.symbolTable) {
    buildSymbolDifferences(ctx, font, c.symbolTable);
  } else {
    QPDFObjectHandle enc = font.getKey("/Encoding");
    if (enc.isDictionary()) {
      if (!enc.getKey("/BaseEncoding").isName()) {
        enc.replaceKey("/BaseEncoding", QPDFObjectHandle::newName("/WinAnsiEncoding"));
      }
    } else if (!enc.isName()) {
      font.replaceKey("/Encoding", QPDFObjectHandle::newName("/WinAnsiEncoding"));
    }
  }
  if (fitted.empty() && !host && !c.symbolTable && FT_HAS_GLYPH_NAMES(face->face)) {
    SimpleEncoding enc = readEncoding(font, false);
    QPDFObjectHandle diffs = QPDFObjectHandle::newArray();
    int last = -2;
    for (int code = 0; code < 256; ++code) {
      FT_UInt gid = glyphForCode(face->face, code, enc, false);
      char name[128];
      if (!gid || FT_Get_Glyph_Name(face->face, gid, name, sizeof(name)) || !*name) continue;
      std::string own(name);
      if (!enc.diffs[code].empty()) {
        if (FT_Get_Name_Index(face->face, enc.diffs[code].substr(1).c_str()) == gid) continue;
      } else if (enc.base && enc.base(code) && aglNameToUnicode(own) == enc.base(code) && own.rfind("uni", 0) != 0) {
        continue;
      }
      if (code != last + 1) diffs.appendItem(QPDFObjectHandle::newInteger(code));
      diffs.appendItem(QPDFObjectHandle::newName("/" + own));
      last = code;
    }
    if (diffs.getArrayNItems()) {
      QPDFObjectHandle e = font.getKey("/Encoding");
      QPDFObjectHandle d = e.isDictionary() ? e : QPDFObjectHandle::newDictionary();
      if (e.isName()) d.replaceKey("/BaseEncoding", e);
      d.replaceKey("/Type", QPDFObjectHandle::newName("/Encoding"));
      d.replaceKey("/Differences", diffs);
      font.replaceKey("/Encoding", d);
    }
  }
  if (fitted.empty()) rebuildSimpleWidths(ctx, font, *face);
  font.removeKey("/ToUnicode");
  ctx.issue("FONT_SUBSTITUTED",
            "embedded " + std::string(c.asset->psName) + " as substitute for " + original +
                " (visual fidelity depends on metric compatibility)",
            true);
  return true;
}

uint32_t hexToCodepoint(const std::string& hex) {
  if (hex.size() < 4) return 0;
  uint32_t first = static_cast<uint32_t>(std::strtoul(hex.substr(0, 4).c_str(), nullptr, 16));
  if (first >= 0xD800 && first <= 0xDBFF && hex.size() >= 8) {
    uint32_t second = static_cast<uint32_t>(std::strtoul(hex.substr(4, 4).c_str(), nullptr, 16));
    if (second >= 0xDC00 && second <= 0xDFFF) {
      return 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
    }
  }
  return first;
}

bool embedCidSubstitute(Ctx& ctx, FtLib& lib, EmbedCache& cache, QPDFObjectHandle type0) {
  std::string encName = nameOf(type0.getKey("/Encoding"));
  if (encName != "/Identity-H" && encName != "/Identity-V") return false;
  QPDFObjectHandle df = type0.getKey("/DescendantFonts");
  if (!df.isArray() || df.getArrayNItems() != 1 || !df.getArrayItem(0).isDictionary()) {
    return false;
  }
  QPDFObjectHandle cidFont = df.getArrayItem(0);
  QPDFObjectHandle oldFd = cidFont.getKey("/FontDescriptor");
  SubstituteChoice c = chooseSubstitute(type0, oldFd);
  if (!c.asset) return false;
  const FontAsset* host = hostFont(ctx, nameOf(type0.getKey("/BaseFont")));
  c.asset = host ? host : findAsset(std::string(c.asset->key) + ":ttf");
  if (!c.asset) return false;
  auto face = assetFace(lib, cache, c.asset);
  if (!face->face) return false;
  std::string original = nameOf(type0.getKey("/BaseFont"));

  std::map<uint32_t, std::string> toUni = parseExistingToUnicode(type0.getKey("/ToUnicode"));
  if (toUni.empty() && nameIs(cidFont.getKey("/Subtype"), "/CIDFontType2")) return false;
  uint32_t maxCid = 255;
  for (const auto& kv : toUni) maxCid = std::max(maxCid, kv.first);
  QPDFObjectHandle oldW = cidFont.getKey("/W");
  if (oldW.isArray()) {
    for (int i = 0; i < oldW.getArrayNItems(); ++i) {
      QPDFObjectHandle item = oldW.getArrayItem(i);
      if (item.isInteger()) {
        maxCid = std::max(maxCid, static_cast<uint32_t>(
                                      std::min<long long>(65535, item.getIntValue())));
      }
    }
  }
  if (maxCid > 65535) maxCid = 65535;

  bool haveUnicodeCmap = FT_Select_Charmap(face->face, FT_ENCODING_UNICODE) == 0;
  std::string mapData((static_cast<size_t>(maxCid) + 1) * 2, '\0');
  for (uint32_t cid = 0; cid <= maxCid; ++cid) {
    uint32_t uni = 0;
    auto it = toUni.find(cid);
    if (it != toUni.end()) {
      uni = hexToCodepoint(it->second);
      size_t unitLen = uni > 0xFFFF ? 8 : 4;
      if (it->second.size() > unitLen) {
        static const std::map<std::string, uint32_t> kLatinLigatures = {
            {"00660066", 0xFB00}, {"00660069", 0xFB01}, {"0066006C", 0xFB02},
            {"006600660069", 0xFB03}, {"00660066006C", 0xFB04}};
        std::string up = it->second;
        std::transform(up.begin(), up.end(), up.begin(), [](unsigned char ch) { return std::toupper(ch); });
        auto lig = kLatinLigatures.find(up);
        if (lig == kLatinLigatures.end()) return false;
        uni = lig->second;
      }
    } else if (toUni.empty() && cid >= 0x20 && !(cid >= 0xD800 && cid <= 0xDFFF)) {
      uni = cid;
    }
    FT_UInt gid = (uni && haveUnicodeCmap) ? FT_Get_Char_Index(face->face, uni) : 0;
    if (!gid && it != toUni.end() && uni > 0x20 && !(uni >= 0x7F && uni <= 0xA0) && uni != 0xAD &&
        !(uni >= 0x200B && uni <= 0x200F) && uni != 0xFEFF)
      return false;
    mapData[cid * 2] = static_cast<char>((gid >> 8) & 0xFF);
    mapData[cid * 2 + 1] = static_cast<char>(gid & 0xFF);
  }

  QPDFObjectHandle fd = buildDescriptor(ctx, c, face->face, c.asset->psName);
  fd.replaceKey("/FontFile2", assetStream(ctx, cache, c.asset, true));
  cidFont.replaceKey("/FontDescriptor", ctx.pdf.makeIndirectObject(fd));
  cidFont.replaceKey("/Subtype", QPDFObjectHandle::newName("/CIDFontType2"));
  cidFont.replaceKey("/BaseFont",
                     QPDFObjectHandle::newName("/" + std::string(c.asset->psName)));
  type0.replaceKey("/BaseFont", QPDFObjectHandle::newName("/" + std::string(c.asset->psName)));
  QPDFObjectHandle sysInfo = QPDFObjectHandle::newDictionary();
  sysInfo.replaceKey("/Registry", QPDFObjectHandle::newString("Adobe"));
  sysInfo.replaceKey("/Ordering", QPDFObjectHandle::newString("Identity"));
  sysInfo.replaceKey("/Supplement", QPDFObjectHandle::newInteger(0));
  cidFont.replaceKey("/CIDSystemInfo", sysInfo);
  QPDFObjectHandle mapStream = QPDFObjectHandle::newStream(&ctx.pdf, mapData);
  cidFont.replaceKey("/CIDToGIDMap", ctx.pdf.makeIndirectObject(mapStream));
  cidFont.removeKey("/W");
  cidFont.removeKey("/DW");
  ctx.issue("FONT_SUBSTITUTED",
            "embedded " + std::string(c.asset->psName) + " as substitute for " + original +
                " (visual fidelity depends on metric compatibility)",
            true);
  return true;
}

namespace {
uint32_t be32(const std::string& s, size_t o) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(s[o])) << 24) |
         (static_cast<uint32_t>(static_cast<unsigned char>(s[o + 1])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(s[o + 2])) << 8) |
         static_cast<unsigned char>(s[o + 3]);
}
uint16_t be16(const std::string& s, size_t o) {
  return static_cast<uint16_t>((static_cast<unsigned char>(s[o]) << 8) | static_cast<unsigned char>(s[o + 1]));
}
void put32(std::string& s, size_t o, uint32_t v) {
  for (int i = 0; i < 4; ++i) s[o + i] = static_cast<char>((v >> (24 - 8 * i)) & 0xFF);
}
void put16(std::string& s, size_t o, uint16_t v) {
  s[o] = static_cast<char>(v >> 8);
  s[o + 1] = static_cast<char>(v & 0xFF);
}
uint32_t tableSum(const std::string& t) {
  uint32_t sum = 0;
  std::string p = t;
  while (p.size() % 4) p += '\0';
  for (size_t i = 0; i < p.size(); i += 4) sum += be32(p, i);
  return sum;
}

std::string subsetTrueType(const std::string& font, std::set<uint32_t> keep,
                           const std::map<uint32_t, uint16_t>* advances, bool keepCmap) {
  if (font.size() < 12) return std::string();
  uint16_t numTables = be16(font, 4);
  std::map<std::string, std::string> tables;
  for (uint16_t i = 0; i < numTables && 12 + 16u * (i + 1) <= font.size(); ++i) {
    size_t rec = 12 + 16u * i;
    uint32_t off = be32(font, rec + 8), len = be32(font, rec + 12);
    if (off > font.size() || len > font.size() - off) return std::string();
    tables[font.substr(rec, 4)] = font.substr(off, len);
  }
  for (const char* need : {"head", "hhea", "maxp", "hmtx", "loca", "glyf"}) {
    if (!tables.count(need)) return std::string();
  }
  std::string& head = tables["head"];
  if (head.size() < 54 || tables["maxp"].size() < 6 || tables["hhea"].size() < 36) return std::string();
  bool longLoca = be16(head, 50) == 1;
  uint32_t numGlyphs = be16(tables["maxp"], 4);
  const std::string& loca = tables["loca"];
  const std::string& glyf = tables["glyf"];
  auto glyphRange = [&](uint32_t g, uint32_t& a, uint32_t& b) {
    if (g >= numGlyphs) return false;
    if (longLoca) {
      if ((g + 2) * 4 > loca.size()) return false;
      a = be32(loca, g * 4);
      b = be32(loca, (g + 1) * 4);
    } else {
      if ((g + 2) * 2 > loca.size()) return false;
      a = be16(loca, g * 2) * 2u;
      b = be16(loca, (g + 1) * 2) * 2u;
    }
    return a <= b && b <= glyf.size();
  };
  keep.insert(0);
  std::vector<uint32_t> work(keep.begin(), keep.end());
  while (!work.empty()) {
    uint32_t g = work.back();
    work.pop_back();
    uint32_t a, b;
    if (!glyphRange(g, a, b) || b - a < 10) continue;
    if (static_cast<int16_t>(be16(glyf, a)) >= 0) continue;
    size_t p = a + 10;
    for (;;) {
      if (p + 4 > b) break;
      uint16_t flags = be16(glyf, p);
      uint16_t comp = be16(glyf, p + 2);
      if (keep.insert(comp).second) work.push_back(comp);
      p += 4 + ((flags & 1) ? 4 : 2);
      if (flags & 8) p += 2;
      else if (flags & 0x40) p += 4;
      else if (flags & 0x80) p += 8;
      if (!(flags & 0x20)) break;
    }
  }
  std::string newGlyf, newLoca;
  for (uint32_t g = 0; g < numGlyphs; ++g) {
    std::string off(4, '\0');
    put32(off, 0, static_cast<uint32_t>(newGlyf.size()));
    newLoca += off;
    uint32_t a, b;
    if (keep.count(g) && glyphRange(g, a, b)) {
      newGlyf += glyf.substr(a, b - a);
      while (newGlyf.size() % 4) newGlyf += '\0';
    }
  }
  std::string end(4, '\0');
  put32(end, 0, static_cast<uint32_t>(newGlyf.size()));
  newLoca += end;
  tables["glyf"] = newGlyf;
  tables["loca"] = newLoca;
  put16(head, 50, 1);
  put32(head, 8, 0);
  std::string& hmtx = tables["hmtx"];
  uint16_t numMetrics = be16(tables["hhea"], 34);
  if (advances && !advances->empty() && numMetrics > 0 && numGlyphs <= 0xFFFF) {
    std::string full(static_cast<size_t>(numGlyphs) * 4, '\0');
    for (uint32_t g = 0; g < numGlyphs; ++g) {
      uint32_t src = g < numMetrics ? g : numMetrics - 1u;
      uint16_t adv = src * 4 + 2 <= hmtx.size() ? be16(hmtx, src * 4) : 0;
      size_t lsbAt = g < numMetrics ? g * 4u + 2 : numMetrics * 4u + (g - numMetrics) * 2u;
      int16_t lsb = lsbAt + 2 <= hmtx.size() ? static_cast<int16_t>(be16(hmtx, lsbAt)) : 0;
      auto it = advances->find(g);
      if (it != advances->end()) adv = it->second;
      put16(full, g * 4, adv);
      put16(full, g * 4 + 2, static_cast<uint16_t>(lsb));
    }
    hmtx = full;
    numMetrics = static_cast<uint16_t>(numGlyphs);
    put16(tables["hhea"], 34, numMetrics);
    uint16_t maxAdv = 0;
    for (uint32_t g = 0; g < numGlyphs; ++g) maxAdv = std::max<uint16_t>(maxAdv, be16(hmtx, g * 4));
    put16(tables["hhea"], 10, maxAdv);
  }
  for (uint32_t g = 0; g < numGlyphs; ++g) {
    if (keep.count(g)) continue;
    if (g < numMetrics) {
      if (g * 4 + 4 <= hmtx.size()) put32(hmtx, g * 4, 0);
    } else {
      size_t o = numMetrics * 4u + (g - numMetrics) * 2u;
      if (o + 2 <= hmtx.size()) put16(hmtx, o, 0);
    }
  }
  if (tables.count("post") && tables["post"].size() >= 32) {
    tables["post"] = tables["post"].substr(0, 32);
    put32(tables["post"], 0, 0x00030000);
  }
  std::vector<std::string> order;
  for (const char* t : {"OS/2", "cmap", "cvt ", "fpgm", "glyf", "head", "hhea", "hmtx", "loca", "maxp", "name", "post", "prep"}) {
    if (tables.count(t) && (keepCmap || std::strcmp(t, "cmap") != 0)) order.push_back(t);
  }
  uint16_t n = static_cast<uint16_t>(order.size());
  uint16_t pow2 = 1, log2 = 0;
  while (pow2 * 2 <= n) {
    pow2 *= 2;
    ++log2;
  }
  std::string out(12 + 16u * n, '\0');
  put32(out, 0, 0x00010000);
  put16(out, 4, n);
  put16(out, 6, static_cast<uint16_t>(pow2 * 16));
  put16(out, 8, log2);
  put16(out, 10, static_cast<uint16_t>(n * 16 - pow2 * 16));
  size_t headOffset = 0;
  for (uint16_t i = 0; i < n; ++i) {
    const std::string& t = tables[order[i]];
    size_t rec = 12 + 16u * i;
    out.replace(rec, 4, order[i]);
    put32(out, rec + 4, tableSum(t));
    put32(out, rec + 8, static_cast<uint32_t>(out.size()));
    put32(out, rec + 12, static_cast<uint32_t>(t.size()));
    if (order[i] == "head") headOffset = out.size();
    out += t;
    while (out.size() % 4) out += '\0';
  }
  put32(out, headOffset + 8, 0xB1B0AFBAu - tableSum(out));
  return out;
}

std::string cjkScriptOf(QPDFObjectHandle type0, bool& vertical, bool& unicodeCodes) {
  std::string enc = nameOf(type0.getKey("/Encoding"));
  vertical = enc.size() > 2 && enc.compare(enc.size() - 2, 2, "-V") == 0;
  unicodeCodes = false;
  static const std::pair<const char*, const char*> kPrefixes[] = {
      {"/UniGB-", "SC"}, {"/UniCNS-", "TC"}, {"/UniJIS-", "JP"}, {"/UniKS-", "KR"}};
  for (const auto& p : kPrefixes) {
    if (enc.rfind(p.first, 0) == 0 && (enc.find("UCS2") != std::string::npos || enc.find("UTF16") != std::string::npos)) {
      unicodeCodes = true;
      return p.second;
    }
  }
  if (enc != "/Identity-H" && enc != "/Identity-V") return std::string();
  QPDFObjectHandle df = type0.getKey("/DescendantFonts");
  if (!df.isArray() || df.getArrayNItems() != 1 || !df.getArrayItem(0).isDictionary()) return std::string();
  QPDFObjectHandle info = df.getArrayItem(0).getKey("/CIDSystemInfo");
  std::string ordering = info.isDictionary() && info.getKey("/Ordering").isString() ? info.getKey("/Ordering").getStringValue() : "";
  if (ordering == "GB1") return "SC";
  if (ordering == "CNS1") return "TC";
  if (ordering == "Japan1") return "JP";
  if (ordering == "Korea1") return "KR";
  return std::string();
}

std::string subsetTag(const std::set<uint32_t>& gids, const std::string& salt) {
  uint32_t h = 2166136261u;
  for (char c : salt) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
  for (uint32_t g : gids) h = (h ^ g) * 16777619u;
  std::string tag;
  for (int i = 0; i < 6; ++i) {
    tag += static_cast<char>('A' + h % 26);
    h /= 26;
    if (!h) h = 2654435761u;
  }
  return tag;
}
}

bool embedCjkSubstitute(Ctx& ctx, FtLib& lib, EmbedCache& cache, QPDFObjectHandle type0, const FontUse* use) {
  if (!ctx.opt.loadCjkFont) return false;
  bool vertical = false, unicodeCodes = false;
  std::string script = cjkScriptOf(type0, vertical, unicodeCodes);
  if (script.empty()) return false;
  QPDFObjectHandle df = type0.getKey("/DescendantFonts");
  if (!df.isArray() || df.getArrayNItems() != 1 || !df.getArrayItem(0).isDictionary()) return false;
  QPDFObjectHandle cidFont = df.getArrayItem(0);
  SubstituteChoice c = chooseSubstitute(type0, cidFont.getKey("/FontDescriptor"));
  std::string key = "cjk:" + script + (c.bold ? ":b" : ":r");
  auto fit = cache.faces.find(key);
  if (fit == cache.faces.end()) {
    auto face = std::make_shared<FtFace>();
    if (!ctx.opt.loadCjkFont(script, c.bold, face->data) &&
        !(c.bold && ctx.opt.loadCjkFont(script, false, face->data))) {
      face->data.clear();
    }
    if (!face->data.empty()) {
      FT_New_Memory_Face(lib.lib, reinterpret_cast<const FT_Byte*>(face->data.data()),
                         static_cast<FT_Long>(face->data.size()), 0, &face->face);
    }
    fit = cache.faces.emplace(key, face).first;
  }
  FT_Face face = fit->second->face;
  if (!face || FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) return false;
  std::map<uint32_t, std::string> toUni = parseExistingToUnicode(type0.getKey("/ToUnicode"));
  bool orderingUni = !unicodeCodes && !type0.getKey("/ToUnicode").isStream();
  QPDFObjectHandle cjkInfo = cidFont.getKey("/CIDSystemInfo");
  std::string cjkOrdering =
      cjkInfo.isDictionary() && cjkInfo.getKey("/Ordering").isString() ? cjkInfo.getKey("/Ordering").getUTF8Value() : "";
  std::set<uint32_t> codes;
  if (use) codes = use->codes;
  else if (orderingUni) for (uint32_t cid = 0; cid < orderingCidCount(cjkOrdering); ++cid) codes.insert(cid);
  else for (const auto& kv : toUni) codes.insert(kv.first);
  std::map<uint32_t, uint32_t> codeUni;
  for (uint32_t code : codes) {
    if (code > 0xFFFF) return false;
    uint32_t uni = 0;
    if (unicodeCodes) {
      if (code >= 0xD800 && code <= 0xDFFF) return false;
      uni = code;
    } else {
      auto it = toUni.find(code);
      if (it != toUni.end()) uni = hexToCodepoint(it->second);
      else if (orderingUni) uni = orderingCidToUnicode(cjkOrdering, code);
    }
    codeUni[code] = uni;
  }
  std::set<uint32_t> keep;
  std::map<uint32_t, uint32_t> codeGid;
  for (const auto& kv : codeUni) {
    FT_UInt gid = kv.second ? FT_Get_Char_Index(face, kv.second) : 0;
    codeGid[kv.first] = gid;
    if (gid) keep.insert(gid);
  }
  std::map<uint32_t, double> origW;
  double origDW = cidFont.getKey("/DW").isNumber() ? cidFont.getKey("/DW").getNumericValue() : 1000.0;
  {
    QPDFObjectHandle wArr = cidFont.getKey("/W");
    for (int i = 0; wArr.isArray() && i + 1 < wArr.getArrayNItems();) {
      QPDFObjectHandle a = wArr.getArrayItem(i), b = wArr.getArrayItem(i + 1);
      if (!a.isInteger()) break;
      long long c0 = a.getIntValue();
      if (b.isArray()) {
        for (int k = 0; k < b.getArrayNItems(); ++k)
          if (b.getArrayItem(k).isNumber()) origW[static_cast<uint32_t>(c0 + k)] = b.getArrayItem(k).getNumericValue();
        i += 2;
      } else if (b.isInteger() && i + 2 < wArr.getArrayNItems() && wArr.getArrayItem(i + 2).isNumber()) {
        double v = wArr.getArrayItem(i + 2).getNumericValue();
        for (long long cc = c0; cc <= b.getIntValue() && cc - c0 < 65536; ++cc) origW[static_cast<uint32_t>(cc)] = v;
        i += 3;
      } else {
        break;
      }
    }
  }
  auto originalWidth = [&](uint32_t code) {
    uint32_t cid = code;
    if (unicodeCodes) {
      if (code < 0x20 || code > 0x7E) return origDW;
      cid = code - 0x1F;
    }
    auto it = origW.find(cid);
    return it != origW.end() ? it->second : origDW;
  };
  double upemK = face->units_per_EM ? face->units_per_EM / 1000.0 : 1.0;
  std::map<uint32_t, uint16_t> advances;
  std::map<uint32_t, double> codeWidth;
  for (const auto& kv : codeGid) {
    double ow = originalWidth(kv.first);
    codeWidth[kv.first] = ow;
    if (kv.second && !advances.count(kv.second) && ow >= 0 && ow * upemK < 65535)
      advances[kv.second] = static_cast<uint16_t>(llround(ow * upemK));
  }
  std::string program = subsetTrueType(fit->second->data, keep, &advances, false);
  if (program.empty()) return false;
  uint32_t maxCode = codeGid.empty() ? 0 : codeGid.rbegin()->first;
  std::string mapData((static_cast<size_t>(maxCode) + 1) * 2, '\0');
  for (const auto& kv : codeGid) {
    mapData[kv.first * 2] = static_cast<char>((kv.second >> 8) & 0xFF);
    mapData[kv.first * 2 + 1] = static_cast<char>(kv.second & 0xFF);
  }
  QPDFObjectHandle w = QPDFObjectHandle::newArray();
  for (const auto& kv : codeGid) {
    double adv = kv.second && advances.count(kv.second) ? advances[kv.second] / upemK : codeWidth[kv.first];
    QPDFObjectHandle one = QPDFObjectHandle::newArray();
    one.appendItem(QPDFObjectHandle::newInteger(llround(adv)));
    w.appendItem(QPDFObjectHandle::newInteger(kv.first));
    w.appendItem(one);
  }
  const char* ps = FT_Get_Postscript_Name(face);
  std::string psName = subsetTag(keep, key) + "+" + (ps ? ps : "NotoSansCJK");
  std::string original = nameOf(type0.getKey("/BaseFont"));
  QPDFObjectHandle fd = buildDescriptor(ctx, c, face, psName);
  fd.replaceKey("/Flags", QPDFObjectHandle::newInteger(4));
  QPDFObjectHandle prog = QPDFObjectHandle::newStream(&ctx.pdf, program);
  prog.getDict().replaceKey("/Length1", QPDFObjectHandle::newInteger(static_cast<long long>(program.size())));
  fd.replaceKey("/FontFile2", ctx.pdf.makeIndirectObject(prog));
  cidFont.replaceKey("/FontDescriptor", ctx.pdf.makeIndirectObject(fd));
  cidFont.replaceKey("/Subtype", QPDFObjectHandle::newName("/CIDFontType2"));
  cidFont.replaceKey("/BaseFont", QPDFObjectHandle::newName("/" + psName));
  QPDFObjectHandle sysInfo = QPDFObjectHandle::newDictionary();
  sysInfo.replaceKey("/Registry", QPDFObjectHandle::newString("Adobe"));
  sysInfo.replaceKey("/Ordering", QPDFObjectHandle::newString("Identity"));
  sysInfo.replaceKey("/Supplement", QPDFObjectHandle::newInteger(0));
  cidFont.replaceKey("/CIDSystemInfo", sysInfo);
  cidFont.replaceKey("/CIDToGIDMap", ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, mapData)));
  cidFont.replaceKey("/W", w);
  cidFont.replaceKey("/DW", QPDFObjectHandle::newInteger(1000));
  cidFont.removeKey("/W2");
  cidFont.removeKey("/DW2");
  type0.replaceKey("/BaseFont", QPDFObjectHandle::newName("/" + psName));
  type0.replaceKey("/Encoding", QPDFObjectHandle::newName(vertical ? "/Identity-V" : "/Identity-H"));
  if (unicodeCodes || orderingUni) {
    std::map<uint32_t, std::string> entries;
    for (const auto& kv : codeUni) {
      if (kv.second) entries[kv.first] = utf16Hex(kv.second);
    }
    type0.replaceKey("/ToUnicode", ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, buildCMapStream(entries, true))));
  }
  ctx.issue("FONT_SUBSTITUTED",
            "embedded a subset of " + std::string(ps ? ps : "a CJK font") + " (" + std::to_string(keep.size()) +
                " glyphs) as substitute for " + original,
            true);
  return true;
}

std::string hexSafeName(const std::string& raw) {
  std::string prefix;
  std::string body = raw;
  if (body.size() > 7 && body[6] == '+') {
    prefix = body.substr(0, 7);
    body = body.substr(7);
  }
  char buf[8];
  std::string out = prefix + "Font";
  for (unsigned char c : body) {
    if (std::isalnum(c)) {
      out += static_cast<char>(c);
    } else {
      std::snprintf(buf, sizeof(buf), "%02X", c);
      out += buf;
    }
  }
  if (out.size() > 60) out = out.substr(0, 60);
  return out;
}

int renameIfInvalid(Ctx&, QPDFObjectHandle dict, const char* key) {
  if (!dict.isDictionary()) return 0;
  QPDFObjectHandle v = dict.getKey(key);
  if (!v.isName()) return 0;
  std::string name = v.getName().substr(1);
  if (validUtf8(name)) return 0;
  dict.replaceKey(key, QPDFObjectHandle::newName("/" + hexSafeName(name)));
  return 1;
}

void sanitizeFontNames(Ctx& ctx, QPDFObjectHandle font) {
  int n = 0;
  n += renameIfInvalid(ctx, font, "/BaseFont");
  n += renameIfInvalid(ctx, font.getKey("/FontDescriptor"), "/FontName");
  QPDFObjectHandle df = font.getKey("/DescendantFonts");
  if (df.isArray() && df.getArrayNItems() == 1 && df.getArrayItem(0).isDictionary()) {
    QPDFObjectHandle cid = df.getArrayItem(0);
    n += renameIfInvalid(ctx, cid, "/BaseFont");
    n += renameIfInvalid(ctx, cid.getKey("/FontDescriptor"), "/FontName");
  }
  if (n) {
    ctx.issue("FONT_NAME_SANITIZED",
              "renamed font with invalid UTF-8 name to " + nameOf(font.getKey("/BaseFont")),
              true);
  }
}

bool descriptorHasFontFile(QPDFObjectHandle fd) {
  if (!fd.isDictionary()) return false;
  return fd.getKey("/FontFile").isStream() || fd.getKey("/FontFile2").isStream() ||
         fd.getKey("/FontFile3").isStream();
}

bool fontEmbedded(QPDFObjectHandle font) {
  std::string subtype = nameOf(font.getKey("/Subtype"));
  if (subtype == "/Type3") return true;
  if (subtype == "/Type0") {
    QPDFObjectHandle df = font.getKey("/DescendantFonts");
    if (df.isArray() && df.getArrayNItems() == 1) {
      QPDFObjectHandle cid = df.getArrayItem(0);
      if (cid.isDictionary()) return descriptorHasFontFile(cid.getKey("/FontDescriptor"));
    }
    return false;
  }
  return descriptorHasFontFile(font.getKey("/FontDescriptor"));
}

void collectFonts(Ctx& ctx, QPDFObjectHandle res, Visited& visited,
                  std::vector<QPDFObjectHandle>& fonts, const std::map<QPDFObjGen, FontUse>* uses = nullptr) {
  DepthGuard g_(visited);
  if (g_.over) return;
  if (!res.isDictionary() || !visited.enter(res)) return;
  QPDFObjectHandle fd = res.getKey("/Font");
  if (fd.isDictionary()) {
    for (const std::string& k : fd.getKeys()) {
      QPDFObjectHandle f = fd.getKey(k);
      bool notDict = !f.isDictionary() || f.isStream();
      std::string sub = notDict ? std::string() : nameOf(f.getKey("/Subtype"));
      bool fontType = !notDict && (!f.hasKey("/Type") || nameIs(f.getKey("/Type"), "/Font"));
      bool judged = !uses || (f.isIndirect() && uses->count(f.getObjGen()));
      if (fontType && (sub == "/CIDFontType0" || sub == "/CIDFontType2")) {
        QPDFObjectHandle wrap = QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type0 /Encoding /Identity-H >>");
        wrap.replaceKey("/BaseFont", f.getKey("/BaseFont").isName() ? f.getKey("/BaseFont") : QPDFObjectHandle::newName("/Unnamed"));
        wrap.replaceKey("/DescendantFonts", QPDFObjectHandle::newArray({f}));
        f = ctx.pdf.makeIndirectObject(wrap);
        fd.replaceKey(k, f);
        ctx.issue("FONT_RESOURCE_REPAIRED", "font resource " + k + " pointed at a CIDFont directly; wrapped it in a composite font", judged);
      } else if (notDict || !fontType || (sub != "/Type0" && sub != "/Type1" && sub != "/MMType1" && sub != "/Type3" && sub != "/TrueType") ||
                 f.hasKey("/Ordering") || f.hasKey("/BM")) {
        f = ctx.pdf.makeIndirectObject(
            QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));
        fd.replaceKey(k, f);
        ctx.issue("FONT_RESOURCE_REPAIRED", "font resource " + k + " pointed at an object that is not a font; replaced it with a standard font", judged);
      }
      if (f.isIndirect() && !visited.enter(f)) continue;
      fonts.push_back(f);
      if (nameIs(f.getKey("/Subtype"), "/Type3")) {
        collectFonts(ctx, f.getKey("/Resources"), visited, fonts, uses);
      }
    }
  }
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (xod.isDictionary()) {
    for (const std::string& k : xod.getKeys()) {
      QPDFObjectHandle xo = xod.getKey(k);
      if (xo.isStream() && visited.enter(xo) &&
          nameIs(xo.getDict().getKey("/Subtype"), "/Form")) {
        collectFonts(ctx, xo.getDict().getKey("/Resources"), visited, fonts, uses);
      }
    }
  }
  QPDFObjectHandle pat = res.getKey("/Pattern");
  if (pat.isDictionary()) {
    for (const std::string& k : pat.getKeys()) {
      QPDFObjectHandle p = pat.getKey(k);
      if (p.isStream() && visited.enter(p)) {
        collectFonts(ctx, p.getDict().getKey("/Resources"), visited, fonts, uses);
      }
    }
  }
}
}

bool simpleFontAdvances(QPDFObjectHandle font, std::vector<double>& w) {
  if (!font.isDictionary()) return false;
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  bool symbolic = fd.isDictionary() && (static_cast<long long>(numOf(fd.getKey("/Flags"), 0)) & 4) != 0;
  FtLib lib;
  FtFace face;
  QPDFObjectHandle prog = fontFileStream(fd);
  if (!(prog.isStream() && loadFace(lib, prog, face))) {
    SubstituteChoice c = chooseSubstitute(font, fd);
    if (!c.asset || c.symbolTable) return false;
    face.data.assign(reinterpret_cast<const char*>(c.asset->data), c.asset->len);
    if (FT_New_Memory_Face(lib.lib, reinterpret_cast<const FT_Byte*>(face.data.data()),
                           static_cast<FT_Long>(face.data.size()), 0, &face.face) != 0) {
      face.face = nullptr;
      return false;
    }
    symbolic = false;
  }
  if (!face.face) return false;
  HmtxTable hmtx = parseHmtx(face.data);
  double upem = face.face->units_per_EM ? face.face->units_per_EM : 1000.0;
  SimpleEncoding enc = readEncoding(font, symbolic);
  w.assign(256, 0);
  for (int code = 0; code < 256; ++code) {
    FT_UInt gid = glyphForCode(face.face, code, enc, symbolic);
    double adv = programAdvance(face, hmtx, gid, upem);
    w[code] = adv < 0 ? 0 : adv;
  }
  return true;
}

namespace {
bool sfntTable(const std::string& f, const char* tag, std::string& out) {
  const unsigned char* d = reinterpret_cast<const unsigned char*>(f.data());
  if (f.size() < 12) return false;
  unsigned numTables = (d[4] << 8) | d[5];
  for (unsigned i = 0; i < numTables && 12 + 16 * (i + 1) <= f.size(); ++i) {
    const unsigned char* rec = d + 12 + 16 * i;
    if (std::memcmp(rec, tag, 4) != 0) continue;
    size_t off = (static_cast<size_t>(rec[8]) << 24) | (rec[9] << 16) | (rec[10] << 8) | rec[11];
    size_t len = (static_cast<size_t>(rec[12]) << 24) | (rec[13] << 16) | (rec[14] << 8) | rec[15];
    if (off > f.size() || len > f.size() - off) return false;
    out = f.substr(off, len);
    return true;
  }
  return false;
}

bool cffIndexSkip(const std::string& c, size_t& pos, size_t* firstStart, size_t* firstEnd) {
  if (pos + 2 > c.size()) return false;
  unsigned count = (static_cast<unsigned char>(c[pos]) << 8) | static_cast<unsigned char>(c[pos + 1]);
  if (count == 0) {
    pos += 2;
    return true;
  }
  if (pos + 3 > c.size()) return false;
  unsigned offSize = static_cast<unsigned char>(c[pos + 2]);
  if (offSize < 1 || offSize > 4) return false;
  size_t offs = pos + 3;
  if (offs + static_cast<size_t>(count + 1) * offSize > c.size()) return false;
  auto off = [&](unsigned i) {
    size_t v = 0;
    for (unsigned k = 0; k < offSize; ++k) v = (v << 8) | static_cast<unsigned char>(c[offs + i * offSize + k]);
    return v;
  };
  size_t data = offs + static_cast<size_t>(count + 1) * offSize - 1;
  if (firstStart) *firstStart = data + off(0);
  if (firstEnd) *firstEnd = data + off(1);
  pos = data + off(count);
  return pos <= c.size();
}

bool cffCidKeyed(const std::string& c) {
  if (c.size() < 4) return false;
  size_t pos = static_cast<unsigned char>(c[2]);
  size_t ts = 0, te = 0;
  if (!cffIndexSkip(c, pos, nullptr, nullptr) || !cffIndexSkip(c, pos, &ts, &te) || te > c.size()) return false;
  for (size_t i = ts; i < te;) {
    unsigned b = static_cast<unsigned char>(c[i]);
    if (b == 12) {
      if (i + 1 < te && static_cast<unsigned char>(c[i + 1]) == 30) return true;
      i += 2;
    } else if (b <= 21) {
      i += 1;
    } else if (b == 28) {
      i += 3;
    } else if (b == 29) {
      i += 5;
    } else if (b == 30) {
      ++i;
      while (i < te && (static_cast<unsigned char>(c[i]) & 0x0F) != 0x0F && (static_cast<unsigned char>(c[i]) >> 4) != 0x0F) ++i;
      ++i;
    } else if (b >= 247 && b <= 254) {
      i += 2;
    } else {
      i += 1;
    }
  }
  return false;
}

void unwrapOpenType(Ctx& ctx, QPDFObjectHandle font, bool nameKeyedCidOnly, bool mislabeledOnly = false) {
  QPDFObjectHandle target = font;
  if (nameOf(font.getKey("/Subtype")) == "/Type0") {
    QPDFObjectHandle df = font.getKey("/DescendantFonts");
    if (!df.isArray() || df.getArrayNItems() != 1) return;
    target = df.getArrayItem(0);
  }
  QPDFObjectHandle fd = target.getKey("/FontDescriptor");
  QPDFObjectHandle ff = fd.isDictionary() ? fd.getKey("/FontFile3") : QPDFObjectHandle::newNull();
  if (!ff.isStream()) return;
  std::string otf;
  try {
    auto buf = ff.getStreamData(qpdf_dl_all);
    otf.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return;
  }
  bool mislabeled = nameOf(ff.getDict().getKey("/Subtype")) != "/OpenType" && otf.compare(0, 4, "OTTO") == 0;
  if ((nameOf(ff.getDict().getKey("/Subtype")) != "/OpenType" || mislabeledOnly) && !mislabeled) return;
  if (mislabeled) nameKeyedCidOnly = false;
  std::string sub = nameOf(target.getKey("/Subtype"));
  bool cid = sub == "/CIDFontType0" || sub == "/CIDFontType2";
  std::string base = nameOf(target.getKey("/BaseFont"));
  std::string cff;
  if (nameKeyedCidOnly &&
      !(sub == "/CIDFontType0" && otf.compare(0, 4, "OTTO") == 0 && sfntTable(otf, "CFF ", cff) && !cffCidKeyed(cff)))
    return;
  if (mislabeled && (!sfntTable(otf, "CFF ", cff) || (cid && target.getKey("/CIDToGIDMap").isStream()))) return;
  if (otf.compare(0, 4, "OTTO") == 0 && sfntTable(otf, "CFF ", cff) &&
      !(cid && target.getKey("/CIDToGIDMap").isStream())) {
    QPDFObjectHandle st = ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, cff));
    st.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName(cid ? "/CIDFontType0C" : "/Type1C"));
    fd.replaceKey("/FontFile3", st);
    if (cid) target.replaceKey("/Subtype", QPDFObjectHandle::newName("/CIDFontType0"));
    else if (sub != "/Type1" && sub != "/MMType1") target.replaceKey("/Subtype", QPDFObjectHandle::newName("/Type1"));
    if (cid) target.removeKey("/CIDToGIDMap");
  } else if (otf.compare(0, 4, std::string("\0\1\0\0", 4)) == 0 || otf.compare(0, 4, "true") == 0) {
    QPDFObjectHandle st = ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, otf));
    st.getDict().replaceKey("/Length1", QPDFObjectHandle::newInteger(static_cast<long long>(otf.size())));
    fd.removeKey("/FontFile3");
    fd.replaceKey("/FontFile2", st);
    if (cid) {
      if (sub != "/CIDFontType2") {
        target.replaceKey("/Subtype", QPDFObjectHandle::newName("/CIDFontType2"));
        target.replaceKey("/CIDToGIDMap", QPDFObjectHandle::newName("/Identity"));
      }
    } else {
      target.replaceKey("/Subtype", QPDFObjectHandle::newName("/TrueType"));
    }
  } else {
    ctx.fatal("FONT_FORMAT_NOT_ALLOWED", "font " + base + " is an OpenType font that cannot be stored in a PDF 1.4 format");
    return;
  }
  ctx.issue("OPENTYPE_UNWRAPPED", "stored the font program of " + base + " as bare compact font data", true);
}
}

bool fixProgramLengths(QPDFObjectHandle font) {
  QPDFObjectHandle target = font;
  if (nameIs(font.getKey("/Subtype"), "/Type0")) {
    QPDFObjectHandle df = font.getKey("/DescendantFonts");
    if (!df.isArray() || df.getArrayNItems() != 1) return false;
    target = df.getArrayItem(0);
  }
  QPDFObjectHandle fd = target.getKey("/FontDescriptor");
  if (!fd.isDictionary()) return false;
  QPDFObjectHandle ff = fd.getKey("/FontFile2");
  bool type1 = false;
  if (!ff.isStream()) {
    ff = fd.getKey("/FontFile");
    type1 = true;
  }
  if (!ff.isStream()) return false;
  std::string prog;
  try {
    auto buf = ff.getStreamData(qpdf_dl_all);
    prog.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return false;
  }
  long long l1 = static_cast<long long>(prog.size()), l2 = -1, l3 = -1;
  if (type1) {
    size_t e = prog.find("eexec");
    if (e == std::string::npos) return false;
    size_t p = e + 5;
    while (p < prog.size() && (prog[p] == '\r' || prog[p] == '\n' || prog[p] == ' ' || prog[p] == '\t')) ++p;
    size_t c = prog.rfind("cleartomark");
    size_t t = prog.size();
    if (c != std::string::npos && c > p) {
      t = c;
      while (t > p && (prog[t - 1] == '0' || prog[t - 1] == '\r' || prog[t - 1] == '\n' || prog[t - 1] == ' ' || prog[t - 1] == '\t')) --t;
    }
    l1 = static_cast<long long>(p);
    l2 = static_cast<long long>(t - p);
    l3 = static_cast<long long>(prog.size() - t);
  }
  QPDFObjectHandle d = ff.getDict();
  long long size = static_cast<long long>(prog.size());
  auto val = [&](const char* k) { return d.getKey(k).isInteger() ? d.getKey(k).getIntValue() : -1; };
  long long g1 = val("/Length1"), g2 = val("/Length2"), g3 = val("/Length3");
  bool broken = g1 <= 0 || g1 > size;
  if (type1) broken = broken || g2 <= 0 || g3 < 0 || g1 + g2 + g3 > size + 2;
  if (!broken) return false;
  d.replaceKey("/Length1", QPDFObjectHandle::newInteger(l1));
  if (type1) {
    d.replaceKey("/Length2", QPDFObjectHandle::newInteger(l2));
    d.replaceKey("/Length3", QPDFObjectHandle::newInteger(l3));
  }
  return true;
}

bool trimType1Program(QPDFObjectHandle font) {
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  QPDFObjectHandle ff = fd.isDictionary() ? fd.getKey("/FontFile") : QPDFObjectHandle::newNull();
  if (!ff.isStream()) return false;
  QPDFObjectHandle d = ff.getDict();
  if (!d.getKey("/Length1").isInteger() || !d.getKey("/Length2").isInteger()) return false;
  std::string prog;
  try {
    auto buf = ff.getStreamData(qpdf_dl_all);
    prog.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return false;
  }
  long long l1 = d.getKey("/Length1").getIntValue(), l2 = d.getKey("/Length2").getIntValue();
  if (l1 <= 0 || l2 <= 4 || l1 + l2 > static_cast<long long>(prog.size())) return false;
  std::string plain(static_cast<size_t>(l2), '\0');
  unsigned r = 55665;
  for (long long i = 0; i < l2; ++i) {
    unsigned char c = static_cast<unsigned char>(prog[static_cast<size_t>(l1 + i)]);
    plain[static_cast<size_t>(i)] = static_cast<char>(c ^ (r >> 8));
    r = ((c + r) * 52845u + 22719u) & 0xFFFFu;
  }
  size_t end = plain.find("closefile", 4);
  if (end == std::string::npos) return false;
  end += 9;
  size_t rest = end;
  while (rest < plain.size() && (plain[rest] == '\n' || plain[rest] == '\r' || plain[rest] == ' ' || plain[rest] == '\t')) ++rest;
  if (rest >= plain.size()) return false;
  plain.resize(end);
  plain += '\n';
  std::string enc(plain.size(), '\0');
  r = 55665;
  for (size_t i = 0; i < plain.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(static_cast<unsigned char>(plain[i]) ^ (r >> 8));
    enc[i] = static_cast<char>(c);
    r = ((c + r) * 52845u + 22719u) & 0xFFFFu;
  }
  std::string out = prog.substr(0, static_cast<size_t>(l1)) + enc + prog.substr(static_cast<size_t>(l1 + l2));
  ff.replaceStreamData(out, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
  d.replaceKey("/Length2", QPDFObjectHandle::newInteger(static_cast<long long>(enc.size())));
  d.replaceKey("/Length3", QPDFObjectHandle::newInteger(static_cast<long long>(prog.size()) - l1 - l2));
  return true;
}

bool inlineType1MetricSubrs(QPDFObjectHandle font) {
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  QPDFObjectHandle ff = fd.isDictionary() ? fd.getKey("/FontFile") : QPDFObjectHandle::newNull();
  if (!ff.isStream()) return false;
  QPDFObjectHandle d = ff.getDict();
  if (!d.getKey("/Length1").isInteger() || !d.getKey("/Length2").isInteger()) return false;
  std::string prog;
  try {
    auto buf = ff.getStreamData(qpdf_dl_all);
    prog.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return false;
  }
  long long l1 = d.getKey("/Length1").getIntValue(), l2 = d.getKey("/Length2").getIntValue();
  if (l1 <= 0 || l2 <= 4 || l1 + l2 > static_cast<long long>(prog.size())) return false;
  if (static_cast<unsigned char>(prog[static_cast<size_t>(l1)]) == 0x80) return false;
  std::string plain(static_cast<size_t>(l2), '\0');
  unsigned r = 55665;
  for (long long i = 0; i < l2; ++i) {
    unsigned char c = static_cast<unsigned char>(prog[static_cast<size_t>(l1 + i)]);
    plain[static_cast<size_t>(i)] = static_cast<char>(c ^ (r >> 8));
    r = ((c + r) * 52845u + 22719u) & 0xFFFFu;
  }
  int lenIV = 4;
  size_t li = plain.find("/lenIV");
  if (li != std::string::npos) lenIV = std::atoi(plain.c_str() + li + 6);
  if (lenIV < 0) return false;
  auto crypt = [](const std::string& in, bool decrypt) {
    std::string out(in.size(), '\0');
    unsigned k = 4330;
    for (size_t i = 0; i < in.size(); ++i) {
      unsigned char c = static_cast<unsigned char>(in[i]);
      unsigned char o = static_cast<unsigned char>(c ^ (k >> 8));
      out[i] = static_cast<char>(o);
      unsigned char cipher = decrypt ? c : o;
      k = ((cipher + k) * 52845u + 22719u) & 0xFFFFu;
    }
    return out;
  };
  auto isWs = [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
  auto readEntry = [&](size_t& pos, long long& len, size_t& dataAt) {
    while (pos < plain.size() && isWs(plain[pos])) ++pos;
    size_t numAt = pos;
    while (pos < plain.size() && std::isdigit(static_cast<unsigned char>(plain[pos]))) ++pos;
    if (pos == numAt) return false;
    len = std::atoll(plain.c_str() + numAt);
    while (pos < plain.size() && isWs(plain[pos])) ++pos;
    size_t tok = pos;
    while (pos < plain.size() && !isWs(plain[pos])) ++pos;
    std::string t = plain.substr(tok, pos - tok);
    if (t != "RD" && t != "-|") return false;
    dataAt = pos + 1;
    if (len < 0 || dataAt + static_cast<size_t>(len) > plain.size()) return false;
    pos = dataAt + static_cast<size_t>(len);
    return true;
  };
  std::map<long long, std::string> subrs;
  size_t sp = plain.find("/Subrs");
  size_t csAt = plain.find("/CharStrings");
  if (sp == std::string::npos || csAt == std::string::npos || sp > csAt) return false;
  for (size_t pos = sp; pos < csAt;) {
    size_t dup = plain.find("dup ", pos);
    if (dup == std::string::npos || dup > csAt) break;
    pos = dup + 4;
    size_t idxAt = pos;
    while (pos < plain.size() && std::isdigit(static_cast<unsigned char>(plain[pos]))) ++pos;
    if (pos == idxAt) continue;
    long long idx = std::atoll(plain.c_str() + idxAt);
    long long len = 0;
    size_t dataAt = 0;
    if (!readEntry(pos, len, dataAt)) return false;
    std::string dec = crypt(plain.substr(dataAt, static_cast<size_t>(len)), true);
    if (dec.size() < static_cast<size_t>(lenIV)) return false;
    subrs[idx] = dec.substr(static_cast<size_t>(lenIV));
  }
  auto setsMetrics = [](const std::string& b) {
    for (size_t i = 0; i < b.size();) {
      unsigned char v = static_cast<unsigned char>(b[i]);
      if (v >= 32) {
        i += v <= 246 ? 1 : v <= 254 ? 2 : 5;
        continue;
      }
      if (v == 13) return true;
      if (v == 12) {
        if (i + 1 < b.size() && static_cast<unsigned char>(b[i + 1]) == 7) return true;
        i += 2;
        continue;
      }
      if (v == 11) return false;
      ++i;
    }
    return false;
  };
  std::string rebuilt = plain.substr(0, csAt);
  size_t pos = csAt;
  bool changed = false;
  size_t dictStart = plain.find("begin", csAt);
  if (dictStart == std::string::npos) return false;
  dictStart += 5;
  rebuilt += plain.substr(csAt, dictStart - csAt);
  pos = dictStart;
  while (true) {
    size_t slash = pos;
    while (slash < plain.size() && isWs(plain[slash])) ++slash;
    if (plain.compare(slash, 2, "ND") == 0 || plain.compare(slash, 2, "|-") == 0) slash += 2;
    while (slash < plain.size() && isWs(plain[slash])) ++slash;
    if (slash >= plain.size() || plain[slash] != '/') break;
    size_t nameEnd = slash + 1;
    while (nameEnd < plain.size() && !isWs(plain[nameEnd]) && plain[nameEnd] != '/') ++nameEnd;
    size_t p2 = nameEnd;
    long long len = 0;
    size_t dataAt = 0;
    if (!readEntry(p2, len, dataAt)) return false;
    std::string dec = crypt(plain.substr(dataAt, static_cast<size_t>(len)), true);
    std::string head = dec.substr(0, std::min(dec.size(), static_cast<size_t>(lenIV)));
    std::string body = dec.size() > static_cast<size_t>(lenIV) ? dec.substr(static_cast<size_t>(lenIV)) : std::string();
    size_t lastOpnd = std::string::npos, i = 0;
    int op = -1;
    while (i < body.size()) {
      unsigned char v = static_cast<unsigned char>(body[i]);
      if (v >= 32) {
        lastOpnd = i;
        i += v <= 246 ? 1 : v <= 254 ? 2 : 5;
        continue;
      }
      op = v;
      break;
    }
    std::string nb = body;
    if (op == 10 && lastOpnd != std::string::npos && i < body.size()) {
      unsigned char v = static_cast<unsigned char>(body[lastOpnd]);
      long long n = -1;
      if (v <= 246) n = v - 139;
      else if (v <= 250) n = (v - 247) * 256 + static_cast<unsigned char>(body[lastOpnd + 1]) + 108;
      else if (v <= 254) n = -(v - 251) * 256 - static_cast<unsigned char>(body[lastOpnd + 1]) - 108;
      else if (lastOpnd + 4 < body.size()) {
        uint32_t u = 0;
        for (int b = 1; b <= 4; ++b) u = (u << 8) | static_cast<unsigned char>(body[lastOpnd + b]);
        n = static_cast<int32_t>(u);
      }
      auto it = subrs.find(n);
      if (it != subrs.end() && !it->second.empty() &&
          static_cast<unsigned char>(it->second.back()) == 11 && setsMetrics(it->second)) {
        nb = body.substr(0, lastOpnd) + it->second.substr(0, it->second.size() - 1) + body.substr(i + 1);
      }
    }
    std::string enc = crypt(head + nb, false);
    if (nb != body) changed = true;
    size_t after = dataAt + static_cast<size_t>(len);
    rebuilt += plain.substr(pos, nameEnd - pos) + " " + std::to_string(enc.size()) + " " +
               plain.substr(dataAt - 3, 3) + enc;
    pos = after;
  }
  if (!changed) return false;
  rebuilt += plain.substr(pos);
  std::string enc(rebuilt.size(), '\0');
  r = 55665;
  for (size_t k = 0; k < rebuilt.size(); ++k) {
    unsigned char c = static_cast<unsigned char>(static_cast<unsigned char>(rebuilt[k]) ^ (r >> 8));
    enc[k] = static_cast<char>(c);
    r = ((c + r) * 52845u + 22719u) & 0xFFFFu;
  }
  std::string out = prog.substr(0, static_cast<size_t>(l1)) + enc + prog.substr(static_cast<size_t>(l1 + l2));
  ff.replaceStreamData(out, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
  d.replaceKey("/Length2", QPDFObjectHandle::newInteger(static_cast<long long>(enc.size())));
  return true;
}

std::map<uint32_t, std::string> worldToUnicodeMap(QPDFObjectHandle tu) {
  std::map<uint32_t, std::string> out = parseExistingToUnicode(tu);
  for (auto it = out.begin(); it != out.end();) it = it->second.empty() ? out.erase(it) : std::next(it);
  return out;
}
std::set<uint32_t> puaGlyphCodes(QPDFObjectHandle font) {
  std::set<uint32_t> out;
  if (!font.isDictionary()) return out;
  for (const auto& [code, hex] : parseExistingToUnicode(font.getKey("/ToUnicode"))) {
    for (size_t i = 0; i + 4 <= hex.size(); i += 4) {
      unsigned long u = std::strtoul(hex.substr(i, 4).c_str(), nullptr, 16);
      if ((u >= 0xE000 && u <= 0xF8FF) || (u >= 0xDB80 && u <= 0xDBFF)) {
        out.insert(code);
        break;
      }
    }
  }
  return out;
}
std::string worldToUnicodeCMap(const std::map<uint32_t, std::string>& entries, bool twoByte) {
  return buildCMapStream(entries, twoByte);
}

namespace {
bool simpleCoveredByLiberation(FtLib& lib, EmbedCache& cache, QPDFObjectHandle font) {
  SubstituteChoice c = chooseSubstitute(font, font.getKey("/FontDescriptor"));
  if (!c.asset || c.symbolTable) return true;
  auto face = assetFace(lib, cache, c.asset);
  if (!face->face || FT_Select_Charmap(face->face, FT_ENCODING_UNICODE) != 0) return true;
  std::map<uint32_t, std::string> tu = parseExistingToUnicode(font.getKey("/ToUnicode"));
  SimpleEncoding enc = readEncoding(font, false);
  for (int code = 0; code < 256; ++code) {
    uint32_t u = 0;
    auto t = tu.find(static_cast<uint32_t>(code));
    if (t != tu.end()) {
      u = hexToCodepoint(t->second);
    } else if (!enc.diffs[code].empty()) {
      u = aglNameToUnicode(enc.diffs[code].substr(1));
    }
    if (!u || u <= 0x20 || (u >= 0x7F && u <= 0xA0)) continue;
    if (!FT_Get_Char_Index(face->face, u)) return false;
  }
  return true;
}

bool standardFourteen(const std::string& base) {
  static const std::set<std::string> kNames = {
      "/Courier", "/Courier-Bold", "/Courier-BoldOblique", "/Courier-Oblique", "/Helvetica",
      "/Helvetica-Bold", "/Helvetica-BoldOblique", "/Helvetica-Oblique", "/Symbol", "/Times-Bold",
      "/Times-BoldItalic", "/Times-Italic", "/Times-Roman", "/ZapfDingbats"};
  return kNames.count(base) != 0;
}

bool fontKeysConform(Ctx& ctx, QPDFObjectHandle dict, bool fix) {
  bool ok = true;
  if (!nameIs(dict.getKey("/Type"), "/Font")) {
    ok = false;
    if (fix) dict.replaceKey("/Type", QPDFObjectHandle::newName("/Font"));
  }
  QPDFObjectHandle fd = dict.getKey("/FontDescriptor");
  if (!nameIs(dict.getKey("/Subtype"), "/Type3") && !dict.getKey("/BaseFont").isName()) {
    ok = false;
    if (fix) {
      QPDFObjectHandle fn = fd.isDictionary() ? fd.getKey("/FontName") : QPDFObjectHandle::newNull();
      dict.replaceKey("/BaseFont", fn.isName() ? fn : QPDFObjectHandle::newName("/Unnamed"));
    }
  }
  if (!fd.isDictionary()) return ok;
  for (const char* key : {"/FontFile", "/FontFile2", "/FontFile3"}) {
    QPDFObjectHandle ff = fd.getKey(key);
    if (!ff.isStream()) continue;
    QPDFObjectHandle st = ff.getDict().getKey("/Subtype");
    std::string sub = st.isName() ? st.getName() : "";
    if (!st.isName() || sub == "/Type1C" || sub == "/CIDFontType0C" || (sub == "/OpenType" && ctx.part >= 2)) break;
    ok = false;
    if (!fix) break;
    if (std::string(key) != "/FontFile3") {
      ff.getDict().removeKey("/Subtype");
      break;
    }
    std::string data;
    try {
      auto buf = ff.getStreamData(qpdf_dl_all);
      data.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    } catch (...) {
      break;
    }
    std::string fixed = data.compare(0, 4, "OTTO") == 0 ? "/OpenType" : cffCidKeyed(data) ? "/CIDFontType0C" : "/Type1C";
    ff.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName(fixed));
    break;
  }
  return ok;
}

bool simpleWidthKeysConform(QPDFObjectHandle f) {
  std::string sub = nameOf(f.getKey("/Subtype"));
  if ((sub == "/Type1" || sub == "/MMType1") && !fontEmbedded(f) && standardFourteen(nameOf(f.getKey("/BaseFont")))) return true;
  QPDFObjectHandle first = f.getKey("/FirstChar");
  QPDFObjectHandle last = f.getKey("/LastChar");
  QPDFObjectHandle widths = f.getKey("/Widths");
  return first.isNumber() && last.isNumber() && widths.isArray() &&
         widths.getArrayNItems() == static_cast<int>(last.getNumericValue() - first.getNumericValue()) + 1;
}
}

void passFonts(Ctx& ctx) {
  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFObjectHandle> fonts;
  Visited visited;
  std::map<QPDFObjGen, FontUse> uses;
  if (ctx.opt.verifyOnly) uses = collectFontUse(ctx.pdf);
  const bool judgeUsed = ctx.opt.verifyOnly && ctx.isA();
  int kuraPage1 = 0;
  for (auto& ph : dh.getAllPages()) {
    PageScope kuraScope1(ctx, ++kuraPage1);
    QPDFObjectHandle page = ph.getObjectHandle();
    collectFonts(ctx, ph.getAttribute("/Resources", false), visited, fonts, judgeUsed ? &uses : nullptr);
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (annots.isArray()) {
      for (int i = 0; i < annots.getArrayNItems(); ++i) {
        QPDFObjectHandle a = annots.getArrayItem(i);
        if (!a.isDictionary()) continue;
        std::vector<QPDFObjectHandle> streams = normalAppearanceStreams(a);
        for (QPDFObjectHandle s : streams) {
          collectFonts(ctx, s.getDict().getKey("/Resources"), visited, fonts, judgeUsed ? &uses : nullptr);
        }
      }
    }
  }

  FtLib lib;
  EmbedCache cache;
  const FontUse noUse;
  auto fontUse = [&](QPDFObjectHandle f) -> const FontUse* {
    if (!ctx.opt.verifyOnly || !f.isIndirect()) return nullptr;
    auto it = uses.find(f.getObjGen());
    return it == uses.end() ? &noUse : &it->second;
  };
  std::set<std::string> reported;
  std::set<QPDFObjGen> unfixable;
  std::map<QPDFObjGen, FontUse> cjkUses;
  for (QPDFObjectHandle f : fonts) {
    if (judgeUsed && (!f.isIndirect() || !uses.count(f.getObjGen()))) {
      sanitizeFontNames(ctx, f);
      continue;
    }
    {
      QPDFObjectHandle dfk = f.getKey("/DescendantFonts");
      bool keysOk = fontKeysConform(ctx, f, !ctx.opt.verifyOnly);
      if (nameIs(f.getKey("/Subtype"), "/Type0") && dfk.isArray() && dfk.getArrayNItems() == 1 &&
          dfk.getArrayItem(0).isDictionary()) {
        keysOk = fontKeysConform(ctx, dfk.getArrayItem(0), !ctx.opt.verifyOnly) && keysOk;
      }
      if (!keysOk && ctx.isA()) {
        ctx.issue("FONT_DICT_REPAIRED", "font dictionary of " + nameOf(f.getKey("/BaseFont")) +
                                            " lacks a required entry or names a disallowed font program type", true);
      }
      std::string sk = nameOf(f.getKey("/Subtype"));
      if (judgeUsed && (sk == "/Type1" || sk == "/MMType1" || sk == "/TrueType" || sk == "/Type3") &&
          !simpleWidthKeysConform(f)) {
        ctx.issue("FONT_WIDTHS_SYNCED", "/FirstChar, /LastChar or /Widths of " + nameOf(f.getKey("/BaseFont")) +
                                            " is missing or does not cover the declared code range", true);
      }
    }
    sanitizeFontNames(ctx, f);
    if (ctx.pdf14Target() || ctx.isX()) {
      unwrapOpenType(ctx, f, !ctx.pdf14Target());
      if (ctx.failed()) return;
    } else {
      unwrapOpenType(ctx, f, false, true);
    }
    std::string base = nameOf(f.getKey("/BaseFont"));
    std::string subtype = nameOf(f.getKey("/Subtype"));
    bool embedded = fontEmbedded(f);
    bool corrupt = embedded && subtype != "/Type3" && !programUsable(lib, f, subtype);
    const Level lvl = ctx.opt.level;
    if (embedded && !corrupt && subtype != "/Type3" &&
        (ctx.opt.ua || ctx.isA() || ((lvl == Level::X1A || lvl == Level::X3) ? subtype == "/TrueType" || subtype == "/Type0"
                                                                : lvl == Level::X6)) &&
        embeddingRestricted(lib, f, subtype)) {
      QPDFObjectHandle lfd = subtype == "/Type0" && f.getKey("/DescendantFonts").isArray() &&
                                     f.getKey("/DescendantFonts").getArrayNItems() == 1
                                 ? f.getKey("/DescendantFonts").getArrayItem(0).getKey("/FontDescriptor")
                                 : f.getKey("/FontDescriptor");
      if (lfd.isDictionary()) {
        for (const char* k : {"/FontFile", "/FontFile2", "/FontFile3"}) lfd.removeKey(k);
      }
      ctx.issue("FONT_FSTYPE_RESTRICTED", "the OS/2 fsType of " + base + " allows only restricted-licence embedding; replaced with a substitute", true);
      embedded = false;
    }
    if (corrupt) {
      QPDFObjectHandle fd = subtype == "/Type0" && f.getKey("/DescendantFonts").isArray() &&
                                    f.getKey("/DescendantFonts").getArrayNItems() == 1
                                ? f.getKey("/DescendantFonts").getArrayItem(0).getKey(
                                      "/FontDescriptor")
                                : f.getKey("/FontDescriptor");
      if (fd.isDictionary()) {
        for (const char* k : {"/FontFile", "/FontFile2", "/FontFile3"}) fd.removeKey(k);
      }
      ctx.issue("FONT_PROGRAM_UNUSABLE",
                "embedded program for " + base + " could not be parsed; substituting", true);
      embedded = false;
    }
    if (embedded && fixProgramLengths(f)) {
      ctx.issue("FONT_PROGRAM_LENGTHS_FIXED", "recorded the correct program section lengths for " + base, !ctx.opt.verifyOnly);
    }
    if (embedded && subtype == "/Type1" && trimType1Program(f)) {
      ctx.issue("FONT_PROGRAM_TRIMMED", "removed data after the end of the encrypted section of " + base, true);
    }
    if (embedded && subtype == "/Type1" && inlineType1MetricSubrs(f)) {
      ctx.issue("FONT_PROGRAM_METRICS_INLINED",
                "moved glyph width instructions of " + base + " out of shared subroutines so every glyph states its own width",
                true);
    }
    if (!embedded && subtype != "/Type3" && fontUse(f) && !fontUse(f)->visibleShow) continue;
    if (!embedded && subtype != "/Type3") {
      bool ok = false;
      if (subtype == "/Type0") {
        if (ctx.opt.loadCjkFont && !fontUse(f) && cjkUses.empty()) cjkUses = collectFontUse(ctx.pdf);
        const FontUse* cu = fontUse(f);
        if (!cu && f.isIndirect()) {
          auto cit = cjkUses.find(f.getObjGen());
          if (cit != cjkUses.end()) cu = &cit->second;
        }
        ok = embedCjkSubstitute(ctx, lib, cache, f, cu);
        if (!ok) ok = embedCidSubstitute(ctx, lib, cache, f);
        if (!ok) ok = embedWorldSubstitute(ctx, f);
      } else {
        ok = simpleCoveredByLiberation(lib, cache, f) && embedSimpleSubstitute(ctx, lib, cache, f);
        if (!ok) ok = embedWorldSubstitute(ctx, f);
      }
      if (!ok) {
        if (f.isIndirect()) unfixable.insert(f.getObjGen());
        if (reported.insert(base).second) {
          ctx.issue("FONT_NOT_EMBEDDED",
                    "font " + base + " is not embedded and no substitute applies", ctx.opt.verifyOnly);
        }
        continue;
      }
      subtype = nameOf(f.getKey("/Subtype"));
    }
    bool needUnicode = ctx.needUnicode() && !(ctx.opt.verifyOnly && ctx.part >= 4 && !ctx.ua2());
    if (subtype == "/TrueType" || subtype == "/Type1" || subtype == "/MMType1" ||
        subtype == "/Type3") {
      if (needUnicode && !fontUse(f)) ensureSimpleToUnicode(ctx, lib, f);
    }
    if (subtype == "/Type0" && needUnicode && !fontUse(f)) ensureCidToUnicode(ctx, lib, f);
    if (fontUse(f) && fontUse(f)->partialCode) {
      ctx.issue("MISSING_GLYPHS_REMOVED", "text shown with " + base + " ends in an incomplete character code", true);
    }
    if (fontUse(f)) {
      int forbidden = 0;
      int missing = verifyUnicode(ctx, lib, f, *fontUse(f), forbidden);
      if (needUnicode && missing) {
        ctx.issue("TOUNICODE_BUILT", std::to_string(missing) + " used character code(s) of " + base +
                                         " have no Unicode mapping", true);
      }
      if (ctx.conf == 'A' && (ctx.part == 2 || ctx.part == 3) && puaWithoutActualText(ctx, f, *fontUse(f))) {
        ctx.issue("ACTUALTEXT_MISSING", "private-use characters of " + base + " are shown without ActualText", true);
      }
      if (forbidden && (needUnicode || (ctx.isA() && ctx.part >= 4))) {
        ctx.issue("TOUNICODE_SCRUBBED", std::to_string(forbidden) + " used character code(s) of " + base +
                                            " map to U+0000, U+FEFF or U+FFFE", true);
      }
    } else {
      scrubToUnicode(ctx, f);
    }
    if (subtype == "/TrueType" || subtype == "/Type1" || subtype == "/MMType1") {
      QPDFObjectHandle flagFd = f.getKey("/FontDescriptor");
      if (subtype == "/TrueType" && !ctx.opt.verifyOnly && flagFd.isDictionary() && flagFd.getKey("/Flags").isInteger() &&
          (flagFd.getKey("/Flags").getIntValue() & 36) == 36) {
        long long fl = flagFd.getKey("/Flags").getIntValue();
        flagFd.replaceKey("/Flags", QPDFObjectHandle::newInteger(programHasCmap(lib, f, 3, 0) ? fl & ~32LL : fl & ~4LL));
        ctx.issue("FONT_FLAGS_FIXED", "made the symbolic and non-symbolic flags of " + base + " consistent", true);
      }
      if (subtype == "/TrueType" && (ctx.isA() || ctx.isX() || ctx.isE())) {
        int act = symbolicCmapAction(lib, f, ctx.opt.verifyOnly);
        if (act == 1) {
          QPDFObjectHandle fd2 = f.getKey("/FontDescriptor");
          long long fl = fd2.getKey("/Flags").getIntValue();
          fd2.replaceKey("/Flags", QPDFObjectHandle::newInteger((fl & ~4LL) | 32LL));
          f.replaceKey("/Encoding", QPDFObjectHandle::newName("/WinAnsiEncoding"));
          f.removeKey("/ToUnicode");
          ctx.issue("SYMBOLIC_FLAG_CLEARED",
                    "reclassified " + base +
                        " as non-symbolic WinAnsi (font has a Unicode cmap; multiple cmap "
                        "encodings violate the symbolic-font rule)",
                    true);
        } else if (act == 2) {
          QPDFObjectHandle fd2 = f.getKey("/FontDescriptor");
          for (const char* k : {"/FontFile", "/FontFile2", "/FontFile3"}) fd2.removeKey(k);
          if (embedSimpleSubstitute(ctx, lib, cache, f)) {
            ctx.issue("FONT_SUBSTITUTED",
                      "replaced " + base +
                          " (symbolic TrueType with non-conforming cmap encodings)",
                      true);
            subtype = nameOf(f.getKey("/Subtype"));
          }
        }
      }
      if (subtype == "/TrueType" && (lvl == Level::X6 || (ctx.isA() && ctx.part >= 2))) {
        QPDFObjectHandle fdx = f.getKey("/FontDescriptor");
        long long flx = fdx.isDictionary() && fdx.getKey("/Flags").isInteger() ? fdx.getKey("/Flags").getIntValue() : 0;
        QPDFObjectHandle encx = f.getKey("/Encoding");
        bool diffx = ctx.isA() && encx.isDictionary() && encx.hasKey("/Differences");
        if (!(flx & 4) && fontEmbedded(f) && !programHasCmap(lib, f, 3, 1) &&
            (diffx || !programHasCmap(lib, f, 1, 0)) && addMsUnicodeCmap(f)) {
          ctx.issue("FONT_CMAP_ADDED", "added a Microsoft Unicode cmap subtable to " + base, true);
        }
        if (!(flx & 4) && fontEmbedded(f) && !programHasCmap(lib, f, 3, 1) &&
            (diffx || !programHasCmap(lib, f, 1, 0))) {
          for (const char* k : {"/FontFile", "/FontFile2", "/FontFile3"}) fdx.removeKey(k);
          if (embedSimpleSubstitute(ctx, lib, cache, f)) {
            ctx.issue("FONT_SUBSTITUTED", "replaced " + base + " (its program has no cmap subtable that can resolve its encoding)", true);
            subtype = nameOf(f.getKey("/Subtype"));
          } else if (f.isIndirect()) {
            unfixable.insert(f.getObjGen());
          }
        }
      }
      if (subtype == "/TrueType" && (ctx.isX() || ctx.isE()) && !ctx.opt.verifyOnly) {
        QPDFObjectHandle fdx = f.getKey("/FontDescriptor");
        long long flx = fdx.isDictionary() && fdx.getKey("/Flags").isInteger() ? fdx.getKey("/Flags").getIntValue() : 0;
        if (!(flx & 4) && fontEmbedded(f) && !programHasCmap(lib, f, 3, 1) && !programHasCmap(lib, f, 1, 0) &&
            addMsUnicodeCmap(f)) {
          ctx.issue("FONT_CMAP_ADDED", "added a Microsoft Unicode cmap subtable to " + base, true);
        }
      }
      if (subtype == "/TrueType") {
        fixNonsymbolicTtfEncoding(ctx, f);
        QPDFObjectHandle fd = f.getKey("/FontDescriptor");
        long long flags = fd.isDictionary() && fd.getKey("/Flags").isInteger()
                              ? fd.getKey("/Flags").getIntValue()
                              : 0;
        if ((flags & 4) && f.hasKey("/Encoding")) {
          f.removeKey("/Encoding");
          ctx.issue("SYMBOLIC_ENCODING_REMOVED",
                    "removed /Encoding from symbolic TrueType font " + base, true);
        }
        if ((ctx.isA() && ctx.part == 1) || ctx.isE() || lvl == Level::X4 || lvl == Level::VT1 || lvl == Level::VT3 ||
            (lvl == Level::X6 && !programHasCmap(lib, f, 3, 1))) {
          QPDFObjectHandle e2 = f.getKey("/Encoding");
          if (e2.isDictionary() && e2.hasKey("/Differences")) {
            std::string be = nameOf(e2.getKey("/BaseEncoding"));
            f.replaceKey("/Encoding",
                         QPDFObjectHandle::newName(be == "/MacRomanEncoding"
                                                       ? "/MacRomanEncoding"
                                                       : "/WinAnsiEncoding"));
            ctx.issue("TT_DIFFERENCES_REMOVED",
                      "removed /Differences from TrueType font " + base +
                          " (the target requires plain MacRoman/WinAnsi encoding; visual "
                          "difference possible)",
                      true);
          }
        }
      }
      syncSimpleFontWidths(ctx, lib, f, fontUse(f));
      QPDFObjectHandle csFd = f.getKey("/FontDescriptor");
      bool isT1 = subtype == "/Type1" || subtype == "/MMType1";
      if (ctx.opt.verifyOnly && ctx.isA() && isT1 && fontEmbedded(f)) {
        bool has = csFd.isDictionary() && csFd.hasKey("/CharSet");
        const FontUse* used = ctx.part == 1 ? fontUse(f) : nullptr;
        if (isSubsetName(base) && (ctx.part == 1 ? !has || !charSetConsistent(lib, f, used)
                                                  : has && ctx.part <= 3 && !charSetConsistent(lib, f, nullptr))) {
          ctx.issue("CHARSET_ADDED", "/CharSet of " + base + " does not list exactly the glyphs of the font program", true);
        }
      } else if (ctx.opt.verifyOnly && ctx.isA()) {
      } else if ((ctx.isA() && ctx.part == 1) || ctx.isE()) {
        if (subtype == "/Type1" || subtype == "/MMType1") ensureCharSet(ctx, lib, f);
      } else if (ctx.part >= 2 || !ctx.isA()) {
        QPDFObjectHandle fd = f.getKey("/FontDescriptor");
        if (fd.isDictionary() && fd.hasKey("/CharSet")) {
          fd.removeKey("/CharSet");
          ctx.issue("CHARSET_REMOVED",
                    "removed /CharSet from " + base + " (consistency not guaranteed)", true);
        }
      }
    } else if (subtype == "/Type3") {
      syncType3Widths(ctx, f, fontUse(f));
    } else if (subtype == "/Type0") {
      if (ctx.isA() && f.isIndirect()) {
        QPDFObjectHandle enc0 = f.getKey("/Encoding");
        if (enc0.isName()) {
          std::string en = enc0.getName();
          if (en != "/Identity-H" && en != "/Identity-V") {
            for (const auto& entry : kPredefRos) {
              if (en.rfind(entry.prefix, 0) == 0) {
                unfixable.insert(f.getObjGen());
                break;
              }
            }
          }
        }
      }
      embedFallbackCMap(ctx, f);
      fixEmbeddedCMap(ctx, f);
      syncRosWithPredefined(ctx, f);
      if (!fontUse(f) || !fontUse(f)->visibleCodes.empty()) ensureCidToGidMap(ctx, f);
      syncCidFontWidths(ctx, lib, f, fontUse(f));
      QPDFObjectHandle dfv = f.getKey("/DescendantFonts");
      QPDFObjectHandle cidFd = dfv.isArray() && dfv.getArrayNItems() == 1 && dfv.getArrayItem(0).isDictionary()
                                   ? dfv.getArrayItem(0).getKey("/FontDescriptor")
                                   : QPDFObjectHandle::newNull();
      if (ctx.opt.verifyOnly && ctx.isA()) {
        bool has = cidFd.isDictionary() && cidFd.hasKey("/CIDSet");
        bool subset = isSubsetName(nameOf(dfv.isArray() && dfv.getArrayNItems() == 1 ? dfv.getArrayItem(0).getKey("/BaseFont") : QPDFObjectHandle::newNull()));
        const std::set<uint32_t>* usedCids = ctx.part == 1 && fontUse(f) ? &fontUse(f)->cids : nullptr;
        if (subset && (ctx.part == 1 ? !has || !cidSetConsistent(lib, f, usedCids)
                                     : has && ctx.part <= 3 && fontEmbedded(f) && !cidSetConsistent(lib, f, nullptr))) {
          ctx.issue("CIDSET_ADDED", "/CIDSet of " + base + " does not identify exactly the CIDs of the font program", true);
        }
      } else if ((ctx.isA() && ctx.part == 1) || ctx.isE()) {
        ensureCidSet(ctx, lib, f);
      } else {
        QPDFObjectHandle df = f.getKey("/DescendantFonts");
        if (df.isArray() && df.getArrayNItems() == 1 && df.getArrayItem(0).isDictionary()) {
          QPDFObjectHandle fd = df.getArrayItem(0).getKey("/FontDescriptor");
          if (fd.isDictionary() && fd.hasKey("/CIDSet")) {
            fd.removeKey("/CIDSet");
            ctx.issue("CIDSET_REMOVED",
                      "removed /CIDSet from " + base + " (consistency not guaranteed)", true);
          }
        }
      }
    }
  }

  if (!unfixable.empty()) {
    QPDFPageDocumentHelper dh2(ctx.pdf);
    std::vector<QPDFPageObjectHelper> pages = dh2.getAllPages();
    for (size_t i = 0; i < pages.size(); ++i) {
      PageScope kuraScope(ctx, static_cast<int>(i) + 1);
      QPDFObjectHandle page = pages[i].getObjectHandle();
      std::vector<QPDFObjectHandle> pageFonts;
      {
        Visited pv;
        collectFonts(ctx, page.getKey("/Resources"), pv, pageFonts);
      }
      bool badContent = false;
      for (QPDFObjectHandle& pf : pageFonts) {
        if (pf.isIndirect() && unfixable.count(pf.getObjGen())) {
          badContent = true;
          break;
        }
      }
      std::vector<int> badAnnots;
      QPDFObjectHandle annots = page.getKey("/Annots");
      if (annots.isArray()) {
        for (int ai = 0; ai < annots.getArrayNItems(); ++ai) {
          QPDFObjectHandle a = annots.getArrayItem(ai);
          if (!a.isDictionary()) continue;
          std::vector<QPDFObjectHandle> streams = normalAppearanceStreams(a);
          bool badAp = false;
          for (QPDFObjectHandle& st : streams) {
            std::vector<QPDFObjectHandle> af;
            Visited av;
            collectFonts(ctx, st.getDict().getKey("/Resources"), av, af);
            for (QPDFObjectHandle& pf : af) {
              if (pf.isIndirect() && unfixable.count(pf.getObjGen())) {
                badAp = true;
                break;
              }
            }
            if (badAp) break;
          }
          if (badAp) badAnnots.push_back(ai);
        }
      }
      if (badContent) {
        bool keepPageText = true;
        {
          std::vector<QPDFObjectHandle> rasterFonts;
          Visited rasterSeen;
          collectFonts(ctx, pages[i].getAttribute("/Resources", false), rasterSeen, rasterFonts);
          for (QPDFObjectHandle& pf : rasterFonts) {
            if (!pf.isDictionary()) continue;
            QPDFObjectHandle dfs = pf.getKey("/DescendantFonts");
            if (nameIs(pf.getKey("/Subtype"), "/Type0") &&
                (!dfs.isArray() || dfs.getArrayNItems() != 1 || !dfs.getArrayItem(0).isDictionary()))
              keepPageText = false;
            QPDFObjectHandle e = pf.getKey("/Encoding");
            if (nameIs(pf.getKey("/Subtype"), "/Type0") && (e.isStream() || (e.isName() && e.getName() != "/Identity-H" &&
                                                                               e.getName() != "/Identity-V")))
              keepPageText = false;
          }
        }
        if (!ctx.opt.rasterizePage || !rasterFlattenPage(ctx, pages[i], static_cast<int>(i), keepPageText)) {
          ctx.fatal("FONT_UNEMBEDDABLE",
                    "page " + std::to_string(i + 1) +
                        " uses a font that cannot be embedded or substituted (CID font "
                        "without a usable substitute, or a predefined CMap that PDF/A "
                        "requires to be embedded) and rasterization is unavailable");
          return;
        }
        QPDFObjectHandle img =
            page.getKey("/Resources").getKey("/XObject").getKey("/FlatIm");
        if (img.isStream()) {
          QPDFObjectHandle icc = buildIccStream(ctx, kSrgbIcc, kSrgbIccLen, 3);
          QPDFObjectHandle arr = QPDFObjectHandle::newArray();
          arr.appendItem(QPDFObjectHandle::newName("/ICCBased"));
          arr.appendItem(icc);
          img.getDict().replaceKey("/ColorSpace", ctx.pdf.makeIndirectObject(arr));
          if (ctx.opt.level == Level::X1A && !convertImageToProcess(ctx, img)) {
            ctx.fatal("X1A_COLOR_UNCONVERTIBLE",
                      "page " + std::to_string(i + 1) +
                          " had to be rendered to an image, and that image could not be converted to CMYK");
            return;
          }
        }
        ctx.issue("FONT_PAGE_RASTERIZED",
                  "rasterized page " + std::to_string(i + 1) +
                      (keepPageText ? " (a font could not be embedded or substituted; the original text is "
                                  "kept as an invisible layer so it stays searchable)"
                                : " (a font could not be embedded or substituted, and its character map "
                                  "cannot be kept for an invisible text layer; text on this page is no longer selectable)"),
                  true);
      }
      if (!badAnnots.empty() && annots.isArray()) {
        for (auto it = badAnnots.rbegin(); it != badAnnots.rend(); ++it) {
          annots.eraseItem(*it);
        }
        ctx.issue("ANNOT_REMOVED",
                  "removed " + std::to_string(badAnnots.size()) +
                      " annotation(s) on page " + std::to_string(i + 1) +
                      " whose appearance uses a font that cannot be embedded",
                  true);
      }
    }
  }
}
}
