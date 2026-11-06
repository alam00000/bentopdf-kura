#include <lcms2.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFCryptoProvider.hh>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>

#include "fonts_ft.hh"
#include FT_CID_H
#include FT_TRUETYPE_TABLES_H
#include "images.hh"
#include "profile_eval.hh"
#include "profile_ext.hh"
#include "util.hh"

namespace pdfa {
namespace {
std::string str(QPDFObjectHandle o) {
  if (o.isString()) return o.getUTF8Value();
  if (o.isName()) return o.getName().substr(1);
  return std::string();
}

QPDFObjectHandle fieldRoot(QPDFObjectHandle w) {
  QPDFObjectHandle f = w;
  for (int i = 0; i < 32 && f.isDictionary(); ++i) {
    if (f.hasKey("/FT") || !f.getKey("/Parent").isDictionary()) return f;
    f = f.getKey("/Parent");
  }
  return w;
}

QPDFObjectHandle inheritedField(QPDFObjectHandle w, const std::string& key) {
  QPDFObjectHandle f = w;
  for (int i = 0; i < 32 && f.isDictionary(); ++i) {
    if (f.hasKey(key)) return f.getKey(key);
    f = f.getKey("/Parent");
  }
  return QPDFObjectHandle::newNull();
}

std::string fullFieldName(QPDFObjectHandle w) {
  std::string n;
  QPDFObjectHandle f = w;
  for (int i = 0; i < 32 && f.isDictionary(); ++i) {
    if (f.getKey("/T").isString()) n = f.getKey("/T").getUTF8Value() + (n.empty() ? "" : "." + n);
    f = f.getKey("/Parent");
  }
  return n;
}

std::string xmpOf(QPDFObjectHandle root) {
  QPDFObjectHandle md = root.isDictionary() ? root.getKey("/Metadata") : QPDFObjectHandle();
  if (!md.isStream()) return std::string();
  try {
    auto b = md.getStreamData(qpdf_dl_all);
    return std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
  } catch (...) {
    return std::string();
  }
}

std::string xmpValue(const std::string& x, const std::string& tag) {
  size_t p = x.find("<" + tag);
  if (p != std::string::npos) {
    size_t gt = x.find('>', p);
    size_t end = x.find("</" + tag, gt);
    if (gt != std::string::npos && end != std::string::npos) {
      std::string v = x.substr(gt + 1, end - gt - 1);
      size_t li = v.find("<rdf:li");
      if (li != std::string::npos) {
        size_t g2 = v.find('>', li), e2 = v.find("</rdf:li", g2);
        if (g2 != std::string::npos && e2 != std::string::npos) v = v.substr(g2 + 1, e2 - g2 - 1);
      }
      return v;
    }
  }
  p = x.find(tag + "=\"");
  if (p != std::string::npos) {
    size_t s = p + tag.size() + 2, e = x.find('"', s);
    if (e != std::string::npos) return x.substr(s, e - s);
  }
  return std::string();
}

std::vector<QPDFObjectHandle> layers(QPDFObjectHandle root) {
  std::vector<QPDFObjectHandle> out;
  QPDFObjectHandle ocp = root.getKey("/OCProperties");
  QPDFObjectHandle ocgs = ocp.isDictionary() ? ocp.getKey("/OCGs") : QPDFObjectHandle();
  for (int i = 0; ocgs.isArray() && i < ocgs.getArrayNItems(); ++i) {
    if (ocgs.getArrayItem(i).isDictionary()) out.push_back(ocgs.getArrayItem(i));
  }
  return out;
}

bool inArray(QPDFObjectHandle arr, QPDFObjectHandle o) {
  for (int i = 0; arr.isArray() && i < arr.getArrayNItems(); ++i) {
    QPDFObjectHandle it = arr.getArrayItem(i);
    if (it.isIndirect() && o.isIndirect() && it.getObjGen() == o.getObjGen()) return true;
    if (it.isArray() && inArray(it, o)) return true;
  }
  return false;
}

std::string streamBytes(QPDFObjectHandle st) {
  if (!st.isStream()) return std::string();
  try {
    auto b = st.getStreamData(qpdf_dl_all);
    return std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
  } catch (...) {
    return std::string();
  }
}

std::vector<QPDFObjectHandle> outputIntents(QPDFObjectHandle root, const std::string& subtype) {
  std::vector<QPDFObjectHandle> out;
  QPDFObjectHandle ois = root.getKey("/OutputIntents");
  for (int i = 0; ois.isArray() && i < ois.getArrayNItems(); ++i) {
    QPDFObjectHandle oi = ois.getArrayItem(i);
    if (oi.isDictionary() && (subtype.empty() || nameIs(oi.getKey("/S"), subtype))) out.push_back(oi);
  }
  return out;
}

bool isTagObject(QPDFObjectHandle o) {
  if (o.isArray()) return true;
  if (!o.isDictionary()) return false;
  static const std::set<std::string> types = {"/StructElem", "/StructTreeRoot", "/MCR", "/OBJR"};
  if (types.count(nameOf(o.getKey("/Type")))) return true;
  if (o.hasKey("/S") && o.hasKey("/P")) return true;
  return !o.hasKey("/Type") && (o.hasKey("/Nums") || o.hasKey("/Limits"));
}

const Events::ObjStats& objStats(const Events& ev) {
  Events::ObjStats& st = ev.objStats;
  if (st.done) return st;
  st.done = true;
  QPDF* q = ev.root.isInitialized() ? ev.root.getOwningQPDF() : nullptr;
  if (!q) return st;
  std::map<QPDFObjGen, QPDFXRefEntry> xref = q->getXRefTable();
  std::function<void(QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle o, int depth) {
    if (depth > 64) return;
    if (o.isInteger()) {
      long long v = o.getIntValue();
      if (v < 0 && (!st.anyNegInt || v < st.negInt)) {
        st.negInt = static_cast<double>(v);
        st.anyNegInt = true;
      }
      if (v > 0 && (!st.anyPosInt || v > st.posInt)) {
        st.posInt = static_cast<double>(v);
        st.anyPosInt = true;
      }
    } else if (o.isReal()) {
      double v = o.getNumericValue();
      if (v < 0) {
        if (!st.anyNegReal || v < st.negReal) st.negReal = v;
        if (!st.anyNegReal || v > st.negRealNearZero) st.negRealNearZero = v;
        st.anyNegReal = true;
      } else if (v > 0) {
        if (!st.anyPosReal || v > st.posReal) st.posReal = v;
        if (!st.anyPosReal || v < st.posRealNearZero) st.posRealNearZero = v;
        st.anyPosReal = true;
      }
    } else if (o.isName()) {
      int n = static_cast<int>(o.getName().size()) - 1;
      st.maxName = std::max(st.maxName, n);
      st.nameLens.insert(n);
    } else if (o.isString()) {
      st.maxStr = std::max(st.maxStr, o.getStringValue().size());
    } else if (o.isArray()) {
      st.maxArr = std::max(st.maxArr, o.getArrayNItems());
      for (int i = 0; i < o.getArrayNItems(); ++i) {
        QPDFObjectHandle it = o.getArrayItem(i);
        if (it.isIndirect()) {
          if (!xref.count(it.getObjGen())) st.refMissing = true;
        } else {
          walk(it, depth + 1);
        }
      }
    } else if (o.isDictionary()) {
      std::set<std::string> keys = o.getKeys();
      st.maxDict = std::max(st.maxDict, static_cast<int>(keys.size()));
      for (const std::string& k : keys) {
        int n = static_cast<int>(k.size()) - 1;
        st.maxName = std::max(st.maxName, n);
        st.nameLens.insert(n);
        QPDFObjectHandle v = o.getKey(k);
        if (v.isIndirect()) {
          if (!xref.count(v.getObjGen())) st.refMissing = true;
        } else {
          walk(v, depth + 1);
        }
      }
    }
  };
  int count = 0;
  for (QPDFObjectHandle o : q->getAllObjects()) {
    if (++count > 400000) break;
    if (o.isStream()) {
      QPDFObjectHandle d = o.getDict();
      walk(d, 0);
      st.maxStream = std::max(st.maxStream, static_cast<size_t>(std::max(0.0, numOf(d.getKey("/Length"), 0))));
      QPDFObjectHandle fl = d.getKey("/Filter");
      QPDFObjectHandle dp = d.getKey("/DecodeParms");
      std::vector<QPDFObjectHandle> fs, ps;
      if (fl.isName()) {
        fs.push_back(fl);
        ps.push_back(dp);
      }
      for (int i = 0; fl.isArray() && i < fl.getArrayNItems(); ++i) {
        fs.push_back(fl.getArrayItem(i));
        ps.push_back(dp.isArray() && i < dp.getArrayNItems() ? dp.getArrayItem(i) : QPDFObjectHandle());
      }
      for (size_t i = 0; i < fs.size(); ++i) {
        std::string n = nameOf(fs[i]);
        if (n.size() > 1) st.filters.insert(n.substr(1));
        if (n == "/Crypt") {
          st.cryptFilter = true;
          if (ps[i].isDictionary() && ps[i].getKey("/Name").isName()) st.cryptNames.insert(ps[i].getKey("/Name").getName().substr(1));
        }
      }
      if (d.getKey("/DL").isInteger()) {
        try {
          auto b = o.getStreamData(qpdf_dl_generalized);
          if (static_cast<long long>(b->getSize()) != d.getKey("/DL").getIntValue()) st.dlBad = true;
        } catch (...) {
        }
      }
    } else {
      walk(o, 0);
    }
    if (o.isIndirect()) {
      auto it = xref.find(o.getObjGen());
      if (it != xref.end() && it->second.getType() == 2 && !isTagObject(o)) st.compressedNonTag = true;
    }
  }
  return st;
}

QPDFObjectHandle resolveDest(QPDFObjectHandle root, QPDFObjectHandle d) {
  if (d.isArray()) return d;
  std::string key = d.isName() ? d.getName().substr(1) : d.isString() ? d.getUTF8Value() : std::string();
  if (key.empty()) return QPDFObjectHandle();
  QPDFObjectHandle out;
  QPDFObjectHandle dests = root.getKey("/Dests");
  if (d.isName() && dests.isDictionary()) out = dests.getKey("/" + key);
  QPDFObjectHandle names = root.getKey("/Names");
  std::function<void(QPDFObjectHandle, int)> nt = [&](QPDFObjectHandle n, int depth) {
    if (out.isInitialized() || !n.isDictionary() || depth > 32) return;
    QPDFObjectHandle arr = n.getKey("/Names");
    for (int i = 0; arr.isArray() && i + 1 < arr.getArrayNItems(); i += 2) {
      if (arr.getArrayItem(i).isString() && arr.getArrayItem(i).getUTF8Value() == key) {
        out = arr.getArrayItem(i + 1);
        return;
      }
    }
    QPDFObjectHandle kids = n.getKey("/Kids");
    for (int i = 0; kids.isArray() && i < kids.getArrayNItems(); ++i) nt(kids.getArrayItem(i), depth + 1);
  };
  if (!out.isInitialized() && names.isDictionary()) nt(names.getKey("/Dests"), 0);
  if (out.isDictionary()) out = out.getKey("/D");
  return out.isArray() ? out : QPDFObjectHandle();
}

std::map<int, int> jbig2Segments(const std::string& d) {
  std::map<int, int> out;
  size_t p = 0;
  auto u32 = [&](size_t o) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(d[o])) << 24) | (static_cast<uint32_t>(static_cast<unsigned char>(d[o + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d[o + 2])) << 8) | static_cast<unsigned char>(d[o + 3]);
  };
  for (int guard = 0; guard < 100000 && p + 6 <= d.size(); ++guard) {
    uint32_t num = u32(p);
    unsigned char flags = static_cast<unsigned char>(d[p + 4]);
    int type = flags & 63;
    size_t q = p + 5;
    unsigned char r = static_cast<unsigned char>(d[q]);
    uint32_t count = r >> 5;
    if (count == 7) {
      if (q + 4 > d.size()) break;
      count = u32(q) & 0x1fffffff;
      q += 4 + (count + 8) / 8;
    } else {
      q += 1;
    }
    size_t refSize = num <= 256 ? 1 : num <= 65536 ? 2 : 4;
    q += refSize * count;
    q += (flags & 64) ? 4 : 1;
    if (q + 4 > d.size()) break;
    uint32_t len = u32(q);
    q += 4;
    ++out[type];
    if (len == 0xffffffffu || q + len > d.size()) break;
    p = q + len;
  }
  return out;
}

struct JpxInfo {
  bool ok = false;
  int nc = 0, colr = 0, approx1 = 0;
  bool sameDepth = true, cieJab = false;
  std::set<int> meth;
};

void jpxBoxes(const std::string& d, size_t from, size_t to, JpxInfo& ji, int depth) {
  size_t p = from;
  auto u32 = [&](size_t o) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(d[o])) << 24) | (static_cast<uint32_t>(static_cast<unsigned char>(d[o + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d[o + 2])) << 8) | static_cast<unsigned char>(d[o + 3]);
  };
  while (p + 8 <= to && depth < 8) {
    uint64_t len = u32(p);
    std::string type = d.substr(p + 4, 4);
    size_t hdr = 8;
    if (len == 1) {
      if (p + 16 > to) return;
      len = (static_cast<uint64_t>(u32(p + 8)) << 32) | u32(p + 12);
      hdr = 16;
    } else if (len == 0) {
      len = to - p;
    }
    if (len < hdr || p + len > to) return;
    size_t b = p + hdr, e = p + static_cast<size_t>(len);
    if (type == "jp2h") {
      jpxBoxes(d, b, e, ji, depth + 1);
    } else if (type == "ihdr" && e - b >= 14) {
      ji.ok = true;
      ji.nc = (static_cast<unsigned char>(d[b + 8]) << 8) | static_cast<unsigned char>(d[b + 9]);
      ji.sameDepth = static_cast<unsigned char>(d[b + 10]) != 255;
    } else if (type == "bpcc") {
      ji.sameDepth = true;
      for (size_t i = b + 1; i < e; ++i) {
        if (d[i] != d[b]) ji.sameDepth = false;
      }
    } else if (type == "colr" && e - b >= 3) {
      ++ji.colr;
      int meth = static_cast<unsigned char>(d[b]);
      ji.meth.insert(meth);
      if (static_cast<unsigned char>(d[b + 2]) == 1) ++ji.approx1;
      if (meth == 1 && e - b >= 7 && u32(b + 3) == 19) ji.cieJab = true;
    }
    p = e;
  }
}

JpxInfo jpxInfo(const std::string& d) {
  JpxInfo ji;
  if (d.size() >= 12 && d.compare(4, 4, "jP  ") == 0) {
    jpxBoxes(d, 0, d.size(), ji, 0);
  } else if (d.size() >= 42 && static_cast<unsigned char>(d[0]) == 0xFF && static_cast<unsigned char>(d[1]) == 0x4F &&
             static_cast<unsigned char>(d[2]) == 0xFF && static_cast<unsigned char>(d[3]) == 0x51) {
    ji.ok = true;
    ji.nc = (static_cast<unsigned char>(d[40]) << 8) | static_cast<unsigned char>(d[41]);
    for (int i = 1; i < ji.nc && 42 + i * 3 < static_cast<int>(d.size()); ++i) {
      if (d[42 + i * 3] != d[42]) ji.sameDepth = false;
    }
  }
  return ji;
}

int jpegQuality(const std::string& d) {
  static const int lum[64] = {16, 11, 12, 14, 12, 10, 16, 14, 13, 14, 18, 17, 16, 19, 24, 40, 26, 24, 22, 22, 24, 49,
                              35, 37, 29, 40, 58, 51, 61, 60, 57, 51, 56, 55, 64, 72, 92, 78, 64, 68, 87, 69, 55, 56,
                              80, 109, 81, 87, 95, 98, 103, 104, 103, 62, 77, 113, 121, 112, 100, 120, 92, 101, 103, 99};
  size_t p = 2;
  if (d.size() < 4 || static_cast<unsigned char>(d[0]) != 0xFF || static_cast<unsigned char>(d[1]) != 0xD8) return -1;
  while (p + 4 <= d.size()) {
    if (static_cast<unsigned char>(d[p]) != 0xFF) return -1;
    int m = static_cast<unsigned char>(d[p + 1]);
    if (m == 0xFF) {
      ++p;
      continue;
    }
    if (m == 0xDA || m == 0xD9) return -1;
    size_t len = (static_cast<unsigned char>(d[p + 2]) << 8) | static_cast<unsigned char>(d[p + 3]);
    if (len < 2 || p + 2 + len > d.size()) return -1;
    if (m == 0xDB) {
      size_t q = p + 4;
      int pq = static_cast<unsigned char>(d[q]) >> 4, tq = static_cast<unsigned char>(d[q]) & 15;
      if (tq != 0) {
        p += 2 + len;
        continue;
      }
      double sum = 0;
      for (int i = 0; i < 64; ++i) {
        size_t o = q + 1 + (pq ? i * 2 : i);
        if (o + (pq ? 1 : 0) >= d.size()) return -1;
        int v = pq ? (static_cast<unsigned char>(d[o]) << 8) | static_cast<unsigned char>(d[o + 1]) : static_cast<unsigned char>(d[o]);
        sum += v * 100.0 / lum[i];
      }
      double s = sum / 64.0;
      double qual = s <= 100 ? (200.0 - s) / 2.0 : 5000.0 / s;
      return static_cast<int>(std::lround(std::max(0.0, std::min(100.0, qual))));
    }
    p += 2 + len;
  }
  return -1;
}

bool parseDateInstant(const std::string& in, long long& out) {
  std::string s = in;
  bool pdf = s.rfind("D:", 0) == 0;
  if (pdf) s = s.substr(2);
  std::string digits;
  size_t i = 0;
  int fields[6] = {0, 1, 1, 0, 0, 0};
  int nf = 0;
  static const int width[6] = {4, 2, 2, 2, 2, 2};
  for (; i < s.size() && nf < 6;) {
    if (!pdf && (s[i] == '-' || s[i] == ':' || s[i] == 'T') && nf > 0) {
      ++i;
      continue;
    }
    if (i + width[nf] > s.size()) break;
    std::string part = s.substr(i, width[nf]);
    if (part.find_first_not_of("0123456789") != std::string::npos) break;
    fields[nf++] = std::atoi(part.c_str());
    i += width[nf - 1];
  }
  if (nf == 0) return false;
  if (!pdf && i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
  }
  long long tz = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    int sign = s[i] == '-' ? -1 : 1;
    std::string rest;
    for (size_t j = i + 1; j < s.size(); ++j) {
      if (std::isdigit(static_cast<unsigned char>(s[j]))) rest += s[j];
    }
    int hh = rest.size() >= 2 ? std::atoi(rest.substr(0, 2).c_str()) : 0;
    int mm = rest.size() >= 4 ? std::atoi(rest.substr(2, 2).c_str()) : 0;
    tz = sign * (hh * 3600LL + mm * 60LL);
  }
  int y = fields[0], mo = fields[1], d = fields[2];
  y -= mo <= 2;
  long long era = (y >= 0 ? y : y - 399) / 400;
  long long yoe = y - era * 400;
  long long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long long days = era * 146097 + doe - 719468;
  out = days * 86400LL + fields[3] * 3600LL + fields[4] * 60LL + fields[5] - tz;
  return true;
}

bool sfntHasCmap(const std::string& p, int platform, int encoding, int& count) {
  count = 0;
  if (p.size() < 12) return false;
  auto u16 = [&](size_t o) { return o + 2 <= p.size() ? (static_cast<unsigned char>(p[o]) << 8) | static_cast<unsigned char>(p[o + 1]) : 0; };
  auto u32 = [&](size_t o) { return o + 4 <= p.size() ? (static_cast<uint32_t>(u16(o)) << 16) | static_cast<uint32_t>(u16(o + 2)) : 0u; };
  int nt = u16(4);
  for (int i = 0; i < nt; ++i) {
    size_t rec = 12 + static_cast<size_t>(i) * 16;
    if (rec + 16 > p.size()) break;
    if (p.compare(rec, 4, "cmap") != 0) continue;
    size_t off = u32(rec + 8);
    count = u16(off + 2);
    bool found = false;
    for (int k = 0; k < count; ++k) {
      size_t e = off + 4 + static_cast<size_t>(k) * 8;
      if (static_cast<int>(u16(e)) == platform && static_cast<int>(u16(e + 2)) == encoding) found = true;
    }
    return found;
  }
  return false;
}

using Strs = std::vector<std::string>;

std::vector<QPDFObjectHandle> allObjects(const Events& ev) {
  QPDF* q = ev.root.isInitialized() ? ev.root.getOwningQPDF() : nullptr;
  return q ? q->getAllObjects() : std::vector<QPDFObjectHandle>();
}

Value boolText(bool b) { return Value(std::string(b ? "true" : "false")); }

Value textOf(QPDFObjectHandle o) { return o.isString() || o.isName() ? Value(str(o)) : Value(); }

Value listOf(const std::set<std::string>& s) { return Value(Strs(s.begin(), s.end())); }

bool asks(const PfAtom& a, const char* v) {
  for (const std::string& x : a.vals) {
    if (x == v) return true;
  }
  return false;
}

int each(const std::vector<Value>& vs, const PfAtom& a) {
  int n = 0;
  for (const Value& v : vs) n += compare(v, a);
  return n;
}

Value orAbsent(const Strs& v) { return v.empty() ? Value() : Value(v); }

template <class F>
int onPages(const Subject& s, const Events& ev, F fn) {
  if (s.pageFacts) return fn(*s.pageFacts);
  int n = 0;
  for (const PageFacts& p : ev.pages) n += fn(p);
  return n;
}

const Box& effBox(const PageFacts& p, const std::string& n) {
  if (n == "media") return p.media;
  if (n == "bleed") return p.bleed.valid ? p.bleed : p.crop;
  if (n == "trim") return p.trim.valid ? p.trim : p.crop;
  if (n == "art") return p.art.valid ? p.art : p.crop;
  return p.crop.valid ? p.crop : p.media;
}

bool sameBox(const Box& a, const Box& b) {
  return a.valid && b.valid && std::fabs(a.x0 - b.x0) < 0.01 && std::fabs(a.y0 - b.y0) < 0.01 &&
         std::fabs(a.x1 - b.x1) < 0.01 && std::fabs(a.y1 - b.y1) < 0.01;
}

bool encloses(const Box& o, const Box& i) {
  return o.valid && i.valid && o.x0 <= i.x0 + 0.01 && o.y0 <= i.y0 + 0.01 && o.x1 >= i.x1 - 0.01 && o.y1 >= i.y1 - 0.01;
}

std::set<QPDFObjGen> pageSet(const Events& ev) {
  std::set<QPDFObjGen> s;
  for (const PageFacts& p : ev.pages) {
    if (p.obj.isIndirect()) s.insert(p.obj.getObjGen());
  }
  return s;
}

std::map<QPDFObjGen, int> pageIndex(const Events& ev) {
  std::map<QPDFObjGen, int> m;
  for (size_t i = 0; i < ev.pages.size(); ++i) {
    if (ev.pages[i].obj.isIndirect()) m[ev.pages[i].obj.getObjGen()] = static_cast<int>(i);
  }
  return m;
}

void forOutline(QPDFObjectHandle root, const std::function<void(QPDFObjectHandle)>& fn) {
  std::set<QPDFObjGen> seen;
  std::function<void(QPDFObjectHandle, int)> ow = [&](QPDFObjectHandle it, int dd) {
    for (int g = 0; it.isDictionary() && g < 100000 && dd < 64; ++g) {
      if (it.isIndirect() && !seen.insert(it.getObjGen()).second) return;
      fn(it);
      ow(it.getKey("/First"), dd + 1);
      it = it.getKey("/Next");
    }
  };
  QPDFObjectHandle ol = root.isDictionary() ? root.getKey("/Outlines") : QPDFObjectHandle();
  if (ol.isDictionary()) ow(ol.getKey("/First"), 0);
}

void forPageAnnots(const Events& ev, const std::function<void(QPDFObjectHandle, const PageFacts&)>& fn) {
  for (const PageFacts& p : ev.pages) {
    QPDFObjectHandle an = p.obj.isDictionary() ? p.obj.getKey("/Annots") : QPDFObjectHandle();
    for (int i = 0; an.isArray() && i < an.getArrayNItems(); ++i) {
      if (an.getArrayItem(i).isDictionary()) fn(an.getArrayItem(i), p);
    }
  }
}

void forNameTree(QPDFObjectHandle n, const std::function<void(QPDFObjectHandle, QPDFObjectHandle)>& fn, int depth = 0) {
  if (!n.isDictionary() || depth > 32) return;
  QPDFObjectHandle arr = n.getKey("/Names");
  for (int i = 0; arr.isArray() && i + 1 < arr.getArrayNItems(); i += 2) fn(arr.getArrayItem(i), arr.getArrayItem(i + 1));
  QPDFObjectHandle kids = n.getKey("/Kids");
  for (int i = 0; kids.isArray() && i < kids.getArrayNItems(); ++i) forNameTree(kids.getArrayItem(i), fn, depth + 1);
}

QPDFObjectHandle namesEntry(const Events& ev, const char* key) {
  QPDFObjectHandle names = ev.root.isDictionary() ? ev.root.getKey("/Names") : QPDFObjectHandle();
  return names.isDictionary() ? names.getKey(key) : QPDFObjectHandle::newNull();
}

struct ActionRef {
  std::string type, on, trigger;
  QPDFObjectHandle act;
};

std::string actionType(const std::string& s) {
  static const std::map<std::string, std::string> m = {
      {"/GoTo", "goto"}, {"/GoToR", "goto_remote"}, {"/GoToE", "goto_embedded"}, {"/GoTo3DView", "goto_3d_view"},
      {"/Launch", "launch"}, {"/Thread", "thread"}, {"/URI", "uri"}, {"/Sound", "sound"}, {"/Movie", "movie"},
      {"/Hide", "hide"}, {"/Named", "named"}, {"/SubmitForm", "submit_form"}, {"/ResetForm", "reset_form"},
      {"/ImportData", "import_data"}, {"/JavaScript", "javascript"}, {"/SetOCGState", "set_ocg_state"},
      {"/Rendition", "rendition"}, {"/Trans", "transition"}, {"/RichMediaExecute", "rich_media"}};
  auto it = m.find(s);
  return it != m.end() ? it->second : lowerAscii(s.size() > 1 ? s.substr(1) : s);
}

std::vector<ActionRef> collectActions(const Events& ev) {
  std::vector<ActionRef> out;
  QPDFObjectHandle root = ev.root;
  if (!root.isDictionary()) return out;
  std::set<QPDFObjGen> seen;
  std::function<void(QPDFObjectHandle, const std::string&, const std::string&, int)> add =
      [&](QPDFObjectHandle act, const std::string& on, const std::string& trig, int depth) {
        if (depth > 32) return;
        if (act.isArray()) {
          for (int i = 0; i < act.getArrayNItems(); ++i) add(act.getArrayItem(i), on, trig, depth + 1);
          return;
        }
        if (!act.isDictionary()) return;
        if (act.isIndirect() && !seen.insert(act.getObjGen()).second) return;
        if (act.getKey("/S").isName()) out.push_back({actionType(act.getKey("/S").getName()), on, trig, act});
        add(act.getKey("/Next"), on, trig, depth + 1);
      };
  auto addAA = [&](QPDFObjectHandle aa, const std::string& on) {
    if (!aa.isDictionary()) return;
    for (const std::string& k : aa.getKeys()) add(aa.getKey(k), on, "additional", 0);
  };
  auto dest = [&](QPDFObjectHandle d, const std::string& on, const std::string& trig) {
    if (d.isArray() || d.isName() || d.isString()) out.push_back({"goto", on, trig, d});
  };
  QPDFObjectHandle oa = root.getKey("/OpenAction");
  if (oa.isArray()) dest(oa, "document", "open");
  else add(oa, "document", "open", 0);
  addAA(root.getKey("/AA"), "document");
  forNameTree(namesEntry(ev, "/JavaScript"), [&](QPDFObjectHandle, QPDFObjectHandle v) { add(v, "document", "open", 0); });
  for (const PageFacts& p : ev.pages) {
    if (p.obj.isDictionary()) addAA(p.obj.getKey("/AA"), "page");
  }
  forPageAnnots(ev, [&](QPDFObjectHandle an, const PageFacts&) {
    bool widget = nameIs(an.getKey("/Subtype"), "/Widget");
    std::string on = widget ? "form_field" : "annotation";
    add(an.getKey("/A"), on, "activate", 0);
    if (!an.getKey("/A").isDictionary()) dest(an.getKey("/Dest"), on, "activate");
    addAA(an.getKey("/AA"), on);
    if (widget) {
      QPDFObjectHandle f = fieldRoot(an);
      if (f.isDictionary() && !(f.isIndirect() && an.isIndirect() && f.getObjGen() == an.getObjGen())) addAA(f.getKey("/AA"), "form_field");
    }
  });
  std::function<void(QPDFObjectHandle, int)> fields = [&](QPDFObjectHandle f, int depth) {
    if (!f.isDictionary() || depth > 32) return;
    addAA(f.getKey("/AA"), "form_field");
    QPDFObjectHandle k = f.getKey("/Kids");
    for (int i = 0; k.isArray() && i < k.getArrayNItems(); ++i) fields(k.getArrayItem(i), depth + 1);
  };
  QPDFObjectHandle acro = root.getKey("/AcroForm");
  QPDFObjectHandle fl = acro.isDictionary() ? acro.getKey("/Fields") : QPDFObjectHandle();
  for (int i = 0; fl.isArray() && i < fl.getArrayNItems(); ++i) fields(fl.getArrayItem(i), 0);
  forOutline(root, [&](QPDFObjectHandle it) {
    add(it.getKey("/A"), "outline", "activate", 0);
    if (!it.getKey("/A").isDictionary()) dest(it.getKey("/Dest"), "outline", "activate");
  });
  return out;
}

QPDFObjectHandle destOf(QPDFObjectHandle holder) {
  QPDFObjectHandle d = holder.getKey("/Dest");
  if (!d.isNull()) return d;
  QPDFObjectHandle act = holder.getKey("/A");
  if (act.isDictionary() && nameIs(act.getKey("/S"), "/GoTo")) return act.getKey("/D");
  return QPDFObjectHandle();
}

std::vector<QPDFObjectHandle> gatherDests(const Events& ev, const std::string& from) {
  std::vector<QPDFObjectHandle> out;
  bool any = from == "any";
  if (from == "link" || any) {
    forPageAnnots(ev, [&](QPDFObjectHandle an, const PageFacts&) {
      if (!nameIs(an.getKey("/Subtype"), "/Link")) return;
      QPDFObjectHandle d = any ? an.getKey("/Dest") : destOf(an);
      if (d.isInitialized() && !d.isNull()) out.push_back(d);
    });
  }
  if (from == "bookmark" || any) {
    forOutline(ev.root, [&](QPDFObjectHandle it) {
      QPDFObjectHandle d = any ? it.getKey("/Dest") : destOf(it);
      if (d.isInitialized() && !d.isNull()) out.push_back(d);
    });
  }
  if (from == "action" || any) {
    QPDFObjectHandle oa = ev.root.isDictionary() ? ev.root.getKey("/OpenAction") : QPDFObjectHandle();
    if (oa.isArray()) out.push_back(oa);
    for (const ActionRef& r : collectActions(ev)) {
      if (r.type == "goto" && !r.act.getKey("/D").isNull()) out.push_back(r.act.getKey("/D"));
    }
  }
  return out;
}

std::string fitMode(QPDFObjectHandle arr) {
  if (!arr.isArray() || arr.getArrayNItems() < 2 || !arr.getArrayItem(1).isName()) return std::string();
  return lowerAscii(arr.getArrayItem(1).getName().substr(1));
}

int evActionBrokenDestination(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::string from = param(a, "from");
  std::set<QPDFObjGen> pages = pageSet(ev);
  bool broken = false;
  for (const std::string& f : {std::string("link"), std::string("bookmark")}) {
    if (from != "any" && from != f) continue;
    for (QPDFObjectHandle d : gatherDests(ev, f)) {
      QPDFObjectHandle arr = resolveDest(ev.root, d);
      if (!arr.isArray() || arr.getArrayNItems() == 0) {
        broken = true;
        continue;
      }
      QPDFObjectHandle pg = arr.getArrayItem(0);
      if (pg.isInteger()) continue;
      if (!pg.isDictionary() || (pg.isIndirect() && !pages.count(pg.getObjGen()))) broken = true;
    }
  }
  return compare(Value(broken), a);
}

int evActionDestination(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  Strs modes;
  for (QPDFObjectHandle d : gatherDests(ev, param(a, "from"))) {
    std::string m = fitMode(resolveDest(ev.root, d));
    if (!m.empty()) modes.push_back(m);
  }
  return compare(Value(modes), a);
}

int evActionPresent(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string &type = param(a, "type"), &on = param(a, "on"), &trig = param(a, "trigger");
  bool hit = false;
  for (const ActionRef& r : collectActions(ev)) {
    if ((type == "any" || type == r.type) && (on == "any" || on == r.on) && (trig == "any" || trig == r.trigger)) hit = true;
  }
  return compare(Value(hit), a);
}

int evActionType(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::set<std::string> types;
  for (const ActionRef& r : collectActions(ev)) types.insert(r.type);
  return compare(listOf(types), a);
}

int evActionUri(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.annot || !s.annot->obj.isDictionary()) return 0;
  QPDFObjectHandle act = s.annot->obj.getKey("/A");
  if (!act.isDictionary() || !nameIs(act.getKey("/S"), "/URI")) return 0;
  return compare(textOf(act.getKey("/URI")), a);
}

bool isWidget(const AnnotFacts& an) { return an.subtype == "Widget"; }

int evAnnotationAppearance(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.annot || !s.annot->obj.isDictionary()) return 0;
  QPDFObjectHandle o = s.annot->obj;
  QPDFObjectHandle ap = o.getKey("/AP");
  std::set<std::string> out;
  QPDFObjectHandle n = ap.isDictionary() ? ap.getKey("/N") : QPDFObjectHandle();
  if (!ap.isDictionary() || n.isNull()) out.insert("missing");
  for (const std::string& k : ap.isDictionary() ? ap.getKeys() : std::set<std::string>()) {
    if (k != "/N") out.insert("has_other_than_normal");
  }
  if (n.isDictionary()) {
    out.insert("normal_is_dictionary");
    if (n.getKeys().size() > 1) out.insert("normal_has_several_states");
  }
  if (isWidget(*s.annot)) {
    QPDFObjectHandle acro = ev.root.isDictionary() ? ev.root.getKey("/AcroForm") : QPDFObjectHandle();
    QPDFObjectHandle na = acro.isDictionary() ? acro.getKey("/NeedAppearances") : QPDFObjectHandle();
    if (na.isBool() && na.getBoolValue()) out.insert("needs_appearances_flag");
  }
  return compare(listOf(out), a);
}

std::string numbersText(QPDFObjectHandle arr) {
  std::string out;
  for (int i = 0; arr.isArray() && i < arr.getArrayNItems(); ++i) {
    out += (out.empty() ? "" : " ") + fmtFixed(numOf(arr.getArrayItem(i), 0), 4);
  }
  return out;
}

int evAnnotationEntry(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.annot || !s.annot->obj.isDictionary()) return 0;
  QPDFObjectHandle o = s.annot->obj;
  const std::string& e = param(a, "entry");
  const PageFacts* p = s.pageFacts ? s.pageFacts : pageFor(ev, s.annot->page);
  QPDFObjectHandle tabs = p && p->obj.isDictionary() ? p->obj.getKey("/Tabs") : QPDFObjectHandle();
  Value v;
  if (e == "contents") v = textOf(o.getKey("/Contents"));
  else if (e == "name") v = textOf(o.getKey("/NM"));
  else if (e == "opacity") v = o.getKey("/CA").isNumber() ? Value(o.getKey("/CA").getNumericValue()) : Value(1.0);
  else if (e == "has_opacity") v = boolText(o.hasKey("/CA"));
  else if (e == "blend_mode") v = textOf(o.getKey("/BM"));
  else if (e == "has_blend_mode") v = boolText(o.hasKey("/BM"));
  else if (e == "colour") v = o.getKey("/C").isArray() ? Value(numbersText(o.getKey("/C"))) : Value();
  else if (e == "colour_is_rgb") v = boolText(o.getKey("/C").isArray() && o.getKey("/C").getArrayNItems() == 3);
  else if (e == "interior_colour") v = o.getKey("/IC").isArray() ? Value(numbersText(o.getKey("/IC"))) : Value();
  else if (e == "interior_colour_is_rgb") v = boolText(o.getKey("/IC").isArray() && o.getKey("/IC").getArrayNItems() == 3);
  else if (e == "has_appearance") v = boolText(o.getKey("/AP").isDictionary());
  else if (e == "tab_order") v = textOf(tabs);
  else if (e == "has_tab_order") v = boolText(tabs.isName());
  return compare(v, a);
}

int evAnnotationFlags(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.annot || !s.annot->obj.isDictionary()) return 0;
  QPDFObjectHandle f = s.annot->obj.getKey("/F");
  static const char* names[] = {"invisible", "hidden", "print", "no_zoom", "no_rotate", "no_view", "read_only", "locked", "toggle_no_view", "locked_contents"};
  Strs out;
  if (!f.isInteger()) out.push_back("missing");
  long long v = f.isInteger() ? f.getIntValue() : 0;
  for (int i = 0; i < 10; ++i) {
    if ((v >> i) & 1) out.push_back(names[i]);
  }
  return compare(Value(out), a);
}

int evAnnotationInteractive(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.annot) return 0;
  const std::string& t = s.annot->subtype;
  return compare(Value(t != "PrinterMark" && t != "TrapNet" && t != "Watermark"), a);
}

int evAnnotationMinimumPdfVersion(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.annot) return 0;
  static const std::map<std::string, double> v = {
      {"Text", 1.0}, {"Link", 1.0}, {"Sound", 1.2}, {"Movie", 1.2}, {"Widget", 1.2}, {"FreeText", 1.3}, {"Line", 1.3},
      {"Square", 1.3}, {"Circle", 1.3}, {"Highlight", 1.3}, {"Underline", 1.3}, {"StrikeOut", 1.3}, {"Stamp", 1.3},
      {"Ink", 1.3}, {"Popup", 1.3}, {"FileAttachment", 1.3}, {"TrapNet", 1.3}, {"Squiggly", 1.4}, {"PrinterMark", 1.4},
      {"Polygon", 1.5}, {"PolyLine", 1.5}, {"Caret", 1.5}, {"Screen", 1.5}, {"Watermark", 1.6}, {"3D", 1.6},
      {"Redact", 1.7}, {"Projection", 2.0}, {"RichMedia", 2.0}};
  auto it = v.find(s.annot->subtype);
  return it == v.end() ? 0 : compare(Value(it->second), a);
}

int evAnnotationOutsideCrop(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.annot || !s.annot->rect.valid) return 0;
  const Box& r = s.annot->rect;
  const PageFacts* p = s.pageFacts ? s.pageFacts : pageFor(ev, s.annot->page);
  std::string v = "inside";
  if (r.x1 - r.x0 < 0.01 || r.y1 - r.y0 < 0.01) v = "zero_size";
  else if (p && p->crop.valid && !boxesIntersect(p->crop, r)) v = "outside";
  return compare(Value(v), a);
}

struct FileRef {
  QPDFObjectHandle fs;
  std::string at;
};

std::string afKind(QPDFObjectHandle o, QPDFObjectHandle d) {
  if (nameIs(d.getKey("/Type"), "/Catalog")) return "document";
  if (nameIs(d.getKey("/Type"), "/Page")) return "page";
  if (o.isStream() && nameIs(d.getKey("/Subtype"), "/Image")) return "image";
  if (o.isStream() && nameIs(d.getKey("/Subtype"), "/Form")) return "form_xobject";
  if (nameIs(d.getKey("/Type"), "/DPart")) return "document_part";
  if (nameIs(d.getKey("/Type"), "/StructElem") || (d.hasKey("/S") && d.hasKey("/P") && !d.hasKey("/Rect"))) return "structure_element";
  if (d.hasKey("/Rect") && d.hasKey("/Subtype")) return "annotation";
  return "other";
}

struct Attachments {
  std::vector<FileRef> af;
  std::vector<FileRef> plain;
  std::set<QPDFObjGen> assoc;
  int directAssoc = 0;
  std::vector<QPDFObjectHandle> unique;
  std::vector<std::pair<QPDFObjectHandle, QPDFObjectHandle>> afHolders;
};

bool hasEF(QPDFObjectHandle fs) { return fs.isDictionary() && fs.getKey("/EF").isDictionary(); }

Attachments attachments(const Events& ev) {
  Attachments at;
  for (QPDFObjectHandle o : allObjects(ev)) {
    QPDFObjectHandle d = o.isStream() ? o.getDict() : o;
    if (!d.isDictionary() || !d.hasKey("/AF")) continue;
    at.afHolders.push_back({o, d});
    QPDFObjectHandle af = d.getKey("/AF");
    std::string kind = afKind(o, d);
    for (int i = 0; af.isArray() && i < af.getArrayNItems(); ++i) {
      QPDFObjectHandle fs = af.getArrayItem(i);
      if (!hasEF(fs)) continue;
      at.af.push_back({fs, kind});
      if (fs.isIndirect()) at.assoc.insert(fs.getObjGen());
    }
  }
  QPDF* q = ev.root.isInitialized() ? ev.root.getOwningQPDF() : nullptr;
  for (const QPDFObjGen& g : ev.bdcFiles) {
    if (q) at.af.push_back({q->getObject(g), "marked_content"});
    at.assoc.insert(g);
  }
  for (int i = 0; i < ev.bdcDirectFiles; ++i) at.af.push_back({QPDFObjectHandle(), "marked_content"});
  forNameTree(namesEntry(ev, "/EmbeddedFiles"), [&](QPDFObjectHandle, QPDFObjectHandle fs) {
    if (hasEF(fs)) at.plain.push_back({fs, "document"});
  });
  forPageAnnots(ev, [&](QPDFObjectHandle an, const PageFacts&) {
    if (nameIs(an.getKey("/Subtype"), "/FileAttachment") && hasEF(an.getKey("/FS"))) at.plain.push_back({an.getKey("/FS"), "annotation"});
  });
  std::set<QPDFObjGen> seen;
  for (const auto* list : {&at.plain, &at.af}) {
    for (const FileRef& r : *list) {
      if (!r.fs.isDictionary()) continue;
      if (r.fs.isIndirect() && !seen.insert(r.fs.getObjGen()).second) continue;
      at.unique.push_back(r.fs);
    }
  }
  return at;
}

QPDFObjectHandle efStream(QPDFObjectHandle fs) {
  QPDFObjectHandle ef = fs.isDictionary() ? fs.getKey("/EF") : QPDFObjectHandle();
  if (!ef.isDictionary()) return QPDFObjectHandle();
  return ef.getKey("/UF").isStream() ? ef.getKey("/UF") : ef.getKey("/F");
}

std::string fileName(QPDFObjectHandle fs) { return str(fs.getKey("/UF")).empty() ? str(fs.getKey("/F")) : str(fs.getKey("/UF")); }

int evAttachmentCount(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string &to = param(a, "attached_to"), &assoc = param(a, "associated");
  Attachments at = attachments(ev);
  auto placed = [&](const std::string& k) {
    if (to == "any") return true;
    if (to == "page_object") return k == "image" || k == "form_xobject" || k == "marked_content";
    return k == to;
  };
  std::set<QPDFObjGen> files;
  int direct = 0;
  auto add = [&](const FileRef& r, bool associated) {
    if (!placed(r.at)) return;
    if (assoc == "yes" && !associated) return;
    if (assoc == "no" && associated) return;
    if (r.fs.isIndirect()) files.insert(r.fs.getObjGen());
    else ++direct;
  };
  for (const FileRef& r : at.af) add(r, true);
  for (const FileRef& r : at.plain) add(r, r.fs.isIndirect() && at.assoc.count(r.fs.getObjGen()));
  return compare(Value(static_cast<double>(files.size()) + direct), a);
}

Strs conformanceClaims(const std::string& x) {
  Strs out;
  std::string part = xmpValue(x, "pdfaid:part"), conf = lowerAscii(xmpValue(x, "pdfaid:conformance"));
  if (!part.empty()) out.push_back("pdfa-" + part + conf);
  std::string xv = xmpValue(x, "pdfxid:GTS_PDFXVersion");
  if (xv.rfind("PDF/X-", 0) == 0) out.push_back("pdfx-" + lowerAscii(xv.substr(6, xv.find(':') == std::string::npos ? std::string::npos : xv.find(':') - 6)));
  std::string ev2 = xmpValue(x, "pdfe:ISO_PDFEVersion");
  if (ev2.rfind("PDF/E-", 0) == 0) out.push_back("pdfe-" + ev2.substr(6));
  std::string ua = xmpValue(x, "pdfuaid:part");
  if (!ua.empty()) out.push_back("pdfua-" + ua);
  std::string vt = xmpValue(x, "pdfvtid:GTS_PDFVTVersion");
  if (vt.rfind("PDF/VT-", 0) == 0) out.push_back("pdfvt-" + lowerAscii(vt.substr(7)));
  return out;
}

int evAttachmentIsPdf(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string& want = param(a, "conforms_to");
  bool hit = false;
  for (QPDFObjectHandle fs : attachments(ev).unique) {
    std::string data = streamBytes(efStream(fs));
    if (data.rfind("%PDF-", 0) != 0) continue;
    if (want.empty()) {
      hit = true;
      break;
    }
    try {
      QPDF q;
      q.setSuppressWarnings(true);
      q.processMemoryFile("attachment", data.data(), data.size());
      for (const std::string& c : conformanceClaims(xmpOf(q.getRoot()))) hit = hit || c == want;
    } catch (...) {
    }
  }
  return compare(Value(hit), a);
}

int evAttachmentProblem(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  static const std::set<std::string> rel = {"/Source", "/Data", "/Alternative", "/Supplement", "/EncryptedPayload", "/FormData", "/Schema", "/Unspecified"};
  static const std::regex mime("^[a-zA-Z0-9.+-]+/[a-zA-Z0-9.+-]+$");
  Attachments at = attachments(ev);
  std::set<QPDFObjGen> inTree;
  for (const FileRef& r : at.plain) {
    if (r.at == "document" && r.fs.isIndirect()) inTree.insert(r.fs.getObjGen());
  }
  std::set<std::string> out;
  std::string me = ev.inputPath.empty() ? std::string() : lowerAscii(std::filesystem::path(ev.inputPath).stem().string());
  for (QPDFObjectHandle fs : at.unique) {
    QPDFObjectHandle st = efStream(fs);
    QPDFObjectHandle sd = st.isStream() ? st.getDict() : QPDFObjectHandle();
    if (!fs.hasKey("/Desc")) out.insert("missing_description");
    else if (str(fs.getKey("/Desc")).empty()) out.insert("empty_description");
    if (sd.isDictionary() && !sd.hasKey("/Subtype")) out.insert("missing_mime_type");
    if (sd.isDictionary() && sd.hasKey("/Subtype") && !std::regex_match(str(sd.getKey("/Subtype")), mime)) out.insert("invalid_mime_type");
    if (!fs.hasKey("/AFRelationship")) out.insert("missing_relationship");
    else if (!rel.count(nameOf(fs.getKey("/AFRelationship")))) out.insert("non_standard_relationship");
    bool assoc = fs.isIndirect() && at.assoc.count(fs.getObjGen());
    if (!assoc) out.insert("not_associated");
    if (!(fs.isIndirect() && inTree.count(fs.getObjGen()))) out.insert("not_in_name_tree");
    QPDFObjectHandle pp = sd.isDictionary() ? sd.getKey("/Params") : QPDFObjectHandle();
    if (pp.isDictionary() && !pp.hasKey("/ModDate")) out.insert("params_without_mod_date");
    if (sd.isDictionary() && !sd.hasKey("/Type")) out.insert("missing_stream_type");
    if (!fs.hasKey("/F")) out.insert("missing_f_key");
    if (!fs.hasKey("/UF")) out.insert("missing_uf_key");
    if (fs.hasKey("/RF")) out.insert("has_related_files");
    if (!me.empty() && lowerAscii(std::filesystem::path(fileName(fs)).stem().string()) != me) out.insert("name_differs_from_pdf_name");
  }
  for (auto& h : at.afHolders) {
    QPDFObjectHandle af = h.second.getKey("/AF");
    bool ok = af.isArray();
    for (int i = 0; ok && i < af.getArrayNItems(); ++i) ok = af.getArrayItem(i).isDictionary();
    if (!ok) out.insert("af_not_array_of_file_specs");
  }
  return compare(listOf(out), a);
}

int evAttachmentProperty(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string& p = param(a, "property");
  Strs out;
  for (QPDFObjectHandle fs : attachments(ev).unique) {
    QPDFObjectHandle st = efStream(fs);
    QPDFObjectHandle sd = st.isStream() ? st.getDict() : QPDFObjectHandle();
    std::string n = fileName(fs);
    std::string v;
    bool has = true;
    if (p == "name") has = !n.empty(), v = n;
    else if (p == "extension") {
      size_t dot = n.rfind('.');
      has = dot != std::string::npos;
      if (has) v = n.substr(dot + 1);
    } else if (p == "description") has = fs.hasKey("/Desc"), v = str(fs.getKey("/Desc"));
    else if (p == "mime_type" || p == "subtype") has = sd.isDictionary() && sd.hasKey("/Subtype"), v = has ? str(sd.getKey("/Subtype")) : "";
    else if (p == "relationship") has = fs.hasKey("/AFRelationship"), v = str(fs.getKey("/AFRelationship"));
    else if (p == "stream_type") has = sd.isDictionary() && sd.hasKey("/Type"), v = has ? str(sd.getKey("/Type")) : "";
    if (has) out.push_back(v);
  }
  return compare(orAbsent(out), a);
}

bool barcodesReady(const Events& ev, bool& supported) {
  if (ev.barcodesScanned) return true;
  supported = false;
  return false;
}

int evBarcodeDimension(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  if (!barcodesReady(ev, supported)) return 0;
  const std::string &m = param(a, "measure"), &unit = param(a, "unit");
  std::vector<Value> vs;
  for (const auto& b : ev.barcodes) {
    if (m == "width") vs.push_back(Value(b.box.x1 - b.box.x0));
    else if (m == "height") vs.push_back(Value(b.box.y1 - b.box.y0));
    else if (m == "module_width" && b.moduleWidthPt > 0) vs.push_back(Value(b.moduleWidthPt));
    else if (m == "quiet_zone") vs.push_back(Value(b.quietZonePt));
    else if (m == "bar_width_reduction" && b.barReductionKnown) vs.push_back(Value(unit == "percent" ? b.barReductionPct : b.barReductionPt));
  }
  return each(vs, a);
}

int evBarcodeFound(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  if (!barcodesReady(ev, supported)) return 0;
  return compare(Value(!ev.barcodes.empty()), a);
}

int evBarcodeProperty(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  if (!barcodesReady(ev, supported)) return 0;
  const std::string& p = param(a, "property");
  std::vector<Value> vs;
  for (const auto& b : ev.barcodes) {
    if (p == "symbology") vs.push_back(Value(b.format));
    else if (p == "value") vs.push_back(Value(b.text));
    else if (p == "rotation") vs.push_back(Value(((b.rotation % 360) + 360) % 360));
    else if (p == "colour_contrast") vs.push_back(Value(b.contrast));
    else if (p == "in_area") vs.push_back(Value(fmtFixed(b.box.x0, 2) + " " + fmtFixed(b.box.y0, 2) + " " + fmtFixed(b.box.x1, 2) + " " + fmtFixed(b.box.y1, 2)));
  }
  return each(vs, a);
}

int evBarcodeQuietZoneSufficient(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  if (!barcodesReady(ev, supported)) return 0;
  std::vector<Value> vs;
  for (const auto& b : ev.barcodes) vs.push_back(Value(b.quietZonePt + 0.01 >= b.moduleWidthPt * (b.linear ? 10 : 4)));
  return each(vs, a);
}

int evConformanceClaim(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string &fam = param(a, "family"), &field = param(a, "field");
  std::string x = xmpOf(ev.root);
  Value v;
  auto digits = [](const std::string& s) { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; };
  if (fam == "pdfa") {
    std::string part = xmpValue(x, "pdfaid:part"), conf = xmpValue(x, "pdfaid:conformance"), amd = xmpValue(x, "pdfaid:amd");
    std::string lc = lowerAscii(conf);
    bool ok = (part == "1" && (lc == "a" || lc == "b")) || ((part == "2" || part == "3") && (lc == "a" || lc == "b" || lc == "u")) ||
              (part == "4" && (lc.empty() || lc == "e" || lc == "f"));
    if (field == "label") v = part.empty() ? Value() : Value("PDF/A-" + part + lc);
    else if (field == "part") v = part.empty() ? Value() : Value(part);
    else if (field == "conformance") v = conf.empty() ? Value() : Value(conf);
    else if (field == "amendment") v = amd.empty() ? Value() : Value(amd);
    else if (field == "well_formed") v = part.empty() ? Value() : boolText(ok);
  } else {
    std::string label = fam == "pdfx" ? xmpValue(x, "pdfxid:GTS_PDFXVersion")
                        : fam == "pdfe" ? xmpValue(x, "pdfe:ISO_PDFEVersion")
                        : fam == "pdfvt" ? xmpValue(x, "pdfvtid:GTS_PDFVTVersion")
                                         : std::string();
    std::string part;
    if (fam == "pdfua") {
      part = xmpValue(x, "pdfuaid:part");
      if (!part.empty()) label = "PDF/UA-" + part;
    } else if (!label.empty()) {
      size_t dash = label.find('-');
      part = dash == std::string::npos ? std::string() : label.substr(dash + 1, label.find(':') == std::string::npos ? std::string::npos : label.find(':') - dash - 1);
    }
    static const std::regex xre("^PDF/X-(1a|3|4|4p|5g|5n|5pg|6|6n|6p)(:[0-9]{4})?$");
    bool ok = fam == "pdfx" ? std::regex_match(label, xre) : fam == "pdfe" ? label == "PDF/E-1"
              : fam == "pdfvt" ? (label == "PDF/VT-1" || label == "PDF/VT-2" || label == "PDF/VT-3") : digits(part);
    std::string conf = fam == "pdfx" ? xmpValue(x, "pdfxid:GTS_PDFXConformance") : std::string();
    if (field == "label") v = label.empty() ? Value() : Value(label);
    else if (field == "part") v = part.empty() ? Value() : Value(part);
    else if (field == "conformance") v = conf.empty() ? Value() : Value(conf);
    else if (field == "well_formed") v = label.empty() ? Value() : boolText(ok);
  }
  return compare(v, a);
}

int evDocumentBinding(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle vp = ev.root.isDictionary() ? ev.root.getKey("/ViewerPreferences") : QPDFObjectHandle();
  return compare(Value(vp.isDictionary() && nameIs(vp.getKey("/Direction"), "/R2L") ? "right" : "left"), a);
}

void colourFamilies(const ColorInfo& ci, std::set<std::string>& out) {
  static const std::map<std::string, std::string> fam = {{"gray", "DeviceGray"}, {"rgb", "DeviceRGB"}, {"cmyk", "DeviceCMYK"},
                                                         {"icc", "ICCBased"}, {"lab", "Lab"}, {"separation", "Separation"},
                                                         {"devicen", "DeviceN"}, {"pattern", "Pattern"}};
  if (ci.cls == "cal") {
    out.insert(ci.declaredComps == 1 ? "CalGray" : "CalRGB");
  } else {
    auto it = fam.find(ci.cls);
    if (it != fam.end()) out.insert(it->second);
  }
  if (ci.indexed) out.insert("Indexed");
}

std::set<std::string> pageColourFamilies(const Events& ev, int page) {
  std::set<std::string> out;
  for (const PaintEvent& e : ev.paints) {
    if ((page < 0 || e.page == page) && !e.noPaint) colourFamilies(e.color, out);
  }
  for (const TextEvent& e : ev.texts) {
    if (page < 0 || e.page == page) colourFamilies(e.color, out);
  }
  for (const ImageEvent& e : ev.images) {
    if ((page < 0 || e.page == page)) colourFamilies(e.color, out);
  }
  return out;
}

int evDocumentColourSpaces(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (param(a, "scope") == "page") return onPages(s, ev, [&](const PageFacts& p) { return compare(listOf(pageColourFamilies(ev, p.page)), a); });
  return compare(listOf(pageColourFamilies(ev, -1)), a);
}

double contentLength(const PageFacts& p) {
  QPDFObjectHandle c = p.obj.isDictionary() ? p.obj.getKey("/Contents") : QPDFObjectHandle();
  double size = 0;
  if (c.isStream()) size = numOf(c.getDict().getKey("/Length"), 0);
  for (int i = 0; c.isArray() && i < c.getArrayNItems(); ++i) {
    if (c.getArrayItem(i).isStream()) size += numOf(c.getArrayItem(i).getDict().getKey("/Length"), 0);
  }
  return size;
}

int evDocumentContentStreamSize(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  double best = 0;
  for (const PageFacts& p : ev.pages) best = std::max(best, contentLength(p));
  return compare(Value(best), a);
}

QPDFObjectHandle infoDict(const Events& ev) { return ev.trailer.isDictionary() ? ev.trailer.getKey("/Info") : QPDFObjectHandle(); }

int evDocumentCreatedByPrintDriver(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle info = infoDict(ev);
  std::string pr = lowerAscii(info.isDictionary() ? str(info.getKey("/Producer")) : std::string());
  bool drv = pr.find("pdfwriter") != std::string::npos || pr.find("print to pdf") != std::string::npos ||
             pr.find("pdf printer") != std::string::npos || pr.find("pdffactory") != std::string::npos;
  return compare(Value(drv), a);
}

int evDocumentDamaged(const PfAtom& a, const Subject&, const Events& ev, bool&) { return compare(Value(ev.damaged), a); }

int evDocumentExtensionLevel(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle ex = ev.root.isDictionary() ? ev.root.getKey("/Extensions") : QPDFObjectHandle();
  const std::string& prefix = param(a, "prefix");
  std::vector<Value> vs;
  for (const std::string& k : ex.isDictionary() ? ex.getKeys() : std::set<std::string>()) {
    if (!prefix.empty() && k.substr(1) != prefix && k != prefix) continue;
    QPDFObjectHandle e = ex.getKey(k);
    double lvl = -1;
    if (e.isDictionary()) lvl = numOf(e.getKey("/ExtensionLevel"), -1);
    for (int i = 0; e.isArray() && i < e.getArrayNItems(); ++i) lvl = std::max(lvl, numOf(e.getArrayItem(i).getKey("/ExtensionLevel"), -1));
    if (lvl >= 0) vs.push_back(Value(lvl));
  }
  return each(vs, a);
}

double versionNumber(const std::string& v) { return v.empty() ? 0 : std::atof(v.c_str()); }

int evDocumentExtensionVersionMismatch(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle ex = ev.root.isDictionary() ? ev.root.getKey("/Extensions") : QPDFObjectHandle();
  std::string ver = ev.root.isDictionary() ? str(ev.root.getKey("/Version")) : std::string();
  if (ver.empty()) ver = ev.header;
  bool bad = false;
  for (const std::string& k : ex.isDictionary() ? ex.getKeys() : std::set<std::string>()) {
    QPDFObjectHandle de = ex.getKey(k);
    std::vector<QPDFObjectHandle> list;
    if (de.isDictionary()) list.push_back(de);
    for (int i = 0; de.isArray() && i < de.getArrayNItems(); ++i) list.push_back(de.getArrayItem(i));
    for (auto& d : list) {
      if (d.isDictionary() && d.hasKey("/BaseVersion") && str(d.getKey("/BaseVersion")) != ver) bad = true;
    }
  }
  return compare(Value(bad), a);
}

int evDocumentFormNesting(const PfAtom& a, const Subject&, const Events& ev, bool&) { return compare(Value(ev.maxFormNest), a); }

std::set<std::string> usedDestNames(const Events& ev) {
  std::set<std::string> used;
  auto note = [&](QPDFObjectHandle d) {
    if (d.isName()) used.insert(d.getName().substr(1));
    if (d.isString()) used.insert(d.getUTF8Value());
  };
  for (QPDFObjectHandle o : allObjects(ev)) {
    if (!o.isDictionary()) continue;
    if (o.hasKey("/Dest")) note(o.getKey("/Dest"));
    if (nameIs(o.getKey("/S"), "/GoTo")) note(o.getKey("/D"));
    QPDFObjectHandle act = o.getKey("/A");
    if (act.isDictionary() && !act.isIndirect() && nameIs(act.getKey("/S"), "/GoTo")) note(act.getKey("/D"));
    QPDFObjectHandle aa = o.getKey("/AA");
    for (const std::string& k : aa.isDictionary() && !aa.isIndirect() ? aa.getKeys() : std::set<std::string>()) {
      QPDFObjectHandle x = aa.getKey(k);
      if (x.isDictionary() && !x.isIndirect() && nameIs(x.getKey("/S"), "/GoTo")) note(x.getKey("/D"));
    }
  }
  forPageAnnots(ev, [&](QPDFObjectHandle an, const PageFacts&) { note(destOf(an)); });
  forOutline(ev.root, [&](QPDFObjectHandle it) { note(destOf(it)); });
  for (const ActionRef& r : collectActions(ev)) {
    if (r.type == "goto") note(r.act.getKey("/D"));
  }
  return used;
}

bool unusedDestinations(const Events& ev) {
  std::set<std::string> defined;
  QPDFObjectHandle dests = ev.root.getKey("/Dests");
  for (const std::string& k : dests.isDictionary() ? dests.getKeys() : std::set<std::string>()) defined.insert(k.substr(1));
  forNameTree(namesEntry(ev, "/Dests"), [&](QPDFObjectHandle k, QPDFObjectHandle) {
    if (k.isString()) defined.insert(k.getUTF8Value());
  });
  if (defined.empty()) return false;
  std::set<std::string> used = usedDestNames(ev);
  for (const std::string& d : defined) {
    if (!used.count(d)) return true;
  }
  return false;
}

int evDocumentHas(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  const std::string& f = param(a, "feature");
  QPDFObjectHandle root = ev.root;
  if (!root.isDictionary()) {
    supported = false;
    return 0;
  }
  bool has = false;
  if (f == "bookmarks") has = root.getKey("/Outlines").isDictionary() && root.getKey("/Outlines").getKey("/First").isDictionary();
  else if (f == "page_labels") has = root.getKey("/PageLabels").isDictionary();
  else if (f == "thumbnails") {
    for (const PageFacts& p : ev.pages) has = has || (p.obj.isDictionary() && p.obj.hasKey("/Thumb"));
  } else if (f == "article_threads") has = root.getKey("/Threads").isArray() && root.getKey("/Threads").getArrayNItems() > 0;
  else if (f == "named_destinations") has = root.hasKey("/Dests") || !namesEntry(ev, "/Dests").isNull();
  else if (f == "unused_destinations") has = unusedDestinations(ev);
  else if (f == "form_fields") {
    QPDFObjectHandle af = root.getKey("/AcroForm");
    has = af.isDictionary() && af.getKey("/Fields").isArray() && af.getKey("/Fields").getArrayNItems() > 0;
  } else if (f == "interactive_annotations") {
    forPageAnnots(ev, [&](QPDFObjectHandle an, const PageFacts&) {
      std::string st = nameOf(an.getKey("/Subtype"));
      if (!st.empty() && st != "/PrinterMark" && st != "/TrapNet") has = true;
    });
  } else if (f == "file_specifications") {
    has = !namesEntry(ev, "/EmbeddedFiles").isNull();
    for (QPDFObjectHandle o : has ? std::vector<QPDFObjectHandle>() : allObjects(ev)) {
      if (o.isDictionary() && (nameIs(o.getKey("/Type"), "/Filespec") || o.getKey("/EF").isDictionary())) {
        has = true;
        break;
      }
    }
  } else if (f == "alternate_presentations") has = !namesEntry(ev, "/AlternatePresentations").isNull();
  else if (f == "piece_info") has = root.hasKey("/PieceInfo");
  else if (f == "xmp_metadata") has = root.getKey("/Metadata").isStream();
  else if (f == "non_plain_text_xmp") has = root.getKey("/Metadata").isStream() && !root.getKey("/Metadata").getDict().getKey("/Filter").isNull();
  else if (f == "optional_content") has = root.getKey("/OCProperties").isDictionary();
  else if (f == "named_optional_content") {
    for (QPDFObjectHandle g : layers(root)) has = has || !str(g.getKey("/Name")).empty();
  } else if (f == "mark_info") has = root.getKey("/MarkInfo").isDictionary();
  else if (f == "needs_rendering") has = root.hasKey("/NeedsRendering");
  else if (f == "javascript") {
    has = !namesEntry(ev, "/JavaScript").isNull();
    for (const ActionRef& r : has ? std::vector<ActionRef>() : collectActions(ev)) has = has || r.type == "javascript";
  } else if (f == "embedded_files_name_tree") has = !namesEntry(ev, "/EmbeddedFiles").isNull();
  else {
    supported = false;
    return 0;
  }
  return compare(Value(has), a);
}

int evDocumentHeaderConforms(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string& st = param(a, "standard");
  const std::string& h = ev.header;
  bool ok = false;
  if (st == "pdf-2.0") ok = h == "2.0";
  else if (st == "pdfa-1b") ok = h.size() == 3 && h[0] == '1' && h[2] >= '0' && h[2] <= '4';
  else if (st == "pdfa-2b") ok = h.size() == 3 && h[0] == '1' && h[2] >= '0' && h[2] <= '7';
  else if (st == "pdfe-1") ok = h.size() == 3 && h[0] == '1' && h[2] >= '0' && h[2] <= '6';
  if (ok && ev.inputData && ev.inputSize > 16) {
    std::string_view f(reinterpret_cast<const char*>(ev.inputData), std::min<size_t>(ev.inputSize, 1024));
    size_t eol = f.find_first_of("\r\n");
    if (eol == std::string_view::npos) {
      ok = false;
    } else {
      size_t p = eol;
      while (p < f.size() && (f[p] == '\r' || f[p] == '\n')) ++p;
      int high = 0;
      if (p < f.size() && f[p] == '%') {
        for (size_t i = p + 1; i < f.size() && f[i] != '\r' && f[i] != '\n'; ++i) high += static_cast<unsigned char>(f[i]) > 127;
      }
      if (high < 4) ok = false;
    }
  }
  return compare(Value(ok), a);
}

int evDocumentId(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle id = ev.trailer.isDictionary() ? ev.trailer.getKey("/ID") : QPDFObjectHandle();
  std::string v = "present";
  if (id.isNull() || !id.isInitialized()) v = "missing";
  else if (id.isIndirect()) v = "indirect";
  else if (!id.isArray() || id.getArrayNItems() != 2 || !id.getArrayItem(0).isString() || !id.getArrayItem(1).isString()) v = "malformed";
  else if (id.getArrayItem(0).getStringValue() != id.getArrayItem(1).getStringValue()) {
    int revisions = 1;
    if (ev.inputData) {
      std::string_view f(reinterpret_cast<const char*>(ev.inputData), ev.inputSize);
      revisions = 0;
      for (size_t p = f.find("startxref"); p != std::string_view::npos; p = f.find("startxref", p + 9)) ++revisions;
    }
    if (revisions <= 1) v = "inconsistent";
  }
  return compare(Value(v), a);
}

bool validLangTag(const std::string& l) {
  static const std::regex bcp("^[A-Za-z]{2,8}(-[A-Za-z0-9]{1,8})*$");
  return !l.empty() && std::regex_match(l, bcp);
}

Value languageValue(QPDFObjectHandle holder, const std::string& c) {
  QPDFObjectHandle l = holder.getKey("/Lang");
  if (c == "present") return boolText(!l.isNull());
  if (c == "valid_tag") return boolText(validLangTag(str(l)));
  if (c == "is_text_string") return l.isNull() ? Value() : boolText(l.isString());
  return l.isString() ? Value(l.getUTF8Value()) : Value();
}

int evDocumentLanguage(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  if (!ev.root.isDictionary()) return 0;
  return compare(languageValue(ev.root, param(a, "test")), a);
}

int evDocumentLargestStream(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  return compare(Value(static_cast<double>(objStats(ev).maxStream)), a);
}

int evDocumentLinearized(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDF* q = ev.root.isInitialized() ? ev.root.getOwningQPDF() : nullptr;
  return compare(Value(q && q->isLinearized()), a);
}

QPDFObjectHandle markInfo(const Events& ev) { return ev.root.isDictionary() ? ev.root.getKey("/MarkInfo") : QPDFObjectHandle(); }

int evDocumentMarkInfoExtraKeys(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle mi = markInfo(ev);
  bool other = false;
  for (const std::string& k : mi.isDictionary() ? mi.getKeys() : std::set<std::string>()) other = other || k != "/Marked";
  return compare(Value(other), a);
}

int evDocumentMarked(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle mi = markInfo(ev);
  return compare(Value(mi.isDictionary() && mi.getKey("/Marked").isBool() && mi.getKey("/Marked").getBoolValue()), a);
}

int evDocumentNeedsRendering(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle nr = ev.root.isDictionary() ? ev.root.getKey("/NeedsRendering") : QPDFObjectHandle();
  return compare(Value(nr.isBool() && nr.getBoolValue()), a);
}

int evDocumentOpenView(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle root = ev.root;
  if (!root.isDictionary()) return 0;
  const std::string& st = param(a, "setting");
  QPDFObjectHandle vp = root.getKey("/ViewerPreferences");
  QPDFObjectHandle oa = root.getKey("/OpenAction");
  QPDFObjectHandle d = oa.isArray() ? oa : (oa.isDictionary() && nameIs(oa.getKey("/S"), "/GoTo") ? resolveDest(root, oa.getKey("/D")) : QPDFObjectHandle());
  Value v;
  if (st == "page_layout") v = textOf(root.getKey("/PageLayout"));
  else if (st == "page_mode") v = textOf(root.getKey("/PageMode"));
  else if (st == "open_page" && d.isArray() && d.getArrayNItems() > 0) {
    QPDFObjectHandle pg = d.getArrayItem(0);
    if (pg.isInteger()) v = Value(static_cast<double>(pg.getIntValue() + 1));
    for (size_t i = 0; i < ev.pages.size() && pg.isIndirect(); ++i) {
      if (ev.pages[i].obj.isIndirect() && ev.pages[i].obj.getObjGen() == pg.getObjGen()) v = Value(static_cast<double>(i + 1));
    }
  } else if (st == "zoom" && d.isArray() && d.getArrayNItems() >= 2) {
    std::string mode = str(d.getArrayItem(1));
    v = Value(mode);
    if (mode == "XYZ" && d.getArrayNItems() >= 5 && d.getArrayItem(4).isNumber() && d.getArrayItem(4).getNumericValue() > 0) {
      v = Value(d.getArrayItem(4).getNumericValue() * 100);
    }
  } else if (st == "show_title") {
    v = boolText(vp.isDictionary() && vp.getKey("/DisplayDocTitle").isBool() && vp.getKey("/DisplayDocTitle").getBoolValue());
  } else if (st == "viewer_preferences" && vp.isDictionary()) {
    std::string all;
    for (const std::string& k : vp.getKeys()) all += (all.empty() ? "" : ",") + k.substr(1);
    v = Value(all);
  } else if (st == "print_scaling" && vp.isDictionary()) {
    v = textOf(vp.getKey("/PrintScaling"));
  } else if (st == "clip_to" && vp.isDictionary()) {
    v = textOf(vp.getKey("/ViewClip"));
  }
  return compare(v, a);
}

int pageRotation(const PageFacts& p) {
  int r = p.obj.isDictionary() ? static_cast<int>(numOf(p.obj.getKey("/Rotate"), 0)) : 0;
  return ((r % 360) + 360) % 360;
}

int evDocumentPagesUniform(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string &aspect = param(a, "aspect"), &box = param(a, "box");
  double tol = paramNum(a, "tolerance");
  bool same = true;
  auto dims = [&](const PageFacts& p, double& w, double& h) {
    const Box& b = effBox(p, box);
    w = b.x1 - b.x0;
    h = b.y1 - b.y0;
    if (pageRotation(p) % 180 == 90) std::swap(w, h);
  };
  double w0 = 0, h0 = 0;
  for (size_t i = 0; i < ev.pages.size(); ++i) {
    double w = 0, h = 0;
    dims(ev.pages[i], w, h);
    if (i == 0) {
      w0 = w, h0 = h;
      continue;
    }
    bool sizeSame = (std::fabs(w - w0) <= tol && std::fabs(h - h0) <= tol) || (std::fabs(w - h0) <= tol && std::fabs(h - w0) <= tol);
    bool orientSame = (w > h + tol) == (w0 > h0 + tol) && (h > w + tol) == (h0 > w0 + tol);
    if (aspect != "orientation" && !sizeSame) same = false;
    if (aspect != "size" && !orientSame) same = false;
  }
  return compare(Value(same), a);
}

int evDocumentPortfolio(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  return compare(Value(ev.root.isDictionary() && ev.root.getKey("/Collection").isDictionary()), a);
}

Strs filterNames(QPDFObjectHandle d) {
  Strs v;
  QPDFObjectHandle f = d.isDictionary() ? d.getKey("/Filter") : QPDFObjectHandle();
  if (f.isName()) v.push_back(f.getName().substr(1));
  for (int i = 0; f.isArray() && i < f.getArrayNItems(); ++i) {
    if (f.getArrayItem(i).isName()) v.push_back(f.getArrayItem(i).getName().substr(1));
  }
  return v;
}

int evDocumentStreamCompression(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::set<std::string> out;
  for (QPDFObjectHandle o : allObjects(ev)) {
    if (!o.isStream()) continue;
    Strs f = filterNames(o.getDict());
    if (f.empty()) out.insert("none");
    out.insert(f.begin(), f.end());
  }
  return compare(listOf(out), a);
}

int evDocumentStreamLayout(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  if (!ev.syntaxScanned && (asks(a, "wrong_length") || asks(a, "keyword_line_ends"))) {
    supported = false;
    return 0;
  }
  std::set<std::string> out;
  if (ev.syntaxIssues.count("streamLength")) out.insert("wrong_length");
  if (ev.syntaxIssues.count("streamKeyword")) out.insert("keyword_line_ends");
  for (QPDFObjectHandle o : allObjects(ev)) {
    if (!o.isStream()) continue;
    QPDFObjectHandle d = o.getDict();
    if (d.hasKey("/F")) out.insert("external_file");
    if (d.hasKey("/FFilter")) out.insert("external_filter");
    if (d.hasKey("/FDecodeParms")) out.insert("external_decode_parms");
    if (d.hasKey("/DL")) out.insert("has_dl_entry");
  }
  const Events::ObjStats& st = objStats(ev);
  if (st.dlBad) out.insert("dl_inaccurate");
  if (st.cryptFilter) out.insert("uses_crypt_filter");
  return compare(listOf(out), a);
}

int evDocumentTrailer(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle t = ev.trailer;
  std::set<std::string> out;
  if (!t.isDictionary() || !t.hasKey("/Size")) out.insert("missing_size");
  else if (!t.getKey("/Size").isInteger()) out.insert("size_not_integer");
  if (!t.isDictionary() || !t.getKey("/Root").isDictionary()) out.insert("missing_root");
  if (t.isDictionary() && t.hasKey("/Info")) out.insert("has_info");
  if (t.isDictionary() && t.hasKey("/Encrypt")) out.insert("encrypted");
  return compare(listOf(out), a);
}

int evDocumentUsesObjectStreams(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  if (paramBool(a, "except_structure")) return compare(Value(objStats(ev).compressedNonTag), a);
  bool os = false;
  for (QPDFObjectHandle o : allObjects(ev)) {
    if (o.isStream() && nameIs(o.getDict().getKey("/Type"), "/ObjStm")) {
      os = true;
      break;
    }
  }
  return compare(Value(os), a);
}

int evDocumentVersionConflict(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::string v = ev.root.isDictionary() ? str(ev.root.getKey("/Version")) : std::string();
  return compare(Value(!v.empty() && versionNumber(v) > versionNumber(ev.header)), a);
}

int evEinvoiceMetadata(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::string x = lowerAscii(xmpOf(ev.root));
  return compare(Value(x.find("zugferd") != std::string::npos || x.find("factur-x") != std::string::npos || x.find("urn:ferd") != std::string::npos), a);
}

int evEinvoicePresent(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  static const std::set<std::string> names = {"factur-x.xml", "zugferd-invoice.xml", "xrechnung.xml", "order-x.xml"};
  int n = 0;
  for (QPDFObjectHandle fs : attachments(ev).unique) n += names.count(lowerAscii(fileName(fs))) > 0;
  return compare(Value(n), a);
}

struct FontParts {
  QPDFObjectHandle d, desc, cid;
};

FontParts fontParts(const FontFacts& f) {
  FontParts fp;
  fp.d = f.dict;
  if (!fp.d.isDictionary()) return fp;
  fp.desc = fp.d.getKey("/FontDescriptor");
  if (nameIs(fp.d.getKey("/Subtype"), "/Type0")) {
    QPDFObjectHandle dfs = fp.d.getKey("/DescendantFonts");
    if (dfs.isArray() && dfs.getArrayNItems()) fp.cid = dfs.getArrayItem(0);
    if (fp.cid.isDictionary()) fp.desc = fp.cid.getKey("/FontDescriptor");
  }
  return fp;
}

int evFontEncoding(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.font || !s.font->dict.isDictionary()) return 0;
  QPDFObjectHandle d = s.font->dict;
  QPDFObjectHandle e = d.getKey("/Encoding");
  std::string v;
  if (nameIs(d.getKey("/Subtype"), "/Type0")) {
    if (nameIs(e, "/Identity-H")) v = "identity_h";
    else if (nameIs(e, "/Identity-V")) v = "identity_v";
    else if (e.isName()) v = "cmap_predefined";
    else if (e.isStream()) v = "cmap_embedded";
    else return 0;
  } else {
    static const std::map<std::string, std::string> m = {{"/StandardEncoding", "standard"}, {"/WinAnsiEncoding", "win_ansi"},
                                                         {"/MacRomanEncoding", "mac_roman"}, {"/MacExpertEncoding", "mac_expert"}};
    QPDFObjectHandle base = e.isDictionary() ? e.getKey("/BaseEncoding") : e;
    if (e.isDictionary() && e.getKey("/Differences").isArray()) v = "custom_differences";
    else if (base.isName() && m.count(base.getName())) v = m.at(base.getName());
    else v = "builtin";
  }
  return compare(Value(v), a);
}

int evFontFlags(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.font) return 0;
  FontParts fp = fontParts(*s.font);
  if (!fp.desc.isDictionary() || !fp.desc.getKey("/Flags").isInteger()) return compare(Value(Strs()), a);
  long long fl = fp.desc.getKey("/Flags").getIntValue();
  static const std::pair<int, const char*> bits[] = {{1, "fixed_pitch"}, {2, "serif"}, {3, "symbolic"}, {4, "script"}, {6, "nonsymbolic"},
                                                     {7, "italic"}, {17, "all_cap"}, {18, "small_cap"}, {19, "force_bold"}};
  Strs out;
  for (auto& b : bits) {
    if ((fl >> (b.first - 1)) & 1) out.push_back(b.second);
  }
  return compare(Value(out), a);
}

int evFontHasToUnicode(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.font || !s.font->dict.isDictionary()) return 0;
  return compare(Value(s.font->dict.getKey("/ToUnicode").isStream()), a);
}

int evFontMetric(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.font) return 0;
  const FontFacts& f = *s.font;
  FontParts fp = fontParts(f);
  const std::string& m = param(a, "metric");
  Value v;
  static const std::map<std::string, const char*> descKeys = {{"x_height", "/XHeight"}, {"stem_h", "/StemH"}, {"avg_width", "/AvgWidth"}, {"max_width", "/MaxWidth"}};
  static const std::map<std::string, int> bboxIdx = {{"bbox_left", 0}, {"bbox_bottom", 1}, {"bbox_right", 2}, {"bbox_top", 3}};
  if (descKeys.count(m)) {
    if (fp.desc.isDictionary()) v = Value(numOf(fp.desc.getKey(descKeys.at(m)), 0));
  } else if (bboxIdx.count(m)) {
    QPDFObjectHandle bb = fp.desc.isDictionary() ? fp.desc.getKey("/FontBBox") : fp.d.getKey("/FontBBox");
    if (bb.isArray() && bb.getArrayNItems() == 4) v = Value(numOf(bb.getArrayItem(bboxIdx.at(m)), 0));
  } else if (m == "largest_cid") {
    std::string enc = nameOf(fp.d.getKey("/Encoding"));
    if (f.cid && (enc == "/Identity-H" || enc == "/Identity-V")) {
      int best = -1;
      for (const TextEvent& te : ev.texts) {
        if (te.fontOg != f.og) continue;
        for (size_t i = 0; i + 1 < te.bytes.size(); i += 2) {
          best = std::max(best, (static_cast<unsigned char>(te.bytes[i]) << 8) | static_cast<unsigned char>(te.bytes[i + 1]));
        }
      }
      if (best >= 0) v = Value(best);
    }
  }
  return compare(v, a);
}

const std::set<std::string>& standardCMaps() {
  static const std::set<std::string> s = {
      "GB-EUC-H", "GB-EUC-V", "GBpc-EUC-H", "GBpc-EUC-V", "GBK-EUC-H", "GBK-EUC-V", "GBKp-EUC-H", "GBKp-EUC-V", "GBK2K-H",
      "GBK2K-V", "UniGB-UCS2-H", "UniGB-UCS2-V", "UniGB-UTF16-H", "UniGB-UTF16-V", "B5pc-H", "B5pc-V", "HKscs-B5-H",
      "HKscs-B5-V", "ETen-B5-H", "ETen-B5-V", "ETenms-B5-H", "ETenms-B5-V", "CNS-EUC-H", "CNS-EUC-V", "UniCNS-UCS2-H",
      "UniCNS-UCS2-V", "UniCNS-UTF16-H", "UniCNS-UTF16-V", "83pv-RKSJ-H", "90ms-RKSJ-H", "90ms-RKSJ-V", "90msp-RKSJ-H",
      "90msp-RKSJ-V", "90pv-RKSJ-H", "Add-RKSJ-H", "Add-RKSJ-V", "EUC-H", "EUC-V", "Ext-RKSJ-H", "Ext-RKSJ-V", "H", "V",
      "UniJIS-UCS2-H", "UniJIS-UCS2-V", "UniJIS-UCS2-HW-H", "UniJIS-UCS2-HW-V", "UniJIS-UTF16-H", "UniJIS-UTF16-V",
      "KSC-EUC-H", "KSC-EUC-V", "KSCms-UHC-H", "KSCms-UHC-V", "KSCms-UHC-HW-H", "KSCms-UHC-HW-V", "KSCpc-EUC-H",
      "UniKS-UCS2-H", "UniKS-UCS2-V", "UniKS-UTF16-H", "UniKS-UTF16-V", "Identity-H", "Identity-V"};
  return s;
}

bool cmapSyntaxGood(const std::string& t, bool requireCodespace) {
  if (t.find("begincmap") == std::string::npos || t.find("endcmap") == std::string::npos) return false;
  if (requireCodespace && t.find("begincodespacerange") == std::string::npos) return false;
  for (const char* blk : {"codespacerange", "bfchar", "bfrange", "cidchar", "cidrange", "notdefchar", "notdefrange"}) {
    std::string b = std::string("begin") + blk, e = std::string("end") + blk;
    size_t pos = 0;
    while ((pos = t.find(b, pos)) != std::string::npos) {
      size_t ns = t.find_last_not_of(" \t\r\n", pos == 0 ? 0 : pos - 1);
      size_t numStart = ns == std::string::npos ? 0 : t.find_last_of(" \t\r\n", ns);
      long declared = ns == std::string::npos ? -1 : std::strtol(t.c_str() + (numStart == std::string::npos ? 0 : numStart + 1), nullptr, 10);
      size_t end = t.find(e, pos + b.size());
      if (end == std::string::npos) return false;
      size_t nextBegin = t.find("begin", pos + b.size());
      if (nextBegin != std::string::npos && nextBegin < end) return false;
      std::string body = t.substr(pos + b.size(), end - pos - b.size());
      int hexes = 0, items = 0;
      for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '<') {
          size_t c = body.find('>', i);
          if (c == std::string::npos) return false;
          int digits = 0;
          for (size_t k = i + 1; k < c; ++k) {
            if (std::isxdigit(static_cast<unsigned char>(body[k]))) ++digits;
            else if (!std::isspace(static_cast<unsigned char>(body[k]))) return false;
          }
          if (digits % 2) return false;
          ++hexes;
          i = c;
        } else if (body[i] == '[') {
          size_t c = body.find(']', i);
          if (c == std::string::npos) return false;
          ++items;
          i = c;
        } else if (std::isdigit(static_cast<unsigned char>(body[i])) || body[i] == '/') {
          ++items;
          while (i + 1 < body.size() && !std::isspace(static_cast<unsigned char>(body[i + 1])) && body[i + 1] != '<') ++i;
        }
      }
      int per = std::string(blk).find("range") != std::string::npos ? 3 : 2;
      if (std::string(blk) == "codespacerange") per = 2;
      if (declared >= 0 && declared <= 100 && hexes + items != declared * per) return false;
      pos = end;
    }
  }
  return true;
}

void type3ResourceNames(QPDFObjectHandle cp, std::map<std::string, std::set<std::string>>& used) {
  struct Cb : QPDFObjectHandle::ParserCallbacks {
    std::map<std::string, std::set<std::string>>& u;
    std::vector<QPDFObjectHandle> args;
    explicit Cb(std::map<std::string, std::set<std::string>>& x) : u(x) {}
    void handleObject(QPDFObjectHandle o, size_t, size_t) override {
      if (!o.isOperator()) {
        args.push_back(o);
        return;
      }
      std::string op = o.getOperatorValue();
      auto nm = [&](size_t i) { return i < args.size() && args[i].isName() ? args[i].getName() : std::string(); };
      static const std::set<std::string> dev = {"/DeviceGray", "/DeviceRGB", "/DeviceCMYK", "/G", "/RGB", "/CMYK", "/Pattern"};
      if (op == "Tf") u["/Font"].insert(nm(0));
      else if (op == "Do") u["/XObject"].insert(nm(0));
      else if (op == "gs") u["/ExtGState"].insert(nm(0));
      else if (op == "sh") u["/Shading"].insert(nm(0));
      else if ((op == "cs" || op == "CS") && !dev.count(nm(0))) u["/ColorSpace"].insert(nm(0));
      else if ((op == "scn" || op == "SCN") && !args.empty() && args.back().isName()) u["/Pattern"].insert(args.back().getName());
      else if (op == "BDC" && args.size() >= 2 && args[1].isName()) u["/Properties"].insert(args[1].getName());
      args.clear();
    }
    void handleEOF() override {}
  } cb(used);
  try {
    QPDFObjectHandle::parseContentStream(cp, &cb);
  } catch (...) {
  }
  for (auto& kv : used) kv.second.erase(std::string());
}

bool cidSetComplete(const FontFacts& f, QPDFObjectHandle cidFont, QPDFObjectHandle cs) {
  FtLib lib;
  FtFace face;
  face.data = f.fontProgram;
  if (face.data.empty() || !lib.lib ||
      FT_New_Memory_Face(lib.lib, reinterpret_cast<const FT_Byte*>(face.data.data()), static_cast<FT_Long>(face.data.size()), 0, &face.face) != 0) {
    return true;
  }
  std::string bits = streamBytes(cs);
  auto has = [&](uint32_t cid) { return cid / 8 < bits.size() && (static_cast<unsigned char>(bits[cid / 8]) & (0x80 >> (cid % 8))); };
  QPDFObjectHandle map = cidFont.isDictionary() ? cidFont.getKey("/CIDToGIDMap") : QPDFObjectHandle();
  if (f.cid0 && FT_IS_CID_KEYED(face.face)) {
    for (FT_Long g = 0; g < face.face->num_glyphs; ++g) {
      FT_UInt cid = 0;
      if (FT_Get_CID_From_Glyph_Index(face.face, static_cast<FT_UInt>(g), &cid) == 0 && !has(cid)) return false;
    }
  } else if (map.isStream()) {
    std::string m = streamBytes(map);
    for (size_t c = 0; 2 * c + 1 < m.size(); ++c) {
      unsigned gid = (static_cast<unsigned char>(m[2 * c]) << 8) | static_cast<unsigned char>(m[2 * c + 1]);
      if ((gid != 0 || c == 0) && gid < static_cast<unsigned>(face.face->num_glyphs) && !has(static_cast<uint32_t>(c))) return false;
    }
  } else {
    for (FT_Long g = 0; g < face.face->num_glyphs; ++g) {
      if (!has(static_cast<uint32_t>(g))) return false;
    }
  }
  return true;
}

bool charSetComplete(const FontFacts& f, QPDFObjectHandle cs) {
  FtLib lib;
  FtFace face;
  face.data = f.fontProgram;
  if (face.data.empty() || !lib.lib ||
      FT_New_Memory_Face(lib.lib, reinterpret_cast<const FT_Byte*>(face.data.data()), static_cast<FT_Long>(face.data.size()), 0, &face.face) != 0 ||
      !FT_HAS_GLYPH_NAMES(face.face)) {
    return true;
  }
  std::set<std::string> listed, present;
  std::string s = cs.getStringValue();
  for (size_t i = 0; i < s.size();) {
    if (s[i] != '/') {
      ++i;
      continue;
    }
    size_t e = s.find_first_of("/ \t\r\n", i + 1);
    listed.insert(s.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1));
    i = e == std::string::npos ? s.size() : e;
  }
  for (FT_Long g = 0; g < face.face->num_glyphs; ++g) {
    char buf[128] = {0};
    if (FT_Get_Glyph_Name(face.face, static_cast<FT_UInt>(g), buf, sizeof buf) == 0 && buf[0]) present.insert(buf);
  }
  listed.erase(".notdef");
  present.erase(".notdef");
  return listed == present;
}

bool programInvalidStrict(const FontFacts& f) {
  FtLib lib;
  FtFace face;
  face.data = f.fontProgram;
  if (face.data.empty() || !lib.lib ||
      FT_New_Memory_Face(lib.lib, reinterpret_cast<const FT_Byte*>(face.data.data()), static_cast<FT_Long>(face.data.size()), 0, &face.face) != 0) {
    return true;
  }
  if (FT_IS_SFNT(face.face) && !f.openType && f.trueType) {
    for (const char* tag : {"head", "hhea", "hmtx", "loca", "glyf", "maxp"}) {
      FT_ULong len = 0;
      if (FT_Load_Sfnt_Table(face.face, FT_MAKE_TAG(tag[0], tag[1], tag[2], tag[3]), 0, nullptr, &len) != 0 || len == 0) return true;
    }
  }
  for (FT_Long g = 0; g < face.face->num_glyphs && g < 6000; ++g) {
    if (FT_Load_Glyph(face.face, static_cast<FT_UInt>(g), FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) return true;
  }
  return false;
}

int evFontProblem(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.font || !s.font->dict.isDictionary()) return 0;
  const FontFacts& f = *s.font;
  static const std::set<std::string> programOnly = {"truetype_errors", "glyph_overlapping_paths",
                                                    "identical_glyphs_for_different_chars"};
  bool wantProgram = a.op == "count";
  for (const std::string& v : programOnly) wantProgram = wantProgram || asks(a, v.c_str());
  FontParts fp = fontParts(f);
  QPDFObjectHandle d = fp.d, desc = fp.desc, cid = fp.cid;
  std::string sub = nameOf(d.getKey("/Subtype"));
  bool simpleTT = sub == "/TrueType";
  bool type1 = sub == "/Type1" || sub == "/MMType1";
  std::set<std::string> out;
  if (f.embedded && !f.type3 && (paramBool(a, "strict") ? programInvalidStrict(f) : !f.ftValid)) out.insert("invalid_program");
  if (f.embedded && f.anyUndefinedGlyph) out.insert("missing_glyph");
  if (f.embedded && f.subsetName && f.anyUndefinedGlyph) out.insert("subset_missing_glyph");
  if (f.anyWidthMismatch) out.insert("width_mismatch");
  static const std::set<std::string> std14 = {"Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic", "Helvetica",
                                              "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique", "Courier",
                                              "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique", "Symbol", "ZapfDingbats"};
  if ((type1 || simpleTT || f.type3) && !d.getKey("/Widths").isArray() && !std14.count(str(d.getKey("/BaseFont")))) out.insert("metrics_missing");
  QPDFObjectHandle bb = desc.isDictionary() ? desc.getKey("/FontBBox") : QPDFObjectHandle();
  if (f.embedded && bb.isArray() && bb.getArrayNItems() == 4 &&
      (numOf(bb.getArrayItem(0), 0) == numOf(bb.getArrayItem(2), 0) || numOf(bb.getArrayItem(1), 0) == numOf(bb.getArrayItem(3), 0))) {
    out.insert("empty_bbox");
  }
  QPDFObjectHandle enc = d.getKey("/Encoding");
  QPDFObjectHandle diffs = enc.isDictionary() ? enc.getKey("/Differences") : QPDFObjectHandle();
  for (int i = 0; !f.cid && diffs.isArray() && i < diffs.getArrayNItems(); ++i) {
    QPDFObjectHandle it = diffs.getArrayItem(i);
    if (it.isName() && it.getName() != "/.notdef" && !aglNameToUnicode(it.getName().substr(1))) out.insert("glyph_names_not_in_standard_list");
  }
  if (f.cid && f.embedded && f.subsetName) {
    QPDFObjectHandle cs = desc.isDictionary() ? desc.getKey("/CIDSet") : QPDFObjectHandle();
    if (!cs.isStream()) out.insert("cidset_missing");
    else if (!cidSetComplete(f, cid, cs)) out.insert("cidset_incomplete");
  }
  if (type1 && f.embedded && f.subsetName) {
    QPDFObjectHandle cs = desc.isDictionary() ? desc.getKey("/CharSet") : QPDFObjectHandle();
    if (!cs.isString()) out.insert("charset_missing");
    else if (!charSetComplete(f, cs)) out.insert("charset_incomplete");
  }
  if (f.cid && f.trueType && f.embedded && !f.hasCIDToGIDMap) out.insert("cidtogidmap_missing");
  if (sub == "/Type0") {
    QPDFObjectHandle c1 = cid.isDictionary() ? cid.getKey("/CIDSystemInfo") : QPDFObjectHandle();
    QPDFObjectHandle c2 = enc.isStream() ? enc.getDict().getKey("/CIDSystemInfo") : QPDFObjectHandle();
    if (c1.isDictionary() && c2.isDictionary()) {
      bool reg = str(c1.getKey("/Registry")) == str(c2.getKey("/Registry"));
      bool ord = str(c1.getKey("/Ordering")) == str(c2.getKey("/Ordering"));
      if (!reg) out.insert("cid_registry_mismatch");
      if (!ord) out.insert("cid_ordering_mismatch");
      if (!reg || !ord) out.insert("cid_system_info_mismatch");
      if (numOf(c1.getKey("/Supplement"), 0) < numOf(c2.getKey("/Supplement"), 0)) out.insert("cid_supplement_too_low");
    }
    if (enc.isStream()) {
      int dictW = static_cast<int>(numOf(enc.getDict().getKey("/WMode"), 0));
      std::string data = streamBytes(enc);
      size_t wp = data.find("/WMode");
      int dataW = wp == std::string::npos ? 0 : std::atoi(data.c_str() + wp + 6);
      if (dictW != dataW) out.insert("wmode_mismatch");
      QPDFObjectHandle cd = enc.getDict();
      bool ok = cd.getKey("/CMapName").isName() && cd.getKey("/CIDSystemInfo").isDictionary() &&
                (!cd.hasKey("/Type") || nameIs(cd.getKey("/Type"), "/CMap")) && cmapSyntaxGood(data, true) &&
                (!cd.hasKey("/WMode") || cd.getKey("/WMode").isInteger());
      if (!ok) out.insert("cmap_bad_syntax");
      QPDFObjectHandle use = cd.getKey("/UseCMap");
      if (use.isName() && !standardCMaps().count(use.getName().substr(1))) out.insert("cmap_references_unknown");
    } else if (enc.isName() && !standardCMaps().count(enc.getName().substr(1))) {
      out.insert("cmap_not_embedded");
    }
  }
  QPDFObjectHandle tu = d.getKey("/ToUnicode");
  if (tu.isStream() && !cmapSyntaxGood(streamBytes(tu), false)) out.insert("to_unicode_bad_syntax");
  bool sfnt = desc.isDictionary() && (desc.getKey("/FontFile2").isStream() || (f.openType && f.fontProgram.rfind("OTTO", 0) != 0));
  if (simpleTT && !f.cid && f.embedded && f.hasFlags) {
    int count = 0;
    bool has30 = sfnt && sfntHasCmap(f.fontProgram, 3, 0, count);
    int c31 = 0;
    bool has31 = sfnt && sfntHasCmap(f.fontProgram, 3, 1, c31);
    if (f.symbolic) {
      if (f.hasEncodingDict || !f.encodingName.empty()) out.insert("symbolic_truetype_has_encoding");
      if (sfnt && count > 1) out.insert("symbolic_truetype_cmap_count");
      if (sfnt && !has30) out.insert("symbolic_truetype_no_3_0_cmap");
    } else {
      bool ok = f.encodingName == "MacRomanEncoding" || f.encodingName == "WinAnsiEncoding" || f.encodingName == "Differences";
      if (!ok) out.insert("nonsymbolic_truetype_bad_encoding");
      if (enc.isDictionary() && enc.hasKey("/Differences")) out.insert("nonsymbolic_truetype_differences");
      if (sfnt && !has31) out.insert("nonsymbolic_truetype_no_3_1_cmap");
    }
  }
  if (f.ftLoaded && f.cmapCount > 1) out.insert("cmap_subtables");
  if (wantProgram) {
    for (const std::string& v : fontProgramProblems(f, ev, paramBool(a, "strict"))) {
      if (programOnly.count(v)) out.insert(v);
    }
  }
  if (f.type3) {
    std::map<std::string, std::set<std::string>> used;
    QPDFObjectHandle cps = d.getKey("/CharProcs");
    for (const std::string& k : cps.isDictionary() ? cps.getKeys() : std::set<std::string>()) {
      if (cps.getKey(k).isStream()) type3ResourceNames(cps.getKey(k), used);
    }
    QPDFObjectHandle res = d.getKey("/Resources");
    for (const auto& kv : used) {
      for (const std::string& n : kv.second) {
        QPDFObjectHandle cat = res.isDictionary() ? res.getKey(kv.first) : QPDFObjectHandle();
        if (!cat.isDictionary() || !cat.hasKey(n)) out.insert("type3_missing_resources");
      }
    }
  }
  std::string x = desc.isDictionary() ? streamBytes(desc.getKey("/Metadata")) : std::string();
  if (!x.empty()) {
    out.insert("has_xmp");
    for (const char* k : {"xmp:Title", "xmpRights:Copyright", "xmpRights:Marked", "xmpRights:Owner", "xmpRights:UsageTerms"}) {
      if (x.find(k) == std::string::npos) out.insert("font_xmp_missing_fields");
    }
  }
  return compare(listOf(out), a);
}

int evFontType(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.font || !s.font->dict.isDictionary()) return 0;
  const FontFacts& f = *s.font;
  FontParts fp = fontParts(f);
  std::string sub = nameOf(fp.d.getKey("/Subtype"));
  std::string cidSub = fp.cid.isDictionary() ? nameOf(fp.cid.getKey("/Subtype")) : std::string();
  QPDFObjectHandle ff3 = fp.desc.isDictionary() ? fp.desc.getKey("/FontFile3") : QPDFObjectHandle();
  std::string ff3sub = ff3.isStream() ? nameOf(ff3.getDict().getKey("/Subtype")) : std::string();
  static const std::set<std::string> std14 = {"Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic", "Helvetica",
                                              "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique", "Courier",
                                              "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique", "Symbol", "ZapfDingbats"};
  std::string v;
  if (ff3sub == "/OpenType") v = f.fontProgram.rfind("OTTO", 0) == 0 ? "opentype_cff" : "opentype_truetype";
  else if (sub == "/Type3") v = "type3";
  else if (sub == "/Type0") v = cidSub == "/CIDFontType2" ? "cid_truetype" : "cid_type0";
  else if (sub == "/MMType1") v = "multiple_master";
  else if (sub == "/TrueType") v = "truetype";
  else if (sub == "/Type1") v = !f.embedded && std14.count(str(fp.d.getKey("/BaseFont"))) ? "standard_14" : ff3.isStream() ? "cff" : "type1";
  else return 0;
  return compare(Value(v), a);
}

bool widgetField(const Subject& s, QPDFObjectHandle& o) {
  if (!s.annot || !isWidget(*s.annot) || !s.annot->obj.isDictionary()) return false;
  o = s.annot->obj;
  return true;
}

int evFormFieldFlags(const PfAtom& a, const Subject& s, const Events&, bool&) {
  QPDFObjectHandle o;
  if (!widgetField(s, o)) return 0;
  QPDFObjectHandle ff = inheritedField(o, "/Ff");
  static const std::pair<int, const char*> bits[] = {{1, "read_only"}, {2, "required"}, {3, "no_export"}, {13, "multiline"}, {14, "password"},
                                                     {15, "no_toggle_to_off"}, {16, "radio"}, {17, "push_button"}, {18, "combo"}, {19, "edit"},
                                                     {20, "sort"}, {21, "file_select"}, {22, "multi_select"}, {23, "do_not_spell_check"},
                                                     {24, "do_not_scroll"}, {25, "comb"}, {26, "radios_in_unison"}, {27, "commit_on_change"}};
  Strs out;
  if (!ff.isInteger()) out.push_back("missing");
  long long v = ff.isInteger() ? ff.getIntValue() : 0;
  for (auto& b : bits) {
    if ((v >> (b.first - 1)) & 1) out.push_back(b.second);
  }
  return compare(Value(out), a);
}

int evFormFieldName(const PfAtom& a, const Subject& s, const Events&, bool&) {
  QPDFObjectHandle o;
  if (!widgetField(s, o)) return 0;
  const std::string& w = param(a, "which");
  Value v;
  if (w == "full") {
    std::string n = fullFieldName(o);
    if (!n.empty()) v = Value(n);
  } else if (w == "partial") {
    v = textOf(fieldRoot(o).getKey("/T"));
  } else if (w == "alternate") {
    v = textOf(inheritedField(o, "/TU"));
  } else if (w == "export") {
    v = textOf(inheritedField(o, "/TM"));
  }
  return compare(v, a);
}

int evFormFieldType(const PfAtom& a, const Subject& s, const Events&, bool&) {
  QPDFObjectHandle o;
  if (!widgetField(s, o)) return 0;
  std::string ft = str(inheritedField(o, "/FT"));
  long long ff = inheritedField(o, "/Ff").isInteger() ? inheritedField(o, "/Ff").getIntValue() : 0;
  std::string v;
  if (ft == "Btn") v = (ff & (1 << 16)) ? "push_button" : (ff & (1 << 15)) ? "radio" : "check_box";
  else if (ft == "Ch") v = (ff & (1 << 17)) ? "combo" : "list";
  else if (ft == "Tx") v = "text";
  else if (ft == "Sig") v = "signature";
  else return 0;
  return compare(Value(v), a);
}

int evFormFieldValue(const PfAtom& a, const Subject& s, const Events&, bool&) {
  QPDFObjectHandle o;
  if (!widgetField(s, o)) return 0;
  QPDFObjectHandle v = inheritedField(o, "/V");
  if (v.isArray()) {
    Strs out;
    for (int i = 0; i < v.getArrayNItems(); ++i) {
      if (v.getArrayItem(i).isString() || v.getArrayItem(i).isName()) out.push_back(str(v.getArrayItem(i)));
    }
    return compare(orAbsent(out), a);
  }
  return compare(textOf(v), a);
}

int evFormXfa(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  if (!ev.root.isDictionary()) return 0;
  QPDFObjectHandle af = ev.root.getKey("/AcroForm");
  QPDFObjectHandle nr = ev.root.getKey("/NeedsRendering");
  std::string v = "none";
  if (af.isDictionary() && af.hasKey("/XFA")) v = nr.isBool() && nr.getBoolValue() ? "dynamic" : "static";
  return compare(Value(v), a);
}

uint32_t be32(const std::string& p, size_t o) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(p[o])) << 24) | (static_cast<uint32_t>(static_cast<unsigned char>(p[o + 1])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[o + 2])) << 8) | static_cast<unsigned char>(p[o + 3]);
}

std::string trimSig(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
  return s;
}

Value iccHeaderField(const std::string& p, const std::string& f) {
  if (p.size() < 128) return Value();
  auto sig = [&](size_t o) { return Value(trimSig(p.substr(o, 4))); };
  auto fixed = [&](size_t o) { return Value(static_cast<int32_t>(be32(p, o)) / 65536.0); };
  if (f == "version") {
    return Value(std::to_string(static_cast<unsigned char>(p[8])) + "." + std::to_string(static_cast<unsigned char>(p[9]) >> 4));
  }
  if (f == "colour_space") return sig(16);
  if (f == "device_class") return sig(12);
  if (f == "pcs") return sig(20);
  if (f == "rendering_intent") return Value(be32(p, 64));
  if (f == "size") return Value(be32(p, 0));
  if (f == "cmm") return sig(4);
  if (f == "creator") return sig(80);
  if (f == "manufacturer") return sig(48);
  if (f == "model") return sig(52);
  if (f == "platform") return sig(40);
  if (f == "flags") return Value(be32(p, 44));
  if (f == "attributes") {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%08x%08x", be32(p, 56), be32(p, 60));
    return Value(std::string(buf));
  }
  if (f == "date") {
    auto u16 = [&](size_t o) { return (static_cast<unsigned char>(p[o]) << 8) | static_cast<unsigned char>(p[o + 1]); };
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d", u16(24), u16(26), u16(28), u16(30), u16(32), u16(34));
    return Value(std::string(buf));
  }
  if (f == "profile_id") {
    std::string id;
    for (int i = 84; i < 100; ++i) {
      char h[3];
      std::snprintf(h, sizeof h, "%02x", static_cast<unsigned char>(p[i]));
      id += h;
    }
    return Value(id);
  }
  if (f == "illuminant_x") return fixed(68);
  if (f == "illuminant_y") return fixed(72);
  if (f == "illuminant_z") return fixed(76);
  if (f == "signature_valid") return boolText(p.compare(36, 4, "acsp") == 0);
  return Value();
}

Value iccTagText(const std::string& p, const std::string& tag) {
  static const std::map<std::string, const char*> sigs = {
      {"description", "desc"}, {"copyright", "cprt"}, {"calibration_date", "calt"}, {"device_manufacturer", "dmnd"},
      {"device_model", "dmdd"}, {"technology", "tech"}, {"viewing_conditions", "vued"}, {"screening", "scrd"},
      {"rendering_info", "crdi"}, {"named_colours", "ncl2"}};
  auto it = sigs.find(tag);
  if (it == sigs.end() || p.size() < 132) return Value();
  uint32_t n = be32(p, 128);
  for (uint32_t i = 0; i < n && 132 + i * 12 + 12 <= p.size(); ++i) {
    if (p.compare(132 + i * 12, 4, it->second) != 0) continue;
    uint32_t off = be32(p, 132 + i * 12 + 4), len = be32(p, 132 + i * 12 + 8);
    if (static_cast<size_t>(off) + len > p.size() || len < 12) return Value(std::string());
    std::string type = p.substr(off, 4), txt;
    if (type == "desc") {
      txt = p.substr(off + 12, std::min<uint32_t>(be32(p, off + 8), len - 12));
    } else if (type == "text") {
      txt = p.substr(off + 8, len - 8);
    } else if (type == "mluc" && len >= 28) {
      uint32_t l = be32(p, off + 20), o = be32(p, off + 24);
      for (uint32_t k = 0; k + 1 < l && off + o + k + 1 < p.size(); k += 2) txt += p[off + o + k + 1];
    } else if (type == "sig ") {
      txt = p.substr(off + 8, 4);
    } else if (type == "dtim" && len >= 20) {
      auto u16 = [&](size_t q) { return (static_cast<unsigned char>(p[q]) << 8) | static_cast<unsigned char>(p[q + 1]); };
      char buf[32];
      std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d", u16(off + 8), u16(off + 10), u16(off + 12), u16(off + 14), u16(off + 16), u16(off + 18));
      txt = buf;
    }
    while (!txt.empty() && txt.back() == '\0') txt.pop_back();
    return Value(txt);
  }
  return Value();
}

std::set<QPDFObjGen> outputProfileObjs(const Events& ev) {
  std::set<QPDFObjGen> out;
  auto take = [&](QPDFObjectHandle ois) {
    for (int i = 0; ois.isArray() && i < ois.getArrayNItems(); ++i) {
      QPDFObjectHandle pr = ois.getArrayItem(i).getKey("/DestOutputProfile");
      if (pr.isIndirect()) out.insert(pr.getObjGen());
    }
  };
  if (ev.root.isDictionary()) take(ev.root.getKey("/OutputIntents"));
  for (const PageFacts& p : ev.pages) {
    if (p.obj.isDictionary()) take(p.obj.getKey("/OutputIntents"));
  }
  return out;
}

std::string oiSubtype(const std::string& which) {
  if (which == "output_intent_pdfa" || which == "pdfa") return "/GTS_PDFA1";
  if (which == "output_intent_pdfe" || which == "pdfe") return "/ISO_PDFE1";
  if (which == "any" || which == "destination") return std::string();
  return "/GTS_PDFX";
}

std::vector<std::string> iccProfiles(const Events& ev, const std::string& which) {
  std::vector<std::string> out;
  if (which != "object") {
    std::vector<QPDFObjectHandle> ois = outputIntents(ev.root, oiSubtype(which));
    std::string b = ois.empty() ? std::string() : streamBytes(ois.front().getKey("/DestOutputProfile"));
    if (b.size() >= 128) out.push_back(b);
    return out;
  }
  std::set<QPDFObjGen> skip = outputProfileObjs(ev);
  for (QPDFObjectHandle o : allObjects(ev)) {
    if (!o.isStream() || skip.count(o.getObjGen())) continue;
    QPDFObjectHandle d = o.getDict();
    if (!d.getKey("/N").isInteger() || d.hasKey("/Subtype") || d.hasKey("/Type")) continue;
    std::string b = streamBytes(o);
    if (b.size() >= 128 && b.compare(36, 4, "acsp") == 0) out.push_back(b);
  }
  return out;
}

int evIccHeader(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::vector<Value> vs;
  for (const std::string& p : iccProfiles(ev, param(a, "profile"))) vs.push_back(iccHeaderField(p, param(a, "field")));
  return each(vs, a);
}

int evIccMatchesOutputIntent(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::vector<std::string> oi = iccProfiles(ev, param(a, "intent") == "destination" ? "any" : param(a, "intent"));
  const std::string& cmp = param(a, "field");
  bool any = false;
  for (const std::string& pr : oi.empty() ? std::vector<std::string>() : iccProfiles(ev, "object")) {
    const std::string& o = oi.front();
    if (cmp == "profile" && pr == o) any = true;
    if (cmp == "colour_space" && pr.compare(16, 4, o, 16, 4) == 0) any = true;
    if (cmp == "device_class" && pr.compare(12, 4, o, 12, 4) == 0) any = true;
  }
  return compare(Value(any), a);
}

int evIccMaxInk(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::vector<Value> vs;
  for (const std::string& prof : iccProfiles(ev, param(a, "profile"))) {
    if (prof.compare(16, 4, "CMYK") != 0) continue;
    cmsHPROFILE dst = cmsOpenProfileFromMem(prof.data(), static_cast<cmsUInt32Number>(prof.size()));
    cmsHPROFILE lab = cmsCreateLab4Profile(nullptr);
    double ink = -1;
    if (dst && lab) {
      cmsHTRANSFORM xf = cmsCreateTransform(lab, TYPE_Lab_DBL, dst, TYPE_CMYK_DBL, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
      if (xf) {
        double in[3] = {0, 0, 0}, out[4] = {0, 0, 0, 0};
        cmsDoTransform(xf, in, out, 1);
        ink = out[0] + out[1] + out[2] + out[3];
        cmsDeleteTransform(xf);
      }
    }
    if (dst) cmsCloseProfile(dst);
    if (lab) cmsCloseProfile(lab);
    if (ink >= 0) vs.push_back(Value(ink));
  }
  return each(vs, a);
}

int evIccTag(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::vector<Value> vs;
  for (const std::string& p : iccProfiles(ev, param(a, "profile"))) vs.push_back(iccTagText(p, param(a, "tag")));
  return each(vs, a);
}

QPDFObjectHandle imageDict(const Subject& s) {
  if (!s.image || s.image->inlineImg || !s.image->obj.isStream()) return QPDFObjectHandle();
  return s.image->obj.getDict();
}

bool anyOf(const Strs& v, std::initializer_list<const char*> names) {
  for (const std::string& x : v) {
    for (const char* n : names) {
      if (x == n) return true;
    }
  }
  return false;
}

int evImageAbbreviatedFilterNames(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.image) return 0;
  QPDFObjectHandle d = imageDict(s);
  bool abbr = d.isDictionary() && anyOf(filterNames(d), {"AHx", "A85", "LZW", "Fl", "RL", "CCF", "DCT"});
  return compare(Value(abbr), a);
}

int evImageAlternates(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.image) return 0;
  QPDFObjectHandle d = imageDict(s);
  QPDFObjectHandle alts = d.isDictionary() ? d.getKey("/Alternates") : QPDFObjectHandle();
  std::set<std::string> out;
  for (int i = 0; alts.isArray() && i < alts.getArrayNItems(); ++i) {
    QPDFObjectHandle ad = alts.getArrayItem(i);
    QPDFObjectHandle img = ad.isDictionary() ? ad.getKey("/Image") : QPDFObjectHandle();
    if (!img.isStream()) continue;
    out.insert("present");
    QPDFObjectHandle idd = img.getDict();
    if (ad.getKey("/DefaultForPrinting").isBool() && ad.getKey("/DefaultForPrinting").getBoolValue()) out.insert("default_for_printing");
    if (idd.hasKey("/OC") || ad.hasKey("/OC")) out.insert("uses_optional_content");
    if (numOf(idd.getKey("/Width"), -1) == s.image->width && numOf(idd.getKey("/Height"), -1) == s.image->height) out.insert("same_size_and_position");
  }
  return compare(listOf(out), a);
}

bool singleFilterData(const Subject& s, const char* filter, std::string& out) {
  QPDFObjectHandle d = imageDict(s);
  Strs fl = filterNames(d);
  if (fl.size() != 1 || fl[0] != filter) return false;
  try {
    QPDFObjectHandle o = s.image->obj;
    auto b = o.getRawStreamData();
    out.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
    return true;
  } catch (...) {
    return false;
  }
}

int evImageJbig2(const PfAtom& a, const Subject& s, const Events&, bool&) {
  std::string data;
  if (!s.image || !singleFilterData(s, "JBIG2Decode", data)) return 0;
  QPDFObjectHandle d = imageDict(s);
  std::map<int, int> segs = jbig2Segments(data);
  std::set<std::string> out;
  QPDFObjectHandle dp = d.getKey("/DecodeParms");
  if (dp.isArray() && dp.getArrayNItems() > 0) dp = dp.getArrayItem(dp.getArrayNItems() - 1);
  QPDFObjectHandle glob = dp.isDictionary() ? dp.getKey("/JBIG2Globals") : QPDFObjectHandle();
  if (glob.isStream()) {
    out.insert("globals");
    try {
      auto b = glob.getStreamData(qpdf_dl_generalized);
      for (auto& [k, v] : jbig2Segments(std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize()))) segs[k] += v;
    } catch (...) {
    }
  }
  struct Kind {
    const char* base;
    int inter, imm, lossless;
  };
  static const Kind kinds[] = {{"text_region", 4, 6, 7}, {"halftone_region", 20, 22, 23}, {"generic_region", 36, 38, 39}, {"generic_refinement", 40, 42, 43}};
  for (const Kind& k : kinds) {
    if (segs[k.inter]) out.insert(std::string("intermediate_") + k.base);
    if (segs[k.imm]) out.insert(std::string("immediate_") + k.base);
    if (segs[k.lossless]) out.insert(std::string("immediate_lossless_") + k.base);
    if (segs[k.inter] || segs[k.imm] || segs[k.lossless]) out.insert(k.base);
  }
  if (segs[0]) out.insert("symbol_dictionary");
  if (segs[16]) out.insert("pattern_dictionary");
  if ((out.count("text_region") && !out.count("generic_refinement")) || out.count("halftone_region")) out.insert("lossy");
  return compare(listOf(out), a);
}

int evImageJpeg2000(const PfAtom& a, const Subject& s, const Events&, bool&) {
  std::string data;
  if (!s.image || !singleFilterData(s, "JPXDecode", data)) return 0;
  JpxInfo ji = jpxInfo(data);
  if (!ji.ok) return 0;
  const std::string& p = param(a, "property");
  Value v;
  if (p == "channels") v = Value(ji.nc);
  else if (p == "colour_specs") v = Value(ji.colr);
  else if (p == "approximate_specs") v = Value(ji.approx1);
  else if (p == "same_bit_depth") v = boolText(ji.sameDepth);
  else if (p == "jab_colour_space") v = boolText(ji.cieJab);
  else if (p == "colour_method") {
    Strs m;
    for (int x : ji.meth) m.push_back(std::to_string(x));
    v = orAbsent(m);
  }
  return compare(v, a);
}

int evImageOpi(const PfAtom& a, const Subject& s, const Events&, bool&) {
  QPDFObjectHandle d = imageDict(s);
  QPDFObjectHandle opi = d.isDictionary() ? d.getKey("/OPI") : QPDFObjectHandle();
  QPDFObjectHandle od = opi.isDictionary() ? opi.getKey("/" + param(a, "version")) : QPDFObjectHandle();
  if (!od.isDictionary()) return 0;
  const std::string& f = param(a, "field");
  auto arrNum = [&](const char* key, int idx) {
    QPDFObjectHandle arr = od.getKey(key);
    return arr.isArray() && arr.getArrayNItems() > idx && arr.getArrayItem(idx).isNumber() ? Value(arr.getArrayItem(idx).getNumericValue()) : Value();
  };
  static const std::map<std::string, std::pair<const char*, int>> nums = {
      {"crop_left", {"/CropRect", 0}}, {"crop_top", {"/CropRect", 1}}, {"crop_right", {"/CropRect", 2}}, {"crop_bottom", {"/CropRect", 3}},
      {"size_width", {"/Size", 0}}, {"size_height", {"/Size", 1}}, {"position_ulx", {"/Position", 0}}, {"position_uly", {"/Position", 1}},
      {"position_urx", {"/Position", 2}}, {"position_ury", {"/Position", 3}}, {"position_lrx", {"/Position", 4}}, {"position_lry", {"/Position", 5}},
      {"position_llx", {"/Position", 6}}, {"position_lly", {"/Position", 7}}, {"resolution_x", {"/Resolution", 0}},
      {"resolution_y", {"/Resolution", 1}}, {"samples", {"/ImageType", 0}}, {"bits", {"/ImageType", 1}}, {"colour_c", {"/Color", 0}},
      {"colour_m", {"/Color", 1}}, {"colour_y", {"/Color", 2}}, {"colour_k", {"/Color", 3}},
      {"included_width", {"/IncludedImageDimensions", 0}}, {"included_height", {"/IncludedImageDimensions", 1}}};
  Value v;
  auto it = nums.find(f);
  if (it != nums.end()) v = arrNum(it->second.first, it->second.second);
  else if (f == "file") v = od.getKey("/F").isDictionary() ? textOf(od.getKey("/F").getKey("/F")) : textOf(od.getKey("/F"));
  else if (f == "id") v = textOf(od.getKey("/ID"));
  else if (f == "comments") v = textOf(od.getKey("/Comments"));
  else if (f == "type") v = textOf(od.getKey("/ColorType"));
  else if (f == "colour") v = od.getKey("/Color").isArray() ? Value(numbersText(od.getKey("/Color"))) : Value();
  else if (f == "colour_name") {
    QPDFObjectHandle c = od.getKey("/Color");
    v = c.isArray() && c.getArrayNItems() > 4 ? textOf(c.getArrayItem(4)) : Value();
  } else if (f == "tint") v = od.getKey("/Tint").isNumber() ? Value(od.getKey("/Tint").getNumericValue()) : Value();
  else if (f == "overprint") v = boolText(od.getKey("/Overprint").isBool() && od.getKey("/Overprint").getBoolValue());
  else if (f == "transparency") v = boolText(!od.getKey("/Transparency").isBool() || od.getKey("/Transparency").getBoolValue());
  else if (f == "included_quality") v = od.getKey("/IncludedImageQuality").isNumber() ? Value(od.getKey("/IncludedImageQuality").getNumericValue()) : Value();
  else if (f == "main_image") v = textOf(od.getKey("/MainImage"));
  return compare(v, a);
}

int componentsOf(QPDFObjectHandle cs) {
  if (cs.isArray() && cs.getArrayNItems() > 0) {
    std::string f = nameOf(cs.getArrayItem(0));
    if (f == "/ICCBased" && cs.getArrayNItems() > 1 && cs.getArrayItem(1).isStream()) return static_cast<int>(numOf(cs.getArrayItem(1).getDict().getKey("/N"), 3));
    if (f == "/Indexed" || f == "/Separation" || f == "/CalGray") return 1;
    if (f == "/DeviceN" && cs.getArrayNItems() > 1 && cs.getArrayItem(1).isArray()) return cs.getArrayItem(1).getArrayNItems();
    if (f == "/Lab" || f == "/CalRGB") return 3;
    return 1;
  }
  std::string n = nameOf(cs);
  if (n == "/DeviceRGB" || n == "/RGB") return 3;
  if (n == "/DeviceCMYK" || n == "/CMYK") return 4;
  return 1;
}

Value imageProperty(const std::string& p, QPDFObjectHandle o, const ImageEvent* e) {
  QPDFObjectHandle d = o.isStream() ? o.getDict() : QPDFObjectHandle();
  bool dict = d.isDictionary();
  Strs fl = dict ? filterNames(d) : e ? Strs(e->filters.begin(), e->filters.end()) : Strs();
  bool mask = dict ? d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue() : e && e->mask;
  double w = dict ? numOf(d.getKey("/Width"), 0) : e ? e->width : 0;
  double h = dict ? numOf(d.getKey("/Height"), 0) : e ? e->height : 0;
  double bpc = mask ? 1 : dict ? numOf(d.getKey("/BitsPerComponent"), e ? e->bpc : 8) : e ? e->bpc : 8;
  double comps = mask ? 1 : e && !dict ? std::max(1, e->color.declaredComps) : dict ? componentsOf(d.getKey("/ColorSpace")) : 1;
  if (e && dict && !mask) comps = std::max(1, e->color.declaredComps);
  double raw = dict ? numOf(d.getKey("/Length"), 0) : 0;
  double unc = w * h * comps * bpc / 8.0;
  if (p == "bits_per_component") return Value(bpc);
  if (p == "width") return Value(w);
  if (p == "height") return Value(h);
  if (p == "filter") return fl.empty() ? Value(Strs{"none"}) : Value(fl);
  if (p == "lossy") return boolText(anyOf(fl, {"DCTDecode", "DCT", "JPXDecode"}));
  if (p == "compressed") return boolText(!fl.empty());
  if (p == "ascii_encoded") return boolText(anyOf(fl, {"ASCIIHexDecode", "ASCII85Decode", "AHx", "A85"}));
  if (p == "interpolate") return boolText(dict ? d.getKey("/Interpolate").isBool() && d.getKey("/Interpolate").getBoolValue() : e && e->interpolate);
  if (p == "is_xobject") return boolText(dict && !(e && e->inlineImg));
  if (p == "stencil_mask") return boolText(mask);
  if (p == "soft_mask") return boolText(dict ? d.getKey("/SMask").isStream() : e && e->hasSMask);
  if (p == "invalid") return boolText(w <= 0 || h <= 0);
  if (p == "inline_size") return e && e->inlineImg ? Value(static_cast<double>(e->inlineSize)) : Value();
  if (p == "colour_type") {
    if (!e) return Value();
    if (e->mask) return Value("mask");
    if (e->color.cls == "devicen" && e->color.colorants.size() == 2) return Value("duotone");
    if (e->color.declaredComps > 1 || e->color.cls == "separation" || e->color.cls == "devicen") return Value("colour");
    return Value("grey");
  }
  if (!dict) return Value();
  if (p == "interpolate_present") return boolText(d.hasKey("/Interpolate"));
  if (p == "intent_present") return boolText(d.hasKey("/Intent"));
  if (p == "colour_key_mask") return boolText(d.getKey("/Mask").isArray());
  if (p == "explicit_mask") return boolText(d.getKey("/Mask").isStream());
  if (p == "soft_mask_in_data") return boolText(d.hasKey("/SMaskInData"));
  if (p == "soft_mask_in_data_value") return d.getKey("/SMaskInData").isNumber() ? Value(d.getKey("/SMaskInData").getNumericValue()) : Value();
  if (p == "soft_mask_matte") return boolText(d.getKey("/SMask").isStream() && d.getKey("/SMask").getDict().hasKey("/Matte"));
  if (p == "length_correct") {
    try {
      return boolText(o.getRawStreamData()->getSize() == static_cast<size_t>(raw));
    } catch (...) {
      return boolText(false);
    }
  }
  if (p == "bytes_compressed") return Value(raw);
  if (p == "bytes_uncompressed") return Value(unc);
  if (p == "compression_ratio") return unc > 0 ? Value(raw * 100.0 / unc) : Value();
  if (p == "opi_info") return boolText(d.hasKey("/OPI"));
  if (p == "jpeg_quality") {
    if (fl.size() != 1 || fl[0] != "DCTDecode") return Value();
    try {
      auto b = o.getRawStreamData();
      int q = jpegQuality(std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize()));
      return q >= 0 ? Value(q) : Value();
    } catch (...) {
      return Value();
    }
  }
  return Value();
}

int evImageProperty(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.image) return 0;
  const std::string& p = param(a, "property");
  QPDFObjectHandle o = s.image->inlineImg ? QPDFObjectHandle() : s.image->obj;
  if (!paramBool(a, "alternate")) return compare(imageProperty(p, o, s.image), a);
  QPDFObjectHandle d = imageDict(s);
  QPDFObjectHandle alts = d.isDictionary() ? d.getKey("/Alternates") : QPDFObjectHandle();
  std::vector<Value> vs;
  for (int i = 0; alts.isArray() && i < alts.getArrayNItems(); ++i) {
    QPDFObjectHandle img = alts.getArrayItem(i).isDictionary() ? alts.getArrayItem(i).getKey("/Image") : QPDFObjectHandle();
    if (img.isStream()) vs.push_back(imageProperty(p, img, nullptr));
  }
  return each(vs, a);
}

QPDFObjectHandle defaultConfig(const Events& ev) {
  QPDFObjectHandle ocp = ev.root.isDictionary() ? ev.root.getKey("/OCProperties") : QPDFObjectHandle();
  return ocp.isDictionary() ? ocp.getKey("/D") : QPDFObjectHandle();
}

std::vector<QPDFObjectHandle> ocConfigs(const Events& ev) {
  std::vector<QPDFObjectHandle> out;
  QPDFObjectHandle ocp = ev.root.isDictionary() ? ev.root.getKey("/OCProperties") : QPDFObjectHandle();
  if (!ocp.isDictionary()) return out;
  if (ocp.getKey("/D").isDictionary()) out.push_back(ocp.getKey("/D"));
  QPDFObjectHandle c = ocp.getKey("/Configs");
  for (int i = 0; c.isArray() && i < c.getArrayNItems(); ++i) {
    if (c.getArrayItem(i).isDictionary()) out.push_back(c.getArrayItem(i));
  }
  return out;
}

int evLayerConfiguration(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle ocp = ev.root.isDictionary() ? ev.root.getKey("/OCProperties") : QPDFObjectHandle();
  std::set<std::string> out;
  if (ocp.isDictionary()) {
    QPDFObjectHandle d = ocp.getKey("/D");
    QPDFObjectHandle cf = ocp.getKey("/Configs");
    if (!d.isDictionary()) out.insert("no_default_config");
    if (cf.isArray() && cf.getArrayNItems() > 0) out.insert("has_alternate_configs");
    std::set<std::string> names;
    for (QPDFObjectHandle c : ocConfigs(ev)) {
      if (!c.getKey("/Name").isString()) out.insert("config_missing_name");
      else if (!names.insert(c.getKey("/Name").getUTF8Value()).second) out.insert("config_name_not_unique");
      if (c.hasKey("/AS")) out.insert("has_as_entry");
    }
    if (d.isDictionary()) {
      if (d.hasKey("/BaseState")) out.insert("has_base_state");
      if (nameIs(d.getKey("/BaseState"), "/OFF")) out.insert("base_state_off");
      if (nameIs(d.getKey("/BaseState"), "/ON")) out.insert("base_state_on");
      if (d.getKey("/OFF").isArray()) out.insert("has_off_array");
      QPDFObjectHandle order = d.getKey("/Order");
      if (!order.isArray()) out.insert("missing_order");
      else if (order.getArrayNItems() == 0) out.insert("empty_order");
      for (QPDFObjectHandle g : layers(ev.root)) {
        if (!inArray(order, g)) out.insert("order_missing_layers");
      }
    }
  }
  return compare(listOf(out), a);
}

int evLayerConfigurationName(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  Strs out;
  bool order = param(a, "which") == "order";
  std::function<void(QPDFObjectHandle, int)> scan = [&](QPDFObjectHandle o, int d) {
    if (d > 32) return;
    if (o.isArray()) {
      for (int i = 0; i < o.getArrayNItems(); ++i) scan(o.getArrayItem(i), d + 1);
      return;
    }
    std::string n = o.isDictionary() ? str(o.getKey("/Name")) : str(o);
    if (!n.empty()) out.push_back(n);
  };
  for (QPDFObjectHandle c : ocConfigs(ev)) {
    if (order) scan(c.getKey("/Order"), 0);
    else if (c.getKey("/Name").isString()) out.push_back(c.getKey("/Name").getUTF8Value());
  }
  return compare(Value(out), a);
}

std::set<QPDFObjGen> referencedLayers(const Events& ev) {
  std::set<QPDFObjGen> referenced;
  auto refOC = [&](QPDFObjectHandle oc) {
    if (!oc.isDictionary()) return;
    if (nameIs(oc.getKey("/Type"), "/OCMD")) {
      QPDFObjectHandle g = oc.getKey("/OCGs");
      if (g.isIndirect()) referenced.insert(g.getObjGen());
      for (int i = 0; g.isArray() && i < g.getArrayNItems(); ++i) {
        if (g.getArrayItem(i).isIndirect()) referenced.insert(g.getArrayItem(i).getObjGen());
      }
    } else if (oc.isIndirect()) {
      referenced.insert(oc.getObjGen());
    }
  };
  std::function<void(QPDFObjectHandle, int)> scan = [&](QPDFObjectHandle d, int depth) {
    if (depth > 8) return;
    if (d.isArray()) {
      for (int i = 0; i < d.getArrayNItems(); ++i) {
        if (!d.getArrayItem(i).isIndirect()) scan(d.getArrayItem(i), depth + 1);
      }
      return;
    }
    if (!d.isDictionary()) return;
    refOC(d.getKey("/OC"));
    QPDFObjectHandle props = d.getKey("/Properties");
    for (const std::string& k : props.isDictionary() ? props.getKeys() : std::set<std::string>()) refOC(props.getKey(k));
    for (const std::string& k : d.getKeys()) {
      if (k != "/Properties" && k != "/OC" && !d.getKey(k).isIndirect()) scan(d.getKey(k), depth + 1);
    }
  };
  for (QPDFObjectHandle o : allObjects(ev)) {
    if (nameIs(o.isStream() ? o.getDict().getKey("/Type") : o.getKey("/Type"), "/OCProperties")) continue;
    scan(o.isStream() ? o.getDict() : o, 0);
  }
  return referenced;
}

bool subjectLayers(const Subject& s, const Events& ev, std::vector<QPDFObjectHandle>& conds, bool& supported) {
  if (!s.paint && !s.text && !s.image) return false;
  if (!objectOcConds(s, ev, conds)) supported = false;
  return true;
}

int evLayerProperty(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  const std::string& p = param(a, "property");
  QPDFObjectHandle d = defaultConfig(ev);
  std::set<QPDFObjGen> referenced;
  if (p == "empty") referenced = referencedLayers(ev);
  QPDFObjectHandle ocp = ev.root.isDictionary() ? ev.root.getKey("/OCProperties") : QPDFObjectHandle();
  auto usage = [](QPDFObjectHandle g, const char* cat) {
    QPDFObjectHandle u = g.getKey("/Usage");
    return u.isDictionary() ? u.getKey(cat) : QPDFObjectHandle();
  };
  auto inD = [&](QPDFObjectHandle g, const char* key) { return d.isDictionary() && inArray(d.getKey(key), g); };
  std::vector<Value> vs;
  std::vector<QPDFObjectHandle> conds, groups;
  if (subjectLayers(s, ev, conds, supported)) {
    if (!supported) return 0;
    for (QPDFObjectHandle c : conds) {
      for (QPDFObjectHandle g : ocgsOf(c)) groups.push_back(g);
    }
  } else {
    groups = layers(ev.root);
  }
  for (QPDFObjectHandle g : groups) {
    Value v;
    if (p == "name") v = textOf(g.getKey("/Name"));
    else if (p == "has_name") v = boolText(!str(g.getKey("/Name")).empty());
    else if (p == "creator") v = usage(g, "/CreatorInfo").isDictionary() ? textOf(usage(g, "/CreatorInfo").getKey("/Creator")) : Value();
    else if (p == "has_creator_info") v = boolText(usage(g, "/CreatorInfo").isDictionary());
    else if (p == "locked") v = boolText(inD(g, "/Locked"));
    else if (p == "empty") v = boolText(g.isIndirect() && !referenced.count(g.getObjGen()));
    else if (p == "in_navigation") v = boolText(inD(g, "/Order"));
    else if (p == "radio_group") v = boolText(inD(g, "/RBGroups"));
    else if (p == "exported") v = boolText(!usage(g, "/Export").isDictionary() || !nameIs(usage(g, "/Export").getKey("/ExportState"), "/OFF"));
    else if (p == "printed") v = boolText(usage(g, "/Print").isDictionary() && !nameIs(usage(g, "/Print").getKey("/PrintState"), "/OFF"));
    else if (p == "zoom_min" || p == "zoom_max") {
      QPDFObjectHandle z = usage(g, "/Zoom");
      QPDFObjectHandle n = z.isDictionary() ? z.getKey(p == "zoom_min" ? "/min" : "/max") : QPDFObjectHandle();
      if (n.isNumber()) v = Value(n.getNumericValue() * 100);
    } else if (p == "hidden_at_some_zoom") v = boolText(usage(g, "/Zoom").isDictionary());
    else if (p == "language") v = usage(g, "/Language").isDictionary() ? textOf(usage(g, "/Language").getKey("/Lang")) : Value();
    else if (p == "for_language") v = boolText(usage(g, "/Language").isDictionary());
    else if (p == "users") {
      QPDFObjectHandle n = usage(g, "/User").isDictionary() ? usage(g, "/User").getKey("/Name") : QPDFObjectHandle();
      if (n.isArray()) {
        std::string all;
        for (int i = 0; i < n.getArrayNItems(); ++i) all += (all.empty() ? "" : ",") + str(n.getArrayItem(i));
        v = Value(all);
      } else {
        v = textOf(n);
      }
    } else if (p == "user_type") v = usage(g, "/User").isDictionary() ? textOf(usage(g, "/User").getKey("/Type")) : Value();
    else if (p == "for_users" || p == "has_usage_user") v = boolText(usage(g, "/User").isDictionary());
    else if (p == "content_type" || p == "pagination_subtype") v = usage(g, "/PageElement").isDictionary() ? textOf(usage(g, "/PageElement").getKey("/Subtype")) : Value();
    else if (p == "pagination_artifact") v = boolText(usage(g, "/PageElement").isDictionary());
    else if (p == "metadata") v = boolText(g.getKey("/Metadata").isStream());
    else if (p == "listed") v = boolText(ocp.isDictionary() && inArray(ocp.getKey("/OCGs"), g));
    vs.push_back(v);
  }
  return each(vs, a);
}

int evLayerVisibilityPolicy(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  std::vector<Value> vs;
  std::vector<QPDFObjectHandle> conds;
  if (subjectLayers(s, ev, conds, supported)) {
    if (!supported) return 0;
  } else {
    conds = allObjects(ev);
  }
  for (QPDFObjectHandle o : conds) {
    if (o.isDictionary() && nameIs(o.getKey("/Type"), "/OCMD")) vs.push_back(Value(o.getKey("/P").isName() ? o.getKey("/P").getName().substr(1) : "AnyOn"));
  }
  return each(vs, a);
}

std::string infoKey(const std::string& k) { return k.rfind('/', 0) == 0 ? k : "/" + k; }

int evMetadataInfo(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle info = infoDict(ev);
  if (!info.isDictionary()) return 0;
  return compare(textOf(info.getKey(infoKey(param(a, "key")))), a);
}

int evMetadataInfoKeys(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  static const std::set<std::string> standard = {"/Title", "/Author", "/Subject", "/Keywords", "/Creator", "/Producer", "/CreationDate", "/ModDate", "/Trapped"};
  QPDFObjectHandle info = infoDict(ev);
  std::set<std::string> out;
  for (const std::string& k : info.isDictionary() ? info.getKeys() : std::set<std::string>()) {
    if (standard.count(k)) out.insert("standard");
    else if (k == "/GTS_PDFXVersion" || k == "/GTS_PDFXConformance") out.insert("pdfx");
    else out.insert("custom");
    if (k != "/ModDate") out.insert("other_than_mod_date");
  }
  return compare(listOf(out), a);
}

bool infoMatchesXmp(QPDFObjectHandle info, const std::string& xmp, const std::string& key) {
  static const std::map<std::string, std::string> pairs = {
      {"Title", "dc:title"}, {"Author", "dc:creator"}, {"Subject", "dc:description"}, {"Keywords", "pdf:Keywords"},
      {"Creator", "xmp:CreatorTool"}, {"Producer", "pdf:Producer"}, {"CreationDate", "xmp:CreateDate"},
      {"ModDate", "xmp:ModifyDate"}, {"Trapped", "pdf:Trapped"}, {"GTS_PDFXVersion", "pdfxid:GTS_PDFXVersion"},
      {"ISO_PDFEVersion", "pdfe:ISO_PDFEVersion"}};
  if (key == "all") {
    for (const char* k : {"Title", "Author", "Subject", "Keywords", "Creator", "Producer", "CreationDate", "ModDate"}) {
      if (!infoMatchesXmp(info, xmp, k)) return false;
    }
    return true;
  }
  auto it = pairs.find(key);
  if (it == pairs.end()) return true;
  std::string iv = info.isDictionary() ? str(info.getKey("/" + key)) : std::string();
  if (iv.empty()) return true;
  std::string xv = xmpValue(xmp, it->second);
  if (key == "CreationDate" || key == "ModDate") {
    long long i1 = 0, i2 = 0;
    return !xv.empty() && parseDateInstant(iv, i1) && parseDateInstant(xv, i2) && i1 == i2;
  }
  return iv == xv;
}

int evMetadataInfoMatchesXmp(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  return compare(Value(infoMatchesXmp(infoDict(ev), xmpOf(ev.root), param(a, "key"))), a);
}

int evMetadataInfoProblem(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle info = infoDict(ev);
  std::set<std::string> out;
  if (info.isDictionary()) {
    auto uni = [&](const char* k) {
      QPDFObjectHandle v = info.getKey(k);
      std::string raw = v.isString() ? v.getStringValue() : std::string();
      return raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFE && static_cast<unsigned char>(raw[1]) == 0xFF;
    };
    if (uni("/CreationDate") || uni("/ModDate")) out.insert("date_in_unicode");
    if (info.hasKey("/Trapped") && !info.getKey("/Trapped").isName()) out.insert("trapped_not_name");
    if (info.hasKey("/GTS_PDFXVersion") && !info.getKey("/GTS_PDFXVersion").isString()) out.insert("pdfx_version_not_string");
    if (info.hasKey("/GTS_PDFXConformance") && !info.getKey("/GTS_PDFXConformance").isString()) out.insert("pdfx_conformance_not_string");
    if (uni("/GTS_PDFXVersion")) out.insert("pdfx_version_unicode");
    if (uni("/ISO_PDFEVersion")) out.insert("pdfe_version_unicode");
  }
  return compare(listOf(out), a);
}

std::string metadataKind(QPDFObjectHandle o, QPDFObjectHandle d) {
  if (nameIs(d.getKey("/Type"), "/Catalog")) return "document";
  if (nameIs(d.getKey("/Type"), "/Page")) return "page";
  if (o.isStream() && nameIs(d.getKey("/Subtype"), "/Image")) return "image";
  if (o.isStream() && nameIs(d.getKey("/Subtype"), "/Form")) return "form_xobject";
  if (nameIs(d.getKey("/Type"), "/FontDescriptor")) return "font";
  if (o.isStream() && d.getKey("/N").isInteger()) return "icc_profile";
  return "property_list";
}

struct Packet {
  std::string kind, data;
  QPDFObjectHandle stream;
};

std::vector<Packet> packets(const Events& ev) {
  std::vector<Packet> out;
  for (QPDFObjectHandle o : allObjects(ev)) {
    QPDFObjectHandle d = o.isStream() ? o.getDict() : o;
    if (!d.isDictionary() || !d.getKey("/Metadata").isStream()) continue;
    out.push_back({metadataKind(o, d), std::string(), d.getKey("/Metadata")});
  }
  return out;
}

std::vector<Packet> scopedPackets(const Subject& s, const Events& ev, const std::string& scope) {
  std::vector<Packet> out;
  if (scope == "document") {
    QPDFObjectHandle md = ev.root.isDictionary() ? ev.root.getKey("/Metadata") : QPDFObjectHandle();
    if (md.isStream()) out.push_back({"document", streamBytes(md), md});
    return out;
  }
  if (scope == "page") {
    auto add = [&](const PageFacts& p) {
      QPDFObjectHandle md = p.obj.isDictionary() ? p.obj.getKey("/Metadata") : QPDFObjectHandle();
      if (md.isStream()) out.push_back({"page", streamBytes(md), md});
    };
    if (s.pageFacts) add(*s.pageFacts);
    else for (const PageFacts& p : ev.pages) add(p);
    return out;
  }
  for (Packet& p : packets(ev)) {
    if (scope == "object" && (p.kind == "document" || p.kind == "page")) continue;
    p.data = streamBytes(p.stream);
    out.push_back(p);
  }
  return out;
}

int evMetadataObjectHasXmp(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string& want = param(a, "object");
  bool has = false;
  for (const Packet& p : packets(ev)) has = has || (p.kind != "document" && (want == "any" || want == p.kind));
  return compare(Value(has), a);
}

int evMetadataXmpDocumentIdIsUuid(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::string x = xmpOf(ev.root);
  if (x.empty()) return 0;
  return compare(Value(xmpValue(x, "xmpMM:DocumentID").rfind("uuid:", 0) == 0), a);
}

std::map<std::string, std::string> nsBindings(const std::string& x) {
  std::map<std::string, std::string> out;
  static const std::regex re("xmlns:([A-Za-z_][A-Za-z0-9_.-]*)\\s*=\\s*[\"']([^\"']*)[\"']");
  for (auto it = std::sregex_iterator(x.begin(), x.end(), re); it != std::sregex_iterator(); ++it) out[(*it)[1].str()] = (*it)[2].str();
  return out;
}

int evMetadataXmpIdentification(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  struct Std {
    const char *prefix, *uri, *key, *rev;
    std::set<std::string> props;
  };
  static const std::map<std::string, Std> stds = {
      {"pdfa", {"pdfaid", "http://www.aiim.org/pdfa/ns/id/", "pdfaid:part", "pdfaid:rev", {"part", "conformance", "amd", "corr", "rev"}}},
      {"pdfua", {"pdfuaid", "http://www.aiim.org/pdfua/ns/id/", "pdfuaid:part", "pdfuaid:rev", {"part", "amd", "corr", "rev"}}},
      {"pdfx", {"pdfxid", "http://www.npes.org/pdfx/ns/id/", "pdfxid:GTS_PDFXVersion", "", {"GTS_PDFXVersion"}}},
      {"pdfe", {"pdfe", "http://www.aiim.org/pdfe/ns/id/", "pdfe:ISO_PDFEVersion", "", {"ISO_PDFEVersion"}}},
      {"pdfvt", {"pdfvtid", "http://www.npes.org/pdfvt/ns/id/", "pdfvtid:GTS_PDFVTVersion", "pdfvtid:GTS_PDFVTRev", {"GTS_PDFVTVersion", "GTS_PDFVTModDate", "GTS_PDFVTRev"}}}};
  const Std& st = stds.at(param(a, "standard"));
  std::string x = xmpOf(ev.root);
  std::map<std::string, std::string> ns = nsBindings(x);
  std::set<std::string> out;
  bool uriUsed = false;
  for (const auto& kv : ns) uriUsed = uriUsed || kv.second == st.uri;
  bool bound = ns.count(st.prefix) > 0;
  std::string key = xmpValue(x, st.key);
  bool keyPresent = x.find(std::string("<") + st.key) != std::string::npos || x.find(std::string(st.key) + "=") != std::string::npos;
  if (!uriUsed && !bound) {
    out.insert("missing");
  } else {
    if (!keyPresent) out.insert("part_missing");
    if (bound && ns[st.prefix] != st.uri) out.insert("wrong_namespace");
    std::string rev = *st.rev ? xmpValue(x, st.rev) : std::string();
    bool revWanted = *st.rev && (param(a, "standard") == "pdfa" ? key == "4" : param(a, "standard") == "pdfua" ? key == "2" : keyPresent);
    if (revWanted && rev.empty()) out.insert("revision_missing");
    if (!rev.empty() && (rev.size() != 4 || rev.find_first_not_of("0123456789") != std::string::npos)) out.insert("revision_not_four_digits");
    std::set<std::string> used;
    std::regex prop(std::string("[<\\s]") + st.prefix + ":([A-Za-z_][A-Za-z0-9_.-]*)");
    for (auto it = std::sregex_iterator(x.begin(), x.end(), prop); it != std::sregex_iterator(); ++it) used.insert((*it)[1].str());
    for (const std::string& u : used) {
      if (!st.props.count(u)) out.insert("unknown_properties");
    }
    if (used.size() == 1 && used.count(std::string(st.key).substr(std::string(st.key).find(':') + 1))) out.insert("version_only");
  }
  return compare(listOf(out), a);
}

int evMetadataXmpNamespaces(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  std::set<std::string> out;
  for (const Packet& p : scopedPackets(s, ev, param(a, "scope"))) {
    for (const auto& kv : nsBindings(p.data)) out.insert(kv.second);
  }
  return compare(listOf(out), a);
}

int evMetadataXmpPrefix(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  const std::string& uri = param(a, "namespace");
  Strs out;
  for (const Packet& p : scopedPackets(s, ev, param(a, "scope"))) {
    for (const auto& kv : nsBindings(p.data)) {
      if (kv.second == uri) out.push_back(kv.first);
    }
  }
  return compare(orAbsent(out), a);
}

int evMetadataXmpProblem(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  std::set<std::string> out;
  for (const Packet& p : scopedPackets(s, ev, param(a, "scope"))) {
    bool wf = xmpWellFormed(p.data);
    if (!wf) out.insert("not_well_formed");
    if (!wf || p.data.find("rdf:RDF") == std::string::npos) out.insert("not_xmp_conformant");
    if (p.stream.isStream() && !p.stream.getDict().getKey("/Filter").isNull()) out.insert("filtered");
    size_t pi = p.data.find("<?xpacket");
    size_t pe = pi == std::string::npos ? std::string::npos : p.data.find("?>", pi);
    std::string head = pe == std::string::npos ? std::string() : p.data.substr(pi, pe - pi);
    if (head.find("bytes=") != std::string::npos) out.insert("bytes_attribute");
    if (head.find("encoding=") != std::string::npos) out.insert("encoding_attribute");
  }
  return compare(listOf(out), a);
}

int evMetadataXmpProperty(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  const std::string& name = param(a, "name");
  if (name.empty()) return 0;
  Strs out;
  for (const Packet& p : scopedPackets(s, ev, param(a, "scope"))) {
    if (p.data.find("<" + name) == std::string::npos && p.data.find(name + "=") == std::string::npos) continue;
    out.push_back(xmpValue(p.data, name));
  }
  return compare(orAbsent(out), a);
}

int evMetadataXmpSize(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  std::vector<Value> vs;
  for (const Packet& p : scopedPackets(s, ev, param(a, "scope"))) vs.push_back(Value(static_cast<double>(p.data.size())));
  return each(vs, a);
}

std::string md5Of(const std::string& data) {
  auto c = QPDFCryptoProvider::getImpl();
  c->MD5_init();
  c->MD5_update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
  c->MD5_finalize();
  QPDFCryptoImpl::MD5_Digest d;
  c->MD5_digest(d);
  return std::string(reinterpret_cast<const char*>(d), sizeof(d));
}

std::string specUrl(QPDFObjectHandle fs) {
  if (fs.isString()) return fs.getUTF8Value();
  if (!fs.isDictionary()) return std::string();
  for (const char* k : {"/UF", "/F", "/Unix", "/DOS", "/Mac"}) {
    if (fs.getKey(k).isString()) return fs.getKey(k).getUTF8Value();
  }
  return std::string();
}

std::string localPath(const Events& ev, std::string url) {
  if (url.rfind("file://", 0) == 0) url = url.substr(7);
  if (url.empty() || url.find("://") != std::string::npos) return std::string();
  std::filesystem::path p(url);
  if (p.is_relative()) {
    if (ev.inputPath.empty()) return std::string();
    p = std::filesystem::path(ev.inputPath).parent_path() / p;
  }
  std::error_code ec;
  return std::filesystem::is_regular_file(p, ec) ? p.string() : std::string();
}

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool refProfileAvailable(const Events& ev, QPDFObjectHandle ref) {
  std::string sum = ref.getKey("/CheckSum").isString() ? ref.getKey("/CheckSum").getStringValue() : std::string();
  std::vector<std::string> cands;
  QPDFObjectHandle urls = ref.getKey("/URLs");
  for (int i = 0; urls.isArray() && i < urls.getArrayNItems(); ++i) cands.push_back(specUrl(urls.getArrayItem(i)));
  std::string pn = str(ref.getKey("/ProfileName"));
  if (!pn.empty()) {
    cands.push_back(pn);
    cands.push_back(pn + ".icc");
  }
  for (const std::string& u : cands) {
    std::string path = localPath(ev, u);
    if (!path.empty() && (sum.empty() || md5Of(readFile(path)) == sum)) return true;
  }
  return false;
}

bool standardCondition(const std::string& id) {
  static const char* known[] = {"FOGRA", "CGATS", "GRACoL", "SWOP", "JC200", "JCN", "Japan", "IFRA", "PSO", "ISO", "sRGB", "CRPC", "SNAP"};
  for (const char* k : known) {
    if (id.rfind(k, 0) == 0) return true;
  }
  return false;
}

int evOutputIntentEntry(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  if (!ev.root.isDictionary()) return 0;
  const std::string& e = param(a, "entry");
  std::vector<QPDFObjectHandle> ois = outputIntents(ev.root, oiSubtype(param(a, "subtype")));
  if (e == "referenced_profile_available" && ev.inputPath.empty()) {
    supported = false;
    return 0;
  }
  if (e == "referenced_profiles") {
    int n = 0;
    for (QPDFObjectHandle o : ois) n += o.getKey("/DestOutputProfileRef").isDictionary();
    return compare(Value(n), a);
  }
  if (e == "profiles_same_in_all") {
    if (ois.empty()) return 0;
    bool same = true;
    std::string first;
    bool have = false;
    for (QPDFObjectHandle o : outputIntents(ev.root, "")) {
      QPDFObjectHandle pr = o.getKey("/DestOutputProfile");
      if (!pr.isStream()) continue;
      std::string b = streamBytes(pr);
      if (!have) first = b, have = true;
      else if (b != first) same = false;
    }
    return compare(boolText(same), a);
  }
  std::vector<Value> vs;
  for (QPDFObjectHandle oi : ois) {
    QPDFObjectHandle pr = oi.getKey("/DestOutputProfile");
    QPDFObjectHandle ref = oi.getKey("/DestOutputProfileRef");
    Value v;
    if (e == "identifier") v = textOf(oi.getKey("/OutputConditionIdentifier"));
    else if (e == "condition") v = textOf(oi.getKey("/OutputCondition"));
    else if (e == "registry") v = textOf(oi.getKey("/RegistryName"));
    else if (e == "info") v = textOf(oi.getKey("/Info"));
    else if (e == "subtype") v = textOf(oi.getKey("/S"));
    else if (e == "has_profile") v = boolText(pr.isStream());
    else if (e == "has_registry") v = boolText(oi.hasKey("/RegistryName"));
    else if (e == "identifier_is_string") v = boolText(oi.getKey("/OutputConditionIdentifier").isString());
    else if (e == "condition_is_string") v = boolText(oi.getKey("/OutputCondition").isString());
    else if (e == "registry_is_string") v = boolText(oi.getKey("/RegistryName").isString());
    else if (e == "info_is_string") v = boolText(oi.getKey("/Info").isString());
    else if (e == "condition_is_standard") v = boolText(standardCondition(str(oi.getKey("/OutputConditionIdentifier"))));
    else if (e == "profile_ref_is_dictionary") v = boolText(ref.isDictionary());
    else if (e == "device_independent_allowed") v = oi.getKey("/DevIndependentEnbl").isBool() ? boolText(oi.getKey("/DevIndependentEnbl").getBoolValue()) : Value();
    else if (e == "min_halftone_resolution" || e == "min_bitmap_resolution" || e == "max_plates") {
      QPDFObjectHandle n = oi.getKey(e == "max_plates" ? "/MaxNumberOfColorPlates" : e == "min_bitmap_resolution" ? "/MinBitmapImgResolution" : "/MinHalftoneImgResolution");
      if (n.isNumber()) v = Value(n.getNumericValue());
    } else if (e == "uses_nchannel_profile") {
      std::string cs = ref.isDictionary() ? str(ref.getKey("/ProfileCS")) : std::string();
      std::string b = streamBytes(pr);
      bool n = (cs.size() == 4 && cs.compare(1, 3, "CLR") == 0) || (b.size() >= 20 && b.compare(17, 3, "CLR") == 0);
      v = boolText(n);
    } else if (e == "alternate_colour_spaces") v = boolText(pr.isStream() && pr.getDict().hasKey("/Alternate"));
    else if (e == "profile_component_mismatch") {
      std::string b = streamBytes(pr);
      int n = pr.isStream() ? static_cast<int>(numOf(pr.getDict().getKey("/N"), 0)) : 0;
      std::string cs = b.size() >= 20 ? b.substr(16, 4) : std::string();
      int want = cs == "GRAY" ? 1 : cs == "RGB " ? 3 : cs == "CMYK" ? 4 : n;
      v = pr.isStream() ? boolText(n != want) : Value();
    } else if (e == "referenced_profile_available") v = ref.isDictionary() ? boolText(refProfileAvailable(ev, ref)) : Value();
    vs.push_back(v);
  }
  return each(vs, a);
}

int evOutputIntentProfileReference(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::set<std::string> out;
  if (ev.root.isDictionary()) {
    for (QPDFObjectHandle oi : outputIntents(ev.root, "")) {
      QPDFObjectHandle ref = oi.getKey("/DestOutputProfileRef");
      if (!ref.isDictionary()) continue;
      if (!ref.hasKey("/ProfileName")) out.insert("missing_name");
      else if (!ref.getKey("/ProfileName").isString()) out.insert("name_not_string");
      QPDFObjectHandle iv = ref.getKey("/ICCVersion");
      if (!ref.hasKey("/ICCVersion")) out.insert("missing_icc_version");
      else if (!iv.isString()) out.insert("icc_version_not_string");
      else if (iv.getStringValue().size() != 4) out.insert("icc_version_bad");
      if (!ref.hasKey("/CheckSum")) out.insert("missing_checksum");
      else if (!ref.getKey("/CheckSum").isString() || ref.getKey("/CheckSum").getStringValue().size() != 16) out.insert("checksum_not_16_bytes");
      std::string cs = str(ref.getKey("/ProfileCS"));
      bool xclr = cs.size() == 4 && cs.compare(1, 3, "CLR") == 0;
      if (!ref.hasKey("/ProfileCS")) out.insert("missing_colour_space");
      else if (!ref.getKey("/ProfileCS").isString()) out.insert("colour_space_not_string");
      else if (cs != "GRAY" && cs != "RGB " && cs != "CMYK" && cs != "Lab " && !xclr) out.insert("colour_space_not_profile_space");
      QPDFObjectHandle urls = ref.getKey("/URLs");
      if (!urls.isArray()) out.insert("missing_urls");
      for (int i = 0; urls.isArray() && i < urls.getArrayNItems(); ++i) {
        QPDFObjectHandle u = urls.getArrayItem(i);
        if (!u.isDictionary()) out.insert("url_not_file_spec");
        else if (!u.hasKey("/FS")) out.insert("url_without_file_system");
        else if (!nameIs(u.getKey("/FS"), "/URL")) out.insert("url_not_file_spec");
      }
      QPDFObjectHandle ct = ref.getKey("/ColorantTable");
      if (xclr && !ct.isArray()) out.insert("missing_colorant_table");
      if (ct.isArray()) {
        std::set<std::string> names;
        for (int i = 0; i < ct.getArrayNItems(); ++i) names.insert(str(ct.getArrayItem(i)));
        out.insert("colorant_table_names");
        if (names.count("Cyan") && names.count("Magenta") && names.count("Yellow") && names.count("Black")) out.insert("colorant_table_cmyk");
        if (names.count("Black")) out.insert("colorant_table_black");
        if (!xclr) out.insert("missing_xclr_space");
      }
    }
  }
  return compare(listOf(out), a);
}

int evPageBoxRelation(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts) return 0;
  const Box& b = effBox(*s.pageFacts, param(a, "box"));
  const Box& o = effBox(*s.pageFacts, param(a, "other"));
  if (!b.valid || !o.valid) return 0;
  std::string v;
  if (sameBox(b, o)) v = "same";
  else if (encloses(b, o)) v = "encloses";
  else if (encloses(o, b)) {
    bool centered = std::fabs((b.x0 - o.x0) - (o.x1 - b.x1)) < 0.5 && std::fabs((b.y0 - o.y0) - (o.y1 - b.y1)) < 0.5;
    v = centered ? "centered" : "inside";
  } else {
    v = boxesIntersect(b, o) ? "overlaps" : "apart";
  }
  return compare(Value(v), a);
}

int evPageBoxesNested(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts) return 0;
  const PageFacts& p = *s.pageFacts;
  const Box &crop = effBox(p, "crop"), &bleed = effBox(p, "bleed");
  bool ok = encloses(p.media, crop) && encloses(crop, bleed) && (!p.trim.valid || encloses(bleed, p.trim)) &&
            (!p.art.valid || encloses(bleed, p.art));
  return compare(Value(ok), a);
}

int evPageContentStream(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts || !s.pageFacts->obj.isDictionary()) return 0;
  QPDFObjectHandle c = s.pageFacts->obj.getKey("/Contents");
  std::vector<QPDFObjectHandle> streams;
  if (c.isStream()) streams.push_back(c);
  for (int i = 0; c.isArray() && i < c.getArrayNItems(); ++i) {
    if (c.getArrayItem(i).isStream()) streams.push_back(c.getArrayItem(i));
  }
  if (streams.empty()) return 0;
  std::set<std::string> filters;
  bool none = false;
  for (auto& st : streams) {
    Strs f = filterNames(st.getDict());
    if (f.empty()) none = true;
    filters.insert(f.begin(), f.end());
  }
  const std::string& p = param(a, "property");
  Value v;
  if (p == "compression") {
    if (none) filters.insert("none");
    v = listOf(filters);
  } else if (p == "size_kb") {
    v = Value(contentLength(*s.pageFacts) / 1024.0);
  } else if (p == "ascii_encoded") {
    v = Value(filters.count("ASCII85Decode") || filters.count("A85") ? "ascii85" : filters.count("ASCIIHexDecode") || filters.count("AHx") ? "ascii_hex" : "no");
  }
  return compare(v, a);
}

int evPageHasMeasureViewport(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts) return 0;
  QPDFObjectHandle vp = s.pageFacts->obj.isDictionary() ? s.pageFacts->obj.getKey("/VP") : QPDFObjectHandle();
  bool has = false;
  for (int i = 0; vp.isArray() && i < vp.getArrayNItems(); ++i) has = has || vp.getArrayItem(i).getKey("/Measure").isDictionary();
  return compare(Value(has), a);
}

int evPageHasNoText(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.pageFacts) return 0;
  bool text = false;
  for (const TextEvent& te : ev.texts) text = text || te.page == s.pageFacts->page;
  return compare(Value(!s.pageFacts->empty && !text), a);
}

std::string roman(int n, bool upper) {
  static const std::pair<int, const char*> t[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"}, {50, "l"},
                                                  {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}};
  std::string out;
  for (auto& [v, s] : t) {
    while (n >= v) out += s, n -= v;
  }
  if (upper) std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return out;
}

int evPageLabel(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.pageFacts || !ev.root.isDictionary()) return 0;
  QPDFObjectHandle pl = ev.root.getKey("/PageLabels");
  if (!pl.isDictionary()) return 0;
  int idx = s.pageFacts->page - 1, best = -1;
  QPDFObjectHandle range;
  std::function<void(QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle n, int depth) {
    if (!n.isDictionary() || depth > 16) return;
    QPDFObjectHandle nums = n.getKey("/Nums");
    for (int i = 0; nums.isArray() && i + 1 < nums.getArrayNItems(); i += 2) {
      int start = static_cast<int>(numOf(nums.getArrayItem(i), 0));
      if (start <= idx && start > best) {
        best = start;
        range = nums.getArrayItem(i + 1);
      }
    }
    QPDFObjectHandle kids = n.getKey("/Kids");
    for (int i = 0; kids.isArray() && i < kids.getArrayNItems(); ++i) walk(kids.getArrayItem(i), depth + 1);
  };
  walk(pl, 0);
  const std::string& part = param(a, "part");
  std::string st = range.isDictionary() ? nameOf(range.getKey("/S")) : std::string();
  static const std::map<std::string, std::string> styles = {{"/D", "decimal"}, {"/R", "upper_roman"}, {"/r", "lower_roman"},
                                                            {"/A", "upper_letters"}, {"/a", "lower_letters"}};
  Value v;
  if (part == "style") {
    v = Value(styles.count(st) ? styles.at(st) : std::string("none"));
  } else if (part == "prefix") {
    if (range.isDictionary() && range.getKey("/P").isString()) v = Value(range.getKey("/P").getUTF8Value());
  } else if (best >= 0 && range.isDictionary()) {
    int n = idx - best + static_cast<int>(numOf(range.getKey("/St"), 1));
    std::string num;
    if (st == "/D") num = std::to_string(n);
    else if (st == "/R" || st == "/r") num = roman(n, st == "/R");
    else if ((st == "/A" || st == "/a") && n > 0) num = std::string(static_cast<size_t>((n - 1) / 26 + 1), static_cast<char>((st == "/A" ? 'A' : 'a') + (n - 1) % 26));
    v = Value(str(range.getKey("/P")) + num);
  }
  return compare(v, a);
}

int evPageMediaOriginNotZero(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts || !s.pageFacts->media.valid) return 0;
  const Box& m = s.pageFacts->media;
  return compare(Value(std::fabs(m.x0) > 0.01 || std::fabs(m.y0) > 0.01), a);
}

int evPageOrientation(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts) return 0;
  const Box& b = effBox(*s.pageFacts, param(a, "box"));
  if (!b.valid) return 0;
  return compare(Value(b.x1 - b.x0 > b.y1 - b.y0 ? "landscape" : "portrait"), a);
}

bool sameProfile(QPDFObjectHandle a1, QPDFObjectHandle a2) {
  return a1.isStream() && a2.isStream() && ((a1.isIndirect() && a2.isIndirect() && a1.getObjGen() == a2.getObjGen()) || streamBytes(a1) == streamBytes(a2));
}

int evPageOutputIntent(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.pageFacts || !s.pageFacts->obj.isDictionary()) return 0;
  std::string sub = oiSubtype(param(a, "subtype"));
  std::vector<QPDFObjectHandle> pois = outputIntents(s.pageFacts->obj, sub);
  std::string v = "none";
  if (!pois.empty()) {
    std::vector<QPDFObjectHandle> dois = ev.root.isDictionary() ? outputIntents(ev.root, sub) : std::vector<QPDFObjectHandle>();
    bool same = !dois.empty() && sameProfile(pois.front().getKey("/DestOutputProfile"), dois.front().getKey("/DestOutputProfile"));
    v = same ? "same_as_document" : "different";
  }
  return compare(Value(v), a);
}

int evPagePrivateData(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts || !s.pageFacts->obj.isDictionary()) return 0;
  QPDFObjectHandle pg = s.pageFacts->obj;
  std::set<std::string> out;
  QPDFObjectHandle pi = pg.getKey("/PieceInfo");
  if (pi.isDictionary()) out.insert("piece_info");
  if (pg.hasKey("/SeparationInfo")) out.insert("separation_info");
  if (pi.isDictionary() && pi.hasKey("/Illustrator")) out.insert("application_private");
  return compare(listOf(out), a);
}

int evPageProcessingStepsEntry(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts || !s.pageFacts->obj.isDictionary()) return 0;
  QPDFObjectHandle res = s.pageFacts->obj.getKey("/Resources");
  QPDFObjectHandle props = res.isDictionary() ? res.getKey("/Properties") : QPDFObjectHandle();
  bool has = s.pageFacts->hasProcStepsLayer;
  for (const std::string& k : props.isDictionary() ? props.getKeys() : std::set<std::string>()) {
    QPDFObjectHandle g = props.getKey(k);
    if (g.isDictionary() && g.getKey("/GTS_Metadata").isDictionary()) has = true;
  }
  std::string x = streamBytes(s.pageFacts->obj.getKey("/Metadata"));
  if (x.find("GTS_ProcSteps") != std::string::npos) has = true;
  return compare(Value(has), a);
}

int evPageTransparencyGroup(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts) return 0;
  const PageFacts& p = *s.pageFacts;
  QPDFObjectHandle g = p.obj.isDictionary() ? p.obj.getKey("/Group") : QPDFObjectHandle();
  std::set<std::string> out;
  if (p.hasTransGroup) {
    out.insert("present");
    if (g.getKey("/K").isBool() && g.getKey("/K").getBoolValue()) out.insert("knockout");
    if (g.hasKey("/CS")) out.insert("has_colour_space");
    out.insert(p.hasTransObj ? "contains_transparency" : "not_needed");
  }
  return compare(listOf(out), a);
}

int evPageTrimOrArtOnly(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts) return 0;
  return compare(Value(s.pageFacts->trim.valid != s.pageFacts->art.valid), a);
}

int evPageUnmappableText(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.pageFacts) return 0;
  double all = 0, bad = 0;
  for (const TextEvent& te : ev.texts) {
    if (te.page != s.pageFacts->page) continue;
    double n = static_cast<double>(std::max<size_t>(te.bytes.size(), 1));
    all += n;
    if (!te.mappedToUnicode) bad += n;
  }
  return all > 0 ? compare(Value(bad * 100.0 / all), a) : 0;
}

int evPageUsesTransparency(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts) return 0;
  return compare(Value(s.pageFacts->hasTransObj), a);
}

int evPrintBleed(const PfAtom& a, const Subject& s, const Events&, bool&) {
  if (!s.pageFacts || !s.pageFacts->trim.valid) return 0;
  const PageFacts& p = *s.pageFacts;
  const Box& b = p.bleed.valid ? p.bleed : p.media;
  double l = p.trim.x0 - b.x0, r = b.x1 - p.trim.x1, t = b.y1 - p.trim.y1, bo = p.trim.y0 - b.y0;
  const std::string& side = param(a, "side");
  double d = side == "left" ? l : side == "right" ? r : side == "top" ? t : side == "bottom" ? bo : std::min({l, r, t, bo});
  return compare(Value(d), a);
}

bool imageCmyOnly(const ImageEvent& e) {
  static const std::set<std::string> cmy = {"Cyan", "Magenta", "Yellow"};
  if (e.color.cls == "separation" || e.color.cls == "devicen") {
    for (const std::string& c : e.color.colorants) {
      if (!cmy.count(c)) return false;
    }
    return !e.color.colorants.empty();
  }
  bool fourComp = e.color.cls == "cmyk" || (e.color.cls == "icc" && e.color.declaredComps == 4);
  if (!fourComp || e.color.indexed || !e.obj.isStream()) return false;
  RawImage img = decodeImage(e.obj);
  if (!img.ok || img.comps != 4) return false;
  bool ink = false;
  for (size_t i = 0; i + 3 < img.samples.size(); i += 4) {
    if (img.samples[i + 3] != 0) return false;
    ink = ink || img.samples[i] || img.samples[i + 1] || img.samples[i + 2];
  }
  return ink;
}

bool cmyOnly(const Events& ev, int page) {
  bool any = false, all = true;
  for (const ImageEvent& e : ev.images) {
    if (e.page != page || e.mask) continue;
    any = true;
    all = all && imageCmyOnly(e);
    if (!all) break;
  }
  return any && all;
}

int evPrintImagesOnCmyOnly(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (param(a, "scope") == "page") return onPages(s, ev, [&](const PageFacts& p) { return compare(Value(cmyOnly(ev, p.page)), a); });
  bool any = false;
  for (const PageFacts& p : ev.pages) any = any || cmyOnly(ev, p.page);
  return compare(Value(any), a);
}

int evPrintPlates(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  static const std::set<std::string> proc = {"Cyan", "Magenta", "Yellow", "Black"};
  const std::string& kind = param(a, "kind");
  auto pick = [&](const std::set<std::string>& plates) {
    std::set<std::string> out;
    for (const std::string& pl : plates) {
      bool isProc = proc.count(pl) > 0;
      if (kind == "all" || (kind == "process") == isProc) out.insert(pl);
    }
    return listOf(out);
  };
  if (param(a, "scope") == "page") return onPages(s, ev, [&](const PageFacts& p) { return compare(pick(p.plates), a); });
  std::set<std::string> all;
  for (const PageFacts& p : ev.pages) all.insert(p.plates.begin(), p.plates.end());
  return compare(pick(all), a);
}

int evPrintRegistrationUsed(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.pageFacts) return 0;
  int page = s.pageFacts->page;
  auto reg = [](const ColorInfo& ci, const std::vector<double>& c) {
    if (ci.cls != "separation" && ci.cls != "devicen") return false;
    for (size_t i = 0; i < ci.colorants.size(); ++i) {
      double v = i < c.size() ? c[i] : 1.0;
      if ((ci.colorants[i] == "All" || ci.colorants[i] == "Registration") && v > 0.001) return true;
    }
    return false;
  };
  bool used = false;
  for (const PaintEvent& e : ev.paints) used = used || (e.page == page && !e.noPaint && reg(e.color, e.comps));
  for (const TextEvent& e : ev.texts) used = used || (e.page == page && e.renderMode != 3 && reg(e.color, e.comps));
  for (const ImageEvent& e : ev.images) used = used || (e.page == page && reg(e.color, e.mask ? e.comps : std::vector<double>{1.0}));
  return compare(Value(used), a);
}

bool trapNetInvalid(QPDFObjectHandle d) {
  bool lm = d.getKey("/LastModified").isString();
  bool vs = d.getKey("/Version").isArray() && d.getKey("/AnnotStates").isArray();
  if (!lm && !vs) return true;
  QPDFObjectHandle ff = d.getKey("/FontFauxing");
  if (d.hasKey("/FontFauxing")) {
    if (!ff.isArray()) return true;
    for (int i = 0; i < ff.getArrayNItems(); ++i) {
      if (!ff.getArrayItem(i).isDictionary()) return true;
    }
  }
  QPDFObjectHandle ap = d.getKey("/AP");
  if (!ap.isDictionary() || ap.getKey("/N").isNull()) return true;
  QPDFObjectHandle fl = d.getKey("/F");
  return ((fl.isInteger() ? fl.getIntValue() : 0) & 4) == 0;
}

int evPrintTrapNetwork(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::set<std::string> out;
  forPageAnnots(ev, [&](QPDFObjectHandle tr, const PageFacts&) {
    if (!nameIs(tr.getKey("/Subtype"), "/TrapNet")) return;
    out.insert("present");
    if (trapNetInvalid(tr)) out.insert("invalid");
    if (tr.getKey("/FontFauxing").isArray() && tr.getKey("/FontFauxing").getArrayNItems() > 0) out.insert("font_fauxed_list_not_empty");
    QPDFObjectHandle ap = tr.getKey("/AP");
    QPDFObjectHandle n = ap.isDictionary() ? ap.getKey("/N") : QPDFObjectHandle();
    std::string pcm = n.isStream() ? str(n.getDict().getKey("/PCM")) : std::string();
    if (pcm.empty()) out.insert("process_model_missing");
    else if (pcm != "DeviceCMYK") out.insert("process_model_not_cmyk");
  });
  return compare(listOf(out), a);
}

int evSealRecord(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle pi = ev.root.isDictionary() ? ev.root.getKey("/PieceInfo") : QPDFObjectHandle();
  QPDFObjectHandle e = pi.isDictionary() ? pi.getKey("/BentoPDFKura") : QPDFObjectHandle();
  QPDFObjectHandle pv = e.isDictionary() ? e.getKey("/Private") : QPDFObjectHandle();
  if (!pv.isDictionary()) return 0;
  const std::string& f = param(a, "field");
  std::string engine = str(pv.getKey("/Engine"));
  size_t sp = engine.rfind(' ');
  bool ok = pv.getKey("/Compliant").isBool() && pv.getKey("/Compliant").getBoolValue();
  Value v;
  if (f == "result") v = pv.getKey("/Compliant").isBool() ? Value(ok ? "pass" : "fail") : Value();
  else if (f == "result_text") v = pv.getKey("/Compliant").isBool() ? Value(ok ? "compliant" : "not compliant") : Value();
  else if (f == "profile_name") v = textOf(pv.getKey("/Level"));
  else if (f == "profile_author") v = engine.empty() ? Value() : Value(sp == std::string::npos ? engine : engine.substr(0, sp));
  else if (f == "profile_author_version") v = sp == std::string::npos ? Value() : Value(engine.substr(sp + 1));
  else if (f == "profile_digest") v = textOf(pv.getKey("/Digest"));
  else if (f == "profile_format") {
    QPDFObjectHandle n = pv.getKey("/ProfileFormatVersion");
    v = n.isInteger() ? Value(std::to_string(n.getIntValue())) : n.isNumber() ? Value(n.getNumericValue()) : textOf(n);
  }
  return compare(v, a);
}

int evSecurityEncryptionDictionary(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle enc = ev.trailer.isDictionary() ? ev.trailer.getKey("/Encrypt") : QPDFObjectHandle();
  if (!enc.isDictionary()) return 0;
  const std::string& e = param(a, "entry");
  auto allDirect = [](QPDFObjectHandle d, int kind) {
    for (const std::string& k : d.isDictionary() ? d.getKeys() : std::set<std::string>()) {
      QPDFObjectHandle v = d.getKey(k);
      bool match = kind == 0 ? v.isString() : kind == 1 ? v.isArray() : v.isDictionary();
      if (v.isIndirect() && match) return false;
    }
    return true;
  };
  auto cfDirect = [&](int kind) {
    QPDFObjectHandle cf = enc.getKey("/CF");
    bool ok = true;
    for (const std::string& k : cf.isDictionary() ? cf.getKeys() : std::set<std::string>()) ok = ok && allDirect(cf.getKey(k), kind);
    return ok;
  };
  Value v;
  if (e == "version") v = enc.getKey("/V").isNumber() ? Value(enc.getKey("/V").getNumericValue()) : Value();
  else if (e == "has_version") v = boolText(enc.hasKey("/V"));
  else if (e == "has_permissions") v = boolText(enc.hasKey("/P"));
  else if (e == "direct_strings") v = boolText(allDirect(enc, 0));
  else if (e == "direct_arrays") v = boolText(allDirect(enc, 1));
  else if (e == "direct_dictionaries") v = boolText(allDirect(enc, 2));
  else if (e == "crypt_filter_direct_strings") v = boolText(cfDirect(0));
  else if (e == "crypt_filter_direct_arrays") v = boolText(cfDirect(1));
  else if (e == "crypt_filter_direct_dictionaries") v = boolText(cfDirect(2));
  else if (e == "crypt_filter_decode_name") {
    const Events::ObjStats& st = objStats(ev);
    v = orAbsent(Strs(st.cryptNames.begin(), st.cryptNames.end()));
  }
  return compare(v, a);
}

int evSecurityPermission(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  static const std::map<std::string, int> bit = {{"print", 3}, {"modify", 4}, {"copy_text", 5}, {"annotate", 6},
                                                 {"fill_forms", 9}, {"accessibility", 10}, {"assemble", 11}, {"print_high", 12}};
  QPDFObjectHandle enc = ev.trailer.isDictionary() ? ev.trailer.getKey("/Encrypt") : QPDFObjectHandle();
  auto it = bit.find(param(a, "action"));
  bool denied = false;
  if (enc.isDictionary() && it != bit.end()) {
    long long p = enc.getKey("/P").isInteger() ? enc.getKey("/P").getIntValue() : -1;
    denied = !((p >> (it->second - 1)) & 1);
  }
  return compare(Value(denied), a);
}

std::vector<QPDFObjectHandle> signatureDicts(const Events& ev) {
  std::vector<QPDFObjectHandle> sigs;
  std::function<void(QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle f, int depth) {
    if (!f.isDictionary() || depth > 32) return;
    if (f.getKey("/V").isDictionary() && nameIs(f.getKey("/FT"), "/Sig")) sigs.push_back(f.getKey("/V"));
    QPDFObjectHandle k = f.getKey("/Kids");
    for (int i = 0; k.isArray() && i < k.getArrayNItems(); ++i) walk(k.getArrayItem(i), depth + 1);
  };
  QPDFObjectHandle acro = ev.root.isDictionary() ? ev.root.getKey("/AcroForm") : QPDFObjectHandle();
  QPDFObjectHandle fields = acro.isDictionary() ? acro.getKey("/Fields") : QPDFObjectHandle();
  for (int i = 0; fields.isArray() && i < fields.getArrayNItems(); ++i) walk(fields.getArrayItem(i), 0);
  return sigs;
}

int evSignatureDictionary(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string& e = param(a, "entry");
  std::vector<Value> vs;
  for (QPDFObjectHandle sg : signatureDicts(ev)) {
    QPDFObjectHandle r = sg.getKey("/Reference");
    QPDFObjectHandle ref = r.isArray() && r.getArrayNItems() ? r.getArrayItem(0) : QPDFObjectHandle();
    QPDFObjectHandle tp = ref.isDictionary() ? ref.getKey("/TransformParams") : QPDFObjectHandle();
    QPDFObjectHandle br = sg.getKey("/ByteRange");
    Value v;
    if (e == "sub_filter") v = textOf(sg.getKey("/SubFilter"));
    else if (e == "byte_range") v = br.isArray() ? Value(numbersText(br)) : Value();
    else if (e == "covers_whole_file") {
      bool ok = false;
      if (br.isArray() && br.getArrayNItems() == 4) {
        double a0 = numOf(br.getArrayItem(0), -1), a1 = numOf(br.getArrayItem(1), 0), a2 = numOf(br.getArrayItem(2), 0), a3 = numOf(br.getArrayItem(3), 0);
        ok = a0 == 0 && a2 > a1 && std::fabs(a2 + a3 - ev.filesize) < 0.5;
      }
      v = Value(ok ? "yes" : "no");
    } else if (e == "has_version") v = boolText(sg.hasKey("/V"));
    else if (e == "digest_location") v = ref.isDictionary() && ref.getKey("/DigestLocation").isArray() ? Value(numbersText(ref.getKey("/DigestLocation"))) : Value();
    else if (e == "digest_method") v = ref.isDictionary() ? textOf(ref.getKey("/DigestMethod")) : Value();
    else if (e == "digest_value") v = ref.isDictionary() ? textOf(ref.getKey("/DigestValue")) : Value();
    else if (e == "transform_method") v = ref.isDictionary() ? textOf(ref.getKey("/TransformMethod")) : Value();
    else if (e == "transform_permissions") v = tp.isDictionary() && tp.getKey("/P").isNumber() ? Value(tp.getKey("/P").getNumericValue()) : Value();
    vs.push_back(v);
  }
  return each(vs, a);
}

int evSignatureFlags(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle acro = ev.root.isDictionary() ? ev.root.getKey("/AcroForm") : QPDFObjectHandle();
  QPDFObjectHandle sf = acro.isDictionary() ? acro.getKey("/SigFlags") : QPDFObjectHandle();
  std::string v = "absent";
  if (sf.isInteger()) {
    long long f = sf.getIntValue() & 3;
    v = f == 0 ? "none" : f == 1 ? "signatures_exist" : f == 2 ? "append_only" : "both";
  }
  return compare(Value(v), a);
}

int evSignaturePermissions(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle perms = ev.root.isDictionary() ? ev.root.getKey("/Perms") : QPDFObjectHandle();
  std::set<std::string> out;
  for (const std::string& k : perms.isDictionary() ? perms.getKeys() : std::set<std::string>()) {
    out.insert(k == "/DocMDP" ? "docmdp" : k == "/UR" ? "ur" : k == "/UR3" ? "ur3" : "other");
  }
  return compare(listOf(out), a);
}

int evStructureElement(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle tr = ev.root.isDictionary() ? ev.root.getKey("/StructTreeRoot") : QPDFObjectHandle();
  if (!tr.isDictionary()) return 0;
  const std::string& p = param(a, "property");
  std::vector<Value> vs;
  std::set<QPDFObjGen> seen;
  auto isElem = [](QPDFObjectHandle k) { return k.isDictionary() && (nameIs(k.getKey("/Type"), "/StructElem") || (k.hasKey("/S") && !k.hasKey("/MCID") && !nameIs(k.getKey("/Type"), "/MCR") && !nameIs(k.getKey("/Type"), "/OBJR"))); };
  std::function<void(QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle n, int depth) {
    if (depth > 128 || vs.size() > 200000) return;
    std::vector<QPDFObjectHandle> kids;
    QPDFObjectHandle k = n.getKey("/K");
    if (k.isArray()) {
      for (int i = 0; i < k.getArrayNItems(); ++i) kids.push_back(k.getArrayItem(i));
    } else if (!k.isNull()) {
      kids.push_back(k);
    }
    for (QPDFObjectHandle c : kids) {
      if (!isElem(c) || (c.isIndirect() && !seen.insert(c.getObjGen()).second)) continue;
      Value v;
      if (p == "type") v = textOf(c.getKey("/S"));
      else if (p == "child_count") {
        int cnt = 0;
        QPDFObjectHandle ck = c.getKey("/K");
        for (int i = 0; ck.isArray() && i < ck.getArrayNItems(); ++i) cnt += isElem(ck.getArrayItem(i));
        if (!ck.isArray()) cnt = isElem(ck);
        v = Value(cnt);
      } else if (p == "language") v = c.getKey("/Lang").isString() ? Value(c.getKey("/Lang").getUTF8Value()) : Value();
      else if (p == "has_language") v = boolText(c.hasKey("/Lang"));
      else if (p == "language_valid") v = c.hasKey("/Lang") ? boolText(str(c.getKey("/Lang")).empty() || validLangTag(str(c.getKey("/Lang")))) : Value();
      else if (p == "language_is_text_string") v = c.hasKey("/Lang") ? boolText(c.getKey("/Lang").isString()) : Value();
      vs.push_back(v);
      walk(c, depth + 1);
    }
  };
  walk(tr, 0);
  return each(vs, a);
}

int evSyntaxLimit(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  const std::string& m = param(a, "measure");
  const Events::ObjStats& st = objStats(ev);
  Value v;
  if (m == "largest_integer") v = st.anyPosInt ? Value(st.posInt) : Value();
  else if (m == "smallest_integer") v = st.anyNegInt ? Value(st.negInt) : Value();
  else if (m == "largest_real") v = st.anyPosReal ? Value(st.posReal) : Value();
  else if (m == "smallest_real") v = st.anyNegReal ? Value(st.negReal) : Value();
  else if (m == "real_closest_to_zero_positive") v = st.anyPosReal ? Value(st.posRealNearZero) : Value();
  else if (m == "real_closest_to_zero_negative") v = st.anyNegReal ? Value(st.negRealNearZero) : Value();
  else if (m == "longest_string") v = Value(static_cast<double>(st.maxStr));
  else if (m == "longest_content_string") v = Value(static_cast<double>(ev.maxContentString));
  else if (m == "longest_name") v = Value(st.maxName);
  else if (m == "largest_array") v = Value(st.maxArr);
  else if (m == "largest_dictionary") v = Value(st.maxDict);
  else if (m == "deepest_graphics_state_nesting") {
    int d = 0;
    for (const PaintEvent& e : ev.paints) d = std::max(d, e.x.qDepth);
    for (const TextEvent& e : ev.texts) d = std::max(d, e.x.qDepth);
    for (const ImageEvent& e : ev.images) d = std::max(d, e.x.qDepth);
    v = Value(d);
  } else {
    supported = false;
    return 0;
  }
  return compare(v, a);
}

int evSyntaxObjectCount(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDF* q = ev.root.isInitialized() ? ev.root.getOwningQPDF() : nullptr;
  return compare(Value(q ? static_cast<double>(q->getObjectCount()) : 0.0), a);
}

int syntaxIssueList(const PfAtom& a, const Events& ev, bool& supported, std::initializer_list<std::pair<const char*, const char*>> map) {
  if (!ev.syntaxScanned) {
    supported = false;
    return 0;
  }
  std::set<std::string> out;
  for (auto& m : map) {
    if (ev.syntaxIssues.count(m.first)) out.insert(m.second);
  }
  return compare(listOf(out), a);
}

int evSyntaxObjectLayout(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  return syntaxIssueList(a, ev, supported,
                         {{"objNotPrecededByEol", "no_eol_before_obj"}, {"objNotFollowedByEol", "no_eol_after_obj"},
                          {"endobjNotPrecededByEol", "no_eol_before_endobj"}, {"endobjNotFollowedByEol", "no_eol_after_endobj"},
                          {"objNumSep", "bad_object_number"}, {"objKeySep", "bad_obj_keyword_spacing"}});
}

int evSyntaxXrefLayout(const PfAtom& a, const Subject&, const Events& ev, bool& supported) {
  return syntaxIssueList(a, ev, supported, {{"xrefHeader", "bad_subsection_header"}, {"xrefEol", "subsection_not_eol_separated"}});
}

int evSyntaxPostscript(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::set<std::string> out;
  if (ev.contentOps.count("PS")) out.insert("operator");
  for (QPDFObjectHandle o : allObjects(ev)) {
    if (!o.isStream()) continue;
    QPDFObjectHandle d = o.getDict();
    if (nameIs(d.getKey("/Subtype"), "/PS")) out.insert("xobject");
    if (nameIs(d.getKey("/Subtype"), "/Form") && (nameIs(d.getKey("/Subtype2"), "/PS") || d.hasKey("/PS"))) out.insert("form");
  }
  return compare(listOf(out), a);
}

struct PartNode {
  QPDFObjectHandle d, parent;
  int depth = 0;
  int start = -1, end = -1;
  bool leaf = false;
};

std::vector<PartNode> partNodes(const Events& ev) {
  std::vector<PartNode> out;
  QPDFObjectHandle dpr = ev.root.isDictionary() ? ev.root.getKey("/DPartRoot") : QPDFObjectHandle();
  QPDFObjectHandle top = dpr.isDictionary() ? dpr.getKey("/DPartRootNode") : QPDFObjectHandle();
  std::map<QPDFObjGen, int> idx = pageIndex(ev);
  std::set<QPDFObjGen> seen;
  auto pageOf = [&](QPDFObjectHandle p) {
    auto it = p.isIndirect() ? idx.find(p.getObjGen()) : idx.end();
    return it == idx.end() ? -1 : it->second;
  };
  std::function<void(QPDFObjectHandle, QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle n, QPDFObjectHandle parent, int depth) {
    if (!n.isDictionary() || depth > 64 || out.size() > 200000) return;
    if (n.isIndirect() && !seen.insert(n.getObjGen()).second) return;
    PartNode pn;
    pn.d = n;
    pn.parent = parent;
    pn.depth = depth;
    pn.start = pageOf(n.getKey("/Start"));
    pn.end = pageOf(n.getKey("/End"));
    pn.leaf = !n.getKey("/DParts").isArray();
    out.push_back(pn);
    QPDFObjectHandle kids = n.getKey("/DParts");
    for (int i = 0; kids.isArray() && i < kids.getArrayNItems(); ++i) {
      QPDFObjectHandle grp = kids.getArrayItem(i);
      for (int j = 0; grp.isArray() && j < grp.getArrayNItems(); ++j) walk(grp.getArrayItem(j), n, depth + 1);
    }
  };
  walk(top, dpr, 0);
  return out;
}

int partDepth(const std::vector<PartNode>& nodes) {
  int d = 0;
  for (const PartNode& n : nodes) d = std::max(d, n.depth);
  return d;
}

int evVariableDataDocumentParts(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  std::set<std::string> out;
  QPDFObjectHandle dpr = ev.root.isDictionary() ? ev.root.getKey("/DPartRoot") : QPDFObjectHandle();
  if (!dpr.isDictionary()) {
    if (!ev.root.isDictionary() || !ev.root.hasKey("/DPartRoot")) out.insert("missing_root");
    else out.insert("root_not_valid");
    return compare(listOf(out), a);
  }
  if (dpr.hasKey("/Type") && !nameIs(dpr.getKey("/Type"), "/DPartRoot")) out.insert("root_not_valid");
  if (!dpr.hasKey("/Type")) out.insert("root_missing_type");
  QPDFObjectHandle nnl = dpr.getKey("/NodeNameList");
  if (!nnl.isArray()) out.insert("root_missing_node_names");
  if (!dpr.hasKey("/RecordLevel")) out.insert("root_missing_record_level");
  if (!dpr.getKey("/DPartRootNode").isDictionary()) out.insert("root_not_referenced");
  for (int i = 0; nnl.isArray() && i < nnl.getArrayNItems(); ++i) {
    if (!nnl.getArrayItem(i).isName()) out.insert("node_names_not_names");
  }
  std::vector<PartNode> nodes = partNodes(ev);
  if (nnl.isArray() && !nodes.empty() && nnl.getArrayNItems() != partDepth(nodes)) out.insert("node_names_depth_mismatch");
  std::map<QPDFObjGen, std::set<QPDFObjGen>> parents;
  std::map<QPDFObjGen, const PartNode*> byObj;
  for (const PartNode& n : nodes) {
    if (n.d.isIndirect()) byObj[n.d.getObjGen()] = &n;
    if (n.depth == 0) continue;
    QPDFObjectHandle p = n.d.getKey("/Parent");
    if (!n.d.hasKey("/Type")) out.insert("part_missing_type");
    if (!p.isDictionary()) {
      out.insert("part_missing_parent");
      out.insert("part_no_parent");
    } else if (n.parent.isIndirect() && p.getObjGen() != n.parent.getObjGen()) {
      out.insert("part_parent_invalid");
    }
    if (n.d.isIndirect() && n.parent.isIndirect()) parents[n.d.getObjGen()].insert(n.parent.getObjGen());
    if (n.leaf) {
      if (!n.d.getKey("/Start").isDictionary()) out.insert(n.d.hasKey("/DParts") ? "part_missing_children" : "part_missing_start");
      if (!n.d.getKey("/End").isDictionary()) out.insert("part_missing_end");
      if (!n.d.getKey("/DPM").isDictionary()) out.insert("part_missing_metadata");
      if (n.start >= 0 && n.end >= 0 && n.end == n.start) out.insert("start_end_same_page_error");
    }
    QPDFObjectHandle dpm = n.d.getKey("/DPM");
    if (dpm.isDictionary()) {
      static const std::regex nm("^[A-Za-z0-9._:\\-]+$");
      for (const std::string& k : dpm.getKeys()) {
        if (!std::regex_match(k.substr(1), nm)) out.insert("metadata_key_not_nmtoken");
      }
      QPDFObjectHandle m = dpm.getKey("/GTS_Managed"), su = dpm.getKey("/GTS_Suspect");
      if (dpm.hasKey("/GTS_Managed")) out.insert("metadata_has_managed");
      if (dpm.hasKey("/GTS_Suspect")) out.insert("metadata_has_suspect");
      if (dpm.hasKey("/GTS_Managed") && !m.isDictionary()) out.insert("metadata_managed_not_dict");
      if (dpm.hasKey("/GTS_Suspect") && !su.isDictionary()) out.insert("metadata_suspect_not_dict");
      for (const std::string& k : m.isDictionary() && su.isDictionary() ? m.getKeys() : std::set<std::string>()) {
        if (su.hasKey(k)) out.insert("metadata_managed_suspect_overlap");
      }
      QPDFObjGen g = dpm.isIndirect() ? dpm.getObjGen() : n.d.getObjGen();
      std::string tag = "object " + std::to_string(g.getObj()) + " " + std::to_string(g.getGen());
      for (const std::string& w : ev.qpdfWarningText) {
        if (w.find("duplicated key") != std::string::npos && w.find(tag) != std::string::npos) out.insert("metadata_key_not_unique");
      }
    }
  }
  for (auto& kv : parents) {
    if (kv.second.size() > 1) out.insert("part_several_parents");
  }
  int next = 0;
  bool order = true;
  for (size_t i = 0; i < ev.pages.size(); ++i) {
    QPDFObjectHandle dp = ev.pages[i].obj.isDictionary() ? ev.pages[i].obj.getKey("/DPart") : QPDFObjectHandle();
    if (!dp.isDictionary()) {
      out.insert("page_missing_dpart");
      continue;
    }
    if (dp.getKey("/Start").isDictionary()) out.insert("page_with_range_part");
    auto it = dp.isIndirect() ? byObj.find(dp.getObjGen()) : byObj.end();
    if (it != byObj.end()) {
      const PartNode& pn = *it->second;
      int e = pn.end >= 0 ? pn.end : pn.start;
      if (pn.start < 0 || static_cast<int>(i) < pn.start || static_cast<int>(i) > e) out.insert("page_outside_parent_range");
    }
  }
  for (const PartNode& n : nodes) {
    if (!n.leaf || n.start < 0) continue;
    int e = n.end >= 0 ? n.end : n.start;
    if (n.start != next || e < n.start) order = false;
    next = e + 1;
  }
  if (next != static_cast<int>(ev.pages.size())) order = false;
  if (!order) out.insert("page_order_differs");
  QPDF* q = ev.root.getOwningQPDF();
  if (q && !nodes.empty()) {
    std::map<QPDFObjGen, QPDFXRefEntry> xref = q->getXRefTable();
    int in = 0, total = 0;
    std::set<int> streams;
    for (const PartNode& n : nodes) {
      if (!n.d.isIndirect()) continue;
      ++total;
      auto it = xref.find(n.d.getObjGen());
      if (it != xref.end() && it->second.getType() == 2) {
        ++in;
        streams.insert(it->second.getObjStreamNumber());
      }
    }
    if (in == 0) out.insert("not_in_object_streams");
    else if (in == total) out.insert("all_in_object_streams");
    if ((in > 0 && in < total) || streams.size() > 1) out.insert("spread_over_object_streams");
  }
  return compare(listOf(out), a);
}

void dpmValues(QPDFObjectHandle o, const std::string& prefix, const std::string& want, Strs& out, int depth) {
  if (depth > 16) return;
  if (o.isDictionary()) {
    for (const std::string& k : o.getKeys()) dpmValues(o.getKey(k), prefix.empty() ? k.substr(1) : prefix + "/" + k.substr(1), want, out, depth + 1);
    return;
  }
  if (o.isArray()) {
    for (int i = 0; i < o.getArrayNItems(); ++i) dpmValues(o.getArrayItem(i), prefix, want, out, depth + 1);
    return;
  }
  if (prefix != want) return;
  if (o.isBool()) out.push_back(o.getBoolValue() ? "true" : "false");
  else if (o.isInteger()) out.push_back(std::to_string(o.getIntValue()));
  else if (o.isNumber()) out.push_back(fmtFixed(o.getNumericValue(), 6));
  else out.push_back(str(o));
}

int evVariableDataPartValue(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  QPDFObjectHandle dpr = ev.root.isDictionary() ? ev.root.getKey("/DPartRoot") : QPDFObjectHandle();
  const std::string& v = param(a, "part_value");
  std::string x = xmpOf(ev.root);
  std::string mod = xmpValue(x, "pdfvtid:GTS_PDFVTModDate");
  std::vector<PartNode> nodes = dpr.isDictionary() ? partNodes(ev) : std::vector<PartNode>();
  auto dpms = [&](const std::function<void(QPDFObjectHandle)>& fn) {
    for (const PartNode& n : nodes) {
      if (n.d.getKey("/DPM").isDictionary()) fn(n.d.getKey("/DPM"));
    }
  };
  if (v == "depth") return dpr.isDictionary() ? compare(Value(partDepth(nodes)), a) : 0;
  if (v == "node_names") return dpr.isDictionary() && dpr.getKey("/NodeNameList").isArray() ? compare(Value(dpr.getKey("/NodeNameList").getArrayNItems()), a) : 0;
  if (v == "record_level") return dpr.isDictionary() && dpr.getKey("/RecordLevel").isNumber() ? compare(Value(dpr.getKey("/RecordLevel").getNumericValue()), a) : 0;
  if (v == "root_type") return dpr.isDictionary() ? compare(textOf(dpr.getKey("/Type")), a) : 0;
  if (v == "version") return compare(xmpValue(x, "pdfvtid:GTS_PDFVTVersion").empty() ? Value() : Value(xmpValue(x, "pdfvtid:GTS_PDFVTVersion")), a);
  if (v == "has_version") return compare(boolText(x.find("pdfvtid:GTS_PDFVTVersion") != std::string::npos), a);
  if (v == "last_modified") return compare(mod.empty() ? Value() : Value(mod), a);
  if (v == "has_last_modified") return compare(boolText(x.find("pdfvtid:GTS_PDFVTModDate") != std::string::npos), a);
  if (v == "last_modified_is_date") {
    static const std::regex date("[0-9]{4}(-[0-9]{2}(-[0-9]{2}(T[0-9]{2}:[0-9]{2}(:[0-9]{2}(\\.[0-9]+)?)?(Z|[+-][0-9]{2}:[0-9]{2})?)?)?)?");
    return mod.empty() ? 0 : compare(boolText(std::regex_match(mod, date)), a);
  }
  if (v == "last_modified_consistent") return mod.empty() ? 0 : compare(boolText(mod == xmpValue(x, "xmp:ModifyDate")), a);
  if (v == "part_type") {
    Strs out;
    for (const PartNode& n : nodes) {
      if (n.d.hasKey("/Type")) out.push_back(str(n.d.getKey("/Type")));
    }
    return compare(orAbsent(out), a);
  }
  if (v == "pages_referencing") {
    if (nodes.empty()) return 0;
    std::vector<Value> vs;
    for (size_t i = 0; i < ev.pages.size(); ++i) {
      int pg = static_cast<int>(i), n = 0;
      for (const PartNode& p : nodes) n += p.start >= 0 && p.start <= pg && pg <= (p.end >= 0 ? p.end : p.start);
      vs.push_back(Value(n));
    }
    return each(vs, a);
  }
  Strs out;
  if (v == "metadata_value") {
    const std::string& key = param(a, "key");
    if (key.empty()) return 0;
    dpms([&](QPDFObjectHandle d) { dpmValues(d, "", key, out, 0); });
    return compare(orAbsent(out), a);
  }
  bool any = false;
  if (v == "environment_present" || v == "environment_is_text") {
    if (x.find("GTS_PDFVTEnv") != std::string::npos) any = true;
    bool text = any;
    dpms([&](QPDFObjectHandle d) {
      if (d.hasKey("/GTS_Env")) any = true, text = text || d.getKey("/GTS_Env").isString();
    });
    if (v == "environment_is_text") return any ? compare(boolText(text), a) : 0;
    return compare(boolText(any), a);
  }
  if (v == "scope_present") {
    dpms([&](QPDFObjectHandle d) { any = any || d.hasKey("/GTS_Scope"); });
    return compare(boolText(any), a);
  }
  if (v == "scope") {
    dpms([&](QPDFObjectHandle d) {
      if (d.hasKey("/GTS_Scope")) out.push_back(str(d.getKey("/GTS_Scope")));
    });
    return compare(orAbsent(out), a);
  }
  return 0;
}

bool referenceCircular(const Events& ev) {
  if (ev.inputPath.empty()) return false;
  std::error_code ec;
  std::set<std::string> visiting;
  std::function<bool(const std::string&, int)> visit = [&](const std::string& path, int depth) -> bool {
    if (depth > 16) return false;
    std::string canon = std::filesystem::weakly_canonical(path, ec).string();
    if (visiting.count(canon)) return true;
    visiting.insert(canon);
    bool hit = false;
    try {
      QPDF q;
      q.setSuppressWarnings(true);
      q.processFile(path.c_str());
      std::filesystem::path dir = std::filesystem::path(path).parent_path();
      for (QPDFObjectHandle o : q.getAllObjects()) {
        if (hit || !o.isStream() || !nameIs(o.getDict().getKey("/Subtype"), "/Form")) continue;
        QPDFObjectHandle ref = o.getDict().getKey("/Ref");
        if (!ref.isDictionary()) continue;
        std::string url = specUrl(ref.getKey("/F"));
        if (url.rfind("file://", 0) == 0) url = url.substr(7);
        if (url.empty()) continue;
        std::filesystem::path p(url);
        if (p.is_relative()) p = dir / p;
        if (std::filesystem::is_regular_file(p, ec) && visit(p.string(), depth + 1)) hit = true;
      }
    } catch (...) {
    }
    visiting.erase(canon);
    return hit;
  };
  return visit(ev.inputPath, 0);
}

void referenceProps(QPDFObjectHandle form, std::set<std::string>& out) {
  QPDFObjectHandle d = form.isStream() ? form.getDict() : form;
  QPDFObjectHandle ref = d.isDictionary() ? d.getKey("/Ref") : QPDFObjectHandle();
  if (!ref.isDictionary()) return;
  if (ref.hasKey("/ID")) out.insert("has_id");
  std::string x = streamBytes(d.getKey("/Metadata"));
  if (d.getKey("/Metadata").isStream()) out.insert("has_metadata");
  if (x.find("stRef:documentID") != std::string::npos) out.insert("metadata_document_id");
  static const std::regex uuid("^uuid:[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
  if (std::regex_match(xmpValue(x, "stRef:documentID"), uuid)) out.insert("metadata_document_id_uuid");
  if (x.find("stRef:versionID") != std::string::npos) out.insert("metadata_version_id");
  if (x.find("stRef:renditionClass") != std::string::npos) out.insert("metadata_rendition_class");
  if (x.find("xmpMM:RenditionOf") != std::string::npos) out.insert("metadata_rendition_of");
}

int evXobjectReference(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  const GsExtra* x = s.paint ? &s.paint->x : s.text ? &s.text->x : s.image ? &s.image->x : nullptr;
  std::set<std::string> out;
  bool anyRef = false;
  if (x) {
    referenceProps(x->formDict, out);
    anyRef = x->formDict.isDictionary() && x->formDict.getKey("/Ref").isDictionary();
  } else {
    for (QPDFObjectHandle o : allObjects(ev)) {
      if (o.isStream() && nameIs(o.getDict().getKey("/Subtype"), "/Form") && o.getDict().getKey("/Ref").isDictionary()) {
        referenceProps(o, out);
        anyRef = true;
      }
    }
  }
  if (asks(a, "circular")) {
    if (ev.inputPath.empty()) {
      supported = false;
      return 0;
    }
    if (anyRef && referenceCircular(ev)) out.insert("circular");
  }
  return compare(listOf(out), a);
}
}

void registerObjectChecks(CheckTable& t) {
  t.add("action.broken_destination", evActionBrokenDestination);
  t.add("action.destination", evActionDestination);
  t.add("action.present", evActionPresent);
  t.add("action.type", evActionType);
  t.add("action.uri", evActionUri);
  t.add("annotation.appearance", evAnnotationAppearance);
  t.add("annotation.entry", evAnnotationEntry);
  t.add("annotation.flags", evAnnotationFlags);
  t.add("annotation.interactive", evAnnotationInteractive);
  t.add("annotation.minimum_pdf_version", evAnnotationMinimumPdfVersion);
  t.add("annotation.outside_crop", evAnnotationOutsideCrop);
  t.add("attachment.count", evAttachmentCount);
  t.add("attachment.is_pdf", evAttachmentIsPdf);
  t.add("attachment.problem", evAttachmentProblem);
  t.add("attachment.property", evAttachmentProperty);
  t.add("barcode.dimension", evBarcodeDimension);
  t.add("barcode.found", evBarcodeFound);
  t.add("barcode.property", evBarcodeProperty);
  t.add("barcode.quiet_zone_sufficient", evBarcodeQuietZoneSufficient);
  t.add("conformance.claim", evConformanceClaim);
  t.add("document.binding", evDocumentBinding);
  t.add("document.colour_spaces", evDocumentColourSpaces);
  t.add("document.content_stream_size", evDocumentContentStreamSize);
  t.add("document.created_by_print_driver", evDocumentCreatedByPrintDriver);
  t.add("document.damaged", evDocumentDamaged);
  t.add("document.extension_level", evDocumentExtensionLevel);
  t.add("document.extension_version_mismatch", evDocumentExtensionVersionMismatch);
  t.add("document.form_nesting", evDocumentFormNesting);
  t.add("document.has", evDocumentHas);
  t.add("document.header_conforms", evDocumentHeaderConforms);
  t.add("document.id", evDocumentId);
  t.add("document.language", evDocumentLanguage);
  t.add("document.largest_stream", evDocumentLargestStream);
  t.add("document.linearized", evDocumentLinearized);
  t.add("document.mark_info_extra_keys", evDocumentMarkInfoExtraKeys);
  t.add("document.marked", evDocumentMarked);
  t.add("document.needs_rendering", evDocumentNeedsRendering);
  t.add("document.open_view", evDocumentOpenView);
  t.add("document.pages_uniform", evDocumentPagesUniform);
  t.add("document.portfolio", evDocumentPortfolio);
  t.add("document.stream_compression", evDocumentStreamCompression);
  t.add("document.stream_layout", evDocumentStreamLayout);
  t.add("document.trailer", evDocumentTrailer);
  t.add("document.uses_object_streams", evDocumentUsesObjectStreams);
  t.add("document.version_conflict", evDocumentVersionConflict);
  t.add("einvoice.metadata", evEinvoiceMetadata);
  t.add("einvoice.present", evEinvoicePresent);
  t.add("font.encoding", evFontEncoding);
  t.add("font.flags", evFontFlags);
  t.add("font.has_to_unicode", evFontHasToUnicode);
  t.add("font.metric", evFontMetric);
  t.add("font.problem", evFontProblem);
  t.add("font.type", evFontType);
  t.add("form.field_flags", evFormFieldFlags);
  t.add("form.field_name", evFormFieldName);
  t.add("form.field_type", evFormFieldType);
  t.add("form.field_value", evFormFieldValue);
  t.add("form.xfa", evFormXfa);
  t.add("icc.header", evIccHeader);
  t.add("icc.matches_output_intent", evIccMatchesOutputIntent);
  t.add("icc.max_ink", evIccMaxInk);
  t.add("icc.tag", evIccTag);
  t.add("image.abbreviated_filter_names", evImageAbbreviatedFilterNames);
  t.add("image.alternates", evImageAlternates);
  t.add("image.jbig2", evImageJbig2);
  t.add("image.jpeg2000", evImageJpeg2000);
  t.add("image.opi", evImageOpi);
  t.add("image.property", evImageProperty);
  t.add("layer.configuration", evLayerConfiguration);
  t.add("layer.configuration_name", evLayerConfigurationName);
  t.add("layer.property", evLayerProperty);
  t.add("layer.visibility_policy", evLayerVisibilityPolicy);
  t.add("metadata.info", evMetadataInfo);
  t.add("metadata.info_keys", evMetadataInfoKeys);
  t.add("metadata.info_matches_xmp", evMetadataInfoMatchesXmp);
  t.add("metadata.info_problem", evMetadataInfoProblem);
  t.add("metadata.object_has_xmp", evMetadataObjectHasXmp);
  t.add("metadata.xmp_document_id_is_uuid", evMetadataXmpDocumentIdIsUuid);
  t.add("metadata.xmp_identification", evMetadataXmpIdentification);
  t.add("metadata.xmp_namespaces", evMetadataXmpNamespaces);
  t.add("metadata.xmp_prefix", evMetadataXmpPrefix);
  t.add("metadata.xmp_problem", evMetadataXmpProblem);
  t.add("metadata.xmp_property", evMetadataXmpProperty);
  t.add("metadata.xmp_size", evMetadataXmpSize);
  t.add("output_intent.entry", evOutputIntentEntry);
  t.add("output_intent.profile_reference", evOutputIntentProfileReference);
  t.add("page.box_relation", evPageBoxRelation);
  t.add("page.boxes_nested", evPageBoxesNested);
  t.add("page.content_stream", evPageContentStream);
  t.add("page.has_measure_viewport", evPageHasMeasureViewport);
  t.add("page.has_no_text", evPageHasNoText);
  t.add("page.label", evPageLabel);
  t.add("page.media_origin_not_zero", evPageMediaOriginNotZero);
  t.add("page.orientation", evPageOrientation);
  t.add("page.output_intent", evPageOutputIntent);
  t.add("page.private_data", evPagePrivateData);
  t.add("page.processing_steps_entry", evPageProcessingStepsEntry);
  t.add("page.transparency_group", evPageTransparencyGroup);
  t.add("page.trim_or_art_only", evPageTrimOrArtOnly);
  t.add("page.unmappable_text", evPageUnmappableText);
  t.add("page.uses_transparency", evPageUsesTransparency);
  t.add("print.bleed", evPrintBleed);
  t.add("print.images_on_cmy_only", evPrintImagesOnCmyOnly);
  t.add("print.plates", evPrintPlates);
  t.add("print.registration_used", evPrintRegistrationUsed);
  t.add("print.trap_network", evPrintTrapNetwork);
  t.add("seal.record", evSealRecord);
  t.add("security.encryption_dictionary", evSecurityEncryptionDictionary);
  t.add("security.permission", evSecurityPermission);
  t.add("signature.dictionary", evSignatureDictionary);
  t.add("signature.flags", evSignatureFlags);
  t.add("signature.permissions", evSignaturePermissions);
  t.add("structure.element", evStructureElement);
  t.add("syntax.limit", evSyntaxLimit);
  t.add("syntax.object_count", evSyntaxObjectCount);
  t.add("syntax.object_layout", evSyntaxObjectLayout);
  t.add("syntax.postscript", evSyntaxPostscript);
  t.add("syntax.xref_layout", evSyntaxXrefLayout);
  t.add("variable_data.document_parts", evVariableDataDocumentParts);
  t.add("variable_data.part_value", evVariableDataPartValue);
  t.add("xobject.reference", evXobjectReference);
}
}
