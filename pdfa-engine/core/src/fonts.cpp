#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
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
#include "ctx.hh"
#include "encodings.hh"
#include "fonts_ft.hh"
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
  explicit CffWidths(const std::string& file) {
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
      }
      parseFdSelect(fdsOff);
    } else {
      fdPriv.push_back(readPrivate(topDict, base));
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
    double defaultW = fdPriv[fd].first, nominalW = fdPriv[fd].second;
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
        default: return defaultW;
      }
      bool hasWidth;
      if (expected == -2) hasWidth = (stack % 2) == 1;
      else if (b == 14) hasWidth = stack == 1 || stack == 5;
      else hasWidth = stack == expected + 1;
      return hasWidth ? nominalW + first : defaultW;
    }
    return defaultW;
  }

 private:
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

void syncSimpleFontWidths(Ctx& ctx, FtLib& lib, QPDFObjectHandle font) {
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  QPDFObjectHandle program = fontFileStream(fd);
  if (!program.isStream()) return;
  QPDFObjectHandle first = font.getKey("/FirstChar");
  QPDFObjectHandle last = font.getKey("/LastChar");
  QPDFObjectHandle widths = font.getKey("/Widths");
  if (!first.isInteger() || !last.isInteger() || !widths.isArray()) return;
  FtFace face;
  if (!loadFace(lib, program, face)) {
    ctx.issue("FONT_PROGRAM_UNREADABLE",
              "embedded font program unreadable: " + nameOf(font.getKey("/BaseFont")), false);
    return;
  }
  double upem = face.face->units_per_EM ? face.face->units_per_EM : 1000.0;
  HmtxTable hmtx = parseHmtx(face.data);
  long long flags = fd.getKey("/Flags").isInteger() ? fd.getKey("/Flags").getIntValue() : 0;
  bool symbolic = (flags & 4) != 0 && (flags & 32) == 0;
  SimpleEncoding enc = readEncoding(font, symbolic);
  int firstChar = static_cast<int>(first.getIntValue());
  int lastChar = static_cast<int>(last.getIntValue());
  int n = widths.getArrayNItems();
  int changed = 0;
  bool extended = false;
  std::vector<QPDFObjectHandle> full(256, QPDFObjectHandle::newInteger(0));
  for (int code = 0; code < 256; ++code) {
    bool inRange = code >= firstChar && code <= lastChar && code - firstChar < n;
    QPDFObjectHandle w =
        inRange ? widths.getArrayItem(code - firstChar) : QPDFObjectHandle::newNull();
    double dictW = w.isNumber() ? w.getNumericValue() : 0.0;
    FT_UInt gid = glyphForCode(face.face, code, enc, symbolic);
    double progW = programAdvance(face, hmtx, gid, upem);
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
  if (changed || extended) {
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

void syncCidFontWidths(Ctx& ctx, FtLib& lib, QPDFObjectHandle type0) {
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
  CffWidths cff(face.data);

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
        long long end = static_cast<long long>(b.getNumericValue());
        double val = oldW.getArrayItem(idx + 2).getNumericValue();
        for (long long cid = start; cid <= end; ++cid) {
          if (cid >= 0 && static_cast<size_t>(cid) < numCids) declared[cid] = val;
        }
        idx += 3;
      } else {
        break;
      }
    }
  }
  std::vector<long long> widths(numCids, 0);
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
  bool had = fd.getKey("/CIDSet").isStream();
  fd.replaceKey("/CIDSet", ctx.pdf.makeIndirectObject(cidSet));
  ctx.issue("CIDSET_ADDED",
            std::string(had ? "regenerated" : "synthesized") + " /CIDSet for subset " +
                nameOf(cidFont.getKey("/BaseFont")),
            true);
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
  bool had = fd.getKey("/CharSet").isString();
  fd.replaceKey("/CharSet", QPDFObjectHandle::newString(charset));
  ctx.issue("CHARSET_ADDED",
            std::string(had ? "regenerated" : "synthesized") + " /CharSet for subset " +
                nameOf(font.getKey("/BaseFont")),
            true);
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
  size_t pos = 0;
  bool inBf = false;
  while (pos < text.size()) {
    if (text.compare(pos, 11, "beginbfchar") == 0 ||
        text.compare(pos, 12, "beginbfrange") == 0) {
      inBf = true;
      pos += 11;
      continue;
    }
    if (text.compare(pos, 9, "endbfchar") == 0 || text.compare(pos, 10, "endbfrange") == 0) {
      inBf = false;
      pos += 9;
      continue;
    }
    if (inBf && text[pos] == '<') {
      size_t end = text.find('>', pos);
      if (end != std::string::npos && end - pos - 1 >= 4) {
        std::string hex = text.substr(pos + 1, end - pos - 1);
        bool bad = false;
        for (size_t i = 0; i + 4 <= hex.size(); i += 4) {
          std::string quad = hex.substr(i, 4);
          unsigned v = static_cast<unsigned>(std::strtoul(quad.c_str(), nullptr, 16));
          if (v == 0 || v == 0xFEFF || v == 0xFFFE) {
            hex.replace(i, 4, "FFFD");
            bad = true;
          } else if (puaBanned && v >= 0xE000 && v <= 0xF8FF) {
            uint16_t win = (v & 0xFF00) == 0xF000
                               ? winAnsiToUnicode(static_cast<int>(v & 0xFF))
                               : 0;
            char rep[8];
            std::snprintf(rep, sizeof(rep), "%04X", win ? win : 0xFFFD);
            hex.replace(i, 4, rep);
            bad = true;
          }
        }
        if (bad) {
          text.replace(pos + 1, end - pos - 1, hex);
          changed = true;
        }
        pos = end + 1;
        continue;
      }
    }
    ++pos;
  }
  if (changed) {
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
  if (!(flags & 32) || (flags & 4)) return;
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
        uint32_t uni = aglNameToUnicode(bare);
        uint32_t parsed = 0;
        bool ok = uni != 0 || parseUniName(bare, parsed) || bare == ".notdef";
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
                  "removed non-AGL glyph names from /Differences of " +
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
  if (!reg.empty() && !ord.empty()) {
    QPDFObjectHandle cd = enc.getDict().getKey("/CIDSystemInfo");
    std::string creg = cd.isDictionary() && cd.getKey("/Registry").isString()
                           ? cd.getKey("/Registry").getStringValue()
                           : "";
    std::string cord = cd.isDictionary() && cd.getKey("/Ordering").isString()
                           ? cd.getKey("/Ordering").getStringValue()
                           : "";
    if (creg != reg || cord != ord) {
      QPDFObjectHandle nr = QPDFObjectHandle::newDictionary();
      nr.replaceKey("/Registry", QPDFObjectHandle::newString(reg));
      nr.replaceKey("/Ordering", QPDFObjectHandle::newString(ord));
      nr.replaceKey("/Supplement", QPDFObjectHandle::newInteger(rosSupp));
      enc.getDict().replaceKey("/CIDSystemInfo", nr);
      ctx.issue("CMAP_ROS_SYNCED",
                "aligned embedded CMap /CIDSystemInfo dictionary with the CIDFont", true);
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
  if (!reg.empty()) patchLiteral("Registry", reg);
  if (!ord.empty()) patchLiteral("Ordering", ord);
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
      }
      if (!standardRef) {
        text.erase(lineStart, uc + 7 - lineStart);
        changed = true;
        if (enc.getDict().hasKey("/UseCMap")) enc.getDict().removeKey("/UseCMap");
      }
    }
    size_t wm = text.find("/WMode");
    if (wm != std::string::npos) {
      int v = std::atoi(text.c_str() + wm + 6);
      QPDFObjectHandle cur = enc.getDict().getKey("/WMode");
      if ((v == 0 || v == 1) && (!cur.isInteger() || cur.getIntValue() != v)) {
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

void syncType3Widths(Ctx& ctx, QPDFObjectHandle font) {
  QPDFObjectHandle cp = font.getKey("/CharProcs");
  QPDFObjectHandle widths = font.getKey("/Widths");
  QPDFObjectHandle fm = font.getKey("/FontMatrix");
  if (!cp.isDictionary() || !widths.isArray()) return;
  double a = fm.isArray() && fm.getArrayNItems() == 6 ? numOf(fm.getArrayItem(0), 0.001)
                                                      : 0.001;
  if (a == 0) a = 0.001;
  (void)a;
  int first = font.getKey("/FirstChar").isInteger()
                  ? static_cast<int>(font.getKey("/FirstChar").getIntValue())
                  : 0;
  SimpleEncoding enc = readEncoding(font, false);
  bool changed = false;
  for (int idx = 0; idx < widths.getArrayNItems(); ++idx) {
    int code = first + idx;
    if (code < 0 || code > 255) break;
    const std::string& gname = enc.diffs[code];
    if (gname.empty()) continue;
    QPDFObjectHandle glyph = cp.getKey(gname);
    if (!glyph.isStream()) continue;
    std::string data;
    try {
      auto buf = glyph.getStreamData(qpdf_dl_all);
      data.assign(reinterpret_cast<const char*>(buf->getBuffer()),
                  std::min<size_t>(buf->getSize(), 512));
    } catch (...) {
      continue;
    }
    std::vector<double> nums;
    size_t i = 0;
    bool found = false;
    double wx = 0;
    while (i < data.size() && nums.size() < 8) {
      while (i < data.size() && std::isspace(static_cast<unsigned char>(data[i]))) ++i;
      size_t j = i;
      while (j < data.size() && !std::isspace(static_cast<unsigned char>(data[j]))) ++j;
      if (j == i) break;
      std::string tok = data.substr(i, j - i);
      if (tok == "d0" || tok == "d1") {
        if (!nums.empty()) {
          wx = nums[0];
          found = true;
        }
        break;
      }
      char* end = nullptr;
      double v = std::strtod(tok.c_str(), &end);
      if (end && *end == '\0') {
        nums.push_back(v);
      } else {
        break;
      }
      i = j;
    }
    if (!found) continue;

    double w = wx;
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

int symbolicCmapAction(FtLib& lib, QPDFObjectHandle font) {
  QPDFObjectHandle fd = font.getKey("/FontDescriptor");
  if (!fd.isDictionary() || !fd.getKey("/Flags").isInteger()) return 0;
  long long flags = fd.getKey("/Flags").getIntValue();
  if (!(flags & 4) || (flags & 32)) return 0;
  QPDFObjectHandle program = fontFileStream(fd);
  if (!program.isStream()) return 0;
  FtFace face;
  if (!loadFace(lib, program, face)) return 0;
  int n = face.face->num_charmaps;
  if (n == 1) return 0;
  for (int i = 0; i < n; ++i) {
    FT_CharMap cm = face.face->charmaps[i];
    if (cm->platform_id == 3 && cm->encoding_id == 1) return 1;
  }
  return 2;
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
      if (src.size() <= 8 && !dst.empty()) {
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
  bool symbolic = (flags & 4) != 0 && (flags & 32) == 0;
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

  int added = 0;
  for (size_t cid = 0; cid < numCids; ++cid) {
    auto it = entries.find(static_cast<uint32_t>(cid));
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
    } else if (cid >= 0x20 && cid <= 0x10FFFF && !(cid >= 0xD800 && cid <= 0xDFFF)) {
      uni = static_cast<uint32_t>(cid);
    } else {
      uni = 0xFFFD;
    }
    entries[static_cast<uint32_t>(cid)] = utf16Hex(uni);
    ++added;
  }
  if (!added) return;
  QPDFObjectHandle stream = QPDFObjectHandle::newStream(&ctx.pdf, buildCMapStream(entries, true));
  type0.replaceKey("/ToUnicode", ctx.pdf.makeIndirectObject(stream));
  ctx.issue("TOUNICODE_BUILT",
            "completed ToUnicode mapping for " + nameOf(type0.getKey("/BaseFont")), true);
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

QPDFObjectHandle buildDescriptor(Ctx& ctx, const SubstituteChoice& c, FT_Face face,
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

void buildSymbolDifferences(Ctx& ctx, QPDFObjectHandle font, uint16_t (*table)(int)) {
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

void rebuildSimpleWidths(Ctx& ctx, QPDFObjectHandle font, const FtFace& sub) {
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
  auto face = assetFace(lib, cache, c.asset);
  if (!face->face) return false;
  std::string original = nameOf(font.getKey("/BaseFont"));

  QPDFObjectHandle fd = buildDescriptor(ctx, c, face->face, c.asset->psName);
  fd.replaceKey("/FontFile3", assetStream(ctx, cache, c.asset, false));
  font.replaceKey("/FontDescriptor", ctx.pdf.makeIndirectObject(fd));
  font.replaceKey("/Subtype", QPDFObjectHandle::newName("/Type1"));
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
  rebuildSimpleWidths(ctx, font, *face);
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
  c.asset = findAsset(std::string(c.asset->key) + ":ttf");
  if (!c.asset) return false;
  auto face = assetFace(lib, cache, c.asset);
  if (!face->face) return false;
  std::string original = nameOf(type0.getKey("/BaseFont"));

  std::map<uint32_t, std::string> toUni = parseExistingToUnicode(type0.getKey("/ToUnicode"));
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
    } else if (cid >= 0x20 && !(cid >= 0xD800 && cid <= 0xDFFF)) {
      uni = cid;
    }
    FT_UInt gid = (uni && haveUnicodeCmap) ? FT_Get_Char_Index(face->face, uni) : 0;
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

bool validUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = s[i];
    int extra = 0;
    if (c < 0x80) extra = 0;
    else if ((c & 0xE0) == 0xC0) extra = 1;
    else if ((c & 0xF0) == 0xE0) extra = 2;
    else if ((c & 0xF8) == 0xF0) extra = 3;
    else return false;
    for (int j = 1; j <= extra; ++j) {
      if (i + j >= s.size() || (static_cast<unsigned char>(s[i + j]) & 0xC0) != 0x80) {
        return false;
      }
    }
    i += extra + 1;
  }
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

int renameIfInvalid(Ctx& ctx, QPDFObjectHandle dict, const char* key) {
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
                  std::vector<QPDFObjectHandle>& fonts) {
  DepthGuard g_(visited);
  if (g_.over) return;
  if (!res.isDictionary() || !visited.enter(res)) return;
  QPDFObjectHandle fd = res.getKey("/Font");
  if (fd.isDictionary()) {
    for (const std::string& k : fd.getKeys()) {
      QPDFObjectHandle f = fd.getKey(k);
      if (!f.isDictionary()) continue;
      if (f.isIndirect() && !visited.enter(f)) continue;
      fonts.push_back(f);
      if (nameIs(f.getKey("/Subtype"), "/Type3")) {
        collectFonts(ctx, f.getKey("/Resources"), visited, fonts);
      }
    }
  }
  QPDFObjectHandle xod = res.getKey("/XObject");
  if (xod.isDictionary()) {
    for (const std::string& k : xod.getKeys()) {
      QPDFObjectHandle xo = xod.getKey(k);
      if (xo.isStream() && visited.enter(xo) &&
          nameIs(xo.getDict().getKey("/Subtype"), "/Form")) {
        collectFonts(ctx, xo.getDict().getKey("/Resources"), visited, fonts);
      }
    }
  }
  QPDFObjectHandle pat = res.getKey("/Pattern");
  if (pat.isDictionary()) {
    for (const std::string& k : pat.getKeys()) {
      QPDFObjectHandle p = pat.getKey(k);
      if (p.isStream() && visited.enter(p)) {
        collectFonts(ctx, p.getDict().getKey("/Resources"), visited, fonts);
      }
    }
  }
}
}

void passFonts(Ctx& ctx) {
  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFObjectHandle> fonts;
  Visited visited;
  for (auto& ph : dh.getAllPages()) {
    QPDFObjectHandle page = ph.getObjectHandle();
    collectFonts(ctx, page.getKey("/Resources"), visited, fonts);
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
          collectFonts(ctx, s.getDict().getKey("/Resources"), visited, fonts);
        }
      }
    }
  }

  FtLib lib;
  EmbedCache cache;
  std::set<std::string> reported;
  std::set<QPDFObjGen> unfixable;
  for (QPDFObjectHandle f : fonts) {
    sanitizeFontNames(ctx, f);
    std::string base = nameOf(f.getKey("/BaseFont"));
    std::string subtype = nameOf(f.getKey("/Subtype"));
    bool embedded = fontEmbedded(f);
    bool corrupt = embedded && subtype != "/Type3" && !programUsable(lib, f, subtype);
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
    if (!embedded && subtype != "/Type3") {
      bool ok = false;
      if (subtype == "/Type0") {
        ok = embedCidSubstitute(ctx, lib, cache, f);
      } else {
        ok = embedSimpleSubstitute(ctx, lib, cache, f);
      }
      if (!ok) {
        if (f.isIndirect()) unfixable.insert(f.getObjGen());
        if (reported.insert(base).second) {
          ctx.issue("FONT_NOT_EMBEDDED",
                    "font " + base + " is not embedded and no substitute applies", false);
        }
        continue;
      }
      subtype = nameOf(f.getKey("/Subtype"));
    }
    bool needUnicode = ctx.needUnicode();
    if (subtype == "/TrueType" || subtype == "/Type1" || subtype == "/MMType1" ||
        subtype == "/Type3") {
      if (needUnicode) ensureSimpleToUnicode(ctx, lib, f);
    }
    if (subtype == "/Type0" && needUnicode) ensureCidToUnicode(ctx, lib, f);
    scrubToUnicode(ctx, f);
    if (subtype == "/TrueType" || subtype == "/Type1" || subtype == "/MMType1") {
      if (subtype == "/TrueType" && ctx.isA()) {
        int act = symbolicCmapAction(lib, f);
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
      if (subtype == "/TrueType") {
        fixNonsymbolicTtfEncoding(ctx, f);
        QPDFObjectHandle fd = f.getKey("/FontDescriptor");
        long long flags = fd.isDictionary() && fd.getKey("/Flags").isInteger()
                              ? fd.getKey("/Flags").getIntValue()
                              : 0;
        if ((flags & 4) && !(flags & 32) && f.hasKey("/Encoding")) {
          f.removeKey("/Encoding");
          ctx.issue("SYMBOLIC_ENCODING_REMOVED",
                    "removed /Encoding from symbolic TrueType font " + base, true);
        }
        if (ctx.isA() && ctx.part == 1) {
          QPDFObjectHandle e2 = f.getKey("/Encoding");
          if (e2.isDictionary() && e2.hasKey("/Differences")) {
            std::string be = nameOf(e2.getKey("/BaseEncoding"));
            f.replaceKey("/Encoding",
                         QPDFObjectHandle::newName(be == "/MacRomanEncoding"
                                                       ? "/MacRomanEncoding"
                                                       : "/WinAnsiEncoding"));
            ctx.issue("TT_DIFFERENCES_REMOVED",
                      "removed /Differences from TrueType font " + base +
                          " (PDF/A-1 requires plain MacRoman/WinAnsi encoding; visual "
                          "difference possible)",
                      true);
          }
        }
      }
      syncSimpleFontWidths(ctx, lib, f);
      if (ctx.isA() && ctx.part == 1) {
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
      syncType3Widths(ctx, f);
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
      ensureCidToGidMap(ctx, f);
      syncCidFontWidths(ctx, lib, f);
      if (ctx.isA() && ctx.part == 1) {
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
          QPDFObjectHandle ap = a.getKey("/AP");
          if (!ap.isDictionary()) continue;
          QPDFObjectHandle nap = ap.getKey("/N");
          std::vector<QPDFObjectHandle> streams;
          if (nap.isStream()) streams.push_back(nap);
          else if (nap.isDictionary()) {
            for (const std::string& k : nap.getKeys()) {
              if (nap.getKey(k).isStream()) streams.push_back(nap.getKey(k));
            }
          }
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
        if (!ctx.opt.rasterizePage || !rasterFlattenPage(ctx, pages[i], static_cast<int>(i))) {
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
        }
        ctx.issue("FONT_PAGE_RASTERIZED",
                  "rasterized page " + std::to_string(i + 1) +
                      " (a font could not be embedded or substituted; text on this page "
                      "is no longer selectable)",
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
