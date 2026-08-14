#pragma once

#include <qpdf/QPDFObjectHandle.hh>

#include <ft2build.h>
#include FT_FREETYPE_H
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
    if (cm->encoding == FT_ENCODING_ADOBE_CUSTOM ||
        cm->encoding == FT_ENCODING_ADOBE_STANDARD) {
      FT_Set_Charmap(face, cm);
      FT_UInt gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(code));
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
}
