#pragma once

#include <qpdf/QPDFObjectHandle.hh>

#include <cstring>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_FONT_FORMATS_H
#include FT_TRUETYPE_IDS_H

#include <cstdint>
#include <string>
#include <vector>

#include "assets/agl_names.hh"
#include "encodings.hh"
#include "util.hh"

namespace pdfa {
struct FtLib {
  FT_Library lib = nullptr;
  FtLib() { FT_Init_FreeType(&lib); }
  ~FtLib() {
    if (lib) FT_Done_FreeType(lib);
  }
};

struct FtFace {
  FT_Face face = nullptr;
  std::string data;
  ~FtFace() {
    if (face) FT_Done_Face(face);
  }
};

inline QPDFObjectHandle fontFileStream(QPDFObjectHandle fd) {
  if (!fd.isDictionary()) return QPDFObjectHandle::newNull();
  for (const char* key : {"/FontFile2", "/FontFile3", "/FontFile"}) {
    if (fd.getKey(key).isStream()) return fd.getKey(key);
  }
  return QPDFObjectHandle::newNull();
}

inline bool loadFace(FtLib& lib, QPDFObjectHandle program, FtFace& out) {
  try {
    auto buf = program.getStreamData(qpdf_dl_all);
    out.data.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return false;
  }
  FT_Error err = FT_New_Memory_Face(
      lib.lib, reinterpret_cast<const FT_Byte*>(out.data.data()),
      static_cast<FT_Long>(out.data.size()), 0, &out.face);
  return err == 0 && out.face != nullptr;
}

inline bool parseUniName(const std::string& name, uint32_t& out) {
  if (name.rfind("uni", 0) == 0 && name.size() >= 7) {
    uint32_t v = 0;
    for (size_t i = 3; i < 7; ++i) {
      char c = name[i];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return false;
    }
    out = v;
    return true;
  }
  if (name.rfind("u", 0) == 0 && name.size() >= 5 && name.size() <= 7) {
    uint32_t v = 0;
    for (size_t i = 1; i < name.size(); ++i) {
      char c = name[i];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return false;
    }
    out = v;
    return true;
  }
  return false;
}

struct SimpleEncoding {
  uint16_t (*base)(int) = nullptr;
  std::vector<std::string> diffs = std::vector<std::string>(256);
};

inline SimpleEncoding readEncoding(QPDFObjectHandle font, bool symbolic) {
  SimpleEncoding enc;
  QPDFObjectHandle e = font.getKey("/Encoding");
  std::string baseName;
  if (e.isName()) {
    baseName = e.getName();
  } else if (e.isDictionary()) {
    baseName = nameOf(e.getKey("/BaseEncoding"));
    QPDFObjectHandle diffs = e.getKey("/Differences");
    if (diffs.isArray()) {
      int code = 0;
      for (int i = 0; i < diffs.getArrayNItems(); ++i) {
        QPDFObjectHandle item = diffs.getArrayItem(i);
        if (item.isInteger()) {
          code = static_cast<int>(item.getIntValue());
        } else if (item.isName() && code >= 0 && code < 256) {
          enc.diffs[code++] = item.getName();
        }
      }
    }
  }
  if (baseName == "/WinAnsiEncoding") enc.base = winAnsiToUnicode;
  else if (baseName == "/MacRomanEncoding") enc.base = macRomanToUnicode;
  else if (baseName == "/StandardEncoding") enc.base = standardToUnicode;
  else if (!symbolic && (e.isDictionary() || e.isName())) enc.base = standardToUnicode;
  return enc;
}

inline FT_UInt glyphForCode(FT_Face face, int code, const SimpleEncoding& enc, bool symbolic) {
  const std::string& diffName = enc.diffs[code];
  if (!diffName.empty()) {
    std::string bare = diffName.substr(1);
    FT_UInt gid = FT_Get_Name_Index(face, bare.c_str());
    if (gid) return gid;
    uint32_t uni = aglNameToUnicode(bare);
    if (uni && FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0) {
      gid = FT_Get_Char_Index(face, uni);
      if (gid) return gid;
    }
  }
  if (!symbolic && enc.base) {
    uint16_t uni = enc.base(code);
    if (uni && FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0) {
      FT_UInt gid = FT_Get_Char_Index(face, uni);
      if (gid) return gid;
    }
  }
  for (int i = 0; i < face->num_charmaps; ++i) {
    FT_CharMap cm = face->charmaps[i];
    if (cm->platform_id == TT_PLATFORM_MICROSOFT && cm->encoding_id == TT_MS_ID_SYMBOL_CS) {
      FT_Set_Charmap(face, cm);
      FT_UInt gid = FT_Get_Char_Index(face, 0xF000 | code);
      if (!gid) gid = FT_Get_Char_Index(face, code);
      if (gid) return gid;
    }
  }
  for (int i = 0; i < face->num_charmaps; ++i) {
    FT_CharMap cm = face->charmaps[i];
    if (cm->platform_id == TT_PLATFORM_MACINTOSH) {
      FT_Set_Charmap(face, cm);
      FT_UInt gid = FT_Get_Char_Index(face, code);
      if (gid) return gid;
    }
  }
  if (!symbolic && FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0) {
    uint16_t uni = winAnsiToUnicode(code);
    if (uni) {
      FT_UInt gid = FT_Get_Char_Index(face, uni);
      if (gid) return gid;
    }
  }
  return FT_Get_Char_Index(face, code);
}

inline int cffCustomEncodingGid(const std::string& file, int code) {
  const unsigned char* d = reinterpret_cast<const unsigned char*>(file.data());
  size_t n = file.size();
  size_t base = 0;
  if (n > 12 && std::memcmp(d, "OTTO", 4) == 0) {
    uint16_t numTables = static_cast<uint16_t>((d[4] << 8) | d[5]);
    bool found = false;
    for (uint16_t i = 0; i < numTables && 12 + 16 * (i + 1) <= n; ++i) {
      const unsigned char* rec = d + 12 + 16 * i;
      if (std::memcmp(rec, "CFF ", 4) == 0) {
        base = (static_cast<size_t>(rec[8]) << 24) | (rec[9] << 16) | (rec[10] << 8) |
               rec[11];
        found = true;
        break;
      }
    }
    if (!found) return -1;
  }
  if (base > n || n - base < 4 || d[base] != 1) return -1;
  size_t pos = base + d[base + 2];
  auto readIndex = [&](size_t p, size_t& first, size_t& firstLen, size_t& end) -> bool {
    if (p + 2 > n) return false;
    unsigned count = (d[p] << 8) | d[p + 1];
    if (count == 0) {
      end = p + 2;
      first = firstLen = 0;
      return true;
    }
    if (p + 3 > n) return false;
    unsigned offSize = d[p + 2];
    if (offSize < 1 || offSize > 4) return false;
    size_t offArr = p + 3;
    size_t dataStart = offArr + static_cast<size_t>(count + 1) * offSize - 1;
    if (dataStart >= n) return false;
    auto off = [&](unsigned i) -> size_t {
      size_t q = offArr + static_cast<size_t>(i) * offSize;
      size_t v = 0;
      for (unsigned b = 0; b < offSize; ++b) v = (v << 8) | d[q + b];
      return v;
    };
    size_t last = off(count);
    if (last > n - dataStart) return false;
    size_t o0 = off(0), o1 = off(1);
    if (o1 < o0 || o1 > last) return false;
    first = dataStart + o0;
    firstLen = o1 - o0;
    end = dataStart + last;
    return true;
  };
  size_t f1, l1, end;
  if (!readIndex(pos, f1, l1, end)) return -1;
  size_t tdFirst, tdLen, tdEnd;
  if (!readIndex(end, tdFirst, tdLen, tdEnd)) return -1;
  long long encOff = -1;
  {
    size_t i = tdFirst;
    size_t e = std::min(tdFirst + tdLen, n);
    long long stack[48];
    int sp = 0;
    while (i < e) {
      unsigned b = d[i];
      if (b <= 21) {
        unsigned op = b;
        if (b == 12) {
          if (i + 2 > e) break;
          op = 1200 + d[i + 1];
          i += 2;
        } else {
          i += 1;
        }
        if (op == 16 && sp > 0) encOff = stack[sp - 1];
        sp = 0;
      } else if (b == 28) {
        if (i + 3 > e) break;
        if (sp < 48) stack[sp++] = static_cast<int16_t>((d[i + 1] << 8) | d[i + 2]);
        i += 3;
      } else if (b == 29) {
        if (i + 5 > e) break;
        long long v = (static_cast<long long>(d[i + 1]) << 24) | (d[i + 2] << 16) |
                      (d[i + 3] << 8) | d[i + 4];
        if (sp < 48) stack[sp++] = static_cast<int32_t>(v);
        i += 5;
      } else if (b == 30) {
        i += 1;
        while (i < e) {
          unsigned nib = d[i];
          ++i;
          if ((nib & 0x0F) == 0x0F || (nib >> 4) == 0x0F) break;
        }
        if (sp < 48) stack[sp++] = 0;
      } else if (b >= 32 && b <= 246) {
        if (sp < 48) stack[sp++] = static_cast<int>(b) - 139;
        i += 1;
      } else if (b >= 247 && b <= 250) {
        if (i + 2 > e) break;
        if (sp < 48) stack[sp++] = (static_cast<int>(b) - 247) * 256 + d[i + 1] + 108;
        i += 2;
      } else if (b >= 251 && b <= 254) {
        if (i + 2 > e) break;
        if (sp < 48) stack[sp++] = -(static_cast<int>(b) - 251) * 256 - d[i + 1] - 108;
        i += 2;
      } else {
        i += 1;
      }
    }
  }
  if (encOff <= 1) return -1;
  size_t ep = base + static_cast<size_t>(encOff);
  if (ep + 2 > n) return -1;
  unsigned fmt = d[ep] & 0x7F;
  if (fmt == 0) {
    unsigned nCodes = d[ep + 1];
    if (ep + 2 + nCodes > n) return 0;
    for (unsigned i = 0; i < nCodes; ++i) {
      if (d[ep + 2 + i] == static_cast<unsigned>(code)) return i + 1;
    }
    return 0;
  }
  if (fmt == 1) {
    unsigned nRanges = d[ep + 1];
    if (ep + 2 + nRanges * 2 > n) return 0;
    unsigned gid = 1;
    for (unsigned r = 0; r < nRanges; ++r) {
      unsigned first = d[ep + 2 + r * 2];
      unsigned nLeft = d[ep + 3 + r * 2];
      if (static_cast<unsigned>(code) >= first &&
          static_cast<unsigned>(code) <= first + nLeft) {
        return gid + (static_cast<unsigned>(code) - first);
      }
      gid += nLeft + 1;
    }
  }
  return 0;
}

inline FT_UInt resolveSimpleGid(const FtFace& face, int code, const SimpleEncoding& enc,
                                bool symbolic) {
  if (symbolic && enc.diffs[code].empty()) {
    const char* fmt = FT_Get_Font_Format(face.face);
    if (fmt && std::strcmp(fmt, "CFF") == 0) {
      int g = cffCustomEncodingGid(face.data, code);
      if (g >= 0) {
        return g > 0 && g < static_cast<int>(face.face->num_glyphs)
                   ? static_cast<FT_UInt>(g)
                   : 0;
      }
    } else if (fmt && std::strcmp(fmt, "Type 1") == 0) {
      FT_CharMap saved = face.face->charmap;
      for (int i = 0; i < face.face->num_charmaps; ++i) {
        FT_CharMap cm = face.face->charmaps[i];
        if (cm->encoding == FT_ENCODING_ADOBE_CUSTOM ||
            cm->encoding == FT_ENCODING_ADOBE_STANDARD) {
          FT_Set_Charmap(face.face, cm);
          FT_UInt gid = FT_Get_Char_Index(face.face, static_cast<FT_ULong>(code));
          if (saved) FT_Set_Charmap(face.face, saved);
          return gid;
        }
      }
    }
  }
  return glyphForCode(face.face, code, enc, symbolic);
}
}
