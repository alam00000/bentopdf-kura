#pragma once

#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>

namespace pdfa {
inline bool validUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    int len;
    uint32_t cp;
    if (c < 0x80) { len = 1; cp = c; }
    else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else return false;
    if (i + len > s.size()) return false;
    for (int j = 1; j < len; ++j) {
      unsigned char cc = static_cast<unsigned char>(s[i + j]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (len == 2 && cp < 0x80) return false;
    if (len == 3 && cp < 0x800) return false;
    if (len == 4 && cp < 0x10000) return false;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += len;
  }
  return true;
}

inline bool isPuaCodepoint(uint32_t cp) {
  return (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) ||
         (cp >= 0x100000 && cp <= 0x10FFFD);
}

inline std::string stripPuaUtf8(const std::string& utf8, bool& changed) {
  std::string out;
  size_t i = 0;
  while (i < utf8.size()) {
    unsigned char c = static_cast<unsigned char>(utf8[i]);
    int len = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
    if (i + len > utf8.size()) break;
    uint32_t cp = c < 0x80 ? c : (c & (0xFF >> (len + 1)));
    for (int j = 1; j < len; ++j) {
      cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + j]) & 0x3F);
    }
    if (isPuaCodepoint(cp)) {
      changed = true;
    } else {
      out.append(utf8, i, len);
    }
    i += len;
  }
  return out;
}

inline bool nameIs(QPDFObjectHandle o, const std::string& n) {
  return o.isName() && o.getName() == n;
}

inline std::string nameOf(QPDFObjectHandle o) {
  return o.isName() ? o.getName() : std::string();
}

inline double numOf(QPDFObjectHandle o, double dflt) {
  return o.isNumber() ? o.getNumericValue() : dflt;
}

struct Visited {
  std::set<QPDFObjGen> seen;
  int depth = 0;
  bool enter(QPDFObjectHandle o) {
    if (!o.isIndirect()) return true;
    return seen.insert(o.getObjGen()).second;
  }
};

constexpr int kMaxNest = 200;

struct DepthGuard {
  Visited& v;
  bool over;
  explicit DepthGuard(Visited& visited) : v(visited), over(visited.depth >= kMaxNest) {
    ++v.depth;
  }
  ~DepthGuard() { --v.depth; }
};

constexpr double kPdfRealLimit = 32767.0;

inline std::string fmtFixed(double v, int decimals) {
  if (!std::isfinite(v)) return "0";
  if (v > kPdfRealLimit) v = kPdfRealLimit;
  if (v < -kPdfRealLimit) v = -kPdfRealLimit;
  if (decimals < 0) decimals = 0;
  if (decimals > 6) decimals = 6;
  bool neg = v < 0;
  if (neg) v = -v;
  long long scale = 1;
  for (int i = 0; i < decimals; ++i) scale *= 10;
  long long scaled = static_cast<long long>(v * static_cast<double>(scale) + 0.5);
  long long whole = scaled / scale;
  long long frac = scaled % scale;
  std::string out;
  if (neg && (whole || frac)) out += '-';
  out += std::to_string(whole);
  if (decimals > 0 && frac) {
    std::string f = std::to_string(frac);
    f.insert(f.begin(), static_cast<size_t>(decimals) - f.size(), '0');
    while (!f.empty() && f.back() == '0') f.pop_back();
    if (!f.empty()) {
      out += '.';
      out += f;
    }
  }
  return out;
}
}
